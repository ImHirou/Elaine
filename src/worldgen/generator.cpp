// SPDX-License-Identifier: GPL-3.0-or-later
#include "worldgen/generator.hpp"

#include <memory>

#include "world/chunk_io.hpp"
#include "worldgen/climate.hpp"

namespace worldgen {

namespace {

// What a thread makes chunks with: the two contexts a chunk has in vanilla - one for its biomes,
// one for its terrain - and the terrain while it is made. Kept between chunks: nothing is
// allocated for the second chunk a thread makes.
struct Work {
    const Overworld* world;
    Sampler biomes, terrain_sampler;
    Terrain terrain;
    u16 biome[Terrain::kSections * 64];

    explicit Work(const Overworld& w) : world(&w), biomes(w.noises()), terrain_sampler(w.noises()) {}
};

Work& work_for(const Overworld& world) {
    thread_local std::unique_ptr<Work> work;
    if (!work || work->world != &world) work = std::make_unique<Work>(world);
    return *work;
}

}  // namespace

bool OverworldGenerator::generate_whole(world::LoadedChunk& out) {
    world::Chunk& c = out.chunk;
    DCHECK(c.sections_count == Terrain::kSections);
    Work& w = work_for(world_);

    // The biomes, a cell at a time; a section's palette in the order vanilla comes to its cells
    // (x, then y, then z).
    w.biomes.reset();
    fill_biomes(w.biomes, c.x, c.z, settings::kMinY, Terrain::kSections, w.biome);
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

    // The terrain.
    Terrain& t = w.terrain;
    t.begin(c.x, c.z);
    NoiseChunk chunk(world_, w.terrain_sampler, c.x, c.z);
    fill_from_noise(chunk, t);
    for (u32 si = 0; si < Terrain::kSections; ++si) c.sections[si].assign(out.alloc, &t.blocks[si * 4096]);
    return true;
}

}  // namespace worldgen
