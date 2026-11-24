// SPDX-License-Identifier: GPL-3.0-or-later
// The overworld's generator: a chunk as vanilla's NoiseBasedChunkGenerator makes it from the
// world's seed and the chunk's position alone - its biomes (the `biomes` status) and its terrain
// (the `terrain` status). Called by workers, several chunks at once; what a call works with is
// its thread's own.
#pragma once

#include "world/block.hpp"
#include "world/generator.hpp"
#include "worldgen/terrain.hpp"

namespace worldgen {

class OverworldGenerator final : public world::ChunkGenerator {
  public:
    explicit OverworldGenerator(i64 seed) : world_(seed) {}

    // The chunk as the `biomes` and `terrain` statuses leave it, with the heightmaps a chunk keeps
    // from then on (Heightmap.primeHeightmaps, at the end of buildTerrain).
    bool staged() const override { return true; }
    void make_base(i32 cx, i32 cz, world::GenChunk& out) override;
    // ChunkGenerator.applyBiomeDecoration for the four chunks in the middle of the area.
    void decorate(world::GenChunk* const* area, i32 x0, i32 z0, u32 pass) override;
    // A chunk of terrain alone, in one go: make_base, handed on as it is (tests and tools; a world's
    // chunks come by the stages above).
    bool generate_whole(world::LoadedChunk& out) override;
    i32 sea_level() const override { return settings::kSeaLevel; }
    i32 spawn_height() const override { return 64; }  // ChunkGenerator.getSpawnHeight
    // NoiseSpawnFinder: the block the climate picks for the world to begin around.
    bool spawn_origin(i32& x, i32& z) override;
    bool is_flat() const override { return false; }
    const Overworld& world() const { return world_; }

  private:
    Overworld world_;
};

// PlayerSpawnFinder.getSpawnPosInChunk, getLevelRespawnPos: the first column of a chunk - x, then
// z - with ground on top of it that is no water's bed, and above that ground a block to stand on.
// `block(x, y, z)`: the chunk's blocks, by x and z within it. True: `spawn` is that place.
template <class BlockAt>
bool spawn_in_chunk(i32 chunk_x, i32 chunk_z, BlockAt&& block, i32 spawn[3]) {
    namespace wg = proto::gen::worldgen;
    const i32 min_y = settings::kMinY, top_of_world = settings::kMinY + settings::kHeight - 1;
    for (i32 x = 0; x < 16; ++x) {
        for (i32 z = 0; z < 16; ++z) {
            // ChunkAccess.getHeight of three heightmaps: the highest block that counts for each.
            const auto height = [&](u8 bit) {
                i32 y = top_of_world;
                while (y >= min_y && !(wg::kHeightmaps[block(x, y, z)] & bit)) --y;
                return y;
            };
            const i32 top = height(wg::kMotionBlocking);
            if (top < min_y) continue;
            const i32 surface = height(wg::kWorldSurface);
            if (surface <= top && surface > height(wg::kOceanFloor)) continue;
            for (i32 y = top + 1; y >= min_y; --y) {
                const u16 state = y > top_of_world ? world::kAir : block(x, y, z);
                if (world::flags_of(state) & world::state::kHasFluid) break;
                if (!world::collision_face_full(state, 1)) continue;  // (Direction.UP)
                spawn[0] = chunk_x * 16 + x;
                spawn[1] = y + 1;
                spawn[2] = chunk_z * 16 + z;
                return true;
            }
        }
    }
    return false;
}

}  // namespace worldgen
