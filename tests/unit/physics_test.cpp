// SPDX-License-Identifier: GPL-3.0-or-later
// Entity physics against recordings of a vanilla server, and the collision code against itself.
//
// tests/unit/data/physics_vectors.bin is what tools/vanilla/physics_probe.py recorded on a vanilla
// 26.3 server: some hundreds of scenarios - an item thrown at a wall, a primed TNT on slime, an
// armor stand walking up a slab - each with the blocks it was built of and the state of its
// entities after every tick, read with /data get under /tick step: position, velocity,
// OnGround and fall_distance, as the eight bytes of each double. A scenario is replayed here on
// the same blocks at the same coordinates and compared bit for bit - nothing is "near enough".
// (One bit is not in the recording: the sign of a zero. NBT has a single zero.)
//
// The file (little-endian; see `emit` in the probe):
//   "PHYSVEC1"
//   i32 chunk x, z of the bot, i32 the server's simulation distance,
//   u32 n, n x (i32 chunk x0, z0, x1, z1)                    chunks that were force-loaded
//   u32 scenarios, then per scenario
//     str name, u8 kind (item, tnt, armor_stand, pig, falling_block), u8 summoned on the ground, f64 step height (-1: the type's)
//     u32 n, n x (i32 x, y, z, str block)                    the blocks, "minecraft:oak_stairs[facing=east]"
//     u32 n, n x (u32 tick, i32 x, y, z, str block)          blocks set after that tick was recorded
//     u32 n, n x (i32 entity id, u32 states, states x (u8 flags: 1 on the ground, 2 gone; i32 fuse; f64 x 7))
//   str: u8 length, bytes.
// physics_falling.bin is a second recording in the same form: falling blocks (FallingBlockEntity),
// made when they were ported, beside the first instead of into it. physics_fluids.bin is a third:
// what is in water and lava - and on fire - in the second form of the file, "PHYSVEC2":
//     ... f64 step height, str entity type ("minecraft:giant"), u8 an item's stack does not burn
//     ... states x (u8 flags; i32 fuse; f64 x 7; i16 Fire; i16 an item's Health)
#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "phys/physics.hpp"
#include "proto/frame.hpp"
#include "world/block.hpp"
#include "world/collide.hpp"
#include "world/dimension.hpp"

#include "states.hpp"

#include PROTO_GEN_PHYSICS_HPP

using namespace world;
namespace blk = proto::gen::block;
namespace gp = proto::gen::physics;
namespace entity_type = proto::gen::entity_type;

namespace {

constexpr unsigned char kVectors[] = {
#embed "data/physics_vectors.bin"
};
constexpr unsigned char kFallingVectors[] = {
#embed "data/physics_falling.bin"
};
constexpr unsigned char kFluidVectors[] = {
#embed "data/physics_fluids.bin"
};

struct State {
    double pos[3], vel[3], fall;
    bool on_ground, gone;
    i32 fuse;
    i16 fire = 0, health = 0;  // (the second form: Fire, and an item's Health)
};
struct Entity {
    i32 id;
    std::vector<State> states;
};
struct BlockAtPos {
    i32 x, y, z;
    std::string block;
};
struct Event {
    u32 tick;
    BlockAtPos block;
};
enum class Kind : u8 { kItem, kTnt, kArmorStand, kPig, kFallingBlock, kLiving };
struct Scenario {
    std::string name;
    Kind kind;
    bool on_ground;
    double step_height;
    u32 type = 0;            // entity_type
    bool fireproof = false;  // an item's stack does not burn
    bool burns = false;      // the recording has Fire and Health: they are compared
    std::vector<BlockAtPos> blocks;
    std::vector<Event> events;
    std::vector<Entity> entities;
};

class VectorReader {
  public:
    VectorReader(const unsigned char* p, usize n) : p_(p), end_(p + n) {}
    template <class T>
    T get() {
        T v;
        if (p_ + sizeof(T) > end_) std::abort();
        std::memcpy(&v, p_, sizeof(T));
        p_ += sizeof(T);
        return v;
    }
    std::string str() {
        const u8 n = get<u8>();
        std::string s(reinterpret_cast<const char*>(p_), n);
        p_ += n;
        return s;
    }
    bool done() const { return p_ == end_; }

  private:
    const unsigned char* p_;
    const unsigned char* end_;
};

// Where the recorded server ticked entities (ServerLevel.tick: inEntityTickingRange): in the
// chunks within its simulation distance of the one player, and in force-loaded chunks.
struct Ticking {
    i32 cx, cz, distance;
    struct Rect {
        i32 x0, z0, x1, z1;
    };
    std::vector<Rect> forced;
    bool at(double x, double z) const {
        const i32 x_chunk = static_cast<i32>(std::floor(x)) >> 4, z_chunk = static_cast<i32>(std::floor(z)) >> 4;
        if (std::max(std::abs(x_chunk - cx), std::abs(z_chunk - cz)) <= distance) return true;
        for (const Rect& r : forced)
            if (x_chunk >= r.x0 && x_chunk <= r.x1 && z_chunk >= r.z0 && z_chunk <= r.z1) return true;
        return false;
    }
};

std::vector<Scenario> load_vectors(Ticking& ticking, std::span<const unsigned char> vectors = kVectors) {
    VectorReader r(vectors.data(), vectors.size());
    std::vector<Scenario> out;
    const bool second = std::memcmp(vectors.data(), "PHYSVEC2", 8) == 0;
    if (!second && std::memcmp(vectors.data(), "PHYSVEC1", 8) != 0) std::abort();
    for (int i = 0; i < 8; ++i) r.get<u8>();
    ticking.cx = r.get<i32>(), ticking.cz = r.get<i32>(), ticking.distance = r.get<i32>();
    for (u32 n = r.get<u32>(); n > 0; --n) {
        Ticking::Rect rect;
        rect.x0 = r.get<i32>(), rect.z0 = r.get<i32>(), rect.x1 = r.get<i32>(), rect.z1 = r.get<i32>();
        ticking.forced.push_back(rect);
    }
    const u32 count = r.get<u32>();
    for (u32 i = 0; i < count; ++i) {
        Scenario s;
        s.name = r.str();
        s.kind = static_cast<Kind>(r.get<u8>());
        s.on_ground = r.get<u8>() != 0;
        s.step_height = r.get<double>();
        static constexpr u32 kTypes[] = {entity_type::kItem, entity_type::kTnt, entity_type::kArmorStand, entity_type::kPig,
                                         entity_type::kFallingBlock};
        if (second) {
            const std::string type = r.str();
            s.type = entity_type::kCount;
            for (u32 t = 0; t < entity_type::kCount; ++t)
                if (type == entity_type::kNames[t]) s.type = t;
            if (s.type == entity_type::kCount) std::abort();
            s.fireproof = r.get<u8>() != 0;
            s.burns = true;
        } else {
            s.type = kTypes[static_cast<u32>(s.kind)];
        }
        for (u32 n = r.get<u32>(); n > 0; --n) {
            BlockAtPos b;
            b.x = r.get<i32>(), b.y = r.get<i32>(), b.z = r.get<i32>();
            b.block = r.str();
            s.blocks.push_back(std::move(b));
        }
        for (u32 n = r.get<u32>(); n > 0; --n) {
            Event e;
            e.tick = r.get<u32>();
            e.block.x = r.get<i32>(), e.block.y = r.get<i32>(), e.block.z = r.get<i32>();
            e.block.block = r.str();
            s.events.push_back(std::move(e));
        }
        for (u32 n = r.get<u32>(); n > 0; --n) {
            Entity e;
            e.id = r.get<i32>();
            for (u32 k = r.get<u32>(); k > 0; --k) {
                State st;
                const u8 flags = r.get<u8>();
                st.on_ground = flags & 1, st.gone = flags & 2;
                st.fuse = r.get<i32>();
                for (double& v : st.pos) v = r.get<double>();
                for (double& v : st.vel) v = r.get<double>();
                st.fall = r.get<double>();
                if (second) st.fire = r.get<i16>(), st.health = r.get<i16>();
                e.states.push_back(st);
            }
            s.entities.push_back(std::move(e));
        }
        out.push_back(std::move(s));
    }
    if (!r.done()) std::abort();
    return out;
}

void put(Dimension& dim, const BlockAtPos& b) {
    dim.ticking(b.x >> 4, b.z >> 4);
    const BlockState s = state_of(b.block);
    if (dim.block(b.x, b.y, b.z) != s) {
        ASSERT_TRUE(dim.set_block(b.x, b.y, b.z, s)) << b.block;
    }
}

phys::Kind kind_of(const Scenario& s) {
    switch (s.kind) {
        case Kind::kItem: return phys::kItem;
        case Kind::kTnt: return phys::kTnt;
        case Kind::kFallingBlock: return phys::kFallingBlock;
        case Kind::kArmorStand:
        case Kind::kPig:
        case Kind::kLiving: break;
    }
    // What lives, as its type is - but for a step height the scenario gave it (an armor stand's own is none).
    phys::Kind k = phys::living_kind(s.type);
    if (s.step_height >= 0) k.max_up_step = static_cast<float>(s.step_height);
    return k;
}

float no_random(void*) {
    ADD_FAILURE() << "a recorded scenario drew from the entity's RandomSource";
    return 0.0f;
}

u64 bits(double v) { return std::bit_cast<u64>(v); }
// The same double - but for the sign of a zero, which the recording does not have: what
// /data get prints went through DoubleTag.valueOf, and that hands out one zero for both.
bool same_double(double here, double recorded) { return bits(here) == bits(recorded) || (here == 0.0 && recorded == 0.0); }

// The whole recording, tick by tick, as the vanilla server ran it: every scenario's blocks in
// one world (what leaves its own platform lands on a neighbour's), every entity ticked in the
// order of its id. `diffs`: per scenario what first differs, empty if nothing does.
//
// `at_rest`: with what lies still not ticked in full (as server/physics.cpp does it): an item
// that phys::item_rests() gets phys::item_rest_tick(), a living entity whose tick changed
// nothing is passed by - until a block within reach is set, or something pushes it. `rested`
// counts the ticks done that way: the recording is the same either way, or the rest is wrong.
void replay(const std::vector<Scenario>& scenarios, const Ticking& ticking, std::vector<std::string>& diffs, bool at_rest = false,
            u64* rested = nullptr) {
    FlatGenerator gen(-64, 24, FlatGenerator::default_layers(), 39);
    Dimension dim(proto::reg::dimension_type::kOverworld, "minecraft:overworld", gen, proto::kNoCompression);
    phys::Scratch scratch;
    phys::World world{dim, scratch};

    struct Live {
        const Scenario* sc;
        const Entity* ent;
        phys::Kind kind;
        phys::Motion m;
        phys::Body b;
        usize scenario;
        bool gone = false, done = false;
        bool asleep = false;
        phys::ItemRest rest{};
        i32 health = 5;  // ItemEntity.health
    };
    std::vector<Live> live;
    usize ticks = 0;
    diffs.assign(scenarios.size(), {});
    for (usize si = 0; si < scenarios.size(); ++si) {
        const Scenario& sc = scenarios[si];
        for (const BlockAtPos& b : sc.blocks) put(dim, b);
        for (const Entity& e : sc.entities) {
            Live l{&sc, &e, kind_of(sc), {}, {}, si};
            const State& s0 = e.states[0];
            for (u32 a = 0; a < 3; ++a) l.m.pos[a] = s0.pos[a], l.m.vel[a] = s0.vel[a];
            l.b.on_ground = sc.on_ground;  // OnGround:1b in the summon; what it stands on is not known yet
            l.b.fire_ticks = s0.fire;      // Fire in the summon
            // Entity.fireImmune: the type's - and an item's stack may be one that does not burn.
            l.b.fire_immune = gp::kEntity[sc.type].fire_immune || sc.fireproof;
            dim.ticking(static_cast<i32>(std::floor(s0.pos[0])) >> 4, static_cast<i32>(std::floor(s0.pos[2])) >> 4);
            ticks = std::max(ticks, e.states.size());
            live.push_back(l);
        }
    }
    std::sort(live.begin(), live.end(), [](const Live& a, const Live& b) { return a.ent->id < b.ent->id; });

    char text[512];
    for (u32 t = 1; t < ticks; ++t) {
        for (const Scenario& sc : scenarios) {
            for (const Event& e : sc.events) {
                if (e.tick != t - 1) continue;
                put(dim, e.block);
                for (Live& l : live)  // Game::wake_near
                    if (l.asleep && phys::block_in_reach(l.m, l.kind.width, l.kind.height, e.block.x, e.block.y, e.block.z))
                        l.asleep = false;
            }
        }
        for (Live& l : live) {
            if (t >= l.ent->states.size() || l.gone || l.done) continue;
            if (!ticking.at(l.m.pos[0], l.m.pos[2])) continue;  // it left the chunks that tick: it hangs where it is
            phys::Fate fate = phys::Fate::kStays;
            if (l.asleep) {
                if (rested) ++*rested;
                if (l.sc->kind == Kind::kItem)
                    phys::item_rest_tick(l.m.vel[0], l.m.vel[1], l.m.vel[2],
                                         static_cast<u32>(static_cast<i32>(t) + l.ent->id) & 3, l.rest);
                continue;
            }
            const phys::Motion before = l.m;
            const phys::Body body_before = l.b;
            bool pushed = false;
            switch (l.sc->kind) {
                case Kind::kItem:
                    fate = phys::item_tick(world, l.m, l.b, t, l.ent->id, no_random, nullptr);
                    // Game::tick_item: asked after a tick that tried the ground and left it where it was.
                    l.b.needs_sync = l.b.sync_position = false;
                    if (at_rest && fate == phys::Fate::kStays && ((static_cast<i32>(t) + l.ent->id) & 3) == 0 && l.b.on_ground &&
                        before.pos[0] == l.m.pos[0] && before.pos[1] == l.m.pos[1] && before.pos[2] == l.m.pos[2])
                        l.asleep = phys::item_rests(world, l.m, l.b, t, l.ent->id, l.rest);
                    break;
                case Kind::kTnt: phys::tnt_tick(world, l.m, l.b); break;
                case Kind::kFallingBlock: {
                    // FallingBlockEntity.tick as Game::tick_falling runs it: on the ground it is a block
                    // again, or broken - gone either way; in the air it is slowed (Entity.getAirDrag).
                    phys::falling_move(world, l.m, l.b);
                    if (l.b.on_ground) {
                        fate = phys::Fate::kGone;
                        break;
                    }
                    const double drag = static_cast<double>(0.98f);
                    for (double& v : l.m.vel) v = v * drag;
                    break;
                }
                case Kind::kArmorStand:
                case Kind::kLiving: fate = phys::living_tick(world, l.kind, l.m, l.b); break;
                case Kind::kPig: {
                    fate = phys::living_tick(world, l.kind, l.m, l.b);
                    // LivingEntity.pushEntities: every other pig whose box touches this one's pushes, and is pushed.
                    const Aabb mine = entity_box(l.m.pos[0], l.m.pos[1], l.m.pos[2], l.kind.width, l.kind.height);
                    for (Live& o : live) {
                        if (&o == &l || o.gone || o.sc != l.sc) continue;
                        if (!intersects(mine, entity_box(o.m.pos[0], o.m.pos[1], o.m.pos[2], o.kind.width, o.kind.height)))
                            continue;
                        double xa, za;
                        if (!phys::push_impulse(o.m.pos, l.m.pos, xa, za)) continue;  // doPush(o): o.push(this)
                        o.m.vel[0] = o.m.vel[0] + -xa, o.m.vel[1] = o.m.vel[1] + 0.0, o.m.vel[2] = o.m.vel[2] + -za;
                        l.m.vel[0] = l.m.vel[0] + xa, l.m.vel[1] = l.m.vel[1] + 0.0, l.m.vel[2] = l.m.vel[2] + za;
                        o.asleep = false;  // Game::push
                        pushed = true;
                    }
                    break;
                }
            }
            // What a tick tells of fire and lava is the game's to act on. An item has five points
            // of health (ItemEntity.hurtServer): a tick of burning takes one, every cell of lava it
            // is in four, and it is gone when none is left.
            if (l.sc->kind == Kind::kItem) {
                l.health -= l.b.burned ? 1 : 0;
                l.health -= 4 * static_cast<i32>(l.b.lava_hurts);
                if (l.health <= 0) fate = phys::Fate::kGone;
            }
            l.b.burned = l.b.extinguished = false, l.b.splashed = 0, l.b.splash_volume = 0, l.b.lava_hurts = 0;
            l.gone = fate == phys::Fate::kGone;
            // Game::tick_living: a tick that changed nothing, and nobody pushing.
            if (at_rest && !l.gone && l.kind.living && !pushed && body_before.on_ground && before.vel[0] == 0 &&
                before.vel[2] == 0 && phys::same_state(before, body_before, l.m, l.b))
                l.asleep = true;
        }
        for (Live& l : live) {
            if (t >= l.ent->states.size() || l.done) continue;
            const State& want = l.ent->states[t];
            if (want.gone || l.gone) {
                if (want.gone != l.gone) {
                    std::snprintf(text, sizeof text, "tick %u, entity %d: %s", t, l.ent->id,
                                  want.gone ? "vanilla discarded it" : "discarded here only");
                    if (diffs[l.scenario].empty()) diffs[l.scenario] = text;
                }
                l.done = true;
                continue;
            }
            bool same = l.b.on_ground == want.on_ground && same_double(l.b.fall_distance, want.fall);
            for (u32 a = 0; a < 3; ++a)
                same = same && same_double(l.m.pos[a], want.pos[a]) && same_double(l.m.vel[a], want.vel[a]);
            if (l.sc->burns) same = same && l.b.fire_ticks == want.fire && (l.sc->kind != Kind::kItem || l.health == want.health);
            if (same) continue;
            std::snprintf(
                text, sizeof text,
                "tick %u, entity %d:\n"
                "      vanilla pos (%.17g, %.17g, %.17g) vel (%.17g, %.17g, %.17g) ground %d fall %.17g fire %d health %d\n"
                "      here    pos (%.17g, %.17g, %.17g) vel (%.17g, %.17g, %.17g) ground %d fall %.17g fire %d health %d",
                t, l.ent->id, want.pos[0], want.pos[1], want.pos[2], want.vel[0], want.vel[1], want.vel[2], want.on_ground,
                want.fall, want.fire, want.health, l.m.pos[0], l.m.pos[1], l.m.pos[2], l.m.vel[0], l.m.vel[1], l.m.vel[2],
                l.b.on_ground, l.b.fall_distance, l.b.fire_ticks, l.sc->kind == Kind::kItem ? l.health : 0);
            if (diffs[l.scenario].empty()) diffs[l.scenario] = text;
            l.done = true;  // what follows a difference is not worth reading
        }
    }
}

}  // namespace

TEST(Physics, WhatLiesStillIsNotTickedAndComesOutTheSame) {
    Ticking ticking;
    const std::vector<Scenario> scenarios = load_vectors(ticking);
    std::vector<std::string> diffs;
    u64 rested = 0;
    replay(scenarios, ticking, diffs, true, &rested);
    u32 failed = 0;
    for (usize i = 0; i < scenarios.size(); ++i) {
        if (diffs[i].empty()) continue;
        ++failed;
        ADD_FAILURE() << scenarios[i].name << ": " << diffs[i];
    }
    EXPECT_GT(rested, 2000u) << "nothing came to rest: the test tests nothing";
    std::printf("            %llu ticks of entities at rest, %u scenarios differ\n", static_cast<unsigned long long>(rested),
                failed);
}

TEST(Physics, ReplaysWhatAVanillaServerRecorded) {
    Ticking ticking;
    const std::vector<Scenario> scenarios = load_vectors(ticking);
    ASSERT_GT(scenarios.size(), 300u);
    std::vector<std::string> diffs;
    replay(scenarios, ticking, diffs);
    u32 states = 0, failed = 0;
    for (usize i = 0; i < scenarios.size(); ++i) {
        for (const Entity& e : scenarios[i].entities) states += static_cast<u32>(e.states.size());
        if (diffs[i].empty()) continue;
        ++failed;
        ADD_FAILURE() << scenarios[i].name << ": " << diffs[i];
    }
    std::printf("            %zu scenarios, %u recorded states, %u scenarios differ\n", scenarios.size(), states, failed);
}

TEST(Physics, ReplaysTheFallingBlocksAVanillaServerRecorded) {
    Ticking ticking;
    const std::vector<Scenario> scenarios = load_vectors(ticking, kFallingVectors);
    ASSERT_GE(scenarios.size(), 12u);
    std::vector<std::string> diffs;
    replay(scenarios, ticking, diffs);
    u32 states = 0, failed = 0, landed = 0;
    for (usize i = 0; i < scenarios.size(); ++i) {
        for (const Entity& e : scenarios[i].entities) {
            states += static_cast<u32>(e.states.size());
            landed += e.states.back().gone;
        }
        if (diffs[i].empty()) continue;
        ++failed;
        ADD_FAILURE() << scenarios[i].name << ": " << diffs[i];
    }
    EXPECT_GE(landed, 10u) << "hardly any of them came down: the recording is of something else";
    std::printf("            %zu scenarios, %u recorded states, %u scenarios differ\n", scenarios.size(), states, failed);
}

// What is in water and lava, and on fire: sinking, floating up, carried by a current, lifted out
// against a bank, lit and put out - and an item's health, down to the tick it is gone in.
TEST(Physics, ReplaysTheFluidsAVanillaServerRecorded) {
    Ticking ticking;
    const std::vector<Scenario> scenarios = load_vectors(ticking, kFluidVectors);
    ASSERT_GE(scenarios.size(), 40u);
    std::vector<std::string> diffs;
    replay(scenarios, ticking, diffs);
    u32 states = 0, failed = 0, burned = 0;
    for (usize i = 0; i < scenarios.size(); ++i) {
        for (const Entity& e : scenarios[i].entities) {
            states += static_cast<u32>(e.states.size());
            burned += e.states.back().gone;
        }
        if (diffs[i].empty()) continue;
        ++failed;
        ADD_FAILURE() << scenarios[i].name << ": " << diffs[i];
    }
    EXPECT_GE(burned, 3u) << "no item burned: the recording is of something else";
    std::printf("            %zu scenarios, %u recorded states, %u scenarios differ\n", scenarios.size(), states, failed);
}

TEST(Physics, WhatLiesStillInAFluidIsNotTickedAndComesOutTheSame) {
    Ticking ticking;
    const std::vector<Scenario> scenarios = load_vectors(ticking, kFluidVectors);
    std::vector<std::string> diffs;
    u64 rested = 0;
    replay(scenarios, ticking, diffs, true, &rested);
    u32 failed = 0;
    for (usize i = 0; i < scenarios.size(); ++i) {
        if (diffs[i].empty()) continue;
        ++failed;
        ADD_FAILURE() << scenarios[i].name << ": " << diffs[i];
    }
    EXPECT_GT(rested, 100u) << "nothing came to rest: the test tests nothing";
    std::printf("            %llu ticks of entities at rest, %u scenarios differ\n", static_cast<unsigned long long>(rested),
                failed);
}

// ---------------------------------------------------------------------------- the collision code against itself

namespace {

// A patch of world with every kind of collision shape in it, and boxes thrown at it. Corners
// are put on sixteenths, and a ten-millionth off them, on purpose: 1e-7 is the margin of every
// comparison vanilla makes, and what lies just this side of it has to come out the same.
struct Arena {
    FlatGenerator gen{-64, 24, FlatGenerator::default_layers(), 39};
    Dimension dim{proto::reg::dimension_type::kOverworld, "minecraft:overworld", gen, proto::kNoCompression};
    std::mt19937_64 rng{0x26'03};
    static constexpr i32 kX0 = -6, kY0 = -60, kZ0 = 2, kSide = 12, kHeight = 6;
    static constexpr i32 kSeam = -48;  // the first layer of the second section

    explicit Arena(bool with_large) {
        std::vector<std::string> blocks = {"minecraft:stone",
                                           "minecraft:stone",
                                           "minecraft:stone",
                                           "minecraft:chest[facing=east]",
                                           "minecraft:honey_block",
                                           "minecraft:soul_sand",
                                           "minecraft:mud",
                                           "minecraft:cobweb",
                                           "minecraft:stone_slab[type=bottom]",
                                           "minecraft:stone_slab[type=top]",
                                           "minecraft:oak_stairs[facing=east,half=bottom]",
                                           "minecraft:oak_stairs[facing=south,half=top]",
                                           "minecraft:white_carpet",
                                           "minecraft:snow[layers=3]",
                                           "minecraft:cauldron",
                                           "minecraft:anvil",
                                           "minecraft:end_rod[facing=up]",
                                           "minecraft:lantern",
                                           "minecraft:enchanting_table",
                                           "minecraft:daylight_detector",
                                           "minecraft:cake",
                                           "minecraft:slime_block",
                                           "minecraft:ice"};
        if (with_large) {
            for (const char* b : {"minecraft:oak_fence", "minecraft:cobblestone_wall",
                                  "minecraft:oak_fence[north=true,east=true]", "minecraft:oak_fence_gate[facing=north]"})
                blocks.emplace_back(b);
        }
        for (i32 cx = -2; cx <= 1; ++cx)
            for (i32 cz = -1; cz <= 1; ++cz) dim.ticking(cx, cz);
        for (i32 x = kX0; x < kX0 + kSide; ++x)
            for (i32 y = kY0; y < kY0 + kHeight; ++y)
                for (i32 z = kZ0; z < kZ0 + kSide; ++z)
                    if (rng() % 100 < 35) dim.set_block(x, y, z, state_of(blocks[rng() % blocks.size()]));
        // ...and a few where one section of the chunk ends and the next - air and nothing else,
        // but for these - begins: what a query may skip is decided section by section.
        for (i32 x = kX0; x < kX0 + kSide; ++x)
            for (i32 z = kZ0; z < kZ0 + kSide; ++z)
                if (rng() % 100 < 12) dim.set_block(x, rng() % 3 ? kSeam - 1 : kSeam, z, state_of(blocks[rng() % blocks.size()]));
        EXPECT_EQ(dim.has_large_shapes(), with_large);
    }

    double uniform(double lo, double hi) { return lo + (hi - lo) * (static_cast<double>(rng() >> 11) * 0x1.0p-53); }
    // A coordinate inside the arena along an axis that starts at `origin`: anywhere, or on a
    // sixteenth and within a few margins of it.
    double coordinate(i32 origin, i32 extent) {
        if (rng() % 3 == 0) return uniform(origin - 1.0, origin + extent + 1.0);
        static constexpr double kOff[] = {0.0,     1.0e-7, -1.0e-7, 5.0e-8,      -5.0e-8,     2.0e-7,
                                          -2.0e-7, 1.0e-6, -1.0e-6, 9.999999e-8, 1.0000001e-7};
        const double grid = static_cast<double>(origin) + static_cast<double>(rng() % static_cast<u64>(extent * 16 + 1)) / 16.0;
        return grid + kOff[rng() % std::size(kOff)];
    }
    Aabb box() {
        static constexpr float kSizes[][2] = {{0.25f, 0.25f}, {0.98f, 0.98f}, {0.5f, 1.975f},
                                              {0.9f, 0.9f},   {0.6f, 1.8f},   {1.4f, 0.9f}};
        const auto& size = kSizes[rng() % std::size(kSizes)];
        const double y = rng() % 6 == 0    ? coordinate(kSeam - 2, 5)
                         : rng() % 40 == 0 ? uniform(0.0, 40.0)
                                           : coordinate(kY0, kHeight);
        Aabb b = entity_box(coordinate(kX0, kSide), y, coordinate(kZ0, kSide), size[0], size[1]);
        if (rng() % 4 == 0) {  // ...or one whose faces themselves are on the grid
            const u32 a = static_cast<u32>(rng() % 3);
            const double shift = coordinate(a == 0 ? kX0 : a == 1 ? kY0 : kZ0, a == 1 ? kHeight : kSide) - b.min[a];
            b.min[a] += shift, b.max[a] += shift;
        }
        return b;
    }
    double component() {
        switch (rng() % 8) {
            case 0: return 0.0;
            case 1: return uniform(-2.0e-7, 2.0e-7);
            case 2: return uniform(-1.0e-4, 1.0e-4);
            case 3: return uniform(-2.5, 2.5);
            case 4: return rng() % 2 ? -0.0784000015258789 : -0.04;
            default: return uniform(-0.6, 0.6);
        }
    }
};

void against_the_long_way(bool with_large, u32 rounds) {
    Arena a(with_large);
    CollideScratch scratch;
    u32 collided = 0, stepped = 0, blocked = 0, supported = 0, landed = 0;
    for (u32 i = 0; i < rounds; ++i) {
        const Aabb box = a.box();
        const double move[3] = {a.component(), a.component(), a.component()};
        static constexpr float kSteps[] = {0.0f, 0.0f, 0.6f, 0.6f, 1.0f, 1.5f};
        const float step = kSteps[a.rng() % std::size(kSteps)];
        const bool on_ground = a.rng() % 2;
        double fast[3], slow_out[3];
        collide(a.dim, box, move, step, on_ground, fast, scratch);
        slow::collide(a.dim, box, move, step, on_ground, slow_out);
        for (u32 k = 0; k < 3; ++k) {
            ASSERT_EQ(bits(fast[k]), bits(slow_out[k]))
                << "round " << i << " axis " << k << ": " << fast[k] << " against " << slow_out[k] << "; box min (" << box.min[0]
                << ", " << box.min[1] << ", " << box.min[2] << ") move (" << move[0] << ", " << move[1] << ", " << move[2]
                << ") step " << step;
        }
        collided += fast[0] != move[0] || fast[1] != move[1] || fast[2] != move[2];
        stepped += fast[1] > 0 && move[1] <= 0;

        const bool free = no_collision(a.dim, box);
        ASSERT_EQ(free, slow::no_collision(a.dim, box)) << "round " << i;
        blocked += !free;

        Aabb feet = box;
        feet.max[1] = feet.min[1];
        feet.min[1] -= 1.0e-6;
        const double x = (box.min[0] + box.max[0]) / 2, z = (box.min[2] + box.max[2]) / 2;
        i32 p[3] = {0, 0, 0}, q[3] = {0, 0, 0};
        const bool found = supporting_block(a.dim, feet, x, box.min[1], z, p);
        ASSERT_EQ(found, slow::supporting_block(a.dim, feet, x, box.min[1], z, q)) << "round " << i;
        if (found) {
            ASSERT_TRUE(p[0] == q[0] && p[1] == q[1] && p[2] == q[2]) << "round " << i;
        }
        supported += found;

        // EntityType.getYOffset at a cell of the arena: where a spawn egg's mob of one of these
        // sizes stands, put into that cell or on top of the one under it.
        static constexpr float kBodies[][2] = {{0.9f, 0.9f}, {1.4f, 2.7f}, {0.6f, 1.95f}, {1.95f, 2.2f}, {0.25f, 0.25f}};
        const auto& body = kBodies[a.rng() % std::size(kBodies)];
        const i32 cx = Arena::kX0 + static_cast<i32>(a.rng() % Arena::kSide);
        const i32 cy = Arena::kY0 - 1 + static_cast<i32>(a.rng() % (Arena::kHeight + 2));
        const i32 cz = Arena::kZ0 + static_cast<i32>(a.rng() % Arena::kSide);
        const bool up = a.rng() % 2;
        const Aabb spawned = entity_box(cx + 0.5, static_cast<double>(cy + 1), cz + 0.5, body[0], body[1]);
        const double offset = spawn_offset(a.dim, cx, cy, cz, up, spawned, scratch);
        ASSERT_EQ(bits(offset), bits(slow::spawn_offset(a.dim, cx, cy, cz, up, spawned)))
            << "round " << i << " cell (" << cx << ", " << cy << ", " << cz << ") moved up " << up;
        landed += offset != (up ? -1.0 : 0.0);
    }
    // The rounds have to have met something, or the comparison says nothing.
    EXPECT_GT(collided, rounds / 4);
    EXPECT_GT(stepped, rounds / 400);
    EXPECT_GT(blocked, rounds / 4);
    EXPECT_GT(supported, rounds / 20);
    EXPECT_GT(landed, rounds / 10);
}

}  // namespace

// Things thrown into a patch of world that keeps changing, each ticked twice: in full, and
// as the server does it - at rest when it lies still, woken when a block within reach is set.
// The two have to agree to the bit after every tick: that is what makes "at rest" a way of
// computing the same thing and not another behaviour. It fails if phys::item_rests() calls
// something at rest that is not, or if phys::block_in_reach() misses a block that matters.
float a_quarter(void*) { return 0.25f; }  // RandomSource.nextFloat for an item a block was put on: any number, the same twice

TEST(Physics, AtRestOrNotItIsTheSameTick) {
    Arena a(true);
    phys::Scratch scratch;
    phys::World world{a.dim, scratch};
    const phys::Kind kCow = phys::living_kind(entity_type::kCow);
    const phys::Kind kStand = phys::living_kind(entity_type::kArmorStand);
    struct Thing {
        const phys::Kind* kind;
        phys::Motion full, lazy;  // ticked in full every tick; with rest
        phys::Body full_body, lazy_body;
        bool asleep = false;
        phys::ItemRest rest;
    };
    std::vector<Thing> things;
    for (u32 i = 0; i < 260; ++i) {
        Thing t;
        t.kind = i % 4 == 3 ? &kCow : i % 9 == 4 ? &kStand : &phys::kItem;
        t.full = {{a.uniform(Arena::kX0 - 1.0, Arena::kX0 + Arena::kSide + 1.0), a.uniform(-54.0, -50.0),
                   a.uniform(Arena::kZ0 - 1.0, Arena::kZ0 + Arena::kSide + 1.0)},
                  {a.uniform(-0.2, 0.2), a.uniform(-0.1, 0.2), a.uniform(-0.2, 0.2)}};
        t.lazy = t.full;
        things.push_back(t);
    }
    const std::vector<std::string> blocks = {"minecraft:air",
                                             "minecraft:air",
                                             "minecraft:air",
                                             "minecraft:stone",
                                             "minecraft:stone",
                                             "minecraft:oak_fence",
                                             "minecraft:cobblestone_wall",
                                             "minecraft:slime_block",
                                             "minecraft:honey_block",
                                             "minecraft:cobweb",
                                             "minecraft:stone_slab[type=bottom]",
                                             "minecraft:oak_stairs[facing=east,half=bottom]",
                                             "minecraft:ice",
                                             "minecraft:soul_sand",
                                             "minecraft:white_carpet",
                                             "minecraft:chest[facing=east]",
                                             "minecraft:water",
                                             "minecraft:water[level=3]",
                                             "minecraft:water[level=8]",
                                             "minecraft:lava",
                                             "minecraft:lava[level=4]",
                                             "minecraft:stone_slab[type=bottom,waterlogged=true]"};
    u64 rested = 0, woken = 0, asked = 0;
    for (u32 tick = 1; tick <= 900; ++tick) {
        // The world changes: a block somewhere in the patch, and often right where something lies.
        if (tick % 3 == 0) {
            i32 x = Arena::kX0 + static_cast<i32>(a.rng() % Arena::kSide),
                y = Arena::kY0 - 1 + static_cast<i32>(a.rng() % (Arena::kHeight + 1)),
                z = Arena::kZ0 + static_cast<i32>(a.rng() % Arena::kSide);
            if (a.rng() % 2) {
                const Thing& near = things[a.rng() % things.size()];
                x = static_cast<i32>(std::floor(near.full.pos[0])) + static_cast<i32>(a.rng() % 5) - 2;
                y = static_cast<i32>(std::floor(near.full.pos[1])) + static_cast<i32>(a.rng() % 5) - 3;
                z = static_cast<i32>(std::floor(near.full.pos[2])) + static_cast<i32>(a.rng() % 5) - 2;
            }
            a.dim.ticking(x >> 4, z >> 4);
            if (a.dim.set_block(x, y, z, state_of(blocks[a.rng() % blocks.size()]))) {
                for (Thing& t : things) {
                    if (!t.asleep || !phys::block_in_reach(t.lazy, t.kind->width, t.kind->height, x, y, z)) continue;
                    t.asleep = false;
                    ++woken;
                }
            }
            a.dim.update_light();
            a.dim.end_tick();
        }
        for (u32 i = 0; i < things.size(); ++i) {
            Thing& t = things[i];
            const i32 id = static_cast<i32>(i) + 1;
            const bool item = t.kind == &phys::kItem;
            // In full.
            if (item)
                phys::item_tick(world, t.full, t.full_body, tick, id, a_quarter, nullptr);
            else
                phys::living_tick(world, *t.kind, t.full, t.full_body);
            // With rest.
            if (t.asleep) {
                ++rested;
                t.lazy_body.needs_sync = item && phys::item_rest_tick(t.lazy.vel[0], t.lazy.vel[1], t.lazy.vel[2],
                                                                      static_cast<u32>(static_cast<i32>(tick) + id) & 3, t.rest);
            } else if (item) {
                const phys::Motion before = t.lazy;
                phys::item_tick(world, t.lazy, t.lazy_body, tick, id, a_quarter, nullptr);
                const bool needs_sync = t.lazy_body.needs_sync;
                t.lazy_body.needs_sync = false;
                if (((static_cast<i32>(tick) + id) & 3) == 0 && t.lazy_body.on_ground && before.pos[0] == t.lazy.pos[0] &&
                    before.pos[1] == t.lazy.pos[1] && before.pos[2] == t.lazy.pos[2]) {
                    ++asked;
                    t.asleep = phys::item_rests(world, t.lazy, t.lazy_body, tick, id, t.rest);
                }
                t.lazy_body.needs_sync = needs_sync;
            } else {
                const phys::Motion before = t.lazy;
                const phys::Body body_before = t.lazy_body;
                phys::living_tick(world, *t.kind, t.lazy, t.lazy_body);
                t.asleep = body_before.on_ground && before.vel[0] == 0 && before.vel[2] == 0 &&
                           phys::same_state(before, body_before, t.lazy, t.lazy_body);
            }
            ASSERT_TRUE(phys::same_state(t.full, t.full_body, t.lazy, t.lazy_body))
                << "tick " << tick << ", thing " << i << (item ? " (an item)" : " (alive)") << (t.asleep ? ", at rest" : "")
                << ": in full at (" << t.full.pos[0] << ", " << t.full.pos[1] << ", " << t.full.pos[2] << ") moving ("
                << t.full.vel[0] << ", " << t.full.vel[1] << ", " << t.full.vel[2] << "), with rest at (" << t.lazy.pos[0] << ", "
                << t.lazy.pos[1] << ", " << t.lazy.pos[2] << ") moving (" << t.lazy.vel[0] << ", " << t.lazy.vel[1] << ", "
                << t.lazy.vel[2] << ")";
            t.full_body.needs_sync = t.lazy_body.needs_sync = false;  // (whoever tells the viewers takes it)
            t.full_body.sync_position = t.lazy_body.sync_position = false;
            for (phys::Body* b : {&t.full_body, &t.lazy_body})  // (...and whoever keeps its health)
                b->burned = b->extinguished = false, b->splashed = 0, b->splash_volume = 0, b->lava_hurts = 0;
        }
    }
    EXPECT_GT(rested, 50000u);
    EXPECT_GT(woken, 200u);
    std::printf("            %llu ticks at rest, %llu times woken by a block, asked %llu times\n",
                static_cast<unsigned long long>(rested), static_cast<unsigned long long>(woken),
                static_cast<unsigned long long>(asked));
}

TEST(Collide, TheShortcutsAnswerAsTheLongWayDoes) { against_the_long_way(false, 150000); }
TEST(Collide, TheSameWithFencesAndWallsAbout) { against_the_long_way(true, 150000); }

// BlockGetter.clip with ClipContext.Block.OUTLINE: what a look is stopped by - an outline where
// nothing collides (a torch), not the air over half a block, a fluid if that is asked for.
TEST(Collide, ALookIsStoppedByWhatItMeets) {
    FlatGenerator gen(-64, 24, FlatGenerator::default_layers(), 39);
    Dimension dim(proto::reg::dimension_type::kOverworld, "minecraft:overworld", gen, proto::kNoCompression);
    dim.ticking(0, 0);
    const auto put = [&](i32 x, i32 y, i32 z, std::string_view spec) {
        ASSERT_TRUE(dim.set_block(x, y, z, state_of(spec))) << spec;
    };
    const auto look = [&](double x, double y, double z, double tx, double ty, double tz, ClipFluid fluid = ClipFluid::kNone) {
        const double from[3] = {x, y, z}, to[3] = {tx, ty, tz};
        return clip_outline(dim, from, to, fluid);
    };
    // Down at the ground (its top is -60): the side that is met is the top.
    ClipHit h = look(3.5, -57.0, 3.5, 3.5, -62.0, 3.5);
    EXPECT_TRUE(h.hit && !h.inside);
    EXPECT_EQ(std::tie(h.x, h.y, h.z), std::tuple(3, -61, 3));
    EXPECT_EQ(h.face, kUp);
    EXPECT_EQ(h.at[1], -60.0);
    // Along the ground, into nothing.
    EXPECT_FALSE(look(3.5, -59.0, 3.5, 8.5, -59.0, 3.5).hit);
    EXPECT_FALSE(look(3.5, -59.0, 3.5, 3.5, -59.0, 3.5).hit);  // (nowhere)
    // A stone in the way: its west side. From inside it: at once, facing back.
    put(6, -60, 3, "minecraft:stone");
    h = look(3.5, -59.5, 3.5, 8.5, -59.5, 3.5);
    EXPECT_TRUE(h.hit);
    EXPECT_EQ(std::tie(h.x, h.y, h.z, h.face), std::tuple(6, -60, 3, static_cast<u8>(kWest)));
    EXPECT_EQ(h.at[0], 6.0);
    h = look(6.5, -59.5, 3.5, 8.5, -59.5, 3.5);
    EXPECT_TRUE(h.hit && h.inside);
    EXPECT_EQ(std::tie(h.x, h.face), std::tuple(6, static_cast<u8>(kWest)));  // (it looks east: the opposite)
    // A torch stops no body and stops a look; over a slab's lower half the look goes on.
    put(5, -60, 5, "minecraft:torch");
    put(6, -60, 5, "minecraft:stone");
    h = look(3.5, -59.8, 5.5, 8.5, -59.8, 5.5);
    EXPECT_EQ(std::tie(h.hit, h.x), std::tuple(true, 5));
    put(5, -60, 5, "minecraft:stone_slab[type=bottom]");
    h = look(3.5, -59.3, 5.5, 8.5, -59.3, 5.5);
    EXPECT_EQ(std::tie(h.hit, h.x), std::tuple(true, 6));
    h = look(3.5, -59.8, 5.5, 8.5, -59.8, 5.5);
    EXPECT_EQ(std::tie(h.hit, h.x, h.face), std::tuple(true, 5, static_cast<u8>(kWest)));
    // Water: nothing to a look that asks for no fluid, the nearer thing to one that does - a
    // source as high as it stands, what flows only if any fluid is asked for.
    put(5, -60, 8, "minecraft:water");
    put(6, -60, 8, "minecraft:water[level=3]");
    EXPECT_EQ(look(5.5, -57.0, 8.5, 5.5, -62.0, 8.5).y, -61);  // through it, to the ground
    h = look(5.5, -57.0, 8.5, 5.5, -62.0, 8.5, ClipFluid::kSourceOnly);
    EXPECT_EQ(std::tie(h.hit, h.y, h.face), std::tuple(true, -60, static_cast<u8>(kUp)));
    EXPECT_EQ(h.at[1], -60.0 + static_cast<double>(8.0f / 9.0f));
    EXPECT_EQ(look(6.5, -57.0, 8.5, 6.5, -62.0, 8.5, ClipFluid::kSourceOnly).y, -61);
    EXPECT_EQ(look(6.5, -57.0, 8.5, 6.5, -62.0, 8.5, ClipFluid::kAny).y, -60);
    EXPECT_EQ(look(6.5, -57.0, 8.5, 6.5, -62.0, 8.5, ClipFluid::kWater).y, -60);
    put(6, -60, 8, "minecraft:lava");
    EXPECT_EQ(look(6.5, -57.0, 8.5, 6.5, -62.0, 8.5, ClipFluid::kWater).y, -61);
    EXPECT_EQ(look(6.5, -57.0, 8.5, 6.5, -62.0, 8.5, ClipFluid::kSourceOnly).y, -60);
    // A slab with water in it, from above: the water stands over the slab, so that is met -
    // and under more water a cell is full of it.
    put(8, -60, 8, "minecraft:stone_slab[type=bottom,waterlogged=true]");
    h = look(8.5, -57.0, 8.5, 8.5, -62.0, 8.5, ClipFluid::kSourceOnly);
    EXPECT_EQ(h.at[1], -60.0 + static_cast<double>(8.0f / 9.0f));
    EXPECT_EQ(look(8.5, -57.0, 8.5, 8.5, -62.0, 8.5).at[1], -59.5);  // (no fluid asked for: the slab's top)
    put(8, -59, 8, "minecraft:water");
    h = look(7.5, -59.05, 8.5, 9.5, -59.05, 8.5, ClipFluid::kSourceOnly);  // level, through the top ninth of the lower cell
    EXPECT_EQ(std::tie(h.hit, h.x, h.y, h.face), std::tuple(true, 8, -60, static_cast<u8>(kWest)));

    // A flower's outline is not in the middle of its cell: BlockState.getOffset moves it, by
    // where the block stands (these are what a 26.3 server answers: mcgen gamedata, `offset`).
    double off[3];
    ASSERT_TRUE(outline_offset(state_of("minecraft:poppy"), 1, 0, off));
    EXPECT_EQ(std::tuple(off[0], off[1], off[2]), std::tuple(0.11666667461395264, 0.0, -0.01666666567325592));
    ASSERT_TRUE(outline_offset(state_of("minecraft:poppy"), -30000000, 29999999, off));
    EXPECT_EQ(std::tuple(off[0], off[1], off[2]), std::tuple(-0.25, 0.0, 0.15000000596046448));
    ASSERT_TRUE(outline_offset(state_of("minecraft:pointed_dripstone"), 5, 11, off));
    EXPECT_EQ(std::tuple(off[0], off[1], off[2]), std::tuple(0.0833333432674408, 0.0, -0.125));  // (an eighth at the most)
    EXPECT_FALSE(outline_offset(state_of("minecraft:torch"), 5, 11, off));
    EXPECT_FALSE(outline_offset(state_of("minecraft:short_grass"), 5, 11, off));  // (its picture moves, its outline does not)
    // ...so a look along the west edge of the cell meets the poppy at (0, 0), which stands a
    // quarter of a block west and north of the middle, and not the one at (1, 0).
    put(0, -60, 0, "minecraft:poppy");
    put(1, -60, 0, "minecraft:poppy");
    ASSERT_TRUE(outline_offset(state_of("minecraft:poppy"), 0, 0, off));
    EXPECT_EQ(std::tuple(off[0], off[2]), std::tuple(-0.25, -0.25));
    EXPECT_TRUE(look(0.25, -57.0, 0.25, 0.25, -59.9, 0.25).y == -60);  // through where the moved outline is
    EXPECT_TRUE(look(0.5, -57.0, 0.5, 0.5, -59.9, 0.5).hit == false);  // the middle of the cell is free
    EXPECT_TRUE(look(1.5, -57.0, 0.5, 1.5, -59.9, 0.5).y == -60);      // (1, 0): a tenth east of the middle, still under the look
}

TEST(Collide, AFenceIsMetFromTheCellAbove) {
    // A fence post is 1.5 blocks tall: a box that stands in the cell above it, a quarter of a
    // block over the post's top, comes down on that top - the cell the fence is in is not
    // one the box reaches into. That is what the cells beside a query are read for.
    FlatGenerator gen(-64, 24, FlatGenerator::default_layers(), 39);
    Dimension dim(proto::reg::dimension_type::kOverworld, "minecraft:overworld", gen, proto::kNoCompression);
    dim.ticking(0, 0);
    EXPECT_FALSE(dim.has_large_shapes());
    ASSERT_TRUE(dim.set_block(8, -60, 8, state_of("minecraft:oak_fence")));
    EXPECT_TRUE(dim.has_large_shapes());
    CollideScratch scratch;
    const Aabb box = entity_box(8.5, -58.25, 8.5, 0.25f, 0.25f);
    const double fall[3] = {0.0, -0.5, 0.0};
    double out[3];
    collide(dim, box, fall, 0.0f, false, out, scratch);
    EXPECT_EQ(out[1], -0.25);
    i32 pos[3];
    Aabb feet = entity_box(8.5, -58.5, 8.5, 0.25f, 0.25f);
    feet.max[1] = feet.min[1];
    feet.min[1] -= 1.0e-6;
    ASSERT_TRUE(supporting_block(dim, feet, 8.5, -58.5, 8.5, pos));
    EXPECT_EQ(pos[1], -60);
}

TEST(Collide, AnEggsMobComesToRestOnWhatIsUnderIt) {
    // EntityType.getYOffset: put at the middle of a cell one block up, it comes down on what that
    // cell holds - and, when it was put on top of the block that was clicked, on that block -
    // falling one block at most, two when moved up. On grass, whose top is at -60.
    FlatGenerator gen(-64, 24, FlatGenerator::default_layers(), 39);
    Dimension dim(proto::reg::dimension_type::kOverworld, "minecraft:overworld", gen, proto::kNoCompression);
    dim.ticking(0, 0);
    ASSERT_TRUE(dim.set_block(8, -60, 9, state_of("minecraft:stone_slab[type=bottom]")));
    ASSERT_TRUE(dim.set_block(8, -60, 10, state_of("minecraft:white_carpet")));
    ASSERT_TRUE(dim.set_block(8, -60, 11, state_of("minecraft:oak_fence")));
    ASSERT_TRUE(dim.set_block(8, -60, 13, state_of("minecraft:short_grass")));
    ASSERT_TRUE(dim.set_block(9, -60, 14, state_of("minecraft:stone")));  // beside the cell, as tall as a golem's knees
    ASSERT_TRUE(dim.set_block(9, -59, 14, state_of("minecraft:stone")));
    CollideScratch scratch;
    const auto offset = [&](i32 x, i32 y, i32 z, bool moved_up, float width, float height) {
        const Aabb box = entity_box(x + 0.5, static_cast<double>(y + 1), z + 0.5, width, height);
        const double fast = spawn_offset(dim, x, y, z, moved_up, box, scratch);
        EXPECT_EQ(bits(fast), bits(slow::spawn_offset(dim, x, y, z, moved_up, box)));
        return fast;
    };
    EXPECT_EQ(offset(8, -60, 8, true, 0.9f, 0.9f), 0.0);       // on top of the grass that was clicked
    EXPECT_EQ(offset(8, -59, 9, true, 0.9f, 0.9f), -0.5);      // ...of a bottom slab
    EXPECT_EQ(offset(8, -59, 10, true, 0.9f, 0.9f), -0.9375);  // ...of a carpet
    EXPECT_EQ(offset(8, -59, 11, true, 0.9f, 0.9f), 0.5);      // ...of a fence, which is 1.5 tall
    EXPECT_EQ(offset(8, -58, 12, false, 0.9f, 0.9f), 0.0);     // beside a block, air under it: it stays (and falls later)
    EXPECT_EQ(offset(8, -60, 13, false, 0.9f, 0.9f), 0.0);     // into a plant, which stops nothing
    EXPECT_EQ(offset(8, -60, 14, true, 1.4f, 2.7f), 0.0);      // wide: only its own column counts, not the stone beside it
}

TEST(Collide, TwoBlocksAsNearTheLaterOneIsStoodOn) {
    // Level.findSupportingBlock on a seam: of two blocks as near, the one Vec3i.compareTo puts
    // last - recorded on a vanilla server as the friction of an item sliding along such a seam
    // (the scenarios item_seam_* and item_corner_tie_*).
    FlatGenerator gen(-64, 24, FlatGenerator::default_layers(), 39);
    Dimension dim(proto::reg::dimension_type::kOverworld, "minecraft:overworld", gen, proto::kNoCompression);
    dim.ticking(0, 0);
    Aabb feet = entity_box(8.0, -60.0, 8.0, 0.25f, 0.25f);  // on the corner of four blocks
    feet.max[1] = feet.min[1];
    feet.min[1] -= 1.0e-6;
    i32 pos[3];
    ASSERT_TRUE(supporting_block(dim, feet, 8.0, -60.0, 8.0, pos));
    EXPECT_EQ(pos[0], 8);
    EXPECT_EQ(pos[1], -61);
    EXPECT_EQ(pos[2], 8);
}

TEST(Physics, TheSineTableIsTheJvms) {
    // Mth.SIN: 65536 floats of (float) Math.sin(i / 10430.378350470453). The checksum (FNV-1a
    // over the floats' bytes) is what a JVM computes - Java 25, Math.sin and StrictMath.sin alike.
    u64 h = 0xcbf29ce484222325ull;
    for (u32 i = 0; i < 65536; ++i) {
        const u32 b = std::bit_cast<u32>(static_cast<float>(std::sin(static_cast<double>(i) / 10430.378350470453)));
        for (u32 k = 0; k < 4; ++k) {
            h ^= (b >> (8 * k)) & 0xFF;
            h *= 0x100000001b3ull;
        }
    }
    EXPECT_EQ(h, 0x115b15b6ad9d2006ull);
}
