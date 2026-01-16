// tests/test_adapters.cpp
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>

#include "csv_adapter.hpp"
#include "feed_handler.h"
#include "order_book.h"
#include "orderbook/replay_session.hpp"
#include "types.h"

// ---------------------------------------------------------------------------
// OrderBook::top_n_levels and best_bid/ask_level
// ---------------------------------------------------------------------------

TEST(OrderBook, TopNLevels_BidDescending) {
  OrderBook book(100, 256);
  Timestamp ts = 1;
  // 5 distinct bid prices, 4 orders of 100 shares each
  for (int p = 0; p < 5; ++p) {
    Price price = 500000 + static_cast<Price>(p) * 100;
    for (int i = 0; i < 4; ++i)
      book.add_order(static_cast<OrderRef>(p * 4 + i + 1), price, 100, 'B',
                     ts++);
  }

  OrderBook::LevelEntry out[10];
  int count = 0;
  book.top_n_levels(3, 'B', out, count);

  ASSERT_EQ(count, 3);
  EXPECT_EQ(out[0].price, 500400);  // highest bid first
  EXPECT_EQ(out[1].price, 500300);
  EXPECT_EQ(out[2].price, 500200);
  EXPECT_EQ(out[0].volume, 400u);  // 4 orders × 100 shares
  EXPECT_EQ(out[0].order_count, 4u);
  // Cross-check via public level() accessor
  EXPECT_EQ(out[0].volume, book.level(500400).volume);
}

TEST(OrderBook, TopNLevels_AskAscending) {
  OrderBook book(100, 256);
  Timestamp ts = 1;
  for (int p = 0; p < 5; ++p) {
    Price price = 510000 + static_cast<Price>(p) * 100;
    book.add_order(static_cast<OrderRef>(p + 1), price, 200, 'S', ts++);
  }

  OrderBook::LevelEntry out[10];
  int count = 0;
  book.top_n_levels(3, 'S', out, count);

  ASSERT_EQ(count, 3);
  EXPECT_EQ(out[0].price, 510000);  // lowest ask first
  EXPECT_EQ(out[1].price, 510100);
  EXPECT_EQ(out[2].price, 510200);
}

TEST(OrderBook, TopNLevels_EmptyBook) {
  OrderBook book(16, 32);
  OrderBook::LevelEntry out[5];
  int count = 99;
  book.top_n_levels(5, 'B', out, count);
  EXPECT_EQ(count, 0);
}

TEST(OrderBook, BestLevels_Empty) {
  OrderBook book(16, 32);
  auto bid = book.best_bid_level();
  auto ask = book.best_ask_level();
  EXPECT_EQ(bid.price, NO_BID);
  EXPECT_EQ(bid.volume, 0u);
  EXPECT_EQ(ask.price, NO_ASK);
  EXPECT_EQ(ask.volume, 0u);
}

TEST(OrderBook, BestLevels_WithOrders) {
  OrderBook book(16, 32);
  book.add_order(1, 500000, 150, 'B', 1);
  book.add_order(2, 500100, 200, 'S', 2);
  auto bid = book.best_bid_level();
  auto ask = book.best_ask_level();
  EXPECT_EQ(bid.price, 500000);
  EXPECT_EQ(bid.volume, 150u);
  EXPECT_EQ(ask.price, 500100);
  EXPECT_EQ(ask.volume, 200u);
}

// ---------------------------------------------------------------------------
// ReplaySession: step-by-step checksum matches FeedHandler::run() batch
// ---------------------------------------------------------------------------

TEST(ReplaySession, FullReplayChecksumMatchesBatchRun) {
  const char* path = std::getenv("ITCH_FILE");
  if (!path) GTEST_SKIP() << "Set ITCH_FILE=data/mock.NASDAQ_ITCH50 to run";

  // Path 1: step-by-step via ReplaySession
  uint64_t step_checksum = 0;
  {
    ReplaySession session(make_itch_source(path));
    while (session.step()) {
    }
    step_checksum = session.snapshot().checksum;
  }

  // Path 2: FeedHandler::run() batch
  uint64_t run_checksum = 0;
  {
    FeedHandler feed(path);
    ASSERT_TRUE(feed.valid());
    OrderBook book;
    feed.run(book);
    run_checksum = book.checksum();
  }

  EXPECT_EQ(step_checksum, run_checksum)
      << "step-by-step replay produced different checksum than batch run";
}

TEST(ReplaySession, SnapshotStructuralInvariants) {
  const char* path = std::getenv("ITCH_FILE");
  if (!path) GTEST_SKIP() << "Set ITCH_FILE=data/mock.NASDAQ_ITCH50 to run";

  ReplaySession session(make_itch_source(path));
  for (int i = 0; i < 5000 && session.step(); ++i) {
  }

  Snapshot snap = session.snapshot();
  EXPECT_GT(snap.order_count, 100u);
  EXPECT_LT(snap.best_bid, snap.best_ask);
  EXPECT_NE(snap.checksum, 0u);
}

TEST(ReplaySession, QueryTopNBidDescending) {
  const char* path = std::getenv("ITCH_FILE");
  if (!path) GTEST_SKIP() << "Set ITCH_FILE=data/mock.NASDAQ_ITCH50 to run";

  ReplaySession session(make_itch_source(path));
  for (int i = 0; i < 5000 && session.step(); ++i) {
  }

  auto levels = session.query_top_n(5, Side::BID);
  ASSERT_LE(static_cast<int>(levels.size()), 5);
  for (size_t i = 1; i < levels.size(); ++i)
    EXPECT_GT(levels[i - 1].first, levels[i].first)
        << "bid levels not strictly descending";
}

// ---------------------------------------------------------------------------
// CsvEventSource round-trip
// ---------------------------------------------------------------------------

TEST(CsvAdapter, RoundTrip) {
  const char* tmp = "/tmp/ob_test_csv_adapter.csv";
  {
    std::ofstream f(tmp);
    // header row (skipped by adapter since it's not a valid type)
    f << "type,side,price,quantity,order_ref,timestamp_ns\n";
    f << "ADD,B,50.0000,100,1,1000000000\n";
    f << "ADD,S,50.0100,200,2,1000000001\n";
    f << "EXECUTE,B,0,50,1,1000000002\n";
    f << "DELETE,B,0,0,2,1000000003\n";
    f << "ADD,B,49.9900,300,3,1000000004\n";
  }

  CsvEventSource src(tmp);
  Event ev{};

  // Row 1: header: UNKNOWN type, skipped internally; first real event is ADD
  ASSERT_TRUE(src.next(ev));
  EXPECT_EQ(ev.type, Event::Type::ADD);
  EXPECT_EQ(ev.side, 'B');
  EXPECT_EQ(ev.price, 500000);  // 50.0000 * 10000
  EXPECT_EQ(ev.quantity, 100u);
  EXPECT_EQ(ev.order_ref, 1u);
  EXPECT_EQ(ev.timestamp_ns, 1000000000u);

  ASSERT_TRUE(src.next(ev));
  EXPECT_EQ(ev.type, Event::Type::ADD);
  EXPECT_EQ(ev.side, 'S');
  EXPECT_EQ(ev.price, 500100);  // 50.0100 * 10000

  ASSERT_TRUE(src.next(ev));
  EXPECT_EQ(ev.type, Event::Type::EXECUTE);

  ASSERT_TRUE(src.next(ev));
  EXPECT_EQ(ev.type, Event::Type::DELETE);

  ASSERT_TRUE(src.next(ev));
  EXPECT_EQ(ev.type, Event::Type::ADD);
  EXPECT_EQ(ev.price, 499900);  // 49.9900 * 10000

  // EOF; must return false and remain stable
  EXPECT_FALSE(src.next(ev));
  EXPECT_FALSE(src.next(ev));

  std::remove(tmp);
}

TEST(CsvAdapter, CustomDelimiter) {
  const char* tmp = "/tmp/ob_test_csv_pipe.csv";
  {
    std::ofstream f(tmp);
    f << "ADD|B|50.0000|100|10|2000000000\n";
  }
  CsvColumnMap cols;
  cols.delimiter = '|';
  CsvEventSource src(tmp, cols);
  Event ev{};
  ASSERT_TRUE(src.next(ev));
  EXPECT_EQ(ev.type, Event::Type::ADD);
  EXPECT_EQ(ev.price, 500000);
  EXPECT_FALSE(src.next(ev));
  std::remove(tmp);
}

// ---------------------------------------------------------------------------
// ParquetEventSource (compiled only when HAVE_ARROW is defined)
// ---------------------------------------------------------------------------

#ifdef HAVE_ARROW
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/writer.h>

#include "parquet_adapter.hpp"

static void write_test_parquet(const char* path) {
  auto type_arr = arrow::ArrayFromJSON(arrow::utf8(),
                                       R"(["ADD","ADD","EXECUTE","DELETE"])");
  auto side_arr = arrow::ArrayFromJSON(arrow::utf8(), R"(["B","S","B","B"])");
  auto price_arr = arrow::ArrayFromJSON(arrow::int64(), "[500000,500100,0,0]");
  auto qty_arr = arrow::ArrayFromJSON(arrow::int64(), "[100,200,50,0]");
  auto ref_arr = arrow::ArrayFromJSON(arrow::int64(), "[1,2,1,2]");
  auto ts_arr = arrow::ArrayFromJSON(
      arrow::int64(), "[1000000000,1000000001,1000000002,1000000003]");

  auto schema = arrow::schema({
      arrow::field("type", arrow::utf8()),
      arrow::field("side", arrow::utf8()),
      arrow::field("price", arrow::int64()),
      arrow::field("quantity", arrow::int64()),
      arrow::field("order_ref", arrow::int64()),
      arrow::field("timestamp_ns", arrow::int64()),
  });
  auto table =
      *arrow::Table::Make(schema, {type_arr.ValueOrDie(), side_arr.ValueOrDie(),
                                   price_arr.ValueOrDie(), qty_arr.ValueOrDie(),
                                   ref_arr.ValueOrDie(), ts_arr.ValueOrDie()});

  auto outfile = *arrow::io::FileOutputStream::Open(path);
  PARQUET_THROW_NOT_OK(parquet::arrow::WriteTable(
      *table, arrow::default_memory_pool(), outfile, /*chunk_size=*/1024));
}

TEST(ParquetAdapter, RoundTrip) {
  const char* tmp = "/tmp/ob_test_parquet_adapter.parquet";
  write_test_parquet(tmp);

  ParquetEventSource src(tmp);
  Event ev{};

  ASSERT_TRUE(src.next(ev));
  EXPECT_EQ(ev.type, Event::Type::ADD);
  EXPECT_EQ(ev.side, 'B');
  EXPECT_EQ(ev.price, 500000);

  ASSERT_TRUE(src.next(ev));
  EXPECT_EQ(ev.type, Event::Type::ADD);
  EXPECT_EQ(ev.price, 500100);

  ASSERT_TRUE(src.next(ev));
  EXPECT_EQ(ev.type, Event::Type::EXECUTE);

  ASSERT_TRUE(src.next(ev));
  EXPECT_EQ(ev.type, Event::Type::DELETE);

  EXPECT_FALSE(src.next(ev));
  std::remove(tmp);
}
#endif  // HAVE_ARROW
