// SPDX-License-Identifier: GPL-3.0-or-later
// Scheduled ticks: "tick the block at this position, so many game ticks from now".
//
// A port of vanilla's LevelTicks / LevelChunkTicks / ScheduledTick / SavedTick (26.3). What it
// has to get right is the ORDER ticks run in - redstone, and everything built out of it, is made
// of that order:
//
//   * a chunk holds its own ticks, by (game time due, priority, order of scheduling)
//                                                                  [ScheduledTick.DRAIN_ORDER]
//   * one tick per position and type at a time; a second is dropped
//                                                                  [LevelChunkTicks.ticksPerPosition]
//   * each game tick, the due ticks of the chunks that tick are merged, chunk against chunk, by
//     (priority, order of scheduling) of what each has next        [INTRA_TICK_DRAIN_ORDER]
//     Ticks that are overdue - their chunk did not tick for a while - come out in an order no
//     single sort gives: collect() is LevelTicks.drainContainers, step for step.
//   * at most 65536 a game tick; the rest stay due                 [ServerLevel.tick]
//   * a chunk is saved with its ticks as delays, and one that comes back counts them again from
//     the moment it first ticks                                    [SavedTick, LevelChunk.unpackTicks]
//
// What is ours is the storage. Vanilla has a PriorityQueue of objects and a hash set of objects
// per chunk, and walks a hash map of every chunk that holds a tick once a game tick. Here a tick
// is 24 bytes in a flat binary heap, the "one per position" set is a flat table of 32-bit keys,
// and the chunks that hold ticks are two parallel arrays - when each is next due, and which it
// is - so that the pass over them reads eight bytes a chunk and touches a chunk only when it is due.
//
// Tick thread only.
#pragma once

#include <span>
#include <vector>

#include "base/buf.hpp"
#include "world/chunk.hpp"

namespace world {

// TickPriority: EXTREMELY_HIGH (-3) .. EXTREMELY_LOW (3).
inline constexpr i32 kTickPriorityMin = -3, kTickPriorityMax = 3, kTickPriorityNormal = 0;

struct ScheduledTick {
    i64 trigger;  // ScheduledTick.triggerTick: the game time it is due at (in ChunkTicks::saved: its delay). Signed, as
                  // vanilla's: a tick that was overdue when it was saved may be due before the time a server starts at.
    u64 order;    // priority above subTickOrder, both biased to be unsigned: INTRA_TICK_DRAIN_ORDER is `<` on this
    u32 what;     // type << 17 | (y - min y) << 8 | z << 4 | x, x and z inside the chunk
    u32 unused;
};
static_assert(sizeof(ScheduledTick) == 24);

// LevelChunkTicks: the ticks of one chunk. Made when the chunk gets its first tick.
struct ChunkTicks {
    static constexpr u32 kNone = ~0u;
    ScheduledTick* heap = nullptr;  // a binary min-heap by (trigger, order)
    u32 n = 0, cap = 0;
    ScheduledTick* saved = nullptr;  // pendingTicks: as they were loaded, in the file's order
    u32 saved_n = 0;
    u32* set = nullptr;  // ticksPerPosition: `what` of every tick in heap and saved; open addressing
    u32 set_cap = 0, set_n = 0;
    u32 slot = kNone;  // LevelTicks: its place among the chunks whose heap holds a tick
};

// Whether `bytes` are the scheduled ticks of a chunk (LevelTicks::write) in a dimension of this
// many types and this height - or nothing at all. What a worker asks before it hands a chunk on.
bool ticks_valid(std::span<const u8> bytes, u32 types, u32 height);

class LevelTicks {
  public:
    struct Due {
        i32 x, y, z;
        u32 type;
    };
    static constexpr u32 kMaxPerTick = 65536;

    // `types`: one more than the largest type id (a block id); `min_y`, `height`: of the dimension.
    LevelTicks(u32 types, i32 min_y, u32 height) : types_(types), min_y_(min_y), height_(height) {}
    LevelTicks(const LevelTicks&) = delete;
    LevelTicks& operator=(const LevelTicks&) = delete;

    // LevelAccessor.scheduleTick: a tick of `type` at (x, y, z), due at game time `trigger`. Nothing
    // happens if the chunk already holds one for that position and type.
    void schedule(Chunk& c, i32 x, i32 y, i32 z, u32 type, i64 trigger, i32 priority = kTickPriorityNormal);
    // LevelTickAccess.hasScheduledTick
    bool has(const Chunk& c, i32 x, i32 y, i32 z, u32 type) const;
    // LevelTickAccess.willTickThisTick: collected for this game tick and not run yet.
    bool will_tick(i32 x, i32 y, i32 z, u32 type);

    // LevelTicks.tick, its first half (collectTicks): what is due at game time `now` in the chunks
    // that tick blocks (Chunk::block_ticking), in the order it is to run in...
    void collect(i64 now, u32 max = kMaxPerTick);
    // ...its second (runCollectedTicks): the next of them, false when there is none left. A tick
    // that is scheduled while these run waits for the next collect(), however soon it is due.
    bool next(Due& out) {
        if (run_head_ == run_.size()) return false;
        out = run_[run_head_++];
        return true;
    }
    // ...and its end (cleanupAfterTick).
    void finish() {
        run_.clear();
        run_head_ = 0;
        index_built_ = false;
    }

    // LevelChunk.unpackTicks: the ticks a chunk was loaded with become due `delay` game ticks from
    // `now`, in the order they were saved in - before everything scheduled since the server started.
    void unpack(Chunk& c, i64 now);
    // The chunk leaves the world (LevelTicks.removeContainer): its ticks go with it.
    void release(Chunk& c);
    // How many ticks wait, anywhere (LevelTicks.count).
    u64 count() const { return count_; }

    // ---- as bytes (world/chunk_io.hpp): u8 version, u32 count, then count x (u32 what, i32 delay, i8 priority)
    // LevelChunkTicks.pack: what was loaded and never unpacked as it is, then the rest in the order
    // it was scheduled in, each with the game ticks left until it is due.
    static void write(const ChunkTicks* t, i64 now, base::ByteBuf& out);
    // The ticks of a chunk that has just been adopted, from bytes that ticks_valid() passed.
    void read(Chunk& c, std::span<const u8> bytes);

    ALWAYS_INLINE u32 pack(i32 x, i32 y, i32 z, u32 type) const {
        return type << 17 | static_cast<u32>(y - min_y_) << 8 | static_cast<u32>(z & 15) << 4 | static_cast<u32>(x & 15);
    }

  private:
    static constexpr u64 kSubTickBias = 1ull << 60;
    static u64 order_of(i32 priority, i64 sub_tick) {
        return static_cast<u64>(priority - kTickPriorityMin) << 61 | (static_cast<u64>(sub_tick) + kSubTickBias);
    }
    ChunkTicks& container(Chunk& c);
    void push(Chunk& c, ChunkTicks& t, const ScheduledTick& tick);
    ScheduledTick pop(Chunk& c, ChunkTicks& t);
    void list(Chunk& c, ChunkTicks& t);
    void unlist(ChunkTicks& t);
    Due due_of(const Chunk& c, const ScheduledTick& tick) const {
        return {c.x * 16 + static_cast<i32>(tick.what & 15), min_y_ + static_cast<i32>((tick.what >> 8) & 511),
                c.z * 16 + static_cast<i32>((tick.what >> 4) & 15), tick.what >> 17};
    }

    u32 types_;
    i32 min_y_;
    u32 height_;
    i64 sub_tick_ = 0;  // Level.subTickCount
    u64 count_ = 0;
    // LevelTicks.nextTickForContainer, as two columns: the chunks whose heap holds a tick, and for
    // each a game time not after its first one.
    std::vector<i64> next_;
    std::vector<Chunk*> chunks_;
    std::vector<Chunk*> queue_;  // containersToTick: a heap by what each chunk has next
    std::vector<Due> run_;       // toRunThisTick
    usize run_head_ = 0;
    // toRunThisTickSet, made when first asked for: run_ indices by a hash of the tick.
    std::vector<u32> index_;
    bool index_built_ = false;
};

}  // namespace world
