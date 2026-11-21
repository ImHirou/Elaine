// SPDX-License-Identifier: GPL-3.0-or-later
// Binary NBT: what the reader takes, what it refuses, how it weighs a tag, and what a tag looks
// like written back. The byte strings are put together by hand from the tag layouts of the
// 26.3 jar (NbtIo, the TagTypes, ListTag), not by the Writer under test.
#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "nbt/nbt.hpp"
#include "nbt_bytes.hpp"

namespace {

using namespace nbt_bytes;
using nbt::Doc;
using nbt::Error;
using nbt::Tag;

// A Doc is a view of the bytes it parsed: they are kept here until the next document.
Bytes g_input;
Error parse(Doc& doc, const Bytes& in, const nbt::Limits& limits = nbt::kUntrusted, nbt::Form form = nbt::Form::kNameless) {
    g_input = in;
    return doc.parse(g_input, form, limits);
}

// The payload of the root as Doc::write puts it out.
Bytes rewritten(const Bytes& in) {
    Doc doc;
    EXPECT_EQ(parse(doc, in), Error::kNone);
    base::ByteBuf out;
    doc.write(doc.root(), out);
    return {out.data, out.data + out.len};
}

std::vector<std::string> names(Tag compound) {
    std::vector<std::string> out;
    for (const Tag e : compound.entries()) out.emplace_back(e.name());
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------- reading

TEST(Nbt, NumbersAsRootTags) {
    Doc doc;
    const Bytes byte = root(1, {0xFE}) + Bytes{0x77};  // what follows the tag is not the tag's
    ASSERT_EQ(parse(doc, byte), Error::kNone);
    EXPECT_EQ(doc.size(), 2u);
    EXPECT_EQ(doc.root().type, nbt::kByte);
    EXPECT_TRUE(doc.root().numeric());
    EXPECT_EQ(doc.root().as_long(), -2);

    ASSERT_EQ(parse(doc, root(2, be16(0x8001))), Error::kNone);
    EXPECT_EQ(doc.root().as_int(), -32767);
    ASSERT_EQ(parse(doc, root(3, be32(0xFFFFFFFE))), Error::kNone);
    EXPECT_EQ(doc.root().as_int(), -2);
    ASSERT_EQ(parse(doc, root(4, be64(0x0000000100000002ull))), Error::kNone);
    EXPECT_EQ(doc.root().as_long(), 0x100000002ll);
    EXPECT_EQ(doc.root().as_int(), 2);  // (int) of a long: the low half
    EXPECT_EQ(doc.root().as_double(), 4294967298.0);
    ASSERT_EQ(parse(doc, root(5, be32(std::bit_cast<u32>(-2.75f)))), Error::kNone);
    EXPECT_EQ(doc.root().as_double(), -2.75);
    EXPECT_EQ(doc.root().as_int(), -2);  // toward zero
    ASSERT_EQ(parse(doc, root(6, be64(std::bit_cast<u64>(1e300)))), Error::kNone);
    EXPECT_EQ(doc.root().as_int(), std::numeric_limits<i32>::max());  // saturates
    EXPECT_EQ(doc.root().as_long(), std::numeric_limits<i64>::max());
    ASSERT_EQ(parse(doc, root(6, be64(std::bit_cast<u64>(-1e300)))), Error::kNone);
    EXPECT_EQ(doc.root().as_int(), std::numeric_limits<i32>::min());
    ASSERT_EQ(parse(doc, root(5, be32(0x7FC00000))), Error::kNone);
    EXPECT_EQ(doc.root().as_int(), 0);  // NaN
    EXPECT_TRUE(std::isnan(doc.root().as_double()));
    EXPECT_FALSE(doc.root().collection());
}

TEST(Nbt, EndAloneIsNoTag) {
    Doc doc;
    ASSERT_EQ(parse(doc, {0x00, 0x55}), Error::kNone);
    EXPECT_FALSE(doc.root());
    EXPECT_EQ(doc.size(), 1u);
}

TEST(Nbt, NamedRootSkipsTheName) {
    Doc doc;
    // TAG_Compound "hello world" { TAG_String "name": "Bananrama" } - the classic test file.
    const Bytes file = Bytes{10} + str("hello world") + compound({entry(8, "name", str("Bananrama"))});
    ASSERT_EQ(parse(doc, file, nbt::kTrusted, nbt::Form::kNamed), Error::kNone);
    EXPECT_EQ(doc.size(), file.size());
    EXPECT_EQ(doc.root().type, nbt::kCompound);
    EXPECT_EQ(doc.root().get("name").str(), "Bananrama");
    // Read as a network tag the same bytes are a compound whose first entry has type 0x00: empty.
    ASSERT_EQ(parse(doc, file), Error::kNone);
    EXPECT_EQ(doc.root().size(), 0u);
    EXPECT_EQ(doc.size(), 2u);
    // The name is skipped, not decoded: bytes readUTF would throw on are fine there.
    ASSERT_EQ(parse(doc, Bytes{1} + be16(2) + Bytes{0xFF, 0xFF} + Bytes{5}, nbt::kUntrusted, nbt::Form::kNamed), Error::kNone);
    EXPECT_EQ(doc.root().as_long(), 5);
}

TEST(Nbt, CompoundEntries) {
    Doc doc;
    const Bytes in = root(10, compound({
                                  entry(1, "b", {7}),
                                  entry(8, "s", str("text")),
                                  entry(10, "inner", compound({entry(3, "x", be32(42)), entry(3, "y", be32(43))})),
                                  entry(4, "l", be64(9)),
                                  entry(1, "b", {8}),  // again: a map keeps the last
                              }));
    ASSERT_EQ(parse(doc, in), Error::kNone);
    const Tag r = doc.root();
    EXPECT_EQ(doc.size(), in.size());
    EXPECT_EQ(r.size(), 5u);  // entries as read, the repeated one included
    EXPECT_EQ(names(r), (std::vector<std::string>{"b", "s", "inner", "l", "b"}));
    EXPECT_EQ(r.get("b").as_long(), 8);
    EXPECT_EQ(r.get("s").str(), "text");
    EXPECT_EQ(r.get("l").as_long(), 9);
    EXPECT_FALSE(r.get("nope"));
    EXPECT_FALSE(r.get("x"));  // not found through a nested compound
    const Tag inner = r.get("inner");
    ASSERT_EQ(inner.type, nbt::kCompound);
    EXPECT_EQ(inner.size(), 2u);
    EXPECT_EQ(inner.get("y").as_int(), 43);
    EXPECT_FALSE(r.get("s").get("anything"));  // get() on what is not a compound
    EXPECT_FALSE(r.wrapper());
}

TEST(Nbt, ListsOfNumbersHaveNoRecords) {
    Doc doc;
    ASSERT_EQ(parse(doc, root(9, list(3, {be32(1), be32(0xFFFFFFFF), be32(3)}))), Error::kNone);
    const Tag l = doc.root();
    EXPECT_EQ(l.size(), 3u);
    EXPECT_EQ(l.element_type(), nbt::kInt);
    EXPECT_TRUE(l.collection());
    std::vector<i64> got;
    for (const Tag e : l.elements()) {
        EXPECT_EQ(e.type, nbt::kInt);
        EXPECT_TRUE(e.flat);
        got.push_back(e.as_long());
    }
    EXPECT_EQ(got, (std::vector<i64>{1, -1, 3}));
    // One record for the whole list.
    ASSERT_EQ(parse(doc, root(10, compound({entry(9, "l", list(6, {be64(0), be64(0)})), entry(1, "after", {1})}))), Error::kNone);
    EXPECT_EQ(doc.root().get("after").as_long(), 1);
}

TEST(Nbt, ArraysAreCollectionsOfTheirNumbers) {
    Doc doc;
    ASSERT_EQ(parse(doc, root(7, be32(3) + Bytes{1, 0xFF, 3})), Error::kNone);
    EXPECT_EQ(doc.root().size(), 3u);
    std::vector<i64> got;
    for (const Tag e : doc.root().elements()) {
        EXPECT_EQ(e.type, nbt::kByte);
        got.push_back(e.as_long());
    }
    EXPECT_EQ(got, (std::vector<i64>{1, -1, 3}));
    ASSERT_EQ(parse(doc, root(11, be32(2) + be32(5) + be32(6))), Error::kNone);
    got.clear();
    for (const Tag e : doc.root().elements()) got.push_back(e.type == nbt::kInt ? e.as_long() : -100);
    EXPECT_EQ(got, (std::vector<i64>{5, 6}));
    ASSERT_EQ(parse(doc, root(12, be32(1) + be64(0x8000000000000000ull))), Error::kNone);
    for (const Tag e : doc.root().elements()) EXPECT_EQ(e.as_long(), std::numeric_limits<i64>::min());
    ASSERT_EQ(parse(doc, root(12, be32(0))), Error::kNone);
    EXPECT_EQ(doc.root().size(), 0u);
    for (const Tag e : doc.root().elements()) FAIL() << e.type;
}

TEST(Nbt, ListsOfTagsAndWrappedElements) {
    Doc doc;
    ASSERT_EQ(parse(doc, root(9, list(8, {str("a"), str(""), str("ccc")}))), Error::kNone);
    std::vector<std::string> strings;
    for (const Tag e : doc.root().elements()) strings.emplace_back(e.str());
    EXPECT_EQ(strings, (std::vector<std::string>{"a", "", "ccc"}));

    // A list of lists; an empty list may name any element type, even one that does not exist.
    ASSERT_EQ(parse(doc, root(9, list(9, {list(1, {{1}, {2}}), list(0, {}), list(99, {})}))), Error::kNone);
    std::vector<u32> sizes;
    for (const Tag e : doc.root().elements()) sizes.push_back(e.size());
    EXPECT_EQ(sizes, (std::vector<u32>{2, 0, 0}));

    // ListTag.addAndUnwrap: {"": x} in a list of compounds is x.
    const Bytes mixed =
        root(9, list(10, {
                             compound({entry(8, "", str("plain"))}),
                             compound({entry(3, "x", be32(1))}),
                             compound({entry(1, "", {5})}),
                             compound({entry(1, "", {5}), entry(1, "", {6})}),          // one name twice: still one tag
                             compound({entry(1, "", {5}), entry(1, "a", {6})}),         // two names: a compound
                             compound({}),                                              // empty: a compound
                             compound({entry(10, "", compound({entry(1, "", {9})}))}),  // taken off once, not twice
                         }));
    ASSERT_EQ(parse(doc, mixed), Error::kNone);
    std::vector<int> types;
    std::vector<Tag> elements;
    for (const Tag e : doc.root().elements()) {
        types.push_back(e.type);
        elements.push_back(e);
    }
    EXPECT_EQ(types, (std::vector<int>{nbt::kString, nbt::kCompound, nbt::kByte, nbt::kByte, nbt::kCompound, nbt::kCompound,
                                       nbt::kCompound}));
    EXPECT_EQ(elements[0].str(), "plain");
    EXPECT_EQ(elements[1].get("x").as_int(), 1);
    EXPECT_EQ(elements[2].as_long(), 5);
    EXPECT_EQ(elements[3].as_long(), 6);
    EXPECT_EQ(elements[4].size(), 2u);
    EXPECT_EQ(elements[5].size(), 0u);
    EXPECT_TRUE(elements[6].wrapper());  // {"": 9b}: what the outer wrapper held
    EXPECT_EQ(elements[6].get("").as_long(), 9);
    // Outside a list such a compound is a compound.
    ASSERT_EQ(parse(doc, root(10, compound({entry(8, "", str("plain"))}))), Error::kNone);
    EXPECT_EQ(doc.root().type, nbt::kCompound);
    EXPECT_TRUE(doc.root().wrapper());
}

TEST(Nbt, WeighsTagsAsNbtAccounterDoes) {
    Doc doc;
    ASSERT_EQ(parse(doc, root(1, {0})), Error::kNone);
    EXPECT_EQ(doc.weight(), 9u);
    ASSERT_EQ(parse(doc, root(8, str("hi"))), Error::kNone);
    EXPECT_EQ(doc.weight(), 36u + 2 * 2);
    ASSERT_EQ(parse(doc, root(8, str("\xD0\xB6\xE2\x82\xAC"))), Error::kNone);  // two characters in five bytes
    EXPECT_EQ(doc.weight(), 36u + 2 * 2);
    ASSERT_EQ(parse(doc, root(9, list(3, {be32(1), be32(2), be32(3)}))), Error::kNone);
    EXPECT_EQ(doc.weight(), 36u + 3 * 4 + 3 * 12);
    ASSERT_EQ(parse(doc, root(9, list(8, {str("ab")}))), Error::kNone);
    EXPECT_EQ(doc.weight(), 36u + 4 + 36 + 4);
    ASSERT_EQ(parse(doc, root(11, be32(5) + Bytes(20, 0))), Error::kNone);
    EXPECT_EQ(doc.weight(), 24u + 20);
    ASSERT_EQ(parse(doc, root(12, be32(2) + Bytes(16, 0))), Error::kNone);
    EXPECT_EQ(doc.weight(), 24u + 16);
    // {"a": 1b}: the compound, the name (28 + 2 per character), the map entry, the byte.
    const Bytes one = root(10, compound({entry(1, "a", {1})}));
    ASSERT_EQ(parse(doc, one), Error::kNone);
    EXPECT_EQ(doc.weight(), 48u + 28 + 2 + 36 + 9);
    EXPECT_EQ(parse(doc, one, {123, 512}), Error::kNone);
    EXPECT_EQ(parse(doc, one, {122, 512}), Error::kTooBig);
    EXPECT_FALSE(doc.root());
    // A million empty compounds in a list are a megabyte on the wire and fifty in vanilla's books.
    Bytes many = Bytes{9, 10} + be32(1000000);
    many.resize(many.size() + 1000000, 0);
    EXPECT_EQ(parse(doc, many), Error::kTooBig);
    EXPECT_EQ(parse(doc, many, nbt::kTrusted), Error::kNone);
    EXPECT_EQ(doc.weight(), 36u + 1000000ull * (4 + 48));
}

TEST(Nbt, NestsFiveHundredAndTwelveDeep) {
    const auto nested = [](u32 depth) {
        Bytes in{9};
        for (u32 i = 1; i < depth; ++i) in = in + Bytes{9} + be32(1);
        return in + Bytes{0} + be32(0);
    };
    Doc doc;
    EXPECT_EQ(parse(doc, nested(512)), Error::kNone);
    EXPECT_EQ(parse(doc, nested(513)), Error::kTooDeep);
    // Compounds count the same, and a list of numbers is a list.
    Bytes in{10};
    for (u32 i = 1; i < 512; ++i) in = in + Bytes{10} + str("");
    const Bytes tail(512, 0);
    EXPECT_EQ(parse(doc, in + tail), Error::kNone);
    EXPECT_EQ(parse(doc, in + Bytes{9} + str("") + list(1, {{1}}) + tail), Error::kTooDeep);
    EXPECT_EQ(parse(doc, in + Bytes{7} + str("") + be32(0) + tail), Error::kNone);  // an array is not
    EXPECT_EQ(parse(doc, nested(3), {1 << 20, 2}), Error::kTooDeep);
}

TEST(Nbt, WhatTheLoaderThrowsOn) {
    Doc doc;
    EXPECT_EQ(parse(doc, {}), Error::kTruncated);
    EXPECT_EQ(parse(doc, {13}), Error::kBadType);
    EXPECT_EQ(parse(doc, {0xFF}), Error::kBadType);
    EXPECT_EQ(parse(doc, root(10, entry(13, "x", {}))), Error::kBadType);
    EXPECT_EQ(parse(doc, root(9, Bytes{0} + be32(1))), Error::kBadType);   // "Missing type on ListTag"
    EXPECT_EQ(parse(doc, root(9, Bytes{13} + be32(1))), Error::kBadType);  // a type that is not, with an element
    EXPECT_EQ(parse(doc, root(9, Bytes{13} + be32(0))), Error::kNone);     // ...and with none
    EXPECT_EQ(parse(doc, root(9, Bytes{1} + be32(0xFFFFFFFF))), Error::kBadLength);
    EXPECT_EQ(parse(doc, root(7, be32(0x80000000))), Error::kBadLength);
    EXPECT_EQ(parse(doc, root(11, be32(0xFFFFFFFF))), Error::kBadLength);
    EXPECT_EQ(parse(doc, root(12, be32(0xFFFFFFFF))), Error::kBadLength);
    // Counts the input cannot hold are caught before anything is done about them.
    EXPECT_EQ(parse(doc, root(9, Bytes{10} + be32(0x7FFFFFFF))), Error::kTruncated);
    EXPECT_EQ(parse(doc, root(9, Bytes{4} + be32(0x7FFFFFFF))), Error::kTruncated);
    EXPECT_EQ(parse(doc, root(12, be32(0x7FFFFFFF))), Error::kTruncated);
    EXPECT_EQ(parse(doc, root(8, be16(5) + raw("abc"))), Error::kTruncated);
    // Every proper prefix of a document is a truncated document.
    const Bytes whole =
        root(10, compound({entry(8, "name", str("value")), entry(9, "list", list(10, {compound({}), compound({})})),
                           entry(11, "ints", be32(1) + be32(7)), entry(6, "d", be64(0))}));
    ASSERT_EQ(parse(doc, whole), Error::kNone);
    for (usize n = 0; n < whole.size(); ++n) {
        EXPECT_EQ(parse(doc, Bytes(whole.begin(), whole.begin() + static_cast<long>(n))), Error::kTruncated) << n;
        EXPECT_FALSE(doc.root());
    }
}

TEST(Nbt, StringsAreJavasModifiedUtf8) {
    Doc doc;
    const auto read = [&](const Bytes& bytes) { return parse(doc, root(8, be16(static_cast<u32>(bytes.size())) + bytes)); };
    EXPECT_EQ(read(raw("plain ascii, longer than eight bytes")), Error::kNone);
    EXPECT_TRUE(doc.plain_strings());
    EXPECT_EQ(doc.root().str(), "plain ascii, longer than eight bytes");
    EXPECT_EQ(read({0xD0, 0xB6, 0xE2, 0x82, 0xAC}), Error::kNone);  // U+0436, U+20AC
    EXPECT_TRUE(doc.plain_strings());
    EXPECT_EQ(read({0xED, 0xA0, 0xBD, 0xED, 0xB8, 0x80}), Error::kNone);  // U+1F600 as its two surrogates
    EXPECT_TRUE(doc.plain_strings());
    EXPECT_EQ(read({0xC0, 0x80}), Error::kNone);  // U+0000 the way Java writes it
    EXPECT_TRUE(doc.plain_strings());
    // What readUTF takes and writeUTF would not have written.
    EXPECT_EQ(read({'a', 0x00, 'b'}), Error::kNone);
    EXPECT_FALSE(doc.plain_strings());
    EXPECT_EQ(read({'0', '1', '2', '3', '4', '5', '6', '7', '8', 0x00}), Error::kNone);  // past the fast path
    EXPECT_FALSE(doc.plain_strings());
    EXPECT_EQ(read({0xC1, 0x81}), Error::kNone);  // 'A' in two bytes
    EXPECT_FALSE(doc.plain_strings());
    EXPECT_EQ(read({0xE0, 0x81, 0x81}), Error::kNone);  // 'A' in three
    EXPECT_FALSE(doc.plain_strings());
    // What readUTF throws on.
    EXPECT_EQ(read({0x80}), Error::kBadString);                    // a continuation byte first
    EXPECT_EQ(read({0xF0, 0x9F, 0x98, 0x80}), Error::kBadString);  // four-byte UTF-8
    EXPECT_EQ(read({0xC3}), Error::kBadString);                    // cut short
    EXPECT_EQ(read({0xC3, 0x41}), Error::kBadString);
    EXPECT_EQ(read({0xE2, 0x82}), Error::kBadString);
    EXPECT_EQ(read({0xE2, 0x82, 0x41}), Error::kBadString);
    EXPECT_EQ(read({'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 0xFF}), Error::kBadString);
    // Names are read the same way.
    EXPECT_EQ(parse(doc, root(10, Bytes{1} + be16(1) + Bytes{0x80} + Bytes{0, 0})), Error::kBadString);
    EXPECT_EQ(parse(doc, root(10, Bytes{1} + be16(1) + Bytes{0x00} + Bytes{0, 0})), Error::kNone);
    EXPECT_FALSE(doc.plain_strings());
    EXPECT_EQ(parse(doc, root(1, {0})), Error::kNone);  // the flag is the document's
    EXPECT_TRUE(doc.plain_strings());
}

// ---------------------------------------------------------------------------- writing back what was read

TEST(NbtWrite, WhatIsAlreadyInOrderComesBackAsItWas) {
    const Bytes in = compound({
        entry(1, "a", {1}),
        entry(2, "b", be16(2)),
        entry(3, "c", be32(3)),
        entry(4, "d", be64(4)),
        entry(5, "e", be32(std::bit_cast<u32>(1.5f))),
        entry(6, "f", be64(std::bit_cast<u64>(2.5))),
        entry(7, "g", be32(2) + Bytes{1, 2}),
        entry(8, "h", str("text")),
        entry(9, "i", list(10, {compound({entry(1, "k", {1})}), compound({})})),
        entry(10, "j", compound({entry(9, "empty", list(0, {}))})),
        entry(11, "k", be32(1) + be32(7)),
        entry(12, "l", be32(1) + be64(8)),
    });
    EXPECT_EQ(rewritten(root(10, in)), in);
    EXPECT_EQ(rewritten(root(8, str("text"))), str("text"));
    EXPECT_EQ(rewritten(root(3, be32(7))), be32(7));
    EXPECT_EQ(rewritten(root(12, be32(1) + be64(8))), be32(1) + be64(8));
}

TEST(NbtWrite, CompoundsLoseRepeatsAndGainAnOrder) {
    const Bytes in = compound({
        entry(1, "zeta", {1}),
        entry(1, "b", {2}),
        entry(10, "m", compound({entry(1, "y", {1}), entry(1, "x", {2}), entry(1, "y", {3})})),
        entry(8, "b", str("the last b wins, whatever its type")),
        entry(1, "B", {4}),
        entry(1, "", {5}),
        entry(1, "ba", {6}),
    });
    const Bytes out = compound({
        entry(1, "", {5}),
        entry(1, "B", {4}),
        entry(8, "b", str("the last b wins, whatever its type")),
        entry(1, "ba", {6}),
        entry(10, "m", compound({entry(1, "x", {2}), entry(1, "y", {3})})),
        entry(1, "zeta", {1}),
    });
    EXPECT_EQ(rewritten(root(10, in)), out);
    EXPECT_EQ(rewritten(root(10, out)), out);
    // Names sort as bytes: U+00E9 (C3 A9) after every ASCII name.
    EXPECT_EQ(rewritten(root(10, compound({entry(1, "\xC3\xA9", {1}), entry(1, "z", {2})}))),
              compound({entry(1, "z", {2}), entry(1, "\xC3\xA9", {1})}));
}

TEST(NbtWrite, ListsAsListTagWritesThem) {
    // An empty list has no element type left.
    EXPECT_EQ(rewritten(root(9, list(8, {}))), list(0, {}));
    EXPECT_EQ(rewritten(root(9, list(10, {}))), list(0, {}));
    // Numbers and strings: as they are.
    EXPECT_EQ(rewritten(root(9, list(2, {be16(1), be16(2)}))), list(2, {be16(1), be16(2)}));
    EXPECT_EQ(rewritten(root(9, list(8, {str("a"), str("b")}))), list(8, {str("a"), str("b")}));
    // Wrapped elements of one type: the wrappers go, the list takes their type.
    EXPECT_EQ(rewritten(root(9, list(10, {compound({entry(1, "", {1})}), compound({entry(1, "", {2})})}))), list(1, {{1}, {2}}));
    EXPECT_EQ(rewritten(root(9, list(10, {compound({entry(8, "", str("a"))})}))), list(8, {str("a")}));
    EXPECT_EQ(rewritten(root(9, list(10, {compound({entry(10, "", compound({entry(1, "k", {1})}))})}))),
              list(10, {compound({entry(1, "k", {1})})}));
    // Mixed: a list of compounds, everything else wrapped.
    const Bytes mixed =
        list(10, {compound({entry(8, "", str("a"))}), compound({entry(1, "k", {1})}), compound({entry(3, "", be32(7))})});
    EXPECT_EQ(rewritten(root(9, mixed)), mixed);
    // A compound that looks like a wrapper is wrapped itself, or it would be taken off on the way back.
    const Bytes looks = list(10, {compound({entry(10, "", compound({entry(1, "", {9})}))}), compound({entry(1, "k", {1})})});
    EXPECT_EQ(rewritten(root(9, looks)), looks);
    const Bytes only = list(10, {compound({entry(10, "", compound({entry(1, "", {9})}))})});
    EXPECT_EQ(rewritten(root(9, only)), only);
    // Lists in lists, and compounds in them sorted too.
    EXPECT_EQ(rewritten(root(9, list(9, {list(10, {compound({entry(1, "b", {1}), entry(1, "a", {2})})}), list(3, {})}))),
              list(9, {list(10, {compound({entry(1, "a", {2}), entry(1, "b", {1})})}), list(0, {})}));
    // Arrays stay arrays.
    EXPECT_EQ(rewritten(root(9, list(11, {be32(1) + be32(5), be32(0)}))), list(11, {be32(1) + be32(5), be32(0)}));
}

TEST(NbtWrite, DamagedDocumentsAreRefusedOrReadSafely) {
    // A document with one of everything, damaged at random: a byte changed, one put in, the end
    // cut off. Whatever still reads can be written back, and what was written is its own
    // rewriting. (Run under the sanitizers this is the reader's fuzz test.)
    const Bytes whole = root(
        10, compound({
                entry(1, "b", {1}),
                entry(8, "name", str("a string of some length")),
                entry(9, "ints", list(3, {be32(1), be32(2), be32(3)})),
                entry(9, "strings", list(8, {str("a"), str(""), str("\xD0\xB6")})),
                entry(9, "mixed",
                      list(10, {compound({entry(8, "", str("x"))}), compound({entry(1, "k", {1}), entry(1, "k", {2})}),
                                compound({entry(3, "", be32(7))})})),
                entry(9, "lists", list(9, {list(1, {{1}, {2}}), list(0, {}), list(10, {compound({})})})),
                entry(10, "nested", compound({entry(10, "deeper", compound({entry(6, "d", be64(1))})), entry(4, "l", be64(2))})),
                entry(7, "bytes", be32(3) + Bytes{1, 2, 3}),
                entry(11, "ia", be32(2) + be32(1) + be32(2)),
                entry(12, "la", be32(1) + be64(1)),
                entry(1, "b", {2}),
            }));
    std::mt19937 rng(20261115);
    Doc doc, again;
    base::ByteBuf first, second;
    const auto rewrite = [](Doc& d, base::ByteBuf& out) {
        out.clear();
        *out.reserve(1) = d.root().type;
        out.len = 1;
        if (d.root()) d.write(d.root(), out);
    };
    u32 read = 0;
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
        if (parse(doc, in) != Error::kNone) continue;
        ++read;
        rewrite(doc, first);
        ASSERT_EQ(again.parse({first.data, first.len}), Error::kNone) << round;
        ASSERT_EQ(again.size(), first.len) << round;
        rewrite(again, second);
        ASSERT_EQ(Bytes(first.data, first.data + first.len), Bytes(second.data, second.data + second.len)) << round;
    }
    EXPECT_GT(read, 1000u);  // enough of them survive to mean something
}

// ---------------------------------------------------------------------------- Writer

TEST(NbtWriter, WritesWhatTheReaderReads) {
    base::ByteBuf buf;
    nbt::Writer w(buf);
    w.root(nbt::kCompound);
    w.put_byte("b", -1);
    w.put_short("s", -2);
    w.put_int("i", 3);
    w.put_long("l", -4);
    w.put_float("f", 1.5f);
    w.put_double("d", 2.5);
    w.put_string("str", "text");
    const i8 bytes[] = {1, -1};
    const i32 ints[] = {5, -6};
    const i64 longs[] = {7};
    w.put_byte_array("ba", bytes);
    w.put_int_array("ia", ints);
    w.put_long_array("la", longs);
    w.begin_list("items", nbt::kCompound, 2);
    for (int i = 0; i < 2; ++i) {
        w.begin_compound();
        w.put_byte("Slot", static_cast<i8>(i));
        w.end_compound();
    }
    w.begin_list("names", nbt::kString, 2);
    w.put_string("a");
    w.put_string("b");
    w.begin_list("numbers", nbt::kInt, 2);
    w.put_int(10);
    w.put_int(11);
    w.begin_compound("inner");
    w.put_string_raw("raw", "as is");
    const u8 payload[] = {0, 0, 0, 9};
    w.put_raw("copied", nbt::kInt, payload);
    w.end_compound();
    w.end_compound();

    const Bytes expected =
        root(10, compound({
                     entry(1, "b", {0xFF}),
                     entry(2, "s", be16(0xFFFE)),
                     entry(3, "i", be32(3)),
                     entry(4, "l", be64(0xFFFFFFFFFFFFFFFCull)),
                     entry(5, "f", be32(std::bit_cast<u32>(1.5f))),
                     entry(6, "d", be64(std::bit_cast<u64>(2.5))),
                     entry(8, "str", str("text")),
                     entry(7, "ba", be32(2) + Bytes{1, 0xFF}),
                     entry(11, "ia", be32(2) + be32(5) + be32(0xFFFFFFFA)),
                     entry(12, "la", be32(1) + be64(7)),
                     entry(9, "items", list(10, {compound({entry(1, "Slot", {0})}), compound({entry(1, "Slot", {1})})})),
                     entry(9, "names", list(8, {str("a"), str("b")})),
                     entry(9, "numbers", list(3, {be32(10), be32(11)})),
                     entry(10, "inner", compound({entry(8, "raw", str("as is")), entry(3, "copied", be32(9))})),
                 }));
    EXPECT_EQ(Bytes(buf.data, buf.data + buf.len), expected);

    Doc doc;
    ASSERT_EQ(parse(doc, expected), Error::kNone);
    EXPECT_EQ(doc.size(), expected.size());
    EXPECT_EQ(doc.root().get("inner").get("copied").as_int(), 9);
    EXPECT_EQ(doc.root().get("items").size(), 2u);
}

TEST(NbtWriter, RootsAndStrings) {
    base::ByteBuf buf;
    nbt::Writer w(buf);
    w.root(nbt::kString);  // a text component that is plain text: a nameless TAG_String
    w.put_string("hi");
    EXPECT_EQ(Bytes(buf.data, buf.data + buf.len), (Bytes{8, 0, 2, 'h', 'i'}));

    buf.clear();
    w.root(nbt::kCompound, "file");  // a named root: the file form
    w.end_compound();
    EXPECT_EQ(Bytes(buf.data, buf.data + buf.len), Bytes{10} + str("file") + Bytes{0});

    // UTF-8 becomes modified UTF-8: NUL in two bytes, U+1F600 as two surrogates of three bytes.
    buf.clear();
    w.put_string(std::string_view("a\0\xF0\x9F\x98\x80", 6));
    EXPECT_EQ(Bytes(buf.data, buf.data + buf.len), be16(9) + (Bytes{'a', 0xC0, 0x80, 0xED, 0xA0, 0xBD, 0xED, 0xB8, 0x80}));
    Doc doc;
    ASSERT_EQ(parse(doc, Bytes{8} + Bytes(buf.data, buf.data + buf.len)), Error::kNone);
    EXPECT_TRUE(doc.plain_strings());
}
