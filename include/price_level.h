#pragma once
#include <cassert>
#include <cstdint>

#include "order.h"

// Cache-line-aligned price level holding an intrusive doubly-linked list
// of Orders at this price. Exactly 64 bytes.
//
// Orders are maintained in price-time priority (FIFO within a price level):
//   head = earliest order (fills first)
//   tail = most recent order (fills last)
struct alignas(64) PriceLevel {
  Order* head;                   // 8: front of queue (next to execute)
  Order* tail;                   // 8: back of queue (last added)
  uint64_t volume;               // 8: total remaining shares across all orders
  uint64_t cumulative_executed;  // 8: total shares executed at this level
                                 // (monotone)
  uint32_t count;                // 4: number of live orders
  uint8_t _pad[28];              // 28

  // Total: 8+8+8+8+4+28 = 64

  [[nodiscard]] bool empty() const noexcept { return head == nullptr; }

  // Append order to back of queue (new orders have lowest priority at their
  // price).
  void push_back(Order* o) noexcept {
    assert(o != nullptr);
    o->next = nullptr;
    o->prev = tail;
    if (__builtin_expect(tail != nullptr, 1))
      tail->next = o;
    else
      head = o;
    tail = o;
    volume += o->quantity;
    ++count;
  }

  // Unlink order from anywhere in the queue.
  void unlink(Order* o) noexcept {
    assert(o != nullptr);
    assert(count > 0);
    assert(volume >= o->quantity);
    if (o->prev)
      o->prev->next = o->next;
    else
      head = o->next;
    if (o->next)
      o->next->prev = o->prev;
    else
      tail = o->prev;
    volume -= o->quantity;
    --count;
  }
};

static_assert(sizeof(PriceLevel) == 64,
              "PriceLevel must be exactly one cache line");
static_assert(alignof(PriceLevel) == 64,
              "PriceLevel must be cache-line aligned");
