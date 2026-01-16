#include "slab_allocator.h"

#include <sys/mman.h>

#include <cassert>
#include <cstdint>

SlabAllocator::SlabAllocator(uint32_t capacity)
    : capacity_(capacity), live_(0) {
  assert(capacity > 0);
  size_t bytes = static_cast<size_t>(capacity) * sizeof(Order);
  base_ = reinterpret_cast<Order*>(mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  assert(base_ != MAP_FAILED && "slab mmap failed");

  // Build intrusive free list through all slots.
  // Each slot's `next` points to the next slot; last slot's `next` = nullptr.
  for (uint32_t i = 0; i < capacity - 1; ++i) {
    base_[i].next = &base_[i + 1];
    base_[i].slab_index = i;
#ifndef NDEBUG
    base_[i].prev = reinterpret_cast<Order*>(DEAD_PTR);
#else
    base_[i].prev = nullptr;
#endif
  }
  base_[capacity - 1].next = nullptr;
  base_[capacity - 1].slab_index = capacity - 1;
#ifndef NDEBUG
  base_[capacity - 1].prev = reinterpret_cast<Order*>(DEAD_PTR);
#else
  base_[capacity - 1].prev = nullptr;
#endif

  free_head_ = &base_[0];
}

SlabAllocator::~SlabAllocator() noexcept {
  if (base_ != MAP_FAILED)
    munmap(base_, static_cast<size_t>(capacity_) * sizeof(Order));
}

Order* SlabAllocator::allocate() noexcept {
  assert(free_head_ != nullptr && "slab exhausted — increase capacity");
  Order* o = free_head_;
  free_head_ = o->next;

#ifndef NDEBUG
  // Verify the order was on the free list (prev == DEAD_PTR).
  assert(reinterpret_cast<uintptr_t>(o->prev) == DEAD_PTR &&
         "slab corruption: allocating non-free slot");
  o->prev = nullptr;  // clear poison before use
#endif

  ++live_;
  return o;
}

void SlabAllocator::free(Order* o) noexcept {
  assert(o != nullptr);
  assert(o >= base_ && o < base_ + capacity_ && "order not from this slab");

#ifndef NDEBUG
  // Detect double-free: live orders have prev != DEAD_PTR.
  assert(reinterpret_cast<uintptr_t>(o->prev) != DEAD_PTR &&
         "double-free detected in slab");
  o->prev = reinterpret_cast<Order*>(DEAD_PTR);
#else
  o->prev = nullptr;
#endif

  o->next = free_head_;
  free_head_ = o;
  --live_;
}
