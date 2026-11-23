// SPDX-License-Identifier: GPL-3.0-or-later
#include "worldgen/carver.hpp"

#include <bit>
#include <cstring>

#include "phys/mth.hpp"
#include "world/block.hpp"
#include "worldgen/material.hpp"

namespace worldgen {

namespace wg = proto::gen::worldgen;
namespace sf = wg::surface;
using phys::mth_cos;
using phys::mth_sin;

namespace {

// (Every draw is a statement of its own here: Java evaluates an expression left to right, C++
// in whatever order it likes.)

// FloatProvider.sample: ConstantFloat, UniformFloat (Mth.randomBetween), TrapezoidFloat.
template <class Random>
float sample(const wg::FloatProvider& p, Random& r) {
    switch (p.kind) {
        case wg::FloatKind::kConstant: return p.a;
        case wg::FloatKind::kUniform: return r.next_float() * (p.b - p.a) + p.a;
        case wg::FloatKind::kTrapezoid: {
            const float range = p.b - p.a;
            const float plateau_start = (range - p.c) / 2.0f;
            const float plateau_end = range - plateau_start;
            const float first = r.next_float();
            const float second = r.next_float();
            return p.a + first * plateau_end + second * plateau_start;
        }
    }
    return 0.0f;
}

// UniformHeight.sample
template <class Random>
i32 sample_y(const wg::Carver& c, Random& r) {
    return c.min_y > c.max_y ? c.min_y : r.next_int(c.max_y - c.min_y + 1) + c.min_y;
}

// `(a - b) * c` of three draws, as the tunnels turn by.
template <class Random>
float turn(Random& r) {
    const float a = r.next_float();
    const float b = r.next_float();
    const float c = r.next_float();
    return (a - b) * c;
}

// The chunk that is being carved.
struct Target {
    i32 chunk_x, chunk_z;
    CarvingMask& mask;
};

// WorldCarver.canReach: whether a tunnel with so many steps left can still come to the chunk.
bool can_reach(const Target& t, double x, double z, i32 step, i32 total, float thickness) {
    const double xd = x - static_cast<double>(t.chunk_x * 16 + 8), zd = z - static_cast<double>(t.chunk_z * 16 + 8);
    const double remaining = static_cast<double>(total - step);
    const double rr = static_cast<double>(thickness + 2.0f + 16.0f);
    return xd * xd + zd * zd - remaining * remaining <= rr * rr;
}

// WorldCarver.carveEllipsoid: the blocks of the chunk inside an ellipsoid, but for those `skip`
// says no to (the floor of a cave, the walls of a canyon).
template <class Skip>
void carve_ellipsoid(const Target& t, double x, double y, double z, double horizontal_radius, double vertical_radius,
                     Skip&& skip) {
    const double center_x = static_cast<double>(t.chunk_x * 16 + 8), center_z = static_cast<double>(t.chunk_z * 16 + 8);
    const double max_delta = 16.0 + horizontal_radius * 2.0;
    if (std::fabs(x - center_x) > max_delta) return;
    if (!(std::fabs(z - center_z) <= max_delta)) return;
    const i32 chunk_min_x = t.chunk_x * 16, chunk_min_z = t.chunk_z * 16;
    const i32 min_x = std::max(floor_int(x - horizontal_radius) - chunk_min_x - 1, 0);
    const i32 max_x = std::min(floor_int(x + horizontal_radius) - chunk_min_x, 15);
    const i32 min_y = std::max(floor_int(y - vertical_radius) - 1, CarvingMask::kMinY);
    const i32 max_y = std::min(floor_int(y + vertical_radius) + 1, CarvingMask::kMaxY);
    const i32 min_z = std::max(floor_int(z - horizontal_radius) - chunk_min_z - 1, 0);
    const i32 max_z = std::min(floor_int(z + horizontal_radius) - chunk_min_z, 15);
    for (i32 xi = min_x; xi <= max_x; ++xi) {
        const double xd = (static_cast<double>(chunk_min_x + xi) + 0.5 - x) / horizontal_radius;
        for (i32 zi = min_z; zi <= max_z; ++zi) {
            const double zd = (static_cast<double>(chunk_min_z + zi) + 0.5 - z) / horizontal_radius;
            if (xd * xd + zd * zd >= 1.0) continue;
            for (i32 world_y = max_y; world_y > min_y; --world_y) {
                const double yd = (static_cast<double>(world_y) - 0.5 - y) / vertical_radius;
                if (!skip(xd, yd, zd, world_y)) t.mask.carve(xi, world_y, zi);
            }
        }
    }
}

// CaveWorldCarver.shouldSkip: under the cave's floor, or outside the ellipsoid.
struct CaveSkip {
    double floor_level;
    bool operator()(double xd, double yd, double zd, i32) const {
        if (yd <= floor_level) return true;
        return xd * xd + yd * yd + zd * zd >= 1.0;
    }
};

// CaveWorldCarver.createTunnel
void cave_tunnel(const Target& t, i64 seed, double x, double y, double z, double horizontal_multiplier,
                 double vertical_multiplier, float thickness, float horizontal_rotation, float vertical_rotation, i32 step,
                 i32 dist, double y_scale, const CaveSkip& skip) {
    LegacyRandom r(seed);  // (RandomSource.createThreadLocalInstance: the same numbers)
    const i32 split = r.next_int(dist / 2) + dist / 4;
    const bool steep = r.next_int(6) == 0;
    float y_rota = 0.0f, x_rota = 0.0f;
    for (i32 current = step; current < dist; ++current) {
        const double horizontal_radius =
            1.5 +
            static_cast<double>(
                mth_sin(static_cast<double>(3.1415927f * static_cast<float>(current) / static_cast<float>(dist))) * thickness);
        const double vertical_radius = horizontal_radius * y_scale;
        const float cos_x = mth_cos(static_cast<double>(vertical_rotation));
        x += static_cast<double>(mth_cos(static_cast<double>(horizontal_rotation)) * cos_x);
        y += static_cast<double>(mth_sin(static_cast<double>(vertical_rotation)));
        z += static_cast<double>(mth_sin(static_cast<double>(horizontal_rotation)) * cos_x);
        vertical_rotation *= steep ? 0.92f : 0.7f;
        vertical_rotation += x_rota * 0.1f;
        horizontal_rotation += y_rota * 0.1f;
        x_rota *= 0.9f;
        y_rota *= 0.75f;
        x_rota += turn(r) * 2.0f;
        y_rota += turn(r) * 4.0f;
        if (current == split && thickness > 1.0f) {
            // Two tunnels go on from here, to the left and to the right.
            const i64 left = r.next_long();
            const float left_thickness = r.next_float() * 0.5f + 0.5f;
            cave_tunnel(t, left, x, y, z, horizontal_multiplier, vertical_multiplier, left_thickness,
                        horizontal_rotation - 1.5707964f, vertical_rotation / 3.0f, current, dist, 1.0, skip);
            const i64 right = r.next_long();
            const float right_thickness = r.next_float() * 0.5f + 0.5f;
            cave_tunnel(t, right, x, y, z, horizontal_multiplier, vertical_multiplier, right_thickness,
                        horizontal_rotation + 1.5707964f, vertical_rotation / 3.0f, current, dist, 1.0, skip);
            return;
        }
        if (r.next_int(4) == 0) continue;
        if (!can_reach(t, x, z, current, dist, thickness)) return;
        carve_ellipsoid(t, x, y, z, horizontal_radius * horizontal_multiplier, vertical_radius * vertical_multiplier, skip);
    }
}

// CaveWorldCarver.carve
template <class Random>
void carve_cave(const wg::Carver& c, Random& r, const Target& t, i32 source_x, i32 source_z) {
    constexpr i32 kMaxDistance = (4 * 2 - 1) << 4;  // (getRange() * 2 - 1 chunks)
    const i32 count =
        c.count_min + r.next_int(r.next_int(r.next_int(c.count_max - c.count_min + 1) + 1) + 1);  // VeryBiasedToBottomInt
    for (i32 cave = 0; cave < count; ++cave) {
        const double x = static_cast<double>(source_x * 16 + r.next_int(16));
        const double y = static_cast<double>(sample_y(c, r));
        const double z = static_cast<double>(source_z * 16 + r.next_int(16));
        const double horizontal_multiplier = static_cast<double>(sample(c.horizontal_radius_multiplier, r));
        const double vertical_multiplier = static_cast<double>(sample(c.vertical_radius_multiplier, r));
        const double start_vertical_multiplier = static_cast<double>(sample(c.start_vertical_radius_multiplier, r));
        const CaveSkip skip{static_cast<double>(sample(c.floor_level, r))};
        i32 tunnels = 1;
        if (r.next_int(4) == 0) {
            // createRoom
            const double y_scale = static_cast<double>(sample(c.room_vertical_radius_multiplier, r));
            const float thickness = 1.0f + r.next_float() * 6.0f;
            const double horizontal_radius = 1.5 + static_cast<double>(mth_sin(1.5707963705062866) * thickness);
            carve_ellipsoid(t, x + 1.0, y, z, horizontal_radius, horizontal_radius * y_scale, skip);
            tunnels += r.next_int(4);
        }
        for (i32 i = 0; i < tunnels; ++i) {
            const float horizontal_rotation = r.next_float() * 6.2831855f;
            const float vertical_rotation = (r.next_float() - 0.5f) / 4.0f;
            float thickness = sample(c.thickness, r);
            if (c.weird_thickness_bias && r.next_int(10) == 0) {
                const float a = r.next_float();
                const float b = r.next_float();
                thickness *= a * b * 3.0f + 1.0f;
            }
            const i32 distance = kMaxDistance - r.next_int(kMaxDistance / 4);
            const i64 seed = r.next_long();
            cave_tunnel(t, seed, x, y, z, horizontal_multiplier, vertical_multiplier, thickness, horizontal_rotation,
                        vertical_rotation, 0, distance, start_vertical_multiplier, skip);
        }
    }
}

// CanyonWorldCarver.doCarve
void canyon(const wg::Carver& c, const Target& t, i64 seed, double x, double y, double z, float thickness,
            float horizontal_rotation, float vertical_rotation, i32 step, i32 distance, double y_scale) {
    LegacyRandom r(seed);
    // initWidthFactors: how wide the canyon is at each height - the same for a few blocks, then another.
    float width[settings::kHeight];
    float factor = 1.0f;
    for (i32 i = 0; i < settings::kHeight; ++i) {
        if (i == 0 || r.next_int(c.width_smoothness) == 0) {
            const float a = r.next_float();
            const float b = r.next_float();
            factor = 1.0f + a * b;
        }
        width[i] = factor * factor;
    }
    // CanyonWorldCarver.shouldSkip
    const auto skip = [&width](double xd, double yd, double zd, i32 world_y) {
        return (xd * xd + zd * zd) * static_cast<double>(width[world_y - settings::kMinY - 1]) + yd * yd / 6.0 >= 1.0;
    };
    float y_rota = 0.0f, x_rota = 0.0f;
    for (i32 current = step; current < distance; ++current) {
        double horizontal_radius =
            1.5 + static_cast<double>(
                      mth_sin(static_cast<double>(static_cast<float>(current) * 3.1415927f / static_cast<float>(distance))) *
                      thickness);
        double vertical_radius = horizontal_radius * y_scale;
        horizontal_radius = horizontal_radius * static_cast<double>(sample(c.horizontal_radius_factor, r));
        {
            // updateVerticalRadius
            const float multiplier = 1.0f - std::fabs(0.5f - static_cast<float>(current) / static_cast<float>(distance)) * 2.0f;
            const float by = c.vertical_radius_default_factor + c.vertical_radius_center_factor * multiplier;
            const float chance = r.next_float() * (1.0f - 0.75f) + 0.75f;  // Mth.randomBetween(random, 0.75f, 1.0f)
            vertical_radius = static_cast<double>(by) * vertical_radius * static_cast<double>(chance);
        }
        const float xc = mth_cos(static_cast<double>(vertical_rotation));
        const float xs = mth_sin(static_cast<double>(vertical_rotation));
        x += static_cast<double>(mth_cos(static_cast<double>(horizontal_rotation)) * xc);
        y += static_cast<double>(xs);
        z += static_cast<double>(mth_sin(static_cast<double>(horizontal_rotation)) * xc);
        vertical_rotation *= 0.7f;
        vertical_rotation += x_rota * 0.05f;
        horizontal_rotation += y_rota * 0.05f;
        x_rota *= 0.8f;
        y_rota *= 0.5f;
        x_rota += turn(r) * 2.0f;
        y_rota += turn(r) * 4.0f;
        if (r.next_int(4) == 0) continue;
        if (!can_reach(t, x, z, current, distance, thickness)) return;
        carve_ellipsoid(t, x, y, z, horizontal_radius, vertical_radius, skip);
    }
}

// CanyonWorldCarver.carve
template <class Random>
void carve_canyon(const wg::Carver& c, Random& r, const Target& t, i32 source_x, i32 source_z) {
    constexpr i32 kMaxDistance = (4 * 2 - 1) * 16;
    const double x = static_cast<double>(source_x * 16 + r.next_int(16));
    const i32 y = sample_y(c, r);
    const double z = static_cast<double>(source_z * 16 + r.next_int(16));
    const float horizontal_rotation = r.next_float() * 6.2831855f;
    const float vertical_rotation = sample(c.vertical_rotation, r);
    const double y_scale = static_cast<double>(sample(c.y_scale, r));
    const float thickness = sample(c.thickness, r);
    const i32 distance = static_cast<i32>(static_cast<float>(kMaxDistance) * sample(c.distance_factor, r));
    const i64 seed = r.next_long();
    canyon(c, t, seed, x, static_cast<double>(y), z, thickness, horizontal_rotation, vertical_rotation, 0, distance, y_scale);
}

// The first bit at or after `from` that is set (or clear); kBits when there is none.
i32 next_bit(const CarvingMask& m, i32 from, bool set) {
    while (from < CarvingMask::kBits) {
        const u32 word = static_cast<u32>(from) >> 6, bit = static_cast<u32>(from) & 63;
        const u64 rest = (set ? m.words[word] : ~m.words[word]) >> bit;
        if (rest) return std::min(from + std::countr_zero(rest), CarvingMask::kBits);
        from = static_cast<i32>((word + 1) << 6);
    }
    return CarvingMask::kBits;
}

bool has_fluid(u16 state) { return world::flags_of(state) & world::state::kHasFluid; }

}  // namespace

void CarvingMask::clear() { std::memset(words, 0, sizeof words); }

bool CarvingMask::empty() const {
    for (const u64 word : words)
        if (word) return false;
    return true;
}

void mark_carved(i64 seed, i32 chunk_x, i32 chunk_z, CarvingMask& mask) {
    mask.clear();
    const Target t{chunk_x, chunk_z, mask};
    WorldgenRandom<LegacyRandom> random{LegacyRandom(0)};
    for (i32 dx = -8; dx <= 8; ++dx) {
        for (i32 dz = -8; dz <= 8; ++dz) {
            const i32 source_x = chunk_x + dx, source_z = chunk_z + dz;
            for (u32 index = 0; index < std::size(wg::kCarvers); ++index) {
                const wg::Carver& c = wg::kCarvers[index];
                random.set_large_feature_seed(static_cast<i64>(static_cast<u64>(seed) + index), source_x, source_z);
                if (!(random.next_float() <= c.probability)) continue;  // isStartChunk
                if (c.canyon)
                    carve_canyon(c, random, t, source_x, source_z);
                else
                    carve_cave(c, random, t, source_x, source_z);
            }
        }
    }
}

void apply_carving_mask(const Overworld& world, NoiseChunk& chunk, CarverBiomes& biomes, const CarvingMask& mask,
                        Terrain& terrain) {
    Aquifer& aquifer = chunk.aquifer();
    const i32 min_x = terrain.chunk_x * 16, min_z = terrain.chunk_z * 16;
    // CarvingMask.visit: every run of marked blocks, a column's part of it at a time, each from its
    // top down.
    for (i32 start = next_bit(mask, 0, true); start < CarvingMask::kBits;) {
        const i32 end = next_bit(mask, start, false) - 1;
        for (i32 column = start / CarvingMask::kColumn; column <= end / CarvingMask::kColumn; ++column) {
            const i32 x = column >> 4 & 15, z = column & 15, base = column * CarvingMask::kColumn;
            const i32 bottom = std::max(start - base, 0) + CarvingMask::kMinY;
            const i32 top = std::min(end - base, CarvingMask::kColumn - 1) + CarvingMask::kMinY;
            bool has_grass = false;
            for (i32 y = top; y >= bottom; --y) {
                const u16 old = terrain.block(x, y, z);
                if (wg::kHeightmaps[old] & wg::kUncarvable) continue;
                const u32 block = world::block_of(old);
                if (block == world::block_of(sf::kGrassBlock) || block == world::block_of(sf::kMycelium)) has_grass = true;
                const u32 substance = aquifer.compute_substance(min_x + x, y, min_z + z, 0.0);
                if (substance == Aquifer::kSolid) continue;
                const u16 state = static_cast<u16>(substance);
                terrain.set_block(x, y, z, state);
                if (aquifer.should_schedule_fluid_update() && has_fluid(state)) terrain.mark_post_process(x, y, z);
                if (!has_grass) continue;
                // The dirt that was under the grass is the top of its column now.
                if (world::block_of(terrain.block_or_air(x, y - 1, z)) != world::block_of(sf::kDirt)) continue;
                u16 top_state;
                if (!top_material(world, chunk, biomes, terrain, min_x + x, y - 1, min_z + z, has_fluid(state), top_state))
                    continue;
                terrain.set_block(x, y - 1, z, top_state);
                if (has_fluid(top_state)) terrain.mark_post_process(x, y - 1, z);
            }
        }
        start = next_bit(mask, end + 1, true);
    }
}

}  // namespace worldgen
