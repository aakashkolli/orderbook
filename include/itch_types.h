#pragma once
#include <cstdint>

#include "types.h"

// NASDAQ TotalView-ITCH 5.0 raw message structures.
//
// All fields are big-endian on the wire. Use be16/be32/be64 helpers
// to convert to host byte order before use. The first two bytes of
// every message frame are the 2-byte big-endian length (NOT including
// the length field itself). Byte 2 is the message type.
//
// Frame layout:
//   [len:2 BE] [type:1] [payload...]
//   Next message at ptr + 2 + len.

#pragma pack(push, 1)

struct ITCHAddOrder {
  uint16_t length;       // big-endian; does NOT count this field
  uint8_t message_type;  // 'A'
  uint16_t stock_locate;
  uint16_t tracking_number;
  uint8_t timestamp[6];      // 6-byte BE nanoseconds from midnight
  uint64_t order_reference;  // BE
  char side;                 // 'B' or 'S'
  uint32_t shares;           // BE
  char stock[8];             // space-padded ASCII
  uint32_t price;            // BE; fixed-point $0.0001/unit
};

// 'F': same as ITCHAddOrder but with a 4-byte MPID attribution field appended.
struct ITCHAddOrderMPID {
  uint16_t length;
  uint8_t message_type;  // 'F'
  uint16_t stock_locate;
  uint16_t tracking_number;
  uint8_t timestamp[6];
  uint64_t order_reference;
  char side;
  uint32_t shares;
  char stock[8];
  uint32_t price;
  char attribution[4];  // MPID; ignored in our model
};

struct ITCHOrderExecuted {
  uint16_t length;
  uint8_t message_type;  // 'E'
  uint16_t stock_locate;
  uint16_t tracking_number;
  uint8_t timestamp[6];
  uint64_t order_reference;  // BE
  uint32_t executed_shares;  // BE
  uint64_t match_number;     // BE; ignored
};

struct ITCHOrderExecutedWithPrice {
  uint16_t length;
  uint8_t message_type;  // 'C'
  uint16_t stock_locate;
  uint16_t tracking_number;
  uint8_t timestamp[6];
  uint64_t order_reference;
  uint32_t executed_shares;
  uint64_t match_number;
  char printable;  // 'Y'/'N'
  uint32_t
      execution_price;  // BE; may differ from order price (e.g. price-improve)
};

struct ITCHOrderCancel {
  uint16_t length;
  uint8_t message_type;  // 'X'
  uint16_t stock_locate;
  uint16_t tracking_number;
  uint8_t timestamp[6];
  uint64_t order_reference;
  uint32_t cancelled_shares;  // BE; partial cancel: quantity remaining after
};

struct ITCHOrderDelete {
  uint16_t length;
  uint8_t message_type;  // 'D'
  uint16_t stock_locate;
  uint16_t tracking_number;
  uint8_t timestamp[6];
  uint64_t order_reference;
};

struct ITCHOrderReplace {
  uint16_t length;
  uint8_t message_type;  // 'U'
  uint16_t stock_locate;
  uint16_t tracking_number;
  uint8_t timestamp[6];
  uint64_t original_reference;  // BE; order being replaced
  uint64_t new_reference;       // BE; new order_ref
  uint32_t shares;              // BE; new quantity
  uint32_t price;               // BE; new price
};

#pragma pack(pop)

// Size assertions against NASDAQ ITCH 5.0 spec.
static_assert(sizeof(ITCHAddOrder) == 2 + 36, "Add Order struct size");
static_assert(sizeof(ITCHAddOrderMPID) == 2 + 40, "Add Order MPID struct size");
static_assert(sizeof(ITCHOrderExecuted) == 2 + 31,
              "Order Executed struct size");
static_assert(sizeof(ITCHOrderExecutedWithPrice) == 2 + 36,
              "Order Executed w/Price struct size");
static_assert(sizeof(ITCHOrderCancel) == 2 + 23, "Order Cancel struct size");
static_assert(sizeof(ITCHOrderDelete) == 2 + 19, "Order Delete struct size");
static_assert(sizeof(ITCHOrderReplace) == 2 + 35, "Order Replace struct size");
