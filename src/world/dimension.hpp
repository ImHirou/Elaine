// Dimension: one level - its chunks, the light engine over them, and the cache of wire frames.
//
// Chunk pipeline (the same staging as vanilla's chunk statuses, for the same reason: a chunk's
// light depends on its neighbours' blocks):
//
//   load(c)     generate blocks, derive heightmaps and sky sources, register the sections with
//               the light engine                                            (-> LightReady)
//   lit(c)      all 8 neighbours loaded, then its own light sources propagate    (-> Lit)
//   frame(c)    all 8 neighbours lit ("ticking"): light is final, the chunk may be sent, edited
//               and broadcast. The framed + compressed packet is built once and cached until a
//               block or a light value in the chunk changes.
//
// So sending a view of radius r generates (2r+5)^2 chunks, lights (2r+3)^2 and encodes (2r+1)^2.
// Chunks are never unloaded yet: that needs persistence (a player's edits must go somewhere) and
// the save/retain half of the light engine.
//
// Tick thread only. Nothing here locks; workers for generation and compression will get chunks
// handed to them, not shared.
#pragma once

#include <span>
#include <string>
#include <vector>

#include "base/buf.hpp"
#include "world/chunk.hpp"
#include "world/chunk_map.hpp"
#include "world/generator.hpp"
#include "world/light.hpp"

namespace world {

class Dimension {
  public:
    // One changed block, in the form the broadcast needs: sorted, these group by chunk and section.
    struct BlockChange {
        Chunk* chunk;
        u16 section;  // section index
        u16 pos;      // SectionPos.sectionRelativePos: x<<8 | z<<4 | y
        bool operator==(const BlockChange&) const = default;
    };

    // `type_id` is the network id in the dimension_type registry (reg::dimension_type).
    Dimension(u32 type_id, std::string name, ChunkGenerator& gen, i32 compression_threshold);
    ~Dimension();
    Dimension(const Dimension&) = delete;
    Dimension& operator=(const Dimension&) = delete;

    u32 type_id() const { return type_id_; }
    const std::string& name() const { return name_; }
    i32 min_y() const { return min_y_; }
    i32 max_y() const { return max_y_; }  // inclusive
    u32 sections() const { return sections_; }
    bool has_skylight() const { return has_sky_; }
    // Whether a block whose collision shape reaches out of its own cell (a fence, a wall: 1.5
    // blocks tall) may be anywhere in this dimension. Only then does a collision query have to
    // read the cells beside its box; never cleared.
    bool has_large_shapes() const { return large_shapes_; }
    // ...and whether a block that does something to an entity inside it or on it (a cobweb,
    // honey, slime) may be anywhere: only then are the blocks an entity passes through looked at.
    bool has_entity_blocks() const { return entity_blocks_; }
    // The same, asked of the sections that the cells [x0, x1] x [y0, y1] x [z0, z1] lie in: do
    // they hold anything but air (kBlocks), a shape that reaches out of its cell (kLarge), a
    // block that acts on an entity (kEntityBlocks), one that breaks a fall (kFallBlocks)? A
    // query's cells lie in one section, rarely in eight; this is a mask per chunk.
    static constexpr u32 kBlocks = 1, kLarge = 2, kEntityBlocks = 4, kFallBlocks = 8;
    ALWAYS_INLINE u32 holds(i32 x0, i32 y0, i32 z0, i32 x1, i32 y1, i32 z1) const {
        const i32 s0 = std::max((y0 - min_y_) >> 4, 0), s1 = std::min((y1 - min_y_) >> 4, static_cast<i32>(sections_) - 1);
        if (s0 > s1) return 0;
        const u32 mask = (2u << s1) - (1u << s0);
        u32 out = 0;
        for (i32 cz = z0 >> 4; cz <= z1 >> 4; ++cz) {
            for (i32 cx = x0 >> 4; cx <= x1 >> 4; ++cx) {
                const Chunk* c = map_.find(cx, cz);
                if (UNLIKELY(!c)) continue;
                out |= ((c->filled_sections & mask) ? kBlocks : 0) | ((c->large_sections & mask) ? kLarge : 0) |
                       ((c->entity_sections & mask) ? kEntityBlocks : 0) | ((c->fall_sections & mask) ? kFallBlocks : 0);
            }
        }
        return out;
    }
    ChunkGenerator& generator() { return gen_; }

    // ---- chunks
    Chunk* find(i32 cx, i32 cz) const { return map_.find(cx, cz); }
    Chunk& load(i32 cx, i32 cz);
    Chunk& lit(i32 cx, i32 cz);
    Chunk& ticking(i32 cx, i32 cz);
    // The level_chunk_with_light packet for (cx, cz), framed for this server's compression
    // threshold. Valid until the next call that changes the world.
    std::span<const u8> frame(i32 cx, i32 cz);
    usize chunk_count() const { return map_.size(); }

    // ---- blocks
    // Air outside the build height and in chunks that are not loaded.
    ALWAYS_INLINE BlockState block(i32 x, i32 y, i32 z) const {
        const Chunk* c = map_.find(x >> 4, z >> 4);
        const u32 si = static_cast<u32>((y - min_y_) >> 4);
        if (UNLIKELY(!c || si >= sections_)) return kAir;
        return c->sections[si].get(Section::index(static_cast<u32>(x & 15), static_cast<u32>(y & 15), static_cast<u32>(z & 15)));
    }
    // Level.setBlock(pos, state, UPDATE_ALL): store the state, keep heightmaps and light in step,
    // run the neighbour shape updates we implement, and record everything for this tick's
    // broadcast. False when nothing changed (same state, outside the world, chunk not ticking).
    bool set_block(i32 x, i32 y, i32 z, BlockState s);

    // ---- block entities (LevelChunk.blockEntities). The dimension keeps where they are and of
    // which type - what a chunk packet says; `data` belongs to the caller.
    // The block entity at a position, null if there is none.
    const BlockEntityRef* block_entity(i32 x, i32 y, i32 z) const;
    // LevelChunk.addAndRegisterBlockEntity. The position must be free and in a loaded chunk.
    void add_block_entity(i32 x, i32 y, i32 z, u32 type, u32 data);
    // LevelChunk.removeBlockEntity: false if there was none; `data` receives its handle.
    bool remove_block_entity(i32 x, i32 y, i32 z, u32& data);

    // ---- per tick
    // Bring light up to date with this tick's block changes. Call once, after all set_block calls
    // and before reading the change lists.
    void update_light();
    // This tick's block changes, sorted by chunk, section, position, without duplicates.
    std::vector<BlockChange>& block_changes();
    // Chunks with Chunk::sky_changed / block_changed bits to broadcast.
    std::vector<Chunk*>& light_changes() { return light_.changed(); }
    // Forget both lists (clears the per-chunk masks).
    void end_tick();

    LightEngine& light() { return light_; }
    const LightEngine& light() const { return light_; }
    usize memory_bytes() const;  // pools + cached frames, for diagnostics

  private:
    void prime_heightmaps(Chunk& c);
    void update_heightmaps(Chunk& c, u32 lx, i32 y, u32 lz, BlockState s);
    void encode(Chunk& c);
    bool set_block_raw(Chunk& c, i32 x, i32 y, i32 z, BlockState s);

    u32 type_id_;
    std::string name_;
    ChunkGenerator& gen_;
    i32 threshold_;
    i32 min_y_, max_y_;
    u32 sections_;
    bool has_sky_;
    bool large_shapes_ = false, entity_blocks_ = false;

    ChunkMap map_;
    SectionAllocator alloc_;
    BlockPool chunk_pool_{sizeof(Chunk)};
    LightEngine light_;
    std::vector<BlockChange> changes_;
    bool changes_sorted_ = true;
    base::ByteBuf scratch_;
    usize frame_bytes_ = 0;
};

}  // namespace world
