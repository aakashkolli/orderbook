#pragma once
#include <cstdint>

#include "types.h"

// FNV-1a 64-bit rolling checksum for deterministic replay validation.
// Periodically hashed into the checksum: (best_bid, best_ask, total_volume,
// order_count). Two independent replays of the same input MUST produce
// identical checksum sequences.
class ReplayChecksum {
 public:
  static constexpr uint64_t FNV_OFFSET = 14695981039346656037ULL;
  static constexpr uint64_t FNV_PRIME = 1099511628211ULL;
  static constexpr uint32_t HASH_INTERVAL = 10000;  // hash every N events

  ReplayChecksum() noexcept : hash_(FNV_OFFSET), event_count_(0) {}

  // Ingest a snapshot of book state.
  void update(Price best_bid, Price best_ask, uint64_t volume,
              uint32_t order_count) noexcept {
    mix(static_cast<uint64_t>(best_bid));
    mix(static_cast<uint64_t>(best_ask));
    mix(volume);
    mix(static_cast<uint64_t>(order_count));
    ++event_count_;
  }

  // Called after each event; triggers a snapshot hash every HASH_INTERVAL
  // events. Returns true when an update was applied (for testing periodic
  // behavior).
  bool tick(Price best_bid, Price best_ask, uint64_t volume,
            uint32_t order_count) noexcept {
    ++event_count_;
    if (__builtin_expect(event_count_ % HASH_INTERVAL == 0, 0)) {
      mix(static_cast<uint64_t>(best_bid));
      mix(static_cast<uint64_t>(best_ask));
      mix(volume);
      mix(static_cast<uint64_t>(order_count));
      return true;
    }
    return false;
  }

  [[nodiscard]] uint64_t digest() const noexcept { return hash_; }
  [[nodiscard]] uint64_t event_count() const noexcept { return event_count_; }

  void reset() noexcept {
    hash_ = FNV_OFFSET;
    event_count_ = 0;
  }

 private:
  void mix(uint64_t val) noexcept {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&val);
    for (int i = 0; i < 8; ++i) {
      hash_ ^= p[i];
      hash_ *= FNV_PRIME;
    }
  }

  uint64_t hash_;
  uint64_t event_count_;
};
