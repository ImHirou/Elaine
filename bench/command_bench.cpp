// Command benchmarks: what a typed line costs before its handler runs, and what its answer
// costs to put on the wire. None of it is a hot path - a line is typed by a person, and ten in
// a row disconnect - the numbers say that it may allocate as it does.
#include <string_view>

#include "bench.hpp"

#include "server/command.hpp"
#include "text/text.hpp"

namespace benchmark {

namespace {

i32 nothing(server::cmd::Invocation&) { return 1; }

}  // namespace

void command_benchmarks() {
    using namespace server;
    using text::Text;
    cmd::Registry r;
    r.add("fill <from:block_pos> <to:block_pos> <block:block_state> [(outline|hollow|destroy|strict)]", cmd::kGamemasters,
          &nothing);
    r.add("fill <from:block_pos> <to:block_pos> <block:block_state> replace [<filter:block_predicate>]", cmd::kGamemasters,
          &nothing);
    r.add("give <targets:players> <item:item> [<count:int(1)>]", cmd::kGamemasters, &nothing);
    r.add("setblock <pos:block_pos> <block:block_state> [(destroy|keep|replace|strict)]", cmd::kGamemasters, &nothing);
    r.add("teleport <location:vec3>", cmd::kGamemasters, &nothing);
    r.add("teleport <destination:entity>", cmd::kGamemasters, &nothing);
    r.add("teleport <targets:entities> <location:vec3> [<rotation:rotation>]", cmd::kGamemasters, &nothing);
    r.add("teleport <targets:entities> <destination:entity>", cmd::kGamemasters, &nothing);
    r.alias("tp", "teleport", cmd::kGamemasters);

    cmd::Parsed parsed;
    const auto parse = [&](const char* name, std::string_view line) {
        bench(name, 1, "M lines/s", [&] {
            r.parse(line, cmd::kOwners, parsed);
            g_sink += parsed.steps.size();
        });
    };
    parse("command parse: setblock", "setblock ~ ~1 ~ minecraft:chest[facing=east,type=left] replace");
    parse("command parse: tp with a selector", "tp @e[type=!player,distance=..16,sort=nearest,limit=3] ~ ~10 ~ 90 0");
    parse("command parse: fill with a tag filter", "fill 0 -60 0 15 -50 15 stone replace #minecraft:logs[axis=y]");
    parse("command parse: a syntax error", "give Steve minecraft:nonsense 3");

    base::ByteBuf out;
    const Uuid id{0x0123456789abcdefull, 0xfedcba9876543210ull};
    bench("command answer: built and encoded", 1, "M texts/s", [&] {
        const Text who = Text::literal("Steve")
                             .suggest_command("/tell Steve ")
                             .hover_entity("minecraft:player", id, Text::literal("Steve"))
                             .insertion("Steve");
        const Text item = Text::translatable("chat.square_brackets")
                              .arg(Text().append(Text::translatable("item.minecraft.diamond")))
                              .color(text::Color::kWhite)
                              .hover_item("minecraft:diamond", 1);
        out.clear();
        Text::translatable("commands.give.success.single").arg(3).arg(item).arg(who).encode(out);
        g_sink += out.len;
    });
    base::ByteBuf tree;
    bench("command tree: encoded for a client", 1, "M trees/s", [&] {
        tree.clear();
        r.encode_tree(cmd::kOwners, tree);
        g_sink += tree.len;
    });
}

}  // namespace benchmark
