// SPDX-License-Identifier: GPL-3.0-or-later
// The arithmetic of net.minecraft.util.Mth and of Java itself that world generation is written in.
//
// These are kept as vanilla writes them - `p0 + alpha * (p1 - p0)`, a comparison before a
// minimum - because a generator is right when it gives vanilla's bits: a float that is one ulp
// off decides, somewhere, whether a block is stone. The build never contracts a product and a sum
// into one operation (-ffp-contract=off), and Java never does either.
#pragma once

#include <cmath>

#include "base/types.hpp"

namespace worldgen {

// Mth.floor(double), Mth.floor(float): (int) Math.floor(v). What is cast is in an int's range
// wherever this is used; Java would clamp what is not.
ALWAYS_INLINE i32 floor_int(double v) { return static_cast<i32>(std::floor(v)); }
ALWAYS_INLINE i32 floor_int(float v) { return static_cast<i32>(std::floor(static_cast<double>(v))); }
// Mth.lfloor
ALWAYS_INLINE i64 floor_long(double v) { return static_cast<i64>(std::floor(v)); }
// Math.floorDiv, Math.floorMod of ints
ALWAYS_INLINE i32 floor_div(i32 a, i32 b) {
    const i32 q = a / b;
    return (a % b != 0 && ((a ^ b) < 0)) ? q - 1 : q;
}
ALWAYS_INLINE i32 floor_mod(i32 a, i32 b) {
    const i32 m = a % b;
    return (m != 0 && ((m ^ b) < 0)) ? m + b : m;
}

// Mth.lerp / lerp2 / lerp3, of floats and of doubles.
ALWAYS_INLINE float lerp(float alpha, float p0, float p1) { return p0 + alpha * (p1 - p0); }
ALWAYS_INLINE double lerp(double alpha, double p0, double p1) { return p0 + alpha * (p1 - p0); }
ALWAYS_INLINE float lerp2(float a1, float a2, float x00, float x10, float x01, float x11) {
    return lerp(a2, lerp(a1, x00, x10), lerp(a1, x01, x11));
}
ALWAYS_INLINE float lerp3(float a1, float a2, float a3, float x000, float x100, float x010, float x110, float x001, float x101,
                          float x011, float x111) {
    return lerp(a3, lerp2(a1, a2, x000, x100, x010, x110), lerp2(a1, a2, x001, x101, x011, x111));
}
// Mth.smoothstep(float)
ALWAYS_INLINE float smoothstep(float x) { return ((x * x) * x) * ((x * ((x * 6.0f) - 15.0f)) + 10.0f); }

// Math.min / Math.max of floats: a NaN wins, and -0.0 is less than 0.0.
ALWAYS_INLINE float java_min(float a, float b) {
    if (a != a) return a;
    if (a == 0.0f && b == 0.0f && std::signbit(b)) return b;
    return a <= b ? a : b;
}
ALWAYS_INLINE float java_max(float a, float b) {
    if (a != a) return a;
    if (a == 0.0f && b == 0.0f && std::signbit(a)) return b;
    return a >= b ? a : b;
}
ALWAYS_INLINE double java_min(double a, double b) {
    if (a != a) return a;
    if (a == 0.0 && b == 0.0 && std::signbit(b)) return b;
    return a <= b ? a : b;
}
ALWAYS_INLINE double java_max(double a, double b) {
    if (a != a) return a;
    if (a == 0.0 && b == 0.0 && std::signbit(a)) return b;
    return a >= b ? a : b;
}
// Mth.clamp(float, float, float): `value < min ? min : Math.min(value, max)`.
ALWAYS_INLINE float clamp(float value, float min, float max) { return value < min ? min : java_min(value, max); }
ALWAYS_INLINE double clamp(double value, double min, double max) { return value < min ? min : java_min(value, max); }

}  // namespace worldgen
