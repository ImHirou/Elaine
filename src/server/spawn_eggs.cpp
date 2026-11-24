// SPDX-License-Identifier: GPL-3.0-or-later
// Spawn eggs: SpawnEggItem.useOn and use, and the EntityType.spawn / create they call (26.3).
//
// An egg names what it makes in its ENTITY_DATA. Every egg has one of its own ({id:
// "minecraft:pig"}); a stack may name another type, carry more of a tag, or have none - and then
// it makes nothing. What appears is what /summon makes of the same tag (server/entity_tags.cpp):
// a mob without a brain, that falls and is pushed. Vanilla's Mob.finalizeSpawn would draw its
// variant, its equipment and whether it is left-handed, and the mob would make its ambient sound
// as it appears; neither is done here (docs/architecture.md, "Spawn eggs").
//
// Not here: an egg on a spawner, whose block entity is the tag an imported world gave it; an
// egg on a mob of its own kind (a baby).
#include <cmath>

#include "base/log.hpp"
#include "item/codec.hpp"
#include "nbt/nbt.hpp"
#include "nbt/ops.hpp"
#include "server/game.hpp"

#include PROTO_GEN_STATIC_IDS_HPP

namespace server {

using namespace proto;
using world::BlockState;
namespace entity_type = proto::gen::entity_type;

// SpawnEggItem.getType: the type its ENTITY_DATA names - TypedEntityData.streamCodec, the type's
// id and then the rest of its tag. Without one it makes nothing.
Game::EggType Game::egg_type(const ItemStack& held) {
    EggType egg;
    const item::Patches::Found data = item::effective(held, item::component::kEntityData);
    if (data.state != item::Patches::State::kSet) return egg;
    Reader r(data.value);
    egg.type = r.varint();
    egg.rest = r.bytes(r.remaining());
    // EntityType.canSpawn: every type is enabled here, and there is no difficulty - so no
    // peaceful, where a monster's egg makes nothing. EntityType.create: what has no factory
    // (a player) makes nothing; here that is what /summon may not make.
    egg.ok = r.ok && world::summonable(egg.type);
    return egg;
}

// SpawnEggItem.useOn: kSuccess when something was made (the arm swings, as for a block placed).
// What vanilla answers FAIL to is kPass here: neither swings, and nothing follows either.
Game::Used Game::use_spawn_egg(u32 pi, const sb::UseItemOn& u, const ItemStack& held) {
    const EggType egg = egg_type(held);
    if (!egg.ok) return Used::kPass;

    const BlockPos pos = u.pos;
    const BlockState clicked = world_.block(pos.x, pos.y, pos.z);
    const u32 holds = world::block_info_of(clicked).block_entity;
    // A spawner would take the egg's type (BaseSpawner.setEntityId). Its block entity is the tag
    // an imported world gave it, which nothing here reads: nothing happens, rather than a mob
    // beside it.
    if (holds == gen::block_entity_type::kMobSpawner || holds == gen::block_entity_type::kTrialSpawner) return Used::kPass;

    // Into the clicked block if nothing in it stops a body (getCollisionShape is empty; a shape
    // that depends on who asks asks nobody here), else against the face that was clicked.
    bool dynamic = false;
    const bool into = world::collision_boxes(clicked, dynamic).empty();
    const BlockPos at = into ? pos
                             : BlockPos{pos.x + world::kStepX[u.direction], pos.y + world::kStepY[u.direction],
                                        pos.z + world::kStepZ[u.direction]};
    const bool moved_up = !into && u.direction == world::kUp;

    return hatch_egg(pi, held, egg, at, true, moved_up);
}

// SpawnEggItem.use: the egg used on no block. Where its holder looks - a source of fluid stops
// the look - and only if what is met is a liquid's own block (LiquidBlock: not a block that holds
// water), the egg's mob is made in that block, at its bottom: nothing is moved down or up.
Game::Used Game::use_spawn_egg(u32 pi, const ItemStack& held) {
    const world::ClipHit hit = pov_hit(pi, world::ClipFluid::kSourceOnly);
    if (!hit.hit) return Used::kPass;
    const EggType egg = egg_type(held);
    if (!egg.ok) return Used::kPass;
    if (world::block_info_of(world_.block(hit.x, hit.y, hit.z)).kind != gen::block_kind::kLiquid) return Used::kPass;
    // (Level.mayInteract, Player.mayUseItemAt: no spawn protection here, and everyone may build.)
    return hatch_egg(pi, held, egg, {hit.x, hit.y, hit.z}, false, false);
}

// SpawnEggItem.spawnMob -> EntityType.spawn / create.
Game::Used Game::hatch_egg(u32 pi, const ItemStack& held, const EggType& egg, BlockPos at, bool try_move_down, bool moved_up) {
    const u32 type = egg.type;
    const std::span<const u8> rest = egg.rest;
    // EntityType.create: set at the block's middle, one block up, it comes down on what that
    // block (and, moved up, the one under it) holds - if it is to come down at all; it looks a
    // way drawn at random.
    const entity_type::Info& info = entity_type::kInfo[type];
    double x = static_cast<double>(at.x) + 0.5, z = static_cast<double>(at.z) + 0.5;
    double y = static_cast<double>(at.y);
    if (try_move_down) {
        const world::Aabb box = world::entity_box(x, static_cast<double>(at.y + 1), z, info.width, info.height);
        y = static_cast<double>(at.y) + world::spawn_offset(world_, at.x, at.y, at.z, moved_up, box, phys_scratch_.collide);
    }
    const float yaw = wrap_degrees(random_float() * 360.0f);

    // Its tag (updateCustomEntityTag -> TypedEntityData.loadInto): what the new entity would
    // save, the egg's tag merged over it, loaded. Of what a fresh one saves, all that is not
    // what read_entity_tag assumes anyway is what the stack gave it
    // (applyComponentsFromItemStack: CUSTOM_NAME, CUSTOM_DATA). The egg's own tag counts for
    // an operator only on the types vanilla keeps to them (EntityType.onlyOpCanSetNbt).
    base::ByteBuf given;
    {
        nbt::Writer w(given);
        w.root(nbt::kCompound);
        const item::Patches::Found name = item::effective(held, item::component::kCustomName);
        if (name.state == item::Patches::State::kSet && !name.value.empty())
            w.put_raw("CustomName", static_cast<nbt::Type>(name.value[0]), name.value.subspan(1));
        const item::Patches::Found custom = item::effective(held, item::component::kCustomData);
        if (custom.state == item::Patches::State::kSet && !custom.value.empty())
            w.put_raw("data", nbt::kCompound, custom.value.subspan(1));
        w.end_compound();
    }
    nbt::Doc base_doc, egg_doc, merged_doc;
    if (base_doc.parse({given.data, given.len}, nbt::Form::kNameless, nbt::kTrusted) != nbt::Error::kNone) return Used::kPass;
    nbt::Tag tag = base_doc.root();
    base::ByteBuf merged;
    if (!rest.empty() && (!info.op_only_data || players_[pi].level > 0) &&
        egg_doc.parse(rest, nbt::Form::kNameless, nbt::kTrusted) == nbt::Error::kNone && egg_doc.root().type == nbt::kCompound &&
        egg_doc.root().size() > 0) {
        nbt::Writer(merged).root(nbt::kCompound);
        nbt::merge(tag, egg_doc.root(), merged);
        if (merged_doc.parse({merged.data, merged.len}, nbt::Form::kNameless, nbt::kTrusted) != nbt::Error::kNone)
            return Used::kPass;
        tag = merged_doc.root();
        // Entity.load takes Pos from it too: the egg's tag may put what it makes elsewhere.
        // (One that names no place, or no finite one, vanilla answers with (0, 0, 0) or an
        // exception; nothing is made of it here.)
        if (const nbt::Tag p = tag.get("Pos")) {
            double v[3];
            u32 n = 0;
            if (p.collection() && p.size() == 3)
                for (const nbt::Tag e : p.elements())
                    if (e.numeric() && std::isfinite(v[n] = e.as_double())) ++n;
            if (n != 3) {
                LOG_DEBUG("%s: a spawn egg whose tag puts what it makes nowhere", players_[pi].name.c_str());
                return Used::kPass;
            }
            x = std::clamp(v[0], -3.0000512E7, 3.0000512E7), y = std::clamp(v[1], -2.0E7, 2.0E7);
            z = std::clamp(v[2], -3.0000512E7, 3.0000512E7);
        }
    }
    if (ents_.size() >= entity::kMaxEntities) return Used::kPass;
    std::string why;
    if (summon(type, x, y, z, tag, why, yaw) == kNone) {
        LOG_DEBUG("%s: a spawn egg of what this server cannot make: %s", players_[pi].name.c_str(), why.c_str());
        return Used::kPass;
    }
    // (Level.gameEvent ENTITY_PLACE: there is nothing that listens. ItemStack.consume: a
    // creative player keeps the egg.)
    return Used::kSuccess;
}

}  // namespace server
