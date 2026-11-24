#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""When does a server show a player's arm swinging? Record it, on vanilla and on ours, and diff.

    tools/vanilla/compare.py swing_probe                # both servers, both recordings, the diff
    swing_probe.py record <port> <out.json> <nameA> <nameB> <console>
    swing_probe.py diff <vanilla.json> <ours.json>

`record` joins two bots (tests/integration/mcproto.py, offline mode, creative): A watches, B does
what a client's left and right mouse buttons do - punches the air, punches again before and after
the first swing is half over, hits A, places blocks with either hand, clicks where nothing can be
placed, breaks the blocks again, drops items around a punch, punches and stabs with a spear,
empties buckets of water and lava beside itself and fills them again.
Every packet either of them receives about B's arm, about a block or as a sound is logged per
step with its bytes. `diff` lines two recordings up step by step.

A swing is the client's own prediction (Minecraft.startAttack / continueAttack, dropItem): the
server only passes it on to the others, and decides whether it counts as a new swing at all
(LivingEntity.SwingState.startIfAble). That decision depends on the ticks between two packets, so
the steps keep clear of its edges: two packets are sent in one write, two ticks apart or five.

The reference is the vanilla server, run under the rules in AGENTS.md (see entity_probe.py for the properties it needs; gamemode=creative and spawn-protection=0 matter here).
tools/vanilla/compare.py starts it and ours and hands `record` the console of each: the probe
keeps a server in step by it (pace.py), so a recording is the same script at the same ticks on
either server, and waits for no clock.

Reading the diff:
  * Use names vanilla has not seen. The blocks B places it breaks again and what it drops it
    picks up again, so the world is left as it was found.
  * Sounds are compared by event, source, volume and pitch; block updates by state; positions,
    entity ids and seeds differ by nature and are cut.
  * A vanilla flat world has animals in it: what happens to blocks further than four blocks
    from B (a sheep eating grass) and the sounds of anything but players and blocks are left out.
  * Known differences that remain are listed in docs/architecture.md ("Items", "Compared with
    vanilla"): what a hit does besides the swing, the spear.
"""
from __future__ import annotations

import json
import math
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tests" / "integration"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import mcproto as mc  # noqa: E402
from inventory_probe import Actor  # noqa: E402
from pace import Lockstep, console_of  # noqa: E402
from mcproto import varint  # noqa: E402

ARM = {"swing_animation", "entity_event", "animate", "hurt_animation", "damage_event", "set_equipment"}
WORLD = {"block_update", "section_blocks_update", "level_event", "block_destruction", "sound", "sound_entity"}
ITEMS = {"add_entity", "remove_entities", "take_item_entity"}
LOGGED = ARM | WORLD | ITEMS | {"container_set_content", "container_set_slot", "disconnect", "set_action_bar_text"}

# ServerboundPlayerActionPacket.Action
START_DESTROY, DROP_ONE, STAB = 0, 5, 8
UP = 1


class Hands(Actor):
    logged = LOGGED

    def __init__(self, port: int, name: str):
        super().__init__(port, name)
        self.sequence = 0

    def punch(self) -> None:
        self.c.send("punch")

    def attack(self, entity_id: int) -> None:
        self.c.send("attack", varint(entity_id))

    def use_on(self, pos: tuple, hand: int = 0, face: int = UP, cursor: tuple = (0.5, 1.0, 0.5)) -> None:
        self.sequence += 1
        self.c.send("use_item_on", varint(hand) + mc.pack_block_pos(*pos) + varint(face) + struct.pack(">fff", *cursor) +
                    b"\x00\x00" + varint(self.sequence))

    def use(self, hand: int = 0) -> None:
        self.sequence += 1
        self.c.send("use_item", varint(hand) + varint(self.sequence) + struct.pack(">ff", *self.rot))

    def destroy(self, pos: tuple) -> None:
        """What a creative client's left click on a block is: the block goes, the arm swings."""
        self.sequence += 1
        self.c.send("player_action", varint(START_DESTROY) + mc.pack_block_pos(*pos) + bytes([UP]) + varint(self.sequence))
        self.punch()


def record(port: int, out_path: str, name_a: str, name_b: str, console: str) -> None:
    """The game stands still all the while (pace.py). An arm is a player's, and players tick in a
    frozen game as in any - once a loop of the server, which is what a "tick" is here."""
    steps: list[dict] = []
    lock = Lockstep(console_of(console))
    a = Hands(port, name_a)
    a.begin(lock)
    lock.place((a,), "swing " + name_a + name_b)
    b = Hands(port, name_b)

    def step(name: str, actions=None, ticks: int = 8, b_alive: bool = True):
        """`actions`: what B does at the start of which client tick - one callable for
        tick 0, or {tick: callable}. Then log what arrived."""
        if callable(actions):
            actions = {0: actions}
        actions = actions or {}
        mark_a, mark_b = len(a.log), len(b.log)
        for i in range(ticks):
            if i in actions:
                actions[i]()
            if not b_alive:
                lock.leave(b)
            elif b in lock.bots:
                b.tick_end()
            lock.mark()
        lock.sync()
        steps.append({"step": name, "A": a.log[mark_a:], "B": b.log[mark_b:]})
        print(f"{name}: A {len(a.log) - mark_a}, B {len(b.log) - mark_b} packets")

    def join_b():
        b.begin(lock)
        lock.place((b,), "swing " + name_a + name_b)

    def stand_apart():
        target = (a.pos[0] + 6.0, a.pos[2])
        for _ in range(40):
            dx, dz = target[0] - b.pos[0], target[1] - b.pos[2]
            if abs(dx) < 1e-9 and abs(dz) < 1e-9:
                break
            b.move(dx=max(-2.0, min(2.0, dx)), dz=max(-2.0, min(2.0, dz)))
            b.tick_end()
            lock.mark()

    def ground(dx: int, dz: int) -> tuple:
        """The block B's feet would stand on, `dx`, `dz` blocks from the one they do."""
        return (math.floor(b.pos[0]) + dx, math.floor(b.pos[1]) - 1, math.floor(b.pos[2]) + dz)

    def above(pos: tuple) -> tuple:
        return (pos[0], pos[1] + 1, pos[2])

    step("B joins", join_b, ticks=20)
    step("B walks to six blocks from A", stand_apart)
    step("B looks straight down", lambda: b.look(0.0, 90.0))
    step("64 stone into hotbar slot 0", lambda: b.give(36, "kStone", 64))

    # ---- the left button
    step("a punch", b.punch)
    step("two punches in one tick", lambda: (b.punch(), b.punch()))
    step("a punch, another two ticks later: the first swing is not half over", {0: b.punch, 2: b.punch}, ticks=10)
    step("a punch, another five ticks later", {0: b.punch, 5: b.punch}, ticks=12)
    step("a punch, another twelve ticks later", {0: b.punch, 12: b.punch}, ticks=20)
    step("a hit on A: attack, then the punch", lambda: (b.attack(a.entity_id), b.punch()))

    # ---- the right button
    step("stone placed next to B", lambda: b.use_on(ground(1, 0)))
    step("the same click again: the place is taken", lambda: b.use_on(ground(1, 0)))
    step("8 dirt into the off hand, hotbar slot 1 (empty) in the main hand", lambda: (b.give(45, "kDirt", 8), b.select(1)))
    step("dirt placed with the off hand", lambda: b.use_on(ground(0, 1), hand=1))
    step("a click with the empty main hand", lambda: b.use_on(ground(0, -1), hand=0))
    step("a stick into slot 1; clicked on the ground, then used: not a block",
         lambda: (b.give(37, "kStick"), b.use_on(ground(-1, 0)), b.use()))
    step("the off hand emptied, slot 0 (stone) in hand", lambda: (b.give(45, None), b.select(0)))
    step("a click on a block that is out of reach", lambda: b.use_on(ground(40, 0)))

    # ---- breaking: a creative client says which block and punches
    step("the stone broken", lambda: b.destroy(above(ground(1, 0))))
    step("the dirt broken", lambda: b.destroy(above(ground(0, 1))))

    # ---- a drop swings too (LivingEntity.drop), by the same rule
    step("Q, then a punch in the same tick", lambda: (b.action(DROP_ONE), b.punch()), ticks=10)
    step("a punch, Q two ticks later: the item goes, the arm does not", {0: b.punch, 2: lambda: b.action(DROP_ONE)}, ticks=10)
    step("wait: the items come back", ticks=60)

    # ---- a spear has its own attack animation; a vanilla client stabs with it and never punches
    step("a wooden spear into slot 2, selected", lambda: (b.give(38, "kWoodenSpear"), b.select(2)), ticks=30)
    step("a punch with the spear", b.punch, ticks=16)
    step("a stab with the spear", lambda: b.action(STAB), ticks=30)
    step("slot 0 again", lambda: b.select(0))

    # Buckets (BucketItem.use): emptied where B looks, filled from what it looks at - the block
    # east of its feet, so that B itself stays dry (a player that gets into water splashes, at a
    # pitch that is chance). The game stands still: what is poured out stays where it is put.
    beside = (-90.0, 58.3)      # from B's eyes down onto the ground one block east
    step("buckets into slots 3, 4 and 5: water, empty, lava; B looks at the ground beside it",
         lambda: (b.give(39, "kWaterBucket"), b.give(40, "kBucket"), b.give(41, "kLavaBucket"), b.select(3), b.look(*beside)))
    step("the water bucket used: water on the ground beside B", b.use)
    step("the same again, into the source", b.use)
    step("the empty bucket: the water is taken", lambda: (b.select(4), b.use()))
    step("the empty bucket at the ground it left: nothing", b.use)
    step("the empty bucket into the air", lambda: (b.look(0.0, -90.0), b.use()))
    step("the lava bucket", lambda: (b.look(*beside), b.select(5), b.use()))
    step("the lava bucket out of the inventory; the empty one takes the lava, and a full one is put there",
         {0: lambda: b.give(41, None), 2: lambda: (b.select(4), b.use())}, ticks=10)
    step("a water bucket in the off hand, an empty main hand: used with the off hand",
         {0: lambda: (b.give(45, "kWaterBucket"), b.select(8)), 2: lambda: b.use(hand=1)}, ticks=10)
    step("...and taken up again; the off hand emptied, slot 0", {0: lambda: (b.select(4), b.use()), 3: lambda: (b.give(45, None), b.select(0))},
         ticks=10)
    step("B disconnects", lambda: b.c.close(), ticks=10, b_alive=False)

    lock.close()
    with open(out_path, "w") as f:
        json.dump({"ids": {"A": a.entity_id, "B": b.entity_id}, "at": {"A": a.pos, "B": b.pos}, "steps": steps}, f, indent=1)
    a.c.close()
    print(f"wrote {out_path}: A is entity {a.entity_id}, B is {b.entity_id}")


# ----------------------------------------------------------------------------- diff

SOURCE_BLOCKS, SOURCE_PLAYERS = 4, 7   # SoundSource ordinals


def _normalise(rec: dict, who: str) -> dict[str, list]:
    ids = {rec["ids"]["A"]: "A", rec["ids"]["B"]: "B"}
    item_type = mc.ENTITY_TYPE["kItem"]
    items = 0
    bx, _, bz = rec["at"]["B"]

    def near(pos: tuple) -> bool:
        return abs(pos[0] + 0.5 - bx) <= 4 and abs(pos[2] + 0.5 - bz) <= 4

    out = {}
    for st in rec["steps"]:
        rows = []
        for name, h in st.get(who, []):
            r = mc.Reader(bytes.fromhex(h))
            if name == "sound":
                holder, source = r.varint(), r.varint()
                r.take(12)
                if source in (SOURCE_BLOCKS, SOURCE_PLAYERS):
                    rows.append((name, f"event {holder - 1} source {source} volume {r.f32():g} pitch {r.f32():g}"))
            elif name == "block_update":
                pos = mc.unpack_block_pos(r.u64())
                if near(pos):
                    rows.append((name, f"state {r.varint()}"))
            elif name == "level_event":
                kind = r.i32()
                pos = mc.unpack_block_pos(r.u64())
                if near(pos):
                    rows.append((name, f"{kind} data {r.i32()}"))
            elif name == "entity_event":
                eid = r.i32()
                if eid in ids:
                    rows.append((name, f"{ids[eid]} {r.u8()}"))
            elif name == "add_entity":
                eid = r.varint()
                r.take(16)
                if r.varint() == item_type and eid not in ids:
                    items += 1
                    ids[eid] = f"item{items}"
                    rows.append((name, ids[eid]))
            elif name == "remove_entities":
                for eid in [r.varint() for _ in range(r.varint())]:
                    if ids.get(eid, "").startswith("item"):
                        rows.append((name, ids.pop(eid)))
            elif name == "take_item_entity":
                eid, collector = r.varint(), r.varint()
                if eid in ids:
                    rows.append((name, f"{ids[eid]} -> {ids.get(collector, collector)}"))
            elif name in ARM:
                eid = r.varint()
                if eid in ids:
                    rows.append((name, f"{ids[eid]} {r.d[r.i:].hex()}"))
            elif name in ("disconnect", "set_action_bar_text", "block_destruction", "section_blocks_update", "sound_entity"):
                rows.append((name, h[:40]))
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
                print(f"   {lhs[0]:19s}{lhs[1][:52]:52s} {' ' if lhs == rhs else '|'} {rhs[0]:19s}{rhs[1][:52]}")
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
