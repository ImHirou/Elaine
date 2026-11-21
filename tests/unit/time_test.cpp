// SPDX-License-Identifier: GPL-3.0-or-later
// World clocks: how one ticks, what `/time set <marker>` moves it to, what a timeline makes of
// it, how dark the sky is by it - and the packets that carry time and suggestions, field by
// field. The numbers are worked out by hand from the 26.3 jar (ServerClockInstance.tick,
// ClockTimeMarker, KeyframeTrackSampler, data/minecraft/timeline/day.json), not by the code
// under test.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "proto/packets.hpp"
#include "server/clock.hpp"
#include "server/command.hpp"

#include PROTO_GEN_COMMAND_DATA_HPP

using namespace server;
using namespace proto;

namespace {

using Bytes = std::vector<u8>;

Bytes operator+(Bytes a, const Bytes& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}
Bytes vi(u32 v) {
    u8 buf[8];
    return {buf, put_varint(buf, v)};
}
Bytes be64(u64 v) {
    Bytes out(8);
    for (u32 i = 0; i < 8; ++i) out[i] = static_cast<u8>(v >> (56 - 8 * i));
    return out;
}
Bytes f32(float v) {
    const u32 b = std::bit_cast<u32>(v);
    return {static_cast<u8>(b >> 24), static_cast<u8>(b >> 16), static_cast<u8>(b >> 8), static_cast<u8>(b)};
}
Bytes str(std::string_view s) { return vi(static_cast<u32>(s.size())) + Bytes(s.begin(), s.end()); }

template <class P>
Bytes encoded(const P& p) {
    base::ByteBuf buf;
    u8* end = p.encode(buf.reserve(p.bound()));
    EXPECT_LE(static_cast<usize>(end - buf.data), p.bound());
    return {buf.data, end};
}

constexpr u32 kOverworld = reg::world_clock::kOverworld, kTheEnd = reg::world_clock::kTheEnd;
constexpr u32 kDay = reg::timeline::kDay, kMoon = reg::timeline::kMoon, kEarlyGame = reg::timeline::kEarlyGame;

i32 nothing(cmd::Invocation&) { return 1; }

}  // namespace

TEST(Clock, TicksAtItsRate) {
    Clock c;
    for (u32 i = 0; i < 5; ++i) c.tick();
    EXPECT_EQ(c.total_ticks, 5);
    EXPECT_EQ(c.partial_tick, 0.0f);
    // A quarter of the speed: a tick every fourth tick, the rest carried.
    c.rate = 0.25f;
    c.tick();
    EXPECT_EQ(c.total_ticks, 5);
    EXPECT_EQ(c.partial_tick, 0.25f);
    for (u32 i = 0; i < 3; ++i) c.tick();
    EXPECT_EQ(c.total_ticks, 6);
    EXPECT_EQ(c.partial_tick, 0.0f);
    // Two and a half times: 2, 3, 2, 3.
    c.rate = 2.5f;
    c.tick();
    EXPECT_EQ(c.total_ticks, 8);
    EXPECT_EQ(c.partial_tick, 0.5f);
    c.tick();
    EXPECT_EQ(c.total_ticks, 11);
    EXPECT_EQ(c.partial_tick, 0.0f);
    // The sum is a float's: a tenth, ten times, is not one.
    Clock tenth;
    tenth.rate = 0.1f;
    for (u32 i = 0; i < 10; ++i) tenth.tick();
    float sum = 0;
    for (u32 i = 0; i < 10; ++i) sum += 0.1f;
    EXPECT_EQ(tenth.total_ticks, sum >= 1.0f ? 1 : 0);
    EXPECT_EQ(tenth.partial_tick, sum >= 1.0f ? sum - 1.0f : sum);
    // Paused: nothing.
    c.paused = true;
    c.tick();
    EXPECT_EQ(c.total_ticks, 11);
}

TEST(Clock, NamesAndTimelines) {
    EXPECT_EQ(clock_name(kOverworld), "minecraft:overworld");
    EXPECT_EQ(find_clock("minecraft:the_end"), kTheEnd);
    EXPECT_EQ(find_clock("minecraft:nether"), ~0u);
    EXPECT_EQ(find_timeline("minecraft:day"), kDay);
    EXPECT_EQ(timeline_name(kMoon), "minecraft:moon");
    EXPECT_EQ(timeline_clock(kDay), kOverworld);
    EXPECT_EQ(default_clock(reg::dimension_type::kOverworld), kOverworld);
    EXPECT_EQ(default_clock(reg::dimension_type::kTheEnd), kTheEnd);
    EXPECT_EQ(default_clock(reg::dimension_type::kTheNether), ~0u);
    // The day is 24000 ticks, the moon's month eight of them; the early game has no period.
    EXPECT_EQ(timeline_ticks(kDay, 0), 0);
    EXPECT_EQ(timeline_ticks(kDay, 23999), 23999);
    EXPECT_EQ(timeline_ticks(kDay, 24000 * 3 + 6000), 6000);
    EXPECT_EQ(timeline_repetitions(kDay, 24000 * 3 + 6000), 3);
    EXPECT_EQ(timeline_ticks(kMoon, 192000 + 5), 5);
    EXPECT_EQ(timeline_repetitions(kMoon, 191999), 0);
    EXPECT_EQ(timeline_ticks(kEarlyGame, 1234567), 1234567);
    EXPECT_EQ(timeline_repetitions(kEarlyGame, 1234567), 0);
    // The overworld's clock has the day's markers - four of them for commands - the end's none.
    u32 shown = 0, hidden = 0, of_the_end = 0;
    MarkerRef m;
    for (u32 i = 0; i < marker_count(); ++i) {
        if (marker_of_clock(i, kOverworld, m)) ++(m.show_in_commands ? shown : hidden);
        of_the_end += marker_of_clock(i, kTheEnd, m);
    }
    EXPECT_EQ(shown, 4u);
    EXPECT_EQ(hidden, 2u);
    EXPECT_EQ(of_the_end, 0u);
}

TEST(Clock, MovesOnToATimeMarker) {
    // day is tick 1000 of the day, noon 6000, night 13000, midnight 18000: a clock moves forward
    // to the next one, never back.
    Clock c;
    EXPECT_EQ(move_to_marker(c, kOverworld, "minecraft:day"), Move::kMoved);
    EXPECT_EQ(c.total_ticks, 1000);
    EXPECT_EQ(move_to_marker(c, kOverworld, "minecraft:day"), Move::kNotMoved);
    EXPECT_EQ(c.total_ticks, 1000);
    EXPECT_EQ(move_to_marker(c, kOverworld, "minecraft:midnight"), Move::kMoved);
    EXPECT_EQ(c.total_ticks, 18000);
    EXPECT_EQ(move_to_marker(c, kOverworld, "minecraft:noon"), Move::kMoved);  // tomorrow's
    EXPECT_EQ(c.total_ticks, 24000 + 6000);
    c.partial_tick = 0.5f;
    EXPECT_EQ(move_to_marker(c, kOverworld, "minecraft:night"), Move::kMoved);
    EXPECT_EQ(c.total_ticks, 24000 + 13000);
    EXPECT_EQ(c.partial_tick, 0.0f);
    // The markers nobody is offered are markers all the same.
    EXPECT_EQ(move_to_marker(c, kOverworld, "minecraft:wake_up_from_sleep"), Move::kMoved);
    EXPECT_EQ(c.total_ticks, 48000);
    EXPECT_EQ(move_to_marker(c, kOverworld, "minecraft:wake_up_from_sleep"), Move::kNotMoved);
    // No such marker - or not one of this clock.
    EXPECT_EQ(move_to_marker(c, kOverworld, "minecraft:teatime"), Move::kNoMarker);
    EXPECT_EQ(move_to_marker(c, kOverworld, "day"), Move::kNoMarker);
    EXPECT_EQ(move_to_marker(c, kTheEnd, "minecraft:day"), Move::kNoMarker);
    EXPECT_EQ(c.total_ticks, 48000);
}

TEST(Clock, TheSkyDarkensByTheTimeOfDay) {
    // The day's sky_light_level track is a factor on 15: 1 from tick 133 to 11867, down to
    // 0.26666668 (4 / 15) by 13670, there until 22330, and up again to 1 at 133 of the next day.
    // Level.skyDarken is (int)(15.0f - level): 0 by day, 11 at night.
    for (const i64 t : {0ll, 133ll, 1000ll, 6000ll, 11867ll}) EXPECT_EQ(sky_darken(t), 0) << t;
    for (const i64 t : {13670ll, 18000ll, 22330ll}) EXPECT_EQ(sky_darken(t), 11) << t;
    // Dusk: 1803 ticks from 1 to 4 / 15, linear. A level is lost for every 15 / 11 of a 15th of
    // the way: by hand, darken = (int)(11 * (t - 11867) / 1803).
    const auto dusk = [](i64 t) {
        return static_cast<i32>(15.0f - 15.0f * (1.0f + static_cast<float>(t - 11867) / 1803.0f * (0.26666668f - 1.0f)));
    };
    EXPECT_EQ(sky_darken(11868), 0);
    EXPECT_EQ(sky_darken(12031), dusk(12031));
    EXPECT_EQ(sky_darken(12031), 1);
    EXPECT_EQ(sky_darken(12030), 0);
    EXPECT_EQ(sky_darken(13000), 6);  // "night": grass still spreads under an open sky (15 - 6 = 9)
    EXPECT_EQ(sky_darken(13015), 7);  // ...and a little later no longer
    EXPECT_EQ(sky_darken(13669), 10);
    for (i64 t = 11867; t <= 13670; ++t) {
        EXPECT_EQ(sky_darken(t), dusk(t)) << t;
        EXPECT_LE(sky_darken(t), sky_darken(t + 1)) << t;
    }
    // Dawn runs through midnight of the clock: from 22330 to 24133.
    EXPECT_EQ(sky_darken(22331), 10);
    EXPECT_EQ(sky_darken(23999), sky_darken(-1));
    EXPECT_EQ(sky_darken(24000), sky_darken(0));
    EXPECT_EQ(sky_darken(132), 0);
    EXPECT_EQ(sky_darken(23000), static_cast<i32>(15.0f - 15.0f * (0.26666668f + 670.0f / 1803.0f * (1.0f - 0.26666668f))));
    // Every day is the same day.
    for (const i64 t : {5ll, 12500ll, 13000ll, 20000ll, 23500ll}) EXPECT_EQ(sky_darken(t + 24000 * 1000), sky_darken(t)) << t;
}

TEST(TimePackets, SetTime) {
    // The game time, then a map: clock id -> ticks (a VarLong), the part of a tick, the rate.
    const cb::SetTime::Clock both[] = {{kOverworld, 13000, 0.25f, 1.0f}, {kTheEnd, 300, 0.0f, 0.0f}};
    EXPECT_EQ(encoded(cb::SetTime{0x0102030405060708ll, both}), be64(0x0102030405060708ull) + vi(2) + vi(kOverworld) +
                                                                    (Bytes{0xC8, 0x65}) + f32(0.25f) + f32(1.0f) + vi(kTheEnd) +
                                                                    (Bytes{0xAC, 0x02}) + f32(0.0f) + f32(0.0f));
    // The one every twentieth tick: no clock at all.
    EXPECT_EQ(encoded(cb::SetTime{20, {}}), be64(20) + vi(0));
    // A VarLong is as long as the number: a clock far on.
    const cb::SetTime::Clock far[] = {{kOverworld, 1ll << 40, 0.0f, 2.0f}};
    EXPECT_EQ(encoded(cb::SetTime{1, far}),
              be64(1) + vi(1) + vi(kOverworld) + (Bytes{0x80, 0x80, 0x80, 0x80, 0x80, 0x20}) + f32(0.0f) + f32(2.0f));
}

TEST(TimePackets, Suggestions) {
    const Bytes ask = vi(7) + str("/time set d");
    Reader r(ask.data(), ask.size());
    const sb::CommandSuggestion c = sb::CommandSuggestion::decode(r);
    EXPECT_TRUE(r.done());
    EXPECT_EQ(c.id, 7u);
    EXPECT_EQ(c.command, "/time set d");
    // The answer: the id, the range, and the texts - each with "no tooltip".
    const std::string texts[] = {"minecraft:day", "minecraft:midnight"};
    EXPECT_EQ(encoded(cb::CommandSuggestions{7, 10, 1, texts}),
              vi(7) + vi(10) + vi(1) + vi(2) + str("minecraft:day") + (Bytes{0}) + str("minecraft:midnight") + (Bytes{0}));
    EXPECT_EQ(encoded(cb::CommandSuggestions{3, 0, 0, {}}), vi(3) + vi(0) + vi(0) + vi(0));
}

// ---------------------------------------------------------------------------- the argument, the tree, what is suggested

namespace {

cmd::Registry time_like() {
    cmd::Registry r;
    for (const std::string& at : {std::string("time"), std::string("time of <clock:clock>")}) {
        r.add(at + " set <time:time>", cmd::kGamemasters, &nothing);
        r.add(at + " set <timemarker:time_marker>", cmd::kGamemasters, &nothing);
        r.add(at + " add <time:time(-2147483648)>", cmd::kGamemasters, &nothing);
        r.add(at + " pause", cmd::kGamemasters, &nothing);
        r.add(at + " query time", cmd::kGamemasters, &nothing);
        r.add(at + " query <timeline:timeline> [repetition]", cmd::kGamemasters, &nothing);
        if (at == "time") r.add("time query gametime", cmd::kGamemasters, &nothing);
    }
    r.add("list [uuids]", cmd::kAll, &nothing);
    return r;
}

// The last argument of a line, or the error with its cursor.
std::string read(const cmd::Registry& r, std::string_view line, cmd::Value* out = nullptr) {
    cmd::Parsed p;
    r.parse(line, cmd::kOwners, p);
    if (!p.run) return p.error.message.plain() + " @" + std::to_string(p.error.cursor);
    if (out && p.steps.back().has_value) *out = p.steps.back().value;
    return std::string(r.node_name(p.steps.back().node));
}

}  // namespace

TEST(TimeCommandArguments, ATimeIsANumberAndAUnit) {
    const cmd::Registry r = time_like();
    cmd::Value v;
    const auto ticks = [&](std::string_view text) {
        v = {};
        const std::string line = "time add " + std::string(text);
        const std::string got = read(r, line, &v);
        return got == "time" ? std::to_string(v.integer) : got;
    };
    EXPECT_EQ(ticks("10"), "10");
    EXPECT_EQ(ticks("10t"), "10");
    EXPECT_EQ(ticks("3s"), "60");
    EXPECT_EQ(ticks("1d"), "24000");
    EXPECT_EQ(ticks("1.5d"), "36000");
    EXPECT_EQ(ticks("0.5"), "1");   // Math.round: a half goes up...
    EXPECT_EQ(ticks("-0.5"), "0");  // ...also below zero
    EXPECT_EQ(ticks("-1.5"), "-1");
    EXPECT_EQ(ticks("0.4"), "0");
    EXPECT_EQ(ticks("0.024s"), "0");  // 0.48 of a tick
    EXPECT_EQ(ticks("0.026s"), "1");
    EXPECT_EQ(ticks("-3d"), "-72000");
    EXPECT_EQ(ticks("100000d"), "2147483647");   // what no int holds is the largest
    EXPECT_EQ(ticks("5x"), "Invalid unit @11");  // behind the unit
    EXPECT_EQ(ticks("5dd"), "Invalid unit @12");
    EXPECT_EQ(ticks("d"), "Expected float @9");
    EXPECT_EQ(ticks("1.2.3"), "Invalid float '1.2.3' @9");
    // `set` takes no time before 0 - and then reads the word as a time marker: an identifier.
    EXPECT_EQ(read(r, "time set 0"), "time");
    EXPECT_EQ(read(r, "time set 5s", &v), "time");
    EXPECT_EQ(v.integer, 100);
    EXPECT_EQ(read(r, "time set -5", &v), "timemarker");
    EXPECT_EQ(v.text, "minecraft:-5");
    EXPECT_EQ(read(r, "time set noon", &v), "timemarker");
    EXPECT_EQ(v.text, "minecraft:noon");
    EXPECT_EQ(read(r, "time set 12:30", &v), "timemarker");
    EXPECT_EQ(v.text, "12:30");
    // Neither a time nor an identifier: two complaints are none in particular.
    EXPECT_EQ(read(r, "time set -5D"), "Incorrect argument for command @9");
    // Clocks and timelines are named, with or without their namespace.
    EXPECT_EQ(read(r, "time of the_end pause"), "pause");
    EXPECT_EQ(read(r, "time of minecraft:overworld query day repetition"), "repetition");
    EXPECT_EQ(read(r, "time query minecraft:moon", &v), "timeline");
    EXPECT_EQ(v.integer, static_cast<i32>(kMoon));
    EXPECT_EQ(read(r, "time of nowhere pause"), "Can't find element 'minecraft:nowhere' of type 'minecraft:world_clock' @15");
    EXPECT_EQ(read(r, "time query week"), "Can't find element 'minecraft:week' of type 'minecraft:timeline' @15");
    EXPECT_EQ(read(r, "time query time"), "time");
    EXPECT_EQ(read(r, "time query gametime"), "gametime");
    EXPECT_EQ(read(r, "time of the_end query gametime"),
              "Can't find element 'minecraft:gametime' of type 'minecraft:timeline' @30");
}

TEST(TimeCommandArguments, TheTree) {
    // The nodes of "time set": a time with its least (an int, always there), and an identifier
    // the client asks the server about.
    cmd::Registry r;
    r.add("time set <time:time>", cmd::kAll, &nothing);
    r.add("time set <timemarker:time_marker>", cmd::kAll, &nothing);
    r.add("time add <time:time(-2147483648)>", cmd::kAll, &nothing);
    r.add("time of <clock:clock> query <timeline:timeline>", cmd::kAll, &nothing);
    base::ByteBuf buf;
    r.encode_tree(cmd::kAll, buf);
    const Bytes tree(buf.data, buf.data + buf.len);
    const auto holds = [&](const Bytes& part) {
        return std::search(tree.begin(), tree.end(), part.begin(), part.end()) != tree.end();
    };
    namespace argument = proto::gen::command::argument;
    // flags: an argument (2) that runs (4); no children; the name; the parser and its properties.
    const Bytes runs{2 | 4, 0}, asks{2 | 4 | 16, 0};
    EXPECT_TRUE(holds(runs + str("time") + vi(argument::kTime) + Bytes(4, 0)));
    EXPECT_TRUE(holds(runs + str("time") + vi(argument::kTime) + (Bytes{0x80, 0, 0, 0})));
    // ...with suggestions of its own (16), which are the server's to give.
    EXPECT_TRUE(holds(asks + str("timemarker") + vi(argument::kResourceLocation) + str("minecraft:ask_server")));
    EXPECT_TRUE(
        holds(asks + str("timeline") + vi(argument::kResource) + str("minecraft:timeline") + str("minecraft:ask_server")));
    // The clock is a resource the client completes from the registry it was sent: no flag.
    const Bytes clock = str("clock") + vi(argument::kResource) + str("minecraft:world_clock");
    const auto at = std::search(tree.begin(), tree.end(), clock.begin(), clock.end());
    ASSERT_NE(at, tree.end());
    EXPECT_EQ(*(at - 3), 2);  // flags, one child, its number
}

TEST(TimeCommandArguments, WhatTheServerSuggests) {
    const cmd::Registry r = time_like();
    // The arguments marked ask_server ask this: here, every name of a fixed list that fits.
    struct Asked {
        std::vector<std::string> log;
    } asked;
    const auto ask = [](void* ctx, const cmd::Registry::Asked& a, std::vector<std::string>& out) {
        std::string clock = "default";
        for (const cmd::Parsed::Step& s : a.steps)
            if (s.has_value && s.value.type == cmd::Type::kClock) clock = s.value.text;
        static_cast<Asked*>(ctx)->log.push_back((a.type == cmd::Type::kTimeMarker ? "marker " : "timeline ") + clock + " '" +
                                                std::string(a.typed) + "'");
        for (const std::string_view id : {"minecraft:day", "minecraft:midnight", "minecraft:noon", "other:deep/day_off"})
            if (cmd::suggests_resource(a.typed, id)) out.emplace_back(id);
    };
    const auto suggest = [&](std::string_view line, u8 level = cmd::kOwners) {
        cmd::Registry::Suggestions s;
        r.suggest(line, level, ask, &asked, s);
        std::string out = std::to_string(s.start) + "+" + std::to_string(s.length) + ":";
        for (const std::string& t : s.texts) out += " " + t;
        return out;
    };
    // Nothing typed yet: every marker; the time before it offers nothing without a number.
    EXPECT_EQ(suggest("time set "), "9+0: minecraft:day minecraft:midnight minecraft:noon other:deep/day_off");
    EXPECT_EQ(asked.log.back(), "marker default ''");
    // A letter: what begins with it - the path, the namespace, or a part after '.', '_' or '/'.
    EXPECT_EQ(suggest("time set d"), "9+1: minecraft:day other:deep/day_off");
    EXPECT_EQ(suggest("time set m"), "9+1: minecraft:day minecraft:midnight minecraft:noon");
    EXPECT_EQ(suggest("time set mi"), "9+2: minecraft:day minecraft:midnight minecraft:noon");
    EXPECT_EQ(suggest("time set mid"), "9+3: minecraft:midnight");
    EXPECT_EQ(suggest("time set minecraft:n"), "9+11: minecraft:noon");
    EXPECT_EQ(suggest("time set OFF"), "9+3: other:deep/day_off");
    EXPECT_EQ(asked.log.back(), "marker default 'off'");
    EXPECT_EQ(suggest("time set x"), "0+0:");
    // What is there in full is not suggested again.
    EXPECT_EQ(suggest("time set minecraft:noon"), "0+0:");
    // Behind a number, the units of a time - to be put behind it; the marker's argument is asked
    // too, and has nothing.
    EXPECT_EQ(suggest("time set 5"), "10+0: d s t");
    EXPECT_EQ(suggest("time set 1.5d"), "0+0:");
    EXPECT_EQ(suggest("time add 7"), "10+0: d s t");
    // Where both have something, every text covers what both would replace.
    EXPECT_EQ(suggest("time set 0"), "10+0: d s t");
    // "query": two words and the timelines, each stretched over what is typed.
    EXPECT_EQ(suggest("time query "), "11+0: gametime minecraft:day minecraft:midnight minecraft:noon other:deep/day_off time");
    EXPECT_EQ(asked.log.back(), "timeline default ''");
    EXPECT_EQ(suggest("time query g"), "11+1: gametime");
    EXPECT_EQ(suggest("time query t"), "11+1: time");
    EXPECT_EQ(suggest("time query d"), "11+1: minecraft:day other:deep/day_off");
    // Under "of <clock>" the question names the clock; "gametime" is not a word there.
    EXPECT_EQ(suggest("time of the_end query "), "22+0: minecraft:day minecraft:midnight minecraft:noon other:deep/day_off time");
    EXPECT_EQ(asked.log.back(), "timeline minecraft:the_end ''");
    EXPECT_EQ(suggest("time of minecraft:overworld set n"), "32+1: minecraft:noon");
    EXPECT_EQ(asked.log.back(), "marker minecraft:overworld 'n'");
    // Words are suggested like anything else: the children of the node the line stands in.
    EXPECT_EQ(suggest("time "), "5+0: add of pause query set");
    EXPECT_EQ(suggest("time q"), "5+1: query");
    EXPECT_EQ(suggest("ti"), "0+2: time");
    EXPECT_EQ(suggest(""), "0+0: list time");
    EXPECT_EQ(suggest("time set"), "0+0:");  // (the word is there in full)
    EXPECT_EQ(suggest("time se"), "5+2: set");
    // Only what the asker may use.
    EXPECT_EQ(suggest("", cmd::kAll), "0+0: list");
    EXPECT_EQ(suggest("time set ", cmd::kAll), "0+0:");
}
