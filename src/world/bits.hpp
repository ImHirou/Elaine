// SPDX-License-Identifier: GPL-3.0-or-later
// Bit-packed arrays used by the world storage.
//
//   dense   entry i occupies bits [i*bits, (i+1)*bits) of a little-endian bit stream; bits is 1..8.
//           This is OUR layout for palette indices: no padding at all, so a section with 5..8
//           states costs 1.5 KiB where vanilla's in-memory layout costs 2 KiB.
//   words   vanilla's SimpleBitStorage: 64/bits entries per 64-bit word, none spanning two words.
//           This is the WIRE layout (paletted containers, heightmaps).
//
// Dense arrays never loop bit by bit: one entry is a 16-bit unaligned load, and whole arrays move
// through pdep/pext eight entries at a time (eight entries are exactly `bits` bytes, so every group
// starts on a byte boundary).
#pragma once

#include <x86gprintrin.h>  // pdep/pext; <immintrin.h> is 0.7 s of every file that includes this one

#include "base/types.hpp"

namespace world {

inline constexpr u32 kSectionVolume = 4096;  // 16^3 entries; every dense array here has this length

// Dense storage must be followed by this many readable and writable bytes: the bulk kernels load
// and store 8 bytes at a time.
inline constexpr u32 kPackedSlack = 8;
constexpr u32 packed_bytes(u32 bits) { return kSectionVolume * bits / 8; }

ALWAYS_INLINE u32 packed_get(const u8* data, u32 bits, u32 i) {
    const u32 pos = i * bits;
    return (static_cast<u32>(load_le<u16>(data + (pos >> 3))) >> (pos & 7)) & ((1u << bits) - 1);
}

// Store v at entry i and return what was there.
ALWAYS_INLINE u32 packed_exchange(u8* data, u32 bits, u32 i, u32 v) {
    const u32 pos = i * bits;
    u8* p = data + (pos >> 3);
    const u32 sh = pos & 7, mask = (1u << bits) - 1;
    const u32 w = load_le<u16>(p);
    store_le<u16>(p, static_cast<u16>((w & ~(mask << sh)) | (v << sh)));
    return (w >> sh) & mask;
}

// All 4096 entries -> one byte each.
inline void unpack_indices(const u8* src, u32 bits, u8* RESTRICT out) {
    const u64 m = 0x0101010101010101ull * ((1u << bits) - 1);
    for (u32 g = 0; g < kSectionVolume / 8; ++g) store_le<u64>(out + 8 * g, _pdep_u64(load_le<u64>(src + g * bits), m));
}

// One byte per entry -> dense. Writes up to 7 zero bytes into the slack.
inline void pack_indices(const u8* RESTRICT in, u32 bits, u8* dst) {
    const u64 m = 0x0101010101010101ull * ((1u << bits) - 1);
    for (u32 g = 0; g < kSectionVolume / 8; ++g) store_le<u64>(dst + g * bits, _pext_u64(load_le<u64>(in + 8 * g), m));
}

// ---- vanilla word layout ---------------------------------------------------------------------

constexpr u32 words_for(u32 entries, u32 bits) {
    const u32 per = 64 / bits;
    return (entries + per - 1) / per;
}

ALWAYS_INLINE u32 words_get(const u64* w, u32 bits, u32 i) {
    const u32 per = 64 / bits;
    return static_cast<u32>(w[i / per] >> (i % per * bits)) & ((1u << bits) - 1);
}
ALWAYS_INLINE void words_set(u64* w, u32 bits, u32 i, u32 v) {
    const u32 per = 64 / bits, sh = i % per * bits;
    u64& x = w[i / per];
    x = (x & ~(static_cast<u64>((1u << bits) - 1) << sh)) | (static_cast<u64>(v) << sh);
}

}  // namespace world
