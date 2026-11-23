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
#include <cstring>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "world/block.hpp"
#include "world/chunk_io.hpp"
#include "world/region.hpp"
#include "worldgen/carver.hpp"
#include "worldgen/climate.hpp"
#include "worldgen/density.hpp"
#include "worldgen/generator.hpp"
#include "worldgen/material.hpp"
#include "worldgen/noise.hpp"
#include "worldgen/random.hpp"
#include "worldgen/terrain.hpp"

#include PROTO_GEN_WORLDGEN_CLIMATE_HPP

using namespace worldgen;
namespace wg = proto::gen::worldgen;

namespace {

constexpr unsigned char kRandom[] = {
#embed "data/worldgen_random.bin"
};
constexpr unsigned char kNoise[] = {
#embed "data/worldgen_noise.bin"
};
constexpr unsigned char kDensity[] = {
#embed "data/worldgen_density.bin"
};
constexpr unsigned char kClimate[] = {
#embed "data/worldgen_climate.bin"
};
constexpr unsigned char kTerrain[] = {
#embed "data/worldgen_terrain.bin"
};
constexpr unsigned char kSpawn[] = {
#embed "data/worldgen_spawn.bin"
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
        if (present) {
            ASSERT_EQ(got, want) << "parseSeed(\"" << text << "\")";
        }
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

// The noise the script names: one of the registry's, or one of old_blended_noise's three.
const Noise* find_noise(const Noises& noises, std::string_view name) {
    if (name == "blended:min_limit") return &noises.blended_min_limit();
    if (name == "blended:max_limit") return &noises.blended_max_limit();
    if (name == "blended:main") return &noises.blended_main();
    for (u32 i = 0; i < wg::noise::kCount; ++i)
        if (wg::kNoises[i].name == name) return &noises[i];
    return nullptr;
}

// What WorldgenGolden.prefill puts into a buffer before a noise is added to it.
float prefill(u32 kind, u32 index) { return kind == 0 ? 0.0f : static_cast<float>(index) * 0.125f - 3.0f; }

TEST(Worldgen, NoisesAreWhatTheSeedMakesThem) {
    In in(kNoise);
    in.magic("WGNOIS01");
    const i64 seed = in.i64_();
    const Noises noises(seed);
    const u32 count = in.num<u32>();
    ASSERT_EQ(count, wg::noise::kCount + 3);
    u32 layers = 0, points = 0, cells = 0;
    for (u32 n = 0; n < count; ++n) {
        const std::string name = in.str();
        const Noise* noise = find_noise(noises, name);
        ASSERT_NE(noise, nullptr) << name;
        ASSERT_EQ(noise->smeared, in.u8_() != 0) << name;
        ASSERT_EQ(noise->count, in.num<u32>()) << name;
        // Each layer: its place in the stack, and what the seed drew for it.
        for (u32 i = 0; i < noise->count; ++i, ++layers) {
            const Layer& l = noise->layers[i];
            ASSERT_EQ(bits(l.frequency), in.f64_bits()) << name << " layer " << i;
            ASSERT_EQ(bits(l.amplitude), in.f32_bits()) << name << " layer " << i;
            ASSERT_EQ(bits(l.noise.fudge_y_scale), in.f64_bits()) << name << " layer " << i;
            ASSERT_EQ(bits(l.noise.offset_x), in.f64_bits()) << name << " layer " << i;
            ASSERT_EQ(bits(l.noise.offset_y), in.f64_bits()) << name << " layer " << i;
            ASSERT_EQ(bits(l.noise.offset_z), in.f64_bits()) << name << " layer " << i;
            ASSERT_EQ(std::memcmp(l.noise.perms, in.p, 256), 0) << name << " layer " << i << ": the permutation";
            in.p += 256;
        }
        // Its value at points...
        for (u32 k = in.num<u32>(), i = 0; i < k; ++i, ++points) {
            const double x = in.f64_(), y = in.f64_(), z = in.f64_();
            ASSERT_EQ(bits(noise->get(x, y, z)), in.f32_bits()) << name << " at " << x << ", " << y << ", " << z;
            ASSERT_EQ(bits(noise->get(x, z)), in.f32_bits()) << name << " at " << x << ", " << z << " (two coordinates)";
        }
        // ...and over volumes.
        for (u32 k = in.num<u32>(), i = 0; i < k; ++i) {
            Volume v;
            v.size_x = in.i32_(), v.size_y = in.i32_(), v.size_z = in.i32_();
            v.min_x = in.i32_(), v.min_y = in.i32_(), v.min_z = in.i32_();
            v.step_x = in.i32_(), v.step_y = in.i32_(), v.step_z = in.i32_();
            const double xz_scale = in.f64_(), y_scale = in.f64_();
            const float amplitude = in.f32_();
            const u32 kind = in.u8_();
            std::vector<float> buffer(v.size());
            for (u32 j = 0; j < v.size(); ++j) buffer[j] = prefill(kind, j);
            noise->add_to_volume(buffer.data(), v, xz_scale, y_scale, amplitude);
            for (u32 j = 0; j < v.size(); ++j, ++cells)
                ASSERT_EQ(bits(buffer[j]), in.f32_bits())
                    << name << ": volume " << i << " (from " << v.min_x << ", " << v.min_y << ", " << v.min_z << ", scales "
                    << xz_scale << " and " << y_scale << "), cell " << j;
        }
    }
    EXPECT_EQ(in.p, in.end) << "the script was not read to its end";
    EXPECT_GT(layers, 300u);
    EXPECT_EQ(points, count * 48);
    EXPECT_EQ(cells, count * (4 * 12 * 3 + 3 * 7 * 3 + 2 * 2));
}

// Sequences of questions to the density functions - a value at a block, a volume - each on a
// context of its own: without caches, where every answer stands by itself, and with them, where
// what a cache holds from the question before decides which of vanilla's two ways answers.
TEST(Worldgen, DensityFunctionsAnswerAsVanillasSamplers) {
    In in(kDensity);
    in.magic("WGDENS01");
    const Noises noises(in.i64_());
    // The script's functions, by the names the generated program has them under.
    std::vector<u32> nodes;
    std::vector<std::string> names;
    for (u32 n = in.num<u32>(), i = 0; i < n; ++i) {
        names.push_back(in.str());
        ASSERT_LT(i, std::size(wg::kRoots));
        ASSERT_EQ(wg::kRoots[i].name, names.back());
        nodes.push_back(wg::kRoots[i].node);
    }
    u32 values = 0, volumes = 0, cells = 0;
    const u32 sessions = in.num<u32>();
    for (u32 s = 0; s < sessions; ++s) {
        Sampler sampler(noises, in.u8_() != 0);
        for (u32 n = in.num<u32>(), q = 0; q < n; ++q) {
            const u32 root = in.num<u32>();
            ASSERT_LT(root, nodes.size());
            if (in.u8_() == 0) {
                const i32 x = in.i32_(), y = in.i32_(), z = in.i32_();
                const u32 want = in.f32_bits();
                const float got = sampler.value(nodes[root], x, y, z);
                ASSERT_EQ(bits(got), want) << "session " << s << ", question " << q << ": " << names[root] << " at " << x << ", "
                                           << y << ", " << z << " is " << got << ", vanilla's is " << std::bit_cast<float>(want);
                ++values;
            } else {
                Volume v;
                v.size_x = in.i32_(), v.size_y = in.i32_(), v.size_z = in.i32_();
                v.min_x = in.i32_(), v.min_y = in.i32_(), v.min_z = in.i32_();
                v.step_x = in.i32_(), v.step_y = in.i32_(), v.step_z = in.i32_();
                std::vector<float> buffer(v.size());
                sampler.volume(nodes[root], buffer.data(), v);
                for (u32 j = 0; j < v.size(); ++j, ++cells) {
                    const u32 want = in.f32_bits();
                    ASSERT_EQ(bits(buffer[j]), want)
                        << "session " << s << ", question " << q << ": " << names[root] << " over " << v.size_x << "x" << v.size_y
                        << "x" << v.size_z << " from " << v.min_x << ", " << v.min_y << ", " << v.min_z << " in steps of "
                        << v.step_x << ", " << v.step_y << ", " << v.step_z << ", cell " << j << " is " << buffer[j]
                        << ", vanilla's is " << std::bit_cast<float>(want);
                }
                ++volumes;
            }
        }
    }
    EXPECT_EQ(in.p, in.end) << "the script was not read to its end";
    EXPECT_GT(values, 1400u);
    EXPECT_GT(volumes, 300u);
    EXPECT_GT(cells, 60000u);
}

// Which biome a climate is. Where two are equally near, the one the search before found stays:
// the script's searches come in sequences, each begun with nothing found yet.
TEST(Worldgen, ClimateSearchFindsVanillasBiome) {
    In in(kClimate);
    in.magic("WGCLIM01");
    in.i64_();
    std::vector<u32> biome;  // the script's biomes as network ids
    for (u32 n = in.num<u32>(), i = 0; i < n; ++i) {
        const std::string name = in.str();
        biome.push_back(world::find_biome(name));
        ASSERT_NE(biome.back(), world::kNoId) << name;
    }
    u32 searches = 0, kept = 0;
    for (u32 n = in.num<u32>(), s = 0; s < n; ++s) {
        i32 last = kNoLeaf;
        for (u32 k = in.num<u32>(), i = 0; i < k; ++i, ++searches) {
            float c[6];
            for (float& v : c) v = in.f32_();
            const ClimateTarget target = climate_target(c[0], c[1], c[2], c[3], c[4], c[5]);
            const i32 fresh = climate_search(target, kNoLeaf);
            last = climate_search(target, last);
            kept += climate_biome(fresh) != climate_biome(last);
            ASSERT_EQ(climate_biome(last), biome[in.u8_()]) << "sequence " << s << ", search " << i;
        }
    }
    EXPECT_EQ(searches, 24u * 96u);
    // The script lands on the faces of boxes on purpose: some searches are decided by the one before.
    EXPECT_GT(kept, 0u);
}

TEST(Worldgen, BiomesOfAChunkAreVanillas) {
    In in(kClimate);
    in.magic("WGCLIM01");
    const Noises noises(in.i64_());
    std::vector<u32> biome;
    for (u32 n = in.num<u32>(), i = 0; i < n; ++i) biome.push_back(world::find_biome(in.str()));
    for (u32 n = in.num<u32>(), s = 0; s < n; ++s)
        for (u32 k = in.num<u32>(), i = 0; i < k; ++i) in.p += 6 * 4 + 1;

    const u32 chunks = in.num<u32>();
    const i32 min_y = in.i32_();
    const u32 sections = in.num<u32>();
    ASSERT_EQ(sections, 24u);
    std::vector<u16> got(sections * 64);
    Sampler sampler(noises);
    for (u32 c = 0; c < chunks; ++c) {
        const i32 cx = in.i32_(), cz = in.i32_();
        sampler.reset();  // (a chunk's biomes have a context of their own)
        fill_biomes(sampler, cx, cz, min_y, sections, got.data());
        for (u32 s = 0; s < sections; ++s)
            for (u32 x = 0; x < 4; ++x)
                for (u32 y = 0; y < 4; ++y)
                    for (u32 z = 0; z < 4; ++z) {
                        const u32 want = biome[in.u8_()];
                        ASSERT_EQ(got[s * 64 + (y << 4 | z << 2 | x)], want)
                            << "chunk " << cx << ", " << cz << ", section " << s << ", cell " << x << ", " << y << ", " << z
                            << ": " << world::biome_name(got[s * 64 + (y << 4 | z << 2 | x)]) << ", vanilla's is "
                            << world::biome_name(want);
                    }
    }
    EXPECT_EQ(in.p, in.end) << "the script was not read to its end";
    EXPECT_EQ(chunks, 40u);
}

// The hash the extractor takes of a chunk's numbers: FNV-1a over each as four bytes.
u64 fnv(u64 h, u32 v) {
    for (u32 i = 0; i < 4; ++i) h = (h ^ ((v >> (8 * i)) & 0xFF)) * 0x100000001b3ull;
    return h;
}
constexpr u64 kFnv = 0xcbf29ce484222325ull;

// A stage of the script against the chunk as it stands. The script has a few chunks whole - there
// a difference is told block by block - and of the rest a hash of each part.
void expect_stage(In& in, const Terrain& t, bool whole, const std::string& what) {
    u64 blocks = kFnv;
    for (const u16 state : t.blocks) blocks = fnv(blocks, state);
    const u64 want_blocks = in.num<u64>();
    u64 heights = kFnv;
    for (const i32* map : {t.world_surface_wg, t.ocean_floor_wg})
        for (u32 i = 0; i < 256; ++i) {
            heights = fnv(heights, static_cast<u32>(map[i]));
            if (whole) {
                const i32 want = in.i32_();
                EXPECT_EQ(map[i], want) << what << ": " << (map == t.world_surface_wg ? "WORLD_SURFACE_WG" : "OCEAN_FLOOR_WG")
                                        << " at " << (i & 15) << ", " << (i >> 4);
            }
        }
    EXPECT_EQ(heights, in.num<u64>()) << what << ": the heightmaps";
    // The marks, section by section and in each in the order they were made.
    std::vector<u32> made;
    for (u32 s = 0; s < Terrain::kSections; ++s)
        for (const u16 mark : t.post_process[s]) made.push_back(s << 12 | mark);
    const u32 count = in.num<u32>();
    EXPECT_EQ(made.size(), count) << what << ": how many blocks are marked to be let flow";
    u64 marks = kFnv;
    for (const u32 mark : made) marks = fnv(marks, mark);
    if (whole)
        for (u32 i = 0; i < count; ++i) {
            const u32 want = in.num<u32>();
            if (i < made.size()) {
                EXPECT_EQ(made[i], want) << what << ": mark " << i << " (section << 12 | x | y << 4 | z << 8)";
            }
        }
    EXPECT_EQ(marks, in.num<u64>()) << what << ": what is marked to be let flow";
    if (whole) {
        const u32 packed = in.num<u32>();
        std::vector<u8> raw;
        ASSERT_TRUE(world::inflate_zlib({in.p, packed}, raw));
        in.p += packed;
        ASSERT_EQ(raw.size(), sizeof t.blocks);
        u32 differ = 0;
        for (u32 i = 0; i < std::size(t.blocks); ++i) {
            const u16 want = load_le<u16>(raw.data() + 2 * i);
            if (t.blocks[i] == want) continue;
            if (++differ <= 8)
                ADD_FAILURE() << what << ": block " << (i & 15) << ", " << static_cast<i32>(i >> 8) + settings::kMinY << ", "
                              << (i >> 4 & 15) << " is " << world::block_name(world::block_of(t.blocks[i])) << " (state "
                              << t.blocks[i] << "), vanilla's is " << world::block_name(world::block_of(want)) << " (state "
                              << want << ")";
        }
        EXPECT_EQ(differ, 0u) << what << ": blocks that differ";
    }
    EXPECT_EQ(blocks, want_blocks) << what << ": the blocks";
}

// Whole chunks, step by step as vanilla's `terrain` status makes them: after the density and the
// aquifers have said what every block is (doFill), after the material rule has made the surface
// of it (buildSurface), and after the carvers (generateCarvers) - of those also what they marked.
// The script has a chunk for every biome of the overworld.
TEST(Worldgen, TerrainOfAChunkIsVanillas) {
    In in(kTerrain);
    in.magic("WGTERR03");
    const Overworld world(in.i64_());
    const u32 chunks = in.num<u32>(), stages = in.num<u32>();
    ASSERT_EQ(stages, 3u);
    Sampler sampler(world.noises()), climate(world.noises()), uncached(world.noises(), false);
    const auto terrain = std::make_unique<Terrain>();
    const auto grid = std::make_unique<BiomeGrid>();
    const auto mask = std::make_unique<CarvingMask>();
    std::vector<u16> own(Terrain::kSections * 64);
    u64 seen[2] = {0, 0};
    u32 whole = 0, carved = 0;
    for (u32 c = 0; c < chunks; ++c) {
        const i32 cx = in.i32_(), cz = in.i32_();
        const bool all = in.u8_() != 0;
        whole += all;
        const std::string where = "chunk " + std::to_string(cx) + ", " + std::to_string(cz);
        terrain->begin(cx, cz);
        climate.reset();
        fill_biome_grid(climate, cx, cz, settings::kMinY, Terrain::kSections, *grid, own.data());
        seen[0] |= grid->present[0];
        seen[1] |= grid->present[1];
        NoiseChunk chunk(world, sampler, cx, cz);
        fill_from_noise(chunk, *terrain);
        expect_stage(in, *terrain, all, where + " after doFill");
        build_surface(world, chunk, *grid, *terrain);
        expect_stage(in, *terrain, all, where + " after buildSurface");

        mark_carved(world.seed(), cx, cz, *mask);
        u32 bits = 0, last_word = 0;
        for (u32 i = 0; i < std::size(mask->words); ++i) {
            bits += static_cast<u32>(std::popcount(mask->words[i]));
            if (mask->words[i]) last_word = i + 1;
        }
        u64 hash = kFnv;
        for (u32 i = 0; i < last_word; ++i)
            hash = fnv(fnv(hash, static_cast<u32>(mask->words[i])), static_cast<u32>(mask->words[i] >> 32));
        EXPECT_EQ(bits, in.num<u32>()) << where << ": how many blocks the carvers mark";
        EXPECT_EQ(hash, in.num<u64>()) << where << ": which blocks the carvers mark";
        carved += bits != 0;
        // Which of two biomes that are exactly as near a block has is the one vanilla's thread found
        // last, and the script's thread had asked this before it came to the blocks: the biome of
        // each of the 289 chunks around (generateCarvers, where there is no region of chunks to
        // remember them in).
        CarverBiomes carver_biomes{uncached};
        for (i32 dx = -8; dx <= 8; ++dx)
            for (i32 dz = -8; dz <= 8; ++dz) carver_biomes.at({(cx + dx) * 4, 0, (cz + dz) * 4});
        if (!mask->empty()) apply_carving_mask(world, chunk, carver_biomes, *mask, *terrain);
        expect_stage(in, *terrain, all, where + " after generateCarvers");
        if (::testing::Test::HasFailure()) return;  // (one chunk's differences say enough)
    }
    EXPECT_EQ(in.p, in.end) << "the script was not read to its end";
    EXPECT_GE(chunks, 60u);
    EXPECT_EQ(whole, 4u);
    EXPECT_GE(carved, chunks / 2) << "the carvers came to few of the chunks";
    // Every biome the overworld's climate can give stood in one of them.
    u64 possible[2] = {0, 0};
    for (const proto::gen::worldgen::ClimateNode& n : proto::gen::worldgen::kClimate)
        if (n.children == 0) possible[n.first >> 6] |= 1ull << (n.first & 63);
    EXPECT_EQ(seen[0] & possible[0], possible[0]);
    EXPECT_EQ(seen[1] & possible[1], possible[1]);
}

// Where a world begins: the block NoiseSpawnFinder picks, for a handful of seeds.
TEST(Worldgen, AWorldBeginsWhereVanillasDoes) {
    In in(kSpawn);
    in.magic("WGSPWN01");
    const u32 seeds = in.num<u32>();
    ASSERT_GE(seeds, 6u);
    u32 away = 0;
    for (u32 i = 0; i < seeds; ++i) {
        const i64 seed = in.i64_();
        const i32 want_x = in.i32_(), want_z = in.i32_();
        const Noises noises(seed);
        Sampler sampler(noises);
        i32 x = 0, z = 0;
        spawn_origin(sampler, x, z);
        EXPECT_EQ(x, want_x) << "seed " << seed;
        EXPECT_EQ(z, want_z) << "seed " << seed;
        away += want_x != 0 || want_z != 0;
    }
    EXPECT_GT(away, 0u) << "every world of the script begins at 0, 0: the search was not tried";
    EXPECT_EQ(in.p, in.end);
}

// The biomes a chunk's surface is made by are its own and a ring of its neighbours' - each
// neighbour's as the neighbour itself is filled, which the grid finds without filling it.
TEST(Worldgen, BiomeGridIsWhatTheNeighboursAreFilledWith) {
    In in(kTerrain);
    in.magic("WGTERR03");
    const Overworld world(in.i64_());
    Sampler sampler(world.noises());
    const auto grid = std::make_unique<BiomeGrid>();
    const u32 sections = Terrain::kSections;
    std::vector<u16> own(sections * 64), theirs(sections * 64);
    LegacyRandom script(20261124);
    for (u32 c = 0; c < 24; ++c) {
        const i32 cx = script.next_int(4000) - 2000, cz = script.next_int(4000) - 2000;
        sampler.reset();
        fill_biome_grid(sampler, cx, cz, settings::kMinY, sections, *grid, own.data());
        for (i32 nx = -1; nx <= 1; ++nx)
            for (i32 nz = -1; nz <= 1; ++nz) {
                sampler.reset();
                fill_biomes(sampler, cx + nx, cz + nz, settings::kMinY, sections, theirs.data());
                if (nx == 0 && nz == 0) {
                    EXPECT_EQ(own, theirs) << "chunk " << cx << ", " << cz;
                }
                u32 differ = 0;
                for (i32 qx = -1; qx <= 4; ++qx)
                    for (i32 qz = -1; qz <= 4; ++qz) {
                        if ((qx < 0 ? -1 : qx > 3 ? 1 : 0) != nx || (qz < 0 ? -1 : qz > 3 ? 1 : 0) != nz) continue;
                        for (i32 qy = 0; qy < static_cast<i32>(sections) * 4; ++qy) {
                            const u16 want = theirs[static_cast<u32>(qy >> 2) * 64 + (static_cast<u32>(qy & 3) << 4 |
                                                                                      static_cast<u32>(qz & 3) << 2 |
                                                                                      static_cast<u32>(qx & 3))];
                            differ += grid->at(cx * 4 + qx, (settings::kMinY >> 2) + qy, cz * 4 + qz) != want;
                        }
                    }
                EXPECT_EQ(differ, 0u) << "chunk " << cx << ", " << cz << ": cells of the neighbour " << nx << ", " << nz;
            }
    }
}

// The generator a world is given: a worker's chunk - sections, a biome a cell - is the terrain and
// the biomes above, as the world keeps them.
TEST(Worldgen, GeneratorHandsAWorkerItsChunk) {
    In in(kTerrain);
    in.magic("WGTERR03");
    const i64 seed = in.i64_();
    in.num<u32>();
    in.num<u32>();
    const i32 cx = in.i32_(), cz = in.i32_();
    // The chunk as the steps above make it - a thread that begins a chunk has searched for no biome.
    u64 want_blocks = kFnv;
    {
        const Overworld world(seed);
        Sampler sampler(world.noises()), climate(world.noises()), uncached(world.noises(), false);
        const auto terrain = std::make_unique<Terrain>();
        const auto grid = std::make_unique<BiomeGrid>();
        const auto mask = std::make_unique<CarvingMask>();
        std::vector<u16> own(Terrain::kSections * 64);
        terrain->begin(cx, cz);
        fill_biome_grid(climate, cx, cz, settings::kMinY, Terrain::kSections, *grid, own.data());
        NoiseChunk chunk(world, sampler, cx, cz);
        fill_from_noise(chunk, *terrain);
        build_surface(world, chunk, *grid, *terrain);
        CarverBiomes carver_biomes{uncached};
        carve(world, chunk, carver_biomes, *mask, *terrain);
        for (const u16 state : terrain->blocks) want_blocks = fnv(want_blocks, state);
    }

    OverworldGenerator generator(seed);
    EXPECT_FALSE(generator.is_flat());
    EXPECT_EQ(generator.sea_level(), 63);
    const world::Layout layout{settings::kMinY, settings::kMinY >> 4, Terrain::kSections, true};
    world::LoadedChunk made;
    std::string err;
    ASSERT_TRUE(world::load_chunk({}, cx, cz, layout, generator, made, err)) << err;
    EXPECT_TRUE(made.fresh);
    // Its blocks are vanilla's (the hash the script has of them).
    u64 blocks = kFnv;
    for (u32 si = 0; si < Terrain::kSections; ++si)
        for (u32 i = 0; i < 4096; ++i) blocks = fnv(blocks, made.chunk.sections[si].get(i));
    EXPECT_EQ(blocks, want_blocks);
    // ...and its biomes those of the cells, a section's palette in the order vanilla comes to them.
    Sampler sampler(generator.world().noises());
    std::vector<u16> biome(Terrain::kSections * 64);
    fill_biomes(sampler, cx, cz, settings::kMinY, Terrain::kSections, biome.data());
    for (u32 si = 0; si < Terrain::kSections; ++si) {
        const world::BiomeSection* b = made.chunk.biomes && made.chunk.biomes[si].count ? &made.chunk.biomes[si] : nullptr;
        u16 first[64];
        u32 kinds = 0;
        for (u32 x = 0; x < 4; ++x)
            for (u32 y = 0; y < 4; ++y)
                for (u32 z = 0; z < 4; ++z) {
                    const u16 want = biome[si * 64 + (y << 4 | z << 2 | x)];
                    ASSERT_EQ(b ? b->palette[b->index[y << 4 | z << 2 | x]] : made.chunk.biome[si], want) << "section " << si;
                    if (std::find(first, first + kinds, want) == first + kinds) first[kinds++] = want;
                }
        if (kinds == 1) {
            EXPECT_EQ(b, nullptr) << "section " << si << " has one biome and a palette";
        } else {
            ASSERT_NE(b, nullptr) << "section " << si;
            ASSERT_EQ(b->count, kinds);
            for (u32 k = 0; k < kinds; ++k) EXPECT_EQ(b->palette[k], first[k]) << "section " << si << ", palette entry " << k;
        }
    }
}
