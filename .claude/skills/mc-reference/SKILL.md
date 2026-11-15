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
- The whole command tree is `reports/commands.json`.
- Generated C++ does not read these files yet. When a feature needs a table, teach `gen.py` to take it from
  here and to `die()` when a field it relies on is missing, as it does for `blockdata.json`.

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
- A packet you remember may be gone, with another in its place: 26.x has no serverbound `swing`, the left
  button is `punch`. Before concluding that the client sends nothing, list what it can send
  (`src/proto/gen/*/packet_ids.hpp`, or the `handle*` methods of `ServerGamePacketListenerImpl`) and read what
  the client does on that key (`mcgen.py pseudo net.minecraft.client.Minecraft --client --method 'startAttack'`).
