// SPDX-License-Identifier: GPL-3.0-or-later
// What the game holds, as the tags vanilla saves it as: BlockEntity.saveWithFullMetadata and
// Entity.saveWithoutId (26.3) - for `/data get`, and for whatever compares a tag with one
// (`@e[nbt=...]`) - and the way back: a block entity out of such a tag
// (BlockEntity.loadWithComponents), for `/setblock chest{...}`, for a block item that carries
// data, for a block entity of an imported world. Nothing here is what the server keeps on disk:
// that is server/persist.cpp, in a form of its own.
//
// Of a chest, a barrel and a furnace the server holds the stacks and the timers itself; what
// else their tag says - CustomName, lock, components, the recipes a furnace has used - is kept
// beside them as a tag (Chest::keep), each entry as its vanilla codec writes it, and read where
// it is acted on: the name when a menu opens, the lock before.
//
// Entries are written in the order vanilla's code puts them, because the text of `/data get`
// shows a compound in the order of a HashMap, and what that is depends on the order of arrival
// (nbt::map_order).
//
// Of an entity, what the server does not keep is written as the value vanilla gives a fresh one
// (Fire, Air, PortalCooldown...).
#include <cstring>

#include "item/codec.hpp"
#include "nbt/nbt.hpp"
#include "nbt/ops.hpp"
#include "proto/identifier.hpp"
#include "server/game.hpp"
#include "server/persist.hpp"
#include "text/component.hpp"

#include PROTO_GEN_PHYSICS_HPP
#include PROTO_GEN_STATIC_IDS_HPP

namespace server {

using namespace proto;
namespace gp = proto::gen::physics;

namespace {

// BlockState.CODEC: a block's default state is its name; any other the block by name and its
// properties by theirs.
void put_block_state(nbt::Writer& w, std::string_view name, world::BlockState state) {
    const u32 block = world::block_of(state);
    const world::BlockInfo& info = world::block_info(block);
    if (state == info.default_state) {
        w.put_string(name, world::block_name(block));
        return;
    }
    w.begin_compound(name);
    w.put_string("Name", world::block_name(block));
    w.begin_compound("Properties");
    for (u32 i = 0, n = world::block_property_count(block); i < n; ++i) {
        const world::BlockProperty p = world::block_property(block, i);
        w.put_string(p.name, p.values[(state - info.first_state) / p.stride % p.values.size()]);
    }
    w.end_compound();
    w.end_compound();
}

// UUIDUtil.CODEC: four ints, the most significant first.
void put_uuid(nbt::Writer& w, std::string_view name, Uuid id) {
    const i32 parts[4] = {static_cast<i32>(id.hi >> 32), static_cast<i32>(id.hi), static_cast<i32>(id.lo >> 32),
                          static_cast<i32>(id.lo)};
    w.put_int_array(name, parts);
}

// What the tag of a chest, a barrel or a furnace says: loadAdditional of BaseContainerBlockEntity,
// ChestBlockEntity / BarrelBlockEntity and AbstractFurnaceBlockEntity, and BlockEntity.components.
struct ContainerTag {
    ItemStack items[Chest::kSlots];
    i32 lit_remaining = 0, lit_total = 0, cook = 0, cook_total = 0;
    float speed = 1.0f;
    base::ByteBuf rest;  // CustomName, lock, RecipesUsed, components: a nameless compound; empty for none of them
};

// Reads it as vanilla's ValueInput does: what is not there is its default, and of what is
// wrong vanilla keeps what the failed decode left it (DataResult's partial value) - a stack
// whose count is none is one of it, a lock one of whose fields is wrong is a lock without
// that field, an item that does not exist is no stack at all. False, and `why`, where the
// tag asks for something this server cannot do - a loot table to roll, a stack or a text it
// does not read: such a tag is not half taken.
// `components`: the entries of the block entity's components to keep when the tag is read
// without them (loadCustomOnly), else none - then the tag's own are read.
bool read_container_tag(nbt::Tag tag, u32 slots, bool furnace, bool with_components, nbt::Tag components, ContainerTag& out,
                        std::string& why) {
    if (tag.type != nbt::kCompound) return true;  // (nothing to read: everything is its default)
    nbt::Writer w(out.rest);
    bool any = false;
    const auto entry = [&](std::string_view name, std::span<const u8> nameless) {
        if (!any) w.root(nbt::kCompound);
        any = true;
        w.put_raw(name, static_cast<nbt::Type>(nameless[0]), nameless.subspan(1));
    };
    base::ByteBuf value;
    // ---- BaseContainerBlockEntity: lock (LockCode.fromTag), CustomName (parseCustomNameSafe)
    if (const nbt::Tag lock = tag.get("lock"); lock.type == nbt::kCompound) {
        // A field of the predicate that is wrong is left out, and the rest is the lock: vanilla
        // goes on with what the failed decode left it (an optional field, absent).
        base::ByteBuf fields;
        nbt::Writer fw(fields);
        fw.root(nbt::kCompound);
        for (const std::string_view field : {"items", "count", "components", "predicates"}) {
            const nbt::Tag f = lock.get(field);
            if (!f) continue;
            base::ByteBuf one;
            nbt::Writer ow(one);
            ow.root(nbt::kCompound);
            ow.put_raw(field, f.type, f.bytes());
            ow.end_compound();
            nbt::Doc alone;
            alone.parse({one.data, one.len}, nbt::Form::kNameless, nbt::kTrusted);
            value.clear();
            const item::Verdict v = item::tag_component(item::component::kLock, alone.root(), value);
            if (v == item::Verdict::kUnsupported) {
                why = "Locks that test components";
                return false;
            }
            if (v == item::Verdict::kOk) fw.put_raw(field, f.type, f.bytes());
        }
        fw.end_compound();
        nbt::Doc kept;
        kept.parse({fields.data, fields.len}, nbt::Form::kNameless, nbt::kTrusted);
        value.clear();
        if (item::tag_component(item::component::kLock, kept.root(), value) == item::Verdict::kOk)
            entry("lock", {value.data, value.len});
    }
    if (const nbt::Tag name = tag.get("CustomName")) {
        value.clear();
        const text::Result v = text::transcode(name, value);
        if (v == text::Result::kUnsupported) {
            why = "Names with parts of a text this server does not read";
            return false;
        }
        if (v == text::Result::kOk) entry("CustomName", {value.data, value.len});
    }
    // ---- the items: ContainerHelper.loadAllItems - unless a chest names a loot table to roll
    if (!furnace) {
        const nbt::Tag loot = tag.get("LootTable");
        proto::Identifier id;
        std::string name;
        if (loot.type == nbt::kString && proto::parse_identifier(name = nbt::to_utf8(loot.str()), id)) {
            why = "Loot tables of containers";
            return false;
        }
    }
    if (const nbt::Tag items = tag.get("Items"); items.type == nbt::kList) {
        for (const nbt::Tag e : items.elements()) {
            if (e.type != nbt::kCompound) continue;
            // ExtraCodecs.UNSIGNED_BYTE: a number's byte, unsigned; 0 if left out - or if it is no number.
            const nbt::Tag slot = e.get("Slot");
            const u32 at = slot.numeric() ? static_cast<u8>(slot.as_int()) : 0;
            ItemStack stack;
            const item::Verdict v = item::stack_from_nbt(e, stack, true);
            if (v == item::Verdict::kUnsupported) {
                why = "Items with components this server does not read";
                return false;
            }
            if (v == item::Verdict::kOk && at < slots) out.items[at] = std::move(stack);  // isValidInContainer
        }
    }
    if (furnace) {
        const auto number = [&](std::string_view name, i32 fallback) {  // getIntOr
            const nbt::Tag t = tag.get(name);
            return t.numeric() ? t.as_int() : fallback;
        };
        out.cook = number("cooking_time_spent", 0);
        out.cook_total = number("cooking_total_time", 0);
        out.lit_remaining = number("lit_time_remaining", 0);
        out.lit_total = number("lit_total_time", 0);
        if (const nbt::Tag speed = tag.get("speed_multiplier"); speed.numeric())
            out.speed = static_cast<float>(speed.as_double());
        // RecipesUsed: recipes and how often - the experience it owes. Nothing here reads it.
        if (const nbt::Tag used = tag.get("RecipesUsed"); used.type == nbt::kCompound && used.size()) {
            bool ok = true;
            for (const nbt::Tag e : used.entries()) {
                proto::Identifier id;
                const std::string key = nbt::to_utf8(e.name());
                ok &= e.numeric() && proto::parse_identifier(key, id);
            }
            if (ok) {
                if (!any) w.root(nbt::kCompound);
                any = true;
                w.put_raw("RecipesUsed", nbt::kCompound, used.bytes());
            }
        }
    }
    // ---- BlockEntity.components: DataComponentMap.CODEC - component and value; what is wrong is left out
    if (!with_components) {
        if (components.type == nbt::kCompound && components.size()) {
            if (!any) w.root(nbt::kCompound);
            any = true;
            w.put_raw("components", nbt::kCompound, components.bytes());
        }
    } else if (const nbt::Tag given = tag.get("components"); given.type == nbt::kCompound) {
        base::ByteBuf map;
        nbt::Writer mw(map);
        mw.root(nbt::kCompound);
        u32 n = 0;
        for (const nbt::Tag e : given.entries()) {
            if (given.get(e.name()).at != e.at) continue;
            const std::string key = nbt::to_utf8(e.name());
            const u32 type = item::find_component(key);
            if (type == item::kNoComponent || item::transient(type)) continue;
            value.clear();
            const item::Verdict v = item::value_from_nbt(type, e, value);
            if (v == item::Verdict::kUnsupported) {
                why = "Block entities with components this server does not read";
                return false;
            }
            if (v != item::Verdict::kOk) continue;
            item::value_to_nbt(type, {value.data, value.len}, mw, item::component_name(type));
            ++n;
        }
        mw.end_compound();
        if (n) entry("components", {map.data, map.len});
    }
    if (any) w.end_compound();
    return true;
}

// The components of a block entity's tag (DataComponentMap.CODEC), as a tag to keep: {components: {...}},
// empty for none. False: one of them is a component this server does not read.
bool read_components(nbt::Tag given, base::ByteBuf& rest) {
    if (given.type != nbt::kCompound) return true;
    base::ByteBuf map, value;
    nbt::Writer mw(map);
    mw.root(nbt::kCompound);
    u32 n = 0;
    for (const nbt::Tag e : given.entries()) {
        if (given.get(e.name()).at != e.at) continue;
        const std::string key = nbt::to_utf8(e.name());
        const u32 type = item::find_component(key);
        if (type == item::kNoComponent || item::transient(type)) continue;
        value.clear();
        const item::Verdict v = item::value_from_nbt(type, e, value);
        if (v == item::Verdict::kUnsupported) return false;
        if (v != item::Verdict::kOk) continue;
        item::value_to_nbt(type, {value.data, value.len}, mw, item::component_name(type));
        ++n;
    }
    mw.end_compound();
    if (!n) return true;
    nbt::Writer w(rest);
    w.root(nbt::kCompound);
    w.put_raw("components", nbt::kCompound, {map.data + 1, map.len - 1});
    w.end_compound();
    return true;
}

// SignBlockEntity.loadAdditional: allow_op_features, front_text, back_text, is_waxed. A text
// that is wrong is an empty one (SignText.EMPTY); one this server does not read is refused.
struct SignTag {
    base::ByteBuf text[2];
    bool waxed = false, allow_op = false;
};
bool read_sign_tag(nbt::Tag tag, SignTag& out, std::string& why) {
    const auto flag = [&](std::string_view name) {  // getBooleanOr: a number that is not 0
        const nbt::Tag t = tag.get(name);
        return t.numeric() && t.as_int() != 0;
    };
    out.allow_op = flag("allow_op_features");
    out.waxed = flag("is_waxed");
    for (u32 side = 0; side < 2; ++side) {
        const nbt::Tag text = tag.get(side == 0 ? "front_text" : "back_text");
        if (!text) continue;
        const item::Verdict v = item::value_from_nbt(item::component::kSignTextFront, text, out.text[side]);
        if (v == item::Verdict::kUnsupported) {
            why = "Signs with parts of a text this server does not read";
            return false;
        }
        if (v != item::Verdict::kOk) out.text[side].clear();
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------- block entities

nbt::Tag Game::kept_entry(u32 keep, std::string_view name, nbt::Doc& doc) const {
    if (keep == kNone || doc.parse(opaque_[keep], nbt::Form::kNameless, nbt::kTrusted) != nbt::Error::kNone) return {};
    return doc.root().get(name);
}

bool Game::can_load_block_entity(u32 block, nbt::Tag tag, std::string& why) {
    const u32 kind = world::block_info(block).kind;
    if (sign_kind(kind)) {
        SignTag read;
        base::ByteBuf rest;
        if (!read_sign_tag(tag, read, why)) return false;
        if (!read_components(tag.get("components"), rest)) why = "Block entities with components this server does not read";
        return why.empty();
    }
    if (!chest_kind(kind) && kind != gen::block_kind::kFurnace)
        return true;  // no block entity of this server's: nothing is loaded
    ContainerTag read;
    return read_container_tag(tag, chest_kind(kind) ? Chest::kSlots : Furnace::kSlots, !chest_kind(kind), true, {}, read, why);
}

Game::Loaded Game::load_block_entity(BlockPos pos, nbt::Tag tag, bool with_components, bool& changed, std::string& why) {
    changed = false;
    const world::BlockEntityRef* ref = world_.block_entity(pos.x, pos.y, pos.z);
    if (!ref) return Loaded::kNone;
    if (ref->data & kOpaqueBlockEntity) {
        why = "Block entities that were imported as they are";
        return Loaded::kRefused;
    }
    const u32 kind = world::block_info_of(world_.block(pos.x, pos.y, pos.z)).kind;
    if (sign_kind(kind)) {
        SignTag read;
        base::ByteBuf rest, before, after;
        if (!read_sign_tag(tag, read, why)) return Loaded::kRefused;
        if (with_components && !read_components(tag.get("components"), rest)) {
            why = "Block entities with components this server does not read";
            return Loaded::kRefused;
        }
        save_block_entity(pos.x, pos.y, pos.z, before);
        Sign& sign = signs_[ref->data];
        for (u32 side = 0; side < 2; ++side)
            sign.text[side].assign(read.text[side].data, read.text[side].data + read.text[side].len);
        sign.waxed = read.waxed, sign.allow_op = read.allow_op;
        if (with_components) {
            drop_kept(sign.keep);
            sign.keep = keep_tag({rest.data, rest.len});
        }
        save_block_entity(pos.x, pos.y, pos.z, after);
        changed = before.len != after.len || std::memcmp(before.data, after.data, before.len) != 0;
        if (changed) sign_changed(ref->data);
        return Loaded::kOk;
    }
    const bool chest = chest_kind(kind);
    if (!chest && kind != gen::block_kind::kFurnace) return Loaded::kNone;
    u32& keep = chest ? chests_[ref->data].keep : furnaces_[ref->data].keep;
    nbt::Doc kept;
    ContainerTag read;
    if (!read_container_tag(tag, chest ? Chest::kSlots : Furnace::kSlots, !chest, with_components,
                            with_components ? nbt::Tag{} : kept_entry(keep, "components", kept), read, why))
        return Loaded::kRefused;
    base::ByteBuf before, after;
    save_block_entity(pos.x, pos.y, pos.z, before);
    if (chest) {
        Chest& c = chests_[ref->data];
        for (u32 i = 0; i < Chest::kSlots; ++i) c.items[i] = std::move(read.items[i]);
    } else {
        Furnace& f = furnaces_[ref->data];
        for (u32 i = 0; i < Furnace::kSlots; ++i) f.items[i] = std::move(read.items[i]);
        f.cook = read.cook, f.cook_total = read.cook_total, f.lit_remaining = read.lit_remaining, f.lit_total = read.lit_total;
        f.speed = read.speed;
    }
    drop_kept(keep);
    keep = keep_tag({read.rest.data, read.rest.len});
    save_block_entity(pos.x, pos.y, pos.z, after);
    changed = before.len != after.len || std::memcmp(before.data, after.data, before.len) != 0;
    if (changed) world_.mark_dirty(pos.x, pos.z);
    return Loaded::kOk;
}

bool Game::adopt_kept_block_entity(BlockPos pos, world::BlockState state, u32 type, std::span<const u8> tag) {
    const world::BlockInfo& info = world::block_info_of(state);
    const bool chest = chest_kind(info.kind);
    if ((!chest && info.kind != gen::block_kind::kFurnace && !sign_kind(info.kind)) || type != info.block_entity) return false;
    nbt::Doc doc;
    if (doc.parse(tag, nbt::Form::kNameless, nbt::kTrusted) != nbt::Error::kNone) return false;
    if (sign_kind(info.kind)) {  // a sign of a vanilla world shows what is written on it
        SignTag read;
        base::ByteBuf rest;
        std::string why;
        if (!read_sign_tag(doc.root(), read, why) || !read_components(doc.root().get("components"), rest)) return false;
        const u32 data = new_sign(pos);
        for (u32 side = 0; side < 2; ++side)
            signs_[data].text[side].assign(read.text[side].data, read.text[side].data + read.text[side].len);
        signs_[data].waxed = read.waxed, signs_[data].allow_op = read.allow_op;
        signs_[data].keep = keep_tag({rest.data, rest.len});
        world_.add_block_entity(pos.x, pos.y, pos.z, type, data);
        show_sign(data);
        world_.mark_dirty(pos.x, pos.z);
        return true;
    }
    ContainerTag read;
    std::string why;
    if (!read_container_tag(doc.root(), chest ? Chest::kSlots : Furnace::kSlots, !chest, true, {}, read, why)) return false;
    u32 data;
    if (chest) {
        data = new_chest(pos);
        for (u32 i = 0; i < Chest::kSlots; ++i) chests_[data].items[i] = std::move(read.items[i]);
        chests_[data].keep = keep_tag({read.rest.data, read.rest.len});
    } else {
        data = new_furnace(pos, cooker_of(type));
        Furnace& f = furnaces_[data];
        for (u32 i = 0; i < Furnace::kSlots; ++i) f.items[i] = std::move(read.items[i]);
        f.cook = read.cook, f.cook_total = read.cook_total, f.lit_remaining = read.lit_remaining, f.lit_total = read.lit_total;
        f.speed = read.speed;
        f.keep = keep_tag({read.rest.data, read.rest.len});
    }
    world_.add_block_entity(pos.x, pos.y, pos.z, type, data);
    world_.mark_dirty(pos.x, pos.z);  // (it is written as a chest from now on)
    return true;
}

Game::Saved Game::save_block_entity(i32 x, i32 y, i32 z, base::ByteBuf& out) {
    const world::BlockEntityRef* ref = world_.block_entity(x, y, z);
    if (!ref) return Saved::kNone;
    if (ref->data & kOpaqueBlockEntity) {  // kept as the tag vanilla saved: that is the tag
        const std::vector<u8>& kept = opaque_[ref->data & ~kOpaqueBlockEntity];
        out.append(kept.data(), kept.size());
        return Saved::kOk;
    }
    const u32 kind = world::block_info_of(world_.block(x, y, z)).kind;
    nbt::Writer w(out);
    w.root(nbt::kCompound);
    if (sign_kind(kind)) {  // SignBlockEntity.saveAdditional, BlockEntity.components, saveMetadata
        const Sign& sign = signs_[ref->data];
        write_sign_tag(sign, w);
        nbt::Doc doc;
        if (const nbt::Tag components = kept_entry(sign.keep, "components", doc)) {
            w.put_raw("components", components.type, components.bytes());
        } else {
            w.begin_compound("components");
            w.end_compound();
        }
        w.put_string("id", world::block_entity_type_name(ref->type));
        w.put_int("x", x);
        w.put_int("y", y);
        w.put_int("z", z);
        w.end_compound();
        return Saved::kOk;
    }
    // ContainerHelper.saveAllItems: the slots that hold something.
    const auto items = [&](const ItemStack* stacks, u32 n) {
        u32 held = 0;
        for (u32 i = 0; i < n; ++i) held += !stacks[i].empty();
        w.begin_list("Items", held ? nbt::kCompound : nbt::kEnd, held);
        for (u32 i = 0; i < n; ++i) {
            if (stacks[i].empty()) continue;
            w.put_byte("Slot", static_cast<i8>(i));
            item::stack_to_nbt(stacks[i], w);
            w.end_compound();
        }
    };
    // What is kept beside it, each entry where vanilla's code puts it.
    const u32 keep = chest_kind(kind) ? chests_[ref->data].keep : furnaces_[ref->data].keep;
    nbt::Doc kept_doc;
    nbt::Tag kept{};
    if (keep != kNone && kept_doc.parse(opaque_[keep], nbt::Form::kNameless, nbt::kTrusted) == nbt::Error::kNone)
        kept = kept_doc.root();
    const auto kept_as_is = [&](std::string_view name) {
        const nbt::Tag e = kept.get(name);
        if (e) w.put_raw(name, e.type, e.bytes());
        return static_cast<bool>(e);
    };
    kept_as_is("lock");  // BaseContainerBlockEntity.saveAdditional: lockKey.addToTag, CustomName
    kept_as_is("CustomName");
    if (chest_kind(kind)) {
        items(chests_[ref->data].items, Chest::kSlots);
    } else {  // AbstractFurnaceBlockEntity.saveAdditional
        const Furnace& f = furnaces_[ref->data];
        w.put_int("cooking_time_spent", f.cook);
        w.put_int("cooking_total_time", f.cook_total);
        w.put_int("lit_time_remaining", f.lit_remaining);
        w.put_int("lit_total_time", f.lit_total);
        w.put_float("speed_multiplier", f.speed);
        items(f.items, Furnace::kSlots);
        if (!kept_as_is("RecipesUsed")) {  // recipesUsed: a map that is always written
            w.begin_compound("RecipesUsed");
            w.end_compound();
        }
    }
    if (!kept_as_is("components")) {  // BlockEntity.components: a map that is always written
        w.begin_compound("components");
        w.end_compound();
    }
    w.put_string("id", world::block_entity_type_name(ref->type));  // saveMetadata
    w.put_int("x", x);
    w.put_int("y", y);
    w.put_int("z", z);
    w.end_compound();
    return Saved::kOk;
}

// ---------------------------------------------------------------------------- entities

Game::Saved Game::save_entity(u32 row, base::ByteBuf& out) {
    const u32 slot = ents_.slot[row], type = ents_.type[row];
    const bool is_player = row < players_.size();
    const phys::Body* body = slot < movers_.size() && movers_[slot].kind != gp::kMoverNone ? &movers_[slot].body : nullptr;
    nbt::Writer w(out);
    w.root(nbt::kCompound);
    // ---- Entity.saveWithoutId
    w.begin_list("Pos", nbt::kDouble, 3);
    w.put_double(ents_.x[row]);
    w.put_double(ents_.y[row]);
    w.put_double(ents_.z[row]);
    w.begin_list("Motion", nbt::kDouble, 3);
    w.put_double(ents_.vx[row]);
    w.put_double(ents_.vy[row]);
    w.put_double(ents_.vz[row]);
    w.begin_list("Rotation", nbt::kFloat, 2);
    w.put_float(ents_.yaw[row]);
    w.put_float(ents_.pitch[row]);
    w.put_double("fall_distance", body ? body->fall_distance : 0.0);
    w.put_short("Fire", is_player ? -20 : 0);  // remainingFireTicks: a player's rest at -getFireImmuneTicks()
    w.put_short("Air", 300);
    w.put_byte("OnGround", (ents_.flags[row] & entity::flag::kOnGround) != 0);
    w.put_byte("Invulnerable", 0);
    w.put_int("PortalCooldown", 0);
    put_uuid(w, "UUID", ents_.uuid[row]);
    // (What stands still because nothing here moves it has the flag too; that is not vanilla's NoGravity.)
    if ((ents_.flags[row] & entity::flag::kNoGravity) && body) w.put_byte("NoGravity", 1);

    const gen::entity_type::Info& info = gen::entity_type::kInfo[type];
    const bool is_item = slot < item_of_slot_.size() && item_of_slot_[slot] != kNone;
    const bool is_falling = slot < falling_.size() && falling_[slot].alive;
    const auto stack = [&](std::string_view name, const ItemStack& s) {
        w.begin_compound(name);
        item::stack_to_nbt(s, w);
        w.end_compound();
    };
    if (info.living) {  // ---- LivingEntity.addAdditionalSaveData
        w.put_float("Health", info.max_health);
        w.put_short("HurtTime", 0);
        w.put_short("DeathTime", 0);
        w.put_float("AbsorptionAmount", 0.0f);
        w.put_int("current_impulse_context_reset_grace_time", 0);
        // AttributeMap.pack: the attributes that were asked for by now. Of a fresh entity those
        // are its speed and - a Mob's, Mob.finalizeSpawn - how far it follows, with the bonus
        // every mob is given at random; of a player in creative mode, these four.
        const auto attribute = [&](std::string_view id, double base) {
            w.put_string("id", id);
            w.put_double("base", base);
        };
        if (is_player) {
            w.begin_list("attributes", nbt::kCompound, 4);
            attribute("minecraft:waypoint_transmit_range", 6.0E7);
            w.end_compound();
            attribute("minecraft:movement_speed", info.movement_speed);
            w.end_compound();
            attribute("minecraft:entity_interaction_range", 3.0);
            w.end_compound();
            attribute("minecraft:block_interaction_range", 4.5);
            w.end_compound();
        } else {
            w.begin_list("attributes", nbt::kCompound, info.mob ? 2 : 1);
            attribute("minecraft:movement_speed", info.movement_speed);
            w.end_compound();
            if (info.mob) {
                attribute("minecraft:follow_range", info.follow_range);
                // RandomSource.triangle(0, 0.11485): nothing here follows anything, so the bonus
                // is not kept - it is made of the entity's UUID, the same every time it is asked.
                const Uuid id = ents_.uuid[row];
                const auto unit = [](u64 x) {  // splitmix64, to [0, 1)
                    x += 0x9E3779B97F4A7C15ull;
                    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
                    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
                    return static_cast<double>((x ^ (x >> 31)) >> 11) * 0x1.0p-53;
                };
                w.begin_list("modifiers", nbt::kCompound, 1);
                w.put_string("id", "minecraft:random_spawn_bonus");
                w.put_double("amount", 0.11485000000000001 * (unit(id.hi) - unit(id.lo)));
                w.put_string("operation", "add_multiplied_base");
                w.end_compound();
                w.end_compound();
            }
        }
        w.put_byte("FallFlying", 0);
        w.begin_compound("Brain");
        w.begin_compound("memories");
        w.end_compound();
        w.end_compound();
        if (is_player) {  // EntityEquipment.CODEC: what is worn, by the slot's name; a player's is always written
            const Inventory& inv = players_[row].menu.inv;
            static constexpr struct {
                u32 index;
                std::string_view name;
            } kWorn[] = {{inv::kOffhand, "offhand"},
                         {inv::kFeet, "feet"},
                         {inv::kLegs, "legs"},
                         {inv::kChest, "chest"},
                         {inv::kHead, "head"}};
            w.begin_compound("equipment");
            for (const auto& e : kWorn)
                if (!inv.items[e.index].empty()) stack(e.name, inv.items[e.index]);
            w.end_compound();
        }
    }
    if (info.mob) {  // ---- Mob
        w.put_byte("CanPickUpLoot", 0);
        w.put_byte("PersistenceRequired", 0);
        w.put_byte("LeftHanded", 0);
    }
    if (info.ageable) {  // ---- AgeableMob
        w.put_int("Age", 0);
        w.put_int("ForcedAge", 0);
        w.put_byte("AgeLocked", 0);
    }
    if (is_item) {  // ---- ItemEntity
        const u32 k = item_of_slot_[slot];
        const u64 age = std::min<u64>(tick_ - item_age_from_[k], 32766);
        const u64 delay = item_pickup_[k] == kNever ? record::kNeverPickUp
                          : item_pickup_[k] > tick_ ? std::min<u64>(item_pickup_[k] - tick_, 32766)
                                                    : 0;
        w.put_short("Health", item_health_[k]);
        w.put_short("Age", static_cast<i16>(age));
        w.put_short("PickupDelay", static_cast<i16>(delay));
        if (!ents_.item[row].empty()) stack("Item", ents_.item[row]);
    } else if (is_falling) {  // ---- FallingBlockEntity
        const Falling& f = falling_[slot];
        put_block_state(w, "BlockState", ents_.block_state[row]);
        w.put_int("Time", f.time);
        w.put_byte("DropItem", f.drop_item);
        w.put_byte("HurtEntities", 0);
        w.put_float("FallHurtAmount", 0.0f);
        w.put_int("FallHurtMax", 40);
        w.put_byte("CancelDrop", f.cancel_drop);
    } else if (type == gen::entity_type::kTnt) {  // ---- PrimedTnt
        w.put_short("fuse", static_cast<i16>(ents_.fuse[row]));
        put_block_state(w, "block_state", gen::block::kTnt);
    } else if (type == gen::entity_type::kArmorStand) {  // ---- ArmorStand
        w.put_byte("Invisible", 0);
        w.put_byte("Small", 0);
        w.put_byte("ShowArms", 0);
        w.put_int("DisabledSlots", 0);
        w.put_byte("NoBasePlate", 0);
        w.begin_compound("Pose");
        w.end_compound();
    }
    if (is_player) {
        // ---- Player, ServerPlayer: what of theirs there is here - the inventory, the hand, the
        // abilities of creative mode, the dimension.
        const Player& p = players_[row];
        const Inventory& inv = p.menu.inv;
        u32 held = 0;
        for (u32 i = 0; i < inv::kItems; ++i) held += !inv.items[i].empty();
        w.begin_list("Inventory", held ? nbt::kCompound : nbt::kEnd, held);
        for (u32 i = 0; i < inv::kItems; ++i) {
            if (inv.items[i].empty()) continue;
            w.put_byte("Slot", static_cast<i8>(i));
            item::stack_to_nbt(inv.items[i], w);
            w.end_compound();
        }
        w.put_int("SelectedItemSlot", inv.selected);
        w.begin_compound("abilities");  // Abilities.Packed
        w.put_byte("invulnerable", 1);
        w.put_byte("flying", p.flying);
        w.put_byte("mayfly", 1);
        w.put_byte("instabuild", 1);
        w.put_byte("mayBuild", 1);
        w.put_float("flySpeed", 0.05f);
        w.put_float("walkSpeed", 0.1f);
        w.end_compound();
        w.put_int("playerGameType", 1);
        w.put_string("Dimension", "minecraft:overworld");
        // NbtPredicate.getEntityTagToCompare
        if (!inv.selected_item().empty()) stack("SelectedItem", inv.selected_item());
    }
    w.end_compound();
    return Saved::kOk;
}

}  // namespace server
