---
name: add-packet
description: Implement, change or start handling a Minecraft protocol packet in this server (clientbound or serverbound, any state). Use whenever a task involves a new packet, a wrong/changed packet layout, a client disconnecting with a decode error, or wiring a handler for a packet that is currently ignored.
---

# Adding or changing a packet

A wrong layout does not fail a test here — it disconnects real clients with an opaque error. The steps exist to make
the jar, not memory, the source of every byte.

## Steps

1. **Read the layout from the jar** with the `mc-reference` skill (`mcgen.py codec <class>`, recurse into nested
   codecs). For a clientbound packet also read the client's `handle…` method when a field's meaning or a required
   companion packet is unclear.

2. **Add the struct to `src/proto/packets.hpp`** — layouts only, no logic, no allocation.

   Clientbound (`proto::cb`):
   ```cpp
   struct SetHealth {
       static constexpr u32 kId = ids::play::cb::kSetHealth;   // generated; never a literal
       float health;
       u32 food;
       float saturation;
       usize bound() const { return 4 + 5 + 4; }               // upper bound of encode(), VarInt = 5
       u8* encode(u8* p) const { return put_f32(put_varint(put_f32(p, health), food), saturation); }
   };
   ```
   `bound()` must never be smaller than what `encode()` writes: the writer does no checks after reserving.
   Strings: `str_bound(s)`; a text component that is plain text: `nbt_text_bound(s)`. Any other NBT in a
   packet (a block entity, a styled text) is written with `nbt::Writer` into a `ByteBuf` and read with
   `nbt::Doc` (`src/nbt/nbt.hpp`) - never assembled or picked apart by hand.

   Serverbound (`proto::sb`): a struct with `static T decode(Reader& r)`. Give every string and array a maximum
   (`r.str(max_bytes)`, `r.byte_array(max)`) taken from the vanilla codec's limit — the client is untrusted input.
   Views returned by the `Reader` die with the packet; copy what must outlive the handler.

   Note anything that differs from 1.21.x in a comment on the field, e.g. `// 26.x: VarInt (was a byte)`.

3. **Send or handle it.**
   - Pre-play (handshake/status/login/configuration): `src/server/prelogin.cpp`, on the reactor thread, via
     `r.send(c, pkt)`. Check `rd.ok` and the expected stage before acting; on violation `r.close(c)`.
   - Play: `src/server/game.cpp`, on the tick thread. Inbound: add a `case` in `Game::on_packet`, check `rd.ok`,
     ignore or kick on bad values (never abort). Outbound: `send(pi, pkt)` to one player, `broadcast(pkt)` to all —
     if several players get the same bytes, they must come from one serialization.
   - Never touch `Conn` from the tick thread or game state from the reactor thread.

4. **Unit test the layout** in `tests/unit/wire_test.cpp`: encode with every optional branch exercised and read it
   back field by field with `Reader`, asserting `r.done()` and `encoded size <= bound()`. Follow
   `Packets.LoginPlayLayout`.

5. **Teach the test client** (`tests/integration/mcproto.py` / `run.py`): parse the packet by name and assert on its
   contents inside a scenario. Write the parser from the jar's layout too, not by copying the C++ — the two
   implementations are only useful as a cross-check while they are independent.

6. **Run**
   ```sh
   cmake --build --preset release && ctest --preset release
   cmake --build --preset dev && ASAN_OPTIONS=detect_leaks=1 python3 tests/integration/run.py build/dev/craftc
   ```

7. **Report honestly**: say which class(es) you read the layout from, and that it has not been checked against a
   real client unless the user did so. If the packet is clientbound and new to the join sequence, ask the user to
   connect once with a real 26.3 client.

## Things that have bitten already

- Game modes, enums and registry references are VarInts in 26.x; several were single bytes in 1.21.x.
- Optional values come in three encodings (bool-prefixed, `value + 1`, `id + 1` for holders) — the table in
  `mc-reference` says which codec means which.
- Text components are NBT everywhere except login disconnect and the status response, which are JSON strings.
  One from a client is never passed on as it came: it goes through `text::transcode`, which writes what
  vanilla would have written for it.
- Packet ids ≥ 128 take two bytes; `write_packet` handles it, hand-built bodies must use `put_varint`.
- Entity packets changed shape in 26.x: `move_entity_rot` has on-ground *before* the angles, position deltas
  carry tick-counted steps, velocities are `LpVec3`, `entity_position_sync` carries a `PositionPath`. The byte
  strings in `tests/unit/wire_test.cpp` (`EntityLayoutsAreWhatVanillaSent`) were captured from a vanilla server.
- A packet that goes to everyone who sees an entity is written into the tracker's arena once
  (`entity::Tracker::send_changes`), never per viewer. `bound()` decides the frame header: keep it tight for
  tiny packets (≤ 126 bytes with the id gets a one-byte length).
- With compression on, a handler that both reads an inflated packet and sends large packets is safe only because
  inflate and deflate use separate scratch buffers (`proto/frame.cpp`); do not "simplify" them into one.
- Item stacks have two wire forms. Clientbound: `proto::ItemView` + `put_item` / `item_bound` (the patch bytes
  come ready-made from `item::view(stack)`). Serverbound: never decode one in `packets.hpp` - call
  `item::read_untrusted(reader, stack)`, kick on `kMalformed` and on bytes left over, refuse on `kUnsupported`.
- Vanilla's packet decoder throws on bytes left over behind a packet. New handlers check `rd.done()`, not `rd.ok`,
  where a wrong length would otherwise go unnoticed.
- `entity_event` carries its entity id as a plain int, every other entity packet as a VarInt.
- A handler that kicks must return at once: `kick()` swaps the last player into the freed index, so `p` and `pi`
  now name somebody else.
- Container packets name their menu: id 0 is the player's own, whatever else is open (the creative
  inventory keeps talking to it through `set_creative_mode_slot`); a block's menu gets 1..100 from the
  player's counter. A click for a menu that is not the open one is dropped, not an error.
- Not everything about a menu goes out with its synchronisation: a crafting grid's result is sent the
  moment the grid changes (`Effect::kSetSlot`), once per change - byte for byte what vanilla does, and
  only visible in a recording (`tools/vanilla/container_probe.py`).
- A text component the server makes itself - a system message, a kick's reason - is a `text::Text`
  (`Text::translatable(key).arg(...)`), sent with `cb::SystemChatComponent` / `cb::DisconnectComponent`
  (`Game::system_message`, `Game::kick(pi, Text)`): vanilla's own messages are translatable, with typed
  arguments, and `Text::encode` gives them the bytes vanilla's codec does. `cb::SystemChat{"..."}` is for plain
  words only.
- `player_position` after the join is a teleport like any other: `Game::teleport` sets the flags of
  `net.minecraft.world.entity.Relative` for what the client is to add to its own position, and the client's
  `accept_teleportation` echoes where that put it.
- What others see a player do is mostly not in the packet that says it: a swing comes from `punch` (26.x; no
  payload - the server reads the animation off the held item), from a successful `use_item_on`, from a drop.
  When wiring a handler, read vanilla's whole `handle…` method to its end and port what it broadcasts
  (`Game::swing`, `Game::play_sound`), or write down in the handler what is left out.
