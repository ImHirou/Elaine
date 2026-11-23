// SPDX-License-Identifier: GPL-3.0-or-later
// How fast the game ticks, and whether it does: TickRateManager and what ServerTickRateManager
// adds to it (26.3), as plain state. `/tick` sets it (server/commands.cpp), the loop reads it
// (Game::run), and whatever tells the clients is the game's (server/time.cpp).
//
//   rate      ticks a second: a tick lasts a second over the rate, taken as a long of
//             nanoseconds (setTickRate), 1 at least.
//   frozen    the game's elements stand still - clocks, the game time, scheduled and random
//             ticks, block events, every entity that is not a player, block entities - while
//             packets are read, players tick, and what changed is shown as ever.
//   step      so many ticks a frozen game takes all the same, at its rate.
//   sprint    so many ticks without a wait between them, then back to what was before.
//
// The names are vanilla's, in snake_case.
#pragma once

#include <algorithm>

#include "base/clock.hpp"
#include "base/types.hpp"

namespace server {

struct TickRate {
    // ---- TickRateManager
    float rate = 20.0f;
    u64 ns_per_tick = base::kNsPerSec / 20;
    i32 frozen_ticks_to_run = 0;
    bool run_game_elements = true;
    bool frozen = false;
    // ---- ServerTickRateManager
    i64 remaining_sprint_ticks = 0;
    u64 sprint_tick_start = 0;
    u64 sprint_time_spent = 0;
    i64 scheduled_sprint_ticks = 0;
    bool previous_frozen = false;

    void set_rate(float r) {
        rate = std::max(r, 1.0f);
        ns_per_tick = static_cast<u64>(static_cast<double>(base::kNsPerSec) / static_cast<double>(rate));
    }
    float ms_per_tick() const { return static_cast<float>(ns_per_tick) / 1e6f; }
    bool runs_normally() const { return run_game_elements; }
    bool stepping_forward() const { return frozen_ticks_to_run > 0; }
    bool sprinting() const { return scheduled_sprint_ticks > 0; }
    // TickRateManager.tick, at the start of a tick: does this one run the game?
    void tick() {
        run_game_elements = !frozen || frozen_ticks_to_run > 0;
        if (frozen_ticks_to_run > 0) --frozen_ticks_to_run;
    }
};

}  // namespace server
