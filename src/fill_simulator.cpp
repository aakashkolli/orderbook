#include "fill_simulator.h"

#include <cassert>

uint32_t FillSimulator::submit(Price price, char side, Quantity qty,
                               uint32_t queue_position,
                               uint64_t cumul_exec_at_entry, Timestamp ts) {
  SimOrder o{};
  o.price = price;
  o.side = side;
  o.quantity = qty;
  o.queue_position = queue_position;
  o.cumul_exec_at_entry = cumul_exec_at_entry;
  o.submitted_ts = ts;
  o.filled = false;
  o.fill_ts = 0;
  o.fill_price = 0;
  orders_.push_back(o);
  return static_cast<uint32_t>(orders_.size() - 1);
}

void FillSimulator::on_execute(Price price, Quantity /*qty*/, Timestamp ts,
                               uint64_t new_cumul_exec) {
  for (SimOrder& o : orders_) {
    if (o.filled) continue;
    if (o.price != price) continue;

    // Fill condition: cumulative executions at this price (since the
    // beginning of time, adjusted for entry baseline) have consumed
    // through our queue position and into our order.
    uint64_t exec_since_entry = new_cumul_exec - o.cumul_exec_at_entry;
    if (exec_since_entry >= static_cast<uint64_t>(o.queue_position) +
                                static_cast<uint64_t>(o.quantity)) {
      o.filled = true;
      o.fill_ts = ts;
      o.fill_price = price;
      ++fill_count_;
    }
  }
}

uint32_t FillSimulator::pending_count() const noexcept {
  uint32_t n = 0;
  for (const SimOrder& o : orders_)
    if (!o.filled) ++n;
  return n;
}
