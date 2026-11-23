// SPDX-License-Identifier: GPL-3.0-or-later
// Item entities: stacks lying in the world.
//
// The rules are vanilla's, read from the 26.3 jar; each function names the method it follows:
//   ItemEntity (constructors, tick, mergeWithNeighbours, tryToMerge, merge, playerTouch),
//   Player.aiStep (the pickup area), LivingEntity.take, Inventory.add
//
// An item moves as vanilla's does: thrown, falling, sliding, coming to rest (phys::item_tick is
// the part of ItemEntity.tick that decides where it is; this file is the rest of it - the
// stack, its age, when it merges). A client simulates items as well
// (ItemEntity.getMoveSimulationType is SERVER_AND_CLIENT), so the two have to agree tick by
// tick, and what they disagree on shows as a twitch at the next position update.
//
// An item entity is a row of the entity table (its stack is the `item` column, which the
// tracker shows as synchronized data) plus the timers below, kept beside the table by slot, and
// an entry in a grid of 2-block cells for the two questions asked every tick: which items can
// this player reach, which items lie next to this one.
//
// What is NOT here: water and lava (an item floats in one and burns in the other), hoppers.
#include "server/game.hpp"

#include PROTO_GEN_PHYSICS_HPP
#include PROTO_GEN_STATIC_IDS_HPP

namespace server {

using namespace proto;

namespace {

constexpr u64 kLifetime = 6000;        // ItemEntity.LIFETIME: age at which it is discarded
constexpr i8 kHealth = 5;              // ItemEntity.health: what an explosion has to take before it is gone
constexpr u32 kMergePeriod = 40;       // ItemEntity.tick: an item that did not change block looks for neighbours this often,
constexpr u32 kMovingMergePeriod = 2;  // ...and one that did, this often
constexpr i32 kMergeLimit = 64;        // ItemEntity.merge(to, from): never beyond 64, whatever the stack allows
// EntityTypes.ITEM: 0.25 x 0.25. (Exact in float and double alike.)
constexpr double kItemHalfWidth = 0.125, kItemHeight = 0.25;
// Avatar.POSES: 0.6 wide; the heights a player on foot has.
constexpr float kPlayerWidth = 0.6f, kStandingHeight = 1.8f, kCrouchingHeight = 1.5f, kLowHeight = 0.6f;

struct Box {
    double min[3], max[3];
    // AABB.intersects: touching is not intersecting.
    bool intersects(const Box& o) const {
        return min[0] < o.max[0] && max[0] > o.min[0] && min[1] < o.max[1] && max[1] > o.min[1] && min[2] < o.max[2] &&
               max[2] > o.min[2];
    }
};

Box item_box(const entity::Store& e, u32 row) {  // EntityDimensions.makeBoundingBox
    return {{e.x[row] - kItemHalfWidth, e.y[row], e.z[row] - kItemHalfWidth},
            {e.x[row] + kItemHalfWidth, e.y[row] + kItemHeight, e.z[row] + kItemHalfWidth}};
}

}  // namespace

// new ItemEntity(level, x, y, z, stack): it hops up, and up to a tenth of a block a tick sideways.
void Game::spawn_item(ItemStack stack, double x, double y, double z, u64 pickup_delay, u64 age) {
    const double vx = random_double() * 0.2 - 0.1, vz = random_double() * 0.2 - 0.1;
    spawn_item(std::move(stack), x, y, z, vx, 0.2, vz, pickup_delay, age);
}

// new ItemEntity(level, x, y, z, stack, vx, vy, vz) + Level.addFreshEntity: a random yaw, and
// `pickup_delay` ticks before it can be picked up - 40 for what a player drops
// (createItemStackToDrop), 10 for what a block drops (setDefaultPickUpDelay), none for what
// falls out of a chest.
void Game::spawn_item(ItemStack stack, double x, double y, double z, double vx, double vy, double vz, u64 pickup_delay, u64 age) {
    if (stack.empty() || ents_.size() >= entity::kMaxEntities) return;
    // Mth.createInsecureUUID, and the yaw the ItemEntity constructor draws: random.nextFloat() * 360.
    const Uuid uuid{(random() & ~0xF000ull) | 0x4000ull, (random() & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull};
    const float yaw = static_cast<float>(random() >> 40) / static_cast<float>(1 << 24) * 360.0f;
    add_item(uuid, yaw, std::move(stack), x, y, z, vx, vy, vz, pickup_delay, age, kHealth);
}

// An item entity with everything about it said: a new one (spawn_item), or one that comes back
// with its chunk (server/persist.cpp). Returns its slot.
u32 Game::add_item(Uuid uuid, float yaw, ItemStack stack, double x, double y, double z, double vx, double vy, double vz,
                   u64 pickup_delay, u64 age, i8 health) {
    entity::Store::Moves moved;
    const u32 row = tracker_.spawn(entity_type::kItem, false, uuid, x, y, z, yaw, 0, moved);
    ents_.set_item(row, std::move(stack));
    ents_.vx[row] = vx, ents_.vy[row] = vy, ents_.vz[row] = vz;
    tracker_.created_with_velocity(row);

    const u32 slot = ents_.slot[row];
    if (slot >= item_of_slot_.size()) item_of_slot_.resize(ents_.slot_count(), kNone);
    item_of_slot_[slot] = static_cast<u32>(item_slot_.size());
    item_slot_.push_back(slot);
    item_cell_.push_back(item_grid_.add(item_cell_of(x), item_cell_of(z), slot));
    // Its first tick: the next pass over the entities - which is the one after this, if this is
    // one (what an explosion or a broken chest leaves while they tick is ticked from the next on).
    const u64 first = entity_tick_ + (entities_ticked_ ? 1 : 0);
    item_born_.push_back(first);
    item_age_from_.push_back(first - age);  // (modulo 2^64, as every difference taken from it is)
    // The delay counts down in the item's own tick, which runs before the players': after its
    // 40th tick an item with a delay of 40 can be taken - entity_tick_, outside a pass, being the
    // passes there have been. kNever: ItemEntity.setNeverPickUp.
    item_pickup_.push_back(pickup_delay == kNever ? kNever : first + pickup_delay);
    item_health_.push_back(health);
    start_ticking(slot, proto::gen::physics::kMoverItem);
    return slot;
}

// Entity.discard: whoever sees it is told at once, then the entity is gone.
void Game::remove_item(u32 slot) {
    const u32 k = item_of_slot_[slot], last = static_cast<u32>(item_slot_.size()) - 1;
    DCHECK(k != kNone);
    const u32 row = ents_.by_slot(slot);
    // Out of its cell; the cell's last item takes its place there.
    const u32 filler = item_grid_.remove(item_cell_of(ents_.x[row]), item_cell_of(ents_.z[row]), item_cell_[k]);
    if (filler != ~0u) item_cell_[item_of_slot_[filler]] = item_cell_[k];
    stop_ticking(slot);
    entity::Store::Moves moved;
    tracker_.despawn(row, out_.data(), moved);  // rows after the players move; nothing here goes by row
    if (k != last) {
        item_slot_[k] = item_slot_[last];
        item_born_[k] = item_born_[last];
        item_age_from_[k] = item_age_from_[last];
        item_pickup_[k] = item_pickup_[last];
        item_health_[k] = item_health_[last];
        item_cell_[k] = item_cell_[last];
        item_of_slot_[item_slot_[k]] = k;
    }
    item_slot_.pop_back();
    item_born_.pop_back();
    item_age_from_.pop_back();
    item_pickup_.pop_back();
    item_health_.pop_back();
    item_cell_.pop_back();
    item_of_slot_[slot] = kNone;
}

// An item was moved: the grid keeps it in the cell it stands in.
void Game::move_item_cell(u32 slot, double old_x, double old_z) {
    const u32 row = ents_.by_slot(slot), k = item_of_slot_[slot];
    const i32 ox = item_cell_of(old_x), oz = item_cell_of(old_z), nx = item_cell_of(ents_.x[row]),
              nz = item_cell_of(ents_.z[row]);
    if (LIKELY(ox == nx && oz == nz)) return;
    const u32 filler = item_grid_.remove(ox, oz, item_cell_[k]);
    if (filler != ~0u) item_cell_[item_of_slot_[filler]] = item_cell_[k];
    item_cell_[k] = item_grid_.add(nx, nz, slot);
}

// ItemEntity.tick: it moves (phys::item_tick), merges with what lies next to it, ages, expires.
void Game::tick_item(u32 slot, u32 row) {
    u32 k = item_of_slot_[slot];
    if (UNLIKELY(ents_.item[row].empty())) return remove_item(slot);  // "if (getItem().isEmpty()) discard()"
    const double ox = ents_.x[row], oy = ents_.y[row], oz = ents_.z[row];
    phys::Motion m = motion_of(row);
    phys::World w = phys_world();
    const u32 tick_count = static_cast<u32>(entity_tick_ - item_born_[k] + 1);  // Entity.tickCount
    const i32 id = ents_.net_id(row);
    phys::Body& body = movers_[slot].body;
    if (UNLIKELY(phys::item_tick(w, m, body, tick_count, id, &random_float_of, this) == phys::Fate::kGone))
        return remove_item(slot);  // it fell out of the world
    store_motion(row, m, body);
    move_item_cell(slot, ox, oz);
    // ItemEntity.hurtServer, for what the tick says of fire: a tick of burning takes one point of
    // its five (Entity.baseTick), each cell of lava it was in four (Entity.lavaHurt) - and with
    // none left it is gone, as it would be in the middle of vanilla's tick, the rest of which
    // does not reach it. Lava that hurts is heard (ItemEntity.shouldPlayLavaHurtSound: as it
    // burns up, and every tenth tick of its life).
    const double began[3] = {ox, oy, oz};
    if (const Burnt burnt = took_fire(row, body, began, false); UNLIKELY(burnt.burned || burnt.lava != 0)) {
        i32 health = item_health_[k] - (burnt.burned ? 1 : 0);
        const entity::Cold* cold = ents_.cold(row);
        const bool silent = cold && (cold->bits & entity::cold::kSilent);
        for (u32 i = 0; i < burnt.lava && health > 0; ++i) {
            health -= 4;
            if ((health <= 0 || tick_count % 10 == 0) && !silent)
                play_sound(gen::sound::kEntityGenericBurn, proto::gen::physics::kEntity[entity_type::kItem].sound_source,
                           m.pos[0], m.pos[1], m.pos[2], 0.4f, 2.0f + random_float() * 0.4f);
        }
        item_health_[k] = static_cast<i8>(std::max(health, -100));
        ents_.flags[row] |= entity::flag::kSyncVelocity;  // markHurt
        if (health <= 0) return remove_item(slot);
    }

    // mergeWithNeighbours: every other tick while it goes from block to block, every 40 where it lies.
    const bool moved =
        floor_int(ox) != floor_int(m.pos[0]) || floor_int(oy) != floor_int(m.pos[1]) || floor_int(oz) != floor_int(m.pos[2]);
    if (UNLIKELY(tick_count % (moved ? kMovingMergePeriod : kMergePeriod) == 0)) {
        merge_item(k);
        if (!item_dead_.empty()) {
            // What a merge emptied is discarded at once - this item itself, when it went into another.
            for (usize i = 0; i < item_dead_.size(); ++i) remove_item(item_dead_[i]);
            item_dead_.clear();
            k = item_of_slot_[slot];
            if (k == kNone) return;
        }
    }
    // age++, then "age >= 6000 -> discard": the item has had `age` ticks, this one included.
    if (UNLIKELY(entity_tick_ - item_age_from_[k] + 1 >= kLifetime)) return remove_item(slot);

    // At rest? Asked when a tick in which it tried the ground left it where it was, and - of an
    // item that then was not (one on a slime block never is) - less and less often.
    // (`row` is not to be used down here: a merge may have removed another item, and rows move.)
    Mover& mv = movers_[slot];
    const bool tried = (static_cast<u32>(static_cast<i32>(tick_count) + id) & 3) == 0;
    if (tried && mv.body.on_ground && entity_tick_ >= mv.retry && ox == m.pos[0] && oy == m.pos[1] && oz == m.pos[2]) {
        ItemSleep& rest = item_sleep_[slot];
        if (phys::item_rests(w, m, mv.body, tick_count, id, rest.rest)) {
            rest.phase = static_cast<u8>((1 - item_born_[k] + static_cast<u64>(id)) & 3);
            rest.merge_at = static_cast<u32>(entity_tick_ + (kMergePeriod - tick_count % kMergePeriod));
            rest.expires = static_cast<u32>(item_age_from_[k] + kLifetime - 1);
            sleep(slot);
        } else {
            mv.patience = static_cast<u8>(std::min<u32>(mv.patience + 1u, 6u));
            mv.retry = entity_tick_ + (4ull << mv.patience);
        }
    }
}

// ItemEntity.tick for an item at rest (see server/physics.cpp): its velocity, and the two
// things its age brings - a look around for others to merge with, and its end.
void Game::rest_item(u32 slot) {
    ItemSleep& rest = item_sleep_[slot];
    const u32 row = ents_.by_slot(slot);
    const u32 now = static_cast<u32>(entity_tick_);
    if (phys::item_rest_tick(ents_.vx[row], ents_.vy[row], ents_.vz[row], (now + rest.phase) & 3, rest.rest))
        ents_.flags[row] |= entity::flag::kNeedsSync;
    if (UNLIKELY(now == rest.merge_at)) {
        rest.merge_at += kMergePeriod;
        merge_item(item_of_slot_[slot]);
        if (!item_dead_.empty()) {
            for (usize i = 0; i < item_dead_.size(); ++i) remove_item(item_dead_[i]);
            item_dead_.clear();
            if (item_of_slot_[slot] == kNone) return;
        }
    }
    if (UNLIKELY(static_cast<i32>(now - rest.expires) >= 0))
        remove_item(slot);  // (32 bits of the clock: good for years of difference)
}

// An item in a chunk that does not tick is not ticked: its age, and what is left of the delay
// before it can be picked up, stand still with it.
void Game::freeze_item(u32 slot) {
    const u32 k = item_of_slot_[slot];
    ++item_born_[k];
    ++item_age_from_[k];
    if (item_pickup_[k] != kNever) ++item_pickup_[k];
}

// ItemEntity.mergeWithNeighbours: other mergable items within half a block sideways.
void Game::merge_item(u32 k) {
    // isMergable: alive, not at its end, and with room in its stack.
    const auto mergable = [&](u32 i, u32 row) {
        const ItemStack& s = ents_.item[row];
        return !s.empty() && item_pickup_[i] != kNever && entity_tick_ - item_age_from_[i] < kLifetime &&
               s.count() < s.max_stack();
    };
    const u32 row = ents_.by_slot(item_slot_[k]);
    if (!mergable(k, row)) return;
    Box box = item_box(ents_, row);  // getBoundingBox().inflate(0.5, 0, 0.5)
    box.min[0] -= 0.5, box.max[0] += 0.5, box.min[2] -= 0.5, box.max[2] += 0.5;
    item_scratch_.clear();
    // The cells are by where an item stands; its box reaches half its width further.
    item_grid_.for_each(item_cell_of(box.min[0] - kItemHalfWidth), item_cell_of(box.min[2] - kItemHalfWidth),
                        item_cell_of(box.max[0] + kItemHalfWidth), item_cell_of(box.max[2] + kItemHalfWidth), [&](u32 slot) {
                            const u32 other = ents_.by_slot(slot);
                            if (other != row && box.intersects(item_box(ents_, other))) item_scratch_.push_back(other);
                        });
    // Vanilla walks its entity sections here; which neighbour comes first when there are
    // several is the one thing not reproduced.
    for (const u32 other : item_scratch_) {  // rows stay put: nothing is removed before the pass ends
        const u32 j = item_of_slot_[ents_.slot[other]];
        if (!mergable(j, other)) continue;
        // tryToMerge: both must fit one stack (areMergable), then the smaller goes into the larger.
        const ItemStack& mine = ents_.item[row];
        const ItemStack& theirs = ents_.item[other];
        if (mine.count() + theirs.count() > theirs.max_stack() || !item::same_item_same_components(mine, theirs)) continue;
        const bool into_mine = theirs.count() < mine.count();
        const u32 to = into_mine ? row : other, from = into_mine ? other : row;
        const u32 to_k = into_mine ? k : j, from_k = into_mine ? j : k;
        // merge(to, from, 64). The limit is below a stack of 65..99 that is already over it, and
        // then items flow the other way: vanilla's arithmetic, kept as it is.
        const i32 to_count = static_cast<i32>(ents_.item[to].count()), from_count = static_cast<i32>(ents_.item[from].count());
        const i32 delta = std::min(std::min(static_cast<i32>(ents_.item[to].max_stack()), kMergeLimit) - to_count, from_count);
        ents_.set_item(to, ents_.item[to].with_count(static_cast<u32>(to_count + delta)));
        ents_.item[from].set_count(
            static_cast<u32>(from_count - delta));  // changed in place: viewers are not told, as in vanilla
        item_pickup_[to_k] = std::max(item_pickup_[to_k], item_pickup_[from_k]);
        item_age_from_[to_k] = std::max(item_age_from_[to_k], item_age_from_[from_k]);
        item_sleep_[ents_.slot[to]].expires =
            static_cast<u32>(item_age_from_[to_k] + kLifetime - 1);  // (if it is at rest: when it ends)
        if (ents_.item[from].empty()) item_dead_.push_back(ents_.slot[from]);
        if (ents_.item[row].empty()) return;  // this item is the one that went
    }
}

// Player.aiStep: everything that touches the player's box, grown by 1 sideways and 0.5 up and
// down, is touched - for an item that is ItemEntity.playerTouch: into the inventory with it.
void Game::pick_up_items(u32 pi) {
    const u8 pose_now = ents_.pose[pi];
    const double height = static_cast<double>(pose_now == pose::kStanding    ? kStandingHeight
                                              : pose_now == pose::kCrouching ? kCrouchingHeight
                                                                             : kLowHeight);
    const double half = static_cast<double>(kPlayerWidth / 2.0f);
    const Box area{{ents_.x[pi] - half - 1.0, ents_.y[pi] - 0.5, ents_.z[pi] - half - 1.0},
                   {ents_.x[pi] + half + 1.0, ents_.y[pi] + height + 0.5, ents_.z[pi] + half + 1.0}};
    item_scratch_.clear();
    // The cells are by where an item stands; its box reaches half its width further.
    item_grid_.for_each(item_cell_of(area.min[0] - kItemHalfWidth), item_cell_of(area.min[2] - kItemHalfWidth),
                        item_cell_of(area.max[0] + kItemHalfWidth), item_cell_of(area.max[2] + kItemHalfWidth), [&](u32 slot) {
                            if (!area.intersects(item_box(ents_, ents_.by_slot(slot)))) return;
                            if (entity_tick_ >= item_pickup_[item_of_slot_[slot]])
                                item_scratch_.push_back(slot);  // pickupDelay == 0
                        });
    if (LIKELY(item_scratch_.empty())) return;
    Player& p = players_[pi];
    for (const u32 slot : item_scratch_) {  // by slot: a removal below moves rows
        const u32 row = ents_.by_slot(slot);
        ItemStack& stack = ents_.item[row];
        const u32 count = stack.count();
        if (!may_carry(p, stack)) continue;              // a heavy stack too many: it stays where it lies
        if (!p.menu.inv.add(-1, stack, true)) continue;  // everyone is creative: what does not fit is destroyed
        // LivingEntity.take: the collect animation, with the count the stack had - a client takes
        // that many off its copy of the item and drops the entity when none are left.
        scratch_.clear();
        write_packet(scratch_, s_.compression_threshold, cb::TakeItemEntity{ents_.net_id(row), ents_.net_id(pi), count});
        tracker_.broadcast(row, {scratch_.data, scratch_.len}, out_.data());
        // ServerPlayer.take: ...and the menu is synchronised at once, not at the next tick.
        // (Of whatever menu is open: one on a chest shows the inventory too.)
        p.menu.send_changes(out(pi), p.threshold, containers(p));
        dirty_[pi] |= kEquipDirty;
        if (stack.empty()) remove_item(slot);
    }
}

}  // namespace server
