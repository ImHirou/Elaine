// SPDX-License-Identifier: GPL-3.0-or-later
// Mth.sin / Mth.cos: vanilla's sines are a table of 65536 floats, and what is computed from them
// (which way a thrown item flies) is a float off that table, not the processor's sine.
#pragma once

#include <cmath>

#include "base/types.hpp"

namespace phys {

// Mth.SIN: (float) Math.sin(i / 10430.378350470453). The C library's sine rounds to the same
// floats as the JVM's for all of them (tests/unit/physics_test.cpp holds the table's checksum).
// A function and not a lambda: a lambda is constexpr by itself, and GCC then runs all 65536 sines
// at compile time to see whether the table could be a constant - 3.9 s of every file that includes
// this one, before it gives up (gcc 16.2, 2026-11-22).
inline float* make_sin_table() {
    float* t = new float[65536];
    for (u32 i = 0; i < 65536; ++i) t[i] = static_cast<float>(std::sin(static_cast<double>(i) / 10430.378350470453));
    return t;
}
inline const float* sin_table() {
    static const float* const table = make_sin_table();
    return table;
}
// Mth.sin(double), Mth.cos(double): (long) truncates, & 65535 wraps.
inline float mth_sin(double v) { return sin_table()[static_cast<u32>(static_cast<i64>(v * 10430.378350470453) & 65535)]; }
inline float mth_cos(double v) {
    return sin_table()[static_cast<u32>(static_cast<i64>(v * 10430.378350470453 + 16384.0) & 65535)];
}

}  // namespace phys
