// SPDX-License-Identifier: GPL-3.0-or-later
// What the game holds, as the tags vanilla saves it as: BlockEntity.saveWithFullMetadata and
// Entity.saveWithoutId (26.3) - for `/data get`, and for whatever compares a tag with one
// (`@e[nbt=...]`). Nothing here is what the server keeps on disk: that is server/persist.cpp, in a
// form of its own.
//
// Entries are written in the order vanilla's code puts them, because the text of `/data get`
// shows a compound in the order of a HashMap, and what that is depends on the order of arrival
// (nbt::map_order).
//
// Of an entity, what the server does not keep is written as the value vanilla gives a fresh one
// (Fire, Air, PortalCooldown...).
#include "item/codec.hpp"
#include "nbt/nbt.hpp"
#include "nbt/ops.hpp"
#include "server/game.hpp"
#include "server/persist.hpp"

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

// What follows `prefix` in a tag that was kept beside a block entity: the entries of the
// compound, as they are.
void put_kept(nbt::Writer& w, std::span<const u8> kept) {
    nbt::Doc doc;
    if (kept.empty() || doc.parse(kept, nbt::Form::kNameless, nbt::kTrusted) != nbt::Error::kNone) return;
    if (doc.root().type != nbt::kCompound) return;
    for (const nbt::Tag e : doc.root().entries()) w.put_raw(e.name(), e.type, e.bytes());
}

}  // namespace

// ---------------------------------------------------------------------------- block entities

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
    u32 keep = ~0u;
    if (chest_kind(kind)) {
        const Chest& c = chests_[ref->data];
        items(c.items, Chest::kSlots);
        keep = c.keep;
    } else {  // AbstractFurnaceBlockEntity.saveAdditional
        const Furnace& f = furnaces_[ref->data];
        w.put_int("cooking_time_spent", f.cook);
        w.put_int("cooking_total_time", f.cook_total);
        w.put_int("lit_time_remaining", f.lit_remaining);
        w.put_int("lit_total_time", f.lit_total);
        w.put_float("speed_multiplier", f.speed);
        items(f.items, Furnace::kSlots);
        keep = f.keep;
        if (keep == ~0u) {  // recipesUsed: a map that is always written
            w.begin_compound("RecipesUsed");
            w.end_compound();
        }
    }
    if (keep != ~0u) put_kept(w, opaque_[keep]);
    w.begin_compound("components");  // BlockEntity.components: nothing it holds beyond its own fields
    w.end_compound();
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
