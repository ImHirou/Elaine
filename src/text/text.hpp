// SPDX-License-Identifier: GPL-3.0-or-later
// Text: a component the server makes itself - what a command answers, what a kick says.
//
// The counterpart of transcode() (text/component.hpp), which carries a client's component
// through: a Text is put together field by field, as vanilla's code calls Component.literal,
// translatable, append and withStyle, and encode() writes the tag vanilla's codec writes for
// the result - through transcode(), so there is one place that knows that form.
//
//     Text::translatable("commands.setblock.success").arg(x).arg(y).arg(z)
//     Text::literal("").append(message).color(Color::kRed)                    // sendFailure
//
// plain() is Component.getString(): the text with its translations filled in from en_us, the
// language a server prints its console in.
//
// A cold path: a Text owns its strings and allocates. Nothing per tick builds one.
#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/buf.hpp"
#include "base/types.hpp"

namespace text {

// ChatFormatting's colours, in the order of TextColor.NAMED_COLORS.
enum class Color : i8 {
    kNone = -1,
    kBlack,
    kDarkBlue,
    kDarkGreen,
    kDarkAqua,
    kDarkRed,
    kDarkPurple,
    kGold,
    kGray,
    kDarkGray,
    kBlue,
    kGreen,
    kAqua,
    kRed,
    kLightPurple,
    kYellow,
    kWhite,
};

class Text {
  public:
    Text() = default;  // Component.empty()
    static Text literal(std::string_view s) {
        Text t;
        t.text_ = s;
        return t;
    }
    static Text translatable(std::string_view key) {
        Text t;
        t.translate_ = true;
        t.text_ = key;
        return t;
    }

    // A component that is a tag already (type, payload), as text::transcode wrote it: an
    // item's custom name. What is set on it after is laid over what it brings.
    static Text encoded(std::span<const u8> tag) {
        Text t;
        t.raw_.assign(tag.begin(), tag.end());
        return t;
    }

    // ---- the arguments of a translatable, in order. Numbers and strings stay what they are
    // (TranslatableContents: an Integer travels as an int tag, a Long as a long).
    Text& arg(std::string_view s) & { return add_arg({Arg::kString, 0, 0, std::string(s), {}}); }
    Text& arg(const std::string& s) & { return arg(std::string_view(s)); }
    Text& arg(const char* s) & { return arg(std::string_view(s)); }
    Text& arg(i32 n) & { return add_arg({Arg::kInt, n, 0, {}, {}}); }
    Text& arg(u32 n) & { return arg(static_cast<i32>(n)); }
    Text& arg_long(i64 n) & { return add_arg({Arg::kLong, n, 0, {}, {}}); }
    Text& arg(float v) & { return add_arg({Arg::kFloat, 0, static_cast<double>(v), {}, {}}); }
    Text& arg(double v) & { return add_arg({Arg::kDouble, 0, v, {}, {}}); }
    Text& arg(Text t) & {
        Arg a{Arg::kText, 0, 0, {}, {}};
        a.text.push_back(std::move(t));
        return add_arg(std::move(a));
    }
    template <class T>
    Text&& arg(T&& v) && {
        arg(std::forward<T>(v));
        return std::move(*this);
    }
    Text&& arg_long(i64 n) && {
        arg_long(n);
        return std::move(*this);
    }

    // MutableComponent.append
    Text& append(Text child) & {
        extra_.push_back(std::move(child));
        return *this;
    }
    Text& append(std::string_view s) & { return s.empty() ? *this : append(literal(s)); }  // append(String): nothing for ""
    template <class T>
    Text&& append(T&& v) && {
        append(std::forward<T>(v));
        return std::move(*this);
    }

    // ---- style (MutableComponent.withStyle)
    Text& color(Color c) & {
        color_ = c;
        return *this;
    }
    Text& italic() & {
        italic_ = true;
        return *this;
    }
    Text& underlined() & {
        underlined_ = true;
        return *this;
    }
    Text& suggest_command(std::string_view command) & {  // ClickEvent.SuggestCommand
        has_click_ = true;
        click_ = command;
        return *this;
    }
    Text& insertion(std::string_view s) & {
        has_insertion_ = true;
        insertion_ = s;
        return *this;
    }
    // HoverEvent.ShowEntity: the type by name ("minecraft:pig"), the UUID, the name.
    Text& hover_entity(std::string_view type, Uuid uuid, Text name) & {
        hover_ = Hover::kEntity;
        hover_id_ = type;
        hover_uuid_ = uuid;
        hover_text_.assign(1, std::move(name));
        return *this;
    }
    // HoverEvent.ShowItem, for a stack without components: the item by name, how many.
    Text& hover_item(std::string_view item, u32 count) & {
        hover_ = Hover::kItem;
        hover_id_ = item;
        hover_count_ = count;
        return *this;
    }
    Text&& color(Color c) && { return std::move(color(c)); }
    Text&& italic() && { return std::move(italic()); }
    Text&& underlined() && { return std::move(underlined()); }
    Text&& suggest_command(std::string_view command) && { return std::move(suggest_command(command)); }
    Text&& insertion(std::string_view s) && { return std::move(insertion(s)); }
    Text&& hover_entity(std::string_view type, Uuid uuid, Text name) && {
        return std::move(hover_entity(type, uuid, std::move(name)));
    }
    Text&& hover_item(std::string_view item, u32 count) && { return std::move(hover_item(item, count)); }
    // ...and for one with components: the compound of them (type, payload), as
    // DataComponentPatch.CODEC writes it.
    Text& hover_item(std::string_view item, u32 count, std::span<const u8> components) & {
        hover_item(item, count);
        hover_components_.assign(components.begin(), components.end());
        return *this;
    }
    Text&& hover_item(std::string_view item, u32 count, std::span<const u8> components) && {
        return std::move(hover_item(item, count, components));
    }

    // Appends the component's tag as vanilla writes it: type byte and payload, no name.
    // Tick thread only (transcode's scratch space).
    void encode(base::ByteBuf& out) const;
    // Component.getString(): contents and children, translations from en_us.
    std::string plain() const;
    void plain(std::string& out) const;

  private:
    struct Arg {
        enum Kind : u8 { kString, kInt, kLong, kFloat, kDouble, kText } kind;
        i64 n;
        double d;
        std::string s;
        std::vector<Text> text;  // one, for kText
    };
    enum class Hover : u8 { kNone, kEntity, kItem };

    Text& add_arg(Arg a) {
        args_.push_back(std::move(a));
        return *this;
    }
    void write(struct Loose& w) const;

    bool translate_ = false;
    bool italic_ = false, underlined_ = false, has_click_ = false, has_insertion_ = false;
    Color color_ = Color::kNone;
    Hover hover_ = Hover::kNone;
    u32 hover_count_ = 1;
    Uuid hover_uuid_;
    std::string text_;  // the text, or the translation key
    std::string click_, insertion_, hover_id_;
    std::vector<Arg> args_;
    std::vector<Text> extra_;
    std::vector<Text> hover_text_;
    std::vector<u8> raw_, hover_components_;
};

// The en_us text of a translation key; the key itself where there is none (Language.getOrDefault).
std::string_view lang(std::string_view key);

// String.format(Locale.ROOT, "%f", v): six decimals, rounded half up from the shortest decimal
// form of the double (java.util.Formatter does not round the binary value, as printf does).
std::string java_format_double(double v);
// UUID.toString()
std::string uuid_string(Uuid u);
// Float.toString / Double.toString (base/java_number.hpp).
std::string java_float(float v);
std::string java_double(double v);

}  // namespace text
