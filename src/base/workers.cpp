// SPDX-License-Identifier: GPL-3.0-or-later
#include "base/workers.hpp"

#include <pthread.h>

#include <cstdio>

#include "base/clock.hpp"

namespace base {

namespace {
constexpr u32 kRing = 4096;  // tasks queued per worker; a full ring makes the owner wait for room
}

Workers::Workers(u32 threads) {
    for (u32 i = 0; i < threads; ++i) {
        workers_.push_back(std::make_unique<Worker>(kRing));
        Worker* w = workers_.back().get();
        w->thread = std::thread([w, i] {
            char name[16];
            std::snprintf(name, sizeof name, "work-%u", i);
            pthread_setname_np(pthread_self(), name);
            run(*w);
        });
    }
}

Workers::~Workers() {
    for (auto& w : workers_) {
        w->stop.store(true, std::memory_order_release);
        w->signal.fetch_add(1, std::memory_order_release);
        w->signal.notify_one();
    }
    for (auto& w : workers_) w->thread.join();
}

void Workers::run(Worker& w) {
    for (;;) {
        // The signal is read before the ring is looked at: a task pushed after this load bumps it,
        // and the wait below returns at once.
        const u32 seen = w.signal.load(std::memory_order_acquire);
        Task t;
        bool any = false;
        while (w.in.pop(t)) {
            any = true;
            t.work(t);
            while (UNLIKELY(!w.out.push(t))) std::this_thread::yield();  // the owner is behind with poll()
        }
        if (any) continue;
        if (w.stop.load(std::memory_order_acquire)) return;
        w.signal.wait(seen, std::memory_order_acquire);
    }
}

void Workers::submit(Task t, u32 weight) {
    t.weight = weight;
    ++in_flight_;
    if (workers_.empty()) {
        t.work(t);
        inline_done_.push_back(t);
        return;
    }
    u32 best = 0;
    for (u32 i = 1; i < workers_.size(); ++i)
        if (workers_[i]->load < workers_[best]->load) best = i;
    Worker& w = *workers_[best];
    t.worker = best;
    w.load += weight;
    while (UNLIKELY(!w.in.push(t))) {
        // 4096 tasks behind. Let it work - and take what it has finished off its hands, or it
        // stands at a full ring of answers while this thread stands at a full ring of tasks. The
        // answers are kept for poll(): a `done` does not run in the middle of whoever is submitting.
        w.signal.fetch_add(1, std::memory_order_release);
        w.signal.notify_one();
        Task finished;
        while (w.out.pop(finished)) held_.push_back(finished);
        std::this_thread::yield();
    }
    w.touched = true;
}

void Workers::kick() {
    for (auto& w : workers_) {
        if (!w->touched) continue;
        w->touched = false;
        w->signal.fetch_add(1, std::memory_order_release);
        w->signal.notify_one();
    }
}

u32 Workers::poll(u64 deadline_ns) {
    u32 n = 0;
    if (workers_.empty()) {
        // `done` may submit more: those land behind what is being walked.
        for (usize i = 0; i < inline_done_.size(); ++i) {
            Task t = inline_done_[i];
            --in_flight_;
            ++n;
            if (t.done) t.done(t);
        }
        inline_done_.clear();
        return n;
    }
    const auto finish = [&](Task& t) {
        workers_[t.worker]->load -= t.weight;
        --in_flight_;
        ++n;
        if (t.done) t.done(t);
        return deadline_ns && now_ns() >= deadline_ns;
    };
    // What submit() took off a worker's hands first (a `done` may submit, and add to it).
    while (held_at_ < held_.size()) {
        Task t = held_[held_at_++];
        if (finish(t)) {
            kick();
            return n;
        }
    }
    held_.clear();
    held_at_ = 0;
    for (auto& w : workers_) {
        Task t;
        while (w->out.pop(t)) {
            if (finish(t)) {
                kick();
                return n;
            }
        }
    }
    kick();  // a `done` may have submitted the next step
    return n;
}

void Workers::drain() {
    kick();
    while (in_flight_ != 0) {
        if (poll() == 0) std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

}  // namespace base
