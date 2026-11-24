// SPDX-License-Identifier: GPL-3.0-or-later
// world::GenPipeline: chunks made in stages come out as the one order of decoration makes them -
// whoever asks, in whatever order, with however many workers, and whatever was thrown away and
// made again on the way.
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <thread>
#include <vector>

#include "base/clock.hpp"
#include "world/gen_pipeline.hpp"

namespace world {
namespace {

using At = std::pair<i32, i32>;

u64 mix(u64 h, u64 v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    h *= 0xff51afd7ed558ccdull;
    return h ^ (h >> 33);
}

// A generator whose features cost nothing and tell on every mistake. A chunk's terrain is eight
// numbers made of where it is; decorating a chunk reads the nine chunks around it and writes into
// each of the nine what it read, mixed with what was there. A chunk that is decorated out of
// turn, or twice, or beside the wrong version of a neighbour, comes out as other numbers - and so
// does every chunk made of it afterwards.
class Mixer final : public ChunkGenerator {
  public:
    static constexpr u32 kNumbers = 8;

    bool staged() const override { return true; }
    void make_base(i32 cx, i32 cz, GenChunk& out) override {
        out.x = cx;
        out.z = cz;
        out.sections_count = 1;
        out.version = 0;
        out.decorated = false;
        for (u32 k = 0; k < kNumbers; ++k)
            out.cells[k] = static_cast<u16>(mix(mix(static_cast<u32>(cx), static_cast<u32>(cz)), k));
        const std::lock_guard<std::mutex> lock(mutex);  // (a test's own bookkeeping)
        again += !made.insert({cx, cz}).second;
    }
    void decorate(GenChunk* const* area, i32 x0, i32 z0, u32 pass) override {
        for (i32 i = 0; i < 16; ++i)
            if (area[i]->x != x0 + (i & 3) || area[i]->z != z0 + (i >> 2) || area[i]->version != pass - 1) bad.store(true);
        if (x0 != GenPipeline::area_x0(pass, GenPipeline::area_a(pass, x0)) ||
            z0 != GenPipeline::area_z0(pass, GenPipeline::area_b(pass, z0)))
            bad.store(true);
        for (i32 i = 0; i < 4; ++i) {
            const i32 cx = 1 + (i & 1), cz = 1 + (i >> 1);
            GenChunk& centre = *area[cx + cz * 4];
            if (centre.decorated) bad.store(true);
            u64 read = 0;
            for (i32 dz = -1; dz <= 1; ++dz)
                for (i32 dx = -1; dx <= 1; ++dx)
                    for (u32 k = 0; k < kNumbers; ++k) read = mix(read, area[(cx + dx) + (cz + dz) * 4]->cells[k]);
            for (i32 dz = -1; dz <= 1; ++dz)
                for (i32 dx = -1; dx <= 1; ++dx)
                    for (u32 k = 0; k < kNumbers; ++k) {
                        u16& cell = area[(cx + dx) + (cz + dz) * 4]->cells[k];
                        cell = static_cast<u16>(mix(mix(cell, read), static_cast<u64>((dx + 1) + (dz + 1) * 3) * kNumbers + k));
                    }
            centre.decorated = true;
        }
        for (i32 i = 0; i < 16; ++i) area[i]->version = static_cast<u8>(pass);
        areas.fetch_add(1, std::memory_order_relaxed);
    }
    i32 sea_level() const override { return 63; }
    i32 spawn_height() const override { return 64; }
    bool is_flat() const override { return false; }

    std::atomic<u64> areas{0};
    std::atomic<bool> bad{false};
    std::mutex mutex;
    std::set<std::pair<i32, i32>> made;  // the chunks whose terrain was made...
    u64 again = 0;                       // ...and how often one of them a second time
};

using Numbers = std::array<u16, Mixer::kNumbers>;

// The same world the long way: a square of terrain, then every area of pass 1 that lies in it,
// every area of pass 2 whose chunks have all been through pass 1, and so on. What has been
// through all four is what a chunk is.
std::map<At, Numbers> the_long_way(i32 x0, i32 z0, i32 x1, i32 z1) {
    constexpr i32 kMargin = 12;
    Mixer gen;
    std::map<At, std::unique_ptr<GenChunk>> chunks;
    for (i32 z = z0 - kMargin; z <= z1 + kMargin; ++z)
        for (i32 x = x0 - kMargin; x <= x1 + kMargin; ++x) {
            auto& c = chunks[{x, z}];
            c = std::make_unique<GenChunk>();
            gen.make_base(x, z, *c);
        }
    for (u32 pass = 1; pass <= GenPipeline::kPasses; ++pass) {
        for (i32 b = GenPipeline::area_b(pass, z0 - kMargin) - 1; b <= GenPipeline::area_b(pass, z1 + kMargin) + 1; ++b)
            for (i32 a = GenPipeline::area_a(pass, x0 - kMargin) - 1; a <= GenPipeline::area_a(pass, x1 + kMargin) + 1; ++a) {
                GenChunk* area[16];
                bool whole = true;
                for (i32 i = 0; i < 16 && whole; ++i) {
                    const auto it =
                        chunks.find({GenPipeline::area_x0(pass, a) + (i & 3), GenPipeline::area_z0(pass, b) + (i >> 2)});
                    whole = it != chunks.end() && it->second->version == pass - 1;
                    if (whole) area[i] = it->second.get();
                }
                if (whole) gen.decorate(area, GenPipeline::area_x0(pass, a), GenPipeline::area_z0(pass, b), pass);
            }
    }
    EXPECT_FALSE(gen.bad.load());
    std::map<At, Numbers> out;
    for (i32 z = z0; z <= z1; ++z)
        for (i32 x = x0; x <= x1; ++x) {
            const GenChunk& c = *chunks[{x, z}];
            EXPECT_EQ(c.version, GenPipeline::kPasses) << x << ", " << z;
            EXPECT_TRUE(c.decorated);
            Numbers n;
            std::copy(c.cells, c.cells + Mixer::kNumbers, n.begin());
            out[{x, z}] = n;
        }
    return out;
}

// A pipeline, its workers, and what it has made.
struct Stand {
    explicit Stand(u32 threads) : workers(threads), pipeline(workers, gen, &made, this) {}
    ~Stand() {
        pipeline.stop();
        workers.drain();
    }

    static void made(void* ctx, i32 cx, i32 cz, GenChunk* g) {
        auto& s = *static_cast<Stand*>(ctx);
        EXPECT_EQ(g->x, cx);
        EXPECT_EQ(g->z, cz);
        EXPECT_EQ(g->version, GenPipeline::kPasses);
        EXPECT_TRUE(g->decorated);
        Numbers n;
        std::copy(g->cells, g->cells + Mixer::kNumbers, n.begin());
        EXPECT_TRUE(s.got.emplace(At{cx, cz}, n).second) << "chunk " << cx << ", " << cz << " was made twice";
        delete g;
    }
    void request(i32 cx, i32 cz) { pipeline.request(cx, cz, seq++); }
    // Until nothing is out and nothing is waiting.
    void finish() {
        for (u32 round = 0; pipeline.waiting() != 0 || pipeline.running() != 0; ++round) {
            workers.kick();
            if (workers.poll() == 0) std::this_thread::yield();
            ASSERT_LT(round, 50'000'000u) << "the pipeline does not come to an end";
        }
    }

    Mixer gen;
    base::Workers workers;
    GenPipeline pipeline;
    std::map<At, Numbers> got;
    u64 seq = 0;
};

TEST(GenPipeline, EveryChunkIsInTheMiddleOfOneAreaWithItsNine) {
    for (i32 z = -9; z <= 9; ++z)
        for (i32 x = -9; x <= 9; ++x) {
            u32 middles = 0;
            for (u32 pass = 1; pass <= GenPipeline::kPasses; ++pass) {
                const i32 a = GenPipeline::area_a(pass, x), b = GenPipeline::area_b(pass, z);
                const i32 lx = x - GenPipeline::area_x0(pass, a), lz = z - GenPipeline::area_z0(pass, b);
                ASSERT_TRUE(lx >= 0 && lx < 4 && lz >= 0 && lz < 4);
                EXPECT_EQ(GenPipeline::area_slot(pass, x, z), static_cast<u32>(lx + lz * 4));
                middles += (lx == 1 || lx == 2) && (lz == 1 || lz == 2);
            }
            EXPECT_EQ(middles, 1u) << x << ", " << z;
        }
}

// The first chunk of a world: the terrain of 96 chunks and 13 areas - 6, 4, 2 and 1 of the four passes.
TEST(GenPipeline, OneChunkIsMadeOfItsCone) {
    Stand s(0);
    s.request(5, -3);
    s.finish();
    ASSERT_EQ(s.got.size(), 1u);
    EXPECT_EQ(s.pipeline.terrains_made(), 96u);
    EXPECT_EQ(s.pipeline.areas_made(), 13u);
    EXPECT_FALSE(s.gen.bad.load());
    EXPECT_EQ(s.got.begin()->second, (the_long_way(5, -3, 5, -3)[{5, -3}]));
    // What else came of it waits: the other fifteen of its area, finished, and halves of areas around.
    EXPECT_EQ(s.pipeline.kept_chunks(), 96u - 1);
    // A chunk of the same area is there at once; nothing is made for it.
    const At beside{GenPipeline::area_x0(4, GenPipeline::area_a(4, 5)), -3};
    s.request(beside.first, beside.second);
    EXPECT_EQ(s.got.size(), 2u);
    EXPECT_EQ(s.pipeline.terrains_made(), 96u);
    EXPECT_EQ(s.pipeline.waiting(), 0u);
}

TEST(GenPipeline, AWorldIsTheSameWhoeverMakesIt) {
    const i32 x0 = -7, z0 = 3, x1 = 4, z1 = 13;
    const std::map<At, Numbers> want = the_long_way(x0, z0, x1, z1);
    std::vector<At> all;
    for (i32 z = z0; z <= z1; ++z)
        for (i32 x = x0; x <= x1; ++x) all.push_back({x, z});
    for (const u32 threads : {0u, 1u, 4u}) {
        for (u32 order = 0; order < 3; ++order) {
            Stand s(threads);
            std::vector<At> asked = all;
            if (order == 1) std::reverse(asked.begin(), asked.end());
            if (order == 2) std::shuffle(asked.begin(), asked.end(), std::mt19937(threads * 7 + 1));
            for (const At& at : asked) s.request(at.first, at.second);
            s.finish();
            EXPECT_FALSE(s.gen.bad.load());
            EXPECT_EQ(s.got, want) << threads << " threads, order " << order;
            // Nothing was thrown away, so nothing was made twice: every chunk's terrain once, every area once.
            EXPECT_EQ(s.gen.again, 0u);
            EXPECT_EQ(s.gen.made.size(), s.pipeline.terrains_made());
            EXPECT_EQ(s.gen.areas.load(), s.pipeline.areas_made());
        }
    }
}

// With no room to keep anything, whatever nobody waits for goes at every look - and is made again
// when the next chunk is asked for. The chunks are the same.
TEST(GenPipeline, WhatIsThrownAwayIsMadeAgainTheSame) {
    const i32 x0 = 0, z0 = 0, x1 = 6, z1 = 6;
    const std::map<At, Numbers> want = the_long_way(x0, z0, x1, z1);
    for (const u32 threads : {0u, 3u}) {
        Stand s(threads);
        s.pipeline.set_budget(0);
        u64 now = 0;
        for (i32 z = z0; z <= z1; ++z)
            for (i32 x = x0; x <= x1; ++x) {
                s.request(x, z);
                s.finish();
                s.pipeline.tick(now += 20);
                EXPECT_EQ(s.pipeline.kept_chunks(), 0u);
                EXPECT_EQ(s.pipeline.kept_bytes(), 0u);
            }
        EXPECT_FALSE(s.gen.bad.load());
        EXPECT_EQ(s.got, want) << threads << " threads";
        EXPECT_EQ(s.pipeline.terrains_made(), u64{96} * want.size());  // every chunk from nothing
        EXPECT_GT(s.gen.again, 0u);
    }
    // ...and what has lain long enough goes too, however much room there is.
    Stand s(0);
    s.pipeline.set_keep(100);
    s.request(0, 0);
    s.finish();
    s.pipeline.tick(40);
    EXPECT_EQ(s.pipeline.kept_chunks(), 95u);
    s.pipeline.tick(120);
    EXPECT_EQ(s.pipeline.kept_chunks(), 0u);
}

// A chunk nobody waits for any more is not made - what was begun for it comes back and is kept -
// and asked for again it is the chunk it would have been.
TEST(GenPipeline, AForgottenChunkIsNotMade) {
    const std::map<At, Numbers> want = the_long_way(-2, -2, 9, 9);
    for (const u32 threads : {0u, 4u}) {
        Stand s(threads);
        for (i32 z = -2; z <= 9; ++z)
            for (i32 x = -2; x <= 9; ++x) s.request(x, z);
        std::set<At> forgotten;
        for (i32 z = -2; z <= 9; ++z)
            for (i32 x = 3; x <= 9; ++x)
                if (s.pipeline.forget(x, z)) forgotten.insert({x, z});
        if (threads == 0) EXPECT_EQ(forgotten.size(), 7u * 12);  // (nothing comes back before the first poll)
        EXPECT_FALSE(s.pipeline.forget(100, 100));
        s.finish();
        EXPECT_EQ(s.got.size(), 12u * 12 - forgotten.size());
        for (const At& at : forgotten) EXPECT_FALSE(s.got.contains(at));
        const u64 made_for_some = s.pipeline.terrains_made();
        for (const At& at : forgotten) s.request(at.first, at.second);
        s.finish();
        EXPECT_GT(s.pipeline.terrains_made(), made_for_some);
        EXPECT_FALSE(s.gen.bad.load());
        EXPECT_EQ(s.got, want) << threads << " threads";
        EXPECT_FALSE(s.pipeline.forget(0, 0));  // made: nothing to forget
    }
}

TEST(GenPipeline, NothingIsStartedWhileItIsHeld) {
    Stand s(2);
    s.pipeline.hold(true);
    s.request(1, 1);
    for (u32 i = 0; i < 50; ++i) {
        s.workers.kick();
        s.workers.poll();
    }
    EXPECT_EQ(s.pipeline.running(), 0u);
    EXPECT_TRUE(s.got.empty());
    s.pipeline.hold(false);
    s.finish();
    EXPECT_EQ(s.got.size(), 1u);
}

// Requests, second thoughts, polls and looks at what is kept in any order: every chunk that is
// handed out is the chunk it is, and each request is answered once.
TEST(GenPipeline, AnyOrderOfEventsMakesTheSameChunks) {
    const i32 lo = -6, hi = 9;
    const std::map<At, Numbers> want = the_long_way(lo, lo, hi, hi);
    for (const u32 threads : {0u, 4u}) {
        Stand s(threads);
        s.pipeline.set_budget(usize{300} * 1024);  // a few chunks
        std::mt19937 random(threads + 99);
        std::set<At> waiting;
        std::map<At, u32> answered;
        u64 now = 0;
        for (u32 step = 0; step < 4000; ++step) {
            const u32 what = random() % 10;
            const At at{lo + static_cast<i32>(random() % (hi - lo + 1)), lo + static_cast<i32>(random() % (hi - lo + 1))};
            if (what < 5) {
                if (waiting.insert(at).second) {
                    s.got.erase(at);  // (asked for again: the world lost it)
                    s.request(at.first, at.second);
                }
            } else if (what < 7) {
                if (waiting.contains(at) && s.pipeline.forget(at.first, at.second)) waiting.erase(at);
            } else if (what < 9) {
                // (Something that is out is waited for: how much gets done must not be the machine's to say.)
                s.workers.kick();
                if (s.pipeline.running() != 0) s.workers.wait(base::now_ns() + base::kNsPerSec);
                s.workers.poll();
            } else {
                s.pipeline.tick(now += 20);
            }
            for (auto it = waiting.begin(); it != waiting.end();) {
                const auto made = s.got.find(*it);
                if (made == s.got.end()) {
                    ++it;
                    continue;
                }
                EXPECT_EQ(made->second, want.at(*it)) << "chunk " << it->first << ", " << it->second;
                ++answered[*it];
                it = waiting.erase(it);
            }
        }
        s.finish();
        for (const At& at : waiting) {
            ASSERT_TRUE(s.got.contains(at));
            EXPECT_EQ(s.got.at(at), want.at(at));
        }
        EXPECT_FALSE(s.gen.bad.load());
        EXPECT_GT(answered.size(), 50u);
    }
}

}  // namespace
}  // namespace world
