// SPDX-License-Identifier: GPL-3.0-or-later
#include "text/nbt_text.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "base/java_number.hpp"
#include "nbt/ops.hpp"
#include "nbt/snbt.hpp"

namespace text {

namespace {

constexpr u32 kMaxDepth = 64;    // TextComponentTagVisitor.MAX_DEPTH
constexpr u32 kMaxLength = 128;  // MAX_LENGTH
constexpr u32 kInlineList = 8;   // INLINE_LIST_THRESHOLD

constexpr Color kKey = Color::kAqua, kString = Color::kGreen, kNumber = Color::kGold, kType = Color::kRed;

struct Visitor {
    Text result;  // Component.empty(): everything is appended to it, nothing nests
    u32 depth = 0;

    void plain(std::string_view s) { result.append(Text::literal(s)); }  // a Token without a style, a quote
    void styled(std::string_view s, Color c) { result.append(Text::literal(s).color(c)); }
    void folded() { styled("<...>", Color::kGray); }

    // handleEscapePretty: a name of "[A-Za-z0-9._+-]+" as it is, any other in quotes.
    void key(std::string_view name) {
        const bool simple = !name.empty() && std::all_of(name.begin(), name.end(), [](char c) {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                   c == '+' || c == '-';
        });
        if (simple) {
            styled(name, kKey);
            return;
        }
        std::string quoted;
        nbt::quote_and_escape(name, quoted);
        Text t = Text::literal(quoted.substr(0, 1));
        t.append(Text::literal(quoted.substr(1, quoted.size() - 2)).color(kKey));
        t.append(std::string_view(quoted).substr(0, 1));
        result.append(std::move(t));
    }

    void number(nbt::Tag tag) {
        switch (tag.type) {
            case nbt::kByte:
                styled(std::to_string(tag.as_long()), kNumber);
                styled("b", kType);
                break;
            case nbt::kShort:
                styled(std::to_string(tag.as_long()), kNumber);
                styled("s", kType);
                break;
            case nbt::kInt: styled(std::to_string(tag.as_long()), kNumber); break;
            case nbt::kLong:
                styled(std::to_string(tag.as_long()), kNumber);
                styled("L", kType);
                break;
            case nbt::kFloat:
                styled(base::java_float(static_cast<float>(tag.as_double())), kNumber);
                styled("f", kType);
                break;
            default:
                styled(base::java_double(tag.as_double()), kNumber);
                styled("d", kType);
                break;
        }
    }

    void sub(nbt::Tag tag) {  // appendSubTag
        ++depth;
        visit(tag);
        --depth;
    }

    void visit(nbt::Tag tag) {
        switch (tag.type) {
            case nbt::kString: {
                std::string quoted;
                nbt::quote_and_escape(nbt::to_utf8(tag.str()), quoted);
                const std::string_view quote = std::string_view(quoted).substr(0, 1);
                plain(quote);
                styled(std::string_view(quoted).substr(1, quoted.size() - 2), kString);
                plain(quote);
                break;
            }
            case nbt::kByteArray:
            case nbt::kIntArray:
            case nbt::kLongArray: {
                plain("[");
                styled(tag.type == nbt::kByteArray ? "B" : tag.type == nbt::kIntArray ? "I" : "L", kType);
                plain(";");
                const u32 n = tag.size();
                u32 i = 0;
                for (const nbt::Tag e : tag.elements()) {
                    if (i >= kMaxLength) break;
                    plain(" ");
                    styled(std::to_string(e.as_long()), kNumber);
                    if (tag.type == nbt::kByteArray) styled("B", kType);
                    if (tag.type == nbt::kLongArray) styled("L", kType);
                    if (i != n - 1) plain(",");
                    ++i;
                }
                if (n > kMaxLength) folded();
                plain("]");
                break;
            }
            case nbt::kList: {
                const u32 n = tag.size();
                plain("[");
                if (n == 0) {
                    plain("]");
                    break;
                }
                if (depth >= kMaxDepth) {
                    folded();
                    plain("]");
                    break;
                }
                // shouldWrapListElements: a short list that holds something other than numbers.
                bool wrap = false;
                if (n < kInlineList)
                    for (const nbt::Tag e : tag.elements()) wrap |= !e.numeric();
                u32 i = 0;
                if (!wrap) {
                    for (const nbt::Tag e : tag.elements()) {
                        if (i++ != 0) {
                            plain(",");
                            plain(" ");
                        }
                        sub(e);
                    }
                } else {
                    for (const nbt::Tag e : tag.elements()) {
                        if (i >= kMaxLength) break;
                        plain("");  // the indentation of an entry: none, and a component still
                        sub(e);
                        if (i != n - 1) {
                            plain(",");
                            plain(" ");
                        }
                        ++i;
                    }
                    if (n > kMaxLength) {
                        plain("");
                        folded();
                    }
                }
                plain("]");
                break;
            }
            case nbt::kCompound: {
                std::vector<nbt::Tag> entries;
                nbt::map_order(tag, entries);
                plain("{");
                if (entries.empty()) {
                    plain("}");
                    break;
                }
                if (depth >= kMaxDepth) {
                    folded();
                    plain("}");
                    break;
                }
                for (usize i = 0; i < entries.size(); ++i) {
                    plain("");
                    key(nbt::to_utf8(entries[i].name()));
                    plain(":");
                    plain(" ");
                    sub(entries[i]);
                    if (i + 1 != entries.size()) {
                        plain(",");
                        plain(" ");
                    }
                }
                plain("}");
                break;
            }
            case nbt::kEnd: break;
            default: number(tag); break;
        }
    }
};

}  // namespace

Text pretty_nbt(nbt::Tag tag) {
    Visitor v;
    v.visit(tag);
    return std::move(v.result);
}

}  // namespace text
