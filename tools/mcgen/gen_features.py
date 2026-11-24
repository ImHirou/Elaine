# SPDX-License-Identifier: GPL-3.0-or-later
"""The tables of a chunk's features (src/worldgen/feature.cpp): what the biomes of the overworld
put into a chunk once its terrain is made - worldgen_features.hpp and worldgen_state_sets.bin.

Two sources, both vanilla's: the JSON under data/minecraft/worldgen/ in the jar - placed_feature/
(where and how often), feature/ (what) - and features.json of `mcgen.py worldgen`, which says what
no file says: the one order vanilla puts all biomes' features in (FeatureSorter). A feature's place
in its decoration step is what its random source is seeded with.

Everything is compiled where it is named, as vanilla's codecs resolve it: a placed feature names
its feature (by name, or written out in place), a feature of features names placed features. What
is written out twice the same is kept once; a placed feature that has a name is never merged with
another (its name is its place in the order).

A predicate that asks nothing but which block state is at a place - a tag, a list of blocks, a
fluid, "solid", "replaceable", a sturdy face - is a set of block states, a bit a state, made here:
the test is one bit. `not`, `any_of` and `all_of` over such sets at one place are sets too.

Not every feature is ported. A type src/worldgen has no code for is compiled as kUnported and
whatever is made of it is not `ported`: a decoration leaves it out (its own random source is
seeded from its index, so nothing else moves). The run stops at a type or a field that is *new* -
one neither ported nor known to be waiting: that is vanilla having changed.
"""
from __future__ import annotations

import json
import struct

from mcgen import die, vdir

import worldgen

HEIGHTMAPS = ["WORLD_SURFACE_WG", "WORLD_SURFACE", "OCEAN_FLOOR_WG", "OCEAN_FLOOR", "MOTION_BLOCKING", "MOTION_BLOCKING_NO_LEAVES"]
DIRECTIONS = ["down", "up", "north", "south", "west", "east"]
STEPS = ["raw_generation", "lakes", "local_modifications", "underground_structures", "surface_structures", "strongholds",
         "underground_ores", "underground_decoration", "fluid_springs", "vegetal_decoration", "top_layer_modification"]

# Feature types src/worldgen/feature.cpp places, with the name of their FeatureType.
PORTED_FEATURES = {
    "no_op": "kNoOp", "random_selector": "kRandomSelector", "simple_random_selector": "kSimpleRandomSelector",
    "weighted_random_selector": "kWeightedRandomSelector", "random_boolean_selector": "kRandomBooleanSelector",
    "sequence": "kSequence", "overlay": "kOverlay",
}
# ...and the types of the overworld that wait for theirs. A type in neither list stops the run.
UNPORTED_FEATURES = {
    "simple_block", "tree", "ore", "block_column", "template", "disk", "coral_tree", "coral_claw", "fallen_tree", "vegetation_patch",
    "speleothem", "spring_feature", "lake", "multiface_growth", "iceberg", "fossil", "bamboo", "speleothem_cluster", "root_system",
    "geode", "monster_room", "underwater_magma", "huge_brown_mushroom", "huge_red_mushroom", "freeze_top_layer", "blue_ice",
    "block_blob", "vines", "spike", "large_dripstone", "waterlogged_vegetation_patch", "sculk_patch",
}
# Predicates that ask a block what it would do, which is the block's own code: until that is here.
UNPORTED_PREDICATES = {"would_survive"}

INT_MIN, INT_MAX = -(1 << 31), (1 << 31) - 1


def strip(name: str) -> str:
    return name.removeprefix("minecraft:")


def canon(value) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


class StateSets:
    """Sets of block states as Python integers, bit i for state i; what is asked of them, and the
    ones the tables keep."""

    def __init__(self, version: str, vanilla, blocks):
        import gamedata
        self.blocks = blocks.blocks
        self.block_tags, self.fluid_tags = vanilla.tags("block"), vanilla.tags("fluid")
        data = json.loads((vdir(version) / "extract" / "blockdata.json").read_text())
        states = data["states"]
        self.count = data["state_count"]
        self.everything = (1 << self.count) - 1
        self.fluid_names = data["fluids"]
        self.fluid_of = states["fluid"]
        column = lambda c: sum(1 << i for i, v in enumerate(c) if v)
        self.facts = {"solid": column(states["solid"]), "replaceable": column(states["replaceable"])}
        sturdy = gamedata.load(version, "states")
        if [d.lower() for d in sturdy["legend"]["directions"]] != DIRECTIONS:
            die("gen_features: gamedata/states.json: the directions are not down, up, north, south, west, east")
        for d, name in enumerate(DIRECTIONS):
            self.facts[f"sturdy_{name}"] = column(v >> d & 1 for v in sturdy["columns"]["sturdy_full"])
        self.kept: list[int] = []
        self.sources: list[str] = []   # of each: the predicate it was first made of, as the jar writes it
        self.index: dict[int, int] = {}

    def keep(self, bits: int, source) -> int:
        if bits not in self.index:
            self.index[bits] = len(self.kept)
            self.kept.append(bits)
            self.sources.append(canon(source))
        return self.index[bits]

    def _tagged(self, tags: dict, tag: str, what: str, depth: int = 0) -> set[str]:
        if depth > 16 or tag not in tags:
            die(f"gen_features: the {what} tag {tag}: missing, or a cycle")
        found: set[str] = set()
        for value in tags[tag]:
            value = value["id"] if isinstance(value, dict) else value
            found |= self._tagged(tags, value[1:], what, depth + 1) if value.startswith("#") else {value}
        return found

    def _holders(self, value, tags: dict, what: str) -> set[str]:
        """A HolderSet as the codec reads it: a name, a "#tag", or a list of names."""
        if isinstance(value, str):
            return self._tagged(tags, value[1:], what) if value.startswith("#") else {value}
        if isinstance(value, list) and all(isinstance(v, str) and not v.startswith("#") for v in value):
            return set(value)
        die(f"gen_features: a set of {what}s written as {value!r}")

    def of_blocks(self, names: set[str]) -> int:
        bits = 0
        for name in names:
            if name not in self.blocks:
                die(f"gen_features: no block {name!r}")
            for s in self.blocks[name]["states"]:
                bits |= 1 << s["id"]
        return bits

    def blocks_in(self, value) -> int:
        return self.of_blocks(self._holders(value, self.block_tags, "block"))

    def block_tag(self, tag: str) -> int:
        return self.of_blocks(self._tagged(self.block_tags, tag, "block"))

    def fluids_in(self, value) -> int:
        names = self._holders(value, self.fluid_tags, "fluid")
        for name in names:
            if name not in self.fluid_names:
                die(f"gen_features: no fluid {name!r}")
        wanted = {self.fluid_names.index(name) for name in names}
        return sum(1 << i for i, fluid in enumerate(self.fluid_of) if fluid in wanted)

    def bytes(self) -> bytes:
        width = (self.count + 7) // 8
        return b"".join(bits.to_bytes(width, "little") for bits in self.kept)


class Pool:
    """Rows of one table: what is the same twice is one row."""

    def __init__(self):
        self.rows: list = []
        self.index: dict = {}

    def add(self, row, key=None) -> int:
        key = row if key is None else key
        if key not in self.index:
            self.index[key] = len(self.rows)
            self.rows.append(row)
        return self.index[key]

    def run(self, rows: list) -> int:
        """Rows that are read one after the other: the index of the first."""
        first = len(self.rows)
        self.rows += rows
        return first


class FeatureTables:
    def __init__(self, version: str, vanilla, blocks, settings: dict):
        self.vanilla, self.blocks = vanilla, blocks
        self.sets = StateSets(version, vanilla, blocks)
        self.min_y, self.height = settings["noise"]["min_y"], settings["noise"]["height"]
        self.biome_ids = {name: i for i, name in enumerate(vanilla.entries("worldgen/biome"))}
        self.order = worldgen.load(version, "features")
        self.ints, self.int_entries = Pool(), Pool()
        self.heights, self.height_entries = Pool(), Pool()
        self.predicates, self.predicate_lists = Pool(), Pool()
        self.doubles = Pool()
        self.modifiers = Pool()
        self.features, self.chances, self.placed_lists, self.weighted = Pool(), Pool(), Pool(), Pool()
        self.placed: list = []                 # (feature, first modifier, modifiers, ported, where); None while it is made
        self.placed_names: list[str] = []      # "" for one that is written out inside a feature
        self.named_placed: dict[str, int] = {}
        self.inline_placed: dict[str, int] = {}
        self.named_features: dict[str, int] = {}
        self.inline_features: dict[str, int] = {}
        self.feature_ported: list[bool] = []
        self.unported_types: dict[str, int] = {}
        self.waiting: dict[int, str] = {}      # rows that have their place and are still to be read
        self.nested: set[int] = set()          # placed features a feature is made of
        self.making: list[str] = []            # the named features that are being read: one of them again is a circle
        # The placed features the biomes name come first, in vanilla's order.
        for names in self.order["steps"]:
            for name in names:
                if name in self.named_placed:
                    die(f"gen_features: {name} is in two decoration steps")
                self.named_placed[name] = self._new_placed(name)
                self.waiting[self.named_placed[name]] = name
        self.named = len(self.placed)
        for at in range(self.named):
            self._fill_waiting(at)
        for at in self.nested:
            if self._asks_biome(self.placed[at][1], self.placed[at][2]):
                die(f"gen_features: a feature within a feature asks for its biome ({self.placed_names[at] or 'unnamed'}): vanilla throws there (BiomeFilter)")

    # ---------------------------------------------------------------- reading

    def load(self, kind: str, name: str):
        path = f"data/minecraft/worldgen/{kind}/{strip(name)}.json"
        if path not in self.vanilla.names:
            die(f"gen_features: {name} is no {kind}")
        return json.loads(self.vanilla.zip.read(path))

    def need(self, what: str, value: dict, fields: set, optional: set = frozenset()) -> None:
        keys = set(value) - {"type"}
        if not fields <= keys or not keys <= fields | optional:
            die(f"gen_features: {what} has the fields {sorted(keys)}: src/worldgen reads {sorted(fields)} and {sorted(optional)}")

    def kind(self, what: str, value: dict) -> str:
        if not isinstance(value, dict) or not isinstance(value.get("type"), str):
            die(f"gen_features: {what} is written as {value!r}")
        return strip(value["type"])

    def offset(self, value: dict) -> tuple[int, int, int]:
        off = value.get("offset", [0, 0, 0])
        if not (isinstance(off, list) and len(off) == 3 and all(isinstance(v, int) and -16 <= v <= 16 for v in off)):
            die(f"gen_features: an offset of {off!r}")
        return tuple(off)

    # ---------------------------------------------------------------- numbers

    def int_provider(self, value) -> int:
        """IntProvider: (op, a, b, c, fa, fb)."""
        if isinstance(value, int) and not isinstance(value, bool):
            return self.ints.add(("kConstant", value, 0, 0, 0.0, 0.0))
        kind = self.kind("a number", value)
        if kind == "constant":
            self.need("constant", value, {"value"})
            return self.ints.add(("kConstant", value["value"], 0, 0, 0.0, 0.0))
        if kind in ("uniform", "biased_to_bottom", "very_biased_to_bottom"):
            self.need(kind, value, {"min_inclusive", "max_inclusive"})
            if value["min_inclusive"] > value["max_inclusive"]:
                die(f"gen_features: {kind} from {value['min_inclusive']} to {value['max_inclusive']}")
            op = {"uniform": "kUniform", "biased_to_bottom": "kBiasedToBottom", "very_biased_to_bottom": "kVeryBiasedToBottom"}[kind]
            return self.ints.add((op, value["min_inclusive"], value["max_inclusive"], 0, 0.0, 0.0))
        if kind == "trapezoid":
            self.need(kind, value, {"min", "max"}, {"plateau"})
            return self.ints.add(("kTrapezoid", value["min"], value["max"], value.get("plateau", 0), 0.0, 0.0))
        if kind == "clamped":
            self.need(kind, value, {"source", "min_inclusive", "max_inclusive"})
            return self.ints.add(("kClamped", value["min_inclusive"], value["max_inclusive"], self.int_provider(value["source"]), 0.0, 0.0))
        if kind == "clamped_normal":
            self.need(kind, value, {"mean", "deviation", "min_inclusive", "max_inclusive"})
            return self.ints.add(("kClampedNormal", value["min_inclusive"], value["max_inclusive"], 0, float(value["mean"]), float(value["deviation"])))
        if kind == "weighted_list":
            self.need(kind, value, {"distribution"})
            entries = [(self.int_provider(e["data"]), e["weight"]) for e in value["distribution"]]
            if not entries or any(w < 0 for _, w in entries):
                die(f"gen_features: a weighted list of {value['distribution']!r}")
            return self.ints.add(("kWeightedList", self.int_entries.run(entries), len(entries), sum(w for _, w in entries), 0.0, 0.0),
                                 key=("kWeightedList", tuple(entries)))
        die(f"gen_features: a number of the type {kind}: src/worldgen has no word for it")

    def anchor(self, value) -> int:
        """VerticalAnchor.resolveY in a world of the noise settings' height."""
        if isinstance(value, dict) and len(value) == 1:
            (how, n), = value.items()
            if how == "absolute":
                return n
            if how == "above_bottom":
                return self.min_y + n
            if how == "below_top":
                return self.height - 1 + self.min_y - n
        die(f"gen_features: a vertical anchor of {value!r}")

    def height_provider(self, value) -> int:
        """HeightProvider: (op, min, max, inner)."""
        if isinstance(value, dict) and "type" not in value:
            return self.heights.add(("kConstant", self.anchor(value), 0, 0))
        kind = self.kind("a height", value)
        if kind == "constant":
            self.need(kind, value, {"value"})
            return self.heights.add(("kConstant", self.anchor(value["value"]), 0, 0))
        if kind == "weighted_list":
            self.need(kind, value, {"distribution"})
            entries = [(self.height_provider(e["data"]), e["weight"]) for e in value["distribution"]]
            return self.heights.add(("kWeightedList", self.height_entries.run(entries), len(entries), sum(w for _, w in entries)),
                                    key=("kWeightedList", tuple(entries)))
        ops = {"uniform": ("kUniform", None), "trapezoid": ("kTrapezoid", "plateau"), "biased_to_bottom": ("kBiasedToBottom", "inner"),
               "very_biased_to_bottom": ("kVeryBiasedToBottom", "inner")}
        if kind not in ops:
            die(f"gen_features: a height of the type {kind}: src/worldgen has no word for it")
        op, extra = ops[kind]
        self.need(kind, value, {"min_inclusive", "max_inclusive"}, {extra} if extra else set())
        default = {"plateau": 0, "inner": 1}.get(extra, 0)
        return self.heights.add((op, self.anchor(value["min_inclusive"]), self.anchor(value["max_inclusive"]), value.get(extra, default) if extra else 0))

    # ---------------------------------------------------------------- predicates

    def _predicate(self, value):
        """A BlockPredicate: ("in", offset, set of states), or ("row", the row, ported)."""
        kind = self.kind("a predicate", value)
        if kind == "true":
            self.need(kind, value, set())
            return ("in", (0, 0, 0), self.sets.everything, value)
        if kind == "matching_blocks":
            self.need(kind, value, {"blocks"}, {"offset"})
            return ("in", self.offset(value), self.sets.blocks_in(value["blocks"]), value)
        if kind == "matching_block_tag":
            self.need(kind, value, {"tag"}, {"offset"})
            return ("in", self.offset(value), self.sets.block_tag(value["tag"]), value)
        if kind == "matching_fluids":
            self.need(kind, value, {"fluids"}, {"offset"})
            return ("in", self.offset(value), self.sets.fluids_in(value["fluids"]), value)
        if kind in ("solid", "replaceable"):
            self.need(kind, value, set(), {"offset"})
            return ("in", self.offset(value), self.sets.facts[kind], value)
        if kind == "has_sturdy_face":
            self.need(kind, value, {"direction"}, {"offset"})
            if value["direction"] not in DIRECTIONS:
                die(f"gen_features: a face to the {value['direction']}")
            return ("in", self.offset(value), self.sets.facts[f"sturdy_{value['direction']}"], value)
        if kind == "not":
            self.need(kind, value, {"predicate"})
            inner = self._predicate(value["predicate"])
            if inner[0] == "in":
                return ("in", inner[1], self.sets.everything & ~inner[2], value)
            return ("row", ("kNot", (0, 0, 0), self._row(inner), 0, (0, 0, 0)), self._ported(inner))
        if kind in ("all_of", "any_of"):
            self.need(kind, value, {"predicates"})
            inner = [self._predicate(p) for p in value["predicates"]]
            if inner and all(p[0] == "in" and p[1] == inner[0][1] for p in inner):
                bits = inner[0][2]
                for p in inner[1:]:
                    bits = bits & p[2] if kind == "all_of" else bits | p[2]
                return ("in", inner[0][1], bits, value)
            rows = [self._row(p) for p in inner]
            first = self.predicate_lists.run(rows)
            return ("row", ("kAllOf" if kind == "all_of" else "kAnyOf", (0, 0, 0), first, len(rows), (0, 0, 0)), all(self._ported(p) for p in inner))
        if kind == "inside_world_bounds":
            self.need(kind, value, set(), {"offset"})
            return ("row", ("kInsideWorld", self.offset(value), 0, 0, (0, 0, 0)), True)
        if kind == "volume_match":
            self.need(kind, value, {"min", "max", "match"})
            inner = self._predicate(value["match"])
            return ("row", ("kVolume", self.offset({"offset": value["min"]}), self._row(inner), 0, self.offset({"offset": value["max"]})), self._ported(inner))
        if kind == "would_survive":
            self.need(kind, value, {"state"}, {"offset"})
            return ("row", ("kWouldSurvive", self.offset(value), self.blocks.state(value["state"]), 0, (0, 0, 0)), False)
        die(f"gen_features: a predicate of the type {kind}: src/worldgen has no word for it")

    def _ported(self, made) -> bool:
        return made[0] == "in" or made[2]

    def _row(self, made) -> int:
        if made[0] == "in":
            return self.predicates.add(("kStateIn", made[1], self.sets.keep(made[2], made[3]), 0, (0, 0, 0)))
        return self.predicates.add(made[1])

    def predicate(self, value) -> tuple[int, bool]:
        made = self._predicate(value)
        return self._row(made), self._ported(made)

    # ---------------------------------------------------------------- where

    def modifier(self, value) -> tuple[tuple, bool]:
        """A PlacementModifier: (op, flags, a, b, c, f, d), and whether everything in it is ported."""
        kind = self.kind("a placement", value)
        row = lambda op, flags=0, a=0, b=0, c=0, f=0.0, d=0: (op, flags, a, b, c, f, d)
        if kind == "count":
            self.need(kind, value, {"count"})
            return row("kCount", a=self.int_provider(value["count"])), True
        if kind in ("in_square", "biome"):
            self.need(kind, value, set())
            return row("kInSquare" if kind == "in_square" else "kBiome"), True
        if kind == "height_range":
            self.need(kind, value, {"height"})
            return row("kHeightRange", a=self.height_provider(value["height"])), True
        if kind == "heightmap":
            self.need(kind, value, {"heightmap"})
            return row("kHeightmap", a=HEIGHTMAPS.index(value["heightmap"])), True
        if kind == "rarity_filter":
            self.need(kind, value, {"chance"})
            return row("kRarityFilter", a=value["chance"]), True
        if kind == "random_chance":
            self.need(kind, value, {"chance"})
            return row("kRandomChance", f=float(value["chance"])), True
        if kind == "block_predicate_filter":
            self.need(kind, value, {"predicate"})
            at, ported = self.predicate(value["predicate"])
            return row("kBlockPredicateFilter", a=at), ported
        if kind == "offset":
            self.need(kind, value, {"x", "y", "z"})
            return row("kOffset", a=self.int_provider(value["x"]), b=self.int_provider(value["y"]), c=self.int_provider(value["z"])), True
        if kind == "environment_scan":
            self.need(kind, value, {"direction_of_search", "target_condition", "max_steps"}, {"allowed_search_condition"})
            if value["direction_of_search"] not in ("up", "down"):
                die(f"gen_features: a search to the {value['direction_of_search']}")
            target, ported = self.predicate(value["target_condition"])
            allowed, also = self.predicate(value.get("allowed_search_condition", {"type": "minecraft:true"}))
            return row("kEnvironmentScan", flags=int(value["direction_of_search"] == "up"), a=target, b=allowed, d=value["max_steps"]), ported and also
        if kind == "surface_water_depth_filter":
            self.need(kind, value, {"max_water_depth"})
            return row("kSurfaceWaterDepthFilter", a=value["max_water_depth"]), True
        if kind == "surface_relative_threshold_filter":
            self.need(kind, value, {"heightmap"}, {"min_inclusive", "max_inclusive"})
            return row("kSurfaceRelativeThresholdFilter", a=HEIGHTMAPS.index(value["heightmap"]), b=value.get("min_inclusive", INT_MIN),
                       c=value.get("max_inclusive", INT_MAX)), True
        if kind == "noise_threshold_count":
            self.need(kind, value, {"noise_level", "below_noise", "above_noise"})
            return row("kNoiseThresholdCount", a=value["below_noise"], b=value["above_noise"], d=self.doubles.run([float(value["noise_level"])])), True
        if kind == "noise_based_count":
            self.need(kind, value, {"noise_to_count_ratio", "noise_factor"}, {"noise_offset"})
            return row("kNoiseBasedCount", a=value["noise_to_count_ratio"],
                       d=self.doubles.run([float(value["noise_factor"]), float(value.get("noise_offset", 0.0))])), True
        if kind == "cuboid":
            self.need(kind, value, {"xz_size", "y_size"}, {"include_edges", "include_interior"})
            flags = int(value.get("include_edges", True)) | int(value.get("include_interior", True)) << 1
            return row("kCuboid", flags=flags, a=self.int_provider(value["xz_size"]), b=self.int_provider(value["y_size"])), True
        if kind == "randomly_selected":
            self.need(kind, value, {"placements"})
            made = [self.modifier(v) for v in value["placements"]]
            if not made:
                die("gen_features: one of no placements")
            return row("kRandomlySelected", a=self.modifiers.run([m for m, _ in made]), b=len(made)), all(p for _, p in made)
        die(f"gen_features: a placement of the type {kind}: src/worldgen has no word for it")

    # ---------------------------------------------------------------- what

    def _new_placed(self, name: str) -> int:
        self.placed.append(None)
        self.placed_names.append(name)
        return len(self.placed) - 1

    def _fill_waiting(self, at: int) -> None:
        name = self.waiting.pop(at, None)
        if name is not None:
            self.fill_placed(at, self.load("placed_feature", name))

    def _asks_biome(self, first: int, count: int) -> bool:
        for op, _, a, b, *_ in self.modifiers.rows[first:first + count]:
            if op == "kBiome" or (op == "kRandomlySelected" and self._asks_biome(a, b)):
                return True
        return False

    def placed_ref(self, value) -> int:
        """A Holder<PlacedFeature>: a name of the registry, or one written out where it is used."""
        if isinstance(value, str):
            at = self.named_placed.get(value)
            if at is None:
                at = self.named_placed[value] = self._new_placed(value)
                self.waiting[at] = value
            self._fill_waiting(at)
        else:
            key = canon(value)
            at = self.inline_placed.get(key)
            if at is None:
                at = self.inline_placed[key] = self._new_placed("")
                self.fill_placed(at, value)
        if self.placed[at] is None:
            die(f"gen_features: the placed feature {self.placed_names[at] or 'written out in place'} is made of itself")
        return at

    def fill_placed(self, at: int, value) -> None:
        if not isinstance(value, dict) or set(value) != {"feature", "placement"} or not isinstance(value["placement"], list):
            die(f"gen_features: a placed feature written as {canon(value)[:200]}")
        made = [self.modifier(m) for m in value["placement"]]
        if len(made) > 255:
            die("gen_features: a placed feature of more than 255 placements")
        feature = self.feature_ref(value["feature"])
        where = all(p for _, p in made)
        self.placed[at] = (feature, self.modifiers.run([m for m, _ in made]), len(made), where and self.feature_ported[feature], where)

    def feature_ref(self, value) -> int:
        """A Holder<Feature>, the same two ways."""
        if isinstance(value, str):
            if value in self.making:
                die(f"gen_features: {value} is made of itself ({' -> '.join(self.making)})")
            if value not in self.named_features:
                self.making.append(value)
                self.named_features[value] = self.feature(self.load("feature", value))
                self.making.pop()
            return self.named_features[value]
        key = canon(value)
        if key not in self.inline_features:
            self.inline_features[key] = self.feature(value)
        return self.inline_features[key]

    def _placed_list(self, value) -> tuple[int, int, list[int]]:
        """A HolderSet<PlacedFeature> that is written out: its placed features one after the other."""
        if isinstance(value, (str, dict)):
            value = [value]
        if not isinstance(value, list) or not value or any(isinstance(v, str) and v.startswith("#") for v in value):
            die(f"gen_features: a list of placed features written as {canon(value)[:200]}")
        rows = [self.placed_ref(v) for v in value]
        return self.placed_lists.run(rows), len(rows), rows

    def feature(self, value) -> int:
        kind = self.kind("a feature", value)
        if kind not in PORTED_FEATURES:
            if kind not in UNPORTED_FEATURES:
                die(f"gen_features: a feature of the type {kind}: neither ported nor known to wait for it")
            self.unported_types[kind] = self.unported_types.get(kind, 0) + 1
            self.feature_ported.append(False)
            return self.features.run([("kUnported", 0, 0, 0)])
        uses: list[int] = []
        if kind == "no_op":
            self.need(kind, value, set())
            row = ("kNoOp", 0, 0, 0)
        elif kind == "random_selector":
            self.need(kind, value, {"features", "default"})
            entries = []
            for e in value["features"]:
                if set(e) != {"chance", "feature"}:
                    die(f"gen_features: one of a random selector's features written as {canon(e)[:200]}")
                entries.append((self.placed_ref(e["feature"]), float(e["chance"])))
            default = self.placed_ref(value["default"])
            uses = [p for p, _ in entries] + [default]
            row = ("kRandomSelector", self.chances.run(entries), len(entries), default)
        elif kind in ("simple_random_selector", "sequence", "overlay"):
            self.need(kind, value, {"features"})
            first, count, uses = self._placed_list(value["features"])
            row = (PORTED_FEATURES[kind], first, count, 0)
        elif kind == "weighted_random_selector":
            self.need(kind, value, {"features"})
            entries = []
            for e in value["features"]:
                if set(e) != {"data", "weight"}:
                    die(f"gen_features: one of a weighted selector's features written as {canon(e)[:200]}")
                entries.append((self.placed_ref(e["data"]), e["weight"]))
            uses = [p for p, _ in entries]
            row = ("kWeightedRandomSelector", self.weighted.run(entries), len(entries), sum(w for _, w in entries))
        elif kind == "random_boolean_selector":
            self.need(kind, value, {"feature_true", "feature_false"})
            uses = [self.placed_ref(value["feature_true"]), self.placed_ref(value["feature_false"])]
            row = ("kRandomBooleanSelector", uses[0], uses[1], 0)
        else:
            die(f"gen_features: {kind} is listed as ported and has no table")
        self.nested.update(uses)
        self.feature_ported.append(all(self.placed[p][3] for p in uses))
        return self.features.run([row])


def emit(hdr: str, version: str, vanilla, blocks, settings: dict) -> dict:
    t = FeatureTables(version, vanilla, blocks, settings)
    order = t.order
    if len(order["steps"]) != len(STEPS):
        die(f"gen_features: {len(order['steps'])} decoration steps: GenerationStep.Decoration had {len(STEPS)}")
    cfloat = lambda x: struct.unpack("<f", struct.pack("<f", x))[0].hex() + "f"
    out = [hdr, """
// What the biomes of the overworld put into a chunk once its terrain is made: placed features
// (where, how often), features (what), and the order vanilla runs them in. Made of the jar's
// data/minecraft/worldgen/{placed_feature,feature}/ and of features.json (mcgen.py worldgen):
// tools/mcgen/gen_features.py. Included by src/worldgen/feature.cpp only.

#include <string_view>

#include "base/types.hpp"

namespace proto::gen::worldgen::feature {

// IntProvider. kConstant: a. kUniform, kBiasedToBottom, kVeryBiasedToBottom: from a to b.
// kTrapezoid: from a to b, a plateau of c. kWeightedList: kIntEntries[a .. a + b), whose weights
// add up to c. kClamped: kInts[c], held between a and b. kClampedNormal: around fa by fb, held
// between a and b.
enum class IntOp : u8 { kConstant, kUniform, kTrapezoid, kWeightedList, kClamped, kClampedNormal, kBiasedToBottom, kVeryBiasedToBottom };
struct Int {
    IntOp op;
    i32 a, b, c;
    float fa, fb;
};
struct Entry {  // of a weighted list: a row of the table it is a list of
    u32 value;
    i32 weight;
};

// HeightProvider, its anchors already heights of this world. kConstant: min. kWeightedList:
// kHeightEntries[min .. min + max), whose weights add up to inner. kTrapezoid: inner is the plateau.
enum class HeightOp : u8 { kConstant, kUniform, kTrapezoid, kBiasedToBottom, kVeryBiasedToBottom, kWeightedList };
struct Height {
    HeightOp op;
    i32 min, max, inner;
};

// BlockPredicate. kStateIn: the block at the offset is in state set a. kNot: not kPredicates[a].
// kAllOf, kAnyOf: kPredicateLists[a .. a + b), asked in that order. kInsideWorld: the offset is
// within the world's height. kVolume: kPredicates[a] of every block from the offset to `to`.
// kWouldSurvive: the block state a could stand at the offset (its own canSurvive).
enum class PredicateOp : u8 { kStateIn, kNot, kAllOf, kAnyOf, kInsideWorld, kVolume, kWouldSurvive };
struct Predicate {
    PredicateOp op;
    i8 dx, dy, dz;
    u32 a, b;
    i8 to[3];
};

// PlacementModifier.
enum class ModifierOp : u8 {
    kCount,                           // a: kInts
    kInSquare,
    kHeightRange,                     // a: kHeights
    kHeightmap,                       // a: the heightmap
    kBiome,
    kRarityFilter,                    // a: once in so many
    kRandomChance,                    // f
    kBlockPredicateFilter,            // a: kPredicates
    kOffset,                          // a, b, c: kInts for x, y, z
    kEnvironmentScan,                 // a: what is looked for, b: what may be gone through, d: steps at most; flags 1: upwards
    kSurfaceWaterDepthFilter,         // a: at most so deep
    kSurfaceRelativeThresholdFilter,  // a: the heightmap, b .. c: where to it
    kNoiseThresholdCount,             // a: below, b: above, kDoubles[d]: the level
    kNoiseBasedCount,                 // a: the ratio, kDoubles[d]: the factor, kDoubles[d + 1]: the offset
    kCuboid,                          // a: kInts across, b: kInts up; flags 1: edges, 2: the inside
    kRandomlySelected,                // one of kModifiers[a .. a + b)
};
struct Modifier {
    ModifierOp op;
    u8 flags;
    i32 a, b, c;
    float f;
    u32 d;
};

// Feature. kRandomSelector: kChances[a .. a + b) one after the other, else the placed feature c.
// kSimpleRandomSelector, kSequence, kOverlay: kPlacedLists[a .. a + b). kWeightedRandomSelector:
// kWeighted[a .. a + b), whose weights add up to c. kRandomBooleanSelector: a if true, else b.
// kUnported: a type that waits for its code.
enum class FeatureType : u8 {
    kUnported, kNoOp, kRandomSelector, kSimpleRandomSelector, kWeightedRandomSelector, kRandomBooleanSelector, kSequence, kOverlay,
};
struct Feature {
    FeatureType type;
    bool ported;  // it and whatever it is made of
    u32 a, b, c;
};
struct Chance {
    u32 placed;
    float chance;
};

// PlacedFeature: kModifiers[first .. first + modifiers), then the feature.
struct Placed {
    u16 feature;
    u16 first;
    u8 modifiers;
    bool ported;  // all of it
    bool where;   // its placements, whatever its feature is
};
"""]
    ints = t.ints.rows
    out.append(f"\ninline constexpr Int kInts[{max(1, len(ints))}] = {{\n")
    out += [f"    {{IntOp::{op}, {a}, {b}, {c}, {cfloat(fa)}, {cfloat(fb)}}},\n" for op, a, b, c, fa, fb in ints]
    out.append("};\n")

    def entries(name: str, rows: list) -> None:
        out.append(f"inline constexpr Entry {name}[{max(1, len(rows))}] = {{" + ", ".join(f"{{{v}, {w}}}" for v, w in rows) + "};\n")

    entries("kIntEntries", t.int_entries.rows)
    out.append(f"\ninline constexpr Height kHeights[{max(1, len(t.heights.rows))}] = {{\n")
    out += [f"    {{HeightOp::{op}, {a}, {b}, {c}}},\n" for op, a, b, c in t.heights.rows]
    out.append("};\n")
    entries("kHeightEntries", t.height_entries.rows)
    out.append(f"\ninline constexpr Predicate kPredicates[{max(1, len(t.predicates.rows))}] = {{\n")
    out += [f"    {{PredicateOp::{op}, {dx}, {dy}, {dz}, {a}, {b}, {{{to[0]}, {to[1]}, {to[2]}}}}},\n" for op, (dx, dy, dz), a, b, to in t.predicates.rows]
    out.append("};\n")
    lists = t.predicate_lists.rows
    out.append(f"inline constexpr u16 kPredicateLists[{max(1, len(lists))}] = {{{', '.join(map(str, lists)) or '0'}}};\n")
    out.append(f"inline constexpr double kDoubles[{max(1, len(t.doubles.rows))}] = {{{', '.join(float(d).hex() for d in t.doubles.rows) or '0.0'}}};\n")
    out.append(f"\ninline constexpr Modifier kModifiers[{max(1, len(t.modifiers.rows))}] = {{\n")
    out += [f"    {{ModifierOp::{op}, {flags}, {a}, {b}, {c}, {cfloat(f)}, {d}}},\n" for op, flags, a, b, c, f, d in t.modifiers.rows]
    out.append("};\n")
    out.append(f"\ninline constexpr Feature kFeatures[{len(t.features.rows)}] = {{\n")
    out += [f"    {{FeatureType::{op}, {str(t.feature_ported[i]).lower()}, {a}, {b}, {c}}},\n" for i, (op, a, b, c) in enumerate(t.features.rows)]
    out.append("};\n")
    out.append(f"inline constexpr Chance kChances[{max(1, len(t.chances.rows))}] = {{"
               + ", ".join(f"{{{p}, {cfloat(c)}}}" for p, c in t.chances.rows) + "};\n")
    out.append(f"inline constexpr u16 kPlacedLists[{max(1, len(t.placed_lists.rows))}] = {{{', '.join(map(str, t.placed_lists.rows)) or '0'}}};\n")
    entries("kWeighted", t.weighted.rows)
    if len(t.placed) > 65535 or len(t.features.rows) > 65535 or len(t.modifiers.rows) > 65535:
        die("gen_features: more rows than the tables' 16 bits count")
    out.append(f"""
// The first kNamedPlaced are the placed features the biomes name, in vanilla's order; then come
// the ones features are made of. kPlacedNames: the name of each that has one in the registry.
inline constexpr u32 kNamedPlaced = {t.named};
inline constexpr Placed kPlaced[{len(t.placed)}] = {{
""")
    for at, (feature, first, count, ported, where) in enumerate(t.placed):
        out.append(f"    {{{feature}, {first}, {count}, {str(ported).lower()}, {str(where).lower()}}},"
                   f"{'  // ' + strip(t.placed_names[at]) if t.placed_names[at] else ''}\n")
    out.append("};\n")
    out.append(f"inline constexpr std::string_view kPlacedNames[{len(t.placed_names)}] = {{\n")
    out += [f'    "{strip(name)}",\n' for name in t.placed_names]
    out.append("};\n")

    # The order: per step the placed features by index; per biome and step the indices it has.
    widest = max(len(names) for names in order["steps"])
    words = (widest + 63) // 64
    firsts, flat = [], []
    for names in order["steps"]:
        firsts.append(len(flat))
        flat += [t.named_placed[name] for name in names]
    firsts.append(len(flat))
    out.append(f"""
// GenerationStep.Decoration, and FeatureSorter's order: the placed features of step s are
// kStepPlaced[kStepFirst[s] .. kStepFirst[s + 1]), and a feature's place among them is the index
// its random source is seeded with.
inline constexpr u32 kSteps = {len(STEPS)};
inline constexpr u16 kStepFirst[kSteps + 1] = {{{', '.join(map(str, firsts))}}};
inline constexpr u16 kStepPlaced[{len(flat)}] = {{{', '.join(map(str, flat))}}};
inline constexpr std::string_view kStepNames[kSteps] = {{{', '.join(f'"{s}"' for s in STEPS)}}};

// Which of a step's features a biome has (BiomeGenerationSettings.features, and what hasFeature
// answers): a bit an index, by the biome's network id. A biome that is none of the overworld's has none.
inline constexpr u32 kStepWords = {words};
inline constexpr u64 kBiomeFeatures[{len(t.biome_ids)}][kSteps][kStepWords] = {{
""")
    possible = [0, 0]
    for name, biome in sorted(t.biome_ids.items(), key=lambda kv: kv[1]):
        of = order["biomes"].get(name)
        rows = []
        for s in range(len(STEPS)):
            bits = 0
            if of is not None:
                if sorted(of["features"][s]) != sorted(of["has"][s]):
                    die(f"gen_features: {name}: hasFeature is not the biome's own list in step {s}: BiomeFilter needs a table of its own")
                for i in of["features"][s]:
                    bits |= 1 << i
            rows.append("{" + ", ".join(hex(bits >> (64 * w) & (1 << 64) - 1) + "ull" for w in range(words)) + "}")
        if of is not None:
            possible[biome >> 6] |= 1 << (biome & 63)
        out.append(f"    {{{', '.join(rows)}}},  // {strip(name)}\n")
    out.append("};\n")
    if set(order["biomes"]) - set(t.biome_ids):
        die(f"gen_features: features.json names biomes the jar has no file for: {sorted(set(order['biomes']) - set(t.biome_ids))[:4]}")
    out.append(f"// The biomes of the overworld (BiomeSource.possibleBiomes), a bit each.\n"
               f"inline constexpr u64 kPossibleBiomes[2] = {{{hex(possible[0])}ull, {hex(possible[1])}ull}};\n")
    width = (t.sets.count + 7) // 8
    out.append(f"""
// Sets of block states, a bit a state: set n is kStateSets[n * kStateSetBytes ..), state s its
// bit s & 7 of byte s >> 3.
inline constexpr u32 kStateSetCount = {len(t.sets.kept)}, kStateSetBytes = {width};
inline constexpr unsigned char kStateSets[] = {{
#embed "worldgen_state_sets.bin"
}};
// What each was made of: the predicate as the jar writes it, its keys in order (for whoever holds
// the sets to vanilla's own predicates).
inline constexpr std::string_view kStateSetSources[kStateSetCount] = {{
""")
    for source in t.sets.sources:
        out.append("    " + json.dumps(source) + ",\n")
    out.append(f"""}};

}}  // namespace proto::gen::worldgen::feature
""")
    ported = sum(1 for at in range(t.named) if t.placed[at][3])
    where = sum(1 for at in range(t.named) if t.placed[at][4])
    return {"worldgen_features.hpp": "".join(out), "worldgen_state_sets.bin": t.sets.bytes() or b"\0",
            "_stats": {"placed_features": len(t.placed), "placed_features_named": t.named, "placed_features_ported": ported, "placed_features_whose_placements_are": where,
                       "features": len(t.features.rows), "feature_types_waiting": len(t.unported_types),
                       "placements": len(t.modifiers.rows), "predicates": len(t.predicates.rows), "state_sets": len(t.sets.kept)}}
