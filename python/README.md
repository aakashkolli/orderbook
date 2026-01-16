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
cmake --build build-py --target orderbook -j$(nproc 2>/dev/null || sysctl -n hw.logicalcpu)
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
orderbook.EventType.ADD            # ADD
orderbook.EventType.EXECUTE        # EXECUTE
orderbook.EventType.EXECUTE_PRICE  # EXECUTE_PRICE
orderbook.EventType.CANCEL         # CANCEL
orderbook.EventType.DELETE         # DELETE
orderbook.EventType.REPLACE        # REPLACE
```

## Performance note

Python-bound `step()` throughput is intentionally lower than native
`FeedHandler::run()` due to the pybind11 call overhead (~50-200 ns per call).
Typical measured range: **0.5–2 M events/sec** from Python (hardware-dependent).
The Python layer targets usability and reproducibility, not peak throughput.

Run the benchmark: `python benchmarks/py_replay_bench.py <file>`

## Determinism

> OrderBook observable state is a deterministic function of the ordered event
> sequence. All internal randomness sources are eliminated or fixed-seeded, and
> no memory addresses, timestamps (except event timestamps), or allocator
> metadata contribute to observable outputs.

`snapshot()["checksum"]` is stable across runs for identical input.

## Notebook example

See `examples/replay_demo.ipynb` for an interactive walkthrough.
