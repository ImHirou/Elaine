# SPDX-License-Identifier: GPL-3.0-or-later
"""mcgen code generation, the game-data half: what blocks and items *do*, as far as the server
simulates it. Called from gen.py; everything here reads the jar's data pack and the caches of
`mcgen.py reports` / `gamedata`, and runs nothing.

Emitted:
  recipes.hpp         crafting (shaped, shapeless, transmute) and cooking recipes, ingredients as
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
#   item      (optional) the class of the item that places it, when that is not plain BlockItem
_WEATHERS = {"isRandomlyTicking", "randomTick"}
_SLAB = {"canBeReplaced(BlockState,BlockPlaceContext)", "getFluidState", "getShape", "isPathfindable", "updateShape",
         "useShapeForLightOcclusion", "createBlockStateDefinition", "getStateForPlacement"}
_STAIRS = {"getFluidState", "getShape", "isPathfindable", "mirror", "rotate", "updateShape", "useShapeForLightOcclusion",
           "createBlockStateDefinition", "getExplosionResistance", "getStateForPlacement"}
_CROSS = {"getCollisionShape", "getFluidState", "getShape", "getVisualShape", "isPathfindable", "mirror", "propagatesSkylightDown",
          "rotate", "updateShape", "createBlockStateDefinition", "getStateForPlacement"}  # CrossCollisionBlock and what both add
_CHAIN = {"getFluidState", "getShape", "isPathfindable", "rotate", "updateShape", "createBlockStateDefinition", "getStateForPlacement"}
_LANTERN = {"canSurvive", "getFluidState", "getShape", "isPathfindable", "updateShape", "createBlockStateDefinition",
            "getStateForPlacement"}
_GRATE = {"getFluidState", "getShadeBrightness", "getVisualShape", "propagatesSkylightDown", "skipRendering", "updateShape",
          "createBlockStateDefinition", "getStateForPlacement"}
_CHEST = {"affectNeighborsAfterRemoval", "getAnalogOutputSignal", "getFluidState", "getMenuProvider", "getShape",
          "hasAnalogOutputSignal", "isPathfindable", "mirror", "rotate", "tick", "triggerEvent", "updateShape",
          "useWithoutItem", "createBlockStateDefinition", "getStateForPlacement"}
_FURNACE = {"affectNeighborsAfterRemoval", "getAnalogOutputSignal", "getMenuProvider", "hasAnalogOutputSignal", "mirror",
            "rotate", "triggerEvent", "useWithoutItem", "animateTick", "createBlockStateDefinition", "getStateForPlacement"}
_PLANT = {"canSurvive", "isPathfindable", "propagatesSkylightDown", "updateShape"}  # VegetationBlock
_SEGMENTS = _PLANT | {"canBeReplaced(BlockState,BlockPlaceContext)", "getShape", "mirror", "rotate",
                      "createBlockStateDefinition", "getStateForPlacement"}
_SIGN = {"canSurvive", "getFluidState", "getMenuProvider", "getShape", "mirror", "rotate", "triggerEvent", "updateShape",
         "useItemOn", "useWithoutItem", "createBlockStateDefinition", "getStateForPlacement", "isPossibleToRespawnInThis",
         "setPlacedBy"}
PORTED_BLOCKS = {
    "ChestBlock": ("kChest", "kPlaceChest", _CHEST),
    # A copper chest joins any other copper chest, and the pair is then of one block: the less
    # weathered of the two. The unwaxed ones weather (the left half, and the right one after it).
    "CopperChestBlock": ("kChest", "kPlaceChest", _CHEST | {"shouldChangedStateKeepBlockEntity"}),
    "WeatheringCopperChestBlock": ("kChest", "kPlaceChest", _CHEST | {"shouldChangedStateKeepBlockEntity"} | _WEATHERS),
    # One block entity (AbstractFurnaceBlockEntity) for the three: which recipes it cooks, and
    # how fast its fuel burns, goes by its block entity type.
    "FurnaceBlock": ("kFurnace", "kPlaceFacing", _FURNACE),
    "SmokerBlock": ("kFurnace", "kPlaceFacing", _FURNACE),
    "BlastFurnaceBlock": ("kFurnace", "kPlaceFacing", _FURNACE),
    # A chest's 27 stacks behind a ChestMenu; `open` instead of a lid.
    "BarrelBlock": ("kBarrel", "kPlaceBarrel", {
        "affectNeighborsAfterRemoval", "getAnalogOutputSignal", "getMenuProvider", "hasAnalogOutputSignal", "mirror",
        "rotate", "tick", "triggerEvent", "useWithoutItem", "createBlockStateDefinition", "getStateForPlacement"}),
    "CraftingTableBlock": ("kCraftingTable", "kPlaceDefault", {"getMenuProvider", "useWithoutItem"}),
    "TntBlock": ("kTnt", "kPlaceDefault", {
        "neighborChanged", "onPlace", "onProjectileHit", "useItemOn", "createBlockStateDefinition", "dropFromExplosion",
        "playerWillDestroy", "wasExploded"}),
    # A constant redstone signal on every side (redstone_block). Placeable by the general rule;
    # the kind is what TNT's "has a neighbour signal" looks for.
    "PoweredBlock": ("kPowered", None, {"isSignalSource", "ownSignal"}),
    # `snowy` follows what lies on it (podzol); grass and mycelium also spread and die (randomTick).
    "SnowyBlock": ("kSnowy", "kPlaceSnowy", {"updateShape", "createBlockStateDefinition", "getStateForPlacement"}),
    "GrassBlock": ("kSpreading", "kPlaceSnowy", {"randomTick", "updateShape", "createBlockStateDefinition", "getStateForPlacement"}),
    "MyceliumBlock": ("kSpreading", "kPlaceSnowy", {
        "randomTick", "updateShape", "createBlockStateDefinition", "getStateForPlacement", "animateTick"}),
    "NyliumBlock": ("kNylium", "kPlaceDefault", {"randomTick"}),
    "WeatheringCopperFullBlock": ("kPlain", "kPlaceDefault", {"isRandomlyTicking", "randomTick"}),
    "BuddingAmethystBlock": ("kBuddingAmethyst", "kPlaceDefault", {"onProjectileHit", "randomTick"}),
    "AmethystClusterBlock": ("kAmethystCluster", "kPlaceCluster", {
        "canSurvive", "getFluidState", "getShape", "mirror", "onProjectileHit", "rotate", "updateShape",
        "createBlockStateDefinition", "getStateForPlacement"}),
    "TorchBlock": ("kTorch", "kPlaceTorch", {"canSurvive", "getShape", "updateShape", "animateTick"}, "StandingAndWallBlockItem"),
    "WallTorchBlock": ("kWallTorch", "kPlaceTorch", {
        "canSurvive", "getShape", "mirror", "rotate", "updateShape", "animateTick", "createBlockStateDefinition",
        "getStateForPlacement"}, "StandingAndWallBlockItem"),
    "RedstoneLampBlock": ("kLamp", "kPlaceLamp", {"neighborChanged", "tick", "createBlockStateDefinition", "getStateForPlacement"}),
    "SandBlock": ("kFalling", "kPlaceDefault", {"onPlace", "tick", "updateShape", "animateTick"}),
    "ColoredFallingBlock": ("kFalling", "kPlaceDefault", {"onPlace", "tick", "updateShape", "animateTick"}),
    "ConcretePowderBlock": ("kConcretePowder", "kPlaceConcretePowder", {
        "onPlace", "tick", "updateShape", "animateTick", "getStateForPlacement"}),
    # ---- signs: a block entity that holds text (SignBlockEntity; server/signs.cpp). Standing and on a
    # wall they are one item's two blocks, as a torch's; hanging from a ceiling and from a wall too.
    "StandingSignBlock": ("kSign", "kPlaceSign", _SIGN, "StandingAndWallBlockItem"),
    "WallSignBlock": ("kWallSign", "kPlaceSign", _SIGN, "StandingAndWallBlockItem"),
    "CeilingHangingSignBlock": ("kHangingSign", "kPlaceHangingSign", _SIGN | {"getBlockSupportShape"}, "HangingSignItem"),
    "WallHangingSignBlock": ("kWallHangingSign", "kPlaceHangingSign",
                             (_SIGN - {"canSurvive"}) | {"getBlockSupportShape", "getCollisionShape", "isPathfindable"},
                             "HangingSignItem"),
    # ---- shapes: a state from how the block is placed and what stands beside it (server/block_shapes.cpp).
    # `waterlogged` is in most of them: getFluidState answers water, and updateShape asks for the
    # water's tick. A class whose updateShape does nothing else has a shape hook for its wet states
    # alone (NO_SHAPE_HOOK).
    "SlabBlock": ("kSlab", "kPlaceSlab", _SLAB),
    "StairBlock": ("kStairs", "kPlaceStairs", _STAIRS),
    "FenceBlock": ("kFence", "kPlaceCross", _CROSS | {"getOcclusionShape", "useWithoutItem"}),
    "IronBarsBlock": ("kBars", "kPlaceCross", _CROSS | {"skipRendering"}),
    "StainedGlassPaneBlock": ("kBars", "kPlaceCross", _CROSS | {"skipRendering"}),
    "WallBlock": ("kWall", "kPlaceWall", {
        "getCollisionShape", "getFluidState", "getShape", "isPathfindable", "mirror", "propagatesSkylightDown", "rotate",
        "updateShape", "createBlockStateDefinition", "getStateForPlacement"}),
    "GlazedTerracottaBlock": ("kPlain", "kPlaceHorizontal", {"mirror", "rotate", "createBlockStateDefinition", "getStateForPlacement"}),
    "ChainBlock": ("kPlain", "kPlaceChain", _CHAIN),
    "LanternBlock": ("kLantern", "kPlaceLantern", _LANTERN),
    "EndRodBlock": ("kPlain", "kPlaceEndRod", {
        "getShape", "isPathfindable", "mirror", "rotate", "animateTick", "createBlockStateDefinition", "getStateForPlacement"}),
    "WaterloggedTransparentBlock": ("kPlain", "kPlaceWaterlogged", _GRATE),
    "HugeMushroomBlock": ("kMushroom", "kPlaceMushroom", {
        "mirror", "rotate", "updateShape", "createBlockStateDefinition", "getStateForPlacement"}),
    "BarrierBlock": ("kPlain", "kPlaceWaterlogged", {
        "getFluidState", "getRenderShape", "getShadeBrightness", "propagatesSkylightDown", "updateShape",
        "createBlockStateDefinition", "getStateForPlacement"}),
    "HeavyCoreBlock": ("kPlain", "kPlaceWaterlogged", {
        "getFluidState", "getShape", "isPathfindable", "updateShape", "createBlockStateDefinition", "getStateForPlacement"}),
    "MangroveRootsBlock": ("kPlain", "kPlaceWaterlogged", {
        "getFluidState", "skipRendering", "updateShape", "createBlockStateDefinition", "getStateForPlacement"}),
    # ...and the copper of them that weathers: the same class with ChangeOverTimeBlock's random tick.
    "WeatheringCopperSlabBlock": ("kSlab", "kPlaceSlab", _SLAB | _WEATHERS),
    "WeatheringCopperStairBlock": ("kStairs", "kPlaceStairs", _STAIRS | _WEATHERS),
    "WeatheringCopperBarsBlock": ("kBars", "kPlaceCross", _CROSS | {"skipRendering"} | _WEATHERS),
    "WeatheringCopperChainBlock": ("kPlain", "kPlaceChain", _CHAIN | _WEATHERS),
    "WeatheringLanternBlock": ("kLantern", "kPlaceLantern", _LANTERN | _WEATHERS),
    "WeatheringCopperGrateBlock": ("kPlain", "kPlaceWaterlogged", _GRATE | _WEATHERS),
    # ---- plants: VegetationBlock - they stand on what their class says they may (PLANT_SUPPORT),
    # and break when that goes. Nothing grows: bone meal is an item's business, and there is none.
    "FlowerBlock": ("kPlant", "kPlaceDefault", _PLANT | {"getShape"}),
    "TallGrassBlock": ("kPlant", "kPlaceDefault", _PLANT | {"getShape"}),
    "BushBlock": ("kPlant", "kPlaceDefault", _PLANT | {"getShape"}),
    "FireflyBushBlock": ("kPlant", "kPlaceDefault", _PLANT | {"animateTick"}),
    "DryVegetationBlock": ("kPlant", "kPlaceDefault", _PLANT | {"getShape", "animateTick"}),
    "ShortDryGrassBlock": ("kPlant", "kPlaceDefault", _PLANT | {"getShape", "animateTick"}),
    "TallDryGrassBlock": ("kPlant", "kPlaceDefault", _PLANT | {"getShape", "animateTick"}),
    "AzaleaBlock": ("kPlant", "kPlaceDefault", _PLANT | {"getShape"}),
    "NetherSproutsBlock": ("kPlant", "kPlaceDefault", _PLANT | {"getShape"}),
    "NetherFungusBlock": ("kPlant", "kPlaceDefault", _PLANT | {"getShape"}),
    "NetherRootsBlock": ("kPlant", "kPlaceDefault", _PLANT | {"getShape"}),
    "CactusFlowerBlock": ("kPlant", "kPlaceDefault", _PLANT | {"getShape"}),
    # SegmentableBlock: up to four of it in one block, each placed onto the ones that are there.
    "FlowerBedBlock": ("kSegmented", "kPlaceSegment", _SEGMENTS),
    "LeafLitterBlock": ("kSegmented", "kPlaceSegment", _SEGMENTS),
    # ---- fluids: the block a fluid is by itself (server/fluids.cpp; the flow is world/fluid.*). No
    # item places it - a bucket empties the fluid, /setblock sets the block (ITEMLESS_PORTED).
    # Its block tick is the bubble column's, of which there is none; its random tick is lava's
    # (RANDOM_TICK_GAPS); its collision shape is empty for whatever cannot stand on a fluid.
    "LiquidBlock": ("kLiquid", None, {
        "getCollisionShape", "getDrops", "getFluidState", "getRenderShape", "getShape", "isPathfindable", "isRandomlyTicking",
        "neighborChanged", "onPlace", "randomTick", "tick", "updateShape", "propagatesSkylightDown", "skipRendering",
        "createBlockStateDefinition"}),
    # ---- the sponge: it takes up the water around it when it is placed and when a block beside it
    # changes (SpongeBlock.tryAbsorbWater: server/fluids.cpp). A wet one dries where water does not stay.
    "SpongeBlock": ("kSponge", "kPlaceDefault", {"neighborChanged", "onPlace"}),
    "WetSpongeBlock": ("kWetSponge", "kPlaceDefault", {"onPlace", "animateTick"}),
}
# Ported classes whose blocks have no item: they can be in the world, and nothing places them by
# hand (block_flag::kSettable: what /setblock and /fill may set besides what is placeable).
ITEMLESS_PORTED = {"LiquidBlock"}
# What a plant stands on: VegetationBlock.mayPlaceOn of its class, asked of the state below it
# (canSurvive) - a block tag, the tag a field of the block names ("field:<name>"), or one of the
# two rules that ask for more than a tag. gen.py makes each rule a bit a state (state::kSupport)
# and gives the block the bit of its rule (BlockInfo::alt[0]).
PLANT_SUPPORT = {
    "FlowerBlock": "#supports_vegetation",
    "TallGrassBlock": "#supports_vegetation",
    "BushBlock": "#supports_vegetation",
    "FireflyBushBlock": "#supports_vegetation",
    "FlowerBedBlock": "#supports_vegetation",
    "DryVegetationBlock": "#supports_dry_vegetation",
    "ShortDryGrassBlock": "#supports_dry_vegetation",   # (a DryVegetationBlock)
    "TallDryGrassBlock": "#supports_dry_vegetation",
    "AzaleaBlock": "#supports_azalea",
    "NetherSproutsBlock": "#supports_nether_sprouts",
    "NetherFungusBlock": "field:supportBlocks",
    "NetherRootsBlock": "field:supportBlocks",
    # CactusFlowerBlock.mayPlaceOn: #support_override_cactus_flower, or a face that holds a centre.
    "CactusFlowerBlock": "cactus_flower",
    # LeafLitterBlock.canSurvive: isFaceSturdy(UP) of the block below - no tag at all.
    "LeafLitterBlock": "sturdy_top",
}
BLOCK_KINDS = ["kPlain", "kChest", "kFurnace", "kCraftingTable", "kTnt", "kPowered", "kSnowy", "kSpreading", "kNylium",
               "kBuddingAmethyst", "kAmethystCluster", "kTorch", "kWallTorch", "kLamp", "kFalling", "kConcretePowder",
               "kSlab", "kStairs", "kFence", "kBars", "kWall", "kLantern", "kMushroom", "kBarrel", "kPlant", "kSegmented",
               "kSign", "kWallSign", "kHangingSign", "kWallHangingSign", "kLiquid", "kSponge", "kWetSponge"]
# The kinds whose randomTick is ported, and of those the ones whose tick may act whatever stands
# around the block (the others are asked what is around them: state::kSmothers and its like).
RANDOM_TICK_KINDS = {"kSpreading", "kNylium", "kBuddingAmethyst"}
RANDOM_TICK_ALWAYS = {"kBuddingAmethyst"}
# The ported classes whose randomTick is ChangeOverTimeBlock.changeOverTime (copper), whatever
# else the class is: it weathers into WeatheringCopper.NEXT_BY_BLOCK's block with the same
# properties (BlockInfo::next_stage), and may do so whatever stands around it.
WEATHERING_CLASSES = {"WeatheringCopperFullBlock", "WeatheringCopperSlabBlock", "WeatheringCopperStairBlock",
                      "WeatheringCopperBarsBlock", "WeatheringCopperChainBlock", "WeatheringLanternBlock",
                      "WeatheringCopperGrateBlock", "WeatheringCopperChestBlock"}
# Classes whose updateShape only asks for the tick of the water in a waterlogged block
# (ticks.scheduleTick(pos, Fluids.WATER, ...), then super): their dry states carry no kHookShape,
# their wet ones do - Game::update_shape asks for that tick for every wet block, whatever its class.
NO_SHAPE_HOOK = {"SlabBlock", "ChainBlock", "WaterloggedTransparentBlock", "BarrierBlock", "HeavyCoreBlock", "MangroveRootsBlock"}
# What a block that holds water by itself (SimpleWaterloggedBlock) does when a fluid comes to it, or
# is taken from it, is the interface's own - canPlaceLiquid: a source of water; placeLiquid: it is
# waterlogged; pickupBlock: it is dry again - but for the classes that say otherwise, which the
# port knows by name (gen.py finds who declares one of the three, and stops at a new one):
#   SlabBlock     a double slab takes none
#   BarrierBlock  takes and gives water only at the hands of a player in creative mode
#   LiquidBlock   is no SimpleWaterloggedBlock: the fluid's own block, taken whole if it is a source
LIQUID_OVERRIDES = {"SlabBlock", "BarrierBlock", "LiquidBlock"}

# Blocks whose class overrides randomTick, and the tick is not ported - of a class that is placeable
# by the general rule, or of a ported one: what they would do waits for something else. Named
# here so that a new one stops the generator.
RANDOM_TICK_GAPS = {"IceBlock": "melts into water where a light is near: not ported, ice stays ice",
                    "LiquidBlock": "lava sets fire to what burns around it: it waits for fire"}

# Property layouts the C++ side computes states from: block -> [(property, values in order)].
# state = first_state + sum(value index * stride); gen.py emits the strides and checks the orders.
_CLUSTER = [("facing", ["north", "east", "south", "west", "up", "down"]), ("waterlogged", ["true", "false"])]
_WALL_TORCH = [("facing", ["north", "south", "west", "east"])]
STATE_LAYOUTS = {
    "minecraft:barrel": [("facing", ["north", "east", "south", "west", "up", "down"]), ("open", ["true", "false"])],
    "minecraft:tnt": [("unstable", ["true", "false"])],
    "minecraft:redstone_lamp": [("lit", ["true", "false"])],
    "minecraft:wall_torch": _WALL_TORCH,
    "minecraft:soul_wall_torch": _WALL_TORCH,
    "minecraft:copper_wall_torch": _WALL_TORCH,
    "minecraft:small_amethyst_bud": _CLUSTER,
    "minecraft:medium_amethyst_bud": _CLUSTER,
    "minecraft:large_amethyst_bud": _CLUSTER,
    "minecraft:amethyst_cluster": _CLUSTER,
    "minecraft:snow": [("layers", ["1", "2", "3", "4", "5", "6", "7", "8"])],
}
# The same for whole families: props::<name> has the strides (no kFirst: BlockInfo::first_state),
# and every block whose own class is one of the classes has exactly this layout.
_WET = ("waterlogged", ["true", "false"])
_BOOL = ["true", "false"]
_SIDE = ["none", "low", "tall"]
_HORIZONTAL = ["north", "south", "west", "east"]
_FOUR = ["1", "2", "3", "4"]
_SIXTEEN = [str(i) for i in range(16)]
FAMILY_LAYOUTS = {
    "chest": ({"ChestBlock", "CopperChestBlock", "WeatheringCopperChestBlock"},
              [("facing", _HORIZONTAL), ("type", ["single", "left", "right"]), _WET]),
    "furnace": ({"FurnaceBlock", "SmokerBlock", "BlastFurnaceBlock"}, [("facing", _HORIZONTAL), ("lit", _BOOL)]),
    "flower_bed": ({"FlowerBedBlock"}, [("facing", _HORIZONTAL), ("flower_amount", _FOUR)]),
    "leaf_litter": ({"LeafLitterBlock"}, [("facing", _HORIZONTAL), ("segment_amount", _FOUR)]),
    "slab": ({"SlabBlock", "WeatheringCopperSlabBlock"}, [("type", ["top", "bottom", "double"]), _WET]),
    "stairs": ({"StairBlock", "WeatheringCopperStairBlock"},
               [("facing", ["north", "south", "west", "east"]), ("half", ["top", "bottom"]),
                ("shape", ["straight", "inner_left", "inner_right", "outer_left", "outer_right"]), _WET]),
    "cross": ({"FenceBlock", "IronBarsBlock", "StainedGlassPaneBlock", "WeatheringCopperBarsBlock"},
              [("east", _BOOL), ("north", _BOOL), ("south", _BOOL), _WET, ("west", _BOOL)]),
    "wall": ({"WallBlock"}, [("east", _SIDE), ("north", _SIDE), ("south", _SIDE), ("up", _BOOL), _WET, ("west", _SIDE)]),
    "horizontal": ({"GlazedTerracottaBlock"}, [("facing", ["north", "south", "west", "east"])]),
    "chain": ({"ChainBlock", "WeatheringCopperChainBlock"}, [("axis", ["x", "y", "z"]), _WET]),
    "lantern": ({"LanternBlock", "WeatheringLanternBlock"}, [("hanging", _BOOL), _WET]),
    "end_rod": ({"EndRodBlock"}, [("facing", ["north", "east", "south", "west", "up", "down"])]),
    "waterlogged": ({"WaterloggedTransparentBlock", "WeatheringCopperGrateBlock", "BarrierBlock", "HeavyCoreBlock",
                     "MangroveRootsBlock"}, [_WET]),
    "mushroom": ({"HugeMushroomBlock"}, [("down", _BOOL), ("east", _BOOL), ("north", _BOOL), ("south", _BOOL), ("up", _BOOL),
                                         ("west", _BOOL)]),
    "sign": ({"StandingSignBlock"}, [("rotation", _SIXTEEN), _WET]),
    "wall_sign": ({"WallSignBlock", "WallHangingSignBlock"}, [("facing", _HORIZONTAL), _WET]),
    "hanging_sign": ({"CeilingHangingSignBlock"}, [("attached", _BOOL), ("rotation", _SIXTEEN), _WET]),
}

# Sounds and particles the server sends by id (static registries: sound_event, particle_type).
SOUNDS = ["block.chest.open", "block.chest.close", "entity.tnt.primed", "entity.generic.explode", "entity.item.pickup",
          "entity.armor_stand.break", "block.barrel.open", "block.barrel.close", "block.copper_chest.open",
          "block.sponge.absorb", "block.wet_sponge.dries",
          "block.copper_chest.close", "block.copper_chest_weathered.open", "block.copper_chest_weathered.close",
          "block.copper_chest_oxidized.open", "block.copper_chest_oxidized.close", "block.chest.locked",
          "item.dye.use", "item.ink_sac.use", "item.glow_ink_sac.use", "item.honeycomb.wax_on",
          "block.sign.waxed_interact_fail"]
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
    path = d / "gamedata" / "enums.json"
    if not path.exists():
        die("gamedata/enums.json is missing - run: mcgen.py gamedata")
    out["enums"] = json.loads(path.read_text())["enums"]  # enum constants in ordinal order
    path = d / "gamedata" / "fluids.json"
    if not path.exists():
        die("gamedata/fluids.json is missing - run: mcgen.py gamedata")
    out["fluids"] = json.loads(path.read_text())  # fluid states: which fluid, how much of it
    path = d / "gamedata" / "statics.json"
    if not path.exists():
        die("gamedata/statics.json is missing - run: mcgen.py gamedata")
    out["statics"] = json.loads(path.read_text())["classes"]  # static tables of classes (WeatheringCopper.NEXT_BY_BLOCK)
    return out


def ported_block(game_block: dict):
    """(kind, placement, item class) of a block whose own class is ported, else None. Checks the hook set."""
    cls = game_block["class"][0]
    if cls not in PORTED_BLOCKS:
        return None
    kind, place, hooks, *item = PORTED_BLOCKS[cls]
    if set(game_block["hooks"]) != hooks:
        die(f"{game_block['name']}: {cls} overrides {sorted(set(game_block['hooks']) ^ hooks)} differently from the version "
            "the server's port was written against - read the class again, then update PORTED_BLOCKS")
    return kind, place, item[0] if item else "BlockItem"


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
        self.block = ""        # the block whose table is being read
        self.doubles = False   # ...and whether its table gives two for `type=double` (a slab's)
        self.segments: set[int] = set()  # ...and the counts it gives by the number of segments (petals, leaf litter)
        self.segment_property = None

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
        if kind == "minecraft:random_chance":
            if not isinstance(c["chance"], (int, float)):
                die(f"{where}: a random_chance that is not a number")
            return float(c["chance"])       # the same draw: random.nextFloat() < chance
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
            # A slab's: two of it for the double slab. That is a state, not a draw - the server
            # doubles the count for it (Game::block_drop), and mcgen holds every slab to this form.
            if kind == "minecraft:set_count" and m.get("count") == 2 and not m.get("add") and m.get("condition") == {
                    "type": "minecraft:match_block", "blocks": self.block, "state": {"type": "double"}}:
                self.doubles = True
                continue
            # Petals' and leaf litter's: as many as the block holds, one set_count a number of
            # segments. A state again, not a draw (Game::block_drop multiplies; mcgen holds
            # every block of the kind to this form, all four counts).
            cond = m.get("condition", {})
            if kind == "minecraft:set_count" and not m.get("add") and isinstance(m.get("count"), int) and isinstance(cond, dict) \
                    and cond.get("type") == "minecraft:match_block" and cond.get("blocks") == self.block \
                    and self.segment_property and cond.get("state") == {self.segment_property: str(m["count"])}:
                self.segments.add(m["count"])
                continue
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
                # A floor of nothing under a range that begins below it (a mushroom block: -6..2,
                # most often none) is kept as the range: a count below one drops nothing.
                limit = m["limit"]
                if (lo < limit.get("min", lo) and limit["min"] != 0) or hi > limit.get("max", hi):
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
            if not (-128 <= lo <= hi <= 99 and hi >= 1):
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

    def explosion_drops(self, block: str, table_name: str | None, slab: bool = False,
                        segments: str | None = None) -> list[tuple[float, int, int, int]]:
        """`slab`: the block is a SlabBlock - its table must give two for the double slab, and no other's may.
        `segments`: the property that counts the segments of a SegmentableBlock - its table must give
        one a segment, and no other's may."""
        if not table_name:
            if slab or segments:
                die(f"{block}: a slab or a segmented block without a loot table")
            return []
        t = self.read("loot_table", table_name)
        where = f"loot table of {block}"
        self.block, self.doubles, self.segments, self.segment_property = block, False, set(), segments
        out = self.reduce(t, where)
        if self.segments != ({1, 2, 3, 4} if segments else set()) or (segments and (
                len(out) != 1 or out[0][0] < 1.0 or (out[0][2], out[0][3]) != (1, 1))):
            die(f"{where}: {'not ' if segments else ''}a segmented block's table (one of it a segment) - Game::block_drop goes by the kind")
        if self.doubles != slab or (slab and (len(out) != 1 or out[0][0] < 1.0 or (out[0][2], out[0][3]) != (1, 1))):
            die(f"{where}: {'not ' if slab else ''}a slab's table (one of it, two for the double slab) - Game::block_drop goes by the class")
        return out

    def reduce(self, t: dict, where: str) -> list[tuple[float, int, int, int]]:
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

# AbstractFurnaceBlockEntity.getSpeedMultiplier of every fuel in a smoker and a blast furnace
# (context_float_provider/cooking/fast_speed_multiplier): one number for all of them, which
# fuel_ticks holds each fuel to. In a furnace it is 1.
FAST_COOKING_SPEED = 2.0
FAST_COOKERS = ("minecraft:smoker", "minecraft:blast_furnace")


def fuel_ticks(vanilla, comps: dict, name: str) -> tuple[int, int]:
    """AbstractFurnaceBlockEntity.getBurnDuration of the item: its cooking_fuel burn time in a
    furnace, and in a smoker or a blast furnace. (0, 0): not a fuel.

    26.x: the component names two context providers (data/minecraft/context_int_provider,
    context_float_provider) that ask whether the block is #fast_cooking - a smoker or a blast
    furnace. Evaluated here for each of the three blocks; the two fast ones must agree, and the
    speed multiplier must come out as 1 in a furnace and as FAST_COOKING_SPEED in the others.
    """
    fuel = comps.get("minecraft:cooking_fuel")
    if fuel is None:
        return 0, 0

    def read(kind: str, ref):
        path = f"data/minecraft/{kind}/{ref.split(':', 1)[1]}.json"
        if path not in vanilla.names:
            die(f"{name}: cooking_fuel refers to {ref}, which is not in the jar")
        return json.loads(vanilla.zip.read(path))

    def holds(cond, block: str) -> bool:
        p = read("predicate", cond) if isinstance(cond, str) else cond
        if p.get("type") != "minecraft:match_block" or "state" in p:
            die(f"{name}: the fast-cooking condition is no longer a plain match_block: {p}")
        blocks = p["blocks"]
        return block in ([blocks] if isinstance(blocks, str) else blocks)

    def value(kind: str, v, block: str):
        if isinstance(v, (int, float)):
            return v
        if isinstance(v, str):
            return value(kind, read(kind, v), block)
        if v.get("type") == "minecraft:conditional":
            return value(kind, v["on_true"] if holds(v["condition"], block) else v["on_false"], block)
        if v.get("type") == "minecraft:div":
            left, right = value(kind, v["left"], block), value(kind, v["right"], block)
            if right == 0:
                die(f"{name}: burn time divides by zero")
            return int(left / right) if kind == "context_int_provider" else left / right
        die(f"{name}: cooking_fuel provider {v} is not one mcgen evaluates")

    out = []
    for block, want in (("minecraft:furnace", 1.0), *((b, FAST_COOKING_SPEED) for b in FAST_COOKERS)):
        ticks = value("context_int_provider", fuel["burn_time"], block)
        speed = value("context_float_provider", fuel.get("speed_multiplier", 1.0), block)
        if speed != want:
            die(f"{name}: burns with speed multiplier {speed} in a {block} - the server's has {want} for every fuel")
        if ticks != int(ticks) or not 0 < ticks <= 0xFFFF:
            die(f"{name}: burn time {ticks} in a {block} does not fit ItemInfo::fuel_ticks")
        out.append(int(ticks))
    if len(set(out[1:])) != 1:
        die(f"{name}: burns for {out[1:]} in {FAST_COOKERS} - ItemInfo has one burn time for both")
    return out[0], out[1]


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


# The cooking recipes the server has a block for, by recipe type -> recipe::Cooker.
COOKING = {"minecraft:smelting": "kSmelting", "minecraft:smoking": "kSmoking", "minecraft:blasting": "kBlasting"}


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
        cooking = []    # (name, type, ingredient, result, count, ticks, experience)
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
                if kind in COOKING:
                    item, count, patch = self.result(r["result"], where)
                    ticks = r.get("cookingtime", 200)
                    if patch or not 1 <= ticks <= 0x7FFF:
                        die(f"{where}: a cooking result with components, or a cooking time of {ticks}")
                    cooking.append((name, COOKING[kind], self.ingredient(r["ingredient"], where), item, count, ticks,
                                    float(r.get("experience", 0.0))))
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
        if not crafting or {c[1] for c in cooking} != set(COOKING.values()):
            die("no crafting recipes in the jar, or none of one of the cooking kinds: the recipe directory moved")
        return {"crafting": crafting, "cooking": cooking, "skipped": skipped, "sets": sets}

    def emit(self, header: str, packets: dict) -> dict:
        b = self.build()
        ings = sorted(self.ingredients, key=self.ingredients.get)
        if len(ings) >= 0xFFFF:
            die("too many distinct ingredients for 16-bit ids")
        o = [header, """
// Recipes of the vanilla data pack (data/minecraft/recipe), reduced to what the server matches:
// an ingredient is a sorted list of item ids, a result an item, a count and - rarely - components.
// kCrafting and kCooking are in the order RecipeManager looks recipes up in (by id): the first
// match - of its type, for a cooking recipe - wins.
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
// RecipeType of an AbstractCookingRecipe: what a furnace, a smoker and a blast furnace cook.
enum Cooker : unsigned char { kSmelting, kSmoking, kBlasting, kCookers };
struct Cooking {
    unsigned short ingredient, result;
    unsigned short ticks;      // cookingtime
    unsigned char count;
    unsigned char type;        // a Cooker
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
        o.append(f"}};\n\ninline constexpr unsigned kCookingCount = {len(b['cooking'])};\n"
                 f"inline constexpr Cooking kCooking[kCookingCount] = {{\n")
        for name, kind, ing, item, count, ticks, xp in b["cooking"]:
            o.append(f"    {{{ing}, {item}, {ticks}, {count}, {kind}, {_cfloat32(xp)}}},  // {name}\n")
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
        stats = {"crafting": len(b["crafting"]), "cooking": len(b["cooking"]), "recipes_skipped": sum(b["skipped"].values())}
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
