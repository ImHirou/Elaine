// SPDX-License-Identifier: GPL-3.0-or-later
// Workers: threads for what must not run on the tick thread - reading and writing files,
// inflating and deflating, generating a chunk.
//
// One owner (the thread that calls submit and poll - the tick thread) and N workers. Like every
// other pair of threads here they share nothing mutable: each worker has an SPSC ring of tasks
// coming in and one going back (base/spsc.hpp). A task is a small record with two functions:
// `work` runs on a worker, `done` runs back on the owner's thread from poll(), with whatever
// `work` left in the record. What a task points at belongs to the task while it is out: the owner
// does not touch it between submit() and done().
//
// A task goes to the worker with the least outstanding work, by the weights the owner gives - one
// queue per worker means a short task can stand behind a long one, and this keeps that rare.
//
// Workers(0) has no threads: submit() runs the work at once, poll() the `done`s. Tests use it, and
// so does anything that wants the pipeline without the timing.
#pragma once

#include <pthread.h>

#include <atomic>
#include <memory>
#include <vector>

#include "base/spsc.hpp"
#include "base/types.hpp"

namespace base {

struct Task {
    void (*work)(Task&) = nullptr;  // on a worker thread
    void (*done)(Task&) = nullptr;  // on the owner's thread, from poll()
    void* ctx = nullptr;            // the owner's object
    void* data = nullptr;           // the task's own state
    u64 a = 0, b = 0;
    u32 weight = 1;  // set by submit()
    u32 worker = 0;
};

class Workers {
  public:
    explicit Workers(u32 threads);
    ~Workers();  // runs what is queued, then joins; `done` of what never came back is not called
    Workers(const Workers&) = delete;
    Workers& operator=(const Workers&) = delete;

    u32 threads() const { return static_cast<u32>(workers_.size()); }
    // Hand a task to a worker. `weight`: how long it will take, in units of the caller's choosing.
    void submit(Task t, u32 weight = 1);
    // Wake the workers that were given tasks since the last call (one futex wake each).
    void kick();
    // Run `done` for tasks that have finished: all of them, or until now_ns() passes `deadline_ns`
    // (0 = no deadline; at least one is always taken). Returns how many.
    u32 poll(u64 deadline_ns = 0);
    u32 in_flight() const { return in_flight_; }
    // Sleep until a task has finished or now_ns() reaches `deadline_ns`. True: poll() has something
    // to take. For the owner's idle time: what one task's `done` starts need not wait for a tick.
    bool wait(u64 deadline_ns);
    // Block until everything submitted has come back (shutdown, tests).
    void drain();

  private:
    // What wakes an owner that sleeps in wait(): a count of finished tasks to sleep on (a futex),
    // and whether anybody does - a worker that finishes makes a system call only then.
    struct Owner {
        CACHE_ALIGNED std::atomic<u32> finished{0};
        std::atomic<u32> sleeping{0};
    };
    struct Worker {
        explicit Worker(u32 ring) : in(ring), out(ring) {}
        SpscRing<Task> in, out;
        Owner* owner = nullptr;
        CACHE_ALIGNED std::atomic<u32> signal{0};
        std::atomic<bool> stop{false};
        pthread_t thread{};  // (not a std::thread: <thread> is 0.9 s of every file that includes this one)
        u32 index = 0;
        u32 load = 0;  // weight out with this worker (owner's count)
        bool touched = false;
    };
    static void run(Worker& w);

    Owner owner_;
    std::vector<std::unique_ptr<Worker>> workers_;
    std::vector<Task> inline_done_;  // Workers(0): finished, `done` not yet run
    std::vector<Task> held_;         // finished, taken out of a full ring by submit(), `done` not yet run
    usize held_at_ = 0;
    u32 in_flight_ = 0;
};

}  // namespace base
