#pragma once
#include <sys/mman.h>

#include <cassert>
#include <cstdint>
#include <cstring>

#include "order.h"

// Robin Hood open-addressing hash map: OrderRef -> Order*.
//   - Open addressing with Robin Hood displacement balancing
//   - Murmur3 finalizer hash (avalanche for sequential keys)
//   - Max load factor 0.7; assert-fails on overflow in debug builds
//   - Preallocated via mmap (no heap allocation after construction)
//   - Capacity must be a power of two (bitmask modulo)
class OrderHashMap {
 public:
  static constexpr float MAX_LOAD = 0.70f;
  static constexpr uint64_t EMPTY_KEY = 0;            // order_refs start from 1
  static constexpr uint32_t EMPTY_DIST = UINT32_MAX;  // sentinel for empty slot

  struct Slot {
    OrderRef key;   // 8: 0 means empty
    Order* value;   // 8
    uint32_t dist;  // 4: probe distance from ideal bucket (Robin Hood)
    uint32_t _pad;  // 4
                    // 24 bytes total
  };

  explicit OrderHashMap(uint64_t capacity = 1u << 21)  // 2M slots
      : capacity_(capacity), size_(0) {
    assert((capacity & (capacity - 1)) == 0 && "capacity must be power of two");
    size_t bytes = capacity * sizeof(Slot);
    slots_ =
        reinterpret_cast<Slot*>(mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    assert(slots_ != MAP_FAILED && "mmap failed for hash map");
    // mmap returns zero-initialized pages; key=0 means EMPTY_KEY ✓
    mask_ = capacity - 1;
  }

  ~OrderHashMap() noexcept {
    if (slots_ != MAP_FAILED) munmap(slots_, capacity_ * sizeof(Slot));
  }

  // Not copyable or movable (owns mmap region).
  OrderHashMap(const OrderHashMap&) = delete;
  OrderHashMap& operator=(const OrderHashMap&) = delete;

  void insert(OrderRef key, Order* value) noexcept {
    assert(key != EMPTY_KEY);
#ifndef NDEBUG
    assert(static_cast<float>(size_) <
               MAX_LOAD * static_cast<float>(capacity_) &&
           "hash map load factor exceeded — increase initial capacity");
#endif
    uint64_t idx = hash(key) & mask_;
    Slot incoming{key, value, 0, 0};

    while (true) {
      Slot& slot = slots_[idx];
      if (slot.key == EMPTY_KEY) {
        slot = incoming;
        ++size_;
        return;
      }
      // Robin Hood: if we've traveled further, steal this slot.
      if (incoming.dist > slot.dist) {
        Slot tmp = slot;
        slot = incoming;
        incoming = tmp;
      }
      idx = (idx + 1) & mask_;
      ++incoming.dist;
    }
  }

  [[nodiscard]] Order* find(OrderRef key) const noexcept {
    assert(key != EMPTY_KEY);
    uint64_t idx = hash(key) & mask_;
    uint32_t dist = 0;

    while (true) {
      const Slot& slot = slots_[idx];
      if (__builtin_expect(slot.key == key, 0)) return slot.value;
      if (slot.key == EMPTY_KEY || dist > slot.dist) return nullptr;
      idx = (idx + 1) & mask_;
      ++dist;
    }
  }

  void erase(OrderRef key) noexcept {
    assert(key != EMPTY_KEY);
    uint64_t idx = hash(key) & mask_;
    uint32_t dist = 0;

    // Locate the key.
    while (true) {
      const Slot& slot = slots_[idx];
      if (slot.key == key) break;
      if (slot.key == EMPTY_KEY || dist > slot.dist) return;  // not found
      idx = (idx + 1) & mask_;
      ++dist;
    }

    // Backward shift deletion: maintains Robin Hood invariant without
    // tombstones.
    while (true) {
      uint64_t next = (idx + 1) & mask_;
      Slot& next_slot = slots_[next];
      if (next_slot.key == EMPTY_KEY || next_slot.dist == 0) {
        slots_[idx] = {EMPTY_KEY, nullptr, 0, 0};
        --size_;
        return;
      }
      slots_[idx] = next_slot;
      --slots_[idx].dist;
      idx = next;
    }
  }

  [[nodiscard]] uint64_t size() const noexcept { return size_; }
  [[nodiscard]] uint64_t capacity() const noexcept { return capacity_; }

 private:
  // Murmur3 finalizer: good avalanche for sequential integer keys.
  [[nodiscard]] static uint64_t hash(OrderRef ref) noexcept {
    ref ^= ref >> 33;
    ref *= 0xff51afd7ed558ccdULL;
    ref ^= ref >> 33;
    return ref;
  }

  Slot* slots_;
  uint64_t capacity_;
  uint64_t mask_;
  uint64_t size_;
};

static_assert(sizeof(OrderHashMap::Slot) == 24, "hash slot must be 24 bytes");
