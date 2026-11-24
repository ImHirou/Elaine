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
from gen_trees import TreeTables, emit_trees

HEIGHTMAPS = ["WORLD_SURFACE_WG", "WORLD_SURFACE", "OCEAN_FLOOR_WG", "OCEAN_FLOOR", "MOTION_BLOCKING", "MOTION_BLOCKING_NO_LEAVES"]
DIRECTIONS = ["down", "up", "north", "south", "west", "east"]
STEPS = ["raw_generation", "lakes", "local_modifications", "underground_structures", "surface_structures", "strongholds",
         "underground_ores", "underground_decoration", "fluid_springs", "vegetal_decoration", "top_layer_modification"]

# Feature types src/worldgen/feature.cpp places, with the name of their FeatureType.
PORTED_FEATURES = {
    "no_op": "kNoOp", "random_selector": "kRandomSelector", "simple_random_selector": "kSimpleRandomSelector",
    "weighted_random_selector": "kWeightedRandomSelector", "random_boolean_selector": "kRandomBooleanSelector",
    "sequence": "kSequence", "overlay": "kOverlay", "ore": "kOre",
    "disk": "kDisk", "underwater_magma": "kUnderwaterMagma", "block_blob": "kBlockBlob", "spring_feature": "kSpring", "lake": "kLake",
    "blue_ice": "kBlueIce", "spike": "kSpike", "iceberg": "kIceberg", "freeze_top_layer": "kFreezeTopLayer",
    "simple_block": "kSimpleBlock", "block_column": "kBlockColumn", "bamboo": "kBamboo", "vines": "kVines",
    "coral_tree": "kCoralTree", "coral_claw": "kCoralClaw", "huge_brown_mushroom": "kHugeBrownMushroom", "huge_red_mushroom": "kHugeRedMushroom",
    "tree": "kTree", "fallen_tree": "kFallenTree",
}
# ...and the types of the overworld that wait for theirs. A type in neither list stops the run.
UNPORTED_FEATURES = {
    "template", "vegetation_patch", "speleothem", "multiface_growth", "fossil", "speleothem_cluster", "root_system",
    "geode", "monster_room", "large_dripstone", "waterlogged_vegetation_patch", "sculk_patch",
}
# BlockStateProvider types that wait for theirs: none of the overworld's now.
UNPORTED_PROVIDERS: set = set()

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
        self.facts["liquid"] = column(sturdy["columns"]["liquid"])
        faces = worldgen.load(version, "features").get("face_occludes")
        if not faces or len(faces) != self.count:
            die("gen_features: worldgen/features.json does not say which faces of a state occlude - run: mcgen.py worldgen")
        for d, name in enumerate(DIRECTIONS):
            self.facts[f"face_occludes_{name}"] = column(v >> d & 1 for v in faces)
        self.facts["snow_survives_on"] = column(worldgen.load(version, "features")["snow_survives_on"])
        self.facts["solid_render"] = column(states["solid_render"])
        self.facts["air"] = column(states["air"])
        attaches = worldgen.load(version, "features").get("vine_attaches")
        if not attaches:
            die("gen_features: worldgen/features.json does not say what vines hold on to - run: mcgen.py worldgen")
        for d, name in enumerate(DIRECTIONS):
            self.facts[f"vine_attaches_{name}"] = column(v >> d & 1 for v in attaches)
        self.facts["sturdy_center_down"] = column(v >> 0 & 1 for v in sturdy["columns"]["sturdy_center"])
        # A state's fluid, as much as there is of it (gamedata: FluidState.getAmount).
        import gamedata as _gamedata
        amounts = [f["amount"] for f in _gamedata.load(version, "fluids")["states"]]
        self.facts["fluid_full"] = column(amounts[f] == 8 for f in sturdy["columns"]["fluid_state"])
        sources = [f["source"] for f in _gamedata.load(version, "fluids")["states"]]
        self.facts["fluid_source"] = column(sources[f] for f in sturdy["columns"]["fluid_state"])
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


class FeatureTables(TreeTables):
    def __init__(self, version: str, vanilla, blocks, settings: dict):
        self.vanilla, self.blocks, self.version = vanilla, blocks, version
        self.init_trees()
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
        self.rules, self.rule_lists = Pool(), Pool()
        self.ores, self.ore_targets = Pool(), Pool()
        self.providers, self.provider_rules = Pool(), Pool()
        self.named_providers: dict[str, tuple[int, bool]] = {}
        self.provider_entries, self.provider_states, self.rotations, self.int_states, self.noise_providers = Pool(), Pool(), Pool(), Pool(), Pool()
        self.provider_layers: list = []
        self.extra = worldgen.load(version, "features")
        self.noise_made = {p["provider"]: p for p in self.extra["noise_providers"]}
        self.survive_data = worldgen.load(version, "survive")
        self.survive, self.survive_pairs = Pool(), Pool()
        self.survive_of: dict[int, int] = {}       # state -> its row of self.survive
        self.survive_ported: list[bool] = []
        self.simple_blocks = Pool()
        self.columns, self.column_layers, self.bamboos, self.mushrooms, self.mushroom_caps = Pool(), Pool(), Pool(), Pool(), Pool()
        import gamedata
        self.classes = {b["name"]: b["class"] for b in gamedata.load(version, "blocks")["blocks"]}   # a block's class and what it extends
        self.by_state = {}                          # state id -> (block name, properties)
        for name, block in blocks.blocks.items():
            for state in block["states"]:
                self.by_state[state["id"]] = (name, state.get("properties", {}))
        self.disks, self.magmas, self.springs, self.lakes = Pool(), Pool(), Pool(), Pool()
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
            rule, ported = self.survive_rule(self.blocks.state(value["state"]))
            return ("row", ("kWouldSurvive", self.offset(value), rule, 0, (0, 0, 0)), ported)
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

    # ---------------------------------------------------------------- whether a block can stay

    def _survive_row(self, state: int):
        """BlockState.canSurvive of one state: (row, ported). What asks about one block is a set the jar made
        (mcgen.py worldgen survive); what asks more is a rule written from its class, of sets made here."""
        of = self.survive_data["states"].get(str(state))
        if of is None:
            die(f"gen_features: worldgen/survive.json knows nothing of block state {state} ({self.by_state[state][0]}) - run: mcgen.py worldgen")
        sets = self.sets
        pairs = lambda rows: ("kAll", self.survive_pairs.run(rows), len(rows), 0)
        kind, cls = of["kind"], of["class"]
        name, properties = self.by_state[state]
        if kind in ("always", "never"):
            return ("kAlways" if kind == "always" else "kNever", 0, 0, 0), True
        if kind == "one":
            bits = int.from_bytes(bytes.fromhex(self.survive_data["sets"][of["set"]]), "little")
            return pairs([(*of["at"], sets.keep(bits, f"survives {of['set']}"))]), True
        everything = sets.everything
        own = sets.of_blocks({name})
        named = lambda bits, what: sets.keep(bits, f"kSet {what}")   # (each is held to the jar under this name: WorldgenFeatures.golden)
        if cls == "CactusBlock":
            side = named(everything & ~(sets.facts["solid"] | sets.fluids_in("#minecraft:lava")), "a cactus has beside it")
            below = named(own | sets.block_tag("minecraft:supports_cactus"), "a cactus stands on")
            above = named(everything & ~sets.facts["liquid"], "that is no liquid")
            return pairs([(0, 0, -1, side), (1, 0, 0, side), (0, 0, 1, side), (-1, 0, 0, side), (0, -1, 0, below), (0, 1, 0, above)]), True
        if cls == "LilyPadBlock":
            on = named(sets.fluids_in("#minecraft:supports_lily_pad") | sets.block_tag("minecraft:supports_lily_pad"), "a lily pad lies on")
            return pairs([(0, -1, 0, on), (0, 0, 0, named(sets.fluids_in("minecraft:empty"), "that holds no fluid"))]), True
        if cls == "BigDripleafStemBlock":
            below = named(own | sets.block_tag("minecraft:supports_big_dripleaf"), "a big dripleaf's stem stands on")
            above = named(own | sets.of_blocks({"minecraft:big_dripleaf"}), "a big dripleaf's stem has above it")
            return pairs([(0, -1, 0, below), (0, 1, 0, above)]), True
        if cls == "SporeBlossomBlock":
            above = named(sets.facts["sturdy_center_down"] & ~sets.block_tag("minecraft:unstable_bottom_center"), "a spore blossom hangs from")
            return pairs([(0, 1, 0, above), (0, 0, 0, named(everything & ~sets.fluids_in("#minecraft:water"), "that holds no water"))]), True
        if cls == "TallSeagrassBlock" and properties.get("half") == "lower":
            below = named(sets.facts["sturdy_up"] & ~sets.block_tag("minecraft:cannot_support_seagrass"), "tall seagrass stands on")
            return pairs([(0, -1, 0, below), (0, 0, 0, named(sets.fluids_in("#minecraft:water") & sets.facts["fluid_full"], "full of water"))]), True
        if cls == "SmallDripleafBlock" and properties.get("half") == "lower":
            return ("kSmallDripleaf", named(sets.block_tag("minecraft:supports_small_dripleaf"), "a small dripleaf stands on"),
                    named(sets.fluids_in("minecraft:water"), "that holds a source of water"),
                    named(sets.block_tag("minecraft:supports_vegetation"), "plants stand on")), True
        if cls == "SugarCaneBlock":
            beside = named(sets.fluids_in("#minecraft:supports_sugar_cane_adjacently") | sets.block_tag("minecraft:supports_sugar_cane_adjacently"),
                           "sugar cane has beside its ground")
            return ("kSugarCane", named(own, "of sugar cane"), named(sets.block_tag("minecraft:supports_sugar_cane"), "sugar cane stands on"), beside), True
        if cls == "MushroomBlock":
            return ("kMushroom", named(sets.block_tag("minecraft:overrides_mushroom_light_requirement"), "a mushroom needs no dark on"),
                    named(sets.facts["solid_render"], "of whole blocks that let no light through"), 0), True
        return ("kUnported", 0, 0, 0), False

    def double_plant(self, state: int) -> tuple[int, int, int, int]:
        """DoublePlantBlock.placeAt: for a state of such a block its lower and its upper half - each as it is where
        there is no water, then as it is in water (copyWaterloggedFrom) - and four zeroes for any other block."""
        name, properties = self.by_state[state]
        if "DoublePlantBlock" not in self.classes[name]:
            return 0, 0, 0, 0
        out = []
        for wet in ("false", "true"):
            for half in ("lower", "upper"):
                out.append(self.blocks.state({"id": name, "properties": {**properties, "half": half, **({"waterlogged": wet} if "waterlogged" in properties else {})}}))
        return tuple(out)

    def survive_rule(self, state: int) -> tuple[int, bool]:
        if state not in self.survive_of:
            row, ported = self._survive_row(state)
            at = self.survive.add(row)
            if at == len(self.survive_ported):
                self.survive_ported.append(ported)
            self.survive_of[state] = at
        return self.survive_of[state], self.survive_ported[self.survive_of[state]]

    # ---------------------------------------------------------------- state providers

    def _layers(self, made: list) -> tuple[int, int]:
        first = len(self.provider_layers)
        self.provider_layers += made
        return first, len(made)

    def outcomes(self, value) -> set[int]:
        """Every state a provider can come to give (nothing of what a rule leaves as it is)."""
        if isinstance(value, str):
            return self.outcomes(self.load("block_state_provider", value))
        if "type" not in value:
            return {self.blocks.state(value)}
        kind = strip(value["type"])
        if kind == "rule_based":
            found = set().union(*[self.outcomes(r["then"]) for r in value["rules"]]) if value["rules"] else set()
            return found | (self.outcomes(value["fallback"]) if "fallback" in value else set())
        if kind == "weighted":
            return {self.blocks.state(e["data"]) for e in value["entries"]}
        if kind == "rotated":
            return {to for state in self.outcomes(value["state"]) for to in self.rotated(state)}
        if kind == "randomized_int":
            return {to for state in self.outcomes(value["source"]) for to in (self.int_values(state, value["property"]) or (0, [state]))[1]}
        if kind == "random_block":
            return {self.blocks.state(name) for name in self.tag_in_order(value["blocks"])}
        if kind in ("noise", "dual_noise"):
            return {self.blocks.state(v) for v in value["states"]}
        if kind == "noise_threshold":
            return {self.blocks.state(v) for v in [value["default_state"], *value["low_states"], *value["high_states"]]}
        die(f"gen_features: what a state provider of the type {kind} can give is not known")

    def rotated(self, state: int) -> list[int]:
        """RotatedBlockProvider: the state turned to each of the six directions (Direction's order) - its axis,
        its facing, where it has them (trySetValue: by the property itself, which the values it takes tell)."""
        name, properties = self.by_state[state]
        block = self.blocks.blocks[name]
        values = {p: sorted({s["properties"][p] for s in block["states"]}) for p in properties}
        out = []
        for direction, axis in (("down", "y"), ("up", "y"), ("north", "z"), ("south", "z"), ("west", "x"), ("east", "x")):
            turned = dict(properties)
            if values.get("axis") == ["x", "y", "z"]:                                     # BlockStateProperties.AXIS
                turned["axis"] = axis
            if values.get("facing") == sorted(DIRECTIONS):                               # ...FACING
                turned["facing"] = direction
            if axis != "y" and values.get("facing") == ["east", "north", "south", "west"]:  # ...HORIZONTAL_FACING
                turned["facing"] = direction
            if "facing" in values and values["facing"] not in (sorted(DIRECTIONS), ["east", "north", "south", "west"]):
                die(f"gen_features: {name} is turned by a provider and faces {values['facing']}: which property that is has to be looked up")
            if "axis" in values and values["axis"] != ["x", "y", "z"]:
                die(f"gen_features: {name} is turned by a provider and has the axes {values['axis']}")
            out.append(self.blocks.state({"id": name, "properties": turned}))
        return out

    def int_values(self, state: int, prop: str):
        """RandomizedIntStateProvider: the state with its property at each value it can take, lowest first - or
        None where the block has no such property."""
        name, properties = self.by_state[state]
        if prop not in properties:
            return None
        block = self.blocks.blocks[name]
        values = sorted({int(s["properties"][prop]) for s in block["states"]})
        if values != list(range(values[0], values[-1] + 1)):
            die(f"gen_features: the property {prop} of {name} takes the values {values}")
        return values[0], [self.blocks.state({"id": name, "properties": {**properties, prop: str(v)}}) for v in values]

    def tag_in_order(self, value) -> list[str]:
        """A HolderSet of blocks in the order the game holds it (what a random element of it is the n-th of)."""
        if isinstance(value, str) and value.startswith("#"):
            if value[1:] not in self.extra["block_tags"]:
                die(f"gen_features: the block tag {value}: worldgen/features.json does not have it")
            return self.extra["block_tags"][value[1:]]
        return [value] if isinstance(value, str) else list(value)

    def _states(self, values: list) -> tuple[int, int]:
        return self.provider_states.run([self.blocks.state(v) for v in values]), len(values)

    def provider(self, value) -> tuple[int, bool]:
        """A Holder<BlockStateProvider> - a name, a block state written out, or a provider with its type: its row,
        and whether src/worldgen has all of it."""
        if isinstance(value, str):
            if value not in self.named_providers:
                self.named_providers[value] = self.provider(self.load("block_state_provider", value))
            return self.named_providers[value]
        if not isinstance(value, dict):
            die(f"gen_features: a state provider written as {value!r}")
        if "type" not in value:  # (BlockStateProvider.STATE_OR_PROVIDER_CODEC: one or the other, never both)
            if not set(value) <= {"id", "properties"}:
                die(f"gen_features: a block state written as {canon(value)[:200]}")
            return self.providers.add(("kSimple", self.blocks.state(value), 0, 0, 0)), True
        kind = strip(value["type"])
        if kind == "rule_based":
            self.need(kind, value, {"rules"}, {"fallback"})
            rules, ported = [], True
            for rule in value["rules"]:
                if set(rule) != {"if_true", "then"}:
                    die(f"gen_features: a rule of a state provider written as {canon(rule)[:200]}")
                at, has_predicate = self.predicate(rule["if_true"])
                then, has_then = self.provider(rule["then"])
                rules.append((at, then))
                ported = ported and has_predicate and has_then
            fallback = 0
            if "fallback" in value:
                at, has_fallback = self.provider(value["fallback"])
                fallback, ported = at + 1, ported and has_fallback
            return self.providers.add(("kRuleBased", self.provider_rules.run(rules), len(rules), fallback, 0)), ported
        if kind == "weighted":
            self.need(kind, value, {"entries"})
            entries = []
            for e in value["entries"]:
                if set(e) != {"data", "weight"}:
                    die(f"gen_features: an entry of a weighted provider written as {canon(e)[:200]}")
                entries.append((self.blocks.state(e["data"]), e["weight"]))
            if not entries or sum(w for _, w in entries) <= 0:
                die("gen_features: a weighted provider of nothing")
            return self.providers.add(("kWeighted", self.provider_entries.run(entries), len(entries), sum(w for _, w in entries), 0)), True
        if kind == "rotated":
            self.need(kind, value, {"state"}, {"direction"})
            inner, ported = self.provider(value["state"])
            rows = [(state, *self.rotated(state)) for state in sorted(self.outcomes(value["state"]))]
            direction = DIRECTIONS.index(value["direction"]) + 1 if "direction" in value else 0
            return self.providers.add(("kRotated", inner, self.rotations.run(rows), len(rows), direction)), ported
        if kind == "randomized_int":
            self.need(kind, value, {"source", "property", "values"})
            inner, ported = self.provider(value["source"])
            rows = []
            for state in sorted(self.outcomes(value["source"])):
                made = self.int_values(state, value["property"])
                if made:
                    rows.append((state, made[0], len(made[1]), self.provider_states.run(made[1])))
            return self.providers.add(("kRandomizedInt", inner, self.int_states.run(rows), len(rows), self.int_provider(value["values"]))), ported
        if kind == "random_block":
            self.need(kind, value, {"blocks"})
            first, count = self._states(self.tag_in_order(value["blocks"]))
            return self.providers.add(("kRandomBlock", first, count, 0, 0)), True
        if kind in ("noise", "dual_noise", "noise_threshold"):
            made = self.noise_made.get(canon(value))
            if made is None:
                die(f"gen_features: worldgen/features.json has no noise for the provider {canon(value)[:160]} - run: mcgen.py worldgen")
            row = {"kind": {"noise": "kNoise", "dual_noise": "kDualNoise", "noise_threshold": "kNoiseThreshold"}[kind],
                   "noise": self._layers(made["noise"]), "slow": (0, 0), "scale": float(value["scale"]), "slow_scale": 0.0,
                   "states": (0, 0), "variety": (0, 0), "threshold": 0.0, "high_chance": 0.0, "default": 0, "low": (0, 0), "high": (0, 0)}
            if kind == "noise_threshold":
                self.need(kind, value, {"seed", "noise", "scale", "threshold", "high_chance", "default_state", "low_states", "high_states"})
                row.update(threshold=float(value["threshold"]), high_chance=float(value["high_chance"]), default=self.blocks.state(value["default_state"]),
                           low=self._states(value["low_states"]), high=self._states(value["high_states"]))
            else:
                self.need(kind, value, {"seed", "noise", "scale", "states"} | ({"slow_noise", "slow_scale", "variety"} if kind == "dual_noise" else set()))
                row["states"] = self._states(value["states"])
                if kind == "dual_noise":
                    variety = value["variety"]
                    if isinstance(variety, dict):
                        variety = [variety["min_inclusive"], variety["max_inclusive"]]
                    row.update(slow=self._layers(made["slow_noise"]), slow_scale=float(value["slow_scale"]), variety=tuple(variety))
            return self.providers.add(("kNoise", self.noise_providers.run([row]), 0, 0, 0)), True
        if kind in UNPORTED_PROVIDERS:
            return self.providers.add(("kUnported", 0, 0, 0, 0)), False
        die(f"gen_features: a state provider of the type {kind}: neither ported nor known to wait for it")

    # ---------------------------------------------------------------- rule tests

    def _rule(self, value):
        """A RuleTest (what an ore may replace): ("in", set of states, as written), or ("row", the row)."""
        if not isinstance(value, dict) or not isinstance(value.get("predicate_type"), str):
            die(f"gen_features: a rule test written as {value!r}")
        kind = strip(value["predicate_type"])
        fields = set(value) - {"predicate_type"}

        def need(*wanted: str) -> None:
            if fields != set(wanted):
                die(f"gen_features: the rule test {kind} has the fields {sorted(fields)}: src/worldgen reads {sorted(wanted)}")

        if kind == "always_true":
            need()
            return ("in", self.sets.everything, value)
        if kind == "tag_match":
            need("tag")
            return ("in", self.sets.block_tag(value["tag"]), value)
        if kind == "block_match":
            need("block")
            return ("in", self.sets.of_blocks({value["block"]}), value)
        if kind == "height_match":
            need("min_inclusive", "max_inclusive")
            return ("row", ("kHeight", value["min_inclusive"], value["max_inclusive"]))
        if kind == "not":
            need("rule")
            inner = self._rule(value["rule"])
            if inner[0] == "in":
                return ("in", self.sets.everything & ~inner[1], value)
            return ("row", ("kNot", self._rule_row(inner), 0))
        if kind in ("all_of", "any_of"):
            need("rules")
            inner = [self._rule(r) for r in value["rules"]]
            if inner and all(r[0] == "in" for r in inner):
                bits = inner[0][1]
                for r in inner[1:]:
                    bits = bits & r[1] if kind == "all_of" else bits | r[1]
                return ("in", bits, value)
            rows = [self._rule_row(r) for r in inner]
            return ("row", ("kAllOf" if kind == "all_of" else "kAnyOf", self.rule_lists.run(rows), len(rows)))
        die(f"gen_features: a rule test of the type {kind}: src/worldgen has no word for it")

    def _rule_row(self, made) -> int:
        if made[0] == "in":
            return self.rules.add(("kStateIn", self.sets.keep(made[1], made[2]), 0))
        return self.rules.add(made[1])

    def rule(self, value) -> int:
        return self._rule_row(self._rule(value))

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
        parts = True  # the providers and predicates it is made of are ported too
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
        elif kind == "ore":
            self.need(kind, value, {"targets", "size", "discard_chance_on_air_exposure"})
            targets = []
            for target in value["targets"]:
                if set(target) != {"target", "state"}:
                    die(f"gen_features: one of an ore's targets written as {canon(target)[:200]}")
                targets.append((self.rule(target["target"]), self.blocks.state(target["state"])))
            if not 0 <= value["size"] <= 64:
                die(f"gen_features: an ore of the size {value['size']}")
            row = ("kOre", self.ores.run([(value["size"], float(value["discard_chance_on_air_exposure"]), self.ore_targets.run(targets), len(targets))]), 0, 0)
        elif kind == "disk":
            self.need(kind, value, {"state_provider", "target", "radius", "half_height"})
            provider, has = self.provider(value["state_provider"])
            target, has_target = self.predicate(value["target"])
            parts = parts and has and has_target
            row = ("kDisk", self.disks.run([(provider, target, self.int_provider(value["radius"]), value["half_height"])]), 0, 0)
        elif kind == "underwater_magma":
            self.need(kind, value, {"floor_search_range", "placement_radius_around_floor", "placement_probability_per_valid_position"})
            row = ("kUnderwaterMagma", self.magmas.run([(value["floor_search_range"], value["placement_radius_around_floor"],
                                                           float(value["placement_probability_per_valid_position"]))]), 0, 0)
        elif kind == "block_blob":
            self.need(kind, value, {"state", "can_place_on"})
            on, has = self.predicate(value["can_place_on"])
            parts = parts and has
            row = ("kBlockBlob", self.blocks.state(value["state"]), on, 0)
        elif kind == "spring_feature":
            self.need(kind, value, {"state", "valid_blocks"}, {"requires_block_below", "rock_count", "hole_count"})
            # FluidState.CODEC, and FluidState.createLegacyBlock: the fluid's own block, as full as the fluid is.
            fluid = value["state"]
            if set(fluid) - {"id", "properties"} or fluid.get("id") not in ("minecraft:water", "minecraft:lava") or \
                    fluid.get("properties", {}) != {"falling": "true"}:
                die(f"gen_features: a spring of {canon(fluid)}: src/worldgen knows the falling source of water and of lava")
            block = self.blocks.state({"id": fluid["id"], "properties": {"level": "0"}})  # (FlowingFluid.getLegacyLevel: a source is 0, falling or not)
            valid = self.sets.keep(self.sets.blocks_in(value["valid_blocks"]), {"type": "minecraft:matching_blocks", "blocks": value["valid_blocks"]})
            row = ("kSpring", self.springs.run([(block, self.sets.fluid_names.index(fluid["id"]), int(value.get("requires_block_below", True)),
                                                 value.get("rock_count", 4), value.get("hole_count", 1), valid)]), 0, 0)
        elif kind == "lake":
            self.need(kind, value, {"fluid", "barrier", "can_place_feature", "can_replace_with_air_or_fluid", "can_replace_with_barrier"})
            made = [self.provider(value["fluid"]), self.provider(value["barrier"]), self.predicate(value["can_place_feature"]),
                    self.predicate(value["can_replace_with_air_or_fluid"]), self.predicate(value["can_replace_with_barrier"])]
            parts = parts and all(has for _, has in made)
            row = ("kLake", self.lakes.run([tuple(at for at, _ in made)]), 0, 0)
        elif kind in ("blue_ice", "freeze_top_layer"):
            self.need(kind, value, set())
            row = (PORTED_FEATURES[kind], 0, 0, 0)
        elif kind == "spike":
            self.need(kind, value, {"state", "can_place_on", "can_replace"})
            on, has_on = self.predicate(value["can_place_on"])
            replace, has_replace = self.predicate(value["can_replace"])
            parts = parts and has_on and has_replace
            row = ("kSpike", self.blocks.state(value["state"]), on, replace)
        elif kind == "iceberg":
            self.need(kind, value, {"state"})
            row = ("kIceberg", self.blocks.state(value["state"]), 0, 0)
        elif kind == "block_column":
            self.need(kind, value, {"layers", "direction", "allowed_placement", "prioritize_tip"})
            layers = []
            for layer in value["layers"]:
                if set(layer) != {"height", "provider"}:
                    die(f"gen_features: a layer of a column written as {canon(layer)[:200]}")
                provider, has = self.provider(layer["provider"])
                parts = parts and has
                layers.append((self.int_provider(layer["height"]), provider))
            allowed, has = self.predicate(value["allowed_placement"])
            parts = parts and has
            row = ("kBlockColumn", self.columns.run([(self.column_layers.run(layers), len(layers), DIRECTIONS.index(value["direction"]), allowed,
                                                    int(bool(value["prioritize_tip"])))]), 0, 0)
        elif kind == "bamboo":
            self.need(kind, value, {"probability"})
            stalk = lambda **properties: self.blocks.state({"id": "minecraft:bamboo", "properties": {"age": "1", "leaves": "none", "stage": "0", **properties}})
            # (BambooFeature's own states: BAMBOO_TRUNK, BAMBOO_FINAL_LARGE, BAMBOO_TOP_LARGE, BAMBOO_TOP_SMALL.)
            rule, has = self.survive_rule(self.blocks.state("minecraft:bamboo"))
            parts = parts and has
            under = self.sets.keep(self.sets.block_tag("minecraft:beneath_bamboo_podzol_replaceable"), "kSet podzol takes the place of under bamboo")
            row = ("kBamboo", self.bamboos.run([(float(value["probability"]), rule, stalk(), stalk(leaves="large", stage="1"), stalk(leaves="large"),
                                                 stalk(leaves="small"), self.blocks.state("minecraft:podzol"), under)]), 0, 0)
        elif kind == "vines":
            self.need(kind, value, set())
            # Direction.values() without DOWN: the vine that hangs on that side of its block.
            first, _ = self._states([{"id": "minecraft:vine", "properties": {d: "true"}} for d in DIRECTIONS[1:]])
            row = ("kVines", first, 0, 0)
        elif kind in ("coral_tree", "coral_claw"):
            self.need(kind, value, {"feature"})
            uses = [self.placed_ref(value["feature"])]
            row = (PORTED_FEATURES[kind], uses[0], 0, 0)
        elif kind in ("huge_brown_mushroom", "huge_red_mushroom"):
            self.need(kind, value, {"cap_provider", "stem_provider", "can_place_on"}, {"foliage_radius"})
            cap, has_cap = self.provider(value["cap_provider"])
            stem, has_stem = self.provider(value["stem_provider"])
            on, has_on = self.predicate(value["can_place_on"])
            parts = parts and has_cap and has_stem and has_on
            # A cap's block with its faces as the cap says: by up << 4 | west << 3 | east << 2 | north << 1 | south.
            caps = []
            for state in sorted(self.outcomes(value["cap_provider"])):
                name, properties = self.by_state[state]
                if not {"up", "west", "east", "north", "south"} <= set(properties):
                    continue  # (HugeMushroomBlock's properties: a cap of anything else stays as it is)
                flag = lambda bit: "true" if bit else "false"
                caps.append((state, int(properties["up"] == "true"),
                             *[self.blocks.state({"id": name, "properties": {**properties, "up": flag(i >> 4 & 1), "west": flag(i >> 3 & 1), "east": flag(i >> 2 & 1),
                                                                             "north": flag(i >> 1 & 1), "south": flag(i & 1)}}) for i in range(32)]))
            row = (PORTED_FEATURES[kind], self.mushrooms.run([(cap, stem, value.get("foliage_radius", 2), on, self.mushroom_caps.run(caps), len(caps))]), 0, 0)
        elif kind == "simple_block":
            self.need(kind, value, {"to_place"}, {"schedule_tick"})
            provider, has = self.provider(value["to_place"])
            # What it may come to place is asked whether it can stay - and placed its own way, if its class has one.
            first = len(self.simple_blocks.rows)
            for state in sorted(self.outcomes(value["to_place"])):
                rule, has_rule = self.survive_rule(state)
                # (MossyCarpetBlock.placeAt - the carpet of pale moss, with its sides - is not here yet.)
                parts = parts and has_rule and "MossyCarpetBlock" not in self.classes[self.by_state[state][0]]
                self.simple_blocks.rows.append((state, rule, *self.double_plant(state)))
            parts = parts and has
            row = ("kSimpleBlock", provider, first | (len(self.simple_blocks.rows) - first) << 20, int(bool(value.get("schedule_tick", False))))
        elif kind == "tree":
            row, has = self.tree(value)
            parts = parts and has
        elif kind == "fallen_tree":
            row, has = self.fallen_tree(value)
            parts = parts and has
        else:
            die(f"gen_features: {kind} is listed as ported and has no table")
        self.nested.update(uses)
        self.feature_ported.append(parts and all(self.placed[p][3] for p in uses))
        return self.features.run([row])


def emit(hdr: str, version: str, vanilla, blocks, settings: dict) -> dict:
    t = FeatureTables(version, vanilla, blocks, settings)
    t.make_shapes()
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
// kWouldSurvive: a block state could stand at the offset - by kSurvive[a], its own canSurvive.
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
// kOre: kOres[a]. kDisk: kDisks[a]. kUnderwaterMagma: kMagmas[a]. kBlockBlob: state a, on what the
// predicate b holds for. kSpring: kSprings[a]. kLake: kLakes[a]. kSpike: state a, on what predicate b
// holds for, through what predicate c holds for. kIceberg: of state a. kBlueIce, kFreezeTopLayer: no
// more to say. kSimpleBlock: what the provider a gives - one of kSimpleBlocks[b & 0xFFFFF .. + (b >> 20)) -
// if it can stay; with a tick asked for if c. kBlockColumn: kColumns[a]. kBamboo: kBamboos[a]. kVines:
// one of kProviderStates[a .. a + 5), a vine on the side up, north, south, west, east. kCoralTree,
// kCoralClaw: of the placed feature a, block by block. kHugeBrownMushroom, kHugeRedMushroom:
// kMushrooms[a]. kTree: kTrees[a]. kFallenTree: kFallenTrees[a]. kUnported: a type that waits for its code.
enum class FeatureType : u8 {
    kUnported, kNoOp, kRandomSelector, kSimpleRandomSelector, kWeightedRandomSelector, kRandomBooleanSelector, kSequence, kOverlay,
    kOre, kDisk, kUnderwaterMagma, kBlockBlob, kSpring, kLake, kBlueIce, kSpike, kIceberg, kFreezeTopLayer, kSimpleBlock,
    kBlockColumn, kBamboo, kVines, kCoralTree, kCoralClaw, kHugeBrownMushroom, kHugeRedMushroom, kTree, kFallenTree,
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

// RuleTest: what a block state at a height is to an ore. kStateIn: in state set a. kHeight: the
// block's y is a .. b. kNot: not kRules[a]. kAllOf, kAnyOf: kRuleLists[a .. a + b), in that order.
enum class RuleOp : u8 { kStateIn, kHeight, kNot, kAllOf, kAnyOf };
struct Rule {
    RuleOp op;
    i32 a, b;
};
// OreFeature: so many blocks at most, laid into what kOreTargets[first .. first + targets) say
// may be replaced, each with what replaces it - the first that holds for a block.
struct OreTarget {
    u16 rule;
    u16 state;
};
struct Ore {
    u8 size;
    u8 targets;
    u16 first;
    float discard_chance_on_air_exposure;
};

// BlockStateProvider. kSimple: the state a. kRuleBased: kProviderRules[a .. a + b) in order - the
// first whose predicate holds and whose provider has a state - and failing them the provider c - 1,
// if c is not 0. kUnported: a type that waits for its code.
// kWeighted: one of kProviderEntries[a .. a + b), whose weights add up to c. kRotated: what the
// provider a gives, turned to a direction that is drawn (or, d not 0, to direction d - 1): the row
// of that state among kRotations[b .. b + c). kRandomizedInt: what the provider a gives with a
// property of it set to a number drawn of kInts[d] - the row of that state among
// kIntStates[b .. b + c), or the state as it is where there is none. kRandomBlock: one of
// kProviderStates[a .. a + b). kNoise: kNoiseProviders[a].
enum class ProviderOp : u8 { kSimple, kRuleBased, kWeighted, kRotated, kRandomizedInt, kRandomBlock, kNoise, kUnported };
struct Provider {
    ProviderOp op;
    u32 a, b, c, d;
};
struct ProviderRule {
    u16 predicate, provider;
};
struct Rotation {
    u16 state;
    u16 to[6];  // by Direction's ordinal
};
struct IntStates {
    u16 state;
    i32 min;  // the least value of the property
    u16 count;
    u32 first;  // kProviderStates[first + value - min]
};
// A layer of a noise that is the same in every world: as it stands once it is made.
struct ProviderLayer {
    double frequency;
    float amplitude;
    double offsets[3];
    u8 perms[256];
};
// NoiseProvider, DualNoiseProvider, NoiseThresholdProvider: kProviderLayers[noise .. noise + layers)
// and, of the dual one, [slow .. slow + slow_layers); states and the like are runs of kProviderStates.
enum class NoiseKind : u8 { kNoise, kDualNoise, kNoiseThreshold };
struct NoiseProvider {
    NoiseKind kind;
    u16 noise, layers, slow, slow_layers;
    float scale, slow_scale;
    u16 states, state_count;
    i32 variety_min, variety_max;
    float threshold, high_chance;
    u16 default_state;
    u16 low, low_count, high, high_count;
};

// BlockState.canSurvive. kAll: each of kSurvivePairs[a .. a + b) holds - the block at its offset is in
// its set. kSugarCane: the block below is of set a, or of set b with a block of set c on one of its
// four sides. kSmallDripleaf: the block below is of set a, or the place holds a block of set b and
// the block below is of set c. kMushroom: the block below is of set a, or it is dark enough and
// that block is of set b. kUnported: a class that waits for its code.
enum class SurviveOp : u8 { kAlways, kNever, kAll, kSugarCane, kSmallDripleaf, kMushroom, kUnported };
struct Survive {
    SurviveOp op;
    u32 a, b, c;
};
struct SurvivePair {
    i8 dx, dy, dz;
    u16 set;
};
// SimpleBlockFeature: what a state its provider can give is to it - the rule it stays by, and, of a
// plant of two blocks, its two halves (dry: lower, upper; in water: the same two).
struct SimpleBlock {
    u16 state;
    u16 survive;
    u16 halves[4];
};
// DiskFeature, UnderwaterMagmaFeature, SpringFeature, LakeFeature: their fields, with predicates,
// providers and numbers as rows of their tables.
struct Disk {
    u16 provider, target, radius;
    u8 half_height;
};
struct Magma {
    i32 floor_search_range, placement_radius_around_floor;
    float placement_probability_per_valid_position;
};
struct Spring {
    u16 state;  // FluidState.createLegacyBlock
    u8 fluid;   // the fluid that is ticked
    bool requires_block_below;
    i32 rock_count, hole_count;
    u16 valid_blocks;  // a state set
};
struct Lake {
    u16 fluid, barrier;  // providers
    u16 can_place_feature, can_replace_with_air_or_fluid, can_replace_with_barrier;
};

// BlockColumnFeature: kColumnLayers[first .. first + layers), each so many blocks of what its provider
// says, one after the other in a direction, as far as the predicate allows.
struct ColumnLayer {
    u16 height;  // kInts
    u16 provider;
};
struct Column {
    u16 first, layers;
    u8 direction;
    u16 allowed_placement;
    bool prioritize_tip;
};
// BambooFeature: how often podzol lies round it, the rule the stalk stays by, and the blocks it is made of.
struct Bamboo {
    float probability;
    u16 survive;
    u16 trunk, final_large, top_large, top_small, podzol;
    u16 podzol_replaces;  // a state set
};
// The big mushrooms: kMushroomCaps[caps .. caps + cap_count) say what a block of the cap is with its
// faces set, for every state the cap's provider gives.
struct Mushroom {
    u16 cap, stem;  // providers
    i32 foliage_radius;
    u16 can_place_on;
    u16 caps, cap_count;
};
struct MushroomCap {
    u16 state;
    u8 up;       // whether the state itself has its upper face
    u16 to[32];  // by up << 4 | west << 3 | east << 2 | north << 1 | south
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
    out.append(f"\ninline constexpr Rule kRules[{max(1, len(t.rules.rows))}] = {{"
               + ", ".join(f"{{RuleOp::{op}, {a}, {b}}}" for op, a, b in t.rules.rows) + "};\n")
    out.append(f"inline constexpr u16 kRuleLists[{max(1, len(t.rule_lists.rows))}] = {{{', '.join(map(str, t.rule_lists.rows)) or '0'}}};\n")
    out.append(f"inline constexpr OreTarget kOreTargets[{max(1, len(t.ore_targets.rows))}] = {{"
               + ", ".join(f"{{{rule}, {state}}}" for rule, state in t.ore_targets.rows) + "};\n")
    out.append(f"inline constexpr Ore kOres[{max(1, len(t.ores.rows))}] = {{"
               + ", ".join(f"{{{size}, {count}, {first}, {cfloat(chance)}}}" for size, chance, first, count in t.ores.rows) + "};\n")
    out.append(f"inline constexpr Provider kProviders[{max(1, len(t.providers.rows))}] = {{"
               + ", ".join(f"{{ProviderOp::{op}, {a}, {b}, {c}, {d}}}" for op, a, b, c, d in t.providers.rows) + "};\n")
    out.append(f"inline constexpr Entry kProviderEntries[{max(1, len(t.provider_entries.rows))}] = {{"
               + ", ".join(f"{{{state}, {weight}}}" for state, weight in t.provider_entries.rows) + "};\n")
    out.append(f"inline constexpr u16 kProviderStates[{max(1, len(t.provider_states.rows))}] = {{{', '.join(map(str, t.provider_states.rows)) or '0'}}};\n")
    out.append(f"inline constexpr Rotation kRotations[{max(1, len(t.rotations.rows))}] = {{"
               + ", ".join(f"{{{row[0]}, {{{', '.join(map(str, row[1:]))}}}}}" for row in t.rotations.rows) + "};\n")
    out.append(f"inline constexpr IntStates kIntStates[{max(1, len(t.int_states.rows))}] = {{"
               + ", ".join(f"{{{a}, {b}, {c}, {d}}}" for a, b, c, d in t.int_states.rows) + "};\n")
    as_double = lambda bits: struct.unpack("<d", struct.pack("<q", int(bits)))[0].hex()
    as_float = lambda bits: cfloat(struct.unpack("<f", struct.pack("<i", int(bits)))[0])
    out.append(f"inline constexpr ProviderLayer kProviderLayers[{max(1, len(t.provider_layers))}] = {{\n")
    for layer in t.provider_layers:
        out.append(f"    {{{as_double(layer['frequency'])}, {as_float(layer['amplitude'])}, {{{', '.join(as_double(o) for o in layer['offsets'])}}},\n"
                   f"     {{{', '.join(map(str, layer['perms']))}}}}},\n")
    out.append("};\n")
    out.append(f"inline constexpr NoiseProvider kNoiseProviders[{max(1, len(t.noise_providers.rows))}] = {{\n")
    for r in t.noise_providers.rows:
        out.append(f"    {{NoiseKind::{r['kind']}, {r['noise'][0]}, {r['noise'][1]}, {r['slow'][0]}, {r['slow'][1]}, {cfloat(r['scale'])}, {cfloat(r['slow_scale'])}, "
                   f"{r['states'][0]}, {r['states'][1]}, {r['variety'][0]}, {r['variety'][1]}, {cfloat(r['threshold'])}, {cfloat(r['high_chance'])}, "
                   f"{r['default']}, {r['low'][0]}, {r['low'][1]}, {r['high'][0]}, {r['high'][1]}}},\n")
    out.append("};\n")
    out.append(f"inline constexpr Survive kSurvive[{max(1, len(t.survive.rows))}] = {{"
               + ", ".join(f"{{SurviveOp::{op}, {a}, {b}, {c}}}" for op, a, b, c in t.survive.rows) + "};\n")
    out.append(f"inline constexpr SurvivePair kSurvivePairs[{max(1, len(t.survive_pairs.rows))}] = {{"
               + ", ".join(f"{{{dx}, {dy}, {dz}, {at}}}" for dx, dy, dz, at in t.survive_pairs.rows) + "};\n")
    out.append(f"inline constexpr ColumnLayer kColumnLayers[{max(1, len(t.column_layers.rows))}] = {{"
               + ", ".join(f"{{{a}, {b}}}" for a, b in t.column_layers.rows) + "};\n")
    out.append(f"inline constexpr Column kColumns[{max(1, len(t.columns.rows))}] = {{"
               + ", ".join(f"{{{a}, {b}, {c}, {d}, {'true' if e else 'false'}}}" for a, b, c, d, e in t.columns.rows) + "};\n")
    out.append(f"inline constexpr Bamboo kBamboos[{max(1, len(t.bamboos.rows))}] = {{"
               + ", ".join(f"{{{cfloat(row[0])}, {', '.join(map(str, row[1:]))}}}" for row in t.bamboos.rows) + "};\n")
    out.append(f"inline constexpr Mushroom kMushrooms[{max(1, len(t.mushrooms.rows))}] = {{"
               + ", ".join("{" + ", ".join(map(str, row)) + "}" for row in t.mushrooms.rows) + "};\n")
    out.append(f"inline constexpr MushroomCap kMushroomCaps[{max(1, len(t.mushroom_caps.rows))}] = {{"
               + ", ".join(f"{{{row[0]}, {row[1]}, {{{', '.join(map(str, row[2:]))}}}}}" for row in t.mushroom_caps.rows) + "};\n")
    out.append(f"inline constexpr SimpleBlock kSimpleBlocks[{max(1, len(t.simple_blocks.rows))}] = {{"
               + ", ".join(f"{{{state}, {rule}, {{{a}, {b}, {c}, {d}}}}}" for state, rule, a, b, c, d in t.simple_blocks.rows) + "};\n")
    # For the golden script (WorldgenSurvive.golden) and the test of the rules: which rule a state stays by.
    (vdir(version) / "worldgen" / "survive_rules.txt").write_text("".join(f"{state} {rule}\n" for state, rule in sorted(t.survive_of.items())))
    out.append(f"inline constexpr ProviderRule kProviderRules[{max(1, len(t.provider_rules.rows))}] = {{"
               + ", ".join(f"{{{a}, {b}}}" for a, b in t.provider_rules.rows) + "};\n")
    out.append(f"inline constexpr Disk kDisks[{max(1, len(t.disks.rows))}] = {{"
               + ", ".join(f"{{{a}, {b}, {c}, {d}}}" for a, b, c, d in t.disks.rows) + "};\n")
    out.append(f"inline constexpr Magma kMagmas[{max(1, len(t.magmas.rows))}] = {{"
               + ", ".join(f"{{{a}, {b}, {cfloat(c)}}}" for a, b, c in t.magmas.rows) + "};\n")
    out.append(f"inline constexpr Spring kSprings[{max(1, len(t.springs.rows))}] = {{"
               + ", ".join(f"{{{a}, {b}, {'true' if c else 'false'}, {d}, {e}, {f}}}" for a, b, c, d, e, f in t.springs.rows) + "};\n")
    out.append(f"inline constexpr Lake kLakes[{max(1, len(t.lakes.rows))}] = {{"
               + ", ".join("{" + ", ".join(map(str, row)) + "}" for row in t.lakes.rows) + "};\n")
    emit_trees(t, out)
    # What feature.cpp asks of a state by name: sets like any other, each with a name of its own.
    sets = t.sets
    code = {"kSetLiquid": sets.facts["liquid"], "kSetSnowSurvivesOn": sets.facts["snow_survives_on"],
            # AbstractHugeMushroomFeature: what a block of a mushroom takes the place of, and what its room may hold.
            "kSetMushroomGrowsThrough": sets.facts["air"] | sets.block_tag("minecraft:replaceable_by_mushrooms"),
            "kSetMushroomHasRoomIn": sets.facts["air"] | sets.block_tag("minecraft:leaves")}
    names = {"kSetMushroomGrowsThrough": "kSet a big mushroom grows through", "kSetMushroomHasRoomIn": "kSet a big mushroom has room in"}
    # Trees (tree.cpp) and the block updates at their edges (shape.hpp).
    tag = lambda name: sets.block_tag("minecraft:" + name)
    for name, words, bits in (
        ("kSetTreeGrowsThrough", "a tree grows through", sets.facts["air"] | tag("replaceable_by_trees")),            # TreeFeature.validTreePos
        ("kSetLogs", "of logs", tag("logs")),
        ("kSetAirOrLeaves", "a big mushroom has room in", sets.facts["air"] | tag("leaves")),                        # TreeFeature.isAirOrLeaves
        ("kSetKeepsLeaves", "that keeps leaves from decaying", tag("prevents_nearby_leaf_decay")),                    # LeavesBlock.getOptionalDistanceAt
        ("kSetSnowySetting", "that makes the ground under it snowy", tag("snow")),                                  # SnowyBlock.isSnowySetting
        ("kSetSolidRender", "of whole blocks that let no light through", sets.facts["solid_render"]),
        ("kSetReplaceable", "that gives way to a block placed", sets.facts["replaceable"]),                          # BlockState.canBeReplaced()
        ("kSetHoldsWater", "that holds water", sets.fluids_in("#minecraft:water")),                                   # FluidState.is(FluidTags.WATER)
        ("kSetWaterSource", "that holds a source of water", sum(1 << i for i, f in enumerate(sets.fluid_of) if sets.fluid_names[f] == "minecraft:water")),
        ("kSetFluidSource", "whose fluid is a source", sets.facts["fluid_source"]),                                  # FluidState.isSource()
        ("kSetBubbleColumnBelow", "a column of bubbles rises from", tag("enables_bubble_column_drag_down") | tag("enables_bubble_column_push_up")),
        ("kSetBubbleColumnIn", "a column of bubbles takes the place of",                                              # LiquidBlock.shouldBubbleColumnOccupy
         sets.fluids_in("#minecraft:bubble_column_can_occupy") & sets.facts["fluid_source"] & sets.facts["fluid_full"]),
        ("kSetPaleOakLogs", "of pale oak logs", tag("pale_oak_logs")),
        ("kSetSturdyUp", "with a whole face on top", sets.facts["sturdy_up"]),                                       # BlockState.isFaceSturdy(.., UP)
    ):
        code[name] = bits
        names[name] = "kSet " + words
    out.append("\n// Sets of block states that src/worldgen/feature.cpp asks about by name: BlockState.liquid(); what a\n"
               "// layer of snow may lie on (SnowLayerBlock.canSurvive, asked of the state below); and - a set a\n"
               "// direction, by its ordinal - whether BlockState.getFaceOcclusionShape(direction) is a whole face.\n")
    for name, bits in code.items():
        out.append(f"inline constexpr u32 {name} = {sets.keep(bits, names.get(name, name))};\n")
    out.append("inline constexpr u32 kSetFaceOccludes[6] = {" + ", ".join(str(sets.keep(sets.facts[f'face_occludes_{d}'], f'kSetFaceOccludes {d}')) for d in DIRECTIONS) + "};\n")
    out.append("// VineBlock.isAcceptableNeighbour: what a vine holds on to, lying in that direction from it.\n")
    out.append("inline constexpr u32 kSetVineAttaches[6] = {" + ", ".join(str(sets.keep(sets.facts[f'vine_attaches_{d}'], f'kSetVineAttaches {d}')) for d in DIRECTIONS) + "};\n")
    # SnowyBlock.SNOWY: a state without snow on it, and the same with.
    snowy = []
    for name, block in t.blocks.blocks.items():
        for state in block["states"]:
            if "snowy" in state.get("properties", {}):
                snowy.append((state["id"], t.blocks.state({"id": name, "properties": {**state["properties"], "snowy": "true"}})))
    out.append("// BlockState.setValue(SnowyBlock.SNOWY, true), for every state that has the property (one that is snowy: itself).\n")
    out.append(f"inline constexpr u16 kSnowy[{len(snowy)}][2] = {{" + ", ".join(f"{{{a}, {b}}}" for a, b in sorted(snowy)) + "};\n")
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
    math = (vdir(version) / "worldgen" / "math.bin").read_bytes() if (vdir(version) / "worldgen" / "math.bin").exists() else b""
    if math[:8] != b"WGMATH01":
        die("gen_features: worldgen/math.bin is missing or not what WorldgenMath.java writes - run: mcgen.py worldgen")
    math_hpp = hdr + """
// Where java.lang.Math's sine and cosine are not the ones src/worldgen computes for itself: for
// every angle a feature can make of a float, what is to be added to the bits of our own answer
// (tools/mcgen/extract/WorldgenMath.java, which says how the bytes lie). Included by
// src/worldgen/jmath.cpp only.

namespace proto::gen::worldgen::math {

inline constexpr unsigned char kCorrections[] = {
#embed "worldgen_math.bin"
};

}  // namespace proto::gen::worldgen::math
"""
    ported = sum(1 for at in range(t.named) if t.placed[at][3])
    where = sum(1 for at in range(t.named) if t.placed[at][4])
    # For the golden script (WorldgenFeatures.decorations): which features of the biomes src/worldgen
    # places as the tables stand, so that vanilla's side of a whole decoration is made of the same.
    (vdir(version) / "worldgen" / "ported_features.txt").write_text("".join(f"{t.placed_names[at]}\n" for at in range(t.named) if t.placed[at][3]))
    return {"worldgen_features.hpp": "".join(out), "worldgen_state_sets.bin": t.sets.bytes() or b"\0",
            "worldgen_math.hpp": math_hpp, "worldgen_math.bin": math,
            "_stats": {"placed_features": len(t.placed), "placed_features_named": t.named, "placed_features_ported": ported, "placed_features_whose_placements_are": where,
                       "features": len(t.features.rows), "feature_types_waiting": len(t.unported_types),
                       "placements": len(t.modifiers.rows), "predicates": len(t.predicates.rows), "state_sets": len(t.sets.kept)}}
