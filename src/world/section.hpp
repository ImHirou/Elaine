// Section: the 16x16x16 block container - the structure everything per-block ends up reading.
//
// In memory (ours to choose, see AGENTS.md "Bit packing"):
//   bits == 0      one state everywhere (`single`), no allocation. Most sections of most worlds.
//   bits == 1..8   a palette of up to 2^bits states and 4096 dense `bits`-wide indices, with the
//                  EXACT width for the palette size (3 states = 2 bits, 20 states = 5 bits).
//                  One block from the pool: [palette][indices][reference counts].
//   bits == 16     more than 256 distinct states: 4096 raw state ids.
// On the wire vanilla wants 0, 4..8 or the registry width, in a padded word layout; the chunk
// encoder converts (world/chunk_codec.cpp). Entry order is vanilla's: index = y<<8 | z<<4 | x, so a
// horizontal layer is 256 consecutive entries.
//
// Each palette slot is reference counted. That makes non-air/fluid counters, "is this section one
// state again" and slot reuse O(1) per write instead of a rescan.
//
// A Section is a plain 32-byte value owned by its chunk; the pool it allocates from is passed in.
#pragma once

#include <bit>

#include "world/bits.hpp"
#include "world/block.hpp"
#include "world/pool.hpp"

namespace world {

// Where sections get their blocks. The world's own allocator is a pool per size, owned by the
// tick thread. A worker that builds a chunk outside the world has an ChunkArena instead: its sections
// are copied into the pools when the chunk is adopted (Section::copy_from), and the arena goes.
class SectionAllocator {
  public:
    SectionAllocator();
    explicit SectionAllocator(ChunkArena& arena) : arena_(&arena) {}
    ALWAYS_INLINE void* alloc(u32 bits) {
        if (UNLIKELY(arena_ != nullptr)) return arena_->alloc(block_size(bits));
        return pools_[pool_of(bits)].alloc();
    }
    ALWAYS_INLINE void free(u32 bits, void* p) {
        if (LIKELY(arena_ == nullptr)) pools_[pool_of(bits)].free(p);
    }
    usize reserved_bytes() const;
    usize live_blocks() const;

  private:
    static u32 pool_of(u32 bits) { return bits <= 8 ? bits - 1 : 8; }
    static u32 block_size(u32 bits);
    BlockPool pools_[9];
    ChunkArena* arena_ = nullptr;
};

struct Section {
    static constexpr u32 kVolume = kSectionVolume;
    static constexpr u32 kMaxPaletteBits = 8;
    static constexpr u32 kDirectBits = 16;
    static constexpr u32 kNoSlot = ~0u;
    static constexpr u16 kNoState = 0xFFFF;  // padding in palettes: never equal to a real state

    u8* data = nullptr;      // 1..8: dense indices; 16: u16 states; 0: null
    u16* palette = nullptr;  // 1..8: palette_cap(bits) states, padded with kNoState (= start of the block)
    u16 single = kAir;       // the state when bits == 0
    u16 non_air = 0;         // vanilla nonEmptyBlockCount
    u16 fluids = 0;          // vanilla fluidCount
    u16 palette_len = 0;     // slots handed out, alive or dead
    u16 dead = 0;            // of those, how many have no blocks (reused before the palette grows)
    u16 ticking = 0;         // blocks that tick at random here (state::kRandomTick): vanilla tickingBlockCount, of what is ported
    u8 bits = 0;

    ALWAYS_INLINE static constexpr u32 index(u32 x, u32 y, u32 z) { return (y << 8) | (z << 4) | x; }
    // Palette storage is padded to whole 16-entry vectors for the search in find().
    static constexpr u32 palette_cap(u32 bits) { return std::max<u32>(16, 1u << bits); }
    // Width for n >= 2 distinct states.
    static constexpr u32 bits_for(u32 n) { return n <= 256 ? static_cast<u32>(std::bit_width(n - 1)) : kDirectBits; }
    static constexpr u32 block_bytes(u32 bits) {
        return bits == kDirectBits ? kVolume * 2 : 4 * palette_cap(bits) + packed_bytes(bits) + kPackedSlack;
    }

    bool uniform() const { return bits == 0; }
    bool only_air() const { return non_air == 0; }
    // Blocks per palette slot (bits 1..8); stored behind the indices, written only by set().
    u16* refs() const { return reinterpret_cast<u16*>(data + packed_bytes(bits) + kPackedSlack); }

    ALWAYS_INLINE BlockState get(u32 i) const {
        if (bits == 0) return single;
        if (UNLIKELY(bits == kDirectBits)) return load_le<u16>(data + 2 * i);
        return palette[packed_get(data, bits, i)];
    }
    ALWAYS_INLINE BlockState get(u32 x, u32 y, u32 z) const { return get(index(x, y, z)); }

    // Returns the state that was there (== s when nothing changed).
    BlockState set(SectionAllocator& a, u32 i, BlockState s);

    void fill(SectionAllocator& a, BlockState s);                // every block = s
    void assign(SectionAllocator& a, const BlockState* states);  // all 4096 at once (generators, loaders)
    void copy_from(SectionAllocator& a, const Section& o);
    // A section as it was saved: 4096 dense `bits`-wide indices into `pal` (bits 1..8), or raw
    // states (bits 16, `pal` unused). False - and the section is left as it was - when that is not
    // a section: a width that does not exist, an index past the palette, a state that is none.
    bool load(SectionAllocator& a, u32 new_bits, const u16* pal, u32 pal_len, const u8* packed);
    void unpack(BlockState* RESTRICT out) const;  // all 4096 states
    // Drop dead palette slots and shrink to the narrowest width that still fits. Never changes what
    // get() returns; cheap to call when nothing is to be gained.
    void compact(SectionAllocator& a);
    void release(SectionAllocator& a);  // free the block; the section becomes uniform air

    // Slot holding state s, or kNoSlot. Valid for bits 1..8.
    ALWAYS_INLINE u32 find(BlockState s) const {
        const __m256i key = _mm256_set1_epi16(static_cast<short>(s));
        for (u32 i = 0; i < palette_len; i += 16) {
            const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(palette + i));
            const u32 m = static_cast<u32>(_mm256_movemask_epi8(_mm256_cmpeq_epi16(v, key)));
            if (m) return i + (static_cast<u32>(std::countr_zero(m)) >> 1);
        }
        return kNoSlot;
    }

  private:
    void attach(SectionAllocator& a, u32 new_bits);  // allocate a block for new_bits and point data/palette into it
    void detach(SectionAllocator& a);
    u32 add_slot(SectionAllocator& a, BlockState s);
    void grow(SectionAllocator& a);
    void to_direct(SectionAllocator& a);
    void recount();
};
static_assert(sizeof(Section) == 32);

}  // namespace world
