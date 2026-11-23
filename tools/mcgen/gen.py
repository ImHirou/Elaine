# SPDX-License-Identifier: GPL-3.0-or-later
"""mcgen code generation: .cache/mc/<version>/ -> src/proto/gen/v<protocol>/.

Emitted files (all deterministic; none is in git - their lock is, see "the lock" below):
  meta.hpp            version constants
  packet_ids.hpp      packet ids per state/direction, name tables
  registries.hpp      network ids of every synchronized-registry entry (our order = sorted)
  registry_data.bin   pre-serialized configuration `registry_data` packet bodies (known-pack form)
  blobs.hpp           #embed wrappers for the packet .bin files
  tags.bin            pre-serialized `update_tags` packet body
  blocks.hpp          default block state id per block, by name
  items.hpp           item id per item, by name; data component type ids
  dimension_types.hpp height / skylight per dimension type, in network id order
  chat_types.hpp      the translation and its arguments for each chat type, in network id order
  timelines.hpp       world clocks, timelines and their time markers; the overworld's sky light by time
  entities.hpp        entity type ids + tracking parameters, entity-data serializer / accessor ids, poses
  block_data.hpp      HOT per-state tables (#embed of state_*.bin) + the light occlusion matrix
  state_*.bin         one table per file, indexed by block state id
  block_info.hpp      layout + flag bits of the cold tables
  block_tables.hpp    COLD per-block / per-item / collision-shape tables (include from one .cpp)
  recipes.hpp         crafting and cooking recipes (gen_game.py; include from one .cpp)
  update_recipes.bin  pre-serialized play `update_recipes` packet body (recipe property sets)
  static_ids.hpp      menu and block entity type ids, the sounds and particles the server sends
  command_data.hpp    what commands look up by name: argument type ids, block properties, item
                      names, which entities can be summoned (gen_commands.py; one .cpp includes it)
  lang.hpp            en_us.json's messages, for the console (gen_commands.py; one .cpp includes it)
  physics.hpp         what entity physics reads off a block state (collision shape kind, material)
                      and an entity type; collision shapes as grids of cells (gen_physics.py)
  state_physics.bin   its per-state byte

Inputs beyond the jar itself: `mcgen.py reports` (ids, tags, item components), `mcgen.py blockdata`
(per-state light, fluid and shape properties) and `mcgen.py gamedata` (block classes and their
hooks, loot table names, crafting remainders). All are required: a server without block ids cannot
hold a world, and vanilla clients cannot join without tags.

"Body" means packet id + payload, i.e. everything after the frame length; the
server only has to frame (and maybe compress) these once at startup.
"""
from __future__ import annotations

import hashlib
import json
import re
import sys
import zipfile
from pathlib import Path

import gamedata
import gen_commands
import gen_game
import gen_physics
import mcgen
from mcgen import ROOT, die, javap, jar_for, vdir, version_info

# ----------------------------------------------------------------------------- wire helpers


def nbt_of_json(v) -> bytes:
    """A JSON value as a nameless NBT tag (type, payload), the way JsonOps' values convert: a
    whole number an int (a long if it has to be), another a double, true and false bytes, a list
    of values that differ as ListTag.write wraps them."""
    import struct

    def mutf8(s: str) -> bytes:
        out = bytearray()
        for unit in struct.unpack(f"<{len(s.encode('utf-16-le')) // 2}H", s.encode("utf-16-le")):
            if 0 < unit < 0x80:
                out.append(unit)
            elif unit < 0x800:
                out += bytes((0xC0 | unit >> 6, 0x80 | unit & 0x3F))
            else:
                out += bytes((0xE0 | unit >> 12, 0x80 | unit >> 6 & 0x3F, 0x80 | unit & 0x3F))
        return struct.pack(">H", len(out)) + bytes(out)

    if isinstance(v, bool):
        return bytes((1, int(v)))
    if isinstance(v, int):
        return b"\x03" + struct.pack(">i", v) if -2**31 <= v < 2**31 else b"\x04" + struct.pack(">q", v)
    if isinstance(v, float):
        return b"\x06" + struct.pack(">d", v)
    if isinstance(v, str):
        return b"\x08" + mutf8(v)
    if isinstance(v, list):
        tags = [nbt_of_json(x) for x in v]
        kinds = {t[0] for t in tags}
        if len(kinds) <= 1:
            return bytes((9, kinds.pop() if kinds else 0)) + struct.pack(">i", len(tags)) + b"".join(t[1:] for t in tags)
        wrapped = [t[1:] if t[0] == 10 else t[:1] + b"\x00\x00" + t[1:] + b"\x00" for t in tags]
        return bytes((9, 10)) + struct.pack(">i", len(tags)) + b"".join(wrapped)
    if isinstance(v, dict):
        return b"\x0a" + b"".join(t[:1] + mutf8(k) + t[1:] for k, x in v.items() for t in [nbt_of_json(x)]) + b"\x00"
    die(f"a JSON value that is no tag: {v!r}")


def varint(n: int) -> bytes:
    n &= 0xFFFFFFFF
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def mc_str(s: str) -> bytes:
    raw = s.encode("utf-8")
    return varint(len(raw)) + raw


def ident(name: str) -> str:
    """'minecraft:debug/block_value' -> 'kDebugBlockValue'."""
    name = name.split(":", 1)[-1]
    parts = [p for p in re.split(r"[^0-9a-zA-Z]+", name) if p]
    return "k" + "".join(p[:1].upper() + p[1:] for p in parts)


def ns_ident(path: str) -> str:
    """'worldgen/biome' -> 'worldgen_biome' (namespace name)."""
    return re.sub(r"[^0-9a-z]+", "_", path.lower()).strip("_")


HEADER = "// GENERATED by tools/mcgen (Minecraft {id}, protocol {proto}). Do not edit; run `mcgen.py generate`.\n#pragma once\n"


# ----------------------------------------------------------------------------- inputs

def load_packets(version: str) -> tuple[dict, str]:
    d = vdir(version)
    official = d / "reports" / "packets.json"
    static = d / "static" / "packets.json"
    if official.exists():
        pk = json.loads(official.read_text())
        if static.exists() and json.loads(static.read_text()) != pk:
            die("static packets.json disagrees with Mojang's report - the static extractor needs fixing")
        return pk, "reports"
    if not static.exists():
        die("no packets.json - run: mcgen.py packets   (or: mcgen.py reports)")
    return json.loads(static.read_text()), "static"


def synced_registries(version: str) -> list[str]:
    """Registry paths ('worldgen/biome', ...) in RegistryDataLoader.SYNCHRONIZED_REGISTRIES order."""
    text = javap(version, ["net.minecraft.resources.RegistryDataLoader"])
    groups: dict[str, list[str]] = {}
    cur: list[str] = []
    in_clinit = False
    for line in text.splitlines():
        if line.startswith("  static {}"):
            in_clinit = True
            continue
        if not in_clinit:
            continue
        m = re.search(r"getstatic .*Field net/minecraft/core/registries/Registries\.(\w+):", line)
        if m:
            cur.append(m.group(1))
            continue
        m = re.search(r"putstatic .*Field (\w+):", line)
        if m:
            groups[m.group(1)] = cur
            cur = []
    fields = groups.get("SYNCHRONIZED_REGISTRIES")
    if not fields:
        die("could not locate RegistryDataLoader.SYNCHRONIZED_REGISTRIES")

    paths: dict[str, str] = {}
    pending = None
    for line in javap(version, ["net.minecraft.core.registries.Registries"]).splitlines():
        m = re.search(r"\d+: ldc(?:_w)?\s+#\d+\s+// String (\S+)", line)
        if m:
            pending = m.group(1)
            continue
        m = re.search(r"putstatic .*Field (\w+):Lnet/minecraft/resources/ResourceKey;", line)
        if m and pending:
            paths[m.group(1)] = pending
            pending = None
    missing = [f for f in fields if f not in paths]
    if missing:
        die(f"unresolved registry keys: {missing}")
    return [paths[f] for f in fields]


class VanillaData:
    """Read-only view of the vanilla datapack inside the server jar."""

    def __init__(self, version: str):
        self.zip = zipfile.ZipFile(jar_for(version, client=False))
        self.names = [n for n in self.zip.namelist() if n.startswith("data/minecraft/") and n.endswith(".json")]

    def entries(self, registry_path: str) -> list[str]:
        prefix = f"data/minecraft/{registry_path}/"
        return sorted(f"minecraft:{n[len(prefix):-5]}" for n in self.names if n.startswith(prefix))

    def tag_dirs(self) -> list[str]:
        """Registry paths that have tags, longest-first so 'worldgen/biome' wins over 'worldgen'."""
        return sorted({n[len("data/minecraft/tags/"):].rsplit("/", 1)[0] for n in self.names
                       if n.startswith("data/minecraft/tags/")}, key=lambda p: (-len(p), p))

    def tags(self, registry_path: str) -> dict[str, list]:
        prefix = f"data/minecraft/tags/{registry_path}/"
        return {f"minecraft:{n[len(prefix):-5]}": json.loads(self.zip.read(n))["values"]
                for n in self.names if n.startswith(prefix)}


def with_ns(name: str) -> str:
    return name if ":" in name else f"minecraft:{name}"


def flatten_tags(raw: dict[str, list], ids: dict[str, int], where: str) -> dict[str, list[int]]:
    """Resolve '#tag' references and names -> protocol ids (order-preserving, de-duplicated)."""
    done: dict[str, list[int]] = {}
    visiting: set[str] = set()

    def resolve(tag: str) -> list[int]:
        if tag in done:
            return done[tag]
        if tag in visiting:
            die(f"{where}: tag cycle through {tag}")
        visiting.add(tag)
        out: dict[int, None] = {}
        for v in raw[tag]:
            required = True
            if isinstance(v, dict):
                required = v.get("required", True)
                v = v["id"]
            if v.startswith("#"):
                ref = with_ns(v[1:])
                if ref not in raw:
                    if required:
                        die(f"{where}: {tag} references missing tag {ref}")
                    continue
                for i in resolve(ref):
                    out[i] = None
            else:
                name = with_ns(v)
                if name not in ids:
                    if required:
                        die(f"{where}: {tag} references unknown entry {name}")
                    continue
                out[ids[name]] = None
        visiting.discard(tag)
        done[tag] = list(out)
        return done[tag]

    for t in sorted(raw):
        resolve(t)
    return done


# ----------------------------------------------------------------------------- emitters

def emit_meta(v: dict) -> str:
    return HEADER.format(id=v["id"], proto=v["protocol_version"]) + f"""
#include <string_view>

namespace proto::gen {{

inline constexpr int kProtocolVersion = {v['protocol_version']};
inline constexpr int kWorldVersion = {v['world_version']};
inline constexpr std::string_view kVersionName = "{v['id']}";

}}  // namespace proto::gen
"""


def emit_packet_ids(v: dict, packets: dict) -> str:
    out = [HEADER.format(id=v["id"], proto=v["protocol_version"]), "\n#include <string_view>\n\n"
           "// ids::<state>::cb = clientbound (we send), ids::<state>::sb = serverbound (we receive).\n"
           "namespace proto::gen::ids {\n"]
    for state in ("handshake", "status", "login", "configuration", "play"):
        out.append(f"\nnamespace {state} {{\n")
        for direction, short in (("clientbound", "cb"), ("serverbound", "sb")):
            table = packets.get(state, {}).get(direction)
            if not table:
                continue
            ordered = sorted(table.items(), key=lambda kv: kv[1]["protocol_id"])
            if [e["protocol_id"] for _, e in ordered] != list(range(len(ordered))):
                die(f"{state}/{direction}: packet ids are not dense")
            out.append(f"namespace {short} {{\n")
            for name, e in ordered:
                out.append(f"inline constexpr unsigned {ident(name)} = 0x{e['protocol_id']:02X};\n")
            out.append(f"inline constexpr unsigned kCount = {len(ordered)};\n")
            out.append("inline constexpr std::string_view kNames[kCount] = {\n")
            out.extend(f'    "{name.split(":", 1)[1]}",\n' for name, _ in ordered)
            out.append(f"}};\n}}  // namespace {short}\n")
        out.append(f"}}  // namespace {state}\n")
    out.append("\n}  // namespace proto::gen::ids\n")
    return "".join(out)


def emit_registries(v: dict, regs: list[tuple[str, list[str]]]) -> str:
    out = [HEADER.format(id=v["id"], proto=v["protocol_version"]), "\n#include <string_view>\n\n"
           "// Network ids of synchronized (data-driven) registries. The server chooses the order by the\n"
           "// order of entries in registry_data; ours is sorted by name and baked into registry_data.bin.\n"
           "namespace proto::gen::reg {\n"]
    for path, entries in regs:
        ns = ns_ident(path)
        out.append(f'\nnamespace {ns} {{  // minecraft:{path}\n')
        out.append(f'inline constexpr std::string_view kKey = "minecraft:{path}";\n')
        seen: dict[str, str] = {}
        for i, e in enumerate(entries):
            name = ident(e)
            if name in seen or name in ("kKey", "kCount", "kNames"):
                die(f"registry {path}: identifier clash {e} vs {seen.get(name)}")
            seen[name] = e
            out.append(f"inline constexpr unsigned {name} = {i};\n")
        out.append(f"inline constexpr unsigned kCount = {len(entries)};\n")
        if entries:
            out.append("inline constexpr std::string_view kNames[kCount] = {\n")
            out.extend(f'    "{e}",\n' for e in entries)
            out.append("};\n")
        out.append(f"}}  // namespace {ns}\n")
    out.append("\n}  // namespace proto::gen::reg\n")
    return "".join(out)


def registry_data_blob(packets: dict, regs: list[tuple[str, list[str]]]) -> bytes:
    """[varint count] then per packet [varint body_len][body]; body = id + payload."""
    pid = packets["configuration"]["clientbound"]["minecraft:registry_data"]["protocol_id"]
    blob = bytearray(varint(len(regs)))
    for path, entries in regs:
        body = bytearray(varint(pid))
        body += mc_str(f"minecraft:{path}")
        body += varint(len(entries))
        for e in entries:
            body += mc_str(e)
            body += b"\x00"  # no inline NBT: the client takes the entry from known pack minecraft:core
        blob += varint(len(body)) + body
    return bytes(blob)


def tags_blob(packets: dict, tag_tables: list[tuple[str, dict[str, list[int]]]]) -> bytes:
    """One update_tags packet body (id + payload). Empty when there is nothing to send."""
    if not tag_tables:
        return b""
    pid = packets["configuration"]["clientbound"]["minecraft:update_tags"]["protocol_id"]
    body = bytearray(varint(pid))
    body += varint(len(tag_tables))
    for key, tags in tag_tables:
        body += mc_str(key)
        body += varint(len(tags))
        for name in sorted(tags):
            body += mc_str(name)
            body += varint(len(tags[name]))
            for i in tags[name]:
                body += varint(i)
    return bytes(body)


def emit_blobs(v: dict) -> str:
    return HEADER.format(id=v["id"], proto=v["protocol_version"]) + """
#include "meta.hpp"

// Pre-serialized packet bodies, embedded into the binary. Include from exactly one .cpp.
namespace proto::gen {

// [varint n] then n x ([varint body_len][body]); body = packet id + payload of one registry_data.
inline constexpr unsigned char kRegistryDataBlob[] = {
#embed "registry_data.bin"
};

// One update_tags packet body (packet id + payload).
inline constexpr unsigned char kTagsBlob[] = {
#embed "tags.bin"
};

// One play update_recipes packet body (packet id + payload): the recipe property sets.
inline constexpr unsigned char kUpdateRecipesBlob[] = {
#embed "update_recipes.bin"
};

// The default components of every item, as tags: per item [varint n], n x ([varint component]
// [varint length][a nameless tag]).
inline constexpr unsigned char kItemDefaultsBlob[] = {
#embed "item_defaults.bin"
};

}  // namespace proto::gen
"""


def emit_blocks(v: dict, blocks: dict) -> str:
    out = [HEADER.format(id=v["id"], proto=v["protocol_version"]),
           "\n// Default block state id per block (from Mojang's blocks.json).\n"
           "namespace proto::gen::block {\n\n"]
    rows = []
    total = 0
    for name, b in blocks.items():
        default = next((s["id"] for s in b["states"] if s.get("default")), b["states"][0]["id"])
        rows.append((default, name))
        total += len(b["states"])
    for default, name in sorted(rows):
        out.append(f"inline constexpr unsigned {ident(name)} = {default};\n")
    out.append(f"\ninline constexpr unsigned kStateCount = {total};\n")
    out.append("\n}  // namespace proto::gen::block\n")
    return "".join(out)


# ----------------------------------------------------------------------------- block / item tables

# proto::gen::state::kFlags bits. Everything a per-block scan (section counters, heightmaps) asks.
STATE_FLAGS = [
    ("kIsAir", "BlockState.isAir(): excluded from a section's non-empty count"),
    ("kHasFluid", "getFluidState() is not empty (water, lava, waterlogged states): the section's fluid count"),
    ("kMotionBlocking", "Heightmap MOTION_BLOCKING: #blocks_motion_in_heightmap or has a fluid"),
    ("kMotionBlockingNoLeaves", "Heightmap MOTION_BLOCKING_NO_LEAVES: #blocks_motion_in_heightmap_no_leaves or has a fluid"),
    ("kReplaceable", "BlockState.canBeReplaced(): placing a block here overwrites it"),
    ("kRandomTicks", "isRandomlyTicking()"),
    ("kBlockEntity", "hasBlockEntity()"),
    ("kRedstoneConductor", "isRedstoneConductor(): a chest with this on top does not open"),
]

# proto::gen::BlockInfo::flags. kPlace* is how BlockItem.place() picks the state; a block is given a
# placement kind only when every hook vanilla runs for it is one the server implements (see
# placement_kind below) - everything else stays kPlaceNone until its behaviour is written.
BLOCK_FLAGS = [
    ("kPlaceMask", 0x3F, "placement kind, one of:"),
    ("kPlaceNone", 0, "not placeable yet: the block or its item has behaviour that is not implemented"),
    ("kPlaceDefault", 1, "Block.getStateForPlacement(): the default state"),
    ("kPlaceAxis", 2, "RotatedPillarBlock: axis = clicked face's axis; alt = states for x, y, z"),
    ("kPlaceSnowy", 3, "SnowyBlock: snowy = block above is in #snow; alt = states for false, true"),
    ("kPlaceChest", 4, "ChestBlock: faces the player, joins a single chest beside it (props::chest); alt = the sounds of "
                       "its lid, open and close, and for a waxed copper chest the first state of the block it is without the wax"),
    ("kPlaceFacing", 5, "AbstractFurnaceBlock: `facing` = opposite of the player's horizontal direction (props::furnace)"),
    ("kPlaceTorch", 6, "StandingAndWallBlockItem: standing, or on the first wall the player looks at that holds it; "
                       "alt = the standing block's state, the wall block's first state (facing north, south, west, east)"),
    ("kPlaceCluster", 7, "AmethystClusterBlock: `facing` = the clicked face, waterlogged in water (props::amethyst_cluster)"),
    ("kPlaceLamp", 8, "RedstoneLampBlock: lit while a signal arrives (props::redstone_lamp)"),
    ("kPlaceConcretePowder", 9, "ConcretePowderBlock: its concrete (alt[0]) when it is placed in or against water"),
    ("kPlaceSlab", 10, "SlabBlock: the half that was clicked, or the other half of the slab that is there (props::slab)"),
    ("kPlaceStairs", 11, "StairBlock: away from the player, the clicked half, the corner its neighbours make (props::stairs)"),
    ("kPlaceCross", 12, "FenceBlock / IronBarsBlock: an arm to each side that takes one (props::cross)"),
    ("kPlaceWall", 13, "WallBlock: sides low or tall by what stands on it, a post where it needs one (props::wall)"),
    ("kPlaceHorizontal", 14, "GlazedTerracottaBlock: `facing` = opposite of the player's horizontal direction (props::horizontal)"),
    ("kPlaceChain", 15, "ChainBlock: axis = clicked face's axis, waterlogged in water (props::chain)"),
    ("kPlaceLantern", 16, "LanternBlock: hanging or standing by where the player looks and what holds it (props::lantern)"),
    ("kPlaceEndRod", 17, "EndRodBlock: `facing` = the clicked face, turned round on a rod that points the same way (props::end_rod)"),
    ("kPlaceWaterlogged", 18, "the default state, waterlogged in water (props::waterlogged)"),
    ("kPlaceMushroom", 19, "HugeMushroomBlock: a side is skin unless more of the same block is there (props::mushroom)"),
    ("kPlaceBarrel", 20, "BarrelBlock: `facing` = opposite of the direction the player looks along most (props::barrel)"),
    ("kPlaceSegment", 21, "SegmentableBlock (petals, leaf litter): one more onto the block of it that is there, else one "
                          "that faces the player (props::flower_bed)"),
    ("kPlaceSign", 22, "a sign's StandingAndWallBlockItem: standing, turned to the player, or on the first wall the player "
                       "looks at that holds it; alt = the standing block's default state, the wall block's first state"),
    ("kPlaceHangingSign", 23, "HangingSignItem: under what holds it when the player looks up, else between two walls; "
                              "alt = the ceiling block's default state, the wall block's first state"),
    ("kSnowTag", 0x40, "member of #minecraft:snow (makes the SnowyBlock below it snowy)"),
    ("kInteractive", 0x80, "overrides useItemOn/useWithoutItem: a click may be consumed by the block"),
    ("kGameMaster", 0x100, "GameMasterBlock: only operators may break it"),
    ("kCustomReplace", 0x200, "overrides canBeReplaced(state, context): kReplaceable alone does not decide"),
    ("kCustomClone", 0x400, "overrides getCloneItemStack: \"pick block\" gives something other than a plain stack of its item"),
    ("kSettable", 0x800, "its class is ported and no item places it (a fluid's block): /setblock and /fill may set it"),
]

# proto::gen::state::kHooks: which of the hooks a block change runs the state's block has - of a
# ported class; the others have none here, and a block of theirs that an imported world holds
# does nothing. One byte a state: what Level.setBlock asks of each of the twelve neighbours.
HOOK_FLAGS = [
    ("kHookShape", "updateShape", "BlockBehaviour.updateShape is its class's own"),
    ("kHookNeighbor", "neighborChanged", "...neighborChanged"),
    ("kHookPlace", "onPlace", "...onPlace"),
    ("kHookTick", "tick", "...tick: what a scheduled tick of it runs"),
    ("kFallThrough", None, "FallingBlock.isFree(): air, fire, a liquid, or replaceable - what stands on it falls"),
]
# proto::gen::state::kRandom: what a random tick does with the state, and what the state is to
# the random ticks of the blocks under it.
RANDOM_FLAGS = [
    ("kRandomTick", "isRandomlyTicking(), and the block's randomTick is ported"),
    ("kRandomAlways", "...and may act whatever stands around the block (copper weathers, amethyst buds)"),
    ("kSpreader", "SpreadingSnowyBlock (grass, mycelium): dies under what smothers it, spreads to soil"),
    ("kSoil", "the block a spreader spreads to and dies into (dirt)"),
    ("kSmothers", "as the block above a spreader: SpreadingSnowyBlock.canStayAlive is false"),
    ("kNoSpread", "as the block above soil: nothing spreads there (canPropagate: it smothers, or is water)"),
    ("kNylium", "NyliumBlock: turns into netherrack under what smothers it"),
    ("kSmothersNylium", "as the block above nylium: NyliumBlock.canBeNylium is false"),
]

# proto::gen::state::kConnect: what a state is to a fence, a pane or a wall beside it
# (FenceBlock.connectsTo, IronBarsBlock.attachsTo, WallBlock.connectsTo) - beyond its sturdy faces -
# and to what hangs under it.
CONNECT_FLAGS = [
    ("kConnectException", "Block.isExceptionForConnection: leaves, a barrier, a pumpkin, a melon, a shulker box - no arm for a whole face"),
    ("kConnectFence", "#fences"),
    ("kConnectWoodenFence", "#wooden_fences: a fence joins the fences that are wooden as it is, or not as it is not"),
    ("kConnectWall", "#walls"),
    ("kConnectBars", "an IronBarsBlock (bars, panes)"),
    ("kConnectGateNS", "a FenceGateBlock that joins what stands north or south of it (FenceGateBlock.connectsToDirection)"),
    ("kConnectGateWE", "...west or east of it"),
    ("kUnstableBottomCenter", "#unstable_bottom_center: nothing hangs from the middle of its underside (Block.canSupportCenter)"),
]
# proto::gen::state::kWallAbove: what a wall reads off the state on top of it (WallBlock.updateShape:
# the bottom face of its collision shape against WallBlock.TEST_SHAPES_WALL and TEST_SHAPE_POST).
WALL_ABOVE_FLAGS = [
    ("kCoversNorth", "the wall's north arm is covered: it is tall. This bit << (direction - kNorth) for south, west, east"),
    ("kCoversSouth", ""),
    ("kCoversWest", ""),
    ("kCoversEast", ""),
    ("kCoversPost", "...and its post: a straight wall raises one under it"),
    ("kPostOverride", "#wall_post_override: the same, whatever its shape (a torch, a sign)"),
    ("kWallPost", "a wall with a post of its own (`up`): the wall under it has one too"),
]
# proto::gen::state::kFluidFacts: what a state is to a fluid that would flow into it (FlowingFluid).
FLUID_FACT_FLAGS = [
    ("kHoldsFluid", "FlowingFluid.canHoldAnyFluid: a LiquidBlockContainer, or in #washed_away_by_fluids - a fluid may flow here"),
    ("kFluidContainer", "a LiquidBlockContainer: a fluid that flows here is handed to the block (placeLiquid), which stays"),
    ("kTakesWater", "...and canPlaceLiquid says yes to a source of water, whoever asks: a SimpleWaterloggedBlock of a ported "
                    "class (a double slab says no; one of a class that is not ported is left as it is)"),
    ("kCreativeOnly", "BarrierBlock: it takes water, and gives it up, only at the hands of a player in creative mode"),
    ("kBlocksFlow", "#blocks_fluid_flow: a fluid beside it does not run off over its edge (FlowingFluid.getFlow)"),
    ("kIce", "an IceBlock: no side of it is a wall to a fluid that falls beside it (FlowingFluid.isSolidFace)"),
]
# proto::gen::fluid::kFlags, by fluid state.
FLUID_STATE_FLAGS = [
    ("kIsSource", "FluidState.isSource()"),
    ("kIsFalling", "its FALLING property: the fluid came from above"),
    ("kInWater", "FluidState.is(FluidTags.WATER)"),
    ("kInLava", "FluidState.is(FluidTags.LAVA)"),
]
# Fluid.isSame: a fluid and its flowing kind are one family. proto::gen::fluid::kFamily, kFlowingOf, kSourceOf.
FLUID_FAMILIES = [("kWaterFamily", "minecraft:water", "minecraft:flowing_water"),
                  ("kLavaFamily", "minecraft:lava", "minecraft:flowing_lava")]

EXCEPTIONS_FOR_CONNECTION = ("minecraft:barrier", "minecraft:carved_pumpkin", "minecraft:jack_o_lantern", "minecraft:melon",
                             "minecraft:pumpkin")  # Block.isExceptionForConnection; and LeavesBlock, #shulker_boxes

ITEM_FLAGS = [
    ("kPlainBlockItem", 0x1, "the item class is exactly BlockItem (no place() override)"),
    ("kDestroysInCreative", 0x2, "Item.canDestroyBlock() in creative: no TOOL component, or can_destroy_blocks_in_creative"),
    ("kHasMaxDamage", 0x4, "default components hold MAX_DAMAGE"),
    ("kHasDamage", 0x8, "default components hold DAMAGE (always 0 there)"),
    ("kEquipRestricted", 0x10, "its EQUIPPABLE lists allowed_entities, and players are not among them"),
    ("kExplosionProof", 0x20, "its DAMAGE_RESISTANT covers explosions: as an item entity it survives them (the nether star)"),
    ("kStandingAndWall", 0x40, "StandingAndWallBlockItem: places its block standing, or the block's wall variant (a torch)"),
    ("kFireProof", 0x80, "its DAMAGE_RESISTANT covers fire and lava: as an item entity it does not burn (what is of netherite)"),
]

# net.minecraft.world.entity.EquipmentSlot, by serialized name -> ordinal. The ordinal is what
# set_equipment carries; emit_entities() reads the enum from the jar and checks this table.
EQUIPMENT_SLOTS = ["mainhand", "offhand", "feet", "legs", "chest", "head", "body", "saddle"]

# Hooks vanilla would run on placement / interaction / neighbour change that we do not simulate. A
# block overriding any of them is not placeable, so such a block can never be in the world and the
# server never has to guess what it would have done.
UNSIMULATED_HOOKS = ("onPlace", "neighborChanged", "affectNeighborsAfterRemoval", "tick", "useItemOn",
                     "useWithoutItem", "getFluidState", "canBeReplaced")
BASE_OF_HOOK = {"getStateForPlacement": "Block", "setPlacedBy": "Block", "playerWillDestroy": "Block"}


def hook_default(block: dict, hook: str) -> bool:
    return block["hooks"][hook] == BASE_OF_HOOK.get(hook, "BlockBehaviour")


def placement_kind(block: dict, item: dict | None, ported: tuple | None = None) -> int:
    """`ported`: (kind, placement, item class) from gen_game.PORTED_BLOCKS when the block's own class is one
    the server implements in full - then its hooks are simulated, whatever they are."""
    if item is None or block["game_master"]:
        return 0
    if ported and ported[1]:
        if item["class"] != ported[2]:
            die(f"{block['name']}: its item is a {item['class']}, the port of its class places it with a {ported[2]}")
        return next(value for name, value, _ in BLOCK_FLAGS if name == ported[1])
    if item["class"] != "BlockItem" or block["entity_block"]:
        return 0
    if not all(hook_default(block, h) for h in UNSIMULATED_HOOKS + ("canSurvive", "setPlacedBy", "playerWillDestroy")):
        return 0
    how = block["hooks"]["getStateForPlacement"]
    shape = block["hooks"]["updateShape"]
    kind = 0
    if how == "Block" and shape == "BlockBehaviour":
        kind = 1
    elif how == "RotatedPillarBlock" and shape == "BlockBehaviour" and "axis_states" in block:
        kind = 2
    # A random tick nobody ported is a gap that has a name, never a silent one.
    if kind and not ported and not hook_default(block, "randomTick") and block["class"] not in gen_game.RANDOM_TICK_GAPS:
        die(f"{block['name']}: placeable by the general rule, but {block['hooks']['randomTick']}.randomTick is neither "
            "ported (gen_game.PORTED_BLOCKS) nor a known gap (RANDOM_TICK_GAPS)")
    return kind


def classes_declaring(version: str, classes: set[str], method: str) -> set[str]:
    """Which of `classes` declare `method` themselves (javap -p: static, nothing is executed)."""
    import subprocess
    tool = mcgen.find_tool("javap", version_info(version)["java_version"])
    res = subprocess.run([tool, "-p", "-cp", str(jar_for(version, client=False)), *sorted(classes)], capture_output=True, text=True)
    if res.returncode != 0:
        die(f"javap failed: {res.stderr.strip()}")
    out: set[str] = set()
    current = None
    for line in res.stdout.splitlines():
        m = re.match(r"^(?:public |protected |private |abstract |final |static )*(?:class|interface|enum) ([\w.$]+)", line)
        if m:
            current = m.group(1)
        elif current and re.search(r"\b" + re.escape(method) + r"\(", line):
            out.add(current)
    return out


def blocks_with_custom_clone(version: str, block_classes: set[str]) -> set[str]:
    """The block classes (by the short name blockdata.json uses) that do not inherit
    BlockBehaviour.getCloneItemStack - `new ItemStack(asItem())` - unchanged."""
    base = "net.minecraft.world.level.block.state.BlockBehaviour"
    with zipfile.ZipFile(jar_for(version, client=False)) as z:
        names = [n[:-6].replace("/", ".") for n in z.namelist() if n.startswith("net/minecraft/world/level/block/") and n.endswith(".class")]
    full: dict[str, str] = {}
    for short in block_classes:
        found = [n for n in names if n.rsplit(".", 1)[1] == short]
        if len(found) != 1:
            die(f"block class {short}: {len(found)} classes of that name in the jar")
        full[short] = found[0]
    parents = class_parents(version, set(full.values()))
    declaring = classes_declaring(version, set(parents) | set(parents.values()) - {""}, "getCloneItemStack")
    if base not in declaring:
        die("BlockBehaviour.getCloneItemStack is gone: pick block works differently now, re-read handlePickItemFromBlock")
    out = set()
    for short, cls in full.items():
        while cls != base:
            if cls in declaring:
                out.add(short)
                break
            if cls not in parents or not parents[cls]:
                die(f"block class {short} does not descend from BlockBehaviour")
            cls = parents[cls]
    return out


def swing_animations(version: str) -> tuple[dict[str, int], str, int]:
    """SwingAnimationType by serialized name -> network id, and SwingAnimation.DEFAULT (type name,
    ticks): what an item's attack_animation component says when it says nothing."""
    types = {name: int(i) for i, name in re.findall(
        r"^\s+\w+ = new SwingAnimationType\(\"\w+\", \d+, (\d+), \"(\w+)\"\);$",
        pseudo_of(version, ["net.minecraft.world.item.SwingAnimationType"], r"static \{\}"), re.M)}
    m = re.search(r"^\s+DEFAULT = new SwingAnimation\(SwingAnimationType\.(\w+), (\d+)\);$",
                  pseudo_of(version, ["net.minecraft.world.item.component.SwingAnimation"], r"static \{\}"), re.M)
    if len(types) < 3 or not m or m.group(1).lower() not in types:
        die("SwingAnimationType / SwingAnimation.DEFAULT: could not read them from the jar")
    return types, m.group(1).lower(), int(m.group(2))


def u16s(values) -> bytes:
    out = bytearray()
    for x in values:
        if not 0 <= x <= 0xFFFF:
            die(f"value {x} does not fit the 16-bit state tables")
        out += x.to_bytes(2, "little")
    return bytes(out)


def cfloat(x: float) -> str:
    """Shortest literal that round-trips: 17 significant digits always do for a double."""
    r = repr(float(x))
    return r if ("." in r or "e" in r or "n" in r) else r + ".0"


def build_block_tables(v: dict, blocks: dict, data: dict, static_regs: dict, vanilla: "VanillaData", comp_dir: Path,
                       game: dict) -> dict:
    """Cross-check the three sources (blocks.json, registries.json, blockdata.json) and derive every
    table the C++ side embeds. Returns {file name: bytes | str}."""
    n = data["state_count"]
    st = data["states"]
    if data["version"] != v["id"]:
        die(f"blockdata.json is for {data['version']}, expected {v['id']} - run: mcgen.py blockdata")
    if data["directions"] != ["down", "up", "north", "south", "west", "east"]:
        die(f"unexpected Direction order {data['directions']}: the occlusion matrix is indexed by it")
    if n != sum(len(b["states"]) for b in blocks.values()) or n > 0x10000:
        die("state count mismatch between blocks.json and blockdata.json (or > 65536 states: widen the tables)")

    block_ids = {name: e["protocol_id"] for name, e in static_regs["minecraft:block"]["entries"].items()}
    item_ids = {name: e["protocol_id"] for name, e in static_regs["minecraft:item"]["entries"].items()}
    sound_ids = {name: e["protocol_id"] for name, e in static_regs["minecraft:sound_event"]["entries"].items()}
    fluid_ids = {name: e["protocol_id"] for name, e in static_regs["minecraft:fluid"]["entries"].items()}
    if data["fluids"] != sorted(fluid_ids, key=fluid_ids.get) or fluid_ids.get("minecraft:empty") != 0:
        die("fluid registry order differs between registries.json and blockdata.json")

    ext_blocks = sorted(data["blocks"], key=lambda b: b["id"])
    ext_items = {i["name"]: i for i in data["items"]}
    if [b["name"] for b in ext_blocks] != sorted(block_ids, key=block_ids.get):
        die("block registry differs between registries.json and blockdata.json")
    if {i["name"]: i["id"] for i in data["items"]} != item_ids:
        die("item registry differs between registries.json and blockdata.json")

    # State ids are handed out block by block in registry order; the tables rely on that.
    first: dict[str, int] = {}
    nxt = 0
    for b in ext_blocks:
        ids = sorted(s["id"] for s in blocks[b["name"]]["states"])
        if ids != list(range(nxt, nxt + len(ids))):
            die(f"{b['name']}: block state ids are not contiguous in registry order")
        default = next(s["id"] for s in blocks[b["name"]]["states"] if s.get("default"))
        if default != b["default_state"] or any(st["block"][i] != b["id"] for i in ids):
            die(f"{b['name']}: blocks.json and blockdata.json disagree")
        first[b["name"]] = nxt
        nxt += len(ids)

    block_tags = flatten_tags(vanilla.tags("block"), block_ids, "minecraft:block")

    def tagged(tag: str) -> set[int]:
        if f"minecraft:{tag}" not in block_tags:
            die(f"block tag #{tag} is missing: a heightmap/behaviour predicate moved, re-read it in the jar")
        return set(block_tags[f"minecraft:{tag}"])

    motion, motion_nl, snow = tagged("blocks_motion_in_heightmap"), tagged("blocks_motion_in_heightmap_no_leaves"), tagged("snow")

    # ---- per-state tables
    bit = {name: 1 << i for i, (name, _) in enumerate(STATE_FLAGS)}
    if len(STATE_FLAGS) > 8:
        die("more than 8 state flags: kFlags is one byte per state")
    conductor = game["states"]["columns"]["redstone_conductor"]
    if game["states"]["state_count"] != n or len(conductor) != n or game["states"]["columns"]["block"] != st["block"]:
        die("gamedata/states.json does not describe the same block states as blockdata.json - run: mcgen.py gamedata")
    flags = bytearray(n)
    light = bytearray(n)
    for i in range(n):
        blk = st["block"][i]
        fluid = st["fluid"][i] != 0
        f = 0
        f |= bit["kIsAir"] if st["air"][i] else 0
        f |= bit["kHasFluid"] if fluid else 0
        f |= bit["kMotionBlocking"] if (blk in motion or fluid) else 0
        f |= bit["kMotionBlockingNoLeaves"] if (blk in motion_nl or fluid) else 0
        f |= bit["kReplaceable"] if st["replaceable"][i] else 0
        f |= bit["kRandomTicks"] if st["randomly_ticking"][i] else 0
        f |= bit["kBlockEntity"] if st["has_block_entity"][i] else 0
        f |= bit["kRedstoneConductor"] if conductor[i] else 0
        flags[i] = f
        if not (0 <= st["emission"][i] <= 15 and 0 <= st["dampening"][i] <= 15):
            die(f"state {i}: light values out of nibble range")
        light[i] = st["emission"][i] << 4 | st["dampening"][i]
        if st["air"][i] and (st["dampening"][i] or st["emission"][i] or st["occlusion_tuple"][i] or fluid):
            die(f"state {i}: an air state with light properties breaks the uniform-air shortcuts")
        # world::light_differs() reads "uses its shape for light occlusion" off the tuple id. That is
        # only the same thing while every such state really has an occluding face.
        if bool(st["use_shape_for_light_occlusion"][i]) != (st["occlusion_tuple"][i] != 0):
            die(f"state {i}: useShapeForLightOcclusion without an occlusion shape - give it its own flag bit")

    tuples = data["occlusion_tuples"]
    occ = data["face_occludes"]
    if len(tuples) > 64 or tuples[0] != [0] * 6:
        die(f"{len(tuples)} occlusion tuples: kOccludes packs one row per u64, widen it")
    opposite = [1, 0, 3, 2, 5, 4]
    # kOccludes[a][d] bit b: light leaving a block of tuple a in direction d is stopped by the pair of
    # touching faces when it enters a block of tuple b (LightEngine.shapeOccludes).
    matrix = [[sum(1 << b for b in range(len(tuples)) if occ[tuples[a][d]][tuples[b][opposite[d]]] == "1")
               for d in range(6)] for a in range(len(tuples))]

    # ---- what a block change, a scheduled tick and a random tick ask of a state
    ported_of = {}
    for b in ext_blocks:
        gb = game["blocks"].get(b["name"])
        if gb is None or gb["id"] != b["id"]:
            die(f"{b['name']}: gamedata/blocks.json is from another version - run: mcgen.py gamedata")
        ported_of[b["name"]] = gen_game.ported_block(gb)
    hbit = {name: 1 << i for i, (name, _, _) in enumerate(HOOK_FLAGS)}
    rbit = {name: 1 << i for i, (name, _) in enumerate(RANDOM_FLAGS)}
    if len(HOOK_FLAGS) > 8 or len(RANDOM_FLAGS) > 8:
        die("kHooks and kRandom are one byte per state")
    cols = game["states"]["columns"]
    fire = tagged("fire")
    fluid_tags = flatten_tags(vanilla.tags("fluid"), fluid_ids, "minecraft:fluid")
    if "minecraft:water" not in fluid_tags:
        die("fluid tag #water is missing: SpreadingSnowyBlock.canPropagate asks it")
    water = set(fluid_tags["minecraft:water"])
    fluid_states = game["fluids"]["states"]
    if [f["id"] for f in fluid_states] != list(range(len(fluid_states))) or len(cols["fluid_state"]) != n:
        die("gamedata/fluids.json: fluid states are not listed by id")
    snow_one = next(s["id"] for s in blocks["minecraft:snow"]["states"] if s["properties"] == {"layers": "1"})
    soil: set[int] = set()
    for b in ext_blocks:
        gb = game["blocks"][b["name"]]
        if (ported_of[b["name"]] or (None,))[0] == "kSpreading":
            base = gb["fields"].get("baseBlock")
            if base != "minecraft:dirt":
                die(f"{b['name']}: spreads to {base}; the port and its tables take one soil for every spreader")
            soil.add(block_ids[base])
    up = 1  # Direction.UP
    hooks = bytearray(n)
    random_ticks = bytearray(n)
    for i in range(n):
        b = ext_blocks[st["block"][i]]
        ported = ported_of[b["name"]]
        kind = ported[0] if ported else "kPlain"
        gh = game["blocks"][b["name"]]["hooks"]
        h = 0
        if ported:
            for name, hook, _ in HOOK_FLAGS:
                if hook == "updateShape" and gh.get(hook) in gen_game.NO_SHAPE_HOOK and cols["fluid_state"][i] == 0:
                    continue   # (it asks for the water's tick and nothing else: nothing to do for a dry one)
                h |= hbit[name] if hook and hook in gh else 0
        # FallingBlock.isFree
        h |= hbit["kFallThrough"] if (st["air"][i] or b["id"] in fire or cols["liquid"][i] or st["replaceable"][i]) else 0
        hooks[i] = h
        r = 0
        weathers = bool(ported) and game["blocks"][b["name"]]["class"][0] in gen_game.WEATHERING_CLASSES
        if st["randomly_ticking"][i] and (kind in gen_game.RANDOM_TICK_KINDS or weathers):
            r |= rbit["kRandomTick"] | (rbit["kRandomAlways"] if kind in gen_game.RANDOM_TICK_ALWAYS or weathers else 0)
        elif st["randomly_ticking"][i] and ported and gh.get("randomTick") not in gen_game.RANDOM_TICK_GAPS:
            die(f"{b['name']}: its class is ported, {gh.get('randomTick')}.randomTick is not, and it is no known gap "
                "(gen_game.RANDOM_TICK_KINDS, RANDOM_TICK_GAPS)")
        if kind in ("kSpreading", "kNylium"):
            # LightEngine.getLightDampeningInto(this, above, UP, ...) is read below with an empty shape for `this`.
            if st["occlusion_tuple"][i] != 0 or not st["randomly_ticking"][i]:
                die(f"{b['name']}: a spreading block that uses its shape for light occlusion, or does not tick")
            r |= rbit["kSpreader"] if kind == "kSpreading" else rbit["kNylium"]
        r |= rbit["kSoil"] if b["id"] in soil else 0
        # As the block above: LightEngine.getLightDampeningInto(below, this, UP, this.getLightDampening()) -
        # 16 when the two faces together shut the light out, else its dampening - is 15 or more.
        dark = ((matrix[0][up] >> st["occlusion_tuple"][i]) & 1) or st["dampening"][i] >= 15
        fluid = fluid_states[cols["fluid_state"][i]]
        if fluid_ids[fluid["fluid"]] != st["fluid"][i]:
            die(f"state {i}: gamedata and blockdata disagree on its fluid")
        smothers = i != snow_one and (fluid["amount"] == 8 or dark)  # SpreadingSnowyBlock.canStayAlive, negated
        r |= rbit["kSmothers"] if smothers else 0
        r |= rbit["kNoSpread"] if (smothers or st["fluid"][i] in water) else 0  # canPropagate, negated
        r |= rbit["kSmothersNylium"] if dark else 0                              # NyliumBlock.canBeNylium, negated
        random_ticks[i] = r
    for col in ("sturdy_full", "sturdy_center"):
        if len(cols[col]) != n or any(not 0 <= v < 64 for v in cols[col]):
            die(f"gamedata/states.json: column {col} is not six bits a state")
    if game["states"]["legend"]["directions"] != ["DOWN", "UP", "NORTH", "SOUTH", "WEST", "EAST"]:
        die("gamedata/states.json: the sturdy columns are not in Direction order")

    shapes = data["collision_shapes"]
    coll = [0xFFFF if c < 0 else c for c in st["collision_shape"]]
    # Block.isFaceFull(collision shape, DOWN): do the boxes that reach the bottom cover it? (A
    # shape is on a grid of sixteenths, a few on thirty-seconds: sampled at sixty-fourths.)
    def covers_bottom(boxes) -> bool:
        low = [b for b in boxes if b[1] <= 0.0]
        return bool(low) and all(any(b[0] <= (x + 0.5) / 64 <= b[3] and b[2] <= (z + 0.5) / 64 <= b[5] for b in low)
                                 for x in range(64) for z in range(64))
    full_below = {0xFFFF: False, **{i: covers_bottom(boxes) for i, boxes in enumerate(shapes)}}

    # ExplosionDamageCalculator.getBlockExplosionResistance takes the more of the block's and its
    # fluid's; the server reads the fluid's off the fluid's block (world::blast_positions).
    for fluid_class, fluid_block in (("WaterFluid", "minecraft:water"), ("LavaFluid", "minecraft:lava")):
        own = re.search(r"getExplosionResistance\(\) \{\s*return ([0-9.]+)f?;", pseudo_of(
            v["id"], [f"net.minecraft.world.level.material.{fluid_class}"], "getExplosionResistance"))
        block = next(b for b in ext_blocks if b["name"] == fluid_block)
        if not own or float(own.group(1)) != block["explosion_resistance"]:
            die(f"{fluid_class}.getExplosionResistance is not {fluid_block}'s any more: give fluids a resistance of their own")

    # ---- what a fence, a pane and a wall ask of the states around them
    cbit = {name: 1 << i for i, (name, _) in enumerate(CONNECT_FLAGS)}
    wbit = {name: 1 << i for i, (name, _) in enumerate(WALL_ABOVE_FLAGS)}
    fences, wooden_fences, walls = tagged("fences"), tagged("wooden_fences"), tagged("walls")
    shulkers, post_override, unstable_bottom = tagged("shulker_boxes"), tagged("wall_post_override"), tagged("unstable_bottom_center")
    for name in EXCEPTIONS_FOR_CONNECTION:
        if name not in block_ids:
            die(f"{name} is no block any more: read Block.isExceptionForConnection again")
    exceptions = {block_ids[name] for name in EXCEPTIONS_FOR_CONNECTION} | shulkers
    wall_tests = game["statics"].get("net.minecraft.world.level.block.WallBlock", {})
    try:
        (post_test,) = wall_tests["TEST_SHAPE_POST"]
        side_tests = [wall_tests["TEST_SHAPES_WALL"][d][0] for d in ("NORTH", "SOUTH", "WEST", "EAST")]
        if any(len(wall_tests["TEST_SHAPES_WALL"][d]) != 1 for d in ("NORTH", "SOUTH", "WEST", "EAST")):
            raise ValueError
    except (KeyError, ValueError):
        die("gamedata/statics.json: WallBlock.TEST_SHAPE_POST / TEST_SHAPES_WALL are not one box each any more")
    game_shapes, game_coll = game["states"]["shapes"], cols["collision_shape"]

    def covers(rects: list, test: list) -> bool:
        """WallBlock.isCovered: no part of the test shape's column lies outside the rectangles
        (x0, z0, x1, z1) - Shapes.joinIsNotEmpty(test, face, ONLY_FIRST), which takes coordinates
        nearer than 1e-7 for one."""
        x0, z0, x1, z1 = test[0], test[2], test[3], test[5]
        xs = sorted({x0, x1} | {v for r in rects for v in (r[0], r[2]) if x0 < v < x1})
        zs = sorted({z0, z1} | {v for r in rects for v in (r[1], r[3]) if z0 < v < z1})
        for xa, xb in zip(xs, xs[1:]):
            for za, zb in zip(zs, zs[1:]):
                if xb - xa < 1e-7 or zb - za < 1e-7:
                    continue
                cx, cz = (xa + xb) / 2, (za + zb) / 2
                if not any(r[0] <= cx <= r[2] and r[1] <= cz <= r[3] for r in rects):
                    return False
        return True

    connect = bytearray(n)
    wall_above = bytearray(n)
    cover_cache: dict[tuple, int] = {}
    for b in ext_blocks:
        gb = game["blocks"][b["name"]]
        chain = gb["class"]
        c = 0
        c |= cbit["kConnectException"] if (b["id"] in exceptions or "LeavesBlock" in chain) else 0
        c |= cbit["kConnectFence"] if b["id"] in fences else 0
        c |= cbit["kConnectWoodenFence"] if b["id"] in wooden_fences else 0
        c |= cbit["kConnectWall"] if b["id"] in walls else 0
        c |= cbit["kConnectBars"] if "IronBarsBlock" in chain else 0
        c |= cbit["kUnstableBottomCenter"] if b["id"] in unstable_bottom else 0
        if b["id"] in wooden_fences and b["id"] not in fences:
            die(f"{b['name']}: in #wooden_fences and not in #fences - FenceBlock.isSameFence asks #fences first")
        # What the server takes a block's kind for: the class, with its subclasses.
        for cls, kind in (("StairBlock", "kStairs"), ("WallBlock", "kWall")):
            if (cls in chain) != ((ported_of[b["name"]] or (None,))[0] == kind):
                die(f"{b['name']}: {cls} and block_kind::{kind} are not the same blocks - StairBlock.isStairs is read off the kind")
        for s in blocks[b["name"]]["states"]:
            i, props = s["id"], s.get("properties", {})
            f = c
            if "FenceGateBlock" in chain:
                if props.get("facing") not in ("north", "south", "west", "east"):
                    die(f"{b['name']}: a fence gate without a horizontal `facing`")
                # connectsToDirection(state, d): facing.getAxis() == d.getClockWise().getAxis()
                f |= cbit["kConnectGateNS"] if props["facing"] in ("west", "east") else cbit["kConnectGateWE"]
            connect[i] = f
            # BlockState.getCollisionShape(level, pos).getFaceShape(DOWN): the cells of the layer at
            # y = 1e-7 (VoxelShape.calculateFace), which is what its boxes are that reach across it.
            # A shape that depends on where the block is (bamboo, dripstone) or on its block
            # entity (a shulker box) is taken as gamedata asked for it: at no offset, shut.
            boxes = shapes[st["collision_shape"][i]] if st["collision_shape"][i] >= 0 else game_shapes[game_coll[i]]
            key = tuple(tuple(q) for q in boxes)
            if key not in cover_cache:
                rects = [(q[0], q[2], q[3], q[5]) for q in boxes if q[1] <= 1e-7 < q[4]]
                w = wbit["kCoversPost"] if covers(rects, post_test) else 0
                for d, test in enumerate(side_tests):
                    w |= wbit["kCoversNorth"] << d if covers(rects, test) else 0
                cover_cache[key] = w
            w = cover_cache[key]
            w |= wbit["kPostOverride"] if b["id"] in post_override else 0
            if "WallBlock" in chain:
                if props.get("up") not in ("true", "false"):
                    die(f"{b['name']}: a wall without `up`")
                w |= wbit["kWallPost"] if props["up"] == "true" else 0
            wall_above[i] = w
    # ---- what a plant asks of the state under it (gen_game.PLANT_SUPPORT): a bit a rule
    def support_rule(block_name: str) -> str | None:
        gb = game["blocks"][block_name]
        rule = gen_game.PLANT_SUPPORT.get(gb["class"][0])
        if rule is None:
            return None
        if rule.startswith("field:"):
            rule = gb["fields"].get(rule[6:], "")
            if not rule.startswith("#minecraft:"):
                die(f"{block_name}: its {gb['class'][0]} names no tag of blocks it stands on")
            rule = "#" + rule[11:]
        return rule

    def supporting(rule: str) -> bytes:
        """One byte a state: may a plant of this rule stand on it?"""
        if rule.startswith("#"):
            members = block_id_set(tagged(rule[1:]))
            return bytes(1 if st["block"][i] in members else 0 for i in range(n))
        if rule == "cactus_flower":
            members = block_id_set(tagged("support_override_cactus_flower"))
            return bytes(1 if st["block"][i] in members or (cols["sturdy_center"][i] >> up) & 1 else 0 for i in range(n))
        if rule == "sturdy_top":
            return bytes((cols["sturdy_full"][i] >> up) & 1 for i in range(n))
        die(f"gen_game.PLANT_SUPPORT: {rule} is not a rule gen.py knows")

    def block_id_set(ids: set[int]) -> set[int]:
        index_of = {b["id"]: k for k, b in enumerate(ext_blocks)}  # st["block"] holds indices into ext_blocks
        return {index_of[i] for i in ids}

    support_bits: dict[bytes, int] = {}   # the states a rule accepts -> its bit (two tags with the same members are one)
    support_of: dict[str, int] = {}       # block -> the bit of its rule
    for b in ext_blocks:
        rule = support_rule(b["name"])
        if rule is None:
            if (ported_of[b["name"]] or (None,))[0] in ("kPlant", "kSegmented"):
                die(f"{b['name']}: a plant whose class has no entry in gen_game.PLANT_SUPPORT")
            continue
        if (ported_of[b["name"]] or (None,))[0] not in ("kPlant", "kSegmented"):
            die(f"{b['name']}: in gen_game.PLANT_SUPPORT, and not a ported plant")
        accepts = supporting(rule)
        support_of[b["name"]] = support_bits.setdefault(accepts, 1 << len(support_bits))
    if len(support_bits) > 8:
        die("state::kSupport is one byte a state: more than eight different rules of what a plant stands on")
    support = bytearray(n)
    for accepts, mask in support_bits.items():
        for i in range(n):
            support[i] |= mask if accepts[i] else 0

    stone = next((b["default_state"] for b in ext_blocks if b["name"] == "minecraft:stone"), None)
    if stone is None or wall_above[stone] != 0x1F or wall_above[0] != 0:
        die("kWallAbove: a full block does not cover a wall's every test shape, or air covers one")

    # ---- fluids: the FluidState a block state holds, by its id (Fluid.FLUID_STATE_REGISTRY), and
    # what each of those is. A flowing fluid and its source are one family (Fluid.isSame) of two fluids.
    fluid_col = cols["fluid_state"]
    if {f["name"]: f["id"] for f in game["fluids"]["fluids"]} != fluid_ids:
        die("gamedata/fluids.json and registries.json disagree on the fluid registry")
    if len(fluid_states) > 255 or len(FLUID_STATE_FLAGS) > 8 or len(FLUID_FACT_FLAGS) > 8:
        die("state::kFluid, fluid::kFlags and state::kFluidFacts are one byte each")
    fsbit = {name: 1 << i for i, (name, _) in enumerate(FLUID_STATE_FLAGS)}
    family_of: dict[str, int] = {}
    for k, (_, source, flowing) in enumerate(FLUID_FAMILIES):
        family_of[source] = family_of[flowing] = k
        # FluidTags.WATER / LAVA are asked where Fluid.isSame is: they must name the same fluids.
        if set(fluid_tags.get(source, ())) != {fluid_ids[source], fluid_ids[flowing]}:
            die(f"fluid tag #{source.split(':', 1)[1]} is not {source} and {flowing}: the port takes the tag for the family")
        for f in game["fluids"]["fluids"]:
            if f["name"] in (source, flowing) and (f.get("source"), f.get("flowing")) != (source, flowing):
                die(f"{f['name']}: its source and its flowing fluid are not {source} and {flowing}")
    in_tag = {"kInWater": water, "kInLava": set(fluid_tags["minecraft:lava"])}
    fluid_type, fluid_amount, fluid_flags, fluid_legacy = [], [], [], []
    flowing_of = [[[0] * 9 for _ in range(2)] for _ in FLUID_FAMILIES]   # [family][falling][amount]
    source_of = [[0, 0] for _ in FLUID_FAMILIES]                          # [family][falling]
    for fs in fluid_states:
        fid, fam, falling = fluid_ids[fs["fluid"]], family_of.get(fs["fluid"]), int(bool(fs.get("falling", False)))
        if fs["empty"] != (fid == 0) or fs["empty"] != (fam is None):
            die(f"fluid state {fs['id']}: a fluid of no family that is not the empty one")
        if abs(fs["own_height"] - fs["amount"] / 9.0) > 1e-6:
            die(f"fluid state {fs['id']}: FlowingFluid.getOwnHeight is not amount / 9 any more")
        if fam is not None:
            _, source, flowing = FLUID_FAMILIES[fam]
            if fs["source"] and (fs["fluid"] != source or fs["amount"] != 8):
                die(f"fluid state {fs['id']}: a source that is not {source} with amount 8")
            if not fs["source"] and (fs["fluid"] != flowing or not 1 <= fs["amount"] <= 8):
                die(f"fluid state {fs['id']}: a flowing fluid that is not {flowing} with an amount of 1..8")
            if fs["source"]:
                source_of[fam][falling] = fs["id"]
            else:
                flowing_of[fam][falling][fs["amount"]] = fs["id"]
        fluid_type.append(fid)
        fluid_amount.append(fs["amount"])
        fluid_flags.append((fsbit["kIsSource"] if fs["source"] else 0) | (fsbit["kIsFalling"] if falling else 0) |
                           sum(fsbit[flag] for flag, members in in_tag.items() if fid in members))
        fluid_legacy.append(fs["legacy_block"])
    if any(0 in fam[f][1:] for fam in flowing_of for f in (0, 1)) or any(0 in fam for fam in source_of):
        die("gamedata/fluids.json: a family without a fluid state for some amount, falling or not")
    for i in range(n):
        if fluid_legacy[fluid_col[i]] != st["fluid_legacy_block"][i]:
            die(f"state {i}: its fluid state's legacy block is not what blockdata.json says removeBlock leaves")
    for k, (_, source, _) in enumerate(FLUID_FAMILIES):
        # LiquidBlock.getFluidState: level 0 the source, 1..7 flowing with 8 - level, 8 and more falling.
        liquid = game["blocks"].get(source)
        if not liquid or liquid["class"][0] != "LiquidBlock" or [(q["name"], q["stride"], len(q["values"])) for q in liquid[
                "properties"]] != [("level", 1, 16)]:
            die(f"{source}: the block of the fluid is no LiquidBlock with one property, level 0..15")
        for level in range(16):
            want = source_of[k][0] if level == 0 else flowing_of[k][0][8 - level] if level < 8 else flowing_of[k][1][8]
            if fluid_col[liquid["first_state"] + level] != want:
                die(f"{source}[level={level}]: LiquidBlock.getFluidState is not what the flow's port takes it for")
        # What the flow makes - a source, 1..7 of it flowing, a falling column - is a block state
        # that holds exactly that again (FlowingFluid.getLegacyLevel).
        for made in [source_of[k][0], flowing_of[k][1][8]] + flowing_of[k][0][1:8]:
            if fluid_col[fluid_legacy[made]] != made:
                die(f"fluid state {made}: its legacy block holds another fluid state")

    ffbit = {name: 1 << i for i, (name, _) in enumerate(FLUID_FACT_FLAGS)}
    washed = tagged("washed_away_by_fluids")
    fluid_facts = bytearray(n)
    wet_stride: dict[str, int] = {}     # a ported block that holds water by itself -> the stride of its `waterlogged`
    blocks_flow = tagged("blocks_fluid_flow")
    for b in ext_blocks:
        gb = game["blocks"][b["name"]]
        container = "LiquidBlockContainer" in gb["interfaces"]
        f = (ffbit["kHoldsFluid"] if container or b["id"] in washed else 0) | (ffbit["kFluidContainer"] if container else 0)
        f |= (ffbit["kBlocksFlow"] if b["id"] in blocks_flow else 0) | (ffbit["kIce"] if "IceBlock" in gb["class"] else 0)
        simple = "SimpleWaterloggedBlock" in gb["interfaces"] and ported_of[b["name"]] is not None
        if simple:
            wet = [q for q in gb["properties"] if q["name"] == "waterlogged"]
            if len(wet) != 1 or wet[0]["values"] != ["true", "false"]:
                die(f"{b['name']}: a SimpleWaterloggedBlock whose `waterlogged` is not (true, false)")
            wet_stride[b["name"]] = wet[0]["stride"]
            f |= ffbit["kCreativeOnly"] if "BarrierBlock" in gb["class"] else 0
        for s in blocks[b["name"]]["states"]:
            g = f
            if simple:
                is_wet = s["properties"]["waterlogged"] == "true"
                if is_wet != (fluid_states[fluid_col[s["id"]]]["fluid"] == "minecraft:water"):
                    die(f"{b['name']}: `waterlogged` and holding a source of water are not the same states")
                if "BarrierBlock" not in gb["class"] and not ("SlabBlock" in gb["class"] and s["properties"]["type"] == "double"):
                    g |= ffbit["kTakesWater"]
            fluid_facts[s["id"]] = g
    # Who says otherwise than SimpleWaterloggedBlock: the classes of ported blocks that declare one
    # of its three methods themselves (javap: nothing is run).
    liquid_classes = {c for b in ext_blocks if ported_of[b["name"]] and "BucketPickup" in game["blocks"][b["name"]]["interfaces"]
                      for c in game["blocks"][b["name"]]["class"]} - {"Block", "BlockBehaviour"}
    with zipfile.ZipFile(jar_for(v["id"], client=False)) as z:
        block_classes = {n[:-6].replace("/", ".") for n in z.namelist()
                         if n.startswith("net/minecraft/world/level/block/") and n.endswith(".class")}
    full_names = {n for n in block_classes if n.rsplit(".", 1)[1] in liquid_classes}
    if {n.rsplit(".", 1)[1] for n in full_names} != liquid_classes:
        die(f"block classes that are not in the jar under their names: {sorted(liquid_classes - {n.rsplit('.', 1)[1] for n in full_names})}")
    says = {cls.rsplit(".", 1)[1] for method in ("canPlaceLiquid", "placeLiquid", "pickupBlock")
            for cls in classes_declaring(v["id"], full_names, method)}
    if says != gen_game.LIQUID_OVERRIDES & liquid_classes or not gen_game.LIQUID_OVERRIDES <= liquid_classes:
        die(f"canPlaceLiquid / placeLiquid / pickupBlock are declared by {sorted(says)} among the ported classes, the port knows "
            f"{sorted(gen_game.LIQUID_OVERRIDES)}: read the class, then gen_game.LIQUID_OVERRIDES")
    slab = next(b for b in ext_blocks if b["name"] == "minecraft:oak_slab")
    if (fluid_facts[slab["default_state"]] != ffbit["kHoldsFluid"] | ffbit["kFluidContainer"] | ffbit["kTakesWater"] | ffbit["kBlocksFlow"] or
            fluid_facts[stone] != ffbit["kBlocksFlow"] or fluid_facts[0] != ffbit["kHoldsFluid"]):
        die("kFluidFacts: a slab is no LiquidBlockContainer that takes water, stone holds a fluid, or air does not "
            "(#washed_away_by_fluids)")

    # ---- FlowingFluid.canPassThroughWall(direction, from, to) = !Shapes.mergedFaceOccludes(the
    # collision shape of `from`, that of `to`, direction): of each shape the layer of cells at the
    # face between the two - if the shape reaches that face at all - and the two layers together
    # must not cover the face. A layer is the footprints of the shape's boxes that reach the face;
    # layers that answer alike against every other are one class, the six classes of a state one
    # tuple (as light's kOccludes is made). The shape of a block whose shape depends on where it is
    # or on its block entity is taken as gamedata asked for it: at no offset, shut.
    axis_of = (1, 1, 2, 2, 0, 0)                      # Direction: down, up, north, south, west, east
    others = {0: (1, 2), 1: (0, 2), 2: (0, 1)}
    layer_ids: dict[tuple, int] = {(): 0}

    def face_layers(boxes) -> tuple:
        out = []
        for d in range(6):
            a = axis_of[d]
            u, w = others[a]
            rects: list = []
            if boxes and d & 1:                       # up, south, east: where the shape ends at 1
                edge = max(q[a + 3] for q in boxes)
                if abs(edge - 1.0) <= 1e-7:
                    rects = sorted((q[u], q[w], q[u + 3], q[w + 3]) for q in boxes if q[a + 3] >= edge - 1e-7)
            elif boxes:                               # down, north, west: where it begins at 0
                edge = min(q[a] for q in boxes)
                if abs(edge) <= 1e-7:
                    rects = sorted((q[u], q[w], q[u + 3], q[w + 3]) for q in boxes if q[a] <= edge + 1e-7)
            out.append(layer_ids.setdefault(tuple(rects), len(layer_ids)))
        return tuple(out)

    layers_cache: dict[tuple, tuple] = {}
    state_layers = []
    for i in range(n):
        boxes = shapes[st["collision_shape"][i]] if st["collision_shape"][i] >= 0 else game_shapes[game_coll[i]]
        key = tuple(tuple(q) for q in boxes)
        if key not in layers_cache:
            layers_cache[key] = face_layers(boxes)
        state_layers.append(layers_cache[key])
    layer_rects = sorted(layer_ids, key=layer_ids.get)
    unit = [0.0, 0.0, 0.0, 1.0, 1.0, 1.0]
    together = [[bool(ra or rb) and covers(list(ra) + list(rb), unit) for rb in layer_rects] for ra in layer_rects]
    class_ids: dict[tuple, int] = {}
    class_of = [class_ids.setdefault(tuple(row), len(class_ids)) for row in together]
    if len(class_ids) > 64:
        die(f"{len(class_ids)} classes of face layers: kFlowCovers packs one row per u64, widen it")
    flow_covers = [0] * len(class_ids)
    for a, row in enumerate(together):
        for b, hit in enumerate(row):
            flow_covers[class_of[a]] |= 1 << class_of[b] if hit else 0
    for a, row in enumerate(together):   # (two layers of one class answer alike to every class: the relation is symmetric)
        if any(hit != bool(flow_covers[class_of[a]] >> class_of[b] & 1) for b, hit in enumerate(row)):
            die("kFlowCovers: layers of one class that do not cover the same")
    tuple_ids: dict[tuple, int] = {}
    flow_faces = bytearray(n)
    for i in range(n):
        key = tuple(class_of[layer] for layer in state_layers[i])
        if key not in tuple_ids and len(tuple_ids) == 256:
            die("more than 256 tuples of face layer classes: state::kFlowFaces is one byte a state")
        flow_faces[i] = tuple_ids.setdefault(key, len(tuple_ids))
    flow_tuples = sorted(tuple_ids, key=tuple_ids.get)

    def fluid_passes(a: int, b: int, d: int) -> bool:
        return not flow_covers[flow_tuples[flow_faces[a]][d]] >> flow_tuples[flow_faces[b]][d ^ 1] & 1
    if (flow_tuples[0] != (0,) * 6 or not all(fluid_passes(0, 0, d) for d in range(6)) or
            any(fluid_passes(stone, 0, d) or fluid_passes(0, stone, d) for d in range(6))):
        die("kFlowCovers: a fluid does not pass from air to air, or passes a full block")
    if len(shapes) >= 0xFFFF:
        die("too many collision shapes for 16-bit ids")

    files: dict = {
        "state_flags.bin": bytes(flags),
        "state_light.bin": bytes(light),
        "state_occlusion.bin": bytes(st["occlusion_tuple"]),
        "state_block.bin": u16s(st["block"]),
        "state_collision.bin": u16s(coll),
        "state_fluid.bin": bytes(fluid_col),
        "state_fluid_facts.bin": bytes(fluid_facts),
        "state_flow.bin": bytes(flow_faces),
        "state_hooks.bin": bytes(hooks),
        "state_random.bin": bytes(random_ticks),
        "state_sturdy_full.bin": bytes(cols["sturdy_full"]),
        "state_sturdy_center.bin": bytes(cols["sturdy_center"][i] | (0x40 if st["solid"][i] else 0) |
                                         (0x80 if full_below[coll[i]] else 0) for i in range(n)),
        "state_connect.bin": bytes(connect),
        "state_wall_above.bin": bytes(wall_above),
        "state_support.bin": bytes(support),
    }

    hdr = HEADER.format(id=v["id"], proto=v["protocol_version"])
    def carray(values, per_line: int = 24) -> str:
        return "".join("    " + ", ".join(str(x) for x in values[k:k + per_line]) + ",\n" for k in range(0, len(values), per_line))

    fl = ["""
// Fluids: BuiltInRegistries.FLUID, and the states of them (Fluid.FLUID_STATE_REGISTRY: what
// BlockState.getFluidState() answers, by id - 0 is the empty one). Source: mcgen gamedata.
namespace proto::gen::fluid {

"""]
    fl.append(f"inline constexpr unsigned kCount = {len(fluid_ids)};        // the fluids: the type of a scheduled fluid tick\n")
    for name in sorted(fluid_ids, key=fluid_ids.get):
        fl.append(f"inline constexpr unsigned char {ident(name)} = {fluid_ids[name]};\n")
    fl.append("// ...by name, as a saved tick names its fluid\ninline constexpr const char* kNames[kCount] = {" +
              ", ".join(f'"{name}"' for name in sorted(fluid_ids, key=fluid_ids.get)) + "};\n")
    fl.append(f"inline constexpr unsigned kStateCount = {len(fluid_states)};\n\n// kFlags[fluid state]\n")
    for name, why in FLUID_STATE_FLAGS:
        fl.append(f"inline constexpr unsigned char {name} = 0x{fsbit[name]:02X};  // {why}\n")
    fl.append("\n// Fluid.isSame: the family of a fluid, kNoFamily for the empty one.\n")
    for k, (name, source, flowing) in enumerate(FLUID_FAMILIES):
        fl.append(f"inline constexpr unsigned char {name} = {k};  // {source}, {flowing}\n")
    fl.append(f"inline constexpr unsigned char kNoFamily = 0xFF;\ninline constexpr unsigned kFamilies = {len(FLUID_FAMILIES)};\n")
    fl.append("inline constexpr unsigned char kFamily[kCount] = {" + ", ".join(
        str(family_of.get(name, "kNoFamily")) for name in sorted(fluid_ids, key=fluid_ids.get)) + "};\n\n")
    fl.append("// FluidState.getType(): the fluid, by fluid state.\ninline constexpr unsigned char kType[kStateCount] = {\n" +
              carray(fluid_type) + "};\n")
    fl.append("// FluidState.getAmount(): 1..8 of a flowing fluid, 8 of a source. Its own height is amount / 9.0f\n"
              "// (FlowingFluid.getOwnHeight).\ninline constexpr unsigned char kAmount[kStateCount] = {\n" + carray(fluid_amount) + "};\n")
    fl.append("inline constexpr unsigned char kFlags[kStateCount] = {\n" + carray(fluid_flags) + "};\n")
    fl.append("// FluidState.createLegacyBlock(): the block state that is this fluid and nothing else - what is\n"
              "// left of a block that held it (Level.removeBlock), what a flow puts where it arrives.\n"
              "inline constexpr unsigned short kLegacy[kStateCount] = {\n" + carray(fluid_legacy, 16) + "};\n")
    fl.append("// FlowingFluid.getFlowing(amount, falling) and getSource(falling), as fluid states:\n"
              "// kFlowingOf[family][falling][amount] (amount 1..8), kSourceOf[family][falling].\n"
              "inline constexpr unsigned char kFlowingOf[kFamilies][2][9] = {\n")
    for fam in flowing_of:
        fl.append("    {" + ", ".join("{" + ", ".join(str(x) for x in row) + "}" for row in fam) + "},\n")
    fl.append("};\ninline constexpr unsigned char kSourceOf[kFamilies][2] = {" + ", ".join(
        "{" + ", ".join(str(x) for x in fam) + "}" for fam in source_of) + "};\n\n}  // namespace proto::gen::fluid\n")

    o = [hdr, "".join(fl), """
// Per block state tables, indexed by state id. One byte (or two) per state and one table per
// question, so a scan over a section touches only the column it needs. Sources: Mojang's
// blocks.json for the ids, tools/mcgen/extract/Extract.java for the properties, the vanilla
// datapack for the tags.
namespace proto::gen::state {

"""]
    o.append(f"inline constexpr unsigned kCount = {n};\n")
    o.append(f"inline constexpr unsigned kBlockCount = {len(ext_blocks)};\n")
    o.append(f"inline constexpr unsigned kOcclusionTuples = {len(tuples)};\n")
    o.append(f"inline constexpr unsigned kCollisionShapes = {len(shapes)};\n")
    o.append("inline constexpr unsigned short kDynamicCollision = 0xFFFF;  // shape depends on position or entity\n")
    o.append(f"inline constexpr unsigned kFlowTuples = {len(flow_tuples)};\n")
    o.append(f"inline constexpr unsigned kFlowClasses = {len(class_ids)};\n\n")
    o.append("// kFlags[state]\n")
    for name, why in STATE_FLAGS:
        o.append(f"inline constexpr unsigned char {name} = 0x{bit[name]:02X};  // {why}\n")
    o.append("\n// kHooks[state]\n")
    for name, _, why in HOOK_FLAGS:
        o.append(f"inline constexpr unsigned char {name} = 0x{hbit[name]:02X};  // {why}\n")
    o.append("\n// kRandom[state]\n")
    for name, why in RANDOM_FLAGS:
        o.append(f"inline constexpr unsigned char {name} = 0x{rbit[name]:02X};  // {why}\n")
    o.append("\n// kConnect[state]\n")
    for name, why in CONNECT_FLAGS:
        o.append(f"inline constexpr unsigned char {name} = 0x{cbit[name]:02X};  // {why}\n")
    o.append("\n// kWallAbove[state]\n")
    for name, why in WALL_ABOVE_FLAGS:
        o.append(f"inline constexpr unsigned char {name} = 0x{wbit[name]:02X};{'  // ' + why if why else ''}\n")
    o.append("\n// kFluidFacts[state]\n")
    for name, why in FLUID_FACT_FLAGS:
        o.append(f"inline constexpr unsigned char {name} = 0x{ffbit[name]:02X};  // {why}\n")
    o.append("""
inline constexpr unsigned char kFlags[] = {
#embed "state_flags.bin"
};
inline constexpr unsigned char kHooks[] = {
#embed "state_hooks.bin"
};
inline constexpr unsigned char kRandom[] = {
#embed "state_random.bin"
};
// BlockState.isFaceSturdy(level, pos, direction, SupportType.FULL / CENTER): bit = direction. Asked
// of the state alone: a block whose support shape depends on where it is answers for nowhere.
// kSturdyCenter has two bits to spare, and they are: 0x40 BlockState.isSolid() (what a sign
// stands on or hangs against), 0x80 Block.isFaceFull(the collision shape, DOWN) (what a hanging
// sign hangs from with both chains).
inline constexpr unsigned char kSturdyFull[] = {
#embed "state_sturdy_full.bin"
};
inline constexpr unsigned char kSturdyCenter[] = {
#embed "state_sturdy_center.bin"
};
inline constexpr unsigned char kConnect[] = {
#embed "state_connect.bin"
};
inline constexpr unsigned char kWallAbove[] = {
#embed "state_wall_above.bin"
};
// What a plant may stand on (VegetationBlock.mayPlaceOn): a bit a rule - a tag of blocks, mostly -
// and BlockInfo::alt[0] of a plant is the bit of its own. Nothing here names a rule: the plant does.
inline constexpr unsigned char kSupport[] = {
#embed "state_support.bin"
};
// emission << 4 | dampening  (BlockState.getLightEmission() / getLightDampening())
inline constexpr unsigned char kLight[] = {
#embed "state_light.bin"
};
// Occlusion tuple: which of the 6 faces carry a light-occluding shape. 0 = none, which is every
// state for which LightEngine.isEmptyShape() holds.
inline constexpr unsigned char kOcclusion[] = {
#embed "state_occlusion.bin"
};
inline constexpr unsigned char kBlockBytes[] = {
#embed "state_block.bin"
};
inline constexpr unsigned char kCollisionBytes[] = {
#embed "state_collision.bin"
};
// The fluid state a block state holds (BlockState.getFluidState()): an index into fluid::k*.
inline constexpr unsigned char kFluid[] = {
#embed "state_fluid.bin"
};
inline constexpr unsigned char kFluidFacts[] = {
#embed "state_fluid_facts.bin"
};
// Which tuple of face layers the state's collision shape has: kFlowFace[kFlowFaces[state]].
inline constexpr unsigned char kFlowFaces[] = {
#embed "state_flow.bin"
};
static_assert(sizeof kFlags == kCount && sizeof kLight == kCount && sizeof kOcclusion == kCount);
static_assert(sizeof kHooks == kCount && sizeof kRandom == kCount && sizeof kSturdyFull == kCount && sizeof kSturdyCenter == kCount);
static_assert(sizeof kConnect == kCount && sizeof kWallAbove == kCount && sizeof kSupport == kCount);
static_assert(sizeof kBlockBytes == 2 * kCount && sizeof kCollisionBytes == 2 * kCount);
static_assert(sizeof kFluid == kCount && sizeof kFluidFacts == kCount && sizeof kFlowFaces == kCount);

constexpr unsigned u16_at(const unsigned char* t, unsigned i) { return t[2 * i] | static_cast<unsigned>(t[2 * i + 1]) << 8; }
constexpr unsigned block_of(unsigned state) { return u16_at(kBlockBytes, state); }
constexpr unsigned collision_of(unsigned state) { return u16_at(kCollisionBytes, state); }
// What Level.removeBlock() leaves: air, or the block of the fluid the state held (a waterlogged one's water).
constexpr unsigned fluid_legacy_of(unsigned state) { return fluid::kLegacy[kFluid[state]]; }

// Direction order of everything indexed by direction (net.minecraft.core.Direction ordinals).
enum Direction : unsigned char { kDown, kUp, kNorth, kSouth, kWest, kEast };

// LightEngine.shapeOccludes(from, to, direction) ==
//   kOccludes[kOcclusion[from]][direction] >> kOcclusion[to] & 1
// precomputed with Shapes.faceShapeOccludes() over every pair of face shapes.
inline constexpr unsigned long long kOccludes[kOcclusionTuples][6] = {
""")
    for row in matrix:
        o.append("    {" + ", ".join(f"0x{m:X}ull" for m in row) + "},\n")
    o.append("""};

// FlowingFluid.canPassThroughWall(direction, from, to) ==
//   !(kFlowCovers[kFlowFace[kFlowFaces[from]][direction]] >> kFlowFace[kFlowFaces[to]][direction ^ 1] & 1)
// precomputed with Shapes.mergedFaceOccludes() over every pair of collision shapes: the class of
// the layer of cells a shape has at each of its six faces, and which two classes cover a face
// between them.
inline constexpr unsigned char kFlowFace[kFlowTuples][6] = {
""")
    for row in flow_tuples:
        o.append("    {" + ", ".join(str(c) for c in row) + "},\n")
    o.append("};\ninline constexpr unsigned long long kFlowCovers[kFlowClasses] = {\n")
    for k in range(0, len(flow_covers), 4):
        o.append("    " + ", ".join(f"0x{m:X}ull" for m in flow_covers[k:k + 4]) + ",\n")
    o.append("};\n\n}  // namespace proto::gen::state\n")
    files["block_data.hpp"] = "".join(o)

    # ---- cold tables
    def item_default_components(name: str) -> dict:
        path = comp_dir / (name.split(":", 1)[1] + ".json")
        if not path.exists():
            die(f"no default components for {name} - run: mcgen.py reports")
        return json.loads(path.read_text())["components"]

    def sound(name: str) -> int:
        if name not in sound_ids:
            die(f"unknown sound event {name}")
        return sound_ids[name]

    entity_tags = vanilla.tags("entity_type")

    def entity_tag(tag: str, depth: int = 0) -> set[str]:
        """Members of an entity type tag, nested tags resolved."""
        tag = with_ns(tag)
        if depth > 8 or tag not in entity_tags:
            die(f"entity type tag #{tag} is missing or recursive")
        out: set[str] = set()
        for value in entity_tags[tag]:
            value = value["id"] if isinstance(value, dict) else value
            out |= entity_tag(value[1:], depth + 1) if value.startswith("#") else {with_ns(value)}
        return out

    info = [hdr, """
// Layout of the cold per-block / per-item tables (the tables themselves are in block_tables.hpp).
namespace proto::gen {

struct BlockInfo {
    unsigned short first_state, state_count, default_state;
    unsigned short item;        // item that places it, 0 (minecraft:air) = none
    unsigned short flags;       // kPlace* kind in the low bits, then the k* bits below
    unsigned short alt[3];      // states its placement kind chooses between (see kPlace*) - and then, by kind:
                                // kSpreading: [2] the soil it dies into
                                // kPlant, kSegmented: [0] the bit of state::kSupport the state under it must have
    unsigned short next_stage;  // WeatheringCopper.getNext() of a block that weathers: the first state of the block it
                                // becomes, with the properties it has (+ state - first_state); 0: it does not
    unsigned short place_sound, break_sound;  // sound_event registry ids
    float sound_volume, sound_pitch;
    float explosion_resistance; // Block.getExplosionResistance()
    float speed_factor;         // Block.getSpeedFactor(): what an entity in or on it keeps of its horizontal speed
    float bounce;               // Block.getBounceRestitution(): what an entity landing on it keeps of its fall
    // What an explosion without drop decay leaves of the block (its loot table for an empty tool):
    // drop[0] with probability drop_chance, else drop[1]; item 0 = nothing. Only for blocks that
    // can be in the world (placeable ones) - the others have none. A double slab leaves twice
    // the count (block_kind::kSlab: its table says so by state).
    struct Drop {
        unsigned short item;
        signed char min, max;    // the count is uniform in [min, max]; one below 1 is nothing
    } drop[2];
    float drop_chance;
    unsigned char kind;         // block_kind::*: the behaviour the server implements for its class
    unsigned char block_entity; // block_entity_type id of its block entity, kNoBlockEntity without one
    unsigned char weather;      // ChangeOverTimeBlock.getAge() of a WeatheringCopper block, ordinal + 1; 0: it is none
    unsigned short wet_stride;  // of a ported SimpleWaterloggedBlock: what its `waterlogged` adds to a state - a wet
                                // state plus this is the same block dry (the values are true, false); 0: it is none
};
struct ItemInfo {
    unsigned short block;       // block a BlockItem places, kNoBlock = not a block item
    unsigned short equip_sound; // sound_event id of the default EQUIPPABLE's equip sound (0 without one)
    unsigned char max_stack;    // default MAX_STACK_SIZE
    unsigned char flags;
    unsigned char equip_slot;   // EquipmentSlot ordinal of the default EQUIPPABLE, kNoEquipSlot without one
    unsigned char attack_swing; // default ATTACK_ANIMATION: SwingAnimationType id << kSwingTypeShift | ticks
    unsigned short fuel_ticks;  // default COOKING_FUEL burn time in a furnace, 0 = not a fuel
    unsigned short fuel_ticks_fast;  // ...and in a smoker or a blast furnace, where it also cooks kFastCookingSpeed times as fast
    unsigned short remainder;   // Item.getCraftingRemainder(): what crafting or burning it leaves, 0 = nothing
};
inline constexpr unsigned short kNoBlock = 0xFFFF;
inline constexpr unsigned char kNoBlockEntity = 0xFF;
inline constexpr unsigned char kNoEquipSlot = 0xFF;
inline constexpr float kFastCookingSpeed = %sf;  // AbstractFurnaceBlockEntity.getSpeedMultiplier of a fuel in a smoker or a blast furnace
inline constexpr unsigned kSwingTypeShift = 6;
inline constexpr unsigned char kSwingTicksMask = 0x3F;
struct Box {
    double min[3], max[3];      // x, y, z in block units
};

namespace block_flag {
""".replace("%sf", cfloat(gen_game.FAST_COOKING_SPEED) + "f")]
    for name, value, why in BLOCK_FLAGS:
        info.append(f"inline constexpr unsigned short {name} = 0x{value:X};  // {why}\n")
    info.append("}  // namespace block_flag\nnamespace item_flag {\n")
    for name, value, why in ITEM_FLAGS:
        info.append(f"inline constexpr unsigned char {name} = 0x{value:X};  // {why}\n")
    info.append("}  // namespace item_flag\n\n// BlockInfo::kind: block classes the server has a port of.\nnamespace block_kind {\n")
    for i, kind in enumerate(gen_game.BLOCK_KINDS):
        info.append(f"inline constexpr unsigned char {kind} = {i};\n")
    info.append("}  // namespace block_kind\n\n"
                "// Block state layouts the server computes states from:\n"
                "//   state = kFirst + sum over properties of (value index * stride)\n"
                "// with the values of each property in the order of its comment.\nnamespace props {\n")
    for name, layout in gen_game.STATE_LAYOUTS.items():
        gb = game["blocks"][name]
        have = [(p["name"], p["values"]) for p in gb["properties"]]
        if have != layout or gb["first_state"] != first[name]:
            die(f"{name}: its block state properties are {have}, the server's port expects {layout}")
        info.append(f"namespace {name.split(':', 1)[1]} {{\ninline constexpr unsigned kFirst = {gb['first_state']};\n")
        for p in gb["properties"]:
            info.append(f"inline constexpr unsigned k{p['name'].capitalize()}Stride = {p['stride']};  // {', '.join(p['values'])}\n")
        info.append(f"}}  // namespace {name.split(':', 1)[1]}\n")
    for family, (classes, layout) in gen_game.FAMILY_LAYOUTS.items():
        members = [b["name"] for b in ext_blocks if game["blocks"][b["name"]]["class"][0] in classes]
        if not members or {game["blocks"][m]["class"][0] for m in members} != classes:
            die(f"props::{family}: no block of one of {sorted(classes)}")
        for name in members:
            have = [(p["name"], p["values"]) for p in game["blocks"][name]["properties"]]
            if have != [(k, list(vals)) for k, vals in layout] or not ported_of[name]:
                die(f"{name}: its block state properties are {have}, the server's port of its family expects {layout}")
        strides = {(p["name"], p["stride"]) for m in members for p in game["blocks"][m]["properties"]}
        if len(strides) != len(layout):
            die(f"props::{family}: its blocks do not share one layout")
        info.append(f"namespace {family} {{  // {', '.join(sorted(classes))}\n")
        for p in game["blocks"][members[0]]["properties"]:
            pname = "".join(part.capitalize() for part in p["name"].split("_"))
            info.append(f"inline constexpr unsigned k{pname}Stride = {p['stride']};  // {', '.join(p['values'])}\n")
        info.append(f"}}  // namespace {family}\n")
    info.append("}  // namespace props\n\n}  // namespace proto::gen\n")
    files["block_info.hpp"] = "".join(info)

    t = [hdr, """
#include <string_view>

#include "block_info.hpp"

// Cold per-block / per-item tables. Large and rarely read: include from exactly one .cpp.
namespace proto::gen {

"""]

    bf = {name: value for name, value, _ in BLOCK_FLAGS}
    custom_clone = blocks_with_custom_clone(v["id"], {b["class"] for b in ext_blocks})
    kinds = [0] * (bf["kPlaceMask"] + 1)
    placeable_names: set[str] = set()
    loot = gen_game.Loot(vanilla, item_ids)
    be_ids = {n: e["protocol_id"] for n, e in static_regs["minecraft:block_entity_type"]["entries"].items()}
    signal_hooks = ("isSignalSource", "ownSignal", "getSignal", "getDirectSignal")
    drop_hooks = ("dropFromExplosion", "onExplosionHit", "getExplosionResistance", "getDrops", "getSpeedFactor",
                  "getBounceRestitution")
    default_states = {b["name"]: b["default_state"] for b in ext_blocks}

    def default_of(block_name: str) -> int:
        if block_name not in default_states:
            die(f"a block's fields name {block_name}, which is no block")
        return default_states[block_name]

    # StandingAndWallBlockItem: the standing block and the wall block of one item, by either's name.
    torch_pairs: dict[str, tuple[str, str]] = {}
    for it in game["items"].values():
        if it["class"][0] == "StandingAndWallBlockItem":
            standing, wall = it["fields"]["block"], it["fields"]["wallBlock"]
            pair_kind = (ported_of.get(standing) or (None,))[0]
            if pair_kind not in ("kTorch", "kSign"):
                continue
            want = "kWallTorch" if pair_kind == "kTorch" else "kWallSign"
            if it["fields"]["attachmentDirection"] != "DOWN" or (ported_of.get(wall) or (None,))[0] != want:
                die(f"{it['name']}: a torch or sign item that hangs, or whose wall block is not the ported wall block")
            torch_pairs[standing] = torch_pairs[wall] = (standing, wall)
        elif it["class"][0] == "HangingSignItem":   # ...and the two blocks of a hanging sign: under a ceiling, between walls
            ceiling, wall = it["fields"]["block"], it["fields"]["wallBlock"]
            if (it["fields"]["attachmentDirection"] != "UP" or (ported_of.get(ceiling) or (None,))[0] != "kHangingSign" or
                    (ported_of.get(wall) or (None,))[0] != "kWallHangingSign"):
                die(f"{it['name']}: a hanging sign item whose blocks are not the two ported hanging sign blocks")
            torch_pairs[ceiling] = torch_pairs[wall] = (ceiling, wall)
    weather_states = [c["name"] for c in game["enums"]["net.minecraft.world.level.block.WeatheringCopper$WeatherState"]]
    weathering_next = game["statics"]["net.minecraft.world.level.block.WeatheringCopper"]["NEXT_BY_BLOCK"]
    wax_off = game["statics"]["net.minecraft.world.item.HoneycombItem"]["WAX_OFF_BY_BLOCK"]
    copper_chests = tagged("copper_chests")
    t.append(f"inline constexpr BlockInfo kBlocks[{len(ext_blocks)}] = {{\n")
    for b in ext_blocks:
        name = b["name"]
        item = ext_items.get(b["item"])
        gb = game["blocks"][name]
        ported = ported_of[name]
        kind = placement_kind(b, item if b["item"] != "minecraft:air" else None, ported)
        kinds[kind] += 1
        if kind:
            placeable_names.add(name)
        # What the entity and explosion code reads off a block that can be in the world. A hook
        # that would answer otherwise than the tables below has to be ported first.
        drops: list = []
        if kind:
            cls = gb["class"][0]
            if any(h in gb["hooks"] for h in signal_hooks) and cls != "PoweredBlock":
                die(f"{name}: a placeable block with a redstone signal of its own ({cls}) - TNT would have to know")
            # (StairBlock.getExplosionResistance is its base block's: what blockdata.json has, and the
            # same as its own setting - checked below.)
            overridden = [h for h in drop_hooks if h in gb["hooks"] and (h, gb["hooks"][h]) != ("getExplosionResistance", "StairBlock")]
            if overridden and cls != "TntBlock":
                die(f"{name}: a placeable block that overrides {overridden}")
            # TntBlock.dropFromExplosion is false: it is primed instead (wasExploded).
            # SegmentableBlock: the property that counts its segments is the family's second (props::flower_bed).
            segments = gb["properties"][1]["name"] if (ported or (None,))[0] == "kSegmented" else None
            drops = [] if cls == "TntBlock" else loot.explosion_drops(name, gb.get("loot_table"), "SlabBlock" in gb["class"], segments)
            if ("SlabBlock" in gb["class"]) != ((ported or (None,))[0] == "kSlab"):
                die(f"{name}: SlabBlock and block_kind::kSlab are not the same blocks")
        drops += [(1.0, 0, 0, 0)] * (2 - len(drops))
        be = gb.get("block_entity_type")
        if (be is not None) != bool(b["entity_block"]) or (be is not None and be_ids[be] >= 0xFF):
            die(f"{name}: block entity type {be} against entity_block={b['entity_block']}")
        settings = gb["settings"]
        if settings["explosionResistance"] != b["explosion_resistance"]:
            die(f"{name}: blockdata.json and gamedata disagree on its explosion resistance")
        alt = [0, 0, 0]
        own_kind = ported[0] if ported else "kPlain"
        if kind == 2:
            alt = list(b["axis_states"])
        elif kind == bf["kPlaceSnowy"]:
            snowy = {s["properties"]["snowy"]: s["id"] for s in blocks[name]["states"]}
            if set(blocks[name]["properties"]) != {"snowy"}:
                die(f"{name}: a SnowyBlock with more properties than `snowy`")
            alt = [snowy["false"], snowy["true"], default_of(gb["fields"]["baseBlock"]) if own_kind == "kSpreading" else 0]
        elif kind in (bf["kPlaceTorch"], bf["kPlaceSign"], bf["kPlaceHangingSign"]):
            pair = torch_pairs.get(name)
            if pair is None:
                die(f"{name}: a torch or a sign that no StandingAndWallBlockItem places")
            alt = [default_of(pair[0]), first[pair[1]], 0]
        elif kind == bf["kPlaceConcretePowder"]:
            alt = [default_of(gb["fields"]["concrete"]), 0, 0]
        elif own_kind == "kChest":
            # ChestBlock.getOpenChestSound / getCloseChestSound, and for a copper chest
            # HoneycombItem.WAX_OFF_BY_BLOCK: what it is without its wax - withPropertiesOf is
            # the same offset from the first state (props::chest is one layout for all of them).
            copper = "CopperChestBlock" in gb["class"]
            if copper != (b["id"] in copper_chests) or (not copper and gb["class"][0] != "ChestBlock"):
                die(f"{name}: CopperChestBlock and #copper_chests are not the same blocks - chestCanConnectTo is read off the class")
            waxed = copper and "WeatheringCopper" not in gb["interfaces"]
            bare = wax_off.get(name)
            if waxed != (bare is not None) or (waxed and (
                    game["blocks"][bare]["class"][0] != "WeatheringCopperChestBlock" or
                    game["blocks"][bare]["fields"]["weatherState"] != gb["fields"]["weatherState"])):
                die(f"{name}: a waxed copper chest without an unwaxed one of its age, or the other way round")
            alt = [sound(gb["fields"]["openSound"]), sound(gb["fields"]["closeSound"]), first[bare] if waxed else 0]
        elif own_kind in ("kPlant", "kSegmented"):
            alt = [support_of[name], 0, 0]
        # ChangeOverTimeBlock: every block of the family counts when one of them weathers, ported or not.
        weather = 0
        if "WeatheringCopper" in gb["interfaces"]:
            age = gb["fields"].get("weatherState", gb["fields"].get("weatheringState"))  # (the golem statue's is its parent's)
            if age not in weather_states:
                die(f"{name}: a WeatheringCopper block without a weatherState of {weather_states}")
            weather = weather_states.index(age) + 1
        next_stage = 0
        if ported and gb["class"][0] in gen_game.WEATHERING_CLASSES:
            if not weather:
                die(f"{name}: a weathering block without an age")
            nxt = weathering_next.get(name)
            if any(st["randomly_ticking"][s["id"]] != (nxt is not None) for s in blocks[name]["states"]):
                die(f"{name}: isRandomlyTicking is not 'it has a next stage'")
            if nxt:
                # BlockState.withPropertiesOf: the same values of the same properties - the same offset.
                into = next(x for x in ext_blocks if x["name"] == nxt)
                if game["blocks"][nxt]["properties"] != gb["properties"] or not placement_kind(
                        into, ext_items.get(into["item"]), ported_of[nxt]):
                    die(f"{name}: weathers into {nxt}, which has other properties or cannot be in the world")
                next_stage = first[nxt]
        f = kind
        f |= bf["kSnowTag"] if b["id"] in snow else 0
        f |= bf["kInteractive"] if not (hook_default(b, "useItemOn") and hook_default(b, "useWithoutItem")) else 0
        f |= bf["kGameMaster"] if b["game_master"] else 0
        f |= bf["kCustomReplace"] if not hook_default(b, "canBeReplaced") else 0
        f |= bf["kCustomClone"] if b["class"] in custom_clone else 0
        # A ported class without an item: nothing places it, a command may set it.
        if ported and b["item"] == "minecraft:air":
            if gb["class"][0] not in gen_game.ITEMLESS_PORTED or b["game_master"]:
                die(f"{name}: its class is ported and it has no item - is it to be set by commands? (gen_game.ITEMLESS_PORTED)")
            f |= bf["kSettable"]
        snd = b["sound"]
        t.append(f"    {{{first[name]}, {len(blocks[name]['states'])}, {b['default_state']}, {item_ids[b['item']]}, 0x{f:X}, "
                 f"{{{alt[0]}, {alt[1]}, {alt[2]}}}, {next_stage}, {sound(snd['place'])}, {sound(snd['break'])}, "
                 f"{cfloat(snd['volume'])}f, {cfloat(snd['pitch'])}f, {cfloat(settings['explosionResistance'])}f, "
                 f"{cfloat(settings['speedFactor'])}f, {cfloat(settings['bounceRestitution'])}f, "
                 f"{{{{{drops[0][1]}, {drops[0][2]}, {drops[0][3]}}}, {{{drops[1][1]}, {drops[1][2]}, {drops[1][3]}}}}}, "
                 f"{cfloat(drops[0][0])}f, block_kind::{own_kind}, "
                 f"{be_ids[be] if be is not None else 'kNoBlockEntity'}, {weather}, {wet_stride.get(name, 0)}}},  "
                 f"// {name.split(':', 1)[1]}\n")
    t.append("};\n\n")

    itf = {name: value for name, value, _ in ITEM_FLAGS}
    damage_tags = vanilla.tags("damage_type")
    swing_types, swing_default_type, swing_default_ticks = swing_animations(v["id"])
    t.append(f"inline constexpr ItemInfo kItems[{len(item_ids)}] = {{\n")
    for name in sorted(item_ids, key=item_ids.get):
        it = ext_items[name]
        comps = item_default_components(name)
        tool = comps.get("minecraft:tool")
        f = itf["kPlainBlockItem"] if it["class"] == "BlockItem" else 0
        f |= itf["kStandingAndWall"] if it["class"] in ("StandingAndWallBlockItem", "HangingSignItem") else 0
        f |= itf["kDestroysInCreative"] if (tool is None or tool.get("can_destroy_blocks_in_creative", True)) else 0
        blk = block_ids[it["block"]] if "block" in it else 0xFFFF
        stack = comps.get("minecraft:max_stack_size", 1)
        if not 1 <= stack <= 99:
            die(f"{name}: max stack size {stack}")
        # What ItemStack.isDamageableItem() / isStackable() read. No vanilla item is unbreakable or
        # enchanted by default; the server's stack logic relies on that.
        f |= itf["kHasMaxDamage"] if "minecraft:max_damage" in comps else 0
        f |= itf["kHasDamage"] if "minecraft:damage" in comps else 0
        if "minecraft:unbreakable" in comps or comps.get("minecraft:damage", 0) != 0 or comps.get("minecraft:enchantments"):
            die(f"{name}: unbreakable, damaged or enchanted by default - item::ItemStack assumes no item is")
        if "minecraft:max_damage" in comps and stack != 1:
            die(f"{name}: damageable and stackable")
        equip_slot, equip_sound = 0xFF, 0
        eq = comps.get("minecraft:equippable")
        if eq is not None:
            equip_slot = EQUIPMENT_SLOTS.index(eq["slot"])
            equip_sound = sound(eq.get("equip_sound", "minecraft:item.armor.equip_generic"))  # Equippable.CODEC's default
            if "allowed_entities" in eq:
                allowed = eq["allowed_entities"]
                allowed = [allowed] if isinstance(allowed, str) else allowed
                if "minecraft:player" in allowed or any(a.startswith("#") and "minecraft:player" in entity_tag(a[1:]) for a in allowed):
                    die(f"{name}: players are among its allowed_entities - kEquipRestricted would be wrong")
                f |= itf["kEquipRestricted"]
        # ItemStack.canBeHurtBy(an explosion): DAMAGE_RESISTANT names a damage type tag.
        resistant = comps.get("minecraft:damage_resistant", {}).get("types")
        if resistant is not None:
            tag = damage_tags.get(with_ns(resistant.lstrip("#")))
            if not resistant.startswith("#") or tag is None:
                die(f"{name}: damage_resistant {resistant} is not a damage type tag of the jar")
            members = {v if isinstance(v, str) else v["id"] for v in tag}
            if any(m.startswith("#") for m in members):
                die(f"damage type tag {resistant} nests another tag: resolve it")
            hit = {"minecraft:explosion", "minecraft:player_explosion"} & members
            if len(hit) == 1:
                die(f"{name}: resists one kind of explosion and not the other")
            f |= itf["kExplosionProof"] if hit else 0
            # ...and canBeHurtBy(fire): ItemEntity.fireImmune asks about in_fire, Entity.baseTick
            # hurts with on_fire, Entity.lavaHurt with lava - one answer here, so one tag for all.
            fire = {"minecraft:in_fire", "minecraft:on_fire", "minecraft:lava"} & members
            if 0 < len(fire) < 3:
                die(f"{name}: resists {sorted(fire)} and not the rest of in_fire, on_fire and lava")
            f |= itf["kFireProof"] if fire else 0
        # ItemStack.getAttackAnimation(): the swing others see when the item's holder punches.
        # SwingAnimation.CODEC fills what the component leaves out from SwingAnimation.DEFAULT.
        anim = comps.get("minecraft:attack_animation", {})
        swing_type, swing_ticks = anim.get("type", swing_default_type), anim.get("duration", swing_default_ticks)
        if swing_type not in swing_types or swing_types[swing_type] > 3 or not 0 <= swing_ticks <= 0x3F:
            die(f"{name}: attack animation {anim} does not fit ItemInfo::attack_swing")
        # ItemStack.getInteractAnimation(): the swing of placing a block. The same for every item,
        # so the server keeps no table of it (Game::on_use_item_on).
        if comps.get("minecraft:interact_animation", {}) != {}:
            die(f"{name}: an interact animation of its own - the server swings with SwingAnimation.DEFAULT")
        remainder = game["items"][name].get("crafting_remainder")
        if remainder is not None and set(remainder) != {"id"}:
            die(f"{name}: a crafting remainder with a count or components: {remainder}")
        t.append(f"    {{{blk}, {equip_sound}, {stack}, 0x{f:X}, {equip_slot}, "
                 f"0x{swing_types[swing_type] << 6 | swing_ticks:02X}, {', '.join(map(str, gen_game.fuel_ticks(vanilla, comps, name)))}, "
                 f"{item_ids[remainder['id']] if remainder else 0}}},  // {name.split(':', 1)[1]}\n")
    t.append("};\n\n")

    # Collision boxes: kCollisionBoxes[kCollisionFirst[id] .. kCollisionFirst[id + 1])
    offsets = [0]
    for sh in shapes:
        offsets.append(offsets[-1] + len(sh))
    t.append(f"inline constexpr unsigned short kCollisionFirst[{len(offsets)}] = {{\n    ")
    t.append(", ".join(str(x) for x in offsets))
    t.append(f"\n}};\ninline constexpr Box kCollisionBoxes[{max(offsets[-1], 1)}] = {{\n")
    for sh in shapes:
        for b in sh:
            t.append("    {{" + ", ".join(cfloat(x) for x in b[:3]) + "}, {" + ", ".join(cfloat(x) for x in b[3:]) + "}},\n")
    t.append("};\n\n")
    t.append(f"inline constexpr std::string_view kBlockNames[{len(ext_blocks)}] = {{\n")
    t.extend(f'    "{b["name"]}",\n' for b in ext_blocks)
    t.append("};\n\n}  // namespace proto::gen\n")
    files["block_tables.hpp"] = "".join(t)

    # ---- items.hpp: ids by name (the test client and future commands address items by name)
    comp_ids = {name: e["protocol_id"] for name, e in static_regs["minecraft:data_component_type"]["entries"].items()}
    it = [hdr, "\n#include <string_view>\n\n// Item registry ids.\nnamespace proto::gen::item {\n\n"]
    seen: dict[str, str] = {}
    for name in sorted(item_ids, key=item_ids.get):
        cid = ident(name)
        if cid in seen or cid == "kCount":
            die(f"item identifier clash: {name} vs {seen.get(cid)}")
        seen[cid] = name
        it.append(f"inline constexpr unsigned {cid} = {item_ids[name]};\n")
    it.append(f"\ninline constexpr unsigned kCount = {len(item_ids)};\n\n}}  // namespace proto::gen::item\n\n"
              "// Data component type ids (the keys of an item stack's component patch).\nnamespace proto::gen::component {\n\n")
    for name in sorted(comp_ids, key=comp_ids.get):
        it.append(f"inline constexpr unsigned {ident(name)} = {comp_ids[name]};\n")
    it.append(f"\ninline constexpr unsigned kCount = {len(comp_ids)};\n\n"
              "// Their names: the keys of a patch as a tag, what `stone[...]` spells out.\n"
              "inline constexpr std::string_view kNames[kCount] = {\n")
    it.extend(f'    "{name}",\n' for name in sorted(comp_ids, key=comp_ids.get))
    it.append("};\n\n}  // namespace proto::gen::component\n")
    # Sizes of the static registries a component value can point into: an id past the end makes a
    # client's decoder throw (Registry.byIdOrThrow), so the server has to check before passing one on.
    it.append("\n// Entry counts of static registries that component values refer to by id.\n"
              "namespace proto::gen::static_registry {\n\n")
    for key in ("potion", "mob_effect", "sound_event", "block_entity_type"):
        it.append(f"inline constexpr unsigned {ident(key)}Count = {len(static_regs['minecraft:' + key]['entries'])};\n")
    # ...and the names of those a component's Codec names its entries by.
    for key in ("potion", "mob_effect"):
        entries = static_regs["minecraft:" + key]["entries"]
        it.append(f"\ninline constexpr std::string_view {ident(key)}Names[{len(entries)}] = {{\n")
        it.extend(f'    "{name}",\n' for name in sorted(entries, key=lambda n: entries[n]["protocol_id"]))
        it.append("};\n")
    for key in ("villager_type", "map_decoration_type", "sound_event"):
        entries = static_regs["minecraft:" + key]["entries"]
        if key != "sound_event":
            it.append(f"inline constexpr unsigned {ident(key)}Count = {len(entries)};\n")
        it.append(f"\ninline constexpr std::string_view {ident(key)}Names[{len(entries)}] = {{\n")
        it.extend(f'    "{name}",\n' for name in sorted(entries, key=lambda n: entries[n]["protocol_id"]))
        it.append("};\n")
    it.append("\n}  // namespace proto::gen::static_registry\n")
    # The enums a component holds one of: a name in a tag (StringRepresentable), an id in a packet
    # (ByteBufCodecs.idMapper over the enum's own id, which is not always its ordinal).
    it.append("\n// Enums that are component values: the name a tag spells, the id a packet carries.\n"
              "namespace proto::gen::variant {\n\nstruct Named {\n    std::string_view name;\n    unsigned id;\n};\n")
    for cid, cls, id_field in (("kFox", "animal.fox.Fox$Variant", "id"), ("kSalmon", "animal.fish.Salmon$Variant", "id"),
                               ("kParrot", "animal.parrot.Parrot$Variant", "id"),
                               ("kTropicalFishPattern", "animal.fish.TropicalFish$Pattern", "packedId"),
                               ("kMooshroom", "animal.cow.MushroomCow$Variant", "id"),
                               ("kRabbit", "animal.rabbit.Rabbit$Variant", "id"), ("kHorse", "animal.equine.Variant", "id"),
                               ("kLlama", "animal.equine.Llama$Variant", "id"), ("kAxolotl", "animal.axolotl.Axolotl$Variant", "id")):
        constants = game["enums"].get("net.minecraft.world.entity." + cls)
        if not constants or any("serialized" not in c or not isinstance(c.get(id_field), int) for c in constants):
            die(f"gamedata/enums.json: {cls} is not an enum of names and ids any more")
        it.append(f"inline constexpr Named {cid}[{len(constants)}] = {{" +
                  ", ".join(f'{{"{c["serialized"]}", {c[id_field]}}}' for c in constants) + "};\n")
    it.append("\n}  // namespace proto::gen::variant\n")
    files["items.hpp"] = "".join(it)

    # ---- item_defaults.bin: the components every item has of its own (Item.components()), each
    # as the tag its Codec reads - the report is that Codec's JSON. Per item, in id order:
    # [varint n] then n x ([varint component][varint length][a nameless tag]). The server reads
    # them at startup through the same code that reads a command's `stone[...]` (item/codec.cpp).
    defaults = bytearray()
    for name in sorted(item_ids, key=item_ids.get):
        comps = {} if name == "minecraft:air" and not (comp_dir / "air.json").exists() else item_default_components(name)
        defaults += varint(len(comps))
        for comp in sorted(comps, key=comp_ids.get):
            if comp not in comp_ids:
                die(f"{name}: a default component {comp} that is no data component type")
            tag = nbt_of_json(comps[comp])
            defaults += varint(comp_ids[comp]) + varint(len(tag)) + tag
    files["item_defaults.bin"] = bytes(defaults)

    entity_ids = {n.split(":", 1)[1]: e["protocol_id"] for n, e in static_regs["minecraft:entity_type"]["entries"].items()}
    physics = gen_physics.emit(hdr, v["id"], ext_blocks, st, shapes, data.get("collision_grids", []), game, tagged, placeable_names, entity_ids, cfloat32)
    physics_stats = physics.pop("_physics_stats")
    files.update(physics)
    files["_stats"] = {"placeable": sum(kinds[1:]), "blocks": len(ext_blocks), **physics_stats}
    return files


def emit_dimension_types(v: dict, regs: list[tuple[str, list[str]]], vanilla: "VanillaData") -> str:
    entries = dict(regs)["dimension_type"]
    out = [HEADER.format(id=v["id"], proto=v["protocol_version"]), """
// The parts of each dimension type the world storage is shaped by, in network id order
// (reg::dimension_type). From data/minecraft/dimension_type/*.json in the vanilla datapack.
namespace proto::gen::dimension {

struct Type {
    int min_y;          // lowest block y; a multiple of 16
    int height;         // blocks; a multiple of 16
    bool has_skylight;
    bool has_ceiling;
    bool fast_lava;         // EnvironmentAttributes.FAST_LAVA: lava flows as fast and as far as it does in the nether
    bool water_evaporates;  // EnvironmentAttributes.WATER_EVAPORATES: water that is poured here is gone
};

"""]
    for path in vanilla.zip.namelist():
        if path.startswith("data/minecraft/worldgen/biome/") and path.endswith(".json") and any(
                key in vanilla.zip.read(path) for key in (b"gameplay/fast_lava", b"gameplay/water_evaporates")):
            die(f"{path}: a biome says how lava flows or whether water stays - Type's two flags are a dimension's")
    out.append(f"inline constexpr Type kTypes[{len(entries)}] = {{\n")
    for name in entries:
        j = json.loads(vanilla.zip.read(f"data/minecraft/dimension_type/{name.split(':', 1)[1]}.json"))
        if j["min_y"] % 16 or j["height"] % 16:
            die(f"{name}: min_y/height are not multiples of 16")
        # What a dimension type says of an environment attribute is its value there (no biome
        # says anything of these two); what it does not say is the attribute's default, false.
        gameplay = [j.get("attributes", {}).get(f"minecraft:gameplay/{key}", False) for key in ("fast_lava", "water_evaporates")]
        if any(not isinstance(value, bool) for value in gameplay):
            die(f"{name}: fast_lava / water_evaporates is no plain boolean any more - read EnvironmentAttributes again")
        out.append(f"    {{{j['min_y']}, {j['height']}, {str(j['has_skylight']).lower()}, {str(j['has_ceiling']).lower()}, "
                   f"{str(gameplay[0]).lower()}, {str(gameplay[1]).lower()}}},  // {name}\n")
    out.append("};\n\n}  // namespace proto::gen::dimension\n")
    return "".join(out)


def emit_chat_types(v: dict, regs: list[tuple[str, list[str]]], vanilla: "VanillaData") -> str:
    """ChatType.chat of every chat type: the translation a message of that type is shown through,
    and what goes into it. A client decorates the messages it is sent by itself; a server needs
    this for what it prints (MinecraftServer.logChatMessage) and tells the console."""
    entries = dict(regs)["chat_type"]
    params = {"sender": "kSender", "target": "kTarget", "content": "kContent"}
    out = [HEADER.format(id=v["id"], proto=v["protocol_version"]), """
#include <string_view>

// How a chat message of each type reads, in network id order (reg::chat_type). From
// data/minecraft/chat_type/*.json in the vanilla datapack: ChatTypeDecoration.
namespace proto::gen::chat_type {

enum Param : unsigned char { kSender, kTarget, kContent };  // ChatTypeDecoration.Parameter

struct Decoration {
    std::string_view translation_key;
    unsigned char count;
    Param params[3];  // the translation's arguments, in order
};

"""]
    out.append(f"inline constexpr Decoration kChat[{len(entries)}] = {{\n")
    for name in entries:
        chat = json.loads(vanilla.zip.read(f"data/minecraft/chat_type/{name.split(':', 1)[1]}.json"))["chat"]
        given = chat["parameters"]
        if len(given) > 3 or any(p not in params for p in given):
            die(f"{name}: chat parameters {given} - ChatTypeDecoration.Parameter has changed")
        row = ", ".join(params[p] for p in given)
        out.append(f'    {{"{chat["translation_key"]}", {len(given)}, {{{row}}}}},  // {name}\n')
    out.append("};\n\n}  // namespace proto::gen::chat_type\n")
    return "".join(out)


def emit_timelines(v: dict, regs: list[tuple[str, list[str]]], vanilla: "VanillaData") -> str:
    """World clocks and the timelines that run on them: what `/time` names, and how dark the
    overworld's sky is at a clock's time (Level.updateSkyBrightness)."""
    by_path = dict(regs)
    clocks, timelines, dims = by_path["world_clock"], by_path["timeline"], by_path["dimension_type"]

    def read(kind: str, name: str) -> dict:
        return json.loads(vanilla.zip.read(f"data/minecraft/{kind}/{name.split(':', 1)[1]}.json"))

    out = [HEADER.format(id=v["id"], proto=v["protocol_version"]), """
#include <string_view>

// World clocks (reg::world_clock) and timelines (reg::timeline), in network id order. From
// data/minecraft/timeline/*.json and dimension_type/*.json in the vanilla datapack.
namespace proto::gen::timeline {

inline constexpr int kNoClock = -1;

struct Timeline {
    unsigned clock;  // the world clock it runs on
    int period;      // period_ticks; 0: it does not repeat
};

// ClockTimeMarker: a named moment of a timeline - of its clock, that is.
struct Marker {
    std::string_view name;
    unsigned clock;
    int ticks;
    int period;             // its timeline's; 0: none
    bool show_in_commands;  // offered by `/time set`
};

struct Keyframe {
    int ticks;
    float value;
};

"""]
    rows, markers, seen = [], [], set()
    for name in timelines:
        j = read("timeline", name)
        if with_ns(j["clock"]) not in clocks:
            die(f"{name}: clock {j['clock']} is no world clock")
        clock, period = clocks.index(with_ns(j["clock"])), j.get("period_ticks", 0)
        rows.append(f"    {{{clock}, {period}}},  // {name}\n")
        for marker, info in j.get("time_markers", {}).items():
            ticks, shown = (info, False) if isinstance(info, int) else (info["ticks"], info.get("show_in_commands", False))
            if (clock, marker) in seen:      # Timeline.validateRegistry
                die(f"{name}: time marker {marker} is defined twice for one clock")
            seen.add((clock, marker))
            markers.append(f'    {{"{with_ns(marker)}", {clock}, {ticks}, {period}, {str(shown).lower()}}},\n')
    out.append(f"inline constexpr Timeline kTimelines[{len(rows)}] = {{\n" + "".join(rows) + "};\n\n")
    out.append(f"inline constexpr Marker kMarkers[{len(markers)}] = {{\n" + "".join(markers) + "};\n\n")

    # DimensionType.defaultClock: the clock `/time` means in a dimension.
    out.append(f"// DimensionType.defaultClock, by reg::dimension_type.\ninline constexpr int kDefaultClock[{len(dims)}] = {{\n")
    for name in dims:
        clock = read("dimension_type", name).get("default_clock")
        out.append(f"    {clocks.index(with_ns(clock)) if clock else 'kNoClock'},  // {name}\n")
    out.append("};\n\n")

    # gameplay/sky_light_level in the overworld: the attribute's own default, times what the
    # timelines of the dimension make of it. The server computes Level.skyDarken from it, so
    # everything about it that is data is checked to be what the C++ expects.
    attribute = "minecraft:gameplay/sky_light_level"
    overworld = read("dimension_type", "minecraft:overworld")
    if attribute in overworld.get("attributes", {}):
        die(f"the overworld sets {attribute} itself now: teach emit_timelines and server/clock.cpp")

    def tagged(tag: str) -> list[str]:
        values = json.loads(vanilla.zip.read(f"data/minecraft/tags/timeline/{tag.split(':', 1)[1]}.json"))["values"]
        return [n for v_ in values for n in (tagged(with_ns(v_[1:])) if v_.startswith("#") else [with_ns(v_)])]

    which = overworld["timelines"]
    in_overworld = tagged(with_ns(which[1:])) if which.startswith("#") else [with_ns(which)]
    having = [n for n in in_overworld if attribute in read("timeline", n).get("tracks", {})]
    if len(having) != 1:
        die(f"{attribute}: tracks in {having}, expected exactly one timeline of the overworld's")
    track = read("timeline", having[0])["tracks"][attribute]
    if track.get("modifier") != "multiply" or track.get("ease", "linear") != "linear" or len(track["keyframes"]) < 2:
        die(f"{attribute} in {having[0]}: {track} - no longer a linear track that multiplies")
    if not read("timeline", having[0]).get("period_ticks"):
        die(f"{having[0]} has no period")
    keys = track["keyframes"]
    out.append(f"// {attribute} of {having[0]}: a factor (modifier \"multiply\"), linear between keyframes.\n"
               f"inline constexpr unsigned kSkyLightTimeline = {timelines.index(having[0])};\n"
               f"inline constexpr Keyframe kSkyLight[{len(keys)}] = {{\n")
    out.extend(f"    {{{k['ticks']}, {k['value']!r}f}},\n" for k in keys)
    out.append("};\n\n}  // namespace proto::gen::timeline\n")
    return "".join(out)


# ----------------------------------------------------------------------------- entities

def pseudo_of(version: str, classes: list[str], method_rx: str | None) -> str:
    """Pseudo-code of some methods (tools/mcgen/pseudo.py); static, like javap()."""
    import subprocess

    import pseudo
    tool = mcgen.find_tool("javap", version_info(version)["java_version"])
    res = subprocess.run([tool, "-v", "-p", "-c", "-l", "-constants", "-cp", str(jar_for(version, client=False)), *classes],
                         capture_output=True, text=True)
    if res.returncode != 0:
        die(f"javap failed: {res.stderr.strip()}")
    return pseudo.pseudo(res.stdout, method_rx)


def class_parents(version: str, classes: set[str]) -> dict[str, str]:
    """{class: superclass} for `classes` and everything above them, from javap class headers."""
    import subprocess
    tool = mcgen.find_tool("javap", version_info(version)["java_version"])
    parents: dict[str, str] = {}
    todo = set(classes)
    while todo:
        res = subprocess.run([tool, "-p", "-cp", str(jar_for(version, client=False)), *sorted(todo)], capture_output=True, text=True)
        if res.returncode != 0:
            die(f"javap failed: {res.stderr.strip()}")
        for m in re.finditer(r"^(?:public |abstract |final )*class ([\w.$]+)(?:<.*?>)?(?: extends ([\w.$]+))?", res.stdout, re.M):
            parents[m.group(1)] = m.group(2) or ""  # no extends clause: a root (java.lang.Object)
        missing = todo - set(parents)
        if missing:
            die(f"no class header found for {sorted(missing)[:3]}")
        todo = {p for p in parents.values() if p.startswith("net.minecraft.") and p not in parents}
    return parents


def f32(x: float) -> float:
    import struct
    return struct.unpack("<f", struct.pack("<f", x))[0]


def cfloat32(x: float) -> str:
    """Shortest float literal that round-trips to the same 32-bit value."""
    for digits in range(1, 10):
        r = f"{x:.{digits}g}"
        if f32(float(r)) == f32(x):
            return (r if ("." in r or "e" in r) else r + ".0") + "f"
    die(f"cannot print {x!r} as a float")


# Classes whose synchronized data the server writes itself, root first. Ids continue down the
# hierarchy (ClassTreeIdRegistry), so every class on the way to a leaf has to be listed.
ENTITY_DATA_CLASSES = [
    ("entity", "net.minecraft.world.entity.Entity", None),
    ("living_entity", "net.minecraft.world.entity.LivingEntity", "entity"),
    ("avatar", "net.minecraft.world.entity.Avatar", "living_entity"),
    ("player", "net.minecraft.world.entity.player.Player", "avatar"),
    ("item_entity", "net.minecraft.world.entity.item.ItemEntity", "entity"),
    ("primed_tnt", "net.minecraft.world.entity.item.PrimedTnt", "entity"),
    ("falling_block", "net.minecraft.world.entity.item.FallingBlockEntity", "entity"),
    ("mob", "net.minecraft.world.entity.Mob", "living_entity"),
    ("ageable_mob", "net.minecraft.world.entity.AgeableMob", "mob"),
    ("armor_stand", "net.minecraft.world.entity.decoration.ArmorStand", "living_entity"),
]
LIVING = "net.minecraft.world.entity.LivingEntity"


def emit_entities(v: dict, version: str, static_regs: dict, d: Path) -> tuple[str, int]:
    ids = {n.split(":", 1)[1]: e["protocol_id"] for n, e in static_regs["minecraft:entity_type"]["entries"].items()}
    if sorted(ids.values()) != list(range(len(ids))):
        die("entity_type ids are not dense")

    # EntityTypeIds: FIELD = create("name")
    keys = dict(re.findall(r"^\s+(\w+) = create\(\"([^\"]+)\"\);$",
                           pseudo_of(version, ["net.minecraft.world.entity.EntityTypeIds"], r"static \{\}"), re.M))
    # EntityTypes: the entity class of each field (for "is it a LivingEntity"), then its builder chain.
    decl = dict((f, c) for c, f in re.findall(
        r"^\s+public static final net\.minecraft\.world\.entity\.EntityType<([\w.$]+)(?:<.*?>)?> (\w+);$",
        javap(version, ["net.minecraft.world.entity.EntityTypes"]), re.M))
    parents = class_parents(version, set(decl.values()))

    def is_living(cls: str) -> bool:
        while cls in parents:
            if cls == LIVING:
                return True
            cls = parents[cls]
        return cls == LIVING

    def num(s: str) -> float:
        return float(s.rstrip("fd"))

    rows: dict[int, tuple] = {}
    text = pseudo_of(version, ["net.minecraft.world.entity.EntityTypes"], r"static \{\}")
    for field, chain in re.findall(r"^\s+(\w+) = register\(EntityTypeIds\.\w+, EntityType\$Builder\.(.*)\);$", text, re.M):
        name = keys.get(field)
        if name not in ids or field not in decl:
            die(f"EntityTypes.{field}: no registry id / declaration")
        m = re.search(r"\.sized\(([-\d.e]+f?), ([-\d.e]+f?)\)", chain)
        if not m:
            die(f"EntityTypes.{field}: no sized()")
        width, height = f32(num(m.group(1))), f32(num(m.group(2)))
        m = re.search(r"\.eyeHeight\(([-\d.e]+f?)\)", chain)
        eye = f32(num(m.group(1))) if m else f32(height * f32(0.85))  # EntityDimensions.defaultEyeHeight
        m = re.search(r"\.clientTrackingRange\((\d+)\)", chain)
        tracking = int(m.group(1)) if m else 5                         # EntityType.Builder defaults
        m = re.search(r"\.updateInterval\((\d+)\)", chain)
        interval = 0 if ".noUpdateInterval()" in chain else int(m.group(1)) if m else 3
        if interval == 2147483647:  # EntityType.hasUpdateInterval(): Integer.MAX_VALUE means never
            interval = 0
        if not (0 <= tracking <= 255 and 0 <= interval <= 255):
            die(f"EntityTypes.{field}: tracking range / update interval out of range")
        rows[ids[name]] = (name, width, height, eye, tracking, interval, ".dontTrackDeltas()" not in chain, is_living(decl[field]),
                           decl[field])
    if sorted(rows) != list(range(len(ids))):
        die(f"EntityTypes: parsed {len(rows)} of {len(ids)} entity types - the static initializer changed shape")

    # What a living entity saves beyond its own state (Entity.saveWithoutId and below): which of
    # Mob's and AgeableMob's entries it has, and the attributes a fresh one has touched.
    path = d / "gamedata" / "entities.json"
    if not path.exists():
        die("gamedata/entities.json is missing - run: mcgen.py gamedata")
    game = {e["name"]: e for e in json.loads(path.read_text())["types"]}
    MOB, AGEABLE = "net.minecraft.world.entity.Mob", "net.minecraft.world.entity.AgeableMob"

    def chain(cls: str) -> list[str]:
        up = [cls]
        while up[-1] in parents:
            up.append(parents[up[-1]])
        return up

    # AgeableMob.canBeABaby: true, but for the classes that say otherwise - they save no age.
    below_ageable = sorted({c for r in rows.values() if AGEABLE in chain(r[8]) for c in chain(r[8])[:chain(r[8]).index(AGEABLE)]})
    no_baby = set()
    for cls in below_ageable:
        body = pseudo_of(version, [cls], r"canBeABaby\(")
        if "canBeABaby" not in body:
            continue
        if not re.search(r"canBeABaby\(\) \{\s+return 0;\s+\}", body):
            die(f"{cls}.canBeABaby: not `return false` - read it, and teach emit_entities what it means")
        no_baby.add(cls)
    saved = {}
    for i, r in rows.items():
        up, attrs = chain(r[8]), game.get("minecraft:" + r[0], {}).get("attributes") or {}
        if r[7] and "minecraft:max_health" not in attrs:
            die(f"gamedata/entities.json: {r[0]} is a LivingEntity without attributes - run: mcgen.py gamedata")
        saved[i] = (MOB in up, AGEABLE in up and not (no_baby & set(up)), attrs.get("minecraft:max_health", 0.0),
                    attrs.get("minecraft:movement_speed", 0.0), attrs.get("minecraft:follow_range", 0.0))

    out = [HEADER.format(id=v["id"], proto=v["protocol_version"]), """
#include <string_view>

// Entity types: network ids (minecraft:entity_type, from Mojang's registries.json) and what the
// entity tracker needs to know about each, read from EntityTypes.<clinit> in the jar.
namespace proto::gen::entity_type {

"""]
    for i in range(len(rows)):
        out.append(f"inline constexpr unsigned {ident(rows[i][0])} = {i};\n")
    out.append(f"inline constexpr unsigned kCount = {len(rows)};\n")
    out.append("""
struct Info {
    float width, height, eye_height;  // EntityDimensions of the default pose
    unsigned char tracking_range;     // clientTrackingRange, in chunks
    unsigned char update_interval;    // ticks between movement updates; 0 = never (noUpdateInterval)
    bool track_deltas;                // velocity changes are sent with movement
    bool living;                      // extends LivingEntity: movement is sent as stepped paths
    bool mob;                         // extends Mob
    bool ageable;                     // extends AgeableMob and can be a baby (AgeableMob.canBeABaby): it saves an age
    float max_health;                 // the default of its max_health attribute: what a fresh one's Health is
    double movement_speed;            // ...of movement_speed
    double follow_range;              // ...of follow_range (a Mob's)
};

inline constexpr Info kInfo[kCount] = {
""")
    for i in range(len(rows)):
        name, width, height, eye, tracking, interval, deltas, living, _ = rows[i]
        mob, ageable, health, speed, follow = saved[i]
        out.append(f"    {{{cfloat32(width)}, {cfloat32(height)}, {cfloat32(eye)}, {tracking}, {interval}, {str(deltas).lower()}, "
                   f"{str(living).lower()}, {str(mob).lower()}, {str(ageable).lower()}, {cfloat32(health)}, {speed!r}, {follow!r}}},  // {name}\n")
    out.append("};\n\ninline constexpr std::string_view kNames[kCount] = {\n")
    out.extend(f'    "minecraft:{rows[i][0]}",\n' for i in range(len(rows)))
    out.append("};\n\n}  // namespace proto::gen::entity_type\n")

    # ---- synchronized entity data
    order = re.findall(r"^\s+registerSerializer\((\w+)\);$",
                       pseudo_of(version, ["net.minecraft.network.syncher.EntityDataSerializers"], r"static \{\}"), re.M)
    if len(order) < 20 or len(set(order)) != len(order):
        die("EntityDataSerializers: could not read the registration order")
    out.append("""
// SynchedEntityData as set_entity_data carries it: [u8 accessor id][VarInt serializer id][value]...
// then 0xFF. Serializer ids are the registration order in EntityDataSerializers.<clinit>; accessor
// ids are the order of SynchedEntityData.defineId calls, continuing down the class hierarchy.
namespace proto::gen::entity_data {

namespace serializer {
""")
    for i, name in enumerate(order):
        out.append(f"inline constexpr unsigned {ident(name.lower())} = {i};\n")
    out.append("}  // namespace serializer\n")
    counts: dict[str, int] = {}
    for ns, cls, parent in ENTITY_DATA_CLASSES:
        defs = re.findall(r"^\s+(\w+) = SynchedEntityData\.defineId\((\w+)\.class, EntityDataSerializers\.(\w+)\);$",
                          pseudo_of(version, [cls], r"static \{\}"), re.M)
        if not defs or any(owner != cls.rsplit(".", 1)[1] for _, owner, _ in defs):
            die(f"{cls}: could not read its SynchedEntityData.defineId calls")
        first = counts[parent] if parent else 0
        out.append(f"\nnamespace {ns} {{  // {cls.rsplit('.', 1)[1]}\n")
        for i, (field, _, ser) in enumerate(defs):
            short = re.sub(r"^DATA_|_ID$", "", field)
            out.append(f"inline constexpr unsigned {ident(short.lower())} = {first + i};  // {ser}\n")
        out.append(f"}}  // namespace {ns}\n")
        counts[ns] = first + len(defs)
    out.append("\n}  // namespace proto::gen::entity_data\n")

    # EquipmentSlot: NAME = new EquipmentSlot("NAME", ordinal, type, index, [count limit,] id, "name")
    slots = re.findall(r"^\s+\w+ = new EquipmentSlot\(\"\w+\", (\d+), EquipmentSlot\$Type\.(\w+), .*\"(\w+)\"\);$",
                       pseudo_of(version, ["net.minecraft.world.entity.EquipmentSlot"], r"static \{\}"), re.M)
    if [name for _, _, name in slots] != EQUIPMENT_SLOTS or [int(o) for o, _, _ in slots] != list(range(len(slots))):
        die(f"EquipmentSlot changed: {slots} - update EQUIPMENT_SLOTS and everything that indexes by slot")
    out.append("\n// net.minecraft.world.entity.EquipmentSlot ordinals: the slot byte of set_equipment, and the\n"
               "// index of everything kept per equipment slot.\nnamespace proto::gen::equipment_slot {\n")
    for ordinal, kind, name in slots:
        out.append(f"inline constexpr unsigned {ident(name)} = {ordinal};  // {kind}\n")
    out.append(f"inline constexpr unsigned kCount = {len(slots)};\n}}  // namespace proto::gen::equipment_slot\n")

    swing_types, swing_default_type, swing_default_ticks = swing_animations(version)
    out.append("\n// How an arm swing is described (swing_animation, an item's attack_animation component):\n"
               "// net.minecraft.world.item.SwingAnimationType network ids, and SwingAnimation.DEFAULT.\n"
               "namespace proto::gen::swing_animation {\n")
    for name, sid in sorted(swing_types.items(), key=lambda kv: kv[1]):
        out.append(f"inline constexpr unsigned {ident(name)} = {sid};\n")
    out.append(f"inline constexpr unsigned kDefaultType = {ident(swing_default_type)};\n"
               f"inline constexpr unsigned kDefaultTicks = {swing_default_ticks};\n"
               "}  // namespace proto::gen::swing_animation\n")

    poses = re.findall(r"^\s+(\w+) = new Pose\(\"\w+\", \d+, (\d+), \"\w+\"\);$",
                       pseudo_of(version, ["net.minecraft.world.entity.Pose"], r"static \{\}"), re.M)
    if len(poses) < 8:
        die("Pose: could not read the enum")
    out.append("\n// net.minecraft.world.entity.Pose network ids (Pose.STREAM_CODEC).\nnamespace proto::gen::pose {\n")
    for name, pid in poses:
        out.append(f"inline constexpr unsigned {ident(name.lower())} = {pid};\n")
    out.append("}  // namespace proto::gen::pose\n")
    return "".join(out), len(rows)


# ----------------------------------------------------------------------------- driver

def put(path: Path, content: str | bytes) -> bool:
    """Write a generated file unless it already holds exactly this; tells whether it wrote.

    A build goes by modification times, and what current.hpp includes reaches three objects in four: a file
    rewritten with what it already had recompiles nearly everything, once per preset.
    """
    data = content.encode("utf-8") if isinstance(content, str) else content
    if path.is_file() and path.read_bytes() == data:
        return False
    path.write_bytes(data)
    return True


def generate_into(version: str, out: Path, reports: Path | None = None) -> dict:
    """`reports` overrides where registries.json / blocks.json are read from (tests only)."""
    v = version_info(version)
    d = vdir(version)
    packets, packets_src = load_packets(version)
    reports = reports or d / "reports"
    reg_report = reports / "registries.json"
    blk_report = reports / "blocks.json"
    comp_dir = reports / "item_components"
    ext_report = d / "extract" / "blockdata.json"
    if not (reg_report.exists() and blk_report.exists() and comp_dir.is_dir()):
        die("Mojang's reports are missing (registries.json, blocks.json, item_components/).\\n"
            "       They give the static registry ids: without them there are no tags and no blocks.\\n"
            "       Run `mcgen.py reports` - it executes Mojang's data generator, ask the user first.")
    if not ext_report.exists():
        die("extract/blockdata.json is missing (per-state light, fluid and shape properties).\\n"
            "       Run `mcgen.py blockdata` - it executes the jar like `reports` does, ask the user first.")

    data = VanillaData(version)
    regs = [(p, data.entries(p)) for p in synced_registries(version)]
    empty = [p for p, entries in regs if not entries]
    if empty:  # legal, but far more often a data directory that is not named after its registry key
        print(f"mcgen: WARNING: synchronized registries with no vanilla entries: {', '.join(empty)}", file=sys.stderr)

    out.mkdir(parents=True, exist_ok=True)
    saved: dict[str, bool] = {}  # file name -> did its content change

    def save(name: str, content: str | bytes) -> None:
        saved[name] = put(out / name, content)

    save("meta.hpp", emit_meta(v))
    save("packet_ids.hpp", emit_packet_ids(v, packets))
    save("registries.hpp", emit_registries(v, regs))
    save("registry_data.bin", registry_data_blob(packets, regs))
    save("blobs.hpp", emit_blobs(v))
    save("dimension_types.hpp", emit_dimension_types(v, regs, data))
    save("chat_types.hpp", emit_chat_types(v, regs, data))
    save("timelines.hpp", emit_timelines(v, regs, data))

    tag_tables: list[tuple[str, dict[str, list[int]]]] = []
    stats = {"registries": len(regs), "entries": sum(len(e) for _, e in regs), "tags": 0, "packets": packets_src}
    static_regs = json.loads(reg_report.read_text())
    entities, stats["entity_types"] = emit_entities(v, version, static_regs, d)
    save("entities.hpp", entities)
    synced = {p: {e: i for i, e in enumerate(entries)} for p, entries in regs}
    for path in data.tag_dirs():
        key = f"minecraft:{path}"
        if key in static_regs:
            ids = {n: e["protocol_id"] for n, e in static_regs[key]["entries"].items()}
        elif path in synced:
            ids = synced[path]
        else:
            continue  # not a network-visible registry (worldgen/structure, villager_trade, ...)
        flat = flatten_tags(data.tags(path), ids, key)
        if flat:
            tag_tables.append((key, flat))
            stats["tags"] += len(flat)
    tag_tables.sort()
    save("tags.bin", tags_blob(packets, tag_tables))

    blocks = json.loads(blk_report.read_text())
    save("blocks.hpp", emit_blocks(v, blocks))
    game = gen_game.load_gamedata(d)
    tables = build_block_tables(v, blocks, json.loads(ext_report.read_text()), static_regs, data, comp_dir, game)
    stats.update(tables.pop("_stats"))
    hdr = HEADER.format(id=v["id"], proto=v["protocol_version"])
    item_ids = {n: e["protocol_id"] for n, e in static_regs["minecraft:item"]["entries"].items()}
    comp_ids = {n: e["protocol_id"] for n, e in static_regs["minecraft:data_component_type"]["entries"].items()}
    recipes = gen_game.Recipes(v, data, static_regs, flatten_tags(data.tags("item"), item_ids, "minecraft:item"), comp_ids)
    tables.update(recipes.emit(hdr, packets))
    stats.update(tables.pop("_stats"))
    tables["static_ids.hpp"] = gen_game.emit_static_ids(hdr, static_regs)
    tables["command_data.hpp"] = gen_commands.emit_command_data(hdr, d, static_regs, blocks)
    tables["lang.hpp"] = gen_commands.emit_lang(hdr, version)
    for name, content in tables.items():
        save(name, content)
    stats["files"] = len(saved)
    stats["names"] = sorted(saved)
    stats["changed"] = sorted(name for name, changed in saved.items() if changed)
    return stats


GEN_ROOT = ROOT / "src" / "proto" / "gen"


def out_dir(version: str) -> Path:
    return GEN_ROOT / f"v{version_info(version)['protocol_version']}"


def current_dir() -> Path:
    """The directory current.hpp names: what the build includes, known without the cache."""
    m = re.search(r'#define PROTO_GEN_DIR "proto/gen/([^"/]+)"', (GEN_ROOT / "current.hpp").read_text())
    if not m:
        die("src/proto/gen/current.hpp does not define PROTO_GEN_DIR")
    return GEN_ROOT / m.group(1)


# ----------------------------------------------------------------------------- the lock
#
# The generated directory is not in git: it is Mojang's data, and everybody makes it from the jar.
# What is in git is its lock - src/proto/gen/v<protocol>.sha256, in sha256sum's format - which
# says what the sources were written against. `generate` writes it, so a change of the generator
# shows as a diff; `setup` and the build compare with it, so nobody compiles against other tables.

def lock_path(out: Path) -> Path:
    return out.with_name(out.name + ".sha256")


def lock_of(out: Path, names: list[str]) -> str:
    return "".join(f"{hashlib.sha256((out / n).read_bytes()).hexdigest()}  {n}\n" for n in sorted(names))


def lock_problems(out: Path) -> list[str] | None:
    """What of `out` differs from its lock, by file name; None when there is no lock."""
    lock = lock_path(out)
    if not lock.is_file():
        return None
    bad = []
    for line in lock.read_text().splitlines():
        digest, name = line.split("  ", 1)
        path = out / name
        if not path.is_file():
            bad.append(f"{name} (missing)")
        elif hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            bad.append(name)
    return bad


def print_stats(out: Path, stats: dict) -> None:
    print(f"mcgen: generated {out.relative_to(ROOT)}: {stats['registries']} registries, {stats['entries']} entries, "
          f"{stats['tags']} tags, {stats['blocks']} blocks ({stats['placeable']} placeable), {stats['entity_types']} entity types, "
          f"{stats['crafting']} crafting and {stats['cooking']} cooking recipes ({stats['recipes_skipped']} of other kinds left out), "
          f"{stats['materials']} block materials, {stats['shapes_grid']} collision grids, "
          f"{stats['movers']} entity types that move, packet ids from {stats['packets']}")
    changed = stats["changed"]  # the rest was left as it was, so nothing that includes it is rebuilt
    print(f"mcgen: {len(changed)} of {stats['files']} files changed" + (f": {', '.join(changed)}" if changed else ""))


def cmd_generate(args) -> None:
    out = out_dir(args.version)
    stats = generate_into(args.version, out)
    print_stats(out, stats)
    lock = lock_path(out)
    if put(lock, lock_of(out, stats["names"])):
        print(f"mcgen: {lock.relative_to(ROOT)} changed: the tables are not what they were - commit it with what changed them")


def verify_lock(out: Path) -> None:
    """Stops unless `out` is what its lock says. Reads nothing but the two: no cache, no Java."""
    lock = lock_path(out)
    if not out.is_dir():
        die(f"{out.relative_to(ROOT)}/ is not there: it is generated from Mojang's jar, not kept in git.\n"
            "       Run ./install.sh (everything, once) or: python3 tools/mcgen/mcgen.py setup")
    bad = lock_problems(out)
    if bad is None:
        die(f"{lock.relative_to(ROOT)} is missing: `mcgen.py generate` writes it, and it belongs in git")
    if bad:
        die(f"{out.relative_to(ROOT)}/ is not what {lock.name} says ({', '.join(bad)}).\n"
            "       After a pull, or with nothing of tools/mcgen changed: python3 tools/mcgen/mcgen.py setup\n"
            "       After a change of the generator: python3 tools/mcgen/mcgen.py generate, and commit the lock.\n"
            "       If `setup` itself ends here, this machine generates other bytes than the lock's author:\n"
            "       please report it with your Java and Python versions (-DMC_GENERATED_CHECK=OFF builds anyway).")


def cmd_check(args) -> None:
    out = current_dir()
    verify_lock(out)
    if not args.quiet:
        print(f"mcgen: {out.relative_to(ROOT)}/ is what {lock_path(out).name} says")


def cmd_setup(args) -> None:
    """From nothing to the generated tables: every step that is not already done, then the lock."""
    args.client, args.sections = False, []
    mcgen.cmd_fetch(args)
    for step, run in (("reports", mcgen.cmd_reports), ("blockdata", mcgen.cmd_blockdata), ("gamedata", gamedata.cmd_gamedata)):
        if mcgen.stamp_is_current(args.version, step):
            print(f"mcgen: {step}: already there")
        else:
            run(args)
    out = out_dir(args.version)
    print_stats(out, generate_into(args.version, out))
    if out != current_dir():
        print(f"mcgen: note: the build includes {current_dir().relative_to(ROOT)}/ (src/proto/gen/current.hpp), not this")
    verify_lock(out)
    print(f"mcgen: {out.relative_to(ROOT)}/ is what {lock_path(out).name} says - ready to build")


_ = mcgen  # imported for its side-effect-free helpers; keeps linters quiet about the module import
