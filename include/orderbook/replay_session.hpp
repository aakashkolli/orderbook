// include/orderbook/replay_session.hpp
#pragma once
#include <memory>
#include <utility>
#include <vector>

#include "order_book.h"
#include "orderbook/event_source.hpp"
#include "types.h"

using SinkFn = void (*)(const Event&, void* ctx);

struct SinkState {
  SinkFn fn;
  void* ctx;
};

struct Snapshot {
  Price best_bid;
  uint64_t best_bid_volume;
  Price best_ask;
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

  // Not noexcept: the sink callable (if registered) may throw — e.g., Python
  // exceptions propagated through the pybind11 trampoline. Callers should not
  // assume step() is safe to call from a noexcept context when a sink is
  // active.
  bool step();
  Snapshot snapshot() const noexcept;

  // Returns [(price_tick, volume), ...], up to n levels from best inward.
  // May throw std::bad_alloc if the result vector cannot be allocated.
  std::vector<std::pair<int64_t, int64_t>> query_top_n(int n, Side side) const;

  // Replaces any previously registered sink. The previous sink's ctx pointer
  // is not notified before it is dropped — caller is responsible for ctx
  // lifetime.
  void register_sink(SinkFn fn, void* ctx);

 private:
  std::unique_ptr<IEventSource> source_;
  OrderBook book_;
  std::unique_ptr<SinkState> sink_;
  Timestamp last_ts_{0};
};

// Factory — creates an ItchEventSource. Defined in replay_session.cpp.
std::unique_ptr<IEventSource> make_itch_source(const char* path);
