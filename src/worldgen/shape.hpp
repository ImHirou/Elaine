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

// `level`: block(x, y, z), raw_brightness(x, y, z), schedule_tick(x, y, z, type, fluid).
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
        case ShapeOp::kUnported: break;
    }
    ++unported;
    return state;
}

}  // namespace worldgen
