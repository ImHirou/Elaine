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

    bool generate_whole(world::LoadedChunk& out) override;
    i32 sea_level() const override { return settings::kSeaLevel; }
    i32 spawn_height() const override { return 64; }  // ChunkGenerator.getSpawnHeight
    bool is_flat() const override { return false; }
    const Overworld& world() const { return world_; }

  private:
    Overworld world_;
};

}  // namespace worldgen
