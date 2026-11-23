# SPDX-License-Identifier: GPL-3.0-or-later
"""The tables of src/worldgen: what the overworld's generator is made of.

Two sources, both vanilla's: the JSON under data/minecraft/worldgen/ in the server jar (what a
world is made of) and what `mcgen.py worldgen` extracted (what vanilla made of it once loaded -
tools/mcgen/extract/WorldgenRef.java). Where both say the same thing they are compared, and a
difference stops the run: the port relies on how the two fit together.

  noises     every noise of the overworld as the layers vanilla stacks it from: a frequency and an
             amplitude each, and the names its random sources are made from. The numbers a seed
             draws - permutations, offsets - are drawn at run time (worldgen/noise.cpp).
  program    the overworld's density functions as vanilla compiled them: one node per sampler
             object, a child before its parents, with the splines, the lists of an interval
             select and the roots - the functions a generator asks for by name (worldgen/density.cpp).
  climate    the tree the overworld's biomes are searched in by climate, as vanilla built it
             (worldgen/climate.cpp).
  settings   the overworld's noise settings: its height, its sea, its stone and its water.
  heightmaps which of vanilla's six heightmaps a block state counts for (worldgen_heightmaps.bin).
"""
from __future__ import annotations

import hashlib
import json
import re
import struct

from mcgen import die, vdir

import worldgen

INPUT_FACTOR = 1.0181268882175227  # NormalNoise.INPUT_FACTOR: the second generator of an octave runs this much faster


def f32(x: float) -> float:
    return struct.unpack("<f", struct.pack("<f", x))[0]


def cdouble(x: float) -> str:
    """A double as a hexadecimal literal: every bit of it."""
    return float(x).hex()


def cfloat(x: float) -> str:
    """A float - which the extractor wrote as the double it widens to - as a hexadecimal literal."""
    if f32(x) != x:
        die(f"gen_worldgen: {x!r} is not a float")
    return float(x).hex() + "f"


def md5_seed(name: str) -> tuple[int, int]:
    """RandomSupport.seedFromHashOf: the two halves of the name's MD5, as big-endian longs."""
    digest = hashlib.md5(name.encode("utf-8")).digest()
    return int.from_bytes(digest[:8], "big"), int.from_bytes(digest[8:], "big")


def chex(v: int) -> str:
    return f"0x{v:016x}ull"


def ident(name: str) -> str:
    """'minecraft:cave_cheese' -> 'kCaveCheese' (as gen.ident)."""
    parts = [p for p in re.split(r"[^0-9a-zA-Z]+", name.split(":", 1)[-1]) if p]
    return "k" + "".join(p[:1].upper() + p[1:] for p in parts)


class Program:
    """program.json: a graph of the objects vanilla compiled, by node index."""

    def __init__(self, version: str):
        root = worldgen.load(version, "program")
        self.nodes, self.roots, self.noises, self.schemas = root["nodes"], root["roots"], root["noises"], root["schemas"]

    def node(self, ref) -> dict:
        if not (isinstance(ref, dict) and "#" in ref):
            die(f"gen_worldgen: {ref!r} is not a reference to a node")
        return self.nodes[ref["#"]]

    def need(self, node: dict, cls: str, *fields: str) -> dict:
        """`node`, which must be of class `cls` and made of exactly `fields`."""
        if node["c"] != cls:
            die(f"gen_worldgen: expected a {cls}, the extractor found a {node['c']}: vanilla changed, read the class again")
        have = set(node) - {"c", "@name"}
        if have != set(fields):
            die(f"gen_worldgen: {cls} has fields {sorted(have)}, the generator knows {sorted(fields)}")
        return node


def noise_tables(vanilla, program: Program) -> tuple[list, list, list, dict]:
    """(noises, their layers, the blended noise's layers, name -> index)."""
    noises, layers, index = [], [], {}
    names = sorted(n for n in program.noises if not n.startswith("blended:"))
    for name in names:
        stack = program.need(program.node(program.noises[name]), "NoiseStack$Perlin", "layers", "range")
        path = name.split(":", 1)[1]
        params = json.loads(vanilla.zip.read(f"data/minecraft/worldgen/noise/{path}.json"))
        base, count = params["base_octave"], params.get("octave_count", 1)
        modifiers = params.get("amplitude_modifiers") or [1.0] * count
        if len(modifiers) != count:
            die(f"gen_worldgen: noise {name}: {len(modifiers)} amplitude modifiers for {count} octaves")
        octaves = [base + i for i in range(count) if modifiers[i] != 0]
        made = [program.need(program.node(l), "NoiseStack$Layer", "noise", "frequency", "amplitude") for l in stack["layers"]]
        # NormalNoise.create: an octave is two PerlinNoise of one amplitude, the second a little faster.
        if len(made) != 2 * len(octaves):
            die(f"gen_worldgen: noise {name}: vanilla stacked {len(made)} layers, its JSON gives {len(octaves)} octaves")
        first = len(layers)
        for k, octave in enumerate(octaves):
            a, b = made[2 * k], made[2 * k + 1]
            frequency = 2.0 ** octave
            if a["frequency"] != frequency or b["frequency"] != frequency * INPUT_FACTOR or a["amplitude"] != b["amplitude"]:
                die(f"gen_worldgen: noise {name}, octave {octave}: its two layers are not what NormalNoise.create makes")
            for second, layer in enumerate((a, b)):
                program.need(program.node(layer["noise"]), "PerlinNoise", "perms", "offsetX", "offsetY", "offsetZ")
                layers.append((layer["frequency"], layer["amplitude"], octave, second))
        index[name] = len(noises)
        noises.append((name, first, 2 * len(octaves)))
    # old_blended_noise (BlendedNoise.createFbmSet): three stacks, their layers made one after the
    # other from one random source - min limit, max limit, main.
    blended = []
    for name, want in (("blended:min_limit", 16), ("blended:max_limit", 16), ("blended:main", 8)):
        stack = program.need(program.node(program.noises[name]), "NoiseStack$SmearedPerlin", "layers", "range")
        if len(stack["layers"]) != want:
            die(f"gen_worldgen: {name} has {len(stack['layers'])} layers, expected {want}")
        for l in stack["layers"]:
            layer = program.need(program.node(l), "NoiseStack$Layer", "noise", "frequency", "amplitude")
            smeared = program.need(program.node(layer["noise"]), "SmearedPerlinNoise", "perms", "offsetX", "offsetY", "offsetZ", "fudgeYScale")
            blended.append((layer["frequency"], layer["amplitude"], smeared["fudgeYScale"]))
    return noises, layers, blended, index


# A compiled sampler class -> (the node's op, where each of its fields goes in the node). A field
# that names another sampler goes into `a`, `b` or `c`; the rest into the slot of its type. A class
# that is not here is one src/worldgen has no word for: the generator stops at it.
SAMPLERS = {
    "ConstantFunction$Sampler": ("kConstant", {"value": "f0"}),
    "BinaryFunction$AddSampler": ("kAdd", {"left": "a", "right": "b"}),
    "BinaryFunction$SubSampler": ("kSub", {"left": "a", "right": "b"}),
    "BinaryFunction$MulSampler": ("kMul", {"left": "a", "right": "b"}),
    "BinaryFunction$DivSampler": ("kDiv", {"left": "a", "right": "b"}),
    "BinaryFunction$MinSampler": ("kMin", {"left": "a", "right": "b", "rightMinValue": "f0"}),
    "BinaryFunction$MaxSampler": ("kMax", {"left": "a", "right": "b", "rightMaxValue": "f0"}),
    "BinaryFunction$ConstAddSampler": ("kConstAdd", {"left": "a", "right": "f0"}),
    "BinaryFunction$ConstMulSampler": ("kConstMul", {"left": "a", "right": "f0"}),
    "BinaryFunction$ConstMinSampler": ("kConstMin", {"left": "a", "right": "f0"}),
    "BinaryFunction$ConstMaxSampler": ("kConstMax", {"left": "a", "right": "f0"}),
    "BinaryFunction$ConstSubSampler": ("kConstSub", {"left": "f0", "right": "a"}),
    "BinaryFunction$ConstDivSampler": ("kConstDiv", {"left": "f0", "right": "a"}),
    "UnaryFunction$AbsSampler": ("kAbs", {"input": "a"}),
    "UnaryFunction$CubeSampler": ("kCube", {"input": "a"}),
    "UnaryFunction$NegateSampler": ("kNegate", {"input": "a"}),
    "UnaryFunction$SquareSampler": ("kSquare", {"input": "a"}),
    "UnaryFunction$SqueezeSampler": ("kSqueeze", {"input": "a"}),
    "UnaryFunction$LeakyReLUSampler": ("kLeakyRelu", {"input": "a", "negativeFactor": "f0"}),
    "ClampFunction$Sampler": ("kClamp", {"input": "a", "min": "f0", "max": "f1"}),
    "RangeChoiceFunction$Sampler": ("kRangeChoice", {"input": "a", "whenInRange": "b", "whenOutOfRange": "c",
                                                     "minInclusive": "f0", "maxExclusive": "f1"}),
    "RangeChoiceFunction$ConstSampler": ("kRangeChoiceConst", {"input": "a", "minInclusive": "f0", "maxExclusive": "f1",
                                                               "whenInRange": "f2", "whenOutOfRange": "f3"}),
    "LerpFunction$Sampler": ("kLerp", {"alpha": "a", "first": "b", "second": "c"}),
    "LerpFunction$ConstFirstSampler": ("kLerpConstFirst", {"alpha": "a", "first": "f0", "second": "b"}),
    "SliceFunction$YSampler": ("kSliceY", {"input": "a", "y": "i0"}),
    "SliceFunction$XzSampler": ("kSliceXz", {"input": "a", "x": "i0", "z": "i1"}),
    "GradientFunction$ClampedSampler": ("kGradient", {"axis": "axis", "fromCoordinate": "i0", "minCoordinate": "i1", "maxCoordinate": "i2",
                                                      "fromValue": "f0", "coordinateFactor": "f1"}),
    "NoiseFunction$Sampler": ("kNoise", {"noise": "noise", "xzScale": "d0", "yScale": "d1"}),
    "NoiseFunction$ShiftedXzSampler": ("kShiftedXzNoise", {"shiftX": "a", "shiftZ": "b", "noise": "noise", "xzScale": "d0", "yScale": "d1"}),
    "ShiftNoiseFunction$ShiftB$1": ("kShiftB", {"val$noise": "noise"}),
    "InterpolatedFunction$Sampler": ("kInterpolated", {"input": "a", "cellSizeXz": "i0", "cellSizeY": "i1",
                                                       "cellSizeXzInv": "f0", "cellSizeYInv": "f1"}),
    "FindTopSurfaceFunction$Sampler": ("kFindTopSurface", {"density": "a", "upperBound": "b", "lowerBound": "i0", "cellHeight": "i1"}),
    "BlendDensityFunction$Sampler": ("kBlendDensity", {"input": "a"}),
    "CachingDensitySampler": ("kCache", {"input": "a", "id": "i0"}),
    "ContextBoundSampler": ("kContextBound", {"fallbackSampler": "a", "key": "key"}),
    "IntervalSelectFunction$Sampler": ("kIntervalSelect", {"input": "a", "thresholds": "thresholds", "samplers": "samplers"}),
    "SplineFunction$Sampler": ("kSpline", {"sampler": "spline", "coordinateCount": "count"}),
}
OPS = sorted({op for op, _ in SAMPLERS.values()})
CONTEXT_KEYS = {"minecraft:blender/alpha": 0, "minecraft:blender/offset": 1, "minecraft:beardifier": 2}
AXES = {"X": 0, "Y": 1, "Z": 2}


class ProgramTables:
    """The compiled samplers as a flat table: a node per sampler object, children first."""

    def __init__(self, program: Program, noise_index: dict):
        self.p, self.noise_index = program, noise_index
        self.nodes: list[dict] = []          # the emitted nodes, by index
        self.index: dict[int, int] = {}      # dump node -> emitted node
        self.thresholds: list[float] = []    # interval selects: their thresholds...
        self.select: list[int] = []          # ...and the samplers they choose between
        self.coordinates: list[int] = []     # splines: the samplers of their coordinates
        self.splines: list[tuple] = []       # (coordinate, first point, points)
        self.points: list[tuple] = []        # (location, derivative, value, child spline or -1)
        self.cells = 0
        self.roots = [(name, self.sampler(ref)) for name, ref in program.roots.items()]

    def noise(self, ref) -> int:
        """A noise by what the extractor called it: one of the registry, or one of the blended three."""
        name = self.p.node(ref).get("@name")
        blended = {"blended:min_limit": 0, "blended:max_limit": 1, "blended:main": 2}
        if name in blended:
            return len(self.noise_index) + blended[name]
        if name not in self.noise_index:
            die(f"gen_worldgen: a sampler holds a noise the extractor did not name ({self.p.node(ref)['c']})")
        return self.noise_index[name]

    def sampler(self, ref) -> int:
        at = ref["#"]
        if at in self.index:
            return self.index[at]
        node = self.p.node(ref)
        cls = node["c"]
        if cls not in SAMPLERS:
            die(f"gen_worldgen: {cls}: a compiled sampler src/worldgen has no word for. Read the class "
                f"(mcgen.py pseudo), port it (worldgen/density.cpp) and enter it in SAMPLERS")
        op, fields = SAMPLERS[cls]
        self.p.need(node, cls, *fields)
        out = {"op": op}
        for field, slot in fields.items():
            v = node[field]
            if slot in ("a", "b", "c"):
                out[slot] = self.sampler(v)
            elif slot == "noise":
                out["noise"] = self.noise(v)
            elif slot == "axis":
                out["axis"] = AXES[v]
            elif slot == "key":
                key = self.p.need(self.p.node(v), "ContextKey", "name")["name"]
                if key not in CONTEXT_KEYS:
                    die(f"gen_worldgen: a sampler bound to the context key {key}, which src/worldgen does not know")
                out["i0"] = CONTEXT_KEYS[key]
            elif slot == "thresholds":
                out["list"], out["count"] = len(self.select), len(node["samplers"])
                if len(v) != len(node["samplers"]) - 1:
                    die("gen_worldgen: an interval select with other than one threshold between two samplers")
                chosen = [self.sampler(s) for s in node["samplers"]]
                out["list"] = len(self.select)   # (its samplers may hold selects of their own)
                self.select += chosen
                self.thresholds += v + [0.0]     # one a sampler: the last has none
            elif slot == "samplers":
                pass
            elif slot == "spline":
                # CubicSpline.asSampler: an anonymous class around the spline's top.
                top = self.p.need(self.p.node(v), "CubicSpline$1", "val$multipoint")
                coordinates: dict[int, int] = {}
                root = self.spline(top["val$multipoint"], coordinates)
                if sorted(coordinates) != list(range(node["coordinateCount"])):
                    die("gen_worldgen: a spline's coordinates are not numbered 0..n-1")
                out["i0"], out["list"] = len(self.coordinates), root
                self.coordinates += [coordinates[i] for i in range(len(coordinates))]
            elif slot == "count":
                out["count"] = v
            else:
                out[slot] = v
        if op == "kCache":
            self.cells = max(self.cells, out["i0"] + 1)
        self.index[at] = len(self.nodes)
        self.nodes.append(out)
        return self.index[at]

    def spline(self, ref, coordinates: dict[int, int]) -> int:
        """A CubicSpline.Multipoint, its points' own splines first; `coordinates` gathers index -> sampler."""
        node = self.p.need(self.p.node(ref), "CubicSpline$Multipoint", "coordinate", "locations", "values", "derivatives")
        coordinate = self.p.need(self.p.node(node["coordinate"]), "SplineFunction$SamplerCoordinate", "sampler", "index", "range")
        sampler = self.sampler(coordinate["sampler"])
        if coordinates.setdefault(coordinate["index"], sampler) != sampler:
            die("gen_worldgen: two samplers for one coordinate of a spline")
        if not (len(node["locations"]) == len(node["values"]) == len(node["derivatives"])):
            die("gen_worldgen: a spline whose points do not line up")
        points = []
        for location, value, derivative in zip(node["locations"], node["values"], node["derivatives"]):
            v = self.p.node(value)
            if v["c"] == "CubicSpline$Constant":
                points.append((location, derivative, self.p.need(v, "CubicSpline$Constant", "value")["value"], -1))
            else:
                points.append((location, derivative, 0.0, self.spline(value, coordinates)))
        first = len(self.points)
        self.points += points
        self.splines.append((coordinate["index"], first, len(points)))
        return len(self.splines) - 1


def emit_program(t: ProgramTables) -> str:
    out = ["""
// ---------------------------------------------------------------------------------------------
// The density functions, as vanilla compiled them (RandomState.getSampler): a node is a sampler
// object, and comes after the nodes it reads. What a field means is the op's to say
// (worldgen/density.cpp, which names the vanilla class of each).
enum class Op : u8 {
"""]
    out += [f"    {op},\n" for op in OPS]
    out.append("""};
struct Node {
    Op op;
    u8 axis;          // a gradient's: 0 x, 1 y, 2 z
    u16 a, b, c;      // the samplers it reads
    u16 noise;        // kNoises[noise], or - from noise::kCount on - old_blended_noise's min limit, max limit, main
    u16 list, count;  // an interval select's samplers (kSelect) and thresholds; a spline's top (kSplines), its coordinates
    i32 i0, i1, i2;
    float f0, f1, f2, f3;
    double d0, d1;
};
// A CubicSpline.Multipoint: `coordinate` is which of its function's coordinates it is a curve of.
struct Spline {
    u16 coordinate, first_point, points;
};
// ...and a point of one: where, how steep, and how high - a number, or what another spline says.
struct SplinePoint {
    float location, derivative, value;
    i32 spline;  // -1: `value`
};
""")
    out.append(f"\ninline constexpr u32 kCacheCells = {t.cells};  // CachingDensitySampler ids are 0..kCacheCells-1\n")
    out.append(f"\ninline constexpr Node kNodes[{len(t.nodes)}] = {{\n")
    for i, n in enumerate(t.nodes):
        g = lambda k, d=0: n.get(k, d)
        out.append(f"    {{Op::{n['op']}, {g('axis')}, {g('a')}, {g('b')}, {g('c')}, {g('noise')}, {g('list')}, {g('count')}, "
                   f"{g('i0')}, {g('i1')}, {g('i2')}, {cfloat(g('f0', 0.0))}, {cfloat(g('f1', 0.0))}, {cfloat(g('f2', 0.0))}, "
                   f"{cfloat(g('f3', 0.0))}, {cdouble(g('d0', 0.0))}, {cdouble(g('d1', 0.0))}}},  // {i}\n")
    out.append("};\n")
    out.append(f"\ninline constexpr u16 kSelect[{max(1, len(t.select))}] = {{{', '.join(map(str, t.select)) or '0'}}};\n")
    out.append(f"inline constexpr float kSelectThresholds[{max(1, len(t.thresholds))}] = "
               f"{{{', '.join(cfloat(x) for x in t.thresholds) or '0.0f'}}};\n")
    out.append(f"inline constexpr u16 kSplineCoordinates[{max(1, len(t.coordinates))}] = "
               f"{{{', '.join(map(str, t.coordinates)) or '0'}}};\n")
    out.append(f"\ninline constexpr Spline kSplines[{len(t.splines)}] = {{\n")
    out += [f"    {{{c}, {first}, {n}}},\n" for c, first, n in t.splines]
    out.append("};\n")
    out.append(f"\ninline constexpr SplinePoint kSplinePoints[{len(t.points)}] = {{\n")
    out += [f"    {{{cfloat(l)}, {cfloat(d)}, {cfloat(v)}, {child}}},\n" for l, d, v, child in t.points]
    out.append("};\n")
    out.append("""
// The functions a generator asks for (NoiseRouter, Aquifer.Config, the spawn target, the ore
// veins of the material rules), by where vanilla names them.
struct Root {
    std::string_view name;
    u16 node;
};
""")
    out.append(f"inline constexpr Root kRoots[{len(t.roots)}] = {{\n")
    out += [f'    {{"{name}", {node}}},\n' for name, node in t.roots]
    out.append("};\nnamespace root {\n")
    seen = set()
    for name, node in t.roots:
        const = ident(re.sub(r"([a-z])([A-Z])", r"\1_\2", name.replace("minecraft:", "")))
        if const in seen:
            die(f"gen_worldgen: two roots named {const}")
        seen.add(const)
        out.append(f"inline constexpr u32 {const} = {node};  // {name}\n")
    out.append("}  // namespace root\n")
    return "".join(out)


def climate_table(version: str, vanilla) -> tuple[list, list]:
    """The overworld's biome search tree (Climate.RTree) as nodes, a node's children side by side:
    (nodes, the names of the biomes its leaves give). A node is (mins[7], maxs[7], first child or
    the leaf's biome, children)."""
    root = worldgen.load(version, "climate")
    graph = root["nodes"]

    def node(ref, cls, *fields):
        n = graph[ref["#"]]
        if n["c"] != cls or set(n) - {"c"} != set(fields):
            die(f"gen_worldgen: climate: expected a {cls} of {sorted(fields)}, found a {n['c']} of {sorted(set(n) - {'c'})}")
        return n

    listed = node(root["list"], "Climate$ParameterList", "values", "index")
    tree = node(listed["index"], "Climate$RTree", "root", "lastResult")
    biome_ids = {name: i for i, name in enumerate(vanilla.entries("worldgen/biome"))}  # the registry as clients are sent it
    nodes: list = [None]
    used: set = set()

    def space(n) -> tuple[list, list]:
        ps = [node(p, "Climate$Parameter", "min", "max") for p in n["parameterSpace"]]
        if len(ps) != 7:  # Climate.PARAMETER_COUNT: six of the climate and the offset
            die("gen_worldgen: climate: a node's parameter space is not seven parameters")
        if any(not -32768 <= v <= 32767 for p in ps for v in (p["min"], p["max"])):
            die("gen_worldgen: climate: a parameter beyond 16 bits (they are fractions of -2..2 times 10000)")
        return [p["min"] for p in ps], [p["max"] for p in ps]

    def fill(at: int, ref) -> None:
        n = graph[ref["#"]]
        if n["c"] == "Climate$RTree$Leaf":
            leaf = node(ref, "Climate$RTree$Leaf", "parameterSpace", "value")
            name = leaf["value"].get("holder")
            if name not in biome_ids:
                die(f"gen_worldgen: climate: a leaf gives {leaf['value']!r}, which is no biome of the registry")
            used.add(name)
            nodes[at] = (*space(leaf), biome_ids[name], 0)
            return
        sub = node(ref, "Climate$RTree$SubTree", "parameterSpace", "children")
        first = len(nodes)
        nodes.extend([None] * len(sub["children"]))
        nodes[at] = (*space(sub), first, len(sub["children"]))
        for i, child in enumerate(sub["children"]):
            fill(first + i, child)

    fill(0, tree["root"])
    if len(nodes) > 65535:
        die("gen_worldgen: climate: more nodes than 16 bits count")
    if sum(1 for n in nodes if n[3] == 0) != len(listed["values"]):
        die("gen_worldgen: climate: the tree does not have one leaf a parameter point")
    return nodes, sorted(used)


def emit_climate(hdr: str, nodes: list) -> str:
    out = [hdr, """
// The overworld's biomes by climate: the R-tree vanilla searches (Climate.RTree, built over
// MultiNoiseBiomeSourceParameterList.Preset.OVERWORLD - which is code, not data), node by node as
// vanilla built it. A node's children lie side by side, in the order the search visits them: who
// comes first wins a tie. Included by worldgen/climate.cpp only.

#include "base/types.hpp"

namespace proto::gen::worldgen {

// The box of a node in the seven parameters - temperature, humidity, continentalness, erosion,
// depth, weirdness, offset - in ten-thousandths (Climate.quantizeCoord).
struct ClimateNode {
    i16 min[7], max[7];
    u16 first;     // its first child; a leaf's biome (the network id)
    u16 children;  // 0: a leaf
};

"""]
    out.append(f"inline constexpr ClimateNode kClimate[{len(nodes)}] = {{\n")
    for mins, maxs, first, children in nodes:
        out.append(f"    {{{{{', '.join(map(str, mins))}}}, {{{', '.join(map(str, maxs))}}}, {first}, {children}}},\n")
    out.append("};\n\n}  // namespace proto::gen::worldgen\n")
    return "".join(out)


class Blocks:
    """Block states by how vanilla's data names them (reports/blocks.json)."""

    def __init__(self, version: str):
        path = vdir(version) / "reports" / "blocks.json"
        if not path.exists():
            die("gen_worldgen: reports/blocks.json is missing - run: mcgen.py reports")
        self.blocks = json.loads(path.read_text())

    def state(self, value) -> int:
        """BlockState.CODEC: a name (the block's default state), or {id, properties}."""
        name, properties = (value, {}) if isinstance(value, str) else (value.get("id") or value.get("Name"), value.get("properties") or value.get("Properties") or {})
        block = self.blocks.get(name)
        if block is None:
            die(f"gen_worldgen: no block {name!r}")
        default = next(s for s in block["states"] if s.get("default"))
        want = {**default.get("properties", {}), **properties}
        for s in block["states"]:
            if s.get("properties", {}) == want:
                return s["id"]
        die(f"gen_worldgen: {name} has no state {properties}")


def emit_settings(settings: dict, blocks: Blocks) -> str:
    """noise_settings/overworld.json: what the whole generator goes by."""
    noise = settings["noise"]
    if set(noise) != {"min_y", "height"}:
        die(f"gen_worldgen: noise settings of {sorted(noise)}: the generator knows min_y and height")
    if "aquifers" not in settings:
        die("gen_worldgen: the overworld has no aquifers: src/worldgen has only the noise-based kind")
    return f"""
// ---------------------------------------------------------------------------------------------
// noise_settings/overworld.json
inline constexpr i32 kMinY = {noise['min_y']}, kHeight = {noise['height']};
inline constexpr i32 kSeaLevel = {settings['sea_level']};
inline constexpr u16 kDefaultBlock = {blocks.state(settings['default_block'])};  // {settings['default_block']}
inline constexpr u16 kDefaultFluid = {blocks.state(settings['default_fluid'])};  // {settings['default_fluid']}
inline constexpr u16 kLava = {blocks.state('minecraft:lava')};  // (NoiseBasedChunkGenerator.createFluidPicker: below y -54)

// Which of vanilla's heightmaps a block state counts for: a bit a heightmap, by Heightmap.Types' ordinal.
inline constexpr u8 kWorldSurfaceWg = 1, kWorldSurface = 2, kOceanFloorWg = 4, kOceanFloor = 8, kMotionBlocking = 16,
                    kMotionBlockingNoLeaves = 32;
inline constexpr unsigned char kHeightmaps[] = {{
#embed "worldgen_heightmaps.bin"
}};
"""


def heightmap_bits(version: str) -> bytes:
    """A byte a block state: which of vanilla's heightmaps it counts for (Heightmap.Types, by ordinal)."""
    import gamedata
    states = gamedata.load(version, "states")
    want = ["WORLD_SURFACE_WG", "WORLD_SURFACE", "OCEAN_FLOOR_WG", "OCEAN_FLOOR", "MOTION_BLOCKING", "MOTION_BLOCKING_NO_LEAVES"]
    if states["legend"].get("heightmap_opaque") != want:
        die("gen_worldgen: gamedata/states.json: heightmap_opaque is not the six heightmaps in the order of Heightmap.Types")
    column = states["columns"]["heightmap_opaque"]
    if len(column) != states["state_count"] or max(column) > 63:
        die("gen_worldgen: gamedata/states.json: heightmap_opaque is not six bits a state")
    return bytes(column)


def emit(hdr: str, version: str, vanilla) -> dict:
    """The generated files, by name - and "_stats"."""
    settings = json.loads(vanilla.zip.read("data/minecraft/worldgen/noise_settings/overworld.json"))
    if settings.get("legacy_random_source") is not False:
        die("gen_worldgen: the overworld draws from the legacy random source: src/worldgen takes it for Xoroshiro")
    program = Program(version)
    noises, layers, blended, noise_index = noise_tables(vanilla, program)
    tables = ProgramTables(program, noise_index)

    out = [hdr, """
// The tables of src/worldgen: the overworld's generator. Made of data/minecraft/worldgen/ in the
// jar and of what vanilla built from it once loaded (mcgen.py worldgen): tools/mcgen/gen_worldgen.py.

#include <string_view>

#include "base/types.hpp"

namespace proto::gen::worldgen {

// RandomSupport.seedFromHashOf of a name: what a positional random factory makes a source from.
struct Hash {
    u64 lo, hi;
};

// A layer of a noise (NoiseStack.Layer): a PerlinNoise seeded by "octave_<octave>" from the first
// or the second factory of the noise (NormalNoise.create), sampled at `frequency`, times `amplitude`.
struct NoiseLayer {
    double frequency;
    float amplitude;
    i8 octave;
    u8 second;
};
// A noise of the registry: its layers, in the order vanilla adds them up.
struct Noise {
    std::string_view name;
    Hash hash;
    u16 first_layer, layers;
};
// A layer of old_blended_noise: a SmearedPerlinNoise.
struct BlendedLayer {
    double frequency;
    float amplitude;
    double fudge_y_scale;
};
"""]
    octaves = sorted({l[2] for l in layers})
    lo, hi = octaves[0], octaves[-1]
    out.append(f"\ninline constexpr i32 kFirstOctave = {lo};  // kOctaveHash[octave - kFirstOctave]\n")
    out.append(f"inline constexpr Hash kOctaveHash[{hi - lo + 1}] = {{\n")
    for octave in range(lo, hi + 1):
        a, b = md5_seed(f"octave_{octave}")
        out.append(f"    {{{chex(a)}, {chex(b)}}},  // octave_{octave}\n")
    out.append("};\n")
    out.append(f"\ninline constexpr NoiseLayer kNoiseLayers[{len(layers)}] = {{\n")
    for frequency, amplitude, octave, second in layers:
        out.append(f"    {{{cdouble(frequency)}, {cfloat(amplitude)}, {octave}, {second}}},\n")
    out.append("};\n")
    out.append(f"\ninline constexpr Noise kNoises[{len(noises)}] = {{\n")
    for name, first, count in noises:
        a, b = md5_seed(name)
        out.append(f'    {{"{name}", {{{chex(a)}, {chex(b)}}}, {first}, {count}}},\n')
    out.append("};\n\nnamespace noise {\n")
    for i, (name, _, _) in enumerate(noises):
        out.append(f"inline constexpr u32 {ident(name)} = {i};\n")
    out.append(f"inline constexpr u32 kCount = {len(noises)};\n}}  // namespace noise\n")
    a, b = md5_seed("minecraft:terrain")  # BlendedNoise.NOISE_SEED
    out.append(f"""
// old_blended_noise: the layers of its three stacks - min limit, max limit, main - in the order
// they are made, all from the one source of "minecraft:terrain".
inline constexpr Hash kBlendedHash = {{{chex(a)}, {chex(b)}}};
inline constexpr u32 kBlendedMinLimit = 16, kBlendedMaxLimit = 16, kBlendedMain = 8;
inline constexpr BlendedLayer kBlendedLayers[{len(blended)}] = {{
""")
    for frequency, amplitude, fudge in blended:
        out.append(f"    {{{cdouble(frequency)}, {cfloat(amplitude)}, {cdouble(fudge)}}},\n")
    out.append("};\n")
    out.append(emit_program(tables))
    out.append(emit_settings(settings, Blocks(version)))
    out.append("\n}  // namespace proto::gen::worldgen\n")
    climate, biomes = climate_table(version, vanilla)
    return {"worldgen.hpp": "".join(out), "worldgen_climate.hpp": emit_climate(hdr, climate),
            "worldgen_heightmaps.bin": heightmap_bits(version),
            "_stats": {"noises": len(noises), "noise_layers": len(layers) + len(blended), "density_nodes": len(tables.nodes),
                       "climate_nodes": len(climate), "climate_biomes": len(biomes)}}
