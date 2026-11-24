// SPDX-License-Identifier: GPL-3.0-or-later
// The overworld's generator: a chunk as vanilla's NoiseBasedChunkGenerator makes it from the
// world's seed and the chunk's position alone - its biomes (the `biomes` status) and its terrain
// (the `terrain` status). Called by workers, several chunks at once; what a call works with is
// its thread's own.
#pragma once

#include "world/generator.hpp"
#include "worldgen/terrain.hpp"

namespace worldgen {

class OverworldGenerator final : public world::ChunkGenerator {
  public:
    explicit OverworldGenerator(i64 seed) : world_(seed) {}

    // The chunk as the `biomes` and `terrain` statuses leave it, with the heightmaps a chunk keeps
    // from then on (Heightmap.primeHeightmaps, at the end of buildTerrain).
    void make_base(i32 cx, i32 cz, world::GenChunk& out) override;
    // A chunk of terrain alone, in one go: make_base, handed on as it is.
    bool generate_whole(world::LoadedChunk& out) override;
    i32 sea_level() const override { return settings::kSeaLevel; }
    i32 spawn_height() const override { return 64; }  // ChunkGenerator.getSpawnHeight
    bool is_flat() const override { return false; }
    const Overworld& world() const { return world_; }

  private:
    Overworld world_;
};

// MinecraftServer.setInitialSpawn for a world of this generator: the chunk the climate picks for
// the world to begin in (NoiseSpawnFinder), then the first block one can stand on there or in the
// chunks around, in a spiral five chunks out (PlayerSpawnFinder.getSpawnPosInChunk) - and where
// there is none, the middle of that chunk at ChunkGenerator.getSpawnHeight. It makes the terrain
// of the chunks it looks at: asked once, of a world that is new. (Vanilla looks at chunks that
// have their trees, too: until the features are here the two need not agree.)
void find_world_spawn(const Overworld& world, i32 spawn[3]);

}  // namespace worldgen
