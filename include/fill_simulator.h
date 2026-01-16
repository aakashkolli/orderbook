#pragma once
#include <cstdint>
#include <vector>

#include "types.h"

// Queue-position fill simulator for passive order execution modeling.
//
// Fill occurs when: cumulative_executed_at_P >= queue_position + order.quantity
//   Stricter than "any trade at price = fill" and avoids systematic
//   overestimation of fill probability in backtesting.
class FillSimulator {
 public:
  struct SimOrder {
    Price price;
    char side;
    uint8_t _pad[3];
    Quantity quantity;
    uint32_t queue_position;  // total shares ahead at time of submission
    uint64_t
        cumul_exec_at_entry;  // cumulative executed at price when submitted
    Timestamp submitted_ts;
    bool filled;
    Timestamp fill_ts;
    Price fill_price;
  };

  // Submit a simulated passive order. queue_position should be the total
  // volume currently resting ahead of this order at `price`.
  uint32_t submit(Price price, char side, Quantity qty, uint32_t queue_position,
                  uint64_t cumul_exec_at_entry, Timestamp ts);

  // Notify the simulator that `qty` shares were executed at `price`.
  // Checks all pending simulated orders at that price for fills.
  void on_execute(Price price, Quantity qty, Timestamp ts,
                  uint64_t new_cumul_exec);

  [[nodiscard]] const SimOrder& order(uint32_t id) const { return orders_[id]; }
  [[nodiscard]] uint32_t pending_count() const noexcept;
  [[nodiscard]] uint32_t fill_count() const noexcept { return fill_count_; }

 private:
  std::vector<SimOrder> orders_;
  uint32_t fill_count_ = 0;
};
