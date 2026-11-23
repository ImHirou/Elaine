#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""End-to-end tests: start the real server binary and talk to it with mcproto.Client.

    python3 tests/integration/run.py build/release/elaine [build/dev/elaine ...] [-k substring]... [-j N] [--failed] [-q] [--slow] [--keep-log]

Each scenario gets a fresh server on a free port. --slow adds the tests that wait on real timers
(keep-alive every 15 s). -j runs N scenarios at a time, each in a process of its own (0: two a core,
sixteen at most), the ones that took longest the last time first; several binaries are tested side by
side, in one pool. --failed runs the ones that failed the last time a binary was tested; -q prints
what failed and the last line, nothing else.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import http.server
import json
import math
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import traceback
import urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mcproto as mc  # noqa: E402
from mcproto import Client, META, varint  # noqa: E402

TESTS = []


def test(fn=None, *, slow=False, alone=False, **server_args):
    """`alone`: the scenario measures the server's ticks or fills the machine's cores by itself, so
    with -j it runs after the others, with nothing beside it."""
    def deco(f):
        TESTS.append((f, slow, alone, server_args))
        return f
    return deco(fn) if fn else deco


# Scenarios that run side by side (-j) are processes of their own, and each asks the kernel for free
# ports. A port is free again the moment it was looked at, so two of them may be given the same one -
# and the servers bind with SO_REUSEPORT: both would start, and share the connections. A port is
# therefore claimed by a file in a directory the runs share before it is used.
PORT_CLAIMS = os.environ.get("MC_TEST_PORT_CLAIMS")


def free_port() -> int:
    while True:
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            port = s.getsockname()[1]
        if not PORT_CLAIMS:
            return port
        try:
            os.close(os.open(os.path.join(PORT_CLAIMS, str(port)), os.O_CREAT | os.O_EXCL))
            return port
        except FileExistsError:
            pass


# The ticks a second a scenario's server runs at (tick-rate, vanilla's /tick rate). A scenario waits
# for ticks, not for seconds - settle() and the rest count pongs - so five times the rate is a fifth of
# the waiting. One that measures the server's ticks, or times what it does by the clock, asks for 20.
TICK_RATE = 100


class Server:
    def __init__(self, exe: str, world: str | None = None, **args):
        """`world`: the directory of a world to run (and to leave behind); without one the server
        gets a new world of its own, which goes when it stops."""
        self.port = free_port()
        # The operators' file: none to begin with, and nothing of a test left in the working directory.
        self.ops_file = os.path.join(tempfile.gettempdir(), f"ops-{os.getpid()}-{self.port}.json")
        self.own_world = None if world else tempfile.mkdtemp(prefix="world-")
        self.world = world or self.own_world
        # No random ticks unless a scenario asks for them: grass under a stone that was placed dies
        # whenever chance has it, and what a scenario is told must not depend on when.
        opts = {"server-ip": "127.0.0.1", "server-port": self.port, "online-mode": "false", "debug": "true",
                "config": "/nonexistent/server.properties", "ops-file": self.ops_file, "level-name": self.world,
                "random-tick-speed": 0, "tick-rate": TICK_RATE}
        start_timeout = float(args.pop("start_timeout", 5))  # (importing a world takes its time)
        opts.update({k.replace("_", "-"): v for k, v in args.items()})
        self.tick = 1.0 / float(opts["tick-rate"])   # seconds a tick
        self.log = tempfile.NamedTemporaryFile("w+", suffix=".log", delete=False)
        # stdin is the server's console: a pipe, so that a test can type on it.
        self.proc = subprocess.Popen([exe] + [f"--{k}={v}" for k, v in opts.items()], stderr=self.log, stdout=self.log,
                                     stdin=subprocess.PIPE)
        deadline = time.time() + start_timeout
        while True:
            if self.proc.poll() is not None:
                raise RuntimeError(f"server exited with {self.proc.returncode}:\n{self.read_log()}")
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=0.2).close()
                return
            except OSError:
                if time.time() > deadline:
                    self.stop()
                    raise RuntimeError("server did not start listening")
                time.sleep(0.002)

    def client(self, timeout: float = 5.0) -> Client:
        return Client("127.0.0.1", self.port, timeout)

    def cpu_seconds(self) -> float:
        """User + system CPU time the server process has used so far."""
        with open(f"/proc/{self.proc.pid}/stat") as f:
            fields = f.read().rsplit(")", 1)[1].split()
        return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")

    def read_log(self) -> str:
        self.log.flush()
        with open(self.log.name) as f:
            return f.read()

    def console(self, line: str) -> None:
        """Type a line on the server's console."""
        self.proc.stdin.write(line.encode() + b"\n")
        self.proc.stdin.flush()

    def wait_log(self, text: str, timeout: float = 5.0, since: int = 0) -> str:
        """The server's log once `text` is in it - after its first `since` characters, for a line
        that may be there already."""
        deadline = time.time() + timeout
        while True:
            log = self.read_log()
            if text in log[since:]:
                return log
            assert time.time() < deadline, f"{text!r} never came up in the log:\n" + "\n".join(log.splitlines()[-15:])
            time.sleep(0.02)

    def stop(self) -> int:
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        if self.proc.stdin:
            self.proc.stdin.close()
        if os.path.exists(self.ops_file):
            os.unlink(self.ops_file)
        if self.own_world:
            shutil.rmtree(self.own_world, ignore_errors=True)
        return self.proc.returncode


class MockSessionServer:
    """Stands in for sessionserver.mojang.com: approves everyone except the user 'Denied'."""

    def __init__(self):
        self.requests = []
        outer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                url = urllib.parse.urlparse(self.path)
                q = urllib.parse.parse_qs(url.query)
                outer.requests.append((url.path, q))
                name = q.get("username", [""])[0]
                if name == "Denied":
                    self.send_response(204)
                    self.end_headers()
                    return
                # One fixed id for the test that checks it; everyone else gets an id of their own.
                uid = "069a79f444e94726a5befca90e38aaf5" if name == "Notch" else mc.offline_uuid("online:" + name)
                body = json.dumps({"id": uid, "name": name,
                                   "properties": [{"name": "textures", "value": "dGV4", "signature": "c2ln"}]}).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *a):
                pass

        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.url = f"http://127.0.0.1:{self.httpd.server_address[1]}"
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def stop(self):
        self.httpd.shutdown()


# ----------------------------------------------------------------------------- shared flows

def join(c: Client, name: str, online: bool = False, view: int = 2, chat: int = 0) -> dict:
    """Login + configuration + the whole join burst. Returns everything observed.

    The view is the smallest a client may ask for: what is sent of it is paced by ticks
    (PlayerChunkSender), and the default view of 10 is 473 chunks - half a second of waiting at each
    join, for chunks most scenarios never look at. A scenario about the view asks for it."""
    out = {"profile": c.login(name, online)}
    out["config"] = c.configure(view_distance=view, chat=chat)
    out["login"] = mc.parse_login(c.expect("login"))
    c.entity_id = out["login"]["entity_id"]
    r = c.expect("player_abilities")
    out["abilities"] = (r.u8(), r.f32(), r.f32())
    r.done()
    r = c.expect("player_position")
    tp = {"id": r.varint(), "pos": (r.f64(), r.f64(), r.f64()), "delta": (r.f64(), r.f64(), r.f64()),
          "rot": (r.f32(), r.f32()), "relative": r.i32()}
    r.done()
    out["teleport"] = tp
    c.send("accept_teleportation", varint(tp["id"]) + struct.pack(">dddff", *tp["pos"], *tp["rot"]))

    out["tab"] = []
    while True:
        got, r = c.recv()
        if got == "player_info_update":
            actions = r.u8()
            assert actions == 0b00011101, bin(actions)
            count = r.varint()
            assert count >= 1
            out["tab_packets"] = out.get("tab_packets", 0) + 1
            for _ in range(count):
                entry = {"uuid": r.uuid(), "name": r.str(), "props": []}
                for _ in range(r.varint()):
                    pname, value = r.str(), r.str()
                    entry["props"].append((pname, value, r.str() if r.bool() else None))
                entry.update(game_mode=r.varint(), listed=r.bool(), latency=r.varint())
                out["tab"].append(entry)
            r.done()
        elif got == "set_default_spawn_position":
            out["spawn"] = (r.str(), r.i64(), r.f32(), r.f32())
            r.done()
        elif got == "game_event":
            out["game_event"] = (r.u8(), r.f32())
            r.done()
            break      # (what follows - the view's centre, the chunks, the menu - is the client's model's)
        else:
            raise AssertionError(f"unexpected play packet {got}")
    # The chunks come batch by batch from the end of the join's tick on (PlayerChunkSender): the
    # client's own cache (c.chunks) takes them; here the view is waited for.
    out["view"] = min(max(view, 2), out["login"]["view_distance"])
    c.sync_chunks(out["view"])
    assert c.chunk_log[0][0] == "center", "no set_chunk_cache_center before the first chunk"
    # A client says where it is every tick, at the least once a second. A server looks again at
    # who sees whom on each such packet (ChunkMap.move) - which is when what stands in the chunks
    # that have just come is shown.
    c.send("move_player_status_only", b"\x00")
    out["center"] = c.chunk_center
    out["chunks"] = c.chunks
    out["ticking"] = (c.tick_rate, c.tick_frozen, c.tick_steps)
    out["first_chunk"] = next(e[1][0] for e in c.chunk_log if e[0] == "batch")
    return out


def hashed_seed(seed: int) -> int:
    """What a client is told of a world's seed (BiomeManager.obfuscateSeed): the first eight bytes of
    the SHA-256 of its eight - Guava's hashLong and asLong, both low byte first."""
    return struct.unpack("<q", hashlib.sha256(struct.pack("<q", seed)).digest()[:8])[0]


def text_seed(text: str) -> int:
    """The seed a level-seed that is no number stands for: its String.hashCode()."""
    h = 0
    units = text.encode("utf-16-be")
    for i in range(0, len(units), 2):
        h = (h * 31 + (units[i] << 8 | units[i + 1])) & 0xFFFFFFFF
    return h - (1 << 32) if h >> 31 else h


def check_join(j: dict, name: str, view: int = 2, online: bool = False) -> None:
    cfg, login = j["config"], j["login"]
    assert cfg["features"] == ["minecraft:vanilla"], cfg["features"]
    assert cfg["known_packs"] == [("minecraft", "core", META["version"])], cfg["known_packs"]
    assert len(cfg["registries"]) == 32, len(cfg["registries"])
    for key in ("minecraft:dimension_type", "minecraft:worldgen/biome", "minecraft:damage_type", "minecraft:enchantment"):
        assert cfg["registries"].get(key), f"{key} missing or empty"
    dims = cfg["registries"]["minecraft:dimension_type"]
    assert dims == mc.load_registry("dimension_type")
    assert cfg["tags"] is not None, "no update_tags packet: a vanilla client cannot leave configuration"
    for key in ("minecraft:block", "minecraft:item", "minecraft:worldgen/biome"):
        assert cfg["tags"].get(key), f"no tags for {key}"

    assert login["dimensions"] == ["minecraft:overworld"] and login["dimension"] == "minecraft:overworld"
    assert dims[login["dimension_type"]] == "minecraft:overworld"
    assert login["game_mode"] == 1 and login["previous_game_mode"] == -1
    assert login["online_mode"] == online and login["secure_chat"] is False
    # The flat generator: FlatLevelSource.getSeaLevel() is -63 (a negative VarInt), spawn on the grass.
    assert login["is_flat"] is True and login["sea_level"] == 0x100000000 - 63, login
    assert j["teleport"]["id"] == 1 and j["teleport"]["relative"] == 0
    assert j["teleport"]["pos"] == (8.5, -60.0, 8.5), j["teleport"]["pos"]
    assert j["abilities"][0] == 0x01 | 0x04 | 0x08, j["abilities"]  # invulnerable, may fly, instabuild - not flying
    assert j["game_event"] == (13, 0.0)
    # ServerTickRateManager.updateJoiningPlayer: the rate the world ticks at, not frozen, no steps.
    assert j["ticking"] == (float(TICK_RATE), False, 0), j["ticking"]

    cx, cz = j["center"]
    assert j["first_chunk"] == (cx, cz), "the player's own chunk must be sent first"
    # ChunkTrackingView: a square of the view distance + 1 with its corners cut (a square for small ones).
    assert set(j["chunks"]) == mc.view_chunks(cx, cz, view)
    assert len(j["chunks"]) == {2: 49, 3: 81, 10: 473}.get(view, len(j["chunks"])), len(j["chunks"])
    plains = mc.load_registry("worldgen_biome").index("minecraft:plains")
    for ch in j["chunks"].values():
        check_flat_chunk(ch, plains)


def check_flat_chunk(ch: dict, plains: int) -> None:
    """An untouched chunk of the default flat world: bedrock, 2 dirt, grass from y = -64, lit from the surface up."""
    # The three heightmaps a vanilla server sends: WORLD_SURFACE, MOTION_BLOCKING, MOTION_BLOCKING_NO_LEAVES.
    assert set(ch["heightmaps"]) == {1, 4, 5} and all(len(v) == 37 for v in ch["heightmaps"].values())
    for hm in ch["heightmaps"].values():
        assert {mc.heightmap_get(hm, i) for i in range(256)} == {4}  # top block at y = -61: -61 + 1 - (-64)
    assert all(s["biomes"]["palette"] == [plains] for s in ch["sections"])
    assert [s["non_air"] for s in ch["sections"]] == [4 * 256] + [0] * 23
    assert all(s["fluids"] == 0 for s in ch["sections"])
    assert all(s["blocks"]["bits"] == 0 and s["blocks"]["palette"] == [0] for s in ch["sections"][1:])
    ground = ch["sections"][0]["blocks"]
    assert ground["bits"] == 4 and len(ground["palette"]) == 4, (ground["bits"], ground["palette"])
    for x, z in ((0, 0), (15, 15), (7, 3)):
        column = [mc.chunk_block(ch, x, y, z) for y in range(-64, -58)]
        assert column == [mc.BLOCK["kBedrock"], mc.BLOCK["kDirt"], mc.BLOCK["kDirt"], mc.BLOCK["kGrassBlock"], 0, 0], column
    # Light sections start one below the world. Stored: the ground's section and its two neighbours.
    light = ch["light"]
    assert (light["sky_mask"], light["empty_sky"], light["block_mask"], light["empty_block"]) == (0b110, 0b001, 0, 0b111), light
    for y in range(16):
        assert mc.light_get(light["sky"][1], 5, y, 9) == (15 if y >= 4 else 0)
        assert mc.light_get(light["sky"][2], 5, y, 9) == 15


# ----------------------------------------------------------------------------- tests

@test(motd='Hello "world"\\!', max_players=123)
def status_ping(srv: Server):
    c = srv.client()
    c.handshake(1)
    c.send("status_request")
    status = json.loads(c.expect("status_response").str())
    assert status["version"] == {"name": META["version"], "protocol": META["protocol"]}, status
    assert status["players"] == {"max": 123, "online": 0}, status
    assert status["description"] == {"text": 'Hello "world"\\!'}, status
    c.send("ping_request", struct.pack(">q", 0x1122334455667788))
    assert c.expect("pong_response").i64() == 0x1122334455667788
    c.expect_closed()


@test(network_compression_threshold=-1)
def join_offline_uncompressed(srv: Server):
    c = srv.client()
    j = join(c, "Steve", view=10)     # (the default view of a vanilla server)
    assert j["profile"]["uuid"] == mc.offline_uuid("Steve") and j["profile"]["name"] == "Steve"
    assert c.threshold == -1 and not j["profile"]["encrypted"]
    assert j["config"]["brand"] == "vanilla"
    check_join(j, "Steve", view=10)
    assert [t["name"] for t in j["tab"]] == ["Steve"]


@test(alone=True, level_type="minecraft:normal", level_seed=20261123)
def a_world_of_terrain(srv: Server):
    """level-type=minecraft:normal: the overworld's terrain, made from the seed - not flat, the sea at
    63, a biome a cell. (What it is block for block is the unit tests' to say, against the jar; this
    is that a client is sent it, whole. By itself: making terrain takes every core the workers have,
    and the scenarios beside it are counting ticks.)"""
    c = srv.client()
    j = join(c, "Steve")
    login = j["login"]
    assert login["is_flat"] is False and login["sea_level"] == 63, login
    assert login["seed"] == hashed_seed(20261123), login["seed"]
    cx, cz = j["center"]
    assert set(j["chunks"]) == mc.view_chunks(cx, cz, 2)
    stone, water, lava = mc.BLOCK["kStone"], mc.BLOCK["kWater"], mc.BLOCK["kLava"]
    bedrock, deepslate = mc.BLOCK["kBedrock"], mc.BLOCK["kDeepslate"]
    biomes = mc.load_registry("worldgen_biome")
    seen, kinds = set(), set()
    for ch in j["chunks"].values():
        # Bedrock under everything, and at the top of the world nothing.
        column = [mc.chunk_block(ch, 8, y, 8) for y in range(-64, 320)]
        assert column[0] == bedrock and set(column[-40:]) == {0}, column
        kinds |= set(column)
        for section in ch["sections"]:
            for cell in range(64):
                seen.add(biomes[mc.paletted_get(section["biomes"], cell)])
    # Rock, deepslate below it, and on it the surface of its biome: something that is none of these.
    assert {stone, deepslate} <= kinds and kinds - {0, stone, deepslate, bedrock, water, lava}, kinds
    assert len(seen) > 1 and "minecraft:the_void" not in seen, seen


def flowing(ch: dict) -> bool:
    """Whether a chunk holds water or lava that has run: a state of them that is no source."""
    water, lava = mc.BLOCK["kWater"], mc.BLOCK["kLava"]
    for section in ch["sections"]:
        blocks = section["blocks"]
        states = blocks["palette"] if blocks["palette"] else {mc.paletted_get(blocks, i) for i in range(4096)}
        if any(water < s <= water + 15 or lava < s <= lava + 15 for s in states):
            return True
    return False


@test(alone=True)
def a_world_of_terrain_comes_back():
    """A world of terrain that is stopped and started again is the same world, chunk for chunk - it is
    read, not made anew - and what generation left to be done is done once: the water an aquifer left
    standing beside a cave runs when its chunk first ticks, and a chunk that has not ticked yet keeps
    its marks through the restart. (The game stands still: what the first tick of a fluid asks for is
    not run, so what a client is sent does not depend on when it looks.)"""
    world = tempfile.mkdtemp(prefix="world-terrain-")
    srv = None
    try:
        srv = Server(EXE, world=world, level_type="minecraft:normal", level_seed=7, simulation_distance=2)
        srv.console("tick freeze")
        srv.wait_log("The game is frozen")
        a = srv.client()
        ja = join(a, "Alice", view=6)
        a.send("player_loaded")
        settle(a)
        cx, cz = ja["center"]
        # Bob comes to look when all of it is there.
        b = srv.client()
        jb = join(b, "Bob", view=6)
        assert jb["center"] == (cx, cz)
        settle(b)
        before = frames(b)
        far = lambda pos: max(abs(pos[0] - cx), abs(pos[1] - cz))
        # Within the simulation distance and one chunk more the chunks run their ticks, and there
        # water has begun to run (in this world: a chunk north of the spawn); further out nothing
        # has, but for what ran across a border.
        ran = sorted(pos for pos in b.chunks if flowing(b.chunks[pos]))
        assert ran and all(far(pos) <= 4 for pos in ran), ran
        assert srv.stop() == 0, srv.read_log()
        os.unlink(srv.log.name)

        srv = Server(EXE, world=world, simulation_distance=2)     # (what kind of world it is, the world says)
        assert "a new" not in srv.read_log(), srv.read_log()
        srv.console("tick freeze")
        srv.wait_log("The game is frozen")
        c = srv.client()
        jc = join(c, "Carol", view=6)
        assert jc["center"] == (cx, cz) and jc["login"]["seed"] == hashed_seed(7) and jc["login"]["is_flat"] is False
        c.send("player_loaded")
        settle(c)
        after = frames(c)
        assert set(after) == set(before)
        differ = sorted(pos for pos in before if before[pos] != after[pos])
        assert not differ, f"chunks that came back different: {differ}"
        # Carol goes where no chunk has ticked yet: the marks those chunks were saved with are run
        # now (seven chunks east there is water that has somewhere to go).
        x, y, z = jc["teleport"]["pos"]
        c.send("move_player_pos", struct.pack(">dddB", x + 16.0 * 7, y, z, 1))
        c.sync_chunks(6, (cx + 7, cz))
        ran_now = {(pos[0] >> 4, pos[2] >> 4) for pos in collect(c)} | {pos for pos in c.chunks if flowing(c.chunks[pos])}
        assert any(pos[0] - cx >= 6 for pos in ran_now), sorted(ran_now)
        assert srv.stop() == 0, srv.read_log()
        os.unlink(srv.log.name)
    finally:
        if srv:
            srv.stop()
        shutil.rmtree(world, ignore_errors=True)


@test()
def join_offline_compressed(srv: Server):
    c = srv.client()
    j = join(c, "Alex", view=10)
    assert c.threshold == 256
    assert c.deflated > 0 and c.stored > 0, (c.deflated, c.stored)
    check_join(j, "Alex", view=10)


@test(view_distance=4)
def view_distance_is_min_of_client_and_server(srv: Server):
    a = srv.client()
    check_join(join(a, "Near", view=12), "Near", view=4)   # server caps the client
    b = srv.client()
    check_join(join(b, "Tiny", view=2), "Tiny", view=2)    # client asks for less


@test()
def join_online_encrypted():
    mock = MockSessionServer()
    srv = Server(EXE, online_mode="true", session_server=mock.url)
    try:
        c = srv.client()
        j = join(c, "Notch", online=True, view=10)
        prof = j["profile"]
        assert prof["encrypted"] and prof["should_authenticate"] is True
        assert prof["uuid"] == "069a79f444e94726a5befca90e38aaf5" and prof["name"] == "Notch"
        assert prof["props"] == [("textures", "dGV4", "c2ln")]
        # The server must have asked about exactly the hash this client computed independently.
        path, q = mock.requests[-1]
        assert path == "/session/minecraft/hasJoined"
        assert q["username"] == ["Notch"] and q["serverId"] == [prof["hash"]], (q, prof["hash"])
        check_join(j, "Notch", view=10, online=True)
        assert j["tab"][0]["props"] == [("textures", "dGV4", "c2ln")]

        d = srv.client()
        try:
            d.login("Denied", online=True)
            raise AssertionError("login of an unverified user succeeded")
        except ConnectionRefusedError as e:
            assert "Failed to verify username" in str(e), e
        d.expect_closed()
    finally:
        code = srv.stop()
        mock.stop()
        assert code == 0, f"server exit code {code}\n{srv.read_log()}"


@test()
def movement_streams_chunks(srv: Server):
    c = srv.client()
    j = join(c, "Walker", view=3)
    cx, cz = j["center"]
    # Walk +x across a chunk border: the view moves with the player (ChunkMap.move). The centre
    # first, then - at once - what left the view; what entered it comes in batches from this
    # tick's end on.
    before = set(c.chunks)
    assert before == mc.view_chunks(cx, cz, 3)
    mark = len(c.chunk_log)
    x, y, z = j["teleport"]["pos"]
    c.send("move_player_pos", struct.pack(">dddB", x + 16.0, y, z, 1))
    c.sync_chunks(3, (cx + 1, cz))
    after = mc.view_chunks(cx + 1, cz, 3)
    assert c.chunk_center == (cx + 1, cz) and set(c.chunks) == after
    log = c.chunk_log[mark:]
    assert log[0] == ("center", cx + 1, cz), log[0]
    gone = [e[1:] for e in log if e[0] == "forget"]
    assert set(gone) == before - after and len(gone) == len(before - after), gone
    assert gone == sorted(gone), "ChunkTrackingView.difference walks x, then z"
    new = [pos for e in log if e[0] == "batch" for pos in e[1]]
    assert set(new) == after - before and len(new) == len(after - before), new
    assert all(e[0] != "forget" for e in log[1 + len(gone):]), "what leaves the view is forgotten before anything is sent"
    # Within a batch the nearest first (PlayerChunkSender.collectChunksToSend).
    for e in log:
        if e[0] == "batch":
            dist = [(px - cx - 1) ** 2 + (pz - cz) ** 2 for px, pz in e[1]]
            assert dist == sorted(dist), dist
    # F3 ping graph
    c.send("ping_request", struct.pack(">q", 42))
    assert c.expect("pong_response").i64() == 42


@test()
def tab_list_and_duplicate_login(srv: Server):
    a = srv.client()
    join(a, "Alice")
    b = srv.client()
    jb = join(b, "Bob")
    assert [t["name"] for t in jb["tab"]] == ["Alice", "Bob"]
    r = a.expect("player_info_update")  # Alice learns about Bob
    r.u8(); r.varint(); r.uuid()
    assert r.str() == "Bob"

    # Same name logs in again: the old session is kicked, the new one is not.
    a2 = srv.client()
    ja2 = join(a2, "Alice")
    assert sorted(t["name"] for t in ja2["tab"]) == ["Alice", "Bob"]
    assert ja2["tab_packets"] == 2  # everyone already online arrives in ONE packet, then the newcomer itself
    assert a.expect("disconnect").nbt_text() == "You logged in from another location"
    a.expect_closed()
    # Bob sees old Alice leave, then new Alice arrive.
    r = b.expect("player_info_remove")
    assert r.varint() == 1 and r.uuid() == mc.offline_uuid("Alice")
    r = b.expect("player_info_update")
    r.u8(); r.varint(); r.uuid()
    assert r.str() == "Alice"

    # A client that just drops the connection is removed too.
    a2.close()
    r = b.expect("player_info_remove")
    assert r.varint() == 1 and r.uuid() == mc.offline_uuid("Alice")
    c = srv.client()
    c.handshake(1)
    c.send("status_request")
    assert json.loads(c.expect("status_response").str())["players"]["online"] == 1


@test(max_players=1)
def server_full(srv: Server):
    a = srv.client()
    join(a, "First")
    b = srv.client()
    try:
        b.login("Second")
        raise AssertionError("second player got into a full server")
    except ConnectionRefusedError as e:
        assert "full" in str(e), e
    b.expect_closed()


@test()
def rejects_wrong_version_and_bad_input(srv: Server):
    c = srv.client()
    c.handshake(2, protocol=META["protocol"] - 1)
    c.send("hello", mc.mc_str("Old") + bytes(16))
    msg = json.loads(c.expect("login_disconnect").str())["text"]
    assert msg == f"Outdated client! Please use {META['version']}", msg
    c.expect_closed()

    c = srv.client()
    c.handshake(2, protocol=META["protocol"] + 1)
    msg = json.loads(c.expect("login_disconnect").str())["text"]
    assert msg.startswith("Outdated server!"), msg

    # Zero length, over-long length varint, unknown handshake id, oversized frame, noise.
    for garbage in (b"\x00", b"\xff\xff\xff\xff\xff", b"\x02\x55\x00", b"\xff\x7f" + bytes(64), os.urandom(4096)):
        c = srv.client()
        c.send_raw(garbage)
        c.expect_closed()

    c = srv.client()  # invalid player name
    c.handshake(2)
    c.send("hello", mc.mc_str("has space") + bytes(16))
    c.expect_closed()

    c = srv.client()  # login packets out of order
    c.handshake(2)
    c.send("login_acknowledged")
    c.expect_closed()

    c = srv.client()  # client without the vanilla core pack is told why
    c.login("NoPack")
    try:
        c.configure(known_packs=[])
        raise AssertionError("configuration succeeded without known packs")
    except ConnectionRefusedError as e:
        assert "minecraft:core" in str(e), e

    check_join(join(srv.client(), "StillAlive"), "StillAlive")  # and the server is still fine


@test(login_timeout=1)
def stalled_login_times_out(srv: Server):
    """A connection that never completes a frame (slow loris, port scanner, HTTP probe) is dropped."""
    for partial in (b"\x05\x00\x01", b"GET / HTTP/1.1\r\n\r\n"):
        c = srv.client()
        t0 = time.time()
        c.send_raw(partial)
        c.expect_closed()
        assert 0.5 < time.time() - t0 < 3.5, time.time() - t0
    c = srv.client()  # ...and so is one that completes the handshake but never logs in
    c.handshake(2)
    c.expect_closed()


@test()
def split_and_coalesced_writes(srv: Server):
    """Frames cut at every possible byte boundary, and many frames in one segment."""
    c = srv.client()
    hs = varint(META["protocol"]) + mc.mc_str("localhost") + struct.pack(">H", 25565) + varint(1)
    f1 = varint(len(hs) + 1) + b"\x00" + hs
    f2 = varint(1) + b"\x00"
    f3 = varint(9) + b"\x01" + struct.pack(">q", 7)
    for b in f1 + f2:
        c.send_raw(bytes([b]))
        time.sleep(0.001)
    c.state = "status"
    assert json.loads(c.expect("status_response").str())["version"]["protocol"] == META["protocol"]
    c.send_raw(f3)
    assert c.expect("pong_response").i64() == 7

    c = srv.client()
    c.send_raw(f1 + f2 + f3)  # everything at once
    c.state = "status"
    c.expect("status_response")
    assert c.expect("pong_response").i64() == 7


# ----------------------------------------------------------------------------- blocks

def give(c: Client, slot: int, item: int, count: int = 1) -> None:
    """Creative inventory: put a stack into a slot of the inventory menu (36..44 = hotbar, 45 = off hand)."""
    c.send("set_creative_mode_slot", struct.pack(">h", slot) + mc.item_stack(item, count))


def click(c: Client, pos: tuple, face: int, seq: int, hand: int = 0, cursor: tuple = (0.5, 1.0, 0.5)) -> None:
    """use_item_on: right-click a face of a block (0 down, 1 up, 2 north, 3 south, 4 west, 5 east)."""
    c.send("use_item_on", varint(hand) + mc.pack_block_pos(*pos) + varint(face) + struct.pack(">fff", *cursor) +
           b"\x00\x00" + varint(seq))


def dig(c: Client, pos: tuple, seq: int, action: int = 0, face: int = 1) -> None:
    """player_action; action 0 = START_DESTROY_BLOCK, which is all a creative client sends to break a block."""
    c.send("player_action", varint(action) + mc.pack_block_pos(*pos) + bytes([face]) + varint(seq))


def expect_block(c: Client, pos: tuple, state: int) -> None:
    got = mc.parse_block_update(c.expect("block_update"))
    assert got == (pos, state), f"block_update {got}, expected {(pos, state)}"


def expect_ack(c: Client, seq: int) -> None:
    r = c.expect("block_changed_ack")
    assert r.varint() == seq
    r.done()


def expect_quiet(c: Client) -> None:
    """Nothing is on its way to this client: the next packet it gets is the answer to a ping sent now."""
    token = int(time.time() * 1000) & 0xFFFFFFFF
    c.send("ping_request", struct.pack(">q", token))
    assert c.expect("pong_response").i64() == token


def skip_tab_updates(c: Client, n: int) -> None:
    for _ in range(n):
        c.expect("player_info_update")


def walk_to_chunk(c: Client, j: dict, x: float, z: float, view: int) -> None:
    """Move and take the chunk stream that follows; returns once the view is settled."""
    c.send("move_player_pos", struct.pack(">dddB", x, j["teleport"]["pos"][1], z, 1))
    new = (int(x // 16), int(z // 16))
    c.sync_chunks(j["view"], new)
    # ...and, as a client would, says where it is again: what stands in the new chunks is shown.
    c.send("move_player_pos", struct.pack(">dddB", x, j["teleport"]["pos"][1], z, 1))
    j["center"] = new


GRASS_TOP = (0.5, 1.0, 0.5)  # hit location for a click on the middle of a block's top face
STONE, GLOW = "kStone", "kGlowstone"


@test()
def place_and_break_blocks(srv: Server):
    """A places and breaks; B stands next to A; C has A's chunk on the very edge of its view."""
    a, b, c = srv.client(), srv.client(), srv.client()
    ja = join(a, "Alice")
    join(b, "Bob")
    jc = join(c, "Carol", view=2)
    skip_tab_updates(a, 2)
    skip_tab_updates(b, 1)
    for cl in (a, b, c):
        cl.send("player_loaded")
    walk_to_chunk(c, jc, -40.0, 8.5, view=2)  # chunk (-3, 0): chunk (0, 0) is now on the border of Carol's view

    ground = (8, -61, 10)                      # a grass block two steps from Alice
    above = (8, -60, 10)
    give(a, 36, mc.ITEM["kGlowstone"])
    a.send("set_carried_item", struct.pack(">h", 0))
    click(a, ground, 1, seq=1)

    # Alice: the two blocks her click could have touched (straight from the handler), then the change
    # itself like every other viewer, then the acknowledgement of her prediction.
    expect_block(a, ground, mc.BLOCK["kGrassBlock"])
    expect_block(a, above, mc.BLOCK["kGlowstone"])
    expect_block(a, above, mc.BLOCK["kGlowstone"])
    expect_ack(a, 1)
    # Bob sees the block; his client relights its own copy, so no light is sent to him.
    expect_block(b, above, mc.BLOCK["kGlowstone"])
    expect_quiet(b)
    # Carol has chunk (0, 0) at the edge of her view and cannot relight it herself: she gets the
    # light of every border chunk that changed (the glowstone reaches z = -4 and z = 24), then the block.
    lit = {}
    for _ in range(3):
        u = mc.parse_light_update(c.expect("light_update"))
        lit[(u["x"], u["z"])] = u
    assert set(lit) == {(0, 0), (0, -1), (0, 1)}, set(lit)
    here = lit[(0, 0)]
    assert here["block_mask"] == 0b110 and here["sky_mask"] == 0b010, (bin(here["block_mask"]), bin(here["sky_mask"]))
    assert mc.light_get(here["block"][1], 8, 4, 10) == 15       # the glowstone itself (y = -60 is local y 4)
    assert mc.light_get(here["block"][1], 8, 5, 10) == 14
    assert mc.light_get(here["block"][1], 11, 4, 10) == 12
    assert mc.light_get(here["block"][2], 8, 0, 10) == 3        # y = -48: twelve blocks up
    assert mc.light_get(here["sky"][1], 8, 4, 10) == 0          # opaque: no sky light inside it
    assert mc.light_get(here["sky"][1], 8, 5, 10) == 15
    assert lit[(0, 1)]["block_mask"] == 0b010 and mc.light_get(lit[(0, 1)]["block"][1], 8, 4, 0) == 9  # z = 16
    expect_block(c, above, mc.BLOCK["kGlowstone"])
    expect_quiet(c)

    # Someone who joins now gets the chunk with the block and its light already in it (the cached
    # frame was thrown away), and the heightmaps know the column grew.
    d = srv.client()
    jd = join(d, "Dave", view=2)
    skip_tab_updates(a, 1); skip_tab_updates(b, 1); skip_tab_updates(c, 1)
    ch = jd["chunks"][(0, 0)]
    assert mc.chunk_block(ch, 8, -60, 10) == mc.BLOCK["kGlowstone"]
    assert ch["sections"][0]["non_air"] == 4 * 256 + 1
    assert all(mc.heightmap_get(hm, 8 + 10 * 16) == 5 for hm in ch["heightmaps"].values())
    assert ch["light"]["block_mask"] == 0b110 and mc.light_get(ch["light"]["block"][1], 8, 4, 10) == 15
    assert mc.light_get(ch["light"]["sky"][1], 8, 4, 10) == 0
    plains = mc.load_registry("worldgen_biome").index("minecraft:plains")
    check_flat_chunk(jd["chunks"][(2, 2)], plains)  # light never got that far
    d.close()
    for cl in (a, b, c):
        cl.expect("player_info_remove")

    # Break it again (creative: one START_DESTROY_BLOCK and the block is gone).
    dig(a, above, seq=2)
    expect_block(a, above, 0)
    expect_ack(a, 2)
    expect_block(b, above, 0)
    dark = {}
    for _ in range(3):
        u = mc.parse_light_update(c.expect("light_update"))
        dark[(u["x"], u["z"])] = u
    assert set(dark) == {(0, 0), (0, -1), (0, 1)}
    # The layers were written, so vanilla keeps sending them as arrays (now all zero), not as "empty".
    assert dark[(0, 0)]["block_mask"] == 0b110 and set(dark[(0, 0)]["block"][1]) == {0}
    assert mc.light_get(dark[(0, 0)]["sky"][1], 8, 4, 10) == 15
    expect_block(c, above, 0)
    expect_quiet(a); expect_quiet(b); expect_quiet(c)


@test()
def placement_rules(srv: Server):
    a, b = srv.client(), srv.client()
    # A small view: the sixty ticks below run from the moment Alice joined, and the 441 chunks of
    # the default view take a sanitized server longer than that to make and send.
    join(a, "Alice", view=2)
    grass, air = mc.BLOCK["kGrassBlock"], 0

    # Until the client says it finished loading (or 60 ticks pass) block actions are ignored - no
    # acknowledgement either. (Before Bob joins: on a busy machine that alone is sixty ticks.)
    give(a, 36, mc.ITEM["kStone"])
    click(a, (8, -61, 10), 1, seq=1)
    dig(a, (8, -61, 10), seq=2)
    expect_quiet(a)
    join(b, "Bob", view=2)
    skip_tab_updates(a, 1)
    a.send("player_loaded")
    b.send("player_loaded")

    # Out of reach (creative: 5 blocks, the server allows 1 more): acknowledged, nothing else.
    click(a, (8, -61, 40), 1, seq=3)
    expect_ack(a, 3)
    # In reach again: several actions in one tick are acknowledged once, with the highest sequence.
    seq = 4
    click(a, (10, -61, 10), 1, seq=seq)      # stone on the grass at (10, -60, 10)
    expect_block(a, (10, -61, 10), grass)
    expect_block(a, (10, -60, 10), mc.BLOCK["kStone"])
    expect_block(a, (10, -60, 10), mc.BLOCK["kStone"])
    expect_ack(a, seq)
    expect_block(b, (10, -60, 10), mc.BLOCK["kStone"])

    # Cannot build where a player stands (both are at 8.5, -60, 8.5).
    seq += 1
    click(a, (8, -61, 8), 1, seq=seq)
    expect_block(a, (8, -61, 8), grass)
    expect_block(a, (8, -60, 8), air)
    expect_ack(a, seq)
    expect_quiet(b)

    # A log takes its axis from the clicked face: against the east side of the stone it lies along x.
    give(a, 37, mc.ITEM["kOakLog"])
    a.send("set_carried_item", struct.pack(">h", 1))
    seq += 1
    click(a, (10, -60, 10), 5, seq=seq, cursor=(1.0, 0.5, 0.5))
    log_x = mc.BLOCK["kOakLog"] - 1  # states are ordered axis = x, y (default), z
    expect_block(a, (10, -60, 10), mc.BLOCK["kStone"])
    expect_block(a, (11, -60, 10), log_x)
    expect_block(a, (11, -60, 10), log_x)
    expect_ack(a, seq)
    expect_block(b, (11, -60, 10), log_x)

    # Snow on grass: the grass below turns snowy. Two changes in one section travel as one packet.
    give(a, 38, mc.ITEM["kSnowBlock"])
    a.send("set_carried_item", struct.pack(">h", 2))
    seq += 1
    click(a, (8, -61, 11), 1, seq=seq)
    snowy = grass - 1  # snowy=true comes before the default snowy=false
    both = {(8, -60, 11): mc.BLOCK["kSnowBlock"], (8, -61, 11): snowy}
    expect_block(a, (8, -61, 11), snowy)
    expect_block(a, (8, -60, 11), mc.BLOCK["kSnowBlock"])
    assert mc.parse_section_blocks_update(a.expect("section_blocks_update")) == both
    expect_ack(a, seq)
    assert mc.parse_section_blocks_update(b.expect("section_blocks_update")) == both
    seq += 1
    dig(a, (8, -60, 11), seq=seq)  # and back
    back = {(8, -60, 11): air, (8, -61, 11): grass}
    assert mc.parse_section_blocks_update(a.expect("section_blocks_update")) == back
    expect_ack(a, seq)
    assert mc.parse_section_blocks_update(b.expect("section_blocks_update")) == back

    # A block whose placement needs behaviour that is not written yet (a lever: what it is on, its
    # signal) is not placed at all; the client is told what is really there.
    give(a, 39, mc.ITEM["kLever"])
    a.send("set_carried_item", struct.pack(">h", 3))
    seq += 1
    click(a, (8, -61, 12), 1, seq=seq)
    expect_block(a, (8, -61, 12), grass)
    expect_block(a, (8, -60, 12), air)
    expect_ack(a, seq)
    expect_quiet(b)

    # A sword in hand does not break blocks in creative: the client's prediction is undone.
    give(a, 40, mc.ITEM["kDiamondSword"])
    a.send("set_carried_item", struct.pack(">h", 4))
    seq += 1
    dig(a, (10, -60, 10), seq=seq)
    expect_block(a, (10, -60, 10), mc.BLOCK["kStone"])
    expect_ack(a, seq)
    expect_quiet(b)

    # Empty hand: the click does nothing, the block breaks.
    a.send("set_carried_item", struct.pack(">h", 8))
    seq += 1
    click(a, (10, -60, 10), 1, seq=seq)
    expect_block(a, (10, -60, 10), mc.BLOCK["kStone"])
    expect_block(a, (10, -59, 10), air)
    expect_ack(a, seq)
    seq += 1
    dig(a, (10, -60, 10), seq=seq)
    expect_block(a, (10, -60, 10), air)
    expect_ack(a, seq)
    expect_block(b, (10, -60, 10), air)
    # Bedrock is just a block to a creative player; below it there is nothing to break.
    seq += 1
    dig(a, (8, -61, 9), seq=seq)
    dig(a, (8, -62, 9), seq=seq + 1)
    dig(a, (8, -63, 9), seq=seq + 2)
    dig(a, (8, -64, 9), seq=seq + 3)
    dig(a, (8, -65, 9), seq=seq + 4)
    changes = {}
    while len(changes) < 4:
        got, r = a.recv()
        if got == "block_update":
            pos, state = mc.parse_block_update(r)
            changes[pos] = state
        else:
            assert got == "section_blocks_update", got
            changes.update(mc.parse_section_blocks_update(r))
    assert changes == {(8, y, 9): air for y in (-61, -62, -63, -64)}, changes
    expect_ack(a, seq + 4)
    expect_quiet(a)


# ----------------------------------------------------------------------------- entities

BYTE, FLOAT, POSE_T, ARM = mc.SERIALIZER["kByte"], mc.SERIALIZER["kFloat"], mc.SERIALIZER["kPose"], mc.SERIALIZER["kHumanoidArm"]
D_FLAGS, D_POSE, D_HEALTH = mc.DATA["kSharedFlags"], mc.DATA["kPose"], mc.DATA["kHealth"]
D_HAND, D_SKIN = mc.DATA["kPlayerMainHand"], mc.DATA["kPlayerModeCustomisation"]
# What everyone is told about a player that just stands there: full health and the skin layers its
# client asked for (mcproto sends 0x7F). The same two entries a vanilla server sends.
FRESH_PLAYER = {D_HEALTH: (FLOAT, 20.0), D_SKIN: (BYTE, 0x7F)}
SPAWN = (8.5, -60.0, 8.5)


def settle(*clients: Client, ticks: int = 4) -> None:
    """Whatever the server decided because of packets already sent has reached these clients.

    A ping is answered in the tick that reads it, ahead of that tick's entity updates. Once every
    client has its first answer, the packets sent before are in; each further round is one more
    whole tick, and three cover the slowest reaction there is (an update interval of 2, then a
    pose that follows a tick later)."""
    for i in range(ticks):
        for c in clients:
            c.send("ping_request", struct.pack(">q", 0x5E77 + i))
        for c in clients:
            assert c.expect("pong_response").i64() == 0x5E77 + i


def walk(c: Client, j: dict, x: float, z: float, view: int, y: float = SPAWN[1], ground: bool = True) -> None:
    """Move to (x, y, z); if that crosses a chunk border, take the chunks that come with it."""
    if (int(x // 16), int(z // 16)) != tuple(j["center"]):
        assert y == SPAWN[1] and ground
        walk_to_chunk(c, j, x, z, view)
    else:
        c.send("move_player_pos", struct.pack(">dddB", x, y, z, 1 if ground else 0))


def look(c: Client, yaw: float, pitch: float) -> None:
    c.send("move_player_rot", struct.pack(">ffB", yaw, pitch, 1))


DOWN = 90.0     # the pitch of a player that looks at its feet: what it drops lands there


def log_of(c: Client, eid: int, since: int = 0) -> list:
    return [(name, what) for name, e, what in c.entity_log[since:] if e == eid]


@test()
def players_see_each_other(srv: Server):
    a, b = srv.client(), srv.client()
    ja = join(a, "Alice")
    ida = ja["login"]["entity_id"]
    a.ticks(3)
    # Alone in the world: the only entity news is about oneself, once - its permission level
    # (entity event 24: none) at the join, then its own data.
    assert a.own_data == FRESH_PLAYER, a.own_data
    assert not a.entities and [e[:1] + e[2:] if e[0] == "entity_event" else e[:1] for e in a.entity_log] == [
        ("entity_event", 24), ("set_entity_data",)], a.entity_log

    jb = join(b, "Bob")
    idb = jb["login"]["entity_id"]
    assert idb != ida
    # The tab-list entry first: a client cannot create a player it has no entry for.
    a.expect("player_info_update")
    assert not a.entities
    settle(b, a)
    assert set(a.entities) == {idb} and set(b.entities) == {ida}, (set(a.entities), set(b.entities))
    eb, ea = a.entities[idb], b.entities[ida]
    assert eb.uuid == mc.offline_uuid("Bob") and ea.uuid == mc.offline_uuid("Alice")
    assert eb.type == mc.ENTITY_TYPE["kPlayer"] and eb.object_data == 0 and eb.velocity == (0.0, 0.0, 0.0)
    assert eb.pos == SPAWN and ea.pos == SPAWN and (eb.yaw, eb.pitch, eb.head) == (0.0, 0.0, 0)
    assert eb.data == FRESH_PLAYER and ea.data == FRESH_PLAYER and b.own_data == FRESH_PLAYER
    # A sees Bob appear as vanilla shows it: the bundle, then Bob's first update - a zero move
    # (every 60th tick of an entity's life sends its position, tick 0 included) and its data.
    assert [n for n, _ in log_of(a, idb)] == ["add_entity", "set_entity_data", "move_entity_pos", "set_entity_data"], log_of(a, idb)
    names = [e[0] for e in a.entity_log]
    at = names.index("add_entity")
    assert names[at - 1:at + 3] == ["bundle_delimiter", "add_entity", "set_entity_data", "bundle_delimiter"] and eb.bundled
    assert log_of(a, idb)[2] == ("move_entity_pos", (0, SPAWN)) and not eb.on_ground  # no steps yet: a plain delta

    # ---- Bob walks. The first packet says "on the ground", which he was not: that goes out as an
    # absolute position; after it, deltas, each one step that took as many ticks as the update is apart.
    mark = len(a.entity_log)
    x, y, z = SPAWN
    for _ in range(8):
        x += 0.25
        b.send("move_player_pos", struct.pack(">dddB", x, y, z, 1))
        b.ticks(2)
    settle(b, a)
    assert eb.pos == (x, y, z) and eb.on_ground, eb.pos
    moves = log_of(a, idb, mark)
    assert moves[0][0] == "entity_position_sync" and moves[0][1][0] == 1, moves
    assert {n for n, _ in moves[1:]} == {"move_entity_pos"} and all(steps == 1 for _, (steps, _) in moves[1:]), moves
    assert set(eb.steps) <= {1, 2}, eb.steps
    assert not log_of(b, ida, 0)[4:], "Alice did nothing, and still Bob was told something"

    # ---- Bob turns: the body at once, the head after his own tick has copied it.
    mark = len(a.entity_log)
    b.send("move_player_rot", struct.pack(">ffB", 90.0, -20.0, 1))
    settle(b, a)
    assert (eb.yaw, eb.pitch, eb.head) == (90.0, -15 * 360 / 256, 64), (eb.yaw, eb.pitch, eb.head)
    assert [n for n, _ in log_of(a, idb, mark)] == ["move_entity_rot", "rotate_head"], log_of(a, idb, mark)
    # Angles are wrapped and the pitch clamped before anyone is told.
    b.send("move_player_pos_rot", struct.pack(">dddffB", x, y, z + 0.5, 270.0, 123.0, 1))
    z += 0.5
    settle(b, a)
    assert (eb.yaw, eb.pitch, eb.head) == (-90.0, 90.0, -64) and eb.pos == (x, y, z), (eb.yaw, eb.pitch, eb.head, eb.pos)

    # ---- sneaking: the key is a flag at once; the pose follows from Bob's own tick, one tick later.
    for cl in (a, b):
        cl.send("player_loaded")
    mark = len(a.entity_log)
    b.send("player_input", bytes([0x20]))
    settle(b, a)
    assert [w for n, w in log_of(a, idb, mark)] == [{D_FLAGS: (BYTE, 0x02)}, {D_POSE: (POSE_T, mc.POSE["kCrouching"])}], log_of(a, idb, mark)
    assert b.own_data[D_FLAGS] == (BYTE, 0x02) and b.own_data[D_POSE] == (POSE_T, mc.POSE["kCrouching"])  # Bob is told too
    b.send("player_input", bytes([0x00]))
    b.send("player_command", varint(idb) + varint(1) + varint(0))  # START_SPRINTING
    settle(b, a)
    assert eb.data[D_FLAGS] == (BYTE, 0x08) and eb.data[D_POSE] == (POSE_T, mc.POSE["kStanding"]), eb.data
    b.send("player_command", varint(idb) + varint(2) + varint(0))
    # Options changed in play: left-handed, no cape.
    b.send("client_information", mc.mc_str("en_us") + struct.pack(">b", 10) + varint(0) + b"\x01" + b"\x7e" + varint(0) +
           b"\x00\x01" + varint(0))
    settle(b, a)
    assert eb.data[D_FLAGS] == (BYTE, 0) and eb.data[D_HAND] == (ARM, 0) and eb.data[D_SKIN] == (BYTE, 0x7E), eb.data

    # ---- too far for a delta of shorts (8 blocks): an absolute position, exact to the last bit.
    mark = len(a.entity_log)
    x -= 9.3  # stays inside the chunk: no chunk traffic to wade through
    b.send("move_player_pos", struct.pack(">dddB", x, y, z, 1))
    settle(b, a)
    assert eb.pos == (x, y, z) and [n for n, _ in log_of(a, idb, mark)] == ["entity_position_sync"], log_of(a, idb, mark)
    # ...and leaving the ground is one too, whatever the distance.
    mark = len(a.entity_log)
    b.send("move_player_pos", struct.pack(">dddB", x, y + 0.5, z, 0))
    settle(b, a)
    assert eb.pos == (x, y + 0.5, z) and not eb.on_ground and log_of(a, idb, mark)[0][0] == "entity_position_sync"

    # ---- someone who joins now is shown Bob as he is: where his viewers believe him, turned, left-handed.
    c = srv.client()
    jc = join(c, "Carol")
    idc = jc["login"]["entity_id"]
    skip_tab_updates(a, 1)
    skip_tab_updates(b, 1)
    settle(c, a, b)
    assert set(c.entities) == {ida, idb} and set(a.entities) == {idb, idc}
    cb_ = c.entities[idb]
    assert cb_.pos == eb.pos and (cb_.yaw, cb_.pitch, cb_.head) == (eb.yaw, eb.pitch, eb.head), (cb_.pos, cb_.yaw, cb_.pitch, cb_.head)
    # Only what differs from a fresh entity is sent: no flags, no pose - Bob stands again.
    assert cb_.data == {D_HEALTH: (FLOAT, 20.0), D_HAND: (ARM, 0), D_SKIN: (BYTE, 0x7E)}, cb_.data

    # ---- Bob leaves: the entity goes, then the tab-list entry.
    b.close()
    a.expect("player_info_remove")
    assert set(a.entities) == {idc}
    c.expect("player_info_remove")
    assert set(c.entities) == {ida}


@test(view_distance=2)
def entities_enter_and_leave_view(srv: Server):
    """With a view of 2 chunks, entities are shown up to 32 blocks away (horizontally, inclusive)."""
    a, b = srv.client(), srv.client()
    ja = join(a, "Alice", view=2)
    jb = join(b, "Bob", view=2)
    ida, idb = ja["login"]["entity_id"], jb["login"]["entity_id"]
    a.expect("player_info_update")
    settle(b, a)
    assert idb in a.entities and ida in b.entities

    # Bob walks away along x; Alice never sends a thing.
    x = SPAWN[0]
    for step in range(8):
        x += 4.0
        walk(b, jb, x, SPAWN[2], view=2)
        settle(b, a)
        assert idb in a.entities and ida in b.entities, f"lost sight at {x - SPAWN[0]} blocks"
        assert a.entities[idb].pos == (x, SPAWN[1], SPAWN[2])
    assert x - SPAWN[0] == 32.0
    walk(b, jb, x + 0.5, SPAWN[2], view=2)
    settle(b, a)
    assert not a.entities and not b.entities, "still visible at 32.5 blocks"
    assert a.entity_log[-1] == ("remove_entities", idb, None) and b.entity_log[-1] == ("remove_entities", ida, None)
    # Height does not count.
    b.send("move_player_pos", struct.pack(">dddB", x, SPAWN[1] + 100.0, SPAWN[2], 0))
    settle(b, a)
    assert idb in a.entities and ida in b.entities
    eb = a.entities[idb]
    assert eb.pos == (x, SPAWN[1] + 100.0, SPAWN[2]) and eb.bundled and eb.data == FRESH_PLAYER, eb.pos
    # Out again (one block is enough), and this time it is Alice who walks up to Bob.
    b.send("move_player_pos", struct.pack(">dddB", x + 1.0, SPAWN[1] + 100.0, SPAWN[2], 0))
    settle(b, a)
    assert not a.entities and not b.entities
    walk(a, ja, SPAWN[0] + 8.0, SPAWN[2], view=2)
    settle(a, b)
    assert idb in a.entities and ida in b.entities
    assert a.entities[idb].pos == (x + 1.0, SPAWN[1] + 100.0, SPAWN[2]) and b.entities[ida].pos == (SPAWN[0] + 8.0, SPAWN[1], SPAWN[2])

    # Entity ids are reused (they index a table): whoever joins after Bob left gets his.
    b.close()
    a.expect("player_info_remove")
    assert not a.entities
    c = srv.client()
    jc = join(c, "Carol", view=2)
    assert jc["login"]["entity_id"] == idb
    a.expect("player_info_update")
    settle(c, a)
    assert a.entities[idb].uuid == mc.offline_uuid("Carol") and a.entities[idb].pos == SPAWN


# ----------------------------------------------------------------------------- inventory

PICKUP, QUICK_MOVE, SWAP, CLONE, THROW, QUICK_CRAFT, PICKUP_ALL = range(7)   # ContainerInput
OUTSIDE = -999
DROP_ALL, DROP_ONE, SWAP_HANDS = 4, 5, 7                                      # ServerboundPlayerActionPacket.Action
MAINHAND, OFFHAND, FEET, LEGS, CHEST, HEAD = (mc.EQUIPMENT_SLOT[k] for k in ("kMainhand", "kOffhand", "kFeet", "kLegs", "kChest", "kHead"))
I = mc.ITEM
D_ITEM = mc.ITEM_DATA["kItem"]
BOOL, STACK = mc.SERIALIZER["kBoolean"], mc.SERIALIZER["kItemStack"]
EYE = 1.62            # a standing player's eyes; an item appears 0.3 (as a float) below them
DROP_Y = SPAWN[1] + struct.unpack(">f", struct.pack(">f", EYE))[0] - struct.unpack(">f", struct.pack(">f", 0.3))[0]


def plain(item: str, count: int = 1) -> tuple:
    """A stack without components, as the test client reads it."""
    return (I[item], count, {})


def menu_click(c: Client, slot: int, button: int, kind: int, changed: dict | None = None, carried=None, state: int | None = None) -> None:
    """container_click on the menu that is open - the player's own, or the one a block opened.
    `changed` and `carried` are what a client reports of its own prediction: {slot: (item, count)
    or None}. Reporting nothing makes the server describe everything the click did."""
    changed = changed or {}
    body = varint(c.window["id"] if c.window else 0) + varint(c.state_id if state is None else state)
    body += struct.pack(">hb", slot, button) + varint(kind)
    body += varint(len(changed)) + b"".join(struct.pack(">h", k) + mc.hashed_stack(v) for k, v in changed.items())
    c.send("container_click", body + mc.hashed_stack(carried))


def action(c: Client, kind: int) -> None:
    c.send("player_action", varint(kind) + mc.pack_block_pos(0, 0, 0) + b"\x00" + varint(0))


def server_menu(c: Client) -> list:
    """The menu as the server holds it: a click that does nothing, with a state id the server
    has never used, makes it send everything (handleContainerClick: broadcastFullState)."""
    before = len(c.menu_log)
    menu_click(c, -1, 0, PICKUP, carried=c.cursor[:2] if c.cursor else None, state=(c.state_id + 1000) & 32767)
    settle(c, ticks=2)
    assert [m[0] for m in c.menu_log[before:]] == ["container_set_content"], c.menu_log[before:]
    return c.slots


def expect_kick(c: Client, why: str) -> None:
    got, r = c.recv()
    assert got == "disconnect", f"{why}: expected a disconnect, got {got}"
    c.expect_closed()


@test()
def creative_inventory(srv: Server):
    c = srv.client()
    join(c, "Alice")
    c.send("player_loaded")
    # The join: the hotbar slot in hand right after the abilities, the whole (empty) menu at the end.
    assert c.menu_log == [("set_held_slot", 0), ("container_set_content", 1)], c.menu_log
    assert c.slots == [None] * 46 and c.cursor is None and c.held_slot == 0

    # The creative inventory screen owns its slots and tells the server: nothing comes back -
    # but for the crafting result, which the server says again whenever the grid changes.
    give(c, 36, I["kStone"], 64)
    give(c, 9, I["kDirt"], 3)
    give(c, 1, I["kGlowstone"], 5)    # the crafting grid holds items too
    give(c, 6, I["kStone"], 2)        # ...and it may put anything anywhere, armour slots included
    give(c, 0, I["kStone"], 1)        # but not into the crafting result (slots 1..45 only)
    give(c, 46, I["kStone"], 1)
    give(c, 37, I["kStone"], 65)      # more than a stack of it: ignored
    give(c, 38, I["kEnderPearl"], 17)
    give(c, 38, I["kEnderPearl"], 16)
    settle(c)
    assert c.menu_log[2:] == [("container_set_slot", 0, None)] and not c.sounds, (c.menu_log, c.sounds)   # glowstone makes nothing
    slots = server_menu(c)
    held = {i: s for i, s in enumerate(slots) if s}
    assert held == {1: plain("kGlowstone", 5), 6: plain("kStone", 2), 9: plain("kDirt", 3), 36: plain("kStone", 64),
                    38: plain("kEnderPearl", 16)}, held
    give(c, 9, 0, 0)                  # an empty stack empties the slot
    give(c, 6, I["kAir"], 5)          # so does air
    settle(c)
    assert {i for i, s in enumerate(server_menu(c)) if s} == {1, 36, 38}

    # Components the server reads are kept and come back as vanilla would write them: here a
    # damaged sword with a padded VarInt, out of order with its repair cost.
    c.send("set_creative_mode_slot", struct.pack(">h", 40) + mc.item_stack(
        I["kDiamondSword"], 1, {"kRepairCost": varint(3), "kDamage": b"\x85\x00"}, removed=("kLore",)))
    potion = {"kPotionContents": b"\x01" + varint(7) + b"\x00" + varint(0) + b"\x00"}
    c.send("set_creative_mode_slot", struct.pack(">h", 41) + mc.item_stack(I["kPotion"], 1, potion))
    settle(c)
    slots = server_menu(c)
    assert slots[40] == (I["kDiamondSword"], 1, {"kDamage": 5, "kRepairCost": 3, "kLore": None}), slots[40]
    assert slots[41] == (I["kPotion"], 1, {"kPotionContents": {"potion": 7, "color": None, "effects": [], "name": None}}), slots[41]

    # Text and NBT are tags, and a tag has many spellings: what comes back is the one vanilla
    # writes - a name's entries in the order of its HashMap, custom data without the repeat.
    name = {"italic": mc.Byte(0), "text": "Excalibur", "color": "#ffaa00", "unknown": 1}
    lore = [{"text": "Forged"}, ["in ", {"text": "fire", "color": "red"}]]
    data = mc.nbt({"uses": 3, "owner": "Alice"})[:-1] + b"\x03\x00\x04uses\x00\x00\x00\x04\x00"   # ...and "uses" again
    c.send("set_creative_mode_slot", struct.pack(">h", 43) + mc.item_stack(I["kDiamondSword"], 1, {
        "kCustomName": mc.nbt(name), "kLore": varint(2) + mc.nbt(lore[0]) + mc.nbt(lore[1]), "kCustomData": data}))
    settle(c)
    comps = server_menu(c)[43][2]
    assert mc.nbt(comps["kCustomName"]) == mc.nbt({"color": "#FFAA00", "text": "Excalibur", "italic": mc.Byte(0)}), comps
    assert [mc.nbt(line) for line in comps["kLore"]] == [mc.nbt("Forged"), mc.nbt({"extra": [{"color": "red", "text": "fire"}], "text": "in "})], comps
    assert mc.nbt(comps["kCustomData"]) == mc.nbt({"owner": "Alice", "uses": 4}), comps
    give(c, 43, 0, 0)

    # The ominous banner, as Raid.getBannerComponentPatch makes it: the one stack among the
    # creative inventory's that carries text - its name.
    patterns, dye = mc.load_registry("banner_pattern"), {"gray": 7, "light_gray": 8, "cyan": 9, "black": 15}
    layers = [("rhombus", "cyan"), ("stripe_bottom", "light_gray"), ("stripe_center", "gray"), ("border", "light_gray"),
              ("stripe_middle", "black"), ("half_horizontal", "light_gray"), ("circle", "light_gray"), ("border", "black")]
    layers = [(patterns.index("minecraft:" + p), dye[d]) for p, d in layers]
    c.send("set_creative_mode_slot", struct.pack(">h", 43) + mc.item_stack(I["kWhiteBanner"], 1, {
        "kBannerPatterns": varint(8) + b"".join(varint(p + 1) + varint(d) for p, d in layers),
        "kTooltipDisplay": b"\x00" + varint(1) + varint(mc.COMPONENT["kBannerPatterns"]),
        "kItemName": mc.nbt({"translate": "block.minecraft.ominous_banner"}), "kRarity": varint(1)}))
    settle(c)
    assert server_menu(c)[43] == (I["kWhiteBanner"], 1, {
        "kItemName": {"translate": "block.minecraft.ominous_banner"}, "kRarity": 1,
        "kTooltipDisplay": (False, [mc.COMPONENT["kBannerPatterns"]]), "kBannerPatterns": layers}), c.slots[43]
    give(c, 43, 0, 0)

    # A component it does not read yet: the stack is refused, and the slot is sent back as it is
    # here - the client shows the stack already. (Vanilla would have taken it.)
    before = len(c.menu_log)
    boxed = {"kTool": varint(0)}   # what an item does: a behaviour component
    c.send("set_creative_mode_slot", struct.pack(">h", 42) + mc.item_stack(I["kStone"], 1, boxed))
    c.send("set_creative_mode_slot", struct.pack(">h", 36) + mc.item_stack(I["kStone"], 1, boxed))
    # ...and so is a text with a part it does not read (a selector), or lore longer than a stack carries here.
    c.send("set_creative_mode_slot", struct.pack(">h", 44) + mc.item_stack(I["kStone"], 1, {"kCustomName": mc.nbt({"selector": "@a"})}))
    c.send("set_creative_mode_slot", struct.pack(">h", 45) + mc.item_stack(I["kStone"], 1, {
        "kLore": varint(20) + b"".join(mc.nbt(chr(97 + i) * 60000) for i in range(20))}))     # 1.2 MiB
    settle(c)
    assert c.menu_log[before:] == [("container_set_slot", 42, None), ("container_set_slot", 36, plain("kStone", 64)),
                                   ("container_set_slot", 44, None), ("container_set_slot", 45, None)], c.menu_log[before:]
    c.send("set_creative_mode_slot", struct.pack(">h", -1) + mc.item_stack(I["kStone"], 1, boxed))  # thrown away: nothing to put right
    settle(c)
    assert len(c.menu_log) == before + 4 and not c.entities

    # set_carried_item: a slot outside the hotbar is ignored (vanilla logs a warning).
    c.send("set_carried_item", struct.pack(">h", 9))
    c.send("set_carried_item", struct.pack(">h", -1))
    expect_quiet(c)

    # ---- pick block (the middle click): the server finds the item and says which slot is in hand.
    def pick(pos):
        mark = len(c.menu_log)
        c.send("pick_item_from_block", mc.pack_block_pos(*pos) + b"\x00")
        settle(c, ticks=2)
        return c.menu_log[mark:]
    for slot in range(1, 46):
        give(c, slot, 0, 0)
    give(c, 20, I["kDirt"], 5)
    settle(c)             # emptying the crafting grid: its result is said each time
    under = (8, -61, 8)   # the grass block Alice stands on; dirt below it
    # Not in the inventory: creative makes one, in the hand's slot if that is free.
    assert pick(under) == [("set_held_slot", 0), ("container_set_slot", 36, plain("kGrassBlock"))]
    assert pick(under) == [("set_held_slot", 0)]   # in hand already: only the answer
    # Further back in the inventory: it trades places with the first free hotbar slot after the hand.
    assert pick((8, -62, 8)) == [("set_held_slot", 1), ("container_set_slot", 20, None), ("container_set_slot", 37, plain("kDirt", 5))]
    assert pick(under) == [("set_held_slot", 0)]   # in the hotbar: selected
    assert pick((8, -59, 8)) == []                 # air has no item
    assert pick((8, -61, 30)) == []                # out of reach
    assert pick((8, 400, 8)) == []                 # outside the world


@test()
def malformed_item_stacks_disconnect(srv: Server):
    """What vanilla's decoder throws on closes the connection; the server lives on."""
    bad = {
        "a count over 99": mc.item_stack(I["kStone"], 100),
        "an item that does not exist": varint(1) + varint(I["kCount"]) + varint(0) + varint(0),
        "a component type that does not exist": varint(1) + varint(1) + varint(1) + varint(0) + varint(mc.COMPONENT["kCount"]) + varint(0),
        "a value longer than the packet": varint(1) + varint(1) + varint(1) + varint(0) + varint(mc.COMPONENT["kDamage"]) + varint(9) + b"\x01",
        "a damage below zero": mc.item_stack(I["kDiamondSword"], 1, {"kDamage": varint(0xFFFFFFFF)}),
        "a stack size of 100": mc.item_stack(I["kStone"], 1, {"kMaxStackSize": varint(100)}),
        "an enchantment that does not exist": mc.item_stack(I["kDiamondSword"], 1, {"kEnchantments": varint(1) + varint(200) + varint(1)}),
        "a name that is half a tag": mc.item_stack(I["kStone"], 1, {"kCustomName": b"\x08\x00\x05hi"}),
        "a name that is a number": mc.item_stack(I["kStone"], 1, {"kCustomName": mc.nbt(5)}),
        "a name without text": mc.item_stack(I["kStone"], 1, {"kItemName": mc.nbt({"color": "red"})}),
        "a name in no colour": mc.item_stack(I["kStone"], 1, {"kCustomName": mc.nbt({"text": "x", "color": "pinkish"})}),
        "257 lines of lore": mc.item_stack(I["kStone"], 1, {"kLore": varint(257) + mc.nbt("") * 257}),
        "custom data that is a list": mc.item_stack(I["kStone"], 1, {"kCustomData": mc.nbt([1, 2])}),
        "the data of a block entity that does not exist": mc.item_stack(I["kStone"], 1, {"kBlockEntityData": varint(200) + mc.nbt({})}),
        "a book copied four times": mc.item_stack(I["kWrittenBook"], 1, {"kWrittenBookContent": mc.mc_str("t") + b"\x00" + mc.mc_str("a") + varint(4) + varint(0) + b"\x00"}),
        "bytes after the stack": mc.item_stack(I["kStone"], 1) + b"\x00",
        "no stack at all": b"",
    }
    for i, (why, stack) in enumerate(bad.items()):
        c = srv.client()
        join(c, f"Mallory{i}")
        c.send("set_creative_mode_slot", struct.pack(">h", 36) + stack)
        expect_kick(c, why)
        c.close()
    # A click that names a slot vanilla would index its list with, and one whose stacks are no stacks.
    for i, body in enumerate([
        varint(0) + varint(1) + struct.pack(">hb", -1, 0) + varint(SWAP) + varint(0) + b"\x00",
        varint(0) + varint(1) + struct.pack(">hb", 9, 0) + varint(PICKUP) + varint(0) + b"\x01" + varint(I["kCount"]) + varint(1) + varint(0) + varint(0),
        varint(0) + varint(1) + struct.pack(">hb", 9, 0) + varint(PICKUP) + varint(129),
    ]):
        c = srv.client()
        join(c, f"Clicky{i}")
        c.send("container_click", body)
        expect_kick(c, f"click {i}")
        c.close()
    c = srv.client()
    check_join(join(c, "Alice"), "Alice")


# At vanilla's rate: what a client sends is forgiven by the tick, and the burst below has to come
# within a tick or two. (Megabytes through the test client: it fills a core by itself.)
@test(tick_rate=20)
def heavy_stacks(srv: Server):
    """Stacks of up to a megabyte: kept, shown to others, there again when their holder is - sent slot by
    slot when no frame holds them all, and bounded: how fast a client may send them, how much of them it
    may come to carry."""
    a, b = srv.client(), srv.client()
    join(a, "Alice")
    jb = join(b, "Bob")
    a.expect("player_info_update")
    for cl in (a, b):
        cl.send("player_loaded")
    settle(b, a)

    def stack(slot: int, floats: int, tint: int) -> tuple:
        """custom_model_data: so many floats, no flags, no strings, one colour that tells the stacks apart."""
        value = varint(floats) + struct.pack(">f", 1.0) * floats + varint(0) + varint(0) + varint(1) + struct.pack(">i", tint)
        return "set_creative_mode_slot", struct.pack(">h", slot) + mc.item_stack(I["kStone"], 1, {"kCustomModelData": value})

    def put(slot: int, floats: int, tint: int) -> None:
        b.send(*stack(slot, floats, tint))

    def read(floats: int, tint: int) -> tuple:
        return (I["kStone"], 1, {"kCustomModelData": ([1.0] * floats, [], [], [tint])})

    def whole_menu() -> list:
        """As server_menu() - but this menu is more than a frame holds, and comes slot by slot."""
        before = len(b.menu_log)
        menu_click(b, -1, 0, PICKUP, carried=None, state=(b.state_id + 1000) & 32767)
        settle(b, ticks=2)
        kinds = [m[0] for m in b.menu_log[before:]]
        assert kinds == ["container_set_slot"] * 46 + ["set_cursor_item"], kinds
        return b.slots

    # 160 KiB in the hand: kept, and Alice is shown it.
    put(36, 40000, 1)
    settle(b, a)
    assert a.entities[jb["login"]["entity_id"]].equipment == {MAINHAND: read(40000, 1)}

    # Eight stacks of 900 KiB at once. A client may send four megabytes of heavy stacks in a
    # burst: that is the one above and four of these. The rest are refused unread, and their
    # slots come back as they are here - empty.
    before = len(b.menu_log)
    b.send_together(*[stack(37 + i, 225000, 10 + i) for i in range(8)])     # (made first, then one write)
    settle(b, ticks=2)
    assert b.menu_log[before:] == [("container_set_slot", 41 + i, None) for i in range(4)], [m[:2] for m in b.menu_log[before:]]
    slots = whole_menu()
    assert slots[36] == read(40000, 1) and [slots[37 + i] for i in range(8)] == [read(225000, 10 + i) for i in range(4)] + [None] * 4

    # Bob leaves and is back: with all of it, and Alice is shown his hand again.
    b.close()
    a.expect("player_info_remove")
    b = srv.client()
    jb = join(b, "Bob")
    a.expect("player_info_update")
    b.send("player_loaded")
    settle(b, a)
    assert b.slots[36] == read(40000, 1) and [b.slots[37 + i] for i in range(4)] == [read(225000, 10 + i) for i in range(4)]
    assert a.entities[jb["login"]["entity_id"]].equipment == {MAINHAND: read(40000, 1)}

    # What was sent is forgiven with time (a megabyte and a quarter a second). What a player may
    # come to carry is not: eight megabytes of heavy stacks, and the tenth of these would be more.
    for i in range(4):
        put(41 + i, 225000, 20 + i)
    settle(b, ticks=20)
    put(9, 225000, 30)
    settle(b, ticks=60)             # sixty ticks: nothing of what was sent counts any more
    before = len(b.menu_log)
    put(10, 225000, 31)
    settle(b)
    assert b.menu_log[before:] == [("container_set_slot", 10, None)], [m[:2] for m in b.menu_log[before:]]
    slots = whole_menu()
    assert [slots[41 + i] for i in range(4)] == [read(225000, 20 + i) for i in range(4)] and slots[9] == read(225000, 30)
    assert slots[10] is None


@test()
def equipment_is_shown(srv: Server):
    """What Bob holds and wears, as Alice sees it."""
    a, b = srv.client(), srv.client()
    ja = join(a, "Alice")
    jb = join(b, "Bob")
    ida, idb = ja["login"]["entity_id"], jb["login"]["entity_id"]
    a.expect("player_info_update")
    for cl in (a, b):
        cl.send("player_loaded")
    settle(b, a)
    eb = a.entities[idb]
    assert eb.equipment == {}

    def since(mark):
        return [(n, w) for n, e, w in a.entity_log[mark:] if e == idb and n in ("set_equipment", "entity_event", "swing_animation")]

    # ---- the hand: a stack in the selected hotbar slot, and another slot selected.
    mark = len(a.entity_log)
    give(b, 36, I["kStone"], 64)
    give(b, 37, I["kDirt"], 1)        # not in hand: nobody is told
    settle(b, a)
    assert since(mark) == [("set_equipment", {MAINHAND: plain("kStone", 64)})], since(mark)
    assert not log_of(b, ida)[4:] and not b.sounds
    mark = len(a.entity_log)
    b.send("set_carried_item", struct.pack(">h", 1))
    settle(b, a)
    assert since(mark) == [("set_equipment", {MAINHAND: plain("kDirt", 1)})] and eb.equipment == {MAINHAND: plain("kDirt", 1)}
    b.send("set_carried_item", struct.pack(">h", 5))   # an empty slot: an empty stack for the hand
    settle(b, a)
    assert eb.equipment == {}
    b.send("set_carried_item", struct.pack(">h", 0))
    settle(b, a)

    # ---- armour: several slots changed in one tick are one packet; what goes on sounds, for
    # everyone within 16 blocks, its wearer included.
    mark = len(a.entity_log)
    give(b, 5, I["kDiamondHelmet"])
    give(b, 8, I["kLeatherBoots"])
    give(b, 6, I["kStone"], 2)         # the creative inventory may; stone makes no sound going on
    settle(b, a)
    assert since(mark) == [("set_equipment", {FEET: plain("kLeatherBoots"), CHEST: plain("kStone", 2), HEAD: plain("kDiamondHelmet")})], since(mark)
    want = [(mc.ITEM_INFO["diamond_helmet"]["equip_sound"], 7, SPAWN, 1.0, 1.0),
            (mc.ITEM_INFO["leather_boots"]["equip_sound"], 7, SPAWN, 1.0, 1.0)]
    assert a.sounds == want and b.sounds == want, (a.sounds, b.sounds)
    give(b, 5, I["kDiamondHelmet"])    # the same again: no sound, nothing to show
    give(b, 6, 0, 0)
    settle(b, a)
    assert len(a.sounds) == 2 and eb.equipment == {MAINHAND: plain("kStone", 64), FEET: plain("kLeatherBoots"), HEAD: plain("kDiamondHelmet")}

    # ---- F: the hands trade places. Viewers get an event and swap what they know; the player's
    # own client did nothing and is sent both slots.
    give(b, 45, I["kShield"])
    settle(b, a)
    assert eb.equipment[OFFHAND] == plain("kShield") and len(a.sounds) == 3   # a shield onto the arm sounds as well
    mark, mmark = len(a.entity_log), len(b.menu_log)
    action(b, SWAP_HANDS)
    settle(b, a)
    assert since(mark) == [("entity_event", 55)], since(mark)
    assert eb.equipment[MAINHAND] == plain("kShield") and eb.equipment[OFFHAND] == plain("kStone", 64)
    assert b.menu_log[mmark:] == [("container_set_slot", 36, plain("kShield")), ("container_set_slot", 45, plain("kStone", 64))], b.menu_log[mmark:]
    assert len(a.sounds) == 3
    action(b, SWAP_HANDS)              # back: the shield goes onto the arm again
    settle(b, a)
    assert eb.equipment[MAINHAND] == plain("kStone", 64) and len(a.sounds) == 4
    mark = len(a.entity_log)
    action(b, SWAP_HANDS)              # twice within a tick: nothing to see
    action(b, SWAP_HANDS)
    settle(b, a)
    assert since(mark) == [], since(mark)

    # ---- components travel: a worn sword in hand.
    b.send("set_creative_mode_slot", struct.pack(">h", 36) + mc.item_stack(I["kDiamondSword"], 1, {"kDamage": varint(300)}))
    settle(b, a)
    assert eb.equipment[MAINHAND] == (I["kDiamondSword"], 1, {"kDamage": 300}), eb.equipment

    # ---- whoever comes later is shown all of it with the entity, in one bundle.
    c = srv.client()
    join(c, "Carol")
    skip_tab_updates(a, 1)
    skip_tab_updates(b, 1)
    settle(c, a, b)
    seen = c.entities[idb]
    assert seen.equipment == eb.equipment and seen.bundled, seen.equipment
    names = [n for n, e, _ in c.entity_log if e in (idb, None)]
    at = names.index("set_equipment")
    assert names[at - 2:at + 2] == ["add_entity", "set_entity_data", "set_equipment", "bundle_delimiter"], names
    assert c.entities[ida].equipment == {} and "set_equipment" not in [n for n, e, _ in c.entity_log if e == ida]


@test()
def drop_and_pick_up(srv: Server):
    a, b = srv.client(), srv.client()
    ja = join(a, "Alice")
    jb = join(b, "Bob")
    ida, idb = ja["login"]["entity_id"], jb["login"]["entity_id"]
    a.expect("player_info_update")
    for cl in (a, b):
        cl.send("player_loaded")
    walk(a, ja, 12.5, 12.5, 10)     # Alice watches from a few blocks away
    look(b, 0.0, DOWN)              # Bob looks at his feet: what he drops falls there
    settle(b, a)
    item_type = mc.ENTITY_TYPE["kItem"]

    def items(c):
        return {i: e for i, e in c.entities.items() if e.type == item_type}

    # ---- Q: one stone leaves the hand. Bob's client took it out itself; Alice sees the arm swing
    # and the item appear below Bob's eyes, thrown the way he looks: down, a fifth of a block a
    # tick give or take a tenth, and up to a fiftieth of one sideways (createItemStackToDrop).
    give(b, 36, I["kStone"], 3)
    settle(b, a)
    mark, mmark = len(a.entity_log), len(b.menu_log)
    action(b, DROP_ONE)
    settle(b, a)
    assert len(items(a)) == 1 and len(items(b)) == 1
    eid, e = next(iter(items(a).items()))
    where, (vx, vy, vz) = e.spawned
    assert where == (SPAWN[0], DROP_Y, SPAWN[2]) and e.bundled, (where, DROP_Y)
    assert abs(vx) < 0.0201 and abs(vz) < 0.0201 and -0.3001 < vy < -0.0999, e.spawned
    assert e.data == {D_ITEM: (STACK, plain("kStone", 1))}, e.data
    # It falls (a client lets it fall by itself, and is put right when it lands): half a second
    # later it lies on the ground by Bob's feet, and both clients know exactly how high.
    settle(b, a, ticks=12)
    for cl in (a, b):
        x, y, z = cl.entities[eid].pos
        assert y == SPAWN[1] and cl.entities[eid].on_ground and abs(x - SPAWN[0]) < 0.3 and abs(z - SPAWN[2]) < 0.3, (x, y, z)
    assert ("swing_animation", idb, (0, 1, 6)) in a.entity_log[mark:]     # main hand, WHACK, 6 ticks
    assert not [x for x in b.entity_log if x[0] == "swing_animation"]     # Bob's client swung by itself
    assert b.menu_log[mmark:] == [] and a.entities[idb].equipment == {MAINHAND: plain("kStone", 2)}
    assert server_menu(b)[36] == plain("kStone", 2)

    # ---- 40 ticks later it can be picked up, and Bob stands right there: the collect animation,
    # the entity goes, and the stack is back - which Bob's client has to be told.
    mark, mmark = len(a.entity_log), len(b.menu_log)
    b.ticks(45)
    settle(b, a)
    assert not items(a) and not items(b)
    # (Meanwhile Alice heard of it every so often, as of any item that lies somewhere: every
    # fourth tick it tries the ground under it, and its velocity is news each time.)
    assert [x for x in a.entity_log[mark:] if x[1] == eid][-2:] == [("take_item_entity", eid, (idb, 1)), ("remove_entities", eid, None)]
    assert [x for x in b.entity_log if x[1] == eid][-2:] == [("take_item_entity", eid, (idb, 1)), ("remove_entities", eid, None)]
    assert b.menu_log[mmark:] == [("container_set_slot", 36, plain("kStone", 3))], b.menu_log[mmark:]
    assert a.entities[idb].equipment == {MAINHAND: plain("kStone", 3)}

    # ---- ctrl+Q drops the stack; an empty hand drops nothing (and swings nothing).
    action(b, DROP_ALL)
    settle(b, a)
    assert [e.data[D_ITEM][1] for e in items(a).values()] == [plain("kStone", 3)] and a.entities[idb].equipment == {}
    mark = len(a.entity_log)
    action(b, DROP_ONE)
    settle(b, a)
    assert len(items(a)) == 1 and not [x for x in a.entity_log[mark:] if x[0] == "swing_animation"]

    # ---- Bob walks off before it can be picked up. Thrown out of the creative inventory (slot -1),
    # two more stacks land on the same spot... which is where Bob is now, so walk first.
    walk(b, jb, 2.5, 2.5, 10)
    mark = len(a.entity_log)
    b.send("set_creative_mode_slot", struct.pack(">h", -1) + mc.item_stack(I["kDirt"], 5))
    b.send("set_creative_mode_slot", struct.pack(">h", -1) + mc.item_stack(I["kDirt"], 7))
    settle(b, a)
    # Both appeared at the same place below his eyes. (Two items that fall through the same
    # blocks may become one on the way down already: see below.)
    thrown = [where for name, _, where in a.entity_log[mark:] if name == "add_entity"]
    assert thrown == [(2.5, DROP_Y, 2.5)] * 2, thrown
    dirt = {i: e for i, e in items(a).items() if e.data[D_ITEM][1][0] == I["kDirt"]}
    assert len(dirt) in (1, 2) and len(items(a)) == 1 + len(dirt)
    walk(b, jb, 8.5, 2.5, 10)       # six blocks from the dirt, six from the stone
    settle(b, a)
    # Items lying together become one: every other tick while it passes from block to block,
    # and every 40 ticks of its life where it lies, an item looks around; the smaller stack
    # goes into the larger, and the emptied entity is removed.
    b.ticks(45)
    settle(b, a)
    left = items(a)
    assert sorted(e.data[D_ITEM][1][:2] for e in left.values()) == sorted([(I["kDirt"], 12), (I["kStone"], 3)]), [e.data for e in left.values()]
    assert set(items(b)) == set(left)
    merged = next(i for i, e in left.items() if e.data[D_ITEM][1][0] == I["kDirt"])
    # ---- back onto the pile: all twelve at once.
    mark = len(a.entity_log)
    walk(b, jb, 2.5, 2.5, 10)
    settle(b, a, ticks=6)
    assert ("take_item_entity", merged, (idb, 12)) in a.entity_log[mark:] and merged not in a.entities
    assert sorted(s[:2] for s in server_menu(b) if s) == [(I["kDirt"], 12)]
    # The stone is still out of reach; Alice walking over it takes it.
    assert len(items(a)) == 1
    walk(a, ja, 8.5, 8.5, 10)
    settle(a, b, ticks=6)
    assert not items(a) and not items(b) and server_menu(a)[36] == plain("kStone", 3)

    # ---- a full inventory in creative: what is picked up and does not fit is destroyed.
    for slot in range(9, 45):
        give(a, slot, I["kGlowstone"], 64)
    walk(b, jb, 8.5, 2.5, 10)
    give(b, 36, I["kDiamondSword"])
    action(b, DROP_ONE)
    walk(b, jb, 2.5, 2.5, 10)
    walk(a, ja, 8.5, 2.5, 10)
    a.ticks(45)
    settle(a, b)
    assert not items(a) and not items(b)
    assert all(s == plain("kGlowstone", 64) for s in server_menu(a)[9:45])


@test()
def named_items(srv: Server):
    """Text and NBT components travel with the stack: into a hand, onto the ground, back."""
    a, b = srv.client(), srv.client()
    ja = join(a, "Alice")
    jb = join(b, "Bob")
    idb = jb["login"]["entity_id"]
    a.expect("player_info_update")
    for cl in (a, b):
        cl.send("player_loaded")
    walk(a, ja, 12.5, 12.5, 10)
    look(b, 0.0, DOWN)
    settle(b, a)
    item_type = mc.ENTITY_TYPE["kItem"]
    sent = {"kCustomName": mc.nbt({"text": "Sting", "italic": False, "color": "aqua"}),
            "kLore": varint(1) + mc.nbt({"translate": "item.minecraft.elytra", "with": ["a", 1]}),
            "kCustomData": mc.nbt({"b": mc.Byte(1), "a": {"z": [1, 2], "y": mc.LongArray((7,))}}),
            "kDamage": varint(10)}
    kept = {"kCustomName": {"color": "aqua", "text": "Sting", "italic": mc.Byte(0)},
            "kLore": [{"with": ["a", 1], "translate": "item.minecraft.elytra"}],
            "kCustomData": {"a": {"y": mc.LongArray((7,)), "z": [1, 2]}, "b": mc.Byte(1)},
            "kDamage": 10}

    def wire(comps):
        """Each tag among the components as its bytes: equal only if the entries are in the same order."""
        return {k: [mc.nbt(x) for x in v] if k == "kLore" else v if k == "kDamage" else mc.nbt(v) for k, v in comps.items()}

    def same(stack, count=1):
        return stack[:2] == (I["kDiamondSword"], count) and wire(stack[2]) == wire(kept)

    # In Bob's hand: Alice is shown the stack, name and all.
    b.send("set_creative_mode_slot", struct.pack(">h", 36) + mc.item_stack(I["kDiamondSword"], 1, sent))
    settle(b, a)
    assert same(a.entities[idb].equipment[MAINHAND]), a.entities[idb].equipment
    assert same(server_menu(b)[36])

    # Dropped: the item on the ground is that stack. (Bob's client took it out of the slot itself.)
    action(b, DROP_ALL)
    settle(b, a)
    on_ground = [e for e in a.entities.values() if e.type == item_type]
    assert len(on_ground) == 1 and same(on_ground[0].data[D_ITEM][1]), [e.data for e in on_ground]
    assert a.entities[idb].equipment == {}

    # Picked up again 40 ticks later: Bob's client is told what it holds - the same stack still.
    mmark = len(b.menu_log)
    b.ticks(45)
    settle(b, a)
    assert not [e for e in a.entities.values() if e.type == item_type]
    assert [m[:2] for m in b.menu_log[mmark:]] == [("container_set_slot", 36)] and same(b.menu_log[mmark][2]), b.menu_log[mmark:]
    assert same(a.entities[idb].equipment[MAINHAND])

    # One name spelled three ways is one name: the stacks that carry it are one kind of thing
    # and go together. Another name is another thing.
    for slot, name in ((37, mc.nbt("Rock")), (38, mc.nbt({"text": "Rock"})), (39, mc.nbt(["Rock"]))):
        b.send("set_creative_mode_slot", struct.pack(">h", slot) + mc.item_stack(I["kStone"], 10, {"kCustomName": name}))
    b.send("set_creative_mode_slot", struct.pack(">h", 40) + mc.item_stack(I["kStone"], 10, {"kCustomName": mc.nbt("Pebble")}))
    settle(b)
    slots = server_menu(b)
    assert slots[37] == slots[38] == slots[39] == (I["kStone"], 10, {"kCustomName": "Rock"}) and slots[40][2] == {"kCustomName": "Pebble"}
    menu_click(b, 38, 0, PICKUP)      # pick the second stack up...
    settle(b)
    menu_click(b, 37, 0, PICKUP)      # ...and put it on the first: they are the same thing
    settle(b)
    menu_click(b, 40, 0, PICKUP)      # the cursor is empty now; this picks the pebbles up
    settle(b)
    menu_click(b, 39, 0, PICKUP)      # which do not go onto rocks: the two trade places
    settle(b)
    slots = server_menu(b)
    assert slots[37] == (I["kStone"], 20, {"kCustomName": "Rock"}) and slots[38] is None, slots[36:41]
    assert slots[39] == (I["kStone"], 10, {"kCustomName": "Pebble"}) and b.cursor == (I["kStone"], 10, {"kCustomName": "Rock"}), (slots[39], b.cursor)


@test()
def arm_swings(srv: Server):
    """Whoever sees a player sees its arm swing: at a punch, at a block it places, at an item it
    drops - once per swing, which is the server's to decide (SwingState.startIfAble)."""
    a, b = srv.client(), srv.client()
    ja = join(a, "Alice")
    jb = join(b, "Bob")
    idb = jb["login"]["entity_id"]
    a.expect("player_info_update")
    walk(a, ja, 12.5, 12.5, 10)     # Alice watches from a few blocks away
    settle(b, a)
    for cl in (a, b):
        cl.ignore = {"block_update", "block_changed_ack", "keep_alive"}   # the blocks are place_and_break_blocks' subject
    WHACK, STAB = mc.SWING["kWhack"], mc.SWING["kStab"]
    MAIN, OFF = 0, 1
    default = (MAIN, WHACK, 6)       # SwingAnimation.DEFAULT in the main hand

    def swings(mark: int) -> list:
        return [what for name, e, what in a.entity_log[mark:] if name == "swing_animation" and e == idb]

    def rest(ticks: int = 0) -> int:
        """Where Alice's log stands, once Bob's last swing no longer stands in the way of another.
        Every step here ends with settle(): four ticks, more than half a whack. Half a spear's
        stab takes `ticks` more."""
        if ticks:
            b.ticks(ticks)
        return len(a.entity_log)

    def soon_after(first, second, between: int, within: int, ticks: int = 0) -> list:
        """`first`, then `second` at least `between` + 1 ticks later and - going by the clock - not
        many more: a client that was held up longer than `within` ticks' time tries again."""
        for attempt in range(20):
            if attempt:
                settle(b, a)   # the attempt before ended in the middle of something
            mark = rest(ticks)
            t0 = time.time()
            first()
            b.ticks(between + 1)
            second()
            if time.time() - t0 < within * srv.tick:
                settle(b, a)
                return swings(mark)
        raise AssertionError("never quick enough to tell")

    def punch():
        b.send("punch")

    # ---- a punch: the left button. Not held back until the client has loaded, unlike a drop.
    mark = rest()
    punch()
    settle(b, a)
    assert swings(mark) == [default], swings(mark)
    b.send("player_loaded")
    a.send("player_loaded")

    # Two in one tick are two swings: the second starts the first one again.
    mark = rest()
    b.send_together(("punch",), ("punch",))
    settle(b, a)
    assert swings(mark) == [default, default], swings(mark)
    # One that comes while a swing is in its first half - three ticks of a whack's six - is not
    # a swing; after that it is.
    assert soon_after(punch, punch, between=0, within=2) == [default]
    mark = rest()
    punch()
    b.ticks(4)
    punch()
    settle(b, a)
    assert swings(mark) == [default, default], swings(mark)
    # A packet with something behind it is not a punch.
    mark = rest()
    b.send("punch", b"\x00")
    settle(b, a)
    assert swings(mark) == []

    # ---- placing a block swings the hand that placed it (once: see Game::on_use_item_on).
    give(b, 36, I["kStone"], 64)
    give(b, 45, I["kDirt"], 8)
    mark = rest()
    click(b, (9, -61, 8), 1, seq=1)                 # stone onto the grass next to Bob
    settle(b, a)
    assert swings(mark) == [default], swings(mark)
    mark = rest()
    click(b, (9, -61, 8), 1, seq=2)                 # the place is taken: nothing happens, nothing swings
    click(b, (40, -61, 8), 1, seq=3)                # out of reach
    settle(b, a)
    assert swings(mark) == []
    b.send("set_carried_item", struct.pack(">h", 1))   # an empty main hand: the off hand's dirt is placed
    mark = rest()
    click(b, (8, -61, 9), 1, seq=4, hand=1)
    settle(b, a)
    assert swings(mark) == [(OFF, WHACK, 6)], swings(mark)
    give(b, 37, I["kStick"])                        # not a block: nothing to place
    give(b, 45, 0, 0)
    mark = rest()
    click(b, (7, -61, 8), 1, seq=5)
    settle(b, a)
    assert swings(mark) == []
    # Breaking is a punch like any other: the client names the block and swings.
    b.send("set_carried_item", struct.pack(">h", 0))
    mark = rest()
    dig(b, (9, -60, 8), seq=6)
    punch()
    settle(b, a)
    assert swings(mark) == [default], swings(mark)

    # ---- a drop is a swing by the same rule: begun again in the tick of a punch, not begun in
    # the first half of one - the item goes all the same.
    mark = rest()
    b.send_together(("player_action", varint(DROP_ONE) + mc.pack_block_pos(0, 0, 0) + b"\x00" + varint(0)), ("punch",))
    settle(b, a)
    assert swings(mark) == [default, default], swings(mark)
    held = a.entities[idb].equipment[MAINHAND]
    assert held == plain("kStone", 63), held
    assert soon_after(punch, lambda: action(b, DROP_ONE), between=0, within=2) == [default]
    assert a.entities[idb].equipment[MAINHAND][1] < held[1]

    # ---- the animation is the main hand's attack animation: a spear stabs, and for longer. The
    # half that refuses another swing is half of that: eleven ticks of a netherite spear's 23.
    give(b, 36, I["kNetheriteSpear"])
    stab = (MAIN, STAB, 23)
    assert mc.ITEM_INFO["netherite_spear"]["attack_swing"] == (STAB, 23)
    mark = rest()
    punch()
    settle(b, a)
    assert swings(mark) == [stab], swings(mark)
    assert soon_after(punch, punch, between=4, within=9, ticks=8) == [stab]      # a whack would be over
    mark = rest(8)
    punch()
    b.ticks(12)
    punch()
    settle(b, a)
    assert swings(mark) == [stab, stab], swings(mark)
    # What is dropped out of that hand swings as a drop does, not as the spear.
    mark = rest(8)
    action(b, DROP_ONE)
    settle(b, a)
    assert swings(mark) == [default], swings(mark)

    # Bob's client swung its own arm every time; it was never told to.
    assert not [x for x in b.entity_log if x[0] == "swing_animation"]


@test()
def dropped_items_expire(srv: Server):
    """ItemEntity.LIFETIME: an item nobody picks up is gone after 6000 ticks - five minutes at the
    rate of a vanilla server. Here most of them are a sprint (`/tick sprint`), the last hundred
    are counted one by one, and the game time says how many it was."""
    c = srv.client()
    j = join(c, "Alice")
    c.send("player_loaded")
    make_op(srv, c, "Alice")

    def game_time() -> int:
        said = run_command(c, "time query gametime", ticks=1)["said"]
        assert said and said[0]["translate"] == "commands.time.query.gametime", said
        return int(said[0]["with"][0])

    give(c, 36, I["kStone"], 1)
    action(c, DROP_ONE)
    walk(c, j, 2.5, 2.5, 10)        # out of reach before it can be picked up (forty ticks)
    dropped = game_time()           # (the drop's tick, or one of the few after it)
    settle(c)
    item_type = mc.ENTITY_TYPE["kItem"]
    lying = [i for i, e in c.entities.items() if e.type == item_type]
    assert len(lying) == 1
    mark = len(srv.read_log())
    srv.console("tick sprint 5900")
    srv.wait_log("Sprint completed", timeout=60, since=mark)
    c.mark_chat_read()      # (an operator is told of the sprint)
    settle(c)
    c.mark_chat_read()
    assert lying[0] in c.entities and game_time() - dropped < 6000 - 15
    for _ in range(200):
        if lying[0] not in c.entities:
            break
        c.ticks(1)
    lay = game_time() - dropped
    assert 6000 - 15 <= lay <= 6000 + 30, lay
    assert c.entity_log[-1] == ("remove_entities", lying[0], None), c.entity_log[-1]


@test()
def menu_clicks(srv: Server):
    """container_click on the player's own menu. The client reports no prediction, so the server
    says what each click did: the slots it changed, and the cursor."""
    c = srv.client()
    join(c, "Alice")
    c.send("player_loaded")
    look(c, 0.0, DOWN)      # what she throws out of the menu lands at her feet
    for slot, (item, count) in {9: ("kStone", 10), 10: ("kDirt", 5), 11: ("kStone", 64), 13: ("kIronChestplate", 1)}.items():
        give(c, slot, I[item], count)
        c.slots[slot] = plain(item, count)   # a client knows what it put there
    settle(c)

    def click(*args, **kw):
        mark = len(c.menu_log)
        menu_click(c, *args, **kw)
        settle(c, ticks=2)
        return c.menu_log[mark:]

    # Take a stack, put one down, put the rest onto it, take half back.
    assert click(9, 0, PICKUP) == [("container_set_slot", 9, None), ("set_cursor_item", plain("kStone", 10))]
    assert c.state_id == 2
    assert click(20, 1, PICKUP) == [("container_set_slot", 20, plain("kStone", 1)), ("set_cursor_item", plain("kStone", 9))]
    assert click(20, 0, PICKUP) == [("container_set_slot", 20, plain("kStone", 10))]   # an empty cursor is what was reported
    assert click(20, 1, PICKUP) == [("container_set_slot", 20, plain("kStone", 5)), ("set_cursor_item", plain("kStone", 5))]
    # Different items trade places; armour slots take armour only.
    assert click(10, 0, PICKUP) == [("container_set_slot", 10, plain("kStone", 5)), ("set_cursor_item", plain("kDirt", 5))]
    assert click(5, 0, PICKUP) == [("set_cursor_item", plain("kDirt", 5))]   # refused; the cursor is repeated, it was reported empty
    assert not c.sounds
    assert click(12, 0, PICKUP) == [("container_set_slot", 12, plain("kDirt", 5))]
    # Shift-click: armour goes on (with its sound), a stack goes to the hotbar and back.
    assert click(13, 0, QUICK_MOVE) == [("container_set_slot", 6, plain("kIronChestplate")), ("container_set_slot", 13, None)]
    assert c.sounds == [(mc.ITEM_INFO["iron_chestplate"]["equip_sound"], 7, SPAWN, 1.0, 1.0)], c.sounds
    assert click(11, 0, QUICK_MOVE) == [("container_set_slot", 11, None), ("container_set_slot", 36, plain("kStone", 64))]
    # ...back up: onto the stacks of 5 in slots 10 and 20 first - one fills, the other takes the rest.
    assert click(36, 0, QUICK_MOVE) == [("container_set_slot", 10, plain("kStone", 64)), ("container_set_slot", 20, plain("kStone", 10)),
                                        ("container_set_slot", 36, None)]
    # Number keys and F swap a slot with the hotbar or the off hand.
    assert click(10, 2, SWAP) == [("container_set_slot", 10, None), ("container_set_slot", 38, plain("kStone", 64))]
    assert click(20, 40, SWAP) == [("container_set_slot", 20, None), ("container_set_slot", 45, plain("kStone", 10))]
    # A drag: start, one click per slot, end - only the end does something.
    assert click(38, 0, PICKUP) == [("container_set_slot", 38, None), ("set_cursor_item", plain("kStone", 64))]
    mark = len(c.menu_log)
    menu_click(c, OUTSIDE, 0 | 0 << 2, QUICK_CRAFT)
    for slot in (21, 22, 23):
        menu_click(c, slot, 1 | 0 << 2, QUICK_CRAFT)
    menu_click(c, OUTSIDE, 2 | 0 << 2, QUICK_CRAFT)
    settle(c, ticks=2)
    got = c.menu_log[mark:]
    # Every click of the drag reported an empty cursor, so the cursor is repeated after each.
    assert [g for g in got if g[0] == "container_set_slot"] == [("container_set_slot", s, plain("kStone", 21)) for s in (21, 22, 23)], got
    assert got[-1] == ("set_cursor_item", plain("kStone", 1)) and c.cursor == plain("kStone", 1)
    # Double click gathers; Q throws from a slot; a middle click clones (creative).
    assert click(30, 0, PICKUP) == [("container_set_slot", 30, plain("kStone", 1))]
    assert click(31, 0, PICKUP) == []
    assert click(30, 0, PICKUP) == [("container_set_slot", 30, None), ("set_cursor_item", plain("kStone", 1))]
    got = click(31, 0, PICKUP_ALL)
    assert got[-1] == ("set_cursor_item", plain("kStone", 64)) and {g[1] for g in got[:-1]} == {21, 22, 23}, got
    assert click(31, 0, PICKUP) == [("container_set_slot", 31, plain("kStone", 64))]
    assert click(31, 2, CLONE) == [("set_cursor_item", plain("kStone", 64))]
    assert click(32, 0, PICKUP) == [("container_set_slot", 32, plain("kStone", 64))]

    # A client that predicted right and says so is told nothing...
    c.slots[33] = None
    assert click(32, 0, PICKUP, changed={32: None}, carried=(I["kStone"], 64)) == []
    assert click(33, 0, PICKUP, changed={33: (I["kStone"], 64)}, carried=None) == []
    # ...one that predicted wrong is put right, slot by slot...
    assert click(33, 0, PICKUP, changed={33: (I["kStone"], 60)}, carried=(I["kDirt"], 1)) == [
        ("container_set_slot", 33, None), ("set_cursor_item", plain("kStone", 64))]
    assert click(33, 0, PICKUP, changed={33: (I["kStone"], 64)}) == []
    # ...and one that clicked on an older state of the menu gets all of it.
    state = c.state_id
    got = click(33, 0, PICKUP, state=(state + 5) & 32767)
    assert got == [("container_set_content", state + 1)] and c.cursor == plain("kStone", 64) and c.slots[33] is None, got
    assert click(33, 0, PICKUP, changed={33: (I["kStone"], 64)}) == []
    # A click on a menu that is not open is not for us.
    c.send("container_click", varint(7) + varint(c.state_id) + struct.pack(">hb", 33, 0) + varint(PICKUP) + varint(0) + b"\x00")
    expect_quiet(c)

    # ---- out of the menu into the world: Q over a slot throws one, a click outside the window
    # throws the cursor. Both lie at Alice's feet and come back when they may be picked up - each
    # with the slot it went to, straight after the pickup.
    assert click(31, 0, THROW) == [("container_set_slot", 31, plain("kStone", 63))]
    assert click(12, 0, PICKUP) == [("container_set_slot", 12, None), ("set_cursor_item", plain("kDirt", 5))]
    assert click(OUTSIDE, 0, PICKUP) == [] and c.cursor == plain("kDirt", 5)   # the client believes what it predicted
    c.cursor = None
    item_type = mc.ENTITY_TYPE["kItem"]
    lying = {i: e.data[D_ITEM][1] for i, e in c.entities.items() if e.type == item_type}
    assert sorted(lying.values()) == [plain("kStone", 1), plain("kDirt", 5)], lying
    mark, emark = len(c.menu_log), len(c.entity_log)
    c.ticks(45)
    settle(c)
    # The stone joins the stack in the off hand, the dirt takes the first free slot: the hand's.
    assert sorted(c.menu_log[mark:]) == sorted([("container_set_slot", 45, plain("kStone", 11)), ("container_set_slot", 36, plain("kDirt", 5))])
    taken = [(n, w) for n, e, w in c.entity_log[emark:] if e in lying and n in ("take_item_entity", "remove_entities")]
    assert sorted(taken) == sorted([("take_item_entity", (c.entity_id, lying[i][1])) for i in lying] + [("remove_entities", None)] * 2), taken
    assert not [e for e in c.entities.values() if e.type == item_type]

    # ---- closing the menu: what is in the crafting grid and on the cursor goes back into the
    # inventory. The client is told twice, as by vanilla: at once by inventory index, and in the
    # next tick by menu slot.
    give(c, 1, I["kGlowstone"], 3)
    give(c, 4, I["kGlowstone"], 2)
    c.slots[1], c.slots[4] = plain("kGlowstone", 3), plain("kGlowstone", 2)
    settle(c)    # the result slot is said twice, empty both times
    assert click(31, 0, PICKUP) == [("container_set_slot", 31, None), ("set_cursor_item", plain("kStone", 63))]
    mark = len(c.menu_log)
    c.send("container_close", varint(0))
    settle(c)
    got = c.menu_log[mark:]
    # The stone tops up the stack in the off hand (inventory index 40) and the rest takes the first
    # free slot; the glowstone of both grid cells ends up in one.
    assert got[:4] == [("set_player_inventory", 40, plain("kStone", 64)), ("set_player_inventory", 1, plain("kStone", 10)),
                       ("set_player_inventory", 2, plain("kGlowstone", 3)), ("set_player_inventory", 2, plain("kGlowstone", 5))], got
    assert got[4:] == [("container_set_slot", 1, None), ("container_set_slot", 4, None), ("container_set_slot", 37, plain("kStone", 10)),
                       ("container_set_slot", 38, plain("kGlowstone", 5)), ("container_set_slot", 45, plain("kStone", 64)),
                       ("set_cursor_item", None)], got
    # All of it, as the server holds it: nothing was lost or made on the way (but the clone).
    held = {i: st for i, st in enumerate(server_menu(c)) if st}
    assert c.cursor is None and held == {6: plain("kIronChestplate"), 33: plain("kStone", 64), 36: plain("kDirt", 5),
                                         37: plain("kStone", 10), 38: plain("kGlowstone", 5), 45: plain("kStone", 64)}, held


# ----------------------------------------------------------------------------- chests, furnaces, crafting, TNT

AIR = 0
INPUT_SHIFT = 0x20   # player_input: the sneak key


def collect(c: Client, ticks: int = 3) -> dict:
    """Let `ticks` server ticks pass and return the block changes they brought: {pos: state}.
    (Menus, entities, sounds, block events and explosions go into the client's model as always.)"""
    out = {}
    for i in range(ticks):
        token = 0xB10C00 + i
        c.send("ping_request", struct.pack(">q", token))
        while True:
            name, r = c.recv()
            if name == "pong_response":
                assert r.i64() == token
                break
            if name == "block_update":
                pos, state = mc.parse_block_update(r)
                out[pos] = state
            elif name == "section_blocks_update":
                out.update(mc.parse_section_blocks_update(r))
            elif name == "keep_alive":       # a scenario that watches a furnace outlasts the 15 seconds
                c.send("keep_alive", r.take(8))
            else:
                assert name in ("block_changed_ack", "light_update", "player_info_update"), f"unexpected {name}"
    return out


def close_window(c: Client) -> None:
    """The client closes the screen: it tells the server and goes back to its own menu, with
    nothing on the cursor (AbstractContainerMenu.removed runs on both sides)."""
    c.send("container_close", varint(c.window["id"]))
    c.window = None
    c.cursor = None


def of_type(c: Client, kind: str) -> dict:
    return {i: e for i, e in c.entities.items() if e.type == mc.ENTITY_TYPE[kind]}


@test()
def chest(srv: Server):
    """A chest: it faces whoever places it, two players have it open at once, its lid follows the
    count, a second one makes it a double chest, and breaking it spills what it holds."""
    a, b = srv.client(), srv.client()
    join(a, "Alice")
    join(b, "Bob")
    a.expect("player_info_update")
    for cl in (a, b):
        cl.send("player_loaded")
    settle(a, b)
    # The join told the client what a furnace takes (update_recipes): nine property sets.
    assert len(a.recipe_sets) == 9 and a.stonecutter_recipes == 0
    assert {I["kCobblestone"], I["kIronOre"], I["kOakLog"]} <= a.recipe_sets["minecraft:furnace_input"]
    assert I["kDirt"] not in a.recipe_sets["minecraft:furnace_input"]

    ground, pos = (8, -61, 10), (8, -60, 10)
    give(a, 36, I["kChest"], 2)
    give(a, 37, I["kStone"], 20)
    click(a, ground, 1, seq=1)
    assert collect(a)[pos] == mc.BLOCK["kChest"]   # facing north: at Alice, who looks south
    assert collect(b)[pos] == mc.BLOCK["kChest"]

    # A click on it opens it, whatever is in hand: the sound, the screen, the contents, the lid.
    click(a, pos, 1, seq=2)
    collect(a), collect(b)
    w = a.window
    assert w and (w["id"], w["type"], w["title"]) == (1, "kGeneric9x3", {"translate": "container.chest"}), w
    assert len(w["slots"]) == 63 and w["slots"][:27] == [None] * 27
    assert w["slots"][54] == plain("kChest", 2) and w["slots"][55] == plain("kStone", 20)   # the hotbar, after 27 + 27
    sound, source, where, volume, pitch = b.sounds[-1]
    assert (sound, source, where, volume) == (mc.SOUND["kBlockChestOpen"], 4, (8.5, -59.5, 10.5), 0.5) and 0.9 <= pitch < 1.0
    assert a.block_events[-1][:3] == (pos, 1, 1) and b.block_events[-1] == a.block_events[-1]
    assert ("swing_animation", a.entity_id, (0, 1, 6)) in b.entity_log     # the others see the arm

    menu_click(a, 55, 0, QUICK_MOVE)     # the stone, by shift-click
    settle(a)
    assert w["slots"][0] == plain("kStone", 20) and w["slots"][55] is None

    # Bob opens the same chest: he sees the stone, and the lid counts two.
    click(b, pos, 1, seq=1)
    collect(b), collect(a)
    assert b.window["id"] == 1 and b.window["slots"][0] == plain("kStone", 20)
    assert a.block_events[-1][:3] == (pos, 1, 2)
    menu_click(b, 0, 1, PICKUP)          # Bob takes half; Alice's screen follows a tick later
    settle(b, a)
    assert b.cursor == plain("kStone", 10) and a.window["slots"][0] == plain("kStone", 10)
    # While it is open the chest says so again every five ticks (ChestBlock.tick).
    n = len(a.block_events)
    settle(a, ticks=12)
    assert len(a.block_events) - n in (2, 3) and a.block_events[-1][:3] == (pos, 1, 2), a.block_events[n:]

    close_window(a)
    settle(a, b)
    assert b.block_events[-1][:3] == (pos, 1, 1)
    heard = len(a.sounds)
    close_window(b)                      # what was on Bob's cursor goes back into his inventory
    settle(a, b)
    assert a.block_events[-1][:3] == (pos, 1, 0) and a.sounds[heard][0] == mc.SOUND["kBlockChestClose"]
    assert b.slots[36] == plain("kStone", 10) and b.cursor is None and server_menu(b)[36] == plain("kStone", 10)

    # A second chest beside it, facing the same way: both become halves of one.
    pos2 = (9, -60, 10)
    click(a, (9, -61, 10), 1, seq=3)
    first = mc.BLOCK["kChest"] - 1       # facing north, single, waterlogged: the block's first state
    changes = collect(a)
    assert changes[pos] == first + 2 + 1 and changes[pos2] == first + 4 + 1, changes   # left, right
    collect(b)
    click(a, pos2, 1, seq=4)
    collect(a), collect(b)
    w = a.window
    assert (w["id"], w["type"], w["title"]) == (2, "kGeneric9x6", {"translate": "container.chestDouble"}) and len(w["slots"]) == 90
    assert w["slots"][27] == plain("kStone", 10)     # the right half comes first; the stone lies in the left one
    assert [e[:3] for e in a.block_events[-2:]] == [(pos2, 1, 1), (pos, 1, 1)]

    # Bob breaks the half with the stone in it: Alice's screen closes, the stone lies on the
    # ground, the other half is a chest of its own again.
    mark = len(a.menu_log)
    dig(b, pos, seq=2)
    changes = collect(a)
    collect(b)
    assert changes[pos] == AIR and changes[pos2] == mc.BLOCK["kChest"], changes
    # (The menu shows the slot the stone fell out of before it closes: as in vanilla, where it
    # looks at its container once more in the tick that finds it gone.)
    assert a.menu_log[mark:] == [("container_set_slot", 27, None), ("container_close", 2)] and a.window is None, a.menu_log[mark:]
    spilled = [e for e in of_type(a, "kItem").values() if e.data[D_ITEM] == (STACK, plain("kStone", 10))]
    assert len(spilled) == 1 and all(lo <= v < lo + 1 for v, lo in zip(spilled[0].pos, pos)), [e.pos for e in spilled]

    # Something solid on top keeps the lid shut: the click does nothing but swing the arm.
    a.send("set_carried_item", struct.pack(">h", 1))
    give(a, 37, I["kStone"], 20)
    a.send("player_input", bytes([INPUT_SHIFT]))    # sneaking, the click goes to the block in hand
    click(a, pos2, 1, seq=5)
    assert collect(a)[(9, -59, 10)] == mc.BLOCK["kStone"]
    a.send("player_input", bytes([0]))
    click(a, pos2, 2, seq=6, cursor=(0.5, 0.5, 0.0))
    collect(a), collect(b)
    assert a.window is None

    # Who joins now is told there is a block entity in the chunk: that is what draws a chest.
    d = srv.client()
    jd = join(d, "Dave", view=2)
    assert jd["chunks"][(0, 0)]["block_entities"] == [((9, -60, 10), mc.BLOCK_ENTITY["kChest"], None)]
    assert all(not ch["block_entities"] for at, ch in jd["chunks"].items() if at != (0, 0))


@test()
def picking_up_with_a_chest_open(srv: Server):
    """What a player picks up while it looks into a chest appears in that screen's inventory rows.
    (It crashed the server once: the menu was synchronised without its chest.)"""
    a, b = srv.client(), srv.client()
    join(a, "Alice")
    join(b, "Bob")
    a.expect("player_info_update")
    for cl in (a, b):
        cl.send("player_loaded")
    settle(a, b)
    give(a, 36, I["kChest"])
    click(a, (8, -61, 10), 1, seq=1)
    click(a, (8, -60, 10), 1, seq=2)
    collect(a)
    assert a.window and a.window["type"] == "kGeneric9x3"
    # Bob, who stands where Alice stands, drops a stone at their feet and walks off.
    give(b, 36, I["kStone"], 1)
    look(b, 0.0, DOWN)
    action(b, DROP_ONE)
    collect(b)
    b.send("move_player_pos", struct.pack(">dddB", 13.5, SPAWN[1], 13.5, 1))
    for _ in range(30):                         # (a dropped item can be picked up after two seconds)
        collect(a, ticks=3)
        collect(b, ticks=1)
        if plain("kStone", 1) in a.window["slots"][27:]:
            break
    assert a.window and plain("kStone", 1) in a.window["slots"][27:], a.window["slots"][27:]
    assert not of_type(a, "kItem")


@test()
def a_lid_is_told_once(srv: Server):
    """A chest says how many have it open when that changes, and again with every recount - five
    ticks apart while it is open, and once more after. A block event that is asked for twice in a
    tick is sent once (vanilla keeps the tick's events in a set): closed in the very tick of the
    recount, the lid is told to fall once, not twice. (The game stands still and is stepped: in
    which tick the chest is closed is the scenario's to say, not the clock's.)"""
    a = wet_start(srv)
    ground, pos = (8, -61, 10), (8, -60, 10)
    give(a, 36, I["kChest"], 1)
    click(a, ground, 1, seq=1)
    assert collect(a)[pos] == mc.BLOCK["kChest"]
    told = []
    for wait in range(1, 9):                # every phase of the five ticks, and some of them again
        click(a, pos, 1, seq=1 + wait)
        collect(a)
        n = len(a.block_events)
        step_game(a, wait)
        close_window(a)
        collect(a)
        step_game(a, 8)                     # (the recount that was due when it closed has come by now)
        after = [e[:3] for e in a.block_events[n:]]
        assert set(after) <= {(pos, 1, 1), (pos, 1, 0)} and after[-1] == (pos, 1, 0), (wait, after)
        told.append(after.count((pos, 1, 0)))
    # By the close and by the recount - or, closed four ticks after it was opened, by both at once.
    assert told == [2, 2, 2, 1, 2, 2, 2, 2], told


def furnace_setup(srv: Server, stone: int, planks: int):
    c = srv.client()
    join(c, "Alice")
    c.send("player_loaded")
    ground, pos = (8, -61, 10), (8, -60, 10)
    give(c, 36, I["kFurnace"])
    give(c, 37, I["kCobblestone"], stone)
    give(c, 38, I["kOakPlanks"], planks)
    click(c, ground, 1, seq=1)
    off = mc.BLOCK["kFurnace"]                      # the default state: facing north, not lit
    assert collect(c)[pos] == off
    click(c, pos, 1, seq=2)
    collect(c)
    w = c.window
    assert (w["id"], w["type"], w["title"]) == (1, "kFurnace", {"translate": "container.furnace"}), w
    assert len(w["slots"]) == 39 and w["data"] == {0: 0, 1: 0, 2: 0, 3: 0}
    # Shift-clicks sort themselves: what smelts into the input, what burns into the fuel slot.
    menu_click(c, 31, 0, QUICK_MOVE)
    menu_click(c, 32, 0, QUICK_MOVE)
    changes = collect(c, ticks=4)
    assert changes[pos] == off - 1, changes         # lit
    assert w["slots"][0] == plain("kCobblestone", stone) and w["slots"][2] is None
    assert w["slots"][1] == (plain("kOakPlanks", planks - 1) if planks > 1 else None)
    # The fire: 300 ticks for a plank, 200 to cook a stone - counting down and up as we look, a tick
    # at a time: the tick that lit it cooked too, and burnt nothing yet (AbstractFurnaceBlockEntity.serverTick).
    assert w["data"][1] == 300 and w["data"][3] == 200 and 0 < w["data"][2] < 100 and w["data"][0] == 301 - w["data"][2], w["data"]
    return c, pos, off


@test()
def furnace(srv: Server):
    """A furnace lights up when it has something to smelt and something to burn, and its menu
    shows the fire."""
    c, pos, off = furnace_setup(srv, stone=3, planks=2)
    w = c.window
    before = dict(w["data"])
    settle(c, ticks=5)
    assert w["data"][2] > before[2] and w["data"][0] < before[0]
    # Taking the input out stops the cooking at once (AbstractFurnaceBlockEntity.setItem).
    menu_click(c, 0, 0, PICKUP)
    settle(c, ticks=3)
    assert c.cursor == plain("kCobblestone", 3) and w["data"][2] == 0 and w["data"][0] > 0
    # Breaking it spills the fuel and closes the screen.
    mark = len(c.menu_log)
    dig(c, pos, seq=3)
    assert collect(c)[pos] == AIR
    # The menu shows the fuel slot empty, then closes; what was on the cursor goes back.
    log = c.menu_log[mark:]
    assert ("container_set_slot", 1, None) in log and c.window is None, log
    assert log.index(("container_set_slot", 1, None)) < log.index(("container_close", 1)) < log.index(("set_player_inventory", 1, plain("kCobblestone", 3))), log
    assert [e.data[D_ITEM][1] for e in of_type(c, "kItem").values()] == [plain("kOakPlanks", 1)]
    # The plank flies off at random, and whether it lands within reach is chance: it is picked up
    # (or not) once its pickup delay of ten ticks is over and it has come to rest - not while the
    # server is asked for the menu below, whose answer would then not come alone.
    settle(c, ticks=20)
    assert server_menu(c)[37] == plain("kCobblestone", 3)    # the cursor went back into the inventory: the slot it came from is the first free one


@test()
def furnace_smelts(srv: Server):
    """One cobblestone, one plank: a stone after 200 ticks, and the fire out after 300."""
    c, pos, off = furnace_setup(srv, stone=1, planks=1)
    w = c.window
    changes = {}
    for _ in range(70):
        changes.update(collect(c, ticks=3))
        if w["slots"][2]:
            break
    assert w["slots"][2] == plain("kStone", 1) and w["slots"][0] is None and w["data"][2] == 0, (w["slots"][:3], w["data"])
    assert 50 < w["data"][0] <= 101 and pos not in changes, (w["data"], changes)     # still burning: the tick that lit it cooked too
    for _ in range(40):
        changes.update(collect(c, ticks=3))
        if pos in changes:
            break
    assert changes[pos] == off and w["data"][0] == 0, (changes, w["data"])
    menu_click(c, 2, 0, QUICK_MOVE)          # the stone into the inventory: the hotbar's far end first
    settle(c)
    assert w["slots"][2] is None and w["slots"][38] == plain("kStone", 1)


COOKERS = (("kSmoker", "container.smoker", "kBeef", "kCookedBeef", "kIronOre"),
           ("kBlastFurnace", "container.blast_furnace", "kIronOre", "kIronIngot", "kBeef"))


def cooker_setup(c: Client, n: int, count: int, planks: int):
    """Places the n-th of COOKERS, opens it and fills it by shift-clicks; (where it is, its unlit state)."""
    block, title, cooks, _, other = COOKERS[n]
    ground, pos = (8 + 2 * n, -61, 10), (8 + 2 * n, -60, 10)
    give(c, 36, I[block])
    give(c, 37, I[cooks], count)
    give(c, 38, I["kOakPlanks"], planks)
    give(c, 39, I[other], 3)
    click(c, ground, 1, seq=10 * n + 1)
    off = mc.BLOCK[block]                           # the default state: facing north, not lit
    assert collect(c)[pos] == off
    click(c, pos, 1, seq=10 * n + 2)
    collect(c)
    w = c.window
    assert (w["type"], w["title"]) == (block, {"translate": title}), w      # (the menu types are named as the blocks are)
    assert len(w["slots"]) == 39 and w["data"] == {0: 0, 1: 0, 2: 0, 3: 0}
    # What the other of the two cooks is nothing to this one: a shift-click sends it up into the inventory.
    menu_click(c, 33, 0, QUICK_MOVE)
    settle(c)
    assert w["slots"][0] is None and w["slots"][3] == plain(other, 3), w["slots"][:4]
    menu_click(c, 31, 0, QUICK_MOVE)
    menu_click(c, 32, 0, QUICK_MOVE)
    changes = collect(c, ticks=4)
    assert changes[pos] == off - 1, changes         # lit
    assert w["slots"][0] == plain(cooks, count) and w["slots"][2] is None
    assert w["slots"][1] == (plain("kOakPlanks", planks - 1) if planks > 1 else None)
    # A plank burns for 150 ticks here, not 300, and what cooks takes 100, not 200.
    assert w["data"][1] == 150 and w["data"][3] == 100 and 0 < w["data"][2] < 50 and w["data"][0] == 151 - w["data"][2], w["data"]
    return pos, off


@test()
def smoker_and_blast_furnace(srv: Server):
    """A smoker and a blast furnace are furnaces with recipes of their own - meat for the one, ore
    for the other - a menu of their own type and name, and twice the speed on half the fuel."""
    c = srv.client()
    join(c, "Alice")
    c.send("player_loaded")
    settle(c)
    smokes, blasts = c.recipe_sets["minecraft:smoker_input"], c.recipe_sets["minecraft:blast_furnace_input"]
    assert I["kBeef"] in smokes and I["kIronOre"] not in smokes and I["kCobblestone"] not in smokes
    assert I["kIronOre"] in blasts and I["kBeef"] not in blasts and I["kCobblestone"] not in blasts
    for n in range(2):
        cooker_setup(c, n, count=3, planks=2)
        close_window(c)
        settle(c)
        for slot in range(9, 45):               # (an empty inventory for the next one)
            give(c, slot, 0, 0)
    # Who joins is told of both, each by its own type.
    d = srv.client()
    jd = join(d, "Dave", view=2)
    assert sorted(jd["chunks"][(0, 0)]["block_entities"]) == [((8, -60, 10), mc.BLOCK_ENTITY["kSmoker"], None),
                                                               ((10, -60, 10), mc.BLOCK_ENTITY["kBlastFurnace"], None)]


@test()
def smoker_smokes(srv: Server):
    """One piece of beef, one plank, in a smoker: a steak after 100 ticks, and the fire out after
    150. An ingot from a blast furnace likewise."""
    c = srv.client()
    join(c, "Alice")
    c.send("player_loaded")
    settle(c)
    for n in range(2):
        pos, off = cooker_setup(c, n, count=1, planks=1)
        w = c.window
        changes = {}
        for _ in range(40):
            changes.update(collect(c, ticks=3))
            if w["slots"][2]:
                break
        assert w["slots"][2] == plain(COOKERS[n][3], 1) and w["slots"][0] is None and w["data"][2] == 0, (w["slots"][:3], w["data"])
        assert 10 < w["data"][0] <= 51 and pos not in changes, (w["data"], changes)
        for _ in range(25):
            changes.update(collect(c, ticks=3))
            if pos in changes:
                break
        assert changes[pos] == off and w["data"][0] == 0, (changes, w["data"])
        close_window(c)
        settle(c)
        for slot in range(9, 45):
            give(c, slot, 0, 0)


@test()
def barrel(srv: Server):
    """A barrel is a chest's 27 slots without a lid: it shows that it is open by its state, has
    sounds of its own, opens whatever stands on it, and its mouth is towards whoever placed it."""
    a, b = srv.client(), srv.client()
    join(a, "Alice")
    join(b, "Bob")
    a.expect("player_info_update")
    for cl in (a, b):
        cl.send("player_loaded")
    settle(a, b)
    ground, pos = (8, -61, 10), (8, -60, 10)
    give(a, 36, I["kBarrel"], 2)
    give(a, 37, I["kStone"], 20)
    click(a, ground, 1, seq=1)
    shut = mc.BLOCK["kBarrel"]                  # the default state: facing north - at Alice, who looks south - and shut
    assert collect(a)[pos] == shut
    collect(b)
    a.send("set_carried_item", struct.pack(">h", 1))
    a.send("player_input", bytes([INPUT_SHIFT]))    # a stone on top of it does not keep it shut
    click(a, pos, 1, seq=2)
    assert collect(a)[(8, -59, 10)] == mc.BLOCK["kStone"]
    a.send("player_input", bytes([0]))
    collect(b)

    click(a, pos, 2, seq=3, cursor=(0.5, 0.5, 0.0))
    changes = collect(a)
    w = a.window
    assert w and (w["type"], w["title"]) == ("kGeneric9x3", {"translate": "container.barrel"}) and len(w["slots"]) == 63, w
    assert changes[pos] == shut - 1 and collect(b)[pos] == shut - 1         # `open`
    sound, source, where, volume, pitch = b.sounds[-1]                      # from the middle of the face that opens
    assert (sound, source, where, volume) == (mc.SOUND["kBlockBarrelOpen"], 4, (8.5, -59.5, 10.0), 0.5) and 0.9 <= pitch < 1.0
    assert not a.block_events and not b.block_events                        # no lid: nothing to tell about one
    menu_click(a, 55, 0, QUICK_MOVE)
    settle(a)
    assert w["slots"][0] == plain("kStone", 20)

    # Bob opens it too: it was open already. It shuts when the last of them has closed it.
    heard = len(a.sounds)
    click(b, pos, 2, seq=1, cursor=(0.5, 0.5, 0.0))
    assert pos not in collect(a)
    collect(b)
    assert b.window["slots"][0] == plain("kStone", 20) and len(a.sounds) == heard
    close_window(a)
    assert pos not in collect(a, ticks=8) and len(a.sounds) == heard     # (the recount of who has it open has come by)
    collect(b)
    close_window(b)
    assert collect(a)[pos] == shut
    assert a.sounds[heard][:4] == (mc.SOUND["kBlockBarrelClose"], 4, (8.5, -59.5, 10.0), 0.5)
    assert not a.block_events

    # Looking down as it is placed: its mouth is up, and so is where it is heard from.
    look(a, 0.0, DOWN)
    a.send("set_carried_item", struct.pack(">h", 0))
    click(a, (10, -61, 9), 1, seq=4)
    up = shut + 8                               # facing: north, east, south, west, up, down; then open
    assert collect(a)[(10, -60, 9)] == up
    click(a, (10, -60, 9), 1, seq=5)
    assert collect(a)[(10, -60, 9)] == up - 1
    assert a.sounds[-1][:4] == (mc.SOUND["kBlockBarrelOpen"], 4, (10.5, -59.0, 9.5), 0.5), a.sounds[-1]
    close_window(a)
    assert collect(a)[(10, -60, 9)] == up
    collect(b)

    # Broken, it spills what it holds.
    dig(b, pos, seq=2)
    assert collect(a)[pos] == AIR
    # (Containers.dropItemStack throws a stack out in pieces of 10 to 30: twenty stones are one item or
    # two - which may merge, and be shown as one gone before the other is shown its new count.)
    for _ in range(40):
        spilled = [e.data[D_ITEM][1] for e in of_type(a, "kItem").values() if e.data[D_ITEM][0] == STACK]
        if sum(s[1] for s in spilled) == 20:
            break
        collect(a, ticks=1)
    assert spilled and all(s[0] == I["kStone"] and s[2] == {} for s in spilled) and sum(s[1] for s in spilled) == 20, spilled
    d = srv.client()
    jd = join(d, "Dave", view=2)
    assert jd["chunks"][(0, 0)]["block_entities"] == [((10, -60, 9), mc.BLOCK_ENTITY["kBarrel"], None)]


LEFT, RIGHT = 2, 4      # a chest's `type`, counted from its default state (single, dry): props::chest


def put_stone_in(c: Client, pos: tuple, seq: int, hotbar: int, sound: str, title: str = "container.chest") -> None:
    """Opens the chest at pos - heard as `sound` -, shift-clicks the stone of that hotbar slot into it, closes it."""
    click(c, pos, 1, seq=seq)
    collect(c)
    assert c.window and c.window["title"] == {"translate": title}, c.window
    assert c.sounds[-1][0] == mc.SOUND[sound], (c.sounds[-1], mc.SOUND[sound])
    menu_click(c, len(c.window["slots"]) - 9 + hotbar, 0, QUICK_MOVE)
    settle(c)
    close_window(c)
    settle(c)


@test()
def copper_chests(srv: Server):
    """Copper chests join each other whatever their age - the pair is then of the less weathered
    block, and of no wax if only one had any - and keep what they hold as their block changes.
    They are heard as copper of their age, and are nothing to a chest of wood."""
    a = srv.client()
    join(a, "Alice")
    a.send("player_loaded")
    settle(a)
    copper, exposed, oxidized, wooden = (mc.BLOCK[k] for k in ("kCopperChest", "kExposedCopperChest", "kOxidizedCopperChest", "kChest"))
    for slot, (item, count) in enumerate((("kOxidizedCopperChest", 4), ("kCopperChest", 4), ("kStone", 20),
                                          ("kWaxedOxidizedCopperChest", 2), ("kExposedCopperChest", 2), ("kChest", 2),
                                          ("kWaxedCopperChest", 2))):
        give(a, 36 + slot, I[item], count)
    hold = lambda slot: a.send("set_carried_item", struct.pack(">h", slot))  # noqa: E731

    # An oxidized one with stone in it, then a new one beside it: both are new copper, and the stone is there.
    click(a, (8, -61, 10), 1, seq=1)
    assert collect(a)[(8, -60, 10)] == oxidized
    put_stone_in(a, (8, -60, 10), 2, 2, "kBlockCopperChestOxidizedOpen")
    assert a.sounds[-1][0] == mc.SOUND["kBlockCopperChestOxidizedClose"]
    hold(1)
    click(a, (9, -61, 10), 1, seq=3)
    assert shows(collect(a), {(8, -60, 10): copper + LEFT, (9, -60, 10): copper + RIGHT})
    click(a, (9, -60, 10), 1, seq=4)
    collect(a)
    w = a.window
    assert (w["type"], w["title"]) == ("kGeneric9x6", {"translate": "container.chestDouble"}), w
    assert w["slots"][27] == plain("kStone", 20) and a.sounds[-1][0] == mc.SOUND["kBlockCopperChestOpen"]
    close_window(a)
    settle(a)
    assert not of_type(a, "kItem")                      # nothing was spilled as the block became another

    # Wood and copper stand side by side, each a chest of its own.
    hold(5)
    click(a, (8, -61, 12), 1, seq=5)
    hold(1)
    click(a, (9, -61, 12), 1, seq=6)
    changes = collect(a)
    assert changes[(8, -60, 12)] == wooden and changes[(9, -60, 12)] == copper, changes

    # Wax on one of the two: the pair has none. Here the new one is the less weathered...
    hold(3)
    click(a, (11, -61, 10), 1, seq=7)
    assert collect(a)[(11, -60, 10)] == mc.BLOCK["kWaxedOxidizedCopperChest"]
    hold(4)
    click(a, (12, -61, 10), 1, seq=8)
    assert shows(collect(a), {(11, -60, 10): exposed + LEFT, (12, -60, 10): exposed + RIGHT})
    # ...and here the one that stood: the pair is the block that one is without its wax.
    hold(6)
    click(a, (11, -61, 12), 1, seq=9)
    assert collect(a)[(11, -60, 12)] == mc.BLOCK["kWaxedCopperChest"]
    hold(0)
    click(a, (12, -61, 12), 1, seq=10)
    assert shows(collect(a), {(11, -60, 12): copper + LEFT, (12, -60, 12): copper + RIGHT})
    # Two with wax keep it: the pair is the less weathered of them, waxed.
    hold(3)
    click(a, (5, -61, 10), 1, seq=11)
    hold(6)
    click(a, (6, -61, 10), 1, seq=12)
    waxed = mc.BLOCK["kWaxedCopperChest"]
    assert shows(collect(a), {(5, -60, 10): waxed + LEFT, (6, -60, 10): waxed + RIGHT})


@test(random_tick_speed=4096, simulation_distance=2)
def copper_chest_weathers(srv: Server):
    """A copper chest weathers as copper does, and is the same chest after it: with what it held.
    Not while somebody has it open. Of a pair the left half weathers, and the right one with it."""
    a = srv.client()
    join(a, "Alice", view=2)
    a.send("player_loaded")
    settle(a)
    copper = mc.BLOCK["kCopperChest"]
    aged = [mc.BLOCK[k] for k in ("kExposedCopperChest", "kWeatheredCopperChest", "kOxidizedCopperChest")]
    held, shut, left, right = (4, -60, 8), (12, -60, 8), (8, -60, 12), (9, -60, 12)   # (far enough apart not to count each other)
    oxidized = mc.BLOCK["kOxidizedCopperChest"]
    give(a, 36, I["kCopperChest"], 4)
    give(a, 37, I["kStone"], 20)
    seen = {}                                   # the blocks as Alice knows them: they change whenever chance has it

    def wait(ticks: int = 3) -> None:
        seen.update(collect(a, ticks=ticks))
    for n, pos in enumerate((shut, left, right)):
        click(a, (pos[0], -61, pos[2]), 1, seq=1 + n)
    wait()
    assert seen[left] - LEFT == seen[right] - RIGHT and seen[left] - LEFT in (copper, *aged), seen
    click(a, shut, 1, seq=4)                    # stone into one of them
    wait()
    menu_click(a, 55, 0, QUICK_MOVE)
    wait()
    assert a.window and a.window["slots"][0] == plain("kStone", 20), a.window
    close_window(a)
    wait()
    # ...and another one, open for as long as the others take - opened as soon as it stands: copper
    # oxidizes in some eighty of these ticks, and one that had would never be seen to weather.
    click(a, (held[0], -61, held[2]), 1, seq=5)
    wait()
    click(a, held, 1, seq=6)
    wait()
    assert a.window and seen[held] != oxidized, (a.window, seen)
    frozen = seen[held]
    for _ in range(1200):
        wait(1)
        # The halves of a pair are one block whenever a client is told of them: the right one follows in the same tick.
        assert seen[left] - LEFT == seen[right] - RIGHT, seen
        if seen[shut] == oxidized and seen[left] != copper + LEFT:
            break
    else:
        raise AssertionError(f"never weathered: {seen}")
    assert seen[held] == frozen and a.window, (seen, frozen)
    close_window(a)
    for _ in range(1200):
        wait(1)
        if seen[held] != frozen:
            break
    else:
        raise AssertionError(f"the chest that was open never weathered once it was shut: {seen}")
    # Each is the chest it was: the stone is in the one it was put into, which is heard as old copper now.
    click(a, shut, 1, seq=7)
    wait()
    assert a.window and a.window["slots"][0] == plain("kStone", 20), a.window
    assert a.sounds[-1][0] == mc.SOUND["kBlockCopperChestOxidizedOpen"], a.sounds[-1]
    assert not of_type(a, "kItem")


@test()
def plants(srv: Server):
    """A flower stands on what its kind grows on and breaks - seen, heard, dropped - when that goes.
    Petals and leaf litter lie up to four to a block, each placed onto the ones before it."""
    a = srv.client()
    join(a, "Alice")
    a.send("player_loaded")
    settle(a)
    poppy, petals, litter, stone, grass = (mc.BLOCK[k] for k in ("kPoppy", "kPinkPetals", "kLeafLitter", "kStone", "kShortGrass"))
    for slot, (item, count) in enumerate((("kPoppy", 8), ("kStone", 8), ("kPinkPetals", 8), ("kLeafLitter", 8),
                                          ("kShortGrass", 4), ("kDeadBush", 4), ("kSand", 4))):
        give(a, 36 + slot, I[item], count)
    hold = lambda slot: a.send("set_carried_item", struct.pack(">h", slot))  # noqa: E731

    click(a, (8, -61, 12), 1, seq=1)            # a poppy on the grass
    assert collect(a)[(8, -60, 12)] == poppy
    hold(1)
    click(a, (10, -61, 12), 1, seq=2)           # ...and none on stone
    hold(0)
    click(a, (10, -60, 12), 1, seq=3)
    changes = collect(a)
    assert changes[(10, -60, 12)] == stone and changes.get((10, -59, 12), AIR) == AIR, changes
    # A dead bush wants sand (or soil); stone is nothing to it either.
    hold(5)
    click(a, (10, -60, 12), 1, seq=4)
    assert collect(a).get((10, -59, 12), AIR) == AIR
    hold(6)
    click(a, (12, -61, 12), 1, seq=5)
    hold(5)
    click(a, (12, -60, 12), 1, seq=6)
    assert shows(collect(a), {(12, -60, 12): mc.BLOCK["kSand"], (12, -59, 12): mc.BLOCK["kDeadBush"]})

    # Leaf litter lies on anything with a whole top. One, and three more onto it: the amount counts up.
    hold(3)
    click(a, (10, -60, 12), 1, seq=7)
    assert collect(a)[(10, -59, 12)] == litter  # facing north (at Alice), one of it
    for n in (2, 3, 4):
        click(a, (10, -59, 12), 1, seq=6 + n)
        assert collect(a)[(10, -59, 12)] == litter + n - 1
    click(a, (10, -59, 12), 1, seq=11)          # a fifth has nowhere to go: not into the block, not on top of it
    changes = collect(a)
    assert changes[(10, -59, 12)] == litter + 3 and changes.get((10, -58, 12), AIR) == AIR, changes
    # Petals the same, placed by a click on the ground under them - but not by one who sneaks.
    hold(2)
    for n in (1, 2, 3):
        click(a, (8, -61, 14), 1, seq=11 + n)
        assert collect(a)[(8, -60, 14)] == petals + n - 1
    a.send("player_input", bytes([INPUT_SHIFT]))
    click(a, (8, -61, 14), 1, seq=15)
    assert collect(a)[(8, -60, 14)] == petals + 2
    a.send("player_input", bytes([0]))
    click(a, (8, -61, 14), 1, seq=16)
    assert collect(a)[(8, -60, 14)] == petals + 3
    assert not a.level_events and not of_type(a, "kItem")

    # Short grass and leaf litter give way to a block that is set into them; petals and flowers do not.
    hold(4)
    click(a, (6, -61, 12), 1, seq=17)
    assert collect(a)[(6, -60, 12)] == grass
    click(a, (6, -60, 12), 1, seq=18)           # not to more of itself, though
    assert collect(a)[(6, -60, 12)] == grass
    hold(1)
    click(a, (6, -60, 12), 1, seq=19)
    click(a, (10, -59, 12), 1, seq=20)
    click(a, (8, -60, 12), 2, seq=21, cursor=(0.5, 0.5, 0.0))      # against the poppy's side: beside it
    assert shows(collect(a), {(6, -60, 12): stone, (10, -59, 12): stone, (8, -60, 12): poppy, (8, -60, 11): stone})
    assert not a.level_events and not of_type(a, "kItem")           # what was replaced is gone, not broken

    # The ground under them goes: the poppy breaks and drops, and the petals as many as there were.
    hold(8)
    dig(a, (8, -61, 12), seq=22)
    assert shows(collect(a), {(8, -61, 12): AIR, (8, -60, 12): AIR})
    dig(a, (8, -61, 14), seq=23)
    assert shows(collect(a), {(8, -61, 14): AIR, (8, -60, 14): AIR})
    assert a.level_events == [(DESTROY_BLOCK, (8, -60, 12), poppy, False), (DESTROY_BLOCK, (8, -60, 14), petals + 3, False)]
    settle(a)
    assert [e.data[D_ITEM][1] for e in items_of(a, "kPoppy")] == [plain("kPoppy", 1)]
    assert [e.data[D_ITEM][1] for e in items_of(a, "kPinkPetals")] == [plain("kPinkPetals", 4)]


@test()
def crafting(srv: Server):
    """The 2x2 grid of the inventory and the 3x3 of a crafting table: the result follows the grid,
    taking it uses the ingredients up."""
    c = srv.client()
    join(c, "Alice")
    c.send("player_loaded")
    settle(c)
    # A log in the inventory's grid (the creative screen has none, but the slots are there).
    mark = len(c.menu_log)
    give(c, 1, I["kOakLog"], 2)
    c.slots[1] = plain("kOakLog", 2)
    settle(c)
    assert c.menu_log[mark:] == [("container_set_slot", 0, plain("kOakPlanks", 4))], c.menu_log[mark:]
    mark = len(c.menu_log)
    menu_click(c, 0, 0, PICKUP)              # take the planks: one log is used, the other makes four more
    settle(c)
    assert c.menu_log[mark:] == [("container_set_slot", 0, plain("kOakPlanks", 4)), ("container_set_slot", 1, plain("kOakLog", 1)),
                                 ("set_cursor_item", plain("kOakPlanks", 4))], c.menu_log[mark:]
    menu_click(c, 36, 0, PICKUP)             # put them down in the hotbar
    settle(c)
    assert c.slots[36] == plain("kOakPlanks", 4)

    # A crafting table: placed with the planks' neighbour slot, opened by a click.
    ground, pos = (8, -61, 10), (8, -60, 10)
    give(c, 37, I["kCraftingTable"])
    c.send("set_carried_item", struct.pack(">h", 1))
    click(c, ground, 1, seq=1)
    assert collect(c)[pos] == mc.BLOCK["kCraftingTable"]
    click(c, pos, 1, seq=2)
    collect(c)
    w = c.window
    assert (w["id"], w["type"], w["title"]) == (1, "kCrafting", {"translate": "container.crafting"}) and len(w["slots"]) == 46
    assert w["slots"][37] == plain("kOakPlanks", 4)
    # Four planks in a square, one at a time: a button, a pressure plate, nothing, a table.
    menu_click(c, 37, 0, PICKUP)
    seen = []
    for slot in (1, 2, 4, 5):
        mark = len(c.menu_log)
        menu_click(c, slot, 1, PICKUP)
        settle(c)
        seen.append(next(m[2] for m in c.menu_log[mark:] if m[:2] == ("container_set_slot", 0)))
    assert seen == [plain("kOakButton"), plain("kOakPressurePlate"), None, plain("kCraftingTable")], seen
    menu_click(c, 0, 0, QUICK_MOVE)          # shift-click: onto the table already in the hotbar; the grid is used up
    settle(c)
    assert w["slots"][0] is None and w["slots"][1:10] == [None] * 9 and w["slots"][38] == plain("kCraftingTable", 2)

    # What is left in the grid comes back when the screen closes.
    menu_click(c, 38, 0, PICKUP)
    menu_click(c, 9, 0, PICKUP)
    settle(c)
    assert w["slots"][9] == plain("kCraftingTable", 2) and w["slots"][0] is None     # two tables make nothing
    close_window(c)
    settle(c)
    assert c.slots[36] == plain("kCraftingTable", 2) and server_menu(c)[36] == plain("kCraftingTable", 2)

    # Walking away closes it from the server's side (CraftingMenu.stillValid: 4 blocks past reach).
    click(c, pos, 1, seq=3)
    collect(c)
    assert c.window["id"] == 2
    c.send("move_player_pos", struct.pack(">dddB", 0.5, SPAWN[1], 0.5, 1))     # twelve blocks off
    settle(c)
    assert c.window is None and c.menu_log[-1] == ("container_close", 2)


@test()
def tnt(srv: Server):
    """TNT: lit with flint and steel it becomes an entity that hops, lands and - 80 ticks later -
    takes the ground with it, sets off the TNT beside it and pushes who stands near."""
    a, b = srv.client(), srv.client()
    ja = join(a, "Alice")
    jb = join(b, "Bob")
    a.expect("player_info_update")
    for cl in (a, b):
        cl.send("player_loaded")
    walk(b, jb, 8.5, 26.5, 10)               # Bob watches from 14 blocks behind the TNT
    settle(a, b)
    ground, pos, pos2 = (8, -61, 12), (8, -60, 12), (9, -60, 12)
    give(a, 36, I["kTnt"], 2)
    give(a, 37, I["kFlintAndSteel"])
    click(a, ground, 1, seq=1)
    click(a, (9, -61, 12), 1, seq=2)
    changes = collect(a)
    assert changes[pos] == mc.BLOCK["kTnt"] and changes[pos2] == mc.BLOCK["kTnt"]
    collect(b)
    assert not of_type(b, "kTnt")            # TNT is a block like any other until something lights it

    a.send("set_carried_item", struct.pack(">h", 1))
    click(a, pos, 1, seq=4)                  # flint and steel
    assert collect(a)[pos] == AIR
    collect(b)
    (eid, e), = of_type(b, "kTnt").items()
    assert abs(e.pos[0] - 8.5) < 0.2 and abs(e.pos[2] - 12.5) < 0.2, e.pos
    sound, source, where, volume, pitch = b.sounds[-1]
    assert (sound, source, where, volume, pitch) == (mc.SOUND["kEntityTntPrimed"], 4, (8.5, -60.0, 12.5), 1.0, 1.0)
    settle(a, b, ticks=30)
    e = b.entities[eid]
    hops = [w[1][1] for n, i, w in b.entity_log if i == eid and n == "move_entity_pos"]
    assert max(hops) > -59.7, hops                                       # it hopped: a third of a block up
    assert e.pos[1] == -60.0 and e.on_ground, e.pos                      # ...and landed
    assert 20 <= e.data[mc.TNT_DATA["kFuse"]][1] <= 60, e.data            # the fuse burns

    changes, bchanges = {}, {}
    for _ in range(40):
        changes.update(collect(a, ticks=2))
        bchanges.update(collect(b, ticks=2))
        if len(a.explosions) == 2:
            break
    changes.update(collect(a))               # the blocks of an explosion follow it by a tick
    bchanges.update(collect(b))
    assert len(a.explosions) == 2 and len(b.explosions) == 2
    ex = a.explosions[0]
    assert abs(ex["center"][0] - 8.5) < 0.5 and abs(ex["center"][2] - 12.5) < 0.5 and abs(ex["center"][1] + 59.93875) < 1e-6, ex
    assert ex["radius"] == 4.0 and ex["blocks"] > 50 and ex["play_sound"]
    assert (ex["particle"], ex["sound"]) == (mc.PARTICLE["kExplosionEmitter"], mc.SOUND["kEntityGenericExplode"])
    assert ex["block_particles"] == [(mc.PARTICLE["kPoof"], 0.5, 1.0, 1), (mc.PARTICLE["kSmoke"], 1.0, 1.0, 1)]
    # Alice, four blocks off and on her feet, is pushed away from it; Bob is too far to feel it.
    assert ex["knockback"] and ex["knockback"][2] < -0.1 and abs(ex["knockback"][0]) < 0.1, ex["knockback"]
    assert b.explosions[0]["knockback"] is None and b.explosions[0]["center"] == ex["center"]
    # The ground is gone - grass and dirt, not the bedrock under them - and lies about as items.
    assert changes[ground] == AIR and changes[(8, -63, 12)] == AIR and (8, -64, 12) not in changes, sorted(changes)[:8]
    assert bchanges == changes
    dirt = sum(e.data[D_ITEM][1][1] for e in of_type(b, "kItem").values() if e.data[D_ITEM][1][0] == I["kDirt"])
    assert dirt > 10 and not any(e.data[D_ITEM][1][0] == I["kGrassBlock"] for e in of_type(b, "kItem").values())
    # The TNT beside it did not drop: it went off, on a short fuse, a little later and elsewhere.
    assert not of_type(b, "kTnt") and not any(e.data[D_ITEM][1][0] == I["kTnt"] for e in of_type(b, "kItem").values())
    assert a.explosions[1]["center"] != ex["center"]

    # A block of redstone beside it sets TNT off the moment it is placed.
    give(a, 38, I["kRedstoneBlock"])
    a.send("set_carried_item", struct.pack(">h", 2))
    click(a, (8, -61, 6), 1, seq=5)
    assert collect(a)[(8, -60, 6)] == mc.BLOCK["kRedstoneBlock"]
    a.send("set_carried_item", struct.pack(">h", 0))
    click(a, (9, -61, 6), 1, seq=6)
    assert collect(a).get((9, -60, 6), AIR) == AIR
    collect(b)
    assert len(of_type(b, "kTnt")) == 1


# ----------------------------------------------------------------------------- block updates

DESTROY_BLOCK = 2001   # LevelEvent.PARTICLES_AND_SOUND_DESTROY_BLOCK
BLOCK_POS = mc.SERIALIZER["kBlockPos"]


def until(c: Client, done, ticks: int = 60, others: tuple = ()) -> dict:
    """Let server ticks pass until done(changes so far) says so; the block changes they brought."""
    changes = {}
    for _ in range(ticks):
        changes.update(collect(c, ticks=1))
        for o in others:
            collect(o, ticks=1)
        if done(changes):
            return changes
    raise AssertionError(f"never happened; block changes so far: {changes}")


def shows(changes: dict, expected: dict) -> bool:
    """The block changes a client was sent hold these. (They hold more: whoever clicks is told
    what the block it clicked, and the one beside it, are - changed or not.)"""
    assert all(changes.get(pos) == state for pos, state in expected.items()), (changes, expected)
    return True


def items_of(c: Client, item: str) -> list:
    return [e for e in of_type(c, "kItem").values() if e.data[D_ITEM][1][0] == I[item]]


@test()
def torches(srv: Server):
    """A torch stands on what holds it or hangs on a wall, by the face that is clicked and by where
    its player looks; when what held it goes it breaks - seen and heard by whoever is near - and
    drops. The neighbours of a block take their new shapes in vanilla's order."""
    a, b = srv.client(), srv.client()
    join(a, "Alice")
    jb = join(b, "Bob")
    a.expect("player_info_update")
    for cl in (a, b):
        cl.send("player_loaded")
    walk(b, jb, 8.5, 20.5, 10)
    settle(a, b)
    torch, wall, stone = mc.BLOCK["kTorch"], mc.BLOCK["kWallTorch"], mc.BLOCK["kStone"]   # wall: facing north, south, west, east
    give(a, 36, I["kStone"], 16)
    give(a, 37, I["kTorch"], 16)
    click(a, (8, -61, 12), 1, seq=1)                 # a pillar of two stones
    click(a, (8, -60, 12), 1, seq=2)
    assert shows(collect(a), {(8, -60, 12): stone, (8, -59, 12): stone})
    a.send("set_carried_item", struct.pack(">h", 1))
    click(a, (8, -59, 12), 1, seq=3)                 # on top of it: the torch stands
    click(a, (8, -59, 12), 2, seq=4)                 # against its sides: it hangs, facing away from the stone
    click(a, (8, -59, 12), 5, seq=5)
    click(a, (8, -59, 12), 3, seq=6)
    click(a, (8, -61, 14), 1, seq=7)                 # on the grass
    assert shows(collect(a), {(8, -58, 12): torch, (8, -59, 11): wall, (9, -59, 12): wall + 3, (8, -59, 13): wall + 1,
                              (8, -60, 14): torch})
    collect(b)
    # Nothing holds a torch on top of a torch, and there is no wall beside it.
    click(a, (8, -60, 14), 1, seq=8)
    assert collect(a).get((8, -59, 14), AIR) == AIR
    assert not a.level_events and not of_type(a, "kItem")

    # The stone that holds four of them goes. Its neighbours are asked west, east, north, south,
    # down, up (BlockBehaviour.UPDATE_SHAPE_ORDER): the torches break in that order.
    a.send("set_carried_item", struct.pack(">h", 0))
    dig(a, (8, -59, 12), seq=9)
    gone = collect(a)
    assert collect(b) == gone                            # (Bob clicked nothing: what he is told is what changed)
    assert gone == {(8, -59, 12): AIR, (9, -59, 12): AIR, (8, -59, 11): AIR, (8, -59, 13): AIR, (8, -58, 12): AIR}, gone
    broke = [(DESTROY_BLOCK, (9, -59, 12), wall + 3, False), (DESTROY_BLOCK, (8, -59, 11), wall, False),
             (DESTROY_BLOCK, (8, -59, 13), wall + 1, False), (DESTROY_BLOCK, (8, -58, 12), torch, False)]
    assert a.level_events == broke, a.level_events
    # Bob also sees the stone break under Alice's hand (her own client showed her that).
    assert b.level_events == [(DESTROY_BLOCK, (8, -59, 12), stone, False)] + broke, b.level_events
    settle(a, b)
    for cl in (a, b):
        dropped = items_of(cl, "kTorch")
        # Four torches - in how many stacks depends on how long this took: two that lie close merge.
        assert sum(e.data[D_ITEM][1][1] for e in dropped) == 4 and all(e.data[D_ITEM][1][2] == {} for e in dropped), [e.data for e in dropped]
    # The one on the grass stands until the grass goes.
    dig(a, (8, -61, 14), seq=10)
    assert shows(collect(a), {(8, -61, 14): AIR, (8, -60, 14): AIR})
    assert a.level_events[4:] == [(DESTROY_BLOCK, (8, -60, 14), torch, False)]

    # Under a block, where a torch cannot hang: it goes where its player looks. Level, towards a
    # wall in the south: onto that wall. Down: onto the ground.
    for x in (4, 12):
        click(a, (x, -61, 9), 1, seq=11)                 # a stone, one on it...
        click(a, (x, -60, 9), 1, seq=12)
        click(a, (x, -61, 10), 1, seq=13)                # ...and the wall to the south
    collect(a)
    dig(a, (4, -60, 9), seq=14)                          # the lower stones go: the upper ones hang there
    dig(a, (12, -60, 9), seq=15)
    collect(a)
    a.send("set_carried_item", struct.pack(">h", 1))
    look(a, 0.0, 0.0)
    click(a, (4, -59, 9), 0, seq=16, cursor=(0.5, 0.0, 0.5))
    assert shows(collect(a), {(4, -60, 9): wall})             # facing north, on the stone south of it
    look(a, 0.0, DOWN)
    click(a, (12, -59, 9), 0, seq=17, cursor=(0.5, 0.0, 0.5))
    assert shows(collect(a), {(12, -60, 9): torch})


@test()
def sand_falls(srv: Server):
    """Sand, gravel and concrete powder with nothing under them fall: two ticks after the block
    was placed, or lost its ground, it is an entity, and where it lands it is a block again -
    or an item, on what does not give way."""
    a, b = srv.client(), srv.client()
    join(a, "Alice")
    jb = join(b, "Bob")
    a.expect("player_info_update")
    for cl in (a, b):
        cl.send("player_loaded")
    walk(b, jb, 8.5, 22.5, 10)
    settle(a, b)
    sand, stone, powder = mc.BLOCK["kSand"], mc.BLOCK["kStone"], mc.BLOCK["kWhiteConcretePowder"]
    falling = lambda cl: of_type(cl, "kFallingBlock")
    give(a, 36, I["kStone"], 32)
    give(a, 37, I["kSand"], 8)
    give(a, 38, I["kWhiteConcretePowder"], 8)
    give(a, 39, I["kTorch"], 8)
    give(a, 40, I["kGravel"], 8)

    # On the ground it lies.
    a.send("set_carried_item", struct.pack(">h", 1))
    click(a, (6, -61, 12), 1, seq=1)
    assert shows(collect(a), {(6, -60, 12): sand})
    assert collect(b) == {(6, -60, 12): sand}
    assert collect(a, ticks=6) == {} and collect(b, ticks=6) == {}
    assert not falling(a) and not falling(b)

    # On a stone that goes: the sand above it follows two ticks later.
    a.send("set_carried_item", struct.pack(">h", 0))
    click(a, (8, -61, 12), 1, seq=2)
    a.send("set_carried_item", struct.pack(">h", 1))
    click(a, (8, -60, 12), 1, seq=3)
    assert shows(collect(a), {(8, -60, 12): stone, (8, -59, 12): sand})
    collect(b)
    dig(a, (8, -60, 12), seq=4)
    changes = until(a, lambda ch: ch.get((8, -59, 12)) == AIR, others=(b,))
    assert changes == {(8, -60, 12): AIR, (8, -59, 12): AIR}, changes
    collect(a, ticks=2), collect(b, ticks=2)
    for cl in (a, b):
        (eid, e), = falling(cl).items()
        assert e.object_data == sand and e.spawned[0] == (8.5, -59.0, 12.5), (e.object_data, e.spawned)
        assert e.data[mc.FALLING_DATA["kStartPos"]] == (BLOCK_POS, (8, -59, 12)), e.data
    assert not a.transient_blocks
    changes = until(a, lambda ch: ch.get((8, -60, 12)) == sand, others=(b,))
    assert changes == {(8, -60, 12): sand}, changes
    # (Told twice: at once, as somebody who saw the entity; and with the chunk's changes, a tick later.)
    assert collect(a) in ({}, {(8, -60, 12): sand}) and collect(b) in ({}, {(8, -60, 12): sand})
    for cl in (a, b):
        assert not falling(cl)                                   # the entity went as the block came
        assert cl.transient_blocks == [((8, -60, 12), sand)], cl.transient_blocks
        assert not of_type(cl, "kItem")

    # Placed where nothing is under it - against the side of a pillar - it falls at once, and lands as itself.
    a.send("set_carried_item", struct.pack(">h", 0))
    for seq, y in enumerate((-61, -60, -59), start=5):
        click(a, (10, y, 12), 1, seq=seq)
    collect(a)
    a.send("set_carried_item", struct.pack(">h", 2))
    click(a, (10, -58, 12), 5, seq=8)
    changes = until(a, lambda ch: ch.get((11, -60, 12)) == powder, others=(b,))
    assert shows(changes, {(11, -58, 12): AIR, (11, -60, 12): powder})   # (it was there, and went)
    collect(a), collect(b)
    assert not falling(a) and not falling(b)

    # Onto a torch: that does not give way. What fell breaks, and lies there as an item.
    a.send("set_carried_item", struct.pack(">h", 3))
    click(a, (9, -61, 12), 1, seq=9)
    assert shows(collect(a), {(9, -60, 12): mc.BLOCK["kTorch"]})
    a.send("set_carried_item", struct.pack(">h", 4))
    click(a, (10, -58, 12), 4, seq=10)                           # gravel, west of the pillar's top: over the torch
    until(a, lambda ch: ch.get((9, -58, 12)) == AIR, others=(b,))
    for _ in range(40):
        collect(a, ticks=2), collect(b, ticks=2)
        if not falling(a):
            break
    assert not falling(a) and not falling(b)
    settle(a, b)       # (what the tick that broke it sent the one who was read first)
    for cl in (a, b):
        # (One time in ten gravel is flint when it is mined; this one was not mined.)
        assert [e.data[D_ITEM][1] for e in of_type(cl, "kItem").values()] == [plain("kGravel", 1)]
    assert collect(a).get((9, -60, 12), mc.BLOCK["kTorch"]) == mc.BLOCK["kTorch"]


@test()
def redstone_lamp(srv: Server):
    """A redstone lamp is lit the moment a signal arrives, and goes out four ticks after it left -
    a neighbour update, and a scheduled tick."""
    a = srv.client()
    join(a, "Alice")
    a.send("player_loaded")
    settle(a)
    lit, unlit, redstone = mc.BLOCK["kRedstoneLamp"] - 1, mc.BLOCK["kRedstoneLamp"], mc.BLOCK["kRedstoneBlock"]
    give(a, 36, I["kRedstoneLamp"], 4)
    give(a, 37, I["kRedstoneBlock"], 4)
    click(a, (8, -61, 12), 1, seq=1)
    assert shows(collect(a), {(8, -60, 12): unlit})
    a.send("set_carried_item", struct.pack(">h", 1))
    click(a, (9, -61, 12), 1, seq=2)
    assert shows(collect(a), {(9, -60, 12): redstone, (8, -60, 12): lit})        # at once
    # Beside a signal a lamp is placed lit.
    a.send("set_carried_item", struct.pack(">h", 0))
    click(a, (10, -61, 12), 1, seq=3)
    assert shows(collect(a), {(10, -60, 12): lit})
    # The signal goes: both stay lit for four ticks, then go out together.
    dig(a, (9, -60, 12), seq=4)
    assert collect(a, ticks=3) == {(9, -60, 12): AIR}    # (the block's going, and for three ticks no more)
    assert until(a, lambda ch: len(ch) == 2, ticks=5) == {(8, -60, 12): unlit, (10, -60, 12): unlit}
    # A signal that comes back before the four ticks are over: the lamp never goes out.
    a.send("set_carried_item", struct.pack(">h", 1))
    click(a, (9, -61, 12), 1, seq=5)
    assert shows(collect(a), {(9, -60, 12): redstone, (8, -60, 12): lit, (10, -60, 12): lit})
    dig(a, (9, -60, 12), seq=6)
    click(a, (9, -61, 12), 1, seq=7)
    after = collect(a, ticks=8)
    assert after.get((9, -60, 12)) == redstone and (8, -60, 12) not in after and (10, -60, 12) not in after, after


@test(random_tick_speed=4096, simulation_distance=2)
def random_ticks(srv: Server):
    """Random ticks, a great many of them a tick so that nobody has to wait: grass takes bare
    dirt and dies under stone, copper weathers, amethyst buds."""
    a = srv.client()
    join(a, "Alice", view=2)
    a.send("player_loaded")
    settle(a)
    grass, dirt, stone = mc.BLOCK["kGrassBlock"], mc.BLOCK["kDirt"], mc.BLOCK["kStone"]
    settle(a, ticks=10)
    assert collect(a) == {}                              # a world of grass with nowhere to go: nothing happens
    # A hole in the grass: the dirt at its bottom is taken from all sides.
    dig(a, (8, -61, 12), seq=1)
    changes = until(a, lambda ch: ch.get((8, -62, 12)) == grass)
    assert changes == {(8, -61, 12): AIR, (8, -62, 12): grass}, changes
    # A stone on grass: dirt, under it.
    give(a, 36, I["kStone"], 4)
    give(a, 37, I["kDirt"], 4)
    give(a, 38, I["kCopperBlock"], 4)
    give(a, 39, I["kBuddingAmethyst"], 4)
    click(a, (10, -61, 12), 1, seq=2)
    changes = until(a, lambda ch: ch.get((10, -61, 12)) == dirt)
    assert shows(changes, {(10, -60, 12): stone, (10, -61, 12): dirt})
    # Dirt on grass: the grass under it dies, and the dirt on top is grass before long.
    a.send("set_carried_item", struct.pack(">h", 1))
    click(a, (6, -61, 12), 1, seq=3)
    until(a, lambda ch: ch.get((6, -61, 12)) == dirt and ch.get((6, -60, 12)) == grass)
    settle(a, ticks=10)
    assert collect(a) == {}                              # and then it is quiet again
    # Copper, by itself: stage by stage to the last. (Two random ticks may fall on it in one game
    # tick at this speed, so not every stage need be seen - but what is seen comes in order.)
    a.send("set_carried_item", struct.pack(">h", 2))
    click(a, (8, -61, 6), 1, seq=4)
    stages = [mc.BLOCK[k] for k in ("kCopperBlock", "kExposedCopper", "kWeatheredCopper", "kOxidizedCopper")]
    seen = []

    def weathered(ch: dict) -> bool:
        state = ch.get((8, -60, 6))
        if state is not None and state not in seen[-1:]:
            seen.append(state)
        return state == stages[-1]

    until(a, weathered, ticks=1200)
    assert seen == [s for s in stages if s in seen] and len(seen) >= 3, seen
    # Budding amethyst: small buds on its free sides, each facing away from it, and growing.
    a.send("set_carried_item", struct.pack(">h", 3))
    click(a, (11, -61, 6), 1, seq=5)
    # A state of the family: its block's first, + 2 x facing (north, east, south, west, up, down), + 1 when dry.
    facing = {(11, -60, 5): 0, (12, -60, 6): 1, (11, -60, 7): 2, (10, -60, 6): 3, (11, -59, 6): 4}
    buds = until(a, lambda ch: all(pos in ch for pos in facing), ticks=600)
    for pos, index in facing.items():
        kind = next(k for k in ("kSmallAmethystBud", "kMediumAmethystBud", "kLargeAmethystBud", "kAmethystCluster")
                    if 0 <= buds[pos] - (mc.BLOCK[k] - 9) < 12)
        # facing away from the block it grows on, and dry
        assert buds[pos] - (mc.BLOCK[kind] - 9) == index * 2 + 1, (pos, buds[pos], kind)


# ----------------------------------------------------------------------------- blocks whose state is their shape

# States of a family, counted from the block's default (mc.BLOCK) by the layout blocks.json gives
# its properties: in the report's order, the last one running fastest, `true` before `false`.
FACING4 = ("north", "south", "west", "east")
STAIR_SHAPES = ("straight", "inner_left", "inner_right", "outer_left", "outer_right")


def stairs(block: str, facing: str, half: str = "bottom", shape: str = "straight", wet: bool = False) -> int:
    """facing x half (top, bottom) x shape x waterlogged; the default is north, bottom, straight, dry."""
    return (mc.BLOCK[block] - 11 + FACING4.index(facing) * 20 + ("top", "bottom").index(half) * 10 +
            STAIR_SHAPES.index(shape) * 2 + (0 if wet else 1))


def slab(block: str, kind: str = "bottom", wet: bool = False) -> int:
    """type (top, bottom, double) x waterlogged; the default is the bottom half, dry."""
    return mc.BLOCK[block] - 3 + ("top", "bottom", "double").index(kind) * 2 + (0 if wet else 1)


def cross(block: str, arms: str = "", wet: bool = False) -> int:
    """A fence, bars, a pane: east, north, south, waterlogged, west; the default has no arms and is
    dry. `arms`: the letters of the sides that have one."""
    return (mc.BLOCK[block] - 31 + (0 if "e" in arms else 16) + (0 if "n" in arms else 8) + (0 if "s" in arms else 4) +
            (0 if wet else 2) + (0 if "w" in arms else 1))


def wall(block: str, up: bool = True, wet: bool = False, **sides: str) -> int:
    """east, north, south (none, low, tall) x up x waterlogged x west; the default is a post alone, dry."""
    def side(name: str) -> int:
        return ("none", "low", "tall").index(sides.get(name, "none"))
    return (mc.BLOCK[block] - 3 + side("east") * 108 + side("north") * 36 + side("south") * 12 + (0 if up else 6) +
            (0 if wet else 3) + side("west"))


def hold(c: Client, slot: int) -> None:
    c.send("set_carried_item", struct.pack(">h", slot))


def upper(face: int) -> tuple:
    """A click on the upper half of a side face."""
    return {2: (0.5, 0.75, 0.0), 3: (0.5, 0.75, 1.0), 4: (0.0, 0.75, 0.5), 5: (1.0, 0.75, 0.5)}[face]


def lower(face: int) -> tuple:
    return {2: (0.5, 0.25, 0.0), 3: (0.5, 0.25, 1.0), 4: (0.0, 0.25, 0.5), 5: (1.0, 0.25, 0.5)}[face]


@test()
def slabs(srv: Server):
    """A slab is the half that was clicked; the same slab into the free half makes one block of
    two, which drops two."""
    a, b = srv.client(), srv.client()
    join(a, "Alice")
    join(b, "Bob")
    a.expect("player_info_update")
    for cl in (a, b):
        cl.send("player_loaded")
    settle(a, b)
    make_op(srv, a, "Alice")
    stone = mc.BLOCK["kStone"]
    oak = lambda kind="bottom": slab("kOakSlab", kind)   # noqa: E731
    give(a, 36, I["kStone"], 16)
    give(a, 37, I["kOakSlab"], 16)
    give(a, 38, I["kStoneSlab"], 16)
    # On the ground: the bottom half. On that half: the block is full.
    hold(a, 1)
    click(a, (8, -61, 11), 1, seq=1)
    assert shows(collect(a), {(8, -60, 11): oak()})
    click(a, (8, -60, 11), 1, seq=2, cursor=(0.5, 0.5, 0.5))
    assert shows(collect(a), {(8, -60, 11): oak("double")})
    assert collect(b) == {(8, -60, 11): oak("double")}
    # A double slab takes no more: the next one lies on it.
    click(a, (8, -60, 11), 1, seq=3)
    assert shows(collect(a), {(8, -60, 11): oak("double"), (8, -59, 11): oak()})
    # Against the side of a block: the half that was clicked. And against the same side again,
    # the other half: the slab that is there is filled (BlockPlaceContext.canPlace asks it).
    hold(a, 0)
    click(a, (10, -61, 11), 1, seq=4)
    hold(a, 1)
    click(a, (10, -60, 11), 4, seq=5, cursor=upper(4))
    assert shows(collect(a), {(10, -60, 11): stone, (9, -60, 11): oak("top")})
    click(a, (10, -60, 11), 4, seq=6, cursor=lower(4))
    assert shows(collect(a), {(9, -60, 11): oak("double")})
    # ...but a click on the half itself fills it only from where its free half is: on a top
    # slab's upper half the click is against its side, and the slab goes beside it.
    click(a, (10, -60, 11), 5, seq=7, cursor=upper(5))
    assert shows(collect(a), {(11, -60, 11): oak("top")})
    click(a, (11, -60, 11), 3, seq=8, cursor=upper(3))
    assert shows(collect(a), {(11, -60, 11): oak("top"), (11, -60, 12): oak("top")})
    click(a, (11, -60, 11), 3, seq=9, cursor=lower(3))
    assert shows(collect(a), {(11, -60, 11): oak("double")})
    # Under a block: the top half; and from below into that half.
    hold(a, 0)
    click(a, (6, -61, 11), 1, seq=10)
    click(a, (6, -60, 11), 1, seq=11)
    dig(a, (6, -60, 11), seq=12)
    hold(a, 1)
    click(a, (6, -59, 11), 0, seq=13, cursor=(0.5, 0.0, 0.5))
    assert shows(collect(a), {(6, -60, 11): oak("top")})
    click(a, (6, -60, 11), 0, seq=14, cursor=(0.5, 0.5, 0.5))
    assert shows(collect(a), {(6, -60, 11): oak("double")})
    # Another kind of slab is another block: it does not fill the half, it lies on it.
    click(a, (11, -61, 9), 1, seq=15)
    hold(a, 2)
    click(a, (11, -60, 9), 1, seq=16, cursor=(0.5, 0.5, 0.5))
    assert shows(collect(a), {(11, -60, 9): oak(), (11, -59, 9): slab("kStoneSlab")})
    collect(b)
    # What a double slab drops: two, as one stack. A half drops one.
    got = run_command(a, "setblock 9 -60 11 air destroy")
    assert got["blocks"] == {(9, -60, 11): AIR}, got
    got = run_command(a, "setblock 11 -60 12 air destroy")
    assert got["blocks"] == {(11, -60, 12): AIR}, got
    collect(b)
    for cl in (a, b):
        dropped = sorted(e.data[D_ITEM][1] for e in items_of(cl, "kOakSlab"))
        assert dropped == [plain("kOakSlab", 1), plain("kOakSlab", 2)], dropped
    # A command sets any of the three.
    got = run_command(a, "setblock 9 -60 11 oak_slab[type=double]")
    assert got["blocks"] == {(9, -60, 11): oak("double")}, got


@test()
def stairs_turn_corners(srv: Server):
    """Stairs face the way their player looks, upside down where the upper half was clicked, and
    turn a corner where the stairs behind or in front of them stand across - again whenever a
    block beside them changes."""
    a, b = srv.client(), srv.client()
    join(a, "Alice")
    join(b, "Bob")
    a.expect("player_info_update")
    for cl in (a, b):
        cl.send("player_loaded")
    settle(a, b)
    oak = lambda *args: stairs("kOakStairs", *args)   # noqa: E731
    give(a, 36, I["kStone"], 16)
    give(a, 37, I["kOakStairs"], 16)
    hold(a, 1)
    look(a, 0.0, 0.0)                                   # south
    click(a, (8, -61, 5), 1, seq=1)
    assert shows(collect(a), {(8, -60, 5): oak("south")})
    # Stairs behind them (south of stairs that face south), across: an outer corner.
    look(a, -90.0, 0.0)                                 # east
    click(a, (8, -61, 6), 1, seq=2)
    changes = collect(a)
    assert shows(changes, {(8, -60, 6): oak("east"), (8, -60, 5): oak("south", "bottom", "outer_left")})
    assert collect(b) == {(8, -60, 6): oak("east"), (8, -60, 5): oak("south", "bottom", "outer_left")}
    # They go: straight again.
    dig(a, (8, -60, 6), seq=3)
    assert collect(a) == {(8, -60, 6): AIR, (8, -60, 5): oak("south")}
    # Stairs in front of them, across: an inner corner.
    look(a, 90.0, 0.0)                                  # west
    click(a, (8, -61, 4), 1, seq=4)
    assert shows(collect(a), {(8, -60, 4): oak("west"), (8, -60, 5): oak("south", "bottom", "inner_right")})
    # ...unless the stairs on that side of them go on as they do (canTakeShape): a straight run.
    look(a, 0.0, 0.0)
    click(a, (7, -61, 5), 1, seq=5)
    assert shows(collect(a), {(7, -60, 5): oak("south"), (8, -60, 5): oak("south")})
    dig(a, (7, -60, 5), seq=6)
    assert collect(a) == {(7, -60, 5): AIR, (8, -60, 5): oak("south", "bottom", "inner_right")}
    # Stairs that are placed into a corner take it at once: facing north, with the stairs that
    # face west in front of them (south of them).
    look(a, 180.0, 0.0)                                 # north
    dig(a, (8, -60, 5), seq=7)
    collect(a)
    click(a, (8, -61, 3), 1, seq=8)
    assert shows(collect(a), {(8, -60, 3): oak("north", "bottom", "inner_left")})
    # The upper half of a side, or the underside of a block: upside down. Halves do not corner each other.
    hold(a, 0)
    click(a, (10, -61, 11), 1, seq=9)
    hold(a, 1)
    look(a, 0.0, 0.0)
    click(a, (10, -60, 11), 5, seq=10, cursor=upper(5))
    assert shows(collect(a), {(11, -60, 11): oak("south", "top")})
    click(a, (10, -60, 11), 4, seq=11, cursor=lower(4))
    assert shows(collect(a), {(9, -60, 11): oak("south")})
    look(a, -90.0, 0.0)
    click(a, (11, -60, 11), 3, seq=12, cursor=lower(3))     # south of the upside-down ones, the right way up
    assert shows(collect(a), {(11, -60, 12): oak("east"), (11, -60, 11): oak("south", "top")})
    collect(b)
    assert not a.level_events and not of_type(b, "kItem")


@test()
def fences_panes_and_walls(srv: Server):
    """A fence, a pane and a wall reach for what stands beside them - a whole face, their own
    kind - and let go when it leaves; a wall's sides are tall under a block, and it has a post
    where it ends, turns, or carries something that wants one."""
    a, b = srv.client(), srv.client()
    join(a, "Alice")
    join(b, "Bob")
    a.expect("player_info_update")
    for cl in (a, b):
        cl.send("player_loaded")
    settle(a, b)
    stone = mc.BLOCK["kStone"]
    fence = lambda arms="": cross("kOakFence", arms)                  # noqa: E731
    pane = lambda arms="": cross("kGlassPane", arms)                  # noqa: E731
    cobble = lambda **sides: wall("kCobblestoneWall", **sides)        # noqa: E731
    for slot, item in enumerate(("kStone", "kOakFence", "kNetherBrickFence", "kGlassPane", "kCobblestoneWall", "kTorch", "kIronBars")):
        give(a, 36 + slot, I[item], 16)
    click(a, (8, -61, 11), 1, seq=1)
    hold(a, 1)
    click(a, (9, -61, 11), 1, seq=2)
    assert shows(collect(a), {(8, -60, 11): stone, (9, -60, 11): fence("w")})
    click(a, (10, -61, 11), 1, seq=3)
    assert shows(collect(a), {(10, -60, 11): fence("w"), (9, -60, 11): fence("ew")})
    assert collect(b) == {(8, -60, 11): stone, (10, -60, 11): fence("w"), (9, -60, 11): fence("ew")}
    # A fence of brick and a fence of wood are not one fence.
    hold(a, 2)
    click(a, (11, -61, 11), 1, seq=4)
    changes = collect(a)
    assert shows(changes, {(11, -60, 11): cross("kNetherBrickFence")}) and (10, -60, 11) not in changes
    # Panes hold on to a whole face and to each other - not to a fence, nor a fence to them.
    hold(a, 3)
    click(a, (8, -61, 12), 1, seq=5)
    assert shows(collect(a), {(8, -60, 12): pane("n")})
    click(a, (9, -61, 12), 1, seq=6)
    changes = collect(a)
    assert shows(changes, {(9, -60, 12): pane("w"), (8, -60, 12): pane("en")}) and (9, -60, 11) not in changes
    hold(a, 6)                                               # iron bars are panes to a pane
    click(a, (10, -61, 12), 1, seq=7)
    assert shows(collect(a), {(10, -60, 12): cross("kIronBars", "w"), (9, -60, 12): pane("ew")})
    # A wall: beside a block, an end with a post; two walls; three - the middle one runs straight
    # and has no post.
    hold(a, 4)
    click(a, (8, -61, 10), 1, seq=8)
    assert shows(collect(a), {(8, -60, 10): cobble(south="low")})
    click(a, (7, -61, 10), 1, seq=9)
    assert shows(collect(a), {(7, -60, 10): cobble(east="low"), (8, -60, 10): cobble(south="low", west="low")})
    click(a, (6, -61, 10), 1, seq=10)
    assert shows(collect(a), {(6, -60, 10): cobble(east="low"), (7, -60, 10): cobble(up=False, east="low", west="low")})
    # A block on it: its sides are tall. A torch on it: a post. Neither: as before.
    hold(a, 0)
    click(a, (7, -60, 10), 1, seq=11)
    assert shows(collect(a), {(7, -59, 10): stone, (7, -60, 10): cobble(up=False, east="tall", west="tall")})
    dig(a, (7, -59, 10), seq=12)
    assert collect(a) == {(7, -59, 10): AIR, (7, -60, 10): cobble(up=False, east="low", west="low")}
    hold(a, 5)
    click(a, (7, -60, 10), 1, seq=13)
    assert shows(collect(a), {(7, -59, 10): mc.BLOCK["kTorch"], (7, -60, 10): cobble(east="low", west="low")})
    dig(a, (7, -59, 10), seq=14)
    assert collect(a) == {(7, -59, 10): AIR, (7, -60, 10): cobble(up=False, east="low", west="low")}
    # A wall under a wall with a post has one; a pane and a wall hold each other.
    hold(a, 4)
    click(a, (7, -60, 10), 1, seq=15)
    assert shows(collect(a), {(7, -59, 10): cobble(), (7, -60, 10): cobble(east="low", west="low")})
    hold(a, 3)
    click(a, (6, -61, 11), 1, seq=16)
    assert shows(collect(a), {(6, -60, 11): pane("n"), (6, -60, 10): cobble(east="low", south="low")})
    collect(b)
    # The stone goes: everything that held on to it lets go, and nothing breaks.
    dig(a, (8, -60, 11), seq=17)
    gone = {(8, -60, 11): AIR, (9, -60, 11): fence("e"), (8, -60, 12): pane("e"), (8, -60, 10): cobble(west="low")}
    assert collect(a) == gone
    assert collect(b) == gone
    # (Bob saw three blocks break under Alice's hand: the stone and the torch on the wall, and this one.)
    assert [e[:2] for e in b.level_events] == [(DESTROY_BLOCK, pos) for pos in ((7, -59, 10), (7, -59, 10), (8, -60, 11))]
    assert not of_type(b, "kItem")
    # A click on a fence is the fence's first (a lead would be tied to it): with nothing on a
    # lead it passes, and the block in hand is placed against it.
    hold(a, 0)
    click(a, (9, -60, 11), 2, seq=18, cursor=(0.5, 0.5, 0.375))
    assert shows(collect(a), {(9, -60, 10): stone, (9, -60, 11): fence("en"), (8, -60, 10): cobble(east="low", west="low", up=False)})


@test()
def lanterns_chains_and_other_shapes(srv: Server):
    """A lantern hangs or stands by where its player looks and what is there to hold it, and
    breaks when that goes; a chain lies along the face it is put on, an end rod points out of
    it, glazed terracotta faces its player, a mushroom block has no skin towards its own kind."""
    a, b = srv.client(), srv.client()
    join(a, "Alice")
    join(b, "Bob")
    a.expect("player_info_update")
    for cl in (a, b):
        cl.send("player_loaded")
    settle(a, b)
    standing = mc.BLOCK["kLantern"]
    hanging = standing - 2                                   # hanging (true, false) x waterlogged
    for slot, item in enumerate(("kStone", "kLantern", "kIronChain", "kEndRod", "kWhiteGlazedTerracotta", "kBrownMushroomBlock",
                                 "kWaxedCopperGrate", "kBarrier")):
        give(a, 36 + slot, I[item], 16)
    # A stone in the air, two above the ground.
    click(a, (10, -61, 11), 1, seq=1)
    click(a, (10, -60, 11), 1, seq=2)
    dig(a, (10, -60, 11), seq=3)
    collect(a)
    hold(a, 1)
    look(a, 0.0, 0.0)
    click(a, (10, -59, 11), 0, seq=4, cursor=(0.5, 0.0, 0.5))    # against its underside: up comes first, it hangs
    assert shows(collect(a), {(10, -60, 11): hanging})
    click(a, (8, -61, 11), 1, seq=5)                             # on the ground: it stands
    assert shows(collect(a), {(8, -60, 11): standing})
    # Against the stone's side, with nothing under and nothing over the place: it cannot be.
    click(a, (10, -59, 11), 5, seq=6, cursor=(1.0, 0.5, 0.5))
    assert collect(a).get((11, -59, 11), AIR) == AIR
    collect(b)
    # What it hangs from goes: it breaks, and drops.
    dig(a, (10, -59, 11), seq=7)
    assert collect(a) == {(10, -59, 11): AIR, (10, -60, 11): AIR}
    collect(b)
    assert a.level_events == [(DESTROY_BLOCK, (10, -60, 11), hanging, False)], a.level_events
    settle(a, b)
    assert [e.data[D_ITEM][1] for e in items_of(b, "kLantern")] == [plain("kLantern", 1)]
    dig(a, (8, -61, 11), seq=8)                                  # ...and the one that stands, with its ground
    assert collect(a) == {(8, -61, 11): AIR, (8, -60, 11): AIR}

    # A chain: along the axis of the face it is put on (x, y, z; then waterlogged).
    chain = mc.BLOCK["kIronChain"]                               # y, dry
    hold(a, 0)
    click(a, (10, -61, 9), 1, seq=9)
    hold(a, 2)
    click(a, (10, -60, 9), 1, seq=10)
    click(a, (10, -60, 9), 4, seq=11, cursor=(0.0, 0.5, 0.5))
    click(a, (10, -60, 9), 2, seq=12, cursor=(0.5, 0.5, 0.0))
    assert shows(collect(a), {(10, -59, 9): chain, (9, -60, 9): chain - 2, (10, -60, 8): chain + 2})
    # An end rod: out of the face - and on a rod that points that way, back at it.
    rod = mc.BLOCK["kEndRod"]                                    # facing: north, east, south, west, up, down
    hold(a, 3)
    click(a, (6, -61, 11), 1, seq=13)
    assert shows(collect(a), {(6, -60, 11): rod})
    click(a, (6, -60, 11), 1, seq=14)
    click(a, (10, -60, 9), 5, seq=15, cursor=(1.0, 0.5, 0.5))
    assert shows(collect(a), {(6, -59, 11): rod + 1, (11, -60, 9): rod - 3})
    # Glazed terracotta faces whoever places it.
    hold(a, 4)
    look(a, 0.0, 0.0)                                            # looking south: it faces north
    click(a, (6, -61, 9), 1, seq=16)
    look(a, -90.0, 0.0)                                          # looking east: west
    click(a, (6, -61, 8), 1, seq=17)
    glazed = mc.BLOCK["kWhiteGlazedTerracotta"]
    assert shows(collect(a), {(6, -60, 9): glazed, (6, -60, 8): glazed + 2})
    # Mushroom blocks: down, east, north, south, up, west - skin where no other of it is.
    cap = mc.BLOCK["kBrownMushroomBlock"]
    hold(a, 5)
    click(a, (8, -61, 5), 1, seq=18)
    click(a, (9, -61, 5), 1, seq=19)
    assert shows(collect(a), {(8, -60, 5): cap + 16, (9, -60, 5): cap + 1})
    dig(a, (9, -60, 5), seq=20)
    assert collect(a) == {(9, -60, 5): AIR}                      # (the skin does not grow back)
    # What is only ever dry or waterlogged: dry.
    hold(a, 6)
    click(a, (11, -61, 6), 1, seq=21)
    hold(a, 7)
    click(a, (11, -61, 7), 1, seq=22)
    assert shows(collect(a), {(11, -60, 6): mc.BLOCK["kWaxedCopperGrate"], (11, -60, 7): mc.BLOCK["kBarrier"]})


@test(random_tick_speed=4096, simulation_distance=2)
def copper_keeps_its_shape(srv: Server):
    """Copper that has a shape weathers like the block of it - and stays what it was: stairs
    upside down, facing east, are that at every stage."""
    a = srv.client()
    join(a, "Alice", view=2)
    a.send("player_loaded")
    settle(a)
    give(a, 36, I["kStone"], 4)
    give(a, 37, I["kCutCopperStairs"], 4)
    give(a, 38, I["kCutCopperSlab"], 4)
    give(a, 39, I["kWaxedCutCopperStairs"], 4)
    click(a, (8, -61, 6), 1, seq=1)
    hold(a, 1)
    look(a, -90.0, 0.0)
    click(a, (8, -60, 6), 5, seq=2, cursor=upper(5))
    stages = [stairs(k, "east", "top") for k in ("kCutCopperStairs", "kExposedCutCopperStairs", "kWeatheredCutCopperStairs",
                                                  "kOxidizedCutCopperStairs")]
    hold(a, 2)
    click(a, (8, -61, 11), 1, seq=3)
    click(a, (8, -60, 11), 1, seq=4, cursor=(0.5, 0.5, 0.5))     # a double slab of it
    hold(a, 3)
    click(a, (10, -61, 11), 1, seq=5)                            # waxed: it stays as it is
    seen = []

    def weathered(ch: dict) -> bool:
        state = ch.get((9, -60, 6))
        if state is not None and state not in seen[-1:]:
            seen.append(state)
        return state == stages[-1] and ch.get((8, -60, 11)) == slab("kOxidizedCutCopperSlab", "double")

    changes = until(a, weathered, ticks=2400)
    assert seen == [s for s in stages if s in seen] and len(seen) >= 3, (seen, stages)
    assert changes[(10, -60, 11)] == stairs("kWaxedCutCopperStairs", "east"), changes


@test()
def placed_in_water():
    """A block that can hold water holds the source it is placed into, and leaves it behind when it
    goes; flowing water is displaced. (The water is an imported world's, and the game stands still
    while it is built into: water that is touched flows - water_spreads and the scenarios after it.)"""
    world = tempfile.mkdtemp(prefix="world-water-")
    srv = None
    try:
        flowing = {"id": "minecraft:water", "properties": {"level": "3"}}
        anvil_world(world, extra={(x, 4, 8): "minecraft:water" for x in (2, 3, 4, 5, 6, 7)} | {(8, 4, 8): flowing})
        srv = Server(EXE, world=world, start_timeout=30)
        c = srv.client()
        join(c, "Alice", view=2)
        c.send("player_loaded")
        settle(c)
        make_op(srv, c, "Alice")
        assert run_command(c, "tick freeze")["said"] == [tr("commands.tick.status.frozen")]
        water = mc.BLOCK["kWater"]
        for slot, item in enumerate(("kOakSlab", "kOakStairs", "kOakFence", "kCobblestoneWall", "kLantern", "kIronChain", "kBarrier")):
            give(c, 36 + slot, I[item], 16)
        look(c, 0.0, 0.0)
        click(c, (2, -61, 8), 1, seq=1)                          # on the ground under the water: into it
        hold(c, 1)
        click(c, (3, -61, 8), 1, seq=2)
        hold(c, 2)
        click(c, (4, -61, 8), 1, seq=3)
        hold(c, 3)
        click(c, (5, -61, 8), 1, seq=4)
        hold(c, 4)
        click(c, (6, -61, 8), 1, seq=5)
        hold(c, 5)
        click(c, (7, -61, 8), 1, seq=6)
        hold(c, 6)
        click(c, (8, -61, 8), 1, seq=7)                          # water that flows is no source: dry
        wet = {(2, -60, 8): slab("kOakSlab", "bottom", True), (3, -60, 8): stairs("kOakStairs", "south", "bottom", "straight", True),
               (4, -60, 8): cross("kOakFence", "", True), (5, -60, 8): wall("kCobblestoneWall", True, True),
               (6, -60, 8): mc.BLOCK["kLantern"] - 1, (7, -60, 8): mc.BLOCK["kIronChain"] - 1, (8, -60, 8): mc.BLOCK["kBarrier"]}
        assert shows(collect(c), wet)
        # The second half of a slab drives the water out (SlabBlock.getStateForPlacement).
        hold(c, 0)
        click(c, (2, -60, 8), 1, seq=8, cursor=(0.5, 0.5, 0.5))
        assert shows(collect(c), {(2, -60, 8): slab("kOakSlab", "double")})
        # What goes leaves its water where it was (Level.removeBlock: the fluid's block).
        dig(c, (3, -60, 8), seq=9)
        dig(c, (4, -60, 8), seq=10)
        dig(c, (2, -60, 8), seq=11)
        dig(c, (8, -60, 8), seq=12)
        assert collect(c) == {(3, -60, 8): water, (4, -60, 8): water, (2, -60, 8): AIR, (8, -60, 8): AIR}
        assert srv.stop() == 0, srv.read_log()
        os.unlink(srv.log.name)
    finally:
        if srv:
            srv.stop()
        shutil.rmtree(world, ignore_errors=True)


def use(c: Client, seq: int, yaw: float, pitch: float, hand: int = 0) -> None:
    """use_item: the right click on no block - with the way the client looks, which the server takes."""
    c.send("use_item", varint(hand) + varint(seq) + struct.pack(">ff", yaw, pitch))


@test()
def buckets(srv: Server):
    """A bucket is emptied where its holder looks, and filled from what it looks at (BucketItem.use
    over Item.getPlayerPOVHitResult: a look as far as the hand reaches, stopped by a block's
    outline - and, of an empty bucket's, by a source). The fluid goes against the side that was
    met; into the block itself if that holds water; into the place of what gives way to it,
    which breaks. Everyone is creative: a bucket stays what it is, and filling one puts a full
    one into the inventory if none is there. The game stands still, so what is set stays."""
    a, b = srv.client(), srv.client()
    ja = join(a, "Alice")
    join(b, "Bob")
    a.expect("player_info_update")
    for cl in (a, b):
        cl.send("player_loaded")
    settle(a, b)
    make_op(srv, a, "Alice")
    assert run_command(a, "tick freeze")["said"] == [tr("commands.tick.status.frozen")]
    settle(b)
    b.ignore = {"block_update", "section_blocks_update", "light_update", "system_chat"}
    ida = ja["login"]["entity_id"]
    water, lava = mc.BLOCK["kWater"], mc.BLOCK["kLava"]
    feet, DOWN_AT_FEET = (8, -60, 8), (0.0, 90.0)
    BLOCKS, PLAYERS = 4, 7              # SoundSource
    for slot, item in enumerate(("kWaterBucket", "kBucket", "kLavaBucket")):
        give(a, 36 + slot, I[item])
        a.slots[36 + slot] = plain(item)        # (a client knows what it put there)
    settle(a, b)

    def used(slot: int, seq: int, looks: tuple, hand: int = 0, ticks: int = 3) -> tuple:
        """Alice uses what is in that hotbar slot: the blocks that changed, what Bob heard, whether he saw her arm."""
        mark = len(b.entity_log)
        b.sounds.clear()
        hold(a, slot)
        use(a, seq, *looks, hand=hand)
        changed = collect(a, ticks=ticks)
        settle(b)
        swung = [what for name, e, what in b.entity_log[mark:] if name == "swing_animation" and e == ida]
        return changed, [(s[0], s[1], s[2]) for s in b.sounds], len(swung)

    # Water, looking at her feet: it is met by the ground, and goes on top of it - where she stands.
    assert used(0, 1, DOWN_AT_FEET) == ({feet: water}, [(mc.SOUND["kItemBucketEmpty"], BLOCKS, (8.5, -59.5, 8.5))], 1)
    assert a.slots[36] == plain("kWaterBucket")                      # still full
    # ...and once more into the source: nothing changes, and it sounds all the same.
    assert used(0, 2, DOWN_AT_FEET) == ({}, [(mc.SOUND["kItemBucketEmpty"], BLOCKS, (8.5, -59.5, 8.5))], 1)
    # The empty bucket: its look is stopped by the source. The water is gone; a full bucket is in the inventory already.
    assert used(1, 3, DOWN_AT_FEET) == ({feet: AIR}, [(mc.SOUND["kItemBucketFill"], PLAYERS, (8.5, -60.0, 8.5))], 1)
    assert a.slots[36:39] == [plain("kWaterBucket"), plain("kBucket"), plain("kLavaBucket")]
    # ...into the air, or at the ground: nothing to take, no arm.
    assert used(1, 4, (0.0, -90.0)) == ({}, [], 0)
    assert used(1, 5, DOWN_AT_FEET) == ({}, [], 0)
    # Lava. With no bucket of it left in the inventory, filling one puts one there.
    assert used(2, 6, DOWN_AT_FEET) == ({feet: lava}, [(mc.SOUND["kItemBucketEmptyLava"], BLOCKS, (8.5, -59.5, 8.5))], 1)
    a.send("set_creative_mode_slot", struct.pack(">h", 38) + mc.item_stack(0, 0))
    a.slots[38] = None
    assert used(1, 7, DOWN_AT_FEET) == ({feet: AIR}, [(mc.SOUND["kItemBucketFillLava"], PLAYERS, (8.5, -60.0, 8.5))], 1)
    assert a.slots[38] == plain("kLavaBucket") and a.slots[37] == plain("kBucket"), a.slots[36:40]

    # A slab east of her: water goes into it; an empty bucket takes it out again (its look meets the
    # water that stands over the slab's top); one who sneaks pours it on top instead.
    at_slab = (-90.0, 48.0)             # from her eyes down onto the slab's top
    assert run_command(a, "setblock 9 -60 8 oak_slab")["said"] == [tr("commands.setblock.success", 9, -60, 8)]
    assert used(0, 8, at_slab)[0] == {(9, -60, 8): slab("kOakSlab", "bottom", True)}
    assert used(1, 9, at_slab) == ({(9, -60, 8): slab("kOakSlab")}, [(mc.SOUND["kItemBucketFill"], PLAYERS, (8.5, -60.0, 8.5))], 1)
    a.send("player_input", bytes([0x20]))
    assert used(0, 10, at_slab)[0] == {(9, -59, 8): water}
    a.send("player_input", bytes([0x00]))
    settle(a, b)                        # (she stands up with her next tick: the look is from where her eyes are)
    assert used(1, 11, at_slab)[0] == {(9, -59, 8): AIR}

    # A torch south of her. A look that meets it puts the water against the side it met - back at her feet -
    # and leaves it be; a look past it at the ground under it puts the water in its place: it breaks, and drops.
    run_command(a, "setblock 8 -60 9 torch")
    assert used(0, 12, (0.0, 52.9))[0] == {feet: water}
    assert used(1, 13, DOWN_AT_FEET)[0] == {feet: AIR}
    assert not of_type(a, "kItem")
    assert used(0, 14, (-14.5, 49.3))[0] == {(8, -60, 9): water}
    assert [e.data[D_ITEM][1][0] for e in of_type(a, "kItem").values()] == [I["kTorch"]]


@test()
def imported_water_flows_on():
    """A vanilla world saved in the middle of a flow: the chunk's `fluid_ticks` are read with it
    (SavedTick: the fluid, where, the ticks until it is due), so the source that was about to
    spread spreads - that many ticks after its chunk ticks again - and one that nothing was
    scheduled for stands as it stood. (A tick of a fluid this server does not know, or outside
    the world, and the list is kept as the tag it is: nothing of it runs, nothing is lost.)"""
    world = tempfile.mkdtemp(prefix="world-ticks-")
    srv = None
    try:
        due, still = (12, -60, 12), (4, -60, 5)
        anvil_world(world, extra={(12, 4, 12): "minecraft:water", (4, 4, 5): "minecraft:water"},
                    fluid_ticks=[{"i": "minecraft:water", "x": 12, "y": -60, "z": 12, "t": 150, "p": 0},
                                 {"i": "minecraft:water", "x": 40, "y": -60, "z": 12, "t": 1, "p": 0}])      # (another chunk's: passed over)
        srv = Server(EXE, world=world, start_timeout=30)
        c = srv.client()
        join(c, "Alice", view=2)
        c.send("player_loaded")
        water = mc.BLOCK["kWater"]
        got = run_command(c, "list", ticks=175)["blocks"]       # 150 ticks, and five steps of water after them
        assert {pos: got.get(pos) for pos in ring(due, 1)} == {pos: water + 1 for pos in ring(due, 1)}, got
        assert {pos: got.get(pos) for pos in ring(due, 4)} == {pos: water + 4 for pos in ring(due, 4)}, got
        assert not any(pos in got for pos in ring(still, 1)), got
        assert srv.stop() == 0, srv.read_log()
        os.unlink(srv.log.name)
    finally:
        if srv:
            srv.stop()
        shutil.rmtree(world, ignore_errors=True)


# ----------------------------------------------------------------------------- fluids

def step_game(c: Client, ticks: int) -> dict:
    """`/tick step` of a game that stands still: so many game ticks and no more - and the block
    changes they brought. (What a fluid does is counted in game ticks: a scenario that asks when
    freezes the game and steps it.)"""
    got = run_command(c, f"tick step {ticks}", ticks=ticks + 4)
    assert got["said"] == [tr("commands.tick.step.success", ticks)], got["said"]
    return got["blocks"]


def ring(centre: tuple, d: int) -> list:
    """The cells `d` steps from `centre` on its level: where a fluid is after d of its steps."""
    x, y, z = centre
    return sorted({(x + dx, y, z + dz) for dx in range(-d, d + 1) for dz in (d - abs(dx), abs(dx) - d)})


def wet_start(srv: Server) -> Client:
    """An operator in a game that stands still: what is set is ticked when the scenario says."""
    a = srv.client()
    join(a, "Alice")
    a.send("player_loaded")
    settle(a)
    make_op(srv, a, "Alice")
    assert run_command(a, "tick freeze")["said"] == [tr("commands.tick.status.frozen")]
    return a


@test()
def water_spreads(srv: Server):
    """Water flows (FlowingFluid.tick): from a source a block every five game ticks, a level less
    each block, seven blocks far on flat ground - a diamond of 113 cells. With the source gone it
    drains, every cell finding in its turn that nothing feeds it."""
    a = wet_start(srv)
    water, source = mc.BLOCK["kWater"], (8, -60, 12)
    got = run_command(a, "setblock 8 -60 12 water")
    assert got["said"] == [tr("commands.setblock.success", 8, -60, 12)] and got["blocks"] == {source: water}, got
    assert step_game(a, 4) == {}                    # WaterFluid.getTickDelay: five ticks a step
    assert step_game(a, 1) == {pos: water + 1 for pos in ring(source, 1)}
    for d in range(2, 8):
        assert step_game(a, 5) == {pos: water + d for pos in ring(source, d)}, d
    assert step_game(a, 20) == {}                   # one level left gives none away: it ends there
    # /fill sets water as /setblock does, and water set beside water is water: nothing to flow.
    assert run_command(a, "fill 8 -60 12 8 -60 12 water")["said"] == [failure(tr("commands.fill.failed"))]
    # The source goes: what it fed sinks and dries up, from the middle outwards - the middle
    # last, into which the water beside it flows back until that is gone too.
    assert run_command(a, "setblock 8 -60 12 air")["blocks"] == {source: AIR}
    wet = {pos for d in range(0, 8) for pos in ring(source, d)}
    drained = step_game(a, 150)
    assert drained == {pos: AIR for pos in wet}, {p: s for p, s in drained.items() if s != AIR}
    assert step_game(a, 20) == {}


@test()
def water_finds_the_hole(srv: Server):
    """Water goes to the nearest way down within its reach, and nowhere else (FlowingFluid.getSpread);
    over the hole it falls - a falling column, which is not a source - and does not spread from there."""
    a = wet_start(srv)
    water = mc.BLOCK["kWater"]
    run_command(a, "setblock 11 -61 12 air")        # a hole in the ground, three blocks east
    run_command(a, "setblock 8 -60 12 water")
    assert step_game(a, 5) == {(9, -60, 12): water + 1}
    assert step_game(a, 5) == {(10, -60, 12): water + 2}
    assert step_game(a, 5) == {(11, -60, 12): water + 3}
    assert step_game(a, 5) == {(11, -61, 12): water + 8}    # down: level 8 is "falling"
    assert step_game(a, 30) == {}
    # The hole is filled in under it: now the water spreads as on flat ground, from where it stands.
    assert run_command(a, "setblock 11 -61 12 stone")["blocks"] == {(11, -61, 12): mc.BLOCK["kStone"]}
    got = step_game(a, 5)
    assert got == {(12, -60, 12): water + 4, (11, -60, 11): water + 4, (11, -60, 13): water + 4}, got


def water_between_two_sources(srv: Server, converts: bool) -> None:
    a = wet_start(srv)
    water = mc.BLOCK["kWater"]
    run_command(a, "setblock 8 -60 12 water")
    run_command(a, "setblock 10 -60 12 water")
    between = water if converts else water + 1
    assert step_game(a, 5) == {(9, -60, 12): between} | {
        pos: water + 1 for pos in ((8, -60, 11), (8, -60, 13), (7, -60, 12), (10, -60, 11), (10, -60, 13), (11, -60, 12))}
    # What lies beside the cell between them is fed by a source - or is two blocks from one.
    got = step_game(a, 5)
    assert (got[(9, -60, 11)], got[(9, -60, 13)]) == ((water + 1,) * 2 if converts else (water + 2,) * 2), got


@test()
def water_makes_a_source(srv: Server):
    """Between two sources, over solid ground, water becomes a source itself (FlowingFluid.getNewLiquid)."""
    water_between_two_sources(srv, True)


@test(water_source_conversion="false")
def water_source_conversion_off(srv: Server):
    """water-source-conversion=false (the game rule of that name): it flows, and is no source."""
    water_between_two_sources(srv, False)


@test()
def water_washes_away(srv: Server):
    """What gives way to a fluid is washed away by it: it drops what it would to an empty hand, with
    no sound and no particles of a block breaking (WaterFluid.beforeDestroyingBlock) - a torch, a
    flower, petals. A slab holds its ground, and stays dry."""
    a = wet_start(srv)
    water = mc.BLOCK["kWater"]
    for line in ("setblock 9 -60 12 torch", "setblock 7 -60 12 dandelion", "setblock 8 -60 11 pink_petals[flower_amount=3]",
                 "setblock 8 -60 13 oak_slab"):
        run_command(a, line)
    run_command(a, "setblock 8 -60 12 water")
    assert step_game(a, 5) == {(9, -60, 12): water + 1, (7, -60, 12): water + 1, (8, -60, 11): water + 1}
    a.ticks(3)
    dropped = sorted((e.data[D_ITEM][1][0], e.data[D_ITEM][1][1]) for e in of_type(a, "kItem").values())
    assert dropped == sorted([(I["kTorch"], 1), (I["kDandelion"], 1), (I["kPinkPetals"], 3)]), dropped
    assert not a.level_events
    # On: round the slab, not into it.
    got = step_game(a, 5)
    assert (8, -60, 13) not in got and got[(9, -60, 13)] == water + 2 and got[(7, -60, 13)] == water + 2, got


@test()
def waterlogged_water_flows(srv: Server):
    """The water in a block that holds it (SimpleWaterloggedBlock) is a source where the block
    stands: it is ticked when the block's shape is asked, flows out where the block's shape lets
    it - a bottom slab to its sides, a top slab downwards too - and what a broken block leaves is
    water that flows. Where a source would form, a dry block that can hold one is waterlogged."""
    a = wet_start(srv)
    water = mc.BLOCK["kWater"]
    # A wet fence, broken at once: its water is left, and flows.
    fence = (9, -60, 9)
    assert run_command(a, "setblock 9 -60 9 oak_fence[waterlogged=true]")["blocks"] == {fence: cross("kOakFence", "", True)}
    dig(a, fence, seq=1)
    assert collect(a) == {fence: water}
    assert step_game(a, 5) == {pos: water + 1 for pos in ring(fence, 1)}
    # A wet bottom slab, set by a command: the water leaves it sideways, and the slab stays wet.
    at = (-10, -60, 12)
    assert run_command(a, "setblock -10 -60 12 oak_slab[waterlogged=true]")["blocks"] == {at: slab("kOakSlab", "bottom", True)}
    got = step_game(a, 5)
    assert at not in got and all(got[pos] == water + 1 for pos in ring(at, 1)), got
    # A wet top slab a block up: its water falls out of it, and spreads where it lands.
    run_command(a, "setblock 20 -59 0 oak_slab[type=top,waterlogged=true]")
    got = step_game(a, 5)
    assert got[(20, -60, 0)] == water + 8 and not any(pos in got for pos in ring((20, -59, 0), 1)), got
    got = step_game(a, 5)
    assert all(got[pos] == water + 1 for pos in ring((20, -60, 0), 1)), got
    # Dry stairs between two sources: the source that would form there is in the stairs.
    for line in ("setblock 21 -60 20 oak_stairs", "setblock 20 -60 20 water", "setblock 22 -60 20 water"):
        run_command(a, line)
    got = step_game(a, 5)
    assert got[(21, -60, 20)] == stairs("kOakStairs", "north", "bottom", "straight", True), got
    # ...and what /fill sets wet is wet.
    got = run_command(a, "fill -20 -60 -20 -19 -60 -20 oak_slab[waterlogged=true]")
    assert got["said"] == [tr("commands.fill.success", 2)], got
    assert got["blocks"] == {(-20, -60, -20): slab("kOakSlab", "bottom", True), (-19, -60, -20): slab("kOakSlab", "bottom", True)}


@test()
def sponge_absorbs(srv: Server):
    """A sponge takes up the water around it when it is placed and when water comes beside it
    (SpongeBlock.tryAbsorbWater), and is a wet sponge then - with the sound of it. With no
    water in reach it stays what it is."""
    a = wet_start(srv)
    water, sponge, wet = mc.BLOCK["kWater"], mc.BLOCK["kSponge"], mc.BLOCK["kWetSponge"]
    absorb = mc.SOUND["kBlockSpongeAbsorb"]
    assert run_command(a, "setblock 8 -60 12 sponge")["blocks"] == {(8, -60, 12): sponge}
    assert not a.sounds
    # Water set beside it: gone the moment it comes.
    got = run_command(a, "setblock 9 -60 12 water")
    assert got["said"] == [tr("commands.setblock.success", 9, -60, 12)], got
    assert got["blocks"] == {(8, -60, 12): wet, (9, -60, 12): AIR}, got
    assert a.sounds == [(absorb, 4, (8.5, -59.5, 12.5), 1.0, 1.0)], a.sounds
    # A wet sponge takes nothing more.
    assert run_command(a, "setblock 8 -60 13 water")["blocks"] == {(8, -60, 13): water}
    run_command(a, "setblock 8 -60 13 air")
    # A sponge set over a trench of three sources, with a wet slab at its end: all of it, and the slab is dry.
    for line in ("fill 0 -61 14 2 -61 14 water", "setblock 3 -61 14 oak_slab[waterlogged=true]"):
        run_command(a, line)
    got = run_command(a, "setblock 1 -60 14 sponge")
    assert got["blocks"] == {(1, -60, 14): wet, (0, -61, 14): AIR, (1, -61, 14): AIR, (2, -61, 14): AIR,
                             (3, -61, 14): slab("kOakSlab", "bottom", False)}, got
    assert [snd[:3] for snd in a.sounds[1:]] == [(absorb, 4, (1.5, -59.5, 14.5))], a.sounds
    assert step_game(a, 12) == {}                   # nothing is left to flow


LAVA_FIZZ = 1501    # LevelEvent.LAVA_FIZZ


@test()
def lava_flows(srv: Server):
    """Lava is water at a pace of its own (LavaFluid): a block every thirty game ticks, two levels
    less each block - three blocks from its source, and no further."""
    a = wet_start(srv)
    lava, source = mc.BLOCK["kLava"], (8, -60, 12)
    got = run_command(a, "setblock 8 -60 12 lava")
    assert got["said"] == [tr("commands.setblock.success", 8, -60, 12)] and got["blocks"] == {source: lava}, got
    assert step_game(a, 29) == {}
    assert step_game(a, 1) == {pos: lava + 2 for pos in ring(source, 1)}
    for d in (2, 3):
        assert step_game(a, 30) == {pos: lava + 2 * d for pos in ring(source, d)}, d
    assert step_game(a, 45) == {}                   # two levels left give none away
    assert not a.level_events


@test()
def lava_meets_water(srv: Server):
    """Lava that has water beside it or on it is stone of a kind where it stands - obsidian if it
    was a source, cobblestone if it flowed (LiquidBlock.shouldSpreadLiquid: the lava's answer, in
    the tick the water comes); lava that comes down on water makes stone of the water
    (LavaFluid.spreadTo); over soul soil and beside blue ice it is basalt. Each with a hiss."""
    a = wet_start(srv)
    water, lava = mc.BLOCK["kWater"], mc.BLOCK["kLava"]
    # What takes thirty ticks first: lava that is to flow, and lava over a pit of water.
    run_command(a, "setblock -10 -60 12 lava")
    for line in ("setblock 8 -61 -10 water", "setblock 8 -60 -10 lava"):
        run_command(a, line)
    got = step_game(a, 30)
    assert got == {pos: lava + 2 for pos in ring((-10, -60, 12), 1)} | {(8, -61, -10): mc.BLOCK["kStone"]}, got
    assert a.level_events == [(LAVA_FIZZ, (8, -61, -10), 0, False)], a.level_events
    # Water beside lava that flowed: cobblestone, at once.
    got = run_command(a, "setblock -8 -60 12 water")
    assert got["blocks"] == {(-8, -60, 12): water, (-9, -60, 12): mc.BLOCK["kCobblestone"]}, got
    # ...beside a source: obsidian. And on one.
    run_command(a, "setblock 8 -60 12 lava")
    got = run_command(a, "setblock 9 -60 12 water")
    assert got["blocks"] == {(9, -60, 12): water, (8, -60, 12): mc.BLOCK["kObsidian"]}, got
    run_command(a, "setblock 20 -60 12 lava")
    got = run_command(a, "setblock 20 -59 12 water")
    assert got["blocks"] == {(20, -59, 12): water, (20, -60, 12): mc.BLOCK["kObsidian"]}, got
    # Water under lava is nothing to the lava where it stands (it looks up and around).
    run_command(a, "setblock 20 -61 0 water")
    assert run_command(a, "setblock 20 -60 0 lava")["blocks"] == {(20, -60, 0): lava}
    # Over soul soil, beside blue ice: basalt.
    for line in ("setblock -10 -61 -10 soul_soil", "setblock -9 -60 -10 blue_ice"):
        run_command(a, line)
    got = run_command(a, "setblock -10 -60 -10 lava")
    assert got["said"] == [tr("commands.setblock.success", -10, -60, -10)] and got["blocks"] == {(-10, -60, -10): mc.BLOCK["kBasalt"]}, got
    assert a.level_events[1:] == [(LAVA_FIZZ, pos, 0, False) for pos in ((-9, -60, 12), (8, -60, 12), (20, -60, 12), (-10, -60, -10))]


def tag_numbers(shown_tag: str) -> list:
    """The numbers of a tag as /data get shows it - "[-8.5d, 1.0E-4d]", "300s" - as Python has them."""
    return [float(n) if any(ch in n for ch in ".E") else int(n) for n in re.findall(r"(-?[0-9][0-9.E-]*)[dfsbL]?", shown_tag)]


@test()
def afloat(srv: Server):
    """What is in water and lava moves as on a vanilla server - to the bit: three things out of
    the recording the unit test replays (tools/vanilla/physics_probe.py: fluid_item_pool_bottom,
    fluid_stand_channel, fluid_item_lava_pool), built here where they were recorded, ticked by
    the game itself and read back with /data get. An item floats up from the bottom of a pool; a
    current pushes an armor stand along a channel; an item that falls into lava is lit, has one
    point of its five left and is gone a tick later. (Three that never lie on the ground: an item
    that does moves every fourth tick, and which is its entity id's to say.) Then a pig: in lava
    it burns, for everyone to see, and lives (nothing takes damage here); in water it is put out."""
    a = wet_start(srv)
    got = run_command(a, "tp -996.5 -45 1004.5", ticks=40)      # in the air between the fixtures: within hearing of each
    assert len(got["teleports"]) == 1, got
    item = '{Item:{id:"minecraft:stone",count:1},PickupDelay:32767s}'
    ITEM_POS, ITEM_MOTION = [-1000.7, -45.36049717167888, 992.6], [0.0, 0.01720995705604936, 0.0]     # after sixty ticks
    lines = [
        # a pool of water, three by three and two deep, in a wall of stone
        "fill -1004 -47 989 -998 -47 995 stone", "fill -1003 -46 990 -999 -45 994 stone", "fill -1002 -46 991 -1000 -45 993 water",
        # a channel: a source, and the water as it stands once it has run to the wall, a level less each block
        "fill -1004 -33 1005 -998 -33 1011 stone", "fill -1004 -32 1007 -998 -32 1009 stone",
        *[f"setblock {-1003 + i} -32 1008 water[level={i}]" for i in range(5)],
        # a pit of lava
        "fill -996 -47 1013 -990 -47 1019 stone", "fill -995 -46 1014 -991 -45 1018 stone", "fill -994 -46 1015 -992 -45 1017 lava",
        f"summon item -1000.7 -46.0 992.6 {item}", "summon armor_stand -1002.5 -32.0 1008.5 {OnGround:1b}",
    ]
    for line in lines:
        said = run_command(a, line)["said"]
        assert len(said) == 1 and said[0].get("translate", "").endswith(".success"), (line, said)

    def data(selector: str, path: str) -> list:
        said = run_command(a, f"data get entity {selector} {path}")["said"]
        assert len(said) == 1 and "with" in said[0], (selector, path, said)
        return tag_numbers(shown(said[0]["with"][-1]))

    # Sixty ticks: nothing of the fixtures flows (they are what a flow leaves), and the two are
    # where the vanilla server had them - every bit of every double.
    assert step_game(a, 60) == {}
    assert data("@e[type=item,limit=1]", "Pos") == ITEM_POS
    assert data("@e[type=item,limit=1]", "Motion") == ITEM_MOTION
    assert data("@e[type=armor_stand,limit=1]", "Pos") == [-998.5799993581122, -32.0, 1008.5]
    assert data("@e[type=armor_stand,limit=1]", "Motion") == [0.055999918352491976, -0.005, 0.0]
    seen = next(iter(of_type(a, "kItem").values()))
    assert abs(seen.pos[1] - ITEM_POS[1]) < 1e-3 and abs(seen.pos[0] - ITEM_POS[0]) < 1e-3, seen.pos    # ...and Alice was told as much

    # Lava: the item falls for five ticks, is in it in the sixth - lit, four of its five points gone - and burns up in the seventh.
    run_command(a, "kill @e[type=item]")
    run_command(a, f"summon item -992.5 -43.5 1016.5 {item}")
    step_game(a, 5)
    assert data("@e[type=item,limit=1]", "Pos") == [-992.5, -44.08423810120649, 1016.5]
    assert (data("@e[type=item,limit=1]", "Fire"), data("@e[type=item,limit=1]", "Health")) == ([0], [5])
    step_game(a, 1)
    assert data("@e[type=item,limit=1]", "Pos") == [-992.5, -44.312553350325814, 1016.5]
    assert (data("@e[type=item,limit=1]", "Fire"), data("@e[type=item,limit=1]", "Health")) == ([300], [1])
    assert not a.sounds                     # (lava that hurts an item is heard as it burns up, and every tenth tick of its life)
    step_game(a, 1)
    assert not of_type(a, "kItem")
    AMBIENT, NEUTRAL = 8, 6                 # SoundSource: an item's sounds, a pig's
    assert [(s[0], s[1]) for s in a.sounds] == [(mc.SOUND["kEntityGenericBurn"], AMBIENT)], a.sounds
    assert abs(a.sounds[0][3] - 0.4) < 1e-6 and 2.0 <= a.sounds[0][4] <= 2.4 and abs(a.sounds[0][2][1] - -44.5) < 0.2, a.sounds
    a.sounds.clear()
    assert run_command(a, "data get entity @e[type=item,limit=1]")["said"] == [failure(tr("argument.entity.notfound.entity"))]
    # ...but not what is of netherite (ItemStack.canBeHurtBy: its damage_resistant): it floats.
    run_command(a, 'summon item -992.5 -43.5 1016.5 {Item:{id:"minecraft:netherite_ingot",count:1},PickupDelay:32767s}')
    step_game(a, 40)
    assert (data("@e[type=item,limit=1]", "Fire"), data("@e[type=item,limit=1]", "Health")) == ([0], [5])
    assert -46.0 <= data("@e[type=item,limit=1]", "Pos")[1] < -44.1

    # A pig in the lava: it burns - the flag everyone is shown - and goes on burning; in the water it is put out.
    run_command(a, "summon pig -992.5 -45.0 1016.5")
    step_game(a, 3)
    pig = next(iter(of_type(a, "kPig").values()))
    assert pig.data[D_FLAGS] == (BYTE, 0x01) and data("@e[type=pig,limit=1]", "Fire") == [300], pig.data
    step_game(a, 40)
    assert data("@e[type=pig,limit=1]", "Fire") == [300] and of_type(a, "kPig")
    assert not a.sounds
    run_command(a, "tp @e[type=pig] -1000.5 -44.5 992.5")
    step_game(a, 2)
    assert data("@e[type=pig,limit=1]", "Fire") == [0] and pig.data[D_FLAGS] == (BYTE, 0), pig.data
    # ...with a splash as it finds itself in it, and the hiss of its fire.
    assert [(s[0], s[1]) for s in a.sounds] == [(mc.SOUND["kEntityGenericSplash"], NEUTRAL),
                                                (mc.SOUND["kEntityGenericExtinguishFire"], NEUTRAL)], a.sounds
    assert abs(a.sounds[1][3] - 0.7) < 1e-6 and 1.2 <= a.sounds[1][4] <= 2.0, a.sounds


@test()
def a_player_swims(srv: Server):
    """A player that sprints with its eyes under water swims: the flag and the pose others are
    shown (Entity.updateSwimming, Player.updatePlayerPose - the server's to say, from where the
    player is). It swims on for as long as it sprints in water, and not at all while it flies."""
    a, b = srv.client(), srv.client()
    join(a, "Alice")
    jb = join(b, "Bob")
    a.expect("player_info_update")
    for cl in (a, b):
        cl.send("player_loaded")
    settle(a, b)
    make_op(srv, a, "Alice")
    idb = jb["login"]["entity_id"]
    eb = a.entities[idb]
    x, y, z = (int(math.floor(v)) for v in jb["teleport"]["pos"])
    for cl in (a, b):       # (the tank's blocks are not what this is about)
        cl.ignore = {"block_update", "section_blocks_update", "light_update"}
    SPRINTING, SWIMMING = 0x08, 0x10

    def sprint(on: bool) -> None:
        b.send("player_command", varint(idb) + varint(1 if on else 2) + varint(0))
        settle(b, a, ticks=3)

    # On dry land a sprint is a sprint.
    sprint(True)
    assert eb.data[D_FLAGS] == (BYTE, SPRINTING) and eb.data.get(D_POSE, (POSE_T, mc.POSE["kStanding"]))[1] == mc.POSE["kStanding"]
    sprint(False)
    # A tank of glass around Bob, full of water: his eyes are under it.
    run_command(a, f"fill {x - 2} {y - 1} {z - 2} {x + 2} {y + 3} {z + 2} glass")
    run_command(a, f"fill {x - 1} {y} {z - 1} {x + 1} {y + 2} {z + 1} water")
    settle(b, a, ticks=3)
    assert eb.data[D_FLAGS] == (BYTE, 0)                 # in water, and not swimming: nobody sprints
    sprint(True)
    assert eb.data[D_FLAGS] == (BYTE, SPRINTING | SWIMMING) and eb.data[D_POSE] == (POSE_T, mc.POSE["kSwimming"]), eb.data
    assert b.own_data[D_POSE] == (POSE_T, mc.POSE["kSwimming"])                                 # Bob is told too
    sprint(False)
    assert eb.data[D_FLAGS] == (BYTE, 0) and eb.data[D_POSE] == (POSE_T, mc.POSE["kStanding"]), eb.data
    # Only his feet in water: he sprints, and swims no more than on land.
    run_command(a, f"fill {x - 1} {y + 1} {z - 1} {x + 1} {y + 2} {z + 1} air")
    settle(b, a, ticks=3)
    sprint(True)
    assert eb.data[D_FLAGS] == (BYTE, SPRINTING) and eb.data[D_POSE] == (POSE_T, mc.POSE["kStanding"]), eb.data
    # Under water again while he sprints: he swims - and stops when the water is gone from around him.
    run_command(a, f"fill {x - 1} {y + 1} {z - 1} {x + 1} {y + 2} {z + 1} water")
    settle(b, a, ticks=4)
    assert eb.data[D_FLAGS] == (BYTE, SPRINTING | SWIMMING) and eb.data[D_POSE] == (POSE_T, mc.POSE["kSwimming"]), eb.data
    run_command(a, f"fill {x - 1} {y} {z - 1} {x + 1} {y + 2} {z + 1} air")
    settle(b, a, ticks=4)
    assert eb.data[D_FLAGS] == (BYTE, SPRINTING) and eb.data[D_POSE] == (POSE_T, mc.POSE["kStanding"]), eb.data
    # One who flies does not swim (Player.updateSwimming).
    sprint(False)
    run_command(a, f"fill {x - 1} {y} {z - 1} {x + 1} {y + 2} {z + 1} water")
    b.send("player_abilities", b"\x02")
    sprint(True)
    assert eb.data[D_FLAGS] == (BYTE, SPRINTING) and eb.data[D_POSE] == (POSE_T, mc.POSE["kStanding"]), eb.data


@test()
def water_flows_on_after_a_restart():
    """A fluid's tick waits with its chunk as a block's does, a restart long if need be: water set
    four chunks off - outside what a player simulates - stands until somebody comes nearer, and
    then flows as if it had just been set."""
    world = tempfile.mkdtemp(prefix="world-water-ticks-")
    srv = None
    water, far = mc.BLOCK["kWater"], (72, -60, 8)       # in chunk 4; the players stand in chunk 0 and simulate two around it
    try:
        srv = Server(EXE, world=world, simulation_distance=2)
        a = srv.client()
        join(a, "Alice", view=6)
        a.send("player_loaded")
        settle(a)
        make_op(srv, a, "Alice")
        got = run_command(a, "setblock 72 -60 8 water", ticks=20)
        assert got["said"] == [tr("commands.setblock.success", 72, -60, 8)] and got["blocks"] == {far: water}, got
        assert srv.stop() == 0, srv.read_log()
        os.unlink(srv.log.name)

        srv = Server(EXE, world=world, simulation_distance=2)
        c = srv.client()
        join(c, "Carol", view=6)
        c.send("player_loaded")
        settle(c, ticks=20)                     # (a block that changed would have been said)
        make_op(srv, c, "Carol")
        got = run_command(c, "tp 40 -60 12", ticks=80)      # into chunk 2: the tick the water was saved with is due
        assert got["blocks"] == {pos: water + d for d in range(1, 8) for pos in ring(far, d)}, got["blocks"]
        assert srv.stop() == 0, srv.read_log()
        os.unlink(srv.log.name)
    finally:
        if srv:
            srv.stop()
        shutil.rmtree(world, ignore_errors=True)


@test()
def falling_block_survives_a_restart():
    """A block that is falling when the server stops is falling when it starts again, and lands."""
    world = tempfile.mkdtemp(prefix="world-falling-")
    srv = None
    try:
        srv = Server(EXE, world=world)
        a = srv.client()
        join(a, "Alice", view=3)
        a.send("player_loaded")
        settle(a)
        make_op(srv, a, "Alice")
        # High up: the server ticks on while it stops and while Carol joins (some twenty ticks of it
        # at a hundred a second), and sand falls two blocks a tick.
        out = run_command(a, "setblock 8 300 12 minecraft:sand")
        assert out["said"] == [tr("commands.setblock.success", 8, 300, 12)], out["said"]
        for _ in range(20):
            settle(a, ticks=2)
            if of_type(a, "kFallingBlock"):
                break
        (eid, e), = of_type(a, "kFallingBlock").items()
        assert e.object_data == mc.BLOCK["kSand"] and e.spawned[0] == (8.5, 300.0, 12.5)
        settle(a, ticks=8)
        assert srv.stop() == 0, srv.read_log()
        os.unlink(srv.log.name)

        srv = Server(EXE, world=world)
        c = srv.client()
        join(c, "Carol", view=3)
        c.send("player_loaded")
        settle(c, ticks=6)
        (eid, e), = of_type(c, "kFallingBlock").items()
        assert e.object_data == mc.BLOCK["kSand"] and 20.0 < e.pos[1] < 300.0, e.pos
        assert mc.FALLING_DATA["kStartPos"] not in e.data      # where it began is not kept (vanilla does not either)
        changes = until(c, lambda ch: ch.get((8, -60, 12)) == mc.BLOCK["kSand"], ticks=600)
        assert changes == {(8, -60, 12): mc.BLOCK["kSand"]}, changes
        collect(c)
        assert not of_type(c, "kFallingBlock") and c.transient_blocks == [((8, -60, 12), mc.BLOCK["kSand"])]
        assert srv.stop() == 0, srv.read_log()
        os.unlink(srv.log.name)
    finally:
        if srv:
            srv.stop()
        shutil.rmtree(world, ignore_errors=True)


@test()
def a_tick_waits_for_its_chunk():
    """Scheduled ticks run where blocks tick - within the simulation distance of a player and one
    chunk beyond - and wait elsewhere, a restart long if need be: sand set into the air four chunks
    off hangs there until somebody comes nearer; three chunks off its tick runs at once."""
    world = tempfile.mkdtemp(prefix="world-ticks-")
    srv = None
    sand = mc.BLOCK["kSand"]
    near, far = (56, -50, 8), (72, -50, 8)      # in chunks 3 and 4; the players stand in chunk 0 and simulate two around it
    try:
        srv = Server(EXE, world=world, simulation_distance=2)
        a = srv.client()
        join(a, "Alice", view=6)
        a.send("player_loaded")
        settle(a)
        make_op(srv, a, "Alice")
        got = run_command(a, "setblock 72 -50 8 minecraft:sand", ticks=12)
        assert got["said"] == [tr("commands.setblock.success", 72, -50, 8)] and got["blocks"] == {far: sand}, got
        assert not of_type(a, "kFallingBlock")
        # (What becomes of the entity out there is another matter: vanilla ticks entities a chunk
        # less far than blocks, this server wherever a chunk ticks - here it has landed by the end.)
        got = run_command(a, "setblock 56 -50 8 minecraft:sand", ticks=60)
        assert got["blocks"][near] == AIR, got
        assert srv.stop() == 0, srv.read_log()
        os.unlink(srv.log.name)

        srv = Server(EXE, world=world, simulation_distance=2)
        c = srv.client()
        join(c, "Carol", view=6)
        c.send("player_loaded")
        settle(c, ticks=12)                     # (a block that changed would have been said)
        assert not of_type(c, "kFallingBlock")
        make_op(srv, c, "Carol")
        got = run_command(c, "tp 40 -60 12", ticks=60)      # into chunk 2: the tick the sand was saved with is due
        assert got["blocks"] == {far: AIR, (72, -60, 8): sand}, got
        assert not of_type(c, "kFallingBlock") and c.transient_blocks == [((72, -60, 8), sand)]
        assert srv.stop() == 0, srv.read_log()
        os.unlink(srv.log.name)
    finally:
        if srv:
            srv.stop()
        shutil.rmtree(world, ignore_errors=True)


# ----------------------------------------------------------------------------- commands

def run_command(c: Client, line: str, ticks: int = 3) -> dict:
    """Type a command and let `ticks` server ticks pass. What came of it: {"said": the system
    messages (text components as values), "blocks": {pos: state}, "teleports": [...]}; a
    teleport is accepted, the chunks of a new view are taken. Entities, sounds and menus go
    into the client's model as always."""
    out = {"said": [], "blocks": {}, "teleports": []}
    mark = len(c.chat)
    c.send("chat_command", mc.mc_str(line))
    for i in range(ticks):
        token = 0xC0DE00 + i
        c.send("ping_request", struct.pack(">q", token))
        while True:
            name, r = c.recv()
            if name == "pong_response":
                assert r.i64() == token
                break
            if name == "player_position":
                tp = {"id": r.varint(), "pos": (r.f64(), r.f64(), r.f64()), "delta": (r.f64(), r.f64(), r.f64()),
                      "rot": (r.f32(), r.f32()), "relative": r.i32()}
                r.done()
                out["teleports"].append(tp)
                c.send("accept_teleportation", varint(tp["id"]) + struct.pack(">dddff", *tp["pos"], *tp["rot"]))
            elif name == "block_update":
                pos, state = mc.parse_block_update(r)
                out["blocks"][pos] = state
            elif name == "section_blocks_update":
                out["blocks"].update(mc.parse_section_blocks_update(r))
            elif name == "keep_alive":
                c.send("keep_alive", r.take(8))
            else:
                assert name in ("block_changed_ack", "light_update", "player_info_update", "player_info_remove", "set_chunk_cache_center",
                                "chunk_batch_start", "level_chunk_with_light", "chunk_batch_finished", "forget_level_chunk"), \
                    f"unexpected {name} after /{line}"
    assert all(m[0] != "system" or m[2] is False for m in c.chat[mark:])     # nothing on the action bar
    out["said"] = c.said(mark)
    c.mark_chat_read()
    return out


def tr(key: str, *args) -> dict:
    """Component.translatable(key, args) as it arrives."""
    return {"translate": key, "with": list(args)} if args else {"translate": key}


def failure(message) -> dict:
    """CommandSourceStack.sendFailure: the message inside an empty red text."""
    return {"text": "", "extra": [message], "color": "red"}


def here(line: str, cursor: int) -> dict:
    """Commands.finishParsing: the line with the place of the error marked."""
    parts = (["..."] if cursor > 10 else []) + ([line[max(0, cursor - 10):cursor]] if cursor else [])
    if cursor < len(line):
        parts.append({"text": line[cursor:], "color": "red", "underlined": mc.Byte(1)})
    parts.append({"translate": "command.context.here", "color": "red", "italic": mc.Byte(1)})
    return failure({"text": "", "color": "gray", "extra": parts,
                    "click_event": {"action": "suggest_command", "command": "/" + line}})


def player_name(name: str) -> dict:
    """Player.getDisplayName: a click suggests a whisper, pointing shows the entity."""
    raw = bytes.fromhex(mc.offline_uuid(name))
    return {"text": name, "insertion": name, "click_event": {"action": "suggest_command", "command": f"/tell {name} "},
            "hover_event": {"action": "show_entity", "id": "minecraft:player", "name": name,
                            "uuid": mc.IntArray(struct.unpack(">4i", raw))}}


def item_name(item: str, count: int = 1, block: bool = False, color: str = "white") -> dict:
    """ItemStack.getDisplayName of a stack without components."""
    hover = {"action": "show_item", "id": "minecraft:" + item}
    if count != 1:
        hover["count"] = count
    return {"translate": "chat.square_brackets", "color": color, "hover_event": hover,
            "with": [{"text": "", "extra": [tr(("block" if block else "item") + ".minecraft." + item)]}]}


def admin(who, message) -> dict:
    """CommandSourceStack.broadcastToAdmins: what the other operators read."""
    return {"translate": "chat.type.admin", "with": [who, message], "color": "gray", "italic": mc.Byte(1)}


def make_op(srv: Server, c: Client, name: str) -> None:
    """`op` on the console; the client is told its new level and sent the tree that goes with it."""
    srv.console(f"op {name}")
    srv.wait_log(f"Made {name} a server operator")
    # An operator by the time the console's answer goes round: it is the first thing it reads as one.
    assert c.recv_pending_chat() == [admin("Server", tr("commands.op.success", name))]
    assert c.permission_level == 4, c.permission_level


@test()
def command_tree_and_operators(srv: Server):
    """Who may run what: the tree a client is sent, the console, /op and /deop, what the other
    operators are told."""
    a, b = srv.client(), srv.client()
    join(a, "Alice")
    join(b, "Bob")
    a.expect("player_info_update")
    for cl in (a, b):
        cl.send("player_loaded")
    settle(a, b)

    # Nobody is an operator: level 0, and a tree of the commands anyone may use.
    assert a.permission_level == 0 and sorted(a.commands["children"]) == ["help", "list", "me", "msg", "teammsg", "tell", "tm", "w"], \
        a.commands["children"].keys()
    lst = a.commands["children"]["list"]
    assert lst["executable"] and not lst["restricted"] and list(lst["children"]) == ["uuids"] and lst["children"]["uuids"]["executable"]
    assert a.commands["children"]["help"]["children"]["command"]["properties"] == {"type": "greedy"}
    # What is not in the tree is not a command, for this player.
    line = "setblock 8 -60 12 stone"
    got = run_command(a, line)
    assert got["said"] == [failure(tr("command.unknown.command")), here(line, 0)] and not got["blocks"], got
    assert run_command(a, "list")["said"] == [tr("commands.list.players", 2, 500, {"text": "", "extra": [
        player_name("Alice"), {"text": ", ", "color": "gray"}, player_name("Bob")]})]
    assert run_command(a, "help")["said"] == ["/me <action>", "/help [<command>]", "/list [uuids]", "/msg <targets> <message>",
                                              "/tell -> msg", "/w -> msg", "/teammsg <message>", "/tm -> teammsg"]

    # The console makes Alice an operator: entity event 28, the whole tree, a line in ops.json.
    make_op(srv, a, "Alice")
    tree = a.commands["children"]
    assert sorted(tree) == ["data", "deop", "fill", "give", "help", "kick", "kill", "list", "me", "msg", "op", "playsound", "say",
                            "setblock", "stop", "stopsound", "summon", "teammsg", "teleport", "tell", "tellraw", "tick", "time", "tm", "tp",
                            "w"], sorted(tree)
    assert tree["setblock"]["restricted"] and not tree["setblock"]["executable"] and not tree["list"]["restricted"]
    pos = tree["setblock"]["children"]["pos"]
    assert pos["parser"] == "kBlockPos" and not pos["executable"]
    block = pos["children"]["block"]
    assert block["parser"] == "kBlockState" and block["executable"] and list(block["children"]) == ["destroy", "keep", "replace", "strict"]
    targets = tree["give"]["children"]["targets"]
    assert targets["properties"] == {"amount": "multiple", "type": "players"}
    count = targets["children"]["item"]["children"]["count"]
    assert (count["parser"], count["properties"], count["executable"]) == ("kInteger", {"min": 1}, True)
    sound = tree["playsound"]["children"]["sound"]
    assert (sound["parser"], sound["suggestions"]) == ("kResourceLocation", "minecraft:available_sounds")
    assert list(sound["children"]) == ["master", "music", "record", "weather", "block", "hostile", "neutral", "player", "ambient", "voice", "ui"]
    pitch = sound["children"]["block"]["children"]["targets"]["children"]["pos"]["children"]["volume"]["children"]["pitch"]
    assert pitch["properties"] == {"min": 0.0, "max": 2.0}
    entity = tree["summon"]["children"]["entity"]
    assert (entity["parser"], entity["properties"], entity["suggestions"]) == ("kResource", {"registry": "minecraft:entity_type"}, "minecraft:summonable_entities")
    assert tree["tp"]["redirect"] == "teleport" and not tree["tp"]["children"] and tree["tp"]["restricted"]
    assert list(tree["teleport"]["children"]) == ["location", "destination", "targets"]
    assert tree["teleport"]["children"]["destination"]["properties"] == {"amount": "single", "type": "entities"}
    ops = json.load(open(srv.ops_file))
    dashed = mc.offline_uuid("Alice")
    dashed = "-".join((dashed[:8], dashed[8:12], dashed[12:16], dashed[16:20], dashed[20:]))
    assert ops == [{"uuid": dashed, "name": "Alice", "level": 4, "bypassesPlayerLimit": False}], ops
    assert b.permission_level == 0 and sorted(b.commands["children"]) == ["help", "list", "me", "msg", "teammsg", "tell", "tm", "w"]

    # Alice ops Bob: both read it - Alice as the answer, Bob as what an operator did.
    got = run_command(a, "op Bob")
    done = tr("commands.op.success", "Bob")
    assert got["said"] == [done], got
    assert b.recv_pending_chat() == [admin(player_name("Alice"), done)]
    assert b.permission_level == 4 and "setblock" in b.commands["children"]
    assert run_command(a, "op Bob")["said"] == [failure(tr("commands.op.failed"))]
    assert "[Alice: Made Bob a server operator]" in srv.read_log()
    # What Bob does now, Alice is told about; what only answers a question (list) she is not.
    got = run_command(b, "setblock 8 -60 12 stone")
    changed = tr("commands.setblock.success", 8, -60, 12)
    assert got["said"] == [changed] and got["blocks"] == {(8, -60, 12): mc.BLOCK["kStone"]}, got
    run_command(b, "list")
    assert a.recv_pending_chat() == [admin(player_name("Bob"), changed)]

    # /deop: back to level 0 and the small tree; a third time there is nobody to deop.
    assert run_command(a, "deop Bob")["said"] == [tr("commands.deop.success", "Bob")]
    settle(b)
    assert b.permission_level == 0 and sorted(b.commands["children"]) == ["help", "list", "me", "msg", "teammsg", "tell", "tm", "w"]
    assert run_command(a, "deop Bob")["said"] == [failure(tr("commands.deop.failed"))]
    assert [o["name"] for o in json.load(open(srv.ops_file))] == ["Alice"]
    # A name nobody has: in offline mode it is a profile all the same (and can be opped ahead of joining).
    assert run_command(a, "op Carol")["said"] == [tr("commands.op.success", "Carol")]
    c = srv.client()
    join(c, "Carol")
    assert c.permission_level == 4 and "stop" in c.commands["children"]

    # The console is a source too: it runs everything, and reads the answers in its log.
    srv.console("list")
    srv.wait_log("There are 3 of a max of 500 players online: Alice, Bob, Carol")
    srv.console("/setblock 8 -60 13 glowstone")
    srv.wait_log("Changed the block at 8, -60, 13")
    srv.console("nonsense")
    srv.wait_log("nonsense<--[HERE]")
    srv.console("kill")
    srv.wait_log("An entity is required to run this command here")


@test()
def command_blocks(srv: Server):
    """/setblock and /fill: the modes, what they answer, what they refuse."""
    a, b = srv.client(), srv.client()
    join(a, "Alice")
    join(b, "Bob")
    a.expect("player_info_update")
    for cl in (a, b):
        cl.send("player_loaded")
    settle(a, b)
    make_op(srv, a, "Alice")
    stone, glow, chest_state = mc.BLOCK["kStone"], mc.BLOCK["kGlowstone"], mc.BLOCK["kChest"]

    # Relative to where Alice stands (8.5, -60, 8.5): "~ ~ ~3" is the block (8, -60, 11).
    at = (8, -60, 11)
    got = run_command(a, "setblock ~ ~ ~3 stone")
    assert got["said"] == [tr("commands.setblock.success", *at)] and got["blocks"] == {at: stone}, got
    assert collect(b)[at] == stone
    assert run_command(a, "setblock ~ ~ ~3 minecraft:stone")["said"] == [failure(tr("commands.setblock.failed"))]   # no change
    assert run_command(a, "setblock 8 -60 11 glowstone keep")["said"] == [failure(tr("commands.setblock.failed"))]  # not air
    assert run_command(a, "setblock 8 -60 11 glowstone replace")["blocks"] == {at: glow}
    # destroy: the old block breaks first and drops what it drops - glowstone dust.
    got = run_command(a, "setblock 8 -60 11 air destroy")
    assert got["said"] == [tr("commands.setblock.success", *at)] and got["blocks"] == {at: AIR}
    dust = [e for e in of_type(a, "kItem").values() if e.data[D_ITEM][1][0] == I["kGlowstoneDust"]]
    assert len(dust) == 1 and all(lo <= v < lo + 1 for v, lo in zip(dust[0].pos, at)), [e.pos for e in dust]
    # Properties: a chest facing east, the right half of a pair; `strict` leaves the neighbours alone.
    first = chest_state - 1
    got = run_command(a, "setblock 8 -60 11 chest[facing=east]")
    assert got["blocks"] == {at: first + 3 * 6 + 1}, got
    got = run_command(a, "setblock 8 -60 12 chest[facing=east,type=right] strict")
    assert got["blocks"] == {(8, -60, 12): first + 3 * 6 + 4 + 1}, got
    # Without `strict` the neighbours take their shape: the single chest joins the half beside it.
    got = run_command(a, "setblock 8 -60 12 chest[facing=east,type=right]")
    assert got["said"] == [failure(tr("commands.setblock.failed"))]             # it is that already
    got = run_command(a, "setblock 8 -60 12 chest[facing=east,type=right,waterlogged=false] destroy")
    assert got["blocks"] == {(8, -60, 12): first + 3 * 6 + 4 + 1, at: first + 3 * 6 + 2 + 1}, got    # ...as the left half
    # What a command replaces is not spilled: the chest's contents go with it.
    give(a, 36, I["kStone"], 20)
    click(a, at, 1, seq=1)
    collect(a), collect(b)
    assert a.window and a.window["type"] == "kGeneric9x6"
    menu_click(a, 54 + 27, 0, QUICK_MOVE)
    settle(a)
    mark = len(a.menu_log)
    got = run_command(a, "fill 8 -60 11 8 -60 12 air")
    assert got["said"] == [tr("commands.fill.success", 2)] and got["blocks"] == {at: AIR, (8, -60, 12): AIR}, got
    settle(a)
    collect(b)
    assert a.window is None and ("container_close", 1) in a.menu_log[mark:]    # the screen closes with its chest
    # (Not a count of the items lying about: the chests the commands above broke are among them,
    # and whether Bob has walked into one of those by now is chance.)
    assert not [e for e in of_type(b, "kItem").values() if e.data[D_ITEM][0] == STACK and e.data[D_ITEM][1][0] == I["kStone"]]

    # What is not a block, what a block does not have, what this server cannot simulate yet.
    line = "setblock 8 -60 11 nonsense"
    assert run_command(a, line)["said"] == [failure(tr("argument.block.id.invalid", "minecraft:nonsense")), here(line, 18)]
    line = "setblock 8 -60 11 chest[facing=up]"
    assert run_command(a, line)["said"] == [failure(tr("argument.block.property.invalid", "minecraft:chest", "up", "facing")), here(line, 31)]
    line = "setblock 8 -60"
    assert run_command(a, line)["said"] == [failure(tr("argument.pos3d.incomplete")), here(line, 9)]
    line = "setblock 8 -60 11 stone now"
    assert run_command(a, line)["said"] == [failure(tr("command.unknown.argument")), here(line, 24)]
    assert run_command(a, "setblock 8 -60 11")["said"] == [failure(tr("command.unknown.command")), here("setblock 8 -60 11", 17)]
    assert run_command(a, "setblock 8 999 11 stone")["said"] == [failure(tr("argument.pos.outofworld"))]
    assert run_command(a, "setblock 100000 -60 11 stone")["said"] == [failure(tr("argument.pos.unloaded"))]
    got = run_command(a, "setblock 8 -60 11 lever")
    assert got["said"] == [failure("The block minecraft:lever is not supported by this server yet")] and not got["blocks"]

    # /fill: a 5 x 3 x 3 box, east of Alice.
    lo, hi = (10, -60, 10), (14, -58, 12)
    box = [(x, y, z) for x in range(lo[0], hi[0] + 1) for y in range(lo[1], hi[1] + 1) for z in range(lo[2], hi[2] + 1)]
    shell = [p for p in box if any(v in (l, h) for v, l, h in zip(p, lo, hi))]
    got = run_command(a, "fill 10 -60 10 14 -58 12 stone")
    assert got["said"] == [tr("commands.fill.success", 45)] and got["blocks"] == {p: stone for p in box}
    assert collect(b) == got["blocks"]
    got = run_command(a, "fill 14 -58 12 10 -60 10 glowstone hollow")      # the corners in the other order
    assert got["said"] == [tr("commands.fill.success", 45)] and len(shell) == 42
    assert got["blocks"] == {p: (glow if p in shell else AIR) for p in box}
    got = run_command(a, "fill 10 -60 10 14 -58 12 stone outline")
    assert got["said"] == [tr("commands.fill.success", 42)] and got["blocks"] == {p: stone for p in shell}
    got = run_command(a, "fill 10 -60 10 14 -58 12 oak_planks keep")       # only where there is air: the inside
    assert got["said"] == [tr("commands.fill.success", 3)] and set(got["blocks"]) == set(box) - set(shell)
    got = run_command(a, "fill 10 -60 10 14 -58 12 glowstone replace #minecraft:planks")
    assert got["said"] == [tr("commands.fill.success", 3)] and got["blocks"] == {p: glow for p in set(box) - set(shell)}
    got = run_command(a, "fill 10 -60 10 14 -58 12 glowstone replace stone strict")
    assert got["said"] == [tr("commands.fill.success", 42)]
    assert run_command(a, "fill 10 -60 10 14 -58 12 glowstone")["said"] == [failure(tr("commands.fill.failed"))]   # nothing to change
    assert run_command(a, "fill 10 -60 10 14 -58 12 stone replace stone")["said"] == [failure(tr("commands.fill.failed"))]
    got = run_command(a, "fill 0 -60 0 40 -30 40 stone")
    assert got["said"] == [failure(tr("commands.fill.toobig", 32768, mc.Long(41 * 31 * 41)))] and not got["blocks"]
    assert run_command(a, "fill 10 -60 10 14 -58 12 air")["said"] == [tr("commands.fill.success", 45)]

    # TNT beside a block of redstone: set by a command it goes off (onPlace) - unless `strict`.
    run_command(a, "setblock 12 -60 10 redstone_block")
    got = run_command(a, "setblock 13 -60 10 tnt strict")
    assert got["blocks"] == {(13, -60, 10): mc.BLOCK["kTnt"]} and not of_type(a, "kTnt")
    got = run_command(a, "setblock 11 -60 10 tnt")
    assert got["said"] == [tr("commands.setblock.success", 11, -60, 10)] and len(of_type(a, "kTnt")) == 1
    # ...and the one set strictly hears of the next change beside it.
    run_command(a, "setblock 13 -59 10 stone")
    assert len(of_type(a, "kTnt")) == 2
    run_command(a, "kill @e[type=tnt]")
    collect(b)
    assert not of_type(a, "kTnt") and not of_type(b, "kTnt")


def plain_text(component) -> str:
    return shown(component)


def shown(component) -> str:
    """Component.getString of literal text: a text and its children."""
    if isinstance(component, str):
        return component
    return str(component.get("text", "")) + "".join(shown(x) for x in component.get("extra", []))


@test()
def command_data_and_tellraw(srv: Server):
    """Tags as text, out and in: /data get of a block entity, an entity and a storage, with a
    path and a scale; an entity selector's nbt=; /tellraw. (What a vanilla server answers to the
    same lines is compared by tools/vanilla/command_probe.py.)"""
    a, b = srv.client(), srv.client()
    join(a, "Alice")
    join(b, "Bob")
    a.expect("player_info_update")
    for cl in (a, b):
        cl.send("player_loaded")
    settle(a, b)
    make_op(srv, a, "Alice")
    alice = player_name("Alice")

    def data(line: str):
        """A /data get that works: its key, the arguments in front of the tag, the tag as it reads."""
        said = run_command(a, line)["said"]
        assert len(said) == 1 and "with" in said[0], said
        return said[0]["translate"], said[0]["with"][:-1], shown(said[0]["with"][-1])

    # A block entity: what it saves, its entries in the order of vanilla's HashMap.
    run_command(a, "setblock 8 -60 12 chest")
    assert data("data get block 8 -60 12") == ("commands.data.block.query", [8, -60, 12],
                                               '{components: {}, x: 8, y: -60, Items: [], z: 12, id: "minecraft:chest"}')
    # ...shown token by token: names aqua, numbers gold, strings green between plain quotes.
    tag = run_command(a, "data get block 8 -60 12")["said"][0]["with"][3]
    assert tag["extra"][:6] == ["{", "", {"text": "components", "color": "aqua"}, ":", " ", "{"], tag["extra"][:6]
    assert {"text": "-60", "color": "gold"} in tag["extra"] and {"text": "minecraft:chest", "color": "green"} in tag["extra"]
    run_command(a, "setblock 8 -60 12 furnace")
    assert data("data get block 8 -60 12")[2] == (
        '{lit_total_time: 0, components: {}, cooking_time_spent: 0, speed_multiplier: 1.0f, x: 8, y: -60, cooking_total_time: 0, '
        'Items: [], z: 12, id: "minecraft:furnace", lit_time_remaining: 0, RecipesUsed: {}}')
    # A path names one tag in it; with a scale, a number times it, floored ("%.2f" of the scale).
    assert data("data get block 8 -60 12 id") == ("commands.data.block.query", [8, -60, 12], '"minecraft:furnace"')
    assert run_command(a, "data get block 8 -60 12 z 2.5")["said"] == [tr("commands.data.block.get", "z", 8, -60, 12, "2.50", 30)]
    assert run_command(a, "data get block 8 -60 12 y 0.125")["said"] == [tr("commands.data.block.get", "y", 8, -60, 12, "0.13", -8)]
    assert run_command(a, "data get block 8 -60 12 id 2")["said"] == [failure(tr("commands.data.get.invalid", "id"))]
    assert run_command(a, "data get block 8 -60 12 Items[0]")["said"] == [failure(tr("arguments.nbtpath.nothing_found", "Items[0]"))]
    assert run_command(a, "data get block 8 -60 12 nope.deeper")["said"] == [failure(tr("arguments.nbtpath.nothing_found", "nope"))]
    assert run_command(a, "data get block 8 -60 13")["said"] == [failure(tr("commands.data.block.invalid"))]
    run_command(a, "setblock 8 -60 12 air")

    # An entity: a player's place, what it holds, what it has - by path, by index, by what an element holds.
    run_command(a, "give @s diamond 3")
    assert data("data get entity @s Pos") == ("commands.data.entity.query", [alice], "[8.5d, -60.0d, 8.5d]")
    assert data("data get entity @s SelectedItem") == ("commands.data.entity.query", [alice], '{count: 3, id: "minecraft:diamond"}')
    assert data("data get entity @s Inventory[{Slot:0b}].count")[2] == "3"
    assert data("data get entity @s UUID[0]")[2] == str(struct.unpack(">i", bytes.fromhex(mc.offline_uuid("Alice"))[:4])[0])
    assert run_command(a, "data get entity @s Pos[1] 10")["said"] == [tr("commands.data.entity.get", "Pos[1]", alice, "10.00", -600)]
    assert run_command(a, "data get entity @s Pos[]")["said"] == [failure(tr("commands.data.get.multiple"))]
    assert run_command(a, "data get entity @a")["said"][0] == failure(tr("argument.entity.toomany"))
    whole = data("data get entity @s")[2]
    for part in ("Health: 20.0f", "Air: 300s", "playerGameType: 1", 'Dimension: "minecraft:overworld"',
                 "SelectedItemSlot: 0", 'Inventory: [{count: 3, Slot: 0b, id: "minecraft:diamond"}]', "Brain: {memories: {}}"):
        assert part in whole, (part, whole)
    # A stack with components: each as its Codec writes it, under its name.
    a.send("set_creative_mode_slot", struct.pack(">h", 37) + mc.item_stack(I["kStone"], 1, {
        "kCustomName": mc.nbt({"text": "Named", "color": "red"}), "kDamage": varint(3), "kUnbreakable": b""}))
    settle(a)
    assert data("data get entity @s Inventory[{Slot:1b}]")[2] == (
        '{components: {"minecraft:damage": 3, "minecraft:unbreakable": {}, "minecraft:custom_name": {color: "red", text: "Named"}}, '
        'count: 1, Slot: 1b, id: "minecraft:stone"}')
    assert data("data get entity @s Inventory[{components:{\"minecraft:damage\":3}}].id")[2] == '"minecraft:stone"'
    assert run_command(a, "kill @e[nbt={Inventory:[{components:{\"minecraft:damage\":4}}]}]")["said"] == [
        failure(tr("argument.entity.notfound.entity"))]
    a.send("set_creative_mode_slot", struct.pack(">h", 37) + mc.item_stack(0, 0))
    settle(a)

    # A storage: nothing writes one here, and one nobody wrote to is empty.
    assert data("data get storage some:where") == ("commands.data.storage.query", ["some:where"], "{}")
    assert run_command(a, "data get storage some:where a.b")["said"] == [failure(tr("arguments.nbtpath.nothing_found", "a"))]

    # nbt= in a selector: who saves what the tag asks for.
    run_command(a, "summon armor_stand 10 -60 10")
    run_command(b, "list")      # (takes the block changes Bob was sent meanwhile)
    settle(a, b)
    stand = [e for e in a.entities.values() if e.type == mc.ENTITY_TYPE["kArmorStand"]]
    assert len(stand) == 1
    assert run_command(a, "kill @e[type=armor_stand,nbt={Small:1b}]")["said"] == [failure(tr("argument.entity.notfound.entity"))]
    assert run_command(a, "kill @e[nbt={Small:0b,Pose:{}},nbt=!{Invisible:1b}]")["said"][0]["translate"] == "commands.kill.success.single"
    assert run_command(a, "tp @s[nbt={SelectedItem:{id:\"minecraft:diamond\",count:3}}] ~ ~ ~")["said"][0]["translate"] == \
        "commands.teleport.success.location.single"
    assert run_command(a, "tp @s[nbt={SelectedItem:{count:4}}] ~ ~ ~")["said"] == [failure(tr("argument.entity.notfound.entity"))]
    line = "tp @s[nbt={Pos:[1.0d,}] ~ ~ ~"   # read by three ways of writing /tp, wrong in each: no one error
    assert run_command(a, line)["said"] == [failure(tr("command.unknown.argument")), here(line, 3)]

    # /tellraw: a component as SNBT, in vanilla's form to whoever is named.
    mark = len(b.chat)
    assert run_command(a, "tellraw @a {text:'hi',color:\"red\",extra:[{text:\" there\",bold:true},\"!\"]}")["said"] == [
        {"text": "hi", "color": "red", "extra": [{"text": " there", "bold": mc.Byte(1)}, "!"]}]
    settle(a, b)
    assert b.said(mark) == [{"text": "hi", "color": "red", "extra": [{"text": " there", "bold": mc.Byte(1)}, "!"]}]
    assert run_command(a, "tellraw @s plain")["said"] == ["plain"]
    assert run_command(a, "tellraw Nobody \"x\"")["said"] == [failure(tr("argument.entity.notfound.player"))]
    line = "tellraw @s [300b]"
    assert run_command(a, line)["said"] == [
        failure(tr("snbt.parser.number_parse_failure", 'Value out of range. Value:"300" Radix:10')), here(line, 16)]
    line = "tellraw @s {text:\"a\""
    assert run_command(a, line)["said"] == [failure(tr("argument.literal.incorrect", ",")), here(line, 20)]
    assert run_command(b, "tellraw @s \"x\"")["said"][0] == failure(tr("command.unknown.command"))   # not an operator's


@test()
def command_teleport_give_summon(srv: Server):
    """/tp, /give, /summon, /kill: what moves, what appears, what everyone is told."""
    a, b = srv.client(), srv.client()
    ja = join(a, "Alice")
    jb = join(b, "Bob")
    a.expect("player_info_update")
    for cl in (a, b):
        cl.send("player_loaded")
    settle(a, b)
    make_op(srv, a, "Alice")
    ida, idb = ja["login"]["entity_id"], jb["login"]["entity_id"]
    alice, bob = player_name("Alice"), player_name("Bob")

    # Relative: the client is sent offsets and told which parts are such (X Y Z, both angles,
    # and the velocity along each axis: 0xFF); the others see her five blocks up.
    got = run_command(a, "tp ~ ~5 ~")
    assert got["said"] == [tr("commands.teleport.success.location.single", alice, "8.500000", "-55.000000", "8.500000")], got
    assert got["teleports"] == [{"id": 2, "pos": (0.0, 5.0, 0.0), "delta": (0.0, 0.0, 0.0), "rot": (0.0, 0.0), "relative": 0xFF}]
    settle(a, b)
    assert b.entities[ida].pos == (8.5, -55.0, 8.5), b.entities[ida].pos
    # Absolute, with a rotation; a whole number is the middle of its block.
    got = run_command(a, "tp @s 10 -60 9 90 45")
    assert got["said"] == [tr("commands.teleport.success.location.single", alice, "10.500000", "-60.000000", "9.500000")]
    assert got["teleports"] == [{"id": 3, "pos": (10.5, -60.0, 9.5), "delta": (0.0, 0.0, 0.0), "rot": (90.0, 45.0), "relative": 0}]
    settle(a, b)
    e = b.entities[ida]
    assert e.pos == (10.5, -60.0, 9.5) and (e.yaw, e.pitch) == (90.0, 45.0), (e.pos, e.yaw, e.pitch)
    # Others: Bob to Alice, then everyone to a place - and each is told where it now is.
    got = run_command(a, "teleport Bob Alice")
    assert got["said"] == [tr("commands.teleport.success.entity.single", bob, alice)] and not got["teleports"]
    tp = run_command(b, "list")["teleports"]
    assert len(tp) == 1 and (tp[0]["pos"], tp[0]["rot"], tp[0]["relative"]) == ((10.5, -60.0, 9.5), (90.0, 45.0), 0), tp
    got = run_command(a, "tp @a 8.5 -60 8.5")
    assert got["said"] == [tr("commands.teleport.success.location.multiple", 2, "8.500000", "-60.000000", "8.500000")]
    assert got["teleports"][0]["relative"] == 0x18      # the rotation was not given: it stays (Y_ROT | X_ROT)
    run_command(b, "list")
    settle(a, b)
    assert a.entities[idb].pos == (8.5, -60.0, 8.5) and b.entities[ida].pos == (8.5, -60.0, 8.5)
    # Far away: the view follows, and the place is as real as any (a block can be set there).
    got = run_command(a, "tp 1000 -60 1000", ticks=6)
    assert got["teleports"][0]["pos"] == (1000.5, -60.0, 1000.5)
    a.sync_chunks(2, (62, 62))     # (made by the workers, in their own time: so many ticks are no measure of it)
    got = run_command(a, "setblock ~ ~ ~1 stone")
    assert got["blocks"] == {(1000, -60, 1001): mc.BLOCK["kStone"]}, got
    settle(a, b)
    assert ida not in b.entities                        # out of Bob's sight
    run_command(a, "tp Bob", ticks=6)
    settle(a, b)
    assert b.entities[ida].pos == (8.5, -60.0, 8.5)
    # What cannot be: outside the world, nobody of that name, a selector that does not parse.
    assert run_command(a, "tp 0 30000000 0")["said"] == [failure(tr("commands.teleport.invalidPosition"))]
    assert run_command(a, "tp Nobody")["said"] == [failure(tr("argument.entity.notfound.entity"))]
    line = "kill @e[type=nonsense]"
    assert run_command(a, line)["said"] == [failure(tr("argument.entity.options.type.invalid", "minecraft:nonsense")), here(line, 13)]
    line = "give @e stone"
    assert run_command(a, line)["said"] == [failure(tr("argument.player.entities")), here(line, 0)]

    # /give: into the inventory, with the sound and the item that flies in - there for one tick.
    sounds, log = len(b.sounds), len(b.entity_log)
    got = run_command(a, "give @s diamond 3")
    assert got["said"] == [tr("commands.give.success.single", 3, item_name("diamond"), alice)], got
    assert a.slots[36] == plain("kDiamond", 3) and server_menu(a)[36] == plain("kDiamond", 3)
    settle(b)
    sound, source, where, volume, pitch = b.sounds[sounds]
    assert (sound, source, where) == (mc.SOUND["kEntityItemPickup"], 7, (8.5, -60.0, 8.5)), b.sounds[sounds]
    assert abs(volume - 0.2) < 1e-6 and 0.6 <= pitch <= 3.4, (volume, pitch)
    flown = [(n, w) for n, eid, w in b.entity_log[log:] if n in ("add_entity", "remove_entities")]
    assert [n for n, w in flown] == ["add_entity", "remove_entities"] and not of_type(b, "kItem"), flown
    got = run_command(a, "give @a stone 130")           # more than a stack: three of them
    assert got["said"] == [tr("commands.give.success.multiple", 130, item_name("stone", block=True), 2)]
    assert [a.slots[i] for i in (37, 38, 39)] == [plain("kStone", 64), plain("kStone", 64), plain("kStone", 2)]
    settle(b)
    assert b.slots[36] == plain("kStone", 64) and b.slots[38] == plain("kStone", 2)
    assert run_command(a, "give Bob enchanted_golden_apple")["said"] == [
        tr("commands.give.success.single", 1, item_name("enchanted_golden_apple", color="aqua"), bob)]
    got = run_command(a, "give @s ender_pearl 1601")    # a hundred stacks is the most
    assert got["said"] == [failure(tr("commands.give.failed.toomanyitems", 1600, item_name("ender_pearl")))]
    line = "give @s stone 0"
    assert run_command(a, line)["said"] == [failure(tr("argument.integer.low", 1, 0)), here(line, 14)]
    # With components: each a tag as text, read by the component's Codec. The answer shows the
    # stack as it is - its own name in italics, the colour of its rarity (an enchantment raises
    # it), and under the pointer the item with its components, in the order of vanilla's HashMap.
    got = run_command(a, "give @s diamond_sword[custom_name='Sting',enchantments={sharpness:5},damage=7]")
    shown = got["said"][0]["with"][1]
    assert got["said"][0]["translate"] == "commands.give.success.single" and shown["color"] == "aqua", got
    assert shown["with"] == [{"text": "", "extra": ["Sting"], "italic": mc.Byte(1)}], shown
    assert shown["hover_event"] == {"action": "show_item", "id": "minecraft:diamond_sword", "components": {
        "minecraft:damage": 7, "minecraft:enchantments": {"minecraft:sharpness": 5}, "minecraft:custom_name": "Sting"}}, shown
    settle(a)
    sword = next(s for s in a.slots if s and s[0] == I["kDiamondSword"])
    assert set(sword[2]) == {"kCustomName", "kEnchantments", "kDamage"} and mc.nbt(sword[2]["kCustomName"]) == mc.nbt("Sting"), sword
    line = "give @s diamond[damage=-1]"
    assert run_command(a, line)["said"] == [
        failure(tr("arguments.item.component.malformed", "minecraft:damage", "Not a valid value of this component")), here(line, 23)]
    line = "give @s diamond[damage=1,damage=2]"
    assert run_command(a, line)["said"] == [failure(tr("arguments.item.component.repeated", "minecraft:damage"))]
    line = "give @s diamond[food={}]"
    assert run_command(a, line)["said"] == [
        failure("Items with the component minecraft:food are not supported by this server yet"), here(line, 16)]
    assert run_command(a, "give @s diamond_sword[max_stack_size=16]")["said"] == [
        failure(tr("arguments.item.malformed", "Item cannot be both damageable and stackable"))]
    assert run_command(a, "give Nobody stone")["said"] == [failure(tr("argument.entity.notfound.player"))]

    # /summon: an entity like any other - shown, selected, killed. It has no mind of its own:
    # it stands where it is put, and falls when that is in the air.
    before = set(a.entities)
    got = run_command(a, "summon armor_stand ~2 ~ ~")
    (eid,) = set(a.entities) - before
    stand = a.entities[eid]
    assert stand.type == mc.ENTITY_TYPE["kArmorStand"] and stand.pos == (10.5, -60.0, 8.5), (stand.type, stand.pos)
    uuid = "-".join((stand.uuid[:8], stand.uuid[8:12], stand.uuid[12:16], stand.uuid[16:20], stand.uuid[20:]))
    shown = {"translate": "entity.minecraft.armor_stand", "insertion": uuid,
             "hover_event": {"action": "show_entity", "id": "minecraft:armor_stand", "name": tr("entity.minecraft.armor_stand"),
                             "uuid": mc.IntArray(struct.unpack(">4i", bytes.fromhex(stand.uuid)))}}
    assert got["said"] == [tr("commands.summon.success", shown)], got
    settle(b)
    assert b.entities[eid].pos == stand.pos
    run_command(a, "summon minecraft:armor_stand 12 -60 8")
    run_command(a, "summon cow")
    assert len(of_type(a, "kArmorStand")) == 2 and len(of_type(a, "kCow")) == 1
    # Selectors: the nearest stand; then by a box, by distance, by what they are not.
    mark = len(b.entity_log)
    got = run_command(a, "tp @e[type=armor_stand,sort=nearest,limit=1] ~ ~3 ~")
    assert got["said"] == [tr("commands.teleport.success.location.single", shown, "8.500000", "-57.000000", "8.500000")], got
    # Three blocks up it does not stay: it falls as a living thing does, and ten ticks later it
    # stands on the ground again - under where it was put, and the players it lands among do
    # not push it (ArmorStand.isPushable).
    settle(a, b, ticks=16)
    heard = [what[1] for name, what in log_of(b, eid, mark) if name in ("move_entity_pos", "entity_position_sync")]
    # (Nobody is told of the teleport itself: a stand's viewers hear where it is every third tick.)
    assert heard[0][0] == 8.5 and heard[0][2] == 8.5 and -57.5 < heard[0][1] <= -57.0, heard
    for cl in (a, b):
        assert cl.entities[eid].pos == (8.5, -60.0, 8.5) and cl.entities[eid].on_ground, cl.entities[eid].pos
    assert run_command(a, "kill @e[type=!player,type=!cow,type=!armor_stand,distance=..2]")["said"] == [failure(tr("argument.entity.notfound.entity"))]
    got = run_command(a, "kill @e[type=armor_stand,x=8,dx=0,y=-60,dy=1]")
    assert got["said"] == [tr("commands.kill.success.single", shown)] and eid not in a.entities
    got = run_command(a, "kill @e[type=!player]")
    assert got["said"] == [tr("commands.kill.success.multiple", 2)] and not of_type(a, "kArmorStand") and not of_type(a, "kCow")
    settle(b)
    assert set(b.entities) == {ida}
    assert run_command(a, "kill")["said"] == [failure("Players cannot be killed on this server yet")]
    line = "summon nonsense"
    assert run_command(a, line)["said"] == [failure(tr("argument.resource.not_found", "minecraft:nonsense", "minecraft:entity_type")), here(line, 15)]
    assert run_command(a, "summon player")["said"] == [failure(tr("entity.not_summonable", "minecraft:player"))]
    assert run_command(a, "summon cow 0 30000000 0")["said"] == [failure(tr("commands.summon.invalidPosition"))]
    # A summoned TNT is a primed one: it falls, and its fuse burns down from 80.
    run_command(a, "summon tnt ~ ~4 ~3")
    settle(a, ticks=12)
    ((tid, tnt),) = of_type(a, "kTnt").items()
    assert tnt.pos[1] < -56.0 and 60 <= tnt.data[mc.TNT_DATA["kFuse"]][1] < 80, (tnt.pos, tnt.data)
    # A client lets a primed TNT fall by itself between the server's updates, so when a command
    # moves one, whoever sees it is told at once (teleport_entity; a stand's viewers were not):
    # where it is now, and that a place given outright leaves it no velocity.
    mark = len(b.entity_log)
    run_command(a, "tp @e[type=tnt] 8.5 -40 14.5")
    settle(b, ticks=1)
    moved = [what for name, what in log_of(b, tid, mark) if name == "teleport_entity"]
    assert len(moved) == 1 and moved[0][0] == (8.5, -40.0, 14.5) and moved[0][1] == (0.0, 0.0, 0.0), log_of(b, tid, mark)
    run_command(a, "kill @e[type=tnt]")


def tick_stats(srv: Server) -> list:
    """The server's own account of its ticks (--debug, one line every 100 ticks): per line
    (ms avg, ms worst, ms entity tracker, ms entities' ticks, ms flush, KiB/tick, players, entities, at rest)."""
    return re.findall(r"tick: ([\d.]+) ms avg, ([\d.]+) ms worst, ([\d.]+) ms of it the entity tracker, ([\d.]+) ms the entities' own "
                      r"ticks, ([\d.]+) ms flush, [\d.]+ ms block ticks; ([\d.]+) KiB/tick sent; (\d+) players, (\d+) entities "
                      r"\((\d+) at rest\)", srv.read_log())


F32_098 = struct.unpack(">f", struct.pack(">f", 0.98))[0]      # (double) 0.98f


def living_fall(y: float, ground: float, ticks: int) -> list:
    """Where a living entity that nobody steers is after each tick of a fall onto flat ground:
    LivingEntity.aiStep and travelInAir, written out for one axis. [0] is where it starts.

    A tick: a velocity under 0.003 is dropped; the entity moves by its velocity, as far as the
    ground lets it (Entity.move keeps the position when it was stopped within 1e-7 of nothing);
    stopped, the velocity is gone; then gravity, 0.08, and the drag on what is left, 0.98f."""
    vy, out = 0.0, [y]
    for _ in range(ticks):
        if abs(vy) < 0.003:
            vy = 0.0
        moved = max(vy, ground - y) if vy < 0 else vy
        if moved * moved > 1.0e-7 or vy * vy - moved * moved < 1.0e-7:
            y = y + moved
        if moved != vy:
            vy = 0.0
        vy = (vy - 0.08) * F32_098
        out.append(y)
    return out


@test()
def things_fall(srv: Server):
    """What is in the air falls, what is thrown flies, what is walked into gives way - tick for
    tick as on a vanilla server (the arithmetic is tested against one in tests/unit/physics_test.cpp;
    this is that it runs, and that clients hear of it)."""
    a, b = srv.client(), srv.client()
    ja = join(a, "Alice")
    jb = join(b, "Bob")
    a.expect("player_info_update")
    for cl in (a, b):
        cl.send("player_loaded")
    settle(a, b)
    make_op(srv, a, "Alice")

    # ---- an armor stand put ten blocks up. Bob is told where it is every third tick, each time
    # with the ticks that passed (a living entity's path), in steps of 1/4096 of a block - and
    # exactly when it lands. Every one of those positions is on the path the model above takes.
    before = set(b.entities)
    run_command(a, "summon armor_stand 8.5 -50 12.5")
    settle(a, b, ticks=40)
    (eid,) = set(b.entities) - before
    stand = b.entities[eid]
    assert stand.spawned[0] == (8.5, -50.0, 12.5) and stand.pos == (8.5, -60.0, 12.5) and stand.on_ground, (stand.spawned, stand.pos)
    steps = list(stand.steps)
    told = [(name, what[1], steps.pop(0) if what[0] else 0)           # ...a step of so many ticks, or just a place
            for name, what in log_of(b, eid) if name in ("move_entity_pos", "move_entity_pos_rot", "entity_position_sync")]
    assert not steps and len(told) >= 6, (told, stand.steps)
    path = living_fall(-50.0, -60.0, 60)

    def on_path(first: int) -> bool:
        tick = first
        for name, pos, step in told:
            tick += step
            if not 0 <= tick < len(path) or (pos[0], pos[2]) != (8.5, 12.5):
                return False
            if abs(pos[1] - path[tick]) > (0.0 if name == "entity_position_sync" else 1 / 8192 + 1e-12):
                return False
        return True

    assert [first for first in range(-3, 4) if on_path(first)], (told, path[:30])
    assert told[-1][:2] == ("entity_position_sync", (8.5, -60.0, 12.5))   # the landing: on the ground, to the last bit

    # ---- a stone thrown straight ahead: three tenths of a block a tick the way Bob looks (+z),
    # a tenth up give or take a tenth, a fiftieth of anything; it lands three to five blocks off,
    # slides a little and lies there, on the ground to the last bit.
    look(b, 0.0, 0.0)
    give(b, 36, I["kStone"], 1)
    action(b, DROP_ONE)
    settle(a, b, ticks=60)
    (item,) = of_type(a, "kItem").values()
    where, (vx, vy, vz) = item.spawned
    assert where == (SPAWN[0], DROP_Y, SPAWN[2]), where
    assert abs(vx) < 0.0201 and -0.0001 < vy < 0.2001 and 0.2799 < vz < 0.3201, item.spawned
    x, y, z = item.pos
    assert y == SPAWN[1] and item.on_ground and abs(x - SPAWN[0]) < 0.6 and 2.5 < z - SPAWN[2] < 5.5, item.pos
    assert of_type(b, "kItem")[item.id].pos == item.pos

    # ---- a cow put where the two of them stand, a little to the side: each of them pushes it
    # (and it pushes itself off them) until nothing touches - a cow is 0.9 wide, a player 0.6.
    before = set(a.entities)
    run_command(a, "summon cow 8.9 -60 8.5")
    settle(a, b, ticks=40)
    (eid,) = set(a.entities) - before
    x, y, z = a.entities[eid].pos
    assert 9.2499 <= x < 10.5 and y == -60.0 and abs(z - 8.5) < 1e-9, (x, y, z)
    assert b.entities[eid].pos == (x, y, z)

    # ---- at rest. Seven seconds on, the three of them lie and stand still, and the server has
    # stopped ticking them in full (its own account of it, every five seconds, says so) - which
    # nobody can tell from outside: the item's viewers hear of it as often as ever.
    mark = len(a.entity_log)
    settle(a, b, ticks=140)
    assert int(tick_stats(srv)[-1][8]) == 3, tick_stats(srv)[-1]
    # (An item at rest falls for three ticks and is stopped by the ground in the fourth, and its
    # velocity is sent whenever it is found changed: at its update every 20 ticks - 0.04, 0.08 or
    # 0.12 down, by where in that round the update falls - and when the ground has stopped it
    # again. Unless its update falls on that very tick: then there is never anything to say.)
    heard = [what for name, what in log_of(a, item.id, mark) if name == "set_entity_motion"]
    assert not heard or (len(heard) >= 12 and {round(v[1] / -0.04) for v in heard} in ({0, 1}, {0, 2}, {0, 3})), heard
    # What wakes them: the ground going from under them...
    ix, iz = math.floor(item.pos[0]), math.floor(item.pos[2])
    run_command(a, f"fill {ix - 1} -61 {iz - 1} {ix + 1} -61 {iz + 1} air")
    run_command(a, "fill 8 -61 12 8 -61 12 air")
    collect(a, ticks=12)        # (the holes and their light, to both)
    collect(b, ticks=12)
    assert a.entities[item.id].pos[1] == -61.0 and a.entities[item.id].on_ground, a.entities[item.id].pos
    assert a.entities[stand.id].pos == (8.5, -61.0, 12.5) and b.entities[stand.id].pos == (8.5, -61.0, 12.5)
    # ...and somebody walking into them: Bob steps into the cow, and it gives way again.
    walk(b, jb, x - 0.3, 8.5, 10)
    settle(a, b, ticks=30)
    assert a.entities[eid].pos[0] > x + 0.3 and a.entities[eid].pos[1] == -60.0, (a.entities[eid].pos, x)


@test()
def command_sounds_kick_stop(srv: Server):
    """/playsound and /stopsound, /kick, the limit on commands in a row, and /stop."""
    a, b = srv.client(), srv.client()
    join(a, "Alice")
    join(b, "Bob")
    a.expect("player_info_update")
    for cl in (a, b):
        cl.send("player_loaded")
    settle(a, b)
    make_op(srv, a, "Alice")
    alice, bob = player_name("Alice"), player_name("Bob")
    harp = "minecraft:block.note_block.harp"

    # A sound by its name - whatever the name: the client looks it up, not the server.
    n = len(a.sounds)
    got = run_command(a, "playsound block.note_block.harp")
    assert got["said"] == [tr("commands.playsound.success.single", harp, alice)], got
    assert a.sounds[n:] == [(harp, 0, (8.5, -60.0, 8.5), 1.0, 1.0)] and len(b.sounds) == 0     # to herself, as "master"
    got = run_command(a, "playsound some:thing voice @a ~ ~2 ~ 0.5 2")
    assert got["said"] == [tr("commands.playsound.success.multiple", "some:thing", 2)]
    settle(b)
    assert a.sounds[-1] == ("some:thing", 9, (8.5, -58.0, 8.5), 0.5, 2.0) and b.sounds[-1] == a.sounds[-1]
    # Out of earshot nothing is sent - unless a least volume is given: then from two blocks away.
    assert run_command(a, "playsound some:thing block Bob ~ ~100 ~")["said"] == [failure(tr("commands.playsound.failed"))]
    got = run_command(a, "playsound some:thing block Bob ~ ~100 ~ 1 1 0.25")
    assert got["said"] == [tr("commands.playsound.success.single", "some:thing", bob)]
    settle(b)
    assert b.sounds[-1] == ("some:thing", 4, (8.5, -58.0, 8.5), 0.25, 1.0), b.sounds[-1]
    line = "playsound some:thing block @a ~ ~ ~ 1 3"
    assert run_command(a, line)["said"] == [failure(tr("argument.float.big", mc.Float(2.0), mc.Float(3.0))), here(line, 38)]
    line = "playsound Some:Thing"     # an identifier has no capitals: it ends before the first, empty
    assert run_command(a, line)["said"] == [failure(tr("command.expected.separator")), here(line, 10)]

    assert run_command(a, "stopsound @a")["said"] == [tr("commands.stopsound.success.sourceless.any")]
    assert run_command(a, "stopsound @a music")["said"] == [tr("commands.stopsound.success.source.any", "music")]
    assert run_command(a, "stopsound Bob * block.note_block.harp")["said"] == [tr("commands.stopsound.success.sourceless.sound", harp)]
    assert run_command(a, "stopsound Bob ui block.note_block.harp")["said"] == [tr("commands.stopsound.success.source.sound", harp, "ui")]
    settle(b)
    assert a.stopped_sounds == [(None, None), (1, None)] and b.stopped_sounds == [(None, None), (1, None), (None, harp), (10, harp)]

    # Twelve commands in a row are two too many for someone who is not an operator.
    for _ in range(12):
        b.send("chat_command", mc.mc_str("list"))
    name, r = b.recv()
    assert name == "disconnect" and r.nbt() == tr("disconnect.spam"), name
    b.expect_closed()
    for _ in range(12):                                   # an operator may
        a.send("chat_command", mc.mc_str("list"))
    assert len(a.recv_pending_chat()) == 12

    # /kick: with the default reason, and with one given; everyone left reads who went.
    c, d = srv.client(), srv.client()
    join(c, "Carol")
    join(d, "Dave")
    got = run_command(a, "kick Carol")
    reason = tr("multiplayer.disconnect.kicked")
    assert got["said"] == [tr("commands.kick.success", player_name("Carol"), reason)], got

    def kicked(cl: Client):
        """The reason this client is thrown out with (after whatever it was still to read)."""
        while True:
            name, r = cl.recv()
            if name == "disconnect":
                why = r.nbt()
                cl.expect_closed()
                return why

    assert kicked(c) == reason
    got = run_command(a, "kick Dave that is довольно")
    assert got["said"] == [tr("commands.kick.success", player_name("Dave"), "that is довольно")]
    assert kicked(d) == "that is довольно"
    assert run_command(a, "kick Nobody")["said"] == [failure(tr("argument.entity.notfound.player"))]
    # A section sign may not travel in chat: the one who types it is thrown out.
    e = srv.client()
    join(e, "Erin")
    e.send("chat_command", mc.mc_str("list §c"))
    assert kicked(e) == tr("multiplayer.disconnect.illegal_characters")

    # /stop: said, logged, done - the server ends by itself, cleanly.
    mark = len(a.chat)
    a.send("chat_command", mc.mc_str("stop"))
    while a.recv()[0] != "disconnect":
        pass
    assert a.said(mark) == [tr("commands.stop.stopping")], a.said(mark)
    assert srv.proc.wait(timeout=5) == 0
    assert "[Alice: Stopping the server]" in srv.read_log()


# ----------------------------------------------------------------------------- chat

CHAT_TYPE = {name.split(":")[1]: i for i, name in enumerate(mc.load_registry("chat_type"))}
A_SIGNATURE = bytes(range(256))


def last_seen(offset: int = 0, bits: int = 0, checksum: int = 0) -> bytes:
    """LastSeenMessages.Update: how far the window moved, which of its 20 were seen, a checksum."""
    return varint(offset) + bits.to_bytes(3, "little") + bytes([checksum])


def chat_body(message: str, signature: bytes | None = None, seen: bytes | None = None, stamp: int = 5, salt: int = 7) -> bytes:
    """ServerboundChatPacket: the message, when it was typed, a salt, a signature or none, what was seen."""
    return mc.mc_str(message) + struct.pack(">qq", stamp, salt) + (b"\x01" + signature if signature else b"\x00") + \
        (last_seen() if seen is None else seen)


def signed_command(line: str, signatures: dict | None = None, seen: bytes | None = None) -> bytes:
    """ServerboundChatCommandSignedPacket: the line, time and salt, a signature for each message
    argument a client with a key signed, what was seen."""
    sigs = signatures or {}
    return mc.mc_str(line) + struct.pack(">qq", 5, 7) + varint(len(sigs)) + \
        b"".join(mc.mc_str(k) + v for k, v in sigs.items()) + (last_seen() if seen is None else seen)


def announced(what: str, name: str) -> tuple:
    """"Steve joined the game" / "Steve left the game", as it stands in a client's chat."""
    return ("system", {"translate": "multiplayer.player." + what, "with": [player_name(name)], "color": "yellow"}, False)


def spoken(c: Client, since: int = 0) -> list:
    """What was said - by players and for them - in c.chat[since:], without the time it was said
    at (the server's own: checked here to be now)."""
    out = []
    for kind, m in ((e[0], e[1]) for e in c.chat[since:] if e[0] != "system"):
        m = dict(m)
        if kind == "player":
            assert abs(m.pop("timestamp") - time.time() * 1000) < 60_000, "a message carries the server's time"
        out.append((kind, m))
    return out


def by_player(name: str, content: str, kind: str = "chat", unsigned=None, target=None) -> tuple:
    """A message as its sender typed it: PlayerChatMessage.unsigned - the sender, no signature,
    the first of no session, nothing seen."""
    return ("player", {"sender": mc.offline_uuid(name), "index": 0, "signature": None, "content": content, "salt": 0,
                       "last_seen": [], "unsigned": unsigned, "type": CHAT_TYPE[kind], "name": player_name(name),
                       "target": target})


def disguised(name, message, kind: str, target=None) -> tuple:
    """A message that is nobody's: what the console says, what a command says for a player."""
    return ("disguised", {"message": message, "type": CHAT_TYPE[kind], "name": name, "target": target})


@test()
def chat(srv: Server):
    """What one player types, the others read - those who show the chat: it goes out as vanilla
    sends the message of a player without a chat session. Who comes and who goes is said to
    everyone. What does not belong in chat puts its sender out."""
    a, b, c, d = srv.client(), srv.client(), srv.client(), srv.client()
    join(a, "Alice")
    assert a.chat == []                              # nobody is told that it has come itself
    join(b, "Bob")
    join(c, "Carol", chat=1)                         # system messages only
    join(d, "Dave", chat=2)                          # nothing at all
    skip_tab_updates(a, 3)
    skip_tab_updates(b, 2)
    skip_tab_updates(c, 1)
    settle(a, b, c, d)
    assert a.chat == [announced("joined", n) for n in ("Bob", "Carol", "Dave")], a.chat
    assert b.chat == [announced("joined", n) for n in ("Carol", "Dave")] and c.chat == [announced("joined", "Dave")]
    assert d.chat == []
    log = srv.read_log()
    assert "Bob joined the game" in log, log[-600:]

    def say(who: Client, *args, **kw) -> list:
        """Send a chat packet; what everyone was then sent of chat, by client."""
        marks = [len(x.chat) for x in (a, b, c, d)]
        who.send("chat", chat_body(*args, **kw))
        settle(a, b, c, d)
        return [x.chat[m:] and spoken(x, m) + [e for e in x.chat[m:] if e[0] == "system"] for x, m in zip((a, b, c, d), marks)]

    # ---- a message: to the two who show chat, the sender among them; the first, index 0, for both.
    hello = by_player("Alice", "hello there")
    assert say(a, "hello there") == [[hello], [hello], [], []]
    assert a.chat_index == 1 and b.chat_index == 1 and c.chat_index == 0
    assert "[Not Secure] <Alice> hello there" in srv.read_log()
    # A signature, a time and a salt of the client's own change nothing: there is no session to
    # check them against, and the message goes out as unsigned as the first.
    signed = by_player("Bob", "мир, 世界 & \U0001F600")
    assert say(b, "мир, 世界 & \U0001F600", signature=A_SIGNATURE, stamp=123456789, salt=-1) == [[signed], [signed], [], []]
    # 256 characters are a message.
    long = by_player("Alice", "x" * 256)
    assert say(a, "x" * 256) == [[long], [long], [], []]
    # Carol does not show what players say - nor what she says herself. The others read her.
    carol = by_player("Carol", "anyone?")
    assert say(c, "anyone?") == [[carol], [carol], [], []]
    # Dave has the chat off: he is told so, in red, and nobody reads him.
    off = ("system", {"translate": "chat.disabled.options", "color": "red"}, False)
    assert say(d, "hello?") == [[], [], [], [off]]
    # ...and nothing a command answers reaches him either.
    assert run_command(d, "list")["said"] == [] and len(run_command(b, "list")["said"]) == 1
    # He turns it on (client_information in play): now he reads, and his count begins.
    d.send("client_information", mc.mc_str("en_us") + struct.pack(">b", 10) + varint(0) + b"\x01" + b"\x7f" + varint(1) +
           b"\x00\x01" + varint(0))
    dave = by_player("Dave", "there you are")
    assert say(d, "there you are") == [[dave], [dave], [], [dave]]
    assert (a.chat_index, b.chat_index, c.chat_index, d.chat_index) == (5, 5, 0, 1)

    # ---- what a client says about what it has seen: nothing, as nothing it is sent is signed.
    # The checksum of nothing is 1, and 0 is "not checked"; a chat_ack of 0 moves nothing.
    b.send("chat_ack", varint(0))
    b.send("chat_session_update", bytes(16) + struct.pack(">q", 1 << 41) + varint(294) + bytes(294) + varint(512) + bytes(512))
    fine = by_player("Bob", "fine")
    assert say(b, "fine", seen=last_seen(0, 0, 1)) == [[fine], [fine], [], [fine]]
    # Anything else is out of step with a server that keeps no window - and is put out.
    for i, (name, packet) in enumerate([("chat", chat_body("x", seen=last_seen(1, 0, 0))),
                                        ("chat", chat_body("x", seen=last_seen(0, 1 << 19, 0))),
                                        ("chat", chat_body("x", seen=last_seen(0, 1 << 20, 0))),
                                        ("chat", chat_body("x", seen=last_seen(0, 0, 2))),
                                        ("chat_command_signed", signed_command("list", seen=last_seen(3, 0, 0))),
                                        ("chat_ack", varint(1))]):
        mark = len(a.chat)
        e = srv.client()
        join(e, f"Eve{i}")
        e.send(name, packet)
        got, r = e.recv()
        assert got == "disconnect" and r.nbt() == tr("multiplayer.disconnect.chat_validation_failed"), (name, got)
        e.expect_closed()
        skip_tab_updates(a, 1)
        a.expect("player_info_remove")
        assert a.chat[mark:] == [announced("joined", f"Eve{i}"), announced("left", f"Eve{i}")], a.chat[mark:]   # and nothing it said
    for cl in (b, c, d):
        cl.ignore = {"player_info_update", "player_info_remove"}
    # A section sign, a control character: out.
    e = srv.client()
    join(e, "Mallory")
    e.send("chat", chat_body("a §c b"))
    got, r = e.recv()
    assert got == "disconnect" and r.nbt() == tr("multiplayer.disconnect.illegal_characters"), got
    e.expect_closed()
    skip_tab_updates(a, 1)
    a.expect("player_info_remove")
    settle(a, b, c, d)
    assert c.chat[-2:] == [announced("joined", "Mallory"), announced("left", "Mallory")], c.chat[-2:]
    assert d.chat[-2:] == [announced("joined", "Mallory"), announced("left", "Mallory")]
    assert "Mallory left the game" in srv.read_log()

    # ---- ten messages at once are one too many for someone who is not an operator: the tenth
    # still goes out, then its sender does.
    mark = len(a.chat)
    b.send_together(*[("chat", chat_body(f"spam {i}")) for i in range(12)])
    got, r = b.recv()
    assert got == "disconnect" and r.nbt() == tr("disconnect.spam"), got
    b.expect_closed()
    a.expect("player_info_remove")
    settle(a)
    assert spoken(a, mark) == [by_player("Bob", f"spam {i}") for i in range(10)], spoken(a, mark)
    make_op(srv, a, "Alice")
    mark = len(a.chat)
    a.send_together(*[("chat", chat_body(f"news {i}")) for i in range(12)])
    settle(a, d)
    assert spoken(a, mark) == [by_player("Alice", f"news {i}") for i in range(12)]
    assert spoken(d)[-1] == by_player("Alice", "news 11")


@test()
def chat_commands(srv: Server):
    """/say, /me, /msg and its other names, /teammsg: a player's own words where its client
    vouches for the line (chat_command_signed), somebody's otherwise; selectors in them are
    names for who may use selectors."""
    a, b, c = srv.client(), srv.client(), srv.client()
    join(a, "Alice")
    join(b, "Bob")
    join(c, "Carol")
    skip_tab_updates(a, 2)
    skip_tab_updates(b, 1)
    for cl in (a, b, c):
        cl.send("player_loaded")
    settle(a, b, c)
    make_op(srv, a, "Alice")
    alice, bob, carol = player_name("Alice"), player_name("Bob"), player_name("Carol")

    def typed(who: Client, line: str, signed: bool = True, **kw) -> list:
        """A command line as a client sends one with message arguments; what all three read."""
        marks = [len(x.chat) for x in (a, b, c)]
        who.send("chat_command_signed", signed_command(line, **kw)) if signed else who.send("chat_command", mc.mc_str(line))
        settle(a, b, c)
        return [spoken(x, m) + [e[1] for e in x.chat[m:] if e[0] == "system"] for x, m in zip((a, b, c), marks)]

    # ---- /me: anyone may. From a client the action is the player's own; a line that came as a
    # plain chat_command is said for it, by nobody.
    waves = by_player("Bob", "waves", "emote_command")
    assert typed(b, "me waves") == [[waves]] * 3
    for_bob = disguised(bob, "waves", "emote_command")
    assert typed(b, "me waves", signed=False) == [[for_bob]] * 3
    log = srv.read_log()
    assert "[Not Secure] * Bob waves" in log, log[-600:]
    # Bob may not use selectors: his "@a" is two characters.
    at = by_player("Bob", "greets @a", "emote_command")
    assert typed(b, "me greets @a") == [[at]] * 3
    # ...and not /say: it is not a command to him.
    assert typed(b, "say hi")[1] == [failure(tr("command.unknown.command")), here("say hi", 0)]

    # ---- /say, by an operator: selectors become names - shown in place of what was typed.
    shown = {"text": "hello ", "extra": [alice, "!"]}
    says = by_player("Alice", "hello @s!", "say_command", unsigned=shown)
    assert typed(a, "say hello @s!") == [[says]] * 3
    everyone = {"text": "", "extra": [alice, {"text": ", ", "color": "gray"}, bob, {"text": ", ", "color": "gray"}, carol]}
    all_of_you = by_player("Alice", "@a: tea?", "say_command", unsigned={"text": "", "extra": [everyone, ": tea?"]})
    assert typed(a, "say @a: tea?") == [[all_of_you]] * 3
    plain = by_player("Alice", "just this", "say_command")
    assert typed(a, "say just this") == [[plain]] * 3
    assert typed(a, "say hello @s!", signed=False) == [[disguised(alice, shown, "say_command")]] * 3
    # A selector that is wrong is the line's mistake; an '@' that is none is a character.
    line = "say hi @e[type=nothing]"
    got = typed(a, line)[0]
    assert got == [failure(tr("argument.entity.options.type.invalid", "minecraft:nothing")), here(line, 15)], got
    assert typed(a, "say a@b @ @x") == [[by_player("Alice", "a@b @ @x", "say_command")]] * 3
    # More than 256 characters are no message.
    assert typed(a, "say " + "x" * 257)[0] == [failure(tr("argument.message.too_long", 257, 256))]
    assert typed(a, "say " + "x" * 256)[2] == [by_player("Alice", "x" * 256, "say_command")]

    # ---- a client with a key signs its message arguments and names them: those and no others.
    assert typed(a, "say signed", signatures={"message": A_SIGNATURE}) == [[by_player("Alice", "signed", "say_command")]] * 3
    bad = {"text": "", "extra": [tr("chat.disabled.invalid_command_signature")], "color": "red"}
    assert typed(a, "say signed", signatures={"reason": A_SIGNATURE}) == [[bad], [], []]
    assert typed(a, "say signed", signatures={"message": A_SIGNATURE, "other": A_SIGNATURE}) == [[bad], [], []]
    assert typed(a, "list", signatures={"message": A_SIGNATURE}) == [[bad], [], []]

    # ---- /msg, /tell, /w: the one it is for reads it, the one who wrote it reads it back.
    for word in ("msg", "tell", "w"):
        got = typed(b, f"{word} Alice psst")
        assert got == [[by_player("Bob", "psst", "msg_command_incoming")],
                       [by_player("Bob", "psst", "msg_command_outgoing", target=alice)], []], got
    got = typed(a, "msg @a all of you")      # to each of three: three echoes, hers among what she is sent
    out = [by_player("Alice", "all of you", "msg_command_outgoing", target=t) for t in (alice, bob, carol)]
    into = by_player("Alice", "all of you", "msg_command_incoming")
    assert got == [[out[0], into, out[1], out[2]], [into], [into]], got
    assert typed(b, "msg Nobody hi")[1] == [failure(tr("argument.entity.notfound.player"))]
    assert typed(b, "msg Alice", signed=False)[1][0] == failure(tr("command.unknown.command"))
    assert typed(b, "msg Carol pst", signed=False) == [[], [disguised(bob, "pst", "msg_command_outgoing", target=carol)],
                                                       [disguised(bob, "pst", "msg_command_incoming")]]

    # ---- /teammsg: there are no teams to be in.
    assert typed(b, "teammsg hello")[1] == [failure(tr("commands.teammsg.failed.noteam"))]
    assert typed(b, "tm hello")[1] == [failure(tr("commands.teammsg.failed.noteam"))]

    # ---- the console: nobody's words, under the name "Server".
    marks = [len(x.chat) for x in (a, b, c)]
    srv.console("say backup in five @a")
    srv.wait_log("[Not Secure] [Server] backup in five Alice, Bob, Carol")
    srv.console("me yawns")
    srv.wait_log("[Not Secure] * Server yawns")
    srv.console("msg Bob you there?")
    srv.wait_log("You whisper to Bob: you there?")
    srv.console("teammsg hi")
    srv.wait_log("An entity is required to run this command here")
    settle(a, b, c)
    backup = disguised("Server", {"text": "backup in five ", "extra": [everyone]}, "say_command")
    yawns = disguised("Server", "yawns", "emote_command")
    assert spoken(a, marks[0]) == [backup, yawns] and spoken(c, marks[2]) == [backup, yawns]
    assert spoken(b, marks[1]) == [backup, yawns, disguised("Server", "you there?", "msg_command_incoming")]

    # ---- /kick's reason is a message too: with names for selectors.
    mark = len(a.chat)
    a.send("chat_command_signed", signed_command("kick Carol enough, says @s"))
    got, r = c.recv()
    assert got == "disconnect" and r.nbt() == {"text": "enough, says ", "extra": [alice]}, got
    c.expect_closed()


# ----------------------------------------------------------------------------- time

CLOCK = {name.split(":")[1]: i for i, name in enumerate(mc.load_registry("world_clock"))}
OVERWORLD, THE_END = CLOCK["overworld"], CLOCK["the_end"]


def suggestions(c: Client, line: str, request: int = 1) -> tuple:
    """command_suggestion for a line as typed (with its slash); the answer: (start, length, texts)."""
    c.send("command_suggestion", varint(request) + mc.mc_str(line))
    r = c.expect("command_suggestions")
    assert r.varint() == request
    start, length = r.varint(), r.varint()
    texts = []
    for _ in range(r.varint()):
        texts.append(r.str())
        assert not r.bool(), "a tooltip"
    r.done()
    return start, length, texts


@test()
def tick_command(srv: Server):
    """/tick: the rate, which everyone is told; a game that stands still - its time, a fuse, what
    falls - while its players go on; steps of it; a sprint, which is so many ticks with no wait
    between them and then the game as it was. What the server suggests for a rate and a time."""
    a, b = srv.client(), srv.client()
    join(a, "Alice")
    join(b, "Bob")
    a.expect("player_info_update")
    for cl in (a, b):
        cl.send("player_loaded")
    settle(a, b)
    assert (a.tick_rate, a.tick_frozen, a.tick_steps) == (float(TICK_RATE), False, 0)
    line = "tick freeze"
    assert run_command(b, line)["said"] == [failure(tr("command.unknown.command")), here(line, 0)]   # an admin's
    make_op(srv, a, "Alice")
    tree = a.commands["children"]["tick"]["children"]
    assert list(tree) == ["query", "rate", "step", "sprint", "unfreeze", "freeze"], list(tree)
    rate = tree["rate"]["children"]["rate"]
    assert rate["properties"] == {"min": 1.0, "max": 10000.0} and rate["suggestions"] == "minecraft:ask_server", rate
    assert list(tree["step"]["children"]) == ["stop", "time"] and tree["step"]["executable"]
    assert list(tree["sprint"]["children"]) == ["stop", "time"] and not tree["sprint"]["executable"]
    for word in ("step", "sprint"):
        time_node = tree[word]["children"]["time"]
        assert time_node["properties"] == {"min": 1} and time_node["suggestions"] == "minecraft:ask_server", time_node

    def tick(line: str) -> list:
        """Run a /tick line as Alice: what she is told. Bob has been told the same of the ticks."""
        said = run_command(a, "tick " + line)["said"]
        settle(b)
        assert (b.tick_rate, b.tick_frozen, b.tick_steps) == (a.tick_rate, a.tick_frozen, a.tick_steps), "Bob was told otherwise"
        return said

    def game_time() -> int:
        (said,) = run_command(a, "time query gametime")["said"]
        return int(said["with"][0])

    # rate: told to everyone, said as Java's "%.1f" says it (a half goes up).
    assert tick("rate 1000.25") == [tr("commands.tick.rate.success", "1000.3")] and a.tick_rate == 1000.25
    assert tick(f"rate {TICK_RATE}") == [tr("commands.tick.rate.success", f"{TICK_RATE}.0")] and a.tick_rate == float(TICK_RATE)
    line = "tick rate 0.5"
    assert tick("rate 0.5") == [failure(tr("argument.float.low", mc.Float(1.0), mc.Float(0.5))), here(line, 10)]
    # A game that runs has nothing to step, and nothing to stop.
    assert tick("step") == [failure(tr("commands.tick.step.fail"))]
    assert tick("step stop") == [failure(tr("commands.tick.step.stop.fail"))]
    assert tick("sprint stop") == [failure(tr("commands.tick.sprint.stop.fail"))]

    # ---- freeze: the game's time stands, a primed TNT hangs where it was with its fuse as it was.
    fuse_of = mc.TNT_DATA["kFuse"]
    run_command(a, "summon tnt ~ ~4 ~3")
    assert tick("freeze") == [tr("commands.tick.status.frozen")] and a.tick_frozen is True
    a.ticks(12)     # (where it was is told every tenth tick of the tracker, which goes on)
    ((tid, tnt),) = of_type(a, "kTnt").items()
    at, fuse, t0 = tnt.pos, tnt.data[fuse_of][1], game_time()
    assert at[1] < -56.0 and fuse < 80, (at, fuse)      # it had begun to fall
    a.ticks(25)
    assert (tnt.pos, tnt.data[fuse_of][1], game_time()) == (at, fuse, t0)
    # ...while a player is no part of what stands still: Alice walks, and Bob sees her.
    a.send("move_player_pos", struct.pack(">dddB", 9.5, SPAWN[1], 8.5, 1))
    settle(a, b)
    assert b.entities[a.entity_id].pos == (9.5, SPAWN[1], 8.5), b.entities[a.entity_id].pos
    # Whoever joins is told that it stands.
    c = srv.client()
    assert join(c, "Carol")["ticking"] == (float(TICK_RATE), True, 0)
    for cl in (a, b):
        cl.expect("player_info_update")
    c.close()
    for cl in (a, b):
        cl.expect("player_info_remove")
    settle(a, b)
    a.mark_chat_read()

    # ---- step: so many ticks all the same - of a frozen game only - and no more.
    assert tick("step 3") == [tr("commands.tick.step.success", 3)] and a.tick_steps == 3
    a.ticks(12)
    assert (game_time(), tnt.data[fuse_of][1]) == (t0 + 3, fuse - 3)
    assert tick("step stop") == [failure(tr("commands.tick.step.stop.fail"))]       # they are done
    assert tick("step") == [tr("commands.tick.step.success", 1)] and a.tick_steps == 1
    a.ticks(3)
    assert game_time() == t0 + 4
    assert tick("step 1d") == [tr("commands.tick.step.success", 24000)] and a.tick_steps == 24000
    assert tick("step stop") == [tr("commands.tick.step.stop.success")] and a.tick_steps == 0
    t1 = game_time()
    assert t0 + 4 < t1 < t0 + 4 + 60, (t0, t1)
    a.ticks(5)
    assert game_time() == t1
    run_command(a, "kill @e[type=tnt]")

    # ---- sprint: the ticks asked for, one after the other without a wait - and one before them
    # at the usual pace, the tick that takes the game out of its freeze - then frozen as before.
    # The server says how fast it was, to its log and to the operators.
    def sprint(line: str) -> tuple:
        """Run a /tick line that ends a sprint, now or soon: what Alice is told besides the
        server's report, and the report - which may come before her answer, or ticks after it."""
        def is_report(m) -> bool:
            return m.get("translate") == "chat.type.admin" and m["with"][0] == "Server" and \
                m["with"][1]["translate"] == "commands.tick.sprint.report"
        said = run_command(a, "tick " + line)["said"]
        end = time.time() + 30
        while not any(is_report(m) for m in said):
            assert time.time() < end, f"the sprint did not end: {said}"
            said += a.recv_pending_chat()
        settle(a, b)
        assert (b.tick_rate, b.tick_frozen, b.tick_steps) == (a.tick_rate, a.tick_frozen, a.tick_steps), "Bob was told otherwise"
        (done,) = [m["with"][1] for m in said if is_report(m)]
        assert len(done["with"]) == 2 and re.fullmatch(r"\d+\.\d\d", done["with"][1]), done
        return [m for m in said if not is_report(m)], done

    said, done = sprint("sprint 200")
    assert said == [tr("commands.tick.status.sprinting")], said
    per_second, ms_per_tick = done["with"]
    assert per_second > 2 * TICK_RATE, done     # not at the pace of the clock
    srv.wait_log(f"Sprint completed with {per_second} ticks per second, or {ms_per_tick} ms per tick")
    assert a.tick_frozen is True
    assert game_time() == t1 + 201
    # A sprint is stopped by hand, or by the next one - after which "before" was a sprint, not a freeze.
    assert tick("sprint 100d") == [tr("commands.tick.status.sprinting")] and a.tick_frozen is False
    assert sprint("sprint stop")[0] == [tr("commands.tick.sprint.stop.success")] and a.tick_frozen is True
    assert tick("sprint 100d") == [tr("commands.tick.status.sprinting")]
    assert sprint("sprint 5")[0] == [tr("commands.tick.sprint.stop.success"), tr("commands.tick.status.sprinting")]
    assert a.tick_frozen is False
    # freeze ends a sprint too.
    assert tick("sprint 100d") == [tr("commands.tick.status.sprinting")]
    assert sprint("freeze")[0] == [tr("commands.tick.status.frozen")] and a.tick_frozen is True
    assert tick("unfreeze") == [tr("commands.tick.status.running")] and a.tick_frozen is False

    # ---- query: what it is doing, the rate with the time a tick takes and may take, and where
    # the last hundred ticks lie.
    said = tick("query")
    assert len(said) == 3 and said[0]["translate"] in ("commands.tick.status.running", "commands.tick.status.lagging"), said
    assert said[1]["translate"] == "commands.tick.query.rate.running" and said[1]["with"][0] == f"{TICK_RATE}.0" and \
        said[1]["with"][2] == f"{1000 / TICK_RATE:.1f}", said
    assert said[2]["translate"] == "commands.tick.query.percentiles" and said[2]["with"][3] == 100 and \
        all(re.fullmatch(r"\d+\.\d", v) for v in said[1]["with"][1:2] + said[2]["with"][:3]), said

    # ---- what the server suggests: TickCommand's own lists, not the units of a time.
    assert suggestions(a, "/tick rate ") == (11, 0, ["20"])
    assert suggestions(a, "/tick step ") == (11, 0, ["1s", "1t", "stop"])
    assert suggestions(a, "/tick step 1") == (11, 1, ["1s", "1t"])
    assert suggestions(a, "/tick sprint ") == (13, 0, ["1d", "3d", "60s", "stop"])
    assert suggestions(b, "/tick rate ") == (0, 0, [])


@test()
def time_of_day(srv: Server):
    """The world's clocks: everyone is told all of them on joining, the game time every twentieth
    tick, and a clock at once when /time changes it - set, moved on to a time marker, added to,
    paused, run faster. What the server suggests for the arguments it is asked about."""
    a, b = srv.client(), srv.client()
    join(a, "Alice")
    join(b, "Bob")
    a.expect("player_info_update")
    for cl in (a, b):
        cl.send("player_loaded")
    settle(a, b)
    # Joining: one packet with both clocks. In a new world they have run as long as the world has.
    first, clocks = a.time_log[0]
    assert clocks == {OVERWORLD: (first, 0.0, 1.0), THE_END: (first, 0.0, 1.0)}, a.time_log[0]
    assert b.time_log[0][1].keys() == {OVERWORLD, THE_END} and b.time_log[0][0] >= first
    # Then, every twentieth tick, the game time and no clock.
    a.ticks(45)
    beats = a.time_log[1:]
    assert len(beats) >= 2 and all(not named for _, named in beats), a.time_log
    assert all(later[0] - earlier[0] == 20 for earlier, later in zip(beats, beats[1:])), beats
    # /time is a gamemaster's.
    line = "time set day"
    assert run_command(b, line)["said"] == [failure(tr("command.unknown.command")), here(line, 0)]
    make_op(srv, a, "Alice")
    tree = a.commands["children"]["time"]["children"]
    assert list(tree) == ["set", "add", "pause", "resume", "rate", "query", "of"], list(tree)
    assert list(tree["set"]["children"]) == ["time", "timemarker"] and list(tree["query"]["children"]) == ["time", "timeline", "gametime"]
    assert tree["set"]["children"]["time"]["properties"] == {"min": 0}
    assert tree["add"]["children"]["time"]["properties"] == {"min": -2147483648}
    assert tree["set"]["children"]["timemarker"]["suggestions"] == "minecraft:ask_server"
    assert tree["query"]["children"]["timeline"]["suggestions"] == "minecraft:ask_server"
    clock = tree["of"]["children"]["clock"]
    assert clock["properties"] == {"registry": "minecraft:world_clock"} and "suggestions" not in clock
    assert list(clock["children"]) == ["set", "add", "pause", "resume", "rate", "query"] and \
        list(clock["children"]["query"]["children"]) == ["time", "timeline"]

    def time(line: str) -> tuple:
        """Run a /time line as Alice: what she is told, and the clocks everyone was told of."""
        marks = len(a.time_log), len(b.time_log)
        said = run_command(a, "time " + line)["said"]
        settle(b)
        told = [named for _, named in a.time_log[marks[0]:] if named]
        assert told == [named for _, named in b.time_log[marks[1]:] if named], "Bob was told otherwise"
        return said, told

    ow, end = "minecraft:overworld", "minecraft:the_end"
    # A clock that stands still, so that it is where it was put: to a client, one with rate 0.
    said, told = time("pause")
    assert said == [tr("commands.time.pause", ow)] and len(told) == 1 and told[0][OVERWORLD][2] == 0.0, (said, told)
    assert time("pause") == ([failure(tr("commands.time.pause.already_paused", ow))], [])
    # set <ticks>: there, whichever way that is.
    assert time("set 1000") == ([tr("commands.time.set.absolute", ow, 1000)], [{OVERWORLD: (1000, 0.0, 0.0)}])
    assert time("set 1000") == ([failure(tr("commands.time.set.already_at_time", ow, 1000))], [])
    assert time("set 50s") == ([failure(tr("commands.time.set.already_at_time", ow, 1000))], [])
    # set <marker>: on to the next time it is that time of day. (The clock is sent whatever comes of it.)
    at = [{OVERWORLD: (1000, 0.0, 0.0)}]
    assert time("set day") == ([failure(tr("commands.time.set.already_at_time_marker", ow, "minecraft:day"))], at)
    assert time("set noon") == ([tr("commands.time.set.time_marker", ow, "minecraft:noon")], [{OVERWORLD: (6000, 0.0, 0.0)}])
    assert time("set minecraft:night") == ([tr("commands.time.set.time_marker", ow, "minecraft:night")], [{OVERWORLD: (13000, 0.0, 0.0)}])
    assert time("set day") == ([tr("commands.time.set.time_marker", ow, "minecraft:day")], [{OVERWORLD: (25000, 0.0, 0.0)}])
    assert time("set teatime") == ([failure(tr("commands.time.no_time_marker_found", "minecraft:teatime", ow))], [{OVERWORLD: (25000, 0.0, 0.0)}])
    # add: by so much, never to before the beginning.
    assert time("add 2s") == ([tr("commands.time.set.absolute", ow, mc.Long(25040))], [{OVERWORLD: (25040, 0.0, 0.0)}])
    assert time("add -1d") == ([tr("commands.time.set.absolute", ow, mc.Long(1040))], [{OVERWORLD: (1040, 0.0, 0.0)}])
    assert time("add -100000") == ([tr("commands.time.set.absolute", ow, mc.Long(0))], [{OVERWORLD: (0, 0.0, 0.0)}])
    assert time("set 3.5d") == ([tr("commands.time.set.absolute", ow, 84000)], [{OVERWORLD: (84000, 0.0, 0.0)}])
    # query: the clock, a timeline of it (where in its period, how many periods), the level's ticks.
    assert time("query time") == ([tr("commands.time.query.absolute", ow, mc.Long(84000))], [])
    assert time("add 6000") == ([tr("commands.time.set.absolute", ow, mc.Long(90000))], [{OVERWORLD: (90000, 0.0, 0.0)}])
    assert time("query day") == ([tr("commands.time.query.timeline", "minecraft:day", mc.Long(18000))], [])
    assert time("query day repetition") == ([tr("commands.time.query.timeline.repetitions", "minecraft:day", mc.Long(3))], [])
    assert time("query minecraft:moon") == ([tr("commands.time.query.timeline", "minecraft:moon", mc.Long(90000))], [])
    assert time("query early_game repetition") == ([tr("commands.time.query.timeline.repetitions", "minecraft:early_game", mc.Long(0))], [])
    said, told = time("query gametime")
    assert told == [] and len(said) == 1 and said[0]["translate"] == "commands.time.query.gametime", said
    # (A set_time of the command's tick, or of one of the three after it, may have come before the
    # answer was read: it tells a time a little later.)
    assert isinstance(said[0]["with"][0], mc.Long) and a.game_time - 4 <= said[0]["with"][0] <= a.game_time + 40, (said, a.game_time)
    line = "time query week"
    assert time("query week")[0] == [failure(tr("argument.resource.not_found", "minecraft:week", "minecraft:timeline")), here(line, 15)]
    # of <clock>: another clock. The end's has run all the while; the day is not its timeline.
    said, told = time("of the_end query time")
    assert said[0]["with"][1] > first and told == []
    assert time("of the_end query day") == ([failure(tr("commands.time.wrong_timeline_for_clock", "minecraft:day", end))], [])
    assert time("of minecraft:the_end set 5") == ([tr("commands.time.set.absolute", end, 5)], [{THE_END: (5, 0.0, 1.0)}])
    assert time("of the_end set noon")[0] == [failure(tr("commands.time.no_time_marker_found", "minecraft:noon", end))]
    assert time("of overworld query time") == ([tr("commands.time.query.absolute", ow, mc.Long(90000))], [])
    # rate: how fast a clock runs - told at once, used once it runs again.
    assert time("rate 2.5") == ([tr("commands.time.rate", ow, mc.Float(2.5))], [{OVERWORLD: (90000, 0.0, 0.0)}])
    assert time("rate 2.5") == ([failure(tr("commands.time.rate.already_same", ow, mc.Float(2.5)))], [])
    line = "time rate 1001"
    assert time("rate 1001")[0] == [failure(tr("argument.float.big", mc.Float(1000.0), mc.Float(1001.0))), here(line, 10)]
    said, told = time("resume")
    assert said == [tr("commands.time.resume", ow)] and told[0][OVERWORLD][2] == 2.5, (said, told)
    assert time("resume") == ([failure(tr("commands.time.pause.already_running", ow))], [])
    a.ticks(21)
    said, told = time("pause")
    ticks, partial, rate = told[0][OVERWORLD]
    assert 90000 + 50 <= ticks <= 90000 + 150 and partial in (0.0, 0.5) and rate == 0.0, told

    # ---- suggestions, for the arguments the tree says to ask the server about.
    assert suggestions(a, "/time set ") == (10, 0, ["minecraft:day", "minecraft:midnight", "minecraft:night", "minecraft:noon"])
    assert suggestions(a, "/time set n") == (10, 1, ["minecraft:night", "minecraft:noon"])
    assert suggestions(a, "/time set mid") == (10, 3, ["minecraft:midnight"])
    assert suggestions(a, "/time set 5") == (11, 0, ["d", "s", "t"])
    assert suggestions(a, "time query ") == (11, 0, ["gametime", "minecraft:day", "minecraft:early_game", "minecraft:moon",
                                                    "minecraft:villager_schedule", "time"])
    assert suggestions(a, "/time of the_end set ") == (0, 0, [])     # the end's clock has no markers
    assert suggestions(a, "/time of the_end query ") == (23, 0, ["time"])
    assert suggestions(a, "/time set x", request=77) == (0, 0, [])
    assert suggestions(b, "/time set ") == (0, 0, [])           # not Bob's command: nothing
    assert suggestions(b, "/t") == (1, 1, ["teammsg", "tell", "tm"])
    # One answer a tick, to the latest question.
    a.send_together(("command_suggestion", varint(1) + mc.mc_str("/time set d")), ("command_suggestion", varint(2) + mc.mc_str("/time set m")))
    r = a.expect("command_suggestions")
    assert r.varint() == 2
    settle(a)

    # ---- someone who joins now is told the clocks as they are.
    c = srv.client()
    join(c, "Carol")
    assert c.time_log[0][1] == {OVERWORLD: (ticks, partial, 0.0), THE_END: (c.time_log[0][1][THE_END][0], 0.0, 1.0)}, c.time_log[0]


@test(advance_time="false")
def time_that_stands_still(srv: Server):
    """advance-time=false (the game rule advance_time): no clock runs; the level's ticks do."""
    a = srv.client()
    join(a, "Alice")
    a.send("player_loaded")
    assert a.time_log[0][1] == {OVERWORLD: (0, 0.0, 0.0), THE_END: (0, 0.0, 0.0)}, a.time_log[0]
    a.ticks(45)
    assert a.game_time >= a.time_log[0][0] + 20 and all(not named for _, named in a.time_log[1:])   # the level's ticks go on
    make_op(srv, a, "Alice")
    assert run_command(a, "time query time")["said"] == [tr("commands.time.query.absolute", "minecraft:overworld", mc.Long(0))]
    mark = len(a.time_log)
    assert run_command(a, "time set 500")["said"] == [tr("commands.time.set.absolute", "minecraft:overworld", 500)]
    assert [named for _, named in a.time_log[mark:] if named] == [{OVERWORLD: (500, 0.0, 0.0)}]
    a.ticks(25)
    assert run_command(a, "time query time")["said"] == [tr("commands.time.query.absolute", "minecraft:overworld", mc.Long(500))]


@test(random_tick_speed=4096, simulation_distance=2)
def grass_sleeps_at_night(srv: Server):
    """Grass spreads where the light above it is 9 or more - the sky's light less what the time
    of day takes from it: none at noon, eleven levels at midnight."""
    a = srv.client()
    join(a, "Alice", view=2)
    a.send("player_loaded")
    settle(a)
    grass, dirt = mc.BLOCK["kGrassBlock"], mc.BLOCK["kDirt"]
    srv.console("time pause")
    srv.console("time set midnight")
    srv.wait_log("to time marker minecraft:midnight")
    settle(a, ticks=3)
    assert a.clocks[OVERWORLD] == (18000, 0.0, 0.0), a.clocks
    # A hole in the grass, at midnight: the dirt at its bottom stays dirt.
    dig(a, (8, -61, 12), seq=1)
    changes = collect(a, ticks=40)
    assert changes == {(8, -61, 12): AIR}, changes
    # Dusk, as long as the sky still gives nine levels: at tick 13014 of the day it is 15 - 6.
    srv.console("time set 13015")
    srv.wait_log("to 13015 tick")
    changes = collect(a, ticks=40)
    assert changes == {}, changes
    srv.console("time set 37014")
    srv.wait_log("to 37014 tick")
    changes = until(a, lambda ch: ch.get((8, -62, 12)) == grass)
    assert changes == {(8, -62, 12): grass}, changes
    # ...and a torch does what the sun does not: light of its own is not the sky's to take.
    mark = len(srv.read_log())     # (it said "midnight" once already)
    srv.console("time set midnight")
    srv.wait_log("to time marker minecraft:midnight", since=mark)
    dig(a, (11, -61, 12), seq=2)
    changes = collect(a, ticks=40)
    assert changes == {(11, -61, 12): AIR}, changes
    give(a, 36, I["kTorch"])
    click(a, (12, -61, 12), 1, seq=3)
    changes = until(a, lambda ch: ch.get((11, -62, 12)) == grass)
    assert changes.get((11, -62, 12)) == grass and dirt not in changes.values(), changes


def join_many(srv: Server, n: int, view: int, online: bool = False) -> list:
    """n bots joined at once, a thread each; returns their clients in order."""
    errors, clients = [], [None] * n

    def one(i):
        try:
            c = srv.client(timeout=60)
            j = join(c, f"Bot{i:03d}", online=online, view=view)
            assert set(j["chunks"]) == mc.view_chunks(*j["center"], j["view"])
            clients[i] = c
        except Exception as e:  # noqa: BLE001
            errors.append(f"Bot{i:03d}: {e!r}")

    threads = [threading.Thread(target=one, args=(i,)) for i in range(n)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert not errors, errors[:5]
    return clients


# (A hundred Python clients: a core of their own, of the eight the pool leaves free. What it asserts
# is what they are shown, not how long the server took.)
@test(max_players=100, view_distance=2, tick_rate=20)
def crowd(srv: Server):
    """60 players on one spot: everyone is shown everyone, and everyone's picture of the others is exact."""
    n = 60
    clients = join_many(srv, n, view=2)
    ids = [c.entity_id for c in clients]
    assert len(set(ids)) == n
    for c in clients:
        c.ignore = {"player_info_update"}  # the tab-list entries of those who joined later
    settle(*clients)
    for c in clients:
        assert set(c.entities) == set(ids) - {c.entity_id}, f"{len(c.entities)} of {n - 1} others"
    # Everyone walks to a place of its own, in a few steps; all stay within sight of each other.
    where = {}
    for step in range(1, 5):
        for i, c in enumerate(clients):
            where[c.entity_id] = (SPAWN[0] - 4.0 + (i % 8) * step * 0.25, SPAWN[1], SPAWN[2] - 4.0 + (i // 8) * step * 0.25)
            c.send("move_player_pos_rot", struct.pack(">dddffB", *where[c.entity_id], float(i * 5 % 360), 0.0, 1))
        settle(*clients, ticks=1)
    settle(*clients)
    for i, c in enumerate(clients):
        for eid, e in c.entities.items():
            assert e.pos == where[eid], f"Bot{i:03d} has entity {eid} at {e.pos}, it is at {where[eid]}"
        assert len(c.entities) == n - 1
    # One leaves: everyone else is told once.
    clients[0].close()
    for c in clients[1:]:
        c.expect("player_info_remove")
        assert set(c.entities) == set(ids[1:]) - {c.entity_id}


def crowd_load(srv: Server, n: int, seconds: int, online: bool) -> None:
    """n players within sight of each other, all walking and turning 20 times a second. Reports
    what a tick costs; three of them check that what they decode is where everyone is."""
    import select
    watchers = 3
    t0 = time.time()
    clients = join_many(srv, n, view=2, online=online)
    print(f"    {n} joins in {time.time() - t0:.1f}s (client-side Python is the bottleneck)")
    ids = [c.entity_id for c in clients]
    assert len(set(ids)) == n and max(ids) == n  # ids are slots: dense, small
    watch, rest = clients[:watchers], clients[watchers:]
    for c in watch:
        c.pump(1.0)
        assert len(c.entities) == n - 1, len(c.entities)

    stop = threading.Event()

    def drain():  # the other 497 only have to keep their sockets empty
        socks = [c.sock for c in rest]
        while not stop.is_set():
            ready, _, _ = select.select(socks, [], [], 0.05)
            for sk in ready:
                try:
                    sk.recv(1 << 18)
                except OSError:
                    pass
    threading.Thread(target=drain, daemon=True).start()

    # Everyone walks a circle of its own inside a 16 x 16 block square: always within 32 blocks of
    # everyone else. Coordinates are multiples of 1/64, so what a watcher decodes must be exact.
    where = {}
    cpu0, t1 = srv.cpu_seconds(), time.time()
    ticks = seconds * 20
    for tick in range(ticks):
        for i, c in enumerate(clients):
            ang = (tick * 9 + i * 37) % 360
            x = SPAWN[0] - 8 + (i % 23) * 0.6875 + round(math.cos(math.radians(ang)) * 64) / 64
            z = SPAWN[2] - 8 + (i // 23) * 0.6875 + round(math.sin(math.radians(ang)) * 64) / 64
            where[c.entity_id] = (x, SPAWN[1], z)
            c.send("move_player_pos_rot", struct.pack(">dddffB", x, SPAWN[1], z, float(ang), 0.0, 1))
        for c in watch:
            c.pump(0.0)
        time.sleep(max(0.0, t1 + (tick + 1) * 0.05 - time.time()))
    wall = time.time() - t1
    cpu = srv.cpu_seconds() - cpu0
    for c in watch:
        c.pump(0.6)
    for c in watch:
        assert len(c.entities) == n - 1
        for eid, e in c.entities.items():
            assert e.pos == where[eid], f"entity {eid} is seen at {e.pos}, it is at {where[eid]}"
    stop.set()
    full = [s for s in tick_stats(srv) if int(s[6]) == n]
    print(f"    {n} players x 20 moves/s for {wall:.1f}s, each seeing {n - 1}, {'encrypted' if online else 'plain'}: "
          f"server used {100 * cpu / wall:.0f}% of one core (all threads)")
    for avg, worst, ent, _, flush, kib, *_ in full[-2:]:
        print(f"    tick thread: {avg} ms avg, {worst} ms worst per tick - {ent} ms the entity tracker, {flush} ms the flush "
              f"(encryption + hand-over); {kib} KiB sent per tick")
    assert full, "no tick statistics in the server log"
    # The average must fit a tick with room to spare. The worst tick is only held to that without
    # encryption: with it, this scenario is bound by AES on the tick thread (see "What a tick of
    # this costs" in docs/architecture.md), and the bots share the machine's cores with it.
    assert all(float(s[0]) < 40.0 for s in full[-2:]), "ticks are taking too long on average"
    assert online or all(float(s[1]) < 50.0 for s in full[-2:]), "a tick took longer than a tick"
    for c in clients:
        c.close()


@test(alone=True, slow=True, max_players=200, view_distance=2, debug="true", tick_rate=20)
def ten_thousand_at_rest(srv: Server):
    """What a tick costs with ten thousand things lying about and nobody there: 150 bots each
    walk a row and drop a stone every block and a half - 68 of them, nothing near enough to
    anything to merge with it - and leave; then as many armor stands are summoned from the
    console. The layout is the one tools/vanilla/physics_probe.py perf gives a vanilla server;
    this prints the server's own account of its ticks, for docs/architecture.md to set the two
    side by side."""
    bots, each = 150, 68
    errors = []

    def listen(c: Client, seconds: float) -> bool:
        """Read and drop what arrives for a while, answering what a client must: a keep-alive,
        and a teleport - true if there was one."""
        asked, until = False, time.time() + seconds
        while True:
            if time.time() >= until:        # (not "until nothing comes": with thousands of items in sight something always does)
                c.sock.settimeout(60)
                return asked
            c.sock.settimeout(max(until - time.time(), 0.001))
            try:
                name, r = c.recv_any()
            except (socket.timeout, BlockingIOError, TimeoutError):
                c.sock.settimeout(60)
                return asked
            if name == "player_position":
                tid, where = r.varint(), (r.f64(), r.f64(), r.f64())
                r.take(24)
                c.send("accept_teleportation", varint(tid) + struct.pack(">dddff", *where, r.f32(), r.f32()))
                asked = True
            elif name == "keep_alive":
                c.send("keep_alive", r.take(8))

    def one(i):
        try:
            c = srv.client(timeout=60)
            join(c, f"Bot{i:03d}", view=2)
            c.send("player_loaded")
            # To the start of a row of its own, well away from where the later bots join (they
            # would pick the items up). A server that still waits for a teleport to be confirmed
            # ignores moves: answer, should it ask again, and go again.
            z = 8.5 + 1.5 * i
            for _ in range(40):
                c.send("move_player_pos_rot", struct.pack(">dddffB", 40.5, SPAWN[1], z, 0.0, DOWN, 1))
                if not listen(c, 0.4):
                    break
            for k in range(each):                # along the row: a step, a stone at its feet
                c.send("move_player_pos", struct.pack(">dddB", 40.5 + 1.5 * k, SPAWN[1], z, 1))
                c.send("set_creative_mode_slot", struct.pack(">h", -1) + mc.item_stack(I["kStone"], 1))
            # ...and away again at once: two seconds on, an item can be picked up, and a slow bot is still there.
            c.send("move_player_pos", struct.pack(">dddB", SPAWN[0], SPAWN[1], SPAWN[2], 1))
            listen(c, 0.3)
            c.close()
        except Exception as e:  # noqa: BLE001
            errors.append(f"Bot{i:03d}: {e!r}")

    for first in range(0, bots, 30):         # thirty at a time: more and the bots starve each other
        threads = [threading.Thread(target=one, args=(i,)) for i in range(first, min(first + 30, bots))]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
    assert not errors, errors[:5]

    def account(what: str, entities: int) -> None:
        time.sleep(16)      # they fall, slide, lie - and three of the server's accounts go by
        # (Give or take a few: two stones that landed within reach of each other are one stack now.)
        quiet = [st for st in tick_stats(srv) if int(st[6]) == 0 and abs(int(st[7]) - entities) <= entities // 100]
        assert quiet, tick_stats(srv)[-3:]
        avg, worst, tracker, ticks, flush, _, _, count, rest = quiet[-1]
        print(f"    {what}: {count} entities, {rest} of them at rest - a tick takes {avg} ms: {tracker} ms the entity tracker, "
              f"{ticks} ms the entities' own ticks ({float(ticks) * 1e6 / int(count):.1f} ns each)")
        assert int(rest) >= int(count) * 0.98 and float(avg) < 5.0, quiet[-1]

    account("items", bots * each)
    for i in range(100):                     # (a hundred lines at a time: the console's ring is not endless)
        for k in range(100):
            srv.console(f"summon armor_stand {40.5 + i * 0.9} -60 {12.5 + k * 0.9}")
        time.sleep(0.06)
    account("items and armor stands", bots * each + 10000)


@test(alone=True, slow=True, max_players=500, view_distance=2, debug="true", tick_rate=20)
def crowd_500(srv: Server):
    """The worst case the server is built for, without encryption (offline mode)."""
    crowd_load(srv, 500, 11, online=False)


@test(alone=True, slow=True)
def crowd_500_online():
    """The same crowd in online mode: every byte of it goes through AES-CFB8 in the flush."""
    mock = MockSessionServer()
    srv = Server(EXE, online_mode="true", session_server=mock.url, max_players=500, view_distance=2, debug="true", tick_rate=20)
    try:
        crowd_load(srv, 500, 11, online=True)
    finally:
        code = srv.stop()
        mock.stop()
        assert code == 0, f"server exit code {code}\n{srv.read_log()[-2000:]}"


# (As crowd: the server's cost is printed, not asserted.)
@test(max_players=400, tick_rate=20)
def many_players(srv: Server):
    n = 200
    errors = []
    clients = [None] * n

    def one(i):
        try:
            c = srv.client(timeout=30)
            j = join(c, f"Bot{i:03d}", view=5)
            assert set(j["chunks"]) == mc.view_chunks(*j["center"], 5)
            clients[i] = c
        except Exception as e:  # noqa: BLE001
            errors.append(f"Bot{i:03d}: {e!r}")

    t0 = time.time()
    threads = [threading.Thread(target=one, args=(i,)) for i in range(n)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert not errors, errors[:5]
    c = srv.client()
    c.handshake(1)
    c.send("status_request")
    assert json.loads(c.expect("status_response").str())["players"]["online"] == n
    print(f"    {n} concurrent joins in {time.time() - t0:.2f}s (client-side Python is the bottleneck)")

    # Steady state: every client moves 20 times a second for a second. Report what that costs the server.
    cpu0, t1 = srv.cpu_seconds(), time.time()
    for tick in range(20):
        for i, cl in enumerate(clients):
            cl.send("move_player_pos_rot", struct.pack(">dddffB", 8.5 + (tick % 5) * 0.1, 64.0, 8.5, float(i), 0.0, 1))
        time.sleep(max(0.0, t1 + (tick + 1) * 0.05 - time.time()))
    wall = time.time() - t1
    print(f"    {n} players x 20 moves/s: server used {100 * (srv.cpu_seconds() - cpu0) / wall:.1f}% of one core")
    for cl in clients:
        cl.close()
    deadline = time.time() + 5
    while True:
        c = srv.client()
        c.handshake(1)
        c.send("status_request")
        online = json.loads(c.expect("status_response").str())["players"]["online"]
        if online == 0:
            break
        assert time.time() < deadline, f"{online} players still online after everyone left"
        time.sleep(0.05)


@test(slow=True)
def keepalive(srv: Server):
    c = srv.client(timeout=20)
    join(c, "Patient")
    t0 = time.time()
    ka = c.expect("keep_alive").i64()
    assert 13 < time.time() - t0 < 17, time.time() - t0
    c.send("keep_alive", struct.pack(">q", ka))
    c.expect("keep_alive")  # still connected one interval later


@test(slow=True)
def keepalive_timeout_kicks(srv: Server):
    c = srv.client(timeout=50)
    join(c, "Silent")
    c.expect("keep_alive")  # never answered
    assert c.expect("disconnect").nbt_text() == "Timed out"
    c.expect_closed()


# ----------------------------------------------------------------------------- runner

EXE = ""


# ----------------------------------------------------------------------------- the world's life cycle

def build_something(a: Client) -> dict:
    """Alice leaves her mark near the spawn: a chest with twenty stone in it, a stone on the
    ground, a glowstone at the edge of the chunk (its light crosses into the next). Returns where."""
    chest, glow = (8, -60, 10), (15, -60, 8)
    give(a, 36, I["kChest"])
    give(a, 37, I["kStone"], 21)
    give(a, 38, I["kGlowstone"])
    click(a, (8, -61, 10), 1, seq=1)
    assert collect(a)[chest] == mc.BLOCK["kChest"]
    click(a, chest, 1, seq=2)
    collect(a)
    assert a.window and a.window["type"] == "kGeneric9x3"
    a.send("set_carried_item", struct.pack(">h", 1))
    menu_click(a, 55, 0, QUICK_MOVE)        # the stone into the chest...
    settle(a)
    assert a.window["slots"][0] == plain("kStone", 21)
    menu_click(a, 0, 1, PICKUP)             # ...and (right click: half, rounded up) eleven of it out again,
    menu_click(a, 55, 1, PICKUP)            # one of them back into the hand
    menu_click(a, 0, 0, PICKUP)             # and the other ten back
    settle(a)
    assert a.window["slots"][0] == plain("kStone", 20) and a.window["slots"][55] == plain("kStone", 1), a.window["slots"][:1]
    close_window(a)
    settle(a)
    a.send("move_player_pos", struct.pack(">dddB", 13.5, SPAWN[1], 8.5, 1))   # within reach of the chunk's edge
    a.send("set_carried_item", struct.pack(">h", 2))
    click(a, (15, -61, 8), 1, seq=3)
    assert collect(a, ticks=6)[glow] == mc.BLOCK["kGlowstone"]
    a.send("set_carried_item", struct.pack(">h", 1))
    look(a, 0.0, DOWN)
    action(a, DROP_ONE)                     # the one stone, at her feet - and away from it before it can be taken again
    settle(a)
    a.send("move_player_pos", struct.pack(">dddB", 9.5, SPAWN[1], 8.5, 1))
    return {"chest": chest, "glow": glow}


def build_more(a: Client) -> dict:
    """...and, after build_something (Alice looks down): a smoker that burns coal, full of beef, and
    a barrel with twenty stone in it. Returns where."""
    smoker, barrel = (10, -60, 10), (11, -60, 7)
    for slot, (item, count) in enumerate((("kSmoker", 1), ("kBeef", 64), ("kCoal", 1), ("kBarrel", 1), ("kStone", 20))):
        give(a, 39 + slot, I[item], count)
    a.send("set_carried_item", struct.pack(">h", 3))
    click(a, (10, -61, 10), 1, seq=4)
    a.send("set_carried_item", struct.pack(">h", 6))
    click(a, (11, -61, 7), 1, seq=5)
    up = mc.BLOCK["kBarrel"] + 8                # its mouth towards Alice: up
    assert shows(collect(a), {smoker: mc.BLOCK["kSmoker"], barrel: up})
    click(a, smoker, 1, seq=6)
    collect(a)
    menu_click(a, 34, 0, QUICK_MOVE)
    menu_click(a, 35, 0, QUICK_MOVE)
    changes = collect(a, ticks=4)
    assert changes[smoker] == mc.BLOCK["kSmoker"] - 1 and a.window["data"][1] == 800 and a.window["data"][3] == 100, a.window["data"]
    close_window(a)
    settle(a)
    click(a, barrel, 1, seq=7)
    collect(a)
    menu_click(a, 61, 0, QUICK_MOVE)
    settle(a)
    assert a.window["slots"][0] == plain("kStone", 20)
    close_window(a)
    assert collect(a)[barrel] == up             # shut again: that is how it is saved
    return {"smoker": smoker, "barrel": barrel}


def near(p: tuple, q: tuple) -> bool:
    """The same place, give or take what a watcher does not know: an item's last slide before it
    lay still reaches a client with its next update, a second later, and rounded to 1 / 4096."""
    return all(abs(u - v) < 0.02 for u, v in zip(p, q))


def frames(c: Client) -> dict:
    """The chunks a client holds, as the bytes they came as."""
    return {pos: c.chunks.raw(pos) for pos in c.chunks}


def the_stone(c: Client):
    lying = list(of_type(c, "kItem").values())
    assert len(lying) == 1, [e.data for e in lying]
    assert lying[0].data[D_ITEM] == (STACK, plain("kStone", 1)), lying[0].data
    return lying[0]


def open_and_check_chest(c: Client, pos: tuple, seq: int) -> None:
    click(c, pos, 1, seq=seq)
    collect(c)
    assert c.window and c.window["type"] == "kGeneric9x3", c.window
    assert c.window["slots"][0] == plain("kStone", 20) and c.window["slots"][1:27] == [None] * 26, c.window["slots"][:3]
    close_window(c)
    settle(c)


def player_file(name: str) -> str:
    """The file an offline player's state is kept in: its UUID, dashed."""
    h = mc.offline_uuid(name)
    return f"{h[:8]}-{h[8:12]}-{h[12:16]}-{h[16:20]}-{h[20:]}.bin"


def as_shown(degrees: float) -> float:
    """An angle as a watcher has it: Mth.packDegrees - 256ths of a turn, rounded down."""
    return (math.floor(degrees * 256 / 360) + 128) % 256 * 360 / 256 - 180


@test()
def a_player_comes_back(srv: Server):
    """Whoever leaves is the same when it is back: where it stood and how it looked about, whether
    it flew, what it had and which slot was in hand, what it wore - which others see again. What
    was on its cursor it has dropped: it lies where it left. And a login from another place gets
    what the first one had at that moment."""
    a, b = srv.client(), srv.client()
    join(a, "Alice")
    jb = join(b, "Bob")
    a.expect("player_info_update")
    for cl in (a, b):
        cl.send("player_loaded")
    settle(a, b)
    sword = (I["kDiamondSword"], 1, {"kDamage": 300})
    give(b, 36, I["kStone"], 64)
    b.send("set_creative_mode_slot", struct.pack(">h", 37) + mc.item_stack(I["kDiamondSword"], 1, {"kDamage": varint(300)}))
    give(b, 20, I["kDirt"], 3)
    give(b, 5, I["kDiamondHelmet"])
    give(b, 45, I["kShield"])
    for slot, stack in {36: plain("kStone", 64), 37: sword, 20: plain("kDirt", 3), 5: plain("kDiamondHelmet"), 45: plain("kShield")}.items():
        b.slots[slot] = stack              # a client knows what it put there
    b.send("set_carried_item", struct.pack(">h", 1))
    b.send("player_abilities", b"\x02")    # flying
    where, looks = (3.25, -55.5, 12.75), (123.5, -30.25)
    b.send("move_player_pos_rot", struct.pack(">dddffB", *where, *looks, 0))
    settle(a, b)
    # The dirt onto the cursor: it is neither in the inventory nor saved - it falls as Bob goes.
    menu_click(b, 20, 0, PICKUP)
    settle(a, b, ticks=2)
    assert b.cursor == plain("kDirt", 3) and b.slots[20] is None
    had = list(b.slots)
    assert not of_type(a, "kItem")
    b.close()
    a.expect("player_info_remove")
    settle(a, ticks=6)
    dropped = list(of_type(a, "kItem").values())
    assert [e.data[D_ITEM] for e in dropped] == [(STACK, plain("kDirt", 3))], [e.data for e in dropped]
    assert not of_type(a, "kPlayer")

    # ---- back again, in the same run of the server.
    b = srv.client()
    jb = join(b, "Bob")
    a.expect("player_info_update")
    b.send("player_loaded")
    settle(a, b)
    assert jb["teleport"]["pos"] == where and jb["teleport"]["rot"] == looks, jb["teleport"]
    assert jb["teleport"]["id"] == 1 and jb["teleport"]["relative"] == 0
    assert jb["abilities"][0] == 0x01 | 0x02 | 0x04 | 0x08, jb["abilities"]     # ...and flying
    assert jb["center"] == (0, 0)
    assert b.menu_log[:2] == [("set_held_slot", 1), ("container_set_content", 1)], b.menu_log[:2]
    assert b.held_slot == 1 and b.cursor is None
    assert b.slots == had, (b.slots, had)
    assert server_menu(b) == had
    # Alice is shown him where he is, with what he holds and wears, in the bundle he comes in.
    seen = a.entities[jb["login"]["entity_id"]]
    assert seen.pos == where and (seen.yaw, seen.pitch) == (as_shown(looks[0]), as_shown(looks[1])), (seen.pos, seen.yaw, seen.pitch)
    assert seen.equipment == {MAINHAND: sword, OFFHAND: plain("kShield"), HEAD: plain("kDiamondHelmet")} and seen.bundled, seen.equipment

    # ---- a second login under the same name: the first is put out, and the second has what the
    # first had when that happened - not what was last written.
    give(b, 38, I["kGlowstone"], 7)
    b.slots[38] = plain("kGlowstone", 7)
    there = (5.5, -60.0, 2.5)
    b.send("move_player_pos_rot", struct.pack(">dddffB", *there, 10.0, 20.0, 1))
    b.send("player_abilities", b"\x00")
    settle(a, b)
    had = list(b.slots)
    b2 = srv.client()
    jb2 = join(b2, "Bob")
    expect_kick(b, "a second login under the same name")
    a.expect("player_info_remove")
    a.expect("player_info_update")
    assert jb2["teleport"]["pos"] == there and jb2["teleport"]["rot"] == (10.0, 20.0), jb2["teleport"]
    assert jb2["abilities"][0] == 0x01 | 0x04 | 0x08, jb2["abilities"]
    assert b2.slots == had and b2.held_slot == 1
    # A newcomer is new: the world's spawn, empty hands.
    c = srv.client()
    check_join(join(c, "Carol"), "Carol")
    assert c.slots == [None] * 46
    # Bob's file is there; Alice's and Carol's come when they leave, or with the next autosave.
    assert os.listdir(os.path.join(srv.world, "playerstate")) == [player_file("Bob")]


@test()
def block_entity_tags():
    """A block entity out of a tag: /setblock and /fill with {...}, a filter that asks for one; a
    chest's name on its menu, its lock; a chest placed from an item that carries its contents - and
    all of it there when the server starts again. (What a vanilla server makes of the same tags is
    compared by tools/vanilla/command_probe.py.)"""
    world = tempfile.mkdtemp(prefix="world-tags-")
    srv = None
    try:
        srv = Server(EXE, world=world)
        a = srv.client()
        join(a, "Alice", view=3)
        a.send("player_loaded")
        settle(a)
        make_op(srv, a, "Alice")
        chest, placed = (8, -60, 12), (8, -60, 10)

        def data(pos: tuple, path: str) -> str:
            said = run_command(a, "data get block %d %d %d %s" % (*pos, path))["said"]
            assert len(said) == 1 and "with" in said[0], said
            return shown(said[0]["with"][-1])

        def held(pos: tuple) -> dict:
            return {"Items": data(pos, "Items"), "CustomName": data(pos, "CustomName"), "components": data(pos, "components")}

        # ---- /setblock with a tag. What is wrong in it is left out as vanilla leaves it out: a slot
        # that is none (300 is the byte 44), an item that is none, an entry no chest has.
        line = ('setblock 8 -60 12 chest{CustomName:"Loot",lock:{items:"stick"},foo:1b,components:{rarity:"rare",nope:1},Items:['
                '{Slot:3b,id:"stone",count:5},{Slot:300,id:"dirt"},{Slot:1b,id:"diamond_sword",components:{damage:7}},{Slot:2b,id:"nope"}]}')
        assert run_command(a, line)["said"] == [tr("commands.setblock.success", *chest)]
        loot = {"Items": '[{components: {"minecraft:damage": 7}, count: 1, Slot: 1b, id: "minecraft:diamond_sword"}, '
                         '{count: 5, Slot: 3b, id: "minecraft:stone"}]',
                "CustomName": '"Loot"', "components": '{"minecraft:rarity": "rare"}'}
        assert held(chest) == loot, held(chest)
        assert data(chest, "lock") == '{items: "minecraft:stick"}'
        assert run_command(a, "data get block 8 -60 12 foo")["said"] == [failure(tr("arguments.nbtpath.nothing_found", "foo"))]
        # The same again changes nothing, and that is a failure; a tag that is not loaded is said to be, before a block is touched.
        assert run_command(a, line)["said"] == [failure(tr("commands.setblock.failed"))]
        got = run_command(a, 'setblock 8 -60 14 chest{LootTable:"chests/simple_dungeon"}')
        assert got["said"] == [failure("Loot tables of containers are not supported by this server yet")] and not got["blocks"], got
        got = run_command(a, 'setblock 8 -60 14 chest{Items:[{id:"apple",components:{food:{nutrition:1,saturation:1}}}]}')
        assert got["said"] == [failure("Items with components this server does not read are not supported by this server yet")], got

        # ---- its lock, and its name. An empty hand does not open it: a line over the hotbar, a rattle.
        mark = len(a.chat)
        click(a, chest, 1, seq=1)
        collect(a)
        assert a.window is None and a.chat[mark:] == [("system", tr("container.isLocked", "Loot"), True)], a.chat[mark:]
        assert a.sounds[-1] == (mc.SOUND["kBlockChestLocked"], 4, (8.5, -59.5, 12.5), 1.0, 1.0), a.sounds[-1]
        a.mark_chat_read()
        give(a, 36, I["kStick"], 1)
        click(a, chest, 1, seq=2)
        collect(a)
        w = a.window
        assert w and (w["type"], w["title"]) == ("kGeneric9x3", "Loot"), w
        assert w["slots"][1] == (I["kDiamondSword"], 1, {"kDamage": 7}) and w["slots"][3] == plain("kStone", 5)
        assert [i for i, st in enumerate(w["slots"][:27]) if st] == [1, 3]
        close_window(a)
        collect(a)

        # ---- /fill with a tag, and a filter that asks for one (a block entity that saves as at least that).
        assert run_command(a, 'fill 10 -60 12 12 -60 12 barrel{Items:[{Slot:0b,id:"apple"}]}')["said"] == [tr("commands.fill.success", 3)]
        assert data((11, -60, 12), "Items") == '[{count: 1, Slot: 0b, id: "minecraft:apple"}]'
        # (A barrel set where one stands is the block it was, and holds what it held: nothing changed.)
        assert run_command(a, "setblock 11 -60 12 barrel")["said"] == [failure(tr("commands.setblock.failed"))]
        run_command(a, "setblock 11 -60 12 air")
        run_command(a, "setblock 11 -60 12 barrel")
        got = run_command(a, 'fill 10 -60 12 12 -60 12 stone replace barrel{Items:[{id:"minecraft:apple"}]}')
        assert got["said"] == [tr("commands.fill.success", 2)] and sorted(got["blocks"]) == [(10, -60, 12), (12, -60, 12)], got

        # ---- a chest out of an item that carries one: what it holds, its name, and what else its
        # stack says, which becomes the block entity's components.
        template = varint(I["kStone"]) + varint(5) + varint(0) + varint(0)
        a.send("set_creative_mode_slot", struct.pack(">h", 36) + mc.item_stack(I["kChest"], 1, {
            "kContainer": varint(3) + b"\x00\x00\x01" + template, "kCustomName": mc.nbt("Box"), "kRarity": varint(2)}))
        settle(a)
        click(a, (8, -61, 10), 1, seq=3)
        collect(a)
        box = {"Items": '[{count: 5, Slot: 2b, id: "minecraft:stone"}]', "CustomName": '"Box"',
               "components": '{"minecraft:rarity": "rare"}'}
        assert held(placed) == box, held(placed)
        # ...and one whose item names a loot table is not placed.
        a.send("set_creative_mode_slot", struct.pack(">h", 36) + mc.item_stack(I["kChest"], 1, {
            "kContainerLoot": mc.nbt({"loot_table": "minecraft:chests/simple_dungeon"})}))
        settle(a)
        click(a, (10, -61, 10), 1, seq=4)
        collect(a)
        assert run_command(a, "data get block 10 -60 10")["said"] == [failure(tr("commands.data.block.invalid"))]

        # ---- and all of it is there when the server is.
        assert srv.stop() == 0, srv.read_log()
        os.unlink(srv.log.name)
        srv = Server(EXE, world=world)
        a = srv.client()
        join(a, "Alice", view=3)
        a.send("player_loaded")
        settle(a, ticks=6)
        make_op(srv, a, "Alice")
        assert held(chest) == loot and data(chest, "lock") == '{items: "minecraft:stick"}', held(chest)
        assert held(placed) == box, held(placed)
        assert srv.stop() == 0, srv.read_log()
        srv = None
    finally:
        if srv:
            srv.stop()
        shutil.rmtree(world, ignore_errors=True)


@test()
def signs():
    """Signs: standing and on a wall and hanging, turned to whoever places them; the editor that opens,
    and whose lines are taken; dye, glow ink and wax; what holds them; a sign out of a tag - and its
    text there for whoever comes later, in the chunk's own packet, also after the server was stopped.
    (Where a vanilla server puts a sign and what it sends is compared by tools/vanilla/command_probe.py.)"""
    world = tempfile.mkdtemp(prefix="world-signs-")
    srv = None
    try:
        srv = Server(EXE, world=world)
        a, b = srv.client(), srv.client()
        join(a, "Alice", view=3)
        join(b, "Bob", view=3)
        a.expect("player_info_update")
        for cl in (a, b):
            cl.send("player_loaded")
        settle(a, b)
        make_op(srv, a, "Alice")
        SIGN = mc.BLOCK_ENTITY["kSign"]
        pos = (8, -60, 10)

        def write(c: Client, lines: list, front: bool = True, at: tuple = pos) -> None:
            c.send("sign_update", mc.pack_block_pos(*at) + b"".join(mc.mc_str(line) for line in lines) + varint(1 if front else 0))

        def shown(c: Client, at: tuple = pos, of: int = SIGN) -> dict:
            """What the client was last told the sign says: its update tag."""
            told = [tag for where, kind, tag in c.block_entity_data if where == at and kind == of]
            assert told, f"nothing was told about the sign at {at}"
            return told[-1]

        # ---- placing: it stands turned to Alice, who looks south, and her editor opens on its front.
        give(a, 36, I["kOakSign"], 8)
        click(a, (8, -61, 10), 1, seq=1)
        assert collect(a)[pos] == mc.BLOCK["kOakSign"]         # rotation 8: facing north
        assert collect(b)[pos] == mc.BLOCK["kOakSign"]
        assert a.sign_editor == (pos, True) and b.sign_editor is None
        empty = ["", "", "", ""]
        for cl in (a, b):
            assert shown(cl)["front_text"]["messages"] == empty and shown(cl)["back_text"]["messages"] == empty, shown(cl)
            assert shown(cl)["is_waxed"] == 0
        # Only whoever was sent the editor may write - once.
        told = len(b.block_entity_data)
        write(b, ["Bob", "was", "here", ""])
        settle(a, b)
        assert len(b.block_entity_data) == told
        write(a, ["Hello", "\u00a7cred?", "", "x" * 384])
        collect(a), collect(b)
        for cl in (a, b):
            assert shown(cl)["front_text"]["messages"] == ["Hello", "red?", "", "x" * 384], shown(cl)["front_text"]
        told = len(b.block_entity_data)
        write(a, ["again", "", "", ""])
        settle(a, b)
        assert len(b.block_entity_data) == told

        # ---- a click with an empty hand opens the editor of the side one stands on; while Bob has it, Alice gets none.
        a.sign_editor = None
        click(b, pos, 2, seq=1)
        collect(b)
        assert b.sign_editor == (pos, True)
        give(a, 36, 0, 0)
        click(a, pos, 2, seq=2)
        collect(a)
        assert a.sign_editor is None
        write(b, ["Hello", "from", "Bob", ""])
        collect(a), collect(b)
        assert shown(a)["front_text"]["messages"] == ["Hello", "from", "Bob", ""]

        # ---- dye, glow ink, wax.
        give(a, 36, I["kRedDye"], 1)
        click(a, pos, 2, seq=3)
        collect(a), collect(b)
        assert shown(b)["front_text"]["color"] == "red" and a.sounds[-1][0] == mc.SOUND["kItemDyeUse"], shown(b)["front_text"]
        give(a, 36, I["kGlowInkSac"], 1)
        click(a, pos, 2, seq=4)
        collect(a), collect(b)
        assert shown(b)["front_text"]["has_glowing_text"] == 1 and a.sounds[-1][0] == mc.SOUND["kItemGlowInkSacUse"]
        give(a, 36, I["kHoneycomb"], 1)
        click(a, pos, 2, seq=5)
        collect(a), collect(b)
        assert shown(b)["is_waxed"] == 1 and a.sounds[-1][0] == mc.SOUND["kItemHoneycombWaxOn"]
        assert b.level_events[-1][:2] == (3003, pos), b.level_events[-1]
        # Waxed: nothing opens it any more, and it says so with a sound.
        give(a, 36, 0, 0)
        a.sign_editor = None
        click(a, pos, 2, seq=6)
        collect(a)
        assert a.sign_editor is None and a.sounds[-1][0] == mc.SOUND["kBlockSignWaxedInteractFail"]

        # ---- on a wall: against the side of a block, facing away from it.
        run_command(a, "setblock 8 -60 13 stone")
        give(a, 36, I["kOakSign"], 8)
        click(a, (8, -60, 13), 2, seq=7)
        assert collect(a)[(8, -60, 12)] == mc.BLOCK["kOakWallSign"]     # facing north
        assert a.sign_editor == ((8, -60, 12), True)
        # ...and hanging: under a block, by both chains, across the way Alice looks.
        run_command(a, "setblock 10 -58 10 stone")
        give(a, 36, I["kOakHangingSign"], 8)
        click(a, (10, -58, 10), 0, seq=8)
        assert collect(a)[(10, -59, 10)] == mc.BLOCK["kOakHangingSign"]
        assert shown(a, (10, -59, 10), mc.BLOCK_ENTITY["kHangingSign"])["front_text"]["messages"] == empty

        # ---- what holds a sign: without it the sign breaks.
        got = run_command(a, "setblock 8 -60 13 air")
        assert got["blocks"][(8, -60, 12)] == 0, got["blocks"]
        got = run_command(a, "setblock 10 -58 10 air")
        assert got["blocks"][(10, -59, 10)] == 0, got["blocks"]

        # ---- a sign out of a tag, and what it saves as.
        line = ('setblock 8 -60 14 oak_sign{front_text:{messages:["a",{text:"b",color:"red"},"",""],color:"blue"},'
                'is_waxed:1b,back_text:{messages:["only three","",""]}}')
        assert run_command(a, line)["said"] == [tr("commands.setblock.success", 8, -60, 14)]
        tagged = shown(a, (8, -60, 14))
        assert tagged["front_text"] == {"messages": ["a", {"color": "red", "text": "b"}, "", ""], "color": "blue",
                                        "has_glowing_text": 0}, tagged
        assert tagged["back_text"]["messages"] == empty and tagged["is_waxed"] == 1     # (a text of three lines is no text)

        def data(path: str) -> str:
            said = run_command(a, "data get block 8 -60 14 " + path)["said"]
            assert len(said) == 1 and "with" in said[0], said
            return said[0]["with"][-1]

        assert plain_text(data("front_text.messages[1].color")) == '"red"' and plain_text(data("is_waxed")) == "1b"

        # ---- whoever comes later reads it in the chunk's own packet.
        def in_chunk(c: Client) -> dict:
            return {where: (kind, tag) for where, kind, tag in c.chunks[(0, 0)]["block_entities"]}

        c = srv.client()
        join(c, "Carol", view=3)
        c.send("player_loaded")
        settle(c)
        seen = in_chunk(c)
        assert seen[pos][0] == SIGN and seen[pos][1]["front_text"]["messages"] == ["Hello", "from", "Bob", ""], seen[pos]
        assert seen[pos][1]["front_text"]["color"] == "red" and seen[pos][1]["is_waxed"] == 1
        assert seen[(8, -60, 14)][1] == tagged
        c.close()

        # ---- and after the server was stopped and started.
        assert srv.stop() == 0, srv.read_log()
        os.unlink(srv.log.name)
        srv = Server(EXE, world=world)
        c = srv.client()
        join(c, "Carol", view=3)
        c.send("player_loaded")
        settle(c, ticks=6)
        assert in_chunk(c) == seen, in_chunk(c)
        assert srv.stop() == 0, srv.read_log()
        srv = None
    finally:
        if srv:
            srv.stop()
        shutil.rmtree(world, ignore_errors=True)


@test()
def entity_tags():
    """An entity out of a tag: /summon with {...} - a name over a pig, whether it is heard and thinks,
    an armor stand's pose and what it wears, an item that is something, a fuse - what clients are
    shown of it, what it saves as, and all of it there when the server starts again. (What a vanilla
    server makes of the same tags is compared by tools/vanilla/command_probe.py.)"""
    world = tempfile.mkdtemp(prefix="world-entities-")
    srv = None
    MOB, STAND = mc._constants("entities.hpp", "mob"), mc._constants("entities.hpp", "armor_stand")
    NAME, VISIBLE, SILENT = mc.DATA["kCustomName"], mc.DATA["kCustomNameVisible"], mc.DATA["kSilent"]
    OPTIONAL_TEXT, BOOL, ROTATIONS = mc.SERIALIZER["kOptionalComponent"], mc.SERIALIZER["kBoolean"], mc.SERIALIZER["kRotations"]
    try:
        srv = Server(EXE, world=world)
        a = srv.client()
        join(a, "Alice", view=3)
        a.send("player_loaded")
        settle(a)
        make_op(srv, a, "Alice")

        def data(selector: str, path: str) -> str:
            said = run_command(a, f"data get entity {selector} {path}")["said"]
            assert len(said) == 1 and "with" in said[0], said
            return shown(said[0]["with"][-1])

        def looks(c: Client) -> None:
            pig = one(of_type(c, "kPig"))
            assert pig.data[NAME] == (OPTIONAL_TEXT, "Bob") and pig.data[VISIBLE] == (BOOL, True), pig.data
            assert pig.data[SILENT] == (BOOL, True) and pig.data[D_FLAGS] == (BYTE, 0x41), pig.data      # glowing, and on fire
            assert pig.data[MOB["kMobFlags"]] == (BYTE, 1), pig.data                                        # NoAI
            stand = one(of_type(c, "kArmorStand"))
            assert stand.data[STAND["kClientFlags"]] == (BYTE, 4 | 8) and stand.data[D_FLAGS] == (BYTE, 0x20), stand.data
            assert stand.data[STAND["kHeadPose"]] == (ROTATIONS, (10.0, 20.0, 30.0)), stand.data
            assert STAND["kBodyPose"] not in stand.data
            assert stand.equipment == {MAINHAND: plain("kStone", 3), HEAD: (I["kDiamondHelmet"], 1, {"kDamage": 5})}, stand.equipment

        def one(found: dict):
            assert len(found) == 1, found
            return next(iter(found.values()))

        # ---- a pig with a name; an armor stand that was posed and dressed.
        got = run_command(a, 'summon pig 8.5 -60 12.5 {CustomName:"Bob",CustomNameVisible:1b,NoAI:1b,Silent:1b,Glowing:1b,'
                             'Tags:["x","y","x"],Rotation:[90f,0f],data:{from:"a test"},foo:1b,Fire:20000s}')
        assert len(got["said"]) == 1 and got["said"][0]["translate"] == "commands.summon.success", got
        assert shown(got["said"][0]["with"][0]) == "Bob"                    # it is called by its name
        run_command(a, 'summon armor_stand 10.5 -60 12.5 {ShowArms:1b,NoBasePlate:1b,Invisible:1b,Pose:{Head:[10f,20f,30f],'
                       'Body:[0f,0f,0f]},equipment:{head:{id:"diamond_helmet",components:{damage:5}},mainhand:{id:"stone",count:3}}}')
        settle(a)
        looks(a)
        assert data("@e[type=pig,limit=1]", "CustomName") == '"Bob"' and data("@e[type=pig,limit=1]", "Tags") in ('["x", "y"]', '["y", "x"]')
        assert data("@e[type=pig,limit=1]", "NoAI") == "1b" and data("@e[type=pig,limit=1]", "data") == '{from: "a test"}'
        burning = int(data("@e[type=pig,limit=1]", "Fire")[:-1])            # (it burns down a tick at a time, and nothing hurts it)
        assert 19000 < burning < 20000, burning
        assert data("@e[tag=y,limit=1]", "Rotation") == "[90.0f, 0.0f]"              # ...and found by its tag, and by its name
        assert data("@e[name=Bob,limit=1]", "Silent") == "1b"
        assert data("@e[type=armor_stand,limit=1]", "Pose") == "{Head: [10.0f, 20.0f, 30.0f]}"
        assert data("@e[type=armor_stand,limit=1]", "equipment.head.components") == '{"minecraft:damage": 5}'
        assert run_command(a, "data get entity @e[tag=nope,limit=1]")["said"] == [failure(tr("argument.entity.notfound.entity"))]

        # ---- what this server cannot make is refused, and nothing appears.
        before = len(a.entities)
        got = run_command(a, "summon cow 8.5 -60 14.5 {Age:-100}")
        assert got["said"] == [failure("Mobs that are not grown up are not supported by this server yet")], got
        got = run_command(a, "summon armor_stand 8.5 -60 14.5 {Small:1b}")
        assert got["said"] == [failure("Armor stands that are small or markers are not supported by this server yet")], got
        got = run_command(a, 'summon pig 8.5 -60 14.5 {CustomName:{selector:"@a"}}')
        assert got["said"] == [failure("Names with parts of a text this server does not read are not supported by this server yet")], got
        settle(a)
        assert len(a.entities) == before

        # ---- an item that is something, and that nobody picks up; a fuse; a block that falls.
        run_command(a, 'summon item 8.5 -60 15.5 {Item:{id:"diamond",count:3,components:{rarity:"epic"}},PickupDelay:32767}')
        run_command(a, "summon tnt 12.5 -58 15.5 {fuse:2000s,NoGravity:1b}")
        run_command(a, "summon cow 6.5 -58 12.5 {NoGravity:1b}")     # what moves, told not to fall: it stays in the air
        settle(a, ticks=6)
        assert one(of_type(a, "kTnt")).pos == (12.5, -58.0, 15.5) and one(of_type(a, "kCow")).pos == (6.5, -58.0, 12.5)
        item = one(of_type(a, "kItem"))
        assert item.data[D_ITEM][1] == (I["kDiamond"], 3, {"kRarity": 3}), item.data
        assert a.slots == [None] * 46                                       # never picked up
        tnt = one(of_type(a, "kTnt"))
        assert 1900 < tnt.data[mc._constants("entities.hpp", "primed_tnt")["kFuse"]][1] <= 2000, tnt.data
        run_command(a, "kill @e[type=tnt]")

        # ---- and all of it is there when the server is.
        assert srv.stop() == 0, srv.read_log()
        os.unlink(srv.log.name)
        srv = Server(EXE, world=world)
        c = srv.client()
        join(c, "Carol", view=3)
        c.send("player_loaded")
        settle(c, ticks=6)
        looks(c)
        make_op(srv, c, "Carol")
        said = run_command(c, "data get entity @e[type=pig,limit=1] Fire")["said"]
        assert 18000 < int(shown(said[0]["with"][-1])[:-1]) < burning, said    # the fire it was on, burnt further down
        assert one(of_type(c, "kItem")).data[D_ITEM][1] == (I["kDiamond"], 3, {"kRarity": 3})
        assert one(of_type(c, "kCow")).pos == (6.5, -58.0, 12.5)   # ...and still does
        assert srv.stop() == 0, srv.read_log()
        srv = None
    finally:
        if srv:
            srv.stop()
        shutil.rmtree(world, ignore_errors=True)


@test()
def spawn_eggs(srv: Server):
    """A spawn egg on a block makes what its entity_data names, where vanilla puts it: on top of the
    block that was clicked, beside it, into a plant - coming down on a slab, on a fence - looking a
    way of its own, with the stack's name and custom data and the egg's own tag on it; an egg of
    nothing, or of what this server cannot make, makes nothing; a falling block's tag is an
    operator's to give. use_item is acknowledged and turns whoever holds something. (What a vanilla
    server makes of the same eggs is compared by tools/vanilla/command_probe.py, section eggs.)"""
    a, b = srv.client(), srv.client()
    join(a, "Alice", view=3)
    join(b, "Bob", view=3)
    a.expect("player_info_update")
    for cl in (a, b):
        cl.send("player_loaded")
    settle(a, b)
    make_op(srv, a, "Alice")
    NAME, OPTIONAL_TEXT = mc.DATA["kCustomName"], mc.SERIALIZER["kOptionalComponent"]
    MOB = mc._constants("entities.hpp", "mob")
    seq = [0]

    def egg(line: str | None = None) -> None:
        """In Alice's hand: a pig's egg, or what /give makes of `line`."""
        give(a, 36, 0, 0)
        if line is None:
            give(a, 36, I["kPigSpawnEgg"], 1)
        else:
            run_command(a, f"give @s {line}")

    def use(c: Client, pos: tuple, face: int, cursor: tuple = (0.5, 1.0, 0.5)) -> dict:
        """`c` clicks a face of a block: what Bob was shown appear meanwhile, {entity id: Entity}."""
        collect(b)  # what Bob was told before, taken first: a /kill frees ids, which the next entity gets
        before = set(b.entities)
        seq[0] += 1
        click(c, pos, face, seq=seq[0], cursor=cursor)
        collect(c)
        if c is not b:
            collect(b)
        return {i: e for i, e in b.entities.items() if i not in before}

    def one(found: dict):
        assert len(found) == 1, found
        return next(iter(found.values()))

    def data(selector: str, path: str) -> str:
        said = run_command(a, f"data get entity {selector} {path}")["said"]
        assert len(said) == 1 and "with" in said[0], said
        return shown(said[0]["with"][-1])

    def acks(c: Client, ticks: int = 2) -> list:
        """Let `ticks` server ticks pass: the block_changed_acks they brought."""
        out = []
        for i in range(ticks):
            token = 0xACC000 + i
            c.send("ping_request", struct.pack(">q", token))
            while True:
                name, r = c.recv()
                if name == "pong_response":
                    assert r.i64() == token
                    break
                if name == "block_changed_ack":
                    out.append(r.varint())
                else:
                    assert name in ("block_update", "section_blocks_update", "light_update", "player_info_update"), f"unexpected {name}"
        return out

    # ---- on top of the grass, south of Alice: in the middle of the block above, looking a way
    # of its own with its head the same way - and Bob sees her arm swing.
    egg()
    mark = len(b.entity_log)
    pig = one(use(a, (8, -61, 11), 1))
    assert pig.type == mc.ENTITY_TYPE["kPig"] and pig.spawned[0] == (8.5, -60.0, 11.5), (pig.type, pig.spawned)
    # (The head as an angle packed into a byte, Mth.packDegrees; the yaw may have come in full since.)
    assert pig.pitch == 0 and pig.head == math.floor(pig.yaw * 256 / 360), (pig.yaw, pig.head)
    assert any(n == "swing_animation" and eid == a.entity_id for n, eid, _ in b.entity_log[mark:]), b.entity_log[mark:]
    heads = [pig.head]
    # ...against the side of a stone: beside it, on the ground.
    run_command(a, "setblock 11 -60 11 stone")
    pig = one(use(a, (11, -60, 11), 4, cursor=(0.0, 0.5, 0.5)))
    assert pig.spawned[0] == (10.5, -60.0, 11.5), pig.spawned
    heads.append(pig.head)
    # ...on top of a bottom slab and of a fence: it comes down on them.
    run_command(a, "setblock 8 -60 13 stone_slab")
    pig = one(use(a, (8, -60, 13), 1))
    assert pig.spawned[0] == (8.5, -59.5, 13.5), pig.spawned
    heads.append(pig.head)
    run_command(a, "setblock 6 -60 13 oak_fence")
    pig = one(use(a, (6, -60, 13), 1))
    assert pig.spawned[0] == (6.5, -58.5, 13.5), pig.spawned
    heads.append(pig.head)
    # ...into a plant, which stops nothing: where the plant is.
    run_command(a, "setblock 10 -60 13 short_grass")
    pig = one(use(a, (10, -60, 13), 1, cursor=(0.5, 0.5, 0.5)))
    assert pig.spawned[0] == (10.5, -60.0, 13.5), pig.spawned
    heads.append(pig.head)
    assert len(set(heads)) > 1, heads     # a way drawn at random each time
    run_command(a, "kill @e[type=pig]")

    # ---- what the stack says: a name, and custom data, which become the pig's.
    egg('pig_spawn_egg[custom_name="Bob",custom_data={a:1}]')
    pig = one(use(a, (8, -61, 11), 1))
    assert pig.data[NAME] == (OPTIONAL_TEXT, "Bob"), pig.data
    assert data("@e[type=pig,limit=1]", "data") == "{a: 1}"
    run_command(a, "kill @e[type=pig]")
    # ...and the egg's own tag over that: the data merged, its rotation instead of a random one.
    egg('pig_spawn_egg[custom_name="Bob",custom_data={a:1},entity_data={id:"minecraft:pig",NoAI:1b,Rotation:[90f,0f],'
        'data:{b:2}}]')
    pig = one(use(a, (8, -61, 11), 1))
    assert pig.yaw == 90.0 and pig.head == 64, (pig.yaw, pig.head)
    assert pig.data[NAME] == (OPTIONAL_TEXT, "Bob") and pig.data[MOB["kMobFlags"]] == (BYTE, 1), pig.data   # NoAI
    assert data("@e[type=pig,limit=1]", "data") == "{a: 1, b: 2}"
    run_command(a, "kill @e[type=pig]")
    # ...where it puts it: Pos.
    egg('pig_spawn_egg[entity_data={id:"minecraft:pig",Pos:[12.5d,-60d,6.5d]}]')
    assert one(use(a, (8, -61, 11), 1)).spawned[0] == (12.5, -60.0, 6.5)
    run_command(a, "kill @e[type=pig]")
    # ...another type.
    egg('pig_spawn_egg[entity_data={id:"minecraft:cow"}]')
    assert one(use(a, (8, -61, 11), 1)).type == mc.ENTITY_TYPE["kCow"]
    run_command(a, "kill @e[type=cow]")
    # ...none at all, a baby (another body to the physics), no place: nothing, and no swing.
    for line in ("pig_spawn_egg[!entity_data]", 'pig_spawn_egg[entity_data={id:"minecraft:pig",Age:-100}]',
                 'pig_spawn_egg[entity_data={id:"minecraft:pig",Pos:[1d,2d]}]'):
        egg(line)
        mark = len(b.entity_log)
        assert use(a, (8, -61, 11), 1) == {}, line
        assert not any(n == "swing_animation" for n, _, _ in b.entity_log[mark:]), line

    # ---- a falling block's tag only an operator's egg puts on it: Bob's egg of floating stone
    # makes a block of sand that falls and lands; Alice's, the stone, and it floats.
    stone = varint(mc.ENTITY_TYPE["kFallingBlock"]) + mc.nbt({"BlockState": {"Name": "minecraft:stone"}, "NoGravity": mc.Byte(1)})
    b.send("set_creative_mode_slot", struct.pack(">h", 36) + mc.item_stack(I["kPigSpawnEgg"], 1, {"kEntityData": stone}))
    seq[0] += 1
    click(b, (6, -61, 8), 1, seq=seq[0])
    landed = collect(b, ticks=5)
    assert landed.get((6, -60, 8)) == mc.BLOCK["kSand"], landed
    egg('pig_spawn_egg[entity_data={id:"minecraft:falling_block",BlockState:{Name:"minecraft:stone"},NoGravity:1b}]')
    fell = one(use(a, (10, -61, 6), 1))
    assert fell.type == mc.ENTITY_TYPE["kFallingBlock"] and fell.object_data == mc.BLOCK["kStone"], (fell.type, fell.object_data)

    # ---- use_item (an egg into the air): acknowledged; Alice turns for Bob, and nothing appears.
    egg()
    collect(b)
    before = set(b.entities)
    a.send("use_item", varint(0) + varint(500) + struct.pack(">ff", 45.0, 0.0))
    assert acks(a) == [500]
    collect(b)
    assert b.entities[a.entity_id].yaw == 45.0 and set(b.entities) == before
    # With nothing in hand: acknowledged all the same, but it turns nobody.
    give(a, 36, 0, 0)
    a.send("use_item", varint(0) + varint(501) + struct.pack(">ff", -90.0, 0.0))
    assert acks(a) == [501]
    collect(b)
    assert b.entities[a.entity_id].yaw == 45.0


@test()
def pick_entity(srv: Server):
    """The middle click on an entity: a mob gives its spawn egg and an armor stand its item, into the
    hand as a picked block is; an item on the ground, a mob out of reach, an id of nothing give nothing."""
    c = srv.client()
    join(c, "Alice", view=3)
    c.send("player_loaded")
    settle(c)
    make_op(srv, c, "Alice")
    run_command(c, "summon pig 8.5 -60 11.5")
    run_command(c, "summon armor_stand 10.5 -60 8.5")
    run_command(c, 'summon item 6.5 -60 8.5 {Item:{id:"diamond",count:1},PickupDelay:32767}')
    run_command(c, "summon pig 8.5 -60 20.5")        # twelve blocks away: out of reach
    settle(c)
    pigs = sorted(of_type(c, "kPig").values(), key=lambda e: e.spawned[0][2])
    near, far = pigs[0].id, pigs[1].id
    stand, item = next(iter(of_type(c, "kArmorStand"))), next(iter(of_type(c, "kItem")))

    def pick(eid: int) -> list:
        mark = len(c.menu_log)
        c.send("pick_item_from_entity", varint(eid) + b"\x00")
        settle(c, ticks=2)
        return c.menu_log[mark:]

    assert pick(near) == [("set_held_slot", 0), ("container_set_slot", 36, plain("kPigSpawnEgg"))]
    assert pick(near) == [("set_held_slot", 0)]                 # in hand already: only the answer
    got = pick(stand)
    assert got and got[0][0] == "set_held_slot" and c.slots[36 + c.held_slot] == plain("kArmorStand"), got
    assert pick(item) == [] and pick(far) == [] and pick(4000) == []


@test()
def the_world_is_kept():
    """What was built is there when the server starts again: every chunk a client is sent is the
    same bytes as before - blocks, heightmaps, light, block entities - a chest holds what it
    held, and what lay on the ground lies there."""
    world = tempfile.mkdtemp(prefix="world-kept-")
    srv = None
    try:
        srv = Server(EXE, world=world, level_seed="Glacier")
        a = srv.client()
        ja = join(a, "Alice", view=3)
        # The world's seed is the one it was made with (WorldOptions.parseSeed: a word is its hashCode).
        assert text_seed("Glacier") == 1772835215 and ja["login"]["seed"] == hashed_seed(1772835215), ja["login"]
        a.send("player_loaded")
        settle(a)
        made = build_something(a)
        made |= build_more(a)
        # Bob comes to look: his chunks are the world as it is now.
        b = srv.client()
        join(b, "Bob", view=3)
        a.expect("player_info_update")
        settle(b, a, ticks=30)              # the stone has come to rest
        before = frames(b)
        # (The frames hold the light as well; two values by hand: beside the glowstone, and across the border.)
        assert mc.light_get(b.chunks[(0, 0)]["light"]["block"][1], 14, 4, 8) == 14
        assert mc.light_get(b.chunks[(1, 0)]["light"]["block"][1], 0, 4, 8) == 14
        stone = the_stone(b)
        where = stone.pos
        # Alice herself: what she has left of it, and where she stands when the server stops.
        stood, looked = (5.25, -60.0, 6.75), (90.0, 10.0)
        a.send("move_player_pos_rot", struct.pack(">dddffB", *stood, *looked, 1))
        hers = list(server_menu(a))
        assert any(hers), hers
        # ...and what time it is: a clock that was set and stopped, the ticks of the world.
        srv.console("time pause")
        srv.console("time set 5000")
        srv.wait_log("to 5000 tick")
        settle(a, ticks=2)
        ticks_before = a.game_time
        assert a.clocks[OVERWORLD] == (5000, 0.0, 0.0) and ticks_before > 30, (a.clocks, ticks_before)
        the_ends = a.time_log[0][1][THE_END][0] - a.time_log[0][0]
        assert srv.stop() == 0, srv.read_log()
        assert "world saved" in srv.read_log()
        os.unlink(srv.log.name)
        assert os.path.exists(os.path.join(world, "world.json")) and os.path.exists(os.path.join(world, "session.lock"))
        regions = os.listdir(os.path.join(world, "chunks", "minecraft", "overworld"))
        assert regions and all(re.fullmatch(r"r\.-?\d+\.-?\d+\.rgn", name) for name in regions), regions

        srv = Server(EXE, world=world, level_seed=7)     # (a seed for a new world: this one has its own)
        assert "a new flat world" not in srv.read_log()
        c = srv.client()
        jc = join(c, "Carol", view=3)
        assert jc["login"]["seed"] == hashed_seed(1772835215), jc["login"]
        c.send("player_loaded")
        settle(c, ticks=6)
        after = frames(c)
        assert set(after) == set(before)
        differ = [pos for pos in before if before[pos] != after[pos]]
        assert not differ, f"chunks that came back different: {differ}"
        assert sorted(c.chunks[(0, 0)]["block_entities"]) == sorted([
            ((8, -60, 10), mc.BLOCK_ENTITY["kChest"], None), (made["smoker"], mc.BLOCK_ENTITY["kSmoker"], None),
            (made["barrel"], mc.BLOCK_ENTITY["kBarrel"], None)])
        back = the_stone(c)
        assert near(back.pos, where), (back.pos, where)
        open_and_check_chest(c, made["chest"], 1)
        click(c, made["barrel"], 1, seq=2)
        assert collect(c)[made["barrel"]] == mc.BLOCK["kBarrel"] + 7    # mouth up, open
        assert c.window["title"] == {"translate": "container.barrel"} and c.window["slots"][0] == plain("kStone", 20)
        close_window(c)
        assert collect(c)[made["barrel"]] == mc.BLOCK["kBarrel"] + 8
        # The smoker burns on, at the speed of its coal: pork put into it now takes 100 ticks, as the beef did.
        give(c, 36, I["kPorkchop"])
        click(c, made["smoker"], 1, seq=3)
        collect(c)
        w = c.window
        assert w and w["type"] == "kSmoker" and w["data"][1] == 800 and w["data"][3] == 100 and 0 < w["data"][0] < 800, w
        menu_click(c, 0, 0, QUICK_MOVE)
        menu_click(c, 30, 0, QUICK_MOVE)
        settle(c)
        assert w["slots"][0] == plain("kPorkchop", 1) and w["data"][3] == 100, (w["slots"][:3], w["data"])
        close_window(c)
        settle(c)
        # Alice is back where she stood, with what she had; Bob too, and he had nothing.
        a = srv.client()
        ja = join(a, "Alice", view=3)
        assert ja["teleport"]["pos"] == stood and ja["teleport"]["rot"] == looked, ja["teleport"]
        assert a.slots == hers, (a.slots, hers)
        # The time went on from where it was: the overworld's clock stands where it was stopped,
        # the end's and the world's own ticks run on from what they had come to.
        now, clocks = a.time_log[0]
        assert clocks[OVERWORLD] == (5000, 0.0, 0.0) and clocks[THE_END] == (now + the_ends, 0.0, 1.0), (now, clocks)
        assert ticks_before <= now <= ticks_before + 400, (ticks_before, now)
        assert sorted(os.listdir(os.path.join(world, "playerstate"))) == sorted(player_file(n) for n in ("Alice", "Bob"))
        # ...and a world is one server's: a second one on it does not start.
        try:
            Server(EXE, world=world)
            raise AssertionError("two servers on one world")
        except RuntimeError as e:
            assert "in use by another server" in str(e), e
        assert srv.stop() == 0, srv.read_log()
        os.unlink(srv.log.name)
    finally:
        if srv:
            srv.stop()   # (whatever went wrong: no server is left behind)
        shutil.rmtree(world, ignore_errors=True)


@test(chunk_unload_seconds=1, view_distance=3)
def chunks_leave_and_return(srv: Server):
    """Nobody near: the chunks are saved and leave the world, with the block entities in them
    and the entities on them. Somebody comes back: they are as they were - from what is still
    waiting to be written, or from the file."""
    a = srv.client()
    ja = join(a, "Alice", view=3)
    a.send("player_loaded")
    settle(a)
    made = build_something(a)
    b = srv.client()
    join(b, "Bob", view=3)
    a.expect("player_info_update")
    settle(b, a, ticks=30)
    before = frames(b)
    where = the_stone(b).pos
    b.close()
    a.expect("player_info_remove")

    overworld = os.path.join(srv.world, "chunks", "minecraft", "overworld")
    for far, from_file, seq in ((16 * 40 + 8.5, False, 13), (16 * 80 + 8.5, True, 18)):
        # Away, far beyond every chunk that was loaded - and until those have left.
        mark = len(srv.read_log())
        walk_to_chunk(a, ja, far, 8.5, 3)
        settle(a)
        assert not of_type(a, "kItem")
        deadline = time.time() + 20
        while "left the world" not in srv.read_log()[mark:]:
            assert time.time() < deadline, srv.read_log()[-2000:]
            a.pump(0.05)                   # (keeps the connection alive)
        # The first time round they come back from what waits to be written; the second time from
        # the file - a region is written 100 ticks after the first of its chunks was saved.
        if from_file:
            settle(a, ticks=110)
            assert os.path.isdir(overworld) and os.listdir(overworld), "no region was written"
        # Back: the same chunks, byte for byte, the stone where it lay, the chest with its stone.
        walk_to_chunk(a, ja, 8.5, 8.5, 3)
        settle(a, ticks=6)
        after = frames(a)
        differ = [pos for pos in before if before[pos] != after[pos]]
        assert set(after) == set(before) and not differ, f"chunks that came back different: {differ}"
        assert near(the_stone(a).pos, where), (the_stone(a).pos, where)
        open_and_check_chest(a, made["chest"], seq)
        written = os.path.isdir(overworld) and os.listdir(overworld)
        assert bool(written) == from_file, (from_file, written)


@test(level_name="", chunk_unload_seconds=1, view_distance=2)
def a_world_that_is_not_kept(srv: Server):
    """level-name= (empty): nothing is written anywhere, and so nothing leaves memory - what was
    built is still there when its builder comes back from far away."""
    assert "the world is not kept" in srv.read_log()
    a = srv.client()
    ja = join(a, "Alice", view=2)
    a.send("player_loaded")
    settle(a)
    give(a, 36, I["kStone"])
    click(a, (8, -61, 10), 1, seq=1)
    assert collect(a)[(8, -60, 10)] == mc.BLOCK["kStone"]
    walk_to_chunk(a, ja, 16 * 30 + 8.5, 8.5, 2)
    a.pump(3.0)
    assert "left the world" not in srv.read_log()
    walk_to_chunk(a, ja, 8.5, 8.5, 2)
    assert mc.chunk_block(a.chunks[(0, 0)], 8, -60, 10) == mc.BLOCK["kStone"]
    assert os.listdir(srv.own_world) == []


# A vanilla world written by hand, from the layouts of the 26.3 jar (LevelStorageSource, RegionFile,
# SerializableChunkData, PalettedContainer, BlockState.CODEC): what the import is given to read.

def anvil_named(tag: dict) -> bytes:
    """A tag as a file holds it: the root compound with a name (the empty one)."""
    return b"\x0a\x00\x00" + mc.nbt(tag)[1:]


def anvil_container(palette: list, indices: list, least_bits: int) -> dict:
    """PalettedContainer over NBT: the palette, and - for more than one entry - the indices in
    longs, max(least_bits, ceillog2(n)) bits each, none across two longs (SimpleBitStorage)."""
    if len(palette) == 1:
        return {"palette": palette}
    bits = max(least_bits, (len(palette) - 1).bit_length())
    per = 64 // bits
    longs = []
    for at in range(0, len(indices), per):
        v = 0
        for k, index in enumerate(indices[at:at + per]):
            v |= index << (k * bits)
        longs.append(v - (1 << 64) if v >> 63 else v)
    return {"palette": palette, "data": mc.LongArray(longs)}


def anvil_chunk(cx: int, cz: int, blocks: dict | None = None, block_entities: list | None = None, biomes=None,
                status: str = "minecraft:full", ground: str = "minecraft:sandstone", light: dict | None = None,
                fluid_ticks: list | None = None) -> dict:
    """A chunk of flat ground (bedrock, three of `ground`) with `blocks` {(x, y, z): state} in its
    lowest section - coordinates in the chunk, y from the bottom of the world; a state is a block's
    name, or {id, properties}. `light`: {"sky"/"block": {section Y: bytes}} makes it a lit chunk."""
    palette = ["minecraft:bedrock", ground, "minecraft:air"]
    indices = [0 if i < 256 else 1 if i < 1024 else 2 for i in range(4096)]
    for (x, y, z), state in (blocks or {}).items():
        if state not in palette:
            palette.append(state)
        indices[y << 8 | z << 4 | x] = palette.index(state)
    sections = []
    for y in range(-4, 20):
        section = {"Y": mc.Byte(y), "block_states": anvil_container(palette, indices, 4) if y == -4 else {"palette": ["minecraft:air"]},
                   "biomes": (biomes or {}).get(y) or {"palette": ["minecraft:desert"]}}
        for kind, name in (("sky", "SkyLight"), ("block", "BlockLight")):
            if light and y in light.get(kind, {}):
                section[name] = mc.ByteArray(light[kind][y])
        sections.append(section)
    chunk = {"DataVersion": mc.META["world_version"], "xPos": cx, "yPos": -4, "zPos": cz, "Status": status,
             "LastUpdate": mc.Long(1234), "InhabitedTime": mc.Long(77), "sections": sections,
             "block_entities": block_entities or [], "structures": {"References": {}, "starts": {}}, "Heightmaps": {}}
    if light:
        chunk["isLightOn"] = mc.Byte(1)
    if fluid_ticks is not None:     # SavedTick.codec: the fluid by name, where, the ticks until it is due, its priority
        chunk["fluid_ticks"] = fluid_ticks
    return chunk


def anvil_region(path: str, chunks: dict) -> None:
    """RegionFile: 4 KiB of locations (sector << 8 | sectors), 4 KiB of timestamps, then per chunk
    an i32 length, the compression's id (2: zlib) and the bytes, in whole 4 KiB sectors."""
    import zlib
    header, body, sector = bytearray(8192), bytearray(), 2
    for (lx, lz), tag in chunks.items():
        data = zlib.compress(anvil_named(tag))
        entry = struct.pack(">IB", len(data) + 1, 2) + data
        entry += b"\x00" * (-len(entry) % 4096)
        struct.pack_into(">I", header, 4 * (lx + lz * 32), sector << 8 | len(entry) // 4096)
        body += entry
        sector += len(entry) // 4096
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(header + body)


def anvil_world(path: str, extra: dict | None = None, fluid_ticks: list | None = None) -> dict:
    """The world: nine chunks of a flat desert around the origin's north-east, with things in the
    first (and `extra`: more of them, {(x, y, z): state}; `fluid_ticks`: the ticks of its fluids
    that were scheduled when it was saved); one chunk in the region across the origin; one that
    vanilla had not finished."""
    import gzip
    version = mc.META["world_version"]
    os.makedirs(os.path.join(path, "data", "minecraft"))
    with open(os.path.join(path, "level.dat"), "wb") as f:
        f.write(gzip.compress(anvil_named({"Data": {
            "DataVersion": version, "LevelName": "by hand", "version": 19133, "Time": mc.Long(123456),
            "spawn": {"dimension": "minecraft:overworld", "pos": mc.IntArray([4, -60, 4]), "yaw": mc.Float(0), "pitch": mc.Float(0)}}})))
    # ServerClockManager's saved data: a ClockState by clock; what a state leaves out is its default.
    with open(os.path.join(path, "data", "minecraft", "world_clocks.dat"), "wb") as f:
        f.write(gzip.compress(anvil_named({"DataVersion": version, "data": {
            "minecraft:overworld": {"total_ticks": mc.Long(30000), "rate": mc.Float(2.0), "paused": mc.Byte(1)},
            "minecraft:the_end": {"total_ticks": mc.Long(77)}}})))
    with open(os.path.join(path, "data", "minecraft", "world_gen_settings.dat"), "wb") as f:
        f.write(gzip.compress(anvil_named({"DataVersion": version, "data": {"seed": mc.Long(42), "dimensions": {
            "minecraft:overworld": {"type": "minecraft:overworld", "generator": {"type": "minecraft:flat", "settings": {
                "biome": "minecraft:desert", "features": mc.Byte(0), "lakes": mc.Byte(0),
                "layers": [{"block": "minecraft:bedrock", "height": 1}, {"block": "minecraft:sandstone", "height": 3}]}}}}}})))

    lit_furnace = {"id": "minecraft:furnace", "properties": {"facing": "north", "lit": "true"}}
    things = {
        (1, 4, 1): {"id": "minecraft:chest", "properties": {"facing": "north", "type": "single", "waterlogged": "false"}},
        (3, 4, 1): "minecraft:chest",                      # the default state by its name alone
        (5, 4, 1): lit_furnace,
        (7, 4, 1): "minecraft:oak_sign",
        (9, 4, 1): {"id": "minecraft:oak_stairs", "properties": {"shape": "no such shape"}},   # an unknown value: the default's
        (11, 4, 1): "minecraft:glowstone",
        (1, 4, 3): "minecraft:barrel",
        (3, 4, 3): {"id": "minecraft:blast_furnace", "properties": {"facing": "north", "lit": "true"}},
    }
    things.update(extra or {})
    block_entities = [
        {"id": "minecraft:chest", "x": 1, "y": -60, "z": 1, "keepPacked": mc.Byte(0), "components": {},
         "Items": [{"Slot": mc.Byte(0), "id": "minecraft:stone", "count": 20}, {"Slot": mc.Byte(26), "id": "minecraft:diamond"}]},
        {"id": "minecraft:chest", "x": 3, "y": -60, "z": 1, "CustomName": "Treasure",
         "Items": [{"Slot": mc.Byte(0), "id": "minecraft:gold_ingot", "count": 9},
                   {"Slot": mc.Byte(1), "id": "minecraft:diamond_sword", "components": {"minecraft:damage": 7}}]},
        {"id": "minecraft:furnace", "x": 5, "y": -60, "z": 1, "Items": [{"Slot": mc.Byte(0), "id": "minecraft:iron_ore", "count": 3}],
         "cooking_time_spent": mc.Short(0), "cooking_total_time": mc.Short(200), "lit_time_remaining": mc.Short(0),
         "lit_total_time": mc.Short(0), "RecipesUsed": {"minecraft:iron_ingot_from_smelting_iron_ore": 2}},
        {"id": "minecraft:sign", "x": 7, "y": -60, "z": 1, "is_waxed": mc.Byte(0),
         "front_text": {"messages": ["hello", "", "", ""]}},
        {"id": "minecraft:barrel", "x": 1, "y": -60, "z": 3, "Items": [{"Slot": mc.Byte(4), "id": "minecraft:stone", "count": 7}]},
        # A blast furnace half-way through an ore, on coal: 800 ticks of it, at twice the speed.
        {"id": "minecraft:blast_furnace", "x": 3, "y": -60, "z": 3, "Items": [{"Slot": mc.Byte(0), "id": "minecraft:iron_ore", "count": 5}],
         "cooking_time_spent": mc.Short(10), "cooking_total_time": mc.Short(100), "lit_time_remaining": mc.Short(700),
         "lit_total_time": mc.Short(800), "speed_multiplier": mc.Float(2.0)},
    ]
    two_biomes = {-4: anvil_container(["minecraft:desert", "minecraft:jungle"], [(i >> 2) & 1 for i in range(64)], 1)}
    # A chunk that says its light is right: it is believed. (No server would compute this light.)
    odd_light = {"sky": {-4: bytes([0x00] * 512 + [0xFF] * 1536), -3: bytes([0xFF] * 2048)}, "block": {-4: bytes([0x55] * 2048)}}
    region = {(x, z): anvil_chunk(x, z) for x in range(3) for z in range(3)}
    region[(0, 0)] = anvil_chunk(0, 0, things, block_entities, two_biomes, fluid_ticks=fluid_ticks)
    region[(2, 2)] = anvil_chunk(2, 2, light=odd_light)
    region[(3, 3)] = anvil_chunk(3, 3, status="minecraft:features", ground="minecraft:stone")
    overworld = os.path.join(path, "dimensions", "minecraft", "overworld")
    anvil_region(os.path.join(overworld, "region", "r.0.0.mca"), region)
    anvil_region(os.path.join(overworld, "region", "r.-1.-1.mca"), {(31, 31): anvil_chunk(-1, -1, {(15, 4, 15): "minecraft:glass"})})
    anvil_region(os.path.join(overworld, "entities", "r.0.0.mca"), {(0, 0): {
        "DataVersion": version, "Position": mc.IntArray([0, 0]),
        "Entities": [{"id": "minecraft:pig", "Pos": [mc.Double(8.5), mc.Double(-60.0), mc.Double(8.5)]}]}})
    return {"lit_furnace": mc.BLOCK["kFurnace"] - 1}   # facing north, lit: the state before the default (props::furnace)


@test()
def a_vanilla_world_is_imported():
    """A vanilla world in the server's directory becomes the server's own, once: its blocks,
    biomes and light, its chests and furnaces; what the server does not read is kept and inert.
    The vanilla files are left as they were."""
    world = tempfile.mkdtemp(prefix="world-anvil-")
    srv = None
    try:
        made = anvil_world(world)
        vanilla = {}
        for root, _, names in os.walk(world):
            for name in names:
                with open(os.path.join(root, name), "rb") as f:
                    vanilla[os.path.join(root, name)] = f.read()
        srv = Server(EXE, world=world, start_timeout=30)
        log = srv.read_log()
        assert "imported 10 chunks" in log and "(1 more were not finished" in log, log
        assert "4 chests and furnaces, 2 other block entities and 1 entities kept" in log, log
        c = srv.client()
        j = join(c, "Alice", view=3)
        c.send("player_loaded")
        settle(c)
        # The world's own spawn and its own generator: flat, sandstone, desert - and its own seed.
        assert j["teleport"]["pos"] == (4.5, -60.0, 4.5), j["teleport"]["pos"]
        assert j["login"]["is_flat"] is True
        assert j["login"]["seed"] == hashed_seed(42), j["login"]["seed"]
        # ...and its own time: the level's ticks go on from where they were, the end's clock with
        # them, and the overworld's - which was stopped - stands.
        now, clocks = c.time_log[0]
        assert 123456 <= now <= 123456 + 400, now
        assert clocks == {OVERWORLD: (30000, 0.0, 0.0), THE_END: (77 + now - 123456, 0.0, 1.0)}, (now, clocks)
        desert, jungle = (mc.load_registry("worldgen_biome").index(f"minecraft:{b}") for b in ("desert", "jungle"))
        sand = mc.BLOCK["kSandstone"]
        beyond = c.chunks[(-3, 1)]                       # never in the vanilla world: the generator's
        assert [mc.chunk_block(beyond, 5, y, 5) for y in range(-64, -59)] == [mc.BLOCK["kBedrock"], sand, sand, sand, 0]
        assert beyond["sections"][0]["biomes"]["palette"] == [desert]
        unfinished = c.chunks[(3, 3)]                    # vanilla had not finished it: not its stone, the generator's sandstone
        assert mc.chunk_block(unfinished, 5, -62, 5) == sand

        ch = c.chunks[(0, 0)]
        at = lambda x, z: mc.chunk_block(ch, x, -60, z)  # noqa: E731
        assert (at(1, 1), at(3, 1), at(5, 1), at(11, 1)) == (mc.BLOCK["kChest"], mc.BLOCK["kChest"], made["lit_furnace"],
                                                              mc.BLOCK["kGlowstone"]), (at(1, 1), at(3, 1), at(5, 1))
        assert at(7, 1) == mc.BLOCK["kOakSign"] and at(9, 1) == mc.BLOCK["kOakStairs"]
        assert mc.chunk_block(c.chunks[(-1, -1)], 15, -60, 15) == mc.BLOCK["kGlass"]
        # Two biomes in the lowest section, cell by cell; the palette in the file's order.
        biomes = ch["sections"][0]["biomes"]
        assert biomes["bits"] == 1 and biomes["palette"] == [desert, jungle], biomes
        assert [mc.paletted_get(biomes, i) for i in range(64)] == [(desert, jungle)[(i >> 2) & 1] for i in range(64)]
        # Every block entity is listed - a client draws a chest only if it is told of one.
        kinds = {pos: kind for pos, kind, tag in ch["block_entities"]}
        assert kinds == {(1, -60, 1): mc.BLOCK_ENTITY["kChest"], (3, -60, 1): mc.BLOCK_ENTITY["kChest"],
                         (5, -60, 1): mc.BLOCK_ENTITY["kFurnace"], (7, -60, 1): mc.BLOCK_ENTITY["kSign"],
                         (1, -60, 3): mc.BLOCK_ENTITY["kBarrel"], (3, -60, 3): mc.BLOCK_ENTITY["kBlastFurnace"]}, kinds
        # Light: a chunk that was not lit is lit here (the glowstone shines); one that was is believed.
        assert mc.light_get(ch["light"]["block"][1], 10, 4, 1) == 14
        odd = c.chunks[(2, 2)]["light"]
        assert odd["block"][1] == bytes([0x55] * 2048) and odd["sky"][1] == bytes([0x00] * 512 + [0xFF] * 1536)
        assert not c.entities, "an entity of the vanilla world is kept, not shown"

        # The plain chest is a chest: it opens, with what it held.
        click(c, (1, -60, 1), 1, seq=1)
        collect(c)
        assert c.window and c.window["type"] == "kGeneric9x3"
        assert c.window["slots"][0] == plain("kStone", 20) and c.window["slots"][26] == plain("kDiamond", 1)
        assert c.window["slots"][1:26] == [None] * 25
        close_window(c)
        # The furnace as well: three iron ore, nothing burning.
        click(c, (5, -60, 1), 1, seq=2)
        collect(c)
        assert c.window and c.window["type"] == "kFurnace" and c.window["slots"][0] == plain("kIronOre", 3), c.window
        close_window(c)
        # The blast furnace too - which burns on as it did, at the speed of its fuel: another ore
        # put into it takes the 100 ticks the first one took.
        give(c, 36, I["kGoldOre"])
        click(c, (3, -60, 3), 1, seq=4)
        collect(c)
        w = c.window
        assert w and w["type"] == "kBlastFurnace" and w["slots"][0] == plain("kIronOre", 5), w
        assert w["data"][1] == 800 and w["data"][3] == 100 and 600 < w["data"][0] < 700 and w["data"][2] >= 10, w["data"]
        menu_click(c, 0, 0, QUICK_MOVE)
        menu_click(c, 30, 0, QUICK_MOVE)
        settle(c)
        assert w["slots"][0] == plain("kGoldOre", 1) and w["data"][3] == 100, (w["slots"][:3], w["data"])
        close_window(c)
        # The chest with a name, and a sword in it that is worn: a worker could not make that
        # ours (the item codecs are the tick thread's), so it came as its tag - and became a
        # chest when its chunk was read.
        give(c, 36, I["kStone"], 5)
        click(c, (3, -60, 1), 1, seq=5)
        collect(c)
        w = c.window
        assert w and (w["type"], w["title"]) == ("kGeneric9x3", "Treasure"), w
        assert w["slots"][0] == plain("kGoldIngot", 9) and w["slots"][1] == (I["kDiamondSword"], 1, {"kDamage": 7}), w["slots"][:3]
        close_window(c)
        collect(c)
        first = frames(c)
        assert srv.stop() == 0, srv.read_log()
        os.unlink(srv.log.name)

        # Once: the next start reads the server's own files, and every chunk is the same bytes.
        for path, content in vanilla.items():
            with open(path, "rb") as f:
                assert f.read() == content, f"{path} was changed"
        srv = Server(EXE, world=world)
        log = srv.read_log()
        assert "importing" not in log and "imported from vanilla" in log, log
        c = srv.client()
        join(c, "Bob", view=3)
        again = frames(c)
        differ = [pos for pos in first if first[pos] != again.get(pos)]
        assert not differ, f"chunks that came back different: {differ}"
        # The barrel is a barrel still, with what vanilla left in it. (Opened only now: an open
        # barrel is another state, and the section's palette remembers that it was there.)
        c.send("player_loaded")
        settle(c)
        click(c, (1, -60, 3), 1, seq=1)
        assert collect(c)[(1, -60, 3)] == mc.BLOCK["kBarrel"] - 1
        assert c.window and c.window["title"] == {"translate": "container.barrel"} and c.window["slots"][4] == plain("kStone", 7)
        close_window(c)
        assert collect(c)[(1, -60, 3)] == mc.BLOCK["kBarrel"]
        assert srv.stop() == 0, srv.read_log()
        os.unlink(srv.log.name)
    finally:
        if srv:
            srv.stop()
        shutil.rmtree(world, ignore_errors=True)


def run_one(fn, sargs: dict, keep_log: bool, label: str = "") -> bool:
    srv = None
    t0 = time.time()
    try:
        if fn.__code__.co_argcount:
            srv = Server(EXE, **sargs)
            fn(srv)
            code = srv.stop()
            assert code == 0, f"server exit code {code}"
        else:
            fn()
        print(f"PASS {label}{fn.__name__} ({time.time() - t0:.2f}s)")
        return True
    except Exception:  # noqa: BLE001
        print(f"FAIL {label}{fn.__name__}")
        traceback.print_exc()
        if srv:
            srv.stop()
            print("---- server log (tail) ----")
            print("\n".join(srv.read_log().splitlines()[-25:]))
        return False
    finally:
        if srv and not keep_log:
            os.unlink(srv.log.name)
        elif srv:
            print(f"    log: {srv.log.name}")


def run_apart(pool: list[tuple[str, str]], alone: list[tuple[str, str]], jobs: int, quiet: bool, keep_log: bool,
              label: dict[str, str]) -> tuple[list[tuple[str, str]], dict[tuple[str, str], float]]:
    """Every scenario - (binary, name) - in a process of its own, `jobs` at a time and in the order
    given, then the ones that want the machine to themselves. What a scenario printed comes out in
    one piece when it is over. Returns who failed, and how long each of the others took."""
    failed, took = [], {}
    claims = tempfile.mkdtemp(prefix="ports-")
    env = dict(os.environ, MC_TEST_PORT_CLAIMS=claims)

    def one(job: tuple[str, str]) -> tuple[tuple[str, str], bool, str, float]:
        exe, name = job
        cmd = [sys.executable, os.path.abspath(__file__), exe, "--one", name, "--label", label[exe]] + (["--keep-log"] if keep_log else [])
        t0 = time.time()
        try:
            p = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=900)
            return job, p.returncode == 0, p.stdout, time.time() - t0
        except subprocess.TimeoutExpired as e:
            out = e.stdout.decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
            return job, False, out + f"FAIL {label[exe]}{name}: still running after 900 s\n", time.time() - t0

    def report(result: tuple[tuple[str, str], bool, str, float]) -> None:
        job, ok, out, seconds = result
        if ok:
            took[job] = seconds
        else:
            failed.append(job)
        if not ok or not quiet:
            sys.stdout.write(out)
            sys.stdout.flush()

    try:
        with concurrent.futures.ThreadPoolExecutor(max(1, jobs)) as threads:
            for done in concurrent.futures.as_completed([threads.submit(one, job) for job in pool]):
                report(done.result())
        for job in alone:
            report(one(job))
    finally:
        shutil.rmtree(claims, ignore_errors=True)
    return failed, took


def main() -> int:
    global EXE
    ap = argparse.ArgumentParser()
    ap.add_argument("exe", nargs="+", help="the server binary; several are tested side by side, in one pool")
    ap.add_argument("-k", action="append", default=[], help="only tests whose name contains this (may be given more than once)")
    ap.add_argument("-j", type=int, default=1, help="scenarios at a time (0: two a core, sixteen at most)")
    ap.add_argument("--failed", action="store_true", help="only the tests that failed the last time a binary was tested")
    ap.add_argument("-q", action="store_true", help="print only what failed, and the last line")
    ap.add_argument("--slow", action="store_true", help="include tests that wait on real timers")
    ap.add_argument("--keep-log", action="store_true")
    ap.add_argument("--one", help=argparse.SUPPRESS)    # (a scenario by its whole name: what -j starts)
    ap.add_argument("--label", default="", help=argparse.SUPPRESS)  # (...and what it says before that name)
    args = ap.parse_args()
    exes = [os.path.abspath(e) for e in args.exe]

    if args.one:
        if len(exes) != 1:
            ap.error("--one takes one binary")
        EXE = exes[0]
        for fn, _, _, sargs in TESTS:
            if fn.__name__ == args.one:
                return 0 if run_one(fn, sargs, args.keep_log, args.label) else 1
        print(f"FAIL {args.label}{args.one}: no such scenario")
        return 1

    # Who failed and how long each took are kept beside the binary: a build directory is a preset, and
    # each has its own. With more than one binary, a scenario is named by its preset too.
    label = {exe: (os.path.basename(os.path.dirname(exe)) + ":" if len(exes) > 1 else "") for exe in exes}
    failed_file = {exe: os.path.join(os.path.dirname(exe), "integration-failed.txt") for exe in exes}
    times_file = {exe: os.path.join(os.path.dirname(exe), "integration-times.json") for exe in exes}
    before, times = {}, {}
    for exe in exes:
        before[exe] = set()
        if os.path.exists(failed_file[exe]):
            with open(failed_file[exe]) as f:
                before[exe] = set(f.read().split())
        times[exe] = {}
        try:
            with open(times_file[exe]) as f:
                times[exe] = json.load(f)
        except (OSError, ValueError):
            pass
    chosen = []
    for exe in exes:
        for fn, slow, alone, sargs in TESTS:
            name = fn.__name__
            if args.failed:
                if name not in before[exe] or (args.k and not any(k in name for k in args.k)):
                    continue
            elif (args.k and not any(k in name for k in args.k)) or (slow and not args.slow):
                continue
            chosen.append((exe, fn, alone, sargs))
    if args.failed and not chosen:
        print("OK: nothing failed the last time")
        return 0

    t0 = time.time()
    # A scenario mostly waits for ticks to pass, its client blocked in recv: all 67 of them, eight at a
    # time on four cores with two threads each, took 78 s; sixteen at a time 58 s and 0.9 of a core,
    # with each scenario as long as before (2026-11-22).
    jobs = args.j if args.j > 0 else max(1, min(16, 2 * (os.cpu_count() or 1)))
    if jobs > 1 or args.q or len(exes) > 1:
        # Longest first (as long as they took the last time; one never timed counts as longest): the
        # pool is over when its last scenario is, and a long one that starts last is all of the tail.
        pool = sorted(((exe, fn.__name__) for exe, fn, alone, _ in chosen if not alone or jobs == 1),
                      key=lambda job: -times[job[0]].get(job[1], math.inf))
        apart = [(exe, fn.__name__) for exe, fn, alone, _ in chosen if alone and jobs > 1]
        failed, took = run_apart(pool, apart, jobs, args.q, args.keep_log, label)
    else:
        EXE = exes[0]
        failed, took = [], {}
        for exe, fn, _, sargs in chosen:
            t1 = time.time()
            if run_one(fn, sargs, args.keep_log):
                took[(exe, fn.__name__)] = time.time() - t1
            else:
                failed.append((exe, fn.__name__))

    for exe in exes:
        still = (before[exe] - {fn.__name__ for e, fn, _, _ in chosen if e == exe}) | {name for e, name in failed if e == exe}
        times[exe].update({name: round(seconds, 2) for (e, name), seconds in took.items() if e == exe})
        try:
            if still:
                with open(failed_file[exe], "w") as f:
                    f.write("\n".join(sorted(still)) + "\n")
            elif os.path.exists(failed_file[exe]):
                os.unlink(failed_file[exe])
            with open(times_file[exe], "w") as f:
                json.dump(dict(sorted(times[exe].items())), f, indent=0)
        except OSError:  # (a binary somewhere that cannot be written to: there is no "last time" for it)
            pass
    if failed:
        names = " ".join(sorted(label[exe] + name for exe, name in failed))
        print(f"FAILED: {len(failed)} failed of {len(chosen)} in {time.time() - t0:.0f}s: {names}")
    else:
        print(f"OK: 0 failed of {len(chosen)} in {time.time() - t0:.0f}s")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
