// SPDX-License-Identifier: GPL-3.0-or-later
// ChunkStore: where chunks come from and where they go - the region files of a dimension, what is
// on its way into them, and the generator for everything that was never saved.
//
// Owned by the tick thread, which is the only thread that knows anything about the files: which
// regions exist, where each chunk is in its file (a copy of every open region's table), what is
// waiting to be written. Workers are handed finished questions - "read these bytes of this file
// and make a chunk of them", "write these chunks to this region" - and hand back the answers
// through base::Workers. Nothing is locked and nothing is shared:
//
//   load     the chunk's entry is looked up here; a worker reads, inflates and decodes (or runs
//            the generator, when there is no entry). A region whose table has not been read yet
//            is opened first, by a worker, and the loads wait for it.
//   save     the chunk's bytes are kept here (`pending`) until its region is written: once it has
//            been waiting five seconds, or holds 256 chunks, or flush() says so. One write per
//            region at a time; the job owns the RegionFile (table and sector bitmap) while it
//            runs and hands the new table back.
//   between  a chunk that is asked for while its bytes wait or are being written is decoded from
//            those bytes - they are newer than the file.
//   made     a generator that makes chunks in stages has its own way through the workers
//            (world/gen_pipeline.hpp): a chunk whose blocks are its to make waits here, with
//            whatever else was read of it, until they come.
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/workers.hpp"
#include "world/chunk_io.hpp"
#include "world/gen_pipeline.hpp"
#include "world/region.hpp"

namespace world {

class ChunkStore {
  public:
    // `dir`: the directory of the dimension's region files, made if it is not there. Empty:
    // nothing is kept, every chunk is the generator's and save() must not be called.
    // `sync`: fdatasync around every header switch (see world/region.hpp).
    ChunkStore(base::Workers& workers, const Layout& layout, ChunkGenerator& gen, std::string dir, bool sync);
    ~ChunkStore();
    ChunkStore(const ChunkStore&) = delete;
    ChunkStore& operator=(const ChunkStore&) = delete;

    bool persistent() const { return !dir_.empty(); }

    // Ask for the chunk at (cx, cz). It arrives in loaded() after a later Workers::poll(). The
    // caller asks once and waits: there is no second request for a chunk that is on its way.
    // Chunks arrive in the order they were asked for, whichever worker was done first: what the
    // world makes of them (the light engine's bookkeeping depends on which of two neighbours
    // came first) is then the same on every run, with any number of workers.
    void load(i32 cx, i32 cz);
    // Nobody wants the chunk that was asked for any more. True: it will not arrive - it was still
    // waiting to be made, and what was to be made for it alone is left. False: it is too far on
    // its way (read, or being read), and arrives.
    bool forget(i32 cx, i32 cz);
    // The chunks that have arrived; the caller takes them out and deletes each when done with it.
    std::vector<LoadedChunk*>& loaded() { return loaded_; }
    // What makes the chunks of a generator that works in stages, or none.
    GenPipeline* generation() { return pipeline_.get(); }
    // Nothing more is made from here on: the server stops. Chunks that wait to be made never arrive.
    void stop_generation() {
        if (pipeline_) pipeline_->stop();
    }

    // Keep these bytes (encode_chunk's) as the chunk at (cx, cz); they replace what was waiting.
    void save(i32 cx, i32 cz, std::span<const u8> bytes);

    // Once a tick: start the writes that are due, close the regions nobody has used for a minute.
    void tick(u64 now);
    // Write everything that waits as soon as its region can be written (shutdown, save-all).
    void flush() { flush_ = true; }
    // flush(), and wait until it is done - or cannot be (shutdown). The caller's thread sleeps.
    void finish();
    // Anything waiting to be written, or any job out with a worker.
    bool busy() const { return jobs_ != 0 || waiting_ != 0; }
    usize waiting() const { return waiting_; }
    u64 chunks_written() const { return written_; }
    bool failed() const { return failed_; }  // a region could not be opened or written: see the log

  private:
    struct Blob {  // a chunk's bytes; counted on the tick thread, read by the jobs that hold it
        u32 refs;
        u32 len;
        u8* data() { return reinterpret_cast<u8*>(this + 1); }
    };
    struct Item {
        u16 index;
        Blob* blob;
    };
    struct Region {
        enum State : u8 { kNone, kClosed, kOpening, kOpen, kFailed };
        i32 rx = 0, rz = 0;
        State state = kNone;
        bool writing = false;
        bool listed = false;       // in dirty_
        bool want_create = false;  // the open that is under way may make the file
        u32 readers = 0;           // load jobs that read its file
        RegionFile* file = nullptr;
        RegionEntry index[kRegionChunks] = {};
        std::vector<Item> pending, in_flight;
        struct Waiting {
            u16 index;
            u64 seq;
        };
        std::vector<Waiting> waiting;  // loads that wait for the table
        u64 first_pending = 0, last_used = 0, retry_at = 0;
    };
    struct LoadJob;
    struct OpenJob;
    struct WriteJob;

    Region& region(i32 rx, i32 rz);
    std::string path_of(const Region& r) const;
    void start_load(Region* r, i32 cx, i32 cz, const RegionEntry& entry, Blob* blob, u64 seq);
    void start_open(Region& r, bool create);
    void start_write(Region& r);
    void load_known(Region& r, u16 index, u64 seq);
    static Blob* find(std::vector<Item>& items, u16 index);
    void release(Blob* b);
    void arrive(u64 seq, LoadedChunk* chunk);
    void await_made(LoadJob* job);
    static void made(void* ctx, i32 cx, i32 cz, GenChunk* chunk);

    static void load_work(base::Task& t);
    static void load_done(base::Task& t);
    static void open_work(base::Task& t);
    static void open_done(base::Task& t);
    static void write_work(base::Task& t);
    static void write_done(base::Task& t);

    base::Workers& workers_;
    Layout layout_;
    ChunkGenerator& gen_;
    std::string dir_;
    bool sync_;
    bool flush_ = false, failed_ = false;
    u64 now_ = 0;
    u32 jobs_ = 0;
    usize waiting_ = 0;
    u64 written_ = 0;
    std::unordered_map<u64, Region*> regions_;  // looked at once per chunk loaded or saved, never per tick
    std::vector<Region*> dirty_;                // regions with something pending
    std::vector<LoadedChunk*> loaded_;
    // The requests that have not been handed on, by their number: a ring, each place taken when
    // its chunk is done (or will never be: forgotten) and waiting for the ones asked for earlier.
    struct Arrival {
        LoadedChunk* chunk = nullptr;
        bool here = false;
    };
    std::vector<Arrival> ring_;
    u64 asked_ = 0, handed_ = 0;  // requests so far; of them, how many are in loaded_ or were
    std::unique_ptr<GenPipeline> pipeline_;
    std::unordered_map<u64, LoadJob*> unmade_;  // chunks that wait for their blocks, by place
};

}  // namespace world
