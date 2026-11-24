---
name: mc-reference
description: Look up how vanilla Minecraft actually works by reading the official (unobfuscated) jar — packet wire formats, packet ids, registry contents, the order the server sends things, and what the client does with a packet. Use before writing or changing any packet layout, whenever an id or constant is needed, and whenever a question starts with "what does vanilla do when…". Never answer these from memory or a wiki.
---

# Reading vanilla

Since 26.1 Mojang ships jars with real class and method names. `tools/mcgen/mcgen.py` keeps them in
`.cache/mc/<version>/` and wraps `javap`, which only *reads* class files — nothing from the jar is executed.

```sh
python3 tools/mcgen/mcgen.py fetch [--client]            # once per machine; --client also downloads the client jar
python3 tools/mcgen/mcgen.py find 'protocol/game/Clientbound.*Chunk'        # class names by regex
python3 tools/mcgen/mcgen.py codec net.minecraft.network.protocol.game.ClientboundLoginPacket   # wire format, compact
python3 tools/mcgen/mcgen.py javap <class> --method 'handleHello|tick\('     # filtered bytecode of some methods
python3 tools/mcgen/mcgen.py javap <class> --full                            # raw javap -p -c
```

Add `--client` to `find`/`codec`/`javap` for client-only classes (`net.minecraft.client.*`). Shell note: quote
class names containing `$` (`'…ClientboundPlayerInfoUpdatePacket$Action'`).

## Workflow for a packet layout

1. Find the class: packet names in `src/proto/gen/*/packet_ids.hpp` map to classes as
   `set_chunk_cache_center` → `ClientboundSetChunkCacheCenterPacket`; packages are
   `protocol.{handshake,status,login,configuration,game,common,ping,cookie}`.
2. `mcgen.py codec <class>`: fields are listed in declaration order, then one line per codec-bearing method.
   - `static: A B C composite()` → fields are encoded in exactly that order (`StreamCodec.composite`).
   - a constructor line plus `write: …` → hand-written codec; the `write` line is the wire order.
   - `static: unit()` → no payload.
3. Recurse into every nested type (`CommonPlayerSpawnInfo.STREAM_CODEC`, `GlobalPos`, …) the same way. When a
   line says `this.X` or names a lambda, read it with `javap --full --method 'lambda\$static'`.
4. Write down what you read in the packet's comment in `src/proto/packets.hpp` if it differs from older versions.

## What the codec names mean on the wire

| In the jar | Bytes |
|---|---|
| `ByteBufCodecs.BOOL / BYTE / SHORT / INT / LONG / FLOAT / DOUBLE`, `readInt`, `writeLong`… | fixed width, big-endian |
| `VAR_INT`, `readVarInt`, `idMapper(...)`, any enum's `STREAM_CODEC` built with `idMapper` | VarInt |
| `OPTIONAL_VAR_INT` | VarInt of `value + 1`; `0` = absent |
| `optional(codec)`, `readNullable/writeNullable`, `readOptional` | bool, then the value if true |
| `STRING_UTF8`, `stringUtf8(n)`, `readUtf(n)`, `Identifier.STREAM_CODEC`, `ResourceKey.streamCodec(...)` | VarInt byte length + UTF-8 |
| `BYTE_ARRAY`, `byteArray(max)` | VarInt length + bytes |
| `LONG_ARRAY`, `FriendlyByteBuf.writeBitSet` | VarInt count + big-endian longs |
| `ByteBufCodecs.BIT_SET` (light masks) | VarInt **byte** count + `BitSet.toByteArray()`: little-endian bytes, no trailing zero byte |
| `writeFixedSizeLongArray` | longs with **no** count (paletted container data) |
| `writeEnumSet(set, Enum.class)` / fixed bit set | `ceil(n/8)` bytes, bit i = ordinal i |
| `list()`, `collection(...)`, `map(...)` | VarInt count, then elements (key then value for maps) |
| `holderRegistry(key)` (`Item.STREAM_CODEC`, effects, potions) | VarInt registry id; one the registry does not have throws |
| `registry(key)` (`EntityType.STREAM_CODEC`, block entity types) | VarInt registry id through `Registry.byIdOrThrow` - which a `DefaultedRegistry` (`BuiltInRegistries`: `registerDefaulted…` - entity types default to the pig, blocks and items to air) answers with its default instead of throwing |
| `holder(key, codec)` | VarInt `id + 1`; `0` = an inline value follows |
| `UUIDUtil.STREAM_CODEC`, `readUUID` | two longs, most significant first |
| `BlockPos.STREAM_CODEC` | one long: x:26 \| z:26 \| y:12 |
| `Vec3.STREAM_CODEC` | three doubles |
| `Vec3.LP_STREAM_CODEC` (`LpVec3`: velocities) | 1 byte `0` for ~zero, else 6 bytes (3 x 15-bit fractions + 2-bit scale) + a VarInt when the scale needs more bits - `proto::put_lp_vec3` |
| `X.STREAM_CODEC.dispatch(type, codec)` (`PositionPath`) | the type's id (VarInt), then that type's own codec |
| `VecDelta.write` (entity moves) | no count of its own: the step count is in the packet's "properties" VarInt (`steps << 1 \| onGround`); 0 steps = three shorts, else per step VarInt ticks + three shorts |
| `ClientboundBundlePacket` | not a packet of its own: `bundle_delimiter`, the sub-packets, `bundle_delimiter` |
| `writeChunkPos` | one long: `(z << 32) \| (x & 0xFFFFFFFF)` |
| `ByteBufCodecs.TAG`, `COMPOUND_TAG` (`FriendlyByteBuf.readNbt/writeNbt`) | one network NBT tag: type byte, payload, no name (`NbtIo.readAnyTag`). A lone `TAG_End` is "no tag" and throws here. `TAG` weighs what it reads against 2 MiB (`NbtAccounter.defaultQuota`); the `TRUSTED_` ones do not - `nbt::Doc`, `nbt::Writer` |
| `fromCodecWithRegistries(codec)`, and a `DataComponentType` built with `persistent(codec)` only (`custom_data`, `lock`, `recipes`…) | a tag as above, then the `Codec` over `NbtOps`: a layout is the wrong question, see "Codecs over NBT" below |
| `ComponentSerialization.STREAM_CODEC` (text components) | the same: a tag read by `ComponentSerialization.CODEC` - `text::transcode` |
| `fixedSizeList(n)` (`SignText`) | n elements and **no** count |
| `Filterable.streamCodec(c)` (book pages and titles) | the value, a bool, and if true the value again (what a chat filter left of it) |
| `TypedEntityData.streamCodec(type)` (`entity_data`, `block_entity_data`) | the type's registry id (VarInt), then `COMPOUND_TAG` |
| `ByteBufCodecs.CONTAINER_ID` | VarInt |
| `idMapper(BY_ID, ...)` over `ByIdMap.continuous(..., ZERO / WRAP / CLAMP)` | VarInt; an id outside the enum does **not** throw - it becomes the first value / wraps / clamps (check which: it decides what vanilla writes back) |
| `ItemStack.OPTIONAL_STREAM_CODEC` (clientbound) | VarInt count; if > 0: VarInt item id, then `DataComponentPatch.STREAM_CODEC`: VarInt added, VarInt removed, added x (type id + value), removed x type id. No lengths: reading it needs every component's codec - `proto::put_item`, `mcproto.read_item` |
| `ItemStack.OPTIONAL_UNTRUSTED_STREAM_CODEC` (serverbound) | the same with a VarInt byte length in front of every value (`lengthPrefixed`; bytes the codec does not read are dropped). Under `ItemStack.validatedStreamCodec` the result is also encoded with the `Codec`, so the Codec's ranges apply - `item::read_untrusted` |
| `HashedStack.STREAM_CODEC` (in `container_click`) | bool present; item id, VarInt count, map type id -> int (CRC32C of the value's Codec form, at most 256), set of removed type ids |
| `ClientboundSetEquipmentPacket` | VarInt entity, then (byte slot ordinal, `0x80` set while more follow; stack) - no count |
| `lenientJson(n)` + `fromCodec(JsonOps, …)` | a JSON string (only status response and login disconnect) |
| `ByteBufCodecs.OPTIONAL_COMPOUND_TAG` (a block entity's tag in the chunk packet) | a tag, or one `TAG_End` byte for none - which is what an empty `getUpdateTag()` becomes (chests, furnaces) |
| `ComponentSerialization.TRUSTED_STREAM_CODEC` (the title in `open_screen`) | a text component's tag, as `STREAM_CODEC`, without the quota |
| `ParticleTypes.STREAM_CODEC` | VarInt particle type id, then that type's own options - nothing for the simple ones (explosion, poof, smoke) |
| `WeightedList.streamCodec(c)` | VarInt count, then per entry the value and a VarInt weight |
| `ClientboundBlockEventPacket` | position, two bytes, and the block as its **block registry** id (not a state id) |
| `ClientboundCommandsPacket` | VarInt count, then per node: a flags byte (bits 0-1: root / literal / argument; 4 runs; 8 redirects; 16 suggestions; 32 restricted), VarInt array of child indices, the redirect index if any, the name unless root, and for an argument its `command_argument_type` id with that type's properties (numbers: a byte 1 = min, 2 = max, then those; string: VarInt word / phrase / greedy; entity: a byte 1 = single, 2 = players only; resource: the registry's identifier; most: nothing) and the suggestions' identifier if flagged; last the root's index - `cmd::Registry::encode_tree` |
| `SoundEvent.STREAM_CODEC` (a `Holder`) | VarInt registry id + 1; or `0`, then the event inline: its identifier and an optional (bool-prefixed) float range - what `/playsound` sends for any name |
| `ClientboundStopSoundPacket` | a byte: bit 0 a source (VarInt enum) follows, bit 1 a sound (identifier) follows |
| `ClientboundPlayerChatPacket` | VarInt global index (the server's count per recipient, from 0), sender UUID, VarInt index, optional 256-byte signature (no length), `SignedMessageBody.Packed` (content of 256 chars, long ms, long salt, VarInt count of at most 20 packed signatures: VarInt id + 1, or 0 and the 256 bytes), optional component, `FilterMask` (VarInt type; 2 is followed by a `BIT_SET`), `ChatType.Bound` (holder: registry id + 1; the name; an optional target name) - `proto::cb::PlayerChatTail` |
| `ClientboundSetTimePacket` | long game time, then a map: VarInt count x (VarInt `world_clock` id, `ClockNetworkState`: **VarLong** total ticks, float partial tick, float rate - 0 for a clock that stands). No day time, no "tick day time" flag: 26.x has clocks - `proto::cb::SetTime` |
| `ClientboundCommandSuggestionsPacket` | VarInt id, VarInt start, VarInt length (chars, in the line as the client sent it), VarInt count x (string, optional tooltip component) |
| `ServerboundChatCommandSignedPacket` | command, long timestamp, long salt, up to 8 x (argument name of 16 chars, 256-byte signature), then `LastSeenMessages.Update`: VarInt offset, 20 bits as 3 bytes, a checksum byte |

All rows above were read from the 26.3 jar. If you meet a codec that is not in the table, read its
implementation (`mcgen.py find 'codec/ByteBufCodecs'`, then `javap --full` the anonymous class) and add a row.

## Codecs over NBT

Text components, `custom_data` and everything else that has a `Codec` but no `StreamCodec` travel as an NBT
tag that a `Codec` (`com.mojang.serialization`, DataFixerUpper) reads. Then the question is not "which bytes"
but "which tags does the codec take, and which one does it write back" - one value has many spellings, and
vanilla passes none of them on. DFU is not in the server jar that `mcgen.py` indexes; read it with `javap`
from `.cache/mc/<version>/datagen/libraries/com/mojang/datafixerupper/` (there after `mcgen.py reports`).
What was read there for 26.3 (DFU 10.0.21), and then seen to hold on a running vanilla server
(`tools/vanilla/component_probe.py`):

| In the jar | What it does with a tag |
|---|---|
| `Codec.STRING` | a `TAG_String` only |
| `Codec.BOOL` | any number tag; true unless `doubleValue() == 0` (so `256` and NaN are true). Written as a byte |
| `Codec.INT` and its ranges | any number tag, `Number.intValue()` (a float truncates, a long keeps its low half). Written as `TAG_Int` |
| `fieldOf(name)` | the entry must be there. Entries the record does not name are ignored |
| `optionalFieldOf(name)` | absent is fine; present and wrong is an **error** |
| `lenientOptionalFieldOf(name)` | present and wrong counts as absent |
| `optionalFieldOf(name, default)` | as above; **not written** when the value equals the default |
| `Codec.either(a, b)`, `withAlternative(a, b)` | `a` first, `b` only if `a` failed; written as whichever side the value came out of the `xmap` on |
| `listOf()` | a `TAG_List` or any array tag (`NbtOps.getList` takes every `CollectionTag`); one bad element fails the list. Written as a `TAG_List` always |
| `dispatch("type", ...)`, `dispatchMap` | the type entry picks the record, which then reads the same compound; a missing or unknown type is an error |
| `StringRepresentable.fromEnum`, `idResolverCodec` | the name as a string; an unknown one is an error |
| `validate(f)`, `comapFlatMap` | the check runs when reading (and, by the bytecode, when writing) |
| `RecordCodecBuilder` | writes its fields in the order of `group(...)` into a `CompoundTag` - a `HashMap`, so on the wire they come out bucket by bucket (`map_order` in `src/text/component.cpp`), not in that order |
| `ListTag` (any list on the wire) | elements of mixed types travel as compounds, the others wrapped as `{"": x}`; a reader takes the wrapper off (`addAndUnwrap`). An empty list is written with element type 0 |

`mcgen.py pseudo` shows the arguments of an anonymous class's constructor in the order the class *uses* the
captured variables, not in the order of the enclosing method's parameters (`ExtraCodecs.orCompressed` reads as
if its arguments were swapped). When the order matters, look at the `aload` order in `javap --full`.

`pseudo` also shows `x++` used as a value as an increment followed by a read (`this.nextChatIndex =
this.nextChatIndex + 1; send(new Packet(this.nextChatIndex, ...))` is `new Packet(this.nextChatIndex++, ...)`
in the source: the first `player_chat` carries 0, which a recording of the vanilla server showed and the
pseudo-code did not). Where a counter's first value matters, read the `dup_x1` in `javap --full` - or record it.

## Other things the jar answers

- **Packet ids**: registration order in `*Protocols` classes; `mcgen.py packets` extracts it. Never hardcode an id.
- **Order of packets the server sends**: read the server-side listener, e.g.
  `net.minecraft.server.network.ServerLoginPacketListenerImpl`, `ServerConfigurationPacketListenerImpl`,
  `net.minecraft.server.players.PlayerList` (`placeNewPlayer`, `sendLevelInfo`).
- **What the client requires**: `net.minecraft.client.multiplayer.ClientPacketListener` (`handle*` methods),
  `ClientConfigurationPacketListenerImpl`, `RegistryDataCollector`. This is the real spec for clientbound packets:
  a field the client ignores is free, a missing packet the client waits for is a hang.
- **Registry contents and tags**: inside the server jar under `data/minecraft/…`
  (`unzip -l .cache/mc/26.3/server-inner.jar | grep data/minecraft/tags/block/`).
- **Constants / enum ordinals**: `javap --full <Enum> --method 'static \{\}'` shows `ldc "NAME"` with the ordinal
  and constructor arguments after it.
- **What a block does, and in which order**: the hooks are methods of `net.minecraft.world.level.block.<Class>`
  (`mcgen.py data blocks <name> --field hooks` lists the ones its class chain overrides); what calls them is
  `Level.setBlock` / `LevelChunk.setBlockState`, `net.minecraft.world.level.redstone.CollectingNeighborUpdater`
  (and `NeighborUpdater.UPDATE_ORDER`, which is not `BlockBehaviour.UPDATE_SHAPE_ORDER`),
  `net.minecraft.world.ticks.LevelTicks` / `LevelChunkTicks` / `ScheduledTick` (three comparators: read all three),
  `ServerLevel.tickChunk` (random ticks) and `ServerChunkCache.tickChunks` (which chunks: 26.3 walks
  `DistanceManager.forEachEntityTickingChunk`, though the method is called `forEachBlockTickingChunk`).
  Read this version's code, not what is remembered of another's: in 26.3 `LevelChunkTicks.pack` sorts what it
  saves (`SUB_TICK_ORDERING`), the game rules are a registry (`gamerule minecraft:random_tick_speed`), and a
  falling block that lands sends `add_transient_block` beside its `block_update`.
- **The state a block is placed as**: `getStateForPlacement` of its class, over a `BlockPlaceContext` - read that
  class too. `getClickedPos()` is where the block *goes* (the clicked block only when that is replaced);
  `getClickLocation()` is the clicked block's position plus the packet's cursor floats, as doubles;
  `getNearestLookingDirections()` puts "into the clicked face" first unless the clicked block is replaced. And
  `canBeReplaced(state, context)` is asked twice: of the clicked block while `replacingClickedOnBlock()` still
  answers true (the constructor), then - `canPlace()` - of what stands where the block would go, with it false.
  That second question is how a slab is filled from the side.
- **Which chunk's changes a tick sends first is no rule**: `ServerChunkCache.chunkHoldersToBroadcast` is a
  `ReferenceOpenHashSet`, walked in identity-hash order. Within a chunk it is sections bottom up, one packet a
  section. A probe that compares the order of block changes must not compare it across chunks.
- **How a chunk gets its features** (26.3; none of it is 1.21's): statuses run `biomes, terrain, features,
  initialize_light, light, spawn, full` - there is no `carvers`. `ChunkStatusTasks.generateFeatures` builds a
  `WorldGenRegion` (centre and eight around at `terrain`, write radius 1: `ensureCanWrite` refuses the rest without a
  word; a read further off logs) and calls `ChunkGenerator.applyBiomeDecoration`: per step the indices of
  `FeatureSorter` in ascending order, `WorldgenRandom.setFeatureSeed(decorationSeed, index, step)`, one `FeaturePlacer`
  (an explicit stack: a modifier makes all its positions, with all their draws, before the first goes on). The four
  final heightmaps are primed at the end of `terrain` and moved by every `setBlock`; the two `_WG` ones stay as
  terrain left them; `OreFeature` writes through `BulkSectionAccess`, past all of that. A block entity is a
  `{id: "DUMMY"}` placeholder until something asks for it; a tick is kept with delay 0. The registry is
  `worldgen/feature` (not `configured_feature`), a block state is a name or `{id, properties}`. Only one chunk is
  decorated at a time, in an order of the server's; `--jfrProfile` records it (`minecraft.ChunkGeneration`, field
  `status`). What a feature is told of light is the light thread's business (docs/architecture.md, "Features").
  All of it can be run without a server: `tools/mcgen/extract/WorldgenFeatures.java`.
- **`Math.sin`, `cos`, `pow`, `log` are HotSpot's own**, not the C library's: the last bit differs in about one
  argument of five hundred. `Math.pow(x, 2.0)` is `x * x`. `Mth.sin` is a table (`phys/mth.hpp`), another thing.
- **Collision shapes** are `VoxelShape`s: coordinates per axis and a bit a cell. `mcgen.py blockdata` writes both the
  boxes and that grid (`collision_grids`); most joined shapes have exactly the lines their boxes' faces cut, 42 of
  330 have more (a piston's head, a straw bed). `getFaceShape(direction)` is the layer of cells at 1e-7 inside
  that face, stretched over the whole axis - what `WallBlock` holds its test shapes against.
- **A look is `BlockGetter.clip`**, not a line against collision boxes: `ClipContext.Block` says which shape a block
  has for it (`OUTLINE`: what the crosshair picks - a torch has one and no collision; `COLLIDER`; `FALLDAMAGE_RESETTING`)
  and `ClipContext.Fluid` whether a fluid stops it. The outline of a flower, of bamboo and of a dripstone is moved by
  where the block stands (`BlockState.getOffset`); `getInteractionShape` only changes which *face* was met. A use of
  an item on no block (`use_item`) aims with it: `Item.getPlayerPOVHitResult`, from the eyes of the pose the player
  has in that tick.
- **What a fluid state's shape is depends on what was asked first**: `FlowingFluid.getShape` keeps one shape a state
  in a map, built from the height at the first position it was asked about - "under more of the same" or not. A
  recording that crosses the top ninth of a cell of water sideways shows whichever came first on that server.
- **An entity in a fluid** is `EntityFluidInteraction` (26.x): trackers by fluid *type*, currents by fluid *tag*, the
  cells walked x, then y, then z. A mob's `MoveControl.tick` runs every tick it has AI and may add to its velocity
  with no goal asking (`SmoothSwimmingMoveControl` with gravity, `DrownedMoveControl`): read the control of a class
  before taking its `travel` for all there is - and a mob with a `FloatGoal` cannot be recorded in deep water.

## What only exists once the game has loaded: `mcgen.py data`

Shapes, which class overrides which hook, what a block object was constructed with, default attributes,
enum ordinals - none of that is in a class file as data, and all of it is one lookup away once
`mcgen.py gamedata` has been run (it executes the jar: ask the user first; the result stays in
`.cache/mc/<version>/gamedata/`, one JSON file per section, one record per line).

```sh
python3 tools/mcgen/mcgen.py data                               # what is there
python3 tools/mcgen/mcgen.py data blocks '^minecraft:oak_stairs$'   # one whole record (regex on the name)
python3 tools/mcgen/mcgen.py data blocks _door$ --field hooks --field fields    # some fields of many records
python3 tools/mcgen/mcgen.py data blocks --names | wc -l        # names only
python3 tools/mcgen/mcgen.py data states oak_stairs --field collision_shape --field sturdy_full   # states: ids, a-b, block names
python3 tools/mcgen/mcgen.py data entities '^minecraft:zombie$' 'entity\.monster\.zombie\.Zombie$'   # the type, and its class's synced data
python3 tools/mcgen/mcgen.py data registries '^attribute/' --field fields.defaultValue --field fields.maxValue
python3 tools/mcgen/mcgen.py data enums 'inventory\.ContainerInput$'
python3 tools/mcgen/mcgen.py data statics 'entity\.Avatar$'      # static tables and constants of a class (pose sizes)
```

| Section | One record is | Holds |
|---|---|---|
| `states` | a block state id (columns) | outline / collision / support / visual / interaction shape, sturdy faces per support type, redstone conductor, suffocating, signal source, push reaction, note block instrument, map colour, path type, heightmap predicates, fluid state, the state after each rotation and mirror, POI type, block entity type, destroy speed |
| `blocks` | a block | class chain, interfaces, `hooks` (which class overrides each `BlockBehaviour`/`Block` method), `properties` (order and stride: `state = first_state + sum(index * stride)`), `settings` (its `BlockBehaviour.Properties`), `fields` (what its own class holds: the fluid of a `LiquidBlock`, fire's burn odds), all five sounds, offset function samples |
| `fluids` | a fluid, a fluid state | amount, height, source, the block state that holds it |
| `items` | an item | class chain, `hooks` (overrides of `Item` methods), fields, crafting remainder. Default components are in `reports/item_components/` |
| `entities` | an entity type; an entity class | type: class chain, dimensions, attachments, category, default attributes, spawn placement. Class: its synchronized data - id, serializer, default (the default is read from bytecode, as an expression) |
| `registries` | an entry of any other static registry | id, class, every field: attributes (default/min/max), mob effects (attribute modifiers), potions, game rules, sound events (fixed range), POI types, block entity types (valid blocks), particles, stats, villager professions... |
| `enums` | an enum of the server jar | constants in ordinal order with serialized names and fields (ids) |
| `statics` | a class | static tables and constants of every block / item / entity / block entity / menu class and a list of others (`LevelEvent`, `EntityEvent`, `ServerGamePacketListenerImpl`, `MapColor`...). Extend `STATIC_EXTRA` in `tools/mcgen/extract/MiscSection.java` when a class is missing |
| `menus` | a menu type | slots in order (class, container, index in it, stack limit), where the player's slots start, data slot count, which slot classes override `mayPlace` and the like. Menus that need a player to be built say `unavailable` |

How to read it:

- Values are vanilla's: `hooks` and `fields` use the names in the jar, so `mcgen.py pseudo <class> --method <hook>`
  is the next step. A registry entry is written as its name, a block state as `{"state": id}`, a tag as `#name`.
- Everything in `states` was asked with nothing around the block (`EmptyBlockGetter`, no entity). Where the real
  answer depends on neighbours or on who asks, the block's class overrides that hook - check `hooks` before
  trusting a column for a block (`getCollisionShape` of scaffolding and powder snow, `getShape` of a shulker box).
- Shapes of blocks with an offset function (bamboo, pointed dripstone) are stored at offset zero; `offset_shapes`
  says which ones move, `blocks[].offset` has samples of the function.
- Data-driven things are not here because they are plain JSON in the server jar (`unzip -l … | grep data/minecraft/`):
  recipes, loot tables, enchantments, biomes, damage types, block transformers (strip / till / flatten), trades.
  In 26.x fuel, composting and brewing are data too (item components and recipes) - there is no table in code.
  What `tools/mcgen/gen_game.py` learned reading them for 26.3:
  - **Recipes** are a registry, looked up in path order (the first match wins). Kinds: `crafting_shaped`,
    `crafting_shapeless`, `crafting_transmute` (dyeing bundles and shulker boxes, cloning maps: the result
    keeps the input's components), `crafting_dye` / `crafting_imbue` / `crafting_decorated_pot` /
    `crafting_special_*` (results computed from components), `smelting` / `blasting` / `smoking` /
    `campfire_cooking`, `stonecutting`, `smithing_*`, and `brewing`. An ingredient is an item, a `#tag` or a
    list of items; a result may carry components (suspicious stew).
  - **Loot tables** spell conditions and functions as `condition` and `modifier` (one, or a list for
    `modifier`); a condition may be a string naming `data/minecraft/predicate/<path>.json`
    (`minecraft:tool/can_silk_touch`).
  - **Fuel** is the item component `cooking_fuel`: `burn_time` and `speed_multiplier` name providers in
    `data/minecraft/context_int_provider/` / `context_float_provider/` that ask whether the block is
    `#fast_cooking` (smoker, blast furnace).
  - **`update_recipes`** (play) carries nine "recipe property sets" - item lists a client's screens ask
    (`furnace_input`: what shift-click sends to a furnace's input) - and the stonecutter's recipes.
- The whole command tree is `reports/commands.json`.
- Generated C++ does not read these files yet. When a feature needs a table, teach `gen.py` to take it from
  here and to `die()` when a field it relies on is missing, as it does for `blockdata.json`.

## A saved world

What a vanilla server writes to disk is read by `Codec`s and by hand-written loaders, like everything else; what
was read for 26.3 when the import was written (`src/server/import.*`, `tools/vanilla/world_probe.py`):

| In the jar | On disk |
|---|---|
| `LevelStorageSource`, a world made by 26.x | `level.dat` (gzip, a named compound: `Data.DataVersion`, `Data.spawn {pos [I; x, y, z], dimension, yaw, pitch}`), `data/minecraft/*.dat` (`world_gen_settings.dat`: `data.dimensions["minecraft:overworld"].generator {type, settings}`; game rules, weather...), `players/data/<uuid>.dat`, and per dimension `dimensions/<namespace>/<path>/{region,entities,poi}/r.X.Z.mca`. Before 26.1 the overworld's `region/` was at the top |
| `RegionFile` | 4 KiB of locations (big-endian `sector << 8 \| sectors`, entry = x + z * 32), 4 KiB of timestamps, chunks in 4 KiB sectors: i32 length, u8 compression (`RegionFileVersion`: 1 gzip, 2 zlib, 3 none, 4 lz4, 127 custom; `\| 128` = the bytes are in `c.X.Z.mcc`), a *named* compound |
| `SerializableChunkData.parse` / `write` | `DataVersion`, `Status` (only `minecraft:full` is a finished chunk), `xPos`, `yPos`, `zPos`, `LastUpdate`, `InhabitedTime`, `isLightOn`, `sections [{Y, block_states, biomes, SkyLight, BlockLight}]` (also for the light sections below and above the world, which have light only), `block_entities`, `block_ticks`, `fluid_ticks`, `PostProcessing`, `structures`, `Heightmaps` |
| `BlockState.CODEC` (26.x) | `Codec.either(block name, FULL_CODEC)`: a plain string for a block's default state, else `{id, properties: {name: value}}` - it was `{Name, Properties}`. A property that is missing or has no such value takes the default state's. In a palette the two forms mix, so the list holds compounds and the names travel as `{"": name}` |
| `PalettedContainer.codec`, `Strategy` | `palette` + `data` (longs; absent for one entry). Bits on disk: blocks `max(4, ceillog2(n))`, biomes `ceillog2(n)`; entries never span two longs. The palette is in order of first appearance (`pack` re-encodes), which is also the order a freshly loaded chunk goes out to a client in |
| `DataLayer`, `SerializableChunkData.copyOf` | `SkyLight` / `BlockLight`: 2048 bytes, nibble index `y << 8 \| z << 4 \| x`; a layer whose `isEmpty()` (no array and default 0) is not written. `read` queues what is there (`queueSectionData`); `ChunkStatusTasks.isLighted` = status at least `light` and `isLightOn` |
| `BlockEntity.saveWithFullMetadata`, `ContainerHelper`, `ItemStack.CODEC` | `{id, x, y, z, components, keepPacked, ...}`; a container's `Items: [{Slot, id, count (1 when absent), components}]`; a furnace's `lit_time_remaining`, `lit_total_time`, `cooking_time_spent`, `cooking_total_time`, `speed_multiplier`, `RecipesUsed`; `CustomName`, `lock`, `LootTable` / `LootTableSeed` |
| `EntityStorage` | `entities/r.X.Z.mca`, the same file format: `{DataVersion, Position [I; x, z], Entities [...]}` |

The vanilla server takes `--world <name>` (and `--universe <dir>`): another world without touching
`server.properties` - how `world_probe.py` keeps its fixture apart from the other probes' world.

## Limits

- Static block/item/entity ids are assigned at runtime: they come only from `mcgen.py reports`, which executes the
  jar — ask the user first (see AGENTS.md).
- To observe vanilla's runtime behaviour (packet order, timings), the vanilla server may be run from
  `.cache/vanilla-server/` under the rules in AGENTS.md ("The vanilla server as a reference"). Never touch
  `eula.txt`. `tools/vanilla/entity_probe.py` records what one bot is told about another on any server
  and diffs two recordings - the bytecode says what a packet looks like, this says when vanilla sends it
  (it is how the server-side simulation of players, saved `OnGround`, and the lazily created attributes
  were found). Use fresh player names per run: vanilla saves players, and a returning one starts differently.
- `javap` output is bytecode, not source: for long methods, narrow with `--method` and read the `invoke*` lines in
  order; branch targets (`ifeq 42`) tell you what is conditional. For logic (as opposed to layouts)
  `mcgen.py pseudo <class> --method '<regex>'` is far easier to read: real local names, `if ... goto` flow.
- `tools/vanilla/inventory_probe.py` is the same idea as `entity_probe.py` for inventories: creative slots,
  equipment, drops, pickups and every kind of click. It is how `ServerPlayer.take` (the menu is synchronised
  right after a pickup) and the two-ticks merge of moving items were found.
- `tools/vanilla/component_probe.py` asks a server what it makes of an item's text and NBT components: some two
  hundred spellings, each put into a hand and read back from what a second bot is shown. It is how the codec
  table above was checked, and the first thing to extend when a component or a part of the text codec is ported.
- Read the subclass too. A method found on `LivingEntity` or `Player` may be overridden in `ServerPlayer`
  (`take`, `drop`): `mcgen.py pseudo net.minecraft.server.level.ServerPlayer --method ' name\('`.
- `tools/vanilla/swing_probe.py`: a player's hands - punches, hits, placing, breaking, drops, a spear - and
  which of them the others are shown as an arm swing. It is how the double swing of a placed block was seen
  (`handleUseItemOn` holds the branch twice: the bytecode said so, the recording confirmed two packets).
- `tools/vanilla/container_probe.py`: chests, crafting, a furnace, TNT - menus compared byte for byte,
  primed TNT by its height tick by tick. It is how `Entity.requiresPrecisePosition` was found (a TNT on the
  ground is sent as exact positions every tick), that `CraftingMenu` sends its result slot once per
  ingredient taken, and that a menu shows its emptied slots before it closes on a broken chest. Two
  recordings on one vanilla world do not compare: the first one's crater is in the second.
- `tools/vanilla/command_probe.py`: what a server answers to commands - some 120 lines, right and wrong, typed by
  an operator bot while a second one watches; system messages byte for byte, block changes, teleports, sounds, the
  command tree. It is how the order of `/help`, the cursor of an unknown entity type (behind the name, where every
  other parser puts it back in front), the empty string `MutableComponent.append` drops, and the second, absolute
  `player_position` a server sends when a confirmed teleport does not match were found. The bot is opped from the
  console after it joined: asked to op a name it has not seen, vanilla looks it up at Mojang.
- `tools/vanilla/physics_probe.py`: what vanilla *computes*, not what it sends - positions and velocities of
  entities as exact doubles, tick by tick (`record`: `/tick freeze`, `/tick step 1`, `/data get entity`, whose
  decimals are the shortest that name the double). It starts the server itself. What it taught: a step is only
  armed by `/tick step` and runs later, so a recording counts ticks by an entity of its own (a TNT with a long
  fuse) instead of trusting the order of commands; the console prints what a command answers as `System chat: ...`;
  NBT cannot show the sign of a zero (`DoubleTag.valueOf` makes `-0.0` the shared zero); `/summon` hands entity
  ids out in order, and an entity's id decides which tick an item at rest tries the ground; an entity that leaves
  the chunks that tick simply stops, in the recording as anywhere; fixtures 8 blocks apart are near enough for a
  thrown item to land on its neighbour's (replay the recording in one world, as it was made). `perf` times
  vanilla's ticks with `/tick sprint` - a 1000-tick sprint with ten thousand entities is silent for two minutes.
- **Commands** are Brigadier's (`com.mojang.brigadier`, not in the server jar: read it with `javap -p -c -cp
  .cache/mc/<version>/datagen/libraries/com/mojang/brigadier/*/brigadier-*.jar com.mojang.brigadier.CommandDispatcher`).
  A command's tree is the builder chain in its class (`mcgen.py pseudo net.minecraft.server.commands.FillCommand`
  prints it in one line; `then()` order is child order - `reports/commands.json` sorts children by name, the wire
  does not). An error's text is a translation key and its arguments in the order of the *lambda's array*, not of
  the `create(...)` call (`argument.block.property.invalid` is created with block, property, value and reads block,
  value, property): print the `lambda$static$N` bodies. Where the cursor is when a parser throws is in its code
  (`reader.setCursor(start)` or not) and shows as the `<--[HERE]` line.
- A packet you remember may be gone, with another in its place: 26.x has no serverbound `swing`, the left
  button is `punch`. Before concluding that the client sends nothing, list what it can send
  (`src/proto/gen/*/packet_ids.hpp`, or the `handle*` methods of `ServerGamePacketListenerImpl`) and read what
  the client does on that key (`mcgen.py pseudo net.minecraft.client.Minecraft --client --method 'startAttack'`).
