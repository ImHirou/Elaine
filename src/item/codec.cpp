// SPDX-License-Identifier: GPL-3.0-or-later
#include "item/codec.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "nbt/ops.hpp"
#include "nbt/snbt.hpp"
#include "proto/identifier.hpp"
#include "proto/static_data.hpp"
#include "text/component.hpp"

#include PROTO_GEN_ENTITIES_HPP

namespace item {

using nbt::Tag;
using proto::Reader;
namespace reg = proto::reg;
namespace sreg = proto::gen::static_registry;

namespace {

using Names = std::span<const std::string_view>;

// DyeColor, Rarity, FireworkExplosion.Shape: StringRepresentable, by ordinal.
constexpr std::string_view kDyes[] = {"white",      "orange", "magenta", "light_blue", "yellow", "lime",  "pink", "gray",
                                      "light_gray", "cyan",   "purple",  "blue",       "brown",  "green", "red",  "black"};
constexpr std::string_view kRarities[] = {"common", "uncommon", "rare", "epic"};
constexpr std::string_view kFireworkShapes[] = {"small_ball", "large_ball", "star", "creeper", "burst"};

// The value in the wire form, as it is put together from a tag.
struct Out {
    base::ByteBuf& b;
    void u8v(u8 v) {
        *b.reserve(1) = v;
        ++b.len;
    }
    void varint(u32 v) { b.commit(proto::put_varint(b.reserve(proto::kVarIntMaxBytes), v)); }
    void be32(u32 v) { b.commit(proto::put_be32(b.reserve(4), v)); }
    void bytes(std::span<const u8> s) { b.append(s.data(), s.size()); }
    void str(std::string_view s) {
        varint(static_cast<u32>(s.size()));
        b.append(s.data(), s.size());
    }
};

constexpr Verdict kOk = Verdict::kOk, kBad = Verdict::kMalformed, kNotRead = Verdict::kUnsupported;

// ---------------------------------------------------------------------------- reading a tag as DataFixerUpper's codecs do

// Codec.INT, Codec.FLOAT: any number (NbtOps.getNumberValue), as Number.intValue() / floatValue().
bool as_int(Tag t, i32& v) {
    if (!t.numeric()) return false;
    v = t.as_int();
    return true;
}
bool as_float(Tag t, float& v) {
    if (!t.numeric()) return false;
    v = static_cast<float>(t.as_double());
    return true;
}
// Codec.BOOL: NbtOps.getBooleanValue - a number whose byteValue() is not 0.
bool as_bool(Tag t, bool& v) {
    if (!t.numeric()) return false;
    v = static_cast<i8>(t.as_int()) != 0;
    return true;
}
bool as_string(Tag t, std::string& v) {
    if (t.type != nbt::kString) return false;
    v = nbt::to_utf8(t.str());
    return true;
}
// Identifier.CODEC: a string through Identifier.read; as "namespace:path".
bool as_identifier(Tag t, std::string& v) {
    std::string s;
    proto::Identifier id;
    if (!as_string(t, s) || !proto::parse_identifier(s, id)) return false;
    v.assign(id.ns);
    v += ':';
    v += id.path;
    return true;
}
// A registry's entry by name (Registry.byNameCodec, a Holder by reference).
bool as_entry(Tag t, Names names, u32& id) {
    std::string name;
    if (!as_identifier(t, name)) return false;
    const auto it = std::find(names.begin(), names.end(), name);
    if (it == names.end()) return false;
    id = static_cast<u32>(it - names.begin());
    return true;
}
// StringRepresentable.fromEnum: the name, exactly.
bool as_enum(Tag t, Names names, u32& id) {
    std::string name;
    if (!as_string(t, name)) return false;
    const auto it = std::find(names.begin(), names.end(), name);
    if (it == names.end()) return false;
    id = static_cast<u32>(it - names.begin());
    return true;
}

// An optional field of a record: absent is `fallback`; present and wrong is the record's error.
template <class T, class Read>
bool optional(Tag record, std::string_view key, T& v, T fallback, Read read) {
    v = fallback;
    const Tag field = record.get(key);
    return !field || read(field, v);
}

// ComponentSerialization.CODEC over a tag: the tag vanilla writes for it.
Verdict text_from(Tag t, Out& out) {
    switch (text::transcode(t, out.b)) {
        case text::Result::kOk: return kOk;
        case text::Result::kInvalid: return kBad;
        default: return kNotRead;
    }
}

// ---------------------------------------------------------------------------- writing a stored value as a tag

// One tag of a stored value: its type and its payload.
struct Stored {
    nbt::Type type;
    std::span<const u8> payload;
};
Stored take_tag(Reader& in) {
    nbt::Doc doc;
    doc.parse({in.p, in.remaining()}, nbt::Form::kNameless, nbt::kTrusted);
    const std::span<const u8> whole = in.bytes(std::max<u32>(doc.size(), 1));
    return {static_cast<nbt::Type>(whole[0]), whole.subspan(1)};
}
std::string_view take_str(Reader& in) { return in.str(~0u); }

// A list of tags as ListTag.write writes it: of their one type, or - if they differ - of
// compounds, with what is not one wrapped as {"": x}. (A text component is never a compound
// that only holds a "", so none of them is taken for such a wrapper.)
void put_tags(nbt::Writer& w, std::string_view name, std::span<const Stored> tags) {
    nbt::Type of = nbt::kEnd;
    for (const Stored& t : tags) of = of == nbt::kEnd || of == t.type ? t.type : nbt::kCompound;
    w.begin_list(name, of, static_cast<u32>(tags.size()));
    for (const Stored& t : tags) {
        if (of == nbt::kCompound && t.type != nbt::kCompound) {
            w.entry(t.type, "");
            w.put_raw(t.payload);
            w.end_compound();
        } else {
            w.put_raw(t.payload);
        }
    }
}

// Codec.INT.listOf(): a list of ints (not an int array: only what is built as one is one).
void put_ints(nbt::Writer& w, std::string_view name, std::span<const i32> v) {
    w.begin_list(name, v.empty() ? nbt::kEnd : nbt::kInt, static_cast<u32>(v.size()));
    for (const i32 x : v) w.put_int(x);
}

// ---------------------------------------------------------------------------- the components, each both ways
//
// `from`: the tag its Codec reads -> the bytes its StreamCodec writes (unchecked: check_value
// follows). `to`: those bytes -> the entry its Codec writes.

// Filterable.codec(Codec.string(0, max)): the text, or {raw, filtered}.
Verdict filterable_string_from(Tag t, Out& out) {
    std::string raw, filtered;
    if (as_string(t, raw)) {
        out.str(raw);
        out.u8v(0);
        return kOk;
    }
    if (t.type != nbt::kCompound || !as_string(t.get("raw"), raw)) return kBad;
    const Tag f = t.get("filtered");
    if (f && !as_string(f, filtered)) return kBad;
    out.str(raw);
    out.u8v(static_cast<bool>(f));
    if (f) out.str(filtered);
    return kOk;
}
void filterable_string_to(Reader& in, nbt::Writer& w) {  // the entries of {raw, filtered}; the compound is the caller's
    w.put_string("raw", take_str(in));
    if (in.u8v()) w.put_string("filtered", take_str(in));
}

// MobEffectInstance.CODEC / Details.MAP_CODEC, after the "id".
Verdict effect_details_from(Tag t, Out& out, u32 depth) {
    i32 amplifier, duration;
    bool ambient, particles, icon;
    // ExtraCodecs.UNSIGNED_BYTE: a byte, read as 0..255.
    if (!optional(t, "amplifier", amplifier, 0, [](Tag f, i32& v) {
            if (!f.numeric()) return false;
            v = static_cast<u8>(f.as_int());
            return true;
        }))
        return kBad;
    if (!optional(t, "duration", duration, 0, as_int) || !optional(t, "ambient", ambient, false, as_bool) ||
        !optional(t, "show_particles", particles, true, as_bool) || !optional(t, "show_icon", icon, particles, as_bool))
        return kBad;
    out.varint(static_cast<u32>(amplifier));
    out.varint(static_cast<u32>(duration));
    out.u8v(ambient);
    out.u8v(particles);
    out.u8v(icon);
    const Tag hidden = t.get("hidden_effect");
    out.u8v(static_cast<bool>(hidden));
    if (!hidden) return kOk;
    if (hidden.type != nbt::kCompound) return kBad;
    if (depth >= 8) return kNotRead;
    return effect_details_from(hidden, out, depth + 1);
}
void effect_details_to(Reader& in, nbt::Writer& w) {
    const u32 amplifier = in.varint();
    const i32 duration = static_cast<i32>(in.varint());
    const bool ambient = in.u8v(), particles = in.u8v(), icon = in.u8v(), hidden = in.u8v();
    if (amplifier != 0) w.put_byte("amplifier", static_cast<i8>(amplifier));
    if (duration != 0) w.put_int("duration", duration);
    if (ambient) w.put_byte("ambient", 1);
    if (!particles) w.put_byte("show_particles", 0);
    w.put_byte("show_icon", icon);
    if (hidden) {
        w.begin_compound("hidden_effect");
        effect_details_to(in, w);
        w.end_compound();
    }
}
Verdict effect_from(Tag t, Out& out) {
    u32 id;
    if (t.type != nbt::kCompound || !as_entry(t.get("id"), sreg::kMobEffectNames, id)) return kBad;
    out.varint(id);
    return effect_details_from(t, out, 0);
}
void effect_to(Reader& in, nbt::Writer& w) {
    w.put_string("id", sreg::kMobEffectNames[in.varint()]);
    effect_details_to(in, w);
}

Verdict explosion_from(Tag t, Out& out) {
    u32 shape;
    if (t.type != nbt::kCompound || !as_enum(t.get("shape"), kFireworkShapes, shape)) return kBad;
    out.varint(shape);
    for (const std::string_view key : {"colors", "fade_colors"}) {
        const Tag list = t.get(key);
        if (list && !list.collection()) return kBad;
        out.varint(list ? list.size() : 0);
        if (!list) continue;
        for (const Tag e : list.elements()) {
            i32 colour;
            if (!as_int(e, colour)) return kBad;
            out.be32(static_cast<u32>(colour));
        }
    }
    bool trail, twinkle;
    if (!optional(t, "has_trail", trail, false, as_bool) || !optional(t, "has_twinkle", twinkle, false, as_bool)) return kBad;
    out.u8v(trail);
    out.u8v(twinkle);
    return kOk;
}
void explosion_to(Reader& in, nbt::Writer& w) {
    w.put_string("shape", kFireworkShapes[in.varint()]);
    for (const std::string_view key : {"colors", "fade_colors"}) {
        std::vector<i32> colours(in.varint());
        for (i32& c : colours) c = static_cast<i32>(in.be32());
        if (!colours.empty()) put_ints(w, key, colours);
    }
    if (in.u8v()) w.put_byte("has_trail", 1);
    if (in.u8v()) w.put_byte("has_twinkle", 1);
}

Verdict from(u32 type, Tag t, Out& out) {
    switch (type) {
        case component::kCustomName:
        case component::kItemName: return text_from(t, out);
        case component::kLore: {  // ItemLore.CODEC: at most 256 lines
            if (!t.collection() || t.size() > 256) return kBad;
            out.varint(t.size());
            for (const Tag line : t.elements()) {
                if (const Verdict v = text_from(line, out); v != kOk) return v;
            }
            return kOk;
        }
        case component::kCustomData:
        case component::kBucketEntityData: {  // CustomData.COMPOUND_TAG_CODEC: a compound, or its SNBT in a string
            if (t.type == nbt::kString) {
                base::ByteBuf read;
                nbt::SnbtError why;
                if (!nbt::read_snbt_fully(nbt::to_utf8(t.str()), read, why) || read.data[0] != nbt::kCompound) return kBad;
                out.bytes({read.data, read.len});
                return kOk;
            }
            if (t.type != nbt::kCompound) return kBad;
            out.u8v(nbt::kCompound);
            out.bytes(t.bytes());
            return kOk;
        }
        case component::kEntityData:
        case component::kBlockEntityData: {  // TypedEntityData.codec: a compound that names its type as "id"
            if (t.type != nbt::kCompound) return kBad;
            std::string name;
            if (!as_identifier(t.get("id"), name)) return kBad;
            const u32 id = type == component::kEntityData ? world::find_entity_type(name) : world::find_block_entity_type(name);
            if (id == world::kNoId) return kBad;
            out.varint(id);
            out.u8v(nbt::kCompound);
            nbt::Writer w(out.b);
            for (const Tag e : t.entries())
                if (e.name() != "id") w.put_raw(e.name(), e.type, e.bytes());
            w.end_compound();
            return kOk;
        }
        case component::kWritableBookContent: {
            if (t.type != nbt::kCompound) return kBad;
            const Tag pages = t.get("pages");
            if (pages && (!pages.collection() || pages.size() > 100)) return kBad;
            out.varint(pages ? pages.size() : 0);
            if (pages)
                for (const Tag page : pages.elements())
                    if (const Verdict v = filterable_string_from(page, out); v != kOk) return v;
            return kOk;
        }
        case component::kWrittenBookContent: {
            if (t.type != nbt::kCompound) return kBad;
            if (const Verdict v = filterable_string_from(t.get("title"), out); v != kOk) return v;
            std::string author;
            i32 generation;
            bool resolved;
            if (!as_string(t.get("author"), author) || !optional(t, "generation", generation, 0, as_int) ||
                !optional(t, "resolved", resolved, false, as_bool))
                return kBad;
            out.str(author);
            out.varint(static_cast<u32>(generation));
            const Tag pages = t.get("pages");
            if (pages && !pages.collection()) return kBad;
            out.varint(pages ? pages.size() : 0);
            if (pages) {
                for (const Tag page : pages.elements()) {  // Filterable.codec of a component: it, or {raw, filtered}
                    const Tag raw = page.type == nbt::kCompound ? page.get("raw") : Tag{};
                    if (!raw) {
                        if (const Verdict v = text_from(page, out); v != kOk) return v;
                        out.u8v(0);
                        continue;
                    }
                    if (const Verdict v = text_from(raw, out); v != kOk) return v;
                    const Tag filtered = page.get("filtered");
                    out.u8v(static_cast<bool>(filtered));
                    if (filtered)
                        if (const Verdict v = text_from(filtered, out); v != kOk) return v;
                }
            }
            out.u8v(resolved);
            return kOk;
        }
        case component::kSignTextFront:
        case component::kSignTextBack: {  // SignText.CODEC
            if (t.type != nbt::kCompound) return kBad;
            const Tag messages = t.get("messages"), filtered = t.get("filtered_messages");
            if (!messages.collection() || messages.size() != 4) return kBad;
            for (const Tag line : messages.elements())
                if (const Verdict v = text_from(line, out); v != kOk) return v;
            // lenientOptionalFieldOf: filtered lines that are not four lines are none.
            const bool has = filtered && filtered.collection() && filtered.size() == 4;
            const u32 mark = out.b.len;
            out.u8v(has);
            if (has) {
                for (const Tag line : filtered.elements()) {
                    if (text_from(line, out) != kOk) {
                        out.b.len = mark;
                        out.u8v(0);
                        break;
                    }
                }
            }
            u32 colour;
            bool glowing;
            if (!optional(t, "color", colour, 15u, [](Tag f, u32& v) { return as_enum(f, kDyes, v); }) ||
                !optional(t, "has_glowing_text", glowing, false, as_bool))
                return kBad;
            out.varint(colour);
            out.u8v(glowing);
            return kOk;
        }
        case component::kMaxStackSize:
        case component::kMaxDamage:
        case component::kDamage:
        case component::kRepairCost:
        case component::kOminousBottleAmplifier:
        case component::kMapId: {
            i32 v;
            if (!as_int(t, v)) return kBad;
            out.varint(static_cast<u32>(v));
            return kOk;
        }
        case component::kUnbreakable:
        case component::kGlider:
        case component::kWaxed: return t.type == nbt::kCompound ? kOk : kBad;  // Unit.CODEC: a map, of whatever
        case component::kEnchantmentGlintOverride: {
            bool v;
            if (!as_bool(t, v)) return kBad;
            out.u8v(v);
            return kOk;
        }
        case component::kMinimumAttackCharge:
        case component::kPotionDurationScale: {
            float v;
            if (!as_float(t, v)) return kBad;
            out.be32(std::bit_cast<u32>(v));
            return kOk;
        }
        case component::kDyedColor: {  // ExtraCodecs.RGB_COLOR_CODEC: an int, or three floats of 0..1
            i32 v;
            if (as_int(t, v)) {
                out.be32(static_cast<u32>(v));
                return kOk;
            }
            if (!t.collection() || t.size() != 3) return kBad;
            u32 rgb = 0xFF;  // ARGB.colorFromFloat(1, r, g, b)
            for (const Tag part : t.elements()) {
                float f;
                if (!as_float(part, f)) return kBad;
                rgb = rgb << 8 | (static_cast<u32>(static_cast<i32>(std::floor(f * 255.0f))) & 0xFF);
            }
            out.be32(rgb);
            return kOk;
        }
        case component::kRarity: {
            u32 v;
            if (!as_enum(t, kRarities, v)) return kBad;
            out.varint(v);
            return kOk;
        }
        case component::kDye:
        case component::kBaseColor:
        case component::kWolfCollar:
        case component::kCatCollar:
        case component::kSheepColor:
        case component::kShulkerColor:
        case component::kTropicalFishBaseColor:
        case component::kTropicalFishPatternColor:
        case component::kCushionColor: {
            u32 v;
            if (!as_enum(t, kDyes, v)) return kBad;
            out.varint(v);
            return kOk;
        }
        case component::kItemModel:
        case component::kTooltipStyle:
        case component::kNoteBlockSound: {
            std::string id;
            if (!as_identifier(t, id)) return kBad;
            out.str(id);
            return kOk;
        }
        case component::kEnchantments:
        case component::kStoredEnchantments: {  // a map of enchantment -> level
            if (t.type != nbt::kCompound) return kBad;
            out.varint(t.size());
            for (const Tag e : t.entries()) {
                proto::Identifier id;
                const std::string key = nbt::to_utf8(e.name());
                i32 level;
                if (!proto::parse_identifier(key, id) || !as_int(e, level)) return kBad;
                std::string full(id.ns);
                full += ':';
                full += id.path;
                const auto it = std::find(std::begin(reg::enchantment::kNames), std::end(reg::enchantment::kNames), full);
                if (it == std::end(reg::enchantment::kNames)) return kBad;
                out.varint(static_cast<u32>(it - std::begin(reg::enchantment::kNames)));
                out.varint(static_cast<u32>(level));
            }
            return kOk;
        }
        case component::kTooltipDisplay: {
            if (t.type != nbt::kCompound) return kBad;
            bool hide;
            if (!optional(t, "hide_tooltip", hide, false, as_bool)) return kBad;
            out.u8v(hide);
            const Tag hidden = t.get("hidden_components");
            if (hidden && !hidden.collection()) return kBad;
            out.varint(hidden ? hidden.size() : 0);
            if (hidden) {
                for (const Tag e : hidden.elements()) {
                    u32 id;
                    if (!as_entry(e, component::kNames, id)) return kBad;
                    out.varint(id);
                }
            }
            return kOk;
        }
        case component::kPotionContents: {  // the potion alone, or {potion, custom_color, custom_effects, custom_name}
            u32 potion;
            if (t.type == nbt::kString) {
                if (!as_entry(t, sreg::kPotionNames, potion)) return kBad;
                out.u8v(1);
                out.varint(potion);
                out.u8v(0);
                out.varint(0);
                out.u8v(0);
                return kOk;
            }
            if (t.type != nbt::kCompound) return kBad;
            const Tag p = t.get("potion"), colour = t.get("custom_color"), effects = t.get("custom_effects"),
                      name = t.get("custom_name");
            out.u8v(static_cast<bool>(p));
            if (p) {
                if (!as_entry(p, sreg::kPotionNames, potion)) return kBad;
                out.varint(potion);
            }
            out.u8v(static_cast<bool>(colour));
            if (colour) {
                i32 v;
                if (!as_int(colour, v)) return kBad;
                out.be32(static_cast<u32>(v));
            }
            if (effects && !effects.collection()) return kBad;
            out.varint(effects ? effects.size() : 0);
            if (effects)
                for (const Tag e : effects.elements())
                    if (const Verdict v = effect_from(e, out); v != kOk) return v;
            out.u8v(static_cast<bool>(name));
            if (name) {
                std::string s;
                if (!as_string(name, s)) return kBad;
                out.str(s);
            }
            return kOk;
        }
        case component::kSuspiciousStewEffects: {
            if (!t.collection()) return kBad;
            out.varint(t.size());
            for (const Tag e : t.elements()) {
                u32 id;
                if (e.type != nbt::kCompound || !as_entry(e.get("id"), sreg::kMobEffectNames, id)) return kBad;
                i32 duration = 160;  // lenientOptionalFieldOf: a duration that is no number is the default
                if (const Tag d = e.get("duration"); d && d.numeric()) duration = d.as_int();
                out.varint(id);
                out.varint(static_cast<u32>(duration));
            }
            return kOk;
        }
        case component::kFireworkExplosion: return explosion_from(t, out);
        case component::kFireworks: {
            if (t.type != nbt::kCompound) return kBad;
            const Tag flight = t.get("flight_duration"), explosions = t.get("explosions");
            if (flight && !flight.numeric()) return kBad;
            out.varint(flight ? static_cast<u8>(flight.as_int()) : 0);  // ExtraCodecs.UNSIGNED_BYTE
            if (explosions && (!explosions.collection() || explosions.size() > 256)) return kBad;
            out.varint(explosions ? explosions.size() : 0);
            if (explosions)
                for (const Tag e : explosions.elements())
                    if (const Verdict v = explosion_from(e, out); v != kOk) return v;
            return kOk;
        }
        case component::kInstrument:
        case component::kPaintingVariant: {  // a Holder: the entry's name - or the entry itself, which is not read
            if (t.type == nbt::kCompound) return kNotRead;
            u32 id;
            const Names names =
                type == component::kInstrument ? Names(reg::instrument::kNames) : Names(reg::painting_variant::kNames);
            if (!as_entry(t, names, id)) return kBad;
            out.varint(id + 1);
            return kOk;
        }
        case component::kBannerPatterns: {
            if (!t.collection()) return kBad;
            out.varint(t.size());
            for (const Tag layer : t.elements()) {
                if (layer.type != nbt::kCompound) return kBad;
                const Tag pattern = layer.get("pattern");
                if (pattern.type == nbt::kCompound) return kNotRead;
                u32 id, colour;
                if (!as_entry(pattern, reg::banner_pattern::kNames, id) || !as_enum(layer.get("color"), kDyes, colour))
                    return kBad;
                out.varint(id + 1);
                out.varint(colour);
            }
            return kOk;
        }
        case component::kBlockState: {  // a map of property -> value, both strings
            if (t.type != nbt::kCompound) return kBad;
            out.varint(t.size());
            for (const Tag e : t.entries()) {
                std::string value;
                if (!as_string(e, value)) return kBad;
                out.str(nbt::to_utf8(e.name()));
                out.str(value);
            }
            return kOk;
        }
        default: return kNotRead;
    }
}

void to(u32 type, Reader& in, nbt::Writer& w, std::string_view name) {
    switch (type) {
        case component::kCustomName:
        case component::kItemName:
        case component::kCustomData:
        case component::kBucketEntityData: {
            const Stored t = take_tag(in);
            w.put_raw(name, t.type, t.payload);
            break;
        }
        case component::kLore: {
            std::vector<Stored> lines(in.varint());
            for (Stored& line : lines) line = take_tag(in);
            put_tags(w, name, lines);
            break;
        }
        case component::kEntityData:
        case component::kBlockEntityData: {  // the tag, and its type put into it as "id"
            const u32 id = in.varint();
            const Stored t = take_tag(in);
            w.entry(nbt::kCompound, name);
            w.put_raw(t.payload.first(t.payload.size() - 1));  // its entries, without the end
            w.put_string("id", type == component::kEntityData ? world::entity_type_name(id) : world::block_entity_type_name(id));
            w.end_compound();
            break;
        }
        case component::kWritableBookContent: {
            w.begin_compound(name);
            if (const u32 pages = in.varint()) {
                w.begin_list("pages", nbt::kCompound, pages);
                for (u32 i = 0; i < pages; ++i) {
                    filterable_string_to(in, w);
                    w.end_compound();
                }
            }
            w.end_compound();
            break;
        }
        case component::kWrittenBookContent: {
            w.begin_compound(name);
            w.begin_compound("title");
            filterable_string_to(in, w);
            w.end_compound();
            w.put_string("author", take_str(in));
            if (const i32 generation = static_cast<i32>(in.varint())) w.put_int("generation", generation);
            if (const u32 pages = in.varint()) {
                w.begin_list("pages", nbt::kCompound, pages);
                for (u32 i = 0; i < pages; ++i) {
                    const Stored raw = take_tag(in);
                    w.put_raw("raw", raw.type, raw.payload);
                    if (in.u8v()) {
                        const Stored filtered = take_tag(in);
                        w.put_raw("filtered", filtered.type, filtered.payload);
                    }
                    w.end_compound();
                }
            }
            if (in.u8v()) w.put_byte("resolved", 1);
            w.end_compound();
            break;
        }
        case component::kSignTextFront:
        case component::kSignTextBack: {
            w.begin_compound(name);
            Stored lines[4];
            for (Stored& line : lines) line = take_tag(in);
            put_tags(w, "messages", lines);
            if (in.u8v()) {
                for (Stored& line : lines) line = take_tag(in);
                put_tags(w, "filtered_messages", lines);
            }
            w.put_string("color", kDyes[in.varint()]);
            w.put_byte("has_glowing_text", in.u8v());
            w.end_compound();
            break;
        }
        case component::kMaxStackSize:
        case component::kMaxDamage:
        case component::kDamage:
        case component::kRepairCost:
        case component::kOminousBottleAmplifier:
        case component::kMapId: w.put_int(name, static_cast<i32>(in.varint())); break;
        case component::kUnbreakable:
        case component::kGlider:
        case component::kWaxed:
            w.begin_compound(name);
            w.end_compound();
            break;
        case component::kEnchantmentGlintOverride: w.put_byte(name, in.u8v()); break;
        case component::kMinimumAttackCharge:
        case component::kPotionDurationScale: w.put_float(name, std::bit_cast<float>(in.be32())); break;
        case component::kDyedColor: w.put_int(name, static_cast<i32>(in.be32())); break;
        case component::kRarity: w.put_string(name, kRarities[in.varint()]); break;
        case component::kDye:
        case component::kBaseColor:
        case component::kWolfCollar:
        case component::kCatCollar:
        case component::kSheepColor:
        case component::kShulkerColor:
        case component::kTropicalFishBaseColor:
        case component::kTropicalFishPatternColor:
        case component::kCushionColor: w.put_string(name, kDyes[in.varint()]); break;
        case component::kItemModel:
        case component::kTooltipStyle:
        case component::kNoteBlockSound: w.put_string(name, take_str(in)); break;
        case component::kEnchantments:
        case component::kStoredEnchantments: {
            w.begin_compound(name);
            for (u32 n = in.varint(); n > 0; --n) {
                const u32 id = in.varint();
                w.put_int(reg::enchantment::kNames[id], static_cast<i32>(in.varint()));
            }
            w.end_compound();
            break;
        }
        case component::kTooltipDisplay: {
            w.begin_compound(name);
            if (in.u8v()) w.put_byte("hide_tooltip", 1);
            if (const u32 n = in.varint()) {
                w.begin_list("hidden_components", nbt::kString, n);
                for (u32 i = 0; i < n; ++i) w.put_string(component::kNames[in.varint()]);
            }
            w.end_compound();
            break;
        }
        case component::kPotionContents: {
            w.begin_compound(name);
            if (in.u8v()) w.put_string("potion", sreg::kPotionNames[in.varint()]);
            if (in.u8v()) w.put_int("custom_color", static_cast<i32>(in.be32()));
            if (const u32 effects = in.varint()) {
                w.begin_list("custom_effects", nbt::kCompound, effects);
                for (u32 i = 0; i < effects; ++i) {
                    effect_to(in, w);
                    w.end_compound();
                }
            }
            if (in.u8v()) w.put_string("custom_name", take_str(in));
            w.end_compound();
            break;
        }
        case component::kSuspiciousStewEffects: {
            const u32 n = in.varint();
            w.begin_list(name, n ? nbt::kCompound : nbt::kEnd, n);
            for (u32 i = 0; i < n; ++i) {
                w.put_string("id", sreg::kMobEffectNames[in.varint()]);
                if (const i32 duration = static_cast<i32>(in.varint()); duration != 160) w.put_int("duration", duration);
                w.end_compound();
            }
            break;
        }
        case component::kFireworkExplosion:
            w.begin_compound(name);
            explosion_to(in, w);
            w.end_compound();
            break;
        case component::kFireworks: {
            w.begin_compound(name);
            if (const u32 flight = in.varint()) w.put_byte("flight_duration", static_cast<i8>(flight));
            if (const u32 n = in.varint()) {
                w.begin_list("explosions", nbt::kCompound, n);
                for (u32 i = 0; i < n; ++i) {
                    explosion_to(in, w);
                    w.end_compound();
                }
            }
            w.end_compound();
            break;
        }
        case component::kInstrument: w.put_string(name, reg::instrument::kNames[in.varint() - 1]); break;
        case component::kPaintingVariant: w.put_string(name, reg::painting_variant::kNames[in.varint() - 1]); break;
        case component::kBannerPatterns: {
            const u32 n = in.varint();
            w.begin_list(name, n ? nbt::kCompound : nbt::kEnd, n);
            for (u32 i = 0; i < n; ++i) {
                w.put_string("pattern", reg::banner_pattern::kNames[in.varint() - 1]);
                w.put_string("color", kDyes[in.varint()]);
                w.end_compound();
            }
            break;
        }
        case component::kBlockState: {
            w.begin_compound(name);
            for (u32 n = in.varint(); n > 0; --n) {
                const std::string_view key = take_str(in);
                w.put_string(key, take_str(in));
            }
            w.end_compound();
            break;
        }
        default: DCHECK(false); break;
    }
}

}  // namespace

std::string_view component_name(u32 type) { return component::kNames[type]; }

u32 find_component(std::string_view name) {
    proto::Identifier id;
    if (!proto::parse_identifier(name, id)) return kNoComponent;
    for (u32 type = 0; type < component::kCount; ++type) {
        const std::string_view full = component::kNames[type];
        if (full.size() == id.size() && full.starts_with(id.ns) && full[id.ns.size()] == ':' && full.ends_with(id.path))
            return type;
    }
    return kNoComponent;
}

bool has_codec(u32 type) {
    // The components `from` reads are those components.cpp reads on the wire (every one of which
    // has a Codec in vanilla): asked of its decoder, which says kUnsupported of the others.
    static const std::vector<bool> known = [] {
        std::vector<bool> k(component::kCount);
        base::ByteBuf scratch;
        for (u32 t = 0; t < component::kCount; ++t) k[t] = check_value(t, {}, scratch) != Verdict::kUnsupported;
        return k;
    }();
    return type < component::kCount && known[type];
}

Verdict value_from_nbt(u32 type, Tag value, base::ByteBuf& out) {
    if (!value) return kBad;
    base::ByteBuf raw;
    Out to_raw{raw};
    if (const Verdict v = from(type, value, to_raw); v != kOk) return v;
    return check_value(type, {raw.data, raw.len}, out);
}

void value_to_nbt(u32 type, std::span<const u8> value, nbt::Writer& w, std::string_view name) {
    Reader in(value);
    to(type, in, w, name);
}

Verdict patch_from_nbt(Tag components, PatchId& out) {
    out = 0;
    if (components.type != nbt::kCompound) return kBad;
    PatchBuilder patch;
    base::ByteBuf value;
    Verdict verdict = kOk;
    for (const Tag e : components.entries()) {
        if (components.get(e.name()).at != e.at) continue;  // a name twice: the last one counts
        // DataComponentPatch.PatchKey.CODEC: "type", or "!type" for one that is taken away.
        std::string key = nbt::to_utf8(e.name());
        const bool removed = key.starts_with('!');
        const u32 type = find_component(std::string_view(key).substr(removed));
        if (type == kNoComponent) return kBad;
        if (removed) {
            patch.remove(type);  // (its value is Codec.EMPTY: anything)
            continue;
        }
        value.clear();
        const Verdict v = value_from_nbt(type, e, value);
        if (v == kBad) return v;
        if (v == kNotRead) verdict = v;
        patch.set(type, {value.data, value.len});
    }
    return verdict == kOk ? patch.build(out) : verdict;
}

Verdict stack_from_nbt(Tag tag, ItemStack& out) {
    out = {};
    if (tag.type != nbt::kCompound) return kBad;
    // ItemStack.MAP_CODEC: id (Item.CODEC: not air), count (1..99, 1 if left out), components.
    std::string name;
    if (!as_identifier(tag.get("id"), name)) return kBad;
    const u32 item = world::find_item(name);
    i32 count;
    if (item == world::kNoId || item == 0 || !optional(tag, "count", count, 1, as_int) || count < 1 || count > 99) return kBad;
    PatchId patch = 0;
    if (const Tag components = tag.get("components")) {
        if (const Verdict v = patch_from_nbt(components, patch); v != kOk) return v;
    }
    out = ItemStack(item, static_cast<u32>(count), sanitized(item, patch));
    return kOk;
}

// ---------------------------------------------------------------------------- what an item has of its own

namespace {

struct Defaults {
    struct Entry {
        u16 type;
        bool read;
        u32 off, len;
    };
    std::vector<u32> first;  // of item i: entries[first[i] .. first[i + 1])
    std::vector<Entry> entries;
    base::ByteBuf values;
    u32 unread = 0;

    Defaults() {
        Reader r(proto::item_defaults());
        nbt::Doc doc;
        while (r.remaining() > 0) {
            first.push_back(static_cast<u32>(entries.size()));
            for (u32 n = r.varint(); n > 0; --n) {
                const u32 type = r.varint(), len = r.varint();
                const std::span<const u8> tag = r.bytes(len);
                if (!r.ok || type >= component::kCount) std::abort();  // generated data is corrupt: refuse to start
                const u32 at = values.len;
                const bool read = has_codec(type) && doc.parse(tag, nbt::Form::kNameless, nbt::kTrusted) == nbt::Error::kNone &&
                                  value_from_nbt(type, doc.root(), values) == kOk;
                if (!read) values.len = at;
                unread += has_codec(type) && !read;
                entries.push_back({static_cast<u16>(type), read, at, values.len - at});
            }
        }
        first.push_back(static_cast<u32>(entries.size()));
    }
};

const Defaults& defaults() {
    static const Defaults table;
    return table;
}

}  // namespace

Default default_of(u32 item, u32 type) {
    const Defaults& d = defaults();
    if (item + 1 >= d.first.size()) return {};
    for (u32 i = d.first[item]; i < d.first[item + 1]; ++i) {
        const Defaults::Entry& e = d.entries[i];
        if (e.type == type) return {true, e.read, {d.values.data + e.off, e.len}};
    }
    return {};
}

u32 unread_defaults() { return defaults().unread; }

Patches::Found effective(const ItemStack& stack, u32 type) {
    if (stack.patch()) {
        const Patches::Found f = patches().find(stack.patch(), type);
        if (f.state != Patches::State::kInherit) return f;
    }
    const Default d = default_of(stack.item(), type);
    return d.read ? Patches::Found{Patches::State::kSet, d.value} : Patches::Found{};
}

PatchId sanitized(u32 item, PatchId patch) {
    if (!patch) return 0;
    Patches& store = patches();
    bool changes = false;
    for (const Patches::Entry& e : store.entries(patch)) {
        const Default d = default_of(item, e.type);
        const std::span<const u8> value = store.wire(patch).subspan(e.off, e.len);
        changes |= e.removed
                       ? !d.has
                       : d.read && d.value.size() == value.size() && std::equal(value.begin(), value.end(), d.value.begin());
    }
    if (!changes) return patch;
    PatchBuilder kept;
    for (const Patches::Entry& e : store.entries(patch)) {
        const Default d = default_of(item, e.type);
        const std::span<const u8> value = store.wire(patch).subspan(e.off, e.len);
        if (e.removed) {
            if (d.has) kept.remove(e.type);
        } else if (!(d.read && d.value.size() == value.size() && std::equal(value.begin(), value.end(), d.value.begin()))) {
            kept.set(e.type, value);
        }
    }
    PatchId out = 0;
    kept.build(out);  // (smaller than the patch it comes from: it fits)
    store.release(patch);
    return out;
}

std::string computed_name_key(const ItemStack& stack) {
    namespace it = proto::gen::item;
    const u32 item = stack.item();
    std::string key(world::item_text_key(item));
    if (item == it::kShield) {  // ShieldItem.getName
        const Patches::Found colour = effective(stack, component::kBaseColor);
        if (colour.state != Patches::State::kSet) return {};
        return key + "." + std::string(kDyes[colour.value[0]]);
    }
    if (item != it::kPotion && item != it::kSplashPotion && item != it::kLingeringPotion && item != it::kTippedArrow) return {};
    // PotionItem / TippedArrowItem.getName -> PotionContents.getName: the custom name, else the
    // potion's (Potion.name: the long and the strong one are named as the plain one), else "empty".
    const Patches::Found contents = effective(stack, component::kPotionContents);
    if (contents.state != Patches::State::kSet) return {};
    Reader in(contents.value);
    std::string suffix = "empty";
    if (in.u8v()) {
        std::string_view name = sreg::kPotionNames[in.varint()];
        name.remove_prefix(name.find(':') + 1);
        for (const std::string_view variant : {"long_", "strong_"})
            if (name.starts_with(variant)) name.remove_prefix(variant.size());
        suffix = name;
    }
    if (in.u8v()) in.be32();
    for (u32 effects = in.varint(); effects > 0; --effects) {
        in.varint();
        for (bool more = true; more;) {  // the details, and those of the effects hidden beneath
            in.varint();
            in.varint();
            in.u8v();
            in.u8v();
            in.u8v();
            more = in.u8v() != 0;
        }
    }
    if (in.u8v()) suffix = take_str(in);
    return key + ".effect." + suffix;
}

std::string book_title(const ItemStack& stack) {
    const Patches::Found book = effective(stack, component::kWrittenBookContent);
    if (book.state != Patches::State::kSet) return {};
    Reader in(book.value);
    const std::string_view title = take_str(in);
    // StringUtil.isBlank
    const bool blank = std::all_of(title.begin(), title.end(), [](char c) { return c == ' ' || (c >= '\t' && c <= '\r'); });
    return blank ? std::string() : std::string(title);
}

void patch_to_nbt(PatchId patch, nbt::Writer& w) {
    const Patches& store = patches();
    for (const Patches::Entry& e : store.entries(patch)) {
        if (e.removed) {
            std::string key = "!";
            key += component::kNames[e.type];
            w.begin_compound(key);
            w.end_compound();
        } else {
            value_to_nbt(e.type, store.wire(patch).subspan(e.off, e.len), w, component::kNames[e.type]);
        }
    }
}

void stack_to_nbt(const ItemStack& stack, nbt::Writer& w) {
    w.put_string("id", world::item_name(stack.item()));
    w.put_int("count", static_cast<i32>(stack.count()));
    if (!stack.patch()) return;
    w.begin_compound("components");
    patch_to_nbt(stack.patch(), w);
    w.end_compound();
}

void patch_to_wire_nbt(PatchId patch, base::ByteBuf& out) {
    base::ByteBuf plain;
    nbt::Writer w(plain);
    w.root(nbt::kCompound);
    if (patch) patch_to_nbt(patch, w);
    w.end_compound();
    nbt::Doc doc;
    doc.parse({plain.data, plain.len}, nbt::Form::kNameless, nbt::kTrusted);
    *out.reserve(1) = nbt::kCompound;
    ++out.len;
    nbt::write_map_order(doc.root(), out);
}

text::Result hover_components(Tag components, base::ByteBuf& out) {
    PatchId patch = 0;
    switch (patch_from_nbt(components, patch)) {
        case kOk: break;
        case kBad: return text::Result::kInvalid;
        default: return text::Result::kUnsupported;
    }
    base::ByteBuf tag;
    patch_to_wire_nbt(patch, tag);
    if (patch) patches().release(patch);
    out.append(tag.data + 1, tag.len - 1);  // the payload
    return text::Result::kOk;
}

}  // namespace item
