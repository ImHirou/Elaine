// SPDX-License-Identifier: GPL-3.0-or-later
// Entity benchmarks: what a tick of the entity tracker costs at the scale the server is built
// for, and where that cost goes. Every scenario runs the real Store + Tracker against one output
// buffer per player - everything but the sockets.
//
// The scenarios:
//   crowd      500 players inside one another's view, all walking and turning every tick. The
//              worst case: 249 500 (viewer, entity) pairs, every one with news every other tick.
//   spread     the same 500 spread over a 5 km square: a handful of neighbours each. The usual case.
//   mobs       spread players plus 20 000 mobs, a third of them walking. What the design has to
//              survive once mobs exist.
// For each: milliseconds per tick, bytes per tick, and the same bytes through AES-CFB8 as the
// flush would do in online mode - the number that says where the time really goes.
#include <cmath>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "bench.hpp"

#include "crypto/cfb8.hpp"
#include "entity/store.hpp"
#include "entity/tracker.hpp"
#include "entity/visibility.hpp"

namespace benchmark {

namespace {

using entity::Store;
using entity::Tracker;
namespace flag = entity::flag;
namespace et = proto::gen::entity_type;

struct Sim {
    Store store;
    Tracker tracker{store, 256};
    std::vector<std::unique_ptr<base::ByteBuf>> bufs;
    std::vector<base::ByteBuf*> outs;
    std::vector<crypto::Cfb8> ciphers;
    std::vector<crypto::Cfb8Job> jobs;
    std::vector<double> cx, cz;  // where each entity circles around
    u64 bytes = 0;
    u32 tick_no = 0;

    u32 add(u32 type, bool player, double x, double z, u32 view = 10) {
        Store::Moves mv;
        const u32 e = tracker.spawn(type, player, Uuid{1, store.size() + 1ull}, x, 64, z, 0, 0, mv);
        if (mv.n) {  // a mob gave its row to this player
            cx.push_back(cx[mv.m[0].from]);
            cz.push_back(cz[mv.m[0].from]);
            cx[e] = x;
            cz[e] = z;
        } else {
            cx.push_back(x);
            cz.push_back(z);
        }
        if (player) {
            bufs.push_back(std::make_unique<base::ByteBuf>(1 << 16));
            outs.push_back(bufs.back().get());
            tracker.set_view_distance(e, view);
            static const u8 key[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
            ciphers.emplace_back().init(key);
            store.flags[e] |= flag::kOnGround;
        }
        return e;
    }

    // What the packets of one tick do: every player (and every `mob_every`-th mob) takes a step
    // on its circle; players also turn, as a walking client does. Steps come from a table: the
    // benchmark is of the tracker, not of sin().
    static constexpr u32 kSteps = 64;
    double step_x[kSteps], step_z[kSteps];
    float step_yaw[kSteps];
    Sim() {
        for (u32 i = 0; i < kSteps; ++i) {
            const double a = i * (6.283185307179586 / kSteps);
            step_x[i] = std::cos(a) * 2.0;
            step_z[i] = std::sin(a) * 2.0;
            step_yaw[i] = static_cast<float>(i * (360.0 / kSteps) - 180.0);
        }
    }
    void input(u32 mob_every) {
        const u32 np = store.players(), n = store.size();
        for (u32 e = 0; e < np; ++e) {
            const u32 i = (tick_no + e * 7) % kSteps;
            store.x[e] = cx[e] + step_x[i];
            store.z[e] = cz[e] + step_z[i];
            store.yaw[e] = step_yaw[i];
            store.flags[e] |= flag::kMoved;
        }
        if (mob_every)
            for (u32 e = np + tick_no % mob_every; e < n; e += mob_every) {
                const u32 i = (tick_no / mob_every + e * 5) % kSteps;
                store.x[e] = cx[e] + step_x[i] * 4.0;  // a circle of 8 blocks: sections and cells get crossed
                store.z[e] = cz[e] + step_z[i] * 4.0;
            }
    }

    void tick(u32 mob_every, bool encrypt) {
        input(mob_every);
        tracker.tick(outs.data());
        const u32 np = store.players();
        std::copy_n(store.yaw, np, store.head_yaw);
        if (encrypt) {
            jobs.clear();
            for (u32 p = 0; p < np; ++p)
                if (outs[p]->len) jobs.push_back({&ciphers[p], outs[p]->data, outs[p]->len});
            if (!jobs.empty()) crypto::cfb8_encrypt_batch(jobs);
        }
        for (u32 p = 0; p < np; ++p) {
            bytes += outs[p]->len;
            outs[p]->clear();
        }
        ++tick_no;
    }
};

void scenario(const char* name, Sim& sim, u32 mob_every) {
    const std::string base = std::string("tracker tick, ") + name;
    if (!selected(base.c_str())) return;
    for (int i = 0; i < 40; ++i) sim.tick(mob_every, false);  // everyone paired, buffers at their size
    constexpr int kTicks = 200;
    sim.bytes = 0;
    u32 pairs = 0;
    for (u32 e = 0; e < sim.store.size(); ++e) pairs += sim.tracker.viewers(e);
    const double plain = best_seconds(5, [&] {
        for (int i = 0; i < kTicks; ++i) sim.tick(mob_every, false);
    });
    const double per_tick = static_cast<double>(sim.bytes) / (5.0 * kTicks);
    const double with_aes = best_seconds(3, [&] {
        for (int i = 0; i < kTicks; ++i) sim.tick(mob_every, true);
    });
    report(base.c_str(), plain / kTicks * 1e3, "ms");
    report((base + ": pairs").c_str(), pairs / 1e3, "K");
    report((base + ": sent").c_str(), per_tick / 1024.0, "KiB/tick");
    report((base + ": + AES-CFB8").c_str(), with_aes / kTicks * 1e3, "ms");
}

}  // namespace

void entity_benchmarks() {
    std::mt19937 rng(4);
    const auto uniform = [&](double span) { return (static_cast<double>(rng() % 100000) / 100000.0 - 0.5) * span; };

    // ---- the visibility kernel alone: one player's row
    for (const u32 n : {512u, 20480u}) {
        const std::string name = "visibility scan, " + std::to_string(n) + " entities";
        if (!selected(name.c_str())) continue;
        Store store;
        Store::Moves mv;
        for (u32 i = 0; i < n; ++i) store.create(et::kPig, false, Uuid{0, i}, uniform(600), 64, uniform(600), 0, 0, mv);
        std::vector<double> range2(store.capacity(), 160.0 * 160.0);
        std::vector<u64> words(store.capacity() / 64);
        u32 at = 0;
        bench(name.c_str(), 1e3, "K scans/s", [&] {
            at = (at + 1) % n;
            entity::visible_words(store.x, store.z, range2.data(), n / 64, store.x[at], store.z[at], 160.0 * 160.0, words.data());
            g_sink = words[at >> 6];
        });
    }

    {
        Sim crowd;
        for (u32 i = 0; i < 500; ++i) crowd.add(et::kPlayer, true, uniform(40), uniform(40));
        scenario("500 players in a crowd", crowd, 0);
    }
    {
        Sim spread;
        for (u32 i = 0; i < 500; ++i) spread.add(et::kPlayer, true, uniform(5000), uniform(5000));
        scenario("500 players spread out", spread, 0);
    }
    {
        Sim mobs;
        for (u32 i = 0; i < 20000; ++i) mobs.add(i % 5 ? et::kZombie : et::kItem, false, uniform(5000), uniform(5000));
        for (u32 i = 0; i < 500; ++i) mobs.add(et::kPlayer, true, uniform(5000), uniform(5000));
        scenario("500 players + 20000 mobs", mobs, 3);
    }
}

}  // namespace benchmark
