// SPDX-License-Identifier: GPL-3.0-or-later
// The world a server runs: where it is kept and what it is made of.
//
//   <level-name>/                      the world's directory
//     session.lock                     held (fcntl) while a server runs the world: vanilla's file, and
//                                      vanilla's kind of lock - neither takes a world the other has
//     world.json                       what this server knows of the world: the format and data
//                                      version it was saved by, its seed, its generator, its spawn -
//                                      and what time it is: written again with every autosave
//     chunks/minecraft/overworld/      region files of this server's own format (world/region.hpp)
//     playerstate/<uuid>.bin           what is kept of a player (server/player_data.hpp)
//
// A directory that holds a vanilla world (level.dat) and no world.json is imported: every chunk of
// its region files is written into ours, once, before the server starts to listen
// (server/import.cpp). The vanilla files are not changed and not read again.
//
// An empty level-name is a world that is not kept: nothing is written, and no chunk ever leaves
// memory.
#pragma once

#include <string>
#include <vector>

#include "base/workers.hpp"
#include "server/clock.hpp"
#include "server/settings.hpp"
#include "world/generator.hpp"

namespace server {

struct WorldInfo {
    std::string dir;        // empty: nothing is kept
    std::string chunk_dir;  // the overworld's regions; empty with `dir`
    // The generator. Flat: `layers` from the bottom of the world up, in `biome`. Noise: the
    // overworld's terrain, from the seed (worldgen::OverworldGenerator). Neither: nothing
    // (world::EmptyGenerator) - what lies beyond the chunks of an imported vanilla world.
    bool flat = true;
    bool noise = false;
    std::vector<world::FlatGenerator::Layer> layers = world::FlatGenerator::default_layers();
    u16 biome = 0;
    i32 sea_level = -63;  // ChunkGenerator.getSeaLevel(): -63 for a flat world, 63 for the overworld's noise
    // WorldOptions.seed: what everything a generator draws comes from, and - hashed - what a
    // client zooms biomes with. A world has one from the day it is made: `level-seed`, or a
    // random one; an imported world has vanilla's.
    i64 seed = 0;
    // Where players appear: a block position, and the middle of that block is where they stand.
    // Without one it is (8, top of the generator's ground, 8).
    bool has_spawn = false;
    i32 spawn[3] = {0, 0, 0};
    bool imported = false;  // made from a vanilla world
    // What time it was when the world was last saved: its count of ticks and its clocks.
    WorldTime time;
    int lock_fd = -1;

    WorldInfo() = default;
    WorldInfo(const WorldInfo&) = delete;
    WorldInfo& operator=(const WorldInfo&) = delete;
    ~WorldInfo();
};

// world.json as text: what `info` says of the world, at the time `now`.
std::string world_json(const WorldInfo& info, const WorldTime& now);

// Open the world `s.level_name` names: lock it, read what is known of it - or import the vanilla
// world that is there, or begin a new one. False with `err` when the world cannot be used: it is
// in use, was saved by another version, or its import failed.
bool open_world(const Settings& s, base::Workers& workers, WorldInfo& out, std::string& err);

}  // namespace server
