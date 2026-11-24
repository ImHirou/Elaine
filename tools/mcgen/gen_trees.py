# SPDX-License-Identifier: GPL-3.0-or-later
"""The tables of trees (gen_features.py's FeatureTables is one of these too).

A tree is data all the way down: TreeFeature holds a trunk placer, a foliage placer, a size, maybe a root
placer, and decorators - each a type and a few numbers. What a type *does* is src/worldgen/tree.cpp, a
function a type; here its numbers become a row. A type or a field this does not know stops the generator.

And two tables for what the code of trees - and of the block updates at a tree's edge - asks of a block
state by itself:

  kPropStride   BlockState.getValue / setValue of the properties that code names: per property and block,
                by how much a state's id moves with the property's value (0: the block has no such property).
                A property is a name *and* its values, as vanilla's property objects are told apart.
  kShape...     BlockState.updateShape: which class's method a block has (gamedata: the hook's owner), for
                the classes src/worldgen/shape.hpp has a rule for - and "not ported" for the others.
"""
from __future__ import annotations

from mcgen import die

# The properties src/worldgen names: (enum, the property's name, its values in the order of the state ids).
PROPS = [
    ("kAxis", "axis", ["x", "y", "z"]),
    ("kDistance", "distance", ["1", "2", "3", "4", "5", "6", "7"]),
    ("kPersistent", "persistent", ["true", "false"]),
    ("kWaterlogged", "waterlogged", ["true", "false"]),
    ("kSnowy", "snowy", ["true", "false"]),
    ("kHalf", "half", ["upper", "lower"]),
    ("kHorizontalFacing", "facing", ["north", "south", "west", "east"]),
    ("kAge1", "age", ["0", "1"]),
    ("kAge2", "age", ["0", "1", "2"]),
    ("kUp", "up", ["true", "false"]),
    ("kDown", "down", ["true", "false"]),
    ("kNorth", "north", ["true", "false"]),
    ("kEast", "east", ["true", "false"]),
    ("kSouth", "south", ["true", "false"]),
    ("kWest", "west", ["true", "false"]),
    ("kTip", "tip", ["true", "false"]),
    ("kHanging", "hanging", ["true", "false"]),
    ("kFacing", "facing", ["north", "east", "south", "west", "up", "down"]),
    ("kThickness", "thickness", ["tip_merge", "tip", "frustum", "middle", "base"]),
    ("kVerticalDirection", "vertical_direction", ["up", "down"]),
    ("kBottom", "bottom", ["true", "false"]),
    ("kWallNorth", "north", ["none", "low", "tall"]),
    ("kWallEast", "east", ["none", "low", "tall"]),
    ("kWallSouth", "south", ["none", "low", "tall"]),
    ("kWallWest", "west", ["none", "low", "tall"]),
    ("kCanSummon", "can_summon", ["true", "false"]),
    ("kPotentSulfurState", "potent_sulfur_state", ["dry", "wet", "dormant", "erupting", "continuous"]),
]

TRUNKS = {
    "straight_trunk_placer": "kStraight", "forking_trunk_placer": "kForking", "giant_trunk_placer": "kGiant",
    "mega_jungle_trunk_placer": "kMegaJungle", "dark_oak_trunk_placer": "kDarkOak", "fancy_trunk_placer": "kFancy",
    "bending_trunk_placer": "kBending", "upwards_branching_trunk_placer": "kUpwardsBranching", "cherry_trunk_placer": "kCherry",
    "poplar_trunk_placer": "kPoplar",
}
FOLIAGES = {
    "blob_foliage_placer": "kBlob", "spruce_foliage_placer": "kSpruce", "pine_foliage_placer": "kPine", "acacia_foliage_placer": "kAcacia",
    "bush_foliage_placer": "kBush", "fancy_foliage_placer": "kFancy", "jungle_foliage_placer": "kMegaJungle",
    "mega_pine_foliage_placer": "kMegaPine", "dark_oak_foliage_placer": "kDarkOak", "random_spread_foliage_placer": "kRandomSpread",
    "cherry_foliage_placer": "kCherry", "poplar_foliage_placer": "kPoplar",
}
DECORATORS = {
    "beehive": "kBeehive", "cocoa": "kCocoa", "leave_vine": "kLeaveVine", "trunk_vine": "kTrunkVine", "alter_ground": "kAlterGround",
    "attached_to_leaves": "kAttachedToLeaves", "attached_to_logs": "kAttachedToLogs", "place_on_ground": "kPlaceOnGround",
    "shelf_mushroom": "kShelfMushroom", "pale_moss": "kPaleMoss", "creaking_heart": "kCreakingHeart",
}
DIRECTION_NAMES = ["down", "up", "north", "south", "west", "east"]

# BlockBehaviour.updateShape, by the class that has the method (gamedata's hooks): the rule of
# src/worldgen/shape.hpp. A class that is not here is "not ported": what would call its method counts it.
SHAPES = {
    None: "kNone",
    "LeavesBlock": "kLeaves",
    "SnowyBlock": "kSnowy",
    "VegetationBlock": "kSurviveOrAir",
    "SnowLayerBlock": "kSurviveOrAir",
    "CarpetBlock": "kSurviveOrAir",
    "DoublePlantBlock": "kDoublePlant",
    "SeagrassBlock": "kSeagrass",
    "LiquidBlock": "kLiquid",
    "FallingBlock": "kFalling",
    "HugeMushroomBlock": "kHugeMushroom",
    "VineBlock": "kVine",
    "CocoaBlock": "kCocoa",
    "ShelfMushroomBlock": "kShelfMushroom",
    "MangrovePropaguleBlock": "kMangrovePropagule",
    "MangroveRootsBlock": "kWaterTick",
    "HangingMossBlock": "kHangingMoss",
    "SugarCaneBlock": "kSurviveOrTick",
    "CactusBlock": "kSurviveOrTick",
    "BambooStalkBlock": "kBamboo",
    "BambooSaplingBlock": "kBambooSapling",
    "BeehiveBlock": "kNone",          # (it lets its bees out beside a fire: nothing of the block changes)
    "CreakingHeartBlock": "kTick",
    "MossyCarpetBlock": "kMossyCarpet",
    # What stands in a structure template, and at its edge (template.cpp).
    "SlabBlock": "kWaterTick",
    "BrushableBlock": "kTick",
    "PotentSulfurBlock": "kPotentSulfur",
    "SpeleothemBlock": "kSpeleothem",
}
# Which of them ask BlockState.canSurvive.
SHAPES_THAT_SURVIVE = {"kSurviveOrAir", "kDoublePlant", "kSeagrass", "kCocoa", "kShelfMushroom", "kMangrovePropagule", "kSurviveOrTick",
                       "kBamboo", "kBambooSapling", "kMossyCarpet"}


class TreeTables:
    """What FeatureTables knows of trees. (It has the pools, the providers and the sets.)"""

    def init_trees(self) -> None:
        from gen_features import Pool
        self.trees, self.trunks, self.foliages, self.sizes, self.roots = Pool(), Pool(), Pool(), Pool(), Pool()
        self.tree_decorators, self.fallen_trees = Pool(), Pool()

    def _directions(self, value) -> int:
        """A list of directions: how many << 24, then each in three bits, the first lowest."""
        if not isinstance(value, list) or not value or len(value) > 6 or any(v not in DIRECTION_NAMES for v in value):
            die(f"gen_features: a tree decorator's directions written as {value!r}")
        return len(value) << 24 | sum(DIRECTION_NAMES.index(v) << 3 * i for i, v in enumerate(value))

    def _uniform(self, value) -> tuple[int, int]:
        """UniformInt.MAP_CODEC: the two bounds, with no type beside them."""
        if not isinstance(value, dict) or set(value) != {"min_inclusive", "max_inclusive"}:
            die(f"gen_features: a UniformInt written as {value!r}")
        return int(value["min_inclusive"]), int(value["max_inclusive"])

    def trunk(self, value) -> int:
        kind = self.kind("a trunk placer", value)
        if kind not in TRUNKS:
            die(f"gen_features: a trunk placer of the type {kind}: src/worldgen/tree.cpp has none")
        base = {"base_height", "height_rand_a", "height_rand_b"}
        a = b = c = d = e = in_set = 0
        f = 0.0
        if kind == "bending_trunk_placer":
            self.need(kind, value, base | {"bend_length"}, {"min_height_for_leaves"})
            a, b = int(value.get("min_height_for_leaves", 1)), self.int_provider(value["bend_length"])
        elif kind == "upwards_branching_trunk_placer":
            self.need(kind, value, base | {"extra_branch_steps", "extra_branch_length", "place_branch_per_log_probability", "can_grow_through"})
            a, b = self.int_provider(value["extra_branch_steps"]), self.int_provider(value["extra_branch_length"])
            in_set = self.sets.keep(self.sets.blocks_in(value["can_grow_through"]), {"type": "minecraft:matching_blocks", "blocks": value["can_grow_through"]})
            f = float(value["place_branch_per_log_probability"])
        elif kind == "cherry_trunk_placer":
            self.need(kind, value, base | {"branch_count", "branch_horizontal_length", "branch_start_offset_from_top", "branch_end_offset_from_top"})
            low, high = self._uniform(value["branch_start_offset_from_top"])
            if high - low < 1:
                die("gen_features: a cherry's branches start in a range of one (CherryTrunkPlacer.BRANCH_START_CODEC refuses it)")
            uniform = lambda lo, hi: self.int_provider({"type": "minecraft:uniform", "min_inclusive": lo, "max_inclusive": hi})
            a, b = self.int_provider(value["branch_count"]), self.int_provider(value["branch_horizontal_length"])
            # (branchStartOffsetFromTop, and the second branch's: the same range one shorter at its top.)
            c, d, e = uniform(low, high), uniform(low, high - 1), self.int_provider(value["branch_end_offset_from_top"])
        elif kind == "poplar_trunk_placer":
            self.need(kind, value, base | {"trunk_height_above_branches", "branch_amount"})
            a, b = self.int_provider(value["trunk_height_above_branches"]), self.int_provider(value["branch_amount"])
        else:
            self.need(kind, value, base)
        return self.trunks.run([(TRUNKS[kind], int(value["base_height"]), int(value["height_rand_a"]), int(value["height_rand_b"]),
                                 a, b, c, d, e, in_set, f)])

    def foliage(self, value) -> int:
        kind = self.kind("a foliage placer", value)
        if kind not in FOLIAGES:
            die(f"gen_features: a foliage placer of the type {kind}: src/worldgen/tree.cpp has none")
        base = {"radius", "offset"}
        a = b = 0
        f = [0.0, 0.0, 0.0, 0.0]
        if kind in ("blob_foliage_placer", "bush_foliage_placer", "fancy_foliage_placer", "jungle_foliage_placer"):
            self.need(kind, value, base | {"height"})
            a = int(value["height"])
        elif kind in ("pine_foliage_placer", "spruce_foliage_placer", "mega_pine_foliage_placer"):
            field = {"pine_foliage_placer": "height", "spruce_foliage_placer": "trunk_height", "mega_pine_foliage_placer": "crown_height"}[kind]
            self.need(kind, value, base | {field})
            a = self.int_provider(value[field])
        elif kind == "random_spread_foliage_placer":
            self.need(kind, value, base | {"foliage_height", "leaf_placement_attempts"})
            a, b = self.int_provider(value["foliage_height"]), int(value["leaf_placement_attempts"])
        elif kind == "cherry_foliage_placer":
            chances = ["wide_bottom_layer_hole_chance", "corner_hole_chance", "hanging_leaves_chance", "hanging_leaves_extension_chance"]
            self.need(kind, value, base | {"height", *chances})
            a, f = self.int_provider(value["height"]), [float(value[c]) for c in chances]
        elif kind == "poplar_foliage_placer":
            self.need(kind, value, base | {"height", "side_hole_chance"})
            a, f[0] = self.int_provider(value["height"]), float(value["side_hole_chance"])
        else:
            self.need(kind, value, base)
        return self.foliages.run([(FOLIAGES[kind], self.int_provider(value["radius"]), self.int_provider(value["offset"]), a, b, *f)])

    def feature_size(self, value) -> int:
        kind = self.kind("a tree's size", value)
        if kind == "two_layers_feature_size":
            self.need(kind, value, set(), {"limit", "lower_size", "upper_size", "min_clipped_height"})
            row = (0, int(value.get("limit", 1)), 0, int(value.get("lower_size", 0)), 0, int(value.get("upper_size", 1)))
        elif kind == "three_layers_feature_size":
            self.need(kind, value, set(), {"limit", "upper_limit", "lower_size", "middle_size", "upper_size", "min_clipped_height"})
            row = (1, int(value.get("limit", 1)), int(value.get("upper_limit", 1)), int(value.get("lower_size", 0)), int(value.get("middle_size", 1)),
                   int(value.get("upper_size", 1)))
        else:
            die(f"gen_features: a tree's size of the type {kind}")
        return self.sizes.run([(*row, int(value.get("min_clipped_height", -1)))])

    def root_placer(self, value) -> tuple[int, bool]:
        kind = self.kind("a root placer", value)
        if kind != "mangrove_root_placer":
            die(f"gen_features: a root placer of the type {kind}")
        self.need(kind, value, {"trunk_offset_y", "root_provider", "mangrove_root_placement"}, {"above_root_placement"})
        roots, has = self.provider(value["root_provider"])
        above = value.get("above_root_placement")
        above_provider, above_chance = 0, 0.0
        if above is not None:
            if set(above) != {"above_root_provider", "above_root_placement_chance"}:
                die(f"gen_features: what is placed above roots written as {above!r}")
            above_provider, has_above = self.provider(above["above_root_provider"])
            has = has and has_above
            above_chance = float(above["above_root_placement_chance"])
        m = value["mangrove_root_placement"]
        if set(m) != {"can_grow_through", "muddy_roots_in", "muddy_roots_provider", "max_root_width", "max_root_length", "random_skew_chance"}:
            die(f"gen_features: a mangrove's roots written as {m!r}")
        muddy, has_muddy = self.provider(m["muddy_roots_provider"])
        through = self.sets.keep(self.sets.blocks_in(m["can_grow_through"]), {"type": "minecraft:matching_blocks", "blocks": m["can_grow_through"]})
        muddy_in = self.sets.keep(self.sets.blocks_in(m["muddy_roots_in"]), {"type": "minecraft:matching_blocks", "blocks": m["muddy_roots_in"]})
        row = (self.int_provider(value["trunk_offset_y"]), roots, int(above is not None), above_provider, above_chance, through, muddy_in, muddy,
               int(m["max_root_width"]), int(m["max_root_length"]), float(m["random_skew_chance"]))
        return self.roots.run([row]), has and has_muddy

    def tree_decorator(self, value) -> tuple[tuple, bool]:
        kind = self.kind("a tree decorator", value)
        if kind not in DECORATORS:
            die(f"gen_features: a tree decorator of the type {kind}: src/worldgen/tree.cpp has none")
        f = [0.0, 0.0, 0.0]
        provider = a = b = c = d = 0
        has = True
        state = self.blocks.state
        if kind in ("beehive", "cocoa", "leave_vine", "shelf_mushroom", "creaking_heart"):
            self.need(kind, value, {"probability"})
            f[0] = float(value["probability"])
            # The block the decorator's code names, as it places it (cocoa and the shelf with an age and a facing of their own).
            a = {"beehive": lambda: state({"id": "minecraft:bee_nest", "properties": {"facing": "south"}}),
                 "cocoa": lambda: state("minecraft:cocoa"),
                 "leave_vine": lambda: state("minecraft:vine"),
                 "shelf_mushroom": lambda: state("minecraft:shelf_mushroom"),
                 "creaking_heart": lambda: state({"id": "minecraft:creaking_heart", "properties": {"creaking_heart_state": "dormant", "natural": "true"}}),
                 }[kind]()
        elif kind == "trunk_vine":
            self.need(kind, value, set())
            a = state("minecraft:vine")
        elif kind == "alter_ground":
            self.need(kind, value, {"provider"})
            provider, has = self.provider(value["provider"])
        elif kind == "attached_to_leaves":
            self.need(kind, value, {"probability", "exclusion_radius_xz", "exclusion_radius_y", "block_provider", "required_empty_blocks", "directions"})
            f[0] = float(value["probability"])
            provider, has = self.provider(value["block_provider"])
            a, b, c, d = int(value["exclusion_radius_xz"]), int(value["exclusion_radius_y"]), int(value["required_empty_blocks"]), self._directions(value["directions"])
        elif kind == "attached_to_logs":
            self.need(kind, value, {"probability", "block_provider", "directions"})
            f[0] = float(value["probability"])
            provider, has = self.provider(value["block_provider"])
            d = self._directions(value["directions"])
        elif kind == "place_on_ground":
            self.need(kind, value, {"block_state_provider"}, {"tries", "radius", "height"})
            provider, has = self.provider(value["block_state_provider"])
            a, b, c = int(value.get("tries", 128)), int(value.get("radius", 2)), int(value.get("height", 1))
        elif kind == "pale_moss":
            self.need(kind, value, {"leaves_probability", "trunk_probability", "ground_probability"})
            f = [float(value["leaves_probability"]), float(value["trunk_probability"]), float(value["ground_probability"])]
            # PaleMossDecorator: the feature it looks up by name and places by itself.
            a = self.feature_ref("minecraft:pale_moss_patch")
            has = self.feature_ported[a]
            b = state("minecraft:pale_hanging_moss")
        return (DECORATORS[kind], *f, provider, a, b, c, d), has

    def _decorators(self, values) -> tuple[int, int, bool]:
        if not isinstance(values, list):
            die(f"gen_features: a tree's decorators written as {values!r}")
        made = [self.tree_decorator(v) for v in values]
        return (self.tree_decorators.run([row for row, _ in made]) if made else 0), len(made), all(has for _, has in made)

    def tree(self, value) -> tuple[tuple, bool]:
        self.need("tree", value, {"trunk_provider", "trunk_placer", "foliage_provider", "foliage_placer", "minimum_size", "decorators", "ignore_vines",
                                  "below_trunk_provider"}, {"root_placer"})
        trunk_provider, has_trunk = self.provider(value["trunk_provider"])
        foliage_provider, has_foliage = self.provider(value["foliage_provider"])
        below, has_below = self.provider(value["below_trunk_provider"])
        roots, has_roots = self.root_placer(value["root_placer"]) if "root_placer" in value else (-1, True)
        first, count, has_decorators = self._decorators(value["decorators"])
        row = (trunk_provider, foliage_provider, below, self.trunk(value["trunk_placer"]), self.foliage(value["foliage_placer"]),
               self.feature_size(value["minimum_size"]), roots + 1, first, count, int(bool(value["ignore_vines"])))
        return ("kTree", self.trees.run([row]), 0, 0), has_trunk and has_foliage and has_below and has_roots and has_decorators

    def fallen_tree(self, value) -> tuple[tuple, bool]:
        self.need("fallen_tree", value, {"trunk_provider", "log_length", "stump_decorators", "log_decorators"})
        provider, has = self.provider(value["trunk_provider"])
        stump, stumps, has_stump = self._decorators(value["stump_decorators"])
        log, logs, has_log = self._decorators(value["log_decorators"])
        row = (provider, self.int_provider(value["log_length"]), stump, stumps, log, logs)
        return ("kFallenTree", self.fallen_trees.run([row]), 0, 0), has and has_stump and has_log

    def blocks_by_id(self) -> list:
        """(name, the block of blocks.json) in the order of the registry's ids - what world::block_of gives."""
        import gamedata
        listed = sorted(gamedata.load(self.version, "blocks")["blocks"], key=lambda b: b["id"])
        if [b["id"] for b in listed] != list(range(len(listed))) or len(listed) != len(self.blocks.blocks):
            die("gen_features: gamedata/blocks.json does not list the blocks of the registry one an id")
        for b in listed:
            if min(s["id"] for s in self.blocks.blocks[b["name"]]["states"]) != b["first_state"]:
                die(f"gen_features: {b['name']} begins at another state in gamedata than in the reports")
        return [(b["name"], self.blocks.blocks[b["name"]]) for b in listed]

    def make_shapes(self) -> None:
        """BlockState.updateShape per block, and canSurvive of the states whose rule asks it. (Before the tables
        are written: what a rule asks about is a row of kSurvive.)"""
        import gamedata
        blocks = self.blocks_by_id()
        owners = {b["name"]: b.get("hooks", {}).get("updateShape") for b in gamedata.load(self.version, "blocks")["blocks"]}
        shape_of, unported = [], {}
        for name, block in blocks:
            owner = owners[name]
            if owner in SHAPES:
                shape_of.append(SHAPES[owner])
            else:
                shape_of.append("kUnported")
                unported[owner] = unported.get(owner, 0) + 1
        # canSurvive of the states whose updateShape asks it: per block where its states' rows begin (0xFFFF: its
        # rule asks nothing), and a row of kSurvive a state - 0xFFFF where that rule is not ported.
        known = self.survive_data["states"]
        at, rows, missing = [], [], []
        for (name, block), shape in zip(blocks, shape_of):
            if shape not in SHAPES_THAT_SURVIVE:
                at.append(0xFFFF)
                continue
            at.append(len(rows))
            for s in sorted(block["states"], key=lambda s: s["id"]):
                row = 0xFFFF
                if str(s["id"]) in known:
                    rule, ported = self.survive_rule(s["id"])
                    if ported:
                        row = rule
                elif name not in missing:
                    missing.append(name)
                rows.append(row)
        if len(rows) >= 0xFFFF:
            die("gen_features: more states ask whether they can stay than the table's 16 bits count")
        self.shapes = (shape_of, at, rows)
        self.shape_report = (unported, missing)
        # For the golden script (WorldgenShape.golden) and the test of the rules: a few states of every block that
        # has a rule - the first, the last and two between - whose rule is all here.
        tested = []
        for (name, block), shape, begin in zip(blocks, shape_of, at):
            if shape in ("kNone", "kUnported"):
                continue
            ids = sorted(s["id"] for s in block["states"])
            if begin != 0xFFFF and any(row == 0xFFFF for row in rows[begin:begin + len(ids)]):
                continue
            tested += sorted({ids[0], ids[len(ids) // 3], ids[2 * len(ids) // 3], ids[-1]})
        from mcgen import vdir
        (vdir(self.version) / "worldgen" / "shape_states.txt").write_text("".join(f"{state}\n" for state in tested))


def cfloat(v: float) -> str:
    import struct
    return struct.unpack("<f", struct.pack("<f", v))[0].hex() + "f"


def emit_trees(t, out: list[str]) -> None:
    """The structs and tables of trees, and the two tables of block states' properties and shapes."""
    out.append("""
// ---------------------------------------------------------------------------------------------
// Trees (tools/mcgen/gen_trees.py): TreeFeature and what it is made of, FallenTreeFeature.

enum class TrunkType : u8 { """ + ", ".join(dict.fromkeys(TRUNKS.values())) + """ };
// TrunkPlacer: the height is base + nextInt(rand_a + 1) + nextInt(rand_b + 1); the rest is its type's.
//   kBending            a min_height_for_leaves, b bend_length (kInts)
//   kUpwardsBranching   a extra_branch_steps, b extra_branch_length (kInts), set can_grow_through, f the chance of a branch a log
//   kCherry             a branch_count, b branch_horizontal_length, c the first branch's start from the top, d the second's,
//                       e branch_end_offset_from_top (all kInts)
//   kPoplar             a trunk_height_above_branches, b branch_amount (kInts)
struct Trunk {
    TrunkType type;
    i32 base_height, height_rand_a, height_rand_b;
    i32 a, b, c, d, e;
    u16 set;
    float f;
};
enum class FoliageType : u8 { """ + ", ".join(dict.fromkeys(FOLIAGES.values())) + """ };
// FoliagePlacer: radius and offset are kInts; the rest is its type's.
//   kBlob, kBush, kFancy, kMegaJungle   a height
//   kPine a height, kSpruce a trunk_height, kMegaPine a crown_height (kInts)
//   kRandomSpread   a foliage_height (kInts), b leaf_placement_attempts
//   kCherry         a height (kInts), f: the chances of a hole in the wide bottom layer, of a hole at a corner, of
//                   hanging leaves, of their extension
//   kPoplar         a height (kInts), f[0] the chance of a hole in a side
struct Foliage {
    FoliageType type;
    u16 radius, offset;
    i32 a, b;
    float f[4];
};
// FeatureSize.getSizeAtHeight, of two layers or of three; min_clipped_height -1: none.
struct TreeSize {
    bool three;
    i32 limit, upper_limit, lower_size, middle_size, upper_size;
    i32 min_clipped_height;
};
// MangroveRootPlacer (the one root placer there is).
struct Roots {
    u16 trunk_offset_y;  // kInts
    u16 root_provider;
    bool has_above;
    u16 above_provider;
    float above_chance;
    u16 can_grow_through, muddy_roots_in;  // state sets
    u16 muddy_roots_provider;
    i32 max_root_width, max_root_length;
    float random_skew_chance;
};
enum class TreeDecoratorType : u8 { """ + ", ".join(DECORATORS.values()) + """ };
// TreeDecorator: f[0] is its probability where it has one.
//   kAlterGround        provider
//   kAttachedToLeaves   provider, a exclusion_radius_xz, b exclusion_radius_y, c required_empty_blocks, d its directions
//   kAttachedToLogs     provider, d its directions
//   kPlaceOnGround      provider, a tries, b radius, c height
//   kPaleMoss           f: the chances of moss on a leaf, on a log, on the ground; a the feature of the patch (kFeatures),
//                       b the moss that hangs
//   kBeehive, kCocoa, kLeaveVine, kTrunkVine, kShelfMushroom, kCreakingHeart   a the block its code names, as it begins
// Directions: how many << 24, then each in three bits, the first lowest.
struct TreeDecorator {
    TreeDecoratorType type;
    float f[3];
    u16 provider;
    i32 a, b, c;
    u32 d;
};
struct Tree {
    u16 trunk_provider, foliage_provider, below_trunk_provider;
    u16 trunk, foliage, size;  // kTrunks, kFoliages, kTreeSizes
    u16 roots;                 // kRoots + 1, 0 for none
    u16 decorators, decorator_count;
    bool ignore_vines;
};
struct FallenTree {
    u16 trunk_provider;
    u16 log_length;  // kInts
    u16 stump_decorators, stump_decorator_count;
    u16 log_decorators, log_decorator_count;
};
""")
    rows = lambda pool: pool.rows
    out.append(f"inline constexpr Trunk kTrunks[{max(1, len(rows(t.trunks)))}] = {{\n")
    out += [f"    {{TrunkType::{r[0]}, {r[1]}, {r[2]}, {r[3]}, {r[4]}, {r[5]}, {r[6]}, {r[7]}, {r[8]}, {r[9]}, {cfloat(r[10])}}},\n" for r in rows(t.trunks)]
    out.append("};\n")
    out.append(f"inline constexpr Foliage kFoliages[{max(1, len(rows(t.foliages)))}] = {{\n")
    out += [f"    {{FoliageType::{r[0]}, {r[1]}, {r[2]}, {r[3]}, {r[4]}, {{{', '.join(cfloat(v) for v in r[5:9])}}}}},\n" for r in rows(t.foliages)]
    out.append("};\n")
    out.append(f"inline constexpr TreeSize kTreeSizes[{max(1, len(rows(t.sizes)))}] = {{"
               + ", ".join(f"{{{'true' if r[0] else 'false'}, {r[1]}, {r[2]}, {r[3]}, {r[4]}, {r[5]}, {r[6]}}}" for r in rows(t.sizes)) + "};\n")
    out.append(f"inline constexpr Roots kRoots[{max(1, len(rows(t.roots)))}] = {{"
               + ", ".join(f"{{{r[0]}, {r[1]}, {'true' if r[2] else 'false'}, {r[3]}, {cfloat(r[4])}, {r[5]}, {r[6]}, {r[7]}, {r[8]}, {r[9]}, {cfloat(r[10])}}}"
                           for r in rows(t.roots)) + "};\n")
    out.append(f"inline constexpr TreeDecorator kTreeDecorators[{max(1, len(rows(t.tree_decorators)))}] = {{\n")
    out += [f"    {{TreeDecoratorType::{r[0]}, {{{cfloat(r[1])}, {cfloat(r[2])}, {cfloat(r[3])}}}, {r[4]}, {r[5]}, {r[6]}, {r[7]}, {r[8]}u}},\n"
            for r in rows(t.tree_decorators)]
    out.append("};\n")
    out.append(f"inline constexpr Tree kTrees[{max(1, len(rows(t.trees)))}] = {{\n")
    out += ["    {" + ", ".join(str(v) for v in r[:9]) + f", {'true' if r[9] else 'false'}}},\n" for r in rows(t.trees)]
    out.append("};\n")
    out.append(f"inline constexpr FallenTree kFallenTrees[{max(1, len(rows(t.fallen_trees)))}] = {{"
               + ", ".join("{" + ", ".join(map(str, r)) + "}" for r in rows(t.fallen_trees)) + "};\n")

    # ---- properties
    blocks = t.blocks_by_id()
    out.append("""
// ---------------------------------------------------------------------------------------------
// The properties of block states that src/worldgen names (worldgen/props.hpp): kPropStride[property][block]
// is by how much a state's id moves from one value of the property to the next - the values in the
// order below - and 0 where the block has no such property. (A property is its name and its values:
// the age of cocoa and the age of bamboo are two.)
""")
    out.append("enum class Prop : u8 {\n" + "".join(f"    {name},  // {prop}: {' '.join(values)}\n" for name, prop, values in PROPS) + "};\n")
    out.append(f"inline constexpr u32 kPropCount = {len(PROPS)}, kPropBlocks = {len(blocks)};\n")
    out.append("inline constexpr u8 kPropValues[kPropCount] = {" + ", ".join(str(len(values)) for _, _, values in PROPS) + "};\n")
    strides = []
    for _, prop, values in PROPS:
        row = []
        for name, block in blocks:
            states = block["states"]
            properties = list(states[0].get("properties", {}))
            stride = 0
            if prop in properties:
                # The values in the order of the ids, and how far apart two of them are.
                seen = list(dict.fromkeys(s["properties"][prop] for s in sorted(states, key=lambda s: s["id"])))
                if seen == values:
                    first = states[0]["properties"]
                    other = next(s for s in states if s["properties"][prop] == values[1]
                                 and all(s["properties"][p] == first[p] for p in properties if p != prop))
                    base = next(s for s in states if s["properties"][prop] == values[0]
                                and all(s["properties"][p] == first[p] for p in properties if p != prop))
                    stride = other["id"] - base["id"]
                    if stride <= 0:
                        die(f"gen_features: the ids of {name} do not grow with {prop}")
                    low = min(s["id"] for s in states)
                    for s in states:   # (the layout this table stands for: checked for every state)
                        if (s["id"] - low) // stride % len(values) != values.index(s["properties"][prop]):
                            die(f"gen_features: the states of {name} are not laid out by {prop} as a mixed radix number")
            row.append(stride)
        strides.append(row)
    out.append("inline constexpr u16 kPropStride[kPropCount][kPropBlocks] = {\n")
    out += ["    {" + ", ".join(map(str, row)) + "},\n" for row in strides]
    out.append("};\n")

    shape_of, at, rows = t.shapes
    out.append("""
// ---------------------------------------------------------------------------------------------
// BlockState.updateShape (worldgen/shape.hpp): per block, the rule of the class whose method it is -
// kUnported for a class that has none here. For the rules that ask BlockState.canSurvive:
// kShapeSurvive[kShapeSurviveAt[block] + (state - the block's first)] is the row of kSurvive that
// is the state's (0xFFFF: not ported, and then neither is the shape).
""")
    kinds = list(dict.fromkeys(SHAPES.values()))
    out.append("enum class ShapeOp : u8 { " + ", ".join(kinds + ["kUnported"]) + " };\n")
    out.append("inline constexpr ShapeOp kShapeOf[kPropBlocks] = {\n")
    for i in range(0, len(shape_of), 8):
        out.append("    " + " ".join(f"ShapeOp::{s}," for s in shape_of[i:i + 8]) + "\n")
    out.append("};\n")
    out.append("inline constexpr u16 kShapeSurviveAt[kPropBlocks] = {\n")
    for i in range(0, len(at), 32):
        out.append("    " + ",".join(map(str, at[i:i + 32])) + ",\n")
    out.append("};\n")
    out.append(f"inline constexpr u16 kShapeSurvive[{max(1, len(rows))}] = {{\n")
    for i in range(0, len(rows), 32):
        out.append("    " + ",".join(map(str, rows[i:i + 32])) + ",\n")
    out.append("};\n")
