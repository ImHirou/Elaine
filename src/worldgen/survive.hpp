// SPDX-License-Identifier: GPL-3.0-or-later
// BlockState.canSurvive, for the blocks world generation places: whether a block could stay
// where a feature is about to put it.
//
// Forty classes of vanilla's answer that question, and nearly all of them by one thing only - what
// one block is, the one below for most. Those are not ported: the extractor runs the method
// against every block state there is (tools/mcgen/extract/WorldgenSurvive.java), and what comes of
// it is a set of states, a bit a state. The few that ask more - a cactus about its four sides, sugar
// cane about the water beside its ground, a mushroom about the light - are rules written from their
// classes, over sets made of the tags they name (tools/mcgen/gen_features.py, `_survive_row`).
//
// A template over whoever can say what block is where: the generator's Region, and a test's few
// blocks (tests/unit/feature_test.cpp holds every rule to the jar's answers in worlds drawn at
// random).
#pragma once

#include "base/types.hpp"
#include "proto/gen/current.hpp"

#include PROTO_GEN_WORLDGEN_FEATURES_HPP

namespace worldgen {

// (worldgen/feature.hpp has it too: the same tables.)
ALWAYS_INLINE bool state_in(u32 set, u32 state) {
    namespace ft = proto::gen::worldgen::feature;
    return ft::kStateSets[usize{set} * ft::kStateSetBytes + (state >> 3)] >> (state & 7) & 1;
}

// `level`: block(x, y, z) and raw_brightness(x, y, z).
template <class Level>
bool survives(Level& level, const proto::gen::worldgen::feature::Survive& rule, i32 x, i32 y, i32 z) {
    namespace ft = proto::gen::worldgen::feature;
    switch (rule.op) {
        case ft::SurviveOp::kAlways: return true;
        case ft::SurviveOp::kNever: return false;
        case ft::SurviveOp::kAll:
            for (u32 i = rule.a; i < rule.a + rule.b; ++i) {
                const ft::SurvivePair& p = ft::kSurvivePairs[i];
                if (!state_in(p.set, level.block(x + p.dx, y + p.dy, z + p.dz))) return false;
            }
            return true;
        case ft::SurviveOp::kSugarCane: {
            // SugarCaneBlock: on sugar cane, or on ground that has water (or frosted ice) on one of its sides.
            const u32 below = level.block(x, y - 1, z);
            if (state_in(rule.a, below)) return true;
            if (!state_in(rule.b, below)) return false;
            return state_in(rule.c, level.block(x, y - 1, z - 1)) || state_in(rule.c, level.block(x + 1, y - 1, z)) ||
                   state_in(rule.c, level.block(x, y - 1, z + 1)) || state_in(rule.c, level.block(x - 1, y - 1, z));
        }
        case ft::SurviveOp::kSmallDripleaf: {
            // SmallDripleafBlock.mayPlaceOn: on clay and the like, or in water on what plants stand on.
            const u32 below = level.block(x, y - 1, z);
            return state_in(rule.a, below) || (state_in(rule.b, level.block(x, y, z)) && state_in(rule.c, below));
        }
        case ft::SurviveOp::kMushroom: {
            // MushroomBlock: on mycelium and the like whatever the light, else in the dark on a whole block.
            const u32 below = level.block(x, y - 1, z);
            if (state_in(rule.a, below)) return true;
            return level.raw_brightness(x, y, z) < 13 && state_in(rule.b, below);
        }
        case ft::SurviveOp::kUnported: break;
    }
    DCHECK(false);  // (what asks a rule that is not ported is not placed)
    return false;
}

}  // namespace worldgen
