#include <benchmark/benchmark.h>

#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "feed_handler.h"
#include "order_book.h"
#include "types.h"

// ---------------------------------------------------------------------------
// Micro-benchmark: raw order book operations
// ---------------------------------------------------------------------------

// Add N orders then delete all of them. Measures add+delete throughput.
static void BM_AddDelete(benchmark::State& state) {
  const int64_t N = state.range(0);
  OrderBook book(static_cast<uint32_t>(N) + 16, static_cast<uint64_t>(N) * 4);

  // Pre-generate order data to eliminate RNG overhead from measured path.
  std::vector<OrderRef> refs(N);
  std::vector<Price> prices(N);
  std::mt19937_64 rng(42);
  for (int64_t i = 0; i < N; ++i) {
    refs[i] = static_cast<OrderRef>(i + 1);
    prices[i] = static_cast<Price>(450000 + (rng() % 100000));
  }

  for (auto _ : state) {
    Timestamp ts = 1;
    for (int64_t i = 0; i < N; ++i) {
      book.add_order(refs[i], prices[i], 100, (i & 1) ? 'S' : 'B', ts++);
    }
    for (int64_t i = 0; i < N; ++i) {
      book.delete_order(refs[i], ts++);
    }
    benchmark::DoNotOptimize(book.order_count());
  }
  state.SetItemsProcessed(state.iterations() * N * 2);
}
BENCHMARK(BM_AddDelete)->Arg(1000)->Arg(10000)->Arg(100000);

// Execute N orders sequentially (full-fill each one).
static void BM_Execute(benchmark::State& state) {
  const int64_t N = state.range(0);

  for (auto _ : state) {
    state.PauseTiming();
    OrderBook book(static_cast<uint32_t>(N) + 16, static_cast<uint64_t>(N) * 4);
    Timestamp ts = 1;
    for (int64_t i = 1; i <= N; ++i)
      book.add_order(static_cast<OrderRef>(i), 500000, 100, 'B', ts++);
    state.ResumeTiming();

    for (int64_t i = 1; i <= N; ++i)
      book.execute_order(static_cast<OrderRef>(i), 100, ts++);
    benchmark::DoNotOptimize(book.order_count());
  }
  state.SetItemsProcessed(state.iterations() * N);
}
BENCHMARK(BM_Execute)->Arg(1000)->Arg(10000)->Arg(100000);

// Hash map: insert + lookup + erase cycle.
static void BM_HashMap(benchmark::State& state) {
  const int64_t N = state.range(0);
  OrderHashMap map(static_cast<uint64_t>(N) * 4);

  alignas(64) static Order dummy;
  dummy.order_ref = 1;

  for (auto _ : state) {
    for (int64_t i = 1; i <= N; ++i)
      map.insert(static_cast<OrderRef>(i), &dummy);
    for (int64_t i = 1; i <= N; ++i)
      benchmark::DoNotOptimize(map.find(static_cast<OrderRef>(i)));
    for (int64_t i = 1; i <= N; ++i) map.erase(static_cast<OrderRef>(i));
  }
  state.SetItemsProcessed(state.iterations() * N * 3);
}
BENCHMARK(BM_HashMap)->Arg(1000)->Arg(10000)->Arg(100000);

// ---------------------------------------------------------------------------
// Full ITCH file replay (BM_ProcessITCH: the primary benchmark)
// Reads path from ITCH_FILE env var; skips if not set.
// ---------------------------------------------------------------------------

static void BM_ProcessITCH(benchmark::State& state) {
  const char* path = std::getenv("ITCH_FILE");
  if (!path) {
    state.SkipWithMessage(
        "ITCH_FILE not set; use: ITCH_FILE=data/mock.NASDAQ_ITCH50 "
        "./bench_orderbook");
    return;
  }

  FeedHandler feed(path);
  if (!feed.valid()) {
    state.SkipWithMessage("ITCH_FILE could not be opened");
    return;
  }

  for (auto _ : state) {
    OrderBook book;
    uint64_t events = feed.run(book);
    benchmark::DoNotOptimize(events);
    state.counters["events"] = static_cast<double>(events);
  }

  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(state.counters["events"]));
}
BENCHMARK(BM_ProcessITCH)->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();
