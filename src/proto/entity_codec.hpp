// SPDX-License-Identifier: GPL-3.0-or-later
// The number formats entity packets use, each a transcription of one vanilla method (26.3 jar).
// They decide what a client reconstructs, so they reproduce Java's arithmetic exactly: float
// where vanilla computes in float, round-half-up where it calls Math.round, no fused multiply-add.
#pragma once

#include <cmath>

#include "proto/wire.hpp"

namespace proto {

// Java's (int) cast of a double: saturating, NaN -> 0. (The C++ cast is undefined out of range.)
ALWAYS_INLINE i32 java_int(double v) {
    if (UNLIKELY(!(v > -2147483649.0 && v < 2147483648.0))) return v != v ? 0 : v < 0 ? INT32_MIN : INT32_MAX;
    return static_cast<i32>(v);
}

// Mth.floor(double)
ALWAYS_INLINE i32 floor_int(double v) { return java_int(std::floor(v)); }

// Mth.packDegrees: an angle as 1/256 of a turn. (byte)floor(angle * 256.0F / 360.0F), in float.
ALWAYS_INLINE u8 pack_degrees(float angle) {
    const float t = angle * 256.0f / 360.0f;
    return static_cast<u8>(floor_int(static_cast<double>(t)));
}
// Mth.unpackDegrees
ALWAYS_INLINE float unpack_degrees(u8 packed) { return static_cast<float>(static_cast<i8>(packed) * 360) / 256.0f; }

// Mth.wrapDegrees(float): into [-180, 180).
ALWAYS_INLINE float wrap_degrees(float angle) {
    float a = std::fmod(angle, 360.0f);
    if (a >= 180.0f) a -= 360.0f;
    if (a < -180.0f) a += 360.0f;
    return a;
}

// VecDeltaCodec.encode: a coordinate in 1/4096 of a block, Math.round(v * 4096.0). Math.round is
// "nearest, ties towards +infinity"; v - floor(v) is exact, so this is too.
ALWAYS_INLINE i64 delta_units(double v) {
    const double s = v * 4096.0;
    const double f = std::floor(s);
    return static_cast<i64>(f) + (s - f >= 0.5 ? 1 : 0);
}
// VecDeltaCodec.isDeltaTooBig: a step has to fit three shorts.
ALWAYS_INLINE bool delta_fits(i64 dx, i64 dy, i64 dz) {
    return (static_cast<u64>(dx + 32768) | static_cast<u64>(dy + 32768) | static_cast<u64>(dz + 32768)) <= 0xFFFF;
}

// LpVec3.write (Vec3.LP_STREAM_CODEC): a velocity in 1..11 bytes. Three 15-bit fractions of a
// shared integer scale; a vector shorter than ~3e-5 on every axis is the single byte 0.
inline constexpr u32 kLpVec3MaxBytes = 6 + kVarIntMaxBytes;
inline u8* put_lp_vec3(u8* p, double x, double y, double z) {
    const auto sanitize = [](double v) { return v != v ? 0.0 : std::fmin(std::fmax(v, -1.7179869183E10), 1.7179869183E10); };
    x = sanitize(x);
    y = sanitize(y);
    z = sanitize(z);
    const double longest = std::fmax(std::fabs(x), std::fmax(std::fabs(y), std::fabs(z)));
    if (longest < 3.051944088384301E-5) return put_u8(p, 0);
    const u64 scale = static_cast<u64>(std::ceil(longest));  // Mth.ceilLong; <= 2^34 after sanitize
    const bool partial = (scale & 3) != scale;               // the scale needs more than its two inline bits
    const u64 markers = partial ? (scale & 3) | 4 : scale;
    const auto pack = [scale](double v) {  // Math.round((v / scale * 0.5 + 0.5) * 32766.0)
        const double s = (v / static_cast<double>(scale) * 0.5 + 0.5) * 32766.0;
        const double f = std::floor(s);
        return static_cast<u64>(static_cast<i64>(f) + (s - f >= 0.5 ? 1 : 0));
    };
    const u64 bits = markers | pack(x) << 3 | pack(y) << 18 | pack(z) << 33;
    p[0] = static_cast<u8>(bits);
    p[1] = static_cast<u8>(bits >> 8);
    store_be32(p + 2, static_cast<u32>(bits >> 16));
    p += 6;
    if (partial) p = put_varint(p, static_cast<u32>(scale >> 2));
    return p;
}

}  // namespace proto
