// Fixed-width aliases, branch/inlining hints, byte-order and bit helpers.
// Included everywhere; keep it free of heavy headers.
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using usize = std::size_t;

#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#define ALWAYS_INLINE inline __attribute__((always_inline))
#define NOINLINE __attribute__((noinline))
#define COLD __attribute__((cold))
#define HOT __attribute__((hot))
#define RESTRICT __restrict__
#define ASSUME(x) __attribute__((assume(x)))

// One cache line. Used to keep producer/consumer indices of lock-free queues apart.
inline constexpr usize kCacheLine = 64;
#define CACHE_ALIGNED alignas(kCacheLine)

// Debug-only invariant check, compiled out of release builds. (It is deliberately NOT turned into
// ASSUME there: a wrong check would become undefined behaviour instead of a missed assert. Use
// ASSUME explicitly, with a side-effect-free condition, where a measured hint is wanted.)
#ifdef NDEBUG
#define DCHECK(x) ((void)0)
#else
#include <cassert>
#define DCHECK(x) assert(x)
#endif

// Unaligned loads/stores. memcpy compiles to a single mov; the protocol is big-endian.
template <class T>
ALWAYS_INLINE T load_le(const void* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}
template <class T>
ALWAYS_INLINE void store_le(void* p, T v) {
    std::memcpy(p, &v, sizeof(T));
}
ALWAYS_INLINE u16 load_be16(const void* p) { return __builtin_bswap16(load_le<u16>(p)); }
ALWAYS_INLINE u32 load_be32(const void* p) { return __builtin_bswap32(load_le<u32>(p)); }
ALWAYS_INLINE u64 load_be64(const void* p) { return __builtin_bswap64(load_le<u64>(p)); }
ALWAYS_INLINE void store_be16(void* p, u16 v) { store_le<u16>(p, __builtin_bswap16(v)); }
ALWAYS_INLINE void store_be32(void* p, u32 v) { store_le<u32>(p, __builtin_bswap32(v)); }
ALWAYS_INLINE void store_be64(void* p, u64 v) { store_le<u64>(p, __builtin_bswap64(v)); }

struct Uuid {
    u64 hi = 0;
    u64 lo = 0;
    bool operator==(const Uuid&) const = default;
};
