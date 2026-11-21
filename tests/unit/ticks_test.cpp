// Scheduled ticks and the counts behind random ticks.
//
// The order scheduled ticks run in is held against a model of vanilla's LevelTicks written out the
// long way from the 26.3 jar (LevelTicks.collectTicks / drainContainers / drainFromCurrentContainer,
// LevelChunkTicks, ScheduledTick.DRAIN_ORDER and INTRA_TICK_DRAIN_ORDER): sorted lists and sets
// where the server has a heap and a flat table, and nothing shared with it.
//
// The random-tick counts are held against counting from nothing, and what they are for - "this
// section's random ticks can do nothing" - against the question asked of every cell.
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <tuple>
#include <vector>

#include "proto/frame.hpp"
#include "world/chunk_io.hpp"
#include "world/dimension.hpp"
#include "world/ticks.hpp"

using namespace world;
namespace blk = proto::gen::block;

namespace {

// ---- vanilla's LevelTicks, the long way
struct ModelTick {
    i64 trigger;
    i32 priority;
    i64 sub;
    i32 x, y, z;
    u32 type;
};
// ScheduledTick.DRAIN_ORDER
bool drain_before(const ModelTick& a, const ModelTick& b) {
    return std::tie(a.trigger, a.priority, a.sub) < std::tie(b.trigger, b.priority, b.sub);
}
// ScheduledTick.INTRA_TICK_DRAIN_ORDER.compare(a, b)
int intra(const ModelTick& a, const ModelTick& b) {
    if (a.priority != b.priority) return a.priority < b.priority ? -1 : 1;
    return a.sub < b.sub ? -1 : a.sub > b.sub ? 1 : 0;
}
struct ModelContainer {            // LevelChunkTicks
    std::vector<ModelTick> queue;  // kept in DRAIN_ORDER: front() is peek()
    std::set<std::tuple<i32, i32, i32, u32>> per_position;
    bool ticks = true;
    const ModelTick* peek() const { return queue.empty() ? nullptr : &queue.front(); }
    ModelTick poll() {
        const ModelTick t = queue.front();
        queue.erase(queue.begin());
        per_position.erase({t.x, t.y, t.z, t.type});
        return t;
    }
    bool schedule(const ModelTick& t) {
        if (!per_position.insert({t.x, t.y, t.z, t.type}).second) return false;
        queue.insert(std::upper_bound(queue.begin(), queue.end(), t, drain_before), t);
        return true;
    }
};
struct ModelLevel {  // LevelTicks
    std::map<std::pair<i32, i32>, ModelContainer> all;
    i64 sub = 0;
    bool schedule(i32 x, i32 y, i32 z, u32 type, i64 trigger, i32 priority) {
        return all[{x >> 4, z >> 4}].schedule({trigger, priority, ++sub, x, y, z, type});
    }
    std::vector<ModelTick> tick(i64 now, u32 max) {
        std::vector<ModelTick> run;
        // sortContainersToTick
        std::vector<ModelContainer*> to_tick;
        for (auto& [at, c] : all)
            if (c.peek() && c.peek()->trigger <= now && c.ticks) to_tick.push_back(&c);
        const auto best = [&]() -> ModelContainer* {  // containersToTick.peek()
            ModelContainer* b = nullptr;
            for (ModelContainer* c : to_tick)
                if (!b || intra(*c->peek(), *b->peek()) < 0) b = c;
            return b;
        };
        // drainContainers
        while (run.size() < max) {
            ModelContainer* top = best();
            if (!top) break;
            to_tick.erase(std::find(to_tick.begin(), to_tick.end(), top));
            run.push_back(top->poll());
            // drainFromCurrentContainer
            if (run.size() < max) {
                const ModelContainer* next_best = best();
                const ModelTick* from_next = next_best ? next_best->peek() : nullptr;
                while (run.size() < max) {
                    const ModelTick* cur = top->peek();
                    if (!cur || cur->trigger > now) break;
                    if (from_next && intra(*cur, *from_next) > 0) break;
                    run.push_back(top->poll());
                }
            }
            const ModelTick* next = top->peek();
            if (next && next->trigger <= now && run.size() < max) to_tick.push_back(top);
        }
        return run;
    }
};

std::unique_ptr<Chunk> chunk_at(i32 x, i32 z) {
    auto c = std::make_unique<Chunk>();
    c->x = x;
    c->z = z;
    c->ticking = true;
    c->block_refs = 1;
    return c;
}

constexpr i32 kMinY = -64;
constexpr u32 kHeight = 384, kTypes = 1286;

}  // namespace

TEST(ScheduledTicks, OnePerPositionAndType) {
    LevelTicks ticks(kTypes, kMinY, kHeight);
    auto c = chunk_at(2, -3);
    EXPECT_FALSE(ticks.has(*c, 33, 70, -40, 5));
    ticks.schedule(*c, 33, 70, -40, 5, 10);
    ticks.schedule(*c, 33, 70, -40, 5, 4);  // dropped: the first one stands, late as it is
    ticks.schedule(*c, 33, 70, -40, 6, 4);  // another type at the same place is another tick
    ticks.schedule(*c, 33, 71, -40, 5, 4);
    EXPECT_EQ(ticks.count(), 3u);
    EXPECT_TRUE(ticks.has(*c, 33, 70, -40, 5));
    EXPECT_FALSE(ticks.has(*c, 34, 70, -40, 5));
    // Outside the world, or of a type that is none: nothing.
    ticks.schedule(*c, 33, kMinY - 1, -40, 5, 4);
    ticks.schedule(*c, 33, kMinY + static_cast<i32>(kHeight), -40, 5, 4);
    ticks.schedule(*c, 33, 70, -40, kTypes, 4);
    EXPECT_EQ(ticks.count(), 3u);

    LevelTicks::Due due;
    ticks.collect(4);
    ASSERT_TRUE(ticks.next(due));
    EXPECT_EQ(due.type, 6u);  // scheduled before the one above it
    EXPECT_EQ(due.x, 33);
    EXPECT_EQ(due.y, 70);
    EXPECT_EQ(due.z, -40);
    // Collected, it is no longer scheduled - a block may ask for its next tick from inside this one.
    EXPECT_FALSE(ticks.has(*c, 33, 70, -40, 6));
    EXPECT_TRUE(ticks.will_tick(33, 71, -40, 5));
    EXPECT_FALSE(ticks.will_tick(33, 70, -40, 6));  // it is running: no longer "to run"
    EXPECT_FALSE(ticks.will_tick(33, 70, -40, 5));  // due at 10
    ticks.schedule(*c, 33, 70, -40, 6, 4);          // due now, and still it waits for the next collect
    ASSERT_TRUE(ticks.next(due));
    EXPECT_EQ(due.y, 71);
    EXPECT_FALSE(ticks.will_tick(33, 71, -40, 5));
    EXPECT_FALSE(ticks.next(due));
    ticks.finish();
    ticks.collect(5);
    ASSERT_TRUE(ticks.next(due));
    EXPECT_EQ(due.type, 6u);
    EXPECT_FALSE(ticks.next(due));
    ticks.finish();
    EXPECT_EQ(ticks.count(), 1u);
    ticks.release(*c);
    EXPECT_EQ(ticks.count(), 0u);
    EXPECT_EQ(c->block_ticks, nullptr);
}

TEST(ScheduledTicks, PriorityThenTheOrderOfScheduling) {
    LevelTicks ticks(kTypes, kMinY, kHeight);
    auto a = chunk_at(0, 0), b = chunk_at(1, 0);
    // Due together: by priority, then as they were scheduled - whichever chunk they are in.
    ticks.schedule(*a, 1, 0, 1, 1, 7, 0);
    ticks.schedule(*b, 17, 0, 1, 2, 7, 0);
    ticks.schedule(*a, 2, 0, 1, 3, 7, -1);
    ticks.schedule(*b, 18, 0, 1, 4, 7, 3);
    ticks.schedule(*a, 3, 0, 1, 5, 7, 0);
    ticks.schedule(*b, 19, 0, 1, 6, 7, -3);
    ticks.schedule(*a, 4, 0, 1, 7, 8, -3);  // not due yet
    std::vector<u32> order;
    LevelTicks::Due due;
    ticks.collect(7);
    while (ticks.next(due)) order.push_back(due.type);
    ticks.finish();
    EXPECT_EQ(order, (std::vector<u32>{6, 3, 1, 2, 5, 4}));
    ticks.release(*a);
    ticks.release(*b);
}

TEST(ScheduledTicks, RunInVanillasOrder) {
    for (u32 seed = 1; seed <= 6; ++seed) {
        std::mt19937 rng(seed);
        LevelTicks ticks(kTypes, kMinY, kHeight);
        ModelLevel model;
        std::vector<std::unique_ptr<Chunk>> chunks;
        for (i32 i = 0; i < 6; ++i) chunks.push_back(chunk_at(i % 3 - 1, i / 3));
        const auto schedule = [&](i64 now) {
            Chunk& c = *chunks[rng() % chunks.size()];
            // Few places, so that many are asked for twice.
            const i32 x = c.x * 16 + static_cast<i32>(rng() % 3), z = c.z * 16 + static_cast<i32>(rng() % 3);
            const i32 y = kMinY + static_cast<i32>(rng() % 4) * 100;
            const u32 type = 1 + rng() % 3;
            const i64 trigger = now + static_cast<i64>(rng() % 9);
            const i32 priority = static_cast<i32>(rng() % 7) - 3;
            const bool was = ticks.has(c, x, y, z, type);
            ticks.schedule(c, x, y, z, type, trigger, priority);
            const bool taken = model.schedule(x, y, z, type, trigger, priority);
            ASSERT_EQ(taken, !was);
            ASSERT_TRUE(ticks.has(c, x, y, z, type));
        };
        u64 ran = 0;
        for (i64 now = 1; now <= 400; ++now) {
            for (u32 k = rng() % 12; k--;) schedule(now);
            // Chunks stop ticking for a while and start again: what was due in between is overdue.
            if (rng() % 8 == 0) {
                Chunk& c = *chunks[rng() % chunks.size()];
                c.block_refs = c.block_refs ? 0 : 1;
                model.all[{c.x, c.z}].ticks = c.block_refs != 0;
            }
            const u32 max = rng() % 5 == 0 ? 1 + rng() % 6 : LevelTicks::kMaxPerTick;
            const std::vector<ModelTick> want = model.tick(now, max);
            ticks.collect(now, max);
            LevelTicks::Due due;
            for (const ModelTick& w : want) {
                ASSERT_TRUE(ticks.next(due)) << "seed " << seed << " at " << now;
                ASSERT_EQ(std::tie(due.x, due.y, due.z, due.type), std::tie(w.x, w.y, w.z, w.type))
                    << "seed " << seed << " at " << now;
                if (rng() % 4 == 0) schedule(now);  // what a tick schedules is not run with the ticks it was collected with
                ++ran;
            }
            ASSERT_FALSE(ticks.next(due)) << "seed " << seed << " at " << now;
            ticks.finish();
            u64 left = 0;
            for (const auto& [at, c] : model.all) left += c.queue.size();
            ASSERT_EQ(ticks.count(), left);
        }
        EXPECT_GT(ran, 1000u);
        for (auto& c : chunks) ticks.release(*c);
        EXPECT_EQ(ticks.count(), 0u);
    }
}

TEST(ScheduledTicks, AreSavedAsDelaysAndCountAgainWhenTheChunkTicks) {
    LevelTicks ticks(kTypes, kMinY, kHeight);
    auto c = chunk_at(-1, 4);
    ticks.schedule(*c, -16, 0, 64, 9, 1000 + 20, 0);
    ticks.schedule(*c, -15, 0, 64, 9, 1000 + 5, 2);
    ticks.schedule(*c, -14, 0, 64, 9, 1000 + 5, -1);
    ticks.schedule(*c, -13, 0, 64, 9, 1000 - 3, 0);  // overdue: its delay is negative
    base::ByteBuf bytes;
    LevelTicks::write(c->block_ticks, 1000, bytes);
    ASSERT_TRUE(ticks_valid({bytes.data, bytes.len}, kTypes, kHeight));
    // Written in the order they were scheduled in, whatever their priority (SUB_TICK_ORDERING).
    ASSERT_EQ(bytes.len, 5u + 4 * 9);
    EXPECT_EQ(bytes.data[0], 1);
    EXPECT_EQ(load_le<u32>(bytes.data + 1), 4u);
    const i32 delays[4] = {20, 5, 5, -3};
    const i8 priorities[4] = {0, 2, -1, 0};
    for (u32 i = 0; i < 4; ++i) {
        const u8* r = bytes.data + 5 + i * 9;
        EXPECT_EQ(load_le<u32>(r), ticks.pack(-16 + static_cast<i32>(i), 0, 64, 9));
        EXPECT_EQ(load_le<i32>(r + 4), delays[i]);
        EXPECT_EQ(static_cast<i8>(r[8]), priorities[i]);
    }
    // Bytes that are not ticks of this world.
    EXPECT_TRUE(ticks_valid({}, kTypes, kHeight));
    EXPECT_FALSE(ticks_valid({bytes.data, bytes.len - 1}, kTypes, kHeight));
    EXPECT_FALSE(ticks_valid({bytes.data, bytes.len}, 9, kHeight));  // a type that does not exist
    EXPECT_FALSE(ticks_valid({bytes.data, bytes.len}, kTypes, 64));  // a height that does not
    {
        std::vector<u8> bad(bytes.data, bytes.data + bytes.len);
        bad[5 + 8] = 4;  // a priority that is none
        EXPECT_FALSE(ticks_valid(bad, kTypes, kHeight));
        bad[5 + 8] = 0;
        bad[0] = 2;
        EXPECT_FALSE(ticks_valid(bad, kTypes, kHeight));
    }

    // Another server, much later: the chunk is loaded, and its ticks wait until it ticks.
    LevelTicks later(kTypes, kMinY, kHeight);
    auto d = chunk_at(-1, 4);
    d->block_refs = 0;
    later.read(*d, {bytes.data, bytes.len});
    EXPECT_TRUE(later.has(*d, -16, 0, 64, 9));
    later.schedule(*d, -16, 0, 64, 9, 5000);  // one per position: what was loaded counts
    later.schedule(*d, -12, 0, 64, 9, 5001);  // ...and what is new is there beside it
    LevelTicks::Due due;
    later.collect(6000);
    EXPECT_FALSE(later.next(due));  // the chunk does not tick
    later.finish();
    // Saved again before it ever ticked: what was loaded as it was, then the new one.
    base::ByteBuf again;
    LevelTicks::write(d->block_ticks, 6000, again);
    ASSERT_EQ(again.len, 5u + 5 * 9);
    EXPECT_EQ(0, std::memcmp(again.data + 5, bytes.data + 5, 4 * 9));
    EXPECT_EQ(load_le<i32>(again.data + 5 + 4 * 9 + 4), 5001 - 6000);

    d->block_refs = 1;
    later.unpack(*d, 7000);
    later.unpack(*d, 9000);  // (once)
    std::vector<std::pair<i64, i32>> ran;
    for (i64 now = 7000; now <= 7020; ++now) {
        later.collect(now);
        while (later.next(due)) ran.push_back({now, due.x});
        later.finish();
    }
    // What is overdue at once, the longest overdue first (the one scheduled for 5001, then the
    // loaded one that was three ticks late when it was saved); the others when their delay has
    // passed again - of two that are due together, the higher priority first.
    EXPECT_EQ(ran, (std::vector<std::pair<i64, i32>>{{7000, -12}, {7000, -13}, {7005, -14}, {7005, -15}, {7020, -16}}));
    ticks.release(*c);
    later.release(*d);
}

// A tick that waited in a chunk nobody simulated is long overdue when the chunk is saved - and
// the server that loads it starts its clock at zero. The time it is due at lies before that, as
// it may in vanilla (a long): it runs at once, and the longest overdue first.
TEST(ScheduledTicks, OverdueOnesRunAtOnceWhateverTheClockSays) {
    LevelTicks ticks(kTypes, kMinY, kHeight);
    auto c = chunk_at(2, 2);
    c->block_refs = 0;
    ticks.schedule(*c, 32, 0, 32, 9, 502);
    ticks.schedule(*c, 33, 0, 32, 9, 100, 3);  // overdue for longer, and of the lowest priority
    ticks.schedule(*c, 34, 0, 32, 9, 502, -1);
    ticks.schedule(*c, 35, 0, 32, 9, 5000);
    base::ByteBuf bytes;
    LevelTicks::write(c->block_ticks, 4000, bytes);
    EXPECT_EQ(load_le<i32>(bytes.data + 5 + 4), 502 - 4000);

    LevelTicks later(kTypes, kMinY, kHeight);
    auto d = chunk_at(2, 2);
    d->block_refs = 0;
    later.read(*d, {bytes.data, bytes.len});
    LevelTicks::Due due;
    for (i64 now = 1; now <= 5; ++now) {
        later.collect(now);
        EXPECT_FALSE(later.next(due));
        later.finish();
    }
    d->block_refs = 1;
    later.unpack(*d, 5);
    later.schedule(*d, 36, 0, 32, 9, 6);  // scheduled here and now: after what was overdue
    std::vector<std::pair<i64, i32>> ran;
    for (i64 now = 6; now <= 1100; ++now) {
        later.collect(now);
        while (later.next(due)) ran.push_back({now, due.x});
        later.finish();
    }
    EXPECT_EQ(ran, (std::vector<std::pair<i64, i32>>{{6, 33}, {6, 34}, {6, 32}, {6, 36}, {1005, 35}}));
    ticks.release(*c);
    later.release(*d);
}

namespace {
struct TempDir {
    std::string path;
    TempDir() {
        char name[] = "/tmp/ticks-test-XXXXXX";
        path = ::mkdtemp(name);
    }
    ~TempDir() { std::filesystem::remove_all(path); }
};
}  // namespace

TEST(ScheduledTicks, ComeBackWithTheirChunk) {
    TempDir dir;
    FlatGenerator gen(-64, 24, FlatGenerator::default_layers(), 39);
    const u32 sand = block_of(blk::kSand), lamp = block_of(blk::kRedstoneLamp);
    {
        Dimension dim(proto::reg::dimension_type::kOverworld, "minecraft:overworld", gen, proto::kNoCompression, nullptr,
                      dir.path);
        Chunk& c = dim.ticking(0, 0);
        dim.simulate(c, 1, 1);
        dim.update_light();
        dim.end_tick();
        dim.save(c, {});
        EXPECT_FALSE(c.dirty);
        dim.set_time(100);
        dim.schedule_tick(3, -60, 5, sand, 7);
        dim.schedule_tick(4, -60, 5, lamp, 2, -1);
        dim.schedule_tick(40000, -60, 5, sand, 7);  // where no chunk is: nothing
        EXPECT_TRUE(dim.has_scheduled_tick(3, -60, 5, sand));
        EXPECT_FALSE(dim.has_scheduled_tick(3, -60, 5, lamp));
        EXPECT_TRUE(c.dirty);  // a chunk with a tick it was not saved with is to be saved again
        dim.set_time(103);
        std::vector<Chunk*> all;
        dim.for_each([&](Chunk& each) { all.push_back(&each); });
        for (Chunk* each : all) dim.save(*each, {});
        for (Chunk* each : all) dim.unload(*each);
        EXPECT_EQ(dim.block_ticks().count(), 0u);
    }
    Dimension dim(proto::reg::dimension_type::kOverworld, "minecraft:overworld", gen, proto::kNoCompression, nullptr, dir.path);
    dim.set_time(5000);
    Chunk& c = dim.ticking(0, 0);
    EXPECT_TRUE(dim.has_scheduled_tick(3, -60, 5, sand));
    EXPECT_TRUE(dim.has_scheduled_tick(4, -60, 5, lamp));
    LevelTicks::Due due;
    dim.block_ticks().collect(6000);
    EXPECT_FALSE(dim.block_ticks().next(due));  // nobody simulates the chunk: its ticks have not begun to count
    dim.block_ticks().finish();
    dim.set_time(7000);
    dim.simulate(c, 1, 1);
    std::vector<std::pair<u64, u32>> ran;
    for (u64 now = 7000; now <= 7010; ++now) {
        dim.set_time(now);
        dim.block_ticks().collect(now);
        while (dim.block_ticks().next(due)) ran.push_back({now, due.type});
        dim.block_ticks().finish();
    }
    // Saved at 103: the lamp's tick was overdue by one, the sand's had four to go.
    EXPECT_EQ(ran, (std::vector<std::pair<u64, u32>>{{7000, lamp}, {7004, sand}}));
}

TEST(ScheduledTicks, AChunkSavedBeforeThereWereTicksIsStillRead) {
    FlatGenerator gen(-64, 24, FlatGenerator::default_layers(), 39);
    const Layout layout{-64, -4, 24, true};
    LoadedChunk made;
    std::string err;
    ASSERT_TRUE(load_chunk({}, 2, 3, layout, gen, made, err));
    base::ByteBuf now;
    encode_chunk(made.chunk, {}, now);
    ASSERT_EQ(now.data[0], 2);
    // Version 1 is version 2 without its last part.
    std::vector<u8> old(now.data, now.data + now.len - 4);
    old[0] = 1;
    LoadedChunk from_old, from_new;
    ASSERT_TRUE(load_chunk(old, 2, 3, layout, gen, from_old, err)) << err;
    EXPECT_TRUE(from_old.parts.ticks.empty());
    EXPECT_EQ(from_old.chunk.sections[0].ticking, 256u);  // counted when it is read: the grass on top
    EXPECT_EQ(from_old.chunk.ticking_sections, 1u);
    ASSERT_TRUE(load_chunk({now.data, now.len}, 2, 3, layout, gen, from_new, err)) << err;
    // With ticks: they come back as bytes, for the world to take; ticks that are none refuse the chunk.
    const u8 one[] = {1, 1, 0, 0, 0, /* what */ 0x21, 0x43, 0x02, 0x00, /* delay */ 5, 0, 0, 0, /* priority */ 0xFF};
    base::ByteBuf with;
    encode_chunk(made.chunk, {{}, {}, one}, with);
    LoadedChunk ticking;
    ASSERT_TRUE(load_chunk({with.data, with.len}, 2, 3, layout, gen, ticking, err)) << err;
    ASSERT_EQ(ticking.parts.ticks.size(), sizeof one);
    EXPECT_EQ(0, std::memcmp(ticking.parts.ticks.data(), one, sizeof one));
    u8 bad[sizeof one];
    std::memcpy(bad, one, sizeof one);
    bad[13] = 9;
    base::ByteBuf broken;
    encode_chunk(made.chunk, {{}, {}, bad}, broken);
    LoadedChunk refused;
    EXPECT_FALSE(load_chunk({broken.data, broken.len}, 2, 3, layout, gen, refused, err));
}

// ---------------------------------------------------------------------------- random ticks

namespace {

// The states the counts are about, and some they are not.
std::vector<BlockState> tick_states() {
    const BlockInfo& grass = block_info_of(blk::kGrassBlock);
    const BlockInfo& snow = block_info_of(blk::kSnow);
    std::vector<BlockState> v = {kAir,
                                 kAir,
                                 static_cast<BlockState>(blk::kStone),
                                 static_cast<BlockState>(blk::kDirt),
                                 static_cast<BlockState>(blk::kDirt),
                                 grass.alt[0],
                                 grass.alt[0],
                                 grass.alt[1],
                                 static_cast<BlockState>(blk::kMycelium),
                                 static_cast<BlockState>(blk::kWarpedNylium),
                                 static_cast<BlockState>(blk::kCopperBlock),
                                 static_cast<BlockState>(blk::kOxidizedCopper),
                                 static_cast<BlockState>(blk::kBuddingAmethyst),
                                 static_cast<BlockState>(blk::kGlass),
                                 static_cast<BlockState>(blk::kWater),
                                 static_cast<BlockState>(snow.first_state),      // one layer: grass lives under it
                                 static_cast<BlockState>(snow.first_state + 3),  // four: it does not
                                 static_cast<BlockState>(blk::kSnowBlock),
                                 static_cast<BlockState>(blk::kTorch)};
    return v;
}

struct Counts {
    u32 ticking = 0, always = 0, dying = 0, targets = 0;
    bool operator==(const Counts&) const = default;
};

// What a section's counts should be, asked of the world cell by cell.
Counts count_section(const Dimension& dim, const Chunk& c, u32 si) {
    Counts n;
    const i32 y0 = dim.min_y() + static_cast<i32>(si) * 16;
    for (i32 y = y0; y < y0 + 16; ++y) {
        for (i32 z = c.z * 16; z < c.z * 16 + 16; ++z) {
            for (i32 x = c.x * 16; x < c.x * 16 + 16; ++x) {
                const u32 r = state::kRandom[dim.block(x, y, z)], above = state::kRandom[dim.block(x, y + 1, z)];
                n.ticking += (r & state::kRandomTick) != 0;
                n.always += (r & state::kRandomAlways) != 0;
                n.dying += random_dies(r, above);
                n.targets += random_target(r, above);
            }
        }
    }
    return n;
}

// Could a random tick of the cell at (x, y, z) change anything? SpreadingSnowyBlock.randomTick,
// NyliumBlock.randomTick and the kinds that always may - whatever the light and the dice.
bool may_act(const Dimension& dim, i32 x, i32 y, i32 z) {
    const u32 r = state::kRandom[dim.block(x, y, z)];
    if (!(r & state::kRandomTick)) return false;
    if (r & state::kRandomAlways) return true;
    const u32 above = state::kRandom[dim.block(x, y + 1, z)];
    if (random_dies(r, above)) return true;
    if (!(r & state::kSpreader)) return false;
    for (i32 dy = -3; dy <= 1; ++dy)
        for (i32 dz = -1; dz <= 1; ++dz)
            for (i32 dx = -1; dx <= 1; ++dx)
                if (random_target(state::kRandom[dim.block(x + dx, y + dy, z + dz)],
                                  state::kRandom[dim.block(x + dx, y + dy + 1, z + dz)]))
                    return true;
    return false;
}

void check_counts(Dimension& dim, i32 r) {
    for (i32 cz = -r; cz <= r; ++cz) {
        for (i32 cx = -r; cx <= r; ++cx) {
            const Chunk& c = *dim.find(cx, cz);
            u32 active = 0;
            for (u32 si = 0; si < dim.sections(); ++si) {
                const Counts want = count_section(dim, c, si);
                const Counts have{c.sections[si].ticking, c.random[si].always, c.random[si].dying, c.random[si].targets};
                ASSERT_EQ(have, want) << "chunk " << cx << ", " << cz << " section " << si;
                ASSERT_EQ((c.ticking_sections >> si) & 1, want.ticking != 0);
                ASSERT_EQ((c.always_sections >> si) & 1, want.always != 0);
                ASSERT_EQ((c.dying_sections >> si) & 1, want.dying != 0);
                ASSERT_EQ((c.target_sections >> si) & 1, want.targets != 0);
                // A section that is not active: none of its cells could do anything.
                if ((c.active_sections >> si) & 1) {
                    ++active;
                    continue;
                }
                if (!want.ticking || !c.ticking) continue;
                const i32 y0 = dim.min_y() + static_cast<i32>(si) * 16;
                for (i32 y = y0; y < y0 + 16; ++y)
                    for (i32 z = cz * 16; z < cz * 16 + 16; ++z)
                        for (i32 x = cx * 16; x < cx * 16 + 16; ++x)
                            ASSERT_FALSE(may_act(dim, x, y, z))
                                << "a dormant section with work at " << x << ", " << y << ", " << z;
            }
            // Listed for random ticks: it ticks, somebody simulates it, a section has work (or, with
            // DCHECKs, cells that tick at all: those are run to be caught changing something).
            const u32 sections = kCheckDormantSections ? c.ticking_sections : c.active_sections;
            const bool listed =
                std::find(dim.random_chunks().begin(), dim.random_chunks().end(), &c) != dim.random_chunks().end();
            ASSERT_EQ(listed, c.ticking && c.sim_refs != 0 && sections != 0) << "chunk " << cx << ", " << cz;
            (void)active;
        }
    }
}

}  // namespace

TEST(RandomTicks, TheFlatWorldHasNothingToDo) {
    FlatGenerator gen(-64, 24, FlatGenerator::default_layers(), 39);
    Dimension dim(proto::reg::dimension_type::kOverworld, "minecraft:overworld", gen, proto::kNoCompression);
    for (i32 cz = -1; cz <= 1; ++cz)
        for (i32 cx = -1; cx <= 1; ++cx) dim.simulate(dim.ticking(cx, cz), 1, 1);
    const Chunk& c = *dim.find(0, 0);
    // Grass on top: 256 cells that tick at random in the section it lies in, and not one with anything to do.
    EXPECT_EQ(c.ticking_sections, 1u);
    EXPECT_EQ(c.sections[0].ticking, 256u);
    EXPECT_EQ(c.active_sections, 0u);
    check_counts(dim, 1);
    // A hole in the grass bares the dirt under it: soil that the grass around it can take. The section
    // has work - and so have the sections beside it in the chunks around, which hold grass too.
    dim.set_block(0, -61, 0, kAir);
    dim.set_block(0, -62, 0, kAir);  // (the dirt below that one is bare now)
    EXPECT_EQ(dim.find(0, 0)->random[0].targets, 1u);
    EXPECT_EQ(dim.find(0, 0)->active_sections, 1u);
    EXPECT_EQ(dim.find(-1, -1)->active_sections, 1u);
    check_counts(dim, 1);
    // Grown over again: nothing to do.
    dim.set_block(0, -63, 0, block_info_of(blk::kGrassBlock).alt[0]);
    EXPECT_EQ(dim.find(0, 0)->active_sections, 0u);
    EXPECT_EQ(dim.find(-1, -1)->active_sections, 0u);
    // Stone on grass: it dies. Only its own section has work.
    dim.set_block(20, -60, 5, static_cast<BlockState>(blk::kStone));
    EXPECT_EQ(dim.find(1, 0)->random[0].dying, 1u);
    EXPECT_EQ(dim.find(1, 0)->active_sections, 1u);
    EXPECT_EQ(dim.find(0, 0)->active_sections, 0u);
    // Copper weathers whatever is around it.
    dim.set_block(-10, 0, 5, static_cast<BlockState>(blk::kCopperBlock));
    EXPECT_EQ(dim.find(-1, 0)->active_sections, 1u << 4);
    dim.set_block(-10, 0, 5, static_cast<BlockState>(blk::kOxidizedCopper));  // ...until it is through with it
    EXPECT_EQ(dim.find(-1, 0)->active_sections, 0u);
    EXPECT_EQ(dim.find(-1, 0)->ticking_sections, 1u);
    check_counts(dim, 1);
}

TEST(RandomTicks, CountsFollowEveryChange) {
    FlatGenerator gen(-64, 24, FlatGenerator::default_layers(), 39);
    Dimension dim(proto::reg::dimension_type::kOverworld, "minecraft:overworld", gen, proto::kNoCompression);
    for (i32 cz = -1; cz <= 1; ++cz)
        for (i32 cx = -1; cx <= 1; ++cx) dim.simulate(dim.ticking(cx, cz), 1, 1);
    const std::vector<BlockState> states = tick_states();
    std::mt19937 rng(11);
    for (u32 round = 0; round < 40; ++round) {
        for (u32 k = 0; k < 300; ++k) {
            // Around the origin, where four chunks meet, and around the border of two sections.
            const i32 x = static_cast<i32>(rng() % 12) - 6, z = static_cast<i32>(rng() % 12) - 6;
            const i32 y = (rng() % 3 == 0 ? -64 : -52) + static_cast<i32>(rng() % 8);
            dim.set_block(x, y, z, states[rng() % states.size()]);
        }
        if (round % 5 == 4) {  // somebody stops simulating a chunk, and starts again
            Chunk& c = *dim.find(static_cast<i32>(rng() % 3) - 1, static_cast<i32>(rng() % 3) - 1);
            dim.simulate(c, c.sim_refs ? -1 : 1, 0);
        }
        dim.update_light();
        dim.end_tick();
        check_counts(dim, 1);
    }
    // The top of the world: nothing lies on its highest cells.
    dim.set_block(3, dim.max_y(), 3, static_cast<BlockState>(blk::kDirt));
    EXPECT_EQ(dim.find(0, 0)->random[dim.sections() - 1].targets, 1u);
    dim.set_block(3, dim.max_y(), 3, block_info_of(blk::kGrassBlock).alt[0]);
    EXPECT_EQ(dim.find(0, 0)->random[dim.sections() - 1].targets, 0u);
    EXPECT_EQ(dim.find(0, 0)->random[dim.sections() - 1].dying, 0u);
    check_counts(dim, 1);
}
