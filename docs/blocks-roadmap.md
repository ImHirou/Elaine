# Blocks: what is simulated, and what is next

Minecraft 26.3 has 1286 blocks in 268 block classes. The server holds a block only if it
simulates everything vanilla would do with it - so the list of what can be **placed** is the list
of what is **done**: 773 blocks today. This file is that list, block class by block class,
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
flows, the water in a waterlogged slab or fence stays where it is, a fence takes no lead, ores
give no experience, an amethyst block does not chime.

## Systems

What the block classes lean on. A milestone below is workable once the systems it names are.

- [x] Block entities: stored per chunk, listed in the chunk packet, created and removed with their block
- [x] Menus on blocks: `open_screen`, any number of slots, data slots, closing when out of reach
- [x] Crafting: shaped, shapeless and transmute recipes; smelting, smoking and blasting; fuel, and how fast it cooks
- [x] Entity physics for one kind (primed TNT): gravity, collision with blocks, bounce, speed factor
- [x] Explosions: rays, exposure, block drops without a tool, chain reactions
- [ ] Survival: mining over time, tools, block drops through loot tables with a tool and enchantments
- [x] Neighbour updates: `neighborChanged` and `updateShape` in vanilla's order (`CollectingNeighborUpdater`), blocks that break when what holds them goes
- [x] Scheduled block ticks (`LevelTicks`), saved with their chunk
- [x] Random block ticks, at the simulation distance
- [x] Falling blocks (`FallingBlockEntity`)
- [x] The particles and the sound of a block breaking, for the others (level event 2001)
- [ ] The sound of a block being placed (`sound`)
- [ ] Fluids: water and lava that flow, fluid ticks, waterlogged blocks that ask for them
- [ ] Redstone signals beyond the block of redstone
- [ ] The time of day and the weather: light that changes with them, rain and snow on blocks (`tickPrecipitation`)
- [ ] Experience orbs: a furnace's stored experience, ores blown up by a player's TNT
- [x] Item physics: dropped and spilled items fall and are pushed by explosions
- [ ] What blocks do to entities inside them (`entityInside`): cobweb, honey, bubble columns, portals, pressure plates
- [ ] The recipe book (`recipe_book_add`, `place_recipe`) and recipe unlocking
- [ ] Special crafting recipes: dyed armour, banner and book copies, fireworks, map extending, repairing, shield decoration, tipped arrows, decorated pots
- [ ] Block entity names and locks (a chest named on an anvil), saved block entity data on placed items (ctrl + pick block)
- [ ] Comparator output of containers (`getAnalogOutputSignal`)
- [x] Persistence of block entities with their chunk

## 0. Plain blocks (302 of 303 blocks)

Needs: Nothing: a state that is stored, lit and sent. Done since the first world.

- [x] **Block** (182)  
  stone, granite, polished_granite, diorite, polished_diorite, andesite, polished_andesite, dirt, coarse_dirt, cobblestone, oak_planks, spruce_planks, birch_planks, jungle_planks, acacia_planks, cherry_planks, dark_oak_planks, pale_oak_planks, mangrove_planks, poplar_planks, bamboo_planks, bamboo_mosaic, bedrock, lapis_block, sandstone, chiseled_sandstone, cut_sandstone, white_wool, orange_wool, magenta_wool, light_blue_wool, yellow_wool, lime_wool, pink_wool, gray_wool, light_gray_wool, cyan_wool, purple_wool, blue_wool, brown_wool, green_wool, red_wool, black_wool, gold_block, iron_block, bricks, bookshelf, mossy_cobblestone, obsidian, diamond_block, snow_block, clay, soul_soil, glowstone, stone_bricks, mossy_stone_bricks, cracked_stone_bricks, chiseled_stone_bricks, packed_mud, mud_bricks, melon, resin_block, resin_bricks, chiseled_resin_bricks, nether_bricks, end_stone, emerald_block, quartz_block, chiseled_quartz_block, white_terracotta, orange_terracotta, magenta_terracotta, light_blue_terracotta, yellow_terracotta, lime_terracotta, pink_terracotta, gray_terracotta, light_gray_terracotta, cyan_terracotta, purple_terracotta, blue_terracotta, brown_terracotta, green_terracotta, red_terracotta, black_terracotta, prismarine, prismarine_bricks, dark_prismarine, sea_lantern, terracotta, coal_block, packed_ice, red_sandstone, chiseled_red_sandstone, cut_red_sandstone, smooth_stone, smooth_sandstone, smooth_quartz, smooth_red_sandstone, purpur_block, end_stone_bricks, nether_wart_block, red_nether_bricks, white_concrete, orange_concrete, magenta_concrete, light_blue_concrete, yellow_concrete, lime_concrete, pink_concrete, gray_concrete, light_gray_concrete, cyan_concrete, purple_concrete, blue_concrete, brown_concrete, green_concrete, red_concrete, black_concrete, dried_kelp_block, dead_tube_coral_block, dead_brain_coral_block, dead_bubble_coral_block, dead_fire_coral_block, dead_horn_coral_block, fletching_table, warped_wart_block, shroomlight, crimson_planks, warped_planks, honeycomb_block, netherite_block, ancient_debris, lodestone, blackstone, polished_blackstone, polished_blackstone_bricks, cracked_polished_blackstone_bricks, chiseled_polished_blackstone, gilded_blackstone, chiseled_nether_bricks, cracked_nether_bricks, quartz_bricks, tuff, polished_tuff, chiseled_tuff, tuff_bricks, chiseled_tuff_bricks, sulfur, polished_sulfur, sulfur_bricks, chiseled_sulfur, cinnabar, polished_cinnabar, cinnabar_bricks, chiseled_cinnabar, calcite, waxed_copper_block, waxed_exposed_copper, waxed_weathered_copper, waxed_oxidized_copper, waxed_cut_copper, waxed_exposed_cut_copper, waxed_weathered_cut_copper, waxed_oxidized_cut_copper, waxed_chiseled_copper, waxed_exposed_chiseled_copper, waxed_weathered_chiseled_copper, waxed_oxidized_chiseled_copper, dripstone_block, cobbled_deepslate, polished_deepslate, deepslate_tiles, deepslate_bricks, chiseled_deepslate, cracked_deepslate_bricks, cracked_deepslate_tiles, smooth_basalt, raw_iron_block, raw_copper_block, raw_gold_block, reinforced_deepslate
- [x] **RotatedPillarBlock** (60) - `getStateForPlacement`  
  pale_oak_wood, oak_log, spruce_log, birch_log, jungle_log, acacia_log, cherry_log, dark_oak_log, pale_oak_log, mangrove_log, poplar_log, muddy_mangrove_roots, bamboo_block, stripped_spruce_log, stripped_birch_log, stripped_jungle_log, stripped_acacia_log, stripped_cherry_log, stripped_dark_oak_log, stripped_pale_oak_log, stripped_oak_log, stripped_mangrove_log, stripped_poplar_log, stripped_bamboo_block, oak_wood, spruce_wood, birch_wood, jungle_wood, acacia_wood, cherry_wood, dark_oak_wood, mangrove_wood, poplar_wood, stripped_oak_wood, stripped_spruce_wood, stripped_birch_wood, stripped_jungle_wood, stripped_acacia_wood, stripped_cherry_wood, stripped_dark_oak_wood, stripped_pale_oak_wood, stripped_mangrove_wood, stripped_poplar_wood, basalt, polished_basalt, quartz_pillar, purpur_pillar, bone_block, warped_stem, stripped_warped_stem, warped_hyphae, stripped_warped_hyphae, crimson_stem, stripped_crimson_stem, crimson_hyphae, stripped_crimson_hyphae, deepslate, ochre_froglight, verdant_froglight, pearlescent_froglight
- [x] **DropExperienceBlock** (16) - `spawnAfterBreak`  
  gold_ore, deepslate_gold_ore, iron_ore, deepslate_iron_ore, coal_ore, deepslate_coal_ore, nether_gold_ore, lapis_ore, deepslate_lapis_ore, diamond_ore, deepslate_diamond_ore, emerald_ore, deepslate_emerald_ore, nether_quartz_ore, copper_ore, deepslate_copper_ore
- [x] **StainedGlassBlock** (16)  
  white_stained_glass, orange_stained_glass, magenta_stained_glass, light_blue_stained_glass, yellow_stained_glass, lime_stained_glass, pink_stained_glass, gray_stained_glass, light_gray_stained_glass, cyan_stained_glass, purple_stained_glass, blue_stained_glass, brown_stained_glass, green_stained_glass, red_stained_glass, black_stained_glass
- [x] **InfestedBlock** (6) - `spawnAfterBreak`  
  infested_stone, infested_cobblestone, infested_stone_bricks, infested_mossy_stone_bricks, infested_cracked_stone_bricks, infested_chiseled_stone_bricks
- [x] **AirBlock** (3)  
  air, void_air, cave_air
- [x] **BonemealableFeaturePlacerBlock** (2)  
  moss_block, pale_moss_block
- [x] **AmethystBlock** (1) - `onProjectileHit`  
  amethyst_block
- [x] **CryingObsidianBlock** (1)  
  crying_obsidian
- [x] **HalfTransparentBlock** (1)  
  blue_ice
- [x] **HayBlock** (1) - `fallOn`, `getStateForPlacement`  
  hay_block
- [x] **HoneyBlock** (1) - `entityInside`, `fallOn`  
  honey_block
- [x] **IceBlock** (1) - `playerDestroy`, `randomTick`  
  ice
- [ ] **InfestedRotatedPillarBlock** (1) - `getStateForPlacement`, `spawnAfterBreak`  
  infested_deepslate
- [x] **MagmaBlock** (1) - `stepOn`  
  magma_block
- [x] **MudBlock** (1)  
  mud
- [x] **NetherrackBlock** (1)  
  netherrack
- [x] **PoweredBlock** (1) - `isSignalSource`, `ownSignal`  
  redstone_block
- [x] **RootedDirtBlock** (1)  
  rooted_dirt
- [x] **SlimeBlock** (1) - `fallOn`, `stepOn`  
  slime_block
- [x] **SoulSandBlock** (1)  
  soul_sand
- [x] **StructureVoidBlock** (1)  
  structure_void
- [x] **TintedGlassBlock** (1)  
  tinted_glass
- [x] **TransparentBlock** (1)  
  glass
- [x] **WebBlock** (1) - `entityInside`  
  cobweb

## 1. Block entities and menus: the first four (4 of 4 blocks)

Needs: Menus over containers, recipes, a furnace's tick, primed TNT and explosions.

- [x] **ChestBlock** (1, *block entity*) - `affectNeighborsAfterRemoval`, `getAnalogOutputSignal`, `getFluidState`, `getMenuProvider`, `getStateForPlacement`, `hasAnalogOutputSignal`, `tick`, `triggerEvent`, `updateShape`, `useWithoutItem`  
  chest
- [x] **CraftingTableBlock** (1) - `getMenuProvider`, `useWithoutItem`  
  crafting_table
- [x] **FurnaceBlock** (1, *block entity*) - `affectNeighborsAfterRemoval`, `getAnalogOutputSignal`, `getMenuProvider`, `getStateForPlacement`, `hasAnalogOutputSignal`, `triggerEvent`, `useWithoutItem`  
  furnace
- [x] **TntBlock** (1) - `dropFromExplosion`, `neighborChanged`, `onPlace`, `onProjectileHit`, `playerWillDestroy`, `useItemOn`, `wasExploded`  
  tnt

## 2. Shapes: a state from how the block is placed and what stands beside it (297 of 307 blocks)

Needs: BlockPlaceContext - the clicked face and where on it, the player's look, a block replaced by more of itself - and `updateShape` for every neighbour change are there (`server/block_shapes.cpp`: slabs, stairs, fences, panes, walls, lanterns, chains). What is left has a rule of its own: a path that turns to dirt, a pumpkin that makes a golem, what grows on the faces of other blocks.

- [x] **SlabBlock** (97) - `canBeReplaced(BlockState,BlockPlaceContext)`, `getFluidState`, `getStateForPlacement`, `updateShape`  
  white_wool_slab, orange_wool_slab, magenta_wool_slab, light_blue_wool_slab, yellow_wool_slab, lime_wool_slab, pink_wool_slab, gray_wool_slab, light_gray_wool_slab, cyan_wool_slab, purple_wool_slab, blue_wool_slab, brown_wool_slab, green_wool_slab, red_wool_slab, black_wool_slab, resin_brick_slab, prismarine_slab, prismarine_brick_slab, dark_prismarine_slab, oak_slab, spruce_slab, birch_slab, jungle_slab, acacia_slab, cherry_slab, dark_oak_slab, pale_oak_slab, mangrove_slab, poplar_slab, bamboo_slab, bamboo_mosaic_slab, stone_slab, sandstone_slab, cut_sandstone_slab, petrified_oak_slab, cobblestone_slab, brick_slab, stone_brick_slab, mud_brick_slab, nether_brick_slab, quartz_slab, red_sandstone_slab, cut_red_sandstone_slab, smooth_stone_slab, purpur_slab, white_concrete_slab, orange_concrete_slab, magenta_concrete_slab, light_blue_concrete_slab, yellow_concrete_slab, lime_concrete_slab, pink_concrete_slab, gray_concrete_slab, light_gray_concrete_slab, cyan_concrete_slab, purple_concrete_slab, blue_concrete_slab, brown_concrete_slab, green_concrete_slab, red_concrete_slab, black_concrete_slab, polished_granite_slab, smooth_red_sandstone_slab, mossy_stone_brick_slab, polished_diorite_slab, mossy_cobblestone_slab, end_stone_brick_slab, smooth_sandstone_slab, smooth_quartz_slab, granite_slab, andesite_slab, red_nether_brick_slab, polished_andesite_slab, diorite_slab, crimson_slab, warped_slab, blackstone_slab, polished_blackstone_brick_slab, polished_blackstone_slab, tuff_slab, polished_tuff_slab, tuff_brick_slab, sulfur_slab, polished_sulfur_slab, sulfur_brick_slab, cinnabar_slab, polished_cinnabar_slab, cinnabar_brick_slab, waxed_cut_copper_slab, waxed_exposed_cut_copper_slab, waxed_weathered_cut_copper_slab, waxed_oxidized_cut_copper_slab, cobbled_deepslate_slab, polished_deepslate_slab, deepslate_tile_slab, deepslate_brick_slab
- [x] **StairBlock** (93) - `getExplosionResistance`, `getFluidState`, `getStateForPlacement`, `updateShape`  
  white_wool_stairs, orange_wool_stairs, magenta_wool_stairs, light_blue_wool_stairs, yellow_wool_stairs, lime_wool_stairs, pink_wool_stairs, gray_wool_stairs, light_gray_wool_stairs, cyan_wool_stairs, purple_wool_stairs, blue_wool_stairs, brown_wool_stairs, green_wool_stairs, red_wool_stairs, black_wool_stairs, oak_stairs, cobblestone_stairs, brick_stairs, stone_brick_stairs, mud_brick_stairs, resin_brick_stairs, nether_brick_stairs, sandstone_stairs, spruce_stairs, birch_stairs, jungle_stairs, quartz_stairs, acacia_stairs, cherry_stairs, dark_oak_stairs, pale_oak_stairs, mangrove_stairs, poplar_stairs, bamboo_stairs, bamboo_mosaic_stairs, prismarine_stairs, prismarine_brick_stairs, dark_prismarine_stairs, red_sandstone_stairs, purpur_stairs, white_concrete_stairs, orange_concrete_stairs, magenta_concrete_stairs, light_blue_concrete_stairs, yellow_concrete_stairs, lime_concrete_stairs, pink_concrete_stairs, gray_concrete_stairs, light_gray_concrete_stairs, cyan_concrete_stairs, purple_concrete_stairs, blue_concrete_stairs, brown_concrete_stairs, green_concrete_stairs, red_concrete_stairs, black_concrete_stairs, polished_granite_stairs, smooth_red_sandstone_stairs, mossy_stone_brick_stairs, polished_diorite_stairs, mossy_cobblestone_stairs, end_stone_brick_stairs, stone_stairs, smooth_sandstone_stairs, smooth_quartz_stairs, granite_stairs, andesite_stairs, red_nether_brick_stairs, polished_andesite_stairs, diorite_stairs, crimson_stairs, warped_stairs, blackstone_stairs, polished_blackstone_brick_stairs, polished_blackstone_stairs, tuff_stairs, polished_tuff_stairs, tuff_brick_stairs, sulfur_stairs, polished_sulfur_stairs, sulfur_brick_stairs, cinnabar_stairs, polished_cinnabar_stairs, cinnabar_brick_stairs, waxed_cut_copper_stairs, waxed_exposed_cut_copper_stairs, waxed_weathered_cut_copper_stairs, waxed_oxidized_cut_copper_stairs, cobbled_deepslate_stairs, polished_deepslate_stairs, deepslate_tile_stairs, deepslate_brick_stairs
- [x] **WallBlock** (32) - `getFluidState`, `getStateForPlacement`, `updateShape`  
  resin_brick_wall, cobblestone_wall, mossy_cobblestone_wall, brick_wall, prismarine_wall, red_sandstone_wall, mossy_stone_brick_wall, granite_wall, stone_brick_wall, mud_brick_wall, nether_brick_wall, andesite_wall, red_nether_brick_wall, sandstone_wall, end_stone_brick_wall, diorite_wall, blackstone_wall, polished_blackstone_brick_wall, polished_blackstone_wall, tuff_wall, polished_tuff_wall, tuff_brick_wall, sulfur_wall, polished_sulfur_wall, sulfur_brick_wall, cinnabar_wall, polished_cinnabar_wall, cinnabar_brick_wall, cobbled_deepslate_wall, polished_deepslate_wall, deepslate_tile_wall, deepslate_brick_wall
- [x] **GlazedTerracottaBlock** (16) - `getStateForPlacement`  
  white_glazed_terracotta, orange_glazed_terracotta, magenta_glazed_terracotta, light_blue_glazed_terracotta, yellow_glazed_terracotta, lime_glazed_terracotta, pink_glazed_terracotta, gray_glazed_terracotta, light_gray_glazed_terracotta, cyan_glazed_terracotta, purple_glazed_terracotta, blue_glazed_terracotta, brown_glazed_terracotta, green_glazed_terracotta, red_glazed_terracotta, black_glazed_terracotta
- [x] **StainedGlassPaneBlock** (16) - `getFluidState`, `getStateForPlacement`, `updateShape`  
  white_stained_glass_pane, orange_stained_glass_pane, magenta_stained_glass_pane, light_blue_stained_glass_pane, yellow_stained_glass_pane, lime_stained_glass_pane, pink_stained_glass_pane, gray_stained_glass_pane, light_gray_stained_glass_pane, cyan_stained_glass_pane, purple_stained_glass_pane, blue_stained_glass_pane, brown_stained_glass_pane, green_stained_glass_pane, red_stained_glass_pane, black_stained_glass_pane
- [x] **FenceBlock** (14) - `getFluidState`, `getStateForPlacement`, `updateShape`, `useWithoutItem`  
  oak_fence, nether_brick_fence, spruce_fence, birch_fence, jungle_fence, acacia_fence, cherry_fence, dark_oak_fence, pale_oak_fence, mangrove_fence, poplar_fence, bamboo_fence, crimson_fence, warped_fence
- [x] **IronBarsBlock** (6) - `getFluidState`, `getStateForPlacement`, `updateShape`  
  iron_bars, waxed_copper_bars, waxed_exposed_copper_bars, waxed_weathered_copper_bars, waxed_oxidized_copper_bars, glass_pane
- [x] **LanternBlock** (6) - `canSurvive`, `getFluidState`, `getStateForPlacement`, `updateShape`  
  lantern, soul_lantern, waxed_copper_lantern, waxed_exposed_copper_lantern, waxed_weathered_copper_lantern, waxed_oxidized_copper_lantern
- [x] **ChainBlock** (5) - `getFluidState`, `getStateForPlacement`, `updateShape`  
  iron_chain, waxed_copper_chain, waxed_exposed_copper_chain, waxed_weathered_copper_chain, waxed_oxidized_copper_chain
- [x] **WaterloggedTransparentBlock** (4) - `getFluidState`, `getStateForPlacement`, `updateShape`  
  waxed_copper_grate, waxed_exposed_copper_grate, waxed_weathered_copper_grate, waxed_oxidized_copper_grate
- [x] **HugeMushroomBlock** (3) - `getStateForPlacement`, `updateShape`  
  brown_mushroom_block, red_mushroom_block, mushroom_stem
- [ ] **CarvedPumpkinBlock** (2) - `getStateForPlacement`, `onPlace`  
  carved_pumpkin, jack_o_lantern
- [x] **BarrierBlock** (1) - `getFluidState`, `getStateForPlacement`, `updateShape`  
  barrier
- [x] **EndRodBlock** (1) - `getStateForPlacement`  
  end_rod
- [ ] **GlowLichenBlock** (1) - `canBeReplaced(BlockState,BlockPlaceContext)`, `canSurvive`, `getFluidState`, `getStateForPlacement`, `updateShape`  
  glow_lichen
- [ ] **HangingMossBlock** (1) - `canSurvive`, `tick`, `updateShape`  
  pale_hanging_moss
- [x] **HeavyCoreBlock** (1) - `getFluidState`, `getStateForPlacement`, `updateShape`  
  heavy_core
- [ ] **LightBlock** (1) - `getCloneItemStack`, `getFluidState`, `updateShape`, `useWithoutItem`  
  light
- [x] **MangroveRootsBlock** (1) - `getFluidState`, `getStateForPlacement`, `updateShape`  
  mangrove_roots
- [ ] **MossyCarpetBlock** (1) - `canSurvive`, `getStateForPlacement`, `setPlacedBy`, `updateShape`  
  pale_moss_carpet
- [ ] **MultifaceBlock** (1) - `canBeReplaced(BlockState,BlockPlaceContext)`, `canSurvive`, `getFluidState`, `getStateForPlacement`, `updateShape`  
  resin_clump
- [ ] **PathBlock** (1) - `canSurvive`, `getStateForPlacement`, `tick`, `updateShape`  
  dirt_path
- [ ] **PointedDripstoneBlock** (1) - `canSurvive`, `fallOn`, `getFluidState`, `getStateForPlacement`, `onProjectileHit`, `randomTick`, `tick`, `updateShape`  
  pointed_dripstone
- [ ] **SculkVeinBlock** (1) - `canBeReplaced(BlockState,BlockPlaceContext)`, `canSurvive`, `getFluidState`, `getStateForPlacement`, `updateShape`  
  sculk_vein
- [x] **SnowyBlock** (1) - `getStateForPlacement`, `updateShape`  
  podzol

## 3. Support: blocks that need something to stand on or hang from (41 of 81 blocks)

Needs: `canSurvive` and the break-and-drop when the support goes are there (`Game::can_survive`, `break_block`: torches, amethyst); each class is its own rule of placement and its own drops. Survival drops need loot tables with a tool.

- [ ] **WoolCarpetBlock** (16) - `canSurvive`, `updateShape`  
  white_carpet, orange_carpet, magenta_carpet, light_blue_carpet, yellow_carpet, lime_carpet, pink_carpet, gray_carpet, light_gray_carpet, cyan_carpet, purple_carpet, blue_carpet, brown_carpet, green_carpet, red_carpet, black_carpet
- [x] **FlowerBlock** (14) - `canSurvive`, `updateShape`  
  dandelion, golden_dandelion, torchflower, poppy, blue_orchid, allium, azure_bluet, red_tulip, orange_tulip, white_tulip, pink_tulip, oxeye_daisy, cornflower, lily_of_the_valley
- [x] **AmethystClusterBlock** (4) - `canSurvive`, `getFluidState`, `getStateForPlacement`, `onProjectileHit`, `updateShape`  
  amethyst_cluster, large_amethyst_bud, medium_amethyst_bud, small_amethyst_bud
- [ ] **TallFlowerBlock** (4) - `canSurvive`, `getStateForPlacement`, `playerDestroy`, `playerWillDestroy`, `setPlacedBy`, `updateShape`  
  sunflower, lilac, rose_bush, peony
- [ ] **DoublePlantBlock** (3) - `canSurvive`, `getStateForPlacement`, `playerDestroy`, `playerWillDestroy`, `setPlacedBy`, `updateShape`  
  tall_grass, large_fern, pitcher_plant
- [x] **TorchBlock** (3) - `canSurvive`, `updateShape`  
  torch, soul_torch, copper_torch
- [x] **WallTorchBlock** (3) - `canSurvive`, `getStateForPlacement`, `updateShape`  
  wall_torch, soul_wall_torch, copper_wall_torch
- [x] **AzaleaBlock** (2) - `canSurvive`, `updateShape`  
  azalea, flowering_azalea
- [x] **BushBlock** (2) - `canSurvive`, `updateShape`  
  bush, red_shrub
- [ ] **EyeblossomBlock** (2) - `canSurvive`, `entityInside`, `randomTick`, `tick`, `updateShape`  
  open_eyeblossom, closed_eyeblossom
- [x] **FlowerBedBlock** (2) - `canBeReplaced(BlockState,BlockPlaceContext)`, `canSurvive`, `getStateForPlacement`, `updateShape`  
  pink_petals, wildflowers
- [ ] **MushroomBlock** (2) - `canSurvive`, `randomTick`, `updateShape`  
  brown_mushroom, red_mushroom
- [x] **NetherRootsBlock** (2) - `canSurvive`, `updateShape`  
  warped_roots, crimson_roots
- [x] **TallGrassBlock** (2) - `canSurvive`, `updateShape`  
  short_grass, fern
- [ ] **BigDripleafBlock** (1) - `canSurvive`, `entityInside`, `getFluidState`, `getStateForPlacement`, `neighborChanged`, `onProjectileHit`, `tick`, `updateShape`  
  big_dripleaf
- [ ] **BigDripleafStemBlock** (1) - `canSurvive`, `getCloneItemStack`, `getFluidState`, `tick`, `updateShape`  
  big_dripleaf_stem
- [x] **CactusFlowerBlock** (1) - `canSurvive`, `updateShape`  
  cactus_flower
- [ ] **CarpetBlock** (1) - `canSurvive`, `updateShape`  
  moss_carpet
- [ ] **DecoratedPotBlock** (1, *block entity*) - `affectNeighborsAfterRemoval`, `getAnalogOutputSignal`, `getCloneItemStack`, `getDrops`, `getFluidState`, `getMenuProvider`, `getStateForPlacement`, `hasAnalogOutputSignal`, `onProjectileHit`, `playerWillDestroy`, `triggerEvent`, `updateShape`, `useItemOn`, `useWithoutItem`  
  decorated_pot
- [x] **DryVegetationBlock** (1) - `canSurvive`, `updateShape`  
  dead_bush
- [x] **FireflyBushBlock** (1) - `canSurvive`, `updateShape`  
  firefly_bush
- [ ] **FrogspawnBlock** (1) - `canSurvive`, `entityInside`, `onPlace`, `tick`, `updateShape`  
  frogspawn
- [ ] **HangingRootsBlock** (1) - `canSurvive`, `getFluidState`, `getStateForPlacement`, `updateShape`  
  hanging_roots
- [ ] **LadderBlock** (1) - `canSurvive`, `getFluidState`, `getStateForPlacement`, `updateShape`  
  ladder
- [x] **LeafLitterBlock** (1) - `canBeReplaced(BlockState,BlockPlaceContext)`, `canSurvive`, `getStateForPlacement`, `updateShape`  
  leaf_litter
- [ ] **LilyPadBlock** (1) - `canSurvive`, `entityInside`, `updateShape`  
  lily_pad
- [x] **NetherSproutsBlock** (1) - `canSurvive`, `updateShape`  
  nether_sprouts
- [ ] **ShelfMushroomBlock** (1) - `bounceOn`, `canSurvive`, `getStateForPlacement`, `updateShape`  
  shelf_mushroom
- [x] **ShortDryGrassBlock** (1) - `canSurvive`, `updateShape`  
  short_dry_grass
- [ ] **SmallDripleafBlock** (1) - `canSurvive`, `getFluidState`, `getStateForPlacement`, `playerDestroy`, `playerWillDestroy`, `setPlacedBy`, `updateShape`  
  small_dripleaf
- [ ] **SnowLayerBlock** (1) - `canBeReplaced(BlockState,BlockPlaceContext)`, `canSurvive`, `getStateForPlacement`, `randomTick`, `updateShape`  
  snow
- [ ] **SporeBlossomBlock** (1) - `canSurvive`, `updateShape`  
  spore_blossom
- [x] **TallDryGrassBlock** (1) - `canSurvive`, `updateShape`  
  tall_dry_grass
- [ ] **WitherRoseBlock** (1) - `canSurvive`, `entityInside`, `updateShape`  
  wither_rose

## 4. Scheduled ticks and falling blocks (19 of 26 blocks)

Needs: The tick queue (`world::LevelTicks`) and `FallingBlockEntity` are there: sand, gravel, concrete powder. What is left hurts what it lands on (anvils: damage), teleports (the dragon egg) or has a block entity.

- [x] **ConcretePowderBlock** (16) - `getStateForPlacement`, `onPlace`, `tick`, `updateShape`  
  white_concrete_powder, orange_concrete_powder, magenta_concrete_powder, light_blue_concrete_powder, yellow_concrete_powder, lime_concrete_powder, pink_concrete_powder, gray_concrete_powder, light_gray_concrete_powder, cyan_concrete_powder, purple_concrete_powder, blue_concrete_powder, brown_concrete_powder, green_concrete_powder, red_concrete_powder, black_concrete_powder
- [ ] **AnvilBlock** (3) - `getMenuProvider`, `getStateForPlacement`, `onPlace`, `tick`, `updateShape`, `useWithoutItem`  
  anvil, chipped_anvil, damaged_anvil
- [ ] **BrushableBlock** (2, *block entity*) - `getMenuProvider`, `onPlace`, `tick`, `triggerEvent`, `updateShape`  
  suspicious_sand, suspicious_gravel
- [x] **SandBlock** (2) - `onPlace`, `tick`, `updateShape`  
  sand, red_sand
- [x] **ColoredFallingBlock** (1) - `onPlace`, `tick`, `updateShape`  
  gravel
- [ ] **DragonEggBlock** (1) - `attack`, `onPlace`, `tick`, `updateShape`, `useWithoutItem`  
  dragon_egg
- [ ] **ScaffoldingBlock** (1) - `canBeReplaced(BlockState,BlockPlaceContext)`, `canSurvive`, `getFluidState`, `getStateForPlacement`, `onPlace`, `tick`, `updateShape`  
  scaffolding

## 5. Fluids (2 of 53 blocks)

Needs: Water and lava: flowing (`FlowingFluid.tick`), waterlogging (`getFluidState`, `SimpleWaterloggedBlock`), buckets.

- [ ] **BaseCoralFanBlock** (5) - `canSurvive`, `getFluidState`, `getStateForPlacement`, `updateShape`  
  dead_tube_coral_fan, dead_brain_coral_fan, dead_bubble_coral_fan, dead_fire_coral_fan, dead_horn_coral_fan
- [ ] **BaseCoralPlantBlock** (5) - `canSurvive`, `getFluidState`, `getStateForPlacement`, `updateShape`  
  dead_tube_coral, dead_brain_coral, dead_bubble_coral, dead_fire_coral, dead_horn_coral
- [ ] **BaseCoralWallFanBlock** (5) - `canSurvive`, `getFluidState`, `getStateForPlacement`, `updateShape`  
  dead_tube_coral_wall_fan, dead_brain_coral_wall_fan, dead_bubble_coral_wall_fan, dead_fire_coral_wall_fan, dead_horn_coral_wall_fan
- [ ] **CoralBlock** (5) - `getStateForPlacement`, `tick`, `updateShape`  
  tube_coral_block, brain_coral_block, bubble_coral_block, fire_coral_block, horn_coral_block
- [ ] **CoralFanBlock** (5) - `canSurvive`, `getFluidState`, `getStateForPlacement`, `onPlace`, `tick`, `updateShape`  
  tube_coral_fan, brain_coral_fan, bubble_coral_fan, fire_coral_fan, horn_coral_fan
- [ ] **CoralPlantBlock** (5) - `canSurvive`, `getFluidState`, `getStateForPlacement`, `onPlace`, `tick`, `updateShape`  
  tube_coral, brain_coral, bubble_coral, fire_coral, horn_coral
- [ ] **CoralWallFanBlock** (5) - `canSurvive`, `getFluidState`, `getStateForPlacement`, `onPlace`, `tick`, `updateShape`  
  tube_coral_wall_fan, brain_coral_wall_fan, bubble_coral_wall_fan, fire_coral_wall_fan, horn_coral_wall_fan
- [ ] **LayeredCauldronBlock** (2) - `entityInside`, `getAnalogOutputSignal`, `handlePrecipitation`, `hasAnalogOutputSignal`, `tick`, `useItemOn`  
  water_cauldron, powder_snow_cauldron
- [x] **LiquidBlock** (2) - `getDrops`, `getFluidState`, `isRandomlyTicking`, `neighborChanged`, `onPlace`, `randomTick`, `tick`, `updateShape`  
  water, lava
- [ ] **BubbleColumnBlock** (1) - `canSurvive`, `entityInside`, `getFluidState`, `tick`, `updateShape`  
  bubble_column
- [ ] **CauldronBlock** (1) - `handlePrecipitation`, `hasAnalogOutputSignal`, `tick`, `useItemOn`  
  cauldron
- [ ] **ConduitBlock** (1, *block entity*) - `getFluidState`, `getMenuProvider`, `getStateForPlacement`, `triggerEvent`, `updateShape`  
  conduit
- [ ] **FrostedIceBlock** (1) - `getCloneItemStack`, `neighborChanged`, `onPlace`, `playerDestroy`, `randomTick`, `tick`  
  frosted_ice
- [ ] **KelpBlock** (1) - `canSurvive`, `getFluidState`, `getStateForPlacement`, `isRandomlyTicking`, `randomTick`, `tick`, `updateShape`  
  kelp
- [ ] **KelpPlantBlock** (1) - `canBeReplaced(BlockState,BlockPlaceContext)`, `canSurvive`, `getCloneItemStack`, `getFluidState`, `getStateForPlacement`, `tick`, `updateShape`  
  kelp_plant
- [ ] **LavaCauldronBlock** (1) - `entityInside`, `getAnalogOutputSignal`, `hasAnalogOutputSignal`, `tick`, `useItemOn`  
  lava_cauldron
- [ ] **MangrovePropaguleBlock** (1) - `canSurvive`, `getFluidState`, `getStateForPlacement`, `randomTick`, `updateShape`  
  mangrove_propagule
- [ ] **PowderSnowBlock** (1) - `entityInside`, `fallOn`  
  powder_snow
- [ ] **SeaPickleBlock** (1) - `canBeReplaced(BlockState,BlockPlaceContext)`, `canSurvive`, `getFluidState`, `getStateForPlacement`, `updateShape`  
  sea_pickle
- [ ] **SeagrassBlock** (1) - `canSurvive`, `getFluidState`, `getStateForPlacement`, `updateShape`  
  seagrass
- [ ] **SpongeBlock** (1) - `neighborChanged`, `onPlace`  
  sponge
- [ ] **TallSeagrassBlock** (1) - `canSurvive`, `getCloneItemStack`, `getFluidState`, `getStateForPlacement`, `playerDestroy`, `playerWillDestroy`, `setPlacedBy`, `updateShape`  
  tall_seagrass
- [ ] **WetSpongeBlock** (1) - `onPlace`  
  wet_sponge

## 6. Random ticks: growth, spread and decay (48 of 128 blocks)

Needs: Random block ticks are there, and sections whose ticks could do nothing are passed by: a class that is ported says in `state::kRandom` when its tick can act (`RANDOM_TICK_KINDS` in gen_game.py; docs/architecture.md, "Block updates"). Still needed: bone meal, tree and mushroom features for what grows into a structure, fire's spread.

- [x] **WeatheringCopperFullBlock** (12) - `isRandomlyTicking`, `randomTick`  
  copper_block, exposed_copper, weathered_copper, oxidized_copper, cut_copper, exposed_cut_copper, weathered_cut_copper, oxidized_cut_copper, chiseled_copper, exposed_chiseled_copper, weathered_chiseled_copper, oxidized_chiseled_copper
- [ ] **SaplingBlock** (9) - `canSurvive`, `randomTick`, `updateShape`  
  oak_sapling, spruce_sapling, birch_sapling, jungle_sapling, acacia_sapling, cherry_sapling, dark_oak_sapling, pale_oak_sapling, poplar_sapling
- [ ] **UntintedParticleLeavesBlock** (7) - `getFluidState`, `getStateForPlacement`, `isRandomlyTicking`, `randomTick`, `tick`, `updateShape`  
  cherry_leaves, pale_oak_leaves, red_poplar_leaves, orange_poplar_leaves, yellow_poplar_leaves, azalea_leaves, flowering_azalea_leaves
- [ ] **TintedParticleLeavesBlock** (5) - `getFluidState`, `getStateForPlacement`, `isRandomlyTicking`, `randomTick`, `tick`, `updateShape`  
  oak_leaves, birch_leaves, jungle_leaves, acacia_leaves, dark_oak_leaves
- [x] **WeatheringCopperBarsBlock** (4) - `getFluidState`, `getStateForPlacement`, `isRandomlyTicking`, `randomTick`, `updateShape`  
  copper_bars, exposed_copper_bars, weathered_copper_bars, oxidized_copper_bars
- [ ] **WeatheringCopperBulbBlock** (4) - `getAnalogOutputSignal`, `hasAnalogOutputSignal`, `isRandomlyTicking`, `neighborChanged`, `onPlace`, `randomTick`  
  copper_bulb, exposed_copper_bulb, weathered_copper_bulb, oxidized_copper_bulb
- [x] **WeatheringCopperChainBlock** (4) - `getFluidState`, `getStateForPlacement`, `isRandomlyTicking`, `randomTick`, `updateShape`  
  copper_chain, exposed_copper_chain, weathered_copper_chain, oxidized_copper_chain
- [x] **WeatheringCopperChestBlock** (4, *block entity*) - `affectNeighborsAfterRemoval`, `getAnalogOutputSignal`, `getFluidState`, `getMenuProvider`, `getStateForPlacement`, `hasAnalogOutputSignal`, `isRandomlyTicking`, `randomTick`, `shouldChangedStateKeepBlockEntity`, `tick`, `triggerEvent`, `updateShape`, `useWithoutItem`  
  copper_chest, exposed_copper_chest, weathered_copper_chest, oxidized_copper_chest
- [ ] **WeatheringCopperDoorBlock** (4) - `canSurvive`, `getStateForPlacement`, `isRandomlyTicking`, `neighborChanged`, `onExplosionHit`, `playerWillDestroy`, `randomTick`, `setPlacedBy`, `updateShape`, `useWithoutItem`  
  copper_door, exposed_copper_door, weathered_copper_door, oxidized_copper_door
- [ ] **WeatheringCopperGolemStatueBlock** (4, *block entity*) - `affectNeighborsAfterRemoval`, `getAnalogOutputSignal`, `getCloneItemStack`, `getFluidState`, `getMenuProvider`, `getStateForPlacement`, `hasAnalogOutputSignal`, `isRandomlyTicking`, `randomTick`, `shouldChangedStateKeepBlockEntity`, `triggerEvent`, `updateShape`, `useItemOn`  
  copper_golem_statue, exposed_copper_golem_statue, weathered_copper_golem_statue, oxidized_copper_golem_statue
- [x] **WeatheringCopperGrateBlock** (4) - `getFluidState`, `getStateForPlacement`, `isRandomlyTicking`, `randomTick`, `updateShape`  
  copper_grate, exposed_copper_grate, weathered_copper_grate, oxidized_copper_grate
- [x] **WeatheringCopperSlabBlock** (4) - `canBeReplaced(BlockState,BlockPlaceContext)`, `getFluidState`, `getStateForPlacement`, `isRandomlyTicking`, `randomTick`, `updateShape`  
  cut_copper_slab, exposed_cut_copper_slab, weathered_cut_copper_slab, oxidized_cut_copper_slab
- [x] **WeatheringCopperStairBlock** (4) - `getExplosionResistance`, `getFluidState`, `getStateForPlacement`, `isRandomlyTicking`, `randomTick`, `updateShape`  
  cut_copper_stairs, exposed_cut_copper_stairs, weathered_cut_copper_stairs, oxidized_cut_copper_stairs
- [ ] **WeatheringCopperTrapDoorBlock** (4) - `getFluidState`, `getStateForPlacement`, `isRandomlyTicking`, `neighborChanged`, `onExplosionHit`, `randomTick`, `updateShape`, `useWithoutItem`  
  copper_trapdoor, exposed_copper_trapdoor, weathered_copper_trapdoor, oxidized_copper_trapdoor
- [x] **WeatheringLanternBlock** (4) - `canSurvive`, `getFluidState`, `getStateForPlacement`, `isRandomlyTicking`, `randomTick`, `updateShape`  
  copper_lantern, exposed_copper_lantern, weathered_copper_lantern, oxidized_copper_lantern
- [ ] **WeatheringLightningRodBlock** (4) - `affectNeighborsAfterRemoval`, `getDirectSignal`, `getFluidState`, `getStateForPlacement`, `isRandomlyTicking`, `isSignalSource`, `onPlace`, `ownSignal`, `randomTick`, `tick`, `updateShape`  
  lightning_rod, exposed_lightning_rod, weathered_lightning_rod, oxidized_lightning_rod
- [ ] **AttachedStemBlock** (2) - `canSurvive`, `getCloneItemStack`, `updateShape`  
  attached_pumpkin_stem, attached_melon_stem
- [x] **NetherFungusBlock** (2) - `canSurvive`, `updateShape`  
  warped_fungus, crimson_fungus
- [x] **NyliumBlock** (2) - `randomTick`  
  warped_nylium, crimson_nylium
- [ ] **RedStoneOreBlock** (2) - `attack`, `isRandomlyTicking`, `randomTick`, `spawnAfterBreak`, `stepOn`, `useItemOn`  
  redstone_ore, deepslate_redstone_ore
- [ ] **StemBlock** (2) - `canSurvive`, `getCloneItemStack`, `randomTick`, `updateShape`  
  pumpkin_stem, melon_stem
- [ ] **BambooSaplingBlock** (1) - `canSurvive`, `getCloneItemStack`, `randomTick`, `updateShape`  
  bamboo_sapling
- [ ] **BambooStalkBlock** (1) - `canSurvive`, `getStateForPlacement`, `isRandomlyTicking`, `randomTick`, `tick`, `updateShape`  
  bamboo
- [ ] **BeetrootBlock** (1) - `canSurvive`, `entityInside`, `getCloneItemStack`, `isRandomlyTicking`, `randomTick`, `updateShape`  
  beetroots
- [x] **BuddingAmethystBlock** (1) - `onProjectileHit`, `randomTick`  
  budding_amethyst
- [ ] **CactusBlock** (1) - `canSurvive`, `entityInside`, `randomTick`, `tick`, `updateShape`  
  cactus
- [ ] **CarrotBlock** (1) - `canSurvive`, `entityInside`, `getCloneItemStack`, `isRandomlyTicking`, `randomTick`, `updateShape`  
  carrots
- [ ] **CaveVinesBlock** (1) - `canSurvive`, `getCloneItemStack`, `getStateForPlacement`, `isRandomlyTicking`, `randomTick`, `tick`, `updateShape`, `useWithoutItem`  
  cave_vines
- [ ] **CaveVinesPlantBlock** (1) - `canBeReplaced(BlockState,BlockPlaceContext)`, `canSurvive`, `getCloneItemStack`, `getStateForPlacement`, `tick`, `updateShape`, `useWithoutItem`  
  cave_vines_plant
- [ ] **ChorusFlowerBlock** (1) - `canSurvive`, `isRandomlyTicking`, `onProjectileHit`, `randomTick`, `tick`, `updateShape`  
  chorus_flower
- [ ] **ChorusPlantBlock** (1) - `canSurvive`, `getStateForPlacement`, `tick`, `updateShape`  
  chorus_plant
- [ ] **CocoaBlock** (1) - `canSurvive`, `getStateForPlacement`, `isRandomlyTicking`, `randomTick`, `updateShape`  
  cocoa
- [ ] **CropBlock** (1) - `canSurvive`, `entityInside`, `getCloneItemStack`, `isRandomlyTicking`, `randomTick`, `updateShape`  
  wheat
- [ ] **DriedGhastBlock** (1) - `getFluidState`, `getStateForPlacement`, `randomTick`, `setPlacedBy`, `tick`, `updateShape`  
  dried_ghast
- [ ] **FarmlandBlock** (1) - `canSurvive`, `fallOn`, `getStateForPlacement`, `randomTick`, `tick`, `updateShape`  
  farmland
- [ ] **FireBlock** (1) - `canSurvive`, `entityInside`, `getStateForPlacement`, `onPlace`, `playerWillDestroy`, `spawnDestroyByEntityParticles`, `tick`, `updateShape`  
  fire
- [x] **GrassBlock** (1) - `getStateForPlacement`, `randomTick`, `updateShape`  
  grass_block
- [ ] **LeavesBlock** (1) - `getFluidState`, `getStateForPlacement`, `isRandomlyTicking`, `randomTick`, `tick`, `updateShape`  
  spruce_leaves
- [ ] **MangroveLeavesBlock** (1) - `getFluidState`, `getStateForPlacement`, `isRandomlyTicking`, `randomTick`, `tick`, `updateShape`  
  mangrove_leaves
- [x] **MyceliumBlock** (1) - `getStateForPlacement`, `randomTick`, `updateShape`  
  mycelium
- [ ] **NetherWartBlock** (1) - `canSurvive`, `getCloneItemStack`, `isRandomlyTicking`, `randomTick`, `updateShape`  
  nether_wart
- [ ] **PitcherCropBlock** (1) - `canBeReplaced(BlockState,BlockPlaceContext)`, `canSurvive`, `entityInside`, `getStateForPlacement`, `isRandomlyTicking`, `playerDestroy`, `playerWillDestroy`, `randomTick`, `setPlacedBy`, `updateShape`  
  pitcher_crop
- [ ] **PotatoBlock** (1) - `canSurvive`, `entityInside`, `getCloneItemStack`, `isRandomlyTicking`, `randomTick`, `updateShape`  
  potatoes
- [ ] **PumpkinBlock** (1) - `useItemOn`  
  pumpkin
- [x] **SculkBlock** (1) - `spawnAfterBreak`  
  sculk
- [ ] **SculkCatalystBlock** (1, *block entity*) - `getMenuProvider`, `spawnAfterBreak`, `tick`, `triggerEvent`  
  sculk_catalyst
- [ ] **SnifferEggBlock** (1) - `onPlace`, `tick`  
  sniffer_egg
- [ ] **SoulFireBlock** (1) - `canSurvive`, `entityInside`, `getStateForPlacement`, `onPlace`, `playerWillDestroy`, `spawnDestroyByEntityParticles`, `updateShape`  
  soul_fire
- [ ] **SugarCaneBlock** (1) - `canSurvive`, `randomTick`, `tick`, `updateShape`  
  sugar_cane
- [ ] **SulfurSpikeBlock** (1) - `canSurvive`, `getFluidState`, `getStateForPlacement`, `onProjectileHit`, `randomTick`, `tick`, `updateShape`  
  sulfur_spike
- [ ] **SweetBerryBushBlock** (1) - `canSurvive`, `entityInside`, `getCloneItemStack`, `isRandomlyTicking`, `randomTick`, `updateShape`, `useItemOn`, `useWithoutItem`  
  sweet_berry_bush
- [ ] **TorchflowerCropBlock** (1) - `canSurvive`, `entityInside`, `getCloneItemStack`, `isRandomlyTicking`, `randomTick`, `updateShape`  
  torchflower_crop
- [ ] **TurtleEggBlock** (1) - `canBeReplaced(BlockState,BlockPlaceContext)`, `fallOn`, `getStateForPlacement`, `onPlace`, `playerDestroy`, `randomTick`, `stepOn`  
  turtle_egg
- [ ] **TwistingVinesBlock** (1) - `canSurvive`, `getStateForPlacement`, `isRandomlyTicking`, `randomTick`, `tick`, `updateShape`  
  twisting_vines
- [ ] **TwistingVinesPlantBlock** (1) - `canBeReplaced(BlockState,BlockPlaceContext)`, `canSurvive`, `getCloneItemStack`, `getStateForPlacement`, `tick`, `updateShape`  
  twisting_vines_plant
- [ ] **VineBlock** (1) - `canBeReplaced(BlockState,BlockPlaceContext)`, `canSurvive`, `getStateForPlacement`, `randomTick`, `updateShape`  
  vine
- [ ] **WeepingVinesBlock** (1) - `canSurvive`, `getStateForPlacement`, `isRandomlyTicking`, `randomTick`, `tick`, `updateShape`  
  weeping_vines
- [ ] **WeepingVinesPlantBlock** (1) - `canBeReplaced(BlockState,BlockPlaceContext)`, `canSurvive`, `getCloneItemStack`, `getStateForPlacement`, `tick`, `updateShape`  
  weeping_vines_plant

## 7. Blocks that answer a click without a menu (0 of 136 blocks)

Needs: `useItemOn` / `useWithoutItem` beyond opening a screen: the item in hand changes, the block changes state, a sound plays.

- [ ] **FlowerPotBlock** (40) - `getCloneItemStack`, `isRandomlyTicking`, `randomTick`, `updateShape`, `useItemOn`, `useWithoutItem`  
  flower_pot, potted_torchflower, potted_oak_sapling, potted_spruce_sapling, potted_birch_sapling, potted_jungle_sapling, potted_acacia_sapling, potted_cherry_sapling, potted_dark_oak_sapling, potted_pale_oak_sapling, potted_poplar_sapling, potted_mangrove_propagule, potted_fern, potted_dandelion, potted_golden_dandelion, potted_poppy, potted_blue_orchid, potted_allium, potted_azure_bluet, potted_red_tulip, potted_orange_tulip, potted_white_tulip, potted_pink_tulip, potted_oxeye_daisy, potted_cornflower, potted_lily_of_the_valley, potted_wither_rose, potted_red_mushroom, potted_brown_mushroom, potted_dead_bush, potted_cactus, potted_bamboo, potted_crimson_fungus, potted_warped_fungus, potted_crimson_roots, potted_warped_roots, potted_azalea_bush, potted_flowering_azalea_bush, potted_open_eyeblossom, potted_closed_eyeblossom
- [ ] **DoorBlock** (18) - `canSurvive`, `getStateForPlacement`, `neighborChanged`, `onExplosionHit`, `playerWillDestroy`, `setPlacedBy`, `updateShape`, `useWithoutItem`  
  oak_door, iron_door, spruce_door, birch_door, jungle_door, acacia_door, cherry_door, dark_oak_door, pale_oak_door, mangrove_door, poplar_door, bamboo_door, crimson_door, warped_door, waxed_copper_door, waxed_exposed_copper_door, waxed_weathered_copper_door, waxed_oxidized_copper_door
- [ ] **TrapDoorBlock** (18) - `getFluidState`, `getStateForPlacement`, `neighborChanged`, `onExplosionHit`, `updateShape`, `useWithoutItem`  
  oak_trapdoor, spruce_trapdoor, birch_trapdoor, jungle_trapdoor, acacia_trapdoor, cherry_trapdoor, dark_oak_trapdoor, pale_oak_trapdoor, mangrove_trapdoor, poplar_trapdoor, bamboo_trapdoor, iron_trapdoor, crimson_trapdoor, warped_trapdoor, waxed_copper_trapdoor, waxed_exposed_copper_trapdoor, waxed_weathered_copper_trapdoor, waxed_oxidized_copper_trapdoor
- [ ] **CandleBlock** (17) - `canBeReplaced(BlockState,BlockPlaceContext)`, `canSurvive`, `getFluidState`, `getStateForPlacement`, `onExplosionHit`, `onProjectileHit`, `updateShape`, `useItemOn`  
  candle, white_candle, orange_candle, magenta_candle, light_blue_candle, yellow_candle, lime_candle, pink_candle, gray_candle, light_gray_candle, cyan_candle, purple_candle, blue_candle, brown_candle, green_candle, red_candle, black_candle
- [ ] **CandleCakeBlock** (17) - `canSurvive`, `getAnalogOutputSignal`, `getCloneItemStack`, `hasAnalogOutputSignal`, `onExplosionHit`, `onProjectileHit`, `updateShape`, `useItemOn`, `useWithoutItem`  
  candle_cake, white_candle_cake, orange_candle_cake, magenta_candle_cake, light_blue_candle_cake, yellow_candle_cake, lime_candle_cake, pink_candle_cake, gray_candle_cake, light_gray_candle_cake, cyan_candle_cake, purple_candle_cake, blue_candle_cake, brown_candle_cake, green_candle_cake, red_candle_cake, black_candle_cake
- [ ] **FenceGateBlock** (13) - `getStateForPlacement`, `neighborChanged`, `onExplosionHit`, `updateShape`, `useWithoutItem`  
  oak_fence_gate, spruce_fence_gate, birch_fence_gate, jungle_fence_gate, acacia_fence_gate, cherry_fence_gate, dark_oak_fence_gate, pale_oak_fence_gate, mangrove_fence_gate, poplar_fence_gate, bamboo_fence_gate, crimson_fence_gate, warped_fence_gate
- [ ] **CopperBulbBlock** (4) - `getAnalogOutputSignal`, `hasAnalogOutputSignal`, `neighborChanged`, `onPlace`  
  waxed_copper_bulb, waxed_exposed_copper_bulb, waxed_weathered_copper_bulb, waxed_oxidized_copper_bulb
- [ ] **LightningRodBlock** (4) - `affectNeighborsAfterRemoval`, `getDirectSignal`, `getFluidState`, `getStateForPlacement`, `isSignalSource`, `onPlace`, `ownSignal`, `tick`, `updateShape`  
  waxed_lightning_rod, waxed_exposed_lightning_rod, waxed_weathered_lightning_rod, waxed_oxidized_lightning_rod
- [ ] **CakeBlock** (1) - `canSurvive`, `getAnalogOutputSignal`, `hasAnalogOutputSignal`, `updateShape`, `useItemOn`, `useWithoutItem`  
  cake
- [ ] **ComposterBlock** (1) - `getAnalogOutputSignal`, `hasAnalogOutputSignal`, `onPlace`, `tick`, `useItemOn`, `useWithoutItem`  
  composter
- [ ] **EndPortalFrameBlock** (1) - `getAnalogOutputSignal`, `getStateForPlacement`, `hasAnalogOutputSignal`  
  end_portal_frame
- [ ] **RespawnAnchorBlock** (1) - `getAnalogOutputSignal`, `hasAnalogOutputSignal`, `useItemOn`, `useWithoutItem`  
  respawn_anchor
- [ ] **StrawBedBlock** (1) - `getStateForPlacement`, `playerWillDestroy`, `setPlacedBy`, `updateShape`, `useWithoutItem`  
  straw_bed

## 8. Menus without a block entity (0 of 5 blocks)

Needs: The menu classes and their recipes: `AnvilMenu`, `GrindstoneMenu`, `LoomMenu`, `CartographyTableMenu`, `SmithingMenu`, `StonecutterMenu` (and the stonecutter recipes in `update_recipes`).

- [ ] **CartographyTableBlock** (1) - `getMenuProvider`, `useWithoutItem`  
  cartography_table
- [ ] **GrindstoneBlock** (1) - `canSurvive`, `getMenuProvider`, `getStateForPlacement`, `updateShape`, `useWithoutItem`  
  grindstone
- [ ] **LoomBlock** (1) - `getMenuProvider`, `getStateForPlacement`, `useWithoutItem`  
  loom
- [ ] **SmithingTableBlock** (1) - `getMenuProvider`, `useWithoutItem`  
  smithing_table
- [ ] **StonecutterBlock** (1) - `getMenuProvider`, `getStateForPlacement`, `useWithoutItem`  
  stonecutter

## 9. Block entities: containers (7 of 49 blocks)

Needs: The chest's machinery again - a block entity with stacks, a menu, what the client is told - plus each one's own tick or rule: hoppers move items, shulker boxes keep theirs when broken (stacks inside stacks).

- [ ] **ShulkerBoxBlock** (17, *block entity*) - `affectNeighborsAfterRemoval`, `getAnalogOutputSignal`, `getDrops`, `getMenuProvider`, `getStateForPlacement`, `hasAnalogOutputSignal`, `playerWillDestroy`, `triggerEvent`, `useWithoutItem`  
  shulker_box, white_shulker_box, orange_shulker_box, magenta_shulker_box, light_blue_shulker_box, yellow_shulker_box, lime_shulker_box, pink_shulker_box, gray_shulker_box, light_gray_shulker_box, cyan_shulker_box, purple_shulker_box, blue_shulker_box, brown_shulker_box, green_shulker_box, red_shulker_box, black_shulker_box
- [ ] **ShelfBlock** (13, *block entity*) - `affectNeighborsAfterRemoval`, `getAnalogOutputSignal`, `getFluidState`, `getMenuProvider`, `getStateForPlacement`, `hasAnalogOutputSignal`, `neighborChanged`, `onPlace`, `triggerEvent`, `updateShape`, `useItemOn`  
  acacia_shelf, bamboo_shelf, birch_shelf, cherry_shelf, crimson_shelf, dark_oak_shelf, jungle_shelf, mangrove_shelf, oak_shelf, pale_oak_shelf, poplar_shelf, spruce_shelf, warped_shelf
- [x] **CopperChestBlock** (4, *block entity*) - `affectNeighborsAfterRemoval`, `getAnalogOutputSignal`, `getFluidState`, `getMenuProvider`, `getStateForPlacement`, `hasAnalogOutputSignal`, `shouldChangedStateKeepBlockEntity`, `tick`, `triggerEvent`, `updateShape`, `useWithoutItem`  
  waxed_copper_chest, waxed_exposed_copper_chest, waxed_weathered_copper_chest, waxed_oxidized_copper_chest
- [ ] **CampfireBlock** (2, *block entity*) - `entityInside`, `getFluidState`, `getMenuProvider`, `getStateForPlacement`, `onProjectileHit`, `triggerEvent`, `updateShape`, `useItemOn`  
  campfire, soul_campfire
- [x] **BarrelBlock** (1, *block entity*) - `affectNeighborsAfterRemoval`, `getAnalogOutputSignal`, `getMenuProvider`, `getStateForPlacement`, `hasAnalogOutputSignal`, `tick`, `triggerEvent`, `useWithoutItem`  
  barrel
- [x] **BlastFurnaceBlock** (1, *block entity*) - `affectNeighborsAfterRemoval`, `getAnalogOutputSignal`, `getMenuProvider`, `getStateForPlacement`, `hasAnalogOutputSignal`, `triggerEvent`, `useWithoutItem`  
  blast_furnace
- [ ] **BrewingStandBlock** (1, *block entity*) - `affectNeighborsAfterRemoval`, `getAnalogOutputSignal`, `getMenuProvider`, `hasAnalogOutputSignal`, `triggerEvent`, `useWithoutItem`  
  brewing_stand
- [ ] **ChiseledBookShelfBlock** (1, *block entity*) - `affectNeighborsAfterRemoval`, `getAnalogOutputSignal`, `getMenuProvider`, `getStateForPlacement`, `hasAnalogOutputSignal`, `triggerEvent`, `useItemOn`, `useWithoutItem`  
  chiseled_bookshelf
- [ ] **CrafterBlock** (1, *block entity*) - `affectNeighborsAfterRemoval`, `getAnalogOutputSignal`, `getMenuProvider`, `getStateForPlacement`, `hasAnalogOutputSignal`, `neighborChanged`, `setPlacedBy`, `tick`, `triggerEvent`, `useWithoutItem`  
  crafter
- [ ] **DispenserBlock** (1, *block entity*) - `affectNeighborsAfterRemoval`, `getAnalogOutputSignal`, `getMenuProvider`, `getStateForPlacement`, `hasAnalogOutputSignal`, `neighborChanged`, `tick`, `triggerEvent`, `useWithoutItem`  
  dispenser
- [ ] **DropperBlock** (1, *block entity*) - `affectNeighborsAfterRemoval`, `getAnalogOutputSignal`, `getMenuProvider`, `getStateForPlacement`, `hasAnalogOutputSignal`, `neighborChanged`, `tick`, `triggerEvent`, `useWithoutItem`  
  dropper
- [ ] **EnderChestBlock** (1, *block entity*) - `getFluidState`, `getMenuProvider`, `getStateForPlacement`, `tick`, `triggerEvent`, `updateShape`, `useWithoutItem`  
  ender_chest
- [ ] **HopperBlock** (1, *block entity*) - `affectNeighborsAfterRemoval`, `entityInside`, `getAnalogOutputSignal`, `getMenuProvider`, `getStateForPlacement`, `hasAnalogOutputSignal`, `neighborChanged`, `onPlace`, `triggerEvent`, `useWithoutItem`  
  hopper
- [ ] **JukeboxBlock** (1, *block entity*) - `affectNeighborsAfterRemoval`, `getAnalogOutputSignal`, `getMenuProvider`, `hasAnalogOutputSignal`, `isSignalSource`, `ownSignal`, `setPlacedBy`, `triggerEvent`, `useItemOn`, `useWithoutItem`  
  jukebox
- [ ] **LecternBlock** (1, *block entity*) - `affectNeighborsAfterRemoval`, `getAnalogOutputSignal`, `getDirectSignal`, `getMenuProvider`, `getStateForPlacement`, `hasAnalogOutputSignal`, `isSignalSource`, `ownSignal`, `tick`, `triggerEvent`, `useItemOn`, `useWithoutItem`  
  lectern
- [x] **SmokerBlock** (1, *block entity*) - `affectNeighborsAfterRemoval`, `getAnalogOutputSignal`, `getMenuProvider`, `getStateForPlacement`, `hasAnalogOutputSignal`, `triggerEvent`, `useWithoutItem`  
  smoker
- [ ] **TrappedChestBlock** (1, *block entity*) - `affectNeighborsAfterRemoval`, `getAnalogOutputSignal`, `getDirectSignal`, `getFluidState`, `getMenuProvider`, `getStateForPlacement`, `hasAnalogOutputSignal`, `isSignalSource`, `ownSignal`, `tick`, `triggerEvent`, `updateShape`, `useWithoutItem`  
  trapped_chest

## 10. Block entities: what a client is shown (52 of 132 blocks)

Needs: Block entities whose data travels to the client: `getUpdateTag` in the chunk packet, `block_entity_data` on change (`nbt::Writer` is there), text through `text::transcode`.

- [ ] **BannerBlock** (16, *block entity*) - `canSurvive`, `getCloneItemStack`, `getMenuProvider`, `getStateForPlacement`, `isPossibleToRespawnInThis`, `triggerEvent`, `updateShape`  
  white_banner, orange_banner, magenta_banner, light_blue_banner, yellow_banner, lime_banner, pink_banner, gray_banner, light_gray_banner, cyan_banner, purple_banner, blue_banner, brown_banner, green_banner, red_banner, black_banner
- [ ] **BedBlock** (16) - `getStateForPlacement`, `playerWillDestroy`, `setPlacedBy`, `updateShape`, `useWithoutItem`  
  white_bed, orange_bed, magenta_bed, light_blue_bed, yellow_bed, lime_bed, pink_bed, gray_bed, light_gray_bed, cyan_bed, purple_bed, blue_bed, brown_bed, green_bed, red_bed, black_bed
- [ ] **WallBannerBlock** (16, *block entity*) - `canSurvive`, `getCloneItemStack`, `getMenuProvider`, `getStateForPlacement`, `isPossibleToRespawnInThis`, `triggerEvent`, `updateShape`  
  white_wall_banner, orange_wall_banner, magenta_wall_banner, light_blue_wall_banner, yellow_wall_banner, lime_wall_banner, pink_wall_banner, gray_wall_banner, light_gray_wall_banner, cyan_wall_banner, purple_wall_banner, blue_wall_banner, brown_wall_banner, green_wall_banner, red_wall_banner, black_wall_banner
- [x] **CeilingHangingSignBlock** (13, *block entity*) - `canSurvive`, `getFluidState`, `getMenuProvider`, `getStateForPlacement`, `isPossibleToRespawnInThis`, `setPlacedBy`, `triggerEvent`, `updateShape`, `useItemOn`, `useWithoutItem`  
  oak_hanging_sign, spruce_hanging_sign, birch_hanging_sign, acacia_hanging_sign, cherry_hanging_sign, jungle_hanging_sign, dark_oak_hanging_sign, pale_oak_hanging_sign, crimson_hanging_sign, warped_hanging_sign, mangrove_hanging_sign, poplar_hanging_sign, bamboo_hanging_sign
- [x] **StandingSignBlock** (13, *block entity*) - `canSurvive`, `getFluidState`, `getMenuProvider`, `getStateForPlacement`, `isPossibleToRespawnInThis`, `setPlacedBy`, `triggerEvent`, `updateShape`, `useItemOn`, `useWithoutItem`  
  oak_sign, spruce_sign, birch_sign, acacia_sign, cherry_sign, jungle_sign, dark_oak_sign, pale_oak_sign, mangrove_sign, poplar_sign, bamboo_sign, crimson_sign, warped_sign
- [x] **WallHangingSignBlock** (13, *block entity*) - `getFluidState`, `getMenuProvider`, `getStateForPlacement`, `isPossibleToRespawnInThis`, `setPlacedBy`, `triggerEvent`, `updateShape`, `useItemOn`, `useWithoutItem`  
  oak_wall_hanging_sign, spruce_wall_hanging_sign, birch_wall_hanging_sign, acacia_wall_hanging_sign, cherry_wall_hanging_sign, jungle_wall_hanging_sign, dark_oak_wall_hanging_sign, pale_oak_wall_hanging_sign, mangrove_wall_hanging_sign, poplar_wall_hanging_sign, crimson_wall_hanging_sign, warped_wall_hanging_sign, bamboo_wall_hanging_sign
- [x] **WallSignBlock** (13, *block entity*) - `canSurvive`, `getFluidState`, `getMenuProvider`, `getStateForPlacement`, `isPossibleToRespawnInThis`, `setPlacedBy`, `triggerEvent`, `updateShape`, `useItemOn`, `useWithoutItem`  
  oak_wall_sign, spruce_wall_sign, birch_wall_sign, acacia_wall_sign, cherry_wall_sign, jungle_wall_sign, dark_oak_wall_sign, pale_oak_wall_sign, mangrove_wall_sign, poplar_wall_sign, bamboo_wall_sign, crimson_wall_sign, warped_wall_sign
- [ ] **SkullBlock** (5, *block entity*) - `getMenuProvider`, `getStateForPlacement`, `neighborChanged`, `triggerEvent`  
  skeleton_skull, zombie_head, creeper_head, dragon_head, piglin_head
- [ ] **CopperGolemStatueBlock** (4, *block entity*) - `affectNeighborsAfterRemoval`, `getAnalogOutputSignal`, `getCloneItemStack`, `getFluidState`, `getMenuProvider`, `getStateForPlacement`, `hasAnalogOutputSignal`, `shouldChangedStateKeepBlockEntity`, `triggerEvent`, `updateShape`, `useItemOn`  
  waxed_copper_golem_statue, waxed_exposed_copper_golem_statue, waxed_weathered_copper_golem_statue, waxed_oxidized_copper_golem_statue
- [ ] **WallSkullBlock** (4, *block entity*) - `getMenuProvider`, `getStateForPlacement`, `neighborChanged`, `triggerEvent`  
  skeleton_wall_skull, zombie_wall_head, creeper_wall_head, dragon_wall_head
- [ ] **BeehiveBlock** (2, *block entity*) - `getAnalogOutputSignal`, `getCloneItemStack`, `getDrops`, `getMenuProvider`, `getStateForPlacement`, `hasAnalogOutputSignal`, `onExplosionHit`, `playerDestroy`, `playerWillDestroy`, `triggerEvent`, `updateShape`, `useItemOn`  
  bee_nest, beehive
- [ ] **BeaconBlock** (1, *block entity*) - `getMenuProvider`, `triggerEvent`, `useWithoutItem`  
  beacon
- [ ] **BellBlock** (1, *block entity*) - `canSurvive`, `getMenuProvider`, `getStateForPlacement`, `neighborChanged`, `onExplosionHit`, `onProjectileHit`, `triggerEvent`, `updateShape`, `useWithoutItem`  
  bell
- [ ] **CalibratedSculkSensorBlock** (1, *block entity*) - `affectNeighborsAfterRemoval`, `getAnalogOutputSignal`, `getDirectSignal`, `getFluidState`, `getMenuProvider`, `getSignal`, `getStateForPlacement`, `hasAnalogOutputSignal`, `isSignalSource`, `onPlace`, `ownSignal`, `spawnAfterBreak`, `stepOn`, `tick`, `triggerEvent`, `updateShape`  
  calibrated_sculk_sensor
- [ ] **CreakingHeartBlock** (1, *block entity*) - `affectNeighborsAfterRemoval`, `getAnalogOutputSignal`, `getMenuProvider`, `getStateForPlacement`, `hasAnalogOutputSignal`, `onExplosionHit`, `playerWillDestroy`, `tick`, `triggerEvent`, `updateShape`  
  creaking_heart
- [ ] **EnchantingTableBlock** (1, *block entity*) - `getMenuProvider`, `triggerEvent`, `useWithoutItem`  
  enchanting_table
- [ ] **PiglinWallSkullBlock** (1, *block entity*) - `getMenuProvider`, `getStateForPlacement`, `neighborChanged`, `triggerEvent`  
  piglin_wall_head
- [ ] **PistonHeadBlock** (1) - `affectNeighborsAfterRemoval`, `canSurvive`, `getCloneItemStack`, `neighborChanged`, `playerWillDestroy`, `updateShape`  
  piston_head
- [ ] **PlayerHeadBlock** (1, *block entity*) - `getMenuProvider`, `getStateForPlacement`, `neighborChanged`, `triggerEvent`  
  player_head
- [ ] **PlayerWallHeadBlock** (1, *block entity*) - `getMenuProvider`, `getStateForPlacement`, `neighborChanged`, `triggerEvent`  
  player_wall_head
- [ ] **PotentSulfurBlock** (1, *block entity*) - `getMenuProvider`, `getStateForPlacement`, `onPlace`, `triggerEvent`, `updateShape`  
  potent_sulfur
- [ ] **SculkSensorBlock** (1, *block entity*) - `affectNeighborsAfterRemoval`, `getAnalogOutputSignal`, `getDirectSignal`, `getFluidState`, `getMenuProvider`, `getStateForPlacement`, `hasAnalogOutputSignal`, `isSignalSource`, `onPlace`, `ownSignal`, `spawnAfterBreak`, `stepOn`, `tick`, `triggerEvent`, `updateShape`  
  sculk_sensor
- [ ] **SculkShriekerBlock** (1, *block entity*) - `getFluidState`, `getMenuProvider`, `getStateForPlacement`, `spawnAfterBreak`, `stepOn`, `tick`, `triggerEvent`, `updateShape`  
  sculk_shrieker
- [ ] **SpawnerBlock** (1, *block entity*) - `getMenuProvider`, `spawnAfterBreak`, `triggerEvent`  
  spawner
- [ ] **TrialSpawnerBlock** (1, *block entity*) - `getMenuProvider`, `triggerEvent`  
  trial_spawner
- [ ] **VaultBlock** (1, *block entity*) - `getMenuProvider`, `getStateForPlacement`, `triggerEvent`, `useItemOn`  
  vault
- [ ] **WitherSkullBlock** (1, *block entity*) - `getMenuProvider`, `getStateForPlacement`, `neighborChanged`, `setPlacedBy`, `triggerEvent`  
  wither_skeleton_skull
- [ ] **WitherWallSkullBlock** (1, *block entity*) - `getMenuProvider`, `getStateForPlacement`, `neighborChanged`, `setPlacedBy`, `triggerEvent`  
  wither_skeleton_wall_skull

## 11. Redstone (1 of 52 blocks)

Needs: Signals (`SignalGetter`) beyond "is there a block of redstone beside me". Neighbour updates in vanilla's order and scheduled ticks are there; TNT and the lamp ask for a neighbour signal, a block of redstone is the one source so far.

- [ ] **ButtonBlock** (15) - `affectNeighborsAfterRemoval`, `canSurvive`, `entityInside`, `getDirectSignal`, `getStateForPlacement`, `isSignalSource`, `onExplosionHit`, `ownSignal`, `tick`, `updateShape`, `useWithoutItem`  
  stone_button, oak_button, spruce_button, birch_button, jungle_button, acacia_button, cherry_button, dark_oak_button, pale_oak_button, mangrove_button, poplar_button, bamboo_button, crimson_button, warped_button, polished_blackstone_button
- [ ] **PressurePlateBlock** (15) - `affectNeighborsAfterRemoval`, `canSurvive`, `entityInside`, `getDirectSignal`, `isPossibleToRespawnInThis`, `isSignalSource`, `ownSignal`, `tick`, `updateShape`  
  stone_pressure_plate, oak_pressure_plate, spruce_pressure_plate, birch_pressure_plate, jungle_pressure_plate, acacia_pressure_plate, cherry_pressure_plate, dark_oak_pressure_plate, pale_oak_pressure_plate, mangrove_pressure_plate, poplar_pressure_plate, bamboo_pressure_plate, crimson_pressure_plate, warped_pressure_plate, polished_blackstone_pressure_plate
- [ ] **PistonBaseBlock** (2) - `getStateForPlacement`, `neighborChanged`, `onPlace`, `setPlacedBy`, `triggerEvent`  
  sticky_piston, piston
- [ ] **PoweredRailBlock** (2) - `affectNeighborsAfterRemoval`, `canSurvive`, `getFluidState`, `getStateForPlacement`, `neighborChanged`, `onPlace`, `updateShape`  
  powered_rail, activator_rail
- [ ] **WeightedPressurePlateBlock** (2) - `affectNeighborsAfterRemoval`, `canSurvive`, `entityInside`, `getDirectSignal`, `isPossibleToRespawnInThis`, `isSignalSource`, `ownSignal`, `tick`, `updateShape`  
  light_weighted_pressure_plate, heavy_weighted_pressure_plate
- [ ] **ComparatorBlock** (1, *block entity*) - `affectNeighborsAfterRemoval`, `canSurvive`, `getDirectSignal`, `getSignal`, `getStateForPlacement`, `isSignalSource`, `neighborChanged`, `onPlace`, `ownSignal`, `setPlacedBy`, `tick`, `triggerEvent`, `updateShape`, `useWithoutItem`  
  comparator
- [ ] **DaylightDetectorBlock** (1, *block entity*) - `getMenuProvider`, `isSignalSource`, `ownSignal`, `triggerEvent`, `useWithoutItem`  
  daylight_detector
- [ ] **DetectorRailBlock** (1) - `affectNeighborsAfterRemoval`, `canSurvive`, `entityInside`, `getAnalogOutputSignal`, `getDirectSignal`, `getFluidState`, `getStateForPlacement`, `hasAnalogOutputSignal`, `isSignalSource`, `neighborChanged`, `onPlace`, `ownSignal`, `tick`, `updateShape`  
  detector_rail
- [ ] **LeverBlock** (1) - `affectNeighborsAfterRemoval`, `canSurvive`, `getDirectSignal`, `getStateForPlacement`, `isSignalSource`, `onExplosionHit`, `ownSignal`, `updateShape`, `useWithoutItem`  
  lever
- [ ] **MovingPistonBlock** (1, *block entity*) - `destroy`, `getCloneItemStack`, `getDrops`, `getMenuProvider`, `triggerEvent`, `useWithoutItem`  
  moving_piston
- [ ] **NoteBlock** (1) - `attack`, `getStateForPlacement`, `neighborChanged`, `triggerEvent`, `updateShape`, `useItemOn`, `useWithoutItem`  
  note_block
- [ ] **ObserverBlock** (1) - `affectNeighborsAfterRemoval`, `getDirectSignal`, `getSignal`, `getStateForPlacement`, `isSignalSource`, `onPlace`, `ownSignal`, `shouldRedstoneWireConnectTo`, `tick`, `updateShape`  
  observer
- [ ] **RailBlock** (1) - `affectNeighborsAfterRemoval`, `canSurvive`, `getFluidState`, `getStateForPlacement`, `neighborChanged`, `onPlace`, `updateShape`  
  rail
- [x] **RedstoneLampBlock** (1) - `getStateForPlacement`, `neighborChanged`, `tick`  
  redstone_lamp
- [ ] **RedstoneTorchBlock** (1) - `affectNeighborsAfterRemoval`, `canSurvive`, `getDirectSignal`, `getSignal`, `isSignalSource`, `neighborChanged`, `onPlace`, `ownSignal`, `tick`, `updateShape`  
  redstone_torch
- [ ] **RedstoneWallTorchBlock** (1) - `affectNeighborsAfterRemoval`, `canSurvive`, `getDirectSignal`, `getSignal`, `getStateForPlacement`, `isSignalSource`, `neighborChanged`, `onPlace`, `ownSignal`, `tick`, `updateShape`  
  redstone_wall_torch
- [ ] **RedstoneWireBlock** (1) - `affectNeighborsAfterRemoval`, `canSurvive`, `getDirectSignal`, `getSignal`, `getStateForPlacement`, `isSignalSource`, `neighborChanged`, `onPlace`, `ownSignal`, `shouldRedstoneWireConnectTo`, `updateIndirectNeighbourShapes`, `updateShape`, `useWithoutItem`  
  redstone_wire
- [ ] **RepeaterBlock** (1) - `affectNeighborsAfterRemoval`, `canSurvive`, `getDirectSignal`, `getSignal`, `getStateForPlacement`, `isSignalSource`, `neighborChanged`, `onPlace`, `ownSignal`, `setPlacedBy`, `shouldRedstoneWireConnectTo`, `tick`, `updateShape`, `useWithoutItem`  
  repeater
- [ ] **TargetBlock** (1) - `isSignalSource`, `onPlace`, `onProjectileHit`, `ownSignal`, `tick`  
  target
- [ ] **TripWireBlock** (1) - `affectNeighborsAfterRemoval`, `entityInside`, `getStateForPlacement`, `onPlace`, `playerWillDestroy`, `tick`, `updateShape`  
  tripwire
- [ ] **TripWireHookBlock** (1) - `affectNeighborsAfterRemoval`, `canSurvive`, `getDirectSignal`, `getStateForPlacement`, `isSignalSource`, `ownSignal`, `setPlacedBy`, `tick`, `updateShape`  
  tripwire_hook

## 12. Portals and technical blocks (0 of 10 blocks)

Needs: Other dimensions; operators and commands; structures.

- [ ] **CommandBlock** (3, *block entity*) - `getAnalogOutputSignal`, `getMenuProvider`, `getStateForPlacement`, `hasAnalogOutputSignal`, `neighborChanged`, `setPlacedBy`, `tick`, `triggerEvent`, `useWithoutItem`  
  command_block, repeating_command_block, chain_command_block
- [ ] **EndGatewayBlock** (1, *block entity*) - `canBeReplaced(BlockState,Fluid)`, `entityInside`, `getCloneItemStack`, `getMenuProvider`, `triggerEvent`  
  end_gateway
- [ ] **EndPortalBlock** (1, *block entity*) - `canBeReplaced(BlockState,Fluid)`, `entityInside`, `getCloneItemStack`, `getMenuProvider`, `triggerEvent`  
  end_portal
- [ ] **JigsawBlock** (1, *block entity*) - `getStateForPlacement`, `useWithoutItem`  
  jigsaw
- [ ] **NetherPortalBlock** (1) - `entityInside`, `getCloneItemStack`, `randomTick`, `updateShape`  
  nether_portal
- [ ] **StructureBlock** (1, *block entity*) - `getMenuProvider`, `neighborChanged`, `setPlacedBy`, `triggerEvent`, `useWithoutItem`  
  structure_block
- [ ] **TestBlock** (1, *block entity*) - `getCloneItemStack`, `getMenuProvider`, `getStateForPlacement`, `neighborChanged`, `ownSignal`, `tick`, `triggerEvent`, `useWithoutItem`  
  test_block
- [ ] **TestInstanceBlock** (1, *block entity*) - `getMenuProvider`, `triggerEvent`, `useWithoutItem`  
  test_instance_block
