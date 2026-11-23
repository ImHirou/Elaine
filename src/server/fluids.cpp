// SPDX-License-Identifier: GPL-3.0-or-later
// Fluids flow: the scheduled ticks of fluids, and what one does.
//
// Read from the 26.3 jar; each function names the method it follows:
//   ServerLevel.tick (fluidTicks.tick, after the blocks'), ServerLevel.tickFluid
//   FlowingFluid.tick, spread, spreadToSides, spreadTo; WaterFluid (its numbers,
//   beforeDestroyingBlock, canBeReplacedWith)
//   LiquidBlock.onPlace, neighborChanged, updateShape (in server/block_updates.cpp: the hooks
//   that ask for a fluid's tick)
// What a fluid asks of the world before it moves - getNewLiquid, getSpread and the rest - is
// world/fluid.hpp: functions of the blocks alone. What it then does is here, and all of it is
// Game::set_block: a cell of fluid is a block (FluidState.createLegacyBlock), so its neighbours
// hear of every level it gains or loses, and the next cell asks for its own tick when it does
// (LiquidBlock.updateShape, onPlace). A flow is nothing but that chain.
//
// The pace: a cell that changes is ticked again `tick_delay` game ticks later (water: 5), so
// water takes five ticks a block, and a source that is taken away drains at the same pace -
// each cell finds, a step after its feeder, that nothing feeds it.
//
// What is NOT here:
//   * lava. Its block has no hooks yet and nothing schedules a tick for it: the lava of an
//     imported world lies as it was saved, and water does not flow into it;
//   * water in a block that holds it (a waterlogged slab): it does not flow out, and none flows
//     in (world::can_hold_specific);
//   * bubble columns (LiquidBlock.tick, tryScheduleBubbleBlockColumn): water over soul sand or
//     magma is water;
//   * what a block that is not placeable would drop when water washes it away: such a block of
//     an imported world goes without a drop (BlockInfo::drop is there for placeable blocks).
#include "server/game.hpp"
#include "world/fluid.hpp"

namespace server {

using namespace proto;
using world::BlockState;
using world::FluidState;
namespace fluid = world::fluid;
namespace st = world::state;

// WaterFluid's and LavaFluid's answers, as far as a setting decides one (canConvertToSource is
// a game rule in vanilla).
world::FluidRules Game::fluid_rules(u32 family) const {
    DCHECK(family == fluid::kWaterFamily);
    (void)family;
    return world::water_rules(s_.water_source_conversion);
}

// ServerLevel.tick: fluidTicks.tick(gameTime, 65536, this::tickFluid) - after the blocks' ticks.
void Game::run_fluid_ticks() {
    world::LevelTicks& ticks = world_.fluid_ticks();
    ticks.collect(static_cast<i64>(time_));
    world::LevelTicks::Due due;
    while (ticks.next(due)) tick_fluid({due.x, due.y, due.z}, due.type);
    ticks.finish();
}

// ServerLevel.tickFluid, with FlowingFluid.tick inside it: a tick is for the fluid it was asked
// for - a cell that has become a source, or dry, since then is not ticked by it. A cell that
// flows first becomes what its neighbours make of it (it rises, sinks or dries up, and asks
// for its next tick), then the fluid goes on from it: down if it can, else to its sides.
void Game::tick_fluid(BlockPos pos, u32 type) {
    BlockState state = world_.block(pos.x, pos.y, pos.z);
    FluidState held = world::fluid_of(state);
    if (world::fluid_type(held) != type) return;  // FluidState.is(type): this fluid exactly
    if (world::fluid_family(held) != fluid::kWaterFamily) return;
    const world::FluidRules r = fluid_rules(world::fluid_family(held));
    if (!world::fluid_source(held)) {
        const FluidState becomes = world::new_liquid(world_, r, pos.x, pos.y, pos.z, state);
        const i32 delay = r.tick_delay;  // FlowingFluid.getSpreadDelay
        if (!becomes) {
            held = becomes;
            state = world::kAir;
            set_block(pos, state);
        } else if (becomes != held) {
            held = becomes;
            state = world::fluid_block(held);
            set_block(pos, state);
            schedule_fluid_tick(pos, world::fluid_type(held), delay);
        }
    }
    // (With what the tick made of the cell, whatever a hook has made of it since: vanilla's
    // spread() is handed the state the tick computed.)
    spread_fluid(r, pos, state, held);
}

// FlowingFluid.spread: down, if the block below takes the fluid - and then to the sides as well
// only from between three sources; else to the sides, unless the cell is a flowing one over a
// hole it has not filled.
void Game::spread_fluid(const world::FluidRules& r, BlockPos pos, BlockState state, FluidState held) {
    if (!held) return;
    const BlockPos below{pos.x, pos.y - 1, pos.z};
    const BlockState under = world_.block(below.x, below.y, below.z);
    if (world::can_maybe_pass_through(r, state, world::kDown, under)) {
        const FluidState becomes = world::new_liquid(world_, r, below.x, below.y, below.z, under);
        const u32 type = world::fluid_type(becomes);
        if (world::can_be_replaced_with(world_, world::fluid_of(under), below.x, below.y, below.z, type, world::kDown) &&
            world::can_hold_specific(under, type)) {
            spread_to(r, below, under, world::kDown, becomes);
            if (world::source_neighbor_count(world_, r, pos.x, pos.y, pos.z) >= 3) spread_to_sides(r, pos, state, held);
            return;
        }
    }
    if (world::fluid_source(held) || !world::water_hole(r, state, under)) spread_to_sides(r, pos, state, held);
}

// FlowingFluid.spreadToSides: a cell with something left to give - one level less than its own,
// seven below a falling column - gives it to the sides getSpread chose.
void Game::spread_to_sides(const world::FluidRules& r, BlockPos pos, BlockState state, FluidState held) {
    const i32 amount = world::fluid_falling(held) ? 7 : static_cast<i32>(world::fluid_amount(held)) - r.drop_off;
    if (amount <= 0) return;
    world::Spread ways;
    world::spread(world_, r, pos.x, pos.y, pos.z, state, ways);
    for (u32 i = 0; i < ways.n; ++i) {
        const u32 dir = ways.dir[i];
        const BlockPos to{pos.x + world::kStepX[dir], pos.y, pos.z + world::kStepZ[dir]};
        // (The block that is there now: an earlier one of these may have changed it.)
        spread_to(r, to, world_.block(to.x, to.y, to.z), dir, ways.fluid[i]);
    }
}

// FlowingFluid.spreadTo: the fluid arrives. A block that holds fluids by itself is handed it
// (LiquidBlockContainer.placeLiquid); any other block that let it in is washed away -
// WaterFluid.beforeDestroyingBlock: it drops what its loot table gives, with no sound and no
// particles of a block breaking - and the fluid's own block takes its place.
void Game::spread_to(const world::FluidRules& r, BlockPos pos, BlockState state, u32 dir, FluidState becomes) {
    (void)r, (void)dir;
    if (st::kFluidFacts[state] & st::kFluidContainer) return;  // (none gets here: world::can_hold_specific)
    if (!world::is_air(state)) drop_resources(state, pos);
    set_block(pos, world::fluid_block(becomes));
}

// LiquidBlock.shouldSpreadLiquid: lava beside water becomes stone of a kind instead of flowing
// on; water always flows.
bool Game::should_spread_liquid(BlockPos pos, BlockState state) {
    (void)pos, (void)state;
    return true;
}

// LiquidBlock.onPlace and neighborChanged: the fluid of the block asks for its tick.
void Game::liquid_changed(BlockState state, BlockPos pos) {
    if (!should_spread_liquid(pos, state)) return;
    const FluidState held = world::fluid_of(state);
    schedule_fluid_tick(pos, world::fluid_type(held), fluid_rules(world::fluid_family(held)).tick_delay);
}

// LiquidBlock.updateShape: a source, or a cell beside which a source has appeared, is ticked.
void Game::liquid_shape_changed(BlockState state, BlockPos pos, BlockState neighbour) {
    const FluidState held = world::fluid_of(state);
    if (!world::fluid_source(held) && !world::fluid_source(world::fluid_of(neighbour))) return;
    schedule_fluid_tick(pos, world::fluid_type(held), fluid_rules(world::fluid_family(held)).tick_delay);
}

}  // namespace server
