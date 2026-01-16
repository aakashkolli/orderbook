#pragma once
#include <cstddef>

#include "order.h"

// Fixed-capacity slab allocator for Order objects.
//   - Contiguous mmap'd backing store (no heap allocation after construction)
//   - O(1) allocate and free via intrusive singly-linked free list
//   - Cache-line-aligned Order objects (64 bytes each)
//   - Debug: double-free detection via prev-pointer poisoning
//   - Zero allocations on hot path after initial setup
class SlabAllocator {
 public:
  static constexpr uint32_t DEFAULT_CAPACITY = 1u << 20;  // ~1M orders

  explicit SlabAllocator(uint32_t capacity = DEFAULT_CAPACITY);
  ~SlabAllocator() noexcept;

  // Not copyable or movable (owns mmap region).
  SlabAllocator(const SlabAllocator&) = delete;
  SlabAllocator& operator=(const SlabAllocator&) = delete;

  // Pop an Order from the free list. Aborts (assert) if exhausted.
  [[nodiscard]] Order* allocate() noexcept;

  // Push an Order back to the free list.
  // Debug: asserts the order is currently live (prev != DEAD_PTR).
  void free(Order* o) noexcept;

  [[nodiscard]] uint32_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] uint32_t live() const noexcept { return live_; }
  [[nodiscard]] uint32_t available() const noexcept {
    return capacity_ - live_;
  }

 private:
  Order* base_;       // mmap'd block of `capacity_` Order objects
  Order* free_head_;  // head of intrusive free list
  uint32_t capacity_;
  uint32_t live_;  // number of currently allocated orders
};
