// SPDX-License-Identifier: GPL-3.0-or-later
// What grows in caves: VegetationPatchFeature and its waterlogged kind, RootSystemFeature,
// SpeleothemFeature, SpeleothemClusterFeature and LargeDripstoneFeature (with SpeleothemUtils and
// Column), GeodeFeature, MultifaceGrowthFeature (with MultifaceSpreader and the two blocks that
// spread by it), SculkPatchFeature (with SculkSpreader, its cursors and the SculkBehaviour of
// sculk, of a vein and of everything else) - and MossyCarpetBlock.placeAt, which a carpet of pale
// moss is placed by. All of the 26.3 jar; a function a class or a method, named after it.
#include <algorithm>
#include <cmath>
#include <string>

#include "nbt/nbt.hpp"
#include "phys/mth.hpp"
#include "worldgen/feature.hpp"
#include "worldgen/jmath.hpp"
#include "worldgen/mth.hpp"
#include "worldgen/props.hpp"
#include "worldgen/shape.hpp"
#include "worldgen/survive.hpp"

namespace worldgen {

namespace blk = proto::gen::block;
namespace hm = world::heightmap;
using ft::Prop;

namespace {

constexpr u32 kHorizontal[4] = {world::kNorth, world::kEast, world::kSouth, world::kWest};  // Direction.Plane.HORIZONTAL

Pos moved(Pos at, u32 direction, i32 by = 1) {
    return {at.x + world::kStepX[direction] * by, at.y + world::kStepY[direction] * by, at.z + world::kStepZ[direction] * by};
}
bool same(Pos a, Pos b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
i32 manhattan(Pos a, Pos b) { return std::abs(a.x - b.x) + std::abs(a.y - b.y) + std::abs(a.z - b.z); }
bool is_block(u16 state, u32 default_state) { return world::block_of(state) == world::block_of(default_state); }

// Util.shuffle
template <class T>
void shuffle(T* list, i32 size, FeatureRandom& random) {
    for (i32 i = size; i > 1; --i) std::swap(list[i - 1], list[random.next_int(i)]);
}
// Direction.allShuffled
void all_shuffled(u32 out[6], FeatureRandom& random) {
    for (u32 d = 0; d < 6; ++d) out[d] = d;
    shuffle(out, 6, random);
}

// Mth.randomBetween, randomBetweenInclusive, clampedMap
float random_between(FeatureRandom& random, float min, float max) { return random.next_float() * (max - min) + min; }
i32 random_between_inclusive(FeatureRandom& random, i32 min, i32 max) { return random.next_int(max - min + 1) + min; }
float clamped_map(float value, float from_min, float from_max, float to_min, float to_max) {
    const float factor = (value - from_min) / (from_max - from_min);
    if (factor < 0.0f) return to_min;
    if (factor > 1.0f) return to_max;
    return to_min + factor * (to_max - to_min);
}
double clamped_map(double value, double from_min, double from_max, double to_min, double to_max) {
    const double factor = (value - from_min) / (from_max - from_min);
    if (factor < 0.0) return to_min;
    if (factor > 1.0) return to_max;
    return to_min + factor * (to_max - to_min);
}

// FloatProvider.sample
float sample(const ft::Float& f, FeatureRandom& random) {
    switch (f.op) {
        case ft::FloatOp::kConstant: return f.a;
        case ft::FloatOp::kUniform: return random_between(random, f.a, f.b);
        case ft::FloatOp::kClampedNormal: {
            // Mth.normal, then Mth.clamp: all of it in float.
            const float normal = f.a + static_cast<float>(random.next_gaussian()) * f.b;
            return clamp(normal, f.c, f.d);
        }
        case ft::FloatOp::kTrapezoid: {
            const float range = f.b - f.a;
            const float plateau_start = (range - f.c) / 2.0f;
            const float plateau_end = range - plateau_start;
            const float first = random.next_float() * plateau_end;
            const float second = random.next_float() * plateau_start;
            return f.a + first + second;
        }
    }
    return 0.0f;
}

// SpeleothemUtils: what a dripstone may grow into, and from.
bool is_empty_or_water(u16 state) { return world::is_air(state) || is_block(state, blk::kWater); }
bool is_empty_or_water_or_lava(u16 state) { return is_empty_or_water(state) || is_block(state, blk::kLava); }

// Column.scan: from a block that is inside, up and down as far as the range goes - a ceiling and a
// floor, where the block that ends each way is one a column may end on.
struct Column {
    bool found = false, has_floor = false, has_ceiling = false;
    i32 floor = 0, ceiling = 0;
    bool range() const { return has_floor && has_ceiling; }
    i32 height() const { return ceiling - floor - 1; }
};
template <class Inside, class Edge>
Column scan_column(Region& region, Pos at, i32 search_range, Inside&& inside, Edge&& edge) {
    Column c;
    if (!inside(region.block(at.x, at.y, at.z))) return c;
    c.found = true;
    const auto scan = [&](i32 step, i32& out) {
        i32 y = at.y;
        for (i32 i = 1; i < search_range && inside(region.block(at.x, y, at.z)); ++i) y += step;
        out = y;
        return edge(region.block(at.x, y, at.z));
    };
    c.has_ceiling = scan(1, c.ceiling);
    c.has_floor = scan(-1, c.floor);
    return c;
}

// MultifaceBlock: a block that lies on the faces of its neighbours - lichen, a sculk vein.
bool has_face(u16 state, u32 direction) {
    const Prop side = prop::side_of(direction);
    return prop::has(state, side) && prop::flag(state, side);
}
bool has_any_face(u16 state) {
    for (u32 d = 0; d < 6; ++d)
        if (has_face(state, d)) return true;
    return false;
}
u32 available_faces(u16 state) {
    u32 faces = 0;
    for (u32 d = 0; d < 6; ++d) faces |= static_cast<u32>(has_face(state, d)) << d;
    return faces;
}

// MultifaceSpreader with its SpreadConfig: the lichen's (DefaultSpreaderConfig), or a sculk vein's -
// as a vein spreads (every way) or onto what it lies on already (the same block only).
struct Spreader {
    Region& region;
    u16 block;        // the default state of the block that spreads
    bool sculk;       // SculkVeinBlock.SculkVeinSpreaderConfig
    bool same_space;  // ...of SculkVeinBlock.getSameSpaceSpreader

    u16 at(Pos p) const { return region.block(p.x, p.y, p.z); }
    bool is_own(u16 state) const { return is_block(state, block); }
    // MultifaceBlock.isValidStateForPlacement, getStateForPlacement (-1: none)
    bool valid_for_placement(u16 old, Pos p, u32 direction) const {
        if (is_own(old) && has_face(old, direction)) return false;
        return in_state_set(ft::kSetVineAttaches[direction], at(moved(p, direction)));
    }
    i32 state_for_placement(u16 old, Pos p, u32 direction) const {
        if (!valid_for_placement(old, p, direction)) return -1;
        u16 made = block;
        if (is_own(old))
            made = old;
        else if (in_state_set(ft::kSetWaterSource, old))
            made = prop::with_flag(block, Prop::kWaterlogged, true);
        return prop::with_flag(made, prop::side_of(direction), true);
    }
    // SpreadConfig.canSpreadInto
    bool can_spread_into(Pos source, Pos p, u32 face) const {
        const u16 existing = at(p);
        if (sculk) {
            const u16 against = at(moved(p, face));
            if (is_block(against, blk::kSculk) || is_block(against, blk::kSculkCatalyst) || is_block(against, blk::kMovingPiston))
                return false;
            if (manhattan(source, p) == 2) {
                const Pos beside = moved(source, world::opposite(face));
                if (in_state_set(ft::kSetSturdy[face], at(beside))) return false;
            }
            if (!in_state_set(ft::kSetNoFluidButWater, existing)) return false;
            if (in_state_set(ft::kSetFire, existing)) return false;
            if (!in_state_set(ft::kSetReplaceable, existing) &&
                !(world::is_air(existing) || is_own(existing) ||
                  (is_block(existing, blk::kWater) && in_state_set(ft::kSetFluidSource, existing))))
                return false;
        } else if (!(world::is_air(existing) || is_own(existing) ||
                     (is_block(existing, blk::kWater) && in_state_set(ft::kSetFluidSource, existing)))) {
            return false;
        }
        return valid_for_placement(existing, p, face);
    }
    // spreadFromFaceTowardDirection: onto the same block's other side, onto the block beside, or round the corner.
    bool spread_from_face_toward(u16 state, Pos p, u32 from_face, u32 direction, bool post_process) const {
        if ((direction >> 1) == (from_face >> 1)) return false;                // (one axis)
        const bool other_source = sculk && !is_block(state, blk::kSculkVein);  // isOtherBlockValidAsSource
        if (!(other_source || (has_face(state, from_face) && !has_face(state, direction)))) return false;
        const u32 types = same_space ? 1 : 3;
        for (u32 type = 0; type < types; ++type) {
            Pos to = p;
            u32 face = direction;
            if (type == 1) to = moved(p, direction), face = from_face;
            if (type == 2) to = moved(moved(p, direction), from_face), face = world::opposite(direction);
            if (!can_spread_into(p, to, face)) continue;
            // spreadToFace, SpreadConfig.placeBlock
            const i32 made = state_for_placement(at(to), to, face);
            if (made < 0) return false;
            if (post_process) region.mark_post_process(to.x, to.y, to.z);
            return region.set_block(to.x, to.y, to.z, static_cast<u16>(made), 2);
        }
        return false;
    }
    // spreadAll: from every face it may spread from, every way. How many took.
    u32 spread_all(u16 state, Pos p, bool post_process) const {
        u32 count = 0;
        const bool other_source = sculk && !is_block(state, blk::kSculkVein);
        for (u32 face = 0; face < 6; ++face) {
            if (!(other_source || has_face(state, face))) continue;  // SpreadConfig.canSpreadFrom
            for (u32 direction = 0; direction < 6; ++direction)
                count += spread_from_face_toward(state, p, face, direction, post_process);
        }
        return count;
    }
};

}  // namespace

// MossyCarpetBlock.placeAt: the carpet, and above it the moss that climbs - a side of it where the
// level's own source says so.
void place_mossy_carpet(Region& region, Pos p) {
    const u16 carpet = static_cast<u16>(blk::kPaleMossCarpet);
    const u16 adjusted = mossy_carpet_updated(region, carpet, p.x, p.y, p.z, true);
    region.set_block(p.x, p.y, p.z, adjusted, 2);
    // createTopperWithSideChance
    const u16 previous = region.block(p.x, p.y + 1, p.z);
    const bool carpet_above = is_block(previous, blk::kPaleMossCarpet);
    if (carpet_above ? prop::flag(previous, Prop::kBottom) : !in_state_set(ft::kSetReplaceable, previous)) return;
    u16 topper = mossy_carpet_updated(region, prop::with_flag(carpet, Prop::kBottom, false), p.x, p.y + 1, p.z, true);
    bool any = false;
    for (const u32 direction : kHorizontal) {
        const Prop wall = kMossyWall[direction];
        if (prop::get(topper, wall) == 0) continue;
        if (!region.random().next_boolean())
            topper = prop::with(topper, wall, 0);
        else
            any = true;
    }
    if (!any || topper == previous) return;
    region.set_block(p.x, p.y + 1, p.z, topper, 2);
    region.set_block(p.x, p.y, p.z, mossy_carpet_updated(region, adjusted, p.x, p.y, p.z, true), 2);
}

// ------------------------------------------------------------------------- VegetationPatchFeature

bool Decorator::place_vegetation_patch(const ft::Patch& patch, FeatureRandom& random, Pos origin) {
    Region& region = region_;
    const i32 x_radius = sample(ft::kInts[patch.xz_radius], random) + 1;
    const i32 z_radius = sample(ft::kInts[patch.xz_radius], random) + 1;
    const i32 inwards = patch.ceiling ? 1 : -1;  // CaveSurface.getDirection: up to a ceiling, down to a floor
    const u32 sturdy = ft::kSetSturdy[patch.ceiling ? world::kDown : world::kUp];
    // placeGroundPatch: the ground of every column of the patch, as deep as it is drawn.
    JavaHashSet& surface = work_.patch;
    surface.clear();
    for (i32 dx = -x_radius; dx <= x_radius; ++dx) {
        const bool x_edge = dx == -x_radius || dx == x_radius;
        for (i32 dz = -z_radius; dz <= z_radius; ++dz) {
            const bool z_edge = dz == -z_radius || dz == z_radius;
            if (x_edge && z_edge) continue;
            if ((x_edge || z_edge) &&
                (patch.extra_edge_column_chance == 0.0f || random.next_float() > patch.extra_edge_column_chance))
                continue;
            Pos p{origin.x + dx, origin.y, origin.z + dz};
            for (i32 offset = 0; world::is_air(region.block(p.x, p.y, p.z)) && offset < patch.vertical_range; ++offset)
                p.y += inwards;
            for (i32 offset = 0; !world::is_air(region.block(p.x, p.y, p.z)) && offset < patch.vertical_range; ++offset)
                p.y -= inwards;
            Pos below{p.x, p.y + inwards, p.z};
            if (!world::is_air(region.block(p.x, p.y, p.z)) || !in_state_set(sturdy, region.block(below.x, below.y, below.z)))
                continue;
            const i32 drawn = sample(ft::kInts[patch.depth], random);
            const i32 depth =
                drawn + (patch.extra_bottom_block_chance > 0.0f && random.next_float() < patch.extra_bottom_block_chance ? 1 : 0);
            const Pos ground = below;
            // placeGround
            bool placed = true;
            for (i32 i = 0; i < depth; ++i) {
                const u16 to_place = state_of(ft::kProviders[patch.ground_state], random, below);
                const u16 there = region.block(below.x, below.y, below.z);
                if (world::block_of(to_place) == world::block_of(there)) continue;
                if (!in_state_set(patch.replaceable, there)) {
                    placed = i != 0;
                    break;
                }
                region.set_block(below.x, below.y, below.z, to_place, 2);
                below.y += inwards;
            }
            if (placed) surface.add(ground.x, ground.y, ground.z);
        }
    }
    // (A list of its own: what a patch's vegetation is made of may use the thread's other lists.)
    std::vector<Pos>& list = work_.patch_spots;
    list.clear();
    if (patch.waterlogged) {
        // WaterloggedVegetationPatchFeature: water where the ground has walls on every side and below.
        JavaHashSet& water = work_.patch_water;
        water.clear();
        surface.each([&](i32 x, i32 y, i32 z) {
            const auto exposed = [&](u32 direction) {
                return !in_state_set(
                    ft::kSetSturdy[world::opposite(direction)],
                    region.block(x + world::kStepX[direction], y + world::kStepY[direction], z + world::kStepZ[direction]));
            };
            if (exposed(world::kNorth) || exposed(world::kEast) || exposed(world::kSouth) || exposed(world::kWest) ||
                exposed(world::kDown))
                return;
            water.add(x, y, z);
        });
        water.each([&](i32 x, i32 y, i32 z) { list.push_back({x, y, z}); });
        for (const Pos& p : list) region.set_block(p.x, p.y, p.z, static_cast<u16>(blk::kWater), 2);
    } else {
        surface.each([&](i32 x, i32 y, i32 z) { list.push_back({x, y, z}); });
    }
    // distributeVegetation
    const std::vector<Pos>& spots = list;
    for (usize i = 0; i < spots.size(); ++i) {
        const Pos p = spots[i];
        if (!(patch.vegetation_chance > 0.0f && random.next_float() < patch.vegetation_chance)) continue;
        if (patch.waterlogged) {
            // ...into the water, and what stands in it holds it.
            if (!place(patch.vegetation, random, p, nullptr)) continue;
            const u16 placed = region.block(p.x, p.y, p.z);
            if (prop::has(placed, Prop::kWaterlogged) && !prop::flag(placed, Prop::kWaterlogged))
                region.set_block(p.x, p.y, p.z, prop::with_flag(placed, Prop::kWaterlogged, true), 2);
        } else {
            place(patch.vegetation, random, {p.x, p.y - inwards, p.z}, nullptr);
        }
    }
    return !spots.empty();
}

// ------------------------------------------------------------------------------ RootSystemFeature

bool Decorator::place_root_system(const ft::RootSystem& roots, FeatureRandom& random, Pos origin) {
    Region& region = region_;
    if (!world::is_air(region.block(origin.x, origin.y, origin.z))) return false;
    // placeDirtAndTree: up the column to where the tree may stand.
    Pos working = origin;
    bool planted = false;
    for (i32 y = 0; y < roots.root_column_max_height; ++y) {
        ++working.y;
        if (region.height(hm::kWorldSurface, working.x, working.z) < working.y) return true;
        bool space = test(region, ft::kPredicates[roots.allowed_tree_position], working);
        // spaceForTree
        for (i32 i = 1; space && i <= roots.required_vertical_space_for_tree; ++i) {
            const u16 state = region.block(working.x, working.y + i, working.z);
            space = world::is_air(state) ||
                    (i + 1 <= roots.allowed_vertical_water_for_tree && in_state_set(ft::kSetHoldsWater, state));
        }
        if (space && roots.level_test_distance > 0) {
            constexpr u32 kBy2d[4] = {world::kSouth, world::kWest, world::kNorth, world::kEast};  // Direction.from2DDataValue
            for (u32 i = 0; space && i < 4; ++i) {
                const Pos corner = moved(working, kBy2d[i], roots.level_test_distance);
                space = !world::is_air(region.block(corner.x, corner.y - roots.max_level_deviation, corner.z)) &&
                        world::is_air(region.block(corner.x, corner.y + roots.max_level_deviation, corner.z));
            }
        }
        if (!space) continue;
        const u16 below = region.block(working.x, working.y - 1, working.z);
        if (in_state_set(ft::kSetHoldsLava, below) || !in_state_set(ft::kSetSolid, below)) return true;
        if (!place(roots.tree, random, working, nullptr)) continue;
        // placeDirt: rooted dirt strewn about the column, a layer at a time.
        for (i32 at = origin.y; at < origin.y + y; ++at)
            for (i32 i = 0; i < roots.root_placement_attempts; ++i) {
                const i32 x0 = random.next_int(roots.root_radius);
                const i32 x1 = random.next_int(roots.root_radius);
                const i32 z0 = random.next_int(roots.root_radius);
                const i32 z1 = random.next_int(roots.root_radius);
                const Pos p{origin.x + x0 - x1, at, origin.z + z0 - z1};
                if (!in_state_set(roots.root_replaceable, region.block(p.x, p.y, p.z))) continue;
                region.set_block(p.x, p.y, p.z, state_of(ft::kProviders[roots.root_provider], random, p), 2);
            }
        planted = true;
        break;
    }
    if (!planted) return true;
    // placeRoots: what hangs from the ceiling around.
    for (i32 i = 0; i < roots.hanging_root_placement_attempts; ++i) {
        const i32 x0 = random.next_int(roots.hanging_root_radius);
        const i32 x1 = random.next_int(roots.hanging_root_radius);
        const i32 y0 = random.next_int(roots.hanging_roots_vertical_span);
        const i32 y1 = random.next_int(roots.hanging_roots_vertical_span);
        const i32 z0 = random.next_int(roots.hanging_root_radius);
        const i32 z1 = random.next_int(roots.hanging_root_radius);
        const Pos p{origin.x + x0 - x1, origin.y + y0 - y1, origin.z + z0 - z1};
        if (!world::is_air(region.block(p.x, p.y, p.z))) continue;
        const u16 target = state_of(ft::kProviders[roots.hanging_root_provider], random, p);
        const ft::SimpleBlock* row = nullptr;
        for (u32 at = roots.hanging; at < u32{roots.hanging} + roots.hanging_count && !row; ++at)
            if (ft::kSimpleBlocks[at].state == target) row = &ft::kSimpleBlocks[at];
        DCHECK(row);
        if (!row || !survives(region, ft::kSurvive[row->survive], p.x, p.y, p.z)) continue;
        if (!in_state_set(ft::kSetSturdy[world::kDown], region.block(p.x, p.y + 1, p.z))) continue;
        region.set_block(p.x, p.y, p.z, target, 2);
    }
    return true;
}

// ---------------------------------------------------------------- SpeleothemUtils and its features

namespace {

struct Dripstone {
    Region& region;
    u16 base_block, pointed_block;
    u32 replaceable;

    u16 at(Pos p) const { return region.block(p.x, p.y, p.z); }
    bool is_base(u16 state) const { return is_block(state, base_block) || in_state_set(replaceable, state); }
    bool place_base_if_possible(Pos p) const {
        if (!in_state_set(replaceable, at(p))) return false;
        region.set_block(p.x, p.y, p.z, base_block, 2);
        return true;
    }
    // growSpeleothem, buildBaseToTipColumn: a base, a middle as long as it takes, a frustum and a tip.
    void grow(Pos start, u32 tip_direction, i32 height, bool merged_tip) const {
        if (!is_base(at(moved(start, world::opposite(tip_direction))))) return;
        Pos p = start;
        enum : u32 { kTipMerge, kTip, kFrustum, kMiddle, kBase };
        const auto put = [&](u32 thickness) {
            u16 state = prop::with(prop::with(pointed_block, Prop::kVerticalDirection, tip_direction == world::kUp ? 0 : 1),
                                   Prop::kThickness, thickness);
            state = prop::with_flag(state, Prop::kWaterlogged, in_state_set(ft::kSetHoldsWater, at(p)));
            region.set_block(p.x, p.y, p.z, state, 2);
            p = moved(p, tip_direction);
        };
        if (height >= 3) {
            put(kBase);
            for (i32 i = 0; i < height - 3; ++i) put(kMiddle);
        }
        if (height >= 2) put(kFrustum);
        if (height >= 1) put(merged_tip ? kTipMerge : kTip);
    }
};

}  // namespace

bool Decorator::place_speleothem(const ft::Speleothem& s, FeatureRandom& random, Pos origin) {
    Region& region = region_;
    const Dripstone drip{region, s.base_block, s.pointed_block, s.replaceable};
    // getTipDirection: down from a ceiling, up from a floor - either, where there are both.
    const bool above = drip.is_base(region.block(origin.x, origin.y + 1, origin.z));
    const bool below = drip.is_base(region.block(origin.x, origin.y - 1, origin.z));
    u32 tip;
    if (above && below)
        tip = random.next_boolean() ? world::kDown : world::kUp;
    else if (above)
        tip = world::kDown;
    else if (below)
        tip = world::kUp;
    else
        return false;
    // createPatchOfBaseBlocks: base blocks where it is rooted, and a few more round that.
    const Pos root = moved(origin, world::opposite(tip));
    drip.place_base_if_possible(root);
    for (const u32 direction : kHorizontal) {
        if (random.next_float() > s.chance_of_directional_spread) continue;
        const Pos first = moved(root, direction);
        drip.place_base_if_possible(first);
        if (random.next_float() > s.chance_of_spread_radius2) continue;
        const Pos second = moved(first, static_cast<u32>(random.next_int(6)));
        drip.place_base_if_possible(second);
        if (random.next_float() > s.chance_of_spread_radius3) continue;
        const Pos third = moved(second, static_cast<u32>(random.next_int(6)));
        drip.place_base_if_possible(third);
    }
    const Pos ahead = moved(origin, tip);
    const i32 height =
        random.next_float() < s.chance_of_taller_generation && is_empty_or_water(region.block(ahead.x, ahead.y, ahead.z)) ? 2 : 1;
    drip.grow(origin, tip, height, false);
    return true;
}

bool Decorator::place_speleothem_cluster(const ft::SpeleothemCluster& c, FeatureRandom& random, Pos origin) {
    Region& region = region_;
    if (!is_empty_or_water(region.block(origin.x, origin.y, origin.z))) return false;
    const Dripstone drip{region, c.base_block, c.pointed_block, c.replaceable};
    const i32 cluster_height = sample(ft::kInts[c.height], random);
    const float wetness = sample(ft::kFloats[c.wetness], random);
    const float density = sample(ft::kFloats[c.density], random);
    const i32 x_radius = sample(ft::kInts[c.radius], random);
    const i32 z_radius = sample(ft::kInts[c.radius], random);
    const auto at = [&](i32 x, i32 y, i32 z) { return region.block(x, y, z); };
    // getSpeleothemHeight: none at all, or about half the room - less, further from the middle.
    const auto height_of = [&](i32 dx, i32 dz, i32 max_height) {
        if (random.next_float() > density) return 0;
        const i32 from_center = std::abs(dx) + std::abs(dz);
        const float mean =
            static_cast<float>(clamped_map(static_cast<double>(from_center), 0.0, static_cast<double>(c.max_distance_from_center),
                                           static_cast<double>(max_height) / 2.0, 0.0));
        const float normal = mean + static_cast<float>(random.next_gaussian()) * static_cast<float>(c.height_deviation);
        return static_cast<i32>(clamp(normal, 0.0f, static_cast<float>(max_height)));
    };
    const auto replace_with_base = [&](Pos p, i32 count, i32 step) {
        for (i32 i = 0; i < count; ++i) {
            if (!drip.place_base_if_possible(p)) return;
            p.y += step;
        }
    };
    const auto beside_water_ok = [&](i32 x, i32 y, i32 z) {
        const u16 state = at(x, y, z);
        return in_state_set(ft::kSetBaseStoneOverworld, state) || in_state_set(ft::kSetHoldsWater, state);
    };
    for (i32 dx = -x_radius; dx <= x_radius; ++dx)
        for (i32 dz = -z_radius; dz <= z_radius; ++dz) {
            const i32 from_edge = std::min(x_radius - std::abs(dx), z_radius - std::abs(dz));
            const double chance =
                static_cast<double>(clamped_map(static_cast<float>(from_edge), 0.0f, static_cast<float>(c.max_distance_from_edge),
                                                c.chance_at_max_distance_from_center, 1.0f));
            // placeColumn
            const Pos p{origin.x + dx, origin.y, origin.z + dz};
            Column column = scan_column(region, p, c.floor_to_ceiling_search_range, is_empty_or_water,
                                        [](u16 state) { return !is_empty_or_water(state); });
            if (!column.found || (!column.has_ceiling && !column.has_floor)) continue;
            const bool has_ceiling = column.has_ceiling;
            const i32 ceiling = column.ceiling;
            const bool want_pool = random.next_float() < wetness;
            if (want_pool && column.has_floor) {
                // canPlacePool: a block of the floor that has stone or water on its sides and below.
                const i32 y = column.floor;
                const u16 state = at(p.x, y, p.z);
                bool pool =
                    !(is_block(state, blk::kWater) || is_block(state, c.base_block) || is_block(state, c.pointed_block)) &&
                    !in_state_set(ft::kSetHoldsWater, at(p.x, y + 1, p.z));
                for (u32 i = 0; pool && i < 4; ++i)
                    pool = beside_water_ok(p.x + world::kStepX[kHorizontal[i]], y, p.z + world::kStepZ[kHorizontal[i]]);
                pool = pool && beside_water_ok(p.x, y - 1, p.z);
                if (pool) {
                    region.set_block(p.x, y, p.z, static_cast<u16>(blk::kWater), 2);
                    column.floor = y - 1;  // Column.withFloor
                }
            }
            const bool has_floor = column.has_floor;
            const i32 floor = column.floor;
            const bool want_stalactite = random.next_double() < chance;
            i32 stalactite = 0;
            if (has_ceiling && want_stalactite && !is_block(at(p.x, ceiling, p.z), blk::kLava)) {
                const i32 thickness = sample(ft::kInts[c.layer_thickness], random);
                replace_with_base({p.x, ceiling, p.z}, thickness, 1);
                stalactite = height_of(dx, dz, has_floor ? std::min(cluster_height, ceiling - floor) : cluster_height);
            }
            const bool want_stalagmite = random.next_double() < chance;
            i32 stalagmite = 0;
            if (has_floor && want_stalagmite && !is_block(at(p.x, floor, p.z), blk::kLava)) {
                const i32 thickness = sample(ft::kInts[c.layer_thickness], random);
                replace_with_base({p.x, floor, p.z}, thickness, -1);
                if (has_ceiling) {
                    const i32 differ = random_between_inclusive(random, -c.max_stalagmite_stalactite_height_diff,
                                                                c.max_stalagmite_stalactite_height_diff);
                    stalagmite = std::max(0, stalactite + differ);
                } else {
                    stalagmite = height_of(dx, dz, cluster_height);
                }
            }
            if (has_ceiling && has_floor && ceiling - stalactite <= floor + stalagmite) {
                // The two would meet: where, is drawn.
                const i32 lowest_bottom = std::max(ceiling - stalactite, floor + 1);
                const i32 highest_top = std::min(floor + stalagmite, ceiling - 1);
                const i32 bottom = random_between_inclusive(random, lowest_bottom, highest_top + 1);
                stalactite = ceiling - bottom;
                stalagmite = bottom - 1 - floor;
            }
            const bool merge = random.next_boolean() && stalactite > 0 && stalagmite > 0 && has_ceiling && has_floor &&
                               stalactite + stalagmite == ceiling - floor - 1;
            if (has_ceiling) drip.grow({p.x, ceiling - 1, p.z}, world::kDown, stalactite, merge);
            if (has_floor) drip.grow({p.x, floor + 1, p.z}, world::kUp, stalagmite, merge);
        }
    return true;
}

namespace {

// SpeleothemUtils.getSpeleothemHeight, with the functions it is made of as parameters.
template <class Pow, class Log>
double speleothem_height_by(double xz_from_center, double radius, double scale, double bluntness, Pow&& pow, Log&& log) {
    if (xz_from_center < bluntness) xz_from_center = bluntness;
    const double r = xz_from_center / radius * 0.384;
    const double part1 = 0.75 * pow(r, 1.3333333333333333);
    const double part2 = pow(r, 0.6666666666666666);
    const double part3 = 0.3333333333333333 * log(r);
    double height = scale * (part1 - part2 - part3);
    height = std::max(height, 0.0);
    return height / 0.384 * radius;
}

}  // namespace

// LargeDripstone.getHeightAtRadius: (int) SpeleothemUtils.getSpeleothemHeight - how many blocks
// tall a dripstone is so far from its middle. Vanilla's height is made of Math.pow and Math.log,
// and what counts of it is the whole number below it: so the C library's functions do for all
// but the heights that lie within a millionth of a whole number - a thousand million times what
// any two logarithms differ by - and there the answer is made of our own (jmath.hpp), which no
// library can change.
i32 speleothem_height(double xz_from_center, double radius, double scale, double bluntness) {
    const double fast = speleothem_height_by(
        xz_from_center, radius, scale, bluntness, [](double x, double y) { return std::pow(x, y); },
        [](double x) { return std::log(x); });
    if (std::fabs(fast - std::nearbyint(fast)) > 1e-6 && fast < 1e9) return static_cast<i32>(fast);
    return static_cast<i32>(speleothem_height_by(
        xz_from_center, radius, scale, bluntness, [](double x, double y) { return jmath::pow(x, y); },
        [](double x) { return jmath::log(x); }));
}

namespace {

// LargeDripstoneFeature.WindOffsetter: a dripstone leans, the further from where it began the more.
struct Wind {
    bool blows = false;
    i32 origin_y = 0, max_offset = 0;
    double x = 0, z = 0;
    Pos offset(Pos p) const {
        if (!blows) return p;
        const i32 dy = origin_y - p.y;
        const i32 dx = std::min(std::max(floor_int(x * static_cast<double>(dy)), -max_offset), max_offset);
        const i32 dz = std::min(std::max(floor_int(z * static_cast<double>(dy)), -max_offset), max_offset);
        return {p.x + dx, p.y, p.z + dz};
    }
};

// LargeDripstoneFeature.LargeDripstone
struct LargeDripstone {
    Pos root;
    bool pointing_up;
    i32 radius;
    double bluntness, scale;

    i32 height_at(float check_radius) const {
        return speleothem_height(static_cast<double>(check_radius), static_cast<double>(radius), scale, bluntness);
    }
    // moveBackUntilBaseIsInsideStoneAndShrinkRadiusIfNecessary
    bool settle(Region& region, const Wind& wind) {
        while (radius > 1) {
            Pos to = root;
            const i32 tries = std::min(10, height_at(0.0f));
            for (i32 i = 0; i < tries; ++i) {
                if (is_block(region.block(to.x, to.y, to.z), blk::kLava)) return false;
                // isCircleMostlyEmbeddedInStone
                const Pos center = wind.offset(to);
                bool embedded = !is_empty_or_water_or_lava(region.block(center.x, center.y, center.z));
                const float step = 6.0f / static_cast<float>(radius);
                for (float angle = 0.0f; embedded && angle < 6.2831855f; angle += step) {
                    const i32 dx = static_cast<i32>(phys::mth_cos(static_cast<double>(angle)) * static_cast<float>(radius));
                    const i32 dz = static_cast<i32>(phys::mth_sin(static_cast<double>(angle)) * static_cast<float>(radius));
                    embedded = !is_empty_or_water_or_lava(region.block(center.x + dx, center.y, center.z + dz));
                }
                if (embedded) {
                    root = to;
                    return true;
                }
                to.y += pointing_up ? -1 : 1;
            }
            radius /= 2;
        }
        return false;
    }
    void place_blocks(Region& region, FeatureRandom& random, const Wind& wind) const {
        for (i32 dx = -radius; dx <= radius; ++dx)
            for (i32 dz = -radius; dz <= radius; ++dz) {
                const float current =
                    static_cast<float>(std::sqrt(static_cast<double>(static_cast<float>(dx * dx + dz * dz))));  // Mth.sqrt
                if (current > static_cast<float>(radius)) continue;
                i32 height = height_at(current);
                if (height <= 0) continue;
                if (static_cast<double>(random.next_float()) < 0.2)
                    height = static_cast<i32>(static_cast<float>(height) * random_between(random, 0.8f, 1.0f));
                Pos p{root.x + dx, root.y, root.z + dz};
                bool out_of_stone = false;
                const i32 max_y = pointing_up ? region.height(hm::kWorldSurfaceWg, p.x, p.z) : INT32_MAX;
                for (i32 i = 0; i < height && p.y < max_y; ++i) {
                    const Pos to = wind.offset(p);
                    const u16 state = region.block(to.x, to.y, to.z);
                    if (is_empty_or_water_or_lava(state)) {
                        out_of_stone = true;
                        region.set_block(to.x, to.y, to.z, static_cast<u16>(blk::kDripstoneBlock), 2);
                    } else if (out_of_stone && in_state_set(ft::kSetBaseStoneOverworld, state)) {
                        break;
                    }
                    p.y += pointing_up ? 1 : -1;
                }
            }
    }
};

}  // namespace

bool Decorator::place_large_dripstone(const ft::LargeDripstone& d, FeatureRandom& random, Pos origin) {
    Region& region = region_;
    if (!is_empty_or_water(region.block(origin.x, origin.y, origin.z))) return false;
    const Column column = scan_column(region, origin, d.floor_to_ceiling_search_range, is_empty_or_water, [&](u16 state) {
        return is_block(state, blk::kDripstoneBlock) || in_state_set(d.replaceable, state) ||
               is_block(state, blk::kLava);  // isBaseOrLava
    });
    if (!column.found || !column.range()) return false;
    if (column.height() < 4) return false;
    const i32 by_height = static_cast<i32>(static_cast<float>(column.height()) * d.max_column_radius_to_cave_height_ratio);
    const i32 max_radius = std::min(std::max(by_height, d.radius_min), d.radius_max);
    const i32 radius = random_between_inclusive(random, d.radius_min, max_radius);
    const auto make = [&](i32 y, bool pointing_up, const ft::Float& bluntness) {
        const double blunt = static_cast<double>(sample(bluntness, random));
        const double scale = static_cast<double>(sample(ft::kFloats[d.height_scale], random));
        return LargeDripstone{{origin.x, y, origin.z}, pointing_up, radius, blunt, scale};
    };
    LargeDripstone stalactite = make(column.ceiling - 1, false, ft::kFloats[d.stalactite_bluntness]);
    LargeDripstone stalagmite = make(column.floor + 1, true, ft::kFloats[d.stalagmite_bluntness]);
    Wind wind;
    const auto suits = [&](const LargeDripstone& one) {
        return one.radius >= d.min_radius_for_wind && one.bluntness >= static_cast<double>(d.min_bluntness_for_wind);
    };
    if (suits(stalactite) && suits(stalagmite)) {
        const float speed = sample(ft::kFloats[d.wind_speed], random);
        const float direction = random_between(random, 0.0f, 3.1415927f);
        wind.blows = true;
        wind.origin_y = origin.y;
        wind.max_offset = 16 - radius;
        wind.x = static_cast<double>(phys::mth_cos(static_cast<double>(direction)) * speed);
        wind.z = static_cast<double>(phys::mth_sin(static_cast<double>(direction)) * speed);
    }
    const bool hangs = stalactite.settle(region, wind);
    const bool stands = stalagmite.settle(region, wind);
    if (hangs) stalactite.place_blocks(region, random, wind);
    if (stands) stalagmite.place_blocks(region, random, wind);
    return true;
}

// ----------------------------------------------------------------------------------- GeodeFeature

bool Decorator::place_geode(const ft::Geode& g, FeatureRandom& random, Pos origin) {
    Region& region = region_;
    // GeodeFeature.NOISE_PARAMETERS.create(new WorldgenRandom(new LegacyRandomSource(seed))): one noise a world.
    FeatureWork& work = work_;
    constexpr u32 kLayers = static_cast<u32>(std::size(ft::kGeodeLayers));
    if (!work.geode_made || work.geode_seed != region.world().seed()) {
        LegacyRandom source(region.world().seed());
        const LegacyPositional first = source.fork_positional(), second = source.fork_positional();
        for (u32 i = 0; i < kLayers; ++i) {
            const std::string octave = "octave_" + std::to_string(ft::kGeodeFirstOctave + static_cast<i32>(i / 2));
            LegacyRandom of = (i & 1 ? second : first).from_hash_of(octave);
            work.geode_layers[i].noise.seed(of);
            work.geode_layers[i].noise.fudge_y_scale = 0.0;
            work.geode_layers[i].frequency = ft::kGeodeLayers[i].frequency;
            work.geode_layers[i].amplitude = ft::kGeodeLayers[i].amplitude;
        }
        work.geode_seed = region.world().seed();
        work.geode_made = true;
    }
    const Noise noise{work.geode_layers, kLayers, false};

    struct Point {
        Pos pos;
        i32 offset;
    };
    Point points[32];
    const i32 count = sample(ft::kInts[g.distribution_points], random);
    DCHECK(count <= 32);
    const double adjustment = static_cast<double>(count) / static_cast<double>(g.outer_wall_distance_max);
    const double inner_air = 1.0 / std::sqrt(g.filling_size);
    const double innermost = 1.0 / std::sqrt(g.inner_layer_size + adjustment);
    const double inner_crust = 1.0 / std::sqrt(g.middle_layer_size + adjustment);
    const double outer_crust = 1.0 / std::sqrt(g.outer_layer_size + adjustment);
    const double crack_draw = random.next_double();
    const double crack_size = 1.0 / std::sqrt(g.base_crack_size + crack_draw / 2.0 + (count > 3 ? adjustment : 0.0));
    const bool crack = static_cast<double>(random.next_float()) < g.generate_crack_chance;
    i32 invalid = 0;
    for (i32 i = 0; i < count; ++i) {
        const i32 x = sample(ft::kInts[g.outer_wall_distance], random);
        const i32 y = sample(ft::kInts[g.outer_wall_distance], random);
        const i32 z = sample(ft::kInts[g.outer_wall_distance], random);
        const Pos p{origin.x + x, origin.y + y, origin.z + z};
        const u16 state = region.block(p.x, p.y, p.z);
        if ((world::is_air(state) || in_state_set(g.invalid_blocks, state)) && ++invalid > g.invalid_blocks_threshold)
            return false;
        points[i] = {p, sample(ft::kInts[g.point_offset], random)};
    }
    Pos crack_points[3];
    u32 crack_count = 0;
    if (crack) {
        const i32 which = random.next_int(4);
        const i32 off = count * 2 + 1;
        const i32 cx = which == 0 || which == 2 ? off : 0, cz = which == 1 || which == 2 ? off : 0;
        for (const i32 y : {7, 5, 1}) crack_points[crack_count++] = {origin.x + cx, origin.y + y, origin.z + cz};
    }
    const auto dist_sqr = [](Pos a, Pos b) {
        const double dx = static_cast<double>(a.x) - static_cast<double>(b.x),
                     dy = static_cast<double>(a.y) - static_cast<double>(b.y),
                     dz = static_cast<double>(a.z) - static_cast<double>(b.z);
        return dx * dx + dy * dy + dz * dz;
    };
    const auto safe_set = [&](Pos p, u16 state) {  // Feature.safeSetBlock
        if (!in_state_set(g.cannot_replace, region.block(p.x, p.y, p.z))) region.set_block(p.x, p.y, p.z, state, 2);
    };
    std::vector<Pos>& crystals = work.list;
    crystals.clear();
    for (i32 z = origin.z + g.min_gen_offset; z <= origin.z + g.max_gen_offset; ++z)
        for (i32 y = origin.y + g.min_gen_offset; y <= origin.y + g.max_gen_offset; ++y)
            for (i32 x = origin.x + g.min_gen_offset; x <= origin.x + g.max_gen_offset; ++x) {
                const Pos p{x, y, z};
                const double noise_offset =
                    static_cast<double>(noise.get(static_cast<double>(x), static_cast<double>(y), static_cast<double>(z))) *
                    g.noise_multiplier;
                double shell = 0.0;
                for (i32 i = 0; i < count; ++i)
                    shell += 1.0 / std::sqrt(dist_sqr(p, points[i].pos) + static_cast<double>(points[i].offset)) + noise_offset;
                if (shell < outer_crust) continue;
                if (shell >= inner_air) {
                    safe_set(p, state_of(ft::kProviders[g.filling], random, p));
                    continue;
                }
                double cracked = 0.0;
                for (u32 i = 0; i < crack_count; ++i)
                    cracked +=
                        1.0 / std::sqrt(dist_sqr(p, crack_points[i]) + static_cast<double>(g.crack_point_offset)) + noise_offset;
                if (crack && cracked >= crack_size) {
                    safe_set(p, world::kAir);
                    for (u32 d = 0; d < 6; ++d) {
                        const Pos beside = moved(p, d);
                        const world::FluidState fluid = world::fluid_of(region.block(beside.x, beside.y, beside.z));
                        if (world::fluid_type(fluid) != world::fluid::kEmpty)
                            region.schedule_tick(beside.x, beside.y, beside.z, world::fluid_type(fluid), true);
                    }
                    continue;
                }
                if (shell >= innermost) {
                    const bool alternate = static_cast<double>(random.next_float()) < g.use_alternate_layer0_chance;
                    safe_set(p, state_of(ft::kProviders[alternate ? g.alternate_inner_layer : g.inner_layer], random, p));
                    if ((!g.placements_require_layer0_alternate || alternate) &&
                        static_cast<double>(random.next_float()) < g.use_potential_placements_chance)
                        crystals.push_back(p);
                } else if (shell >= inner_crust) {
                    safe_set(p, state_of(ft::kProviders[g.middle_layer], random, p));
                } else if (shell >= outer_crust) {
                    safe_set(p, state_of(ft::kProviders[g.outer_layer], random, p));
                }
            }
    // What grows inside: on the first side of each chosen block that has room.
    constexpr u32 kFacing[6] = {5, 4, 0, 2, 3, 1};  // a direction as BlockStateProperties.FACING counts it
    for (usize i = 0; i < crystals.size(); ++i) {
        const Pos at = crystals[i];
        u16 state = ft::kProviderStates[g.inner_placements + static_cast<u32>(random.next_int(g.inner_placement_count))];
        for (u32 d = 0; d < 6; ++d) {
            state = prop::with(state, Prop::kFacing, kFacing[d]);
            const Pos to = moved(at, d);
            const u16 there = region.block(to.x, to.y, to.z);
            if (prop::has(state, Prop::kWaterlogged))
                state = prop::with_flag(state, Prop::kWaterlogged, in_state_set(ft::kSetFluidSource, there));
            // BuddingAmethystBlock.canClusterGrowAtState
            if (!(world::is_air(there) || (is_block(there, blk::kWater) && in_state_set(ft::kSetFluidFull, there)))) continue;
            safe_set(to, state);
            break;
        }
    }
    return true;
}

// ------------------------------------------------------------------------- MultifaceGrowthFeature

bool Decorator::place_multiface_growth(const ft::Multiface& m, FeatureRandom& random, Pos origin) {
    Region& region = region_;
    const auto air_or_water = [](u16 state) { return world::is_air(state) || is_block(state, blk::kWater); };
    if (!air_or_water(region.block(origin.x, origin.y, origin.z))) return false;
    const Spreader spreader{region, m.block, m.sculk, false};
    // validDirections: up to a ceiling, down to a floor, level to a wall.
    u32 valid[6], valid_count = 0;
    if (m.ceiling) valid[valid_count++] = world::kUp;
    if (m.floor) valid[valid_count++] = world::kDown;
    if (m.wall)
        for (const u32 d : kHorizontal) valid[valid_count++] = d;
    // placeGrowthIfPossible
    const auto grow = [&](Pos p, u16 old, const u32* directions, u32 count) {
        for (u32 i = 0; i < count; ++i) {
            const u32 direction = directions[i];
            const Pos beside = moved(p, direction);
            if (!in_state_set(m.can_be_placed_on, region.block(beside.x, beside.y, beside.z))) continue;
            const i32 made = spreader.state_for_placement(old, p, direction);
            if (made < 0) return false;
            region.set_block(p.x, p.y, p.z, static_cast<u16>(made), 3);
            region.mark_post_process(p.x, p.y, p.z);
            if (random.next_float() < m.chance_of_spreading) {
                // MultifaceSpreader.spreadFromFaceTowardRandomDirection: the first way that takes.
                u32 ways[6];
                all_shuffled(ways, random);
                for (const u32 way : ways)
                    if (spreader.spread_from_face_toward(static_cast<u16>(made), p, direction, way, true)) break;
            }
            return true;
        }
        return false;
    };
    u32 search[6];
    std::copy(valid, valid + valid_count, search);
    shuffle(search, static_cast<i32>(valid_count), random);
    if (grow(origin, region.block(origin.x, origin.y, origin.z), search, valid_count)) return true;
    for (u32 s = 0; s < valid_count; ++s) {
        const u32 direction = search[s];
        // getShuffledDirectionsExcept: every way but back.
        u32 placement[6], placement_count = 0;
        for (u32 i = 0; i < valid_count; ++i)
            if (valid[i] != world::opposite(direction)) placement[placement_count++] = valid[i];
        shuffle(placement, static_cast<i32>(placement_count), random);
        // (The block beside the origin every time: vanilla's search does not go on from where it was.)
        const Pos p = moved(origin, direction);
        for (i32 i = 0; i < m.search_range; ++i) {
            const u16 state = region.block(p.x, p.y, p.z);
            if (!air_or_water(state) && !is_block(state, m.block)) break;
            if (grow(p, state, placement, placement_count)) return true;
        }
    }
    return false;
}

// ------------------------------------------------------------------------------ SculkPatchFeature

namespace {

// SculkSpreader.createWorldGenSpreader, its cursors, and the SculkBehaviour of the blocks they pass.
struct Sculk {
    Region& region;
    FeatureRandom& random;
    FeatureWork& work;
    Pos origin;

    static constexpr i32 kGrowthSpawnCost = 50, kNoGrowthRadius = 1, kChargeDecayRate = 5, kAdditionalDecayRate = 10;
    enum : u32 { kDefault, kSculkBlock, kVein };
    using Cursor = FeatureWork::SculkCursor;

    u16 at(Pos p) const { return region.block(p.x, p.y, p.z); }
    static u32 behaviour(u16 state) {
        return is_block(state, blk::kSculk) ? kSculkBlock : is_block(state, blk::kSculkVein) ? kVein : static_cast<u32>(kDefault);
    }
    Spreader vein_spreader() const { return {region, static_cast<u16>(blk::kSculkVein), true, false}; }
    Spreader same_space_spreader() const { return {region, static_cast<u16>(blk::kSculkVein), true, true}; }

    // SculkVeinBlock.onDischarged: the sides that lie on sculk now go, and the vein with its last side.
    void vein_discharged(u16 state, Pos p) {
        if (!is_block(state, blk::kSculkVein)) return;
        for (u32 d = 0; d < 6; ++d) {
            const Pos beside = moved(p, d);
            if (has_face(state, d) && is_block(at(beside), blk::kSculk)) state = prop::with_flag(state, prop::side_of(d), false);
        }
        if (!has_any_face(state))
            state =
                static_cast<u16>(world::fluid_type(world::fluid_of(at(p))) == world::fluid::kEmpty ? world::kAir : blk::kWater);
        region.set_block(p.x, p.y, p.z, state, 3);
    }
    void discharged(u32 kind, u16 state, Pos p) {
        if (kind == kVein) vein_discharged(state, p);
    }

    // SculkBehaviour.attemptSpreadVein
    bool attempt_spread_vein(u32 kind, Pos p, u16 state, i32 facings) {
        if (kind != kDefault) return vein_spreader().spread_all(state, p, true) > 0;
        if (facings < 0) return same_space_spreader().spread_all(at(p), p, true) > 0;
        if (facings == 0) return vein_spreader().spread_all(state, p, true) > 0;
        if (!(world::is_air(state) || in_state_set(ft::kSetWaterSource, state))) return false;
        // SculkVeinBlock.regrow
        u16 made = static_cast<u16>(blk::kSculkVein);
        bool any = false;
        for (u32 d = 0; d < 6; ++d) {
            if (!(facings >> d & 1)) continue;
            const Pos beside = moved(p, d);
            if (!in_state_set(ft::kSetVineAttaches[d], at(beside))) continue;
            made = prop::with_flag(made, prop::side_of(d), true);
            any = true;
        }
        if (!any) return false;
        if (world::fluid_type(world::fluid_of(state)) != world::fluid::kEmpty)
            made = prop::with_flag(made, Prop::kWaterlogged, true);
        region.set_block(p.x, p.y, p.z, made, 3);
        return true;
    }

    // SculkBlock.attemptUseCharge: the charge wears off, or grows a sensor - a shrieker, now and then.
    i32 sculk_use_charge(const Cursor& cursor) {
        const i32 charge = cursor.charge;
        if (charge == 0 || random.next_int(kChargeDecayRate) != 0) return charge;
        const Pos p{cursor.x, cursor.y, cursor.z};
        const double dx = static_cast<double>(p.x) - static_cast<double>(origin.x),
                     dy = static_cast<double>(p.y) - static_cast<double>(origin.y),
                     dz = static_cast<double>(p.z) - static_cast<double>(origin.z);
        const double dist_sqr = dx * dx + dy * dy + dz * dz;
        const bool close = dist_sqr < static_cast<double>(kNoGrowthRadius) * static_cast<double>(kNoGrowthRadius);
        bool can_grow = false;
        if (!close) {
            // canPlaceGrowth: room above, and no more than two of what keeps growth away in the box around.
            const u16 above = at({p.x, p.y + 1, p.z});
            if (world::is_air(above) || (is_block(above, blk::kWater) && in_state_set(ft::kSetWaterSource, above))) {
                u32 inhibitors = 0;
                for (i32 y = p.y; y <= p.y + 2 && inhibitors < 3; ++y)
                    for (i32 z = p.z - 4; z <= p.z + 4 && inhibitors < 3; ++z)
                        for (i32 x = p.x - 4; x <= p.x + 4 && inhibitors < 3; ++x)
                            inhibitors += in_state_set(ft::kSetSculkGrowthInhibitors, at({x, y, z}));
                can_grow = inhibitors <= 2;
            }
        }
        if (close || !can_grow) {
            if (random.next_int(kAdditionalDecayRate) != 0) return charge;
            if (close) return charge - 1;
            // getDecayPenalty
            const float outer = static_cast<float>(std::sqrt(dist_sqr)) - static_cast<float>(kNoGrowthRadius);
            const float outer_squared = outer * outer;
            const i32 reach = 24 - kNoGrowthRadius;
            const float factor = std::min(1.0f, outer_squared / static_cast<float>(reach * reach));
            return charge - std::max(1, static_cast<i32>(static_cast<float>(charge) * factor * 0.5f));
        }
        if (random.next_int(kGrowthSpawnCost) < charge) {
            const Pos above{p.x, p.y + 1, p.z};
            // getRandomGrowthState
            u16 growth = random.next_int(11) == 0 ? prop::with_flag(static_cast<u16>(blk::kSculkShrieker), Prop::kCanSummon, true)
                                                  : static_cast<u16>(blk::kSculkSensor);
            if (prop::has(growth, Prop::kWaterlogged) && world::fluid_type(world::fluid_of(at(above))) != world::fluid::kEmpty)
                growth = prop::with_flag(growth, Prop::kWaterlogged, true);
            region.set_block(above.x, above.y, above.z, growth, 3);
        }
        return std::max(0, charge - kGrowthSpawnCost);
    }
    // SculkVeinBlock.attemptUseCharge, attemptPlaceSculk: what the vein lies on becomes sculk.
    i32 vein_use_charge(const Cursor& cursor, bool spread_veins) {
        const Pos p{cursor.x, cursor.y, cursor.z};
        if (spread_veins) {
            const u16 state = at(p);
            u32 ways[6];
            all_shuffled(ways, random);
            for (const u32 support : ways) {
                if (!has_face(state, support)) continue;
                const Pos on = moved(p, support);
                if (!in_state_set(ft::kSetSculkReplaceableWorldGen, at(on))) continue;
                region.set_block(on.x, on.y, on.z, static_cast<u16>(blk::kSculk), 3);
                vein_spreader().spread_all(static_cast<u16>(blk::kSculk), on, true);
                for (u32 d = 0; d < 6; ++d) {
                    if (d == world::opposite(support)) continue;
                    const Pos beside = moved(on, d);
                    const u16 possible = at(beside);
                    if (is_block(possible, blk::kSculkVein)) vein_discharged(possible, beside);
                }
                return cursor.charge - 1;
            }
        }
        return random.next_int(kChargeDecayRate) == 0 ? floor_int(static_cast<float>(cursor.charge) * 0.5f) : cursor.charge;
    }
    i32 use_charge(u32 kind, const Cursor& cursor, bool spread_veins) {
        switch (kind) {
            case kSculkBlock: return sculk_use_charge(cursor);
            case kVein: return vein_use_charge(cursor, spread_veins);
            default: return cursor.decay_delay > 0 ? cursor.charge : 0;
        }
    }

    // ChargeCursor.getValidMovementPos: a neighbour - not across a corner - of sculk or vein that the charge can get to.
    bool unobstructed(Pos from, u32 direction) const {
        return !in_state_set(ft::kSetSturdy[world::opposite(direction)], at(moved(from, direction)));
    }
    bool movement_unobstructed(Pos from, Pos to) const {
        if (manhattan(from, to) == 1) return true;
        const i32 dx = to.x - from.x, dy = to.y - from.y, dz = to.z - from.z;
        const u32 x = dx < 0 ? world::kWest : world::kEast, y = dy < 0 ? world::kDown : world::kUp,
                  z = dz < 0 ? world::kNorth : world::kSouth;
        if (dx == 0) return unobstructed(from, y) || unobstructed(from, z);
        if (dy == 0) return unobstructed(from, x) || unobstructed(from, z);
        return unobstructed(from, x) || unobstructed(from, y);
    }
    bool valid_movement_pos(Pos p, Pos& out) {
        // NON_CORNER_NEIGHBOURS, shuffled: the 18 of the 26 around that are not across a corner.
        Pos offsets[18];
        u32 n = 0;
        for (i32 z = -1; z <= 1; ++z)
            for (i32 y = -1; y <= 1; ++y)
                for (i32 x = -1; x <= 1; ++x)
                    if ((x == 0 || y == 0 || z == 0) && !(x == 0 && y == 0 && z == 0)) offsets[n++] = {x, y, z};
        shuffle(offsets, 18, random);
        Pos found = p;
        for (const Pos& offset : offsets) {
            const Pos to{p.x + offset.x, p.y + offset.y, p.z + offset.z};
            const i32 dx = origin.x - to.x, dz = origin.z - to.z;
            if (dx * dx + dz * dz > 144) continue;  // canMoveToPos, as a world is made
            const u16 state = at(to);
            if (behaviour(state) == kDefault || !movement_unobstructed(p, to)) continue;
            found = to;
            // SculkVeinBlock.hasSubstrateAccess
            bool access = false;
            if (is_block(state, blk::kSculkVein))
                for (u32 d = 0; d < 6 && !access; ++d)
                    access = has_face(state, d) && in_state_set(ft::kSetSculkReplaceable, at(moved(to, d)));
            if (access) break;
        }
        if (same(found, p)) return false;
        out = found;
        return true;
    }

    // ChargeCursor.update
    void update(Cursor& cursor, bool spread_veins) {
        if (cursor.charge <= 0) return;
        if (cursor.update_delay > 0) {
            --cursor.update_delay;
            return;
        }
        Pos p{cursor.x, cursor.y, cursor.z};
        u16 state = at(p);
        u32 kind = behaviour(state);
        if (spread_veins && attempt_spread_vein(kind, p, state, cursor.facings) && kind != kSculkBlock) {
            state = at(p);  // canChangeBlockStateOnSpread
            kind = behaviour(state);
        }
        cursor.charge = use_charge(kind, cursor, spread_veins);
        if (cursor.charge <= 0) {
            discharged(kind, state, p);
            return;
        }
        Pos to;
        const bool moves = valid_movement_pos(p, to);
        discharged(kind, state, p);
        if (!moves) {
            cursor.charge = 0;
            return;
        }
        cursor.x = to.x, cursor.y = to.y, cursor.z = to.z;
        state = at(to);
        if (behaviour(state) != kDefault) cursor.facings = static_cast<i32>(available_faces(state));
        cursor.decay_delay = kind == kDefault ? std::max(cursor.decay_delay - 1, 0) : 1;  // updateDecayDelay
        cursor.update_delay = 1;                                                          // getSculkSpreadDelay
    }
};

}  // namespace

bool Decorator::place_sculk_patch(const ft::SculkPatch& patch, FeatureRandom& random, Pos origin) {
    Region& region = region_;
    // canSpreadFrom: sculk, or room with something whole beside it.
    const u16 start = region.block(origin.x, origin.y, origin.z);
    if (Sculk::behaviour(start) == Sculk::kDefault) {
        if (!(world::is_air(start) || (is_block(start, blk::kWater) && in_state_set(ft::kSetFluidSource, start)))) return false;
        bool beside = false;
        for (u32 d = 0; d < 6 && !beside; ++d) {
            const Pos p = moved(origin, d);
            beside = in_state_set(ft::kSetCollisionFull, region.block(p.x, p.y, p.z));
        }
        if (!beside) return false;
    }
    Sculk sculk{region, random, work_, origin};
    std::vector<FeatureWork::SculkCursor>& cursors = work_.cursors;
    const i32 rounds = patch.spread_rounds + patch.growth_rounds;
    for (i32 round = 0; round < rounds; ++round) {
        cursors.clear();
        for (i32 i = 0; i < patch.charge_count; ++i)
            for (i32 charge = patch.amount_per_charge;
                 charge > 0;) {  // SculkSpreader.addCursors: a thousand a cursor, 32 cursors
                const i32 of = std::min(charge, 1000);
                // (ChargeCursor(pos, charge): it may decay once before it has to have found sculk, and updates at once.)
                if (cursors.size() < 32) cursors.push_back({origin.x, origin.y, origin.z, of, 0, 1, -1});
                charge -= of;
            }
        const bool spread_veins = round < patch.spread_rounds;
        for (i32 i = 0; i < patch.spread_attempts; ++i) {
            // SculkSpreader.updateCursors: every cursor has its turn, and one that is spent is gone.
            usize kept = 0;
            for (usize c = 0; c < cursors.size(); ++c) {
                FeatureWork::SculkCursor cursor = cursors[c];
                if (std::max(std::max(std::abs(cursor.x - origin.x), std::abs(cursor.y - origin.y)),
                             std::abs(cursor.z - origin.z)) > 1024)
                    continue;
                sculk.update(cursor, spread_veins);
                if (cursor.charge > 0) cursors[kept++] = cursor;
            }
            cursors.resize(kept);
        }
    }
    cursors.clear();
    return true;
}

// ----------------------------------------------------------------------------- MonsterRoomFeature

namespace {

// What a block entity saves besides what it holds, as a chunk in the making writes it
// (BlockEntity.saveWithFullMetadata).
void end_block_entity(nbt::Writer& w, std::string_view id, Pos p) {
    w.begin_compound("components");
    w.end_compound();
    w.put_string("id", id);
    w.put_int("x", p.x);
    w.put_int("y", p.y);
    w.put_int("z", p.z);
    w.end_compound();
}

}  // namespace

// A room of cobblestone round a spawner, with a chest or two - where there is a hollow with a
// floor, a ceiling and a few ways in.
bool Decorator::place_monster_room(FeatureRandom& random, Pos origin) {
    Region& region = region_;
    const auto at = [&](i32 x, i32 y, i32 z) { return region.block(x, y, z); };
    const auto solid = [&](i32 x, i32 y, i32 z) { return in_state_set(ft::kSetSolid, at(x, y, z)); };
    const auto safe_set = [&](Pos p, u16 state) {  // Feature.safeSetBlock, but for what features may not replace
        if (in_state_set(ft::kSetFeaturesCannotReplace, at(p.x, p.y, p.z))) return false;
        return region.set_block(p.x, p.y, p.z, state, 2);
    };
    const i32 xr = random.next_int(2) + 2;
    const i32 min_x = -xr - 1, max_x = xr + 1;
    const i32 zr = random.next_int(2) + 2;
    const i32 min_z = -zr - 1, max_z = zr + 1;
    i32 holes = 0;
    for (i32 dx = min_x; dx <= max_x; ++dx)
        for (i32 dy = -1; dy <= 4; ++dy)
            for (i32 dz = min_z; dz <= max_z; ++dz) {
                const Pos p{origin.x + dx, origin.y + dy, origin.z + dz};
                const bool is_solid = solid(p.x, p.y, p.z);
                if ((dy == -1 || dy == 4) && !is_solid) return false;
                if ((dx == min_x || dx == max_x || dz == min_z || dz == max_z) && dy == 0 && world::is_air(at(p.x, p.y, p.z)) &&
                    world::is_air(at(p.x, p.y + 1, p.z)))
                    ++holes;
            }
    if (holes < 1 || holes > 5) return false;
    const u16 air = static_cast<u16>(blk::kCaveAir);
    for (i32 dx = min_x; dx <= max_x; ++dx)
        for (i32 dy = 3; dy >= -1; --dy)
            for (i32 dz = min_z; dz <= max_z; ++dz) {
                const Pos p{origin.x + dx, origin.y + dy, origin.z + dz};
                const u16 state = at(p.x, p.y, p.z);
                if (dx == min_x || dy == -1 || dz == min_z || dx == max_x || dy == 4 || dz == max_z) {
                    if (p.y >= Region::kMinY && !solid(p.x, p.y - 1, p.z)) {
                        region.set_block(p.x, p.y, p.z, air, 2);
                    } else if (in_state_set(ft::kSetSolid, state) && !is_block(state, blk::kChest)) {
                        if (dy == -1 && random.next_int(4) != 0)
                            safe_set(p, static_cast<u16>(blk::kMossyCobblestone));
                        else
                            safe_set(p, static_cast<u16>(blk::kCobblestone));
                    }
                } else if (!is_block(state, blk::kChest) && !is_block(state, blk::kSpawner)) {
                    safe_set(p, air);
                }
            }
    base::ByteBuf& tag = work_.tag;
    for (i32 chest = 0; chest < 2; ++chest)
        for (i32 attempt = 0; attempt < 3; ++attempt) {
            const i32 x = origin.x + random.next_int(xr * 2 + 1) - xr;
            const i32 z = origin.z + random.next_int(zr * 2 + 1) - zr;
            const Pos p{x, origin.y, z};
            if (!world::is_air(at(p.x, p.y, p.z))) continue;
            i32 walls = 0;
            for (const u32 d : kHorizontal) walls += solid(p.x + world::kStepX[d], p.y, p.z + world::kStepZ[d]);
            if (walls != 1) continue;
            // StructurePiece.reorient: away from the one wall beside it - or, with none or several, from whatever is behind.
            u16 state = static_cast<u16>(blk::kChest);
            const auto solid_render = [&](u32 d) {
                return in_state_set(ft::kSetSolidRender, at(p.x + world::kStepX[d], p.y, p.z + world::kStepZ[d]));
            };
            const auto clockwise = [](u32 d) {
                return d == world::kNorth   ? world::kEast
                       : d == world::kEast  ? world::kSouth
                       : d == world::kSouth ? world::kWest
                                            : world::kNorth;
            };
            bool beside_chest = false, several = false;
            i32 wall = -1;
            for (const u32 d : kHorizontal) {
                if (is_block(at(p.x + world::kStepX[d], p.y, p.z + world::kStepZ[d]), blk::kChest)) {
                    beside_chest = true;
                    break;
                }
                if (!solid_render(d)) continue;
                if (wall >= 0) {
                    several = true;
                    break;
                }
                wall = static_cast<i32>(d);
            }
            if (!beside_chest) {
                if (wall >= 0 && !several) {
                    state = prop::with(state, Prop::kHorizontalFacing, prop::facing_of(world::opposite(static_cast<u32>(wall))));
                } else {
                    u32 lock = prop::direction_of(prop::get(state, Prop::kHorizontalFacing));
                    if (solid_render(lock)) lock = world::opposite(lock);
                    if (solid_render(lock)) lock = clockwise(lock);
                    if (solid_render(lock)) lock = world::opposite(lock);
                    state = prop::with(state, Prop::kHorizontalFacing, prop::facing_of(lock));
                }
            }
            // RandomizableContainer.setBlockEntityLootTable: the chest's loot is drawn when it is first opened, from this seed.
            if (safe_set(p, state) || is_block(at(p.x, p.y, p.z), blk::kChest)) {
                if (region.block_entity(p.x, p.y, p.z)) {
                    const i64 seed = random.next_long();
                    tag.len = 0;
                    nbt::Writer w(tag);
                    w.root(nbt::kCompound);
                    w.put_string("LootTable", "minecraft:chests/simple_dungeon");
                    w.put_long("LootTableSeed", seed);
                    end_block_entity(w, "minecraft:chest", p);
                    region.set_block_entity_tag(p.x, p.y, p.z, {tag.data, tag.len});
                }
            }
            break;
        }
    safe_set(origin, static_cast<u16>(blk::kSpawner));
    if (is_block(at(origin.x, origin.y, origin.z), blk::kSpawner) && region.block_entity(origin.x, origin.y, origin.z)) {
        // SpawnerBlockEntity.setEntityId: a skeleton, a zombie (twice as likely) or a spider.
        constexpr std::string_view kMobs[4] = {"minecraft:skeleton", "minecraft:zombie", "minecraft:zombie", "minecraft:spider"};
        const std::string_view mob = kMobs[random.next_int(4)];
        tag.len = 0;
        nbt::Writer w(tag);
        w.root(nbt::kCompound);
        w.put_short("Delay", 20);
        w.put_short("MinSpawnDelay", 200);
        w.put_short("MaxSpawnDelay", 800);
        w.put_short("SpawnCount", 4);
        w.put_short("MaxNearbyEntities", 6);
        w.put_short("RequiredPlayerRange", 16);
        w.put_short("SpawnRange", 4);
        w.begin_compound("SpawnData");
        w.begin_compound("entity");
        w.put_string("id", mob);
        w.end_compound();
        w.end_compound();
        w.begin_list("SpawnPotentials", nbt::kEnd, 0);
        end_block_entity(w, "minecraft:mob_spawner", origin);
        region.set_block_entity_tag(origin.x, origin.y, origin.z, {tag.data, tag.len});
    }
    return true;
}

}  // namespace worldgen
