// SPDX-License-Identifier: GPL-3.0-or-later
// GenPipeline: the chunks of a generator that makes them in stages (ChunkGenerator::staged), made
// on the workers.
//
// A chunk's features reach into the chunks around it - a tree of one chunk has leaves in the
// next - and what a feature finds depends on what was put there before. Vanilla decorates one
// chunk at a time in whatever order its tickets come up, so its worlds are not a function of the
// seed along the borders of chunks. Here the order is fixed, and it is one that workers can keep
// without ever waiting for each other:
//
//   The plane of chunks is cut into areas of 4 x 4 in four ways, each shifted two chunks against
//   the one before: pass 1 at (0, 0), pass 2 at (2, 0), pass 3 at (2, 2), pass 4 at (0, 2). An
//   area's task decorates the four chunks in its middle, one after the other; the nine chunks
//   around each of them - all a decoration may write to - lie in the area. Every chunk is in the
//   middle of an area in exactly one of the four passes. So "every area of pass 1, then every
//   area of pass 2, of 3, of 4" is one way of decorating every chunk of the world once, each
//   after some of its neighbours and before the others - a world vanilla could have made - and
//   two areas of one pass share no chunk: they are made at the same time.
//
// A chunk is a world::GenChunk that moves: made (its terrain: version 0), it waits in the area of
// pass 1 that holds it; that area's task takes its sixteen and leaves them at version 1, in the
// two areas of pass 2 they lie in, half in each; and so on, until what an area of pass 4 leaves
// is finished chunks. Nothing is copied, counted or locked: a chunk is in one place at a time, and
// a worker is handed sixteen of them and hands sixteen back.
//
// Everything here is a function of the seed, so everything here is a cache. A chunk that is asked
// for (request) makes its area of pass 4 wanted, that the two of pass 3 it is made of, and so on
// down to the terrain of 96 chunks; what is wanted and has all its chunks is run, the earliest
// request's first. What comes of an area and nobody asked for - the halves of areas further out,
// finished chunks nobody is near yet - stays where it is: it is what the next request beside it
// starts from. Until it has lain unasked for long enough, or all that is kept is over the budget;
// then it goes, and is made again if it is ever needed - the same, to the bit (a build with
// DCHECKs compares whatever it makes twice).
//
// Tick thread only; the workers see nothing of it but the chunks of their task.
#pragma once

#include <deque>
#include <unordered_map>
#include <vector>

#include "base/workers.hpp"
#include "world/gen_chunk.hpp"
#include "world/generator.hpp"

namespace world {

class GenPipeline {
  public:
    // A chunk that was asked for is made: it is the receiver's from here on, to delete.
    using Made = void (*)(void* ctx, i32 cx, i32 cz, GenChunk* chunk);

    GenPipeline(base::Workers& workers, ChunkGenerator& gen, Made made, void* ctx);
    // Nothing may be out with a worker: stop(), then Workers::drain().
    ~GenPipeline();
    GenPipeline(const GenPipeline&) = delete;
    GenPipeline& operator=(const GenPipeline&) = delete;

    // Ask for the finished chunk at (cx, cz); `made` is told, now or from a later Workers::poll().
    // Asked once: not again before it is made or forgotten. `seq` says whose turn it is - of all
    // that can be worked on, what the request with the smallest needs comes first - and grows
    // from one request to the next.
    void request(i32 cx, i32 cz, u64 seq);
    // Nobody waits for the chunk any more. False: it was not asked for (or is made already).
    bool forget(i32 cx, i32 cz);
    // Once a tick: what has lain unasked for long enough goes, and the oldest of what is kept
    // while that is more than the budget.
    void tick(u64 now);

    // No task is started while it is held (the caller is waiting for the workers to come back
    // with something else); what is out comes back as ever.
    void hold(bool on);
    // For good: the server stops.
    void stop() { stopped_ = true; }

    // How much may be kept that nobody has asked for, and for how many ticks.
    void set_budget(usize bytes) { budget_ = bytes; }
    void set_keep(u64 ticks) { keep_ = ticks; }

    usize waiting() const { return requests_.size(); }  // chunks asked for and not made yet
    u32 running() const { return running_; }            // tasks out with the workers
    usize kept_bytes() const { return kept_bytes_; }    // of chunks that wait here, wanted or not
    usize kept_chunks() const { return kept_chunks_; }
    u64 terrains_made() const { return terrains_; }  // tasks done so far: chunks of terrain...
    u64 areas_made() const { return areas_; }        // ...and areas decorated

    // The geometry (tests, and whoever wants to know what a request costs). A pass is 1 .. 4; an
    // area of it is named by (a, b) and holds the chunks x0 .. x0 + 3, z0 .. z0 + 3.
    static constexpr u32 kPasses = 4;
    static constexpr i32 kShiftX[kPasses + 2] = {0, 0, 2, 2, 0, 0}, kShiftZ[kPasses + 2] = {0, 0, 0, 2, 2, 2};
    static i32 area_x0(u32 pass, i32 a) { return a * 4 + kShiftX[pass]; }
    static i32 area_z0(u32 pass, i32 b) { return b * 4 + kShiftZ[pass]; }
    static i32 area_a(u32 pass, i32 cx) { return (cx - kShiftX[pass]) >> 2; }
    static i32 area_b(u32 pass, i32 cz) { return (cz - kShiftZ[pass]) >> 2; }
    static u32 area_slot(u32 pass, i32 cx, i32 cz) {
        return (static_cast<u32>(cx - kShiftX[pass]) & 3) | (static_cast<u32>(cz - kShiftZ[pass]) & 3) << 2;
    }

  private:
    // An area of a pass with the chunks that wait for it, or (pass 5) what an area of pass 4 left.
    struct Node {
        enum State : u8 { kFree, kIdle, kQueued, kRunning };
        u64 key = 0;
        u32 index = 0;  // its place in nodes_
        i32 a = 0, b = 0;
        u8 pass = 0;
        State state = kFree;
        bool wanted = false;  // a chunk that is asked for is made of it
        u16 have = 0;         // which of in[] are here
        u16 making = 0;       // pass 1: whose terrain is out with a worker
        u16 asked = 0;        // pass 5: which are asked for and not here
        u64 seq = 0;          // the request it is first wanted by
        u64 touched = 0;      // when a chunk last came to it or was asked of it
        GenChunk* in[16] = {};
    };
    struct Entry {  // something to start: an area's task (slot kWhole), or the terrain of one of its chunks
        u64 seq, order, key;
        u32 node;
        u32 slot;
    };
    struct AreaJob;
    static constexpr u32 kWhole = 16, kOutput = kPasses + 1;

    static u64 key_of(u32 pass, i32 a, i32 b);
    Node* find(u32 pass, i32 a, i32 b);
    Node& node(u32 pass, i32 a, i32 b);
    void free_node(Node& n);
    void drop(Node& n);
    void want(Node& n, u64 seq);
    void queue(Node& n, u32 slot);
    Node& give(u32 pass, GenChunk* g);
    void settle(Node& n);
    void start();
    void rebuild();
    void sweep();
    static usize bytes_of(const GenChunk& g) { return sizeof(GenChunk) + g.arena.reserved_bytes(); }

    static void terrain_work(base::Task& t);
    static void terrain_done(base::Task& t);
    static void area_work(base::Task& t);
    static void area_done(base::Task& t);

    base::Workers& workers_;
    ChunkGenerator& gen_;
    Made made_;
    void* ctx_;
    std::deque<Node> nodes_;  // (a deque: a node stays where it is while others are added)
    std::vector<u32> free_;
    std::unordered_map<u64, u32> index_;     // looked at a few times a task, never per tick
    std::unordered_map<u64, u64> requests_;  // the chunks asked for, and each one's turn
    std::vector<Entry> heap_;                // what can be started, the earliest request's first
    u64 order_ = 0;
    u32 running_ = 0, cap_ = 2;
    bool held_ = false, stopped_ = false, stale_ = false;
    u64 now_ = 0, swept_ = 0;
    u64 keep_ = 12000;                 // ten minutes of ticks
    usize budget_ = usize{256} << 20;  // (chosen without a measurement)
    usize kept_bytes_ = 0, kept_chunks_ = 0;
    u64 terrains_ = 0, areas_ = 0;
};

}  // namespace world
