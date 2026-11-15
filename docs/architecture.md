# Architecture

What exists today is the network stack, a flat world that can be built in, the entity layer
that lets players see each other, items - stacks, the player's inventory, what is held and
worn, what lies on the ground - and the first blocks that do something: a chest, a furnace, a
crafting table, TNT. This document explains how the pieces fit and why; `AGENTS.md` has the
rules in short form, `docs/blocks-roadmap.md` the blocks that are still to come.

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
drain packets ─▶ block ticks ─▶ block + light changes ─▶ entity tracker ─▶ block events ─▶ entity ticks ─▶ block entities ─▶ per-player tick ─▶ flush
(move, click…)   (a chest's     (ChunkHolder             (ChunkMap.tick)   (a chest's      (items, primed   (furnaces)       (ack, equipment,
                  recount)        .broadcastChanges)                         lid)            TNT; menus)                       head, pickup, pose)
```

The order is observable, so it is copied: a block change reaches a client before the entity
movement of the same tick; the tracker runs before a player's own tick, which is why other players
see a head turn one tick after the body and a new pose one tick after the key that caused it; a
menu is brought up to date (`ServerPlayer.tick`) before the player's connection ticks, so a slot
changed by a pickup is sent with the pickup and one changed by a key (F) a tick later; block
entities tick after the entities, so what a furnace cooked in a tick reaches its menu in the next;
an explosion happens in the tick of its TNT, and the blocks it took go out with the next tick's
block changes - after the `explode` packet, as in vanilla.

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
- A newcomer is shown the entities around it at the end of its join tick (after its chunks),
  not as each chunk goes out: chunks are not paced yet.

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
- **No physics yet**, which shows in exactly one place: vanilla throws a dropped item and lets it
  fall. Here it is created at rest where vanilla creates it (0.3 below the eyes) and stays. A
  client simulates items as well, and would let it fall and be yanked back by every position
  update - so the item carries `NO_GRAVITY` in its entity data, which is the truth about it here.
  Both go when entities get physics.

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
- its items fall, so they merge after 2 ticks instead of 40, and land a little way off;
- the `NO_GRAVITY` entry above.

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
leaves its other half, TNT looks for a signal).

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
lid up. That recount is vanilla's scheduled block tick; until there is a tick queue for blocks
in general it has a queue of its own.

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

### TNT: the first thing that moves by itself (`src/server/explosion.cpp`, `src/world/collide.*`)

A primed TNT is a row of the entity table that `Game::tick_tnt` moves the way `PrimedTnt.tick`
does: gravity, `Entity.move`, drag, the fuse. The move is the port that entity physics will
grow from: `world::collide` is `Entity.collide` for something that cannot step up - the box
swept against the collision boxes of the blocks around it, axis by axis in vanilla's order with
vanilla's margins - and around it what a collision does to the velocity
(`restituteMovementAfterCollisions`: a slime block bounces), what the block underfoot does
(`getBlockSpeedFactor`: soul sand, honey), and `requiresPrecisePosition`, which makes the
tracker send exact positions for a TNT that touches a block. A client simulates primed TNT too,
so the two have to agree tick by tick; the probe below compares the height of the hop.

The explosion is `ServerExplosion`: 1352 rays that the blocks wear down, the entities within
twice the radius by how much of them the centre can see (`world::clip_hits`, the collision
shapes again), the blocks. What a block drops is its loot table - which is why `mcgen` reads
loot tables: for every block that can be in the world it reduces the table to what an explosion
without a tool leaves (stone: cobblestone; glass: nothing; glowstone: 2 to 4 dust), and stops
on a table it cannot reduce. TNT in the blast goes off on a short fuse instead of dropping;
players get their push in the `explode` packet, which is where a client takes it from.

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
| move a primed TNT: in the air / lying on the ground | 0.10 / 0.21 us |
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
and where they come from, the items a broken chest spills. Of a primed TNT: the fuse counting
down in its entity data every tick, the height of its hop tick by tick, the switch to exact
positions when it lands, the `explode` packet (its tail of particles and sound byte for byte,
the push it gives a player four blocks away within a few percent - it depends on which blocks
the rays took), the second TNT going off from where it fell, dirt lying about in stacks of 16.
What differs:

- vanilla shows a new entity the moment it is created, so `add_entity` of a primed TNT comes
  before its sound and before the `explode` packet of the TNT that lit it; here it comes with
  the tick's other entity updates (the tracker's one deliberate difference, as for items);
- vanilla swings the arm twice for a click on a block (`handleUseItemOn`), we once - as for a
  placed block;
- `update_recipes` at join carries the same nine property sets (which items a furnace takes...);
  vanilla also lists the stonecutter's recipes there, which no block here can use yet;
- not sent at all, each a known gap rather than a difference in what is sent: the sound of a
  block being placed and the particles of one being broken (to others), a player's footsteps;
- a furnace gives no experience when it is emptied, ores blown up by a player's TNT none
  either: there are no experience orbs;
- items do not move, so what a chest spills or an explosion drops hangs where it appears, and
  an explosion destroys items or leaves them but does not throw them;
- a cobweb does not slow a falling TNT and honey does not let it slide (`entityInside`), water
  does not carry it; a bounce off a slime block is computed but sent as a plain position;
- a player's inventory is not kept when it leaves, so what it had on a menu's cursor or in a
  crafting grid is not dropped at its feet.

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
- What is summoned is not ticked. A mob stands where it was put; anything a client would let
  fall by itself is flagged `NO_GRAVITY`, as items on the ground are. `/summon item` (an item
  entity without a stack, which vanilla discards in its first tick) summons nothing.
- `/kill`: the entity is gone at once - no death animation, no drops - and a player cannot be
  killed: there is no health and no respawn.
- `/kick <reason>`, like every `message` argument: plain text; selectors inside it are not
  resolved, and a signed command's signatures are read past (no secure chat).
- `/op` of someone who is not online works by name only in offline mode (the name is its own
  profile); in online mode Mojang would have to be asked.
- Suggestions the server would have to compute (`ask_server`): `/op` and `/deop` leave their
  argument to the client's own completion of player names.
- `/setblock ... destroy` breaks the block without the particles and sound of a broken block
  (`level_event` 2001), which nothing here sends yet.
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
2. **World**: chunk storage (paletted sections, bit-packed, SoA across sections), a framed-chunk cache
   keyed by chunk with invalidation on change, light, region file I/O on workers.
3. **Entities**: the table and the tracker exist (see "Entities"). Next: attributes, the
   inferred jump, then mobs - spawning, physics, AI. Known headroom in the tracker at 20 000 mobs: a
   summary bitmap over each row (the fan-out and the "what do I see" walk scan 320 mostly-zero words
   per player: 17 % and part of 53 % of that tick).
4. **Chunk pacing**: honour `chunk_batch_received` instead of sending the whole view at once.
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
8. **Blocks**: `docs/blocks-roadmap.md` - shapes that follow their neighbours, scheduled and
   random ticks, fluids, redstone, the other block entities. And what the first four still
   lack: experience orbs, item physics (`world::collide` is there for it), `entityInside`.
9. **Commands**: an SNBT reader (block entity data, item components, `/summon`'s tag, `nbt=`),
   `facing`, then the commands that wait for their subject - chat first.
10. Lua plugin host.
