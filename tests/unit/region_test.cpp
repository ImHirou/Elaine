// SPDX-License-Identifier: GPL-3.0-or-later
// worldgen::Region - what a feature reads and writes through - held to what ProtoChunk and
// WorldGenRegion do (the 26.3 jar), on chunks that are written out here by hand.
#include <gtest/gtest.h>

#include <memory>
#include <random>
#include <vector>

#include "worldgen/region.hpp"

using namespace worldgen;
namespace wg = proto::gen::worldgen;
namespace hm = world::heightmap;
namespace block = proto::gen::block;

namespace {

constexpr i32 kMinY = settings::kMinY;
constexpr u32 kSections = Terrain::kSections;

// A chunk of stone up to `ground`, water from there to y 62, and a pocket of cave air low down.
std::unique_ptr<world::GenChunk> chunk_at(i32 cx, i32 cz, i32 ground) {
    auto c = std::make_unique<world::GenChunk>();
    c->x = cx;
    c->z = cz;
    c->sections_count = static_cast<u8>(kSections);
    std::vector<u16> states(4096);
    for (u32 si = 0; si < kSections; ++si) {
        for (u32 i = 0; i < 4096; ++i) {
            const i32 y = kMinY + static_cast<i32>(si * 16 + (i >> 8));
            states[i] = static_cast<u16>(y < ground ? block::kStone : y <= 62 ? block::kWater : world::kAir);
            if (y >= 200 && y < 216 && (i & 255) == 0) states[i] = block::kCaveAir;  // a section of nothing but air of two kinds
        }
        c->sections[si].assign(c->alloc, states.data());
    }
    std::fill(std::begin(c->cells), std::end(c->cells), u16{3});
    c->biomes_present[0] = 1u << 3;
    prime_heightmaps(*c, (1u << hm::kCount) - 1);
    return c;
}

struct Nine {
    std::vector<std::unique_ptr<world::GenChunk>> owned;
    world::GenChunk* chunks[16];
    u32 width;

    explicit Nine(u32 w = 3) : width(w) {
        for (u32 i = 0; i < w * w; ++i) {
            owned.push_back(chunk_at(10 + static_cast<i32>(i % w), -7 + static_cast<i32>(i / w), 40 + static_cast<i32>(i)));
            chunks[i] = owned.back().get();
        }
    }
};

i32 counted(Region& r, u32 type, i32 x, i32 z) {
    i32 y = kMinY + settings::kHeight - 1;
    while (y >= kMinY && !(wg::kHeightmaps[r.block(x, y, z)] >> type & 1)) --y;
    return y + 1;
}

}  // namespace

TEST(WorldgenRegion, ReadsWhatAProtoChunkAnswers) {
    const Overworld world(1);
    RegionWork work;
    Nine nine;
    Region r(work, world, nine.chunks, 10, -7, 3, 3);
    r.begin(11, -6);
    EXPECT_EQ(r.block(11 * 16 + 3, 39, -6 * 16 + 5), block::kStone);
    EXPECT_EQ(r.block(11 * 16 + 3, 60, -6 * 16 + 5), block::kWater);
    EXPECT_EQ(r.block(11 * 16 + 3, 100, -6 * 16 + 5), world::kAir);
    // Beyond the world's height: void air. In a section of air and cave air: air, whichever it is.
    EXPECT_EQ(r.block(11 * 16, kMinY - 1, -6 * 16), block::kVoidAir);
    EXPECT_EQ(r.block(11 * 16, kMinY + settings::kHeight, -6 * 16), block::kVoidAir);
    EXPECT_EQ(r.block(11 * 16, 200, -6 * 16), world::kAir);
    // The heightmaps are those of the blocks.
    EXPECT_EQ(r.height(hm::kOceanFloor, 11 * 16 + 3, -6 * 16 + 5), 44);      // (the fifth chunk: stone below 44)
    EXPECT_EQ(r.height(hm::kMotionBlocking, 11 * 16 + 3, -6 * 16 + 5), 63);  // water counts
    EXPECT_EQ(r.height(hm::kWorldSurface, 11 * 16 + 3, -6 * 16 + 5), 63);
    // A chunk that is none of the nine has nothing in it, and is not written to.
    EXPECT_EQ(r.counts().far_reads, 0u);
    EXPECT_EQ(r.block(13 * 16, 10, -6 * 16), world::kAir);
    EXPECT_EQ(r.height(hm::kWorldSurface, 9 * 16 + 15, -6 * 16), kMinY);
    EXPECT_EQ(r.counts().far_reads, 2u);
    EXPECT_FALSE(r.set_block(13 * 16, 10, -6 * 16, block::kStone));
    EXPECT_EQ(r.counts().refused_writes, 1u);
    u64 biomes[2];
    r.possible_biomes(biomes);
    EXPECT_EQ(biomes[0], 1u << 3);
    EXPECT_EQ(r.biome(11 * 16 + 8, 64, -6 * 16 + 8), 3);
    r.commit();
}

TEST(WorldgenRegion, WritesMoveTheHeightmapsAChunkKeeps) {
    const Overworld world(1);
    RegionWork work;
    Nine nine;
    Region r(work, world, nine.chunks, 10, -7, 3, 3);
    r.begin(11, -6);
    const u16 kinds[] = {world::kAir,     block::kStone, block::kWater, block::kOakLeaves,
                         block::kCaveAir, block::kSnow,  block::kOakLog};
    std::mt19937 rng(7);
    i16 wg_before[2][256];
    std::copy_n(nine.chunks[4]->height[hm::kWorldSurfaceWg], 256, wg_before[0]);
    std::copy_n(nine.chunks[4]->height[hm::kOceanFloorWg], 256, wg_before[1]);
    for (u32 n = 0; n < 60000; ++n) {
        // Many of them along a few columns and near their tops, where a heightmap has something to do.
        const i32 x = 10 * 16 + static_cast<i32>(rng() % 48), z = -7 * 16 + static_cast<i32>(rng() % 48);
        const i32 top = r.height(hm::kWorldSurface, x, z);
        const i32 y = rng() % 4 ? std::clamp(top - 3 + static_cast<i32>(rng() % 6), kMinY, kMinY + settings::kHeight - 1)
                                : kMinY + static_cast<i32>(rng() % static_cast<u32>(settings::kHeight));
        const u16 state = kinds[rng() % std::size(kinds)];
        ASSERT_TRUE(r.set_block(x, y, z, state));
        if (n % 97 == 0)
            for (const u32 type : {hm::kWorldSurface, hm::kOceanFloor, hm::kMotionBlocking, hm::kMotionBlockingNoLeaves})
                ASSERT_EQ(r.height(type, x, z), counted(r, type, x, z)) << "write " << n << ", heightmap " << type;
    }
    for (i32 x = 10 * 16; x < 13 * 16; ++x)
        for (i32 z = -7 * 16; z < -4 * 16; ++z)
            for (const u32 type : {hm::kWorldSurface, hm::kOceanFloor, hm::kMotionBlocking, hm::kMotionBlockingNoLeaves})
                ASSERT_EQ(r.height(type, x, z), counted(r, type, x, z)) << x << ", " << z << ", heightmap " << type;
    // The two the terrain was made by are no feature's to move.
    EXPECT_TRUE(std::equal(wg_before[0], wg_before[0] + 256, nine.chunks[4]->height[hm::kWorldSurfaceWg]));
    EXPECT_TRUE(std::equal(wg_before[1], wg_before[1] + 256, nine.chunks[4]->height[hm::kOceanFloorWg]));

    // Packed, the chunks hold what was read through the region - and a second region reads it again.
    std::vector<u16> seen;
    for (i32 x = 10 * 16; x < 13 * 16; x += 5)
        for (i32 z = -7 * 16; z < -4 * 16; z += 3)
            for (i32 y = kMinY; y < kMinY + settings::kHeight; ++y) seen.push_back(r.block(x, y, z));
    r.commit();
    Region again(work, world, nine.chunks, 10, -7, 3, 3);
    again.begin(11, -6);
    usize at = 0;
    for (i32 x = 10 * 16; x < 13 * 16; x += 5)
        for (i32 z = -7 * 16; z < -4 * 16; z += 3)
            for (i32 y = kMinY; y < kMinY + settings::kHeight; ++y) ASSERT_EQ(again.block(x, y, z), seen[at++]);
    // A chunk's heightmaps, counted anew from its packed sections, are the ones that were kept.
    for (u32 slot = 0; slot < 9; ++slot) {
        world::GenChunk& c = *nine.chunks[slot];
        i16 kept[hm::kCount][256];
        std::memcpy(kept, c.height, sizeof kept);
        prime_heightmaps(
            c, 1u << hm::kWorldSurface | 1u << hm::kOceanFloor | 1u << hm::kMotionBlocking | 1u << hm::kMotionBlockingNoLeaves);
        EXPECT_EQ(std::memcmp(kept, c.height, sizeof kept), 0) << "chunk " << slot;
    }
    again.commit();
}

TEST(WorldgenRegion, AirIntoNothingButAirIsNotSet) {
    const Overworld world(1);
    RegionWork work;
    Nine nine;
    Region r(work, world, nine.chunks, 10, -7, 3, 3);
    r.begin(11, -6);
    const i32 x = 11 * 16, z = -6 * 16;
    // The section at y 200 holds cave air where x and z are 0: air set there changes nothing
    // (ProtoChunk.setBlockState returns before it sets), and cave air stays what the chunk holds.
    EXPECT_TRUE(r.set_block(x, 201, z, world::kAir));
    r.commit();
    EXPECT_EQ(nine.chunks[4]->sections[(201 - kMinY) >> 4].get(0, (201 - kMinY) & 15, 0), block::kCaveAir);
    // Stone beside it makes the section one that is read: now the cave air is there to see, and air replaces it.
    Region second(work, world, nine.chunks, 10, -7, 3, 3);
    second.begin(11, -6);
    EXPECT_TRUE(second.set_block(x + 1, 201, z, block::kStone));
    EXPECT_EQ(second.block(x, 201, z), block::kCaveAir);
    EXPECT_TRUE(second.set_block(x, 201, z, world::kAir));
    EXPECT_EQ(second.block(x, 201, z), world::kAir);
    second.commit();
}

TEST(WorldgenRegion, MarksTicksAndBlockEntities) {
    const Overworld world(1);
    RegionWork work;
    Nine nine;
    Region r(work, world, nine.chunks, 10, -7, 3, 3);
    r.begin(11, -6);
    const i32 x = 11 * 16 + 4, z = -6 * 16 + 9;
    world::GenChunk& c = *nine.chunks[4];
    // A mushroom marks itself, a magma block the block above (BlockBehaviour.Properties.postProcess) - unless the flags say no.
    r.set_block(x, 70, z, block::kBrownMushroom);
    r.set_block(x, 20, z, block::kMagmaBlock);
    r.set_block(x, 30, z, block::kMagmaBlock, 3 | 16);
    r.set_block(x, 71, z, block::kStone);
    const auto mark = [&](i32 y) {
        return static_cast<u32>((y - kMinY) >> 4) << 12 | 4u | static_cast<u32>((y - kMinY) & 15) << 4 | 9u << 8;
    };
    ASSERT_EQ(c.marks.size(), 2u);
    EXPECT_EQ(c.marks[0], mark(70));
    EXPECT_EQ(c.marks[1], mark(21));
    r.mark_post_process(x, 71, z);
    r.mark_post_process(x, 71, z);  // (a list, not a set)
    EXPECT_EQ(c.marks.size(), 4u);
    r.mark_post_process(x, kMinY - 1, z);
    r.mark_post_process(13 * 16, 5, z);
    EXPECT_EQ(c.marks.size(), 4u);
    EXPECT_EQ(r.counts().far_marks, 1u);

    // One tick a block and type; a fluid's is another list.
    r.schedule_tick(x, 70, z, 111, false);
    r.schedule_tick(x, 70, z, 111, false, -1);
    r.schedule_tick(x, 70, z, 112, false);
    r.schedule_tick(x, 70, z, 111, true);
    r.schedule_tick(x - 16, 70, z, 111, false);
    ASSERT_EQ(c.block_ticks.size(), 2u);
    EXPECT_EQ(c.block_ticks[0].type, 111u);
    EXPECT_EQ(c.block_ticks[0].delay, 0);
    EXPECT_EQ(c.block_ticks[0].x, 4);
    EXPECT_EQ(c.block_ticks[0].z, 9);
    EXPECT_EQ(c.block_ticks[0].y, static_cast<u32>(70 - kMinY));
    EXPECT_EQ(c.fluid_ticks.size(), 1u);
    EXPECT_EQ(nine.chunks[3]->block_ticks.size(), 1u);
    for (i32 y = 80; y < 80 + 3000 / 16; ++y)  // enough of them for the set to grow
        for (i32 dx = 0; dx < 16; ++dx) r.schedule_tick(11 * 16 + dx, y, z, 5, false);
    const usize many = c.block_ticks.size();
    for (i32 y = 80; y < 80 + 3000 / 16; ++y)
        for (i32 dx = 0; dx < 16; ++dx) r.schedule_tick(11 * 16 + dx, y, z, 5, false);
    EXPECT_EQ(c.block_ticks.size(), many);

    // A block that has a block entity leaves a placeholder; a tag may be put in its place; another
    // block there takes it away.
    EXPECT_EQ(r.block_entity(x, 90, z), nullptr);
    r.set_block(x, 90, z, block::kChest);
    ASSERT_NE(r.block_entity(x, 90, z), nullptr);
    EXPECT_TRUE(r.block_entity_tag(x, 90, z).empty());
    const u8 tag[] = {10, 0};
    EXPECT_TRUE(r.set_block_entity_tag(x, 90, z, tag));
    EXPECT_EQ(r.block_entity_tag(x, 90, z).size(), 2u);
    EXPECT_FALSE(r.set_block_entity_tag(x, 91, z, tag));
    r.set_block(x, 90, z, block::kChest);  // (set again: what was known of it is gone)
    EXPECT_TRUE(r.block_entity_tag(x, 90, z).empty());
    r.set_block(x, 90, z, block::kStone);
    EXPECT_EQ(r.block_entity(x, 90, z), nullptr);
    EXPECT_TRUE(c.block_entities.empty());
    r.commit();

    // A second region over the same chunks knows the ticks they came with.
    Region again(work, world, nine.chunks, 10, -7, 3, 3);
    again.begin(11, -6);
    again.schedule_tick(x, 70, z, 111, false);
    again.schedule_tick(x, 70, z, 111, true);
    EXPECT_EQ(c.block_ticks.size(), many);
    EXPECT_EQ(c.fluid_ticks.size(), 1u);
    again.commit();
}

TEST(WorldgenRegion, AChunkIsDecoratedTheSameInAnyRectangle) {
    // The nine of a chunk inside an area of sixteen: what lies outside the nine is not there to
    // the chunk that is decorated, though the region holds it.
    const Overworld world(1);
    RegionWork work;
    Nine area(4);
    Region r(work, world, area.chunks, 10, -7, 4, 4);
    r.begin(11, -6);
    EXPECT_EQ(r.block(13 * 16, 10, -6 * 16), world::kAir);
    EXPECT_FALSE(r.set_block(13 * 16, 10, -6 * 16, block::kOakLog));
    EXPECT_FALSE(r.can_write(11 * 16, -4 * 16));
    r.begin(12, -5);
    EXPECT_EQ(r.block(13 * 16, 10, -6 * 16), block::kStone);
    EXPECT_TRUE(r.set_block(13 * 16, 10, -6 * 16, block::kOakLog));
    EXPECT_EQ(r.block(10 * 16, 10, -6 * 16), world::kAir);  // (and now this one is too far)
    r.commit();
    EXPECT_EQ(area.chunks[1 * 4 + 3]->sections[(10 - kMinY) >> 4].get(0, (10 - kMinY) & 15, 0), block::kOakLog);
}
