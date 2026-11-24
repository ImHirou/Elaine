// SPDX-License-Identifier: GPL-3.0-or-later
// The chunks a decoration works in: what vanilla's WorldGenRegion is to a feature
// (net.minecraft.server.level.WorldGenRegion, and the ProtoChunk behind it, of the 26.3 jar).
//
// A chunk is decorated with the eight around it at hand: its features read all nine and write
// into all nine. A Region is those nine - or a rectangle of chunks that holds the nine of several
// chunks, decorated one after the other (begin) - and every question a feature has of the world:
//
//   block, set_block   ProtoChunk.getBlockState / WorldGenRegion.setBlock: a write moves the four
//                      heightmaps a chunk keeps, leaves a block entity's placeholder, marks what the
//                      block says is to be looked at when the chunk first ticks
//   put_raw            LevelChunkSection.setBlockState, which an ore goes to directly: the block,
//                      and nothing that follows from it
//   height             Heightmap.getFirstAvailable of one of vanilla's six
//   biome              BiomeManager.getBiome: the cell a block's biome is that of
//   raw_brightness     how light it is to whatever must stand in the dark
//
// The rules of the edge are vanilla's where vanilla has one, and they are about the nine, not
// about what else a Region happens to hold - so a chunk comes out the same whichever rectangle it
// was decorated in. A write beyond the nine is refused (WorldGenRegion.ensureCanWrite). A read
// beyond them finds a chunk with nothing in it yet: a server's region holds whatever the chunk
// there happens to be, and nothing in it is one of the things that can be. Neither happens with
// vanilla's features (mcgen.py worldgen census); both are counted.
//
// Light: a chunk that is decorated is not lit, and what a vanilla server tells a feature of the
// light there is a matter of which chunks around its light thread has been given
// (docs/architecture.md, "Features"). Here it is dark, as it is to a chunk made beside older
// ones; a stand that replays a server's run sets what the server said (set_brightness).
//
// Blocks are worked on unpacked. A section a feature touches is spread out into 4096 states of
// the thread's own (RegionWork) the first time, and packed again when the work is done (commit) -
// only if it was written to. A section's bytes follow from its 4096 states alone: the same
// chunk is the same bytes, however it came about.
#pragma once

#include <memory>
#include <vector>

#include "world/block.hpp"
#include "world/gen_chunk.hpp"
#include "worldgen/climate.hpp"
#include "worldgen/terrain.hpp"

namespace worldgen {

// What a thread decorates with, kept from one Region to the next: nothing is allocated for the
// second area a thread works on.
struct RegionWork {
    static constexpr u32 kChunks = 16;  // an area of 4 x 4
    static constexpr u32 kSections = kChunks * Terrain::kSections;

    RegionWork() : flat(new u16[usize{kSections} * 4096]) {}
    std::unique_ptr<u16[]> flat;
    std::vector<u64> ticks;  // the (chunk, list, type, block) that have a tick: an open-addressing set
    u32 tick_count = 0;
};

class Region {
  public:
    struct Pos {
        i32 x, y, z;
    };
    // What happened that vanilla's features do not do (and what a stand reports).
    struct Counts {
        u64 far_reads = 0, refused_writes = 0, far_marks = 0, far_ticks = 0, outside_height = 0;
    };

    // `chunks`: width * depth of them, by x + z * width, the first at (x0, z0); at most
    // RegionWork::kChunks. They are the Region's until commit().
    Region(RegionWork& work, const Overworld& world, world::GenChunk* const* chunks, i32 x0, i32 z0, u32 width, u32 depth);
    ~Region() { DCHECK(used_ == 0); }
    Region(const Region&) = delete;
    Region& operator=(const Region&) = delete;

    // The chunk that is decorated from here on: it and the eight around it must be in the Region.
    void begin(i32 chunk_x, i32 chunk_z);
    // Pack what was written; the chunks are their owner's again.
    void commit();

    // The chunk that is decorated.
    world::GenChunk& center() { return *chunks_[nine_ + 1 + width_]; }
    i32 center_x() const { return center_x_; }
    i32 center_z() const { return center_z_; }
    const Overworld& world() const { return world_; }
    const Counts& counts() const { return counts_; }

    static constexpr i32 kMinY = settings::kMinY, kHeight = settings::kHeight;
    static bool outside_height(i32 y) { return static_cast<u32>(y - kMinY) >= static_cast<u32>(kHeight); }

    // ProtoChunk.getBlockState: void air beyond the world's height, and plain air wherever a section
    // holds nothing but air of some kind.
    ALWAYS_INLINE u16 block(i32 x, i32 y, i32 z) {
        const u32 up = static_cast<u32>(y - kMinY);
        if (UNLIKELY(up >= static_cast<u32>(kHeight))) return proto::gen::block::kVoidAir;
        const u32 slot = slot_of(x >> 4, z >> 4);
        if (UNLIKELY(slot == kNoSlot)) return far_read();
        const u32 section = slot * Terrain::kSections + (up >> 4);
        if (non_air_[section] == 0) return world::kAir;
        const u16* states = flat_[section];
        if (UNLIKELY(!states)) states = spread(section);
        return states[(up & 15) << 8 | (static_cast<u32>(z) & 15) << 4 | (static_cast<u32>(x) & 15)];
    }
    // What a section itself holds, as BulkSectionAccess reads it (an ore): no air where air of another
    // kind is, and air beyond the world's height.
    ALWAYS_INLINE u16 raw(i32 x, i32 y, i32 z) {
        const u32 up = static_cast<u32>(y - kMinY);
        if (UNLIKELY(up >= static_cast<u32>(kHeight))) return world::kAir;
        const u32 slot = slot_of(x >> 4, z >> 4);
        if (UNLIKELY(slot == kNoSlot)) return far_read();
        const u32 section = slot * Terrain::kSections + (up >> 4);
        const u16* states = flat_[section];
        if (UNLIKELY(!states)) {
            const world::Section& packed = chunks_[slot]->sections[up >> 4];
            if (packed.uniform()) return packed.single;
            states = spread(section);
        }
        return states[(up & 15) << 8 | (static_cast<u32>(z) & 15) << 4 | (static_cast<u32>(x) & 15)];
    }
    // WorldGenRegion.setBlock. False: the block is in none of the nine chunks.
    bool set_block(i32 x, i32 y, i32 z, u16 state, u32 flags = 3);
    // The block alone (BulkSectionAccess: ensureCanWrite, then the section itself). False as above.
    bool put_raw(i32 x, i32 y, i32 z, u16 state);
    // WorldGenRegion.ensureCanWrite, for what asks before it does anything.
    bool can_write(i32 x, i32 z) const { return slot_of(x >> 4, z >> 4) != kNoSlot; }

    // WorldGenRegion.getHeight: the first free y of a column, by world::heightmap.
    ALWAYS_INLINE i32 height(u32 type, i32 x, i32 z) {
        const u32 slot = slot_of(x >> 4, z >> 4);
        if (UNLIKELY(slot == kNoSlot)) {
            ++counts_.far_reads;
            return kMinY;
        }
        return chunks_[slot]->height[type][(static_cast<u32>(x) & 15) | (static_cast<u32>(z) & 15) << 4] + kMinY;
    }

    // LevelReader.getBiome: the biome of the cell BiomeManager picks for the block.
    u16 biome(i32 x, i32 y, i32 z) const;
    // The biomes there are in the nine chunks, a bit each (what applyBiomeDecoration collects).
    void possible_biomes(u64 out[2]) const;

    // LevelLightEngine.getRawBrightness.
    i32 raw_brightness(i32 x, i32 y, i32 z) {
        if (UNLIKELY(asked_ != nullptr)) asked_->push_back({x, y, z});
        return brightness_;
    }
    // For a stand that replays a server's run: what it is told, and where every block it asked about goes.
    void set_brightness(i32 value, std::vector<Pos>* asked) {
        brightness_ = value;
        asked_ = asked;
    }

    // ProtoChunk.markPosForPostProcessing.
    void mark_post_process(i32 x, i32 y, i32 z);
    // LevelAccessor.scheduleTick, of a block (`fluid` false) or a fluid: kept once a block and type,
    // due at once (ProtoChunkTicks).
    void schedule_tick(i32 x, i32 y, i32 z, u32 type, bool fluid, i32 priority = 0);

    // The block entity at a block: nullptr, or the record - whose tag is empty until one is set.
    const world::GenBlockEntity* block_entity(i32 x, i32 y, i32 z) const;
    std::span<const u8> block_entity_tag(i32 x, i32 y, i32 z) const;
    // What a feature filled in (one nameless NBT compound). False: there is no block entity there.
    bool set_block_entity_tag(i32 x, i32 y, i32 z, std::span<const u8> tag);

    // For whoever holds a decoration to vanilla's feature by feature: every block that is set from
    // here on - where, and to what - is folded into `*log` (FNV-1a), in the order it is set.
    void set_log(u64* log) { log_ = log; }

    // WorldGenRegion.getRandom: one source for the whole decoration of a chunk, made from where the
    // chunk is ("worldgen_region_random").
    XoroshiroRandom& random() { return random_; }

  private:
    static constexpr u32 kNoSlot = ~0u;

    // The chunk's place in chunks_ if it is one of the nine, else kNoSlot.
    ALWAYS_INLINE u32 slot_of(i32 chunk_x, i32 chunk_z) const {
        const u32 dx = static_cast<u32>(chunk_x - center_x_ + 1), dz = static_cast<u32>(chunk_z - center_z_ + 1);
        if (UNLIKELY(dx > 2 || dz > 2)) return kNoSlot;
        return nine_ + dx + dz * width_;
    }
    NOINLINE u16 far_read() {
        ++counts_.far_reads;
        return world::kAir;
    }
    NOINLINE u16* spread(u32 section);
    NOINLINE void log_write(i32 x, i32 y, i32 z, u16 state);
    void update_height(world::GenChunk& chunk, u32 slot, u32 type, i32 x, i32 y, i32 z, u16 state);
    void put_placeholder(world::GenChunk& chunk, u32 at);
    void drop_block_entity(world::GenChunk& chunk, u32 at);
    bool tick_is_new(u64 key);

    RegionWork& work_;
    const Overworld& world_;
    world::GenChunk* chunks_[RegionWork::kChunks];
    i32 x0_, z0_;
    u32 width_, depth_;
    i32 center_x_ = 0, center_z_ = 0;
    u32 nine_ = 0;  // the slot of the chunk at (center - 1, center - 1)
    u32 used_ = 0;  // sections spread out so far
    u16* flat_[RegionWork::kSections];
    u16 non_air_[RegionWork::kSections];  // LevelChunkSection.nonEmptyBlockCount, as it is now
    u32 dirty_[RegionWork::kChunks];      // a bit a section: written to
    i32 brightness_ = 0;
    std::vector<Pos>* asked_ = nullptr;
    u64* log_ = nullptr;
    XoroshiroRandom random_{0, 0};
    Counts counts_;
};

// Heightmap.primeHeightmaps: each column counted from the top down to the first block that counts
// for a heightmap - for the heightmaps `types` has a bit of (world::heightmap).
void prime_heightmaps(world::GenChunk& chunk, u32 types);

}  // namespace worldgen
