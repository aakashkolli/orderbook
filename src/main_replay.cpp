#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "feed_handler.h"
#include "order_book.h"
#include "types.h"

static void usage(const char* prog) {
  std::fprintf(
      stderr,
      "Usage: %s <itch-file> [--checksum] [--stock-locate N]\n"
      "\n"
      "  --checksum      Print FNV-1a replay checksum after processing\n"
      "  --stock-locate  Filter to a single stock by NASDAQ stock_locate ID\n"
      "\n"
      "Example (determinism check):\n"
      "  %s data/mock.NASDAQ_ITCH50 --checksum > run1.txt\n"
      "  %s data/mock.NASDAQ_ITCH50 --checksum > run2.txt\n"
      "  diff run1.txt run2.txt   # must be empty\n",
      prog, prog, prog);
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    usage(argv[0]);
    return 1;
  }

  const char* path = argv[1];
  bool print_checksum = false;
  uint16_t stock_locate_filter = 0;

  for (int i = 2; i < argc; ++i) {
    if (std::strcmp(argv[i], "--checksum") == 0) {
      print_checksum = true;
    } else if (std::strcmp(argv[i], "--stock-locate") == 0 && i + 1 < argc) {
      stock_locate_filter = static_cast<uint16_t>(std::atoi(argv[++i]));
    }
  }

  FeedHandler feed(path);
  if (!feed.valid()) {
    std::fprintf(stderr, "error: cannot open '%s'\n", path);
    return 1;
  }

  OrderBook book;
  uint64_t events = feed.run(book, stock_locate_filter);

  std::printf("events:      %" PRIu64 "\n", events);
  std::printf("orders:      %" PRIu64 "\n",
              static_cast<uint64_t>(book.order_count()));
  std::printf("volume:      %" PRIu64 "\n", book.total_volume());

  if (book.best_bid() != NO_BID)
    std::printf("best_bid:    %" PRId64 " ($%.4f)\n", book.best_bid(),
                static_cast<double>(book.best_bid()) / 10000.0);
  else
    std::printf("best_bid:    (none)\n");

  if (book.best_ask() != NO_ASK)
    std::printf("best_ask:    %" PRId64 " ($%.4f)\n", book.best_ask(),
                static_cast<double>(book.best_ask()) / 10000.0);
  else
    std::printf("best_ask:    (none)\n");

  if (print_checksum)
    std::printf("checksum:    0x%016" PRIx64 "\n", book.checksum());

  return 0;
}
