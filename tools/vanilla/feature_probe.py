#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Are the features put into a world the ones a vanilla server puts there?

    feature_probe.py run [--keep=DIR]               # all three below, one after the other
    feature_probe.py vanilla <out.json> [bot]       # runs vanilla in a world of this probe's own; what it saved, and in which order it made it
    feature_probe.py jar <out.bin> <vanilla.json>   # the jar's own classes, asked without a server, decorating in that order
    feature_probe.py diff <vanilla.json> <out.bin>

A vanilla server decorates one chunk at a time, and what a chunk's features find depends on which
of its neighbours were decorated before it: a tree does not grow where the lake of the chunk beside
it already is. The order is the server's - which ticket came first, which thread was done when - so
two runs of one server on one seed do not make the same world along the borders of chunks, and
there is no "vanilla's chunk" to hold a generator's to.

But the server says what it did. Started with `--jfrProfile` it keeps a flight recording, and
`minecraft.ChunkGeneration` is an event in it for every status of every chunk; the `features` of
two chunks never run at once. `vanilla` takes the order out of the recording and writes it beside
what the server saved. Whoever can decorate chunks in a given order - the jar's own classes on the
extractor's stand (`jar`: tools/mcgen/extract/WorldgenFeatures.java) - is then asked for exactly
that run, and has to come out with what the server saved:

  * the chunks around what a player sees that were saved before they were chunks of the world -
    with their own features and those of the neighbours that came before, or with their terrain
    and what a neighbour's features put into it. Nothing ever ran in them: blocks, heightmaps, the
    blocks marked to be looked at when the chunk first ticks (in order), the scheduled ticks of
    blocks and fluids (in order), block entities. These must all be the same.
  * of the chunks of the world the ring that never began to tick. Those lie beside chunks that do
    tick, whose water runs into them and whose changes their leaves are told of: how many are still
    as generation left them is said, and does not fail the probe.

One thing a server's answer depends on beyond the order: how light it is where a mushroom would
stand. That is its light engine's answer, given by another thread: bright as long as no chunk
around has been handed to the engine, dark once one beside it has, and later whatever spills over
(SkyLightSectionStorage.getLightValue). A replay says bright, and names every block it asked about
after the server had begun to light a chunk at or beside it; `diff` leaves those blocks out.

The world is terrain_probe.py's - written by hand, this seed, no structures - with the game's own
features in it. Vanilla runs under the rules of AGENTS.md (from .cache/vanilla-server/, the cached
jar, `--world feature-probe`: server.properties stays as it is); the world and the recording are
removed afterwards.
"""
from __future__ import annotations

import base64
import json
import re
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
from terrain_probe import MIN_Y, SECTIONS, SEED, STOPS, States, region_chunks, unpack, write_world  # noqa: E402
from world_probe import JAVA, PORT, SERVER_DIR, Vanilla, fresh_name, raw_pump  # noqa: E402

WORLD = "feature-probe"
JFR = str(Path(JAVA).with_name("jfr"))
# ChunkStatus, in order: what a chunk has been through.
STATUSES = ("empty", "structure_starts", "structure_references", "biomes", "terrain", "features", "initialize_light", "light", "spawn", "full")
# Heightmap.Types that a chunk keeps once its terrain is made (ChunkStatus.FINAL_HEIGHTMAPS).
HEIGHTMAPS = ("WORLD_SURFACE", "OCEAN_FLOOR", "MOTION_BLOCKING", "MOTION_BLOCKING_NO_LEAVES")


def rank(status: str) -> int:
    return STATUSES.index(status)


# ----------------------------------------------------------------------------- vanilla's recording

def instant(text: str) -> int:
    """An event's time as `jfr print --json` writes it (ISO 8601, nanoseconds, an offset), in nanoseconds."""
    m = re.fullmatch(r"(\d+)-(\d+)-(\d+)T(\d+):(\d+):(\d+)(?:\.(\d+))?(Z|[+-]\d+:\d+)", text)
    if not m:
        raise SystemExit(f"a time this probe does not read: {text}")
    import calendar
    seconds = calendar.timegm(tuple(int(m.group(i)) for i in range(1, 7)))
    if m.group(8) != "Z":
        sign = -1 if m.group(8)[0] == "-" else 1
        h, mi = m.group(8)[1:].split(":")
        seconds -= sign * (int(h) * 3600 + int(mi) * 60)
    return seconds * 1_000_000_000 + int((m.group(7) or "0").ljust(9, "0")[:9])


def span(text: str) -> int:
    """A duration ("PT0.012345678S"), in nanoseconds."""
    m = re.fullmatch(r"PT(?:(\d+)H)?(?:(\d+)M)?(?:([\d.]+)S)?", text)
    if not m:
        raise SystemExit(f"a duration this probe does not read: {text}")
    return int(m.group(1) or 0) * 3600_000_000_000 + int(m.group(2) or 0) * 60_000_000_000 + round(float(m.group(3) or 0) * 1e9)


def generation_events(recording: Path) -> list[dict]:
    """minecraft.ChunkGeneration of the overworld: {status, x, z, start, end, thread}, by start."""
    printed = subprocess.run([JFR, "print", "--json", "--events", "minecraft.ChunkGeneration", str(recording)], capture_output=True, text=True)
    if printed.returncode != 0:
        raise SystemExit(f"jfr print failed: {printed.stderr[-2000:]}")
    events = []
    for e in json.loads(printed.stdout)["recording"]["events"]:
        v = e["values"]
        if v["level"] != "minecraft:overworld":
            continue
        start = instant(v["startTime"])
        thread = v.get("eventThread") or {}
        events.append({"status": v["status"].removeprefix("minecraft:"), "x": v["chunkPosX"], "z": v["chunkPosZ"], "start": start,
                       "end": start + span(v["duration"]), "thread": thread.get("javaName") or thread.get("osName") or "?"})
    events.sort(key=lambda e: e["start"])
    return events


def read_chunk(tag: dict, states: States) -> dict | None:
    """What a saved chunk is, once its terrain is made (SerializableChunkData), in the ids of this version."""
    status = tag["Status"].removeprefix("minecraft:")
    if rank(status) < rank("terrain"):
        return None
    sections = {int(s["Y"]): s for s in tag["sections"]}
    blocks = bytearray()
    for y in range(MIN_Y >> 4, (MIN_Y >> 4) + SECTIONS):
        bs = sections.get(y, {}).get("block_states")
        if bs is None:
            blocks += bytes(8192)
            continue
        palette = [states.of(entry) for entry in bs["palette"]]
        cells = [0] * 4096 if len(palette) == 1 else unpack(bs["data"], max(4, (len(palette) - 1).bit_length()), 4096)
        blocks += struct.pack("<4096H", *(palette[i] for i in cells))
    out = {"status": status, "blocks": base64.b64encode(zlib.compress(bytes(blocks), 1)).decode(), "heights": {}}
    for kind in HEIGHTMAPS:
        packed = tag.get("Heightmaps", {}).get(kind)
        if packed is not None:
            out["heights"][kind] = [MIN_Y + v for v in unpack(packed, 9, 256)]
    marks = []
    for index, of in enumerate(tag.get("PostProcessing", [])):
        marks += [index << 12 | (int(m) & 0xFFF) for m in of]
    out["marks"] = marks
    for kind in ("block_ticks", "fluid_ticks"):
        out[kind] = [[t["i"], int(t["x"]), int(t["y"]), int(t["z"]), int(t["t"]), int(t["p"])] for t in tag.get(kind, [])]
    out["block_entities"] = [mc.snbt(be) for be in tag.get("block_entities", [])]
    return out


def record_vanilla(out: str, seed: int, name: str | None = None) -> None:
    world = SERVER_DIR / WORLD
    if world.exists():
        shutil.rmtree(world)
    log = Path(tempfile.gettempdir()) / "feature_probe_vanilla.log"
    debug = SERVER_DIR / "debug"
    before = set(debug.glob("*.jfr")) if debug.exists() else set()
    try:
        write_world(world, seed, pack=None)
        v = Vanilla(WORLD, log, ("--jfrProfile",))
        try:
            v.until(r"Done \(")
            bot = Bot(PORT, name or fresh_name("Fp"))
            bot.join()
            for cx, cz in STOPS:
                v.cmd(f"tp {bot.name} {cx * 16 + 8} 200 {cz * 16 + 8}")
                # Until the chunks of the view have come and nothing more does.
                seen, quiet, end = set(), 0, time.time() + 90
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
        recordings = sorted(set(debug.glob("*.jfr")) - before)
        if len(recordings) != 1:
            raise SystemExit(f"the server left {len(recordings)} flight recordings in {debug}: one was expected (--jfrProfile)")
        events = generation_events(recordings[0])
        states = States()
        chunks, statuses = {}, {}
        for region in sorted((world / "dimensions" / "minecraft" / "overworld" / "region").glob("*.mca")):
            for tag in region_chunks(region):
                statuses[tag["Status"]] = statuses.get(tag["Status"], 0) + 1
                chunk = read_chunk(tag, states)
                if chunk:
                    chunks[f"{int(tag['xPos'])},{int(tag['zPos'])}"] = chunk
        Path(out).write_text(json.dumps({"seed": seed, "events": events, "chunks": chunks}))
        print(f"vanilla saved {sum(statuses.values())} chunks: " + ", ".join(f"{n} {s.removeprefix('minecraft:')}" for s, n in sorted(statuses.items(), key=lambda kv: rank(kv[0].removeprefix("minecraft:")))))
        by = {}
        for e in events:
            by[e["status"]] = by.get(e["status"], 0) + 1
        print(f"its flight recording has {len(events)} generation events: " + ", ".join(f"{n} {s}" for s, n in sorted(by.items(), key=lambda kv: rank(kv[0]))))
        print(f"{len(chunks)} chunks with terrain -> {out}")
    finally:
        shutil.rmtree(world, ignore_errors=True)
        for recording in (set(debug.glob("*.jfr")) - before) if debug.exists() else ():
            recording.unlink()


# ----------------------------------------------------------------------------- the same run, replayed

def steps(events: list[dict]) -> list[tuple[str, int, int]]:
    """What a replay does, in the server's order: a chunk's terrain is there when its `terrain` ends and a
    chunk is decorated when its `features` runs (no two run at once). "L": the server had begun to give
    the chunk to its light engine before the decoration that follows was over - from then on, what light
    a block in it or beside it has is that engine's thread's business."""
    when = []
    for e in events:
        if e["status"] == "terrain":
            when.append((e["end"], "T", e["x"], e["z"]))
        elif e["status"] == "features":
            when.append((e["end"], "F", e["x"], e["z"]))
        elif e["status"] == "initialize_light":
            when.append((e["start"], "L", e["x"], e["z"]))
    return [(what, x, z) for _, what, x, z in sorted(when)]


def record_jar(out: str, vanilla_path: str) -> None:
    """The jar's own classes on the extractor's stand (tools/mcgen/extract/WorldgenFeatures.java), in the server's order."""
    theirs = json.loads(Path(vanilla_path).read_text())
    work = ROOT / ".cache" / "mc" / mc.META["version"] / "worldgen"
    (work / "replay.txt").write_text(f"seed {theirs['seed']}\n" + "".join(f"{what} {x} {z}\n" for what, x, z in steps(theirs["events"])))
    ran = subprocess.run([sys.executable, str(ROOT / "tools" / "mcgen" / "mcgen.py"), "worldgen", "replay"], cwd=ROOT, capture_output=True, text=True)
    if ran.returncode != 0:
        raise SystemExit(f"mcgen.py worldgen replay failed:\n{ran.stdout[-3000:]}{ran.stderr[-3000:]}")
    said = json.loads((work / "replay.json").read_text())
    shutil.copyfile(work / "replay.bin", out)
    print(f"the jar's classes: {said['decorated']} chunks decorated in the server's order, {said['terrain']} with terrain -> {out}")
    if said["far"]:
        print(f"read further off than a decoration writes: {said['far']}")


class In:
    def __init__(self, data: bytes):
        self.data, self.at = data, 0

    def take(self, fmt: str):
        v = struct.unpack_from("<" + fmt, self.data, self.at)
        self.at += struct.calcsize("<" + fmt)
        return v if len(v) > 1 else v[0]

    def bytes(self, n: int) -> bytes:
        self.at += n
        return self.data[self.at - n:self.at]

    def str(self) -> str:
        return self.bytes(self.take("i")).decode()


def read_replay(path: str) -> tuple[int, dict]:
    """replay.bin (WorldgenFeatures.chunk): the chunks of a replayed run, in the shape read_chunk gives vanilla's."""
    r = In(Path(path).read_bytes())
    assert r.bytes(8) == b"WGREPL01", "not a replay"
    seed, count = r.take("q"), r.take("i")
    chunks = {}
    for _ in range(count):
        x, z, decorated = r.take("i"), r.take("i"), r.take("B")
        c = {"decorated": bool(decorated), "blocks": zlib.decompress(r.bytes(r.take("i"))), "heights": {}}
        for kind in HEIGHTMAPS:
            c["heights"][kind] = list(r.take("256i"))
        c["marks"] = [r.take("i") for _ in range(r.take("i"))]
        for kind in ("block_ticks", "fluid_ticks"):
            c[kind] = [[r.str(), *r.take("5i")] for _ in range(r.take("i"))]
        c["block_entities"] = []
        for _ in range(r.take("i")):
            raw = r.bytes(r.take("i"))
            c["block_entities"].append(mc.snbt(mc.Reader(raw[:1] + raw[3:]).nbt()))    # (NbtIo.write: a root with the empty name)
        c["tainted"] = [tuple(r.take("3i")) for _ in range(r.take("i"))]
        chunks[f"{x},{z}"] = c
    return seed, chunks


# ----------------------------------------------------------------------------- diff

def diff(a_path: str, b_path: str) -> int:
    theirs = json.loads(Path(a_path).read_text())
    seed, ours = read_replay(b_path)
    if theirs["seed"] != seed:
        raise SystemExit(f"two seeds: {theirs['seed']} and {seed}")
    block_names = {}
    for name, block in json.loads((ROOT / ".cache" / "mc" / mc.META["version"] / "reports" / "blocks.json").read_text()).items():
        for s in block["states"]:
            block_names[s["id"]] = name.removeprefix("minecraft:")
    their = theirs["chunks"]
    full = {k for k, c in their.items() if c["status"] == "full"}
    counts = {"proto": 0, "edge": 0, "edge_same": 0, "ticked": 0, "bad": 0, "tainted": 0, "missing": 0}
    for key in sorted(their, key=lambda k: tuple(int(v) for v in k.split(","))):
        a, b = their[key], ours.get(key)
        x, z = (int(v) for v in key.split(","))
        if b is None:
            counts["missing"] += 1
            continue
        whole = a["status"] != "full"
        if not whole:
            # A chunk of the world ticks once the eight around it are chunks of the world too
            # (ChunkMap.prepareTickingChunk): then what generation left to do has been done in it.
            if all(f"{x + dx},{z + dz}" in full for dx in (-1, 0, 1) for dz in (-1, 0, 1)):
                counts["ticked"] += 1
                continue
        said = []
        # Where a mushroom may stand or not, the block under it may be snowy or not and snow may lie or not.
        skip = {(tx & 15, ty + dy, tz & 15) for tx, ty, tz in b["tainted"] for dy in (-1, 0)}
        counts["tainted"] += len(b["tainted"])
        va = struct.unpack(f"<{SECTIONS * 4096}H", zlib.decompress(base64.b64decode(a["blocks"])))
        vb = struct.unpack(f"<{SECTIONS * 4096}H", b["blocks"])
        where = [i for i in range(len(va)) if va[i] != vb[i] and (i & 15, (i >> 8) + MIN_Y, i >> 4 & 15) not in skip]
        if where:
            i = where[0]
            said.append(f"{len(where)} blocks, first at {x * 16 + (i & 15)}, {(i >> 8) + MIN_Y}, {z * 16 + (i >> 4 & 15)}: vanilla "
                        f"{block_names.get(va[i], va[i])}, the replay {block_names.get(vb[i], vb[i])}")
        columns = {(tx, tz) for tx, _, tz in skip}
        for kind, heights in a["heights"].items():
            wrong = [i for i in range(256) if heights[i] != b["heights"][kind][i] and (i & 15, i >> 4) not in columns]
            if wrong:
                said.append(f"{kind} in {len(wrong)} columns")
        marked = lambda marks: [m for m in marks if (m & 15, ((m >> 12) << 4 | m >> 4 & 15) + MIN_Y, m >> 8 & 15) not in skip]
        if marked(a["marks"]) != marked(b["marks"]):
            same = sorted(marked(a["marks"])) == sorted(marked(b["marks"]))
            said.append(f"the marks ({len(a['marks'])} / {len(b['marks'])}, {'the same blocks in another order' if same else 'other blocks'})")
        for kind in ("block_ticks", "fluid_ticks"):
            if a[kind] != b[kind]:
                same = sorted(t[:4] for t in a[kind]) == sorted(t[:4] for t in b[kind])
                said.append(f"{kind} ({len(a[kind])} / {len(b[kind])}, {'the same in another order' if same else 'others'})")
        ea, eb = sorted(a["block_entities"]), sorted(b["block_entities"])
        if ea != eb:
            said.append(f"block entities ({len(ea)} / {len(eb)}): " + "; ".join(sorted(set(ea) ^ set(eb))[:2])[:300])
        if not whole:
            # A chunk of the world beside chunks that tick: water runs into it from them, leaves in it are
            # told of what happens beside them, and it is saved as a chunk of the world is (its ticks
            # sorted, its block entities made). How many of these are still as generation left them is
            # worth a line; that one is not does not say the generator is wrong.
            counts["edge"] += 1
            counts["edge_same"] += not said
            continue
        counts["proto"] += 1
        if said:
            counts["bad"] += 1
            if counts["bad"] <= 12:
                print(f"chunk {key} ({a['status']}{'' if b['decorated'] else ', not decorated itself'}): " + "; ".join(said))
    decorated = sum(1 for k, c in their.items() if c["status"] != "full" and k in ours and ours[k]["decorated"])
    print(f"{counts['proto']} chunks vanilla saved before they were chunks of the world ({decorated} with their own features, the rest with "
          f"what their neighbours' put into them): {counts['bad']} differ"
          + (f"; {counts['tainted']} blocks left out (they asked for light beside a chunk the server was lighting)" if counts["tainted"] else "")
          + (f"; {counts['missing']} the replay did not make" if counts["missing"] else ""))
    print(f"of {counts['edge']} chunks of the world that never ticked, {counts['edge_same']} are still as generation left them; "
          f"{counts['ticked']} more had ticked")
    if counts["proto"] == 0:
        print("no chunk to compare")
        return 1
    return 1 if counts["bad"] or counts["missing"] else 0


def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    seed = next((int(a.split("=", 1)[1]) for a in sys.argv[1:] if a.startswith("--seed=")), SEED)
    if len(args) >= 2 and args[0] == "vanilla":
        record_vanilla(args[1], seed, args[2] if len(args) > 2 else None)
    elif len(args) >= 3 and args[0] == "jar":
        record_jar(args[1], args[2])
    elif len(args) >= 3 and args[0] == "diff":
        return diff(args[1], args[2])
    elif args == ["run"]:
        keep = next((a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--keep=")), None)
        work = Path(keep) if keep else Path(tempfile.mkdtemp(prefix="feature-probe-"))
        work.mkdir(parents=True, exist_ok=True)
        try:
            record_vanilla(str(work / "vanilla.json"), seed)
            record_jar(str(work / "jar.bin"), str(work / "vanilla.json"))
            return diff(str(work / "vanilla.json"), str(work / "jar.bin"))
        finally:
            if not keep:
                shutil.rmtree(work, ignore_errors=True)
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
