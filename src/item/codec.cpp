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
namespace variant = proto::gen::variant;

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

// ---------------------------------------------------------------------------- one entry of a registry, one constant of an enum

// The components that are a Holder by name (RegistryCodecs.holder(registry): the entry's name
// in a tag, its id in a packet).
Names registry_names(u32 type) {
    switch (type) {
        case component::kVillagerVariant: return sreg::kVillagerTypeNames;
        case component::kWolfVariant: return reg::wolf_variant::kNames;
        case component::kWolfSoundVariant: return reg::wolf_sound_variant::kNames;
        case component::kPigVariant: return reg::pig_variant::kNames;
        case component::kPigSoundVariant: return reg::pig_sound_variant::kNames;
        case component::kCowVariant: return reg::cow_variant::kNames;
        case component::kCowSoundVariant: return reg::cow_sound_variant::kNames;
        case component::kChickenVariant: return reg::chicken_variant::kNames;
        case component::kChickenSoundVariant: return reg::chicken_sound_variant::kNames;
        case component::kZombieNautilusVariant: return reg::zombie_nautilus_variant::kNames;
        case component::kFrogVariant: return reg::frog_variant::kNames;
        case component::kCatVariant: return reg::cat_variant::kNames;
        case component::kCatSoundVariant: return reg::cat_sound_variant::kNames;
        case component::kProvidesPotteryPattern: return reg::decorated_pot_pattern::kNames;
        default: return {};
    }
}
// ...and those that are an enum (StringRepresentable.fromEnum: its name in a tag, its own id in a packet).
std::span<const variant::Named> enum_values(u32 type) {
    switch (type) {
        case component::kFoxVariant: return variant::kFox;
        case component::kSalmonSize: return variant::kSalmon;
        case component::kParrotVariant: return variant::kParrot;
        case component::kTropicalFishPattern: return variant::kTropicalFishPattern;
        case component::kMooshroomVariant: return variant::kMooshroom;
        case component::kRabbitVariant: return variant::kRabbit;
        case component::kHorseVariant: return variant::kHorse;
        case component::kLlamaVariant: return variant::kLlama;
        case component::kAxolotlVariant: return variant::kAxolotl;
        default: return {};
    }
}

Verdict from(u32 type, Tag t, Out& out);
void to(u32 type, Reader& in, nbt::Writer& w, std::string_view name);

// ---------------------------------------------------------------------------- stacks in stacks

// DataComponentPatch.CODEC into the form a patch has inside a component: counts, the values
// behind their types without a length, the types that are taken away.
Verdict patch_wire_from(Tag components, Out& out) {
    if (components.type != nbt::kCompound) return kBad;
    Patches::Slot slot[component::kCount] = {};
    base::ByteBuf values;
    for (const Tag e : components.entries()) {
        if (components.get(e.name()).at != e.at) continue;  // a name twice: the last one counts
        const std::string key = nbt::to_utf8(e.name());
        const bool removed = key.starts_with('!');
        const u32 type = find_component(std::string_view(key).substr(removed));
        if (type == kNoComponent || transient(type)) return kBad;
        if (removed) {
            slot[type] = {2, 0, 0};
            continue;
        }
        const u32 at = values.len;
        if (const Verdict v = value_from_nbt(type, e, values); v != kOk) return v;
        slot[type] = {1, at, values.len - at};
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
    return kOk;
}

// ...and back: the entries of such a patch into the compound that is open.
void patch_wire_to(Reader& in, nbt::Writer& w) {
    const u32 added = in.varint(), removed = in.varint();
    for (u32 i = 0; i < added; ++i) {
        const u32 type = in.varint();
        if (type == component::kAdditionalTradeCost || type == component::kMapPostProcessing) {
            in.varint();  // no Codec: no part of a tag
        } else if (type != component::kCreativeSlotLock) {
            to(type, in, w, component::kNames[type]);
        }
    }
    for (u32 i = 0; i < removed; ++i) {
        std::string key = "!";
        key += component::kNames[in.varint()];
        w.begin_compound(key);
        w.end_compound();
    }
}

// ItemStackTemplate.CODEC: {id, count, components} - an item that is not air, a count of 1..99
// that is 1 if left out - or, in its place, the name of the item alone.
Verdict template_from(Tag t, Out& out) {
    std::string name;
    if (t.type == nbt::kString) {
        if (!as_identifier(t, name)) return kBad;
        const u32 item = world::find_item(name);
        if (item == world::kNoId || item == 0) return kBad;
        out.varint(item);
        out.varint(1);
        out.varint(0);
        out.varint(0);
        return kOk;
    }
    if (t.type != nbt::kCompound || !as_identifier(t.get("id"), name)) return kBad;
    const u32 item = world::find_item(name);
    i32 count;
    if (item == world::kNoId || item == 0 || !optional(t, "count", count, 1, as_int) || count < 1 || count > 99) return kBad;
    out.varint(item);
    out.varint(static_cast<u32>(count));
    if (const Tag components = t.get("components")) return patch_wire_from(components, out);
    out.varint(0);
    out.varint(0);
    return kOk;
}
// The entries of a template into the compound that is open: what is as a field's default is left out.
void template_to(Reader& in, nbt::Writer& w) {
    w.put_string("id", world::item_name(in.varint()));
    if (const i32 count = static_cast<i32>(in.varint()); count != 1) w.put_int("count", count);
    Reader ahead = in;
    if (ahead.varint() + ahead.varint() == 0) {
        in = ahead;
        return;
    }
    w.begin_compound("components");
    patch_wire_to(in, w);
    w.end_compound();
}

// A list of compounds whose number is known only once they are written.
struct ListOf {
    base::ByteBuf elements;
    nbt::Writer w{elements};
    u32 count = 0;
    void put(nbt::Writer& to, std::string_view name) {
        to.begin_list(name, count ? nbt::kCompound : nbt::kEnd, count);
        to.put_raw({elements.data, elements.len});
    }
};

// TypedEntityData.codec: a compound that names its type as "id".
Verdict typed_data_from(Tag t, Out& out, bool entity) {
    if (t.type != nbt::kCompound) return kBad;
    std::string name;
    if (!as_identifier(t.get("id"), name)) return kBad;
    const u32 id = entity ? world::find_entity_type(name) : world::find_block_entity_type(name);
    if (id == world::kNoId) return kBad;
    out.varint(id);
    out.u8v(nbt::kCompound);
    nbt::Writer w(out.b);
    for (const Tag e : t.entries())
        if (e.name() != "id") w.put_raw(e.name(), e.type, e.bytes());
    w.end_compound();
    return kOk;
}
// ...the entries of the tag, and its type put among them as "id", into the compound that is open.
void typed_data_to(Reader& in, nbt::Writer& w, bool entity) {
    const u32 id = in.varint();
    const Stored t = take_tag(in);
    w.put_raw(t.payload.first(t.payload.size() - 1));  // its entries, without the end
    w.put_string("id", entity ? world::entity_type_name(id) : world::block_entity_type_name(id));
}

// ---------------------------------------------------------------------------- a player's profile

// Codec.sizeLimitedString: a string of at most `max` UTF-16 units.
bool as_limited(Tag t, u32 max, std::string& v) {
    return t.type == nbt::kString && nbt::utf16_length(t.str()) <= max && as_string(t, v);
}
// ExtraCodecs.PLAYER_NAME: at most 16 characters, none of them a space, a control or beyond '~'.
bool as_player_name(Tag t, std::string& v) {
    return as_limited(t, 16, v) && std::all_of(v.begin(), v.end(), [](char c) { return c > 32 && c < 127; });
}
// UUIDUtil.CODEC: four ints, as an array or a list; into the two longs of the wire.
bool uuid_from(Tag t, Out& out) {
    if (!t.collection() || t.size() != 4) return false;
    for (const Tag e : t.elements()) {
        if (!e.numeric()) return false;
        out.be32(static_cast<u32>(e.as_int()));
    }
    return true;
}
// ExtraCodecs.PROPERTY_MAP: a map of names to lists of values, or a list of {name, value, signature}.
Verdict properties_from(Tag t, Out& out) {
    if (!t) {
        out.varint(0);
        return kOk;
    }
    base::ByteBuf list;
    Out to{list};
    u32 n = 0;
    if (t.type == nbt::kCompound) {  // (the order of a map that was read from a tag: that of its HashMap)
        if (t.size() > 16) return kBad;
        std::vector<Tag> entries;
        nbt::map_order(t, entries);
        for (const Tag e : entries) {
            if (!e.collection()) return kBad;
            const std::string name = nbt::to_utf8(e.name());
            for (const Tag value : e.elements()) {
                std::string v;
                if (!as_string(value, v)) return kBad;
                to.str(name);
                to.str(v);
                to.u8v(0);
                ++n;
            }
        }
    } else {
        if (!t.collection() || t.size() > 16) return kBad;
        for (const Tag e : t.elements()) {
            std::string name, value, signature;
            const Tag sig = e.get("signature");
            if (e.type != nbt::kCompound || !as_limited(e.get("name"), 64, name) || !as_limited(e.get("value"), 32767, value) ||
                (sig && !as_limited(sig, 1024, signature)))
                return kBad;
            to.str(name);
            to.str(value);
            to.u8v(static_cast<bool>(sig));
            if (sig) to.str(signature);
            ++n;
        }
    }
    if (n > 16) return kNotRead;  // more than a packet may carry (writeCount): vanilla takes it and cannot send it
    out.varint(n);
    out.bytes({list.data, list.len});
    return kOk;
}

// ResolvableProfile.CODEC: {id, name, properties} (a whole profile, if it has an id and a
// name) or any part of that, with the textures and the model of a skin beside it - or, in
// its place, a player's name alone.
Verdict profile_from(Tag t, Out& out) {
    std::string name;
    if (t.type == nbt::kString) {
        if (!as_player_name(t, name)) return kBad;
        out.u8v(0);  // a partial profile: a name, no id, no properties
        out.u8v(1);
        out.str(name);
        out.u8v(0);
        out.varint(0);
        for (u32 i = 0; i < 4; ++i) out.u8v(0);
        return kOk;
    }
    if (t.type != nbt::kCompound) return kBad;
    const Tag id = t.get("id"), named = t.get("name");
    if (named && !as_player_name(named, name)) return kBad;
    base::ByteBuf uuid;
    Out to_uuid{uuid};
    if (id && !uuid_from(id, to_uuid)) return kBad;
    const bool whole = id && named;
    out.u8v(whole);
    if (whole) {
        out.bytes({uuid.data, uuid.len});
        out.str(name);
    } else {
        out.u8v(static_cast<bool>(named));
        if (named) out.str(name);
        out.u8v(static_cast<bool>(id));
        if (id) out.bytes({uuid.data, uuid.len});
    }
    if (const Verdict v = properties_from(t.get("properties"), out); v != kOk) return v;
    for (const std::string_view key : {"texture", "cape", "elytra"}) {  // PlayerSkin.Patch
        const Tag texture = t.get(key);
        std::string asset;
        if (texture && !as_identifier(texture, asset)) return kBad;
        out.u8v(static_cast<bool>(texture));
        if (texture) out.str(asset);
    }
    const Tag model = t.get("model");
    u32 kind = 0;
    static constexpr std::string_view kModels[] = {"slim", "wide"};
    if (model && !as_enum(model, kModels, kind)) return kBad;
    out.u8v(static_cast<bool>(model));
    if (model) out.u8v(kind == 0);
    return kOk;
}

void profile_to(Reader& in, nbt::Writer& w) {
    const auto uuid = [&] {
        const i32 parts[4] = {static_cast<i32>(in.be32()), static_cast<i32>(in.be32()), static_cast<i32>(in.be32()),
                              static_cast<i32>(in.be32())};
        w.put_int_array("id", parts);
    };
    if (in.u8v()) {
        uuid();
        w.put_string("name", take_str(in));
    } else {
        if (in.u8v()) w.put_string("name", take_str(in));
        if (in.u8v()) uuid();
    }
    if (const u32 n = in.varint()) {  // lambda$static$53: always as the list
        w.begin_list("properties", nbt::kCompound, n);
        for (u32 i = 0; i < n; ++i) {
            w.put_string("name", take_str(in));
            w.put_string("value", take_str(in));
            if (in.u8v()) w.put_string("signature", take_str(in));
            w.end_compound();
        }
    }
    for (const std::string_view key : {"texture", "cape", "elytra"})
        if (in.u8v()) w.put_string(key, take_str(in));
    if (in.u8v()) w.put_string("model", in.u8v() ? "slim" : "wide");
}

// ---------------------------------------------------------------------------- components that are a tag on the wire too

// What was put together with a Writer, as vanilla writes the tag it made of it: every compound
// in the order of its HashMap.
Verdict finish_tag(const base::ByteBuf& built, base::ByteBuf& out) {
    nbt::Doc doc;
    if (doc.parse({built.data, built.len}, nbt::Form::kNameless, nbt::kTrusted) != nbt::Error::kNone) return kNotRead;
    *out.reserve(1) = doc.root().type;
    ++out.len;
    nbt::write_map_order(doc.root(), out);
    return kOk;
}

// Block.getStateDefinition().getProperty(name) != null
bool has_property(u32 block, std::string_view name) {
    for (u32 i = 0; i < world::block_property_count(block); ++i)
        if (world::block_property(block, i).name == name) return true;
    return false;
}

Verdict tag_component_built(u32 type, Tag t, nbt::Writer& w) {
    switch (type) {
        case component::kIntangibleProjectile:  // Unit.CODEC: any map
            if (t.type != nbt::kCompound) return kBad;
            w.root(nbt::kCompound);
            w.end_compound();
            return kOk;
        case component::kRecipes: {  // Recipe.KEY_CODEC.listOf(): the keys of recipes, which are any identifiers
            if (!t.collection()) return kBad;
            w.root(nbt::kList);
            w.begin_list(t.size() ? nbt::kString : nbt::kEnd, t.size());
            for (const Tag key : t.elements()) {
                std::string name;
                if (!as_identifier(key, name)) return kBad;
                w.put_string(name);
            }
            return kOk;
        }
        case component::kContainerLoot: {  // SeededContainerLoot: loot_table (a key), seed (a long, 0 if left out)
            std::string table;
            if (t.type != nbt::kCompound || !as_identifier(t.get("loot_table"), table)) return kBad;
            const Tag seed = t.get("seed");
            if (seed && !seed.numeric()) return kBad;
            w.root(nbt::kCompound);
            w.put_string("loot_table", table);
            if (seed && seed.as_long() != 0) w.put_long("seed", seed.as_long());
            w.end_compound();
            return kOk;
        }
        case component::kMapDecorations: {  // a map of any names to {type, x, z, rotation}
            if (t.type != nbt::kCompound) return kBad;
            w.root(nbt::kCompound);
            for (const Tag e : t.entries()) {
                if (t.get(e.name()).at != e.at) continue;
                u32 kind;
                const Tag x = e.get("x"), z = e.get("z"), rotation = e.get("rotation");
                if (e.type != nbt::kCompound || !as_entry(e.get("type"), sreg::kMapDecorationTypeNames, kind) || !x.numeric() ||
                    !z.numeric() || !rotation.numeric())
                    return kBad;
                w.entry(nbt::kCompound, e.name());
                w.put_string("type", sreg::kMapDecorationTypeNames[kind]);
                w.put_double("x", x.as_double());
                w.put_double("z", z.as_double());
                w.put_float("rotation", static_cast<float>(rotation.as_double()));
                w.end_compound();
            }
            w.end_compound();
            return kOk;
        }
        case component::kDebugStickState: {  // a map of blocks to the name of one of their properties
            if (t.type != nbt::kCompound) return kBad;
            w.root(nbt::kCompound);
            for (const Tag e : t.entries()) {
                if (t.get(e.name()).at != e.at) continue;
                std::string block, property;
                proto::Identifier id;
                const std::string key = nbt::to_utf8(e.name());  // (the identifier is views into it)
                if (!proto::parse_identifier(key, id) || !as_string(e, property)) return kBad;
                block.assign(id.ns).append(":").append(id.path);
                const u32 b = world::find_block(block);
                if (b == world::kNoId || !has_property(b, property)) return kBad;
                w.put_string(block, property);
            }
            w.end_compound();
            return kOk;
        }
        case component::kLock: {  // LockCode: an ItemPredicate - items, count, components, predicates
            if (t.type != nbt::kCompound) return kBad;
            w.root(nbt::kCompound);
            if (const Tag items = t.get("items")) {  // a HolderSet of items: "#tag", one name, or a list of names
                std::string name;
                if (items.type == nbt::kString && items.str().starts_with('#')) {
                    const std::string tag = nbt::to_utf8(items.str()).substr(1);
                    proto::Identifier id;
                    if (!proto::parse_identifier(tag, id)) return kBad;
                    name.assign(id.ns).append(":").append(id.path);
                    if (!registry_has_tag("minecraft:item", name)) return kBad;
                    w.put_string("items", "#" + name);
                } else if (items.type == nbt::kString) {
                    if (!as_identifier(items, name) || world::find_item(name) == world::kNoId) return kBad;
                    w.put_string("items", name);
                } else {
                    if (!items.collection()) return kBad;
                    std::vector<std::string> names;
                    for (const Tag e : items.elements()) {
                        if (!as_identifier(e, name) || world::find_item(name) == world::kNoId) return kBad;
                        names.push_back(name);
                    }
                    if (names.size() == 1) {  // HolderSetCodec: one entry is written alone
                        w.put_string("items", names[0]);
                    } else {
                        w.begin_list("items", names.empty() ? nbt::kEnd : nbt::kString, static_cast<u32>(names.size()));
                        for (const std::string& n : names) w.put_string(n);
                    }
                }
            }
            if (const Tag count = t.get("count")) {  // MinMaxBounds.Ints: {min, max}, or one number for both
                i32 min = 0, max = 0;
                bool has_min = false, has_max = false;
                if (count.type == nbt::kCompound) {
                    const Tag lo = count.get("min"), hi = count.get("max");
                    has_min = static_cast<bool>(lo);
                    has_max = static_cast<bool>(hi);
                    if ((lo && !as_int(lo, min)) || (hi && !as_int(hi, max)) || (lo && hi && min > max)) return kBad;
                } else {
                    if (!as_int(count, min)) return kBad;
                    max = min;
                    has_min = has_max = true;
                }
                if (has_min && has_max && min == max) {
                    w.put_int("count", min);
                } else if (has_min || has_max) {
                    w.begin_compound("count");
                    if (has_min) w.put_int("min", min);
                    if (has_max) w.put_int("max", max);
                    w.end_compound();
                }
            }
            if (const Tag components = t.get("components")) {  // DataComponentExactPredicate: components and their values
                if (components.type != nbt::kCompound) return kBad;
                bool any = false;
                base::ByteBuf value;
                for (const Tag e : components.entries()) {
                    if (components.get(e.name()).at != e.at) continue;
                    const std::string key = nbt::to_utf8(e.name());
                    const u32 of = find_component(key);
                    if (of == kNoComponent || transient(of)) return kBad;
                    value.clear();
                    if (const Verdict v = value_from_nbt(of, e, value); v != kOk) return v;
                    if (!any) w.begin_compound("components");
                    any = true;
                    Reader stored(value.data, value.len);
                    to(of, stored, w, component::kNames[of]);
                }
                if (any) w.end_compound();
            }
            if (const Tag predicates = t.get("predicates")) {  // tests of components: not read
                if (predicates.type != nbt::kCompound) return kBad;
                if (predicates.size() != 0) return kNotRead;
            }
            w.end_compound();
            return kOk;
        }
        default: return kNotRead;
    }
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
    if (const Names names = registry_names(type); !names.empty()) {
        u32 id;
        if (!as_entry(t, names, id)) return kBad;
        out.varint(id);
        return kOk;
    }
    if (const std::span<const variant::Named> values = enum_values(type); !values.empty()) {
        std::string name;
        if (!as_string(t, name)) return kBad;
        for (const variant::Named& v : values) {
            if (v.name != name) continue;
            out.varint(v.id);
            return kOk;
        }
        return kBad;
    }
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
        case component::kBlockEntityData: return typed_data_from(t, out, type == component::kEntityData);
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
        // ---- stacks in stacks
        case component::kContainer: {  // a list of {slot, item}, at most 256, slots 0..255
            if (!t.collection() || t.size() > 256) return kBad;
            std::pair<u32, u32> at[256] = {};  // where a slot's template is, and how long
            base::ByteBuf templates;
            Out to_templates{templates};
            u32 slots = 0;
            for (const Tag e : t.elements()) {
                i32 slot;
                if (e.type != nbt::kCompound || !as_int(e.get("slot"), slot) || slot < 0 || slot > 255 || !e.get("item"))
                    return kBad;
                const u32 start = templates.len;
                if (const Verdict v = template_from(e.get("item"), to_templates); v != kOk) return v;
                at[slot] = {start, templates.len - start};
                slots = std::max(slots, static_cast<u32>(slot) + 1);
            }
            out.varint(slots);
            for (u32 i = 0; i < slots; ++i) {
                out.u8v(at[i].second != 0);
                out.bytes({templates.data + at[i].first, at[i].second});
            }
            return kOk;
        }
        case component::kBundleContents:
        case component::kChargedProjectiles: {  // a list of templates (charged: at most 1024)
            if (!t.collection() || (type == component::kChargedProjectiles && t.size() > 1024)) return kBad;
            out.varint(t.size());
            for (const Tag e : t.elements())
                if (const Verdict v = template_from(e, out); v != kOk) return v;
            return kOk;
        }
        case component::kSulfurCubeContent: return template_from(t, out);
        case component::kPotDecorations: {  // back, left, right, front: each a template, or left out
            if (t.type != nbt::kCompound) return kBad;
            for (const std::string_view side : {"back", "left", "right", "front"}) {
                const Tag item = t.get(side);
                out.u8v(static_cast<bool>(item));
                if (!item) continue;
                if (const Verdict v = template_from(item, out); v != kOk) return v;
            }
            return kOk;
        }
        case component::kBees: {  // a list of {entity_data, ticks_in_hive, min_ticks_in_hive}
            if (!t.collection()) return kBad;
            out.varint(t.size());
            for (const Tag e : t.elements()) {
                i32 ticks, least;
                if (e.type != nbt::kCompound || !as_int(e.get("ticks_in_hive"), ticks) ||
                    !as_int(e.get("min_ticks_in_hive"), least))
                    return kBad;
                if (const Verdict v = typed_data_from(e.get("entity_data"), out, true); v != kOk) return v;
                out.varint(static_cast<u32>(ticks));
                out.varint(static_cast<u32>(least));
            }
            return kOk;
        }
        // ---- the rest of what is only data
        case component::kCustomModelData: {  // floats, flags, strings, colors: lists, each empty if left out
            if (t.type != nbt::kCompound) return kBad;
            const Tag floats = t.get("floats"), flags = t.get("flags"), strings = t.get("strings"), colors = t.get("colors");
            for (const Tag list : {floats, flags, strings, colors})
                if (list && !list.collection()) return kBad;
            out.varint(floats ? floats.size() : 0);
            if (floats)
                for (const Tag e : floats.elements()) {
                    float v;
                    if (!as_float(e, v)) return kBad;
                    out.be32(std::bit_cast<u32>(v));
                }
            out.varint(flags ? flags.size() : 0);
            if (flags)
                for (const Tag e : flags.elements()) {
                    bool v;
                    if (!as_bool(e, v)) return kBad;
                    out.u8v(v);
                }
            out.varint(strings ? strings.size() : 0);
            if (strings)
                for (const Tag e : strings.elements()) {
                    std::string v;
                    if (!as_string(e, v)) return kBad;
                    out.str(v);
                }
            out.varint(colors ? colors.size() : 0);
            if (colors)
                for (const Tag e : colors.elements()) {  // ExtraCodecs.RGB_COLOR_CODEC: an int - or three floats, not read
                    i32 v;
                    if (e.collection()) return kNotRead;
                    if (!as_int(e, v)) return kBad;
                    out.be32(static_cast<u32>(v));
                }
            return kOk;
        }
        case component::kTrim: {  // ArmorTrim: material and pattern, holders - an entry written out is not read
            if (t.type != nbt::kCompound) return kBad;
            const Tag material = t.get("material"), pattern = t.get("pattern");
            if (material.type == nbt::kCompound || pattern.type == nbt::kCompound) return kNotRead;
            u32 m, p;
            if (!as_entry(material, reg::trim_material::kNames, m) || !as_entry(pattern, reg::trim_pattern::kNames, p))
                return kBad;
            out.varint(m + 1);
            out.varint(p + 1);
            return kOk;
        }
        case component::kProvidesTrimMaterial: {
            if (t.type == nbt::kCompound) return kNotRead;
            u32 id;
            if (!as_entry(t, reg::trim_material::kNames, id)) return kBad;
            out.varint(id + 1);
            return kOk;
        }
        case component::kProvidesBannerPatterns: {  // a HolderSet: "#tag", one name, or a list of names
            std::string name;
            if (t.type == nbt::kString && t.str().starts_with('#')) {
                proto::Identifier id;
                const std::string tag = nbt::to_utf8(t.str()).substr(1);
                if (!proto::parse_identifier(tag, id)) return kBad;
                out.varint(0);
                out.str(std::string(id.ns).append(":").append(id.path));  // (that the tag exists: check_value)
                return kOk;
            }
            u32 id;
            if (t.type == nbt::kString) {
                if (!as_entry(t, reg::banner_pattern::kNames, id)) return kBad;
                out.varint(2);
                out.varint(id);
                return kOk;
            }
            if (!t.collection()) return kBad;
            out.varint(t.size() + 1);
            for (const Tag e : t.elements()) {
                if (!as_entry(e, reg::banner_pattern::kNames, id)) return kBad;
                out.varint(id);
            }
            return kOk;
        }
        case component::kLodestoneTracker: {  // target: {dimension, pos} or left out; tracked: true if left out
            if (t.type != nbt::kCompound) return kBad;
            bool tracked;
            if (!optional(t, "tracked", tracked, true, as_bool)) return kBad;
            const Tag target = t.get("target");
            out.u8v(static_cast<bool>(target));
            if (target) {
                std::string dimension;
                const Tag pos = target.get("pos");  // BlockPos.CODEC: three ints, as an array or a list
                if (target.type != nbt::kCompound || !as_identifier(target.get("dimension"), dimension) || !pos.collection() ||
                    pos.size() != 3)
                    return kBad;
                i64 c[3];
                u32 i = 0;
                for (const Tag e : pos.elements()) {
                    if (!e.numeric()) return kBad;
                    c[i++] = e.as_int();
                }
                // On the wire a position is a long of 26 + 26 + 12 bits: one beyond it does not come back as it went.
                if (c[0] < -(1 << 25) || c[0] >= 1 << 25 || c[2] < -(1 << 25) || c[2] >= 1 << 25 || c[1] < -(1 << 11) ||
                    c[1] >= 1 << 11)
                    return kNotRead;
                out.str(dimension);
                const u64 packed = (static_cast<u64>(c[0]) & 0x3FFFFFF) << 38 | (static_cast<u64>(c[2]) & 0x3FFFFFF) << 12 |
                                   (static_cast<u64>(c[1]) & 0xFFF);
                out.be32(static_cast<u32>(packed >> 32));
                out.be32(static_cast<u32>(packed));
            }
            out.u8v(tracked);
            return kOk;
        }
        case component::kBreakSound: {  // a Holder<SoundEvent>: its name, or {sound_id, range}
            if (t.type == nbt::kCompound) {
                std::string sound;
                if (!as_identifier(t.get("sound_id"), sound)) return kBad;
                const Tag range = t.get("range");  // (lenient: a range that is no number is none)
                out.varint(0);
                out.str(sound);
                out.u8v(range.numeric());
                if (range.numeric()) out.be32(std::bit_cast<u32>(static_cast<float>(range.as_double())));
                return kOk;
            }
            u32 id;
            if (!as_entry(t, sreg::kSoundEventNames, id)) return kBad;
            out.varint(id + 1);
            return kOk;
        }
        case component::kProfile: return profile_from(t, out);
        case component::kIntangibleProjectile:
        case component::kMapDecorations:
        case component::kDebugStickState:
        case component::kRecipes:
        case component::kLock:
        case component::kContainerLoot: return tag_component(type, t, out.b);
        case component::kCreativeSlotLock:
        case component::kAdditionalTradeCost:
        case component::kMapPostProcessing: return kBad;  // no Codec: not a component a tag can name
        default: return kNotRead;
    }
}

void to(u32 type, Reader& in, nbt::Writer& w, std::string_view name) {
    if (const Names names = registry_names(type); !names.empty()) {
        w.put_string(name, names[in.varint()]);
        return;
    }
    if (const std::span<const variant::Named> values = enum_values(type); !values.empty()) {
        const u32 id = in.varint();
        for (const variant::Named& v : values)
            if (v.id == id) w.put_string(name, v.name);
        return;
    }
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
        case component::kBlockEntityData:
            w.entry(nbt::kCompound, name);
            typed_data_to(in, w, type == component::kEntityData);
            w.end_compound();
            break;
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
        // ---- stacks in stacks
        case component::kContainer: {
            ListOf slots;
            const u32 n = in.varint();
            for (u32 i = 0; i < n; ++i) {
                if (in.u8v() == 0) continue;
                slots.w.put_int("slot", static_cast<i32>(i));
                slots.w.begin_compound("item");
                template_to(in, slots.w);
                slots.w.end_compound();
                slots.w.end_compound();
                ++slots.count;
            }
            slots.put(w, name);
            break;
        }
        case component::kBundleContents:
        case component::kChargedProjectiles: {
            const u32 n = in.varint();
            w.begin_list(name, n ? nbt::kCompound : nbt::kEnd, n);
            for (u32 i = 0; i < n; ++i) {
                template_to(in, w);
                w.end_compound();
            }
            break;
        }
        case component::kSulfurCubeContent:
            w.begin_compound(name);
            template_to(in, w);
            w.end_compound();
            break;
        case component::kPotDecorations:
            w.begin_compound(name);
            for (const std::string_view side : {"back", "left", "right", "front"}) {
                if (in.u8v() == 0) continue;
                w.begin_compound(side);
                template_to(in, w);
                w.end_compound();
            }
            w.end_compound();
            break;
        case component::kBees: {
            const u32 n = in.varint();
            w.begin_list(name, n ? nbt::kCompound : nbt::kEnd, n);
            for (u32 i = 0; i < n; ++i) {
                w.begin_compound("entity_data");
                typed_data_to(in, w, true);
                w.end_compound();
                w.put_int("ticks_in_hive", static_cast<i32>(in.varint()));
                w.put_int("min_ticks_in_hive", static_cast<i32>(in.varint()));
                w.end_compound();
            }
            break;
        }
        // ---- the rest of what is only data
        case component::kCustomModelData: {
            w.begin_compound(name);
            if (const u32 n = in.varint()) {
                w.begin_list("floats", nbt::kFloat, n);
                for (u32 i = 0; i < n; ++i) w.put_float(std::bit_cast<float>(in.be32()));
            }
            if (const u32 n = in.varint()) {
                w.begin_list("flags", nbt::kByte, n);
                for (u32 i = 0; i < n; ++i) w.put_byte(static_cast<i8>(in.u8v()));
            }
            if (const u32 n = in.varint()) {
                w.begin_list("strings", nbt::kString, n);
                for (u32 i = 0; i < n; ++i) w.put_string(take_str(in));
            }
            if (const u32 n = in.varint()) {
                w.begin_list("colors", nbt::kInt, n);
                for (u32 i = 0; i < n; ++i) w.put_int(static_cast<i32>(in.be32()));
            }
            w.end_compound();
            break;
        }
        case component::kTrim:
            w.begin_compound(name);
            w.put_string("material", reg::trim_material::kNames[in.varint() - 1]);
            w.put_string("pattern", reg::trim_pattern::kNames[in.varint() - 1]);
            w.end_compound();
            break;
        case component::kProvidesTrimMaterial: w.put_string(name, reg::trim_material::kNames[in.varint() - 1]); break;
        case component::kProvidesBannerPatterns: {  // HolderSetCodec: "#tag"; one name alone; else a list
            const u32 n = in.varint();
            if (n == 0) {
                std::string tag = "#";
                tag += take_str(in);
                w.put_string(name, tag);
            } else if (n == 2) {
                w.put_string(name, reg::banner_pattern::kNames[in.varint()]);
            } else {
                w.begin_list(name, n > 1 ? nbt::kString : nbt::kEnd, n - 1);
                for (u32 i = 1; i < n; ++i) w.put_string(reg::banner_pattern::kNames[in.varint()]);
            }
            break;
        }
        case component::kLodestoneTracker: {
            w.begin_compound(name);
            if (in.u8v()) {
                w.begin_compound("target");
                w.put_string("dimension", take_str(in));
                const u64 packed = static_cast<u64>(in.be32()) << 32 | in.be32();
                const i32 pos[3] = {static_cast<i32>(static_cast<i64>(packed) >> 38),
                                    static_cast<i32>(static_cast<i64>(packed << 52) >> 52),
                                    static_cast<i32>(static_cast<i64>(packed << 26) >> 38)};
                w.put_int_array("pos", pos);
                w.end_compound();
            }
            if (!in.u8v()) w.put_byte("tracked", 0);
            w.end_compound();
            break;
        }
        case component::kBreakSound: {
            const u32 id = in.varint();
            if (id != 0) {
                w.put_string(name, sreg::kSoundEventNames[id - 1]);
                break;
            }
            w.begin_compound(name);
            w.put_string("sound_id", take_str(in));
            if (in.u8v()) w.put_float("range", std::bit_cast<float>(in.be32()));
            w.end_compound();
            break;
        }
        case component::kProfile:
            w.begin_compound(name);
            profile_to(in, w);
            w.end_compound();
            break;
        case component::kIntangibleProjectile:
        case component::kMapDecorations:
        case component::kDebugStickState:
        case component::kRecipes:
        case component::kLock:
        case component::kContainerLoot: {
            const Stored t = take_tag(in);
            w.put_raw(name, t.type, t.payload);
            break;
        }
        default: DCHECK(false); break;
    }
}

}  // namespace

bool transient(u32 type) {
    return type == component::kCreativeSlotLock || type == component::kAdditionalTradeCost ||
           type == component::kMapPostProcessing;
}

Verdict tag_component(u32 type, Tag t, base::ByteBuf& out) {
    if (!t) return kBad;
    base::ByteBuf built;
    nbt::Writer w(built);
    if (const Verdict v = tag_component_built(type, t, w); v != kOk) return v;
    return finish_tag(built, out);
}

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
        for (u32 t = 0; t < component::kCount; ++t) k[t] = !transient(t) && check_value(t, {}, scratch) != Verdict::kUnsupported;
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
        if (type == kNoComponent || transient(type)) return kBad;  // PatchKey.CODEC: "... is not a persistent component"
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

bool contained_sizes_fit(const ItemStack& stack, u32& count, u32& max) {
    std::vector<Contained> stacks;
    for (const u32 type : {component::kContainer, component::kBundleContents, component::kChargedProjectiles}) {
        const Patches::Found held = effective(stack, type);
        if (held.state != Patches::State::kSet) continue;
        stacks.clear();
        contained(type, held.value, stacks);
        for (const Contained& c : stacks) {
            // ItemInstance.getMaxStackSize: the template's MAX_STACK_SIZE, its patch's or its item's, else 1.
            const Patches::Found size = find_nested(c.patch, component::kMaxStackSize);
            const Default of_item = default_of(c.item, component::kMaxStackSize);
            const std::span<const u8> value = size.state == Patches::State::kSet ? size.value : of_item.value;
            max = 1;
            if (size.state != Patches::State::kRemoved && !value.empty()) {
                Reader r(value);
                max = r.varint();
            }
            count = c.count;
            if (count > max) return false;
        }
    }
    return true;
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

std::string computed_name_key(const ItemStack& stack, std::string* arg) {
    namespace it = proto::gen::item;
    const u32 item = stack.item();
    std::string key(world::item_text_key(item));
    if (item == it::kCompass)  // CompassItem.getName
        return effective(stack, component::kLodestoneTracker).state == Patches::State::kSet ? "item.minecraft.lodestone_compass"
                                                                                            : "";
    if (item == it::kPlayerHead) {  // PlayerHeadItem.getName: "<name>'s Head", if the profile has a name
        const Patches::Found profile = effective(stack, component::kProfile);
        if (profile.state != Patches::State::kSet) return {};
        Reader in(profile.value);
        if (in.u8v()) {
            in.bytes(16);
        } else if (!in.u8v()) {
            return {};
        }
        if (arg) *arg = take_str(in);
        return key + ".named";
    }
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
        if (transient(e.type)) continue;  // no Codec: no part of a tag
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
