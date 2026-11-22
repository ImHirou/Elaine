// SPDX-License-Identifier: GPL-3.0-or-later
// VarInt (LEB128, max 5 bytes) - the most frequent primitive on the wire: every frame length,
// packet id, string length and most ids. One-byte values take the first branch; everything
// else is branch-free via BMI2 pdep/pext, with a scalar fallback for other targets.
#pragma once

#include "base/types.hpp"

#if defined(__BMI2__)
#include <x86gprintrin.h>  // pdep/pext; <immintrin.h> is 0.7 s of every file that includes this one
#endif

namespace proto {

inline constexpr u32 kVarIntMaxBytes = 5;

ALWAYS_INLINE constexpr u32 varint_size(u32 v) { return (9u * static_cast<u32>(std::bit_width(v | 1u)) + 64u) / 64u; }

// Writes up to 8 bytes at p (callers reserve slack, see base::ByteBuf). Returns the new end.
ALWAYS_INLINE u8* put_varint(u8* p, u32 v) {
    if (LIKELY(v < 0x80)) {
        *p = static_cast<u8>(v);
        return p + 1;
    }
#if defined(__BMI2__)
    const u32 n = varint_size(v);
    u64 x = _pdep_u64(v, 0x0000007F7F7F7F7Full);
    x |= 0x0000000080808080ull & ((1ull << (8 * (n - 1))) - 1);  // continuation bit on all but the last byte
    store_le<u64>(p, x);
    return p + n;
#else
    do {
        *p++ = static_cast<u8>(v | 0x80);
        v >>= 7;
    } while (v >= 0x80);
    *p++ = static_cast<u8>(v);
    return p;
#endif
}

// Fixed 3-byte (padded) encoding of a value < 2^21. Decoders accept non-minimal VarInts, which
// lets a frame length be patched in after the body is written, with no memmove.
ALWAYS_INLINE void put_varint3(u8* p, u32 v) {
    DCHECK(v < (1u << 21));
    p[0] = static_cast<u8>(v | 0x80);
    p[1] = static_cast<u8>((v >> 7) | 0x80);
    p[2] = static_cast<u8>(v >> 14);
}

// Decode from p with at least 8 readable bytes. Returns bytes consumed, 0 if malformed (>5 bytes).
ALWAYS_INLINE u32 get_varint_fast(const u8* p, u32& out) {
    const u64 x = load_le<u64>(p);
    if (LIKELY(!(x & 0x80))) {
        out = static_cast<u32>(x & 0x7F);
        return 1;
    }
    const u64 stop = ~x & 0x0000008080808080ull;  // MSB-clear bytes among the first five
    if (UNLIKELY(!stop)) return 0;
    const u32 n = (static_cast<u32>(std::countr_zero(stop)) + 1) >> 3;
#if defined(__BMI2__)
    out = static_cast<u32>(_pext_u64(x & (~0ull >> (64 - 8 * n)), 0x0000007F7F7F7F7Full));
#else
    u32 v = 0;
    for (u32 i = 0; i < n; ++i) v |= static_cast<u32>(p[i] & 0x7F) << (7 * i);
    out = v;
#endif
    return n;
}

// Bounds-checked decode for buffer tails. Returns bytes consumed; 0 = need more data; ~0u = malformed.
inline u32 get_varint_slow(const u8* p, const u8* end, u32& out) {
    u32 v = 0;
    for (u32 i = 0; i < kVarIntMaxBytes; ++i) {
        if (p + i == end) return 0;
        const u8 b = p[i];
        v |= static_cast<u32>(b & 0x7F) << (7 * i);
        if (!(b & 0x80)) {
            out = v;
            return i + 1;
        }
    }
    return ~0u;
}

ALWAYS_INLINE u32 get_varint(const u8* p, const u8* end, u32& out) {
    if (LIKELY(end - p >= 8)) {
        const u32 n = get_varint_fast(p, out);
        return n ? n : ~0u;
    }
    return get_varint_slow(p, end, out);
}

}  // namespace proto
