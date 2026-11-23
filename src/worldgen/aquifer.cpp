// SPDX-License-Identifier: GPL-3.0-or-later
#include "worldgen/aquifer.hpp"

#include <limits>

namespace worldgen {

namespace wg = proto::gen::worldgen;

namespace {

constexpr i32 kWayBelowMinY = -32512;  // DimensionType.WAY_BELOW_MIN_Y: a level no block is under
constexpr i32 kAbsent = std::numeric_limits<i32>::min();

// The grid of aquifer cells: 16 x 12 x 16 blocks, sampled from (-5, +1, -5) of a block.
constexpr i32 grid_xz(i32 block) { return block >> 4; }
constexpr i32 from_grid_xz(i32 grid, i32 offset) { return (grid << 4) + offset; }
constexpr i32 grid_y(i32 block) { return floor_div(block, 12); }
constexpr i32 from_grid_y(i32 grid, i32 offset) { return grid * 12 + offset; }

// Aquifer.NoiseBasedAquifer.similarity: 1 where two points are equally far, 0 once the squares
// of their distances differ by 25.
constexpr double similarity(i32 distance1, i32 distance2) { return 1.0 - static_cast<double>(distance2 - distance1) / 25.0; }
constexpr double kFlowingUpdateSimilarity = similarity(10 * 10, 12 * 12);

// SURFACE_SAMPLING_OFFSETS_IN_CHUNKS: where around a point the surface is looked at, the point's
// own column first.
constexpr i32 kSurfaceOffsets[13][2] = {{0, 0},  {-2, -1}, {-1, -1}, {0, -1}, {1, -1}, {-3, 0}, {-2, 0},
                                        {-1, 0}, {1, 0},   {-2, 1},  {-1, 1}, {0, 1},  {1, 1}};

// Mth.clampedMap, Mth.map, of doubles
double inverse_lerp(double value, double min, double max) { return (value - min) / (max - min); }
double clamped_map(double value, double from_min, double from_max, double to_min, double to_max) {
    const double factor = inverse_lerp(value, from_min, from_max);
    return factor < 0.0 ? to_min : factor > 1.0 ? to_max : lerp(factor, to_min, to_max);
}
double map(double value, double from_min, double from_max, double to_min, double to_max) {
    return lerp(inverse_lerp(value, from_min, from_max), to_min, to_max);
}

}  // namespace

Aquifer::Aquifer(Sampler& sampler, const XoroshiroPositional& random, const Volume& volume, i32 sea_level, u16 water, u16 lava)
    : sampler_(sampler),
      random_(random),
      sea_level_(sea_level),
      lava_below_(std::min(-54, sea_level)),
      water_(water),
      lava_(lava) {
    const i32 max_x = volume.min_x + volume.size_x * volume.step_x - 1, max_y = volume.min_y + volume.size_y * volume.step_y - 1;
    const i32 max_z = volume.min_z + volume.size_z * volume.step_z - 1;
    min_gx_ = grid_xz(volume.min_x - 5);
    const i32 max_gx = grid_xz(max_x - 5) + 1;
    size_gx_ = max_gx - min_gx_ + 1;
    min_gy_ = grid_y(volume.min_y + 1) - 1;
    const i32 max_gy = grid_y(max_y + 1) + 1;
    const i32 size_gy = max_gy - min_gy_ + 1;
    min_gz_ = grid_xz(volume.min_z - 5);
    const i32 max_gz = grid_xz(max_z - 5) + 1;
    size_gz_ = max_gz - min_gz_ + 1;
    const u32 cells = static_cast<u32>(size_gx_ * size_gy * size_gz_);
    locations_.assign(cells, Location{0, 0, 0, false});
    statuses_.assign(cells, Status{{0, 0}, false});
    // The surface is asked for at a point of a cell and up to three chunks west, one east, one
    // north and south of it.
    const i32 x0 = from_grid_xz(min_gx_, 0), x1 = from_grid_xz(max_gx, 9), z0 = from_grid_xz(min_gz_, 0),
              z1 = from_grid_xz(max_gz, 9);
    surface_qx_ = (x0 - 48) >> 2;
    surface_qz_ = (z0 - 16) >> 2;
    surface_w_ = ((x1 + 16) >> 2) - surface_qx_ + 1;
    surface_h_ = ((z1 + 16) >> 2) - surface_qz_ + 1;
    surface_.assign(static_cast<usize>(surface_w_ * surface_h_), kAbsent);
    // Above the highest surface there is nothing but what the sea is: no cell is asked there.
    const i32 max_adjusted = max_surface_level(x0, z0, x1, z1) + 8;
    skip_above_y_ = from_grid_y(grid_y(max_adjusted + 12) + 1, 11) - 1;
}

// maxSurfaceLevel: the surface over the cells' own columns, as one volume - which is what the
// columns' levels are remembered from.
i32 Aquifer::max_surface_level(i32 min_x, i32 min_z, i32 max_x, i32 max_z) {
    const i32 min_qx = min_x >> 2, max_qx = max_x >> 2, min_qz = min_z >> 2, max_qz = max_z >> 2;
    const Volume v{max_qx - min_qx + 1, 1, max_qz - min_qz + 1, min_qx << 2, 0, min_qz << 2, 4, 1, 4};
    Scratch& scratch = sampler_.scratch();
    const Scratch::Mark mark = scratch.mark();
    float* buffer = scratch.take(v.size());
    sampler_.volume(wg::root::kAquiferSurfaceLevel, buffer, v);
    i32 max_y = std::numeric_limits<i32>::min();
    for (i32 z = 0; z < v.size_z; ++z) {
        for (i32 x = 0; x < v.size_x; ++x) {
            const i32 level = floor_int(buffer[v.index(x, 0, z)]);
            surface_[static_cast<usize>((min_qz + z - surface_qz_) * surface_w_ + (min_qx + x - surface_qx_))] = level;
            if (level > max_y) max_y = level;
        }
    }
    scratch.release(mark);
    return max_y;
}

// surfaceLevel: of a quart column, asked once.
i32 Aquifer::surface_level(i32 x, i32 z) {
    const i32 qx = x >> 2, qz = z >> 2;
    const i32 cx = qx - surface_qx_, cz = qz - surface_qz_;
    if (UNLIKELY(cx < 0 || cz < 0 || cx >= surface_w_ || cz >= surface_h_))  // (no place an aquifer asks about: not remembered)
        return floor_int(sampler_.value(wg::root::kAquiferSurfaceLevel, qx << 2, 0, qz << 2));
    i32& slot = surface_[static_cast<usize>(cz * surface_w_ + cx)];
    if (slot == kAbsent) slot = floor_int(sampler_.value(wg::root::kAquiferSurfaceLevel, qx << 2, 0, qz << 2));
    return slot;
}

u32 Aquifer::compute_substance(i32 x, i32 y, i32 z, double density) {
    if (density > 0.0) {
        should_schedule_ = false;
        return kSolid;
    }
    const FluidStatus global = global_fluid(y);
    if (y > skip_above_y_) {
        should_schedule_ = false;
        return global.at(y);
    }
    if (global.at(y) == lava_) {
        should_schedule_ = false;
        return lava_;
    }
    // The four nearest of the twelve points around: two cells along x and z, three along y.
    const i32 ax = grid_xz(x - 5), ay = grid_y(y + 1), az = grid_xz(z - 5);
    i32 d1 = INT32_MAX, d2 = INT32_MAX, d3 = INT32_MAX, d4 = INT32_MAX;
    u32 i1 = 0, i2 = 0, i3 = 0, i4 = 0;
    for (i32 x1 = 0; x1 <= 1; ++x1) {
        for (i32 y1 = -1; y1 <= 1; ++y1) {
            for (i32 z1 = 0; z1 <= 1; ++z1) {
                const i32 gx = ax + x1, gy = ay + y1, gz = az + z1;
                const u32 at = index(gx, gy, gz);
                Location& l = locations_[at];
                if (!l.known) {
                    XoroshiroRandom r = random_.at(gx, gy, gz);
                    l.x = from_grid_xz(gx, r.next_int(10));
                    l.y = from_grid_y(gy, r.next_int(9));
                    l.z = from_grid_xz(gz, r.next_int(10));
                    l.known = true;
                }
                const i32 dx = l.x - x, dy = l.y - y, dz = l.z - z;
                const i32 d = dx * dx + dy * dy + dz * dz;
                if (d1 >= d) {
                    i4 = i3, i3 = i2, i2 = i1, i1 = at;
                    d4 = d3, d3 = d2, d2 = d1, d1 = d;
                } else if (d2 >= d) {
                    i4 = i3, i3 = i2, i2 = at;
                    d4 = d3, d3 = d2, d2 = d;
                } else if (d3 >= d) {
                    i4 = i3, i3 = at;
                    d4 = d3, d3 = d;
                } else if (d4 >= d) {
                    i4 = at;
                    d4 = d;
                }
            }
        }
    }
    const FluidStatus s1 = status(i1);
    const double similarity12 = similarity(d1, d2);
    const u16 fluid = s1.at(y);
    if (similarity12 <= 0.0) {
        // One point is plainly the nearest: its fluid, which flows if the next one's differs.
        should_schedule_ = similarity12 >= kFlowingUpdateSimilarity && !(s1 == status(i2));
        return fluid;
    }
    if (fluid == water_ && global_fluid(y - 1).at(y - 1) == lava_) {
        should_schedule_ = true;
        return fluid;
    }
    double barrier_noise = std::numeric_limits<double>::quiet_NaN();
    const FluidStatus s2 = status(i2);
    const double barrier12 = similarity12 * calculate_pressure(x, y, z, barrier_noise, s1, s2);
    if (density + barrier12 > 0.0) {
        should_schedule_ = false;
        return kSolid;
    }
    const FluidStatus s3 = status(i3);
    const double similarity13 = similarity(d1, d3);
    if (similarity13 > 0.0) {
        const double barrier13 = (similarity12 * similarity13) * calculate_pressure(x, y, z, barrier_noise, s1, s3);
        if (density + barrier13 > 0.0) {
            should_schedule_ = false;
            return kSolid;
        }
    }
    const double similarity23 = similarity(d2, d3);
    if (similarity23 > 0.0) {
        const double barrier23 = (similarity12 * similarity23) * calculate_pressure(x, y, z, barrier_noise, s2, s3);
        if (density + barrier23 > 0.0) {
            should_schedule_ = false;
            return kSolid;
        }
    }
    const bool may_flow12 = !(s1 == s2);
    const bool may_flow23 = similarity23 >= kFlowingUpdateSimilarity && !(s2 == s3);
    const bool may_flow13 = similarity13 >= kFlowingUpdateSimilarity && !(s1 == s3);
    if (may_flow12 || may_flow23 || may_flow13)
        should_schedule_ = true;
    else
        should_schedule_ =
            similarity13 >= kFlowingUpdateSimilarity && similarity(d1, d4) >= kFlowingUpdateSimilarity && !(s1 == status(i4));
    return fluid;
}

// getAquiferStatus: a cell's fluid, worked out once - at the cell's point.
const FluidStatus& Aquifer::status(u32 at) {
    Status& s = statuses_[at];
    if (!s.known) {
        const Location& l = locations_[at];
        s.status = compute_fluid(l.x, l.y, l.z);
        s.known = true;
    }
    return s.status;
}

FluidStatus Aquifer::compute_fluid(i32 x, i32 y, i32 z) {
    const FluidStatus global = global_fluid(y);
    i32 lowest_surface = INT32_MAX;
    const i32 top = y + 12, bottom = y - 12;
    bool center_under_global = false;
    for (const i32* offset : kSurfaceOffsets) {
        const i32 sx = x + (offset[0] << 4), sz = z + (offset[1] << 4);
        const i32 surface = surface_level(sx, sz);
        const i32 adjusted = surface + 8;
        const bool start = offset[0] == 0 && offset[1] == 0;
        if (start && bottom > adjusted) return global;  // a cell in the open air: the sea, if anything
        const bool pokes_above = top > adjusted;
        if (pokes_above || start) {
            const FluidStatus at_surface = global_fluid(adjusted);
            if (at_surface.at(adjusted) != 0) {
                if (start) center_under_global = true;
                if (pokes_above) return at_surface;
            }
        }
        lowest_surface = std::min(lowest_surface, surface);
    }
    const i32 level = compute_surface_level(x, y, z, global, lowest_surface, center_under_global);
    return {level, compute_fluid_type(x, y, z, global, level)};
}

i32 Aquifer::compute_surface_level(i32 x, i32 y, i32 z, const FluidStatus& global, i32 lowest_surface, bool center_under_global) {
    double partially, fully;
    if (static_cast<double>(sampler_.value(wg::root::kAquiferExclusion, x, y, z)) > 0.0) {
        partially = -1.0;
        fully = -1.0;
    } else {
        const i32 below_surface = lowest_surface + 8 - y;
        const double factor = center_under_global ? clamped_map(static_cast<double>(below_surface), 0.0, 64.0, 1.0, 0.0) : 0.0;
        const double noise =
            clamp(static_cast<double>(sampler_.value(wg::root::kAquiferFluidLevelFloodednessNoise, x, y, z)), -1.0, 1.0);
        const double fully_threshold = map(factor, 1.0, 0.0, -0.3, 0.8);
        const double partially_threshold = map(factor, 1.0, 0.0, -0.8, 0.4);
        partially = noise - partially_threshold;
        fully = noise - fully_threshold;
    }
    if (fully > 0.0) return global.level;
    if (partially > 0.0) return randomized_surface_level(x, y, z, lowest_surface);
    return kWayBelowMinY;
}

// computeRandomizedFluidSurfaceLevel: a level of its own, the same over 16 x 40 x 16 blocks.
i32 Aquifer::randomized_surface_level(i32 x, i32 y, i32 z, i32 lowest_surface) {
    const i32 cx = floor_div(x, 16), cy = floor_div(y, 40), cz = floor_div(z, 16);
    const i32 middle = cy * 40 + 20;
    const double spread = static_cast<double>(sampler_.value(wg::root::kAquiferFluidLevelSpreadNoise, cx, cy, cz) * 10.0f);
    const i32 quantized = floor_int(spread / 3.0) * 3;  // Mth.quantize(spread, 3)
    return std::min(lowest_surface, middle + quantized);
}

// computeFluidType: deep down, and where the noise says, a local level is of lava.
u16 Aquifer::compute_fluid_type(i32 x, i32 y, i32 z, const FluidStatus& global, i32 level) {
    if (level <= -10 && level != kWayBelowMinY && global.fluid != lava_) {
        const i32 cx = floor_div(x, 64), cy = floor_div(y, 40), cz = floor_div(z, 64);
        const double noise = static_cast<double>(sampler_.value(wg::root::kAquiferLavaNoise, cx, cy, cz));
        if (std::fabs(noise) > 0.3) return lava_;
    }
    return global.fluid;
}

// calculatePressure: how much there is against a block between two cells being open - a wall
// where their levels differ, thickest between the levels.
double Aquifer::calculate_pressure(i32 x, i32 y, i32 z, double& barrier_noise, const FluidStatus& a, const FluidStatus& b) {
    const u16 type1 = a.at(y), type2 = b.at(y);
    if ((type1 == lava_ && type2 == water_) || (type1 == water_ && type2 == lava_)) return 2.0;
    const i32 difference = std::abs(a.level - b.level);
    if (difference == 0) return 0.0;
    const double average = 0.5 * static_cast<double>(a.level + b.level);
    const double above_average = static_cast<double>(y) + 0.5 - average;
    const double base = static_cast<double>(difference) / 2.0;
    const double towards_middle = base - std::fabs(above_average);
    double gradient;
    if (above_average > 0.0) {
        const double center = 0.0 + towards_middle;
        gradient = center > 0.0 ? center / 1.5 : center / 2.5;
    } else {
        const double center = 3.0 + towards_middle;
        gradient = center > 0.0 ? center / 3.0 : center / 10.0;
    }
    double noise;
    if (gradient < -2.0 || gradient > 2.0) {
        noise = 0.0;
    } else {
        // (one sample a block, whichever pair of cells asks first)
        if (std::isnan(barrier_noise))
            barrier_noise = static_cast<double>(sampler_.value(wg::root::kAquiferBarrierNoise, x, y, z));
        noise = barrier_noise;
    }
    return 2.0 * (noise + gradient);
}

}  // namespace worldgen
