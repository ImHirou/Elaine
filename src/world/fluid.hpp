// SPDX-License-Identifier: GPL-3.0-or-later
// A fluid that flows: what FlowingFluid (26.3) asks of the world before it moves.
//
// Water and lava are one algorithm with different numbers (FluidRules). A cell of fluid is ticked
// (server/fluids.cpp: FlowingFluid.tick), and these are its questions - what should this cell
// hold, by what stands around it (getNewLiquid); may the fluid leave through that side
// (canPassThroughWall, canHoldAnyFluid); which of the four ways leads down soonest (getSpread,
// getSlopeDistance, isWaterHole). They only read: everything here is a function of the blocks,
// so it can be held against the long way in a unit test. What a tick then changes is the game's
// (Game::set_block).
//
// The answers are table loads. A FluidState is its id (state::kFluid, fluid::k*: mcgen from
// gamedata); "may a fluid pass between these two blocks" is a bit of state::kFlowCovers, made
// from Shapes.mergedFaceOccludes over every pair of collision shapes; "does this block hold a
// fluid" is a bit of state::kFluidFacts (a tag and an interface in vanilla).
//
// Vanilla allocates per spread: an EnumMap for the answer, two hash maps keyed by a short for
// the states and the holes it has looked at (FlowingFluid.SpreadContext), and it keeps a
// 200-entry map of state pairs for canPassThroughWall. Here the search remembers in two flat
// arrays on the stack - the search of a fluid reaches five cells from where it starts, so a
// square of eleven holds it - and a pair of states is a table.
//
// Tick thread only.
#pragma once

#include "world/block.hpp"
#include "world/dimension.hpp"

namespace world {

// What a family of fluids flows by: WaterFluid's answers, or LavaFluid's.
struct FluidRules {
    u8 family;           // fluid::kWaterFamily, kLavaFamily: Fluid.isSame
    u8 source_type;      // FlowingFluid.getSource(): the fluid a source of it is
    u8 flowing_type;     // FlowingFluid.getFlowing(): the fluid it is when it flows
    bool converts;       // canConvertToSource: two sources beside a cell make a third (a game rule)
    i32 tick_delay;      // Fluid.getTickDelay: game ticks from one step of it to the next
    i32 drop_off;        // FlowingFluid.getDropOff: what it loses of its amount a block sideways
    i32 slope_distance;  // getSlopeFindDistance: how many blocks it looks for a way down

    // FlowingFluid.getFlowing(amount, falling), getSource(falling)
    FluidState flowing(u32 amount, bool falling) const { return fluid::kFlowingOf[family][falling][amount]; }
    FluidState source(bool falling) const { return fluid::kSourceOf[family][falling]; }
    // fluidState.getType().isSame(this): of this family (the empty fluid is of none)
    bool same(FluidState f) const { return fluid_family(f) == family; }
    // FlowingFluid.isSourceBlockOfThisType
    bool source_of_this(FluidState f) const { return same(f) && fluid_source(f); }
};
// The search for a way down looks this many blocks at most (FluidRules::slope_distance): what
// the arrays of a search are made for.
inline constexpr i32 kMaxSlopeDistance = 4;

// WaterFluid: a step every 5 ticks, one level a block, a way down within 4.
constexpr FluidRules water_rules(bool converts) {
    return {fluid::kWaterFamily, fluid::kWater, fluid::kFlowingWater, converts, 5, 1, 4};
}
// LavaFluid: a step every 30 ticks, two levels a block, a way down within 2 - and as water
// where lava is fast (the nether: every 10 ticks, one level, within 4).
constexpr FluidRules lava_rules(bool fast, bool converts) {
    return {fluid::kLavaFamily, fluid::kLava, fluid::kFlowingLava, converts, fast ? 10 : 30, fast ? 1 : 2, fast ? 4 : 2};
}
// LavaFluid.MIN_LEVEL_CUTOFF: lava at least this high in its cell gives way to water.
inline constexpr float kLavaCutoff = 0.44444445f;

// Direction.Plane.HORIZONTAL: the order a fluid looks around in - which decides where it goes
// when two ways are as good.
inline constexpr u8 kFlowOrder[4] = {kNorth, kEast, kSouth, kWest};

// FlowingFluid.getHeight: how high a fluid stands in its cell at (x, y, z) - the whole of it
// under more of the same, else its own height.
float fluid_height(const Dimension& d, FluidState f, i32 x, i32 y, i32 z);

// FlowingFluid.getNewLiquid: what the cell at (x, y, z), which holds `state`, should hold of this
// fluid by what is around it - a source between two sources, a falling column under the fluid,
// one level less than the most beside it, or nothing (0).
FluidState new_liquid(const Dimension& d, const FluidRules& r, i32 x, i32 y, i32 z, BlockState state);

// FlowingFluid.canMaybePassThrough: may the fluid of `from` go to its neighbour `to` in direction
// `dir` - no source of its own is there, the block there gives way to a fluid, and nothing
// stands between the two.
bool can_maybe_pass_through(const FluidRules& r, BlockState from, u32 dir, BlockState to);
// FlowingFluid.canHoldSpecificFluid: a block that holds fluids by itself (a LiquidBlockContainer)
// says which; every other takes whatever passed canMaybePassThrough. `type`: the fluid.
bool can_hold_specific(BlockState state, u32 type);
// FlowingFluid.isWaterHole: would the fluid of `top` fall into `bottom`, the block under it?
bool water_hole(const FluidRules& r, BlockState top, BlockState bottom);
// FlowingFluid.sourceNeighborCount: how many of the four cells beside this one are sources of it.
u32 source_neighbor_count(const Dimension& d, const FluidRules& r, i32 x, i32 y, i32 z);
// FluidState.canBeReplacedWith(level, pos, fluid, direction): may the fluid `with`, arriving in
// direction `dir`, take the place of the fluid `held` at (x, y, z)?
bool can_be_replaced_with(const Dimension& d, FluidState held, i32 x, i32 y, i32 z, u32 with, u32 dir);

// FlowingFluid.getSpread: the ways the fluid of the cell (x, y, z), which holds `state`, goes
// sideways, and what each neighbour becomes - only towards the nearest way down, if there is
// one within reach. In the order they are to be applied in (vanilla walks an EnumMap: north,
// south, west, east - not the order it looked in).
struct Spread {
    u32 n = 0;
    u8 dir[4];
    FluidState fluid[4];
};
void spread(const Dimension& d, const FluidRules& r, i32 x, i32 y, i32 z, BlockState state, Spread& out);

// The same without remembering anything: every state and every hole asked of the world each
// time the search comes by. What the unit test holds spread() to.
namespace slow {
void spread(const Dimension& d, const FluidRules& r, i32 x, i32 y, i32 z, BlockState state, Spread& out);
}

}  // namespace world
