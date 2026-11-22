// SPDX-License-Identifier: GPL-3.0-or-later
// Tracker: who has been shown which entity, and the one pass per tick that tells them what moved.
// It is the port of vanilla's ChunkMap.TrackedEntity + ServerEntity (26.3), reshaped around two
// facts: every viewer of an entity is sent the SAME bytes, and viewers are few and contiguous.
//
// State
//   seen[player][entity]   one bit per pair, a row of u64 words per player, indexed by DENSE entity
//                          index. "Player p has been sent add_entity for e and no remove since."
//   Sent (per entity)      what viewers were last told: position base, packed rotation, counters -
//                          ServerEntity's fields, 64 bytes.
//
// tick() - once per tick, after packets and block changes, before the per-connection tick:
//   A. visibility   Only pairs that can have changed are looked at: an entity whose section
//                   changed, a player that sent a movement packet (vanilla re-evaluates on exactly
//                   those two events, ChunkMap.tick / ChunkMap.move), anything new. A player that
//                   moved gets its whole row redone, in two parts:
//                     - against the other players: a SIMD scan over the position columns (players
//                       are the first rows), 64 answers per word, XORed with the row;
//                     - against everything else: the entities it sees are re-tested, and so are
//                       those in the Grid cells its range touches. (One scan over all entities was
//                       the first version: 10 us per player and tick at 20 000 entities.)
//                   Other players only test the entities that moved. Changes append add_entity
//                   bundles / remove_entities to the player's output.
//   B. encode       ServerEntity.sendChanges for every entity, ONCE, into an arena: the packets of
//                   entity e are the bytes [off[e], off[e+1]), in dense order.
//   C. fan out      For each player, each RUN of consecutive set bits [a, b) in its row is one
//                   memcpy of arena[off[a], off[b]) into its output. A player in a crowd sees
//                   everyone, so its row is two runs and its whole update two memcpys.
//
// What reaches the wire is vanilla's, with one deliberate difference: vanilla re-evaluates a pair
// the moment a movement packet is handled, we do it once at the tick's end with the positions
// everyone ended up at. A pair that flickered within one tick is not announced twice.
//
// Tick thread only.
#pragma once

#include <vector>

#include <span>

#include "base/buf.hpp"
#include "entity/grid.hpp"
#include "entity/store.hpp"
#include "item/stack.hpp"

namespace entity {

// What a player has not been sent yet of the chunks its view holds
// (PlayerChunkSender.pendingChunks): a bit per chunk of the window [x0, x0 + side) x
// [z0, z0 + side), row by row. An entity that stands in such a chunk is not shown to that player
// until the chunk has gone out (ChunkMap.isChunkTracked).
struct PendingChunks {
    const u64* bits = nullptr;
    i32 x0 = 0, z0 = 0;
    u32 side = 0;
    u32 count = 0;  // bits set: 0 for nearly every player nearly always, and then nothing is tested
};

class Tracker {
  public:
    struct Stats {                   // of the last tick()
        u32 added = 0, removed = 0;  // pairs that started / stopped being visible
        u32 updated = 0;             // entities that had something to say
        u64 fanout_bytes = 0;        // bytes copied to players in step C
    };

    // `threshold`: the compression threshold all connections use - frames are shared between
    // viewers, so there can only be one. `range_percent`: entity-broadcast-range-percentage.
    Tracker(Store& store, i32 threshold, u32 range_percent = 100);
    ~Tracker();
    Tracker(const Tracker&) = delete;
    Tracker& operator=(const Tracker&) = delete;

    // ---- lifecycle. These wrap Store::create / destroy and keep the tracker's rows in step;
    // `moved` reports the row moves for the caller's own arrays. outs[p] is the output buffer of
    // player row p, for every p < players(), here and in tick().
    u32 spawn(u32 type, bool player, Uuid uuid, double x, double y, double z, float yaw, float pitch, Store::Moves& moved);
    // Tells everyone who sees the entity that it is gone (ChunkMap.removeEntity), then frees it.
    void despawn(u32 ei, base::ByteBuf* const* outs, Store::Moves& moved);

    // An entity that is created moving, right after spawn(): the ServerEntity constructor takes
    // its velocity as the one viewers know - it is in the add_entity they get.
    void created_with_velocity(u32 e) {
        sent_[e].mx = s_.vx[e];
        sent_[e].my = s_.vy[e];
        sent_[e].mz = s_.vz[e];
    }

    // ChunkMap.getPlayerViewDistance: a player is shown nothing further than its view distance.
    void set_view_distance(u32 player, u32 chunks);

    // One entry per player row, read by tick(); null: every chunk counts as sent.
    void set_pending_chunks(const PendingChunks* per_player) { pending_ = per_player; }

    void tick(base::ByteBuf* const* outs);

    // LivingEntity.detectEquipmentUpdates: `now` is what entity `e` holds and wears, one stack
    // per EquipmentSlot. Whoever sees it is told about the slots that differ from what they were
    // told last (set_equipment; a plain swap of the hands is entity event 55 instead), and
    // whoever comes to see it later is shown all of it with the entity (ServerEntity.sendPairingData).
    void show_equipment(u32 e, const item::ItemStack* now, base::ByteBuf* const* outs);

    // ServerChunkCache.sendToTrackingPlayers: `framed` packets to everyone who sees entity `e`
    // (not to `e` itself, if it is a player).
    void broadcast(u32 e, std::span<const u8> framed, base::ByteBuf* const* outs);

    bool sees(u32 player, u32 ei) const { return (row(player)[ei >> 6] >> (ei & 63)) & 1; }
    u32 viewers(u32 ei) const { return sent_[ei].viewers; }
    const Stats& stats() const { return stats_; }

  private:
    // ServerEntity, minus the pointers. Read and written together, one entity at a time: a struct.
    struct Sent {
        double bx, by, bz;    // VecDeltaCodec base: where viewers believe the entity is
        double mx, my, mz;    // lastSentMovement
        u32 tick_count;       // sendChanges calls so far
        u16 teleport_delay;   // updates since the last absolute position
        u16 viewers;          // set bits in this entity's column of `seen`
        i16 since_step;       // SteppedInterpolationTracker.ticksSinceLastStep (living entities)
        u8 yaw, pitch, head;  // lastSentYRot / XRot / YHeadRot, packed degrees
        u8 to_next;           // ticks until the update interval fires; kNever = it never does
        u8 period;            // the update interval
        bool was_on_ground;
    };
    static constexpr u8 kNever = 0xFF;

    u64* row(u32 player) { return seen_.data() + static_cast<usize>(player) * words_; }
    const u64* row(u32 player) const { return seen_.data() + static_cast<usize>(player) * words_; }

    void reserve(u32 entity_cap, u32 player_cap);
    void apply(const Store::Moves& moved, u32 players_before);
    void full_row(u32 w, base::ByteBuf& out);
    bool in_range(u32 w, u32 e) const {  // ChunkMap.TrackedEntity.updatePlayer's test, for one pair
        const double dx = s_.x[w] - s_.x[e], dz = s_.z[w] - s_.z[e];
        if (dx * dx + dz * dz > std::min(range2_[e], view2_[w])) return false;
        return LIKELY(!pending_ || pending_[w].count == 0) || chunk_sent(w, e);
    }
    bool chunk_sent(u32 w, u32 e) const;
    void regrid(u32 e, u64 from_section, u64 to_section);
    void partial_row(u32 w, base::ByteBuf& out);
    void pair(u32 w, u32 e, base::ByteBuf& out);
    void unpair(u32 w, u32 e, base::ByteBuf& out);
    void send_changes(u32 e);
    void fan_out(const u64* bits, u32 n, base::ByteBuf& out);
    u32 non_default(u32 e) const;
    u8* put_data(u8* p, u32 e, u32 mask) const;
    void write_data(u32 e, u32 mask, base::ByteBuf& to);
    void write_equipment(u32 e, const item::ItemStack* stacks, u32 slots, base::ByteBuf& to);
    void drop_equipment(u32 e);

    Store& s_;
    const PendingChunks* pending_ = nullptr;
    i32 threshold_;
    u32 range_percent_;

    u32 cap_ = 0;         // entity rows allocated (the store's capacity)
    u32 words_ = 0;       // cap_ / 64: u64 words per row of `seen`
    u32 player_cap_ = 0;  // rows of `seen` allocated
    Sent* sent_ = nullptr;
    double* range2_ = nullptr;  // per entity: (how far it is shown, in blocks)^2; negative = never shown
    u64* section_ = nullptr;    // per entity: the section it was in when visibility was last evaluated
    u32* cell_pos_ = nullptr;   // per entity that is not a player: its position in its Grid cell
    // What viewers were last told an entity holds and wears (LivingEntity.lastEquipmentItems).
    // Few entities have any: one record per entity that ever showed something, found through
    // an index per row.
    struct Equipment {
        item::ItemStack slot[item::equip::kCount];
        u32 heavy = 0;  // bytes of the heavy stacks among them (item::heavy_bytes)
    };
    // What a tick may copy of heavy stacks to those who look, and what is left of it.
    static constexpr u64 kHeavyPerTick = 8u << 20;
    u64 heavy_left_ = kHeavyPerTick;
    std::vector<u32> retry_;  // viewers a pairing was put off for: their rows are looked at again next tick
    bool afford(u64 cost);
    u32* equip_ = nullptr;  // per entity: its record in equipment_, kNone = shows nothing
    std::vector<Equipment> equipment_;
    std::vector<u32> equipment_free_;
    Grid grid_;                  // the entities that are not players, by where they are
    std::vector<double> view2_;  // per player: (view distance in blocks)^2
    std::vector<i32> view_;      // per player: view distance in blocks
    std::vector<u64> seen_;      // [player_cap_][words_]

    base::ByteBuf data_;     // scratch: the entries of one set_entity_data
    base::ByteBuf scratch_;  // scratch: packets on their way to an entity's viewers
    base::ByteBuf arena_;    // step B: this tick's packets, entity after entity
    std::vector<u32> off_;   // entity e's packets are arena_[off_[e], off_[e + 1])
    struct Self {
        u32 player, off, len;
    };
    std::vector<Self> self_;      // packets that also go to the entity's own client
    base::ByteBuf bundles_;       // pairing bundles encoded this tick, each once
    std::vector<u32> bundle_at_;  // per entity: offset + 1 in bundles_, 0 = not encoded this tick
    std::vector<u32> bundle_len_;
    std::vector<u32> bundled_;  // entities with a bundle_at_ to reset
    std::vector<u32> changed_;  // step A: entities to re-evaluate
    std::vector<u64> vis_;      // step A: one freshly computed row
    Stats stats_;
};

}  // namespace entity
