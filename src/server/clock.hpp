// SPDX-License-Identifier: GPL-3.0-or-later
// World clocks: the time of a world, as 26.x keeps it. There is no "day time" any more - the
// server has clocks (minecraft:world_clock: the overworld's, the end's), each a count of ticks
// that runs at its own rate or stands still, and timelines (minecraft:timeline) that read a
// clock: `day` turns the overworld's clock into the day of 24000 ticks - where the sun is, how
// much light the sky gives - and names moments of it (time markers: day, noon, night, midnight).
// `/time` is about clocks.
//
// A port of ServerClockManager.ServerClockInstance, ClockTimeMarker, KeyframeTrackSampler and the
// one environment attribute a server computes from them (gameplay/sky_light_level, for
// Level.skyDarken) - pure functions over a clock's state, with vanilla's expressions as they are
// written: what a client is told of a clock it goes on computing by itself, and must agree.
#pragma once

#include <string_view>

#include "base/types.hpp"
#include "proto/gen/current.hpp"

namespace server {

inline constexpr u32 kClocks = proto::gen::reg::world_clock::kCount;

// ServerClockInstance (ClockState, as it is saved).
struct Clock {
    i64 total_ticks = 0;
    float partial_tick = 0;
    float rate = 1;
    bool paused = false;

    void tick();  // one server tick of a clock that runs
};

// What every clock of a world is at, and the world's own count of ticks (Level.getGameTime).
struct WorldTime {
    i64 game_time = 0;
    Clock clocks[kClocks];
};

std::string_view clock_name(u32 clock);        // "minecraft:overworld"
u32 find_clock(std::string_view name);         // ~0u: there is none
std::string_view timeline_name(u32 timeline);  // "minecraft:day"
u32 find_timeline(std::string_view name);
u32 timeline_clock(u32 timeline);
u32 timeline_count();
// Timeline.getCurrentTicks / getPeriodCount: where in its period a timeline is, and how many
// periods have passed.
i64 timeline_ticks(u32 timeline, i64 total_ticks);
i32 timeline_repetitions(u32 timeline, i64 total_ticks);

// The time markers of a clock.
struct MarkerRef {
    std::string_view name;
    bool show_in_commands;
};
u32 marker_count();
bool marker_of_clock(u32 index, u32 clock, MarkerRef& out);

// ServerClockManager.moveToTimeMarker: on to the next time this marker comes round.
enum class Move : u8 { kNoMarker, kNotMoved, kMoved };
Move move_to_marker(Clock& c, u32 clock, std::string_view marker);

// The clock a dimension's `/time` means (DimensionType.defaultClock); ~0u: none.
u32 default_clock(u32 dimension_type);

// Level.updateSkyBrightness in the overworld, without weather: how many levels the sky's light
// is down by at this time of the overworld's clock. 0 by day, 11 in the middle of the night.
i32 sky_darken(i64 overworld_ticks);

}  // namespace server
