// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/gen_chunk.hpp"

#include <cstring>

#include "base/buf.hpp"

namespace world {

namespace {

// A chunk's scheduled ticks as the chunk's bytes hold them, in the arena that goes with the chunk.
std::span<const u8> packed_ticks(const std::vector<SavedTick>& ticks, ChunkArena& arena, base::ByteBuf& scratch) {
    if (ticks.empty()) return {};
    scratch.clear();
    write_saved_ticks(ticks, scratch);
    u8* bytes = static_cast<u8*>(arena.alloc(scratch.len));
    std::memcpy(bytes, scratch.data, scratch.len);
    return {bytes, scratch.len};
}

}  // namespace

void finish_chunk(GenChunk& g, LoadedChunk& out) {
    Chunk& c = out.chunk;
    DCHECK(c.sections_count == g.sections_count);
    // The sections stay where they are: the arena they are in becomes the chunk's, beside
    // whatever the chunk had in its own.
    out.arena.absorb(g.arena);
    for (u32 si = 0; si < g.sections_count; ++si) {
        c.sections[si] = g.sections[si];
        g.sections[si] = Section{};
    }

    // The biomes, a cell at a time; a section's palette in the order vanilla comes to its cells
    // (x, then y, then z).
    c.biomes = nullptr;
    for (u32 si = 0; si < g.sections_count; ++si) {
        BiomeSection made;
        for (u32 x = 0; x < 4; ++x) {
            for (u32 y = 0; y < 4; ++y) {
                for (u32 z = 0; z < 4; ++z) {
                    const u32 cell = y << 4 | z << 2 | x;
                    const u16 id = g.cells[si * 64 + cell];
                    u32 slot = 0;
                    while (slot < made.count && made.palette[slot] != id) ++slot;
                    if (slot == made.count) made.palette[made.count++] = id;
                    made.index[cell] = static_cast<u8>(slot);
                }
            }
        }
        c.biome[si] = made.palette[0];
        if (made.count == 1) continue;  // (a section of one biome is told by Chunk::biome alone)
        if (!c.biomes) c.biomes = new (out.arena.alloc(sizeof(BiomeSection) * kMaxSections)) BiomeSection[kMaxSections];
        c.biomes[si] = made;
    }

    // What is to be looked at when the chunk first ticks (Chunk::post_process): section by section,
    // each in the order its blocks were marked.
    c.post_process = {};
    if (!g.marks.empty()) {
        u32 first[kMaxSections + 1] = {};
        for (const u32 mark : g.marks) ++first[(mark >> 12) + 1];
        for (u32 si = 0; si < kMaxSections; ++si) first[si + 1] += first[si];
        u8* p = static_cast<u8*>(out.arena.alloc(g.marks.size() * 4));
        c.post_process = {static_cast<u32>(g.marks.size() * 4), p};
        for (const u32 mark : g.marks) store_le<u32>(p + 4 * first[mark >> 12]++, mark);
    }

    base::ByteBuf scratch;
    out.parts.ticks = packed_ticks(g.block_ticks, out.arena, scratch);
    out.parts.fluid_ticks = packed_ticks(g.fluid_ticks, out.arena, scratch);
    // (Block entities are the game's records, written by the game: nothing makes one yet.)
    DCHECK(g.block_entities.empty());
}

}  // namespace world
