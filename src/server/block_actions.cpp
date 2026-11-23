// SPDX-License-Identifier: GPL-3.0-or-later
// What a (creative) player can do to blocks, and how the result reaches everyone who sees it.
//
// The rules are vanilla's, read from the 26.3 jar; each function names the method it follows:
//   ServerGamePacketListenerImpl.handlePlayerAction / handleUseItemOn / handleUseItem /
//     handlePickItemFromBlock / handlePickItemFromEntity / tryPickItem
//   ServerPlayerGameMode.handleBlockBreakAction / destroyBlock / useItemOn / useItem
//   BlockItem.place, BlockPlaceContext, CollisionGetter.isUnobstructed
//   ChunkHolder.broadcastChanges, ServerGamePacketListenerImpl.tick (the acknowledgement)
//
// What is NOT here yet, and deliberately has no stand-in: block behaviour beyond storing a state.
// A block is placeable only if every hook vanilla would run for it is implemented (the generated
// BlockInfo::flags placement kind, see tools/mcgen/gen.py placement_kind), so the world never holds
// a block whose reaction to being clicked, updated or broken we would have to invent.
#include <cmath>

#include "base/log.hpp"
#include "phys/mth.hpp"
#include "server/game.hpp"
#include "server/viewers.hpp"
#include "world/chunk_codec.hpp"

namespace server {

using namespace proto;
using world::BlockState;
namespace block_flag = world::block_flag;
namespace item_flag = world::item_flag;

namespace {

// Attributes.BLOCK_INTERACTION_RANGE is 4.5; ServerPlayer adds 0.5 in creative. The server then
// checks with some slack (isWithinBlockInteractionRange(pos, buffer)): one block for a click,
// four for a menu that is open on a block.
constexpr double kReach = 4.5 + 0.5;
// Avatar.POSES: the two poses a creative player on foot can be in. Floats, widened exactly as
// vanilla does (EntityDimensions.makeBoundingBox, Entity.getEyeY).
constexpr float kEyeStanding = 1.62f, kEyeCrouching = 1.27f;
constexpr float kHalfWidth = 0.6f / 2.0f, kHeightStanding = 1.8f, kHeightCrouching = 1.5f;
// handleUseItemOn: how far from the centre of the clicked block the hit location may be, per axis.
constexpr double kHitLimit = 1.0000001;
// Shapes.joinIsNotEmpty merges coordinates closer than this: thinner overlaps do not count.
constexpr double kShapeEpsilon = 1.0e-7;

// Direction.getAxis() as an index into BlockInfo::alt for kPlaceAxis (x, y, z).
constexpr u32 kAxisOf[6] = {1, 1, 2, 2, 0, 0};

}  // namespace

// ---------------------------------------------------------------------------- helpers

// Player.isWithinBlockInteractionRange: distance from the eyes to the block's unit cube.
bool Game::within_reach(u32 pi, BlockPos pos, double buffer) const {
    const Player& p = players_[pi];
    const double ex = ents_.x[pi], ey = ents_.y[pi] + static_cast<double>(crouching(p) ? kEyeCrouching : kEyeStanding),
                 ez = ents_.z[pi];
    const double dx = std::max(std::max(pos.x - ex, ex - (pos.x + 1.0)), 0.0);
    const double dy = std::max(std::max(pos.y - ey, ey - (pos.y + 1.0)), 0.0);
    const double dz = std::max(std::max(pos.z - ez, ez - (pos.z + 1.0)), 0.0);
    return dx * dx + dy * dy + dz * dz < (kReach + buffer) * (kReach + buffer);
}

// Direction.fromYRot: the horizontal direction a yaw looks in (Entity.getDirection).
u32 Game::horizontal_direction(float yaw) {
    static constexpr u32 k2d[4] = {world::kSouth, world::kWest, world::kNorth, world::kEast};  // Direction.from2DDataValue
    return k2d[floor_int(static_cast<double>(yaw) / 90.0 + 0.5) & 3];
}

// Level.getBlockState as a packet needs it: void air outside the build height.
BlockState Game::state_for_client(BlockPos pos) const {
    if (pos.y < world_.min_y() || pos.y > world_.max_y()) return static_cast<BlockState>(gen::block::kVoidAir);
    return world_.block(pos.x, pos.y, pos.z);
}

// CollisionGetter.isUnobstructed, inverted: would this state's collision shape at `pos` overlap an
// entity that blocks building (Entity.blocksBuilding)? Players do, and primed TNT.
bool Game::obstructed(BlockState s, BlockPos pos) const {
    bool dynamic = false;
    const std::span<const world::Box> boxes = world::collision_boxes(s, dynamic);
    if (boxes.empty()) return false;
    const double origin[3] = {static_cast<double>(pos.x), static_cast<double>(pos.y), static_cast<double>(pos.z)};
    const auto overlaps = [&](const double lo[3], const double hi[3]) {
        for (const world::Box& b : boxes) {
            bool overlap = true;
            for (int a = 0; a < 3; ++a)
                overlap &= std::min(hi[a], origin[a] + b.max[a]) - std::max(lo[a], origin[a] + b.min[a]) > kShapeEpsilon;
            if (overlap) return true;
        }
        return false;
    };
    const double w = static_cast<double>(kHalfWidth);
    for (u32 i = 0; i < players_.size(); ++i) {
        const double h = static_cast<double>(crouching(players_[i]) ? kHeightCrouching : kHeightStanding);
        const double lo[3] = {ents_.x[i] - w, ents_.y[i], ents_.z[i] - w};
        const double hi[3] = {ents_.x[i] + w, ents_.y[i] + h, ents_.z[i] + w};
        if (overlaps(lo, hi)) return true;
    }
    const entity_type::Info& tnt = entity_type::kInfo[entity_type::kTnt];
    const double tw = static_cast<double>(tnt.width / 2.0f), th = static_cast<double>(tnt.height);
    for (const u32 slot : tnt_) {
        if (slot == kNone) continue;
        const u32 row = ents_.by_slot(slot);
        const double lo[3] = {ents_.x[row] - tw, ents_.y[row], ents_.z[row] - tw};
        const double hi[3] = {ents_.x[row] + tw, ents_.y[row] + th, ents_.z[row] + tw};
        if (overlaps(lo, hi)) return true;
    }
    // ...and a block that is falling (FallingBlockEntity: the same size as a primed TNT).
    static_assert(entity_type::kInfo[entity_type::kFallingBlock].width == entity_type::kInfo[entity_type::kTnt].width &&
                  entity_type::kInfo[entity_type::kFallingBlock].height == entity_type::kInfo[entity_type::kTnt].height);
    for (const u32 slot : falling_slots_) {
        if (!falling_[slot].blocks_building) continue;
        const u32 row = ents_.by_slot(slot);
        const double lo[3] = {ents_.x[row] - tw, ents_.y[row], ents_.z[row] - tw};
        const double hi[3] = {ents_.x[row] + tw, ents_.y[row] + th, ents_.z[row] + tw};
        if (overlaps(lo, hi)) return true;
    }
    return false;
}

// ChunkMap.getPlayers(pos, borderOnly): viewers_ gets the players that track the chunk - it is in
// their view and has been sent - and, when asked for, border_ those of them for whom one of the
// eight chunks around it is not tracked (isChunkOnTrackedBorder).
void Game::find_viewers(i32 cx, i32 cz, bool want_border) {
    viewers_.clear();
    border_.clear();
    find_in_view(pcx_.data(), pcz_.data(), pview_.data(), static_cast<u32>(players_.size()), cx, cz, kViewMargin, viewers_);
    usize kept = 0;
    for (const u32 pi : viewers_) {
        if (UNLIKELY(senders_[pi].pending != 0) && senders_[pi].test(cx, cz)) continue;
        viewers_[kept++] = pi;
        if (!want_border) continue;
        bool border = false;
        for (i32 dz = -1; dz <= 1 && !border; ++dz)
            for (i32 dx = -1; dx <= 1 && !border; ++dx) border = (dx || dz) && !chunk_tracked(pi, cx + dx, cz + dz);
        if (border) border_.push_back(pi);
    }
    viewers_.resize(kept);
}

// ---------------------------------------------------------------------------- breaking

// handlePlayerAction + ServerPlayerGameMode.handleBlockBreakAction, for a creative player.
void Game::on_player_action(u32 pi, const sb::PlayerAction& a) {
    using A = sb::PlayerAction;
    if (!client_loaded(players_[pi])) return;
    switch (a.action) {
        case A::kDropItem: return on_drop_key(pi, false);
        case A::kDropAllItems: return on_drop_key(pi, true);
        case A::kSwapItemWithOffhand: return on_swap_hands(pi);
        // Using an item, and the attack of a spear (PiercingWeapon.attack: a hit along the look,
        // a sound, and the arm swing others would see): nothing they would act on exists yet.
        case A::kReleaseUseItem:
        case A::kStab: return;
        default: break;  // the four that are about breaking a block
    }
    if (a.sequence < 0) return;
    if (within_reach(pi, a.pos)) {
        if (a.pos.y > world_.max_y()) {
            send(pi, cb::BlockUpdate{a.pos.x, a.pos.y, a.pos.z, state_for_client(a.pos)});
        } else if (a.action == A::kStartDestroyBlock) {
            // "creative destroy": the block goes at once. When it does not, the client is told what
            // is really there, because it already removed the block on its side.
            if (!destroy_block(pi, a.pos)) send(pi, cb::BlockUpdate{a.pos.x, a.pos.y, a.pos.z, state_for_client(a.pos)});
        }
        // Change direction / abort / stop only mean something while a block is being mined over
        // time, which a creative player never does.
    }
    ack(pi, a.sequence);
}

// ServerPlayerGameMode.destroyBlock
bool Game::destroy_block(u32 pi, BlockPos pos) {
    const Player& p = players_[pi];
    const BlockState state = world_.block(pos.x, pos.y, pos.z);
    // ItemStack.canDestroyBlock: swords, the trident and the mace do not break blocks in creative.
    const ItemStack& hand = p.menu.inv.selected_item();
    if (!hand.empty() && !(hand.info().flags & item_flag::kDestroysInCreative)) return false;
    // Command blocks and the like need operator rights; nobody is an operator yet.
    if (world::block_info_of(state).flags & block_flag::kGameMaster) return false;
    // Block.playerWillDestroy -> spawnDestroyByEntityParticles: the others see and hear it break
    // (its own client did that by itself). Of air too, as vanilla sends it.
    // (TntBlock.playerWillDestroy primes an `unstable` TNT - but not for a creative player.)
    level_event(kLevelEventDestroyBlock, pos, static_cast<i32>(state), pi);
    // Level.removeBlock: what stays is the block of the fluid the state held (air for a dry one).
    remove_block(pos);
    return true;
}

// ---------------------------------------------------------------------------- placing

// handleUseItemOn + ServerPlayerGameMode.useItemOn
void Game::on_use_item_on(u32 pi, const sb::UseItemOn& u) {
    Player& p = players_[pi];
    if (!client_loaded(p) || u.sequence < 0) return;
    ack(pi, u.sequence);
    const BlockPos pos = u.pos;
    if (!within_reach(pi, pos)) return;
    // The hit location has to be on (or within rounding of) the block it names. Written as vanilla
    // writes it, so that a NaN passes here exactly as it does there.
    const double hx = (static_cast<double>(pos.x) + static_cast<double>(u.cursor_x)) - (static_cast<double>(pos.x) + 0.5);
    const double hy = (static_cast<double>(pos.y) + static_cast<double>(u.cursor_y)) - (static_cast<double>(pos.y) + 0.5);
    const double hz = (static_cast<double>(pos.z) + static_cast<double>(u.cursor_z)) - (static_cast<double>(pos.z) + 0.5);
    if (std::fabs(hx) >= kHitLimit || std::fabs(hy) >= kHitLimit || std::fabs(hz) >= kHitLimit) return;
    if (pos.y > world_.max_y() || pos.y < world_.min_y()) return;  // vanilla also shows "height limit" on the action bar

    if (!p.awaiting_teleport) {
        const Inventory& inv = p.menu.inv;
        // A copy: a block that opens a menu puts what the old menu held back into the inventory.
        const ItemStack held = u.hand == 0 ? inv.selected_item() : inv.items[inv::kOffhand];
        const bool hands_full = !inv.selected_item().empty() || !inv.items[inv::kOffhand].empty();
        // The clicked block gets the click first unless the player sneaks with something in hand
        // (ServerPlayerGameMode.useItemOn: useItemOn, then - main hand - useWithoutItem).
        const BlockState clicked = world_.block(pos.x, pos.y, pos.z);
        const world::BlockInfo& block = world::block_info_of(clicked);
        const bool takes_click = !(p.shift && hands_full) && (block.flags & block_flag::kInteractive);
        // A block with a use hook that nobody has ported cannot be placed; where a world that was
        // imported holds one, doing nothing is the only answer that does not invent behaviour.
        // The same for a block entity that is kept as the tag it came with: a chest whose
        // contents this server does not read is not opened as if it were empty.
        const world::BlockEntityRef* be =
            block.block_entity != gen::kNoBlockEntity ? world_.block_entity(pos.x, pos.y, pos.z) : nullptr;
        const bool unknown = takes_click && (block.kind == gen::block_kind::kPlain || (be && (be->data & kOpaqueBlockEntity)));
        // InteractionResult.SUCCESS - of the block, or of BlockItem.place: the arm swings. The
        // client predicted it for itself; the others see the hand's interact animation, which no
        // item changes (gen.py checks). Vanilla runs the swing twice here - the same branch
        // stands twice in handleUseItemOn - and so sends two packets; the second begins the same
        // swing again in the same tick, which no client can tell from one.
        last_use_face_ = u.direction;
        Used used = takes_click && !unknown ? use_block(pi, pos, clicked, held, u.hand) : Used::kPass;
        // ...then the item's useOn: of a block item BlockItem.place, of an egg SpawnEggItem.useOn.
        if (used == Used::kPass && !unknown && !held.empty()) {
            if (held.info().kind == gen::item_kind::kSpawnEgg)
                used = use_spawn_egg(pi, u, held);
            else if (place_block(pi, u, held))
                used = Used::kSuccess;
        }
        // (SUCCESS_SERVER: the client did not know it would - a sign whose editor opens - and is
        // shown its own arm with everybody else.)
        if (used != Used::kPass) swing(pi, u.hand, {}, used == Used::kSuccessServer);
    }
    // Whatever happened, the client predicted something: tell it what the two blocks it may have
    // touched really are.
    const BlockPos next{pos.x + world::kStepX[u.direction], pos.y + world::kStepY[u.direction],
                        pos.z + world::kStepZ[u.direction]};
    send(pi, cb::BlockUpdate{pos.x, pos.y, pos.z, state_for_client(pos)});
    send(pi, cb::BlockUpdate{next.x, next.y, next.z, state_for_client(next)});
}

// handleUseItem + ServerPlayerGameMode.useItem: the right click on no block. The client predicted
// with a sequence like any other, and it turned the player the way it looks now.
void Game::on_use_item(u32 pi, const sb::UseItem& u) {
    Player& p = players_[pi];
    if (!client_loaded(p) || u.sequence < 0) return;
    ack(pi, u.sequence);
    const Inventory& inv = p.menu.inv;
    if ((u.hand == 0 ? inv.selected_item() : inv.items[inv::kOffhand]).empty()) return;
    // absSnapRotationTo(wrapDegrees(yRot), wrapDegrees(xRot)); Entity.setYRot / setXRot pass over
    // what is not finite.
    if (std::isfinite(u.yaw)) ents_.yaw[pi] = wrap_degrees(u.yaw);
    if (std::isfinite(u.pitch)) ents_.pitch[pi] = std::clamp(wrap_degrees(u.pitch), -90.0f, 90.0f);
    // Item.use of what it holds: a bucket's. (The stack is copied: what it does may give the
    // inventory another.) Nothing else implemented answers anything but PASS: a spawn
    // egg's SpawnEggItem.use - at water further away than a click on a block reaches - is not ported yet.
    const ItemStack held = u.hand == 0 ? inv.selected_item() : inv.items[inv::kOffhand];
    const Used used = held.info().kind == gen::item_kind::kBucket ? use_bucket(pi, held) : Used::kPass;
    if (used == Used::kPass) return;
    swing(pi, u.hand, {}, used == Used::kSuccessServer);
    // Player.inventoryMenu.broadcastChanges comes with the player's tick; what the hand holds is
    // what it held (a creative player keeps its bucket: ItemUtils.createFilledResult).
    p.menu.send_changes(out(pi), p.threshold);
    dirty_[pi] |= kEquipDirty;
}

// BlockItem.place
bool Game::place_block(u32 pi, const sb::UseItemOn& u, const ItemStack& held) {
    const world::ItemInfo& item = held.info();
    if (item.block == gen::kNoBlock) return false;  // not a block item; other items' useOn() is not implemented
    const world::BlockInfo& block = world::block_info(item.block);
    const u32 kind = block.flags & block_flag::kPlaceMask;
    // (A stack with components may ask for something of the block entity that is not done here:
    // can_apply_item_to_block.)
    if (kind == block_flag::kPlaceNone || !(item.flags & (item_flag::kPlainBlockItem | item_flag::kStandingAndWall)) ||
        !can_apply_item_to_block(item.block, held)) {
        LOG_DEBUG("%s: placing %.*s is not implemented yet", players_[pi].name.c_str(),
                  static_cast<int>(world::block_name(item.block).size()), world::block_name(item.block).data());
        return false;
    }
    // BlockBehaviour.canBeReplaced(state, context): replaceable, and not by its own item - or a
    // slab that has room for its other half (SlabBlock's).
    const auto replaceable = [&](BlockState s, bool clicked) {
        const world::BlockInfo& b = world::block_info_of(s);
        if (b.kind == gen::block_kind::kSlab) return slab_can_be_replaced(s, held, u, clicked);
        if (b.kind == gen::block_kind::kSegmented) {
            // SegmentableBlock.canBeReplaced: by one more of itself, while it holds fewer than four
            // and the player does not sneak - or as any block (leaf litter gives way, petals do not).
            const bool full = (s - b.first_state) % gen::props::flower_bed::kFacingStride == 3;
            if (!players_[pi].shift && b.item == held.item() && !full) return true;
            return world::is_replaceable(s) && b.item != held.item();
        }
        return world::is_replaceable(s) && !(b.flags & block_flag::kCustomReplace) && b.item != held.item();
    };
    // BlockPlaceContext: into the clicked block if that can be replaced, otherwise against its
    // face (canPlace: into what is there, if that can be replaced).
    BlockPos at = u.pos;
    const bool replacing = replaceable(world_.block(at.x, at.y, at.z), true);
    if (!replacing) {
        at = {at.x + world::kStepX[u.direction], at.y + world::kStepY[u.direction], at.z + world::kStepZ[u.direction]};
        if (!replaceable(state_for_client(at), false)) return false;
    }
    const BlockState state = state_for_placement(pi, u, block, kind, at, replacing);
    if (state == world::Section::kNoState) return false;  // getPlacementState: null
    // BlockItem.canPlace: the block can be there (canSurvive), and nobody stands there.
    if (!can_survive(state, at) || obstructed(state, at)) return false;
    if (!set_block(at, state)) return false;  // above or below the build height
    // What the stack carries for the block and its block entity: its components, a tag.
    const world::BlockInfo& placed = world::block_info_of(world_.block(at.x, at.y, at.z));
    if (held.has_patch() && world::block_of(world_.block(at.x, at.y, at.z)) == world::block_of(state))
        apply_item_to_block(at, held);
    if (sign_kind(placed.kind) && world::block_of(world_.block(at.x, at.y, at.z)) == world::block_of(state))
        sign_placed(pi, at);  // setPlacedBy
    return true;
}

// Direction.orderedByNearest(player): the six directions, the one it looks along most first.
void Game::nearest_looking_directions(u32 pi, u32 out[6]) const {
    const float pitch = ents_.pitch[pi] * 0.017453292f, yaw = -ents_.yaw[pi] * 0.017453292f;
    const float pitch_sin = phys::mth_sin(static_cast<double>(pitch)), pitch_cos = phys::mth_cos(static_cast<double>(pitch));
    const float yaw_sin = phys::mth_sin(static_cast<double>(yaw)), yaw_cos = phys::mth_cos(static_cast<double>(yaw));
    const bool x_pos = yaw_sin > 0.0f, y_pos = pitch_sin < 0.0f, z_pos = yaw_cos > 0.0f;
    const float x_yaw = x_pos ? yaw_sin : -yaw_sin, y_mag = y_pos ? -pitch_sin : pitch_sin, z_yaw = z_pos ? yaw_cos : -yaw_cos;
    const float x_mag = x_yaw * pitch_cos, z_mag = z_yaw * pitch_cos;
    const u32 ax = x_pos ? world::kEast : world::kWest, ay = y_pos ? world::kUp : world::kDown,
              az = z_pos ? world::kSouth : world::kNorth;
    const auto make = [&](u32 a, u32 b, u32 c) {  // makeDirectionArray
        out[0] = a, out[1] = b, out[2] = c, out[3] = world::opposite(c), out[4] = world::opposite(b), out[5] = world::opposite(a);
    };
    if (x_yaw > z_yaw) {
        if (y_mag > x_mag)
            make(ay, ax, az);
        else if (z_mag > y_mag)
            make(ax, az, ay);
        else
            make(ax, ay, az);
    } else if (y_mag > z_mag) {
        make(ay, az, ax);
    } else if (x_mag > y_mag) {
        make(az, ax, ay);
    } else {
        make(az, ay, ax);
    }
}

// BlockPlaceContext.getNearestLookingDirections: the directions the player looks along, nearest
// first - but against a face, into that face comes first.
void Game::placement_directions(u32 pi, u32 face, bool replacing_clicked, u32 out[6]) const {
    nearest_looking_directions(pi, out);
    if (replacing_clicked) return;
    const u32 into = world::opposite(face);
    u32 index = 0;
    while (index < 6 && out[index] != into) ++index;
    if (index > 0 && index < 6) {
        for (u32 i = index; i > 0; --i) out[i] = out[i - 1];
        out[0] = into;
    }
}

// Block.getStateForPlacement (and, for a torch, StandingAndWallBlockItem.getPlacementState): the
// state a block of this placement kind is placed as at `at`; Section::kNoState: it cannot be.
BlockState Game::state_for_placement(u32 pi, const sb::UseItemOn& u, const world::BlockInfo& block, u32 kind, BlockPos at,
                                     bool replacing_clicked) const {
    switch (kind) {
        case block_flag::kPlaceAxis: return block.alt[kAxisOf[u.direction]];  // RotatedPillarBlock
        case block_flag::kPlaceSnowy: {                                       // SnowyBlock
            const BlockState above = state_for_client({at.x, at.y + 1, at.z});
            return block.alt[(world::block_info_of(above).flags & block_flag::kSnowTag) ? 1 : 0];
        }
        case block_flag::kPlaceFacing: {  // AbstractFurnaceBlock: facing the one who places it, unlit
            const u32 facing = world::opposite(horizontal_direction(ents_.yaw[pi]));
            return static_cast<BlockState>(block.first_state + (facing - world::kNorth) * gen::props::furnace::kFacingStride + 1);
        }
        case block_flag::kPlaceChest: return chest_for_placement(pi, block, at, u.direction);
        case block_flag::kPlaceBarrel: {                         // BarrelBlock: its lid towards the one who places it, shut
            static constexpr u8 kIndex[6] = {5, 4, 0, 2, 3, 1};  // facing: north, east, south, west, up, down
            u32 dirs[6];
            nearest_looking_directions(pi, dirs);  // BlockPlaceContext.getNearestLookingDirection: whatever face was clicked
            return static_cast<BlockState>(gen::props::barrel::kFirst +
                                           kIndex[world::opposite(dirs[0])] * gen::props::barrel::kFacingStride + 1);
        }
        case block_flag::kPlaceTorch: {
            u32 dirs[6];
            placement_directions(pi, u.direction, replacing_clicked, dirs);
            // WallTorchBlock.getStateForPlacement: on the first wall it looks at that holds it.
            BlockState wall = world::Section::kNoState;
            for (const u32 dir : dirs) {
                if (dir < world::kNorth) continue;
                const BlockState s = static_cast<BlockState>(block.alt[1] + (world::opposite(dir) - world::kNorth));
                if (can_survive(s, at)) {
                    wall = s;
                    break;
                }
            }
            // StandingAndWallBlockItem.getPlacementState: looking down first it stands, else it hangs
            // on that wall; never from above (attachmentDirection DOWN).
            for (const u32 dir : dirs) {
                if (dir == world::kUp) continue;
                const BlockState possible = dir == world::kDown ? block.alt[0] : wall;
                if (possible != world::Section::kNoState && can_survive(possible, at)) return possible;
            }
            return world::Section::kNoState;
        }
        case block_flag::kPlaceSign:
        case block_flag::kPlaceHangingSign: return sign_for_placement(pi, u, block, kind, at, replacing_clicked);
        case block_flag::kPlaceCluster: {  // AmethystClusterBlock: on the clicked face, wet in a source of water
            static constexpr u8 kIndex[6] = {5, 4, 0, 2, 3, 1};  // facing: north, east, south, west, up, down
            const bool wet = world::holds_water_source(state_for_client(at));
            return static_cast<BlockState>(block.first_state + kIndex[u.direction] * gen::props::amethyst_cluster::kFacingStride +
                                           (wet ? 0 : 1));
        }
        case block_flag::kPlaceLamp:  // RedstoneLampBlock: lit if a signal arrives
            return static_cast<BlockState>(gen::props::redstone_lamp::kFirst + (has_neighbour_signal(at) ? 0 : 1));
        case block_flag::kPlaceConcretePowder:  // ConcretePowderBlock.shouldSolidify: concrete in water, or beside it
            return world::holds_water(state_for_client(at)) || touches_water(at) ? block.alt[0] : block.default_state;
        case block_flag::kPlaceDefault: return block.default_state;                       // Block.getStateForPlacement
        default: return shaped_for_placement(pi, u, block, kind, at, replacing_clicked);  // server/block_shapes.cpp
    }
}

// ---------------------------------------------------------------------------- picking

// handlePickItemFromBlock + tryPickItem: the middle click. Since 26.x the client only names the
// block; the server finds its item, brings it into the hand - selecting the hotbar slot that has
// it, fetching it from further back, or (creative) making one - and tells the client.
void Game::on_pick_block(u32 pi, const sb::PickItemFromBlock& pk) {
    Player& p = players_[pi];
    const BlockPos pos = pk.pos;
    if (!within_reach(pi, pos) || pos.y < world_.min_y() || pos.y > world_.max_y()) return;  // Level.isLoaded
    const u32 block_id = world::block_of(world_.block(pos.x, pos.y, pos.z));
    const world::BlockInfo& block = world::block_info(block_id);
    // BlockBehaviour.getCloneItemStack is `new ItemStack(asItem())`; the blocks that answer
    // otherwise (crops, potted plants, banners...) cannot be in the world yet.
    if (block.flags & block_flag::kCustomClone) {
        LOG_DEBUG("%s: picking %.*s is not implemented yet", p.name.c_str(), static_cast<int>(world::block_name(block_id).size()),
                  world::block_name(block_id).data());
        return;
    }
    if (block.item == 0) return;  // air, and what has no item: an empty stack, nothing to pick
    // With ctrl a creative player also gets the block entity's data; there are no block entities.
    pick_item(pi, ItemStack(block.item, 1));
}

// handlePickItemFromEntity + Entity.getPickResult: the middle click on an entity. A mob gives its
// spawn egg (Mob.getPickResult: SpawnEggItem.byId; a mob that has none, nothing), an armor stand
// its item. The other classes that answer - boats, minecarts, item frames, paintings, an end
// crystal, a leash knot - are nothing this server simulates, and give nothing. (With ctrl an
// operator would be shown the profile of a player it picked, FetchProfileCommand.printForAvatar:
// not here.)
void Game::on_pick_entity(u32 pi, const sb::PickItemFromEntity& pk) {
    const u32 row = ents_.by_net_id(pk.id);  // Level.getEntityOrPart: an ender dragon has no parts here
    if (row == kNone) return;
    const u32 type = ents_.type[row];
    const u32 item = entity_type::kInfo[type].mob       ? world::spawn_egg_of(type)
                     : type == entity_type::kArmorStand ? gen::item::kArmorStand
                                                        : 0;
    if (item == 0) return;
    // Player.isWithinEntityInteractionRange(entity, 3.0): from the eyes to its box, within the
    // entity interaction range (3, and 2 more in creative: ServerPlayer's
    // CREATIVE_ENTITY_INTERACTION_RANGE_MODIFIER) and 3 more.
    const entity_type::Info& info = entity_type::kInfo[type];
    const world::Aabb box = world::entity_box(ents_.x[row], ents_.y[row], ents_.z[row], info.width, info.height);
    const double eye[3] = {ents_.x[pi], ents_.y[pi] + static_cast<double>(crouching(players_[pi]) ? kEyeCrouching : kEyeStanding),
                           ents_.z[pi]};
    double d2 = 0;
    for (u32 a = 0; a < 3; ++a) {
        const double d = std::max(std::max(box.min[a] - eye[a], eye[a] - box.max[a]), 0.0);
        d2 += d * d;
    }
    constexpr double kRange = 3.0 + 2.0 + 3.0;
    if (!(d2 < kRange * kRange)) return;
    pick_item(pi, ItemStack(item, 1));
}

// tryPickItem: the stack into the hand - selecting the hotbar slot that has it, fetching it from
// further back, or (creative) making one - and the client told.
void Game::pick_item(u32 pi, const ItemStack& stack) {
    Player& p = players_[pi];
    p.menu.pick(stack, true);  // everyone is creative
    send(pi, cb::SetHeldSlot{p.menu.inv.selected});
    p.menu.send_changes(out(pi), p.threshold);
    dirty_[pi] |= kEquipDirty;
}

// ---------------------------------------------------------------------------- broadcast

// ChunkHolder.broadcastChanges for every changed chunk. Each packet is serialized once and copied
// to its readers. (The block-change acknowledgements follow in tick_players.)
void Game::broadcast_changes() {
    world_.update_light();
    const i32 threshold = s_.compression_threshold;

    // Light first. Only players who have the chunk on the edge of their view are sent light: the
    // others have all its neighbours and their client recomputes the same values from the block
    // change itself.
    for (world::Chunk* c : world_.light_changes()) {
        if (!(c->sky_changed | c->block_changed)) continue;
        find_viewers(c->x, c->z, true);
        if (border_.empty()) continue;
        scratch_.clear();
        world::write_light_update(scratch_, threshold, *c, c->sky_changed, c->block_changed);
        for (u32 pi : border_) out(pi).append(scratch_.data, scratch_.len);
    }

    const auto& changes = world_.block_changes();
    const i32 min_section = world_.min_y() >> 4;
    for (usize i = 0; i < changes.size();) {
        world::Chunk* c = changes[i].chunk;
        usize end = i;
        while (end < changes.size() && changes[end].chunk == c) ++end;
        find_viewers(c->x, c->z, false);
        if (!viewers_.empty()) {
            scratch_.clear();
            for (usize j = i; j < end;) {
                usize k = j;
                while (k < end && changes[k].section == changes[j].section) ++k;
                const world::Section& sec = c->sections[changes[j].section];
                const i32 sy = min_section + changes[j].section;
                const auto state_at = [&](u16 pos) {  // pos = x<<8 | z<<4 | y
                    return sec.get(world::Section::index(pos >> 8, pos & 15u, (pos >> 4) & 15u));
                };
                // ChunkHolder.broadcastBlockEntityIfNeeded: behind a block's update, what its block
                // entity shows - of those that show something (a sign's text).
                const auto block_entity_data = [&](u16 pos) {
                    if (LIKELY(!c->block_entity_tags)) return;
                    const i32 x = c->x * 16 + (pos >> 8), y = sy * 16 + (pos & 15), z = c->z * 16 + ((pos >> 4) & 15);
                    const std::span<const u8> tag = world_.block_entity_tag(x, y, z);
                    if (tag.empty()) return;
                    write_packet(scratch_, threshold, cb::BlockEntityData{x, y, z, world_.block_entity(x, y, z)->type, tag});
                };
                if (k - j == 1) {
                    const u16 pos = changes[j].pos;
                    write_packet(scratch_, threshold,
                                 cb::BlockUpdate{c->x * 16 + (pos >> 8), sy * 16 + (pos & 15), c->z * 16 + ((pos >> 4) & 15),
                                                 state_at(pos)});
                    block_entity_data(pos);
                } else {
                    update_pos_.clear();
                    update_state_.clear();
                    for (usize m = j; m < k; ++m) {
                        update_pos_.push_back(changes[m].pos);
                        update_state_.push_back(state_at(changes[m].pos));
                    }
                    write_packet(scratch_, threshold, cb::SectionBlocksUpdate{c->x, sy, c->z, update_pos_, update_state_});
                    for (usize m = j; m < k; ++m) block_entity_data(changes[m].pos);
                }
                j = k;
            }
            for (u32 pi : viewers_) out(pi).append(scratch_.data, scratch_.len);
        }
        i = end;
    }
    world_.end_tick();
}

}  // namespace server
