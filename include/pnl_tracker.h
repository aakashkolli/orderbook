#pragma once
#include <cstdint>
#include <vector>

#include "types.h"

// Execution PnL tracker for simulated orders.
// Computes realized PnL from fill events using fixed-point arithmetic.
// All prices are int64_t ($0.0001/unit); PnL is in the same units.
class PnLTracker {
 public:
  struct Fill {
    Timestamp ts;
    Price price;  // execution price
    Quantity quantity;
    char side;  // 'B' buy or 'S' sell
    uint8_t _pad[3];
  };

  void record_fill(Timestamp ts, Price price, Quantity qty, char side);

  // Realized PnL in $0.0001 units.
  // Positive = profit. Only valid for flat position (equal buy/sell volume).
  [[nodiscard]] int64_t realized_pnl() const noexcept { return realized_pnl_; }

  // Total buy volume in shares.
  [[nodiscard]] uint64_t buy_volume() const noexcept { return buy_volume_; }
  [[nodiscard]] uint64_t sell_volume() const noexcept { return sell_volume_; }
  [[nodiscard]] uint32_t fill_count() const noexcept {
    return static_cast<uint32_t>(fills_.size());
  }

  // Average buy price in fixed-point units (0 if no buys).
  [[nodiscard]] Price avg_buy_price() const noexcept;
  [[nodiscard]] Price avg_sell_price() const noexcept;

  [[nodiscard]] const std::vector<Fill>& fills() const noexcept {
    return fills_;
  }

  void reset() noexcept;

 private:
  std::vector<Fill> fills_;
  int64_t realized_pnl_ = 0;
  uint64_t buy_volume_ = 0;
  uint64_t sell_volume_ = 0;
  int64_t buy_notional_ = 0;  // sum(price * qty) for buys
  int64_t sell_notional_ = 0;
};
