# AGENTS.md

A Minecraft: Java Edition server core written from scratch in C++ (code name **CraftC** — a
placeholder, see "Naming"). The goal is **500 concurrent players on one machine**. Performance is
the product: when speed and elegance disagree, speed wins, and a claim of speed needs a number.

Talk to the user in Russian (their language). Code, comments, commit messages and docs are English.

## Status (keep this section true)

| Area | State |
|---|---|
| Protocol | Minecraft **26.3**, protocol **777** (one version at a time, see `src/proto/gen/current.hpp`). Registries, tags, block states and items generated from the jar |
| Network | Done: io_uring reactor, framing, zlib compression, AES-CFB8, online + offline login, configuration phase, keep-alive. Known limit: encryption runs on the tick thread - 500 players in one place cost it 8-25 ms a tick in online mode |
| World | Flat generator (vanilla default layers), palette sections, chunk wire codec, sky + block light (port of vanilla's engine), chunk frames cached and invalidated on change, chunk view follows the player. One dimension, chunks never unloaded |
| Blocks | Creative only: break, place, block-change broadcast with sequence acks. Placeable: blocks whose vanilla hooks are all implemented (default state, pillar axis, snowy) - the rest stay `kPlaceNone` |
| Entities | One SoA table for everything (`entity::Store`: players are the first rows, slot + generation handles, network id = slot + 1). Tracker (`entity::Tracker`) is a port of vanilla's `ChunkMap.TrackedEntity` + `ServerEntity`: who sees whom, add/remove bundles, movement deltas, rotation, head, velocity, entity data - encoded once per entity, copied per viewer. Its packets were compared with a vanilla 26.3 server's, byte for byte (`tools/vanilla/entity_probe.py`); what still differs is listed in docs/architecture.md |
| Players | Join/leave, tab list. Players see each other: position, rotation, sneaking/sprinting flags, pose, skin layers, main hand, what they hold and wear (`set_equipment`, hand swaps), the arm swinging (`punch`, a placed block, a drop - compared with vanilla by `tools/vanilla/swing_probe.py`). Not yet shown: attributes (`update_attributes`), the jump velocity, footsteps, the locator bar (`waypoint`), what a hit does besides the swing (`attack` is ignored), the stab of a spear, the sounds and particles of placed and broken blocks |
| Items | `item::ItemStack` is 8 bytes: item, count, id of an interned component patch. A patch from a client is read component by component and stored re-encoded; a stack with a component that has no decoder yet is refused (list in `src/item/components.cpp`, reasons in docs/architecture.md). Inventory and the player's own menu: creative slots, every kind of `container_click`, closing, Q / ctrl+Q / throwing out of the window, F, pick block, what the client is known to hold (state ids). Item entities: dropped, merged, picked up, expired - **at rest**: nothing moves entities yet, so a dropped item hangs where it appears (and says `NO_GRAVITY`). Compared with a vanilla 26.3 server (`tools/vanilla/inventory_probe.py`); the differences left are in docs/architecture.md. Not there: recipes (the crafting grid only stores), component hashes of clicks, stacks inside stacks (containers, bundles), the behaviour components (food, tool, equippable...) |
| NBT and text | `nbt::Doc` reads a tag as vanilla's loader does (depth, quota, modified UTF-8, wrapped list elements) into a tape over the bytes - no tree; `nbt::Writer` builds one; `Doc::write` puts a loaded tag back in one canonical form. `text::transcode` is `ComponentSerialization.CODEC` both ways: any spelling of a text component in, the bytes vanilla writes for it out (entry order of its HashMap included) - compared with a vanilla 26.3 server over some 220 spellings (`tools/vanilla/component_probe.py`). On items: custom name, item name, lore, books, sign text; custom data and (block) entity data. Not read (the stack is refused): score / selector / nbt / object contents, `show_item` / `show_entity`, `show_dialog` / `custom`; a patch is still capped at 1 KiB, so a real book does not fit. Nothing *builds* text yet beyond plain strings (`put_nbt_text`): no chat, no renaming |
| Not started | Mobs (the table and tracker take them; nothing spawns or moves them), physics, movement checks, block entities, block updates/neighbour shapes, containers other than the player's own, recipes, survival (everyone is creative), chat, commands, persistence, chunk unloading, workers, Lua plugins |

## Commands

```sh
cmake --preset release && cmake --build --preset release   # -O3, x86-64-v3, LTO  -> build/release/craftc
ctest --preset release                                     # unit tests + integration tests (~70 s)
cmake --preset dev && cmake --build --preset dev           # Debug + ASan/UBSan + DCHECKs
ASAN_OPTIONS=detect_leaks=1 python3 tests/integration/run.py build/dev/craftc   # must be clean before "done"

build/release/unit_tests --gtest_filter='Cfb8*'            # one unit suite
python3 tests/integration/run.py build/release/craftc -k join   # one scenario; --slow adds the timer tests (15 s .. 5 min)
python3 tests/integration/run.py build/release/craftc -k crowd_500 --slow   # 500 bots in sight of each other: tick time, bytes
build/release/bench [filter]                               # micro-benchmarks (release/native builds only); `bench tracker` = entities,
                                                           # `bench item` / `bench menu` / `bench pickup` = items, `bench nbt` / `bench text`
build/release/craftc --online-mode=false --server-port=25565   # run; any server.properties key works as --key=value

python3 tools/mcgen/mcgen.py --help                        # protocol data pipeline
python3 tools/mcgen/mcgen.py data blocks '^minecraft:oak_stairs$'   # a fact that only exists at runtime (shapes, hooks,
                                                           # fields, attributes, enums, menus): see the mc-reference skill
```

Other presets: `native` (`-march=native`, for the production box), `profile` (frame pointers + `-g`, for `perf`).

## Layout

```
src/base/     types.hpp (u8..u64, LIKELY, DCHECK), buf.hpp (ByteBuf, BufPool), spsc.hpp (lock-free queues), log, clock
src/proto/    varint.hpp, wire.hpp (put_* writers, Reader), frame.* (framing + compression),
              packets.hpp (ALL packet layouts), entity_codec.hpp (packed angles, position deltas, LpVec3),
              identifier.hpp ("namespace:path"), static_data.* (pre-baked config packets)
src/proto/gen/  GENERATED by tools/mcgen - never edit by hand; current.hpp selects the active version
src/crypto/   cfb8.* (AES-NI stream cipher, batched), auth.* (RSA key, session hash, UUIDs)
src/net/      reactor.* (io_uring loop, connections, cross-thread queues)
src/nbt/      nbt.* (Doc: a tag read into a tape over its bytes; Tag: a view of one; Writer; Doc::write: canonical form)
src/text/     component.* (text components: ComponentSerialization.CODEC both ways, tag in, vanilla's tag out)
src/item/     stack.* (ItemStack: 8 bytes, vanilla's methods), components.* (the patch store, and the decoder of
              every component a client may send)
src/entity/   store.* (the entity table: SoA columns, slots, handles), tracker.* (who sees whom + the per-tick
              encode-once fan-out, equipment), visibility.hpp (its SIMD kernel), grid.hpp (entities by cell: 64 blocks for
              the tracker, 2 for the game's items)
src/server/   prelogin.* (handshake/status/login/config), session_auth.* (Mojang auth), game.* (tick thread),
              players.cpp (a player's packets and tick as an entity), block_actions.cpp (break/place/pick),
              inventory.* (Inventory + Menu: slots and clicks, no game state), inventory_actions.cpp (its packets,
              drops, equipment), item_entities.cpp (items on the ground), viewers.hpp, settings.*
src/world/    section.* (palette container), chunk*, chunk_codec.* (wire), light.* (light engine),
              dimension.* (chunks + light + frame cache), generator.*, block.* (state/item tables), pool.hpp, bits.hpp
tests/unit/   GoogleTest;  tests/integration/  independent Python client (mcproto.py) + scenarios (run.py)
bench/        micro-benchmarks;  tools/mcgen/  protocol data pipeline;  tools/vanilla/  probes that record what a
              server sends (ours or vanilla) and diff two recordings;  docs/architecture.md  the design in depth
.claude/skills/  task playbooks (below)
```

## Skills

Read the matching `SKILL.md` before starting that kind of task (agents without native skill support: open the file).

| Skill | Use when |
|---|---|
| `.claude/skills/mc-reference` | You need a fact about vanilla: a packet layout, an id, what the client does with a packet |
| `.claude/skills/add-packet` | Implementing or changing any packet |
| `.claude/skills/bump-mc-version` | Moving to a new Minecraft version |
| `.claude/skills/perf` | Optimizing anything, writing SIMD, or claiming something is fast/slow |

## Architecture rules (details and reasons: docs/architecture.md)

Threads: **reactor threads** (`net-N`, one io_uring each) own sockets; the **tick thread** owns all game
state; workers will come later for chunk work. They share nothing mutable.

1. **Threads exchange data only through SPSC rings** (`base/spsc.hpp`). No mutex, no atomics-as-locks,
   no shared containers on any per-packet or per-tick path. Mutexes exist only on cold paths
   (`Reactor::run_on_loop`, the auth worker queue).
2. **One writer per outbound stream.** Before `promote_to_play` the reactor thread writes a connection's
   output; after it only the tick thread does (the cipher state travels in the Join record).
3. **Inbound play packets** are decrypted, split and inflated on the reactor thread, then copied once into
   the reactor's `SpscBytes` ring; the tick thread drains every ring at the start of each tick.
4. **Outbound**: packets are appended to one `ByteBuf` per player per tick; `Game::flush` encrypts all of
   them in one batch (8 streams interleaved) and hands each buffer to the reactor — one send per player
   per tick.
5. **Serialize once.** Anything sent to more than one player is encoded/framed/compressed once and
   `memcpy`'d (see `Game::broadcast`, `Dimension::frame`, the config burst in `static_data`, and the
   entity tracker, where one entity's update is one byte range that every viewer copies).
6. **Connection slots are `(slot, generation)` handles**, never pointers across threads. A slot is reused
   only after the tick thread sends `Release`.
7. **Vanilla on the wire.** The server must be indistinguishable from vanilla to an unmodified client.
   Behaviour that the client can observe is copied from the vanilla jar, not invented.

## Performance rules

- **Hot path = anything per packet, per entity, per block or per tick.** There: no heap allocation, no
  locks, no exceptions, no virtual calls, no `std::function`, `std::shared_ptr`, `std::string` building,
  iostreams, `std::unordered_map`/`std::map`, or logging. Cold paths (login, config load, errors) may
  use whatever is clearest.
- **That list is a default, not a goal.** If a `bench` number (release build, same machine) shows that a
  listed construct is faster for a given case, use it and put the measurement in a comment. Count the cost
  per item, not per call: one indirect call or one amortized buffer growth per batch is fine, one per
  entity or per packet is not. A lookup by key on a hot path wants a flat open-addressing table, not
  `std::unordered_map` (a heap node and a cache miss per element). Exceptions stay out regardless - the
  build has them off.
- **Data layout follows the access pattern.** Data iterated in bulk is SoA: parallel arrays indexed by a
  dense index, removal by swap-with-last (see `entity::Store`). Data touched one object at a time
  (a `Conn` on a completion, the tracker's `Sent` per entity) stays a struct, hot fields first. Do not
  SoA things that are never iterated.
- **Bytes on the wire are the unit of cost.** In online mode every outbound byte is one AES block
  (CFB8), ~3 ns - several times what encoding and copying it cost. A byte saved in a packet that goes to
  N viewers is saved N times: small entity ids, one-byte frame lengths for tiny packets (`frame_begin`).
- **Encoders write through raw pointers after one capacity check**: `u8* p = buf.reserve(bound)` →
  `put_*` → `buf.commit(p)`. Every packet type exposes `bound()`. `ByteBuf` keeps 64 bytes of slack so
  writers may use wide stores.
- **Bit packing**: keep palettes/bitfields in the smallest width that the wire or the cache allows, pack
  with shifts/`pdep`/`pext`, never one value per byte "for simplicity". Wire layouts are fixed by vanilla;
  in-memory layouts are ours to make dense.
- **SIMD**: the baseline is `x86-64-v3` (AVX2, BMI1/2, FMA, LZCNT, MOVBE) plus AES-NI/PCLMUL, so
  intrinsics up to AVX2 need no guard. Wider code goes under `#if defined(__AVX512F__)` /
  `__VAES__` so a `native` build picks it up; always keep the AVX2 path. No runtime dispatch.
  Give SIMD loops a compile-time trip count — a variable count spills the vectors to the stack
  (this cost 35% in `cfb8_encrypt_batch`; see the `perf` skill).
- **No `-ffast-math`**: physics must reproduce vanilla's IEEE results.
- **Built with `-fno-exceptions -fno-rtti`.** Errors are return values or a sticky flag (`proto::Reader::ok`).
  A malformed packet closes the connection; it never throws and never aborts the server.
- **Prove it**: a performance change comes with before/after numbers from `bench` (or `perf`), same
  machine, release build. Add a benchmark when you add a hot path.

## Protocol data — the vanilla jar is the only source of truth

Never write a packet layout, id, or registry value from memory or from a wiki: 26.x differs from 1.21.x in
places nobody documents (examples already hit: `login_finished` gained a session UUID, play `login`
gained `onlineMode` and encodes game modes as VarInts, chunk sections gained a fluid count,
`accept_teleportation` echoes the position, light masks are `BitSet` byte arrays instead of long arrays). Since 26.1 Mojang ships unobfuscated jars, so the answer is
always one command away — use the `mc-reference` skill.

`tools/mcgen/mcgen.py` downloads the official jars (sha1-checked) into `.cache/` and generates
`src/proto/gen/v<protocol>/`. Steps that only read the jar (`fetch`, `find`, `javap`, `codec`, `packets`,
`pseudo`, `data`, `generate`, `check`) are safe to run freely.

**`mcgen.py reports` executes Mojang's data generator (the downloaded server jar, needs Java 25).
Ask the user before running it**, and likewise `blockdata` and `gamedata` — these three are the mcgen
steps that run downloaded code. `reports` produces `registries.json` and `blocks.json`, without which
there are no static-registry ids: no tags, no block ids. Never substitute ids from memory, guesses or
third-party mirrors.

`mcgen.py gamedata` (`tools/mcgen/extract/GameData.java`) loads the game as a server does before it
creates a world - no world, no server, no network - and writes everything else that exists only at
runtime into `.cache/mc/<version>/gamedata/`: every shape and predicate per block state, which class
overrides which hook, what each block/item/entity type was constructed with, default attributes,
synchronized entity data, all static registries, enums, static tables, menu slots. Read it with
`mcgen.py data` before porting a block, an item, a mob or a menu. It is a reference so far: generated
C++ takes nothing from it until `gen.py` is taught to, table by table.

### The vanilla server as a reference

The user accepted Mojang's EULA themselves on 2026-11-12, in `.cache/vanilla-server/eula.txt`. Use the
vanilla server to check behaviour against the real thing: diff its packets against ours with
`mcproto.py`, make reference Anvil worlds, time block entities with `/setblock` and `/data get`,
upgrade old worlds before importing them. Rules:

- Run it only from `.cache/vanilla-server/`, with the cached official jar and Java 25:
  `cd .cache/vanilla-server && /usr/lib/jvm/java-25-openjdk/bin/java -jar ../mc/<version>/server.jar --nogui`.
  The probes in `tools/vanilla/` (`entity_probe.py`, `inventory_probe.py`, `swing_probe.py`,
  `component_probe.py`) record one scripted session on any server; record vanilla and ours, then `diff`. Use player names vanilla has not seen: it saves
  players and what they dropped.
- **Never create, edit or delete `eula.txt`.** If it is missing or not `eula=true`, stop and ask the user.
  Accepting the EULA is the user's act, not an agent's.
- `server.properties` there must keep `server-ip=127.0.0.1` and `online-mode=false`, on a port other
  than our own server's. It is never exposed beyond localhost.
- Stop it when done (`stop` on stdin, or SIGTERM). Do not leave it running between tasks.
- What it shows is ground truth for behaviour; the jar is still the source for layouts and ids.

## Lua plugins (planned — design for it now)

Plugins will run on **LuaJIT**, on the tick thread only. Constraints this puts on today's code:

- Game state that plugins will read must be reachable as plain C data: POD structs and SoA arrays behind
  an `extern "C"` API, so LuaJIT FFI can read a column directly instead of making a C call per entity
  (`entity::Columns` is that view of the entity table).
- Entities, players and connections are referred to by generation-checked integer handles, never
  pointers that could dangle inside a script (`entity::Handle`: slot + generation in 53 bits, so it
  survives being a Lua number).
- Hooks must cost nothing when unused: one bit test per event type before any dispatch.
- Never call into Lua from a reactor or worker thread, and never let a Lua error unwind through C++
  frames (we compile without exceptions): every entry goes through `lua_pcall`.

Java plugin (Bukkit/Paper) compatibility is explicitly out of scope for now.

## Testing rules

- Every packet layout gets a unit test that decodes field by field (`tests/unit/wire_test.cpp` pattern) and,
  if it is part of a flow, an assertion in `tests/integration/run.py`.
- `tests/integration/mcproto.py` is an independent implementation (own framing, zlib, OpenSSL via
  ctypes). Keep it independent: never import server code or copy constants other than the generated ids.
  Its `Client` keeps a model of the entities it was shown (`c.entities`) and of its own menu (`c.slots`,
  `c.cursor`), written from the vanilla client's handlers; scenarios assert on that model, and `settle()`
  waits whole server ticks. `server_menu()` in run.py makes the server send its whole menu (a click with
  a stale state id): the way to see what the server holds rather than what the client believes.
- NBT in tests is written out by hand, never by the code under test: `tests/unit/nbt_bytes.hpp` (C++) builds
  tags from the layouts in the jar, `mc.nbt()` / `Reader.nbt()` in mcproto.py are the test client's own
  reader and writer (`mc.snbt()` prints a value; a dict keeps the wire's entry order, so `mc.nbt(read) ==
  mc.nbt(expected)` compares bytes). What a *codec* makes of a tag is not a layout and cannot be checked by
  reading alone: extend `tools/vanilla/component_probe.py` and compare with the vanilla server.
- The integration tests prove framing, crypto, compression and sequencing. They do **not** prove that a
  layout matches the vanilla client — that comes from reading the jar. Say "verified against the jar"
  or "not verified" in your summary; a real-client check is the user's to run.
- Before calling work done: `ctest --preset release` passes **and** the integration suite is clean under
  the `dev` preset (ASan/UBSan/LSan). The server must exit with code 0 on SIGTERM.

## Conventions

- **Naming**: the code name appears only in `CMakeLists.txt` (`project`, `MC_EXE`) and docs. Namespaces are
  layers (`base`, `proto`, `nbt`, `text`, `crypto`, `net`, `item`, `entity`, `world`, `server`), macros are unprefixed or `MC_`/`PROTO_`. Do not
  introduce the code name into identifiers.
- Types `snake_case` for functions/variables, `PascalCase` for types, `kConstant`, members end in `_`.
  Fixed-width ints are `u8…u64`, `i8…i64`, `usize`.
- Comments explain *why* (the constraint, the vanilla behaviour being matched, the measured reason for an
  optimization). No comments restating code.
- **Item stacks**: pass them by value (`item::ItemStack`, 8 bytes); a copy of one with components takes a
  reference on its patch, so move where a copy is not meant. Nothing from a client becomes a stack except
  through `item::read_untrusted`. A new component gets a decoder in `src/item/components.cpp` written from
  both of its vanilla codecs (StreamCodec = layout, Codec = ranges) and a test with hand-written bytes.
- **NBT**: read with `nbt::Doc` (a tape over the bytes; a `nbt::Tag` is a view that lives as long as the
  Doc and the bytes), write with `nbt::Writer`; no trees of heap nodes, no tags assembled by hand. A tag a
  client sent is stored only as `Doc::write` puts it out (one form per value), and a text component only
  through `text::transcode`. Anything that is read by a vanilla `Codec` rather than a `StreamCodec` gets
  its cases into `component_probe.py` before it is called done.
- `clang-format -i` the files you touch. Generated files are never formatted or edited.
- Dependencies are system packages found by CMake: liburing, libdeflate, OpenSSL (libcrypto), libcurl,
  yyjson, GoogleTest (tests only), LuaJIT (later). Ask before adding one.
- Linux x86-64 only. Kernel ≥ 6.0 (io_uring provided-buffer rings); developed on 7.0 with GCC 16 / Clang 22.
