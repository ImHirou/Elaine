// SPDX-License-Identifier: GPL-3.0-or-later
#include "text/component.hpp"

#include <array>
#include <vector>

#include "proto/identifier.hpp"
#include "world/block.hpp"

namespace text {

namespace {

using nbt::Tag;

constexpr u32 kNone = ~0u;
// Components inside components - children, arguments, hover texts, lists of lists. Vanilla
// follows them as deep as NBT nests (512); what a client builds is a few deep.
constexpr u32 kMaxDepth = 32;

// The names a component's compound is asked for, in the order its codec writes them:
// the contents first (each type its own fields), then "extra", then Style.Serializer's fields.
enum Key : u32 {
    kType,
    kText,
    kTranslate,
    kFallback,
    kWith,
    kKeybind,
    kScore,
    kSelector,
    kNbt,
    kObject,
    kSprite,
    kPlayer,
    kExtra,
    kColor,
    kShadowColor,
    kBold,
    kItalic,
    kUnderlined,
    kStrikethrough,
    kObfuscated,
    kClickEvent,
    kHoverEvent,
    kInsertion,
    kFont,
    kKeyCount,
};
constexpr std::string_view kKeyName[kKeyCount] = {
    "type",   "text",       "translate",     "fallback",   "with",        "keybind",     "score",        "selector",
    "nbt",    "object",     "sprite",        "player",     "extra",       "color",       "shadow_color", "bold",
    "italic", "underlined", "strikethrough", "obfuscated", "click_event", "hover_event", "insertion",    "font",
};

// TextColor.NAMED_COLORS.
struct NamedColor {
    std::string_view name;
    u32 rgb;
};
constexpr NamedColor kColors[] = {
    {"black", 0x000000},     {"dark_blue", 0x0000AA},    {"dark_green", 0x00AA00}, {"dark_aqua", 0x00AAAA},
    {"dark_red", 0xAA0000},  {"dark_purple", 0xAA00AA},  {"gold", 0xFFAA00},       {"gray", 0xAAAAAA},
    {"dark_gray", 0x555555}, {"blue", 0x5555FF},         {"green", 0x55FF55},      {"aqua", 0x55FFFF},
    {"red", 0xFF5555},       {"light_purple", 0xFF55FF}, {"yellow", 0xFFFF55},     {"white", 0xFFFFFF},
};

enum Contents : u8 { kPlain, kTranslatable, kKeybindContents };
enum Click : u8 { kOpenUrl, kRunCommand, kSuggestCommand, kChangePage, kCopyToClipboard };
enum Hover : u8 { kShowText, kShowEntity, kShowItem };

constexpr u32 style_bit(u32 key) { return 1u << (key - kColor); }

// A component as the codec leaves it, before anything is written: what vanilla has in a
// MutableComponent. Strings stay where they are, in the document.
struct Style {
    u32 has = 0;    // style_bit of every field that is set
    u8 flags = 0;   // the values of bold .. obfuscated, bit (key - kBold)
    i8 named = -1;  // colour: an index into kColors, or -1 for `rgb`
    Click click{};  // click_event: the action...
    u32 rgb = 0;
    i32 shadow = 0;
    i32 page = 0;                      // ...and its page, or
    Tag click_value;                   // ...its url / command / value
    Hover hover_kind{};                // hover_event: the action...
    u32 hover = kNone;                 // ...show_text: the component; show_entity: its name, if it has one
    u32 hover_id = 0;                  // ...show_entity: the entity type; show_item: the item
    i32 hover_uuid[4] = {0, 0, 0, 0};  // ...show_entity: UUIDUtil.uuidToIntArray
    i32 hover_count = 1;               // ...show_item
    Tag insertion;
    Tag font;
};
struct Comp {
    Contents contents = kPlain;
    Tag text;                                     // the text, the translation key or the key binding
    Tag fallback;                                 // translatable
    u32 first_arg = kNone;                        // translatable: its arguments, a chain through Arg::next
    u32 first_child = kNone, last_child = kNone;  // "extra", a chain through Comp::next
    u32 next = kNone;
    Style style;
};
struct Arg {
    Tag tag;   // a number or a string...
    u32 comp;  // ...or (not kNone) a component
    u32 next;
};

// A name and where a HashMap would put it.
struct Field {
    u32 hash;
    u32 id;
};
// String.hashCode of an ASCII name, spread as HashMap.hash spreads it.
constexpr u32 map_hash(std::string_view s) {
    u32 h = 0;
    for (const char c : s) h = 31 * h + static_cast<u8>(c);
    return h ^ (h >> 16);
}
constexpr std::array<u32, kKeyCount> kKeyHash = [] {
    std::array<u32, kKeyCount> hash{};
    for (u32 k = 0; k < kKeyCount; ++k) hash[k] = map_hash(kKeyName[k]);
    return hash;
}();
// The order in which a HashMap hands out `n` entries it was given in this order: bucket by
// bucket, the entries of a bucket in the order they came. Its table starts with 16 buckets and
// doubles when more than three quarters of it are in use. This is the order of a CompoundTag's
// entries on the wire - CompoundTag.write walks its HashMap - and so of a component's fields.
void map_order(Field* f, u32 n) {
    u32 mask = 15;
    while (n * 4 > (mask + 1) * 3) mask = mask * 2 + 1;
    for (u32 i = 1; i < n; ++i) {  // stable, and n is a handful
        const Field x = f[i];
        u32 j = i;
        for (; j > 0 && (f[j - 1].hash & mask) > (x.hash & mask); --j) f[j] = f[j - 1];
        f[j] = x;
    }
}

// ExtraCodecs.CHAT_STRING: no control characters and no section sign (StringUtil.isAllowedChatCharacter).
bool chat_string(std::string_view s) {
    for (usize i = 0; i < s.size(); ++i) {
        const u8 c = static_cast<u8>(s[i]);
        if (c < 0x20 || c == 0x7F || c == 0xC0) return false;  // C0 80 is U+0000
        if (c == 0xC2 && i + 1 < s.size() && static_cast<u8>(s[i + 1]) == 0xA7) return false;
    }
    return true;
}

// Util.parseAndValidateUntrustedUri is java.net.URI's parser plus "the scheme is http or
// https". The parser is not ported; this accepts the URLs it certainly accepts -
//     http(s)://host[:port][/path][?query][#fragment]
// with a host of letters, digits, '.' and '-', and the rest made of the characters RFC 2396
// allows there and well-formed %XX escapes. Anything else is for the caller to call unsupported.
bool plain_url(std::string_view s) {
    const auto lower = [](char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c); };
    const auto alnum = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); };
    usize i = 0;
    for (const char c : std::string_view("http")) {
        if (i >= s.size() || lower(s[i]) != c) return false;
        ++i;
    }
    if (i < s.size() && lower(s[i]) == 's') ++i;
    if (s.substr(i, 3) != "://") return false;
    i += 3;
    const usize host = i;
    while (i < s.size() && (alnum(s[i]) || s[i] == '.' || s[i] == '-')) ++i;
    if (i == host) return false;
    if (i < s.size() && s[i] == ':') {
        const usize port = ++i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
        if (i == port) return false;
    }
    // Path, query and fragment: each starts with its own character and ends at one it may not hold.
    const auto hex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); };
    const auto part = [&](std::string_view also) {
        for (; i < s.size(); ++i) {
            const char c = s[i];
            if (alnum(c) || std::string_view("-_.!~*'();:@&=+$,").find(c) != std::string_view::npos ||
                also.find(c) != std::string_view::npos)
                continue;
            if (c != '%' || i + 2 >= s.size() || !hex(s[i + 1]) || !hex(s[i + 2])) return;
            i += 2;
        }
    };
    if (i < s.size() && s[i] == '/') part("/");
    if (i < s.size() && s[i] == '?') {
        ++i;
        part("/?");
    }
    if (i < s.size() && s[i] == '#') {
        ++i;
        part("/?");
    }
    return i == s.size();
}

class Transcoder {
  public:
    Result run(Tag in, base::ByteBuf& out) {
        comps_.clear();
        args_.clear();
        u32 root;
        if (const Result r = resolve(in, 0, root); r != Result::kOk) return r;
        nbt::Writer w(out);
        w.root(shape(root));
        emit(root, w);
        return Result::kOk;
    }

  private:
    // ---- reading: ComponentSerialization.CODEC's decode

    u32 add() {
        comps_.emplace_back();
        return static_cast<u32>(comps_.size() - 1);
    }
    void append_child(u32 parent, u32 child) {
        Comp& p = comps_[parent];
        if (p.first_child == kNone) {
            p.first_child = child;
        } else {
            comps_[p.last_child].next = child;
        }
        p.last_child = child;
    }

    // Codec.either(Codec.either(STRING, nonEmptyList(CODEC.listOf())), the record): a string is
    // plain text, a list is its first component with the others appended as children
    // (createFromList), a compound is the component in full.
    Result resolve(Tag in, u32 depth, u32& out) {
        if (depth > kMaxDepth) return Result::kUnsupported;
        switch (in.type) {
            case nbt::kString:
                out = add();
                comps_[out].text = in;
                return Result::kOk;
            case nbt::kList: {
                if (in.size() == 0) return Result::kInvalid;
                u32 head = kNone;
                for (const Tag e : in.elements()) {
                    u32 c;
                    if (const Result r = resolve(e, depth + 1, c); r != Result::kOk) return r;
                    if (head == kNone) {
                        head = c;
                    } else {
                        append_child(head, c);
                    }
                }
                out = head;
                return Result::kOk;
            }
            case nbt::kCompound: return record(in, depth, out);
            // A number is nothing; an array is a list of numbers, or an empty list.
            default: return Result::kInvalid;
        }
    }

    Result record(Tag in, u32 depth, u32& out) {
        Tag f[kKeyCount];
        for (const Tag e : in.entries()) {
            const std::string_view name = e.name();
            for (u32 k = 0; k < kKeyCount; ++k) {
                if (name != kKeyName[k]) continue;
                f[k] = e;  // a name read twice: the later value
                break;
            }
        }
        out = add();
        if (const Result r = contents(f, depth, out); r != Result::kOk) return r;
        if (const Tag extra = f[kExtra]) {  // optionalFieldOf: absent is fine, wrong is an error
            if (extra.type != nbt::kList || extra.size() == 0) return Result::kInvalid;
            for (const Tag e : extra.elements()) {
                u32 c;
                if (const Result r = resolve(e, depth + 1, c); r != Result::kOk) return r;
                append_child(out, c);
            }
        }
        return style(f, depth, out);
    }

    Result contents(const Tag* f, u32 depth, u32 index) {
        const auto plain = [&] {
            if (f[kText].type != nbt::kString) return false;
            comps_[index].contents = kPlain;
            comps_[index].text = f[kText];
            return true;
        };
        const auto keybind = [&] {
            if (f[kKeybind].type != nbt::kString) return false;
            comps_[index].contents = kKeybindContents;
            comps_[index].text = f[kKeybind];
            return true;
        };
        if (const Tag type = f[kType]) {  // StrictEither: a "type" decides, and then it has to be right
            if (type.type != nbt::kString) return Result::kInvalid;
            const std::string_view t = type.str();
            if (t == "text") return plain() ? Result::kOk : Result::kInvalid;
            if (t == "translatable") return translatable(f, depth, index);
            if (t == "keybind") return keybind() ? Result::kOk : Result::kInvalid;
            if (t == "score" || t == "selector" || t == "nbt" || t == "object") return Result::kUnsupported;
            return Result::kInvalid;
        }
        // FuzzyCodec: the first type that can be read from these fields, in the order the types
        // were registered - text, translatable, keybind, score, selector, nbt, object.
        if (plain()) return Result::kOk;
        if (const Result r = translatable(f, depth, index); r != Result::kInvalid) return r;
        if (keybind()) return Result::kOk;
        // Whether one of the others would take it is not this server's to say; without the
        // field each of them requires, none does.
        if (f[kScore] || f[kSelector] || f[kNbt] || f[kObject] || f[kSprite] || f[kPlayer]) return Result::kUnsupported;
        return Result::kInvalid;
    }

    // TranslatableContents.MAP_CODEC: "translate", a lenient "fallback" (a wrong one counts as
    // none), and "with" - a list whose elements are numbers and strings as they are
    // (ExtraCodecs.JAVA, filtered) or else components.
    Result translatable(const Tag* f, u32 depth, u32 index) {
        if (f[kTranslate].type != nbt::kString) return Result::kInvalid;
        const usize comps = comps_.size(), args = args_.size();
        u32 first = kNone, last = kNone;
        if (const Tag with = f[kWith]) {
            if (!with.collection()) return Result::kInvalid;
            for (const Tag e : with.elements()) {
                Arg a{e, kNone, kNone};
                if (!e.numeric() && e.type != nbt::kString) {
                    if (const Result r = resolve(e, depth + 1, a.comp); r != Result::kOk) {
                        comps_.resize(comps);  // the next type gets its turn at these fields
                        args_.resize(args);
                        return r;
                    }
                }
                const u32 at = static_cast<u32>(args_.size());
                args_.push_back(a);
                if (first == kNone) {
                    first = at;
                } else {
                    args_[last].next = at;
                }
                last = at;
            }
        }
        Comp& c = comps_[index];
        c.contents = kTranslatable;
        c.text = f[kTranslate];
        c.fallback = f[kFallback].type == nbt::kString ? f[kFallback] : Tag{};
        c.first_arg = first;
        return Result::kOk;
    }

    // Style.Serializer.MAP_CODEC: every field optional, and an error when it is there and wrong.
    Result style(const Tag* f, u32 depth, u32 index) {
        Style s;
        if (const Tag t = f[kColor]) {
            if (t.type != nbt::kString) return Result::kInvalid;
            if (const Result r = color(t.str(), s); r != Result::kOk) return r;
            s.has |= style_bit(kColor);
        }
        if (const Tag t = f[kShadowColor]) {  // ExtraCodecs.ARGB_COLOR_CODEC: an int, or four floats
            if (t.collection()) return Result::kUnsupported;
            if (!t.numeric()) return Result::kInvalid;
            s.shadow = t.as_int();
            s.has |= style_bit(kShadowColor);
        }
        for (u32 k = kBold; k <= kObfuscated; ++k) {  // Codec.BOOL of a tag: any number, true unless zero
            if (!f[k]) continue;
            if (!f[k].numeric()) return Result::kInvalid;
            s.flags |= static_cast<u8>((f[k].as_double() != 0) << (k - kBold));
            s.has |= style_bit(k);
        }
        if (const Tag t = f[kClickEvent]) {
            if (const Result r = click(t, s); r != Result::kOk) return r;
            s.has |= style_bit(kClickEvent);
        }
        if (const Tag t = f[kHoverEvent]) {
            if (const Result r = hover(t, depth, s); r != Result::kOk) return r;
            s.has |= style_bit(kHoverEvent);
        }
        if (const Tag t = f[kInsertion]) {
            if (t.type != nbt::kString) return Result::kInvalid;
            s.insertion = t;
            s.has |= style_bit(kInsertion);
        }
        if (const Tag t = f[kFont]) {  // FontDescription.CODEC: an Identifier
            proto::Identifier id;
            if (t.type != nbt::kString || !proto::parse_identifier(t.str(), id)) return Result::kInvalid;
            s.font = t;
            s.has |= style_bit(kFont);
        }
        comps_[index].style = s;
        return Result::kOk;
    }

    // TextColor.parseColor: a name, or '#' and what Integer.parseInt(s, 16) makes of the rest -
    // a sign is allowed, digits of any length, the value 0 .. 0xFFFFFF.
    static Result color(std::string_view text, Style& s) {
        if (text.empty() || text[0] != '#') {
            for (u32 i = 0; i < std::size(kColors); ++i) {
                if (text != kColors[i].name) continue;
                s.named = static_cast<i8>(i);
                return Result::kOk;
            }
            return Result::kInvalid;
        }
        usize i = 1;
        const bool negative = i < text.size() && text[i] == '-';
        if (i < text.size() && (text[i] == '-' || text[i] == '+')) ++i;
        if (i == text.size()) return Result::kInvalid;
        u32 value = 0;
        for (; i < text.size(); ++i) {
            const u8 c = static_cast<u8>(text[i]);
            if (c >= 0x80) return Result::kUnsupported;  // Character.digit reads the digits of every script
            const u32 digit = c >= '0' && c <= '9'                     ? c - '0'
                              : (c | 0x20) >= 'a' && (c | 0x20) <= 'f' ? (c | 0x20) - 'a' + 10
                                                                       : 16;
            if (digit == 16) return Result::kInvalid;
            value = value * 16 + digit;
            if (value > 0xFFFFFF) return Result::kInvalid;  // out of range, or soon too long for an int
        }
        if (negative && value != 0) return Result::kInvalid;
        s.named = -1;
        s.rgb = value;
        return Result::kOk;
    }

    // ClickEvent.CODEC: "action" picks the record. open_file exists and is refused (not
    // allowed from a server: ClickEvent.Action.filterForSerialization).
    static Result click(Tag t, Style& s) {
        if (t.type != nbt::kCompound) return Result::kInvalid;
        const Tag action = t.get("action");
        if (action.type != nbt::kString) return Result::kInvalid;
        const std::string_view a = action.str();
        const auto string = [&](std::string_view key, Click what) {
            s.click = what;
            s.click_value = t.get(key);
            return s.click_value.type == nbt::kString;
        };
        if (a == "open_url") {
            if (!string("url", kOpenUrl)) return Result::kInvalid;
            return plain_url(s.click_value.str()) ? Result::kOk : Result::kUnsupported;
        }
        if (a == "run_command" || a == "suggest_command") {
            return string("command", a[0] == 'r' ? kRunCommand : kSuggestCommand) && chat_string(s.click_value.str())
                       ? Result::kOk
                       : Result::kInvalid;
        }
        if (a == "copy_to_clipboard") return string("value", kCopyToClipboard) ? Result::kOk : Result::kInvalid;
        if (a == "change_page") {  // ExtraCodecs.POSITIVE_INT
            const Tag page = t.get("page");
            if (!page.numeric() || page.as_int() < 1) return Result::kInvalid;
            s.click = kChangePage;
            s.page = page.as_int();
            return Result::kOk;
        }
        if (a == "show_dialog" || a == "custom") return Result::kUnsupported;
        return Result::kInvalid;
    }

    // HoverEvent.CODEC.
    Result hover(Tag t, u32 depth, Style& s) {
        if (t.type != nbt::kCompound) return Result::kInvalid;
        const Tag action = t.get("action");
        if (action.type != nbt::kString) return Result::kInvalid;
        const std::string_view a = action.str();
        if (a == "show_text") {
            const Tag value = t.get("value");
            s.hover_kind = kShowText;
            return value ? resolve(value, depth + 1, s.hover) : Result::kInvalid;
        }
        // A registry entry by name; one that is not in the registry is an error to vanilla too
        // (it throws the sender out: tools/vanilla/component_probe.py).
        char full[320];
        const auto named = [&](Tag id) -> std::string_view {
            proto::Identifier parsed;
            if (id.type != nbt::kString || !proto::parse_identifier(id.str(), parsed) || parsed.size() > sizeof full) return {};
            return {full, static_cast<usize>(reinterpret_cast<char*>(parsed.put(reinterpret_cast<u8*>(full))) - full)};
        };
        if (a == "show_entity") {  // HoverEvent.EntityTooltipInfo.CODEC: id, uuid, an optional name
            const std::string_view type = named(t.get("id"));
            if (type.empty()) return Result::kInvalid;
            s.hover_kind = kShowEntity;
            s.hover_id = world::find_entity_type(type);
            if (s.hover_id == world::kNoId) return Result::kInvalid;
            // UUIDUtil.LENIENT_CODEC: four ints (an array, or a list of numbers), or a string -
            // of which only the form UUID.toString writes is read here.
            const Tag uuid = t.get("uuid");
            if (uuid.type == nbt::kString) {
                const std::string_view text = uuid.str();
                if (text.size() != 36) return Result::kUnsupported;
                u32 part = 0, digits = 0;
                for (u32 i = 0; i < 36; ++i) {
                    const u8 c = static_cast<u8>(text[i]);
                    if (i == 8 || i == 13 || i == 18 || i == 23) {
                        if (c != '-') return Result::kUnsupported;
                        continue;
                    }
                    const u32 digit = c >= '0' && c <= '9'                     ? c - '0'
                                      : (c | 0x20) >= 'a' && (c | 0x20) <= 'f' ? (c | 0x20) - 'a' + 10
                                                                               : 16;
                    if (digit == 16) return Result::kUnsupported;
                    part = part << 4 | digit;
                    if (++digits % 8 == 0) s.hover_uuid[digits / 8 - 1] = static_cast<i32>(part);
                }
            } else if (uuid.type == nbt::kIntArray || uuid.type == nbt::kList) {
                if (uuid.size() != 4) return Result::kInvalid;
                u32 i = 0;
                for (const Tag e : uuid.elements()) {
                    if (!e.numeric()) return Result::kInvalid;
                    s.hover_uuid[i++] = e.as_int();
                }
            } else {
                return uuid ? Result::kUnsupported : Result::kInvalid;
            }
            const Tag name = t.get("name");
            return name ? resolve(name, depth + 1, s.hover) : Result::kOk;
        }
        if (a == "show_item") {  // ItemStackTemplate.MAP_CODEC: id, count (1..99, 1 if absent), components
            const std::string_view item = named(t.get("id"));
            if (item.empty()) return Result::kInvalid;
            s.hover_kind = kShowItem;
            s.hover_id = world::find_item(item);
            if (s.hover_id == world::kNoId || s.hover_id == 0) return Result::kInvalid;  // Item.CODEC: not air either
            if (const Tag count = t.get("count")) {
                if (!count.numeric() || count.as_int() < 1 || count.as_int() > 99) return Result::kInvalid;
                s.hover_count = count.as_int();
            }
            // A stack with components: each one's Codec would have to be read, and none is yet.
            if (const Tag components = t.get("components")) {
                if (components.type != nbt::kCompound) return Result::kInvalid;
                if (components.size() != 0) return Result::kUnsupported;
            }
            return Result::kOk;
        }
        return Result::kInvalid;
    }

    // ---- writing: ComponentSerialization.CODEC's encode, then CompoundTag.write

    // Component.tryCollapseToString: plain text without children or style is written as a string.
    nbt::Type shape(u32 index) const {
        const Comp& c = comps_[index];
        return c.contents == kPlain && c.first_child == kNone && c.style.has == 0 ? nbt::kString : nbt::kCompound;
    }

    // The payload of the component's tag.
    void emit(u32 index, nbt::Writer& w) const {
        const Comp& c = comps_[index];
        if (shape(index) == nbt::kString) return w.put_string_raw(c.text.str());
        Field fields[kKeyCount];
        u32 n = 0;
        const auto field = [&](u32 key) { fields[n++] = {kKeyHash[key], key}; };
        // In the order the record writes them, which is the order the map receives them in.
        if (c.contents == kTranslatable) {
            field(kTranslate);
            if (c.fallback) field(kFallback);
            if (c.first_arg != kNone) field(kWith);
        } else {
            field(c.contents == kPlain ? kText : kKeybind);
        }
        if (c.first_child != kNone) field(kExtra);
        for (u32 k = kColor; k <= kFont; ++k)
            if (c.style.has & style_bit(k)) field(k);
        map_order(fields, n);
        for (u32 i = 0; i < n; ++i) {
            const u32 key = fields[i].id;
            const std::string_view name = kKeyName[key];
            switch (key) {
                case kText:
                case kTranslate:
                case kKeybind: w.put_string_raw(name, c.text.str()); break;
                case kFallback: w.put_string_raw(name, c.fallback.str()); break;
                case kWith: emit_args(c, w); break;
                case kExtra: emit_children(c, w); break;
                case kColor: emit_color(c.style, w); break;
                case kShadowColor: w.put_int(name, c.style.shadow); break;
                case kClickEvent: emit_click(c.style, w); break;
                case kHoverEvent: emit_hover(c.style, w); break;
                case kInsertion: w.put_string_raw(name, c.style.insertion.str()); break;
                case kFont: {  // Identifier.toString: always with its namespace
                    proto::Identifier id;
                    proto::parse_identifier(c.style.font.str(), id);
                    u8* p = put_tag_header(w.buf().reserve(3 + name.size() + 2 + id.size()), nbt::kString, name);
                    store_be16(p, static_cast<u16>(id.size()));
                    w.buf().commit(id.put(p + 2));
                    break;
                }
                default: w.put_byte(name, static_cast<i8>((c.style.flags >> (key - kBold)) & 1)); break;  // bold .. obfuscated
            }
        }
        w.end_compound();
    }

    static u8* put_tag_header(u8* p, nbt::Type type, std::string_view name) {
        *p = type;
        store_be16(p + 1, static_cast<u16>(name.size()));
        std::memcpy(p + 3, name.data(), name.size());
        return p + 3 + name.size();
    }

    // An element of a list whose type is `of`: as it is, or - a string or a number in a list
    // that also holds compounds - inside {"": x} (ListTag.wrapIfNeeded).
    template <class Payload>
    static void element(nbt::Writer& w, nbt::Type of, nbt::Type type, Payload&& payload) {
        if (of != type) {
            const u8 head[3] = {type, 0, 0};
            w.put_raw(head);
            payload();
            w.end_compound();
        } else {
            payload();
        }
    }
    // ListTag.identifyRawElementType over the elements seen so far.
    static void mix(nbt::Type& of, nbt::Type type) { of = of == nbt::kEnd || of == type ? type : nbt::kCompound; }

    void emit_children(const Comp& c, nbt::Writer& w) const {
        nbt::Type of = nbt::kEnd;
        u32 count = 0;
        for (u32 i = c.first_child; i != kNone; i = comps_[i].next, ++count) mix(of, shape(i));
        w.begin_list(kKeyName[kExtra], of, count);
        for (u32 i = c.first_child; i != kNone; i = comps_[i].next) element(w, of, shape(i), [&] { emit(i, w); });
    }

    void emit_args(const Comp& c, nbt::Writer& w) const {
        const auto type = [&](const Arg& a) { return a.comp == kNone ? a.tag.type : shape(a.comp); };
        nbt::Type of = nbt::kEnd;
        u32 count = 0;
        for (u32 i = c.first_arg; i != kNone; i = args_[i].next, ++count) mix(of, type(args_[i]));
        w.begin_list(kKeyName[kWith], of, count);
        for (u32 i = c.first_arg; i != kNone; i = args_[i].next) {
            const Arg& a = args_[i];
            element(w, of, type(a), [&] {
                if (a.comp != kNone) {
                    emit(a.comp, w);
                } else {
                    w.put_raw(a.tag.bytes());
                }
            });
        }
    }

    // TextColor.serialize: its name if it was given by name, "#RRGGBB" otherwise.
    static void emit_color(const Style& s, nbt::Writer& w) {
        if (s.named >= 0) return w.put_string_raw(kKeyName[kColor], kColors[s.named].name);
        char hex[7] = {'#'};
        for (u32 i = 0; i < 6; ++i) hex[1 + i] = "0123456789ABCDEF"[(s.rgb >> (20 - 4 * i)) & 15];
        w.put_string_raw(kKeyName[kColor], {hex, 7});
    }

    // The fields of a hover event in the order of its HashMap: every name in a bucket of its own
    // (see the static_assert below), so the order in which the codec hands them over is not asked.
    void emit_hover(const Style& s, nbt::Writer& w) const {
        w.begin_compound(kKeyName[kHoverEvent]);
        const auto component = [&](std::string_view field) {
            w.buf().commit(put_tag_header(w.buf().reserve(3 + field.size()), shape(s.hover), field));
            emit(s.hover, w);
        };
        switch (s.hover_kind) {
            case kShowText:  // action, value
                w.put_string_raw("action", "show_text");
                component("value");
                break;
            case kShowEntity:  // name, action, id, uuid
                if (s.hover != kNone) component("name");
                w.put_string_raw("action", "show_entity");
                w.put_string_raw("id", world::entity_type_name(s.hover_id));
                w.put_int_array("uuid", s.hover_uuid);
                break;
            case kShowItem:  // count (unless 1), action, id
                if (s.hover_count != 1) w.put_int("count", s.hover_count);
                w.put_string_raw("action", "show_item");
                w.put_string_raw("id", world::item_name(s.hover_id));
                break;
        }
        w.end_compound();
    }

    // {action: ..., <the action's field>: ...}: "action" lands in an earlier bucket than any of them.
    static void emit_click(const Style& s, nbt::Writer& w) {
        static constexpr std::string_view kAction[] = {"open_url", "run_command", "suggest_command", "change_page",
                                                       "copy_to_clipboard"};
        w.begin_compound(kKeyName[kClickEvent]);
        w.put_string_raw("action", kAction[s.click]);
        switch (s.click) {
            case kOpenUrl: w.put_string_raw("url", s.click_value.str()); break;
            case kRunCommand:
            case kSuggestCommand: w.put_string_raw("command", s.click_value.str()); break;
            case kChangePage: w.put_int("page", s.page); break;
            case kCopyToClipboard: w.put_string_raw("value", s.click_value.str()); break;
        }
        w.end_compound();
    }

    std::vector<Comp> comps_;
    std::vector<Arg> args_;
};

// "action" before the field of every click and hover event: emit_click and emit rely on it.
constexpr u32 bucket16(std::string_view s) { return map_hash(s) & 15; }
static_assert(bucket16("action") < bucket16("url") && bucket16("action") < bucket16("command") &&
              bucket16("action") < bucket16("page") && bucket16("action") < bucket16("value"));
// ...and the fields of show_entity and show_item, as emit_hover writes them.
static_assert(bucket16("name") < bucket16("action") && bucket16("action") < bucket16("id") && bucket16("id") < bucket16("uuid") &&
              bucket16("count") < bucket16("action"));

Transcoder transcoder;

}  // namespace

Result transcode(nbt::Tag tag, base::ByteBuf& out) {
    // Names and strings are compared and copied as bytes: that is only right for the bytes Java
    // itself writes (see nbt::Doc::plain_strings).
    if (!tag.doc || !tag.doc->plain_strings()) return Result::kUnsupported;
    return transcoder.run(tag, out);
}

}  // namespace text
