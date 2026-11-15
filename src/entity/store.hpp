// Store: every entity of a dimension - players, mobs, items, whatever comes - in ONE table of
// parallel arrays (SoA) indexed by a dense entity index.
//
// Why one table and not "players here, mobs there": the passes that dominate the tick treat an
// entity as a point that moves and has to be shown to whoever is near - they do not care what it
// is. One table means one tracker pass, one id space, one handle type and one set of columns for
// plugins. What differs between kinds lives beside the table, keyed by the same index:
//
//   * players are the rows [0, players()): the PLAYER PREFIX. A player's entity index is its
//     player index, so the game's per-player columns (connection, view, inventory) line up with
//     these without a lookup, and "all viewers" is a contiguous range for the visibility scan;
//   * what only some kinds have (AI state, an inventory) belongs in side tables keyed by slot,
//     iterated on their own - not in columns every row pays for. (`item` is a column all the
//     same: an item entity, a thrown potion, an item frame, a display each show one stack, and
//     the tracker reads it as it reads the other synchronized values.)
//
// Rows move: removal fills the hole with the last row (twice for a player, to keep the prefix
// dense), so a dense index is valid only until the next create/destroy. Everything that outlives
// that - packets on the wire, plugins, references between entities - uses the SLOT: a stable
// index that is reused only after its entity is gone, paired with a generation in a Handle.
// The network entity id is slot + 1: ids stay small (one or two VarInt bytes in every movement
// packet, for the whole uptime) and "which entity is id N" is an array lookup.
//
// Tick thread only.
#pragma once

#include <vector>

#include "base/types.hpp"
#include "item/stack.hpp"
#include "proto/gen/current.hpp"

#include PROTO_GEN_ENTITIES_HPP

namespace entity {

inline constexpr u32 kNone = ~0u;

// (slot, generation) in one integer that survives a trip through a Lua number (53 bits):
// slot in the low 24 bits, generation in the 29 above. 0 is never a live handle.
using Handle = u64;
inline constexpr u32 kSlotBits = 24;
inline constexpr u32 kMaxEntities = (1u << kSlotBits) - 1;

namespace flag {                              // Store::flags
inline constexpr u16 kOnGround = 1 << 0;      // Entity.onGround
inline constexpr u16 kNeedsSync = 1 << 1;     // Entity.needsSync: got an impulse, viewers hear its velocity at once
inline constexpr u16 kSyncVelocity = 1 << 2;  // Entity.syncVelocity: the same, and its own client hears it too
inline constexpr u16 kMoved = 1 << 3;         // who-sees-whom must be re-evaluated for it (set on creation and on
                                              // every movement packet of a player; cleared by the tracker)
inline constexpr u16 kLiving = 1 << 4;        // LivingEntity: movement is sent as stepped paths
inline constexpr u16 kTrackDeltas = 1 << 5;   // EntityType.trackDeltas: velocity is sent with movement
inline constexpr u16 kNoGravity = 1 << 6;     // Entity.DATA_NO_GRAVITY, synchronized: write it with set_no_gravity()
inline constexpr u16 kPrecise = 1 << 7;       // Entity.requiresPrecisePosition: every update is an absolute position
                                              // (a primed TNT that touches a block)
}  // namespace flag

// Entity.DATA_SHARED_FLAGS_ID bits (Entity.FLAG_*).
namespace shared_flag {
inline constexpr u32 kOnFire = 0, kShiftKeyDown = 1, kSprinting = 3, kSwimming = 4, kInvisible = 5, kGlowing = 6, kFallFlying = 7;
}  // namespace shared_flag

inline constexpr u8 kMainHandLeft = 0, kMainHandRight = 1;  // HumanoidArm ids; right is Avatar.DEFAULT_MAIN_HAND

// The columns as plain C data: what the plugin API will hand to LuaJIT FFI, so that a script
// loops over `x[i]` without a call per entity. Valid until the next create/destroy.
struct Columns {
    u32 count;    // rows in use
    u32 players;  // rows [0, players) are players
    const double *x, *y, *z;
    const double *vx, *vy, *vz;
    const float *yaw, *pitch, *head_yaw;
    const u16* flags;
    const u16* type;
    const u32* slot;
};

class Store {
  public:
    // Rows that changed their dense index during a create/destroy, for whoever keeps arrays in step.
    struct Moves {
        struct Move {
            u32 from, to;
        } m[2];
        u32 n = 0;
    };

    Store();
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    u32 size() const { return n_; }
    u32 players() const { return players_; }
    u32 capacity() const { return cap_; }  // a multiple of 64; rows past size() are zeroed

    // Adds a row and returns its index: players() - 1 for a player (the row that was there, if
    // any, moves to the end and is reported), size() - 1 for anything else.
    u32 create(u32 type, bool player, Uuid id, double px, double py, double pz, float yaw_deg, float pitch_deg, Moves& moved);
    void destroy(u32 ei, Moves& moved);

    // ---- identity
    Handle handle(u32 ei) const { return (static_cast<u64>(slot_gen_[slot[ei]]) << kSlotBits) | slot[ei]; }
    // The row a handle refers to, kNone if that entity is gone.
    u32 resolve(Handle h) const {
        const u32 s = static_cast<u32>(h) & kMaxEntities;
        return s < slot_dense_.size() && slot_gen_[s] == static_cast<u32>(h >> kSlotBits) ? slot_dense_[s] : kNone;
    }
    i32 net_id(u32 ei) const { return static_cast<i32>(slot[ei]) + 1; }
    u32 by_net_id(i32 id) const { return by_slot(static_cast<u32>(id) - 1); }
    // The row of the entity in slot `s`, kNone if the slot is free. For side tables keyed by slot.
    u32 by_slot(u32 s) const { return s < slot_dense_.size() ? slot_dense_[s] : kNone; }
    u32 slot_count() const { return static_cast<u32>(slot_dense_.size()); }  // slots ever handed out

    // ---- synchronized data (SynchedEntityData): a write that changes the value marks it dirty,
    // and the tracker sends dirty values to everyone who sees the entity.
    void set_shared_flag(u32 ei, u32 bit, bool on) {
        const u8 v = static_cast<u8>(on ? shared_flags[ei] | (1u << bit) : shared_flags[ei] & ~(1u << bit));
        if (v != shared_flags[ei]) {
            shared_flags[ei] = v;
            dirty[ei] |= 1u << proto::gen::entity_data::entity::kSharedFlags;
        }
    }
    void set_pose(u32 ei, u8 v) {
        if (v != pose[ei]) {
            pose[ei] = v;
            dirty[ei] |= 1u << proto::gen::entity_data::entity::kPose;
        }
    }
    void set_skin_parts(u32 ei, u8 v) {
        if (v != skin_parts[ei]) {
            skin_parts[ei] = v;
            dirty[ei] |= 1u << proto::gen::entity_data::avatar::kPlayerModeCustomisation;
        }
    }
    void set_main_hand(u32 ei, u8 v) {
        if (v != main_hand[ei]) {
            main_hand[ei] = v;
            dirty[ei] |= 1u << proto::gen::entity_data::avatar::kPlayerMainHand;
        }
    }
    void set_no_gravity(u32 ei, bool on) {
        if (on != ((flags[ei] & flag::kNoGravity) != 0)) {
            flags[ei] ^= flag::kNoGravity;
            dirty[ei] |= 1u << proto::gen::entity_data::entity::kNoGravity;
        }
    }
    // PrimedTnt.setFuse: ticks until it explodes.
    void set_fuse(u32 ei, i32 v) {
        if (v != fuse[ei]) {
            fuse[ei] = v;
            dirty[ei] |= 1u << proto::gen::entity_data::primed_tnt::kFuse;
        }
    }
    // The stack an item entity is (ItemEntity.setItem). Always dirty: vanilla compares the
    // objects, and a stack that was set is never the one that was there.
    void set_item(u32 ei, item::ItemStack v) {
        item[ei] = std::move(v);
        dirty[ei] |= 1u << proto::gen::entity_data::item_entity::kItem;
    }

    Columns columns() const { return {n_, players_, x, y, z, vx, vy, vz, yaw, pitch, head_yaw, flags, type, slot}; }

    // ---- columns, index = dense entity index. Read freely; write positions and rotations
    // directly, synchronized data through the setters above.
    double *x = nullptr, *y = nullptr, *z = nullptr;     // Entity.position
    double *vx = nullptr, *vy = nullptr, *vz = nullptr;  // Entity.deltaMovement, blocks per tick
    float *yaw = nullptr, *pitch = nullptr;              // yRot / xRot, degrees
    float* head_yaw = nullptr;                           // LivingEntity.yHeadRot
    u16* flags = nullptr;                                // flag::*
    u16* type = nullptr;                                 // entity_type registry id
    u32* slot = nullptr;                                 // stable id (see top)
    u32* dirty = nullptr;                                // synchronized data to send: bit = accessor id
    u8* shared_flags = nullptr;                          // Entity.DATA_SHARED_FLAGS_ID
    u8* pose = nullptr;                                  // Entity.DATA_POSE
    u8* skin_parts = nullptr;                            // Avatar.DATA_PLAYER_MODE_CUSTOMISATION (players)
    u8* main_hand = nullptr;                             // Avatar.DATA_PLAYER_MAIN_HAND (players)
    item::ItemStack* item = nullptr;                     // ItemEntity.DATA_ITEM (item entities); change the count in place
                                                         // where vanilla does, anything else through set_item()
    i32* fuse = nullptr;                                 // PrimedTnt.DATA_FUSE_ID (primed TNT): 4 bytes a row for one kind,
                                                         // against a lookup per TNT and tick in the tracker's encode pass
    Uuid* uuid = nullptr;                                // cold: only read when the entity is shown to someone

  private:
    template <class F>
    void each_column(F&& f) {
        f(x), f(y), f(z), f(vx), f(vy), f(vz), f(yaw), f(pitch), f(head_yaw);
        f(flags), f(type), f(slot), f(dirty), f(shared_flags), f(pose), f(skin_parts), f(main_hand), f(item), f(fuse), f(uuid);
    }
    void grow(u32 cap);
    void move_row(u32 from, u32 to, Moves& moved);
    void clear_row(u32 i);

    u32 n_ = 0, players_ = 0, cap_ = 0;
    std::vector<u32> slot_dense_;  // slot -> dense index, kNone while free
    std::vector<u32> slot_gen_;    // slot -> generation, bumped when the slot is freed
    std::vector<u32> free_slots_;
};

}  // namespace entity
