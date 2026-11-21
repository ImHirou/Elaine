# Contributing to Elaine

Thank you for wanting to. This file says how to build, what "done" means here, and what a pull
request needs. The rules of the code itself are in [AGENTS.md](AGENTS.md): it is written for coding
agents, and it binds everyone - read it before a change of any size.

## Two rules that most projects do not have

1. **Vanilla is the specification.** Elaine must be indistinguishable from the vanilla server to
   an unmodified client. A packet layout, an id, the order in which neighbours are updated, the
   exact double a falling item has after three ticks - none of it is written from memory, from a
   wiki, or from what seems reasonable. It is read from the official jar (`tools/mcgen`, the
   `mc-reference` playbook in `.claude/skills/`) and, where it is behaviour, compared with a
   running vanilla server (`tools/vanilla/`).
2. **Performance is the product, and a claim of speed needs a number.** The hot paths - anything
   per packet, per entity, per block or per tick - follow the rules in AGENTS.md ("Performance
   rules"). "This is faster" comes with before and after from `build/release/bench`, release
   build, same machine.

A change that is elegant and breaks either of them will be asked to change.

## Where to start

Good first contributions are narrow and have vanilla as their judge:

- **a block class** from [docs/blocks-roadmap.md](docs/blocks-roadmap.md) - the entry lists the
  hooks to port, AGENTS.md ("Architecture rules", 9) the steps;
- **a command** - one syntax line per way of writing it, a handler, its cases in
  `tools/vanilla/command_probe.py`;
- **an item component's decoder** in `src/item/components.cpp`;
- **a difference from vanilla** listed as known and left in
  [docs/architecture.md](docs/architecture.md).

For anything larger - a new system, a change of an architecture rule, a dependency - open an issue
first and say what you intend. It is cheaper than a pull request that goes the wrong way.

## Building

```sh
./install.sh                 # once: packages, Mojang's jar, the generated tables, a release build
```

The protocol tables (`src/proto/gen/v<protocol>/`) are generated from the Minecraft server jar on
your machine and are not in git; their lock (`src/proto/gen/v<protocol>.sha256`) is. See
[src/proto/gen/README.md](src/proto/gen/README.md). Two things follow:

- A build that stops with *"is not what v777.sha256 says"* after a pull means the generator
  changed: `python3 tools/mcgen/mcgen.py setup`.
- If your change touches the generator (`tools/mcgen/`, including `PORTED_BLOCKS`), run
  `python3 tools/mcgen/mcgen.py generate` and commit the lock it rewrites together with the change.

Day to day:

```sh
cmake --build --preset release --target server_exe      # only what you are about to run
build/release/unit_tests --gtest_filter='Cfb8*'         # one unit suite
python3 tests/integration/run.py build/release/elaine -k join    # one scenario
```

The full list of commands is in AGENTS.md ("Commands").

## Done means

- `ctest --preset release` passes: the unit tests and the integration scenarios (about four minutes).
- The integration suite is clean under sanitizers:
  ```sh
  cmake --preset dev && cmake --build --preset dev --target server_exe
  ASAN_OPTIONS=detect_leaks=1 python3 tests/integration/run.py build/dev/elaine
  ```
- New behaviour has its test: a packet layout a unit test that decodes it field by field, a flow an
  integration scenario, physics a recording (AGENTS.md, "Testing rules" and "Conventions").
- What a client can observe was compared with vanilla, and the pull request says how: *verified
  against the jar*, *compared with a vanilla server (probe, result)*, or *not verified*. All three
  are acceptable answers; a missing one is not.
- `clang-format -i` on the C++ files you touched. Nothing under `src/proto/gen/` is formatted or
  edited by hand.
- Every new source file begins with `// SPDX-License-Identifier: GPL-3.0-or-later` (`#` in Python,
  shell and CMake).
- The Status table in AGENTS.md and the goals in README.md still tell the truth.

`tools/ci/checks.sh` runs the cheap half of this list (headers, formatting, the roadmap); CI runs
all of it.

## Mojang's jar and the vanilla server

- `tools/mcgen` downloads the official server jar from Mojang (sha1-checked) into `.cache/`. Three
  of its steps - `reports`, `blockdata`, `gamedata` - and `setup`, which runs them, execute code
  from that jar. No server is started by them.
- The probes in `tools/vanilla/` do start a vanilla server, from `.cache/vanilla-server/`, bound to
  localhost. That needs Mojang's EULA accepted in `eula.txt` there. **Accepting it is yours to do**,
  by hand; no script of this repository and no agent working in it writes that file.
- **Never commit** a jar, anything else from `.cache/`, anything under `src/proto/gen/v*/`, assets
  or texts of the game, or an `eula.txt`. `.gitignore` covers the usual paths; it does not think for you.

## Where behaviour comes from

From the jar and from the vanilla server - nowhere else. In particular, **do not copy code from
other server implementations** (Paper, Spigot, Minestom, Pumpkin, Valence, Cuberite, ...): their
licences are not all compatible with this one, and they are not the specification either - what
they do differently from vanilla you would be copying too.

## Pull requests

- One change per pull request; commits that each build and pass.
- Commit messages: an imperative subject that says what the commit does, a body that says why
  where it is not obvious. English, like the code and its comments.
- The description says what was verified against vanilla and how (see "Done means"), and for a
  performance change gives the numbers.

## Sign your work

Elaine takes contributions under the [Developer Certificate of Origin](https://developercertificate.org/),
version 1.1. Add a `Signed-off-by` line with your name and an address you can be reached at to
every commit - `git commit -s` does it - and you certify the following:

```
Developer's Certificate of Origin 1.1

By making a contribution to this project, I certify that:

(a) The contribution was created in whole or in part by me and I
    have the right to submit it under the open source license
    indicated in the file; or

(b) The contribution is based upon previous work that, to the best
    of my knowledge, is covered under an appropriate open source
    license and I have the right under that license to submit that
    work with modifications, whether created in whole or in part
    by me, under the same open source license (unless I am
    permitted to submit under a different license), as indicated
    in the file; or

(c) The contribution was provided directly to me by some other
    person who certified (a), (b) or (c) and I have not modified
    it.

(d) I understand and agree that this project and the contribution
    are public and that a record of the contribution (including all
    personal information I submit with it, including my sign-off) is
    maintained indefinitely and may be redistributed consistent with
    this project or the open source license(s) involved.
```

Your contribution is licensed as the project is: GPL-3.0-or-later, with the additional permission
of [PLUGIN-EXCEPTION](PLUGIN-EXCEPTION). You keep your copyright; there is no CLA.

## Working with AI tools

Much of Elaine is written with coding agents, and you may use them too - `AGENTS.md` and
`.claude/skills/` are there for exactly that. The conditions are the ones above, with nothing
taken off:

- You sign off, so you answer for every line: that you may contribute it, and that it is right.
- "The agent said it matches vanilla" is *not verified*. The jar and the probes decide.
- Run the tests yourself, and say in the pull request which agent you worked with.

## Security

Do not open a public issue for a vulnerability: see [SECURITY.md](SECURITY.md).
