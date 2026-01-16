#include "feed_handler.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>

#include "itch_types.h"
#include "order_book.h"
#include "types.h"

FeedHandler::FeedHandler(const char* path)
    : data_(nullptr), size_(0), fd_(-1), skipped_(0), cursor_(nullptr) {
  fd_ = ::open(path, O_RDONLY);
  if (fd_ < 0) return;

  struct stat st{};
  if (::fstat(fd_, &st) < 0) {
    ::close(fd_);
    fd_ = -1;
    return;
  }
  size_ = static_cast<size_t>(st.st_size);
  if (size_ == 0) return;

  void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
  if (p == MAP_FAILED) {
    ::close(fd_);
    fd_ = -1;
    size_ = 0;
    return;
  }

  data_ = reinterpret_cast<const uint8_t*>(p);
  cursor_ = data_;
  ::madvise(const_cast<uint8_t*>(data_), size_, MADV_SEQUENTIAL);
}

FeedHandler::~FeedHandler() noexcept {
  if (data_) ::munmap(const_cast<uint8_t*>(data_), size_);
  if (fd_ >= 0) ::close(fd_);
}

// ---------------------------------------------------------------------------
// Byte-offset accessors for packed ITCH message frames.
//
// Frame layout (ptr points to the 2-byte length field):
//   ptr[0..1]  = length  (big-endian, NOT including these 2 bytes)
//   ptr[2]     = message_type
//   ptr[3..4]  = stock_locate  (big-endian)
//   ptr[5..6]  = tracking_number
//   ptr[7..12] = timestamp[6]  (big-endian ns from midnight)
//   ptr[13..]  = type-specific fields (see itch_types.h)
//
// All loads use load_be* helpers to avoid UBSan misalignment errors on ARM.
// ---------------------------------------------------------------------------

static inline uint16_t itch_stock_locate(const uint8_t* p) noexcept {
  return load_be16(p + 3);
}
static inline Timestamp itch_timestamp(const uint8_t* p) noexcept {
  return decode_timestamp(p + 7);
}
static inline OrderRef itch_order_ref(const uint8_t* p) noexcept {
  return load_be64(p + 13);
}
static inline char itch_side(const uint8_t* p) noexcept {
  return static_cast<char>(p[21]);
}
static inline Quantity itch_shares_add(const uint8_t* p) noexcept {
  return load_be32(p + 22);
}
static inline Price itch_price_add(const uint8_t* p) noexcept {
  return static_cast<Price>(load_be32(p + 34));
}
// Execute / Cancel share the same offsets for their first two fields.
static inline Quantity itch_exec_qty(const uint8_t* p) noexcept {
  return load_be32(p + 21);
}
static inline Quantity itch_cancel_qty(const uint8_t* p) noexcept {
  return load_be32(p + 21);
}
// Execute With Price
static inline Price itch_exec_price(const uint8_t* p) noexcept {
  return static_cast<Price>(load_be32(p + 34));
}
// Replace
static inline OrderRef itch_orig_ref(const uint8_t* p) noexcept {
  return load_be64(p + 13);
}
static inline OrderRef itch_new_ref(const uint8_t* p) noexcept {
  return load_be64(p + 21);
}
static inline Quantity itch_repl_shares(const uint8_t* p) noexcept {
  return load_be32(p + 29);
}
static inline Price itch_repl_price(const uint8_t* p) noexcept {
  return static_cast<Price>(load_be32(p + 33));
}

// ---------------------------------------------------------------------------
// Decode one ITCH message into an Event (book-agnostic).
// ptr points to the 2-byte length field.
// Returns false for unknown/unsupported message types.
// ---------------------------------------------------------------------------

static bool decode_event(const uint8_t* ptr, Event& ev) noexcept {
  const uint8_t type = ptr[2];
  ev.stock_locate = itch_stock_locate(ptr);
  ev.timestamp_ns = itch_timestamp(ptr);
  ev._pad[0] = ev._pad[1] = 0;
  ev.new_order_ref = 0;

  switch (type) {
    case 'A':
    case 'F':
      ev.type = Event::Type::ADD;
      ev.side = itch_side(ptr);
      ev.order_ref = itch_order_ref(ptr);
      ev.price = itch_price_add(ptr);
      ev.quantity = itch_shares_add(ptr);
      return true;
    case 'E':
      ev.type = Event::Type::EXECUTE;
      ev.side = 0;
      ev.order_ref = itch_order_ref(ptr);
      ev.price = 0;  // resting price requires book lookup; not available here
      ev.quantity = itch_exec_qty(ptr);
      return true;
    case 'C':
      ev.type = Event::Type::EXECUTE_PRICE;
      ev.side = 0;
      ev.order_ref = itch_order_ref(ptr);
      ev.price = itch_exec_price(ptr);
      ev.quantity = itch_exec_qty(ptr);
      return true;
    case 'X':
      ev.type = Event::Type::CANCEL;
      ev.side = 0;
      ev.order_ref = itch_order_ref(ptr);
      ev.price = 0;
      ev.quantity = itch_cancel_qty(
          ptr);  // cancelled delta; remaining requires book lookup
      return true;
    case 'D':
      ev.type = Event::Type::DELETE;
      ev.side = 0;
      ev.order_ref = itch_order_ref(ptr);
      ev.price = 0;
      ev.quantity = 0;
      return true;
    case 'U':
      ev.type = Event::Type::REPLACE;
      ev.side = 0;
      ev.order_ref = itch_orig_ref(ptr);
      ev.new_order_ref = itch_new_ref(ptr);
      ev.price = itch_repl_price(ptr);
      ev.quantity = itch_repl_shares(ptr);
      return true;
    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
// Dispatch a single message (ptr points to the 2-byte length field).
// ---------------------------------------------------------------------------

void FeedHandler::dispatch(OrderBook& book, const uint8_t* ptr,
                           uint16_t filter) noexcept {
  const uint8_t type = ptr[2];

  switch (type) {
    case 'A':
      if (filter && itch_stock_locate(ptr) != filter) break;
      book.add_order(itch_order_ref(ptr), itch_price_add(ptr),
                     itch_shares_add(ptr), itch_side(ptr), itch_timestamp(ptr));
      break;
    case 'F':  // Add Order with MPID: same fields as 'A', MPID appended
      if (filter && itch_stock_locate(ptr) != filter) break;
      book.add_order(itch_order_ref(ptr), itch_price_add(ptr),
                     itch_shares_add(ptr), itch_side(ptr), itch_timestamp(ptr));
      break;
    case 'E':
      if (filter && itch_stock_locate(ptr) != filter) break;
      book.execute_order(itch_order_ref(ptr), itch_exec_qty(ptr),
                         itch_timestamp(ptr));
      break;
    case 'C':  // Execute With Price: exec_price at offset 34
      if (filter && itch_stock_locate(ptr) != filter) break;
      book.execute_order_price(itch_order_ref(ptr), itch_exec_qty(ptr),
                               itch_exec_price(ptr), itch_timestamp(ptr));
      break;
    case 'X':
      if (filter && itch_stock_locate(ptr) != filter) break;
      book.cancel_order(itch_order_ref(ptr), itch_cancel_qty(ptr),
                        itch_timestamp(ptr));
      break;
    case 'D':
      if (filter && itch_stock_locate(ptr) != filter) break;
      book.delete_order(itch_order_ref(ptr), itch_timestamp(ptr));
      break;
    case 'U':
      if (filter && itch_stock_locate(ptr) != filter) break;
      book.replace_order(itch_orig_ref(ptr), itch_new_ref(ptr),
                         itch_repl_price(ptr), itch_repl_shares(ptr),
                         itch_timestamp(ptr));
      break;
    default:
      ++skipped_;
      break;
  }
}

// ---------------------------------------------------------------------------
// Main replay loop
// ---------------------------------------------------------------------------

uint64_t FeedHandler::run(OrderBook& book) noexcept { return run(book, 0); }

uint64_t FeedHandler::run(OrderBook& book,
                          uint16_t stock_locate_filter) noexcept {
  if (__builtin_expect(!valid(), 0)) return 0;

  const uint8_t* ptr = data_;
  const uint8_t* end = data_ + size_;

  while (__builtin_expect(ptr + 2 <= end, 1)) {
    const uint16_t msg_len = load_be16(ptr);
    if (__builtin_expect(ptr + 2 + msg_len > end, 0)) break;  // truncated
    if (__builtin_expect(msg_len >= 3, 1))  // need at least type byte
      dispatch(book, ptr, stock_locate_filter);
    ptr += 2 + msg_len;
  }

  return book.event_count();
}

bool FeedHandler::step(Event& ev) noexcept {
  if (!valid()) return false;
  const uint8_t* end = data_ + size_;
  while (cursor_ + 2 <= end) {
    const uint16_t msg_len = load_be16(cursor_);
    if (cursor_ + 2 + msg_len > end)
      return false;  // truncated frame: check BEFORE advancing
    const uint8_t* msg = cursor_;
    cursor_ += 2 + msg_len;
    if (msg_len >= 3 && decode_event(msg, ev)) return true;
  }
  return false;
}
