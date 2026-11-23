// SPDX-License-Identifier: GPL-3.0-or-later
// The random sources world generation draws from, ported from the 26.3 jar:
// net.minecraft.world.level.levelgen.{Xoroshiro128PlusPlus, XoroshiroRandomSource, LegacyRandomSource,
// SingleThreadedRandomSource, BitRandomSource, WorldgenRandom, MarsagliaPolarGaussian, RandomSupport}
// and net.minecraft.util.RandomSource.
//
// A world is its seed: every number here has to be the one vanilla draws, in the order vanilla
// draws it, so the methods are vanilla's methods under vanilla's names (snake_case). Tested
// against sequences recorded from the jar (tests/unit/worldgen_test.cpp, Worldgen.Random*).
//
// Vanilla has one interface and picks the implementation at run time; here every place that draws
// knows which source it holds - an aquifer's is a Xoroshiro, a carver's a WorldgenRandom over the
// 48-bit generator - so the sources are separate types and a draw is a few inlined instructions.
// Code that is one algorithm over several sources (the providers of a feature) is a template.
#pragma once

#include <cmath>
#include <string_view>

#include "base/types.hpp"

namespace worldgen {

// Mth.getSeed(x, y, z): the first product is an int's, the second a long's.
ALWAYS_INLINE i64 position_seed(i32 x, i32 y, i32 z) {
    u64 seed = static_cast<u64>(static_cast<i64>(static_cast<i32>(static_cast<u32>(x) * 3129871u))) ^
               (static_cast<u64>(static_cast<i64>(z)) * 116129781ull) ^ static_cast<u64>(static_cast<i64>(y));
    seed = seed * seed * 42317861ull + seed * 11ull;
    return static_cast<i64>(seed) >> 16;
}

// RandomSupport.mixStafford13
ALWAYS_INLINE u64 mix_stafford13(u64 z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

struct Seed128 {
    u64 lo, hi;
};

inline constexpr u64 kGoldenRatio64 = 0x9E3779B97F4A7C15ull;  // RandomSupport.GOLDEN_RATIO_64
inline constexpr u64 kSilverRatio64 = 0x6A09E667F3BCC909ull;  // RandomSupport.SILVER_RATIO_64

// RandomSupport.upgradeSeedTo128bit
ALWAYS_INLINE Seed128 upgrade_seed(i64 seed) {
    const u64 lo = static_cast<u64>(seed) ^ kSilverRatio64, hi = lo + kGoldenRatio64;
    return {mix_stafford13(lo), mix_stafford13(hi)};
}

// RandomSupport.seedFromHashOf: the MD5 of the name, its two halves as big-endian longs.
Seed128 seed_from_hash_of(std::string_view name);
// String.hashCode() of the name (which is UTF-8 here and UTF-16 there).
i32 java_string_hash(std::string_view utf8);
// BiomeManager.obfuscateSeed: what a client is told of the seed (the `login` packet's hashed
// seed) - enough to zoom biomes the way the server does, not enough to know the world.
i64 obfuscate_seed(i64 seed);
// WorldOptions.parseSeed: the `level-seed` of server.properties. False: there is none, the
// world gets a random one. A number is itself, anything else its String.hashCode(). (Digits of
// other scripts, which Long.parseLong reads as digits, are hashed here.)
bool parse_seed(std::string_view text, i64& out);

// MarsagliaPolarGaussian: two numbers a draw, the second kept for the next call.
struct Gaussian {
    double next = 0.0;
    bool have = false;

    template <class Source>
    double draw(Source& r) {
        if (have) {
            have = false;
            return next;
        }
        for (;;) {
            const double x = 2.0 * r.next_double() - 1.0, y = 2.0 * r.next_double() - 1.0;
            const double radius = x * x + y * y;
            if (radius >= 1.0 || radius == 0.0) continue;
            const double multiplier = std::sqrt(-2.0 * std::log(radius) / radius);
            next = y * multiplier;
            have = true;
            return x * multiplier;
        }
    }
};

// What RandomSource gives every source on top of its own draws.
template <class Self>
struct RandomDefaults {
    i32 next_int_between_inclusive(i32 min, i32 max) { return self().next_int(max - min + 1) + min; }
    i32 next_int(i32 origin, i32 bound) { return origin + self().next_int(bound - origin); }
    double triangle(double mean, double spread) {
        const double a = self().next_double();
        return mean + spread * (a - self().next_double());
    }
    float triangle(float mean, float spread) {
        const float a = self().next_float();
        return mean + spread * (a - self().next_float());
    }

  private:
    Self& self() { return static_cast<Self&>(*this); }
};

// BitRandomSource: every draw made of `next(bits)`.
template <class Self>
struct BitRandom : RandomDefaults<Self> {
    using RandomDefaults<Self>::next_int;
    i32 next_int() { return self().next(32); }
    i32 next_int(i32 bound) {
        DCHECK(bound > 0);
        if ((bound & (bound - 1)) == 0)
            return static_cast<i32>((static_cast<i64>(bound) * static_cast<i64>(self().next(31))) >> 31);
        for (;;) {
            const i32 sample = self().next(31), modulo = sample % bound;
            // `sample - modulo + (bound - 1) < 0` as Java computes it: an int that wraps.
            if (static_cast<i32>(static_cast<u32>(sample) - static_cast<u32>(modulo) + static_cast<u32>(bound - 1)) >= 0)
                return modulo;
        }
    }
    i64 next_long() {
        const i32 upper = self().next(32), lower = self().next(32);
        return static_cast<i64>(static_cast<u64>(static_cast<i64>(upper)) << 32) + static_cast<i64>(lower);
    }
    bool next_boolean() { return self().next(1) != 0; }
    float next_float() { return static_cast<float>(self().next(24)) * 5.9604645E-8f; }
    double next_double() {
        const i32 upper = self().next(26), lower = self().next(27);
        return static_cast<double>((static_cast<i64>(upper) << 27) + static_cast<i64>(lower)) * 1.1102230246251565E-16;
    }
    void consume_count(i32 rounds) {
        for (i32 i = 0; i < rounds; ++i) self().next(32);
    }

  private:
    Self& self() { return static_cast<Self&>(*this); }
};

struct XoroshiroPositional;
struct LegacyPositional;

// XoroshiroRandomSource over Xoroshiro128PlusPlus.
struct XoroshiroRandom : RandomDefaults<XoroshiroRandom> {
    using RandomDefaults<XoroshiroRandom>::next_int;
    u64 lo, hi;
    Gaussian gaussian;

    explicit XoroshiroRandom(i64 seed) : XoroshiroRandom(upgrade_seed(seed)) {}
    explicit XoroshiroRandom(Seed128 s) : XoroshiroRandom(s.lo, s.hi) {}
    XoroshiroRandom(u64 seed_lo, u64 seed_hi) : lo(seed_lo), hi(seed_hi) {
        if ((lo | hi) == 0) {
            lo = kGoldenRatio64;
            hi = kSilverRatio64;
        }
    }

    ALWAYS_INLINE u64 bits() {
        const u64 s0 = lo;
        u64 s1 = hi;
        const u64 result = std::rotl(s0 + s1, 17) + s0;
        s1 ^= s0;
        lo = std::rotl(s0, 49) ^ s1 ^ (s1 << 21);
        hi = std::rotl(s1, 28);
        return result;
    }
    i64 next_long() { return static_cast<i64>(bits()); }
    i32 next_int() { return static_cast<i32>(bits()); }
    i32 next_int(i32 bound) {
        DCHECK(bound > 0);
        const u64 b = static_cast<u32>(bound);
        u64 multiplied = static_cast<u64>(static_cast<u32>(next_int())) * b;
        u64 fraction = multiplied & 0xFFFFFFFFull;
        if (fraction < b) {
            const u32 start = (~static_cast<u32>(bound) + 1u) % static_cast<u32>(bound);
            while (fraction < start) {
                multiplied = static_cast<u64>(static_cast<u32>(next_int())) * b;
                fraction = multiplied & 0xFFFFFFFFull;
            }
        }
        return static_cast<i32>(multiplied >> 32);
    }
    bool next_boolean() { return (bits() & 1) != 0; }
    float next_float() { return static_cast<float>(bits() >> 40) * 5.9604645E-8f; }
    double next_double() { return static_cast<double>(bits() >> 11) * 1.1102230246251565E-16; }
    double next_gaussian() { return gaussian.draw(*this); }
    void set_seed(i64 seed) {
        const Seed128 s = upgrade_seed(seed);
        *this = XoroshiroRandom(s.lo, s.hi);  // (the gaussian's second number goes with it)
    }
    void consume_count(i32 rounds) {
        for (i32 i = 0; i < rounds; ++i) bits();
    }
    XoroshiroRandom fork() {
        const u64 a = bits(), b = bits();
        return {a, b};
    }
    inline XoroshiroPositional fork_positional();
};

// XoroshiroRandomSource.XoroshiroPositionalRandomFactory: a source per position, per name.
struct XoroshiroPositional {
    u64 lo, hi;
    ALWAYS_INLINE XoroshiroRandom at(i32 x, i32 y, i32 z) const { return {static_cast<u64>(position_seed(x, y, z)) ^ lo, hi}; }
    XoroshiroRandom from_hash_of(std::string_view name) const { return from_hash(seed_from_hash_of(name)); }
    // ...of a name whose hash is known (the generated tables hold those of every noise).
    XoroshiroRandom from_hash(Seed128 s) const { return {s.lo ^ lo, s.hi ^ hi}; }
    XoroshiroRandom from_seed(i64 seed) const { return {static_cast<u64>(seed) ^ lo, static_cast<u64>(seed) ^ hi}; }
};

inline XoroshiroPositional XoroshiroRandom::fork_positional() {
    const u64 a = bits(), b = bits();
    return {a, b};
}

// LegacyRandomSource and SingleThreadedRandomSource: java.util.Random's 48 bits. (The two differ
// in what they do about a second thread; their numbers are the same.)
struct LegacyRandom : BitRandom<LegacyRandom> {
    static constexpr u64 kMultiplier = 25214903917ull, kIncrement = 11ull, kMask = (1ull << 48) - 1;
    u64 seed = 0;
    Gaussian gaussian;

    explicit LegacyRandom(i64 s) { set_seed(s); }
    void set_seed(i64 s) {
        seed = (static_cast<u64>(s) ^ kMultiplier) & kMask;
        gaussian.have = false;
    }
    ALWAYS_INLINE i32 next(u32 bits) {
        seed = (seed * kMultiplier + kIncrement) & kMask;
        return static_cast<i32>(seed >> (48 - bits));
    }
    double next_gaussian() { return gaussian.draw(*this); }
    LegacyRandom fork() { return LegacyRandom(next_long()); }
    inline LegacyPositional fork_positional();
};

// LegacyRandomSource.LegacyPositionalRandomFactory
struct LegacyPositional {
    i64 seed;
    ALWAYS_INLINE LegacyRandom at(i32 x, i32 y, i32 z) const { return LegacyRandom(position_seed(x, y, z) ^ seed); }
    LegacyRandom from_hash_of(std::string_view name) const {
        return LegacyRandom(static_cast<i64>(java_string_hash(name)) ^ seed);
    }
    LegacyRandom from_seed(i64 s) const { return LegacyRandom(s); }
};

inline LegacyPositional LegacyRandom::fork_positional() { return {next_long()}; }

// WorldgenRandom over one of the two: a BitRandomSource whose bits are the inner source's, and
// the seeds a chunk's carvers, decoration and features begin from. Its gaussian is its own, and
// vanilla does not forget the second number when the seed is set (WorldgenRandom.setSeed
// overrides the method that would).
template <class Inner>
struct WorldgenRandom : BitRandom<WorldgenRandom<Inner>> {
    Inner inner;
    Gaussian gaussian;

    explicit WorldgenRandom(Inner source) : inner(source) {}
    ALWAYS_INLINE i32 next(u32 bits);
    void set_seed(i64 seed) { inner.set_seed(seed); }
    double next_gaussian() { return gaussian.draw(*this); }
    Inner fork() { return inner.fork(); }
    auto fork_positional() { return inner.fork_positional(); }

    i64 set_decoration_seed(i64 seed, i32 chunk_x, i32 chunk_z) {
        set_seed(seed);
        const u64 x_scale = static_cast<u64>(this->next_long()) | 1, z_scale = static_cast<u64>(this->next_long()) | 1;
        const i64 result = static_cast<i64>(
            (static_cast<u64>(static_cast<i64>(chunk_x)) * x_scale + static_cast<u64>(static_cast<i64>(chunk_z)) * z_scale) ^
            static_cast<u64>(seed));
        set_seed(result);
        return result;
    }
    void set_feature_seed(i64 seed, i32 index, i32 step) {
        // (seed + index) + 10000 * step: the product is an int's.
        set_seed(static_cast<i64>(static_cast<u64>(seed) + static_cast<u64>(static_cast<i64>(index)) +
                                  static_cast<u64>(static_cast<i64>(static_cast<i32>(10000u * static_cast<u32>(step))))));
    }
    void set_large_feature_seed(i64 seed, i32 chunk_x, i32 chunk_z) {
        set_seed(seed);
        const u64 x_scale = static_cast<u64>(this->next_long()), z_scale = static_cast<u64>(this->next_long());
        set_seed(static_cast<i64>((static_cast<u64>(static_cast<i64>(chunk_x)) * x_scale) ^
                                  (static_cast<u64>(static_cast<i64>(chunk_z)) * z_scale) ^ static_cast<u64>(seed)));
    }
    void set_large_feature_with_salt(i64 seed, i32 x, i32 z, i32 salt) {
        set_seed(static_cast<i64>(static_cast<u64>(static_cast<i64>(x)) * 341873128712ull +
                                  static_cast<u64>(static_cast<i64>(z)) * 132897987541ull + static_cast<u64>(seed) +
                                  static_cast<u64>(static_cast<i64>(salt))));
    }
};

template <>
ALWAYS_INLINE i32 WorldgenRandom<LegacyRandom>::next(u32 bits) {
    return inner.next(bits);
}
template <>
ALWAYS_INLINE i32 WorldgenRandom<XoroshiroRandom>::next(u32 bits) {
    return static_cast<i32>(inner.bits() >> (64 - bits));
}

}  // namespace worldgen
