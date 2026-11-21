---
name: perf
description: Optimize code in this server, write or review SIMD/bit-packing/cache-layout code, add a benchmark, profile a slow path, or back up any claim that something is fast or slow. Use for requests like "make X faster", "why is the tick slow", "vectorize this", "is this hot path OK", and before merging changes to anything per-packet, per-entity, per-block or per-tick.
---

# Performance work

The target is 500 players at 20 TPS: a 50 ms tick budget shared by everything. Work is judged by measurements on
a release build, never by how the code looks.

## The loop

1. **Find the cost before touching code.** Use an existing benchmark (`build/release/bench <filter>`) or profile the
   server under load:
   ```sh
   cmake --preset profile && cmake --build --preset profile          # release flags + frame pointers + -g
   # The load test starts its own server, joins 200 players, moves them, and prints the server's CPU share:
   python3 tests/integration/run.py build/profile/elaine -k many_players
   # Same run under perf; the server's threads are named `tick` and `net-N`, which filters out the Python client:
   perf record -g -o /tmp/perf.data -- python3 tests/integration/run.py build/profile/elaine -k many_players
   perf report -i /tmp/perf.data --comm tick,net-0 --percentage relative --no-children --sort symbol
   # A long-running server:  perf record -g -p $(pidof elaine) -- sleep 10
   #                         perf stat -e cycles,instructions,cache-misses,branch-misses -p $(pidof elaine) -- sleep 10
   ```
   The Python client cannot push the server hard (it is the bottleneck); for anything beyond a smoke profile,
   write the load in C++ under `bench/`. For entities both exist: `bench tracker` runs the real tracker for
   500 players (crowd / spread / + 20 000 mobs) and prints ms per tick, bytes, and the same with AES;
   `run.py -k crowd_500 --slow` is the crowd over real sockets, with the tick times from the server's log.
2. **State the hypothesis** in one sentence: what is slow and why (latency chain, cache misses, port contention,
   allocation, syscalls, false sharing).
3. **Change one thing**, rebuild `release` (or `native` for machine-specific numbers), re-run the same benchmark
   at least twice. Benchmarks print best-of-5; ignore differences under ~3%.
4. **Keep or revert on the numbers.** A change that is not measurably better is reverted, however clever.
5. **Report**: before → after, the CPU model, the preset. Put the measured reason in a comment where a future
   reader would otherwise "simplify" the code back.

No benchmark for the path you are changing? Add one to `bench/bench.cpp` first (`bench(name, units_per_call,
unit, lambda)`), using data with realistic sizes and entropy — all-zero or constant input flatters compressors,
branch predictors and palettes.

## Rules of thumb that hold in this codebase

**Layout**
- Iterate columns, not objects: bulk passes read SoA arrays indexed by a dense index (`Game::px_…`). Keep an array
  per field that is read together; pad nothing "for symmetry".
- Fields written by different threads never share a cache line (`CACHE_ALIGNED`, see `base/spsc.hpp`).
- Per-object state touched once per event stays a struct with hot fields first (`net::Conn`).
- Prefer indices/handles (`u32`) to pointers in hot arrays: half the size, and stable across reallocation.

**Hot loops**
- No allocation, locks, virtual calls, `std::function`, logging, or `unordered_map` lookups inside.
- Hoist capacity checks: one `reserve(bound)` then raw pointer writes. `ByteBuf` has 64 bytes of slack so 8/16/32-byte
  stores at the tail are legal.
- Mark the rare side `UNLIKELY`/`COLD` and move it out of line (`NOINLINE`) so the hot path stays in a few cache lines.
- A byte-pointer store (`u8*`) may alias anything: load what you need into locals before the loop, or the compiler
  reloads it every iteration.

**SIMD**
- Baseline is AVX2 + BMI2 + AES-NI (`-march=x86-64-v3 -maes -mpclmul`): use those intrinsics unguarded. AVX-512/VAES
  code goes under `#if defined(__AVX512F__)` / `__VAES__` next to the AVX2 version, which must remain.
- **Give vector loops a compile-time width.** With a runtime lane count the compiler keeps the vectors in memory and
  every round pays a store-forward: `cfb8_encrypt_batch` went from 230 to 358 MB/s (i5-9300H) just by templating the
  kernel on `W` (`run_lanes<8>` in `src/crypto/cfb8.cpp`). Handle remainders with smaller fixed widths.
- Break dependency chains by interleaving independent streams when one stream is latency-bound (same file).
- Know the port limit before chasing it: on a core with one AES unit, 8 lanes cannot beat ~10 cycles/byte for
  10-round AES. `perf stat` IPC and https://uops.info tell you whether you are at the limit.
- `pdep`/`pext` are fast on Intel since Haswell and AMD since Zen 3, microcoded (slow) on Zen 1/2. If Zen 2 ever
  matters, measure the scalar fallbacks in `proto/varint.hpp`.
- Check the generated code when it matters: `objdump -d --no-show-raw-insn build/release/elaine | less` (search the
  symbol), or compile one file with `-S -fverbose-asm`. Look for spills (`vmovdqa … (%rsp)`) inside the loop.

**Bits**
- In memory, store the narrowest representation that the access pattern allows (palette indices, packed flags,
  16-bit local coordinates) and widen in registers. Wire formats are fixed by vanilla; our caches are not.
- Shift/mask or `pdep`/`pext` whole words; never loop bit by bit or keep one flag per byte in a bulk structure.

**Across threads and the kernel**
- One syscall per reactor wake-up, one send per player per tick. If a change adds syscalls per packet, it is wrong.
- Batch cross-thread hand-offs (publish a ring once per batch, wake once per tick).
- Serialize shared data once and `memcpy` it; compress once and cache the compressed frame.

## What not to do

- No `-ffast-math`, no reordering of floating-point physics: results must match vanilla.
- No micro-optimizing cold paths (login, config parsing, error handling): clarity wins there.
- No speculative "this should be faster" commits. No benchmark that measures something the server does not do.
