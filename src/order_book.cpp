#include "order_book.h"

#include <sys/mman.h>

#include <cassert>
#include <cstring>

OrderBook::OrderBook(uint32_t slab_capacity, uint64_t map_capacity)
    : slab_(slab_capacity),
      lookup_(map_capacity),
      best_bid_tick_(0),
      best_ask_tick_(MAX_TICKS),
      total_volume_(0),
      event_count_(0)
#ifndef NDEBUG
      ,
      last_ts_(0)
#endif
{
  size_t bytes = static_cast<size_t>(MAX_TICKS) * sizeof(PriceLevel);
  levels_ =
      reinterpret_cast<PriceLevel*>(mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  assert(levels_ != MAP_FAILED && "price ladder mmap failed");
  // mmap gives zero-initialized pages -> all PriceLevels start empty ✓
}

OrderBook::~OrderBook() noexcept {
  if (levels_ != MAP_FAILED)
    munmap(levels_, static_cast<size_t>(MAX_TICKS) * sizeof(PriceLevel));
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

void OrderBook::update_best_after_add(uint32_t tick, char side) noexcept {
  if (side == 'B') {
    if (tick > best_bid_tick_) best_bid_tick_ = tick;
  } else {
    if (tick < best_ask_tick_) best_ask_tick_ = tick;
  }
}

void OrderBook::update_best_after_remove(uint32_t tick, char side) noexcept {
  if (side == 'B') {
    if (tick == best_bid_tick_ && levels_[tick].empty()) {
      // Scan downward for next non-empty bid level.
      while (best_bid_tick_ > 0 && levels_[best_bid_tick_].empty())
        --best_bid_tick_;
    }
  } else {
    if (tick == best_ask_tick_ && levels_[tick].empty()) {
      // Scan upward for next non-empty ask level.
      while (best_ask_tick_ < MAX_TICKS && levels_[best_ask_tick_].empty())
        ++best_ask_tick_;
    }
  }
}

void OrderBook::remove_order_internal(Order* o) noexcept {
  uint32_t tick = static_cast<uint32_t>(o->price);
  char side = static_cast<char>(o->side);
  PriceLevel& lvl = levels_[tick];
  lvl.unlink(o);
  total_volume_ -= o->quantity;
  update_best_after_remove(tick, side);
  lookup_.erase(o->order_ref);
  slab_.free(o);
}

void OrderBook::tick_checksum(Timestamp ts) noexcept {
  (void)ts;
  checksum_.tick(best_bid(), best_ask(), total_volume_, lookup_.size());
}

// ---------------------------------------------------------------------------
// Hot path: event handlers
// ---------------------------------------------------------------------------

void OrderBook::insert_order_raw(OrderRef ref, Price price, Quantity qty,
                                 char side) noexcept {
  uint32_t tick = static_cast<uint32_t>(price);
  PriceLevel& lvl = levels_[tick];

  Order* o = slab_.allocate();
  o->order_ref = ref;
  o->price = price;
  o->quantity = qty;
  o->original_quantity = qty;
  o->queue_position = static_cast<uint32_t>(
      lvl.volume > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(lvl.volume));
  o->side = static_cast<uint8_t>(side);
  o->next = nullptr;
  o->prev = nullptr;

  lvl.push_back(o);
  total_volume_ += qty;
  lookup_.insert(ref, o);
  update_best_after_add(tick, side);
}

void OrderBook::add_order(OrderRef ref, Price price, Quantity qty, char side,
                          Timestamp ts) noexcept {
  assert(price > 0 && price < MAX_TICKS && "price out of range");
  assert(qty > 0 && "zero-quantity add");
  assert(side == 'B' || side == 'S');
#ifndef NDEBUG
  assert(ts >= last_ts_ && "timestamp not monotonically non-decreasing");
  last_ts_ = ts;
#endif

  insert_order_raw(ref, price, qty, side);
  ++event_count_;
  tick_checksum(ts);
}

void OrderBook::execute_order(OrderRef ref, Quantity qty,
                              Timestamp ts) noexcept {
  Order* o = lookup_.find(ref);
  if (__builtin_expect(o == nullptr, 0)) return;  // stale ref (safe skip)

  assert(qty <= o->quantity && "executing more than remaining quantity");

  uint32_t tick = static_cast<uint32_t>(o->price);
  PriceLevel& lvl = levels_[tick];
  lvl.cumulative_executed += qty;

  if (qty == o->quantity) {
    // Full execution: remove from book.
    remove_order_internal(o);
  } else {
    // Partial execution: reduce quantity in place.
    o->quantity -= qty;
    lvl.volume -= qty;
    total_volume_ -= qty;
  }

  ++event_count_;
  tick_checksum(ts);
}

void OrderBook::execute_order_price(OrderRef ref, Quantity qty,
                                    Price /*exec_price*/,
                                    Timestamp ts) noexcept {
  // Treat as a standard execution at the resting price.
  // exec_price is ignored for book state (order was at original resting price).
  execute_order(ref, qty, ts);
}

void OrderBook::cancel_order(OrderRef ref, Quantity qty_cancelled,
                             Timestamp ts) noexcept {
  Order* o = lookup_.find(ref);
  if (__builtin_expect(o == nullptr, 0)) return;

  if (__builtin_expect(qty_cancelled >= o->quantity, 0)) {
    // Cancelling all remaining shares: treat as a full delete.
    remove_order_internal(o);
  } else {
    uint32_t tick = static_cast<uint32_t>(o->price);
    o->quantity -= qty_cancelled;
    levels_[tick].volume -= qty_cancelled;
    total_volume_ -= qty_cancelled;
  }

  ++event_count_;
  tick_checksum(ts);
}

void OrderBook::delete_order(OrderRef ref, Timestamp ts) noexcept {
  Order* o = lookup_.find(ref);
  if (__builtin_expect(o == nullptr, 0)) return;
  remove_order_internal(o);

  ++event_count_;
  tick_checksum(ts);
}

void OrderBook::replace_order(OrderRef orig_ref, OrderRef new_ref,
                              Price new_price, Quantity new_qty,
                              Timestamp ts) noexcept {
  Order* old = lookup_.find(orig_ref);
  if (__builtin_expect(old == nullptr, 0)) return;

  char side = static_cast<char>(old->side);
#ifndef NDEBUG
  assert(ts >= last_ts_ && "timestamp not monotonically non-decreasing");
  last_ts_ = ts;
#endif

  remove_order_internal(old);
  insert_order_raw(new_ref, new_price, new_qty, side);
  ++event_count_;
  tick_checksum(ts);
}

// ---------------------------------------------------------------------------
// Query
// ---------------------------------------------------------------------------

Price OrderBook::best_bid() const noexcept {
  return best_bid_tick_ == 0 ? NO_BID : static_cast<Price>(best_bid_tick_);
}

Price OrderBook::best_ask() const noexcept {
  return best_ask_tick_ == MAX_TICKS ? NO_ASK
                                     : static_cast<Price>(best_ask_tick_);
}

OrderBook::BestLevel OrderBook::best_bid_level() const noexcept {
  Price bid = best_bid();
  if (bid == NO_BID) return {NO_BID, 0, 0};
  const PriceLevel& lvl = levels_[static_cast<uint32_t>(bid)];
  return {bid, lvl.volume, lvl.count};
}

OrderBook::BestLevel OrderBook::best_ask_level() const noexcept {
  Price ask = best_ask();
  if (ask == NO_ASK) return {NO_ASK, 0, 0};
  const PriceLevel& lvl = levels_[static_cast<uint32_t>(ask)];
  return {ask, lvl.volume, lvl.count};
}

void OrderBook::top_n_levels(int n, char side, LevelEntry* out,
                             int& count) const noexcept {
  count = 0;
  if (n <= 0) return;
  if (n > 64) n = 64;  // hard cap: prevents unbounded price-ladder walks
  if (side == 'B') {
    Price bid = best_bid();
    if (bid == NO_BID) return;
    for (uint32_t tick = static_cast<uint32_t>(bid); count < n;) {
      if (!levels_[tick].empty())
        out[count++] = {static_cast<Price>(tick), levels_[tick].volume,
                        levels_[tick].count};
      if (tick == 0) break;
      --tick;
    }
  } else {
    Price ask = best_ask();
    if (ask == NO_ASK) return;
    for (uint32_t tick = static_cast<uint32_t>(ask);
         count < n && tick < MAX_TICKS; ++tick) {
      if (!levels_[tick].empty())
        out[count++] = {static_cast<Price>(tick), levels_[tick].volume,
                        levels_[tick].count};
    }
  }
}

// ---------------------------------------------------------------------------
// Invariant validation (debug builds; no-op in release)
// ---------------------------------------------------------------------------

void OrderBook::validate_invariants() const noexcept {
#ifndef NDEBUG
  // best_bid < best_ask when both sides are present.
  if (best_bid_tick_ > 0 && best_ask_tick_ < MAX_TICKS)
    assert(best_bid_tick_ < best_ask_tick_ && "crossed market detected");

  // best_bid_tick level must be non-empty if > 0.
  if (best_bid_tick_ > 0)
    assert(!levels_[best_bid_tick_].empty() && "best bid level is empty");

  // best_ask_tick level must be non-empty if < MAX_TICKS.
  if (best_ask_tick_ < MAX_TICKS)
    assert(!levels_[best_ask_tick_].empty() && "best ask level is empty");

  // Structural consistency: walk all levels and recount volume/orders.
  uint64_t vol = 0;
  uint32_t cnt = 0;
  for (uint32_t t = 1; t < MAX_TICKS; ++t) {
    const PriceLevel& lvl = levels_[t];
    if (lvl.empty()) continue;
    vol += lvl.volume;
    cnt += lvl.count;
    // Walk the intrusive list and verify linkage.
    const Order* o = lvl.head;
    uint32_t n = 0;
    uint64_t v = 0;
    while (o) {
      assert(static_cast<uint32_t>(o->price) == t && "order in wrong level");
      assert(reinterpret_cast<uintptr_t>(o->prev) != DEAD_PTR &&
             "live order has poisoned prev pointer");
      v += o->quantity;
      ++n;
      o = o->next;
    }
    assert(n == lvl.count && "level count mismatch");
    assert(v == lvl.volume && "level volume mismatch");
  }
  assert(vol == total_volume_ && "total volume mismatch");
  assert(cnt == lookup_.size() && "order count mismatch vs hash map");
#endif
}
