# Elaine

**A Minecraft: Java Edition server written from scratch in C++, built to hold 500 players on one machine.**

[![CI](https://github.com/ImHirou/elaine/actions/workflows/ci.yml/badge.svg)](https://github.com/ImHirou/elaine/actions/workflows/ci.yml)
[![License: GPL-3.0-or-later](https://img.shields.io/badge/license-GPL--3.0--or--later-blue.svg)](LICENSE)

> NOT AN OFFICIAL MINECRAFT PRODUCT. NOT APPROVED BY OR ASSOCIATED WITH MOJANG OR MICROSOFT.

Elaine is not a fork of the vanilla server and not a wrapper around it. It is a new server that an
unmodified Minecraft client cannot tell from the vanilla one - and that is written so that 500 
players in one place are an ordinary evening.

## Status

**Early.** Elaine speaks Minecraft **26.3** and is being built system by system; each one is
finished - compared with a real vanilla server, byte for byte where that is possible - before the
next begins. Today a vanilla client can join, build in creative mode in a world that is kept on disk
or imported from vanilla, open chests, smelt, craft, light TNT, watch sand fall and the sun set, talk
to whoever else is there - and find itself where it was, with what it had, when it comes back. There is no
survival mode, no mob AI and no terrain generator yet: **it is not ready to host players.**

What exists is listed under [Goals](#goals); how it works and why, in
[docs/architecture.md](docs/architecture.md).

## Principles

- **Vanilla is the specification.** Nothing a client can observe is invented or taken from a wiki:
  layouts and ids are read from the official jar, behaviour is ported from it, and the result is
  recorded next to a running vanilla server and compared (`tools/vanilla/`). Entity physics gives
  vanilla's doubles bit for bit; menus and command answers match byte for byte.
- **Performance is the product.** When speed and elegance disagree, speed wins - and a claim of
  speed comes with a number, measured on a release build.
- **One Minecraft version at a time**, the newest. Other client versions are a proxy's job.
- **Linux and io_uring**, x86-64 with AVX2 and AES-NI. Choosing one platform is what lets the
  network path and the hot loops be written for it.
- **A feature is done or absent.** A block can be placed only when everything vanilla does with it
  is ported; a command that cannot do something yet says so instead of half doing it.

## Goals

The big aim of each area, what of it is there (checked), and what comes next. The finer grain -
every difference from vanilla that is known and left - is in
[docs/architecture.md](docs/architecture.md).

### Protocol

*An unmodified vanilla client cannot tell Elaine from a vanilla server; one Minecraft version at a
time, following the newest release.*

- [x] Minecraft 26.3 (protocol 777): packet ids, registries, tags and block states generated from the official jar
- [x] io_uring reactors, framing, zlib compression, AES-CFB8 encryption, online and offline login, the configuration phase, keep-alive
- [x] Whatever goes to more than one player is encoded, framed and compressed once
- [ ] Encryption off the tick thread - today 500 players in one place cost it 8-25 ms a tick in online mode
- [ ] Every play packet handled
- [ ] Proxy forwarding (Velocity), per-connection rate limits, IPv6 by default

### Server

*An operator's server: starts in seconds, stops cleanly, runs for weeks and says what it is doing.*

- [x] The console on stdin, operators in vanilla's `ops.json`
- [x] A clean stop on SIGTERM or `/stop`: the world is saved, the exit code is 0
- [ ] Whitelist and bans in vanilla's files
- [ ] RCON, query, the server icon
- [ ] Log files; metrics - the tick by subsystem, bytes per player

### Config

*A vanilla `server.properties` works as it is; whatever Elaine adds is documented, checked and
defaults to what suits a big server.*

- [x] `server.properties` is read; every key also works on the command line as `--key=value`
- [x] Elaine's own keys for what vanilla does not have: reactor and worker threads, when chunks are unloaded and saved
- [ ] Every vanilla key either honoured or reported at startup (today an unknown key is skipped silently)
- [ ] Game rules and `/gamerule` (`random-tick-speed` and `advance-time` are settings until then)
- [ ] A reference of every key

### World

*Vanilla's worlds - generated, lit, kept and sent - with all the waiting moved off the tick thread.*

- [x] Chunks are read, generated, lit, sent at the client's pace, saved and unloaded around the players - by worker threads
- [x] Sky and block light: a port of vanilla's engine
- [x] A region format of its own: checksums, and a chunk is never overwritten in place
- [x] A vanilla (Anvil) world is imported at startup; what Elaine does not understand yet is kept, not dropped
- [x] The flat generator
- [ ] Terrain generation: vanilla's, the same world from the same seed
- [ ] The Nether and the End (one dimension today)
- [x] The time of day: 26.x's world clocks, told to clients, kept, imported; the sky darkens for what grows
- [ ] Weather
- [ ] Export back to Anvil; worlds of older versions

### Blocks

*All 1286 blocks, each doing everything it does in vanilla.*

- [x] Breaking and placing; 716 blocks can be placed
- [x] Neighbour updates, scheduled ticks and random ticks, in vanilla's order
- [x] Chest, furnace, crafting table, TNT and explosions
- [x] Barrel, smoker, blast furnace, and the chests of copper that weather
- [x] What falls (sand, gravel, concrete powder), torches, the redstone lamp, amethyst, grass and mycelium, copper that weathers
- [x] The shapes: slabs, stairs, fences, panes, walls, lanterns, chains - placed as vanilla places them, joined to what stands beside them
- [ ] Doors, trapdoors, fence gates and what else answers a click
- [x] Plants that only stand: flowers, grass, bushes, petals and leaf litter by the four
- [ ] Plants that grow, that are two blocks tall or stand in a pot; candles; everything else that needs something to stand on
- [ ] Fluids
- [ ] Redstone and pistons
- [ ] The other block entities

Every block, done or to do, class by class: [docs/blocks-roadmap.md](docs/blocks-roadmap.md).

### Tracking

*Every player sees what vanilla would show them, and telling N players costs little more than
telling one.*

- [x] Who sees whom: a port of vanilla's entity tracker - add and remove, movement deltas, rotation, velocity, entity data
- [x] An entity's update is encoded once and copied to every viewer
- [x] Chunks follow the view distance, in vanilla's batches
- [ ] What is not shown yet: attributes, footsteps, the locator bar
- [ ] Headroom for tens of thousands of mobs in one world

### Entity

*Every mob as vanilla runs it - bit for bit where it can be checked - at a fraction of vanilla's
cost per entity.*

- [x] One table for everything that exists: structure of arrays, generation-checked handles
- [x] Physics: `Entity.move`, collisions with every block shape, items, primed TNT, falling blocks, 61 kinds of mob - vanilla's results bit for bit, replayed from a recording of a vanilla server
- [x] What lies still costs next to nothing
- [ ] Attributes, damage, combat
- [ ] A mob's brain: goals, pathfinding
- [ ] Spawning
- [ ] Boats, minecarts and what else collides

### Player

*Survival as vanilla plays it, for 500 players at once.*

- [x] Join and leave, the tab list; players see each other move, sneak, sprint, hold, wear and swing
- [x] Creative mode
- [ ] Game modes; survival - health, hunger, death, respawn, experience
- [x] Chat: what players type, `say`, `me`, `msg`, who came and who went - unsigned, as vanilla sends it without a chat session
- [ ] Chat signed as vanilla signs it (secure chat)
- [x] A player's data kept: position, inventory
- [ ] Movement checks: vanilla's
- [ ] Advancements, statistics, the recipe book

### Items

*Every item, component, recipe and menu.*

- [x] Stacks of eight bytes with interned components; nothing a client sends is stored unread
- [x] The inventory and every kind of click; dropping, picking up, items on the ground
- [x] Crafting (1235 of 1266 recipes), smelting, fuel
- [ ] The components that behave: food, tools, equipment
- [ ] Stacks inside stacks: bundles, shulker boxes
- [ ] Enchanting, the anvil, brewing and the other menus

### Commands

*Every vanilla command, read by Brigadier's rules and answered in vanilla's words.*

- [x] Brigadier's tree, its parsing and its errors; answers in vanilla's text components
- [x] `stop`, `setblock`, `fill`, `teleport`, `give`, `playsound`, `stopsound`, `summon`, `kill`, `list`, `kick`, `op`, `deop`, `help`, `say`, `me`, `msg`, `teammsg`, `time`, `tellraw`, `data get`
- [x] Coordinates, entity selectors, block states and tags, items
- [x] Tags as text (SNBT) with vanilla's errors, NBT paths, `nbt=` in selectors
- [x] Item components in commands: `give @s diamond_sword[damage=3,!lore]`
- [ ] Tags into the world: block entity data, `/summon`'s tag; `data merge` / `modify` / `remove`
- [x] Suggestions a client asks the server for (time markers, timelines)
- [ ] `execute`, `gamemode`, `weather`, `gamerule`, `effect`, `scoreboard` and the rest

### Plugins

*Lua plugins on LuaJIT that cost nothing when unused and read the game's state without a call per
entity.*

- [x] The ground for it: game state as plain C data, integer handles, commands registered as a line and a function
- [ ] The LuaJIT host
- [ ] Events
- [ ] A documented plugin interface - plugins that keep to it may carry any licence ([PLUGIN-EXCEPTION](PLUGIN-EXCEPTION))

### Performance

*500 players on one machine at 20 ticks a second - and a number behind every claim.*

- [x] Micro-benchmarks for the hot paths (`build/release/bench`)
- [x] A test that joins 500 bots within sight of each other (`run.py -k crowd_500 --slow`)
- [ ] The same measurements with a full game running: mobs that think, fluids, redstone

Where it stands, on a four-core laptop (i5-9300H) with those 500 bots on the same machine: a tick
takes 1.0 ms on average in offline mode, 17-26 ms in online mode. The difference is AES - a crowd
like that is 50 MB/s of ciphertext, and it is still encrypted on the tick thread. The numbers and
how they were taken are in [docs/architecture.md](docs/architecture.md); they describe a server
that does not run a whole game yet.

## Non-goals

- **Bukkit, Spigot or Paper plugins.** Elaine is not a JVM and will not pretend to be one.
- **Several client versions from one process.** That is what a translating proxy is for.
- **Bedrock Edition.**
- **Client-side mod loaders** (Forge, Fabric, NeoForge mods).
- **Changing the game.** Elaine's own behaviour is vanilla's; what a server wants to be different is a plugin's to make so.
- **Windows, macOS, ARM** - for now.

## Getting started

You need Linux (kernel 6.0 or newer) on x86-64 with AVX2 and AES-NI - any Intel since Haswell, any
AMD since Zen - and a compiler that knows `#embed`: GCC 15 or newer, or a recent Clang (Elaine is
developed with GCC 16 and built with Clang 23 as well).

```sh
git clone https://github.com/ImHirou/elaine
cd elaine
./install.sh
```

`install.sh` asks once, then installs the packages a build needs, downloads the Minecraft server
jar from Mojang, generates the protocol tables from it and builds. The tables are Mojang's data, so
they are not in this repository: every clone makes its own. By running it you download Mojang's
software under [Mojang's terms](https://aka.ms/MinecraftEULA).

Where it was run from a bare system image to a server that passes all its tests (November 2026):

| Distribution | Compiler | |
|---|---|---|
| Arch Linux | GCC 16.2, Clang 23 | works |
| Fedora 45 | GCC 16.2 | works |
| Ubuntu 26.04 LTS | GCC 15.2 | works |
| Debian 13 | GCC 14.2 | stops: the compiler has no `#embed`. With a newer one in `CXX`, `./install.sh` goes on |

On a distribution it does not know, `install.sh` lists what to install and carries on from there.

```sh
build/release/elaine --online-mode=false    # a local test server: join localhost with a Minecraft 26.3 client
ctest --preset release                      # unit tests, and scenarios played by an independent protocol client
```

Any `server.properties` key works as `--key=value`. The world is kept in `./world`; a vanilla
world put there is imported on the first start. The server's stdin is its console: `op <name>`
makes the first operator.

## Documentation

- [docs/architecture.md](docs/architecture.md) - the design in depth, the measurements, and every known difference from vanilla
- [docs/blocks-roadmap.md](docs/blocks-roadmap.md) - all 1286 blocks, done or to do
- [AGENTS.md](AGENTS.md) - the rules of the code base, written for coding agents and binding for everyone
- [CONTRIBUTING.md](CONTRIBUTING.md) - how to build, test and send a change
- [SECURITY.md](SECURITY.md) - how to report a vulnerability

## Contributing

Contributions are welcome - a block class from the roadmap, a command, a component decoder are
good first ones. Read [CONTRIBUTING.md](CONTRIBUTING.md) first: this project has two rules that
most do not, *vanilla is the specification* and *performance needs a number*.

## The name

Elaine of Astolat is Tennyson's Lady of Shalott, who sees the world only in a mirror and weaves
what it shows. This server looks at vanilla the same way.

## License

Copyright © 2026 the Elaine contributors.

Elaine is free software: you can redistribute it and/or modify it under the terms of the
[GNU General Public License](LICENSE) as published by the Free Software Foundation, either
version 3 of the License, or (at your option) any later version. It is distributed in the hope
that it will be useful, but WITHOUT ANY WARRANTY.

As an additional permission, plugins that use only Elaine's plugin interface may be distributed
under terms of their author's choice: see [PLUGIN-EXCEPTION](PLUGIN-EXCEPTION).

This repository holds none of Minecraft's code, data or assets. Elaine's behaviour is a port:
C++ written to do what the official server does, from reading its jar and from watching it run.
The tables Elaine needs about one version of the game are generated on your machine from the
official jar, which you download from Mojang under Mojang's own terms; the licence above covers
Elaine and not them. "Minecraft" is a trademark of Mojang Synergies AB.
