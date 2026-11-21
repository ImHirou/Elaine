---
name: bump-mc-version
description: Move the server to a new Minecraft release or snapshot (new protocol number, new packet ids, changed packet layouts, new or changed synchronized registries). Use when the user asks to support/update to a version, when a client reports "Outdated server", or when checking what changed between two versions' network protocol.
---

# Bumping the Minecraft version

The server speaks exactly one protocol version. A bump regenerates the data, then fixes by hand only the layouts
that changed. Budget: ids and registries are automatic; expect a handful of layout changes per release.

Let `OLD` be the current version (see `src/proto/gen/current.hpp` and `meta.hpp`) and `NEW` the target.

## 1. Fetch and extract (no code from the jar is executed)

```sh
python3 tools/mcgen/mcgen.py --version NEW fetch --client
python3 tools/mcgen/mcgen.py --version NEW packets
```
`fetch` prints the new protocol number and required Java version. It refuses versions older than 26.1 (obfuscated).

## 2. Reports — needs the user

`python3 tools/mcgen/mcgen.py --version NEW reports` runs Mojang's data generator (executes the downloaded
server jar, Java version from step 1). **Ask the user to run it or to approve it.** Without it `generate` still
works but emits no tags and no block ids, and vanilla clients cannot join. When reports exist, `packets` cross-checks
its static extraction against Mojang's `packets.json` and exits non-zero on any difference — if that happens, fix
`static_packets()` in `mcgen.py` before going on.

Under the same approval, run `mcgen.py --version NEW gamedata` (it executes the jar too). It fails with a list of
problems when a field or class it names is gone - fix the extractor (`tools/mcgen/extract/`), do not skip the line.
Its files keep one record per line, so what the version changed outside the protocol is a diff away:
`diff .cache/mc/{OLD,NEW}/gamedata/blocks.json` (new blocks, changed hooks and properties), likewise `entities.json`
(synchronized data ids move when a class gains a field), `enums.json`, `registries.json`.

## 3. Generate and switch

```sh
python3 tools/mcgen/mcgen.py --version NEW generate      # writes src/proto/gen/v<NEWPROTO>/
```
Then replace the old directory name everywhere in `src/proto/gen/current.hpp` (it is the only source file that
names one) and update `DEFAULT_VERSION` in `tools/mcgen/mcgen.py`. Keep the old `v<OLDPROTO>/` directory until the
bump is finished (it is the diff base), then delete it.

## 4. Find what changed

```sh
diff <(grep -h constexpr src/proto/gen/vOLD/packet_ids.hpp) <(grep -h constexpr src/proto/gen/vNEW/packet_ids.hpp)
diff src/proto/gen/vOLD/registries.hpp src/proto/gen/vNEW/registries.hpp | grep '^[<>] namespace'
```
- **Renamed/removed packets** show up as compile errors on `ids::…::kName` — good, follow them.
- **Shifted ids** need nothing: code uses the generated constants.
- **Layout changes are silent.** For *every* packet struct in `src/proto/packets.hpp`, compare the codec between
  versions:
  ```sh
  for v in OLD NEW; do python3 tools/mcgen/mcgen.py --version $v codec <class> > /tmp/$v.txt; done; diff /tmp/OLD.txt /tmp/NEW.txt
  ```
  Do the same for the nested types each packet uses and for `LevelChunkSection`, `PalettedContainer`,
  `ClientboundLevelChunkPacketData`, `ClientboundLightUpdatePacketData`, `ByteBufCodecs` (new or changed
  primitives). Update `packets.hpp`, its unit tests and `tests/integration/mcproto.py` together.
- **Flow changes**: diff the bytecode of `ServerLoginPacketListenerImpl`, `ServerConfigurationPacketListenerImpl`
  and `PlayerList.placeNewPlayer` (`mcgen.py javap … --method …`) for new mandatory packets or reordered steps.
- **New synchronized registries** are picked up by `generate` automatically (it reads
  `RegistryDataLoader.SYNCHRONIZED_REGISTRIES`). `generate` warns about any that came out empty: check whether the
  registry really has no vanilla entries or its data directory is not named after its key
  (`unzip -l .cache/mc/NEW/server-inner.jar | grep 'data/minecraft/'`).

## 5. Verify

```sh
python3 tools/mcgen/mcgen.py check
cmake --build --preset release && ctest --preset release
cmake --build --preset dev && ASAN_OPTIONS=detect_leaks=1 python3 tests/integration/run.py build/dev/elaine
```
The integration client reads ids and the protocol number from the generated headers, so it follows the bump; its
parsers do not — a layout you changed in C++ must be changed there too.

Finally update the Status table in `AGENTS.md`, and tell the user which layouts changed and that a real-client join
test on NEW is still theirs to do.
