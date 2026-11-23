// SPDX-License-Identifier: GPL-3.0-or-later
#include "server/game.hpp"

#include <cmath>
#include <ctime>
#include <filesystem>
#include <thread>

#include "base/clock.hpp"
#include "base/log.hpp"
#include "proto/packets.hpp"
#include "text/text.hpp"

#include PROTO_GEN_BLOBS_HPP
#include PROTO_GEN_DIMENSION_TYPES_HPP

namespace server {

using namespace proto;
using net::InHeader;
using net::InKind;
using net::OutKind;
using net::OutMsg;
using text::Text;

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
      files_(workers, s.sync_chunk_writes),
      rng_(base::now_ns() | 1),
      console_(console) {
    rate_.set_rate(s.tick_rate);
    random_value_ = static_cast<u32>(random());  // Level.randValue: RandomSource.create().nextInt()
    time_ = static_cast<u64>(world.time.game_time);
    for (u32 i = 0; i < kClocks; ++i) clocks_[i] = world.time.clocks[i];
    sky_darken_ = sky_darken(clocks_[reg::world_clock::kOverworld].total_ticks);
    world_.set_time(time_);
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
    chat_index_.reserve(s.max_players);
    chat_visibility_.reserve(s.max_players);
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
    // DedicatedServer.enforceSecureProfile is true only for a server that can check the keys
    // players sign their chat with. This one cannot yet, and says so rather than pretend.
    if (s.online_mode && s.enforce_secure_profile)
        LOG_INFO("enforce-secure-profile is set, but chat is not signed here yet: clients are told that it is not enforced");
    if (!world.dir.empty()) {
        player_dir_ = world.dir + "/playerstate";
        std::error_code ec;
        std::filesystem::create_directories(player_dir_, ec);
        if (ec) {
            LOG_ERROR("cannot make %s: %s - nothing of a player will be kept", player_dir_.c_str(), ec.message().c_str());
            player_dir_.clear();
        }
    }
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
        // MinecraftServer.runServer: a tick of a sprint is not waited for - and the first loop
        // after the last of them is where the sprint ends, before anything of that tick.
        const bool sprinting = rate_.sprinting() && check_should_sprint_this_tick();
        if (sprinting) next = base::now_ns();
        next += sprinting ? 0 : rate_.ns_per_tick;
        const u64 t0 = base::now_ns();
        // Same order as MinecraftServer.processPacketsAndTick, tickServer and tickChildren:
        // packets, then - if the game is not frozen - the clocks; the level - its time, the
        // scheduled ticks of blocks and of fluids, the chunks' random ticks, block and light changes go out to
        // whoever sees them (ServerChunkCache.broadcastChangedChunks), the entities are shown (ChunkMap.tick),
        // block events are sent, the entities tick (items, primed TNT; ServerPlayer.tick, which
        // is the menu), then the block entities (furnaces) - then the per-connection tick (block
        // change acks, the player's own tick), the console, and one flush per player at the end.
        entities_ticked_ = false;
        pump_chunks(t0);  // the chunks the workers have ready enter the world
        for (u32 ri = 0; ri < reactors_.size(); ++ri) drain(ri);
        if (UNLIKELY(!joining_.empty())) admit_joining();
        if (UNLIKELY(!suggestion_requests_.empty())) answer_suggestions();
        // TickRateManager.tick, the first thing of tickServer: a `/tick freeze` among this tick's
        // packets holds this tick already, one from the console (below) the next. What a frozen
        // game leaves out is what ServerLevel.tick and Level.tickBlockEntities ask runsNormally()
        // for; a player is no frozen entity (isEntityFrozen), and nothing rides one here.
        rate_.tick();
        const bool runs = rate_.runs_normally();
        tick_clocks(runs);  // MinecraftServer.tickChildren: the clocks, then (every 20th tick) the time to everyone
        const u64 t_blocks = base::now_ns();
        if (LIKELY(runs)) {
            // ServerLevel.tick: tickTime - what packets scheduled was scheduled by the tick before -
            // then the block ticks that are due, the fluids' after them, then
            // (ServerChunkCache.tickChunks) the random ticks.
            world_.set_time(++time_);
            run_block_ticks();
            run_fluid_ticks();
            run_random_ticks();
        }
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
        if (LIKELY(runs)) run_block_events();  // ...block events after the chunks and the tracker, before the entities
        const u64 t_entities = base::now_ns();
        if (LIKELY(runs)) {
            tick_entities();
            // The pass is done: counted at once, so that whoever asks afterwards - a player that
            // picks an item up, in this loop or in one of a game that stands - counts the ticks
            // an entity has had.
            ++entity_tick_;
            entities_ticked_ = false;
        }
        move_ns += base::now_ns() - t_entities;
        sync_menus();
        if (LIKELY(runs)) {
            tick_furnaces();  // block entities tick after the entities
            if (UNLIKELY(!editing_signs_.empty())) tick_signs();
        }
        tick_players();
        // DedicatedServer.tickConnection: the console's lines after the connections' tick - so
        // what a line changes is shown by the next tick, and a `/tick` typed there is that tick's.
        run_console();
        autosave_players();
        if (UNLIKELY((tick_ + 1) % ticks_of(s_.autosave_seconds) == 0)) save_time();  // (with the autosave: what time it is)
        // Not t0: a player who joined during drain() has keepalive_sent_ns > t0, and the unsigned
        // difference would wrap and send a keep-alive at once. Looked at once a second.
        if (++tick_ % ticks_of(1) == 0) keepalives(base::now_ns());
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
        u64& sample = tick_times_[tick_ % kTickSamples];  // MinecraftServer.tickServer, "tallying"
        tick_times_sum_ += took - sample;
        sample = took;
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
                static_cast<unsigned long long>(world_.block_ticks().count() + world_.fluid_ticks().count()),
                static_cast<double>(world_.memory_bytes()) / (1024.0 * 1024.0));
            busy_ns = worst_ns = track_ns = move_ns = flush_ns = block_ns = sent_bytes = 0;
        }

        const u64 now = base::now_ns();
        if (UNLIKELY(sprinting)) {
            rate_.sprint_time_spent += now - rate_.sprint_tick_start;  // ServerTickRateManager.endTickWork
        } else if (now < next) {
            timespec ts{static_cast<time_t>(next / base::kNsPerSec), static_cast<long>(next % base::kNsPerSec)};
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
        } else if (now - next > base::kNsPerSec) {
            next = now;  // fell more than a second behind: do not try to replay the backlog
        }
    }
    while (!players_.empty()) kick(static_cast<u32>(players_.size()) - 1, "Server closed");
    for (const Joining& j : joining_) refuse(j.reactor, j.h, j.t, "Server closed");
    joining_.clear();  // (their files, when they come, find nobody waiting)
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
    // Whoever is here under this profile makes way - and is saved as it goes, which is what the
    // newcomer is then given (Files answers from memory while a write is out).
    for (u32 i = 0; i < players_.size(); ++i) {
        if (players_[i].uuid == j.uuid) {
            kick(i, "You logged in from another location");
            break;
        }
    }
    for (usize i = 0; i < joining_.size(); ++i) {
        if (joining_[i].info.uuid == j.uuid) {
            refuse(joining_[i].reactor, joining_[i].h, joining_[i].t, "You logged in from another location");
            joining_.erase(joining_.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    if (players_.size() + joining_.size() >= s_.max_players) {
        refuse(ri, h, t, "The server is full!");
        return;
    }
    if (!keeps_players()) {
        finish_join(ri, h, t, j, nullptr);
        return;
    }
    // PlayerList.loadPlayerData: its file first - a worker reads it, and the player is one from
    // the tick in which it has come back (the next one, as a rule).
    const u64 id = ++join_seq_;
    const std::string path = player_path(j.uuid);
    joining_.push_back({id, ri, h, t, std::move(j), false, false, {}});
    files_.read(path, this, id, &player_file_read);
}

std::string Game::player_path(Uuid uuid) const { return player_dir_ + "/" + text::uuid_string(uuid) + ".bin"; }

// A player's file has been read (on the tick thread: from Workers::poll, or from Files::read
// itself). Only noted here: whoever calls is in the middle of something.
void Game::player_file_read(void* game, u64 id, std::span<const u8> bytes, bool found) {
    Game& g = *static_cast<Game*>(game);
    for (Joining& j : g.joining_) {
        if (j.id != id) continue;
        j.ready = true;
        j.has_data = found && read_player(bytes, j.data);
        // A file that cannot be read is not half taken: the player starts anew, and the file is
        // replaced when it is next saved.
        if (found && !j.has_data)
            LOG_WARN("%s cannot be read: %s joins as a new player", g.player_path(j.info.uuid).c_str(), j.info.name.c_str());
        return;
    }
    // Nobody: gone, or pushed out by a later login, while its file was read.
}

// Those whose files have come become players, in the order they came to the server in.
void Game::admit_joining() {
    usize kept = 0;
    for (usize i = 0; i < joining_.size(); ++i) {
        if (!joining_[i].ready) {
            if (kept != i) joining_[kept] = std::move(joining_[i]);
            ++kept;
            continue;
        }
        Joining j = std::move(joining_[i]);
        finish_join(j.reactor, j.h, j.t, j.info, j.has_data ? &j.data : nullptr);
    }
    joining_.resize(kept);
}

// `saved`: what is kept of this player from its last visit, null for a first one.
void Game::finish_join(u32 ri, const InHeader& h, const net::JoinTransport& t, JoinInfo& j, PlayerData* saved) {
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
    p.saved_tick = tick_;
    const double spawn_y = spawn_[1];
    // ServerPlayer.load: where it was and how it looked about, or the world's spawn.
    const double x = saved ? saved->x : spawn_[0], y = saved ? saved->y : spawn_y, z = saved ? saved->z : spawn_[2];
    const float yaw = saved ? saved->yaw : 0.0f, pitch = saved ? saved->pitch : 0.0f;
    // The entity: a player's row in the entity table is its index here (the store keeps players
    // in front). Whoever is near is shown it by the tracker at the end of this tick, after the
    // tab-list entry below - a client drops an add_entity for a player it has no entry for.
    entity::Store::Moves moved;
    const u32 ei = tracker_.spawn(entity_type::kPlayer, true, p.uuid, x, y, z, yaw, pitch, moved);
    DCHECK(ei == pi);
    (void)ei;
    enter_push_grid(ents_.slot[pi], x, z, entity_type::kInfo[entity_type::kPlayer].width);
    ents_.set_skin_parts(pi, j.skin_parts);  // ServerPlayer.updateOptions
    ents_.set_main_hand(pi, j.main_hand);
    pcx_.push_back(chunk_of(x));
    pcz_.push_back(chunk_of(z));
    // ChunkMap.getPlayerViewDistance: what the client asked for, within 2 and the server's own.
    pview_.push_back(std::min<i32>(std::max<i32>(j.view_distance, 2), static_cast<i32>(s_.view_distance)));
    tracker_.set_view_distance(pi, static_cast<u32>(pview_[pi]));
    senders_.emplace_back();
    ack_.push_back(-1);
    chat_index_.push_back(0);
    chat_visibility_.push_back(j.chat_visibility);
    dirty_.push_back(0);
    out_.push_back(pool_.get(kOutInitial));
    slot_to_player_[ri][h.slot] = pi;
    online_.store(static_cast<u32>(players_.size()), std::memory_order_relaxed);
    if (saved) {
        if (saved->on_ground) ents_.flags[pi] |= entity::flag::kOnGround;
        p.flying = saved->flying;
        for (u32 i = 0; i < inv::kSize; ++i) p.menu.inv.items[i] = std::move(saved->items[i]);
        p.menu.inv.selected = saved->selected;
        show_equipment(pi);  // nobody sees it yet: what it wears is known before anybody does
    }
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

    send(pi,
         cb::PlayerAbilities{static_cast<u8>(cb::PlayerAbilities::kInvulnerable | cb::PlayerAbilities::kCanFly |
                                             cb::PlayerAbilities::kInstabuild | (p.flying ? cb::PlayerAbilities::kFlying : 0))});
    send(pi, cb::SetHeldSlot{p.menu.inv.selected});
    // What a client's screens ask about recipes: which items a furnace takes (the shift-click
    // it predicts), and so on. The recipe book stays empty: nothing is unlocked.
    out(pi).append(recipes_frame_.data, recipes_frame_.len);
    send_permission_level(pi);  // its permission level, and the commands it may use
    // "Steve joined the game", to those who are here: vanilla says it before the newcomer is
    // one of the players it speaks to.
    broadcast_system(Text::translatable("multiplayer.player.joined").arg(display_name(pi)).color(text::Color::kYellow), pi);

    p.teleport_id = 1;
    p.awaiting_teleport = true;
    send(pi, cb::PlayerPosition{p.teleport_id, ents_.x[pi], ents_.y[pi], ents_.z[pi], 0, 0, 0, yaw, pitch, 0});

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

    send_clocks(pi);  // PlayerList.sendLevelInfo: every clock, then the spawn
    send(pi,
         cb::SetDefaultSpawnPosition{kOverworld, floor_int(spawn_[0]), static_cast<i32>(spawn_y), floor_int(spawn_[2]), 0, 0});
    send(pi, cb::GameEvent{cb::GameEvent::kLevelChunksLoadStart, 0});
    // ServerTickRateManager.updateJoiningPlayer, at the end of sendLevelInfo: how fast the world
    // ticks, whether it stands still, and how many ticks it is still to step.
    send(pi, cb::TickingState{rate_.rate, rate_.frozen});
    send(pi, cb::TickingStep{static_cast<u32>(rate_.frozen_ticks_to_run)});
    // ServerLevel.addNewPlayer -> ChunkMap.updatePlayerStatus: the view is the player's from here
    // on. Its chunks go out batch by batch, from the end of this tick on (send_chunks).
    apply_view(pi, pcx_[pi], pcz_[pi], pview_[pi]);
    // ServerPlayer.initInventoryMenu: the whole menu.
    p.menu.send_all(out(pi), p.threshold);
}

void Game::on_leave(u32 ri, const InHeader& h) {
    const u32 pi = slot_to_player_[ri][h.slot];
    if (pi != kNone && players_[pi].gen == h.gen) {
        LOG_INFO("%s left", players_[pi].name.c_str());
        remove_player(pi);
    }
    // Gone before its file was read: it never was a player.
    std::erase_if(joining_, [&](const Joining& j) { return j.reactor == ri && j.h.slot == h.slot && j.h.gen == h.gen; });
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
    // removePlayerFromWorld: "Steve left the game" first.
    broadcast_system(Text::translatable("multiplayer.player.left").arg(display_name(pi)).color(text::Color::kYellow), pi);
    // PlayerList.remove: the player is saved, then taken out of the level - and Player.remove
    // takes its menus away: what was on a cursor or in a crafting grid falls where it stood,
    // and a chest loses an opener.
    save_player(pi);
    const bool chest_open = p.menu.kind() == Menu::Kind::kChest;
    p.menu.leave(effects_);
    apply_effects(pi);
    if (chest_open) {
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
        chat_index_[pi] = chat_index_[last];
        chat_visibility_[pi] = chat_visibility_[last];
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
    chat_index_.pop_back();
    chat_visibility_.pop_back();
    dirty_.pop_back();
    out_.pop_back();
    online_.store(static_cast<u32>(players_.size()), std::memory_order_relaxed);
    broadcast(cb::PlayerInfoRemove{uuid});
}

// What is kept of a player, to its file - unless that holds these very bytes already.
void Game::save_player(u32 pi) {
    if (!keeps_players()) return;
    Player& p = players_[pi];
    p.saved_tick = tick_;
    PlayerData d;
    d.x = ents_.x[pi];
    d.y = ents_.y[pi];
    d.z = ents_.z[pi];
    d.yaw = ents_.yaw[pi];
    d.pitch = ents_.pitch[pi];
    d.on_ground = ents_.flags[pi] & entity::flag::kOnGround;
    d.flying = p.flying;
    d.selected = p.menu.inv.selected;
    for (u32 i = 0; i < inv::kSize; ++i) d.items[i] = p.menu.inv.items[i];
    player_scratch_.clear();
    write_player(player_scratch_, d);
    u64 hash = 0xCBF29CE484222325ull;  // FNV-1a: only ever compared with the one before
    for (u32 i = 0; i < player_scratch_.len; ++i) hash = (hash ^ player_scratch_.data[i]) * 0x100000001B3ull;
    hash |= 1;  // (0: never saved)
    if (hash == p.saved_hash) return;
    p.saved_hash = hash;
    files_.write(player_path(p.uuid), {player_scratch_.data, player_scratch_.len});
}

// PlayerList.saveAll, spread out: one player a tick is looked at, and saved when its last save
// is autosave-seconds old. (The cold Player is touched for one of them, not for all.)
void Game::autosave_players() {
    if (players_.empty() || !keeps_players()) return;
    if (++autosave_at_ >= players_.size()) autosave_at_ = 0;
    if (tick_ - players_[autosave_at_].saved_tick >= ticks_of(s_.autosave_seconds)) save_player(autosave_at_);
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
        case sbid::kSignUpdate: {  // handleSignUpdate
            const auto u = sb::SignUpdate::decode(rd);
            if (rd.done()) on_sign_update(pi, u);
            break;
        }
        case sbid::kSetCreativeModeSlot: {
            const auto c = sb::SetCreativeModeSlot::decode(rd);
            ItemStack stack;
            // A heavy stack costs the tick thread its reading: more of them than a client may
            // send in a while is refused unread - as a stack that is not read is.
            if (rd.ok && rd.remaining() > item::kHeavyPatchBytes && !admit_heavy(p, static_cast<u32>(rd.remaining()))) {
                LOG_DEBUG("%s sends heavy item stacks too fast, one refused", p.name.c_str());
                on_creative_slot(pi, c.slot, {}, true);
                break;
            }
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
        case sbid::kUseItem: {
            const auto u = sb::UseItem::decode(rd);
            if (rd.ok) on_use_item(pi, u);
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
            if (rd.done()) on_chat_command(pi, c);  // (may kick: nothing of `p` after this)
            break;
        }
        case sbid::kChat: {
            const auto c = sb::Chat::decode(rd);
            if (rd.done()) on_chat(pi, c);  // (may kick)
            break;
        }
        case sbid::kChatAck: {
            const auto a = sb::ChatAck::decode(rd);
            if (rd.done()) on_chat_ack(pi, a.offset);  // (may kick)
            break;
        }
        case sbid::kCommandSuggestion: {
            const auto c = sb::CommandSuggestion::decode(rd);
            if (rd.done()) on_command_suggestion(pi, c);
            break;
        }
        case sbid::kChatSessionUpdate:
            // handleChatSessionUpdate, on a server that has no Services key to check a player's
            // key with: "Ignoring chat session ... due to missing Services public key". Its
            // messages then count as unsigned, here as there.
            sb::ChatSessionUpdate::decode(rd);
            break;
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
