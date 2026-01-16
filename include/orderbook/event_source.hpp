#pragma once
#include <cstdint>

#include "event.h"

// Side enum: used at adapter and Python API boundaries.
// Core Event struct retains char side to avoid touching hot-path types.
enum class Side : uint8_t { BID = 'B', ASK = 'S' };

// Abstract source of normalized events.
// All implementations must be noexcept on next(); catch internally and return
// false.
struct IEventSource {
  virtual bool next(Event& ev) noexcept = 0;
  virtual ~IEventSource() = default;
};
