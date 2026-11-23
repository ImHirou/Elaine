// SPDX-License-Identifier: GPL-3.0-or-later
// World generation against what vanilla computes.
//
// tests/unit/data/worldgen_*.bin are scripts written by tools/mcgen/extract/WorldgenGolden.java
// (`mcgen.py worldgen golden --emit`), which runs the 26.3 jar's own classes: every record says
// what was asked and what vanilla answered, floats and doubles as their bits. A test replays the
// script on src/worldgen and compares bit for bit - nothing is "near enough". The layouts are in
// the Java file, beside the code that writes them.
#include <gtest/gtest.h>

#include <bit>
#include <string>
#include <variant>

#include "worldgen/random.hpp"

using namespace worldgen;

namespace {

constexpr unsigned char kRandom[] = {
#embed "data/worldgen_random.bin"
};

// A script being read.
struct In {
    const unsigned char* p;
    const unsigned char* end;

    template <usize N>
    explicit In(const unsigned char (&bytes)[N]) : p(bytes), end(bytes + N) {}

    template <class T>
    T num() {
        EXPECT_LE(p + sizeof(T), end);
        const T v = load_le<T>(p);
        p += sizeof(T);
        return v;
    }
    u8 u8_() { return num<u8>(); }
    i32 i32_() { return num<i32>(); }
    i64 i64_() { return num<i64>(); }
    u32 f32_bits() { return num<u32>(); }
    u64 f64_bits() { return num<u64>(); }
    float f32_() { return std::bit_cast<float>(num<u32>()); }
    double f64_() { return std::bit_cast<double>(num<u64>()); }
    std::string str() {
        const u32 n = num<u32>();
        std::string s(reinterpret_cast<const char*>(p), n);
        p += n;
        return s;
    }
    void magic(std::string_view want) {
        ASSERT_EQ(std::string_view(reinterpret_cast<const char*>(p), want.size()), want);
        p += want.size();
    }
};

u32 bits(float v) { return std::bit_cast<u32>(v); }
u64 bits(double v) { return std::bit_cast<u64>(v); }

// The sources a script may hold: a fork of a WorldgenRandom is its inner source's.
using AnyRandom = std::variant<XoroshiroRandom, LegacyRandom, WorldgenRandom<LegacyRandom>, WorldgenRandom<XoroshiroRandom>>;

AnyRandom make_source(u32 kind, i64 seed) {
    switch (kind) {
        case 0: return XoroshiroRandom(seed);
        case 1:
        case 2: return LegacyRandom(seed);  // LegacyRandomSource, SingleThreadedRandomSource
        case 3: return WorldgenRandom<LegacyRandom>(LegacyRandom(seed));
        default: return WorldgenRandom<XoroshiroRandom>(XoroshiroRandom(seed));
    }
}

template <class T>
constexpr bool kIsWorldgen = false;
template <class I>
constexpr bool kIsWorldgen<WorldgenRandom<I>> = true;

// One call of the script on `any`; false when vanilla answered otherwise.
bool replay_call(In& in, AnyRandom& any, std::string& what) {
    const u32 op = in.u8_();
    what = "op " + std::to_string(op);
    bool ok = true;
    AnyRandom next = any;
    bool replaced = false;
    std::visit(
        [&](auto& r) {
            using R = std::decay_t<decltype(r)>;
            switch (op) {
                case 0: ok = r.next_int() == in.i32_(); break;
                case 1: {
                    const i32 bound = in.i32_();
                    what += " bound " + std::to_string(bound);
                    ok = r.next_int(bound) == in.i32_();
                    break;
                }
                case 2: ok = r.next_long() == in.i64_(); break;
                case 3: ok = r.next_boolean() == (in.u8_() != 0); break;
                case 4: ok = bits(r.next_float()) == in.f32_bits(); break;
                case 5: ok = bits(r.next_double()) == in.f64_bits(); break;
                case 6: ok = bits(r.next_gaussian()) == in.f64_bits(); break;
                case 7:
                    next = r.fork();
                    replaced = true;
                    break;
                case 8: {
                    const i32 x = in.i32_(), y = in.i32_(), z = in.i32_();
                    next = r.fork_positional().at(x, y, z);
                    replaced = true;
                    break;
                }
                case 9:
                    next = r.fork_positional().from_hash_of(in.str());
                    replaced = true;
                    break;
                case 10:
                    next = r.fork_positional().from_seed(in.i64_());
                    replaced = true;
                    break;
                case 11: r.set_seed(in.i64_()); break;
                case 12: r.consume_count(in.i32_()); break;
                case 13: {
                    const i32 min = in.i32_(), max = in.i32_();
                    ok = r.next_int_between_inclusive(min, max) == in.i32_();
                    break;
                }
                case 14: {
                    const double mean = in.f64_(), spread = in.f64_();
                    ok = bits(r.triangle(mean, spread)) == in.f64_bits();
                    break;
                }
                case 15: {
                    const float mean = in.f32_(), spread = in.f32_();
                    ok = bits(r.triangle(mean, spread)) == in.f32_bits();
                    break;
                }
                default:
                    if constexpr (kIsWorldgen<R>) {
                        const i64 seed = in.i64_();
                        if (op == 16) {
                            const i32 x = in.i32_(), z = in.i32_();
                            ok = r.set_decoration_seed(seed, x, z) == in.i64_();
                        } else if (op == 17) {
                            const i32 index = in.i32_(), step = in.i32_();
                            r.set_feature_seed(seed, index, step);
                        } else if (op == 18) {
                            const i32 x = in.i32_(), z = in.i32_();
                            r.set_large_feature_seed(seed, x, z);
                        } else {
                            const i32 x = in.i32_(), z = in.i32_(), salt = in.i32_();
                            r.set_large_feature_with_salt(seed, x, z, salt);
                        }
                    } else {
                        ok = false;  // the script asks this of a WorldgenRandom only
                    }
            }
        },
        any);
    if (replaced) any = next;
    return ok;
}

}  // namespace

TEST(Worldgen, RandomSeedsOfPositionsNamesAndStrings) {
    In in(kRandom);
    in.magic("WGRAND01");
    for (u32 n = in.num<u32>(), i = 0; i < n; ++i) {
        const i32 x = in.i32_(), y = in.i32_(), z = in.i32_();
        ASSERT_EQ(position_seed(x, y, z), in.i64_()) << "Mth.getSeed(" << x << ", " << y << ", " << z << ")";
    }
    for (u32 n = in.num<u32>(), i = 0; i < n; ++i) {
        const i64 v = in.i64_();
        ASSERT_EQ(static_cast<i64>(mix_stafford13(static_cast<u64>(v))), in.i64_()) << "mixStafford13(" << v << ")";
        const Seed128 wide = upgrade_seed(v);
        ASSERT_EQ(static_cast<i64>(wide.lo), in.i64_()) << "upgradeSeedTo128bit(" << v << ").lo";
        ASSERT_EQ(static_cast<i64>(wide.hi), in.i64_()) << "upgradeSeedTo128bit(" << v << ").hi";
        ASSERT_EQ(obfuscate_seed(v), in.i64_()) << "obfuscateSeed(" << v << ")";
    }
    for (u32 n = in.num<u32>(), i = 0; i < n; ++i) {
        const std::string name = in.str();
        const Seed128 hash = seed_from_hash_of(name);
        ASSERT_EQ(static_cast<i64>(hash.lo), in.i64_()) << "seedFromHashOf(" << name << ").lo";
        ASSERT_EQ(static_cast<i64>(hash.hi), in.i64_()) << "seedFromHashOf(" << name << ").hi";
        ASSERT_EQ(java_string_hash(name), in.i32_()) << "hashCode of " << name;
    }
    u32 seeds = 0;
    for (u32 n = in.num<u32>(), i = 0; i < n; ++i) {
        const std::string text = in.str();
        const bool present = in.u8_() != 0;
        const i64 want = in.i64_();
        i64 got = 0;
        ASSERT_EQ(parse_seed(text, got), present) << "parseSeed(\"" << text << "\")";
        if (present) ASSERT_EQ(got, want) << "parseSeed(\"" << text << "\")";
        ++seeds;
    }
    EXPECT_GT(seeds, 20u);
}

TEST(Worldgen, RandomSourcesDrawWhatVanillaDraws) {
    In in(kRandom);
    in.magic("WGRAND01");
    // Past the first part of the file (the test above reads it).
    for (u32 n = in.num<u32>(), i = 0; i < n; ++i) in.p += 12 + 8;
    for (u32 n = in.num<u32>(), i = 0; i < n; ++i) in.p += 5 * 8;
    for (u32 n = in.num<u32>(), i = 0; i < n; ++i) {
        in.str();
        in.p += 8 + 8 + 4;
    }
    for (u32 n = in.num<u32>(), i = 0; i < n; ++i) {
        in.str();
        in.p += 1 + 8;
    }

    u32 calls = 0;
    const u32 cases = in.num<u32>();
    ASSERT_EQ(cases, 5u * 24u);
    for (u32 c = 0; c < cases; ++c) {
        const u32 kind = in.u8_();
        const i64 seed = in.i64_();
        AnyRandom r = make_source(kind, seed);
        for (u32 n = in.num<u32>(), i = 0; i < n; ++i, ++calls) {
            std::string what;
            ASSERT_TRUE(replay_call(in, r, what))
                << "source kind " << kind << ", seed " << seed << ", call " << i << ": " << what;
        }
    }
    EXPECT_EQ(calls, 5u * 24u * 96u);

    // A positional factory asked many times.
    for (u32 kind = 0; kind < 2; ++kind) {
        const u32 n = in.num<u32>();
        ASSERT_EQ(n, 512u);
        XoroshiroRandom xoroshiro(20261123);
        LegacyRandom legacy(20261123);
        const XoroshiroPositional xp = xoroshiro.fork_positional();
        const LegacyPositional lp = legacy.fork_positional();
        for (u32 i = 0; i < n; ++i) {
            const i32 x = in.i32_(), y = in.i32_(), z = in.i32_();
            const i32 want_int = in.i32_();
            const u32 want_float = in.f32_bits();
            const u64 want_double = in.f64_bits();
            if (kind == 0) {
                XoroshiroRandom r = xp.at(x, y, z);
                ASSERT_EQ(r.next_int(10), want_int) << "at " << x << " " << y << " " << z;
                ASSERT_EQ(bits(r.next_float()), want_float);
                ASSERT_EQ(bits(r.next_double()), want_double);
            } else {
                LegacyRandom r = lp.at(x, y, z);
                ASSERT_EQ(r.next_int(10), want_int) << "at " << x << " " << y << " " << z;
                ASSERT_EQ(bits(r.next_float()), want_float);
                ASSERT_EQ(bits(r.next_double()), want_double);
            }
        }
    }
    EXPECT_EQ(in.p, in.end) << "the script was not read to its end";
}
