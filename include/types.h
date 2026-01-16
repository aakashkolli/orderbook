#pragma once
#include <cstddef>
#include <cstdint>

// Byte-swap helpers: host is assumed little-endian (x86/ARM).
// These use compiler intrinsics to generate BSWAP instructions.
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
[[nodiscard]] inline uint16_t be16(uint16_t x) noexcept {
  return __builtin_bswap16(x);
}
[[nodiscard]] inline uint32_t be32(uint32_t x) noexcept {
  return __builtin_bswap32(x);
}
[[nodiscard]] inline uint64_t be64(uint64_t x) noexcept {
  return __builtin_bswap64(x);
}
#else
[[nodiscard]] inline uint16_t be16(uint16_t x) noexcept { return x; }
[[nodiscard]] inline uint32_t be32(uint32_t x) noexcept { return x; }
[[nodiscard]] inline uint64_t be64(uint64_t x) noexcept { return x; }
#endif

// Unaligned big-endian loads. ITCH messages are byte-packed and may not be
// naturally aligned. Use memcpy to avoid undefined behavior on strict-alignment
// architectures (ARM) and under UBSan.
#include <cstring>
[[nodiscard]] inline uint16_t load_be16(const void* p) noexcept {
  uint16_t v;
  std::memcpy(&v, p, 2);
  return be16(v);
}
[[nodiscard]] inline uint32_t load_be32(const void* p) noexcept {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return be32(v);
}
[[nodiscard]] inline uint64_t load_be64(const void* p) noexcept {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return be64(v);
}

// All prices are int64_t fixed-point in $0.0001 units.
// A price of $50.00 is represented as 500000.
// Floating-point is forbidden in the core pipeline.
using Price = int64_t;
using Quantity = uint32_t;
using OrderRef = uint64_t;
using Timestamp = uint64_t;  // nanoseconds from midnight

// Decode ITCH 6-byte big-endian timestamp.
// Correctness-critical: off-by-one byte shifts produce wrong timestamps.
[[nodiscard]] inline Timestamp decode_timestamp(const uint8_t ts[6]) noexcept {
  uint64_t ns = 0;
  for (int i = 0; i < 6; ++i) ns = (ns << 8) | ts[i];
  return ns;
}

// Compile-time sentinel prices.
static constexpr Price NO_BID = 0;
static constexpr Price NO_ASK = INT64_MAX;
