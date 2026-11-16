// What moves by itself: the entities' own tick, in the order vanilla ticks them, and what
// living things do to each other.
//
// The rules are vanilla's, read from the 26.3 jar; each function names the method it follows:
//   ServerLevel.tick (the entity loop: EntityTickList, inEntityTickingRange), tickNonPassenger
//   LivingEntity.tick / aiStep / pushEntities / doPush / isPushable, Entity.push, ArmorStand
// The physics itself - where an entity is after a tick - is src/phys/physics.cpp, a set of pure
// functions that is tested against recordings of a vanilla server; this file is what holds the
// state between ticks and carries it to and from the entity table.
//
// One list for everything that ticks (items, primed TNT, the living), in the order of
// creation: vanilla ticks entities in that order, and what one does to another within a tick
// - an explosion that throws an item about, two items that merge, two mobs that push each
// other - comes out differently in another. What is created while the list is walked ticks from
// the next tick on, what is removed is skipped; both as in EntityTickList.
//
// At rest. Nearly everything in a world lies still, and what lies still is not ticked: a
// living entity whose tick changed nothing is skipped until something wakes it, an item that
// phys::item_rests() says is at rest has its tick done on three numbers (phys::item_rest_tick).
// Both are exactly what the full tick would have done - as long as what that tick reads stays
// as it is. So whatever changes it wakes the entity: a block set within reach (Game::set_block
// -> wake_near), a push or a teleport, another entity put where it stands, a player in its box.
// A chunk that stops ticking wakes the items in it (Game::tend_chunks): at rest or not, nothing
// is ticked where chunks do not tick.
//
// What is NOT here (docs/architecture.md has the reasons):
//   * a brain: a living entity is moved as LivingEntity.travel moves one that wants nothing -
//     it falls, slides, is pushed and steps up, and that is all. Types that fly, swim, climb or
//     hang (gen_physics.OWN_MOVEMENT) stay where they are put;
//   * players: a player's own client moves it. A player pushes what it walks into and is not
//     pushed back here (vanilla tells its viewers the velocity it gets);
//   * cramming, vehicles, leashes, fluids, health (see phys/physics.hpp).
#include <algorithm>
#include <cmath>

#include "server/game.hpp"

#include PROTO_GEN_PHYSICS_HPP

namespace server {

using namespace proto;
namespace flag = entity::flag;
namespace gp = proto::gen::physics;

namespace {

// Avatar.POSES, as in server/players.cpp.
constexpr float kPlayerWidth = 0.6f, kStandingHeight = 1.8f, kCrouchingHeight = 1.5f, kLowHeight = 0.6f;

float player_height(u32 pose_now) {
    return pose_now == pose::kStanding ? kStandingHeight : pose_now == pose::kCrouching ? kCrouchingHeight : kLowHeight;
}

}  // namespace

// ---------------------------------------------------------------------------- the list

// Level.addFreshEntity, for what ticks: its place at the end of the list, and its Body.
void Game::start_ticking(u32 slot, u8 mover) {
    if (slot >= movers_.size()) movers_.resize(std::max<usize>(ents_.slot_count(), slot + 1));
    if (slot >= item_sleep_.size()) item_sleep_.resize(movers_.size());
    Mover& mv = movers_[slot];
    mv = {};
    mv.kind = mover;
    mv.order = static_cast<u32>(tick_order_.size());
    tick_order_.push_back(slot);
}

// Entity.discard: out of the list at once. Its place stays (a hole) until the list is next packed.
void Game::stop_ticking(u32 slot) {
    if (slot >= movers_.size() || movers_[slot].kind == gp::kMoverNone) return;
    wake(slot);
    Mover& mv = movers_[slot];
    tick_order_[mv.order] = kNone;
    ++tick_holes_;
    mv.kind = gp::kMoverNone;
    mv.order = kNone;
}

// ServerLevel.tick: every entity, in the order it was created.
void Game::tick_entities() {
    const u32 n = static_cast<u32>(tick_order_.size());  // what is created during the pass ticks from the next one on
    entities_ticked_ = true;
    if (UNLIKELY(asleep_living_ != 0)) wake_at_players();
    for (u32 i = 0; i < n; ++i) {
        const u32 entry = tick_order_[i];
        if (entry & kAsleep) {
            // A hole; or a living entity at rest, whose tick changes nothing; or an item at rest.
            if (entry == kNone || !(entry & kAsleepItem)) continue;
            rest_item(entry & kSlotMask);
            continue;
        }
        const u32 slot = entry;
        const u32 row = ents_.by_slot(slot);
        // inEntityTickingRange: not in a chunk that is not loaded with its neighbours - there
        // would be nothing to stand on. (Vanilla draws that line at the simulation distance of
        // the nearest player; here a chunk ticks while every chunk around it is loaded and lit,
        // which is so within a view and for a while after the last player has gone.)
        const world::Chunk* chunk = world_.find(chunk_of(ents_.x[row]), chunk_of(ents_.z[row]));
        const u8 kind = movers_[slot].kind;
        if (UNLIKELY(!chunk || !chunk->ticking)) {
            if (kind == gp::kMoverItem) freeze_item(slot);
            // Thrown past everything that is loaded: the chunk it is over is brought in, so that
            // there is a chunk to keep it - an entity is saved with the chunk it stands in.
            if (UNLIKELY(!chunk)) world_.request(chunk_of(ents_.x[row]), chunk_of(ents_.z[row]));
            continue;
        }
        if (kind == gp::kMoverItem)
            tick_item(slot, row);
        else if (kind == gp::kMoverTnt)
            tick_tnt(slot, row);
        else if (LIKELY(kind != kMoverEmptyItem))
            tick_living(slot, row);
        else
            remove_entity(slot);  // ItemEntity.tick: "if (getItem().isEmpty()) discard()"
    }
    // Pack the list when a quarter of it is holes: the order stays.
    if (UNLIKELY(tick_holes_ > 64 && tick_holes_ * 4 > tick_order_.size())) {
        u32 kept = 0;
        for (const u32 entry : tick_order_) {
            if (entry == kNone) continue;
            movers_[entry & kSlotMask].order = kept;
            tick_order_[kept++] = entry;
        }
        tick_order_.resize(kept);
        tick_holes_ = 0;
    }
}

// ---------------------------------------------------------------------------- to and from the entity table

phys::Motion Game::motion_of(u32 row) const {
    return {{ents_.x[row], ents_.y[row], ents_.z[row]}, {ents_.vx[row], ents_.vy[row], ents_.vz[row]}};
}

// What a tick of physics left: where the entity is, and what the tracker reads off its flags.
void Game::store_motion(u32 row, const phys::Motion& m, phys::Body& b) {
    ents_.x[row] = m.pos[0], ents_.y[row] = m.pos[1], ents_.z[row] = m.pos[2];
    ents_.vx[row] = m.vel[0], ents_.vy[row] = m.vel[1], ents_.vz[row] = m.vel[2];
    u16 f = static_cast<u16>(ents_.flags[row] & ~(flag::kOnGround | flag::kHorizontalCollision | flag::kVerticalCollision));
    if (b.on_ground) f |= flag::kOnGround;
    if (b.horizontal_collision) f |= flag::kHorizontalCollision;
    if (b.vertical_collision) f |= flag::kVerticalCollision;
    if (b.needs_sync) f |= flag::kNeedsSync;
    if (b.sync_position) f |= flag::kSyncPosition;
    b.needs_sync = b.sync_position = false;
    ents_.flags[row] = f;
}

// Entity.push(x, y, z): an impulse from outside - an explosion, another entity.
void Game::push(u32 row, double x, double y, double z) {
    if (!(std::isfinite(x) && std::isfinite(y) && std::isfinite(z))) return;
    wake(ents_.slot[row]);
    ents_.vx[row] = ents_.vx[row] + x;
    ents_.vy[row] = ents_.vy[row] + y;
    ents_.vz[row] = ents_.vz[row] + z;
    ents_.flags[row] |= flag::kNeedsSync;
}

// ---------------------------------------------------------------------------- the living

// LivingEntity.tick for what has no brain: aiStep's travel, then pushEntities.
void Game::tick_living(u32 slot, u32 row) {
    const u32 type = ents_.type[row];
    const entity_type::Info& info = entity_type::kInfo[type];
    const gp::EntityPhysics& how = gp::kEntity[type];
    const phys::Kind kind = phys::living_kind(info.width, info.height, how.step_height, how.gravity);
    Mover& mv = movers_[slot];
    phys::Motion m = motion_of(row);
    phys::World w{world_, phys_scratch_};
    // Standing still already? Then what it was is kept, to see whether this tick changes any of it.
    const bool still = mv.body.on_ground && m.vel[0] == 0 && m.vel[2] == 0;
    phys::Motion motion_before;
    phys::Body body_before;
    if (still) motion_before = m, body_before = mv.body;
    if (UNLIKELY(phys::living_tick(w, kind, m, mv.body) == phys::Fate::kGone)) {
        remove_entity(slot);
        return;
    }
    const bool same = still && phys::same_state(motion_before, body_before, m, mv.body);
    store_motion(row, m, mv.body);
    move_in_push_grid(slot, m.pos[0], m.pos[2]);
    // ArmorStand.pushEntities: minecarts only.
    const bool pushed = mv.kind == gp::kMoverLiving && push_entities(row, kind.width, kind.height);
    // A tick that left every bit of it as it was, and nobody pushing: the next will too.
    if (same && !pushed) sleep(slot);
}

// At rest from now on: tick_entities() passes it by (a living entity) or gives it rest_item().
void Game::sleep(u32 slot) {
    Mover& mv = movers_[slot];
    DCHECK(!mv.asleep && mv.order != kNone);
    mv.asleep = true;
    const bool item = mv.kind == gp::kMoverItem;
    tick_order_[mv.order] = slot | kAsleep | (item ? kAsleepItem : 0);
    ++asleep_;
    asleep_living_ += !item;
}

// Something about it or around it changed: it is ticked in full again, from the state it has -
// which is, to the bit, the one the full ticks would have left it in.
void Game::wake(u32 slot) {
    if (slot >= movers_.size() || !movers_[slot].asleep) return;
    Mover& mv = movers_[slot];
    mv.asleep = false;
    mv.patience = 0;
    mv.retry = 0;
    tick_order_[mv.order] = slot;
    --asleep_;
    asleep_living_ -= mv.kind != gp::kMoverItem;
}

// A block changed (Game::set_block): whatever is at rest within reach of it - and of its
// neighbours, which may have changed shape with it - is ticked in full again.
void Game::wake_near(BlockPos pos) {
    if (LIKELY(asleep_ == 0)) return;
    const auto in_reach = [&](u32 slot, float width, float height) {
        const u32 row = ents_.by_slot(slot);
        const phys::Motion m{{ents_.x[row], ents_.y[row], ents_.z[row]}, {0, 0, 0}};
        return phys::block_in_reach(m, width, height, pos.x, pos.y, pos.z);
    };
    if (asleep_ != asleep_living_) {
        item_grid_.for_each(item_cell_of(pos.x - 4.0), item_cell_of(pos.z - 4.0), item_cell_of(pos.x + 5.0),
                            item_cell_of(pos.z + 5.0), [&](u32 slot) {
                                if (movers_[slot].asleep && in_reach(slot, phys::kItem.width, phys::kItem.height)) wake(slot);
                            });
    }
    if (asleep_living_ != 0) {
        const double reach = static_cast<double>(push_half_width_) + 4.0;
        push_grid_.for_each(push_cell_of(pos.x - reach), push_cell_of(pos.z - reach), push_cell_of(pos.x + 1.0 + reach),
                            push_cell_of(pos.z + 1.0 + reach), [&](u32 slot) {
                                if (slot >= movers_.size() || !movers_[slot].asleep) return;
                                const entity_type::Info& info = entity_type::kInfo[ents_.type[ents_.by_slot(slot)]];
                                if (in_reach(slot, info.width, info.height)) wake(slot);
                            });
    }
}

// Something that can be pushed was put here by other means than its own tick (a teleport, a
// summon): the living at rest that it touches have their own tick again, which pushes.
void Game::wake_touching(const world::Aabb& box, u32 but) {
    if (LIKELY(asleep_living_ == 0)) return;
    const double reach = static_cast<double>(push_half_width_);
    push_grid_.for_each(
        push_cell_of(box.min[0] - reach), push_cell_of(box.min[2] - reach), push_cell_of(box.max[0] + reach),
        push_cell_of(box.max[2] + reach), [&](u32 slot) {
            if (slot == but || slot >= movers_.size() || !movers_[slot].asleep) return;
            const u32 row = ents_.by_slot(slot);
            const entity_type::Info& info = entity_type::kInfo[ents_.type[row]];
            if (world::intersects(box, world::entity_box(ents_.x[row], ents_.y[row], ents_.z[row], info.width, info.height)))
                wake(slot);
        });
}

// A player moves when its packets say so, not in a tick of its own: before the entities tick,
// the living at rest that a player stands in are woken (LivingEntity.pushEntities finds it).
void Game::wake_at_players() {
    const u32 np = static_cast<u32>(players_.size());
    for (u32 pi = 0; pi < np; ++pi)
        wake_touching(world::entity_box(ents_.x[pi], ents_.y[pi], ents_.z[pi], kPlayerWidth, player_height(ents_.pose[pi])),
                      kNone);
}

// LivingEntity.pushEntities: whatever can be pushed and touches this entity's box pushes it,
// and is pushed by it (doPush(e) is e.push(this)). `row` is the pusher: a mob, or a player.
//
// Vanilla takes the others in the order its entity sections hand them out; here they come in
// the order of the grid's cells. With one neighbour that is the same thing; with several the
// impulses are added up in another order and may differ in their last bit.
bool Game::push_entities(u32 row, float width, float height) {
    bool pushed = false;
    const world::Aabb box = world::entity_box(ents_.x[row], ents_.y[row], ents_.z[row], width, height);
    const double reach = static_cast<double>(push_half_width_);
    push_scratch_.clear();
    push_grid_.for_each(push_cell_of(box.min[0] - reach), push_cell_of(box.min[2] - reach), push_cell_of(box.max[0] + reach),
                        push_cell_of(box.max[2] + reach), [&](u32 slot) { push_scratch_.push_back(slot); });
    const u32 np = static_cast<u32>(players_.size());
    for (const u32 slot : push_scratch_) {
        const u32 other = ents_.by_slot(slot);
        if (other == row) continue;
        float w, h;
        if (other < np) {
            w = kPlayerWidth, h = player_height(ents_.pose[other]);
        } else {
            if (movers_[slot].kind != gp::kMoverLiving) continue;  // LivingEntity.isPushable; an armor stand is not
            w = entity_type::kInfo[ents_.type[other]].width, h = entity_type::kInfo[ents_.type[other]].height;
        }
        if (!world::intersects(box, world::entity_box(ents_.x[other], ents_.y[other], ents_.z[other], w, h))) continue;
        // Entity.push(Entity), as other.push(this).
        const double a[3] = {ents_.x[other], ents_.y[other], ents_.z[other]}, b[3] = {ents_.x[row], ents_.y[row], ents_.z[row]};
        double xa, za;
        if (!phys::push_impulse(a, b, xa, za)) continue;
        if (other >= np) push(other, -xa, 0.0, -za);  // (a player's velocity is its client's business)
        if (row >= np) push(row, xa, 0.0, za);
        pushed = true;
    }
    return pushed;
}

// ---------------------------------------------------------------------------- where the pushable are

// Mobs and players by 4-block cell: who could be touching whom.
void Game::enter_push_grid(u32 slot, double x, double z, float width) {
    if (slot >= movers_.size()) movers_.resize(std::max<usize>(ents_.slot_count(), slot + 1));
    Mover& mv = movers_[slot];
    mv.gx = push_cell_of(x), mv.gz = push_cell_of(z);
    mv.cell = push_grid_.add(mv.gx, mv.gz, slot);
    mv.in_grid = true;
    push_half_width_ = std::max(push_half_width_, width / 2.0f);
}

void Game::leave_push_grid(u32 slot) {
    if (slot >= movers_.size() || !movers_[slot].in_grid) return;
    Mover& mv = movers_[slot];
    const u32 filler = push_grid_.remove(mv.gx, mv.gz, mv.cell);
    if (filler != ~0u) movers_[filler].cell = mv.cell;
    mv.in_grid = false;
}

void Game::move_in_push_grid(u32 slot, double x, double z) {
    Mover& mv = movers_[slot];
    const i32 gx = push_cell_of(x), gz = push_cell_of(z);
    if (LIKELY(gx == mv.gx && gz == mv.gz) || !mv.in_grid) return;
    const u32 filler = push_grid_.remove(mv.gx, mv.gz, mv.cell);
    if (filler != ~0u) movers_[filler].cell = mv.cell;
    mv.gx = gx, mv.gz = gz;
    mv.cell = push_grid_.add(gx, gz, slot);
}

}  // namespace server
