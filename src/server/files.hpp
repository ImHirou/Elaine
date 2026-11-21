// SPDX-License-Identifier: GPL-3.0-or-later
// Files: the small files of a world - a player's, world.json - read and written by the workers,
// and what the tick thread knows about them in between.
//
// A file is written whole and beside its place, then moved there (`<path>.tmp`, rename): what a
// reader finds is one write or another, never part of one. One write of a file is out at a time;
// what is written while it is out waits, and only the latest of it. Until its last write has come
// back a file is answered from memory - a player who leaves and is back the next tick, or who logs
// in from another place, gets what it had, not what the disk still says.
//
// The owner is one thread (the tick thread): it calls everything here, and `done` runs on it -
// from Workers::poll(), or from read() itself when the bytes are still in memory.
#pragma once

#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/types.hpp"
#include "base/workers.hpp"

namespace server {

class Files {
  public:
    // `found` false: there is no such file (or it could not be read, which the log says).
    using Done = void (*)(void* ctx, u64 tag, std::span<const u8> bytes, bool found);

    // `sync`: a write is on the disk (fdatasync) before it takes its file's place.
    Files(base::Workers& workers, bool sync) : workers_(workers), sync_(sync) {}
    ~Files() { finish(); }
    Files(const Files&) = delete;
    Files& operator=(const Files&) = delete;

    void read(const std::string& path, void* ctx, u64 tag, Done done);
    void write(const std::string& path, std::span<const u8> bytes);
    // Wait until everything written is where it belongs, and every read has been answered.
    void finish();
    u32 busy() const { return jobs_; }

    static constexpr usize kMaxSize = 4u << 20;  // a file larger than this is not one of ours

  private:
    struct Entry {
        std::vector<u8> latest;  // what the file holds once its writes are through
        bool stale = false;      // `latest` is newer than the write that is out
    };
    struct ReadJob;
    struct WriteJob;
    void start_write(const std::string& path, const Entry& e);
    static void read_work(base::Task& t);
    static void read_done(base::Task& t);
    static void write_work(base::Task& t);
    static void write_done(base::Task& t);

    base::Workers& workers_;
    bool sync_;
    u32 jobs_ = 0;
    // Files with a write out. Cold: a join, a leave, an autosave.
    std::unordered_map<std::string, Entry> writing_;
};

}  // namespace server
