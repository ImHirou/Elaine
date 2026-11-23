// SPDX-License-Identifier: GPL-3.0-or-later
// The caves and canyons of the carvers: net.minecraft.world.level.levelgen.carver of the 26.3 jar
// and NoiseBasedChunkGenerator.generateCarvers, the last step of the `terrain` status.
//
// A carver begins in a chunk, by chance, and digs from there for up to seven chunks: a room, and
// tunnels that turn as they go and split once. So a chunk asks every chunk within eight of it
// what begins there and follows each tunnel, by the numbers of that chunk's own random source, to
// see where it passes through. What a carver does to the chunk
// is only to mark blocks (CarvingMask); when all have run the marked blocks are given to the
// aquifer, top down in each run of them, which makes them air, water or lava - or leaves them.
// Where grass goes, the dirt under it becomes what the material rule says the top of a column is.
//
// Which carvers run is the same for every biome of the overworld (generated: kCarvers).
#pragma once

#include "worldgen/climate.hpp"
#include "worldgen/terrain.hpp"

namespace worldgen {

// CarvingMask: a bit a block of the chunk, by column (x, then z) and up each from the bottom.
struct CarvingMask {
    static constexpr i32 kMinY = settings::kMinY + 1;  // (the lowest layer is never carved)
    // (Nor the seven under the top of the world: protectedBlocksOnTop, of a chunk that is not being upgraded.)
    static constexpr i32 kMaxY = settings::kMinY + settings::kHeight - 1 - 7;
    static constexpr i32 kColumn = kMaxY - kMinY + 1;
    static constexpr i32 kBits = 256 * kColumn;
    u64 words[(kBits + 63) / 64];

    void clear();
    bool empty() const;
    void carve(i32 x, i32 y, i32 z) {
        const u32 i = static_cast<u32>((y - kMinY) + (z + (x << 4)) * kColumn);
        words[i >> 6] |= 1ull << (i & 63);
    }
};

// What the carvers that begin in the chunks around (chunk_x, chunk_z) mark in it.
void mark_carved(i64 seed, i32 chunk_x, i32 chunk_z, CarvingMask& mask);

// NoiseBasedChunkGenerator.applyCarvingMask
void apply_carving_mask(const Overworld& world, NoiseChunk& chunk, CarverBiomes& biomes, const CarvingMask& mask,
                        Terrain& terrain);

// generateCarvers: both. `mask` is the caller's to keep between chunks.
inline void carve(const Overworld& world, NoiseChunk& chunk, CarverBiomes& biomes, CarvingMask& mask, Terrain& terrain) {
    mark_carved(world.seed(), terrain.chunk_x, terrain.chunk_z, mask);
    if (!mask.empty()) apply_carving_mask(world, chunk, biomes, mask, terrain);
}

}  // namespace worldgen
