// What a (creative) player can do to blocks, and how the result reaches everyone who sees it.
//
// The rules are vanilla's, read from the 26.3 jar; each function names the method it follows:
//   ServerGamePacketListenerImpl.handlePlayerAction / handleUseItemOn / handlePickItemFromBlock
//   ServerPlayerGameMode.handleBlockBreakAction / destroyBlock / useItemOn
//   BlockItem.place, BlockPlaceContext, CollisionGetter.isUnobstructed
//   ChunkHolder.broadcastChanges, ServerGamePacketListenerImpl.tick (the acknowledgement)
//
// What is NOT here yet, and deliberately has no stand-in: block behaviour beyond storing a state.
// A block is placeable only if every hook vanilla would run for it is implemented (the generated
// BlockInfo::flags placement kind, see tools/mcgen/gen.py placement_kind), so the world never holds
// a block whose reaction to being clicked, updated or broken we would have to invent.
#include <cmath>

#include "base/log.hpp"
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
    // Level.removeBlock: what stays is the block of the fluid the state held (air for a dry one).
    // (TntBlock.playerWillDestroy primes an `unstable` TNT - but not for a creative player.)
    set_block(pos, static_cast<BlockState>(world::state::fluid_legacy_of(state)));
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
        if ((takes_click && !unknown && use_block(pi, pos, clicked, held, u.hand)) ||
            (!unknown && !held.empty() && place_block(pi, u, held)))
            swing(pi, u.hand, {}, false);
    }
    // Whatever happened, the client predicted something: tell it what the two blocks it may have
    // touched really are.
    const BlockPos next{pos.x + world::kStepX[u.direction], pos.y + world::kStepY[u.direction],
                        pos.z + world::kStepZ[u.direction]};
    send(pi, cb::BlockUpdate{pos.x, pos.y, pos.z, state_for_client(pos)});
    send(pi, cb::BlockUpdate{next.x, next.y, next.z, state_for_client(next)});
}

// BlockItem.place
bool Game::place_block(u32 pi, const sb::UseItemOn& u, const ItemStack& held) {
    const world::ItemInfo& item = held.info();
    if (item.block == gen::kNoBlock) return false;  // not a block item; other items' useOn() is not implemented
    const world::BlockInfo& block = world::block_info(item.block);
    const u32 kind = block.flags & block_flag::kPlaceMask;
    // A stack with components may ask for more than the block's default state (block_state,
    // block_entity_data): BlockItem.updateBlockStateFromTag and friends are not implemented.
    if (kind == block_flag::kPlaceNone || !(item.flags & item_flag::kPlainBlockItem) || held.has_patch()) {
        LOG_DEBUG("%s: placing %.*s is not implemented yet", players_[pi].name.c_str(),
                  static_cast<int>(world::block_name(item.block).size()), world::block_name(item.block).data());
        return false;
    }
    // BlockBehaviour.canBeReplaced(state, context): replaceable, and not by its own item.
    const auto replaceable = [&](BlockState s) {
        const world::BlockInfo& b = world::block_info_of(s);
        return world::is_replaceable(s) && !(b.flags & block_flag::kCustomReplace) && b.item != held.item();
    };
    // BlockPlaceContext: into the clicked block if that can be replaced, otherwise against its face.
    BlockPos at = u.pos;
    if (!replaceable(world_.block(at.x, at.y, at.z))) {
        at = {at.x + world::kStepX[u.direction], at.y + world::kStepY[u.direction], at.z + world::kStepZ[u.direction]};
        if (!replaceable(state_for_client(at))) return false;
    }
    BlockState state = block.default_state;  // Block.getStateForPlacement
    if (kind == block_flag::kPlaceAxis) {    // RotatedPillarBlock
        state = block.alt[kAxisOf[u.direction]];
    } else if (kind == block_flag::kPlaceSnowy) {  // SnowyBlock
        const BlockState above = state_for_client({at.x, at.y + 1, at.z});
        state = block.alt[(world::block_info_of(above).flags & block_flag::kSnowTag) ? 1 : 0];
    } else if (kind == block_flag::kPlaceFacing) {  // AbstractFurnaceBlock: facing the one who places it, unlit
        const u32 facing = world::opposite(horizontal_direction(ents_.yaw[pi]));
        state = static_cast<BlockState>(gen::props::furnace::kFirst +
                                        (facing - world::kNorth) * gen::props::furnace::kFacingStride + 1);
    } else if (kind == block_flag::kPlaceChest) {
        state = chest_for_placement(pi, at, u.direction);
    }
    // BlockItem.canPlace: canSurvive() is the default for every placeable block; what is left is
    // "nobody stands there".
    if (obstructed(state, at)) return false;
    return set_block(at, state);  // false above/below the build height
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
    p.menu.pick(ItemStack(block.item, 1), true);  // everyone is creative
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
                if (k - j == 1) {
                    const u16 pos = changes[j].pos;
                    write_packet(scratch_, threshold,
                                 cb::BlockUpdate{c->x * 16 + (pos >> 8), sy * 16 + (pos & 15), c->z * 16 + ((pos >> 4) & 15),
                                                 state_at(pos)});
                } else {
                    update_pos_.clear();
                    update_state_.clear();
                    for (usize m = j; m < k; ++m) {
                        update_pos_.push_back(changes[m].pos);
                        update_state_.push_back(state_at(changes[m].pos));
                    }
                    write_packet(scratch_, threshold, cb::SectionBlocksUpdate{c->x, sy, c->z, update_pos_, update_state_});
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
