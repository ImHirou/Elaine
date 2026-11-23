// SPDX-License-Identifier: GPL-3.0-or-later
// A block changes - and what follows from it: the blocks around it hear of it and take the
// shape they have beside it, ticks that were asked for come due, and the random ticks fall.
//
// The rules are vanilla's, read from the 26.3 jar; each function names the method it follows:
//   Level.setBlock, LevelChunk.setBlockState (what a change does, and in which order)
//   CollectingNeighborUpdater (addAndRun, runUpdates, its four kinds of update), NeighborUpdater
//   (UPDATE_ORDER, executeShapeUpdate, executeUpdate), BlockBehaviour.UPDATE_SHAPE_ORDER,
//   BlockStateBase.updateNeighbourShapes, Block.updateOrDestroy / updateFromNeighbourShapes
//   Level.destroyBlock / removeBlock, Block.dropResources / popResource, ServerLevel.levelEvent
//   ServerLevel.tick (tickTime, then blockTicks.tick), tickBlock; LevelTicks is world/ticks.hpp
//   (the fluids' ticks, which run next, and what a fluid does are server/fluids.cpp)
//   ServerChunkCache.tickChunks, ServerLevel.tickChunk, Level.getBlockRandomPos (random ticks)
// and, for the blocks that have a part in any of it:
//   SnowyBlock, SpreadingSnowyBlock (grass, mycelium), NyliumBlock, ChangeOverTimeBlock /
//   WeatheringCopper, BuddingAmethystBlock, AmethystClusterBlock, BaseTorchBlock, WallTorchBlock,
//   RedstoneLampBlock, FallingBlock, ConcretePowderBlock, TntBlock, ChestBlock, CopperChestBlock,
//   BarrelBlock, VegetationBlock and what is one, LiquidBlock, SpongeBlock, WetSpongeBlock (their
//   hooks; what the last three do is server/fluids.cpp), SimpleWaterloggedBlock's part of
//   updateShape (the tick of the water in a block that holds it)
// The blocks whose state is their shape - stairs, fences, panes, walls, lanterns, mushroom
// blocks - answer updateShape in server/block_shapes.cpp.
//
// The order is the point. A neighbour update is not run where it is made: it is put on a stack,
// and what an update makes while it runs is run before what was waiting (depth first, in the
// order made). Redstone - and every machine players build out of blocks that react to each
// other - is that order, so it is kept as it is: records on a stack instead of vanilla's
// objects, and nothing else changed.
//
// A block of a class that is not ported has no hook here (state::kHooks is empty for it): an
// imported world's lever or lava is told of a change and does nothing, as it does nothing else.
//
// What is NOT here:
//   * comparators (Level.updateNeighbourForOutputSignal, affectNeighborsAfterRemoval of the
//     containers), redstone wire's updateIndirectNeighbourShapes: there are none;
//   * tickPrecipitation and tickThunder (ice, snow layers, lightning): there is no weather - nor
//     what rain takes from the sky's light (Level.skyDarken follows the time of day alone);
//   * game rules: random-tick-speed is a setting, blocks and entities always drop.
#include "base/log.hpp"
#include "server/game.hpp"

#include PROTO_GEN_ITEMS_HPP
#include PROTO_GEN_STATIC_IDS_HPP

namespace server {

using namespace proto;
using world::BlockState;
namespace block_kind = proto::gen::block_kind;
namespace block_flag = world::block_flag;
namespace props = proto::gen::props;
namespace st = world::state;

namespace {

constexpr double kLevelEventRange = 64.0;      // ServerLevel.levelEvent
constexpr u64 kPopPickupDelay = 10;            // ItemEntity.setDefaultPickUpDelay
constexpr i32 kFallDelay = 2;                  // FallingBlock.getDelayAfterPlace
constexpr i32 kLampOffDelay = 4;               // RedstoneLampBlock.neighborChanged
constexpr float kWeatherChance = 0.05688889f;  // ChangeOverTimeBlock.changeOverTime
constexpr i32 kWeatherScan = 4;                // ChangeOverTimeBlock.SCAN_DISTANCE

// NeighborUpdater.UPDATE_ORDER and BlockBehaviour.UPDATE_SHAPE_ORDER: not the same order.
constexpr u8 kNeighborOrder[6] = {world::kWest, world::kEast, world::kDown, world::kUp, world::kNorth, world::kSouth};
constexpr u8 kShapeOrder[6] = {world::kWest, world::kEast, world::kNorth, world::kSouth, world::kDown, world::kUp};

BlockPos step(BlockPos p, u32 dir) { return {p.x + world::kStepX[dir], p.y + world::kStepY[dir], p.z + world::kStepZ[dir]}; }

// ---- AmethystClusterBlock's state: facing (north, east, south, west, up, down), waterlogged.
// The four blocks of the family share the layout (mcgen checks each).
namespace cluster = props::amethyst_cluster;
constexpr u8 kClusterFacing[6] = {world::kNorth, world::kEast, world::kSouth, world::kWest, world::kUp, world::kDown};
constexpr u8 kClusterIndex[6] = {5, 4, 0, 2, 3, 1};  // by Direction
u32 cluster_facing(BlockState s, const world::BlockInfo& info) {
    return kClusterFacing[(s - info.first_state) / cluster::kFacingStride];
}
BlockState cluster_state(u32 first, u32 facing, bool waterlogged) {
    return static_cast<BlockState>(first + kClusterIndex[facing] * cluster::kFacingStride + (waterlogged ? 0 : 1));
}
// WallTorchBlock's: facing (north, south, west, east) - Direction's own order.
u32 wall_torch_facing(BlockState s, const world::BlockInfo& info) { return world::kNorth + (s - info.first_state); }
// RedstoneLampBlock's: lit (true, false).
bool lamp_lit(BlockState s) { return s == props::redstone_lamp::kFirst; }
BlockState lamp_state(bool lit) { return static_cast<BlockState>(props::redstone_lamp::kFirst + (lit ? 0 : 1)); }

}  // namespace

// ---------------------------------------------------------------------------- a block is set

// Level.setBlock(pos, state, flags, updateLimit), with LevelChunk.setBlockState inside it: the
// state is stored (world::Dimension: heightmaps, light, the change for the clients); the block
// entity of the block that went goes, and spills what it held; the new block is told it was
// placed, and gets its block entity; the six neighbours hear that a block changed beside them;
// and each of them takes the shape it has beside the new block. Every change of a block the
// game makes goes through here. `flags` are vanilla's (kUpdate*): what a player does is
// kUpdateAll; a command leaves parts of it out.
bool Game::set_block(BlockPos pos, BlockState state, u32 flags, i32 limit) {
    const BlockState old = world_.block(pos.x, pos.y, pos.z);
    if (!world_.set_block(pos.x, pos.y, pos.z, state)) return false;
    DCHECK(!expect_no_change_);  // a section that was counted as having nothing to do did something
    wake_near(pos);              // what lay at rest on it, or beside it
    const u32 old_block = world::block_of(old), new_block = world::block_of(state);
    const world::BlockInfo& was = world::block_info(old_block);
    const world::BlockInfo& now = world::block_info(new_block);
    // A block entity that went with the old block: its entry stays (dead, and empty) until the
    // menus that are open on it have been closed.
    u32 gone_kind = block_kind::kPlain, gone = 0;
    // BlockState.shouldChangedStateKeepBlockEntity: a copper chest that becomes another copper
    // chest - weathered, or the block of the chest it joined - is the chest it was.
    const bool kept = was.kind == block_kind::kChest && now.kind == block_kind::kChest && copper_chest(was) && copper_chest(now);
    if (old_block != new_block && !kept && (world::flags_of(old) & st::kBlockEntity) &&
        world_.remove_block_entity(pos.x, pos.y, pos.z, gone)) {
        // BaseContainerBlockEntity.preRemoveSideEffects: Containers.dropContents...
        // ...unless the change says to leave that out: then what it held is gone with it.
        gone_kind = was.kind;
        const bool spill = !(flags & kUpdateSkipContainer);
        if (gone & kOpaqueBlockEntity) {
            // One that is kept as the tag it came with: nobody here knows what it held. It
            // goes with its block and spills nothing.
            free_block_entity(gone_kind, gone);
            gone_kind = block_kind::kPlain;
        } else if (chest_kind(gone_kind)) {
            chests_[gone].alive = false;  // ChestBlockEntity.stopOpen does nothing for a removed chest
            if (spill) drop_contents(pos, chests_[gone].items, Chest::kSlots);
        } else if (gone_kind == block_kind::kFurnace) {
            furnaces_[gone].alive = false;
            if (spill) drop_contents(pos, furnaces_[gone].items, Furnace::kSlots);
        } else if (sign_kind(gone_kind)) {  // nothing to spill, nobody to tell: it is gone at once
            free_block_entity(gone_kind, gone);
            gone_kind = block_kind::kPlain;
        }
    }
    // (BlockState.affectNeighborsAfterRemoval comes here. Of the ported classes the containers
    // have one, and it tells comparators.)
    if (!(flags & kUpdateSkipOnPlace)) on_place(state, pos, old);
    // EntityBlock.newBlockEntity - if the block is still the one that was set, and has none yet.
    if (old_block != new_block && (chest_kind(now.kind) || now.kind == block_kind::kFurnace || sign_kind(now.kind)) &&
        world::block_of(world_.block(pos.x, pos.y, pos.z)) == new_block && !(kept && world_.block_entity(pos.x, pos.y, pos.z))) {
        const u32 data = chest_kind(now.kind)  ? new_chest(pos)
                         : sign_kind(now.kind) ? new_sign(pos)
                                               : new_furnace(pos, cooker_of(now.block_entity));
        world_.add_block_entity(pos.x, pos.y, pos.z, now.block_entity, data);
        if (sign_kind(now.kind)) show_sign(data);  // (what a client is shown of it: an empty sign's text)
    }
    // Back in Level.setBlock: the rest happens only if the block is what was asked for - onPlace
    // may have made it something else, with a setBlock of its own (a TNT that went off).
    if (world_.block(pos.x, pos.y, pos.z) == state) {
        if (flags & kUpdateNeighbors) update_neighbors_at(pos, old_block);
        if (!(flags & kUpdateKnownShape) && limit > 0)
            update_neighbour_shapes(state, pos, flags & ~(kUpdateNeighbors | kUpdateSuppressDrops), limit - 1);
    }
    if (gone_kind != block_kind::kPlain) {
        // In vanilla a menu finds its container gone at its player's next tick: it shows the
        // slots that emptied, then closes (stillValid). Here that happens now, once the
        // neighbours have heard of the change - the other half of a double chest is a chest of
        // its own by the time its lid is heard to fall.
        close_menus_on(gone_kind, gone);
        free_block_entity(gone_kind, gone);
    }
    return true;
}

// Level.removeBlock(pos, false): what stays is the block of the fluid the state held.
bool Game::remove_block(BlockPos pos) {
    return set_block(pos, static_cast<BlockState>(st::fluid_legacy_of(world_.block(pos.x, pos.y, pos.z))));
}

// Level.destroyBlock(pos, dropResources, null, updateLimit): the block breaks by itself - whoever
// is near sees and hears it, it drops what its loot table gives an empty hand, and is gone.
bool Game::break_block(BlockPos pos, bool drop, i32 limit) {
    const BlockState state = world_.block(pos.x, pos.y, pos.z);
    if (world::is_air(state)) return false;
    level_event(kLevelEventDestroyBlock, pos, static_cast<i32>(state));  // Block.spawnDestroyParticles
    if (drop) drop_resources(state, pos);
    return set_block(pos, static_cast<BlockState>(st::fluid_legacy_of(state)), kUpdateAll, limit);
}

// Block.dropResources(state, level, pos, blockEntity, null, ItemStack.EMPTY)
void Game::drop_resources(BlockState state, BlockPos pos) {
    ItemStack stack = block_drop(state);
    if (!stack.empty()) pop_resource(pos, std::move(stack));
}

// Block.getDrops without a tool: the loot table, reduced by mcgen to what it gives then
// (BlockInfo::drop) - the first candidate with its chance, else the second; a count that comes
// out below one is nothing (a mushroom block, most times); a double slab is two, and a block of
// petals as many as it holds.
ItemStack Game::block_drop(BlockState state) {
    const world::BlockInfo& info = world::block_info_of(state);
    const world::BlockInfo::Drop& drop =
        info.drop_chance >= 1.0f || random_float() < info.drop_chance ? info.drop[0] : info.drop[1];
    if (!drop.item) return {};
    i32 count = drop.min + static_cast<i32>(random_int(static_cast<u32>(drop.max - drop.min) + 1u));
    if (info.kind == block_kind::kSlab && (state - info.first_state) / props::slab::kTypeStride == 2) count *= 2;  // type=double
    // One for each of the petals, or leaves, that the block holds (the amount: 1, 2, 3, 4).
    if (info.kind == block_kind::kSegmented)
        count *= 1 + static_cast<i32>((state - info.first_state) % props::flower_bed::kFacingStride);
    return count > 0 ? ItemStack(drop.item, static_cast<u32>(count)) : ItemStack();
}

// Block.popResource: near the middle of the block, tossed up.
void Game::pop_resource(BlockPos pos, ItemStack stack) {
    const double x = pos.x + 0.5 + (random_double() * 0.5 - 0.25);
    const double y = pos.y + 0.5 + (random_double() * 0.5 - 0.25) - 0.125;
    const double z = pos.z + 0.5 + (random_double() * 0.5 - 0.25);
    spawn_item(std::move(stack), x, y, z, kPopPickupDelay);
}

// ServerLevel.levelEvent(source, type, pos, data): to everyone within 64 blocks - but for the
// player that caused it (`except`), whose client has shown it already.
void Game::level_event(u32 type, BlockPos pos, i32 data, u32 except) {
    scratch_.clear();
    write_packet(scratch_, s_.compression_threshold, cb::LevelEvent{type, pos.x, pos.y, pos.z, data, false});
    const u32 n = static_cast<u32>(players_.size());
    for (u32 i = 0; i < n; ++i) {
        const double dx = pos.x - ents_.x[i], dy = pos.y - ents_.y[i], dz = pos.z - ents_.z[i];
        if (i != except && dx * dx + dy * dy + dz * dz < kLevelEventRange * kLevelEventRange)
            out(i).append(scratch_.data, scratch_.len);
    }
}

// ---------------------------------------------------------------------------- the neighbours are told

// Level.updateNeighborsAt(pos, block): the six blocks around `pos` hear that it changed.
void Game::update_neighbors_at(BlockPos pos, u32 source_block) {
    add_update({NeighborUpdate::kMulti, kNoSide, 0, false, 0, static_cast<u16>(source_block), pos, {}, 0, 0});
}

// BlockStateBase.updateNeighbourShapes: each of the six takes the shape it has beside `state`.
void Game::update_neighbour_shapes(BlockState state, BlockPos pos, u32 flags, i32 limit) {
    for (const u32 dir : kShapeOrder)
        add_update({NeighborUpdate::kShape, static_cast<u8>(world::opposite(dir)), 0, false, state, 0, step(pos, dir), pos, flags,
                    limit});
}

// CollectingNeighborUpdater.addAndRun: the first update of a chain is run at once, with
// everything it sets off; one that is made while another runs waits for that one to finish.
void Game::add_update(const NeighborUpdate& update) {
    const bool running = update_count_ > 0;
    const i32 max = s_.max_chained_neighbor_updates;
    const bool too_many = max >= 0 && update_count_ >= static_cast<u32>(max);
    ++update_count_;
    if (!too_many) {
        (running ? update_added_ : update_stack_).push_back(update);
    } else if (update_count_ - 1 == static_cast<u32>(max)) {
        LOG_ERROR("Too many chained neighbor updates. Skipping the rest. First skipped position: %d, %d, %d", update.pos.x,
                  update.pos.y, update.pos.z);
    }
    if (!running) run_updates();
}

// CollectingNeighborUpdater.runUpdates
void Game::run_updates() {
    while (!update_stack_.empty() || !update_added_.empty()) {
        // What the last one made goes on top, the first of it uppermost.
        for (usize i = update_added_.size(); i-- > 0;) update_stack_.push_back(update_added_[i]);
        update_added_.clear();
        const usize top = update_stack_.size() - 1;
        while (update_added_.empty()) {
            if (!run_next(top)) {
                update_stack_.pop_back();
                break;
            }
        }
    }
    update_count_ = 0;
}

// NeighborUpdates.runNext: one step of the update at `at`; true while it has more.
bool Game::run_next(usize at) {
    NeighborUpdate& u = update_stack_[at];  // (nothing below touches the stack: what it makes goes to update_added_)
    switch (u.kind) {
        case NeighborUpdate::kShape: {  // NeighborUpdater.executeShapeUpdate
            const NeighborUpdate s = u;
            const BlockState current = world_.block(s.pos.x, s.pos.y, s.pos.z);
            if (LIKELY(!(st::kHooks[current] & st::kHookShape))) return false;
            const BlockState shaped = update_shape(current, s.pos, s.dir, s.from, s.state);
            if (shaped == current) return false;
            // Block.updateOrDestroy: a block that cannot stay breaks, and drops unless told not to.
            if (world::is_air(shaped))
                break_block(s.pos, !(s.flags & kUpdateSuppressDrops), s.limit);
            else
                set_block(s.pos, shaped, s.flags & ~kUpdateSuppressDrops, s.limit);
            return false;
        }
        case NeighborUpdate::kSimple: {
            const BlockPos pos = u.pos;
            const u32 block = u.block;
            neighbor_changed(world_.block(pos.x, pos.y, pos.z), pos, block);
            return false;
        }
        case NeighborUpdate::kFull: {
            const NeighborUpdate f = u;
            neighbor_changed(f.state, f.pos, f.block);
            return false;
        }
        default: {  // kMulti: the next of the six, in NeighborUpdater.UPDATE_ORDER
            if (u.next < 6 && kNeighborOrder[u.next] == u.dir) ++u.next;  // (the constructor's skip)
            const BlockPos pos = step(u.pos, kNeighborOrder[u.next++]);
            const u32 block = u.block;
            if (u.next < 6 && kNeighborOrder[u.next] == u.dir) ++u.next;
            const bool more = u.next < 6;
            neighbor_changed(world_.block(pos.x, pos.y, pos.z), pos, block);
            return more;
        }
    }
}

// ---------------------------------------------------------------------------- what a block does about it

// BlockBehaviour.neighborChanged, for the classes that have one.
void Game::neighbor_changed(BlockState state, BlockPos pos, u32 source_block) {
    (void)source_block;
    if (LIKELY(!(st::kHooks[state] & st::kHookNeighbor))) return;
    switch (world::block_info_of(state).kind) {
        case block_kind::kTnt:  // TntBlock: a signal beside it sets it off
            if (has_neighbour_signal(pos)) {
                prime_tnt(pos, 80, true);
                remove_block(pos);
            }
            break;
        case block_kind::kLamp: {  // RedstoneLampBlock: on at once, off four ticks after the signal went
            const bool lit = lamp_lit(state);
            if (lit == has_neighbour_signal(pos)) break;
            if (lit)
                schedule_tick(pos, state, kLampOffDelay);
            else
                set_block(pos, lamp_state(true), kUpdateClients);
            break;
        }
        case block_kind::kLiquid: liquid_changed(state, pos); break;  // LiquidBlock: its fluid is ticked
        case block_kind::kSponge: try_absorb_water(pos); break;       // SpongeBlock: water has come beside it, perhaps
        default: break;
    }
}

// BlockBehaviour.updateShape: what `state` at `pos` becomes when its neighbour in direction `dir`
// (at `neighbour_pos`) is `neighbour` - for the classes that override it, itself for the others.
// Air: it cannot stay. A block may ask for a tick here (ScheduledTickAccess).
BlockState Game::update_shape(BlockState state, BlockPos pos, u32 dir, BlockPos neighbour_pos, BlockState neighbour) {
    (void)neighbour_pos;
    if (!(st::kHooks[state] & st::kHookShape)) return state;
    const world::BlockInfo& info = world::block_info_of(state);
    const BlockState shaped = shape_beside(state, info, pos, dir, neighbour);
    // SimpleWaterloggedBlock: a block that holds water asks for the water's tick whenever its shape
    // is asked - every class of them, the first thing it does; but a sign, which looks first
    // whether it can stay, and asks only if it does (SignBlock.updateShape, behind its subclasses').
    if (info.wet_stride && world::fluid_of(state) && !(sign_kind(info.kind) && world::is_air(shaped)))
        schedule_fluid_tick(pos, world::fluid::kWater, fluid_rules(world::fluid::kWaterFamily).tick_delay);
    return shaped;
}

// ...the shape itself, class by class.
BlockState Game::shape_beside(BlockState state, const world::BlockInfo& info, BlockPos pos, u32 dir, BlockState neighbour) {
    switch (info.kind) {
        case block_kind::kChest: return chest_update_shape(state, dir, neighbour);
        case block_kind::kSnowy:
        case block_kind::kSpreading:  // SnowyBlock: snowy while what lies on it is snow
            return dir == world::kUp ? info.alt[(world::block_info_of(neighbour).flags & block_flag::kSnowTag) ? 1 : 0] : state;
        case block_kind::kTorch:  // BaseTorchBlock: it stands on something, or not at all
            return dir == world::kDown && !can_survive(state, pos) ? world::kAir : state;
        case block_kind::kWallTorch:
            return world::opposite(dir) == wall_torch_facing(state, info) && !can_survive(state, pos) ? world::kAir : state;
        // A sign: gone with what it stands on, hangs against or hangs from. (One that hangs
        // between walls stays whatever goes: WallHangingSignBlock has no canSurvive of its own.)
        case block_kind::kSign: return dir == world::kDown && !can_survive(state, pos) ? world::kAir : state;
        case block_kind::kWallSign:
            return world::opposite(dir) == sign_facing(state) && !can_survive(state, pos) ? world::kAir : state;
        case block_kind::kHangingSign: return dir == world::kUp && !can_survive(state, pos) ? world::kAir : state;
        case block_kind::kAmethystCluster:
            return dir == world::opposite(cluster_facing(state, info)) && !can_survive(state, pos) ? world::kAir : state;
        case block_kind::kPlant:
        case block_kind::kSegmented:  // VegetationBlock: whatever changes beside it, it looks at what it stands on
            return can_survive(state, pos) ? state : world::kAir;
        case block_kind::kConcretePowder:  // ConcretePowderBlock: water beside it makes it concrete
            if (touches_water(pos)) return info.alt[0];
            [[fallthrough]];
        case block_kind::kFalling:  // FallingBlock: it looks whether it still lies on something
            schedule_tick(pos, state, kFallDelay);
            return state;
        case block_kind::kLiquid:  // LiquidBlock: a source, or what has one beside it, flows on (server/fluids.cpp)
            liquid_shape_changed(state, pos, neighbour);
            return state;
        default: return shaped_update(state, info, pos, dir, neighbour);  // server/block_shapes.cpp
    }
}

// Block.updateFromNeighbourShapes: the state as its six neighbours would have it, asked in
// vanilla's order (BlockBehaviour.UPDATE_SHAPE_ORDER: west, east, north, south, down, up).
BlockState Game::update_from_neighbour_shapes(BlockState state, BlockPos pos) {
    for (const u32 dir : kShapeOrder) {
        const BlockPos at = step(pos, dir);
        state = update_shape(state, pos, dir, at, state_for_client(at));
    }
    return state;
}

// BlockBehaviour.onPlace: `state` was put where `old` was.
void Game::on_place(BlockState state, BlockPos pos, BlockState old) {
    if (LIKELY(!(st::kHooks[state] & st::kHookPlace))) return;
    switch (world::block_info_of(state).kind) {
        case block_kind::kTnt:  // TntBlock: next to a redstone signal it goes off at once
            if (world::block_of(old) != world::block_of(state) && has_neighbour_signal(pos)) {
                prime_tnt(pos, 80, true);
                remove_block(pos);
            }
            break;
        case block_kind::kFalling:
        case block_kind::kConcretePowder: schedule_tick(pos, state, kFallDelay); break;  // FallingBlock
        case block_kind::kLiquid: liquid_changed(state, pos); break;                     // LiquidBlock: its fluid is ticked
        case block_kind::kSponge:  // SpongeBlock: it takes up the water around it (not when it was a sponge already)
            if (world::block_of(old) != world::block_of(state)) try_absorb_water(pos);
            break;
        case block_kind::kWetSponge: wet_sponge_placed(pos); break;  // WetSpongeBlock: dry at once where water does not stay
        default: break;
    }
}

// BlockBehaviour.canSurvive: may `state` be at `pos`, with what is around it?
bool Game::can_survive(BlockState state, BlockPos pos) const {
    const world::BlockInfo& info = world::block_info_of(state);
    switch (info.kind) {
        case block_kind::kTorch:  // Block.canSupportCenter(level, below, UP)
            return world::face_sturdy_center(state_for_client({pos.x, pos.y - 1, pos.z}), world::kUp);
        case block_kind::kWallTorch: {  // the block behind it has a whole face towards it
            const u32 facing = wall_torch_facing(state, info);
            return world::face_sturdy(state_for_client(step(pos, world::opposite(facing))), facing);
        }
        // StandingSignBlock / WallSignBlock: on, or against, a block that is solid (isSolid).
        case block_kind::kSign: return world::is_solid(state_for_client({pos.x, pos.y - 1, pos.z}));
        case block_kind::kWallSign: return world::is_solid(state_for_client(step(pos, world::opposite(sign_facing(state)))));
        // CeilingHangingSignBlock: under a block whose middle holds something (SupportType.CENTER).
        case block_kind::kHangingSign:
            return world::face_sturdy_center(state_for_client({pos.x, pos.y + 1, pos.z}), world::kDown);
        case block_kind::kAmethystCluster: {
            const u32 facing = cluster_facing(state, info);
            return world::face_sturdy(state_for_client(step(pos, world::opposite(facing))), facing);
        }
        // VegetationBlock.mayPlaceOn of the block below, and LeafLitterBlock.canSurvive: what the
        // plant's class asks is a bit of that state, and the plant says which (state::kSupport).
        case block_kind::kPlant:
        case block_kind::kSegmented: return (st::kSupport[state_for_client({pos.x, pos.y - 1, pos.z})] & info.alt[0]) != 0;
        case block_kind::kLantern: {  // LanternBlock: Block.canSupportCenter of what it hangs from or stands on
            if ((state - info.first_state) / props::lantern::kHangingStride != 0)  // (hanging: true, false)
                return world::face_sturdy_center(state_for_client({pos.x, pos.y - 1, pos.z}), world::kUp);
            const BlockState above = state_for_client({pos.x, pos.y + 1, pos.z});
            return !(st::kConnect[above] & st::kUnstableBottomCenter) && world::face_sturdy_center(above, world::kDown);
        }
        default: return true;
    }
}

// ConcretePowderBlock.touchesLiquid: water on one of its sides, not held off by a whole face -
// written as vanilla has it, with the block it looks at before it has moved there (the block
// below counts only when the powder's own place holds water).
bool Game::touches_water(BlockPos pos) const {
    BlockPos test = pos;
    for (u32 dir = 0; dir < 6; ++dir) {  // Direction.values()
        BlockState there = state_for_client(test);
        if (dir == world::kDown && !world::holds_water(there)) continue;
        test = step(pos, dir);
        there = state_for_client(test);
        if (world::holds_water(there) && !world::face_sturdy(there, world::opposite(dir))) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------- scheduled ticks

// ServerLevel.tick: blockTicks.tick(gameTime, 65536, this::tickBlock).
void Game::run_block_ticks() {
    world::LevelTicks& ticks = world_.block_ticks();
    ticks.collect(static_cast<i64>(time_));
    world::LevelTicks::Due due;
    while (ticks.next(due)) {
        const BlockState state = world_.block(due.x, due.y, due.z);
        if (world::block_of(state) != due.type) continue;  // tickBlock: the block is another one by now
        tick_block(state, {due.x, due.y, due.z});
    }
    ticks.finish();
}

// BlockBehaviour.tick, for the classes that have one.
void Game::tick_block(BlockState state, BlockPos pos) {
    if (!(st::kHooks[state] & st::kHookTick)) return;
    switch (world::block_info_of(state).kind) {
        case block_kind::kChest:
        case block_kind::kBarrel: recheck_chest(pos); break;  // ChestBlock / BarrelBlock: recheckOpen
        case block_kind::kLamp:                               // RedstoneLampBlock
            if (lamp_lit(state) && !has_neighbour_signal(pos)) set_block(pos, lamp_state(false), kUpdateClients);
            break;
        case block_kind::kFalling:
        case block_kind::kConcretePowder:  // FallingBlock: nothing under it - it falls
            if ((st::kHooks[state_for_client({pos.x, pos.y - 1, pos.z})] & st::kFallThrough) && pos.y >= world_.min_y())
                fall(pos, state);
            break;
        default: break;
    }
}

// ---------------------------------------------------------------------------- random ticks

// ServerChunkCache.tickChunks -> ServerLevel.tickChunk: in every chunk that is simulated, each
// section with a block that ticks at random has random-tick-speed of its cells picked, and what
// is picked ticks if it is such a block. world::Dimension knows which sections have anything to
// do (random_chunks(), Chunk::active_sections); the rest are passed by.
void Game::run_random_ticks() {
    const u32 speed = s_.random_tick_speed;
    if (!speed || world_.random_chunks().empty()) return;
    // A copy: a tick changes blocks, a changed block changes which chunks are in the list.
    random_scratch_.assign(world_.random_chunks().begin(), world_.random_chunks().end());
    const i32 min_y = world_.min_y();
    for (world::Chunk* c : random_scratch_) {
        u32 sections = world::kCheckDormantSections ? c->ticking_sections : c->active_sections;
        for (; sections; sections &= sections - 1) {
            const u32 si = static_cast<u32>(std::countr_zero(sections));
            for (u32 i = 0; i < speed; ++i) {
                // Level.getBlockRandomPos
                random_value_ = random_value_ * 3u + 1013904223u;
                const u32 v = random_value_ >> 2;
                const u32 lx = v & 15, lz = (v >> 8) & 15, ly = (v >> 16) & 15;
                const BlockState state = c->sections[si].get(world::Section::index(lx, ly, lz));
                if (LIKELY(!(st::kRandom[state] & st::kRandomTick))) continue;
                const bool dormant = !((c->active_sections >> si) & 1);
                if (dormant && !world::kCheckDormantSections) break;  // (a tick before this one put it to rest)
                expect_no_change_ = dormant;
                random_tick(state, {c->x * 16 + static_cast<i32>(lx), min_y + static_cast<i32>(si * 16 + ly),
                                    c->z * 16 + static_cast<i32>(lz)});
                expect_no_change_ = false;
            }
        }
    }
}

// BlockBehaviour.randomTick, for the classes whose random tick is ported (state::kRandomTick).
void Game::random_tick(BlockState state, BlockPos pos) {
    const world::BlockInfo& info = world::block_info_of(state);
    if (info.next_stage) {  // copper, whatever else its class is
        // WeatheringCopperChestBlock.randomTick: the left half of a pair weathers, and takes the
        // right one with it (CopperChestBlock.updateShape) - while nobody has the chest open.
        if (info.kind == block_kind::kChest && !copper_chest_may_weather(state, pos)) return;
        return tick_weathering(state, pos);
    }
    switch (info.kind) {
        case block_kind::kSpreading: tick_spreading(state, pos); break;
        case block_kind::kNylium:  // NyliumBlock: netherrack under what shuts the light out
            if (st::kRandom[state_for_client({pos.x, pos.y + 1, pos.z})] & st::kSmothersNylium)
                set_block(pos, static_cast<BlockState>(gen::block::kNetherrack));
            break;
        case block_kind::kBuddingAmethyst: tick_budding_amethyst(pos); break;
        default: break;
    }
}

// LevelLightEngine.getRawBrightness(pos, Level.getSkyDarken()): the brighter of the block's
// light and the sky's - the sky's less what the time of day takes from it (sky_darken_: none by
// day, eleven levels in the middle of the night; there is no weather to take more).
u32 Game::raw_brightness(BlockPos pos) const {
    const world::LightEngine& light = world_.light();
    const i32 block = static_cast<i32>(light.light(world::LightEngine::kBlock, pos.x, pos.y, pos.z));
    const i32 sky = world_.has_skylight() ? static_cast<i32>(light.light(world::LightEngine::kSky, pos.x, pos.y, pos.z)) : 0;
    return static_cast<u32>(std::max(block, sky - sky_darken_));
}

// SpreadingSnowyBlock.randomTick (grass, mycelium): soil again under what smothers it; in the
// light, four tries at a soil block near it - one to each side, three down, one up - that has
// room above. (The questions are answered per state by mcgen: state::kSmothers, kNoSpread.)
void Game::tick_spreading(BlockState state, BlockPos pos) {
    const world::BlockInfo& info = world::block_info_of(state);
    const BlockPos above{pos.x, pos.y + 1, pos.z};
    if (st::kRandom[state_for_client(above)] & st::kSmothers) {  // !canStayAlive
        set_block(pos, info.alt[2]);
        return;
    }
    if (raw_brightness(above) < 9) return;
    for (u32 i = 0; i < 4; ++i) {
        const i32 dx = static_cast<i32>(random_int(3)) - 1, dy = static_cast<i32>(random_int(5)) - 3;
        const BlockPos to{pos.x + dx, pos.y + dy, pos.z + static_cast<i32>(random_int(3)) - 1};
        if (!(st::kRandom[state_for_client(to)] & st::kSoil)) continue;
        const BlockState over = state_for_client({to.x, to.y + 1, to.z});
        if (st::kRandom[over] & st::kNoSpread) continue;  // !canPropagate
        set_block(to, info.alt[(world::block_info_of(over).flags & block_flag::kSnowTag) ? 1 : 0]);
    }
}

// ChangeOverTimeBlock.changeOverTime / getNextState (copper): now and then it looks at the
// copper within four blocks - anything less weathered than itself holds it back, and the more
// of what is around has gone further, the likelier it follows.
void Game::tick_weathering(BlockState state, BlockPos pos) {
    if (random_float() >= kWeatherChance) return;
    const world::BlockInfo& info = world::block_info_of(state);
    const u32 own = info.weather;
    u32 same = 0, older = 0;
    for (i32 dy = -kWeatherScan; dy <= kWeatherScan; ++dy) {  // BlockPos.withinManhattan(pos, 4), in an order of our own
        const i32 left = kWeatherScan - std::abs(dy);
        for (i32 dz = -left; dz <= left; ++dz) {
            const i32 reach = left - std::abs(dz);
            for (i32 dx = -reach; dx <= reach; ++dx) {
                if (!(dx | dy | dz)) continue;
                const u32 age = world::block_info_of(state_for_client({pos.x + dx, pos.y + dy, pos.z + dz})).weather;
                if (!age) continue;
                if (age < own) return;
                ++(age > own ? older : same);
            }
        }
    }
    const float chance = static_cast<float>(older + 1) / static_cast<float>(older + same + 1);
    const float modified = chance * chance * (own == 1 ? 0.75f : 1.0f);  // WeatheringCopper.getChanceModifier
    // WeatheringCopper.getNext(state): the next block, with this one's properties.
    if (random_float() < modified) set_block(pos, static_cast<BlockState>(info.next_stage + (state - info.first_state)));
}

// BuddingAmethystBlock.randomTick: one time in five a bud on one of its sides grows a stage -
// or a small one appears where there is air, or water.
void Game::tick_budding_amethyst(BlockPos pos) {
    if (random_int(5) != 0) return;
    const u32 dir = random_int(6);
    const BlockPos at = step(pos, dir);
    const BlockState there = state_for_client(at);
    const world::BlockInfo& info = world::block_info_of(there);
    u32 next = 0;
    // canClusterGrowAtState: air, or a block of water that is full (a source, or falling: of the
    // states of water those are the ones that smother what is under them).
    if (world::is_air(there) ||
        (world::block_of(there) == world::block_of(gen::block::kWater) && (st::kRandom[there] & st::kSmothers))) {
        next = props::small_amethyst_bud::kFirst;
    } else if (info.kind == block_kind::kAmethystCluster && cluster_facing(there, info) == dir) {
        next = info.first_state == props::small_amethyst_bud::kFirst    ? props::medium_amethyst_bud::kFirst
               : info.first_state == props::medium_amethyst_bud::kFirst ? props::large_amethyst_bud::kFirst
               : info.first_state == props::large_amethyst_bud::kFirst  ? props::amethyst_cluster::kFirst
                                                                        : 0u;
    }
    if (next) set_block(at, cluster_state(next, dir, world::holds_water_source(there)));
}

}  // namespace server
