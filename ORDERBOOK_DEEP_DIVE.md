# NASDAQ ITCH 5.0 Order Book Engine — Deep Dive

---

## Executive Summary

This project is a **single-symbol limit order book engine** that replays historical NASDAQ TotalView-ITCH 5.0 binary market data at production-grade throughput. Written in C++17/20, it prioritizes correctness first and performance second — the two reinforce each other through a zero-allocation hot path, cache-line-sized data structures, and mmap-backed memory.

The system achieves ~7M events/sec on Apple M-series (ARM64) in a single thread, with zero heap allocations after construction and bitwise-deterministic output across independent runs. A pybind11 Python module exposes the engine for research and backtesting.

| Property | Value |
|---|---|
| Protocol | NASDAQ TotalView-ITCH 5.0 |
| Language | C++20 (core), Python 3 (bindings/analysis) |
| Throughput | ≥5M events/sec (target), ~7M observed on M-series |
| Memory model | mmap-backed; zero heap on hot path |
| Determinism | Bitwise: identical input → identical FNV-1a checksum |
| Price representation | `int64_t` fixed-point, $0.0001/unit |
| Thread model | Single-threaded; no locks |

---

## System Architecture

```
Binary ITCH File (mmap'd)
        │
        ▼
  ┌─────────────┐
  │ FeedHandler │  ← mmap + MADV_SEQUENTIAL; zero-copy cursor walk
  └──────┬──────┘
         │ dispatch() per message type → calls OrderBook methods directly
         │ step()     per message type → fills Event struct (book-agnostic)
         ▼
  ┌──────────────┐          ┌───────────────────┐
  │  OrderBook   │◄─────────│  ReplaySession    │  ← Python-facing abstraction
  │              │          │  (IEventSource)   │
  │ price_ladder │          └───────────────────┘
  │ (mmap array) │
  │              │          ┌──────────────┐
  │ hash_map     │          │ CsvAdapter   │  ← alternative event sources
  │ (mmap RH map)│          │ ParquetAdapter│
  │              │          └──────────────┘
  │ slab_alloc   │
  │ (mmap pool)  │
  │              │
  │ checksum     │  ← FNV-1a rolling, ticked every 10k events
  └──────────────┘
         │
         ▼
  ┌──────────────────────┐
  │  FillSimulator       │  ← queue-position fill model (research path)
  │  PnLTracker          │  ← fixed-point PnL from fills
  └──────────────────────┘
         │
         ▼
  Python module (pybind11) → Jupyter notebooks / analysis scripts
```

There are two replay modes:

- **Batch (`FeedHandler::run`)**: walks the entire mmap'd file in one tight loop, dispatching directly into `OrderBook`. Fastest path.
- **Step (`FeedHandler::step` / `ReplaySession::step`)**: one event at a time, produces a normalized `Event` struct. Used by the Python API and for research workflows.

---

## Trading System Context

### What ITCH 5.0 is

NASDAQ TotalView-ITCH 5.0 is the raw binary feed that carries the full order-by-order history of every limit order submitted, modified, or executed on the NASDAQ exchange. Unlike consolidated trade feeds (e.g., CTA/SIP), ITCH delivers every Add, Cancel, Execute, and Delete event with nanosecond timestamps. Subscribers can reconstruct the exact state of the order book at any point in time.

### Why order book reconstruction matters

A reconstructed order book gives you:
- **Best bid/offer (BBO)** and full depth at every nanosecond
- **Queue position** for passive orders — how many shares are ahead
- **Fill probability estimation** — when does your order execute?
- **Microstructure signals** — price impact, order flow imbalance, spread dynamics

This engine reconstructs the book for a single symbol (filtered by `stock_locate` ID), which is the standard approach for single-stock backtesting.

### Price encoding

All prices on ITCH are 4-byte big-endian unsigned integers representing **cents of a cent**: `$50.00 = 500000`. This engine stores them as `int64_t` (called `Price`) throughout — never floating-point. The `int64_t` encoding prevents the silent precision loss that would arise from casting ITCH `uint32_t` prices to `double` in intermediate steps.

---

## Directory Walkthrough

```
orderbook/
├── include/
│   ├── types.h              ← primitive types, BE helpers, timestamp decoder
│   ├── order.h              ← Order struct (64B, cache-line aligned)
│   ├── price_level.h        ← PriceLevel struct (64B, intrusive FIFO list)
│   ├── slab_allocator.h     ← fixed-capacity mmap slab for Orders
│   ├── hash_map.h           ← Robin Hood open-addressing map (OrderRef→Order*)
│   ├── checksum.h           ← FNV-1a rolling determinism checksum
│   ├── event.h              ← normalized Event struct (protocol-agnostic)
│   ├── itch_types.h         ← raw ITCH wire structs (#pragma pack)
│   ├── feed_handler.h       ← mmap ITCH parser declaration
│   ├── order_book.h         ← OrderBook declaration
│   ├── fill_simulator.h     ← queue-position fill model
│   ├── pnl_tracker.h        ← fixed-point PnL accumulator
│   └── orderbook/
│       ├── event_source.hpp ← IEventSource abstract interface
│       └── replay_session.hpp ← ReplaySession (step-mode, Python-facing)
│
├── src/
│   ├── feed_handler.cpp     ← mmap open, dispatch loop, step() cursor
│   ├── order_book.cpp       ← all event handlers + best-price tracking
│   ├── slab_allocator.cpp   ← mmap slab init + alloc/free
│   ├── fill_simulator.cpp   ← fill detection from cumulative exec
│   ├── pnl_tracker.cpp      ← fill recording + PnL math
│   ├── replay_session.cpp   ← ReplaySession + ItchEventSource
│   ├── python_bindings.cpp  ← pybind11 module + format-detection factory
│   └── main_replay.cpp      ← CLI: replay binary, print BBO + checksum
│
├── adapters/
│   ├── csv_adapter.{hpp,cpp}     ← CSV event source
│   └── parquet_adapter.{hpp,cpp} ← Arrow/Parquet event source (optional)
│
├── benchmarks/
│   ├── bench_orderbook.cpp       ← Google Benchmark: add/delete/execute/hashmap/ITCH
│   ├── py_replay_bench.py        ← Python-side throughput measurement
│   └── results_2026-05-18.txt    ← recorded results with hardware metadata
│
├── fuzz/
│   └── itch_parser_fuzz.cpp      ← libFuzzer harness (parser + book)
│
├── tests/
│   ├── test_book_invariants.cpp  ← GoogleTest: invariants, components, E2E
│   ├── fuzz_book.cpp             ← structured-random stress test
│   └── test_adapters.cpp         ← adapter-level tests
│
├── scripts/
│   ├── gen_mock_itch.py          ← deterministic synthetic ITCH generator
│   └── run_perf.sh               ← reproducibility runner (perf stat + pinning)
│
└── CMakeLists.txt                ← five build types, optional pybind11/Arrow
```

---

## Core Data Structures

### Order (64 bytes, cache-line aligned)

```cpp
struct alignas(64) Order {
    OrderRef order_ref;      // 8 — NASDAQ reference number
    Price    price;          // 8 — fixed-point $0.0001/unit
    Quantity quantity;       // 4 — remaining shares
    Quantity original_quantity; // 4 — shares at ADD time
    uint32_t queue_position; // 4 — shares ahead at entry
    uint8_t  side;           // 1 — 'B' or 'S'
    uint8_t  _pad[3];        // 3
    Order*   next;           // 8 — dual-use: live=list ptr, free=free-list ptr
    Order*   prev;           // 8 — dual-use: live=list ptr, free=0xDEADDEAD...
    uint32_t slab_index;     // 4
    uint8_t  _pad2[12];      // 12
};                           // = 64 bytes exactly
```

The `next`/`prev` pair is **dual-use**: when live, they form the intrusive doubly-linked list within a `PriceLevel`; when freed, `next` is the slab free-list chain and `prev` is set to `0xDEADDEADDEADDEAD` as a debug poison. This eliminates the need for any auxiliary free-list nodes.

The 64-byte alignment means each `Order` occupies exactly one cache line. When the CPU loads an order from the slab, it gets all fields in a single cache-line fetch.

### PriceLevel (64 bytes, cache-line aligned)

```cpp
struct alignas(64) PriceLevel {
    Order*   head;                // 8 — front of queue (executes first)
    Order*   tail;                // 8 — back of queue (executes last)
    uint64_t volume;              // 8 — total remaining shares
    uint64_t cumulative_executed; // 8 — total shares ever executed here
    uint32_t count;               // 4 — number of live orders
    uint8_t  _pad[28];            // 28
};                                // = 64 bytes exactly
```

Orders within a level are in **price-time priority (FIFO)**: `head` is always the next order to execute, `tail` is the most recently added. `push_back` is O(1); `unlink` is O(1) with pointer arithmetic. The level tracks `cumulative_executed` monotonically — this feeds the `FillSimulator`'s queue-position model.

### Price Ladder

The price ladder is a **flat array** of `1<<20` (~1M) `PriceLevel` structs, mmap'd contiguously:

```cpp
levels_ = mmap(nullptr, (1<<20) * sizeof(PriceLevel), PROT_READ|PROT_WRITE,
               MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
```

Price tick → array index is a direct cast: `levels_[static_cast<uint32_t>(price)]`. This is O(1) with no hash computation. The array covers prices from 0 to $104.8575 in $0.0001 increments, which covers the vast majority of US equities in normal conditions.

The tradeoff: 64 MB of virtual address space at initialization. The OS maps this lazily (demand paging), so physical pages are only faulted in for price ticks that actually appear in the replay — sparse books pay only for levels actually touched.

### SlabAllocator

```
mmap region: [Order0][Order1][Order2]...[OrderN-1]
                │         │
                └──next───┘  ← intrusive free list
```

All `Order` objects live in a single contiguous mmap'd block. Construction builds the free list through all slots via `base_[i].next = &base_[i+1]`. `allocate()` pops from `free_head_`; `free()` pushes back. Both are O(1) pointer swaps, no branching except the debug assertions.

Debug builds set `prev = 0xDEADDEADDEADDEAD` on free and assert it's set before allocating — this catches both double-free (freeing something still poisoned) and use-after-free (accessing a poisoned order).

Default capacity: `1<<20` (~1M orders), configurable at construction.

### OrderHashMap (Robin Hood open-addressing)

```
Slot { OrderRef key; Order* value; uint32_t dist; uint32_t _pad; }  // 24 bytes
```

Hash function: Murmur3 finalizer (`ref ^= ref >> 33; ref *= 0xff51...; ref ^= ref >> 33`). ITCH order references are sequential integers starting from 1 — Murmur3's avalanche property destroys that sequential structure, giving good bucket distribution.

Robin Hood invariant: when inserting, if the incoming entry has traveled further from its ideal bucket than the slot's current occupant, steal the slot and continue inserting the displaced entry. On lookup, early termination when `dist > slot.dist` — anything that should have displaced this entry would have done so during insertion.

Deletion uses **backward shift** (no tombstones): after erasing a key, shift subsequent entries backward until hitting an empty slot or a slot at dist=0 (already at its ideal bucket). This keeps lookup performance stable under heavy delete traffic.

Load factor: capped at 0.70, enforced by assertion in debug builds. Default capacity `1<<21` (2M slots) for a 1M-order workload.

---

## Critical Execution Paths

### Hot Path: Add Order

```
book.add_order(ref, price, qty, side, ts)
│
├── assert(price > 0 && price < MAX_TICKS)
├── assert(ts >= last_ts_)   [debug only]
│
├── tick = static_cast<uint32_t>(price)
├── PriceLevel& lvl = levels_[tick]       ← O(1) direct array index
│
├── Order* o = slab_.allocate()            ← pop free_head_; O(1)
├── o->order_ref = ref
├── o->queue_position = lvl.volume         ← shares ahead at entry time
│
├── lvl.push_back(o)                       ← O(1) tail pointer update
├── total_volume_ += qty
├── lookup_.insert(ref, o)                 ← Robin Hood insert; ~O(1)
├── update_best_after_add(tick, side)      ← single comparison; O(1)
│
└── tick_checksum(ts)                      ← FNV-1a tick; O(1) amortized
```

Every operation is O(1) or constant-time amortized. The only non-constant case is `update_best_after_remove`, which scans the price ladder when the best level empties — O(spread) worst case, but typical spread on liquid stocks is < 10 ticks.

### Hot Path: Execute Order

```
book.execute_order(ref, qty, ts)
│
├── Order* o = lookup_.find(ref)          ← Robin Hood probe; ~O(1)
├── if (o == nullptr) return;             ← stale ref: safe no-op
│
├── levels_[o->price].cumulative_executed += qty  ← monotone counter
│
├── if (qty == o->quantity)
│   └── remove_order_internal(o)         ← unlink + erase + slab.free
└── else
    ├── o->quantity -= qty
    ├── levels_[tick].volume -= qty
    └── total_volume_ -= qty
```

The `__builtin_expect(o == nullptr, 0)` hint on the stale-ref branch tells the compiler the common case is a valid lookup, keeping the fast path straight-line.

### Replace Order

Replace = delete old + add new. The implementation does exactly that:
1. `remove_order_internal(old)` — erase from hash map, unlink from level, free to slab
2. `add_order(new_ref, new_price, new_qty, side, ts)` — full add logic

Crucially: the replacement loses queue priority. It goes to the back of the new price level's FIFO queue, even if the new price equals the old price. This is correct ITCH semantics — a replace is treated as a new order.

Event count: `add_order` increments `event_count_` itself, so `replace_order` decrements after calling it to avoid double-counting one Replace as two events.

### Best Bid/Ask Tracking

Two scalar fields — `best_bid_tick_` and `best_ask_tick_` — track the best prices. Updates:
- **After add**: single comparison, no scan
- **After remove**: scan only when the current best level becomes empty

```cpp
// After removing from tick:
if (tick == best_bid_tick_ && levels_[tick].empty()) {
    while (best_bid_tick_ > 0 && levels_[best_bid_tick_].empty())
        --best_bid_tick_;
}
```

This scan is the only amortized-O(spread) operation in the system. For liquid equities with tight spreads (<5 ticks), this is effectively O(1) in practice.

---

## Parsing and Event Flow

### ITCH Frame Layout

```
┌──────────┬──────┬──────────────┬───────────────────────────────────┐
│ len (2B) │ type │ stock_locate │ tracking_num │ timestamp[6] │ ... │
│  BE u16  │ (1B) │   (2B BE)   │   (2B BE)    │   (6B BE)    │     │
└──────────┴──────┴──────────────┴───────────────────────────────────┘
          ↑
          ptr points here; next message at ptr + 2 + len
```

The length field does NOT include its own 2 bytes. `run()` advances by `2 + msg_len` after each message.

### Byte Extraction Pattern

Rather than casting the raw pointer to a packed struct (UBSan/UBND hazard on ARM), the parser uses `load_be*` helpers backed by `memcpy`:

```cpp
[[nodiscard]] inline uint32_t load_be32(const void* p) noexcept {
    uint32_t v; std::memcpy(&v, p, 4); return be32(v);
}
```

`memcpy` of a small known size compiles to a single load instruction on x86/ARM. It's semantically correct for unaligned access and eliminates UBSan false positives.

### Timestamp Decoding

ITCH timestamps are 6-byte big-endian nanoseconds from midnight. The decoder:

```cpp
inline Timestamp decode_timestamp(const uint8_t ts[6]) noexcept {
    uint64_t ns = 0;
    for (int i = 0; i < 6; ++i) ns = (ns << 8) | ts[i];
    return ns;
}
```

This is the only loop on the parsing path. The compiler unrolls it (6 iterations, fixed bounds). Maximum encodable time: `(1<<48) - 1` ns ≈ 3.2 days. The 6-byte encoding fits a full trading session (nanoseconds from midnight, ~23 hours = ~82.8 trillion ns, fits in 6 bytes since `2^48 ≈ 281 trillion`).

### Message Type Dispatch

```
FeedHandler::dispatch()
    switch (ptr[2]) {
    'A' → add_order()
    'F' → add_order()         // MPID variant; MPID field ignored
    'E' → execute_order()
    'C' → execute_order_price() // price-improved execution
    'X' → cancel_order()      // partial cancel (delta, not remaining)
    'D' → delete_order()
    'U' → replace_order()
    default → ++skipped_
    }
```

The `'C'` (Execute With Price) case records the execution price but treats the book state identically to a standard execution — the resting order was placed at its original price, and the execution price affects realized PnL but not book depth. `execute_order_price` forwards directly to `execute_order`.

### Two Replay APIs

| API | Use Case | Coupling |
|-----|----------|---------|
| `FeedHandler::run(book)` | Maximum throughput; batch | Tight; calls book methods directly |
| `FeedHandler::step(ev)` | Event-by-event; research | Loose; fills `Event` struct |

`step()` is **book-agnostic**: it decodes ITCH bytes into a normalized `Event` without touching any `OrderBook` state. This enables the `ReplaySession` abstraction: any `IEventSource` (ITCH, CSV, Parquet) produces `Event` objects, and `ReplaySession::step()` dispatches them to `OrderBook`.

---

## Memory and Allocation Strategy

### Three mmap'd regions

| Region | Size | Type | Purpose |
|---|---|---|---|
| Price ladder | `1<<20 × 64B = 64MB` | `PriceLevel[]` | Tick-indexed book depth |
| Slab | `1<<20 × 64B = 64MB` | `Order[]` | All live order objects |
| Hash map | `1<<21 × 24B = 48MB` | `Slot[]` | `OrderRef → Order*` |

Total virtual address space at construction: ~176MB. Physical pages are demand-paged; a sparse book with 10,000 live orders and a 100-tick spread touches only ~640KB of physical RAM for the price ladder.

### Why mmap instead of new[]?

1. **Zero-initialization**: `MAP_ANONYMOUS` returns zeroed pages. `PriceLevel::head = nullptr` is correct from the start; `OrderHashMap::Slot::key = 0 = EMPTY_KEY` is correct from the start. No memset required.
2. **No heap fragmentation**: `mmap` allocates from virtual address space directly; the slab heap stays flat. After initial construction, `malloc` is never called on the hot path.
3. **NUMA awareness**: On Linux, anonymous mmap pages are allocated from the local NUMA node when first touched. Touching all pages at startup from the pinned core gets local pages.
4. **Huge page eligibility**: Large contiguous mmap'd regions can be backed by 2MB huge pages via `madvise(MADV_HUGEPAGE)`, cutting TLB pressure.

### Free list bootstrap

The slab constructor walks all `N` slots once in order:
```cpp
for (uint32_t i = 0; i < capacity - 1; ++i)
    base_[i].next = &base_[i + 1];
base_[capacity - 1].next = nullptr;
free_head_ = &base_[0];
```
After this, `allocate()` and `free()` are pure pointer chases — no arithmetic, no bounds checks in release.

### Object lifetime invariant

```
LIVE state:  owned by PriceLevel list; lookup_ maps ref → ptr
FREE state:  on slab free list; prev = 0xDEADDEAD (debug)

Transitions:
  add_order   → slab.allocate() → lvl.push_back() → lookup_.insert()
  delete/execute/cancel → lookup_.erase() → lvl.unlink() → slab.free()
```

There is no shared ownership. Every `Order*` in the system is either live (in exactly one level's list and the hash map) or free (on the slab free list). The invariant check in `validate_invariants()` walks all live levels and cross-checks total count against `lookup_.size()`.

---

## Performance Engineering

### Cache Line Strategy

Both `Order` (64B) and `PriceLevel` (64B) are exactly one cache line. This has two effects:

1. **No false sharing**: Two adjacent `PriceLevel` entries for different price ticks occupy different cache lines — updating one doesn't invalidate the other.
2. **One load, all fields**: Reading `o->quantity` after loading `o` from the slab brings the entire order into L1. Subsequent accesses to `o->price`, `o->side` are cache hits.

### Branch Hints

Every branch on the "failure" path (stale ref, truncated message, file invalid) uses `__builtin_expect(condition, 0)`:

```cpp
if (__builtin_expect(o == nullptr, 0)) return;
if (__builtin_expect(ptr + 2 + msg_len > end, 0)) break;
```

The compiler arranges the common-case code in the fall-through path, keeping the instruction cache hot.

### MADV_SEQUENTIAL

```cpp
::madvise(data_, size_, MADV_SEQUENTIAL);
```

Tells the kernel's read-ahead unit to aggressively prefetch pages in sequential order. For a 43MB ITCH file, this keeps the page cache warm one or two pages ahead of the parser cursor, hiding the disk-to-memory latency.

### Fixed-point arithmetic

Integer prices eliminate all floating-point instructions from the hot path. Price comparison, addition, and subtraction are native 64-bit integer operations. There is no FPU state transition on the order processing path.

### Zero-allocation hot path

After construction, the sequence `add_order → execute_order → delete_order` allocates nothing from the heap. The verified allocation profile (from the benchmark results file):
- Orders from `SlabAllocator` (mmap'd, pre-allocated)
- Price ladder from mmap
- Hash map from mmap
- No `new`/`malloc` calls in the replay loop

### Benchmark Results (2026-05-18)

| Metric | Value |
|---|---|
| Hardware | Apple M-series ARM64, macOS |
| Dataset | 1,497,704 synthetic events (~43MB) |
| Avg throughput | ~7.0M events/sec |
| Target | ≥5M events/sec |
| Determinism | checksum identical across 3 runs ✓ |
| Heap allocs (hot path) | 0 ✓ |

Formal p99 latency measurements require pinned Linux cores with `SCHED_FIFO`. The reference target is p99 < 400ns per event.

### Build Types

| Type | Flags | Purpose |
|---|---|---|
| Debug | `-O0 -g -fsanitize=address,undefined` | Catch bugs; slow |
| Release | `-O3 -march=native -DNDEBUG` | Production replay |
| Benchmark | `-O3 -march=native -DNDEBUG` | Performance measurement |
| Validate | `-O2 -DNDEBUG -DREPLAY_VALIDATE` | Correctness replay with extra checks |
| Profile | `-O2 -g -fno-omit-frame-pointer` | `perf` / flamegraph |

`NDEBUG` strips all `assert()` calls, including the debug checksum assertions, timestamp monotonicity checks, and slab poison validation.

---

## Design Tradeoffs

### Flat tick array vs. tree/map for price ladder

| | Flat array | std::map |
|---|---|---|
| Level access | O(1) direct index | O(log N) |
| Best-bid update after add | O(1) compare | O(log N) |
| Best-bid update after empty | O(spread) scan | O(log N) |
| Memory | 64MB virtual (sparse) | Fragmented heap |
| Cache behavior | Sequential access | Pointer-chasing |

The flat array wins on every dimension except static memory reservation. For US equities below $104 and with tick spreads measured in single digits, the O(spread) scan is bounded in practice.

### Robin Hood vs. other hash maps

- **vs. `std::unordered_map`**: no heap allocation per element, no chaining, no separate bucket array. Robin Hood packs all data in one flat array.
- **vs. linear probing**: Robin Hood bounds the variance in probe distance. The early-termination property (`dist > slot.dist`) makes lookup as fast as linear probing in the hit case but faster in the miss case.
- **vs. hopscotch/cuckoo**: simpler deletion (backward shift vs. rehashing); no worst-case O(N) resize.

### Intrusive list vs. index-based list for orders

Using `Order*` pointers (not indices) in the intrusive list means no slab bounds arithmetic per traversal. The slab index is stored in `slab_index` for debugging but is never read on the hot path.

### Single-symbol vs. multi-symbol book

This implementation uses a single `OrderBook` instance per invocation. Multi-symbol support would require either (a) a `std::vector<OrderBook>` indexed by `stock_locate`, or (b) filter at the feed handler level. The current design filters via `stock_locate_filter` in `run()`, and individual OrderBook instances don't know about `stock_locate` at all — the correct separation of concerns.

### Cancel semantics: delta vs. absolute

ITCH 5.0 cancel (`'X'`) carries `cancelled_shares` — the amount being cancelled, not the remaining quantity. This is semantically important: the engine stores the **delta** interpretation, not the remaining quantity. A cancel event with `qty_cancelled >= o->quantity` is treated as a full delete (safe fallback, not a bug). The comment in `cancel_order` makes this explicit.

---

## Determinism Assumptions

The FNV-1a checksum (`ReplayChecksum`) makes determinism testable and verifiable:

```
Every 10,000 events:
  mix(best_bid) | mix(best_ask) | mix(total_volume) | mix(order_count)
```

For two replays to produce identical checksums:
1. Events must arrive in identical order (guaranteed by sequential mmap traversal)
2. All event handlers must be pure functions of the book state
3. No PRNG, no wall clock, no OS-dependent behavior on the hot path
4. Timestamps are recorded, not generated — the stream is the source of truth

The checksum is published via `book.checksum()` and printable via `./replay file --checksum`. The test `DeterministicChecksum` verifies identical state across two independently-constructed books fed the same event sequence.

### What would break determinism?

- Any `rand()` or `std::mt19937` call on the hot path
- Hash maps with non-deterministic bucket ordering (e.g., `std::unordered_map` with address-randomization-based hashing on MSVC)
- Undefined behavior (signed overflow, unaligned access) — the current `load_be*` / memcpy pattern prevents this
- Processing events from multiple threads without strict ordering

---

## Hidden Details and Edge Cases

### Add Order MPID (`'F'`) is silently the same as `'A'`

The 4-byte Market Participant ID is appended after the price field in `ITCHAddOrderMPID` but is **ignored**. The dispatch in `feed_handler.cpp` falls into the same `add_order` call. This is documented in the code but easy to miss when extending the parser.

### Execute With Price (`'C'`) doesn't change book price

When a resting order executes at a price-improved price (e.g., a buy resting at $50.00 gets filled at $50.05 due to midpoint matching), ITCH sends a `'C'` message. The book state is unchanged — the order still rested at $50.00. Only the PnL is affected by the execution price. `execute_order_price` captures `exec_price` in its signature but passes it as `/*exec_price*/` in the body comment, explicitly ignoring it for book state.

### Replace loses queue priority

`replace_order` calls `add_order(new_ref, ...)` which does `lvl.push_back(o)` — appending to the back of the price level's FIFO queue. Even if the replacement is at the same price, the order loses its position. This matches ITCH 5.0 semantics exactly.

### `best_bid_tick_ = 0` is the "no bids" sentinel

Tick 0 corresponds to price $0.00 — a valid price in the fixed-point encoding but nonsensical for an equity. The sentinel is: `best_bid_tick_ == 0 → NO_BID`. Similarly `best_ask_tick_ == MAX_TICKS → NO_ASK`. This means the first valid bid must be at tick ≥ 1, enforced by the `assert(price > 0)` in `add_order`.

### `top_n_levels` is O(spread), not O(n)

The depth query walks the price ladder tick-by-tick from the best price inward. For a book with a 1,000-tick spread and only 5 non-empty levels in that range, querying top-5 scans 1,000 array entries. This is explicitly called out as "not for hot path" — it's fine for Python research workflows at 1Hz, not for production signal calculation.

### Timestamp monotonicity is debug-only

The `ts >= last_ts_` assertion compiles out in release (`NDEBUG`). In production replay, a non-monotonic timestamp from a malformed ITCH file would silently proceed. This is intentional — correctness assertions are for development; replay throughput doesn't pay for runtime checks.

### Slab crash safety

If the process crashes after `slab_.allocate()` but before `lookup_.insert()`, the order lives in the slab but is invisible to the hash map. There is no recovery mechanism. The design is explicitly stateless: recovery requires replaying from the beginning of the ITCH file. This is always possible because ITCH files are immutable historical records.

### `fuzz/itch_parser_fuzz.cpp` uses a small slab (4096 orders)

The libFuzzer harness caps the slab at 4096 to bound per-corpus-entry memory. Without this, a fuzzer-generated file with 1M adds and no deletes would exhaust the 1M-slot default slab.

---

## Rebuild From Scratch Notes

If starting over, these are the decisions to make in order, and the reasoning that leads to the same design:

1. **Fix-point prices, not floats**: ITCH prices are already integers. Floating-point adds precision risk. `int64_t` with known scale ($0.0001) avoids all that and makes comparison exact.

2. **Flat tick array for levels**: For equities below ~$100, the direct-index array gives O(1) access with zero hash overhead. 64MB of virtual space is free. The alternative (sorted BST or hash map on prices) adds complexity with no throughput benefit.

3. **One cache line per Order, one per PriceLevel**: Any struct that fits in 64B should be padded to exactly 64B to avoid false sharing and guarantee a single-line load. Static asserts enforce this at compile time.

4. **Intrusive list for order queues**: Doubly-linked list with `next`/`prev` inside `Order`. No external node allocation. Splice/unlink is pointer arithmetic.

5. **Slab allocator, not arena, not general heap**: Slab gives O(1) alloc/free with reuse. Arena doesn't free. General heap (glibc `malloc`) adds lock overhead and fragmentation. The intrusive free list lets the `Order` struct carry both states with no extra memory.

6. **Robin Hood open-addressing for order lookup**: O(1) amortized insert/find/delete. Better cache behavior than chaining. No heap per entry. Murmur3 for sequential integer keys.

7. **mmap everything large**: Zero initialization, no fragmentation, NUMA-aware, huge-page eligible.

8. **FNV-1a for the rolling checksum**: Simple, fast, no dependencies. The checksum is not for security (no adversary can craft matching inputs) — it's for detecting non-determinism between runs.

9. **Two replay APIs (batch + step)**: Batch for throughput; step for composability with the Python layer and research workflows.

10. **`IEventSource` for data format abstraction**: Once you have a step-mode API, abstracting the source format is minimal work that unlocks CSV and Parquet inputs for backtesting without touching the core engine.

---

## Resume Bullet Drafts

---

### Draft A — Systems/Performance Focus

**NASDAQ ITCH 5.0 Order Book Engine** | C++20, pybind11, CMake

- Implemented mmap-based zero-copy ITCH 5.0 parser replaying ~7M events/sec on a single thread with zero heap allocations after construction
- Designed cache-line-aligned price ladder (flat tick array) and Robin Hood hash map, both mmap-backed, achieving O(1) add/execute/cancel/delete on the hot path
- Built intrusive slab allocator for 1M+ concurrent order objects with O(1) alloc/free and debug double-free detection via pointer poisoning
- Validated bitwise-deterministic replay across independent runs using a rolling FNV-1a checksum over best-bid, best-ask, volume, and order count

---

### Draft B — Market Data/Quant Focus

**Limit Order Book Reconstructor** | C++20, Python (pybind11)

- Reconstructed NASDAQ ITCH 5.0 order book from binary market data feed, processing 1.5M events in under 200ms with FNV-1a checksum-verified determinism
- Implemented queue-position fill simulator modeling passive order execution probability: fill triggers when cumulative volume at price exceeds queue position plus order size
- Exposed full replay engine to Python via pybind11 with auto-detected ITCH/CSV/Parquet inputs; enables event-by-event backtesting with nanosecond timestamps
- Enforced fixed-point arithmetic ($0.0001/unit) throughout core pipeline eliminating float precision errors in order matching and PnL calculation

---

### Draft C — Low-Latency/Interview Concise

**Low-Latency Order Book** | C++20

- Replay engine for NASDAQ TotalView-ITCH 5.0 achieving 7M events/sec via mmap zero-copy parsing, 64B cache-line-aligned structs, and zero hot-path heap allocation
- Custom Robin Hood open-addressing hash map (Murmur3, backward-shift deletion, 0.7 load factor) for O(1) order reference lookup across 1M+ concurrent orders
- Five CMake build modes (Debug/Release/Benchmark/Validate/Profile); libFuzzer harness with AddressSanitizer/UBSan verifies parser safety on arbitrary byte sequences
- Delivered bitwise-deterministic replay: identical input produces identical 64-bit checksum across any number of independent runs

---

### Draft D — Most Concise (30 words max per bullet)

**Order Book Replay Engine** | C++20, pybind11

- Built NASDAQ ITCH 5.0 parser + limit order book processing 7M events/sec; zero heap allocation on hot path via mmap slab and Robin Hood hash map
- Designed 64-byte cache-line-aligned Order and PriceLevel structs with intrusive doubly-linked FIFO queues; O(1) add, execute, cancel, delete, replace
- Verified bitwise determinism across runs using rolling FNV-1a checksum; hardened against malformed input via libFuzzer + AddressSanitizer harness
- Exposed C++ engine to Python via pybind11 with ITCH/CSV/Parquet adapters; queue-position fill simulator and fixed-point PnL tracker for backtesting

---
