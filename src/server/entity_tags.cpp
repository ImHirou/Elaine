// SPDX-License-Identifier: GPL-3.0-or-later
// An entity out of a tag: `/summon pig ~ ~ ~ {CustomName:"Bob",NoAI:1b}`, and what a chunk kept
// of an entity. The keys are those of Entity.load and of
// readAdditionalSaveData of LivingEntity, Mob, AgeableMob, ItemEntity, PrimedTnt,
// FallingBlockEntity and ArmorStand (26.3); what a class below those saves (a pig's variant, a
// villager's trades) is not read, and is nobody's here.
//
// What a tag says that every entity has a place for goes there - its motion into the table,
// its fuse into the fuse. The rest - a name, tags, whether it is heard, an armor stand's pose -
// is cold (entity::Cold): few entities have any, and it is read when one is shown or saved.
//
// A tag is read whole before anything is made of it: what this server cannot do - a baby, whose
// size the physics does not know; a small armor stand or a marker; a text it does not read - is
// refused in words of our own, and no entity appears.
#include <algorithm>
#include <cmath>

#include "item/codec.hpp"
#include "nbt/nbt.hpp"
#include "nbt/ops.hpp"
#include "server/game.hpp"
#include "text/component.hpp"

#include PROTO_GEN_PHYSICS_HPP

namespace server {

using namespace proto;
using world::BlockState;
namespace entity_type = proto::gen::entity_type;
namespace cold = entity::cold;
namespace equip = item::equip;

namespace {

// EquipmentSlot.getName, by ordinal.
constexpr std::string_view kSlotNames[equip::kCount] = {"mainhand", "offhand", "feet", "legs", "chest", "head", "body", "saddle"};
// ArmorStand.ArmorStandPose.CODEC's fields, in the order of the six rotations a client is sent.
constexpr std::string_view kPoseNames[6] = {"Head", "Body", "LeftArm", "RightArm", "LeftLeg", "RightLeg"};

// ArmorStand.DEFAULT_*_POSE, in that order.
constexpr float kDefaultPose[6][3] = {{0, 0, 0}, {0, 0, 0}, {-10, 0, -10}, {-15, 0, 10}, {-1, 0, -1}, {1, 0, 1}};

bool flag(nbt::Tag tag, std::string_view name) {  // getBooleanOr(name, false): a number that is not 0
    const nbt::Tag t = tag.get(name);
    return t.numeric() && t.as_int() != 0;
}
i32 number(nbt::Tag tag, std::string_view name, i32 fallback) {  // getIntOr / getShortOr
    const nbt::Tag t = tag.get(name);
    return t.numeric() ? t.as_int() : fallback;
}
// A list of exactly `n` numbers (Vec3.CODEC, Vec2.CODEC, Rotations.CODEC).
bool numbers(nbt::Tag t, u32 n, double* out) {
    if (!t.collection() || t.size() != n) return false;
    u32 i = 0;
    for (const nbt::Tag e : t.elements()) {
        if (!e.numeric()) return false;
        out[i++] = e.as_double();
    }
    return true;
}

}  // namespace

// Everything a tag can say of an entity of `type`, read: false and `why` where it asks for what
// this server cannot do.
bool Game::read_entity_tag(u32 type, nbt::Tag tag, EntityTag& out, std::string& why) {
    const entity_type::Info& info = entity_type::kInfo[type];
    if (tag.type != nbt::kCompound) return true;
    entity::Cold& c = out.cold;
    // ---- Entity.load
    double v[3];
    if (numbers(tag.get("Motion"), 3, v)) {  // more than ten blocks a tick is none
        for (u32 i = 0; i < 3; ++i) out.motion[i] = std::fabs(v[i]) <= 10.0 ? v[i] : 0.0;
        out.has_motion = true;
    }
    if (numbers(tag.get("Rotation"), 2, v)) {
        out.yaw = static_cast<float>(v[0]), out.pitch = static_cast<float>(v[1]);
        out.has_rotation = std::isfinite(out.yaw) && std::isfinite(out.pitch);
    }
    if (const nbt::Tag t = tag.get("fall_distance"); t.numeric()) out.fall_distance = t.as_double();
    out.on_ground = flag(tag, "OnGround");
    out.no_gravity = flag(tag, "NoGravity");
    out.glowing = flag(tag, "Glowing");
    c.fire = static_cast<i16>(number(tag, "Fire", 0));
    c.air = number(tag, "Air", 300);
    c.portal_cooldown = number(tag, "PortalCooldown", 0);
    c.ticks_frozen = number(tag, "TicksFrozen", 0);
    if (flag(tag, "Invulnerable")) c.bits |= cold::kInvulnerable;
    if (flag(tag, "CustomNameVisible")) c.bits |= cold::kNameVisible;
    if (flag(tag, "Silent")) c.bits |= cold::kSilent;
    if (const nbt::Tag name = tag.get("CustomName")) {
        base::ByteBuf text;
        const text::Result r = text::transcode(name, text);
        if (r == text::Result::kUnsupported) {
            why = "Names with parts of a text this server does not read";
            return false;
        }
        if (r == text::Result::kOk) c.name.assign(text.data, text.data + text.len);
    }
    if (const nbt::Tag tags = tag.get("Tags"); tags.type == nbt::kList && tags.size() <= 1024) {  // TAG_LIST_CODEC
        bool strings = true;
        for (const nbt::Tag t : tags.elements()) strings &= t.type == nbt::kString;
        if (strings)
            for (const nbt::Tag t : tags.elements()) {
                std::string s = nbt::to_utf8(t.str());
                if (std::find(c.tags.begin(), c.tags.end(), s) == c.tags.end()) c.tags.push_back(std::move(s));
            }
    }
    if (const nbt::Tag data = tag.get("data"); data.type == nbt::kCompound && data.size()) {  // CustomData.CODEC
        c.data.push_back(nbt::kCompound);
        c.data.insert(c.data.end(), data.bytes().begin(), data.bytes().end());
    }
    // ---- LivingEntity, Mob, AgeableMob
    if (info.living) {
        if (const nbt::Tag health = tag.get("Health"); health.numeric())  // setHealth: clamped to what it may have
            c.health = std::clamp(static_cast<float>(health.as_double()), 0.0f, info.max_health);
        if (const nbt::Tag worn = tag.get("equipment"); worn.type == nbt::kCompound) {  // EntityEquipment.CODEC
            for (u32 slot = 0; slot < equip::kCount; ++slot) {
                const nbt::Tag stack = worn.get(kSlotNames[slot]);
                if (!stack) continue;
                const item::Verdict r = item::stack_from_nbt(stack, c.worn[slot], true);
                if (r == item::Verdict::kUnsupported) {
                    why = "Items with components this server does not read";
                    return false;
                }
            }
        }
    }
    if (info.mob) {
        if (flag(tag, "NoAI")) c.bits |= cold::kNoAi;
        if (flag(tag, "PersistenceRequired")) c.bits |= cold::kPersistent;
        if (flag(tag, "LeftHanded")) c.bits |= cold::kLeftHanded;
        if (flag(tag, "CanPickUpLoot")) c.bits |= cold::kPickUpLoot;
    }
    if (info.ageable) {
        c.age = number(tag, "Age", 0);
        c.forced_age = number(tag, "ForcedAge", 0);
        if (flag(tag, "AgeLocked")) c.bits |= cold::kAgeLocked;
        if (c.age < 0) {  // a baby: half the size, which is another body to the physics
            why = "Mobs that are not grown up";
            return false;
        }
    }
    // ---- the kinds that are something of their own
    if (type == entity_type::kArmorStand) {
        out.invisible = flag(tag, "Invisible");
        if (flag(tag, "ShowArms")) c.bits |= cold::kShowArms;
        if (flag(tag, "NoBasePlate")) c.bits |= cold::kNoBasePlate;
        c.disabled_slots = number(tag, "DisabledSlots", 0);
        if (flag(tag, "Small") || flag(tag, "Marker")) {  // another box, or none at all
            why = "Armor stands that are small or markers";
            return false;
        }
        if (const nbt::Tag pose = tag.get("Pose"); pose.type == nbt::kCompound) {
            for (u32 i = 0; i < 6; ++i) {
                if (!numbers(pose.get(kPoseNames[i]), 3, v)) continue;
                // (A rotation that is the one a stand has anyway is no news to anybody.)
                if (static_cast<float>(v[0]) == kDefaultPose[i][0] && static_cast<float>(v[1]) == kDefaultPose[i][1] &&
                    static_cast<float>(v[2]) == kDefaultPose[i][2])
                    continue;
                for (u32 k = 0; k < 3; ++k) c.pose[i][k] = static_cast<float>(v[k]);
                c.posed |= static_cast<u8>(1u << i);
            }
        }
    } else if (type == entity_type::kItem) {
        if (const nbt::Tag stack = tag.get("Item")) {
            const item::Verdict r = item::stack_from_nbt(stack, out.item, true);
            if (r == item::Verdict::kUnsupported) {
                why = "Items with components this server does not read";
                return false;
            }
        }
        out.item_age = number(tag, "Age", 0);
        out.pickup_delay = number(tag, "PickupDelay", 0);
        out.item_health = number(tag, "Health", 5);
    } else if (type == entity_type::kTnt) {
        out.fuse = number(tag, "fuse", 80);
    } else if (type == entity_type::kFallingBlock) {
        // BlockState.CODEC: the block's name alone, or {Name, Properties}.
        const nbt::Tag state = tag.get("BlockState");
        const nbt::Tag name = state.type == nbt::kCompound ? state.get("Name") : state;
        if (name.type == nbt::kString) {
            std::string id = nbt::to_utf8(name.str());
            if (id.find(':') == std::string::npos) id.insert(0, "minecraft:");
            const u32 block = world::find_block(id);
            if (block != world::kNoId) {
                const world::BlockInfo& bi = world::block_info(block);
                BlockState s = bi.default_state;
                if (const nbt::Tag given = state.get("Properties"); given.type == nbt::kCompound) {
                    for (u32 i = 0, n = world::block_property_count(block); i < n; ++i) {
                        const world::BlockProperty p = world::block_property(block, i);
                        const nbt::Tag value = given.get(p.name);
                        if (value.type != nbt::kString) continue;
                        const auto it = std::find(p.values.begin(), p.values.end(), value.str());
                        if (it == p.values.end()) continue;
                        const i32 now = static_cast<i32>((s - bi.first_state) / p.stride % p.values.size());
                        s = static_cast<BlockState>(s +
                                                    (static_cast<i32>(it - p.values.begin()) - now) * static_cast<i32>(p.stride));
                    }
                }
                out.falling = s;
                out.has_falling = true;
            }
        }
        out.falling_time = number(tag, "Time", 0);
        out.drop_item = !tag.get("DropItem") || flag(tag, "DropItem");
        out.cancel_drop = flag(tag, "CancelDrop");
    }
    out.has_cold = !c.name.empty() || !c.data.empty() || !c.tags.empty() || c.health >= 0.0f || c.age || c.forced_age ||
                   c.air != 300 || c.portal_cooldown || c.ticks_frozen || c.disabled_slots || c.fire || c.bits || c.posed ||
                   std::any_of(std::begin(c.worn), std::end(c.worn), [](const ItemStack& s) { return !s.empty(); });
    return true;
}

// What a tag said, onto the entity that was made for it.
void Game::apply_entity_tag(u32 slot, EntityTag&& t) {
    const u32 row = ents_.by_slot(slot);
    if (t.has_motion) {
        ents_.vx[row] = t.motion[0], ents_.vy[row] = t.motion[1], ents_.vz[row] = t.motion[2];
        tracker_.created_with_velocity(row);
    }
    if (t.has_rotation) ents_.yaw[row] = t.yaw, ents_.pitch[row] = t.pitch, ents_.head_yaw[row] = t.yaw;  // setYHeadRot
    if (t.on_ground) ents_.flags[row] |= entity::flag::kOnGround;
    if (t.no_gravity) {  // ...and for what moves here, its body: Entity.getGravity is 0 for it
        ents_.set_no_gravity(row, true);
        if (slot < movers_.size() && movers_[slot].kind != proto::gen::physics::kMoverNone) movers_[slot].body.no_gravity = true;
    }
    if (t.glowing) ents_.set_shared_flag(row, 6, true);    // Entity.FLAG_GLOWING
    if (t.invisible) ents_.set_shared_flag(row, 5, true);  // FLAG_INVISIBLE
    if (slot < movers_.size() && movers_[slot].kind != proto::gen::physics::kMoverNone)
        movers_[slot].body.fall_distance = t.fall_distance;
    if (!t.has_cold) return;
    const bool worn = std::any_of(std::begin(t.cold.worn), std::end(t.cold.worn), [](const ItemStack& s) { return !s.empty(); });
    entity::Cold& c = ents_.make_cold(row);
    c = std::move(t.cold);
    // LivingEntity.detectEquipmentUpdates: what it wears, to whoever sees it (nobody yet: it is
    // part of how the entity appears).
    if (worn) tracker_.show_equipment(row, c.worn, out_.data());
    wake(slot);
}

// SummonCommand.createEntity with a tag: the entity `tag` describes, at this place, looking along
// `yaw` unless the tag says otherwise. kNone, and `why`, where the tag asks for what this server
// cannot do; nothing was made then.
u32 Game::summon(u32 type, double x, double y, double z, nbt::Tag tag, std::string& why, float yaw) {
    namespace gp = proto::gen::physics;
    EntityTag read;
    if (!read_entity_tag(type, tag, read, why)) return kNone;
    u32 slot;
    if (type == entity_type::kItem && !read.item.empty()) {
        const Uuid uuid{(random() & ~0xF000ull) | 0x4000ull, (random() & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull};
        const u64 delay = read.pickup_delay == 32767 ? kNever : static_cast<u64>(std::max(read.pickup_delay, 0));
        slot = add_item(uuid, yaw, std::move(read.item), x, y, z, 0, 0, 0, delay, static_cast<u64>(std::max(read.item_age, 0)),
                        static_cast<i8>(read.item_health));
    } else if (type == entity_type::kFallingBlock && read.has_falling) {
        const Uuid uuid{(random() & ~0xF000ull) | 0x4000ull, (random() & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull};
        slot = add_falling(uuid, read.falling, x, y, z, read.falling_time, false);
        falling_[slot].drop_item = read.drop_item;
        falling_[slot].cancel_drop = read.cancel_drop;
    } else {
        slot = summon(type, x, y, z, yaw);
        if (type == entity_type::kTnt) ents_.set_fuse(ents_.by_slot(slot), read.fuse);
    }
    apply_entity_tag(slot, std::move(read));
    return slot;
}

// What a tag said of an entity, as a tag again: the keys read_entity_tag reads of what is cold.
// For the chunk's bytes - and so in no order but this one.
void Game::write_cold_tag(u32 row, nbt::Writer& w) const {
    if (ents_.shared_flags[row] & (1u << 6)) w.put_byte("Glowing", 1);
    if (ents_.type[row] == entity_type::kArmorStand && (ents_.shared_flags[row] & (1u << 5))) w.put_byte("Invisible", 1);
    const entity::Cold* c = ents_.cold(row);
    if (!c) return;
    const entity_type::Info& info = entity_type::kInfo[ents_.type[row]];
    if (!c->name.empty()) w.put_raw("CustomName", static_cast<nbt::Type>(c->name[0]), {c->name.data() + 1, c->name.size() - 1});
    if (c->bits & cold::kNameVisible) w.put_byte("CustomNameVisible", 1);
    if (c->bits & cold::kSilent) w.put_byte("Silent", 1);
    if (c->bits & cold::kInvulnerable) w.put_byte("Invulnerable", 1);
    if (c->fire) w.put_short("Fire", c->fire);
    if (c->air != 300) w.put_int("Air", c->air);
    if (c->portal_cooldown) w.put_int("PortalCooldown", c->portal_cooldown);
    if (c->ticks_frozen) w.put_int("TicksFrozen", c->ticks_frozen);
    if (!c->tags.empty()) {
        w.begin_list("Tags", nbt::kString, static_cast<u32>(c->tags.size()));
        for (const std::string& t : c->tags) w.put_string(t);
    }
    if (!c->data.empty()) w.put_raw("data", nbt::kCompound, {c->data.data() + 1, c->data.size() - 1});
    if (info.living && c->health >= 0.0f) w.put_float("Health", c->health);
    if (std::any_of(std::begin(c->worn), std::end(c->worn), [](const ItemStack& s) { return !s.empty(); })) {
        w.begin_compound("equipment");
        for (u32 slot = 0; slot < equip::kCount; ++slot) {
            if (c->worn[slot].empty()) continue;
            w.begin_compound(kSlotNames[slot]);
            item::stack_to_nbt(c->worn[slot], w);
            w.end_compound();
        }
        w.end_compound();
    }
    if (c->bits & cold::kNoAi) w.put_byte("NoAI", 1);
    if (c->bits & cold::kPersistent) w.put_byte("PersistenceRequired", 1);
    if (c->bits & cold::kLeftHanded) w.put_byte("LeftHanded", 1);
    if (c->bits & cold::kPickUpLoot) w.put_byte("CanPickUpLoot", 1);
    if (c->age) w.put_int("Age", c->age);
    if (c->forced_age) w.put_int("ForcedAge", c->forced_age);
    if (c->bits & cold::kAgeLocked) w.put_byte("AgeLocked", 1);
    if (c->bits & cold::kShowArms) w.put_byte("ShowArms", 1);
    if (c->bits & cold::kNoBasePlate) w.put_byte("NoBasePlate", 1);
    if (c->disabled_slots) w.put_int("DisabledSlots", c->disabled_slots);
    if (c->posed) {
        w.begin_compound("Pose");
        for (u32 i = 0; i < 6; ++i) {
            if (!(c->posed & (1u << i))) continue;
            w.begin_list(kPoseNames[i], nbt::kFloat, 3);
            for (const float f : c->pose[i]) w.put_float(f);
        }
        w.end_compound();
    }
}

}  // namespace server
