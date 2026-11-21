#!/usr/bin/env python3
"""End-to-end tests: start the real server binary and talk to it with mcproto.Client.

    python3 tests/integration/run.py build/release/craftc [-k substring] [--slow] [--keep-log]

Each scenario gets a fresh server on a free port. --slow adds the tests that wait on real timers
(keep-alive every 15 s).
"""
from __future__ import annotations

import argparse
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


def test(fn=None, *, slow=False, **server_args):
    def deco(f):
        TESTS.append((f, slow, server_args))
        return f
    return deco(fn) if fn else deco


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


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
                "random-tick-speed": 0}
        start_timeout = float(args.pop("start_timeout", 5))  # (importing a world takes its time)
        opts.update({k.replace("_", "-"): v for k, v in args.items()})
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
                time.sleep(0.02)

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

    def wait_log(self, text: str, timeout: float = 5.0) -> str:
        """The server's log once `text` is in it."""
        deadline = time.time() + timeout
        while True:
            log = self.read_log()
            if text in log:
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

def join(c: Client, name: str, online: bool = False, view: int = 10) -> dict:
    """Login + configuration + the whole join burst. Returns everything observed."""
    out = {"profile": c.login(name, online)}
    out["config"] = c.configure(view_distance=view)
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
        elif got == "system_chat":
            out["chat"] = (r.nbt_text(), r.bool())
            r.done()
            break
        else:
            raise AssertionError(f"unexpected play packet {got}")
    # The chunks come batch by batch from the end of the join's tick on (PlayerChunkSender): the
    # client's own cache (c.chunks) takes them; here the view is waited for.
    assert c.chunk_center is not None, "no set_chunk_cache_center before the first chunk"
    out["view"] = min(max(view, 2), out["login"]["view_distance"])
    c.sync_chunks(out["view"])
    # A client says where it is every tick, at the least once a second. A server looks again at
    # who sees whom on each such packet (ChunkMap.move) - which is when what stands in the chunks
    # that have just come is shown.
    c.send("move_player_status_only", b"\x00")
    out["center"] = c.chunk_center
    out["chunks"] = c.chunks
    out["first_chunk"] = next(e[1][0] for e in c.chunk_log if e[0] == "batch")
    return out


def check_join(j: dict, name: str, view: int = 10, online: bool = False) -> None:
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
    assert j["chat"] == (f"Welcome, {name}!", False)

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
    j = join(c, "Steve")
    assert j["profile"]["uuid"] == mc.offline_uuid("Steve") and j["profile"]["name"] == "Steve"
    assert c.threshold == -1 and not j["profile"]["encrypted"]
    assert j["config"]["brand"] == "vanilla"
    check_join(j, "Steve")
    assert [t["name"] for t in j["tab"]] == ["Steve"]


@test()
def join_offline_compressed(srv: Server):
    c = srv.client()
    j = join(c, "Alex")
    assert c.threshold == 256
    assert c.deflated > 0 and c.stored > 0, (c.deflated, c.stored)
    check_join(j, "Alex")


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
        j = join(c, "Notch", online=True)
        prof = j["profile"]
        assert prof["encrypted"] and prof["should_authenticate"] is True
        assert prof["uuid"] == "069a79f444e94726a5befca90e38aaf5" and prof["name"] == "Notch"
        assert prof["props"] == [("textures", "dGV4", "c2ln")]
        # The server must have asked about exactly the hash this client computed independently.
        path, q = mock.requests[-1]
        assert path == "/session/minecraft/hasJoined"
        assert q["username"] == ["Notch"] and q["serverId"] == [prof["hash"]], (q, prof["hash"])
        check_join(j, "Notch", online=True)
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
    join(b, "Bob", view=2)
    skip_tab_updates(a, 1)
    grass, air = mc.BLOCK["kGrassBlock"], 0

    # Until the client says it finished loading (or 60 ticks pass) block actions are ignored - no
    # acknowledgement either.
    give(a, 36, mc.ITEM["kStone"])
    click(a, (8, -61, 10), 1, seq=1)
    dig(a, (8, -61, 10), seq=2)
    expect_quiet(a)
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

    # A block whose placement needs behaviour that is not written yet (stairs: facing, shape) is not
    # placed at all; the client is told what is really there.
    give(a, 39, mc.ITEM["kOakStairs"])
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
    boxed = {"kContainer": varint(0)}   # stacks within a stack
    c.send("set_creative_mode_slot", struct.pack(">h", 42) + mc.item_stack(I["kStone"], 1, boxed))
    c.send("set_creative_mode_slot", struct.pack(">h", 36) + mc.item_stack(I["kStone"], 1, boxed))
    # ...and so is a text with a part it does not read (a selector), or a name longer than a stack carries here.
    c.send("set_creative_mode_slot", struct.pack(">h", 44) + mc.item_stack(I["kStone"], 1, {"kCustomName": mc.nbt({"selector": "@a"})}))
    c.send("set_creative_mode_slot", struct.pack(">h", 45) + mc.item_stack(I["kStone"], 1, {"kCustomName": mc.nbt("x" * 2000)}))
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

    def soon_after(first, second, between: int, within: float, ticks: int = 0) -> list:
        """`first`, then `second` at least `between` + 1 ticks later and - going by the clock - not
        many more: a client that was held up longer than `within` seconds tries again."""
        for attempt in range(20):
            if attempt:
                settle(b, a)   # the attempt before ended in the middle of something
            mark = rest(ticks)
            t0 = time.time()
            first()
            b.ticks(between + 1)
            second()
            if time.time() - t0 < within:
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
    assert soon_after(punch, punch, between=0, within=0.1) == [default]
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
    assert soon_after(punch, lambda: action(b, DROP_ONE), between=0, within=0.1) == [default]
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
    assert soon_after(punch, punch, between=4, within=0.45, ticks=8) == [stab]      # a whack would be over
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


@test(slow=True)
def dropped_items_expire(srv: Server):
    """ItemEntity.LIFETIME: an item nobody picks up is gone after 6000 ticks - five minutes."""
    c = srv.client()
    j = join(c, "Alice")
    c.send("player_loaded")
    give(c, 36, I["kStone"], 1)
    action(c, DROP_ONE)
    walk(c, j, 2.5, 2.5, 10)        # out of reach before it can be picked up
    settle(c)
    item_type = mc.ENTITY_TYPE["kItem"]
    lying = [i for i, e in c.entities.items() if e.type == item_type]
    assert len(lying) == 1
    t0 = time.time()
    c.pump(290 - (time.time() - t0))
    assert lying[0] in c.entities, f"gone after {time.time() - t0:.0f} s"
    c.pump(13)
    assert lying[0] not in c.entities, "still there after 303 s"
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
def a_lid_is_told_once(srv: Server):
    """A chest says how many have it open when that changes, and again with every recount - five
    ticks apart while it is open, and once more after. A block event that is asked for twice in a
    tick is sent once (vanilla keeps the tick's events in a set): closed in the very tick of the
    recount, the lid is told to fall once, not twice."""
    a = srv.client()
    join(a, "Alice")
    a.send("player_loaded")
    settle(a)
    ground, pos = (8, -61, 10), (8, -60, 10)
    give(a, 36, I["kChest"], 1)
    click(a, ground, 1, seq=1)
    assert collect(a)[pos] == mc.BLOCK["kChest"]
    told = []
    for wait in range(1, 9):                # every phase of the five ticks, and some of them again
        click(a, pos, 1, seq=1 + wait)
        collect(a, ticks=wait)
        n = len(a.block_events)
        close_window(a)
        collect(a, ticks=8)                 # (the recount that was due when it closed has come by now)
        after = [e[:3] for e in a.block_events[n:]]
        assert set(after) <= {(pos, 1, 1), (pos, 1, 0)} and after[-1] == (pos, 1, 0), (wait, after)
        told.append(after.count((pos, 1, 0)))   # (a short wait: the lid's going up is still on its way)
    assert set(told) == {1, 2}, told        # by the close and by the recount - or by both at once


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
    # The fire: 300 ticks for a plank, 200 to cook a stone - counting down and up as we look.
    assert w["data"][1] == 300 and w["data"][3] == 200 and 0 < w["data"][2] <= 6 and 293 <= w["data"][0] < 300, w["data"]
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
    assert server_menu(c)[37] == plain("kCobblestone", 3)    # the cursor went back into the inventory: the slot it came from is the first free one


@test(slow=True)
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
    assert 80 < w["data"][0] <= 101 and pos not in changes, (w["data"], changes)     # still burning: the tick that lit it cooked too
    for _ in range(40):
        changes.update(collect(c, ticks=3))
        if pos in changes:
            break
    assert changes[pos] == off and w["data"][0] == 0, (changes, w["data"])
    menu_click(c, 2, 0, QUICK_MOVE)          # the stone into the inventory: the hotbar's far end first
    settle(c)
    assert w["slots"][2] is None and w["slots"][38] == plain("kStone", 1)


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
        assert len(dropped) == 4 and all(e.data[D_ITEM][1] == plain("kTorch", 1) for e in dropped), [e.data for e in dropped]
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
        out = run_command(a, "setblock 8 120 12 minecraft:sand")
        assert out["said"] == [tr("commands.setblock.success", 8, 120, 12)], out["said"]
        for _ in range(20):
            settle(a, ticks=2)
            if of_type(a, "kFallingBlock"):
                break
        (eid, e), = of_type(a, "kFallingBlock").items()
        assert e.object_data == mc.BLOCK["kSand"] and e.spawned[0] == (8.5, 120.0, 12.5)
        settle(a, ticks=8)
        assert srv.stop() == 0, srv.read_log()
        os.unlink(srv.log.name)

        srv = Server(EXE, world=world)
        c = srv.client()
        join(c, "Carol", view=3)
        c.send("player_loaded")
        settle(c, ticks=6)
        (eid, e), = of_type(c, "kFallingBlock").items()
        assert e.object_data == mc.BLOCK["kSand"] and 20.0 < e.pos[1] < 120.0, e.pos
        assert mc.FALLING_DATA["kStartPos"] not in e.data      # where it began is not kept (vanilla does not either)
        changes = until(c, lambda ch: ch.get((8, -60, 12)) == mc.BLOCK["kSand"], ticks=300)
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
    c.send("chat_command", mc.mc_str(line))
    for i in range(ticks):
        token = 0xC0DE00 + i
        c.send("ping_request", struct.pack(">q", token))
        while True:
            name, r = c.recv()
            if name == "pong_response":
                assert r.i64() == token
                break
            if name == "system_chat":
                out["said"].append(r.nbt())
                assert r.bool() is False     # not the action bar
                r.done()
            elif name == "player_position":
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

    # Nobody is an operator: level 0, and a tree of the two commands anyone may use.
    assert a.permission_level == 0 and sorted(a.commands["children"]) == ["help", "list"], a.commands["children"].keys()
    lst = a.commands["children"]["list"]
    assert lst["executable"] and not lst["restricted"] and list(lst["children"]) == ["uuids"] and lst["children"]["uuids"]["executable"]
    assert a.commands["children"]["help"]["children"]["command"]["properties"] == {"type": "greedy"}
    # What is not in the tree is not a command, for this player.
    line = "setblock 8 -60 12 stone"
    got = run_command(a, line)
    assert got["said"] == [failure(tr("command.unknown.command")), here(line, 0)] and not got["blocks"], got
    assert run_command(a, "list")["said"] == [tr("commands.list.players", 2, 500, {"text": "", "extra": [
        player_name("Alice"), {"text": ", ", "color": "gray"}, player_name("Bob")]})]
    assert run_command(a, "help")["said"] == ["/help [<command>]", "/list [uuids]"]

    # The console makes Alice an operator: entity event 28, the whole tree, a line in ops.json.
    make_op(srv, a, "Alice")
    tree = a.commands["children"]
    assert sorted(tree) == ["deop", "fill", "give", "help", "kick", "kill", "list", "op", "playsound", "setblock", "stop",
                            "stopsound", "summon", "teleport", "tp"], sorted(tree)
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
    assert b.permission_level == 0 and sorted(b.commands["children"]) == ["help", "list"]

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
    assert b.permission_level == 0 and sorted(b.commands["children"]) == ["help", "list"]
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
    items = len(of_type(b, "kItem"))
    mark = len(a.menu_log)
    got = run_command(a, "fill 8 -60 11 8 -60 12 air")
    assert got["said"] == [tr("commands.fill.success", 2)] and got["blocks"] == {at: AIR, (8, -60, 12): AIR}, got
    settle(a)
    collect(b)
    assert a.window is None and ("container_close", 1) in a.menu_log[mark:]    # the screen closes with its chest
    assert len(of_type(b, "kItem")) == items

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
    got = run_command(a, "setblock 8 -60 11 oak_stairs")
    assert got["said"] == [failure("The block minecraft:oak_stairs is not supported by this server yet")] and not got["blocks"]
    got = run_command(a, "setblock 8 -60 11 chest[waterlogged=true]")
    assert got["said"] == [failure("The block minecraft:chest cannot hold a fluid on this server yet")]

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
    line = "give @s diamond[damage=1]"
    assert run_command(a, line)["said"] == [failure("Item components in commands are not supported by this server yet"), here(line, 15)]
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
    while True:
        name, r = b.recv()
        if name == "disconnect":
            assert r.nbt() == tr("disconnect.spam")
            break
        assert name == "system_chat", name
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
    a.send("chat_command", mc.mc_str("stop"))
    said = []
    while True:
        name, r = a.recv()
        if name == "system_chat":
            said.append(r.nbt())
        elif name == "disconnect":
            break
    assert said == [tr("commands.stop.stopping")], said
    assert srv.proc.wait(timeout=5) == 0
    assert "[Alice: Stopping the server]" in srv.read_log()


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


@test(max_players=100, view_distance=2)
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


@test(slow=True, max_players=200, view_distance=2, debug="true")
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


@test(slow=True, max_players=500, view_distance=2, debug="true")
def crowd_500(srv: Server):
    """The worst case the server is built for, without encryption (offline mode)."""
    crowd_load(srv, 500, 11, online=False)


@test(slow=True)
def crowd_500_online():
    """The same crowd in online mode: every byte of it goes through AES-CFB8 in the flush."""
    mock = MockSessionServer()
    srv = Server(EXE, online_mode="true", session_server=mock.url, max_players=500, view_distance=2, debug="true")
    try:
        crowd_load(srv, 500, 11, online=True)
    finally:
        code = srv.stop()
        mock.stop()
        assert code == 0, f"server exit code {code}\n{srv.read_log()[-2000:]}"


@test(max_players=400)
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

    # Steady state: every client moves 20 times a second for 3 s. Report what that costs the server.
    cpu0, t1 = srv.cpu_seconds(), time.time()
    for tick in range(60):
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


@test()
def the_world_is_kept():
    """What was built is there when the server starts again: every chunk a client is sent is the
    same bytes as before - blocks, heightmaps, light, block entities - a chest holds what it
    held, and what lay on the ground lies there."""
    world = tempfile.mkdtemp(prefix="world-kept-")
    srv = None
    try:
        srv = Server(EXE, world=world)
        a = srv.client()
        join(a, "Alice", view=3)
        a.send("player_loaded")
        settle(a)
        made = build_something(a)
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
        assert srv.stop() == 0, srv.read_log()
        assert "world saved" in srv.read_log()
        os.unlink(srv.log.name)
        assert os.path.exists(os.path.join(world, "world.json")) and os.path.exists(os.path.join(world, "session.lock"))
        regions = os.listdir(os.path.join(world, "chunks", "minecraft", "overworld"))
        assert regions and all(re.fullmatch(r"r\.-?\d+\.-?\d+\.rgn", name) for name in regions), regions

        srv = Server(EXE, world=world)
        assert "a new flat world" not in srv.read_log()
        c = srv.client()
        join(c, "Carol", view=3)
        c.send("player_loaded")
        settle(c, ticks=6)
        after = frames(c)
        assert set(after) == set(before)
        differ = [pos for pos in before if before[pos] != after[pos]]
        assert not differ, f"chunks that came back different: {differ}"
        assert c.chunks[(0, 0)]["block_entities"] == [((8, -60, 10), mc.BLOCK_ENTITY["kChest"], None)]
        back = the_stone(c)
        assert near(back.pos, where), (back.pos, where)
        open_and_check_chest(c, made["chest"], 1)
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

    for far, wait in ((16 * 40 + 8.5, 3.0), (16 * 80 + 8.5, 8.0)):
        # Away, far beyond every chunk that was loaded - and until those have left.
        walk_to_chunk(a, ja, far, 8.5, 3)
        settle(a)
        assert not of_type(a, "kItem")
        a.pump(wait)                       # (keeps the connection alive)
        log = srv.read_log()
        assert "left the world" in log, log[-2000:]
        # Back: the same chunks, byte for byte, the stone where it lay, the chest with its stone.
        walk_to_chunk(a, ja, 8.5, 8.5, 3)
        settle(a, ticks=6)
        after = frames(a)
        differ = [pos for pos in before if before[pos] != after[pos]]
        assert set(after) == set(before) and not differ, f"chunks that came back different: {differ}"
        assert near(the_stone(a).pos, where), (the_stone(a).pos, where)
        open_and_check_chest(a, made["chest"], 10 + int(wait))
        # The first time round they came from what waited to be written - a region is written five
        # seconds after the first of its chunks was saved; the second time from the file.
        written = os.path.isdir(os.path.join(srv.world, "chunks", "minecraft", "overworld")) and \
            os.listdir(os.path.join(srv.world, "chunks", "minecraft", "overworld"))
        assert bool(written) == (wait > 5), (wait, written)


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
                status: str = "minecraft:full", ground: str = "minecraft:sandstone", light: dict | None = None) -> dict:
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


def anvil_world(path: str) -> dict:
    """The world: nine chunks of a flat desert around the origin's north-east, with things in the
    first; one chunk in the region across the origin; one that vanilla had not finished."""
    import gzip
    version = mc.META["world_version"]
    os.makedirs(os.path.join(path, "data", "minecraft"))
    with open(os.path.join(path, "level.dat"), "wb") as f:
        f.write(gzip.compress(anvil_named({"Data": {
            "DataVersion": version, "LevelName": "by hand", "version": 19133,
            "spawn": {"dimension": "minecraft:overworld", "pos": mc.IntArray([4, -60, 4]), "yaw": mc.Float(0), "pitch": mc.Float(0)}}})))
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
    }
    block_entities = [
        {"id": "minecraft:chest", "x": 1, "y": -60, "z": 1, "keepPacked": mc.Byte(0), "components": {},
         "Items": [{"Slot": mc.Byte(0), "id": "minecraft:stone", "count": 20}, {"Slot": mc.Byte(26), "id": "minecraft:diamond"}]},
        {"id": "minecraft:chest", "x": 3, "y": -60, "z": 1, "CustomName": "Treasure",
         "Items": [{"Slot": mc.Byte(0), "id": "minecraft:gold_ingot", "count": 9}]},
        {"id": "minecraft:furnace", "x": 5, "y": -60, "z": 1, "Items": [{"Slot": mc.Byte(0), "id": "minecraft:iron_ore", "count": 3}],
         "cooking_time_spent": mc.Short(0), "cooking_total_time": mc.Short(200), "lit_time_remaining": mc.Short(0),
         "lit_total_time": mc.Short(0), "RecipesUsed": {"minecraft:iron_ingot_from_smelting_iron_ore": 2}},
        {"id": "minecraft:sign", "x": 7, "y": -60, "z": 1, "is_waxed": mc.Byte(0),
         "front_text": {"messages": ["hello", "", "", ""]}},
    ]
    two_biomes = {-4: anvil_container(["minecraft:desert", "minecraft:jungle"], [(i >> 2) & 1 for i in range(64)], 1)}
    # A chunk that says its light is right: it is believed. (No server would compute this light.)
    odd_light = {"sky": {-4: bytes([0x00] * 512 + [0xFF] * 1536), -3: bytes([0xFF] * 2048)}, "block": {-4: bytes([0x55] * 2048)}}
    region = {(x, z): anvil_chunk(x, z) for x in range(3) for z in range(3)}
    region[(0, 0)] = anvil_chunk(0, 0, things, block_entities, two_biomes)
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
        assert "2 chests and furnaces, 2 other block entities and 1 entities kept" in log, log
        c = srv.client()
        j = join(c, "Alice", view=3)
        c.send("player_loaded")
        settle(c)
        # The world's own spawn and its own generator: flat, sandstone, desert.
        assert j["teleport"]["pos"] == (4.5, -60.0, 4.5), j["teleport"]["pos"]
        assert j["login"]["is_flat"] is True
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
                         (5, -60, 1): mc.BLOCK_ENTITY["kFurnace"], (7, -60, 1): mc.BLOCK_ENTITY["kSign"]}, kinds
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
        # The chest with a name is kept as it is and stays shut: nothing opens, nothing is placed against it.
        give(c, 36, I["kStone"], 5)
        click(c, (3, -60, 1), 1, seq=3)
        assert collect(c) == {(3, -60, 1): mc.BLOCK["kChest"], (3, -59, 1): 0} and c.window is None
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
        assert srv.stop() == 0, srv.read_log()
        os.unlink(srv.log.name)
    finally:
        if srv:
            srv.stop()
        shutil.rmtree(world, ignore_errors=True)


def main() -> int:
    global EXE
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("-k", default="", help="only tests whose name contains this")
    ap.add_argument("--slow", action="store_true", help="include tests that wait on real timers")
    ap.add_argument("--keep-log", action="store_true")
    args = ap.parse_args()
    EXE = os.path.abspath(args.exe)

    failed = 0
    for fn, slow, sargs in TESTS:
        if args.k not in fn.__name__ or (slow and not args.slow):
            continue
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
            print(f"PASS {fn.__name__} ({time.time() - t0:.2f}s)")
        except Exception:  # noqa: BLE001
            failed += 1
            print(f"FAIL {fn.__name__}")
            traceback.print_exc()
            if srv:
                srv.stop()
                print("---- server log (tail) ----")
                print("\n".join(srv.read_log().splitlines()[-25:]))
        finally:
            if srv and not args.keep_log:
                os.unlink(srv.log.name)
            elif srv:
                print(f"    log: {srv.log.name}")
    print(f"{'FAILED' if failed else 'OK'}: {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
