// SPDX-License-Identifier: GPL-3.0-or-later
// Fluids flow: the scheduled ticks of fluids, and what one does.
//
// Read from the 26.3 jar; each function names the method it follows:
//   ServerLevel.tick (fluidTicks.tick, after the blocks'), ServerLevel.tickFluid
//   FlowingFluid.tick, spread, spreadToSides, spreadTo; WaterFluid and LavaFluid (their numbers,
//   beforeDestroyingBlock, getSpreadDelay, LavaFluid.spreadTo)
//   LiquidBlock.onPlace, neighborChanged, updateShape, shouldSpreadLiquid (the hooks that ask
//   for a fluid's tick: called from server/block_updates.cpp)
// What a fluid asks of the world before it moves - getNewLiquid, getSpread and the rest - is
// world/fluid.hpp: functions of the blocks alone. What it then does is here, and all of it is
// Game::set_block: a cell of fluid is a block (FluidState.createLegacyBlock), so its neighbours
// hear of every level it gains or loses, and the next cell asks for its own tick when it does
// (LiquidBlock.updateShape, onPlace). A flow is nothing but that chain.
//
// The pace: a cell that changes is ticked again `tick_delay` game ticks later, so water takes
// five ticks a block and lava thirty, and a source that is taken away drains at the same pace -
// each cell finds, a step after its feeder, that nothing feeds it.
//
// Lava and water: lava that has water beside it or on it turns to stone of a kind where it
// is - obsidian if it was a source, cobblestone if it flowed (LiquidBlock.shouldSpreadLiquid:
// asked when the lava is placed and when a block beside it changes, so it is the lava that
// answers the water's arrival, in the same tick) - and lava that comes down on water makes
// stone of the water (LavaFluid.spreadTo). Each with a hiss: level event 1501.
//
// What is NOT here:
//   * water in a block that holds it (a waterlogged slab): it does not flow out, and none flows
//     in (world::can_hold_specific);
//   * bubble columns (LiquidBlock.tick, tryScheduleBubbleBlockColumn): water over soul sand or
//     magma is water;
//   * lava's random tick (LavaFluid.randomTick: it sets fire to what burns around it): there is
//     no fire;
//   * what a block that is not placeable would drop when water washes it away: such a block of
//     an imported world goes without a drop (BlockInfo::drop is there for placeable blocks).
#include "server/game.hpp"
#include "world/fluid.hpp"

namespace server {

using namespace proto;
using world::BlockState;
using world::FluidState;
namespace blk = proto::gen::block;
namespace block_kind = proto::gen::block_kind;
namespace fluid = world::fluid;
namespace st = world::state;

// WaterFluid's and LavaFluid's answers, as far as a setting or the dimension decides one
// (canConvertToSource is a game rule in vanilla, how fast lava is an attribute of the dimension).
world::FluidRules Game::fluid_rules(u32 family) const {
    return family == fluid::kLavaFamily ? world::lava_rules(world_.fast_lava(), s_.lava_source_conversion)
                                        : world::water_rules(s_.water_source_conversion);
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
    if (!held || world::fluid_type(held) != type) return;  // FluidState.is(type): this fluid exactly
    const world::FluidRules r = fluid_rules(world::fluid_family(held));
    if (!world::fluid_source(held)) {
        const FluidState becomes = world::new_liquid(world_, r, pos.x, pos.y, pos.z, state);
        const i32 delay = spread_delay(r, pos, held, becomes);
        if (!becomes) {
            held = becomes;
            state = world::kAir;
            set_block(pos, state);
        } else if (becomes != held) {
            held = becomes;
            state = world::fluid_block(held);
            set_block(pos, state);
            // (The block has asked for this tick itself by now - LiquidBlock.onPlace, with the
            // fluid's plain delay - and a second one for the same cell is dropped: `delay`
            // counts only for lava that turned to stone in being placed, where it finds no lava.)
            schedule_fluid_tick(pos, world::fluid_type(held), delay);
        }
    }
    // (With what the tick made of the cell, whatever a hook has made of it since: vanilla's
    // spread() is handed the state the tick computed.)
    spread_fluid(r, pos, state, held);
}

// FlowingFluid.getSpreadDelay, and LavaFluid's: lava that rises in its cell is, three times in
// four, in no hurry. (See tick_fluid for what becomes of the number.)
i32 Game::spread_delay(const world::FluidRules& r, BlockPos pos, FluidState old, FluidState becomes) {
    i32 delay = r.tick_delay;
    if (r.family == fluid::kLavaFamily && old && becomes && !world::fluid_falling(old) && !world::fluid_falling(becomes) &&
        world::fluid_height(world_, becomes, pos.x, pos.y, pos.z) > world::fluid_height(world_, old, pos.x, pos.y, pos.z) &&
        random_int(4) != 0)
        delay *= 4;
    return delay;
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

// FlowingFluid.spreadToSides: a cell with something left to give - its own amount less what a
// block costs, seven below a falling column - gives it to the sides getSpread chose.
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
// (LiquidBlockContainer.placeLiquid); any other block that let it in goes - water washes it
// away, and it drops what its loot table gives, with no sound and no particles of a block
// breaking (WaterFluid.beforeDestroyingBlock); lava burns it with a hiss and nothing is left
// (LavaFluid's) - and the fluid's own block takes its place.
// LavaFluid.spreadTo comes first: lava that comes down on water makes stone of it.
void Game::spread_to(const world::FluidRules& r, BlockPos pos, BlockState state, u32 dir, FluidState becomes) {
    const bool lava = r.family == fluid::kLavaFamily;
    if (lava && dir == world::kDown && world::holds_water(world_.block(pos.x, pos.y, pos.z))) {
        // (Of water's own block: water in a block that holds it only hisses.)
        if (world::block_info_of(state).kind == block_kind::kLiquid) set_block(pos, blk::kStone);
        level_event(kLevelEventLavaFizz, pos, 0);
        return;
    }
    if (st::kFluidFacts[state] & st::kFluidContainer) return;  // (none gets here: world::can_hold_specific)
    if (!world::is_air(state)) {
        if (lava)
            level_event(kLevelEventLavaFizz, pos, 0);
        else
            drop_resources(state, pos);
    }
    set_block(pos, world::fluid_block(becomes));
}

// LiquidBlock.shouldSpreadLiquid: water flows on. Lava looks at what is on it and beside it
// first - in LiquidBlock.POSSIBLE_FLOW_DIRECTIONS' order, each turned round: up, north, south,
// west, east - and where there is water it turns to obsidian (a source) or cobblestone (lava
// that flowed); over soul soil, beside blue ice, to basalt. Then it does not flow.
bool Game::should_spread_liquid(BlockPos pos, BlockState state) {
    if (!(fluid::kFlags[world::fluid_of(state)] & fluid::kInLava)) return true;
    static constexpr u8 kLook[5] = {world::kUp, world::kNorth, world::kSouth, world::kWest, world::kEast};
    const bool over_soul_soil = world::block_of(world_.block(pos.x, pos.y - 1, pos.z)) == world::block_of(blk::kSoulSoil);
    for (const u32 dir : kLook) {
        const BlockState beside =
            world_.block(pos.x + world::kStepX[dir], pos.y + world::kStepY[dir], pos.z + world::kStepZ[dir]);
        if (world::holds_water(beside)) {
            const bool source = world::fluid_source(world::fluid_of(world_.block(pos.x, pos.y, pos.z)));
            set_block(pos, source ? blk::kObsidian : blk::kCobblestone);
            level_event(kLevelEventLavaFizz, pos, 0);
            return false;
        }
        if (over_soul_soil && world::block_of(beside) == world::block_of(blk::kBlueIce)) {
            set_block(pos, blk::kBasalt);
            level_event(kLevelEventLavaFizz, pos, 0);
            return false;
        }
    }
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
