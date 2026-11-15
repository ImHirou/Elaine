// Item entities: stacks lying in the world.
//
// The rules are vanilla's, read from the 26.3 jar; each function names the method it follows:
//   ItemEntity (constructor, tick, mergeWithNeighbours, tryToMerge, merge, playerTouch),
//   Player.aiStep (the pickup area), LivingEntity.take, Inventory.add
//
// What is NOT here, and why an item hangs where it was dropped: physics. Vanilla throws a
// dropped item (LivingEntity.createItemStackToDrop gives it a velocity), then ItemEntity.tick
// applies gravity, moves it against the blocks, and lets water carry it. None of that exists
// yet, so an item is created at rest and stays. A client simulates items too
// (ItemEntity.getMoveSimulationType is SERVER_AND_CLIENT): told nothing, it would let the item
// fall and be pulled back up by the server's next position update, every three seconds. So the
// item says what is true of it here - Entity.DATA_NO_GRAVITY - and the client leaves it be.
// Both go away when entities get physics.
//
// An item entity is a row of the entity table (its stack is the `item` column, which the
// tracker shows as synchronized data) plus the timers below, kept beside the table by slot, and
// an entry in a grid of 2-block cells for the two questions asked every tick: which items can
// this player reach, which items lie next to this one.
#include "server/game.hpp"

namespace server {

using namespace proto;

namespace {

constexpr u64 kLifetime = 6000;   // ItemEntity.LIFETIME: age at which it is discarded
constexpr u64 kPickupDelay = 40;  // LivingEntity.createItemStackToDrop: setPickUpDelay(40)
constexpr u64 kMergePeriod = 40;  // ItemEntity.tick: an item that did not change block looks for neighbours this often
constexpr i32 kMergeLimit = 64;   // ItemEntity.merge(to, from): never beyond 64, whatever the stack allows
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

// new ItemEntity(level, x, y, z, stack) + Level.addFreshEntity, as createItemStackToDrop sets it
// up: a random yaw, 40 ticks before it can be picked up. At rest and without gravity (see top).
void Game::spawn_item(ItemStack stack, double x, double y, double z) {
    if (stack.empty() || ents_.size() >= entity::kMaxEntities) return;
    // Mth.createInsecureUUID, and the yaw the ItemEntity constructor draws: random.nextFloat() * 360.
    const Uuid uuid{(random() & ~0xF000ull) | 0x4000ull, (random() & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull};
    const float yaw = static_cast<float>(random() >> 40) / static_cast<float>(1 << 24) * 360.0f;
    entity::Store::Moves moved;
    const u32 row = tracker_.spawn(entity_type::kItem, false, uuid, x, y, z, yaw, 0, moved);
    ents_.set_item(row, std::move(stack));
    ents_.set_no_gravity(row, true);

    const u32 slot = ents_.slot[row];
    if (slot >= item_of_slot_.size()) item_of_slot_.resize(ents_.slot_count(), kNone);
    item_of_slot_[slot] = static_cast<u32>(item_slot_.size());
    item_slot_.push_back(slot);
    item_cell_.push_back(item_grid_.add(item_cell_of(x), item_cell_of(z), slot));  // nothing moves it: it stays in this cell
    item_born_.push_back(tick_);
    item_age_from_.push_back(tick_);
    // The delay counts down in the item's own tick, which runs before the players': 40 ticks
    // from one set while packets are handled, the 40th item tick has brought it to zero.
    item_pickup_.push_back(tick_ + kPickupDelay - 1);
}

// Entity.discard: whoever sees it is told at once, then the entity is gone.
void Game::remove_item(u32 slot) {
    const u32 k = item_of_slot_[slot], last = static_cast<u32>(item_slot_.size()) - 1;
    DCHECK(k != kNone);
    const u32 row = ents_.by_slot(slot);
    // Out of its cell; the cell's last item takes its place there.
    const u32 filler = item_grid_.remove(item_cell_of(ents_.x[row]), item_cell_of(ents_.z[row]), item_cell_[k]);
    if (filler != ~0u) item_cell_[item_of_slot_[filler]] = item_cell_[k];
    entity::Store::Moves moved;
    tracker_.despawn(row, out_.data(), moved);  // rows after the players move; nothing here goes by row
    if (k != last) {
        item_slot_[k] = item_slot_[last];
        item_born_[k] = item_born_[last];
        item_age_from_[k] = item_age_from_[last];
        item_pickup_[k] = item_pickup_[last];
        item_cell_[k] = item_cell_[last];
        item_of_slot_[item_slot_[k]] = k;
    }
    item_slot_.pop_back();
    item_born_.pop_back();
    item_age_from_.pop_back();
    item_pickup_.pop_back();
    item_cell_.pop_back();
    item_of_slot_[slot] = kNone;
}

// ItemEntity.tick, for every item: merge with neighbours, age, expire. Runs where vanilla ticks
// entities - after the tracker, before the players' own tick.
void Game::tick_items() {
    const u32 n = static_cast<u32>(item_slot_.size());
    if (LIKELY(n == 0)) return;
    item_dead_.clear();
    for (u32 k = 0; k < n; ++k) {
        const u32 row = ents_.by_slot(item_slot_[k]);
        if (ents_.item[row].empty()) continue;  // merged away earlier in this pass
        if ((tick_ - item_born_[k] + 1) % kMergePeriod == 0) merge_item(k);
        // age++, then "age >= 6000 -> discard": the item has had `age` ticks, this one included.
        if (tick_ - item_age_from_[k] + 1 >= kLifetime && !ents_.item[row].empty()) {
            ents_.item[row].set_count(0);
            item_dead_.push_back(item_slot_[k]);
        }
    }
    // Removal reorders the arrays, so it waits for the pass to end. Only other items could
    // tell the difference, and they skip an empty stack as they would a removed entity.
    for (const u32 slot : item_dead_) remove_item(slot);
}

// ItemEntity.mergeWithNeighbours: other mergable items within half a block sideways.
void Game::merge_item(u32 k) {
    // isMergable: alive, not at its end, and with room in its stack.
    const auto mergable = [&](u32 i, u32 row) {
        const ItemStack& s = ents_.item[row];
        return !s.empty() && tick_ - item_age_from_[i] < kLifetime && s.count() < s.max_stack();
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
                            if (tick_ >= item_pickup_[item_of_slot_[slot]]) item_scratch_.push_back(slot);  // pickupDelay == 0
                        });
    if (LIKELY(item_scratch_.empty())) return;
    Player& p = players_[pi];
    for (const u32 slot : item_scratch_) {  // by slot: a removal below moves rows
        const u32 row = ents_.by_slot(slot);
        ItemStack& stack = ents_.item[row];
        const u32 count = stack.count();
        if (!p.menu.inv.add(-1, stack, true)) continue;  // everyone is creative: what does not fit is destroyed
        // LivingEntity.take: the collect animation, with the count the stack had - a client takes
        // that many off its copy of the item and drops the entity when none are left.
        scratch_.clear();
        write_packet(scratch_, s_.compression_threshold, cb::TakeItemEntity{ents_.net_id(row), ents_.net_id(pi), count});
        tracker_.broadcast(row, {scratch_.data, scratch_.len}, out_.data());
        // ServerPlayer.take: ...and the menu is synchronised at once, not at the next tick.
        p.menu.send_changes(out(pi), p.threshold);
        dirty_[pi] |= kEquipDirty;
        if (stack.empty()) remove_item(slot);
    }
}

}  // namespace server
