// SPDX-License-Identifier: GPL-3.0-or-later
// SNBT: NBT as text - what a command line holds between braces, and what `/data get` shows.
//
// The reader is a port of TagParser and the grammar behind it (SnbtGrammar, SnbtOperations,
// 26.3) rule by rule, backtracking and "cuts" included, because of what it answers when a tag
// is wrong: vanilla's parser keeps every reason it failed for at the furthest character it
// reached and reports the first of them (ErrorCollector.LongestOnly). Which reason comes first
// is an accident of the order the grammar tries things in - "{a:5" complains that it expected a
// "." - and a client shows that message under the line, so the order is kept.
//
// A tag that was read comes out as bytes in the network form (type, payload), ready for
// nbt::Doc. The printer is Tag.toString() (StringTagVisitor).
//
// Not vanilla's: "\N{...}", a character by its Unicode name, is never a name here (it would take
// Java's table of names); a tag nests at most 512 deep (vanilla's parser has no limit but its
// stack); a string or a key over 65535 bytes is refused when it is read, where vanilla fails when
// the tag is written.
#pragma once

#include <string>
#include <string_view>

#include "base/buf.hpp"
#include "nbt/nbt.hpp"

namespace nbt {

// CommandSyntaxException, as its parts: a translation key, the one argument it may take, and
// where in the text it points. An empty key: `arg` is the complaint in words of our own.
struct SnbtError {
    std::string_view key;
    std::string arg;
    bool has_arg = false;
    u32 cursor = 0;
};

// TagParser.parseAsArgument: reads one tag from text[cursor...] and leaves the cursor behind it;
// what follows is the caller's to judge. `out` is appended the tag: its type, then its payload.
bool read_snbt(std::string_view text, u32& cursor, base::ByteBuf& out, SnbtError& err);
// TagParser.parseCompoundAsArgument: the same, and the tag is a compound.
bool read_snbt_compound(std::string_view text, u32& cursor, base::ByteBuf& out, SnbtError& err);
// TagParser.parseFully: the whole text is one tag, whitespace after it aside.
bool read_snbt_fully(std::string_view text, base::ByteBuf& out, SnbtError& err);

// Tag.toString(): the tag in one line, a compound's entries sorted by name, as UTF-8.
void print_snbt(Tag tag, std::string& out);
// StringTag.quoteAndEscape: in double quotes - or single ones, if the first quote in it is a
// double one - with that quote, backslashes and control characters escaped.
void quote_and_escape(std::string_view utf8, std::string& out);

// A string of a Doc (Java's modified UTF-8) as UTF-8.
void append_utf8(std::string& out, std::string_view mutf8);
inline std::string to_utf8(std::string_view mutf8) {
    std::string s;
    append_utf8(s, mutf8);
    return s;
}

}  // namespace nbt
