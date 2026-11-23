// SPDX-License-Identifier: GPL-3.0-or-later
// terrain_dump <seed> <out>, the chunks to make on stdin ("x z" a line): the overworld's terrain
// as src/worldgen makes it, a chunk as the last step of vanilla's `terrain` status leaves it -
// for tools/vanilla/terrain_probe.py, which holds it against what a vanilla server saved.
//
// The file, little-endian: "TERRDMP1", i64 seed, u32 chunks; of each
//   i32 x, z
//   u16 biome[sections * 64]      a cell's, by section and in it y << 4 | z << 2 | x
//   u8  tied[sections * 64]       1: another biome is exactly as near that cell's climate
//   i32 world_surface_wg[256], ocean_floor_wg[256]   the first free block, by x + z * 16
//   u32 marks, u32 packed bytes
//   u32 mark[marks]               section << 12 | x | y << 4 | z << 8, in the order of marking
//   the blocks, deflated: u16 state[sections * 4096], by section and in it y << 8 | z << 4 | x
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "world/region.hpp"
#include "worldgen/carver.hpp"
#include "worldgen/climate.hpp"
#include "worldgen/material.hpp"
#include "worldgen/terrain.hpp"

using namespace worldgen;

namespace {

template <class T>
void put(std::vector<u8>& out, const T* values, usize count) {
    const u8* bytes = reinterpret_cast<const u8*>(values);
    out.insert(out.end(), bytes, bytes + count * sizeof(T));  // (x86-64: the bytes are little-endian as they lie)
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <seed> <out>   (chunks on stdin: \"x z\" a line)\n", argv[0]);
        return 2;
    }
    const i64 seed = std::strtoll(argv[1], nullptr, 10);
    std::vector<std::pair<i32, i32>> wanted;
    for (int x, z; std::scanf("%d %d", &x, &z) == 2;) wanted.push_back({x, z});

    const Overworld world(seed);
    Sampler sampler(world.noises()), climate(world.noises()), uncached(world.noises(), false);
    const auto terrain = std::make_unique<Terrain>();
    const auto grid = std::make_unique<BiomeGrid>();
    const auto mask = std::make_unique<CarvingMask>();
    constexpr u32 kCells = Terrain::kSections * 64;
    std::vector<u16> own(kCells), again(kCells);
    std::vector<u8> tied(kCells), packed, out;
    out.insert(out.end(), {'T', 'E', 'R', 'R', 'D', 'M', 'P', '1'});
    put(out, &seed, 1);
    const u32 count = static_cast<u32>(wanted.size());
    put(out, &count, 1);
    for (const auto& [cx, cz] : wanted) {
        // As OverworldGenerator::generate_whole makes a chunk (worldgen/generator.cpp).
        terrain->begin(cx, cz);
        climate.reset();
        fill_biome_grid(climate, cx, cz, settings::kMinY, Terrain::kSections, *grid, own.data());
        NoiseChunk chunk(world, sampler, cx, cz);
        fill_from_noise(chunk, *terrain);
        build_surface(world, chunk, *grid, *terrain);
        CarverBiomes carver_biomes{uncached};
        carve(world, chunk, carver_biomes, *mask, *terrain);
        climate.reset();
        fill_biomes(climate, cx, cz, settings::kMinY, Terrain::kSections, again.data(), tied.data());

        const i32 at[2] = {cx, cz};
        put(out, at, 2);
        put(out, own.data(), kCells);
        put(out, tied.data(), kCells);
        put(out, terrain->world_surface_wg, 256);
        put(out, terrain->ocean_floor_wg, 256);
        std::vector<u32> marks;
        for (u32 s = 0; s < Terrain::kSections; ++s)
            for (const u16 mark : terrain->post_process[s]) marks.push_back(s << 12 | mark);
        world::deflate_bytes({reinterpret_cast<const u8*>(terrain->blocks), sizeof terrain->blocks}, packed);
        const u32 sizes[2] = {static_cast<u32>(marks.size()), static_cast<u32>(packed.size())};
        put(out, sizes, 2);
        put(out, marks.data(), marks.size());
        put(out, packed.data(), packed.size());
    }
    std::FILE* f = std::fopen(argv[2], "wb");
    if (!f || std::fwrite(out.data(), 1, out.size(), f) != out.size() || std::fclose(f) != 0) {
        std::fprintf(stderr, "cannot write %s\n", argv[2]);
        return 1;
    }
    return 0;
}
