// SPDX-License-Identifier: GPL-3.0-or-later
// What fills the places the terrain leaves open: net.minecraft.world.level.levelgen.Aquifer
// (Aquifer.NoiseBasedAquifer) of the 26.3 jar.
//
// The world is cut into cells of 16 x 12 x 16 blocks, each with one point somewhere in it (drawn
// from the cell's position) and a fluid level of its own: the sea's near the surface, a local
// one - or none - further down, lava in places. A block that the density leaves open takes the
// fluid of the nearest point; between two points whose levels differ the aquifer may make a
// wall instead, of the terrain's own block. Where water borders something it would flow into,
// the block is marked, and the game lets it flow once (Aquifer.shouldScheduleFluidUpdate).
//
// Everything an aquifer asks the density functions goes through the chunk's own context
// (Sampler), in the order vanilla asks: what a cache answers depends on it.
#pragma once

#include <vector>

#include "worldgen/density.hpp"

namespace worldgen {

// Aquifer.FluidStatus: a fluid up to (not including) a level. `fluid` is a block state.
struct FluidStatus {
    i32 level;
    u16 fluid;
    bool operator==(const FluidStatus&) const = default;
    constexpr u16 at(i32 y) const { return y < level ? fluid : 0; }  // (0: air)
};

class Aquifer {
  public:
    static constexpr u32 kSolid = 0xFFFFFFFFu;  // computeSubstance's null: not a fluid's place - the terrain's block

    // `random`: RandomState.getOrCreateRandomFactory("minecraft:aquifer"). `volume`: the blocks
    // this aquifer will be asked about (a chunk's). The constructor asks for the surface level
    // around the volume: the first thing a chunk's context is asked.
    Aquifer(Sampler& sampler, const XoroshiroPositional& random, const Volume& volume, i32 sea_level, u16 water, u16 lava);

    // Aquifer.computeSubstance: the block state of an open place - or kSolid.
    u32 compute_substance(i32 x, i32 y, i32 z, double density);
    // ...and whether what it just answered should be let flow.
    bool should_schedule_fluid_update() const { return should_schedule_; }

  private:
    struct Location {
        i32 x, y, z;
        bool known;
    };
    struct Status {
        FluidStatus status;
        bool known;
    };

    FluidStatus global_fluid(i32 y) const { return y < lava_below_ ? FluidStatus{-54, lava_} : FluidStatus{sea_level_, water_}; }
    u32 index(i32 gx, i32 gy, i32 gz) const {
        return static_cast<u32>(((gy - min_gy_) * size_gz_ + (gz - min_gz_)) * size_gx_ + (gx - min_gx_));
    }
    i32 surface_level(i32 x, i32 z);
    i32 max_surface_level(i32 min_x, i32 min_z, i32 max_x, i32 max_z);
    const FluidStatus& status(u32 index);
    FluidStatus compute_fluid(i32 x, i32 y, i32 z);
    i32 compute_surface_level(i32 x, i32 y, i32 z, const FluidStatus& global, i32 lowest_surface, bool center_under_global);
    i32 randomized_surface_level(i32 x, i32 y, i32 z, i32 lowest_surface);
    u16 compute_fluid_type(i32 x, i32 y, i32 z, const FluidStatus& global, i32 level);
    double calculate_pressure(i32 x, i32 y, i32 z, double& barrier_noise, const FluidStatus& a, const FluidStatus& b);

    Sampler& sampler_;
    XoroshiroPositional random_;
    i32 sea_level_, lava_below_;
    u16 water_, lava_;
    bool should_schedule_ = false;
    i32 skip_above_y_ = 0;
    i32 min_gx_, min_gy_, min_gz_, size_gx_, size_gz_;
    // Surface levels by quart column (Long2IntMap in vanilla): all an aquifer can ask for lie
    // within three chunks of its cells.
    i32 surface_qx_, surface_qz_, surface_w_, surface_h_;
    std::vector<Location> locations_;
    std::vector<Status> statuses_;
    std::vector<i32> surface_;
};

}  // namespace worldgen
