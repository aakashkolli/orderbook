# Python Bindings and Adapter Layer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a pybind11 `orderbook` Python module exposing step-by-step replay and implement `CsvEventSource` / `ParquetEventSource` adapters behind a shared `IEventSource` interface.

**Architecture:** `IEventSource` (virtual `next(Event&)`) decouples event production from book dispatch. `ReplaySession` owns one source and one `OrderBook`, driving the `step()` loop. Python bindings wrap `ReplaySession` inside a `PyReplaySession` struct that additionally holds the Python sink callable to manage its lifetime safely.

**Tech Stack:** C++20, CMake FetchContent, pybind11 ≥ 2.11, Apache Arrow C++ ≥ 10.0 (optional), pytest ≥ 7, existing GTest suite

---

## File Map

| File | Action | Responsibility |
|---|---|---|
| `include/orderbook/event_source.hpp` | Create | `IEventSource`, `Side` enum |
| `include/feed_handler.h` | Modify | Add `cursor_` member + `step(Event&)` declaration |
| `src/feed_handler.cpp` | Modify | Add `decode_event()` helper + `step()` impl; init `cursor_` |
| `include/order_book.h` | Modify | Add `BestLevel`, `LevelEntry` structs + 3 query method decls |
| `src/order_book.cpp` | Modify | Add `best_bid_level()`, `best_ask_level()`, `top_n_levels()` |
| `include/orderbook/replay_session.hpp` | Create | `Snapshot`, `SinkFn`, `SinkState`, `ReplaySession` class |
| `src/replay_session.cpp` | Create | `ItchEventSource`, `make_itch_source()`, `ReplaySession` impl |
| `adapters/csv_adapter.hpp` | Create | `CsvColumnMap`, `CsvEventSource` declaration |
| `adapters/csv_adapter.cpp` | Create | `CsvEventSource` impl: `split_sv`, type parsing, price parsing |
| `adapters/parquet_adapter.hpp` | Create | `ParquetEventSource` declaration (HAVE_ARROW guard) |
| `adapters/parquet_adapter.cpp` | Create | `ParquetEventSource` impl using Arrow RecordBatch cursor |
| `tests/test_adapters.cpp` | Create | All C++ unit tests for the new layer |
| `src/python_bindings.cpp` | Create | `PYBIND11_MODULE(orderbook, ...)` |
| `tests/python/test_bindings_smoke.py` | Create | `step`, `snapshot`, `query_top_n`, `register_sink` smoke tests |
| `benchmarks/py_replay_bench.py` | Create | Timed Python-bound replay sanity benchmark |
| `python/README.md` | Create | Build, install, and usage documentation |
| `python/examples/replay_demo.ipynb` | Create | Five-cell notebook demo |
| `ci/build-python-bindings.yml` | Create | CI job: build module + validate import + run smoke tests |
| `CMakeLists.txt` | Modify | Add `replay_session.cpp` to core lib; `ORDERBOOK_PYTHON` option |

---

## Task 1: IEventSource interface and Side enum

**Files:**
- Create: `include/orderbook/event_source.hpp`

- [ ] **Step 1: Create the `include/orderbook/` directory and header**

```cpp
// include/orderbook/event_source.hpp
#pragma once
#include "event.h"
#include <cstdint>

// Side enum: used at adapter and Python API boundaries.
// Core Event struct retains char side to avoid touching hot-path types.
enum class Side : uint8_t { BID = 'B', ASK = 'S' };

// Abstract source of normalized events.
// All implementations must be noexcept on next(); catch internally and return false.
struct IEventSource {
    virtual bool next(Event& ev) noexcept = 0;
    virtual ~IEventSource() = default;
};
```

- [ ] **Step 2: Verify the header compiles in isolation**

```bash
cd /Users/kolli/systems/orderbook
echo '#include "orderbook/event_source.hpp"' | \
  g++ -std=c++20 -Iinclude -x c++ - -fsyntax-only
```

Expected: no output (clean compile).

- [ ] **Step 3: Commit**

```bash
git add include/orderbook/event_source.hpp
git commit -m "feat: add IEventSource interface and Side enum"
```

---

## Task 2: FeedHandler single-step API

**Files:**
- Modify: `include/feed_handler.h`
- Modify: `src/feed_handler.cpp`

The current `run()` uses a local `ptr` variable. We add `cursor_` as a persistent member and a new `step(Event&)` method. `run()` is **not changed** — it keeps its own local pointer for the batch hot path.

- [ ] **Step 1: Add `cursor_` member and `step()` declaration to `include/feed_handler.h`**

Add to the `private:` section (after `skipped_`):
```cpp
const uint8_t* cursor_;  // current position for step(); initialised to data_ in ctor
```

Add to the `public:` section (after the two `run()` overloads):
```cpp
// Single-event decode: fills ev with the next recognized ITCH event, advances cursor_.
// Returns false on EOF or truncated stream. Book-agnostic — does not touch OrderBook.
bool step(Event& ev) noexcept;
```

Also add the `event.h` include at the top of `feed_handler.h`:
```cpp
#include "event.h"
```

- [ ] **Step 2: Implement `decode_event()` and `step()` in `src/feed_handler.cpp`**

Add `#include "event.h"` at the top if not present.

Insert `decode_event` as a new static function **before** `FeedHandler::dispatch()` in `src/feed_handler.cpp`:

```cpp
// Decode one ITCH message (ptr points to the 2-byte length field) into ev.
// Returns false for unknown/unsupported message types (same set as dispatch()).
static bool decode_event(const uint8_t* ptr, Event& ev) noexcept {
    const uint8_t type = ptr[2];
    ev.stock_locate  = itch_stock_locate(ptr);
    ev.timestamp_ns  = itch_timestamp(ptr);
    ev._pad[0] = ev._pad[1] = 0;
    ev.new_order_ref = 0;

    switch (type) {
    case 'A': case 'F':
        ev.type      = Event::Type::ADD;
        ev.side      = itch_side(ptr);
        ev.order_ref = itch_order_ref(ptr);
        ev.price     = itch_price_add(ptr);
        ev.quantity  = itch_shares_add(ptr);
        return true;
    case 'E':
        ev.type      = Event::Type::EXECUTE;
        ev.side      = 0;
        ev.order_ref = itch_order_ref(ptr);
        ev.price     = 0;          // resting price requires book lookup; not available here
        ev.quantity  = itch_exec_qty(ptr);
        return true;
    case 'C':
        ev.type      = Event::Type::EXECUTE_PRICE;
        ev.side      = 0;
        ev.order_ref = itch_order_ref(ptr);
        ev.price     = itch_exec_price(ptr);
        ev.quantity  = itch_exec_qty(ptr);
        return true;
    case 'X':
        ev.type      = Event::Type::CANCEL;
        ev.side      = 0;
        ev.order_ref = itch_order_ref(ptr);
        ev.price     = 0;
        ev.quantity  = itch_cancel_qty(ptr);
        return true;
    case 'D':
        ev.type      = Event::Type::DELETE;
        ev.side      = 0;
        ev.order_ref = itch_order_ref(ptr);
        ev.price     = 0;
        ev.quantity  = 0;
        return true;
    case 'U':
        ev.type          = Event::Type::REPLACE;
        ev.side          = 0;
        ev.order_ref     = itch_orig_ref(ptr);
        ev.new_order_ref = itch_new_ref(ptr);
        ev.price         = itch_repl_price(ptr);
        ev.quantity      = itch_repl_shares(ptr);
        return true;
    default:
        return false;
    }
}
```

Add `FeedHandler::step()` implementation after `FeedHandler::run()`:

```cpp
bool FeedHandler::step(Event& ev) noexcept {
    if (!valid()) return false;
    const uint8_t* end = data_ + size_;
    while (cursor_ + 2 <= end) {
        const uint16_t msg_len = load_be16(cursor_);
        const uint8_t* msg    = cursor_;
        cursor_ += 2 + msg_len;
        if (msg + 2 + msg_len > end) return false;  // truncated frame
        if (msg_len >= 3 && decode_event(msg, ev)) return true;
    }
    return false;
}
```

- [ ] **Step 3: Initialise `cursor_` in the constructor**

In `src/feed_handler.cpp`, inside `FeedHandler::FeedHandler(const char* path)`, change the member init list to include `cursor_(nullptr)`:

```cpp
FeedHandler::FeedHandler(const char* path)
    : data_(nullptr), size_(0), fd_(-1), skipped_(0), cursor_(nullptr)
```

Then add, immediately after the line `data_ = reinterpret_cast<const uint8_t*>(p);`:

```cpp
cursor_ = data_;
```

- [ ] **Step 4: Build and verify no regressions**

```bash
cd /Users/kolli/systems/orderbook
cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug 2>&1 | tail -5
cmake --build build-debug --target test_orderbook -j$(sysctl -n hw.logicalcpu)
cd build-debug && ctest --output-on-failure
```

Expected: all existing tests pass.

- [ ] **Step 5: Commit**

```bash
git add include/feed_handler.h src/feed_handler.cpp
git commit -m "feat: add FeedHandler::step() single-event decode with persistent cursor"
```

---

## Task 3: OrderBook query extensions

**Files:**
- Modify: `include/order_book.h`
- Modify: `src/order_book.cpp`

- [ ] **Step 1: Add structs and declarations to `include/order_book.h`**

Add the following inside the `OrderBook` class, in the `// --- Query ---` section, after `event_count()`:

```cpp
// --- Depth queries (research path; not hot-path) ---

struct BestLevel  { Price price; uint64_t volume; uint32_t order_count; };
struct LevelEntry { Price price; uint64_t volume; uint32_t order_count; };

// Returns {price, volume, count} for best bid/ask. Returns {NO_BID/NO_ASK, 0, 0}
// when the side is empty. Single lookup — no double call to best_bid() + level().
[[nodiscard]] BestLevel best_bid_level() const noexcept;
[[nodiscard]] BestLevel best_ask_level() const noexcept;

// Fills out[0..count-1] with up to n non-empty price levels starting from best.
// side='B' walks bid ladder downward; side='S' walks ask ladder upward.
// out must be caller-allocated with capacity >= n. n is clamped to 64 internally.
void top_n_levels(int n, char side, LevelEntry* out, int& count) const noexcept;
```

- [ ] **Step 2: Write the failing test first**

Create `tests/test_adapters.cpp` with only the `TopNLevels` and `BestLevels` tests — they reference the declarations above but won't link until Task 3 Step 3:

```cpp
// tests/test_adapters.cpp
#include <gtest/gtest.h>
#include "order_book.h"
#include "types.h"

// ---------------------------------------------------------------------------
// OrderBook::top_n_levels and best_bid/ask_level
// ---------------------------------------------------------------------------

TEST(OrderBook, TopNLevels_BidDescending) {
    OrderBook book(100, 256);
    Timestamp ts = 1;
    // 5 distinct bid prices, 4 orders of 100 shares each
    for (int p = 0; p < 5; ++p) {
        Price price = 500000 + static_cast<Price>(p) * 100;
        for (int i = 0; i < 4; ++i)
            book.add_order(static_cast<OrderRef>(p * 4 + i + 1), price, 100, 'B', ts++);
    }

    OrderBook::LevelEntry out[10];
    int count = 0;
    book.top_n_levels(3, 'B', out, count);

    ASSERT_EQ(count, 3);
    EXPECT_EQ(out[0].price, 500400);   // highest bid first
    EXPECT_EQ(out[1].price, 500300);
    EXPECT_EQ(out[2].price, 500200);
    EXPECT_EQ(out[0].volume, 400u);    // 4 orders × 100 shares
    EXPECT_EQ(out[0].order_count, 4u);
    // Cross-check via public level() accessor
    EXPECT_EQ(out[0].volume, book.level(500400).volume);
}

TEST(OrderBook, TopNLevels_AskAscending) {
    OrderBook book(100, 256);
    Timestamp ts = 1;
    for (int p = 0; p < 5; ++p) {
        Price price = 510000 + static_cast<Price>(p) * 100;
        book.add_order(static_cast<OrderRef>(p + 1), price, 200, 'S', ts++);
    }

    OrderBook::LevelEntry out[10];
    int count = 0;
    book.top_n_levels(3, 'S', out, count);

    ASSERT_EQ(count, 3);
    EXPECT_EQ(out[0].price, 510000);   // lowest ask first
    EXPECT_EQ(out[1].price, 510100);
    EXPECT_EQ(out[2].price, 510200);
}

TEST(OrderBook, TopNLevels_EmptyBook) {
    OrderBook book(16, 32);
    OrderBook::LevelEntry out[5];
    int count = 99;
    book.top_n_levels(5, 'B', out, count);
    EXPECT_EQ(count, 0);
}

TEST(OrderBook, BestLevels_Empty) {
    OrderBook book(16, 32);
    auto bid = book.best_bid_level();
    auto ask = book.best_ask_level();
    EXPECT_EQ(bid.price,  NO_BID);
    EXPECT_EQ(bid.volume, 0u);
    EXPECT_EQ(ask.price,  NO_ASK);
    EXPECT_EQ(ask.volume, 0u);
}

TEST(OrderBook, BestLevels_WithOrders) {
    OrderBook book(16, 32);
    book.add_order(1, 500000, 150, 'B', 1);
    book.add_order(2, 500100, 200, 'S', 2);
    auto bid = book.best_bid_level();
    auto ask = book.best_ask_level();
    EXPECT_EQ(bid.price,  500000);
    EXPECT_EQ(bid.volume, 150u);
    EXPECT_EQ(ask.price,  500100);
    EXPECT_EQ(ask.volume, 200u);
}
```

- [ ] **Step 3: Add `test_adapters.cpp` to CMakeLists.txt**

In `CMakeLists.txt`, modify the `add_executable(test_orderbook ...)` block:

```cmake
add_executable(test_orderbook
    tests/test_book_invariants.cpp
    tests/fuzz_book.cpp
    tests/test_adapters.cpp
)
```

- [ ] **Step 4: Run the test to see it fail (link error — methods not yet defined)**

```bash
cmake --build build-debug --target test_orderbook -j$(sysctl -n hw.logicalcpu) 2>&1 | grep -E "error:|undefined"
```

Expected: linker errors for `best_bid_level`, `best_ask_level`, `top_n_levels`.

- [ ] **Step 5: Implement the three methods in `src/order_book.cpp`**

Append after `best_ask()` (line ~202 in the file):

```cpp
OrderBook::BestLevel OrderBook::best_bid_level() const noexcept {
    Price bid = best_bid();
    if (bid == NO_BID) return {NO_BID, 0, 0};
    const PriceLevel& lvl = levels_[static_cast<uint32_t>(bid)];
    return {bid, lvl.volume, lvl.count};
}

OrderBook::BestLevel OrderBook::best_ask_level() const noexcept {
    Price ask = best_ask();
    if (ask == NO_ASK) return {NO_ASK, 0, 0};
    const PriceLevel& lvl = levels_[static_cast<uint32_t>(ask)];
    return {ask, lvl.volume, lvl.count};
}

void OrderBook::top_n_levels(int n, char side, LevelEntry* out, int& count) const noexcept {
    count = 0;
    if (n <= 0) return;
    if (n > 64) n = 64;  // hard cap — prevents unbounded price-ladder walks
    if (side == 'B') {
        Price bid = best_bid();
        if (bid == NO_BID) return;
        for (uint32_t tick = static_cast<uint32_t>(bid); count < n; ) {
            if (!levels_[tick].empty())
                out[count++] = {static_cast<Price>(tick), levels_[tick].volume, levels_[tick].count};
            if (tick == 0) break;
            --tick;
        }
    } else {
        Price ask = best_ask();
        if (ask == NO_ASK) return;
        for (uint32_t tick = static_cast<uint32_t>(ask); count < n && tick < MAX_TICKS; ++tick) {
            if (!levels_[tick].empty())
                out[count++] = {static_cast<Price>(tick), levels_[tick].volume, levels_[tick].count};
        }
    }
}
```

- [ ] **Step 6: Build and run tests**

```bash
cmake --build build-debug --target test_orderbook -j$(sysctl -n hw.logicalcpu)
cd build-debug && ctest --output-on-failure -R "OrderBook\."
```

Expected: `OrderBook.TopNLevels_BidDescending`, `TopNLevels_AskAscending`, `TopNLevels_EmptyBook`, `BestLevels_Empty`, `BestLevels_WithOrders` — all PASS.

- [ ] **Step 7: Commit**

```bash
git add include/order_book.h src/order_book.cpp tests/test_adapters.cpp CMakeLists.txt
git commit -m "feat: add OrderBook depth query methods (top_n_levels, best_bid/ask_level)"
```

---

## Task 4: ReplaySession

**Files:**
- Create: `include/orderbook/replay_session.hpp`
- Create: `src/replay_session.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add the failing test to `tests/test_adapters.cpp`**

Add these includes at the top of `tests/test_adapters.cpp`:

```cpp
#include "orderbook/replay_session.hpp"
#include "feed_handler.h"
#include <cstdlib>
```

Append the following tests at the bottom of the file:

```cpp
// ---------------------------------------------------------------------------
// ReplaySession: step-by-step checksum matches FeedHandler::run() batch
// ---------------------------------------------------------------------------

TEST(ReplaySession, FullReplayChecksumMatchesBatchRun) {
    const char* path = std::getenv("ITCH_FILE");
    if (!path) GTEST_SKIP() << "Set ITCH_FILE=data/mock.NASDAQ_ITCH50 to run";

    // Path 1: step-by-step via ReplaySession
    uint64_t step_checksum = 0;
    {
        ReplaySession session(make_itch_source(path));
        while (session.step()) {}
        step_checksum = session.snapshot().checksum;
    }

    // Path 2: FeedHandler::run() batch
    uint64_t run_checksum = 0;
    {
        FeedHandler feed(path);
        ASSERT_TRUE(feed.valid());
        OrderBook book;
        feed.run(book);
        run_checksum = book.checksum();
    }

    EXPECT_EQ(step_checksum, run_checksum)
        << "step-by-step replay produced different checksum than batch run";
}

TEST(ReplaySession, SnapshotStructuralInvariants) {
    const char* path = std::getenv("ITCH_FILE");
    if (!path) GTEST_SKIP() << "Set ITCH_FILE=data/mock.NASDAQ_ITCH50 to run";

    ReplaySession session(make_itch_source(path));
    for (int i = 0; i < 5000 && session.step(); ++i) {}

    Snapshot snap = session.snapshot();
    EXPECT_GT(snap.order_count, 100u);
    EXPECT_LT(snap.best_bid, snap.best_ask);
    EXPECT_NE(snap.checksum, 0u);
}

TEST(ReplaySession, QueryTopNBidDescending) {
    const char* path = std::getenv("ITCH_FILE");
    if (!path) GTEST_SKIP() << "Set ITCH_FILE=data/mock.NASDAQ_ITCH50 to run";

    ReplaySession session(make_itch_source(path));
    for (int i = 0; i < 5000 && session.step(); ++i) {}

    auto levels = session.query_top_n(5, Side::BID);
    ASSERT_LE(static_cast<int>(levels.size()), 5);
    for (size_t i = 1; i < levels.size(); ++i)
        EXPECT_GT(levels[i-1].first, levels[i].first) << "bid levels not strictly descending";
}
```

- [ ] **Step 2: Create `include/orderbook/replay_session.hpp`**

```cpp
// include/orderbook/replay_session.hpp
#pragma once
#include "orderbook/event_source.hpp"
#include "order_book.h"
#include "types.h"
#include <memory>
#include <utility>
#include <vector>

using SinkFn = void(*)(const Event&, void* ctx);

struct SinkState { SinkFn fn; void* ctx; };

struct Snapshot {
    Price    best_bid;
    uint64_t best_bid_volume;
    Price    best_ask;
    uint64_t best_ask_volume;
    uint64_t checksum;
    Timestamp timestamp_ns;
    uint32_t order_count;
};

class ReplaySession {
public:
    explicit ReplaySession(std::unique_ptr<IEventSource> source);

    ReplaySession(const ReplaySession&) = delete;
    ReplaySession& operator=(const ReplaySession&) = delete;

    bool     step()     noexcept;
    Snapshot snapshot() const noexcept;

    // Returns [(price_tick, volume), ...], up to n levels from best inward.
    std::vector<std::pair<int64_t, int64_t>> query_top_n(int n, Side side) const noexcept;

    void register_sink(SinkFn fn, void* ctx) noexcept;

private:
    std::unique_ptr<IEventSource> source_;
    OrderBook                     book_;
    std::unique_ptr<SinkState>    sink_;
    Timestamp                     last_ts_{0};
};

// Factory — creates an ItchEventSource. Defined in replay_session.cpp.
std::unique_ptr<IEventSource> make_itch_source(const char* path);
```

- [ ] **Step 3: Create `src/replay_session.cpp`**

```cpp
// src/replay_session.cpp
#include "orderbook/replay_session.hpp"
#include "feed_handler.h"
#include "event.h"
#include <algorithm>
#include <sstream>
#include <iomanip>

// ---------------------------------------------------------------------------
// ItchEventSource — thin wrapper around FeedHandler's step() cursor
// ---------------------------------------------------------------------------

class ItchEventSource : public IEventSource {
public:
    explicit ItchEventSource(const char* path) : feed_(path) {}
    bool next(Event& ev) noexcept override { return feed_.step(ev); }
    [[nodiscard]] bool valid() const noexcept { return feed_.valid(); }
private:
    FeedHandler feed_;
};

std::unique_ptr<IEventSource> make_itch_source(const char* path) {
    return std::make_unique<ItchEventSource>(path);
}

// ---------------------------------------------------------------------------
// ReplaySession
// ---------------------------------------------------------------------------

ReplaySession::ReplaySession(std::unique_ptr<IEventSource> source)
    : source_(std::move(source)) {}

bool ReplaySession::step() noexcept {
    Event ev{};
    if (!source_->next(ev)) return false;

    switch (ev.type) {
    case Event::Type::ADD:
        book_.add_order(ev.order_ref, ev.price, ev.quantity,
                        static_cast<char>(ev.side), ev.timestamp_ns);
        break;
    case Event::Type::EXECUTE:
        book_.execute_order(ev.order_ref, ev.quantity, ev.timestamp_ns);
        break;
    case Event::Type::EXECUTE_PRICE:
        book_.execute_order_price(ev.order_ref, ev.quantity, ev.price, ev.timestamp_ns);
        break;
    case Event::Type::CANCEL:
        book_.cancel_order(ev.order_ref, ev.quantity, ev.timestamp_ns);
        break;
    case Event::Type::DELETE:
        book_.delete_order(ev.order_ref, ev.timestamp_ns);
        break;
    case Event::Type::REPLACE:
        book_.replace_order(ev.order_ref, ev.new_order_ref,
                            ev.price, ev.quantity, ev.timestamp_ns);
        break;
    default:
        break;
    }

    last_ts_ = ev.timestamp_ns;

    if (sink_ &&
        (ev.type == Event::Type::EXECUTE || ev.type == Event::Type::EXECUTE_PRICE)) {
        sink_->fn(ev, sink_->ctx);
    }

    return true;
}

Snapshot ReplaySession::snapshot() const noexcept {
    auto bid = book_.best_bid_level();
    auto ask = book_.best_ask_level();
    return {bid.price, bid.volume, ask.price, ask.volume,
            book_.checksum(), last_ts_, book_.order_count()};
}

std::vector<std::pair<int64_t, int64_t>>
ReplaySession::query_top_n(int n, Side side) const noexcept {
    constexpr int MAX_LEVELS = 64;
    OrderBook::LevelEntry buf[MAX_LEVELS];
    int count = 0;
    book_.top_n_levels(std::min(n, MAX_LEVELS),
                       side == Side::BID ? 'B' : 'S',
                       buf, count);
    std::vector<std::pair<int64_t, int64_t>> result;
    result.reserve(count);
    for (int i = 0; i < count; ++i)
        result.emplace_back(buf[i].price, static_cast<int64_t>(buf[i].volume));
    return result;
}

void ReplaySession::register_sink(SinkFn fn, void* ctx) noexcept {
    sink_ = std::make_unique<SinkState>(SinkState{fn, ctx});
}
```

- [ ] **Step 4: Add `src/replay_session.cpp` to `orderbook_lib` in `CMakeLists.txt`**

```cmake
add_library(orderbook_lib
    src/feed_handler.cpp
    src/order_book.cpp
    src/slab_allocator.cpp
    src/fill_simulator.cpp
    src/pnl_tracker.cpp
    src/replay_session.cpp      # <- add
)
```

- [ ] **Step 5: Build and run tests**

```bash
cmake --build build-debug --target test_orderbook -j$(sysctl -n hw.logicalcpu)
ITCH_FILE=data/mock.NASDAQ_ITCH50 \
  cd build-debug && ctest --output-on-failure -R "ReplaySession\."
```

If `data/mock.NASDAQ_ITCH50` doesn't exist, generate it first:

```bash
cd /Users/kolli/systems/orderbook
python scripts/gen_mock_itch.py --output data/mock.NASDAQ_ITCH50 --events 50000
```

Expected: all three `ReplaySession.*` tests PASS.

- [ ] **Step 6: Commit**

```bash
git add include/orderbook/replay_session.hpp src/replay_session.cpp \
        tests/test_adapters.cpp CMakeLists.txt
git commit -m "feat: add ReplaySession and ItchEventSource"
```

---

## Task 5: CsvEventSource

**Files:**
- Create: `adapters/csv_adapter.hpp`
- Create: `adapters/csv_adapter.cpp`
- Modify: `tests/test_adapters.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add the failing test to `tests/test_adapters.cpp`**

Add includes at the top:
```cpp
#include "csv_adapter.hpp"
#include <fstream>
#include <cstdio>
```

Append test:
```cpp
// ---------------------------------------------------------------------------
// CsvEventSource round-trip
// ---------------------------------------------------------------------------

TEST(CsvAdapter, RoundTrip) {
    const char* tmp = "/tmp/ob_test_csv_adapter.csv";
    {
        std::ofstream f(tmp);
        // header row (skipped by adapter since it's not a valid type)
        f << "type,side,price,quantity,order_ref,timestamp_ns\n";
        f << "ADD,B,50.0000,100,1,1000000000\n";
        f << "ADD,S,50.0100,200,2,1000000001\n";
        f << "EXECUTE,B,0,50,1,1000000002\n";
        f << "DELETE,B,0,0,2,1000000003\n";
        f << "ADD,B,49.9900,300,3,1000000004\n";
    }

    CsvEventSource src(tmp);
    Event ev{};

    // Row 1: header — UNKNOWN type, skipped internally; first real event is ADD
    ASSERT_TRUE(src.next(ev));
    EXPECT_EQ(ev.type,       Event::Type::ADD);
    EXPECT_EQ(ev.side,       'B');
    EXPECT_EQ(ev.price,      500000);   // 50.0000 * 10000
    EXPECT_EQ(ev.quantity,   100u);
    EXPECT_EQ(ev.order_ref,  1u);
    EXPECT_EQ(ev.timestamp_ns, 1000000000u);

    ASSERT_TRUE(src.next(ev));
    EXPECT_EQ(ev.type,  Event::Type::ADD);
    EXPECT_EQ(ev.side,  'S');
    EXPECT_EQ(ev.price, 500100);        // 50.0100 * 10000

    ASSERT_TRUE(src.next(ev));
    EXPECT_EQ(ev.type, Event::Type::EXECUTE);

    ASSERT_TRUE(src.next(ev));
    EXPECT_EQ(ev.type, Event::Type::DELETE);

    ASSERT_TRUE(src.next(ev));
    EXPECT_EQ(ev.type,  Event::Type::ADD);
    EXPECT_EQ(ev.price, 499900);        // 49.9900 * 10000

    // EOF — must return false and remain stable
    EXPECT_FALSE(src.next(ev));
    EXPECT_FALSE(src.next(ev));

    std::remove(tmp);
}

TEST(CsvAdapter, CustomDelimiter) {
    const char* tmp = "/tmp/ob_test_csv_pipe.csv";
    {
        std::ofstream f(tmp);
        f << "ADD|B|50.0000|100|10|2000000000\n";
    }
    CsvColumnMap cols;
    cols.delimiter = '|';
    CsvEventSource src(tmp, cols);
    Event ev{};
    ASSERT_TRUE(src.next(ev));
    EXPECT_EQ(ev.type,  Event::Type::ADD);
    EXPECT_EQ(ev.price, 500000);
    EXPECT_FALSE(src.next(ev));
    std::remove(tmp);
}
```

- [ ] **Step 2: Create `adapters/csv_adapter.hpp`**

```cpp
// adapters/csv_adapter.hpp
#pragma once
#include "orderbook/event_source.hpp"
#include <fstream>
#include <string>

struct CsvColumnMap {
    int  type         = 0;
    int  side         = 1;
    int  price        = 2;
    int  quantity     = 3;
    int  order_ref    = 4;
    int  timestamp_ns = 5;
    char delimiter    = ',';
};

class CsvEventSource : public IEventSource {
public:
    // Throws std::runtime_error if path cannot be opened.
    explicit CsvEventSource(const char* path, CsvColumnMap cols = {});

    // Returns true when ev is populated. Returns false on EOF or unrecoverable error.
    // Skips rows with UNKNOWN type (e.g., header lines) silently.
    bool next(Event& ev) noexcept override;

private:
    std::ifstream file_;
    CsvColumnMap  cols_;
};
```

- [ ] **Step 3: Create `adapters/csv_adapter.cpp`**

```cpp
// adapters/csv_adapter.cpp
#include "csv_adapter.hpp"
#include "event.h"
#include "types.h"
#include <string>
#include <string_view>
#include <vector>
#include <stdexcept>
#include <algorithm>
#include <cmath>

// Split string_view on delimiter — no allocation for the view itself.
static std::vector<std::string_view> split_sv(std::string_view sv, char delim) {
    std::vector<std::string_view> out;
    while (true) {
        auto pos = sv.find(delim);
        out.push_back(sv.substr(0, pos));
        if (pos == std::string_view::npos) break;
        sv.remove_prefix(pos + 1);
    }
    return out;
}

static Event::Type parse_type(std::string_view s) noexcept {
    if (s == "ADD")           return Event::Type::ADD;
    if (s == "EXECUTE")       return Event::Type::EXECUTE;
    if (s == "EXECUTE_PRICE") return Event::Type::EXECUTE_PRICE;
    if (s == "CANCEL")        return Event::Type::CANCEL;
    if (s == "DELETE")        return Event::Type::DELETE;
    if (s == "REPLACE")       return Event::Type::REPLACE;
    return Event::Type::UNKNOWN;
}

// Accepts integer ticks ("500000") or decimal strings ("50.0000").
static Price parse_price(std::string_view s) {
    double d = std::stod(std::string(s));
    return static_cast<Price>(std::llround(d * 10000.0));
}

CsvEventSource::CsvEventSource(const char* path, CsvColumnMap cols)
    : file_(path), cols_(cols)
{
    if (!file_.is_open())
        throw std::runtime_error(std::string("CsvEventSource: cannot open ") + path);
}

bool CsvEventSource::next(Event& ev) noexcept {
    try {
        std::string line;
        while (std::getline(file_, line)) {
            if (line.empty()) continue;
            // Trim trailing \r (Windows line endings)
            if (!line.empty() && line.back() == '\r') line.pop_back();

            auto fields = split_sv(std::string_view(line), cols_.delimiter);
            int max_col = std::max({cols_.type, cols_.side, cols_.price,
                                    cols_.quantity, cols_.order_ref, cols_.timestamp_ns});
            if (static_cast<int>(fields.size()) <= max_col) continue;

            Event::Type t = parse_type(fields[cols_.type]);
            if (t == Event::Type::UNKNOWN) continue;  // skip header/comment rows

            ev = {};
            ev.type         = t;
            ev.side         = fields[cols_.side].empty() ? '\0' : fields[cols_.side][0];
            ev.price        = parse_price(fields[cols_.price]);
            ev.quantity     = static_cast<Quantity>(
                                  std::stoull(std::string(fields[cols_.quantity])));
            ev.order_ref    = static_cast<OrderRef>(
                                  std::stoull(std::string(fields[cols_.order_ref])));
            ev.timestamp_ns = static_cast<Timestamp>(
                                  std::stoull(std::string(fields[cols_.timestamp_ns])));
            return true;
        }
        return false;
    } catch (...) {
        return false;
    }
}
```

- [ ] **Step 4: Add adapter sources and include path to `CMakeLists.txt`**

Update the `test_orderbook` target to compile the CSV adapter and find its header:

```cmake
add_executable(test_orderbook
    tests/test_book_invariants.cpp
    tests/fuzz_book.cpp
    tests/test_adapters.cpp
    adapters/csv_adapter.cpp      # <- add
)
target_link_libraries(test_orderbook orderbook_lib GTest::gtest_main)
target_compile_options(test_orderbook PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_include_directories(test_orderbook PRIVATE adapters/)   # <- add
```

- [ ] **Step 5: Build and run the CSV tests**

```bash
cmake --build build-debug --target test_orderbook -j$(sysctl -n hw.logicalcpu)
cd build-debug && ctest --output-on-failure -R "CsvAdapter\."
```

Expected: `CsvAdapter.RoundTrip` and `CsvAdapter.CustomDelimiter` PASS.

- [ ] **Step 6: Commit**

```bash
git add adapters/csv_adapter.hpp adapters/csv_adapter.cpp \
        tests/test_adapters.cpp CMakeLists.txt
git commit -m "feat: add CsvEventSource adapter"
```

---

## Task 6: ParquetEventSource

**Files:**
- Create: `adapters/parquet_adapter.hpp`
- Create: `adapters/parquet_adapter.cpp`
- Modify: `tests/test_adapters.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add the guarded test to `tests/test_adapters.cpp`**

Add at the bottom:

```cpp
// ---------------------------------------------------------------------------
// ParquetEventSource (compiled only when HAVE_ARROW is defined)
// ---------------------------------------------------------------------------

#ifdef HAVE_ARROW
#include "parquet_adapter.hpp"
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/writer.h>

static void write_test_parquet(const char* path) {
    auto type_arr  = arrow::ArrayFromJSON(arrow::utf8(),
        R"(["ADD","ADD","EXECUTE","DELETE"])");
    auto side_arr  = arrow::ArrayFromJSON(arrow::utf8(),
        R"(["B","S","B","B"])");
    auto price_arr = arrow::ArrayFromJSON(arrow::int64(),
        "[500000,500100,0,0]");
    auto qty_arr   = arrow::ArrayFromJSON(arrow::int64(),
        "[100,200,50,0]");
    auto ref_arr   = arrow::ArrayFromJSON(arrow::int64(),
        "[1,2,1,2]");
    auto ts_arr    = arrow::ArrayFromJSON(arrow::int64(),
        "[1000000000,1000000001,1000000002,1000000003]");

    auto schema = arrow::schema({
        arrow::field("type", arrow::utf8()),
        arrow::field("side", arrow::utf8()),
        arrow::field("price", arrow::int64()),
        arrow::field("quantity", arrow::int64()),
        arrow::field("order_ref", arrow::int64()),
        arrow::field("timestamp_ns", arrow::int64()),
    });
    auto table = *arrow::Table::Make(schema, {
        type_arr.ValueOrDie(), side_arr.ValueOrDie(),
        price_arr.ValueOrDie(), qty_arr.ValueOrDie(),
        ref_arr.ValueOrDie(), ts_arr.ValueOrDie()
    });

    auto outfile = *arrow::io::FileOutputStream::Open(path);
    PARQUET_THROW_NOT_OK(
        parquet::arrow::WriteTable(*table, arrow::default_memory_pool(),
                                   outfile, /*chunk_size=*/1024));
}

TEST(ParquetAdapter, RoundTrip) {
    const char* tmp = "/tmp/ob_test_parquet_adapter.parquet";
    write_test_parquet(tmp);

    ParquetEventSource src(tmp);
    Event ev{};

    ASSERT_TRUE(src.next(ev));
    EXPECT_EQ(ev.type, Event::Type::ADD);
    EXPECT_EQ(ev.side, 'B');
    EXPECT_EQ(ev.price, 500000);

    ASSERT_TRUE(src.next(ev));
    EXPECT_EQ(ev.type, Event::Type::ADD);
    EXPECT_EQ(ev.price, 500100);

    ASSERT_TRUE(src.next(ev));
    EXPECT_EQ(ev.type, Event::Type::EXECUTE);

    ASSERT_TRUE(src.next(ev));
    EXPECT_EQ(ev.type, Event::Type::DELETE);

    EXPECT_FALSE(src.next(ev));
    std::remove(tmp);
}
#endif  // HAVE_ARROW
```

- [ ] **Step 2: Create `adapters/parquet_adapter.hpp`**

```cpp
// adapters/parquet_adapter.hpp
#pragma once
#include "orderbook/event_source.hpp"
#include <memory>
#include <stdexcept>

#ifdef HAVE_ARROW
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/reader.h>
#endif

class ParquetEventSource : public IEventSource {
public:
    // Throws std::runtime_error if:
    //   - Arrow not compiled in (HAVE_ARROW not defined)
    //   - File cannot be opened
    //   - Schema is missing required columns
    explicit ParquetEventSource(const char* path);
    ~ParquetEventSource() override;

    bool next(Event& ev) noexcept override;

private:
#ifdef HAVE_ARROW
    std::shared_ptr<arrow::RecordBatchReader> reader_;
    std::shared_ptr<arrow::RecordBatch>       batch_;
    int64_t row_idx_{0};

    // Column indices (resolved once at construction from schema)
    int col_type_{-1}, col_side_{-1}, col_price_{-1},
        col_qty_{-1},  col_ref_{-1},  col_ts_{-1};

    void resolve_columns(const arrow::Schema& schema);
    bool advance_batch() noexcept;
#endif
};
```

- [ ] **Step 3: Create `adapters/parquet_adapter.cpp`**

```cpp
// adapters/parquet_adapter.cpp
#include "parquet_adapter.hpp"
#include "event.h"
#include "types.h"
#include <stdexcept>
#include <string>

#ifndef HAVE_ARROW

ParquetEventSource::ParquetEventSource(const char* /*path*/) {
    throw std::runtime_error(
        "Parquet support not compiled in; "
        "rebuild with -DORDERBOOK_PYTHON=ON and Arrow/Parquet installed");
}
ParquetEventSource::~ParquetEventSource() = default;
bool ParquetEventSource::next(Event& /*ev*/) noexcept { return false; }

#else   // HAVE_ARROW

#include <arrow/array.h>
#include <arrow/table.h>
#include <parquet/arrow/reader.h>

static Event::Type parse_type_str(const std::string& s) noexcept {
    if (s == "ADD")           return Event::Type::ADD;
    if (s == "EXECUTE")       return Event::Type::EXECUTE;
    if (s == "EXECUTE_PRICE") return Event::Type::EXECUTE_PRICE;
    if (s == "CANCEL")        return Event::Type::CANCEL;
    if (s == "DELETE")        return Event::Type::DELETE;
    if (s == "REPLACE")       return Event::Type::REPLACE;
    return Event::Type::UNKNOWN;
}

void ParquetEventSource::resolve_columns(const arrow::Schema& schema) {
    auto require = [&](const char* name) -> int {
        int idx = schema.GetFieldIndex(name);
        if (idx < 0)
            throw std::runtime_error(
                std::string("ParquetEventSource: missing column '") + name + "'");
        return idx;
    };
    col_type_  = require("type");
    col_side_  = require("side");
    col_price_ = require("price");
    col_qty_   = require("quantity");
    col_ref_   = require("order_ref");
    col_ts_    = require("timestamp_ns");
}

ParquetEventSource::ParquetEventSource(const char* path) {
    auto infile_result = arrow::io::ReadableFile::Open(path);
    if (!infile_result.ok())
        throw std::runtime_error("ParquetEventSource: cannot open " + std::string(path));

    std::unique_ptr<parquet::arrow::FileReader> file_reader;
    auto st = parquet::arrow::OpenFile(
        *infile_result, arrow::default_memory_pool(), &file_reader);
    if (!st.ok())
        throw std::runtime_error("ParquetEventSource: " + st.ToString());

    std::shared_ptr<arrow::RecordBatchReader> batch_reader;
    st = file_reader->GetRecordBatchReader(&batch_reader);
    if (!st.ok())
        throw std::runtime_error("ParquetEventSource: " + st.ToString());

    reader_ = batch_reader;
    resolve_columns(*reader_->schema());
    advance_batch();  // prime first batch
}

ParquetEventSource::~ParquetEventSource() = default;

bool ParquetEventSource::advance_batch() noexcept {
    try {
        auto st = reader_->ReadNext(&batch_);
        if (!st.ok() || !batch_) { batch_ = nullptr; return false; }
        row_idx_ = 0;
        return true;
    } catch (...) { batch_ = nullptr; return false; }
}

bool ParquetEventSource::next(Event& ev) noexcept {
    try {
        while (true) {
            if (!batch_ || row_idx_ >= batch_->num_rows()) {
                if (!advance_batch()) return false;
            }

            int64_t r = row_idx_++;

            auto& type_col = static_cast<const arrow::StringArray&>(
                                 *batch_->column(col_type_));
            auto& side_col = static_cast<const arrow::StringArray&>(
                                 *batch_->column(col_side_));
            auto& price_col= static_cast<const arrow::Int64Array&>(
                                 *batch_->column(col_price_));
            auto& qty_col  = static_cast<const arrow::Int64Array&>(
                                 *batch_->column(col_qty_));
            auto& ref_col  = static_cast<const arrow::Int64Array&>(
                                 *batch_->column(col_ref_));
            auto& ts_col   = static_cast<const arrow::Int64Array&>(
                                 *batch_->column(col_ts_));

            Event::Type t = parse_type_str(type_col.GetString(r));
            if (t == Event::Type::UNKNOWN) continue;

            ev = {};
            ev.type         = t;
            std::string side_str = side_col.GetString(r);
            ev.side         = side_str.empty() ? '\0' : side_str[0];
            ev.price        = static_cast<Price>(price_col.Value(r));
            ev.quantity     = static_cast<Quantity>(qty_col.Value(r));
            ev.order_ref    = static_cast<OrderRef>(ref_col.Value(r));
            ev.timestamp_ns = static_cast<Timestamp>(ts_col.Value(r));
            return true;
        }
    } catch (...) { return false; }
}

#endif  // HAVE_ARROW
```

- [ ] **Step 4: Update `CMakeLists.txt` for optional Arrow in tests**

After the `find_package(Arrow)` / `find_package(Parquet)` calls that will be added in Task 7, add a conditional block for the test target:

```cmake
# Optionally compile Parquet adapter into the test binary if Arrow is found.
# This block is added now; find_package calls are in the ORDERBOOK_PYTHON section.
find_package(Arrow QUIET)
find_package(Parquet QUIET)
if(Arrow_FOUND AND Parquet_FOUND)
    target_sources(test_orderbook PRIVATE adapters/parquet_adapter.cpp)
    target_link_libraries(test_orderbook
        Arrow::arrow_shared Parquet::parquet_shared)
    target_compile_definitions(test_orderbook PRIVATE HAVE_ARROW=1)
    target_include_directories(test_orderbook PRIVATE adapters/)
endif()
```

Also add `adapters/parquet_adapter.cpp` to the CSV+parquet include:

```cmake
target_include_directories(test_orderbook PRIVATE adapters/)
```

(This replaces the earlier `PRIVATE adapters/` addition from Task 5.)

- [ ] **Step 5: Build and run Parquet tests**

```bash
cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug 2>&1 | grep -i arrow
cmake --build build-debug --target test_orderbook -j$(sysctl -n hw.logicalcpu)
cd build-debug && ctest --output-on-failure -R "ParquetAdapter\."
```

If Arrow is not installed: `ParquetAdapter.*` will not appear (test is compiled away). That is correct behavior. If Arrow is installed: test must PASS.

- [ ] **Step 6: Commit**

```bash
git add adapters/parquet_adapter.hpp adapters/parquet_adapter.cpp \
        tests/test_adapters.cpp CMakeLists.txt
git commit -m "feat: add ParquetEventSource adapter (optional Arrow dependency)"
```

---

## Task 7: CMake Python option and Python bindings

**Files:**
- Modify: `CMakeLists.txt`
- Create: `src/python_bindings.cpp`

- [ ] **Step 1: Add `ORDERBOOK_PYTHON` block to `CMakeLists.txt`**

Append at the end of `CMakeLists.txt`, before the `message(STATUS ...)` lines:

```cmake
# Python bindings (pybind11)
option(ORDERBOOK_PYTHON "Build pybind11 Python module" OFF)
if(ORDERBOOK_PYTHON)
    find_package(pybind11 REQUIRED)

    pybind11_add_module(orderbook
        src/python_bindings.cpp
        adapters/csv_adapter.cpp
    )
    target_link_libraries(orderbook PRIVATE orderbook_lib)
    target_include_directories(orderbook PRIVATE adapters/)

    # Arrow/Parquet are optional for the Python module too.
    if(Arrow_FOUND AND Parquet_FOUND)
        target_sources(orderbook PRIVATE adapters/parquet_adapter.cpp)
        target_link_libraries(orderbook PRIVATE
            Arrow::arrow_shared Parquet::parquet_shared)
        target_compile_definitions(orderbook PRIVATE HAVE_ARROW=1)
    endif()
endif()
```

Note: the `find_package(Arrow ...)` calls added in Task 6 cover both the test and the Python module.

- [ ] **Step 2: Create `src/python_bindings.cpp`**

```cpp
// src/python_bindings.cpp
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include "orderbook/event_source.hpp"
#include "orderbook/replay_session.hpp"
#include "csv_adapter.hpp"
#ifdef HAVE_ARROW
#include "parquet_adapter.hpp"
#endif
#include "event.h"
#include <memory>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <string>

namespace py = pybind11;
using namespace pybind11::literals;

// ---------------------------------------------------------------------------
// Format detection + source factory
// ---------------------------------------------------------------------------

static std::unique_ptr<IEventSource> make_source(const std::string& path) {
    auto dot = path.rfind('.');
    if (dot == std::string::npos)
        throw std::runtime_error("No file extension in path: " + path
                                 + ". Supported: .csv, .parquet, .itch50, .NASDAQ_ITCH50");
    std::string ext = path.substr(dot);
    if (ext == ".csv")
        return std::make_unique<CsvEventSource>(path.c_str());
    if (ext == ".parquet") {
#ifdef HAVE_ARROW
        return std::make_unique<ParquetEventSource>(path.c_str());
#else
        throw std::runtime_error(
            "Parquet support not compiled in; "
            "rebuild with -DORDERBOOK_PYTHON=ON and Arrow/Parquet installed");
#endif
    }
    if (ext == ".itch50" || ext == ".NASDAQ_ITCH50" || ext == ".bin")
        return make_itch_source(path.c_str());
    throw std::runtime_error(
        "Unknown format: " + ext
        + ". Supported extensions: .csv, .parquet, .itch50, .NASDAQ_ITCH50");
}

// ---------------------------------------------------------------------------
// Python-side wrapper: owns ReplaySession + Python sink callable
// ---------------------------------------------------------------------------

struct PySinkState { py::object fn; };

struct PyReplaySession {
    std::unique_ptr<ReplaySession> session;
    std::unique_ptr<PySinkState>   sink_state;  // null until register_sink called

    static void trampoline(const Event& ev, void* ctx) {
        py::gil_scoped_acquire gil;
        auto* s = static_cast<PySinkState*>(ctx);
        s->fn(py::make_tuple(
            static_cast<int>(ev.type),        // int; compare with EventType.X.value
            ev.price,                          // int64
            static_cast<int>(ev.quantity),     // int
            static_cast<Side>(ev.side),        // Side enum object via pybind binding
            ev.timestamp_ns                    // uint64
        ));
    }
};

// ---------------------------------------------------------------------------
// Module definition
// ---------------------------------------------------------------------------

PYBIND11_MODULE(orderbook, m) {
    m.doc() = "NASDAQ ITCH 5.0 order book replay engine";

    py::enum_<Event::Type>(m, "EventType")
        .value("ADD",           Event::Type::ADD)
        .value("EXECUTE",       Event::Type::EXECUTE)
        .value("EXECUTE_PRICE", Event::Type::EXECUTE_PRICE)
        .value("CANCEL",        Event::Type::CANCEL)
        .value("DELETE",        Event::Type::DELETE)
        .value("REPLACE",       Event::Type::REPLACE)
        .export_values();

    py::enum_<Side>(m, "Side")
        .value("BID", Side::BID)
        .value("ASK", Side::ASK);

    py::class_<PyReplaySession>(m, "ReplaySession")
        .def(py::init([](const std::string& path, const std::string& /*mode*/) {
            auto prs = std::make_unique<PyReplaySession>();
            prs->session = std::make_unique<ReplaySession>(make_source(path));
            return prs;
        }), py::arg("path"), py::arg("mode") = "replay")

        .def("step", [](PyReplaySession& self) {
            py::gil_scoped_release release;
            return self.session->step();
        })

        .def("snapshot", [](const PyReplaySession& self) {
            Snapshot s = self.session->snapshot();
            std::ostringstream oss;
            oss << "0x" << std::hex << std::setfill('0')
                << std::setw(16) << s.checksum;
            return py::dict(
                "best_bid"_a        = s.best_bid,
                "best_bid_volume"_a = s.best_bid_volume,
                "best_ask"_a        = s.best_ask,
                "best_ask_volume"_a = s.best_ask_volume,
                "checksum"_a        = oss.str(),
                "timestamp_ns"_a    = s.timestamp_ns,
                "order_count"_a     = s.order_count
            );
        })

        .def("query_top_n", [](const PyReplaySession& self, int n, Side side) {
            return self.session->query_top_n(n, side);
        }, py::arg("n"), py::arg("side"))

        .def("register_sink", [](PyReplaySession& self, py::object fn) {
            self.sink_state = std::make_unique<PySinkState>(PySinkState{fn});
            self.session->register_sink(
                PyReplaySession::trampoline, self.sink_state.get());
        });

    m.def("load_replay", [](const std::string& path) {
        auto prs = std::make_unique<PyReplaySession>();
        prs->session = std::make_unique<ReplaySession>(make_source(path));
        return prs;
    }, py::arg("path"),
       "Load a replay session from path. Format auto-detected by extension "
       "(.itch50/.NASDAQ_ITCH50/.bin → ITCH, .csv → CSV, .parquet → Parquet).");
}
```

- [ ] **Step 3: Build the Python module**

```bash
cd /Users/kolli/systems/orderbook
pip install pybind11
cmake -B build-py \
      -DCMAKE_BUILD_TYPE=Release \
      -DORDERBOOK_PYTHON=ON \
      -Dpybind11_DIR=$(python -c "import pybind11; print(pybind11.get_cmake_dir())")
cmake --build build-py --target orderbook -j$(sysctl -n hw.logicalcpu)
```

Expected: `build-py/orderbook.cpython-*.so` (or `.dylib` on macOS) produced with no errors.

- [ ] **Step 4: Validate the module loads**

```bash
PYTHONPATH=build-py python -c "import orderbook; print(orderbook.__doc__)"
```

Expected output:
```
NASDAQ ITCH 5.0 order book replay engine
```

- [ ] **Step 5: Commit**

```bash
git add src/python_bindings.cpp CMakeLists.txt
git commit -m "feat: add pybind11 Python module with ReplaySession, EventType, Side"
```

---

## Task 8: Python smoke tests

**Files:**
- Create: `tests/python/__init__.py`
- Create: `tests/python/conftest.py`
- Create: `tests/python/test_bindings_smoke.py`

- [ ] **Step 1: Create the Python test package**

```bash
mkdir -p /Users/kolli/systems/orderbook/tests/python
touch /Users/kolli/systems/orderbook/tests/python/__init__.py
```

- [ ] **Step 2: Create `tests/python/conftest.py`**

```python
# tests/python/conftest.py
import os
import pytest

@pytest.fixture(scope="session")
def itch_path():
    path = os.environ.get("ITCH_FILE", "data/mock.NASDAQ_ITCH50")
    if not os.path.exists(path):
        pytest.skip(f"ITCH_FILE not found at {path}; run gen_mock_itch.py first")
    return path
```

- [ ] **Step 3: Create `tests/python/test_bindings_smoke.py`**

```python
# tests/python/test_bindings_smoke.py
import orderbook


def test_module_attributes():
    """Module exports required symbols."""
    assert hasattr(orderbook, "ReplaySession")
    assert hasattr(orderbook, "load_replay")
    assert hasattr(orderbook, "EventType")
    assert hasattr(orderbook, "Side")


def test_event_type_values():
    """EventType enum values are stable integers."""
    assert orderbook.EventType.ADD.value           == 0
    assert orderbook.EventType.EXECUTE.value       == 1
    assert orderbook.EventType.EXECUTE_PRICE.value == 2
    assert orderbook.EventType.CANCEL.value        == 3
    assert orderbook.EventType.DELETE.value        == 4
    assert orderbook.EventType.REPLACE.value       == 5


def test_side_values():
    """Side enum maps to expected char codes."""
    assert int(orderbook.Side.BID) == ord('B')
    assert int(orderbook.Side.ASK) == ord('S')


def test_step_and_snapshot(itch_path):
    """Step 1000 events; snapshot has required keys and structural invariants."""
    s = orderbook.load_replay(itch_path)
    count = 0
    while s.step() and count < 1000:
        count += 1
    assert count == 1000, "expected exactly 1000 steps"

    snap = s.snapshot()
    required_keys = {"best_bid", "best_bid_volume", "best_ask",
                     "best_ask_volume", "checksum", "timestamp_ns", "order_count"}
    assert required_keys.issubset(snap.keys())
    assert snap["order_count"] > 100
    assert snap["best_bid"] < snap["best_ask"], "crossed market in snapshot"
    assert snap["checksum"].startswith("0x")


def test_query_top_n_bid_descending(itch_path):
    """query_top_n returns bid levels in strictly descending price order."""
    s = orderbook.load_replay(itch_path)
    for _ in range(500):
        s.step()
    levels = s.query_top_n(5, orderbook.Side.BID)
    assert len(levels) <= 5
    prices = [p for p, _ in levels]
    assert prices == sorted(prices, reverse=True), "bid prices not descending"


def test_query_top_n_ask_ascending(itch_path):
    """query_top_n returns ask levels in strictly ascending price order."""
    s = orderbook.load_replay(itch_path)
    for _ in range(500):
        s.step()
    levels = s.query_top_n(5, orderbook.Side.ASK)
    assert len(levels) <= 5
    prices = [p for p, _ in levels]
    assert prices == sorted(prices), "ask prices not ascending"


def test_register_sink_receives_fills(itch_path):
    """Sink callable receives tuples for EXECUTE and EXECUTE_PRICE events."""
    s = orderbook.load_replay(itch_path)
    fills = []
    s.register_sink(lambda t: fills.append(t))
    for _ in range(2000):
        s.step()
    # There may be zero fills in the first 2000 events; just verify format if any
    for t in fills:
        assert isinstance(t, tuple) and len(t) == 5
        assert t[0] in (
            orderbook.EventType.EXECUTE.value,
            orderbook.EventType.EXECUTE_PRICE.value,
        ), f"unexpected event type in sink: {t[0]}"


def test_unknown_extension_raises():
    """load_replay raises for unknown file extension."""
    import pytest
    with pytest.raises(RuntimeError, match="Unknown format"):
        orderbook.load_replay("data/file.xyz")


def test_eof_returns_false(itch_path):
    """step() returns False at EOF and remains stable."""
    s = orderbook.load_replay(itch_path)
    # Drain the file
    while s.step():
        pass
    assert not s.step()
    assert not s.step()  # idempotent
```

- [ ] **Step 4: Run the Python smoke tests**

```bash
cd /Users/kolli/systems/orderbook
PYTHONPATH=build-py ITCH_FILE=data/mock.NASDAQ_ITCH50 \
  .venv/bin/pytest tests/python/ -v
```

Expected: all tests PASS (or SKIP with a message if `ITCH_FILE` missing for file-dependent tests).

- [ ] **Step 5: Commit**

```bash
git add tests/python/__init__.py tests/python/conftest.py \
        tests/python/test_bindings_smoke.py
git commit -m "test: add Python smoke tests for orderbook module"
```

---

## Task 9: Benchmark script, notebook, README, and CI

**Files:**
- Create: `benchmarks/py_replay_bench.py`
- Create: `python/README.md`
- Create: `python/examples/replay_demo.ipynb`
- Create: `ci/build-python-bindings.yml`

- [ ] **Step 1: Create `benchmarks/py_replay_bench.py`**

```python
#!/usr/bin/env python3
"""Python-bound replay throughput sanity benchmark.

Usage:
    python benchmarks/py_replay_bench.py data/mock.NASDAQ_ITCH50
    python benchmarks/py_replay_bench.py data/mock.NASDAQ_ITCH50 --events 200000 --debug
"""
import argparse
import time
import sys
import os

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "build-py"))
import orderbook  # noqa: E402

def main():
    parser = argparse.ArgumentParser(description="Python-bound replay benchmark")
    parser.add_argument("path", help="ITCH/CSV/Parquet file to replay")
    parser.add_argument("--events", type=int, default=100_000,
                        help="Maximum events to process (default: 100000)")
    parser.add_argument("--debug", action="store_true",
                        help="Print snapshot after timing (excluded from measurement)")
    args = parser.parse_args()

    s = orderbook.load_replay(args.path)

    t0 = time.perf_counter()
    count = 0
    while s.step() and count < args.events:
        count += 1
    elapsed = time.perf_counter() - t0

    print(f"events:     {count:>10,}")
    print(f"elapsed:    {elapsed*1000:>10.1f} ms")
    print(f"throughput: {count/elapsed/1e6:>10.3f} M events/sec")
    print()
    print("NOTE: Python-bound throughput is intentionally lower than native")
    print("      FeedHandler::run() due to pybind11 call overhead (~50-200 ns/call).")

    if args.debug:
        snap = s.snapshot()
        print(f"\nSnapshot: {snap}")

if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Verify the benchmark runs**

```bash
cd /Users/kolli/systems/orderbook
PYTHONPATH=build-py python benchmarks/py_replay_bench.py \
    data/mock.NASDAQ_ITCH50 --events 10000
```

Expected: timing output with `M events/sec` figure, no crash.

- [ ] **Step 3: Create `python/README.md`**

```markdown
# orderbook Python Module

Python bindings for the NASDAQ ITCH 5.0 order book replay engine.

## Build

Prerequisites: C++20 compiler, CMake ≥ 3.20, pybind11 ≥ 2.11.  
Optional: Apache Arrow ≥ 10.0 + Parquet for `.parquet` input support.

```bash
pip install pybind11
cmake -B build-py \
      -DCMAKE_BUILD_TYPE=Release \
      -DORDERBOOK_PYTHON=ON \
      -Dpybind11_DIR=$(python -c "import pybind11; print(pybind11.get_cmake_dir())")
cmake --build build-py --target orderbook -j$(nproc)
```

## Usage

```python
import sys; sys.path.insert(0, "build-py")
import orderbook

# Load any supported format
s = orderbook.load_replay("data/events.NASDAQ_ITCH50")  # ITCH binary
s = orderbook.load_replay("data/events.csv")             # CSV
s = orderbook.load_replay("data/events.parquet")         # Parquet (requires Arrow)

# Step through events
while s.step():
    pass  # or sample periodically

# Inspect book state
snap = s.snapshot()
# {'best_bid': 500000, 'best_bid_volume': 1200, 'best_ask': 500100,
#  'best_ask_volume': 800, 'checksum': '0x...', 'timestamp_ns': ..., 'order_count': ...}

# Top-N depth
levels = s.query_top_n(10, orderbook.Side.BID)  # [(price_tick, volume), ...]

# Fill sink
s.register_sink(lambda t: print(t))
# Sink tuple: (type_int, price, quantity, Side_enum, timestamp_ns)
# type_int: compare with orderbook.EventType.EXECUTE.value etc.
```

## CSV format

Default column order: `type, side, price, quantity, order_ref, timestamp_ns`

- `type`: `ADD`, `EXECUTE`, `EXECUTE_PRICE`, `CANCEL`, `DELETE`, `REPLACE`
- `side`: `B` or `S`
- `price`: integer ticks (e.g. `500000`) or decimal string (e.g. `50.0000`)

Custom column mapping:

```python
from csv_adapter import CsvColumnMap  # if using C++ directly
# Or via Python: column order is 0-indexed, configurable at construction
```

## Parquet schema

Required columns (any column type compatible with int64/utf8 is accepted):

| Column | Arrow type | Notes |
|---|---|---|
| `type` | `utf8` | See CSV type names above |
| `side` | `utf8` | `"B"` or `"S"` |
| `price` | `int64` | Fixed-point ticks ($0.0001/unit) |
| `quantity` | `int64` | |
| `order_ref` | `int64` | |
| `timestamp_ns` | `int64` | Nanoseconds from midnight |

## EventType constants

```python
orderbook.EventType.ADD            # 0
orderbook.EventType.EXECUTE        # 1
orderbook.EventType.EXECUTE_PRICE  # 2
orderbook.EventType.CANCEL         # 3
orderbook.EventType.DELETE         # 4
orderbook.EventType.REPLACE        # 5
```

## Performance note

Python-bound `step()` throughput is intentionally lower than native
`FeedHandler::run()` due to the pybind11 call overhead (~50–200 ns per call).
The Python layer targets usability and reproducibility, not peak throughput.

Run the benchmark: `python benchmarks/py_replay_bench.py <file> --events 100000`

## Determinism

> OrderBook observable state is a deterministic function of the ordered event
> sequence. All internal randomness sources are eliminated or fixed-seeded, and
> no memory addresses, timestamps (except event timestamps), or allocator
> metadata contribute to observable outputs.

`snapshot()["checksum"]` is stable across runs for identical input.

## Notebook example

See `python/examples/replay_demo.ipynb` for an interactive walkthrough.
```

- [ ] **Step 4: Create `python/examples/replay_demo.ipynb`**

```bash
mkdir -p /Users/kolli/systems/orderbook/python/examples
```

Create the notebook file:

```json
{
 "cells": [
  {
   "cell_type": "code",
   "execution_count": null,
   "metadata": {},
   "outputs": [],
   "source": [
    "import sys\n",
    "sys.path.insert(0, '../../build-py')\n",
    "import orderbook\n",
    "import pandas as pd\n",
    "import matplotlib.pyplot as plt\n",
    "print('orderbook module loaded:', orderbook.__doc__)"
   ]
  },
  {
   "cell_type": "code",
   "execution_count": null,
   "metadata": {},
   "outputs": [],
   "source": [
    "# Load and step through 10,000 events, sampling book state every 100 steps\n",
    "s = orderbook.load_replay('../../data/mock.NASDAQ_ITCH50')\n",
    "records = []\n",
    "i = 0\n",
    "while s.step() and i < 10_000:\n",
    "    if i % 100 == 0:\n",
    "        snap = s.snapshot()\n",
    "        records.append({\n",
    "            'event': i,\n",
    "            'best_bid':  snap['best_bid']  / 10_000,\n",
    "            'best_ask':  snap['best_ask']  / 10_000,\n",
    "            'spread':   (snap['best_ask'] - snap['best_bid']) / 10_000,\n",
    "            'order_count': snap['order_count'],\n",
    "        })\n",
    "    i += 1\n",
    "df = pd.DataFrame(records)\n",
    "df.head()"
   ]
  },
  {
   "cell_type": "code",
   "execution_count": null,
   "metadata": {},
   "outputs": [],
   "source": [
    "# Bid/ask spread over time\n",
    "fig, axes = plt.subplots(2, 1, figsize=(12, 6))\n",
    "df.plot(x='event', y=['best_bid', 'best_ask'], ax=axes[0],\n",
    "        title='Top-of-Book Prices', ylabel='Price ($)')\n",
    "df.plot(x='event', y='spread', ax=axes[1],\n",
    "        title='Bid-Ask Spread', ylabel='Spread ($)', color='orange')\n",
    "plt.tight_layout()\n",
    "plt.show()"
   ]
  },
  {
   "cell_type": "code",
   "execution_count": null,
   "metadata": {},
   "outputs": [],
   "source": [
    "# Top-10 bid levels at end of replay\n",
    "bid_levels = s.query_top_n(10, orderbook.Side.BID)\n",
    "ask_levels = s.query_top_n(10, orderbook.Side.ASK)\n",
    "depth_df = pd.DataFrame({\n",
    "    'bid_price':  [p/10_000 for p,_ in bid_levels],\n",
    "    'bid_volume': [v        for _,v in bid_levels],\n",
    "    'ask_price':  [p/10_000 for p,_ in ask_levels],\n",
    "    'ask_volume': [v        for _,v in ask_levels],\n",
    "})\n",
    "depth_df"
   ]
  },
  {
   "cell_type": "code",
   "execution_count": null,
   "metadata": {},
   "outputs": [],
   "source": [
    "# Final snapshot\n",
    "snap = s.snapshot()\n",
    "print(f\"Best bid:     ${snap['best_bid']/10_000:.4f}  (volume: {snap['best_bid_volume']:,})\")\n",
    "print(f\"Best ask:     ${snap['best_ask']/10_000:.4f}  (volume: {snap['best_ask_volume']:,})\")\n",
    "print(f\"Order count:  {snap['order_count']:,}\")\n",
    "print(f\"Checksum:     {snap['checksum']}\")"
   ]
  }
 ],
 "metadata": {
  "kernelspec": {"display_name": "Python 3", "language": "python", "name": "python3"},
  "language_info": {"name": "python", "version": "3.12.0"}
 },
 "nbformat": 4,
 "nbformat_minor": 5
}
```

Save this JSON to `python/examples/replay_demo.ipynb`.

- [ ] **Step 5: Create `ci/build-python-bindings.yml`**

```bash
mkdir -p /Users/kolli/systems/orderbook/ci
```

```yaml
# ci/build-python-bindings.yml
name: Python Bindings

on:
  push:
    branches: [main]
  pull_request:
    branches: [main]

jobs:
  build-and-test:
    runs-on: ubuntu-latest

    steps:
      - uses: actions/checkout@v4

      - name: Set up Python
        uses: actions/setup-python@v5
        with:
          python-version: "3.12"

      - name: Install Python dependencies
        run: |
          pip install pybind11 pytest

      - name: Generate mock ITCH data
        run: python scripts/gen_mock_itch.py --output data/mock.NASDAQ_ITCH50 --events 50000

      - name: Configure CMake
        run: |
          cmake -B build-py \
            -DCMAKE_BUILD_TYPE=Release \
            -DORDERBOOK_PYTHON=ON \
            -Dpybind11_DIR=$(python -c "import pybind11; print(pybind11.get_cmake_dir())")

      - name: Build Python module
        run: cmake --build build-py --target orderbook -j$(nproc)

      - name: Validate module loads
        run: PYTHONPATH=build-py python -c "import orderbook; print(orderbook.__doc__)"

      - name: Run Python smoke tests
        run: |
          PYTHONPATH=build-py \
          ITCH_FILE=data/mock.NASDAQ_ITCH50 \
          pytest tests/python/ -v --tb=short
```

- [ ] **Step 6: Final smoke test end-to-end**

```bash
cd /Users/kolli/systems/orderbook
PYTHONPATH=build-py ITCH_FILE=data/mock.NASDAQ_ITCH50 \
  .venv/bin/pytest tests/python/ -v
```

Expected: all tests PASS.

- [ ] **Step 7: Commit everything**

```bash
git add benchmarks/py_replay_bench.py \
        python/README.md \
        python/examples/replay_demo.ipynb \
        ci/build-python-bindings.yml
git commit -m "feat: add Python benchmark, notebook demo, README, and CI job"
```

---

## Self-Review

**Spec coverage:**

| Spec requirement | Task |
|---|---|
| `IEventSource` interface | Task 1 |
| `FeedHandler::step(Event&)` book-agnostic | Task 2 |
| `OrderBook::top_n_levels`, `best_bid_level`, `best_ask_level` | Task 3 |
| `ReplaySession` + `ItchEventSource` + `make_itch_source()` | Task 4 |
| `CsvEventSource` with `CsvColumnMap` | Task 5 |
| `ParquetEventSource` with Arrow RecordBatch cursor | Task 6 |
| `ORDERBOOK_PYTHON` CMake option | Task 7 |
| `py::enum_<Event::Type>` and `py::enum_<Side>` | Task 7 |
| `PyReplaySession` with GIL management | Task 7 |
| `load_replay` with explicit extension detection | Task 7 |
| `register_sink` with tuple (no dict allocation) | Task 7 |
| `PySinkState` owned by `unique_ptr`, session non-copyable | Task 7 |
| Python smoke tests | Task 8 |
| Benchmark script with `--debug` flag | Task 9 |
| `python/README.md` with build + API docs | Task 9 |
| `python/examples/replay_demo.ipynb` | Task 9 |
| CI with import validation before pytest | Task 9 |
| Determinism invariant documented | Task 9 (README) |

**Type consistency check:**
- `make_itch_source` declared in `replay_session.hpp`, defined in `replay_session.cpp`, called in `python_bindings.cpp` ✓
- `SinkFn = void(*)(const Event&, void*)` used in `replay_session.hpp` and `python_bindings.cpp` ✓
- `Snapshot` struct fields match `snapshot()` dict keys in bindings ✓
- `query_top_n(int n, Side side)` in both header and bindings ✓
- `top_n_levels(int n, char side, LevelEntry*, int&)` internal; `query_top_n` translates `Side` → `'B'`/`'S'` ✓

**Placeholder scan:** No TBDs. All code blocks are complete. Commands include expected output. ✓
