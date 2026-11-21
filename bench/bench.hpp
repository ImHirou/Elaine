// SPDX-License-Identifier: GPL-3.0-or-later
// Shared by the benchmark files: best-of-5 timing loop and the name filter.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "base/types.hpp"

namespace benchmark {

using Clock = std::chrono::steady_clock;
inline const char* g_filter = "";
inline volatile u64 g_sink;

inline bool selected(const char* name) { return std::strstr(name, g_filter) != nullptr; }

// Runs fn repeatedly for ~0.4 s (after a warm-up) and prints the best-of-5 rate.
template <class F>
void bench(const char* name, double units_per_call, const char* unit, F&& fn) {
    if (!selected(name)) return;
    fn();
    double best = 0;
    for (int rep = 0; rep < 5; ++rep) {
        u64 calls = 0;
        const auto t0 = Clock::now();
        double dt;
        do {
            for (int i = 0; i < 16; ++i) fn();
            calls += 16;
            dt = std::chrono::duration<double>(Clock::now() - t0).count();
        } while (dt < 0.08);
        const double rate = static_cast<double>(calls) * units_per_call / dt;
        if (rate > best) best = rate;
    }
    std::printf("%-44s %12.1f %s\n", name, best / 1e6, unit);
}

// For work too slow or too stateful to loop: the best wall time of `reps` runs of fn, in seconds.
template <class F>
double best_seconds(int reps, F&& fn) {
    double best = 1e30;
    for (int i = 0; i < reps; ++i) {
        const auto t0 = Clock::now();
        fn();
        best = std::min(best, std::chrono::duration<double>(Clock::now() - t0).count());
    }
    return best;
}

inline void report(const char* name, double value, const char* unit) {
    if (selected(name)) std::printf("%-44s %12.2f %s\n", name, value, unit);
}

void world_benchmarks();    // bench/world_bench.cpp
void entity_benchmarks();   // bench/entity_bench.cpp
void item_benchmarks();     // bench/item_bench.cpp
void nbt_benchmarks();      // bench/nbt_bench.cpp
void game_benchmarks();     // bench/game_bench.cpp
void command_benchmarks();  // bench/command_bench.cpp
void physics_benchmarks();  // bench/physics_bench.cpp

}  // namespace benchmark
