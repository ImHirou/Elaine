// SPDX-License-Identifier: GPL-3.0-or-later
// Which biome is where: net.minecraft.world.level.biome.Climate and MultiNoiseBiomeSource of the
// 26.3 jar, and the part of ChunkGenerator that fills a chunk's biomes with them.
//
// A place has a climate - six of the density functions: temperature, humidity, continentalness,
// erosion, depth, weirdness - and the overworld's biomes are some seven and a half thousand boxes
// in that space. The biome of a place is the box nearest to its climate, found in an R-tree
// (Climate.RTree; the generated table is vanilla's own tree).
//
// Where two boxes are equally near - they share their faces, so a climate that lies exactly on
// one is in both - vanilla keeps whichever it found last time: the search begins with the leaf
// the search before it ended at, and only a strictly nearer one replaces it. So the biome of such
// a place depends on which place was asked before, and a chunk's biomes are asked in a fixed
// order: sections from the bottom up, and in a section x, then y, then z. That order is kept here
// (fill_biomes). What vanilla has before a chunk's first place is whatever its thread asked last -
// here, nothing.
#pragma once

#include "worldgen/density.hpp"

namespace worldgen {

// Climate.TargetPoint.toParameterArray: the six parameters in ten-thousandths, and a zero where a
// box has its offset.
struct ClimateTarget {
    i64 v[7];
};

// Climate.quantizeCoord: (long)(coord * 10000.0f), as Java casts.
ALWAYS_INLINE i64 quantize_coord(float coord) {
    const float scaled = coord * 10000.0f;
    if (scaled != scaled) return 0;
    if (scaled >= 9223372036854775807.0f) return INT64_MAX;
    if (scaled <= -9223372036854775808.0f) return INT64_MIN;
    return static_cast<i64>(scaled);
}

// Climate.target
ALWAYS_INLINE ClimateTarget climate_target(float temperature, float humidity, float continentalness, float erosion, float depth,
                                           float weirdness) {
    return {{quantize_coord(temperature), quantize_coord(humidity), quantize_coord(continentalness), quantize_coord(erosion),
             quantize_coord(depth), quantize_coord(weirdness), 0}};
}

inline constexpr i32 kNoLeaf = -1;

// Climate.RTree.search: the leaf nearest to `target`. `last` is the leaf the search before
// found (kNoLeaf: there was none): it stays unless another is strictly nearer.
i32 climate_search(const ClimateTarget& target, i32 last);
// The biome a leaf gives: its network id in worldgen/biome.
u16 climate_biome(i32 leaf);

// ChunkGenerator.doCreateBiomes and ChunkAccess.fillBiomesFromNoise: the biomes of a chunk's
// 4x4x4 cells. `sampler` is a context of its own (vanilla makes one for a chunk's biomes, with
// caches). `out` takes `sections` x 64 biomes, each section's by y << 4 | z << 2 | x. (Vanilla
// sets a section's cells x, then y, then z: the order its palette is in.)
// `tied`, if it is asked for: of each cell, whether a second biome is exactly as near its climate
// as the one it got - where the answer is the search's before it, and in vanilla, for a chunk's
// first such cell, whatever its thread searched last.
void fill_biomes(Sampler& sampler, i32 chunk_x, i32 chunk_z, i32 min_y, u32 sections, u16* out, u8* tied = nullptr);

// The biomes a chunk's terrain is made by: the chunk's own cells and one ring of the cells of the
// chunks around it - what vanilla reads of its neighbours through a WorldGenRegion. The biome of
// a *block* is not its cell's (BiomeManager.getBiome): it is the biome of the nearest of the
// eight cell corners around, each moved a little by the seed, and for a block near the edge that
// is a neighbour's cell.
struct BiomeGrid {
    static constexpr i32 kWidth = 6;  // quart columns -1 .. 4 of the chunk, in x and in z
    i32 qx0 = 0, qy0 = 0, qz0 = 0;    // the quart coordinates of the first cell
    i32 height = 0;                   // cells in y: four a section
    u16 cells[kWidth * kWidth * 96];  // by (x * kWidth + z) * height + y
    u64 present[2] = {0, 0};          // the biomes that are in it, a bit each

    // ChunkAccess.getNoiseBiome: the biome of a quart cell, y clamped into the world.
    u16 at(i32 qx, i32 qy, i32 qz) const {
        const i32 y = qy < qy0 ? 0 : qy >= qy0 + height ? height - 1 : qy - qy0;
        return cells[((qx - qx0) * kWidth + (qz - qz0)) * height + y];
    }
    bool has(u32 biome) const { return (present[biome >> 6] >> (biome & 63)) & 1; }
};

// The grid of a chunk, and - as fill_biomes - the chunk's own cells into `own`. A neighbour's
// cell is what the neighbour's own filling makes of it: where two biomes are equally near there,
// the neighbour is filled as it would be, for its answer depends on the cell before.
void fill_biome_grid(Sampler& sampler, i32 chunk_x, i32 chunk_z, i32 min_y, u32 sections, BiomeGrid& grid, u16* own);

// BiomeManager.getBiome: the cell whose biome a block has. `zoom_seed` is the world's seed as
// clients are told it (obfuscate_seed) - a client zooms the same way.
struct QuartCell {
    i32 x, y, z;
};
QuartCell zoom_cell(i64 zoom_seed, i32 x, i32 y, i32 z);
inline u16 zoomed_biome(const BiomeGrid& grid, i64 zoom_seed, i32 x, i32 y, i32 z) {
    const QuartCell cell = zoom_cell(zoom_seed, x, y, z);
    return grid.at(cell.x, cell.y, cell.z);
}

// NoiseSpawnFinder.findSpawnPosition: the block a world begins at - where, within two thousand
// blocks of 0, 0, the climate is nearest to what the settings ask of a place to begin (land, and
// no river), nearer the middle being better. `sampler` is begun anew.
void spawn_origin(Sampler& sampler, i32& x, i32& z);

// The biomes the carvers go by (BiomeSource.createUncachedResolver): not the cells a chunk was
// filled with, but the climate asked again at the cell, a function at a time and by value - which
// is not, to the last bit, what filled the chunk. `climate` is a context without caches. `last`
// is the leaf the search before ended at: vanilla's is whatever its thread searched last, and a
// chunk here begins with none.
struct CarverBiomes {
    Sampler& climate;
    i32 last = kNoLeaf;

    u16 at(const QuartCell& cell);
};

}  // namespace worldgen
