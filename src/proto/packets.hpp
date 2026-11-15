// Packet layouts for the protocol in proto/gen/current.hpp (Minecraft 26.3, protocol 777).
//
// Every layout here was read off the vanilla jar's StreamCodec (see the mc-reference skill);
// the field comments name anything that differs from 1.21.x. Clientbound packets are structs
// with  kId / bound() / encode();  serverbound ones are structs with a static decode(Reader&).
// Keep this file layout-only: no game logic, no allocation in encode/decode.
#pragma once

#include <algorithm>
#include <span>
#include <string_view>

#include "proto/entity_codec.hpp"
#include "proto/gen/current.hpp"
#include "proto/wire.hpp"

#include PROTO_GEN_ENTITIES_HPP

namespace proto {

namespace entity_type = gen::entity_type;
namespace entity_data = gen::entity_data;
namespace pose = gen::pose;
namespace swing_animation = gen::swing_animation;

// SwingAnimation: how an arm swings (SwingAnimationType) and for how many ticks. Default-made
// it is SwingAnimation.DEFAULT, a whack of 6 ticks.
struct Swing {
    u8 type = swing_animation::kDefaultType;
    u8 ticks = swing_animation::kDefaultTicks;
};

struct ProfileProperty {
    std::string_view name;
    std::string_view value;
    std::string_view signature;  // empty = unsigned
};

inline usize profile_props_bound(std::span<const ProfileProperty> props) {
    usize n = kVarIntMaxBytes;
    for (const auto& p : props) n += str_bound(p.name) + str_bound(p.value) + 1 + str_bound(p.signature);
    return n;
}
inline u8* put_profile_props(u8* p, std::span<const ProfileProperty> props) {
    p = put_varint(p, static_cast<u32>(props.size()));
    for (const auto& pr : props) {
        p = put_str(p, pr.name);
        p = put_str(p, pr.value);
        p = put_bool(p, !pr.signature.empty());
        if (!pr.signature.empty()) p = put_str(p, pr.signature);
    }
    return p;
}

// A game profile exactly as ByteBufCodecs.GAME_PROFILE writes it: uuid, name, properties.
// It never changes while a player is online, so it is encoded once at join and reused.
inline usize profile_wire_bound(std::string_view name, std::span<const ProfileProperty> props) {
    return 16 + str_bound(name) + profile_props_bound(props);
}
inline u8* put_profile_wire(u8* p, Uuid uuid, std::string_view name, std::span<const ProfileProperty> props) {
    p = put_uuid(p, uuid);
    p = put_str(p, name);
    return put_profile_props(p, props);
}

// An item stack as ItemStack.OPTIONAL_STREAM_CODEC writes it for a client: the count, and for a
// count above zero the item id and the component patch (DataComponentPatch.STREAM_CODEC: both
// entry counts, the added components each as type + value, the removed types). `patch` is those
// bytes, ready-made (item::Patches keeps them); empty means "no components", two zero counts.
struct ItemView {
    u32 count = 0;
    u32 item = 0;
    std::span<const u8> patch;
};
inline usize item_bound(const ItemView& s) {
    return s.count ? varint_size(s.count) + varint_size(s.item) + std::max<usize>(s.patch.size(), 2) : 1;
}
ALWAYS_INLINE u8* put_item(u8* p, const ItemView& s) {
    if (!s.count) return put_u8(p, 0);
    p = put_varint(put_varint(p, s.count), s.item);
    if (s.patch.empty()) return put_be16(p, 0);
    return put_bytes(p, s.patch.data(), s.patch.size());
}

// ============================================================================ clientbound
namespace cb {

// ---------------------------------------------------------------- status
struct StatusResponse {
    static constexpr u32 kId = ids::status::cb::kStatusResponse;
    std::string_view json;
    usize bound() const { return str_bound(json); }
    u8* encode(u8* p) const { return put_str(p, json); }
};
struct StatusPong {
    static constexpr u32 kId = ids::status::cb::kPongResponse;
    u64 time;
    usize bound() const { return 8; }
    u8* encode(u8* p) const { return put_be64(p, time); }
};

// ---------------------------------------------------------------- login
struct LoginDisconnect {  // reason is a JSON text component here (NBT everywhere else)
    static constexpr u32 kId = ids::login::cb::kLoginDisconnect;
    std::string_view json;
    usize bound() const { return str_bound(json); }
    u8* encode(u8* p) const { return put_str(p, json); }
};
struct EncryptionRequest {
    static constexpr u32 kId = ids::login::cb::kHello;
    std::string_view server_id;
    std::span<const u8> public_key;  // DER SubjectPublicKeyInfo
    std::span<const u8> challenge;
    bool should_authenticate;
    usize bound() const { return str_bound(server_id) + 10 + public_key.size() + challenge.size() + 1; }
    u8* encode(u8* p) const {
        p = put_str(p, server_id);
        p = put_varint(p, static_cast<u32>(public_key.size()));
        p = put_bytes(p, public_key.data(), public_key.size());
        p = put_varint(p, static_cast<u32>(challenge.size()));
        p = put_bytes(p, challenge.data(), challenge.size());
        return put_bool(p, should_authenticate);
    }
};
struct LoginCompression {
    static constexpr u32 kId = ids::login::cb::kLoginCompression;
    i32 threshold;
    usize bound() const { return 5; }
    u8* encode(u8* p) const { return put_varint(p, static_cast<u32>(threshold)); }
};
struct LoginFinished {
    static constexpr u32 kId = ids::login::cb::kLoginFinished;
    Uuid uuid;
    std::string_view name;
    std::span<const ProfileProperty> props;
    Uuid session_id;  // 26.x: per-server-run id appended after the profile
    usize bound() const { return 16 + str_bound(name) + profile_props_bound(props) + 16; }
    u8* encode(u8* p) const {
        p = put_uuid(p, uuid);
        p = put_str(p, name);
        p = put_profile_props(p, props);
        return put_uuid(p, session_id);
    }
};

// ---------------------------------------------------------------- configuration
struct ConfigBrand {  // custom_payload minecraft:brand
    static constexpr u32 kId = ids::configuration::cb::kCustomPayload;
    std::string_view brand;
    usize bound() const { return 32 + str_bound(brand); }
    u8* encode(u8* p) const {
        p = put_str(p, "minecraft:brand");
        return put_str(p, brand);
    }
};
struct ConfigDisconnect {
    static constexpr u32 kId = ids::configuration::cb::kDisconnect;
    std::string_view text;
    usize bound() const { return nbt_text_bound(text); }
    u8* encode(u8* p) const { return put_nbt_text(p, text); }
};
struct ConfigKeepAlive {
    static constexpr u32 kId = ids::configuration::cb::kKeepAlive;
    u64 id;
    usize bound() const { return 8; }
    u8* encode(u8* p) const { return put_be64(p, id); }
};
struct EnabledFeatures {
    static constexpr u32 kId = ids::configuration::cb::kUpdateEnabledFeatures;
    usize bound() const { return 32; }
    u8* encode(u8* p) const {
        p = put_varint(p, 1);
        return put_str(p, "minecraft:vanilla");
    }
};
struct SelectKnownPacks {  // we only ever offer the vanilla core pack of our own version
    static constexpr u32 kId = ids::configuration::cb::kSelectKnownPacks;
    usize bound() const { return 64; }
    u8* encode(u8* p) const {
        p = put_varint(p, 1);
        p = put_str(p, "minecraft");
        p = put_str(p, "core");
        return put_str(p, gen::kVersionName);
    }
};
struct FinishConfiguration {
    static constexpr u32 kId = ids::configuration::cb::kFinishConfiguration;
    usize bound() const { return 0; }
    u8* encode(u8* p) const { return p; }
};

// ---------------------------------------------------------------- play
struct Login {
    static constexpr u32 kId = ids::play::cb::kLogin;
    i32 entity_id;
    bool hardcore;
    std::span<const std::string_view> dimensions;
    u32 max_players;
    u32 view_distance;
    u32 simulation_distance;
    bool reduced_debug_info;
    bool show_death_screen;
    bool limited_crafting;
    // CommonPlayerSpawnInfo
    u32 dimension_type;  // id in the dimension_type registry
    std::string_view dimension;
    u64 hashed_seed;
    u8 game_mode;           // 26.x: VarInt (was a byte)
    i8 previous_game_mode;  // -1 = none; 26.x: optional VarInt, encoded as value + 1
    bool is_debug;
    bool is_flat;
    u32 portal_cooldown;
    u32 sea_level;
    bool online_mode;  // 26.x: new, sits before enforces_secure_chat
    bool enforces_secure_chat;

    usize bound() const {
        usize n = 64 + str_bound(dimension);
        for (auto d : dimensions) n += str_bound(d);
        return n;
    }
    u8* encode(u8* p) const {
        p = put_be32(p, static_cast<u32>(entity_id));
        p = put_bool(p, hardcore);
        p = put_varint(p, static_cast<u32>(dimensions.size()));
        for (auto d : dimensions) p = put_str(p, d);
        p = put_varint(p, max_players);
        p = put_varint(p, view_distance);
        p = put_varint(p, simulation_distance);
        p = put_bool(p, reduced_debug_info);
        p = put_bool(p, show_death_screen);
        p = put_bool(p, limited_crafting);
        p = put_varint(p, dimension_type);
        p = put_str(p, dimension);
        p = put_be64(p, hashed_seed);
        p = put_varint(p, game_mode);
        p = put_varint(p, static_cast<u32>(previous_game_mode + 1));
        p = put_bool(p, is_debug);
        p = put_bool(p, is_flat);
        p = put_bool(p, false);  // no last death location
        p = put_varint(p, portal_cooldown);
        p = put_varint(p, sea_level);
        p = put_bool(p, online_mode);
        return put_bool(p, enforces_secure_chat);
    }
};

struct GameEvent {
    static constexpr u32 kId = ids::play::cb::kGameEvent;
    static constexpr u8 kChangeGameMode = 3;
    static constexpr u8 kLevelChunksLoadStart = 13;  // client leaves "Loading terrain" after this + chunks
    u8 event;
    float param;
    usize bound() const { return 5; }
    u8* encode(u8* p) const { return put_f32(put_u8(p, event), param); }
};

struct SetChunkCacheCenter {
    static constexpr u32 kId = ids::play::cb::kSetChunkCacheCenter;
    i32 x, z;
    usize bound() const { return 10; }
    u8* encode(u8* p) const { return put_varint(put_varint(p, static_cast<u32>(x)), static_cast<u32>(z)); }
};

struct PlayerPosition {  // server -> client teleport; client must answer with AcceptTeleportation
    static constexpr u32 kId = ids::play::cb::kPlayerPosition;
    u32 teleport_id;
    double x, y, z;
    double dx, dy, dz;
    float yaw, pitch;
    u32 relative_mask;  // bit per net.minecraft.world.entity.Relative; 0 = all absolute
    usize bound() const { return 5 + 48 + 8 + 4; }
    u8* encode(u8* p) const {
        p = put_varint(p, teleport_id);
        p = put_f64(p, x);
        p = put_f64(p, y);
        p = put_f64(p, z);
        p = put_f64(p, dx);
        p = put_f64(p, dy);
        p = put_f64(p, dz);
        p = put_f32(p, yaw);
        p = put_f32(p, pitch);
        return put_be32(p, relative_mask);
    }
};

struct PlayerAbilities {
    static constexpr u32 kId = ids::play::cb::kPlayerAbilities;
    static constexpr u8 kInvulnerable = 1, kFlying = 2, kCanFly = 4, kInstabuild = 8;
    u8 flags;
    float fly_speed = 0.05f;
    float walk_speed = 0.1f;
    usize bound() const { return 9; }
    u8* encode(u8* p) const { return put_f32(put_f32(put_u8(p, flags), fly_speed), walk_speed); }
};

struct SetDefaultSpawnPosition {
    static constexpr u32 kId = ids::play::cb::kSetDefaultSpawnPosition;
    std::string_view dimension;
    i32 x, y, z;
    float yaw, pitch;
    usize bound() const { return str_bound(dimension) + 16; }
    u8* encode(u8* p) const {
        p = put_str(p, dimension);
        p = put_be64(p, pack_block_pos(x, y, z));
        return put_f32(put_f32(p, yaw), pitch);
    }
};

struct KeepAlive {
    static constexpr u32 kId = ids::play::cb::kKeepAlive;
    u64 id;
    usize bound() const { return 8; }
    u8* encode(u8* p) const { return put_be64(p, id); }
};

struct Disconnect {
    static constexpr u32 kId = ids::play::cb::kDisconnect;
    std::string_view text;
    usize bound() const { return nbt_text_bound(text); }
    u8* encode(u8* p) const { return put_nbt_text(p, text); }
};

struct SystemChat {
    static constexpr u32 kId = ids::play::cb::kSystemChat;
    std::string_view text;
    bool overlay = false;  // true = action bar
    usize bound() const { return nbt_text_bound(text) + 1; }
    u8* encode(u8* p) const { return put_bool(put_nbt_text(p, text), overlay); }
};

// The same two packets with a text component that is more than plain text: `component` is its
// tag, ready-made (text::Text::encode, text::transcode).
struct DisconnectComponent {
    static constexpr u32 kId = ids::play::cb::kDisconnect;
    std::span<const u8> component;
    usize bound() const { return component.size(); }
    u8* encode(u8* p) const { return put_bytes(p, component.data(), component.size()); }
};
struct SystemChatComponent {
    static constexpr u32 kId = ids::play::cb::kSystemChat;
    std::span<const u8> component;
    bool overlay = false;
    usize bound() const { return component.size() + 1; }
    u8* encode(u8* p) const { return put_bool(put_bytes(p, component.data(), component.size()), overlay); }
};

struct PlayPong {  // answer to serverbound ping_request (F3 ping graph)
    static constexpr u32 kId = ids::play::cb::kPongResponse;
    u64 time;
    usize bound() const { return 8; }
    u8* encode(u8* p) const { return put_be64(p, time); }
};

struct ChunkBatchStart {
    static constexpr u32 kId = ids::play::cb::kChunkBatchStart;
    usize bound() const { return 0; }
    u8* encode(u8* p) const { return p; }
};
struct ChunkBatchFinished {
    static constexpr u32 kId = ids::play::cb::kChunkBatchFinished;
    u32 count;
    usize bound() const { return 5; }
    u8* encode(u8* p) const { return put_varint(p, count); }
};
struct ForgetLevelChunk {
    static constexpr u32 kId = ids::play::cb::kForgetLevelChunk;
    i32 x, z;
    usize bound() const { return 8; }
    u8* encode(u8* p) const {  // ChunkPos.pack(): z in the high word
        return put_be64(p, (static_cast<u64>(static_cast<u32>(z)) << 32) | static_cast<u32>(x));
    }
};

// ---- block changes (ChunkHolder.broadcastChanges): one changed block in a section goes out as
// block_update, several as one section_blocks_update.
struct BlockUpdate {
    static constexpr u32 kId = ids::play::cb::kBlockUpdate;
    i32 x, y, z;
    u32 state;  // block state id
    usize bound() const { return 8 + 5; }
    u8* encode(u8* p) const { return put_varint(put_be64(p, pack_block_pos(x, y, z)), state); }
};
struct SectionBlocksUpdate {
    static constexpr u32 kId = ids::play::cb::kSectionBlocksUpdate;
    i32 sx, sy, sz;                  // section coordinates (block >> 4)
    std::span<const u16> positions;  // inside the section: x<<8 | z<<4 | y  (SectionPos.sectionRelativePos)
    std::span<const u16> states;     // parallel to positions
    usize bound() const { return 8 + 5 + positions.size() * kVarLongMaxBytes; }
    u8* encode(u8* p) const {
        p = put_be64(p, pack_section_pos(sx, sy, sz));
        p = put_varint(p, static_cast<u32>(positions.size()));
        for (usize i = 0; i < positions.size(); ++i) p = put_varlong(p, (static_cast<u64>(states[i]) << 12) | positions[i]);
        return p;
    }
};
// Tells the client its predicted block changes up to `sequence` are settled: whatever the server
// sent for those positions is the truth. Sent once per tick (ServerGamePacketListenerImpl.tick).
struct BlockChangedAck {
    static constexpr u32 kId = ids::play::cb::kBlockChangedAck;
    i32 sequence;
    usize bound() const { return 5; }
    u8* encode(u8* p) const { return put_varint(p, static_cast<u32>(sequence)); }
};

// player_info_update carrying the actions a fresh tab-list entry needs, for any number of players.
struct PlayerInfoEntry {
    std::span<const u8> profile_wire;  // put_profile_wire() output
    u8 game_mode;
    bool listed;
    u32 latency_ms;
};
struct PlayerInfoAdd {
    static constexpr u32 kId = ids::play::cb::kPlayerInfoUpdate;
    // Action ordinals: ADD_PLAYER 0, INITIALIZE_CHAT 1, UPDATE_GAME_MODE 2, UPDATE_LISTED 3,
    // UPDATE_LATENCY 4, UPDATE_DISPLAY_NAME 5, UPDATE_LIST_ORDER 6, UPDATE_HAT 7.
    static constexpr u8 kActions = (1 << 0) | (1 << 2) | (1 << 3) | (1 << 4);
    std::span<const PlayerInfoEntry> entries;
    usize bound() const {
        usize n = 1 + 5;
        for (const auto& e : entries) n += e.profile_wire.size() + 5 + 1 + 5;
        return n;
    }
    u8* encode(u8* p) const {
        p = put_u8(p, kActions);  // EnumSet<Action> as a fixed 8-bit set
        p = put_varint(p, static_cast<u32>(entries.size()));
        for (const auto& e : entries) {  // per entry: uuid, then each action's data in ordinal order
            p = put_bytes(p, e.profile_wire.data(), e.profile_wire.size());
            p = put_varint(p, e.game_mode);
            p = put_bool(p, e.listed);
            p = put_varint(p, e.latency_ms);
        }
        return p;
    }
};
struct PlayerInfoRemove {
    static constexpr u32 kId = ids::play::cb::kPlayerInfoRemove;
    Uuid uuid;
    usize bound() const { return 21; }
    u8* encode(u8* p) const { return put_uuid(put_varint(p, 1), uuid); }
};

// ---------------------------------------------------------------- entities
// Layouts from the 26.3 jar, then compared byte for byte with what a vanilla 26.3 server sent for
// the same situations (the captured packets are the test vectors in tests/unit/wire_test.cpp).

// Both ends of a bundle: the client applies everything in between within one frame. Vanilla
// wraps what a player is told when an entity enters its view (ServerEntity.addPairing).
struct BundleDelimiter {
    static constexpr u32 kId = ids::play::cb::kBundleDelimiter;
    usize bound() const { return 0; }
    u8* encode(u8* p) const { return p; }
};

struct AddEntity {
    static constexpr u32 kId = ids::play::cb::kAddEntity;
    i32 id;
    Uuid uuid;
    u32 type;  // entity_type registry id
    double x, y, z;
    double vx, vy, vz;            // 26.x: an LpVec3 in front of the rotation (was three shorts at the end)
    u8 x_rot, y_rot, y_head_rot;  // packed degrees, pitch first
    u32 data;                     // per type; 0 for players and mobs
    usize bound() const { return 5 + 16 + 5 + 24 + kLpVec3MaxBytes + 3 + 5; }
    u8* encode(u8* p) const {
        p = put_varint(p, static_cast<u32>(id));
        p = put_uuid(p, uuid);
        p = put_varint(p, type);
        p = put_f64(p, x);
        p = put_f64(p, y);
        p = put_f64(p, z);
        p = put_lp_vec3(p, vx, vy, vz);
        p = put_u8(p, x_rot);
        p = put_u8(p, y_rot);
        p = put_u8(p, y_head_rot);
        return put_varint(p, data);
    }
};

// remove_entities carrying one id, which is how ServerEntity.removePairing sends it.
struct RemoveEntity {
    static constexpr u32 kId = ids::play::cb::kRemoveEntities;
    i32 id;
    usize bound() const { return 1 + 5; }
    u8* encode(u8* p) const { return put_varint(put_varint(p, 1), static_cast<u32>(id)); }
};

// A position change relative to what the client was last told (VecDelta), in 1/4096 block.
// 26.x: the on-ground flag became a VarInt "properties" that also counts steps; a delta is either
// one plain triple (no steps) or a path of steps, each saying how many server ticks it took. We
// only ever send a path of one step, which is what vanilla sends for every living entity.
inline constexpr usize kVecDeltaMaxBytes = 1 + 5 + 6;
ALWAYS_INLINE u8* put_vec_delta(u8* p, bool on_ground, u32 ticks, i16 dx, i16 dy, i16 dz) {
    p = put_u8(p, static_cast<u8>((on_ground ? 1 : 0) | (ticks ? 2 : 0)));  // on ground | step count << 1
    if (ticks) p = put_varint(p, ticks);
    p = put_be16(p, static_cast<u16>(dx));
    p = put_be16(p, static_cast<u16>(dy));
    return put_be16(p, static_cast<u16>(dz));
}
struct MoveEntityPos {
    static constexpr u32 kId = ids::play::cb::kMoveEntityPos;
    i32 id;
    bool on_ground;
    u32 ticks;  // 0: plain delta; otherwise one step that took this many ticks
    i16 dx, dy, dz;
    usize bound() const { return 5 + kVecDeltaMaxBytes; }
    u8* encode(u8* p) const { return put_vec_delta(put_varint(p, static_cast<u32>(id)), on_ground, ticks, dx, dy, dz); }
};
struct MoveEntityPosRot {
    static constexpr u32 kId = ids::play::cb::kMoveEntityPosRot;
    i32 id;
    bool on_ground;
    u32 ticks;
    i16 dx, dy, dz;
    u8 y_rot, x_rot;  // packed degrees, yaw first
    usize bound() const { return 5 + kVecDeltaMaxBytes + 2; }
    u8* encode(u8* p) const {
        p = put_vec_delta(put_varint(p, static_cast<u32>(id)), on_ground, ticks, dx, dy, dz);
        return put_u8(put_u8(p, y_rot), x_rot);
    }
};
struct MoveEntityRot {
    static constexpr u32 kId = ids::play::cb::kMoveEntityRot;
    i32 id;
    bool on_ground;  // 26.x: in front of the angles (was behind them)
    u8 y_rot, x_rot;
    usize bound() const { return 5 + 3; }
    u8* encode(u8* p) const { return put_u8(put_u8(put_bool(put_varint(p, static_cast<u32>(id)), on_ground), y_rot), x_rot); }
};

// The absolute form, for when a delta cannot say it: too far for shorts, the on-ground state
// flipped, or 400 updates passed. 26.x: carries a PositionPath (type, then a point or steps)
// instead of position + velocity.
struct EntityPositionSync {
    static constexpr u32 kId = ids::play::cb::kEntityPositionSync;
    i32 id;
    u32 ticks;  // 0: PositionPath.Linear; otherwise Stepped with one step of this many ticks
    double x, y, z;
    float y_rot, x_rot;  // degrees, not packed
    bool on_ground;
    usize bound() const { return 5 + 2 + 24 + 5 + 8 + 1; }
    u8* encode(u8* p) const {
        p = put_varint(p, static_cast<u32>(id));
        p = put_u8(p, ticks ? 1 : 0);  // PositionPath.Type
        if (ticks) p = put_u8(p, 1);   // number of steps
        p = put_f64(p, x);
        p = put_f64(p, y);
        p = put_f64(p, z);
        if (ticks) p = put_varint(p, ticks);
        p = put_f32(p, y_rot);
        p = put_f32(p, x_rot);
        return put_bool(p, on_ground);
    }
};

struct RotateHead {
    static constexpr u32 kId = ids::play::cb::kRotateHead;
    i32 id;
    u8 y_head_rot;
    usize bound() const { return 5 + 1; }
    u8* encode(u8* p) const { return put_u8(put_varint(p, static_cast<u32>(id)), y_head_rot); }
};

struct SetEntityMotion {
    static constexpr u32 kId = ids::play::cb::kSetEntityMotion;
    i32 id;
    double vx, vy, vz;  // 26.x: LpVec3 (was three shorts)
    usize bound() const { return 5 + kLpVec3MaxBytes; }
    u8* encode(u8* p) const { return put_lp_vec3(put_varint(p, static_cast<u32>(id)), vx, vy, vz); }
};

// set_entity_data: entries [u8 accessor][VarInt serializer][value], closed by 0xFF. The writers
// below cover the value types the server sets itself; `entries` is their concatenation.
inline constexpr usize kEntityDataEntryMax = 1 + 1 + 5;
ALWAYS_INLINE u8* put_entity_byte(u8* p, u32 accessor, u8 v) {
    p[0] = static_cast<u8>(accessor);
    p[1] = static_cast<u8>(entity_data::serializer::kByte);
    p[2] = v;
    return p + 3;
}
ALWAYS_INLINE u8* put_entity_float(u8* p, u32 accessor, float v) {
    p[0] = static_cast<u8>(accessor);
    p[1] = static_cast<u8>(entity_data::serializer::kFloat);
    return put_f32(p + 2, v);
}
// Every serializer whose value is one VarInt: INT, POSE, HUMANOID_ARM, ...
ALWAYS_INLINE u8* put_entity_varint(u8* p, u32 accessor, u32 serializer, u32 v) {
    p[0] = static_cast<u8>(accessor);
    p[1] = static_cast<u8>(serializer);
    return put_varint(p + 2, v);
}
ALWAYS_INLINE u8* put_entity_bool(u8* p, u32 accessor, bool v) {
    p[0] = static_cast<u8>(accessor);
    p[1] = static_cast<u8>(entity_data::serializer::kBoolean);
    p[2] = v;
    return p + 3;
}
// ITEM_STACK: the one value here without a fixed size - 2 + item_bound() bytes.
ALWAYS_INLINE u8* put_entity_item(u8* p, u32 accessor, const ItemView& v) {
    p[0] = static_cast<u8>(accessor);
    p[1] = static_cast<u8>(entity_data::serializer::kItemStack);
    return put_item(p + 2, v);
}
struct SetEntityData {
    static constexpr u32 kId = ids::play::cb::kSetEntityData;
    i32 id;
    std::span<const u8> entries;
    usize bound() const { return 5 + entries.size() + 1; }
    u8* encode(u8* p) const {
        p = put_varint(p, static_cast<u32>(id));
        p = put_bytes(p, entries.data(), entries.size());
        return put_u8(p, 0xFF);
    }
};

// What a living entity holds and wears, slot by slot: [slot | 0x80 while more follow][stack].
// Sent whole (the non-empty slots) when the entity enters someone's view, and for the slots
// that changed afterwards (LivingEntity.detectEquipmentUpdates).
struct EquipmentEntry {
    u8 slot;  // EquipmentSlot ordinal
    ItemView item;
};
struct SetEquipment {
    static constexpr u32 kId = ids::play::cb::kSetEquipment;
    i32 id;
    std::span<const EquipmentEntry> slots;  // at least one: the list has no length, only "more follow" bits
    usize bound() const {
        usize n = 5;
        for (const auto& e : slots) n += 1 + item_bound(e.item);
        return n;
    }
    u8* encode(u8* p) const {
        p = put_varint(p, static_cast<u32>(id));
        for (usize i = 0; i < slots.size(); ++i) {
            p = put_u8(p, static_cast<u8>(slots[i].slot | (i + 1 < slots.size() ? 0x80 : 0)));
            p = put_item(p, slots[i].item);
        }
        return p;
    }
};

// The collect animation: `item_id` flies into `collector_id`. The client then takes `amount` off
// its copy of the item entity and removes it once nothing is left (handleTakeItemEntity).
struct TakeItemEntity {
    static constexpr u32 kId = ids::play::cb::kTakeItemEntity;
    i32 item_id, collector_id;
    u32 amount;
    usize bound() const { return 15; }
    u8* encode(u8* p) const {
        return put_varint(put_varint(put_varint(p, static_cast<u32>(item_id)), static_cast<u32>(collector_id)), amount);
    }
};

// 26.x: replaces `animate` for arm swings, and carries the animation (SwingAnimation: type, ticks).
struct SwingAnimation {
    static constexpr u32 kId = ids::play::cb::kSwingAnimation;
    static constexpr u32 kMainHand = 0, kOffHand = 1;  // InteractionHand
    i32 id;
    u32 hand;
    Swing swing;
    usize bound() const { return 5 + 1 + 1 + 2; }
    u8* encode(u8* p) const {
        return put_varint(put_varint(put_varint(put_varint(p, static_cast<u32>(id)), hand), swing.type), swing.ticks);
    }
};

struct EntityEvent {
    static constexpr u32 kId = ids::play::cb::kEntityEvent;
    static constexpr u8 kSwapHands = 55;  // LivingEntity.handleHandSwap: main and off hand traded places
    i32 id;                               // an int here, not a VarInt
    u8 event;
    usize bound() const { return 5; }
    u8* encode(u8* p) const { return put_u8(put_be32(p, static_cast<u32>(id)), event); }
};

// A sound at a place. Only sounds of the registry are sent (holder id + 1; 0 would announce an
// inline sound event).
struct Sound {
    static constexpr u32 kId = ids::play::cb::kSound;
    static constexpr u32 kSourceBlocks = 4, kSourcePlayers = 7;  // SoundSource ordinals
    u32 sound;                                                   // sound_event registry id
    u32 source;
    double x, y, z;  // sent as (int)(v * 8.0)
    float volume, pitch;
    u64 seed;
    usize bound() const { return 5 + 1 + 12 + 8 + 8; }
    u8* encode(u8* p) const {
        p = put_varint(put_varint(p, sound + 1), source);
        p = put_be32(p, static_cast<u32>(java_int(x * 8.0)));
        p = put_be32(p, static_cast<u32>(java_int(y * 8.0)));
        p = put_be32(p, static_cast<u32>(java_int(z * 8.0)));
        return put_be64(put_f32(put_f32(p, volume), pitch), seed);
    }
};

// A sound that is named, not looked up (Holder.direct of SoundEvent.createVariableRangeEvent:
// what /playsound sends, whatever the name): holder id 0, then the event itself - its
// identifier and no fixed range.
struct SoundByName {
    static constexpr u32 kId = ids::play::cb::kSound;
    std::string_view sound;  // "minecraft:block.note_block.harp"
    u32 source;              // SoundSource ordinal
    double x, y, z;
    float volume, pitch;
    u64 seed;
    usize bound() const { return 1 + str_bound(sound) + 1 + 5 + 12 + 8 + 8; }
    u8* encode(u8* p) const {
        p = put_bool(put_str(put_varint(p, 0), sound), false);
        p = put_varint(p, source);
        p = put_be32(p, static_cast<u32>(java_int(x * 8.0)));
        p = put_be32(p, static_cast<u32>(java_int(y * 8.0)));
        p = put_be32(p, static_cast<u32>(java_int(z * 8.0)));
        return put_be64(put_f32(put_f32(p, volume), pitch), seed);
    }
};

// ClientboundStopSoundPacket: a byte saying what follows - bit 0 a source, bit 1 a sound.
struct StopSound {
    static constexpr u32 kId = ids::play::cb::kStopSound;
    static constexpr u32 kAnySource = ~0u;
    u32 source = kAnySource;  // SoundSource ordinal
    std::string_view sound;   // empty: every sound
    usize bound() const { return 1 + 5 + str_bound(sound); }
    u8* encode(u8* p) const {
        p = put_u8(p, static_cast<u8>((source != kAnySource ? 1 : 0) | (sound.empty() ? 0 : 2)));
        if (source != kAnySource) p = put_varint(p, source);
        return sound.empty() ? p : put_str(p, sound);
    }
};

// ---------------------------------------------------------------- inventory
// A menu is a numbered list of slots; container id 0 is the player's own inventory (InventoryMenu,
// 46 slots). `state_id` counts what the server has sent about the menu; clicks echo it back.

struct ContainerSetContent {
    static constexpr u32 kId = ids::play::cb::kContainerSetContent;
    u32 container_id;
    u32 state_id;
    std::span<const ItemView> items;
    ItemView carried;  // on the cursor
    usize bound() const {
        usize n = 5 + 5 + 5 + item_bound(carried);
        for (const auto& i : items) n += item_bound(i);
        return n;
    }
    u8* encode(u8* p) const {
        p = put_varint(put_varint(p, container_id), state_id);
        p = put_varint(p, static_cast<u32>(items.size()));
        for (const auto& i : items) p = put_item(p, i);
        return put_item(p, carried);
    }
};
struct ContainerSetSlot {
    static constexpr u32 kId = ids::play::cb::kContainerSetSlot;
    u32 container_id;
    u32 state_id;
    i16 slot;
    ItemView item;
    usize bound() const { return 5 + 5 + 2 + item_bound(item); }
    u8* encode(u8* p) const {
        p = put_varint(put_varint(p, container_id), state_id);
        return put_item(put_be16(p, static_cast<u16>(slot)), item);
    }
};
struct SetCursorItem {
    static constexpr u32 kId = ids::play::cb::kSetCursorItem;
    ItemView item;
    usize bound() const { return item_bound(item); }
    u8* encode(u8* p) const { return put_item(p, item); }
};
// One slot of the player's inventory, by Inventory index (0..8 hotbar, 9..35 the rest, 36..39
// feet to head, 40 off hand) - not by menu slot.
struct SetPlayerInventory {
    static constexpr u32 kId = ids::play::cb::kSetPlayerInventory;
    u32 slot;
    ItemView item;
    usize bound() const { return 5 + item_bound(item); }
    u8* encode(u8* p) const { return put_item(put_varint(p, slot), item); }
};
struct SetHeldSlot {
    static constexpr u32 kId = ids::play::cb::kSetHeldSlot;
    u32 slot;  // hotbar index
    usize bound() const { return 5; }
    u8* encode(u8* p) const { return put_varint(p, slot); }
};

// A menu other than the player's own opens: its id (1..100, counted by the player), its type
// (the menu registry) and the title - a text component, as the tag vanilla writes for it.
struct OpenScreen {
    static constexpr u32 kId = ids::play::cb::kOpenScreen;
    u32 container_id;
    u32 type;
    std::span<const u8> title;  // a network NBT tag (text::transcode output)
    usize bound() const { return 5 + 5 + title.size(); }
    u8* encode(u8* p) const { return put_bytes(put_varint(put_varint(p, container_id), type), title.data(), title.size()); }
};
// One of a menu's numbers (ContainerData): for a furnace, how long the fire still burns and how
// far the cooking is. Both the index and the value are shorts.
struct ContainerSetData {
    static constexpr u32 kId = ids::play::cb::kContainerSetData;
    u32 container_id;
    i16 id, value;
    usize bound() const { return 5 + 4; }
    u8* encode(u8* p) const {
        return put_be16(put_be16(put_varint(p, container_id), static_cast<u16>(id)), static_cast<u16>(value));
    }
};
// The server closes the menu the client has open (ServerPlayer.closeContainer).
struct ContainerClose {
    static constexpr u32 kId = ids::play::cb::kContainerClose;
    u32 container_id;
    usize bound() const { return 5; }
    u8* encode(u8* p) const { return put_varint(p, container_id); }
};

// ---------------------------------------------------------------- the world
// Level.blockEvent: two bytes for the block at a position, whose meaning is the block's own
// (a chest: 1, the number of players that have it open - its lid). The block is named by its
// id in the block registry, not by a state.
struct BlockEvent {
    static constexpr u32 kId = ids::play::cb::kBlockEvent;
    i32 x, y, z;
    u8 b0, b1;
    u32 block;
    usize bound() const { return 8 + 2 + 5; }
    u8* encode(u8* p) const { return put_varint(put_u8(put_u8(put_be64(p, pack_block_pos(x, y, z)), b0), b1), block); }
};

// An explosion as a client shows it: the flash, the sound, the smoke - and what it does to the
// receiving player, which is the only thing that moves it (the blocks arrive as block updates).
struct ExplosionParticle {  // Weighted<ExplosionParticleInfo>: where the cloud's particles come from
    u32 particle;           // particle_type id; only particles without options of their own
    float scaling, speed;
    u32 weight;
};
struct Explode {
    static constexpr u32 kId = ids::play::cb::kExplode;
    double x, y, z;
    float radius;
    i32 block_count;  // how many blocks it took: an int
    bool knocked;     // Optional<Vec3> playerKnockback
    double kx, ky, kz;
    u32 particle;  // ParticleOptions of the explosion itself, by id
    u32 sound;     // sound_event id
    std::span<const ExplosionParticle> block_particles;
    bool play_sound;
    usize bound() const { return 24 + 4 + 4 + 1 + 24 + 5 + 5 + 5 + block_particles.size() * (5 + 8 + 5) + 1; }
    u8* encode(u8* p) const {
        p = put_f64(put_f64(put_f64(p, x), y), z);
        p = put_be32(put_f32(p, radius), static_cast<u32>(block_count));
        p = put_bool(p, knocked);
        if (knocked) p = put_f64(put_f64(put_f64(p, kx), ky), kz);
        p = put_varint(put_varint(p, particle), sound + 1);  // a holder: id + 1
        p = put_varint(p, static_cast<u32>(block_particles.size()));
        for (const ExplosionParticle& b : block_particles)
            p = put_varint(put_f32(put_f32(put_varint(p, b.particle), b.scaling), b.speed), b.weight);
        return put_bool(p, play_sound);
    }
};

}  // namespace cb

// ============================================================================ serverbound
namespace sb {

struct Handshake {
    u32 protocol;
    std::string_view host;
    u16 port;
    u32 intent;  // 1 status, 2 login, 3 transfer
    static Handshake decode(Reader& r) {
        Handshake h;
        h.protocol = r.varint();
        h.host = r.str(255 * 3);
        h.port = r.be16();
        h.intent = r.varint();
        return h;
    }
};

struct LoginStart {
    std::string_view name;
    Uuid uuid;
    static LoginStart decode(Reader& r) {
        LoginStart s;
        s.name = r.str(16 * 3);
        s.uuid = r.uuid();
        return s;
    }
};

struct EncryptionResponse {
    std::span<const u8> secret;     // RSA-encrypted shared secret
    std::span<const u8> challenge;  // RSA-encrypted copy of our challenge
    static EncryptionResponse decode(Reader& r) {
        EncryptionResponse e;
        e.secret = r.byte_array(512);
        e.challenge = r.byte_array(512);
        return e;
    }
};

struct KnownPack {
    std::string_view ns, id, version;
};

struct ClientInformation {
    std::string_view language;
    i8 view_distance;
    u32 chat_visibility;
    bool chat_colors;
    u8 skin_parts;
    u32 main_hand;
    bool text_filtering;
    bool allows_listing;
    u32 particle_status;
    static ClientInformation decode(Reader& r) {
        ClientInformation c;
        c.language = r.str(16 * 3);
        c.view_distance = static_cast<i8>(r.u8v());
        c.chat_visibility = r.varint();
        c.chat_colors = r.boolean();
        c.skin_parts = r.u8v();
        c.main_hand = r.varint();
        c.text_filtering = r.boolean();
        c.allows_listing = r.boolean();
        c.particle_status = r.varint();
        return c;
    }
};

struct AcceptTeleportation {  // 26.x: echoes the position/rotation after the id
    u32 id;
    double x, y, z;
    float yaw, pitch;
    static AcceptTeleportation decode(Reader& r) {
        AcceptTeleportation a;
        a.id = r.varint();
        a.x = r.f64();
        a.y = r.f64();
        a.z = r.f64();
        a.yaw = r.f32();
        a.pitch = r.f32();
        return a;
    }
};

struct PlayerAction {
    // ServerboundPlayerActionPacket.Action ordinals. 26.x: CHANGE_DESTROY_DIRECTION is new and
    // shifts everything after it by one; STAB is new at the end.
    enum : u32 {
        kStartDestroyBlock,
        kChangeDestroyDirection,
        kAbortDestroyBlock,
        kStopDestroyBlock,
        kDropAllItems,
        kDropItem,
        kReleaseUseItem,
        kSwapItemWithOffhand,
        kStab,
        kActionCount
    };
    u32 action;
    BlockPos pos;
    u32 direction;  // 0..5 = down, up, north, south, west, east
    i32 sequence;
    static PlayerAction decode(Reader& r) {
        PlayerAction a;
        a.action = r.varint();
        a.pos = unpack_block_pos(r.be64());
        a.direction = r.u8v() % 6u;  // Direction.from3DDataValue
        a.sequence = static_cast<i32>(r.varint());
        if (a.action >= kActionCount) r.ok = false;  // readEnum throws
        return a;
    }
};

struct UseItemOn {  // hand, BlockHitResult, sequence
    u32 hand;       // 0 main, 1 off
    BlockPos pos;
    u32 direction;
    float cursor_x, cursor_y, cursor_z;  // hit location relative to the block's corner
    bool inside;
    bool world_border_hit;
    i32 sequence;
    static UseItemOn decode(Reader& r) {
        UseItemOn u;
        const u32 hand = r.varint();
        u.hand = hand == 1 ? 1 : 0;  // ByIdMap ... OutOfBoundsStrategy.ZERO
        u.pos = unpack_block_pos(r.be64());
        const i32 dir = static_cast<i32>(r.varint());
        u.direction = static_cast<u32>(((dir % 6) + 6) % 6);  // Direction.BY_ID wraps
        u.cursor_x = r.f32();
        u.cursor_y = r.f32();
        u.cursor_z = r.f32();
        u.inside = r.boolean();
        u.world_border_hit = r.boolean();
        u.sequence = static_cast<i32>(r.varint());
        return u;
    }
};

struct SetCarriedItem {
    i16 slot;  // hotbar index 0..8
    static SetCarriedItem decode(Reader& r) { return {static_cast<i16>(r.be16())}; }
};

// The stack is ItemStack.OPTIONAL_UNTRUSTED_STREAM_CODEC, which only item::read_untrusted reads:
// this stops in front of it.
struct SetCreativeModeSlot {
    i16 slot;  // menu slot; negative: drop the stack
    static SetCreativeModeSlot decode(Reader& r) { return {static_cast<i16>(r.be16())}; }
};

// HashedStack: what a client believes a slot holds after predicting its own click - item and
// count in full, each component as a CRC32C of its value. Only the number of components is kept:
// the server cannot compute those hashes yet, so a stack with components never counts as known.
struct HashedStack {
    bool present = false;
    u32 item = 0;
    i32 count = 0;
    u32 components = 0;    // added + removed entries
    u32 highest_type = 0;  // the largest component type id among them
    static HashedStack decode(Reader& r) {
        HashedStack h;
        h.present = r.boolean();
        if (!h.present) return h;
        h.item = r.varint();
        h.count = static_cast<i32>(r.varint());
        const u32 added = r.varint();  // map type -> int, at most 256
        if (added > 256) r.ok = false;
        for (u32 i = 0; i < added && r.ok; ++i) {
            h.highest_type = std::max(h.highest_type, r.varint());
            r.be32();
        }
        const u32 removed = r.varint();  // set of types, at most 256
        if (removed > 256) r.ok = false;
        for (u32 i = 0; i < removed && r.ok; ++i) h.highest_type = std::max(h.highest_type, r.varint());
        h.components = added + removed;
        return h;
    }
};

struct ContainerClick {
    // ContainerInput ids.
    enum : u32 { kPickup, kQuickMove, kSwap, kClone, kThrow, kQuickCraft, kPickupAll, kInputCount };
    static constexpr u32 kMaxChanged = 128;
    u32 container_id;
    u32 state_id;
    i16 slot;  // -999: outside the window
    i8 button;
    u32 input;
    u32 changed_count;
    struct Changed {
        i16 slot;
        HashedStack stack;
    } changed[kMaxChanged];  // the slots the client's prediction touched, and what it left there
    HashedStack carried;
    static void decode(Reader& r, ContainerClick& c) {
        c.container_id = r.varint();
        c.state_id = r.varint();
        c.slot = static_cast<i16>(r.be16());
        c.button = static_cast<i8>(r.u8v());
        c.input = r.varint();
        if (c.input >= kInputCount) c.input = kPickup;  // ByIdMap ... OutOfBoundsStrategy.ZERO
        c.changed_count = r.varint();
        if (c.changed_count > kMaxChanged) {
            r.ok = false;
            c.changed_count = 0;
        }
        for (u32 i = 0; i < c.changed_count && r.ok; ++i) {
            c.changed[i].slot = static_cast<i16>(r.be16());
            c.changed[i].stack = HashedStack::decode(r);
        }
        c.carried = HashedStack::decode(r);
    }
};

struct ContainerClose {
    u32 container_id;
    static ContainerClose decode(Reader& r) { return {r.varint()}; }
};

// 26.x: the left mouse button, whatever it hit - the air, a block, an entity (then `attack`
// comes first). Replaces `swing`: no hand, the server reads the animation off the main hand's
// item. No payload.
struct Punch {
    static Punch decode(Reader&) { return {}; }
};

// Middle click on a block ("pick block"). 26.x: the server finds the item and moves it into
// the hand; the client only says which block.
struct PickItemFromBlock {
    BlockPos pos;
    bool include_data;  // ctrl held: take the block entity's data along
    static PickItemFromBlock decode(Reader& r) {
        PickItemFromBlock p;
        p.pos = unpack_block_pos(r.be64());
        p.include_data = r.boolean();
        return p;
    }
};

// A command typed into the chat, without its slash. chat_command when nothing in it is signed;
// chat_command_signed when the client signed its message arguments (a kick's reason) - the
// signatures are read past, not checked: this server does not enforce secure chat.
struct ChatCommand {
    static constexpr u32 kMaxBytes = 32767 * 3;  // readUtf(): 32767 chars
    std::string_view command;
    static ChatCommand decode(Reader& r) { return {r.str(kMaxBytes)}; }
    // ServerboundChatCommandSignedPacket: command, timestamp, salt, up to 8 (argument name,
    // 256-byte signature), then LastSeenMessages.Update: an offset, 20 bits, a checksum byte.
    static ChatCommand decode_signed(Reader& r) {
        ChatCommand c{r.str(kMaxBytes)};
        r.be64();
        r.be64();
        const u32 signatures = r.varint();
        if (signatures > 8) r.ok = false;
        for (u32 i = 0; i < signatures && r.ok; ++i) {
            r.str(16 * 3);
            r.bytes(256);
        }
        r.varint();
        r.bytes(3 + 1);
        return c;
    }
};

struct PlayerCommand {
    // ServerboundPlayerCommandPacket.Action ordinals. 26.x: sneaking is no longer here - it
    // arrives as the shift bit of player_input.
    enum : u32 {
        kStopSleeping,
        kStartSprinting,
        kStopSprinting,
        kStartRidingJump,
        kStopRidingJump,
        kOpenInventory,
        kStartFallFlying,
        kActionCount
    };
    u32 entity_id;  // the client's own id; vanilla ignores it
    u32 action;
    u32 data;  // jump strength for kStartRidingJump
    static PlayerCommand decode(Reader& r) {
        PlayerCommand c;
        c.entity_id = r.varint();
        c.action = r.varint();
        c.data = r.varint();
        if (c.action >= kActionCount) r.ok = false;  // readEnum throws
        return c;
    }
};

struct PlayerInput {  // which movement keys are held
    static constexpr u8 kForward = 1, kBackward = 2, kLeft = 4, kRight = 8, kJump = 16, kShift = 32, kSprint = 64;
    u8 flags;
    static PlayerInput decode(Reader& r) { return {r.u8v()}; }
};

struct PlayerAbilities {
    static constexpr u8 kFlying = 2;
    u8 flags;
    static PlayerAbilities decode(Reader& r) { return {r.u8v()}; }
};

// move_player_pos / _pos_rot / _rot / _status_only share one shape; flags: bit0 on ground,
// bit1 horizontal collision.
struct MovePlayer {
    double x, y, z;
    float yaw, pitch;
    u8 flags;
    static MovePlayer decode_pos(Reader& r) {
        MovePlayer m{};
        m.x = r.f64();
        m.y = r.f64();
        m.z = r.f64();
        m.flags = r.u8v();
        return m;
    }
    static MovePlayer decode_pos_rot(Reader& r) {
        MovePlayer m{};
        m.x = r.f64();
        m.y = r.f64();
        m.z = r.f64();
        m.yaw = r.f32();
        m.pitch = r.f32();
        m.flags = r.u8v();
        return m;
    }
    static MovePlayer decode_rot(Reader& r) {
        MovePlayer m{};
        m.yaw = r.f32();
        m.pitch = r.f32();
        m.flags = r.u8v();
        return m;
    }
    static MovePlayer decode_status(Reader& r) {
        MovePlayer m{};
        m.flags = r.u8v();
        return m;
    }
    static constexpr u8 kOnGround = 1, kHorizontalCollision = 2;
};

}  // namespace sb
}  // namespace proto
