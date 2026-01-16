# Design: Python Bindings and Adapter Layer

**Date:** 2026-05-19  
**Status:** Approved  
**Scope:** pybind11 module exposing replay API; CSV and Parquet adapters implementing `IEventSource`

---

## 1. Goals

Add a Python-accessible replay API and modular adapters so the orderbook engine can be used interactively from Python notebooks and can read CSV and Parquet datasets without bespoke scripts.

**Non-goals:**
- Changing the native hot path (`FeedHandler::run()`, `OrderBook` internals)
- Replicating LOBSIM research features (dashboards, multi-book orchestration)
- Peak throughput parity with native benchmarks (usability is the target)

---

## 2. Architecture

```
Python (pybind11 module "orderbook")
        │
        ▼
ReplaySession  (include/orderbook/replay_session.hpp + src/replay_session.cpp)
  owns: unique_ptr<IEventSource>  +  OrderBook
        │
        ▼
IEventSource  (include/orderbook/event_source.hpp)
  ┌──────────────┬─────────────────┬────────────────────┐
  │ItchEventSource│ CsvEventSource  │ ParquetEventSource │
  │(wraps         │ (adapters/      │ (adapters/         │
  │ FeedHandler   │  csv_adapter)   │  parquet_adapter)  │
  │ + cursor)     │                 │                    │
  └──────────────┴─────────────────┴────────────────────┘
        │
        ▼
OrderBook  (unchanged hot path)
  + top_n_levels(n, side) added
  + best_bid_level() / best_ask_level() added
```

`FeedHandler::run()` stays completely untouched for the native benchmark path. `ItchEventSource` adds a cursor member to `FeedHandler` and a new `step(Event&)` method.

---

## 3. C++ Layer

### 3.1 `IEventSource` (`include/orderbook/event_source.hpp`)

```cpp
struct IEventSource {
    virtual bool next(Event& ev) noexcept = 0;
    virtual ~IEventSource() = default;
};

// Side enum used at adapter and Python boundaries only.
// Core Event struct retains char side to avoid touching hot-path types.
enum class Side : uint8_t { BID = 'B', ASK = 'S' };
```

### 3.2 `FeedHandler` additions

One new method and one new member:

```cpp
// Advances to the next recognized ITCH message; fills ev.
// Returns false on EOF or truncated stream.
bool step(Event& ev) noexcept;

private:
    const uint8_t* cursor_;  // promoted from run() local; persists between step() calls
```

`run()` becomes a loop over `step()`. The hot-path batch behavior is preserved; stepping is an alternative entry point.

`step()` is **book-agnostic** — it decodes one message into `Event` and returns. `ReplaySession` applies the event to `OrderBook`. This preserves the clean pipeline:

```
FeedHandler → Event → ReplaySession → OrderBook
```

### 3.3 `ReplaySession` (`include/orderbook/replay_session.hpp`)

Owns `unique_ptr<IEventSource>` and `OrderBook`. Non-copyable.

```cpp
class ReplaySession {
public:
    explicit ReplaySession(std::unique_ptr<IEventSource> source);

    bool step() noexcept;
    Snapshot snapshot() const noexcept;
    std::vector<std::pair<int64_t, int64_t>> top_n_levels(int n, Side side) const noexcept;
    void register_sink(SinkFn fn, void* ctx) noexcept;

private:
    std::unique_ptr<IEventSource>  source_;
    OrderBook                       book_;
    std::unique_ptr<SinkState>      sink_;   // null if no sink registered
    Timestamp                       last_ts_{0};
};
```

**`Snapshot`** — plain struct, no heap:
```cpp
struct Snapshot {
    Price    best_bid;
    uint64_t best_bid_volume;
    Price    best_ask;
    uint64_t best_ask_volume;
    uint64_t checksum;
    Timestamp timestamp_ns;
    uint32_t order_count;
};
```

**`SinkFn`** — C function pointer to avoid `std::function` overhead in the C++ event loop:
```cpp
using SinkFn = void(*)(const Event&, void* ctx);

struct SinkState {
    SinkFn  fn;
    void*   ctx;
};
```

`std::function` wrapping occurs only at the pybind11 boundary, not in the C++ loop.

**`step()` sequence:**
1. Call `source_->next(ev)`; return `false` on EOF
2. Dispatch `ev` to the appropriate `book_.*` method based on `ev.type`
3. If sink registered and `ev.type` is `EXECUTE` or `EXECUTE_PRICE`, call `sink_->fn(ev, sink_->ctx)`
4. Update `last_ts_`
5. Return `true`

### 3.4 `OrderBook` additions

Three new query methods (no changes to hot-path add/execute/cancel/delete/replace):

```cpp
struct BestLevel { Price price; uint64_t volume; uint32_t order_count; };

[[nodiscard]] BestLevel best_bid_level() const noexcept;
[[nodiscard]] BestLevel best_ask_level() const noexcept;

struct LevelEntry { Price price; uint64_t volume; uint32_t order_count; };

// Fills out[0..count-1] with up to n price levels from best inward.
// side='B' walks bid ladder downward; side='S' walks ask upward.
// out must be caller-allocated with capacity >= n.
void top_n_levels(int n, char side, LevelEntry* out, int& count) const noexcept;
```

`best_bid_level()` and `best_ask_level()` do a single lookup: read `best_bid_tick_` (or `best_ask_tick_`), return `levels_[tick].volume` and `levels_[tick].count`. No double lookup.

---

## 4. Adapters

### 4.1 `ItchEventSource` (wraps `FeedHandler`)

Delegates to `FeedHandler::step(Event&)`. Constructed from a file path.

### 4.2 `CsvEventSource` (`adapters/csv_adapter.hpp/.cpp`)

```cpp
struct CsvColumnMap {
    int type = 0, side = 1, price = 2, quantity = 3,
        order_ref = 4, timestamp_ns = 5;
    char delimiter = ',';
};

class CsvEventSource : public IEventSource {
public:
    CsvEventSource(const char* path, CsvColumnMap cols = {});
    bool next(Event& ev) noexcept override;
};
```

**Parsing:** `std::string` per line + `split_sv(std::string_view, char)` helper returning `std::vector<std::string_view>`. No `strtok_r`, no fixed-size buffers. Handles quoted fields and variable-length rows safely.

**Type mapping:** string names (`"ADD"`, `"EXECUTE"`, etc.) → `Event::Type`. Unknown type strings → `Event::Type::UNKNOWN`, `next()` returns `true` (row consumed, event skipped by `ReplaySession`).

**Termination:** `next()` returns `false` on EOF. Asserts no rows remain after returning `false` (catches phantom trailing rows in debug builds).

**Price conversion:** decimal string × 10000 → `int64_t` fixed-point. Overflow-checked in debug builds.

### 4.3 `ParquetEventSource` (`adapters/parquet_adapter.hpp/.cpp`)

Only compiled when `find_package(Arrow)` and `find_package(Parquet)` succeed (`HAVE_ARROW` compile definition). Otherwise, the constructor throws:
```
std::runtime_error("Parquet support not compiled in; rebuild with -DORDERBOOK_PYTHON=ON and Arrow installed")
```

**Internal cursor:**
```cpp
std::shared_ptr<arrow::RecordBatchReader> reader_;
std::shared_ptr<arrow::RecordBatch>       batch_;
int64_t row_idx_{0};
```

`next()` walks `row_idx_` within `batch_`; pulls the next batch via `reader_` when exhausted. Returns `false` when reader is exhausted.

**Expected schema:** columns named `type` (string), `side` (string), `price` (int64 fixed-point), `quantity` (int32/int64), `order_ref` (int64/uint64), `timestamp_ns` (int64). Column name mismatches throw at construction. Batches are read in 65k-row chunks.

---

## 5. Python Bindings (`src/python_bindings.cpp`)

### 5.1 Module structure

```
orderbook (Python module)
├── ReplaySession         — main class
├── load_replay(path)     — factory, auto-detects format
├── Snapshot              — read-only result type
├── EventType             — py::enum_ bound from Event::Type
└── Side                  — py::enum_ bound from Side
```

### 5.2 Enums

```cpp
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
```

Numeric values are owned by the C++ enum definitions. README describes semantics only — no duplication risk.

### 5.3 `ReplaySession` Python class

| Method | Python signature | Notes |
|---|---|---|
| `__init__` | `(path: str, mode: str = "replay")` | `mode` reserved; only `"replay"` supported |
| `step` | `() -> bool` | Releases GIL during native call |
| `snapshot` | `() -> dict` | Returns `Snapshot` fields as Python dict |
| `query_top_n` | `(n: int, side: Side) -> list[tuple[int, int]]` | Accepts `orderbook.Side.BID` / `.ASK` |
| `register_sink` | `(callable) -> None` | Callable receives tuple; see §5.5 |

**`snapshot()` dict shape:**
```python
{
    "best_bid":        500000,         # price tick (int64)
    "best_bid_volume": 1200,           # total volume at best level
    "best_ask":        500100,
    "best_ask_volume": 800,
    "checksum":        "0x3f4a...",    # hex string
    "timestamp_ns":    34202000000000, # uint64
    "order_count":     4231
}
```

### 5.4 GIL strategy

| Call | GIL |
|---|---|
| `step()` | Released via `py::gil_scoped_release` for full duration |
| `snapshot()`, `query_top_n()` | GIL held (< 1 µs) |
| Sink trampoline | Re-acquired via `py::gil_scoped_acquire` inside trampoline |

### 5.5 Sink design

`register_sink(fn)` stores `fn` as `py::object` inside `SinkState`. A static trampoline is registered as the `SinkFn`:

```cpp
static void py_sink_trampoline(const Event& ev, void* ctx) {
    py::gil_scoped_acquire gil;
    auto* state = static_cast<PySinkState*>(ctx);
    state->fn(py::make_tuple(
        static_cast<int>(ev.type),        // int; compare with EventType.X.value
        ev.price,                          // int64
        static_cast<int>(ev.quantity),     // int
        static_cast<Side>(ev.side),        // Side enum; pybind converts to Python Side
        ev.timestamp_ns                    // int
    ));
}
```

Tuple index layout is documented and stable:
`(type_int, price, quantity, side_enum, timestamp_ns)`

Default path passes a `tuple` for speed. Users wrap into `dict` if needed:
```python
s.register_sink(lambda t: fills.append({"type": t[0], "price": t[1], ...}))
```

`PySinkState` is owned by `unique_ptr` inside `ReplaySession`. Session is non-copyable. No dangling pointer risk.

### 5.6 `load_replay` factory

Format detection is exhaustive — no silent fallback:

```cpp
if (ext == ".csv")                         → CsvEventSource
else if (ext == ".parquet")               → ParquetEventSource
else if (ext == ".itch50"
      || ext == ".NASDAQ_ITCH50"
      || ext == ".bin")                   → ItchEventSource
else throw std::runtime_error("Unknown format: " + ext
                               + ". Supported: .csv, .parquet, .itch50, .NASDAQ_ITCH50")
```

---

## 6. File Layout

```
include/orderbook/
    event_source.hpp        — IEventSource, Side enum
    replay_session.hpp      — ReplaySession, Snapshot, SinkFn, SinkState

src/
    python_bindings.cpp     — pybind11 module definition
    replay_session.cpp      — ReplaySession implementation

adapters/
    csv_adapter.hpp/.cpp    — CsvEventSource
    parquet_adapter.hpp/.cpp — ParquetEventSource (HAVE_ARROW guard)

tests/
    test_adapters.cpp       — C++ unit tests for adapters and ReplaySession
    python/
        test_bindings_smoke.py — Python integration tests

benchmarks/
    py_replay_bench.py      — Python-bound throughput sanity check

python/
    examples/
        replay_demo.ipynb   — notebook demo
    README.md               — build instructions, API reference
```

---

## 7. CMake

```cmake
option(ORDERBOOK_PYTHON "Build pybind11 Python module" OFF)
if(ORDERBOOK_PYTHON)
    find_package(pybind11 REQUIRED)

    # orderbook_lib (existing) is the core; pybind module links against it.
    pybind11_add_module(orderbook
        src/python_bindings.cpp
        src/replay_session.cpp
        adapters/csv_adapter.cpp
    )
    target_link_libraries(orderbook PRIVATE orderbook_lib)

    find_package(Arrow QUIET)
    find_package(Parquet QUIET)
    if(Arrow_FOUND AND Parquet_FOUND)
        target_sources(orderbook PRIVATE adapters/parquet_adapter.cpp)
        target_link_libraries(orderbook PRIVATE Arrow::arrow_shared Parquet::parquet_shared)
        target_compile_definitions(orderbook PRIVATE HAVE_ARROW=1)
    endif()
endif()
```

`orderbook_lib` and `orderbook` (pybind module) are separate targets. No object file duplication.

---

## 8. Testing

### 8.1 C++ unit tests (`tests/test_adapters.cpp`)

| Test | Invariant checked |
|---|---|
| `ItchEventSource_Step1000` | Step 1000 events via `ItchEventSource`; assert `book.checksum()` equals checksum from `FeedHandler::run()` on same file after same event count |
| `CsvAdapter_RoundTrip` | Write 5 known `Event` values to temp CSV; read back; assert all fields match exactly; assert `next()` returns `false` immediately after last row; assert total rows consumed == 5 |
| `ParquetAdapter_RoundTrip` | Same as CSV for Parquet; `#ifdef HAVE_ARROW` guard; otherwise `GTEST_SKIP()` |
| `ReplaySession_Step` | Step 1000 events on mock ITCH; assert `order_count > 100`, `best_bid < best_ask`, `checksum != 0`; compare against golden checksum constant for deterministic input |
| `TopNLevels` | Add 20 bid orders at 5 distinct prices; call `top_n_levels(3, 'B', ...)`; assert prices strictly descend; assert each `volume` equals sum of quantities of orders at that tick (cross-checked via `level(price).volume`) |

### 8.2 Python smoke tests (`tests/python/test_bindings_smoke.py`)

```python
def test_step_and_snapshot():
    s = orderbook.load_replay("data/mock_medium.NASDAQ_ITCH50")
    count = 0
    while s.step() and count < 1000:
        count += 1
    assert count == 1000
    snap = s.snapshot()
    assert "best_bid" in snap and "checksum" in snap
    assert snap["order_count"] > 100
    assert snap["best_bid"] < snap["best_ask"]

def test_query_top_n():
    s = orderbook.load_replay("data/mock_medium.NASDAQ_ITCH50")
    for _ in range(500): s.step()
    levels = s.query_top_n(5, orderbook.Side.BID)
    assert len(levels) <= 5
    prices = [p for p, _ in levels]
    assert prices == sorted(prices, reverse=True)

def test_sink():
    s = orderbook.load_replay("data/mock_medium.NASDAQ_ITCH50")
    fills = []
    s.register_sink(lambda t: fills.append(t))
    for _ in range(1000): s.step()
    for t in fills:
        assert t[0] in (orderbook.EventType.EXECUTE.value,
                        orderbook.EventType.EXECUTE_PRICE.value)
```

### 8.3 Benchmark script (`benchmarks/py_replay_bench.py`)

```
python py_replay_bench.py data/mock_medium.NASDAQ_ITCH50 --events 100000 [--debug]
```

Timing output only by default. `--debug` prints `snapshot()`. Snapshot printing excluded from timed region.

---

## 9. CI (`ci/build-python-bindings.yml`)

```yaml
- name: Build Python module
  run: |
    pip install pybind11
    cmake -B build-py -DORDERBOOK_PYTHON=ON \
          -DCMAKE_BUILD_TYPE=Release \
          -Dpybind11_DIR=$(python -c "import pybind11; print(pybind11.get_cmake_dir())")
    cmake --build build-py --target orderbook -j$(nproc)

- name: Validate module loads
  run: PYTHONPATH=build-py python -c "import orderbook; print(orderbook.__doc__)"

- name: Python smoke tests
  run: PYTHONPATH=build-py python -m pytest tests/python/ -v
```

Import validation runs before pytest — catches symbol errors and ABI mismatches early.

---

## 10. Determinism Invariant

> **OrderBook observable state is a deterministic function of the ordered event sequence. All internal randomness sources are eliminated or fixed-seeded, and no memory addresses, timestamps (except event timestamps), or allocator metadata contribute to observable outputs.**

Verified from code:
- Hash map: Murmur3 finalizer on `OrderRef` — pure function, no seed
- Slab allocator: sequential free list, LIFO reuse driven by event order only
- Checksum: FNV-1a with fixed offset `14695981039346656037`, hashing logical values only
- No `std::unordered_map`, no `std::chrono`, no pointer values in any checksum input

Golden checksum assertions in `ReplaySession_Step` are stable across machines and runs.

---

## 11. Known Limitations

- Python-bound `step()` throughput will be significantly lower than native `FeedHandler::run()` (pybind11 boundary overhead ~50–200 ns per call). This is intentional — the Python layer targets usability and reproducibility, not peak throughput.
- Parquet support is optional: tests and CI skip gracefully if Arrow is not installed.
- `mode` parameter on `ReplaySession.__init__` is reserved; only `"replay"` is valid in this implementation.
- `load_replay` format detection is extension-based; files with non-standard extensions must be opened via `ReplaySession` directly with an explicit `IEventSource`.
