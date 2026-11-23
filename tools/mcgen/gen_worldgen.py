# SPDX-License-Identifier: GPL-3.0-or-later
"""The tables of src/worldgen: what the overworld's generator is made of.

Two sources, both vanilla's: the JSON under data/minecraft/worldgen/ in the server jar (what a
world is made of) and what `mcgen.py worldgen` extracted (what vanilla made of it once loaded -
tools/mcgen/extract/WorldgenRef.java). Where both say the same thing they are compared, and a
difference stops the run: the port relies on how the two fit together.

  noises     every noise of the overworld as the layers vanilla stacks it from: a frequency and an
             amplitude each, and the names its random sources are made from. The numbers a seed
             draws - permutations, offsets - are drawn at run time (worldgen/noise.cpp).
"""
from __future__ import annotations

import hashlib
import json
import re
import struct

from mcgen import die

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
        have = set(node) - {"c", "name"}
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


def emit(hdr: str, version: str, vanilla) -> dict:
    """The generated files, by name - and "_stats"."""
    settings = json.loads(vanilla.zip.read("data/minecraft/worldgen/noise_settings/overworld.json"))
    if settings.get("legacy_random_source") is not False:
        die("gen_worldgen: the overworld draws from the legacy random source: src/worldgen takes it for Xoroshiro")
    program = Program(version)
    noises, layers, blended, _ = noise_tables(vanilla, program)

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
    out.append("};\n\n}  // namespace proto::gen::worldgen\n")
    return {"worldgen.hpp": "".join(out), "_stats": {"noises": len(noises), "noise_layers": len(layers) + len(blended)}}
