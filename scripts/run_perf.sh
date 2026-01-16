#!/bin/bash
# Pin to core 0, run perf stat
taskset -c 0 perf stat \
    -e L1-dcache-load-misses,L1-dcache-loads,\
       LLC-load-misses,LLC-loads,\
       branch-misses,branches,\
       instructions,cycles \
    -r 5 \
    ./build/bench_orderbook \
    --benchmark_filter=BM_ProcessITCH \
    2>&1 | tee benchmarks/perf_results.txt
