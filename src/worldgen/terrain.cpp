// SPDX-License-Identifier: GPL-3.0-or-later
#include "worldgen/terrain.hpp"

#include "world/block.hpp"

namespace worldgen {

namespace wg = proto::gen::worldgen;

namespace {

Sampler& reset(Sampler& s) {
    s.reset();
    return s;
}

}  // namespace

Overworld::Overworld(i64 seed)
    : seed_(seed), noises_(seed), aquifer_random_(noises_.random().from_hash_of("minecraft:aquifer").fork_positional()) {}

void Terrain::begin(i32 cx, i32 cz) {
    chunk_x = cx;
    chunk_z = cz;
    std::fill(std::begin(blocks), std::end(blocks), world::kAir);
    std::fill(std::begin(world_surface_wg), std::end(world_surface_wg), settings::kMinY);
    std::fill(std::begin(ocean_floor_wg), std::end(ocean_floor_wg), settings::kMinY);
    for (std::vector<u16>& marks : post_process) marks.clear();
}

// Heightmap.update: a block that counts raises the column to above itself; one that does not,
// set where the column's top was, lets the column fall to the next that does.
void Terrain::update_heightmap(i32* map, u8 bit, i32 x, i32 y, i32 z, u16 state) {
    i32& first = map[x + z * 16];
    if (y <= first - 2) return;
    if (wg::kHeightmaps[state] & bit) {
        if (y >= first) first = y + 1;
    } else if (first - 1 == y) {
        for (i32 below = y - 1; below >= settings::kMinY; --below) {
            if (wg::kHeightmaps[block(x, below, z)] & bit) {
                first = below + 1;
                return;
            }
        }
        first = settings::kMinY;
    }
}

void Terrain::set_block(i32 x, i32 y, i32 z, u16 state) {
    blocks[index(x, y, z)] = state;
    update_heightmap(ocean_floor_wg, wg::kOceanFloorWg, x, y, z, state);
    update_heightmap(world_surface_wg, wg::kWorldSurfaceWg, x, y, z, state);
}

void Terrain::mark_post_process(i32 x, i32 y, i32 z) {
    post_process[static_cast<u32>(y - settings::kMinY) >> 4].push_back(static_cast<u16>(x | (y & 15) << 4 | z << 8));
}

NoiseChunk::NoiseChunk(const Overworld& world, Sampler& sampler, i32 chunk_x, i32 chunk_z)
    : sampler_(reset(sampler)),
      volume_{16, settings::kHeight, 16, chunk_x * 16, settings::kMinY, chunk_z * 16},
      aquifer_(sampler_, world.aquifer_random(), volume_, settings::kSeaLevel, settings::kDefaultFluid, settings::kLava) {}

void fill_from_noise(NoiseChunk& chunk, Terrain& terrain) {
    const Volume& v = chunk.volume();
    Sampler& sampler = chunk.sampler();
    Aquifer& aquifer = chunk.aquifer();
    Scratch& scratch = sampler.scratch();
    const Scratch::Mark mark = scratch.mark();
    float* density = scratch.take(v.size());
    sampler.volume(wg::root::kRouterFinalDensity, density, v);
    // Column by column, from the top down - the order the aquifer is asked in, which is the
    // order its cells and the context's caches come to hold what they hold.
    for (i32 z = 0; z < v.size_z; ++z) {
        const i32 block_z = v.block_z(z);
        for (i32 x = 0; x < v.size_x; ++x) {
            const i32 block_x = v.block_x(x);
            for (i32 y = v.size_y - 1; y >= 0; --y) {
                const i32 block_y = v.block_y(y);
                const u32 substance =
                    aquifer.compute_substance(block_x, block_y, block_z, static_cast<double>(density[v.index(x, y, z)]));
                const u16 state = substance == Aquifer::kSolid ? settings::kDefaultBlock : static_cast<u16>(substance);
                if (state == world::kAir) continue;
                terrain.set_block(x, block_y, z, state);
                if (aquifer.should_schedule_fluid_update() && (world::flags_of(state) & world::state::kHasFluid))
                    terrain.mark_post_process(x, block_y, z);
            }
        }
    }
    scratch.release(mark);
}

}  // namespace worldgen
