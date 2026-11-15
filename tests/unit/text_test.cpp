// Text components: every spelling of a text the codec takes, the one spelling vanilla writes
// back, and what is refused. Input and expected output are NBT written out by hand. The cases
// are those of tools/vanilla/component_probe.py and a few variations of them: for the probe's
// a vanilla 26.3 server sent exactly these bytes, entry order included.
#include <gtest/gtest.h>

#include <random>
#include <string>
#include <vector>

#include "nbt/nbt.hpp"
#include "nbt_bytes.hpp"
#include "text/component.hpp"

namespace {

using namespace nbt_bytes;
using text::Result;

struct Out {
    Result result;
    Bytes tag;  // kOk: the tag written, type byte first
    bool operator==(const Out&) const = default;
};
std::ostream& operator<<(std::ostream& os, const Out& o) {
    os << (o.result == Result::kOk ? "ok" : o.result == Result::kInvalid ? "invalid" : "unsupported") << " [";
    for (const u8 byte : o.tag) os << std::hex << (byte >> 4) << (byte & 15);
    return os << "]" << std::dec;
}

Out transcode(const Bytes& in) {
    nbt::Doc doc;
    EXPECT_EQ(doc.parse(in), nbt::Error::kNone);
    base::ByteBuf out;
    const Result r = text::transcode(doc.root(), out);
    return {r, r == Result::kOk ? Bytes(out.data, out.data + out.len) : Bytes{}};
}
Out ok(const Bytes& tag) { return {Result::kOk, tag}; }
const Out kInvalid{Result::kInvalid, {}};
const Out kUnsupported{Result::kUnsupported, {}};

Bytes plain(std::string_view s) { return root(8, str(s)); }                                 // "s"
Bytes record(std::initializer_list<Bytes> entries) { return root(10, compound(entries)); }  // {...}
Bytes f(std::string_view name, float v) { return entry(5, name, be32(std::bit_cast<u32>(v))); }
Bytes d(std::string_view name, double v) { return entry(6, name, be64(std::bit_cast<u64>(v))); }
// A string as an element of a list of compounds: {"": s}.
Bytes wrapped(std::string_view v) { return compound({s("", v)}); }

}  // namespace

TEST(TextComponent, PlainTextIsAString) {
    EXPECT_EQ(transcode(plain("hi")), ok(plain("hi")));
    EXPECT_EQ(transcode(plain("")), ok(plain("")));
    EXPECT_EQ(transcode(plain("\xD0\x9C\xD0\xB5\xD1\x87")), ok(plain("\xD0\x9C\xD0\xB5\xD1\x87")));
    // The same text spelled as a record, with a type, with keys the codec does not know, twice.
    EXPECT_EQ(transcode(record({s("text", "hi")})), ok(plain("hi")));
    EXPECT_EQ(transcode(record({s("type", "text"), s("text", "hi")})), ok(plain("hi")));
    EXPECT_EQ(transcode(record({s("text", "hi"), s("unknown", "x"), i("another", 5)})), ok(plain("hi")));
    EXPECT_EQ(transcode(record({s("text", "first"), s("text", "hi")})), ok(plain("hi")));
    EXPECT_EQ(transcode(record({s("text", "")})), ok(plain("")));
}

TEST(TextComponent, StyleMakesItARecordInHashMapOrder) {
    // color lands in bucket 4, text in 11.
    EXPECT_EQ(transcode(record({s("text", "hi"), s("color", "red")})), ok(record({s("color", "red"), s("text", "hi")})));
    EXPECT_EQ(transcode(record({s("color", "red"), s("text", "hi")})), ok(record({s("color", "red"), s("text", "hi")})));
    // text and bold share a bucket: text was put first. italic is two buckets on.
    EXPECT_EQ(transcode(record({b("italic", 0), b("bold", 1), s("text", "hi")})),
              ok(record({s("text", "hi"), b("bold", 1), b("italic", 0)})));
    // false is a value: the style is not empty.
    EXPECT_EQ(transcode(record({s("text", "hi"), b("italic", 0)})), ok(record({s("text", "hi"), b("italic", 0)})));
    // An empty text with a style stays a record.
    EXPECT_EQ(transcode(record({s("text", ""), b("bold", 1)})), ok(record({s("text", ""), b("bold", 1)})));
    // Every field at once: thirteen entries, so the map has grown to 32 buckets.
    const Bytes all = record({
        s("text", "t"),
        l("extra", 8, {str("e")}),
        s("color", "gold"),
        i("shadow_color", 0x80112233),
        b("bold", 1),
        b("italic", 1),
        b("underlined", 1),
        b("strikethrough", 1),
        b("obfuscated", 1),
        c("click_event", {s("action", "copy_to_clipboard"), s("value", "v")}),
        c("hover_event", {s("action", "show_text"), s("value", "h")}),
        s("insertion", "ins"),
        s("font", "minecraft:alt"),
    });
    EXPECT_EQ(transcode(all), ok(record({
                                  s("color", "gold"),                                                     // 4
                                  b("bold", 1),                                                           // 11
                                  b("italic", 1),                                                         // 13
                                  c("click_event", {s("action", "copy_to_clipboard"), s("value", "v")}),  // 19
                                  i("shadow_color", 0x80112233),                                          // 20
                                  l("extra", 8, {str("e")}),                                              // 23
                                  b("underlined", 1),                                                     // 23
                                  s("insertion", "ins"),                                                  // 26
                                  s("text", "t"),                                                         // 27
                                  b("strikethrough", 1),                                                  // 27
                                  c("hover_event", {s("action", "show_text"), s("value", "h")}),          // 28
                                  b("obfuscated", 1),                                                     // 29
                                  s("font", "minecraft:alt"),                                             // 31
                              })));
    // Twelve: still 16 buckets.
    const Bytes twelve = record({
        s("text", "t"),
        s("color", "gold"),
        i("shadow_color", 1),
        b("bold", 1),
        b("italic", 1),
        b("underlined", 1),
        b("strikethrough", 1),
        b("obfuscated", 1),
        c("click_event", {s("action", "copy_to_clipboard"), s("value", "v")}),
        c("hover_event", {s("action", "show_text"), s("value", "h")}),
        s("insertion", "ins"),
        s("font", "minecraft:alt"),
    });
    EXPECT_EQ(transcode(twelve), ok(record({
                                     c("click_event", {s("action", "copy_to_clipboard"), s("value", "v")}),  // 3
                                     s("color", "gold"),                                                     // 4
                                     i("shadow_color", 1),                                                   // 4
                                     b("underlined", 1),                                                     // 7
                                     s("insertion", "ins"),                                                  // 10
                                     s("text", "t"),                                                         // 11
                                     b("bold", 1),                                                           // 11
                                     b("strikethrough", 1),                                                  // 11
                                     c("hover_event", {s("action", "show_text"), s("value", "h")}),          // 12
                                     b("italic", 1),                                                         // 13
                                     b("obfuscated", 1),                                                     // 13
                                     s("font", "minecraft:alt"),                                             // 15
                                 })));
}

TEST(TextComponent, ListsAreTheFirstWithTheRestAsChildren) {
    EXPECT_EQ(transcode(root(9, list(8, {str("a")}))), ok(plain("a")));
    EXPECT_EQ(transcode(root(9, list(9, {list(9, {list(8, {str("a")})})}))), ok(plain("a")));
    EXPECT_EQ(transcode(root(9, list(8, {str("a"), str("b"), str("c")}))),
              ok(record({l("extra", 8, {str("b"), str("c")}), s("text", "a")})));
    // The first element keeps its own children, the others follow them.
    EXPECT_EQ(transcode(root(9, list(10, {compound({s("text", "a"), l("extra", 8, {str("x")})}), wrapped("b")}))),
              ok(record({l("extra", 8, {str("x"), str("b")}), s("text", "a")})));
    // A list in a list: the inner one is one component.
    EXPECT_EQ(
        transcode(root(9, list(9, {list(8, {str("a"), str("b")}), list(8, {str("c"), str("d")})}))),
        ok(record({l("extra", 10, {wrapped("b"), compound({l("extra", 8, {str("d")}), s("text", "c")})}), s("text", "a")})));
    // Children of mixed shape travel as compounds, the strings among them wrapped.
    EXPECT_EQ(transcode(record({s("text", "a"), l("extra", 10, {wrapped("b"), compound({s("text", "c"), b("bold", 1)})})})),
              ok(record({l("extra", 10, {wrapped("b"), compound({s("text", "c"), b("bold", 1)})}), s("text", "a")})));
    // ...and are strings when all of them are plain, however they were spelled.
    EXPECT_EQ(transcode(record({s("text", "a"), l("extra", 10, {compound({s("text", "b")}), compound({s("text", "c")})})})),
              ok(record({l("extra", 8, {str("b"), str("c")}), s("text", "a")})));
    EXPECT_EQ(transcode(root(9, list(8, {}))), kInvalid);
    EXPECT_EQ(transcode(root(9, list(3, {be32(1)}))), kInvalid);
    EXPECT_EQ(transcode(record({s("text", "a"), l("extra", 0, {})})), kInvalid);  // "List must have contents"
    EXPECT_EQ(transcode(record({s("text", "a"), s("extra", "b")})), kInvalid);
    EXPECT_EQ(transcode(record({s("text", "a"), l("extra", 8, {str("b")}), l("extra", 3, {be32(1)})})), kInvalid);
}

TEST(TextComponent, ContentsByTypeOrByTheFieldsThatAreThere) {
    EXPECT_EQ(transcode(record({s("keybind", "key.jump")})), ok(record({s("keybind", "key.jump")})));
    EXPECT_EQ(transcode(record({s("type", "keybind"), s("keybind", "key.jump")})), ok(record({s("keybind", "key.jump")})));
    EXPECT_EQ(transcode(record({s("translate", "block.minecraft.stone")})),
              ok(record({s("translate", "block.minecraft.stone")})));
    // Without a type the first that fits wins: text, then translatable, then keybind.
    EXPECT_EQ(transcode(record({s("keybind", "k"), s("translate", "t"), s("text", "x")})), ok(plain("x")));
    EXPECT_EQ(transcode(record({s("keybind", "k"), s("translate", "t"), i("text", 5)})), ok(record({s("translate", "t")})));
    EXPECT_EQ(transcode(record({s("keybind", "k"), i("translate", 5)})), ok(record({s("keybind", "k")})));
    // A translatable whose arguments cannot be read is no translatable: the keybind gets its turn.
    EXPECT_EQ(transcode(record({s("keybind", "k"), s("translate", "t"), s("with", "not a list")})),
              ok(record({s("keybind", "k")})));
    EXPECT_EQ(transcode(record({s("keybind", "k"), s("translate", "t"), l("with", 10, {compound({})})})),
              ok(record({s("keybind", "k")})));
    EXPECT_EQ(transcode(record({s("translate", "t"), s("with", "not a list")})), kInvalid);
    // With a type only that type is tried.
    EXPECT_EQ(transcode(record({s("type", "translatable"), s("text", "x")})), kInvalid);
    EXPECT_EQ(transcode(record({s("type", "text"), s("translate", "t")})), kInvalid);
    EXPECT_EQ(transcode(record({s("type", "translatable"), s("translate", "t"), s("text", "x")})),
              ok(record({s("translate", "t")})));
    EXPECT_EQ(transcode(record({s("type", "nope"), s("text", "x")})), kInvalid);
    EXPECT_EQ(transcode(record({i("type", 1), s("text", "x")})), kInvalid);
    EXPECT_EQ(transcode(record({})), kInvalid);
    EXPECT_EQ(transcode(record({b("bold", 1)})), kInvalid);
    EXPECT_EQ(transcode(record({s("", "x")})), kInvalid);  // a wrapper is only one inside a list
}

TEST(TextComponent, TranslatableArguments) {
    // Numbers and strings as they are, components as components; one that is plain text is a string again.
    const Bytes in = record({
        s("fallback", "fb"),
        s("translate", "key"),
        l("with", 10,
          {wrapped("s"), compound({i("", 7)}), compound({f("", 2.5f)}), compound({s("text", "x"), b("bold", 1)}),
           compound({l("", 8, {str("y")})}), compound({s("text", "z")})}),
    });
    EXPECT_EQ(transcode(in), ok(record({
                                 l("with", 10,
                                   {wrapped("s"), compound({i("", 7)}), compound({f("", 2.5f)}),
                                    compound({s("text", "x"), b("bold", 1)}), wrapped("y"), wrapped("z")}),
                                 s("fallback", "fb"),
                                 s("translate", "key"),
                             })));
    // All of one type: a list of that type; an array is a list of its numbers.
    EXPECT_EQ(transcode(record({s("translate", "k"), l("with", 3, {be32(1), be32(2)})})),
              ok(record({l("with", 3, {be32(1), be32(2)}), s("translate", "k")})));
    EXPECT_EQ(transcode(record({s("translate", "k"), entry(11, "with", be32(2) + be32(1) + be32(2))})),
              ok(record({l("with", 3, {be32(1), be32(2)}), s("translate", "k")})));
    EXPECT_EQ(transcode(record({s("translate", "k"), entry(7, "with", be32(2) + Bytes{1, 2})})),
              ok(record({l("with", 1, {{1}, {2}}), s("translate", "k")})));
    EXPECT_EQ(transcode(record({s("translate", "k"), l("with", 8, {str("a"), str("b")})})),
              ok(record({l("with", 8, {str("a"), str("b")}), s("translate", "k")})));
    EXPECT_EQ(transcode(record({s("translate", "k"), l("with", 10, {compound({s("text", "a"), b("bold", 0)})})})),
              ok(record({l("with", 10, {compound({s("text", "a"), b("bold", 0)})}), s("translate", "k")})));
    // No arguments: no "with".
    EXPECT_EQ(transcode(record({s("translate", "k"), l("with", 0, {})})), ok(record({s("translate", "k")})));
    EXPECT_EQ(transcode(record({s("translate", "k"), entry(12, "with", be32(0))})), ok(record({s("translate", "k")})));
    // A fallback that is no string is no fallback (lenientOptionalFieldOf).
    EXPECT_EQ(transcode(record({s("translate", "k"), i("fallback", 5)})), ok(record({s("translate", "k")})));
    // An array is not an argument, and neither is a component that is none.
    EXPECT_EQ(transcode(record({s("translate", "k"), l("with", 11, {be32(0)})})), kInvalid);
    EXPECT_EQ(transcode(record({s("translate", "k"), l("with", 10, {compound({i("x", 1)})})})), kInvalid);
    // Style and children go with it.
    EXPECT_EQ(transcode(record({s("translate", "k"), s("color", "aqua"), l("extra", 8, {str("e")})})),
              ok(record({s("color", "aqua"), l("extra", 8, {str("e")}), s("translate", "k")})));
}

TEST(TextComponent, Colours) {
    const auto colour = [](std::string_view in) { return transcode(record({s("text", "t"), s("color", in)})); };
    const auto kept = [](std::string_view out) { return ok(record({s("color", out), s("text", "t")})); };
    for (const char* name : {"black", "dark_blue", "dark_green", "dark_aqua", "dark_red", "dark_purple", "gold", "gray",
                             "dark_gray", "blue", "green", "aqua", "red", "light_purple", "yellow", "white"})
        EXPECT_EQ(colour(name), kept(name)) << name;
    EXPECT_EQ(colour("#FF5555"), kept("#FF5555"));  // not turned into "red"
    EXPECT_EQ(colour("#ff5555"), kept("#FF5555"));
    EXPECT_EQ(colour("#f"), kept("#00000F"));
    EXPECT_EQ(colour("#000000000abc"), kept("#000ABC"));
    EXPECT_EQ(colour("#+ff"), kept("#0000FF"));  // Integer.parseInt takes a sign
    EXPECT_EQ(colour("#-0"), kept("#000000"));
    EXPECT_EQ(colour("#FFFFFF"), kept("#FFFFFF"));
    EXPECT_EQ(colour("#1000000"), kInvalid);
    EXPECT_EQ(colour("#-1"), kInvalid);
    EXPECT_EQ(colour("#"), kInvalid);
    EXPECT_EQ(colour("#+"), kInvalid);
    EXPECT_EQ(colour("#12345G"), kInvalid);
    EXPECT_EQ(colour("#FFFFFFFFFFFFFFFFFFFF"), kInvalid);
    EXPECT_EQ(colour(""), kInvalid);
    EXPECT_EQ(colour("Red"), kInvalid);
    EXPECT_EQ(colour("reset"), kInvalid);
    EXPECT_EQ(colour("#\xEF\xBC\x91"), kUnsupported);  // a full-width digit one: Character.digit knows it
    EXPECT_EQ(transcode(record({s("text", "t"), i("color", 0xFF5555)})), kInvalid);
}

TEST(TextComponent, FlagsAndShadowColour) {
    const auto flag = [](const Bytes& in) { return transcode(record({s("text", "t"), in})); };
    const auto kept = [](u8 v) { return ok(record({s("text", "t"), b("bold", v)})); };
    // Codec.BOOL of a tag: any number, true unless it is zero.
    EXPECT_EQ(flag(b("bold", 1)), kept(1));
    EXPECT_EQ(flag(b("bold", 0)), kept(0));
    EXPECT_EQ(flag(b("bold", 0x80)), kept(1));
    EXPECT_EQ(flag(i("bold", 256)), kept(1));
    EXPECT_EQ(flag(entry(4, "bold", be64(0x100000000ull))), kept(1));
    EXPECT_EQ(flag(f("bold", 0.25f)), kept(1));
    EXPECT_EQ(flag(f("bold", -0.0f)), kept(0));
    EXPECT_EQ(flag(d("bold", 0.0)), kept(0));
    EXPECT_EQ(flag(entry(5, "bold", be32(0x7FC00000))), kept(1));  // NaN is not zero
    EXPECT_EQ(flag(s("bold", "true")), kInvalid);
    EXPECT_EQ(flag(l("bold", 1, {{1}})), kInvalid);

    const auto shadow = [](const Bytes& in) { return transcode(record({s("text", "t"), in})); };
    const auto shade = [](u32 v) { return ok(record({i("shadow_color", v), s("text", "t")})); };
    EXPECT_EQ(shadow(i("shadow_color", 0xFF000000)), shade(0xFF000000));
    EXPECT_EQ(shadow(b("shadow_color", 0xFF)), shade(0xFFFFFFFF));  // Number.intValue
    EXPECT_EQ(shadow(entry(4, "shadow_color", be64(0x100000002ull))), shade(2));
    EXPECT_EQ(shadow(f("shadow_color", 2.9f)), shade(2));
    EXPECT_EQ(shadow(d("shadow_color", -1e300)), shade(0x80000000));
    EXPECT_EQ(shadow(s("shadow_color", "#FFFFFF")), kInvalid);
    EXPECT_EQ(shadow(l("shadow_color", 5, {be32(0), be32(0), be32(0), be32(0)})), kUnsupported);  // four floats: not read
}

TEST(TextComponent, InsertionAndFont) {
    EXPECT_EQ(transcode(record({s("text", "t"), s("insertion", "typed")})),
              ok(record({s("insertion", "typed"), s("text", "t")})));
    EXPECT_EQ(transcode(record({s("text", "t"), i("insertion", 1)})), kInvalid);
    // An Identifier: written back in full.
    EXPECT_EQ(transcode(record({s("text", "t"), s("font", "uniform")})),
              ok(record({s("text", "t"), s("font", "minecraft:uniform")})));
    EXPECT_EQ(transcode(record({s("text", "t"), s("font", "my:fonts/big")})),
              ok(record({s("text", "t"), s("font", "my:fonts/big")})));
    EXPECT_EQ(transcode(record({s("text", "t"), s("font", "Uniform")})), kInvalid);
    EXPECT_EQ(transcode(record({s("text", "t"), b("font", 1)})), kInvalid);
}

TEST(TextComponent, ClickEvents) {
    const auto click = [](std::initializer_list<Bytes> event) {
        return transcode(record({s("text", "t"), c("click_event", event)}));
    };
    const auto kept = [](std::initializer_list<Bytes> event) { return ok(record({c("click_event", event), s("text", "t")})); };
    EXPECT_EQ(click({s("url", "https://example.com/a-b_c/d.html?x=1&y=%20#top"), s("action", "open_url")}),
              kept({s("action", "open_url"), s("url", "https://example.com/a-b_c/d.html?x=1&y=%20#top")}));
    EXPECT_EQ(click({s("action", "open_url"), s("url", "HTTP://127.0.0.1:8080")}),
              kept({s("action", "open_url"), s("url", "HTTP://127.0.0.1:8080")}));
    EXPECT_EQ(click({s("action", "run_command"), s("command", "/say hi")}),
              kept({s("action", "run_command"), s("command", "/say hi")}));
    EXPECT_EQ(click({s("action", "suggest_command"), s("command", "")}),
              kept({s("action", "suggest_command"), s("command", "")}));
    EXPECT_EQ(click({s("action", "copy_to_clipboard"), s("value", "\xC2\xA7x")}),
              kept({s("action", "copy_to_clipboard"), s("value", "\xC2\xA7x")}));
    EXPECT_EQ(click({s("action", "change_page"), i("page", 3)}), kept({s("action", "change_page"), i("page", 3)}));
    EXPECT_EQ(click({s("action", "change_page"), f("page", 2.9f)}), kept({s("action", "change_page"), i("page", 2)}));
    EXPECT_EQ(click({s("action", "change_page"), i("page", 3), s("url", "ignored")}),
              kept({s("action", "change_page"), i("page", 3)}));
    // Wrong for vanilla.
    EXPECT_EQ(click({s("action", "change_page"), i("page", 0)}), kInvalid);
    EXPECT_EQ(click({s("action", "change_page"), s("page", "1")}), kInvalid);
    EXPECT_EQ(click({s("action", "run_command"), s("command",
                                                   "a\xC2\xA7"
                                                   "b")}),
              kInvalid);  // a section sign
    EXPECT_EQ(click({s("action", "run_command"), s("command", "a\x7F")}), kInvalid);
    EXPECT_EQ(click({s("action", "run_command"), s("command", "a\nb")}), kInvalid);
    EXPECT_EQ(click({s("action", "run_command")}), kInvalid);
    EXPECT_EQ(click({s("action", "open_url"), i("url", 1)}), kInvalid);
    EXPECT_EQ(click({s("action", "open_file"), s("path", "/etc/passwd")}), kInvalid);  // not from a server
    EXPECT_EQ(click({s("action", "explode")}), kInvalid);
    EXPECT_EQ(click({s("url", "https://example.com")}), kInvalid);
    EXPECT_EQ(transcode(record({s("text", "t"), s("click_event", "open_url")})), kInvalid);
    // Not this server's to say.
    EXPECT_EQ(click({s("action", "open_url"), s("url", "https://example.com/a b")}), kUnsupported);
    EXPECT_EQ(click({s("action", "open_url"), s("url", "https://[::1]/")}), kUnsupported);
    EXPECT_EQ(click({s("action", "open_url"), s("url", "https://example.com/%zz")}), kUnsupported);
    EXPECT_EQ(click({s("action", "open_url"), s("url", "https://example.com/#a#b")}), kUnsupported);
    EXPECT_EQ(click({s("action", "open_url"), s("url", "ftp://example.com")}), kUnsupported);
    EXPECT_EQ(click({s("action", "open_url"), s("url", "https://")}), kUnsupported);
    EXPECT_EQ(click({s("action", "show_dialog"), s("dialog", "minecraft:x")}), kUnsupported);
    EXPECT_EQ(click({s("action", "custom"), s("id", "a:b")}), kUnsupported);
}

TEST(TextComponent, HoverEvents) {
    const auto hover = [](std::initializer_list<Bytes> event) {
        return transcode(record({s("text", "t"), c("hover_event", event)}));
    };
    const auto kept = [](std::initializer_list<Bytes> event) { return ok(record({s("text", "t"), c("hover_event", event)})); };
    EXPECT_EQ(hover({s("value", "tip"), s("action", "show_text")}), kept({s("action", "show_text"), s("value", "tip")}));
    EXPECT_EQ(hover({s("action", "show_text"), c("value", {s("text", "tip")})}),
              kept({s("action", "show_text"), s("value", "tip")}));
    EXPECT_EQ(hover({s("action", "show_text"), c("value", {s("text", "tip"), s("color", "green")})}),
              kept({s("action", "show_text"), c("value", {s("color", "green"), s("text", "tip")})}));
    EXPECT_EQ(hover({s("action", "show_text"), l("value", 8, {str("a"), str("b")})}),
              kept({s("action", "show_text"), c("value", {l("extra", 8, {str("b")}), s("text", "a")})}));
    EXPECT_EQ(hover({s("action", "show_text")}), kInvalid);
    EXPECT_EQ(hover({s("action", "show_text"), i("value", 1)}), kInvalid);
    EXPECT_EQ(hover({s("action", "show_nothing"), s("value", "x")}), kInvalid);
    EXPECT_EQ(hover({s("value", "x")}), kInvalid);
    EXPECT_EQ(hover({s("action", "show_item"), s("id", "minecraft:stone")}), kUnsupported);
    EXPECT_EQ(hover({s("action", "show_entity"), s("id", "minecraft:pig")}), kUnsupported);
}

TEST(TextComponent, WhatIsNoComponentAndWhatIsNotRead) {
    EXPECT_EQ(transcode(root(3, be32(5))), kInvalid);
    EXPECT_EQ(transcode(root(1, {1})), kInvalid);
    EXPECT_EQ(transcode(root(7, be32(0))), kInvalid);             // an empty array: an empty list
    EXPECT_EQ(transcode(root(11, be32(1) + be32(5))), kInvalid);  // a list of a number
    EXPECT_EQ(transcode(record({s("text", "a"), l("extra", 3, {be32(1)})})), kInvalid);
    // The contents this server does not read: unsupported - unless something it does read fits first.
    EXPECT_EQ(transcode(record({s("selector", "@a")})), kUnsupported);
    EXPECT_EQ(transcode(record({c("score", {s("name", "x"), s("objective", "y")})})), kUnsupported);
    EXPECT_EQ(transcode(record({s("nbt", "a.b"), s("entity", "@s")})), kUnsupported);
    EXPECT_EQ(transcode(record({s("object", "atlas"), s("sprite", "x")})), kUnsupported);
    EXPECT_EQ(transcode(record({s("sprite", "x")})), kUnsupported);
    EXPECT_EQ(transcode(record({c("player", {s("name", "x")})})), kUnsupported);
    EXPECT_EQ(transcode(record({s("type", "selector"), s("selector", "@a")})), kUnsupported);
    EXPECT_EQ(transcode(record({s("selector", "@a"), s("text", "a")})), ok(plain("a")));
    EXPECT_EQ(transcode(record({s("selector", "@a"), s("keybind", "k")})), ok(record({s("keybind", "k")})));
    // One unsupported part spoils the whole.
    EXPECT_EQ(transcode(root(9, list(10, {wrapped("a"), compound({s("selector", "@a")})}))), kUnsupported);
    // Deeper than anything real.
    Bytes deep = root(9, {});
    for (int n = 0; n < 40; ++n) deep = deep + Bytes{9} + be32(1);
    deep = deep + Bytes{8} + be32(1) + str("a");
    EXPECT_EQ(transcode(deep), kUnsupported);
    Bytes fine = root(9, {});
    for (int n = 0; n < 20; ++n) fine = fine + Bytes{9} + be32(1);
    fine = fine + Bytes{8} + be32(1) + str("a");
    EXPECT_EQ(transcode(fine), ok(plain("a")));
    // Strings Java would have written differently: compared as bytes here, so not taken.
    EXPECT_EQ(transcode(root(8, be16(2) + Bytes{'a', 0})), kUnsupported);
    EXPECT_EQ(transcode(record({entry(8, "text", be16(2) + Bytes{0xC1, 0x81})})), kUnsupported);
}

TEST(TextComponent, WhatIsWrittenReadsBackAsItself) {
    // A component with a bit of everything, damaged at random. Whatever the codec still takes
    // it writes in the one form it has: reading that form gives that form.
    const Bytes whole = record({
        s("translate", "key"),
        s("fallback", "fb"),
        l("with", 10,
          {wrapped("arg"), compound({i("", 7)}), compound({s("text", "x"), b("bold", 1)}), compound({l("", 8, {str("y")})})}),
        l("extra", 10,
          {wrapped("child"), compound({s("keybind", "key.jump"), s("color", "#12abEF")}),
           compound({l("", 8, {str("a"), str("b")})})}),
        s("color", "gold"),
        i("shadow_color", 0x80112233),
        b("bold", 1),
        b("italic", 0),
        b("underlined", 1),
        b("strikethrough", 0),
        b("obfuscated", 1),
        c("click_event", {s("action", "open_url"), s("url", "https://example.com/path?q=1")}),
        c("hover_event", {s("action", "show_text"), c("value", {s("text", "tip"), s("font", "alt")})}),
        s("insertion", "ins"),
        s("font", "minecraft:uniform"),
    });
    ASSERT_EQ(transcode(whole).result, Result::kOk);
    std::mt19937 rng(20261115);
    nbt::Doc doc;
    base::ByteBuf out;
    u32 written = 0;
    for (int round = 0; round < 30000; ++round) {
        Bytes in = whole;
        for (u32 edits = 1 + rng() % 3; edits > 0 && !in.empty(); --edits) {
            const usize at = rng() % in.size();
            switch (rng() % 4) {
                case 0: in[at] = static_cast<u8>(rng()); break;
                case 1: in[at] ^= static_cast<u8>(1u << (rng() % 8)); break;
                case 2: in.insert(in.begin() + static_cast<long>(at), static_cast<u8>(rng())); break;
                default: in.resize(at); break;
            }
        }
        if (doc.parse(in) != nbt::Error::kNone || !doc.root()) continue;
        out.clear();
        if (text::transcode(doc.root(), out) != Result::kOk) continue;
        ++written;
        const Bytes once(out.data, out.data + out.len);
        ASSERT_EQ(transcode(once), ok(once)) << round;
    }
    EXPECT_GT(written, 1000u);
}

TEST(TextComponent, WhatAClientBuilds) {
    // An anvil's rename, an /give with a styled name, a line of lore.
    EXPECT_EQ(transcode(plain("Excalibur")), ok(plain("Excalibur")));
    EXPECT_EQ(transcode(record({s("text", "Excalibur"), s("color", "gold"), b("italic", 0)})),
              ok(record({s("color", "gold"), s("text", "Excalibur"), b("italic", 0)})));
    EXPECT_EQ(transcode(record({s("translate", "block.minecraft.ominous_banner")})),
              ok(record({s("translate", "block.minecraft.ominous_banner")})));
    EXPECT_EQ(transcode(record({s("text", ""), l("extra", 10,
                                                 {compound({s("text", "Forged in "), s("color", "gray")}),
                                                  compound({s("text", "fire"), s("color", "red"), b("bold", 1)})})})),
              ok(record({l("extra", 10,
                           {compound({s("color", "gray"), s("text", "Forged in ")}),
                            compound({s("color", "red"), s("text", "fire"), b("bold", 1)})}),
                         s("text", "")})));
}
