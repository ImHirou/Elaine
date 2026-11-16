"""mcgen code generation, the game-data half: what blocks and items *do*, as far as the server
simulates it. Called from gen.py; everything here reads the jar's data pack and the caches of
`mcgen.py reports` / `gamedata`, and runs nothing.

Emitted:
  recipes.hpp         crafting (shaped, shapeless, transmute) and smelting recipes, ingredients as
                      item lists, results; include from one .cpp
  update_recipes.bin  the play `update_recipes` packet body: the recipe property sets a client needs
  static_ids.hpp      ids of static registries the server names: menus, block entity types, and the
                      sounds and particles it sends

and, for gen.py's block and item tables: what an explosion leaves of a block, fuel burn times,
crafting remainders, the ported block classes.

Every rule is written against one shape of the data and stops (`die`) on anything else: a loot
table or a recipe the reduction does not understand must not turn into a wrong table silently.
"""
from __future__ import annotations

import json
import struct

from mcgen import die

# ----------------------------------------------------------------------------- ported block classes

# Block classes whose behaviour the server implements, by the class name of the block itself:
#   kind      proto::gen::block_kind constant the server dispatches on
#   place     placement kind (gen.py BLOCK_FLAGS): how BlockItem.place picks the state
#   hooks     every BlockBehaviour / Block method the class chain overrides, as gamedata lists
#             them. The port was written against exactly this set: a hook that appears or
#             disappears in a new version means the class has to be read again.
PORTED_BLOCKS = {
    "ChestBlock": ("kChest", "kPlaceChest", {
        "affectNeighborsAfterRemoval", "getAnalogOutputSignal", "getFluidState", "getMenuProvider", "getShape",
        "hasAnalogOutputSignal", "isPathfindable", "mirror", "rotate", "tick", "triggerEvent", "updateShape",
        "useWithoutItem", "createBlockStateDefinition", "getStateForPlacement"}),
    "FurnaceBlock": ("kFurnace", "kPlaceFacing", {
        "affectNeighborsAfterRemoval", "getAnalogOutputSignal", "getMenuProvider", "hasAnalogOutputSignal", "mirror",
        "rotate", "triggerEvent", "useWithoutItem", "animateTick", "createBlockStateDefinition", "getStateForPlacement"}),
    "CraftingTableBlock": ("kCraftingTable", "kPlaceDefault", {"getMenuProvider", "useWithoutItem"}),
    "TntBlock": ("kTnt", "kPlaceDefault", {
        "neighborChanged", "onPlace", "onProjectileHit", "useItemOn", "createBlockStateDefinition", "dropFromExplosion",
        "playerWillDestroy", "wasExploded"}),
    # A constant redstone signal on every side (redstone_block). Placeable by the general rule;
    # the kind is what TNT's "has a neighbour signal" looks for.
    "PoweredBlock": ("kPowered", None, {"isSignalSource", "ownSignal"}),
}
BLOCK_KINDS = ["kPlain", "kChest", "kFurnace", "kCraftingTable", "kTnt", "kPowered"]

# Property layouts the C++ side computes states from: block -> [(property, values in order)].
# state = first_state + sum(value index * stride); gen.py emits the strides and checks the orders.
STATE_LAYOUTS = {
    "minecraft:chest": [("facing", ["north", "south", "west", "east"]), ("type", ["single", "left", "right"]),
                        ("waterlogged", ["true", "false"])],
    "minecraft:furnace": [("facing", ["north", "south", "west", "east"]), ("lit", ["true", "false"])],
    "minecraft:tnt": [("unstable", ["true", "false"])],
}

# Sounds and particles the server sends by id (static registries: sound_event, particle_type).
SOUNDS = ["block.chest.open", "block.chest.close", "entity.tnt.primed", "entity.generic.explode", "entity.item.pickup",
          "entity.armor_stand.break"]
PARTICLES = ["explosion", "explosion_emitter", "poof", "smoke"]


def load_gamedata(d) -> dict:
    """gamedata/blocks.json and items.json by name, states.json as it is (written by `mcgen.py gamedata`)."""
    out = {}
    for section in ("blocks", "items"):
        path = d / "gamedata" / f"{section}.json"
        if not path.exists():
            die(f"gamedata/{section}.json is missing (block classes, loot tables, crafting remainders).\n"
                "       Run `mcgen.py gamedata` - it executes the jar like `reports` does, ask the user first.")
        out[section] = {e["name"]: e for e in json.loads(path.read_text())[section]}
    path = d / "gamedata" / "states.json"
    if not path.exists():
        die("gamedata/states.json is missing - run: mcgen.py gamedata")
    out["states"] = json.loads(path.read_text())  # per-state columns, indexed by state id
    path = d / "gamedata" / "entities.json"
    if not path.exists():
        die("gamedata/entities.json is missing - run: mcgen.py gamedata")
    out["entities"] = json.loads(path.read_text())  # entity types: class chains, default attributes
    return out


def ported_block(game_block: dict):
    """(kind, placement) of a block whose own class is ported, else None. Checks the hook set."""
    cls = game_block["class"][0]
    if cls not in PORTED_BLOCKS:
        return None
    kind, place, hooks = PORTED_BLOCKS[cls]
    if set(game_block["hooks"]) != hooks:
        die(f"{game_block['name']}: {cls} overrides {sorted(set(game_block['hooks']) ^ hooks)} differently from the version "
            "the server's port was written against - read the class again, then update PORTED_BLOCKS")
    return kind, place


# ----------------------------------------------------------------------------- explosion drops

class Loot:
    """Reduces a block's loot table to what ServerExplosion makes of it for a TNT: no tool
    (ItemStack.EMPTY), no explosion radius (Explosion.BlockInteraction.DESTROY: the game rule
    tntExplosionDropDecay is off), so survives_explosion holds and explosion_decay does nothing.

    The result is a short list of candidates (chance, item, min, max), tried in order: the first
    whose draw succeeds drops, nothing after it is looked at. That is `alternatives` with at
    most a table_bonus in front - every table of a block that can be in the world has that shape.
    """

    def __init__(self, vanilla, item_ids: dict[str, int]):
        self.vanilla = vanilla
        self.item_ids = item_ids

    def read(self, kind: str, name: str):
        path = f"data/minecraft/{kind}/{name.split(':', 1)[1]}.json"
        if path not in self.vanilla.names:
            die(f"{path} is not in the jar")
        return json.loads(self.vanilla.zip.read(path))

    def condition(self, c, where: str):
        """True, False, or a probability."""
        if c is None:
            return True
        if isinstance(c, str):
            return self.condition(self.read("predicate", c), where)
        kind = c.get("type") or c.get("condition")
        if kind == "minecraft:survives_explosion":
            return True
        if kind == "minecraft:match_tool":
            p = c["predicate"]
            # An empty stack is in no item set and carries no enchantment.
            if "items" in p:
                return False
            ench = p.get("predicates", {}).get("minecraft:enchantments")
            if ench and all(e.get("levels", {}).get("min", 0) >= 1 for e in ench):
                return False
            die(f"{where}: a match_tool predicate the reduction cannot decide for an empty tool: {p}")
        if kind == "minecraft:table_bonus":
            return float(c["chances"][0])  # the enchantment's level on an empty tool is 0
        if kind == "minecraft:any_of":
            terms = [self.condition(t, where) for t in c["terms"]]
            if any(t is True for t in terms):
                return True
            if all(t is False for t in terms):
                return False
        die(f"{where}: loot condition {c} is not one the explosion-drop reduction knows")

    def count(self, modifiers, where: str) -> tuple[int, int]:
        lo = hi = 1
        if modifiers is None:
            return lo, hi
        for m in modifiers if isinstance(modifiers, list) else [modifiers]:
            kind = m["type"]
            if "condition" in m or "conditions" in m:
                die(f"{where}: a conditional loot function")
            if kind == "minecraft:set_count":
                c = m["count"]
                if m.get("add"):
                    die(f"{where}: set_count with add")
                if isinstance(c, (int, float)) and c == int(c):
                    lo = hi = int(c)
                elif isinstance(c, dict) and c.get("type") == "minecraft:uniform" and all(
                        isinstance(c[k], (int, float)) and c[k] == int(c[k]) for k in ("min", "max")):
                    lo, hi = int(c["min"]), int(c["max"])  # UniformGenerator.getInt: nextInt(min, max), inclusive
                else:
                    die(f"{where}: set_count {c}")
            elif kind == "minecraft:apply_bonus":
                # At enchantment level 0 ore_drops leaves the count alone and uniform_bonus_count
                # adds nextInt(1): both nothing.
                if m["formula"] not in ("minecraft:ore_drops", "minecraft:uniform_bonus_count"):
                    die(f"{where}: apply_bonus formula {m['formula']} draws even without the enchantment")
            elif kind == "minecraft:limit_count":
                limit = m["limit"]
                if lo < limit.get("min", lo) or hi > limit.get("max", hi):
                    die(f"{where}: limit_count cuts into the range {lo}..{hi}")
            elif kind == "minecraft:explosion_decay":
                pass  # needs LootContextParams.EXPLOSION_RADIUS, which a TNT's explosion does not set
            elif kind == "minecraft:copy_components":
                # Copies the block entity's custom name onto the item. Block entities keep no name
                # here (a named chest cannot be placed), so there is nothing to copy.
                if m.get("source") != "block_entity" or m.get("include") != ["minecraft:custom_name"]:
                    die(f"{where}: copy_components {m}")
            else:
                die(f"{where}: loot function {kind} is not one the explosion-drop reduction knows")
        return lo, hi

    def entry(self, e: dict, where: str) -> list[tuple[float, int, int, int]]:
        chance = self.condition(e.get("condition"), where)
        if "conditions" in e or "functions" in e:
            die(f"{where}: the loot table format changed (conditions / functions)")
        if chance is False:
            return []
        if e["type"] == "minecraft:item":
            lo, hi = self.count(e.get("modifier"), where)
            if not 1 <= lo <= hi <= 99:
                die(f"{where}: drops {lo}..{hi}")
            return [(1.0 if chance is True else chance, self.item_ids[e["name"]], lo, hi)]
        if e["type"] == "minecraft:alternatives":
            if chance is not True or "modifier" in e:
                die(f"{where}: alternatives behind a chance, or with a function of their own")
            out: list = []
            for child in e["children"]:
                out += self.entry(child, where)
                if out and out[-1][0] >= 1.0:
                    break  # this one always matches: the rest is never reached
            return out
        die(f"{where}: loot entry type {e['type']}")

    def explosion_drops(self, block: str, table_name: str | None) -> list[tuple[float, int, int, int]]:
        if not table_name:
            return []
        t = self.read("loot_table", table_name)
        where = f"loot table of {block}"
        if "functions" in t or "modifier" in t:
            die(f"{where}: table-wide functions")
        pools = t.get("pools", [])
        if not pools:
            return []
        if len(pools) != 1 or pools[0].get("rolls") != 1 or pools[0].get("bonus_rolls", 0) != 0 or len(pools[0]["entries"]) != 1:
            die(f"{where}: more than one pool, roll or entry - a weighted choice the reduction does not do")
        if "modifier" in pools[0] or "functions" in pools[0]:
            die(f"{where}: pool-wide functions")
        chance = self.condition(pools[0].get("condition"), where)
        if chance is False:
            return []
        if chance is not True:
            die(f"{where}: a pool behind a chance")
        out = self.entry(pools[0]["entries"][0], where)
        if len(out) > 2 or (len(out) == 2 and out[1][0] < 1.0):
            die(f"{where}: {out} does not fit BlockInfo's two drop candidates")
        return out


# ----------------------------------------------------------------------------- fuel

def fuel_ticks(vanilla, comps: dict, name: str) -> int:
    """AbstractFurnaceBlockEntity.getBurnDuration in a furnace: the item's cooking_fuel burn time.

    26.x: the component names two context providers (data/minecraft/context_int_provider,
    context_float_provider) that ask whether the block is #fast_cooking - a smoker or a blast
    furnace. Evaluated here for the plain furnace, where the speed multiplier must come out as 1:
    the server's furnace has no other.
    """
    fuel = comps.get("minecraft:cooking_fuel")
    if fuel is None:
        return 0

    def read(kind: str, ref):
        path = f"data/minecraft/{kind}/{ref.split(':', 1)[1]}.json"
        if path not in vanilla.names:
            die(f"{name}: cooking_fuel refers to {ref}, which is not in the jar")
        return json.loads(vanilla.zip.read(path))

    def fast(cond) -> bool:
        p = read("predicate", cond) if isinstance(cond, str) else cond
        if p.get("type") != "minecraft:match_block" or "state" in p:
            die(f"{name}: the fast-cooking condition is no longer a plain match_block: {p}")
        blocks = p["blocks"]
        return "minecraft:furnace" in ([blocks] if isinstance(blocks, str) else blocks)

    def value(kind: str, v):
        if isinstance(v, (int, float)):
            return v
        if isinstance(v, str):
            return value(kind, read(kind, v))
        if v.get("type") == "minecraft:conditional":
            return value(kind, v["on_true"] if fast(v["condition"]) else v["on_false"])
        if v.get("type") == "minecraft:div":
            left, right = value(kind, v["left"]), value(kind, v["right"])
            if right == 0:
                die(f"{name}: burn time divides by zero")
            return int(left / right) if kind == "context_int_provider" else left / right
        die(f"{name}: cooking_fuel provider {v} is not one mcgen evaluates")

    ticks = value("context_int_provider", fuel["burn_time"])
    speed = value("context_float_provider", fuel.get("speed_multiplier", 1.0))
    if speed != 1.0:
        die(f"{name}: burns with speed multiplier {speed} in a furnace - the server's furnace has none")
    if ticks != int(ticks) or not 0 < ticks <= 0xFFFF:
        die(f"{name}: burn time {ticks} does not fit ItemInfo::fuel_ticks")
    return int(ticks)


# ----------------------------------------------------------------------------- recipes

def _varint(n: int) -> bytes:
    out = bytearray()
    n &= 0xFFFFFFFF
    while True:
        b = n & 0x7F
        n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def _str(s: str) -> bytes:
    raw = s.encode("utf-8")
    return _varint(len(raw)) + raw


def shrink(pattern: list[str]) -> list[str]:
    """ShapedRecipePattern.shrink: the pattern without its empty border rows and columns."""
    left, right, top, bottom = 1 << 30, 0, 0, 0
    for i, line in enumerate(pattern):
        first = len(line) - len(line.lstrip(" "))
        last = len(line.rstrip(" ")) - 1
        left = min(left, first)
        right = max(right, last)
        if last < 0:
            if top == i:
                top += 1
            bottom += 1
        else:
            bottom = 0
    if len(pattern) == bottom:
        return []
    return [line[left:right + 1] for line in pattern[top:len(pattern) - bottom]]


class Recipes:
    def __init__(self, v: dict, vanilla, static_regs: dict, item_tags: dict[str, list[int]], comp_ids: dict[str, int]):
        self.v = v
        self.vanilla = vanilla
        self.item_ids = {n: e["protocol_id"] for n, e in static_regs["minecraft:item"]["entries"].items()}
        self.effect_ids = {n: e["protocol_id"] for n, e in static_regs["minecraft:mob_effect"]["entries"].items()}
        self.item_tags = item_tags
        self.comp_ids = comp_ids
        self.ingredients: dict[tuple, int] = {}
        self.patches: dict[bytes, int] = {}
        prefix = "data/minecraft/recipe/"
        # RecipeMap.create walks the recipe registry in registration order, and the loader
        # registers what ResourceManager.listResources hands it: a TreeMap by Identifier - path
        # order. The first recipe that matches wins, so the order is part of the data.
        self.raw = sorted((n[len(prefix):-5], json.loads(vanilla.zip.read(n))) for n in vanilla.names if n.startswith(prefix))

    def ingredient_items(self, ing, where: str) -> tuple:
        """Ingredient.CODEC: an item, a tag, or a list of items -> sorted item ids."""
        if isinstance(ing, str):
            if ing.startswith("#"):
                tag = ing[1:]
                if tag not in self.item_tags:
                    die(f"{where}: item tag #{tag} is missing")
                ids = self.item_tags[tag]
            else:
                ids = [self.item_ids[ing]]
        elif isinstance(ing, list) and all(isinstance(i, str) and not i.startswith("#") for i in ing):
            ids = [self.item_ids[i] for i in ing]
        else:
            die(f"{where}: ingredient {ing} is not an item, a tag or a list of items")
        if not ids:
            die(f"{where}: an ingredient nothing matches (Recipe.placementInfo: impossible to place)")
        return tuple(sorted(set(ids)))

    def ingredient(self, ing, where: str) -> int:
        return self.ingredients.setdefault(self.ingredient_items(ing, where), len(self.ingredients))

    def result(self, r, where: str) -> tuple[int, int, int]:
        """ItemStackTemplate -> (item id, count, patch index + 1)."""
        if isinstance(r, str):
            r = {"id": r}
        count = r.get("count", 1)
        if not 1 <= count <= 99:
            die(f"{where}: result count {count}")
        patch = 0
        comps = r.get("components")
        if comps:
            patch = self.patches.setdefault(self.patch(comps, where), len(self.patches)) + 1
        return self.item_ids[r["id"]], count, patch

    def patch(self, comps: dict, where: str) -> bytes:
        """A result's components as DataComponentPatch.DELIMITED_STREAM_CODEC writes them - the
        form a client sends, so that the server reads its own recipes through the same decoders
        (item::Patches::read_untrusted) as everything else that becomes a stack."""
        added = bytearray()
        for name in sorted(comps):
            if name == "minecraft:suspicious_stew_effects":
                # SuspiciousStewEffects.STREAM_CODEC: a list of (effect id, duration); the Codec's
                # default duration is 160.
                value = bytearray(_varint(len(comps[name])))
                for e in comps[name]:
                    value += _varint(self.effect_ids[e["id"]]) + _varint(e.get("duration", 160))
            else:
                die(f"{where}: a recipe result with component {name}, which mcgen does not encode")
            added += _varint(self.comp_ids[name]) + _varint(len(value)) + value
        return bytes(_varint(len(comps)) + _varint(0) + added)

    def build(self) -> dict:
        crafting = []   # (name, kind, a, b, extra, [ingredients], result, count, patch)
        smelting = []   # (name, ingredient, result, count, ticks, experience)
        skipped: dict[str, int] = {}
        sets: dict[str, set[int]] = {k: set() for k in (
            "smithing_base", "smithing_template", "smithing_addition", "furnace_input", "blast_furnace_input",
            "smoker_input", "campfire_input", "brewing_input", "brewing_reagent")}
        cooking_sets = {"minecraft:smelting": "furnace_input", "minecraft:blasting": "blast_furnace_input",
                        "minecraft:smoking": "smoker_input", "minecraft:campfire_cooking": "campfire_input"}

        for name, r in self.raw:
            kind = r["type"]
            where = f"recipe {name}"
            if kind == "minecraft:crafting_shaped":
                rows = shrink(r["pattern"])
                if not rows or len(rows) > 3 or len(rows[0]) > 3:
                    die(f"{where}: pattern {r['pattern']}")
                ings = []
                for row in rows:
                    for ch in row:
                        if ch == " ":
                            ings.append(0xFFFF)
                        elif ch not in r["key"]:
                            die(f"{where}: pattern symbol {ch!r} has no key")
                        else:
                            ings.append(self.ingredient(r["key"][ch], where))
                item, count, patch = self.result(r["result"], where)
                crafting.append((name, "kShaped", len(rows[0]), len(rows), 0, ings, item, count, patch))
            elif kind == "minecraft:crafting_shapeless":
                ings = [self.ingredient(i, where) for i in r["ingredients"]]
                if not 1 <= len(ings) <= 9:
                    die(f"{where}: {len(ings)} ingredients")
                item, count, patch = self.result(r["result"], where)
                crafting.append((name, "kShapeless", len(ings), 0, 0, ings, item, count, patch))
            elif kind == "minecraft:crafting_transmute":
                # TransmuteRecipe: one `input`, some `material`; the result is the input's stack
                # with another item (or its own) and its components kept.
                bounds = r.get("material_count", {"min": 1, "max": 1})  # DEFAULT_MATERIAL_COUNT
                if isinstance(bounds, int):
                    bounds = {"min": bounds, "max": bounds}
                lo, hi = bounds.get("min", 1), bounds.get("max", 8)
                if not 1 <= lo <= hi <= 8:
                    die(f"{where}: material count {bounds}")
                res = r["result"]
                res = {"id": res} if isinstance(res, str) else res
                if "components" in res:
                    die(f"{where}: a transmute result with components of its own")
                item = self.item_ids[res["id"]] if "id" in res else 0  # TransmuteResult: no id = keep the input's item
                count = res.get("count", 1)
                ings = [self.ingredient(r["input"], where), self.ingredient(r["material"], where)]
                crafting.append((name, "kTransmute", lo, hi, 1 if r.get("add_material_count_to_result") else 0,
                                 ings, item, count, 0))
            elif kind in cooking_sets:
                items = self.ingredient_items(r["ingredient"], where)
                sets[cooking_sets[kind]].update(items)
                if kind == "minecraft:smelting":
                    item, count, patch = self.result(r["result"], where)
                    ticks = r.get("cookingtime", 200)
                    if patch or not 1 <= ticks <= 0x7FFF:
                        die(f"{where}: a smelting result with components, or a cooking time of {ticks}")
                    smelting.append((name, self.ingredient(r["ingredient"], where), item, count, ticks, float(r.get("experience", 0.0))))
                else:
                    skipped[kind] = skipped.get(kind, 0) + 1
            else:
                # Property sets are sent whatever the server crafts: a client's screens ask them.
                if kind in ("minecraft:smithing_transform", "minecraft:smithing_trim"):
                    sets["smithing_base"].update(self.ingredient_items(r["base"], where))
                    sets["smithing_addition"].update(self.ingredient_items(r["addition"], where))
                    if "template" in r:
                        sets["smithing_template"].update(self.ingredient_items(r["template"], where))
                elif kind == "minecraft:brewing":
                    sets["brewing_input"].update(self.ingredient_items(r["input"]["item"], where))
                    sets["brewing_reagent"].update(self.ingredient_items(r["reagent"]["item"], where))
                skipped[kind] = skipped.get(kind, 0) + 1
        if not crafting or not smelting:
            die("no crafting or no smelting recipes in the jar: the recipe directory moved")
        return {"crafting": crafting, "smelting": smelting, "skipped": skipped, "sets": sets}

    def emit(self, header: str, packets: dict) -> dict:
        b = self.build()
        ings = sorted(self.ingredients, key=self.ingredients.get)
        if len(ings) >= 0xFFFF:
            die("too many distinct ingredients for 16-bit ids")
        o = [header, """
// Recipes of the vanilla data pack (data/minecraft/recipe), reduced to what the server matches:
// an ingredient is a sorted list of item ids, a result an item, a count and - rarely - components.
// kCrafting is in the order RecipeManager looks recipes up in (by id): the first match wins.
// Include from exactly one .cpp (item/recipes.cpp).
namespace proto::gen::recipe {

inline constexpr unsigned short kNoIngredient = 0xFFFF;  // an empty cell of a shaped pattern
enum Kind : unsigned char { kShaped, kShapeless, kTransmute };

struct Crafting {
    unsigned char kind;
    unsigned char a, b;        // shaped: width, height. shapeless: ingredient count, -. transmute: min, max material count
    unsigned char extra;       // transmute: 1 = the material count is added to the result's count
    unsigned short ing[9];     // shaped: row by row. shapeless: the ingredients. transmute: input, material
    unsigned short result;     // item id; transmute: 0 = the input's own item
    unsigned char count;
    unsigned char patch;       // the result's components: index into kPatchFirst + 1, 0 = none
};
struct Smelting {
    unsigned short ingredient, result;
    unsigned short ticks;      // cookingtime
    unsigned char count;
    float experience;
};

"""]
        first = [0]
        for items in ings:
            first.append(first[-1] + len(items))
        o.append(f"// Ingredient i accepts the items kIngredientItems[kIngredientFirst[i] .. kIngredientFirst[i + 1]), ascending.\n"
                 f"inline constexpr unsigned kIngredientCount = {len(ings)};\n"
                 f"inline constexpr unsigned short kIngredientFirst[{len(first)}] = {{\n    ")
        o.append(", ".join(str(x) for x in first))
        o.append(f"\n}};\ninline constexpr unsigned short kIngredientItems[{first[-1]}] = {{\n    ")
        o.append(", ".join(str(i) for items in ings for i in items))
        o.append("\n};\n\n")
        if len(self.patches) > 254:
            die("too many distinct result patches for Crafting::patch")
        patches = sorted(self.patches, key=self.patches.get)
        pfirst = [0]
        for p in patches:
            pfirst.append(pfirst[-1] + len(p))
        o.append("// Result components, each as DataComponentPatch.DELIMITED_STREAM_CODEC writes a patch (the\n"
                 "// form item::Patches::read_untrusted reads): kPatchBytes[kPatchFirst[i] .. kPatchFirst[i + 1]).\n"
                 f"inline constexpr unsigned kPatchCount = {len(patches)};\n"
                 f"inline constexpr unsigned short kPatchFirst[{len(pfirst)}] = {{" + ", ".join(str(x) for x in pfirst) + "};\n"
                 f"inline constexpr unsigned char kPatchBytes[{max(pfirst[-1], 1)}] = {{\n    ")
        o.append(", ".join(str(x) for p in patches for x in p) or "0")
        o.append("\n};\n\n")
        o.append(f"inline constexpr unsigned kCraftingCount = {len(b['crafting'])};\n"
                 f"inline constexpr Crafting kCrafting[kCraftingCount] = {{\n")
        for name, kind, a, bb, extra, ing, item, count, patch in b["crafting"]:
            cells = ", ".join("kNoIngredient" if i == 0xFFFF else str(i) for i in ing)
            o.append(f"    {{{kind}, {a}, {bb}, {extra}, {{{cells}}}, {item}, {count}, {patch}}},  // {name}\n")
        o.append(f"}};\n\ninline constexpr unsigned kSmeltingCount = {len(b['smelting'])};\n"
                 f"inline constexpr Smelting kSmelting[kSmeltingCount] = {{\n")
        for name, ing, item, count, ticks, xp in b["smelting"]:
            o.append(f"    {{{ing}, {item}, {ticks}, {count}, {_cfloat32(xp)}}},  // {name}\n")
        o.append("};\n\n}  // namespace proto::gen::recipe\n")

        # ClientboundUpdateRecipesPacket: Map<ResourceKey<RecipePropertySet>, RecipePropertySet>
        # (count, then key + a list of item ids), then the stonecutter recipes - none: no
        # stonecutter can be placed yet, and its entries carry slot displays nobody encodes here.
        pid = packets["play"]["clientbound"]["minecraft:update_recipes"]["protocol_id"]
        body = bytearray(_varint(pid))
        body += _varint(len(b["sets"]))
        for key in sorted(b["sets"]):
            body += _str(f"minecraft:{key}") + _varint(len(b["sets"][key]))
            for i in sorted(b["sets"][key]):
                body += _varint(i)
        body += _varint(0)
        stats = {"crafting": len(b["crafting"]), "smelting": len(b["smelting"]), "recipes_skipped": sum(b["skipped"].values())}
        return {"recipes.hpp": "".join(o), "update_recipes.bin": bytes(body), "_stats": stats}


def _cfloat32(x: float) -> str:
    f = struct.unpack("f", struct.pack("f", x))[0]
    for digits in range(1, 10):
        r = f"{x:.{digits}g}"
        if struct.unpack("f", struct.pack("f", float(r)))[0] == f:
            return (r if ("." in r or "e" in r) else r + ".0") + "f"
    die(f"cannot print {x!r} as a float")


# ----------------------------------------------------------------------------- static ids

def emit_static_ids(header: str, static_regs: dict) -> str:
    def ids(reg: str) -> dict[str, int]:
        return {n.split(":", 1)[1]: e["protocol_id"] for n, e in static_regs[f"minecraft:{reg}"]["entries"].items()}

    def ident(name: str) -> str:
        return "k" + "".join(p[:1].upper() + p[1:] for p in name.replace(".", "_").split("_") if p)

    o = [header, "\n#include <string_view>\n\n// Ids of static registries (Mojang's registries.json) that the server names in packets.\n"
                 "namespace proto::gen {\n"]
    for ns, reg, names in (("menu", "menu", None), ("block_entity_type", "block_entity_type", None),
                           ("sound", "sound_event", SOUNDS), ("particle", "particle_type", PARTICLES)):
        table = ids(reg)
        o.append(f"\nnamespace {ns} {{  // minecraft:{reg}\n")
        for name in sorted(table, key=table.get) if names is None else names:
            if name not in table:
                die(f"minecraft:{reg} has no entry {name}: it was renamed, find what the server should send now")
            o.append(f"inline constexpr unsigned {ident(name)} = {table[name]};\n")
        if names is None:
            o.append(f"inline constexpr unsigned kCount = {len(table)};\n")
        if ns == "block_entity_type":  # by id: what a saved block entity names its type by ("id": "minecraft:chest")
            if sorted(table.values()) != list(range(len(table))):
                die("minecraft:block_entity_type: protocol ids are not 0..n-1")
            o.append("inline constexpr std::string_view kNames[kCount] = {\n")
            o.extend(f'    "minecraft:{name}",\n' for name in sorted(table, key=table.get))
            o.append("};\n")
        o.append(f"}}  // namespace {ns}\n")
    o.append("\n}  // namespace proto::gen\n")
    return "".join(o)
