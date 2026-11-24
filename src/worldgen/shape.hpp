// SPDX-License-Identifier: GPL-3.0-or-later
// BlockState.updateShape, for what world generation calls it on: the blocks at the edge of a tree
// (StructureTemplate.updateShapeAtEdge, at the end of TreeFeature.place) - a leaf asks for a tick, a
// flower whose ground a root took goes, water beside a new block is let flow.
//
// A hundred classes have the method; which of them a block has is a table (ft::kShapeOf: gamedata's
// owner of the hook), and what it does is a case here, written from the class and named after it.
// Most classes cannot stand beside a tree in a world of terrain and features (rails, doors,
// redstone); they have no case, and whatever would call one is counted
// (`unported`, Region::Counts::unported_shapes) - tests and probes hold that at nothing.
//
// A template over whoever can say what block is where and take a tick: the generator's Region.
#pragma once

#include <vector>

#include "worldgen/props.hpp"
#include "worldgen/survive.hpp"

namespace worldgen {

// LeavesBlock.getDistanceAt: 0 for what keeps leaves alive, a leaf's own distance, 7 for all else -
// and getOptionalDistanceAt: -1 where there is none to speak of.
inline i32 optional_leaf_distance(u32 state) {
    namespace ft = proto::gen::worldgen::feature;
    if (state_in(ft::kSetKeepsLeaves, state)) return 0;
    if (prop::has(state, ft::Prop::kDistance)) return static_cast<i32>(prop::get(state, ft::Prop::kDistance)) + 1;
    return -1;
}

// MossyCarpetBlock.getUpdatedState: a carpet of pale moss with the sides it sends up the walls
// beside it - low, or tall under more of it, or none where there is nothing to climb. A side by
// its direction (kMossyWall); none, low, tall are 0, 1, 2.
inline constexpr proto::gen::worldgen::feature::Prop kMossyWall[6] = {
    proto::gen::worldgen::feature::Prop::kBottom,    proto::gen::worldgen::feature::Prop::kBottom,
    proto::gen::worldgen::feature::Prop::kWallNorth, proto::gen::worldgen::feature::Prop::kWallSouth,
    proto::gen::worldgen::feature::Prop::kWallWest,  proto::gen::worldgen::feature::Prop::kWallEast};
template <class Level>
u16 mossy_carpet_updated(Level& level, u16 state, i32 x, i32 y, i32 z, bool create_sides) {
    namespace ft = proto::gen::worldgen::feature;
    namespace blk = proto::gen::block;
    using ft::Prop;
    constexpr u32 kHorizontal[4] = {world::kNorth, world::kEast, world::kSouth, world::kWest};
    const u32 carpet = world::block_of(blk::kPaleMossCarpet);
    bool have_above = false, have_below = false;
    u16 above = 0, below = 0;
    const bool base = prop::flag(state, Prop::kBottom);
    create_sides |= base;
    for (const u32 direction : kHorizontal) {
        const Prop wall = kMossyWall[direction];
        u32 side = 0;
        if (state_in(ft::kSetVineAttaches[direction], level.block(x + world::kStepX[direction], y, z + world::kStepZ[direction])))
            side = create_sides ? 1u : prop::get(state, wall);
        if (side == 1) {
            if (!have_above) above = level.block(x, y + 1, z), have_above = true;
            if (world::block_of(above) == carpet && prop::get(above, wall) != 0 && !prop::flag(above, Prop::kBottom)) side = 2;
            if (!base) {
                if (!have_below) below = level.block(x, y - 1, z), have_below = true;
                if (world::block_of(below) == carpet && prop::get(below, wall) == 0) side = 0;
            }
        }
        state = prop::with(state, wall, side);
    }
    return state;
}

// SpeleothemBlock: a spike of dripstone or sulfur. Its tip points up (0) or down (1); it is a tip
// where two meet, a tip, the piece behind a tip, a middle piece, the base (0 .. 4).
inline bool speleothem_pointing(u32 state, u32 tip) {  // isSpeleothemWithDirection
    namespace ft = proto::gen::worldgen::feature;
    return state_in(ft::kSetSpeleothems, state) &&
           (prop::get(state, ft::Prop::kVerticalDirection) == 1 ? world::kDown : world::kUp) == tip;
}
// isValidSpeleothemPlacement: on a whole face, or on more of itself that points the same way.
template <class Level>
bool speleothem_stays(Level& level, u32 block, i32 x, i32 y, i32 z, u32 tip) {
    namespace ft = proto::gen::worldgen::feature;
    const u16 behind = level.block(x, y - world::kStepY[tip], z);
    return state_in(ft::kSetSturdy[tip], behind) || (speleothem_pointing(behind, tip) && world::block_of(behind) == block);
}
// calculateSpeleothemThickness
template <class Level>
u32 speleothem_thickness(Level& level, u32 block, i32 x, i32 y, i32 z, u32 tip, bool merge_opposing_tips) {
    namespace ft = proto::gen::worldgen::feature;
    enum : u32 { kTipMerge, kTip, kFrustum, kMiddle, kBase };
    const u32 base = world::opposite(tip);
    const u16 in_front = level.block(x, y + world::kStepY[tip], z);
    if (speleothem_pointing(in_front, base) && world::block_of(in_front) == block)
        return merge_opposing_tips || prop::get(in_front, ft::Prop::kThickness) == kTipMerge ? kTipMerge : kTip;
    if (!speleothem_pointing(in_front, tip)) return kTip;
    if (prop::get(in_front, ft::Prop::kThickness) <= kTip) return kFrustum;
    return speleothem_pointing(level.block(x, y - world::kStepY[tip], z), tip) ? kMiddle : kBase;
}

// `level`: block(x, y, z), raw_brightness(x, y, z), schedule_tick(x, y, z, type, fluid),
// has_block_tick(x, y, z, block) - is a tick of that block asked for there - and
// reset_countdown(x, y, z): the potent sulfur there reaches for its block entity.
// `direction`: from the block to the neighbour whose state is `neighbour`.
template <class Level>
u16 update_shape(Level& level, u16 state, i32 x, i32 y, i32 z, u32 direction, u16 neighbour, u64& unported) {
    namespace ft = proto::gen::worldgen::feature;
    namespace blk = proto::gen::block;
    using ft::Prop;
    using ft::ShapeOp;
    const u32 block = world::block_of(state);
    const ShapeOp op = ft::kShapeOf[block];
    if (op == ShapeOp::kNone) return state;
    // BlockState.canSurvive, for the rules that ask it. (A rule that is not ported: counted, and the block stays.)
    const auto survives_here = [&]() {
        const u16 row = ft::kShapeSurvive[ft::kShapeSurviveAt[block] + (state - world::block_info(block).first_state)];
        if (row == 0xFFFF) {
            ++unported;
            return true;
        }
        return survives(level, ft::kSurvive[row], x, y, z);
    };
    const auto water_tick = [&]() { level.schedule_tick(x, y, z, world::fluid::kWater, true); };
    const auto block_tick = [&]() { level.schedule_tick(x, y, z, block, false); };
    switch (op) {
        case ShapeOp::kNone: return state;
        case ShapeOp::kLeaves: {
            if (prop::flag(state, Prop::kWaterlogged)) water_tick();
            const i32 at_neighbour = optional_leaf_distance(neighbour);
            const i32 from = (at_neighbour < 0 ? 7 : at_neighbour) + 1;
            if (from != 1 || static_cast<i32>(prop::get(state, Prop::kDistance)) + 1 != from) block_tick();
            return state;
        }
        case ShapeOp::kSnowy:
            if (direction != world::kUp) return state;
            return prop::with_flag(state, Prop::kSnowy, state_in(ft::kSetSnowySetting, neighbour));
        case ShapeOp::kSurviveOrAir: return survives_here() ? state : world::kAir;
        case ShapeOp::kDoublePlant: {
            // The other half is gone or is not this plant's: so is this one.
            const u32 half = prop::get(state, Prop::kHalf);  // upper, lower
            if (direction < 2 && (half == 1) == (direction == world::kUp)) {
                if (world::block_of(neighbour) != block || prop::get(neighbour, Prop::kHalf) == half) return world::kAir;
            }
            if (half == 1 && direction == world::kDown && !survives_here()) return world::kAir;
            return survives_here() ? state : world::kAir;
        }
        case ShapeOp::kSeagrass: {
            if (!survives_here()) return world::kAir;
            water_tick();
            return state;
        }
        case ShapeOp::kLiquid:
            if (state_in(ft::kSetFluidSource, state) || state_in(ft::kSetFluidSource, neighbour))
                level.schedule_tick(x, y, z, world::fluid_type(world::fluid_of(state)), true);
            if (direction == world::kDown && state_in(ft::kSetBubbleColumnIn, state) &&
                state_in(ft::kSetBubbleColumnBelow, neighbour))
                block_tick();
            return state;
        case ShapeOp::kFalling: block_tick(); return state;
        case ShapeOp::kHugeMushroom:
            // A side that has the same mushroom against it is an inside.
            if (world::block_of(neighbour) != block) return state;
            return prop::with_flag(state, prop::side_of(direction), false);
        case ShapeOp::kVine: {
            if (direction == world::kDown) return state;
            // VineBlock.getUpdatedState: a side stays while it has something to hold on to - a
            // block's face, or the same side of a vine above.
            u16 made = state;
            const u16 above = level.block(x, y + 1, z);
            if (prop::flag(made, Prop::kUp))
                made = prop::with_flag(made, Prop::kUp, state_in(ft::kSetVineAttaches[world::kDown], above));
            constexpr u32 kHorizontal[4] = {world::kNorth, world::kEast, world::kSouth, world::kWest};
            bool any = prop::flag(made, Prop::kUp);
            for (const u32 d : kHorizontal) {
                const Prop side = prop::side_of(d);
                if (!prop::flag(made, side)) continue;
                const bool held = state_in(ft::kSetVineAttaches[d], level.block(x + world::kStepX[d], y, z + world::kStepZ[d])) ||
                                  (world::block_of(above) == block && prop::flag(above, side));
                made = prop::with_flag(made, side, held);
                any |= held;
            }
            return any ? made : static_cast<u16>(world::kAir);
        }
        case ShapeOp::kCocoa:
            if (direction == prop::direction_of(prop::get(state, Prop::kHorizontalFacing)) && !survives_here())
                return world::kAir;
            return state;
        case ShapeOp::kShelfMushroom:
            if (direction == world::opposite(prop::direction_of(prop::get(state, Prop::kHorizontalFacing))) && !survives_here())
                return world::kAir;
            return state;
        case ShapeOp::kMangrovePropagule:
            if (prop::flag(state, Prop::kWaterlogged)) water_tick();
            return survives_here() ? state : world::kAir;
        case ShapeOp::kWaterTick:
            if (prop::flag(state, Prop::kWaterlogged)) water_tick();
            return state;
        case ShapeOp::kHangingMoss: {
            const u16 above = level.block(x, y + 1, z);
            if (!(state_in(ft::kSetVineAttaches[world::kUp], above) || world::block_of(above) == block)) block_tick();
            return prop::with_flag(state, Prop::kTip, world::block_of(level.block(x, y - 1, z)) != block);
        }
        case ShapeOp::kSurviveOrTick:
            if (!survives_here()) block_tick();
            return state;
        case ShapeOp::kBamboo:
            if (!survives_here()) block_tick();
            if (direction == world::kUp && world::block_of(neighbour) == block &&
                prop::get(neighbour, Prop::kAge1) > prop::get(state, Prop::kAge1))
                return prop::with(state, Prop::kAge1, (prop::get(state, Prop::kAge1) + 1) % 2);  // BlockState.cycle
            return state;
        case ShapeOp::kBambooSapling:
            if (!survives_here()) return world::kAir;
            if (direction == world::kUp && world::block_of(neighbour) == world::block_of(blk::kBamboo)) return blk::kBamboo;
            return state;
        case ShapeOp::kTick: block_tick(); return state;
        case ShapeOp::kMossyCarpet: {
            if (!survives_here()) return world::kAir;
            const u16 made = mossy_carpet_updated(level, state, x, y, z, false);
            // hasFaces: the carpet itself, or a side.
            bool any = prop::flag(made, Prop::kBottom);
            for (u32 d = 2; d < 6; ++d) any |= prop::get(made, kMossyWall[d]) != 0;
            return any ? made : static_cast<u16>(world::kAir);
        }
        case ShapeOp::kSpeleothem: {
            if (prop::flag(state, Prop::kWaterlogged)) water_tick();
            if (direction != world::kUp && direction != world::kDown) return state;
            const u32 tip = prop::get(state, Prop::kVerticalDirection) == 1 ? world::kDown : world::kUp;
            // (One that hangs and is about to fall is left as it is.)
            if (tip == world::kDown && level.has_block_tick(x, y, z, block)) return state;
            if (direction == world::opposite(tip) && !speleothem_stays(level, block, x, y, z, tip)) {
                block_tick();  // (in two ticks if it hangs, in one if it stands: a chunk in the making keeps no delay)
                return state;
            }
            const bool merge = prop::get(state, Prop::kThickness) == 0;
            return prop::with(state, Prop::kThickness, speleothem_thickness(level, block, x, y, z, tip, merge));
        }
        case ShapeOp::kPotentSulfur: {
            // PotentSulfurBlock.validBlockState: dry without a source of water on it; under water a geyser
            // by what lies below - lava: without end; magma: now and then - or merely wet.
            enum : u32 { kDry, kWet, kDormant, kErupting, kContinuous };
            if (!state_in(ft::kSetWaterSource, level.block(x, y + 1, z)))
                return prop::with(state, Prop::kPotentSulfurState, kDry);
            const u16 below = level.block(x, y - 1, z);
            if (state_in(ft::kSetContinuousGeyser, below) && state_in(ft::kSetSourceIfFluid, below))
                return prop::with(state, Prop::kPotentSulfurState, kContinuous);
            if (state_in(ft::kSetPeriodicGeyser, below) && state_in(ft::kSetSourceIfFluid, below)) {
                const u32 now = prop::get(state, Prop::kPotentSulfurState);
                if (now != kErupting && now != kDormant) level.reset_countdown(x, y, z);
                return now == kErupting ? state : prop::with(state, Prop::kPotentSulfurState, kDormant);
            }
            return prop::with(state, Prop::kPotentSulfurState, kWet);
        }
        case ShapeOp::kUnported: break;
    }
    ++unported;
    return state;
}

// Block.updateFromNeighbourShapes: a block told of all six of its neighbours, in the order shapes
// are asked in - west, east, north, south, down, up.
template <class Level>
u16 update_from_neighbour_shapes(Level& level, u16 state, i32 x, i32 y, i32 z, u64& unported) {
    constexpr u32 kOrder[6] = {world::kWest, world::kEast, world::kNorth, world::kSouth, world::kDown, world::kUp};
    for (const u32 direction : kOrder) {
        const u16 neighbour =
            level.block(x + world::kStepX[direction], y + world::kStepY[direction], z + world::kStepZ[direction]);
        state = update_shape(level, state, x, y, z, direction, neighbour, unported);
    }
    return state;
}

// BitSetDiscreteVoxelShape: a box of blocks, and which of them something set.
struct ShapeBox {
    i32 x0, y0, z0, x1, y1, z1;
    bool inside(i32 x, i32 y, i32 z) const { return x >= x0 && x <= x1 && z >= z0 && z <= z1 && y >= y0 && y <= y1; }
    i32 xs() const { return x1 - x0 + 1; }
    i32 ys() const { return y1 - y0 + 1; }
    i32 zs() const { return z1 - z0 + 1; }
    usize cells() const { return static_cast<usize>(xs()) * static_cast<usize>(ys()) * static_cast<usize>(zs()); }
};
struct ShapeBits {
    std::vector<u64>& bits;
    const ShapeBox& box;
    usize index(i32 x, i32 y, i32 z) const {
        return (static_cast<usize>(x - box.x0) * static_cast<usize>(box.ys()) + static_cast<usize>(y - box.y0)) *
                   static_cast<usize>(box.zs()) +
               static_cast<usize>(z - box.z0);
    }
    void fill(i32 x, i32 y, i32 z) {
        const usize i = index(x, y, z);
        bits[i >> 6] |= u64{1} << (i & 63);
    }
    bool full(i32 x, i32 y, i32 z) const {
        const usize i = index(x, y, z);
        return bits[i >> 6] >> (i & 63) & 1;
    }
};

// StructureTemplate.updateShapeAtEdge: every face between a block that was set and one that was
// not - the block inside is told of its neighbour, then the neighbour of it. The faces in the
// order of DiscreteVoxelShape.forAllFaces: along z for every (x, y), along y for every (z, x),
// along x for every (y, z). `flags`: what a block that changes is set with (the caller's, less
// the bit that tells neighbours). `level`: update_shape's, and set_block and note_unported_shapes.
template <class Level>
void update_shape_at_edge(Level& level, const ShapeBits& shape, u32 flags) {
    const ShapeBox& box = shape.box;
    const auto face = [&](i32 x, i32 y, i32 z, u32 direction) {
        const i32 nx = x + world::kStepX[direction], ny = y + world::kStepY[direction], nz = z + world::kStepZ[direction];
        const u16 state = level.block(x, y, z), beside = level.block(nx, ny, nz);
        u64 unported = 0;
        const u16 made = update_shape(level, state, x, y, z, direction, beside, unported);
        if (made != state) level.set_block(x, y, z, made, flags);
        const u16 made_beside = update_shape(level, beside, nx, ny, nz, world::opposite(direction), made, unported);
        if (made_beside != beside) level.set_block(nx, ny, nz, made_beside, flags);
        if (unported) level.note_unported_shapes(unported);
    };
    const auto run = [&](u32 a_size, u32 b_size, u32 c_size, u32 negative, auto&& at) {
        for (u32 a = 0; a < a_size; ++a)
            for (u32 b = 0; b < b_size; ++b) {
                bool last = false;
                for (u32 c = 0; c <= c_size; ++c) {
                    const bool full = c != c_size && at(a, b, c, false, 0);
                    if (!last && full) at(a, b, c, true, negative);
                    if (last && !full) at(a, b, c - 1, true, negative ^ 1);
                    last = full;
                }
            }
    };
    const u32 xs = static_cast<u32>(box.xs()), ys = static_cast<u32>(box.ys()), zs = static_cast<u32>(box.zs());
    const auto cell = [&](u32 x, u32 y, u32 z, bool tell, u32 direction) {
        const i32 wx = box.x0 + static_cast<i32>(x), wy = box.y0 + static_cast<i32>(y), wz = box.z0 + static_cast<i32>(z);
        if (!tell) return shape.full(wx, wy, wz);
        face(wx, wy, wz, direction);
        return true;
    };
    run(xs, ys, zs, world::kNorth, [&](u32 a, u32 b, u32 c, bool tell, u32 d) { return cell(a, b, c, tell, d); });
    run(zs, xs, ys, world::kDown, [&](u32 a, u32 b, u32 c, bool tell, u32 d) { return cell(b, c, a, tell, d); });
    run(ys, zs, xs, world::kWest, [&](u32 a, u32 b, u32 c, bool tell, u32 d) { return cell(c, a, b, tell, d); });
}

}  // namespace worldgen
