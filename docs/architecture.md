# Architecture

What exists today is the network stack, a world that can be built in - kept on disk, loaded and
unloaded around the players, or imported from vanilla - the entity layer that lets players see
each other, items - stacks, the player's inventory, what is held and worn, what lies on the
ground - the first blocks that do something: a chest, a furnace, a crafting table, TNT - and
what makes blocks act on each other and by themselves: neighbour updates, scheduled ticks,
random ticks. This document explains how the pieces fit and why; `AGENTS.md` has the rules in
short form, `docs/blocks-roadmap.md` the blocks that are still to come.

## Threads and the data between them

```
                 ┌──────────────────────── reactor thread "net-N" (one io_uring) ─────────────────────┐
  TCP ──recv──▶  │ decrypt ─▶ split frames ─▶ inflate ─┬─▶ pre-play: Prelogin (handshake…config)      │
                 │                                     └─▶ play: copy into SpscBytes `inbound` ───────┼──┐
  TCP ◀─send───  │ send queue per connection ◀── outbox (SpscRing<OutMsg>) ◀──────────────────────────┼─┐│
                 │ sent tick-owned buffers ──▶ recycled (SpscRing<ByteBuf*>) ─────────────────────────┼┐││
                 └────────────────────────────────────────────────────────────────────────────────────┘│││
                 ┌──────────────────────── tick thread "tick" (20 Hz) ────────────────────────────────┐│││
                 │ 1. drain every reactor: recycled buffers → pool; Join / Leave / Packet records ◀───┼┘│◀┘
                 │ 2. game logic writes packets into one ByteBuf per player                           │ │
                 │ 3. flush: encrypt all dirty streams in one batch, post Send per player, wake ──────┼─┘
                 └────────────────────────────────────────────────────────────────────────────────────┘
                   auth workers (2 threads, libcurl): session-server HTTPS, result → Reactor::run_on_loop
                   workers "work-N": files, deflate, the generator - a task out, its result back,
                   each through an SPSC ring of that worker's (see "The world's life cycle")
```

There is no shared mutable state on any hot path. Each arrow between threads is a single-producer /
single-consumer ring with cached indices (`base/spsc.hpp`), so an uncontended push or pop touches no
cache line owned by the other side. Producers batch: the reactor publishes the inbound ring once per
completion batch, the tick thread wakes each reactor at most once per tick (an `eventfd`, coalesced by
`wake_pending_`).

Why the reactor does not run game logic, and the tick thread does not touch sockets: both would need
locks around player state or around the send path. With this split a packet crosses a thread boundary
exactly once in each direction, as a `memcpy` into a ring.

### Reactor (src/net/reactor.*)

- Ring flags `SINGLE_ISSUER | DEFER_TASKRUN | SUBMIT_ALL`: completions are processed only when the
  thread enters `io_uring_submit_and_wait_timeout`, so there are no interrupts or task-work storms. The
  ring is created inside `run()` because `SINGLE_ISSUER` binds it to the creating thread.
- **Accept**: one multishot accept per reactor; every reactor has its own `SO_REUSEPORT` listener.
- **Recv**: one multishot recv per connection with a provided-buffer ring (2048 × 2 KiB). There is no
  recv syscall and no per-connection read buffer; a frame is parsed straight out of the kernel-filled
  buffer, and only a frame that straddles two buffers is copied into `Conn::in`.
- **Send**: one in-flight `send` (`MSG_WAITALL`) per connection, the rest queued as an intrusive list of
  `ByteBuf`. Short sends are resubmitted. `TCP_NODELAY` is on because the tick flush is already one
  write per player.
- **Close**: `shutdown(SHUT_RDWR)` forces the armed recv and any in-flight send to complete; the fd is
  closed only after both completions arrived (so no stale completion can hit a reused slot — and the
  24-bit generation in `user_data` guards anyway). A kick is `close_after_flush`: send what is queued,
  `shutdown(SHUT_WR)`, wait for the peer's FIN or 5 s. A full close with unread input would become a
  RST and could discard the disconnect message before the client reads it.
- **Limits**: frame size by phase (1 KiB handshake/status, 64 KiB login, 2 MiB − 1 after), inflated size
  2 MiB, a deadline to reach the play state (`login-timeout`, default 30 s), and a full inbound ring
  drops the offending connection instead of blocking the loop.

### Connection lifecycle

```
accept ─▶ Handshake ─▶ Status ─▶ close
                  └──▶ Login ─▶ [encryption + session server] ─▶ [compression] ─▶ Config ─▶ Play
slot:   allocated on accept ─────────────────────────────────────────────────── promote_to_play ──▶
        freed by the reactor on close (pre-play)          Leave record ─▶ tick removes player ─▶ Release ─▶ freed
```

Before `promote_to_play`, `Prelogin` (reactor thread) owns the protocol and writes to `Conn::out`. The
promotion emits a Join record `[InHeader][JoinTransport{cipher, threshold}][profile blob]`; from then on
the reactor only forwards. A connection closed while in play becomes a *zombie* slot until the tick
thread acknowledges with `Release`, so a `(slot, gen)` the tick thread still holds can never point at a
new connection.

### Tick thread (src/server/game.*)

One tick, in vanilla's order (`MinecraftServer.tickChildren`):

```
chunks in ─▶ drain packets ─▶ block ticks ──▶ block + light changes ─▶ chunks out ─▶ entity tracker ─▶ block events ─▶ entity ticks ─▶ block entities ─▶ per-player tick ─▶ send chunks ─▶ flush
(what the    (move, click…)   (the clock;     (ChunkHolder             (saved,        (ChunkMap.tick)   (a chest's      (items, TNT,     (furnaces)       (ack, equipment,     (PlayerChunk
 workers                       scheduled,      .broadcastChanges)       unloaded)                        lid)            falling blocks;                   head, pickup, pose)  Sender)
 finished)                     then random)                                                                              menus)
```

The order is observable, so it is copied: a block change reaches a client before the entity
movement of the same tick; the tracker runs before a player's own tick, which is why other players
see a head turn one tick after the body and a new pose one tick after the key that caused it; a
menu is brought up to date (`ServerPlayer.tick`) before the player's connection ticks, so a slot
changed by a pickup is sent with the pickup and one changed by a key (F) a tick later; block
entities tick after the entities, so what a furnace cooked in a tick reaches its menu in the next;
an explosion happens in the tick of its TNT, and the blocks it took go out with the next tick's
block changes - after the `explode` packet, as in vanilla. The entities themselves tick in the
order they were made (`Game::tick_entities`, one list for items, TNT and the living, as
`EntityTickList`): an explosion throws the item that ticks after it in the same tick and the
one before it in the next. What is made while the list is walked ticks from the next tick on.

A player is an entity: position, rotation and everything other players are shown live in the
entity table (below), where players are the first rows - a player's index here is its entity
index. What only a connection has stays in `Game`, split by access pattern: view centre, pending
ack and output buffer are parallel arrays at that index; everything else is a cold `Player` struct.
Removal swaps with the last element so bulk passes stay dense. `slot_to_player_[reactor][slot]`
maps inbound records to the index.

`Game::flush` is where outbound cost concentrates: all streams with output are encrypted by
`cfb8_encrypt_batch`, then each buffer is posted to its reactor and comes back through `recycled` after
the send completes. Buffers never cross threads any other way, so both pools are lock-free by
construction.

## Entities (src/entity/, src/server/players.cpp)

The heaviest path of a server like this is telling N players about M moving things, every tick.
Three decisions carry it: how entities are stored, how "who sees whom" is found, and how movement
is encoded once for everyone who sees it.

### One table for everything (`entity::Store`)

Players, mobs, items: one table of parallel arrays indexed by a dense entity index. Not "players
here, mobs there" - the passes that dominate a tick treat an entity as a point that moves and must
be shown to whoever is near; one table means one tracker pass, one id space, one handle type, one
set of columns for plugins. What differs between kinds lives *beside* the table:

- **Players are the rows `[0, players)`** - the player prefix. Viewers are therefore contiguous
  (the visibility scan over them is a straight SIMD loop), and the game's per-connection columns
  share the index. Creating a player moves the first non-player row to the end; removing one
  fills the hole with the last player, then that hole with the last row.
- **What only some kinds have** (AI state, an inventory, a path) belongs in side tables keyed by
  slot and iterated on their own, not in columns every row pays for.

A dense index is only valid until the next create/destroy. Everything that outlives that uses the
**slot**: a stable index, reused once its entity is gone, with a generation. `Handle` packs both
into 53 bits (it survives being a Lua number); the **network entity id is slot + 1**, so ids stay
one or two VarInt bytes for the whole uptime - they are in every movement packet - and "which
entity is id N" is an array lookup. Reuse is safe because a despawn tells its viewers at once, in
order, before the slot can be handed out again.

Columns are 64-byte aligned, their capacity a multiple of 64 rows with zeros past the end, so SIMD
loops read whole blocks. `entity::Columns` is the same data as a plain C struct: the view the
LuaJIT FFI will get.

### Who sees whom, and telling them (`entity::Tracker`)

A port of vanilla's `ChunkMap.TrackedEntity` + `ServerEntity` (26.3 - which differs from 1.21.x in
ways no one documents: movement deltas are "stepped paths" with a tick count, `move_entity_rot` has
its fields in another order, pairing is sent as a bundle, visibility is re-evaluated on every
movement packet). State: one bit per (player, entity) pair - a row of words per player, indexed by
dense entity index - and 64 bytes per entity of what its viewers were last told (`Sent`). One
pass per tick, in three steps:

**A. Visibility.** Vanilla looks at a pair again on two events: the entity changed section, or the
player sent a movement packet. So only those are looked at. A player that moved has its whole row
redone: the players by a SIMD scan over the position columns, 64 answers per word, XORed with the
row; everything else by re-testing what it sees plus what stands in the cells of `entity::Grid`
that its range touches. Players that stood still only test the entities that moved. The test is
vanilla's - horizontal distance against the smaller of the type's range and the player's view
distance, inclusive, computed without a fused multiply-add so that a pair on the boundary falls on
the same side as in Java.

Chunk-sized or section-sized cells are the wrong shape for this: a player's range is 160 blocks, so
a query would touch 21 x 21 chunk cells to find a few dozen entities. The grid's cells are 64 x 64
blocks (6 x 6 per query), holding only non-players; measured at 500 moving players + 20 000 mobs:

| "what could this player see" | tracker tick |
|---|---|
| one SIMD scan over all entities (11 us per player) | 6.5 ms |
| grid, 32-block cells | 2.4 ms |
| grid, **64-block cells** | **2.0 ms** |
| grid, 128-block cells | 2.1 ms |

With players only the grid is never asked: 500 players are 8 words.

**B. Encode once.** `ServerEntity.sendChanges` runs for every entity, once, into an arena: the
packets of entity e are the bytes `[off[e], off[e+1])`, in dense order. All viewers of an entity
are told the same thing - a delta against the position *they all* believe (the base), which is
also the position a newcomer is given in `add_entity`, so the next delta fits everyone.

**C. Fan out.** For each player, each *run* of consecutive set bits `[a, b)` in its row is one
`memcpy` of `arena[off[a], off[b])`. A player in a crowd sees everyone: two runs, two copies,
however many entities.

The same once-only rule holds for pairing: the bundle that introduces an entity is encoded the
first time someone needs it in a tick and copied after that (a player joining a crowd appears to
everyone at once).

### What a tick of this costs

`bench tracker` (i5-9300H, `release`), every player walking and turning every tick:

| scenario | pairs | tracker | bytes per tick | + AES-CFB8 |
|---|---|---|---|---|
| 500 players, all within sight of each other | 249 500 | 0.2 - 0.3 ms | 2.5 MB | 8 - 9 ms |
| 500 players spread over 5 x 5 km | 800 | 0.13 ms | 8 KB | 0.16 ms |
| the same + 20 000 mobs, a third of them walking | 19 000 | 2.0 ms | 81 KB | 2.4 ms |

The crowd is the case the design is judged by: a quarter of a million pairs for a quarter of a
millisecond, because steps B and C are two copies per player. What is left is not ours to shrink: in online mode
every outbound byte is one AES block. **Bytes are the unit of cost**, and two choices here come
from that:

- Tiny packets get a one-byte frame length instead of the padded three (`frame_begin`): a
  `move_entity_pos` is 12 bytes instead of 14. In the crowd: 2.99 -> 2.50 MB per tick, 10.5 -> 8.6 ms.
- Entity ids are slots, not a counter that grows for the whole uptime.

`tests/integration/run.py -k crowd_500 --slow` is the same case end to end: 500 `mcproto` bots
over real sockets, three of them checking that what they decode is where everyone is. Same
machine, bots included:

| | tick, average | worst | of it: tracker | of it: flush | server CPU |
|---|---|---|---|---|---|
| offline mode | 1.0 ms | 3.2 - 3.5 ms | 0.6 ms | 0.07 ms | 11 % of a core |
| online mode (AES) | 17 - 26 ms | 34 - 69 ms | 0.4 - 0.5 ms | 17 - 25 ms | 50 - 60 % of a core |

(Three runs of each. The tracker's 0.6 ms against the benchmark's 0.3 is the output buffers: here
they come back from the reactor cold, there they are the same 500 every tick.)

The second row is the finding of this exercise. A crowd of 500 is 50 MB/s of ciphertext, CFB8
makes that one AES block per byte, and all of it happens on the tick thread: 8 ms when the core
is alone at full clock (the benchmark above), three times that with the bots competing for the
same four cores and their one AES port each. The entity system is 2 % of that tick. Encryption
has to leave the tick thread before a crowd like this is safe in online mode - the streams are
independent, so it parallelises cleanly: either the reactors encrypt what they send (it scales
with `net-threads` and needs the cipher state to stay where the socket is), or the flush is split
across workers.

### Where this differs from vanilla, on purpose

- A pair is evaluated once per tick, with the positions everyone ended up at, instead of at the
  moment each movement packet is handled. Same result; a pair that flickered inside one tick is
  not announced twice.
- (Until chunks were paced, a newcomer was shown everything around it at the end of its join
  tick. Now it is vanilla's rule: an entity is shown once the chunk it stands in has gone out -
  `ChunkMap.isChunkTracked` - and the pair is looked at again when the player next says where it
  is, which a client does every tick and at the least once a second.)

### What is not there yet

Read off a diff against a vanilla 26.3 server (`tools/vanilla/entity_probe.py`: two bots, one
watching the other walk, turn, sneak, sprint, jump, leave and return). Walking, turning, the head
lagging a tick, sneaking, sprinting flags, options, range, leaving: the same bytes. What differs:

- `update_attributes` (a player's own creative reach comes from there too), the locator bar
  (`waypoint`): not sent. Nor are footsteps (`sound`, to those who hear a player walk).
- Vanilla 26.x simulates a player's movement on the server as well as on the client. From that it
  knows a player stands on the ground before the client says so, and it infers jumps, which
  viewers get as `set_entity_motion`. We take the on-ground flag from the client's packets and
  never give a player a velocity - visible in the stream as one `entity_position_sync` arriving
  with the client's first packet instead of right after the join, and no motion packet on a jump.
- Movement checks: vanilla refuses a 12-block step, we pass it on.

For mobs the table and the tracker are ready (velocity, update intervals, plain paths for
non-living things are ported and tested); nothing spawns them, and their type-specific data
(health, variants) has no storage yet.

### Mobs and AI (a sketch, not built)

- A mob is a row after the players. Movement, visibility and the wire are done by the passes
  above; a mob costs the tracker 64 bytes and, while it stands still, a few nanoseconds per tick.
- Behaviour state goes into side tables, one per kind of brain, dense and iterated in bulk:
  `struct Wander { u32 slot; float tx, tz; u16 cooldown; }` in a vector, and a system that walks
  the vector and writes `x/z/yaw` through `slot -> row`. A mob with no brain table entry is inert.
- Level of detail by distance: vanilla only ticks entities near players. Keep the awake mobs as
  a prefix of the mob rows (the same trick as the player prefix), so "tick the awake ones" is a
  range and the sleeping ones cost nothing.
- Senses and collisions need small-radius queries; those want chunk-sized buckets (and the chunk
  already exists as an object), separate from the tracker's coarse grid.
- Pathfinding is the part that goes to workers: a request and a result through SPSC rings, the
  world read from a snapshot.
- Everything a client can observe about a mob comes from the jar, per type - which is why none
  can be spawned until its behaviour is ported.

## Items (src/item/, src/server/inventory.*, inventory_actions.cpp, item_entities.cpp)

### A stack is eight bytes; its components are somebody else's (`item::ItemStack`, `item::Patches`)

An item stack is an item id, a count and - rarely - a *component patch*: what this stack has that
its item does not have by default (damage, enchantments, a potion's contents). Vanilla keeps a map
of decoded objects per stack. Here every distinct patch is stored once, in the bytes
`DataComponentPatch.STREAM_CODEC` writes for it, and a stack holds a 32-bit id to it:

- a stack is 8 bytes whatever it carries; an inventory is a small dense array, and nearly every
  stack has id 0, for which copying and destroying are one test;
- "same item, same components" - the question behind every merge, in a slot or on the ground -
  is one integer compare, because equal patches are one patch (entries sorted by type, values
  re-encoded, then interned by hash);
- sending a stack to a client is a copy of bytes that are already in wire form.

Patches are reference-counted by the stacks that hold them (copy = retain, destroy = release; the
store checks at exit, in debug builds, that nothing is left). The few components the server itself
acts on - stack size, damage, enchanted or not, where it is worn - are read out of the patch when
asked, falling back to the item's generated defaults (`ItemInfo`).

### Nothing a client sends is stored unread

A patch only ever comes from a client (`set_creative_mode_slot`), and what a player holds is shown
to everyone around: as equipment, as a dropped item. One value another client's decoder throws on
disconnects everybody in sight. Vanilla is safe because it decodes every component into an object
and re-encodes it; so does this server, component by component, with decoders written from the
vanilla codecs - the `StreamCodec` for the layout, the `Codec` for the ranges that
`ItemStack.validatedStreamCodec` enforces - and what is stored is the re-encoding, never the
client's bytes (`item/components.cpp`). Three outcomes:

| | what it means | what happens |
|---|---|---|
| ok | every component was read | the stack is taken |
| malformed | vanilla's decoder throws on it | the sender is disconnected, as by vanilla |
| unsupported | well-formed envelope, but a component without a decoder here | the stack is refused and the slot sent back as the server holds it |

"Unsupported" is the same rule as for blocks that cannot be placed yet: the server never holds
what it cannot vouch for. Read today:

- the plain ones: stack size, damage, max damage, unbreakable, repair cost, rarity, enchantments
  and stored enchantments, glint override, potion contents and duration scale, suspicious stew
  effects, fireworks and firework explosions, instruments, painting variants, banner patterns
  (all three by registry reference only), block state, tooltip display, dyed colour, map id, item
  model, tooltip style, note block sound, ominous bottle amplifier, glider, waxed, the dye and
  collar colours;
- text: custom name, item name, lore, the pages and title of books, the lines of a sign - each
  a text component, read and written by `text::transcode` (below);
- tags: custom data, bucket entity data, entity data and block entity data - compounds, kept as
  vanilla would write the compound it loaded but with their entries in name order
  (`nbt::Doc::write`). What is *in* them is nobody's business on the wire: a client's decoder
  takes any well-formed compound, and the server will read them only where it acts on them.

That covers everything the creative inventory offers, the ominous banner included. Not read:
stacks within stacks (containers, bundles, charged projectiles), the behaviour components (food,
tool, equippable, attribute modifiers...), the components that have a `Codec` only and so travel
as the tag it makes (map decorations, lock, recipes... - custom data is the one of them that is
read), and in a text the parts listed under "Text components".

Knowing differences from vanilla:

- a patch is not "sanitized" against the item's defaults (a modified client can make a stone with
  `max_stack_size=64` that will not merge with plain stone - a vanilla client never sends that);
- a patch is capped at 1 KiB (vanilla's only limit is the packet; a stack is copied to every
  viewer of its holder). Names, lore and custom data fit; **a book of more than a page or two
  does not** and is refused like an unread component - the cap was set before there was text,
  and is one constant (`kMaxPatchBytes`) to revisit together with a limit on how often a player's
  equipment may be re-sent. A tag of more than sixteen times the cap is refused without being
  read: it is the tick thread that reads;
- "equal" is "equal bytes". Vanilla holds a few things equal that it writes differently - a
  colour by name and the same colour by number, two NaNs - and such stacks merge there, not here;
- strings that are not well-formed UTF-8 (or, inside a tag, not what Java itself writes) are
  refused. Vanilla reads them with replacement characters and passes on what it made of them.

### The inventory and its menu (`server::Inventory`, `server::Menu`)

A port of `Inventory`, `InventoryMenu` and `AbstractContainerMenu.doClick` - every kind of click:
pick up, shift, number keys, clone, throw, drag, double click - as plain data and functions with
no game state, so that it runs in a unit test. What a click does to the world (an item thrown
out, the sound of armour going on, a `set_player_inventory`) comes back as a list of effects that
`Game` acts on in order.

The client predicts its own clicks, so the server keeps what the client *is known to hold* per
slot (`RemoteSlot`) and sends only differences. A click reports the client's prediction as item,
count and one hash per component; this server does not compute those hashes yet (they are CRC32C
over the component's `Codec` form, a second encoding of every component), so a slot predicted to
hold a stack with components is always sent again. Correct, and a few bytes more than vanilla.

Vanilla compares all 46 slots of every player every tick. Here a player whose inventory nothing
touched costs one byte test per tick (`Game::dirty_`); one whose inventory was touched costs
0.2 us for the comparison of its 46 slots and cursor (`bench menu`).

Everyone is in creative mode for now, which decides what reaches this code: the creative
inventory screen keeps its own copy and reports slots (`set_creative_mode_slot`), and
`container_click` arrives from the screens a block opens - a chest, a furnace, a crafting table
(below: "Blocks that do something"). The 2x2 grid of the inventory crafts like the table's 3x3.

### Held, worn, dropped

- **Equipment** is entity state the tracker shows: `Tracker::show_equipment` is
  `LivingEntity.detectEquipmentUpdates` - it keeps what viewers were told last, sends the slots
  that changed (`set_equipment`; a plain swap of the hands is entity event 55), and adds what an
  entity holds to the bundle a newcomer gets. Game calls it for players flagged as touched.
- **The arm.** A client swings its own arm and tells the server only why: `punch` (26.x: the
  left button, whatever it hit; it took the place of `swing` and carries nothing), a block placed
  with `use_item_on`, a drop. The server passes that on - `swing_animation` to everyone who sees
  the player, with the hand and the animation: the attack animation of what the main hand holds
  for a punch (`ItemInfo::attack_swing`: a whack of 6 ticks, a stab of 13 to 23 for the spears),
  the default for the rest - and decides whether it is a swing at all. `SwingState.startIfAble`
  refuses one that comes in the first half of the swing before; that state is two fields of the
  player here, the tick the swing began in and its length. `Game::swing` is the one place.
- **An item on the ground** is a row of the entity table whose stack is the `item` column (shown
  as synchronized data), plus three timers kept beside the table by slot: created, aged from,
  pick-up from. Items of one kind within half a block merge after 40 ticks, are picked up by a
  player whose box (grown by 1 / 0.5) touches them, and expire after 6000 ticks - `ItemEntity`'s
  rules. "Which items can this player reach" is asked by every player every tick, so items have
  a grid of their own with cells the size of the question (`BasicGrid<1>`: 2 blocks; the
  tracker's cells are 64). With no items in the world the pass is skipped.
- **A dropped item is thrown**, as `LivingEntity.createItemStackToDrop` throws it: from 0.3
  below the eyes, three tenths of a block a tick the way the player looks (the sines are
  `Mth.sin`'s table, the sum a float), a little up, a little of anything. What a chest spills
  gets `Containers.dropItemStack`'s toss, what a block drops the hop of a new `ItemEntity`. From
  there it is physics (see "Physics"): a client simulates items as well, so the two have to
  agree tick by tick.

### What it costs

`bench item`, `bench menu`, `bench pickup` (i5-9300H, `release`):

| | |
|---|---|
| read a plain stack from a client | 15 - 18 ns |
| read a stack with two components (decode, re-encode, intern) | 0.3 - 0.4 us |
| write a stack with components | 6 - 7 ns |
| menu sync of a touched player, nothing to send | 0.2 us |
| 500 players spread out looking for items among 2000 | 8 - 10 us per tick |
| the same in one place (everything within 40 blocks) | 0.17 - 0.18 ms per tick |

The last row is the one that needed a decision - with the tracker's 64-block cells it was 1.7 ms,
eight times the tracker's own tick for that crowd:

| cell size | 2 blocks | 4 | 8 | 64 |
|---|---|---|---|---|
| crowd, per tick | **0.17 - 0.18 ms** | 0.25 - 0.27 ms | 0.47 - 0.53 ms | 1.7 - 2.0 ms |

The tracker itself is where it was (0.21 ms for the crowd, 2.0 ms with 20 000 mobs): the item
column and the equipment index add nothing to its loops.

### Compared with vanilla

`tools/vanilla/inventory_probe.py` runs one script against a vanilla 26.3 server and against this
one - creative slots, equipment, hand swaps, drops, pickups, every kind of click, closing the
menu - and diffs what the two bots were sent. The clicks, slot packets, equipment, sounds and
pickups come out the same, byte for byte (the vectors in `tests/unit/wire_test.cpp` are from that
recording). What differs:

- vanilla shows a new entity to its viewers the moment it is created, we at the end of the tick
  (the tracker's one deliberate difference): a swing and the item it threw swap places in the stream;
- which of two items that lie together goes into the other when both are as large, and which
  neighbour an item with several takes first, follows the order of our grid's cells, not of
  vanilla's entity sections.

`tools/vanilla/swing_probe.py` does the same for a player's hands: punches in one tick and two,
five and twelve ticks apart, a hit on the other bot, blocks placed with either hand and clicks
that place nothing, blocks broken, drops around a punch, a spear. Which swings the watcher is
shown, with which hand and animation, comes out the same. What differs:

- vanilla sends the swing of a placed block twice (`handleUseItemOn` holds the same branch
  twice). The second begins the same swing again within the tick, which a client cannot tell
  from one: sent once here;
- a hit does nothing here but swing the arm: `attack` is not read, so there is none of
  vanilla's damage, knockback or sounds (`entity.player.attack.nodamage` between two creative
  players);
- a spear's stab (`player_action` STAB: a hit along the look, the spear's sound, the arm) is not
  handled - a vanilla client never punches with a spear, so nobody sees it attack;
- others do not hear a block being placed or see it break (`sound`, level event 2001).

`tools/vanilla/component_probe.py` does it for what a stack carries as text and as tags - the
names, lore, books, signs and custom data a client may spell in a hundred ways. Its results are
under "A text component is read by a codec" below.

## Blocks that do something (src/server/block_entities.cpp, block_entity.*, explosion.cpp, src/item/recipes.*, src/world/collide.*)

Until here a block was a state: stored, lit, sent. Four blocks are more now - a chest, a
furnace, a crafting table, TNT - and each is the first of a family. `docs/blocks-roadmap.md`
lists every other block by the class that gives it its behaviour, in the order the missing
systems are meant to be built; it is generated, so it cannot say "done" about a block the
server would refuse to place.

The rule for all of them stays the one from the first block: the world never holds a block
whose reaction to being clicked, updated or blown up would have to be invented. `mcgen` decides
what can be placed: a block class that is ported is entered in `PORTED_BLOCKS`
(`tools/mcgen/gen_game.py`) together with every hook it overrides in the jar - a version that
adds a hook to `ChestBlock` stops the generator until someone has read it.

### A block entity is in three places

- **Where it is, and of which type** belongs to the world: 8 bytes in an array of its chunk
  (`world::BlockEntityRef`). That is all a chunk packet says about a chest - and it has to say
  it: a client draws a chest only if the packet lists a block entity there. Finding one by
  position is a scan of its chunk's array; chunks hold a handful.
- **What it holds** belongs to the game: `server::Chest` (27 stacks, who has it open) and
  `server::Furnace` (three stacks, four timers) are plain structs in two arrays that the
  chunk's entry points into. Entries are reused but never moved - a menu names its chest by
  index - and furnaces, the one kind that ticks, are walked in a row: one that neither burns
  nor could start costs the test at the top of the loop.
- **Who is looking** belongs to the player: the menu (below).

`Game::set_block` is the one door every block change of the game goes through, and the port of
what `LevelChunk.setBlockState` and `Level.setBlock` do around a change: the old block's block
entity goes and spills what it held, the new block's comes, the block is told it was placed
(TNT beside a block of redstone goes off), and the six neighbours hear of it (a chest joins or
leaves its other half, TNT looks for a signal) - in the order and by the rules of "Block
updates", below.

### One player, two menus (`server::Menu`)

A player always has its own menu (container id 0) and the menu that is open - which is its own
unless it clicked a chest, a furnace or a crafting table. `Menu` holds both: the inventory they
share, and for each its cursor, its crafting grid and what the client is known to hold.
`AbstractContainerMenu.doClick` is written once; what differs between `InventoryMenu`,
`ChestMenu`, `FurnaceMenu` and `CraftingMenu` is a table from slot number to "which stack, which
kind of slot", `quickMoveStack`, and three hooks that vanilla hides in `Container` and `Slot`
subclasses:

- `Slot.set` on a crafting grid recomputes the result **and sends it at once**
  (`CraftingMenu.slotChangedCraftingGrid`), on a furnace's input restarts the cooking
  (`AbstractFurnaceBlockEntity.setItem`);
- `Slot.remove` on a result slot takes all of it, whatever was asked;
- `Slot.onTake` on a result slot uses the ingredients up, one `removeItem` at a time - each of
  which is a grid change, so crafting sticks from two planks sends the result slot twice.
  That is vanilla's stream, and the probe below compares it byte for byte.

A menu on a chest or a furnace is compared with what its client knows every tick while it is
open (somebody else may have a hand in the same chest; a furnace burns), and closes when its
block is gone or more than four blocks past reach. Two players in one chest need nothing more
than that: both menus point at the same 27 stacks.

A chest counts who has it open (`ContainerOpenersCounter`): the first opener is heard, every
change moves the lid (a block event to everyone within 64 blocks), and while it is open the
chest counts again every five ticks - which is how a player who comes into view later sees the
lid up. That recount is vanilla's scheduled block tick (`ChestBlock.tick`), and runs as one:
see "Block updates". A tick's block events are a set in vanilla (`ServerLevel.blockEvents`), so
a lid that is closed in the very tick of its recount is told to fall once; `Game::block_event`
drops the second likewise.

### Recipes are tables (`src/item/recipes.*`)

`mcgen` reduces `data/minecraft/recipe` to what matching needs: an ingredient is a sorted list
of item ids (a tag is expanded), a recipe a few bytes. 1235 crafting recipes - shaped,
shapeless, transmute - and the 73 of the furnace; the 31 recipes that compute their result from
components (dyed armour, fireworks, banner copies...) are not there, and a grid that holds one
makes nothing.

The first recipe in id order that matches wins, as in `RecipeManager`. Trying 1235 in a row on
every click would be the simple way; instead the recipes that name an item at all are listed
per item, in order, and only the shortest of the grid's lists is walked - any recipe that
matches is in every one of them. Smelting is one table load per item: a furnace asks for its
recipe every tick it burns. Fuel is an item component in 26.x (`cooking_fuel`, a provider that
asks whether the block cooks fast); `mcgen` evaluates it for the plain furnace into
`ItemInfo::fuel_ticks`.

### TNT (`src/server/explosion.cpp`)

A primed TNT is a row of the entity table that `Game::tick_tnt` ticks the way `PrimedTnt.tick`
does: gravity, the move, drag, the hop off the ground (`phys::tnt_tick`, see "Physics"), then
the fuse, and `requiresPrecisePosition`, which makes the tracker send exact positions for a TNT
that touches a block. A client simulates primed TNT too, so the two have to agree tick by tick;
the probe below compares the height of the hop.

The explosion is `ServerExplosion`: 1352 rays that the blocks wear down, the entities within
twice the radius by how much of them the centre can see (`world::clip_hits`, the collision
shapes again), the blocks. What a block drops is its loot table - which is why `mcgen` reads
loot tables: for every block that can be in the world it reduces the table to what an explosion
without a tool leaves (stone: cobblestone; glass: nothing; glowstone: 2 to 4 dust), and stops
on a table it cannot reduce. TNT in the blast goes off on a short fuse instead of dropping;
players get their push in the `explode` packet, which is where a client takes it from; items,
primed TNT and mobs get theirs as velocity (`Entity.push`), an item its damage first (five
points of health), and an armor stand in reach breaks.

### What it costs

`bench craft`, `bench smelting`, `bench furnace`, `bench collide`, `bench clip`, `bench blast`
(i5-9300H, `release`):

| | |
|---|---|
| what a grid makes: sticks / a chest (shaped) | 0.14 / 0.17 us |
| ...flint and steel (shapeless) | 0.06 us |
| ...nine planks, which make nothing: the longest walk there is | 0.7 us |
| a furnace's recipe for an item | 2.6 ns |
| 10 000 furnaces that neither burn nor could start | 13 us per tick |
| 10 000 furnaces, all burning | 0.15 ms per tick |
| one line of sight of an explosion, 7 blocks long | 0.15 us |
| the 1352 rays of one explosion | 0.18 ms |

The rays are what an explosion costs: 25 000 points, each a step of a ray. Asking the world
for the block at every one of them took 0.23 ms; each block's resistance is now looked up once
per explosion and kept in a cube around the centre. On the running server one TNT then takes
0.4 to 1.8 ms of its tick (`--debug=true` logs each one) - the rays on a core that was asleep between
ticks, plus 0.05 to 0.15 ms for the hundred-odd blocks it changes; the light of the crater is
the next tick's block changes. A hundred TNT in one tick is a tick lost: explosions are the
first thing here that a player can make arbitrarily expensive, and vanilla has no limit either.

The menus cost what the player's own did: a touched menu with nothing to send is compared in
0.2 us whichever menu it is (`bench menu`), and only players with a chest, a furnace or a
crafting table open are looked at every tick. The entity table's new column (a TNT's fuse)
and the tracker's new flag left `bench tracker` where it was: 0.3 ms for the crowd, 2.0 ms
with 20 000 mobs.

### Compared with vanilla

`tools/vanilla/container_probe.py` runs one script against a vanilla 26.3 server and against
this one: a chest placed, opened by two players at once, filled, made double, broken; the
inventory's grid and a crafting table; a furnace fed and broken; a TNT lit beside another. It
diffs what the two bots are sent. Every menu packet - `open_screen` with its title, contents,
slot and cursor updates with their state ids, the furnace's four numbers, `container_close` -
comes out the same byte for byte, in the same order; so do the block states (which way a chest
faces, which half it is), the lid's block events and their five-tick rhythm, the chest sounds
and where they come from, the items a broken chest spills, the level event that shows the other
player a chest, a table or a furnace breaking. Of a primed TNT: the fuse counting
down in its entity data every tick, the height of its hop tick by tick, the switch to exact
positions when it lands, the `explode` packet (its tail of particles and sound byte for byte,
the push it gives a player four blocks away within a few percent - it depends on which blocks
the rays took), the second TNT going off from where it fell, dirt lying about in stacks of 16.
One thing that looks like a bug and is not: a chest that becomes half of a double one while a
player has it open keeps that player's screen as it is - 27 slots onto its own half, the lid of
the pair up - until it is closed; opened again it is the double chest. Vanilla does exactly
this (the probe's steps "A opens the single chest again" to "A puts it back and closes", and
again with the second chest placed sneaking against the first one's side), and so it stays.
Only the half that is open gets lid events, on both servers; a vanilla client raises both lids
all the same, because its renderer takes the wider of the two (`ChestBlock.opennessCombiner`).
A client whose mod draws chests itself may not: Better Block Entities (part of Fabulously
Optimized) shows one lid up and one down there - on a vanilla server as on this one. What
differs:

- vanilla shows a new entity the moment it is created, so `add_entity` of a primed TNT comes
  before its sound and before the `explode` packet of the TNT that lit it; here it comes with
  the tick's other entity updates (the tracker's one deliberate difference, as for items);
- vanilla swings the arm twice for a click on a block (`handleUseItemOn`), we once - as for a
  placed block;
- `update_recipes` at join carries the same nine property sets (which items a furnace takes...);
  vanilla also lists the stonecutter's recipes there, which no block here can use yet;
- not sent at all, each a known gap rather than a difference in what is sent: the sound of a
  block being placed, a player's footsteps. (A block breaking is seen and heard by the others
  now: level event 2001.)
- a furnace gives no experience when it is emptied, ores blown up by a player's TNT none
  either: there are no experience orbs;
- an explosion hurts nothing but items (nothing else has health): a mob is thrown and lives,
  an armor stand breaks with its sound but without its particles;
- a player's inventory is not kept when it leaves, so what it had on a menu's cursor or in a
  crafting grid is not dropped at its feet.

## Block updates (src/server/block_updates.cpp, falling.cpp, src/world/ticks.*, dimension.*)

A block that is set is the beginning of something. The blocks around it hear of it, and some
change: a torch whose wall is gone breaks; sand with nothing under it asks to be looked at two
ticks from now; a lamp that lost its signal goes out four ticks later; and the dirt beside
grass is grass too, whenever chance has it. Those are the three mechanisms every block with a
behaviour is made of - neighbour updates, scheduled ticks, random ticks - and with them came
the first blocks that need them: torches, the redstone lamp, amethyst, grass and mycelium that
spread and die, nylium, copper that weathers, and what falls - sand, gravel, concrete powder.

One thing holds for all three: **the order is the behaviour**. Which of two blocks hears of a
change first, which of two ticks that are due in the same game tick runs first - whatever
players build out of blocks that react to each other is built on the answer. So nothing here
is designed. It is vanilla's, read from the jar and kept in its order; what is ours is how it
is stored.

### A change, and who hears of it (`Game::set_block`)

`Game::set_block(pos, state, flags, limit)` is `Level.setBlock` with `LevelChunk.setBlockState`
inside it, step for step:

1. the state is stored (`world::Dimension::set_block`: heightmaps, light, the change for the
   clients, the counts of "Random ticks" below);
2. the block entity of the block that went goes, and spills what it held;
3. the new block is told it was placed (`onPlace`: TNT beside a signal goes off, sand asks for
   a tick), and gets its block entity;
4. if the block is still the one that was set - `onPlace` may have made it another - the six
   around it hear that a block changed beside them (`neighborChanged`: a lamp looks for its
   signal), and then each of the six takes the shape it has beside the new block
   (`updateShape`: a chest joins its other half, grass under snow is snowy, a torch looks for
   what holds it). A shape that is no shape at all is the end of the block:
   `Block.updateOrDestroy` breaks it, with the particles and the sound of a breaking block
   (level event 2001, to everyone within 64 blocks) and what its loot table drops.

`flags` are vanilla's bits and mean what they mean there: 1 tells the neighbours, 2 the clients,
16 says the shape is known and nobody need be asked, 32 that what breaks drops nothing, 256
that a container goes without spilling, 512 that `onPlace` is skipped. A player's hand passes
1 | 2 (vanilla's 11 has an 8 in it, which is a client's business), `/setblock` what
`SetBlockCommand` passes.

None of step 4 runs where it is made. `CollectingNeighborUpdater` is a stack: the first update
of a chain runs at once, and what an update makes while it runs is put on top of what was
waiting, the first of it uppermost - depth first, in the order made. Here an update is a
record of 40 bytes on a `std::vector` that keeps its capacity (`Game::NeighborUpdate`,
`add_update`, `run_updates`, `run_next`), of vanilla's four kinds: one block's shape, one
block told, one block told with the state it had, and "the six around this one", which hands
out its six one at a time so that what the first sets off is done before the second is told.
The two orders are not the same, and both are kept: neighbours are told west, east, down, up,
north, south (`NeighborUpdater.UPDATE_ORDER`), shapes are asked west, east, north, south, down,
up (`BlockBehaviour.UPDATE_SHAPE_ORDER`). A chain of shape updates ends 512 deep (`limit`,
vanilla's `updateLimit`), and all of a chain after `max-chained-neighbor-updates` (a setting,
1 000 000 as in vanilla) is dropped with vanilla's line in the log.

What a block does when it is told is a `switch` over its kind in five functions
(`neighbor_changed`, `update_shape`, `on_place`, `tick_block`, `random_tick`; `can_survive` for
what has to stand on something) - one per hook of `BlockBehaviour`, each case the method of the
class it names. Whether a state has a hook at all is one byte per state, generated
(`state::kHooks`, from the hooks its class chain overrides in the jar): of the twelve updates a
change makes, those that reach stone - nearly all - are a load and a branch. A block of a class
that is not ported has no bits there: an imported world's lever is told of a change and does
nothing, as it does nothing else.

### Scheduled ticks (`world::LevelTicks`, src/world/ticks.*)

"Tick the block at this position, so many game ticks from now." A port of `LevelTicks`,
`LevelChunkTicks` and `ScheduledTick`, and what it has to get right is again an order:

- a chunk holds its own ticks, by game time due, then priority, then the order they were
  scheduled in (a counter of the level, `subTickOrder`);
- one tick per position and block at a time: a second is dropped (`ticksPerPosition`) - which
  is what makes "schedule a tick" something a block may do on every update it hears of;
- each game tick the due ticks of the chunks that tick are merged, chunk against chunk, by
  priority and order of scheduling of what each has next. Ticks that are overdue - their chunk
  did not tick for a while - come out of that in an order no single sort gives, so
  `LevelTicks::collect` is `drainContainers` step for step, and the unit test holds it to a
  model of vanilla's queues written from the jar;
- at most 65 536 a game tick; a tick that is scheduled while the collected ones run waits for
  the next game tick, however soon it is due;
- a tick is run only if the block at its position is still the one that asked
  (`ServerLevel.tickBlock`).

What is ours is the storage. Vanilla keeps a `PriorityQueue` of objects and a hash set of
objects per chunk, and walks a hash map of every chunk that holds a tick once a game tick.
Here a tick is 24 bytes in a flat binary heap, with priority and order packed into one 64-bit
key so that vanilla's two comparators are `<` on integers; the one-per-position set is a flat
open-addressing table of 32-bit keys (block, y, z, x); and the chunks that hold ticks are two
parallel arrays - when each is next due, which chunk it is - so that the pass over them reads
eight bytes a chunk and touches a chunk only when something in it is due. A chunk without
ticks has none of this: its `ChunkTicks` is made with its first tick.

Ticks belong to their chunk and are saved with it (`world/chunk_io.*`, version 2 of a chunk's
bytes: a fourth part after block entities and entities; a chunk written before there were
ticks is still read) - as vanilla saves them: the game ticks left until each is due, in the
order they were scheduled in. A chunk that comes back counts them again from the moment it
first ticks blocks (`LevelChunk.unpackTicks`), before everything scheduled since. So a lamp
that was about to go out when its chunk was unloaded goes out when somebody returns, and sand
that hung over a hole falls. The game time itself (`Game::time_`, `Level.getGameTime`) is not
saved yet: it starts at zero with the server, which is all a delay needs - a tick that was long
overdue when its chunk was saved is due at a time before that zero (times are signed, as
vanilla's are) and runs at once.

The chest's lid, which had a queue of its own for `ChestBlock.tick`'s recount, is a scheduled
tick now like every other.

### Random ticks, and the sections that have none to run

`ServerLevel.tickChunk`: every game tick, in every section of every simulated chunk that holds
a block which ticks at random, `random-tick-speed` cells (3) are picked - by vanilla's
generator, `randValue * 3 + 1013904223` and its bits - and a picked cell whose block is one of
those gets its `randomTick`. Ported so far: grass and mycelium spread to dirt within reach
(one to each side, three down, one up) where there is light and room above, and turn to dirt
under what shuts the light out; nylium turns to netherrack; copper weathers, slower among
copper that is behind it; budding amethyst grows buds on its sides.

On a server of 500 players that is some hundreds of thousands of sections asked three times a
tick, and nearly every answer is "nothing": the cell is stone, or it is grass that is neither
smothered nor has any dirt to take. A world of grass ticks all day and nothing ever happens.
So the question is answered ahead of time, per section, by counting:

- `Section::ticking` - cells whose state ticks at random at all;
- `RandomCounts::always` - of those, cells that may act whatever is around them (copper,
  budding amethyst);
- `RandomCounts::dying` - grass, mycelium or nylium under a block that kills it;
- `RandomCounts::targets` - dirt with room above: what a spreader could take.

Each is kept by `Dimension::set_block` as the cell and the cell above or below it change (two
table lookups per changed block: `state::kRandom`, a byte per state that says what a state is
to a random tick and what it is to the block under it - generated from the jar's occlusion
shapes, light dampening and fluids, so that "does this kill grass" is not computed at
runtime), and counted in full when a chunk is loaded, on the worker. Per chunk the counts fold
into bit masks over its sections, and

    active = ticking & (always | dying | near)

where `near` is the targets of the chunk and of the eight around it, widened by a section up
and down - a spreader reaches one block sideways and from three below to one above, never
further than the sections next to its own. A section that is not active is **dormant**: no
random tick in it can change anything, so it is not ticked. Only chunks that tick, are
simulated and have an active section are in the list the tick walks
(`Dimension::random_chunks`), and it walks their active sections alone. A flat world of grass
has an empty list; a hole dug into it wakes the sections around the hole until the dirt is
grass, and they go dormant again.

This is a shortcut, and it is held to the long way like the others: in a build with DCHECKs
(`dev`) dormant sections are ticked all the same, and a random tick in one that changes a block
stops the server (`kCheckDormantSections`, `Game::expect_no_change_`) - the suite's
`random_ticks` scenario and `block_probe.py ours build/dev/elaine` run that way, at 4096 ticks
a section. The unit test counts every section of a chunk by brute force after each of some
thousand changes and compares.

Two things differ from vanilla, neither visible: the generator's values go to other cells (it
is seeded from the clock there too), and a section that a tick wakes is ticked from the next
game tick on, where vanilla - which walks its chunks in the order of a hash map - would get to
it in the same tick if its chunk happened to come later.

### Who is simulated

A player's simulation ticket is at the chunk it is in (`DistanceManager.addPlayer`). Chunks
within `simulation-distance` of a player - a square - have random ticks; those one chunk
further still run their scheduled ticks (vanilla's entity-ticking and block-ticking levels:
`ServerChunkCache.tickChunks` walks the first, `LevelTicks` asks for the second). A tick in a
chunk beyond that waits, saved and loaded with its chunk if need be, until a player comes.
`Chunk::sim_refs` and `block_refs` count the players that cover a chunk, and are changed when
a player crosses into another chunk, joins or leaves, and when a chunk is adopted
(`Game::move_simulation`, `simulate_adopted`) - nothing is worked out per tick. Entities do not
follow it yet: they tick wherever their chunk ticks (see "Physics"), where vanilla's stop a
chunk short of the scheduled ticks - `Chunk::sim_refs` is what its `inEntityTickingRange` would
read here.

### What falls (`server/falling.cpp`)

Sand, red sand, gravel and the sixteen concrete powders (`FallingBlock`, `ColoredFallingBlock`,
`ConcretePowderBlock`) ask, when they are placed or hear of a change, for a tick two game ticks
later, and if what is under them then gives way - air, fire, a liquid, anything replaceable
(`state::kFallThrough`) - the block becomes an entity in its place: a `FallingBlockEntity`,
which is a row of the entity table like an item or a primed TNT (`entity::Store::block_state`,
`start_pos`), moved by `phys::falling_move` and held to vanilla's bits by a recording of its
own (16 scenarios of `physics_probe.py`, `Physics.ReplaysTheFallingBlocksAVanillaServerRecorded`).
Where it comes down it is a block again if the block there gives way and it could stand; on a
torch or a slab it breaks and lies there as an item. Concrete powder that lands in water, or
beside it, is concrete, and so is powder that water is poured beside - or would be: nothing
here places water yet, an imported world has it.

A client moves a falling block by itself and takes it away when told; the block it becomes is
sent twice, as in vanilla - at once to those who see the entity (`block_update`, and
`add_transient_block`: the client draws that block by itself for up to a second, a
`TransientBlock`, whatever its chunk shows meanwhile), and with the chunk's changes to everyone. A falling block is saved with its chunk (`server/persist.cpp`,
entity records version 2), and nothing can be built where one is.

### Compared with vanilla

`tools/vanilla/block_probe.py` runs both servers through the same 55 steps - two bots, one
building, one watching from three blocks off, and the console - and compares what each bot is
told: every block change, level event, falling block, primed TNT and dropped item, in the
order it came and by the tick it came in. It starts the servers itself; vanilla gets a new flat world every time.
In the last recordings nothing differs, in any step, for either bot. What that covers:

- **torches**: which form a torch takes by the face that is clicked and where its player looks
  (standing; on a wall; under a block, looking level and looking down), a torch on a torch
  refused; five torches of three kinds around one stone breaking in the order west, east,
  north, south, up when the stone goes, each with its level event and its item;
- **the lamp**: lit in the tick its signal comes, out four game ticks after it went, and not
  out if the signal is back by then - at once, or two ticks later;
- **amethyst**: a cluster and a bud go with the stone they are on; the cluster leaves two
  shards, the bud nothing;
- **commands**: `/setblock ... destroy` and `/fill ... destroy` break like a hand does and take
  the torch on top along; sand set into the air falls; a torch set into the air stays, and
  goes when the block under it is replaced by air; a lamp set beside a block of redstone stays
  dark - the command tells the neighbours, not the block - until something beside it changes;
- **what falls**: the tick the entity appears in and what it says of itself (where it began,
  twice), the tick it lands in - seven after the block under it went for one block's fall, ten
  for two - the block, the transient block, the entity's removal, the block once more a tick
  later; gravel that lands on a torch lying there as an item; two blocks of sand falling as
  two entities and standing as two blocks again; and of a fall of forty blocks what its
  viewers are told on the way: a step every twentieth tick, the velocity with it, a whole
  position once the step no longer fits a packet's sixteen bits;
- **random ticks** (4096 a section, to have them soon): a hole dug into grass is grass at its
  bottom; grass under a stone is dirt; dirt put on grass is grass, and the grass under it
  dirt; copper goes all the way to oxidized; budding amethyst has a full cluster on each of
  five sides, each facing away from it; nylium under a stone is netherrack; and where nothing
  was done nothing happens;
- **TNT** on five sides of a block of redstone is primed west, east, up, north, south.

What a recording cannot say is said by the unit tests: the tick queue's order against a model
of vanilla's queues, the falling block's path against a recording of vanilla's doubles.

Differences that remain, by decision:

- an entity that a tick makes - a falling block, a primed TNT - is shown by vanilla the moment
  it is made and here with the tick's other entities: after that tick's block changes instead
  of before them, in the same flush;
- level events and sounds leave vanilla when they happen - between two ticks if a packet was
  the cause - and this server at the end of the tick, with everything else;
- the entities of a tick are told about in the order they were made; vanilla walks a hash map
  of them;
- which cells the random ticks fall on (see above);
- entities - falling blocks too - tick wherever their chunk ticks, not within the simulation
  distance (see "Physics").

### What it costs

Not measured: no benchmark was written for any of this, by request, so no number is claimed
here. What the layouts are meant to avoid is said where they are described - a hook that is
absent costs a table byte, a chunk with no tick due costs eight bytes of a pass, a section
with nothing to do costs nothing - and `bench` cases for a chain of neighbour updates, the
tick queue under load and the random pass belong with the first claim that any of it is fast.

### What is not there

- **Fluids.** No fluid ticks (vanilla's second `LevelTicks`, run after the blocks'): a
  waterlogged block that would ask for one on an update does not. Ice does not melt - it
  would leave water - and lava sets nothing alight (a fluid's random tick). Concrete powder
  hardens beside water, which only an imported world has, and that water does not flow.
- **The time of day and the weather.** `raw_brightness` reads the sky's light as at noon: grass
  spreads by night as by day, where vanilla's would wait for a torch. Nothing rains or snows
  on a block (`tickPrecipitation`), no lightning.
- **Game rules.** `random-tick-speed` is a setting of the server; blocks and entities always
  drop.
- **Comparators** (`updateNeighbourForOutputSignal`) and redstone wire's shape updates at a
  distance (`updateIndirectNeighbourShapes`): neither block exists. A signal is still "a block
  of redstone beside me".
- **The scheduled ticks of an imported world.** A vanilla chunk's `block_ticks` are kept with
  the rest of its tags, unread: sand that vanilla had saved in the middle of its two ticks
  waits for the next change beside it.
- **The game time across a restart**: it starts at zero. Delays do not mind; the day will.
- Falling: anvils and pointed dripstone, which hurt what they land on; a falling block's own
  tag from `/summon` (it is sand); concrete powder that falls faster than a block a tick is
  looked for water along its way only when it falls straight down.
- The sound of a block being placed.

## Physics (src/phys/, src/world/collide.*, src/server/physics.cpp)

What moves by itself - an item, a primed TNT, a living thing nobody steers - moves as on a
vanilla server, and "as" is meant to the last bit of every coordinate. Not for its own sake: a
client simulates items and TNT too and is corrected by what the server says, a farm is built
around where things land, and a position that is one bit off is a different block two hundred
ticks later.

### A port, and a recording to hold it to

`src/phys` and `src/world/collide.*` are vanilla's code read from the jar and written down
again, method for method:

- `Entity.move`: the cobweb's multiplier, `Entity.collide`, the rule for when the position is
  taken (`movementLength > 1e-7 || delta^2 - movement^2 < 1e-7` - a move that is stopped within a
  hair of nothing is dropped whole), which sides it hit (`Mth.equal`: 1e-5), the ground and the
  block stood on (`checkSupportingBlock`, with its look back at where the entity came from),
  the fall, `restituteMovementAfterCollisions` (a slime block, `#suppresses_bounce`, four fifths
  for what is not alive), `getBlockSpeedFactor`;
- `Entity.collide` with the step up (`collectCandidateStepUpHeights`), over `BlockCollisions` in
  its cursor's order - the box grown by 1e-7 and one more cell, the cells on its faces only for
  shapes that reach out of their block, a whole block met by `AABB.intersects` and any other
  shape by `Shapes.joinIsNotEmpty` with its index mergers - and `Shapes.collide` /
  `VoxelShape.collideX` over a shape's grid of cells; `Shapes.create`, which turns an entity's
  box into a grid of eighths when its corners lie on them;
- `ItemEntity.tick` (it moves only every fourth tick once it lies), `PrimedTnt.tick`,
  `LivingEntity.aiStep` and `travelInAir` with no input, `Entity.push` and `pushEntities`;
- `applyEffectsFromBlocks`: the blocks a box passes through
  (`BlockGetter.forEachBlockIntersectedBetween`), and what three of them do - a cobweb, the side
  of a honey block, the top of a slime block.

Reading is not proof, so there is a recording. `tools/vanilla/physics_probe.py record` starts
the vanilla server, freezes it (`/tick freeze`), builds a small fixture per scenario, summons
entities with exact positions and velocities, steps the game a tick at a time and reads every
entity back with `/data get`: positions, velocities, the fall distance, on-ground, printed as
the shortest decimals that name those doubles. 341 scenarios, 18 741 states: drops and throws
onto every kind of ground, walls, seams between blocks, ties between two blocks as near, tiny
velocities around each threshold, some 36 collision shapes stood on and run into, slime, honey,
cobwebs, soul sand, ice, steps up, blocks that change under an entity, pigs pushing each
other, the void - and "courses" of random obstacles. `tests/unit/physics_test.cpp` builds the
same blocks at the same coordinates, runs the same ticks and compares bits. All of them match.
(What a recording cannot show is the sign of a zero - NBT stores `-0.0` as `0.0` - and what
happens inside a tick.)

The tables it needs come from the jar too (`tools/mcgen/gen_physics.py`): a byte per block
state (what kind of collision shape, whether it reaches out of its block, which of 12
"materials" - friction, speed and jump factor, bounce, what it does to an entity), and 210
collision shapes as grids of cells (183 of them made of several boxes). The generator stops rather than guess: at a placeable
block whose shape depends on who asks, at a block hook that acts on entities and is not
ported, at a living entity class that declares a movement method the port was not written
against, at a new answer to `getMoveSimulationType`.

### Shortcuts are tested against the long way

Vanilla's way is general: every query walks every cell of a grown box and builds a shape for
each. `world::collide` keeps a literal port of that (`world::slow`) and a fast one beside it,
and a test throws 300 000 random boxes at a cluttered patch of world through both. The fast one

- asks the sections first (`Dimension::holds`: a mask per chunk of which sections hold
  anything, hold a shape that reaches out of its cell, a block that acts on entities, one that
  breaks a fall). Nothing but air around: nothing is read. No tall shape around: the cells
  beside the box are not read - at most 8 cells instead of 56 for an item. An entity's tick asks once,
  for all of its queries;
- compares a shape of one box with the entity's box in closed form (`cells_meet`: the index
  mergers of `joinIsNotEmpty` for two lists of two, written out) and sweeps against it without
  the grid (`collide_cell`).

### At rest

Nearly everything in a world lies still. In vanilla that is not free: an item at rest checks
every tick whether it is inside a block and tries the ground every fourth, a mob that stands
runs `travel` every tick. But what lies still does the same thing every tick, exactly:

- a living entity at rest is a fixed point - a tick leaves every bit of it as it was;
- an item at rest goes round in four ticks: its fall grows by 0.04 three times, the fourth tick
  the ground stops it, and what is left of its sideways velocity (under 1e-7, which moves
  nothing) shrinks by the ground's friction.

So an entity found at rest is not ticked in full (`Game::sleep`): a living one is passed by -
the tick list carries a bit for it, nothing is looked up - and an item gets
`phys::item_rest_tick`, which is its velocity and nothing else. Whether an item is at rest is
not reasoned out from blocks: `phys::item_rests` takes its next four ticks on a copy and
compares each with what `item_rest_tick` would have done. For a living entity it is a
comparison of before and after.

Whatever a tick at rest would have read wakes the entity when it changes: a block set within
reach (`Game::set_block` -> `wake_near`; `phys::block_in_reach` is the cells such a tick reads,
and their neighbours), a push, a teleport, a living thing put where it stands, a player in its
box. Then it is ticked in full again from the state it has - which is the state the full ticks
would have left, so nothing can be seen of the difference. Two tests hold that: the vanilla
recording replayed with rest, and 260 things thrown into a patch of world that changes every
third tick, each ticked both ways in lockstep for 900 ticks and compared to the bit.

One thing of vanilla's shows here: an item at rest loses 0.12 of velocity in the tick the
ground stops it, which is "more than a tenth" to `ItemEntity.tick`, so it sets `needsSync`
every fourth tick for as long as it lies - and three in four lying items (by where in that
round their 20-tick update falls) are sent as a velocity twice a second. That is kept; a
client would not notice it missing, but it is what a vanilla server sends
(`physics_probe.py wire`: of four items put down a tick apart, one is told a fall of 0.04 and
then none, over and over, the next two 0.08 and 0.12, the fourth nothing at all - and the
same four patterns come out of this server).

### What clients are told

The entity tracker was already `ServerEntity`; physics gives it what it reads: `needsSync`
(a push, an item's change of velocity), `syncPosition` (a bounce brings the next update
forward), exact positions for a primed TNT that touches a block and for an item that lies
against one (`isFullPrecisionEncodingRequired`). A teleport of something a client moves by
itself goes out at once (`teleport_entity`, `Entity.sendTeleportTransitionToPlayers`).

### What it costs

`bench physics` (i5-9300H, `release`): 10 000 entities on a 128 x 128 block patch, nanoseconds
per entity per tick, by what the entity is doing. "Clutter" is a slab, a stair, a carpet, a
chest or a block on every fourth column; "fences" adds fences and walls to that, so that every
query has to look at the cells beside its box.

| | flat ground | clutter | clutter + fences |
|---|---|---|---|
| an item falling through open air | 61 | 62 | 67 |
| an item thrown: through the air, onto the ground, to a stop | 204 | 289 | 827 |
| an item lying still, ticked in full | 135 | 185 | 574 |
| ...at rest, as the server ticks it | 4.3 | 4.5 | 4.3 |
| a primed TNT on the ground | 321 | 461 | 1209 |
| a mob (a cow's size) falling | 78 | 83 | 72 |
| ...walking into whatever is there, stepping up | 433 | 969 | 2123 |
| ...standing, ticked in full | 375 | 505 | 1215 |
| ...at rest, as the server ticks it | 0 | 0 | 0 |

What the numbers say: the blocks decide. Open air costs a mask per chunk and no block read;
on the ground a tick reads a dozen cells and builds a box for each that is not air; a fence
within a section makes every query of that section read some fifty cells instead of eight,
and that is three to four times the tick. What lies still costs nothing to speak of, which is
what a world mostly holds.

**Against vanilla.** `tools/vanilla/physics_probe.py perf` puts 10 000 entities into a vanilla
26.3 server - far from its one player, in force-loaded chunks, 1.5 blocks apart - runs the
game flat out (`/tick sprint`) and takes the time of a tick without them (1.3 ms) off the
time with them. Same machine, nothing else running but for the last row (see the note):

| 10 000 of them | a vanilla tick | per entity | here, per entity | vanilla / here |
|---|---|---|---|---|
| items lying still | 60 ms | 5.9 us | 4.3 ns at rest (135 ns ticked in full) | 1361x |
| items falling through open air | 120 ms | 11.9 us | 61 ns | 196x |
| armor stands standing | 142 ms | 14.1 us | 0 at rest (319 ns ticked in full) | 44x even ticked in full |
| armor stands falling | 137 ms | 13.5 us | 77 ns | 176x |
| primed TNT on the ground | 117 ms | 11.6 us | 321 ns | 36x |

(A vanilla server cannot hold 20 ticks a second with ten thousand items lying about on this
machine: 60 ms a tick. During the TNT row a game client was started on the machine; its other
runs were 103 and 125 ms.)

The two columns do not measure the same thing, and both ways of reading them are worth
saying. Vanilla's is everything those entities add to its tick: the entity's own tick - which
is the physics, but also its checks for fluids, fire and portals and, for an item, a look for
neighbours to merge with - the tracker's `sendChanges`, and the bookkeeping of entity sections.
And a good part of it, going by where thread dumps found the server (two of four taken during
the TNT row), is one thing that this server does not do at all yet: every `move` of every
entity asks for the entities it could collide with (boats, shulkers), which walks every entity
of the sections around - 114 of them a section in this layout. Where entities are fewer to
the block, vanilla is that much faster; here there is nothing to ask until such entities
exist, and then the ones to ask about are those few, in a grid of their own.

Ours is the physics alone. For the whole tick, `tests/integration/run.py -k ten_thousand
--slow` does on a running server what the probe does on vanilla - the same layout: 150 bots
each walk a row and drop a stone every block and a half, and leave; then 10 000 armor stands
are summoned among them:

| nobody online | a tick | of it: the entities' ticks | the entity tracker |
|---|---|---|---|
| 10 200 items lying still | 1.3 ms | 0.39 ms | 0.93 ms |
| ...and 10 000 armor stands | 2.2 ms | 0.46 ms | 1.76 ms |

That is where vanilla takes 60 ms, and it is the slow end of what this machine gives: a server
that sleeps 49 ms out of every 50 starts each tick on a core that has cooled down and clocked
down, and the same passes in a loop are several times faster (the first table's 4 ns an item
against 38 here; two earlier runs of the scenario, with the bots' Python still warm on the
other cores, took 0.7 and 0.8 ms a tick). Two things to know about what is left:

- With nobody watching, most of it is the entity tracker, which asks every entity every tick
  whether it has something to say (`ServerEntity.sendChanges` does, and an item at rest has,
  twice a second). That is the next thing to look at, and it is not physics.
- Items in a pile cost more than items apart: every 40th tick of its life an item looks for
  others to merge with, and the look is as long as the pile. With each bot dropping 68
  different items on one spot instead, the entities' ticks were 57 ns an item. Vanilla's look
  is as long.

The state a tick at rest reads is 32 bytes an item in an array of its own (`Game::ItemSleep`),
not the 200 of everything an entity keeps about how it moves: the first version read the
latter, and one cache miss an item was two thirds of the pass.


### Compared with vanilla, and what is not there

- Fluids: nothing floats, nothing is carried. No block holds one yet.
- A brain. `living_tick` is `LivingEntity.travel` with no input: a mob falls, slides, steps
  where it is pushed, and stands. The classes that move in a way of their own - what flies,
  swims, hangs, climbs or flops (`gen_physics.OWN_MOVEMENT`) - are not moved at all.
- Damage: no fall damage, no suffocation; what falls out of the world is removed at once.
- A player is moved by its client. It pushes the mobs it walks into, and is not pushed back
  here (vanilla gives it a velocity that its viewers are told).
- With several neighbours at once, `pushEntities` adds their pushes in the order of our grid's
  cells, vanilla in the order of its entity sections: the sum may differ in its last bit.
- A living entity that bounces is sent as one step of its path, not two.
- Shapes of several boxes are grids cut by their boxes' faces. Vanilla's own grid for such a
  shape can have more cuts; that shows only for an entity that is already inside the shape.
- Entities that collide with each other (boats, shulkers), the world border, pistons, powder
  snow, climbing.

## The world's life cycle (src/world/dimension.*, store.*, region.*, chunk_io.*, src/base/workers.*, src/server/chunks.cpp, persist.cpp, import.cpp)

A world is larger than memory and older than the process. Chunks come from somewhere when
somebody approaches - a file, or the generator - go somewhere when everybody has left, and are
on disk when the server stops; and none of that may hold up a tick.

```
 tick thread                                          a worker
 request(chunk) ── where is it? ──────────────────▶   read, check, inflate, decode - or generate;
                                                      heightmaps, sky sources, palette counts
 adopt: copy into the pools, tell the light  ◀──────  a chunk outside the world, in its own arena
 engine; lit, then ticking, as neighbours come
 a frame is wanted: write the packet's body ───────▶  deflate, frame
 cache it, send it  ◀──────────────────────────────   the frame
 idle, or changed long ago: the chunk as bytes ────▶  (with others of its region) deflate, write,
 the region's new table  ◀─────────────────────────   sync, switch the entries, sync
```

### Workers (`base::Workers`)

N threads, and for each of them one SPSC ring of tasks going out and one coming back - the same
rings the reactors use, and the same rule: nothing mutable is shared. A task is a small record
with two functions: `work` runs on a worker, `done` runs on the tick thread when it next polls,
with whatever `work` left in the record. What a task points at is the task's while it is out. A
task goes to the worker with the least outstanding work (the caller says how heavy each is): one
queue per worker means a short task can stand behind a long one, and this keeps that rare.

A worker is handed a whole thing - bytes, a file descriptor and an offset, a generator to run -
and hands a whole thing back. It never sees the world, the pools the world's sections and light
come from (`world::BlockPool` is the tick thread's), or the patch store of item stacks. A chunk it
builds lives in an arena of its own (`world::LoadedChunk`); adopting it copies its sections into
the pools - what is copied is the packed form, a few kilobytes a section - and the arena goes.

`Workers(0)` has no threads: the work runs inside `submit`, the answers come with `poll`. The unit
tests drive the whole pipeline that way, and the same tests run it with threads.

### The tick thread knows where everything is (`world::ChunkStore`)

Which regions exist, where each chunk lies in its file, what is waiting to be written: all of it
is the tick thread's, in memory - a copy of every open region's table (16 KiB). So "is there a
chunk here?" never touches the disk (a chunk without an entry goes straight to the generator),
a reader needs nothing but a descriptor and a copied entry (`pread`), and there is one writer per
region at a time, which owns that region's `RegionFile` - its table and its map of free sectors -
for as long as it runs. Loads are handed on in the order they were asked for, whichever worker
finished first: what the world makes of two neighbours depends on which came first (below), and
this way it is the same on every run.

A chunk that is saved is first only bytes held by the store. Its region is written when the first
of them has waited five seconds, or 256 have gathered, or the server stops. A chunk that is asked
for while its bytes wait, or are being written, is decoded from those bytes: they are newer than
the file.

### Region files (`world/region.hpp`)

One file per 32 x 32 chunks, as in Anvil, and little else as in Anvil: a table of 1024 entries
(first sector, stored bytes, raw bytes, CRC-32), then chunks in runs of 512-byte sectors,
deflated. A chunk of a flat world is some seventy bytes and takes one such sector, where Anvil's
smallest unit is 4 KiB; a chunk can be any size; bytes that are not what the table says are found
by their checksum instead of being parsed.

A chunk is never overwritten in place. Its new bytes go to free sectors; the file is synced; the
table is written (an entry is 16 bytes inside one disk sector); the file is synced again; only
then are the old sectors free. Whenever the process or the machine stops, every entry points at a
whole chunk - the new one or the one before. (`sync-chunk-writes=false` leaves the syncing to
the kernel: a crash of the process loses nothing, a crash of the machine may lose a batch.)

A chunk's bytes (`world/chunk_io.hpp`) are its sections in the layout they have in memory, its
biomes, its light layers as they are - a layer that stores nothing is saved as such - and three
parts nobody in `world` reads: the game's block entities, the game's entities
(`server/persist.cpp`), and whatever an import kept. Heightmaps, sky sources and counts are not
stored; the worker that reads a chunk derives them. Ids are this version's: `world.json` names
the data version a world was saved by, and another version refuses it until there is a converter.

### The stages of a chunk

A chunk's light depends on its neighbours' blocks, so a chunk has the stages vanilla's has:
*loaded* (its sections registered with the light engine), *lit* (its own sources propagated - all
eight neighbours must be loaded), *ticking* (all eight neighbours lit: its light is final, it may
be sent, edited, ticked). A chunk that arrives is promoted as far as its neighbours allow, and so
are they. For a view to tick, then, the chunks around it are lit and the chunks around those
loaded: the game asks for the view widened by two.

A chunk leaves through the same door. The chunks around it stop ticking - light can no longer
cross into it - until it is back.

### Views, and sending (`server/chunks.cpp`)

What a client is sent, and when, is vanilla's:

- **The view** is `ChunkTrackingView`: around the chunk the player is in, the chunks with
  `max(0, |dx| - 2)^2 + max(0, |dz| - 2)^2 < distance^2` - a square one chunk wider than the
  view distance, with its corners cut. When the player crosses into another chunk the client is
  told the new centre, then what left the view (`forget_level_chunk`, in the order
  `ChunkTrackingView.difference` walks: x, then z); what entered it is marked to be sent.
- **Sending** is `PlayerChunkSender`, the last thing in a tick: the nearest of what is marked, as
  many as the client's pace allows - nine a tick until its first `chunk_batch_received`, then
  what it asks for, at most ten batches unanswered. A chunk whose frame a worker has not made
  yet waits a tick; nothing goes out before the chunk the player stands in.
- **Tracked** (`ChunkMap.isChunkTracked`) is "in the view and sent". Block changes go to those
  who track the chunk, light to those who have it on the edge of what they track, and an entity
  is shown only in a tracked chunk.

There is still no list of viewers per chunk: three columns (centre x, centre z, distance) of all
players are scanned, eight players a step, for "who has this chunk in view" as for "does anybody
still need it".

### Leaving, and being saved

The chunks are walked a few a tick (`ChunkMap::scan`), all of them in half of
`chunk-unload-seconds`. One that the walk has found in nobody's widened view for that long
(10 s by default) is saved and leaves: its bytes go to the store, the block entities in it and
the entities standing on it are written into them and taken out of the game, the light engine is
told. One that stays is saved when the walk finds it changed (`Chunk::dirty`: a block, a light
value, a block entity, anything standing in it) and last written `autosave-seconds` ago or more -
vanilla's autosave period, 300 s, without the wave of writes every five minutes. At shutdown
everything that is loaded is written, and the server waits for it.

An entity is saved with the chunk it stands in, and comes back as vanilla's does: a new entity
with the state `Entity.saveWithoutId` keeps - where it is, how it moves, how far it has fallen,
an item's age and pickup delay, a fuse - and everything a tick works out again left to the next
tick. One that is thrown past everything that is loaded has the chunk under it brought in, so
that there is a chunk to keep it. A stack - in a chest, on the ground - is written in the form a
client sends one in, and read back by the reader that reads a client's: a file is trusted no
further.

`level-name` is the world's directory; `session.lock` in it is vanilla's file, held with
vanilla's kind of lock, so a world is run by one server of either kind. An empty `level-name` is
a world that is not kept: nothing is written, and no chunk ever leaves memory.

### Light, across leaving and coming back

The light engine is a port, and so is this half of it (`ThreadedLevelLightEngine.updateChunkStatus`,
`LayerLightSectionStorage.queueSectionData` / `markNewInconsistencies`): a chunk that leaves has
its sections count as empty - the sections around it that stored light only because of its
blocks stop storing it; a chunk that comes brings the layers it was saved with, which wait
(`Chunk::queued`) until their section stores light - the section beside it that makes it do so
may be in a chunk that has not arrived yet. A chunk that was saved lit is not lit again.

Two things are this server's own. Every layer is saved as it is, also one that was created and
never written (vanilla leaves those out, and creates them again by rules that depend on what is
around at that moment). And a layer that goes because *the chunk beside it left* is kept, queued,
for the day that chunk is back: vanilla drops it, and has it on disk by then because it unloads
a dozen chunks further out, where nothing is ticking or being built on; here the two chunks can
be neighbours at the edge of what is loaded. With both, a chunk comes back as it left. That is
tested, not argued: hills with glowstone in them and towers on chunk borders, built on, every
frame a client would get recorded; then all of it saved, unloaded and read back in another order
- and, twelve times over, random chunks unloaded and read back while their neighbours stay -
and every frame compared byte for byte (`tests/unit/store_test.cpp`, `Persistence.*`).

### A vanilla world, imported (`server/import.cpp`)

A directory with a `level.dat` and no `world.json` is a vanilla world, and is imported before
the server starts to listen: every chunk of its region files is written into ours, by the
workers, region by region. The vanilla files are read and never changed. Only a world of this
version's data version is taken - an older one is upgraded by vanilla first, and the error says so.

What nobody documents about 26.x was read from the jar: the world's layout
(`dimensions/minecraft/overworld/region`, `data/minecraft/world_gen_settings.dat`), and that a
block state is saved as its block's name alone when it is the default one and as
`{id, properties}` otherwise (it was `{Name, Properties}`).

- **Blocks and biomes** as they are, palette order included: a chunk that has just been loaded
  goes out with the palettes vanilla would send.
- **Light**: the saved layers, queued as vanilla queues them when it loads the chunk.
- **A chunk vanilla had not finished generating** (any `Status` but `minecraft:full`) holds
  nothing anyone made, and is left to the generator here.
- **Block entities**: a chest or a furnace whose tag says nothing this server would not act on -
  stacks without components, no name, no lock, no loot table - becomes one of ours. Every other
  one is kept as its tag: a client is told that it is there, and it does nothing - a chest with
  an enchanted sword in it stays shut rather than open as if it were something else.
- **Entities**, and **the rest of a chunk's tag** (structures, scheduled ticks, `InhabitedTime`...):
  kept as tags, with the chunk. No entity of an imported world is in the game yet.
- **The generator**: a flat world goes on as it was made (its layers and biome are read from
  `world_gen_settings.dat`). For vanilla's terrain there is no generator here: beyond what was
  explored there, the world ends (`world::EmptyGenerator`). What a stand-in generates is marked as
  never changed (`Chunk::modified`), so a real generator can take those chunks over.

Nothing is dropped that a later export could not write back: what is not read is kept, and what
is neither - heightmaps - follows from the blocks.

### Compared with vanilla

`tools/vanilla/world_probe.py` has a vanilla 26.3 server build a world from its console - states
with properties, light sources at chunk borders and a roof across one, sections with two and with
nine biomes, chests plain, double, named and with a damaged sword in one, a furnace, a sign,
entities - save it and stop; then starts vanilla on it again, and this server on a copy, and
compares every `level_chunk_with_light` a bot is sent. Of the 42 chunks both bots were sent:

| | chunks |
|---|---|
| the same in every part: blocks, palettes, biomes, heightmaps, light masks and layers, block entities | 41 |
| ...of those, the same bytes | 0: the heightmaps are a map on the wire, and vanilla writes them in the order of a `HashMap` of enums - another one each time a JVM starts |
| one sky light layer, the one below the world, sent as 2048 zeros instead of "empty" | 1 |

The last is the bookkeeping mentioned above, and vanilla has it too - two chunks of the recording
carry such a layer on both servers: when a chunk is the first of its neighbourhood to be
registered with the light engine, the layer under its lowest section is made by copying the
bottom of the layer above (`SkyLightSectionStorage.createDataLayer`), which gives stored zeros;
when a neighbour was there first, it is made empty. Which chunk is first is the order of loading.
No light value differs.

What does differ, and is a gap rather than a difference of the import:

- **Update tags.** Vanilla sends a sign's text, a banner's patterns, a skull's owner with the
  chunk (`BlockEntity.getUpdateTag`); here every block entity goes out without a tag - right for
  the chests and furnaces, which have none, and a blank sign for a sign. It needs each kind's
  `getUpdateTag`, read from the jar.
- **Entities** of the imported world are not shown (the armor stand and the emeralds of the
  fixture): they are kept, unread.
- **Blocks the server does not simulate** stand as they were saved: water does not flow, a lamp
  does not react, a door does not open. (`AGENTS.md`, rule 9: a block whose class is not ported
  does nothing rather than something invented.)

### What is not there

- **Players.** Nothing of a player is saved: where it was, what it had. `players/` of an imported
  world is left alone.
- **A terrain generator.** Flat ground, or nothing.
- **Export** back to Anvil; a converter for worlds saved by another data version.
- **lz4** region files (`region-file-compression=lz4`): the import says so and stops.
- **Other dimensions**: one dimension is all there is; the nether and the end of an imported world
  are not read.
- **`save-all`, `save-off`**: the server saves on its own schedule and at shutdown.
- A chunk is lit on the tick thread, as before. For the flat generator that is nothing; a
  generator of real terrain will want the first lighting off the tick thread too.

## NBT and text components (src/nbt/, src/text/)

### A tag stays the bytes it is (`nbt::Doc`, `nbt::Writer`)

NBT is the tag tree of vanilla's saves and of everything structured in its protocol: text
components, block entities, custom item data. Vanilla loads a tag into objects - a `HashMap` per
compound, an object per number. Here a tag is never anything but its bytes:

- **Reading** (`nbt::Doc::parse`) is one pass that checks the bytes the way `NbtIo` loads them
  and leaves a *tape*: a 20-byte record per tag - its type, where its payload starts and ends,
  which record follows its subtree. Numbers inside lists and arrays get none; their place is
  arithmetic. Any tag is then reached, and any subtree skipped, without reading bytes twice:
  finding a compound's entry is a walk over its entries' records, not over what they hold. A
  `nbt::Tag` is a 16-byte view (document, record) with vanilla's accessors on it: a number as
  `Number.intValue()` hands it out, a list's elements as `ListTag` holds them.
- **The loader's rules are vanilla's**, each read from the jar: 512 compounds and lists deep;
  strings in modified UTF-8 as `readUTF` takes it; a name read twice keeps its last value; a
  compound in a list that holds nothing but a tag named "" stands for that tag (how a list of
  mixed types travels); and `NbtAccounter` - every tag is charged what vanilla takes its Java
  object to weigh, and a tag from a client may weigh 2 MiB. The quota is also what bounds the
  tape: a megabyte of empty compounds is fifty in vanilla's books and refused, here as there.
- **Writing a tag back** (`Doc::write`) gives the bytes vanilla writes for the tag it loaded -
  repeats gone, list elements wrapped only where the list is mixed, an empty list without a type -
  with one deliberate difference: a compound's entries go out sorted by name. Vanilla writes
  them in the iteration order of its `HashMap`, which depends on the order they arrived in and
  tells a reader nothing. Sorted, equal compounds are equal bytes - which is how the patch store
  sees that two stacks carry the same data.
- **Building** (`nbt::Writer`) appends to a `ByteBuf`, one capacity check per tag.

Strings are compared and copied as bytes. That is exact for the bytes Java writes
(`Doc::plain_strings`); a document that spells a character another way - a raw NUL, a letter in
two bytes - only comes from a hand-made client, and callers refuse it rather than normalise it.

### A text component is read by a codec, not by a layout (`text::transcode`)

A component has no wire layout. It is a tag that `ComponentSerialization.CODEC` makes sense of -
`"hi"`, `{text:"hi"}`, `["hi"]` and `{type:"text",text:"hi",anything:1}` are one text - and a
vanilla server passes none of these on: it decodes the tag into a `Component` and encodes that.
For a server that stores wire bytes this is the whole problem: stacks named `"hi"` and
`{text:"hi"}` have to be one stack, and no client may be sent what another client spelled.

`text::transcode` is that round trip without the object. It reads the tag as the codec does -
the branches of an `Either` in their order, contents without a `type` matched as "the first
type that fits" (text, translatable, keybind...), optional fields strict or lenient as declared,
`Codec.BOOL` taking any number - into a flat description (contents, style, children, arguments;
the strings stay in the document), and writes the tag the codec writes: plain text collapsed to
a string, colours and identifiers in vanilla's form, lists wrapped as `ListTag` wraps them. And
in vanilla's order: a compound is a `HashMap` there, so its entries leave bucket by bucket -
`color` before `text` before `bold` - and move when a thirteenth entry makes the table grow.
`map_order` reproduces that from the names' hash codes, so the bytes are the ones a vanilla
server sends.

That claim is measured, not read off the jar: a codec's behaviour is the sum of many small rules
of DataFixerUpper, and one of them misread makes other bytes. `tools/vanilla/component_probe.py`
puts 221 stacks into a bot's hand on a vanilla 26.3 server and on this one - every spelling
above, each style field alone and all of them at once, the colours, the numbers a flag may be,
click and hover events, lore, books, signs, custom data, and for each what is wrong with it -
and compares what a second bot is then shown:

| | cases |
|---|---|
| the same bytes sent on, or the sender thrown out by both | 193 |
| vanilla takes it, this server does not read it and refuses the stack (the list below) | 20 |
| vanilla throws the sender out, this server only refuses the stack (URLs `java.net.URI` rejects, a shadow colour of three floats) | 6 |
| the same compound, entries by name instead of in HashMap order (custom data) | 1 |
| an empty lore: vanilla drops what the item says by default, a patch is not held against the defaults here | 1 |

Nothing is taken that vanilla throws out, and nothing thrown out that vanilla takes. (The probe
found one such case: an entity type that does not exist is a pig to vanilla, not an error - its
registry has a default.)

Not read, and answered "unsupported" (the stack is refused): the contents score, selector, nbt
and object (a selector is a parser of its own); the hover events `show_item` and `show_entity`;
the click events `show_dialog` and `custom`; a shadow colour given as four floats; a URL that is
not plainly one (`java.net.URI` decides those; `http(s)://host[:port]/path?query#fragment` in
RFC 2396's characters is taken).

### What it costs

`bench nbt`, `bench text`, `bench item` (i5-9300H, `release`):

| | |
|---|---|
| read a chest of 27 stacks: 2.5 KiB of tags, 160 records (3.2 KiB) of tape | 2.6 us (0.95 GB/s) |
| find one of its entries by name | 42 ns |
| write it back, names in order | 4.5 us |
| build the same chest with the `Writer` | 0.8 us |
| a styled name - colour, flags, a hover text, a child - through the codec | 0.6 us |
| read a stack with a plain name from a client | 0.3 us |
| read a stack with a styled name and two lines of lore | 2 us |

All of it is paid once, when a client hands the stack over (`set_creative_mode_slot`). From then
on the stack is the 8 bytes it always was, and its text is bytes to copy. A stack with a plain
name costs what one with two plain components costs: the patch store, not the tag.

## Commands (src/server/command.*, command_args.cpp, commands.cpp, console.*)

### The tree is Brigadier's, because the client has a copy

A client is sent the command tree (`commands`) and reads every line against it while it is
typed: what it colours as valid, what it completes, which usage it shows. So the tree here is
not ours to design. `cmd::Registry` is `CommandDispatcher`: literal and argument nodes, children
in the order they were added, a node that redirects (`tp`), a command on the nodes a line may
end at - and `Registry::parse` is `parseNodes`, read from brigadier-1.3.11.jar: a literal child
spelled like the next word shuts out its argument siblings; argument siblings are all tried;
among the readings the one that used the whole line wins, then one without errors, otherwise
the first tried; what is reported is the one exception of the last node if there was exactly
one, else "Unknown or incomplete command" (nothing matched, or the line ended early) or
"Incorrect argument for command", at the cursor the reading stopped at. `error_context` is
`Commands.finishParsing`: the ten characters before that cursor, the rest underlined, `<--[HERE]`.
None of this is guessed at by a test client: `tools/vanilla/command_probe.py` types some 120
lines, right and wrong, at a vanilla server and at this one and diffs every answer.

### A command is a line and a function

    r.add("give <targets:players> <item:item> [<count:int(1)>]", cmd::kGamemasters, &give);
    r.add("teleport <targets:entities> <location:vec3> [<rotation:rotation>]", cmd::kGamemasters, &teleport);
    r.add("fill <from:block_pos> <to:block_pos> <block:block_state> [(outline|hollow|destroy|strict)]", ...);
    r.alias("tp", "teleport", cmd::kGamemasters);

A word is a literal, `<name:type>` an argument, `(a|b)` one literal of several, `[` makes what
follows optional. Lines that begin alike share their nodes, so a command is written out the
way its usage reads, one line per form, and the tree that results is the one vanilla's builder
chain makes - names and order included, which is the part a client shows. The types
(`cmd::Type`) are vanilla's argument types that have a parser here; each is the port of its
class (`command_args.cpp`), with its errors and the cursor it leaves them at.

A handler is `i32 (*)(cmd::Invocation&)` and a `void*` of its own. It reads what was typed by
name (`in.integer("count", 1)`, `in.has("hollow")`, `in.value("pos")`), asks the game what a
name stands for at that moment - a selector is kept as parsed and resolved when the command
runs, as `EntitySelector` is - and answers through `in.success(text, broadcast)` or
`return in.fail(text)`. No templates, no captures, no exceptions: this is the surface a plugin
gets. A Lua plugin's command is the same `add` with one C trampoline as the handler and the
script's function as `user`; adding or removing a command bumps `Registry::version()`, and the
tick that sees it sends everyone their tree again.

The server's own commands are written against exactly this (`Game::Commands`,
`server/commands.cpp`), which is how the surface was found to be enough.

### Who may, and who hears

Permission levels are vanilla's five; a command's root node has one, a source has one, and a
node the source may not use does not exist for it - not in `parse`, not in the tree it is sent
(`Commands.sendCommands`; one encoding per level, cached until the tree changes). Operators
are `ops.json` in vanilla's format, read at start and written by `/op` and `/deop`; an operator
gets `op-permission-level`. A client is told its level by entity event 24 + level, then sent
its tree - at join, and whenever the level changes.

The console is the other source: a thread blocks on stdin and hands lines to the tick thread
through an SPSC ring (`Console`), where they run after the players' packets, as in vanilla,
with level 4, from the world's spawn. It is how the first operator is made.

`CommandSourceStack.sendSuccess` is `Game::command_success`: the source reads the message; if
the command says so, every other operator reads `[Steve: message]` in grey italics and the log
gets the same line. A failure is the message in red, to the source alone. A console's answers
are printed as text: `text::Text::plain()` fills translations in from en_us.json, generated
from the jar (`lang.hpp`: the 4000 keys a server prints - commands, arguments, the names of
blocks, items and entities - 215 KiB of text in the binary).

The tenth command in a row disconnects a player who is not an operator (`TickThrottler`: 20 a
command, 1 back a tick, `command-spam-threshold-seconds` x 20 at most). That, and the 32768
blocks a `/fill` may touch, is what bounds the cost of this path - which is why it allocates
freely: a line a person typed is parsed into vectors and strings, a `Text` owns its parts, and
none of it is ever per tick. What it costs when it is used is under "What it costs" below.

### What a command says (`text::Text`)

Vanilla's commands answer with `Component.translatable(key, args)`, and what the arguments are
is part of the wire: a number travels as an int tag, a player as a text with a click event
(`/tell name `), a hover event (`show_entity`: type, UUID, name) and an insertion; a stack as
`[Stone]` in the colour of its rarity with a `show_item` hover. `Text` builds such a component
call by call - `Text::translatable("commands.give.success.single").arg(3).arg(item).arg(who)` -
and `encode()` writes it through `text::transcode`, the one place that knows the form vanilla's
codec gives a component (and the order of its HashMap). That needed the two hover events
transcode did not read; they are read now for clients' components too.

### Selectors, blocks, coordinates

`Game::select` is `EntitySelector.findEntities` / `findPlayers` over the entity table: by name,
by UUID, or `@p @a @r @s @e @n` with the options `type` (and `#tags`, negated or not), `name`,
`distance`, `x y z`, `dx dy dz`, `x_rotation`, `y_rotation`, `limit`, `sort`, `gamemode`,
`level`, `tag`, `team` - the last four against what every entity here has (creative, level 0,
no tags, no team). Entities are found by a scan of the table's columns, players by a scan of
the player rows: no index is kept for a question a person types.

A block argument is a block's name and properties (`chest[facing=east,type=left]`), resolved
against the block's property table (generated: every block's properties, values and strides),
or - where vanilla allows it - a `#tag`. What `/setblock` and `/fill` then do is
`BlockInput.place` and vanilla's flags for `Game::set_block` (`kUpdate*`): the state takes the
shape its neighbours give it, the block it replaces does not spill, the neighbours are told
afterwards - all at once for a `/fill`, so a TNT in the region is replaced before the redstone
beside it could light it - and `strict` leaves out both. Rule 9 holds here as for a hand: a
block whose class is not ported is refused, in words of this server's own.

Coordinates are `WorldCoordinates` and `LocalCoordinates`: `~` from where the source stands, `^`
along where it looks, a whole number in a `vec3` the middle of its block. `/tp` sends a player
`player_position` with vanilla's relative flags - a `~` is an offset the client adds to where
it is, velocity kept - and waits for the confirmation like the teleport at join does.

### Compared with vanilla

`tools/vanilla/command_probe.py` joins two bots, makes one an operator from the console, and
has it run the commands above in their forms - and what is not one: unknown blocks, bad
properties, selectors that do not parse, too many items, places outside the world - while the
second watches, is given things, moved, opped, deopped and kicked. Every system message either
of them gets is compared as bytes (UUIDs of summoned entities aside), with the block changes,
teleports, sounds and the command tree.

Of the 186 steps (93 lines, each as two bots see it) 185 come out the same: every answer and
every error with its `<--[HERE]` line as the same bytes, what the other operator is told, the
order of `/help`, the blocks a `/fill hollow` or `replace #planks` changes, the offsets and
flags of a relative teleport and the second player's absolute one, the item that flies into a
player on `/give` with its sound, a sound played from two blocks away at its least volume, the
`stop_sound` variants, the level a player is told when opped and deopped, the reason a kicked
player reads. The command trees agree node for node as far as ours goes - names, order,
parsers and their properties, what runs, what redirects - with what differs listed here:

- `/op` and `/deop` name `minecraft:ask_server` for their argument's suggestions in vanilla
  (who is not an operator yet; who is one); here the client completes player names itself;
- vanilla shows a summoned entity the moment it is made, before the command's answer; here it
  appears with the tick's other entities, after it (the tracker's one deliberate difference);
- left out of the comparison because the things do not exist here: "has made the advancement"
  after a `/give`, "left the game" after a `/kick`, grass turning to dirt under a block a
  command put on it;
- a vanilla server asked to op a name it has not seen asks Mojang whose it is - even in
  offline mode - and ops that account. Here an unseen name in offline mode is its own profile.

The same recording run found what reading the classes had not: that `MutableComponent.append`
drops an empty string (no `""` child in front of an error at cursor 0), that an unknown
entity type leaves the cursor behind its name where every other parser puts it back, that
`/help` lists `deop` before `op`, and that a server treats the position a client confirms a
teleport with as a move - a bot that echoed a relative teleport's offsets was put right with
a second, absolute one. The hover events `show_entity` and `show_item`, which the answers
needed, went into `tools/vanilla/component_probe.py` with their spellings: the same bytes.

### What it costs

Reading a line is 1 - 3 us and building and encoding its answer another 3 us (`bench
command`: 0.6 M lines/s for a `setblock` with properties, 0.3 M for a `tp` with a six-option
selector or a `fill` with a tag filter, 1.0 M for a line that fails at its second argument;
0.3 M answers/s for "Gave 3 [Diamond] to Steve" with both hover events). Next to the tick a
command runs in that is nothing; what a command *does* is the cost. The largest `/fill` there
is, 32768 blocks, takes the tick it runs in: 50 - 57 ms for a 32 x 32 x 32 cube of stone and
for clearing it again (the block changes, their light, one `section_blocks_update` per
section), 33 ms for a hollow shell of 5768 glowstone, where the light is most of it. (Release
build; measured while two game clients were running on the same four cores, so on the high
side.) The limit is vanilla's (`maxBlockModifications`); it is the one command here that costs
a tick, and only an operator can type it.

The binary grew from 1.2 to 2.2 MiB: en_us (215 KiB of text and its index), the names of
1658 items with their translation keys, the property tables of 1286 blocks, and the parsers.
None of it is touched unless a command runs.


### Not there

- NBT in any argument: a block's `{...}`, an item's `[components]`, `/summon`'s tag, a
  selector's `nbt=`. It needs an SNBT reader and, per component, its `Codec`.
- `/tp ... facing`, `/rotate`: the `player_look_at` packet and `Entity.lookAt`.
- What is summoned has no brain: a mob falls to the ground, is pushed, and stands (see
  "Physics" for which kinds; what flies, swims or climbs is not moved at all). What no tick
  here moves and a client would let fall by itself - a falling block, an arrow - is flagged
  `NO_GRAVITY`. `/summon item` makes an item without a stack, which its first tick discards,
  as in vanilla.
- `/kill`: the entity is gone at once - no death animation, no drops - and a player cannot be
  killed: there is no health and no respawn.
- `/kick <reason>`, like every `message` argument: plain text; selectors inside it are not
  resolved, and a signed command's signatures are read past (no secure chat).
- `/op` of someone who is not online works by name only in offline mode (the name is its own
  profile); in online mode Mojang would have to be asked.
- Suggestions the server would have to compute (`ask_server`): `/op` and `/deop` leave their
  argument to the client's own completion of player names.
- Every command about something that does not exist: chat (`say`, `msg`, `me`, `tellraw`),
  time and weather, game modes, effects, experience, scoreboards and teams, `execute`, `data`,
  `clear` (the inventory can do it; its item predicates are the work), bans and the whitelist.

## Wire format handling

- **VarInt** (`proto/varint.hpp`): one-byte values take a predicted branch; longer ones are branch-free
  with `pdep`/`pext`. Frame lengths are written as fixed 3-byte padded VarInts (vanilla's
  `Varint21FrameDecoder` accepts non-minimal encodings — checked in the jar), which lets a frame be
  written in place and its length patched afterwards with no `memmove`.
- **Encoding**: `write_packet` reserves `bound() + header` once, then the packet's `encode()` writes
  through a raw pointer. Bodies ≥ the threshold are deflated with libdeflate (level 4) into thread-local
  scratch and copied back; smaller ones cost one extra zero byte. A packet whose `bound()` is tiny
  (≤ 126 bytes with its id) gets the one-byte length vanilla writes instead of the padded three; the
  bound is a constant for most packets, so the choice costs nothing (framing went from 0.66 to over
  1 billion packets/s in `bench frame`).
- **Decoding**: `proto::Reader` is a pointer pair with a sticky `ok` flag; strings and byte arrays are
  views into the packet. Handlers check `ok` once.
- **AES-CFB8** (`crypto/cfb8.*`) costs one AES block per byte and encryption is serial per stream.
  Decryption runs 8 blocks in parallel (the feedback is ciphertext we already have). Encryption
  interleaves up to 8 *connections* so the AES unit's pipeline is full. Measured on an i5-9300H
  (one AES port): 83 MB/s single stream → 358 MB/s batched, i.e. the port's throughput limit.
- **Pre-baked packets**: the 32 `registry_data` packets and `update_tags` are serialized by `mcgen`
  at build time (`#embed`), framed/compressed once at startup, and sent to each client as one `memcpy`.

## Protocol versions

`src/proto/gen/v<protocol>/` is generated per version; `src/proto/gen/current.hpp` is the only file that
names a version directory. The server speaks exactly one version. Layouts in `packets.hpp` are written
against `proto::ids`/`proto::reg`, so a version bump is "generate, switch `current.hpp`, fix the layouts
that changed" (the `bump-mc-version` skill). Serving several client versions from one process would mean
per-version encoders for every broadcast packet (and per-version caches of every pre-serialized blob);
that is deliberately not designed in — a proxy-side translator is the cheaper answer if it is ever needed.

Registry entries are sent by name only (the `select_known_packs` handshake proves the client has the
vanilla `minecraft:core` pack of our version), in sorted order, which fixes the network ids that
`registries.hpp` exposes as constants.

## Lua plugins (design constraints, not built yet)

LuaJIT, one state, tick thread only. The intended shape:

- A C ABI (`extern "C"`, POD arguments, integer handles with generations) that LuaJIT FFI binds
  directly; the Lua C API stays off the hot path.
- Bulk data is exposed as columns (`double* x, y, z; u32 count`) so a script can loop over players or
  entities without a boundary crossing per element.
- Events: a bitmask of "has listeners" per event type is checked before anything is marshalled; packet
  hooks sit at the two choke points that already exist (`Game::on_packet` for inbound, `write_packet`
  callers for outbound).
- Lua errors never unwind through C++ frames (no exceptions here): all calls go through `lua_pcall`.
- Commands are the first piece of this that exists: `Game::commands()` takes a syntax line, a C
  function and a pointer (see "Commands"), and the handler reads typed values by name. A plugin's
  `register_command("heal <targets:players> [<amount:int(1,20)>]", fn)` is that call with a
  trampoline that pushes the `Invocation` and `lua_pcall`s the function kept under `user`.

## Known gaps and next steps

Roughly in dependency order:

1. **Mojang reports** (`mcgen.py reports`) → real tags and block ids. Until then vanilla clients cannot join.
2. **World**: chunks are stored, lit, cached as frames, loaded, saved and unloaded (see "The world's
   life cycle"). Next: a terrain generator - and with it the first lighting of a chunk on a worker -
   players' data, the update tags of the block entities that are kept as tags, export to Anvil.
3. **Entities**: the table, the tracker and physics exist (see "Entities", "Physics"). Next:
   attributes, the inferred jump, then mobs - spawning, AI; fluids. Known headroom in the tracker at 20 000 mobs: a
   summary bitmap over each row (the fan-out and the "what do I see" walk scan 320 mostly-zero words
   per player: 17 % and part of 53 % of that tick).
4. (Chunk pacing is there: `PlayerChunkSender`.)
5. Network follow-ups, none needed for correctness: `SEND_ZC` for chunk bursts, VAES (4 streams per
   zmm) for `cfb8_encrypt_batch` on AVX-512 machines, per-connection rate limits, thread pinning, proxy
   forwarding (Velocity modern forwarding), IPv6 dual-stack by default.
6. **Encryption off the tick thread.** Needed, not optional, for crowds in online mode: 500 players
   in one place cost 8 - 25 ms of AES per tick there (see "What a tick of this costs"). Encrypt on
   the reactors, or split the flush across workers.
7. **Items**: the component hashes of `container_click`, the rest of the components (stacks in
   stacks, the behaviour components), the crafting recipes that compute their result. Survival
   needs none of that first, only a game mode.
   Text: selectors and the other contents that are not read. (The server's own components are
   built with `text::Text`.)
8. **Blocks**: `docs/blocks-roadmap.md`. Neighbour updates, scheduled ticks and random ticks
   are there (see "Block updates"), so a block class is its own hooks now and no longer a
   missing system: slabs, stairs, fences and the other shapes, plants and what else needs
   something to stand on. Then fluids - with them fluid ticks, ice, waterlogging - redstone,
   the other block entities. And what the first four still lack: experience orbs.
9. **Commands**: an SNBT reader (block entity data, item components, `/summon`'s tag, `nbt=`),
   `facing`, then the commands that wait for their subject - chat first.
10. Lua plugin host.
