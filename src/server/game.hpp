// SPDX-License-Identifier: GPL-3.0-or-later
// Game: the tick thread. Drains packets from the reactors, advances the world, and flushes one
// buffer per player per tick.
//
// A player is an entity: its position, rotation and everything other players are shown live in
// the entity table (entity::Store), where players are the rows [0, players) - so a player's
// index here IS its entity index. What only a connection has stays here, split by access pattern:
//   * hot, bulk-iterated state in parallel arrays (SoA) indexed by that same index;
//   * everything touched only on join/leave/occasional packets in `Player` (cold).
// Removal is swap-with-last (the store does the same to its player rows), so all of it stays dense.
#pragma once

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "base/buf.hpp"
#include "base/workers.hpp"
#include "crypto/cfb8.hpp"
#include "entity/grid.hpp"
#include "entity/store.hpp"
#include "entity/tracker.hpp"
#include "net/reactor.hpp"
#include "phys/physics.hpp"
#include "proto/frame.hpp"
#include "proto/packets.hpp"
#include "server/command.hpp"
#include "server/console.hpp"
#include "server/files.hpp"
#include "server/inventory.hpp"
#include "server/player_data.hpp"
#include "server/prelogin.hpp"
#include "server/settings.hpp"
#include "server/tick_rate.hpp"
#include "server/world_info.hpp"
#include "world/collide.hpp"
#include "world/dimension.hpp"
#include "world/fluid.hpp"
#include "world/generator.hpp"

namespace server {

// StringUtil.isAllowedChatCharacter for every character of a line: no control characters, no
// section sign (server/chat.cpp).
bool chat_allowed(std::string_view s);

// The block kinds whose block entity is a Chest (Game::chests_): a chest of any kind, and a barrel.
constexpr bool chest_kind(u32 kind) { return kind == proto::gen::block_kind::kChest || kind == proto::gen::block_kind::kBarrel; }
// #copper_chests, of a block of kind kChest: it weathers, or is one that would but for its wax
// (BlockInfo::alt[2], see kPlaceChest).
constexpr bool copper_chest(const world::BlockInfo& chest) { return chest.weather != 0 || chest.alt[2] != 0; }

// The chunk a block coordinate is in (SectionPos.blockToSectionCoord).
inline i32 chunk_of(double v) { return proto::floor_int(v) >> 4; }

class Game {
  public:
    // `console`: where the server's own command lines come from, null for none. `world`: where
    // the world is kept and what makes its chunks (server/world_info.hpp); `workers`: the threads
    // that read, write and generate them.
    Game(const Settings& s, std::vector<std::unique_ptr<net::Reactor>>& reactors, std::atomic<u32>& online, Console* console,
         const WorldInfo& world, base::Workers& workers);
    ~Game();

    void run(const std::atomic<bool>& stop);  // blocks; call on the tick thread

    // The command tree: where a command is added (server/command.hpp says how). Players are
    // sent the tree again at the end of the tick in which it changed.
    cmd::Registry& commands() { return commands_; }

  private:
    struct Commands;  // the server's own commands (server/commands.cpp)
    friend struct cmd::Invocation;

    static constexpr u32 kNone = ~0u;
    static constexpr u32 kOutInitial = 8192;  // first size of a player's per-tick output buffer
    static constexpr u64 kNever = ~0ull;      // a tick that does not come

    // dirty_ bits: what a player's tick has to look at again.
    static constexpr u8 kMenuDirty = 1;   // slots may differ from what the client was told (Menu::send_changes)
    static constexpr u8 kEquipDirty = 2;  // hands or armour may differ from what viewers were told

    // How a block is set: the flags of Level.setBlock that mean something here (Block.UPDATE_*).
    static constexpr u32 kUpdateNeighbors = 1;       // the six neighbours hear neighborChanged
    static constexpr u32 kUpdateClients = 2;         // (always: every change is broadcast)
    static constexpr u32 kUpdateKnownShape = 16;     // the neighbours' shapes are left alone
    static constexpr u32 kUpdateSuppressDrops = 32;  // a neighbour that breaks of it drops nothing
    static constexpr u32 kUpdateMoveByPiston = 64;
    static constexpr u32 kUpdateSkipContainer = 256;  // SKIP_BLOCK_ENTITY_SIDEEFFECTS: what a container held is not spilled
    static constexpr u32 kUpdateSkipOnPlace = 512;    // the new block is not told it was placed
    static constexpr u32 kUpdateAll = kUpdateNeighbors | kUpdateClients;
    static constexpr i32 kUpdateLimit = 512;              // Block.UPDATE_LIMIT: how deep one change's shape updates may nest
    static constexpr u32 kLevelEventDestroyBlock = 2001;  // LevelEvent.PARTICLES_AND_SOUND_DESTROY_BLOCK: `data` is the state

    struct Player {
        u32 reactor = 0, slot = 0, gen = 0;
        Uuid uuid;
        std::string name;
        std::vector<u8> profile_wire;  // uuid + name + properties as sent in tab-list entries; encoded once
        crypto::Cfb8 enc;
        bool encrypted = false;
        i32 threshold = proto::kNoCompression;
        u32 teleport_id = 0;
        bool awaiting_teleport = false;
        bool keepalive_pending = false;
        u64 keepalive_id = 0;
        u64 keepalive_sent_ns = 0;
        u32 latency_ms = 0;
        // Block actions are ignored until the client reports it finished loading, or this tick
        // (ServerGamePacketListenerImpl.clientLoadedTimeoutTimer).
        u64 loaded_at_tick = 0;
        bool shift = false;   // sneak key held (player_input)
        bool flying = false;  // creative flight active (player_abilities)
        // LivingEntity.swingState: the tick the arm last started to swing in, and for how long.
        u64 swing_tick = kNever;
        u8 swing_len = 0;
        // ServerGamePacketListenerImpl.dropSpamThrottler, as of drop_spam_tick: +20 for each
        // stack thrown out of the creative inventory, -1 a tick.
        u32 drop_spam = 0;
        u64 drop_spam_tick = 0;
        // Bytes of heavy stacks this client sent of late, as of heavy_inflow_tick (admit_heavy).
        u32 heavy_inflow = 0;
        u64 heavy_inflow_tick = 0;
        // ...and commandSpamThrottler: +20 for each command, -1 a tick, up to 20 a second of the threshold.
        u32 command_spam = 0;
        u64 command_spam_tick = 0;
        u32 chat_spam = 0;  // chatSpamThrottler: the same for what is said
        u64 chat_spam_tick = 0;
        u8 level = cmd::kAll;  // PermissionLevel: 0, or what the ops list gives
        Menu menu;             // the inventory, the menu that is open, and what the client knows of both
        // The menu that is open, when it is not the player's own: its number
        // (ServerPlayer.containerCounter, 1..100), the block it was opened on, and for a chest
        // or a furnace the block entities behind it (chests_ / furnaces_; a double chest has two).
        u32 container_counter = 0;
        proto::BlockPos open_pos{};
        u32 open_data[2] = {kNone, kNone};
        // What is on disk of this player (save_player): the tick it was last looked at for
        // saving, and a hash of the bytes that were written then.
        u64 saved_tick = 0;
        u64 saved_hash = 0;
    };
    // PlayerChatMessage, of a sender without a chat session (there are no others here): whose it
    // is, what was typed, and - where a command made something else of it (the selectors in a
    // /say) - what is shown instead (unsignedContent).
    struct ChatMessage {
        Uuid sender;  // all zero: nobody's (PlayerChatMessage.system), which travels as disguised_chat
        std::string content;
        bool decorated = false;
        text::Text shown;
        bool system() const { return sender == Uuid{}; }
    };
    // Somebody whose connection is the tick thread's already and whose file is still being
    // read: a player from the moment it is (finish_join).
    struct Joining {
        u64 id;
        u32 reactor;
        net::InHeader h;
        net::JoinTransport t;
        JoinInfo info;
        bool ready = false;  // its file has been read...
        bool has_data = false;
        PlayerData data;  // ...and held this
    };
    // Level.blockEvent: (b0, b1) for the block at a position, sent to who is near after the
    // tracker (ServerLevel.runBlockEvents).
    struct BlockEventData {
        i32 x, y, z;
        u8 b0, b1;
    };
    // CollectingNeighborUpdater.NeighborUpdates: one of the things a block change makes its
    // neighbours do, waiting for its turn (server/block_updates.cpp).
    struct NeighborUpdate {
        enum Kind : u8 { kShape, kSimple, kFull, kMulti };
        u8 kind;
        u8 dir;                // kShape: where the block that changed lies from `pos`; kMulti: the side that is skipped, kNoSide
        u8 next;               // kMulti: how many of NeighborUpdater.UPDATE_ORDER are done
        bool moved;            // kFull: movedByPiston
        u16 state;             // kShape: the neighbour's state; kFull: the state that is told
        u16 block;             // the block that changed (kSimple, kFull, kMulti)
        proto::BlockPos pos;   // who is told (kMulti: around which the six are)
        proto::BlockPos from;  // kShape: where the neighbour is
        u32 flags;
        i32 limit;
    };
    static constexpr u8 kNoSide = 0xFF;

    // PlayerChunkSender: the chunks of a player's view that are ready and have not gone out yet,
    // and how fast its client takes them. `bits` is a window over the view: a bit per chunk of
    // [x0, x0 + side) x [z0, z0 + side), side = 2 * (view distance + 1) + 1.
    struct ChunkSender {
        static constexpr u32 kMaxSide = 2 * (32 + 1) + 1;  // a view distance is at most 32
        static constexpr u32 kWords = (kMaxSide * kMaxSide + 63) / 64;
        float desired = 9.0f;   // desiredChunksPerTick: START_CHUNKS_PER_TICK until the client says
        float quota = 0.0f;     // batchQuota
        i32 unacked = 0;        // unacknowledgedBatches...
        i32 max_unacked = 1;    // ...and how many there may be: one until the first is acknowledged
        bool has_view = false;  // a view has been applied (ChunkTrackingView.EMPTY before)
        u32 pending = 0;        // bits set
        i32 x0 = 0, z0 = 0;
        u32 side = 0;
        u64 bits[kWords] = {};
        bool test(i32 cx, i32 cz) const {
            const u32 dx = static_cast<u32>(cx - x0), dz = static_cast<u32>(cz - z0);
            return dx < side && dz < side && ((bits[(dz * side + dx) >> 6] >> ((dz * side + dx) & 63)) & 1);
        }
    };
    // A block entity's handle (world::BlockEntityRef::data) with this bit names an entry of
    // opaque_: one that came with an imported world and is kept as the tag it was (server/persist.cpp).
    static constexpr u32 kOpaqueBlockEntity = 1u << 31;
    // ---- a block entity out of the tag vanilla saves it as (server/saved_tags.cpp)
    enum class Loaded : u8 {
        kOk,
        kNone,     // no block entity there that a tag could be loaded into
        kRefused,  // the tag asks for what this server cannot do: `why` says what, nothing was changed
    };
    // BlockEntity.loadWithComponents, or - without components - loadCustomOnly: what the chest,
    // barrel or furnace at `pos` holds becomes what `tag` says. `changed`: it is not what it was.
    Loaded load_block_entity(proto::BlockPos pos, nbt::Tag tag, bool with_components, bool& changed, std::string& why);
    // Could a block entity of `block` be loaded from `tag`? (Asked before anything is changed.)
    static bool can_load_block_entity(u32 block, nbt::Tag tag, std::string& why);
    // A block entity that was kept as its tag, now that its chunk is here: a chest, a barrel or
    // a furnace of it, if the tag is one this server can act on. False: it stays a tag.
    bool adopt_kept_block_entity(proto::BlockPos pos, world::BlockState state, u32 type, std::span<const u8> tag);
    // BlockItem.place, after the block: what the stack's components make of the block and of
    // its block entity (updateBlockStateFromTag, updateCustomBlockEntityTag, updateBlockEntityComponents).
    void apply_item_to_block(proto::BlockPos pos, const ItemStack& stack);
    // ...and whether this server can do that for `stack` on `block` (a loot table it cannot roll).
    static bool can_apply_item_to_block(u32 block, const ItemStack& stack);
    u32 keep_tag(std::span<const u8> tag);  // an entry of opaque_ with these bytes; kNone for none
    void drop_kept(u32& keep);
    // What is kept beside a chest or a furnace (its CustomName, lock, components...): the entry
    // `name` of it, a view into `doc`.
    nbt::Tag kept_entry(u32 keep, std::string_view name, nbt::Doc& doc) const;
    // BaseContainerBlockEntity.canOpen: does what the player holds open the lock, if there is one?
    bool unlocks(u32 keep, const ItemStack& held) const;
    // sendChestLockedNotifications: "<name> is locked!" over the hotbar, and the rattle.
    void chest_locked(u32 pi, double x, double y, double z, const text::Text& name);

    void drain(u32 ri);
    void on_join(u32 ri, const net::InHeader& h, const u8* payload, u32 n);
    void finish_join(u32 ri, const net::InHeader& h, const net::JoinTransport& t, JoinInfo& j, PlayerData* saved);
    // What is kept of a player (server/player_data.hpp): read before it joins, written when it
    // leaves and every autosave-seconds while it stays.
    bool keeps_players() const { return !player_dir_.empty(); }
    std::string player_path(Uuid uuid) const;
    static void player_file_read(void* game, u64 id, std::span<const u8> bytes, bool found);
    void admit_joining();
    void save_player(u32 pi);
    void autosave_players();
    void on_leave(u32 ri, const net::InHeader& h);
    void on_packet(u32 pi, const u8* body, u32 n);

    // ---- chunks: who sees which, what is loaded, what is saved (server/chunks.cpp)
    void apply_view(u32 pi, i32 cx, i32 cz, i32 distance);
    void chunk_ready(world::Chunk& c);
    void send_chunks();
    void on_chunk_batch_received(u32 pi, float desired);
    bool chunk_tracked(u32 pi, i32 cx, i32 cz) const;
    void pump_chunks(u64 tick_start_ns);
    void tend_chunks();
    void save_chunk(world::Chunk& c, bool leaving);
    void save_world();
    void collect_entities(bool all);

    // ---- what a chunk holds of the game's, as bytes (server/persist.cpp)
    void write_block_entities(const world::Chunk& c, base::ByteBuf& out) const;
    void write_entities(u64 chunk_key, base::ByteBuf& out);
    void read_block_entities(world::Chunk& c, std::span<const u8> bytes);
    void read_entities(world::Chunk& c, std::span<const u8> bytes);
    void drop_block_entities(world::Chunk& c);
    void free_block_entity(u32 kind, u32 data);
    u32 new_chest(proto::BlockPos pos);
    u32 new_furnace(proto::BlockPos pos, item::Cooker type);
    // AbstractFurnaceBlockEntity's recipeType, by the type of the block entity.
    static item::Cooker cooker_of(u32 block_entity_type);
    void keepalives(u64 now);
    u64 flush();  // returns the bytes handed to the reactors
    void kick(u32 pi, std::string_view reason);
    void kick(u32 pi, const text::Text& reason);
    void remove_player(u32 pi);
    void refuse(u32 ri, const net::InHeader& h, const net::JoinTransport& t, std::string_view reason);
    void post(u32 ri, const net::OutMsg& m);
    void init_titles();

    // ---- the player as an entity (server/players.cpp)
    void on_move(u32 pi, const proto::sb::MovePlayer& m, bool has_pos, bool has_rot);
    void on_player_command(u32 pi, const proto::sb::PlayerCommand& c);
    void on_client_information(u32 pi, const proto::sb::ClientInformation& info);
    void tick_players();
    void update_pose(u32 pi);
    bool fits(u32 pi, float height) const;

    // ---- the inventory (server/inventory_actions.cpp)
    void on_creative_slot(u32 pi, i32 slot, ItemStack stack, bool unsupported);
    // ---- heavy stacks (item::kHeavyPatchBytes): what one client may make the server read and hold
    static constexpr u32 kHeavyInflowLimit = 4u << 20;     // bytes of them a client may send in a burst...
    static constexpr u32 kHeavyInflowPerTick = 64u << 10;  // ...and what a tick forgives: 1.25 MiB a second
    // What a player may come to carry of them by creative slots, /give and picking up. (Out of
    // a chest somebody else filled it can be more: a stack is never lost to this.)
    static constexpr u64 kHeavyCarried = 8u << 20;
    bool admit_heavy(Player& p, u32 bytes);  // the client sends so many bytes of a stack: within what it may?
    static u64 carried_heavy(const Player& p);
    static bool may_carry(const Player& p, const ItemStack& stack, u32 stacks = 1) {
        const u32 heavy = item::heavy_bytes(stack);
        return LIKELY(!heavy) || carried_heavy(p) + static_cast<u64>(heavy) * stacks <= kHeavyCarried;
    }
    void on_container_click(u32 pi, const proto::sb::ContainerClick& c);
    void on_container_close(u32 pi);
    void on_drop_key(u32 pi, bool all);
    void on_swap_hands(u32 pi);
    void sync_menus();
    void show_equipment(u32 pi);
    void apply_effects(u32 pi);
    void drop(u32 pi, ItemStack stack, bool thrown, bool predicted);
    void throw_velocity(u32 pi, double v[3]);
    void fake_item(u32 pi, ItemStack stack);
    void on_punch(u32 pi);
    bool swing(u32 pi, u32 hand, proto::Swing how, bool to_self);
    void play_sound(u32 sound, double x, double y, double z);
    Containers containers(const Player& p);
    Menu::Ctx menu_ctx(u32 pi) { return {true, effects_, containers(players_[pi])}; }  // everyone is in creative mode
    // Something changed, or may have, in a player's inventory.
    void touch_inventory(u32 pi) { dirty_[pi] |= kMenuDirty | kEquipDirty; }
    u64 random() {  // xorshift64*: for what vanilla draws from an entity's RandomSource
        rng_ ^= rng_ >> 12;
        rng_ ^= rng_ << 25;
        rng_ ^= rng_ >> 27;
        return rng_ * 0x2545F4914F6CDD1Dull;
    }

    // ---- item entities (server/item_entities.cpp)
    void spawn_item(ItemStack stack, double x, double y, double z, u64 pickup_delay = 40, u64 age = 0);
    void spawn_item(ItemStack stack, double x, double y, double z, double vx, double vy, double vz, u64 pickup_delay,
                    u64 age = 0);
    u32 add_item(Uuid uuid, float yaw, ItemStack stack, double x, double y, double z, double vx, double vy, double vz,
                 u64 pickup_delay, u64 age, i8 health);
    void remove_item(u32 slot);
    void tick_item(u32 slot, u32 row);
    void rest_item(u32 slot);
    void freeze_item(u32 slot);
    void merge_item(u32 k);
    void pick_up_items(u32 pi);
    void move_item_cell(u32 slot, double old_x, double old_z);

    // ---- what moves by itself (server/physics.cpp)
    // An entity's place among those that tick, and what Entity keeps about how it moves
    // (phys::Body). By entity slot; players have one too, for the grid of what can be pushed.
    struct Mover {
        u32 order = kNone;  // its place in tick_order_
        u8 kind = 0;        // gen::physics::kMover*: whose tick moves it, 0 = nobody's
        bool in_grid = false;
        bool asleep = false;  // at rest: see tick_entities()
        u8 patience = 0;      // an item that was asked whether it rests and did not: how long until it is asked again
        u32 cell = 0;         // its place in its cell of push_grid_, which is cell (gx, gz)
        i32 gx = 0, gz = 0;
        u64 retry = 0;  // ...not before this tick
        phys::Body body;
    };
    // What the tick of an item at rest reads, and all it reads: by entity slot like movers_, but
    // 32 bytes to the item where a Mover is 200. With ten thousand items lying about the pass
    // over them is a walk down this array; through movers_ it was a cache miss an item - one
    // load, and 66 % of the pass (perf on a running server; the pass took 1.1 times what the
    // entity tracker took for the same items, now 0.3 times).
    struct ItemSleep {
        phys::ItemRest rest;
        u32 merge_at = 0;  // the tick of its next look around (every 40th of its life), as far as 32 bits say...
        u32 expires = 0;   // ...and of its last
        u8 phase = 0;      // (tickCount + id) & 3 is (entity_tick_ + phase) & 3
    };
    // tick_order_ holds entity slots; the top bits say what is at rest, so that a pass over the
    // list does not have to look anything up to skip it. (kNone, a hole, has them all.)
    static constexpr u32 kAsleep = 1u << 31, kAsleepItem = 1u << 30, kSlotMask = kAsleepItem - 1;
    // An item entity without a stack (/summon item): its first tick discards it.
    static constexpr u8 kMoverEmptyItem = 0xFF;
    void start_ticking(u32 slot, u8 mover);
    void stop_ticking(u32 slot);
    void tick_entities();
    void tick_living(u32 slot, u32 row);
    bool push_entities(u32 row, float width, float height);
    void sleep(u32 slot);
    void wake(u32 slot);
    void wake_near(proto::BlockPos pos);
    void wake_touching(const world::Aabb& box, u32 but);
    void wake_at_players();
    void push(u32 row, double x, double y, double z);
    phys::Motion motion_of(u32 row) const;
    void store_motion(u32 row, const phys::Motion& m, phys::Body& b);
    void enter_push_grid(u32 slot, double x, double z, float width);
    void leave_push_grid(u32 slot);
    void move_in_push_grid(u32 slot, double x, double z);
    static float random_float_of(void* game) { return static_cast<Game*>(game)->random_float(); }

    // ---- the world's clocks (server/time.cpp)
    void tick_clocks(bool runs);
    void send_clocks(u32 pi);
    void clock_changed(u32 clock);
    proto::cb::SetTime::Clock clock_state(u32 clock) const;
    void save_time();
    void on_command_suggestion(u32 pi, const proto::sb::CommandSuggestion& c);
    void answer_suggestions();
    static void ask_server(void* game, const cmd::Registry::Asked& asked, std::vector<std::string>& out);
    // ---- how fast the game ticks (server/time.cpp): ServerTickRateManager, with who is told
    void set_tick_rate(float rate);
    void set_frozen(bool frozen);
    bool step_game_if_paused(i32 ticks);
    bool stop_stepping();
    bool stop_sprinting();
    bool request_game_to_sprint(i32 ticks);
    void finish_tick_sprint();
    bool check_should_sprint_this_tick();

    // ---- chat (server/chat.cpp)
    void on_chat(u32 pi, const proto::sb::Chat& c);
    void on_chat_ack(u32 pi, u32 offset);
    bool chat_seen(u32 pi, const proto::sb::LastSeenUpdate& seen);
    bool detect_spam(u32 pi, bool chat);
    void encode_chat(const ChatMessage& m, u32 chat_type, const text::Text& name, const text::Text* target);
    void deliver_chat(u32 pi, bool system);
    void broadcast_chat(const ChatMessage& m, u32 chat_type, const text::Text& name);
    void send_chat(u32 pi, const ChatMessage& m, u32 chat_type, const text::Text& name, const text::Text* target);
    void broadcast_system(const text::Text& message, u32 except = kNone);
    static text::Text chat_content(const ChatMessage& m);
    static text::Text decorate_chat(u32 chat_type, const text::Text& name, const text::Text* target, const text::Text& content);
    text::Text join_names(const std::vector<u32>& slots) const;

    // ---- commands (server/commands.cpp)
    struct Op {  // an entry of ops.json (ServerOpListEntry)
        Uuid uuid;
        std::string name;
        u8 level;
        bool bypasses_player_limit;
    };
    void init_commands();
    void on_chat_command(u32 pi, const proto::sb::ChatCommand& c);
    void run_command(const cmd::Source& source, std::string_view line);
    void run_console();
    cmd::Source source_of(u32 pi) const;
    cmd::Source console_source() const;
    u32 player_of(const cmd::Source& source) const;
    void send_commands(u32 pi);
    void send_permission_level(u32 pi);
    void system_message(u32 pi, const text::Text& message, bool overlay = false);
    void command_success(const cmd::Source& source, const text::Text& message, bool broadcast);
    void command_failure(const cmd::Source& source, const text::Text& message);
    text::Text display_name(u32 row) const;
    static text::Text item_display_name(const ItemStack& stack);
    void select(const cmd::Selector& sel, const cmd::Source& source, bool players_only, std::vector<u32>& slots);
    bool teleport(u32 row, double x, double y, double z, u32 relatives, float yaw, float pitch);
    u32 summon(u32 type, double x, double y, double z);
    // ---- an entity out of a tag (server/entity_tags.cpp)
    struct EntityTag {
        entity::Cold cold;
        bool has_cold = false;
        double motion[3] = {0, 0, 0};
        bool has_motion = false, has_rotation = false;
        float yaw = 0, pitch = 0;
        double fall_distance = 0;
        bool on_ground = false, no_gravity = false, glowing = false, invisible = false;
        ItemStack item;  // an item entity's stack, its age, its pickup delay, its health
        i32 item_age = 0, pickup_delay = 0, item_health = 5;
        i32 fuse = 80;                  // a primed TNT's
        world::BlockState falling = 0;  // a falling block's state, the ticks it has fallen, whether it drops
        bool has_falling = false, drop_item = true, cancel_drop = false;
        i32 falling_time = 0;
    };
    static bool read_entity_tag(u32 type, nbt::Tag tag, EntityTag& out, std::string& why);
    void apply_entity_tag(u32 slot, EntityTag&& tag);
    u32 summon(u32 type, double x, double y, double z, nbt::Tag tag, std::string& why);
    void write_cold_tag(u32 row, nbt::Writer& w) const;

    // ---- what the game holds, as the tags vanilla saves it as (server/saved_tags.cpp)
    enum class Saved : u8 {
        kOk,
        kNone,  // nothing there to save
    };
    // BlockEntity.saveWithFullMetadata of the block entity at a block: appended in the network form.
    Saved save_block_entity(i32 x, i32 y, i32 z, base::ByteBuf& out);
    // NbtPredicate.getEntityTagToCompare: Entity.saveWithoutId, and what a player holds as SelectedItem.
    Saved save_entity(u32 row, base::ByteBuf& out);
    void remove_entity(u32 slot);
    bool destroy_block_with_drops(proto::BlockPos pos);
    bool place_block_input(proto::BlockPos pos, const cmd::BlockArg& block, u32 flags);
    void load_ops();
    void save_ops() const;
    u32 find_op(Uuid uuid) const;

    // ---- a block changes, and what follows from it (server/block_updates.cpp)
    bool set_block(proto::BlockPos pos, world::BlockState state, u32 flags = kUpdateAll, i32 limit = kUpdateLimit);
    bool remove_block(proto::BlockPos pos);
    bool break_block(proto::BlockPos pos, bool drop, i32 limit = kUpdateLimit);
    void drop_resources(world::BlockState state, proto::BlockPos pos);
    ItemStack block_drop(world::BlockState state);
    void pop_resource(proto::BlockPos pos, ItemStack stack);
    void level_event(u32 type, proto::BlockPos pos, i32 data, u32 except = kNone);
    void update_neighbors_at(proto::BlockPos pos, u32 source_block);
    void update_neighbour_shapes(world::BlockState state, proto::BlockPos pos, u32 flags, i32 limit);
    void add_update(const NeighborUpdate& update);
    void run_updates();
    bool run_next(usize at);
    void neighbor_changed(world::BlockState state, proto::BlockPos pos, u32 source_block);
    world::BlockState update_shape(world::BlockState state, proto::BlockPos pos, u32 direction, proto::BlockPos neighbour_pos,
                                   world::BlockState neighbour);
    world::BlockState update_from_neighbour_shapes(world::BlockState state, proto::BlockPos pos);
    void on_place(world::BlockState state, proto::BlockPos pos, world::BlockState old);
    bool can_survive(world::BlockState state, proto::BlockPos pos) const;
    bool touches_water(proto::BlockPos pos) const;
    void schedule_tick(proto::BlockPos pos, world::BlockState state, i32 delay) {
        world_.schedule_tick(pos.x, pos.y, pos.z, world::block_of(state), delay);
    }
    void run_block_ticks();
    void tick_block(world::BlockState state, proto::BlockPos pos);
    // ---- fluids flow (server/fluids.cpp; what a fluid asks of the world is world/fluid.hpp)
    world::FluidRules fluid_rules(u32 family) const;
    void schedule_fluid_tick(proto::BlockPos pos, u32 fluid, i32 delay) {
        world_.schedule_fluid_tick(pos.x, pos.y, pos.z, fluid, delay);
    }
    void run_fluid_ticks();
    void tick_fluid(proto::BlockPos pos, u32 type);
    void spread_fluid(const world::FluidRules& r, proto::BlockPos pos, world::BlockState state, world::FluidState held);
    void spread_to_sides(const world::FluidRules& r, proto::BlockPos pos, world::BlockState state, world::FluidState held);
    void spread_to(const world::FluidRules& r, proto::BlockPos pos, world::BlockState state, u32 dir, world::FluidState becomes);
    bool should_spread_liquid(proto::BlockPos pos, world::BlockState state);
    void liquid_changed(world::BlockState state, proto::BlockPos pos);
    void liquid_shape_changed(world::BlockState state, proto::BlockPos pos, world::BlockState neighbour);
    void run_random_ticks();
    void random_tick(world::BlockState state, proto::BlockPos pos);
    void tick_spreading(world::BlockState state, proto::BlockPos pos);
    void tick_weathering(world::BlockState state, proto::BlockPos pos);
    void tick_budding_amethyst(proto::BlockPos pos);
    u32 raw_brightness(proto::BlockPos pos) const;
    void recheck_chest(proto::BlockPos pos);
    world::BlockState chest_update_shape(world::BlockState state, u32 dir, world::BlockState neighbour) const;
    bool copper_chest_may_weather(world::BlockState state, proto::BlockPos pos) const;
    // ---- blocks whose state is their shape: slabs, stairs, fences, panes, walls... (server/block_shapes.cpp)
    world::BlockState shaped_for_placement(u32 pi, const proto::sb::UseItemOn& u, const world::BlockInfo& block, u32 kind,
                                           proto::BlockPos at, bool replacing_clicked) const;
    world::BlockState shaped_update(world::BlockState state, const world::BlockInfo& info, proto::BlockPos pos, u32 dir,
                                    world::BlockState neighbour) const;
    u32 stairs_shape(u32 facing, bool top, proto::BlockPos pos) const;
    static bool slab_can_be_replaced(world::BlockState state, const ItemStack& held, const proto::sb::UseItemOn& u,
                                     bool replacing_clicked);
    // Which chunks are simulated (server/chunks.cpp).
    void move_simulation(bool had, i32 old_cx, i32 old_cz, bool has, i32 cx, i32 cz);
    void simulate_adopted(world::Chunk& c);

    // ---- falling blocks (server/falling.cpp)
    struct Falling {                   // what a FallingBlockEntity has beyond its row of the entity table: by entity slot
        i32 time = 0;                  // FallingBlockEntity.time
        bool drop_item = true;         // dropItem
        bool cancel_drop = false;      // cancelDrop
        bool blocks_building = false;  // Entity.blocksBuilding: set for a block that fell, not for one that was summoned
        bool alive = false;
    };
    u32 fall(proto::BlockPos pos, world::BlockState state);
    u32 add_falling(Uuid uuid, world::BlockState state, double x, double y, double z, i32 time, bool fell);
    void tick_falling(u32 slot, u32 row);

    // ---- block entities and the menus on them (server/block_entities.cpp)
    // InteractionResult, of a block that was clicked: nothing for it (the item has the click),
    // done (the client swung its arm itself), or done where only the server knows it was (the
    // player is shown its own arm swing).
    enum class Used : u8 { kPass, kSuccess, kSuccessServer };
    Used use_block(u32 pi, proto::BlockPos pos, world::BlockState state, const ItemStack& held, u32 hand);
    // ---- signs (server/signs.cpp)
    static bool sign_kind(u32 kind);
    static u32 sign_facing(world::BlockState s);    // of the two that hang on a wall: a Direction
    static u32 sign_rotation(world::BlockState s);  // of the two that do not: sixteenths of a turn
    u32 new_sign(proto::BlockPos pos);
    void write_sign_tag(const Sign& sign, nbt::Writer& w) const;
    void show_sign(u32 data);
    void sign_changed(u32 data);
    bool sign_front_faces(u32 pi, const Sign& sign) const;
    void open_sign_editor(u32 pi, u32 data, bool front);
    void tick_signs();
    void sign_placed(u32 pi, proto::BlockPos pos);
    void on_sign_update(u32 pi, const proto::sb::SignUpdate& u);
    Used use_sign(u32 pi, proto::BlockPos pos, world::BlockState state, const ItemStack& held, u32 hand);
    world::BlockState sign_for_placement(u32 pi, const proto::sb::UseItemOn& u, const world::BlockInfo& block, u32 kind,
                                         proto::BlockPos at, bool replacing_clicked) const;
    world::BlockState chest_for_placement(u32 pi, const world::BlockInfo& block, proto::BlockPos at, u32 face) const;
    void open_menu(u32 pi, Menu::Kind kind, u32 menu_type, std::span<const u8> title, u32 rows);
    void close_menu(u32 pi, bool tell_client);
    bool menu_still_valid(u32 pi);
    void close_menus_on(u32 kind, u32 data);
    void chest_opened(u32 chest);
    void chest_closed(u32 chest);
    void lid_moved(u32 chest, bool open);
    void block_event(i32 x, i32 y, i32 z, u8 b0, u8 b1);
    void run_block_events();
    void tick_furnaces();
    void drop_contents(proto::BlockPos pos, ItemStack* items, u32 n);
    void play_sound(u32 sound, u32 source, double x, double y, double z, float volume, float pitch);
    float random_float() { return static_cast<float>(random() >> 40) * (1.0f / 16777216.0f); }           // RandomSource.nextFloat
    double random_double() { return static_cast<double>(random() >> 11) * (1.0 / 9007199254740992.0); }  // nextDouble
    u32 random_int(u32 bound) { return static_cast<u32>((random() >> 32) * bound >> 32); }               // nextInt(bound)

    // ---- TNT and explosions (server/explosion.cpp)
    bool has_neighbour_signal(proto::BlockPos pos) const;
    void prime_tnt(proto::BlockPos pos, i32 fuse, bool sound);
    void tick_tnt(u32 slot, u32 row);
    void explode(double x, double y, double z, float radius);
    float seen_percent(double cx, double cy, double cz, const world::Aabb& box) const;

    // ---- blocks (server/block_actions.cpp)
    void on_player_action(u32 pi, const proto::sb::PlayerAction& a);
    void on_use_item_on(u32 pi, const proto::sb::UseItemOn& u);
    void on_pick_block(u32 pi, const proto::sb::PickItemFromBlock& pk);
    bool client_loaded(const Player& p) const { return tick_ >= p.loaded_at_tick; }
    bool crouching(const Player& p) const { return p.shift && !p.flying; }
    bool within_reach(u32 pi, proto::BlockPos pos, double buffer = 1.0) const;
    static u32 horizontal_direction(float yaw);
    bool destroy_block(u32 pi, proto::BlockPos pos);
    bool place_block(u32 pi, const proto::sb::UseItemOn& u, const ItemStack& held);
    world::BlockState state_for_placement(u32 pi, const proto::sb::UseItemOn& u, const world::BlockInfo& block, u32 kind,
                                          proto::BlockPos at, bool replacing_clicked) const;
    void nearest_looking_directions(u32 pi, u32 out[6]) const;
    void placement_directions(u32 pi, u32 face, bool replacing_clicked, u32 out[6]) const;
    bool obstructed(world::BlockState s, proto::BlockPos pos) const;
    world::BlockState state_for_client(proto::BlockPos pos) const;
    void ack(u32 pi, i32 sequence) { ack_[pi] = std::max(ack_[pi], sequence); }
    // Fill viewers_ with the players that have been sent chunk (cx, cz), and - if asked - border_
    // with those of them who have it on the edge of what they have been sent.
    void find_viewers(i32 cx, i32 cz, bool want_border);
    void broadcast_changes();

    // This tick's outbound bytes of a player (plaintext until flush). Always there while the
    // player is: the entity tracker writes to all of them without asking.
    base::ByteBuf& out(u32 pi) { return *out_[pi]; }
    template <class P>
    void send(u32 pi, const P& pkt) {
        proto::write_packet(out(pi), players_[pi].threshold, pkt);
    }
    // Serialize once, memcpy to every player (all connections share one compression threshold).
    template <class P>
    void broadcast(const P& pkt, u32 except = kNone) {
        scratch_.clear();
        proto::write_packet(scratch_, s_.compression_threshold, pkt);
        for (u32 i = 0; i < players_.size(); ++i)
            if (i != except) out(i).append(scratch_.data, scratch_.len);
    }

    const Settings& s_;
    std::vector<std::unique_ptr<net::Reactor>>& reactors_;
    std::atomic<u32>& online_;
    base::Workers& workers_;
    const WorldInfo& info_;
    world::FlatGenerator flat_generator_;
    world::EmptyGenerator empty_generator_;
    world::ChunkGenerator& generator_;
    world::Dimension world_;
    double spawn_[3] = {8.5, 0, 8.5};  // where a player joins

    // Every entity of the world; rows [0, players_.size()) are the players, in our order.
    entity::Store ents_;
    entity::Tracker tracker_;

    // hot (SoA), index = player index = entity index
    std::vector<i32> pcx_, pcz_, pview_;  // chunk the client's view is centred on, and its radius
    std::vector<i32> ack_;                // highest block-change sequence to acknowledge this tick, -1 = none
    // Chat, read for every recipient of every message: how many player_chat packets this client
    // has been sent (ServerGamePacketListenerImpl.nextChatIndex - it counts along and leaves
    // when the two disagree), and what it wants to be shown (ChatVisiblity).
    std::vector<u32> chat_index_;
    std::vector<u8> chat_visibility_;
    std::vector<u8> dirty_;            // k*Dirty bits: almost always 0, so a tick skips the inventories
    std::vector<base::ByteBuf*> out_;  // this tick's output, never null
    // cold, same index
    std::vector<ChunkSender> senders_;
    std::vector<Player> players_;
    std::vector<std::vector<u32>> slot_to_player_;  // [reactor][slot] -> player index
    std::vector<Joining> joining_;
    u64 join_seq_ = 0;
    std::string player_dir_;  // where players' files are; empty: nothing of a player is kept
    u32 autosave_at_ = 0;     // autosave_players(): whose turn it is
    base::ByteBuf player_scratch_;
    Files files_;  // (after everything its answers touch: it waits for them when it goes)

    base::BufPool pool_;
    base::ByteBuf scratch_;
    base::ByteBuf text_scratch_;                                      // one text component, encoded, on its way into a packet
    base::ByteBuf chat_name_, chat_target_, chat_shown_, chat_tail_;  // a chat message being encoded
    std::vector<crypto::Cfb8Job> jobs_;
    std::vector<u8> touched_;  // per reactor: needs a wake after flush
    std::vector<u32> viewers_, border_;
    std::vector<entity::PendingChunks> pending_views_;  // what the tracker reads of senders_
    struct Nearest {                                    // send_chunks(): a pending chunk and how far it is
        i32 d2, x, z;
    };
    std::vector<Nearest> nearest_;
    // Chunk upkeep: what the walk over the chunks picked this tick, and the entities found in them.
    std::vector<world::Chunk*> leaving_, saving_;
    struct EntityAt {
        u64 chunk;  // packed chunk position
        u32 slot;
    };
    std::vector<EntityAt> entities_at_;
    base::ByteBuf part_blocks_, part_entities_;
    // Block entities that are kept as the tag they came as: the tag's bytes, by index.
    std::vector<std::vector<u8>> opaque_;
    std::vector<u32> opaque_free_;
    std::vector<u16> update_pos_, update_state_;
    u64 tick_ = 0;  // MinecraftServer.tickCount: every tick, also one of a game that stands still
    // The passes over the entities there have been - one in every tick of a game that runs
    // (TickRateManager.runsNormally), none while it is frozen: what an entity's age is counted in.
    // During a pass it is the passes before this one.
    u64 entity_tick_ = 0;
    // `/tick`: the rate (the `tick-rate` setting until a command says otherwise), frozen, stepping,
    // sprinting. What is counted in seconds is counted in ticks of this rate (MinecraftServer's
    // autosave is).
    TickRate rate_;
    u64 ticks_of(u64 seconds) const { return std::max<u64>(1, static_cast<u64>(rate_.rate * static_cast<float>(seconds))); }
    // MinecraftServer.tickTimesNanos: how long each of the last hundred ticks took (`/tick query`).
    static constexpr u32 kTickSamples = 100;
    u64 tick_times_[kTickSamples] = {};
    u64 tick_times_sum_ = 0;
    u64 average_tick_ns() const { return tick_times_sum_ / std::min<u64>(kTickSamples, std::max<u64>(tick_, 1)); }
    u64 rng_;

    // Item entities: what an ItemEntity has beyond its row of the entity table (where its stack
    // is). Parallel arrays in no order, found from the entity's slot.
    std::vector<u32> item_slot_;      // the entity
    std::vector<u64> item_born_;      // (entity) tick it was created in: Entity.tickCount is tick - born + 1 as it ticks
    std::vector<u64> item_age_from_;  // tick ItemEntity.age counts from (a merge keeps the younger age)
    std::vector<u64> item_pickup_;    // the entity tick from which a player can pick it up (after so many passes)
    std::vector<u32> item_cell_;      // its position in its cell of item_grid_
    std::vector<u32> item_of_slot_;   // entity slot -> index into the arrays above, kNone
    // Where the items are, by 2 x 2 block cell and entity slot: what a player's reach or an
    // item's neighbourhood has to look at. The cell size is measured (bench "pickup scan", 500
    // players and 2000 items within 40 blocks of each other, per tick): 1.7 - 2.0 ms through the
    // tracker's 64-block cells, 0.5 ms with 8 blocks, 0.26 with 4, 0.18 with 2. Spread out it
    // is 8 - 10 us whatever the size.
    using ItemGrid = entity::BasicGrid<1>;
    static i32 item_cell_of(double v) { return ItemGrid::cell_of(proto::floor_int(v)); }
    ItemGrid item_grid_;
    std::vector<u32> item_scratch_;  // candidates of one spatial query, as entity slots
    std::vector<u32> item_dead_;     // entity slots to remove at the end of tick_items()
    std::vector<i8> item_health_;    // ItemEntity.health: 5, less what explosions took
    Effects effects_;                // what the menu operation being handled asks of the world

    // Block entities: what a chest (of wood or copper; a barrel too) and a furnace (a smoker, a
    // blast furnace) hold. A world::BlockEntityRef's `data` is the
    // index here; entries are reused (alive = false in between), never moved, so that the menus
    // open on them can name them by index. Furnaces are walked every tick: the ones that
    // neither burn nor could start to are skipped by one test (Furnace::idle).
    std::vector<Chest> chests_;
    std::vector<Furnace> furnaces_;
    std::vector<u32> chest_free_, furnace_free_;
    std::vector<Sign> signs_;
    std::vector<u32> sign_free_;
    std::vector<u32> editing_signs_;  // the signs somebody may write on just now (tick_signs)
    u32 last_use_face_ = 0;           // the face of the block that is being clicked (BlockHitResult.getDirection)
    std::vector<BlockEventData> block_events_;
    // CollectingNeighborUpdater: the updates that wait (a stack), those the one that runs has
    // made, how many there have been since the first of this chain, and how many there may be.
    std::vector<NeighborUpdate> update_stack_, update_added_;
    u32 update_count_ = 0;
    u64 time_ = 0;  // Level.getGameTime: what scheduled ticks are due by
    // ServerClockManager: the world's clocks, and what the overworld's makes of the sky's light
    // (Level.skyDarken, brought up to date once a tick).
    Clock clocks_[kClocks];
    i32 sky_darken_ = 0;
    // ServerCommandSuggestionsProvider: the line each of these players last asked about - one
    // answer a tick, to the latest question.
    struct SuggestionRequest {
        u64 player;  // entity::Handle
        u32 id;
        std::string line;
    };
    std::vector<SuggestionRequest> suggestion_requests_;
    u32 random_value_ = 0;  // Level.randValue: where the random ticks fall
    // A random tick that is run only to be seen doing nothing (world::kCheckDormantSections).
    bool expect_no_change_ = false;
    std::vector<world::Chunk*> random_scratch_;
    std::vector<Falling> falling_;    // by entity slot
    std::vector<u32> falling_slots_;  // the falling blocks there are: what a block being placed, or an explosion, looks through
    // Menu titles, as the text components vanilla sends (text::transcode), and update_recipes.
    std::vector<u8> title_chest_, title_double_chest_, title_furnace_, title_crafting_, title_barrel_, title_smoker_,
        title_blast_furnace_;
    base::ByteBuf recipes_frame_;

    // Primed TNT, by slot: what an explosion looks through for others to push.
    std::vector<u32> tnt_;

    // Everything that ticks, in the order it was created (EntityTickList): entity slots, kNone
    // where one was removed. movers_ is by entity slot.
    std::vector<u32> tick_order_;
    u32 tick_holes_ = 0;
    bool entities_ticked_ = false;        // the pass over them is under way: what is created now has its first tick in the next
    u32 asleep_ = 0, asleep_living_ = 0;  // how many of them are at rest; how many of those are alive
    std::vector<Mover> movers_;
    std::vector<ItemSleep> item_sleep_;
    phys::Scratch phys_scratch_;
    // Where the living and the players are, by 4 x 4 block cell and entity slot: who may be
    // touching whom (LivingEntity.pushEntities, an explosion). A query reaches as far as the
    // widest of them is wide.
    using PushGrid = entity::BasicGrid<2>;
    static i32 push_cell_of(double v) { return PushGrid::cell_of(proto::floor_int(v)); }
    PushGrid push_grid_;
    float push_half_width_ = 0.3f;
    std::vector<u32> push_scratch_;
    std::vector<world::BlockAt> blown_;  // explode(): the blocks one explosion reaches
    world::BlastScratch blast_scratch_;
    struct Collected {  // ServerExplosion.StackCollector
        proto::BlockPos pos;
        ItemStack stack;
    };
    std::vector<Collected> collected_;
    struct Knock {  // ServerExplosion.hitPlayers: what an explosion gives a player's velocity
        bool hit = false;
        double v[3] = {0, 0, 0};
    };
    std::vector<Knock> knock_;

    // Commands: the tree, who may use how much of it, and the console.
    cmd::Registry commands_;
    Console* console_;
    bool stop_requested_ = false;  // /stop
    std::vector<Op> ops_;
    // The `commands` packet per permission level, framed: built when first asked for after a
    // change of the tree, then copied to whoever has that level.
    base::ByteBuf command_frames_[cmd::kOwners + 1];
    u32 command_frames_version_[cmd::kOwners + 1] = {~0u, ~0u, ~0u, ~0u, ~0u};
    u32 commands_sent_ = 0;  // the tree version the players have
    std::vector<u32> select_scratch_;
};

}  // namespace server
