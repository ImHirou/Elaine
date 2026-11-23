#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""What does a server tell one player about another? Record it, on vanilla and on ours, and diff.

    tools/vanilla/compare.py entity_probe            # both servers, both recordings, the diff
    entity_probe.py record <port> <out.json> <nameA> <nameB> <console>
    entity_probe.py diff <vanilla.json> <ours.json>

`record` joins two bots (tests/integration/mcproto.py, offline mode): A watches, B acts - joins,
walks, turns, sneaks, sprints, jumps, stands, walks out of sight and back, sprints in a tank of
water the console builds around it - and so swims - changes its options, leaves. Every play packet either of them receives is logged per step, entity packets with their
bytes. `diff` lines two recordings up step by step, entity ids and the two bots' packets only.

The reference is the vanilla server, run under the rules in AGENTS.md - compare.py starts it and
ours, and gives `record` the console of each: the probe keeps both in step by it (pace.py), so a
recording is the same script at the same ticks on either.
    # server.properties: server-ip=127.0.0.1, online-mode=false, white-list=false, gamemode=creative,
    #                    level-type=minecraft\\:flat, view-distance=2, a port of its own

Reading the diff:
  * Use names vanilla has not seen: it saves players, and one that returns starts on the ground,
    turned the way it left.
  * Updates go out every other tick, so which of two packets an update catches depends on when the
    tick fell: a step that differs by one merged or split move is the same behaviour.
  * Vanilla puts the two bots at random places around the spawn point: coordinates differ.
  * A vanilla flat world has animals in it; only packets about the two bots are compared.
"""
from __future__ import annotations

import json
import math
import socket
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tests" / "integration"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import mcproto as mc  # noqa: E402
from pace import Lockstep, console_of  # noqa: E402
from mcproto import varint  # noqa: E402

ENTITY = {"bundle_delimiter", "add_entity", "set_entity_data", "update_attributes", "set_equipment", "move_entity_pos",
          "move_entity_pos_rot", "move_entity_rot", "rotate_head", "entity_position_sync", "remove_entities",
          "set_entity_motion", "entity_event", "animate", "player_info_update", "player_info_remove", "teleport_entity",
          "swing_animation", "set_passengers", "hurt_animation", "damage_event"}


class Bot:
    def __init__(self, port: int, name: str):
        self.name = name
        self.c = mc.Client("127.0.0.1", port, timeout=10)
        self.pos = None
        self.rot = (0.0, 0.0)
        self.entity_id = None
        self.seen: list[tuple[str, str]] = []   # what pump() has read so far
        self.lock = None                        # the Lockstep it keeps step by, once it has joined

    def begin(self, lock: Lockstep) -> None:
        """Join, and be there: in step with the server, the view's chunks taken, the client loaded."""
        self.join()
        lock.join(self)
        lock.chunks(self)
        self.c.send("player_loaded")
        lock.sync()

    def join(self) -> None:
        c = self.c
        c.login(self.name)
        while True:  # tolerant of whatever else a server sends while configuring
            got, r = c.recv()
            if got == "select_known_packs":
                packs = [(r.str(), r.str(), r.str()) for _ in range(r.varint())]
                c.send("client_information", mc.mc_str("en_us") + struct.pack(">b", 10) + varint(0) + b"\x01" + b"\x7f" +
                       varint(1) + b"\x00\x01" + varint(0))
                c.send("custom_payload", mc.mc_str("minecraft:brand") + mc.mc_str("vanilla"))
                c.send("select_known_packs", varint(len(packs)) + b"".join(mc.mc_str(a) + mc.mc_str(b) + mc.mc_str(v) for a, b, v in packs))
            elif got == "finish_configuration":
                break
            elif got == "keep_alive":
                c.send("keep_alive", r.take(8))
            elif got == "disconnect":
                raise RuntimeError("refused while configuring: " + r.take(r.remaining()).hex())
        c.send("finish_configuration")
        c.state = "play"
        got, r = c.recv_any()
        assert got == "login", got
        self.entity_id = r.i32()

    def pump(self, seconds: float) -> list[tuple[str, str]]:
        """Everything that arrives within `seconds`, raw; answers what a client must answer."""
        c = self.c
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
            data = r.d[r.i:]
            if got == "player_position":
                tid = r.varint()
                x, y, z = r.f64(), r.f64(), r.f64()
                r.take(24)
                yaw, pitch = r.f32(), r.f32()
                # Relative: bits X, Y, Z, Y_ROT, X_ROT say which values are offsets from where the
                # client is. What it confirms is where that puts it - a server takes the echo as a move.
                flags = r.i32()
                old, rot = getattr(self, "pos", None) or [0.0, 0.0, 0.0], getattr(self, "rot", (0.0, 0.0))
                x, y, z = (v + o if flags >> i & 1 else v for i, (v, o) in enumerate(zip((x, y, z), old)))
                yaw, pitch = yaw + rot[0] if flags & 8 else yaw, pitch + rot[1] if flags & 16 else pitch
                self.pos = [x, y, z]
                self.rot = (yaw, pitch)
                c.send("accept_teleportation", varint(tid) + struct.pack(">dddff", x, y, z, yaw, pitch))
            elif got == "keep_alive":
                c.send("keep_alive", data)
            elif got == "chunk_batch_finished":
                c.send("chunk_batch_received", struct.pack(">f", 20.0))
            elif got == "disconnect":
                raise RuntimeError(f"{self.name} was disconnected: {data.hex()}")
            out.append((got, data.hex() if got in ENTITY else ""))
        c.sock.settimeout(10)
        self.seen += out
        return out

    def tick_end(self) -> None:
        self.c.send("client_tick_end")

    def move(self, dx=0.0, dy=0.0, dz=0.0, ground=True) -> None:
        self.pos[0] += dx
        self.pos[1] += dy
        self.pos[2] += dz
        self.c.send("move_player_pos", struct.pack(">dddB", *self.pos, 1 if ground else 0))

    def move_rot(self, dx, dz, yaw, pitch, ground=True) -> None:
        self.pos[0] += dx
        self.pos[2] += dz
        self.rot = (yaw, pitch)
        self.c.send("move_player_pos_rot", struct.pack(">dddffB", *self.pos, yaw, pitch, 1 if ground else 0))

    def look(self, yaw, pitch, ground=True) -> None:
        self.rot = (yaw, pitch)
        self.c.send("move_player_rot", struct.pack(">ffB", yaw, pitch, 1 if ground else 0))


def record(port: int, out_path: str, name_a: str, name_b: str, console: str) -> None:
    """The game stands still all the while (pace.py): what is recorded is players, who tick in a
    frozen game as in any - once a loop of the server, which is what a "tick" is here."""
    steps: list[dict] = []
    lock = Lockstep(console_of(console))
    a = Bot(port, name_a)
    a.begin(lock)
    lock.place((a,), "entity " + name_a + name_b)
    b = Bot(port, name_b)

    def step(name: str, actions, ticks: int = 1, settle: int = 5, bots=()):
        """Run actions(i) once a tick, then log what A (and `bots`) received meanwhile and in
        `settle` ticks more."""
        mark_a, mark_b = len(a.seen), len(b.seen)
        for i in range(ticks):
            actions(i)
            lock.mark()
        lock.sync()
        for _ in range(settle):
            lock.mark()
        got_a = a.seen[mark_a:]
        steps.append({"step": name, "A": got_a, "B": b.seen[mark_b:] if bots else []})
        print(f"{name}: A got {len(got_a)} packets, {sum(1 for n, _ in got_a if n in ENTITY)} about entities")

    def join_b(_):
        b.begin(lock)
        lock.place((b,), "entity " + name_a + name_b)

    def tick(fn):  # one client tick of B: what it does, then client_tick_end
        def run(i):
            fn(i)
            b.tick_end()
        return run

    def options(i):
        if i == 0:
            b.c.send("client_information", mc.mc_str("en_us") + struct.pack(">b", 10) + varint(0) + b"\x01" + b"\x7e" + varint(0) +
                     b"\x00\x01" + varint(0))

    step("B joins", join_b, settle=20, bots=[b])
    step("B walks +x, 0.25 a tick, 8 ticks", tick(lambda i: b.move(dx=0.25)), ticks=8, bots=[b])
    step("B turns only, 6 ticks", tick(lambda i: b.look(10.0 * (i + 1), 5.0 * (i + 1))), ticks=6, bots=[b])
    step("B walks and turns, 6 ticks", tick(lambda i: b.move_rot(0.0, 0.25, 90.0 + 15.0 * i, -20.0)), ticks=6, bots=[b])
    step("B idle, 4 ticks", tick(lambda i: None), ticks=4, bots=[b])
    step("B shift down", tick(lambda i: b.c.send("player_input", bytes([0x20])) if i == 0 else None), ticks=4, bots=[b])
    step("B shift up", tick(lambda i: b.c.send("player_input", bytes([0x00])) if i == 0 else None), ticks=4, bots=[b])
    step("B sprint start", tick(lambda i: b.c.send("player_command", varint(b.entity_id) + varint(1) + varint(0)) if i == 0 else None), ticks=4, bots=[b])
    step("B sprint stop", tick(lambda i: b.c.send("player_command", varint(b.entity_id) + varint(2) + varint(0)) if i == 0 else None), ticks=4, bots=[b])
    step("B leaves the ground", tick(lambda i: b.move(dy=0.5, ground=False)), ticks=2, bots=[b])
    step("B lands", tick(lambda i: b.move(dy=-0.5, ground=(i == 1))), ticks=2, bots=[b])
    step("B stands still, 3.5 s", tick(lambda i: None), ticks=70, bots=[b])
    step("B steps 9 blocks at once (too far for a delta)", tick(lambda i: b.move(dx=9.0)), ticks=1, bots=[b])
    step("B walks away, 4 a tick, 12 ticks", tick(lambda i: b.move(dx=4.0)), ticks=12, bots=[b])
    step("B walks back, 4 a tick, 15 ticks", tick(lambda i: b.move(dx=-4.0)), ticks=15, bots=[b])
    # In water: one that sprints with its eyes under it swims - a flag and a pose, the server's to
    # say. Twelve blocks from A, in a tank of glass the console builds around where B stands.
    def tank(inside: str | None) -> None:
        x, y, z = (math.floor(v) for v in b.pos)
        if inside:
            lock.console(f"fill {x - 2} {y - 1} {z - 2} {x + 2} {y + 3} {z + 2} minecraft:glass\n"
                         f"fill {x - 1} {y} {z - 1} {x + 1} {y + 2} {z + 1} {inside}")
        else:
            lock.console(f"fill {x - 2} {y} {z - 2} {x + 2} {y + 3} {z + 2} minecraft:air\n"
                         f"fill {x - 2} {y - 1} {z - 2} {x + 2} {y - 1} {z + 2} minecraft:grass_block")

    def sprinting(on: bool) -> None:
        b.c.send("player_command", varint(b.entity_id) + varint(1 if on else 2) + varint(0))

    step("B walks 12 blocks off", tick(lambda i: b.move(dz=4.0)), ticks=3, bots=[b])
    step("the console builds a tank of water around B", tick(lambda i: tank("minecraft:water") if i == 0 else None), ticks=4, bots=[b])
    step("B sprints under water: it swims", tick(lambda i: sprinting(True) if i == 0 else None), ticks=6, bots=[b])
    step("B stops sprinting: it stands", tick(lambda i: sprinting(False) if i == 0 else None), ticks=6, bots=[b])
    step("B sprints again, and the water goes: it swims, then only sprints", ticks=8, bots=[b],
         actions=tick(lambda i: sprinting(True) if i == 0 else tank("minecraft:air") if i == 4 else None))
    step("B stops, and the tank goes", tick(lambda i: (sprinting(False), tank(None)) if i == 0 else None), ticks=4, bots=[b])
    step("B walks back", tick(lambda i: b.move(dz=-4.0)), ticks=3, bots=[b])
    step("A turns", lambda i: (a.look(45.0, 0.0), a.tick_end()), ticks=2, bots=[b])
    step("B changes options: cape off, left-handed", tick(options), ticks=4, bots=[b])
    step("B disconnects", lambda i: (lock.leave(b), b.c.close()), ticks=1, settle=20)

    lock.close()
    with open(out_path, "w") as f:
        json.dump({"ids": {"A": a.entity_id, "B": b.entity_id}, "steps": steps}, f, indent=1)
    a.c.close()
    print(f"wrote {out_path}: A is entity {a.entity_id}, B is {b.entity_id}")


# ----------------------------------------------------------------------------- diff

SKIP = {"player_info_update", "player_info_remove", "bundle_delimiter"}


def _varint_at(h: str) -> tuple[int, int]:
    raw = bytes.fromhex(h)
    v = shift = i = 0
    while True:
        v |= (raw[i] & 0x7F) << shift
        shift += 7
        i += 1
        if not raw[i - 1] & 0x80:
            return v, i


def _about_bots(rec: dict, who: str) -> dict[str, list]:
    ids = rec["ids"]
    out = {}
    for st in rec["steps"]:
        rows = []
        for name, h in st.get(who, []):
            if name in SKIP or not h:
                continue
            body = h[2:] if name == "remove_entities" else h  # skip the count of ids
            eid, n = _varint_at(body)
            if eid in ids.values():
                rows.append((name, ("A" if eid == ids["A"] else "B") + ":" + body[2 * n:]))
        out[st["step"]] = rows
    return out


def diff(path_ref: str, path_ours: str) -> int:
    ref, ours = json.load(open(path_ref)), json.load(open(path_ours))
    different = 0
    for who in ("A", "B"):
        print(f"################ what {who} is told (left: {path_ref}, right: {path_ours})")
        left, right = _about_bots(ref, who), _about_bots(ours, who)
        for step in left:
            a, b = left[step], right.get(step, [])
            print(f"== {step}: {'same' if a == b else 'DIFFERENT'} ({len(a)} / {len(b)} packets)")
            if a == b:
                continue
            different += 1
            for i in range(max(len(a), len(b))):
                lhs = a[i] if i < len(a) else ("", "")
                rhs = b[i] if i < len(b) else ("", "")
                print(f"   {lhs[0]:21s}{lhs[1][:60]:60s} {' ' if lhs == rhs else '|'} {rhs[0]:21s}{rhs[1][:60]}")
    return different


def main() -> int:
    if len(sys.argv) >= 4 and sys.argv[1] == "record":
        if len(sys.argv) < 7:
            raise SystemExit("record <port> <out.json> <name A> <name B> <console>: the console keeps the servers in step - compare.py has one")
        record(int(sys.argv[2]), sys.argv[3], sys.argv[4], sys.argv[5], sys.argv[6])
        return 0
    if len(sys.argv) == 4 and sys.argv[1] == "diff":
        diff(sys.argv[2], sys.argv[3])
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
