// SPDX-License-Identifier: GPL-3.0-or-later
#include "text/text.hpp"

#include <charconv>
#include <cmath>

#include "base/log.hpp"
#include "nbt/nbt.hpp"
#include "proto/gen/current.hpp"
#include "text/component.hpp"

#include PROTO_GEN_LANG_HPP

namespace text {

namespace {

// TextColor.NAMED_COLORS, in the order of Color.
constexpr std::string_view kColorNames[] = {"black", "dark_blue",    "dark_green", "dark_aqua", "dark_red", "dark_purple",
                                            "gold",  "gray",         "dark_gray",  "blue",      "green",    "aqua",
                                            "red",   "light_purple", "yellow",     "white"};

}  // namespace

// A Text as an NBT compound with its fields in no particular order and every list a list of
// compounds - a spelling ComponentSerialization.CODEC reads, which transcode() then writes out
// as vanilla would.
struct Loose {
    nbt::Writer w;
};

void Text::write(Loose& l) const {
    nbt::Writer& w = l.w;
    if (translate_) {
        w.put_string("translate", text_);
        if (!args_.empty()) {
            w.begin_list("with", nbt::kCompound, static_cast<u32>(args_.size()));
            for (const Arg& a : args_) {
                switch (a.kind) {  // a number or a string as {"": x}: how a mixed list carries it
                    case Arg::kString:
                        w.put_string("", a.s);
                        w.end_compound();
                        break;
                    case Arg::kInt:
                        w.put_int("", static_cast<i32>(a.n));
                        w.end_compound();
                        break;
                    case Arg::kLong:
                        w.put_long("", a.n);
                        w.end_compound();
                        break;
                    case Arg::kFloat:
                        w.put_float("", static_cast<float>(a.d));
                        w.end_compound();
                        break;
                    case Arg::kDouble:
                        w.put_double("", a.d);
                        w.end_compound();
                        break;
                    case Arg::kText: a.text[0].write(l); break;
                }
            }
        }
    } else {
        w.put_string("text", text_);
    }
    if (!extra_.empty()) {
        w.begin_list("extra", nbt::kCompound, static_cast<u32>(extra_.size()));
        for (const Text& child : extra_) child.write(l);
    }
    if (color_ != Color::kNone) w.put_string("color", kColorNames[static_cast<u32>(color_)]);
    if (italic_) w.put_byte("italic", 1);
    if (underlined_) w.put_byte("underlined", 1);
    if (has_click_) {
        w.begin_compound("click_event");
        w.put_string("action", "suggest_command");
        w.put_string("command", click_);
        w.end_compound();
    }
    if (has_insertion_) w.put_string("insertion", insertion_);
    if (hover_ == Hover::kEntity) {
        w.begin_compound("hover_event");
        w.put_string("action", "show_entity");
        w.put_string("id", hover_id_);
        const i32 parts[4] = {static_cast<i32>(hover_uuid_.hi >> 32), static_cast<i32>(hover_uuid_.hi),
                              static_cast<i32>(hover_uuid_.lo >> 32), static_cast<i32>(hover_uuid_.lo)};
        w.put_int_array("uuid", parts);
        w.begin_compound("name");
        hover_text_[0].write(l);
        w.end_compound();
    } else if (hover_ == Hover::kItem) {
        w.begin_compound("hover_event");
        w.put_string("action", "show_item");
        w.put_string("id", hover_id_);
        w.put_int("count", static_cast<i32>(hover_count_));
        w.end_compound();
    }
    w.end_compound();
}

void Text::encode(base::ByteBuf& out) const {
    static base::ByteBuf raw;
    static nbt::Doc doc;
    raw.clear();
    Loose l{nbt::Writer(raw)};
    l.w.root(nbt::kCompound);
    write(l);
    const u32 before = out.len;
    if (doc.parse({raw.data, raw.len}, nbt::Form::kNameless, nbt::kTrusted) == nbt::Error::kNone &&
        transcode(doc.root(), out) == Result::kOk)
        return;
    // Something no component of vanilla's can hold (a control character in a command to
    // suggest): the words alone.
    LOG_WARN("a text component could not be encoded: %s", plain().c_str());
    out.len = before;
    nbt::Writer w(out);
    w.root(nbt::kString);
    w.put_string(plain());
}

// TranslatableContents.decompose: "%s" and "%1$s" take arguments, "%%" is a percent sign, and
// anything else makes the whole format its own text.
void Text::plain(std::string& out) const {
    if (!translate_) {
        out += text_;
    } else {
        const std::string_view fmt = lang(text_);
        std::string made;
        bool ok = true;
        u32 next = 0;
        for (usize i = 0; i < fmt.size() && ok;) {
            if (fmt[i] != '%') {
                made += fmt[i++];
                continue;
            }
            usize j = i + 1;
            u32 index = 0;
            bool explicit_index = false;
            while (j < fmt.size() && fmt[j] >= '0' && fmt[j] <= '9') index = index * 10 + static_cast<u32>(fmt[j++] - '0');
            if (j > i + 1 && j < fmt.size() && fmt[j] == '$') {
                explicit_index = true;
                ++j;
            } else {
                j = i + 1;
            }
            const char kind = j < fmt.size() ? fmt[j] : '\0';
            if (kind == '%' && !explicit_index) {
                made += '%';
            } else if (kind == 's') {
                const u32 at = explicit_index ? index - 1 : next++;
                if (at >= args_.size()) {
                    ok = false;
                    break;
                }
                const Arg& a = args_[at];
                if (a.kind == Arg::kString) {
                    made += a.s;
                } else if (a.kind == Arg::kText) {
                    a.text[0].plain(made);
                } else if (a.kind == Arg::kFloat) {
                    made += java_float(static_cast<float>(a.d));
                } else if (a.kind == Arg::kDouble) {
                    made += java_double(a.d);
                } else {
                    made += std::to_string(a.n);
                }
            } else {
                ok = false;
            }
            i = j + 1;
        }
        out += ok ? std::string_view(made) : fmt;
    }
    for (const Text& child : extra_) child.plain(out);
}

std::string Text::plain() const {
    std::string out;
    plain(out);
    return out;
}

std::string_view lang(std::string_view key) {
    using proto::gen::lang::kEntries;
    usize lo = 0, hi = std::size(kEntries);
    while (lo < hi) {
        const usize mid = (lo + hi) / 2;
        if (kEntries[mid].key < key) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo < std::size(kEntries) && kEntries[lo].key == key ? kEntries[lo].text : key;
}

std::string java_format_double(double v) {
    if (v != v) return "NaN";
    if (v - v != 0) return v < 0 ? "-Infinity" : "Infinity";
    char buf[400];  // the longest fixed form of a double has 309 digits before the point
    const auto r = std::to_chars(buf, buf + sizeof buf, v < 0 ? -v : v, std::chars_format::fixed);  // shortest digits
    std::string digits(buf, r.ptr);
    usize dot = digits.find('.');
    if (dot == std::string::npos) {
        dot = digits.size();
        digits += '.';
    }
    constexpr usize kDecimals = 6;
    const bool up = digits.size() > dot + 1 + kDecimals && digits[dot + 1 + kDecimals] >= '5';
    digits.resize(dot + 1 + kDecimals, '0');
    if (up) {
        usize i = digits.size();
        while (i-- > 0) {
            if (digits[i] == '.') continue;
            if (digits[i] != '9') {
                ++digits[i];
                break;
            }
            digits[i] = '0';
            if (i == 0) digits.insert(digits.begin(), '1');
        }
    }
    return std::signbit(v) ? "-" + digits : digits;
}

namespace {
template <class T>
std::string java_number(T v) {
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::fixed);
    std::string s(buf, r.ptr);
    if (s.find('.') == std::string::npos) s += ".0";
    return s;
}
}  // namespace
std::string java_float(float v) { return java_number(v); }
std::string java_double(double v) { return java_number(v); }

std::string uuid_string(Uuid u) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string s;
    s.reserve(36);
    for (u32 i = 0; i < 32; ++i) {
        if (i == 8 || i == 12 || i == 16 || i == 20) s += '-';
        const u64 half = i < 16 ? u.hi : u.lo;
        s += kHex[(half >> (60 - 4 * (i & 15))) & 15];
    }
    return s;
}

}  // namespace text
