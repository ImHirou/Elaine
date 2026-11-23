// SPDX-License-Identifier: GPL-3.0-or-later
// Fluids, as far as they are functions of the blocks:
//   * the generated tables against what the jar says of a few states (LiquidBlock.getFluidState,
//     FlowingFluid.getLegacyLevel, who holds a fluid);
//   * "may a fluid pass between these two blocks" (state::kFlowCovers, made by mcgen from the
//     boxes of the collision shapes) against Shapes.mergedFaceOccludes written out here over
//     the grids of the same shapes - every pair of shapes, every direction;
//   * FlowingFluid.getNewLiquid and getSpread on worlds built by hand, where the answer is known
//     from the method - and getSpread's search with its memory against the same search without.
// What a fluid then does to the world is the game's, and is tested by the scenarios
// (tests/integration/run.py: water_*) and against a vanilla server (tools/vanilla/block_probe.py).
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <random>
#include <string>
#include <tuple>
#include <vector>

#include "proto/frame.hpp"
#include "world/dimension.hpp"
#include "world/fluid.hpp"

#include "states.hpp"

#include PROTO_GEN_DIMENSION_TYPES_HPP
#include PROTO_GEN_PHYSICS_HPP

using namespace world;
namespace blk = proto::gen::block;
namespace st = world::state;
namespace physics = proto::gen::physics;

namespace {

constexpr u32 kW = fluid::kWaterFamily;
FluidState flowing(u32 amount, bool falling = false) { return fluid::kFlowingOf[kW][falling][amount]; }
FluidState source(bool falling = false) { return fluid::kSourceOf[kW][falling]; }

// ---- a VoxelShape as the tables hold it (physics::kShapes): coordinates per axis, a bit a cell
struct Grid {
    const double* c[3];
    u32 n[3];
    const unsigned long long* cells;
    bool full(u32 x, u32 y, u32 z) const {
        const u32 bit = (x * n[1] + y) * n[2] + z;
        return (cells[bit >> 6] >> (bit & 63)) & 1;
    }
    bool full(u32 axis, u32 layer, u32 u, u32 w) const {  // the cell (u, w) of a layer across `axis`
        u32 at[3];
        at[axis] = layer, at[(axis + 1) % 3] = u, at[(axis + 2) % 3] = w;
        return full(at[0], at[1], at[2]);
    }
    bool empty() const { return n[0] == 0; }
};
Grid grid_of(u32 shape) {
    const physics::Shape& s = physics::kShapes[shape];
    const double* c = physics::kShapeCoords + s.coords;
    return {{c, c + s.nx + 1, c + s.nx + s.ny + 2}, {s.nx, s.ny, s.nz}, physics::kShapeCells + s.cells};
}
// VoxelShape.min / max: where the full cells begin and end along an axis (infinite for none).
double edge(const Grid& g, u32 axis, bool max) {
    const u32 b = (axis + 1) % 3, c = (axis + 2) % 3;
    for (u32 k = 0; k < g.n[axis]; ++k) {
        const u32 layer = max ? g.n[axis] - 1 - k : k;
        for (u32 u = 0; u < g.n[b]; ++u)
            for (u32 w = 0; w < g.n[c]; ++w)
                if (g.full(axis, layer, u, w)) return g.c[axis][max ? layer + 1 : layer];
    }
    return max ? -INFINITY : INFINITY;
}
// Shapes.block(): the one cell from 0 to 1.
bool is_block(const Grid& g) {
    for (u32 a = 0; a < 3; ++a)
        if (g.n[a] != 1 || g.c[a][0] != 0.0 || g.c[a][1] != 1.0) return false;
    return g.full(0, 0, 0);
}

// Shapes.mergedFaceOccludes(shape, occluder, direction), as the jar has it: of the shape on the
// near side the last layer of cells along the axis - if it reaches 1 - of the one on the far
// side the first - if it begins at 0 - each stretched over the whole axis (SliceShape); the two
// joined (OR), and whether anything of the block is left outside the join (joinIsNotEmpty with
// ONLY_FIRST: over the merged coordinates, an interval under 1e-7 being none).
bool merged_face_occludes(const Grid& shape, const Grid& occluder, u32 dir) {
    if (is_block(shape) || is_block(occluder)) return true;
    const u32 axis = (dir >> 1) == 0 ? 1 : (dir >> 1) == 1 ? 2 : 0;  // down/up y, north/south z, west/east x
    const bool positive = dir & 1;
    const Grid& first = positive ? shape : occluder;
    const Grid& second = positive ? occluder : shape;
    const bool has_first = !first.empty() && std::abs(edge(first, axis, true) - 1.0) <= 1e-7;
    const bool has_second = !second.empty() && std::abs(edge(second, axis, false)) <= 1e-7;
    if (!has_first && !has_second) return false;  // the join is empty: the block is not covered
    const u32 b = (axis + 1) % 3, c = (axis + 2) % 3;
    const auto lines = [&](u32 other) {
        std::vector<double> v = {0.0, 1.0};
        for (const Grid* g : {has_first ? &first : nullptr, has_second ? &second : nullptr})
            for (u32 i = 0; g && i <= g->n[other]; ++i)
                if (g->c[other][i] > 0.0 && g->c[other][i] < 1.0) v.push_back(g->c[other][i]);
        std::sort(v.begin(), v.end());
        return v;
    };
    const auto cell = [](const Grid& g, u32 other, double at) -> i32 {
        for (u32 i = 0; i < g.n[other]; ++i)
            if (g.c[other][i] <= at && at < g.c[other][i + 1]) return static_cast<i32>(i);
        return -1;
    };
    const auto covered = [&](const Grid& g, u32 layer, double pu, double pw) {
        const i32 u = cell(g, b, pu), w = cell(g, c, pw);
        return u >= 0 && w >= 0 && g.full(axis, layer, static_cast<u32>(u), static_cast<u32>(w));
    };
    const std::vector<double> us = lines(b), ws = lines(c);
    for (usize i = 0; i + 1 < us.size(); ++i) {
        for (usize j = 0; j + 1 < ws.size(); ++j) {
            if (us[i + 1] - us[i] < 1e-7 || ws[j + 1] - ws[j] < 1e-7) continue;
            const double pu = (us[i] + us[i + 1]) / 2, pw = (ws[j] + ws[j + 1]) / 2;
            if (!(has_first && covered(first, first.n[axis] - 1, pu, pw)) && !(has_second && covered(second, 0, pu, pw)))
                return false;
        }
    }
    return true;
}

// A flat world with room to build in: the ground's top is y -61, the cells above it are air.
struct Land {
    FlatGenerator gen{-64, 24, FlatGenerator::default_layers(), 39};
    Dimension dim{proto::reg::dimension_type::kOverworld, "minecraft:overworld", gen, proto::kNoCompression};
    Land() {
        for (i32 cz = -1; cz <= 1; ++cz)
            for (i32 cx = -1; cx <= 1; ++cx) dim.ticking(cx, cz);
    }
    void put(i32 x, i32 y, i32 z, std::string_view spec) { dim.set_block(x, y, z, state_of(spec)); }
    BlockState at(i32 x, i32 y, i32 z) const { return dim.block(x, y, z); }
};
constexpr i32 kFloor = -61, kY = -60;  // the ground's top, and the level the fluid is on

std::vector<std::pair<u32, FluidState>> ways(const Spread& s) {
    std::vector<std::pair<u32, FluidState>> v;
    for (u32 i = 0; i < s.n; ++i) v.push_back({s.dir[i], s.fluid[i]});
    return v;
}

}  // namespace

TEST(Fluids, AStateHoldsWhatTheJarSays) {
    // LiquidBlock.getFluidState: level 0 a source, 1..7 so much less than 8 of it flowing, 8 and
    // more a falling column - and FlowingFluid.getLegacyLevel the other way round.
    const BlockState water = state_of("minecraft:water");
    ASSERT_EQ(water, blk::kWater);
    FluidState f = fluid_of(water);
    EXPECT_EQ(f, source());
    EXPECT_TRUE(fluid_source(f));
    EXPECT_FALSE(fluid_falling(f));
    EXPECT_EQ(fluid_amount(f), 8u);
    EXPECT_EQ(fluid_type(f), fluid::kWater);
    EXPECT_EQ(fluid_family(f), fluid::kWaterFamily);
    EXPECT_EQ(fluid_own_height(f), 0.8888889f);
    EXPECT_EQ(fluid_block(f), water);
    for (u32 level = 1; level < 8; ++level) {
        f = fluid_of(static_cast<BlockState>(water + level));
        EXPECT_EQ(f, flowing(8 - level));
        EXPECT_EQ(fluid_type(f), fluid::kFlowingWater);
        EXPECT_EQ(fluid_family(f), fluid::kWaterFamily);
        EXPECT_FALSE(fluid_source(f));
        EXPECT_FALSE(fluid_falling(f));
        EXPECT_EQ(fluid_amount(f), 8 - level);
        EXPECT_EQ(fluid_block(f), water + level);
    }
    EXPECT_EQ(fluid_own_height(flowing(1)), 0.11111111f);
    for (u32 level = 8; level < 16; ++level) {
        f = fluid_of(static_cast<BlockState>(water + level));
        EXPECT_EQ(f, flowing(8, true));
        EXPECT_TRUE(fluid_falling(f));
        EXPECT_EQ(fluid_amount(f), 8u);
        EXPECT_EQ(fluid_block(f), water + 8);
    }
    const BlockState lava = state_of("minecraft:lava");
    EXPECT_EQ(fluid_type(fluid_of(lava)), fluid::kLava);
    EXPECT_EQ(fluid_family(fluid_of(lava)), fluid::kLavaFamily);
    EXPECT_EQ(fluid_type(fluid_of(static_cast<BlockState>(lava + 2))), fluid::kFlowingLava);
    EXPECT_EQ(fluid_block(fluid_of(static_cast<BlockState>(lava + 2))), lava + 2);
    // Nothing: the empty fluid, of no family, whose block is air.
    EXPECT_EQ(fluid_of(kAir), 0);
    EXPECT_EQ(fluid_of(blk::kStone), 0);
    EXPECT_EQ(fluid_family(0), fluid::kNoFamily);
    EXPECT_EQ(fluid_block(0), kAir);

    // A waterlogged block holds a source; what is left of it when it goes is the water's block.
    const BlockState wet = state_of("minecraft:oak_slab[waterlogged=true]"), dry = state_of("minecraft:oak_slab");
    EXPECT_EQ(fluid_of(wet), source());
    EXPECT_EQ(fluid_of(dry), 0);
    EXPECT_EQ(st::fluid_legacy_of(wet), water);
    EXPECT_EQ(st::fluid_legacy_of(dry), kAir);
    EXPECT_EQ(st::fluid_legacy_of(static_cast<BlockState>(water + 12)), water + 8);
    // WaterloggedTransparentBlock.getFluidState is getSource(true): a source that "falls".
    const BlockState grate = state_of("minecraft:copper_grate[waterlogged=true]");
    EXPECT_EQ(fluid_of(grate), source(true));
    EXPECT_EQ(st::fluid_legacy_of(grate), water);
    // FluidState.is(FluidTags.WATER) and is(Fluids.WATER)
    for (const BlockState s : {water, wet, grate}) EXPECT_TRUE(holds_water(s) && holds_water_source(s)) << s;
    EXPECT_TRUE(holds_water(static_cast<BlockState>(water + 3)));
    EXPECT_FALSE(holds_water_source(static_cast<BlockState>(water + 3)));
    for (const BlockState s : {dry, lava, kAir}) EXPECT_FALSE(holds_water(s) || holds_water_source(s)) << s;

    // FlowingFluid.canHoldAnyFluid: a LiquidBlockContainer, or a block of #washed_away_by_fluids.
    const u32 hold_facts = st::kHoldsFluid | st::kFluidContainer | st::kTakesWater | st::kCreativeOnly;
    const auto facts = [&](std::string_view spec) { return st::kFluidFacts[state_of(spec)] & hold_facts; };
    EXPECT_EQ(facts("minecraft:air"), st::kHoldsFluid);
    EXPECT_EQ(facts("minecraft:water"), st::kHoldsFluid);
    EXPECT_EQ(facts("minecraft:lava"), st::kHoldsFluid);
    EXPECT_EQ(facts("minecraft:torch"), st::kHoldsFluid);
    EXPECT_EQ(facts("minecraft:dandelion"), st::kHoldsFluid);
    // ...and of those that hold water by themselves, which take a source of it when asked
    // (LiquidBlockContainer.canPlaceLiquid): a SimpleWaterloggedBlock, but a double slab, and a
    // barrier only from a player in creative mode. One whose class is not ported is left alone.
    const u32 holds = st::kHoldsFluid | st::kFluidContainer;
    EXPECT_EQ(facts("minecraft:oak_slab"), holds | st::kTakesWater);
    EXPECT_EQ(facts("minecraft:oak_slab[waterlogged=true]"), holds | st::kTakesWater);  // (canPlaceLiquid does not look)
    EXPECT_EQ(facts("minecraft:oak_slab[type=double]"), holds);
    EXPECT_EQ(facts("minecraft:oak_fence"), holds | st::kTakesWater);
    EXPECT_EQ(facts("minecraft:oak_sign"), holds | st::kTakesWater);
    EXPECT_EQ(facts("minecraft:barrier"), holds | st::kCreativeOnly);
    EXPECT_EQ(facts("minecraft:oak_trapdoor"), holds);
    EXPECT_TRUE(can_hold_specific(state_of("minecraft:oak_slab"), fluid::kWater));
    EXPECT_FALSE(can_hold_specific(state_of("minecraft:oak_slab"), fluid::kFlowingWater));  // water that flows goes into no slab
    EXPECT_FALSE(can_hold_specific(state_of("minecraft:oak_slab"), fluid::kLava));
    EXPECT_FALSE(can_hold_specific(state_of("minecraft:oak_slab[type=double]"), fluid::kWater));
    EXPECT_FALSE(can_hold_specific(state_of("minecraft:barrier"), fluid::kWater));
    EXPECT_FALSE(can_hold_specific(state_of("minecraft:oak_trapdoor"), fluid::kWater));
    EXPECT_TRUE(can_hold_specific(state_of("minecraft:torch"), fluid::kFlowingLava));  // (what holds none takes any)
    // BlockInfo::wet_stride: a wet state plus it is the same block dry.
    for (const char* name : {"minecraft:oak_slab", "minecraft:oak_fence", "minecraft:chest", "minecraft:oak_sign",
                             "minecraft:barrier", "minecraft:copper_grate"}) {
        const BlockState dry_one = state_of(name), wet_one = state_of(std::string(name) + "[waterlogged=true]");
        const u32 stride = block_info_of(dry_one).wet_stride;
        EXPECT_NE(stride, 0u) << name;
        EXPECT_EQ(wet_one + stride, dry_one) << name;
    }
    EXPECT_EQ(block_info_of(state_of("minecraft:oak_trapdoor")).wet_stride, 0);  // (not ported: left as it is)
    EXPECT_EQ(block_info_of(blk::kStone).wet_stride, 0);
    EXPECT_EQ(block_info_of(blk::kWater).wet_stride, 0);
    EXPECT_EQ(facts("minecraft:stone"), 0);
    EXPECT_EQ(facts("minecraft:oak_door"), 0);
    EXPECT_EQ(facts("minecraft:glass"), 0);
    // What a fluid beside it does not run off over (#blocks_fluid_flow: what blocks motion, and
    // signs), and what is no wall to one that falls beside it (an IceBlock): FlowingFluid.getFlow.
    const auto flow = [](std::string_view spec) { return st::kFluidFacts[state_of(spec)] & (st::kBlocksFlow | st::kIce); };
    EXPECT_EQ(flow("minecraft:stone"), st::kBlocksFlow);
    EXPECT_EQ(flow("minecraft:oak_slab"), st::kBlocksFlow);
    EXPECT_EQ(flow("minecraft:oak_sign"), st::kBlocksFlow);
    EXPECT_EQ(flow("minecraft:ice"), st::kBlocksFlow | st::kIce);
    EXPECT_EQ(flow("minecraft:frosted_ice"), st::kBlocksFlow | st::kIce);
    EXPECT_EQ(flow("minecraft:packed_ice"), st::kBlocksFlow);
    EXPECT_EQ(flow("minecraft:air"), 0);
    EXPECT_EQ(flow("minecraft:water"), 0);
    EXPECT_EQ(flow("minecraft:torch"), 0);
    EXPECT_EQ(flow("minecraft:cobweb"), 0);

    // The block of a fluid: of a ported class, set by a command, placed by no item.
    const BlockInfo& info = block_info_of(water);
    EXPECT_EQ(info.kind, proto::gen::block_kind::kLiquid);
    EXPECT_TRUE(info.flags & block_flag::kSettable);
    EXPECT_EQ(info.flags & block_flag::kPlaceMask, block_flag::kPlaceNone);
    EXPECT_FALSE(block_info_of(blk::kStone).flags & block_flag::kSettable);
    EXPECT_EQ(block_info_of(lava).kind, proto::gen::block_kind::kLiquid);
    EXPECT_TRUE(block_info_of(lava).flags & block_flag::kSettable);
    // How lava flows is the dimension's to say: fast in the nether, and there alone.
    namespace dimension = proto::gen::dimension;
    EXPECT_FALSE(dimension::kTypes[proto::reg::dimension_type::kOverworld].fast_lava);
    EXPECT_FALSE(dimension::kTypes[proto::reg::dimension_type::kOverworld].water_evaporates);
    EXPECT_TRUE(dimension::kTypes[proto::reg::dimension_type::kTheNether].fast_lava);
    EXPECT_TRUE(dimension::kTypes[proto::reg::dimension_type::kTheNether].water_evaporates);
    EXPECT_FALSE(dimension::kTypes[proto::reg::dimension_type::kTheEnd].fast_lava);
}

TEST(Fluids, WhatPassesBetweenTwoBlocksIsWhatTheirShapesLeaveOpen) {
    // One state of every collision shape; the table is asked through states, as the flow asks it.
    std::vector<u32> of_shape(st::kCollisionShapes, ~0u);
    for (u32 s = 0; s < st::kCount; ++s) {
        const u32 id = st::collision_of(s);
        if (id != st::kDynamicCollision && of_shape[id] == ~0u) of_shape[id] = s;
    }
    u32 differ = 0, open = 0, shut = 0;
    for (u32 a = 0; a < st::kCollisionShapes; ++a) {
        if (of_shape[a] == ~0u) continue;
        const Grid ga = grid_of(a);
        for (u32 b = 0; b < st::kCollisionShapes; ++b) {
            if (of_shape[b] == ~0u) continue;
            const Grid gb = grid_of(b);
            for (u32 dir = 0; dir < 6; ++dir) {
                const bool want = !merged_face_occludes(ga, gb, dir);
                ++(want ? open : shut);
                if (fluid_passes(static_cast<BlockState>(of_shape[a]), static_cast<BlockState>(of_shape[b]), dir) == want)
                    continue;
                if (++differ <= 8)
                    ADD_FAILURE() << block_name(block_of(static_cast<BlockState>(of_shape[a]))) << " (state " << of_shape[a]
                                  << ") -> " << block_name(block_of(static_cast<BlockState>(of_shape[b]))) << " (state "
                                  << of_shape[b] << "), direction " << dir << ": Shapes.mergedFaceOccludes says "
                                  << (want ? "open" : "shut");
            }
        }
    }
    EXPECT_EQ(differ, 0u);
    EXPECT_GT(open, 50000u);  // (both answers are there in numbers: the comparison compared something)
    EXPECT_GT(shut, 50000u);

    // What the method says in so many words: nothing passes a full block, either way; everything
    // passes where neither block has a shape.
    const BlockState stone = blk::kStone, torch = state_of("minecraft:torch");
    for (u32 dir = 0; dir < 6; ++dir) {
        EXPECT_FALSE(fluid_passes(stone, kAir, dir));
        EXPECT_FALSE(fluid_passes(kAir, stone, dir));
        EXPECT_TRUE(fluid_passes(kAir, kAir, dir));
        EXPECT_TRUE(fluid_passes(kAir, torch, dir));
        EXPECT_TRUE(fluid_passes(blk::kWater, kAir, dir));
    }
    // A slab: its whole face stops what comes at it; its half faces do not; from its open side
    // nothing is in the way.
    const BlockState bottom = state_of("minecraft:oak_slab"), top = state_of("minecraft:oak_slab[type=top]");
    EXPECT_TRUE(fluid_passes(kAir, bottom, kDown));   // onto a bottom slab: its top is half way down the cell
    EXPECT_FALSE(fluid_passes(kAir, top, kDown));     // onto a top slab: it fills the face
    EXPECT_FALSE(fluid_passes(bottom, kAir, kDown));  // out of a bottom slab's cell, downwards: its own floor
    EXPECT_TRUE(fluid_passes(top, kAir, kDown));      // out of a top slab's cell: nothing of it is at the bottom
    EXPECT_TRUE(fluid_passes(kAir, bottom, kEast));
    EXPECT_TRUE(fluid_passes(bottom, bottom, kEast));  // two bottom slabs side by side: the upper half is open
    EXPECT_FALSE(fluid_passes(bottom, top, kEast));    // a bottom and a top slab: between them they shut the face
    // Stairs: the back is a whole face, the front a step - and the step of one and the upper
    // half of another, turned over, are a whole face again.
    const BlockState stair_east = state_of("minecraft:oak_stairs[facing=east]");
    EXPECT_FALSE(fluid_passes(stair_east, kAir, kEast));
    EXPECT_TRUE(fluid_passes(stair_east, kAir, kWest));
    EXPECT_TRUE(fluid_passes(stair_east, state_of("minecraft:oak_stairs[facing=west]"), kWest));
    EXPECT_FALSE(fluid_passes(stair_east, state_of("minecraft:oak_stairs[facing=west,half=top]"), kWest));
    // A shape that depends on where the block is, or on its block entity, is taken as mcgen
    // asked for it: a shulker box shut (a full block), powder snow and a moving piston empty.
    EXPECT_FALSE(fluid_passes(kAir, state_of("minecraft:shulker_box"), kEast));
    EXPECT_TRUE(fluid_passes(kAir, state_of("minecraft:powder_snow"), kEast));
    EXPECT_TRUE(fluid_passes(kAir, state_of("minecraft:moving_piston"), kDown));
}

TEST(Fluids, ACellBecomesWhatStandsAroundIt) {
    Land land;
    const Dimension& d = land.dim;
    const FluidRules r = water_rules(true), dry = water_rules(false);
    const BlockState air = kAir;
    // Beside one source: one level less. Beside nothing: nothing.
    land.put(0, kY, 0, "minecraft:water");
    EXPECT_EQ(new_liquid(d, r, 1, kY, 0, air), flowing(7));
    EXPECT_EQ(new_liquid(d, r, 2, kY, 0, air), 0);
    land.put(1, kY, 0, "minecraft:water[level=1]");
    EXPECT_EQ(new_liquid(d, r, 2, kY, 0, air), flowing(6));
    land.put(1, kY, 0, "minecraft:water[level=7]");
    EXPECT_EQ(new_liquid(d, r, 2, kY, 0, air), 0);  // one level left gives none away
    // Between two sources, over solid ground or a third source: a source - if sources convert.
    land.put(1, kY, 0, "minecraft:air");
    land.put(2, kY, 0, "minecraft:water");
    EXPECT_EQ(new_liquid(d, r, 1, kY, 0, air), source());
    EXPECT_EQ(new_liquid(d, dry, 1, kY, 0, air), flowing(7));
    land.put(1, kFloor, 0, "minecraft:air");  // over a hole: not solid, no source
    EXPECT_EQ(new_liquid(d, r, 1, kY, 0, air), flowing(7));
    land.put(1, kFloor, 0, "minecraft:water");
    EXPECT_EQ(new_liquid(d, r, 1, kY, 0, air), source());
    land.put(1, kFloor, 0, "minecraft:water[level=1]");  // flowing water below is neither
    EXPECT_EQ(new_liquid(d, r, 1, kY, 0, air), flowing(7));
    land.put(1, kFloor, 0, "minecraft:oak_slab[type=top]");  // BlockState.isSolid(): a slab is
    EXPECT_EQ(new_liquid(d, r, 1, kY, 0, air), source());
    land.put(1, kFloor, 0, "minecraft:oak_fence");  // ...and a fence, by its height
    EXPECT_EQ(new_liquid(d, r, 1, kY, 0, air), source());
    land.put(1, kFloor, 0, "minecraft:white_carpet");  // ...a carpet is not
    EXPECT_EQ(new_liquid(d, r, 1, kY, 0, air), flowing(7));
    // Under the fluid: a falling column, whatever is beside it.
    land.put(5, kY + 1, 5, "minecraft:water[level=3]");
    EXPECT_EQ(new_liquid(d, r, 5, kY, 5, air), flowing(8, true));
    land.put(5, kY + 1, 5, "minecraft:oak_slab[waterlogged=true]");  // the water of a bottom slab does not get out below
    EXPECT_EQ(new_liquid(d, r, 5, kY, 5, air), 0);
    land.put(5, kY + 1, 5, "minecraft:oak_slab[type=top,waterlogged=true]");  // ...that of a top slab does
    EXPECT_EQ(new_liquid(d, r, 5, kY, 5, air), flowing(8, true));
    // Lava beside the cell is no water; a wall between the cell and the water is in the way.
    land.put(8, kY, 8, "minecraft:lava");
    EXPECT_EQ(new_liquid(d, r, 9, kY, 8, air), 0);
    land.put(8, kY, 8, "minecraft:water");
    EXPECT_EQ(new_liquid(d, r, 9, kY, 8, state_of("minecraft:oak_stairs[facing=east]")), flowing(7));  // its open side
    EXPECT_EQ(new_liquid(d, r, 9, kY, 8, state_of("minecraft:oak_stairs[facing=west]")), 0);           // its back

    // FlowingFluid.sourceNeighborCount, isWaterHole, canMaybePassThrough, canBeReplacedWith
    EXPECT_EQ(source_neighbor_count(d, r, 1, kY, 0), 2u);
    EXPECT_EQ(source_neighbor_count(d, r, 9, kY, 8), 1u);
    EXPECT_EQ(source_neighbor_count(d, r, 3, kY, 3), 0u);
    const BlockState water = blk::kWater, stone = blk::kStone, torch = state_of("minecraft:torch");
    EXPECT_TRUE(water_hole(r, water, air));
    EXPECT_TRUE(water_hole(r, water, torch));                               // what is washed away is no floor
    EXPECT_TRUE(water_hole(r, water, static_cast<BlockState>(water + 3)));  // more of the same below
    EXPECT_TRUE(water_hole(r, water, water));
    EXPECT_FALSE(water_hole(r, water, stone));
    EXPECT_FALSE(
        water_hole(r, water, state_of("minecraft:oak_slab")));      // a block that holds water by itself takes none that flows
    EXPECT_TRUE(water_hole(r, water, state_of("minecraft:lava")));  // (lava gives way to a fluid, by its tag)
    EXPECT_TRUE(can_maybe_pass_through(r, water, kEast, air));
    EXPECT_TRUE(can_maybe_pass_through(r, water, kEast, torch));
    EXPECT_TRUE(can_maybe_pass_through(r, water, kEast, static_cast<BlockState>(water + 1)));
    EXPECT_FALSE(can_maybe_pass_through(r, water, kEast, water));  // a source of its own is there
    EXPECT_FALSE(can_maybe_pass_through(r, water, kEast, stone));
    EXPECT_FALSE(can_maybe_pass_through(r, water, kEast, state_of("minecraft:oak_door")));
    EXPECT_TRUE(can_maybe_pass_through(r, water, kEast, state_of("minecraft:lava")));  // (no source of water)
    // WaterFluid.canBeReplacedWith: only from above, and not by water; nothing in the way where nothing is.
    EXPECT_TRUE(can_be_replaced_with(d, 0, 0, 0, 0, fluid::kFlowingWater, kEast));
    EXPECT_FALSE(can_be_replaced_with(d, flowing(3), 0, 0, 0, fluid::kFlowingWater, kEast));
    EXPECT_FALSE(can_be_replaced_with(d, flowing(3), 0, 0, 0, fluid::kFlowingWater, kDown));
    EXPECT_TRUE(can_be_replaced_with(d, flowing(3), 0, 0, 0, fluid::kFlowingLava, kDown));
    EXPECT_FALSE(can_be_replaced_with(d, flowing(3), 0, 0, 0, fluid::kFlowingLava, kEast));
}

TEST(Fluids, LavaIsSlowerAndGivesWayToWater) {
    Land land;
    const Dimension& d = land.dim;
    const FluidRules r = lava_rules(false, false), fast = lava_rules(true, false), converting = lava_rules(false, true);
    const auto lava = [](u32 amount, bool falling = false) { return fluid::kFlowingOf[fluid::kLavaFamily][falling][amount]; };
    const FluidState lava_source = fluid::kSourceOf[fluid::kLavaFamily][0];
    EXPECT_EQ((std::tuple{r.tick_delay, r.drop_off, r.slope_distance}), (std::tuple{30, 2, 2}));
    EXPECT_EQ((std::tuple{fast.tick_delay, fast.drop_off, fast.slope_distance}), (std::tuple{10, 1, 4}));
    // Two levels a block: six beside a source, four beside that, two, and no further.
    land.put(0, kY, 0, "minecraft:lava");
    EXPECT_EQ(new_liquid(d, r, 1, kY, 0, kAir), lava(6));
    EXPECT_EQ(new_liquid(d, fast, 1, kY, 0, kAir), lava(7));
    land.put(1, kY, 0, "minecraft:lava[level=6]");  // two of it left
    EXPECT_EQ(new_liquid(d, r, 2, kY, 0, kAir), 0);
    EXPECT_EQ(new_liquid(d, fast, 2, kY, 0, kAir), lava(1));
    // Water beside the cell is no lava; two sources of lava make no third unless the rule says so.
    land.put(1, kY, 0, "minecraft:air");
    land.put(2, kY, 0, "minecraft:lava");
    EXPECT_EQ(new_liquid(d, r, 1, kY, 0, kAir), lava(6));
    EXPECT_EQ(new_liquid(d, converting, 1, kY, 0, kAir), lava_source);
    EXPECT_EQ(new_liquid(d, water_rules(true), 1, kY, 0, kAir), 0);
    // A way down within two blocks of the cell beside it, and not further: a hole three blocks
    // off is found, one four blocks off is not.
    Land hill;
    hill.put(0, kY, 0, "minecraft:lava");
    hill.put(3, kFloor, 0, "minecraft:air");
    Spread s;
    spread(hill.dim, r, 0, kY, 0, hill.at(0, kY, 0), s);
    using Ways = std::vector<std::pair<u32, FluidState>>;
    EXPECT_EQ(ways(s), (Ways{{kEast, lava(6)}}));
    hill.put(3, kFloor, 0, "minecraft:grass_block");
    hill.put(4, kFloor, 0, "minecraft:air");
    spread(hill.dim, r, 0, kY, 0, hill.at(0, kY, 0), s);
    EXPECT_EQ(ways(s), (Ways{{kNorth, lava(6)}, {kSouth, lava(6)}, {kWest, lava(6)}, {kEast, lava(6)}}));

    // FlowingFluid.getHeight: the whole cell under more of the same, else amount / 9.
    EXPECT_EQ(fluid_height(d, lava_source, 0, kY, 0), 0.8888889f);
    EXPECT_EQ(fluid_height(d, lava(4), 0, kY, 0), 0.44444445f);
    land.put(0, kY + 1, 0, "minecraft:lava[level=7]");
    EXPECT_EQ(fluid_height(d, lava(1), 0, kY, 0), 1.0f);
    land.put(0, kY + 1, 0, "minecraft:water");  // water on lava is not more of the same
    EXPECT_EQ(fluid_height(d, lava(1), 0, kY, 0), 0.11111111f);
    EXPECT_EQ(fluid_height(d, 0, 0, kY, 0), 0.0f);
    // LavaFluid.canBeReplacedWith: by water, from any side, if the lava stands at least 4/9 high
    // in its cell (LavaFluid.MIN_LEVEL_CUTOFF) - never by lava.
    for (const u32 dir : {kDown, kEast}) {
        EXPECT_TRUE(can_be_replaced_with(d, lava_source, 5, kY, 5, fluid::kFlowingWater, dir));
        EXPECT_TRUE(can_be_replaced_with(d, lava(4), 5, kY, 5, fluid::kWater, dir));
        EXPECT_FALSE(can_be_replaced_with(d, lava(3), 5, kY, 5, fluid::kFlowingWater, dir));
        EXPECT_FALSE(can_be_replaced_with(d, lava_source, 5, kY, 5, fluid::kFlowingLava, dir));
        EXPECT_FALSE(can_be_replaced_with(d, lava_source, 5, kY, 5, fluid::kEmpty, dir));
    }
    land.put(5, kY + 1, 5, "minecraft:lava[level=6]");  // low, but under more lava: the whole cell
    EXPECT_TRUE(can_be_replaced_with(d, lava(3), 5, kY, 5, fluid::kFlowingWater, kEast));
    // Water gives way to lava that comes from above, as to anything that is not water.
    EXPECT_TRUE(can_be_replaced_with(d, source(), 0, 0, 0, fluid::kFlowingLava, kDown));
}

TEST(Fluids, ItGoesToTheNearestWayDown) {
    Land land;
    const Dimension& d = land.dim;
    const FluidRules r = water_rules(true);
    const auto from = [&](i32 x, i32 z) {
        Spread s;
        spread(d, r, x, kY, z, land.at(x, kY, z), s);
        return ways(s);
    };
    using Ways = std::vector<std::pair<u32, FluidState>>;
    const Ways all = {{kNorth, flowing(7)}, {kSouth, flowing(7)}, {kWest, flowing(7)}, {kEast, flowing(7)}};
    // On flat ground: every way, in an EnumMap's order.
    land.put(0, kY, 0, "minecraft:water");
    EXPECT_EQ(from(0, 0), all);
    // A hole five blocks east is within reach (the search starts at the cell beside the fluid
    // and goes four further): only that way. One block further is not: every way again.
    land.put(5, kFloor, 0, "minecraft:air");
    EXPECT_EQ(from(0, 0), (Ways{{kEast, flowing(7)}}));
    land.put(5, kFloor, 0, "minecraft:grass_block");
    land.put(6, kFloor, 0, "minecraft:air");
    EXPECT_EQ(from(0, 0), all);
    // Two holes as near: both ways. One nearer: that one.
    land.put(6, kFloor, 0, "minecraft:grass_block");
    land.put(3, kFloor, 0, "minecraft:air");
    land.put(0, kFloor, -3, "minecraft:air");
    EXPECT_EQ(from(0, 0), (Ways{{kNorth, flowing(7)}, {kEast, flowing(7)}}));
    land.put(0, kFloor, -2, "minecraft:air");
    EXPECT_EQ(from(0, 0), (Ways{{kNorth, flowing(7)}}));
    // A wall in the way of the nearer hole: round it if that is within reach, else the other.
    land.put(0, kY, -1, "minecraft:stone");
    EXPECT_EQ(from(0, 0), (Ways{{kEast, flowing(7)}}));
    // The hole right beside the fluid: distance nothing.
    land.put(-1, kFloor, 0, "minecraft:air");
    EXPECT_EQ(from(0, 0), (Ways{{kWest, flowing(7)}}));
    // What flows gives one level less than the most the cell there has beside it - not its own
    // less one: the cell beside a source and a trickle gets the source's seven.
    Land flat;
    flat.put(0, kY, 0, "minecraft:water[level=5]");
    Spread s;
    spread(flat.dim, r, 0, kY, 0, flat.at(0, kY, 0), s);
    EXPECT_EQ(ways(s), (Ways{{kNorth, flowing(2)}, {kSouth, flowing(2)}, {kWest, flowing(2)}, {kEast, flowing(2)}}));
    flat.put(2, kY, 0, "minecraft:water");
    spread(flat.dim, r, 0, kY, 0, flat.at(0, kY, 0), s);
    EXPECT_EQ(ways(s), (Ways{{kNorth, flowing(2)}, {kSouth, flowing(2)}, {kWest, flowing(2)}, {kEast, flowing(7)}}));
    // Water that is there already gives way to none from the side: no way that way - but the
    // search has still counted it (a quirk of the method: `lowest` is set either way).
    flat.put(1, kY, 0, "minecraft:water[level=6]");
    spread(flat.dim, r, 0, kY, 0, flat.at(0, kY, 0), s);
    EXPECT_EQ(ways(s), (Ways{{kNorth, flowing(2)}, {kSouth, flowing(2)}, {kWest, flowing(2)}}));
}

// FlowingFluid.getSpread remembers the states and the holes it has seen (SpreadContext). The
// memory must change nothing: on worlds thrown together at random, every cell of water gives
// the same ways with it and without.
TEST(Fluids, TheSearchRemembersAndFindsTheSame) {
    const char* const floor[] = {"minecraft:stone",     "minecraft:stone",         "minecraft:stone", "minecraft:air",
                                 "minecraft:air",       "minecraft:water",         "minecraft:torch", "minecraft:oak_slab",
                                 "minecraft:oak_fence", "minecraft:water[level=4]"};
    const char* const level[] = {"minecraft:air",
                                 "minecraft:air",
                                 "minecraft:air",
                                 "minecraft:air",
                                 "minecraft:stone",
                                 "minecraft:water",
                                 "minecraft:water[level=2]",
                                 "minecraft:water[level=6]",
                                 "minecraft:water[level=8]",
                                 "minecraft:oak_slab",
                                 "minecraft:oak_slab[type=top,waterlogged=true]",
                                 "minecraft:oak_stairs[facing=east]",
                                 "minecraft:oak_fence[east=true,west=true]",
                                 "minecraft:glass_pane[north=true,south=true]",
                                 "minecraft:torch",
                                 "minecraft:dandelion",
                                 "minecraft:lava",
                                 "minecraft:oak_door"};
    const char* const above[] = {"minecraft:air", "minecraft:air", "minecraft:air", "minecraft:water[level=3]",
                                 "minecraft:stone"};
    std::mt19937 rng(20261123);
    u32 compared = 0, somewhere = 0;
    Land land;
    for (u32 round = 0; round < 40; ++round) {
        for (i32 x = -9; x <= 9; ++x) {
            for (i32 z = -9; z <= 9; ++z) {
                land.put(x, kFloor, z, floor[rng() % std::size(floor)]);
                land.put(x, kY, z, level[rng() % std::size(level)]);
                land.put(x, kY + 1, z, above[rng() % std::size(above)]);
            }
        }
        for (const FluidRules& r :
             {water_rules(true), water_rules(false), FluidRules{kW, fluid::kWater, fluid::kFlowingWater, true, 5, 2, 2}}) {
            for (i32 x = -3; x <= 3; ++x) {
                for (i32 z = -3; z <= 3; ++z) {
                    const BlockState state = land.at(x, kY, z);
                    if (!r.same(fluid_of(state))) continue;
                    Spread fast, slow_one;
                    spread(land.dim, r, x, kY, z, state, fast);
                    slow::spread(land.dim, r, x, kY, z, state, slow_one);
                    ++compared;
                    somewhere += fast.n != 0;
                    EXPECT_EQ(ways(fast), ways(slow_one)) << "round " << round << ", at " << x << ", " << z;
                }
            }
        }
    }
    EXPECT_GT(compared, 300u);
    EXPECT_GT(somewhere, 100u);
}

// FlowingFluid.getFlow: which way a cell of fluid runs - what carries an entity that is in it.
// (That an entity is carried as on a vanilla server is the physics recording's to say:
// tests/unit/physics_test.cpp. These are the cases, each by the rule that makes it.)
TEST(Fluids, ACellRunsTowardsWhereThereIsLess) {
    Land land;
    const auto flow = [&](i32 x, i32 y, i32 z) {
        std::array<double, 3> out{};
        fluid_flow(land.dim, fluid_of(land.at(x, y, z)), x, y, z, out.data());
        return out;
    };
    const std::array<double, 3> none{0.0, 0.0, 0.0};
    const auto east = [](const std::array<double, 3>& v) { return std::fabs(v[0] - 1.0) < 1e-12 && v[1] == 0.0 && v[2] == 0.0; };
    // A channel that runs east, walled in: more of it to the west, less to the east.
    for (i32 x = -1; x <= 5; ++x)
        for (i32 z = -1; z <= 1; ++z) land.put(x, kY, z, "minecraft:stone");
    for (i32 i = 0; i < 5; ++i) land.put(i, kY, 0, "minecraft:water[level=" + std::to_string(i) + "]");
    EXPECT_TRUE(east(flow(0, kY, 0)));  // the source: only the east has less
    EXPECT_TRUE(east(flow(2, kY, 0)));
    EXPECT_TRUE(east(flow(4, kY, 0)));  // the last: its west has more (a wall says nothing)
    // A pool of sources: nowhere - and not at its rim either, where the air beside it has ground under it.
    for (i32 x = 8; x <= 10; ++x)
        for (i32 z = 8; z <= 10; ++z) land.put(x, kY, z, "minecraft:water");
    EXPECT_EQ(flow(9, kY, 9), none);
    EXPECT_EQ(flow(8, kY, 8), none);
    // Falling beside a wall: straight down. Between walls of ice (no side of which is one): nowhere.
    for (const char* wall : {"minecraft:stone", "minecraft:ice"}) {
        for (i32 y = kY; y <= kY + 2; ++y) {
            for (const auto& [dx, dz] : {std::pair{-1, 0}, std::pair{1, 0}, std::pair{0, -1}, std::pair{0, 1}})
                land.put(-8 + dx, y, -8 + dz, wall);
            land.put(-8, y, -8, y == kY + 2 ? "minecraft:water" : "minecraft:water[level=8]");
        }
        EXPECT_EQ(flow(-8, kY, -8), std::string_view(wall) == "minecraft:ice" ? none : (std::array<double, 3>{0.0, -1.0, 0.0}))
            << wall;
        EXPECT_EQ(flow(-8, kY + 2, -8), none) << wall;  // (the source on top of it does not fall)
    }
    // Beside what neither holds a fluid up nor takes it (a ladder) with water under it: over the edge.
    land.put(-8, kY, 8, "minecraft:stone");
    land.put(-8, kY + 1, 8, "minecraft:water");
    for (const auto& [dx, dz] : {std::pair{-1, 0}, std::pair{0, -1}, std::pair{0, 1}})
        land.put(-8 + dx, kY + 1, 8 + dz, "minecraft:stone");
    land.put(-7, kY + 1, 8, "minecraft:ladder[facing=west]");
    EXPECT_EQ(flow(-8, kY + 1, 8), none);  // (nothing under the ladder yet)
    land.put(-7, kY, 8, "minecraft:water");
    EXPECT_TRUE(east(flow(-8, kY + 1, 8)));
    // Lava and water do not pull at each other.
    land.put(20, kY, 0, "minecraft:lava");
    land.put(21, kY, 0, "minecraft:water[level=3]");
    for (const auto& [dx, dz] : {std::pair{-1, 0}, std::pair{0, -1}, std::pair{0, 1}})
        land.put(20 + dx, kY, dz, "minecraft:stone");
    EXPECT_EQ(flow(20, kY, 0), none);
}

// Which sections hold a fluid is kept to the block (Chunk::fluid_sections): an entity's tick
// reads no cell where there is none, and so it has to be told of the first and of the last.
TEST(Fluids, TheSectionsThatHoldAFluidAreKnown) {
    Land land;
    const auto wet = [&](i32 x0, i32 y0, i32 z0, i32 x1, i32 y1, i32 z1) {
        return (land.dim.holds(x0, y0, z0, x1, y1, z1) & Dimension::kFluids) != 0;
    };
    EXPECT_FALSE(wet(-8, -64, -8, 8, 40, 8));
    land.put(3, 20, 3, "minecraft:water");
    land.put(4, 20, 3, "minecraft:oak_slab[waterlogged=true]");
    EXPECT_TRUE(wet(3, 20, 3, 3, 20, 3));
    EXPECT_TRUE(wet(-8, 16, -8, 8, 31, 8));
    EXPECT_FALSE(wet(-8, 32, -8, 8, 40, 8));   // the section above
    EXPECT_FALSE(wet(-8, -64, -8, 8, 15, 8));  // ...and the ones below
    EXPECT_FALSE(wet(16, 16, 0, 31, 31, 15));  // the chunk beside
    land.put(3, 20, 3, "minecraft:air");
    EXPECT_TRUE(wet(-8, 16, -8, 8, 31, 8));  // the slab's water is still there
    land.put(4, 20, 3, "minecraft:oak_slab");
    EXPECT_FALSE(wet(-8, 16, -8, 8, 31, 8));
    land.put(4, 20, 3, "minecraft:lava[level=6]");
    EXPECT_TRUE(wet(4, 20, 3, 4, 20, 3));
    land.put(4, 20, 3, "minecraft:stone");
    EXPECT_FALSE(wet(-8, -64, -8, 8, 40, 8));
}

// Whether one who stands there is in water, and whether its eyes are (world::water_around: a
// player's swimming is made of it).
TEST(Fluids, InWaterToTheEyes) {
    Land land;
    const auto around = [&](double y, float height, float eyes) {
        return water_around(land.dim, entity_box(0.5, y, 0.5, 0.6f, height), 0.5, y + static_cast<double>(eyes), 0.5);
    };
    const double feet = static_cast<double>(kY);
    EXPECT_FALSE(around(feet, 1.8f, 1.62f).in);
    land.put(0, kY, 0, "minecraft:water");
    EXPECT_TRUE(around(feet, 1.8f, 1.62f).in);
    EXPECT_FALSE(around(feet, 1.8f, 1.62f).eyes);      // up to the waist
    EXPECT_FALSE(around(feet + 0.9, 1.8f, 1.62f).in);  // a source stands eight ninths high: over it
    EXPECT_TRUE(around(feet + 0.88, 1.8f, 1.62f).in);
    EXPECT_TRUE(around(feet, 0.6f, 0.4f).eyes);         // lying in it
    EXPECT_FALSE(around(feet + 0.5, 0.6f, 0.4f).eyes);  // (its eyes over the eight ninths)
    land.put(0, kY + 1, 0, "minecraft:stone");
    EXPECT_TRUE(around(feet + 0.5, 0.6f, 0.4f).eyes) << "a source under what closes it from above fills its cell to the eye";
    land.put(0, kY + 1, 0, "minecraft:water");
    EXPECT_TRUE(around(feet, 1.8f, 1.62f).eyes);
    land.put(0, kY, 0, "minecraft:lava");
    land.put(0, kY + 1, 0, "minecraft:air");
    EXPECT_FALSE(around(feet, 1.8f, 1.62f).in);  // lava is not water
}
