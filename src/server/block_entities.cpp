// Blocks that are more than a state: a chest and a furnace (block entities), the crafting table
// (a menu without one), and what happens around a block when it changes.
//
// The rules are vanilla's, read from the 26.3 jar; each function names the method it follows:
//   LevelChunk.setBlockState (block entities come and go with their block), Level.setBlock
//   (neighbours are told), BaseContainerBlockEntity.preRemoveSideEffects, Containers.dropContents
//   ChestBlock (updateShape, useWithoutItem, combine, isChestBlockedAt), DoubleBlockCombiner,
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

constexpr u64 kRecheckDelay = 5;           // ContainerOpenersCounter.CHECK_TICK_DELAY
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

// Level.setBlock(pos, state, flags) with everything that hangs on it: the block entity of the
// old block goes (its contents fall out), the one of the new block comes, the new block is
// told it was placed, and the six neighbours are told that something changed beside them.
// Every change of a block the game makes goes through here. `flags` are vanilla's (kUpdate*):
// what a player does is kUpdateAll; a command leaves parts of it out.
bool Game::set_block(BlockPos pos, BlockState state, u32 flags) {
    const BlockState old = world_.block(pos.x, pos.y, pos.z);
    if (!world_.set_block(pos.x, pos.y, pos.z, state)) return false;
    const world::BlockInfo& was = world::block_info_of(old);
    const world::BlockInfo& now = world::block_info_of(state);
    // A block entity that went with the old block: its entry stays (dead, and empty) until the
    // menus that are open on it have been closed.
    u32 gone_kind = block_kind::kPlain, gone = 0;
    const auto forget = [&] {
        if (gone_kind == block_kind::kPlain) return;
        // In vanilla a menu finds its container gone at its player's next tick: it shows the
        // slots that emptied, then closes (stillValid). Here that happens now, once the
        // neighbours have heard of the change - the other half of a double chest is a chest of
        // its own by the time its lid is heard to fall.
        close_menus_on(gone_kind, gone);
        if (gone_kind == block_kind::kChest) {
            chests_[gone] = {};
            chest_free_.push_back(gone);
        } else {
            furnaces_[gone] = {};
            furnace_free_.push_back(gone);
        }
    };
    if (&was != &now) {  // LevelChunk.setBlockState: another block, not another state of the same
        if ((world::flags_of(old) & world::state::kBlockEntity) && world_.remove_block_entity(pos.x, pos.y, pos.z, gone)) {
            // BaseContainerBlockEntity.preRemoveSideEffects: Containers.dropContents.
            // ...unless the change says to leave that out: then what it held is gone with it.
            gone_kind = was.kind;
            const bool spill = !(flags & kUpdateSkipContainer);
            if (gone_kind == block_kind::kChest) {
                chests_[gone].alive = false;  // ChestBlockEntity.stopOpen does nothing for a removed chest
                if (spill) drop_contents(pos, chests_[gone].items, Chest::kSlots);
            } else {
                furnaces_[gone].alive = false;
                if (spill) drop_contents(pos, furnaces_[gone].items, Furnace::kSlots);
            }
        }
        if (now.kind == block_kind::kChest || now.kind == block_kind::kFurnace) {  // EntityBlock.newBlockEntity
            const bool chest = now.kind == block_kind::kChest;
            std::vector<u32>& free = chest ? chest_free_ : furnace_free_;
            u32 data;
            if (!free.empty()) {
                data = free.back();
                free.pop_back();
            } else if (chest) {
                data = static_cast<u32>(chests_.size());
                chests_.emplace_back();
            } else {
                data = static_cast<u32>(furnaces_.size());
                furnaces_.emplace_back();
            }
            if (chest) {
                chests_[data].alive = true;
                chests_[data].x = pos.x, chests_[data].y = pos.y, chests_[data].z = pos.z;
            } else {
                furnaces_[data].alive = true;
                furnaces_[data].x = pos.x, furnaces_[data].y = pos.y, furnaces_[data].z = pos.z;
            }
            world_.add_block_entity(pos.x, pos.y, pos.z, now.block_entity, data);
        }
        // BlockState.onPlace. TntBlock: next to a redstone signal it goes off at once.
        if (!(flags & kUpdateSkipOnPlace) && now.kind == block_kind::kTnt && has_neighbour_signal(pos)) {
            prime_tnt(pos, 80, true);
            set_block(pos, world::kAir);
            forget();
            return true;
        }
    }
    // Level.markAndNotifyBlock: updateNeighborsAt, then BlockState.updateNeighbourShapes - each
    // neighbour takes the shape it has beside the new block (Block.updateOrDestroy: set without
    // telling its own neighbours again).
    if (flags & kUpdateNeighbors) update_neighbors_at(pos);
    if (!(flags & kUpdateKnownShape)) {
        for (u32 dir = 0; dir < 6; ++dir) {
            const BlockPos at = step(pos, dir);
            const BlockState there = world_.block(at.x, at.y, at.z);
            if (LIKELY(there == world::kAir) || !is_chest(there)) continue;  // (SnowyBlock's is world::Dimension's own)
            const BlockState shaped = update_shape(there, world::opposite(dir), world_.block(pos.x, pos.y, pos.z));
            if (shaped != there) set_block(at, shaped, flags & ~kUpdateNeighbors);
        }
    }
    forget();
    return true;
}

// Level.updateNeighborsAt: the six blocks around `pos` hear that it changed.
void Game::update_neighbors_at(BlockPos pos) {
    for (u32 dir = 0; dir < 6; ++dir) neighbor_changed(step(pos, dir));
}

// BlockBehaviour.neighborChanged, for the blocks that override it.
void Game::neighbor_changed(BlockPos pos) {
    const BlockState state = world_.block(pos.x, pos.y, pos.z);
    if (LIKELY(state == world::kAir)) return;
    if (world::block_info_of(state).kind == block_kind::kTnt && has_neighbour_signal(pos)) {  // TntBlock.neighborChanged
        prime_tnt(pos, 80, true);
        set_block(pos, world::kAir);
    }
}

// BlockState.updateShape: what `state` becomes when its neighbour in direction `dir` is
// `neighbour` - for the blocks that override it, itself for the others.
BlockState Game::update_shape(BlockState state, u32 dir, BlockState neighbour) const {
    const world::BlockInfo& info = world::block_info_of(state);
    if (info.kind == block_kind::kChest) {  // ChestBlock.updateShape
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
    // SnowyBlock.updateShape: snowy while what lies on it is snow.
    if ((info.flags & world::block_flag::kPlaceMask) == world::block_flag::kPlaceSnowy && dir == world::kUp)
        return info.alt[(world::block_info_of(neighbour).flags & world::block_flag::kSnowTag) ? 1 : 0];
    return state;
}

// Block.updateFromNeighbourShapes: the state as its six neighbours would have it, asked in
// vanilla's order (BlockBehaviour.UPDATE_SHAPE_ORDER: west, east, north, south, down, up).
BlockState Game::update_from_neighbour_shapes(BlockState state, BlockPos pos) const {
    static constexpr u32 kOrder[6] = {world::kWest, world::kEast, world::kNorth, world::kSouth, world::kDown, world::kUp};
    for (const u32 dir : kOrder) {
        const BlockPos at = step(pos, dir);
        state = update_shape(state, dir, state_for_client(at));
    }
    return state;
}

// Containers.dropContents: every stack falls out in pieces of 10 to 30, all pieces of a stack
// from one random place inside the block. (Vanilla tosses them; nothing moves items yet.)
void Game::drop_contents(BlockPos pos, ItemStack* items, u32 n) {
    constexpr double kItemWidth = 0.25, kRange = 1.0 - kItemWidth, kHalf = kItemWidth / 2.0;
    for (u32 i = 0; i < n; ++i) {
        ItemStack stack = items[i].take();
        if (stack.empty()) continue;
        const double x = pos.x + random_double() * kRange + kHalf;
        const double y = pos.y + random_double() * kRange;
        const double z = pos.z + random_double() * kRange + kHalf;
        while (!stack.empty()) spawn_item(stack.split(random_int(21) + 10), x, y, z, 0);  // no pick-up delay
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
                        if (blocked(other)) return true;
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
        if (!c.recheck) {
            c.recheck = true;
            // Scheduled while packets are handled: the level's clock is one tick behind, so
            // the fifth tick from "now" is the fourth from this one.
            rechecks_.push_back({tick_ + kRecheckDelay - 1, c.x, c.y, c.z});
        }
    }
    block_events_.push_back({c.x, c.y, c.z, kEventOpenCount, static_cast<u8>(c.open)});
}

// ContainerOpenersCounter.decrementOpeners
void Game::chest_closed(u32 chest) {
    Chest& c = chests_[chest];
    if (c.open > 0 && --c.open == 0) chest_sound(c, gen::sound::kBlockChestClose);
    block_events_.push_back({c.x, c.y, c.z, kEventOpenCount, static_cast<u8>(c.open)});
}

// The scheduled ChestBlock.tick -> ChestBlockEntity.recheckOpen -> recheckOpeners: every five
// ticks an open chest counts again who has it open and says so - which is how someone who comes
// into view later sees its lid up. Runs where vanilla runs block ticks: first thing in the level.
void Game::recheck_chests() {
    while (recheck_head_ < rechecks_.size() && rechecks_[recheck_head_].tick <= tick_) {
        const Recheck r = rechecks_[recheck_head_++];
        const world::BlockEntityRef* be = world_.block_entity(r.x, r.y, r.z);
        if (!be || be->type != be_type::kChest) continue;  // the chest is gone
        const u32 chest = be->data;
        Chest& c = chests_[chest];
        c.recheck = false;
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
        block_events_.push_back({c.x, c.y, c.z, kEventOpenCount, static_cast<u8>(open)});
        if (open > 0) {
            c.recheck = true;
            rechecks_.push_back({tick_ + kRecheckDelay, c.x, c.y, c.z});
        }
    }
    if (recheck_head_ == rechecks_.size()) {
        rechecks_.clear();
        recheck_head_ = 0;
    } else if (recheck_head_ > 1024) {
        rechecks_.erase(rechecks_.begin(), rechecks_.begin() + static_cast<std::ptrdiff_t>(recheck_head_));
        recheck_head_ = 0;
    }
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
