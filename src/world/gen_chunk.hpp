// SPDX-License-Identifier: GPL-3.0-or-later
// A chunk while it is generated: what vanilla's ProtoChunk is between the `terrain` status and the
// day it becomes a chunk of the world.
//
// A generator that makes a chunk in one go fills a LoadedChunk and is done. One whose chunks are
// decorated - a tree of this chunk has leaves in the next - makes a chunk in stages: its terrain
// first, then whatever the features of the chunks around put into it, then its own. Between the
// stages the chunk is this: blocks, and beside them what the next stage reads and what a finished
// chunk hands the game - none of which a LoadedChunk has a place for.
//
// A GenChunk is owned by one thing at a time - the worker that works on it, or whatever keeps it
// until the next stage - and travels by pointer; its sections live in an arena of its own.
#pragma once

#include <vector>

#include "world/chunk_io.hpp"
#include "world/ticks.hpp"

namespace world {

// Heightmap.Types, in vanilla's order (the ids of the three a client is sent are these numbers):
// the two that generation goes by until a chunk's terrain is made, which stay as the terrain left
// them, and the four a chunk keeps, which follow every block that is set afterwards.
namespace heightmap {
enum : u32 { kWorldSurfaceWg, kWorldSurface, kOceanFloorWg, kOceanFloor, kMotionBlocking, kMotionBlockingNoLeaves, kCount };
}

// A block entity generation made (ProtoChunk.blockEntities, pendingBlockEntities). Most are only
// known to be there - WorldGenRegion.setBlock leaves {id: "DUMMY"} for a block that has one, and
// the game makes it when the chunk arrives; a feature that fills one in (a spawner's mob, a
// chest's loot table) leaves its tag.
struct GenBlockEntity {
    u32 at;        // (y - min_y) << 8 | z << 4 | x
    u32 tag, len;  // in GenChunk::tags: one nameless NBT compound; len 0: nothing is known of it
};

struct GenChunk {
    GenChunk() = default;
    GenChunk(const GenChunk&) = delete;
    GenChunk& operator=(const GenChunk&) = delete;

    ChunkArena arena;
    SectionAllocator alloc{arena};
    i32 x = 0, z = 0;
    u8 sections_count = 0;
    u8 version = 0;          // how many passes of decoration it has been through
    bool decorated = false;  // its own features have run (ChunkStatus.FEATURES)
    Section sections[kMaxSections];
    // The biome of every cell (4x4x4 blocks), by section, and in one by y << 4 | z << 2 | x.
    u16 cells[kMaxSections * 64];
    // The first free y of every column for each heightmap, counted from the bottom of the world;
    // by x + z * 16.
    i16 height[heightmap::kCount][256];
    // ProtoChunk.postProcessing: section << 12 | x | y << 4 | z << 8, in the order of marking.
    // (Vanilla has a list a section: the order *within* a section is what is kept.)
    std::vector<u32> marks;
    // ProtoChunkTicks: one a position and type, in the order they were asked for; all due at once.
    std::vector<SavedTick> block_ticks, fluid_ticks;
    std::vector<GenBlockEntity> block_entities;
    std::vector<u8> tags;
};

// What `out` is made of when generation is over: the chunk's blocks (its arena changes hands - `g`
// is empty afterwards), its biomes as a section's palette, what is left to be done when it first
// ticks, its scheduled ticks. `out.chunk` has its place and its height (load_chunk); what follows
// from the blocks is derive()'s, as for a chunk that was read.
void finish_chunk(GenChunk& g, LoadedChunk& out);

}  // namespace world
