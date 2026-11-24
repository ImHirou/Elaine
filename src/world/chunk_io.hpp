// SPDX-License-Identifier: GPL-3.0-or-later
// A chunk as bytes: what a region file holds for it (deflated there, world/region.hpp), and the
// chunk outside the world that a worker makes of those bytes.
//
//   u8  version (4; a chunk of version 3 is one without the last part, of version 2 without the
//       last two, of version 1 without the last three)
//   u8  flags     1 lit: its light is final, lighting it again is not needed
//                 2 modified: something changed it since the generator made it
//                 4 terrain: sections and light follow. Without: nothing was stored of its blocks
//                   (an import that had only entities for it) - the generator makes them
//   i32 x, z
//   u8  sections, light sections
//   per section       u8 bits: 0 (u16 state) | 1..8 (u16 n, n x u16 palette, 512 x bits bytes of
//                     indices) | 16 (4096 x u16); the same dense layout as in memory
//                     u16 biome, u8 n: 0, or n x u16 palette + 64 index bytes
//   per light section sky, then block: u8 0 absent | 1 no array (u8 value) | 2 folded (u8 value)
//                     | 3 (2048 bytes). What is saved is the layer as it is, empty ones included:
//                     a chunk comes back with the light it left with
//   u32 n + bytes     block entities: the game's records (server/persist.cpp)
//   u32 n + bytes     entities: the same
//   u32 n + bytes     what an import kept and nobody reads (world/anvil.hpp)
//   u32 n + bytes     its scheduled ticks (world/ticks.hpp), as delays from the moment it is saved
//   u32 n + bytes     the scheduled ticks of its fluids: the same, with a fluid's id for a block's
//   u32 n + bytes     what generation left to do when the chunk first ticks (Chunk::post_process):
//                     u32 section << 12 | x | y << 4 | z << 8 a block. Not a part the caller hands in:
//                     it is the chunk's, as what an import kept is
// Little-endian. State and biome ids are this version's: a world names the data version it was
// saved by, and another version has to convert it.
//
// Heightmaps, sky light sources, the non-air counts and palette reference counts are not stored:
// they follow from the blocks, and the worker that reads a chunk works them out.
#pragma once

#include <span>
#include <string>

#include "base/buf.hpp"
#include "world/chunk.hpp"
#include "world/generator.hpp"

namespace world {

// What is kept with a chunk as bytes of somebody else's making: the game's two parts
// (server/persist.cpp), and the chunk's scheduled ticks, of its blocks and of its fluids - which
// Dimension::save writes itself.
struct ChunkParts {
    std::span<const u8> block_entities = {}, entities = {}, ticks = {}, fluid_ticks = {};
};

// Appends the chunk to `out`. The chunk is only read.
void encode_chunk(const Chunk& c, const ChunkParts& parts, base::ByteBuf& out);

// A chunk of which nothing is stored but Chunk::keep (an import that had only entities for
// it): its blocks will be the generator's.
void encode_kept(const Chunk& c, base::ByteBuf& out);

// A chunk outside the world. Its sections, its biomes, the light layers it was saved with (in
// chunk.sky / chunk.block: what was loaded, not what is stored - Dimension::adopt queues them) and
// the parts all live in the arena; adopting it copies them into the world's pools.
struct LoadedChunk {
    ChunkArena arena;
    SectionAllocator alloc{arena};
    Chunk chunk;
    ChunkParts parts;
    bool fresh = false;   // made by the generator just now: it has never been saved
    bool unmade = false;  // its blocks are still to come from a generator that makes chunks in stages
    u32 traits = 0;       // derive(): bit 0 large collision shapes, bit 1 blocks that act on entities
};

// Make `out` the chunk at (cx, cz) that `blob` holds - or, with an empty blob, the one the
// generator makes - and derive what follows from its blocks. False (with `err`) if the blob is
// not a chunk of this layout; `out` is then not to be used.
//
// A generator that makes chunks in stages (ChunkGenerator::staged) is not run from here. Where
// the blocks are its to make, `out` has everything but them and says so (`unmade`): the caller has
// the chunk made (world/gen_pipeline.hpp) and hands it to made_chunk.
bool load_chunk(std::span<const u8> blob, i32 cx, i32 cz, const Layout& l, ChunkGenerator& gen, LoadedChunk& out,
                std::string& err);
// The blocks of a chunk load_chunk left unmade - what the generator's stages made of it, which is
// empty afterwards - and what follows from them.
void made_chunk(GenChunk& made, const Layout& l, LoadedChunk& out);

}  // namespace world
