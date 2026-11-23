#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""What does a server do with a player's inventory? Record it, on vanilla and on ours, and diff.

    tools/vanilla/compare.py inventory_probe            # both servers, both recordings, the diff
    inventory_probe.py record <port> <out.json> <nameA> <nameB> <console>
    inventory_probe.py diff <vanilla.json> <ours.json>

`record` joins two bots (tests/integration/mcproto.py, offline mode, creative): A watches, B acts -
fills slots the way the creative inventory does, changes what it holds and wears, swaps hands,
drops items and picks them up again, clicks through its own menu with every kind of click, closes
it with items in the crafting grid, leaves. Every packet either of them receives about B, about
B's menu or about an item on the ground is logged per step with its bytes. `diff` lines two
recordings up step by step.

The reference is the vanilla server, run under the rules in AGENTS.md. tools/vanilla/compare.py
starts it and ours and hands `record` the console of each: the probe keeps a server in step by
it (pace.py) - the game stands still but for the ticks a step asks for - so a recording is the
same script at the same ticks on either server, and waits for no clock.
(See entity_probe.py for the properties vanilla needs; gamemode=creative matters here.)

How the clicks are sent: a real client predicts each click and reports what it expects every
touched slot to hold; a server then only corrects what it disagrees with. B reports nothing
(no changed slots, an empty cursor), so the server has to describe the whole outcome of every
click - which is the thing to compare. One step reports a correct prediction, to see silence.

Reading the diff:
  * Use names vanilla has not seen: it saves players, inventory included.
  * Vanilla throws a dropped item and lets it fall; ours hangs where it was dropped and says so
    (the NO_GRAVITY entry in its entity data). Item positions, velocities and movement packets
    are left out of the comparison; B looks straight down so that both land within reach.
  * Entity ids, UUIDs and sound seeds differ by nature and are replaced or cut.
  * Known differences that remain: vanilla shows a new entity to its viewers the moment it is
    created (ours at the end of the tick, so a swing and the item it threw swap places in the
    stream); its falling items merge after two ticks instead of forty.
"""
from __future__ import annotations

import json
import socket
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tests" / "integration"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import mcproto as mc  # noqa: E402
from entity_probe import Bot  # noqa: E402
from pace import Lockstep, console_of  # noqa: E402
from mcproto import ITEM, varint  # noqa: E402

MENU = {"container_set_content", "container_set_slot", "set_cursor_item", "set_player_inventory", "set_held_slot",
        "container_close", "container_set_data"}
ENTITY = {"add_entity", "set_entity_data", "set_equipment", "remove_entities", "take_item_entity", "swing_animation",
          "entity_event", "animate", "hurt_animation"}
MOVEMENT = {"move_entity_pos", "move_entity_pos_rot", "move_entity_rot", "entity_position_sync", "set_entity_motion",
            "rotate_head"}
LOGGED = MENU | ENTITY | MOVEMENT | {"sound", "bundle_delimiter", "disconnect"}

# ServerboundPlayerActionPacket.Action
DROP_ALL, DROP_ONE, SWAP_HANDS = 4, 5, 7
# ContainerInput
PICKUP, QUICK_MOVE, SWAP, CLONE, THROW, QUICK_CRAFT, PICKUP_ALL = range(7)
OUTSIDE = -999


class Actor(Bot):
    logged = LOGGED   # the packets a recording keeps; another probe may keep others

    def __init__(self, port: int, name: str):
        super().__init__(port, name)
        self.state_id = 0
        self.log: list[tuple[str, str]] = []   # every logged packet so far, (name, bytes as hex)

    def pump(self, seconds: float) -> None:
        """Take in everything that arrives within `seconds`; answer what a client must answer."""
        c = self.c
        out = self.log
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
            elif got in ("container_set_content", "container_set_slot"):
                r.varint()
                self.state_id = r.varint()   # what the next click has to echo
            if got == "disconnect":
                print(f"!! {self.name} was disconnected: {data.hex()}")
            if got in self.logged:
                out.append((got, data.hex()))
        c.sock.settimeout(10)

    # -- what a creative client sends
    def give(self, slot: int, item: str | None, count: int = 1, components: dict | None = None) -> None:
        stack = mc.item_stack(ITEM[item], count, components) if item else varint(0)
        self.c.send("set_creative_mode_slot", struct.pack(">h", slot) + stack)

    def action(self, action: int) -> None:
        self.c.send("player_action", varint(action) + mc.pack_block_pos(0, 0, 0) + b"\x00" + varint(0))

    def select(self, slot: int) -> None:
        self.c.send("set_carried_item", struct.pack(">h", slot))

    def click(self, slot: int, button: int, kind: int, changed: dict | None = None, carried=None, state: int | None = None) -> None:
        """container_click on the player's own menu, and its answer: the next click has to echo
        the state id it brings. `changed` / `carried`: the prediction to report."""
        changed = changed or {}
        body = varint(0) + varint(self.state_id if state is None else state) + struct.pack(">hb", slot, button) + varint(kind)
        body += varint(len(changed)) + b"".join(struct.pack(">h", s) + mc.hashed_stack(v) for s, v in changed.items())
        self.c.send("container_click", body + mc.hashed_stack(carried))
        self.lock.sync()

    def drag(self, kind: int, slots: list[int]) -> None:
        self.click(OUTSIDE, 0 | kind << 2, QUICK_CRAFT)
        for s in slots:
            self.click(s, 1 | kind << 2, QUICK_CRAFT)
        self.click(OUTSIDE, 2 | kind << 2, QUICK_CRAFT)


def record(port: int, out_path: str, name_a: str, name_b: str, console: str) -> None:
    steps: list[dict] = []
    lock = Lockstep(console_of(console))    # the game stands still but for the ticks of a step (pace.py)
    a = Actor(port, name_a)
    a.begin(lock)
    lock.place((a,), "inventory " + name_a + name_b)
    b = Actor(port, name_b)

    def step(name: str, action=None, ticks: int = 6, b_alive: bool = True):
        """Run `action`, let the game take `ticks` ticks, then log what arrived."""
        mark_a, mark_b = len(a.log), len(b.log)
        if action:
            action()
        if not b_alive:
            lock.leave(b)
        lock.step(ticks)
        steps.append({"step": name, "A": a.log[mark_a:], "B": b.log[mark_b:]})
        print(f"{name}: A {len(a.log) - mark_a}, B {len(b.log) - mark_b} packets")

    def join_b():
        b.begin(lock)
        lock.place((b,), "inventory " + name_a + name_b)

    def stand_apart():
        # Six blocks from A: in sight, out of reach of what B drops. (Vanilla spreads players
        # around the spawn point, ours puts them on it; either way B walks, two blocks a tick.)
        target = (a.pos[0] + 6.0, a.pos[2])
        for _ in range(40):
            dx, dz = target[0] - b.pos[0], target[1] - b.pos[2]
            if abs(dx) < 1e-9 and abs(dz) < 1e-9:
                break
            b.move(dx=max(-2.0, min(2.0, dx)), dz=max(-2.0, min(2.0, dz)))
            b.tick_end()
            lock.mark()     # (one move a loop of the server: more of them at once are too fast for vanilla)

    step("B joins", join_b, ticks=20)
    step("B walks to six blocks from A", stand_apart)
    step("B looks straight down", lambda: b.look(0.0, 90.0))

    # ---- the creative inventory: slots, what is held, what is worn
    step("64 stone into hotbar slot 0", lambda: b.give(36, "kStone", 64))
    step("a diamond helmet onto the head", lambda: b.give(5, "kDiamondHelmet"))
    step("stone onto the chest (the creative inventory may)", lambda: b.give(6, "kStone", 2))
    step("an elytra onto the chest instead", lambda: b.give(6, "kElytra"))
    step("a shield into the off hand", lambda: b.give(45, "kShield"))
    step("select hotbar slot 1 (empty)", lambda: b.select(1))
    step("select hotbar slot 0", lambda: b.select(0))
    step("65 stone: more than a stack", lambda: b.give(37, "kStone", 65))
    step("swap hands", lambda: b.action(SWAP_HANDS))
    step("swap hands back", lambda: b.action(SWAP_HANDS))
    step("swap hands twice in one tick", lambda: (b.action(SWAP_HANDS), b.action(SWAP_HANDS)))
    step("the helmet off", lambda: b.give(5, None))

    # ---- dropping and picking up
    step("Q: drop one", lambda: b.action(DROP_ONE), ticks=8)
    step("Q again at once, twice", lambda: (b.action(DROP_ONE), b.action(DROP_ONE)), ticks=8)
    step("ctrl+Q: drop the stack", lambda: b.action(DROP_ALL), ticks=8)
    step("Q with an empty hand", lambda: b.action(DROP_ONE))
    step("wait: the items come back", ticks=60)
    step("3 dirt thrown out of the creative inventory", lambda: b.give(-1, "kDirt", 3), ticks=8)
    step("wait: it comes back", ticks=60)

    # ---- clicks on the player's own menu
    def setup():
        for slot in range(9, 45):
            b.give(slot, None)
        b.give(9, "kStone", 10)
        b.give(10, "kDirt", 5)
        b.give(11, "kStone", 64)
        b.give(12, "kStone", 7)
        b.give(13, "kIronChestplate")
        b.give(14, "kCarvedPumpkin", 4)
        b.give(6, None)
        b.give(45, None)
    step("clear, then a few stacks to click on", setup)
    step("left click 9: pick up 10 stone", lambda: b.click(9, 0, PICKUP))
    step("right click 20: put one down", lambda: b.click(20, 1, PICKUP))
    step("left click 20: put the rest onto it", lambda: b.click(20, 0, PICKUP))
    step("right click 20: take the larger half", lambda: b.click(20, 1, PICKUP))
    step("left click 10: trade with the dirt", lambda: b.click(10, 0, PICKUP))
    step("left click 5: dirt is no helmet", lambda: b.click(5, 0, PICKUP))
    step("left click outside: drop the cursor", lambda: b.click(OUTSIDE, 0, PICKUP), ticks=8)
    step("left click 11, then right click outside: drop one of 64",
         lambda: (b.click(11, 0, PICKUP), b.click(OUTSIDE, 1, PICKUP)), ticks=8)
    step("left click 12: 63 onto 7", lambda: b.click(12, 0, PICKUP))
    step("left click 30: put the rest away", lambda: b.click(30, 0, PICKUP))
    step("shift click 13: the chestplate goes on", lambda: b.click(13, 0, QUICK_MOVE))
    step("shift click 6: and off again", lambda: b.click(6, 0, QUICK_MOVE))
    step("shift click 14: pumpkins are worn one at a time", lambda: b.click(14, 0, QUICK_MOVE))
    step("shift click 12: a stack to the hotbar", lambda: b.click(12, 0, QUICK_MOVE))
    step("shift click 36: and back up", lambda: b.click(36, 0, QUICK_MOVE))
    step("key 3 over 10: into hotbar slot 2", lambda: b.click(10, 2, SWAP))
    step("key 3 over 30: swap with it", lambda: b.click(30, 2, SWAP))
    step("F over 30: into the off hand", lambda: b.click(30, 40, SWAP))
    step("key 3 over 5: the head slot refuses", lambda: b.click(5, 2, SWAP))
    step("pick up 20, drag left over 21 22 23", lambda: (b.click(20, 0, PICKUP), b.drag(0, [21, 22, 23])))
    step("drag right over 21 24", lambda: b.drag(1, [21, 24]))
    step("drag right over 25 only", lambda: b.drag(1, [25]))
    step("middle drag over 26 27 (creative)", lambda: b.drag(2, [26, 27]))
    step("pick up 21, double click 33: gather", lambda: (b.click(21, 0, PICKUP), b.click(33, 0, PICKUP_ALL)))
    step("left click 33: put it down", lambda: b.click(33, 0, PICKUP))
    step("Q over 26: throw one", lambda: b.click(26, 0, THROW), ticks=8)
    step("ctrl+Q over 26: throw the stack", lambda: b.click(26, 1, THROW), ticks=8)
    step("middle click 27: clone (creative)", lambda: b.click(27, 2, CLONE))
    step("left click 34: put the clone down", lambda: b.click(34, 0, PICKUP))
    step("a click with a stale state id", lambda: b.click(34, 0, PICKUP, state=(b.state_id + 100) & 32767))
    step("left click 35 with the prediction reported: silence",
         lambda: b.click(35, 0, PICKUP, changed={35: (ITEM["kStone"], 64)}, carried=None))
    step("a click on a menu that is not open", lambda: b.c.send(
        "container_click", varint(3) + varint(b.state_id) + struct.pack(">hb", 35, 0) + varint(PICKUP) + varint(0) + b"\x00"))
    step("wait: what was thrown comes back", ticks=60)

    # ---- closing the menu
    def grid():
        b.give(1, "kStone", 3)
        b.give(2, "kDirt", 2)
        b.give(4, "kStone", 62)
        b.click(35, 0, PICKUP)
    step("the crafting grid filled, something on the cursor", grid)
    step("close the menu", lambda: b.c.send("container_close", varint(0)))

    # ---- pick block (the middle click): the server finds the item and says which slot is in hand
    def pick(dy: int) -> None:
        import math
        pos = (math.floor(b.pos[0]), math.floor(b.pos[1]) + dy, math.floor(b.pos[2]))
        b.c.send("pick_item_from_block", mc.pack_block_pos(*pos) + b"\x00")
    step("pick the grass underfoot: not in the inventory, creative makes one", lambda: pick(-1))
    step("pick it again: it is in hand", lambda: pick(-1))
    step("5 dirt into slot 20, then pick the dirt below the grass: fetched from the back", lambda: (b.give(20, "kDirt", 5), pick(-2)))
    step("pick the air", lambda: pick(1))

    # ---- components
    potion = {"kPotionContents": b"\x01" + varint(7) + b"\x00" + varint(0) + b"\x00"}
    step("a potion into hotbar slot 3", lambda: b.give(39, "kPotion", 1, potion))
    step("select it", lambda: b.select(3))
    step("Q: drop it", lambda: b.action(DROP_ONE), ticks=8)
    step("wait: it comes back (nothing is left lying for the next run)", ticks=60)
    step("B disconnects", lambda: b.c.close(), ticks=10, b_alive=False)

    lock.close()
    with open(out_path, "w") as f:
        json.dump({"ids": {"A": a.entity_id, "B": b.entity_id}, "steps": steps}, f, indent=1)
    a.c.close()
    print(f"wrote {out_path}: A is entity {a.entity_id}, B is {b.entity_id}")


# ----------------------------------------------------------------------------- diff

def _normalise(rec: dict, who: str) -> dict[str, list]:
    """Per step, the packets `who` got that are about the bots, their menus and items on the
    ground - ids replaced by names, what differs by nature removed."""
    ids = {rec["ids"]["A"]: "A", rec["ids"]["B"]: "B"}
    items_seen = 0
    item_type = mc.ENTITY_TYPE["kItem"]
    out = {}
    for st in rec["steps"]:
        rows = []
        for name, h in st.get(who, []):
            r = mc.Reader(bytes.fromhex(h))
            if name in MENU:
                rows.append((name, h))
            elif name == "sound":
                holder, source = r.varint(), r.varint()
                r.take(12)
                volume, pitch = r.f32(), r.f32()
                # Of all the sounds of a vanilla world (animals, footsteps - which ours does not
                # make yet) only those of a player at full volume: equipment going on.
                if source == 7 and volume == 1.0:
                    rows.append((name, f"sound {holder - 1} pitch {pitch}"))
            elif name == "add_entity":
                eid = r.varint()
                r.take(16)
                if r.varint() == item_type and eid not in ids:
                    items_seen += 1
                    ids[eid] = f"item{items_seen}"
                    rows.append((name, ids[eid]))
                elif eid in ids:
                    rows.append((name, ids[eid]))
            elif name == "remove_entities":
                for eid in [r.varint() for _ in range(r.varint())]:
                    if eid in ids:
                        rows.append((name, ids[eid]))
                        if ids[eid].startswith("item"):   # ours hands the id out again; a name is for one item
                            del ids[eid]
            elif name == "entity_event":
                eid = r.i32()
                if eid in ids:
                    rows.append((name, f"{ids[eid]} {r.u8()}"))
            elif name == "take_item_entity":
                eid, collector = r.varint(), r.varint()
                if eid in ids:
                    rows.append((name, f"{ids[eid]} -> {ids.get(collector, collector)} x {r.varint()}"))
            elif name == "set_entity_data":
                eid = r.varint()
                if eid not in ids or not ids[eid].startswith("item"):
                    continue   # the bots' own data is entity_probe's subject
                data = mc.read_entity_data(r)
                data.pop(mc.DATA["kNoGravity"], None)   # ours hang in the air, and say so
                if data:
                    rows.append((name, f"{ids[eid]} {sorted(data.items())}"))
            elif name in ENTITY:
                eid = r.varint()
                if eid in ids:
                    rows.append((name, f"{ids[eid]} {r.d[r.i:].hex()}"))
        out[st["step"]] = rows
    return out


def diff(path_ref: str, path_ours: str) -> int:
    ref, ours = json.load(open(path_ref)), json.load(open(path_ours))
    different = 0
    for who in ("A", "B"):
        print(f"################ what {who} is told (left: {path_ref}, right: {path_ours})")
        left, right = _normalise(ref, who), _normalise(ours, who)
        for step in left:
            x, y = left[step], right.get(step, [])
            print(f"== {step}: {'same' if x == y else 'DIFFERENT'} ({len(x)} / {len(y)} packets)")
            if x == y:
                continue
            different += 1
            for i in range(max(len(x), len(y))):
                lhs = x[i] if i < len(x) else ("", "")
                rhs = y[i] if i < len(y) else ("", "")
                print(f"   {lhs[0]:21s}{lhs[1][:70]:70s} {' ' if lhs == rhs else '|'} {rhs[0]:21s}{rhs[1][:70]}")
    print(f"{different} steps differ")
    return different


def main() -> int:
    if len(sys.argv) >= 4 and sys.argv[1] == "record":
        if len(sys.argv) < 7:
            raise SystemExit("record <port> <out.json> <name A> <name B> <console>: the console keeps the servers in step - compare.py has one")
        record(int(sys.argv[2]), sys.argv[3], sys.argv[4], sys.argv[5], sys.argv[6])
        return 0
    if len(sys.argv) == 4 and sys.argv[1] == "diff":
        return 1 if diff(sys.argv[2], sys.argv[3]) else 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
