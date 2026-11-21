// SPDX-License-Identifier: GPL-3.0-or-later
// Chunks and the players around them: whose view holds which chunk, when a chunk goes out to a
// client, which chunks have to be in the world for that - and what becomes of one that nobody
// needs any more.
//
// The rules a client can see are vanilla's, read from the 26.3 jar; each function names the
// method it follows:
//   ChunkTrackingView (Positioned, isWithinDistance, difference), ChunkMap.updateChunkTracking /
//   applyChunkTrackingView / isChunkTracked / isChunkOnTrackedBorder / onChunkReadyToSend
//   PlayerChunkSender (markChunkPendingToSend, dropChunk, sendNextChunks, collectChunksToSend,
//   onChunkBatchReceivedByClient), MinecraftServer.tickChildren ("send chunks")
//
// What a client cannot see is this server's own:
//   * Which chunks are loaded. A view's chunks tick, for which the chunks around them are lit,
//     for which the chunks around those are loaded: the view widened by two (kLoadMargin). Vanilla
//     has ticket levels that reach a dozen chunks further; nothing here generates terrain that
//     would need them.
//   * When a chunk leaves. The chunks are walked a few a tick (ChunkMap::scan), all of them in
//     half of chunk-unload-seconds (10 by default); one that the walk has found in nobody's
//     widened view for that long is saved - its blocks and light, the block entities in it, the
//     entities standing in it - and unloaded.
//   * When a chunk that stays is saved: when the same walk finds that it changed and was last
//     written autosave-seconds ago or more (300: vanilla's autosave period, without the wave of
//     writes every five minutes). At most that much is lost if the machine stops.
//   * A frame is deflated by a worker (world::Dimension::frame_if_ready), so a chunk goes out a
//     tick or two after it is due. The order and the pacing are PlayerChunkSender's.
#include <algorithm>

#include "base/clock.hpp"
#include "base/log.hpp"
#include "server/game.hpp"
#include "server/viewers.hpp"

namespace server {

using namespace proto;

namespace {

// How long into a tick chunks that the workers finished are still taken into the world; the rest
// wait a tick. Chosen without a measurement: a fifth of a tick.
constexpr u64 kAdoptBudgetNs = 10 * base::kNsPerMs;
// PlayerChunkSender
constexpr float kMinChunksPerTick = 0.01f, kMaxChunksPerTick = 64.0f;
constexpr i32 kMaxUnacknowledgedBatches = 10;

u64 chunk_key(i32 cx, i32 cz) { return (static_cast<u64>(static_cast<u32>(cx)) << 32) | static_cast<u32>(cz); }

}  // namespace

// ---------------------------------------------------------------------------- views

// ChunkMap.isChunkTracked: the chunk is in the player's view and is not waiting to be sent.
bool Game::chunk_tracked(u32 pi, i32 cx, i32 cz) const {
    return in_view(pcx_[pi], pcz_[pi], pview_[pi], cx, cz) && !senders_[pi].test(cx, cz);
}

// ChunkMap.updateChunkTracking + applyChunkTrackingView: the player's view becomes the one
// around (cx, cz) with this view distance. The client is told the new centre; what left the view
// is forgotten (unless it was still waiting to be sent), what entered it and can be sent is
// marked - what cannot be yet is marked when it can (chunk_ready). And the chunks the new view
// needs are asked for.
void Game::apply_view(u32 pi, i32 cx, i32 cz, i32 distance) {
    ChunkSender& cs = senders_[pi];
    const bool had = cs.has_view;
    const i32 ocx = pcx_[pi], ocz = pcz_[pi], od = pview_[pi];
    if (had && ocx == cx && ocz == cz && od == distance) return;
    if (!had || ocx != cx || ocz != cz) {
        send(pi, cb::SetChunkCacheCenter{cx, cz});
        move_simulation(had, ocx, ocz, true, cx, cz);
    }

    u64 bits[ChunkSender::kWords] = {};
    const i32 x0 = cx - distance - 1, z0 = cz - distance - 1;
    const u32 side = static_cast<u32>(2 * (distance + 1) + 1);
    u32 pending = 0;
    const auto mark = [&](i32 x, i32 z) {
        const u32 i = static_cast<u32>(z - z0) * side + static_cast<u32>(x - x0);
        bits[i >> 6] |= 1ull << (i & 63);
        ++pending;
    };
    const auto enter = [&](i32 x, i32 z) {  // markChunkPendingToSend(player, pos): only what getChunkToSend has
        const world::Chunk* c = world_.find(x, z);
        if (c && c->ticking) mark(x, z);
    };
    const auto leave = [&](i32 x, i32 z) {  // PlayerChunkSender.dropChunk
        if (!cs.test(x, z)) send(pi, cb::ForgetLevelChunk{x, z});
    };
    // ChunkTrackingView.difference: over both squares at once where they touch, x outermost.
    if (had && std::abs(ocx - cx) <= od + distance + 2 && std::abs(ocz - cz) <= od + distance + 2) {
        const i32 min_x = std::min(ocx - od, cx - distance) - 1, max_x = std::max(ocx + od, cx + distance) + 1;
        const i32 min_z = std::min(ocz - od, cz - distance) - 1, max_z = std::max(ocz + od, cz + distance) + 1;
        for (i32 x = min_x; x <= max_x; ++x) {
            for (i32 z = min_z; z <= max_z; ++z) {
                const bool saw = in_view(ocx, ocz, od, x, z), sees = in_view(cx, cz, distance, x, z);
                if (saw && sees) {
                    if (cs.test(x, z)) mark(x, z);
                } else if (sees) {
                    enter(x, z);
                } else if (saw) {
                    leave(x, z);
                }
            }
        }
    } else {
        if (had) {
            for (i32 x = ocx - od - 1; x <= ocx + od + 1; ++x)
                for (i32 z = ocz - od - 1; z <= ocz + od + 1; ++z)
                    if (in_view(ocx, ocz, od, x, z)) leave(x, z);
        }
        for (i32 x = cx - distance - 1; x <= cx + distance + 1; ++x)
            for (i32 z = cz - distance - 1; z <= cz + distance + 1; ++z)
                if (in_view(cx, cz, distance, x, z)) enter(x, z);
    }
    cs.has_view = true;
    cs.x0 = x0, cs.z0 = z0, cs.side = side;
    cs.pending = pending;
    std::memcpy(cs.bits, bits, sizeof bits);
    pcx_[pi] = cx;
    pcz_[pi] = cz;
    pview_[pi] = distance;
    tracker_.set_view_distance(pi, static_cast<u32>(distance));

    // What has to be in the world for this view: a request for a chunk that is there, or on its
    // way, is two lookups. Ring by ring from the middle - the workers take them in this order, and
    // the chunk the player stands in is the first that can tick.
    const i32 reach = distance + 1 + (kLoadMargin - kViewMargin);
    world_.request(cx, cz);
    for (i32 ring = 1; ring <= reach; ++ring) {
        for (i32 d = -ring; d <= ring; ++d) {
            const i32 at[4][2] = {{cx + d, cz - ring}, {cx + d, cz + ring}, {cx - ring, cz + d}, {cx + ring, cz + d}};
            for (const auto& [x, z] : at)
                if (in_view(cx, cz, distance, x, z, kLoadMargin)) world_.request(x, z);
        }
    }
}

// DistanceManager.addPlayer / removePlayer, as ChunkMap.move calls them: a player's simulation
// ticket is at the chunk it is in. The chunks within `simulation-distance` of it (a square) are
// simulated - random ticks, as vanilla's entity-ticking level - and those one chunk further still
// run their scheduled ticks (block-ticking). Counted per chunk, a player at a time, when it
// crosses into another chunk: nothing is worked out per tick.
void Game::move_simulation(bool had, i32 old_cx, i32 old_cz, bool has, i32 cx, i32 cz) {
    const i32 r = static_cast<i32>(s_.simulation_distance);
    const auto visit = [&](i32 x0, i32 x1, i32 z0, i32 z1) {
        for (i32 z = z0; z <= z1; ++z) {
            for (i32 x = x0; x <= x1; ++x) {
                const i32 before = had ? std::max(std::abs(x - old_cx), std::abs(z - old_cz)) : INT32_MAX;
                const i32 after = has ? std::max(std::abs(x - cx), std::abs(z - cz)) : INT32_MAX;
                const i32 sim = (after <= r) - (before <= r), block = (after <= r + 1) - (before <= r + 1);
                if (!(sim | block)) continue;
                if (world::Chunk* c = world_.find(x, z)) world_.simulate(*c, sim, block);
            }
        }
    };
    const i32 reach = r + 1;
    if (had && has && std::abs(old_cx - cx) <= 2 * reach && std::abs(old_cz - cz) <= 2 * reach) {
        visit(std::min(old_cx, cx) - reach, std::max(old_cx, cx) + reach, std::min(old_cz, cz) - reach,
              std::max(old_cz, cz) + reach);
    } else {
        if (had) visit(old_cx - reach, old_cx + reach, old_cz - reach, old_cz + reach);
        if (has) visit(cx - reach, cx + reach, cz - reach, cz + reach);
    }
}

// A chunk that has just entered the world: the players that have it within their simulation distance.
void Game::simulate_adopted(world::Chunk& c) {
    const i32 r = static_cast<i32>(s_.simulation_distance);
    i32 sim = 0, block = 0;
    for (u32 pi = 0; pi < players_.size(); ++pi) {
        if (!senders_[pi].has_view) continue;
        const i32 d = std::max(std::abs(c.x - pcx_[pi]), std::abs(c.z - pcz_[pi]));
        sim += d <= r;
        block += d <= r + 1;
    }
    if (sim | block) world_.simulate(c, sim, block);
}

// ChunkMap.onChunkReadyToSend: a chunk began to tick - whoever has it in view is to be sent it.
void Game::chunk_ready(world::Chunk& c) {
    viewers_.clear();
    find_in_view(pcx_.data(), pcz_.data(), pview_.data(), static_cast<u32>(players_.size()), c.x, c.z, kViewMargin, viewers_);
    for (const u32 pi : viewers_) {
        ChunkSender& cs = senders_[pi];
        if (!cs.has_view || cs.test(c.x, c.z)) continue;
        const u32 i = static_cast<u32>(c.z - cs.z0) * cs.side + static_cast<u32>(c.x - cs.x0);
        cs.bits[i >> 6] |= 1ull << (i & 63);
        ++cs.pending;
    }
}

// PlayerChunkSender.onChunkBatchReceivedByClient
void Game::on_chunk_batch_received(u32 pi, float desired) {
    ChunkSender& cs = senders_[pi];
    if (--cs.unacked < 0) cs.unacked = 0;
    cs.desired = std::isnan(desired) ? kMinChunksPerTick : std::clamp(desired, kMinChunksPerTick, kMaxChunksPerTick);
    if (cs.unacked == 0) cs.quota = 1.0f;
    cs.max_unacked = kMaxUnacknowledgedBatches;
}

// PlayerChunkSender.sendNextChunks for every player: the last thing a tick does before it
// flushes. A batch is the nearest of what waits, as many as the client's pace allows; a chunk
// whose frame a worker has not finished stays for the next tick.
void Game::send_chunks() {
    const u32 n = static_cast<u32>(players_.size());
    for (u32 pi = 0; pi < n; ++pi) {
        ChunkSender& cs = senders_[pi];
        if (cs.unacked >= cs.max_unacked) continue;
        const float max_batch = std::max(1.0f, cs.desired);
        cs.quota = std::min(cs.quota + cs.desired, max_batch);
        if (cs.quota < 1.0f || cs.pending == 0) continue;
        // Nothing goes out before the chunk the player is in: a client waits for that one, and
        // vanilla's loading order (nearest first, strictly) amounts to the same.
        const world::Chunk* own = world_.find(pcx_[pi], pcz_[pi]);
        if (!own || !own->ticking) continue;
        const bool own_pending = cs.test(pcx_[pi], pcz_[pi]);

        // collectChunksToSend: all of them if they fit the batch, else the nearest that do
        // (ChunkPos.distanceSquared from the chunk the player is in).
        const u32 batch = static_cast<u32>(cs.quota);
        nearest_.clear();
        for (u32 w = 0; w < ChunkSender::kWords; ++w) {
            for (u64 m = cs.bits[w]; m; m &= m - 1) {
                const u32 i = w * 64 + static_cast<u32>(std::countr_zero(m));
                const i32 x = cs.x0 + static_cast<i32>(i % cs.side), z = cs.z0 + static_cast<i32>(i / cs.side);
                const i32 dx = x - pcx_[pi], dz = z - pcz_[pi];
                nearest_.push_back({dx * dx + dz * dz, x, z});
            }
        }
        // The batch, and as many again behind it: those have their frames made for the next tick.
        const usize look = std::min<usize>(nearest_.size(), 2ull * batch);
        const auto nearer = [](const Nearest& a, const Nearest& b) { return a.d2 < b.d2; };
        std::partial_sort(nearest_.begin(), nearest_.begin() + static_cast<std::ptrdiff_t>(look), nearest_.end(), nearer);
        base::ByteBuf& o = out(pi);
        const i32 threshold = players_[pi].threshold;
        u32 sent = 0;
        bool hold = false;
        for (usize k = 0; k < look; ++k) {
            const Nearest& at = nearest_[k];
            world::Chunk* c = world_.find(at.x, at.z);
            if (!c || !c->ticking) continue;  // getChunkToSend: null - it stays pending
            const std::span<const u8> frame = world_.frame_if_ready(*c);
            if (k == 0 && own_pending && frame.empty()) hold = true;  // the nearest is the player's own: wait for it
            if (hold || k >= batch || frame.empty()) continue;
            if (sent == 0) write_packet(o, threshold, cb::ChunkBatchStart{});
            o.append(frame.data(), frame.size());  // one cached frame per chunk, whoever asks
            ++sent;
            const u32 i = static_cast<u32>(at.z - cs.z0) * cs.side + static_cast<u32>(at.x - cs.x0);
            cs.bits[i >> 6] &= ~(1ull << (i & 63));
        }
        if (sent == 0) continue;
        ++cs.unacked;
        write_packet(o, threshold, cb::ChunkBatchFinished{sent});
        cs.quota -= static_cast<float>(sent);
        cs.pending -= sent;
    }
}

// ---------------------------------------------------------------------------- coming and going

// What the workers have finished enters the world; what those chunks held of the game's - block
// entities, entities - is made again; who waits for a chunk that now ticks is marked for it.
void Game::pump_chunks(u64 tick_start_ns) {
    world_.pump(tick_, tick_start_ns ? tick_start_ns + kAdoptBudgetNs : 0);
    for (const world::Dimension::Adopted& a : world_.adopted()) {
        const bool dirty = a.chunk->dirty;
        read_block_entities(*a.chunk, a.from->parts.block_entities);
        read_entities(*a.chunk, a.from->parts.entities);
        a.chunk->dirty = dirty;  // what it came with is what is on disk
        simulate_adopted(*a.chunk);
    }
    world_.release_adopted();
    for (world::Chunk* c : world_.ready()) chunk_ready(*c);
    world_.ready().clear();
}

// One chunk to disk - if anything about it changed, or anything stands in it - and, when it
// leaves, what the game holds for it out of the game.
void Game::save_chunk(world::Chunk& c, bool leaving) {
    const u64 key = chunk_key(c.x, c.z);
    const auto first =
        std::lower_bound(entities_at_.begin(), entities_at_.end(), key, [](const EntityAt& e, u64 k) { return e.chunk < k; });
    const bool has_entities = first != entities_at_.end() && first->chunk == key;
    if (c.dirty || has_entities || c.saved_entities) {
        part_blocks_.clear();
        part_entities_.clear();
        write_block_entities(c, part_blocks_);
        write_entities(key, part_entities_);
        c.saved_entities = has_entities;
        world_.save(c, {{part_blocks_.data, part_blocks_.len}, {part_entities_.data, part_entities_.len}});
    } else {
        c.saved_tick = tick_;
    }
    if (!leaving) return;
    for (auto it = first; it != entities_at_.end() && it->chunk == key; ++it) remove_entity(it->slot);
    drop_block_entities(c);
}

// ChunkMap.tick's unloading and saving, a few chunks a tick. Called where no list of this tick
// names a chunk: right after the block and light changes have gone out.
void Game::tend_chunks() {
    if (!world_.persistent()) return;  // a world that is not kept: nothing leaves, there is nowhere to
    const u32 n = static_cast<u32>(players_.size());
    leaving_.clear();
    saving_.clear();
    const u64 idle_ticks = 20ull * s_.chunk_unload_seconds, save_every = 20ull * s_.autosave_seconds;
    const u64 walk_ticks = std::max<u64>(1, idle_ticks / 2);
    world_.scan(static_cast<u32>(world_.chunk_count() / walk_ticks) + 1, [&](world::Chunk& c) {
        const bool wanted = any_in_view(pcx_.data(), pcz_.data(), pview_.data(), n, c.x, c.z, kLoadMargin);
        if (wanted) {
            c.idle_tick = 0;
        } else if (c.idle_tick == 0) {
            c.idle_tick = tick_ + 1;  // (+ 1: 0 says "wanted")
        } else if (tick_ + 1 - c.idle_tick >= idle_ticks) {
            leaving_.push_back(&c);
            return;
        }
        if (tick_ - c.saved_tick >= save_every) saving_.push_back(&c);
    });
    if (leaving_.empty() && saving_.empty()) return;
    for (world::Chunk* c : leaving_) c->unloading = true;
    for (world::Chunk* c : saving_) c->unloading = true;
    collect_entities(false);
    for (world::Chunk* c : saving_) {
        save_chunk(*c, false);
        c->unloading = false;
    }
    for (world::Chunk* c : leaving_) {
        save_chunk(*c, true);
        world_.unload(*c);
    }
    if (!leaving_.empty()) LOG_DEBUG("%zu chunk(s) left the world, %zu are loaded", leaving_.size(), world_.chunk_count());
    // The chunks around one that left no longer tick. An item at rest in one of them would go on
    // ageing (its tick at rest does not ask where it is): it is ticked in full again, which for
    // a chunk that does not tick means not at all.
    if (!world_.stopped().empty() && asleep_ != asleep_living_) {
        for (u32 row = n; row < ents_.size(); ++row) {
            const u32 slot = ents_.slot[row];
            if (slot >= movers_.size() || !movers_[slot].asleep) continue;
            const world::Chunk* c = world_.find(chunk_of(ents_.x[row]), chunk_of(ents_.z[row]));
            if (c && !c->ticking) wake(slot);
        }
    }
    world_.stopped().clear();
}

// Everything that is loaded goes to disk, and the caller waits until it is there: the server stops.
void Game::save_world() {
    save_time();
    files_.finish();  // the players' files, which leaving has just written, and world.json
    if (!world_.persistent()) return;
    const u64 t0 = base::now_ns();
    saving_.clear();
    world_.for_each([&](world::Chunk& c) { saving_.push_back(&c); });
    collect_entities(true);
    const u64 before = world_.store().chunks_written() + world_.store().waiting();
    for (world::Chunk* c : saving_) save_chunk(*c, false);
    const u64 queued = world_.store().chunks_written() + world_.store().waiting() - before;
    world_.store().finish();
    LOG_INFO("world saved: %zu chunk(s) loaded, %llu written now, %.0f ms", saving_.size(),
             static_cast<unsigned long long>(queued), static_cast<double>(base::now_ns() - t0) / 1e6);
}

}  // namespace server
