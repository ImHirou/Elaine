// SPDX-License-Identifier: GPL-3.0-or-later
// TNT: the primed entity, its fuse, and the explosion.
//
// The rules are vanilla's, read from the 26.3 jar; each function names the method it follows:
//   TntBlock (prime, wasExploded, onPlace / neighborChanged: a redstone signal),
//   SignalGetter.hasNeighborSignal
//   PrimedTnt (constructor, tick, explode) - how it falls and hops is phys::tnt_tick
//   ServerExplosion (calculateExplodedPositions, hurtEntities, getSeenPercent,
//   interactWithBlocks), ExplosionDamageCalculator, BlockBehaviour.onExplosionHit,
//   Block.popResource, ItemEntity.hurtServer, ArmorStand.hurtServer, Entity.pushFromExplosion,
//   ServerLevel.explode (the packet)
//
// A client simulates primed TNT too (MoveSimulationType.SERVER_AND_CLIENT), and what the two
// disagree on shows as a jump at the next position update.
//
// What is NOT here:
//   * damage: nothing has health. An item has (five points), an armor stand breaks; a mob is
//     only thrown;
//   * a player's own velocity is not kept, so its knock-back is the client's alone;
//   * blocks that give experience when blown up by a player's TNT give none (no orbs);
//   * fire (Explosion.fire) and the wind charge's block triggers: no explosion here makes them.
#include <algorithm>
#include <bit>
#include <cmath>

#include "base/clock.hpp"
#include "base/log.hpp"
#include "server/game.hpp"

#include PROTO_GEN_PHYSICS_HPP
#include PROTO_GEN_STATIC_IDS_HPP

namespace server {

using namespace proto;
using world::Aabb;
using world::BlockState;
namespace block_kind = proto::gen::block_kind;

namespace {

constexpr i32 kFuse = 80;                        // PrimedTnt.DEFAULT_FUSE_TIME
constexpr float kPower = 4.0f;                   // PrimedTnt.DEFAULT_EXPLOSION_POWER
constexpr double kTwoPi = 6.2831854820251465;    // (double) ((float) Math.PI * 2)
constexpr double kLaunch = 0.20000000298023224;  // (double) 0.2f
constexpr double kExplodePacketRange2 = 4096.0;  // ServerLevel.explode: 64 blocks
constexpr i32 kStackCollectorLimit = 16;         // ServerExplosion.StackCollector.tryMerge
constexpr u64 kPopPickupDelay = 10;              // ItemEntity.setDefaultPickUpDelay
// Avatar.POSES, as in server/players.cpp.
constexpr float kPlayerWidth = 0.6f, kStandingHeight = 1.8f, kCrouchingHeight = 1.5f, kLowHeight = 0.6f;
constexpr float kEyeStanding = 1.62f, kEyeCrouching = 1.27f, kEyeLow = 0.4f;

using world::entity_box;
using world::intersects;
Aabb tnt_box(const entity::Store& e, u32 row) {
    const auto& info = entity_type::kInfo[entity_type::kTnt];
    return entity_box(e.x[row], e.y[row], e.z[row], info.width, info.height);
}

}  // namespace

// ---------------------------------------------------------------------------- the block

// SignalGetter.hasNeighborSignal: is a redstone signal arriving from one of the six sides? The
// only source there can be is a block that powers all its sides (PoweredBlock: a block of
// redstone); nothing conducts or emits a direct signal yet.
bool Game::has_neighbour_signal(BlockPos pos) const {
    for (u32 dir = 0; dir < 6; ++dir) {
        const BlockState s = world_.block(pos.x + world::kStepX[dir], pos.y + world::kStepY[dir], pos.z + world::kStepZ[dir]);
        if (s != world::kAir && world::block_info_of(s).kind == block_kind::kPowered) return true;
    }
    return false;
}

// TntBlock.prime / wasExploded: the block becomes an entity, hopping up in a random direction.
// (The caller takes the block away.)
void Game::prime_tnt(BlockPos pos, i32 fuse, bool sound) {
    if (ents_.size() >= entity::kMaxEntities) return;
    const double x = pos.x + 0.5, y = pos.y, z = pos.z + 0.5;
    const Uuid uuid{(random() & ~0xF000ull) | 0x4000ull, (random() & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull};
    const double rot = random_double() * kTwoPi;
    entity::Store::Moves moved;
    const u32 row = tracker_.spawn(entity_type::kTnt, false, uuid, x, y, z, 0, 0, moved);
    ents_.vx[row] = -std::sin(rot) * 0.02;
    ents_.vy[row] = kLaunch;
    ents_.vz[row] = -std::cos(rot) * 0.02;
    tracker_.created_with_velocity(row);
    ents_.fuse[row] = kFuse;  // what the synchronized value starts at: only another fuse is sent
    ents_.set_fuse(row, fuse);
    tnt_.push_back(ents_.slot[row]);
    start_ticking(ents_.slot[row], proto::gen::physics::kMoverTnt);
    if (sound) play_sound(gen::sound::kEntityTntPrimed, cb::Sound::kSourceBlocks, x, y, z, 1.0f, 1.0f);
}

// ---------------------------------------------------------------------------- the entity

// PrimedTnt.tick: it moves (phys::tnt_tick), its fuse burns - and at its end the explosion, then
// and there, so that what ticks after it already feels it.
void Game::tick_tnt(u32 slot, u32 row) {
    phys::Body& body = movers_[slot].body;
    phys::Motion m = motion_of(row);
    phys::World w{world_, phys_scratch_};
    phys::tnt_tick(w, m, body);
    // setRequiresPrecisePosition(horizontalCollision || verticalCollision): while it touches a
    // block its viewers get its exact position, not steps of 1/4096.
    const bool touching = body.horizontal_collision || body.vertical_collision;
    store_motion(row, m, body);
    ents_.flags[row] =
        static_cast<u16>(touching ? ents_.flags[row] | entity::flag::kPrecise : ents_.flags[row] & ~entity::flag::kPrecise);
    const i32 fuse = ents_.fuse[row] - 1;
    ents_.set_fuse(row, fuse);
    if (fuse > 0) return;
    // discard(), then explode() a sixteenth of its height up.
    const double x = m.pos[0], z = m.pos[2];
    const double y = m.pos[1] + static_cast<double>(entity_type::kInfo[entity_type::kTnt].height) * 0.0625;
    remove_entity(slot);
    explode(x, y, z, kPower);
}

// ---------------------------------------------------------------------------- the explosion

// ServerExplosion.getSeenPercent: how much of a box the centre of an explosion can see - rays
// from a grid of points on the box, each counted if no block's collision shape is in its way.
float Game::seen_percent(double cx, double cy, double cz, const Aabb& box) const {
    const double xs = 1.0 / ((box.max[0] - box.min[0]) * 2.0 + 1.0);
    const double ys = 1.0 / ((box.max[1] - box.min[1]) * 2.0 + 1.0);
    const double zs = 1.0 / ((box.max[2] - box.min[2]) * 2.0 + 1.0);
    const double x_offset = (1.0 - std::floor(1.0 / xs) * xs) / 2.0;
    const double z_offset = (1.0 - std::floor(1.0 / zs) * zs) / 2.0;
    if (xs < 0 || ys < 0 || zs < 0) return 0;
    const double center[3] = {cx, cy, cz};
    i32 hits = 0, count = 0;
    for (double xx = 0; xx <= 1.0; xx += xs) {
        for (double yy = 0; yy <= 1.0; yy += ys) {
            for (double zz = 0; zz <= 1.0; zz += zs) {
                const double from[3] = {box.min[0] + xx * (box.max[0] - box.min[0]) + x_offset,  // Mth.lerp
                                        box.min[1] + yy * (box.max[1] - box.min[1]),
                                        box.min[2] + zz * (box.max[2] - box.min[2]) + z_offset};
                hits += !world::clip_hits(world_, from, center);
                ++count;
            }
        }
    }
    return static_cast<float>(hits) / static_cast<float>(count);
}

// Level.explode(source, x, y, z, radius, false, ExplosionInteraction.TNT) -> ServerLevel.explode
// -> ServerExplosion.explode: which blocks go, what it does to the entities around, the blocks
// and what they drop, and the packet that makes a client see and feel it.
void Game::explode(double x, double y, double z, float radius) {
    const u64 began = base::now_ns();
    // ---- calculateExplodedPositions (world::blast_positions): the rays, each with a strength
    // drawn here; then Util.shuffle. (A HashSet's order, shuffled, is a random order: so is this.)
    float strengths[world::kBlastRays];
    for (float& strength : strengths) strength = random_float();
    world::blast_positions(world_, x, y, z, radius, strengths, blast_scratch_, blown_);
    for (usize i = blown_.size(); i > 1; --i) std::swap(blown_[i - 1], blown_[random_int(static_cast<u32>(i))]);

    const u64 rays_done = base::now_ns();

    // ---- hurtEntities: everything within twice the radius, by how much of it the centre sees.
    const double reach = static_cast<double>(radius * 2.0f);
    const Aabb area{{std::floor(x - reach - 1.0), std::floor(y - reach - 1.0), std::floor(z - reach - 1.0)},
                    {std::floor(x + reach + 1.0), std::floor(y + reach + 1.0), std::floor(z + reach + 1.0)}};
    struct Hit {
        bool in_reach = false;
        double power = 0, dir[3] = {0, 0, 0};
        float exposure = 0;
    };
    // What ServerExplosion computes for one entity: `origin` is where the push is aimed at
    // (the eyes; the feet for a primed TNT).
    const auto hit = [&](const Aabb& box, double ex, double ey, double ez, double origin_y) {
        Hit h;
        if (!intersects(box, area)) return h;
        const double dx = ex - x, dy = ey - y, dz = ez - z;
        const double dist = std::sqrt(dx * dx + dy * dy + dz * dz) / reach;  // Entity.distanceToSqr: from its feet
        if (dist > 1.0) return h;
        h.in_reach = true;
        const double ox = ex - x, oy = origin_y - y, oz = ez - z;
        const double len = std::sqrt(ox * ox + oy * oy + oz * oz);
        if (len >= 1.0e-5) h.dir[0] = ox / len, h.dir[1] = oy / len, h.dir[2] = oz / len;  // Vec3.normalize
        h.exposure = seen_percent(x, y, z, box);
        h.power = (1.0 - dist) * static_cast<double>(h.exposure);  // knock-back multiplier and resistance: 1 and 0
        return h;
    };

    // Players: no damage in creative mode; the push goes into the packet below, for those who
    // are not flying (ServerExplosion.hitPlayers).
    const u32 np = static_cast<u32>(players_.size());
    knock_.assign(np, Knock{});
    for (u32 pi = 0; pi < np; ++pi) {
        const u8 pose_now = ents_.pose[pi];
        const float height = pose_now == pose::kStanding    ? kStandingHeight
                             : pose_now == pose::kCrouching ? kCrouchingHeight
                                                            : kLowHeight;
        const float eye = pose_now == pose::kStanding ? kEyeStanding : pose_now == pose::kCrouching ? kEyeCrouching : kEyeLow;
        const Hit h = hit(entity_box(ents_.x[pi], ents_.y[pi], ents_.z[pi], kPlayerWidth, height), ents_.x[pi], ents_.y[pi],
                          ents_.z[pi], ents_.y[pi] + static_cast<double>(eye));
        if (!h.in_reach || players_[pi].flying) continue;
        knock_[pi] = {true, {h.dir[0] * h.power, h.dir[1] * h.power, h.dir[2] * h.power}};
    }
    // Primed TNT: never hurt, pushed (Entity.pushFromExplosion: the velocity, and viewers hear it at once).
    for (const u32 slot : tnt_) {
        const u32 row = ents_.by_slot(slot);
        const Hit h = hit(tnt_box(ents_, row), ents_.x[row], ents_.y[row], ents_.z[row], ents_.y[row]);
        if (h.in_reach) push(row, h.dir[0] * h.power, h.dir[1] * h.power, h.dir[2] * h.power);
    }
    // Falling blocks: thrown from where their eyes are, and their viewers hear of it at once
    // (FallingBlockEntity.hurtServer: markHurt, no more).
    for (const u32 slot : falling_slots_) {
        const u32 row = ents_.by_slot(slot);
        const entity_type::Info& info = entity_type::kInfo[entity_type::kFallingBlock];
        const Hit h = hit(entity_box(ents_.x[row], ents_.y[row], ents_.z[row], info.width, info.height), ents_.x[row],
                          ents_.y[row], ents_.z[row], ents_.y[row] + static_cast<double>(info.eye_height));
        if (!h.in_reach) continue;
        push(row, h.dir[0] * h.power, h.dir[1] * h.power, h.dir[2] * h.power);
        ents_.flags[row] |= entity::flag::kSyncVelocity;
    }
    // Items: ItemEntity.hurtServer - five points of health, and an explosion this close takes
    // more (what resists explosions stays: the nether star) - and what is left is thrown.
    if (!item_slot_.empty()) {
        item_scratch_.clear();
        item_grid_.for_each(item_cell_of(area.min[0] - 0.125), item_cell_of(area.min[2] - 0.125),
                            item_cell_of(area.max[0] + 0.125), item_cell_of(area.max[2] + 0.125),
                            [&](u32 slot) { item_scratch_.push_back(slot); });
        for (const u32 slot : item_scratch_) {
            const u32 row = ents_.by_slot(slot);
            const ItemStack& stack = ents_.item[row];
            if (stack.empty()) continue;
            const Hit h = hit(entity_box(ents_.x[row], ents_.y[row], ents_.z[row], 0.25f, 0.25f), ents_.x[row], ents_.y[row],
                              ents_.z[row], ents_.y[row] + static_cast<double>(0.25f * 0.85f));
            if (!h.in_reach) continue;
            push(row, h.dir[0] * h.power, h.dir[1] * h.power, h.dir[2] * h.power);
            const bool proof =
                (stack.info().flags & world::item_flag::kExplosionProof) &&
                !(stack.has_patch() &&
                  item::patches().find(stack.patch(), item::component::kDamageResistant).state == item::Patches::State::kRemoved);
            if (proof) continue;
            // ExplosionDamageCalculator.getEntityDamageAmount
            const float damage = static_cast<float>((h.power * h.power + h.power) / 2.0 * 7.0 * reach + 1.0);
            i8& health = item_health_[item_of_slot_[slot]];
            health = static_cast<i8>(static_cast<i32>(static_cast<float>(health) - damage));
            ents_.flags[row] |= entity::flag::kSyncVelocity;  // markHurt
            if (health <= 0) remove_item(slot);
        }
    }
    // The living: thrown from where their eyes are. An armor stand in reach is broken by it
    // (ArmorStand.hurtServer: any explosion), whether or not the centre can see it.
    if (push_grid_.size() > np) {
        const double wide = static_cast<double>(push_half_width_);
        push_scratch_.clear();
        push_grid_.for_each(push_cell_of(area.min[0] - wide), push_cell_of(area.min[2] - wide), push_cell_of(area.max[0] + wide),
                            push_cell_of(area.max[2] + wide), [&](u32 slot) { push_scratch_.push_back(slot); });
        for (const u32 slot : push_scratch_) {
            const u32 row = ents_.by_slot(slot);
            if (row < np) continue;
            const entity_type::Info& info = entity_type::kInfo[ents_.type[row]];
            const Hit h = hit(entity_box(ents_.x[row], ents_.y[row], ents_.z[row], info.width, info.height), ents_.x[row],
                              ents_.y[row], ents_.z[row], ents_.y[row] + static_cast<double>(info.eye_height));
            if (!h.in_reach) continue;
            if (movers_[slot].kind == proto::gen::physics::kMoverArmorStand) {
                // brokenByAnything (it holds nothing to drop), then kill.
                play_sound(gen::sound::kEntityArmorStandBreak, cb::Sound::kSourceNeutral, ents_.x[row], ents_.y[row],
                           ents_.z[row], 1.0f, 1.0f);
                remove_entity(slot);
            } else {
                push(row, h.dir[0] * h.power, h.dir[1] * h.power, h.dir[2] * h.power);
            }
        }
    }

    const u64 entities_done = base::now_ns();

    // ---- interactWithBlocks: every block drops what its loot table gives without a tool (TNT
    // does not: it goes off), stacks of one kind are gathered up to 16, and the block is gone.
    collected_.clear();
    for (const world::BlockAt at : blown_) {
        const BlockPos pos{at.x, at.y, at.z};
        const BlockState state = world_.block(pos.x, pos.y, pos.z);
        if (state == world::kAir || world::is_air(state)) continue;
        const world::BlockInfo& info = world::block_info_of(state);
        if (info.kind != block_kind::kTnt) {
            // BlockBehaviour.onExplosionHit -> getDrops: the first candidate with its chance, else the second.
            const world::BlockInfo::Drop& drop =
                info.drop_chance >= 1.0f || random_float() < info.drop_chance ? info.drop[0] : info.drop[1];
            if (drop.item) {
                ItemStack stack(drop.item, drop.min + random_int(static_cast<u32>(drop.max - drop.min) + 1u));
                // addOrAppendStack: ItemEntity.areMergable / merge(.., 16) against every stack so far.
                for (Collected& c : collected_) {
                    if (stack.count() + c.stack.count() > stack.max_stack() || !item::same_item_same_components(c.stack, stack))
                        continue;
                    const i32 have = static_cast<i32>(c.stack.count());
                    const i32 delta = std::min(std::min(static_cast<i32>(c.stack.max_stack()), kStackCollectorLimit) - have,
                                               static_cast<i32>(stack.count()));
                    c.stack.set_count(static_cast<u32>(have + delta));
                    stack.shrink(static_cast<u32>(std::max(delta, 0)));
                    if (stack.empty()) break;
                }
                if (!stack.empty()) collected_.push_back({pos, std::move(stack)});
            }
        }
        set_block(pos, world::kAir);        // setBlockAndUpdate: a chest spills, the neighbours hear of it
        if (info.kind == block_kind::kTnt)  // TntBlock.wasExploded: a short fuse, 10 to 29 ticks
            prime_tnt(pos, static_cast<i32>(random_int(static_cast<u32>(std::max(1, kFuse / 4)))) + kFuse / 8, false);
    }
    for (Collected& c : collected_) {  // Block.popResource: near the middle of the block
        const double px = c.pos.x + 0.5 + (random_double() * 0.5 - 0.25);
        const double py = c.pos.y + 0.5 + (random_double() * 0.5 - 0.25) - 0.125;
        const double pz = c.pos.z + 0.5 + (random_double() * 0.5 - 0.25);
        spawn_item(std::move(c.stack), px, py, pz, kPopPickupDelay);
    }
    collected_.clear();

    // ---- ServerLevel.explode: the packet, to everyone within 64 blocks.
    static constexpr cb::ExplosionParticle kBlockParticles[] = {
        // Level.DEFAULT_EXPLOSION_BLOCK_PARTICLES
        {gen::particle::kPoof, 0.5f, 1.0f, 1},
        {gen::particle::kSmoke, 1.0f, 1.0f, 1},
    };
    cb::Explode pkt{};
    pkt.x = x, pkt.y = y, pkt.z = z;
    pkt.radius = radius;
    pkt.block_count = static_cast<i32>(blown_.size());
    // ServerExplosion.isSmall: a radius under 2 (or one that leaves the blocks alone).
    pkt.particle = radius < 2.0f ? gen::particle::kExplosion : gen::particle::kExplosionEmitter;
    pkt.sound = gen::sound::kEntityGenericExplode;
    pkt.block_particles = kBlockParticles;
    pkt.play_sound = true;
    scratch_.clear();
    write_packet(scratch_, s_.compression_threshold, pkt);  // the same for everyone it does not push
    for (u32 pi = 0; pi < np; ++pi) {
        const double dx = ents_.x[pi] - x, dy = ents_.y[pi] - y, dz = ents_.z[pi] - z;
        if (dx * dx + dy * dy + dz * dz >= kExplodePacketRange2) continue;
        if (!knock_[pi].hit) {
            out(pi).append(scratch_.data, scratch_.len);
            continue;
        }
        cb::Explode own = pkt;
        own.knocked = true;
        own.kx = knock_[pi].v[0], own.ky = knock_[pi].v[1], own.kz = knock_[pi].v[2];
        send(pi, own);
    }
    const u64 done = base::now_ns();
    LOG_DEBUG("explosion at (%.1f, %.1f, %.1f): %zu blocks in reach; %.3f ms (rays %.3f, entities %.3f, blocks %.3f)", x, y, z,
              blown_.size(), static_cast<double>(done - began) / 1e6, static_cast<double>(rays_done - began) / 1e6,
              static_cast<double>(entities_done - rays_done) / 1e6, static_cast<double>(done - entities_done) / 1e6);
}

}  // namespace server
