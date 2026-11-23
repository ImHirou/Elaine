// SPDX-License-Identifier: GPL-3.0-or-later
// A chunk's terrain: what vanilla's `terrain` chunk status makes of a chunk
// (net.minecraft.world.level.levelgen.NoiseBasedChunkGenerator.buildTerrain of the 26.3 jar) - the
// shape of the ground out of the density and the aquifers (doFill), then the surface and the caves.
//
// Three things take part, as in vanilla:
//   Overworld   what a world has one of: its noises and the random sources its parts draw from
//               (RandomState)
//   NoiseChunk  what one chunk's making has: a context for the density functions, with its
//               caches, and the chunk's aquifer (NoiseChunk)
//   Terrain     the chunk as it is while it is made: blocks, the two heightmaps generation goes
//               by, the places whose fluid is to be let flow (a ProtoChunk)
// All of it is a function of the seed and the chunk's position; nothing of another chunk is read.
#pragma once

#include <vector>

#include "worldgen/aquifer.hpp"

namespace worldgen {

namespace settings = proto::gen::worldgen;  // (kMinY, kHeight, kSeaLevel, the default block and fluid)

class Overworld {
  public:
    explicit Overworld(i64 seed);

    i64 seed() const { return seed_; }
    const Noises& noises() const { return noises_; }
    // RandomState.getOrCreateRandomFactory("minecraft:aquifer"): where a cell's point lies.
    const XoroshiroPositional& aquifer_random() const { return aquifer_random_; }

  private:
    i64 seed_;
    Noises noises_;
    XoroshiroPositional aquifer_random_;
};

struct Terrain {
    static constexpr u32 kSections = static_cast<u32>(settings::kHeight) / 16;

    i32 chunk_x = 0, chunk_z = 0;
    // Block states by section, and in one by y << 8 | z << 4 | x (what world::Section::assign takes).
    u16 blocks[kSections * 4096];
    // Heightmap.Types.WORLD_SURFACE_WG and OCEAN_FLOOR_WG: the first free y of a column, by
    // x + z * 16 - kept as blocks are set, the way Heightmap.update keeps them.
    i32 world_surface_wg[256], ocean_floor_wg[256];
    // ProtoChunk.markPosForPostProcessing: the marks of each section in the order of marking,
    // x | y << 4 | z << 8 within the section. (The game goes through them section by section.)
    std::vector<u16> post_process[kSections];

    void begin(i32 cx, i32 cz);
    u16 block(i32 x, i32 y, i32 z) const { return blocks[index(x, y, z)]; }
    // LevelChunkSection.setBlockState and the heightmaps' update (x and z within the chunk).
    void set_block(i32 x, i32 y, i32 z, u16 state);
    void mark_post_process(i32 x, i32 y, i32 z);

  private:
    static u32 index(i32 x, i32 y, i32 z) { return static_cast<u32>(((y - settings::kMinY) << 8) | (z << 4) | x); }
    void update_heightmap(i32* map, u8 bit, i32 x, i32 y, i32 z, u16 state);
};

class NoiseChunk {
  public:
    // `sampler` is begun anew: a chunk's context has no memory of another's.
    NoiseChunk(const Overworld& world, Sampler& sampler, i32 chunk_x, i32 chunk_z);

    const Volume& volume() const { return volume_; }
    Sampler& sampler() { return sampler_; }
    Aquifer& aquifer() { return aquifer_; }

  private:
    Sampler& sampler_;
    Volume volume_;
    Aquifer aquifer_;
};

// NoiseBasedChunkGenerator.doFill: the density of every block of the chunk, and for each what
// the aquifer says of it - the default block where it says nothing.
void fill_from_noise(NoiseChunk& chunk, Terrain& terrain);

}  // namespace worldgen
