// SPDX-License-Identifier: GPL-3.0-or-later
#include "worldgen/generator.hpp"

#include <memory>

#include "world/chunk_io.hpp"
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

bool OverworldGenerator::generate_whole(world::LoadedChunk& out) {
    world::Chunk& c = out.chunk;
    DCHECK(c.sections_count == Terrain::kSections);
    Work& w = work_for(world_);
    Terrain& t = make_terrain(w, world_, c.x, c.z);

    // The biomes, a cell at a time; a section's palette in the order vanilla comes to its cells
    // (x, then y, then z).
    c.biomes = nullptr;
    for (u32 si = 0; si < Terrain::kSections; ++si) {
        world::BiomeSection made;
        for (u32 x = 0; x < 4; ++x) {
            for (u32 y = 0; y < 4; ++y) {
                for (u32 z = 0; z < 4; ++z) {
                    const u32 cell = y << 4 | z << 2 | x;
                    const u16 id = w.biome[si * 64 + cell];
                    u32 slot = 0;
                    while (slot < made.count && made.palette[slot] != id) ++slot;
                    if (slot == made.count) made.palette[made.count++] = id;
                    made.index[cell] = static_cast<u8>(slot);
                }
            }
        }
        c.biome[si] = made.palette[0];
        if (made.count == 1) continue;  // (a section of one biome is told by Chunk::biome alone)
        if (!c.biomes)
            c.biomes =
                new (out.arena.alloc(sizeof(world::BiomeSection) * world::kMaxSections)) world::BiomeSection[world::kMaxSections];
        c.biomes[si] = made;
    }
    for (u32 si = 0; si < Terrain::kSections; ++si) c.sections[si].assign(out.alloc, &t.blocks[si * 4096]);
    // What is to be let flow when the chunk first ticks (world::Chunk::post_process).
    usize marks = 0;
    for (const std::vector<u16>& of : t.post_process) marks += of.size();
    c.post_process = {};
    if (marks) {
        u8* p = static_cast<u8*>(out.arena.alloc(marks * 4));
        c.post_process = {static_cast<u32>(marks * 4), p};
        for (u32 si = 0; si < Terrain::kSections; ++si)
            for (const u16 mark : t.post_process[si]) {
                store_le<u32>(p, si << 12 | mark);
                p += 4;
            }
    }
    return true;
}

}  // namespace worldgen
