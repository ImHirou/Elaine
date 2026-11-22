// SPDX-License-Identifier: GPL-3.0-or-later
// What vanilla does with two tags: compare them, lay one over the other - and NBT paths, the
// "Items[{Slot:0b}].id" of `/data`. All of it reads tags where they lie (nbt::Tag); what makes a
// new tag writes it out.
#pragma once

#include <string>
#include <vector>

#include "base/buf.hpp"
#include "nbt/nbt.hpp"

namespace nbt {

// Tag.equals: the same type and the same value - compounds as maps, floats as Float.equals
// compares them (NaN is NaN, 0.0 is not -0.0).
bool equal(Tag a, Tag b);

// NbtUtils.compareNbt: does `actual` hold what `expected` asks for? A compound may hold more
// than is asked; with `partial_lists` a list may too, in any order (and an empty list asks for
// an empty one). No `expected` asks for nothing.
bool matches(Tag expected, Tag actual, bool partial_lists);

// CompoundTag.merge: `from` put over `into`, entry by entry - compounds that both have are
// merged in turn, anything else is replaced. Appends the payload of the resulting compound.
void merge(Tag into, Tag from, base::ByteBuf& out);

// String.length() of a string of a Doc: UTF-16 units.
u32 utf16_length(std::string_view mutf8);

// The entries of a compound in the order CompoundTag.keySet() hands them out: that of a
// HashMap<String, Tag> which was put the entries in the order they stand here - bucket by
// bucket of String.hashCode, a bucket in the order of arrival. (A bucket of eight and more
// becomes a tree in Java and may come out otherwise; no tag of vanilla's has such names.)
void map_order(Tag compound, std::vector<Tag>& out);

// NbtPathArgument.NbtPath, read: a list of nodes, each naming some of the tags the one before
// it found.
struct Path {
    struct Node {
        enum Kind : u8 {
            kChild,         // name          the entry of that name
            kMatchObject,   // name{...}     ...if it holds what the pattern asks
            kMatchRoot,     // {...}         the tag itself, if it does (first node only)
            kIndex,         // [3], [-1]     an element of a list or an array, from the end if negative
            kAll,           // []            all of them
            kMatchElement,  // [{...}]       the elements of a list that hold what the pattern asks
        } kind = kChild;
        std::string name;  // modified UTF-8
        i32 index = 0;
        std::vector<u8> pattern;  // a compound, in the network form
        u32 end = 0;              // where the node ends in the path as typed
    };
    std::string text;  // as typed (NbtPath.asString)
    std::vector<Node> nodes;
};

// NbtPath.get: the tags `path` names under `root`, in order. False if a node found nothing:
// `missing` is then the beginning of the path up to and with that node - what
// "arguments.nbtpath.nothing_found" names. The tags are views into root's Doc.
bool find(const Path& path, Tag root, std::vector<Tag>& out, std::string_view& missing);

}  // namespace nbt
