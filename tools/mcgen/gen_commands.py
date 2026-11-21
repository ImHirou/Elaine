# SPDX-License-Identifier: GPL-3.0-or-later
"""What commands need to know about the game by name: generated from the caches like everything else.

Emitted files:
  command_data.hpp    ids of the command argument types (the `commands` packet names a parser by
                      its id in minecraft:command_argument_type); every block's properties with
                      their value names and strides (`stone_slab[type=top]` -> a state id); item
                      names, each item's default name and rarity (how a stack is shown in a
                      message); blocks, items and entity types sorted by name, for lookups; which
                      entity types `/summon` takes. Include from world/block.cpp only.
  lang.hpp            en_us.json, the keys a server prints: what a translatable message reads as
                      on the console. Include from text/lang.cpp only.

Inputs: reports/registries.json, reports/item_components/ (`mcgen.py reports`), gamedata/blocks.json
and entities.json (`mcgen.py gamedata`), and assets/minecraft/lang/en_us.json inside the server jar.
"""
from __future__ import annotations

import json
import zipfile
from pathlib import Path

from mcgen import die, jar_for

# The argument types the server's command tree can name (the others have no parser here).
ARGUMENT_TYPES = ["brigadier:bool", "brigadier:float", "brigadier:double", "brigadier:integer", "brigadier:string",
                  "minecraft:entity", "minecraft:game_profile", "minecraft:block_pos", "minecraft:vec3",
                  "minecraft:block_state", "minecraft:block_predicate", "minecraft:item_stack",
                  "minecraft:item_predicate", "minecraft:message", "minecraft:rotation", "minecraft:resource_location",
                  "minecraft:entity_anchor", "minecraft:resource", "minecraft:time"]

# Translation keys by their first part: what commands answer with, what a kick says, and the
# names of the things those messages mention.
LANG_PREFIXES = ("commands", "command", "argument", "arguments", "parsing", "chat", "multiplayer", "disconnect", "entity",
                 "item", "block", "permissions")

RARITIES = ["common", "uncommon", "rare", "epic"]  # Rarity, in ordinal order


def _ident(name: str) -> str:
    return "k" + "".join(p[:1].upper() + p[1:] for p in name.split(":", 1)[1].split("_") if p)


def _cstr(s: str) -> str:
    out = []
    for ch in s:
        if ch in '"\\':
            out.append("\\" + ch)
        elif ch == "\n":
            out.append("\\n")
        elif ord(ch) < 0x20 or ord(ch) == 0x7F:
            out.append(f"\\{ord(ch):03o}")
        else:
            out.append(ch)
    # Adjacent literals: a '?' can never start a trigraph, an octal escape never runs on.
    return '"' + "".join(out).replace("??", '?""?') + '"'


def emit_command_data(header: str, d: Path, static_regs: dict, blocks_report: dict) -> str:
    def entries(reg: str) -> list[str]:
        table = static_regs[f"minecraft:{reg}"]["entries"]
        names = sorted(table, key=lambda n: table[n]["protocol_id"])
        if [table[n]["protocol_id"] for n in names] != list(range(len(names))):
            die(f"minecraft:{reg}: protocol ids are not 0..n-1")
        return names

    o = [header, "\n#include <string_view>\n\n// What commands look up by name (tools/mcgen/gen_commands.py).\n"
                 "namespace proto::gen::command {\n"]

    args = static_regs["minecraft:command_argument_type"]["entries"]
    o.append("\nnamespace argument {  // minecraft:command_argument_type\n")
    for name in ARGUMENT_TYPES:
        if name not in args:
            die(f"minecraft:command_argument_type has no {name}: read what the command tree should name instead")
        o.append(f"inline constexpr unsigned {_ident(name)} = {args[name]['protocol_id']};\n")
    o.append("}  // namespace argument\n")

    # ---- block properties. A property is a name and its values in order; blocks share them.
    game_blocks = {b["name"]: b for b in json.loads((d / "gamedata" / "blocks.json").read_text())["blocks"]}
    block_names = entries("block")
    values: list[str] = []
    value_at: dict[tuple, int] = {}
    props: list[tuple[str, int, int]] = []
    prop_at: dict[tuple, int] = {}
    uses: list[tuple[int, int]] = []
    block_first = [0]
    for name in block_names:
        gb = game_blocks.get(name)
        if gb is None:
            die(f"gamedata/blocks.json has no {name} - run: mcgen.py gamedata")
        report = blocks_report[name]
        count = 1
        for p in gb["properties"]:
            vals = tuple(p["values"])
            if list(vals) != report["properties"][p["name"]]:
                die(f"{name}: property {p['name']} has other values in blocks.json than in gamedata")
            if vals not in value_at:
                value_at[vals] = len(values)
                values.extend(vals)
            key = (p["name"], vals)
            if key not in prop_at:
                prop_at[key] = len(props)
                props.append((p["name"], value_at[vals], len(vals)))
            uses.append((prop_at[key], p["stride"]))
            count *= len(vals)
        if count != len(report["states"]):
            die(f"{name}: {count} states by its properties, {len(report['states'])} in blocks.json")
        block_first.append(len(uses))
    o.append("\n// Block.getStateDefinition(): state = first_state + sum(index of the value * stride).\n"
             "struct Property {\n    std::string_view name;\n    unsigned short first_value;  // in kPropertyValues\n"
             "    unsigned short values;\n};\nstruct BlockProperty {\n    unsigned short property;  // in kProperties\n"
             "    unsigned short stride;\n};\n")
    o.append(f"inline constexpr std::string_view kPropertyValues[{len(values)}] = {{\n")
    for i in range(0, len(values), 8):
        o.append("    " + ", ".join(_cstr(v) for v in values[i:i + 8]) + ",\n")
    o.append(f"}};\ninline constexpr Property kProperties[{len(props)}] = {{\n")
    o.extend(f"    {{{_cstr(n)}, {first}, {count}}},\n" for n, first, count in props)
    o.append(f"}};\ninline constexpr BlockProperty kBlockProperties[{max(len(uses), 1)}] = {{\n")
    for i in range(0, len(uses), 10):
        o.append("    " + ", ".join(f"{{{p}, {s}}}" for p, s in uses[i:i + 10]) + ",\n")
    o.append(f"}};\n// Block b's properties are kBlockProperties[kBlockPropertyFirst[b] .. kBlockPropertyFirst[b + 1]).\n"
             f"inline constexpr unsigned short kBlockPropertyFirst[{len(block_first)}] = {{\n")
    for i in range(0, len(block_first), 16):
        o.append("    " + ", ".join(str(x) for x in block_first[i:i + 16]) + ",\n")
    o.append("};\n")

    def by_name(label: str, names: list[str]) -> None:
        order = sorted(range(len(names)), key=lambda i: names[i])
        o.append(f"\ninline constexpr unsigned short {label}[{len(order)}] = {{  // ids, sorted by name\n")
        for i in range(0, len(order), 16):
            o.append("    " + ", ".join(str(x) for x in order[i:i + 16]) + ",\n")
        o.append("};\n")

    by_name("kBlocksByName", block_names)

    # ---- items: the name, and how a stack of it reads in a message (ItemStack.getDisplayName).
    item_names = entries("item")
    o.append(f"\nstruct ItemText {{\n    std::string_view name;  // \"minecraft:stone\"\n"
             f"    std::string_view key;   // the translation key of its default ITEM_NAME\n"
             f"    unsigned char rarity;   // Rarity ordinal of its default RARITY: common, uncommon, rare, epic\n}};\n"
             f"inline constexpr ItemText kItems[{len(item_names)}] = {{\n")
    for name in item_names:
        path = d / "reports" / "item_components" / (name.split(":", 1)[1] + ".json")
        if name == "minecraft:air" and not path.exists():
            o.append('    {"minecraft:air", "block.minecraft.air", 0},\n')
            continue
        comps = json.loads(path.read_text())["components"]
        item_name = comps.get("minecraft:item_name")
        if not isinstance(item_name, dict) or set(item_name) != {"translate"}:
            die(f"{name}: its default item_name is not a plain translatable any more: {item_name!r}")
        rarity = comps.get("minecraft:rarity", "common")
        if rarity not in RARITIES:
            die(f"{name}: unknown rarity {rarity!r}")
        o.append(f"    {{{_cstr(name)}, {_cstr(item_name['translate'])}, {RARITIES.index(rarity)}}},\n")
    o.append("};\n")
    by_name("kItemsByName", item_names)

    # ---- entity types
    game_entities = {e["name"]: e for e in json.loads((d / "gamedata" / "entities.json").read_text())["types"]}
    entity_names = entries("entity_type")
    by_name("kEntityTypesByName", entity_names)
    o.append(f"\n// EntityType.canSummon(): what /summon takes.\ninline constexpr bool kSummonable[{len(entity_names)}] = {{\n")
    for i in range(0, len(entity_names), 16):
        row = []
        for name in entity_names[i:i + 16]:
            if name not in game_entities:
                die(f"gamedata/entities.json has no {name} - run: mcgen.py gamedata")
            if game_entities[name]["description_id"] != "entity.minecraft." + name.split(":", 1)[1].replace("/", "."):
                die(f"{name}: its description id is not the usual one: {game_entities[name]['description_id']}")
            row.append("true" if game_entities[name]["summon"] else "false")
        o.append("    " + ", ".join(row) + ",\n")
    o.append("};\n\n}  // namespace proto::gen::command\n")
    return "".join(o)


def emit_lang(header: str, version: str) -> str:
    with zipfile.ZipFile(jar_for(version, client=False)) as z:
        lang = json.loads(z.read("assets/minecraft/lang/en_us.json"))
    keep = sorted(k for k in lang if k.split(".", 1)[0] in LANG_PREFIXES)
    for must in ("commands.setblock.success", "command.context.here", "chat.type.admin", "multiplayer.disconnect.kicked"):
        if must not in keep:
            die(f"en_us.json has no {must}")
    o = [header, "\n#include <string_view>\n\n// assets/minecraft/lang/en_us.json: the messages a server prints, sorted by key.\n"
                 "namespace proto::gen::lang {\n\nstruct Entry {\n    std::string_view key, text;\n};\n"
                 f"inline constexpr Entry kEntries[{len(keep)}] = {{\n"]
    o.extend(f"    {{{_cstr(k)}, {_cstr(lang[k])}}},\n" for k in keep)
    o.append("};\n\n}  // namespace proto::gen::lang\n")
    return "".join(o)
