// Full order book invariants
// 1. Invariants
//    - best_bid < best_ask when both sides exist
//    - queue positions are never negative
//    - slab refcount is always 0 or 1
//    - each order_ref appears at most once in lookup_
//    - prices use int64_t only, no floating point
//    - identical input produces identical replay checksums
//    - level.count matches the number of orders in the level
//    - lookup_ never contains orders with Free state
//    - timestamps are monotonically non-decreasing in debug builds
//
// 2. Crash safety
//    - slab_.allocate() in add_order() is the main failure point
//    - a crash after allocation but before lookup_.insert() leaves
//      an unreachable live order in the slab
//    - recovery requires replaying the full ITCH stream
//
// 3. Performance and contention
//    - single-threaded design with no concurrency
//    - best bid or ask updates can take O(spread) after clearing a level
//    - all other operations are O(1)
//
// 4. Adversarial cases
//    - executing a deleted order safely skips on nullptr lookup
//    - canceling more than remaining quantity triggers a debug assert
//    - exceeding slab capacity triggers a debug assert
//
// 5. Observability
//    - checksum_.digest() must match across identical replays
//    - checksum diffs between identical runs must be empty

#include <gtest/gtest.h>

#include <cstring>

#include "fill_simulator.h"
#include "hash_map.h"
#include "itch_types.h"
#include "order_book.h"
#include "pnl_tracker.h"
#include "slab_allocator.h"
#include "types.h"

// Timestamp decoder

TEST(TimestampDecoder, KnownValues) {
  // 6-byte big-endian encoding of 100 nanoseconds = 0x0000000000000064
  uint8_t ts[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x64};
  EXPECT_EQ(decode_timestamp(ts), 100ULL);

  // 6-byte big-endian encoding of a real market time: 9:30 AM = 34200 * 1e9 ns
  // 34200000000000 = 0x00001F1A3A17C00: fits in 6 bytes (max ~281 trillion)
  uint64_t market_open_ns = 34200ULL * 1'000'000'000ULL;
  uint8_t ts2[6];
  for (int i = 5; i >= 0; --i) {
    ts2[i] = static_cast<uint8_t>(market_open_ns & 0xFF);
    market_open_ns >>= 8;
  }
  // Reconstruct and verify
  uint64_t ns_9am = 34200ULL * 1'000'000'000ULL;
  EXPECT_EQ(decode_timestamp(ts2), ns_9am);
}

// Slab allocator

TEST(SlabAllocator, AllocFreeBasic) {
  SlabAllocator slab(16);
  EXPECT_EQ(slab.available(), 16u);
  Order* o = slab.allocate();
  EXPECT_NE(o, nullptr);
  EXPECT_EQ(slab.live(), 1u);
  slab.free(o);
  EXPECT_EQ(slab.live(), 0u);
  EXPECT_EQ(slab.available(), 16u);
}

TEST(SlabAllocator, ReuseAfterFree) {
  SlabAllocator slab(4);
  Order* a = slab.allocate();
  Order* b = slab.allocate();
  slab.free(a);
  Order* c = slab.allocate();  // should reuse a's slot
  EXPECT_EQ(c, a);
  slab.free(b);
  slab.free(c);
}

TEST(SlabAllocator, AlignmentAndSize) {
  SlabAllocator slab(4);
  Order* o = slab.allocate();
  EXPECT_EQ(reinterpret_cast<uintptr_t>(o) % 64, 0u)
      << "Order not cache-line aligned";
  slab.free(o);
}

TEST(SlabAllocator, ExhaustAndRefill) {
  const uint32_t N = 64;
  SlabAllocator slab(N);
  std::vector<Order*> ptrs(N);
  for (uint32_t i = 0; i < N; ++i) ptrs[i] = slab.allocate();
  EXPECT_EQ(slab.available(), 0u);
  for (uint32_t i = 0; i < N; ++i) slab.free(ptrs[i]);
  EXPECT_EQ(slab.available(), N);
}

// Robin Hood hash map

TEST(OrderHashMap, InsertFindErase) {
  OrderHashMap map(64);  // small capacity for testing
  // We need dummy Order* pointers
  alignas(64) Order dummy{};
  dummy.order_ref = 42;

  map.insert(42, &dummy);
  EXPECT_EQ(map.find(42), &dummy);
  EXPECT_EQ(map.size(), 1u);

  map.erase(42);
  EXPECT_EQ(map.find(42), nullptr);
  EXPECT_EQ(map.size(), 0u);
}

TEST(OrderHashMap, ManyInsertions) {
  const uint64_t N = 1000;
  OrderHashMap map(4096);  // plenty of capacity
  alignas(64) static Order orders[N];
  for (uint64_t i = 1; i <= N; ++i) {
    orders[i - 1].order_ref = i;
    map.insert(i, &orders[i - 1]);
  }
  EXPECT_EQ(map.size(), N);
  for (uint64_t i = 1; i <= N; ++i) {
    EXPECT_EQ(map.find(i), &orders[i - 1]) << "failed at key " << i;
  }
  for (uint64_t i = 1; i <= N; ++i) {
    map.erase(i);
    EXPECT_EQ(map.find(i), nullptr);
  }
  EXPECT_EQ(map.size(), 0u);
}

TEST(OrderHashMap, FindMissing) {
  OrderHashMap map(64);
  EXPECT_EQ(map.find(999), nullptr);
}

// Price level (intrusive list)

TEST(PriceLevel, PushBackAndUnlink) {
  PriceLevel lvl{};
  alignas(64) Order a{}, b{}, c{};
  a.quantity = 100;
  b.quantity = 200;
  c.quantity = 300;

  lvl.push_back(&a);
  lvl.push_back(&b);
  lvl.push_back(&c);

  EXPECT_EQ(lvl.count, 3u);
  EXPECT_EQ(lvl.volume, 600u);
  EXPECT_EQ(lvl.head, &a);
  EXPECT_EQ(lvl.tail, &c);

  lvl.unlink(&b);  // remove middle
  EXPECT_EQ(lvl.count, 2u);
  EXPECT_EQ(lvl.volume, 400u);
  EXPECT_EQ(a.next, &c);
  EXPECT_EQ(c.prev, &a);

  lvl.unlink(&a);  // remove head
  EXPECT_EQ(lvl.head, &c);

  lvl.unlink(&c);  // remove tail
  EXPECT_TRUE(lvl.empty());
}

// OrderBook: add/delete/execute/cancel/replace

class OrderBookTest : public ::testing::Test {
 protected:
  void SetUp() override { book = std::make_unique<OrderBook>(1024, 256); }
  std::unique_ptr<OrderBook> book;
  static constexpr Timestamp T0 = 1'000'000'000ULL;
};

TEST_F(OrderBookTest, AddSingleBid) {
  book->add_order(1, 500000, 100, 'B', T0);
  EXPECT_EQ(book->best_bid(), 500000);
  EXPECT_EQ(book->best_ask(), NO_ASK);
  EXPECT_EQ(book->total_volume(), 100u);
  EXPECT_EQ(book->order_count(), 1u);
}

TEST_F(OrderBookTest, AddBidAndAsk) {
  book->add_order(1, 500000, 100, 'B', T0);
  book->add_order(2, 500100, 200, 'S', T0 + 1);
  EXPECT_EQ(book->best_bid(), 500000);
  EXPECT_EQ(book->best_ask(), 500100);
  EXPECT_GT(book->best_ask(), book->best_bid()) << "crossed market";
}

TEST_F(OrderBookTest, DeleteOrder) {
  book->add_order(1, 500000, 100, 'B', T0);
  book->delete_order(1, T0 + 1);
  EXPECT_EQ(book->best_bid(), NO_BID);
  EXPECT_EQ(book->order_count(), 0u);
  EXPECT_EQ(book->total_volume(), 0u);
}

TEST_F(OrderBookTest, FullExecution) {
  book->add_order(1, 500000, 100, 'B', T0);
  book->execute_order(1, 100, T0 + 1);
  EXPECT_EQ(book->order_count(), 0u);
  EXPECT_EQ(book->total_volume(), 0u);
  EXPECT_EQ(book->best_bid(), NO_BID);
}

TEST_F(OrderBookTest, PartialExecution) {
  book->add_order(1, 500000, 100, 'B', T0);
  book->execute_order(1, 40, T0 + 1);
  EXPECT_EQ(book->order_count(), 1u);
  EXPECT_EQ(book->total_volume(), 60u);
  EXPECT_EQ(book->level(500000).volume, 60u);
}

TEST_F(OrderBookTest, CancelPartial) {
  book->add_order(1, 500000, 100, 'B', T0);
  book->cancel_order(1, 30, T0 + 1);  // cancel 30, leaving 70
  EXPECT_EQ(book->order_count(), 1u);
  EXPECT_EQ(book->total_volume(), 70u);
}

TEST_F(OrderBookTest, ReplaceOrder) {
  book->add_order(1, 500000, 100, 'B', T0);
  book->replace_order(1, 2, 499900, 200, T0 + 1);
  EXPECT_EQ(book->order_count(), 1u);
  EXPECT_EQ(book->best_bid(), 499900);
  EXPECT_EQ(book->total_volume(), 200u);
  // Original ref should be gone
  // New ref should exist (we can verify via level non-empty at new price)
  EXPECT_EQ(book->level(499900).volume, 200u);
  EXPECT_TRUE(book->level(500000).empty());
}

TEST_F(OrderBookTest, StaleRefSafeSkip) {
  // Execute/delete on unknown ref must be safe no-ops.
  book->execute_order(999, 100, T0);
  book->delete_order(999, T0 + 1);
  EXPECT_EQ(book->order_count(), 0u);
}

TEST_F(OrderBookTest, BestBidUpdateAfterDelete) {
  book->add_order(1, 500000, 100, 'B', T0);
  book->add_order(2, 499900, 200, 'B', T0 + 1);
  EXPECT_EQ(book->best_bid(), 500000);

  book->delete_order(1, T0 + 2);
  EXPECT_EQ(book->best_bid(), 499900)
      << "best bid should fall back to second level";

  book->delete_order(2, T0 + 3);
  EXPECT_EQ(book->best_bid(), NO_BID);
}

TEST_F(OrderBookTest, BestAskUpdateAfterDelete) {
  book->add_order(1, 500100, 100, 'S', T0);
  book->add_order(2, 500200, 200, 'S', T0 + 1);
  EXPECT_EQ(book->best_ask(), 500100);

  book->delete_order(1, T0 + 2);
  EXPECT_EQ(book->best_ask(), 500200);

  book->delete_order(2, T0 + 3);
  EXPECT_EQ(book->best_ask(), NO_ASK);
}

TEST_F(OrderBookTest, NoCrossedMarket) {
  book->add_order(1, 500000, 100, 'B', T0);
  book->add_order(2, 500100, 100, 'S', T0 + 1);
  book->validate_invariants();
  EXPECT_LT(book->best_bid(), book->best_ask());
}

TEST_F(OrderBookTest, DeterministicChecksum) {
  // Run identical sequence twice; checksums must match.
  book->add_order(1, 500000, 100, 'B', T0);
  book->add_order(2, 500100, 200, 'S', T0 + 1);
  book->execute_order(1, 50, T0 + 2);
  book->cancel_order(2, 100, T0 + 3);
  uint64_t cs1 = book->checksum();

  auto book2 = std::make_unique<OrderBook>(1024, 256);
  book2->add_order(1, 500000, 100, 'B', T0);
  book2->add_order(2, 500100, 200, 'S', T0 + 1);
  book2->execute_order(1, 50, T0 + 2);
  book2->cancel_order(2, 100, T0 + 3);
  uint64_t cs2 = book2->checksum();

  EXPECT_EQ(cs1, cs2) << "checksum is not deterministic";
}

TEST_F(OrderBookTest, InvariantsAfterMultipleOps) {
  book->add_order(1, 500000, 100, 'B', T0);
  book->add_order(2, 500100, 200, 'S', T0 + 1);
  book->add_order(3, 499800, 300, 'B', T0 + 2);
  book->execute_order(1, 50, T0 + 3);
  book->cancel_order(2, 100, T0 + 4);
  book->replace_order(3, 4, 499900, 150, T0 + 5);
  book->validate_invariants();
}

// FillSimulator

TEST(FillSimulator, BasicFill) {
  FillSimulator sim;
  // Submit an order at price 500000, qty=100, queue_pos=0, cumul_exec=0
  uint32_t id = sim.submit(500000, 'B', 100, 0, 0, 1000);
  EXPECT_FALSE(sim.order(id).filled);

  // Execute 99 shares: not enough to fill (need 100)
  sim.on_execute(500000, 99, 2000, 99);
  EXPECT_FALSE(sim.order(id).filled);

  // Execute 1 more: cumul = 100 >= queue_pos(0) + qty(100)
  sim.on_execute(500000, 1, 3000, 100);
  EXPECT_TRUE(sim.order(id).filled);
  EXPECT_EQ(sim.fill_count(), 1u);
}

TEST(FillSimulator, QueuePosition) {
  FillSimulator sim;
  // Order behind 200 shares ahead; total needed = 200 + 100 = 300
  uint32_t id = sim.submit(500000, 'B', 100, 200, 0, 1000);

  sim.on_execute(500000, 150, 2000, 150);
  EXPECT_FALSE(sim.order(id).filled);  // 150 < 300

  sim.on_execute(500000, 150, 3000, 300);
  EXPECT_TRUE(sim.order(id).filled);  // 300 >= 300
}

// PnL Tracker

TEST(PnLTracker, RoundTrip) {
  PnLTracker tracker;
  // Buy 100 @ 50.00 ($0.0001 units = 500000)
  tracker.record_fill(1000, 500000, 100, 'B');
  // Sell 100 @ 50.10 ($0.0001 units = 501000)
  tracker.record_fill(2000, 501000, 100, 'S');

  // PnL = (501000 - 500000) * 100 = 100000 ($0.0001 units) = $10.00
  EXPECT_EQ(tracker.realized_pnl(), 100'000LL);
  EXPECT_EQ(tracker.avg_buy_price(), 500000);
  EXPECT_EQ(tracker.avg_sell_price(), 501000);
}

// ITCH struct size assertions (compile-time, also caught at runtime)

TEST(ITCHTypes, StructSizes) {
  EXPECT_EQ(sizeof(ITCHAddOrder), 38u);
  EXPECT_EQ(sizeof(ITCHAddOrderMPID), 42u);
  EXPECT_EQ(sizeof(ITCHOrderExecuted), 33u);
  EXPECT_EQ(sizeof(ITCHOrderExecutedWithPrice), 38u);
  EXPECT_EQ(sizeof(ITCHOrderCancel), 25u);
  EXPECT_EQ(sizeof(ITCHOrderDelete), 21u);
  EXPECT_EQ(sizeof(ITCHOrderReplace), 37u);
}

// End-to-end: synthetic ITCH stream round-trip

// Build a minimal ITCH binary message and feed it directly to the OrderBook.
TEST(EndToEnd, SyntheticStreamRoundTrip) {
  // Construct a minimal Add Order 'A' message in memory.
  constexpr uint16_t MSG_LEN = 36;  // per ITCH spec
  std::vector<uint8_t> buf(2 + MSG_LEN, 0);

  auto* hdr = reinterpret_cast<ITCHAddOrder*>(buf.data());
  hdr->length = be16(MSG_LEN);
  hdr->message_type = 'A';
  hdr->stock_locate = be16(1);
  hdr->tracking_number = 0;
  // timestamp = 10 billion ns = 0x00 00 00 02 54 0B E4 00
  // For 6-byte encoding of 10,000,000,000:
  {
    uint64_t v = 10'000'000'000ULL;
    for (int i = 5; i >= 0; --i) {
      hdr->timestamp[i] = static_cast<uint8_t>(v & 0xFF);
      v >>= 8;
    }
  }
  hdr->order_reference = be64(42ULL);
  hdr->side = 'B';
  hdr->shares = be32(100);
  std::memcpy(hdr->stock, "AAPL    ", 8);
  hdr->price = be32(500000);  // $50.00 (within MAX_TICKS = 1<<20)

  // Parse the timestamp back.
  EXPECT_EQ(decode_timestamp(hdr->timestamp), 10'000'000'000ULL);

  OrderBook book(1024, 256);
  book.add_order(be64(hdr->order_reference),
                 static_cast<Price>(be32(hdr->price)), be32(hdr->shares),
                 hdr->side, decode_timestamp(hdr->timestamp));

  EXPECT_EQ(book.best_bid(), 500000);
  EXPECT_EQ(book.total_volume(), 100u);
  EXPECT_EQ(book.order_count(), 1u);
}
