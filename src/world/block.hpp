// SPDX-License-Identifier: GPL-3.0-or-later
// Block states as the world stores them: the vanilla state id (the number the wire uses), plus
// accessors for the generated per-state tables. Everything here is a table load - no block is
// special-cased in code, the properties come from the vanilla jar (tools/mcgen blockdata).
#pragma once

#include <algorithm>
#include <span>
#include <string_view>

#include "base/types.hpp"
#include "proto/gen/current.hpp"

#include PROTO_GEN_BLOCK_DATA_HPP
#include PROTO_GEN_BLOCK_INFO_HPP

namespace world {

using BlockState = u16;
namespace state = proto::gen::state;
namespace fluid = proto::gen::fluid;
namespace block_flag = proto::gen::block_flag;
namespace item_flag = proto::gen::item_flag;
using proto::gen::BlockInfo;
using proto::gen::Box;
using proto::gen::ItemInfo;

inline constexpr BlockState kAir = proto::gen::block::kAir;
// Palettes use 0xFFFF as "no state"; it must never be a real id.
static_assert(state::kCount < 0xFFFF);

// Directions in net.minecraft.core.Direction ordinal order (kDown, kUp, kNorth, kSouth, kWest,
// kEast): the order light propagates in and the index of every per-direction table.
using Direction = state::Direction;
using enum state::Direction;
inline constexpr i8 kStepX[6] = {0, 0, 0, 0, -1, 1};
inline constexpr i8 kStepY[6] = {-1, 1, 0, 0, 0, 0};
inline constexpr i8 kStepZ[6] = {0, 0, -1, 1, 0, 0};
ALWAYS_INLINE constexpr u32 opposite(u32 dir) { return dir ^ 1; }

ALWAYS_INLINE u32 flags_of(BlockState s) { return state::kFlags[s]; }
ALWAYS_INLINE bool is_air(BlockState s) { return state::kFlags[s] & state::kIsAir; }
ALWAYS_INLINE bool is_replaceable(BlockState s) { return state::kFlags[s] & state::kReplaceable; }
ALWAYS_INLINE u32 emission(BlockState s) { return state::kLight[s] >> 4; }
ALWAYS_INLINE u32 dampening(BlockState s) { return state::kLight[s] & 15; }
// LightEngine.getOpacity(): light always loses at least one level per block.
ALWAYS_INLINE u32 opacity(BlockState s) { return std::max<u32>(1, dampening(s)); }
// LightEngine.isEmptyShape(): no face of this state can occlude light by its shape.
ALWAYS_INLINE bool has_empty_shape(BlockState s) { return state::kOcclusion[s] == 0; }
// LightEngine.shapeOccludes(from, to, dir): the two touching faces together cover the whole face.
ALWAYS_INLINE bool shape_occludes(BlockState from, BlockState to, u32 dir) {
    return (state::kOccludes[state::kOcclusion[from]][dir] >> state::kOcclusion[to]) & 1;
}
// LightEngine.hasDifferentLightProperties(): does replacing `a` by `b` need a light update?
ALWAYS_INLINE bool light_differs(BlockState a, BlockState b) {
    return a != b && (state::kLight[a] != state::kLight[b] || state::kOcclusion[a] != 0 || state::kOcclusion[b] != 0);
}

// ChunkSkyLightSources.isEdgeOccluded(): does full sky light stop between these two stacked blocks?
ALWAYS_INLINE bool sky_edge_occluded(BlockState top, BlockState bottom) {
    return dampening(bottom) != 0 || shape_occludes(top, bottom, kDown);
}
// ---- random ticks: what state::kRandom says of a cell and of the cell on top of it
// (SpreadingSnowyBlock.canStayAlive / NyliumBlock.canBeNylium, negated; canPropagate onto soil).
// `r`, `above`: kRandom of the two states.
ALWAYS_INLINE bool random_dies(u32 r, u32 above) {
    return ((r & state::kSpreader) && (above & state::kSmothers)) || ((r & state::kNylium) && (above & state::kSmothersNylium));
}
ALWAYS_INLINE bool random_target(u32 r, u32 above) { return (r & state::kSoil) && !(above & state::kNoSpread); }
// BlockState.isFaceSturdy(level, pos, dir) - SupportType.FULL - and (..., SupportType.CENTER).
ALWAYS_INLINE bool face_sturdy(BlockState s, u32 dir) { return (state::kSturdyFull[s] >> dir) & 1; }
ALWAYS_INLINE bool face_sturdy_center(BlockState s, u32 dir) { return (state::kSturdyCenter[s] >> dir) & 1; }
// BlockState.isSolid(): what a sign stands on, or hangs against.
ALWAYS_INLINE bool is_solid(BlockState s) { return (state::kSturdyCenter[s] & 0x40) != 0; }
// Block.isFaceFull(the state's collision shape, DOWN): what a hanging sign hangs from by both chains.
ALWAYS_INLINE bool collision_full_below(BlockState s) { return (state::kSturdyCenter[s] & 0x80) != 0; }

// ---- fluids. A FluidState is its id in Fluid.FLUID_STATE_REGISTRY - which fluid, how much of
// it, falling or not - and 0 is the empty one: what most blocks hold.
using FluidState = u8;
// BlockState.getFluidState(): the fluid a state is (water, lava) or holds (a waterlogged block).
ALWAYS_INLINE FluidState fluid_of(BlockState s) { return state::kFluid[s]; }
ALWAYS_INLINE u32 fluid_type(FluidState f) { return fluid::kType[f]; }                    // getType(): the fluid's id
ALWAYS_INLINE u32 fluid_family(FluidState f) { return fluid::kFamily[fluid::kType[f]]; }  // Fluid.isSame; kNoFamily
ALWAYS_INLINE u32 fluid_amount(FluidState f) { return fluid::kAmount[f]; }                // getAmount()
ALWAYS_INLINE bool fluid_source(FluidState f) { return fluid::kFlags[f] & fluid::kIsSource; }
ALWAYS_INLINE bool fluid_falling(FluidState f) { return fluid::kFlags[f] & fluid::kIsFalling; }
// FlowingFluid.getOwnHeight: how much of its cell a fluid fills when nothing of it is above.
ALWAYS_INLINE float fluid_own_height(FluidState f) { return static_cast<float>(fluid::kAmount[f]) / 9.0f; }
// FluidState.createLegacyBlock(): the block state that is this fluid and nothing else (air for none).
ALWAYS_INLINE BlockState fluid_block(FluidState f) { return fluid::kLegacy[f]; }
// FluidState.is(FluidTags.WATER): water, still or flowing, as the block or with it (waterlogged) -
// and FluidState.is(Fluids.WATER): a source of it.
ALWAYS_INLINE bool holds_water(BlockState s) { return fluid::kFlags[state::kFluid[s]] & fluid::kInWater; }
ALWAYS_INLINE bool holds_water_source(BlockState s) { return fluid::kType[state::kFluid[s]] == fluid::kWater; }
// FlowingFluid.canPassThroughWall(dir, from, to): the collision shapes of two blocks that touch
// do not, between them, cover the face they share - a fluid gets from one to the other.
ALWAYS_INLINE bool fluid_passes(BlockState from, BlockState to, u32 dir) {
    return !(
        (state::kFlowCovers[state::kFlowFace[state::kFlowFaces[from]][dir]] >> state::kFlowFace[state::kFlowFaces[to]][dir ^ 1]) &
        1);
}

// Block.isFaceFull(the collision shape, dir): the shape covers that whole face of its cell - what
// the empty layer of a neighbour (class 0) could not cover the face without.
ALWAYS_INLINE bool collision_face_full(BlockState s, u32 dir) {
    return state::kFlowCovers[state::kFlowFace[state::kFlowFaces[s]][dir]] & 1;
}

// What a state means to the queries that ask a chunk about its sections first (Chunk::large_sections
// and the masks beside it): bit 0 a collision shape that reaches out of its cell, bit 1 it acts on
// an entity in it or on it, bit 2 it breaks a fall. (world/chunk.cpp)
u32 section_traits(BlockState s);

// ---- cold tables (world/block.cpp)
u32 block_of(BlockState s);
const BlockInfo& block_info(u32 block);
inline const BlockInfo& block_info_of(BlockState s) { return block_info(block_of(s)); }
std::string_view block_name(u32 block);
u32 item_count();
const ItemInfo& item_info(u32 item);  // item < item_count()
// ---- by name (cold: commands, text components). A name is a whole identifier, "minecraft:stone".
inline constexpr u32 kNoId = ~0u;
u32 find_block(std::string_view name);              // a block id, kNoId
u32 find_item(std::string_view name);               // an item id, kNoId
u32 find_entity_type(std::string_view name);        // an entity type id, kNoId
u32 find_biome(std::string_view name);              // a worldgen/biome network id, kNoId
u32 find_block_entity_type(std::string_view name);  // a block_entity_type id, kNoId
std::string_view biome_name(u32 biome);
std::string_view block_entity_type_name(u32 type);
u32 biome_count();
std::string_view item_name(u32 item);
std::string_view item_text_key(u32 item);  // the translation key of its default ITEM_NAME component
u32 item_rarity(u32 item);                 // Rarity ordinal of its default RARITY: common, uncommon, rare, epic
std::string_view entity_type_name(u32 type);
bool summonable(u32 type);   // EntityType.canSummon()
u32 spawn_egg_of(u32 type);  // SpawnEggItem.byId: the egg that makes this entity type, 0 if none does
// Block.getStateDefinition(): a block's properties in order. The state with value index v_i of
// property i is first_state + sum(v_i * stride_i).
struct BlockProperty {
    std::string_view name;
    std::span<const std::string_view> values;
    u32 stride;
};
u32 block_property_count(u32 block);
BlockProperty block_property(u32 block, u32 index);
// Collision boxes of a state in block-local coordinates; empty for states without collision.
// `dynamic` is set when vanilla computes the shape from the position or the entity instead.
std::span<const Box> collision_boxes(BlockState s, bool& dynamic);
// What a look is stopped by: the state's outline (BlockState.getShape for nobody in particular),
// and the shape that says which face was clicked where the outline is hollow
// (getInteractionShape: a cauldron, a hopper; nothing for nearly every state).
std::span<const Box> outline_boxes(BlockState s);
std::span<const Box> interaction_boxes(BlockState s);
// BlockState.getOffset: by how much the outline of a flower, of bamboo, of a dripstone is moved
// from the middle of its cell - by where the block stands (x, z), the same every time. False,
// and nothing, for every other state.
bool outline_offset(BlockState s, i32 x, i32 z, double out[3]);
// BlockState.canBeReplaced(Fluid): does it give way to the fluid a bucket is emptied into its
// place - replaceable, or not solid (a torch), unless its class says never.
ALWAYS_INLINE bool fluid_replaces(BlockState s) {
    return !(state::kFluidFacts[s] & state::kNeverReplaced) && (is_replaceable(s) || !is_solid(s));
}

}  // namespace world
