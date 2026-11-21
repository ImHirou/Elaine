// SPDX-License-Identifier: GPL-3.0-or-later
// Blocks that are more than a state: a chest and a furnace (block entities) and the crafting
// table (a menu without one). What happens around a block when it changes is
// server/block_updates.cpp.
//
// The rules are vanilla's, read from the 26.3 jar; each function names the method it follows:
//   BaseContainerBlockEntity.preRemoveSideEffects, Containers.dropContents
//   ChestBlock (updateShape, tick, useWithoutItem, combine, isChestBlockedAt), DoubleBlockCombiner,
//   ChestBlockEntity + ContainerOpenersCounter (who has it open: the lid, the sound, the recount)
//   AbstractFurnaceBlock / FurnaceBlock.openContainer, CraftingTableBlock, TntBlock.useItemOn
//   ServerPlayer.openMenu / closeContainer / doCloseContainer, the stillValid of each menu
//   ServerLevel.blockEvent / runBlockEvents
// What a block entity holds and how a furnace burns is server/block_entity.cpp; what a click in
// a menu does is server/inventory.cpp.
//
// What is NOT here:
//   * a furnace gives no experience (no orbs yet), and nothing is remembered about its recipes;
//   * comparators (affectNeighborsAfterRemoval, getAnalogOutputSignal): there are none;
//   * custom names: a chest named on an anvil cannot be placed (its item carries a component);
//   * what a leaving player has on a menu's cursor or in a crafting grid is not dropped: a
//     player's inventory is not kept either;
//   * ContainerOpenersCounter counts by a box around the chest; here a player counts while its
//     menu is open on the chest - and that closes when it walks away (stillValid).
#include "nbt/nbt.hpp"
#include "server/game.hpp"
#include "text/component.hpp"

#include PROTO_GEN_ITEMS_HPP
#include PROTO_GEN_STATIC_IDS_HPP

namespace server {

using namespace proto;
using world::BlockState;
namespace block_kind = proto::gen::block_kind;
namespace props = proto::gen::props;
namespace be_type = proto::gen::block_entity_type;

namespace {

constexpr i32 kRecheckDelay = 5;           // ContainerOpenersCounter.CHECK_TICK_DELAY
constexpr double kBlockEventRange = 64.0;  // ServerLevel.runBlockEvents
constexpr double kMenuReach = 4.0;         // Container.stillValidBlockEntity, AbstractContainerMenu.stillValid
constexpr u8 kEventOpenCount = 1;          // ChestBlockEntity.EVENT_SET_OPEN_COUNT

// ---- ChestBlock's state: facing (north, south, west, east), type, waterlogged
enum : u32 { kSingle, kLeft, kRight };  // ChestType
struct ChestState {
    u32 facing;  // a world::Direction
    u32 type;
    bool waterlogged;
};
ChestState chest_state(BlockState s) {
    const u32 off = s - props::chest::kFirst;
    return {world::kNorth + off / props::chest::kFacingStride, off % props::chest::kFacingStride / props::chest::kTypeStride,
            off % props::chest::kTypeStride == 0};
}
BlockState chest_block(const ChestState& c) {
    return static_cast<BlockState>(props::chest::kFirst + (c.facing - world::kNorth) * props::chest::kFacingStride +
                                   c.type * props::chest::kTypeStride + (c.waterlogged ? 0 : 1));
}
bool is_chest(BlockState s) { return world::block_info_of(s).kind == block_kind::kChest; }
// Direction.getClockWise / getCounterClockWise, for the horizontal ones.
constexpr u32 kClockwise[6] = {0, 0, world::kEast, world::kWest, world::kNorth, world::kSouth};
constexpr u32 kCounterClockwise[6] = {0, 0, world::kWest, world::kEast, world::kSouth, world::kNorth};
// ChestBlock.getConnectedDirection: where the other half of a double chest is.
u32 connected_direction(const ChestState& c) { return c.type == kLeft ? kClockwise[c.facing] : kCounterClockwise[c.facing]; }

BlockPos step(BlockPos p, u32 dir) { return {p.x + world::kStepX[dir], p.y + world::kStepY[dir], p.z + world::kStepZ[dir]}; }

// Component.translatable(key), as the tag ComponentSerialization.CODEC writes for it.
std::vector<u8> translatable(std::string_view key) {
    base::ByteBuf raw, out;
    nbt::Writer w(raw);
    w.root(nbt::kCompound);
    w.put_string("translate", key);
    w.end_compound();
    nbt::Doc doc;
    if (doc.parse({raw.data, raw.len}) != nbt::Error::kNone || text::transcode(doc.root(), out) != text::Result::kOk)
        std::abort();
    return {out.data, out.data + out.len};
}

}  // namespace

// The titles of the menus, once: BaseContainerBlockEntity.getDefaultName, ChestBlock's
// "container.chestDouble", CraftingTableBlock.CONTAINER_TITLE.
void Game::init_titles() {
    title_chest_ = translatable("container.chest");
    title_double_chest_ = translatable("container.chestDouble");
    title_furnace_ = translatable("container.furnace");
    title_crafting_ = translatable("container.crafting");
}

// ---------------------------------------------------------------------------- blocks changing

// ChestBlock.updateShape: what the chest `state` becomes when its neighbour in direction `dir`
// is `neighbour`. (A waterlogged one would ask for a tick of its water: there are no fluid ticks.)
BlockState Game::chest_update_shape(BlockState state, u32 dir, BlockState neighbour) const {
    ChestState c = chest_state(state);
    const bool horizontal = dir >= world::kNorth;
    if (is_chest(neighbour) && horizontal) {
        // A single chest joins the half that was just placed against it.
        const ChestState n = chest_state(neighbour);
        if (c.type == kSingle && n.type != kSingle && c.facing == n.facing && connected_direction(n) == world::opposite(dir))
            c.type = n.type == kLeft ? kRight : kLeft;
    } else if (connected_direction(c) == dir) {
        c.type = kSingle;  // the other half is gone
    }
    return chest_block(c);
}

// Containers.dropContents: every stack falls out in pieces of 10 to 30, all pieces of a stack
// from one random place inside the block, each tossed its own way (RandomSource.triangle).
void Game::drop_contents(BlockPos pos, ItemStack* items, u32 n) {
    constexpr double kItemWidth = 0.25, kRange = 1.0 - kItemWidth, kHalf = kItemWidth / 2.0;
    constexpr double kSpread = 0.11485000171139836;
    const auto triangle = [&](double mean) { return mean + kSpread * (random_double() - random_double()); };
    for (u32 i = 0; i < n; ++i) {
        ItemStack stack = items[i].take();
        if (stack.empty()) continue;
        const double x = pos.x + random_double() * kRange + kHalf;
        const double y = pos.y + random_double() * kRange;
        const double z = pos.z + random_double() * kRange + kHalf;
        while (!stack.empty()) {
            ItemStack piece = stack.split(random_int(21) + 10);
            const double vx = triangle(0.0), vy = triangle(0.2), vz = triangle(0.0);
            spawn_item(std::move(piece), x, y, z, vx, vy, vz, 0);  // no pick-up delay
        }
    }
}

// ChestBlock.getStateForPlacement: a chest faces the one who places it, and joins a single chest
// beside it that faces the same way - or, placed sneaking against the side of one, that one.
BlockState Game::chest_for_placement(u32 pi, BlockPos at, u32 face) const {
    // candidatePartnerFacing: the facing of a single chest in direction `dir`, kNone without one.
    const auto partner = [&](u32 dir) {
        const BlockPos n = step(at, dir);
        const BlockState s = world_.block(n.x, n.y, n.z);
        if (!is_chest(s)) return kNone;
        const ChestState c = chest_state(s);
        return c.type == kSingle ? c.facing : kNone;
    };
    // Waterlogged when it replaces a water source. (There is no water yet; the rule is the block's.)
    const BlockState replaced = world_.block(at.x, at.y, at.z);
    ChestState c{world::opposite(horizontal_direction(ents_.yaw[pi])), kSingle,
                 world::state::fluid_legacy_of(replaced) == gen::block::kWater && replaced != world::kAir};
    const bool sneaking = players_[pi].shift;  // BlockPlaceContext.isSecondaryUseActive
    const bool horizontal_face = face >= world::kNorth;
    if (horizontal_face && sneaking) {
        const u32 facing = partner(world::opposite(face));
        if (facing != kNone && (facing - world::kNorth) / 2 != (face - world::kNorth) / 2) {  // not along the face's axis
            c.facing = facing;
            c.type = kCounterClockwise[facing] == world::opposite(face) ? kRight : kLeft;
        }
    }
    if (c.type == kSingle && !sneaking) {  // getChestType
        if (c.facing == partner(kClockwise[c.facing]))
            c.type = kLeft;
        else if (c.facing == partner(kCounterClockwise[c.facing]))
            c.type = kRight;
    }
    return chest_block(c);
}

// ---------------------------------------------------------------------------- clicking a block

// BlockState.useItemOn, then (main hand only) useWithoutItem, for the blocks that override one:
// true when the click was the block's (InteractionResult.SUCCESS).
bool Game::use_block(u32 pi, BlockPos pos, BlockState state, const ItemStack& held, u32 hand) {
    Player& p = players_[pi];
    switch (world::block_info_of(state).kind) {
        case block_kind::kTnt:  // TntBlock.useItemOn: lit by flint and steel or a fire charge, in either hand
            if (held.item() != gen::item::kFlintAndSteel && held.item() != gen::item::kFireCharge) return false;
            prime_tnt(pos, 80, true);
            set_block(pos, world::kAir);
            // hurtAndBreak / consume: neither takes anything from a creative player.
            return true;
        case block_kind::kCraftingTable:  // CraftingTableBlock.useWithoutItem
            if (hand != cb::SwingAnimation::kMainHand) return false;
            open_menu(pi, Menu::Kind::kCrafting, gen::menu::kCrafting, title_crafting_, 0);
            p.open_pos = pos;
            return true;
        case block_kind::kFurnace: {  // AbstractFurnaceBlock.useWithoutItem -> FurnaceBlock.openContainer
            if (hand != cb::SwingAnimation::kMainHand) return false;
            const world::BlockEntityRef* be = world_.block_entity(pos.x, pos.y, pos.z);
            if (!be) return true;
            const u32 data = be->data;
            open_menu(pi, Menu::Kind::kFurnace, gen::menu::kFurnace, title_furnace_, 0);
            p.open_pos = pos;
            p.open_data[0] = data;
            p.menu.send_all(out(pi), p.threshold, containers(p));
            return true;
        }
        case block_kind::kChest: {  // ChestBlock.useWithoutItem -> getMenuProvider -> DoubleBlockCombiner
            if (hand != cb::SwingAnimation::kMainHand) return false;
            const world::BlockEntityRef* be = world_.block_entity(pos.x, pos.y, pos.z);
            // isChestBlockedAt: something solid on top keeps the lid shut. (Or a cat.)
            const auto blocked = [&](BlockPos at) {
                return (world::flags_of(world_.block(at.x, at.y + 1, at.z)) & world::state::kRedstoneConductor) != 0;
            };
            if (!be || blocked(pos)) return true;
            u32 first = be->data, second = kNone;
            const ChestState c = chest_state(state);
            if (c.type != kSingle) {
                const BlockPos other = step(pos, connected_direction(c));
                const BlockState os = world_.block(other.x, other.y, other.z);
                if (is_chest(os)) {
                    const ChestState o = chest_state(os);
                    const world::BlockEntityRef* obe = world_.block_entity(other.x, other.y, other.z);
                    if (o.type != kSingle && o.type != c.type && o.facing == c.facing) {
                        // (A half that is kept as the tag it came with keeps the pair shut.)
                        if (blocked(other) || (obe && (obe->data & kOpaqueBlockEntity))) return true;
                        // The right half is the first 27 slots (DoubleBlockCombiner.BlockType.FIRST).
                        if (obe) second = obe->data;
                        if (obe && c.type != kRight) std::swap(first, second);
                    }
                }
            }
            // new ChestMenu: Container.startOpen comes before the screen opens.
            if (p.menu.kind() != Menu::Kind::kInventory) close_menu(pi, true);
            chest_opened(first);
            if (second != kNone) chest_opened(second);
            open_menu(pi, Menu::Kind::kChest, second == kNone ? gen::menu::kGeneric9x3 : gen::menu::kGeneric9x6,
                      second == kNone ? title_chest_ : title_double_chest_, second == kNone ? 3 : 6);
            p.open_pos = pos;
            p.open_data[0] = first;
            p.open_data[1] = second;
            p.menu.send_all(out(pi), p.threshold, containers(p));
            return true;
        }
        default: return false;
    }
}

Containers Game::containers(const Player& p) {
    Containers c;
    if (p.menu.kind() == Menu::Kind::kChest) {
        c.chest[0] = chests_[p.open_data[0]].items;
        if (p.open_data[1] != kNone) c.chest[1] = chests_[p.open_data[1]].items;
    } else if (p.menu.kind() == Menu::Kind::kFurnace) {
        c.furnace = &furnaces_[p.open_data[0]];
    }
    return c;
}

// ServerPlayer.openMenu: whatever was open closes, the client is told to open a screen, and the
// new menu becomes the player's. A menu with a container behind it sends its contents once the
// caller has said which (initMenu -> sendAllDataToRemote); the crafting menu, which has none,
// does it here.
void Game::open_menu(u32 pi, Menu::Kind kind, u32 menu_type, std::span<const u8> title, u32 rows) {
    Player& p = players_[pi];
    if (p.menu.kind() != Menu::Kind::kInventory) close_menu(pi, true);
    p.container_counter = p.container_counter % 100 + 1;  // nextContainerCounter
    send(pi, cb::OpenScreen{p.container_counter, menu_type, title});
    p.menu.open(kind, p.container_counter, rows);
    p.open_data[0] = p.open_data[1] = kNone;
    if (kind == Menu::Kind::kCrafting) p.menu.send_all(out(pi), p.threshold);
    dirty_[pi] |= kMenuDirty;  // an open menu is looked at every tick (sync_menus)
}

// ServerPlayer.closeContainer (`tell_client`) / doCloseContainer: the open menu is removed -
// what it held for the player goes back to the inventory, a chest loses an opener - and the
// player's own menu is the open one again.
void Game::close_menu(u32 pi, bool tell_client) {
    Player& p = players_[pi];
    const Menu::Kind kind = p.menu.kind();
    if (tell_client) send(pi, cb::ContainerClose{p.menu.container_id()});
    const u32 chests[2] = {p.open_data[0], p.open_data[1]};
    p.menu.close(menu_ctx(pi));
    p.open_data[0] = p.open_data[1] = kNone;
    apply_effects(pi);
    if (kind == Menu::Kind::kChest) {  // ChestMenu.removed: container.stopOpen, each half for itself
        for (const u32 c : chests)
            if (c != kNone && chests_[c].alive) chest_closed(c);
    }
    touch_inventory(pi);
}

// The stillValid of the open menu: its block is still there and within reach.
bool Game::menu_still_valid(u32 pi) {
    const Player& p = players_[pi];
    switch (p.menu.kind()) {
        case Menu::Kind::kChest:  // CompoundContainer.stillValid: both halves
            for (const u32 c : p.open_data)
                if (c != kNone && !within_reach(pi, {chests_[c].x, chests_[c].y, chests_[c].z}, kMenuReach)) return false;
            return true;
        case Menu::Kind::kFurnace: return within_reach(pi, p.open_pos, kMenuReach);
        case Menu::Kind::kCrafting:
            return world::block_info_of(world_.block(p.open_pos.x, p.open_pos.y, p.open_pos.z)).kind ==
                       block_kind::kCraftingTable &&
                   within_reach(pi, p.open_pos, kMenuReach);
        default: return true;
    }
}

// The block entity `data` (a chest or a furnace, by block kind) is gone: every menu on it says
// what changed one last time (ServerPlayer.tick: broadcastChanges) and closes.
void Game::close_menus_on(u32 kind, u32 data) {
    const Menu::Kind menu = kind == block_kind::kChest ? Menu::Kind::kChest : Menu::Kind::kFurnace;
    for (u32 pi = 0; pi < players_.size(); ++pi) {
        Player& p = players_[pi];
        if (p.menu.kind() != menu || (p.open_data[0] != data && p.open_data[1] != data)) continue;
        p.menu.send_changes(out(pi), p.threshold, containers(p));
        close_menu(pi, true);
    }
}

// ---------------------------------------------------------------------------- chests: who has one open

// ChestBlockEntity.playSound: from the middle of the chest - of both halves, for a double one,
// whose left half stays silent.
void Game::chest_sound(const Chest& c, u32 sound) {
    const ChestState s = chest_state(world_.block(c.x, c.y, c.z));
    if (s.type == kLeft) return;
    double x = c.x + 0.5, z = c.z + 0.5;
    if (s.type == kRight) {
        const u32 dir = connected_direction(s);
        x += world::kStepX[dir] * 0.5;
        z += world::kStepZ[dir] * 0.5;
    }
    play_sound(sound, cb::Sound::kSourceBlocks, x, c.y + 0.5, z, 0.5f, random_float() * 0.1f + 0.9f);
}

// ContainerOpenersCounter.incrementOpeners: the first opener is heard and starts the recount;
// every one moves the lid (ChestBlockEntity.signalOpenCount: a block event with the count).
void Game::chest_opened(u32 chest) {
    Chest& c = chests_[chest];
    if (c.open++ == 0) {
        chest_sound(c, gen::sound::kBlockChestOpen);
        // ContainerOpenersCounter.scheduleRecheck: a tick of the block (one per chest at a time).
        schedule_tick({c.x, c.y, c.z}, world_.block(c.x, c.y, c.z), kRecheckDelay);
    }
    block_event(c.x, c.y, c.z, kEventOpenCount, static_cast<u8>(c.open));
}

// ContainerOpenersCounter.decrementOpeners
void Game::chest_closed(u32 chest) {
    Chest& c = chests_[chest];
    if (c.open > 0 && --c.open == 0) chest_sound(c, gen::sound::kBlockChestClose);
    block_event(c.x, c.y, c.z, kEventOpenCount, static_cast<u8>(c.open));
}

// ChestBlock.tick -> ChestBlockEntity.recheckOpen -> ContainerOpenersCounter.recheckOpeners:
// every five ticks an open chest counts again who has it open and says so - which is how someone
// who comes into view later sees its lid up.
void Game::recheck_chest(BlockPos pos) {
    const world::BlockEntityRef* be = world_.block_entity(pos.x, pos.y, pos.z);
    if (!be || be->type != be_type::kChest || (be->data & kOpaqueBlockEntity)) return;
    const u32 chest = be->data;
    Chest& c = chests_[chest];
    u32 open = 0;
    for (const Player& p : players_)
        open += p.menu.kind() == Menu::Kind::kChest && (p.open_data[0] == chest || p.open_data[1] == chest);
    if (open != c.open) {
        if (open != 0 && c.open == 0)
            chest_sound(c, gen::sound::kBlockChestOpen);
        else if (open == 0)
            chest_sound(c, gen::sound::kBlockChestClose);
        c.open = static_cast<u16>(open);
    }
    block_event(c.x, c.y, c.z, kEventOpenCount, static_cast<u8>(open));
    if (open > 0) schedule_tick(pos, world_.block(pos.x, pos.y, pos.z), kRecheckDelay);
}

// Level.blockEvent: queued for this tick's runBlockEvents - once. Vanilla keeps its events in
// a set (an ObjectLinkedOpenHashSet of records), so one that is asked for twice before it is
// sent is sent once: a lid that falls in the tick of its chest's recount is told once.
void Game::block_event(i32 x, i32 y, i32 z, u8 b0, u8 b1) {
    for (const BlockEventData& e : block_events_)
        if (e.x == x && e.y == y && e.z == z && e.b0 == b0 && e.b1 == b1) return;
    block_events_.push_back({x, y, z, b0, b1});
}

// ServerLevel.runBlockEvents: each event that its block still answers to goes to everyone
// within 64 blocks.
void Game::run_block_events() {
    if (LIKELY(block_events_.empty())) return;
    for (const BlockEventData& e : block_events_) {
        const BlockState state = world_.block(e.x, e.y, e.z);
        if (!is_chest(state)) continue;  // doBlockEvent: the block is another one by now
        scratch_.clear();
        write_packet(scratch_, s_.compression_threshold, cb::BlockEvent{e.x, e.y, e.z, e.b0, e.b1, world::block_of(state)});
        for (u32 i = 0; i < players_.size(); ++i) {
            const double dx = e.x - ents_.x[i], dy = e.y - ents_.y[i], dz = e.z - ents_.z[i];
            if (dx * dx + dy * dy + dz * dz < kBlockEventRange * kBlockEventRange) out(i).append(scratch_.data, scratch_.len);
        }
    }
    block_events_.clear();
}

// ---------------------------------------------------------------------------- furnaces

// AbstractFurnaceBlockEntity.serverTick for every furnace, where vanilla ticks block entities:
// after the entities. One that neither burns nor could start costs one test.
void Game::tick_furnaces() {
    const u32 n = static_cast<u32>(furnaces_.size());
    for (u32 i = 0; i < n; ++i) {
        Furnace& f = furnaces_[i];
        if (LIKELY(f.idle()) || !f.alive) continue;
        // Level.shouldTickBlocksAt: block entities tick where chunks do.
        world::Chunk* chunk = world_.find(f.x >> 4, f.z >> 4);
        if (UNLIKELY(!chunk || !chunk->ticking)) continue;
        chunk->dirty = true;  // BlockEntity.setChanged: what it holds is no longer what is on disk
        Furnace::Ticked t = f.tick();
        const BlockPos pos{f.x, f.y, f.z};
        if (UNLIKELY(t.lit_changed)) {  // the block's `lit` follows: it glows, and the light with it
            const u32 off = world_.block(pos.x, pos.y, pos.z) - props::furnace::kFirst;
            set_block(pos, static_cast<BlockState>(props::furnace::kFirst +
                                                   off / props::furnace::kFacingStride * props::furnace::kFacingStride +
                                                   (t.lit ? 0 : 1)));
        }
        if (UNLIKELY(!t.spilled.empty())) drop_contents(pos, &t.spilled, 1);  // Containers.dropItemStack
    }
}

// ServerLevel.playSeededSound -> PlayerList.broadcast: everyone within the sound's range
// (SoundEvent.getRange: 16 blocks, more for a volume above 1).
void Game::play_sound(u32 sound, u32 source, double x, double y, double z, float volume, float pitch) {
    scratch_.clear();
    write_packet(scratch_, s_.compression_threshold, cb::Sound{sound, source, x, y, z, volume, pitch, random()});
    const double range = volume > 1.0f ? 16.0 * static_cast<double>(volume) : 16.0;
    const u32 n = static_cast<u32>(players_.size());
    for (u32 i = 0; i < n; ++i) {
        const double dx = x - ents_.x[i], dy = y - ents_.y[i], dz = z - ents_.z[i];
        if (dx * dx + dy * dy + dz * dz < range * range) out(i).append(scratch_.data, scratch_.len);
    }
}

}  // namespace server
