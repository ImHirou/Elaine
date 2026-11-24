// SPDX-License-Identifier: GPL-3.0-or-later
// A chunk's features: vanilla's `features` chunk status - ChunkGenerator.applyBiomeDecoration, the
// placements of net.minecraft.world.level.levelgen.placement and the features of
// net.minecraft.world.level.levelgen.feature (the 26.3 jar).
//
// What a biome puts into a chunk is data: placed features - where, and how often - of features -
// what. Both are compiled into tables (PROTO_GEN_WORLDGEN_FEATURES_HPP: tools/mcgen/gen_features.py);
// what each kind of placement and each type of feature *does* is here, a function a kind, written
// from its class.
//
// The order is vanilla's, and all of it decides what is drawn from the random source:
//   - the decoration steps in order, and in a step the features of all biomes around the chunk in
//     the one order vanilla sorts them into (FeatureSorter), each from a source seeded with its
//     place in that order;
//   - a placed feature's placements one after the other, each turning a position into some: a
//     placement makes all of its positions - with every draw that takes - before the first of them
//     goes on to the next placement, and they go on in the order they were made (FeaturePlacer);
//   - the feature at every position the last placement made.
//
// Not every feature is ported: one that is not is left out (kPlaced[..].ported). Nothing else
// moves by that - its source is its own.
#pragma once

#include <span>
#include <vector>

#include "base/buf.hpp"
#include "nbt/nbt.hpp"
#include "worldgen/java_hash.hpp"
#include "worldgen/noise.hpp"
#include "worldgen/region.hpp"

#include PROTO_GEN_WORLDGEN_FEATURES_HPP

namespace worldgen {

namespace ft = proto::gen::worldgen::feature;

// The source a chunk's decoration draws from (WorldgenRandom over a Xoroshiro source).
using FeatureRandom = WorldgenRandom<XoroshiroRandom>;
using Pos = Region::Pos;

// What a thread places features with, kept from one chunk to the next.
struct FeatureWork {
    std::vector<Pos> waiting;     // FeaturePlacer.positions: what is still to go through a placement...
    std::vector<u16> waiting_at;  // ...and which (modifierIndices)
    std::vector<Pos> made;        // modifiedPositions: what the placement at hand made
    std::vector<u64> tested;      // OreFeature.doPlace: the blocks of its box an ore has looked at

    // A tree (tree.cpp). What it set, as TreeFeature.place keeps it - in sets with a Java HashSet's order...
    JavaHashSet roots, trunks, foliage, decorations;
    JavaHashSet to_check[7];  // updateLeaves: the leaves still to be given their distance, a set a distance
    JavaHashSet blacklist;    // AttachedToLeavesDecorator: where nothing more is hung
    JavaHashSet fallen;       // FallenTreeFeature: the log that lies
    // ...and as its decorators are handed it (TreeDecorator.Context): lists by height.
    std::vector<Pos> logs, leaves, root_list;
    std::vector<Pos> list;  // whatever a placer or a decorator makes a list of
    struct Attachment {     // FoliagePlacer.FoliageAttachment: where a trunk wants leaves
        i32 x, y, z;
        i32 radius_offset;
        bool double_trunk;
    };
    std::vector<Attachment> attachments;
    struct Limb {  // FancyTrunkPlacer.FoliageCoords
        Pos pos;
        i32 branch_base;
    };
    std::vector<Limb> fancy;
    std::vector<u64> shape;  // the blocks of the tree, a bit a block of its box (BitSetDiscreteVoxelShape)
    base::ByteBuf tag;       // a block entity's tag while it is written (a nest's bees)

    // Caves (cave.cpp).
    JavaHashSet patch, patch_water;  // VegetationPatchFeature: the ground it laid, and where that holds water
    std::vector<Pos> patch_spots;    // ...in the order vegetation is put on it
    bool geode_made = false;         // GeodeFeature's noise, which is the world's: made when a world's first geode is
    i64 geode_seed = 0;
    Layer geode_layers[2];
    struct SculkCursor {  // SculkSpreader.ChargeCursor; facings: -1 for none yet, else a bit a direction
        i32 x, y, z;
        i32 charge, update_delay, decay_delay;
        i32 facings;
    };
    std::vector<SculkCursor> cursors;

    // Structure templates (template.cpp).
    struct TemplateBlock {  // a block of a template once the processors are through with it
        Pos pos;
        u16 state;    // as the template has it: not turned yet
        bool tagged;  // it carries a tag for its block entity
        u16 loot;     // kLootTables: what a rule gave that block entity (0: nothing)
        i64 loot_seed;
    };
    std::vector<TemplateBlock> template_blocks;
    std::vector<Pos> template_placed;  // what was set, in that order
    std::vector<Pos> template_fill;    // toFill: what may yet take water from beside it
    JavaHashSet template_locked;       // lockedFluids: the sources the template brought
};

// IntProvider.sample, HeightProvider.sample.
i32 sample(const ft::Int& provider, FeatureRandom& random);
i32 sample(const ft::Height& provider, FeatureRandom& random);

// Whether block state `state` is in state set `set` (a tag, a list of blocks, a fluid, "solid"...).
ALWAYS_INLINE bool in_state_set(u32 set, u32 state) {
    return ft::kStateSets[usize{set} * ft::kStateSetBytes + (state >> 3)] >> (state & 7) & 1;
}

// BlockPredicate.test
bool test(Region& region, const ft::Predicate& predicate, Pos at);

class Decorator {
  public:
    // A feature of a biome: its decoration step and its place in the step (what BiomeFilter asks for).
    struct Top {
        u32 step, index;
    };

    Decorator(Region& region, FeatureWork& work) : region_(region), work_(work) {}

    // ChunkGenerator.applyBiomeDecoration for the chunk the region has begun.
    void decorate();
    // FeaturePlacer.place: a placed feature (a row of kPlaced) at an origin. `top`: the feature of
    // a biome it is placed as, or none for one a feature is made of. True: something was placed.
    bool place(u32 placed, FeatureRandom& random, Pos origin, const Top* top);
    // PlacementModifier.modify: the positions `at` becomes, added to `out`.
    void modify(const ft::Modifier& modifier, FeatureRandom& random, Pos at, const Top* top, std::vector<Pos>& out);

    // For stands and tests. only: per feature of a biome - a place in kStepPlaced - whether it is
    // placed at all (nothing: every one that is ported). record: where the features of the
    // biomes would be placed is written down there instead of placing them, ported or not - as
    // long as their placements are.
    void set_only(std::span<const u8> only) { only_ = only; }
    void set_record(std::vector<Pos>* record) { record_ = record; }
    // ...and which features of the biomes a decoration ran, in its order.
    void set_log(std::vector<Top>* log) { log_ = log; }
    // ...and of each that it ran: what it wrote (Region::set_log, begun anew for each) and the long
    // its source gave afterwards.
    struct Trace {
        u32 step, index;
        u64 writes;
        i64 next;
    };
    void set_trace(std::vector<Trace>* trace) { trace_ = trace; }

  private:
    bool place_feature(u32 feature, FeatureRandom& random, Pos at);
    bool place_ore(const ft::Ore& ore, FeatureRandom& random, Pos origin);
    bool place_disk(const ft::Disk& disk, FeatureRandom& random, Pos origin);
    bool place_underwater_magma(const ft::Magma& magma, FeatureRandom& random, Pos origin);
    bool place_block_blob(u16 state, const ft::Predicate& can_place_on, FeatureRandom& random, Pos origin);
    bool place_spring(const ft::Spring& spring, Pos origin);
    bool place_lake(const ft::Lake& lake, FeatureRandom& random, Pos origin);
    bool place_blue_ice(FeatureRandom& random, Pos origin);
    bool place_spike(u16 state, const ft::Predicate& can_place_on, const ft::Predicate& can_replace, FeatureRandom& random,
                     Pos origin);
    bool place_iceberg(u16 state, FeatureRandom& random, Pos origin);
    bool place_freeze_top_layer(Pos origin);
    bool place_simple_block(const ft::Feature& feature, FeatureRandom& random, Pos origin);
    bool place_block_column(const ft::Column& column, FeatureRandom& random, Pos origin);
    bool place_bamboo(const ft::Bamboo& bamboo, FeatureRandom& random, Pos origin);
    bool place_vines(u32 states, Pos origin);
    bool place_coral_tree(u32 block, FeatureRandom& random, Pos origin);
    bool place_coral_claw(u32 block, FeatureRandom& random, Pos origin);
    bool place_huge_mushroom(const ft::Mushroom& mushroom, bool red, FeatureRandom& random, Pos origin);
    bool place_tree(const ft::Tree& tree, FeatureRandom& random, Pos origin);
    bool place_fallen_tree(const ft::FallenTree& fallen, FeatureRandom& random, Pos origin);
    bool place_vegetation_patch(const ft::Patch& patch, FeatureRandom& random, Pos origin);
    bool place_root_system(const ft::RootSystem& roots, FeatureRandom& random, Pos origin);
    bool place_speleothem(const ft::Speleothem& speleothem, FeatureRandom& random, Pos origin);
    bool place_speleothem_cluster(const ft::SpeleothemCluster& cluster, FeatureRandom& random, Pos origin);
    bool place_large_dripstone(const ft::LargeDripstone& dripstone, FeatureRandom& random, Pos origin);
    bool place_geode(const ft::Geode& geode, FeatureRandom& random, Pos origin);
    bool place_multiface_growth(const ft::Multiface& growth, FeatureRandom& random, Pos origin);
    bool place_sculk_patch(const ft::SculkPatch& patch, FeatureRandom& random, Pos origin);
    bool place_monster_room(FeatureRandom& random, Pos origin);
    bool place_template(const ft::TemplateFeature& feature, FeatureRandom& random, Pos origin);
    bool place_fossil(const ft::Fossil& fossil, FeatureRandom& random, Pos origin);
    friend struct TreeBuilder;  // (tree.cpp)
    u16 noise_state(const ft::NoiseProvider& provider, FeatureRandom& random, Pos at);
    i32 optional_state(const ft::Provider& provider, FeatureRandom& random, Pos at);
    u16 state_of(const ft::Provider& provider, FeatureRandom& random, Pos at);
    void mark_above(i32 x, i32 y, i32 z);

    Region& region_;
    FeatureWork& work_;
    std::span<const u8> only_;
    std::vector<Pos>* record_ = nullptr;
    std::vector<Top>* log_ = nullptr;
    std::vector<Trace>* trace_ = nullptr;
};

// LargeDripstone.getHeightAtRadius (cave.cpp): (int) SpeleothemUtils.getSpeleothemHeight.
i32 speleothem_height(double xz_from_center, double radius, double scale, double bluntness);

// MossyCarpetBlock.placeAt (cave.cpp): a carpet of pale moss, with what it sends up the walls beside it.
void place_mossy_carpet(Region& region, Pos at);

// RuleTest.test: what a block state at a height is to whoever would replace it.
bool test(const ft::Rule& rule, u32 state, i32 y);

// WeightedList.getRandomOrThrow: of entries[first .. first + count), whose weights sum to `total`, the
// row of the one a draw falls into.
u32 pick_weighted(const ft::Entry* entries, u32 first, u32 count, i32 total, FeatureRandom& random);

// What a block entity saves besides what it holds, as a chunk in the making writes it
// (BlockEntity.saveWithFullMetadata): closes the tag.
void end_block_entity(nbt::Writer& w, std::string_view id, Pos at);

}  // namespace worldgen
