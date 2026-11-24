// SPDX-License-Identifier: GPL-3.0-or-later
#include "worldgen/material.hpp"

#include <limits>

#include "world/block.hpp"

namespace worldgen {

namespace wg = proto::gen::worldgen;
namespace sf = wg::surface;
using wg::ConditionOp;
using wg::RuleOp;

namespace {

constexpr u32 kNothing = 0xFFFFFFFFu;  // a rule that gives no block (RuleEvaluator.tryApply: null)
constexpr i32 kNoWater = std::numeric_limits<i32>::min();
constexpr i32 kWayBelowMinY = -32512;  // DimensionType.WAY_BELOW_MIN_Y
constexpr i32 kMinY = settings::kMinY, kMaxY = settings::kMinY + settings::kHeight - 1;

bool is_air(u16 state) { return world::is_air(state); }
bool has_fluid(u16 state) { return world::flags_of(state) & world::state::kHasFluid; }
// MaterialSystem.isStone: what the surface is built on and of - neither air nor a fluid.
bool is_stone(u16 state) { return !is_air(state) && !has_fluid(state); }
bool is_water(u16 state) { return world::block_of(state) == world::block_of(settings::kDefaultFluid); }

// Mth.map: where `value` lies between two numbers, taken between two others.
double map(double value, double from_min, double from_max, double to_min, double to_max) {
    return lerp((value - from_min) / (from_max - from_min), to_min, to_max);
}
// Math.round(float)
i32 java_round(float v) { return static_cast<i32>(std::floor(static_cast<double>(v) + 0.5)); }

// Biome's own noises (Biome.FROZEN_TEMPERATURE_NOISE, BIOME_INFO_NOISE): the same in every world.
struct BiomeNoises {
    Simplex frozen[3];
    Simplex info;

    BiomeNoises() {
        WorldgenRandom<LegacyRandom> a{LegacyRandom(3456)};
        for (Simplex& s : frozen) s.seed(a);
        WorldgenRandom<LegacyRandom> b{LegacyRandom(2345)};
        info.seed(b);
    }
    // NoiseStack.get(x, y) of the three: at 1, 1/2 and 1/4 of the frequency.
    float frozen_temperature(double x, double z) const {
        float value = 0.0f;
        value = value + 0.14285715f * frozen[0].get(x * 1.0, z * 1.0);
        value = value + 0.2857143f * frozen[1].get(x * 0.5, z * 0.5);
        value = value + 0.5714286f * frozen[2].get(x * 0.25, z * 0.25);
        return value;
    }
};
const BiomeNoises& biome_noises() {
    static const BiomeNoises noises;
    return noises;
}

// Biome.shouldMeltFrozenOceanIcebergSlightly at the sea's level: is the biome's temperature
// there above 0.1? A frozen biome (TemperatureModifier.FROZEN) is 0.2 in patches, whatever it is else.
bool melts_iceberg_slightly(u16 biome, i32 x, i32 z) {
    const wg::BiomeTemperature& b = wg::kBiomeTemperature[biome];
    float temperature = b.temperature;
    if (b.frozen) {
        const BiomeNoises& n = biome_noises();
        const double large =
            static_cast<double>(n.frozen_temperature(static_cast<double>(x) * 0.05, static_cast<double>(z) * 0.05) * 7.0f);
        const double edge = static_cast<double>(n.info.get(static_cast<double>(x) * 0.2, static_cast<double>(z) * 0.2));
        if (large + edge < 0.3) {
            const double small = static_cast<double>(n.info.get(static_cast<double>(x) * 0.09, static_cast<double>(z) * 0.09));
            if (small < 0.8) temperature = 0.2f;
        }
    }
    return temperature > 0.1f;
}

// MaterialRuleContext, with the rule compiled over it: where the surface is being asked about,
// and what was worked out for this column or this block already.
struct Context {
    const Overworld& world;
    Sampler& sampler;
    // Where the biome of a block is read: the cells the chunks were filled with (buildSurface), or
    // the climate asked anew (topMaterial, for a carver).
    const BiomeGrid* biomes;
    CarverBiomes* carver_biomes;
    Volume expected;  // the blocks the rule will be asked about: where an ore vein's densities are sampled ahead
    // Stamps: a thing remembered for a column or for a block is good while its stamp is the current one.
    u64 stamp_xz = 1, stamp_y = 1;
    i32 x = 0, y = 0, z = 0;
    i32 gradient_x = 0, gradient_z = 0, surface_depth = 0;
    i32 water_height = kNoWater, stone_below = 0, stone_above = 0;
    i32 biome = -1;  // the block's, once asked
    u64 secondary_stamp = 0, min_surface_stamp = 0;
    double secondary = 0.0;
    i32 min_surface = 0;
    float* preliminary = nullptr;  // chunk_surface_level over the columns, once asked
    Volume preliminary_volume;
    float* vein_density[std::size(wg::kOreVeins)] = {};
    float* vein_richness[std::size(wg::kOreVeins)] = {};
    u64 condition_stamp[std::size(wg::kMaterialConditions)] = {};
    bool condition_value[std::size(wg::kMaterialConditions)] = {};
    u64 noise2_stamp[wg::noise::kCount] = {}, noise3_stamp[wg::noise::kCount] = {};
    double noise2[wg::noise::kCount], noise3[wg::noise::kCount];

    Context(const Overworld& w, Sampler& s, const BiomeGrid* b, CarverBiomes* c, const Volume& v)
        : world(w), sampler(s), biomes(b), carver_biomes(c), expected(v), preliminary_volume{v.size_x, 1, v.size_z,
                                                                                             v.min_x,  0, v.min_z} {
        compile(wg::kMaterialRoot);
    }

    // MaterialRule.compile, for what it does besides making an evaluator: an ore vein samples its
    // density and its richness over the whole volume, in the order the rules come.
    void compile(u32 rule) {
        const wg::MaterialRule& r = wg::kMaterialRules[rule];
        if (r.op == RuleOp::kSequence) {
            for (u32 i = 0; i < r.b; ++i) compile(wg::kRuleChildren[r.a + i]);
        } else if (r.op == RuleOp::kCondition) {
            compile(r.b);
        } else if (r.op == RuleOp::kOreVein) {
            const wg::OreVein& vein = wg::kOreVeins[r.a];
            vein_density[r.a] = sampler.scratch().take(expected.size());
            sampler.volume(vein.density, vein_density[r.a], expected);
            vein_richness[r.a] = sampler.scratch().take(expected.size());
            sampler.volume(vein.richness, vein_richness[r.a], expected);
        }
    }

    // MaterialRuleContext.updateXZ, updateY
    void update_xz(i32 block_x, i32 block_z, i32 gx, i32 gz) {
        ++stamp_xz;
        ++stamp_y;
        x = block_x;
        z = block_z;
        gradient_x = gx;
        gradient_z = gz;
        // MaterialSystem.getSurfaceDepth: how deep the surface's blocks go here - about three, by a
        // noise and a little by chance.
        const double noise =
            static_cast<double>(world.noises()[wg::noise::kSurface].get(static_cast<double>(x), 0.0, static_cast<double>(z)));
        XoroshiroRandom r = world.noises().random().at(x, 0, z);
        surface_depth = static_cast<i32>(noise * 2.75 + 3.0 + r.next_double() * 0.25);
    }
    void update_y(i32 above, i32 below, i32 water, i32 block_y) {
        ++stamp_y;
        biome = -1;
        y = block_y;
        water_height = water;
        stone_below = below;
        stone_above = above;
    }

    double surface_secondary() {
        if (secondary_stamp != stamp_xz) {
            secondary_stamp = stamp_xz;
            secondary = static_cast<double>(
                world.noises()[wg::noise::kSurfaceSecondary].get(static_cast<double>(x), 0.0, static_cast<double>(z)));
        }
        return secondary;
    }
    u16 block_biome() {
        if (biome < 0) {
            const QuartCell cell = zoom_cell(world.zoom_seed(), x, y, z);
            biome = biomes ? biomes->at(cell.x, cell.y, cell.z) : carver_biomes->at(cell);
        }
        return static_cast<u16>(biome);
    }
    // getMinSurfaceLevel: how far down the surface is built - the terrain's level before its
    // noise (chunk_surface_level), the surface's depth, and eight less.
    i32 min_surface_level() {
        if (min_surface_stamp != stamp_xz) {
            min_surface_stamp = stamp_xz;
            const i32 index = preliminary_volume.index_of_block(x, 0, z);
            float level;
            if (index != -1) {
                if (!preliminary) {
                    preliminary = sampler.scratch().take(preliminary_volume.size());
                    sampler.volume(wg::root::kRouterChunkSurfaceLevel, preliminary, preliminary_volume);
                }
                level = preliminary[index];
            } else {
                level = sampler.value(wg::root::kRouterChunkSurfaceLevel, x, 0, z);
            }
            min_surface = floor_int(level) + surface_depth - 8;
        }
        return min_surface;
    }
    // MaterialRuleContext.getNoiseSampler: a noise at the column, or at the block.
    double noise_2d(u32 id) {
        if (noise2_stamp[id] != stamp_xz) {
            noise2_stamp[id] = stamp_xz;
            noise2[id] = static_cast<double>(world.noises()[id].get(static_cast<double>(x), 0.0, static_cast<double>(z)));
        }
        return noise2[id];
    }
    double noise_3d(u32 id) {
        if (noise3_stamp[id] != stamp_y) {
            noise3_stamp[id] = stamp_y;
            noise3[id] = static_cast<double>(
                world.noises()[id].get(static_cast<double>(x), static_cast<double>(y), static_cast<double>(z)));
        }
        return noise3[id];
    }
    // MaterialSystem.getBand
    u16 band() {
        const i32 offset = java_round(
            world.noises()[wg::noise::kClayBandsOffset].get(static_cast<double>(x), 0.0, static_cast<double>(z)) * 4.0f);
        return world.clay_bands()[(y + offset + 192) % 192];
    }

    bool compute(const wg::MaterialCondition& c) {
        switch (c.op) {
            case ConditionOp::kBiome: {
                // (A biome that is nowhere around cannot be the block's: BiomeCondition.canNeverMatch.)
                if (biomes) {
                    bool possible = false;
                    for (i32 i = 0; i < c.i1; ++i) possible |= biomes->has(wg::kConditionBiomes[c.i0 + i]);
                    if (!possible) return false;
                }
                const u16 b = block_biome();
                for (i32 i = 0; i < c.i1; ++i)
                    if (wg::kConditionBiomes[c.i0 + i] == b) return true;
                return false;
            }
            case ConditionOp::kNoiseThreshold: {
                const double value = c.flag ? noise_3d(static_cast<u32>(c.i0)) : noise_2d(static_cast<u32>(c.i0));
                return value >= c.d0 && value <= c.d1;
            }
            case ConditionOp::kVerticalGradient: {
                if (y <= c.i0) return true;
                if (y >= c.i1) return false;
                const double probability =
                    map(static_cast<double>(y), static_cast<double>(c.i0), static_cast<double>(c.i1), 1.0, 0.0);
                XoroshiroRandom r = world.random(static_cast<u32>(c.i2)).at(x, y, z);
                return static_cast<double>(r.next_float()) < probability;
            }
            case ConditionOp::kYAbove: return y + (c.flag ? stone_above : 0) >= c.i0 + surface_depth * c.i1;
            case ConditionOp::kWater:
                return water_height == kNoWater || y + (c.flag ? stone_above : 0) >= water_height + c.i0 + surface_depth * c.i1;
            case ConditionOp::kSteep: return gradient_x <= -4 || gradient_z >= 4;
            case ConditionOp::kNot: return !test(static_cast<u32>(c.i0));
            case ConditionOp::kHole: return surface_depth <= 0;
            case ConditionOp::kAbovePreliminarySurface: return y >= min_surface_level();
            case ConditionOp::kStoneDepth: {
                const i32 stone = (c.flag & 2) ? stone_below : stone_above;
                const i32 depth = (c.flag & 1) ? surface_depth : 0;
                const i32 secondary_depth =
                    c.i1 == 0 ? 0 : static_cast<i32>(map(surface_secondary(), -1.0, 1.0, 0.0, static_cast<double>(c.i1)));
                return stone <= 1 + c.i0 + depth + secondary_depth;
            }
        }
        return false;
    }
    // A condition's answer: asked once a column (LazyXZCondition), once a block (LazyYCondition),
    // or every time, as vanilla's class of it is.
    bool test(u32 id) {
        const wg::MaterialCondition& c = wg::kMaterialConditions[id];
        u64 stamp;
        switch (c.op) {
            case ConditionOp::kSteep:
            case ConditionOp::kHole: stamp = stamp_xz; break;
            case ConditionOp::kBiome:
            case ConditionOp::kVerticalGradient:
            case ConditionOp::kYAbove:
            case ConditionOp::kWater:
            case ConditionOp::kStoneDepth: stamp = stamp_y; break;
            default: return compute(c);
        }
        if (condition_stamp[id] != stamp) {
            condition_stamp[id] = stamp;
            condition_value[id] = compute(c);
        }
        return condition_value[id];
    }

    // MaterialRuleContext.getDensitiesInChunk, sampled ahead: out of the buffer where the block is in it.
    float ahead(const float* buffer, u32 node) {
        const i32 index = expected.index_of_block(x, y, z);
        return index == -1 ? sampler.value(node, x, y, z) : buffer[index];
    }

    // OreVeinRule: where the vein's density is above nothing a block is, by chance, of the vein -
    // its ore (now and then raw) where the richness and the gap allow, else its filler.
    u32 ore_vein(u32 which) {
        const wg::OreVein& vein = wg::kOreVeins[which];
        const float density = ahead(vein_density[which], vein.density);
        if (!(density > 0.0f)) return kNothing;
        XoroshiroRandom r = world.random(wg::random_name::kOre).at(x, y, z);
        if (r.next_float() > density) return kNothing;
        const float richness = ahead(vein_richness[which], vein.richness);
        if (r.next_float() < richness && sampler.value(vein.filler_gap, x, y, z) < 0.0f)
            return r.next_float() < vein.raw_ore_chance ? vein.raw_ore : vein.ore;
        return vein.filler;
    }

    // RuleEvaluator.tryApply
    u32 apply(u32 rule) {
        const wg::MaterialRule& r = wg::kMaterialRules[rule];
        switch (r.op) {
            case RuleOp::kSequence:
                for (u32 i = 0; i < r.b; ++i) {
                    const u32 state = apply(wg::kRuleChildren[r.a + i]);
                    if (state != kNothing) return state;
                }
                return kNothing;
            case RuleOp::kCondition: return test(r.a) ? apply(r.b) : kNothing;
            case RuleOp::kBlock: return r.a;
            case RuleOp::kBandlands: return band();
            case RuleOp::kOreVein: return ore_vein(r.a);
        }
        return kNothing;
    }
};

// MaterialSystem.buildSurface's column: reading is of the chunk, air beyond it; a block that is
// set and holds a fluid is marked to be let flow.
struct Column {
    Terrain& terrain;
    i32 x, z;  // within the chunk

    u16 get(i32 y) const { return terrain.block_or_air(x, y, z); }
    void set(i32 y, u16 state) const {
        if (y < kMinY || y > kMaxY) return;
        terrain.set_block(x, y, z, state);
        if (has_fluid(state)) terrain.mark_post_process(x, y, z);
    }
};

// The surface's slope, off the heightmap as it stands (MaterialSystem.getSurfaceGradientX / Z).
i32 gradient_x(const Terrain& t, i32 x, i32 z) {
    return t.world_surface_wg[std::min(x + 1, 15) + z * 16] - t.world_surface_wg[std::max(x - 1, 0) + z * 16];
}
i32 gradient_z(const Terrain& t, i32 x, i32 z) {
    return t.world_surface_wg[x + std::min(z + 1, 15) * 16] - t.world_surface_wg[x + std::max(z - 1, 0) * 16];
}

// erodedBadlandsExtension: pillars of the terrain's block on top of the eroded badlands.
void eroded_badlands_extension(const Noises& noises, const Column& column, i32 block_x, i32 block_z, i32 height) {
    const double bx = static_cast<double>(block_x), bz = static_cast<double>(block_z);
    const double buffer = java_min(std::fabs(static_cast<double>(noises[wg::noise::kBadlandsSurface].get(bx, 0.0, bz)) * 8.25),
                                   static_cast<double>(noises[wg::noise::kBadlandsPillar].get(bx * 0.2, 0.0, bz * 0.2) * 15.0f));
    if (!(buffer > 0.0)) return;
    const double floor_ =
        std::fabs(static_cast<double>(noises[wg::noise::kBadlandsPillarRoof].get(bx * 0.75, 0.0, bz * 0.75)) * 1.5);
    const double top = 64.0 + java_min(buffer * buffer * 2.5, std::ceil(floor_ * 50.0) + 24.0);
    const i32 start = floor_int(top);
    if (height > start) return;
    for (i32 y = start; y >= kMinY; --y) {
        const u16 old = column.get(y);
        if (world::block_of(old) == world::block_of(settings::kDefaultBlock)) break;
        if (is_water(old)) return;
    }
    for (i32 y = start; y >= kMinY && is_air(column.get(y)); --y) column.set(y, settings::kDefaultBlock);
}

// frozenOceanExtension: icebergs - packed ice with a cap of snow - on and in the frozen oceans.
void frozen_ocean_extension(const Overworld& world, i32 min_surface_level, u16 biome, const Column& column, i32 block_x,
                            i32 block_z, i32 height) {
    const Noises& noises = world.noises();
    const i32 sea = settings::kSeaLevel;
    const double bx = static_cast<double>(block_x), bz = static_cast<double>(block_z);
    const double iceberg =
        java_min(std::fabs(static_cast<double>(noises[wg::noise::kIcebergSurface].get(bx, 0.0, bz)) * 8.25),
                 static_cast<double>(noises[wg::noise::kIcebergPillar].get(bx * 1.28, 0.0, bz * 1.28) * 15.0f));
    if (!(iceberg > 1.8)) return;
    const double roof =
        std::fabs(static_cast<double>(noises[wg::noise::kIcebergPillarRoof].get(bx * 1.17, 0.0, bz * 1.17)) * 1.5);
    double top = java_min(iceberg * iceberg * 1.2, std::ceil(roof * 40.0) + 14.0);
    if (melts_iceberg_slightly(biome, block_x, block_z)) top = top - 2.0;
    if (!(top > 2.0)) return;
    const double bottom = static_cast<double>(sea) - top - 7.0;
    top = top + static_cast<double>(sea);
    XoroshiroRandom r = noises.random().at(block_x, 0, block_z);
    const i32 max_snow_depth = 2 + r.next_int(4);
    const i32 min_snow_height = sea + 18 + r.next_int(10);
    i32 snow_depth = 0;
    for (i32 y = std::max(height, static_cast<i32>(top) + 1); y >= min_surface_level; --y) {
        const u16 old = column.get(y);
        const bool ice = (is_air(old) && y < static_cast<i32>(top) && r.next_double() > 0.01) ||
                         (is_water(old) && y > static_cast<i32>(bottom) && y < sea && r.next_double() > 0.15);
        if (!ice) continue;
        if (snow_depth <= max_snow_depth && y > min_snow_height) {
            column.set(y, sf::kSnowBlock);
            ++snow_depth;
        } else {
            column.set(y, sf::kPackedIce);
        }
    }
}

}  // namespace

void build_surface(const Overworld& world, NoiseChunk& chunk, const BiomeGrid& biomes, Terrain& terrain) {
    Sampler& sampler = chunk.sampler();
    Scratch& scratch = sampler.scratch();
    const Scratch::Mark mark = scratch.mark();
    // The rule is asked about the chunk up to its highest section that holds anything.
    const Volume& full = chunk.volume();
    const i32 top_block = (terrain.highest_filled_section() + (kMinY >> 4)) * 16 + 15;
    const Volume narrowed{full.size_x, std::max(top_block - full.min_y + 1, 1), full.size_z, full.min_x, full.min_y, full.min_z};
    Context context(world, sampler, &biomes, nullptr, narrowed);
    const i32 min_x = terrain.chunk_x * 16, min_z = terrain.chunk_z * 16;
    for (i32 x = 0; x < 16; ++x) {
        for (i32 z = 0; z < 16; ++z) {
            const i32 block_x = min_x + x, block_z = min_z + z;
            const Column column{terrain, x, z};
            const i32 starting_height = terrain.world_surface_wg[x + z * 16];  // (getHeight(...) + 1)
            const u16 surface_biome = zoomed_biome(biomes, world.zoom_seed(), block_x, starting_height, block_z);
            if (surface_biome == sf::kErodedBadlands)
                eroded_badlands_extension(world.noises(), column, block_x, block_z, starting_height);
            const i32 height = terrain.world_surface_wg[x + z * 16];
            context.update_xz(block_x, block_z, gradient_x(terrain, x, z), gradient_z(terrain, x, z));
            i32 stone_above = 0, water_height = kNoWater, next_ceiling_stone = std::numeric_limits<i32>::max();
            for (i32 y = height; y >= kMinY; --y) {
                const u16 old = column.get(y);
                if (is_air(old)) {
                    stone_above = 0;
                    water_height = kNoWater;
                    continue;
                }
                if (has_fluid(old)) {
                    if (water_height == kNoWater) water_height = y + 1;
                    continue;
                }
                // How far down the stone goes from here: to the next block below that is none.
                if (next_ceiling_stone >= y) {
                    next_ceiling_stone = kWayBelowMinY;
                    for (i32 below = y - 1; below >= kMinY - 1; --below) {
                        if (!is_stone(column.get(below))) {
                            next_ceiling_stone = below + 1;
                            break;
                        }
                    }
                }
                ++stone_above;
                context.update_y(stone_above, y - next_ceiling_stone + 1, water_height, y);
                if (y > kMaxY) continue;
                const u32 state = context.apply(wg::kMaterialRoot);
                if (state != kNothing) column.set(y, static_cast<u16>(state));
            }
            if (surface_biome == sf::kFrozenOcean || surface_biome == sf::kDeepFrozenOcean)
                frozen_ocean_extension(world, context.min_surface_level(), surface_biome, column, block_x, block_z,
                                       starting_height);
        }
    }
    scratch.release(mark);
}

bool top_material(const Overworld& world, NoiseChunk& chunk, CarverBiomes& biomes, const Terrain& terrain, i32 x, i32 y, i32 z,
                  bool under_fluid, u16& out) {
    Sampler& sampler = chunk.sampler();
    Scratch& scratch = sampler.scratch();
    const Scratch::Mark mark = scratch.mark();
    Context context(world, sampler, nullptr, &biomes, Volume{1, 1, 1, x, y, z});
    context.update_xz(x, z, gradient_x(terrain, x & 15, z & 15), gradient_z(terrain, x & 15, z & 15));
    context.update_y(1, 1, under_fluid ? y + 1 : kNoWater, y);
    const u32 state = context.apply(wg::kMaterialRoot);
    scratch.release(mark);
    if (state == kNothing) return false;
    out = static_cast<u16>(state);
    return true;
}

float biome_info_noise(double x, double z) { return biome_noises().info.get(x, z); }

}  // namespace worldgen
