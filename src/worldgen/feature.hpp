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

    Region& region_;
    FeatureWork& work_;
    std::span<const u8> only_;
    std::vector<Pos>* record_ = nullptr;
    std::vector<Top>* log_ = nullptr;
    std::vector<Trace>* trace_ = nullptr;
};

// RuleTest.test: what a block state at a height is to whoever would replace it.
bool test(const ft::Rule& rule, u32 state, i32 y);

}  // namespace worldgen
