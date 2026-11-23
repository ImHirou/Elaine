// SPDX-License-Identifier: GPL-3.0-or-later
// Falling blocks: sand, gravel and concrete powder with nothing under them - a block that is an
// entity for the length of its fall.
//
// The rules are vanilla's, read from the 26.3 jar; each function names the method it follows:
//   FallingBlock (tick: isFree below - that is server/block_updates.cpp), FallingBlockEntity
//   (fall, the constructors, tick, getAddEntityPacket, addAdditionalSaveData),
//   ConcretePowderBlock.onLand, Entity.spawnAtLocation
// How it moves is phys::falling_move (gravity, Entity.move, the blocks it passes), held to
// vanilla's bits like the rest of src/phys; this file is what it is and where it ends.
//
// A client moves a falling block by itself (MoveSimulationType.SERVER_AND_CLIENT) and takes it
// away when told; the block it becomes is the server's to say - twice: to those who see the
// entity at once (block_update, add_transient_block), to everyone with the chunk's changes.
//
// What is NOT here:
//   * anvils and pointed dripstone, the two that hurt what they land on (causeFallDamage), and
//     the anvil that breaks of it: neither block is placeable, nothing has health;
//   * block entity data that falls with the block (FallingBlockEntity.blockData): no block
//     that falls has a block entity;
//   * a falling block's own NBT from /summon: one that is summoned is sand, as in vanilla
//     without a tag;
//   * concrete powder that falls faster than a block a tick is looked for water along its way
//     (Level.clip) only when it falls straight down, and what stops that look is any block with
//     a collision shape, not the shape itself.
#include <cmath>

#include "server/game.hpp"

#include PROTO_GEN_PHYSICS_HPP

namespace server {

using namespace proto;
using world::BlockState;
namespace block_kind = proto::gen::block_kind;
namespace block_flag = world::block_flag;
namespace gp = proto::gen::physics;
namespace st = world::state;

namespace {

constexpr u64 kDropPickupDelay = 10;  // Entity.spawnAtLocation -> ItemEntity.setDefaultPickUpDelay
constexpr i32 kOutOfWorldTime = 100;  // FallingBlockEntity.tick: ticks before one outside the build height is given up...
constexpr i32 kMaxTime = 600;         // ...and before any is

}  // namespace

// A FallingBlockEntity with everything about it said: a new one (fall), one that was summoned,
// one that comes back with its chunk. `fell`: it is a block that fell (the private constructor:
// it remembers where from, and nothing is built where it is). Returns its slot.
u32 Game::add_falling(Uuid uuid, BlockState state, double x, double y, double z, i32 time, bool fell) {
    entity::Store::Moves moved;
    const u32 row = tracker_.spawn(entity_type::kFallingBlock, false, uuid, x, y, z, 0, 0, moved);
    const u32 slot = ents_.slot[row];
    ents_.block_state[row] = state;
    if (fell) {
        // setStartPos(blockPosition()): synchronized, and dirty from the start like every value
        // a constructor sets - whoever sees the entity from its first tick is told twice.
        ents_.start_pos[row] = pack_block_pos(floor_int(x), floor_int(y), floor_int(z));
        if (ents_.start_pos[row]) ents_.dirty[row] |= 1u << entity_data::falling_block::kStartPos;
    }
    if (slot >= falling_.size()) falling_.resize(ents_.slot_count());
    falling_[slot] = {time, true, false, fell, true};
    falling_slots_.push_back(slot);
    start_ticking(slot, gp::kMoverFallingBlock);
    return slot;
}

// FallingBlockEntity.fall: the block at `pos` becomes an entity in its place.
u32 Game::fall(BlockPos pos, BlockState state) {
    // The entity table is full (vanilla's has no end): the block stays one, until it is asked again.
    if (ents_.size() >= entity::kMaxEntities) return kNone;
    // (A waterlogged block would fall dry and leave its water. No block that falls here holds any.)
    // Level.setBlockAndUpdate with what the block leaves, then addFreshEntity: what the block's
    // going sets off has happened by the time the entity is anybody's to see.
    set_block(pos, static_cast<BlockState>(st::fluid_legacy_of(state)));
    if (UNLIKELY(ents_.size() >= entity::kMaxEntities)) {  // ...and what its going dropped took the last rows
        set_block(pos, state);
        return kNone;
    }
    const Uuid uuid{(random() & ~0xF000ull) | 0x4000ull, (random() & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull};
    return add_falling(uuid, state, pos.x + 0.5, pos.y, pos.z + 0.5, 0, true);
}

// FallingBlockEntity.tick
void Game::tick_falling(u32 slot, u32 row) {
    Falling& f = falling_[slot];
    const BlockState state = ents_.block_state[row];
    if (world::is_air(state)) return remove_entity(slot);
    const world::BlockInfo& info = world::block_info_of(state);
    ++f.time;
    phys::Body& body = movers_[slot].body;
    phys::Motion m = motion_of(row);
    const double from[3] = {m.pos[0], m.pos[1], m.pos[2]};  // xo, yo, zo
    phys::World w = phys_world();
    phys::falling_move(w, m, body);
    took_fire(row, body, from, false);  // (lava lights it and nobody sees: it has no base tick, and is gone when it lands)

    BlockPos pos{floor_int(m.pos[0]), floor_int(m.pos[1]), floor_int(m.pos[2])};  // blockPosition()
    const bool concrete = info.kind == block_kind::kConcretePowder;
    bool in_water = concrete && world::holds_water(state_for_client(pos));
    if (concrete && m.vel[0] * m.vel[0] + m.vel[1] * m.vel[1] + m.vel[2] * m.vel[2] > 1.0 && from[0] == m.pos[0] &&
        from[2] == m.pos[2]) {
        // Level.clip(from, to, COLLIDER, SOURCE_ONLY): a source of water on its way catches it,
        // though the tick carried it through (see the top of the file for what this leaves out).
        for (i32 y = floor_int(from[1]); y >= pos.y; --y) {
            const BlockState there = state_for_client({pos.x, y, pos.z});
            if (world::holds_water_source(there)) {
                pos.y = y;
                in_water = true;
                break;
            }
            if ((gp::kState[there] & gp::kShapeMask) != gp::kShapeNone) break;
        }
    }
    // discard(), then Entity.spawnAtLocation(level, block) if it drops: what it is, as an item, where it is.
    const bool drops = f.drop_item && info.item != 0;
    const auto discard = [&](bool drop) {
        remove_entity(slot);
        if (drop) spawn_item(ItemStack(info.item, 1), m.pos[0], m.pos[1], m.pos[2], kDropPickupDelay);
    };

    if (!body.on_ground && !in_water) {
        if ((f.time > kOutOfWorldTime && (pos.y <= world_.min_y() || pos.y > world_.max_y())) || f.time > kMaxTime)
            return discard(drops);
    } else {
        const BlockState current = state_for_client(pos);
        m.vel[0] = m.vel[0] * 0.7, m.vel[1] = m.vel[1] * -0.5, m.vel[2] = m.vel[2] * 0.7;
        if (world::block_of(current) != world::block_of(gen::block::kMovingPiston)) {
            if (f.cancel_drop) return discard(false);
            // canBeReplaced(new DirectionalPlaceContext(..., ItemStack.EMPTY, ...)): what gives way to a block
            // set into it. (A class with a rule of its own for that: a slab never does, and petals and leaf
            // litter - which make room for more of themselves only - do as any block of theirs would.)
            const world::BlockInfo& there = world::block_info_of(current);
            const bool may_replace = world::is_replaceable(current) &&
                                     (!(there.flags & block_flag::kCustomReplace) || there.kind == gen::block_kind::kSegmented);
            const bool would_go_on =
                (st::kHooks[state_for_client({pos.x, pos.y - 1, pos.z})] & st::kFallThrough) && (!concrete || !in_water);
            if (may_replace && can_survive(state, pos) && !would_go_on) {
                if (set_block(pos, state)) {
                    // ChunkMap.sendToTrackingPlayers: those who see the entity see the block at once.
                    const BlockState placed = world_.block(pos.x, pos.y, pos.z);
                    scratch_.clear();
                    write_packet(scratch_, s_.compression_threshold, cb::BlockUpdate{pos.x, pos.y, pos.z, placed});
                    write_packet(scratch_, s_.compression_threshold, cb::AddTransientBlock{pos.x, pos.y, pos.z, placed});
                    tracker_.broadcast(row, {scratch_.data, scratch_.len}, out_.data());
                    discard(false);
                    // Fallable.onLand - ConcretePowderBlock: in water, or beside it, it is concrete.
                    if (concrete && (world::holds_water(current) || touches_water(pos))) set_block(pos, info.alt[0]);
                    return;
                }
                if (f.drop_item) return discard(drops);  // it could not be set (above the world): it breaks
            } else {
                return discard(drops);
            }
        }
    }
    const double drag = static_cast<double>(0.98f);  // Entity.getAirDrag
    m.vel[0] = m.vel[0] * drag, m.vel[1] = m.vel[1] * drag, m.vel[2] = m.vel[2] * drag;
    // setRequiresPrecisePosition(horizontalCollision || verticalCollision), as a primed TNT.
    const bool touching = body.horizontal_collision || body.vertical_collision;
    store_motion(row, m, body);
    ents_.flags[row] =
        static_cast<u16>(touching ? ents_.flags[row] | entity::flag::kPrecise : ents_.flags[row] & ~entity::flag::kPrecise);
}

}  // namespace server
