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

// ---- cold tables (world/block.cpp)
u32 block_of(BlockState s);
const BlockInfo& block_info(u32 block);
inline const BlockInfo& block_info_of(BlockState s) { return block_info(block_of(s)); }
std::string_view block_name(u32 block);
u32 item_count();
const ItemInfo& item_info(u32 item);  // item < item_count()
// Collision boxes of a state in block-local coordinates; empty for states without collision.
// `dynamic` is set when vanilla computes the shape from the position or the entity instead.
std::span<const Box> collision_boxes(BlockState s, bool& dynamic);

}  // namespace world
