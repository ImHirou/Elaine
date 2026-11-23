// SPDX-License-Identifier: GPL-3.0-or-later
// The time of the world: its clocks, who is told of them and when, what is kept of them - and
// the suggestions a client asks the server for, which are about clocks so far. And how fast the
// game ticks, which is told the same way: to everyone, when it changes.
//
// The rules are vanilla's, read from the 26.3 jar; each function names what it follows:
//   MinecraftServer (tickChildren, forceGameTimeSynchronization), ServerClockManager (tick,
//   modifyClock, createFullSyncPacket, commandTimeMarkersForClock), PlayerList.sendLevelInfo,
//   Level.updateSkyBrightness, ServerCommandSuggestionsProvider, TimeCommand (suggestTimeMarkers,
//   suggestTimelines), ServerTickRateManager. The clocks themselves are server/clock.*, the
//   tick rate's state is server/tick_rate.hpp.
//
// A client runs every clock it has been told of by itself - by the game time that passes, at
// the rate it was given - so what goes out is little: all clocks when a player joins, one clock
// when a command changes it, and every twentieth tick the game time alone, which is what the
// clocks of a client that fell behind catch up by.
#include "base/java_number.hpp"
#include "server/game.hpp"

namespace server {

using namespace proto;
using text::Text;

namespace {
constexpr u32 kSyncEvery = 20;  // MinecraftServer.tickChildren: tickCount % 20 == 0
}

// ClockInstance.packNetworkState: a clock that does not run - its own pause, or the game rule -
// is one with rate 0 to a client.
cb::SetTime::Clock Game::clock_state(u32 clock) const {
    const Clock& c = clocks_[clock];
    return {clock, c.total_ticks, c.partial_tick, c.paused || !s_.advance_time ? 0.0f : c.rate};
}

// ServerClockManager.tick - of a game that runs (`runs`: not one that is frozen) - then, on every
// twentieth tick, forceGameTimeSynchronization: the game time, as it is before this tick counts
// it on, and no clock. Level.updateSkyBrightness follows (ServerLevel.tick begins with it).
void Game::tick_clocks(bool runs) {
    if (runs && s_.advance_time)
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

// ---------------------------------------------------------------------------- the tick rate

// ServerTickRateManager.setTickRate: everyone is told (updateStateToClients).
void Game::set_tick_rate(float rate) {
    rate_.set_rate(rate);
    broadcast(cb::TickingState{rate_.rate, rate_.frozen});
}

// setFrozen: everyone is told, whether it changed or not.
void Game::set_frozen(bool frozen) {
    rate_.frozen = frozen;
    broadcast(cb::TickingState{rate_.rate, rate_.frozen});
}

// stepGameIfPaused: only a frozen game steps. A client counts the steps down by itself.
bool Game::step_game_if_paused(i32 ticks) {
    if (!rate_.frozen) return false;
    rate_.frozen_ticks_to_run = ticks;
    broadcast(cb::TickingStep{static_cast<u32>(ticks)});
    return true;
}

bool Game::stop_stepping() {
    if (rate_.frozen_ticks_to_run <= 0) return false;
    rate_.frozen_ticks_to_run = 0;
    broadcast(cb::TickingStep{0});
    return true;
}

bool Game::stop_sprinting() {
    if (rate_.remaining_sprint_ticks <= 0) return false;
    finish_tick_sprint();
    return true;
}

// requestGameToSprint: true if a sprint was under way (which this one takes the place of - and
// what "before" was is then the sprint, not frozen).
bool Game::request_game_to_sprint(i32 ticks) {
    const bool interrupted = rate_.remaining_sprint_ticks > 0;
    rate_.sprint_time_spent = 0;
    rate_.scheduled_sprint_ticks = ticks;
    rate_.remaining_sprint_ticks = ticks;
    rate_.previous_frozen = rate_.frozen;
    set_frozen(false);
    return interrupted;
}

// finishTickSprint: the server says how fast it was - to its log and to the operators - and the
// game is frozen again if it was.
void Game::finish_tick_sprint() {
    const i64 ticks = rate_.scheduled_sprint_ticks - rate_.remaining_sprint_ticks;
    const double ms = std::max(1.0, static_cast<double>(rate_.sprint_time_spent)) / static_cast<double>(base::kNsPerMs);
    const i32 per_second = static_cast<i32>(static_cast<double>(1000 * ticks) / ms);
    const std::string ms_per_tick =
        base::java_format_fixed(ticks == 0 ? static_cast<double>(rate_.ms_per_tick()) : ms / static_cast<double>(ticks), 2);
    rate_.scheduled_sprint_ticks = 0;
    rate_.sprint_time_spent = 0;
    command_success(console_source(), Text::translatable("commands.tick.sprint.report").arg(per_second).arg(ms_per_tick), true);
    rate_.remaining_sprint_ticks = 0;
    set_frozen(rate_.previous_frozen);
}

// checkShouldSprintThisTick, asked at the top of a loop while a sprint is scheduled: a tick of
// it - or its end. Not while the tick before was one of a frozen game: a sprint asked of a frozen
// game from the console begins a tick later, after one tick at the usual pace.
bool Game::check_should_sprint_this_tick() {
    if (!rate_.run_game_elements) return false;
    if (rate_.remaining_sprint_ticks > 0) {
        rate_.sprint_tick_start = base::now_ns();
        --rate_.remaining_sprint_ticks;
        return true;
    }
    finish_tick_sprint();
    return false;
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
