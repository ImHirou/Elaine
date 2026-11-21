// SPDX-License-Identifier: GPL-3.0-or-later
// Wire primitives: unchecked pointer writers ("put_*") and a sticky-error Reader.
//
// Writers never check capacity - the caller reserved an upper bound (see base::ByteBuf).
// Reader never throws: a short or malformed read sets ok=false and yields zeros; check `ok`
// once after decoding a whole packet.
#pragma once

#include <span>
#include <string_view>

#include "base/types.hpp"
#include "proto/varint.hpp"

namespace proto {

// ---------------------------------------------------------------- writers

ALWAYS_INLINE u8* put_u8(u8* p, u8 v) {
    *p = v;
    return p + 1;
}
ALWAYS_INLINE u8* put_bool(u8* p, bool v) {
    *p = v ? 1 : 0;
    return p + 1;
}
ALWAYS_INLINE u8* put_be16(u8* p, u16 v) {
    store_be16(p, v);
    return p + 2;
}
ALWAYS_INLINE u8* put_be32(u8* p, u32 v) {
    store_be32(p, v);
    return p + 4;
}
ALWAYS_INLINE u8* put_be64(u8* p, u64 v) {
    store_be64(p, v);
    return p + 8;
}
ALWAYS_INLINE u8* put_f32(u8* p, float v) { return put_be32(p, std::bit_cast<u32>(v)); }
ALWAYS_INLINE u8* put_f64(u8* p, double v) { return put_be64(p, std::bit_cast<u64>(v)); }
ALWAYS_INLINE u8* put_bytes(u8* p, const void* src, usize n) {
    std::memcpy(p, src, n);
    return p + n;
}
ALWAYS_INLINE u8* put_uuid(u8* p, Uuid u) {
    store_be64(p, u.hi);
    store_be64(p + 8, u.lo);
    return p + 16;
}
ALWAYS_INLINE u8* put_str(u8* p, std::string_view s) {
    p = put_varint(p, static_cast<u32>(s.size()));
    return put_bytes(p, s.data(), s.size());
}
ALWAYS_INLINE constexpr usize str_bound(std::string_view s) { return kVarIntMaxBytes + s.size(); }

// VarLong: same encoding as VarInt, up to 10 bytes. Rare (multi-block updates), so a plain loop.
ALWAYS_INLINE u8* put_varlong(u8* p, u64 v) {
    while (v >= 0x80) {
        *p++ = static_cast<u8>(v | 0x80);
        v >>= 7;
    }
    *p++ = static_cast<u8>(v);
    return p;
}
inline constexpr u32 kVarLongMaxBytes = 10;

// ByteBufCodecs.BIT_SET (26.3): BitSet.toByteArray() as a byte array - VarInt byte count, then the
// bytes little-endian with no trailing zero byte. Not FriendlyByteBuf.writeBitSet, which writes
// longs. The one wide store stays inside kBitSetMaxBytes.
ALWAYS_INLINE u8* put_bit_set(u8* p, u64 bits) {
    const u32 n = (static_cast<u32>(std::bit_width(bits)) + 7) / 8;
    *p = static_cast<u8>(n);
    store_le<u64>(p + 1, bits);
    return p + 1 + n;
}
inline constexpr u32 kBitSetMaxBytes = 9;

// BlockPos packed as x:26 | z:26 | y:12.
ALWAYS_INLINE u64 pack_block_pos(i32 x, i32 y, i32 z) {
    return ((static_cast<u64>(x) & 0x3FFFFFF) << 38) | ((static_cast<u64>(z) & 0x3FFFFFF) << 12) | (static_cast<u64>(y) & 0xFFF);
}
struct BlockPos {
    i32 x, y, z;
    bool operator==(const BlockPos&) const = default;
};
ALWAYS_INLINE BlockPos unpack_block_pos(u64 v) {  // each field is sign-extended (BlockPos.getX/getY/getZ)
    return {static_cast<i32>(static_cast<i64>(v) >> 38), static_cast<i32>(static_cast<i64>(v << 52) >> 52),
            static_cast<i32>(static_cast<i64>(v << 26) >> 38)};
}
// SectionPos.asLong(): x:22 | z:22 | y:20.
ALWAYS_INLINE u64 pack_section_pos(i32 x, i32 y, i32 z) {
    return ((static_cast<u64>(x) & 0x3FFFFF) << 42) | ((static_cast<u64>(z) & 0x3FFFFF) << 20) | (static_cast<u64>(y) & 0xFFFFF);
}

// ---------------------------------------------------------------- reader

struct Reader {
    const u8* p;
    const u8* end;
    bool ok = true;

    Reader(const u8* data, usize n) : p(data), end(data + n) {}
    explicit Reader(std::span<const u8> s) : p(s.data()), end(s.data() + s.size()) {}

    ALWAYS_INLINE usize remaining() const { return static_cast<usize>(end - p); }

    ALWAYS_INLINE bool need(usize n) {
        if (UNLIKELY(remaining() < n)) {
            ok = false;
            p = end;
            return false;
        }
        return true;
    }
    ALWAYS_INLINE u8 u8v() {
        if (!need(1)) return 0;
        return *p++;
    }
    ALWAYS_INLINE bool boolean() { return u8v() != 0; }
    ALWAYS_INLINE u16 be16() {
        if (!need(2)) return 0;
        u16 v = load_be16(p);
        p += 2;
        return v;
    }
    ALWAYS_INLINE u32 be32() {
        if (!need(4)) return 0;
        u32 v = load_be32(p);
        p += 4;
        return v;
    }
    ALWAYS_INLINE u64 be64() {
        if (!need(8)) return 0;
        u64 v = load_be64(p);
        p += 8;
        return v;
    }
    ALWAYS_INLINE float f32() { return std::bit_cast<float>(be32()); }
    ALWAYS_INLINE double f64() { return std::bit_cast<double>(be64()); }
    ALWAYS_INLINE Uuid uuid() {
        Uuid u;
        u.hi = be64();
        u.lo = be64();
        return u;
    }

    ALWAYS_INLINE u32 varint() {
        u32 v = 0;
        const u32 n = get_varint(p, end, v);
        if (UNLIKELY(n == 0 || n == ~0u)) {
            ok = false;
            p = end;
            return 0;
        }
        p += n;
        return v;
    }
    // Zero-copy views into the packet; valid only while the underlying buffer is.
    ALWAYS_INLINE std::span<const u8> bytes(usize n) {
        if (!need(n)) return {};
        std::span<const u8> s(p, n);
        p += n;
        return s;
    }
    ALWAYS_INLINE std::string_view str(u32 max_bytes) {
        const u32 n = varint();
        if (UNLIKELY(n > max_bytes)) {
            ok = false;
            p = end;
            return {};
        }
        auto s = bytes(n);
        return {reinterpret_cast<const char*>(s.data()), s.size()};
    }
    ALWAYS_INLINE std::span<const u8> byte_array(u32 max_bytes) {
        const u32 n = varint();
        if (UNLIKELY(n > max_bytes)) {
            ok = false;
            p = end;
            return {};
        }
        return bytes(n);
    }
    ALWAYS_INLINE bool done() const { return ok && p == end; }
};

// ---------------------------------------------------------------- text helpers (cold)

// String.length() of a UTF-8 string: UTF-16 code units, which is what vanilla's limits count
// (Utf8String.read). A code point beyond the BMP is two.
inline u32 utf16_length(std::string_view s) {
    u32 n = 0;
    for (const char c : s) {
        const u8 b = static_cast<u8>(c);
        n += (b & 0xC0) != 0x80;  // not a continuation byte: a new code point
        n += b >= 0xF0;           // four bytes: a surrogate pair
    }
    return n;
}

// Java "modified UTF-8" as used by NBT strings: NUL -> C0 80, astral code points -> surrogate pair.
u8* put_mutf8(u8* p, std::string_view utf8);
inline constexpr usize mutf8_bound(std::string_view s) { return s.size() * 2 + 2; }

// A text component holding plain text, as network NBT (nameless root): TAG_String.
u8* put_nbt_text(u8* p, std::string_view utf8);
inline constexpr usize nbt_text_bound(std::string_view s) { return 3 + mutf8_bound(s); }

// Append `s` JSON-escaped (without surrounding quotes).
void json_escape(std::string& out, std::string_view s);

}  // namespace proto
