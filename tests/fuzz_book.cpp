// Boundary condition tests that exercise the same paths libFuzzer would stress.
// These are compiled into the GoogleTest binary (not a libFuzzer binary) so
// they run deterministically and report failures as test failures.

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "order_book.h"
#include "types.h"

// ---------------------------------------------------------------------------
// Helper: apply a sequence of operations from a byte buffer.
// This mirrors what a libFuzzer harness does, but without the fuzzer driver.
// ---------------------------------------------------------------------------

static void apply_ops_from_bytes(const uint8_t* data, size_t size,
                                 OrderBook& book) {
  if (!data || size == 0) return;
  const uint8_t* p = data;
  const uint8_t* end = data + size;
  OrderRef next_ref = 1;
  std::vector<OrderRef> live_refs;
  Timestamp ts = 1;

  while (p + 1 <= end) {
    uint8_t op = p[0] % 5;
    ++p;
    ts += 1000;  // ensure monotone timestamps

    switch (op) {
      case 0: {  // ADD
        if (p + 4 > end) goto done;
        uint32_t raw_price = (static_cast<uint32_t>(p[0]) << 8 | p[1]) + 1;
        uint32_t qty = (static_cast<uint32_t>(p[2]) << 8 | p[3]) + 1;
        char side = (p[0] & 1) ? 'B' : 'S';
        p += 4;
        // Keep bids below 500000 and asks above 500100 to avoid crossed
        // markets. Real ITCH streams are never crossed; replaying guarantees
        // this invariant.
        Price price;
        if (side == 'B')
          price = static_cast<Price>(raw_price % 499999 + 1);  // [1, 499999]
        else
          price =
              static_cast<Price>(raw_price % (OrderBook::MAX_TICKS - 500101) +
                                 500101);  // [500101, MAX_TICKS-1]
        OrderRef ref = next_ref++;
        live_refs.push_back(ref);
        book.add_order(ref, price, qty % 10000 + 1, side, ts);
        break;
      }
      case 1: {  // EXECUTE
        if (live_refs.empty()) break;
        if (p + 2 > end) goto done;
        size_t idx = (static_cast<size_t>(p[0]) << 8 | p[1]) % live_refs.size();
        p += 2;
        OrderRef ref = live_refs[idx];
        book.execute_order(ref, 1, ts);  // partial exec of 1 share
        break;
      }
      case 2: {  // DELETE
        if (live_refs.empty()) break;
        if (p + 2 > end) goto done;
        size_t idx = (static_cast<size_t>(p[0]) << 8 | p[1]) % live_refs.size();
        p += 2;
        OrderRef ref = live_refs[idx];
        book.delete_order(ref, ts);
        live_refs.erase(live_refs.begin() + idx);
        break;
      }
      case 3: {  // REPLACE
        if (live_refs.empty()) break;
        if (p + 5 > end) goto done;
        size_t idx = static_cast<size_t>(p[0]) % live_refs.size();
        uint32_t raw_price = (static_cast<uint32_t>(p[1]) << 8 | p[2]) + 1;
        uint32_t qty = (static_cast<uint32_t>(p[3]) << 8 | p[4]) + 1;
        p += 5;
        OrderRef orig_ref = live_refs[idx];
        OrderRef new_ref = next_ref++;
        Price price =
            static_cast<Price>(raw_price % (OrderBook::MAX_TICKS - 1) + 1);
        book.replace_order(orig_ref, new_ref, price, qty % 10000 + 1, ts);
        live_refs[idx] = new_ref;
        break;
      }
      case 4: {  // stale ref operations (must be safe no-ops)
        book.execute_order(0xDEADDEADDEADDEADULL, 1, ts);
        book.delete_order(0xCAFECAFECAFECAFEULL, ts);
        break;
      }
    }
  }
done:;
  book.validate_invariants();
}

// ---------------------------------------------------------------------------
// Boundary tests
// ---------------------------------------------------------------------------

TEST(FuzzBoundary, EmptyBuffer) {
  OrderBook book(1024, 256);
  apply_ops_from_bytes(nullptr, 0, book);
  EXPECT_EQ(book.order_count(), 0u);
}

TEST(FuzzBoundary, AllAdds) {
  OrderBook book(2048, 4096);
  // 500 add operations
  std::vector<uint8_t> ops;
  for (int i = 0; i < 500; ++i) {
    ops.push_back(0);  // ADD op
    ops.push_back(static_cast<uint8_t>(i));
    ops.push_back(static_cast<uint8_t>(i + 1));
    ops.push_back(0);
    ops.push_back(10);
  }
  apply_ops_from_bytes(ops.data(), ops.size(), book);
  book.validate_invariants();
}

TEST(FuzzBoundary, InterleavedAddDelete) {
  OrderBook book(1024, 256);
  std::vector<uint8_t> ops;
  for (int i = 0; i < 100; ++i) {
    // ADD
    ops.push_back(0);
    ops.push_back(static_cast<uint8_t>(i & 0xFF));
    ops.push_back(static_cast<uint8_t>((i + 50) & 0xFF));
    ops.push_back(0);
    ops.push_back(5);
    // DELETE the order we just added (idx=0 always)
    ops.push_back(2);
    ops.push_back(0);
    ops.push_back(0);
  }
  apply_ops_from_bytes(ops.data(), ops.size(), book);
  book.validate_invariants();
  EXPECT_EQ(book.order_count(), 0u);
}

TEST(FuzzBoundary, StaleRefsAreSafe) {
  OrderBook book(1024, 256);
  for (int i = 0; i < 1000; ++i) {
    book.execute_order(static_cast<OrderRef>(i) + 0xFFFF0000ULL, 1,
                       static_cast<Timestamp>(i) * 1000 + 1);
    book.delete_order(static_cast<OrderRef>(i) + 0xFFFF0000ULL,
                      static_cast<Timestamp>(i) * 1000 + 2);
  }
  EXPECT_EQ(book.order_count(), 0u);
}

TEST(FuzzBoundary, ReplaceChain) {
  OrderBook book(1024, 256);
  Timestamp ts = 1;
  book.add_order(1, 500000, 100, 'B', ts++);
  // Chain of replaces
  for (OrderRef i = 1; i < 50; ++i) {
    book.replace_order(i, i + 1, 500000 + static_cast<Price>(i) * 10, 100,
                       ts++);
  }
  book.validate_invariants();
  EXPECT_EQ(book.order_count(), 1u);
}

TEST(FuzzBoundary, MultiplePriceLevels) {
  OrderBook book(4096, 1024);
  Timestamp ts = 1;
  // Add bids at many price levels (all below 500000)
  for (int i = 0; i < 200; ++i) {
    book.add_order(static_cast<OrderRef>(i + 1),
                   499000 - static_cast<Price>(i * 100), 50, 'B', ts++);
  }
  // Add asks at higher price levels (all above 500000)
  for (int i = 0; i < 200; ++i) {
    book.add_order(static_cast<OrderRef>(i + 1001),
                   500100 + static_cast<Price>(i * 100), 50, 'S', ts++);
  }
  book.validate_invariants();
  EXPECT_LT(book.best_bid(), book.best_ask());
}
