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
// A block that holds water by itself (SimpleWaterloggedBlock: a slab, a fence, a chest) is a
// source of it where it stands: its water is ticked when the block's shape is asked
// (Game::update_shape), flows out through what the block's shape leaves open, and is put into it
// where a source would form (place_liquid). The sponge takes water up (try_absorb_water).
//
// What is NOT here:
//   * a block of an imported world that holds water and whose class is not ported (a trapdoor,
//     leaves): it takes no water and gives none, as it does nothing else;
//   * bubble columns (LiquidBlock.tick, tryScheduleBubbleBlockColumn): water over soul sand or
//     magma is water;
//   * lava's random tick (LavaFluid.randomTick: it sets fire to what burns around it): there is
//     no fire;
//   * what a block that is not placeable would drop when water washes it away: such a block of
//     an imported world goes without a drop (BlockInfo::drop is there for placeable blocks).
#include "server/game.hpp"
#include "world/fluid.hpp"

#include PROTO_GEN_ITEMS_HPP
#include PROTO_GEN_STATIC_IDS_HPP

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
    if (st::kFluidFacts[state] & st::kFluidContainer) {
        place_liquid(pos, state, becomes);
        return;
    }
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

// SimpleWaterloggedBlock.placeLiquid (SlabBlock's: not into a double slab): a dry block that is
// handed a source of water is waterlogged, and its water asks for its tick.
bool Game::place_liquid(BlockPos pos, BlockState state, FluidState held) {
    const world::BlockInfo& info = world::block_info_of(state);
    if (!info.wet_stride || world::fluid_of(state) || world::fluid_type(held) != fluid::kWater) return false;
    if (!(st::kFluidFacts[state] & (st::kTakesWater | st::kCreativeOnly))) return false;
    set_block(pos, static_cast<BlockState>(state - info.wet_stride));
    schedule_fluid_tick(pos, fluid::kWater, fluid_rules(fluid::kWaterFamily).tick_delay);
    return true;
}

// BucketPickup.pickupBlock: the fluid is taken out of the block - the bucket it would fill, 0 if
// there is nothing to take. LiquidBlock: a source goes whole, what flows is not taken.
// SimpleWaterloggedBlock: the block is dry, and breaks if it cannot stay (BarrierBlock: at the
// hands of a player in creative mode only).
u32 Game::pickup_block(BlockPos pos, BlockState state, bool creative_player) {
    const world::BlockInfo& info = world::block_info_of(state);
    if (info.kind == block_kind::kLiquid) {
        if (state != info.first_state) return 0;  // LEVEL 0: the source
        set_block(pos, world::kAir);
        return world::fluid_family(world::fluid_of(state)) == fluid::kLavaFamily ? proto::gen::item::kLavaBucket
                                                                                 : proto::gen::item::kWaterBucket;
    }
    if (!info.wet_stride || !world::fluid_of(state)) return 0;
    if ((st::kFluidFacts[state] & st::kCreativeOnly) && !creative_player) return 0;
    set_block(pos, static_cast<BlockState>(state + info.wet_stride));
    if (!can_survive(state, pos)) break_block(pos, true);
    return proto::gen::item::kWaterBucket;
}

// SpongeBlock.tryAbsorbWater (its onPlace and neighborChanged): if there is water to take, it
// is taken, and the sponge is wet.
void Game::try_absorb_water(BlockPos pos) {
    if (!remove_water_around(pos)) return;
    set_block(pos, blk::kWetSponge, kUpdateClients);
    play_sound(gen::sound::kBlockSpongeAbsorb, cb::Sound::kSourceBlocks, pos.x + 0.5, pos.y + 0.5, pos.z + 0.5, 1.0f, 1.0f);
}

// SpongeBlock.removeWaterBreadthFirstSearch, with BlockPos.breadthFirstTraversal written out:
// from the sponge outwards, six steps far and sixty-five cells at most (the sponge is one),
// through the cells that hold water and nothing else - each emptied as it is come to: a source
// and what is waterlogged by pickupBlock, water that flows set to air, kelp and seagrass broken.
// True if anything but the sponge's own cell was taken. A cell is looked at once, and marked
// when it is come to, not when it is queued - so the queue holds a cell as often as it has
// neighbours that were taken, and the order is the queue's: Direction.values() around each.
// (A cell that is emptied tells its neighbours, the sponge among them, which looks again while
// this search is still under way - as vanilla's does: each search has its own queue.)
bool Game::remove_water_around(BlockPos start) {
    constexpr i32 kDepth = 6, kSide = 2 * kDepth + 1;  // SpongeBlock.MAX_DEPTH
    constexpr u32 kCount = 65;                         // MAX_COUNT + 1: the sponge's own cell is one
    struct Node {
        i8 dx, dy, dz;
        u8 depth;
    };
    Node queue[(kCount - 1) * 6 + 1];  // every cell that is taken queues six, and fewer than kCount are taken before it ends
    u64 seen[(kSide * kSide * kSide + 63) / 64] = {};
    u32 head = 0, tail = 0, count = 0;
    queue[tail++] = {0, 0, 0, 0};
    while (head != tail) {
        const Node n = queue[head++];
        const u32 bit = static_cast<u32>(((n.dx + kDepth) * kSide + (n.dy + kDepth)) * kSide + (n.dz + kDepth));
        if (seen[bit >> 6] >> (bit & 63) & 1) continue;
        seen[bit >> 6] |= 1ull << (bit & 63);
        const BlockPos pos{start.x + n.dx, start.y + n.dy, start.z + n.dz};
        if (n.dx | n.dy | n.dz) {
            const BlockState state = world_.block(pos.x, pos.y, pos.z);
            if (!world::holds_water(state)) continue;
            if (!pickup_block(pos, state, false)) {
                const u32 block = world::block_of(state);
                if (world::block_info_of(state).kind == block_kind::kLiquid) {
                    set_block(pos, world::kAir);
                } else if (block == world::block_of(blk::kKelp) || block == world::block_of(blk::kKelpPlant) ||
                           block == world::block_of(blk::kSeagrass) || block == world::block_of(blk::kTallSeagrass)) {
                    drop_resources(state, pos);  // (nothing: what they drop is not known - they are not placeable)
                    set_block(pos, world::kAir);
                } else {
                    continue;
                }
            }
        }
        if (++count >= kCount) return true;
        if (n.depth >= kDepth) continue;
        for (u32 dir = 0; dir < 6; ++dir)
            queue[tail++] = {static_cast<i8>(n.dx + world::kStepX[dir]), static_cast<i8>(n.dy + world::kStepY[dir]),
                             static_cast<i8>(n.dz + world::kStepZ[dir]), static_cast<u8>(n.depth + 1)};
    }
    return count > 1;
}

// WetSpongeBlock.onPlace: where water does not stay (the nether), a wet sponge is dry at once.
void Game::wet_sponge_placed(BlockPos pos) {
    if (!world_.water_evaporates()) return;
    set_block(pos, blk::kSponge);
    level_event(kLevelEventWaterEvaporating, pos, 0);
    play_sound(gen::sound::kBlockWetSpongeDries, cb::Sound::kSourceBlocks, pos.x + 0.5, pos.y + 0.5, pos.z + 0.5, 1.0f,
               (1.0f + random_float() * 0.2f) * 0.7f);
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
