#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>

#include "event.h"
#include "types.h"

class OrderBook;

// mmap-based zero-copy NASDAQ ITCH 5.0 binary feed parser.
//
// Loads the file once via mmap + MADV_SEQUENTIAL. The main dispatch
// loop walks the message stream sequentially, decoding each message
// and calling into the provided OrderBook. Zero intermediate buffers.
//
// Supported message types: A (Add), F (Add+MPID), E (Execute),
//   C (Execute+Price), X (Cancel), D (Delete), U (Replace).
// All other message types are safely skipped.
class FeedHandler {
 public:
  explicit FeedHandler(const char* path);
  ~FeedHandler() noexcept;

  FeedHandler(const FeedHandler&) = delete;
  FeedHandler& operator=(const FeedHandler&) = delete;

  // Process the entire file, dispatching all events to `book`.
  // Returns the number of recognized events processed.
  uint64_t run(OrderBook& book) noexcept;

  // Process a single symbol (filtered by stock_locate).
  // stock_locate = 0 means process all symbols.
  uint64_t run(OrderBook& book, uint16_t stock_locate_filter) noexcept;

  // Single-event decode: fills ev with the next recognized ITCH event, advances
  // cursor_. Returns false on EOF or truncated stream. Book-agnostic; does not
  // touch OrderBook.
  bool step(Event& ev) noexcept;

  [[nodiscard]] bool valid() const noexcept { return data_ != nullptr; }
  [[nodiscard]] size_t file_size() const noexcept { return size_; }
  [[nodiscard]] uint64_t skipped() const noexcept { return skipped_; }

 private:
  void dispatch(OrderBook& book, const uint8_t* msg, uint16_t filter) noexcept;

  const uint8_t* data_;  // mmap'd file start
  size_t size_;          // file size in bytes
  int fd_;
  uint64_t skipped_;  // unknown/skipped message count
  const uint8_t*
      cursor_;  // current position for step(); initialised to data_ in ctor
};
