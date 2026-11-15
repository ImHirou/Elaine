#!/usr/bin/env python3
"""What does a server answer to commands? Record it, on vanilla and on ours, and diff.

    command_probe.py record <port> <out.json> [nameA nameB [console]]
    command_probe.py diff <vanilla.json> <ours.json> [-v]

`record` joins two bots (tests/integration/mcproto.py, offline mode, creative). A has to be an
operator and B must not be: once both are in, type `op <nameA>` on the server's console when
the probe asks - or name a FIFO that is the server's stdin as `console`, and the probe types
it itself (and `deop` at the end). Not before A has joined: a vanilla server asked to op a name
it has not seen looks it up at Mojang, and ops the account of that name instead of the bot.
A types commands, right ones and wrong ones; B watches, is given things, teleported, opped,
deopped and in the end kicked. Per step everything either of them is told is logged: system
messages (their text components, byte for byte), block changes, teleports, sounds, entities
that appear, the command tree. `diff` lines two recordings up step by step.

The reference is the vanilla server, run under the rules in AGENTS.md (gamemode=creative,
level-type=flat, spawn-protection=0):

    cd .cache/vanilla-server && /usr/lib/jvm/java-25-openjdk/bin/java -jar ../mc/26.3/server.jar --nogui
    tools/vanilla/command_probe.py record 25577 /tmp/vanilla.json Eve Fay       # `op Eve` when asked; then stop the server
    build/release/craftc --server-ip=127.0.0.1 --server-port=25580 --online-mode=false --view-distance=2 --max-players=20 --ops-file=/tmp/ops.json
    tools/vanilla/command_probe.py record 25580 /tmp/ours.json Eve Fay          # `op Eve` when asked
    tools/vanilla/command_probe.py diff /tmp/vanilla.json /tmp/ours.json

Reading the diff:
  * Use names vanilla has not seen: it saves players, and the first thing A does is teleport
    both bots to a spot of their own (from the names), so that every coordinate a message names
    is the same on both servers and nothing an earlier recording built is in the way.
  * Offline UUIDs come from the names, so the bots' are the same on both servers; the UUID of
    anything summoned is random and replaced by <uuid>.
  * `help` lists every command a server has: only the lines of commands both have are compared.
    The command tree is compared command by command, for the commands ours has: every node of
    ours must be vanilla's (type, parser and its properties, runs or not, suggestions); what
    vanilla has more is listed, not counted (-v prints it).
  * What this server does not do at all is left out: the particles and sound of a block that
    /setblock destroy breaks (level_event), the death of what /kill kills (ours vanishes at
    once; the messages are the same), "made the advancement" and "left the game" messages,
    grass that turns to dirt under a block a command put on it (a random tick). An entity a
    command made is listed at the end of its step: vanilla shows it at once, ours with the
    tick's other entities.
  * `list` names the most players a server takes: start ours with vanilla's max-players.
  * Known differences are listed in docs/architecture.md ("Commands").
"""
from __future__ import annotations

import json
import re
import struct
import sys
import time
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tests" / "integration"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import mcproto as mc  # noqa: E402
from inventory_probe import Actor  # noqa: E402

LOGGED = {"system_chat", "block_update", "section_blocks_update", "player_position", "sound", "stop_sound", "add_entity",
          "remove_entities", "entity_event", "commands", "disconnect", "set_entity_data", "container_set_slot",
          "set_player_inventory", "take_item_entity"}
UUID_RX = re.compile(r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$")
ROOT = Path(__file__).resolve().parents[2]


class Prober(Actor):
    logged = LOGGED

    def say(self, command: str) -> None:
        self.c.send("chat_command", mc.mc_str(command))


def record(port: int, out_path: str, name_a: str, name_b: str, console: str | None = None) -> None:
    steps: list[dict] = []
    a, b = Prober(port, name_a), Prober(port, name_b)
    for bot in (a, b):
        bot.join()
        bot.pump(1.5)
        bot.c.send("player_loaded")
        bot.tick_end()
        bot.pump(0.5)


    gone = set()   # the bots that were thrown out (B, at the end)

    def pump(bot, seconds: float) -> None:
        if bot in gone:
            return
        try:
            bot.tick_end()
            bot.pump(seconds)
        except (ConnectionError, OSError):
            gone.add(bot)

    def type_on_console(line: str) -> None:
        if console:
            with open(console, "w") as f:
                f.write(line + "\n")
        else:
            print(f">>> type on the server's console now:  {line}")
        for _ in range(1 if console else 8):
            for bot in (a, b):
                pump(bot, 0.5)

    def step(name: str, action=None, ticks: int = 5):
        t0 = time.time()
        mark_a, mark_b = len(a.log), len(b.log)
        for i in range(ticks):
            if i == 0 and action:
                action()
            for bot in (a, b):
                pump(bot, 0.001)
            time.sleep(max(0.0, t0 + (i + 1) * 0.05 - time.time()))
        pump(a, 0.15)
        pump(b, 0.05)
        steps.append({"step": name, "A": a.log[mark_a:], "B": b.log[mark_b:]})
        print(f"{name}: A {len(a.log) - mark_a}, B {len(b.log) - mark_b} packets")

    type_on_console(f"op {name_a}")   # A is told its level and sent the commands that come with it

    def run(command: str, bot=a, ticks: int = 5):
        step(("" if bot is a else "B: ") + "/" + command, lambda: bot.say(command), ticks)

    # The tree as each was sent it: B on joining, A then and again as an operator.
    steps.append({"step": "joined", "A": [x for x in a.log if x[0] in ("commands", "entity_event")],
                  "B": [x for x in b.log if x[0] in ("commands", "entity_event")]})

    # A spot of its own for every pair of names, far from where anything else happened.
    spot = zlib.crc32((name_a + name_b).encode())
    ox, oz = 4000 + spot % 500 * 32, 4000 + spot // 500 % 500 * 32
    oy = -60    # on top of a flat world's grass
    step("A goes to its spot", lambda: a.say(f"tp {ox} {oy} {oz}"), ticks=30)
    step("...and fetches B", lambda: a.say(f"tp {name_b} {ox + 2} {oy} {oz}"), ticks=30)
    origin = (ox, oy, oz)

    run("list")
    run("list uuids")
    run("list", b)
    run("help")
    run("help setblock")
    run("help tp")
    run("help teleport @s")
    run("help nonsense")

    # ---- blocks
    run("setblock ~ ~ ~2 stone")
    run("setblock ~ ~ ~2 stone")                      # nothing changes: failed
    run("setblock ~ ~ ~2 minecraft:glowstone keep")   # not air: failed
    run("setblock ~ ~ ~2 glowstone replace")
    run("setblock ~ ~ ~2 air destroy")                # drops the glowstone dust
    run("setblock ~ ~1 ~2 chest[facing=east]")
    run("setblock ~1 ~1 ~2 chest[facing=east,type=left] strict")
    run("setblock ~ ~1 ~2 air")
    run("setblock ~1 ~1 ~2 air")
    run("setblock ~ 5000 ~ stone")                    # out of the world
    run("setblock ~ ~ ~2 nonsense")
    run("setblock ~ ~ ~2 chest[facing=up]")
    run("setblock ~ ~ ~2 chest[colour=red]")
    run("setblock ~ ~ ~2 #logs")
    run("setblock ~ ~")
    run("setblock ~ ~ ~2")
    run("setblock ~ ~ ~2 stone please")
    run("fill ~-2 ~ ~3 ~2 ~2 ~5 stone")
    run("fill ~-2 ~ ~3 ~2 ~2 ~5 glowstone hollow")
    run("fill ~-2 ~ ~3 ~2 ~2 ~5 oak_planks outline")
    run("fill ~-2 ~ ~3 ~2 ~2 ~5 stone replace oak_planks")
    run("fill ~-2 ~ ~3 ~2 ~2 ~5 dirt replace #minecraft:planks")   # none left: failed
    run("fill ~-2 ~ ~3 ~2 ~2 ~6 cobblestone keep")
    run("fill ~-2 ~ ~3 ~2 ~2 ~6 air")
    run("fill ~-2 ~ ~3 ~2 ~2 ~6 air")                 # nothing changes: failed
    run("fill ~ ~ ~ ~100 ~100 ~100 stone")            # too many
    run("fill ~ ~ ~3 ~ ~ ~3 tnt strict")
    run("fill ~ ~ ~3 ~ ~ ~3 air destroy")             # the TNT comes out as an item

    # ---- items
    run("give @s diamond")
    run("give @s minecraft:stone 3")
    run(f"give {name_b} oak_planks 64")
    run("give @a stick 2")
    run("give @s ender_pearl 1601")                   # more than a hundred stacks of 16
    run("give @s stone 0")
    run("give @s nonsense")
    run("give Nobody stone")
    run("give @e stone")

    # ---- moving
    run("tp ~ ~5 ~")
    run(f"tp @s {ox}.5 {oy} {oz}.5 90 45")
    run("tp @s ~ ~ ~ ~10 ~")
    run(f"tp {name_b}")
    run(f"teleport {name_b} {name_a}")
    run(f"tp @a {ox} {oy + 1} {oz}")
    run(f"tp {name_b} {ox + 2} {oy} {oz}")
    run(f"tp {ox} {oy} {oz}")
    run("tp @s 0 30000000 0")                         # outside the world
    run("tp ~ ~")
    run("tp Nobody")
    run("tp @e[type=nonsense]")
    run("tp @e[type=player,limit=0]")
    run("tp @x")
    run(f"tp @a[name={name_b},distance=..10] ~ ~ ~")

    # ---- entities
    run("summon armor_stand ~ ~ ~3")
    run("summon minecraft:armor_stand ~1 ~ ~3")
    run("kill @e[type=armor_stand,distance=..10,limit=1,sort=nearest]")
    run("summon armor_stand ~2 ~ ~3")
    run("kill @e[type=armor_stand,distance=..10]")
    run("kill @e[type=armor_stand,distance=..10]")    # none left
    run("summon item ~ ~ ~3")
    run("summon player")
    run("summon nonsense")
    run("summon armor_stand 0 30000000 0")

    # ---- sounds
    run("playsound minecraft:block.note_block.harp")
    run("playsound block.note_block.harp block @a ~ ~ ~ 0.5 2")
    run(f"playsound some:thing voice {name_b} ~ ~500 ~")            # too far
    run(f"playsound some:thing voice {name_b} ~ ~500 ~ 1 1 0.25")   # ...but with a least volume
    run("playsound block.note_block.harp master @a ~ ~ ~ 3")
    run("playsound block.note_block.harp master @a ~ ~ ~ 1 5")
    run("stopsound @a")
    run("stopsound @a music")
    run("stopsound @a * minecraft:block.note_block.harp")
    run(f"stopsound {name_b} block block.note_block.harp")

    # ---- what is not a command, and who may run what
    run("nonsense")
    run("")
    run("list all of them")
    run("setblock ~ ~ ~2 stone", b)
    run("help", b)
    run("help setblock", b)
    run(f"op {name_b}")
    run(f"op {name_b}")                               # already one
    run("setblock ~ ~ ~4 stone", b)
    run("setblock ~ ~ ~4 air", b)
    run(f"deop {name_b}")
    run(f"deop {name_b}")
    run("kick Nobody")
    run(f"kick {name_b} that is enough")

    type_on_console(f"deop {name_a}")
    a.c.close()
    Path(out_path).write_text(json.dumps({"origin": origin, "names": [name_a, name_b], "steps": steps}))
    print(f"wrote {out_path}")


# ----------------------------------------------------------------------------- the command tree

def _argument_names() -> dict[int, str]:
    """minecraft:command_argument_type by id, from Mojang's registries report."""
    report = sorted((ROOT / ".cache" / "mc").glob("*/reports/registries.json"))
    if not report:
        sys.exit("command_probe: reports/registries.json is missing (mcgen.py reports)")
    entries = json.loads(report[-1].read_text())["minecraft:command_argument_type"]["entries"]
    return {e["protocol_id"]: name for name, e in entries.items()}


def parse_tree(r: mc.Reader) -> dict:
    """ClientboundCommandsPacket with every argument type of vanilla (the test client's
    parse_commands reads the ones this server names). The properties per type are those of the
    ArgumentTypeInfo classes in net.minecraft.commands.synchronization and .arguments."""
    names = _argument_names()
    nodes = []
    for _ in range(r.varint()):
        flags = r.u8()
        node = {"type": ("root", "literal", "argument")[flags & 3], "executable": bool(flags & 4),
                "restricted": bool(flags & 32), "child_ids": [r.varint() for _ in range(r.varint())]}
        if flags & 8:
            node["redirect_id"] = r.varint()
        if flags & 3:
            node["name"] = r.str()
        if flags & 3 == 2:
            parser = names[r.varint()]
            props = {}
            if parser in ("brigadier:float", "brigadier:double", "brigadier:integer", "brigadier:long"):
                bounds = r.u8()
                read = {"brigadier:float": r.f32, "brigadier:double": r.f64, "brigadier:integer": r.i32, "brigadier:long": r.i64}[parser]
                if bounds & 1:
                    props["min"] = read()
                if bounds & 2:
                    props["max"] = read()
            elif parser == "brigadier:string":
                props["type"] = ("word", "phrase", "greedy")[r.varint()]
            elif parser == "minecraft:entity":
                mask = r.u8()
                props = {"amount": "single" if mask & 1 else "multiple", "type": "players" if mask & 2 else "entities"}
            elif parser == "minecraft:score_holder":
                props["amount"] = "multiple" if r.u8() & 1 else "single"
            elif parser == "minecraft:time":
                props["min"] = r.i32()
            elif parser in ("minecraft:resource_or_tag", "minecraft:resource_or_tag_key", "minecraft:resource",
                            "minecraft:resource_key", "minecraft:resource_selector"):
                props["registry"] = r.str()
            node["parser"], node["properties"] = parser, props
            if flags & 16:
                node["suggestions"] = r.str()
        nodes.append(node)
    root = nodes[r.varint()]
    r.done()
    for node in nodes:
        node["children"] = {nodes[i]["name"]: nodes[i] for i in node.pop("child_ids")}
        if "redirect_id" in node:
            node["redirect"] = nodes[node.pop("redirect_id")].get("name", "")   # "" is the root (execute run)
    return root


def _same_node(ours: dict, ref: dict, path: str, problems: list, extra: list) -> None:
    for key in ("type", "executable", "restricted", "parser", "properties", "suggestions", "redirect"):
        if ours.get(key) != ref.get(key):
            problems.append(f"{path}: {key} is {ours.get(key)!r}, vanilla's {ref.get(key)!r}")
    common = [n for n in ours["children"] if n in ref["children"]]
    if common != [n for n in ref["children"] if n in ours["children"]]:
        problems.append(f"{path}: children in the order {common}, vanilla's {[n for n in ref['children'] if n in ours['children']]}")
    for name, child in ours["children"].items():
        if name not in ref["children"]:
            problems.append(f"{path} {name}: not in vanilla's tree")
        else:
            _same_node(child, ref["children"][name], f"{path} {name}", problems, extra)
    extra.extend(f"{path} {name}" for name in ref["children"] if name not in ours["children"])


def compare_trees(ours: dict, ref: dict) -> tuple[list, list]:
    problems, extra = [], []
    for name, node in ours["children"].items():
        if name not in ref["children"]:
            problems.append(f"/{name}: vanilla has no such command")
        else:
            _same_node(node, ref["children"][name], "/" + name, problems, extra)
    return problems, extra


# ----------------------------------------------------------------------------- diff

def _text(v, bots: set):
    """A text component with the UUIDs that are random by nature replaced."""
    if isinstance(v, dict):
        out = {}
        for k, x in v.items():
            if k == "uuid" and isinstance(x, tuple) and len(x) == 4:
                hi, lo = (x[0] & 0xFFFFFFFF) << 32 | x[1] & 0xFFFFFFFF, (x[2] & 0xFFFFFFFF) << 32 | x[3] & 0xFFFFFFFF
                out[k] = x if f"{hi:016x}{lo:016x}" in bots else "<uuid>"
            elif k == "insertion" and isinstance(x, str) and UUID_RX.match(x) and x.replace("-", "") not in bots:
                out[k] = "<uuid>"
            else:
                out[k] = _text(x, bots)
        return out
    if isinstance(v, list):
        return [_text(x, bots) for x in v]
    return v


def _plain(v) -> str:
    """Enough of Component.getString to recognise a line: literal text and the children."""
    if isinstance(v, str):
        return v
    if isinstance(v, dict):
        return str(v.get("text", "")) + "".join(_plain(x) for x in v.get("extra", []))
    return ""


def _normalise(rec: dict, who: str, commands: set | None) -> tuple[dict[str, list], dict]:
    origin = rec["origin"]
    bots = {mc.offline_uuid(n) for n in rec["names"]}
    out, tree = {}, None
    own_id = None
    for st in rec["steps"]:
        rows, appeared = [], []
        blocks = {}
        for name, hexdata in st[who]:
            r = mc.Reader(bytes.fromhex(hexdata))
            if name == "system_chat":
                value = _text(r.nbt(), bots)
                overlay = r.bool()
                line = _plain(value)
                key = value.get("translate", "") if isinstance(value, dict) else ""
                if key.startswith("chat.type.advancement") or key.startswith("multiplayer.player."):
                    rows.append(("  (not compared)", key))
                    continue
                if st["step"].endswith("/help") or st["step"].endswith("B: /help"):   # one line per command: only those both have
                    if commands is not None and line.split(" ")[0].lstrip("/") not in commands:
                        continue
                rows.append((name, mc.snbt(value) + (" (overlay)" if overlay else "")))
            elif name == "disconnect":
                rows.append((name, mc.snbt(_text(r.nbt(), bots))))
            elif name in ("block_update", "section_blocks_update"):
                changed = dict([mc.parse_block_update(r)]) if name == "block_update" else mc.parse_section_blocks_update(r)
                for pos, state in changed.items():
                    rel = tuple(p - o for p, o in zip(pos, origin))
                    if rel[1] == -1 and state == mc.BLOCK["kDirt"]:   # the ground under a block: a random tick's doing
                        rows.append(("  (not compared)", f"grass turned to dirt at {rel}"))
                    else:
                        blocks[rel] = state
            elif name == "player_position":
                r.varint()
                pos, delta = (r.f64(), r.f64(), r.f64()), (r.f64(), r.f64(), r.f64())
                rot, flags = (r.f32(), r.f32()), r.i32()
                rows.append((name, f"pos {pos} delta {delta} rot {rot} relative {flags:#x}"))
            elif name == "sound":
                holder = r.varint()
                if holder != 0:
                    if holder - 1 != mc.SOUND["kEntityItemPickup"]:
                        continue     # a sound of the world (footsteps, a block placed), not of a command
                    pitch_free = True
                    sound = "entity.item.pickup"
                else:
                    sound, pitch_free = r.str(), False
                    assert not r.bool()
                source = r.varint()
                pos = tuple(v / 8 - o for v, o in zip((r.i32(), r.i32(), r.i32()), origin))
                volume, pitch = r.f32(), r.f32()
                rows.append((name, f"{sound} source {source} at {pos} volume {volume} pitch {'random' if pitch_free else pitch}"))
            elif name == "stop_sound":
                flags = r.u8()
                rows.append((name, f"source {r.varint() if flags & 1 else '*'} sound {r.str() if flags & 2 else '*'}"))
            elif name == "entity_event":
                eid, event = r.i32(), r.u8()
                if 24 <= event <= 28:
                    own_id = eid if own_id is None else own_id
                    if eid == own_id:
                        rows.append((name, f"permission level {event - 24}"))
            elif name == "add_entity":
                r.varint()
                r.take(16)
                etype = r.varint()
                pos = tuple(round(v - o, 3) for v, o in zip((r.f64(), r.f64(), r.f64()), origin))
                if etype == mc.ENTITY_TYPE["kArmorStand"]:
                    appeared.append((name, f"armor stand at {pos}"))
            elif name == "commands":
                tree = parse_tree(r)
                rows.append(("  (not compared)", f"a command tree: {sorted(tree['children'])}"))
        if blocks:
            rows.append(("blocks", str(sorted(blocks.items()))))
        out[st["step"]] = rows + appeared
    return out, tree


def diff(path_ref: str, path_ours: str) -> int:
    ref, ours = json.load(open(path_ref)), json.load(open(path_ours))
    different = 0
    verbose = "-v" in sys.argv
    for who in ("A", "B"):
        right, tree_ours = _normalise(ours, who, None)
        commands = set(tree_ours["children"]) if tree_ours else set()
        left, tree_ref = _normalise(ref, who, commands)
        right, _ = _normalise(ours, who, commands)
        print(f"################ what {who} is told (left: {path_ref}, right: {path_ours})")
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
                if lhs != rhs or verbose:
                    print(f"   {'  ' if lhs == rhs else '| '}{lhs[0]:16s}{lhs[1]}\n   {'  ' if lhs == rhs else '| '}{rhs[0]:16s}{rhs[1]}")
            if notes and verbose:
                print("   not compared: " + "; ".join(notes)[:300])
        if tree_ours and tree_ref:
            problems, extra = compare_trees(tree_ours, tree_ref)
            print(f"== the command tree {who} has at the end: {'same, as far as ours goes' if not problems else 'DIFFERENT'} "
                  f"({len(tree_ours['children'])} commands of vanilla's {len(tree_ref['children'])}; "
                  f"{len(extra)} nodes of theirs are not in ours)")
            for p in problems:
                print("   | " + p)
            different += bool(problems)
            if verbose:
                for e in extra:
                    print("   not in ours: " + e)
    print(f"{different} steps differ")
    return different


def main() -> int:
    if len(sys.argv) >= 4 and sys.argv[1] == "record":
        names = (sys.argv[4], sys.argv[5]) if len(sys.argv) >= 6 else ("ProbeE", "ProbeF")
        record(int(sys.argv[2]), sys.argv[3], *names, sys.argv[6] if len(sys.argv) >= 7 else None)
        return 0
    if len(sys.argv) >= 4 and sys.argv[1] == "diff":   # -v: every row, and what is not compared
        return 1 if diff(sys.argv[2], sys.argv[3]) else 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
