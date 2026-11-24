// SPDX-License-Identifier: GPL-3.0-or-later
// feature_dump <seed> <out> [--only <file>], the steps to take on stdin: chunks of the overworld made
// and decorated by src/worldgen in an order it is given - for tools/vanilla/feature_probe.py, which
// takes the order a vanilla server decorated its chunks in out of the server's flight recording and
// holds what comes of the same order here against what the server saved.
//
//   T x z    the chunk's terrain is made (the `terrain` status)
//   F x z    it is decorated (`features`): the eight around it must have their terrain
//   L x z    the server had begun to light it: from here on, how light it is at a block in it or
//            beside it was its light engine's answer, which no order says (the blocks whose
//            making asks are written down)
//
// --only <file>: of the features of the biomes, only the ones named there (a line each) are placed.
// feature_dump --ported: every feature of a biome by name, and whether src/worldgen places it.
//
// The file, little-endian: "FEATDMP1", i64 seed, u32 chunks, u64 far reads, refused writes, marks
// and ticks beyond the nine chunks, blocks set outside the world's height; of each chunk
//   i32 x, z; u8 decorated
//   u32 packed bytes; the blocks, deflated (raw): u16 state[sections * 4096], by section and in it y << 8 | z << 4 | x
//   i32 height[4][256]            WORLD_SURFACE, OCEAN_FLOOR, MOTION_BLOCKING, MOTION_BLOCKING_NO_LEAVES: the
//                                 first free y of a column, by x + z * 16
//   u32 marks; u32 mark[marks]    section << 12 | x | y << 4 | z << 8, by section, in the order of marking
//   u32 n; n of {u32 type; i32 x, y, z, delay, priority}    the scheduled ticks of blocks (type: a block's id)
//   u32 n; the same of fluids (type: a fluid's id)
//   u32 n; n of {i32 x, y, z; u32 bytes; the tag}            block entities: a nameless compound, or nothing
//   u32 n; n of {i32 x, y, z}     blocks whose making asked for light the order does not say
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "world/gen_chunk.hpp"
#include "world/region.hpp"
#include "worldgen/feature.hpp"
#include "worldgen/generator.hpp"

using namespace worldgen;

namespace {

template <class T>
void put(std::vector<u8>& out, const T* values, usize count) {
    const u8* bytes = reinterpret_cast<const u8*>(values);
    out.insert(out.end(), bytes, bytes + count * sizeof(T));  // (x86-64: the bytes are little-endian as they lie)
}
template <class T>
void put(std::vector<u8>& out, T value) {
    put(out, &value, 1);
}

using At = std::pair<i32, i32>;

void put_ticks(std::vector<u8>& out, const world::GenChunk& c, const std::vector<world::SavedTick>& ticks) {
    put(out, static_cast<u32>(ticks.size()));
    for (const world::SavedTick& t : ticks) {
        put(out, t.type);
        const i32 fields[5] = {c.x * 16 + t.x, static_cast<i32>(t.y) + Region::kMinY, c.z * 16 + t.z, t.delay, t.priority};
        put(out, fields, 5);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--ported") == 0) {
        for (u32 i = 0; i < ft::kNamedPlaced; ++i)
            std::printf("%.*s %d\n", static_cast<int>(ft::kPlacedNames[i].size()), ft::kPlacedNames[i].data(),
                        ft::kPlaced[i].ported ? 1 : 0);
        return 0;
    }
    if (argc != 3 && !(argc == 5 && std::strcmp(argv[3], "--only") == 0)) {
        std::fprintf(stderr,
                     "usage: %s <seed> <out> [--only <file of names>]   (steps on stdin: \"T x z\", \"F x z\", \"L x z\")\n      "
                     " %s --ported\n",
                     argv[0], argv[0]);
        return 2;
    }
    const i64 seed = std::strtoll(argv[1], nullptr, 10);
    std::vector<u8> only;
    if (argc == 5) {
        std::set<std::string> names;
        std::FILE* f = std::fopen(argv[4], "r");
        if (!f) {
            std::fprintf(stderr, "cannot read %s\n", argv[4]);
            return 1;
        }
        for (char line[256]; std::fgets(line, sizeof line, f);) {
            std::string name(line);
            while (!name.empty() && (name.back() == '\n' || name.back() == '\r' || name.back() == ' ')) name.pop_back();
            if (!name.empty()) names.insert(name);
        }
        std::fclose(f);
        only.resize(ft::kStepFirst[ft::kSteps]);
        usize found = 0;
        for (u32 slot = 0; slot < only.size(); ++slot) {
            only[slot] = names.count(std::string(ft::kPlacedNames[ft::kStepPlaced[slot]])) != 0;
            found += only[slot];
        }
        if (found != names.size()) {
            std::fprintf(stderr, "%s names %zu features, %zu of them features of a biome\n", argv[4], names.size(), found);
            return 1;
        }
    }

    OverworldGenerator generator(seed);
    const auto work = std::make_unique<RegionWork>();
    FeatureWork feature_work;
    std::map<At, std::unique_ptr<world::GenChunk>> chunks;
    std::set<At> lighting;
    std::map<At, std::vector<Pos>> tainted;
    Region::Counts counts;
    char what;
    for (int x, z; std::scanf(" %c %d %d", &what, &x, &z) == 3;) {
        if (what == 'T') {
            auto& chunk = chunks[{x, z}];
            if (chunk) continue;
            chunk = std::make_unique<world::GenChunk>();
            generator.make_base(x, z, *chunk);
        } else if (what == 'L') {
            lighting.insert({x, z});
        } else if (what == 'F') {
            world::GenChunk* nine[9];
            for (i32 dz = -1; dz <= 1; ++dz)
                for (i32 dx = -1; dx <= 1; ++dx) {
                    const auto near = chunks.find({x + dx, z + dz});
                    if (near == chunks.end()) {
                        std::fprintf(stderr, "chunk %d, %d is decorated before %d, %d has its terrain\n", x, z, x + dx, z + dz);
                        return 1;
                    }
                    nine[(dx + 1) + (dz + 1) * 3] = near->second.get();
                }
            if (nine[4]->decorated) {
                std::fprintf(stderr, "chunk %d, %d is decorated twice\n", x, z);
                return 1;
            }
            Region region(*work, generator.world(), nine, x - 1, z - 1, 3, 3);
            // A server's light before a chunk is lit: bright (docs/architecture.md, "Features").
            std::vector<Pos> asked;
            region.set_brightness(15, &asked);
            region.begin(x, z);
            Decorator decorator(region, feature_work);
            if (!only.empty()) decorator.set_only(only);
            decorator.decorate();
            region.commit();
            const Region::Counts& c = region.counts();
            counts.far_reads += c.far_reads;
            counts.refused_writes += c.refused_writes;
            counts.far_marks += c.far_marks;
            counts.far_ticks += c.far_ticks;
            counts.outside_height += c.outside_height;
            for (const Pos& p : asked) {
                const i32 cx = p.x >> 4, cz = p.z >> 4;
                bool lit = false;
                for (i32 dz = -1; dz <= 1; ++dz)
                    for (i32 dx = -1; dx <= 1; ++dx) lit |= lighting.count({cx + dx, cz + dz}) != 0;
                if (lit) tainted[{cx, cz}].push_back(p);
            }
        } else {
            std::fprintf(stderr, "a step of the kind '%c'\n", what);
            return 1;
        }
    }

    std::vector<u8> out, packed;
    out.insert(out.end(), {'F', 'E', 'A', 'T', 'D', 'M', 'P', '1'});
    put(out, seed);
    put(out, static_cast<u32>(chunks.size()));
    const u64 counted[5] = {counts.far_reads, counts.refused_writes, counts.far_marks, counts.far_ticks, counts.outside_height};
    put(out, counted, 5);
    std::vector<u16> blocks(usize{Terrain::kSections} * 4096);
    namespace hm = world::heightmap;
    for (const auto& [at, chunk] : chunks) {
        const world::GenChunk& c = *chunk;
        const i32 place[2] = {c.x, c.z};
        put(out, place, 2);
        put(out, static_cast<u8>(c.decorated));
        for (u32 s = 0; s < Terrain::kSections; ++s)
            for (u32 b = 0; b < 4096; ++b) blocks[usize{s} * 4096 + b] = static_cast<u16>(c.sections[s].get(b));
        world::deflate_bytes({reinterpret_cast<const u8*>(blocks.data()), blocks.size() * 2}, packed);
        put(out, static_cast<u32>(packed.size()));
        put(out, packed.data(), packed.size());
        for (const u32 map : {hm::kWorldSurface, hm::kOceanFloor, hm::kMotionBlocking, hm::kMotionBlockingNoLeaves})
            for (u32 column = 0; column < 256; ++column) put(out, static_cast<i32>(c.height[map][column] + Region::kMinY));
        std::vector<u32> marks = c.marks;
        std::stable_sort(marks.begin(), marks.end(), [](u32 a, u32 b) { return a >> 12 < b >> 12; });
        put(out, static_cast<u32>(marks.size()));
        put(out, marks.data(), marks.size());
        put_ticks(out, c, c.block_ticks);
        put_ticks(out, c, c.fluid_ticks);
        put(out, static_cast<u32>(c.block_entities.size()));
        for (const world::GenBlockEntity& e : c.block_entities) {
            const i32 where[3] = {c.x * 16 + static_cast<i32>(e.at & 15), static_cast<i32>(e.at >> 8) + Region::kMinY,
                                  c.z * 16 + static_cast<i32>(e.at >> 4 & 15)};
            put(out, where, 3);
            put(out, e.len);
            put(out, c.tags.data() + e.tag, e.len);
        }
        const auto taints = tainted.find(at);
        put(out, static_cast<u32>(taints == tainted.end() ? 0 : taints->second.size()));
        if (taints != tainted.end())
            for (const Pos& p : taints->second) {
                const i32 where[3] = {p.x, p.y, p.z};
                put(out, where, 3);
            }
    }
    std::FILE* f = std::fopen(argv[2], "wb");
    if (!f || std::fwrite(out.data(), 1, out.size(), f) != out.size() || std::fclose(f) != 0) {
        std::fprintf(stderr, "cannot write %s\n", argv[2]);
        return 1;
    }
    return 0;
}
