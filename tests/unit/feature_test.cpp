// SPDX-License-Identifier: GPL-3.0-or-later
// A chunk's features against what vanilla computes: tests/unit/data/worldgen_features.bin, written
// by tools/mcgen/extract/WorldgenFeatures.java (`mcgen.py worldgen golden --emit`) from the 26.3
// jar's own classes. The layout is in the Java file, beside the code that writes it.
#include <gtest/gtest.h>

#include <bit>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "world/gen_chunk.hpp"
#include "worldgen/feature.hpp"
#include "worldgen/generator.hpp"

using namespace worldgen;

namespace {

constexpr unsigned char kFeatures[] = {
#embed "data/worldgen_features.bin"
};

struct In {
    const unsigned char* p = kFeatures;
    const unsigned char* end = kFeatures + sizeof kFeatures;

    template <class T>
    T num() {
        EXPECT_LE(p + sizeof(T), end);
        const T v = load_le<T>(p);
        p += sizeof(T);
        return v;
    }
    void skip(usize bytes) { p += bytes; }
};

constexpr u64 kFnv = 0xcbf29ce484222325ull;
u64 fnv(u64 h, u32 v) {
    for (u32 i = 0; i < 4; ++i) h = (h ^ ((v >> (8 * i)) & 0xFF)) * 0x100000001b3ull;
    return h;
}
u64 fnv_bytes(const void* data, usize n) {
    u64 h = kFnv;
    for (usize i = 0; i < n; ++i) h = (h ^ static_cast<const unsigned char*>(data)[i]) * 0x100000001b3ull;
    return h;
}

// The parts of the script, one after the other; each test reads up to its own.
struct Script {
    In in;
    i64 seed = 0;

    Script() {
        EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(in.p), 8), "WGFEAT01");
        in.skip(8);
        seed = in.num<i64>();
    }
    void skip_ints() {
        for (u32 n = in.num<u32>(); n; --n) in.skip(1 + 12 + 8 + 8 + 12 * 4 + 8);
    }
    void skip_heights() {
        for (u32 n = in.num<u32>(); n; --n) in.skip(1 + 12 + 8 + 12 * 4 + 8);
    }
    void skip_sets() { in.skip(usize{in.num<u32>()} * 16); }
};

}  // namespace

TEST(WorldgenFeature, NumbersAreDrawnAsVanillaDrawsThem) {
    Script s;
    In& in = s.in;
    const u32 ints = in.num<u32>();
    ASSERT_GT(ints, 0u);
    for (u32 n = 0; n < ints; ++n) {
        ft::Int provider{};
        provider.op = static_cast<ft::IntOp>(in.num<u8>());
        provider.a = in.num<i32>();
        provider.b = in.num<i32>();
        provider.c = in.num<i32>();
        provider.fa = std::bit_cast<float>(in.num<u32>());
        provider.fb = std::bit_cast<float>(in.num<u32>());
        FeatureRandom random{XoroshiroRandom(in.num<i64>())};
        for (u32 i = 0; i < 12; ++i)
            ASSERT_EQ(sample(provider, random), in.num<i32>())
                << "an int of kind " << static_cast<u32>(provider.op) << " (" << provider.a << ", " << provider.b << ", "
                << provider.c << "), draw " << i;
        ASSERT_EQ(random.next_long(), in.num<i64>()) << "the source after an int of kind " << static_cast<u32>(provider.op);
    }
    const u32 heights = in.num<u32>();
    ASSERT_GT(heights, 0u);
    for (u32 n = 0; n < heights; ++n) {
        ft::Height provider{};
        provider.op = static_cast<ft::HeightOp>(in.num<u8>());
        provider.min = in.num<i32>();
        provider.max = in.num<i32>();
        provider.inner = in.num<i32>();
        FeatureRandom random{XoroshiroRandom(in.num<i64>())};
        for (u32 i = 0; i < 12; ++i)
            ASSERT_EQ(sample(provider, random), in.num<i32>())
                << "a height of kind " << static_cast<u32>(provider.op) << " (" << provider.min << ", " << provider.max << ", "
                << provider.inner << "), draw " << i;
        ASSERT_EQ(random.next_long(), in.num<i64>()) << "the source after a height of kind " << static_cast<u32>(provider.op);
    }
}

// Every set of block states the tables hold is the set vanilla's own predicate holds for: all
// 35723 states of each, by the predicate's name.
TEST(WorldgenFeature, StateSetsAreVanillasPredicates) {
    Script s;
    s.skip_ints();
    s.skip_heights();
    In& in = s.in;
    std::unordered_map<u64, u64> vanilla;
    for (u32 n = in.num<u32>(); n; --n) {
        const u64 name = in.num<u64>();
        vanilla[name] = in.num<u64>();
    }
    ASSERT_GT(ft::kStateSetCount, 0u);
    for (u32 set = 0; set < ft::kStateSetCount; ++set) {
        const std::string_view source = ft::kStateSetSources[set];
        const auto found = vanilla.find(fnv_bytes(source.data(), source.size()));
        ASSERT_NE(found, vanilla.end()) << "vanilla has no predicate written " << source;
        EXPECT_EQ(fnv_bytes(ft::kStateSets + usize{set} * ft::kStateSetBytes, ft::kStateSetBytes), found->second)
            << "the block states of " << source;
    }
}

// The placed features of the biomes, on real terrain: where each comes to be placed - through
// counts, squares, heights and heightmaps, the biome of a block, what is scanned for - and what
// its source gives next.
TEST(WorldgenFeature, PlacementsComeToWhereVanillasDo) {
    Script s;
    s.skip_ints();
    s.skip_heights();
    s.skip_sets();
    In& in = s.in;
    OverworldGenerator generator(s.seed);
    const auto work = std::make_unique<RegionWork>();
    FeatureWork feature_work;
    const u32 sites = in.num<u32>();
    ASSERT_GT(sites, 0u);
    u32 compared = 0, waiting = 0;
    std::vector<bool> seen(ft::kNamedPlaced);
    for (u32 site = 0; site < sites; ++site) {
        const i32 cx = in.num<i32>(), cz = in.num<i32>();
        const bool whole = in.num<u8>() != 0;
        std::vector<std::unique_ptr<world::GenChunk>> owned;
        world::GenChunk* nine[9];
        for (i32 dz = -1; dz <= 1; ++dz)
            for (i32 dx = -1; dx <= 1; ++dx) {
                owned.push_back(std::make_unique<world::GenChunk>());
                generator.make_base(cx + dx, cz + dz, *owned.back());
                nine[(dx + 1) + (dz + 1) * 3] = owned.back().get();
            }
        Region region(*work, generator.world(), nine, cx - 1, cz - 1, 3, 3);
        region.begin(cx, cz);
        Decorator decorator(region, feature_work);
        std::vector<Pos> at;
        decorator.set_record(&at);
        const Pos origin{cx * 16, Region::kMinY, cz * 16};
        FeatureRandom random{XoroshiroRandom{u64{0}, u64{0}}};
        const i64 decoration_seed = random.set_decoration_seed(s.seed, origin.x, origin.z);

        // What the decoration itself runs, and in which order: vanilla's list for this chunk.
        std::vector<Decorator::Top> ran, wanted;
        decorator.set_log(&ran);
        decorator.decorate();
        decorator.set_log(nullptr);
        const usize all = at.size();
        usize placed_in_all = 0;

        const u32 count = in.num<u32>();
        for (u32 n = 0; n < count; ++n) {
            const u32 step = in.num<u8>(), index = static_cast<u32>(in.num<i32>());
            ASSERT_LT(step, ft::kSteps);
            ASSERT_LT(ft::kStepFirst[step] + index, ft::kStepFirst[step + 1]);
            const u32 placed = ft::kStepPlaced[ft::kStepFirst[step] + index];
            const std::string_view name = ft::kPlacedNames[placed];
            const i32 positions = in.num<i32>();
            ASSERT_GE(positions, 0) << name;
            std::vector<Pos> theirs;
            if (whole)
                for (i32 i = 0; i < positions; ++i) {
                    const i32 x = in.num<i32>(), y = in.num<i32>(), z = in.num<i32>();
                    theirs.push_back({x, y, z});
                }
            const u64 hash = in.num<u64>();
            const i64 next = in.num<i64>();
            seen[placed] = true;
            if (!ft::kPlaced[placed].where) {  // (a placement that asks a block's own code: still to come)
                ++waiting;
                continue;
            }
            wanted.push_back({step, index});
            at.clear();
            random.set_feature_seed(decoration_seed, static_cast<i32>(index), static_cast<i32>(step));
            const Decorator::Top top{step, index};
            decorator.place(placed, random, origin, &top);
            u64 ours = kFnv;
            for (const Pos& p : at)
                ours = fnv(fnv(fnv(ours, static_cast<u32>(p.x)), static_cast<u32>(p.y)), static_cast<u32>(p.z));
            if (whole && (at.size() != theirs.size() || ours != hash)) {
                for (usize i = 0; i < std::min(at.size(), theirs.size()); ++i)
                    if (at[i].x != theirs[i].x || at[i].y != theirs[i].y || at[i].z != theirs[i].z) {
                        ADD_FAILURE() << name << " around chunk " << cx << ", " << cz << ": position " << i << " is " << at[i].x
                                      << ", " << at[i].y << ", " << at[i].z << ", vanilla's " << theirs[i].x << ", "
                                      << theirs[i].y << ", " << theirs[i].z;
                        break;
                    }
            }
            ASSERT_EQ(at.size(), static_cast<usize>(positions)) << name << " around chunk " << cx << ", " << cz;
            ASSERT_EQ(ours, hash) << name << " around chunk " << cx << ", " << cz << ": " << positions << " positions";
            ASSERT_EQ(random.next_long(), next) << name << " around chunk " << cx << ", " << cz << ": the source afterwards";
            placed_in_all += at.size();
            ++compared;
        }
        ASSERT_EQ(ran.size(), wanted.size()) << "the features a decoration runs around chunk " << cx << ", " << cz;
        for (usize i = 0; i < ran.size(); ++i) {
            ASSERT_EQ(ran[i].step, wanted[i].step) << "feature " << i << " around chunk " << cx << ", " << cz;
            ASSERT_EQ(ran[i].index, wanted[i].index) << "feature " << i << " around chunk " << cx << ", " << cz;
        }
        EXPECT_EQ(all, placed_in_all) << "all of a decoration's positions around chunk " << cx << ", " << cz;
        region.commit();
    }
    EXPECT_EQ(in.p, in.end);
    // Between them the chunks have every feature a biome of the overworld names.
    for (u32 placed = 0; placed < ft::kNamedPlaced; ++placed) EXPECT_TRUE(seen[placed]) << ft::kPlacedNames[placed];
    EXPECT_GT(compared, 1000u);
    RecordProperty("placements_waiting", static_cast<int>(waiting));
}
