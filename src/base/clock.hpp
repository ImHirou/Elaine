// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <ctime>

#include "base/types.hpp"

namespace base {

// Monotonic nanoseconds. vDSO call, ~20ns; fine per tick / per connection event, not per packet.
ALWAYS_INLINE u64 now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<u64>(ts.tv_sec) * 1'000'000'000ull + static_cast<u64>(ts.tv_nsec);
}

inline constexpr u64 kNsPerMs = 1'000'000ull;
inline constexpr u64 kNsPerSec = 1'000'000'000ull;

}  // namespace base
