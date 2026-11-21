// SPDX-License-Identifier: GPL-3.0-or-later
#include "server/clock.hpp"

#include <iterator>

#include PROTO_GEN_TIMELINES_HPP

namespace server {

namespace reg = proto::gen::reg;
namespace tl = proto::gen::timeline;

namespace {

constexpr float kSkyLightDefault = 15.0f;  // EnvironmentAttributes.SKY_LIGHT_LEVEL: its default, and the top of its range

u32 find(const std::string_view* names, u32 count, std::string_view name) {
    for (u32 i = 0; i < count; ++i)
        if (names[i] == name) return i;
    return ~0u;
}

// Mth.floor(float)
i32 mth_floor(float v) {
    const i32 i = static_cast<i32>(v);
    return v < static_cast<float>(i) ? i - 1 : i;
}

// ClockTimeMarker.occursAt / resolveTimeToMoveTo (durationToNext).
bool occurs_at(const tl::Marker& m, i64 total_ticks) {
    return m.period == 0 ? m.ticks == total_ticks : m.ticks == total_ticks % m.period;
}
i64 time_to_move_to(const tl::Marker& m, i64 total_ticks) {
    if (m.period == 0) return m.ticks;
    const i64 duration = m.ticks - total_ticks % m.period;
    return total_ticks + (duration > 0 ? duration : m.period + duration);
}

// KeyframeTrackSampler.sample for a track that repeats: the keyframes, one segment from the
// last (a period back) to the first and one from the last to the first a period on
// (bakeSegments); linear in between (EasingType.LINEAR, Mth.lerp) - in floats, as written.
float sample(const tl::Keyframe* keys, u32 n, i32 period, i64 ticks) {
    const i64 at = ((ticks % period) + period) % period;  // Math.floorMod
    // getSegmentAt: the first segment that ends after `at`, the last one if none does.
    i32 from_ticks = keys[n - 1].ticks - period, to_ticks = keys[0].ticks;
    float from = keys[n - 1].value, to = keys[0].value;
    for (u32 i = 0; at >= to_ticks && i < n; ++i) {
        from_ticks = keys[i].ticks;
        from = keys[i].value;
        const bool last = i + 1 == n;
        to_ticks = last ? keys[0].ticks + period : keys[i + 1].ticks;
        to = last ? keys[0].value : keys[i + 1].value;
    }
    if (at <= from_ticks) return from;
    if (at >= to_ticks) return to;
    const float alpha = static_cast<float>(at - from_ticks) / static_cast<float>(to_ticks - from_ticks);
    return from + alpha * (to - from);
}

}  // namespace

void Clock::tick() {
    if (paused) return;
    partial_tick = partial_tick + rate;
    const i32 full = mth_floor(partial_tick);
    partial_tick = partial_tick - static_cast<float>(full);
    total_ticks = total_ticks + full;
}

std::string_view clock_name(u32 clock) { return reg::world_clock::kNames[clock]; }
u32 find_clock(std::string_view name) { return find(reg::world_clock::kNames, reg::world_clock::kCount, name); }
std::string_view timeline_name(u32 timeline) { return reg::timeline::kNames[timeline]; }
u32 find_timeline(std::string_view name) { return find(reg::timeline::kNames, reg::timeline::kCount, name); }
u32 timeline_clock(u32 timeline) { return tl::kTimelines[timeline].clock; }
u32 timeline_count() { return reg::timeline::kCount; }

i64 timeline_ticks(u32 timeline, i64 total_ticks) {
    const i32 period = tl::kTimelines[timeline].period;
    return period ? total_ticks % period : total_ticks;
}

i32 timeline_repetitions(u32 timeline, i64 total_ticks) {
    const i32 period = tl::kTimelines[timeline].period;
    return period ? static_cast<i32>(total_ticks / period) : 0;
}

u32 marker_count() { return static_cast<u32>(std::size(tl::kMarkers)); }

bool marker_of_clock(u32 index, u32 clock, MarkerRef& out) {
    const tl::Marker& m = tl::kMarkers[index];
    if (m.clock != clock) return false;
    out = {m.name, m.show_in_commands};
    return true;
}

Move move_to_marker(Clock& c, u32 clock, std::string_view marker) {
    for (const tl::Marker& m : tl::kMarkers) {
        if (m.clock != clock || m.name != marker) continue;
        if (occurs_at(m, c.total_ticks)) return Move::kNotMoved;
        c.total_ticks = time_to_move_to(m, c.total_ticks);
        c.partial_tick = 0;
        return Move::kMoved;
    }
    return Move::kNoMarker;
}

u32 default_clock(u32 dimension_type) {
    const int clock = tl::kDefaultClock[dimension_type];
    return clock == tl::kNoClock ? ~0u : static_cast<u32>(clock);
}

// EnvironmentAttributeSystem.getDimensionValue(SKY_LIGHT_LEVEL): the attribute's default, through
// the one layer the overworld has for it without weather - the day's track, which multiplies -
// and into the attribute's range (sanitizeValue); then (int)(15.0f - value).
i32 sky_darken(i64 overworld_ticks) {
    static_assert(tl::kTimelines[tl::kSkyLightTimeline].clock == reg::world_clock::kOverworld);
    const float factor = sample(tl::kSkyLight, static_cast<u32>(std::size(tl::kSkyLight)),
                                tl::kTimelines[tl::kSkyLightTimeline].period, overworld_ticks);
    float level = kSkyLightDefault * factor;
    if (level < 0.0f) level = 0.0f;
    if (level > kSkyLightDefault) level = kSkyLightDefault;
    return static_cast<i32>(15.0f - level);
}

}  // namespace server
