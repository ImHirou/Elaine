// SPDX-License-Identifier: GPL-3.0-or-later
#include "nbt/snbt.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "base/java_number.hpp"
#include "proto/wire.hpp"

namespace nbt {

namespace {

constexpr u32 kMaxDepth = 512;  // ours: what NbtAccounter lets a tag nest when it is read back

// ---------------------------------------------------------------------------- the reader
//
// Each function below is one rule of SnbtGrammar.createParser and is named after it. The terms
// of a rule are the statements of its function, and they restore the cursor where the terms of
// the packrat parser do: a Sequence that fails goes back to where it began, an Alternative goes
// back before each element it tries, an optional term goes back if it did not match. A "cut"
// (Term.cut) is a flag of the alternative it stands in: once passed, the alternatives after it
// are not tried. The parser's cache of results is left out - it changes neither a result nor
// which error was stored first at a cursor.

enum : u8 { kNoSuffix, kFloatSuffix, kDoubleSuffix, kByteSuffix, kShortSuffix, kIntSuffix, kLongSuffix };  // TypeSuffix
enum : u8 { kDefaultSign, kSigned, kUnsigned };                                                            // SignedPrefix
enum : u8 { kBinary, kDecimal, kHex };                                                                     // Base

struct IntegerLiteral {
    bool minus = false;
    u8 base = kDecimal;
    std::string_view digits;
    u8 signedness = kDefaultSign;  // IntegerSuffix
    u8 type = kNoSuffix;
};

// A tag as it is being read. Strings and keys are modified UTF-8 already.
struct Val {
    Type type = kEnd;
    i64 integer = 0;           // kByte .. kLong
    double real = 0;           // kFloat, kDouble
    std::string text;          // kString
    std::vector<i64> numbers;  // the arrays
    std::vector<Val> items;    // kList; the values of a kCompound
    std::vector<std::string> keys;
};

bool whitespace(char c) {  // Character.isWhitespace, of what a command line is made of
    return c == ' ' || (c >= '\t' && c <= '\r') || (c >= 0x1C && c <= 0x1F);
}
bool unquoted_char(char c) {  // StringReader.isAllowedInUnquotedString
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '-' || c == '.' ||
           c == '+';
}
bool can_start_number(char c) { return (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.'; }
bool hex_digit(char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'); }
u32 digit_of(char c) { return c <= '9' ? static_cast<u32>(c - '0') : static_cast<u32>((c | 0x20) - 'a') + 10; }
bool equals_ignore_case(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
                                              [](char x, char y) { return (x | 0x20) == (y | 0x20) && (x | 0x20) >= 'a'; });
}

void append_code_point(std::string& out, u32 cp) {  // UTF-8; half of a surrogate pair as its three bytes
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | cp >> 6);
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | cp >> 12);
        out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | cp >> 18);
        out += static_cast<char>(0x80 | (cp >> 12 & 0x3F));
        out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// UUID.fromString: five groups of hex digits, at most 36 characters in all.
bool parse_uuid(std::string_view s, u64& hi, u64& lo) {
    if (s.size() > 36) return false;
    u64 part[5];
    for (u32 n = 0; n < 5; ++n) {
        const usize dash = s.find('-');
        if ((dash == std::string_view::npos) != (n == 4)) return false;
        std::string_view group = s.substr(0, dash);
        if (group.starts_with('+')) group.remove_prefix(1);  // Long.parseLong takes a sign
        const auto [end, ec] = std::from_chars(group.data(), group.data() + group.size(), part[n], 16);
        if (group.empty() || ec != std::errc() || end != group.data() + group.size() || part[n] >> 63) return false;
        if (n < 4) s = s.substr(dash + 1);
    }
    hi = (part[0] & 0xFFFFFFFFull) << 32 | (part[1] & 0xFFFFull) << 16 | (part[2] & 0xFFFFull);
    lo = (part[3] & 0xFFFFull) << 48 | (part[4] & 0xFFFFFFFFFFFFull);
    return true;
}

struct Parser {
    std::string_view s;
    u32 pos = 0;
    // ErrorCollector.LongestOnly: the errors of the furthest cursor win, and of those the
    // first that was stored is the one thrown (Grammar.parseForCommands).
    i64 far = -1;
    bool have = false;
    std::string_view far_key;
    std::string far_arg;
    bool far_has_arg = false;
    u32 depth = 0;
    const char* refused = nullptr;  // a limit of our own was passed: nothing parses any more

    void store(u32 cursor, std::string_view key, std::string arg = {}, bool has_arg = false) {
        if (static_cast<i64>(cursor) > far) {
            far = cursor;
            have = false;
        }
        if (static_cast<i64>(cursor) == far && !have) {
            have = true;
            far_key = key;
            far_arg = std::move(arg);
            far_has_arg = has_arg;
        }
    }
    void store_arg(u32 cursor, std::string_view key, std::string arg) { store(cursor, key, std::move(arg), true); }

    void skip_whitespace() {
        while (pos < s.size() && whitespace(s[pos])) ++pos;
    }

    // StringReaderTerms.character / characters: the character is read even when it is not the
    // one - whoever tried goes back.
    bool ch(char c) {
        skip_whitespace();
        const u32 cursor = pos;
        if (pos < s.size() && s[pos++] == c) return true;
        store_arg(cursor, "argument.literal.incorrect", std::string(1, c));
        return false;
    }
    bool ch(char a, char b) {
        skip_whitespace();
        const u32 cursor = pos;
        if (pos < s.size()) {
            const char c = s[pos++];
            if (c == a || c == b) return true;
        }
        store_arg(cursor, "argument.literal.incorrect", std::string{a, '|', b});
        return false;
    }
    // Term.positiveLookahead of a character test: nothing is read, and nothing stored if it fails
    // (CachedParseState.Silent).
    template <class F>
    bool ahead(F accepted) {
        const u32 mark = pos;
        skip_whitespace();
        const bool yes = pos < s.size() && accepted(s[pos]);
        pos = mark;
        return yes;
    }

    // ---- numbers

    bool sign(bool& minus) {
        const u32 mark = pos;
        if (ch('+')) {
            minus = false;
            return true;
        }
        pos = mark;
        if (ch('-')) {
            minus = true;
            return true;
        }
        pos = mark;
        return false;
    }
    void optional_sign(bool& minus) {
        const u32 mark = pos;
        minus = false;
        if (!sign(minus)) pos = mark;
    }

    bool integer_type(u8& type) {
        const u32 mark = pos;
        static constexpr struct {
            char lower, upper;
            u8 type;
        } kTypes[] = {{'b', 'B', kByteSuffix}, {'s', 'S', kShortSuffix}, {'i', 'I', kIntSuffix}, {'l', 'L', kLongSuffix}};
        for (const auto& t : kTypes) {
            if (ch(t.lower, t.upper)) {
                type = t.type;
                return true;
            }
            pos = mark;
        }
        return false;
    }
    bool integer_suffix(u8& signedness, u8& type) {
        const u32 mark = pos;
        if (ch('u', 'U') && integer_type(type)) {
            signedness = kUnsigned;
            return true;
        }
        pos = mark;
        if (ch('s', 'S') && integer_type(type)) {
            signedness = kSigned;
            return true;
        }
        pos = mark;
        signedness = kDefaultSign;
        return integer_type(type);
    }

    // NumberRunParseRule: binary_numeral, decimal_numeral, hex_numeral.
    bool numeral(u8 base, std::string_view& out) {
        skip_whitespace();
        const u32 start = pos;
        u32 end = start;
        const auto accepted = [base](char c) {
            if (c == '_') return true;
            return base == kBinary ? c == '0' || c == '1' : base == kDecimal ? c >= '0' && c <= '9' : hex_digit(c);
        };
        while (end < s.size() && accepted(s[end])) ++end;
        if (end == start) {
            store(pos, base == kBinary    ? "snbt.parser.expected_binary_numeral"
                       : base == kDecimal ? "snbt.parser.expected_decimal_numeral"
                                          : "snbt.parser.expected_hex_numeral");
            return false;
        }
        if (s[start] == '_' || s[end - 1] == '_') {
            // The key vanilla's code names; en_us spells it "undescore", so a client shows the key.
            store(pos, "snbt.parser.underscore_not_allowed");
            return false;
        }
        pos = end;
        out = s.substr(start, end - start);
        return true;
    }

    bool integer_literal(IntegerLiteral& out) {
        const u32 mark = pos;
        optional_sign(out.minus);
        const u32 outer = pos;
        bool ok = false, cut_outer = false;
        if (ch('0')) {
            cut_outer = true;
            const u32 inner = pos;
            bool cut_inner = false;
            if (ch('x', 'X')) {
                cut_inner = true;
                if (numeral(kHex, out.digits)) {
                    out.base = kHex;
                    ok = true;
                }
            }
            if (!ok && !cut_inner) {
                pos = inner;
                if (ch('b', 'B') && numeral(kBinary, out.digits)) {
                    out.base = kBinary;
                    ok = true;
                }
            }
            if (!ok && !cut_inner) {
                pos = inner;
                std::string_view more;
                if (numeral(kDecimal, more)) {
                    cut_inner = true;
                    store(pos, "snbt.parser.leading_zero_not_allowed");
                }
            }
            if (!ok && !cut_inner) {
                pos = inner;
                out.digits = "0";
                out.base = kDecimal;
                ok = true;
            }
        }
        if (!ok && !cut_outer) {
            pos = outer;
            if (numeral(kDecimal, out.digits)) {
                out.base = kDecimal;
                ok = true;
            }
        }
        if (!ok) {
            pos = mark;
            return false;
        }
        const u32 before_suffix = pos;
        if (!integer_suffix(out.signedness, out.type)) {
            pos = before_suffix;
            out.signedness = kDefaultSign;
            out.type = kNoSuffix;
        }
        return true;
    }

    // IntegerLiteral.create: the number as the type it was written to be, or the reason Java's
    // parser gave for refusing it.
    bool create_integer(const IntegerLiteral& lit, u8 type, i64& out) {
        const bool is_signed = lit.signedness != kDefaultSign ? lit.signedness == kSigned : lit.base == kDecimal;
        if (!is_signed && lit.minus) {
            store(pos, "snbt.parser.expected_non_negative_number");
            return false;
        }
        std::string digits = lit.minus ? "-" : "";
        u64 magnitude = 0;
        bool over = false;  // more than 64 bits
        const u32 radix = lit.base == kBinary ? 2 : lit.base == kDecimal ? 10 : 16;
        for (const char c : lit.digits) {
            if (c == '_') continue;
            digits += c;
            const u64 d = digit_of(c);
            over |= magnitude > (UINT64_MAX - d) / radix;
            magnitude = magnitude * radix + d;
        }
        const auto failure = [&](std::string message) {
            store_arg(pos, "snbt.parser.number_parse_failure", std::move(message));
            return false;
        };
        // NumberFormatException.forInputString
        const auto for_input = [&] {
            return failure("For input string: \"" + digits + "\"" + (radix == 10 ? "" : " under radix " + std::to_string(radix)));
        };
        const auto out_of_range = [&] {  // Byte.parseByte, Short.parseShort
            return failure("Value out of range. Value:\"" + digits + "\" Radix:" + std::to_string(radix));
        };
        // What `limit` bits of a signed integer hold.
        const auto fits = [&](u32 bits) { return !over && magnitude <= (1ull << (bits - 1)) - (lit.minus ? 0 : 1); };
        const i64 value = lit.minus ? static_cast<i64>(0 - magnitude) : static_cast<i64>(magnitude);
        if (is_signed) {
            switch (type) {
                case kByteSuffix:
                case kShortSuffix:
                    if (!fits(32)) return for_input();  // both go through Integer.parseInt
                    if (!fits(type == kByteSuffix ? 8 : 16)) return out_of_range();
                    break;
                case kIntSuffix:
                    if (!fits(32)) return for_input();
                    break;
                case kLongSuffix:
                    if (!fits(64)) return for_input();
                    break;
                default: store(pos, "snbt.parser.expected_integer_type"); return false;
            }
            out = value;
            return true;
        }
        switch (type) {
            case kByteSuffix:  // UnsignedBytes.parseUnsignedByte
            case kShortSuffix: {
                if (!fits(32)) return for_input();
                const u32 bits = type == kByteSuffix ? 8 : 16;
                if (magnitude >> bits) return failure("out of range: " + std::to_string(magnitude));
                out = type == kByteSuffix ? static_cast<i8>(magnitude) : static_cast<i16>(magnitude);
                return true;
            }
            case kIntSuffix:  // Integer.parseUnsignedInt
                if (over || magnitude >> 32) return failure("String value " + digits + " exceeds range of unsigned int.");
                out = static_cast<i32>(magnitude);
                return true;
            case kLongSuffix:  // Long.parseUnsignedLong
                if (over) return failure("String value " + digits + " exceeds range of unsigned long.");
                out = static_cast<i64>(magnitude);
                return true;
            default: store(pos, "snbt.parser.expected_integer_type"); return false;
        }
    }

    bool float_type_suffix(u8& type) {
        const u32 mark = pos;
        if (ch('f', 'F')) {
            type = kFloatSuffix;
            return true;
        }
        pos = mark;
        if (ch('d', 'D')) {
            type = kDoubleSuffix;
            return true;
        }
        pos = mark;
        return false;
    }
    struct Exponent {
        bool minus = false;
        std::string_view digits;
    };
    bool float_exponent_part(Exponent& out) {
        if (!ch('e', 'E')) return false;
        optional_sign(out.minus);
        return numeral(kDecimal, out.digits);
    }

    bool float_literal(Val& out) {
        const u32 mark = pos;
        bool minus = false;
        optional_sign(minus);
        const u32 start = pos;
        std::string_view whole, fraction;
        Exponent exponent;
        bool has_whole = false, has_fraction = false, has_exponent = false, cut = false, ok = false;
        u8 suffix = kNoSuffix;
        const auto optional_exponent = [&] {
            const u32 m = pos;
            has_exponent = float_exponent_part(exponent);
            if (!has_exponent) pos = m;
        };
        const auto optional_suffix = [&] {
            const u32 m = pos;
            if (!float_type_suffix(suffix)) {
                pos = m;
                suffix = kNoSuffix;
            }
        };
        const auto again = [&] {
            pos = start;
            has_whole = has_fraction = has_exponent = false;
            suffix = kNoSuffix;
        };
        // 1.  1.5  1.5e3f
        if (numeral(kDecimal, whole) && ch('.')) {
            has_whole = cut = ok = true;
            const u32 m = pos;
            has_fraction = numeral(kDecimal, fraction);
            if (!has_fraction) pos = m;
            optional_exponent();
            optional_suffix();
        }
        // .5
        if (!ok && !cut) {
            again();
            if (ch('.')) {
                cut = true;
                if (numeral(kDecimal, fraction)) {
                    has_fraction = ok = true;
                    optional_exponent();
                    optional_suffix();
                }
            }
        }
        // 1e3
        if (!ok && !cut) {
            again();
            if (numeral(kDecimal, whole) && float_exponent_part(exponent)) {
                has_whole = has_exponent = cut = ok = true;
                optional_suffix();
            }
        }
        // 1f  1e3d
        if (!ok && !cut) {
            again();
            if (numeral(kDecimal, whole)) {
                has_whole = true;
                optional_exponent();
                ok = float_type_suffix(suffix);
            }
        }
        if (!ok) {
            pos = mark;
            return false;
        }
        // createFloat: the text Java's parser is handed.
        std::string text = minus ? "-" : "";
        const auto clean = [&](std::string_view digits) {
            for (const char c : digits)
                if (c != '_') text += c;
        };
        if (has_whole) clean(whole);
        if (has_fraction) {
            if (!has_whole) text += '0';  // from_chars wants a digit before the point
            text += '.';
            clean(fraction);
            if (text.back() == '.') text += '0';
        }
        if (has_exponent) {
            text += 'e';
            if (exponent.minus) text += '-';
            clean(exponent.digits);
        }
        if (suffix == kFloatSuffix) {
            float v = 0;
            const auto r = std::from_chars(text.data(), text.data() + text.size(), v);
            if (r.ec == std::errc::result_out_of_range) v = std::strtof(text.c_str(), nullptr);  // too small is 0, not an error
            if (!std::isfinite(v)) {
                store(pos, "snbt.parser.infinity_not_allowed");
                return false;
            }
            out.type = kFloat;
            out.real = v;
        } else {
            double v = 0;
            const auto r = std::from_chars(text.data(), text.data() + text.size(), v);
            if (r.ec == std::errc::result_out_of_range) v = std::strtod(text.c_str(), nullptr);
            if (!std::isfinite(v)) {
                store(pos, "snbt.parser.infinity_not_allowed");
                return false;
            }
            out.type = kDouble;
            out.real = v;
        }
        return true;
    }

    // ---- strings

    // SimpleHexLiteralParseRule: exactly `n` hex digits.
    bool string_hex(u32 n, std::string_view& out) {
        u32 end = pos;
        while (end < s.size() && hex_digit(s[end]) && end - pos < n) ++end;
        if (end - pos < n) {
            store_arg(pos, "snbt.parser.expected_hex_escape", std::to_string(n));
            return false;
        }
        out = s.substr(pos, n);
        pos = end;
        return true;
    }

    bool string_escape_sequence(std::string& out) {
        const u32 mark = pos;
        static constexpr struct {
            char letter, value;
        } kPlain[] = {{'b', '\b'}, {'s', ' '},   {'t', '\t'},  {'n', '\n'}, {'f', '\f'},
                      {'r', '\r'}, {'\\', '\\'}, {'\'', '\''}, {'"', '"'}};
        for (const auto& e : kPlain) {
            if (ch(e.letter)) {
                out += e.value;
                return true;
            }
            pos = mark;
        }
        static constexpr struct {
            char letter;
            u32 digits;
        } kHexEscapes[] = {{'x', 2}, {'u', 4}, {'U', 8}};
        for (const auto& e : kHexEscapes) {
            std::string_view hex;
            if (ch(e.letter) && string_hex(e.digits, hex)) {
                u32 cp = 0;
                for (const char c : hex) cp = cp << 4 | digit_of(c);
                if (cp > 0x10FFFF) {  // Character.isValidCodePoint
                    char name[16];
                    std::snprintf(name, sizeof name, "U+%08X", cp);
                    store_arg(pos, "snbt.parser.invalid_codepoint", name);
                    return false;
                }
                append_code_point(out, cp);
                return true;
            }
            pos = mark;
        }
        // \N{LATIN SMALL LETTER A}: Character.codePointOf - Java's table of names, which is not
        // here. No name is known.
        if (ch('N') && ch('{')) {
            u32 end = pos;
            const auto in_name = [](char c) {
                return c == '-' || c == ' ' || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
            };
            while (end < s.size() && in_name(s[end])) ++end;
            if (end == pos) {
                store(pos, "snbt.parser.invalid_character_name");
            } else {
                pos = end;
                if (ch('}')) {
                    store(pos, "snbt.parser.invalid_character_name");
                    return false;
                }
            }
        }
        pos = mark;
        return false;
    }

    // single_quoted_string_chunk / double_quoted_string_chunk: text, an escape, or the quote
    // of the other kind.
    bool string_chunk(char other_quote, std::string& out) {
        const u32 mark = pos;
        u32 end = pos;  // string_plain_contents
        while (end < s.size() && s[end] != '"' && s[end] != '\'' && s[end] != '\\') ++end;
        if (end > pos) {
            out.append(s.substr(pos, end - pos));
            pos = end;
            return true;
        }
        store(pos, "snbt.parser.invalid_string_contents");
        std::string escaped;
        if (ch('\\') && string_escape_sequence(escaped)) {
            out += escaped;
            return true;
        }
        pos = mark;
        if (ch(other_quote)) {
            out += other_quote;
            return true;
        }
        pos = mark;
        return false;
    }
    void string_contents(char other_quote, std::string& out) {  // Term.repeated
        for (;;) {
            const u32 mark = pos;
            if (!string_chunk(other_quote, out)) {
                pos = mark;
                return;
            }
        }
    }

    bool quoted_string_literal(std::string& out) {
        const u32 mark = pos;
        out.clear();
        if (ch('"')) {  // and a cut: what begins with a double quote is not tried as the other kind
            string_contents('\'', out);
            if (ch('"')) return true;
            pos = mark;
            return false;
        }
        pos = mark;
        if (ch('\'')) {
            string_contents('"', out);
            if (ch('\'')) return true;
        }
        pos = mark;
        return false;
    }

    bool unquoted_string(std::string_view& out) {  // UnquotedStringParseRule(1)
        skip_whitespace();
        const u32 cursor = pos;
        while (pos < s.size() && unquoted_char(s[pos])) ++pos;
        if (pos == cursor) {
            store(cursor, "snbt.parser.expected_unquoted_string");
            return false;
        }
        out = s.substr(cursor, pos - cursor);
        return true;
    }

    // A Java string as the bytes NBT holds; false (and nothing parses any more) if it is one
    // DataOutput.writeUTF would not write.
    bool to_mutf8(std::string_view utf8, std::string& out) {
        out.resize(proto::mutf8_bound(utf8));
        u8* const begin = reinterpret_cast<u8*>(out.data());
        out.resize(static_cast<usize>(proto::put_mutf8(begin, utf8) - begin));
        if (out.size() <= kMaxStringBytes) return true;
        refused = "A string in a tag is too long";
        return false;
    }
    bool make_string(std::string_view utf8, Val& out) {
        out.type = kString;
        return to_mutf8(utf8, out.text);
    }

    // ---- what holds other tags

    // Term.repeatedWithTrailingSeparator(literal, ','): list_entries, arguments.
    void literals(std::vector<Val>& out) {
        for (bool first = true;; first = false) {
            const u32 before_separator = pos;
            if (!first && !ch(',')) {
                pos = before_separator;
                return;
            }
            const u32 after_separator = pos;
            Val v;
            if (!literal(v)) {
                pos = after_separator;
                return;
            }
            out.push_back(std::move(v));
        }
    }

    bool map_key(std::string& out) {
        const u32 mark = pos;
        if (quoted_string_literal(out)) return true;
        pos = mark;
        std::string_view word;
        if (unquoted_string(word)) {
            out.assign(word);
            return true;
        }
        pos = mark;
        return false;
    }
    bool map_entry(std::string& key, Val& value) {
        if (!map_key(key) || !ch(':') || !literal(value)) return false;
        if (key.empty()) {
            store(pos, "snbt.parser.empty_key");
            return false;
        }
        return true;
    }
    bool map_literal(Val& out) {
        if (!ch('{')) return false;
        out.type = kCompound;
        for (bool first = true;; first = false) {  // map_entries
            const u32 before_separator = pos;
            if (!first && !ch(',')) {
                pos = before_separator;
                break;
            }
            const u32 after_separator = pos;
            std::string key, name;
            Val value;
            if (!map_entry(key, value)) {
                pos = after_separator;
                break;
            }
            if (!to_mutf8(key, name)) return false;
            // ImmutableMap.Builder.buildKeepingLast
            const auto same = std::find(out.keys.begin(), out.keys.end(), name);
            if (same != out.keys.end()) {
                out.items[static_cast<usize>(same - out.keys.begin())] = std::move(value);
            } else {
                out.keys.push_back(std::move(name));
                out.items.push_back(std::move(value));
            }
        }
        return ch('}');
    }

    bool list_literal(Val& out) {
        if (!ch('[')) return false;
        const u32 mark = pos;
        // [B; 1, 2]: array_prefix, ';', int_array_entries
        static constexpr struct {
            char letter;
            Type type;
        } kPrefixes[] = {{'B', kByteArray}, {'L', kLongArray}, {'I', kIntArray}};
        Type array = kEnd;
        for (const auto& p : kPrefixes) {
            if (ch(p.letter)) {
                array = p.type;
                break;
            }
            pos = mark;
        }
        std::vector<IntegerLiteral> numbers;
        std::vector<Val> items;
        if (array != kEnd && ch(';')) {
            for (bool first = true;; first = false) {
                const u32 before_separator = pos;
                if (!first && !ch(',')) {
                    pos = before_separator;
                    break;
                }
                const u32 after_separator = pos;
                IntegerLiteral n;
                if (!integer_literal(n)) {
                    pos = after_separator;
                    break;
                }
                numbers.push_back(n);
            }
        } else {
            array = kEnd;
            pos = mark;
            literals(items);
        }
        if (!ch(']')) return false;
        if (array == kEnd) {
            out.type = kList;
            out.items = std::move(items);
            return true;
        }
        // ArrayPrefix.create: an element is of the array's type unless it names a smaller one.
        const u8 own = array == kByteArray ? kByteSuffix : array == kIntArray ? kIntSuffix : kLongSuffix;
        out.type = array;
        for (const IntegerLiteral& n : numbers) {
            const u8 type = n.type == kNoSuffix ? own : n.type;
            if (type > own) {  // TypeSuffix orders them: byte, short, int, long
                store(pos, "snbt.parser.invalid_array_element_type");
                return false;
            }
            i64 v = 0;
            if (!create_integer(n, type, v)) return false;
            out.numbers.push_back(v);
        }
        return true;
    }

    bool unquoted_string_or_builtin(Val& out) {
        std::string_view word;
        if (!unquoted_string(word)) return false;
        bool call = false;
        std::vector<Val> arguments;
        {
            const u32 mark = pos;
            if (ch('(')) {
                literals(arguments);
                call = ch(')');
            }
            if (!call) {
                pos = mark;
                arguments.clear();
            }
        }
        if (can_start_number(word[0])) {
            store(pos, "snbt.parser.invalid_unquoted_start");
            return false;
        }
        if (call) {  // SnbtOperations.BUILTIN_OPERATIONS
            if (word == "bool" && arguments.size() == 1) {
                const Val& a = arguments[0];
                bool value;
                if (a.type >= kByte && a.type <= kLong) {  // Number.byteValue() != 0
                    value = (a.integer & 0xFF) != 0;
                } else if (a.type == kFloat || a.type == kDouble) {
                    const double d = a.real;
                    const i32 whole = d != d               ? 0
                                      : d >= 2147483647.0  ? INT32_MAX
                                      : d <= -2147483648.0 ? INT32_MIN
                                                           : static_cast<i32>(d);
                    value = (whole & 0xFF) != 0;
                } else {
                    store(pos, "snbt.parser.expected_number_or_boolean");
                    return false;
                }
                out.type = kByte;
                out.integer = value;
                return true;
            }
            if (word == "uuid" && arguments.size() == 1) {
                u64 hi = 0, lo = 0;
                if (arguments[0].type != kString || !parse_uuid(to_utf8(arguments[0].text), hi, lo)) {
                    store(pos, "snbt.parser.expected_string_uuid");
                    return false;
                }
                out.type = kIntArray;  // UUIDUtil.uuidToIntArray
                out.numbers = {static_cast<i32>(hi >> 32), static_cast<i32>(hi), static_cast<i32>(lo >> 32),
                               static_cast<i32>(lo)};
                return true;
            }
            store_arg(pos, "snbt.parser.no_such_operation", std::string(word) + "/" + std::to_string(arguments.size()));
            return false;
        }
        if (equals_ignore_case(word, "true") || equals_ignore_case(word, "false")) {
            out.type = kByte;
            out.integer = (word[0] | 0x20) == 't';
            return true;
        }
        return make_string(word, out);
    }

    bool literal(Val& out) {
        if (refused) return false;
        if (depth >= kMaxDepth) {
            refused = "A tag is nested too deeply";
            return false;
        }
        ++depth;
        const bool ok = literal_of(out);
        --depth;
        return ok && !refused;
    }
    bool literal_of(Val& out) {
        const u32 mark = pos;
        if (ahead(can_start_number)) {
            if (float_literal(out)) return true;
            pos = mark;
            IntegerLiteral n;
            if (integer_literal(n)) {
                // The number is made when the literal is, of the type it names - an int otherwise.
                const u8 type = n.type == kNoSuffix ? static_cast<u8>(kIntSuffix) : n.type;
                if (!create_integer(n, type, out.integer)) return false;
                out.type = type == kByteSuffix ? kByte : type == kShortSuffix ? kShort : type == kIntSuffix ? kInt : kLong;
                return true;
            }
        }
        pos = mark;
        if (ahead([](char c) { return c == '"' || c == '\''; })) {
            std::string text;
            if (quoted_string_literal(text)) return make_string(text, out);
            pos = mark;
            return false;
        }
        if (ahead([](char c) { return c == '{'; })) {
            if (map_literal(out)) return true;
            pos = mark;
            return false;
        }
        if (ahead([](char c) { return c == '['; })) {
            if (list_literal(out)) return true;
            pos = mark;
            return false;
        }
        if (unquoted_string_or_builtin(out)) return true;
        pos = mark;
        return false;
    }
};

// ---------------------------------------------------------------------------- a read tag as bytes

bool wrapper(const Val& v) { return v.type == kCompound && v.keys.size() == 1 && v.keys[0].empty(); }

void emit(const Val& v, Writer& w) {
    switch (v.type) {
        case kByte: w.put_byte(static_cast<i8>(v.integer)); break;
        case kShort: w.put_short(static_cast<i16>(v.integer)); break;
        case kInt: w.put_int(static_cast<i32>(v.integer)); break;
        case kLong: w.put_long(v.integer); break;
        case kFloat: w.put_float(static_cast<float>(v.real)); break;
        case kDouble: w.put_double(v.real); break;
        case kString: w.put_string_raw(v.text); break;
        case kByteArray:
            w.put_int(static_cast<i32>(v.numbers.size()));
            for (const i64 n : v.numbers) w.put_byte(static_cast<i8>(n));
            break;
        case kIntArray:
            w.put_int(static_cast<i32>(v.numbers.size()));
            for (const i64 n : v.numbers) w.put_int(static_cast<i32>(n));
            break;
        case kLongArray:
            w.put_int(static_cast<i32>(v.numbers.size()));
            for (const i64 n : v.numbers) w.put_long(n);
            break;
        case kList: {  // ListTag.write: one element type, or compounds with the others wrapped
            Type of = kEnd;
            for (const Val& e : v.items) {
                if (of == kEnd) {
                    of = e.type;
                } else if (of != e.type) {
                    of = kCompound;
                    break;
                }
            }
            w.begin_list(of, static_cast<u32>(v.items.size()));
            for (const Val& e : v.items) {
                if (of == kCompound && (e.type != kCompound || wrapper(e))) {
                    w.entry(e.type, "");
                    emit(e, w);
                    w.end_compound();
                } else {
                    emit(e, w);
                }
            }
            break;
        }
        case kCompound:
            for (usize i = 0; i < v.items.size(); ++i) {
                w.entry(v.items[i].type, v.keys[i]);
                emit(v.items[i], w);
            }
            w.end_compound();
            break;
        default: break;
    }
}

bool read(std::string_view text, u32& cursor, bool compound, base::ByteBuf& out, SnbtError& err) {
    Parser p;
    p.s = text;
    p.pos = cursor;
    Val v;
    if (!p.literal(v)) {
        if (p.refused) {
            err = {{}, p.refused, false, cursor};
        } else if (p.have) {
            err = {p.far_key, std::move(p.far_arg), p.far_has_arg, static_cast<u32>(p.far)};
        } else {
            err = {{}, "This is not a tag", false, cursor};  // vanilla throws nothing it can show
        }
        return false;
    }
    cursor = p.pos;
    if (compound && v.type != kCompound) {
        err = {"argument.nbt.expected.compound", {}, false, cursor};
        return false;
    }
    Writer w(out);
    w.root(v.type);
    emit(v, w);
    return true;
}

}  // namespace

bool read_snbt(std::string_view text, u32& cursor, base::ByteBuf& out, SnbtError& err) {
    return read(text, cursor, false, out, err);
}
bool read_snbt_compound(std::string_view text, u32& cursor, base::ByteBuf& out, SnbtError& err) {
    return read(text, cursor, true, out, err);
}
bool read_snbt_fully(std::string_view text, base::ByteBuf& out, SnbtError& err) {
    u32 cursor = 0;
    const u32 mark = out.len;
    if (!read(text, cursor, false, out, err)) return false;
    while (cursor < text.size() && whitespace(text[cursor])) ++cursor;
    if (cursor < text.size()) {
        out.len = mark;
        err = {"argument.nbt.trailing", {}, false, cursor};
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------- printing

void append_utf8(std::string& out, std::string_view mutf8) {
    const u8* p = reinterpret_cast<const u8*>(mutf8.data());
    const u8* const end = p + mutf8.size();
    const auto surrogate = [end](const u8* q, u32 lead) {  // ED A0..AF xx / ED B0..BF xx
        return end - q >= 3 && q[0] == 0xED && (q[1] & 0xF0) == lead;
    };
    while (p < end) {
        if (p[0] == 0xC0 && end - p >= 2 && p[1] == 0x80) {
            out += '\0';
            p += 2;
        } else if (surrogate(p, 0xA0) && surrogate(p + 3, 0xB0)) {
            const u32 hi = static_cast<u32>(p[1] & 0x0F) << 6 | (p[2] & 0x3F);
            const u32 lo = static_cast<u32>(p[4] & 0x0F) << 6 | (p[5] & 0x3F);
            append_code_point(out, 0x10000 + (hi << 10 | lo));
            p += 6;
        } else {
            out += static_cast<char>(*p++);
        }
    }
}

namespace {

// SnbtGrammar.escapeControlCharacters
bool control_escape(char c, std::string& out) {
    switch (c) {
        case '\b': out += "\\b"; return true;
        case '\t': out += "\\t"; return true;
        case '\n': out += "\\n"; return true;
        case '\f': out += "\\f"; return true;
        case '\r': out += "\\r"; return true;
        default: break;
    }
    if (static_cast<u8>(c) >= 0x20) return false;
    static constexpr char kHex[] = "0123456789ABCDEF";
    out += "\\x";
    out += kHex[static_cast<u8>(c) >> 4];
    out += kHex[c & 15];
    return true;
}

// StringTagVisitor.handleKeyEscape: "[A-Za-z._]+[A-Za-z0-9._+-]*", and neither true nor false.
bool plain_key(std::string_view k) {
    if (k.empty() || equals_ignore_case(k, "true") || equals_ignore_case(k, "false")) return false;
    const auto letter = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '.' || c == '_'; };
    if (!letter(k[0])) return false;
    return std::all_of(k.begin(), k.end(), [&](char c) { return letter(c) || (c >= '0' && c <= '9') || c == '+' || c == '-'; });
}

void print(Tag tag, std::string& out, std::vector<Tag>& scratch) {
    switch (tag.type) {
        case kByte: out += std::to_string(tag.as_long()) + "b"; break;
        case kShort: out += std::to_string(tag.as_long()) + "s"; break;
        case kInt: out += std::to_string(tag.as_long()); break;
        case kLong: out += std::to_string(tag.as_long()) + "L"; break;
        case kFloat: out += base::java_float(static_cast<float>(tag.as_double())) + "f"; break;
        case kDouble: out += base::java_double(tag.as_double()) + "d"; break;
        case kString: quote_and_escape(to_utf8(tag.str()), out); break;
        case kByteArray:
        case kIntArray:
        case kLongArray: {
            out += tag.type == kByteArray ? "[B;" : tag.type == kIntArray ? "[I;" : "[L;";
            const char* const suffix = tag.type == kByteArray ? "B" : tag.type == kIntArray ? "" : "L";
            bool first = true;
            for (const Tag e : tag.elements()) {
                if (!first) out += ',';
                first = false;
                out += std::to_string(e.as_long());
                out += suffix;
            }
            out += ']';
            break;
        }
        case kList: {
            out += '[';
            bool first = true;
            for (const Tag e : tag.elements()) {
                if (!first) out += ',';
                first = false;
                print(e, out, scratch);
            }
            out += ']';
            break;
        }
        case kCompound: {
            // Sorted by name, one entry a name: the last, as a map keeps it.
            const usize mark = scratch.size();
            for (const Tag e : tag.entries()) scratch.push_back(e);
            std::stable_sort(scratch.begin() + static_cast<std::ptrdiff_t>(mark), scratch.end(),
                             [](Tag a, Tag b) { return a.name() < b.name(); });
            out += '{';
            bool first = true;
            for (usize i = mark; i < scratch.size(); ++i) {
                if (i + 1 < scratch.size() && scratch[i].name() == scratch[i + 1].name()) continue;
                const Tag e = scratch[i];
                if (!first) out += ',';
                first = false;
                const std::string key = to_utf8(e.name());
                if (plain_key(key)) {
                    out += key;
                } else {
                    quote_and_escape(key, out);
                }
                out += ':';
                print(e, out, scratch);
            }
            scratch.resize(mark);
            out += '}';
            break;
        }
        default: out += "END"; break;
    }
}

}  // namespace

void quote_and_escape(std::string_view text, std::string& out) {
    const usize quote_at = out.size();
    out += ' ';
    char quote = 0;
    for (const char c : text) {
        if (c == '\\') {
            out += "\\\\";
        } else if (c == '"' || c == '\'') {
            if (!quote) quote = c == '"' ? '\'' : '"';
            if (quote == c) out += '\\';
            out += c;
        } else if (!control_escape(c, out)) {
            out += c;
        }
    }
    if (!quote) quote = '"';
    out[quote_at] = quote;
    out += quote;
}

void print_snbt(Tag tag, std::string& out) {
    std::vector<Tag> scratch;
    print(tag, out, scratch);
}

}  // namespace nbt
