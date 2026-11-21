// SPDX-License-Identifier: GPL-3.0-or-later
// Physics benchmarks: what a tick of an entity that moves by itself costs, by what the entity
// is doing at the time.
//
// Every scenario is 10 000 entities on a 128 x 128 block patch of flat world and the same
// ticks for all of them; the number is nanoseconds per entity per tick. What an entity is doing
// decides nearly all of it:
//   at rest     most of what a world holds: items lying about, mobs standing. An item at rest
//               only tries the ground every fourth tick; a mob or a primed TNT every tick
//   falling     nothing near: the cost of asking the blocks and finding none
//   thrown      a whole life - through the air, onto the ground, sliding to a stop
//   clutter     the same over slabs, stairs, carpets, chests and fences: collision shapes that
//               are not whole blocks, and (fences, walls) shapes taller than their block, which
//               make every query look one block further
// These are the functions of src/phys alone - no entity table, no tracker. What a whole server
// tick costs with entities in it is measured on a running server (docs/architecture.md).
#include <cmath>
#include <random>
#include <string>
#include <vector>

#include "bench.hpp"
#include "phys/physics.hpp"
#include "proto/frame.hpp"
#include "world/block.hpp"
#include "world/generator.hpp"

namespace benchmark {

namespace {

constexpr i32 kGround = -60;  // the first air block over a default flat world
constexpr i32 kChunks = 8;    // the patch: 8 x 8 chunks
constexpr double kSide = kChunks * 16.0;
constexpr u32 kCount = 10000;

struct Thing {
    phys::Motion m;
    phys::Body b;
};

float half(void*) { return 0.5f; }  // RandomSource.nextFloat for an item stuck in a block: none is

struct Field {
    world::FlatGenerator gen{-64, 24, world::FlatGenerator::default_layers(), 39};
    world::Dimension dim{proto::reg::dimension_type::kOverworld, "minecraft:overworld", gen, proto::kNoCompression};
    phys::Scratch scratch;
    phys::World world{dim, scratch};
    std::mt19937_64 rng{2603};

    Field() {
        for (i32 cx = -1; cx <= kChunks; ++cx)
            for (i32 cz = -1; cz <= kChunks; ++cz) dim.ticking(cx, cz);
    }
    double uniform(double lo, double hi) { return lo + (hi - lo) * (static_cast<double>(rng() >> 11) * 0x1.0p-53); }

    // One block in `percent` of the columns, standing on the ground: the default state of one of `blocks`.
    void clutter(std::initializer_list<const char*> blocks, u32 percent) {
        std::vector<world::BlockState> states;
        for (const char* name : blocks) {
            const u32 block = world::find_block(name);
            if (block == world::kNoId) std::abort();
            states.push_back(static_cast<world::BlockState>(world::block_info(block).default_state));
        }
        for (i32 x = 0; x < kChunks * 16; ++x)
            for (i32 z = 0; z < kChunks * 16; ++z)
                if (rng() % 100 < percent) dim.set_block(x, kGround, z, states[rng() % states.size()]);
        dim.update_light();
        dim.end_tick();
    }

    // `kCount` entities over the patch, `height` above the ground, each with a velocity of up
    // to `speed` sideways and `up` upwards.
    std::vector<Thing> scatter(double height, double spread, double speed, double up) {
        std::vector<Thing> out(kCount);
        for (Thing& t : out) {
            const double angle = uniform(0.0, 6.283185307179586);
            t.m = {{uniform(2.0, kSide - 2.0), kGround + height + uniform(0.0, spread), uniform(2.0, kSide - 2.0)},
                   {std::cos(angle) * speed, uniform(0.0, up), std::sin(angle) * speed}};
        }
        return out;
    }
};

// `ticks` ticks of every entity, after `warm` that are not timed; the best of five runs from
// the same start. tick(thing, index, tick number from 1).
template <class Tick>
std::vector<Thing> run(const std::string& name, const std::vector<Thing>& start, u32 warm, u32 ticks, Tick&& tick) {
    std::vector<Thing> things;
    if (!selected(name.c_str())) return things;
    double best = 1e30;
    for (int rep = 0; rep < 5; ++rep) {
        things = start;
        for (u32 t = 1; t <= warm; ++t)
            for (u32 i = 0; i < things.size(); ++i) tick(things[i], i, t);
        const auto t0 = Clock::now();
        for (u32 t = warm + 1; t <= warm + ticks; ++t)
            for (u32 i = 0; i < things.size(); ++i) tick(things[i], i, t);
        best = std::min(best, std::chrono::duration<double>(Clock::now() - t0).count());
    }
    report(name.c_str(), best / (static_cast<double>(ticks) * static_cast<double>(things.size())) * 1e9, "ns/entity tick");
    g_sink = static_cast<u64>(things[0].m.pos[1] * 1e6);
    return things;
}

constexpr const char* kScenarios[] = {"physics: items at rest",         "physics: items falling",
                                      "physics: items thrown, to rest", "physics: primed TNT at rest",
                                      "physics: mobs at rest",          "physics: mobs falling",
                                      "physics: mobs walking",          "physics: armor stands at rest",
                                      "physics: armor stands falling",  "physics: items at rest, as the server ticks them"};

// The scenarios on one kind of ground. `blocks`: what stands on a quarter of its columns.
void scenarios(const char* where, std::initializer_list<const char*> blocks) {
    const std::string at = where;
    bool any = false;
    for (const char* name : kScenarios) any = any || selected((name + at).c_str());
    if (!any) return;
    Field f;
    if (blocks.size()) f.clutter(blocks, 25);

    const auto item = [&](Thing& t, u32 i, u32 tick) {
        phys::item_tick(f.world, t.m, t.b, tick, static_cast<i32>(i), half, nullptr);
    };
    const auto tnt = [&](Thing& t, u32, u32) { phys::tnt_tick(f.world, t.m, t.b); };
    // A cow: 0.9 x 1.4, a step of 0.6, the gravity of everything alive.
    static constexpr phys::Kind kCow = phys::living_kind(0.9f, 1.4f, 0.6f, 0.08);
    const auto cow = [&](Thing& t, u32, u32) { phys::living_tick(f.world, kCow, t.m, t.b); };
    // ...and one that keeps walking: what its legs would add every tick, straight into whatever is there.
    const auto walking = [&](Thing& t, u32 i, u32) {
        t.m.vel[0] += (i & 1) ? 0.05 : -0.05;
        t.m.vel[2] += (i & 2) ? 0.03 : -0.03;
        phys::living_tick(f.world, kCow, t.m, t.b);
        t.m.pos[0] = std::fmod(t.m.pos[0] + kSide, kSide);  // around the patch, and on
        t.m.pos[2] = std::fmod(t.m.pos[2] + kSide, kSide);
    };

    run(kScenarios[0] + at, f.scatter(1.0, 0.5, 0.0, 0.0), 300, 400, item);
    run(kScenarios[1] + at, f.scatter(120.0, 40.0, 0.1, 0.2), 0, 30, item);
    run(kScenarios[2] + at, f.scatter(1.32, 0.0, 0.3, 0.2), 0, 40, item);
    run(kScenarios[3] + at, f.scatter(1.0, 0.5, 0.0, 0.0), 100, 200, tnt);
    run(kScenarios[4] + at, f.scatter(1.0, 0.5, 0.0, 0.0), 100, 200, cow);
    run(kScenarios[5] + at, f.scatter(200.0, 40.0, 0.1, 0.0), 0, 30, cow);
    run(kScenarios[6] + at, f.scatter(1.0, 0.5, 0.0, 0.0), 100, 200, walking);
    // An armor stand: the living thing a vanilla server can be asked to hold ten thousand of
    // without their minds getting in the way of the measurement (tools/vanilla/physics_probe.py perf).
    static constexpr phys::Kind kStand = phys::living_kind(0.5f, 1.975f, 0.0f, 0.08);
    const auto stand = [&](Thing& t, u32, u32) { phys::living_tick(f.world, kStand, t.m, t.b); };
    run(kScenarios[7] + at, f.scatter(1.0, 0.5, 0.0, 0.0), 100, 200, stand);
    run(kScenarios[8] + at, f.scatter(200.0, 40.0, 0.1, 0.0), 0, 30, stand);

    // What lies still is not ticked in full (server/physics.cpp): once phys::item_rests() has
    // said so, an item's tick is phys::item_rest_tick() - and a living entity's is nothing.
    if (selected((kScenarios[9] + at).c_str())) {
        std::vector<Thing> things = f.scatter(1.0, 0.5, 0.0, 0.0);
        std::vector<phys::ItemRest> rest(things.size());
        std::vector<u8> asleep(things.size(), 0);
        u32 tick = 0;
        const auto pass = [&] {
            ++tick;
            for (u32 i = 0; i < things.size(); ++i) {
                Thing& t = things[i];
                const i32 id = static_cast<i32>(i);
                const u32 phase = static_cast<u32>(static_cast<i32>(tick) + id) & 3;
                if (asleep[i]) {
                    t.b.needs_sync = phys::item_rest_tick(t.m.vel[0], t.m.vel[1], t.m.vel[2], phase, rest[i]);
                    continue;
                }
                const double y = t.m.pos[1];
                phys::item_tick(f.world, t.m, t.b, tick, id, half, nullptr);
                t.b.needs_sync = false;
                if (phase == 0 && t.b.on_ground && y == t.m.pos[1])
                    asleep[i] = phys::item_rests(f.world, t.m, t.b, tick, id, rest[i]);
            }
        };
        for (u32 i = 0; i < 300; ++i) pass();
        u32 resting = 0;
        for (const u8 a : asleep) resting += a;
        const double took = best_seconds(5, [&] {
            for (u32 i = 0; i < 400; ++i) pass();
        });
        report((kScenarios[9] + at).c_str(), took / (400.0 * static_cast<double>(things.size())) * 1e9, "ns/entity tick");
        report((kScenarios[9] + at + ": at rest").c_str(), 100.0 * resting / static_cast<double>(things.size()), "%");
        g_sink = static_cast<u64>(things[0].m.vel[1] * 1e9);
    }
}

}  // namespace

void physics_benchmarks() {
    scenarios("", {});
    scenarios(", clutter", {"minecraft:stone_slab", "minecraft:oak_stairs", "minecraft:white_carpet", "minecraft:chest",
                            "minecraft:stone", "minecraft:ice"});
    scenarios(", clutter + fences", {"minecraft:stone_slab", "minecraft:oak_stairs", "minecraft:white_carpet", "minecraft:chest",
                                     "minecraft:stone", "minecraft:ice", "minecraft:oak_fence", "minecraft:cobblestone_wall"});
}

}  // namespace benchmark
