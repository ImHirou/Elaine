// Chunk: one 16x16 column of sections plus everything derived from its blocks that the client is
// sent (heightmaps, light) and the cached wire frame.
//
// A chunk is one fixed-size block: every vanilla dimension is at most 24 sections tall, so arrays
// are sized for that instead of chasing a per-dimension layout through pointers. The generated
// dimension types are checked against these limits at compile time (world/dimension.cpp).
#pragma once

#include "world/bits.hpp"
#include "world/pool.hpp"
#include "world/section.hpp"

namespace world {

inline constexpr u32 kMaxSections = 24;
inline constexpr u32 kMaxLightSections = kMaxSections + 2;  // light also exists one section below and above the world
// ceillog2(height + 1) for every height from 256 to 510: the width vanilla's Heightmap packs with.
inline constexpr u32 kHeightmapBits = 9;
inline constexpr u32 kHeightmapWords = words_for(256, kHeightmapBits);  // 37

// 256 column values in vanilla's SimpleBitStorage layout - the layout heightmaps have on the wire,
// which is also the densest one, so the chunk encoder only byte-swaps. Index = x + z*16.
struct ColumnMap {
    u64 w[kHeightmapWords];
    ALWAYS_INLINE u32 get(u32 i) const { return words_get(w, kHeightmapBits, i); }
    ALWAYS_INLINE void set(u32 i, u32 v) { words_set(w, kHeightmapBits, i, v); }
    void fill(u32 v) {
        u64 word = 0;
        for (u32 k = 0; k < 64 / kHeightmapBits; ++k) word |= static_cast<u64>(v) << (k * kHeightmapBits);
        for (u64& x : w) x = word;
        // The last word holds only 256 - 36*7 = 4 entries; vanilla leaves the rest zero.
        w[kHeightmapWords - 1] &= (1ull << ((256 % (64 / kHeightmapBits)) * kHeightmapBits)) - 1;
    }
};

// Heightmap types the client is sent (Heightmap.Types with Usage.CLIENT), by wire id.
enum : u32 { kWorldSurface = 1, kMotionBlocking = 4, kMotionBlockingNoLeaves = 5 };

// One light layer of one section: vanilla's DataLayer, or its absence.
//   0        the section does not store light (none of the 27 sections around it has a block)
//   1..16    no array, every nibble is v-1                       (DataLayer with data == null)
//   17..32   every nibble is v-17, but vanilla would hold an array here; it was folded away
//            to save 2 KiB. Differs from the form above only when the value is 0: an allocated
//            all-zero layer goes on the wire as 2048 zero bytes, a never-written one as a mask bit.
//   else     pointer to 2048 bytes, nibble index = y<<8 | z<<4 | x, low nibble first
struct LightLayer {
    static constexpr u32 kBytes = 2048;
    uintptr_t v = 0;

    bool present() const { return v != 0; }
    bool has_array() const { return v > 32; }
    u8* array() const { return reinterpret_cast<u8*>(v); }
    bool vanilla_empty() const { return v == 1; }                       // DataLayer.isEmpty()
    u32 uniform_value() const { return static_cast<u32>(v - 1) & 15; }  // valid when present() && !has_array()

    ALWAYS_INLINE u32 get(u32 i) const {
        if (has_array()) return (array()[i >> 1] >> ((i & 1) * 4)) & 15;
        return uniform_value();
    }
    // DataLayer.set(): materialises the array on first write.
    ALWAYS_INLINE void set(BlockPool& pool, u32 i, u32 level) {
        if (UNLIKELY(!has_array())) materialise(pool);
        u8& b = array()[i >> 1];
        const u32 sh = (i & 1) * 4;
        b = static_cast<u8>((b & ~(15u << sh)) | (level << sh));
    }
    void materialise(BlockPool& pool) {
        const u32 val = present() ? uniform_value() : 0;
        u8* a = static_cast<u8*>(pool.alloc());
        std::memset(a, static_cast<int>(val | val << 4), kBytes);
        v = reinterpret_cast<uintptr_t>(a);
    }
    void set_uniform(BlockPool& pool, u32 level) {  // DataLayer.fill() / new DataLayer(level)
        drop(pool);
        v = 1 + level;
    }
    void drop(BlockPool& pool) {
        if (has_array()) pool.free(array());
        v = 0;
    }
    // Fold an array whose nibbles are all equal back into a tag. Purely a memory optimisation.
    void fold(BlockPool& pool);
};

// How a dimension is laid out vertically: what everything that works on a chunk outside the world
// (a generator, a loader, the light engine) has to know of it.
struct Layout {
    i32 min_y = 0;
    i32 min_section = 0;  // min_y >> 4
    u32 sections = 0;
    bool has_skylight = true;
};

// The biomes of one section when they are not all one: vanilla keeps a biome per 4x4x4 cell
// (QuartPos), 64 to a section, index = y<<4 | z<<2 | x - the layout of its PalettedContainer. The
// palette is in order of first appearance, which is the order vanilla writes one in, to disk and to
// a client. Nothing here changes biomes yet; they come with a chunk that was imported.
struct BiomeSection {
    u8 count = 0;  // palette entries, 2..64; 0: the section has the one biome Chunk::biome names
    u8 index[64] = {};
    u16 palette[64] = {};  // network ids (worldgen/biome)
};

// Light layers that were loaded for a chunk and are waiting for their section to store light:
// LayerLightSectionStorage.queuedSections. A section stores light while a section around it holds
// a block; the one around it may be in a chunk that has not been loaded yet.
struct LightQueue;

// A block entity as its chunk knows it: where it is, of which type, and - for whoever keeps what
// it holds (the server's chests and furnaces) - a handle to that. The chunk packet lists them:
// a client only makes the block entity of a chest (which is what draws it) when told to.
struct BlockEntityRef {
    u8 xz;    // x << 4 | z inside the chunk: ClientboundLevelChunkPacketData.BlockEntityInfo.packedXZ
    u8 type;  // block_entity_type id
    i16 y;
    u32 data;  // the owner's index
};
static_assert(sizeof(BlockEntityRef) == 8);

// The scheduled ticks of a chunk (world/ticks.hpp).
struct ChunkTicks;

// What the random ticks of one section have to do (see Dimension, "random ticks"): of its cells
// that tick at random, how many may act whatever is around them, how many are about to die of
// what stands on them - and how many of its cells are soil something could spread to.
struct RandomCounts {
    u16 always = 0, dying = 0, targets = 0;
};

// Chunk::keep: what came with an imported chunk and nobody here reads - the rest of its NBT, its
// entities - kept as bytes so that nothing of a world is lost on the way in (world/anvil.hpp).
struct KeptData {
    u32 len = 0;
    u8* bytes = nullptr;
};

enum class ChunkStatus : u8 {
    Generated,   // blocks, heightmaps and sky sources are final; unknown to the light engine
    LightReady,  // sections registered with the light engine (vanilla INITIALIZE_LIGHT)
    Lit,         // its own light sources are propagated (vanilla LIGHT)
};

struct Chunk {
    i32 x = 0, z = 0;
    ChunkStatus status = ChunkStatus::Generated;
    bool sky_on = false;  // vanilla "light enabled" for the column, per layer
    bool block_on = false;
    bool modified = false;  // differs from what the generator produced (kept with the chunk when it is saved)
    // This chunk and its eight neighbours are lit: it may be sent, edited and broadcast. Until then
    // light changes in it are not recorded (ChunkHolder does the same for non-ticking chunks).
    bool ticking = false;
    bool frame_valid = false;
    bool light_listed = false;  // queued in the light engine's changed list
    u8 sections_count = 0;
    u8 light_count = 0;
    // Differs from what is on disk (ChunkAccess.isUnsaved): a block, a light value, a block entity
    // changed - or it has never been saved. What it holds that the game owns (containers, entities)
    // is the game's to mark.
    bool dirty = false;
    // It was lit when it was saved: its light came with it, and lighting it is not to be done again
    // (ChunkStatusTasks.isLighted).
    bool loaded_lit = false;
    bool unloading = false;       // picked to leave the world in this pass (Dimension::unload)
    bool saved_entities = false;  // its copy on disk holds entities of the game's: one without them has to replace it
    u32 frame_len = 0;
    u8* frame = nullptr;  // cached level_chunk_with_light packet, framed and compressed
    // Bumped whenever the frame goes stale; a frame that a worker compressed is taken only if it
    // was made of this version. frame_job: the version a worker is at work on, 0 for none.
    u64 frame_version = 0, frame_job = 0;
    u64 saved_tick = 0;              // when it was last written out, or loaded
    u64 idle_tick = 0;               // when it was first found in nobody's reach; 0 while someone wants it
    BiomeSection* biomes = nullptr;  // [sections_count], or null while every section has one biome
    LightQueue* queued = nullptr;
    KeptData keep;
    // The chunk's block entities, in no order. A handful at most in nearly every chunk that has
    // any: finding one by position is a scan of this array.
    BlockEntityRef* block_entities = nullptr;
    u16 block_entity_count = 0, block_entity_cap = 0;
    // Light sections whose data changed since the last broadcast (bit = light section index).
    u32 sky_changed = 0, block_changed = 0;
    // What a section holds, a bit per section: anything but air; and - these three are set when
    // such a block is put there and never cleared - a block whose collision shape reaches out of
    // its cell, one that acts on an entity in it or on it, one that breaks a fall.
    // (Dimension::holds: what collision queries and entity physics ask before reading blocks.)
    u32 filled_sections = 0, large_sections = 0, entity_sections = 0, fall_sections = 0;
    // ---- block ticks. Which chunks are simulated is the game's to say, by the players' simulation
    // distance: how many of them have this chunk within it (random ticks: ChunkLevel.isEntityTicking),
    // and how many within one chunk more (scheduled ticks: isBlockTicking).
    u16 sim_refs = 0, block_refs = 0;
    bool block_ticking() const { return ticking && block_refs != 0; }
    ChunkTicks* block_ticks = nullptr;  // its scheduled ticks; null while it never had one
    // A bit per section: it holds cells that tick at random (Section::ticking); cells of them that
    // may always act; cells that are dying; soil that can be spread to - and, worked out from
    // those of this chunk and the eight around it, the sections whose random ticks can do
    // anything at all. Only those are ticked (Dimension::random_chunks).
    u32 ticking_sections = 0, always_sections = 0, dying_sections = 0, target_sections = 0, active_sections = 0;
    static constexpr u32 kUnlisted = ~0u;
    u32 random_slot = kUnlisted;  // its place among the chunks with random ticks to run
    RandomCounts random[kMaxSections];
    // Vanilla SkyLightSectionStorage.topSections for this column: highest light-storing section
    // y + 1, or kNoSkyTop.
    i32 sky_top = kNoSkyTop;
    static constexpr i32 kNoSkyTop = INT32_MIN;

    // Per light section: vanilla LayerLightSectionStorage.SectionState (neighbour count in the low
    // 5 bits, kHasData when the section itself has blocks), plus our pending-removal mark.
    // kRetain goes with kPendingRemoval: the section stops storing light because a chunk beside it
    // left the world, not because a block went - its layers are kept (queued) for when that chunk is back.
    static constexpr u8 kNeighbourMask = 31, kHasData = 32, kPendingRemoval = 64, kRetain = 128;
    static constexpr u8 kMarks = kPendingRemoval | kRetain;
    u8 light_state[kMaxLightSections] = {};
    LightLayer sky[kMaxLightSections];
    LightLayer block[kMaxLightSections];

    Section sections[kMaxSections];
    u16 biome[kMaxSections] = {};  // one biome per section for now (network id in worldgen/biome)

    ColumnMap world_surface, motion_blocking, motion_blocking_no_leaves;
    ColumnMap sky_sources;  // lowest y that still has direct sky light, stored minus (min_y - 1)

    // The layer a client is sent and a file is written for a light section: the stored one, or the
    // one that waits to be (LayerLightSectionStorage.getDataLayerData).
    inline const LightLayer& sky_layer(u32 li) const;
    inline const LightLayer& block_layer(u32 li) const;
};

struct LightQueue {
    LightLayer sky[kMaxLightSections];
    LightLayer block[kMaxLightSections];
};
inline const LightLayer& Chunk::sky_layer(u32 li) const { return sky[li].present() || !queued ? sky[li] : queued->sky[li]; }
inline const LightLayer& Chunk::block_layer(u32 li) const {
    return block[li].present() || !queued ? block[li] : queued->block[li];
}

// ---- what a chunk's blocks say about it (world/chunk.cpp). Pure functions of the chunk: a worker
// runs them on a chunk it has just read or generated.
// Heightmap.primeHeightmaps for the three heightmaps a client is sent.
void prime_heightmaps(Chunk& c);
// Chunk::filled_sections and the three masks beside it. Returns bit 0 if any section holds a shape
// that reaches out of its cell, bit 1 if any holds a block that acts on entities.
u32 fill_section_masks(Chunk& c);
// ChunkSkyLightSources.fillFrom.
void fill_sky_sources(Chunk& c, const Layout& l);
// Chunk::random and the four masks that say which of its counts are not zero (not
// active_sections: that takes the chunks around it, and is the dimension's to work out).
void count_random_ticks(Chunk& c);
// All of the above, and the non-air counts are the sections' own: after this a chunk whose
// sections, biomes and light are in place is ready to be adopted.
u32 derive(Chunk& c, const Layout& l);

}  // namespace world
