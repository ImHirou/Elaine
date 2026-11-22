#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""What does a server do when a block changes beside another, when a scheduled tick comes due,
when a random tick falls? Record it, on vanilla and on ours, and diff.

    block_probe.py vanilla <out.json> [nameA nameB]         # runs the vanilla server itself
    block_probe.py ours <elaine> <out.json> [nameA nameB]   # runs ours
    block_probe.py record <port> <out.json> [nameA nameB [random]]   # a server that runs already
    block_probe.py diff <vanilla.json> <ours.json> [-v]

A recording is of two bots (tests/integration/mcproto.py, offline mode, creative): B builds, A
stands three blocks away and watches. Torches on a stone and on its sides, and the stone taken
away; torches placed under a block, looking level and looking down; a redstone lamp, a block of
redstone beside it, and the block gone; amethyst on a stone that goes; `/setblock` and `/fill`
from the console - destroying, sand into the air and forty blocks up, a torch into the air and
the block under it, a lamp beside a block of redstone; sand on a stone that goes, sand and
concrete powder and gravel placed in the air, gravel onto a torch. Then the random ticks, at 4096 a
section instead of 3: grass taking bare dirt and dying under a stone, copper weathering,
amethyst budding, nylium dying, copper stairs weathering as the stairs they are - and grass by the
time of day: the clock is stopped and set from the console to either side of the two ticks of the day
at which the sky gives grass light enough to spread. Then the blocks whose state is their shape:
stairs that turn corners and stop turning them, slabs by the half that is clicked and two halves
that become one block, fences, panes and walls that reach for what stands beside them and let
go of it, a wall under a block, a torch and a wall, lanterns that hang and stand and fall, chains,
end rods, glazed terracotta, mushroom blocks - by hand, and by `/setblock` and `/fill`. Then
plants: a poppy, a dead bush, an azalea and a cactus flower on what holds them and on what does
not, leaf litter and petals up to four to a block, what gives way to a stone that is set into it,
the ground dug from under them; barrels by where their player looks. Last, TNT on five sides of a
block of redstone.

`vanilla` starts the reference server under the rules in AGENTS.md (from .cache/vanilla-server/,
the cached jar, Java 25; gamemode=creative, level-type=flat, spawn-protection=0,
simulation-distance=2) with `--world block-probe` - a world of this probe's own, made anew every
time, so that nothing an earlier recording built or blew up is in the way and the other probes'
world stays as it is. It sets the game rule from the console for the steps that are about random
ticks and puts it back. `ours` starts this server on a port of its own with
`--random-tick-speed=4096` - it has no command for it - and a world it does not keep.

Every packet either bot receives about a block, a level event, a falling block, primed TNT or an
item on the ground is logged per step, with the moment it arrived. `diff` lines two recordings
up step by step; where the order and the timing are the point - which of four torches breaks
first, how many ticks after its signal a lamp goes out, how long sand takes to become an entity
and to land - both are compared: the events of a step in the order they came, and for each how
many ticks after the step's first it came.

Reading the diff:
  * Positions are relative to the block B stands on; entity ids, UUIDs and seeds are cut.
  * A tick is 50 ms, and what arrives is stamped a few ms late: an offset of n ticks is what
    rounds to n, counted from the first thing of a step that a tick sends (block changes,
    entities). Level events and sounds are compared by their order alone: vanilla sends them
    the moment they happen - between two ticks when a packet was the cause - and this server
    with the tick. Random ticks have no timing to compare - a step waits until what it is about
    has happened, and what is compared is that it did.
  * Items are compared as they were dropped; two that lie close enough become one stack,
    which is chance.
  * Grass that dies under a block, and dirt that grass takes, is a random tick whenever it
    happens: in the steps that are timed, a block that becomes grass or dirt is left out. (On
    ours that is within a second, the random ticks being what they are for the whole run.)
  * Vanilla shows an entity the moment it is made, this server with the tick's other entities -
    after that tick's block changes - and each walks its entities in an order of its own
    (vanilla's is a hash map's). So three things are compared of a step, each in the order it
    came and none against another: what is said about blocks, which entities appear, and what
    is said of each entity.
  * The chunks whose blocks changed in a tick are told one after another, in an order that is
    nobody's rule: vanilla walks a hash set of chunk holders (by identity), this server sorts
    them. Where a step's changes lie in two chunks, each chunk's are compared in their order.
  * Sounds this server does not make (a block being placed, footsteps) are left out; so is
    where an item that dropped flies. What an explosion takes and throws about is chance, and
    container_probe.py's subject: once the fuses burn, the explosions are counted.
  * Vanilla's world has no mobs in it (`spawn_mobs` is off: a sheep eats grass, with a level
    event of its own).
  * Known differences are listed in docs/architecture.md ("Block updates").
"""
from __future__ import annotations

import json
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tests" / "integration"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import mcproto as mc  # noqa: E402
from container_probe import Prober  # noqa: E402
from world_probe import PORT, SERVER_DIR, Vanilla  # noqa: E402
import pace  # noqa: E402
from pace import TICK  # noqa: E402

PROBE_WORLD = "block-probe"     # in .cache/vanilla-server/: this probe's, removed and made anew by `vanilla`
RANDOM_TICK_SPEED = 4096        # every cell of a section, on average, once a tick

WORLD = {"block_update", "section_blocks_update", "level_event", "add_transient_block", "sound", "explode"}
ENTITY = {"add_entity", "set_entity_data", "remove_entities", "bundle_delimiter", "move_entity_pos", "move_entity_pos_rot",
          "entity_position_sync", "set_entity_motion", "teleport_entity"}
TNT, ITEM_ENTITY, FALLING = mc.ENTITY_TYPE["kTnt"], mc.ENTITY_TYPE["kItem"], mc.ENTITY_TYPE["kFallingBlock"]


class Watcher(Prober):
    logged = WORLD | ENTITY | {"disconnect", "system_chat"}

    def __init__(self, port: int, name: str):
        super().__init__(port, name)
        self.stamps: list[float] = []   # when each logged packet arrived

    def pump(self, seconds: float) -> None:
        super().pump(seconds)
        now = time.time()
        self.stamps += [now] * (len(self.log) - len(self.stamps))


def record(port: int, out_path: str, name_a: str, name_b: str, random_ticks=None, console=None) -> None:
    """`random_ticks`: None - the steps about random ticks are left out; otherwise a function
    that is called with True before them and with False after (the game rule, where there is one).
    `console`: types a line on the server's console; without one the steps that need it are left out."""
    steps: list[dict] = []
    a, b = Watcher(port, name_a), Watcher(port, name_b)
    for bot in (a, b):
        bot.join()
        bot.pump(pace.ticks(30))
        bot.c.send("player_loaded")
        bot.tick_end()
        bot.pump(pace.ticks(10))

    def step(name: str, action=None, ticks: int = 8, until=None, about=None, loose=False):
        """`until(blocks)`: go on (at most `ticks`) until the block changes B has seen in this
        step satisfy it. `about`: the places a step about random ticks is about - what becomes
        of them is what is compared. `loose`: a step of explosions - they are counted."""
        mark_a, mark_b = len(a.log), len(b.log)
        t0 = time.time()
        for i in range(ticks):
            if i == 0 and action:
                action()
            for bot in (b, a):
                bot.tick_end()
            end = t0 + (i + 1) * TICK
            while time.time() < end:    # short turns: a packet is stamped within a few ms of its arrival
                for bot in (b, a):
                    bot.pump(min(0.003, TICK / 10))
            if until and until(_blocks(b.log[mark_b:])):
                break
        else:
            if until:
                print(f"!! {name}: it did not happen within {ticks} ticks")
        for bot in (a, b):
            bot.pump(pace.ticks(0.4))
        steps.append({"step": name, "about": [list(p) for p in about] if about is not None else None, "loose": loose,
                      "A": [(n, d, round((t - t0) * 1000)) for (n, d), t in zip(a.log[mark_a:], a.stamps[mark_a:])],
                      "B": [(n, d, round((t - t0) * 1000)) for (n, d), t in zip(b.log[mark_b:], b.stamps[mark_b:])]})
        print(f"{name}: A {len(a.log) - mark_a}, B {len(b.log) - mark_b} packets")

    def walk(bot, x: float, z: float):
        for _ in range(60):
            dx, dz = x - bot.pos[0], z - bot.pos[2]
            if abs(dx) < 1e-9 and abs(dz) < 1e-9:
                return
            bot.move(dx=max(-2.0, min(2.0, dx)), dz=max(-2.0, min(2.0, dz)))
            for other in (a, b):
                other.tick_end()
                other.pump(pace.ticks(0.6))

    def idle(ticks: int) -> None:
        for _ in range(ticks):
            for bot in (a, b):
                bot.tick_end()
                bot.pump(pace.ticks(0.5))

    # A spot of its own for every pair of names, in the chunk the bots spawned in or the next.
    spot = zlib.crc32((name_a + name_b).encode()) % 16
    ox = int(b.pos[0] // 16) * 16 + 1 + spot % 4 * 4
    oz = int(b.pos[2] // 16) * 16 + 1 + spot // 4 * 4
    oy = int(b.pos[1]) - 1      # the block they stand on
    origin = (ox, oy, oz)

    def place(n: int):
        """Both walk to the n-th place - each eight blocks east of the last, B on its corner and A
        three blocks off - and what is built there is counted from that corner."""
        walk(b, ox + 8 * n + 0.5, oz + 0.5)
        walk(a, ox + 8 * n + 3.5, oz - 1.5)
        for bot in (a, b):
            bot.look(0.0, 0.0)      # south: +z
        idle(4)
        return lambda dx, dy, dz: (ox + 8 * n + dx, oy + dy, oz + dz)

    def hotbar(*items: str) -> None:
        for slot, item in enumerate(items):
            b.give(36 + slot, item, 64)
        idle(3)

    NORTH, SOUTH, WEST, EAST = 2, 3, 4, 5       # Direction's ids: the face of a block that is clicked
    side = {NORTH: (0.5, 0.5, 0.0), SOUTH: (0.5, 0.5, 1.0), WEST: (0.0, 0.5, 0.5), EAST: (1.0, 0.5, 0.5)}

    def against(pos: tuple, face: int) -> None:
        b.use(pos, face=face, cursor=side[face])

    # ---- torches: what holds them, and the order they break in
    at = place(0)
    step("settled", ticks=10)
    hotbar("kStone", "kTorch", "kSoulTorch", "kCopperTorch", "kRedstoneLamp", "kRedstoneBlock", "kAmethystCluster",
           "kLargeAmethystBud", "kTnt")
    step("B builds a pillar of two stones", lambda: (b.select(0), b.use(at(0, 0, 2)), b.use(at(0, 1, 2))))
    step("B puts a torch on top of it", lambda: (b.select(1), b.use(at(0, 2, 2))))
    step("...and on its north, east and west sides",
         lambda: (against(at(0, 2, 2), NORTH), against(at(0, 2, 2), EAST), against(at(0, 2, 2), WEST)))
    step("B puts a torch on a torch", lambda: b.use(at(0, 3, 2)))
    step("B puts a soul torch on the south side, and a copper torch on the east of the lower stone",
         lambda: (b.select(2), against(at(0, 2, 2), SOUTH), b.select(3), against(at(0, 1, 2), EAST)))
    step("B breaks the stone the five are on", lambda: b.dig(at(0, 2, 2)), ticks=12)
    step("B breaks the stone under it", lambda: b.dig(at(0, 1, 2)), ticks=12)
    # Under a block, where a torch cannot hang: by where its player looks.
    step("B builds two more pillars, each with a stone to its south",
         lambda: (b.select(0), b.use(at(-1, 0, 1)), b.use(at(-1, 1, 1)), b.use(at(-1, 0, 2)),
                  b.use(at(1, 0, 1)), b.use(at(1, 1, 1)), b.use(at(1, 0, 2))))
    step("B takes the lower stone out of each", lambda: (b.dig(at(-1, 1, 1)), b.dig(at(1, 1, 1))))
    step("B, looking level, puts a torch under the first",
         lambda: (b.select(1), b.look(0.0, 0.0), b.use(at(-1, 2, 1), face=0, cursor=(0.5, 0.0, 0.5))))
    step("B, looking down, puts a torch under the second",
         lambda: (b.look(0.0, 90.0), b.use(at(1, 2, 1), face=0, cursor=(0.5, 0.0, 0.5)), b.look(0.0, 0.0)))
    step("B takes all of it away", lambda: [b.dig(at(*p)) for p in ((-1, 2, 1), (1, 2, 1), (-1, 1, 2), (1, 1, 2), (1, 1, 1))], ticks=12)

    # ---- a lamp: a neighbour update, and a tick four later
    step("B places a redstone lamp", lambda: (b.select(4), b.use(at(0, 0, 3))))
    step("B places a block of redstone beside it", lambda: (b.select(5), b.use(at(1, 0, 3))))
    step("B places a second lamp on the other side", lambda: (b.select(4), b.use(at(2, 0, 3))))
    step("B breaks the block of redstone", lambda: b.dig(at(1, 1, 3)), ticks=12)
    step("B puts it back, breaks it and puts it back at once",
         lambda: (b.select(5), b.use(at(1, 0, 3)), b.dig(at(1, 1, 3)), b.use(at(1, 0, 3))), ticks=12)
    step("B breaks it, and puts it back two ticks later", lambda: b.dig(at(1, 1, 3)), ticks=2)
    step("...now", lambda: b.use(at(1, 0, 3)), ticks=12)
    step("B takes the lamps and the block away", lambda: [b.dig(at(*p)) for p in ((1, 1, 3), (0, 1, 3), (2, 1, 3))], ticks=12)

    # ---- amethyst: a cluster and a bud go with what they grow on
    step("B places a stone, a cluster on it and a bud on its west side",
         lambda: (b.select(0), b.use(at(-2, 0, 3)), b.select(6), b.use(at(-2, 1, 3)), b.select(7), against(at(-2, 1, 3), WEST)))
    step("B breaks the stone", lambda: b.dig(at(-2, 1, 3)), ticks=12)

    # ---- commands: a block a command destroys breaks like any other, one it sets is placed
    if console:
        def setblock(pos: tuple, what: str) -> None:
            console(f"setblock {pos[0]} {pos[1]} {pos[2]} {what}")

        step("B builds a stone with a torch on it", lambda: (b.select(0), b.use(at(2, 0, 5)), b.select(1), b.use(at(2, 1, 5))))
        step("the console destroys the stone", lambda: setblock(at(2, 1, 5), "minecraft:air destroy"), ticks=12)
        step("the console sets sand in the air", lambda: setblock(at(4, 3, 5), "minecraft:sand"), ticks=24)
        # A long fall: what its viewers are told on the way (a client moves a falling block by itself).
        step("the console sets sand forty blocks up", lambda: setblock(at(5, 41, 6), "minecraft:sand"), ticks=80)
        step("the console sets a torch in the air", lambda: setblock(at(3, 3, 6), "minecraft:torch"), ticks=10)
        step("the console sets a stone under it", lambda: setblock(at(3, 2, 6), "minecraft:stone"), ticks=10)
        step("...and air in its place", lambda: setblock(at(3, 2, 6), "minecraft:air"), ticks=12)
        # A block that is set by a command tells its neighbours, and is not told itself. (On stone:
        # grass that dies under the lamp would tell it - a random tick, on ours at once.)
        step("the console lays three stones into the ground", ticks=6,
             action=lambda: console("fill {} {} {} {} {} {} minecraft:stone".format(*at(-3, 0, 6), *at(-1, 0, 6))))
        step("the console sets a block of redstone, and a lamp beside it",
             lambda: (setblock(at(-3, 1, 6), "minecraft:redstone_block"), setblock(at(-2, 1, 6), "minecraft:redstone_lamp")), ticks=10)
        step("the console sets a stone on the lamp's other side", lambda: setblock(at(-1, 1, 6), "minecraft:stone"), ticks=10)
        step("the console fills the three with air, destroying", ticks=12,
             action=lambda: console("fill {} {} {} {} {} {} minecraft:air destroy".format(*at(-3, 1, 6), *at(-1, 1, 6))))

    # ---- falling blocks: the tick two after, the fall, the landing
    at = place(1)
    hotbar("kStone", "kTorch", "kSand", "kGravel", "kWhiteConcretePowder")
    step("B places sand on the ground", lambda: (b.select(2), b.use(at(-1, 0, 2))), ticks=10)
    step("B places a stone and sand on it", lambda: (b.select(0), b.use(at(0, 0, 2)), b.select(2), b.use(at(0, 1, 2))))
    step("B breaks the stone under the sand", lambda: b.dig(at(0, 1, 2)), ticks=24)
    step("B builds a pillar of three", lambda: (b.select(0), b.use(at(1, 0, 3)), b.use(at(1, 1, 3)), b.use(at(1, 2, 3))))
    step("B places concrete powder against its top, on the east", lambda: (b.select(4), against(at(1, 3, 3), EAST)), ticks=30)
    step("B places a torch west of the pillar", lambda: (b.select(1), b.use(at(0, 0, 3))))
    step("B places gravel against the pillar's top, over the torch", lambda: (b.select(3), against(at(1, 3, 3), WEST)), ticks=30)
    step("B places sand on sand, three high, and breaks the lowest",
         lambda: (b.select(2), b.use(at(-1, 1, 2)), b.use(at(-1, 2, 2)), b.dig(at(-1, 1, 2))), ticks=30)

    # ---- random ticks
    if random_ticks:
        at = place(2)
        hotbar("kStone", "kDirt", "kCopperBlock", "kBuddingAmethyst", "kCrimsonNylium", "kCutCopperStairs", "kCopperChest")
        grass, dirt = mc.BLOCK["kGrassBlock"], mc.BLOCK["kDirt"]
        cluster = range(mc.BLOCK["kAmethystCluster"] - 9, mc.BLOCK["kAmethystCluster"] + 3)   # its twelve states
        random_ticks(True)
        idle(4)
        untouched = [at(x, 0, z) for x in (2, 3, 4) for z in (0, 1, 2)]
        step("nothing happens to grass that has nowhere to go", ticks=20, about=untouched)
        step("B digs a hole in the grass: the dirt under it is taken", lambda: b.dig(at(3, 0, 0)), ticks=400,
             until=lambda blocks: blocks.get(at(3, -1, 0)) == grass, about=[at(3, 0, 0), at(3, -1, 0)])
        step("B puts a stone on grass: it dies", lambda: (b.select(0), b.use(at(3, 0, 1))), ticks=400,
             until=lambda blocks: blocks.get(at(3, 0, 1)) == dirt, about=[at(3, 0, 1), at(3, 1, 1)])
        step("B puts dirt on grass: the grass dies, the dirt grows over", lambda: (b.select(1), b.use(at(3, 0, 2))), ticks=600,
             until=lambda blocks: blocks.get(at(3, 0, 2)) == dirt and blocks.get(at(3, 1, 2)) == grass,
             about=[at(3, 0, 2), at(3, 1, 2)])
        step("B places a block of copper: it weathers, all the way", lambda: (b.select(2), b.use(at(5, 0, 0))), ticks=1200,
             until=lambda blocks: blocks.get(at(5, 1, 0)) == mc.BLOCK["kOxidizedCopper"], about=[at(5, 1, 0)])
        buds = [at(5, 2, 3), at(4, 1, 3), at(6, 1, 3), at(5, 1, 2), at(5, 1, 4)]
        step("B places budding amethyst: a cluster grows on every side", lambda: (b.select(3), b.use(at(5, 0, 3))), ticks=2400,
             until=lambda blocks: all(blocks.get(p) in cluster for p in buds), about=[at(5, 1, 3)] + buds)
        step("B places crimson nylium and a stone on it: netherrack", lambda: (b.select(4), b.use(at(1, 0, 3)), b.select(0), b.use(at(1, 1, 3))),
             ticks=400, until=lambda blocks: blocks.get(at(1, 1, 3)) == mc.BLOCK["kNetherrack"], about=[at(1, 1, 3), at(1, 2, 3)])
        # Copper with a shape weathers into the next block with the same shape: stairs upside down, facing east.
        oxidized = mc.BLOCK["kOxidizedCutCopperStairs"] - 11 + 3 * 20 + 1     # facing east, half top, straight, dry
        step("B places cut copper stairs upside down against a stone: they weather as they are",
             lambda: (b.select(0), b.use(at(0, 0, 2)), b.select(5), b.look(-90.0, 0.0),
                      b.use(at(0, 1, 2), face=EAST, cursor=(1.0, 0.75, 0.5)), b.look(0.0, 0.0)), ticks=2400,
             until=lambda blocks: blocks.get(at(1, 1, 2)) == oxidized, about=[at(1, 1, 2)])
        # A copper chest weathers too - a pair of them as one: whenever a client is told, both halves are the same block.
        aged = {mc.BLOCK[k] - 1: n for n, k in enumerate(("kCopperChest", "kExposedCopperChest", "kWeatheredCopperChest",
                                                          "kOxidizedCopperChest"))}     # first state of each -> its age

        def pair_is(blocks: dict, age: int) -> bool:
            left, right = blocks.get(at(2, 1, 5)), blocks.get(at(3, 1, 5))
            return left is not None and right is not None and aged.get(left - 3) == age and aged.get(right - 5) == age
        step("B places two copper chests side by side: the pair weathers", lambda: (b.select(6), b.use(at(2, 0, 5)), b.use(at(3, 0, 5))),
             ticks=2400, until=lambda blocks: pair_is(blocks, 1), about=[at(2, 1, 5), at(3, 1, 5)])
        step("and then it is quiet", ticks=30, about=untouched + buds + [at(5, 1, 0), at(1, 1, 3), at(1, 1, 2)])
        if console:
            # The time of day. Grass spreads where the light above it is nine or more - the sky's
            # fifteen less what the hour takes: six levels at tick 13014 of the day, seven from
            # 13015 on; seven until 22985 before dawn, six from 22986. The clock stands still
            # for it and is put where each step wants it, a day further on each time.
            console("time pause")
            idle(2)
            step("midnight: B digs a hole in the grass, and the dirt under it stays dirt",
                 lambda: (console("time set midnight"), b.dig(at(2, 0, 4))), ticks=80, about=[at(2, 0, 4), at(2, -1, 4)])
            step("dusk, tick 13015 of the day: still dirt", lambda: console("time set 37015"), ticks=80, about=[at(2, -1, 4)])
            step("dusk, tick 13014 of the day: it is taken", lambda: console("time set 61014"), ticks=400,
                 until=lambda blocks: blocks.get(at(2, -1, 4)) == grass, about=[at(2, -1, 4)])
            step("dawn, tick 22985 of the day: B digs another hole, and the dirt stays dirt",
                 lambda: (console("time set 94985"), b.dig(at(2, 0, 6))), ticks=80, about=[at(2, 0, 6), at(2, -1, 6)])
            step("dawn, tick 22986 of the day: it is taken", lambda: console("time set 118986"), ticks=400,
                 until=lambda blocks: blocks.get(at(2, -1, 6)) == grass, about=[at(2, -1, 6)])
            console("time set 126000")     # (the morning after, for what follows)
            console("time resume")
        random_ticks(False)
        idle(4)

    # ---- stairs and slabs: the half that is clicked, the corner the neighbours make, two halves that are one block
    at = place(3)
    hotbar("kStone", "kOakStairs", "kOakSlab", "kStoneSlab")
    DOWN_FACE = 0
    upper = {NORTH: (0.5, 0.75, 0.0), SOUTH: (0.5, 0.75, 1.0), WEST: (0.0, 0.75, 0.5), EAST: (1.0, 0.75, 0.5)}
    lower = {NORTH: (0.5, 0.25, 0.0), SOUTH: (0.5, 0.25, 1.0), WEST: (0.0, 0.25, 0.5), EAST: (1.0, 0.25, 0.5)}
    step("B, looking south, places stairs", lambda: (b.select(1), b.look(0.0, 0.0), b.use(at(0, 0, 3))))
    step("B, looking east, places stairs behind them: an outer corner", lambda: (b.look(-90.0, 0.0), b.use(at(0, 0, 4))))
    step("B breaks the second stairs: straight again", lambda: b.dig(at(0, 1, 4)))
    step("B, looking west, places stairs in front of them: an inner corner", lambda: (b.look(90.0, 0.0), b.use(at(0, 0, 2))))
    step("B, looking south, places stairs west of the first: they run straight on", lambda: (b.look(0.0, 0.0), b.use(at(-1, 0, 3))))
    step("B breaks those: the corner again", lambda: b.dig(at(-1, 1, 3)))
    step("B breaks the first and, looking north, places stairs north of the ones that face west: into the corner",
         lambda: (b.dig(at(0, 1, 3)), b.look(180.0, 0.0), b.use(at(0, 0, 1)), b.look(0.0, 0.0)))
    step("B builds a stone and places stairs against the upper half of its east side, the lower half of its west",
         lambda: (b.select(0), b.use(at(2, 0, 4)), b.select(1), b.use(at(2, 1, 4), face=EAST, cursor=upper[EAST]),
                  b.use(at(2, 1, 4), face=WEST, cursor=lower[WEST])))
    step("B, looking east, places stairs south of the upside-down ones: halves do not turn each other",
         lambda: (b.look(-90.0, 0.0), b.use(at(3, 1, 4), face=SOUTH, cursor=lower[SOUTH]), b.look(0.0, 0.0)))
    # Slabs.
    step("B places a slab on the ground", lambda: (b.select(2), b.use(at(-2, 0, 2))))
    step("...and the same slab on it: one block", lambda: b.use(at(-2, 1, 2), cursor=(0.5, 0.5, 0.5)))
    step("...and another: it lies on the double slab", lambda: b.use(at(-2, 1, 2)))
    step("B builds a stone and places a slab against the upper half of its north side",
         lambda: (b.select(0), b.use(at(-2, 0, 5)), b.select(2), b.use(at(-2, 1, 5), face=NORTH, cursor=upper[NORTH])))
    step("...and against the lower half of the same side: the top slab is filled",
         lambda: b.use(at(-2, 1, 5), face=NORTH, cursor=lower[NORTH]))
    step("B places a slab against the upper half of the stone's west side, and clicks that slab's side, high: another beside it",
         lambda: (b.use(at(-2, 1, 5), face=WEST, cursor=upper[WEST]), b.use(at(-3, 1, 5), face=NORTH, cursor=upper[NORTH])))
    step("...and low: the slab itself is filled", lambda: b.use(at(-3, 1, 5), face=NORTH, cursor=lower[NORTH]))
    step("B clicks the top slab that is left from below: filled",
         lambda: b.use(at(-3, 1, 4), face=DOWN_FACE, cursor=(0.5, 0.5, 0.5)))
    step("B places a slab on the ground and a stone slab on it: two blocks",
         lambda: (b.use(at(-1, 0, 1)), b.select(3), b.use(at(-1, 1, 1), cursor=(0.5, 0.5, 0.5))))
    if console:
        step("the console destroys a double slab, and half a one: two of it, and one",
             lambda: (setblock(at(-2, 1, 2), "minecraft:air destroy"), setblock(at(-1, 1, 1), "minecraft:air destroy")), ticks=12)
        step("the console sets stairs beside stairs, a slab that is double, and stairs in a corner",
             lambda: (setblock(at(1, 1, 1), "minecraft:oak_stairs[facing=east]"), setblock(at(3, 1, 2), "minecraft:oak_slab[type=double]"),
                      setblock(at(2, 1, 1), "minecraft:oak_stairs[facing=south]")), ticks=10)

    # ---- fences, panes and walls: what they reach for; lanterns, chains, rods, glazed terracotta, mushroom blocks
    at = place(4)
    hotbar("kStone", "kOakFence", "kNetherBrickFence", "kGlassPane", "kCobblestoneWall", "kTorch", "kIronBars", "kLantern", "kIronChain")
    step("B places a stone and a fence beside it", lambda: (b.select(0), b.use(at(0, 0, 3)), b.select(1), b.use(at(1, 0, 3))))
    step("...a second fence", lambda: b.use(at(2, 0, 3)))
    step("...and a nether brick fence beside the two of wood", lambda: (b.select(2), b.use(at(3, 0, 3))))
    step("B places a pane south of the stone, a second beside it, and iron bars beside that",
         lambda: (b.select(3), b.use(at(0, 0, 4)), b.use(at(1, 0, 4)), b.select(6), b.use(at(2, 0, 4))))
    step("B places a wall north of the stone", lambda: (b.select(4), b.use(at(0, 0, 2))))
    step("...a second, west of it", lambda: b.use(at(-1, 0, 2)))
    step("...a third: the one in the middle has no post", lambda: b.use(at(-2, 0, 2)))
    step("B places a stone on the middle wall: tall", lambda: (b.select(0), b.use(at(-1, 1, 2))))
    step("B breaks it: low", lambda: b.dig(at(-1, 2, 2)))
    step("B places a torch on the middle wall: a post", lambda: (b.select(5), b.use(at(-1, 1, 2))))
    step("B breaks it: none", lambda: b.dig(at(-1, 2, 2)))
    step("B places a wall on the middle wall: a post under a post", lambda: (b.select(4), b.use(at(-1, 1, 2))))
    step("B places a pane south of the third wall: a corner", lambda: (b.select(3), b.use(at(-2, 0, 3))))
    step("B breaks the stone: everything lets go of it", lambda: b.dig(at(0, 1, 3)), ticks=12)
    step("B clicks the fence's north side with a stone: it is placed there", ticks=10,
         action=lambda: (b.select(0), b.use(at(1, 1, 3), face=NORTH, cursor=(0.5, 0.5, 0.375))))
    # Lanterns: under a block they hang, on one they stand; neither, and they are not placed.
    step("B builds a stone in the air", lambda: (b.use(at(2, 0, 1)), b.use(at(2, 1, 1)), b.dig(at(2, 1, 1))))
    step("B hangs a lantern under it", lambda: (b.select(7), b.use(at(2, 2, 1), face=DOWN_FACE, cursor=(0.5, 0.0, 0.5))))
    step("B stands a lantern on the ground", lambda: b.use(at(3, 0, 1)))
    step("B clicks the side of the stone in the air with a lantern: nothing holds one there",
         lambda: b.use(at(2, 2, 1), face=EAST, cursor=(1.0, 0.5, 0.5)))
    step("B breaks the stone the lantern hangs from", lambda: b.dig(at(2, 2, 1)), ticks=12)
    step("B places a chain on the ground, one against its west side and one against its north",
         lambda: (b.select(8), b.use(at(-2, 0, 5)), against(at(-2, 1, 5), WEST), against(at(-2, 1, 5), NORTH)))
    hotbar("kStone", "kEndRod", "kWhiteGlazedTerracotta", "kBrownMushroomBlock", "kWaxedCopperGrate")
    step("B places an end rod on the ground, one on that, and one against the side of the lower",
         lambda: (b.select(1), b.use(at(1, 0, 5)), b.use(at(1, 1, 5)), against(at(1, 1, 5), EAST)))
    step("B places glazed terracotta looking south, and looking east",
         lambda: (b.select(2), b.look(0.0, 0.0), b.use(at(3, 0, 4)), b.look(-90.0, 0.0), b.use(at(3, 0, 5)), b.look(0.0, 0.0)))
    step("B places two mushroom blocks side by side, and a copper grate", ticks=10,
         action=lambda: (b.select(3), b.use(at(-1, 0, 4)), b.use(at(-1, 0, 5)), b.select(4), b.use(at(0, 0, 5))))
    step("B breaks one of the mushroom blocks", lambda: b.dig(at(-1, 1, 5)), ticks=12)
    if console:
        # What a command sets takes the shape its neighbours give it; what a command fills does too.
        step("the console sets two stones, a fence between them and a wall north of the fence",
             lambda: (setblock(at(-3, 1, -2), "minecraft:stone"), setblock(at(-1, 1, -2), "minecraft:stone"),
                      setblock(at(-2, 1, -2), "minecraft:oak_fence"), setblock(at(-2, 1, -3), "minecraft:cobblestone_wall")), ticks=10)
        step("the console fills a row of walls north of that wall", ticks=10,
             action=lambda: console("fill {} {} {} {} {} {} minecraft:cobblestone_wall".format(*at(-3, 1, -4), *at(-1, 1, -4))))
        step("the console sets a pane with arms nobody holds, and one strictly",
             lambda: (setblock(at(-2, 1, -5), "minecraft:glass_pane[east=true,west=true]"),
                      setblock(at(0, 1, -4), "minecraft:glass_pane[north=true] strict")), ticks=10)
        step("the console fills the row with air, destroying", ticks=12,
             action=lambda: console("fill {} {} {} {} {} {} minecraft:air destroy".format(*at(-3, 1, -4), *at(-1, 1, -4))))

    # ---- plants: what they stand on, how many lie in one block, what gives way to a block and what breaks
    at = place(5)
    hotbar("kStone", "kPoppy", "kPinkPetals", "kLeafLitter", "kShortGrass", "kDeadBush", "kSand", "kCactusFlower", "kAzalea")

    def sneak(on: bool) -> None:
        b.c.send("player_input", bytes([0x20 if on else 0]))   # the shift bit of player_input

    step("B places a poppy on the grass, and tries one on a stone",
         lambda: (b.select(1), b.use(at(0, 0, 2)), b.select(0), b.use(at(1, 0, 2)), b.select(1), b.use(at(1, 1, 2))))
    step("B tries a dead bush on the stone, and places one on sand",
         lambda: (b.select(5), b.use(at(1, 1, 2)), b.select(6), b.use(at(2, 0, 2)), b.select(5), b.use(at(2, 1, 2))))
    step("B tries an azalea on the stone, and places a cactus flower there", lambda: (b.select(8), b.use(at(1, 1, 2)), b.select(7), b.use(at(1, 1, 2))))
    step("B places an azalea on the grass", lambda: (b.select(8), b.use(at(2, 0, 4))))
    step("B places leaf litter on a stone", lambda: (b.select(0), b.use(at(3, 0, 2)), b.select(3), b.use(at(3, 1, 2))))
    step("...a second onto it", lambda: b.use(at(3, 2, 2)))
    step("...a third and a fourth", lambda: (b.use(at(3, 2, 2)), b.use(at(3, 2, 2))))
    step("...and a fifth, which has nowhere to go", lambda: b.use(at(3, 2, 2)))
    step("B places petals on the grass", lambda: (b.select(2), b.use(at(-1, 0, 2))))
    step("...two more, by the ground under them", lambda: (b.use(at(-1, 0, 2)), b.use(at(-1, 0, 2))))
    step("B, sneaking, clicks the ground under them with petals", lambda: (sneak(True), b.use(at(-1, 0, 2)), sneak(False)))
    step("...and not sneaking: the fourth", lambda: b.use(at(-1, 0, 2)))
    step("B, looking west, places petals and leaf litter", lambda: (b.look(90.0, 0.0), b.use(at(-1, 0, 3)), b.select(3), b.use(at(-1, 0, 4)),
                                                                     b.look(0.0, 0.0)))
    step("B places short grass, and tries short grass on it", lambda: (b.select(4), b.use(at(-2, 0, 2)), b.use(at(-2, 1, 2))))
    step("B sets a stone into the short grass, into the leaf litter, and against the poppy",
         lambda: (b.select(0), b.use(at(-2, 1, 2)), b.use(at(3, 2, 2)), against(at(0, 1, 2), NORTH)))
    step("B clicks the petals with a stone: it is set over them", lambda: b.use(at(-1, 1, 3)))
    step("B digs the ground from under the poppy", lambda: b.dig(at(0, 0, 2)), ticks=12)
    step("B digs the ground from under the four petals", lambda: b.dig(at(-1, 0, 2)), ticks=12)
    step("B breaks the stone under the cactus flower", lambda: b.dig(at(1, 1, 2)), ticks=12)
    step("B places a stone, litter on it, and breaks the stone", lambda: (b.select(0), b.use(at(4, 0, 4)), b.select(3), b.use(at(4, 1, 4)),
                                                                          b.use(at(4, 2, 4)), b.dig(at(4, 1, 4))), ticks=12)
    if console:
        step("the console sets a poppy in the air, and one on the grass",
             lambda: (setblock(at(5, 3, 2), "minecraft:poppy"), setblock(at(5, 1, 2), "minecraft:poppy")), ticks=10)
        step("the console sets full petals facing east, and destroys them",
             lambda: (setblock(at(5, 1, 3), "minecraft:wildflowers[facing=east,flower_amount=4]"),
                      setblock(at(5, 1, 3), "minecraft:air destroy")), ticks=12)
        step("the console sets a barrel, open, and a smoker, lit", ticks=10,
             action=lambda: (setblock(at(5, 1, 5), "minecraft:barrel[facing=up,open=true]"), setblock(at(4, 1, 5), "minecraft:smoker[lit=true]")))
    hotbar("kBarrel", "kSmoker", "kBlastFurnace")
    step("B places a barrel looking south, looking up, and looking down", ticks=10,
         action=lambda: (b.select(0), b.use(at(-2, 0, 5)), b.look(0.0, -90.0), b.use(at(-1, 0, 5)),
                         b.look(0.0, 90.0), b.use(at(0, 0, 5)), b.look(0.0, 0.0)))
    step("B, looking east, places a smoker and a blast furnace", ticks=10,
         action=lambda: (b.look(-90.0, 0.0), b.select(1), b.use(at(1, 0, 5)), b.select(2), b.use(at(2, 0, 5)), b.look(0.0, 0.0)))

    # ---- TNT, last, for what it leaves: a signal reaches the six blocks around it in an order
    at = place(6)
    hotbar("kStone", "kTnt", "kRedstoneBlock")
    step("B places TNT west, east, south and north of an empty place",
         lambda: (b.select(1), b.use(at(-1, 0, 3)), b.use(at(1, 0, 3)), b.use(at(0, 0, 4)), b.use(at(0, 0, 2))))
    step("...and, against a stone on the southern one, over it",
         lambda: (b.select(0), b.use(at(0, 1, 4)), b.select(1), against(at(0, 2, 4), NORTH)))
    step("B places a block of redstone in the middle", lambda: (b.select(2), b.use(at(0, 0, 3))), ticks=10)
    step("the fuses burn", ticks=95, loose=True)
    step("afterwards", ticks=30, loose=True)

    for bot in (a, b):
        bot.c.close()
    Path(out_path).write_text(json.dumps({"origin": origin, "ids": {"A": a.entity_id, "B": b.entity_id}, "tick_ms": TICK * 1000,
                                          "steps": steps}))
    print(f"wrote {out_path}")


def _blocks(log: list) -> dict:
    out = {}
    for name, hexdata in log:
        r = mc.Reader(bytes.fromhex(hexdata))
        if name == "block_update":
            pos, state = mc.parse_block_update(r)
            out[pos] = state
        elif name == "section_blocks_update":
            out.update(mc.parse_section_blocks_update(r))
    return out


# ----------------------------------------------------------------------------- diff

def _rel(pos, origin) -> tuple:
    return tuple(p - o for p, o in zip(pos, origin))


SOIL = (mc.BLOCK["kGrassBlock"], mc.BLOCK["kDirt"])
OF_BLOCKS = {"block", "level_event", "add_transient_block", "sound", "blocks in the end"}


def _normalise(rec: dict, who: str) -> dict[str, list]:
    origin = rec["origin"]
    tick_ms = rec.get("tick_ms", 50.0)     # (a recording of before there was --tick-rate: vanilla's 50)
    kinds: dict[int, str] = {}     # entity id -> "tnt1", "falling2"
    counts = {"tnt": 0, "item": 0, "falling": 0}
    dropped: dict[int, tuple] = {}  # item entity -> the stack it was dropped as
    out = {}
    for st in rec["steps"]:
        rows = []
        items = []
        first = None               # when the first of the step's events that a tick sends came
        timed = st.get("about") is None     # (a step about random ticks says what it is about)
        final: dict = {}           # for the steps that are about random ticks: what the blocks ended up as

        def row(kind: str, text: str, ms: int | None = None, chunk: tuple = ()) -> None:
            """`ms`: when it came - of what a server sends with its tick. A level event and a
            sound are sent when they happen: by vanilla between two ticks if a packet was the
            cause, a moment before that tick's block changes; by ours with them. `chunk`: of a
            block change, the chunk it is in (see _streams)."""
            nonlocal first
            if ms is None or not timed:
                rows.append((kind, text))
                return
            if first is None:
                first = ms
            rows.append((kind, f"+{round((ms - first) / tick_ms)}t {text}", *chunk))

        about = None if st.get("about") is None else {tuple(p) for p in st["about"]}
        if st.get("loose"):
            out[st["step"]] = [("explosions", str(sum(name == "explode" for name, _, _ in st[who])))]
            continue
        for name, hexdata, ms in st[who]:
            r = mc.Reader(bytes.fromhex(hexdata))
            if name in ("block_update", "section_blocks_update"):
                changed = [mc.parse_block_update(r)] if name == "block_update" else sorted(mc.parse_section_blocks_update(r).items())
                for pos, state in changed:
                    if not timed:
                        if about is None or tuple(pos) in about:
                            final[_rel(pos, origin)] = state
                    elif state not in SOIL:     # (a random tick, whenever it falls: see the top)
                        row("block", f"{_rel(pos, origin)} = {state}", ms,
                            ((pos[0] // 16 - origin[0] // 16, pos[2] // 16 - origin[2] // 16),))
            elif name == "level_event":
                kind = r.i32()
                where = mc.unpack_block_pos(r.u64())
                if about is None or tuple(where) in about:
                    row(name, f"{kind} at {_rel(where, origin)} data {r.i32()} global {r.bool()}")
            elif name == "add_transient_block":
                pos = _rel(mc.unpack_block_pos(r.u64()), origin)
                row(name, f"{pos} = {r.varint()}", ms)
            elif name == "sound":
                sound, source = r.varint() - 1, r.varint()
                pos = tuple(v / 8 - o for v, o in zip((r.i32(), r.i32(), r.i32()), origin))
                if sound in mc.SOUND.values():   # only the sounds this server makes at all
                    row(name, f"{sound} source {source} at {pos}")
            elif name == "explode":
                rows.append(("  (not compared)", "an explosion"))
            elif name == "add_entity":
                eid = r.varint()
                r.take(16)
                etype = r.varint()
                pos = (r.f64(), r.f64(), r.f64())
                velocity = tuple(round(v, 4) for v in mc.read_lp_vec3(r))
                r.take(3)
                data = r.varint()
                if etype in (TNT, ITEM_ENTITY, FALLING):
                    kind = "tnt" if etype == TNT else "item" if etype == ITEM_ENTITY else "falling"
                    counts[kind] += 1
                    kinds[eid] = f"{kind}{counts[kind]}"
                    dropped.pop(eid, None)      # (an id is used again, by this server soon)
                    if etype == FALLING:
                        row(name, f"a falling block of {data} at {_rel(pos, origin)} velocity {velocity}", ms)
                    elif etype == TNT:
                        # Vanilla shows a primed TNT the moment it is made, ours with the tick's other
                        # entities: which TNT went off first is in the order, not in the moment.
                        rows.append(("add_entity", f"a primed TNT at {_rel(pos, origin)}"))
            elif name == "set_entity_data":
                eid = r.varint()
                if eid not in kinds:
                    continue
                data = mc.read_entity_data(r)
                if kinds[eid].startswith("item"):
                    if mc.ITEM_DATA["kItem"] in data:
                        # What was dropped - not what two stacks that lay close enough became later.
                        stack = tuple(data[mc.ITEM_DATA["kItem"]][1][:2])
                        if dropped.setdefault(eid, stack) == stack:
                            items.append(stack)
                elif kinds[eid].startswith("falling"):
                    start = data.get(mc.FALLING_DATA["kStartPos"])
                    if start:
                        rows.append(("set_entity_data", f"{kinds[eid]} began at {_rel(start[1], origin)}"))
            elif name in ("move_entity_pos", "move_entity_pos_rot"):
                eid = r.varint()
                if kinds.get(eid, "").startswith("falling"):
                    props = r.u8()
                    if props & 2:
                        r.varint()
                    dx, dy, dz = r.i16(), r.i16(), r.i16()
                    row(name, f"{kinds[eid]} by ({dx}, {dy}, {dz}) ground {props & 1}", ms)
            elif name == "entity_position_sync":
                eid = r.varint()
                if kinds.get(eid, "").startswith("falling"):
                    if r.u8():      # (as container_probe.py reads it)
                        r.u8()
                    pos = _rel((r.f64(), r.f64(), r.f64()), origin)
                    row(name, f"{kinds[eid]} at {pos} {r.d[r.i:].hex()}", ms)
            elif name == "set_entity_motion":
                eid = r.varint()
                if kinds.get(eid, "").startswith("falling"):
                    row(name, f"{kinds[eid]} velocity {tuple(round(v, 4) for v in mc.read_lp_vec3(r))}", ms)
            elif name == "teleport_entity":
                eid = r.varint()
                if kinds.get(eid, "").startswith("falling"):
                    row(name, f"{kinds[eid]} {r.d[r.i:].hex()}", ms)
            elif name == "remove_entities":
                for eid in [r.varint() for _ in range(r.varint())]:
                    if kinds.get(eid, "").startswith("falling"):
                        row(name, kinds[eid], ms)
            elif name == "disconnect":
                rows.append((name, hexdata))
        if final:
            rows.append(("blocks in the end", str(sorted(final.items()))))
        if items:
            rows.append(("items on the ground", str(sorted(items))))
        # A falling block is told where it began twice when it is seen from its first tick: once with the
        # entity, once as a value that changed. Either server does; which comes first is not the point.
        out[st["step"]] = rows
    return out


def _chunk_by_chunk(rows: list) -> list:
    """The block changes of one tick, chunk after chunk: which chunk a server tells first is
    chance (see the top), what it says of each, and in which order, is not."""
    out, i = [], 0
    while i < len(rows):
        j = i + 1
        if rows[i][0] == "block" and len(rows[i]) > 2:
            tick = rows[i][1].split()[0]
            while j < len(rows) and rows[j][0] == "block" and len(rows[j]) > 2 and rows[j][1].split()[0] == tick:
                j += 1
            out += sorted(rows[i:j], key=lambda r: r[2])    # (stable: a chunk's own changes stay as they came)
        else:
            out.append(rows[i])
        i = j
    return out


def _streams(rows: list) -> tuple:
    """What is compared of a step: what is said about blocks, in its order; which entities
    appear, in theirs; and what is said of each entity, in its own (see the top)."""
    blocks = _chunk_by_chunk([r for r in rows if r[0] in OF_BLOCKS])
    appear = [r for r in rows if r[0] == "add_entity"]
    each: dict[str, list] = {}
    for r in rows:
        if r[0] in OF_BLOCKS or r[0] == "add_entity":
            continue
        words = r[1].split()
        label = r[0] if not words or r[0] in ("items on the ground", "explosions", "disconnect") else \
            words[1] if words[0].startswith("+") and len(words) > 1 else words[0]
        each.setdefault(label, []).append(r)
    return blocks, appear, each


def diff(path_ref: str, path_ours: str) -> int:
    ref, ours = json.load(open(path_ref)), json.load(open(path_ours))
    different = 0
    for who in ("A", "B"):
        print(f"################ what {who} is told (left: {path_ref}, right: {path_ours})")
        left, right = _normalise(ref, who), _normalise(ours, who)
        for step in left:
            if step not in right:
                print(f"== {step}: only on the left")
                continue
            x, y = left[step], right[step]
            notes = [r[1] for r in x + y if r[0] == "  (not compared)"]
            x = [r for r in x if r[0] != "  (not compared)"]
            y = [r for r in y if r[0] != "  (not compared)"]
            same = _streams(x) == _streams(y)
            print(f"== {step}: {'same' if same else 'DIFFERENT'} ({len(x)} / {len(y)} rows)")
            different += not same
            for i in range(max(len(x), len(y)) if not same or "-v" in sys.argv else 0):
                lhs = x[i] if i < len(x) else ("", "")
                rhs = y[i] if i < len(y) else ("", "")
                print(f"   {lhs[0]:20s}{lhs[1][:80]:80s} {' ' if lhs == rhs else '|'} {rhs[0]:20s}{rhs[1][:80]}")
            if notes and "-v" in sys.argv:
                print("   not compared: " + "; ".join(notes)[:400])
    print(f"{different} steps differ")
    return different


# ----------------------------------------------------------------------------- the servers

def record_vanilla(out: str, name_a: str, name_b: str) -> None:
    world = SERVER_DIR / PROBE_WORLD
    if world.exists():
        if not (world / "level.dat").exists():
            raise SystemExit(f"{world} is not a world: it is not this probe's to remove")
        shutil.rmtree(world)
    v = Vanilla(PROBE_WORLD, Path(tempfile.gettempdir()) / "block_probe_vanilla.log")

    def rule(fast: bool) -> None:
        v.cmd(f"gamerule minecraft:random_tick_speed {RANDOM_TICK_SPEED if fast else 3}")
        v.until(r"Game rule \S*random_tick_speed is now set to", timeout=20)

    try:
        v.until(r"Done \(")
        v.cmd("gamerule minecraft:spawn_mobs false")     # (kept in this probe's world, which is made anew)
        v.until(r"Game rule \S*spawn_mobs is now set to", timeout=20)
        if pace.RATE != 20:
            v.cmd(f"tick rate {pace.RATE:g}")
            v.until(r"Set the target tick rate", timeout=20)
        record(PORT, out, name_a, name_b, rule, v.cmd)
    finally:
        v.stop()


def record_ours(exe: str, out: str, name_a: str, name_b: str) -> None:
    with socket.socket() as probe:      # a port nothing listens on: reactors bind with SO_REUSEPORT
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    tmp = Path(tempfile.mkdtemp(prefix="block-probe-"))
    log = open(tmp / "server.log", "w")
    proc = subprocess.Popen([exe, "--server-ip=127.0.0.1", f"--server-port={port}", "--online-mode=false", "--view-distance=2",
                             "--simulation-distance=2", f"--random-tick-speed={RANDOM_TICK_SPEED}", "--config=/nonexistent", f"--tick-rate={pace.RATE:g}",
                             "--level-name=", f"--ops-file={tmp / 'ops.json'}"], stdin=subprocess.PIPE, stdout=log, stderr=log)
    try:
        end = time.time() + 30
        while "listening on" not in (tmp / "server.log").read_text():
            if proc.poll() is not None or time.time() > end:
                raise SystemExit("our server did not start:\n" + (tmp / "server.log").read_text())
            time.sleep(0.05)
        def console(line: str) -> None:
            proc.stdin.write(line.encode() + b"\n")
            proc.stdin.flush()

        record(port, out, name_a, name_b, lambda fast: None, console)
    finally:
        proc.terminate()
        code = proc.wait(timeout=30)
        log.close()
        print((tmp / "server.log").read_text().strip())
        print(f"our server exited with {code}")
        shutil.rmtree(tmp, ignore_errors=True)


def main() -> int:
    argv = [arg for arg in sys.argv[1:] if arg != "-v"]
    what, args = (argv[0], argv[1:]) if argv else ("", [])
    fixed = {"vanilla": 1, "ours": 2, "record": 2}.get(what)
    if fixed is not None and len(args) >= fixed:
        names = tuple(args[fixed:fixed + 2]) if len(args) >= fixed + 2 else ("ProbeG", "ProbeH")
        if what == "vanilla":
            record_vanilla(args[0], *names)
        elif what == "ours":
            record_ours(args[0], args[1], *names)
        else:
            record(int(args[0]), args[1], *names, (lambda fast: None) if "random" in args[fixed:] else None)
        return 0
    if what == "diff" and len(args) == 2:   # -v: print every step's rows, and what is not compared
        return 1 if diff(args[0], args[1]) else 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
