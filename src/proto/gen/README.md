# src/proto/gen

What the server knows about one Minecraft version: packet ids, registries, tags, block states,
items, recipes, collision shapes, the English of the game's messages.

| | In git | What it is |
|---|---|---|
| `current.hpp` | yes | names the version this build speaks; the only file that names a versioned directory |
| `v<protocol>.sha256` | yes | the lock: a SHA-256 of every generated file, in `sha256sum`'s format |
| `v<protocol>/` | **no** | the generated tables themselves |

The tables are not in the repository because they are not ours. `tools/mcgen` derives them from
the official Minecraft server jar, and what they hold - names, numbers, texts - is Mojang's. Every
clone makes its own copy from a jar it downloads from Mojang itself:

```sh
./install.sh                          # everything a fresh clone needs, once
python3 tools/mcgen/mcgen.py setup    # only this part: fetch the jar, extract, generate, compare with the lock
```

The lock is what keeps everybody on the same bytes. The build compares the directory with it
before compiling anything (`mcgen.py check`), so a pull that moved the lock stops the next build
with one line that says what to run. `mcgen.py generate` rewrites the lock: a change of the
generator shows up as a diff of it, and is committed together with what caused it.

Nothing here is edited or formatted by hand. Elaine's licence covers the generator and the lock,
not the data the generator reads.
