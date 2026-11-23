// SPDX-License-Identifier: GPL-3.0-or-later
#include "worldgen/climate.hpp"

#include PROTO_GEN_WORLDGEN_CLIMATE_HPP

namespace worldgen {

namespace wg = proto::gen::worldgen;

namespace {

// Climate.RTree.Node.distance: the squares of how far the target lies outside the node's box
// (Climate.Parameter.distance), parameter by parameter.
ALWAYS_INLINE i64 distance(const wg::ClimateNode& n, const ClimateTarget& t) {
    i64 sum = 0;
    for (u32 i = 0; i < 7; ++i) {
        const i64 above = t.v[i] - n.max[i], below = n.min[i] - t.v[i];
        const i64 d = above > 0 ? above : std::max<i64>(below, 0);
        sum += d * d;
    }
    return sum;
}

// Climate.RTree.SubTree.search (a leaf's is itself).
i32 search(i32 at, const ClimateTarget& t, i32 candidate) {
    const wg::ClimateNode& n = wg::kClimate[at];
    if (n.children == 0) return at;
    i64 min_distance = candidate == kNoLeaf ? INT64_MAX : distance(wg::kClimate[candidate], t);
    i32 closest = candidate;
    for (u32 i = 0; i < n.children; ++i) {
        const i32 child = static_cast<i32>(n.first + i);
        const i64 child_distance = distance(wg::kClimate[child], t);
        if (min_distance <= child_distance) continue;
        const i32 leaf = search(child, t, closest);
        const i64 leaf_distance = leaf == child ? child_distance : distance(wg::kClimate[leaf], t);
        if (min_distance <= leaf_distance) continue;
        min_distance = leaf_distance;
        closest = leaf;
    }
    return closest;
}

}  // namespace

i32 climate_search(const ClimateTarget& target, i32 last) { return search(0, target, last); }

u16 climate_biome(i32 leaf) { return wg::kClimate[leaf].first; }

void fill_biomes(Sampler& sampler, i32 chunk_x, i32 chunk_z, i32 min_y, u32 sections, u16* out) {
    // MultiNoiseBiomeSource.createResolverForChunk: the six functions over the chunk's cells, one
    // volume each, in this order.
    const Volume v{4, static_cast<i32>(sections) * 4, 4, chunk_x * 16, min_y, chunk_z * 16, 4, 4, 4};
    const u32 size = v.size();
    Scratch& scratch = sampler.scratch();
    const Scratch::Mark mark = scratch.mark();
    float* temperature = scratch.take(size);
    float* vegetation = scratch.take(size);
    float* continents = scratch.take(size);
    float* erosion = scratch.take(size);
    float* depth = scratch.take(size);
    float* ridges = scratch.take(size);
    sampler.volume(wg::root::kRouterTemperature, temperature, v);
    sampler.volume(wg::root::kRouterVegetation, vegetation, v);
    sampler.volume(wg::root::kRouterContinents, continents, v);
    sampler.volume(wg::root::kRouterErosion, erosion, v);
    sampler.volume(wg::root::kRouterDepth, depth, v);
    sampler.volume(wg::root::kRouterRidges, ridges, v);
    // ChunkAccess.fillBiomesFromNoise, LevelChunkSection.fillBiomesFromNoise: every cell asks the
    // tree, beginning where the cell before it ended.
    i32 last = kNoLeaf;
    for (u32 s = 0; s < sections; ++s) {
        for (i32 x = 0; x < 4; ++x) {
            for (i32 y = 0; y < 4; ++y) {
                for (i32 z = 0; z < 4; ++z) {
                    const u32 i = v.index(x, static_cast<i32>(s) * 4 + y, z);
                    last = climate_search(
                        climate_target(temperature[i], vegetation[i], continents[i], erosion[i], depth[i], ridges[i]), last);
                    out[s * 64 + static_cast<u32>(y << 4 | z << 2 | x)] = climate_biome(last);
                }
            }
        }
    }
    scratch.release(mark);
}

}  // namespace worldgen
