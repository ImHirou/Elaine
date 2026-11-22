#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""What does a server do with a chest, a furnace, a crafting grid and TNT - and with a smoker, a
blast furnace, a barrel and chests of copper? Record it, on vanilla and on ours, and diff.

    container_probe.py record <port> <out.json> [nameA nameB]
    container_probe.py diff <vanilla.json> <ours.json>

`record` joins two bots (tests/integration/mcproto.py, offline mode, creative): B acts, A stands
three blocks away and watches - and reaches into B's chest once. B places a chest and opens it,
moves stacks, the two have it open together, B makes it a double chest and breaks half of it;
crafts in its inventory's grid and at a crafting table; feeds a furnace and breaks it; feeds a
smoker and a blast furnace; fills a barrel, with A looking into it; makes pairs of copper chests of
different ages and waxes; lights a TNT beside a second one. Every packet either of them receives about a menu, a block, a block
event, a sound, an explosion, primed TNT or an item on the ground is logged per step. `diff`
lines two recordings up step by step.

The reference is the vanilla server, run under the rules in AGENTS.md (gamemode=creative,
level-type=flat, spawn-protection=0):

    cd .cache/vanilla-server && /usr/lib/jvm/java-25-openjdk/bin/java -jar ../mc/26.3/server.jar --nogui
    tools/vanilla/container_probe.py record 25577 /tmp/vanilla.json Cyd Dee     # then stop the server
    build/release/elaine --server-ip=127.0.0.1 --server-port=25580 --online-mode=false --view-distance=2 &
    tools/vanilla/container_probe.py record 25580 /tmp/ours.json Cyd Dee
    tools/vanilla/container_probe.py diff /tmp/vanilla.json /tmp/ours.json

Reading the diff:
  * Use names vanilla has not seen, and a spot it has not seen either: it saves players and the
    world, so a second recording finds the first one's crater - where nothing can be placed.
    Best a world of its own for every recording: start vanilla with `--world <a new name>`.
  * What is chance on one server is chance between two: a spilled stack falls into pieces of
    10 to 30, and a bot picks up what slides its way - from then on its inventory is another
    in every menu it is sent. Record each server twice and believe what differs in both pairs.
  * Positions are written relative to the block B stands on when it begins, so that the two
    servers' spawn points do not matter. Entity ids, UUIDs and sound seeds are cut.
  * What is random in vanilla is not compared value by value: the pitch of a chest's lid, which
    way a primed TNT hops (its height is compared, tick by tick), which blocks an explosion
    takes and what lies about afterwards (`diff ... -v` prints the counts). Where the TNT hops
    decides more than it seems: towards the second TNT it bumps into it, and from then on its
    position is sent exactly instead of in steps - on either server.
  * The steps are cut by the clock, not by the tick: a chest's five-tick recount can fall on
    either side of a step's end, and then two neighbouring steps differ by one block event each.
  * Sounds this server does not make at all (a block being placed, footsteps, the slimes of a
    flat world) are left out; one arm swing stands for vanilla's two per click.
  * Known differences are listed in docs/architecture.md ("Blocks that do something").
"""
from __future__ import annotations

import json
import struct
import sys
import time
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tests" / "integration"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import mcproto as mc  # noqa: E402
from inventory_probe import Actor, PICKUP, QUICK_MOVE  # noqa: E402
from pace import TICK, ticks  # noqa: E402
from pace import ticks as pace_ticks  # noqa: E402  (where a parameter is called ticks)
from mcproto import ITEM, varint  # noqa: E402

MENU = {"container_set_content", "container_set_slot", "set_cursor_item", "set_player_inventory", "container_close",
        "container_set_data", "open_screen"}
# What a chest of any kind and a barrel are heard as: at a pitch that is drawn.
LID_SOUNDS = {v for k, v in mc.SOUND.items() if k.startswith(("kBlockChest", "kBlockCopperChest", "kBlockBarrel"))}
WORLD = {"block_update", "section_blocks_update", "block_event", "sound", "explode", "block_entity_data", "level_event"}
ENTITY = {"add_entity", "set_entity_data", "remove_entities", "move_entity_pos", "move_entity_pos_rot", "entity_position_sync",
          "set_entity_motion", "swing_animation", "bundle_delimiter"}
TNT, ITEM_ENTITY = mc.ENTITY_TYPE["kTnt"], mc.ENTITY_TYPE["kItem"]


class Prober(Actor):
    logged = MENU | WORLD | ENTITY | {"disconnect", "update_recipes"}

    def __init__(self, port: int, name: str):
        super().__init__(port, name)
        self.window = 0   # container id of the screen that is open

    def pump(self, seconds: float) -> None:
        before = len(self.log)
        super().pump(seconds)
        for name, data in self.log[before:]:
            if name == "open_screen":
                self.window = mc.Reader(bytes.fromhex(data)).varint()
            elif name == "container_close":
                self.window = 0

    def use(self, pos: tuple, face: int = 1, hand: int = 0, cursor: tuple = (0.5, 1.0, 0.5)) -> None:
        self.seq = getattr(self, "seq", 0) + 1
        self.c.send("use_item_on", varint(hand) + mc.pack_block_pos(*pos) + varint(face) + struct.pack(">fff", *cursor) +
                    b"\x00\x00" + varint(self.seq))

    def dig(self, pos: tuple) -> None:
        self.seq = getattr(self, "seq", 0) + 1
        self.c.send("player_action", varint(0) + mc.pack_block_pos(*pos) + b"\x01" + varint(self.seq))

    def wclick(self, slot: int, button: int, kind: int) -> None:
        """container_click on the open screen, reporting no prediction."""
        body = varint(self.window) + varint(self.state_id) + struct.pack(">hb", slot, button) + varint(kind) + varint(0)
        self.c.send("container_click", body + mc.hashed_stack(None))
        self.tick_end()
        self.pump(ticks(2.4))

    def close(self) -> None:
        self.c.send("container_close", varint(self.window))
        self.window = 0


def record(port: int, out_path: str, name_a: str, name_b: str) -> None:
    steps: list[dict] = []
    a, b = Prober(port, name_a), Prober(port, name_b)
    for bot in (a, b):
        bot.join()
        bot.pump(ticks(30))
        bot.c.send("player_loaded")
        bot.tick_end()
        bot.pump(ticks(10))

    def step(name: str, action=None, ticks: int = 6):
        t0 = time.time()
        mark_a, mark_b = len(a.log), len(b.log)
        for i in range(ticks):
            if i == 0 and action:
                action()
            for bot in (b, a):
                bot.tick_end()
                bot.pump(0.001)
            time.sleep(max(0.0, t0 + (i + 1) * TICK - time.time()))
        a.pump(pace_ticks(3))
        b.pump(pace_ticks(1))
        steps.append({"step": name, "A": a.log[mark_a:], "B": b.log[mark_b:]})
        print(f"{name}: A {len(a.log) - mark_a}, B {len(b.log) - mark_b} packets")

    def walk(bot, x: float, z: float):
        for _ in range(60):
            dx, dz = x - bot.pos[0], z - bot.pos[2]
            if abs(dx) < 1e-9 and abs(dz) < 1e-9:
                return
            bot.move(dx=max(-2.0, min(2.0, dx)), dz=max(-2.0, min(2.0, dz)))
            for other in (a, b):
                other.tick_end()
                other.pump(ticks(0.6))

    # A spot of its own for every pair of names, in the chunk the bots spawned in or the next.
    spot = zlib.crc32((name_a + name_b).encode()) % 49
    ox = int(b.pos[0] // 16) * 16 + 2 + spot % 7 * 2
    oz = int(b.pos[2] // 16) * 16 + 1 + spot // 7 * 2
    oy = int(b.pos[1]) - 1      # the block they stand on
    walk(b, ox + 0.5, oz + 0.5)
    walk(a, ox + 3.5, oz + 0.5)
    for bot in (a, b):
        bot.look(0.0, 0.0)      # south: +z
    origin = (ox, oy, oz)

    def at(dx: int, dy: int, dz: int) -> tuple:
        return (ox + dx, oy + dy, oz + dz)

    # What the join brought, before anything was done: the recipe property sets among it.
    steps.append({"step": "joined", "A": [x for x in a.log if x[0] == "update_recipes"], "B": [x for x in b.log if x[0] == "update_recipes"]})
    step("settled", ticks=10)
    b.give(36, "kChest", 2), b.give(37, "kStone", 20), b.give(38, "kOakLog", 3), b.give(39, "kCraftingTable")
    b.give(40, "kFurnace"), b.give(41, "kCobblestone", 3), b.give(42, "kTnt", 2), b.give(43, "kFlintAndSteel")
    b.give(44, "kOakPlanks", 8)
    step("B fills its hotbar", ticks=4)

    # ---- a chest
    step("B places a chest two blocks ahead", lambda: (b.select(0), b.use(at(0, 0, 2))))
    step("B opens it", lambda: b.use(at(0, 1, 2)))
    step("B shift-clicks its stone into it", lambda: b.wclick(55, 0, QUICK_MOVE))
    step("A opens the same chest", lambda: a.use(at(0, 1, 2)))
    step("B takes half of the stone", lambda: b.wclick(0, 1, PICKUP))
    step("both keep it open for 12 ticks", ticks=12)
    step("B closes", b.close)
    step("A closes", a.close)
    # The chest becomes a double one while A has it open: what happens to A's screen, and whose lid is up.
    step("A opens the single chest again", lambda: a.use(at(0, 1, 2)))
    step("B places a second chest beside it", lambda: b.use(at(1, 0, 2)))
    step("A keeps the screen it has for 12 ticks", ticks=12)
    step("B opens the double chest", lambda: b.use(at(1, 1, 2)))
    step("A, still in its screen, takes the stone", lambda: a.wclick(0, 0, PICKUP))
    step("A puts it back and closes", lambda: (a.wclick(0, 0, PICKUP), a.close()))
    step("A opens it too", lambda: a.use(at(0, 1, 2)))
    step("A closes", a.close)
    step("A breaks the half with the stone in it", lambda: a.dig(at(0, 1, 2)), ticks=10)
    step("B breaks the other half", lambda: b.dig(at(1, 1, 2)))

    # The same with the second chest placed the other way: sneaking, against the side of the
    # one A has open (a click on a chest's side without the sneak key would open it).
    def sneak(bot, on: bool) -> None:
        bot.c.send("player_input", bytes([0x20 if on else 0]))   # the shift bit of player_input

    step("B places a chest further along", lambda: (b.give(36, "kChest", 4), b.select(0), b.use(at(3, 0, 2))))
    step("A opens that one", lambda: a.use(at(3, 1, 2)))
    step("B, sneaking, places another against its west side",
         lambda: (sneak(b, True), b.use(at(3, 1, 2), face=4, cursor=(0.0, 0.5, 0.5)), sneak(b, False)))
    step("A keeps its screen for 12 ticks more", ticks=12)
    step("A closes that screen", a.close)
    step("B opens the pair", lambda: b.use(at(2, 1, 2)))
    step("B closes it", b.close)
    step("B breaks one of the pair", lambda: b.dig(at(2, 1, 2)))
    step("B breaks the other", lambda: b.dig(at(3, 1, 2)))

    # ---- crafting
    step("B: a log into the inventory's grid", lambda: b.give(1, "kOakLog", 2))
    step("B takes the planks", lambda: b.click(0, 0, PICKUP))
    step("B puts them into the inventory", lambda: b.click(9, 0, PICKUP))
    step("B: the grid is emptied", lambda: b.give(1, None))
    step("B places a crafting table", lambda: (b.select(3), b.use(at(0, 0, 2))))
    step("B opens it", lambda: b.use(at(0, 1, 2)))
    step("B picks up its planks", lambda: b.wclick(45, 0, PICKUP))
    for slot in (1, 2, 4, 5):
        step(f"B puts one plank into cell {slot}", lambda slot=slot: b.wclick(slot, 1, PICKUP))
    step("B shift-clicks the result", lambda: b.wclick(0, 0, QUICK_MOVE))
    step("B puts planks into two cells and takes the sticks", lambda: (b.wclick(1, 1, PICKUP), b.wclick(4, 1, PICKUP), b.wclick(0, 0, PICKUP)))
    step("B puts a plank into the grid and closes", lambda: (b.wclick(5, 1, PICKUP), b.close()))
    step("B breaks the table", lambda: b.dig(at(0, 1, 2)))

    # ---- a furnace
    step("B places a furnace", lambda: (b.select(4), b.use(at(0, 0, 2))))
    step("B opens it", lambda: b.use(at(0, 1, 2)))
    step("B shift-clicks cobblestone, then a log, then planks", lambda: (b.wclick(35, 0, QUICK_MOVE), b.wclick(32, 0, QUICK_MOVE), b.wclick(38, 0, QUICK_MOVE)))
    step("it burns for 10 ticks", ticks=10)
    step("B takes the cobblestone out", lambda: b.wclick(0, 0, PICKUP))
    step("B breaks the furnace", lambda: b.dig(at(0, 1, 2)), ticks=10)

    # The rest happens ten blocks north of what was spilled so far, and a block further from B
    # than before: whether a bot picks up an item that slides its way is chance, and what it
    # has picked up is in every menu it is sent from then on.
    near = at
    walk(b, ox + 0.5, oz - 9.5)
    walk(a, ox + 3.5, oz - 9.5)
    for bot in (a, b):
        bot.look(0.0, 0.0)

    def at(dx: int, dy: int, dz: int) -> tuple:     # noqa: F811
        return (ox + dx, oy + dy, oz - 10 + dz + 1)
    step("the two walk north", ticks=10)

    # ---- a smoker and a blast furnace: each with what it cooks, what the other cooks, and coal
    for n, block in enumerate(("kSmoker", "kBlastFurnace")):
        what = "smoker" if n == 0 else "blast furnace"
        step(f"B empties its hotbar and takes a {what}", lambda block=block: (
            [b.give(slot, None) for slot in range(36, 45)], b.give(36, block), b.give(37, "kBeef", 3),
            b.give(38, "kIronOre", 3), b.give(39, "kCoal", 2)))
        step(f"B places the {what}", lambda: (b.select(0), b.use(at(0, 0, 2))))
        step(f"B opens the {what}", lambda: b.use(at(0, 1, 2)))
        step(f"B shift-clicks beef, ore and coal into the {what}",
             lambda: (b.wclick(31, 0, QUICK_MOVE), b.wclick(32, 0, QUICK_MOVE), b.wclick(33, 0, QUICK_MOVE)))
        step(f"the {what} burns for 10 ticks", ticks=10)
        step(f"B takes out of the {what} what cooks and puts it back", lambda: (b.wclick(0, 0, PICKUP), b.wclick(0, 0, PICKUP)))
        step(f"the {what} burns for 10 ticks more", ticks=10)
        step(f"B closes the {what}", b.close)
        step(f"B breaks the {what}", lambda: b.dig(at(0, 1, 2)), ticks=10)

    # ---- a barrel
    step("B takes barrels and stone", lambda: ([b.give(slot, None) for slot in range(36, 45)], b.give(36, "kBarrel", 2),
                                               b.give(37, "kStone", 20)))
    step("B places a barrel", lambda: (b.select(0), b.use(at(0, 0, 2))))
    step("B opens the barrel", lambda: b.use(at(0, 1, 2)))
    step("B shift-clicks its stone into the barrel", lambda: b.wclick(55, 0, QUICK_MOVE))
    step("A opens the barrel too", lambda: a.use(at(0, 1, 2)))
    step("both keep the barrel open for 12 ticks", ticks=12)
    step("B closes the barrel", b.close)
    step("A closes the barrel", a.close)
    step("B looks down and places a second barrel", lambda: (b.look(0.0, 90.0), b.use(at(1, 0, 2)), b.look(0.0, 0.0)))
    step("B opens the second barrel", lambda: b.use(at(1, 1, 2)))
    step("B closes the second barrel", b.close)
    step("B breaks the barrel with the stone in it", lambda: b.dig(at(0, 1, 2)), ticks=10)
    step("B breaks the other barrel", lambda: b.dig(at(1, 1, 2)))

    # ---- copper chests: an old one and a new one become a pair of the new block
    step("B takes copper chests", lambda: (
        [b.give(slot, None) for slot in range(36, 45)], b.give(36, "kOxidizedCopperChest", 2), b.give(37, "kCopperChest", 2),
        b.give(38, "kStone", 20), b.give(39, "kWaxedOxidizedCopperChest", 2), b.give(40, "kExposedCopperChest", 2),
        b.give(41, "kWaxedCopperChest", 2), b.give(42, "kChest", 2)))
    step("B places an oxidized copper chest", lambda: (b.select(0), b.use(at(0, 0, 2))))
    step("B opens the oxidized chest", lambda: b.use(at(0, 1, 2)))
    step("B shift-clicks stone into the oxidized chest", lambda: b.wclick(56, 0, QUICK_MOVE))
    step("B closes the oxidized chest", b.close)
    step("A opens the oxidized chest", lambda: a.use(at(0, 1, 2)))
    step("B places a new copper chest beside it", lambda: (b.select(1), b.use(at(1, 0, 2))))
    step("A keeps the copper screen for 12 ticks", ticks=12)
    step("A closes the copper screen", a.close)
    step("B opens the copper pair", lambda: b.use(at(1, 1, 2)))
    step("B closes the copper pair", b.close)
    step("B places a wooden chest beside the pair", lambda: (b.select(6), b.use(at(2, 0, 2))))
    step("B breaks the wooden chest", lambda: b.dig(at(2, 1, 2)))
    step("B breaks the copper half with the stone", lambda: b.dig(at(0, 1, 2)), ticks=10)
    step("B breaks the other copper half", lambda: b.dig(at(1, 1, 2)))
    step("B places a waxed oxidized copper chest", lambda: (b.select(3), b.use(at(0, 0, 2))))
    step("B places an exposed one beside the waxed", lambda: (b.select(4), b.use(at(1, 0, 2))))
    step("B breaks those two", lambda: (b.dig(at(0, 1, 2)), b.dig(at(1, 1, 2))))
    step("B places a waxed copper chest", lambda: (b.select(5), b.use(at(0, 0, 2))))
    step("B places an oxidized one beside the waxed", lambda: (b.select(0), b.use(at(1, 0, 2))))
    step("B breaks those two as well", lambda: (b.dig(at(0, 1, 2)), b.dig(at(1, 1, 2))))
    step("B places a waxed oxidized copper chest again", lambda: (b.select(3), b.use(at(0, 0, 2))))
    step("B places a waxed new one beside the waxed", lambda: (b.select(5), b.use(at(1, 0, 2))))
    step("B breaks the waxed pair", lambda: (b.dig(at(0, 1, 2)), b.dig(at(1, 1, 2))))
    step("B takes TNT again", lambda: ([b.give(slot, None) for slot in range(36, 45)], b.give(42, "kTnt", 2),
                                       b.give(43, "kFlintAndSteel")))
    walk(b, ox + 0.5, oz + 0.5)
    walk(a, ox + 3.5, oz + 0.5)
    for bot in (a, b):
        bot.look(0.0, 0.0)
    at = near
    step("the two walk back", ticks=10)

    # ---- TNT
    step("B places two TNT, four blocks ahead", lambda: (b.select(6), b.use(at(0, 0, 4)), b.use(at(1, 0, 4))))
    step("B lights the first", lambda: (b.select(7), b.use(at(0, 1, 4))), ticks=20)
    step("the fuse burns", ticks=70)
    step("after the first explosion", ticks=40)
    for bot in (a, b):
        bot.c.close()
    Path(out_path).write_text(json.dumps({"origin": origin, "ids": {"A": a.entity_id, "B": b.entity_id}, "steps": steps}))
    print(f"wrote {out_path}")


# ----------------------------------------------------------------------------- diff

def _rel(pos, origin) -> tuple:
    return tuple(p - o for p, o in zip(pos, origin))


def _normalise(rec: dict, who: str) -> dict[str, list]:
    origin = rec["origin"]
    kinds: dict[int, str] = {}     # entity id -> "tnt1", "item3"
    counts = {"tnt": 0, "item": 0}
    out = {}
    for st in rec["steps"]:
        rows = []
        blocks, items = {}, []
        for name, hexdata in st[who]:
            r = mc.Reader(bytes.fromhex(hexdata))
            if name in ("container_set_content", "container_set_slot", "set_cursor_item", "set_player_inventory",
                        "container_close", "container_set_data", "open_screen"):
                rows.append((name, hexdata))
            elif name == "block_update":
                pos, state = mc.parse_block_update(r)
                blocks[_rel(pos, origin)] = state
            elif name == "section_blocks_update":
                for pos, state in mc.parse_section_blocks_update(r).items():
                    blocks[_rel(pos, origin)] = state
            elif name == "block_event":
                pos = _rel(mc.unpack_block_pos(r.u64()), origin)
                rows.append((name, f"{pos} {r.u8()} {r.u8()} block {r.varint()}"))
            elif name == "sound":
                sound, source = r.varint() - 1, r.varint()
                pos = tuple(v / 8 - o for v, o in zip((r.i32(), r.i32(), r.i32()), origin))
                volume, pitch = r.f32(), r.f32()
                random_pitch = sound in LID_SOUNDS
                text = f"{sound} source {source} at {pos} volume {volume} pitch {'0.9..1' if random_pitch and 0.9 <= pitch < 1 else pitch}"
                # Only the sounds this server makes at all: a flat world is full of slimes, players
                # have footsteps, and a placed block is heard (docs/architecture.md lists those).
                ours = sound in mc.SOUND.values()
                rows.append((name if ours else "  (not compared)", text if ours else "sound " + text))
            elif name == "explode":
                c = (r.f64(), r.f64(), r.f64())
                radius, count = r.f32(), r.i32()
                knock = (r.f64(), r.f64(), r.f64()) if r.bool() else None
                rest = r.d[r.i:].hex()
                rows.append((name, f"y {c[1] - origin[1]:.5f} radius {radius} knock-back {'yes' if knock else 'no'} rest {rest}"))
                rows.append(("  (not compared)", f"{count} blocks, centre {tuple(round(v - o, 2) for v, o in zip(c, origin))}"))
            elif name == "add_entity":
                eid = r.varint()
                r.take(16)
                etype = r.varint()
                pos = (r.f64(), r.f64(), r.f64())
                if etype in (TNT, ITEM_ENTITY):
                    kind = "tnt" if etype == TNT else "item"
                    counts[kind] += 1
                    kinds[eid] = f"{kind}{counts[kind]}"
                    if etype == TNT:
                        rows.append((name, f"{kinds[eid]} at {_rel(pos, origin)}"))
            elif name == "set_entity_data":
                eid = r.varint()
                if eid not in kinds:
                    continue
                data = mc.read_entity_data(r)
                data.pop(mc.DATA["kNoGravity"], None)
                if kinds[eid].startswith("item"):
                    if mc.ITEM_DATA["kItem"] in data:
                        items.append(data[mc.ITEM_DATA["kItem"]][1][:2])
                elif data:
                    rows.append((name, f"{kinds[eid]} {sorted(data.items())}"))
            elif name in ("move_entity_pos", "move_entity_pos_rot"):
                eid = r.varint()
                if kinds.get(eid, "").startswith("tnt"):
                    props = r.u8()
                    if props & 2:
                        r.varint()
                    r.i16()
                    rows.append((name, f"{kinds[eid]} dy {r.i16()} ground {props & 1}"))
            elif name == "entity_position_sync":
                eid = r.varint()
                if kinds.get(eid, "").startswith("tnt"):
                    if r.u8():
                        r.u8()
                    r.f64()
                    rows.append((name, f"{kinds[eid]} y {r.f64() - origin[1]:.5f}"))
            elif name == "set_entity_motion":
                eid = r.varint()
                if kinds.get(eid, "").startswith("tnt"):
                    rows.append((name, f"{kinds[eid]} vy {mc.read_lp_vec3(r)[1]:.4f}"))
            elif name == "remove_entities":
                for eid in [r.varint() for _ in range(r.varint())]:
                    if kinds.get(eid, "").startswith("tnt"):
                        rows.append((name, kinds[eid]))
            elif name == "swing_animation":
                eid = r.varint()
                row = (name, f"{'A' if eid == rec['ids']['A'] else 'B' if eid == rec['ids']['B'] else '?'} {r.d[r.i:].hex()}")
                if not rows or rows[-1] != row:   # vanilla swings twice for one click (handleUseItemOn), ours once
                    rows.append(row)
            elif name == "update_recipes":
                sets = {}
                for _ in range(r.varint()):
                    key = r.str()
                    sets[key] = sorted(r.varint() for _ in range(r.varint()))
                rows.append((name, f"{sorted(sets)} {zlib.crc32(json.dumps(sets, sort_keys=True).encode()):08x}"))
                rows.append(("  (not compared)", f"{r.varint()} stonecutter recipes"))
            elif name == "level_event":   # a block breaking, as others see it
                rows.append((name, f"{r.i32()} at {_rel(mc.unpack_block_pos(r.u64()), origin)} data {r.i32()}"))
            elif name in ("block_entity_data", "disconnect"):
                rows.append((name, hexdata))
        exploded = any(n == "explode" for n, _ in rows)
        if blocks and not exploded:
            rows.append(("blocks", str(sorted(blocks.items()))))
        elif blocks:
            rows.append(("  (not compared)", f"{len(blocks)} blocks changed"))
        if items and not exploded:
            rows.append(("items on the ground", str(sorted(items))))
        elif items:
            rows.append(("  (not compared)", f"items on the ground: {sorted(items)}"))
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
            notes = [r[1] for r in x + y if r[0] == "  (not compared)"]
            x = [r for r in x if r[0] != "  (not compared)"]
            y = [r for r in y if r[0] != "  (not compared)"]
            print(f"== {step}: {'same' if x == y else 'DIFFERENT'} ({len(x)} / {len(y)} rows)")
            different += x != y
            for i in range(max(len(x), len(y)) if x != y else 0):
                lhs = x[i] if i < len(x) else ("", "")
                rhs = y[i] if i < len(y) else ("", "")
                print(f"   {lhs[0]:21s}{lhs[1][:90]:90s} {' ' if lhs == rhs else '|'} {rhs[0]:21s}{rhs[1][:90]}")
            if notes and "-v" in sys.argv:
                print("   not compared: " + "; ".join(notes)[:400])
    print(f"{different} steps differ")
    return different


def main() -> int:
    if len(sys.argv) >= 4 and sys.argv[1] == "record":
        names = (sys.argv[4], sys.argv[5]) if len(sys.argv) >= 6 else ("ProbeC", "ProbeD")
        record(int(sys.argv[2]), sys.argv[3], *names)
        return 0
    if len(sys.argv) >= 4 and sys.argv[1] == "diff":   # -v: also print what is not compared
        return 1 if diff(sys.argv[2], sys.argv[3]) else 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
