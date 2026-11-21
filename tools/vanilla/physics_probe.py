#!/usr/bin/env python3
"""What does vanilla's physics do with an entity - tick by tick, in exact doubles?

    physics_probe.py record <out.json> [--only REGEX]    # runs the vanilla server itself
    physics_probe.py emit <in.json> <out.bin>            # the recording as the unit test's vectors
    physics_probe.py list                                # the scenarios
    physics_probe.py perf <out.json> [count]             # ...and how long it takes vanilla (runs the server too)
    physics_probe.py wire <out.json>                     # what a viewer is told about items that lie still

`record` starts the vanilla reference server (under the rules in AGENTS.md: from
.cache/vanilla-server/, the cached jar, Java 25), joins one bot so that the server does not
pause, freezes the game (`/tick freeze`) and builds one small fixture per scenario - a platform
in the air with whatever blocks the scenario names - then summons the scenario's entity with an
exact position and velocity and steps the game one tick at a time (`/tick step 1`). After
every step each entity's NBT is read back with `/data get entity`: `Pos`, `Motion`, `OnGround`
and `fall_distance` are printed as the shortest decimal that names the double, so what is
recorded is the state bit for bit. The fixtures are taken down again before the server stops.

`emit` turns a recording into tests/unit/data/physics_vectors.bin: per scenario the blocks, the
entities and their state after every tick, doubles as their eight bytes. The unit test
(tests/unit/physics_test.cpp) builds the same blocks at the same coordinates, runs this
server's physics and compares bits.

What the recording does not see: an entity's id (the bot is shown it, or it is counted from a
neighbour's - `/summon` hands ids out in order), and what happens inside a tick. Nothing here is
random: scenarios that would draw from an entity's RandomSource (an item pushed out of a block
it is stuck in) are not recorded.

`perf` is the other half of a comparison: what `count` items, armor stands or primed TNT - lying
still, or falling through open air - add to a vanilla tick. It summons them far from the bot in
force-loaded chunks, runs the game flat out (`/tick sprint`) and takes the time of a tick without
them off the time with them; `/debug` then says which part of the tick it was. Ours for the same
cases: `build/release/bench physics`, and `tests/integration/run.py -k ten_thousand --slow` on
a running server. Do not run anything else on the machine meanwhile.

Coordinates matter - floating point rounds by magnitude - so the fixtures stand at fixed
places: around (-993, 1008), where no other probe builds, and a few at the world's origin, where
an entity's box lies inside the unit cube (Shapes.create then makes it a CubeVoxelShape).
"""
from __future__ import annotations

import json
import queue
import re
import socket
import struct
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from decimal import Decimal
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "integration"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import mcproto as mc  # noqa: E402
from entity_probe import Bot  # noqa: E402
from mcproto import varint  # noqa: E402

VERSION = mc.META["version"]
SERVER_DIR = ROOT / ".cache" / "vanilla-server"
JAVA = "/usr/lib/jvm/java-25-openjdk/bin/java"
PORT = 25577

# The bot stands on the ground in the middle; plots are 7 x 7 platforms on an 8-block grid
# around it, stacked every 14 blocks, all within its 2-chunk simulation distance.
BOT_AT = (-992.5, -60.0, 1008.5)
PLOT_X0, PLOT_Z0, PLOT_Y0, PLOT_DY, PLOT_LEVELS = -993, 1008, -46, 14, 18


def fixed(v: float) -> str:
    """A double as a decimal without an exponent that parses back to the same double."""
    s = repr(float(v))
    if "e" in s or "E" in s or s in ("inf", "-inf", "nan"):
        s = format(Decimal(float(v)), "f")
        if "." not in s:
            s += ".0"
    return s


@dataclass
class Sc:
    name: str
    kind: str                       # item | tnt | armor_stand | pig | falling_block
    pos: tuple                      # relative to the plot's origin: the block above the middle of its floor
    motion: tuple = (0.0, 0.0, 0.0)
    blocks: tuple = ()              # (dx, dy, dz, "minecraft:ice[...]"), relative likewise
    floor: str | None = "minecraft:stone"
    ticks: int = 60
    nbt: str = ""                   # more entity NBT: attributes...
    at: tuple | None = None         # an absolute plot origin instead of a place in the grid
    step_height: float | None = None   # armor stands: the step_height attribute (their own is 0)
    extra: tuple = ()               # more entities of the same kind in the plot: (pos, motion)
    events: tuple = ()              # (tick, dx, dy, dz, block): set after that tick was recorded, before the next
    on_ground: bool = False         # summoned with OnGround:1b (what it is known to stand on is another matter: see emit)


def column(x, z, y0, y1, block):
    return tuple((x, y, z, block) for y in range(y0, y1 + 1))


def scenarios() -> list[Sc]:
    S: list[Sc] = []
    stone, ice = "minecraft:stone", "minecraft:ice"

    # ------------------------------------------------------------------ items
    S.append(Sc("item_drop", "item", (0.5, 3.0, 0.5), ticks=70))
    S.append(Sc("item_drop_offcentre", "item", (0.3, 2.7, 0.81), ticks=50))
    S.append(Sc("item_throw", "item", (-2.2, 0.5, 0.3), (0.25, 0.2, 0.1), ticks=110))
    S.append(Sc("item_throw_negative", "item", (2.1, 0.75, 0.6), (-0.21, 0.33, -0.17), ticks=110))
    S.append(Sc("item_throw_steep", "item", (0.5, 0.1, 0.5), (0.02, 0.9, -0.03), ticks=90))
    S.append(Sc("item_slide_ice", "item", (-2.5, 0.0, 0.5), (0.1, 0.0, 0.03), floor=ice, ticks=160))
    S.append(Sc("item_slide_packed_ice", "item", (-2.5, 0.3, 0.5), (0.08, 0.0, -0.02), floor="minecraft:packed_ice", ticks=160))
    S.append(Sc("item_slide_blue_ice", "item", (-2.5, 0.3, -0.5), (0.05, 0.0, 0.01), floor="minecraft:blue_ice", ticks=200))
    S.append(Sc("item_slime_drop", "item", (0.5, 4.0, 0.5), floor="minecraft:slime_block", ticks=160))
    S.append(Sc("item_slime_throw", "item", (-1.5, 2.0, 0.5), (0.06, 0.0, 0.02), floor="minecraft:slime_block", ticks=160))
    S.append(Sc("item_slime_rest", "item", (0.5, 0.0, 0.5), floor="minecraft:slime_block", ticks=60))
    S.append(Sc("item_soul_sand", "item", (-1.5, 1.5, 0.5), (0.12, 0.0, 0.04), floor="minecraft:soul_sand", ticks=90))
    S.append(Sc("item_mud", "item", (-1.5, 1.5, 0.5), (0.12, 0.1, -0.04), floor="minecraft:mud", ticks=90))
    S.append(Sc("item_honey_top", "item", (-1.5, 1.5, 0.5), (0.12, 0.0, 0.04), floor="minecraft:honey_block", ticks=90))
    S.append(Sc("item_honey_side", "item", (1.09, 3.5, 0.5), blocks=column(0, 0, 0, 2, "minecraft:honey_block"), ticks=70))
    S.append(Sc("item_honey_side_moving", "item", (1.3, 3.5, 0.5), (-0.03, 0.0, 0.01),
                blocks=column(0, 0, 0, 2, "minecraft:honey_block"), ticks=70))
    S.append(Sc("item_wall_x", "item", (0.5, 0.2, 0.5), (0.4, 0.3, 0.0), blocks=column(2, 0, 0, 1, stone), ticks=60))
    S.append(Sc("item_wall_diagonal", "item", (0.5, 0.2, 0.5), (0.3, 0.3, 0.2),
                blocks=column(2, 0, 0, 1, stone) + column(2, 1, 0, 1, stone) + column(2, 2, 0, 1, stone), ticks=80))
    S.append(Sc("item_wall_z_negative", "item", (0.5, 0.4, 0.5), (0.05, 0.25, -0.35),
                blocks=column(0, -2, 0, 1, stone) + column(1, -2, 0, 1, stone), ticks=80))
    S.append(Sc("item_corner", "item", (0.5, 0.2, 0.5), (0.3, 0.3, 0.3),
                blocks=column(2, 0, 0, 1, stone) + column(2, 1, 0, 1, stone) + column(2, 2, 0, 1, stone) +
                column(0, 2, 0, 1, stone) + column(1, 2, 0, 1, stone), ticks=80))
    S.append(Sc("item_ceiling", "item", (0.5, 0.3, 0.5), (0.01, 0.6, 0.0), blocks=((0, 2, 0, stone),), ticks=60))
    S.append(Sc("item_edge_slide", "item", (0.3, 1.0, 0.5), (0.45, 0.0, 0.0), blocks=((0, 0, 0, stone),), ticks=90))
    S.append(Sc("item_edge_rest", "item", (0.95, 1.5, 0.95), blocks=((0, 0, 0, stone),), ticks=50))
    S.append(Sc("item_edge_overhang", "item", (1.12, 1.5, 0.5), blocks=((0, 0, 0, stone),), ticks=50))
    S.append(Sc("item_cobweb", "item", (0.5, 4.0, 0.5), (0.05, 0.0, 0.02),
                blocks=((0, 1, 0, "minecraft:cobweb"), (0, 2, 0, "minecraft:cobweb")), ticks=160))
    S.append(Sc("item_cobweb_floor", "item", (0.5, 1.5, 0.5), blocks=((0, 0, 0, "minecraft:cobweb"),), ticks=120))
    S.append(Sc("item_chest_top", "item", (0.5, 2.0, 0.5), (0.02, 0.0, 0.01), blocks=((0, 0, 0, "minecraft:chest[facing=north]"),), ticks=80))
    S.append(Sc("item_chest_edge", "item", (0.95, 2.0, 0.5), blocks=((0, 0, 0, "minecraft:chest[facing=north]"),), ticks=60))
    S.append(Sc("item_chest_side", "item", (-1.2, 0.3, 0.5), (0.2, 0.1, 0.0), blocks=((0, 0, 0, "minecraft:chest[facing=north]"),), ticks=70))
    # Which block an entity stands on when two or four are equally near.
    S.append(Sc("item_seam_x_ice_stone", "item", (0.0, 0.0, -1.5), (0.0, 0.0, 0.12), floor=None, ticks=110,
                blocks=tuple((x, -1, z, ice if x < 0 else stone) for x in range(-2, 2) for z in range(-3, 4))))
    S.append(Sc("item_seam_x_stone_ice", "item", (0.0, 0.0, -1.5), (0.0, 0.0, 0.12), floor=None, ticks=110,
                blocks=tuple((x, -1, z, stone if x < 0 else ice) for x in range(-2, 2) for z in range(-3, 4))))
    S.append(Sc("item_seam_z_ice_stone", "item", (-1.5, 0.0, 0.0), (0.12, 0.0, 0.0), floor=None, ticks=110,
                blocks=tuple((x, -1, z, ice if z < 0 else stone) for z in range(-2, 2) for x in range(-3, 4))))
    S.append(Sc("item_seam_z_stone_ice", "item", (-1.5, 0.0, 0.0), (0.12, 0.0, 0.0), floor=None, ticks=110,
                blocks=tuple((x, -1, z, stone if z < 0 else ice) for z in range(-2, 2) for x in range(-3, 4))))
    four = {(-1, -1): ice, (0, -1): "minecraft:blue_ice", (-1, 0): stone, (0, 0): "minecraft:packed_ice"}
    for n, (vx, vz) in enumerate(((0.002, 0.0), (0.0, 0.002))):
        S.append(Sc(f"item_corner_tie_{n}", "item", (0.0, 0.0, 0.0), (vx, 0.0, vz), floor=None, ticks=40,
                    blocks=tuple((x, -1, z, b) for (x, z), b in four.items())))
    # Small velocities: 1e-7 is where a collision drops a movement, 3.16e-4 where an entity
    # stops being moved, 1e-5 what counts as a collision, 3.16e-3 when an item moves every tick.
    for n, v in enumerate((5e-8, 2e-7, 2e-4, 5e-4, 2e-3, 5e-3, 2e-2)):
        S.append(Sc(f"item_small_v_ground_{n}", "item", (0.5, 0.0, 0.5), (v, 0.0, v / 3), ticks=40))
    S.append(Sc("item_small_v_air", "item", (0.5, 6.0, 0.5), (5e-8, 0.0, -3e-8), ticks=60))
    S.append(Sc("item_small_v_wall", "item", (1.87, 0.0, 0.5), (4e-6, 0.0, 0.0), blocks=column(2, 0, 0, 1, stone), ticks=40))
    S.append(Sc("item_touching_wall", "item", (1.875, 0.0, 0.5), (0.05, 0.0, 0.02), blocks=column(2, 0, 0, 1, stone), ticks=40))
    S.append(Sc("item_up_only", "item", (0.5, 0.0, 0.5), (0.0, 0.4, 0.0), ticks=50))
    S.append(Sc("item_void", "item", (0.5, 0.0, 0.5), at=(PLOT_X0 + 24, -70, PLOT_Z0), floor=None, ticks=70))
    S.append(Sc("item_origin", "item", (0.5, 3.0, 0.5), at=(0, 0, 0), ticks=60))
    S.append(Sc("item_origin_soul_sand", "item", (0.5, 2.0, 0.5), (0.01, 0.0, 0.0), at=(8, 1, 0), floor="minecraft:soul_sand", ticks=60))
    # Shapes of more than one box, or taller than their block: nothing here can place them
    # yet, the collision code has to know them all the same.
    shapes = {
        "slab_bottom": "minecraft:stone_slab[type=bottom]", "slab_top": "minecraft:stone_slab[type=top]",
        "stairs_east": "minecraft:oak_stairs[facing=east,half=bottom]", "stairs_north_top": "minecraft:oak_stairs[facing=north,half=top]",
        "fence": "minecraft:oak_fence", "wall": "minecraft:cobblestone_wall", "bars": "minecraft:iron_bars",
        "carpet": "minecraft:white_carpet", "snow3": "minecraft:snow[layers=3]", "snow8": "minecraft:snow[layers=8]",
        "cauldron": "minecraft:cauldron", "lantern": "minecraft:lantern",
        "anvil": "minecraft:anvil[facing=north]", "farmland": "minecraft:farmland", "trapdoor": "minecraft:oak_trapdoor[half=bottom,open=false]",
        "enchanting_table": "minecraft:enchanting_table", "daylight_detector": "minecraft:daylight_detector",
        "cake": "minecraft:cake", "end_rod": "minecraft:end_rod[facing=up]", "stonecutter": "minecraft:stonecutter[facing=north]",
        "campfire": "minecraft:campfire[lit=false]", "composter": "minecraft:composter", "lectern": "minecraft:lectern[facing=north]",
        "brewing_stand": "minecraft:brewing_stand", "dirt_path": "minecraft:dirt_path", "bell": "minecraft:bell[attachment=floor,facing=north]",
        "grindstone": "minecraft:grindstone[face=floor,facing=north]", "candle": "minecraft:candle[candles=3]",
        "sea_pickle": "minecraft:sea_pickle[pickles=4,waterlogged=false]", "chain": "minecraft:iron_chain[axis=y]",
        "flower_pot": "minecraft:flower_pot", "skull": "minecraft:skeleton_skull[rotation=0]", "conduit": "minecraft:conduit[waterlogged=false]",
        "dragon_egg": "minecraft:dragon_egg", "lightning_rod": "minecraft:lightning_rod[facing=up]",
    }
    for name, block in shapes.items():
        S.append(Sc(f"item_on_{name}", "item", (0.3, 2.5, 0.6), (0.03, 0.0, -0.02), blocks=((0, 0, 0, block),), ticks=70))
        S.append(Sc(f"item_into_{name}", "item", (-1.4, 0.35, 0.45), (0.22, 0.12, 0.01), blocks=((0, 0, 0, block),), ticks=70))
    S.append(Sc("item_over_fence", "item", (-1.4, 1.1, 0.5), (0.2, 0.05, 0.0), blocks=((0, 0, 0, "minecraft:oak_fence"),), ticks=60))
    S.append(Sc("item_over_wall", "item", (-1.4, 1.3, 0.5), (0.2, 0.0, 0.0), blocks=((0, 0, 0, "minecraft:cobblestone_wall"),), ticks=60))
    S.append(Sc("item_fence_row", "item", (-1.4, 1.2, 0.2), (0.2, 0.0, 0.03),
                blocks=((0, 0, -1, "minecraft:oak_fence[south=true]"), (0, 0, 0, "minecraft:oak_fence[north=true,south=true]"),
                        (0, 0, 1, "minecraft:oak_fence[north=true]")), ticks=60))

    # Friction follows the block underneath; a fast fall looks ahead for what breaks it.
    S.append(Sc("item_ice_to_stone", "item", (-2.5, 0.0, 0.5), (0.2, 0.0, 0.0), floor=None, ticks=110,
                blocks=tuple((x, -1, z, ice if x < 0 else stone) for x in range(-3, 4) for z in range(-1, 2))))
    S.append(Sc("item_fast_fall", "item", (0.5, 9.0, 0.5), (0.0, -1.3, 0.0), ticks=40))
    S.append(Sc("item_fast_cobweb", "item", (0.5, 9.0, 0.5), (0.0, -1.3, 0.0), blocks=((0, 3, 0, "minecraft:cobweb"),), ticks=120))
    S.append(Sc("item_fast_wall", "item", (-2.5, 0.5, -2.0), (1.5, 0.2, 0.9), ticks=60,
                blocks=column(2, -1, 0, 2, stone) + column(2, 0, 0, 2, stone) + column(2, 1, 0, 2, stone) + column(2, 2, 0, 2, stone)))
    S.append(Sc("item_far_corner", "item", (-2.2, 0.5, 0.3), (0.25, 0.2, 0.1), at=(1000, 20, -1000), ticks=80))
    S.append(Sc("item_slab_floor", "item", (-1.2, 1.0, 0.3), (0.1, 0.0, 0.05), floor="minecraft:stone_slab[type=bottom]", ticks=70))
    # The world changes under an entity at rest.
    under = tuple((x, -5, z, stone) for x in range(-1, 2) for z in range(-1, 2))
    S.append(Sc("item_floor_removed", "item", (0.5, 0.0, 0.5), blocks=under, ticks=80, events=((30, 0, -1, 0, "minecraft:air"),)))
    S.append(Sc("item_floor_removed_moving", "item", (0.3, 0.0, 0.5), (0.004, 0.0, 0.0), blocks=under, ticks=80,
                events=((27, 0, -1, 0, "minecraft:air"),)))
    S.append(Sc("item_floor_to_soul_sand", "item", (0.5, 0.0, 0.5), ticks=70, events=((30, 0, -1, 0, "minecraft:soul_sand"),)))
    S.append(Sc("item_floor_to_slab", "item", (0.5, 0.0, 0.5), ticks=70, events=((33, 0, -1, 0, "minecraft:stone_slab[type=bottom]"),)))
    S.append(Sc("item_floor_to_ice", "item", (0.2, 0.0, 0.5), (0.03, 0.0, 0.0), ticks=90, events=((6, 0, -1, 0, ice),)))
    S.append(Sc("item_floor_to_slime", "item", (0.5, 0.0, 0.5), ticks=90, events=((31, 0, -1, 0, "minecraft:slime_block"),)))
    S.append(Sc("item_cobweb_placed", "item", (0.5, 0.0, 0.5), ticks=70, events=((30, 0, 0, 0, "minecraft:cobweb"),)))
    S.append(Sc("item_wall_placed", "item", (0.875, 0.0, 0.5), ticks=60, events=((30, 1, 0, 0, stone),)))
    S.append(Sc("item_floor_back", "item", (0.5, 0.0, 0.5), blocks=under, ticks=80,
                events=((30, 0, -1, 0, "minecraft:air"), (33, 0, -2, 0, stone))))

    # ------------------------------------------------------------------ primed TNT (a fuse that outlasts the recording)
    tnt = "fuse:1200s"
    S.append(Sc("tnt_drop", "tnt", (0.5, 3.0, 0.5), nbt=tnt, ticks=70))
    S.append(Sc("tnt_hop", "tnt", (0.5, 0.0, 0.5), (0.013, 0.2, -0.015), nbt=tnt, ticks=60))
    S.append(Sc("tnt_throw", "tnt", (-2.0, 0.5, 0.5), (0.3, 0.3, 0.07), nbt=tnt, ticks=80))
    S.append(Sc("tnt_ice", "tnt", (-2.0, 0.5, 0.5), (0.2, 0.0, 0.03), floor=ice, nbt=tnt, ticks=100))
    S.append(Sc("tnt_slime", "tnt", (0.5, 4.0, 0.5), (0.02, 0.0, 0.0), floor="minecraft:slime_block", nbt=tnt, ticks=140))
    S.append(Sc("tnt_soul_sand", "tnt", (-1.5, 1.0, 0.5), (0.15, 0.0, 0.0), floor="minecraft:soul_sand", nbt=tnt, ticks=70))
    S.append(Sc("tnt_wall", "tnt", (0.5, 0.2, 0.5), (0.4, 0.3, 0.05), blocks=column(2, 0, 0, 2, stone), nbt=tnt, ticks=70))
    S.append(Sc("tnt_honey_side", "tnt", (1.45, 3.5, 0.5), blocks=column(0, 0, 0, 2, "minecraft:honey_block"), nbt=tnt, ticks=80))
    S.append(Sc("tnt_honey_top", "tnt", (0.5, 2.5, 0.5), (0.1, 0.0, 0.0), floor="minecraft:honey_block", nbt=tnt, ticks=70))
    S.append(Sc("tnt_cobweb", "tnt", (0.5, 4.0, 0.5), (0.05, 0.0, 0.0),
                blocks=((0, 1, 0, "minecraft:cobweb"), (0, 2, 0, "minecraft:cobweb")), nbt=tnt, ticks=160))
    S.append(Sc("tnt_ceiling", "tnt", (0.5, 0.0, 0.5), (0.0, 0.7, 0.0), blocks=((0, 3, 0, stone),), nbt=tnt, ticks=60))
    S.append(Sc("tnt_on_fence", "tnt", (0.4, 3.0, 0.55), blocks=((0, 0, 0, "minecraft:oak_fence"),), nbt=tnt, ticks=70))
    S.append(Sc("tnt_into_slab", "tnt", (-0.8, 0.0, 0.5), (0.45, 0.0, 0.0), blocks=((0, 0, 0, "minecraft:stone_slab[type=bottom]"),), nbt=tnt, ticks=60))
    S.append(Sc("tnt_edge", "tnt", (0.3, 1.0, 0.5), (0.5, 0.0, 0.0), blocks=((0, 0, 0, stone),), nbt=tnt, ticks=70))

    S.append(Sc("tnt_floor_removed", "tnt", (0.5, 0.0, 0.5), nbt=tnt, ticks=70,
                blocks=tuple((x, -5, z, stone) for x in range(-1, 2) for z in range(-1, 2)), events=((25, 0, -1, 0, "minecraft:air"),)))
    S.append(Sc("tnt_fast_wall", "tnt", (-2.4, 0.5, 0.5), (2.0, 0.5, 0.0), blocks=column(2, 0, 0, 3, stone), nbt=tnt, ticks=50))

    # ------------------------------------------------------------------ living: armor stands (LivingEntity.travel, no brain)
    S.append(Sc("stand_drop", "armor_stand", (0.5, 5.0, 0.5), ticks=60))
    S.append(Sc("stand_rest", "armor_stand", (0.5, 0.0, 0.5), ticks=20))
    S.append(Sc("stand_push", "armor_stand", (-2.0, 0.0, 0.5), (0.4, 0.0, 0.1), ticks=60))
    S.append(Sc("stand_push_air", "armor_stand", (-2.0, 2.0, 0.5), (0.3, 0.2, -0.1), ticks=60))
    S.append(Sc("stand_ice", "armor_stand", (-2.5, 0.0, 0.5), (0.25, 0.0, 0.05), floor=ice, ticks=120))
    S.append(Sc("stand_blue_ice", "armor_stand", (-2.5, 0.0, 0.5), (0.15, 0.0, 0.0), floor="minecraft:blue_ice", ticks=160))
    S.append(Sc("stand_slime", "armor_stand", (0.5, 5.0, 0.5), (0.03, 0.0, 0.0), floor="minecraft:slime_block", ticks=160))
    S.append(Sc("stand_soul_sand", "armor_stand", (-2.0, 0.5, 0.5), (0.3, 0.0, 0.0), floor="minecraft:soul_sand", ticks=60))
    S.append(Sc("stand_honey", "armor_stand", (-2.0, 0.5, 0.5), (0.3, 0.0, 0.0), floor="minecraft:honey_block", ticks=60))
    S.append(Sc("stand_honey_side", "armor_stand", (1.2, 4.5, 0.5), blocks=column(0, 0, 0, 3, "minecraft:honey_block"), ticks=90))
    S.append(Sc("stand_wall", "armor_stand", (1.4, 0.0, 0.5), (0.5, 0.0, 0.1), blocks=column(2, 0, 0, 2, stone), ticks=40))
    S.append(Sc("stand_cobweb", "armor_stand", (0.5, 4.5, 0.5), blocks=((0, 1, 0, "minecraft:cobweb"),), ticks=160))
    S.append(Sc("stand_ceiling", "armor_stand", (0.5, 0.0, 0.5), (0.0, 0.5, 0.0), blocks=((0, 3, 0, stone),), ticks=40))
    S.append(Sc("stand_edge", "armor_stand", (0.3, 1.0, 0.5), (0.6, 0.0, 0.0), blocks=((0, 0, 0, stone),), ticks=60))
    S.append(Sc("stand_low_ceiling", "armor_stand", (-0.5, 0.0, 0.5), (0.4, 0.0, 0.0), blocks=((0, 1, 0, stone),), ticks=40))
    S.append(Sc("stand_drop_high", "armor_stand", (0.5, 12.0, 0.5), (0.01, 0.0, 0.0), ticks=70))
    S.append(Sc("stand_slab_floor", "armor_stand", (-1.0, 1.0, 0.5), (0.3, 0.0, 0.0), floor="minecraft:stone_slab[type=bottom]", ticks=50))
    S.append(Sc("stand_wall_diagonal", "armor_stand", (1.3, 0.0, -1.5), (0.4, 0.0, 0.5),
                blocks=column(2, -2, 0, 2, stone) + column(2, -1, 0, 2, stone) + column(2, 0, 0, 2, stone) + column(2, 1, 0, 2, stone), ticks=50))
    S.append(Sc("stand_seam_ice_stone", "armor_stand", (0.0, 0.0, -2.0), (0.0, 0.0, 0.5), floor=None, ticks=80,
                blocks=tuple((x, -1, z, ice if x < 0 else stone) for x in range(-2, 2) for z in range(-3, 4))))
    S.append(Sc("stand_seam_stone_ice", "armor_stand", (0.0, 0.0, -2.0), (0.0, 0.0, 0.5), floor=None, ticks=80,
                blocks=tuple((x, -1, z, stone if x < 0 else ice) for x in range(-2, 2) for z in range(-3, 4))))
    S.append(Sc("stand_ice_step", "armor_stand", (-2.0, 0.0, 0.5), (0.3, 0.0, 0.0), floor=ice, step_height=0.6, ticks=80,
                blocks=((0, 0, 0, "minecraft:stone_slab[type=bottom]"),)))
    S.append(Sc("stand_floor_removed", "armor_stand", (0.5, 0.0, 0.5), ticks=60,
                blocks=tuple((x, -5, z, stone) for x in range(-1, 2) for z in range(-1, 2)), events=((20, 0, -1, 0, "minecraft:air"),)))
    S.append(Sc("stand_small_v", "armor_stand", (0.5, 0.0, 0.5), (0.0029, 0.0, 0.0031), ticks=20))
    S.append(Sc("stand_fast_wall", "armor_stand", (-2.5, 0.0, 0.5), (3.0, 0.0, 0.2), blocks=column(2, 0, 0, 2, stone), ticks=40))
    # Stepping up: an armor stand's own step height is 0; given one it climbs like a mob. Only
    # what is on the ground steps, so these start there (OnGround:1b) or get there on the way.
    for name, block in (("slab", "minecraft:stone_slab[type=bottom]"), ("block", stone), ("stairs", "minecraft:oak_stairs[facing=east,half=bottom]"),
                        ("stairs_west", "minecraft:oak_stairs[facing=west,half=bottom]"),
                        ("carpet", "minecraft:white_carpet"), ("soul_sand", "minecraft:soul_sand"), ("snow5", "minecraft:snow[layers=5]"),
                        ("farmland", "minecraft:farmland"), ("fence", "minecraft:oak_fence"), ("trapdoor", "minecraft:oak_trapdoor[half=bottom,open=false]"),
                        ("cake", "minecraft:cake"), ("stonecutter", "minecraft:stonecutter[facing=north]"), ("snow8", "minecraft:snow[layers=8]"),
                        ("chest", "minecraft:chest[facing=north]"), ("honey", "minecraft:honey_block"), ("daylight", "minecraft:daylight_detector"),
                        ("enchanting", "minecraft:enchanting_table"), ("lantern", "minecraft:lantern"), ("cauldron", "minecraft:cauldron")):
        S.append(Sc(f"stand_step_{name}", "armor_stand", (-0.5, 0.0, 0.5), (0.4, 0.0, 0.02), blocks=((0, 0, 0, block),),
                    step_height=0.6, on_ground=True, ticks=40))
        S.append(Sc(f"stand_walk_{name}", "armor_stand", (-1.15, 0.0, 0.45), (0.4, 0.0, 0.01), blocks=((0, 0, 0, block),),
                    step_height=0.6, ticks=40))
        S.append(Sc(f"stand_nostep_{name}", "armor_stand", (-0.5, 0.0, 0.5), (0.4, 0.0, 0.02), blocks=((0, 0, 0, block),),
                    on_ground=True, ticks=30))
    S.append(Sc("stand_step_two_slabs", "armor_stand", (-1.6, 0.0, 0.5), (0.9, 0.0, 0.0), step_height=0.6, on_ground=True, ticks=50,
                blocks=((-1, 0, 0, "minecraft:stone_slab[type=bottom]"), (0, 0, 0, stone))))
    S.append(Sc("stand_step_high", "armor_stand", (-0.5, 0.0, 0.5), (0.4, 0.0, 0.0), blocks=((0, 0, 0, stone),), step_height=1.0,
                on_ground=True, ticks=40))
    S.append(Sc("stand_step_higher", "armor_stand", (-0.5, 0.0, 0.5), (0.4, 0.0, 0.0), blocks=column(0, 0, 0, 1, stone), step_height=1.5,
                on_ground=True, ticks=40))
    S.append(Sc("stand_step_airborne", "armor_stand", (-0.5, 0.3, 0.5), (0.4, 0.0, 0.0), step_height=0.6, ticks=40,
                blocks=((0, 0, 0, "minecraft:stone_slab[type=bottom]"),)))
    S.append(Sc("stand_step_falling", "armor_stand", (-0.4, 1.0, 0.5), (0.3, 0.0, 0.0), step_height=0.6, ticks=40,
                blocks=((0, 0, 0, "minecraft:stone_slab[type=bottom]"),)))
    S.append(Sc("stand_step_landing", "armor_stand", (-0.6, 0.05, 0.5), (0.4, -0.2, 0.0), step_height=0.6, ticks=40,
                blocks=((0, 0, 0, "minecraft:stone_slab[type=bottom]"),)))
    S.append(Sc("stand_step_diagonal", "armor_stand", (-0.5, 0.0, -0.5), (0.4, 0.0, 0.4), step_height=0.6, on_ground=True, ticks=40,
                blocks=((0, 0, 0, "minecraft:stone_slab[type=bottom]"), (0, 0, 1, stone), (1, 0, 0, stone))))
    S.append(Sc("stand_step_corner", "armor_stand", (-0.5, 0.0, -0.5), (0.4, 0.0, 0.3), step_height=0.6, on_ground=True, ticks=40,
                blocks=((0, 0, 0, "minecraft:stone_slab[type=bottom]"),)))
    S.append(Sc("stand_step_under_ceiling", "armor_stand", (-0.5, 0.0, 0.5), (0.4, 0.0, 0.0), step_height=0.6, on_ground=True, ticks=40,
                blocks=((0, 0, 0, "minecraft:stone_slab[type=bottom]"), (0, 2, 0, stone), (-1, 2, 0, stone))))
    S.append(Sc("stand_step_headroom", "armor_stand", (-0.5, 0.0, 0.5), (0.4, 0.0, 0.0), step_height=0.6, on_ground=True, ticks=40,
                blocks=((0, 0, 0, "minecraft:white_carpet"), (0, 2, 0, "minecraft:stone_slab[type=top]"),
                        (-1, 2, 0, "minecraft:stone_slab[type=top]"))))
    S.append(Sc("stand_step_slime", "armor_stand", (-0.9, 2.0, 0.5), (0.12, 0.0, 0.0), step_height=0.6, floor="minecraft:slime_block", ticks=100,
                blocks=((0, 0, 0, "minecraft:stone_slab[type=bottom]"),)))
    S.append(Sc("stand_step_staircase", "armor_stand", (-2.6, 0.0, 0.5), (1.3, 0.0, 0.0), step_height=0.6, on_ground=True, ticks=60,
                blocks=((-2, 0, 0, "minecraft:stone_slab[type=bottom]"), (-1, 0, 0, stone), (0, 0, 0, stone),
                        (0, 1, 0, "minecraft:stone_slab[type=bottom]"), (1, 0, 0, stone), (1, 1, 0, stone))))

    # ------------------------------------------------------------------ living: mobs that cannot walk (movement_speed 0)
    still = 'attributes:[{id:"minecraft:movement_speed",base:0.0d}],PersistenceRequired:1b'
    S.append(Sc("pig_drop", "pig", (0.5, 4.0, 0.5), nbt=still, ticks=40))
    S.append(Sc("pig_slab", "pig", (-0.8, 0.0, 0.5), (0.4, 0.0, 0.0), blocks=((0, 0, 0, "minecraft:stone_slab[type=bottom]"),), nbt=still,
                on_ground=True, ticks=30))
    S.append(Sc("pig_walk_slab", "pig", (-1.35, 0.0, 0.5), (0.4, 0.0, 0.0), blocks=((0, 0, 0, "minecraft:stone_slab[type=bottom]"),), nbt=still, ticks=30))
    S.append(Sc("pig_wall", "pig", (1.2, 0.0, 0.5), (0.4, 0.0, 0.1), blocks=column(2, 0, 0, 1, stone), nbt=still, ticks=30))
    S.append(Sc("pig_pair", "pig", (0.2, 0.0, 0.5), nbt=still, ticks=30, extra=(((0.7, 0.0, 0.6), (0.0, 0.0, 0.0)),)))
    S.append(Sc("pig_pair_again", "pig", (0.2, 0.0, 0.5), nbt=still, ticks=30, extra=(((0.7, 0.0, 0.6), (0.0, 0.0, 0.0)),)))
    S.append(Sc("pig_trio", "pig", (0.0, 0.0, 0.0), nbt=still, ticks=30,
                extra=(((0.5, 0.0, 0.1), (0.0, 0.0, 0.0)), ((0.2, 0.0, 0.6), (0.0, 0.0, 0.0)))))
    S.append(Sc("pig_same_spot", "pig", (0.5, 0.0, 0.5), nbt=still, ticks=20, extra=(((0.5, 0.0, 0.5), (0.0, 0.0, 0.0)),)))
    S.append(Sc("pig_stand", "pig", (0.5, 0.0, 0.5), nbt=still, ticks=20,
                extra=(((0.8, 0.0, 0.5), (0.0, 0.0, 0.0)),)))

    # ------------------------------------------------------------------ an obstacle course, and things thrown about in it
    import random
    rng = random.Random(26_3)
    # ------------------------------------------------------------------ falling blocks
    # A FallingBlockEntity is gone the tick it touches the ground - a block again, or broken on what
    # does not give way: what is recorded is its fall, and the tick it ends in.
    fb = "falling_block"
    S.append(Sc("falling_drop", fb, (0.5, 3.0, 0.5), ticks=40))
    S.append(Sc("falling_drop_from_rest_height", fb, (0.5, 1.0, 0.5), ticks=20))
    S.append(Sc("falling_offcentre", fb, (0.31, 4.7, 0.83), ticks=40))
    S.append(Sc("falling_thrown", fb, (-2.0, 1.5, 0.3), (0.21, 0.35, 0.08), ticks=60))
    S.append(Sc("falling_thrown_negative", fb, (2.1, 2.25, 0.6), (-0.17, 0.2, -0.13), ticks=60))
    S.append(Sc("falling_fast", fb, (0.5, 9.0, 0.5), (0.0, -1.3, 0.0), ticks=30))
    S.append(Sc("falling_wall", fb, (-1.5, 2.0, 0.5), (0.4, 0.3, 0.0), blocks=column(2, 0, 0, 3, stone), ticks=60))
    S.append(Sc("falling_ceiling", fb, (0.5, 0.5, 0.5), (0.01, 0.7, 0.0), blocks=((0, 3, 0, stone),), ticks=60))
    S.append(Sc("falling_cobweb", fb, (0.5, 4.0, 0.5), (0.03, 0.0, 0.01), blocks=((0, 2, 0, "minecraft:cobweb"),), ticks=160))
    S.append(Sc("falling_slime", fb, (0.5, 4.0, 0.5), floor="minecraft:slime_block", ticks=50))
    S.append(Sc("falling_honey_side", fb, (1.48, 3.5, 0.5), blocks=column(0, 0, 0, 2, "minecraft:honey_block"), ticks=70))
    S.append(Sc("falling_onto_slab", fb, (0.5, 3.0, 0.5), blocks=((0, 0, 0, "minecraft:stone_slab[type=bottom]"),), ticks=40))
    S.append(Sc("falling_onto_fence", fb, (0.5, 4.0, 0.5), blocks=((0, 0, 0, "minecraft:oak_fence"),), ticks=40))
    S.append(Sc("falling_past_edge", fb, (1.3, 3.0, 0.5), blocks=((0, 0, 0, stone), (0, 1, 0, stone)), ticks=50))
    S.append(Sc("falling_ice_slide", fb, (-2.0, 0.6, 0.5), (0.3, 0.0, 0.05), floor=ice, ticks=40))
    S.append(Sc("falling_origin", fb, (0.5, 3.0, 0.5), at=(0, 0, 8), ticks=40))

    floors = [stone, stone, ice, "minecraft:soul_sand", "minecraft:slime_block", "minecraft:honey_block", "minecraft:packed_ice", "minecraft:mud"]
    obstacles = ["minecraft:stone_slab[type=bottom]", "minecraft:oak_stairs[facing=east,half=bottom]", "minecraft:oak_fence",
                 "minecraft:cobblestone_wall", "minecraft:chest[facing=north]", "minecraft:cobweb", "minecraft:white_carpet",
                 "minecraft:snow[layers=4]", stone, "minecraft:oak_stairs[facing=south,half=top]", "minecraft:iron_bars",
                 "minecraft:stone_slab[type=top]", "minecraft:anvil[facing=east]", "minecraft:cauldron", "minecraft:end_rod[facing=up]"]

    def course(k: int) -> tuple:
        r = random.Random(1000 + k)
        blocks = [(x, -1, z, r.choice(floors)) for x in range(-3, 4) for z in range(-3, 4)]
        used = set()
        for _ in range(9):
            x, z = r.randint(-3, 3), r.randint(-3, 3)
            if (x, z) in used or (x, z) == (0, 0):
                continue
            used.add((x, z))
            blocks.append((x, 0, z, r.choice(obstacles)))
            if r.random() < 0.25:
                blocks.append((x, 1, z, r.choice(obstacles)))
        if r.random() < 0.5:    # something overhead, but not where the entity is summoned
            x, z = r.choice([(x, z) for x in (-1, 0, 1) for z in (-1, 0, 1) if (x, z) != (0, 0)])
            blocks.append((x, 2, z, r.choice([stone, "minecraft:stone_slab[type=top]", "minecraft:cobweb"])))
        return tuple(blocks)

    def throw(r, up: float) -> tuple:
        return (round(r.uniform(-0.45, 0.45), 3), round(r.uniform(-0.1, up), 3), round(r.uniform(-0.45, 0.45), 3))

    for k in range(40):
        r = random.Random(2000 + k)
        S.append(Sc(f"course_item_{k}", "item", (round(r.uniform(0.2, 0.8), 3), round(r.uniform(0.3, 2.5), 3), round(r.uniform(0.2, 0.8), 3)),
                    throw(r, 0.5), blocks=course(k % 10), floor=None, ticks=120))
    for k in range(16):
        r = random.Random(3000 + k)
        S.append(Sc(f"course_tnt_{k}", "tnt", (0.5, round(r.uniform(0.0, 2.5), 3), 0.5), throw(r, 0.5), blocks=course(k % 10), floor=None,
                    nbt=tnt, ticks=120))
    for k in range(30):
        r = random.Random(4000 + k)
        S.append(Sc(f"course_stand_{k}", "armor_stand", (0.5, round(r.choice([0.0, 0.0, r.uniform(0.0, 1.5)]), 3), 0.5), throw(r, 0.3),
                    blocks=course(k % 10), floor=None, step_height=r.choice([None, 0.6, 0.6, 1.0]), ticks=100))
    return S


def place(scs: list[Sc]) -> None:
    """Give every scenario its plot: `origin`, the block above the middle of its floor."""
    slots = [(i, j) for j in range(-2, 3) for i in range(-2, 3) if (i, j) != (0, 0)]
    n = 0
    for sc in scs:
        if sc.at is not None:
            sc.origin = sc.at
            continue
        level, k = divmod(n, len(slots))
        if level >= PLOT_LEVELS:
            raise SystemExit("more scenarios than plots: add levels")
        sc.origin = (PLOT_X0 + slots[k][0] * 8, PLOT_Y0 + level * PLOT_DY, PLOT_Z0 + slots[k][1] * 8)
        n += 1


# ----------------------------------------------------------------------------- the server

LINE = re.compile(r"^\[\d\d:\d\d:\d\d\] \[([^\]]*)\]: (.*)$")


class Vanilla:
    def __init__(self, log_path: Path):
        eula = SERVER_DIR / "eula.txt"
        if not eula.exists() or "eula=true" not in eula.read_text():
            raise SystemExit(f"{eula} is missing or not accepted: that is the user's to do (AGENTS.md)")
        props = (SERVER_DIR / "server.properties").read_text()
        for must in ("server-ip=127.0.0.1", "online-mode=false", f"server-port={PORT}"):
            if must not in props:
                raise SystemExit(f"server.properties must have {must}")
        jar = ROOT / ".cache" / "mc" / VERSION / "server.jar"
        self.log = open(log_path, "w")
        self.proc = subprocess.Popen([JAVA, "-Xms1G", "-Xmx2G", "-jar", str(jar), "--nogui"], cwd=SERVER_DIR, text=True, bufsize=1,
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.lines: queue.Queue[str] = queue.Queue()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self) -> None:
        for line in self.proc.stdout:
            self.log.write(line)
            m = LINE.match(line.rstrip("\n"))
            text = m.group(2) if m else line.rstrip("\n")
            # What a command answers is logged as "System chat: ...", what /say says as "[Not Secure] [Server] ...".
            self.lines.put(text.removeprefix("System chat: "))
        self.lines.put("<<EOF>>")

    def cmd(self, *lines: str) -> None:
        for line in lines:
            self.log.write(f">>> {line}\n")
        self.proc.stdin.write("".join(line + "\n" for line in lines))
        self.proc.stdin.flush()

    def until(self, pattern: str, timeout: float = 60.0) -> tuple[re.Match, list[str]]:
        """The first line that matches, and the lines before it."""
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
            m = rx.search(line)
            if m:
                return m, before
            before.append(line)

    def stop(self) -> None:
        try:
            self.cmd("stop")
            self.proc.wait(timeout=60)
        except Exception:
            self.proc.terminate()
        self.log.close()


class Watcher(Bot):
    """The bot: keeps the server awake, and keeps what it is shown of the entities."""

    def __init__(self, name: str):
        super().__init__(PORT, name)
        self.packets: list[tuple[str, str]] = []
        self.ids: dict[str, int] = {}       # uuid (hex) -> entity id
        self.lock = threading.Lock()
        self.alive = True

    def run(self) -> None:
        c = self.c
        while self.alive:
            c.sock.settimeout(0.05)
            try:
                got, r = c.recv_any()
            except (socket.timeout, TimeoutError):
                continue
            except Exception:
                return
            data = r.d[r.i:]
            if got == "player_position":
                tid = r.varint()
                x, y, z = r.f64(), r.f64(), r.f64()
                r.take(24)
                yaw, pitch = r.f32(), r.f32()
                c.send("accept_teleportation", varint(tid) + struct.pack(">dddff", x, y, z, yaw, pitch))
            elif got == "keep_alive":
                c.send("keep_alive", data)
            elif got == "chunk_batch_finished":
                c.send("chunk_batch_received", struct.pack(">f", 20.0))
            elif got == "add_entity":
                eid = r.varint()
                with self.lock:
                    self.ids[r.take(16).hex()] = eid
            if got in mc.ENTITY_PACKETS or got in ("disguised_chat", "system_chat"):
                with self.lock:
                    self.packets.append((got, data.hex()))


# ----------------------------------------------------------------------------- record

NUM = r"(-?[0-9.E-]+|NaN|-?Infinity)"
POS = re.compile(r"\bPos: \[" + NUM + "d, " + NUM + "d, " + NUM + r"d\]")
MOTION = re.compile(r"\bMotion: \[" + NUM + "d, " + NUM + "d, " + NUM + r"d\]")
GROUND = re.compile(r"\bOnGround: (\d)b")
FALL = re.compile(r"\bfall_distance: " + NUM + "d")
FUSE = re.compile(r"\bfuse: (-?\d+)s")
DUMP = re.compile(r"^(e\d+) has the following entity data: (.*)$")
KIND_ID = {"item": "minecraft:item", "tnt": "minecraft:tnt", "armor_stand": "minecraft:armor_stand", "pig": "minecraft:pig",
           "falling_block": "minecraft:falling_block"}


RUN = int(time.time()) & 0x7FFFFFFF      # in every UUID: vanilla refuses one it has seen, even on a dead entity


def uuid_of(n: int) -> tuple[str, str]:
    ints = (0x50485953, RUN, 0, n)
    return "[I;" + ",".join(str(v) for v in ints) + "]", "".join(f"{v:08x}" for v in ints)


def record(out_path: str, only: str | None) -> None:
    scs = [s for s in scenarios() if not only or re.search(only, s.name)]
    place(scs)
    log_path = Path(out_path).with_suffix(".server.log")
    v = Vanilla(log_path)
    bot = None
    try:
        v.until(r"Done \(", 180)
        v.cmd("tick freeze")
        v.until(r"^The game is frozen", 20)
        bot = Watcher("PhysWatch")
        bot.join()
        threading.Thread(target=bot.run, daemon=True).start()
        bot.c.send("player_loaded")
        v.cmd(f"tp PhysWatch {fixed(BOT_AT[0])} {fixed(BOT_AT[1])} {fixed(BOT_AT[2])}")
        v.until(r"Teleported PhysWatch", 20)
        far = sorted({(sc.origin[0] >> 4, sc.origin[2] >> 4) for sc in scs if sc.at is not None})
        for cx, cz in far:
            v.cmd(f"forceload add {cx * 16 - 16} {cz * 16 - 16} {cx * 16 + 31} {cz * 16 + 31}")
        time.sleep(1.5)     # the chunks around the bot load
        # What an aborted run left behind: gone before anything is summoned (a mob takes a
        # second to die, and only while the game runs).
        v.cmd("tick unfreeze", "kill @e[tag=phys]")
        time.sleep(1.6)
        v.cmd("tick freeze")
        v.until(r"^The game is frozen", 20)

        # ---- fixtures
        entities = []   # (tag, scenario index, entity index in the scenario)
        for si, sc in enumerate(scs):
            ox, oy, oz = sc.origin
            sc.abs_blocks = []
            if sc.floor:
                v.cmd(f"fill {ox - 3} {oy - 1} {oz - 3} {ox + 3} {oy - 1} {oz + 3} {sc.floor}")
                sc.abs_blocks += [(ox + dx, oy - 1, oz + dz, sc.floor) for dx in range(-3, 4) for dz in range(-3, 4)]
            for dx, dy, dz, block in sc.blocks:
                v.cmd(f"setblock {ox + dx} {oy + dy} {oz + dz} {block}")
                sc.abs_blocks = [b for b in sc.abs_blocks if b[:3] != (ox + dx, oy + dy, oz + dz)]
                sc.abs_blocks.append((ox + dx, oy + dy, oz + dz, block))
        v.cmd("say FIXTURES")
        _, before = v.until(r"\[Server\] FIXTURES", 60)
        bad = [line for line in before if "<--[HERE]" in line or "Unknown" in line or "Incorrect" in line or "Expected" in line]
        if bad:
            raise SystemExit("a fixture command failed:\n  " + "\n  ".join(bad[:10]))

        # ---- entities, in one batch: /summon hands out ids in order
        for si, sc in enumerate(scs):
            ox, oy, oz = sc.origin
            sc.ents = []
            for ei, (pos, motion) in enumerate(((sc.pos, sc.motion),) + tuple(sc.extra)):
                tag = f"e{len(entities)}"
                snbt_uuid, hex_uuid = uuid_of(len(entities) + 1)
                apos = (ox + pos[0], oy + pos[1], oz + pos[2])
                nbt = [f'Tags:["phys"]', f'CustomName:"{tag}"', f"UUID:{snbt_uuid}",
                       "Motion:[" + ",".join(fixed(m) + "d" for m in motion) + "]"]
                if sc.kind == "item":
                    nbt += ['Item:{id:"minecraft:stone",count:1}', "PickupDelay:32767s"]
                if sc.kind == "falling_block":
                    # Sand, which leaves no item where it cannot become a block again (26.x: a default
                    # state is its block's name alone).
                    nbt += ['BlockState:"minecraft:sand"', "DropItem:0b"]
                if sc.on_ground:
                    nbt.append("OnGround:1b")
                attrs = []
                if sc.step_height is not None:
                    attrs.append(f'{{id:"minecraft:step_height",base:{fixed(sc.step_height)}d}}')
                extra = sc.nbt
                if attrs:
                    nbt.append("attributes:[" + ",".join(attrs) + "]")
                if extra:
                    nbt.append(extra)
                v.cmd(f"summon {KIND_ID[sc.kind]} {fixed(apos[0])} {fixed(apos[1])} {fixed(apos[2])} {{{','.join(nbt)}}}")
                sc.ents.append({"tag": tag, "uuid": hex_uuid, "pos": apos, "motion": tuple(float(m) for m in motion), "states": []})
                entities.append((tag, si, ei))
        by_tag = {tag: scs[si].ents[ei] for tag, si, ei in entities}
        max_ticks = max(sc.ticks for sc in scs)
        ticks_of = {tag: scs[si].ticks for tag, si, _ in entities}

        # ---- the ticks. `tick step` only arms the step: the game takes it in its next tick, and
        # a command sent right behind it may still be read before that. So a clock is summoned
        # with the others - a primed TNT that hangs in the air and counts its fuse down, one a
        # tick - and a dump counts for the tick its clock shows.
        clock_at = (BOT_AT[0] + 0.5, 250.0, BOT_AT[2] + 0.5)
        v.cmd(f"summon minecraft:tnt {fixed(clock_at[0])} {fixed(clock_at[1])} {fixed(clock_at[2])} "
              '{Tags:["phys"],CustomName:"clock",NoGravity:1b,fuse:32000s}')
        marks = 0

        def dump() -> tuple[int, dict[str, str]]:
            nonlocal marks
            marks += 1
            v.cmd("execute as @e[tag=phys] run data get entity @s", f"say MARK {marks}")
            _, before = v.until(rf"\[Server\] MARK {marks}$", 60)
            got: dict[str, str] = {}
            clock = None
            for line in before:
                if line.startswith("clock has the following entity data: "):
                    clock = int(FUSE.search(line).group(1))
                    continue
                m = DUMP.match(line)
                if m:
                    got[m.group(1)] = m.group(2)
                elif "<--[HERE]" in line or "Unable to summon" in line or "Invalid" in line:
                    raise SystemExit(f"the server said: {line}")
            if clock is None:
                raise SystemExit("the clock is gone")
            return 32000 - clock, got

        t = 0
        while True:
            ticked, got = dump()
            if ticked == t - 1:         # the step has not been taken yet
                time.sleep(0.03)
                continue
            if ticked != t:
                raise SystemExit(f"the clock shows tick {ticked}, expected {t}")
            for tag, nbt in got.items():
                if t > ticks_of[tag]:
                    continue
                pos, mot = POS.search(nbt), MOTION.search(nbt)
                state = {"pos": [float(pos.group(i)) for i in (1, 2, 3)], "motion": [float(mot.group(i)) for i in (1, 2, 3)],
                         "on_ground": GROUND.search(nbt).group(1) == "1", "fall": float(FALL.search(nbt).group(1))}
                fuse = FUSE.search(nbt)
                if fuse:
                    state["fuse"] = int(fuse.group(1))
                by_tag[tag]["states"].append(state)
            if t == 0 and len(got) != len(entities):
                raise SystemExit(f"{len(entities) - len(got)} entities were not summoned: {sorted(set(by_tag) - set(got))[:10]}")
            for tag, e in by_tag.items():
                if tag not in got and t <= ticks_of[tag] and len(e["states"]) == t:
                    e["states"].append(None)        # gone: discarded in this tick
            if t % 20 == 0:
                print(f"tick {t} / {max_ticks}: {len(got)} entities", flush=True)
            if t == max_ticks:
                break
            for sc in scs:
                for when, dx, dy, dz, block in sc.events:
                    if when == t:
                        v.cmd(f"setblock {sc.origin[0] + dx} {sc.origin[1] + dy} {sc.origin[2] + dz} {block}")
            t += 1
            v.cmd("tick step 1")
            v.until(r"^Stepping 1 tick", 20)
            time.sleep(0.06)

        with bot.lock:
            ids = dict(bot.ids)
        # Ids of what the bot was not shown, counted from a neighbour's.
        known = [(i, ids[by_tag[tag]["uuid"]]) for i, (tag, _, _) in enumerate(entities) if by_tag[tag]["uuid"] in ids]
        if not known:
            raise SystemExit("the bot was shown none of the entities: no entity ids")
        base = known[0][1] - known[0][0]
        if any(eid - i != base for i, eid in known):
            raise SystemExit("entity ids are not in the order of the summons")
        for i, (tag, _, _) in enumerate(entities):
            by_tag[tag]["id"] = base + i
    finally:
        try:
            v.cmd("kill @e[tag=phys]")
            for sc in scs:
                ox, oy, oz = sc.origin
                v.cmd(f"fill {ox - 3} {oy - 6} {oz - 3} {ox + 3} {oy + 5} {oz + 3} minecraft:air")
            v.cmd("tick unfreeze")
            time.sleep(1.6)     # mobs die
            v.cmd("forceload remove all", "say CLEAN")
            v.until(r"\[Server\] CLEAN", 60)
        except SystemExit as e:
            print(f"cleanup: {e}")
        if bot:
            bot.alive = False
            bot.c.close()
        v.stop()

    out = {"version": VERSION, "scenarios": [
        {"name": sc.name, "kind": sc.kind, "origin": sc.origin, "ticks": sc.ticks, "step_height": sc.step_height, "on_ground": sc.on_ground,
         "blocks": [list(b) for b in sorted(sc.abs_blocks)], "entities": sc.ents,
         "events": [[when, sc.origin[0] + dx, sc.origin[1] + dy, sc.origin[2] + dz, block] for when, dx, dy, dz, block in sc.events]}
        for sc in scs]}
    Path(out_path).write_text(json.dumps(out))
    print(f"wrote {out_path}: {len(scs)} scenarios, {len(entities)} entities, {max_ticks} ticks")


# ----------------------------------------------------------------------------- emit

KINDS = ["item", "tnt", "armor_stand", "pig", "falling_block"]


def emit(in_path: str, out_path: str) -> None:
    """The recording as tests/unit/data/physics_vectors.bin, which physics_test.cpp embeds and
    reads (the layout is written out there). An entity's states are cut 26 ticks after it last
    moved: that is six rounds of an item's four-tick rest, and the file is a third smaller.

    A recording need not be of everything: `record --only '^falling_'` and `emit` into
    tests/unit/data/physics_falling.bin is how the falling blocks were added beside the rest."""
    rec = json.load(open(in_path))

    def text(v: str) -> bytes:
        raw = v.encode()
        return struct.pack("<B", len(raw)) + raw

    # Where the server ticked entities: within its simulation distance of the bot, and in the
    # chunks that were force-loaded for the scenarios that stand elsewhere. An entity that
    # leaves those stops, in the recording as on any vanilla server.
    known = {sc.name: sc for sc in scenarios()}
    recorded = [sc for sc in rec["scenarios"] if sc["name"] in known]    # (a scenario taken off the list since is left out)
    distance = int(re.search(r"^simulation-distance=(\d+)$", (SERVER_DIR / "server.properties").read_text(), re.M).group(1))
    forced = sorted({(sc["origin"][0] >> 4, sc["origin"][2] >> 4) for sc in recorded if known[sc["name"]].at is not None})
    o = bytearray(b"PHYSVEC1" + struct.pack("<iiiI", int(BOT_AT[0] // 16), int(BOT_AT[2] // 16), distance, len(forced)))
    for cx, cz in forced:
        o += struct.pack("<iiii", cx - 1, cz - 1, cx + 1, cz + 1)
    o += struct.pack("<I", len(recorded))
    states = 0
    for sc in recorded:
        step = sc.get("step_height")
        events = sc.get("events", [])
        o += text(sc["name"]) + struct.pack("<BBd", KINDS.index(sc["kind"]), 1 if sc.get("on_ground") else 0, -1.0 if step is None else step)
        o += struct.pack("<I", len(sc["blocks"]))
        for x, y, z, block in sc["blocks"]:
            o += struct.pack("<iii", x, y, z) + text(block)
        o += struct.pack("<I", len(events))
        for when, x, y, z, block in events:
            o += struct.pack("<Iiii", when, x, y, z) + text(block)
        o += struct.pack("<I", len(sc["entities"]))
        keep = 0
        for e in sc["entities"]:
            st = e["states"]
            last = 0
            for t in range(1, len(st)):
                if st[t] is None or st[t - 1] is None or st[t]["pos"] != st[t - 1]["pos"] or st[t]["on_ground"] != st[t - 1]["on_ground"]:
                    last = t
            keep = max(keep, last)
        keep = max([keep] + [when for when, *_ in events]) + 26
        for e in sc["entities"]:
            st = e["states"][:keep + 1]
            o += struct.pack("<iI", e["id"], len(st))
            for s_ in st:
                if s_ is None:
                    o += struct.pack("<Bi7d", 2, 0, *([0.0] * 7))
                else:
                    o += struct.pack("<Bi7d", 1 if s_["on_ground"] else 0, s_.get("fuse", 0), *s_["pos"], *s_["motion"], s_["fall"])
            states += len(st)
    Path(out_path).write_bytes(bytes(o))
    print(f"wrote {out_path}: {len(recorded)} scenarios, {states} states, {len(o)} bytes")


# ----------------------------------------------------------------------------- how long it takes

# The field of `perf`: far from the bot (nothing there is shown to it), force-loaded so that
# what is in it ticks. Entities stand on a grid 1.5 blocks apart: nothing touches, no item is
# near enough to another to merge with it.
PERF_X0, PERF_Z0, PERF_SIDE = 4000, 4000, 100
PERF_CASES = [
    # name, entity, nbt, y, ticks to let it settle, ticks measured
    ("items at rest", "item", 'Item:{id:"minecraft:stone",count:1}', -60.0, 300, 1000),
    ("items falling", "item", 'Item:{id:"minecraft:stone",count:1}', 1200.0, 20, 300),
    ("armor stands at rest", "armor_stand", "", -60.0, 200, 1000),
    ("armor stands falling", "armor_stand", "", 1500.0, 20, 200),
    ("primed TNT at rest", "tnt", "fuse:32000s", -60.0, 200, 1000),
]


def perf(out_path: str, count: int) -> None:
    """What a tick costs a vanilla server with `count` entities in it that it would not have
    otherwise: `/tick sprint` runs ticks back to back and says how long one took; the same
    without the entities is taken off. The server's profiler (`/debug`) says where in the tick
    it went - under its own overhead, which is why the first number is the one to quote."""
    side = int(count ** 0.5)
    count = side * side
    assert side <= PERF_SIDE
    log_path = Path(out_path).with_suffix(".server.log")
    v = Vanilla(log_path)
    bot = None
    results = {"count": count, "version": VERSION, "cases": {}}
    try:
        v.until(r"Done \(", 180)
        bot = Watcher("PhysBench")
        bot.join()
        threading.Thread(target=bot.run, daemon=True).start()
        bot.c.send("player_loaded")
        v.cmd(f"tp PhysBench {fixed(BOT_AT[0])} {fixed(BOT_AT[1])} {fixed(BOT_AT[2])}")
        v.until(r"Teleported PhysBench", 20)
        x1, z1 = PERF_X0 + int(side * 1.5) + 16, PERF_Z0 + int(side * 1.5) + 16
        v.cmd(f"forceload add {PERF_X0 - 16} {PERF_Z0 - 16} {x1} {z1}", "kill @e[tag=physbench]", "say LOADED")
        v.until(r"\[Server\] LOADED", 120)
        time.sleep(3.0)

        def sprint(ticks: int) -> float:
            v.cmd(f"tick sprint {ticks}")
            m, _ = v.until(r"Sprint completed with ([\d.]+) ticks per second, or ([\d.]+) ms per tick", 600)
            return float(m.group(2))

        def profile(ticks: int) -> list[str]:
            """The profiler's account of `ticks` ticks: the lines that are about entities."""
            before = set((SERVER_DIR / "debug").glob("profile-results-*.txt")) if (SERVER_DIR / "debug").exists() else set()
            v.cmd("debug start")
            v.until(r"Started tick profiling", 20)
            sprint(ticks)
            v.cmd("debug stop")
            v.until(r"Stopped tick profiling", 60)
            new = sorted(set((SERVER_DIR / "debug").glob("profile-results-*.txt")) - before)
            if not new:
                return []
            text = new[-1].read_text().splitlines()
            keep = [line for line in text[:12]]
            keep += [line for line in text if re.search(r"\] [| ]*(tick|levels|entities|minecraft:(item|armor_stand|tnt)|chunkSource|tracker|"
                                                        r"move|rest|blockEntities|global|ServerLevel\S*|entityTracking?|broadcast)\(", line)]
            new[-1].unlink()        # ours to make, ours to take away
            return keep[:80]

        sprint(400)                 # the JIT has seen a tick
        idle = min(sprint(1000), sprint(1000))
        results["idle_ms_per_tick"] = idle
        print(f"nothing: {idle:.4f} ms per tick")
        for name, kind, nbt, y, settle, ticks in PERF_CASES:
            tags = 'Tags:["physbench"]'
            body = "{" + (nbt + "," if nbt else "") + tags + "}"
            lines = [f"summon {kind} {fixed(PERF_X0 + 0.5 + i * 1.5)} {fixed(y)} {fixed(PERF_Z0 + 0.5 + j * 1.5)} {body}"
                     for i in range(side) for j in range(side)]
            for at in range(0, len(lines), 500):
                v.cmd(*lines[at:at + 500])
            v.cmd("say SUMMONED")
            _, before = v.until(r"\[Server\] SUMMONED", 300)
            bad = [line for line in before if "<--[HERE]" in line or "Unknown" in line or "Incorrect" in line or "Unable" in line]
            if bad:
                raise SystemExit("a summon failed:\n  " + "\n  ".join(bad[:5]))
            sprint(settle)
            took = [sprint(ticks)]
            if "falling" not in name:            # (what falls has landed by a second run)
                took.append(sprint(ticks))
                prof = profile(400)
            else:
                prof = []
            v.cmd("execute if entity @e[tag=physbench]")
            m, _ = v.until(r"Test (passed|failed)(?:, count: (\d+))?", 60)
            left = int(m.group(2) or 0)
            v.cmd("kill @e[tag=physbench]", "say KILLED")
            v.until(r"\[Server\] KILLED", 120)
            sprint(40)              # armor stands take a tick to go
            ms = min(took)
            results["cases"][name] = {"ms_per_tick": ms, "ns_per_entity_tick": (ms - idle) * 1e6 / count, "runs": took,
                                      "entities_at_the_end": left, "profile": prof}
            print(f"{name}: {ms:.4f} ms per tick with {count} of them ({left} left at the end) -> "
                  f"{(ms - idle) * 1e6 / count:.0f} ns per entity per tick")
        results["idle_after_ms_per_tick"] = sprint(1000)
        v.cmd(f"forceload remove {PERF_X0 - 16} {PERF_Z0 - 16} {x1} {z1}")
    finally:
        if bot:
            bot.alive = False
        v.stop()
    Path(out_path).write_text(json.dumps(results, indent=1))
    print(f"wrote {out_path}")


# ----------------------------------------------------------------------------- what a viewer is told

def wire(out_path: str) -> None:
    """What a vanilla server tells a player about an item that is put on the ground beside it and
    lies there: every packet about it for 15 seconds. An item at rest falls for three ticks and
    is stopped by the ground in the fourth (ItemEntity.tick moves it only every fourth tick),
    and each time that is a change of velocity its viewers hear of. Recorded on 26.3: of four
    items made a tick apart, three are sent a velocity twice a second - (0, -0.04, 0) and
    (0, 0, 0) in turn for the first, -0.08 and -0.12 for the next two - and the fourth never."""
    v = Vanilla(Path(out_path).with_suffix(".server.log"))
    bot = None
    out = {"items": []}
    try:
        v.until(r"Done \(", 180)
        bot = Watcher("PhysWire")
        bot.join()
        threading.Thread(target=bot.run, daemon=True).start()
        bot.c.send("player_loaded")
        v.cmd(f"tp PhysWire {fixed(BOT_AT[0])} {fixed(BOT_AT[1])} {fixed(BOT_AT[2])}")
        v.until(r"Teleported PhysWire", 20)
        time.sleep(2.0)
        with bot.lock:
            bot.packets.clear()
        t0 = time.time()
        # Four of them, a tick apart: where in its round of four an item's update falls depends on when it was made.
        for i in range(4):
            v.cmd(f"summon item {fixed(BOT_AT[0] + 3.5 + i)} {fixed(BOT_AT[1])} {fixed(BOT_AT[2] + 3.5)} "
                  '{Item:{id:"minecraft:stone",count:1},PickupDelay:32767s,Tags:["physwire"]}')
            time.sleep(0.05)
        time.sleep(15.0)
        with bot.lock:
            packets = list(bot.packets)
        v.cmd("kill @e[tag=physwire]")
        time.sleep(0.5)
        by_id: dict[int, list] = {}
        for name, data in packets:
            if name not in ("add_entity", "set_entity_motion", "move_entity_pos", "entity_position_sync", "set_entity_data", "remove_entities"):
                continue
            r = mc.Reader(bytes.fromhex(data))
            if name == "remove_entities":
                continue
            eid = r.varint()
            what: object = None
            if name == "add_entity":
                r.take(16)
                what = r.varint()        # its type: what else lives near the bot is left out below
            if name == "set_entity_motion":
                what = [round(c, 5) for c in mc.read_lp_vec3(r)]
            elif name == "move_entity_pos":
                r.varint()
                what = [r.i16(), r.i16(), r.i16()]
            elif name == "entity_position_sync":
                r.varint()
                what = [r.f64(), r.f64(), r.f64()]
            by_id.setdefault(eid, []).append((name, what))
        for eid, log in sorted(by_id.items()):
            if log[0] != ("add_entity", mc.ENTITY_TYPE["kItem"]):
                continue
            out["items"].append({"id": eid, "packets": log})
            motions = [w for n, w in log if n == "set_entity_motion"]
            print(f"item {eid}: {len(log)} packets in {time.time() - t0:.0f} s; velocities sent: {motions[:12]}{' ...' if len(motions) > 12 else ''}")
            print(f"    others: {[(n, w) for n, w in log if n not in ('set_entity_motion', 'add_entity')][:8]}")
    finally:
        if bot:
            bot.alive = False
        v.stop()
    Path(out_path).write_text(json.dumps(out, indent=1))


def main() -> int:
    a = sys.argv[1:]
    if len(a) == 2 and a[0] == "wire":
        wire(a[1])
        return 0
    if len(a) >= 2 and a[0] == "perf":
        perf(a[1], int(a[2]) if len(a) > 2 else 10000)
        return 0
    if len(a) >= 2 and a[0] == "record":
        only = a[a.index("--only") + 1] if "--only" in a else None
        record(a[1], only)
        return 0
    if len(a) == 3 and a[0] == "emit":
        emit(a[1], a[2])
        return 0
    if a == ["list"]:
        scs = scenarios()
        place(scs)
        for sc in scs:
            print(f"{sc.name:32s} {sc.kind:12s} at {sc.origin} ticks {sc.ticks}")
        print(f"{len(scs)} scenarios")
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
