// SPDX-License-Identifier: GPL-3.0-or-later
#include "entity/tracker.hpp"

#include <immintrin.h>

#include <algorithm>
#include <cstdlib>

#include "entity/visibility.hpp"
#include "proto/frame.hpp"
#include "proto/packets.hpp"

namespace entity {

using namespace proto;
namespace ed = proto::entity_data;

namespace {

// ServerEntity constants.
constexpr double kPositionTolerance = 7.62939453125E-6;  // TOLERANCE_LEVEL_POSITION, on the squared distance
constexpr u32 kForcedPositionPeriod = 60;                // FORCED_POS_UPDATE_PERIOD, in ticks
constexpr u32 kForcedTeleportPeriod = 400;               // FORCED_TELEPORT_PERIOD, in updates
constexpr double kMotionTolerance = 1.0E-7;
// A player is created with LivingEntity.setHealth(getMaxHealth()), 20 for the max_health attribute
// default, while the synchronized default is 1: health is always among the values a viewer is
// sent. Nothing damages a (creative) player yet, so it is a constant.
constexpr float kPlayerHealth = 20.0f;
// The fixed-size values (the longest set is a player's six; a falling block's position fits in two of them); an
// item stack comes on top.
// (An armor stand that was posed has the most: three of Entity's, its flags, six rotations of 14 bytes.)
constexpr usize kDataEntriesMax = 8 * cb::kEntityDataEntryMax + 6 * 14;
constexpr i32 kDefaultFuse = 80;  // PrimedTnt.defineSynchedData: DATA_FUSE_ID starts at DEFAULT_FUSE_TIME

constexpr u32 bit_of(u32 accessor) { return 1u << accessor; }

template <class T>
void regrow(T*& p, u32 used, u32 cap) {
    T* fresh = static_cast<T*>(std::aligned_alloc(64, (static_cast<usize>(cap) * sizeof(T) + 63) & ~usize{63}));
    if (UNLIKELY(!fresh)) std::abort();
    if (used) std::memcpy(static_cast<void*>(fresh), p, static_cast<usize>(used) * sizeof(T));
    std::free(p);
    p = fresh;
}

// SectionPos.of(entity) as one comparable word.
ALWAYS_INLINE u64 section_of(double x, double y, double z) {
    return pack_section_pos(floor_int(x) >> 4, floor_int(y) >> 4, floor_int(z) >> 4);
}
// The Grid cell a section is in (a cell is a whole number of sections wide).
static_assert(Grid::kCellShift >= 4);
ALWAYS_INLINE i32 cell_x(u64 section) { return static_cast<i32>(static_cast<i64>(section) >> 42) >> (Grid::kCellShift - 4); }
ALWAYS_INLINE i32 cell_z(u64 section) {
    return static_cast<i32>(static_cast<i64>(section << 22) >> 42) >> (Grid::kCellShift - 4);
}

// ServerEntity.isFullPrecisionEncodingRequired: an item that lies against a block is not shown
// 1/4096 of a block into it - when a step of that grid would lose some of where it is along the
// block's face, it is sent as it is (entity_position_sync) instead.
ALWAYS_INLINE bool full_precision_required(u16 flags, u32 type, double x, double z, i64 dx, i64 dz) {
    if (type != entity_type::kItem || !(flags & (flag::kVerticalCollision | flag::kHorizontalCollision))) return false;
    const auto loss = [](double v) {
        return static_cast<double>(delta_units(v)) / 4096.0 - v;
    };  // VecDeltaCodec.encodingPrecisionLoss
    if ((flags & flag::kVerticalCollision) && ((dx != 0 && loss(x) != 0) || (dz != 0 && loss(z) != 0))) return true;
    return (flags & flag::kHorizontalCollision) && ((dx != 0 && loss(z) != 0) || (dz != 0 && loss(x) != 0));
}

}  // namespace

Tracker::Tracker(Store& store, i32 threshold, u32 range_percent)
    : s_(store), threshold_(threshold), range_percent_(range_percent) {
    static_assert(sizeof(Sent) == 64, "one cache line per entity");
    reserve(s_.capacity(), 64);
}

Tracker::~Tracker() {
    std::free(sent_);
    std::free(range2_);
    std::free(section_);
    std::free(cell_pos_);
    std::free(equip_);
}

// Capacities only grow, by doubling (the store's does): amortized, and never on a steady tick.
void Tracker::reserve(u32 entity_cap, u32 player_cap) {
    if (entity_cap == cap_ && player_cap == player_cap_) return;
    DCHECK(entity_cap % 64 == 0 && entity_cap >= cap_ && player_cap >= player_cap_);
    const u32 words = entity_cap / 64;
    std::vector<u64> seen(static_cast<usize>(player_cap) * words, 0);
    for (u32 p = 0; p < player_cap_; ++p) std::copy_n(row(p), words_, seen.data() + static_cast<usize>(p) * words);
    seen_ = std::move(seen);
    view2_.resize(player_cap, 0.0);
    view_.resize(player_cap, 0);
    if (entity_cap != cap_) {
        regrow(sent_, cap_, entity_cap);
        regrow(range2_, cap_, entity_cap);
        regrow(section_, cap_, entity_cap);
        regrow(cell_pos_, cap_, entity_cap);
        regrow(equip_, cap_, entity_cap);
        std::fill(range2_ + cap_, range2_ + entity_cap, -1.0);  // rows nobody is in are never visible
        std::fill(equip_ + cap_, equip_ + entity_cap, kNone);
        bundle_at_.resize(entity_cap, 0);
        bundle_len_.resize(entity_cap, 0);
        vis_.resize(words);
        off_.reserve(entity_cap + 1);
    }
    cap_ = entity_cap;
    words_ = words;
    player_cap_ = player_cap;
}

// Mirror the store's row moves: per-entity state, the entity's column in every row of `seen`,
// and - when the row that moved was a player - that player's own row.
void Tracker::apply(const Store::Moves& moved, u32 players_before) {
    for (u32 i = 0; i < moved.n; ++i) {
        const u32 from = moved.m[i].from, to = moved.m[i].to;
        sent_[to] = sent_[from];
        range2_[to] = range2_[from];
        range2_[from] = -1.0;
        section_[to] = section_[from];
        cell_pos_[to] = cell_pos_[from];
        equip_[to] = equip_[from];
        equip_[from] = kNone;
        if (from >= players_before) grid_.rename(cell_x(section_[to]), cell_z(section_[to]), cell_pos_[to], to);
        const u64 from_bit = 1ull << (from & 63), to_bit = 1ull << (to & 63);
        for (u32 p = 0; p < players_before; ++p) {
            u64* r = row(p);
            const u64 was = r[from >> 6] & from_bit;
            r[from >> 6] &= ~from_bit;
            r[to >> 6] = (r[to >> 6] & ~to_bit) | (was ? to_bit : 0);
        }
        if (from < players_before) {
            std::copy_n(row(from), words_, row(to));
            std::fill_n(row(from), words_, 0);
            view2_[to] = view2_[from];
            view_[to] = view_[from];
        }
    }
}

u32 Tracker::spawn(u32 type, bool player, Uuid uuid, double x, double y, double z, float yaw, float pitch, Store::Moves& moved) {
    const u32 players_before = s_.players();
    const u32 e = s_.create(type, player, uuid, x, y, z, yaw, pitch, moved);
    u32 player_cap = player_cap_;
    while (s_.players() > player_cap) player_cap *= 2;
    reserve(s_.capacity(), player_cap);
    apply(moved, players_before);

    // ChunkMap.addEntity + the ServerEntity constructor.
    const entity_type::Info& info = entity_type::kInfo[type];
    const i64 range =
        static_cast<i64>(range_percent_) * (info.tracking_range * 16) / 100;  // DedicatedServer.getScaledTrackingDistance
    range2_[e] = range > 0 ? static_cast<double>(range) * static_cast<double>(range) : -1.0;
    section_[e] = section_of(x, y, z);
    Sent& t = sent_[e];
    t = {};
    t.bx = x;
    t.by = y;
    t.bz = z;
    t.yaw = pack_degrees(s_.yaw[e]);
    t.pitch = pack_degrees(s_.pitch[e]);
    t.head = pack_degrees(s_.head_yaw[e]);
    t.since_step = -1;
    t.period = info.update_interval;
    t.to_next = info.update_interval ? 0 : kNever;  // tickCount 0 is a multiple of every interval
    DCHECK(equip_[e] == kNone);
    if (player) {
        std::fill_n(row(e), words_, 0);
        view2_[e] = 0.0;
        view_[e] = 0;
        s_.dirty[e] |= bit_of(ed::living_entity::kHealth);  // set in the constructor, so dirty from the start
    } else {
        cell_pos_[e] = grid_.add(cell_x(section_[e]), cell_z(section_[e]), e);
    }
    return e;
}

// A non-player entity crossed into another section: keep it in the Grid cell it stands in.
void Tracker::regrid(u32 e, u64 from, u64 to) {
    const i32 fx = cell_x(from), fz = cell_z(from), tx = cell_x(to), tz = cell_z(to);
    if (fx == tx && fz == tz) return;
    const u32 filler = grid_.remove(fx, fz, cell_pos_[e]);
    if (filler != ~0u) cell_pos_[filler] = cell_pos_[e];
    cell_pos_[e] = grid_.add(tx, tz, e);
}

void Tracker::despawn(u32 e, base::ByteBuf* const* outs, Store::Moves& moved) {
    const u32 np = s_.players();
    const cb::RemoveEntity gone{s_.net_id(e)};
    const u64 bit = 1ull << (e & 63);
    for (u32 p = 0; p < np; ++p) {  // one word per player; a despawn is rare next to a tick
        u64& w = row(p)[e >> 6];
        if (w & bit) {
            w &= ~bit;
            write_packet(*outs[p], threshold_, gone);
        }
    }
    if (e < np) {  // a player: it no longer sees anyone (its client is leaving, nothing to tell it)
        u64* r = row(e);
        for (u32 k = 0; k < words_; ++k) {
            for (u64 w = r[k]; w; w &= w - 1) --sent_[k * 64 + static_cast<u32>(std::countr_zero(w))].viewers;
            r[k] = 0;
        }
    } else {
        const u32 filler = grid_.remove(cell_x(section_[e]), cell_z(section_[e]), cell_pos_[e]);
        if (filler != ~0u) cell_pos_[filler] = cell_pos_[e];
    }
    drop_equipment(e);
    s_.destroy(e, moved);
    apply(moved, np);
}

void Tracker::set_view_distance(u32 player, u32 chunks) {
    const double blocks = static_cast<double>(chunks * 16);
    view2_[player] = blocks * blocks;
    view_[player] = static_cast<i32>(chunks * 16);
    s_.flags[player] |= flag::kMoved;  // the whole row has to be looked at again
}

// ---------------------------------------------------------------------------- synchronized data

// SynchedEntityData.getNonDefaultValues: what someone who has never seen the entity must be told.
u32 Tracker::non_default(u32 e) const {
    u32 mask = 0;
    if (s_.shared_flags[e]) mask |= bit_of(ed::entity::kSharedFlags);
    if (s_.flags[e] & flag::kNoGravity) mask |= bit_of(ed::entity::kNoGravity);
    if (s_.pose[e] != pose::kStanding) mask |= bit_of(ed::entity::kPose);
    if (const Cold* c = s_.cold(e); UNLIKELY(c != nullptr)) {  // what a tag said of it
        if (!c->name.empty()) mask |= bit_of(ed::entity::kCustomName);
        if (c->bits & cold::kNameVisible) mask |= bit_of(ed::entity::kCustomNameVisible);
        if (c->bits & cold::kSilent) mask |= bit_of(ed::entity::kSilent);
        if (c->ticks_frozen) mask |= bit_of(ed::entity::kTicksFrozen);
        if (s_.type[e] == entity_type::kArmorStand) {
            if (c->bits & (cold::kShowArms | cold::kNoBasePlate | cold::kMarker)) mask |= bit_of(ed::armor_stand::kClientFlags);
            for (u32 i = 0; i < 6; ++i)
                if (c->posed & (1u << i)) mask |= bit_of(ed::armor_stand::kHeadPose + i);
        } else if (entity_type::kInfo[s_.type[e]].mob && (c->bits & (cold::kNoAi | cold::kLeftHanded))) {
            mask |= bit_of(ed::mob::kMobFlags);
        }
    }
    if (e < s_.players()) {
        mask |= bit_of(ed::living_entity::kHealth);
        if (s_.main_hand[e] != kMainHandRight) mask |= bit_of(ed::avatar::kPlayerMainHand);
        if (s_.skin_parts[e]) mask |= bit_of(ed::avatar::kPlayerModeCustomisation);
    } else if (s_.type[e] == entity_type::kItem) {
        if (!s_.item[e].empty()) mask |= bit_of(ed::item_entity::kItem);
    } else if (s_.type[e] == entity_type::kTnt) {
        if (s_.fuse[e] != kDefaultFuse) mask |= bit_of(ed::primed_tnt::kFuse);
    } else if (s_.type[e] == entity_type::kFallingBlock) {
        if (s_.start_pos[e] != 0) mask |= bit_of(ed::falling_block::kStartPos);
    }
    return mask;
}

// Entries in accessor order, as SynchedEntityData walks its items. Accessor ids continue down
// the class hierarchy, so the ids past Entity's own mean different things for different types.
u8* Tracker::put_data(u8* p, u32 e, u32 mask) const {
    const Cold* const c = s_.cold(e);
    if (mask & bit_of(ed::entity::kSharedFlags)) p = cb::put_entity_byte(p, ed::entity::kSharedFlags, s_.shared_flags[e]);
    if (UNLIKELY(c != nullptr)) {
        if (mask & bit_of(ed::entity::kCustomName)) p = cb::put_entity_optional_component(p, ed::entity::kCustomName, c->name);
        if (mask & bit_of(ed::entity::kCustomNameVisible))
            p = cb::put_entity_bool(p, ed::entity::kCustomNameVisible, (c->bits & cold::kNameVisible) != 0);
        if (mask & bit_of(ed::entity::kSilent)) p = cb::put_entity_bool(p, ed::entity::kSilent, (c->bits & cold::kSilent) != 0);
    }
    if (mask & bit_of(ed::entity::kNoGravity))
        p = cb::put_entity_bool(p, ed::entity::kNoGravity, (s_.flags[e] & flag::kNoGravity) != 0);
    if (mask & bit_of(ed::entity::kPose)) p = cb::put_entity_varint(p, ed::entity::kPose, ed::serializer::kPose, s_.pose[e]);
    if (UNLIKELY(c != nullptr)) {
        if (mask & bit_of(ed::entity::kTicksFrozen))
            p = cb::put_entity_varint(p, ed::entity::kTicksFrozen, ed::serializer::kInt, static_cast<u32>(c->ticks_frozen));
        if (s_.type[e] == entity_type::kArmorStand) {  // ArmorStand.DATA_CLIENT_FLAGS: 4 arms, 8 no base plate, 16 marker
            if (mask & bit_of(ed::armor_stand::kClientFlags))
                p = cb::put_entity_byte(
                    p, ed::armor_stand::kClientFlags,
                    static_cast<u8>(((c->bits & cold::kShowArms) ? 4 : 0) | ((c->bits & cold::kNoBasePlate) ? 8 : 0) |
                                    ((c->bits & cold::kMarker) ? 16 : 0)));
            for (u32 i = 0; i < 6; ++i)
                if (mask & bit_of(ed::armor_stand::kHeadPose + i))
                    p = cb::put_entity_rotations(p, ed::armor_stand::kHeadPose + i, c->pose[i]);
            return p;
        }
        if (mask & bit_of(ed::mob::kMobFlags) && e >= s_.players() &&
            entity_type::kInfo[s_.type[e]].mob)  // 1 NoAI, 2 left-handed
            return cb::put_entity_byte(
                p, ed::mob::kMobFlags,
                static_cast<u8>(((c->bits & cold::kNoAi) ? 1 : 0) | ((c->bits & cold::kLeftHanded) ? 2 : 0)));
    }
    if (s_.type[e] == entity_type::kItem) {
        if (mask & bit_of(ed::item_entity::kItem)) p = cb::put_entity_item(p, ed::item_entity::kItem, item::view(s_.item[e]));
        return p;
    }
    if (s_.type[e] == entity_type::kTnt) {  // its block state is always the default one, TNT: never sent
        if (mask & bit_of(ed::primed_tnt::kFuse))
            p = cb::put_entity_varint(p, ed::primed_tnt::kFuse, ed::serializer::kInt, static_cast<u32>(s_.fuse[e]));
        return p;
    }
    if (s_.type[e] == entity_type::kFallingBlock) {
        if (mask & bit_of(ed::falling_block::kStartPos))
            p = cb::put_entity_block_pos(p, ed::falling_block::kStartPos, s_.start_pos[e]);
        return p;
    }
    if (mask & bit_of(ed::living_entity::kHealth)) p = cb::put_entity_float(p, ed::living_entity::kHealth, kPlayerHealth);
    if (mask & bit_of(ed::avatar::kPlayerMainHand))
        p = cb::put_entity_varint(p, ed::avatar::kPlayerMainHand, ed::serializer::kHumanoidArm, s_.main_hand[e]);
    if (mask & bit_of(ed::avatar::kPlayerModeCustomisation))
        p = cb::put_entity_byte(p, ed::avatar::kPlayerModeCustomisation, s_.skin_parts[e]);
    return p;
}

void Tracker::write_data(u32 e, u32 mask, base::ByteBuf& to) {
    data_.clear();
    const Cold* const c = s_.cold(e);
    u8* entries = data_.reserve(kDataEntriesMax + 2 + item_bound(item::view(s_.item[e])) + (c ? c->name.size() + 4 : 0));
    const u8* end = put_data(entries, e, mask);
    write_packet(to, threshold_, cb::SetEntityData{s_.net_id(e), {entries, static_cast<usize>(end - entries)}});
}

// ---------------------------------------------------------------------------- equipment

void Tracker::write_equipment(u32 e, const item::ItemStack* stacks, u32 slots, base::ByteBuf& to) {
    cb::EquipmentEntry entries[item::equip::kCount];
    u32 n = 0;
    for (u32 slot = 0; slot < item::equip::kCount; ++slot)
        if (slots & (1u << slot)) entries[n++] = {static_cast<u8>(slot), item::view(stacks[slot])};
    const cb::SetEquipment all{s_.net_id(e), {entries, n}};
    if (LIKELY(all.bound() + 16 <= proto::kMaxFrame)) {
        write_packet(to, threshold_, all);
    } else {  // heavy stacks, more than one frame holds: a packet a slot
        for (u32 i = 0; i < n; ++i) write_packet(to, threshold_, cb::SetEquipment{all.id, {entries + i, 1}});
    }
}

// Heavy stacks (item::kHeavyPatchBytes) are copied to everyone who looks. A tick copies so many
// bytes of them and no more: what does not fit waits for a later tick - except the first of a
// tick, which always goes, or one that is larger than the whole allowance never would.
bool Tracker::afford(u64 cost) {
    if (cost > heavy_left_ && heavy_left_ != kHeavyPerTick) return false;
    heavy_left_ -= std::min(cost, heavy_left_);
    return true;
}

void Tracker::broadcast(u32 e, std::span<const u8> framed, base::ByteBuf* const* outs) {
    if (!sent_[e].viewers) return;
    const u32 np = s_.players(), word = e >> 6;
    const u64 bit = 1ull << (e & 63);
    for (u32 p = 0; p < np; ++p)  // one word per player: sends of this kind are rare next to a tick
        if (row(p)[word] & bit) outs[p]->append(framed.data(), framed.size());
}

void Tracker::show_equipment(u32 e, const item::ItemStack* now, base::ByteBuf* const* outs) {
    namespace equip = item::equip;
    // collectEquipmentChanges: the slots whose stack is not the one viewers know (ItemStack.matches).
    u32 changed = 0;
    if (equip_[e] == kNone) {
        for (u32 slot = 0; slot < equip::kCount; ++slot) changed |= static_cast<u32>(!now[slot].empty()) << slot;
        if (!changed) return;  // shows nothing, as before
        if (!equipment_free_.empty()) {
            equip_[e] = equipment_free_.back();
            equipment_free_.pop_back();
        } else {
            equip_[e] = static_cast<u32>(equipment_.size());
            equipment_.emplace_back();
        }
    }
    Equipment& shown = equipment_[equip_[e]];
    for (u32 slot = 0; slot < equip::kCount; ++slot)
        changed |= static_cast<u32>(!item::matches(shown.slot[slot], now[slot])) << slot;
    if (!changed) return;
    {
        u64 heavy = 0;
        for (u32 slot = 0; slot < equip::kCount; ++slot)
            if (changed & (1u << slot)) heavy += item::heavy_bytes(now[slot]);
        // Not now: nothing is marked as shown, so the next call finds the same difference.
        if (UNLIKELY(heavy) && !afford(heavy * sent_[e].viewers)) return;
    }

    base::ByteBuf& packets = scratch_;
    packets.clear();
    // handleHandSwap: both hands changed, each into what the other was - viewers get an event
    // and swap the two themselves.
    constexpr u32 kHands = (1u << equip::kMainhand) | (1u << equip::kOffhand);
    if ((changed & kHands) == kHands && item::matches(now[equip::kMainhand], shown.slot[equip::kOffhand]) &&
        item::matches(now[equip::kOffhand], shown.slot[equip::kMainhand])) {
        write_packet(packets, threshold_, cb::EntityEvent{s_.net_id(e), cb::EntityEvent::kSwapHands});
        changed &= ~kHands;
        shown.slot[equip::kMainhand] = now[equip::kMainhand];
        shown.slot[equip::kOffhand] = now[equip::kOffhand];
    }
    if (changed) {  // handleEquipmentChanges
        write_equipment(e, now, changed, packets);
        for (u32 slot = 0; slot < equip::kCount; ++slot)
            if (changed & (1u << slot)) shown.slot[slot] = now[slot];
    }
    shown.heavy = 0;
    for (u32 slot = 0; slot < equip::kCount; ++slot) shown.heavy += item::heavy_bytes(shown.slot[slot]);
    broadcast(e, {packets.data, packets.len}, outs);
}

void Tracker::drop_equipment(u32 e) {
    if (equip_[e] == kNone) return;
    equipment_[equip_[e]] = {};
    equipment_free_.push_back(equip_[e]);
    equip_[e] = kNone;
}

// ---------------------------------------------------------------------------- A: visibility

// ServerEntity.addPairing: the bundle a player gets when an entity enters its view. It describes
// the entity as its viewers know it (position base, last sent rotation), so that the deltas they
// all get from now on apply to the newcomer too. Encoded once per entity and tick, however many
// players it appears to (a player joining a crowd appears to everyone at once).
void Tracker::pair(u32 w, u32 e, base::ByteBuf& out) {
    // What it wears, and - an item on the ground - the stack it is: heavy ones are paid for.
    const u64 heavy = (equip_[e] != kNone ? equipment_[equip_[e]].heavy : 0u) + item::heavy_bytes(s_.item[e]);
    if (UNLIKELY(heavy) && !afford(heavy)) {
        retry_.push_back(w);  // it appears a tick later: the viewer's row is looked at again
        return;
    }
    if (!bundle_at_[e]) {
        const Sent& t = sent_[e];
        const u32 at = bundles_.len;
        write_packet(bundles_, threshold_, cb::BundleDelimiter{});
        write_packet(
            bundles_, threshold_,
            cb::AddEntity{s_.net_id(e), s_.uuid[e], s_.type[e], t.bx, t.by, t.bz, t.mx, t.my, t.mz, t.pitch, t.yaw, t.head,
                          s_.type[e] == entity_type::kFallingBlock ? s_.block_state[e] : 0u});  // Entity.getAddEntityPacket
        if (const u32 mask = non_default(e)) write_data(e, mask, bundles_);
        if (equip_[e] != kNone) {  // sendPairingData: every slot that holds something
            const item::ItemStack* shown = equipment_[equip_[e]].slot;
            u32 slots = 0;
            for (u32 slot = 0; slot < item::equip::kCount; ++slot) slots |= static_cast<u32>(!shown[slot].empty()) << slot;
            if (slots) write_equipment(e, shown, slots, bundles_);
        }
        write_packet(bundles_, threshold_, cb::BundleDelimiter{});
        bundle_at_[e] = at + 1;
        bundle_len_[e] = bundles_.len - at;
        bundled_.push_back(e);
    }
    out.append(bundles_.data + bundle_at_[e] - 1, bundle_len_[e]);
    row(w)[e >> 6] |= 1ull << (e & 63);
    ++sent_[e].viewers;
    ++stats_.added;
}

// ServerEntity.removePairing
void Tracker::unpair(u32 w, u32 e, base::ByteBuf& out) {
    write_packet(out, threshold_, cb::RemoveEntity{s_.net_id(e)});
    row(w)[e >> 6] &= ~(1ull << (e & 63));
    --sent_[e].viewers;
    ++stats_.removed;
}

// ChunkMap.isChunkTracked, for a player that still has chunks to be sent: is the chunk the entity
// stands in one of them? (What is in range lies in the view: only a chunk that waits can fail this.)
bool Tracker::chunk_sent(u32 w, u32 e) const {
    const PendingChunks& p = pending_[w];
    const u32 dx = static_cast<u32>((floor_int(s_.x[e]) >> 4) - p.x0), dz = static_cast<u32>((floor_int(s_.z[e]) >> 4) - p.z0);
    if (dx >= p.side || dz >= p.side) return true;
    const u32 i = dz * p.side + dx;
    return !((p.bits[i >> 6] >> (i & 63)) & 1);
}

// The player moved: every entity may have entered or left its view.
void Tracker::full_row(u32 w, base::ByteBuf& out) {
    const u32 n = s_.size(), nwords = (n + 63) / 64;
    // The players - the first rows, and whatever shares their last word: one scan.
    const u32 pwords = (s_.players() + 63) / 64;
    const u32 scanned = std::min(pwords * 64, n);
    visible_words(s_.x, s_.z, range2_, pwords, s_.x[w], s_.z[w], view2_[w], vis_.data());
    vis_[w >> 6] &= ~(1ull << (w & 63));                                       // nobody is shown itself
    if (scanned == n && (n & 63)) vis_[pwords - 1] &= (1ull << (n & 63)) - 1;  // rows past the end
    if (UNLIKELY(pending_ && pending_[w].count)) {  // ...and nobody that stands in a chunk still to come
        for (u32 k = 0; k < pwords; ++k) {
            for (u64 bits = vis_[k]; bits; bits &= bits - 1) {
                const u32 e = k * 64 + static_cast<u32>(std::countr_zero(bits));
                if (!chunk_sent(w, e)) vis_[k] &= ~(1ull << (e & 63));
            }
        }
    }
    u64* seen = row(w);
    for (u32 k = 0; k < pwords; ++k) {
        const u64 diff = vis_[k] ^ seen[k];
        if (LIKELY(!diff)) continue;
        for (u64 gone = diff & seen[k]; gone; gone &= gone - 1) unpair(w, k * 64 + static_cast<u32>(std::countr_zero(gone)), out);
        for (u64 came = diff & vis_[k]; came; came &= came - 1) pair(w, k * 64 + static_cast<u32>(std::countr_zero(came)), out);
    }
    if (scanned == n) return;
    // Everything else. What it sees may have left its range...
    for (u32 k = pwords; k < nwords; ++k) {
        for (u64 bits = seen[k]; bits; bits &= bits - 1) {
            const u32 e = k * 64 + static_cast<u32>(std::countr_zero(bits));
            if (!in_range(w, e)) unpair(w, e, out);
        }
    }
    // ...and what has come into range stands in one of the cells the range touches.
    // (r is whole blocks, so floor(x +- r) = floor(x) +- r: no entity within r is outside these cells.)
    const i32 x = floor_int(s_.x[w]), z = floor_int(s_.z[w]), r = view_[w];
    grid_.for_each(Grid::cell_of(x - r), Grid::cell_of(z - r), Grid::cell_of(x + r), Grid::cell_of(z + r), [&](u32 e) {
        if (e < scanned || ((seen[e >> 6] >> (e & 63)) & 1)) return;
        if (in_range(w, e)) pair(w, e, out);
    });
}

// The player stood still: only the entities that moved can have changed for it.
void Tracker::partial_row(u32 w, base::ByteBuf& out) {
    const u64* seen = row(w);
    for (const u32 e : changed_) {
        if (e == w) continue;
        const bool visible = in_range(w, e);
        if (visible == static_cast<bool>((seen[e >> 6] >> (e & 63)) & 1)) continue;
        if (visible)
            pair(w, e, out);
        else
            unpair(w, e, out);
    }
}

// ---------------------------------------------------------------------------- B: encode

// ServerEntity.sendChanges for one entity, into the arena. Everything it writes goes to all
// viewers; the ranges recorded in self_ also go to the entity's own client
// (sendToTrackingPlayersAndSelf). Not ported because nothing can be in those states yet:
// passengers, minecarts on rails, arrows, item frames - and paths of more than one step (what
// a living entity that bounces off a slime block is sent as).
HOT void Tracker::send_changes(u32 e) {
    Sent& t = sent_[e];
    u16 f = s_.flags[e];
    const u32 dirty = s_.dirty[e];
    if (f & flag::kLiving) t.since_step = static_cast<i16>(std::min<i32>(t.since_step + 1, INT16_MAX));  // updateTracking

    // Entity.syncPosition (a bounce) brings the next update forward to this tick:
    // tickCount = updateInterval.nextInterval(tickCount). (For a living entity vanilla records
    // a step of its path instead - SteppedInterpolationTracker.updateTracking; paths of more
    // than one step are not sent here, so that bounce goes out with the next update.)
    if (UNLIKELY(f & flag::kSyncPosition)) {
        if (!(f & flag::kLiving)) {
            t.tick_count = t.period ? (t.tick_count / t.period) * t.period + t.period : 0;
            if (t.period) t.to_next = 0;
        }
        f = static_cast<u16>(f & ~flag::kSyncPosition);
        s_.flags[e] = f;
    }
    const bool due = t.to_next == 0;  // UpdateInterval.test(tickCount)
    if (t.to_next != kNever) t.to_next = due ? static_cast<u8>(t.period - 1) : static_cast<u8>(t.to_next - 1);
    if (due | ((f & flag::kNeedsSync) != 0) | (dirty != 0)) {
        const i32 id = s_.net_id(e);
        const u8 yaw = pack_degrees(s_.yaw[e]), pitch = pack_degrees(s_.pitch[e]);
        const bool rotated = yaw != t.yaw || pitch != t.pitch;
        const bool on_ground = (f & flag::kOnGround) != 0;
        const double x = s_.x[e], y = s_.y[e], z = s_.z[e];
        ++t.teleport_delay;
        // InterpolationTracker.getPositionPath: a living entity reports one step that took the
        // ticks since its last update; before its first update, and for everything else, a point.
        u32 ticks = 0;
        if (f & flag::kLiving) {
            ticks = t.since_step > 0 ? static_cast<u32>(t.since_step) : 0;
            t.since_step = 0;
        }
        const double ox = x - t.bx, oy = y - t.by, oz = z - t.bz;
        const bool send_position = ox * ox + oy * oy + oz * oz >= kPositionTolerance || t.tick_count % kForcedPositionPeriod == 0;

        // createMovePacket decides; the velocity goes out in front of whatever it decided.
        enum { kNothing, kPos, kPosRot, kRot, kSync } kind = kNothing;
        i64 dx = 0, dy = 0, dz = 0;
        if (t.teleport_delay > kForcedTeleportPeriod || t.was_on_ground != on_ground || (f & flag::kPrecise)) {
            t.was_on_ground = on_ground;
            t.teleport_delay = 0;
            kind = kSync;
        } else if (send_position) {
            dx = delta_units(x) - delta_units(t.bx);
            dy = delta_units(y) - delta_units(t.by);
            dz = delta_units(z) - delta_units(t.bz);
            kind = !delta_fits(dx, dy, dz) || full_precision_required(f, s_.type[e], x, z, dx, dz) ? kSync
                   : rotated                                                                       ? kPosRot
                                                                                                   : kPos;
        } else if (rotated) {
            kind = kRot;
        }
        if (f & (flag::kNeedsSync | flag::kTrackDeltas)) {
            const double vx = s_.vx[e], vy = s_.vy[e], vz = s_.vz[e];
            const double ax = t.mx - vx, ay = t.my - vy, az = t.mz - vz;
            const double diff = ax * ax + ay * ay + az * az;
            if (diff > kMotionTolerance || (diff > 0 && vx * vx + vy * vy + vz * vz == 0)) {
                t.mx = vx;
                t.my = vy;
                t.mz = vz;
                write_packet(arena_, threshold_, cb::SetEntityMotion{id, vx, vy, vz});
            }
        }
        switch (kind) {
            case kPos:
                write_packet(
                    arena_, threshold_,
                    cb::MoveEntityPos{id, on_ground, ticks, static_cast<i16>(dx), static_cast<i16>(dy), static_cast<i16>(dz)});
                break;
            case kPosRot:
                write_packet(arena_, threshold_,
                             cb::MoveEntityPosRot{id, on_ground, ticks, static_cast<i16>(dx), static_cast<i16>(dy),
                                                  static_cast<i16>(dz), yaw, pitch});
                break;
            case kRot: write_packet(arena_, threshold_, cb::MoveEntityRot{id, on_ground, yaw, pitch}); break;
            case kSync:
                write_packet(arena_, threshold_, cb::EntityPositionSync{id, ticks, x, y, z, s_.yaw[e], s_.pitch[e], on_ground});
                break;
            case kNothing: break;
        }
        if (dirty) {  // sendDirtyEntityData
            const u32 at = arena_.len;
            write_data(e, dirty, arena_);
            s_.dirty[e] = 0;
            if (e < s_.players()) self_.push_back({e, at, arena_.len - at});
        }
        if (kind == kPos || kind == kPosRot || kind == kSync) {
            t.bx = x;
            t.by = y;
            t.bz = z;
        }
        if (kind == kPosRot || kind == kRot || kind == kSync) {
            t.yaw = yaw;
            t.pitch = pitch;
        }
        const u8 head = pack_degrees(s_.head_yaw[e]);
        if (head != t.head) {
            t.head = head;
            write_packet(arena_, threshold_, cb::RotateHead{id, head});
        }
        s_.flags[e] = static_cast<u16>(f & ~flag::kNeedsSync);
    }
    ++t.tick_count;
    if (UNLIKELY(f & flag::kSyncVelocity)) {
        s_.flags[e] &= static_cast<u16>(~flag::kSyncVelocity);
        const u32 at = arena_.len;
        write_packet(arena_, threshold_, cb::SetEntityMotion{s_.net_id(e), s_.vx[e], s_.vy[e], s_.vz[e]});
        if (e < s_.players()) self_.push_back({e, at, arena_.len - at});
    }
}

// ---------------------------------------------------------------------------- C: fan out

// Copy the packets of every entity whose bit is set. The arena is in entity order, so a run of
// set bits [a, b) is one contiguous range of it: a player that sees everyone costs two copies,
// not one per entity. Short ranges (one entity's move is 11..20 bytes) are copied as one fixed
// 32-byte block: both buffers have that much slack, and it avoids a call into memcpy.
HOT void Tracker::fan_out(const u64* bits, u32 n, base::ByteBuf& out) {
    const u32* off = off_.data();
    const u8* arena = arena_.data;
    const auto copy = [&](u32 a, u32 b) {
        const u32 len = off[b] - off[a];
        if (!len) return;
        u8* p = out.reserve(len);
        if (len <= 32)
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(p),
                                _mm256_loadu_si256(reinterpret_cast<const __m256i*>(arena + off[a])));
        else
            std::memcpy(p, arena + off[a], len);
        out.len += len;
        stats_.fanout_bytes += len;
    };
    const u32 nwords = (n + 63) / 64;
    u32 start = kNone;  // first entity of the open run
    for (u32 k = 0; k < nwords; ++k) {
        const u64 w = bits[k];
        if (start == kNone ? w == 0 : w == ~0ull) continue;  // the run state does not change in this word
        for (u32 pos = 0; pos < 64;) {
            if (start == kNone) {
                const u64 rest = w >> pos;
                if (!rest) break;
                pos += static_cast<u32>(std::countr_zero(rest));
                start = k * 64 + pos;
            } else {
                const u64 rest = ~w >> pos;  // zeros shifted in from the top end the search, not the run
                if (!rest) break;
                pos += static_cast<u32>(std::countr_zero(rest));
                copy(start, k * 64 + pos);
                start = kNone;
            }
        }
    }
    if (start != kNone) copy(start, n);  // bits past n are never set, so this is a run up to the last entity
}

// ---------------------------------------------------------------------------- tick

void Tracker::tick(base::ByteBuf* const* outs) {
    const u32 n = s_.size(), np = s_.players();
    stats_ = {};
    heavy_left_ = kHeavyPerTick;

    // A. Which pairs have to be looked at: ChunkMap.tick re-evaluates an entity when its section
    // changed, ChunkMap.move when a player sent a movement packet; both are in kMoved or found here.
    changed_.clear();
    for (u32 e = 0; e < n; ++e) {
        const u64 sec = section_of(s_.x[e], s_.y[e], s_.z[e]);
        if (UNLIKELY((sec != section_[e]) | ((s_.flags[e] & flag::kMoved) != 0))) {
            if (e >= np && sec != section_[e]) regrid(e, section_[e], sec);
            section_[e] = sec;
            changed_.push_back(e);
        }
    }
    if (!changed_.empty()) {
        for (u32 w = 0; w < np; ++w) {
            if (s_.flags[w] & flag::kMoved)
                full_row(w, *outs[w]);
            else
                partial_row(w, *outs[w]);
        }
        for (const u32 e : changed_) s_.flags[e] &= static_cast<u16>(~flag::kMoved);
        for (const u32 w : retry_) s_.flags[w] |= flag::kMoved;
        retry_.clear();
        for (const u32 e : bundled_) bundle_at_[e] = 0;
        bundled_.clear();
        bundles_.clear();
    }

    // B. Every entity says what changed, once.
    arena_.clear();
    self_.clear();
    off_.resize(n + 1);
    for (u32 e = 0; e < n; ++e) {
        off_[e] = arena_.len;
        send_changes(e);
        stats_.updated += arena_.len != off_[e];
    }
    off_[n] = arena_.len;
    arena_.reserve(32);  // fan_out reads fixed 32-byte blocks: keep that much readable past the end

    // C. ...and every player gets the bytes of what it sees.
    if (arena_.len) {
        for (u32 w = 0; w < np; ++w) fan_out(row(w), n, *outs[w]);
        for (const Self& self : self_) outs[self.player]->append(arena_.data + self.off, self.len);
    }
}

}  // namespace entity
