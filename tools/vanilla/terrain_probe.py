#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Is the terrain this server makes the terrain a vanilla server makes of the same seed?

    terrain_probe.py vanilla <out.json>          # runs vanilla in a world of this probe's own; what it saved
    terrain_probe.py ours <exe> <out.json>       # the same chunks as the generator here makes them
    terrain_probe.py diff <vanilla.json> <ours.json>
    tools/vanilla/compare.py terrain_probe       # all three

The unit tests hold every step of the generator to vanilla's own classes, called one by one
(`mcgen.py worldgen golden`). This holds the whole of it to a vanilla *server*: the chunks a
running server made on its worker threads, each with whatever its neighbours and its thread had
done before, and wrote to its region files.

What is compared is not what a client is sent. In a chunk a client sees the water has begun to
run - in an order that is the server's business. But a server makes more chunks than it shows:
around what a player sees lie rings of chunks that got only as far as some earlier status, and
those are saved as they stand. So:

  * every chunk vanilla saved with its biomes (status `biomes` or later): the biome of each cell;
  * every chunk it saved with its terrain and before it was a chunk of the world (`terrain` up to
    `spawn`) - density, aquifers, surface, carvers, and nothing run in it yet: its blocks, the
    blocks marked to be let flow, in vanilla's order, and the two heightmaps generation goes by
    where the chunk still has them.

`vanilla` writes a world by hand - a level.dat and a world_gen_settings.dat that say: this seed,
the overworld's noise generator, no structures (they would press their shapes into the terrain) -
and a data pack in it that is every biome of the jar with its features taken out (the trees and
ores of a chunk reach into the chunks beside it, also into one that is itself still bare terrain;
carvers and all else stay). It starts the reference server on it (under the rules of AGENTS.md: from .cache/vanilla-server/, the
cached jar, Java 25, `--world terrain-probe`: server.properties stays as it is), walks a bot to a
few places far apart, and stops the server. The world is removed and made anew every time.

`ours` asks the generator itself, not a server: build/<preset>/terrain_dump, a few lines over
src/worldgen that write a chunk as the steps of `terrain` leave it (tools/vanilla/terrain_dump.cpp;
this builds it if it is not there). It makes every chunk near the places the bot went to; `diff`
takes those vanilla saved.

One thing can differ and be no fault: where two biomes are *exactly* as near a climate, vanilla
gives the one its thread found last, which depends on what that thread did before - another run
of the same server may give the other. `diff` counts such cells apart (`ours` says of each cell
whether it is one) and they do not fail it.
"""
from __future__ import annotations

import base64
import gzip
import json
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "integration"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import mcproto as mc  # noqa: E402
from entity_probe import Bot  # noqa: E402
from world_probe import PORT, SERVER_DIR, Vanilla, fresh_name, raw_pump  # noqa: E402

WORLD = "terrain-probe"
SEED = 20261123
# Where the bot stands, in chunks: the first is the world's spawn. Far enough apart that what is
# loaded around one is not what is loaded around another.
STOPS = [(0, 0), (60, -45), (-70, 30), (25, 90)]
REACH = 7           # chunks around a stop that `ours` makes: as far as vanilla gets to biomes for a view of two
# The statuses of a chunk whose terrain is made and in which nothing has ticked yet (ChunkStatus).
BARE = ("terrain", "features", "initialize_light", "light", "spawn")
MIN_Y, SECTIONS = -64, 24


# ----------------------------------------------------------------------------- vanilla's world

def named(tag: dict) -> bytes:
    """A tag as a file holds it: the root compound, with a name (the empty one)."""
    return b"\x0a\x00\x00" + mc.nbt(tag)[1:]


PACK = "no-features"


def write_pack(path: Path) -> int:
    """A data pack of the jar's own biomes, each without its features. Made of the jar every time,
    inside .cache, and gone with the world."""
    import zipfile
    jar = zipfile.ZipFile(ROOT / ".cache" / "mc" / mc.META["version"] / "server-inner.jar")
    data = json.loads(jar.read("version.json"))["pack_version"]["data_major"]
    prefix = "data/minecraft/worldgen/biome/"
    biomes = [n for n in jar.namelist() if n.startswith(prefix) and n.endswith(".json")]
    for name in biomes:
        biome = json.loads(jar.read(name))
        biome["features"] = []
        target = path / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(json.dumps(biome))
    (path / "pack.mcmeta").write_text(json.dumps({"pack": {"description": "terrain_probe.py: no features", "min_format": data, "max_format": data}}))
    return len(biomes)


def write_world(path: Path, seed: int) -> None:
    """A world vanilla has not run yet, as LevelStorageSource reads one (26.3: the dimensions and the
    seed are in data/minecraft/world_gen_settings.dat, the rest in level.dat)."""
    version = mc.META["world_version"]
    (path / "data" / "minecraft").mkdir(parents=True)
    write_pack(path / "datapacks" / PACK)
    x, z = STOPS[0][0] * 16 + 8, STOPS[0][1] * 16 + 8
    level = {"Data": {
        "DataPacks": {"Disabled": [], "Enabled": ["vanilla", f"file/{PACK}"]},
        "DataVersion": version, "GameType": 1, "LastPlayed": mc.Long(int(time.time() * 1000)), "LevelName": WORLD,
        "ServerBrands": ["vanilla"], "Time": mc.Long(0),
        "Version": {"Id": version, "Name": mc.META["version"], "Series": "main", "Snapshot": mc.Byte(0)},
        "WasModded": mc.Byte(0), "allowCommands": mc.Byte(0),
        "difficulty_settings": {"difficulty": "peaceful", "hardcore": mc.Byte(0), "locked": mc.Byte(0)},
        "initialized": mc.Byte(1),
        "spawn": {"dimension": "minecraft:overworld", "pitch": mc.Float(0.0), "pos": mc.IntArray((x, 200, z)), "yaw": mc.Float(0.0)},
        "version": 19133, "version_history": mc.IntArray((version,))}}
    (path / "level.dat").write_bytes(gzip.compress(named(level)))
    noise = lambda preset, settings, kind: {
        "generator": {"biome_source": {"preset": preset, "type": "minecraft:multi_noise"}, "settings": settings, "type": "minecraft:noise"},
        "type": kind}
    settings = {"DataVersion": version, "data": {
        "bonus_chest": mc.Byte(0), "generate_structures": mc.Byte(0), "seed": mc.Long(seed),
        "dimensions": {
            "minecraft:overworld": noise("minecraft:overworld", "minecraft:overworld", "minecraft:overworld"),
            "minecraft:the_nether": noise("minecraft:nether", "minecraft:nether", "minecraft:the_nether"),
            "minecraft:the_end": {"generator": {"biome_source": {"type": "minecraft:the_end"}, "settings": "minecraft:end",
                                                "type": "minecraft:noise"}, "type": "minecraft:the_end"}}}}
    (path / "data" / "minecraft" / "world_gen_settings.dat").write_bytes(gzip.compress(named(settings)))


def region_chunks(path: Path):
    """The chunks of a region file (RegionFile): their tags."""
    data = path.read_bytes()
    for i in range(1024):
        where = struct.unpack_from(">I", data, 4 * i)[0]
        if where == 0:
            continue
        at = (where >> 8) * 4096
        length, kind = struct.unpack_from(">IB", data, at)
        raw = data[at + 5:at + 4 + length]
        if kind == 2:
            raw = zlib.decompress(raw)
        elif kind == 1:
            raw = gzip.decompress(raw)
        elif kind != 3:
            raise SystemExit(f"{path}: a chunk compressed in a way this probe does not read ({kind})")
        name = struct.unpack_from(">H", raw, 1)[0]
        yield mc.Reader(raw[:1] + raw[3 + name:]).nbt()


def unpack(longs, bits: int, count: int) -> list[int]:
    """SimpleBitStorage: `count` values of `bits` bits, none across two longs."""
    per, mask, out = 64 // bits, (1 << bits) - 1, []
    for v in longs:
        v &= (1 << 64) - 1
        for k in range(per):
            out.append(v >> (k * bits) & mask)
            if len(out) == count:
                return out
    raise SystemExit("a packed array that is too short")


class States:
    """Block states by name and properties, as vanilla's own report lists them."""

    def __init__(self):
        path = ROOT / ".cache" / "mc" / mc.META["version"] / "reports" / "blocks.json"
        self.ids, self.defaults = {}, {}
        for name, block in json.loads(path.read_text()).items():
            for s in block["states"]:
                self.ids[(name, tuple(sorted(s.get("properties", {}).items())))] = s["id"]
                if s.get("default"):
                    self.defaults[name] = s.get("properties", {})

    def of(self, entry) -> int:
        """BlockState.CODEC: a name - the block's default state - or a name with the properties that are not the default's."""
        if isinstance(entry, str):
            name, given = entry, {}
        else:
            name = entry.get("Name") or entry.get("id")
            given = entry.get("Properties") or entry.get("properties") or {}
        properties = {**self.defaults[name], **{k: str(v) for k, v in given.items()}}
        return self.ids[(name, tuple(sorted(properties.items())))]


def read_chunk(tag: dict, states: States, biome_ids: dict) -> dict | None:
    """What a saved chunk says of itself, in the ids of this version (SerializableChunkData)."""
    status = tag["Status"].removeprefix("minecraft:")
    if status in ("empty", "structure_starts", "structure_references"):
        return None
    sections = {int(s["Y"]): s for s in tag["sections"]}
    out = {"status": status, "biomes": []}
    blocks = bytearray()
    for y in range(MIN_Y >> 4, (MIN_Y >> 4) + SECTIONS):
        s = sections.get(y, {})
        b = s.get("biomes")
        if b is None:
            return None
        palette = [biome_ids[name] for name in b["palette"]]
        cells = [0] * 64 if len(palette) == 1 else unpack(b["data"], max(1, (len(palette) - 1).bit_length()), 64)
        out["biomes"] += [palette[i] for i in cells]
        bs = s.get("block_states")
        if bs is None:
            blocks += bytes(8192)
            continue
        palette = [states.of(entry) for entry in bs["palette"]]
        cells = [0] * 4096 if len(palette) == 1 else unpack(bs["data"], max(4, (len(palette) - 1).bit_length()), 4096)
        blocks += struct.pack("<4096H", *(palette[i] for i in cells))
    if status not in BARE:
        return out
    out["blocks"] = base64.b64encode(zlib.compress(bytes(blocks), 1)).decode()
    for kind in ("WORLD_SURFACE_WG", "OCEAN_FLOOR_WG"):
        packed = tag.get("Heightmaps", {}).get(kind)
        out[kind] = [MIN_Y + v for v in unpack(packed, 9, 256)] if packed is not None else None
    marks = []
    for index, of in enumerate(tag.get("PostProcessing", [])):
        marks += [index << 12 | (int(m) & 0xFFF) for m in of]
    out["marks"] = marks
    return out


def record_vanilla(out: str, seed: int, name: str | None = None) -> None:
    world = SERVER_DIR / WORLD
    if world.exists():
        shutil.rmtree(world)
    log = Path(tempfile.gettempdir()) / "terrain_probe_vanilla.log"
    try:
        write_world(world, seed)
        v = Vanilla(WORLD, log)
        try:
            v.until(r"Done \(")
            bot = Bot(PORT, name or fresh_name("Tp"))
            bot.join()
            for cx, cz in STOPS:
                v.cmd(f"tp {bot.name} {cx * 16 + 8} 200 {cz * 16 + 8}")
                # Until the chunks of the view have come and nothing more does.
                seen, quiet, end = set(), 0, time.time() + 60
                while time.time() < end and (quiet < 15 or len(seen) < 25):
                    got = [mc.Reader(data) for name, data in raw_pump(bot, 0.2) if name == "level_chunk_with_light"]
                    near = {(x, z) for x, z in ((r.i32(), r.i32()) for r in got) if max(abs(x - cx), abs(z - cz)) <= 2}
                    quiet = 0 if near - seen else quiet + 1
                    seen |= near
                if len(seen) < 25:
                    raise SystemExit(f"vanilla sent {len(seen)} of the 25 chunks around {cx}, {cz}")
            bot.c.close()
            time.sleep(1.0)
        finally:
            v.stop()
        bad = [line for line in log.read_text().splitlines() if "Exception" in line or "ERROR" in line]
        if bad:
            raise SystemExit("vanilla did not take the world as it was written:\n  " + "\n  ".join(bad[:8]))
        states = States()
        biome_ids = {name: i for i, name in enumerate(mc.load_registry("worldgen_biome"))}
        chunks, statuses = {}, {}
        for region in sorted((world / "dimensions" / "minecraft" / "overworld" / "region").glob("*.mca")):
            for tag in region_chunks(region):
                statuses[tag["Status"]] = statuses.get(tag["Status"], 0) + 1
                chunk = read_chunk(tag, states, biome_ids)
                if chunk:
                    chunks[f"{int(tag['xPos'])},{int(tag['zPos'])}"] = chunk
        Path(out).write_text(json.dumps({"seed": seed, "chunks": chunks}))
        print(f"vanilla saved {sum(statuses.values())} chunks: " + ", ".join(f"{n} {s.removeprefix('minecraft:')}" for s, n in sorted(statuses.items())))
        print(f"{len(chunks)} with biomes, {sum(1 for c in chunks.values() if c['status'] in BARE)} with terrain nothing has run in -> {out}")
    finally:
        shutil.rmtree(world, ignore_errors=True)


# ----------------------------------------------------------------------------- ours

def record_ours(exe: str, out: str, seed: int) -> None:
    tool = Path(exe).resolve().parent / "terrain_dump"
    preset = tool.parent.name
    built = subprocess.run(["cmake", "--build", "--preset", preset, "--target", "terrain_dump"], cwd=ROOT, capture_output=True, text=True)
    if built.returncode != 0 or not tool.exists():
        raise SystemExit(f"could not build {tool}:\n{built.stdout[-3000:]}{built.stderr[-3000:]}")
    wanted = sorted({(cx + dx, cz + dz) for cx, cz in STOPS for dx in range(-REACH, REACH + 1) for dz in range(-REACH, REACH + 1)})
    asked = "".join(f"{x} {z}\n" for x, z in wanted)
    made = subprocess.run([str(tool), str(seed), out], input=asked, text=True, capture_output=True)
    if made.returncode != 0:
        raise SystemExit(f"terrain_dump failed:\n{made.stdout[-2000:]}{made.stderr[-2000:]}")
    print(f"{len(wanted)} chunks of seed {seed} -> {out}")


def read_ours(path: str) -> tuple[int, dict]:
    """terrain_dump's file: see tools/vanilla/terrain_dump.cpp."""
    data = Path(path).read_bytes()
    assert data[:8] == b"TERRDMP1", "not a file of terrain_dump"
    seed, count = struct.unpack_from("<qI", data, 8)
    at, chunks = 20, {}
    for _ in range(count):
        cx, cz = struct.unpack_from("<ii", data, at)
        at += 8
        biomes = struct.unpack_from(f"<{SECTIONS * 64}H", data, at)
        at += SECTIONS * 64 * 2
        tied = data[at:at + SECTIONS * 64]
        at += SECTIONS * 64
        heights = struct.unpack_from("<512i", data, at)
        at += 512 * 4
        marks_n, packed_n = struct.unpack_from("<II", data, at)
        at += 8
        marks = list(struct.unpack_from(f"<{marks_n}I", data, at))
        at += 4 * marks_n
        chunks[f"{cx},{cz}"] = {"biomes": biomes, "tied": tied, "WORLD_SURFACE_WG": list(heights[:256]), "OCEAN_FLOOR_WG": list(heights[256:]),
                                "marks": marks, "blocks": data[at:at + packed_n]}
        at += packed_n
    return seed, chunks


# ----------------------------------------------------------------------------- diff

def diff(a_path: str, b_path: str) -> int:
    theirs = json.loads(Path(a_path).read_text())
    seed, ours = read_ours(b_path)
    if theirs["seed"] != seed:
        raise SystemExit(f"two seeds: {theirs['seed']} and {seed}")
    names = mc.load_registry("worldgen_biome")
    block_names = {}
    path = ROOT / ".cache" / "mc" / mc.META["version"] / "reports" / "blocks.json"
    for name, block in json.loads(path.read_text()).items():
        for s in block["states"]:
            block_names[s["id"]] = name.removeprefix("minecraft:")
    missing = sorted(set(theirs["chunks"]) - set(ours))
    if missing:
        print(f"{len(missing)} chunks vanilla saved lie further from the bot's stops than this probe makes them: {missing[:6]}")
    cells = biome_differ = biome_tied = 0
    terrain = terrain_bad = 0
    failed = 0
    for key in sorted(set(theirs["chunks"]) & set(ours), key=lambda k: tuple(int(v) for v in k.split(","))):
        a, b = theirs["chunks"][key], ours[key]
        wrong = [i for i in range(SECTIONS * 64) if a["biomes"][i] != b["biomes"][i]]
        cells += SECTIONS * 64
        hard = [i for i in wrong if not b["tied"][i]]
        biome_tied += len(wrong) - len(hard)
        if hard:
            biome_differ += len(hard)
            failed += 1
            i = hard[0]
            print(f"chunk {key} ({a['status']}): {len(hard)} biome cells differ, first: section {i // 64 - 4} cell {i % 64}: "
                  f"vanilla {names[a['biomes'][i]]}, ours {names[b['biomes'][i]]}")
        if a["status"] not in BARE:
            continue
        terrain += 1
        said = []
        va = struct.unpack(f"<{SECTIONS * 4096}H", zlib.decompress(base64.b64decode(a["blocks"])))
        vb = struct.unpack(f"<{SECTIONS * 4096}H", zlib.decompress(b["blocks"], wbits=-15))     # (raw deflate)
        where = [i for i in range(len(va)) if va[i] != vb[i]]
        if where:
            i = where[0]
            said.append(f"{len(where)} blocks, first at {i & 15}, {(i >> 8) + MIN_Y}, {i >> 4 & 15}: vanilla "
                        f"{block_names.get(va[i], va[i])}, ours {block_names.get(vb[i], vb[i])}")
        for kind in ("WORLD_SURFACE_WG", "OCEAN_FLOOR_WG"):
            if a[kind] is not None and a[kind] != b[kind]:
                said.append(f"{kind} in {sum(1 for i in range(256) if a[kind][i] != b[kind][i])} columns")
        if a["marks"] != b["marks"]:
            said.append(f"the marks to be let flow ({len(a['marks'])} / {len(b['marks'])}, "
                        f"{'the same blocks in another order' if sorted(a['marks']) == sorted(b['marks']) else 'other blocks'})")
        if said:
            terrain_bad += 1
            failed += 1
            print(f"chunk {key}: " + "; ".join(said))
    print(f"biomes: {cells} cells of {len(set(theirs['chunks']) & set(ours))} chunks, {biome_differ} differ"
          + (f" ({biome_tied} more where two biomes are exactly as near: vanilla's own answer there depends on its thread)" if biome_tied else ""))
    print(f"terrain: {terrain} chunks vanilla saved before anything ran in them, {terrain_bad} differ")
    if terrain == 0:
        print("no chunk to compare the terrain of: vanilla saved none at such a status")
        return 1
    return 1 if failed else 0


def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith("-")]      # (compare.py hands on -v and --only: not this probe's)
    seed = next((int(a.split("=", 1)[1]) for a in sys.argv[1:] if a.startswith("--seed=")), SEED)
    if len(args) >= 2 and args[0] == "vanilla":
        record_vanilla(args[1], seed, args[2] if len(args) > 2 else None)   # (compare.py names the bot)
    elif len(args) >= 3 and args[0] == "ours":
        record_ours(args[1], args[2], seed)
    elif len(args) >= 3 and args[0] == "diff":
        return diff(args[1], args[2])
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
