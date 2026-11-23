// SPDX-License-Identifier: GPL-3.0-or-later
// A block state as a test writes it: "minecraft:oak_slab[type=top,waterlogged=true]" - the
// block's default state with the properties that are named set (as a command would read it).
#pragma once

#include <gtest/gtest.h>

#include <string_view>

#include "world/block.hpp"

inline world::BlockState state_of(std::string_view spec) {
    using namespace world;
    const usize bracket = spec.find('[');
    const std::string_view name = spec.substr(0, bracket);
    const u32 block = find_block(name);
    if (block == kNoId) {
        ADD_FAILURE() << "no block " << name;
        return kAir;
    }
    const BlockInfo& info = block_info(block);
    u32 state = info.default_state;
    if (bracket == std::string_view::npos) return static_cast<BlockState>(state);
    std::string_view rest = spec.substr(bracket + 1, spec.size() - bracket - 2);
    while (!rest.empty()) {
        const usize comma = rest.find(',');
        const std::string_view pair = rest.substr(0, comma);
        rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
        const usize eq = pair.find('=');
        const std::string_view key = pair.substr(0, eq), value = pair.substr(eq + 1);
        bool found = false;
        for (u32 i = 0; i < block_property_count(block) && !found; ++i) {
            const BlockProperty p = block_property(block, i);
            if (p.name != key) continue;
            for (u32 v = 0; v < p.values.size(); ++v) {
                if (p.values[v] != value) continue;
                const u32 now = (state - info.first_state) / p.stride % static_cast<u32>(p.values.size());
                state = state + (v - now) * p.stride;
                found = true;
            }
        }
        if (!found) ADD_FAILURE() << spec << ": no property value " << pair;
    }
    return static_cast<BlockState>(state);
}
