#include "item/components.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>

#include "nbt/nbt.hpp"
#include "proto/identifier.hpp"
#include "text/component.hpp"

#include PROTO_GEN_ENTITIES_HPP

namespace item {

using proto::Reader;
namespace reg = proto::reg;
namespace sreg = proto::gen::static_registry;
namespace entity_type = proto::gen::entity_type;

namespace {

constexpr u32 kMaxString = 32767;  // ByteBufCodecs.STRING_UTF8: at most this many UTF-16 units, in at most 3x the bytes
// MobEffectInstance.Details nests through its "hidden effect"; vanilla's decoder recurses for as
// long as the stack lasts. Beacons and potions nest one level.
constexpr u32 kMaxEffectDepth = 8;
constexpr u32 kMaxBlockStateProperties = 16;  // the block with the most properties has 9

// The re-encoded value: small appends to a buffer that keeps kSlack bytes writable past every reserve.
struct Out {
    base::ByteBuf& b;
    void u8v(u8 v) {
        *b.reserve(1) = v;
        ++b.len;
    }
    void varint(u32 v) { b.commit(proto::put_varint(b.reserve(proto::kVarIntMaxBytes), v)); }
    void be32(u32 v) { b.commit(proto::put_be32(b.reserve(4), v)); }
    void bytes(std::span<const u8> s) { b.append(s.data(), s.size()); }
};

ALWAYS_INLINE i32 vint(Reader& in) { return static_cast<i32>(in.varint()); }

// The element count of a collection or map: ByteBufCodecs.readCount, then a container made for
// min(count, 65536) elements - whose constructor throws for a negative count. Every element
// read here takes at least one byte, so a count beyond what is left is a short packet.
bool read_count(Reader& in, u32 max, u32& n) {
    const i32 c = vint(in);
    if (!in.ok || c < 0 || static_cast<u32>(c) > max || static_cast<u32>(c) > in.remaining()) return false;
    n = static_cast<u32>(c);
    return true;
}

// ---------------------------------------------------------------------------- value decoders
//
// One per StreamCodec shape. Each reads a value from `in` - the bytes the client said the value
// has - and writes what vanilla would write for the decoded value. A decoder that runs past the
// end leaves in.ok false, which the caller turns into kMalformed; bytes it does not read are
// dropped, as ByteBufCodecs.lengthPrefixed drops them.

// ByteBufCodecs.VAR_INT under ExtraCodecs.intRange(lo, hi) (or NON_NEGATIVE_INT / POSITIVE_INT).
Verdict t_int_range(Reader& in, Out& out, i32 lo, i32 hi) {
    const i32 v = vint(in);
    if (!in.ok || v < lo || v > hi) return Verdict::kMalformed;
    out.varint(static_cast<u32>(v));
    return Verdict::kOk;
}

Verdict t_varint(Reader& in, Out& out) {
    out.varint(in.varint());
    return Verdict::kOk;
}

Verdict t_bool(Reader& in, Out& out) {
    out.u8v(in.u8v() != 0);
    return Verdict::kOk;
}

// ByteBufCodecs.INT
Verdict t_int(Reader& in, Out& out) {
    out.be32(in.be32());
    return Verdict::kOk;
}

// ByteBufCodecs.idMapper over ByIdMap.continuous(..., ZERO): every id decodes, and one outside
// the enum is its first value - which is then what vanilla writes back.
Verdict t_enum(Reader& in, Out& out, u32 values) {
    const u32 id = in.varint();
    out.varint(id < values ? id : 0);
    return Verdict::kOk;
}

// ByteBufCodecs.FLOAT under ExtraCodecs.floatRange: the bounds are tested with Float.compareTo,
// for which NaN is above everything and -0.0 is below 0.0.
Verdict t_float_range(Reader& in, Out& out, float lo, float hi) {
    const u32 bits = in.be32();
    const float f = std::bit_cast<float>(bits);
    if (!in.ok || std::isnan(f) || f < lo || f > hi || (f == lo && std::signbit(f) && !std::signbit(lo)))
        return Verdict::kMalformed;
    out.be32(bits);
    return Verdict::kOk;
}

// ByteBufCodecs.holderRegistry: an id into a registry; Registry.byIdOrThrow.
Verdict t_registry_id(Reader& in, Out& out, u32 size) {
    const u32 id = in.varint();
    if (!in.ok || id >= size) return Verdict::kMalformed;
    out.varint(id);
    return Verdict::kOk;
}

// ByteBufCodecs.holder(registry, directCodec): id + 1, or 0 and the value itself. The inline
// values of the registries used here (instruments, paintings, banner patterns) hold text
// components or free-form assets, which nothing here checks yet.
Verdict t_holder_reference(Reader& in, Out& out, u32 size) {
    const u32 id = in.varint();
    if (!in.ok) return Verdict::kMalformed;
    if (id == 0) return Verdict::kUnsupported;
    if (id - 1 >= size) return Verdict::kMalformed;
    out.varint(id);
    return Verdict::kOk;
}

// How long a Java String these bytes make, in UTF-16 units - or -1 if they are not well-formed
// UTF-8. Vanilla decodes such bytes all the same, a U+FFFD for every fault, and so writes
// back other bytes than it read: not what this server keeps.
i32 utf16_length(std::span<const u8> s) {
    u32 units = 0;
    for (usize i = 0; i < s.size();) {
        const u8 c = s[i];
        if (c < 0x80) {
            ++i;
            ++units;
            continue;
        }
        const u32 more = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 0;
        if (more == 0 || s.size() - i <= more) return -1;
        u32 cp = c & (0x3F >> more);
        for (u32 k = 1; k <= more; ++k) {
            if ((s[i + k] & 0xC0) != 0x80) return -1;
            cp = cp << 6 | (s[i + k] & 0x3F);
        }
        constexpr u32 kSmallest[4] = {0, 0x80, 0x800, 0x10000};
        if (cp < kSmallest[more] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return -1;
        units += cp >= 0x10000 ? 2 : 1;
        i += more + 1;
    }
    return static_cast<i32>(units);
}

// ByteBufCodecs.stringUtf8(max) (Utf8String.read): at most 3 * max bytes that decode to at most
// `max` UTF-16 units. STRING_UTF8 is max = 32767; nothing that long fits a patch anyway.
Verdict t_string(Reader& in, Out& out, u32 max = kMaxString) {
    const i32 n = vint(in);
    if (!in.ok || n < 0 || static_cast<u32>(n) > max * 3 || static_cast<u32>(n) > in.remaining()) return Verdict::kMalformed;
    const std::span<const u8> bytes = in.bytes(static_cast<u32>(n));
    const i32 units = utf16_length(bytes);
    if (units < 0) return Verdict::kUnsupported;
    if (static_cast<u32>(units) > max) return Verdict::kMalformed;
    if (static_cast<u32>(n) > kMaxString) return Verdict::kUnsupported;
    out.varint(static_cast<u32>(n));
    out.bytes(bytes);
    return Verdict::kOk;
}

// Identifier.STREAM_CODEC: a string through Identifier.parse, written back as "namespace:path".
Verdict t_identifier(Reader& in, Out& out) {
    const std::string_view s = in.str(kMaxString);
    proto::Identifier id;
    if (!in.ok || !proto::parse_identifier(s, id)) return Verdict::kMalformed;
    out.varint(static_cast<u32>(id.size()));
    out.b.commit(id.put(out.b.reserve(id.size())));
    return Verdict::kOk;
}

// MobEffectInstance.Details.STREAM_CODEC: amplifier, duration, ambient, show particles, show
// icon, then optionally the same again for the effect hidden beneath. The MobEffectInstance
// constructor clamps the amplifier to 0..255.
Verdict t_effect_details(Reader& in, Out& out) {
    for (u32 depth = 0;; ++depth) {
        out.varint(static_cast<u32>(std::clamp(vint(in), 0, 255)));
        out.varint(in.varint());
        out.u8v(in.u8v() != 0);
        out.u8v(in.u8v() != 0);
        out.u8v(in.u8v() != 0);
        const bool hidden = in.u8v() != 0;
        if (!in.ok) return Verdict::kMalformed;
        out.u8v(hidden);
        if (!hidden) return Verdict::kOk;
        if (depth == kMaxEffectDepth) return Verdict::kUnsupported;
    }
}

// MobEffectInstance.STREAM_CODEC
Verdict t_effect(Reader& in, Out& out) {
    if (const Verdict v = t_registry_id(in, out, sreg::kMobEffectCount); v != Verdict::kOk) return v;
    return t_effect_details(in, out);
}

// PotionContents.STREAM_CODEC: optional potion, optional custom colour, custom effects, optional custom name.
Verdict t_potion_contents(Reader& in, Out& out) {
    const bool potion = in.u8v() != 0;
    out.u8v(potion);
    if (potion) {
        if (const Verdict v = t_registry_id(in, out, sreg::kPotionCount); v != Verdict::kOk) return v;
    }
    const bool colour = in.u8v() != 0;
    out.u8v(colour);
    if (colour) out.be32(in.be32());
    u32 effects;
    if (!read_count(in, INT32_MAX, effects)) return Verdict::kMalformed;
    out.varint(effects);
    for (u32 i = 0; i < effects; ++i) {
        if (const Verdict v = t_effect(in, out); v != Verdict::kOk) return v;
    }
    const bool name = in.u8v() != 0;
    if (!in.ok) return Verdict::kMalformed;
    out.u8v(name);
    return name ? t_string(in, out) : Verdict::kOk;
}

// SuspiciousStewEffects.STREAM_CODEC: a list of (effect, duration).
Verdict t_stew_effects(Reader& in, Out& out) {
    u32 n;
    if (!read_count(in, INT32_MAX, n)) return Verdict::kMalformed;
    out.varint(n);
    for (u32 i = 0; i < n; ++i) {
        if (const Verdict v = t_registry_id(in, out, sreg::kMobEffectCount); v != Verdict::kOk) return v;
        out.varint(in.varint());
    }
    return Verdict::kOk;
}

// ItemEnchantments.STREAM_CODEC: a map enchantment -> level; the Codec wants levels of 1..255.
// A map: a key sent twice keeps its last value, and the order is nobody's - written by id.
Verdict t_enchantments(Reader& in, Out& out) {
    u32 n;
    if (!read_count(in, INT32_MAX, n)) return Verdict::kMalformed;
    u8 level[reg::enchantment::kCount] = {};
    for (u32 i = 0; i < n; ++i) {
        const u32 id = in.varint();
        const i32 lv = vint(in);
        if (!in.ok || id >= reg::enchantment::kCount || lv < 1 || lv > 255) return Verdict::kMalformed;
        level[id] = static_cast<u8>(lv);
    }
    out.varint(static_cast<u32>(std::count_if(std::begin(level), std::end(level), [](u8 l) { return l != 0; })));
    for (u32 id = 0; id < reg::enchantment::kCount; ++id) {
        if (!level[id]) continue;
        out.varint(id);
        out.varint(level[id]);
    }
    return Verdict::kOk;
}

// TooltipDisplay.STREAM_CODEC: hide_tooltip, then a set of component types - written by id.
Verdict t_tooltip_display(Reader& in, Out& out) {
    out.u8v(in.u8v() != 0);
    u32 n;
    if (!read_count(in, INT32_MAX, n)) return Verdict::kMalformed;
    bool hidden[component::kCount] = {};
    for (u32 i = 0; i < n; ++i) {
        const u32 type = in.varint();
        if (!in.ok || type >= component::kCount) return Verdict::kMalformed;
        hidden[type] = true;
    }
    out.varint(static_cast<u32>(std::count(std::begin(hidden), std::end(hidden), true)));
    for (u32 type = 0; type < component::kCount; ++type)
        if (hidden[type]) out.varint(type);
    return Verdict::kOk;
}

// FireworkExplosion.STREAM_CODEC: shape, colours, fade colours, trail, twinkle.
Verdict t_firework_explosion(Reader& in, Out& out) {
    t_enum(in, out, 5);  // FireworkExplosion.Shape
    for (int list = 0; list < 2; ++list) {
        u32 n;
        if (!read_count(in, INT32_MAX, n) || n * 4ull > in.remaining()) return Verdict::kMalformed;
        out.varint(n);
        out.bytes(in.bytes(n * 4));
    }
    out.u8v(in.u8v() != 0);
    out.u8v(in.u8v() != 0);
    return Verdict::kOk;
}

// Fireworks.STREAM_CODEC: flight duration (an unsigned byte for the Codec), at most 256 explosions.
Verdict t_fireworks(Reader& in, Out& out) {
    const i32 flight = vint(in);
    if (!in.ok || flight > 255) return Verdict::kMalformed;
    if (flight < 0)
        return Verdict::kUnsupported;  // ExtraCodecs.UNSIGNED_BYTE lets a negative int through; nothing sane sends one
    out.varint(static_cast<u32>(flight));
    u32 n;
    if (!read_count(in, 256, n)) return Verdict::kMalformed;
    out.varint(n);
    for (u32 i = 0; i < n; ++i) {
        if (const Verdict v = t_firework_explosion(in, out); v != Verdict::kOk) return v;
    }
    return Verdict::kOk;
}

// BannerPatternLayers.STREAM_CODEC: a list of (pattern, colour), bottom layer first.
Verdict t_banner_patterns(Reader& in, Out& out) {
    u32 n;
    if (!read_count(in, INT32_MAX, n)) return Verdict::kMalformed;
    out.varint(n);
    for (u32 i = 0; i < n; ++i) {
        if (const Verdict v = t_holder_reference(in, out, reg::banner_pattern::kCount); v != Verdict::kOk) return v;
        t_enum(in, out, 16);  // DyeColor
    }
    return Verdict::kOk;
}

// BlockItemStateProperties.STREAM_CODEC: a map of property name -> value, both plain strings.
// A map: written sorted by name, a name sent twice keeps its last value.
Verdict t_block_state(Reader& in, Out& out) {
    u32 n;
    if (!read_count(in, INT32_MAX, n)) return Verdict::kMalformed;
    struct Property {
        std::string_view name, value;
        u32 order;
    } props[kMaxBlockStateProperties];
    const auto string = [&](std::string_view& s) {
        const u32 len = in.varint();
        if (!in.ok || len > kMaxString * 3 || len > in.remaining()) return Verdict::kMalformed;
        if (len > kMaxString) return Verdict::kUnsupported;
        const auto bytes = in.bytes(len);
        if (utf16_length(bytes) < 0) return Verdict::kUnsupported;
        s = {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
        return Verdict::kOk;
    };
    Verdict verdict = Verdict::kOk;
    for (u32 i = 0; i < n; ++i) {
        std::string_view name, value;
        for (std::string_view* s : {&name, &value}) {
            const Verdict v = string(*s);
            if (v == Verdict::kMalformed) return v;
            if (v == Verdict::kUnsupported) verdict = v;
        }
        if (i < kMaxBlockStateProperties) props[i] = {name, value, i};
    }
    if (n > kMaxBlockStateProperties || verdict != Verdict::kOk) return Verdict::kUnsupported;
    std::sort(props, props + n,
              [](const Property& a, const Property& b) { return a.name != b.name ? a.name < b.name : a.order < b.order; });
    u32 distinct = 0;
    for (u32 i = 0; i < n; ++i) distinct += i + 1 == n || props[i].name != props[i + 1].name;
    out.varint(distinct);
    for (u32 i = 0; i < n; ++i) {
        if (i + 1 < n && props[i].name == props[i + 1].name) continue;
        for (const std::string_view s : {props[i].name, props[i].value}) {
            out.varint(static_cast<u32>(s.size()));
            out.bytes({reinterpret_cast<const u8*>(s.data()), s.size()});
        }
    }
    return Verdict::kOk;
}

// ---------------------------------------------------------------------------- NBT and text
//
// What travels as a tag: custom data and the data of entities and block entities, which stay
// tags; text components, which a codec reads out of one (text/component.hpp).

// The tag being read. One at a time: a decoder is done with it before the next tag is parsed.
nbt::Doc the_tag;

// A value this much longer than a whole patch may be is not looked at: it could only shrink to
// fit by being mostly things vanilla drops, and it is the tick thread that would do the reading.
constexpr u32 kMaxTagInput = 16 * kMaxPatchBytes;

// ByteBufCodecs.TAG: one tag in the network form, under NbtAccounter.defaultQuota().
Verdict read_tag(Reader& in) {
    if (in.remaining() > kMaxTagInput) return Verdict::kUnsupported;
    const nbt::Error error = the_tag.parse({in.p, in.remaining()});
    if (error == nbt::Error::kTooBig) return Verdict::kUnsupported;  // counted a little high: see nbt::Error
    if (error != nbt::Error::kNone) return Verdict::kMalformed;
    in.p += the_tag.size();
    if (!the_tag.root()) return Verdict::kMalformed;  // TAG_End: "Expected non-null compound tag"
    // Names are compared and strings copied as bytes from here on, which is right for the bytes
    // Java writes and for no others.
    return the_tag.plain_strings() ? Verdict::kOk : Verdict::kUnsupported;
}

// ByteBufCodecs.COMPOUND_TAG: a tag that has to be a compound. Kept as vanilla would write the
// compound it loaded, except that its entries are in name order (nbt::Doc::write).
// `or_snbt`: custom_data, which has no StreamCodec of its own and goes as the tag its Codec
// makes (ByteBufCodecs.fromCodecWithRegistries(CustomData.CODEC)). That is the same compound -
// but the Codec also takes a string of SNBT in its place (TagParser.FLATTENED_CODEC), which no
// client sends and nothing here parses.
Verdict t_compound(Reader& in, Out& out, bool or_snbt = false) {
    if (const Verdict v = read_tag(in); v != Verdict::kOk) return v;
    const nbt::Tag root = the_tag.root();
    if (or_snbt && root.type == nbt::kString) return Verdict::kUnsupported;
    if (root.type != nbt::kCompound) return Verdict::kMalformed;
    out.u8v(nbt::kCompound);
    the_tag.write(root, out.b);
    return Verdict::kOk;
}

// TypedEntityData.streamCodec: the type's registry id, then the compound. The id goes through
// ByteBufCodecs.registry, which is Registry.byIdOrThrow - and the registry of entity types is a
// DefaultedRegistry: an id it does not have is not an error there but its default, the pig.
// (Block entity types have no default. Nor does holderRegistry ever give one: see t_registry_id.)
constexpr u32 kNoDefault = ~0u;
Verdict t_typed_data(Reader& in, Out& out, u32 types, u32 fallback) {
    const u32 id = in.varint();
    if (!in.ok || (id >= types && fallback == kNoDefault)) return Verdict::kMalformed;
    out.varint(id < types ? id : fallback);
    return t_compound(in, out);
}

// ComponentSerialization.STREAM_CODEC: a tag, read by the component codec and written by it.
Verdict t_text(Reader& in, Out& out) {
    if (const Verdict v = read_tag(in); v != Verdict::kOk) return v;
    switch (text::transcode(the_tag.root(), out.b)) {
        case text::Result::kOk: return Verdict::kOk;
        case text::Result::kInvalid: return Verdict::kMalformed;
        default: return Verdict::kUnsupported;
    }
}

// ItemLore.STREAM_CODEC: at most 256 lines.
Verdict t_lore(Reader& in, Out& out) {
    u32 lines;
    if (!read_count(in, 256, lines)) return Verdict::kMalformed;
    out.varint(lines);
    for (u32 i = 0; i < lines; ++i) {
        if (const Verdict v = t_text(in, out); v != Verdict::kOk) return v;
    }
    return Verdict::kOk;
}

// Filterable.streamCodec: a value, and optionally the same again as a chat filter left it.
template <class Value>
Verdict t_filterable(Reader& in, Out& out, Value&& value) {
    if (const Verdict v = value(); v != Verdict::kOk) return v;
    const bool filtered = in.u8v() != 0;
    if (!in.ok) return Verdict::kMalformed;
    out.u8v(filtered);
    return filtered ? value() : Verdict::kOk;
}

// WritableBookContent.STREAM_CODEC: at most 100 pages of at most 1024 characters.
Verdict t_writable_book(Reader& in, Out& out) {
    u32 pages;
    if (!read_count(in, 100, pages)) return Verdict::kMalformed;
    out.varint(pages);
    for (u32 i = 0; i < pages; ++i) {
        if (const Verdict v = t_filterable(in, out, [&] { return t_string(in, out, 1024); }); v != Verdict::kOk) return v;
    }
    return Verdict::kOk;
}

// WrittenBookContent.STREAM_CODEC: title (32 characters), author, generation, pages, resolved.
// The Codec wants a generation of 0 .. 3 (original .. tattered).
Verdict t_written_book(Reader& in, Out& out) {
    if (const Verdict v = t_filterable(in, out, [&] { return t_string(in, out, 32); }); v != Verdict::kOk) return v;
    if (const Verdict v = t_string(in, out); v != Verdict::kOk) return v;
    if (const Verdict v = t_int_range(in, out, 0, 3); v != Verdict::kOk) return v;
    u32 pages;
    if (!read_count(in, INT32_MAX, pages)) return Verdict::kMalformed;
    out.varint(pages);
    for (u32 i = 0; i < pages; ++i) {
        if (const Verdict v = t_filterable(in, out, [&] { return t_text(in, out); }); v != Verdict::kOk) return v;
    }
    return t_bool(in, out);
}

// SignText.STREAM_CODEC: four lines, optionally the four as filtered, the dye, the glow.
// SignText.load takes missing filtered lines to be the lines, and filtered lines that equal
// the lines are not written (filteredMessagesForSerialization). Equal here means equal as
// written, which is what equal components are - but for the few that vanilla holds equal and
// writes differently (a colour by name and the same by number).
Verdict t_sign_text(Reader& in, Out& out) {
    const auto lines = [&] {
        for (u32 i = 0; i < 4; ++i) {
            if (const Verdict v = t_text(in, out); v != Verdict::kOk) return v;
        }
        return Verdict::kOk;
    };
    const u32 at = out.b.len;
    if (const Verdict v = lines(); v != Verdict::kOk) return v;
    const u32 size = out.b.len - at;
    const bool filtered = in.u8v() != 0;
    if (!in.ok) return Verdict::kMalformed;
    out.u8v(filtered);
    if (filtered) {
        const u32 again = out.b.len;
        if (const Verdict v = lines(); v != Verdict::kOk) return v;
        if (out.b.len - again == size && std::memcmp(out.b.data + at, out.b.data + again, size) == 0) {
            out.b.len = again;
            out.b.data[again - 1] = 0;
        }
    }
    t_enum(in, out, 16);  // DyeColor
    return t_bool(in, out);
}

// The components this server can vouch for, each by the codecs DataComponents registers for it
// (26.3): `P` the persistent Codec (value ranges), `N` the network StreamCodec (layout).
// Everything else - stacks inside stacks (containers, bundles, charged projectiles), the
// behaviour components (food, tool, equippable, attribute_modifiers, ...), and what has only a
// Codec and so travels as the tag it makes (map_decorations, lock, recipes, ...; custom_data
// is the one of those that is read) - is kUnsupported until it is read too.
Verdict transcode(u32 type, Reader& in, Out& out) {
    switch (type) {
        case component::kCustomName:
        case component::kItemName: return t_text(in, out);  // P ComponentSerialization.CODEC, N the same as a tag
        case component::kLore: return t_lore(in, out);
        case component::kCustomData: return t_compound(in, out, true);  // P CustomData.CODEC, and no N
        case component::kBucketEntityData: return t_compound(in, out);  // N CustomData.STREAM_CODEC
        case component::kEntityData: return t_typed_data(in, out, entity_type::kCount, entity_type::kPig);
        case component::kBlockEntityData: return t_typed_data(in, out, sreg::kBlockEntityTypeCount, kNoDefault);
        case component::kWritableBookContent: return t_writable_book(in, out);
        case component::kWrittenBookContent: return t_written_book(in, out);
        case component::kSignTextFront:
        case component::kSignTextBack: return t_sign_text(in, out);
        case component::kMaxStackSize: return t_int_range(in, out, 1, 99);           // P intRange(1, 99), N VAR_INT
        case component::kMaxDamage: return t_int_range(in, out, 1, INT32_MAX);       // P POSITIVE_INT
        case component::kDamage: return t_int_range(in, out, 0, INT32_MAX);          // P NON_NEGATIVE_INT
        case component::kRepairCost: return t_int_range(in, out, 0, INT32_MAX);      // P NON_NEGATIVE_INT
        case component::kOminousBottleAmplifier: return t_int_range(in, out, 0, 4);  // P intRange(0, 4)
        case component::kMapId: return t_varint(in, out);                            // P INT, N VAR_INT
        case component::kUnbreakable:
        case component::kGlider:
        case component::kWaxed: return Verdict::kOk;  // Unit: no bytes
        case component::kEnchantmentGlintOverride: return t_bool(in, out);
        case component::kMinimumAttackCharge: return t_float_range(in, out, 0.0f, 1.0f);     // P floatRange(0, 1)
        case component::kPotionDurationScale: return t_float_range(in, out, 0.0f, FLT_MAX);  // P NON_NEGATIVE_FLOAT
        case component::kDyedColor: return t_int(in, out);                                   // N INT; P takes any int
        case component::kRarity: return t_enum(in, out, 4);                                  // Rarity
        case component::kDye:
        case component::kBaseColor:
        case component::kWolfCollar:
        case component::kCatCollar:
        case component::kSheepColor:
        case component::kShulkerColor:
        case component::kTropicalFishBaseColor:
        case component::kTropicalFishPatternColor:
        case component::kCushionColor: return t_enum(in, out, 16);  // DyeColor
        case component::kItemModel:
        case component::kTooltipStyle:
        case component::kNoteBlockSound: return t_identifier(in, out);
        case component::kEnchantments:
        case component::kStoredEnchantments: return t_enchantments(in, out);
        case component::kTooltipDisplay: return t_tooltip_display(in, out);
        case component::kPotionContents: return t_potion_contents(in, out);
        case component::kSuspiciousStewEffects: return t_stew_effects(in, out);
        case component::kFireworkExplosion: return t_firework_explosion(in, out);
        case component::kFireworks: return t_fireworks(in, out);
        case component::kInstrument: return t_holder_reference(in, out, reg::instrument::kCount);
        case component::kPaintingVariant: return t_holder_reference(in, out, reg::painting_variant::kCount);
        case component::kBannerPatterns: return t_banner_patterns(in, out);
        case component::kBlockState: return t_block_state(in, out);
        default: return Verdict::kUnsupported;
    }
}

u32 hash_bytes(std::span<const u8> s) {  // FNV-1a, folded: patches are tens of bytes
    u64 h = 0xCBF29CE484222325ull;
    for (const u8 b : s) h = (h ^ b) * 0x100000001B3ull;
    return static_cast<u32>(h ^ (h >> 32));
}

Patches the_patches;

}  // namespace

Patches& patches() { return the_patches; }

Patches::Patches() : slots_(1, nullptr), table_(64, 0) {}

Patches::~Patches() {
    // Every stack is gone by now; a patch still held is a reference somebody did not give back.
    DCHECK(live_ == 0);
    for (Patch* p : slots_) std::free(p);
}

Verdict Patches::read_untrusted(Reader& r, PatchId& out) {
    out = 0;
    const i32 added = vint(r), removed = vint(r);
    // An added entry is at least a type and a length, a removed one a type.
    if (!r.ok || added < 0 || removed < 0 || 2ull * static_cast<u32>(added) + static_cast<u32>(removed) > r.remaining())
        return Verdict::kMalformed;
    if (added == 0 && removed == 0) return Verdict::kOk;

    // Entries land in a map on vanilla's side: of two for one type the later wins, and a removal
    // (they are read after the values) beats a value.
    struct Slot {
        u8 state;  // 0 absent, 1 set, 2 removed
        u32 off, len;
    } slot[component::kCount] = {};
    Verdict verdict = Verdict::kOk;
    values_.clear();
    Out values{values_};
    for (i32 i = 0; i < added; ++i) {
        const u32 type = r.varint(), len = r.varint();
        if (!r.ok || type >= component::kCount || len > r.remaining()) return Verdict::kMalformed;
        Reader in(r.bytes(len));
        if (values_.len > 16 * kMaxPatchBytes) {  // already far too big to keep: stop copying, keep walking
            verdict = Verdict::kUnsupported;
            continue;
        }
        const u32 at = values_.len;
        Verdict v = transcode(type, in, values);
        if (v == Verdict::kOk && !in.ok) v = Verdict::kMalformed;  // the value is longer than the client said
        if (v == Verdict::kMalformed) return v;
        if (v == Verdict::kUnsupported)
            verdict = v;
        else
            slot[type] = {1, at, values_.len - at};
    }
    for (i32 i = 0; i < removed; ++i) {
        const u32 type = r.varint();
        if (!r.ok || type >= component::kCount) return Verdict::kMalformed;
        slot[type] = {2, 0, 0};
    }
    if (verdict != Verdict::kOk) return verdict;

    // DataComponentPatch.STREAM_CODEC: both counts, the added entries, the removed types. The
    // order within each is free on the wire; by type here, so that equal patches are equal bytes.
    Entry index[component::kCount];
    u32 n = 0, set = 0;
    for (const Slot& s : slot) set += s.state == 1;
    wire_.clear();
    Out wire{wire_};
    wire.varint(set);
    wire.varint(static_cast<u32>(std::count_if(std::begin(slot), std::end(slot), [](const Slot& s) { return s.state == 2; })));
    for (u32 type = 0; type < component::kCount; ++type) {
        if (slot[type].state != 1) continue;
        wire.varint(type);
        index[n++] = {static_cast<u16>(type), false, wire_.len, slot[type].len};
        wire.bytes({values_.data + slot[type].off, slot[type].len});
    }
    for (u32 type = 0; type < component::kCount; ++type) {
        if (slot[type].state != 2) continue;
        wire.varint(type);
        index[n++] = {static_cast<u16>(type), true, 0, 0};
    }
    if (wire_.len > kMaxPatchBytes) return Verdict::kUnsupported;
    out = intern({index, n}, {wire_.data, wire_.len});
    return Verdict::kOk;
}

Patches::Found Patches::find(PatchId id, u32 type) const {
    const Patch& p = *slots_[id];
    for (const Entry& e : std::span<const Entry>(p.index(), p.entries)) {
        if (e.type != type) continue;
        return e.removed ? Found{State::kRemoved, {}} : Found{State::kSet, {p.wire() + e.off, e.len}};
    }
    return {};
}

PatchId Patches::intern(std::span<const Entry> index, std::span<const u8> wire) {
    const u32 hash = hash_bytes(wire);
    u32 mask = static_cast<u32>(table_.size()) - 1;
    for (u32 i = hash & mask; table_[i]; i = (i + 1) & mask) {
        Patch& p = *slots_[table_[i]];
        if (p.hash == hash && p.wire_len == wire.size() && std::memcmp(p.wire(), wire.data(), wire.size()) == 0) {
            ++p.refs;
            return table_[i];
        }
    }
    // A patch nobody holds yet: the one place that allocates, once per distinct patch.
    if ((live_ + 1) * 2 > table_.size()) {
        rehash(static_cast<u32>(table_.size()) * 2);
        mask = static_cast<u32>(table_.size()) - 1;
    }
    Patch* p = static_cast<Patch*>(std::malloc(sizeof(Patch) + index.size_bytes() + wire.size()));
    if (UNLIKELY(!p)) std::abort();
    *p = {1, hash, static_cast<u32>(wire.size()), static_cast<u32>(index.size())};
    std::copy(index.begin(), index.end(), p->index());
    std::memcpy(p->wire(), wire.data(), wire.size());
    PatchId id;
    if (!free_.empty()) {
        id = free_.back();
        free_.pop_back();
        slots_[id] = p;
    } else {
        id = static_cast<PatchId>(slots_.size());
        slots_.push_back(p);
    }
    u32 i = hash & mask;
    while (table_[i]) i = (i + 1) & mask;
    table_[i] = id;
    ++live_;
    return id;
}

void Patches::drop(PatchId id) {
    Patch* p = slots_[id];
    const u32 mask = static_cast<u32>(table_.size()) - 1;
    u32 hole = p->hash & mask;
    while (table_[hole] != id) hole = (hole + 1) & mask;
    // Linear probing without tombstones: pull back every later entry of the run whose home
    // position is not after the hole.
    for (u32 j = (hole + 1) & mask; table_[j]; j = (j + 1) & mask) {
        const u32 home = slots_[table_[j]]->hash & mask;
        if (((j - home) & mask) >= ((j - hole) & mask)) {
            table_[hole] = table_[j];
            hole = j;
        }
    }
    table_[hole] = 0;
    std::free(p);
    slots_[id] = nullptr;
    free_.push_back(id);
    --live_;
}

void Patches::rehash(u32 buckets) {
    std::vector<PatchId> fresh(buckets, 0);
    const u32 mask = buckets - 1;
    for (const PatchId id : table_) {
        if (!id) continue;
        u32 i = slots_[id]->hash & mask;
        while (fresh[i]) i = (i + 1) & mask;
        fresh[i] = id;
    }
    table_.swap(fresh);
}

}  // namespace item
