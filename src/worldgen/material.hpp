// SPDX-License-Identifier: GPL-3.0-or-later
// What the terrain's stone becomes: net.minecraft.world.level.levelgen.material of the 26.3 jar -
// MaterialSystem.buildSurface, the material rules and their conditions.
//
// After the density and the aquifers a chunk is stone, water, lava and air. This step goes down
// every column and asks the overworld's material rule about each block of stone: bedrock at the
// bottom, an ore vein, the surface of the column's biome - grass over dirt, sand over sandstone,
// snow, the bands of the badlands - and deepslate below. The rule is data (generated:
// kMaterialRules, kMaterialConditions); what its parts mean is here.
//
// Two things are added to the terrain itself on the way, by biome: the pillars of the eroded
// badlands and the icebergs of the frozen oceans.
//
// The order is vanilla's - columns x first, then z; in a column from the top - because the
// surface's slope is read off a heightmap the columns before have changed, and because an ore
// vein and the level the surface is built down to ask the chunk's context, whose caches answer by
// what they were asked before.
#pragma once

#include "worldgen/climate.hpp"
#include "worldgen/terrain.hpp"

namespace worldgen {

// MaterialSystem.buildSurface
void build_surface(const Overworld& world, NoiseChunk& chunk, const BiomeGrid& biomes, Terrain& terrain);

// MaterialSystem.topMaterial: what the rule makes of one block as the top of its column - the
// grass a carver puts back on the dirt it lays bare. False: nothing.
bool top_material(const Overworld& world, NoiseChunk& chunk, CarverBiomes& biomes, const Terrain& terrain, i32 x, i32 y, i32 z,
                  bool under_fluid, u16& out);

// Biome.BIOME_INFO_NOISE - the same in every world: what the count of some features goes by.
float biome_info_noise(double x, double z);

// Biome.warmEnoughToRain(pos, seaLevel): the biome's temperature at a block - its own, colder in
// patches where it is of the frozen kind, and the higher above the sea the colder, by a noise of
// its own - is 0.15 at least. Where it is not, water freezes and snow falls.
bool warm_enough_to_rain(u16 biome, i32 x, i32 y, i32 z);
// Biome.hasPrecipitation
bool has_precipitation(u16 biome);

}  // namespace worldgen
