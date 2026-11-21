// SPDX-License-Identifier: GPL-3.0-or-later
// Commands: the syntax lines, how a typed line is read against the tree (Brigadier's rules and
// errors), the argument parsers, the `commands` packet, and the text a command answers with.
//
// The expected errors are vanilla's translation keys and cursors, read from the 26.3 jar; the
// expected tags are NBT written out by hand (nbt_bytes.hpp), as a vanilla server sent them
// (tools/vanilla/command_probe.py).
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "nbt_bytes.hpp"
#include "proto/gen/current.hpp"
#include "proto/packets.hpp"
#include "server/command.hpp"
#include "text/text.hpp"

#include PROTO_GEN_COMMAND_DATA_HPP
#include PROTO_GEN_ENTITIES_HPP

namespace {

using namespace server;
using namespace nbt_bytes;
using cmd::Registry;
using text::Color;
using text::Text;
namespace argument = proto::gen::command::argument;

i32 nothing(cmd::Invocation&) { return 1; }
i32 other(cmd::Invocation&) { return 2; }

// What a line comes to: "ok", or the error's key (as en_us prints it) and its cursor.
std::string read(const Registry& r, std::string_view line, u8 level = cmd::kOwners) {
    cmd::Parsed p;
    r.parse(line, level, p);
    if (p.run) return "ok";
    return p.error.message.plain() + " @" + std::to_string(p.error.cursor);
}

Bytes encoded(const Text& t) {
    base::ByteBuf out;
    t.encode(out);
    return {out.data, out.data + out.len};
}

Registry vanilla_like() {
    Registry r;
    r.add("setblock <pos:block_pos> <block:block_state> [(destroy|keep|replace|strict)]", cmd::kGamemasters, &nothing);
    r.add("give <targets:players> <item:item> [<count:int(1)>]", cmd::kGamemasters, &nothing);
    r.add("teleport <location:vec3>", cmd::kGamemasters, &nothing);
    r.add("teleport <destination:entity>", cmd::kGamemasters, &nothing);
    r.add("teleport <targets:entities> <location:vec3> [<rotation:rotation>]", cmd::kGamemasters, &nothing);
    r.add("teleport <targets:entities> <destination:entity>", cmd::kGamemasters, &nothing);
    r.alias("tp", "teleport", cmd::kGamemasters);
    r.add("list [uuids]", cmd::kAll, &nothing);
    r.add("kick <targets:players> [<reason:message>]", cmd::kAdmins, &nothing);
    r.add("volume <level:float(0,2)> <enabled:bool> <note:string>", cmd::kAll, &nothing);
    return r;
}

}  // namespace

// ---------------------------------------------------------------------------- the syntax line

TEST(CommandSyntax, LinesShareTheirNodesAndSayWhatIsWrong) {
    Registry r;
    std::string err;
    EXPECT_TRUE(r.add("warp <name:word>", cmd::kAll, &nothing, nullptr, err)) << err;
    EXPECT_TRUE(r.add("warp set <name:word> [<pos:block_pos>]", cmd::kAll, &nothing, nullptr, err)) << err;
    EXPECT_EQ(read(r, "warp home"), "ok");
    EXPECT_EQ(read(r, "warp set home"), "ok");
    EXPECT_EQ(read(r, "warp set home 1 2 3"), "ok");
    EXPECT_EQ(read(r, "warp set"), "Unknown or incomplete command. See below for error @8");

    EXPECT_FALSE(r.add("warp <name:int>", cmd::kAll, &nothing, nullptr, err));      // the name is a word already
    EXPECT_FALSE(r.add("warp <name:word>", cmd::kAll, &other, nullptr, err));       // ...and runs something else
    EXPECT_FALSE(r.add("warp <name:word>", cmd::kAdmins, &nothing, nullptr, err));  // one level per command
    EXPECT_FALSE(r.add("<name:word>", cmd::kAll, &nothing, nullptr, err));          // a command has a name
    EXPECT_FALSE(r.add("x <name:nonsense>", cmd::kAll, &nothing, nullptr, err));
    EXPECT_FALSE(r.add("x <name:word(1)>", cmd::kAll, &nothing, nullptr, err));  // only numbers have bounds
    EXPECT_FALSE(r.add("x <name>", cmd::kAll, &nothing, nullptr, err));
    EXPECT_FALSE(r.add("", cmd::kAll, &nothing, nullptr, err));

    const u32 before = r.version();
    EXPECT_TRUE(r.remove("warp"));
    EXPECT_NE(r.version(), before);
    EXPECT_FALSE(r.remove("warp"));
    EXPECT_EQ(read(r, "warp home"), "Unknown or incomplete command. See below for error @0");
}

TEST(CommandSyntax, AHandlerReadsWhatWasTypedByName) {
    Registry r;
    r.add("mode (fast|slow) <times:int(1,9)> [<why:greedy>]", cmd::kAll, &nothing);
    cmd::Parsed p;
    r.parse("mode slow 3 because of reasons", cmd::kAll, p);
    ASSERT_TRUE(p.run);
    ASSERT_EQ(p.steps.size(), 4u);
    EXPECT_EQ(r.node_name(p.steps[1].node), "slow");
    EXPECT_FALSE(p.steps[1].has_value);
    EXPECT_EQ(p.steps[2].value.integer, 3);
    EXPECT_EQ(p.steps[3].value.text, "because of reasons");
    EXPECT_EQ(p.steps[3].value.start, 12u);
}

// ---------------------------------------------------------------------------- reading a line

TEST(CommandParse, ErrorsAreBrigadiersAtItsCursor) {
    const Registry r = vanilla_like();
    EXPECT_EQ(read(r, "list"), "ok");
    EXPECT_EQ(read(r, "list uuids"), "ok");
    EXPECT_EQ(read(r, "setblock 1 2 3 stone"), "ok");
    EXPECT_EQ(read(r, "setblock ~ ~-1 ~ minecraft:stone keep"), "ok");
    // CommandDispatcher: nothing matched at all; something did, and then nothing; the end came early.
    EXPECT_EQ(read(r, "foo"), "Unknown or incomplete command. See below for error @0");
    EXPECT_EQ(read(r, ""), "Unknown or incomplete command. See below for error @0");
    EXPECT_EQ(read(r, "list everything"), "Incorrect argument for command @5");
    EXPECT_EQ(read(r, "setblock 1 2 3 stone extra"), "Incorrect argument for command @21");
    EXPECT_EQ(read(r, "setblock 1 2 3"), "Unknown or incomplete command. See below for error @14");
    EXPECT_EQ(read(r, "setblock"), "Unknown or incomplete command. See below for error @8");
    EXPECT_EQ(read(r, "list "), "Incorrect argument for command @4");
    // One argument child, and it threw: that is the error.
    EXPECT_EQ(read(r, "setblock 1 2"), "Incomplete (expected 3 coordinates) @9");
    EXPECT_EQ(read(r, "setblock 1 2 x stone"), "Expected integer @13");
    EXPECT_EQ(read(r, "setblock 1 2 3 nonsense"), "Unknown block type 'minecraft:nonsense' @15");
    EXPECT_EQ(read(r, "setblock 1 2 3 stone[x=1]"), "Block minecraft:stone does not have property 'x' @21");
    EXPECT_EQ(read(r, "give Steve stone 0"), "Integer must not be less than 1: found 0 @17");
    EXPECT_EQ(read(r, "give Steve stone 1x"), "Expected whitespace to end one argument, but found trailing data @18");
    EXPECT_EQ(read(r, "give Steve stone 99999999999"), "Invalid integer '99999999999' @17");
    EXPECT_EQ(read(r, "give Steve nonsense"), "Unknown item 'minecraft:nonsense' @11");
    EXPECT_EQ(read(r, "volume 3 true x"), "Float must not be more than 2.0: found 3.0 @7");
    EXPECT_EQ(read(r, "volume 1 maybe x"), "Invalid boolean: expected 'true' or 'false' but found 'maybe' @9");
    EXPECT_EQ(read(r, "volume 1 true \"a b\""), "ok");
    EXPECT_EQ(read(r, "volume 1 true \"a b"), "Unclosed quoted string @18");
    // Several argument children that all threw: no one error stands out.
    EXPECT_EQ(read(r, "teleport ~ ~"), "Incorrect argument for command @9");
    // ...and a reading that got further without an error hides the errors of one that did not:
    // "destination" took Steve and stopped; "targets" took Steve and then failed twice.
    EXPECT_EQ(read(r, "teleport Steve @e"), "Incorrect argument for command @15");
    // Too many for an argument that takes one: vanilla puts the cursor at the start of the line.
    Registry one;
    one.add("poke <who:entity>", cmd::kAll, &nothing);
    EXPECT_EQ(read(one, "poke @e"), "Only one entity is allowed, but the provided selector allows more than one @0");
    EXPECT_EQ(read(r, "kick @e"), "Only players may be affected by this command, but the provided selector includes entities @0");
    // ...and behind an entity type nobody registered (ResourceArgument leaves the reader there).
    one.add("spawn <entity:summonable>", cmd::kAll, &nothing);
    EXPECT_EQ(read(one, "spawn minecraft:pig"), "ok");
    EXPECT_EQ(read(one, "spawn nonsense"), "Can't find element 'minecraft:nonsense' of type 'minecraft:entity_type' @14");
}

TEST(CommandParse, AmongSeveralReadingsTheOneThatUsesTheLineWins) {
    const Registry r = vanilla_like();
    cmd::Parsed p;
    // "location" is tried first and fails; "destination" takes a name.
    r.parse("teleport Steve", cmd::kOwners, p);
    ASSERT_TRUE(p.run);
    EXPECT_EQ(r.node_name(p.steps.back().node), "destination");
    r.parse("teleport 1 2 3", cmd::kOwners, p);
    ASSERT_TRUE(p.run);
    EXPECT_EQ(r.node_name(p.steps.back().node), "location");
    r.parse("teleport @a 1 2 3 90 0", cmd::kOwners, p);
    ASSERT_TRUE(p.run);
    EXPECT_EQ(r.node_name(p.steps.back().node), "rotation");
    EXPECT_EQ(p.steps.back().value.coords.c[0].value, 90.0);
    // An alias goes on reading where the command it stands for begins.
    r.parse("tp @a Steve", cmd::kOwners, p);
    ASSERT_TRUE(p.run);
    EXPECT_EQ(r.node_name(p.steps.front().node), "tp");
    EXPECT_EQ(r.node_name(p.steps.back().node), "destination");
    EXPECT_EQ(read(r, "tp"), "Unknown or incomplete command. See below for error @2");
}

TEST(CommandParse, WhatASourceMayNotUseIsNotThere) {
    const Registry r = vanilla_like();
    EXPECT_EQ(read(r, "list", cmd::kAll), "ok");
    EXPECT_EQ(read(r, "setblock 1 2 3 stone", cmd::kAll), "Unknown or incomplete command. See below for error @0");
    EXPECT_EQ(read(r, "setblock 1 2 3 stone", cmd::kGamemasters), "ok");
    EXPECT_EQ(read(r, "kick Steve", cmd::kGamemasters), "Unknown or incomplete command. See below for error @0");
    EXPECT_EQ(read(r, "kick Steve", cmd::kAdmins), "ok");
    // Selectors are for gamemasters and up.
    Registry open;
    open.add("ping <who:players>", cmd::kAll, &nothing);
    EXPECT_EQ(read(open, "ping Steve", cmd::kAll), "ok");
    EXPECT_EQ(read(open, "ping @a", cmd::kAll), "Selector not allowed @5");
    EXPECT_EQ(read(open, "ping @a", cmd::kGamemasters), "ok");
}

TEST(CommandParse, TheErrorIsShownInItsLine) {
    // Commands.finishParsing: ten characters before the cursor, the rest underlined, the marker.
    EXPECT_EQ(cmd::error_context("setblock 1 2 3 stone extra", 21).plain(), "...2 3 stone extra<--[HERE]");
    EXPECT_EQ(cmd::error_context("foo", 0).plain(), "foo<--[HERE]");
    EXPECT_EQ(cmd::error_context("setblock", 8).plain(), "setblock<--[HERE]");
    // Counted in characters: ten Cyrillic letters are twenty bytes.
    EXPECT_EQ(cmd::error_context("say \xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 \xD0\xBC\xD0\xB8\xD1\x80 x", 25).plain(),
              "...\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 \xD0\xBC\xD0\xB8\xD1\x80 x<--[HERE]");
}

TEST(CommandParse, UsageLinesAreBrigadiers) {
    const Registry r = vanilla_like();
    std::vector<std::string> lines;
    ASSERT_TRUE(r.usage("", cmd::kOwners, lines));
    const std::vector<std::string> all{
        "setblock <pos> <block> [destroy|keep|replace|strict]",
        "give <targets> <item> [<count>]",
        "teleport (<location>|<destination>|<targets>)",
        "tp -> teleport",
        "list [uuids]",
        "kick <targets> [<reason>]",
        "volume <level> <enabled> <note>",
    };
    EXPECT_EQ(lines, all);
    lines.clear();
    ASSERT_TRUE(r.usage("", cmd::kAll, lines));  // only what the source may use
    EXPECT_EQ(lines, (std::vector<std::string>{"list [uuids]", "volume <level> <enabled> <note>"}));
    lines.clear();
    ASSERT_TRUE(r.usage("teleport @a", cmd::kOwners, lines));
    EXPECT_EQ(lines, (std::vector<std::string>{"<location> [<rotation>]", "<destination>"}));
    lines.clear();
    ASSERT_TRUE(r.usage("tp", cmd::kOwners, lines));  // a redirecting node has no children of its own
    EXPECT_TRUE(lines.empty());
    EXPECT_FALSE(r.usage("nonsense", cmd::kOwners, lines));
}

// ---------------------------------------------------------------------------- argument types

namespace {

std::string coords(std::string_view text, cmd::Type type, cmd::Coordinates* out = nullptr) {
    cmd::Coordinates c;
    cmd::Error err;
    u32 cursor = 0;
    if (!cmd::parse_coordinates(text, cursor, type, c, err)) return err.message.plain() + " @" + std::to_string(err.cursor);
    if (out) *out = c;
    return "ok " + std::to_string(cursor);
}

}  // namespace

TEST(CommandArguments, Coordinates) {
    cmd::Coordinates c;
    EXPECT_EQ(coords("1 2 3", cmd::Type::kBlockPos, &c), "ok 5");
    EXPECT_EQ(c.c[0].value, 1.0);
    EXPECT_FALSE(c.c[0].relative || c.local);
    EXPECT_EQ(coords("~ ~-1.5 ~2 rest", cmd::Type::kBlockPos, &c), "ok 10");  // stops at the fourth word
    EXPECT_TRUE(c.c[0].relative && c.c[1].relative);
    EXPECT_EQ(c.c[0].value, 0.0);
    EXPECT_EQ(c.c[1].value, -1.5);
    EXPECT_EQ(coords("^ ^1 ^-2", cmd::Type::kBlockPos, &c), "ok 8");
    EXPECT_TRUE(c.local);
    EXPECT_EQ(c.c[2].value, -2.0);
    // A whole number in a vec3 is the middle of that block - sideways, not in height.
    EXPECT_EQ(coords("1 2 3", cmd::Type::kVec3, &c), "ok 5");
    EXPECT_EQ(c.c[0].value, 1.5);
    EXPECT_EQ(c.c[1].value, 2.0);
    EXPECT_EQ(c.c[2].value, 3.5);
    EXPECT_EQ(coords("1.0 -.5 ~3", cmd::Type::kVec3, &c), "ok 10");
    EXPECT_EQ(c.c[0].value, 1.0);
    EXPECT_EQ(c.c[1].value, -0.5);
    EXPECT_EQ(c.c[2].value, 3.0);
    EXPECT_EQ(coords("90 ~-10", cmd::Type::kRotation, &c), "ok 7");
    EXPECT_EQ(c.c[0].value, 90.0);
    EXPECT_TRUE(c.c[1].relative);

    EXPECT_EQ(coords("1 2", cmd::Type::kBlockPos), "Incomplete (expected 3 coordinates) @0");
    EXPECT_EQ(coords("1 2 ", cmd::Type::kBlockPos), "Expected a block position @4");
    EXPECT_EQ(coords("1.5 2 3", cmd::Type::kBlockPos), "Invalid integer '1.5' @0");
    EXPECT_EQ(coords("1 ^2 3", cmd::Type::kBlockPos),
              "Cannot mix world and local coordinates (everything must either use ^ or not) @2");
    EXPECT_EQ(coords("^1 2 3", cmd::Type::kVec3),
              "Cannot mix world and local coordinates (everything must either use ^ or not) @0");
    EXPECT_EQ(coords("1 2 x", cmd::Type::kVec3), "Expected double @4");
    EXPECT_EQ(coords("", cmd::Type::kVec3), "Expected a coordinate @0");
    EXPECT_EQ(coords("90", cmd::Type::kRotation), "Incomplete (expected 2 coordinates) @0");
}

namespace {

std::string selector(std::string_view text, cmd::Type type, cmd::Selector* out = nullptr, bool allow = true) {
    cmd::Selector s;
    cmd::Error err;
    u32 cursor = 0;
    if (!cmd::parse_selector(text, cursor, type, allow, s, err)) return err.message.plain() + " @" + std::to_string(err.cursor);
    if (out) *out = s;
    return "ok " + std::to_string(cursor);
}

}  // namespace

// MessageArgument: the rest of the line, at most 256 characters of it; for a source that may use
// selectors, the selectors in it and where they stand.
TEST(CommandArguments, Messages) {
    Registry r;
    r.add("me <action:message>", cmd::kAll, &nothing);
    const auto message = [&](std::string_view line, u8 level, cmd::Value& out) {
        cmd::Parsed p;
        r.parse(line, level, p);
        if (!p.run) return p.error.message.plain() + " @" + std::to_string(p.error.cursor);
        out = p.steps.back().value;
        return std::string("ok");
    };
    cmd::Value v;
    ASSERT_EQ(message("me waves at @a and @e[type=pig] !", cmd::kAll, v), "ok");
    EXPECT_EQ(v.type, cmd::Type::kMessage);
    EXPECT_EQ(v.text, "waves at @a and @e[type=pig] !");
    EXPECT_TRUE(v.parts.empty()) << "who may not use selectors has none read";
    ASSERT_EQ(message("me waves at @a and @e[type=pig] !", cmd::kGamemasters, v), "ok");
    EXPECT_EQ(v.text, "waves at @a and @e[type=pig] !");
    ASSERT_EQ(v.parts.size(), 2u);
    EXPECT_EQ(v.text.substr(v.parts[0].start, v.parts[0].end - v.parts[0].start), "@a");
    EXPECT_EQ(v.text.substr(v.parts[1].start, v.parts[1].end - v.parts[1].start), "@e[type=pig]");
    EXPECT_FALSE(v.parts[0].selector.includes_entities);
    EXPECT_TRUE(v.parts[1].selector.includes_entities);
    // An '@' that begins no selector is a character: a mail address, an '@' at the end.
    ASSERT_EQ(message("me mail me@example.org or @ or @", cmd::kOwners, v), "ok");
    ASSERT_EQ(v.parts.size(), 1u);
    EXPECT_EQ(v.text.substr(v.parts[0].start, v.parts[0].end - v.parts[0].start),
              "@e");  // of "me@example.org": vanilla's reading too
    ASSERT_EQ(message("me a@b @x @", cmd::kOwners, v), "ok");
    EXPECT_TRUE(v.parts.empty());
    // Any other mistake in a selector is the message's mistake - for who has them read.
    EXPECT_EQ(message("me hi @e[type=nothing]", cmd::kOwners, v), "Invalid or unknown entity type 'minecraft:nothing' @14");
    EXPECT_EQ(message("me hi @e[type=nothing]", cmd::kAll, v), "ok");
    // 256 characters, as Java counts them; the complaint names no place.
    EXPECT_EQ(message("me " + std::string(256, 'a'), cmd::kAll, v), "ok");
    EXPECT_EQ(message("me " + std::string(257, 'a'), cmd::kAll, v),
              "Chat message was too long (257 > maximum 256 characters) @-1");
    std::string emoji;
    for (u32 i = 0; i < 128; ++i) emoji += "\xF0\x9F\x98\x80";
    EXPECT_EQ(message("me " + emoji, cmd::kAll, v), "ok");
    EXPECT_EQ(message("me " + emoji + "a", cmd::kAll, v), "Chat message was too long (257 > maximum 256 characters) @-1");
}

TEST(CommandArguments, EntitySelectors) {
    using T = cmd::Type;
    using Check = cmd::Selector::Test;
    cmd::Selector s;
    EXPECT_EQ(selector("Steve", T::kEntity, &s), "ok 5");
    EXPECT_TRUE(s.by_name && !s.includes_entities);
    EXPECT_EQ(s.name, "Steve");
    EXPECT_EQ(selector("dd12be42-52a9-4a91-a8a1-11c01849e498 x", T::kEntity, &s), "ok 36");
    EXPECT_TRUE(s.by_uuid && s.includes_entities);
    EXPECT_EQ(s.uuid, (Uuid{0xdd12be4252a94a91ull, 0xa8a111c01849e498ull}));
    EXPECT_EQ(selector("@a", T::kPlayers, &s), "ok 2");
    EXPECT_EQ(s.type, static_cast<u32>(proto::gen::entity_type::kPlayer));
    EXPECT_EQ(s.max_results, 0x7FFFFFFF);
    EXPECT_EQ(selector("@p", T::kPlayer, &s), "ok 2");
    EXPECT_EQ(s.order, cmd::Selector::kNearest);
    EXPECT_EQ(selector("@s", T::kPlayer, &s), "ok 2");
    EXPECT_TRUE(s.current_entity);

    EXPECT_EQ(selector("@e[type=!minecraft:player, type=!item ,distance=..5,limit=3,sort=furthest]", T::kEntities, &s), "ok 74");
    EXPECT_EQ(s.max_results, 3);
    EXPECT_EQ(s.order, cmd::Selector::kFurthest);
    EXPECT_TRUE(s.has_distance && !s.distance.has_min && s.distance.has_max && s.distance.max == 5.0);
    EXPECT_TRUE(s.world_limited && s.includes_entities);
    ASSERT_EQ(s.tests.size(), 3u);  // alive, and the two types
    EXPECT_EQ(s.tests[1].kind, Check::kType);
    EXPECT_TRUE(s.tests[1].inverted);
    EXPECT_EQ(s.tests[2].id, static_cast<u32>(proto::gen::entity_type::kItem));
    EXPECT_EQ(s.type, cmd::kNone);
    EXPECT_EQ(selector("@e[type=zombie,x=1,dy=-3.5,name=\"A B\",x_rotation=-10..10]", T::kEntities, &s), "ok 57");
    EXPECT_EQ(s.type, world::find_entity_type("minecraft:zombie"));
    EXPECT_TRUE(s.has_pos[0] && !s.has_pos[1] && s.has_delta[1] && s.delta[1] == -3.5 && s.has_rot_x);
    EXPECT_EQ(s.tests.back().text, "A B");
    EXPECT_EQ(selector("@e[type=#minecraft:skeletons]", T::kEntities, &s), "ok 29");
    EXPECT_EQ(s.tests.back().kind, Check::kTypeTag);
    EXPECT_FALSE(s.tests.back().ids.empty());
    EXPECT_EQ(selector("@a[gamemode=!survival,level=0..]", T::kPlayers), "ok 32");

    EXPECT_EQ(selector("@x", T::kEntities), "Unknown selector type '@x' @1");
    EXPECT_EQ(selector("@", T::kEntities), "Missing selector type @1");
    EXPECT_EQ(selector("@e[", T::kEntities), "Expected end of options @3");
    EXPECT_EQ(selector("@e[type=zombie", T::kEntities), "Expected end of options @14");
    EXPECT_EQ(selector("@e[type]", T::kEntities), "Expected value for option 'type' @3");
    EXPECT_EQ(selector("@e[nonsense=1]", T::kEntities), "Unknown option 'nonsense' @3");
    EXPECT_EQ(selector("@e[type=nonsense]", T::kEntities), "Invalid or unknown entity type 'minecraft:nonsense' @8");
    EXPECT_EQ(selector("@e[type=zombie,type=cow]", T::kEntities), "Option 'type' isn't applicable here @15");
    EXPECT_EQ(selector("@e[limit=0]", T::kEntities), "Limit must be at least 1 @9");
    EXPECT_EQ(selector("@e[limit=1,limit=2]", T::kEntities), "Option 'limit' isn't applicable here @11");
    EXPECT_EQ(selector("@s[limit=1]", T::kEntities), "Option 'limit' isn't applicable here @3");
    EXPECT_EQ(selector("@e[distance=-1]", T::kEntities), "Distance cannot be negative @12");
    EXPECT_EQ(selector("@e[distance=5..1]", T::kEntities), "Min cannot be bigger than max @12");
    EXPECT_EQ(selector("@e[sort=sideways]", T::kEntities), "Invalid or unknown sort type 'sideways' @8");
    EXPECT_EQ(selector("@a[gamemode=hard]", T::kPlayers), "Invalid or unknown game mode 'hard' @12");
    EXPECT_EQ(selector("@e[nbt={}]", T::kEntities), "Selectors with 'nbt' are not supported by this server yet @3");
    EXPECT_EQ(selector("", T::kEntity), "Invalid name or UUID @0");
    EXPECT_EQ(selector("ThisNameIsFarTooLong", T::kEntity), "Invalid name or UUID @0");
    EXPECT_EQ(selector("@e", T::kEntity), "Only one entity is allowed, but the provided selector allows more than one @0");
    EXPECT_EQ(selector("@e[limit=1]", T::kEntity), "ok 11");
    EXPECT_EQ(selector("@a", T::kPlayer), "Only one player is allowed, but the provided selector allows more than one @0");
    EXPECT_EQ(selector("@e", T::kPlayers),
              "Only players may be affected by this command, but the provided selector includes entities @0");
    EXPECT_EQ(selector("@e[type=player]", T::kPlayers), "ok 15");
    EXPECT_EQ(selector("@a", T::kPlayers, nullptr, false), "Selector not allowed @0");
    // A profile is any name up to the next space - or a selector of players.
    EXPECT_EQ(selector("Some.Name-That_Is+Long x", T::kProfile, &s), "ok 22");
    EXPECT_EQ(s.name, "Some.Name-That_Is+Long");
    EXPECT_EQ(selector("@a", T::kProfile, &s), "ok 2");
    EXPECT_TRUE(s.uses_selector);
    EXPECT_EQ(selector("@e", T::kProfile),
              "Only players may be affected by this command, but the provided selector includes entities @2");
}

namespace {

std::string block(std::string_view text, bool predicate, cmd::BlockArg* out = nullptr) {
    cmd::BlockArg b;
    cmd::Error err;
    u32 cursor = 0;
    if (!cmd::parse_block(text, cursor, predicate, b, err)) return err.message.plain() + " @" + std::to_string(err.cursor);
    if (out) *out = b;
    return "ok " + std::to_string(cursor);
}

}  // namespace

TEST(CommandArguments, BlockStatesAndPredicates) {
    namespace chest = proto::gen::props::chest;
    cmd::BlockArg b;
    EXPECT_EQ(block("stone", false, &b), "ok 5");
    EXPECT_EQ(b.state, proto::gen::block::kStone);
    EXPECT_EQ(block("minecraft:chest rest", false, &b), "ok 15");
    EXPECT_EQ(b.state, proto::gen::block::kChest);  // the default state: facing north, single, dry
    // facing: north south west east; type: single left right; waterlogged: true false.
    EXPECT_EQ(block("chest[facing=east, type = right]", false, &b), "ok 32");
    const unsigned first = world::block_info_of(proto::gen::block::kChest).first_state;
    EXPECT_EQ(b.state, first + 3 * chest::kFacingStride + 2 * chest::kTypeStride + 1);
    ASSERT_EQ(b.given.size(), 2u);
    EXPECT_TRUE(b.test(b.state));
    EXPECT_TRUE(b.test(static_cast<world::BlockState>(b.state - 1)));  // waterlogged was not asked about
    EXPECT_FALSE(b.test(static_cast<world::BlockState>(b.state - chest::kTypeStride)));
    EXPECT_FALSE(b.test(proto::gen::block::kStone));
    EXPECT_EQ(block("chest[]", false, &b), "ok 7");
    EXPECT_EQ(b.state, proto::gen::block::kChest);

    EXPECT_EQ(block("nonsense", false), "Unknown block type 'minecraft:nonsense' @0");
    EXPECT_EQ(block("Stone", false), "Unknown block type 'minecraft:' @0");  // an identifier ends at a capital
    EXPECT_EQ(block("chest[colour=red]", false), "Block minecraft:chest does not have property 'colour' @6");
    EXPECT_EQ(block("chest[facing=up]", false), "Block minecraft:chest does not accept 'up' for facing property @13");
    EXPECT_EQ(block("chest[facing=east,facing=west]", false),
              "Property 'facing' can only be set once for block minecraft:chest @18");
    EXPECT_EQ(block("chest[facing]", false), "Expected value for property 'facing' on block minecraft:chest @12");
    EXPECT_EQ(block("chest[facing=east", false), "Expected closing ] for block state properties @17");
    EXPECT_EQ(block("chest[facing=east;", false), "Expected closing ] for block state properties @17");
    EXPECT_EQ(block("chest{Items:[]}", false), "Block entity data in commands are not supported by this server yet @5");
    EXPECT_EQ(block("#logs", false), "Tags aren't allowed here; only actual blocks are @0");

    EXPECT_EQ(block("#minecraft:logs", true, &b), "ok 15");
    EXPECT_TRUE(b.is_tag);
    EXPECT_TRUE(b.test(proto::gen::block::kOakLog));
    EXPECT_FALSE(b.test(proto::gen::block::kStone));
    EXPECT_EQ(block("#logs[axis=x]", true, &b), "ok 13");
    EXPECT_FALSE(b.test(proto::gen::block::kOakLog));  // the default log stands upright
    EXPECT_EQ(block("#nonsense", true), "Unknown block tag 'minecraft:nonsense' @0");
}

TEST(CommandArguments, ItemsAndTags) {
    cmd::ItemArg item;
    cmd::Error err;
    u32 cursor = 0;
    ASSERT_TRUE(cmd::parse_item("minecraft:diamond 3", cursor, false, item, err));
    EXPECT_EQ(cursor, 17u);
    EXPECT_EQ(item.item, world::find_item("minecraft:diamond"));
    cursor = 0;
    EXPECT_FALSE(cmd::parse_item("diamond[damage=1]", cursor, false, item, err));
    EXPECT_EQ(err.cursor, 7);
    cursor = 0;
    ASSERT_TRUE(cmd::parse_item("#minecraft:planks", cursor, true, item, err));
    EXPECT_TRUE(item.test(world::find_item("minecraft:oak_planks")));
    EXPECT_FALSE(item.test(world::find_item("minecraft:stone")));
    cursor = 0;
    ASSERT_TRUE(cmd::parse_item("*", cursor, true, item, err));
    EXPECT_TRUE(item.test(1));

    std::vector<u16> members;
    EXPECT_TRUE(cmd::tag_members("minecraft:block", "minecraft:snow", members));
    EXPECT_FALSE(members.empty());
    EXPECT_FALSE(cmd::tag_members("minecraft:block", "minecraft:nonsense", members));
}

TEST(CommandArguments, NamesAreLookedUp) {
    EXPECT_EQ(world::find_block("minecraft:stone"), world::block_of(proto::gen::block::kStone));
    EXPECT_EQ(world::find_block("minecraft:air"), 0u);
    EXPECT_EQ(world::find_block("stone"), world::kNoId);  // names are whole identifiers
    EXPECT_EQ(world::item_name(world::find_item("minecraft:golden_apple")), "minecraft:golden_apple");
    EXPECT_EQ(world::item_text_key(world::find_item("minecraft:stone")), "block.minecraft.stone");
    EXPECT_EQ(world::item_text_key(world::find_item("minecraft:diamond")), "item.minecraft.diamond");
    EXPECT_EQ(world::item_rarity(world::find_item("minecraft:enchanted_golden_apple")), 2u);  // rare
    EXPECT_EQ(world::find_entity_type("minecraft:tnt"), static_cast<u32>(proto::gen::entity_type::kTnt));
    EXPECT_TRUE(world::summonable(proto::gen::entity_type::kTnt));
    EXPECT_FALSE(world::summonable(proto::gen::entity_type::kPlayer));
    for (u32 b = 0; b < proto::gen::state::kBlockCount; ++b) {  // the strides span exactly the block's states
        u32 states = 1;
        for (u32 i = 0, n = world::block_property_count(b); i < n; ++i)
            states *= static_cast<u32>(world::block_property(b, i).values.size());
        ASSERT_EQ(states, world::block_info(b).state_count) << world::block_name(b);
    }
}

// ---------------------------------------------------------------------------- the packet

TEST(CommandTree, CommandsPacketLayout) {
    Registry r;
    r.add("give <targets:players> <item:item> [<count:int(1)>]", cmd::kGamemasters, &nothing);
    r.add("list [uuids]", cmd::kAll, &nothing);
    r.add("playsound <sound:sound> [<volume:float(0,2)>]", cmd::kGamemasters, &nothing);
    r.add("summon <entity:summonable> [<at:vec3>]", cmd::kGamemasters, &nothing);
    r.add("say <name:word> <text:greedy>", cmd::kAll, &nothing);
    r.alias("g", "give", cmd::kGamemasters);

    base::ByteBuf out;
    r.encode_tree(cmd::kOwners, out);
    proto::Reader rd(out.data, out.len);
    EXPECT_EQ(rd.varint(), static_cast<u32>(proto::ids::play::cb::kCommands));
    // Breadth first from the root: the root, its six children, then theirs...
    ASSERT_EQ(rd.varint(), 17u);
    const auto node = [&](u8 flags, std::vector<u32> children) {
        EXPECT_EQ(rd.u8v(), flags);
        ASSERT_EQ(rd.varint(), children.size());
        for (const u32 c : children) EXPECT_EQ(rd.varint(), c);
    };
    constexpr u8 kLiteral = 1, kArgument = 2, kRuns = 4, kRedirect = 8, kSuggests = 16, kRestricted = 32;
    node(0, {1, 2, 3, 4, 5, 6});        // 0: the root
    node(kLiteral | kRestricted, {7});  // 1
    EXPECT_EQ(rd.str(99), "give");
    node(kLiteral | kRuns, {8});  // 2
    EXPECT_EQ(rd.str(99), "list");
    node(kLiteral | kRestricted, {9});  // 3
    EXPECT_EQ(rd.str(99), "playsound");
    node(kLiteral | kRestricted, {10});  // 4
    EXPECT_EQ(rd.str(99), "summon");
    node(kLiteral, {11});  // 5
    EXPECT_EQ(rd.str(99), "say");
    node(kLiteral | kRedirect | kRestricted, {});  // 6
    EXPECT_EQ(rd.varint(), 1u);                    // ...to "give"
    EXPECT_EQ(rd.str(99), "g");
    node(kArgument, {12});  // 7
    EXPECT_EQ(rd.str(99), "targets");
    EXPECT_EQ(rd.varint(), argument::kEntity);
    EXPECT_EQ(rd.u8v(), 2);      // players only, any number
    node(kLiteral | kRuns, {});  // 8
    EXPECT_EQ(rd.str(99), "uuids");
    node(kArgument | kRuns | kSuggests, {13});  // 9
    EXPECT_EQ(rd.str(99), "sound");
    EXPECT_EQ(rd.varint(), argument::kResourceLocation);
    EXPECT_EQ(rd.str(99), "minecraft:available_sounds");
    node(kArgument | kRuns | kSuggests, {14});  // 10
    EXPECT_EQ(rd.str(99), "entity");
    EXPECT_EQ(rd.varint(), argument::kResource);
    EXPECT_EQ(rd.str(99), "minecraft:entity_type");
    EXPECT_EQ(rd.str(99), "minecraft:summonable_entities");
    node(kArgument, {15});  // 11
    EXPECT_EQ(rd.str(99), "name");
    EXPECT_EQ(rd.varint(), argument::kString);
    EXPECT_EQ(rd.varint(), 0u);     // a single word
    node(kArgument | kRuns, {16});  // 12
    EXPECT_EQ(rd.str(99), "item");
    EXPECT_EQ(rd.varint(), argument::kItemStack);
    node(kArgument | kRuns, {});  // 13
    EXPECT_EQ(rd.str(99), "volume");
    EXPECT_EQ(rd.varint(), argument::kFloat);
    EXPECT_EQ(rd.u8v(), 3);  // a minimum and a maximum
    EXPECT_EQ(rd.f32(), 0.0f);
    EXPECT_EQ(rd.f32(), 2.0f);
    node(kArgument | kRuns, {});  // 14
    EXPECT_EQ(rd.str(99), "at");
    EXPECT_EQ(rd.varint(), argument::kVec3);
    node(kArgument | kRuns, {});  // 15
    EXPECT_EQ(rd.str(99), "text");
    EXPECT_EQ(rd.varint(), argument::kString);
    EXPECT_EQ(rd.varint(), 2u);   // greedy
    node(kArgument | kRuns, {});  // 16
    EXPECT_EQ(rd.str(99), "count");
    EXPECT_EQ(rd.varint(), argument::kInteger);
    EXPECT_EQ(rd.u8v(), 1);  // a minimum only
    EXPECT_EQ(rd.be32(), 1u);
    EXPECT_EQ(rd.varint(), 0u);  // the root's index
    EXPECT_TRUE(rd.done());

    // Someone without permissions is sent what is left: the root, two commands, their nodes.
    out.clear();
    r.encode_tree(cmd::kAll, out);
    proto::Reader few(out.data, out.len);
    few.varint();
    EXPECT_EQ(few.varint(), 6u);
}

TEST(CommandPackets, Layouts) {
    base::ByteBuf buf;
    const proto::cb::SoundByName sound{"minecraft:block.note_block.harp", 4, 1.5, -2.25, 3.0, 0.5f, 2.0f, 0x1122334455667788ull};
    u8* end = sound.encode(buf.reserve(sound.bound()));
    ASSERT_LE(static_cast<usize>(end - buf.data), sound.bound());
    proto::Reader r(buf.data, static_cast<usize>(end - buf.data));
    EXPECT_EQ(r.varint(), 0u);  // not a registry entry: the event follows
    EXPECT_EQ(r.str(99), "minecraft:block.note_block.harp");
    EXPECT_EQ(r.u8v(), 0);  // no fixed range
    EXPECT_EQ(r.varint(), 4u);
    EXPECT_EQ(static_cast<i32>(r.be32()), 12);
    EXPECT_EQ(static_cast<i32>(r.be32()), -18);
    EXPECT_EQ(static_cast<i32>(r.be32()), 24);
    EXPECT_EQ(r.f32(), 0.5f);
    EXPECT_EQ(r.f32(), 2.0f);
    EXPECT_EQ(r.be64(), 0x1122334455667788ull);
    EXPECT_TRUE(r.done());

    const auto stop = [&](proto::cb::StopSound p) {
        end = p.encode(buf.reserve(p.bound()));
        EXPECT_LE(static_cast<usize>(end - buf.data), p.bound());
        return std::vector<u8>(buf.data, end);
    };
    EXPECT_EQ(stop({}), (std::vector<u8>{0}));
    EXPECT_EQ(stop({7, {}}), (std::vector<u8>{1, 7}));
    EXPECT_EQ(stop({proto::cb::StopSound::kAnySource, "a:b"}), (std::vector<u8>{2, 3, 'a', ':', 'b'}));
    EXPECT_EQ(stop({2, "a:b"}), (std::vector<u8>{3, 2, 3, 'a', ':', 'b'}));

    const u8 component[] = {8, 0, 2, 'h', 'i'};
    const proto::cb::SystemChatComponent chat{component, false};
    end = chat.encode(buf.reserve(chat.bound()));
    EXPECT_EQ(std::vector<u8>(buf.data, end), (std::vector<u8>{8, 0, 2, 'h', 'i', 0}));

    // Serverbound: the plain command, and the signed one with everything behind it read past.
    const u8 plain[] = {4, 'l', 'i', 's', 't'};
    proto::Reader a(plain, sizeof plain);
    EXPECT_EQ(proto::sb::ChatCommand::decode(a).command, "list");
    EXPECT_TRUE(a.done());
    std::vector<u8> sig{4, 'k', 'i', 'c', 'k'};
    sig.insert(sig.end(), 16, 0);  // timestamp, salt
    sig.push_back(1);              // one signed argument:
    sig.insert(sig.end(), {6, 'r', 'e', 'a', 's', 'o', 'n'});
    sig.insert(sig.end(), 256, 0xAB);        // ...its signature
    sig.insert(sig.end(), {0, 0, 0, 0, 0});  // last seen: offset, 20 bits, checksum
    sig[sig.size() - 5] = 0;                 // (the offset)
    sig[sig.size() - 4] = 0x02;
    sig[sig.size() - 1] = 0x21;
    proto::Reader b(sig.data(), sig.size());
    const proto::sb::ChatCommand kick = proto::sb::ChatCommand::decode_signed(b);
    EXPECT_TRUE(b.done());
    EXPECT_EQ(kick.command, "kick");
    EXPECT_TRUE(kick.is_signed);
    ASSERT_EQ(kick.signatures, 1u);
    EXPECT_EQ(kick.signed_names[0], "reason");
    EXPECT_EQ(kick.last_seen.offset, 0u);
    EXPECT_EQ(kick.last_seen.acknowledged, 2u);
    EXPECT_EQ(kick.last_seen.checksum, 0x21);
    EXPECT_FALSE(proto::sb::ChatCommand::decode(a).is_signed);
    sig[21] = 9;  // more signatures than a command has arguments
    proto::Reader c(sig.data(), sig.size());
    proto::sb::ChatCommand::decode_signed(c);
    EXPECT_FALSE(c.ok);
}

// ---------------------------------------------------------------------------- what a command says

TEST(CommandText, EncodedAsVanillaWritesIt) {
    // {translate:"commands.setblock.success", with:[1, 2, 3]}: "with" lies in an earlier bucket.
    EXPECT_EQ(encoded(Text::translatable("commands.setblock.success").arg(1).arg(2).arg(3)),
              root(10, compound({l("with", 3, {be32(1), be32(2), be32(3)}), s("translate", "commands.setblock.success")})));
    // Plain text is a string; so is an argument that is plain text.
    EXPECT_EQ(encoded(Text::literal("/list [uuids]")), root(8, str("/list [uuids]")));
    EXPECT_EQ(encoded(Text::translatable("commands.op.success").arg("Steve")),
              root(10, compound({l("with", 8, {str("Steve")}), s("translate", "commands.op.success")})));
    // Numbers of several kinds beside a component: each in its own {"": x}.
    EXPECT_EQ(encoded(Text::translatable("commands.fill.toobig").arg(32768).arg_long(40000)),
              root(10, compound({l("with", 10, {compound({i("", 32768)}), compound({entry(4, "", be64(40000))})}),
                                 s("translate", "commands.fill.toobig")})));
    // CommandSourceStack.sendFailure: an empty text, the message its child, red.
    EXPECT_EQ(encoded(Text().append(Text::translatable("commands.setblock.failed")).color(Color::kRed)),
              root(10, compound({s("color", "red"), l("extra", 10, {compound({s("translate", "commands.setblock.failed")})}),
                                 s("text", "")})));
    // A player's name: what a click suggests, what pointing at it shows, what shift-click inserts.
    const Uuid id{0x0123456789abcdefull, 0xfedcba9876543210ull};
    const Bytes hover = c(
        "hover_event", {s("name", "Steve"), s("action", "show_entity"), s("id", "minecraft:player"),
                        entry(11, "uuid", be32(4) + be32(0x01234567) + be32(0x89abcdef) + be32(0xfedcba98) + be32(0x76543210))});
    EXPECT_EQ(encoded(Text::literal("Steve")
                          .suggest_command("/tell Steve ")
                          .hover_entity("minecraft:player", id, Text::literal("Steve"))
                          .insertion("Steve")),
              root(10, compound({c("click_event", {s("action", "suggest_command"), s("command", "/tell Steve ")}),
                                 s("insertion", "Steve"), s("text", "Steve"), hover})));
    // A stack: "[Stone]", white, showing one stone.
    EXPECT_EQ(
        encoded(Text::translatable("chat.square_brackets")
                    .arg(Text().append(Text::translatable("block.minecraft.stone")))
                    .color(Color::kWhite)
                    .hover_item("minecraft:stone", 1)),
        root(10, compound({l("with", 10,
                             {compound({l("extra", 10, {compound({s("translate", "block.minecraft.stone")})}), s("text", "")})}),
                           s("color", "white"), c("hover_event", {s("action", "show_item"), s("id", "minecraft:stone")}),
                           s("translate", "chat.square_brackets")})));
    EXPECT_EQ(encoded(Text::literal("x").hover_item("minecraft:stone", 64)),
              root(10, compound({s("text", "x"),
                                 c("hover_event", {i("count", 64), s("action", "show_item"), s("id", "minecraft:stone")})})));
}

TEST(CommandText, PlainIsWhatTheConsolePrints) {
    EXPECT_EQ(Text::translatable("commands.setblock.success").arg(1).arg(-2).arg(3).plain(), "Changed the block at 1, -2, 3");
    EXPECT_EQ(Text::translatable("commands.give.success.single")
                  .arg(3)
                  .arg(Text::translatable("chat.square_brackets").arg(Text::translatable("item.minecraft.diamond")))
                  .arg(Text::literal("Steve"))
                  .plain(),
              "Gave 3 [Diamond] to Steve");
    EXPECT_EQ(Text::translatable("chat.type.admin").arg("Steve").arg(Text::translatable("commands.stop.stopping")).plain(),
              "[Steve: Stopping the server]");
    EXPECT_EQ(Text::translatable("no.such.key").arg(1).plain(), "no.such.key");
    EXPECT_EQ(Text().append("a").append(Text::literal("b").append("c")).plain(), "abc");
    // MutableComponent.append(String): an empty string adds nothing - no empty child on the wire.
    EXPECT_EQ(encoded(Text().append("").append("x")), root(10, compound({l("extra", 8, {str("x")}), s("text", "")})));
    EXPECT_EQ(Text::translatable("argument.float.big").arg(2.0f).arg(3.5f).plain(), "Float must not be more than 2.0: found 3.5");
    EXPECT_EQ(text::lang("multiplayer.disconnect.kicked"), "Kicked by an operator");

    // String.format("%f"): six decimals, rounded half up from the shortest decimal form.
    EXPECT_EQ(text::java_format_double(0.5), "0.500000");
    EXPECT_EQ(text::java_format_double(-60.0), "-60.000000");
    EXPECT_EQ(text::java_format_double(8.5), "8.500000");
    EXPECT_EQ(text::java_format_double(1.0000005), "1.000001");  // printf would say 1.000000: the double is just below
    EXPECT_EQ(text::java_format_double(0.0078125), "0.007813");
    EXPECT_EQ(text::java_format_double(0.9999999), "1.000000");
    EXPECT_EQ(text::java_format_double(123456789.125), "123456789.125000");
    EXPECT_EQ(text::uuid_string({0x0123456789abcdefull, 0xfedcba9876543210ull}), "01234567-89ab-cdef-fedc-ba9876543210");
}
