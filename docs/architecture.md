# Architecture

What exists today is the network stack, a flat world that can be built in, the entity layer
that lets players see each other, and items: stacks, the player's inventory, what is held and
worn, what lies on the ground. This document explains how the pieces fit and why; `AGENTS.md`
has the rules in short form.

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
drain packets ─▶ block + light changes ─▶ entity tracker ─▶ entity ticks ─▶ per-player tick ─▶ keep-alives ─▶ flush
(move, click…)   (ChunkHolder             (ChunkMap.tick)   (items; the     (ack, equipment,
                  .broadcastChanges)                         players' menus)  head, pickup, pose)
```

The order is observable, so it is copied: a block change reaches a client before the entity
movement of the same tick; the tracker runs before a player's own tick, which is why other players
see a head turn one tick after the body and a new pose one tick after the key that caused it; a
menu is brought up to date (`ServerPlayer.tick`) before the player's connection ticks, so a slot
changed by a pickup is sent with the pickup and one changed by a key (F) a tick later.

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
  (`waypoint`): not sent. Nor are footsteps (`sound`, to those who hear a player walk) and
  entity event 24 at join (the permission level).
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
what it cannot vouch for. Read today: stack size, damage, max damage, unbreakable, repair cost,
rarity, enchantments and stored enchantments, glint override, potion contents and duration scale,
suspicious stew effects, fireworks and firework explosions, instruments, painting variants, banner
patterns (all three by registry reference only), block state, tooltip display, dyed colour, map
id, item model, tooltip style, note block sound, ominous bottle amplifier, glider, waxed, the dye
and collar colours. That covers what the creative inventory offers except the ominous banner.
Not read: anything holding text (names, lore, books, signs), NBT (custom data, entity data),
stacks within stacks (containers, bundles), and the behaviour components (food, tool, equippable,
attribute modifiers...).

Two knowing differences from vanilla: a patch is not "sanitized" against the item's defaults (a
modified client can make a stone with `max_stack_size=64` that will not merge with plain stone - a
vanilla client never sends that), and a patch is capped at 1 KiB (vanilla's only limit is the
packet; a stack is copied to every viewer of its holder).

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
`container_click` only arrives from test clients. The 2x2 crafting grid stores items; there are no
recipes, so its result slot stays empty.

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
| menu sync of a touched player, nothing to send | 0.16 - 0.2 us |
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
- its crafting grid produces results;
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
7. **Items**: the rest of the components (text and NBT first: names, lore, custom data), the
   component hashes of `container_click`, recipes, then containers and block entities. Survival
   needs none of that first, only a game mode.
8. Lua plugin host.
