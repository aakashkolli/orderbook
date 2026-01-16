#include "pnl_tracker.h"

#include <cassert>
#include <numeric>

void PnLTracker::record_fill(Timestamp ts, Price price, Quantity qty,
                             char side) {
  fills_.push_back({ts, price, qty, side, {0, 0, 0}});

  int64_t notional = static_cast<int64_t>(price) * static_cast<int64_t>(qty);
  if (side == 'B') {
    buy_volume_ += qty;
    buy_notional_ += notional;
    realized_pnl_ -= notional;  // buying is a cash outflow
  } else {
    sell_volume_ += qty;
    sell_notional_ += notional;
    realized_pnl_ += notional;  // selling is a cash inflow
  }
}

Price PnLTracker::avg_buy_price() const noexcept {
  if (buy_volume_ == 0) return 0;
  return static_cast<Price>(buy_notional_ / static_cast<int64_t>(buy_volume_));
}

Price PnLTracker::avg_sell_price() const noexcept {
  if (sell_volume_ == 0) return 0;
  return static_cast<Price>(sell_notional_ /
                            static_cast<int64_t>(sell_volume_));
}

void PnLTracker::reset() noexcept {
  fills_.clear();
  realized_pnl_ = 0;
  buy_volume_ = 0;
  sell_volume_ = 0;
  buy_notional_ = 0;
  sell_notional_ = 0;
}
