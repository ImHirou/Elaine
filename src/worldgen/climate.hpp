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
void fill_biomes(Sampler& sampler, i32 chunk_x, i32 chunk_z, i32 min_y, u32 sections, u16* out);

}  // namespace worldgen
