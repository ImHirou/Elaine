// Chunk generators. A generator only fills blocks and biomes; everything derived from blocks
// (heightmaps, sky sources, light) is computed by the Dimension, so a generator cannot get it wrong.
#pragma once

#include <vector>

#include "world/chunk.hpp"

namespace world {

class ChunkGenerator {
  public:
    // Fill c.sections[0 .. c.sections_count) and c.biome[]. Called once per chunk; one virtual call
    // per chunk is noise next to the work behind it.
    virtual void generate(Chunk& c, SectionAllocator& a) = 0;
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

}  // namespace world
