// SPDX-License-Identifier: GPL-3.0-or-later
// NBT as text and what is done with two tags: the SNBT reader and printer, comparing, merging,
// paths, the order of a HashMap, numbers as Java prints them.
//
// What a tag reads as is checked against bytes put together by hand (nbt_bytes.hpp). The
// messages of numbers out of range and the digits of floats were printed by the JDK itself
// (Byte.parseByte ... Long.parseUnsignedLong, Double.toString, String.format) and copied here;
// which error a wrong tag gets, and where, is compared with a vanilla server by
// tools/vanilla/command_probe.py - the cases here pin what was seen there.
#include <gtest/gtest.h>

#include <limits>
#include <string>
#include <vector>

#include "base/java_number.hpp"
#include "nbt/nbt.hpp"
#include "nbt/ops.hpp"
#include "nbt/snbt.hpp"
#include "nbt_bytes.hpp"
#include "text/nbt_text.hpp"

namespace {

using namespace nbt_bytes;
using nbt::Doc;
using nbt::Tag;

// The tag `text` reads as, in the network form; empty if it does not read.
Bytes read(std::string_view text, u32* end = nullptr) {
    base::ByteBuf out;
    nbt::SnbtError err;
    u32 cursor = 0;
    if (!nbt::read_snbt(text, cursor, out, err)) return {};
    if (end) *end = cursor;
    return {out.data, out.data + out.len};
}

struct Failure {
    std::string key, arg;
    u32 cursor;
    bool operator==(const Failure&) const = default;
};
std::ostream& operator<<(std::ostream& os, const Failure& f) { return os << f.key << "(" << f.arg << ")@" << f.cursor; }
Failure fails(std::string_view text) {
    base::ByteBuf out;
    nbt::SnbtError err;
    u32 cursor = 0;
    if (nbt::read_snbt(text, cursor, out, err)) return {"read", "", cursor};
    return {std::string(err.key), err.arg, err.cursor};
}

// A Doc over bytes that stay where they are.
struct Loaded {
    Bytes bytes;
    Doc doc;
    explicit Loaded(Bytes b) : bytes(std::move(b)) {
        EXPECT_EQ(doc.parse(bytes, nbt::Form::kNameless, nbt::kTrusted), nbt::Error::kNone);
    }
    explicit Loaded(std::string_view snbt) : Loaded(read(snbt)) {}
    Tag root() const { return doc.root(); }
};

std::string printed(std::string_view snbt) {
    const Loaded tag(snbt);
    std::string out;
    nbt::print_snbt(tag.root(), out);
    return out;
}

Bytes d(double v) { return be64(std::bit_cast<u64>(v)); }
Bytes f(float v) { return be32(std::bit_cast<u32>(v)); }

}  // namespace

// ---------------------------------------------------------------------------- reading

TEST(Snbt, Numbers) {
    EXPECT_EQ(read("5"), root(3, be32(5)));
    EXPECT_EQ(read("-5"), root(3, be32(0xFFFFFFFB)));
    EXPECT_EQ(read("+5"), root(3, be32(5)));
    EXPECT_EQ(read("5b"), root(1, {5}));
    EXPECT_EQ(read("-128B"), root(1, {0x80}));
    EXPECT_EQ(read("5s"), root(2, be16(5)));
    EXPECT_EQ(read("5i"), root(3, be32(5)));
    EXPECT_EQ(read("5L"), root(4, be64(5)));
    EXPECT_EQ(read("1_000"), root(3, be32(1000)));
    EXPECT_EQ(read("0"), root(3, be32(0)));
    EXPECT_EQ(read("0b"), root(1, {0}));  // not the beginning of a binary number
    // Another base is unsigned unless it says otherwise.
    EXPECT_EQ(read("0xFF"), root(3, be32(255)));
    EXPECT_EQ(read("0xFFub"), root(1, {0xFF}));
    EXPECT_EQ(read("0xFFFFFFFF"), root(3, be32(0xFFFFFFFF)));
    EXPECT_EQ(read("0b101"), root(3, be32(5)));
    EXPECT_EQ(read("0b1111_1111ub"), root(1, {0xFF}));
    EXPECT_EQ(read("0x7Fsb"), root(1, {0x7F}));
    EXPECT_EQ(read("255ub"), root(1, {0xFF}));
    EXPECT_EQ(read("65535us"), root(2, be16(0xFFFF)));
    EXPECT_EQ(read("18446744073709551615uL"), root(4, be64(~0ull)));
    EXPECT_EQ(read("-9223372036854775808L"), root(4, be64(1ull << 63)));

    EXPECT_EQ(read("1.5"), root(6, d(1.5)));
    EXPECT_EQ(read("1."), root(6, d(1.0)));
    EXPECT_EQ(read(".5"), root(6, d(0.5)));
    EXPECT_EQ(read("-.5e-3"), root(6, d(-0.5e-3)));
    EXPECT_EQ(read("1e3"), root(6, d(1000.0)));
    EXPECT_EQ(read("1E3f"), root(5, f(1000.0f)));
    EXPECT_EQ(read("1f"), root(5, f(1.0f)));
    EXPECT_EQ(read("0.1f"), root(5, f(0.1f)));
    EXPECT_EQ(read("1d"), root(6, d(1.0)));
    EXPECT_EQ(read("1e-999"), root(6, d(0.0)));  // too small is nothing, not an error
    EXPECT_EQ(read("true"), root(1, {1}));
    EXPECT_EQ(read("FALSE"), root(1, {0}));
}

TEST(Snbt, WhereANumberEnds) {
    u32 end = 0;
    // What cannot go on as a number is left for the caller: Brigadier then misses its space.
    EXPECT_EQ(read("5 6", &end), root(3, be32(5)));
    EXPECT_EQ(end, 1u);
    EXPECT_EQ(read("1abc", &end), root(3, be32(1)));
    EXPECT_EQ(end, 1u);
    EXPECT_EQ(read("1e999", &end), root(3, be32(1)));  // not a double: the int in front of it
    EXPECT_EQ(end, 1u);
    EXPECT_EQ(fails("007"), (Failure{"argument.literal.incorrect", ".", 3}));  // a decimal number has no leading zero
    // Whitespace is skipped before every token, inside a number too.
    EXPECT_EQ(read("  - 5 b", &end), root(1, {0xFB}));
    EXPECT_EQ(end, 7u);
    EXPECT_EQ(read("1 . 5", &end), root(6, d(1.5)));
}

TEST(Snbt, NumbersJavaRefuses) {
    const auto parse_failure = [](std::string_view text, std::string message) {
        EXPECT_EQ(fails(std::string("[") + std::string(text) + "]"),
                  (Failure{"snbt.parser.number_parse_failure", std::move(message), static_cast<u32>(text.size()) + 1}))
            << text;
    };
    parse_failure("300b", "Value out of range. Value:\"300\" Radix:10");
    parse_failure("-129b", "Value out of range. Value:\"-129\" Radix:10");
    parse_failure("99999999999b", "For input string: \"99999999999\"");
    parse_failure("70000ss", "Value out of range. Value:\"70000\" Radix:10");
    parse_failure("2147483648i", "For input string: \"2147483648\"");
    parse_failure("-2147483649i", "For input string: \"-2147483649\"");
    parse_failure("9223372036854775808L", "For input string: \"9223372036854775808\"");
    parse_failure("0x100000000si", "For input string: \"100000000\" under radix 16");
    parse_failure("0x1FFFFFFFFi", "String value 1FFFFFFFF exceeds range of unsigned int.");
    parse_failure("4294967296ui", "String value 4294967296 exceeds range of unsigned int.");
    parse_failure("0x1FFFFFFFFFFFFFFFFL", "String value 1FFFFFFFFFFFFFFFF exceeds range of unsigned long.");
    parse_failure("18446744073709551616uL", "String value 18446744073709551616 exceeds range of unsigned long.");
    parse_failure("256ub", "out of range: 256");
    parse_failure("65536us", "out of range: 65536");
    EXPECT_EQ(fails("[-1ub]").key, "snbt.parser.expected_non_negative_number");
    EXPECT_EQ(fails("[-0x1i]").key, "snbt.parser.expected_non_negative_number");
    // Without a suffix the reason is hidden behind an earlier one at the same place: the grammar
    // tried a float first, and a suffix.
    EXPECT_EQ(fails("[2147483648]"), (Failure{"argument.literal.incorrect", ".", 11}));
    EXPECT_EQ(fails("[0x1FFFFFFFF]"), (Failure{"argument.literal.incorrect", "u|U", 12}));
    EXPECT_EQ(fails("[70000s]"), (Failure{"argument.literal.incorrect", "b|B", 7}));
}

TEST(Snbt, Strings) {
    EXPECT_EQ(read("abc"), root(8, str("abc")));
    EXPECT_EQ(read("a.b_c-d+e"), root(8, str("a.b_c-d+e")));
    EXPECT_EQ(read("\"a b\""), root(8, str("a b")));
    EXPECT_EQ(read("'a b'"), root(8, str("a b")));
    EXPECT_EQ(read("\"it's\""), root(8, str("it's")));
    EXPECT_EQ(read("'say \"hi\"'"), root(8, str("say \"hi\"")));
    EXPECT_EQ(read("\"\""), root(8, str("")));
    EXPECT_EQ(read(R"("a\"b\\c\'d")"), root(8, str("a\"b\\c'd")));
    EXPECT_EQ(read(R"("\b\s\t\n\f\r")"), root(8, str("\b \t\n\f\r")));
    EXPECT_EQ(read(R"("\x41\u00e9\U0001F600")"), root(8, str("A\xC3\xA9\xED\xA0\xBD\xED\xB8\x80")));
    EXPECT_EQ(read(R"("\uD83D\uDE00")"), root(8, str("\xED\xA0\xBD\xED\xB8\x80")));  // the same string in Java
    EXPECT_EQ(read(R"("\x00")"), root(8, str("\xC0\x80")));
    EXPECT_EQ(read("\"\xD0\xB6\""), root(8, str("\xD0\xB6")));

    EXPECT_EQ(fails(R"("\U00110000")"), (Failure{"snbt.parser.invalid_codepoint", "U+00110000", 11}));
    EXPECT_EQ(fails(R"("\N{SNOWMAN}")").key, "snbt.parser.invalid_character_name");  // no table of names here
    EXPECT_EQ(fails(R"("\u12")"), (Failure{"snbt.parser.expected_hex_escape", "4", 3}));
    EXPECT_EQ(fails("\"abc"), (Failure{"snbt.parser.invalid_string_contents", "", 4}));
    EXPECT_EQ(fails("-"), (Failure{"snbt.parser.expected_decimal_numeral", "", 1}));
}

TEST(Snbt, CompoundsAndLists) {
    EXPECT_EQ(read("{}"), root(10, compound({})));
    EXPECT_EQ(read("{a:1,b:\"x\"}"), root(10, compound({i("a", 1), s("b", "x")})));
    EXPECT_EQ(read(" { a : 1 , } "), root(10, compound({i("a", 1)})));  // a trailing comma is fine
    EXPECT_EQ(read("{\"a b\":1b,'c':{d:2}}"), root(10, compound({b("a b", 1), c("c", {i("d", 2)})})));
    EXPECT_EQ(read("{a:1,a:2}"), root(10, compound({i("a", 2)})));  // the last of a name is kept
    EXPECT_EQ(read("{true:1}"), root(10, compound({i("true", 1)})));
    EXPECT_EQ(read("{1:2}"), root(10, compound({i("1", 2)})));  // a key is any unquoted string

    EXPECT_EQ(read("[]"), root(9, list(0, {})));
    EXPECT_EQ(read("[1,2,3]"), root(9, list(3, {be32(1), be32(2), be32(3)})));
    EXPECT_EQ(read("[a,b,]"), root(9, list(8, {str("a"), str("b")})));
    EXPECT_EQ(read("[[1],[2b]]"), root(9, list(9, {list(3, {be32(1)}), list(1, {Bytes{2}})})));
    // Mixed: a list of compounds, what is not one wrapped in {"": x} (ListTag.write).
    EXPECT_EQ(read("[1,a]"), root(9, list(10, {compound({i("", 1)}), compound({s("", "a")})})));
    EXPECT_EQ(read("[{a:1},2]"), root(9, list(10, {compound({i("a", 1)}), compound({i("", 2)})})));

    EXPECT_EQ(read("[B;]"), root(7, be32(0)));
    EXPECT_EQ(read("[B;1,-2,0x7F]"), root(7, be32(3) + Bytes{1, 0xFE, 0x7F}));
    EXPECT_EQ(read("[I;1,2b,3s]"), root(11, be32(3) + be32(1) + be32(2) + be32(3)));
    EXPECT_EQ(read("[L;1,2i,-3L]"), root(12, be32(3) + be64(1) + be64(2) + be64(~2ull)));
    EXPECT_EQ(read("[I;200ub]"), root(11, be32(1) + be32(0xFFFFFFC8)));  // the byte it is, widened
    EXPECT_EQ(read("[B]"), root(9, list(8, {str("B")})));                // no ';': a list of one string
    EXPECT_EQ(fails("[B;1s]"), (Failure{"snbt.parser.invalid_array_element_type", "", 6}));
    EXPECT_EQ(fails("[I;1L]").key, "snbt.parser.invalid_array_element_type");

    EXPECT_EQ(fails("{\"\":\"x\"}"), (Failure{"snbt.parser.empty_key", "", 7}));
    EXPECT_EQ(fails("{a:5"), (Failure{"argument.literal.incorrect", ".", 4}));
}

TEST(Snbt, Operations) {
    EXPECT_EQ(read("bool(1)"), root(1, {1}));
    EXPECT_EQ(read("bool(0.0)"), root(1, {0}));
    EXPECT_EQ(read("bool(256)"), root(1, {0}));  // Number.byteValue()
    EXPECT_EQ(read("bool(true)"), root(1, {1}));
    EXPECT_EQ(read("uuid(\"00000001-0002-0003-0004-000000000005\")"),
              root(11, be32(4) + be32(1) + be32(0x00020003) + be32(0x00040000) + be32(5)));
    EXPECT_EQ(read("uuid('1-2-3-4-5')"), root(11, be32(4) + be32(1) + be32(0x00020003) + be32(0x00040000) + be32(5)));
    EXPECT_EQ(fails("bool(a)"), (Failure{"snbt.parser.expected_number_or_boolean", "", 7}));
    EXPECT_EQ(fails("uuid(zz)"), (Failure{"snbt.parser.expected_string_uuid", "", 8}));
    EXPECT_EQ(fails("uuid(1)").key, "snbt.parser.expected_string_uuid");
    EXPECT_EQ(fails("nope(1,2)"), (Failure{"snbt.parser.no_such_operation", "nope/2", 9}));
    u32 end = 0;
    EXPECT_EQ(read("foo(1", &end), root(8, str("foo")));  // not a call after all: the word, and the rest is left
    EXPECT_EQ(end, 3u);
}

TEST(Snbt, CompoundOrNot) {
    base::ByteBuf out;
    nbt::SnbtError err;
    u32 cursor = 2;
    ASSERT_TRUE(nbt::read_snbt_compound("x {a:1} y", cursor, out, err));
    EXPECT_EQ(cursor, 7u);
    EXPECT_EQ(Bytes(out.data, out.data + out.len), root(10, compound({i("a", 1)})));
    cursor = 0;
    EXPECT_FALSE(nbt::read_snbt_compound("[1]", cursor, out, err));
    EXPECT_EQ(err.key, "argument.nbt.expected.compound");
    EXPECT_EQ(err.cursor, 3u);
    EXPECT_FALSE(nbt::read_snbt_fully("{a:1} x", out, err));
    EXPECT_EQ(err.key, "argument.nbt.trailing");
    EXPECT_EQ(err.cursor, 6u);
    out.clear();
    EXPECT_TRUE(nbt::read_snbt_fully(" 5  ", out, err));
}

TEST(Snbt, Limits) {
    std::string deep(600, '[');
    EXPECT_TRUE(fails(deep).key.empty());  // words of our own
    std::string longest = "\"" + std::string(65536, 'a') + "\"";
    EXPECT_TRUE(fails(longest).key.empty());
    longest = "\"" + std::string(65535, 'a') + "\"";
    EXPECT_EQ(read(longest).size(), 3u + 65535);
}

// ---------------------------------------------------------------------------- printing

TEST(Snbt, Prints) {
    EXPECT_EQ(printed("{b:1b,a:2s,c:3,d:4L,e:1.5f,f:2.5d}"), "{a:2s,b:1b,c:3,d:4L,e:1.5f,f:2.5d}");
    EXPECT_EQ(printed("[B;1,2]"), "[B;1B,2B]");
    EXPECT_EQ(printed("[I;1,2]"), "[I;1,2]");
    EXPECT_EQ(printed("[L;1,2]"), "[L;1L,2L]");
    EXPECT_EQ(printed("[1,a,{b:c}]"), "[1,\"a\",{b:\"c\"}]");
    EXPECT_EQ(printed("{\"a b\":1,\"true\":2,\"1\":3,a.b:4}"), "{\"1\":3,\"a b\":1,a.b:4,\"true\":2}");
    EXPECT_EQ(printed("\"it's\""), "\"it's\"");
    EXPECT_EQ(printed("'say \"hi\"'"), "'say \"hi\"'");
    EXPECT_EQ(printed(R"("both ' and \"")"), R"("both ' and \"")");  // the first quote in it decides
    EXPECT_EQ(printed(R"("a\\b\n\x01")"), R"("a\\b\n\x01")");
    EXPECT_EQ(printed("1e10"), "1.0E10d");
    EXPECT_EQ(printed("\"\\U0001F600\""), "\"\xF0\x9F\x98\x80\"");
}

TEST(JavaNumber, DoubleToString) {
    using base::java_double;
    using base::java_float;
    EXPECT_EQ(java_double(1e7), "1.0E7");
    EXPECT_EQ(java_double(1.0E-3), "0.001");
    EXPECT_EQ(java_double(9.999e-4), "9.999E-4");
    EXPECT_EQ(java_double(123456789.125), "1.23456789125E8");
    EXPECT_EQ(java_double(0.1), "0.1");
    EXPECT_EQ(java_double(100), "100.0");
    EXPECT_EQ(java_double(1e21), "1.0E21");
    EXPECT_EQ(java_double(1.5e-10), "1.5E-10");
    EXPECT_EQ(java_double(12345678.9), "1.23456789E7");
    EXPECT_EQ(java_double(4.9e-324), "4.9E-324");
    EXPECT_EQ(java_double(1e23), "1.0E23");
    EXPECT_EQ(java_double(2e-3), "0.002");
    EXPECT_EQ(java_double(1.0E10), "1.0E10");
    EXPECT_EQ(java_double(-0.0), "-0.0");
    EXPECT_EQ(java_double(9999999.0), "9999999.0");
    EXPECT_EQ(java_double(std::numeric_limits<double>::quiet_NaN()), "NaN");
    EXPECT_EQ(java_double(-std::numeric_limits<double>::infinity()), "-Infinity");
    EXPECT_EQ(java_float(1e7f), "1.0E7");
    EXPECT_EQ(java_float(1.0E-3f), "0.001");
    EXPECT_EQ(java_float(9.999e-4f), "9.999E-4");
    EXPECT_EQ(java_float(1.23456789e8f), "1.2345679E8");
    EXPECT_EQ(java_float(0.1f), "0.1");
    EXPECT_EQ(java_float(100.0f), "100.0");
    EXPECT_EQ(java_float(3.4e38f), "3.4E38");
    EXPECT_EQ(java_float(16777216.0f), "1.6777216E7");
    EXPECT_EQ(java_float(1e10f), "1.0E10");
    EXPECT_EQ(java_float(33554432.0f), "3.3554432E7");
    EXPECT_EQ(java_float(1.0E-45f), "1.4E-45");
}

TEST(JavaNumber, Format) {
    using base::java_format_fixed;
    EXPECT_EQ(java_format_fixed(1.005, 2), "1.01");  // half up from the shortest digits, not from the binary value
    EXPECT_EQ(java_format_fixed(2.5, 2), "2.50");
    EXPECT_EQ(java_format_fixed(0.125, 2), "0.13");
    EXPECT_EQ(java_format_fixed(1e20, 2), "100000000000000000000.00");
    EXPECT_EQ(java_format_fixed(-0.001, 2), "-0.00");
    EXPECT_EQ(java_format_fixed(1234567.895, 2), "1234567.90");
    EXPECT_EQ(java_format_fixed(0.5, 6), "0.500000");
}

// ---------------------------------------------------------------------------- two tags

TEST(NbtOps, Equal) {
    const auto eq = [](std::string_view a, std::string_view b) {
        const Loaded x(a), y(b);
        return nbt::equal(x.root(), y.root());
    };
    EXPECT_TRUE(eq("{a:1,b:[1,2]}", "{b:[1,2],a:1}"));
    EXPECT_FALSE(eq("{a:1}", "{a:1b}"));  // another type is another tag
    EXPECT_FALSE(eq("{a:1}", "{a:1,b:2}"));
    EXPECT_FALSE(eq("[1,2]", "[2,1]"));
    EXPECT_TRUE(eq("[1,a]", "[1,a]"));
    EXPECT_FALSE(eq("0.0", "-0.0"));
    EXPECT_TRUE(eq("[I;1,2]", "[I;1,2]"));
    EXPECT_FALSE(eq("[I;1,2]", "[1,2]"));
}

TEST(NbtOps, Matches) {
    const auto match = [](std::string_view expected, std::string_view actual, bool partial = true) {
        const Loaded x(expected), y(actual);
        return nbt::matches(x.root(), y.root(), partial);
    };
    EXPECT_TRUE(match("{}", "{a:1}"));
    EXPECT_TRUE(match("{a:1}", "{a:1,b:2}"));
    EXPECT_FALSE(match("{a:1,c:3}", "{a:1,b:2}"));
    EXPECT_FALSE(match("{a:1b}", "{a:1}"));
    EXPECT_TRUE(match("{a:{b:1}}", "{a:{b:1,c:2}}"));
    // A list asks for its elements somewhere in the other; an empty one for an empty one.
    EXPECT_TRUE(match("{l:[2]}", "{l:[1,2,3]}"));
    EXPECT_TRUE(match("{l:[{a:1}]}", "{l:[{b:2},{a:1,c:3}]}"));
    EXPECT_FALSE(match("{l:[4]}", "{l:[1,2,3]}"));
    EXPECT_FALSE(match("{l:[]}", "{l:[1]}"));
    EXPECT_TRUE(match("{l:[]}", "{l:[]}"));
    EXPECT_FALSE(match("{l:[2]}", "{l:[1,2,3]}", false));
    EXPECT_TRUE(match("{l:[1,2,3]}", "{l:[1,2,3]}", false));
    EXPECT_TRUE(nbt::matches({}, Loaded("1").root(), true));
    EXPECT_FALSE(nbt::matches(Loaded("1").root(), {}, true));
}

TEST(NbtOps, Merge) {
    const auto merged = [](std::string_view into, std::string_view from) {
        const Loaded x(into), y(from);
        base::ByteBuf out;
        *out.reserve(1) = 10;
        out.len = 1;
        nbt::merge(x.root(), y.root(), out);
        const Loaded result(Bytes(out.data, out.data + out.len));
        std::string text;
        nbt::print_snbt(result.root(), text);
        return text;
    };
    EXPECT_EQ(merged("{a:1,b:2}", "{b:3,c:4}"), "{a:1,b:3,c:4}");
    EXPECT_EQ(merged("{a:{x:1,y:2}}", "{a:{y:3,z:4}}"), "{a:{x:1,y:3,z:4}}");
    EXPECT_EQ(merged("{a:{x:1}}", "{a:5}"), "{a:5}");      // not both compounds: replaced
    EXPECT_EQ(merged("{a:[1,2]}", "{a:[3]}"), "{a:[3]}");  // lists are not merged
    EXPECT_EQ(merged("{}", "{a:{b:{}}}"), "{a:{b:{}}}");
}

TEST(NbtOps, MapOrder) {
    // new HashMap<String, Integer>() put a..p in order and iterated: by hash bucket. For single
    // letters the hash is the letter, so with 16 buckets 'p' (112) comes before 'a' (97).
    const Loaded tag("{a:1,b:2,p:3,q:4,ab:5}");
    std::vector<Tag> order;
    nbt::map_order(tag.root(), order);
    std::string names;
    for (const Tag t : order) names += std::string(t.name()) + " ";
    // Buckets: p=112&15=0, a=97&15=1, q=1, b=2, "ab": hash 3105, spread 3105^0=3105, &15 = 1.
    EXPECT_EQ(names, "p a q ab b ");
}

// ---------------------------------------------------------------------------- paths

namespace {

nbt::Path path(std::initializer_list<nbt::Path::Node> nodes, std::string text) {
    nbt::Path p;
    p.text = std::move(text);
    p.nodes = nodes;
    return p;
}
nbt::Path::Node child(std::string name, u32 end) { return {nbt::Path::Node::kChild, std::move(name), 0, {}, end}; }
nbt::Path::Node element(i32 i, u32 end) { return {nbt::Path::Node::kIndex, {}, i, {}, end}; }
nbt::Path::Node all(u32 end) { return {nbt::Path::Node::kAll, {}, 0, {}, end}; }
nbt::Path::Node match(nbt::Path::Node::Kind kind, std::string name, std::string_view pattern, u32 end) {
    return {kind, std::move(name), 0, read(pattern), end};
}

std::string found(const nbt::Path& p, std::string_view snbt) {
    const Loaded tag(snbt);
    std::vector<Tag> tags;
    std::string_view missing;
    if (!nbt::find(p, tag.root(), tags, missing)) return "!" + std::string(missing);
    std::string out;
    for (const Tag t : tags) {
        nbt::print_snbt(t, out);
        out += ' ';
    }
    return out;
}

}  // namespace

TEST(NbtPath, Nodes) {
    using Node = nbt::Path::Node;
    const std::string_view data = "{a:{b:[1,2,3]},l:[{id:x,n:1},{id:y,n:2}],arr:[I;7,8]}";
    EXPECT_EQ(found(path({child("a", 1), child("b", 3)}, "a.b"), data), "[1,2,3] ");
    EXPECT_EQ(found(path({child("a", 1), child("b", 3), element(0, 6)}, "a.b[0]"), data), "1 ");
    EXPECT_EQ(found(path({child("a", 1), child("b", 3), element(-1, 7)}, "a.b[-1]"), data), "3 ");
    EXPECT_EQ(found(path({child("a", 1), child("b", 3), all(5)}, "a.b[]"), data), "1 2 3 ");
    EXPECT_EQ(found(path({child("arr", 3), element(1, 6)}, "arr[1]"), data), "8 ");  // an array is a collection too
    EXPECT_EQ(found(path({child("l", 1), match(Node::kMatchElement, "", "{id:y}", 9), child("n", 11)}, "l[{id:y}].n"), data),
              "2 ");
    EXPECT_EQ(found(path({child("l", 1), all(3), child("n", 5)}, "l[].n"), data), "1 2 ");
    EXPECT_EQ(found(path({match(Node::kMatchObject, "a", "{b:[2]}", 8)}, "a{b:[2]}"), data), "{b:[1,2,3]} ");
    EXPECT_EQ(found(path({match(Node::kMatchRoot, "", "{a:{}}", 6), child("arr", 10)}, "{a:{}}.arr"), data), "[I;7,8] ");
    // Nothing found: the path up to the node that found nothing.
    EXPECT_EQ(found(path({child("a", 1), child("zz", 4), child("c", 6)}, "a.zz.c"), data), "!a.zz");
    EXPECT_EQ(found(path({child("a", 1), child("b", 3), element(5, 6)}, "a.b[5]"), data), "!a.b[5]");
    EXPECT_EQ(found(path({match(Node::kMatchObject, "a", "{b:[9]}", 8)}, "a{b:[9]}"), data), "!a{b:[9]}");
    EXPECT_EQ(found(path({child("arr", 3), match(Node::kMatchElement, "", "{}", 7)}, "arr[{}]"), data), "!arr[{}]");
}

// ---------------------------------------------------------------------------- the text of /data get

TEST(PrettyNbt, OneLine) {
    const auto shown = [](std::string_view snbt) { return text::pretty_nbt(Loaded(snbt).root()).plain(); };
    EXPECT_EQ(shown("{}"), "{}");
    EXPECT_EQ(shown("[]"), "[]");
    EXPECT_EQ(shown("{a:1b}"), "{a: 1b}");
    EXPECT_EQ(shown("{a:\"x y\",\"b c\":[1,2]}"), "{a: \"x y\", \"b c\": [1, 2]}");
    EXPECT_EQ(shown("[I;1,2]"), "[I; 1, 2]");
    EXPECT_EQ(shown("[B;1,2]"), "[B; 1B, 2B]");
    EXPECT_EQ(shown("[L;1]"), "[L; 1L]");
    EXPECT_EQ(shown("[a,b]"), "[\"a\", \"b\"]");
    EXPECT_EQ(shown("1.5f"), "1.5f");
    EXPECT_EQ(shown("2.0d"), "2.0d");
    // Folded: 129 bytes show 128 and "<...>".
    std::string many = "[B;";
    for (int n = 0; n < 129; ++n) many += "1,";
    many += "]";
    const std::string folded = shown(many);
    EXPECT_TRUE(folded.ends_with("1B,<...>]")) << folded;
}
