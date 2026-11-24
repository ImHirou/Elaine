// SPDX-License-Identifier: GPL-3.0-or-later
#include "worldgen/generator.hpp"

#include <memory>

#include "world/chunk_io.hpp"
#include "world/gen_chunk.hpp"
#include "worldgen/carver.hpp"
#include "worldgen/climate.hpp"
#include "worldgen/material.hpp"

namespace worldgen {

namespace {

// What a thread makes chunks with: the contexts a chunk has in vanilla - one for its biomes, one
// for its terrain, and none (a context without caches) for the biomes the carvers ask for - and
// the terrain while it is made. Kept between chunks: nothing is allocated for the second chunk a
// thread makes.
struct Work {
    const Overworld* world;
    Sampler biomes, terrain_sampler, uncached;
    Terrain terrain;
    BiomeGrid grid;
    CarvingMask mask;
    u16 biome[Terrain::kSections * 64];

    explicit Work(const Overworld& w) : world(&w), biomes(w.noises()), terrain_sampler(w.noises()), uncached(w.noises(), false) {}
};

Work& work_for(const Overworld& world) {
    thread_local std::unique_ptr<Work> work;
    if (!work || work->world != &world) work = std::make_unique<Work>(world);
    return *work;
}

// A chunk as the `biomes` and `terrain` statuses leave it: its cells' biomes in w.biome, and the
// terrain.
Terrain& make_terrain(Work& w, const Overworld& world, i32 cx, i32 cz) {
    w.biomes.reset();
    fill_biome_grid(w.biomes, cx, cz, settings::kMinY, Terrain::kSections, w.grid, w.biome);
    Terrain& t = w.terrain;
    t.begin(cx, cz);
    NoiseChunk chunk(world, w.terrain_sampler, cx, cz);
    fill_from_noise(chunk, t);
    build_surface(world, chunk, w.grid, t);
    CarverBiomes carver_biomes{w.uncached};
    carve(world, chunk, carver_biomes, w.mask, t);
    return t;
}

// PlayerSpawnFinder.getSpawnPosInChunk, getLevelRespawnPos: the first column - x, then z - with
// ground on top of it that is no water's bed, and above that ground a block to stand on.
bool spawn_in_chunk(const Terrain& t, i32 spawn[3]) {
    namespace wg = proto::gen::worldgen;
    const i32 min_y = settings::kMinY, top_of_world = settings::kMinY + settings::kHeight - 1;
    for (i32 x = 0; x < 16; ++x) {
        for (i32 z = 0; z < 16; ++z) {
            // ChunkAccess.getHeight of three heightmaps: the highest block that counts for each.
            const auto height = [&](u8 bit) {
                i32 y = top_of_world;
                while (y >= min_y && !(wg::kHeightmaps[t.block(x, y, z)] & bit)) --y;
                return y;
            };
            const i32 top = height(wg::kMotionBlocking);
            if (top < min_y) continue;
            const i32 surface = height(wg::kWorldSurface);
            if (surface <= top && surface > height(wg::kOceanFloor)) continue;
            for (i32 y = top + 1; y >= min_y; --y) {
                const u16 state = t.block_or_air(x, y, z);
                if (world::flags_of(state) & world::state::kHasFluid) break;
                if (!world::collision_face_full(state, 1)) continue;  // (Direction.UP)
                spawn[0] = t.chunk_x * 16 + x;
                spawn[1] = y + 1;
                spawn[2] = t.chunk_z * 16 + z;
                return true;
            }
        }
    }
    return false;
}

}  // namespace

void find_world_spawn(const Overworld& world, i32 spawn[3]) {
    Work& w = work_for(world);
    i32 origin_x = 0, origin_z = 0;
    spawn_origin(w.biomes, origin_x, origin_z);
    const i32 cx = origin_x >> 4, cz = origin_z >> 4;
    spawn[0] = cx * 16 + 8;
    spawn[1] = 64;  // ChunkGenerator.getSpawnHeight
    spawn[2] = cz * 16 + 8;
    // MinecraftServer.setInitialSpawn's spiral: the chunk itself, then round it, further each turn.
    i32 dx = 0, dz = 0, step_x = 0, step_z = -1;
    for (i32 i = 0; i < 11 * 11; ++i) {
        if (dx >= -5 && dx <= 5 && dz >= -5 && dz <= 5 && spawn_in_chunk(make_terrain(w, world, cx + dx, cz + dz), spawn)) return;
        if (dx == dz || (dx < 0 && dx == -dz) || (dx > 0 && dx == 1 - dz)) {
            const i32 old = step_x;
            step_x = -step_z;
            step_z = old;
        }
        dx += step_x;
        dz += step_z;
    }
}

void OverworldGenerator::make_base(i32 cx, i32 cz, world::GenChunk& out) {
    namespace wg = proto::gen::worldgen;
    namespace hm = world::heightmap;
    Work& w = work_for(world_);
    const Terrain& t = make_terrain(w, world_, cx, cz);
    out.x = cx;
    out.z = cz;
    out.sections_count = static_cast<u8>(Terrain::kSections);
    out.version = 0;
    out.decorated = false;
    for (u32 si = 0; si < Terrain::kSections; ++si) out.sections[si].assign(out.alloc, &t.blocks[si * 4096]);
    std::copy(std::begin(w.biome), std::end(w.biome), out.cells);

    // The two heightmaps the terrain was made by stay as they are; the four a chunk keeps are
    // counted now, each column from the top down to the first block that counts for it.
    const i32 top = (t.highest_filled_section() + 1) * 16 + settings::kMinY;
    for (u32 i = 0; i < 256; ++i) {
        const i32 x = static_cast<i32>(i & 15), z = static_cast<i32>(i >> 4);
        out.height[hm::kWorldSurfaceWg][i] = static_cast<i16>(t.world_surface_wg[i] - settings::kMinY);
        out.height[hm::kOceanFloorWg][i] = static_cast<i16>(t.ocean_floor_wg[i] - settings::kMinY);
        u32 open = wg::kWorldSurface | wg::kOceanFloor | wg::kMotionBlocking | wg::kMotionBlockingNoLeaves;
        for (const u32 type : {hm::kWorldSurface, hm::kOceanFloor, hm::kMotionBlocking, hm::kMotionBlockingNoLeaves})
            out.height[type][i] = 0;
        for (i32 y = top - 1; y >= settings::kMinY && open; --y) {
            const u32 found = wg::kHeightmaps[t.block(x, y, z)] & open;
            if (!found) continue;
            for (const u32 type : {hm::kWorldSurface, hm::kOceanFloor, hm::kMotionBlocking, hm::kMotionBlockingNoLeaves})
                if (found >> type & 1) out.height[type][i] = static_cast<i16>(y + 1 - settings::kMinY);
            open &= ~found;
        }
    }

    // What is to be let flow when the chunk first ticks, section by section.
    out.marks.clear();
    for (u32 si = 0; si < Terrain::kSections; ++si)
        for (const u16 mark : t.post_process[si]) out.marks.push_back(si << 12 | mark);
    out.block_ticks.clear();
    out.fluid_ticks.clear();
    out.block_entities.clear();
    out.tags.clear();
}

bool OverworldGenerator::generate_whole(world::LoadedChunk& out) {
    DCHECK(out.chunk.sections_count == Terrain::kSections);
    world::GenChunk made;
    make_base(out.chunk.x, out.chunk.z, made);
    world::finish_chunk(made, out);
    return true;
}

}  // namespace worldgen
