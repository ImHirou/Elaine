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
#include "proto/frame.hpp"
#include "proto/packets.hpp"
#include "server/inventory.hpp"
#include "server/prelogin.hpp"
#include "server/settings.hpp"
#include "world/dimension.hpp"
#include "world/generator.hpp"

namespace server {

// The chunk a block coordinate is in (SectionPos.blockToSectionCoord).
inline i32 chunk_of(double v) { return proto::floor_int(v) >> 4; }

class Game {
  public:
    Game(const Settings& s, std::vector<std::unique_ptr<net::Reactor>>& reactors, std::atomic<u32>& online);
    ~Game();

    void run(const std::atomic<bool>& stop);  // blocks; call on the tick thread

  private:
    static constexpr u32 kNone = ~0u;
    static constexpr u64 kTickNs = 50 * 1'000'000ull;
    static constexpr u32 kOutInitial = 8192;  // first size of a player's per-tick output buffer
    static constexpr u64 kNever = ~0ull;      // a tick that does not come

    // dirty_ bits: what a player's tick has to look at again.
    static constexpr u8 kMenuDirty = 1;   // slots may differ from what the client was told (Menu::send_changes)
    static constexpr u8 kEquipDirty = 2;  // hands or armour may differ from what viewers were told

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
        Menu menu;  // the inventory, and what the client knows of it
    };

    void drain(u32 ri);
    void on_join(u32 ri, const net::InHeader& h, const u8* payload, u32 n);
    void on_leave(u32 ri, const net::InHeader& h);
    void on_packet(u32 pi, const u8* body, u32 n);
    void send_view(u32 pi, i32 ocx, i32 ocz, bool initial);
    void keepalives(u64 now);
    u64 flush();  // returns the bytes handed to the reactors
    void kick(u32 pi, std::string_view reason);
    void remove_player(u32 pi);
    void refuse(u32 ri, const net::InHeader& h, const net::JoinTransport& t, std::string_view reason);
    void post(u32 ri, const net::OutMsg& m);

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
    void on_punch(u32 pi);
    bool swing(u32 pi, u32 hand, proto::Swing how, bool to_self);
    void play_sound(u32 sound, double x, double y, double z);
    Menu::Ctx menu_ctx() { return {true, effects_}; }  // everyone is in creative mode
    // Something changed, or may have, in a player's inventory.
    void touch_inventory(u32 pi) { dirty_[pi] |= kMenuDirty | kEquipDirty; }
    u64 random() {  // xorshift64*: for what vanilla draws from an entity's RandomSource
        rng_ ^= rng_ >> 12;
        rng_ ^= rng_ << 25;
        rng_ ^= rng_ >> 27;
        return rng_ * 0x2545F4914F6CDD1Dull;
    }

    // ---- item entities (server/item_entities.cpp)
    void spawn_item(ItemStack stack, double x, double y, double z);
    void remove_item(u32 slot);
    void tick_items();
    void merge_item(u32 k);
    void pick_up_items(u32 pi);

    // ---- blocks (server/block_actions.cpp)
    void on_player_action(u32 pi, const proto::sb::PlayerAction& a);
    void on_use_item_on(u32 pi, const proto::sb::UseItemOn& u);
    void on_pick_block(u32 pi, const proto::sb::PickItemFromBlock& pk);
    bool client_loaded(const Player& p) const { return tick_ >= p.loaded_at_tick; }
    bool crouching(const Player& p) const { return p.shift && !p.flying; }
    bool within_reach(u32 pi, proto::BlockPos pos) const;
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
    Effects effects_;                // what the menu operation being handled asks of the world
};

}  // namespace server
