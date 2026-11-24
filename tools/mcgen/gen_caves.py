# SPDX-License-Identifier: GPL-3.0-or-later
"""The tables of what grows in caves (gen_features.py's FeatureTables is one of these too): patches of moss and
clay with what stands on them, the roots of an azalea, dripstone small and large, geodes, lichen and sculk.

As everywhere in the features' tables: what a type *does* is a function of src/worldgen/cave.cpp, written
from its class; here its numbers become a row, and a field this does not know stops the generator.
"""
from __future__ import annotations

import struct

from mcgen import die


def cfloat(v: float) -> str:
    return struct.unpack("<f", struct.pack("<f", v))[0].hex() + "f"


def cdouble(v: float) -> str:
    return float(v).hex()


class CaveTables:
    def init_caves(self) -> None:
        from gen_features import Pool
        self.floats = Pool()
        self.patches, self.root_systems, self.speleothems, self.clusters = Pool(), Pool(), Pool(), Pool()
        self.large_dripstones, self.geodes, self.multifaces, self.sculk_patches = Pool(), Pool(), Pool(), Pool()

    # ---------------------------------------------------------------- providers of numbers

    def float_provider(self, value) -> int:
        """A FloatProvider: a number, or {type, ...}. A row of kFloats."""
        if isinstance(value, (int, float)):
            return self.floats.add(("kConstant", float(value), 0.0, 0.0, 0.0))
        kind = self.kind("a provider of floats", value)
        if kind == "constant":
            self.need(kind, value, {"value"})
            return self.floats.add(("kConstant", float(value["value"]), 0.0, 0.0, 0.0))
        if kind == "uniform":
            self.need(kind, value, {"min_inclusive", "max_exclusive"})
            return self.floats.add(("kUniform", float(value["min_inclusive"]), float(value["max_exclusive"]), 0.0, 0.0))
        if kind == "clamped_normal":
            self.need(kind, value, {"mean", "deviation", "min", "max"})
            return self.floats.add(("kClampedNormal", float(value["mean"]), float(value["deviation"]), float(value["min"]), float(value["max"])))
        if kind == "trapezoid":
            self.need(kind, value, {"min", "max", "plateau"})
            return self.floats.add(("kTrapezoid", float(value["min"]), float(value["max"]), float(value["plateau"]), 0.0))
        die(f"gen_features: a provider of floats of the type {kind}")

    def int_bounds(self, value) -> tuple[int, int]:
        """IntProvider.minInclusive / maxInclusive, for the features that ask a provider how far it goes."""
        if isinstance(value, int):
            return value, value
        kind = self.kind("a provider of numbers", value)
        if kind == "constant":
            return int(value["value"]), int(value["value"])
        if kind in ("uniform", "clamped", "biased_to_bottom"):
            return int(value["min_inclusive"]), int(value["max_inclusive"])
        die(f"gen_features: how far a provider of numbers of the type {kind} goes is not known")

    def block_set(self, value) -> int:
        """A HolderSet of blocks as a set of states (the predicate that names the same blocks is its name)."""
        return self.sets.keep(self.sets.blocks_in(value), {"type": "minecraft:matching_blocks", "blocks": value})

    # ---------------------------------------------------------------- the features

    def vegetation_patch(self, value, waterlogged: bool) -> tuple[tuple, list[int], bool]:
        self.need("vegetation_patch", value, {"replaceable", "ground_state", "vegetation_feature", "surface", "depth", "extra_bottom_block_chance",
                                              "vertical_range", "vegetation_chance", "xz_radius", "extra_edge_column_chance"})
        if value["surface"] not in ("floor", "ceiling"):
            die(f"gen_features: a patch on the {value['surface']} of a cave")
        ground, has = self.provider(value["ground_state"])
        vegetation = self.placed_ref(value["vegetation_feature"])
        row = (self.block_set(value["replaceable"]), ground, vegetation, int(value["surface"] == "ceiling"), self.int_provider(value["depth"]),
               float(value["extra_bottom_block_chance"]), int(value["vertical_range"]), float(value["vegetation_chance"]),
               self.int_provider(value["xz_radius"]), float(value["extra_edge_column_chance"]), int(waterlogged))
        return ("kVegetationPatch", self.patches.run([row]), 0, 0), [vegetation], has

    def root_system(self, value) -> tuple[tuple, list[int], bool]:
        ints = ["required_vertical_space_for_tree", "level_test_distance", "max_level_deviation", "root_radius", "root_placement_attempts",
                "root_column_max_height", "hanging_root_radius", "hanging_roots_vertical_span", "hanging_root_placement_attempts",
                "allowed_vertical_water_for_tree"]
        self.need("root_system", value, {"feature", "root_replaceable", "root_state_provider", "hanging_root_state_provider", "allowed_tree_position", *ints})
        tree = self.placed_ref(value["feature"])
        roots, has_roots = self.provider(value["root_state_provider"])
        hanging, has_hanging = self.provider(value["hanging_root_state_provider"])
        allowed, has_allowed = self.predicate(value["allowed_tree_position"])
        # What may come to hang, and the rule it stays by (RootSystemFeature.placeRoots asks canSurvive).
        first = len(self.simple_blocks.rows)
        has = has_roots and has_hanging and has_allowed
        for state in sorted(self.outcomes(value["hanging_root_state_provider"])):
            rule, has_rule = self.survive_rule(state)
            has = has and has_rule
            self.simple_blocks.rows.append((state, rule, 0, 0, 0, 0))
        row = (tree, *[int(value[k]) for k in ints[:4]], self.block_set(value["root_replaceable"]), roots, *[int(value[k]) for k in ints[4:8]], hanging,
               int(value["hanging_root_placement_attempts"]), int(value["allowed_vertical_water_for_tree"]), allowed, first, len(self.simple_blocks.rows) - first)
        return ("kRootSystem", self.root_systems.run([row]), 0, 0), [tree], has

    def _blocks_of(self, value, what: str) -> int:
        """A BlockState that a feature only ever takes the block of: the block's default state."""
        name = value if isinstance(value, str) else value.get("id") or value.get("Name")
        if name not in self.blocks.blocks:
            die(f"gen_features: {what} of {value!r}")
        return self.blocks.state(name)

    def speleothem(self, value) -> tuple:
        self.need("speleothem", value, {"base_block", "pointed_block", "replaceable_blocks"},
                  {"chance_of_taller_generation", "chance_of_directional_spread", "chance_of_spread_radius2", "chance_of_spread_radius3"})
        row = (self._blocks_of(value["base_block"], "a base"), self._blocks_of(value["pointed_block"], "a point"), self.block_set(value["replaceable_blocks"]),
               float(value.get("chance_of_taller_generation", 0.2)), float(value.get("chance_of_directional_spread", 0.7)),
               float(value.get("chance_of_spread_radius2", 0.5)), float(value.get("chance_of_spread_radius3", 0.5)))
        return ("kSpeleothem", self.speleothems.run([row]), 0, 0)

    def speleothem_cluster(self, value) -> tuple:
        self.need("speleothem_cluster", value, {
            "base_block", "pointed_block", "replaceable_blocks", "floor_to_ceiling_search_range", "height", "radius", "max_stalagmite_stalactite_height_diff",
            "height_deviation", "speleothem_block_layer_thickness", "density", "wetness", "chance_of_speleothem_at_max_distance_from_center",
            "max_distance_from_edge_affecting_chance_of_speleothem", "max_distance_from_center_affecting_height_bias"})
        row = (self._blocks_of(value["base_block"], "a base"), self._blocks_of(value["pointed_block"], "a point"), self.block_set(value["replaceable_blocks"]),
               int(value["floor_to_ceiling_search_range"]), self.int_provider(value["height"]), self.int_provider(value["radius"]),
               int(value["max_stalagmite_stalactite_height_diff"]), int(value["height_deviation"]), self.int_provider(value["speleothem_block_layer_thickness"]),
               self.float_provider(value["density"]), self.float_provider(value["wetness"]), float(value["chance_of_speleothem_at_max_distance_from_center"]),
               int(value["max_distance_from_edge_affecting_chance_of_speleothem"]), int(value["max_distance_from_center_affecting_height_bias"]))
        return ("kSpeleothemCluster", self.clusters.run([row]), 0, 0)

    def large_dripstone(self, value) -> tuple:
        self.need("large_dripstone", value, {"replaceable_blocks", "column_radius", "height_scale", "max_column_radius_to_cave_height_ratio",
                                             "stalactite_bluntness", "stalagmite_bluntness", "wind_speed", "min_radius_for_wind", "min_bluntness_for_wind"},
                  {"floor_to_ceiling_search_range"})
        low, high = self.int_bounds(value["column_radius"])
        row = (self.block_set(value["replaceable_blocks"]), int(value.get("floor_to_ceiling_search_range", 30)), low, high,
               self.float_provider(value["height_scale"]), float(value["max_column_radius_to_cave_height_ratio"]),
               self.float_provider(value["stalactite_bluntness"]), self.float_provider(value["stalagmite_bluntness"]), self.float_provider(value["wind_speed"]),
               int(value["min_radius_for_wind"]), float(value["min_bluntness_for_wind"]))
        return ("kLargeDripstone", self.large_dripstones.run([row]), 0, 0)

    def geode(self, value) -> tuple[tuple, bool]:
        self.need("geode", value, {"blocks", "layers", "crack", "invalid_blocks_threshold"},
                  {"use_potential_placements_chance", "use_alternate_layer0_chance", "placements_require_layer0_alternate", "outer_wall_distance",
                   "distribution_points", "point_offset", "min_gen_offset", "max_gen_offset", "noise_multiplier"})
        blocks, layers, crack = value["blocks"], value["layers"], value["crack"]
        names = ["filling_provider", "inner_layer_provider", "alternate_inner_layer_provider", "middle_layer_provider", "outer_layer_provider"]
        if set(blocks) != {*names, "inner_placements", "cannot_replace", "invalid_blocks"}:
            die(f"gen_features: a geode's blocks written as {sorted(blocks)}")
        if not set(layers) <= {"filling", "inner_layer", "middle_layer", "outer_layer"} or not set(crack) <= {"generate_crack_chance", "base_crack_size", "crack_point_offset"}:
            die(f"gen_features: a geode's layers and crack written as {sorted(layers)}, {sorted(crack)}")
        made = [self.provider(blocks[name]) for name in names]
        placements = blocks["inner_placements"]
        if not isinstance(placements, list) or not placements:
            die("gen_features: a geode with nothing to grow inside (GeodeBlockSettings takes a list that is not empty)")
        first, count = self._states(placements)
        uniform = lambda lo, hi: {"type": "minecraft:uniform", "min_inclusive": lo, "max_inclusive": hi}
        outer = value.get("outer_wall_distance", uniform(4, 5))
        row = (*[at for at, _ in made], first, count, self.block_set(blocks["cannot_replace"]), self.block_set(blocks["invalid_blocks"]),
               float(layers.get("filling", 1.7)), float(layers.get("inner_layer", 2.2)), float(layers.get("middle_layer", 3.2)), float(layers.get("outer_layer", 4.2)),
               float(crack.get("generate_crack_chance", 1.0)), float(crack.get("base_crack_size", 2.0)), int(crack.get("crack_point_offset", 2)),
               float(value.get("use_potential_placements_chance", 0.35)), float(value.get("use_alternate_layer0_chance", 0.0)),
               int(bool(value.get("placements_require_layer0_alternate", True))), self.int_provider(outer),
               self.int_provider(value.get("distribution_points", uniform(3, 4))), self.int_provider(value.get("point_offset", uniform(1, 2))),
               self.int_bounds(outer)[1], int(value.get("min_gen_offset", -16)), int(value.get("max_gen_offset", 16)),
               float(value.get("noise_multiplier", 0.05)), int(value["invalid_blocks_threshold"]))
        return ("kGeode", self.geodes.run([row]), 0, 0), all(has for _, has in made)

    def multiface_growth(self, value) -> tuple:
        self.need("multiface_growth", value, {"block", "can_be_placed_on"},
                  {"search_range", "can_place_on_floor", "can_place_on_ceiling", "can_place_on_wall", "chance_of_spreading"})
        block = value["block"]
        if block not in ("minecraft:glow_lichen", "minecraft:sculk_vein"):
            die(f"gen_features: {block} grows on faces: src/worldgen/cave.cpp knows the lichen's way of spreading and the sculk vein's")
        row = (self.blocks.state(block), int(value.get("search_range", 10)), int(bool(value.get("can_place_on_floor", False))),
               int(bool(value.get("can_place_on_ceiling", False))), int(bool(value.get("can_place_on_wall", False))),
               float(value.get("chance_of_spreading", 0.5)), self.block_set(value["can_be_placed_on"]), int(block == "minecraft:sculk_vein"))
        return ("kMultifaceGrowth", self.multifaces.run([row]), 0, 0)

    def sculk_patch(self, value) -> tuple:
        names = ["charge_count", "amount_per_charge", "spread_attempts", "growth_rounds", "spread_rounds"]
        self.need("sculk_patch", value, set(names))
        return ("kSculkPatch", self.sculk_patches.run([tuple(int(value[n]) for n in names)]), 0, 0)


def emit_caves(t, out: list[str]) -> None:
    out.append("""
// ---------------------------------------------------------------------------------------------
// Caves (tools/mcgen/gen_caves.py): the features of src/worldgen/cave.cpp.

// FloatProvider.sample: a constant a; uniform from a to b; a normal number of mean a and deviation b
// kept within c .. d; a trapezoid from a to b with a plateau of c.
enum class FloatOp : u8 { kConstant, kUniform, kClampedNormal, kTrapezoid };
struct Float {
    FloatOp op;
    float a, b, c, d;
};
// VegetationPatchFeature, and WaterloggedVegetationPatchFeature.
struct Patch {
    u16 replaceable;   // a state set
    u16 ground_state;  // a provider
    u16 vegetation;    // a placed feature
    bool ceiling;      // CaveSurface: on the ceiling, or on the floor
    u16 depth;         // kInts
    float extra_bottom_block_chance;
    i32 vertical_range;
    float vegetation_chance;
    u16 xz_radius;  // kInts
    float extra_edge_column_chance;
    bool waterlogged;
};
// RootSystemFeature. kSimpleBlocks[hanging .. hanging + hanging_count): what may hang, each with the rule it stays by.
struct RootSystem {
    u16 tree;  // a placed feature
    i32 required_vertical_space_for_tree, level_test_distance, max_level_deviation, root_radius;
    u16 root_replaceable;  // a state set
    u16 root_provider;
    i32 root_placement_attempts, root_column_max_height, hanging_root_radius, hanging_roots_vertical_span;
    u16 hanging_root_provider;
    i32 hanging_root_placement_attempts, allowed_vertical_water_for_tree;
    u16 allowed_tree_position;  // a predicate
    u16 hanging, hanging_count;
};
// SpeleothemFeature, SpeleothemClusterFeature: the base and the pointed block as their default states.
struct Speleothem {
    u16 base_block, pointed_block;
    u16 replaceable;  // a state set
    float chance_of_taller_generation, chance_of_directional_spread, chance_of_spread_radius2, chance_of_spread_radius3;
};
struct SpeleothemCluster {
    u16 base_block, pointed_block;
    u16 replaceable;
    i32 floor_to_ceiling_search_range;
    u16 height, radius;  // kInts
    i32 max_stalagmite_stalactite_height_diff, height_deviation;
    u16 layer_thickness;   // kInts
    u16 density, wetness;  // kFloats
    float chance_at_max_distance_from_center;
    i32 max_distance_from_edge, max_distance_from_center;
};
// LargeDripstoneFeature. radius_min .. radius_max: how far its provider of radii goes.
struct LargeDripstone {
    u16 replaceable;
    i32 floor_to_ceiling_search_range;
    i32 radius_min, radius_max;
    u16 height_scale;  // kFloats
    float max_column_radius_to_cave_height_ratio;
    u16 stalactite_bluntness, stalagmite_bluntness, wind_speed;  // kFloats
    i32 min_radius_for_wind;
    float min_bluntness_for_wind;
};
// GeodeFeature. kProviderStates[inner_placements .. + inner_placement_count): what grows inside.
struct Geode {
    u16 filling, inner_layer, alternate_inner_layer, middle_layer, outer_layer;  // providers
    u16 inner_placements, inner_placement_count;
    u16 cannot_replace, invalid_blocks;  // state sets
    double filling_size, inner_layer_size, middle_layer_size, outer_layer_size;
    double generate_crack_chance, base_crack_size;
    i32 crack_point_offset;
    double use_potential_placements_chance, use_alternate_layer0_chance;
    bool placements_require_layer0_alternate;
    u16 outer_wall_distance, distribution_points, point_offset;  // kInts
    i32 outer_wall_distance_max;
    i32 min_gen_offset, max_gen_offset;
    double noise_multiplier;
    i32 invalid_blocks_threshold;
};
// MultifaceGrowthFeature: the block's default state; sculk: it spreads as a sculk vein does, not as lichen.
struct Multiface {
    u16 block;
    i32 search_range;
    bool floor, ceiling, wall;
    float chance_of_spreading;
    u16 can_be_placed_on;  // a state set
    bool sculk;
};
struct SculkPatch {
    i32 charge_count, amount_per_charge, spread_attempts, growth_rounds, spread_rounds;
};
""")
    flag = lambda v: "true" if v else "false"
    out.append(f"inline constexpr Float kFloats[{max(1, len(t.floats.rows))}] = {{"
               + ", ".join(f"{{FloatOp::{r[0]}, {cfloat(r[1])}, {cfloat(r[2])}, {cfloat(r[3])}, {cfloat(r[4])}}}" for r in t.floats.rows) + "};\n")
    out.append(f"inline constexpr Patch kPatches[{max(1, len(t.patches.rows))}] = {{\n")
    out += [f"    {{{r[0]}, {r[1]}, {r[2]}, {flag(r[3])}, {r[4]}, {cfloat(r[5])}, {r[6]}, {cfloat(r[7])}, {r[8]}, {cfloat(r[9])}, {flag(r[10])}}},\n" for r in t.patches.rows]
    out.append("};\n")
    out.append(f"inline constexpr RootSystem kRootSystems[{max(1, len(t.root_systems.rows))}] = {{"
               + ", ".join("{" + ", ".join(map(str, r)) + "}" for r in t.root_systems.rows) + "};\n")
    out.append(f"inline constexpr Speleothem kSpeleothems[{max(1, len(t.speleothems.rows))}] = {{"
               + ", ".join(f"{{{r[0]}, {r[1]}, {r[2]}, {cfloat(r[3])}, {cfloat(r[4])}, {cfloat(r[5])}, {cfloat(r[6])}}}" for r in t.speleothems.rows) + "};\n")
    out.append(f"inline constexpr SpeleothemCluster kSpeleothemClusters[{max(1, len(t.clusters.rows))}] = {{"
               + ", ".join("{" + ", ".join(map(str, r[:11])) + f", {cfloat(r[11])}, {r[12]}, {r[13]}}}" for r in t.clusters.rows) + "};\n")
    out.append(f"inline constexpr LargeDripstone kLargeDripstones[{max(1, len(t.large_dripstones.rows))}] = {{"
               + ", ".join(f"{{{r[0]}, {r[1]}, {r[2]}, {r[3]}, {r[4]}, {cfloat(r[5])}, {r[6]}, {r[7]}, {r[8]}, {r[9]}, {cfloat(r[10])}}}" for r in t.large_dripstones.rows)
               + "};\n")
    out.append(f"inline constexpr Geode kGeodes[{max(1, len(t.geodes.rows))}] = {{\n")
    for r in t.geodes.rows:
        out.append("    {" + ", ".join(map(str, r[:9])) + ", " + ", ".join(cdouble(v) for v in r[9:15]) + f", {r[15]}, {cdouble(r[16])}, {cdouble(r[17])}, {flag(r[18])}, "
                   + ", ".join(map(str, r[19:25])) + f", {cdouble(r[25])}, {r[26]}}},\n")
    out.append("};\n")
    out.append(f"inline constexpr Multiface kMultifaces[{max(1, len(t.multifaces.rows))}] = {{"
               + ", ".join(f"{{{r[0]}, {r[1]}, {flag(r[2])}, {flag(r[3])}, {flag(r[4])}, {cfloat(r[5])}, {r[6]}, {flag(r[7])}}}" for r in t.multifaces.rows) + "};\n")
    out.append(f"inline constexpr SculkPatch kSculkPatches[{max(1, len(t.sculk_patches.rows))}] = {{"
               + ", ".join("{" + ", ".join(map(str, r)) + "}" for r in t.sculk_patches.rows) + "};\n")
    # GeodeFeature.NOISE_PARAMETERS, as NormalNoise.create makes a noise of it: per layer how often and how much.
    layers = t.extra.get("geode_noise")
    if not layers:
        die("gen_features: worldgen/features.json does not have the geode's noise - run: mcgen.py worldgen")
    out.append("// GeodeFeature.NOISE_PARAMETERS as a stack of layers: two an octave, the first of one positional factory and\n"
               "// the second of the other (NormalNoise.create), seeded by the octave's name - from kGeodeFirstOctave on.\n")
    out.append(f"inline constexpr i32 kGeodeFirstOctave = {layers['first_octave']};\n")
    out.append("struct GeodeLayer {\n    double frequency;\n    float amplitude;\n};\n")
    out.append(f"inline constexpr GeodeLayer kGeodeLayers[{len(layers['layers'])}] = {{"
               + ", ".join(f"{{{struct.unpack('<d', struct.pack('<q', int(l['frequency'])))[0].hex()}, "
                           f"{struct.unpack('<f', struct.pack('<i', int(l['amplitude'])))[0].hex()}f}}" for l in layers["layers"]) + "};\n")
