#include "item/components.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>

namespace item {

using proto::Reader;
namespace reg = proto::reg;
namespace sreg = proto::gen::static_registry;

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

// ByteBufCodecs.STRING_UTF8 (Utf8String.read). The limit is in UTF-16 units; up to kMaxString
// bytes cannot exceed it, and nothing longer fits a patch anyway.
Verdict t_string(Reader& in, Out& out) {
    const u32 n = in.varint();
    if (!in.ok || n > kMaxString * 3 || n > in.remaining()) return Verdict::kMalformed;
    if (n > kMaxString) return Verdict::kUnsupported;
    out.varint(n);
    out.bytes(in.bytes(n));
    return Verdict::kOk;
}

// Identifier.STREAM_CODEC: a string through Identifier.parse, written back as "namespace:path".
Verdict t_identifier(Reader& in, Out& out) {
    const std::string_view s = in.str(kMaxString);
    if (!in.ok) return Verdict::kMalformed;
    const auto namespace_char = [](char c) {
        return c == '_' || c == '-' || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.';
    };
    const auto path_char = [&](char c) { return namespace_char(c) || c == '/'; };
    // Identifier.bySeparator: everything before the first ':' is the namespace, unless it is empty.
    const usize colon = s.find(':');
    const std::string_view ns = colon == std::string_view::npos || colon == 0 ? "minecraft" : s.substr(0, colon);
    const std::string_view path = colon == std::string_view::npos ? s : s.substr(colon + 1);
    if (ns == ".." || !std::all_of(ns.begin(), ns.end(), namespace_char) || !std::all_of(path.begin(), path.end(), path_char))
        return Verdict::kMalformed;
    out.varint(static_cast<u32>(ns.size() + 1 + path.size()));
    out.bytes({reinterpret_cast<const u8*>(ns.data()), ns.size()});
    out.u8v(':');
    out.bytes({reinterpret_cast<const u8*>(path.data()), path.size()});
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

// The components this server can vouch for, each by the codecs DataComponents registers for it
// (26.3): `P` the persistent Codec (value ranges), `N` the network StreamCodec (layout).
// Everything else - text (custom_name, lore, books, signs), NBT (custom_data, entity data),
// stacks inside stacks (containers, bundles, charged projectiles), and the behaviour components
// (food, tool, equippable, attribute_modifiers, ...) - is kUnsupported until it is read too.
Verdict transcode(u32 type, Reader& in, Out& out) {
    switch (type) {
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
