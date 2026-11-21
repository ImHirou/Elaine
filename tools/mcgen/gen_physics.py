# SPDX-License-Identifier: GPL-3.0-or-later
"""mcgen: what entity physics reads off a block state and off an entity type.

Emitted (included by src/world/collide.cpp and src/phys/):
  physics.hpp        kState (one byte a block state: which kind of collision shape, whether it
                     reaches out of its block, which material), kMaterials (friction, speed, jump
                     and bounce factors, and what the block does to an entity in or on it),
                     kShapes / kShapeCoords / kShapeCells (collision shapes as vanilla holds them:
                     a grid of cells, some of them full), kEntity (how each entity type moves)
  state_physics.bin  kState

A collision shape in vanilla is not a list of boxes but a VoxelShape: per axis a list of
coordinates, and a bit per cell of the grid they cut. The grid is what an entity collides with
(VoxelShape.collideX walks cells), what it may step up to (every y coordinate is a candidate),
and it has more lines than the boxes have faces: Shapes.create makes a box whose corners are
multiples of 1/8 a regular grid of 1, 2, 4 or 8 cells an axis (soul sand is 1 x 8 x 1, seven
cells full). The extractor (blockdata.json) wrote the boxes only, so:

  * a shape of one box is rebuilt exactly - `create()` below is Shapes.create;
  * a shape of several boxes gets the grid its boxes' faces cut, marked inexact. That is the
    same shape for everything outside it; an entity that is INSIDE such a shape may be stopped
    at a line vanilla has and this grid lacks. No block with such a shape can be placed yet,
    and the generator refuses to make one placeable: teach Extract.java to write
    VoxelShape.getCoords and the cells first.
"""
from __future__ import annotations

import re
import subprocess
import zipfile

import mcgen
from mcgen import die, jar_for, version_info

EPS = 1.0e-7

# Block hooks that act on an entity. A placeable block may only override them in a class listed
# here: (hook, declaring class) -> the Material field that stands for it, or None when what the
# hook does needs things the server does not have (health: nothing takes fall or contact damage).
ENTITY_HOOKS = ("entityInside", "stepOn", "fallOn", "bounceOn", "getEntityInsideCollisionShape", "getFriction", "getSpeedFactor",
                "getJumpFactor", "getBounceRestitution", "getFallDistanceReduction")
PORTED_ENTITY_HOOKS = {
    ("entityInside", "WebBlock"): "kInsideWeb",        # Entity.makeStuckInBlock
    ("entityInside", "HoneyBlock"): "kInsideHoney",    # sliding down its side
    ("stepOn", "SlimeBlock"): "kStepSlime",            # slows what walks on it
    ("fallOn", "SlimeBlock"): None,                    # no fall damage
    ("fallOn", "HayBlock"): None,                      # a fifth of the fall damage
    ("fallOn", "HoneyBlock"): None,                    # a fifth of it, a sound and particles (not sent)
    ("stepOn", "MagmaBlock"): None,                    # burns the living
}
# Classes whose getCollisionShape override returns one shape whoever asks (read in the jar).
CONSTANT_COLLISION_SHAPE = {"SoulSandBlock", "MudBlock", "HoneyBlock"}

MATERIAL_FLAGS = [
    ("kFence", 0x1, "#fences: an entity on it stands on it even 0.2 blocks down (Entity.getOnPos)"),
    ("kWallOrGate", 0x2, "#walls or a FenceGateBlock: the same at any depth"),
    ("kSuppressesBounce", 0x4, "#suppresses_bounce: nothing bounces off it (honey)"),
    ("kClimbable", 0x8, "#climbable"),
    ("kFallResetting", 0x10, "#fall_damage_resetting: a fast fall through it starts over"),
    ("kStepSlime", 0x20, "SlimeBlock.stepOn: what stands on it nearly still loses most of its horizontal speed"),
]
INSIDE = ["kInsideNone", "kInsideWeb", "kInsideHoney"]

# How an entity type moves. Everything is read off its class chain (gamedata/entities.json):
MOVERS = ["kMoverNone", "kMoverItem", "kMoverTnt", "kMoverLiving", "kMoverArmorStand", "kMoverFallingBlock"]
# Living classes that declare one of MOVE_METHODS, as of the version the port was written
# against. A class that appears here, disappears, or changes its set stops the generator: read
# it, then decide whether its types still move as plain LivingEntity.travel does.
MOVE_METHODS = ("travel", "getDefaultGravity", "maxUpStep", "move", "omnidirectionalAirMover", "pushEntities", "isPushable", "doPush",
                "push", "getAirDrag", "getEntityBounciness", "getBlockSpeedFactor", "onClimbable", "isEffectiveAi", "isImmobile",
                "makeBoundingBox", "jumpFromGround", "getJumpPower", "checkFallDamage", "getFlyingSpeed", "isAffectedByFluids")
LIVING_DECLARES = {
    "AbstractCubeMob": {"jumpFromGround", "push"}, "AbstractHorse": {"isImmobile", "isPushable", "onClimbable"},
    "Allay": {"checkFallDamage", "travel"}, "ArmorStand": {"doPush", "isEffectiveAi", "isPushable", "pushEntities", "travel"},
    "Bat": {"checkFallDamage", "doPush", "isPushable", "pushEntities"}, "Bee": {"checkFallDamage", "omnidirectionalAirMover"},
    "Camel": {"travel"}, "Creaking": {"isPushable", "push"}, "Fox": {"isImmobile"},
    "Ghast": {"checkFallDamage", "onClimbable", "travel"}, "HappyGhast": {"checkFallDamage", "onClimbable", "travel"},
    "IronGolem": {"doPush"}, "Llama": {"isImmobile"}, "MagmaCube": {"jumpFromGround"}, "Mannequin": {"isEffectiveAi", "isImmobile"},
    "Mob": {"isEffectiveAi"}, "Parrot": {"checkFallDamage", "doPush", "isPushable", "omnidirectionalAirMover"},
    "Phantom": {"checkFallDamage", "onClimbable", "travel"},
    "Player": {"getBlockSpeedFactor", "getFlyingSpeed", "isAffectedByFluids", "isEffectiveAi", "isImmobile", "onClimbable", "travel"},
    "Rabbit": {"getJumpPower", "jumpFromGround"}, "Ravager": {"isImmobile"}, "Shulker": {"makeBoundingBox", "move", "push"},
    "Sniffer": {"jumpFromGround"}, "Spider": {"onClimbable"}, "Squid": {"getDefaultGravity", "travel"}, "Strider": {"checkFallDamage"},
    "SulfurCube": {"doPush", "maxUpStep", "omnidirectionalAirMover"}, "Warden": {"doPush", "isPushable"},
}
# Classes that move in a way of their own - by one of the methods above, or by writing their
# velocity in tick / aiStep / customServerAiStep (AbstractFish and Dolphin flop, Blaze, Chicken
# and the wither fall slowly, a bat hangs, a vex passes through walls, a spider climbs what it
# runs into). Their types are left where they are put, as before there was physics.
OWN_MOVEMENT = {"Allay", "Bat", "Bee", "Camel", "Creaking", "Ghast", "HappyGhast", "Mannequin", "Parrot", "Phantom", "Shulker", "Squid",
                "SulfurCube", "AbstractFish", "Blaze", "Chicken", "Dolphin", "EnderDragon", "Guardian", "Vex", "WitherBoss", "Spider",
                "Player"}
# Entity.getMoveSimulationType(): the classes that declare it, and those of them that answer
# SERVER_AND_CLIENT - what a client moves by itself between the server's updates. (The dragon
# does while it dies; a player answers AUTHORITATIVE_SIDE_AND_SERVER.)
SIMULATION_DECLARES = {"Entity", "EnderDragon", "ExperienceOrb", "FallingBlockEntity", "ItemEntity", "Player", "PrimedTnt", "Projectile"}
CLIENT_SIMULATED = {"ExperienceOrb", "FallingBlockEntity", "ItemEntity", "PrimedTnt", "Projectile"}


def java_round(v: float) -> int:
    import math
    return math.floor(v + 0.5)


def find_bits(lo: float, hi: float) -> int:
    """Shapes.findBits: the number of halvings after which both ends lie on the grid, -1 for none."""
    if lo < -EPS or hi > 1.0000001:
        return -1
    for bits in range(4):
        n = 1 << bits
        a, b = lo * n, hi * n
        if abs(a - java_round(a)) < EPS * n and abs(b - java_round(b)) < EPS * n:
            return bits
    return -1


def create(box: list[float]):
    """Shapes.create(minX..maxZ): 'empty', 'block', or (coords per axis, set of full cells)."""
    lo, hi = box[:3], box[3:]
    if any(hi[a] - lo[a] < EPS for a in range(3)):
        return "empty"
    bits = [find_bits(lo[a], hi[a]) for a in range(3)]
    if min(bits) < 0:       # ArrayVoxelShape over the block's one cell
        return [[lo[a], hi[a]] for a in range(3)], {(0, 0, 0)}
    if bits == [0, 0, 0]:
        return "block"
    size = [1 << b for b in bits]
    # BitSetDiscreteVoxelShape.withFilledBounds, CubePointRange: coordinate i is i / size
    first = [java_round(lo[a] * size[a]) for a in range(3)]
    last = [java_round(hi[a] * size[a]) for a in range(3)]
    cells = {(x, y, z) for x in range(first[0], last[0]) for y in range(first[1], last[1]) for z in range(first[2], last[2])}
    return [[i / size[a] for i in range(size[a] + 1)] for a in range(3)], cells


def box_grid(boxes: list[list[float]]):
    """The grid the faces of several boxes cut (see the module comment: not vanilla's own)."""
    coords = [sorted({b[a] for b in boxes} | {b[a + 3] for b in boxes}) for a in range(3)]
    cells = set()
    for b in boxes:
        r = [range(coords[a].index(b[a]), coords[a].index(b[a + 3])) for a in range(3)]
        cells |= {(x, y, z) for x in r[0] for y in r[1] for z in r[2]}
    return coords, cells


def cdouble(v: float) -> str:
    s = repr(float(v))
    return s if ("." in s or "e" in s or "inf" in s) else s + ".0"


def simulation_declarations(version: str) -> set[str]:
    """The entity classes (short names) that declare getMoveSimulationType (javap -p)."""
    with zipfile.ZipFile(jar_for(version, client=False)) as z:
        names = [n[:-6].replace("/", ".") for n in z.namelist()
                 if n.startswith("net/minecraft/world/entity/") and n.endswith(".class") and b"getMoveSimulationType" in z.read(n)]
    tool = mcgen.find_tool("javap", version_info(version)["java_version"])
    res = subprocess.run([tool, "-p", "-cp", str(jar_for(version, client=False)), *sorted(names)], capture_output=True, text=True)
    if res.returncode != 0:
        die(f"javap failed: {res.stderr.strip()}")
    out, current = set(), None
    for line in res.stdout.splitlines():
        m = re.match(r"^(?:public |protected |private |abstract |final |static )*(?:class|interface|enum) ([\w.$]+)", line)
        if m:
            current = m.group(1).rsplit(".", 1)[1]
        elif current and re.search(r"\bgetMoveSimulationType\(", line):
            out.add(current)
    return out


def living_declarations(version: str, classes: set[str]) -> dict[str, set[str]]:
    """{class: the MOVE_METHODS it declares itself}, for entity classes by short name (javap -p)."""
    with zipfile.ZipFile(jar_for(version, client=False)) as z:
        names = [n[:-6].replace("/", ".") for n in z.namelist() if n.startswith("net/minecraft/world/entity/") and n.endswith(".class")]
    full = {}
    for short in classes:
        found = [n for n in names if n.rsplit(".", 1)[1] == short]
        if len(found) != 1:
            die(f"entity class {short}: {len(found)} classes of that name in the jar")
        full[found[0]] = short
    tool = mcgen.find_tool("javap", version_info(version)["java_version"])
    res = subprocess.run([tool, "-p", "-cp", str(jar_for(version, client=False)), *sorted(full)], capture_output=True, text=True)
    if res.returncode != 0:
        die(f"javap failed: {res.stderr.strip()}")
    out: dict[str, set[str]] = {}
    current = None
    for line in res.stdout.splitlines():
        m = re.match(r"^(?:public |protected |private |abstract |final |static )*(?:class|interface|enum) ([\w.$]+)", line)
        if m:
            current = full.get(m.group(1))
        elif current:
            for method in MOVE_METHODS:
                if re.search(r"\b" + method + r"\(", line):
                    out.setdefault(current, set()).add(method)
    return out


def emit(hdr: str, version: str, ext_blocks: list, st: dict, shapes: list, game: dict, tagged, placeable: set[str],
         entity_ids: dict[str, int], cfloat32) -> dict:
    """`st`: blockdata's per-state columns; `shapes`: its collision shapes (lists of boxes);
    `tagged(name)`: the block ids of a block tag; `placeable`: names of the blocks that can be placed."""
    n = len(st["block"])
    large_col = game["states"]["columns"]["large_collision_shape"]
    full_col = game["states"]["columns"]["collision_full_block"]

    # ---- shapes
    kinds = []          # per shape id: 0 none, 1 cube, 2 box (one cell, not on the 1/8 grid), 3 grid
    exact = []
    records = []        # (coords offset, cells offset, nx, ny, nz)
    all_coords: list[float] = []
    all_cells: list[int] = []
    for sid, boxes in enumerate(shapes):
        made = "empty" if not boxes else create(boxes[0]) if len(boxes) == 1 else box_grid(boxes)
        exact.append(len(boxes) <= 1)
        if made == "empty":
            if boxes:
                die(f"collision shape {sid}: a box thinner than 1e-7")
            kinds.append(0)
            records.append((0, 0, 0, 0, 0))
            continue
        if made == "block":
            kinds.append(1)
            made = ([[0.0, 1.0]] * 3, {(0, 0, 0)})
        else:
            kinds.append(2 if len(boxes) == 1 and len(made[1]) == 1 and all(len(c) == 2 for c in made[0]) else 3)
        coords, cells = made
        size = [len(c) - 1 for c in coords]
        if max(size) > 255 or len(all_coords) > 0xFFFF or len(all_cells) > 0xFFFF:
            die(f"collision shape {sid}: does not fit the Shape record")
        bits = 0
        for x, y, z in cells:
            bits |= 1 << ((x * size[1] + y) * size[2] + z)      # BitSetDiscreteVoxelShape.getIndex
        words = [(bits >> (64 * i)) & 0xFFFFFFFFFFFFFFFF for i in range((size[0] * size[1] * size[2] + 63) // 64)]
        records.append((len(all_coords), len(all_cells), *size))
        all_coords += coords[0] + coords[1] + coords[2]
        all_cells += words
    if shapes[0] != [] or kinds[1] != 1:
        die("collision shapes 0 and 1 are not the empty one and the full block")

    # ---- materials
    block_names = [b["name"] for b in ext_blocks]
    fences, walls, no_bounce = set(tagged("fences")), set(tagged("walls")), set(tagged("suppresses_bounce"))
    climbable, fall_reset = set(tagged("climbable")), set(tagged("fall_damage_resetting"))
    flag = {name: value for name, value, _ in MATERIAL_FLAGS}
    materials: list[tuple] = []
    material_of: list[int] = []
    for bid, name in enumerate(block_names):
        gb = game["blocks"][name]
        s = gb["settings"]
        f = 0
        f |= flag["kFence"] if bid in fences else 0
        f |= flag["kWallOrGate"] if bid in walls or "FenceGateBlock" in gb["class"] else 0
        f |= flag["kSuppressesBounce"] if bid in no_bounce else 0
        f |= flag["kClimbable"] if bid in climbable else 0
        f |= flag["kFallResetting"] if bid in fall_reset else 0
        inside = "kInsideNone"
        if name in placeable:
            for hook in ENTITY_HOOKS:
                if hook not in gb["hooks"]:
                    continue
                key = (hook, gb["hooks"][hook])
                if key not in PORTED_ENTITY_HOOKS:
                    die(f"{name}: a placeable block whose {key[1]}.{hook} is not ported (gen_physics.PORTED_ENTITY_HOOKS)")
                what = PORTED_ENTITY_HOOKS[key]
                if what in INSIDE:
                    inside = what
                elif what:
                    f |= flag[what]
            if "getCollisionShape" in gb["hooks"] and gb["hooks"]["getCollisionShape"] not in CONSTANT_COLLISION_SHAPE:
                die(f"{name}: a placeable block whose collision shape {gb['hooks']['getCollisionShape']} computes")
            if s["dynamicShape"]:
                die(f"{name}: a placeable block with a dynamic shape")
        m = (s["friction"], s["speedFactor"], s["jumpFactor"], s["bounceRestitution"], f, inside)
        if m not in materials:
            materials.append(m)
        material_of.append(materials.index(m))
    default = (0.6, 1.0, 1.0, 0.0, 0, "kInsideNone")
    if default not in materials:
        die("no block has the default material")
    # Air's material first: the entity code asks for the material of the block under an entity
    # in the air more often than for any other.
    order = sorted(range(len(materials)), key=lambda i: (materials[i] != default, i))
    rank = {old: new for new, old in enumerate(order)}
    if len(materials) > 16:
        die(f"{len(materials)} block materials: kState has four bits for them")

    # ---- kState
    state = bytearray(n)
    for i in range(n):
        sid = st["collision_shape"][i]
        kind = 0 if sid < 0 else kinds[sid]
        bid = st["block"][i]
        name = block_names[bid]
        is_large = sid >= 0 and any(c < 0.0 or c > 1.0 for b in shapes[sid] for c in b)
        if sid >= 0 and bool(large_col[i]) != is_large:
            die(f"state {i} ({name}): hasLargeCollisionShape disagrees between gamedata and the boxes")
        # Entity.moveTowardsClosestSpace asks isCollisionShapeFullBlock: that is kShapeCube.
        if sid >= 0 and bool(full_col[i]) != (kind == 1):
            die(f"state {i} ({name}): isCollisionShapeFullBlock is not 'its collision shape is the full block'")
        if name in placeable:
            if sid < 0:
                die(f"{name}: a placeable block whose collision shape depends on who asks")
            if not exact[sid]:
                die(f"{name}: a placeable block whose collision shape has several boxes - the extractor has to write "
                    "its VoxelShape grid first (see gen_physics.py)")
        state[i] = kind | (4 if is_large else 0) | rank[material_of[bid]] << 4

    o = [hdr, """
#include "entities.hpp"

// What entity physics reads off a block state and an entity type (tools/mcgen/gen_physics.py).
namespace proto::gen::physics {

// ---- kState[state]: one byte, the first thing a collision query loads for a block.
inline constexpr unsigned char kShapeMask = 0x3;   // the kind of its collision shape:
inline constexpr unsigned char kShapeNone = 0;     //   none (or one that depends on who asks: scaffolding, powder snow)
inline constexpr unsigned char kShapeCube = 1;     //   the full block (Shapes.block())
inline constexpr unsigned char kShapeBox = 2;      //   one box off the 1/8 grid: a shape of one cell (a chest, honey)
inline constexpr unsigned char kShapeGrid = 3;     //   cells: kShapes[state::collision_of(state)]
inline constexpr unsigned char kLarge = 0x4;       // BlockState.hasLargeCollisionShape(): reaches out of its block
inline constexpr unsigned kMaterialShift = 4;      // kMaterials[kState[state] >> 4]
inline constexpr unsigned char kState[] = {
#embed "state_physics.bin"
};

// ---- what a block is to an entity in or on it
struct Material {
    float friction;         // Block.getFriction()
    float speed_factor;     // Block.getSpeedFactor()
    float jump_factor;      // Block.getJumpFactor()
    float bounce;           // Block.getBounceRestitution()
    unsigned char flags;
    unsigned char inside;   // kInside*: BlockBehaviour.entityInside
};
"""]
    for name, value, why in MATERIAL_FLAGS:
        o.append(f"inline constexpr unsigned char {name} = 0x{value:X};  // {why}\n")
    for i, name in enumerate(INSIDE):
        o.append(f"inline constexpr unsigned char {name} = {i};\n")
    o.append(f"inline constexpr Material kMaterials[{len(materials)}] = {{\n")
    for old in order:
        fr, sp, ju, bo, f, inside = materials[old]
        users = [block_names[b].split(":", 1)[1] for b in range(len(block_names)) if material_of[b] == old]
        note = ", ".join(users[:4]) + (f" and {len(users) - 4} more" if len(users) > 4 else "")
        o.append(f"    {{{cfloat32(fr)}, {cfloat32(sp)}, {cfloat32(ju)}, {cfloat32(bo)}, 0x{f:X}, {inside}}},  // {note}\n")
    o.append("};\n")

    o.append("""
// ---- collision shapes as vanilla holds them (VoxelShape): per axis a list of coordinates,
// and a bit per cell of the grid they cut. Indexed like the box lists (state::collision_of).
struct Shape {
    unsigned short coords;   // kShapeCoords: nx + 1 x coordinates, then ny + 1 of y, then nz + 1 of z
    unsigned short cells;    // kShapeCells: bit (x * ny + y) * nz + z is set for a full cell
    unsigned char nx, ny, nz;
    bool exact;              // false: a shape of several boxes, whose grid here is the one its boxes' faces cut
};
""")
    o.append(f"inline constexpr Shape kShapes[{len(shapes)}] = {{\n")
    for sid, (co, ce, nx, ny, nz) in enumerate(records):
        o.append(f"    {{{co}, {ce}, {nx}, {ny}, {nz}, {'true' if exact[sid] else 'false'}}},\n")
    o.append(f"}};\ninline constexpr double kShapeCoords[{max(len(all_coords), 1)}] = {{\n")
    for i in range(0, len(all_coords), 8):
        o.append("    " + ", ".join(cdouble(c) for c in all_coords[i:i + 8]) + ",\n")
    o.append(f"}};\ninline constexpr unsigned long long kShapeCells[{max(len(all_cells), 1)}] = {{\n")
    for i in range(0, len(all_cells), 4):
        o.append("    " + ", ".join(f"0x{w:X}ull" for w in all_cells[i:i + 4]) + ",\n")
    o.append("};\n")

    # ---- entity types
    types = {t["name"]: t for t in game["entities"]["types"]}
    living_classes = {c for t in types.values() if "LivingEntity" in t["class"] for c in t["class"]} - {"Entity", "LivingEntity"}
    declared = living_declarations(version, living_classes)
    if declared != LIVING_DECLARES:
        changed = sorted(c for c in set(declared) | set(LIVING_DECLARES) if declared.get(c) != LIVING_DECLARES.get(c))
        die(f"living entity classes declare other movement methods than the physics port was written against: "
            f"{[(c, sorted(declared.get(c, ()))) for c in changed]} - read them, then update gen_physics.LIVING_DECLARES / OWN_MOVEMENT")
    missing = OWN_MOVEMENT - living_classes
    if missing:
        die(f"gen_physics.OWN_MOVEMENT names classes no living entity type has any more: {sorted(missing)}")
    simulating = simulation_declarations(version)
    if simulating != SIMULATION_DECLARES:
        die(f"other entity classes declare getMoveSimulationType than gen_physics.SIMULATION_DECLARES says: "
            f"{sorted(simulating ^ SIMULATION_DECLARES)} - read them, then update it and CLIENT_SIMULATED")
    rows = {}
    for name, t in types.items():
        chain = t["class"]
        attrs = t.get("attributes", {})
        mover, gravity, step = "kMoverNone", 0.0, 0.0
        if chain[0] == "ItemEntity":
            mover, gravity = "kMoverItem", 0.04            # ItemEntity.getDefaultGravity
        elif chain[0] == "PrimedTnt":
            mover, gravity = "kMoverTnt", 0.04             # PrimedTnt.getDefaultGravity
        elif chain[0] == "FallingBlockEntity":
            mover, gravity = "kMoverFallingBlock", 0.04    # FallingBlockEntity.getDefaultGravity
        elif "LivingEntity" in chain:
            if "ArmorStand" in chain:
                mover = "kMoverArmorStand"
            elif not set(chain) & OWN_MOVEMENT:
                mover = "kMoverLiving"
            if mover != "kMoverNone":
                for key in ("minecraft:gravity", "minecraft:step_height"):
                    if key not in attrs:
                        die(f"{name}: no default {key}")
                gravity, step = attrs["minecraft:gravity"], attrs["minecraft:step_height"]
                # What the port takes for granted about a living entity: the attributes that
                # scale friction, drag and bounce are at their defaults.
                expect = {"minecraft:friction_modifier": 1.0, "minecraft:air_drag_modifier": 1.0, "minecraft:bounciness": 0.0,
                          "minecraft:movement_efficiency": 0.0, "minecraft:explosion_knockback_resistance": 0.0, "minecraft:scale": 1.0}
                for key, value in expect.items():
                    if attrs.get(key) != value:
                        die(f"{name}: default {key} is {attrs.get(key)}, the physics port assumes {value}")
        rows[entity_ids[name.split(":", 1)[1]]] = (name, mover, gravity, step, bool(set(chain) & CLIENT_SIMULATED))
    if sorted(rows) != list(range(len(entity_ids))):
        die("gamedata/entities.json does not list every entity type")
    o.append("""
// ---- how an entity type moves
""")
    for i, name in enumerate(MOVERS):
        o.append(f"inline constexpr unsigned char {name} = {i};\n")
    o.append("""struct EntityPhysics {
    unsigned char mover;   // kMover*: whose tick moves it. kMoverNone: nothing here does (it stays where it is put)
    bool simulated;        // Entity.getMoveSimulationType() is SERVER_AND_CLIENT: a client moves it by itself between updates
    float step_height;     // LivingEntity.maxUpStep(): the step_height attribute's default
    double gravity;        // Entity.getDefaultGravity(): a constant of its class, or the gravity attribute's default
};
""")
    o.append(f"inline constexpr EntityPhysics kEntity[entity_type::kCount] = {{\n")
    for i in range(len(rows)):
        name, mover, gravity, step, simulated = rows[i]
        o.append(f"    {{{mover}, {'true' if simulated else 'false'}, {cfloat32(step)}, {cdouble(gravity)}}},  // {name.split(':', 1)[1]}\n")
    o.append("};\n\n}  // namespace proto::gen::physics\n")
    stats = {"materials": len(materials), "shapes_grid": sum(k == 3 for k in kinds), "shapes_inexact": sum(not e for e in exact),
             "movers": sum(r[1] != "kMoverNone" for r in rows.values())}
    return {"physics.hpp": "".join(o), "state_physics.bin": bytes(state), "_physics_stats": stats}
