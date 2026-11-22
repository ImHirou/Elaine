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
finished - compared with a real vanilla server, byte for byte where that is possible - before the next begins.
## Goals

### Protocol

*An unmodified vanilla client cannot tell Elaine from a vanilla server; one Minecraft version at a
time, following the newest release.*

- [x] Minecraft 26.3 (protocol 777): packet ids, registries, tags and block states generated from the official jar
- [x] Encryption (AES-CFB8)
- [x] Whatever goes to more than one player is encoded
- [ ] Encryption off the tick thread
- [ ] Every play packet handled
- [ ] Proxy forwarding (Velocity), per-connection rate limits, IPv6 by default

### Server

*An operator's server: starts in seconds, stops cleanly, runs for weeks and says what it is doing.*

- [x] The console
- [x] A clean stop on SIGTERM or `/stop`
- [ ] Whitelist and bans in vanilla's files
- [ ] RCON, query, the server icon
- [ ] Log files; metrics - the tick by subsystem, bytes per player

### Config

- [x] `server.properties`
- [ ] Every vanilla key either honoured or reported at startup (today an unknown key is skipped silently)
- [ ] Game rules and `/gamerule`
- [ ] A reference of every key

### World

- [x] Chunks
- [x] Sky and block light
- [x] A region format of its own: checksums, and a chunk is never overwritten in place
- [x] A vanilla (Anvil) world is imported at startup
- [x] The flat generator
- [ ] Terrain generation: vanilla's, the same world from the same seed
- [ ] The Nether 
- [ ] The End
- [x] The time of day
- [x] The tick rate as a setting (what vanilla's `/tick rate` sets); not yet the `/tick` command
- [ ] Weather
- [ ] Export back to Anvil; worlds of older versions

### Blocks

- [x] Breaking and placing;
- [x] Neighbour updates, scheduled ticks and random ticks
- [x] Block entities (WIP)
- [x] The shapes: slabs, stairs, fences, panes, walls, lanterns, chains
- [ ] Doors, trapdoors, fence gates and what else answers a click
- [ ] Plants that grow
- [ ] Fluids
- [ ] Redstone and pistons

Every block, done or to do, class by class: [docs/blocks-roadmap.md](docs/blocks-roadmap.md).

### Tracking

- [x] An entity's update is encoded once
- [x] Chunks follow the view distance
- [ ] Attributes, footsteps, the locator bar
- [ ] Headroom for tens of thousands of mobs in one world

### Entity

- [x] One table for everything that exists
- [x] Physics
- [ ] Attributes, damage, combat
- [ ] AI pathfinding
- [ ] Spawning
- [ ] Boats, minecarts and what else collides

### Player

- [x] Join and leave 
- [x] The tab list; 
- [x] Players see each other move, sneak, sprint, hold, wear and swing
- [x] Creative mode
- [ ] Game modes; survival - health, hunger, death, respawn, experience
- [x] Chat
- [ ] Secure chat
- [x] A player's data kept: position, inventory
- [ ] Movement checks: vanilla's
- [ ] Advancements, statistics, the recipe book

### Items

- [x] Stacks
- [x] Dropping, picking up
- [x] Crafting (1235 of 1266 recipes)
- [ ] The components that behave: food, tools, equipment
- [x] Stacks inside stacks, as data: a shulker, a bundle, a loaded crossbow
- [ ] What a bundle does when it is clicked; the shulker box as a block
- [ ] Enchanting, the anvil, brewing and the other menus

### Commands

*Every vanilla command, read by Brigadier's rules and answered in vanilla's words.*

- [x] Brigadier's tree
- [x] `stop`, `setblock`, `fill`, `teleport`, `give`, `playsound`, `stopsound`, `summon`, `kill`, `list`, `kick`, `op`, `deop`, `help`, `say`, `me`, `msg`, `teammsg`, `time`, `tellraw`, `data get`
- [x] Coordinates, entity selectors, block states and tags, items
- [x] Tags as text (SNBT) with vanilla's errors, NBT paths, `nbt=` in selectors
- [x] Item components in commands: `give @s diamond_sword[damage=3,!lore]`
- [x] A block entity's tag in commands: `setblock ~ ~ ~ chest{Items:[...],CustomName:"Loot"}`; named and locked chests
- [x] `/summon` with a tag: names, tags, `NoAI`, an armor stand's pose and what it wears
- [ ] `data merge` / `modify` / `remove`; babies, small armor stands
- [x] Suggestions a client asks the server for (time markers, timelines)
- [ ] `execute`, `gamemode`, `weather`, `gamerule`, `effect`, `scoreboard` and the rest

### Plugins

- [ ] The LuaJIT host
- [ ] Events
- [ ] A documented plugin interface - plugins that keep to it may carry any licence ([PLUGIN-EXCEPTION](PLUGIN-EXCEPTION))

## Non-goals

- **Bukkit, Spigot or Paper plugins.**
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
