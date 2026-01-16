// src/replay_session.cpp
#include "orderbook/replay_session.hpp"

#include <algorithm>

#include "event.h"
#include "feed_handler.h"

// ---------------------------------------------------------------------------
// ItchEventSource: thin wrapper around FeedHandler's step() cursor
// ---------------------------------------------------------------------------

class ItchEventSource : public IEventSource {
 public:
  explicit ItchEventSource(const char* path) : feed_(path) {}
  bool next(Event& ev) noexcept override { return feed_.step(ev); }

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

bool ReplaySession::step() {
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
      book_.execute_order_price(ev.order_ref, ev.quantity, ev.price,
                                ev.timestamp_ns);
      break;
    case Event::Type::CANCEL:
      book_.cancel_order(ev.order_ref, ev.quantity, ev.timestamp_ns);
      break;
    case Event::Type::DELETE:
      book_.delete_order(ev.order_ref, ev.timestamp_ns);
      break;
    case Event::Type::REPLACE:
      book_.replace_order(ev.order_ref, ev.new_order_ref, ev.price, ev.quantity,
                          ev.timestamp_ns);
      break;
    default:
      break;
  }

  last_ts_ = ev.timestamp_ns;

  if (sink_ && (ev.type == Event::Type::EXECUTE ||
                ev.type == Event::Type::EXECUTE_PRICE)) {
    sink_->fn(ev, sink_->ctx);
  }

  return true;
}

Snapshot ReplaySession::snapshot() const noexcept {
  auto bid = book_.best_bid_level();
  auto ask = book_.best_ask_level();
  return {bid.price,        bid.volume, ask.price,          ask.volume,
          book_.checksum(), last_ts_,   book_.order_count()};
}

std::vector<std::pair<int64_t, int64_t>> ReplaySession::query_top_n(
    int n, Side side) const {
  constexpr int MAX_LEVELS = 64;
  OrderBook::LevelEntry buf[MAX_LEVELS];
  int count = 0;
  book_.top_n_levels(std::min(n, MAX_LEVELS), side == Side::BID ? 'B' : 'S',
                     buf, count);
  std::vector<std::pair<int64_t, int64_t>> result;
  result.reserve(count);
  for (int i = 0; i < count; ++i)
    result.emplace_back(buf[i].price, static_cast<int64_t>(buf[i].volume));
  return result;
}

void ReplaySession::register_sink(SinkFn fn, void* ctx) {
  sink_ = std::make_unique<SinkState>(SinkState{fn, ctx});
}
