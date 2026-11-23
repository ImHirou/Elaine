// SPDX-License-Identifier: GPL-3.0-or-later
// What a chunk holds of the game's, as bytes: its block entities and the entities that stand in
// it. world::Dimension stores the two parts with the chunk and hands them back when the chunk
// returns (world/chunk_io.hpp); what is in them is decided here.
//
// Written with the wire's own primitives (big-endian, VarInts) and read with proto::Reader:
//
//   block entities   u8 version (3; 2 is the same without kind 4, 1 without kind 3), VarInt count, then per block entity
//                      u8 xz (x << 4 | z in the chunk), i16 y, VarInt block_entity_type, u8 kind
//                      kind 0 kept:    VarInt n + n bytes - the tag it came with (an import), as
//                                      nbt::Doc::write left it. Listed for clients, never opened
//                      kind 1 chest:   27 stacks, then a kept tag (VarInt n + bytes, n = 0: none).
//                                      A barrel's too: which it is, its block says
//                      kind 2 furnace: 3 stacks, i32 lit remaining, lit total, cooking, cooking
//                                      total, then a kept tag (version 1: its fuel's speed was 1)
//                      kind 3 furnace: the same with a float after the four numbers: the speed
//                                      of the fuel that was lit last. Of a furnace, a smoker or
//                                      a blast furnace: which it is, its block says
//                      kind 4 sign:    u8 flags (1 waxed, 2 allow_op_features), its front text and its
//                                      back text (VarInt n + n bytes each: SignText.STREAM_CODEC, n = 0
//                                      for an empty side), then a kept tag. Of any of the four signs
//   entities         u8 version (3; 2 has no tag behind a record, 1 no kind 4 either), then until the end
//                      u8 kind, VarInt entity type, uuid, 3 doubles position, 3 doubles velocity,
//                      float yaw, pitch, head yaw, u8 flags (1 on the ground, 2 no gravity),
//                      double fall distance
//                      kind 1 item:    a stack, u16 age, u16 pickup delay (32767: never), i8 health
//                      kind 2 tnt:     i16 fuse
//                      kind 3 other:   nothing more
//                      kind 4 falling: VarInt block state, i32 time, u8 flags (1 DropItem, 2 CancelDrop)
//   a stack          as a client sends one (ItemStack.OPTIONAL_UNTRUSTED_STREAM_CODEC), and read
//                    by the same reader: a file is trusted no further than a client.
//
// What is kept of an entity is what vanilla keeps (Entity.saveWithoutId, ItemEntity,
// PrimedTnt / FallingBlockEntity.addAdditionalSaveData): where it is and how it moves, how far it
// has fallen, whether it stands; an item's age, pickup delay and health; a fuse; the block that
// is falling, for how long, and whether it drops (not where it began: vanilla does not either). What a tick works out again is
// not: the block it stands on, what it ran into, its place in the round of an item at rest. An entity comes back as vanilla's
// does - a new entity with its old state, ticked in full until it rests. Ids of items, types and components are this version's
// (see chunk_io.hpp).
#include <algorithm>
#include <cmath>

#include "base/log.hpp"
#include "server/game.hpp"
#include "server/persist.hpp"

#include PROTO_GEN_PHYSICS_HPP
#include PROTO_GEN_STATIC_IDS_HPP

namespace server {

using namespace proto;
namespace block_kind = proto::gen::block_kind;
namespace gp = proto::gen::physics;

namespace {

using namespace record;

u64 chunk_key(i32 cx, i32 cz) { return (static_cast<u64>(static_cast<u32>(cx)) << 32) | static_cast<u32>(cz); }

}  // namespace

// ---------------------------------------------------------------------------- block entities

u32 Game::new_chest(BlockPos pos) {
    u32 data;
    if (!chest_free_.empty()) {
        data = chest_free_.back();
        chest_free_.pop_back();
    } else {
        data = static_cast<u32>(chests_.size());
        chests_.emplace_back();
    }
    chests_[data].alive = true;
    chests_[data].x = pos.x, chests_[data].y = pos.y, chests_[data].z = pos.z;
    return data;
}

item::Cooker Game::cooker_of(u32 block_entity_type) {
    return block_entity_type == gen::block_entity_type::kSmoker         ? item::Cooker::kSmoking
           : block_entity_type == gen::block_entity_type::kBlastFurnace ? item::Cooker::kBlasting
                                                                        : item::Cooker::kSmelting;
}

u32 Game::new_furnace(BlockPos pos, item::Cooker type) {
    u32 data;
    if (!furnace_free_.empty()) {
        data = furnace_free_.back();
        furnace_free_.pop_back();
    } else {
        data = static_cast<u32>(furnaces_.size());
        furnaces_.emplace_back();
    }
    furnaces_[data].alive = true;
    furnaces_[data].x = pos.x, furnaces_[data].y = pos.y, furnaces_[data].z = pos.z;
    furnaces_[data].type = type;
    return data;
}

// The entry of a block entity that is gone goes back to its store. `kind`: its block's
// (block_kind), or anything for a kept one.
void Game::free_block_entity(u32 kind, u32 data) {
    const auto free_kept = [&](u32 index) {
        if (index == kNone) return;
        opaque_[index] = {};
        opaque_free_.push_back(index);
    };
    if (data & kOpaqueBlockEntity) return free_kept(data & ~kOpaqueBlockEntity);
    if (chest_kind(kind)) {
        free_kept(chests_[data].keep);
        chests_[data] = {};
        chest_free_.push_back(data);
    } else if (kind == block_kind::kFurnace) {
        free_kept(furnaces_[data].keep);
        furnaces_[data] = {};
        furnace_free_.push_back(data);
    } else if (sign_kind(kind)) {
        free_kept(signs_[data].keep);
        signs_[data] = {};  // (tick_signs finds it dead, if somebody was writing on it)
        sign_free_.push_back(data);
    }
}

u32 Game::keep_tag(std::span<const u8> tag) {
    if (tag.empty()) return kNone;
    u32 index;
    if (!opaque_free_.empty()) {
        index = opaque_free_.back();
        opaque_free_.pop_back();
    } else {
        index = static_cast<u32>(opaque_.size());
        opaque_.emplace_back();
    }
    opaque_[index].assign(tag.begin(), tag.end());
    return index;
}

void Game::drop_kept(u32& keep) {
    if (keep == kNone) return;
    opaque_[keep] = {};
    opaque_free_.push_back(keep);
    keep = kNone;
}

void Game::write_block_entities(const world::Chunk& c, base::ByteBuf& out) const {
    if (c.block_entity_count == 0) return;
    put_block_entities_head(out, c.block_entity_count);
    const auto kept = [&](u32 index) {
        put_counted(out, index == kNone ? std::span<const u8>{} : std::span<const u8>{opaque_[index]});
    };
    for (u32 i = 0; i < c.block_entity_count; ++i) {
        const world::BlockEntityRef& be = c.block_entities[i];
        const BlockPos pos{c.x * 16 + (be.xz >> 4), be.y, c.z * 16 + (be.xz & 15)};
        const u32 kind = world::block_info_of(world_.block(pos.x, pos.y, pos.z)).kind;
        const bool opaque = (be.data & kOpaqueBlockEntity) != 0;
        put_block_entity(out, be.xz, be.y, be.type,
                         opaque             ? kKept
                         : chest_kind(kind) ? kChest
                         : sign_kind(kind)  ? kSign
                                            : kFurnace);
        if (opaque) {
            kept(be.data & ~kOpaqueBlockEntity);
        } else if (sign_kind(kind)) {
            const Sign& sign = signs_[be.data];
            *out.reserve(1) = static_cast<u8>((sign.waxed ? kSignWaxed : 0) | (sign.allow_op ? kSignAllowsOp : 0));
            ++out.len;
            put_counted(out, sign.text[0]);
            put_counted(out, sign.text[1]);
            kept(sign.keep);
        } else if (chest_kind(kind)) {
            const Chest& chest = chests_[be.data];
            for (const ItemStack& stack : chest.items) item::write_untrusted(out, stack);
            kept(chest.keep);
        } else {
            const Furnace& f = furnaces_[be.data];
            for (const ItemStack& stack : f.items) item::write_untrusted(out, stack);
            u8* p = out.reserve(20);
            for (u32 d = 0; d < Furnace::kDataCount; ++d) p = put_be32(p, static_cast<u32>(f.data(d)));
            out.commit(put_f32(p, f.speed));
            kept(f.keep);
        }
    }
}

void Game::read_block_entities(world::Chunk& c, std::span<const u8> bytes) {
    if (bytes.empty()) return;
    Reader r(bytes.data(), bytes.size());
    const u8 version = r.u8v();
    const u32 count = r.varint();
    if (!r.ok || version < 1 || version > kVersion) {
        LOG_ERROR("chunk %d, %d: its block entities are of a version this server does not read", c.x, c.z);
        return;
    }
    const auto keep = [&](std::span<const u8> tag) { return keep_tag(tag); };
    const auto stacks = [&](ItemStack* to, u32 n) {
        for (u32 i = 0; i < n; ++i) {
            // A stack this server would not take from a client it does not take from a file: the slot is empty.
            if (item::read_untrusted(r, to[i]) != item::Verdict::kOk) to[i] = {};
        }
    };
    for (u32 i = 0; i < count && r.ok; ++i) {
        const u8 xz = r.u8v();
        const i32 y = static_cast<i16>(r.be16());
        const u32 type = r.varint();
        const u8 record = r.u8v();
        if (!r.ok) break;
        const BlockPos pos{c.x * 16 + (xz >> 4), y, c.z * 16 + (xz & 15)};
        const world::BlockState state = world_.block(pos.x, pos.y, pos.z);
        const u32 kind = world::block_info_of(state).kind;
        // A block entity stands where a block has one, once (LevelChunk.setBlockEntity checks the same).
        const bool fits = (world::flags_of(state) & world::state::kBlockEntity) && !world_.block_entity(pos.x, pos.y, pos.z) &&
                          y >= world_.min_y() && y <= world_.max_y() && type < gen::block_entity_type::kCount;
        if (record == kKept) {
            const std::span<const u8> tag = r.bytes(r.varint());
            // What an import could not make ours on a worker (a chest with a name, a stack with
            // components) is made ours here, where the item codecs may be used - if it can be.
            if (r.ok && fits && !adopt_kept_block_entity(pos, state, type, tag))
                world_.add_block_entity(pos.x, pos.y, pos.z, type, keep(tag) | kOpaqueBlockEntity);
        } else if (record == kChest) {
            Chest chest;
            stacks(chest.items, Chest::kSlots);
            const std::span<const u8> tag = r.bytes(r.varint());
            if (!r.ok || !fits || !chest_kind(kind)) continue;
            const u32 data = new_chest(pos);
            for (u32 s = 0; s < Chest::kSlots; ++s) chests_[data].items[s] = std::move(chest.items[s]);
            chests_[data].keep = keep(tag);
            world_.add_block_entity(pos.x, pos.y, pos.z, type, data);
        } else if (record == kFurnace || record == kFurnaceV1) {
            Furnace f;
            stacks(f.items, Furnace::kSlots);
            f.lit_remaining = static_cast<i32>(r.be32());
            f.lit_total = static_cast<i32>(r.be32());
            f.cook = static_cast<i32>(r.be32());
            f.cook_total = static_cast<i32>(r.be32());
            if (record == kFurnace) f.speed = r.f32();
            const std::span<const u8> tag = r.bytes(r.varint());
            if (!r.ok || !fits || kind != block_kind::kFurnace) continue;
            const u32 data = new_furnace(pos, cooker_of(world::block_info_of(state).block_entity));
            Furnace& to = furnaces_[data];
            for (u32 s = 0; s < Furnace::kSlots; ++s) to.items[s] = std::move(f.items[s]);
            to.lit_remaining = f.lit_remaining, to.lit_total = f.lit_total, to.cook = f.cook, to.cook_total = f.cook_total;
            to.speed = f.speed;
            to.keep = keep(tag);
            world_.add_block_entity(pos.x, pos.y, pos.z, type, data);
        } else if (record == kSign) {
            const u8 flags = r.u8v();
            const std::span<const u8> front = r.bytes(r.varint()), back = r.bytes(r.varint());
            const std::span<const u8> tag = r.bytes(r.varint());
            if (!r.ok || !fits || !sign_kind(kind)) continue;
            // A text is read by the reader a client's is read by, or the side is empty.
            base::ByteBuf checked[2];
            const std::span<const u8> sides[2] = {front, back};
            const u32 data = new_sign(pos);
            for (u32 side = 0; side < 2; ++side) {
                if (sides[side].empty() ||
                    item::check_value(item::component::kSignTextFront, sides[side], checked[side]) != item::Verdict::kOk)
                    continue;
                signs_[data].text[side].assign(checked[side].data, checked[side].data + checked[side].len);
            }
            signs_[data].waxed = (flags & kSignWaxed) != 0;
            signs_[data].allow_op = (flags & kSignAllowsOp) != 0;
            signs_[data].keep = keep(tag);
            world_.add_block_entity(pos.x, pos.y, pos.z, type, data);
            show_sign(data);  // its text, for the chunk's packet
        } else {
            r.ok = false;
        }
    }
    if (!r.ok) LOG_ERROR("chunk %d, %d: its block entities end too early; the rest is lost", c.x, c.z);
}

// The chunk leaves the world: what its block entities hold has been written; their entries go.
void Game::drop_block_entities(world::Chunk& c) {
    for (u32 i = 0; i < c.block_entity_count; ++i) {
        const world::BlockEntityRef& be = c.block_entities[i];
        const u32 kind = world::block_info_of(world_.block(c.x * 16 + (be.xz >> 4), be.y, c.z * 16 + (be.xz & 15))).kind;
        if (!(be.data & kOpaqueBlockEntity) && (chest_kind(kind) || kind == block_kind::kFurnace)) {
            // Nobody can have it open - a menu closes four blocks away, a chunk leaves far beyond
            // every view - but an entry must not go while a menu names it.
            close_menus_on(kind, be.data);
        }
        free_block_entity(kind, be.data);
    }
    c.block_entity_count = 0;
}

// ---------------------------------------------------------------------------- entities

// The entities that stand in one chunk (entities_at_, sorted, has them): written, and - when the
// chunk leaves - taken out of the world by the caller.
void Game::write_entities(u64 chunk, base::ByteBuf& out) {
    const auto first = std::lower_bound(entities_at_.begin(), entities_at_.end(), chunk,
                                        [](const EntityAt& e, u64 key) { return e.chunk < key; });
    if (first == entities_at_.end() || first->chunk != chunk) return;
    out.commit(put_u8(out.reserve(1), kEntitiesVersion));
    for (auto it = first; it != entities_at_.end() && it->chunk == chunk; ++it) {
        const u32 slot = it->slot, row = ents_.by_slot(slot);
        const u32 type = ents_.type[row];
        const bool is_item = slot < item_of_slot_.size() && item_of_slot_[slot] != kNone;
        const bool is_falling = slot < falling_.size() && falling_[slot].alive;
        const phys::Body* body = slot < movers_.size() && movers_[slot].kind != gp::kMoverNone ? &movers_[slot].body : nullptr;
        u8* p = out.reserve(1 + kVarIntMaxBytes + 16 + 48 + 12 + 1 + 8);
        p = put_u8(p, is_item ? kItem : is_falling ? kFalling : type == entity_type::kTnt ? kTnt : kOther);
        p = put_varint(p, type);
        p = put_uuid(p, ents_.uuid[row]);
        p = put_f64(put_f64(put_f64(p, ents_.x[row]), ents_.y[row]), ents_.z[row]);
        p = put_f64(put_f64(put_f64(p, ents_.vx[row]), ents_.vy[row]), ents_.vz[row]);
        p = put_f32(put_f32(put_f32(p, ents_.yaw[row]), ents_.pitch[row]), ents_.head_yaw[row]);
        p = put_u8(p, static_cast<u8>(((ents_.flags[row] & entity::flag::kOnGround) ? kOnGround : 0) |
                                      ((ents_.flags[row] & entity::flag::kNoGravity) ? kNoGravity : 0)));
        out.commit(put_f64(p, body ? body->fall_distance : 0.0));
        if (is_item) {
            const u32 k = item_of_slot_[slot];
            item::write_untrusted(out, ents_.item[row]);
            // ItemEntity: Age and PickupDelay are shorts; an item is gone at 6000.
            const u64 age = std::min<u64>(entity_tick_ - item_age_from_[k], 32766);
            const u64 delay = item_pickup_[k] == kNever        ? kNeverPickUp
                              : item_pickup_[k] > entity_tick_ ? std::min<u64>(item_pickup_[k] - entity_tick_, 32766)
                                                               : 0;
            p = out.reserve(5);
            p = put_be16(p, static_cast<u16>(age));
            p = put_be16(p, static_cast<u16>(delay));
            out.commit(put_u8(p, static_cast<u8>(item_health_[k])));
        } else if (is_falling) {
            const Falling& f = falling_[slot];
            p = out.reserve(kVarIntMaxBytes + 5);
            p = put_varint(p, ents_.block_state[row]);
            p = put_be32(p, static_cast<u32>(f.time));
            out.commit(put_u8(p, static_cast<u8>((f.drop_item ? kDropItem : 0) | (f.cancel_drop ? kCancelDrop : 0))));
        } else if (type == entity_type::kTnt) {
            out.commit(put_be16(out.reserve(2), static_cast<u16>(ents_.fuse[row])));
        }
        // What a tag said of it, as a tag (write_cold_tag): VarInt n + n bytes, n = 0 for nothing.
        base::ByteBuf told;
        if (ents_.cold(row) || (ents_.shared_flags[row] & ((1u << 5) | (1u << 6)))) {
            nbt::Writer w(told);
            w.root(nbt::kCompound);
            write_cold_tag(row, w);
            w.end_compound();
        }
        put_counted(out, {told.data, told.len});
    }
}

void Game::read_entities(world::Chunk& c, std::span<const u8> bytes) {
    c.saved_entities = !bytes.empty();
    if (bytes.empty()) return;
    Reader r(bytes.data(), bytes.size());
    const u8 version = r.u8v();
    if (version < 1 || version > kEntitiesVersion) {
        LOG_ERROR("chunk %d, %d: its entities are of a version this server does not read", c.x, c.z);
        return;
    }
    while (r.ok && r.remaining()) {
        const u8 record = r.u8v();
        const u32 type = r.varint();
        const Uuid uuid = r.uuid();
        const double x = r.f64(), y = r.f64(), z = r.f64(), vx = r.f64(), vy = r.f64(), vz = r.f64();
        const float yaw = r.f32(), pitch = r.f32(), head = r.f32();
        const u8 flags = r.u8v();
        const double fall = r.f64();
        ItemStack stack;
        u16 age = 0, delay = 0, fuse = 0;
        i8 health = 0;
        u32 falling_state = 0, falling_flags = 0;
        i32 falling_time = 0;
        if (record == kItem) {
            const item::Verdict v = item::read_untrusted(r, stack);
            age = r.be16(), delay = r.be16(), health = static_cast<i8>(r.u8v());
            if (v != item::Verdict::kOk) stack = {};
        } else if (record == kTnt) {
            fuse = r.be16();
        } else if (record == kFalling) {
            falling_state = r.varint();
            falling_time = static_cast<i32>(r.be32());
            falling_flags = r.u8v();
        } else if (record != kOther) {
            r.ok = false;
        }
        const std::span<const u8> told = version >= 3 ? r.bytes(r.varint()) : std::span<const u8>{};
        if (!r.ok || type >= entity_type::kCount || ents_.size() >= entity::kMaxEntities) break;
        // Numbers that are none put an entity nowhere: such a record is passed over (Entity.load
        // refuses a position that is not finite).
        if (!(std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && std::isfinite(vx) && std::isfinite(vy) &&
              std::isfinite(vz)))
            continue;
        u32 slot = kNone;
        if (record == kItem) {
            if (type != entity_type::kItem || stack.empty()) continue;
            slot =
                add_item(uuid, yaw, std::move(stack), x, y, z, vx, vy, vz, delay == kNeverPickUp ? kNever : delay, age, health);
        } else if (record == kFalling) {
            // A state that is none falls as nothing, and is gone with its first tick (BlockState.CODEC's default
            // would be sand: here a file is not trusted that far).
            if (type != entity_type::kFallingBlock || falling_state >= world::state::kCount) continue;
            slot = add_falling(uuid, static_cast<world::BlockState>(falling_state), x, y, z, falling_time, false);
            const u32 row = ents_.by_slot(slot);
            ents_.vx[row] = vx, ents_.vy[row] = vy, ents_.vz[row] = vz;
            tracker_.created_with_velocity(row);
            falling_[slot].drop_item = (falling_flags & kDropItem) != 0;
            falling_[slot].cancel_drop = (falling_flags & kCancelDrop) != 0;
        } else {
            entity::Store::Moves moved;
            const u32 row = tracker_.spawn(type, false, uuid, x, y, z, yaw, pitch, moved);
            slot = ents_.slot[row];
            ents_.vx[row] = vx, ents_.vy[row] = vy, ents_.vz[row] = vz;
            ents_.head_yaw[row] = head;
            tracker_.created_with_velocity(row);
            const u8 mover = gp::kEntity[type].mover;
            if (record == kTnt && type == entity_type::kTnt) {
                ents_.fuse[row] = 80;  // PrimedTnt.DEFAULT_FUSE_TIME, the value a client assumes: any other is sent
                ents_.set_fuse(row, static_cast<i16>(fuse));
                tnt_.push_back(slot);
                start_ticking(slot, gp::kMoverTnt);
            } else if (mover == gp::kMoverLiving || mover == gp::kMoverArmorStand) {
                start_ticking(slot, mover);
                enter_push_grid(slot, x, z, entity_type::kInfo[type].width);
            }
        }
        if (slot == kNone) continue;
        // What nothing here moves has the flag (Game::summon); what moves has it from a tag, and
        // its body then falls no more (Entity.getGravity).
        if (flags & kNoGravity) {
            ents_.set_no_gravity(ents_.by_slot(slot), true);
            if (slot < movers_.size() && movers_[slot].kind != gp::kMoverNone) movers_[slot].body.no_gravity = true;
        }
        if (!told.empty()) {  // what a tag once said of it: read as a command's tag is
            nbt::Doc doc;
            EntityTag tag;
            std::string why;
            if (doc.parse(told, nbt::Form::kNameless, nbt::kTrusted) == nbt::Error::kNone &&
                read_entity_tag(type, doc.root(), tag, why)) {
                tag.item = {};  // (of the tag only what is cold: the rest has its record)
                apply_entity_tag(slot, std::move(tag));
            }
        }
        const u32 row = ents_.by_slot(slot);
        if (flags & kOnGround) ents_.flags[row] |= entity::flag::kOnGround;
        if (slot < movers_.size() && movers_[slot].kind != gp::kMoverNone) {
            movers_[slot].body.fall_distance = fall;
            movers_[slot].body.on_ground = (flags & kOnGround) != 0;
        }
    }
    if (!r.ok) LOG_ERROR("chunk %d, %d: its entities end too early; the rest is lost", c.x, c.z);
}

// The entities of the chunks the upkeep pass picked (Chunk::unloading) - or, with `all`, of every
// loaded chunk - by chunk: what write_entities walks.
void Game::collect_entities(bool all) {
    entities_at_.clear();
    const u32 n = ents_.size();
    for (u32 row = static_cast<u32>(players_.size()); row < n; ++row) {
        const i32 cx = chunk_of(ents_.x[row]), cz = chunk_of(ents_.z[row]);
        const world::Chunk* c = world_.find(cx, cz);
        if (!c || !(all || c->unloading)) continue;
        entities_at_.push_back({chunk_key(cx, cz), ents_.slot[row]});
    }
    std::sort(entities_at_.begin(), entities_at_.end(),
              [](const EntityAt& a, const EntityAt& b) { return a.chunk != b.chunk ? a.chunk < b.chunk : a.slot < b.slot; });
}

}  // namespace server
