// SPDX-License-Identifier: GPL-3.0-or-later
// A chunk's features against what vanilla computes: tests/unit/data/worldgen_features.bin, written
// by tools/mcgen/extract/WorldgenFeatures.java (`mcgen.py worldgen golden --emit`) from the 26.3
// jar's own classes. The layout is in the Java file, beside the code that writes it.
#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "world/gen_chunk.hpp"
#include "worldgen/feature.hpp"
#include "worldgen/generator.hpp"
#include "worldgen/jmath.hpp"
#include "worldgen/survive.hpp"

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
    std::string str() {
        const u32 n = num<u32>();
        std::string text(reinterpret_cast<const char*>(p), n);
        p += n;
        return text;
    }
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
    void skip_decorations() {
        for (u32 kinds = in.num<u32>(); kinds; --kinds) {
            in.str();
            for (u32 areas = in.num<u32>(); areas; --areas) {
                in.skip(8);
                for (u32 chunk = 0; chunk < 4; ++chunk) in.skip(usize{in.num<u32>()} * 21);
                in.skip(16 * 40);
            }
        }
    }
    void skip_attempts() {
        for (u32 kinds = in.num<u32>(); kinds; --kinds) {
            in.str();
            for (u32 sites = in.num<u32>(); sites; --sites) {
                in.skip(8);
                for (u32 placed = in.num<u32>(); placed; --placed) {
                    in.skip(9);
                    in.skip(usize{in.num<u32>()} * 20);
                }
                in.skip(9 * 40);
            }
        }
    }
    void skip_survive() {
        in.skip(usize{in.num<u32>()} * 4);
        for (u32 entries = in.num<u32>(); entries; --entries) {
            in.skip(8);
            in.skip((usize{in.num<u32>()} + 7) / 8);
        }
    }
    void skip_placements() {
        for (u32 sites = in.num<u32>(); sites; --sites) {
            in.skip(8);
            const bool whole = in.num<u8>() != 0;
            for (u32 n = in.num<u32>(); n; --n) {
                in.skip(5);
                const i32 positions = in.num<i32>();
                if (positions < 0) continue;
                in.skip((whole ? usize{12} * static_cast<u32>(positions) : 0) + 16);
            }
        }
    }
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
    // Between them the chunks have every feature a biome of the overworld names.
    for (u32 placed = 0; placed < ft::kNamedPlaced; ++placed) EXPECT_TRUE(seen[placed]) << ft::kPlacedNames[placed];
    EXPECT_GT(compared, 1000u);
    RecordProperty("placements_waiting", static_cast<int>(waiting));
}

// A chunk as the script has it (WorldgenFeatures.chunkState): its blocks, the four heightmaps it
// keeps, its marks section by section - each as one number - and how many ticks and block entities.
void expect_chunk(In& in, const world::GenChunk& c, const std::string& name) {
    namespace hm = world::heightmap;
    u64 blocks = kFnv;
    for (u32 si = 0; si < c.sections_count; ++si)
        for (u32 b = 0; b < 4096; ++b) blocks = fnv(blocks, c.sections[si].get(b));
    EXPECT_EQ(blocks, in.num<u64>()) << name << ": the blocks of chunk " << c.x << ", " << c.z;
    u64 heights = kFnv;
    for (const u32 map : {hm::kWorldSurface, hm::kOceanFloor, hm::kMotionBlocking, hm::kMotionBlockingNoLeaves})
        for (u32 column = 0; column < 256; ++column)
            heights = fnv(heights, static_cast<u32>(c.height[map][column] + Region::kMinY));
    EXPECT_EQ(heights, in.num<u64>()) << name << ": the heightmaps of chunk " << c.x << ", " << c.z;
    std::vector<u32> marks = c.marks;
    std::stable_sort(marks.begin(), marks.end(), [](u32 a, u32 b) { return a >> 12 < b >> 12; });
    u64 marked = kFnv;
    for (const u32 mark : marks) marked = fnv(marked, mark);
    EXPECT_EQ(marks.size(), in.num<u32>()) << name << ": the marks of chunk " << c.x << ", " << c.z;
    EXPECT_EQ(marked, in.num<u64>()) << name << ": the marks of chunk " << c.x << ", " << c.z;
    EXPECT_EQ(c.block_ticks.size(), in.num<u32>()) << name << ": the block ticks of chunk " << c.x << ", " << c.z;
    EXPECT_EQ(c.fluid_ticks.size(), in.num<u32>()) << name << ": the fluid ticks of chunk " << c.x << ", " << c.z;
    EXPECT_EQ(c.block_entities.size(), in.num<u32>()) << name << ": the block entities of chunk " << c.x << ", " << c.z;
}

// Whole decorations: areas of 4 x 4 chunks of terrain, the inner four decorated one after the
// other - with the features of one kind alone, kind by kind, and with every feature that is
// ported, which is the server's decoration. Every feature that runs writes what vanilla's writes
// and leaves its source where vanilla's is; and the sixteen chunks end as vanilla's do.
TEST(WorldgenFeature, DecorationsAreVanillas) {
    Script s;
    s.skip_ints();
    s.skip_heights();
    s.skip_sets();
    s.skip_placements();
    In& in = s.in;
    OverworldGenerator generator(s.seed);
    const auto work = std::make_unique<RegionWork>();
    FeatureWork feature_work;
    namespace hm = world::heightmap;
    const u32 kinds = in.num<u32>();
    ASSERT_GT(kinds, 0u);
    for (u32 kind = 0; kind < kinds; ++kind) {
        const std::string name = in.str();
        // "ported": every feature that is ported, which is what a decoration is to the server.
        const bool everything = name == "ported";
        ft::FeatureType type = ft::FeatureType::kUnported;
        if (name == "ore") type = ft::FeatureType::kOre;
        ASSERT_TRUE(everything || type != ft::FeatureType::kUnported)
            << "the script has decorations of " << name << ", which is no kind this test knows";
        std::vector<u8> only(ft::kStepFirst[ft::kSteps]);
        for (u32 slot = 0; slot < only.size(); ++slot) {
            const ft::Placed& placed = ft::kPlaced[ft::kStepPlaced[slot]];
            only[slot] = everything ? placed.ported : ft::kFeatures[placed.feature].type == type;
        }
        for (u32 areas = in.num<u32>(); areas; --areas) {
            const i32 x0 = in.num<i32>(), z0 = in.num<i32>();
            std::vector<std::unique_ptr<world::GenChunk>> owned;
            world::GenChunk* sixteen[16];
            for (i32 i = 0; i < 16; ++i) {
                owned.push_back(std::make_unique<world::GenChunk>());
                generator.make_base(x0 + (i & 3), z0 + (i >> 2), *owned.back());
                sixteen[i] = owned.back().get();
            }
            Region region(*work, generator.world(), sixteen, x0, z0, 4, 4);
            for (i32 i = 0; i < 4; ++i) {
                const i32 cx = x0 + 1 + (i & 1), cz = z0 + 1 + (i >> 1);
                region.begin(cx, cz);
                Decorator decorator(region, feature_work);
                decorator.set_only(only);
                std::vector<Decorator::Trace> trace;
                decorator.set_trace(&trace);
                decorator.decorate();
                const u32 ran = in.num<u32>();
                ASSERT_EQ(trace.size(), ran) << name << ": the features that run in chunk " << cx << ", " << cz;
                for (const Decorator::Trace& t : trace) {
                    const u32 step = in.num<u8>(), index = static_cast<u32>(in.num<i32>());
                    const u64 writes = in.num<u64>();
                    const i64 next = in.num<i64>();
                    ASSERT_EQ(t.step, step);
                    ASSERT_EQ(t.index, index);
                    const std::string_view placed = ft::kPlacedNames[ft::kStepPlaced[ft::kStepFirst[step] + index]];
                    ASSERT_EQ(t.writes, writes) << placed << " in chunk " << cx << ", " << cz << ": what it wrote";
                    ASSERT_EQ(t.next, next) << placed << " in chunk " << cx << ", " << cz << ": its source afterwards";
                }
                EXPECT_TRUE(sixteen[(cx - x0) + (cz - z0) * 4]->decorated);
            }
            EXPECT_EQ(region.counts().far_reads, 0u);
            EXPECT_EQ(region.counts().refused_writes, 0u);
            region.commit();
            // The same area as one step of the generator's own - a worker's task: the same chunks.
            std::vector<std::unique_ptr<world::GenChunk>> again;
            world::GenChunk* task[16];
            if (everything) {
                for (i32 i = 0; i < 16; ++i) {
                    again.push_back(std::make_unique<world::GenChunk>());
                    generator.make_base(x0 + (i & 3), z0 + (i >> 2), *again.back());
                    task[i] = again.back().get();
                }
                generator.decorate(task, x0, z0, 1);
            }
            for (i32 i = 0; i < 16; ++i) {
                const world::GenChunk& c = *sixteen[i];
                if (everything) {
                    const world::GenChunk& t = *task[i];
                    EXPECT_EQ(t.version, 1);
                    EXPECT_EQ(t.decorated, c.decorated);
                    u32 differ = 0;
                    for (u32 si = 0; si < c.sections_count; ++si)
                        for (u32 b = 0; b < 4096; ++b) differ += t.sections[si].get(b) != c.sections[si].get(b);
                    EXPECT_EQ(differ, 0u) << "chunk " << c.x << ", " << c.z << " as a task of the generator's";
                    EXPECT_EQ(t.marks, c.marks);
                    for (const u32 map : {hm::kWorldSurface, hm::kOceanFloor, hm::kMotionBlocking, hm::kMotionBlockingNoLeaves})
                        EXPECT_TRUE(std::equal(std::begin(t.height[map]), std::end(t.height[map]), std::begin(c.height[map])));
                }
                expect_chunk(in, c, name);
            }
        }
    }
}

// A kind of feature placed again and again in one place - nine chunks of terrain, every placed
// feature of the kind that the biomes there have, with source after source: what rarely happens in
// a chunk happens here, and what was built is in the way of what comes after. Every attempt writes
// what vanilla's writes - or nothing, where vanilla's writes nothing - and leaves its source where
// vanilla's is; and the nine chunks end as vanilla's do.
TEST(WorldgenFeature, FeaturesPlacedAgainAndAgainAreVanillas) {
    Script s;
    s.skip_ints();
    s.skip_heights();
    s.skip_sets();
    s.skip_placements();
    s.skip_decorations();
    In& in = s.in;
    OverworldGenerator generator(s.seed);
    const auto work = std::make_unique<RegionWork>();
    FeatureWork feature_work;
    const u32 kinds = in.num<u32>();
    ASSERT_GT(kinds, 0u);
    for (u32 kind = 0; kind < kinds; ++kind) {
        const std::string name = in.str();
        u32 wrote = 0;
        for (u32 sites = in.num<u32>(); sites; --sites) {
            const i32 cx = in.num<i32>(), cz = in.num<i32>();
            std::vector<std::unique_ptr<world::GenChunk>> owned;
            world::GenChunk* nine[9];
            for (i32 i = 0; i < 9; ++i) {
                owned.push_back(std::make_unique<world::GenChunk>());
                generator.make_base(cx - 1 + i % 3, cz - 1 + i / 3, *owned.back());
                nine[i] = owned.back().get();
            }
            Region region(*work, generator.world(), nine, cx - 1, cz - 1, 3, 3);
            region.begin(cx, cz);
            Decorator decorator(region, feature_work);
            const Pos origin{cx * 16, Region::kMinY, cz * 16};
            FeatureRandom random{XoroshiroRandom{u64{0}, u64{0}}};
            const i64 decoration_seed = random.set_decoration_seed(generator.world().seed(), origin.x, origin.z);
            for (u32 placed = in.num<u32>(); placed; --placed) {
                const u32 step = in.num<u8>(), index = static_cast<u32>(in.num<i32>());
                const i32 attempts = in.num<i32>();
                u32 writers = in.num<u32>();
                const u32 row = ft::kStepPlaced[ft::kStepFirst[step] + index];
                const std::string_view placed_name = ft::kPlacedNames[row];
                ASSERT_TRUE(ft::kPlaced[row].ported) << placed_name << " is in the script and not ported";
                const Decorator::Top top{step, index};
                i32 next_writer = writers ? in.num<i32>() : -1;
                for (i32 attempt = 0; attempt < attempts; ++attempt) {
                    random.set_feature_seed(decoration_seed + attempt, static_cast<i32>(index), static_cast<i32>(step));
                    u64 writes = kFnv;
                    region.set_log(&writes);
                    // FEATURE_TRACE=<kind>:<chunk x>:<chunk z>:<attempt>: every block that attempt sets, as the
                    // extractor says its own with WORLDGEN_TRACE.
                    std::vector<i32> trace;
                    const char* wanted = std::getenv("FEATURE_TRACE");
                    if (wanted &&
                        wanted == name + ":" + std::to_string(cx) + ":" + std::to_string(cz) + ":" + std::to_string(attempt))
                        region.set_trace(&trace);
                    decorator.place(row, random, origin, &top);
                    region.set_log(nullptr);
                    region.set_trace(nullptr);
                    for (usize i = 0; i + 3 < trace.size(); i += 4)
                        std::fprintf(stderr, "TRACE %d %d %d %d\n", trace[i], trace[i + 1], trace[i + 2], trace[i + 3]);
                    if (attempt != next_writer) {
                        ASSERT_EQ(writes, kFnv) << placed_name << " at chunk " << cx << ", " << cz << ", attempt " << attempt
                                                << ": vanilla's wrote nothing";
                        continue;
                    }
                    const u64 want = in.num<u64>();
                    const i64 next = in.num<i64>();
                    ASSERT_EQ(writes, want) << placed_name << " at chunk " << cx << ", " << cz << ", attempt " << attempt
                                            << ": what it wrote";
                    ASSERT_EQ(random.next_long(), next) << placed_name << " at chunk " << cx << ", " << cz << ", attempt "
                                                        << attempt << ": its source afterwards";
                    ++wrote;
                    next_writer = --writers ? in.num<i32>() : -1;
                }
                ASSERT_EQ(writers, 0u);
            }
            EXPECT_EQ(region.counts().far_reads, 0u);
            region.commit();
            for (const world::GenChunk* c : nine) expect_chunk(in, *c, name);
        }
        EXPECT_GT(wrote, 0u) << "nothing of the kind " << name << " was ever placed";
    }
}

// The rules a block stays by (BlockState.canSurvive: survive.hpp), each held to the method itself in
// worlds drawn at random - the blocks within one of the place and two above and below it, any
// block state or one of a few that a rule is likely to tell apart, and light or none.
TEST(WorldgenFeature, BlocksStayWhereVanillasWould) {
    Script s;
    s.skip_ints();
    s.skip_heights();
    s.skip_sets();
    s.skip_placements();
    s.skip_decorations();
    s.skip_attempts();
    In& in = s.in;
    struct Mix {  // (SplitMix64: the same numbers as WorldgenSurvive.Mix)
        u64 x;
        u64 next() {
            x += 0x9E3779B97F4A7C15ull;
            u64 z = x;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            return z ^ (z >> 31);
        }
    };
    struct Cells {
        u16 cell[45], rest;
        i32 light;
        u16 block(i32 x, i32 y, i32 z) const {
            if (x < -1 || x > 1 || y < -2 || y > 2 || z < -1 || z > 1) return rest;
            return cell[(x + 1) * 15 + (y + 2) * 3 + (z + 1)];
        }
        i32 raw_brightness(i32, i32, i32) const { return light; }
    };
    std::vector<u16> telling(in.num<u32>());
    for (u16& state : telling) state = static_cast<u16>(in.num<i32>());
    const u32 entries = in.num<u32>();
    ASSERT_GT(entries, 20u);
    u32 held = 0, waiting = 0, stayed = 0, went = 0;
    for (u32 entry = 0; entry < entries; ++entry) {
        const u32 state = static_cast<u32>(in.num<i32>()), rule = static_cast<u32>(in.num<i32>());
        const u32 trials = in.num<u32>();
        const u8* answers = in.p;
        in.skip((usize{trials} + 7) / 8);
        ASSERT_LT(rule, std::size(ft::kSurvive));
        if (ft::kSurvive[rule].op == ft::SurviveOp::kUnported) {
            ++waiting;
            continue;
        }
        ++held;
        for (u32 trial = 0; trial < trials; ++trial) {
            Mix mix{u64{state} * 1000003ull + trial};
            const auto pick = [&] {
                const u64 r = mix.next();
                return r & 1 ? telling[(r >> 1) % telling.size()] : static_cast<u16>((r >> 1) % world::state::kCount);
            };
            Cells cells;
            cells.light = mix.next() & 1 ? 15 : 0;
            cells.rest = pick();
            for (u16& cell : cells.cell) cell = pick();
            const bool want = answers[trial >> 3] >> (trial & 7) & 1;
            ASSERT_EQ(survives(cells, ft::kSurvive[rule], 0, 0, 0), want)
                << world::block_name(world::block_of(static_cast<world::BlockState>(state))) << " (state " << state << "), world "
                << trial;
            (want ? stayed : went) += 1;
        }
    }
    EXPECT_GT(held, 20u);
    EXPECT_GT(stayed, 1000u);
    EXPECT_GT(went, 1000u);
    (void)waiting;
}

// Math.sin and Math.cos of every angle an ore can have - its random float times pi - are the
// JVM's, all 2^24 of them: our own sine, and the table of where HotSpot's is the next double.
TEST(WorldgenFeature, SineAndCosineOfEveryAngleOfAFloatAreTheJvms) {
    Script s;
    s.skip_ints();
    s.skip_heights();
    s.skip_sets();
    s.skip_placements();
    s.skip_decorations();
    s.skip_attempts();
    s.skip_survive();
    In& in = s.in;
    const u64 want_sines = in.num<u64>(), want_cosines = in.num<u64>();
    u64 sines = kFnv, cosines = kFnv;
    u32 corrected = 0;
    for (u32 k = 0; k < 1u << 24; ++k) {
        const float dir = (static_cast<float>(k) * 5.9604645E-8f) * 3.1415927f;
        const double x = static_cast<double>(dir);
        const u64 sine = std::bit_cast<u64>(jmath::sin(x)), cosine = std::bit_cast<u64>(jmath::cos(x));
        sines = fnv(fnv(sines, static_cast<u32>(sine)), static_cast<u32>(sine >> 32));
        cosines = fnv(fnv(cosines, static_cast<u32>(cosine)), static_cast<u32>(cosine >> 32));
        corrected += sine != std::bit_cast<u64>(jmath::sin_own(x));
    }
    for (u32 n = in.num<u32>(); n; --n) {
        const u32 k = static_cast<u32>(in.num<i32>());
        const double x = std::bit_cast<double>(in.num<u64>());
        const float dir = (static_cast<float>(k) * 5.9604645E-8f) * 3.1415927f;
        EXPECT_EQ(std::bit_cast<u64>(static_cast<double>(dir)), std::bit_cast<u64>(x)) << "the angle of float " << k;
        EXPECT_EQ(std::bit_cast<u64>(jmath::sin(x)), in.num<u64>()) << "sin " << x;
        EXPECT_EQ(std::bit_cast<u64>(jmath::cos(x)), in.num<u64>()) << "cos " << x;
    }
    EXPECT_EQ(sines, want_sines);
    EXPECT_EQ(cosines, want_cosines);
    // (Our own sine is the JVM's for all but a few thousand of them.)
    EXPECT_GT(corrected, 0u);
    EXPECT_LT(corrected, 1u << 16);
    // ...and of the angles that are the same in every world, each written out: the rings of the
    // search for where a world begins (NoiseSpawnFinder.radialSearch).
    const u32 lists = in.num<u32>();
    ASSERT_EQ(lists, 1u);
    for (u32 list = 0; list < lists; ++list) {
        const std::string name = in.str();
        const u32 count = in.num<u32>();
        ASSERT_GT(count, 100u) << name;
        for (u32 i = 0; i < count; ++i) {
            const double x = std::bit_cast<double>(in.num<u64>());
            const u64 sine = in.num<u64>(), cosine = in.num<u64>();
            EXPECT_EQ(std::bit_cast<u64>(jmath::sin(x)), sine) << name << ": sin " << x;
            EXPECT_EQ(std::bit_cast<u64>(jmath::cos(x)), cosine) << name << ": cos " << x;
        }
    }
    EXPECT_EQ(in.p, in.end);
}
