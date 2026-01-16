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
    parser.add_argument("--events", type=int, default=None,
                        help="Maximum events to process (default: all)")
    parser.add_argument("--debug", action="store_true",
                        help="Print snapshot every 1000 events during stepping")
    args = parser.parse_args()

    s = orderbook.load_replay(args.path)

    t0 = time.perf_counter()
    count = 0
    while s.step() and (args.events is None or count < args.events):
        count += 1
        if args.debug and count % 1000 == 0:
            snap = s.snapshot()
            print(f"[{count:>8,}] bid={snap['best_bid']} ask={snap['best_ask']} orders={snap['order_count']}")
    elapsed = time.perf_counter() - t0

    print(f"events:     {count:>10,}")
    print(f"elapsed:    {elapsed*1000:>10.1f} ms")
    if elapsed > 0 and count > 0:
        print(f"throughput: {count/elapsed/1e6:>10.3f} M events/sec")
    else:
        print("throughput: N/A (no events processed)")
    print()
    print("NOTE: Python-bound throughput is intentionally lower than native")
    print("      FeedHandler::run() due to pybind11 call overhead (~50-200 ns/call).")

if __name__ == "__main__":
    main()
