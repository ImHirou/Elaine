// The player as an entity: what its packets do to its row of the entity table, and its own tick.
//
// The rules are vanilla's, read from the 26.3 jar; each function names the method it follows:
//   ServerGamePacketListenerImpl.handleMovePlayer / handlePlayerPositionChange / handlePlayerCommand
//   / handleClientInformation, Entity.absSnapTo / absSnapRotationTo, Player.aiStep / updatePlayerPose
//
// What is NOT here yet, all of it physics or attributes:
//   * movement checks ("moved too quickly", "moved wrongly", flying);
//   * the server's own simulation of the player. In 26.x a player's movement is simulated on both
//     sides (Player.getMoveSimulationType() is AUTHORITATIVE_SIDE_AND_SERVER): vanilla knows a
//     player is on the ground before the client says so, and infers jumps (Player.jumpFromGround
//     gives a velocity that viewers get as set_entity_motion). Here "on the ground" is the flag of
//     the client's last movement packet, and a player's velocity stays zero;
//   * attributes: sprinting adds a speed modifier that travels as update_attributes.
#include <algorithm>
#include <cmath>

#include "server/game.hpp"

namespace server {

using namespace proto;
namespace flag = entity::flag;
namespace shared_flag = entity::shared_flag;

namespace {
constexpr double kMaxHorizontal = 3.0E7, kMaxVertical = 2.0E7;  // clampHorizontal / clampVertical
// Avatar.POSES: every pose is 0.6 wide; what a player on foot can be in differs by height.
constexpr float kWidth = 0.6f, kStandingHeight = 1.8f, kCrouchingHeight = 1.5f, kSwimmingHeight = 0.6f;
constexpr double kDeflate = 1.0e-7;       // canPlayerFitWithinBlocksAndEntitiesWhen shrinks the box by this
constexpr double kShapeEpsilon = 1.0e-7;  // Shapes.joinIsNotEmpty: thinner overlaps do not count
}  // namespace

// handleMovePlayer, for all four move_player_* packets: `m` holds what the packet carried.
void Game::on_move(u32 pi, const sb::MovePlayer& m, bool has_pos, bool has_rot) {
    Player& p = players_[pi];
    if (UNLIKELY((has_pos && (std::isnan(m.x) || std::isnan(m.y) || std::isnan(m.z))) ||
                 (has_rot && !(std::isfinite(m.yaw) && std::isfinite(m.pitch))))) {
        kick(pi, "Invalid move player packet received");
        return;
    }
    if (has_rot) {  // absSnapRotationTo(wrapDegrees(yRot), wrapDegrees(xRot)); Entity.setXRot clamps
        ents_.yaw[pi] = wrap_degrees(m.yaw);
        ents_.pitch[pi] = std::clamp(wrap_degrees(m.pitch), -90.0f, 90.0f);
    }
    // updateAwaitingTeleport: until the client confirms a teleport, its positions are stale and
    // only the rotation is taken.
    if (p.awaiting_teleport) return;

    u16 f = static_cast<u16>(ents_.flags[pi] & ~flag::kOnGround);
    if (m.flags & sb::MovePlayer::kOnGround) f |= flag::kOnGround;  // Entity.setOnGroundWithMovement
    // ChunkMap.move runs for every movement packet, moved or not: who sees whom is re-evaluated.
    ents_.flags[pi] = f | flag::kMoved;
    if (!has_pos) return;

    const double x = std::clamp(m.x, -kMaxHorizontal, kMaxHorizontal);
    const double z = std::clamp(m.z, -kMaxHorizontal, kMaxHorizontal);
    ents_.x[pi] = x;
    ents_.y[pi] = std::clamp(m.y, -kMaxVertical, kMaxVertical);
    ents_.z[pi] = z;
    const i32 cx = chunk_of(x), cz = chunk_of(z);
    if (UNLIKELY(cx != pcx_[pi] || cz != pcz_[pi])) {
        const i32 ocx = pcx_[pi], ocz = pcz_[pi];
        pcx_[pi] = cx;
        pcz_[pi] = cz;
        send_view(pi, ocx, ocz, false);
    }
}

// handlePlayerCommand. Everything but sprinting acts on things that do not exist yet (beds,
// mounts, elytra).
void Game::on_player_command(u32 pi, const sb::PlayerCommand& c) {
    if (!client_loaded(players_[pi])) return;
    if (c.action == sb::PlayerCommand::kStartSprinting)
        ents_.set_shared_flag(pi, shared_flag::kSprinting, true);
    else if (c.action == sb::PlayerCommand::kStopSprinting)
        ents_.set_shared_flag(pi, shared_flag::kSprinting, false);
}

// handleClientInformation -> ServerPlayer.updateOptions, the part other players can see. The
// view distance is not applied yet: the chunk view would have to grow or shrink with it.
void Game::on_client_information(u32 pi, const sb::ClientInformation& info) {
    ents_.set_skin_parts(pi, info.skin_parts);
    ents_.set_main_hand(pi, info.main_hand == entity::kMainHandRight ? entity::kMainHandRight : entity::kMainHandLeft);
}

// The per-connection half of the tick: ServerGamePacketListenerImpl.tick acknowledges block
// changes, then ticks the player (ServerPlayer.doTick -> Player.tick, aiStep). It runs after the
// entity tracker, as in vanilla - which is why others see a head turn one tick after the body and
// a new pose one tick after the key that caused it.
void Game::tick_players() {
    const u32 n = static_cast<u32>(players_.size());
    for (u32 pi = 0; pi < n; ++pi) {
        if (ack_[pi] >= 0) {
            send(pi, cb::BlockChangedAck{ack_[pi]});
            ack_[pi] = -1;
        }
        // LivingEntity.tick: detectEquipmentUpdates, before aiStep. Only for those whose
        // inventory something touched since the last look.
        if (UNLIKELY(dirty_[pi] & kEquipDirty)) {
            dirty_[pi] &= static_cast<u8>(~kEquipDirty);
            show_equipment(pi);
        }
    }
    std::copy_n(ents_.yaw, n, ents_.head_yaw);  // Player.aiStep: yHeadRot = getYRot()
    // ...then it touches what is around it (items), and Player.tick ends with the pose.
    if (UNLIKELY(!item_slot_.empty()))
        for (u32 pi = 0; pi < n; ++pi) pick_up_items(pi);
    for (u32 pi = 0; pi < n; ++pi) update_pose(pi);
}

// Player.updatePlayerPose, with the poses a creative player on foot can ask for.
void Game::update_pose(u32 pi) {
    const u8 desired = static_cast<u8>(crouching(players_[pi]) ? pose::kCrouching : pose::kStanding);
    // The standing box contains the two smaller ones: where it fits, vanilla's three checks pass.
    if (LIKELY(fits(pi, kStandingHeight))) {
        ents_.set_pose(pi, desired);
        return;
    }
    if (!fits(pi, kSwimmingHeight)) return;  // nowhere to be: the pose stays
    const bool crouch_fits = fits(pi, kCrouchingHeight);
    ents_.set_pose(pi, desired == pose::kCrouching && crouch_fits ? desired : crouch_fits ? pose::kCrouching : pose::kSwimming);
}

// Player.canPlayerFitWithinBlocksAndEntitiesWhen: Level.noCollision for a box of this height at
// the player's feet. Blocks only - no entity is solid yet, and there is no world border.
bool Game::fits(u32 pi, float height) const {
    const double half = static_cast<double>(kWidth / 2.0f);  // EntityDimensions.makeBoundingBox
    const double lo[3] = {ents_.x[pi] - half + kDeflate, ents_.y[pi] + kDeflate, ents_.z[pi] - half + kDeflate};
    const double hi[3] = {ents_.x[pi] + half - kDeflate, ents_.y[pi] + static_cast<double>(height) - kDeflate,
                          ents_.z[pi] + half - kDeflate};
    // One block further down: fences and walls collide 1.5 blocks high.
    for (i32 by = floor_int(lo[1]) - 1, y1 = floor_int(hi[1]); by <= y1; ++by) {
        for (i32 bz = floor_int(lo[2]), z1 = floor_int(hi[2]); bz <= z1; ++bz) {
            for (i32 bx = floor_int(lo[0]), x1 = floor_int(hi[0]); bx <= x1; ++bx) {
                const world::BlockState s = world_.block(bx, by, bz);
                if (LIKELY(s == world::kAir)) continue;
                bool dynamic = false;
                const double origin[3] = {static_cast<double>(bx), static_cast<double>(by), static_cast<double>(bz)};
                for (const world::Box& b : world::collision_boxes(s, dynamic)) {
                    bool overlap = true;
                    for (int a = 0; a < 3; ++a)
                        overlap &= std::min(hi[a], origin[a] + b.max[a]) - std::max(lo[a], origin[a] + b.min[a]) > kShapeEpsilon;
                    if (overlap) return false;
                }
            }
        }
    }
    return true;
}

}  // namespace server
