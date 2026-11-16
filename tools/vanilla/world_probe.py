#!/usr/bin/env python3
"""Does a vanilla world, imported, go out to a client as the vanilla server sends it?

    world_probe.py build <name>                  # runs vanilla: makes the world <name> and builds the fixture in it
    world_probe.py vanilla <name> <out.json>     # runs vanilla on that world again and records what a bot is sent
    world_probe.py ours <exe> <name> <out.json>  # copies the world, lets our server import it, records the same
    world_probe.py diff <a.json> <b.json>

`build` starts the vanilla reference server (under the rules in AGENTS.md: from
.cache/vanilla-server/, the cached jar, Java 25) with `--world <name>`, so that the world of the
other probes and server.properties stay as they are, joins one bot so that the chunks around the
spawn are loaded, and builds from the console: blocks whose states have properties, light sources
at chunk borders and a roof that throws a shadow across one, sections with two and with many
biomes, chests - plain, double, named, with a stack that has components - a furnace, a sign, a
few entities. Then `save-all flush` and `stop`: what `vanilla` and `ours` see is a world read
from disk, never the session that made it.

A recording is every level_chunk_with_light the bot gets while it stands at the spawn, by chunk.
`diff` compares chunk by chunk - byte for byte first, and where two differ it says in which part:
heightmaps, the blocks or biomes of a section, the light masks, a light layer, the block
entities. Block entities are compared as a set (a client keeps them in a map; vanilla lists them
in the order of a HashMap), and their update tags are reported apart: this server sends none for
the kinds it keeps as tags (a sign's text), which is a known gap, not a difference of the import.

A player's spawn is somewhere within ten blocks of the world's (vanilla draws it), so the two
bots need not stand in the same chunk: only chunks both were sent are compared. The fixture is
within a chunk and a half of the origin.
"""
from __future__ import annotations

import json
import queue
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "integration"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import mcproto as mc  # noqa: E402
from entity_probe import Bot  # noqa: E402

VERSION = mc.META["version"]
SERVER_DIR = ROOT / ".cache" / "vanilla-server"
JAVA = "/usr/lib/jvm/java-25-openjdk/bin/java"
PORT = 25577
OUR_PORT = 25591
LINE = re.compile(r"^\[\d\d:\d\d:\d\d\] \[([^\]]*)\]: (.*)$")

# The fixture, as console commands. y = -60 is the first free block of the default flat world.
FIXTURE = [
    # states with properties: every one must come back as the same state id
    "setblock 2 -60 2 minecraft:oak_stairs[facing=west,half=top,shape=straight]",
    "setblock 3 -60 2 minecraft:oak_slab[type=double]",
    "setblock 4 -60 2 minecraft:oak_fence",
    "setblock 5 -60 2 minecraft:oak_log[axis=x]",
    "setblock 6 -60 2 minecraft:redstone_lamp[lit=true]",
    "setblock 7 -60 2 minecraft:snow[layers=5]",
    "fill 2 -60 4 7 -58 6 minecraft:glass hollow",
    # light: sources on both sides of a chunk border, one high up beside an empty chunk column,
    # and a roof over a border (the shadow under it is sky light that had to go round)
    "setblock 15 -60 8 minecraft:glowstone",
    "setblock 16 -60 20 minecraft:sea_lantern",
    "setblock -1 -60 -1 minecraft:torch",
    "setblock 0 -40 -17 minecraft:glowstone",
    "setblock 31 -3 31 minecraft:glowstone",
    "fill 10 -50 -6 22 -50 6 minecraft:stone",
    "fill -20 -57 10 -12 -57 18 minecraft:white_stained_glass",
    "fill -16 -60 -16 -16 -30 -16 minecraft:stone",
    "setblock -17 -30 -16 minecraft:jack_o_lantern[facing=south]",
    # biomes: a section with two, and one with more than a palette of three bits holds (9)
    "fillbiome 0 -64 0 15 -49 7 minecraft:desert",
    "fillbiome -16 -64 -16 -13 -49 -13 minecraft:jungle",
    "fillbiome -12 -64 -16 -9 -49 -13 minecraft:badlands",
    "fillbiome -8 -64 -16 -5 -49 -13 minecraft:taiga",
    "fillbiome -4 -64 -16 -1 -49 -13 minecraft:savanna",
    "fillbiome -16 -64 -12 -13 -49 -9 minecraft:swamp",
    "fillbiome -12 -64 -12 -9 -49 -9 minecraft:beach",
    "fillbiome -8 -64 -12 -5 -49 -9 minecraft:forest",
    "fillbiome -4 -64 -12 -1 -49 -9 minecraft:cherry_grove",
    # block entities
    'setblock 2 -60 9 minecraft:chest[facing=east]{Items:[{Slot:0b,id:"minecraft:stone",count:5},{Slot:13b,id:"minecraft:oak_planks",count:64},{Slot:26b,id:"minecraft:diamond"}]}',
    'setblock 2 -60 11 minecraft:chest[facing=east,type=right]{Items:[{Slot:1b,id:"minecraft:cobblestone",count:32}]}',
    'setblock 2 -60 12 minecraft:chest[facing=east,type=left]{Items:[{Slot:2b,id:"minecraft:dirt",count:7}]}',
    'setblock 4 -60 9 minecraft:chest[facing=east]{CustomName:"Treasure",Items:[{Slot:0b,id:"minecraft:gold_ingot",count:9}]}',
    'setblock 6 -60 9 minecraft:chest[facing=east]{Items:[{Slot:0b,id:"minecraft:diamond_sword",count:1,components:{"minecraft:damage":7}}]}',
    'setblock 8 -60 9 minecraft:furnace[facing=south]{Items:[{Slot:0b,id:"minecraft:iron_ore",count:3}]}',
    'setblock 10 -60 9 minecraft:oak_sign[rotation=8]{front_text:{messages:["hello","world","",""]}}',
    "setblock 12 -60 9 minecraft:crafting_table",
    # entities: kept by the import, not shown
    "summon minecraft:armor_stand 3.5 -60 14.5",
    'summon minecraft:item 5.5 -60 14.5 {Item:{id:"minecraft:emerald",count:3}}',
]


class Vanilla:
    def __init__(self, world: str, log_path: Path):
        eula = SERVER_DIR / "eula.txt"
        if not eula.exists() or "eula=true" not in eula.read_text():
            raise SystemExit(f"{eula} is missing or not accepted: that is the user's to do (AGENTS.md)")
        props = (SERVER_DIR / "server.properties").read_text()
        for must in ("server-ip=127.0.0.1", "online-mode=false", f"server-port={PORT}", "level-type=minecraft\\:flat"):
            if must not in props:
                raise SystemExit(f"server.properties must have {must}")
        jar = ROOT / ".cache" / "mc" / VERSION / "server.jar"
        self.log = open(log_path, "w")
        self.proc = subprocess.Popen([JAVA, "-Xms1G", "-Xmx2G", "-jar", str(jar), "--nogui", "--world", world], cwd=SERVER_DIR,
                                     text=True, bufsize=1, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.lines: queue.Queue[str] = queue.Queue()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self) -> None:
        for line in self.proc.stdout:
            self.log.write(line)
            m = LINE.match(line.rstrip("\n"))
            self.lines.put(m.group(2) if m else line.rstrip("\n"))
        self.lines.put("<<EOF>>")

    def cmd(self, *lines: str) -> None:
        for line in lines:
            self.log.write(f">>> {line}\n")
        self.proc.stdin.write("".join(line + "\n" for line in lines))
        self.proc.stdin.flush()

    def until(self, pattern: str, timeout: float = 120.0) -> list[str]:
        rx = re.compile(pattern)
        before: list[str] = []
        end = time.time() + timeout
        while True:
            try:
                line = self.lines.get(timeout=max(0.01, end - time.time()))
            except queue.Empty:
                raise SystemExit(f"the server did not say /{pattern}/ within {timeout} s; last lines: {before[-5:]}")
            if line == "<<EOF>>":
                raise SystemExit(f"the server stopped while waiting for /{pattern}/; last lines: {before[-8:]}")
            if rx.search(line):
                return before
            before.append(line)

    def stop(self) -> None:
        try:
            self.cmd("stop")
            self.proc.wait(timeout=60)
        except Exception:
            self.proc.terminate()
        self.log.close()


def fresh_name(prefix: str) -> str:
    return f"{prefix}{int(time.time()) % 100000:05d}"


def build(world: str) -> None:
    target = SERVER_DIR / world
    if target.exists():
        raise SystemExit(f"{target} exists: a fixture is built into a new world (remove it, or pick another name)")
    v = Vanilla(world, Path(tempfile.gettempdir()) / "world_probe_build.log")
    try:
        v.until(r"Done \(")
        bot = Bot(PORT, fresh_name("Wb"))
        bot.join()
        bot.pump(3.0)
        failed = []
        for line in FIXTURE:
            v.cmd(line)
            # Every command answers with one line; what it says decides whether the fixture stands.
            said = v.until(r"Changed the block|Successfully filled|biome entr|Biomes set|Summoned new|Could not|Unknown|Incorrect|Expected|"
                           r"<--\[HERE\]|not loaded|Invalid|Can't", timeout=20)
            bot.pump(0.1)
            if said and any("<--[HERE]" in s for s in said[-2:]):
                failed.append(line)
        v.cmd("save-all flush")
        v.until(r"Saved the game")
        if failed:
            raise SystemExit("commands the server did not take:\n  " + "\n  ".join(failed))
        print(f"built {len(FIXTURE)} steps into {target}")
    finally:
        v.stop()
    log = (Path(tempfile.gettempdir()) / "world_probe_build.log").read_text()
    bad = [line for line in log.splitlines() if re.search(r"Unknown|Incorrect|Expected|<--\[HERE\]|Could not|not loaded", line)]
    if bad:
        raise SystemExit("the fixture is not whole:\n  " + "\n  ".join(bad[:10]))


def record(port: int, out: str) -> None:
    bot = Bot(port, fresh_name("Wr"))
    bot.join()
    chunks: dict[str, str] = {}
    forgotten = 0
    end = time.time() + 8.0
    quiet = 0
    while time.time() < end and quiet < 20:
        got = raw_pump(bot, 0.1)
        quiet = 0 if got else quiet + 1
        for name, data in got:
            if name == "level_chunk_with_light":
                r = mc.Reader(data)
                chunks[f"{r.i32()},{r.i32()}"] = data.hex()
            elif name == "forget_level_chunk":
                forgotten += 1
    bot.c.close()
    Path(out).write_text(json.dumps({"chunks": chunks, "forgotten": forgotten}))
    print(f"{len(chunks)} chunks -> {out}")


def raw_pump(bot: Bot, seconds: float) -> list[tuple[str, bytes]]:
    """Bot.pump keeps the bytes of entity packets only; here the chunks are wanted."""
    import socket
    import struct
    c = bot.c
    out = []
    end = time.time() + seconds
    while True:
        left = end - time.time()
        if left <= 0:
            break
        c.sock.settimeout(max(left, 0.001))
        try:
            got, r = c.recv_any()
        except (socket.timeout, TimeoutError):
            break
        data = bytes(r.d[r.i:])
        if got == "player_position":
            tid = r.varint()
            x, y, z = r.f64(), r.f64(), r.f64()
            r.take(24)
            yaw, pitch = r.f32(), r.f32()
            c.send("accept_teleportation", mc.varint(tid) + struct.pack(">dddff", x, y, z, yaw, pitch))
        elif got == "keep_alive":
            c.send("keep_alive", data)
        elif got == "chunk_batch_finished":
            c.send("chunk_batch_received", struct.pack(">f", 64.0))
        elif got == "disconnect":
            raise RuntimeError(f"disconnected: {data.hex()}")
        out.append((got, data))
    c.sock.settimeout(10)
    return out


def record_vanilla(world: str, out: str) -> None:
    if not (SERVER_DIR / world / "level.dat").exists():
        raise SystemExit(f"no world {world}: run `world_probe.py build {world}` first")
    v = Vanilla(world, Path(tempfile.gettempdir()) / "world_probe_vanilla.log")
    try:
        v.until(r"Done \(")
        record(PORT, out)
    finally:
        v.stop()


def record_ours(exe: str, world: str, out: str) -> None:
    src = SERVER_DIR / world
    if not (src / "level.dat").exists():
        raise SystemExit(f"no world {world}: run `world_probe.py build {world}` first")
    tmp = Path(tempfile.mkdtemp(prefix="world-probe-"))
    copy = tmp / "world"
    shutil.copytree(src, copy, ignore=shutil.ignore_patterns("session.lock"))
    log = open(tmp / "server.log", "w")
    proc = subprocess.Popen([exe, "--server-ip=127.0.0.1", f"--server-port={OUR_PORT}", "--online-mode=false", "--view-distance=2",
                             "--config=/nonexistent", f"--level-name={copy}", f"--ops-file={tmp / 'ops.json'}"],
                            stdin=subprocess.PIPE, stdout=log, stderr=log)
    try:
        end = time.time() + 120
        while "listening on" not in (tmp / "server.log").read_text():
            if proc.poll() is not None or time.time() > end:
                raise SystemExit("our server did not start:\n" + (tmp / "server.log").read_text())
            time.sleep(0.05)
        record(OUR_PORT, out)
    finally:
        proc.terminate()
        proc.wait(timeout=30)
        log.close()
        print((tmp / "server.log").read_text().strip())
        shutil.rmtree(tmp, ignore_errors=True)


def parts(hexed: str) -> dict:
    ch = mc.parse_chunk(mc.Reader(bytes.fromhex(hexed)))
    out = {"heightmaps": ch["heightmaps"]}
    for i, s in enumerate(ch["sections"]):
        out[f"section {i - 4} counts"] = (s["non_air"], s["fluids"])
        out[f"section {i - 4} blocks"] = [mc.paletted_get(s["blocks"], k) for k in range(4096)]
        out[f"section {i - 4} biomes"] = [mc.paletted_get(s["biomes"], k) for k in range(64)]
        out[f"section {i - 4} block palette"] = (s["blocks"]["bits"], s["blocks"]["palette"])
        out[f"section {i - 4} biome palette"] = (s["biomes"]["bits"], s["biomes"]["palette"])
    light = ch["light"]
    out["light masks"] = (light["sky_mask"], light["block_mask"], light["empty_sky"], light["empty_block"])
    for kind in ("sky", "block"):
        for i, a in light[kind].items():
            out[f"{kind} light {i - 5}"] = a
    out["block entities"] = sorted((pos, kind) for pos, kind, tag in ch["block_entities"])
    out["update tags"] = sorted((pos, mc.snbt(tag) if tag is not None else None) for pos, kind, tag in ch["block_entities"])
    return out


def diff(a_path: str, b_path: str) -> int:
    a, b = json.loads(Path(a_path).read_text())["chunks"], json.loads(Path(b_path).read_text())["chunks"]
    both = sorted(set(a) & set(b), key=lambda k: tuple(int(v) for v in k.split(",")))
    print(f"{len(a)} and {len(b)} chunks, {len(both)} in both")
    same = same_content = differ = 0
    tags = []
    for key in both:
        if a[key] == b[key]:
            same += 1
            continue
        pa, pb = parts(a[key]), parts(b[key])
        if pa["update tags"] != pb["update tags"]:
            tags.append((key, [t for t in pa["update tags"] if t not in pb["update tags"]],
                         [t for t in pb["update tags"] if t not in pa["update tags"]]))
        real = sorted(n for n in pa.keys() | pb.keys() if pa.get(n) != pb.get(n) and n != "update tags")
        if not real:
            # The bytes differ and nothing in them does: the heightmaps, a map on the wire, come in
            # the order of a HashMap of enums on vanilla's side - another one every time a JVM starts.
            same_content += 1
            continue
        differ += 1
        print(f"chunk {key}: differs in {', '.join(real[:12])}{' ...' if len(real) > 12 else ''}")
        for n in real[:4]:
            va, vb = pa.get(n), pb.get(n)
            if isinstance(va, (list, bytes)) and isinstance(vb, (list, bytes)) and len(va) == len(vb) and len(va) > 16:
                where = [i for i in range(len(va)) if va[i] != vb[i]]
                print(f"    {n}: {len(where)} of {len(va)} entries, first at {where[:6]}: {[va[i] for i in where[:6]]} / {[vb[i] for i in where[:6]]}")
            else:
                print(f"    {n}: {str(va)[:200]} / {str(vb)[:200]}")
    for key, only_a, only_b in tags:
        print(f"chunk {key}: update tags of block entities: {only_a} / {only_b}")
    print(f"{same} the same byte for byte, {same_content} the same but for the order of their heightmaps, {differ} different; "
          f"{len(tags)} with other update tags")
    return 1 if differ else 0


def main() -> int:
    args = sys.argv[1:]
    if len(args) == 2 and args[0] == "build":
        build(args[1])
    elif len(args) == 3 and args[0] == "vanilla":
        record_vanilla(args[1], args[2])
    elif len(args) == 4 and args[0] == "ours":
        record_ours(args[1], args[2], args[3])
    elif len(args) == 3 and args[0] == "diff":
        return diff(args[1], args[2])
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
