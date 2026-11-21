// SPDX-License-Identifier: GPL-3.0-or-later
#include "server/game.hpp"

#include <cmath>
#include <ctime>
#include <thread>

#include "base/clock.hpp"
#include "base/log.hpp"
#include "proto/packets.hpp"

#include PROTO_GEN_BLOBS_HPP
#include PROTO_GEN_DIMENSION_TYPES_HPP

namespace server {

using namespace proto;
using net::InHeader;
using net::InKind;
using net::OutKind;
using net::OutMsg;

namespace {
constexpr u64 kKeepAliveEveryNs = 15 * base::kNsPerSec;
constexpr u64 kKeepAliveTimeoutNs = 30 * base::kNsPerSec;
constexpr u8 kCreative = 1;
constexpr std::string_view kOverworld = "minecraft:overworld";
// ServerGamePacketListenerImpl: a client that never sends player_loaded counts as loaded after 60 ticks.
constexpr u64 kClientLoadTimeoutTicks = 60;

constexpr proto::gen::dimension::Type kOverworldType = proto::gen::dimension::kTypes[reg::dimension_type::kOverworld];
}  // namespace

Game::Game(const Settings& s, std::vector<std::unique_ptr<net::Reactor>>& reactors, std::atomic<u32>& online, Console* console,
           const WorldInfo& world, base::Workers& workers)
    : s_(s),
      reactors_(reactors),
      online_(online),
      workers_(workers),
      info_(world),
      flat_generator_(kOverworldType.min_y, static_cast<u32>(kOverworldType.height) / 16, world.layers, world.biome),
      empty_generator_(world.sea_level, world.biome),
      generator_(world.flat ? static_cast<world::ChunkGenerator&>(flat_generator_) : empty_generator_),
      world_(reg::dimension_type::kOverworld, std::string(kOverworld), generator_, s.compression_threshold, &workers,
             world.chunk_dir, s.sync_chunk_writes),
      tracker_(ents_, s.compression_threshold, s.entity_broadcast_range),
      rng_(base::now_ns() | 1),
      console_(console) {
    random_value_ = static_cast<u32>(random());  // Level.randValue: RandomSource.create().nextInt()
    world_.keep_adopted(true);
    // Where players appear: on top of what stands at the world's spawn (the chunk is read for
    // it, once, here), or where the world says when nothing stands there.
    const i32 sx = world.has_spawn ? world.spawn[0] : 8, sz = world.has_spawn ? world.spawn[2] : 8;
    const world::Chunk& at_spawn = world_.load(sx >> 4, sz >> 4);
    const u32 top = at_spawn.motion_blocking.get(static_cast<u32>(sx & 15) + static_cast<u32>(sz & 15) * 16);
    spawn_[0] = sx + 0.5;
    spawn_[1] = top ? world_.min_y() + static_cast<i32>(top) : world.has_spawn ? world.spawn[1] : generator_.spawn_height();
    spawn_[2] = sz + 0.5;
    pump_chunks(0);  // (what the spawn chunk holds of the game's)
    pcx_.reserve(s.max_players);
    pcz_.reserve(s.max_players);
    pview_.reserve(s.max_players);
    ack_.reserve(s.max_players);
    dirty_.reserve(s.max_players);
    out_.reserve(s.max_players);
    players_.reserve(s.max_players);
    viewers_.reserve(s.max_players);
    border_.reserve(s.max_players);
    for (auto& r : reactors_) slot_to_player_.emplace_back(r->max_conns(), kNone);
    touched_.assign(reactors_.size(), 0);
    // The same for every client: the recipe property sets (mcgen), framed once.
    frame_raw(recipes_frame_, s.compression_threshold, {gen::kUpdateRecipesBlob, sizeof gen::kUpdateRecipesBlob});
    init_titles();
    init_commands();
    load_ops();
}

Game::~Game() {
    for (base::ByteBuf* b : out_) delete b;
}

void Game::post(u32 ri, const OutMsg& m) {
    net::Reactor& r = *reactors_[ri];
    while (UNLIKELY(!r.post(m))) {  // outbox full: the reactor is behind; let it catch up
        r.wake();
        std::this_thread::yield();
    }
    touched_[ri] = 1;
}

// ---------------------------------------------------------------------------- tick loop

void Game::run(const std::atomic<bool>& stop) {
    constexpr u64 kStatsEvery = 100;  // ticks: 5 s
    u64 next = base::now_ns();
    u64 busy_ns = 0, worst_ns = 0, track_ns = 0, move_ns = 0, flush_ns = 0, block_ns = 0, sent_bytes = 0;
    while (!stop.load(std::memory_order_relaxed) && !stop_requested_) {
        const u64 t0 = base::now_ns();
        // Same order as MinecraftServer.tickChildren: packets, then the level - its clock, the
        // scheduled block ticks, the chunks' random ticks, block and light changes go out to
        // whoever sees them (ServerChunkCache.broadcastChangedChunks), the entities are shown (ChunkMap.tick),
        // block events are sent, the entities tick (items, primed TNT; ServerPlayer.tick, which
        // is the menu), then the block entities (furnaces) - then the per-connection tick (block
        // change acks, the player's own tick), and one flush per player at the end.
        entities_ticked_ = false;
        pump_chunks(t0);  // the chunks the workers have ready enter the world
        for (u32 ri = 0; ri < reactors_.size(); ++ri) drain(ri);
        run_console();  // DedicatedServer.tickConnection: the console's lines after the players' packets
        // ServerLevel.tick: tickTime - what packets scheduled was scheduled by the tick before -
        // then the block ticks that are due, then (ServerChunkCache.tickChunks) the random ticks.
        world_.set_time(++time_);
        const u64 t_blocks = base::now_ns();
        run_block_ticks();
        run_random_ticks();
        block_ns += base::now_ns() - t_blocks;
        broadcast_changes();
        tend_chunks();  // ChunkMap.tick: what nobody needs is saved and leaves (the change lists are empty here)
        const u64 t1 = base::now_ns();
        pending_views_.resize(players_.size());
        for (u32 pi = 0; pi < players_.size(); ++pi) {
            const ChunkSender& cs = senders_[pi];
            pending_views_[pi] = {cs.bits, cs.x0, cs.z0, cs.side, cs.pending};
        }
        tracker_.set_pending_chunks(pending_views_.data());
        tracker_.tick(out_.data());
        track_ns += base::now_ns() - t1;
        run_block_events();  // ...block events after the chunks and the tracker, before the entities
        const u64 t_entities = base::now_ns();
        tick_entities();
        move_ns += base::now_ns() - t_entities;
        sync_menus();
        tick_furnaces();  // block entities tick after the entities
        tick_players();
        // Not t0: a player who joined during drain() has keepalive_sent_ns > t0, and the unsigned
        // difference would wrap and send a keep-alive at once.
        if (++tick_ % 20 == 0) keepalives(base::now_ns());
        if (UNLIKELY(commands_.version() != commands_sent_)) {  // a command was added or removed: everyone's tree is stale
            for (u32 pi = 0; pi < players_.size(); ++pi) send_commands(pi);
            commands_sent_ = commands_.version();
        }
        send_chunks();  // MinecraftServer.tickChildren ends with "send chunks"
        workers_.kick();
        const u64 t2 = base::now_ns();
        sent_bytes += flush();
        const u64 t3 = base::now_ns();
        flush_ns += t3 - t2;
        const u64 took = t3 - t0;
        busy_ns += took;
        worst_ns = std::max(worst_ns, took);
        if (tick_ % kStatsEvery == 0) {
            LOG_DEBUG(
                "tick: %.3f ms avg, %.3f ms worst, %.3f ms of it the entity tracker, %.3f ms the entities' own ticks, %.3f ms "
                "flush, %.3f ms block ticks; "
                "%.1f KiB/tick sent; %zu players, %u entities (%u at rest), %zu chunks (%zu on their way, %zu to be written, "
                "%zu with random ticks to run), %llu scheduled ticks, %.1f MiB of world",
                static_cast<double>(busy_ns) / kStatsEvery / 1e6, static_cast<double>(worst_ns) / 1e6,
                static_cast<double>(track_ns) / kStatsEvery / 1e6, static_cast<double>(move_ns) / kStatsEvery / 1e6,
                static_cast<double>(flush_ns) / kStatsEvery / 1e6, static_cast<double>(block_ns) / kStatsEvery / 1e6,
                static_cast<double>(sent_bytes) / kStatsEvery / 1024.0, players_.size(), ents_.size(), asleep_,
                world_.chunk_count(), world_.loading_count(), world_.store().waiting(), world_.random_chunks().size(),
                static_cast<unsigned long long>(world_.block_ticks().count()),
                static_cast<double>(world_.memory_bytes()) / (1024.0 * 1024.0));
            busy_ns = worst_ns = track_ns = move_ns = flush_ns = block_ns = sent_bytes = 0;
        }

        next += kTickNs;
        const u64 now = base::now_ns();
        if (now < next) {
            timespec ts{static_cast<time_t>(next / base::kNsPerSec), static_cast<long>(next % base::kNsPerSec)};
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
        } else if (now - next > base::kNsPerSec) {
            next = now;  // fell more than a second behind: do not try to replay the backlog
        }
    }
    while (!players_.empty()) kick(static_cast<u32>(players_.size()) - 1, "Server closed");
    for (auto& r : reactors_) r->wake();
    save_world();
}

void Game::drain(u32 ri) {
    net::Reactor& r = *reactors_[ri];
    base::ByteBuf* b;
    while (r.recycled().pop(b)) pool_.put(b);

    base::SpscBytes& in = r.inbound();
    const u8* rec;
    u32 n;
    while (in.front(rec, n)) {
        InHeader h;
        std::memcpy(&h, rec, sizeof h);
        const u8* payload = rec + sizeof h;
        const u32 plen = n - static_cast<u32>(sizeof h);
        if (LIKELY(h.kind == InKind::Packet)) {
            const u32 pi = slot_to_player_[ri][h.slot];
            if (LIKELY(pi != kNone && players_[pi].gen == h.gen)) on_packet(pi, payload, plen);
        } else if (h.kind == InKind::Join) {
            on_join(ri, h, payload, plen);
        } else {
            on_leave(ri, h);
        }
        in.pop(n);
    }
    in.release();
}

u64 Game::flush() {
    // Encrypt all streams that have output, interleaved 8 at a time, then hand the buffers over.
    jobs_.clear();
    for (u32 pi = 0; pi < players_.size(); ++pi) {
        Player& p = players_[pi];
        if (out_[pi]->len && p.encrypted) jobs_.push_back({&p.enc, out_[pi]->data, out_[pi]->len});
    }
    if (!jobs_.empty()) crypto::cfb8_encrypt_batch(jobs_);

    u64 bytes = 0;
    for (u32 pi = 0; pi < players_.size(); ++pi) {
        base::ByteBuf* b = out_[pi];
        if (b->len == 0) continue;
        const Player& p = players_[pi];
        bytes += b->len;
        b->tag = net::kBufOwnerTick;
        post(p.reactor, {p.slot, p.gen, OutKind::Send, b});
        out_[pi] = pool_.get(kOutInitial);
    }
    for (u32 ri = 0; ri < reactors_.size(); ++ri) {
        if (touched_[ri]) reactors_[ri]->wake();
        touched_[ri] = 0;
    }
    return bytes;
}

// ---------------------------------------------------------------------------- join / leave

void Game::refuse(u32 ri, const InHeader& h, const net::JoinTransport& t, std::string_view reason) {
    base::ByteBuf* b = pool_.get(256);
    write_packet(*b, t.threshold, cb::Disconnect{reason});
    if (t.encrypted) {
        crypto::Cfb8 enc = t.enc;
        enc.encrypt(b->data, b->len);
    }
    b->tag = net::kBufOwnerTick;
    post(ri, {h.slot, h.gen, OutKind::SendClose, b});
}

void Game::on_join(u32 ri, const InHeader& h, const u8* payload, u32 n) {
    net::JoinTransport t;
    JoinInfo j;
    if (n < sizeof t) return;
    std::memcpy(&t, payload, sizeof t);
    if (!decode_join(payload + sizeof t, n - sizeof t, j)) {
        refuse(ri, h, t, "Internal error");
        return;
    }
    for (u32 i = 0; i < players_.size(); ++i) {
        if (players_[i].uuid == j.uuid) {
            kick(i, "You logged in from another location");
            break;
        }
    }
    if (players_.size() >= s_.max_players) {
        refuse(ri, h, t, "The server is full!");
        return;
    }

    const u32 pi = static_cast<u32>(players_.size());
    players_.emplace_back();
    Player& p = players_[pi];
    p.reactor = ri;
    p.slot = h.slot;
    p.gen = h.gen;
    p.uuid = j.uuid;
    p.name = std::move(j.name);
    {
        std::vector<ProfileProperty> props;
        for (const auto& pr : j.props) props.push_back({pr.name, pr.value, pr.signature});
        p.profile_wire.resize(profile_wire_bound(p.name, props) + base::ByteBuf::kSlack);
        const u8* end = put_profile_wire(p.profile_wire.data(), p.uuid, p.name, props);
        p.profile_wire.resize(static_cast<usize>(end - p.profile_wire.data()));
    }
    p.enc = t.enc;
    p.encrypted = t.encrypted != 0;
    p.threshold = t.threshold;
    p.keepalive_sent_ns = base::now_ns();  // first keep-alive one interval from now
    p.loaded_at_tick = tick_ + kClientLoadTimeoutTicks;
    if (const u32 op = find_op(p.uuid); op != kNone) p.level = ops_[op].level;  // MinecraftServer.getProfilePermissions
    const double spawn_y = spawn_[1];
    // The entity: a player's row in the entity table is its index here (the store keeps players
    // in front). Whoever is near is shown it by the tracker at the end of this tick, after the
    // tab-list entry below - a client drops an add_entity for a player it has no entry for.
    entity::Store::Moves moved;
    const u32 ei = tracker_.spawn(entity_type::kPlayer, true, p.uuid, spawn_[0], spawn_y, spawn_[2], 0, 0, moved);
    DCHECK(ei == pi);
    (void)ei;
    enter_push_grid(ents_.slot[pi], spawn_[0], spawn_[2], entity_type::kInfo[entity_type::kPlayer].width);
    ents_.set_skin_parts(pi, j.skin_parts);  // ServerPlayer.updateOptions
    ents_.set_main_hand(pi, j.main_hand);
    pcx_.push_back(chunk_of(spawn_[0]));
    pcz_.push_back(chunk_of(spawn_[2]));
    // ChunkMap.getPlayerViewDistance: what the client asked for, within 2 and the server's own.
    pview_.push_back(std::min<i32>(std::max<i32>(j.view_distance, 2), static_cast<i32>(s_.view_distance)));
    tracker_.set_view_distance(pi, static_cast<u32>(pview_[pi]));
    senders_.emplace_back();
    ack_.push_back(-1);
    dirty_.push_back(0);
    out_.push_back(pool_.get(kOutInitial));
    slot_to_player_[ri][h.slot] = pi;
    online_.store(static_cast<u32>(players_.size()), std::memory_order_relaxed);
    LOG_INFO("%s (%s) joined [%zu online]", p.name.c_str(), j.remote.c_str(), players_.size());

    // Same order as vanilla's PlayerList.placeNewPlayer, minus what we do not simulate yet.
    const std::string_view dims[] = {kOverworld};
    cb::Login login{};
    login.entity_id = ents_.net_id(pi);
    login.dimensions = dims;
    login.max_players = s_.max_players;
    login.view_distance = s_.view_distance;
    login.simulation_distance = s_.simulation_distance;
    login.show_death_screen = true;
    login.dimension_type = reg::dimension_type::kOverworld;
    login.dimension = kOverworld;
    login.game_mode = kCreative;
    login.previous_game_mode = -1;
    login.is_flat = generator_.is_flat();
    login.sea_level = static_cast<u32>(generator_.sea_level());  // a VarInt; -63 for the flat generator
    login.online_mode = s_.online_mode;
    login.enforces_secure_chat = false;
    send(pi, login);

    send(pi, cb::PlayerAbilities{cb::PlayerAbilities::kInvulnerable | cb::PlayerAbilities::kCanFly |
                                 cb::PlayerAbilities::kInstabuild});
    send(pi, cb::SetHeldSlot{p.menu.inv.selected});
    // What a client's screens ask about recipes: which items a furnace takes (the shift-click
    // it predicts), and so on. The recipe book stays empty: nothing is unlocked.
    out(pi).append(recipes_frame_.data, recipes_frame_.len);
    send_permission_level(pi);  // its permission level, and the commands it may use

    p.teleport_id = 1;
    p.awaiting_teleport = true;
    send(pi, cb::PlayerPosition{p.teleport_id, ents_.x[pi], ents_.y[pi], ents_.z[pi], 0, 0, 0, 0, 0, 0});

    // Tab list, as vanilla does it: one packet with everyone already here for the newcomer, then
    // the newcomer to everyone (itself included). Entries reuse each player's pre-encoded profile.
    if (pi > 0) {
        std::vector<cb::PlayerInfoEntry> others;
        others.reserve(pi);
        for (u32 i = 0; i < pi; ++i) others.push_back({players_[i].profile_wire, kCreative, true, players_[i].latency_ms});
        send(pi, cb::PlayerInfoAdd{others});
    }
    const cb::PlayerInfoEntry self{p.profile_wire, kCreative, true, 0};
    broadcast(cb::PlayerInfoAdd{{&self, 1}});

    send(pi,
         cb::SetDefaultSpawnPosition{kOverworld, floor_int(spawn_[0]), static_cast<i32>(spawn_y), floor_int(spawn_[2]), 0, 0});
    send(pi, cb::GameEvent{cb::GameEvent::kLevelChunksLoadStart, 0});
    // ServerLevel.addNewPlayer -> ChunkMap.updatePlayerStatus: the view is the player's from here
    // on. Its chunks go out batch by batch, from the end of this tick on (send_chunks).
    apply_view(pi, pcx_[pi], pcz_[pi], pview_[pi]);
    // ServerPlayer.initInventoryMenu: the whole menu, which is empty - nothing is saved yet.
    p.menu.send_all(out(pi), p.threshold);
    send(pi, cb::SystemChat{"Welcome, " + players_[pi].name + "!"});
}

void Game::on_leave(u32 ri, const InHeader& h) {
    const u32 pi = slot_to_player_[ri][h.slot];
    if (pi != kNone && players_[pi].gen == h.gen) {
        LOG_INFO("%s left", players_[pi].name.c_str());
        remove_player(pi);
    }
    post(ri, {h.slot, h.gen, OutKind::Release, nullptr});  // the slot is ours to give back either way
}

void Game::kick(u32 pi, std::string_view reason) {
    Player& p = players_[pi];
    LOG_INFO("%s kicked: %.*s", p.name.c_str(), static_cast<int>(reason.size()), reason.data());
    send(pi, cb::Disconnect{reason});
    base::ByteBuf* b = out_[pi];
    if (p.encrypted) p.enc.encrypt(b->data, b->len);
    b->tag = net::kBufOwnerTick;
    post(p.reactor, {p.slot, p.gen, OutKind::SendClose, b});
    out_[pi] = pool_.get(kOutInitial);
    remove_player(pi);  // the reactor still owns the socket until its Leave record arrives
}

void Game::remove_player(u32 pi) {
    Player& p = players_[pi];
    const Uuid uuid = p.uuid;
    slot_to_player_[p.reactor][p.slot] = kNone;
    // Player.remove closes the open menu. What it would hand back is not kept - nothing of a
    // player is - but a chest must not go on counting an opener who left.
    if (p.menu.kind() == Menu::Kind::kChest) {
        for (const u32 chest : p.open_data)
            if (chest != kNone && chests_[chest].alive) chest_closed(chest);
    }

    if (senders_[pi].has_view) move_simulation(true, pcx_[pi], pcz_[pi], false, 0, 0);  // DistanceManager.removePlayer

    // PlayerList.remove: the entity goes first - whoever saw it gets remove_entities - then the
    // tab-list entry. The store fills the row with its last player, as we do with ours below.
    const u32 last = static_cast<u32>(players_.size()) - 1;
    leave_push_grid(ents_.slot[pi]);
    entity::Store::Moves moved;
    tracker_.despawn(pi, out_.data(), moved);
    DCHECK(pi == last ? (moved.n == 0 || moved.m[0].from > last) : (moved.m[0].from == last && moved.m[0].to == pi));
    pool_.put(out_[pi]);
    if (pi != last) {
        players_[pi] = std::move(players_[last]);
        pcx_[pi] = pcx_[last];
        pcz_[pi] = pcz_[last];
        pview_[pi] = pview_[last];
        senders_[pi] = senders_[last];
        ack_[pi] = ack_[last];
        dirty_[pi] = dirty_[last];
        out_[pi] = out_[last];
        slot_to_player_[players_[pi].reactor][players_[pi].slot] = pi;
    }
    players_.pop_back();
    pcx_.pop_back();
    pcz_.pop_back();
    pview_.pop_back();
    senders_.pop_back();
    ack_.pop_back();
    dirty_.pop_back();
    out_.pop_back();
    online_.store(static_cast<u32>(players_.size()), std::memory_order_relaxed);
    broadcast(cb::PlayerInfoRemove{uuid});
}

// ---------------------------------------------------------------------------- packets

HOT void Game::on_packet(u32 pi, const u8* body, u32 n) {
    namespace sbid = ids::play::sb;
    Reader rd(body, n);
    const u32 id = rd.varint();
    Player& p = players_[pi];
    switch (id) {
        case sbid::kMovePlayerPos: {
            const auto m = sb::MovePlayer::decode_pos(rd);
            if (rd.ok) on_move(pi, m, true, false);
            break;
        }
        case sbid::kMovePlayerPosRot: {
            const auto m = sb::MovePlayer::decode_pos_rot(rd);
            if (rd.ok) on_move(pi, m, true, true);
            break;
        }
        case sbid::kMovePlayerRot: {
            const auto m = sb::MovePlayer::decode_rot(rd);
            if (rd.ok) on_move(pi, m, false, true);
            break;
        }
        case sbid::kMovePlayerStatusOnly: {
            const auto m = sb::MovePlayer::decode_status(rd);
            if (rd.ok) on_move(pi, m, false, false);
            break;
        }
        case sbid::kKeepAlive:
            if (rd.be64() == p.keepalive_id && p.keepalive_pending) {
                p.keepalive_pending = false;
                p.latency_ms = static_cast<u32>((base::now_ns() - p.keepalive_sent_ns) / base::kNsPerMs);
            }
            break;
        case sbid::kAcceptTeleportation:
            if (rd.varint() == p.teleport_id) p.awaiting_teleport = false;
            break;
        case sbid::kPingRequest: {
            const u64 t = rd.be64();
            if (rd.ok) send(pi, cb::PlayPong{t});
            break;
        }
        case sbid::kPlayerLoaded: p.loaded_at_tick = 0; break;
        case sbid::kChunkBatchReceived: {
            const auto b = sb::ChunkBatchReceived::decode(rd);
            if (rd.ok) on_chunk_batch_received(pi, b.desired_chunks_per_tick);
            break;
        }
        case sbid::kPlayerInput: {
            const auto in = sb::PlayerInput::decode(rd);
            if (rd.ok && client_loaded(p)) {
                p.shift = (in.flags & sb::PlayerInput::kShift) != 0;
                ents_.set_shared_flag(pi, entity::shared_flag::kShiftKeyDown, p.shift);  // Entity.setShiftKeyDown
            }
            break;
        }
        case sbid::kPlayerCommand: {
            const auto c = sb::PlayerCommand::decode(rd);
            if (rd.ok) on_player_command(pi, c);
            break;
        }
        case sbid::kClientInformation: {
            const auto info = sb::ClientInformation::decode(rd);
            if (rd.ok) on_client_information(pi, info);
            break;
        }
        case sbid::kPlayerAbilities: {
            const auto a = sb::PlayerAbilities::decode(rd);
            if (rd.ok) p.flying = (a.flags & sb::PlayerAbilities::kFlying) != 0;  // everyone may fly: all creative
            break;
        }
        case sbid::kSetCarriedItem: {  // handleSetCarriedItem
            const auto c = sb::SetCarriedItem::decode(rd);
            if (rd.ok && c.slot >= 0 && c.slot < static_cast<i16>(inv::kHotbarSize)) {
                p.menu.inv.selected = static_cast<u8>(c.slot);
                dirty_[pi] |= kEquipDirty;  // another stack is in hand
            }
            break;
        }
        case sbid::kSetCreativeModeSlot: {
            const auto c = sb::SetCreativeModeSlot::decode(rd);
            ItemStack stack;
            const item::Verdict v = rd.ok ? item::read_untrusted(rd, stack) : item::Verdict::kMalformed;
            // Vanilla's decoder throws on such a stack, and on bytes left over behind a good one.
            if (v == item::Verdict::kMalformed || (v == item::Verdict::kOk && !rd.done()))
                kick(pi, "Invalid item stack");
            else
                on_creative_slot(pi, c.slot, std::move(stack), v == item::Verdict::kUnsupported);
            break;
        }
        case sbid::kContainerClick: {
            sb::ContainerClick c;
            sb::ContainerClick::decode(rd, c);
            if (rd.done())
                on_container_click(pi, c);
            else
                kick(pi, "Invalid container click");
            break;
        }
        case sbid::kContainerClose: {
            sb::ContainerClose::decode(rd);
            if (rd.ok) on_container_close(pi);
            break;
        }
        case sbid::kPlayerAction: {
            const auto a = sb::PlayerAction::decode(rd);
            if (rd.ok) on_player_action(pi, a);
            break;
        }
        case sbid::kUseItemOn: {
            const auto u = sb::UseItemOn::decode(rd);
            if (rd.ok) on_use_item_on(pi, u);
            break;
        }
        case sbid::kPickItemFromBlock: {
            const auto pk = sb::PickItemFromBlock::decode(rd);
            if (rd.ok) on_pick_block(pi, pk);
            break;
        }
        case sbid::kChatCommand:
        case sbid::kChatCommandSigned: {
            const auto c = id == sbid::kChatCommand ? sb::ChatCommand::decode(rd) : sb::ChatCommand::decode_signed(rd);
            // A copy: running it may send large packets, and the command is a view into the inflate buffer.
            if (rd.done()) on_chat_command(pi, std::string(c.command));  // (may kick: nothing of `p` after this)
            break;
        }
        case sbid::kPunch:  // handlePunch: not held back until the client has loaded, unlike the actions
            sb::Punch::decode(rd);
            if (rd.done()) on_punch(pi);
            break;
        default: break;  // not handled yet: the packet is skipped, not an error
    }
}

void Game::keepalives(u64 now) {
    for (u32 pi = static_cast<u32>(players_.size()); pi-- > 0;) {  // backwards: kick() swap-removes
        Player& p = players_[pi];
        if (p.keepalive_pending) {
            if (now - p.keepalive_sent_ns > kKeepAliveTimeoutNs) kick(pi, "Timed out");
        } else if (now - p.keepalive_sent_ns >= kKeepAliveEveryNs) {
            p.keepalive_pending = true;
            p.keepalive_sent_ns = now;
            p.keepalive_id = now / base::kNsPerMs;
            send(pi, cb::KeepAlive{p.keepalive_id});
        }
    }
}

}  // namespace server
