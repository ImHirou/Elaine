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

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "base/buf.hpp"
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
#include "server/inventory.hpp"
#include "server/prelogin.hpp"
#include "server/settings.hpp"
#include "world/collide.hpp"
#include "world/dimension.hpp"
#include "world/generator.hpp"

namespace server {

// The chunk a block coordinate is in (SectionPos.blockToSectionCoord).
inline i32 chunk_of(double v) { return proto::floor_int(v) >> 4; }

class Game {
  public:
    // `console`: where the server's own command lines come from, null for none.
    Game(const Settings& s, std::vector<std::unique_ptr<net::Reactor>>& reactors, std::atomic<u32>& online,
         Console* console = nullptr);
    ~Game();

    void run(const std::atomic<bool>& stop);  // blocks; call on the tick thread

    // The command tree: where a command is added (server/command.hpp says how). Players are
    // sent the tree again at the end of the tick in which it changed.
    cmd::Registry& commands() { return commands_; }

  private:
    struct Commands;  // the server's own commands (server/commands.cpp)
    friend struct cmd::Invocation;

    static constexpr u32 kNone = ~0u;
    static constexpr u64 kTickNs = 50 * 1'000'000ull;
    static constexpr u32 kOutInitial = 8192;  // first size of a player's per-tick output buffer
    static constexpr u64 kNever = ~0ull;      // a tick that does not come

    // dirty_ bits: what a player's tick has to look at again.
    static constexpr u8 kMenuDirty = 1;   // slots may differ from what the client was told (Menu::send_changes)
    static constexpr u8 kEquipDirty = 2;  // hands or armour may differ from what viewers were told

    // How a block is set: the flags of Level.setBlock that mean something here (Block.UPDATE_*).
    static constexpr u32 kUpdateNeighbors = 1;        // the six neighbours hear neighborChanged
    static constexpr u32 kUpdateClients = 2;          // (always: every change is broadcast)
    static constexpr u32 kUpdateKnownShape = 16;      // the neighbours' shapes are left alone
    static constexpr u32 kUpdateSkipContainer = 256;  // SKIP_BLOCK_ENTITY_SIDEEFFECTS: what a container held is not spilled
    static constexpr u32 kUpdateSkipOnPlace = 512;    // the new block is not told it was placed
    static constexpr u32 kUpdateAll = kUpdateNeighbors | kUpdateClients;

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
        // ...and commandSpamThrottler: +20 for each command, -1 a tick, up to 20 a second of the threshold.
        u32 command_spam = 0;
        u64 command_spam_tick = 0;
        u8 level = cmd::kAll;  // PermissionLevel: 0, or what the ops list gives
        Menu menu;             // the inventory, the menu that is open, and what the client knows of both
        // The menu that is open, when it is not the player's own: its number
        // (ServerPlayer.containerCounter, 1..100), the block it was opened on, and for a chest
        // or a furnace the block entities behind it (chests_ / furnaces_; a double chest has two).
        u32 container_counter = 0;
        proto::BlockPos open_pos{};
        u32 open_data[2] = {kNone, kNone};
    };
    // Level.blockEvent: (b0, b1) for the block at a position, sent to who is near after the
    // tracker (ServerLevel.runBlockEvents).
    struct BlockEventData {
        i32 x, y, z;
        u8 b0, b1;
    };
    struct Recheck {  // a scheduled ChestBlock.tick
        u64 tick;
        i32 x, y, z;
    };

    void drain(u32 ri);
    void on_join(u32 ri, const net::InHeader& h, const u8* payload, u32 n);
    void on_leave(u32 ri, const net::InHeader& h);
    void on_packet(u32 pi, const u8* body, u32 n);
    void send_view(u32 pi, i32 ocx, i32 ocz, bool initial);
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
        u8 phase = 0;      // (tickCount + id) & 3 is (tick_ + phase) & 3
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

    // ---- commands (server/commands.cpp)
    struct Op {  // an entry of ops.json (ServerOpListEntry)
        Uuid uuid;
        std::string name;
        u8 level;
        bool bypasses_player_limit;
    };
    void init_commands();
    void on_chat_command(u32 pi, std::string_view line);
    void run_command(const cmd::Source& source, std::string_view line);
    void run_console();
    cmd::Source source_of(u32 pi) const;
    cmd::Source console_source() const;
    u32 player_of(const cmd::Source& source) const;
    void send_commands(u32 pi);
    void send_permission_level(u32 pi);
    void system_message(u32 pi, const text::Text& message);
    void command_success(const cmd::Source& source, const text::Text& message, bool broadcast);
    void command_failure(const cmd::Source& source, const text::Text& message);
    text::Text display_name(u32 row) const;
    static text::Text item_display_name(const ItemStack& stack);
    void select(const cmd::Selector& sel, const cmd::Source& source, bool players_only, std::vector<u32>& slots);
    bool teleport(u32 row, double x, double y, double z, u32 relatives, float yaw, float pitch);
    u32 summon(u32 type, double x, double y, double z);
    void remove_entity(u32 slot);
    bool destroy_block_with_drops(proto::BlockPos pos);
    bool place_block_input(proto::BlockPos pos, const cmd::BlockArg& block, u32 flags);
    void load_ops();
    void save_ops() const;
    u32 find_op(Uuid uuid) const;

    // ---- block entities and the menus on them (server/block_entities.cpp)
    bool set_block(proto::BlockPos pos, world::BlockState state, u32 flags = kUpdateAll);
    void update_neighbors_at(proto::BlockPos pos);
    void neighbor_changed(proto::BlockPos pos);
    world::BlockState update_shape(world::BlockState state, u32 direction, world::BlockState neighbour) const;
    world::BlockState update_from_neighbour_shapes(world::BlockState state, proto::BlockPos pos) const;
    bool use_block(u32 pi, proto::BlockPos pos, world::BlockState state, const ItemStack& held, u32 hand);
    world::BlockState chest_for_placement(u32 pi, proto::BlockPos at, u32 face) const;
    void open_menu(u32 pi, Menu::Kind kind, u32 menu_type, std::span<const u8> title, u32 rows);
    void close_menu(u32 pi, bool tell_client);
    bool menu_still_valid(u32 pi);
    void close_menus_on(u32 kind, u32 data);
    void chest_opened(u32 chest);
    void chest_closed(u32 chest);
    void chest_sound(const Chest& c, u32 sound);
    void recheck_chests();
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
    bool obstructed(world::BlockState s, proto::BlockPos pos) const;
    world::BlockState state_for_client(proto::BlockPos pos) const;
    void ack(u32 pi, i32 sequence) { ack_[pi] = std::max(ack_[pi], sequence); }
    // Fill viewers_ with the players whose view contains chunk (cx, cz), and border_ with those
    // who have it on the edge of their view.
    void find_viewers(i32 cx, i32 cz);
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
    world::FlatGenerator generator_;
    world::Dimension world_;

    // Every entity of the world; rows [0, players_.size()) are the players, in our order.
    entity::Store ents_;
    entity::Tracker tracker_;

    // hot (SoA), index = player index = entity index
    std::vector<i32> pcx_, pcz_, pview_;  // chunk the client's view is centred on, and its radius
    std::vector<i32> ack_;                // highest block-change sequence to acknowledge this tick, -1 = none
    std::vector<u8> dirty_;               // k*Dirty bits: almost always 0, so a tick skips the inventories
    std::vector<base::ByteBuf*> out_;     // this tick's output, never null
    // cold, same index
    std::vector<Player> players_;
    std::vector<std::vector<u32>> slot_to_player_;  // [reactor][slot] -> player index

    base::BufPool pool_;
    base::ByteBuf scratch_;
    base::ByteBuf text_scratch_;  // one text component, encoded, on its way into a packet
    std::vector<crypto::Cfb8Job> jobs_;
    std::vector<u8> touched_;  // per reactor: needs a wake after flush
    std::vector<u32> viewers_, border_;
    std::vector<u16> update_pos_, update_state_;
    u64 tick_ = 0;
    u64 rng_;

    // Item entities: what an ItemEntity has beyond its row of the entity table (where its stack
    // is). Parallel arrays in no order, found from the entity's slot.
    std::vector<u32> item_slot_;      // the entity
    std::vector<u64> item_born_;      // tick it was created in: Entity.tickCount is tick - born + 1 as it ticks
    std::vector<u64> item_age_from_;  // tick ItemEntity.age counts from (a merge keeps the younger age)
    std::vector<u64> item_pickup_;    // first tick a player can pick it up in
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

    // Block entities: what a chest and a furnace hold. A world::BlockEntityRef's `data` is the
    // index here; entries are reused (alive = false in between), never moved, so that the menus
    // open on them can name them by index. Furnaces are walked every tick: the ones that
    // neither burn nor could start to are skipped by one test (Furnace::idle).
    std::vector<Chest> chests_;
    std::vector<Furnace> furnaces_;
    std::vector<u32> chest_free_, furnace_free_;
    std::vector<BlockEventData> block_events_;
    std::vector<Recheck> rechecks_;  // in the order they are due: the delay is always the same
    usize recheck_head_ = 0;
    // Menu titles, as the text components vanilla sends (text::transcode), and update_recipes.
    std::vector<u8> title_chest_, title_double_chest_, title_furnace_, title_crafting_;
    base::ByteBuf recipes_frame_;

    // Primed TNT, by slot: what an explosion looks through for others to push.
    std::vector<u32> tnt_;

    // Everything that ticks, in the order it was created (EntityTickList): entity slots, kNone
    // where one was removed. movers_ is by entity slot.
    std::vector<u32> tick_order_;
    u32 tick_holes_ = 0;
    bool entities_ticked_ = false;  // this tick's pass over them has begun: what is created now has its first tick in the next
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
