// SPDX-License-Identifier: GPL-3.0-or-later
// Dimension: one level - its chunks, the light engine over them, the cache of wire frames, and
// the coming and going of chunks.
//
// A chunk's life (the same staging as vanilla's chunk statuses, for the same reason: a chunk's
// light depends on its neighbours' blocks):
//
//   request(c)  a worker reads it from its region file, or runs the generator, and derives
//               heightmaps and sky sources (world/store.hpp, world/chunk_io.hpp)
//   adopt       pump() copies it into the world's pools and registers its sections - and the
//               light it was saved with - with the light engine                 (-> LightReady)
//   lit         all 8 neighbours loaded: its own light sources propagate, unless it came lit
//               from disk                                                        (-> Lit)
//   ticking     all 8 neighbours lit: light is final, the chunk may be sent, edited and
//               broadcast. Its frame - the packet, compressed - is built when first asked for:
//               the tick thread writes the body, a worker deflates it; it is cached until a
//               block or a light value in the chunk changes.
//   save(c)     its bytes go to the store, which writes them with others of its region
//   unload(c)   out of the world: its neighbours stop ticking, the light engine forgets it
//
// A chunk that entered through request() is promoted as soon as its neighbours allow it, so a
// loaded square of radius r + 2 has its inner r + 1 lit and its inner r ticking. Who wants which
// chunks, and when one has been idle long enough to go, is the game's to say (server/chunks.cpp).
//
// load() / lit() / ticking() / frame() do the same steps at once, on the calling thread and in
// the order the caller names them: tests, benchmarks and start-up.
//
// Tick thread only. Nothing here locks: a worker is handed a chunk outside the world, or bytes,
// never the world.
#pragma once

#include <span>
#include <string>
#include <vector>

#include "base/buf.hpp"
#include "base/workers.hpp"
#include "world/chunk.hpp"
#include "world/chunk_map.hpp"
#include "world/generator.hpp"
#include "world/light.hpp"
#include "world/store.hpp"
#include "world/ticks.hpp"

namespace world {

// In a build with DCHECKs the random ticks of sections that are known to have nothing to do are
// run all the same, to be caught doing something (Game::run_random_ticks).
#ifdef NDEBUG
inline constexpr bool kCheckDormantSections = false;
#else
inline constexpr bool kCheckDormantSections = true;
#endif

class Dimension {
  public:
    // One changed block, in the form the broadcast needs: sorted, these group by chunk and section.
    struct BlockChange {
        Chunk* chunk;
        u16 section;  // section index
        u16 pos;      // SectionPos.sectionRelativePos: x<<8 | z<<4 | y
        bool operator==(const BlockChange&) const = default;
    };
    // A chunk that entered the world in pump(), with what it was made of: the game reads the
    // parts it keeps there (block entities, entities).
    struct Adopted {
        Chunk* chunk;
        LoadedChunk* from;
    };

    // `type_id` is the network id in the dimension_type registry (reg::dimension_type). Without
    // workers everything runs on the calling thread; without a directory nothing is kept.
    Dimension(u32 type_id, std::string name, ChunkGenerator& gen, i32 compression_threshold, base::Workers* workers = nullptr,
              std::string dir = {}, bool sync = true);
    ~Dimension();
    Dimension(const Dimension&) = delete;
    Dimension& operator=(const Dimension&) = delete;

    u32 type_id() const { return type_id_; }
    // EnvironmentAttributes.FAST_LAVA and WATER_EVAPORATES of the dimension: the nether's.
    bool fast_lava() const { return fast_lava_; }
    bool water_evaporates() const { return water_evaporates_; }
    const std::string& name() const { return name_; }
    i32 min_y() const { return min_y_; }
    i32 max_y() const { return max_y_; }  // inclusive
    u32 sections() const { return sections_; }
    bool has_skylight() const { return has_sky_; }
    const Layout& layout() const { return layout_; }
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

    // ---- chunks: coming and going
    Chunk* find(i32 cx, i32 cz) const { return map_.find(cx, cz); }
    // Have the chunk loaded: nothing if it is in the world or on its way.
    void request(i32 cx, i32 cz) {
        if (map_.find(cx, cz) || map_.loading(cx, cz)) return;
        map_.set_loading(cx, cz, true);
        store_.load(cx, cz);
    }
    // Once a tick: take what the workers have finished. Chunks are adopted - until `deadline_ns`
    // has passed (0: all of them), the rest wait for the next call - then lit and made ticking as
    // far as their neighbours allow.
    void pump(u64 tick, u64 deadline_ns = 0);
    // With keep_adopted(true), what pump() adopted stays listed until release_adopted().
    void keep_adopted(bool on) { keep_adopted_ = on; }
    std::vector<Adopted>& adopted() { return adopted_; }
    void release_adopted();
    // Chunks that began to tick, and chunks that stopped (a neighbour left), since the caller
    // last emptied the lists.
    std::vector<Chunk*>& ready() { return ready_; }
    std::vector<Chunk*>& stopped() { return stopped_; }

    bool persistent() const { return store_.persistent(); }
    ChunkStore& store() { return store_; }
    // Hand the chunk's bytes to the store, with the parts the game keeps in it.
    void save(Chunk& c, const ChunkParts& parts);
    // Take the chunk out of the world (the caller has saved what is to be kept of it). The
    // chunks around it stop ticking and are listed in stopped().
    void unload(Chunk& c);
    // Visit some chunks: see ChunkMap::scan.
    template <class F>
    void scan(u32 budget, F&& f) {
        map_.scan(budget, f);
    }
    template <class F>
    void for_each(F&& f) const {
        map_.for_each(f);
    }
    usize chunk_count() const { return map_.size(); }
    usize loading_count() const { return map_.loading_count(); }

    // ---- chunks: at once, on this thread
    Chunk& load(i32 cx, i32 cz);
    Chunk& lit(i32 cx, i32 cz);
    Chunk& ticking(i32 cx, i32 cz);
    // The level_chunk_with_light packet for (cx, cz), framed for this server's compression
    // threshold. Valid until the next call that changes the world.
    std::span<const u8> frame(i32 cx, i32 cz);
    // The same for a chunk that ticks, if its frame is there; if not, empty - and a worker is
    // put to it, so that a later call has it.
    std::span<const u8> frame_if_ready(Chunk& c);

    // ---- blocks
    // Air outside the build height and in chunks that are not loaded.
    ALWAYS_INLINE BlockState block(i32 x, i32 y, i32 z) const {
        const Chunk* c = map_.find(x >> 4, z >> 4);
        const u32 si = static_cast<u32>((y - min_y_) >> 4);
        if (UNLIKELY(!c || si >= sections_)) return kAir;
        return c->sections[si].get(Section::index(static_cast<u32>(x & 15), static_cast<u32>(y & 15), static_cast<u32>(z & 15)));
    }
    // LevelChunk.setBlockState, the part that is about the block itself: store the state, keep
    // heightmaps, light and the random-tick counts in step, and record the change for this tick's
    // broadcast. False when nothing changed (same state, outside the world, chunk not ticking).
    // What the change means to the blocks around it is the game's (Game::set_block).
    bool set_block(i32 x, i32 y, i32 z, BlockState s);
    // ChunkHolder.blockChanged: the block is told to its viewers with this tick's changes,
    // though it is the block it was (what its block entity holds is not).
    void block_changed(i32 x, i32 y, i32 z);

    // ---- scheduled ticks (world/ticks.hpp)
    // The game time, as the level knows it (Level.getGameTime): told once a tick, before
    // anything of that tick is scheduled or saved.
    void set_time(u64 game_time) { time_ = game_time; }
    u64 time() const { return time_; }
    // LevelAccessor.scheduleTick(pos, block, delay, priority): nothing where no chunk is loaded.
    void schedule_tick(i32 x, i32 y, i32 z, u32 block, i32 delay, i32 priority = kTickPriorityNormal) {
        if (Chunk* c = map_.find(x >> 4, z >> 4)) {
            ticks_.schedule(*c, x, y, z, block, static_cast<i64>(time_) + delay, priority);
            c->dirty = true;
        }
    }
    bool has_scheduled_tick(i32 x, i32 y, i32 z, u32 block) const {
        const Chunk* c = map_.find(x >> 4, z >> 4);
        return c && ticks_.has(*c, x, y, z, block);
    }
    LevelTicks& block_ticks() { return ticks_; }
    // LevelAccessor.scheduleTick(pos, fluid, delay): the fluids' list, which runs after the
    // blocks' (ServerLevel.fluidTicks). `fluid`: its id in the fluid registry.
    void schedule_fluid_tick(i32 x, i32 y, i32 z, u32 fluid, i32 delay) {
        if (Chunk* c = map_.find(x >> 4, z >> 4)) {
            fluid_ticks_.schedule(*c, x, y, z, fluid, static_cast<i64>(time_) + delay);
            c->dirty = true;
        }
    }
    bool has_scheduled_fluid_tick(i32 x, i32 y, i32 z, u32 fluid) const {
        const Chunk* c = map_.find(x >> 4, z >> 4);
        return c && fluid_ticks_.has(*c, x, y, z, fluid);
    }
    LevelTicks& fluid_ticks() { return fluid_ticks_; }

    // ---- random ticks
    // A random tick is three cells picked in each section of each simulated chunk, every game
    // tick (ServerLevel.tickChunk) - and nearly all of them find nothing to do: the cell is not
    // one that ticks, or it is grass that is neither dying nor has anywhere to spread to. So the
    // sections are counted instead of asked: how many of a section's cells tick at random
    // (Section::ticking), how many of those may act whatever is around them, how many are dying
    // under what stands on them, how many cells are soil a spreader could take (RandomCounts,
    // kept by set_block cell by cell, counted in full when a chunk is loaded). A section whose
    // counts say that none of its random ticks can do anything - it is "dormant" - is not
    // ticked, and costs nothing until a block changes within its reach. What remains is this
    // list: the chunks that tick, are simulated, and hold a section with something to do.
    const std::vector<Chunk*>& random_chunks() const { return random_; }
    // A player's simulation distance came to cover the chunk, or ceased to (see Chunk::sim_refs).
    void simulate(Chunk& c, i32 sim_delta, i32 block_delta);

    // ---- block entities (LevelChunk.blockEntities). The dimension keeps where they are and of
    // which type - what a chunk packet says; `data` belongs to the caller.
    // The block entity at a position, null if there is none.
    const BlockEntityRef* block_entity(i32 x, i32 y, i32 z) const;
    // LevelChunk.addAndRegisterBlockEntity. The position must be free and in a loaded chunk.
    void add_block_entity(i32 x, i32 y, i32 z, u32 type, u32 data);
    // LevelChunk.removeBlockEntity: false if there was none; `data` receives its handle.
    bool remove_block_entity(i32 x, i32 y, i32 z, u32& data);
    // What the block entity at a block shows a client (BlockEntity.getUpdateTag): a tag in the
    // network form, copied; empty for none. The chunk's packet carries it from now on.
    void set_block_entity_tag(i32 x, i32 y, i32 z, std::span<const u8> tag);
    std::span<const u8> block_entity_tag(i32 x, i32 y, i32 z) const;
    // What the chunk at a block holds changed in a way only its owner knows (a container's
    // contents): it is to be saved again.
    void mark_dirty(i32 x, i32 z) {
        if (Chunk* c = map_.find(x >> 4, z >> 4)) c->dirty = true;
    }

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
    struct FrameJob;
    void update_heightmaps(Chunk& c, u32 lx, i32 y, u32 lz, BlockState s);
    void encode(Chunk& c);
    void write_body(Chunk& c, base::ByteBuf& to);
    bool set_block_raw(Chunk& c, i32 x, i32 y, i32 z, BlockState s);
    static void free_block_entities(Chunk& c);
    void recount_random(Chunk& c, u32 si, u32 lx, i32 y, u32 lz, BlockState old, BlockState s);
    u32 active_mask(const Chunk& c) const;
    void refresh_active(Chunk& c);
    void status_changed(Chunk& c);
    void start_ticking(Chunk& c);
    void stop_ticking(Chunk& c);
    Chunk& adopt(LoadedChunk& lc);
    void promote_around(i32 cx, i32 cz);
    void invalidate(Chunk& c) {
        c.frame_valid = false;
        c.frame_version = ++versions_;
    }
    static void frame_work(base::Task& t);
    static void frame_done(base::Task& t);

    u32 type_id_;
    std::string name_;
    ChunkGenerator& gen_;
    i32 threshold_;
    i32 min_y_, max_y_;
    u32 sections_;
    bool has_sky_;
    bool fast_lava_, water_evaporates_;
    Layout layout_;
    bool large_shapes_ = false, entity_blocks_ = false;
    bool keep_adopted_ = false;

    ChunkMap map_;
    SectionAllocator alloc_;
    BlockPool chunk_pool_{sizeof(Chunk)};
    LightEngine light_;
    base::Workers own_workers_{0};  // when none are given: everything on the calling thread
    base::Workers& workers_;
    ChunkStore store_;
    LevelTicks ticks_, fluid_ticks_;
    u64 time_ = 0;
    std::vector<Chunk*> random_;
    base::ByteBuf tick_scratch_;
    std::vector<BlockChange> changes_;
    bool changes_sorted_ = true;
    base::ByteBuf scratch_;
    usize frame_bytes_ = 0;
    u64 versions_ = 0;
    u64 tick_ = 0;
    std::vector<Adopted> adopted_;
    std::vector<Chunk*> ready_, stopped_;
};

}  // namespace world
