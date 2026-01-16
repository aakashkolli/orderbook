#pragma once
#include <cstdint>
#include <memory>

#include "checksum.h"
#include "hash_map.h"
#include "order.h"
#include "price_level.h"
#include "slab_allocator.h"
#include "types.h"

// Single-symbol limit order book with O(1) add, execute, cancel, delete,
// replace.
//
// Price ladder: fixed-range tick array indexed by absolute price tick.
//   levels_[price_tick] -> PriceLevel (both bid and ask share one array;
//   NASDAQ prevents crossing so bid and ask never coexist at the same tick).
//
// Order lookup: Robin Hood open-addressing hash map (order_ref -> Order*).
//
// Memory: all Order objects allocated from a SlabAllocator; price ladder
//   and hash map both backed by mmap. Zero heap allocation after construction.
class OrderBook {
 public:
  static constexpr uint32_t MAX_TICKS = 1u << 20;  // ~$104 at $0.0001/tick

  explicit OrderBook(uint32_t slab_capacity = SlabAllocator::DEFAULT_CAPACITY,
                     uint64_t map_capacity = 1u << 21);
  ~OrderBook() noexcept;

  OrderBook(const OrderBook&) = delete;
  OrderBook& operator=(const OrderBook&) = delete;

  // --- Hot path: event handlers ---

  void add_order(OrderRef ref, Price price, Quantity qty, char side,
                 Timestamp ts) noexcept;

  // execute_order: `qty` shares executed at the resting order's price.
  void execute_order(OrderRef ref, Quantity qty, Timestamp ts) noexcept;

  // execute_order_price: `qty` shares executed at `exec_price`
  // (price-improved).
  void execute_order_price(OrderRef ref, Quantity qty, Price exec_price,
                           Timestamp ts) noexcept;

  // cancel_order: cancel `qty` shares; `qty` is the CANCELLED amount.
  // The order remains live with (original_qty - qty) shares if > 0.
  void cancel_order(OrderRef ref, Quantity qty_cancelled,
                    Timestamp ts) noexcept;

  void delete_order(OrderRef ref, Timestamp ts) noexcept;

  void replace_order(OrderRef orig_ref, OrderRef new_ref, Price new_price,
                     Quantity new_qty, Timestamp ts) noexcept;

  // --- Query ---

  [[nodiscard]] Price best_bid() const noexcept;
  [[nodiscard]] Price best_ask() const noexcept;
  [[nodiscard]] uint64_t total_volume() const noexcept { return total_volume_; }
  [[nodiscard]] uint32_t order_count() const noexcept { return lookup_.size(); }
  [[nodiscard]] uint64_t event_count() const noexcept { return event_count_; }
  [[nodiscard]] uint64_t checksum() const noexcept {
    return checksum_.digest();
  }

  // --- Depth queries (research path; not hot-path) ---
  // BestLevel: result of a single best-price lookup (best_bid_level /
  // best_ask_level). LevelEntry: one element in a depth snapshot
  // (top_n_levels). Identical fields; separate types to distinguish
  // single-level vs array-of-levels call sites.
  struct BestLevel {
    Price price;
    uint64_t volume;
    uint32_t order_count;
  };
  struct LevelEntry {
    Price price;
    uint64_t volume;
    uint32_t order_count;
  };

  // Single lookup: no double call to best_bid()/best_ask() + level().
  [[nodiscard]] BestLevel best_bid_level() const noexcept;
  [[nodiscard]] BestLevel best_ask_level() const noexcept;

  // Fills out[0..count-1] with up to n non-empty price levels starting from
  // best. side='B' walks bid ladder downward; side='S' walks ask ladder upward.
  // out must be caller-allocated with capacity >= n. n is clamped to 64
  // internally. NOTE: walks tick-by-tick: O(spread) in worst case, not O(n).
  // Not for hot path.
  void top_n_levels(int n, char side, LevelEntry* out,
                    int& count) const noexcept;

  [[nodiscard]] const PriceLevel& level(Price price) const noexcept {
    return levels_[static_cast<uint32_t>(price) < MAX_TICKS
                       ? static_cast<uint32_t>(price)
                       : 0];
  }

  // Validate structural invariants (debug builds only; no-op in release).
  void validate_invariants() const noexcept;

 private:
  void insert_order_raw(OrderRef ref, Price price, Quantity qty,
                        char side) noexcept;
  void remove_order_internal(Order* o) noexcept;
  void update_best_after_remove(uint32_t tick, char side) noexcept;
  void update_best_after_add(uint32_t tick, char side) noexcept;
  void tick_checksum(Timestamp ts) noexcept;

  PriceLevel* levels_;  // mmap'd array of MAX_TICKS PriceLevels
  SlabAllocator slab_;
  OrderHashMap lookup_;
  ReplayChecksum checksum_;

  uint32_t best_bid_tick_;  // highest tick with live bid orders; 0 = no bids
  uint32_t
      best_ask_tick_;  // lowest tick with live ask orders; MAX_TICKS = no asks
  uint64_t total_volume_;  // sum of remaining shares across all live orders
  uint64_t event_count_;
#ifndef NDEBUG
  Timestamp last_ts_;  // for monotonic timestamp assertion (debug only)
#endif
};
