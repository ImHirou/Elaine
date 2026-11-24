// SPDX-License-Identifier: GPL-3.0-or-later
// BlockState.getValue, setValue and hasProperty, for the properties world generation names
// (ft::Prop: generated, tools/mcgen/gen_trees.py).
//
// A block's states are numbered as a mixed radix number of its properties' values, so a property
// is a stride: the state with another value of it is so many ids away. A value is its place among
// the property's values in vanilla's order - "true" before "false", north, south, west, east for a
// horizontal facing, a distance of 1 .. 7 as 0 .. 6.
#pragma once

#include "world/block.hpp"

#include PROTO_GEN_WORLDGEN_FEATURES_HPP

namespace worldgen::prop {

namespace ft = proto::gen::worldgen::feature;
using ft::Prop;

// hasProperty
ALWAYS_INLINE bool has(u32 state, Prop p) { return ft::kPropStride[static_cast<u32>(p)][world::block_of(state)] != 0; }

// getValue, of a state that has the property.
inline u32 get(u32 state, Prop p) {
    const u32 block = world::block_of(state);
    const u32 stride = ft::kPropStride[static_cast<u32>(p)][block];
    DCHECK(stride != 0);
    return (state - world::block_info(block).first_state) / stride % ft::kPropValues[static_cast<u32>(p)];
}

// trySetValue: the state with the property at `value` - the state itself if it has no such property.
inline u16 with(u32 state, Prop p, u32 value) {
    const u32 block = world::block_of(state);
    const u32 stride = ft::kPropStride[static_cast<u32>(p)][block];
    if (stride == 0) return static_cast<u16>(state);
    DCHECK(value < ft::kPropValues[static_cast<u32>(p)]);
    const u32 now = (state - world::block_info(block).first_state) / stride % ft::kPropValues[static_cast<u32>(p)];
    return static_cast<u16>(state + (value - now) * stride);
}

// A property of true and false.
inline bool flag(u32 state, Prop p) { return get(state, p) == 0; }
inline u16 with_flag(u32 state, Prop p, bool on) { return with(state, p, on ? 0 : 1); }

// Direction.Axis of a direction, as BlockStateProperties.AXIS counts it (x, y, z).
ALWAYS_INLINE u32 axis_of(u32 direction) { return direction < 2 ? 1 : direction < 4 ? 2 : 0; }
// A horizontal direction as HORIZONTAL_FACING counts it, and back.
ALWAYS_INLINE u32 facing_of(u32 direction) { return direction - 2; }
ALWAYS_INLINE u32 direction_of(u32 facing) { return facing + 2; }
// The property that says whether a block has a side in a direction (PipeBlock.PROPERTY_BY_DIRECTION).
ALWAYS_INLINE Prop side_of(u32 direction) {
    constexpr Prop kSides[6] = {Prop::kDown, Prop::kUp, Prop::kNorth, Prop::kSouth, Prop::kWest, Prop::kEast};
    return kSides[direction];
}

}  // namespace worldgen::prop
