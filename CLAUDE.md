# Order Book Replay Project (ITCH 5.0)

## Purpose

This project reconstructs a deterministic NASDAQ TotalView-ITCH 5.0 limit order book from historical binary market data. The system is designed to maximize:
- cache efficiency
- deterministic replay correctness
- microarchitectural performance
- memory predictability

It is a systems project intended for quantitative development interview preparation and low-latency C++ systems engineering.

---

## Core Principle Hierarchy

1. Correctness (deterministic replay)
2. Memory safety (no corruption under fuzzing)
3. Performance (latency + throughput)
4. Optimization (cache + branch + memory hierarchy)

Never optimize before correctness is verified.

---

## Hot Path Definition

The hot path is strictly:

- Feed parsing loop
- Event normalization
- Order book update logic

### Forbidden operations in hot path

- std::string
- iostream / logging
- heap allocation (new/malloc)
- virtual dispatch
- exceptions
- shared_ptr / unique_ptr
- locks / mutexes

---

## File Ownership

### Core system
- `src/feed_handler.cpp` → ITCH parsing and mmap traversal
- `src/order_book.cpp` → book state updates
- `src/price_ladder.cpp` → price level management
- `src/slab_allocator.cpp` → order allocation
- `src/hash_map.cpp` → order lookup

### Testing
- `tests/` → unit + integration tests
- `fuzz/` → libFuzzer harnesses

### Benchmarks
- `benchmarks/` → perf + throughput tests
- `scripts/run_perf.sh` → reproducibility runner

---

## Data Model Rules

- All prices are int64 fixed-point (1/10000 dollars)
- No floating point in core logic
- All ITCH fields are big-endian
- All timestamps are monotonic within replay stream assumption

---

## Performance Goals

- ≥ 5M events/sec sustained replay
- p99 latency < 400ns per event
- <2% L1 cache miss rate
- zero heap allocations on hot path

Benchmarks must be pinned, reproducible, and include CPU metadata.

---

## Determinism Requirements

Replay must be bitwise deterministic:
- identical input → identical book state
- identical input → identical output snapshots
- identical input → identical checksum stream

Validation is done via periodic hash of:
- best bid
- best ask
- total volume
- order count

---

## Memory Model

- Slab allocator for all Order objects
- contiguous allocation only
- no fragmentation in replay path
- cache-line aligned structs (64 bytes)

Free list is intrusive (no auxiliary allocations).

---

## Price Ladder Design

Preferred:
- std::vector<PriceLevel> (contiguous)

Avoid in hot path:
- std::map
- std::unordered_map for price levels

Hash maps only allowed for order_ref → pointer lookup.

---

## Testing Strategy

### Unit tests
- ITCH parsing correctness
- order insertion/deletion correctness
- allocator reuse safety

### Integration tests
- full replay determinism
- cross-run consistency

### Fuzz tests
- malformed message streams
- invalid cancels/deletes
- random event sequences

---

## Benchmark Discipline

All benchmarks must include:
- CPU model
- compiler flags
- dataset size
- run configuration
- pinned core info

No benchmark is valid without reproducibility metadata.

---

## Debug vs Benchmark Modes

| Mode | Purpose |
|------|--------|
| Debug | invariants + sanitizers |
| Validate | correctness replay |
| Benchmark | max performance |
| Profile | perf + flamegraphs |

---

## Development Philosophy

- Measure before optimizing
- Prefer contiguous memory over abstraction
- Prefer deterministic behavior over flexibility
- Prefer explicit structures over generic containers
