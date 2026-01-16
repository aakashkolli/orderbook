#pragma once
#include <cassert>

#include "types.h"

// Cache-line-aligned Order. Exactly 64 bytes.
//
// Pointer lifecycle invariant (enforced in debug builds):
//   LIVE: owned by a PriceLevel's intrusive doubly-linked list;
//         next/prev are list pointers.
//   FREE: on the slab free list;
//         next = free-list chain pointer;
//         prev = 0xDEADDEADDEADDEAD (poison).
struct alignas(64) Order {
  OrderRef order_ref;          // 8: NASDAQ order reference number
  Price price;                 // 8: fixed-point $0.0001/unit
  Quantity quantity;           // 4: current remaining shares
  Quantity original_quantity;  // 4: shares at time of ADD
  uint32_t queue_position;     // 4: total shares ahead in queue at add time
  uint8_t side;                // 1: 'B' or 'S'
  uint8_t _pad[3];             // 3

  // Dual-use pointers (see lifecycle invariant above):
  Order* next;  // 8
  Order* prev;  // 8

  uint32_t slab_index;  // 4: index within SlabAllocator backing store
  uint8_t _pad2[12];    // 12

  // Total: 8+8+4+4+4+1+3+8+8+4+12 = 64
};

static_assert(sizeof(Order) == 64, "Order must be exactly one cache line");
static_assert(alignof(Order) == 64, "Order must be cache-line aligned");

#ifndef NDEBUG
static constexpr uintptr_t DEAD_PTR = 0xDEADDEADDEADDEADULL;
#endif
