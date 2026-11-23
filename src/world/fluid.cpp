// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/fluid.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace world {

namespace {

// FlowingFluid.SpreadContext: the block states and the holes one spread has looked at, by where
// they are from the cell that spreads - the search stays on its level, so two offsets name a
// cell. With `remember` off it asks the world every time (slow::spread).
struct Search {
    static constexpr i32 kReach = kMaxSlopeDistance + 1;  // the last pass looks one cell past its distance
    static constexpr i32 kSide = 2 * kReach + 1;
    static constexpr u8 kState = 1, kHole = 2, kIsHole = 4;

    const Dimension& d;
    const FluidRules& r;
    i32 ox, y, oz;
    bool remember;
    u8 known[kSide * kSide] = {};
    BlockState states[kSide * kSide];  // (those that `known` says are)

    Search(const Dimension& dim, const FluidRules& rules, i32 x, i32 level, i32 z, bool memory)
        : d(dim), r(rules), ox(x), y(level), oz(z), remember(memory) {}

    u32 index(i32 x, i32 z) const {
        DCHECK(std::abs(x - ox) <= kReach && std::abs(z - oz) <= kReach);
        return static_cast<u32>((z - oz + kReach) * kSide + (x - ox + kReach));
    }
    // SpreadContext.getBlockState
    BlockState state(i32 x, i32 z) {
        const u32 i = index(x, z);
        if (!(known[i] & kState)) {
            states[i] = d.block(x, y, z);
            if (remember) known[i] |= kState;
        }
        return states[i];
    }
    // SpreadContext.isHole: would the fluid fall here? (The block below is asked of the level.)
    bool hole(i32 x, i32 z) {
        const u32 i = index(x, z);
        if (known[i] & kHole) return known[i] & kIsHole;
        const bool is = water_hole(r, state(x, z), d.block(x, y - 1, z));
        if (remember) known[i] |= kHole | (is ? kIsHole : 0);
        return is;
    }
};

// FlowingFluid.canPassThrough: canMaybePassThrough, and the block there takes this fluid as it flows.
bool can_pass_through(const FluidRules& r, BlockState from, u32 dir, BlockState to) {
    return can_maybe_pass_through(r, from, dir, to) && can_hold_specific(to, r.flowing_type);
}

// FlowingFluid.getSlopeDistance: from the cell (x, z), reached in `pass` steps, how many steps to
// the nearest cell the fluid would fall from - 1000 if none is within reach. `from`: the side
// the search came from.
i32 slope_distance(Search& s, i32 x, i32 z, i32 pass, u32 from, BlockState state) {
    i32 lowest = 1000;
    for (const u32 dir : kFlowOrder) {
        if (dir == from) continue;
        const i32 tx = x + kStepX[dir], tz = z + kStepZ[dir];
        const BlockState there = s.state(tx, tz);
        if (!can_pass_through(s.r, state, dir, there)) continue;
        if (s.hole(tx, tz)) return pass;
        if (pass < s.r.slope_distance) lowest = std::min(lowest, slope_distance(s, tx, tz, pass + 1, opposite(dir), there));
    }
    return lowest;
}

void spread_with(const Dimension& d, const FluidRules& r, i32 x, i32 y, i32 z, BlockState state, Spread& out, bool remember) {
    DCHECK(r.slope_distance <= kMaxSlopeDistance);
    Search search(d, r, x, y, z, remember);
    i32 lowest = 1000;
    FluidState found[6] = {};
    bool has[6] = {};
    for (const u32 dir : kFlowOrder) {
        const i32 tx = x + kStepX[dir], tz = z + kStepZ[dir];
        const BlockState there = d.block(tx, y, tz);
        if (!can_maybe_pass_through(r, state, dir, there)) continue;
        const FluidState becomes = new_liquid(d, r, tx, y, tz, there);
        if (!can_hold_specific(there, fluid_type(becomes))) continue;
        const i32 distance = search.hole(tx, tz) ? 0 : slope_distance(search, tx, tz, 1, opposite(dir), there);
        if (distance < lowest) std::fill(std::begin(has), std::end(has), false);
        if (distance <= lowest) {
            if (can_be_replaced_with(d, fluid_of(there), tx, y, tz, fluid_type(becomes), dir)) {
                found[dir] = becomes;
                has[dir] = true;
            }
            lowest = distance;  // (also when the fluid there does not give way: vanilla has it so)
        }
    }
    out.n = 0;
    for (u32 dir = kNorth; dir <= kEast; ++dir) {  // an EnumMap's order: north, south, west, east
        if (!has[dir]) continue;
        out.dir[out.n] = static_cast<u8>(dir);
        out.fluid[out.n++] = found[dir];
    }
}

}  // namespace

FluidState new_liquid(const Dimension& d, const FluidRules& r, i32 x, i32 y, i32 z, BlockState state) {
    u32 highest = 0, sources = 0;
    for (const u32 dir : kFlowOrder) {
        const BlockState beside = d.block(x + kStepX[dir], y, z + kStepZ[dir]);
        const FluidState f = fluid_of(beside);
        if (!r.same(f) || !fluid_passes(state, beside, dir)) continue;
        sources += fluid_source(f);
        highest = std::max(highest, fluid_amount(f));
    }
    if (sources >= 2 && r.converts) {
        const BlockState below = d.block(x, y - 1, z);
        if (is_solid(below) || r.source_of_this(fluid_of(below))) return r.source(false);
    }
    const BlockState above = d.block(x, y + 1, z);
    if (r.same(fluid_of(above)) && fluid_passes(state, above, kUp)) return r.flowing(8, true);
    const i32 amount = static_cast<i32>(highest) - r.drop_off;
    return amount <= 0 ? FluidState{0} : r.flowing(static_cast<u32>(amount), false);
}

bool can_maybe_pass_through(const FluidRules& r, BlockState from, u32 dir, BlockState to) {
    return !r.source_of_this(fluid_of(to)) && (state::kFluidFacts[to] & state::kHoldsFluid) && fluid_passes(from, to, dir);
}

bool can_hold_specific(BlockState state, u32 type) {
    const u32 facts = state::kFluidFacts[state];
    if (!(facts & state::kFluidContainer)) return true;
    // LiquidBlockContainer.canPlaceLiquid(null, ...): SimpleWaterloggedBlock takes a source of
    // water and nothing else - water that flows does not go into a slab.
    return (facts & state::kTakesWater) && type == fluid::kWater;
}

bool water_hole(const FluidRules& r, BlockState top, BlockState bottom) {
    if (!fluid_passes(top, bottom, kDown)) return false;
    if (r.same(fluid_of(bottom))) return true;
    // canHoldFluid(level, bottomPos, bottomState, getFlowing())
    return (state::kFluidFacts[bottom] & state::kHoldsFluid) && can_hold_specific(bottom, r.flowing_type);
}

u32 source_neighbor_count(const Dimension& d, const FluidRules& r, i32 x, i32 y, i32 z) {
    u32 count = 0;
    for (const u32 dir : kFlowOrder) count += r.source_of_this(fluid_of(d.block(x + kStepX[dir], y, z + kStepZ[dir])));
    return count;
}

float fluid_height(const Dimension& d, FluidState f, i32 x, i32 y, i32 z) {
    // hasSameAbove: fluidState.getType().isSame(the fluid above)
    return f && fluid_family(fluid_of(d.block(x, y + 1, z))) == fluid_family(f) ? 1.0f : fluid_own_height(f);
}

namespace {

// Vec3.normalize: nothing, for what is shorter than 1e-5.
ALWAYS_INLINE void normalize(double v[3]) {
    const double length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (length < 9.999999747378752e-6) {
        v[0] = v[1] = v[2] = 0.0;
        return;
    }
    v[0] = v[0] / length, v[1] = v[1] / length, v[2] = v[2] / length;
}

// FlowingFluid.isSolidFace: is the block at (x, y, z) a wall to this fluid on the side `dir`?
bool solid_face(const Dimension& d, u32 family, i32 x, i32 y, i32 z, u32 dir) {
    const BlockState s = d.block(x, y, z);
    if (fluid_family(fluid_of(s)) == family) return false;
    if (dir == kUp) return true;
    if (state::kFluidFacts[s] & state::kIce) return false;
    return face_sturdy(s, dir);
}

}  // namespace

void fluid_flow(const Dimension& d, FluidState f, i32 x, i32 y, i32 z, double out[3]) {
    const u32 family = fluid_family(f);
    const float own = fluid_own_height(f);
    double flow_x = 0, flow_z = 0;
    // FlowingFluid.affectsFlow: nothing, or more of the same
    const auto affects = [&](FluidState n) { return !n || fluid_family(n) == family; };
    for (const u32 dir : kFlowOrder) {
        const i32 nx = x + kStepX[dir], nz = z + kStepZ[dir];
        const BlockState beside = d.block(nx, y, nz);
        const FluidState n = fluid_of(beside);
        if (!affects(n)) continue;
        float height = fluid_own_height(n), distance = 0.0f;
        if (height == 0.0f) {
            if (!(state::kFluidFacts[beside] & state::kBlocksFlow)) {
                const FluidState below = fluid_of(d.block(nx, y - 1, nz));
                if (affects(below)) {
                    height = fluid_own_height(below);
                    if (height > 0.0f) distance = own - (height - 0.8888889f);
                }
            }
        } else if (height > 0.0f) {
            distance = own - height;
        }
        if (distance != 0.0f) {
            flow_x = flow_x + static_cast<double>(static_cast<float>(kStepX[dir]) * distance);
            flow_z = flow_z + static_cast<double>(static_cast<float>(kStepZ[dir]) * distance);
        }
    }
    out[0] = flow_x, out[1] = 0.0, out[2] = flow_z;
    if (fluid_falling(f)) {
        for (const u32 dir : kFlowOrder) {
            const i32 nx = x + kStepX[dir], nz = z + kStepZ[dir];
            if (solid_face(d, family, nx, y, nz, dir) || solid_face(d, family, nx, y + 1, nz, dir)) {
                normalize(out);  // flow.normalize().add(0, -6, 0)
                out[0] = out[0] + 0.0, out[1] = out[1] + -6.0, out[2] = out[2] + 0.0;
                break;
            }
        }
    }
    normalize(out);
}

InWater water_around(const Dimension& d, const Aabb& whole, double x, double eye_y, double z) {
    InWater out;
    const auto floor_of = [](double v) {
        const i32 i = static_cast<i32>(v);
        return v < static_cast<double>(i) ? i - 1 : i;
    };
    const auto ceil_of = [](double v) {
        const i32 i = static_cast<i32>(v);
        return v > static_cast<double>(i) ? i + 1 : i;
    };
    const Aabb box = inflated(whole, -0.001, -0.001, -0.001);  // Entity.getFluidInteractionBox
    const i32 x0 = floor_of(box.min[0]), y0 = floor_of(box.min[1]), z0 = floor_of(box.min[2]);
    const i32 x1 = ceil_of(box.max[0]) - 1, y1 = ceil_of(box.max[1]) - 1, z1 = ceil_of(box.max[2]) - 1;
    if (!(d.holds(x0 - 1, y0, z0 - 1, x1 + 1, y1, z1 + 1) & Dimension::kFluids)) return out;  // hasFluidAndLoaded
    const i32 eye_x = floor_of(x), eye_z = floor_of(z);
    for (i32 cx = x0; cx <= x1; ++cx) {
        for (i32 cy = y0; cy <= y1; ++cy) {
            for (i32 cz = z0; cz <= z1; ++cz) {
                const FluidState f = fluid_of(d.block(cx, cy, cz));
                if (LIKELY(!f) || fluid_family(f) != fluid::kWaterFamily) continue;
                const double bottom = static_cast<double>(cy);
                const float height = fluid_height(d, f, cx, cy, cz);
                if (bottom + static_cast<double>(height) < box.min[1]) continue;
                out.in = true;
                if (cx != eye_x || cz != eye_z || eye_y < bottom) continue;
                // FluidState.getHeightForCamera
                const float camera = fluid_source(f) && face_sturdy(d.block(cx, cy + 1, cz), kDown) ? 1.0f : height;
                if (!(eye_y > bottom + static_cast<double>(camera))) out.eyes = true;
            }
        }
    }
    return out;
}

bool can_be_replaced_with(const Dimension& d, FluidState held, i32 x, i32 y, i32 z, u32 with, u32 dir) {
    switch (fluid_family(held)) {
        // WaterFluid: only what comes from above, and is not water, takes water's place.
        case fluid::kWaterFamily: return dir == kDown && fluid::kFamily[with] != fluid::kWaterFamily;
        // LavaFluid: water takes the place of lava that stands high enough in its cell.
        case fluid::kLavaFamily:
            return fluid_height(d, held, x, y, z) >= kLavaCutoff && fluid::kFamily[with] == fluid::kWaterFamily;
        default: return true;  // EmptyFluid
    }
}

void spread(const Dimension& d, const FluidRules& r, i32 x, i32 y, i32 z, BlockState state, Spread& out) {
    spread_with(d, r, x, y, z, state, out, true);
}

namespace slow {
void spread(const Dimension& d, const FluidRules& r, i32 x, i32 y, i32 z, BlockState state, Spread& out) {
    spread_with(d, r, x, y, z, state, out, false);
}
}  // namespace slow

}  // namespace world
