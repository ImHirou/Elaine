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
    bool modified = false;  // differs from what the generator produced
    // This chunk and its eight neighbours are lit: it may be sent, edited and broadcast. Until then
    // light changes in it are not recorded (ChunkHolder does the same for non-ticking chunks).
    bool ticking = false;
    bool frame_valid = false;
    bool light_listed = false;  // queued in the light engine's changed list
    u8 sections_count = 0;
    u8 light_count = 0;
    u32 frame_len = 0;
    u8* frame = nullptr;  // cached level_chunk_with_light packet, framed and compressed
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
    // Vanilla SkyLightSectionStorage.topSections for this column: highest light-storing section
    // y + 1, or kNoSkyTop.
    i32 sky_top = kNoSkyTop;
    static constexpr i32 kNoSkyTop = INT32_MIN;

    // Per light section: vanilla LayerLightSectionStorage.SectionState (neighbour count in the low
    // 5 bits, kHasData when the section itself has blocks), plus our pending-removal mark.
    static constexpr u8 kNeighbourMask = 31, kHasData = 32, kPendingRemoval = 64;
    u8 light_state[kMaxLightSections] = {};
    LightLayer sky[kMaxLightSections];
    LightLayer block[kMaxLightSections];

    Section sections[kMaxSections];
    u16 biome[kMaxSections] = {};  // one biome per section for now (network id in worldgen/biome)

    ColumnMap world_surface, motion_blocking, motion_blocking_no_leaves;
    ColumnMap sky_sources;  // lowest y that still has direct sky light, stored minus (min_y - 1)
};

}  // namespace world
