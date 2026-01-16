#pragma once
#include "types.h"

// Normalized event produced by the ITCH parser.
// This is the canonical representation passed into the order book.
// All fields are host byte order; prices are int64_t fixed-point
// ($0.0001/unit).
struct Event {
  enum class Type : uint8_t {
    ADD,
    EXECUTE,        // partial or full execution at order's resting price
    EXECUTE_PRICE,  // execution at a different (price-improved) price
    CANCEL,  // partial cancel; quantity field = shares CANCELLED (delta, not
             // remaining)
    DELETE,
    REPLACE,
    UNKNOWN,
  };

  Type type;
  char side;  // 'B' (buy) or 'S' (sell); 0 if not applicable
  uint8_t _pad[2];
  uint16_t stock_locate;   // NASDAQ symbol ID
  Timestamp timestamp_ns;  // nanoseconds from midnight

  OrderRef order_ref;      // primary order reference
  OrderRef new_order_ref;  // for REPLACE: replacement reference; else 0

  Price
      price;  // resting price (fixed-point); for EXECUTE_PRICE: execution price
  Quantity quantity;  // ADD: total shares; EXECUTE: executed shares; CANCEL:
                      // shares cancelled (delta)
};

static_assert(sizeof(Event) <= 64, "Event fits in one cache line");
