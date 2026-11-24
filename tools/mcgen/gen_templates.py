# SPDX-License-Identifier: GPL-3.0-or-later
"""The tables of structure templates (gen_features.py's FeatureTables is one of these too): the templates the
features place - TemplateFeature, FossilFeature - their processors, and how a block state turns.

A template is the jar's .nbt as the game holds it once it is loaded: `mcgen.py worldgen` writes each one's
blocks in the order StructureTemplate keeps them (features.json, "templates"), and BlockState.rotate of every
state it changes ("rotations"); here they become rows. What placing one *does* is src/worldgen/template.cpp.
"""
from __future__ import annotations

import struct

from mcgen import die

# The tags a template's block may carry that src/worldgen knows how to make a block entity's tag of: the tag as
# the game prints it, and the block entity it is.
TEMPLATE_TAGS = {'{components:{},id:"minecraft:brushable_block"}': "minecraft:brushable_block"}


def cfloat(v: float) -> str:
    return struct.unpack("<f", struct.pack("<f", v))[0].hex() + "f"


class TemplateTables:
    def init_templates(self) -> None:
        from gen_features import Pool
        self.templates: list = []                 # (size, first block, blocks, name)
        self.template_of: dict[str, int] = {}
        self.template_blocks: list = []
        self.template_states: set[int] = set()    # every state a template or a processor can put down: what is turned
        self.processors, self.processor_rules = Pool(), Pool()
        self.processor_lists: dict[str, tuple[int, int]] = {}
        self.loot_tables: list[str] = [""]
        self.template_entries, self.template_features, self.fossils, self.fossil_templates = Pool(), Pool(), Pool(), Pool()

    def template(self, name: str) -> int:
        if name in self.template_of:
            return self.template_of[name]
        made = self.extra.get("templates", {}).get(name)
        if made is None:
            die(f"gen_features: the structure template {name}: worldgen/features.json does not have it - run: mcgen.py worldgen")
        if len(made["palettes"]) != 1:
            die(f"gen_features: the structure template {name} has {len(made['palettes'])} palettes: src/worldgen draws none")
        first = len(self.template_blocks)
        for block in made["palettes"][0]:
            x, y, z, state = block[:4]
            tag = 0
            if len(block) > 4:
                if block[4] not in TEMPLATE_TAGS:
                    die(f"gen_features: a block of {name} carries the tag {block[4]}: src/worldgen/template.cpp makes no block entity of it")
                tag = 1 + list(TEMPLATE_TAGS).index(block[4])
            if not all(0 <= v < 128 for v in (x, y, z)):
                die(f"gen_features: {name} is larger than a table's bytes count")
            self.template_blocks.append((x, y, z, tag, state))
            self.template_states.add(state)
        self.template_of[name] = len(self.templates)
        self.templates.append((made["size"], first, len(self.template_blocks) - first, name))
        return self.template_of[name]

    def loot_table(self, name: str) -> int:
        if name not in self.loot_tables:
            self.loot_tables.append(name)
        return self.loot_tables.index(name)

    def processor_list(self, value) -> tuple[int, int]:
        """A Holder<StructureProcessorList>: a name, or {processors: [...]}. (first, count) of kProcessors."""
        key = value if isinstance(value, str) else None
        if key is not None:
            if key in self.processor_lists:
                return self.processor_lists[key]
            value = self.load("processor_list", key)
        if not isinstance(value, dict) or set(value) != {"processors"}:
            die(f"gen_features: a list of processors written as {str(value)[:200]}")
        rows = []
        for p in value["processors"]:
            kind = p.get("processor_type", "").removeprefix("minecraft:")
            if kind == "block_rot":
                if not set(p) <= {"processor_type", "integrity", "rottable_blocks"}:
                    die(f"gen_features: a block_rot processor written as {p!r}")
                rottable = 1 + self.block_set(p["rottable_blocks"]) if "rottable_blocks" in p else 0
                rows.append(("kBlockRot", float(p["integrity"]), rottable, 0, 0))
            elif kind == "protected_blocks":
                if set(p) != {"processor_type", "value"}:
                    die(f"gen_features: a protected_blocks processor written as {p!r}")
                rows.append(("kProtectedBlocks", 0.0, self.block_set(p["value"]), 0, 0))
            elif kind == "rule":
                if set(p) != {"processor_type", "rules"}:
                    die(f"gen_features: a rule processor written as {p!r}")
                rules = []
                for rule in p["rules"]:
                    if not set(rule) <= {"input_predicate", "location_predicate", "output_state", "block_entity_modifier", "position_predicate"}:
                        die(f"gen_features: a processor's rule written as {rule!r}")
                    if rule.get("position_predicate", {"predicate_type": "minecraft:always_true"}).get("predicate_type") != "minecraft:always_true":
                        die("gen_features: a processor's rule asks where in the template a block is: src/worldgen/template.cpp has no such test")
                    loot = 0
                    modifier = rule.get("block_entity_modifier", {"type": "minecraft:passthrough"})
                    if modifier["type"] == "minecraft:append_loot":
                        loot = self.loot_table(modifier["loot_table"])
                    elif modifier["type"] != "minecraft:passthrough":
                        die(f"gen_features: a processor's rule changes a block entity by {modifier['type']}")
                    output = self.blocks.state(rule["output_state"])
                    self.template_states.add(output)
                    rules.append((self.rule(rule["input_predicate"]), self.rule(rule["location_predicate"]), output, loot))
                rows.append(("kRule", 0.0, 0, self.processor_rules.run(rules), len(rules)))
            else:
                die(f"gen_features: a structure processor of the type {kind}: src/worldgen/template.cpp has none")
        made = (self.processors.run(rows), len(rows))
        if key is not None:
            self.processor_lists[key] = made
        return made

    def template_feature(self, value) -> tuple:
        self.need("template", value, {"templates"}, {"processors"})
        entries, total = [], 0
        for entry in value["templates"]:
            if set(entry) != {"data", "weight"} or not set(entry["data"]) <= {"id", "rotations"}:
                die(f"gen_features: a template of a feature written as {entry!r}")
            if "rotations" in entry["data"]:
                die("gen_features: a template of a feature names its rotations: src/worldgen/template.cpp draws one of all four")
            entries.append((self.template(entry["data"]["id"]), int(entry["weight"])))
            total += int(entry["weight"])
        first, count = self.processor_list(value["processors"]) if "processors" in value else (0, 0)
        row = (self.template_entries.run(entries), len(entries), total, first, count)
        return ("kTemplate", self.template_features.run([row]), 0, 0)

    def fossil(self, value) -> tuple:
        self.need("fossil", value, {"fossil_structures", "overlay_structures", "fossil_processors", "overlay_processors", "max_empty_corners_allowed"})
        if len(value["fossil_structures"]) != len(value["overlay_structures"]) or not value["fossil_structures"]:
            die("gen_features: a fossil's structures and their overlays are not as many (FossilFeature.CODEC refuses it)")
        pairs = [(self.template(a), self.template(b)) for a, b in zip(value["fossil_structures"], value["overlay_structures"])]
        fossil, overlay = self.processor_list(value["fossil_processors"]), self.processor_list(value["overlay_processors"])
        row = (self.fossil_templates.run(pairs), len(pairs), *fossil, *overlay, int(value["max_empty_corners_allowed"]))
        return ("kFossil", self.fossils.run([row]), 0, 0)


def takes_water(t) -> int:
    """The states SimpleWaterloggedBlock.placeLiquid waterlogs when it is handed water: a block of the interface
    that is dry - but a double slab, whose class says no. (Every state: what a template puts down is checked
    against the classes that say otherwise, in emit_templates.)"""
    import gamedata
    game = {b["name"]: b for b in gamedata.load(t.version, "blocks")["blocks"]}
    bits = 0
    for name, block in t.blocks.blocks.items():
        if "SimpleWaterloggedBlock" not in game[name]["interfaces"]:
            continue
        for s in block["states"]:
            if s["properties"].get("waterlogged") != "false":
                continue
            if "SlabBlock" in game[name]["class"] and s["properties"]["type"] == "double":
                continue
            bits |= 1 << s["id"]
    return bits


def liquid_containers(t) -> int:
    """The states of the blocks a fluid is handed to (LiquidBlockContainer)."""
    import gamedata
    game = {b["name"]: b for b in gamedata.load(t.version, "blocks")["blocks"]}
    return sum(1 << s["id"] for name, block in t.blocks.blocks.items() if "LiquidBlockContainer" in game[name]["interfaces"] for s in block["states"])


def check_block_entities(t) -> None:
    """A block entity a template leaves is one src/worldgen/template.cpp can write the tag of: a brushable block's."""
    import gamedata
    game = {b["name"]: b for b in gamedata.load(t.version, "blocks")["blocks"]}
    name_of = {s["id"]: name for name, block in t.blocks.blocks.items() for s in block["states"]}
    entity = lambda state: game[name_of[state]].get("block_entity_type")
    for x, y, z, tag, state in t.template_blocks:
        if tag and entity(state) != TEMPLATE_TAGS[list(TEMPLATE_TAGS)[tag - 1]]:
            die(f"gen_features: a block of a structure template carries a tag for another block entity than its own ({name_of[state]})")
    for _, _, output, loot in t.processor_rules.rows:
        if entity(output) not in (None, "minecraft:brushable_block") or (loot and entity(output) is None):
            die(f"gen_features: a processor's rule makes {name_of[output]}, with a block entity src/worldgen/template.cpp cannot write")


def check_liquids(t) -> None:
    """A block of a template that takes a fluid does so as SimpleWaterloggedBlock says - or its class is one the
    port knows (a slab). Anything else stops the run: StructureTemplate.placeInWorld hands such a block the fluid
    it was put into, and src/worldgen/template.cpp knows the one way."""
    import gamedata
    from gen import classes_declaring
    game = {b["name"]: b for b in gamedata.load(t.version, "blocks")["blocks"]}
    name_of = {s["id"]: name for name, block in t.blocks.blocks.items() for s in block["states"]}
    containers = sorted({name_of[state] for state in t.template_states if "LiquidBlockContainer" in game[name_of[state]]["interfaces"]})
    classes = set()
    for name in containers:
        if "SimpleWaterloggedBlock" not in game[name]["interfaces"]:
            die(f"gen_features: {name} stands in a structure template and takes a fluid in a way of its own: src/worldgen/template.cpp has none")
        classes |= set(game[name]["class"]) - {"Block", "BlockBehaviour"}
    if classes:
        says = {c.rsplit(".", 1)[1] for c in classes_declaring(t.version, {"net.minecraft.world.level.block." + c for c in classes}, "placeLiquid")}
        if not says <= {"SlabBlock"}:
            die(f"gen_features: placeLiquid is declared by {sorted(says)}, of blocks that stand in structure templates: read the class")


def emit_templates(t, out: list[str]) -> None:
    check_liquids(t)
    check_block_entities(t)
    out.append("""
// ---------------------------------------------------------------------------------------------
// Structure templates (tools/mcgen/gen_templates.py): what src/worldgen/template.cpp places.

// A template as StructureTemplate holds it: its size, and kTemplateBlocks[first .. first + count) in
// the order they are placed. A block's tag: 0 for none, else which of the tags a block entity is made of.
struct Template {
    i32 size_x, size_y, size_z;
    u32 first, count;
};
struct TemplateBlock {
    i8 x, y, z;
    u8 tag;
    u16 state;
};
// StructureProcessor: kBlockRot keeps a block with the chance `integrity` (of the blocks of set - 1 only,
// where set is not 0); kProtectedBlocks leaves what is in the world where that is of the set;
// kRule: kProcessorRules[rules .. rules + rule_count), the first that holds says what the block becomes.
enum class ProcessorOp : u8 { kBlockRot, kProtectedBlocks, kRule };
struct Processor {
    ProcessorOp op;
    float integrity;
    u16 set;
    u16 rules, rule_count;
};
struct ProcessorRule {
    u16 input, location;  // kRules: of the block that would be placed, of the block that is there
    u16 output_state;
    u16 loot_table;  // kLootTables: what AppendLoot gives the block entity (0: nothing)
};
// TemplateFeature: kTemplateEntries[entries .. + entry_count) - templates, one drawn by weight - turned any
// of four ways.
struct TemplateFeature {
    u16 entries, entry_count;
    i32 total_weight;
    u16 processors, processor_count;
};
// FossilFeature: kFossilTemplates[structures .. + count) - a fossil and what is laid over it.
struct FossilTemplates {
    u16 fossil, overlay;
};
struct Fossil {
    u16 structures, count;
    u16 fossil_processors, fossil_processor_count, overlay_processors, overlay_processor_count;
    i32 max_empty_corners_allowed;
};
// BlockState.rotate: a state, and what it is turned clockwise a quarter, a half, and a quarter the
// other way - for the states a template can put down that turning changes, in the order of the states.
struct StateRotation {
    u16 state;
    u16 to[3];
};
""")
    out.append(f"inline constexpr Template kTemplates[{max(1, len(t.templates))}] = {{\n")
    out += [f"    {{{size[0]}, {size[1]}, {size[2]}, {first}, {count}}},  // {name.removeprefix('minecraft:')}\n" for size, first, count, name in t.templates]
    out.append("};\n")
    out.append(f"inline constexpr TemplateBlock kTemplateBlocks[{max(1, len(t.template_blocks))}] = {{\n")
    for i in range(0, len(t.template_blocks), 8):
        out.append("    " + " ".join(f"{{{x},{y},{z},{tag},{state}}}," for x, y, z, tag, state in t.template_blocks[i:i + 8]) + "\n")
    out.append("};\n")
    out.append(f"inline constexpr Processor kProcessors[{max(1, len(t.processors.rows))}] = {{"
               + ", ".join(f"{{ProcessorOp::{op}, {cfloat(integrity)}, {in_set}, {rules}, {count}}}" for op, integrity, in_set, rules, count in t.processors.rows) + "};\n")
    out.append(f"inline constexpr ProcessorRule kProcessorRules[{max(1, len(t.processor_rules.rows))}] = {{"
               + ", ".join("{" + ", ".join(map(str, r)) + "}" for r in t.processor_rules.rows) + "};\n")
    out.append(f"inline constexpr std::string_view kLootTables[{len(t.loot_tables)}] = {{" + ", ".join(f'"{name}"' for name in t.loot_tables) + "};\n")
    out.append(f"inline constexpr Entry kTemplateEntries[{max(1, len(t.template_entries.rows))}] = {{"
               + ", ".join(f"{{{a}, {b}}}" for a, b in t.template_entries.rows) + "};\n")
    out.append(f"inline constexpr TemplateFeature kTemplateFeatures[{max(1, len(t.template_features.rows))}] = {{"
               + ", ".join("{" + ", ".join(map(str, r)) + "}" for r in t.template_features.rows) + "};\n")
    out.append(f"inline constexpr FossilTemplates kFossilTemplates[{max(1, len(t.fossil_templates.rows))}] = {{"
               + ", ".join(f"{{{a}, {b}}}" for a, b in t.fossil_templates.rows) + "};\n")
    out.append(f"inline constexpr Fossil kFossils[{max(1, len(t.fossils.rows))}] = {{"
               + ", ".join("{" + ", ".join(map(str, r)) + "}" for r in t.fossils.rows) + "};\n")
    rotations = {row[0]: row[1:] for row in t.extra.get("rotations", [])}
    if not rotations:
        die("gen_features: worldgen/features.json does not say how block states turn - run: mcgen.py worldgen")
    turned = sorted(state for state in t.template_states if state in rotations)
    out.append(f"inline constexpr StateRotation kStateRotations[{max(1, len(turned))}] = {{"
               + ", ".join(f"{{{state}, {{{rotations[state][0]}, {rotations[state][1]}, {rotations[state][2]}}}}}" for state in turned) + "};\n")
    out.append(f"inline constexpr u32 kStateRotationCount = {len(turned)};\n")
    out.append("// The block entities a template's block may carry a tag for (TemplateBlock::tag - 1).\n")
    out.append(f"inline constexpr std::string_view kTemplateBlockEntities[{len(TEMPLATE_TAGS)}] = {{" + ", ".join(f'"{v}"' for v in TEMPLATE_TAGS.values()) + "};\n")
