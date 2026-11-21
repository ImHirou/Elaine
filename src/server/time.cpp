// SPDX-License-Identifier: GPL-3.0-or-later
// The time of the world: its clocks, who is told of them and when, what is kept of them - and
// the suggestions a client asks the server for, which are about clocks so far.
//
// The rules are vanilla's, read from the 26.3 jar; each function names what it follows:
//   MinecraftServer (tickChildren, forceGameTimeSynchronization), ServerClockManager (tick,
//   modifyClock, createFullSyncPacket, commandTimeMarkersForClock), PlayerList.sendLevelInfo,
//   Level.updateSkyBrightness, ServerCommandSuggestionsProvider, TimeCommand (suggestTimeMarkers,
//   suggestTimelines). The clocks themselves are server/clock.*.
//
// A client runs every clock it has been told of by itself - by the game time that passes, at
// the rate it was given - so what goes out is little: all clocks when a player joins, one clock
// when a command changes it, and every twentieth tick the game time alone, which is what the
// clocks of a client that fell behind catch up by.
#include "server/game.hpp"

namespace server {

using namespace proto;

namespace {
constexpr u32 kSyncEvery = 20;  // MinecraftServer.tickChildren: tickCount % 20 == 0
}

// ClockInstance.packNetworkState: a clock that does not run - its own pause, or the game rule -
// is one with rate 0 to a client.
cb::SetTime::Clock Game::clock_state(u32 clock) const {
    const Clock& c = clocks_[clock];
    return {clock, c.total_ticks, c.partial_tick, c.paused || !s_.advance_time ? 0.0f : c.rate};
}

// ServerClockManager.tick, then - on every twentieth tick - forceGameTimeSynchronization: the
// game time, as it is before this tick counts it on, and no clock. Level.updateSkyBrightness
// follows (ServerLevel.tick begins with it).
void Game::tick_clocks() {
    if (s_.advance_time)
        for (Clock& c : clocks_) c.tick();
    if ((tick_ + 1) % kSyncEvery == 0 && !players_.empty()) broadcast(cb::SetTime{static_cast<i64>(time_), {}});
    sky_darken_ = sky_darken(clocks_[reg::world_clock::kOverworld].total_ticks);
}

// createFullSyncPacket, for someone who joins.
void Game::send_clocks(u32 pi) {
    cb::SetTime::Clock all[kClocks];
    for (u32 i = 0; i < kClocks; ++i) all[i] = clock_state(i);
    send(pi, cb::SetTime{static_cast<i64>(time_), all});
}

// modifyClock: everyone is told of the clock that was changed, at once; what is computed from
// it is computed again (invalidateTickCache).
void Game::clock_changed(u32 clock) {
    const cb::SetTime::Clock one = clock_state(clock);
    broadcast(cb::SetTime{static_cast<i64>(time_), {&one, 1}});
    sky_darken_ = sky_darken(clocks_[reg::world_clock::kOverworld].total_ticks);
}

// What time it is, into world.json (through a worker: server/files.hpp).
void Game::save_time() {
    if (info_.dir.empty()) return;
    WorldTime now;
    now.game_time = static_cast<i64>(time_);
    for (u32 i = 0; i < kClocks; ++i) now.clocks[i] = clocks_[i];
    const std::string json = world_json(info_, now);
    files_.write(info_.dir + "/world.json", {reinterpret_cast<const u8*>(json.data()), json.size()});
}

// ---------------------------------------------------------------------------- suggestions

// handleCustomCommandSuggestions -> ServerCommandSuggestionsProvider.request: the question is
// kept; a later one from the same player takes its place, and the earlier is never answered.
void Game::on_command_suggestion(u32 pi, const sb::CommandSuggestion& c) {
    const u64 who = ents_.handle(pi);
    for (SuggestionRequest& r : suggestion_requests_) {
        if (r.player != who) continue;
        r.id = c.id;
        r.line = c.command;
        return;
    }
    suggestion_requests_.push_back({who, c.id, std::string(c.command)});
}

// tryProcessRequest, once a tick: the line is read as its player may, and what could stand at
// its end is sent back - also when that is nothing.
void Game::answer_suggestions() {
    cmd::Registry::Suggestions found;
    for (const SuggestionRequest& r : suggestion_requests_) {
        const u32 pi = ents_.resolve(r.player);
        if (pi >= players_.size()) continue;
        std::string_view line = r.line;
        const u32 slash = line.starts_with('/') ? 1 : 0;
        line.remove_prefix(slash);
        commands_.suggest(line, players_[pi].level, &ask_server, this, found);
        // A StringRange counts chars as Java does, and from the slash.
        const u32 start = found.texts.empty() ? 0 : slash + utf16_length(line.substr(0, found.start));
        const u32 length = utf16_length(line.substr(found.start, found.length));
        send(pi, cb::CommandSuggestions{r.id, start, length, found.texts});
    }
    suggestion_requests_.clear();
}

// What the arguments marked minecraft:ask_server offer. TimeCommand.suggestTimeMarkers: the
// time markers of the clock the line is about that are meant for commands; suggestTimelines:
// the timelines that run on it.
void Game::ask_server(void*, const cmd::Registry::Asked& asked, std::vector<std::string>& out) {
    u32 clock = default_clock(reg::dimension_type::kOverworld);  // TimeCommand.getDefaultClock: the source's dimension's
    for (const cmd::Parsed::Step& s : asked.steps)
        if (s.has_value && s.value.type == cmd::Type::kClock) clock = static_cast<u32>(s.value.integer);
    if (clock == ~0u) return;
    if (asked.type == cmd::Type::kTimeMarker) {
        MarkerRef m;
        for (u32 i = 0; i < marker_count(); ++i)
            if (marker_of_clock(i, clock, m) && m.show_in_commands && cmd::suggests_resource(asked.typed, m.name))
                out.emplace_back(m.name);
    } else if (asked.type == cmd::Type::kTimeline) {
        for (u32 t = 0; t < timeline_count(); ++t)
            if (timeline_clock(t) == clock && cmd::suggests_resource(asked.typed, timeline_name(t)))
                out.emplace_back(timeline_name(t));
    }
}

}  // namespace server
