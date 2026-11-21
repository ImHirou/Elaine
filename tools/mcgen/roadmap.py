#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes docs/blocks-roadmap.md: every block of the game, by the class that gives it its
behaviour, as a checklist of what the server simulates and what is still to be ported.

    python3 tools/mcgen/roadmap.py            # rewrite docs/blocks-roadmap.md
    python3 tools/mcgen/roadmap.py --check    # fail if the file is stale

The ticks are not kept by hand. A block is done when the generator makes it placeable
(gen.py placement_kind, gen_game.py PORTED_BLOCKS) - which it only does when every hook vanilla
runs for the block is one the server implements. So porting a block class is: write it, enter it
in PORTED_BLOCKS, `mcgen.py generate`, and run this. What is curated here is the order: the
milestones below group the block classes by the system they wait for.

Reads the caches of `mcgen.py blockdata` and `gamedata`; runs nothing.
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen  # noqa: E402
import gen_game  # noqa: E402
import mcgen  # noqa: E402
from mcgen import ROOT, die, vdir, version_info  # noqa: E402

OUT = ROOT / "docs" / "blocks-roadmap.md"

# Milestones in dependency order: (title, what has to exist first, block classes by regex).
# A class goes to the first milestone that claims it; one that nobody claims stops the script.
MILESTONES = [
    ("Plain blocks", "Nothing: a state that is stored, lit and sent. Done since the first world.",
     r"^(Block|RotatedPillarBlock|DropExperienceBlock|StainedGlassBlock|TransparentBlock|HalfTransparentBlock|TintedGlassBlock|"
     r"InfestedBlock|InfestedRotatedPillarBlock|NetherrackBlock|"
     r"BonemealableFeaturePlacerBlock|IceBlock|SoulSandBlock|SlimeBlock|HoneyBlock|HayBlock|MagmaBlock|"
     r"WebBlock|StructureVoidBlock|MudBlock|RootedDirtBlock|AmethystBlock|CryingObsidianBlock|PoweredBlock|"
     r"AirBlock)$"),
    ("Block entities and menus: the first four", "Menus over containers, recipes, a furnace's tick, primed TNT and explosions.",
     r"^(ChestBlock|FurnaceBlock|CraftingTableBlock|TntBlock)$"),
    ("Shapes: a state from how the block is placed and what stands beside it",
     "BlockPlaceContext in full (the clicked face and where on it, the player's look, replacing a block with more of itself). "
     "`updateShape` for every neighbour change is there (`Game::update_shape`; podzol under snow is the first). No ticking.",
     r"^(SnowyBlock|SlabBlock|StairBlock|WallBlock|FenceBlock|IronBarsBlock|StainedGlassPaneBlock|GlazedTerracottaBlock|ChainBlock|"
     r"LanternBlock|EndRodBlock|HugeMushroomBlock|CarvedPumpkinBlock|EquipableCarvedPumpkinBlock|BarrierBlock|LightBlock|PathBlock|"
     r"MangroveRootsBlock|WaterloggedTransparentBlock|HeavyCoreBlock|PointedDripstoneBlock|RodBlock|DirectionalBlock|"
     r"MultifaceBlock|GlowLichenBlock|SculkVeinBlock|MossyCarpetBlock|HangingMossBlock|CopperGrateBlock)$"),
    ("Support: blocks that need something to stand on or hang from",
     "`canSurvive` and the break-and-drop when the support goes are there (`Game::can_survive`, `break_block`: torches, amethyst); "
     "each class is its own rule of placement and its own drops. Survival drops need loot tables with a tool.",
     r"^(TorchBlock|WallTorchBlock|LadderBlock|.*CarpetBlock|FlowerBlock|TallFlowerBlock|DoublePlantBlock|TallGrassBlock|BushBlock|"
     r"DryVegetationBlock|ShortDryGrassBlock|TallDryGrassBlock|MushroomBlock|ShelfMushroomBlock|WitherRoseBlock|EyeblossomBlock|"
     r"FlowerBedBlock|LeafLitterBlock|FireflyBushBlock|NetherRootsBlock|NetherSproutsBlock|RootsBlock|AzaleaBlock|"
     r"AmethystClusterBlock|SnowLayerBlock|LilyPadBlock|SporeBlossomBlock|HangingRootsBlock|PinkPetalsBlock|CactusFlowerBlock|"
     r"SmallDripleafBlock|BigDripleafBlock|BigDripleafStemBlock|FrogspawnBlock|DecoratedPotBlock)$"),
    ("Scheduled ticks and falling blocks", "The tick queue (`world::LevelTicks`) and `FallingBlockEntity` are there: sand, gravel, "
     "concrete powder. What is left hurts what it lands on (anvils: damage), teleports (the dragon egg) or has a block entity.",
     r"^(SandBlock|ColoredFallingBlock|ConcretePowderBlock|AnvilBlock|DragonEggBlock|ScaffoldingBlock|FallingBlock|"
     r"BrushableBlock)$"),
    ("Fluids", "Water and lava: flowing (`FlowingFluid.tick`), waterlogging (`getFluidState`, `SimpleWaterloggedBlock`), buckets.",
     r"^(LiquidBlock|SpongeBlock|WetSpongeBlock|KelpBlock|KelpPlantBlock|SeagrassBlock|TallSeagrassBlock|SeaPickleBlock|.*Coral.*|"
     r"BubbleColumnBlock|FrostedIceBlock|ConduitBlock|.*CauldronBlock|PowderSnowBlock|MangrovePropaguleBlock)$"),
    ("Random ticks: growth, spread and decay", "Random block ticks are there, and sections whose ticks could do nothing are passed by: "
     "a class that is ported says in `state::kRandom` when its tick can act (`RANDOM_TICK_KINDS` in gen_game.py; docs/architecture.md, "
     "\"Block updates\"). Still needed: bone meal, tree and mushroom features for what grows into a structure, fire's spread.",
     r"^(GrassBlock|MyceliumBlock|NyliumBlock|BuddingAmethystBlock|CropBlock|CarrotBlock|PotatoBlock|BeetrootBlock|TorchflowerCropBlock|PitcherCropBlock|StemBlock|AttachedStemBlock|"
     r"PumpkinBlock|FarmlandBlock|SaplingBlock|.*LeavesBlock|VineBlock|CactusBlock|SugarCaneBlock|BambooSaplingBlock|"
     r"BambooStalkBlock|CocoaBlock|NetherWartBlock|SweetBerryBushBlock|.*VinesBlock|.*VinesPlantBlock|CaveVines.*|"
     r"ChorusPlantBlock|ChorusFlowerBlock|NetherFungusBlock|FireBlock|SoulFireBlock|Weathering.*|TurtleEggBlock|SnifferEggBlock|"
     r"SulfurSpikeBlock|"
     r"DriedGhastBlock|MossBlock|SculkBlock|SculkCatalystBlock|RedStoneOreBlock)$"),
    ("Blocks that answer a click without a menu", "`useItemOn` / `useWithoutItem` beyond opening a screen: the item in hand changes, "
     "the block changes state, a sound plays.",
     r"^(DoorBlock|TrapDoorBlock|FenceGateBlock|CakeBlock|CandleBlock|CandleCakeBlock|FlowerPotBlock|ComposterBlock|"
     r"RespawnAnchorBlock|EndPortalFrameBlock|CopperBulbBlock|LightningRodBlock|StrawBedBlock)$"),
    ("Menus without a block entity", "The menu classes and their recipes: `AnvilMenu`, `GrindstoneMenu`, `LoomMenu`, "
     "`CartographyTableMenu`, `SmithingMenu`, `StonecutterMenu` (and the stonecutter recipes in `update_recipes`).",
     r"^(LoomBlock|CartographyTableBlock|GrindstoneBlock|SmithingTableBlock|StonecutterBlock|FletchingTableBlock)$"),
    ("Block entities: containers", "The chest's machinery again - a block entity with stacks, a menu, what the client is told - plus "
     "each one's own tick or rule: hoppers move items, shulker boxes keep theirs when broken (stacks inside stacks).",
     r"^(BarrelBlock|TrappedChestBlock|EnderChestBlock|ShulkerBoxBlock|HopperBlock|DropperBlock|DispenserBlock|BlastFurnaceBlock|"
     r"SmokerBlock|BrewingStandBlock|CampfireBlock|ChiseledBookShelfBlock|ShelfBlock|CrafterBlock|.*CopperChestBlock|"
     r"LecternBlock|JukeboxBlock)$"),
    ("Block entities: what a client is shown", "Block entities whose data travels to the client: `getUpdateTag` in the chunk packet, "
     "`block_entity_data` on change (`nbt::Writer` is there), text through `text::transcode`.",
     r"^(StandingSignBlock|WallSignBlock|CeilingHangingSignBlock|WallHangingSignBlock|BannerBlock|WallBannerBlock|BedBlock|"
     r".*SkullBlock|.*HeadBlock|BeaconBlock|BellBlock|EnchantingTableBlock|BeehiveBlock|.*CopperGolemStatueBlock|"
     r"CreakingHeartBlock|SculkSensorBlock|CalibratedSculkSensorBlock|SculkShriekerBlock|VaultBlock|TrialSpawnerBlock|SpawnerBlock|"
     r"PotentSulfurBlock)$"),
    ("Redstone", "Signals (`SignalGetter`) beyond \"is there a block of redstone beside me\". Neighbour updates in vanilla's order and "
     "scheduled ticks are there; TNT and the lamp ask for a neighbour signal, a block of redstone is the one source so far.",
     r"^(RedstoneWireBlock|RedstoneTorchBlock|RedstoneWallTorchBlock|RepeaterBlock|ComparatorBlock|LeverBlock|ButtonBlock|"
     r"PressurePlateBlock|WeightedPressurePlateBlock|ObserverBlock|PistonBaseBlock|PistonHeadBlock|MovingPistonBlock|NoteBlock|"
     r"TargetBlock|RedstoneLampBlock|TripWireHookBlock|TripWireBlock|DaylightDetectorBlock|RailBlock|PoweredRailBlock|"
     r"DetectorRailBlock)$"),
    ("Portals and technical blocks", "Other dimensions; operators and commands; structures.",
     r"^(NetherPortalBlock|EndPortalBlock|EndGatewayBlock|CommandBlock|StructureBlock|JigsawBlock|TestBlock|TestInstanceBlock)$"),
]

# Systems every family leans on, beyond its own class: the part of the roadmap that is not a block.
SYSTEMS = [
    ("x", "Block entities: stored per chunk, listed in the chunk packet, created and removed with their block"),
    ("x", "Menus on blocks: `open_screen`, any number of slots, data slots, closing when out of reach"),
    ("x", "Crafting: shaped, shapeless and transmute recipes; smelting; fuel"),
    ("x", "Entity physics for one kind (primed TNT): gravity, collision with blocks, bounce, speed factor"),
    ("x", "Explosions: rays, exposure, block drops without a tool, chain reactions"),
    (" ", "Survival: mining over time, tools, block drops through loot tables with a tool and enchantments"),
    ("x", "Neighbour updates: `neighborChanged` and `updateShape` in vanilla's order (`CollectingNeighborUpdater`), blocks that "
          "break when what holds them goes"),
    ("x", "Scheduled block ticks (`LevelTicks`), saved with their chunk"),
    ("x", "Random block ticks, at the simulation distance"),
    ("x", "Falling blocks (`FallingBlockEntity`)"),
    ("x", "The particles and the sound of a block breaking, for the others (level event 2001)"),
    (" ", "The sound of a block being placed (`sound`)"),
    (" ", "Fluids: water and lava that flow, fluid ticks, waterlogged blocks that ask for them"),
    (" ", "Redstone signals beyond the block of redstone"),
    (" ", "The time of day and the weather: light that changes with them, rain and snow on blocks (`tickPrecipitation`)"),
    (" ", "Experience orbs: a furnace's stored experience, ores blown up by a player's TNT"),
    ("x", "Item physics: dropped and spilled items fall and are pushed by explosions"),
    (" ", "What blocks do to entities inside them (`entityInside`): cobweb, honey, bubble columns, portals, pressure plates"),
    (" ", "The recipe book (`recipe_book_add`, `place_recipe`) and recipe unlocking"),
    (" ", "Special crafting recipes: dyed armour, banner and book copies, fireworks, map extending, repairing, shield "
          "decoration, tipped arrows, decorated pots"),
    (" ", "Block entity names and locks (a chest named on an anvil), saved block entity data on placed items (ctrl + pick block)"),
    (" ", "Comparator output of containers (`getAnalogOutputSignal`)"),
    ("x", "Persistence of block entities with their chunk"),
]


def build() -> str:
    version = mcgen.DEFAULT_VERSION
    v = version_info(version)
    d = vdir(version)
    ext = d / "extract" / "blockdata.json"
    if not ext.exists():
        die("extract/blockdata.json is missing - run: mcgen.py blockdata")
    data = json.loads(ext.read_text())
    game = gen_game.load_gamedata(d)
    items = {i["name"]: i for i in data["items"]}

    classes: dict[str, list] = {}
    for b in sorted(data["blocks"], key=lambda b: b["id"]):
        gb = game["blocks"][b["name"]]
        item = items.get(b["item"]) if b["item"] != "minecraft:air" else None
        # Air is what is there when no block is: nothing to place, nothing to port.
        done = gen.placement_kind(b, item, gen_game.ported_block(gb)) != 0 or gb["class"][0] == "AirBlock"
        classes.setdefault(gb["class"][0], []).append((b["name"].split(":", 1)[1], done, gb))

    claimed: dict[str, int] = {}
    for cls in classes:
        for i, (_, _, rx) in enumerate(MILESTONES):
            if re.match(rx, cls):
                claimed[cls] = i
                break
    lost = sorted(set(classes) - set(claimed))
    if lost:
        die(f"block classes no milestone of tools/mcgen/roadmap.py claims: {lost}")

    total = sum(len(v) for v in classes.values())
    done_total = sum(1 for v in classes.values() for _, done, _ in v if done)
    out = [f"""# Blocks: what is simulated, and what is next

Minecraft {v['id']} has {total} blocks in {len(classes)} block classes. The server holds a block only if it
simulates everything vanilla would do with it - so the list of what can be **placed** is the list
of what is **done**: {done_total} blocks today. This file is that list, block class by block class,
in the order the missing systems are meant to be built.

**Generated** by `tools/mcgen/roadmap.py` from the jar's block classes (`mcgen.py gamedata`) and the
generator's own rule for "placeable" (`tools/mcgen/gen.py`: `placement_kind`). Do not tick boxes
by hand: port the class, enter it in `PORTED_BLOCKS` (`tools/mcgen/gen_game.py`) or teach
`placement_kind` its rule, run `mcgen.py generate`, then `roadmap.py`.

How to read an entry: `- [ ] **Class** (blocks) - the BlockBehaviour hooks its class chain
overrides`, then the blocks. The hooks are the work: each is a method to read in the jar
(`mcgen.py pseudo net.minecraft.world.level.block.<Class> --method <hook>`) and to port or to
prove irrelevant. A class marked *block entity* also needs its `BlockEntity` class ported.
A box is ticked when every block of the class can be placed; `(n of m)` says how far it is.

Some blocks that are ticked still do less than in vanilla, because a system around them is
missing; those gaps are listed under "Systems" below and in docs/architecture.md, never
silently: ice does not melt (it would leave water), concrete powder meets no water that
flows, ores give no experience, an amethyst block does not chime.

## Systems

What the block classes lean on. A milestone below is workable once the systems it names are.

"""]
    for mark, text in SYSTEMS:
        out.append(f"- [{mark}] {text}\n")

    # Hooks that say nothing about behaviour on a server: looks, sounds, shapes that are data.
    quiet = {"createBlockStateDefinition", "getShape", "getCollisionShape", "getVisualShape", "getOcclusionShape",
             "getBlockSupportShape", "getInteractionShape", "getRenderShape", "getShadeBrightness", "skipRendering",
             "propagatesSkylightDown", "animateTick", "getSeed", "mirror", "rotate", "isPathfindable", "codec",
             "getShapeForEachState(Function)", "getShapeForEachState(Function,Property;)", "getLightDampening",
             "useShapeForLightOcclusion", "getSoundType", "getMaxHorizontalOffset", "getMaxVerticalOffset", "hasDynamicShape",
             "isCollisionShapeFullBlock", "spawnDestroyParticles", "getName", "asItem", "properties", "getEntityInsideCollisionShape"}
    for i, (title, needs, _) in enumerate(MILESTONES):
        mine = sorted((c for c in classes if claimed[c] == i), key=lambda c: (-len(classes[c]), c))
        n = sum(len(classes[c]) for c in mine)
        done = sum(1 for c in mine for _, ok, _ in classes[c] if ok)
        out.append(f"\n## {i}. {title} ({done} of {n} blocks)\n\nNeeds: {needs}\n\n")
        for cls in mine:
            blocks = classes[cls]
            ok = sum(1 for _, okay, _ in blocks if okay)
            gb = blocks[0][2]
            hooks = sorted(h for b in blocks for h in b[2]["hooks"] if h not in quiet)
            hooks = sorted(set(hooks))
            part = "" if ok in (0, len(blocks)) else f", {ok} of {len(blocks)} done"
            entity = ", *block entity*" if gb.get("block_entity_type") else ""
            out.append(f"- [{'x' if ok == len(blocks) else ' '}] **{cls}** ({len(blocks)}{part}{entity})"
                       f"{' - ' + ', '.join(f'`{h}`' for h in hooks) if hooks else ''}  \n"
                       f"  {', '.join(name for name, _, _ in blocks)}\n")
    return "".join(out)


def main() -> int:
    text = build()
    if "--check" in sys.argv:
        if not OUT.exists() or OUT.read_text() != text:
            die(f"{OUT.relative_to(ROOT)} is stale - run: python3 tools/mcgen/roadmap.py")
        print("roadmap: up to date")
        return 0
    OUT.write_text(text)
    print(f"roadmap: wrote {OUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
