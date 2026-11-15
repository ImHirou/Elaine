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
import os
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
    def __init__(self, exe: str, **args):
        self.port = free_port()
        opts = {"server-ip": "127.0.0.1", "server-port": self.port, "online-mode": "false", "debug": "true",
                "config": "/nonexistent/server.properties"}
        opts.update({k.replace("_", "-"): v for k, v in args.items()})
        self.log = tempfile.NamedTemporaryFile("w+", suffix=".log", delete=False)
        self.proc = subprocess.Popen([exe] + [f"--{k}={v}" for k, v in opts.items()], stderr=self.log, stdout=self.log)
        deadline = time.time() + 5
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

    def stop(self) -> int:
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
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
    out["chunks"] = {}
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
        elif got == "set_chunk_cache_center":
            out["center"] = (r.varint(), r.varint())
            r.done()
        elif got == "chunk_batch_start":
            r.done()
        elif got == "level_chunk_with_light":
            ch = mc.parse_chunk(r)
            assert (ch["x"], ch["z"]) not in out["chunks"], "chunk sent twice"
            out["chunks"][(ch["x"], ch["z"])] = ch
            out.setdefault("first_chunk", (ch["x"], ch["z"]))
        elif got == "chunk_batch_finished":
            assert r.varint() == len(out["chunks"])
            r.done()
        elif got == "system_chat":
            out["chat"] = (r.nbt_text(), r.bool())
            r.done()
            return out
        else:
            raise AssertionError(f"unexpected play packet {got}")


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

    r = view
    assert len(j["chunks"]) == (2 * r + 1) ** 2, len(j["chunks"])
    cx, cz = j["center"]
    assert j["first_chunk"] == (cx, cz), "the player's own chunk must be sent first"
    assert set(j["chunks"]) == {(cx + dx, cz + dz) for dx in range(-r, r + 1) for dz in range(-r, r + 1)}
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
    # Walk +x across a chunk border: one new column of 7 chunks appears, the far column is dropped.
    x, y, z = j["teleport"]["pos"]
    c.send("move_player_pos", struct.pack(">dddB", x + 16.0, y, z, 1))
    r = c.expect("set_chunk_cache_center")
    assert (r.varint(), r.varint()) == (cx + 1, cz)
    c.expect("chunk_batch_start")
    new = set()
    while True:
        got, r = c.recv()
        if got == "chunk_batch_finished":
            assert r.varint() == len(new)
            break
        assert got == "level_chunk_with_light", got
        ch = mc.parse_chunk(r)
        new.add((ch["x"], ch["z"]))
    assert new == {(cx + 4, cz + dz) for dz in range(-3, 4)}, new
    gone = set()
    for _ in range(7):
        r = c.expect("forget_level_chunk")
        packed = r.u64()
        gone.add((struct.unpack(">i", struct.pack(">I", packed & 0xFFFFFFFF))[0],
                  struct.unpack(">i", struct.pack(">I", packed >> 32))[0]))
    assert gone == {(cx - 3, cz + dz) for dz in range(-3, 4)}, gone
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
    """Move and swallow the chunk stream that follows; returns once the view is settled."""
    old = j["center"]
    c.send("move_player_pos", struct.pack(">dddB", x, j["teleport"]["pos"][1], z, 1))
    new = (int(x // 16), int(z // 16))
    r = c.expect("set_chunk_cache_center")
    assert (struct.unpack(">i", struct.pack(">I", r.varint()))[0], struct.unpack(">i", struct.pack(">I", r.varint()))[0]) == new
    c.expect("chunk_batch_start")
    while c.recv()[0] != "chunk_batch_finished":
        pass
    left = sum(1 for cx in range(old[0] - view, old[0] + view + 1) for cz in range(old[1] - view, old[1] + view + 1)
               if abs(cx - new[0]) > view or abs(cz - new[1]) > view)
    for _ in range(left):
        c.expect("forget_level_chunk")
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
    walk_to_chunk(c, jc, -24.0, 8.5, view=2)  # chunk (-2, 0): chunk (0, 0) is now on the border of Carol's view

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
    join(a, "Alice")
    join(b, "Bob")
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


def log_of(c: Client, eid: int, since: int = 0) -> list:
    return [(name, what) for name, e, what in c.entity_log[since:] if e == eid]


@test()
def players_see_each_other(srv: Server):
    a, b = srv.client(), srv.client()
    ja = join(a, "Alice")
    ida = ja["login"]["entity_id"]
    a.ticks(3)
    # Alone in the world: the only entity news is about oneself, once.
    assert a.own_data == FRESH_PLAYER, a.own_data
    assert not a.entities and [e[0] for e in a.entity_log] == ["set_entity_data"], a.entity_log

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
D_NO_GRAVITY, D_ITEM = mc.DATA["kNoGravity"], mc.ITEM_DATA["kItem"]
BOOL, STACK = mc.SERIALIZER["kBoolean"], mc.SERIALIZER["kItemStack"]
EYE = 1.62            # a standing player's eyes; an item appears 0.3 (as a float) below them
DROP_Y = SPAWN[1] + struct.unpack(">f", struct.pack(">f", EYE))[0] - struct.unpack(">f", struct.pack(">f", 0.3))[0]


def plain(item: str, count: int = 1) -> tuple:
    """A stack without components, as the test client reads it."""
    return (I[item], count, {})


def menu_click(c: Client, slot: int, button: int, kind: int, changed: dict | None = None, carried=None, state: int | None = None) -> None:
    """container_click on the player's own menu. `changed` and `carried` are what a client reports
    of its own prediction: {slot: (item, count) or None}. Reporting nothing makes the server
    describe everything the click did."""
    changed = changed or {}
    body = varint(0) + varint(c.state_id if state is None else state) + struct.pack(">hb", slot, button) + varint(kind)
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

    # The creative inventory screen owns its slots and tells the server: nothing comes back.
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
    assert len(c.menu_log) == 2 and not c.sounds, (c.menu_log, c.sounds)
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
    settle(b, a)
    item_type = mc.ENTITY_TYPE["kItem"]

    def items(c):
        return {i: e for i, e in c.entities.items() if e.type == item_type}

    # ---- Q: one stone leaves the hand. Bob's client took it out itself; Alice sees the arm swing
    # and the item appear below Bob's eyes - at rest and without gravity, since nothing moves it yet.
    give(b, 36, I["kStone"], 3)
    settle(b, a)
    mark, mmark = len(a.entity_log), len(b.menu_log)
    action(b, DROP_ONE)
    settle(b, a)
    assert len(items(a)) == 1 and len(items(b)) == 1
    eid, e = next(iter(items(a).items()))
    assert e.pos == (SPAWN[0], DROP_Y, SPAWN[2]) and e.velocity == (0.0, 0.0, 0.0) and e.bundled, (e.pos, DROP_Y)
    assert e.data == {D_NO_GRAVITY: (BOOL, True), D_ITEM: (STACK, plain("kStone", 1))}, e.data
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
    assert [x for x in a.entity_log[mark:] if x[1] == eid] == [("take_item_entity", eid, (idb, 1)), ("remove_entities", eid, None)]
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
    b.send("set_creative_mode_slot", struct.pack(">h", -1) + mc.item_stack(I["kDirt"], 5))
    b.send("set_creative_mode_slot", struct.pack(">h", -1) + mc.item_stack(I["kDirt"], 7))
    settle(b, a)
    dirt = {i: e for i, e in items(a).items() if e.data[D_ITEM][1][0] == I["kDirt"]}
    assert sorted(e.data[D_ITEM][1][1] for e in dirt.values()) == [5, 7] and len(items(a)) == 3
    assert all(e.pos == (2.5, DROP_Y, 2.5) for e in dirt.values())
    walk(b, jb, 8.5, 2.5, 10)       # six blocks from the dirt, six from the stone
    settle(b, a)
    # Items lying together become one: after 40 ticks of its life an item looks around, the
    # smaller stack goes into the larger, and the emptied entity is removed.
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
    taken = [(n, w) for n, e, w in c.entity_log[emark:] if e in lying]
    assert sorted(taken) == sorted([("take_item_entity", (c.entity_id, lying[i][1])) for i in lying] + [("remove_entities", None)] * 2), taken
    assert not [e for e in c.entities.values() if e.type == item_type]

    # ---- closing the menu: what is in the crafting grid and on the cursor goes back into the
    # inventory. The client is told twice, as by vanilla: at once by inventory index, and in the
    # next tick by menu slot.
    give(c, 1, I["kGlowstone"], 3)
    give(c, 4, I["kGlowstone"], 2)
    c.slots[1], c.slots[4] = plain("kGlowstone", 3), plain("kGlowstone", 2)
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


def join_many(srv: Server, n: int, view: int, online: bool = False) -> list:
    """n bots joined at once, a thread each; returns their clients in order."""
    errors, clients = [], [None] * n

    def one(i):
        try:
            c = srv.client(timeout=60)
            j = join(c, f"Bot{i:03d}", online=online, view=view)
            assert len(j["chunks"]) == (2 * view + 1) ** 2
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
    import re
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
    import math
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
    stats = re.findall(r"tick: ([\d.]+) ms avg, ([\d.]+) ms worst, ([\d.]+) ms of it entities, ([\d.]+) ms flush; "
                       r"([\d.]+) KiB/tick sent; (\d+) players", srv.read_log())
    full = [s for s in stats if int(s[5]) == n]
    print(f"    {n} players x 20 moves/s for {wall:.1f}s, each seeing {n - 1}, {'encrypted' if online else 'plain'}: "
          f"server used {100 * cpu / wall:.0f}% of one core (all threads)")
    for avg, worst, ent, flush, kib, _ in full[-2:]:
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
            assert len(j["chunks"]) == 121
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
