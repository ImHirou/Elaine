// SPDX-License-Identifier: GPL-3.0-or-later
// LightEngine: sky and block light, a port of vanilla's algorithm (net.minecraft.world.level.lighting
// LightEngine / BlockLightEngine / SkyLightEngine / *SectionStorage / ChunkSkyLightSources, read from
// the 26.3 jar) onto our own storage.
//
// Why a port and not "any correct flood fill": the client is sent the light arrays, and what
// vanilla stores is not just the fixed point of the propagation rule. Light exists only in sections
// that have a block within one section of them; sections without storage inherit the bottom layer
// of the first stored section above; a layer that was never written goes out as a mask bit while a
// written all-zero layer goes out as 2 KiB. Following the same steps in the same order is the only
// way to send what a vanilla server would have sent. Each function below names its original.
//
// What differs is where the data lives: vanilla keys everything by packed longs in hash maps; here
// a section's light is two tagged pointers inside its Chunk, reached through the ChunkMap.
//
// Tick thread only. Order of use for a new chunk: fill_sky_sources -> init_chunk -> (all eight
// neighbours initialised too) -> light_chunk. For a block change: update_section_status (if the
// section's emptiness flipped), update_sky_sources, check_block, then run_updates once per tick.
#pragma once

#include <vector>

#include "world/chunk.hpp"
#include "world/chunk_map.hpp"

namespace world {

class LightEngine {
  public:
    using Layout = world::Layout;
    enum Kind : u32 { kBlock, kSky };

    LightEngine(ChunkMap& chunks, const Layout& layout);

    // ---- chunk lifecycle
    void fill_sky_sources(Chunk& c);  // ChunkSkyLightSources.fillFrom
    void init_chunk(Chunk& c);        // ThreadedLevelLightEngine.initializeLight
    void light_chunk(Chunk& c);       // ThreadedLevelLightEngine.lightChunk (+ the updates it triggers)
    void release_chunk(Chunk& c);     // free the chunk's light arrays (chunk is being destroyed)
    void unload_chunk(Chunk& c);      // ThreadedLevelLightEngine.updateChunkStatus, then release_chunk
    // LevelLightEngine.queueSectionData, before init_chunk: a layer that was saved with the chunk.
    void queue_layer(Chunk& c, Kind k, u32 li, const LightLayer& loaded);

    // ---- block changes (LevelChunk.setBlockState calls these in this order)
    // `retain`: the section is "empty" because its chunk leaves the world (see unload_chunk).
    void update_section_status(Chunk& c, u32 section_index, bool empty, bool retain = false);
    bool update_sky_sources(Chunk& c, u32 lx, i32 y, u32 lz);  // ChunkSkyLightSources.update
    void check_block(i32 x, i32 y, i32 z);
    // LevelLightEngine.runLightUpdates: block engine, then sky engine. Returns queue entries processed.
    u32 run_updates();
    bool has_work() const { return inconsistent_ || !check_.empty(); }

    // ---- queries (LayerLightSectionStorage.getLightValue)
    u32 light(Kind k, i32 x, i32 y, i32 z) const;
    // Lowest y in the column with direct sky light; INT32_MIN when the column is open to the void.
    i32 lowest_source_y(const Chunk& c, u32 lx, u32 lz) const;

    // Chunks whose light sections changed (see Chunk::sky_changed / block_changed) or whose set of
    // stored sections changed since the list was last cleared. The owner clears light_listed.
    std::vector<Chunk*>& changed() { return changed_; }
    BlockPool& layer_pool() { return pool_; }
    // Fold uniform arrays of a chunk back into tags (memory only; never changes values or the wire).
    void fold_layers(Chunk& c);

  private:
    struct Node {
        i32 x, y, z;
        u32 data;
    };
    struct Fifo {
        std::vector<Node> v;
        usize head = 0;
        bool empty() const { return head == v.size(); }
        void push(i32 x, i32 y, i32 z, u32 data) { v.push_back({x, y, z, data}); }
        Node pop() {
            const Node n = v[head++];
            if (head == v.size()) {
                v.clear();
                head = 0;
            }
            return n;
        }
    };
    struct Pending {
        Chunk* c;
        u32 li;
    };

    ALWAYS_INLINE Chunk* chunk_at(i32 cx, i32 cz) const { return chunks_.find(cx, cz); }
    ALWAYS_INLINE u32 light_index(i32 y) const { return static_cast<u32>((y >> 4) - min_light_section_); }
    ALWAYS_INLINE static LightLayer* layers(Chunk* c, Kind k) { return k == kSky ? c->sky : c->block; }
    ALWAYS_INLINE static u32 nibble(i32 x, i32 y, i32 z) {
        return (static_cast<u32>(y & 15) << 8) | (static_cast<u32>(z & 15) << 4) | static_cast<u32>(x & 15);
    }
    ALWAYS_INLINE BlockState state_in(const Chunk* c, i32 x, i32 y, i32 z) const {
        const u32 si = static_cast<u32>((y >> 4) - min_section_);
        if (UNLIKELY(si >= sections_)) return kAir;  // LevelChunk.getBlockState outside the build height
        return c->sections[si].get(Section::index(static_cast<u32>(x & 15), static_cast<u32>(y & 15), static_cast<u32>(z & 15)));
    }
    BlockState state_at(i32 x, i32 y, i32 z) const;
    bool storing(Kind k, i32 cx, i32 sy, i32 cz) const;
    void set_level(Kind k, Chunk* c, u32 li, i32 x, i32 y, i32 z, u32 level);
    void mark(Kind k, Chunk* c, u32 li);
    void mark_block_and_around(Kind k, Chunk* c, i32 x, i32 y, i32 z);
    void mark_section_and_neighbours(Kind k, i32 cx, i32 sy, i32 cz);
    void list(Chunk* c);

    // LayerLightSectionStorage
    void put_section_state(Chunk& c, u32 li, u8 state, bool retain = false);
    void initialize_section(Chunk& c, u32 li);
    void process_removals(Kind k);
    LightLayer create_sky_layer(Chunk& c, u32 li);
    i32 sky_top(const Chunk& c) const { return c.sky_top == Chunk::kNoSkyTop ? sky_lowest_ : c.sky_top; }
    bool sky_above_data(const Chunk& c, i32 sy) const {  // SkyLightSectionStorage.isAboveData
        const i32 top = sky_top(c);
        return top == sky_lowest_ || sy >= top;
    }
    bool sky_data_at_or_below(i32 sy) const { return sy >= sky_lowest_; }

    // LightEngine
    u32 run(Kind k);
    void block_check_node(const Node& n);
    void block_propagate_increase(const Node& n, u32 from_level);
    void block_propagate_decrease(const Node& n);
    void block_propagate_sources(Chunk& c);

    // SkyLightEngine
    void sky_check_node(const Node& n);
    void sky_update_sources_in_column(i32 x, i32 z, i32 lowest_source);
    void sky_remove_sources_below(i32 x, i32 z, i32 lowest_source, i32 world_bottom);
    void sky_add_sources_above(i32 x, i32 z, i32 lowest_source, i32 world_bottom);
    void sky_propagate_increase(const Node& n, u32 from_level);
    void sky_propagate_decrease(const Node& n);
    u32 sky_empty_sections_below(const Node& n) const;
    void sky_propagate_from_empty_sections(i32 x, i32 y, i32 z, u32 dir, u32 level, bool increase, u32 empty_below);
    void sky_propagate_sources(Chunk& c);
    i32 lowest_source_at(i32 x, i32 z, i32 fallback) const;

    // ChunkSkyLightSources
    i32 raw_source(const Chunk& c, u32 i) const { return static_cast<i32>(c.sky_sources.get(i)) + source_base_; }
    i32 find_lowest_source_below(const Chunk& c, u32 lx, i32 start_y, u32 lz, BlockState start_state) const;
    bool update_source_edge(Chunk& c, u32 i, i32 old_edge, u32 lx, u32 lz, i32 top_y, BlockState top, BlockState bottom);

    ChunkMap& chunks_;
    BlockPool pool_{LightLayer::kBytes};
    i32 min_y_, min_section_, min_light_section_;
    u32 sections_, light_sections_;
    bool has_sky_;
    i32 source_base_;             // ChunkSkyLightSources.minY = level min y - 1
    i32 sky_lowest_ = INT32_MAX;  // SkyDataLayerStorageMap.currentLowestY
    Layout layout_;

    Fifo dec_[2], inc_[2];
    std::vector<Node> check_;        // block positions to re-check (both engines look at each)
    std::vector<Pending> removals_;  // sections that lost their last neighbour with blocks
    std::vector<Chunk*> changed_;
    bool inconsistent_ = false;
};

}  // namespace world
