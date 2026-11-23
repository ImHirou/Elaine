// SPDX-License-Identifier: GPL-3.0-or-later
#include "worldgen/climate.hpp"

#include <limits>

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

// Whether a leaf of another biome than `best`'s is as near to the target as `best` is: then which
// of them a search ends at depends on where it began.
bool tied(i32 at, const ClimateTarget& t, i32 best, i64 best_distance) {
    const wg::ClimateNode& n = wg::kClimate[at];
    if (n.children == 0) return at != best && n.first != wg::kClimate[best].first;
    for (u32 i = 0; i < n.children; ++i) {
        const i32 child = static_cast<i32>(n.first + i);
        if (distance(wg::kClimate[child], t) <= best_distance && tied(child, t, best, best_distance)) return true;
    }
    return false;
}

// LinearCongruentialGenerator.next
ALWAYS_INLINE u64 lcg_next(u64 rval, u64 c) { return rval * (rval * 6364136223846793005ull + 1442695040888963407ull) + c; }

// BiomeManager.getFiddle: a tenth of the low bits, as a step of at most 0.45 either way.
ALWAYS_INLINE double fiddle(u64 rval) {
    const double uniform = static_cast<double>(static_cast<i64>(rval) >> 24 & 1023) / 1024.0;  // Math.floorMod(rval >> 24, 1024)
    return (uniform - 0.5) * 0.9;
}

// BiomeManager.getFiddledDistance
ALWAYS_INLINE double fiddled_distance(i64 seed, i32 x, i32 y, i32 z, double dx, double dy, double dz) {
    const u64 s = static_cast<u64>(seed), ux = static_cast<u64>(static_cast<i64>(x)), uy = static_cast<u64>(static_cast<i64>(y));
    const u64 uz = static_cast<u64>(static_cast<i64>(z));
    u64 rval = s;
    rval = lcg_next(rval, ux);
    rval = lcg_next(rval, uy);
    rval = lcg_next(rval, uz);
    rval = lcg_next(rval, ux);
    rval = lcg_next(rval, uy);
    rval = lcg_next(rval, uz);
    const double fx = fiddle(rval);
    rval = lcg_next(rval, s);
    const double fy = fiddle(rval);
    rval = lcg_next(rval, s);
    const double fz = fiddle(rval);
    const double a = dz + fz, b = dy + fy, c = dx + fx;
    return a * a + b * b + c * c;
}

}  // namespace

i32 climate_search(const ClimateTarget& target, i32 last) { return search(0, target, last); }

QuartCell zoom_cell(i64 zoom_seed, i32 x, i32 y, i32 z) {
    const i32 ax = x - 2, ay = y - 2, az = z - 2;
    const i32 px = ax >> 2, py = ay >> 2, pz = az >> 2;
    const double fx = static_cast<double>(ax & 3) / 4.0, fy = static_cast<double>(ay & 3) / 4.0,
                 fz = static_cast<double>(az & 3) / 4.0;
    u32 nearest = 0;
    double least = std::numeric_limits<double>::infinity();
    for (u32 i = 0; i < 8; ++i) {
        const bool x_even = (i & 4) == 0, y_even = (i & 2) == 0, z_even = (i & 1) == 0;
        const double d = fiddled_distance(zoom_seed, x_even ? px : px + 1, y_even ? py : py + 1, z_even ? pz : pz + 1,
                                          x_even ? fx : fx - 1.0, y_even ? fy : fy - 1.0, z_even ? fz : fz - 1.0);
        if (least > d) {
            nearest = i;
            least = d;
        }
    }
    return {(nearest & 4) == 0 ? px : px + 1, (nearest & 2) == 0 ? py : py + 1, (nearest & 1) == 0 ? pz : pz + 1};
}

u16 CarverBiomes::at(const QuartCell& cell) {
    // Climate.Sampler.sample: the six functions at the cell's block, each by itself.
    const i32 x = cell.x << 2, y = cell.y << 2, z = cell.z << 2;
    const float temperature = climate.value(wg::root::kRouterTemperature, x, y, z);
    const float vegetation = climate.value(wg::root::kRouterVegetation, x, y, z);
    const float continents = climate.value(wg::root::kRouterContinents, x, y, z);
    const float erosion = climate.value(wg::root::kRouterErosion, x, y, z);
    const float depth = climate.value(wg::root::kRouterDepth, x, y, z);
    const float ridges = climate.value(wg::root::kRouterRidges, x, y, z);
    last = climate_search(climate_target(temperature, vegetation, continents, erosion, depth, ridges), last);
    return climate_biome(last);
}

void fill_biome_grid(Sampler& sampler, i32 chunk_x, i32 chunk_z, i32 min_y, u32 sections, BiomeGrid& grid, u16* own) {
    constexpr i32 kW = BiomeGrid::kWidth;
    grid.qx0 = chunk_x * 4 - 1;
    grid.qy0 = min_y >> 2;
    grid.qz0 = chunk_z * 4 - 1;
    grid.height = static_cast<i32>(sections) * 4;
    grid.present[0] = grid.present[1] = 0;
    // The six functions over the cells of the grid. (For the chunk's own cells these are the
    // values of fill_biomes' smaller volume: the functions of the climate come out the same
    // whichever volume they are sampled over.)
    const Volume v{kW, grid.height, kW, grid.qx0 * 4, min_y, grid.qz0 * 4, 4, 4, 4};
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
    const auto target = [&](u32 i) {
        return climate_target(temperature[i], vegetation[i], continents[i], erosion[i], depth[i], ridges[i]);
    };
    const auto cell = [&](i32 x, i32 y, i32 z) -> u16& { return grid.cells[(x * kW + z) * grid.height + y]; };

    // The chunk's own cells, in the order vanilla fills them.
    i32 last = kNoLeaf;
    for (u32 s = 0; s < sections; ++s) {
        for (i32 x = 0; x < 4; ++x) {
            for (i32 y = 0; y < 4; ++y) {
                for (i32 z = 0; z < 4; ++z) {
                    const i32 gy = static_cast<i32>(s) * 4 + y;
                    last = climate_search(target(v.index(x + 1, gy, z + 1)), last);
                    const u16 biome = climate_biome(last);
                    own[s * 64 + static_cast<u32>(y << 4 | z << 2 | x)] = biome;
                    cell(x + 1, gy, z + 1) = biome;
                }
            }
        }
    }
    // The ring: each neighbour's cells by themselves. A cell where no other biome is as near is
    // that biome whatever was asked before it; a neighbour with a cell that is not is filled whole.
    bool ambiguous[3][3] = {};
    for (i32 x = 0; x < kW; ++x) {
        for (i32 z = 0; z < kW; ++z) {
            if (x >= 1 && x <= 4 && z >= 1 && z <= 4) continue;
            bool& unsure = ambiguous[x == 0 ? 0 : x == 5 ? 2 : 1][z == 0 ? 0 : z == 5 ? 2 : 1];
            for (i32 y = 0; y < grid.height; ++y) {
                const ClimateTarget t = target(v.index(x, y, z));
                last = climate_search(t, last);
                cell(x, y, z) = climate_biome(last);
                if (!unsure && tied(0, t, last, distance(wg::kClimate[last], t))) unsure = true;
            }
        }
    }
    scratch.release(mark);
    for (i32 nx = 0; nx < 3; ++nx) {
        for (i32 nz = 0; nz < 3; ++nz) {
            if (!ambiguous[nx][nz]) continue;
            u16 theirs[96 * 16];
            DCHECK(sections * 64 <= std::size(theirs));
            sampler.reset();
            fill_biomes(sampler, chunk_x + nx - 1, chunk_z + nz - 1, min_y, sections, theirs);
            for (i32 x = 0; x < kW; ++x) {
                for (i32 z = 0; z < kW; ++z) {
                    if ((x == 0 ? 0 : x == 5 ? 2 : 1) != nx || (z == 0 ? 0 : z == 5 ? 2 : 1) != nz) continue;
                    // The cell of the neighbour this column of the grid is: quart -1 is its 3, quart 4 its 0.
                    const u32 lx = static_cast<u32>(x - 1) & 3, lz = static_cast<u32>(z - 1) & 3;
                    for (i32 y = 0; y < grid.height; ++y)
                        cell(x, y, z) = theirs[static_cast<u32>(y >> 2) * 64 + (static_cast<u32>(y & 3) << 4 | lz << 2 | lx)];
                }
            }
        }
    }
    for (i32 i = 0; i < kW * kW * grid.height; ++i) grid.present[grid.cells[i] >> 6] |= 1ull << (grid.cells[i] & 63);
}

u16 climate_biome(i32 leaf) { return wg::kClimate[leaf].first; }

void spawn_origin(Sampler& sampler, i32& x, i32& z) {
    sampler.reset();
    // getSpawnPositionAndFitness: how far the climate at a block is from the nearest of the
    // points asked for, and - counting two thousand times less - how far the block is from 0, 0.
    const auto fitness = [&](i32 block_x, i32 block_z) {
        const i32 qx = (block_x >> 2) << 2, qz = (block_z >> 2) << 2;
        i64 least = std::numeric_limits<i64>::max();
        const wg::SpawnParameter* p = wg::kSpawnParameters;
        for (const u8 count : wg::kSpawnPoints) {
            i64 of_point = 0;
            for (u32 i = 0; i < count; ++i, ++p) {
                // Climate.Parameter.distance
                const i64 value = quantize_coord(sampler.value(p->node, qx, 0, qz));
                const i64 above = value - p->max, below = p->min - value;
                const i64 d = above > 0 ? above : std::max<i64>(below, 0);
                of_point += d * d;
            }
            least = std::min(least, of_point);
        }
        const i64 from_origin = static_cast<i64>(block_x) * block_x + static_cast<i64>(block_z) * block_z;
        return least * (2048ll * 2048ll) + from_origin;
    };
    i32 best_x = 0, best_z = 0;
    i64 best = fitness(0, 0);
    // radialSearch: rings around the best so far, wide ones first.
    const auto search = [&](float max_radius, float increment) {
        float angle = 0.0f, radius = increment;
        const i32 origin_x = best_x, origin_z = best_z;
        while (radius <= max_radius) {
            const i32 at_x = origin_x + static_cast<i32>(std::sin(static_cast<double>(angle)) * static_cast<double>(radius));
            const i32 at_z = origin_z + static_cast<i32>(std::cos(static_cast<double>(angle)) * static_cast<double>(radius));
            const i64 f = fitness(at_x, at_z);
            if (f < best) {
                best = f;
                best_x = at_x;
                best_z = at_z;
            }
            angle += increment / radius;
            if (static_cast<double>(angle) > 6.283185307179586) {
                angle = 0.0f;
                radius += increment;
            }
        }
    };
    search(2048.0f, 512.0f);
    search(512.0f, 32.0f);
    x = best_x;
    z = best_z;
}

void fill_biomes(Sampler& sampler, i32 chunk_x, i32 chunk_z, i32 min_y, u32 sections, u16* out, u8* tied_cells) {
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
                    const ClimateTarget t =
                        climate_target(temperature[i], vegetation[i], continents[i], erosion[i], depth[i], ridges[i]);
                    last = climate_search(t, last);
                    const u32 cell = s * 64 + static_cast<u32>(y << 4 | z << 2 | x);
                    out[cell] = climate_biome(last);
                    if (tied_cells) tied_cells[cell] = tied(0, t, last, distance(wg::kClimate[last], t));
                }
            }
        }
    }
    scratch.release(mark);
}

}  // namespace worldgen
