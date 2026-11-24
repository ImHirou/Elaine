// SPDX-License-Identifier: GPL-3.0-or-later
// Chunk generators. A generator only fills blocks and biomes; everything derived from blocks
// (heightmaps, sky sources, light) is computed by the Dimension, so a generator cannot get it wrong.
#pragma once

#include <vector>

#include "world/chunk.hpp"

namespace world {

struct LoadedChunk;
struct GenChunk;

class ChunkGenerator {
  public:
    // Fill c.sections[0 .. c.sections_count) and c.biome[]. Called once per chunk; one virtual call
    // per chunk is noise next to the work behind it. With workers it is called on their threads,
    // for several chunks at once: a generator keeps nothing that two calls would share.
    virtual void generate(Chunk&, SectionAllocator&) {}
    // For a generator that has more to say of a chunk than its blocks and one biome a section -
    // a biome a cell, which lives in the chunk's arena: the whole of what a worker makes. True:
    // it made the chunk, and the above is not asked. (Every virtual here is inline: the tests
    // derive from this class with RTTI on, the server is built without.)
    virtual bool generate_whole(LoadedChunk&) { return false; }
    // A generator whose chunks are decorated makes a chunk in stages (world/gen_chunk.hpp): what
    // a chunk is of itself, read from no other chunk - then, area by area, what the features of the
    // chunks put into each other.
    virtual bool staged() const { return false; }
    // The chunk at (cx, cz) as it is before any feature: version 0.
    virtual void make_base(i32 /*cx*/, i32 /*cz*/, GenChunk& /*out*/) {}
    virtual i32 sea_level() const = 0;     // ChunkGenerator.getSeaLevel(): sent in the login packet
    virtual i32 spawn_height() const = 0;  // ChunkGenerator.getSpawnHeight(): first free y at spawn
    virtual bool is_flat() const = 0;      // "isFlat" in the login packet (changes the client's horizon)

  protected:
    ~ChunkGenerator() = default;
};

// Vanilla's flat world (FlatLevelSource): the same stack of layers in every column, starting at the
// bottom of the world. The default is FlatLevelGeneratorSettings.getDefault() from the 26.3 jar -
// bedrock, 2 dirt, grass block, biome plains - without its villages and strongholds, i.e. the
// world a vanilla server makes with level-type=flat and generate-structures=false.
class FlatGenerator final : public ChunkGenerator {
  public:
    struct Layer {
        BlockState state;
        u32 height;
    };
    // `min_y`/`sections` describe the dimension it generates for.
    FlatGenerator(i32 min_y, u32 sections, std::vector<Layer> layers, u16 biome);
    static std::vector<Layer> default_layers();

    void generate(Chunk& c, SectionAllocator& a) override;
    i32 sea_level() const override { return -63; }  // FlatLevelSource.getSeaLevel()
    i32 spawn_height() const override { return min_y_ + static_cast<i32>(std::min<usize>(column_.size(), sections_ * 16)); }
    bool is_flat() const override { return true; }

  private:
    i32 min_y_;
    u32 sections_;
    u16 biome_;
    std::vector<BlockState> column_;  // one state per y, from the bottom up (FlatLevelGeneratorSettings.getLayers)
};

// Nothing: air from the bottom to the top. What stands in for the terrain of a world that was
// imported from vanilla and whose generator this server does not have - beyond what was explored
// there, the world ends.
class EmptyGenerator final : public ChunkGenerator {
  public:
    EmptyGenerator(i32 sea_level, u16 biome) : sea_level_(sea_level), biome_(biome) {}
    void generate(Chunk& c, SectionAllocator&) override {
        for (u32 si = 0; si < c.sections_count; ++si) c.biome[si] = biome_;
    }
    i32 sea_level() const override { return sea_level_; }
    i32 spawn_height() const override { return sea_level_ + 1; }
    bool is_flat() const override { return false; }

  private:
    i32 sea_level_;
    u16 biome_;
};

}  // namespace world
