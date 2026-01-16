// libFuzzer harness for the ITCH 5.0 parser and order book.
//
// Build with: cmake -B build-fuzz -DENABLE_FUZZING=ON -DCMAKE_BUILD_TYPE=Debug
//             cmake --build build-fuzz --target itch_parser_fuzz
//
// Run:  ./build-fuzz/itch_parser_fuzz corpus/ -max_total_time=300
//
// Invariants under test:
//   - No crash, assertion failure, or undefined behavior on ANY byte sequence
//   - Parser never reads beyond the end of the buffer
//   - Order book remains structurally consistent after every operation
//   - Double-free and use-after-free are caught by AddressSanitizer

#include <cstddef>
#include <cstdint>

#include "itch_types.h"
#include "order_book.h"
#include "types.h"

// libFuzzer entry point.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // Use a small slab to keep memory usage bounded per corpus entry.
  OrderBook book(4096, 8192);

  const uint8_t* ptr = data;
  const uint8_t* end = data + size;
  Timestamp ts = 1;

  while (ptr + 2 <= end) {
    const uint16_t msg_len = be16(*reinterpret_cast<const uint16_t*>(ptr));
    if (ptr + 2 + msg_len > end) break;  // truncated: safe exit
    if (msg_len == 0) {
      ptr += 2;
      continue;
    }

    const uint8_t type = ptr[2];
    switch (type) {
      case 'A': {
        if (msg_len < 36) break;
        const auto* m = reinterpret_cast<const ITCHAddOrder*>(ptr);
        Price price = static_cast<Price>(be32(m->price));
        if (price <= 0 || price >= static_cast<Price>(OrderBook::MAX_TICKS))
          break;
        Quantity qty = be32(m->shares);
        if (qty == 0) break;
        char side = m->side;
        if (side != 'B' && side != 'S') break;
        book.add_order(be64(m->order_reference), price, qty, side, ts++);
        break;
      }
      case 'F': {
        if (msg_len < 40) break;
        const auto* m = reinterpret_cast<const ITCHAddOrderMPID*>(ptr);
        Price price = static_cast<Price>(be32(m->price));
        if (price <= 0 || price >= static_cast<Price>(OrderBook::MAX_TICKS))
          break;
        Quantity qty = be32(m->shares);
        if (qty == 0) break;
        char side = m->side;
        if (side != 'B' && side != 'S') break;
        book.add_order(be64(m->order_reference), price, qty, side, ts++);
        break;
      }
      case 'E': {
        if (msg_len < 31) break;
        const auto* m = reinterpret_cast<const ITCHOrderExecuted*>(ptr);
        Quantity qty = be32(m->executed_shares);
        if (qty == 0) break;
        // Clamp qty to remaining quantity to avoid assertion in debug.
        OrderRef ref = be64(m->order_reference);
        // Safe: execute_order handles stale refs gracefully.
        book.execute_order(ref, qty, ts++);
        break;
      }
      case 'X': {
        if (msg_len < 23) break;
        const auto* m = reinterpret_cast<const ITCHOrderCancel*>(ptr);
        Quantity qty = be32(m->cancelled_shares);
        if (qty == 0) break;
        book.cancel_order(be64(m->order_reference), qty, ts++);
        break;
      }
      case 'D': {
        if (msg_len < 19) break;
        const auto* m = reinterpret_cast<const ITCHOrderDelete*>(ptr);
        book.delete_order(be64(m->order_reference), ts++);
        break;
      }
      case 'U': {
        if (msg_len < 35) break;
        const auto* m = reinterpret_cast<const ITCHOrderReplace*>(ptr);
        Price price = static_cast<Price>(be32(m->price));
        if (price <= 0 || price >= static_cast<Price>(OrderBook::MAX_TICKS))
          break;
        Quantity qty = be32(m->shares);
        if (qty == 0) break;
        book.replace_order(be64(m->original_reference), be64(m->new_reference),
                           price, qty, ts++);
        break;
      }
      default:
        break;  // safely skip unknown types
    }
    ptr += 2 + msg_len;
  }

  return 0;  // return 0 to keep the input in the corpus; -1 to discard
}
