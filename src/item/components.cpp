// SPDX-License-Identifier: GPL-3.0-or-later
#include "item/components.hpp"

#include "item/codec.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>

#include "nbt/nbt.hpp"
#include "proto/identifier.hpp"
#include "proto/static_data.hpp"
#include "text/component.hpp"

#include PROTO_GEN_ENTITIES_HPP

namespace item {

using proto::Reader;
namespace reg = proto::reg;
namespace sreg = proto::gen::static_registry;
namespace entity_type = proto::gen::entity_type;
namespace variant = proto::gen::variant;

namespace {

constexpr u32 kMaxString = 32767;  // ByteBufCodecs.STRING_UTF8: at most this many UTF-16 units, in at most 3x the bytes
// MobEffectInstance.Details nests through its "hidden effect"; vanilla's decoder recurses for as
// long as the stack lasts. Beacons and potions nest one level.
constexpr u32 kMaxEffectDepth = 8;
constexpr u32 kMaxBlockStateProperties = 16;  // the block with the most properties has 9
// A stack in a stack in a stack: a shulker box in a bundle is two. Vanilla recurses for as long
// as the packet lasts.
constexpr u32 kMaxPatchDepth = 8;

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

Verdict transcode(u32 type, Reader& in, Out& out);

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

// ...over ByIdMap.continuous(..., CLAMP): an id outside is the nearest end.
Verdict t_enum_clamp(Reader& in, Out& out, i32 values) {
    out.varint(static_cast<u32>(std::clamp(vint(in), 0, values - 1)));
    return Verdict::kOk;
}
// ...over ByIdMap.continuous(..., WRAP): Mth.positiveModulo.
Verdict t_enum_wrap(Reader& in, Out& out, i32 values) {
    const i32 m = vint(in) % values;
    out.varint(static_cast<u32>(m < 0 ? m + values : m));
    return Verdict::kOk;
}
// ...over ByIdMap.sparse(..., default): the ids are the enum's own, and any other is `fallback`.
Verdict t_enum_sparse(Reader& in, Out& out, std::span<const variant::Named> values, u32 fallback) {
    const u32 id = in.varint();
    const bool known = std::any_of(values.begin(), values.end(), [&](const variant::Named& v) { return v.id == id; });
    out.varint(known ? id : fallback);
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
// The hidden effect is read and not kept: a vanilla 26.3 server that is sent one in an item's
// potion contents - by a client or in a command's tag - holds the effect without it from then
// on (tools/vanilla/component_probe.py, command_probe.py; observed, the code that drops it was
// not found in the jar).
Verdict t_effect_details(Reader& in, Out& out) {
    for (u32 depth = 0;; ++depth) {
        const u32 amplifier = static_cast<u32>(std::clamp(vint(in), 0, 255)), duration = in.varint();
        const bool ambient = in.u8v() != 0, particles = in.u8v() != 0, icon = in.u8v() != 0, hidden = in.u8v() != 0;
        if (!in.ok) return Verdict::kMalformed;
        if (depth == 0) {
            out.varint(amplifier);
            out.varint(duration);
            out.u8v(ambient);
            out.u8v(particles);
            out.u8v(icon);
            out.u8v(0);
        }
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

// TooltipDisplay.STREAM_CODEC: hide_tooltip, then a set of component types. The set keeps the
// order they came in (a ReferenceLinkedOpenHashSet), each once.
Verdict t_tooltip_display(Reader& in, Out& out) {
    out.u8v(in.u8v() != 0);
    u32 n;
    if (!read_count(in, INT32_MAX, n)) return Verdict::kMalformed;
    bool hidden[component::kCount] = {};
    u8 order[component::kCount];
    u32 distinct = 0;
    for (u32 i = 0; i < n; ++i) {
        const u32 type = in.varint();
        if (!in.ok || type >= component::kCount) return Verdict::kMalformed;
        if (!hidden[type]) order[distinct++] = static_cast<u8>(type);
        hidden[type] = true;
    }
    out.varint(distinct);
    for (u32 i = 0; i < distinct; ++i) out.varint(order[i]);
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

// CustomModelData.STREAM_CODEC: four lists - floats, flags, strings, colours (ints).
Verdict t_custom_model_data(Reader& in, Out& out) {
    u32 n;
    if (!read_count(in, INT32_MAX, n) || 4ull * n > in.remaining()) return Verdict::kMalformed;
    out.varint(n);
    out.bytes(in.bytes(4 * n));
    if (!read_count(in, INT32_MAX, n)) return Verdict::kMalformed;
    out.varint(n);
    for (u32 i = 0; i < n; ++i) out.u8v(in.u8v() != 0);
    if (!read_count(in, INT32_MAX, n)) return Verdict::kMalformed;
    out.varint(n);
    Verdict verdict = Verdict::kOk;
    for (u32 i = 0; i < n; ++i) {
        const Verdict v = t_string(in, out);
        if (v == Verdict::kMalformed) return v;
        if (v != Verdict::kOk) verdict = v;
    }
    if (!read_count(in, INT32_MAX, n) || 4ull * n > in.remaining()) return Verdict::kMalformed;
    out.varint(n);
    out.bytes(in.bytes(4 * n));
    return verdict;
}

}  // namespace

bool registry_has_tag(std::string_view registry, std::string_view tag) {
    const std::span<const u8> body = proto::tags_body();
    Reader r(body.data(), body.size());
    r.varint();  // the packet id
    for (u32 registries = r.varint(); registries-- && r.ok;) {
        const bool here = r.str(kMaxString) == registry;
        for (u32 tags = r.varint(); tags-- && r.ok;) {
            if (r.str(kMaxString) == tag && here) return true;
            for (u32 ids = r.varint(); ids-- && r.ok;) r.varint();
        }
    }
    return false;
}

namespace {

// ByteBufCodecs.holderSet: 0 and the name of a tag (which the registry must have), or the number
// of entries + 1 and their ids.
Verdict t_holder_set(Reader& in, Out& out, std::string_view registry, u32 size) {
    const i32 count = vint(in) - 1;
    if (!in.ok) return Verdict::kMalformed;
    if (count == -1) {
        const std::string_view s = in.str(kMaxString);
        proto::Identifier id;
        if (!in.ok || !proto::parse_identifier(s, id)) return Verdict::kMalformed;
        std::string full(id.size(), '\0');
        id.put(reinterpret_cast<u8*>(full.data()));
        if (!registry_has_tag(registry, full)) return Verdict::kMalformed;
        out.varint(0);
        out.varint(static_cast<u32>(full.size()));
        out.bytes({reinterpret_cast<const u8*>(full.data()), full.size()});
        return Verdict::kOk;
    }
    if (count < 0 || static_cast<u32>(count) > in.remaining()) return Verdict::kMalformed;
    out.varint(static_cast<u32>(count) + 1);
    for (i32 i = 0; i < count; ++i) {
        if (const Verdict v = t_registry_id(in, out, size); v != Verdict::kOk) return v;
    }
    return Verdict::kOk;
}

// LodestoneTracker.STREAM_CODEC: an optional GlobalPos - the dimension's key, which is any
// identifier, and a block position as a long - and whether the compass is tracked.
Verdict t_lodestone_tracker(Reader& in, Out& out) {
    const bool target = in.u8v() != 0;
    out.u8v(target);
    if (target) {
        if (const Verdict v = t_identifier(in, out); v != Verdict::kOk) return v;
        out.bytes(in.bytes(8));
    }
    return t_bool(in, out);
}

// ByteBufCodecs.holder(SOUND_EVENT, SoundEvent.DIRECT_STREAM_CODEC): id + 1, or 0 and the event
// itself - the identifier of the sound and an optional range.
Verdict t_sound_event(Reader& in, Out& out) {
    const u32 id = in.varint();
    if (!in.ok) return Verdict::kMalformed;
    out.varint(id);
    if (id != 0) return id - 1 < sreg::kSoundEventCount ? Verdict::kOk : Verdict::kMalformed;
    if (const Verdict v = t_identifier(in, out); v != Verdict::kOk) return v;
    const bool range = in.u8v() != 0;
    out.u8v(range);
    if (range) out.be32(in.be32());
    return Verdict::kOk;
}

// ByteBufCodecs.GAME_PROFILE_PROPERTIES: at most 16 properties - a name (64), a value (32767),
// perhaps a signature (1024). Vanilla holds them in a multimap and writes that: the properties
// of one name together, the names in the order they first came.
Verdict t_properties(Reader& in, Out& out) {
    u32 n;
    if (!read_count(in, 16, n)) return Verdict::kMalformed;
    static base::ByteBuf scratch;
    scratch.clear();
    Out to{scratch};
    struct Property {
        u32 at, len, name_len;  // in `scratch`: the whole of it, and how long its name is behind the length
    } props[16];
    Verdict verdict = Verdict::kOk;
    for (u32 i = 0; i < n; ++i) {
        const u32 at = scratch.len;
        Verdict v = t_string(in, to, 64);
        const u32 name_end = scratch.len;
        if (v != Verdict::kMalformed) v = std::max(v, t_string(in, to, 32767));
        if (v == Verdict::kMalformed) return v;
        const bool sig = in.u8v() != 0;
        if (!in.ok) return Verdict::kMalformed;
        to.u8v(sig);
        if (sig) v = std::max(v, t_string(in, to, 1024));
        if (v == Verdict::kMalformed) return v;
        verdict = std::max(verdict, v);
        props[i] = {at, scratch.len - at, name_end - at};
    }
    if (verdict != Verdict::kOk) return verdict;
    const auto name = [&](const Property& p) {
        return std::string_view(reinterpret_cast<const char*>(scratch.data) + p.at, p.name_len);
    };
    out.varint(n);
    bool written[16] = {};
    for (u32 i = 0; i < n; ++i) {
        for (u32 k = i; k < n; ++k) {
            if (written[k] || name(props[k]) != name(props[i])) continue;
            written[k] = true;
            out.bytes({scratch.data + props[k].at, props[k].len});
        }
    }
    return Verdict::kOk;
}

// ResolvableProfile.STREAM_CODEC: a whole game profile (id, name, properties) or a partial one
// (perhaps a name, perhaps an id, properties) - a flag says which - and a PlayerSkin.Patch:
// three optional textures and an optional model (slim or not).
Verdict t_profile(Reader& in, Out& out) {
    const bool whole = in.u8v() != 0;
    if (!in.ok) return Verdict::kMalformed;
    out.u8v(whole);
    Verdict verdict = Verdict::kOk;
    const auto name = [&] {
        const Verdict v = t_string(in, out, 16);  // ByteBufCodecs.PLAYER_NAME
        verdict = std::max(verdict, v);
        return v != Verdict::kMalformed;
    };
    if (whole) {
        out.bytes(in.bytes(16));  // UUIDUtil.STREAM_CODEC: two longs
        if (!in.ok || !name()) return Verdict::kMalformed;
    } else {
        const bool named = in.u8v() != 0;
        out.u8v(named);
        if (!in.ok || (named && !name())) return Verdict::kMalformed;
        const bool id = in.u8v() != 0;
        out.u8v(id);
        if (id) out.bytes(in.bytes(16));
        if (!in.ok) return Verdict::kMalformed;
    }
    const Verdict properties = t_properties(in, out);
    if (properties == Verdict::kMalformed) return properties;
    verdict = std::max(verdict, properties);
    for (u32 texture = 0; texture < 3; ++texture) {  // body, cape, elytra: ClientAsset.ResourceTexture, an identifier
        const bool present = in.u8v() != 0;
        if (!in.ok) return Verdict::kMalformed;
        out.u8v(present);
        if (!present) continue;
        if (const Verdict v = t_identifier(in, out); v != Verdict::kOk) return v;
    }
    const bool model = in.u8v() != 0;
    out.u8v(model);
    if (model) out.u8v(in.u8v() != 0);
    return in.ok ? verdict : Verdict::kMalformed;
}

// ---------------------------------------------------------------------------- stacks in stacks

// DataComponentPatch.STREAM_CODEC: a patch whose values follow their types without a length -
// the patch of a stack inside a component. Written as Patches::assemble writes one.
Verdict t_patch(Reader& in, Out& out) {
    static base::ByteBuf scratch[kMaxPatchDepth];
    static u32 depth = 0;
    const i32 added = vint(in), removed = vint(in);
    if (!in.ok || added < 0 || removed < 0 || static_cast<u64>(added) + static_cast<u64>(removed) > in.remaining())
        return Verdict::kMalformed;
    if (added == 0 && removed == 0) {
        out.varint(0);
        out.varint(0);
        return Verdict::kOk;
    }
    if (depth == kMaxPatchDepth) return Verdict::kUnsupported;
    Patches::Slot slot[component::kCount] = {};
    base::ByteBuf& values = scratch[depth];
    values.clear();
    Out to{values};
    for (i32 i = 0; i < added; ++i) {
        const u32 type = in.varint();
        if (!in.ok || type >= component::kCount) return Verdict::kMalformed;
        const u32 at = values.len;
        ++depth;
        const Verdict v = transcode(type, in, to);
        --depth;
        if (v == Verdict::kMalformed || !in.ok) return Verdict::kMalformed;
        // (What is behind a value that was not read cannot be found: there is no length.)
        if (v == Verdict::kUnsupported) return v;
        slot[type] = {1, at, values.len - at};
    }
    for (i32 i = 0; i < removed; ++i) {
        const u32 type = in.varint();
        if (!in.ok || type >= component::kCount) return Verdict::kMalformed;
        slot[type] = {2, 0, 0};
    }
    u32 set = 0, gone = 0;
    for (u32 type = 0; type < component::kCount; ++type) {
        set += slot[type].state == 1;
        gone += slot[type].state == 2;
    }
    out.varint(set);
    out.varint(gone);
    for (u32 type = 0; type < component::kCount; ++type) {
        if (slot[type].state != 1) continue;
        out.varint(type);
        out.bytes({values.data + slot[type].off, slot[type].len});
    }
    for (u32 type = 0; type < component::kCount; ++type)
        if (slot[type].state == 2) out.varint(type);
    return Verdict::kOk;
}

// ItemStackTemplate.STREAM_CODEC: the item, the count, the patch. The constructor refuses air
// and a count of 0 - and what a client sends is then written with ItemStack.CODEC to see
// whether it can be (ItemStack.validatedStreamCodec), where the count of a template is
// intRange(1, 99) however deep it lies: a vanilla server puts the sender out for a hundred.
Verdict t_template(Reader& in, Out& out) {
    const u32 item = in.varint();
    const i32 count = vint(in);
    if (!in.ok || item >= proto::gen::item::kCount || item == proto::gen::item::kAir || count < 1 || count > 99)
        return Verdict::kMalformed;
    out.varint(item);
    out.varint(static_cast<u32>(count));
    return t_patch(in, out);
}

// A list of templates (BundleContents, ChargedProjectiles: ByteBufCodecs.list(max)), or of
// optional ones (ItemContainerContents: a flag before each).
Verdict t_templates(Reader& in, Out& out, u32 max, bool optional) {
    u32 n;
    if (!read_count(in, max, n)) return Verdict::kMalformed;
    out.varint(n);
    for (u32 i = 0; i < n; ++i) {
        if (optional) {
            const bool present = in.u8v() != 0;
            if (!in.ok) return Verdict::kMalformed;
            out.u8v(present);
            if (!present) continue;
        }
        if (const Verdict v = t_template(in, out); v != Verdict::kOk) return v;
    }
    return Verdict::kOk;
}

// PotDecorations.STREAM_CODEC: back, left, right, front - each an optional template.
Verdict t_pot_decorations(Reader& in, Out& out) {
    for (u32 side = 0; side < 4; ++side) {
        const bool present = in.u8v() != 0;
        if (!in.ok) return Verdict::kMalformed;
        out.u8v(present);
        if (!present) continue;
        if (const Verdict v = t_template(in, out); v != Verdict::kOk) return v;
    }
    return Verdict::kOk;
}

// ---------------------------------------------------------------------------- NBT and text
//
// What travels as a tag: custom data and the data of entities and block entities, which stay
// tags; text components, which a codec reads out of one (text/component.hpp).

// The tag being read. One at a time: a decoder is done with it before the next tag is parsed.
// One a level: a text component may show an item (a hover event) whose components are values
// again, read while the tag around them is still in use.
constexpr u32 kMaxTagLevels = 8;
struct TagLevel {
    static inline nbt::Doc docs[kMaxTagLevels];
    static inline u32 depth = 0;
    const bool ok = depth < kMaxTagLevels;
    nbt::Doc& doc = docs[ok ? depth : 0];
    TagLevel() { ++depth; }
    ~TagLevel() { --depth; }
    TagLevel(const TagLevel&) = delete;
};

// A value this much longer than a whole patch may be is not looked at: it could only shrink to
// fit by being mostly things vanilla drops, and it is the tick thread that would do the reading.
constexpr u32 kMaxTagInput = 2 * kMaxPatchBytes;

// ByteBufCodecs.TAG: one tag in the network form, under NbtAccounter.defaultQuota().
Verdict read_tag(Reader& in, TagLevel& level) {
    nbt::Doc& the_tag = level.doc;
    if (!level.ok || in.remaining() > kMaxTagInput) return Verdict::kUnsupported;
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
    TagLevel level;
    if (const Verdict v = read_tag(in, level); v != Verdict::kOk) return v;
    nbt::Doc& the_tag = level.doc;
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
// Bees.STREAM_CODEC: a list of occupants - the entity as typed data, and two tick counts.
Verdict t_bees(Reader& in, Out& out) {
    u32 n;
    if (!read_count(in, INT32_MAX, n)) return Verdict::kMalformed;
    out.varint(n);
    for (u32 i = 0; i < n; ++i) {
        if (const Verdict v = t_typed_data(in, out, entity_type::kCount, entity_type::kPig); v != Verdict::kOk) return v;
        out.varint(in.varint());
        out.varint(in.varint());
    }
    return Verdict::kOk;
}

// A component without a StreamCodec of its own travels as the tag its Codec writes
// (ByteBufCodecs.fromCodecWithRegistries): read by that Codec, kept as that Codec writes it
// again (item/codec.cpp).
Verdict t_codec_tag(u32 type, Reader& in, Out& out) {
    TagLevel level;
    if (const Verdict v = read_tag(in, level); v != Verdict::kOk) return v;
    return tag_component(type, level.doc.root(), out.b);
}

Verdict t_text(Reader& in, Out& out) {
    TagLevel level;
    if (const Verdict v = read_tag(in, level); v != Verdict::kOk) return v;
    switch (text::transcode(level.doc.root(), out.b)) {
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
        // ---- stacks in stacks
        case component::kContainer: return t_templates(in, out, 256, true);  // N list(256) of optional templates
        case component::kBundleContents: return t_templates(in, out, INT32_MAX, false);
        case component::kChargedProjectiles: return t_templates(in, out, 1024, false);  // N list(1024)
        case component::kSulfurCubeContent: return t_template(in, out);
        case component::kPotDecorations: return t_pot_decorations(in, out);
        case component::kBees: return t_bees(in, out);
        // ---- what a mob is: ids of registries (ByteBufCodecs.holderRegistry) and of enums
        case component::kVillagerVariant: return t_registry_id(in, out, sreg::kVillagerTypeCount);
        case component::kWolfVariant: return t_registry_id(in, out, reg::wolf_variant::kCount);
        case component::kWolfSoundVariant: return t_registry_id(in, out, reg::wolf_sound_variant::kCount);
        case component::kPigVariant: return t_registry_id(in, out, reg::pig_variant::kCount);
        case component::kPigSoundVariant: return t_registry_id(in, out, reg::pig_sound_variant::kCount);
        case component::kCowVariant: return t_registry_id(in, out, reg::cow_variant::kCount);
        case component::kCowSoundVariant: return t_registry_id(in, out, reg::cow_sound_variant::kCount);
        case component::kChickenVariant: return t_registry_id(in, out, reg::chicken_variant::kCount);
        case component::kChickenSoundVariant: return t_registry_id(in, out, reg::chicken_sound_variant::kCount);
        case component::kZombieNautilusVariant: return t_registry_id(in, out, reg::zombie_nautilus_variant::kCount);
        case component::kFrogVariant: return t_registry_id(in, out, reg::frog_variant::kCount);
        case component::kCatVariant: return t_registry_id(in, out, reg::cat_variant::kCount);
        case component::kCatSoundVariant: return t_registry_id(in, out, reg::cat_sound_variant::kCount);
        case component::kProvidesPotteryPattern: return t_registry_id(in, out, reg::decorated_pot_pattern::kCount);
        case component::kFoxVariant: return t_enum(in, out, std::size(variant::kFox));              // ZERO
        case component::kAxolotlVariant: return t_enum(in, out, std::size(variant::kAxolotl));      // ZERO
        case component::kSalmonSize: return t_enum_clamp(in, out, std::size(variant::kSalmon));     // CLAMP
        case component::kParrotVariant: return t_enum_clamp(in, out, std::size(variant::kParrot));  // CLAMP
        case component::kMooshroomVariant: return t_enum_clamp(in, out, std::size(variant::kMooshroom));
        case component::kLlamaVariant: return t_enum_clamp(in, out, std::size(variant::kLlama));
        case component::kHorseVariant: return t_enum_wrap(in, out, std::size(variant::kHorse));  // WRAP
        case component::kRabbitVariant: return t_enum_sparse(in, out, variant::kRabbit, 0);      // sparse, default BROWN
        case component::kTropicalFishPattern:
            return t_enum_sparse(in, out, variant::kTropicalFishPattern, 0);  // default KOB
        // ---- the rest of what is only data
        case component::kCustomModelData: return t_custom_model_data(in, out);
        case component::kCreativeSlotLock: return Verdict::kOk;          // N Unit, and no P
        case component::kAdditionalTradeCost: return t_varint(in, out);  // N VAR_INT, and no P
        case component::kMapPostProcessing: return t_enum(in, out, 2);   // N idMapper, ZERO; no P
        case component::kTrim: {  // N ArmorTrim: material, pattern - holders, an inline one is not read
            const Verdict v = t_holder_reference(in, out, reg::trim_material::kCount);
            return v == Verdict::kOk ? t_holder_reference(in, out, reg::trim_pattern::kCount) : v;
        }
        case component::kProvidesTrimMaterial: return t_holder_reference(in, out, reg::trim_material::kCount);
        case component::kProvidesBannerPatterns:
            return t_holder_set(in, out, reg::banner_pattern::kKey, reg::banner_pattern::kCount);
        case component::kLodestoneTracker: return t_lodestone_tracker(in, out);
        case component::kBreakSound: return t_sound_event(in, out);
        case component::kProfile: return t_profile(in, out);
        case component::kLock:                  // P LockCode (an ItemPredicate), and no N
        case component::kIntangibleProjectile:  // P Unit, and no N: as the tag {}
        case component::kMapDecorations:
        case component::kDebugStickState:
        case component::kRecipes:
        case component::kContainerLoot: return t_codec_tag(type, in, out);  // P only
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

Patches::Patches() : slots_(1, nullptr), table_(64, 0) {
    // The store is there before any stack is: with it, the text codec learns to read the
    // components of a stack that a hover event shows.
    text::set_item_components(&hover_components);
}

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
    Slot slot[component::kCount] = {};
    Verdict verdict = Verdict::kOk;
    values_.clear();
    Out values{values_};
    for (i32 i = 0; i < added; ++i) {
        const u32 type = r.varint(), len = r.varint();
        if (!r.ok || type >= component::kCount || len > r.remaining()) return Verdict::kMalformed;
        Reader in(r.bytes(len));
        if (values_.len > 2 * kMaxPatchBytes) {  // already far too big to keep: stop copying, keep walking
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
    return assemble(slot, {values_.data, values_.len}, out);
}

Verdict Patches::assemble(const Slot* slot, std::span<const u8> values, PatchId& out) {
    out = 0;
    // DataComponentPatch.STREAM_CODEC: both counts, the added entries, the removed types. The
    // order within each is free on the wire; by type here, so that equal patches are equal bytes.
    Entry index[component::kCount];
    u32 n = 0, set = 0, removed = 0;
    for (u32 type = 0; type < component::kCount; ++type) {
        set += slot[type].state == 1;
        removed += slot[type].state == 2;
    }
    if (set + removed == 0) return Verdict::kOk;
    wire_.clear();
    Out wire{wire_};
    wire.varint(set);
    wire.varint(removed);
    for (u32 type = 0; type < component::kCount; ++type) {
        if (slot[type].state != 1) continue;
        wire.varint(type);
        index[n++] = {static_cast<u16>(type), false, wire_.len, slot[type].len};
        wire.bytes(values.subspan(slot[type].off, slot[type].len));
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

void contained(u32 type, std::span<const u8> value, std::vector<Contained>& out) {
    DCHECK(type == component::kContainer || type == component::kBundleContents || type == component::kChargedProjectiles);
    base::ByteBuf skipped;  // a patch has no length: it is as long as reading it takes
    Out to{skipped};
    Reader in(value);
    const u32 n = in.varint();
    for (u32 i = 0; i < n && in.ok; ++i) {
        if (type == component::kContainer && in.u8v() == 0) continue;
        const u32 item = in.varint(), count = in.varint();
        const u8* const patch = in.p;
        skipped.clear();
        if (t_patch(in, to) != Verdict::kOk) return;
        out.push_back({i, item, count, {patch, static_cast<usize>(in.p - patch)}});
    }
}

Patches::Found find_nested(std::span<const u8> patch, u32 type) {
    base::ByteBuf skipped;
    Out to{skipped};
    Reader in(patch);
    const u32 added = in.varint(), removed = in.varint();
    Patches::Found found;
    for (u32 i = 0; i < added && in.ok; ++i) {
        const u32 t = in.varint();
        const u8* const value = in.p;
        skipped.clear();
        if (transcode(t, in, to) != Verdict::kOk) return {};
        if (t == type) found = {Patches::State::kSet, {value, static_cast<usize>(in.p - value)}};
    }
    for (u32 i = 0; i < removed && in.ok; ++i)
        if (in.varint() == type) found = {Patches::State::kRemoved, {}};
    return found;
}

Verdict nested_patch(std::span<const u8> patch, PatchId& out) {
    out = 0;
    base::ByteBuf skipped;
    Out to{skipped};
    Reader in(patch);
    const u32 added = in.varint(), removed = in.varint();
    PatchBuilder builder;
    for (u32 i = 0; i < added && in.ok; ++i) {
        const u32 type = in.varint();
        const u8* const value = in.p;
        skipped.clear();
        if (type >= component::kCount || transcode(type, in, to) != Verdict::kOk) return Verdict::kMalformed;
        builder.set(type, {value, static_cast<usize>(in.p - value)});
    }
    for (u32 i = 0; i < removed && in.ok; ++i) {
        const u32 type = in.varint();
        if (type >= component::kCount) return Verdict::kMalformed;
        builder.remove(type);
    }
    if (!in.ok) return Verdict::kMalformed;
    return builder.empty() ? Verdict::kOk : builder.build(out);
}

Verdict check_value(u32 type, std::span<const u8> value, base::ByteBuf& out) {
    if (type >= component::kCount) return Verdict::kMalformed;
    Reader in(value);
    Out to{out};
    const Verdict v = transcode(type, in, to);
    return v == Verdict::kOk && !in.ok ? Verdict::kMalformed : v;
}

PatchBuilder::PatchBuilder(PatchId base) {
    if (!base) return;
    for (const Patches::Entry& e : patches().entries(base)) {
        if (e.removed) {
            remove(e.type);
        } else {
            set(e.type, patches().wire(base).subspan(e.off, e.len));
        }
    }
}
void PatchBuilder::set(u32 type, std::span<const u8> value) {
    slots_[type] = {1, values_.len, static_cast<u32>(value.size())};
    values_.append(value.data(), value.size());
}
void PatchBuilder::remove(u32 type) { slots_[type] = {2, 0, 0}; }
void PatchBuilder::erase(u32 type) { slots_[type] = {}; }
bool PatchBuilder::empty() const {
    return std::all_of(std::begin(slots_), std::end(slots_), [](const Patches::Slot& s) { return s.state == 0; });
}
Verdict PatchBuilder::build(PatchId& out) { return patches().assemble(slots_, {values_.data, values_.len}, out); }

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
