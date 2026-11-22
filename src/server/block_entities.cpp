// SPDX-License-Identifier: GPL-3.0-or-later
// Blocks that are more than a state: chests - of wood and of copper -, the barrel, the furnace,
// the smoker and the blast furnace (block entities) and the crafting table (a menu without
// one). What happens around a block when it changes is server/block_updates.cpp.
//
// The rules are vanilla's, read from the 26.3 jar; each function names the method it follows:
//   BaseContainerBlockEntity.preRemoveSideEffects, Containers.dropContents
//   ChestBlock (updateShape, tick, useWithoutItem, combine, isChestBlockedAt), DoubleBlockCombiner,
//   CopperChestBlock (chestCanConnectTo, getLeastOxidizedChestOfConnectedBlocks, updateShape),
//   WeatheringCopperChestBlock.randomTick,
//   ChestBlockEntity + ContainerOpenersCounter (who has it open: the lid, the sound, the recount)
//   BarrelBlock, BarrelBlockEntity (the same count; `open` and its own sounds instead of a lid)
//   AbstractFurnaceBlock / FurnaceBlock, SmokerBlock, BlastFurnaceBlock.openContainer,
//   CraftingTableBlock, TntBlock.useItemOn
//   ServerPlayer.openMenu / closeContainer / doCloseContainer, the stillValid of each menu
//   ServerLevel.blockEvent / runBlockEvents
// What a block entity holds and how a furnace burns is server/block_entity.cpp; what a click in
// a menu does is server/inventory.cpp.
//
// What is NOT here:
//   * a furnace gives no experience (no orbs yet), and nothing is remembered about its recipes;
//   * comparators (affectNeighborsAfterRemoval, getAnalogOutputSignal): there are none;
//   * custom names: a chest named on an anvil cannot be placed (its item carries a component);
//   * wax and axes: a copper chest is waxed, scraped or made by no item here (HoneycombItem,
//     AxeItem; the copper golem that sorts into them does not exist);
//   * what a leaving player has on a menu's cursor or in a crafting grid is not dropped: a
//     player's inventory is not kept either;
//   * ContainerOpenersCounter counts by a box around the chest; here a player counts while its
//     menu is open on the chest - and that closes when it walks away (stillValid).
#include <algorithm>
#include <cstring>

#include "item/codec.hpp"
#include "nbt/nbt.hpp"
#include "nbt/ops.hpp"
#include "server/command.hpp"
#include "server/game.hpp"
#include "text/component.hpp"
#include "text/text.hpp"

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
// (One layout for every chest - props::chest -, each from its own first state.)
ChestState chest_state(BlockState s) {
    const u32 off = s - world::block_info_of(s).first_state;
    return {world::kNorth + off / props::chest::kFacingStride, off % props::chest::kFacingStride / props::chest::kTypeStride,
            off % props::chest::kTypeStride == 0};
}
BlockState chest_block(const world::BlockInfo& block, const ChestState& c) {
    return static_cast<BlockState>(block.first_state + (c.facing - world::kNorth) * props::chest::kFacingStride +
                                   c.type * props::chest::kTypeStride + (c.waterlogged ? 0 : 1));
}
bool is_chest(BlockState s) { return world::block_info_of(s).kind == block_kind::kChest; }
// ChestBlock.chestCanConnectTo: a chest joins its own block - a copper chest any copper chest
// (CopperChestBlock: #copper_chests).
bool chest_connects(const world::BlockInfo& chest, BlockState to) {
    const world::BlockInfo& other = world::block_info_of(to);
    if (other.kind != block_kind::kChest) return false;
    return copper_chest(chest) ? copper_chest(other) : &chest == &other;
}
// CopperChestBlock.weatherState, as BlockInfo::weather has it: of the chest, or of the chest it
// is without its wax.
u32 chest_age(const world::BlockInfo& copper) {
    return copper.weather ? copper.weather : world::block_info_of(static_cast<BlockState>(copper.alt[2])).weather;
}
// BarrelBlock's state: facing (north, east, south, west, up, down), open.
constexpr u8 kBarrelFacing[6] = {world::kNorth, world::kEast, world::kSouth, world::kWest, world::kUp, world::kDown};
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
    title_barrel_ = translatable("container.barrel");
    title_smoker_ = translatable("container.smoker");
    title_blast_furnace_ = translatable("container.blast_furnace");
}

// ---------------------------------------------------------------------------- blocks changing

// ChestBlock.updateShape: what the chest `state` becomes when its neighbour in direction `dir`
// is `neighbour`. (A waterlogged one would ask for a tick of its water: there are no fluid ticks.)
BlockState Game::chest_update_shape(BlockState state, u32 dir, BlockState neighbour) const {
    const world::BlockInfo& block = world::block_info_of(state);
    ChestState c = chest_state(state);
    const bool horizontal = dir >= world::kNorth;
    const bool connects = chest_connects(block, neighbour);
    if (connects && horizontal) {
        // A single chest joins the half that was just placed against it.
        const ChestState n = chest_state(neighbour);
        if (c.type == kSingle && n.type != kSingle && c.facing == n.facing && connected_direction(n) == world::opposite(dir))
            c.type = n.type == kLeft ? kRight : kLeft;
    } else if (connected_direction(c) == dir) {
        c.type = kSingle;  // the other half is gone
    }
    // CopperChestBlock.updateShape: a half takes the block of its other half - the one that was
    // placed against it (already the less weathered of the two), or the one that just weathered.
    if (connects && copper_chest(block) && c.type != kSingle && connected_direction(c) == dir)
        return chest_block(world::block_info_of(neighbour), c);
    return chest_block(block, c);
}

// WeatheringCopperChestBlock.randomTick: not the right half of a pair, and not while somebody has
// it open (ChestBlockEntity.getEntitiesWithContainerOpen - of a pair, both halves count the same).
bool Game::copper_chest_may_weather(BlockState state, BlockPos pos) const {
    if (chest_state(state).type == kRight) return false;
    const world::BlockEntityRef* be = world_.block_entity(pos.x, pos.y, pos.z);
    return be && ((be->data & kOpaqueBlockEntity) || chests_[be->data].open == 0);
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
BlockState Game::chest_for_placement(u32 pi, const world::BlockInfo& block, BlockPos at, u32 face) const {
    // candidatePartnerFacing: the facing of a single chest in direction `dir`, kNone without one.
    const auto partner = [&](u32 dir) {
        const BlockPos n = step(at, dir);
        const BlockState s = world_.block(n.x, n.y, n.z);
        if (!chest_connects(block, s)) return kNone;
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
    // CopperChestBlock.getLeastOxidizedChestOfConnectedBlocks: half of a pair is the block of
    // the less weathered of the two - and of neither's wax, if only one of them has any.
    if (c.type != kSingle && copper_chest(block)) {
        const BlockPos n = step(at, connected_direction(c));
        const world::BlockInfo& other = world::block_info_of(world_.block(n.x, n.y, n.z));
        if (other.kind == block_kind::kChest && copper_chest(other)) {
            const bool waxed = block.alt[2] != 0, other_waxed = other.alt[2] != 0;
            const world::BlockInfo& mine =
                waxed && !other_waxed ? world::block_info_of(static_cast<BlockState>(block.alt[2])) : block;
            const world::BlockInfo& theirs =
                other_waxed && !waxed ? world::block_info_of(static_cast<BlockState>(other.alt[2])) : other;
            return chest_block(chest_age(block) <= chest_age(other) ? mine : theirs, c);
        }
    }
    return chest_block(block, c);
}

// ---------------------------------------------------------------------------- clicking a block

namespace {
std::span<const u8> nameless(nbt::Tag entry, base::ByteBuf& scratch);
}  // namespace

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
        case block_kind::kFurnace: {  // AbstractFurnaceBlock.useWithoutItem -> openContainer of the three
            if (hand != cb::SwingAnimation::kMainHand) return false;
            const world::BlockEntityRef* be = world_.block_entity(pos.x, pos.y, pos.z);
            if (!be) return true;
            if (be->data & kOpaqueBlockEntity) return true;
            const u32 data = be->data;
            // FurnaceMenu, SmokerMenu, BlastFurnaceMenu: one menu, three types and names of it.
            const item::Cooker cooker = furnaces_[data].type;
            const u32 menu = cooker == item::Cooker::kSmoking    ? gen::menu::kSmoker
                             : cooker == item::Cooker::kBlasting ? gen::menu::kBlastFurnace
                                                                 : gen::menu::kFurnace;
            const std::string_view key = cooker == item::Cooker::kSmoking    ? "container.smoker"
                                         : cooker == item::Cooker::kBlasting ? "container.blast_furnace"
                                                                             : "container.furnace";
            const std::span<const u8> plain = cooker == item::Cooker::kSmoking    ? std::span<const u8>(title_smoker_)
                                              : cooker == item::Cooker::kBlasting ? std::span<const u8>(title_blast_furnace_)
                                                                                  : std::span<const u8>(title_furnace_);
            nbt::Doc kept;
            base::ByteBuf named;
            const nbt::Tag name = kept_entry(furnaces_[data].keep, "CustomName", kept);
            const std::span<const u8> title = name ? nameless(name, named) : plain;
            if (!unlocks(furnaces_[data].keep,
                         p.menu.inv.selected_item())) {  // BaseContainerBlockEntity.createMenu: canOpen, or the rattle
                chest_locked(pi, pos.x + 0.5, pos.y + 0.5, pos.z + 0.5,
                             name ? text::Text::encoded(title) : text::Text::translatable(key));
                return true;
            }
            open_menu(pi, Menu::Kind::kFurnace, menu, title, 0);
            p.open_pos = pos;
            p.open_data[0] = data;
            p.menu.send_all(out(pi), p.threshold, containers(p));
            return true;
        }
        case block_kind::kBarrel: {  // BarrelBlock.useWithoutItem: a ChestMenu of three rows, whatever is on top of it
            if (hand != cb::SwingAnimation::kMainHand) return false;
            const world::BlockEntityRef* be = world_.block_entity(pos.x, pos.y, pos.z);
            if (!be) return true;
            if (be->data & kOpaqueBlockEntity) return true;
            const u32 barrel = be->data;
            nbt::Doc kept;
            base::ByteBuf named;
            const nbt::Tag name = kept_entry(chests_[barrel].keep, "CustomName", kept);
            const std::span<const u8> title = name ? nameless(name, named) : std::span<const u8>(title_barrel_);
            if (!unlocks(chests_[barrel].keep, p.menu.inv.selected_item())) {
                chest_locked(pi, pos.x + 0.5, pos.y + 0.5, pos.z + 0.5,
                             name ? text::Text::encoded(title) : text::Text::translatable("container.barrel"));
                return true;
            }
            if (p.menu.kind() != Menu::Kind::kInventory) close_menu(pi, true);
            chest_opened(barrel);  // Container.startOpen comes before the screen opens
            open_menu(pi, Menu::Kind::kChest, gen::menu::kGeneric9x3, title, 3);
            p.open_pos = pos;
            p.open_data[0] = barrel;
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
                if (world::block_of(os) == world::block_of(state)) {  // combineWithNeigbour: the same block
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
            // The name: its own - of a pair the first half's, else the second's (ChestBlock's
            // MenuProvider.getDisplayName) - or what a chest is called.
            nbt::Doc kept;
            base::ByteBuf named;
            nbt::Tag name = kept_entry(chests_[first].keep, "CustomName", kept);
            if (!name && second != kNone) name = kept_entry(chests_[second].keep, "CustomName", kept);
            const std::span<const u8> title = name              ? nameless(name, named)
                                              : second == kNone ? std::span<const u8>(title_chest_)
                                                                : std::span<const u8>(title_double_chest_);
            // createMenu: both halves have to open (canOpen), or it rattles - from between them.
            if (!unlocks(chests_[first].keep, p.menu.inv.selected_item()) ||
                (second != kNone && !unlocks(chests_[second].keep, p.menu.inv.selected_item()))) {
                const Chest& a = chests_[first];
                const double dx = second == kNone ? 0.0 : (chests_[second].x - a.x) / 2.0,
                             dz = second == kNone ? 0.0 : (chests_[second].z - a.z) / 2.0;
                chest_locked(pi, a.x + 0.5 + dx, a.y + 0.5, a.z + 0.5 + dz,
                             name ? text::Text::encoded(title)
                                  : text::Text::translatable(second == kNone ? "container.chest" : "container.chestDouble"));
                return true;
            }
            // new ChestMenu: Container.startOpen comes before the screen opens.
            if (p.menu.kind() != Menu::Kind::kInventory) close_menu(pi, true);
            chest_opened(first);
            if (second != kNone) chest_opened(second);
            open_menu(pi, Menu::Kind::kChest, second == kNone ? gen::menu::kGeneric9x3 : gen::menu::kGeneric9x6, title,
                      second == kNone ? 3 : 6);
            p.open_pos = pos;
            p.open_data[0] = first;
            p.open_data[1] = second;
            p.menu.send_all(out(pi), p.threshold, containers(p));
            return true;
        }
        // FenceBlock.useWithoutItem -> LeadItem.bindPlayerMobs: the mobs the player leads are tied
        // to the fence. PASS when it leads none - and nothing is on a lead here.
        case block_kind::kFence: return false;
        default: return false;
    }
}

// ---------------------------------------------------------------------------- names and locks

namespace {

// A text component that is kept as an entry of a tag, as a tag of its own (type, payload).
std::span<const u8> nameless(nbt::Tag entry, base::ByteBuf& scratch) {
    scratch.clear();
    *scratch.reserve(1) = entry.type;
    ++scratch.len;
    scratch.append(entry.bytes().data(), entry.bytes().size());
    return {scratch.data, scratch.len};
}

// ItemPredicate.test, of a lock: is `held` one of `items`, so many, and with these components exactly?
bool predicate_matches(nbt::Tag lock, const ItemStack& held) {
    if (const nbt::Tag items = lock.get("items")) {  // a HolderSet: "#tag", a name, or a list of names
        if (held.empty()) return false;
        const std::string_view own = world::item_name(held.item());
        bool found = false;
        if (items.type == nbt::kString && items.str().starts_with('#')) {
            std::vector<u16> members;
            cmd::tag_members("minecraft:item", items.str().substr(1), members);
            found = std::find(members.begin(), members.end(), held.item()) != members.end();
        } else if (items.type == nbt::kString) {
            found = items.str() == own;
        } else {
            for (const nbt::Tag e : items.elements()) found |= e.str() == own;
        }
        if (!found) return false;
    }
    if (const nbt::Tag count = lock.get("count")) {  // MinMaxBounds.Ints: one number, or {min, max}
        const i32 have = static_cast<i32>(held.count());
        if (count.type == nbt::kCompound) {
            const nbt::Tag min = count.get("min"), max = count.get("max");
            if ((min && have < min.as_int()) || (max && have > max.as_int())) return false;
        } else if (have != count.as_int()) {
            return false;
        }
    }
    if (const nbt::Tag components = lock.get("components")) {  // DataComponentExactPredicate: equal, each of them
        base::ByteBuf expected;
        for (const nbt::Tag e : components.entries()) {
            const std::string key = nbt::to_utf8(e.name());
            const u32 type = item::find_component(key);
            expected.clear();
            if (type == item::kNoComponent || item::value_from_nbt(type, e, expected) != item::Verdict::kOk) return false;
            const item::Patches::Found has = held.empty() ? item::Patches::Found{} : item::effective(held, type);
            if (has.state != item::Patches::State::kSet || has.value.size() != expected.len ||
                std::memcmp(has.value.data(), expected.data, expected.len) != 0)
                return false;
        }
    }
    return true;
}

}  // namespace

bool Game::unlocks(u32 keep, const ItemStack& held) const {
    nbt::Doc doc;
    const nbt::Tag lock = kept_entry(keep, "lock", doc);
    return !lock || predicate_matches(lock, held);
}

void Game::chest_locked(u32 pi, double x, double y, double z, const text::Text& name) {
    system_message(pi, text::Text::translatable("container.isLocked").arg(name), true);
    play_sound(gen::sound::kBlockChestLocked, cb::Sound::kSourceBlocks, x, y, z, 1.0f, 1.0f);
}

// ---------------------------------------------------------------------------- a block item's components

// BlockItem.updateBlockStateFromTag, updateCustomBlockEntityTag and updateBlockEntityComponents
// can all be done here for this stack - or the block is not placed (a chest whose item names a
// loot table would have to roll it when it is opened).
bool Game::can_apply_item_to_block(u32 block, const ItemStack& stack) {
    if (!stack.patch()) return true;
    const u32 kind = world::block_info(block).kind;
    if (chest_kind(kind) && item::effective(stack, item::component::kContainerLoot).state == item::Patches::State::kSet)
        return false;
    const item::Patches::Found data = item::effective(stack, item::component::kBlockEntityData);
    if (data.state != item::Patches::State::kSet || (!chest_kind(kind) && kind != block_kind::kFurnace)) return true;
    proto::Reader r(data.value);
    if (r.varint() != world::block_info(block).block_entity) return true;  // of another type: not looked at
    nbt::Doc doc;
    std::string why;
    return doc.parse({r.p, r.remaining()}, nbt::Form::kNameless, nbt::kTrusted) == nbt::Error::kNone &&
           can_load_block_entity(block, doc.root(), why);
}

void Game::apply_item_to_block(BlockPos pos, const ItemStack& stack) {
    namespace comp = item::component;
    using State = item::Patches::State;
    // ---- updateBlockStateFromTag: BlockItemStateProperties.apply - the properties the block has, set to the values they have
    if (const item::Patches::Found properties = item::effective(stack, comp::kBlockState); properties.state == State::kSet) {
        const BlockState placed = world_.block(pos.x, pos.y, pos.z);
        const u32 block = world::block_of(placed), first = world::block_info(block).first_state;
        BlockState state = placed;
        proto::Reader r(properties.value);
        for (u32 n = r.varint(); n > 0 && r.ok; --n) {
            const std::string_view name = r.str(32767), value = r.str(32767);
            for (u32 i = 0, count = world::block_property_count(block); i < count; ++i) {
                const world::BlockProperty p = world::block_property(block, i);
                if (p.name != name) continue;
                const auto it = std::find(p.values.begin(), p.values.end(), value);
                if (it == p.values.end()) break;
                const i32 now = static_cast<i32>((state - first) / p.stride % p.values.size());
                state =
                    static_cast<BlockState>(state + (static_cast<i32>(it - p.values.begin()) - now) * static_cast<i32>(p.stride));
                break;
            }
        }
        if (state != placed) set_block(pos, state, kUpdateClients);
    }
    const world::BlockEntityRef* ref = world_.block_entity(pos.x, pos.y, pos.z);
    if (!ref || (ref->data & kOpaqueBlockEntity)) return;
    const u32 kind = world::block_info_of(world_.block(pos.x, pos.y, pos.z)).kind;
    const bool chest = chest_kind(kind);
    if (!chest && kind != block_kind::kFurnace) return;
    bool changed = false;
    std::string why;
    // ---- updateCustomBlockEntityTag: TypedEntityData.loadInto - what it saves, with the item's tag put over it
    if (const item::Patches::Found data = item::effective(stack, comp::kBlockEntityData); data.state == State::kSet) {
        proto::Reader r(data.value);
        nbt::Doc over, now;
        base::ByteBuf saved, merged;
        if (r.varint() == ref->type &&
            over.parse({r.p, r.remaining()}, nbt::Form::kNameless, nbt::kTrusted) == nbt::Error::kNone &&
            save_block_entity(pos.x, pos.y, pos.z, saved) == Saved::kOk &&
            now.parse({saved.data, saved.len}, nbt::Form::kNameless, nbt::kTrusted) == nbt::Error::kNone) {
            *merged.reserve(1) = nbt::kCompound;
            ++merged.len;
            nbt::merge(now.root(), over.root(), merged);
            nbt::Doc all;
            if (all.parse({merged.data, merged.len}, nbt::Form::kNameless, nbt::kTrusted) == nbt::Error::kNone)
                load_block_entity(pos, all.root(), false, changed, why);
        }
    }
    // ---- updateBlockEntityComponents: BlockEntity.applyComponents. What the block entity takes
    // for itself (applyImplicitComponents): its name, its lock, what it holds...
    ref = world_.block_entity(pos.x, pos.y, pos.z);
    ItemStack* const items = chest ? chests_[ref->data].items : furnaces_[ref->data].items;
    const u32 slots = chest ? Chest::kSlots : Furnace::kSlots;
    u32& keep = chest ? chests_[ref->data].keep : furnaces_[ref->data].keep;
    for (u32 i = 0; i < slots; ++i) items[i] = {};  // ItemContainerContents.copyInto: every slot, from the item or empty
    if (const item::Patches::Found held = item::effective(stack, comp::kContainer); held.state == State::kSet) {
        std::vector<item::Contained> stacks;
        item::contained(comp::kContainer, held.value, stacks);
        for (const item::Contained& c : stacks)
            if (c.slot < slots) items[c.slot] = item::stack_of(c);
    }
    // ...and the rest of the stack's patch is the block entity's components: what was set, less
    // what it took (and block_entity_data, block_state).
    base::ByteBuf rest, value;
    nbt::Writer w(rest);
    w.root(nbt::kCompound);
    u32 entries = 0;
    nbt::Doc kept;
    if (const nbt::Tag used = kept_entry(keep, "RecipesUsed", kept)) {
        w.put_raw("RecipesUsed", used.type, used.bytes());
        ++entries;
    }
    if (const item::Patches::Found name = item::effective(stack, comp::kCustomName); name.state == State::kSet) {
        w.put_raw("CustomName", static_cast<nbt::Type>(name.value[0]), name.value.subspan(1));
        ++entries;
    }
    if (const item::Patches::Found lock = item::effective(stack, comp::kLock); lock.state == State::kSet) {
        w.put_raw("lock", static_cast<nbt::Type>(lock.value[0]), lock.value.subspan(1));
        ++entries;
    }
    if (stack.patch()) {
        base::ByteBuf map;
        nbt::Writer mw(map);
        u32 n = 0;
        for (const item::Patches::Entry& e : item::patches().entries(stack.patch())) {
            const bool taken = e.type == comp::kBlockEntityData || e.type == comp::kBlockState || e.type == comp::kCustomName ||
                               e.type == comp::kLock || e.type == comp::kContainer || (chest && e.type == comp::kContainerLoot);
            if (e.removed || taken || item::transient(e.type)) continue;
            item::value_to_nbt(e.type, item::patches().wire(stack.patch()).subspan(e.off, e.len), mw,
                               item::component_name(e.type));
            ++n;
        }
        if (n) {
            mw.end_compound();
            w.put_raw("components", nbt::kCompound, {map.data, map.len});
            ++entries;
        }
    }
    w.end_compound();
    drop_kept(keep);
    if (entries) keep = keep_tag({rest.data, rest.len});
    world_.mark_dirty(pos.x, pos.z);
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
    const Menu::Kind menu = chest_kind(kind) ? Menu::Kind::kChest : Menu::Kind::kFurnace;
    for (u32 pi = 0; pi < players_.size(); ++pi) {
        Player& p = players_[pi];
        if (p.menu.kind() != menu || (p.open_data[0] != data && p.open_data[1] != data)) continue;
        p.menu.send_changes(out(pi), p.threshold, containers(p));
        close_menu(pi, true);
    }
}

// ---------------------------------------------------------------------------- chests and barrels: who has one open

// ContainerOpenersCounter.onOpen / onClose, as the two block entities have them.
// ChestBlockEntity.playSound: its block's own sound (wood, copper, copper that has weathered),
// from the middle of the chest - of both halves, for a double one, whose left half stays silent.
// BarrelBlockEntity: its sound from the middle of the face that opens, and `open` on the block.
void Game::lid_moved(u32 chest, bool open) {
    const BlockPos pos{chests_[chest].x, chests_[chest].y, chests_[chest].z};
    const BlockState state = world_.block(pos.x, pos.y, pos.z);
    const world::BlockInfo& info = world::block_info_of(state);
    const float pitch = random_float() * 0.1f + 0.9f;
    if (info.kind == block_kind::kBarrel) {
        const u32 off = state - props::barrel::kFirst;
        const u32 facing = kBarrelFacing[off / props::barrel::kFacingStride];
        play_sound(open ? gen::sound::kBlockBarrelOpen : gen::sound::kBlockBarrelClose, cb::Sound::kSourceBlocks,
                   (pos.x + 0.5) + world::kStepX[facing] / 2.0, (pos.y + 0.5) + world::kStepY[facing] / 2.0,
                   (pos.z + 0.5) + world::kStepZ[facing] / 2.0, 0.5f, pitch);
        // updateBlockState: setBlockAndUpdate - the block entity stays, the neighbours hear of it.
        set_block(pos, static_cast<BlockState>(state - off % props::barrel::kFacingStride + (open ? 0 : 1)));
        return;
    }
    if (info.kind != block_kind::kChest) return;  // (onOpen: the block is no chest any more)
    const ChestState s = chest_state(state);
    if (s.type == kLeft) return;
    double x = pos.x + 0.5, z = pos.z + 0.5;
    if (s.type == kRight) {
        const u32 dir = connected_direction(s);
        x += world::kStepX[dir] * 0.5;
        z += world::kStepZ[dir] * 0.5;
    }
    play_sound(info.alt[open ? 0 : 1], cb::Sound::kSourceBlocks, x, pos.y + 0.5, z, 0.5f, pitch);
}

// ContainerOpenersCounter.incrementOpeners: the first opener is heard and starts the recount;
// every one moves a chest's lid (ChestBlockEntity.signalOpenCount: a block event with the count -
// a barrel's openerCountChanged does nothing).
void Game::chest_opened(u32 chest) {
    const BlockPos pos{chests_[chest].x, chests_[chest].y, chests_[chest].z};
    if (chests_[chest].open++ == 0) {
        lid_moved(chest, true);
        // ContainerOpenersCounter.scheduleRecheck: a tick of the block (one per chest at a time).
        schedule_tick(pos, world_.block(pos.x, pos.y, pos.z), kRecheckDelay);
    }
    if (is_chest(world_.block(pos.x, pos.y, pos.z)))
        block_event(pos.x, pos.y, pos.z, kEventOpenCount, static_cast<u8>(chests_[chest].open));
}

// ContainerOpenersCounter.decrementOpeners
void Game::chest_closed(u32 chest) {
    const BlockPos pos{chests_[chest].x, chests_[chest].y, chests_[chest].z};
    if (chests_[chest].open > 0 && --chests_[chest].open == 0) lid_moved(chest, false);
    if (is_chest(world_.block(pos.x, pos.y, pos.z)))
        block_event(pos.x, pos.y, pos.z, kEventOpenCount, static_cast<u8>(chests_[chest].open));
}

// ChestBlock.tick / BarrelBlock.tick -> recheckOpen -> ContainerOpenersCounter.recheckOpeners:
// every five ticks an open chest counts again who has it open and says so - which is how someone
// who comes into view later sees its lid up.
void Game::recheck_chest(BlockPos pos) {
    const world::BlockEntityRef* be = world_.block_entity(pos.x, pos.y, pos.z);
    const bool barrel = be && be->type == be_type::kBarrel;
    if (!be || (be->type != be_type::kChest && !barrel) || (be->data & kOpaqueBlockEntity)) return;
    const u32 chest = be->data;
    u32 open = 0;
    for (const Player& p : players_)
        open += p.menu.kind() == Menu::Kind::kChest && (p.open_data[0] == chest || p.open_data[1] == chest);
    if (const u32 was = chests_[chest].open; open != was) {
        chests_[chest].open = static_cast<u16>(open);
        if (open != 0 && was == 0)
            lid_moved(chest, true);
        else if (open == 0)
            lid_moved(chest, false);
    }
    if (!barrel) block_event(pos.x, pos.y, pos.z, kEventOpenCount, static_cast<u8>(open));
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
            const BlockState state = world_.block(pos.x, pos.y, pos.z);
            set_block(pos, static_cast<BlockState>(
                               state - (state - world::block_info_of(state).first_state) % props::furnace::kFacingStride +
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
