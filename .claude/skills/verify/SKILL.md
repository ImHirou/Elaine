---
name: verify
description: Build and test a change in this server at the least cost in time and output - which targets to build, which tests to run while iterating, what to rerun after a failure, and the full gate before work is called done. Use before the first build or test run of any task, when a test fails, and before saying "done".
---

# Building and testing a change

A full check is minutes of wall time and, read line by line, most of a session's output. The work is to run the
smallest thing that can prove the edit wrong, read only what failed, and run everything once.

`tools/check.py` is the way in: it builds what the tests it was asked for need, runs them, and prints a line a step -
of a step that failed, only the compiler's errors or the scenarios that failed with their tracebacks and server logs.
Run it in the background and wait for it; do not start a second build or test run beside it (they share the cores:
timings go wrong, scenarios fail that would not have).

## The ladder

1. **While iterating: only what the edit can reach.**
   ```sh
   tools/check.py --unit 'Light*:Collide*'     # builds unit_tests, runs these suites (a gtest filter)
   tools/check.py -k chest -k furnace          # builds server_exe, runs the scenarios with these in their names
   tools/check.py -k chest --unit 'Container*' # both
   ```
   Pick by what was touched: a codec or a pure function has a unit suite (`tests/unit/<area>_test.cpp`); behaviour
   a client sees has scenarios (`grep -n '^def ' tests/integration/run.py` lists them - their names say what they
   cover). An edit to a comment, a doc or a skill needs no run at all.
2. **After a failure: only what failed.** `tools/check.py --failed` rebuilds and runs the scenarios that failed the
   last time (kept in `build/<preset>/integration-failed.txt`; a scenario leaves the list when it passes). Not the
   suite again - that is step 3, once.
3. **Before "done": everything, once.** `tools/check.py full` - release build, unit tests, all scenarios, dev build
   (ASan/UBSan/LSan), all scenarios again. That is the gate of AGENTS.md ("Testing rules"); `ctest --preset release`
   is the same first half. If it fails: fix, `tools/check.py --failed` (add `--dev` if it was the dev half), then
   `full` once more only if the fix went beyond the test that failed.
   Add `--slow` when the change touches what the slow scenarios wait for (keep-alive, items expiring, smelting,
   the crowds).

## What costs what

- A release link of one binary is an LTO link of the whole program: seconds after a small edit, 10 s after a wide
  one, and three at once (a build without `--target`) take 40 s. `check.py` names its targets; when building by
  hand, do too. `bench` is built only when a benchmark is about to be run.
- The dev preset is a second build tree: an edit is compiled twice. Leave it to step 3 unless the bug is one only
  a sanitizer shows (then `tools/check.py --dev -k <scenario>`).
- All scenarios, eight at a time: about a minute on 8 cores (one by one: five). The ones marked `alone=True` (the
  crowds, `many_players`) measure ticks and run after the rest, by themselves.
- `mcgen.py generate` rewrites only tables whose content changed; what includes them is recompiled.

## Reading a failure

- **Build**: the first error is the one to read; the rest usually follow from it.
- **Scenario**: the traceback names the assertion, the server log's tail is under it. For more, run that one
  scenario in sight: `python3 tests/integration/run.py build/release/<exe> -k <name> --keep-log`.
- **A scenario that fails only beside others** (passes with `-k`, fails in the whole run) depends on timing:
  it waits a fixed time where it should wait for an event (`settle()`, `wait_log()`), or it measures ticks and
  wants `@test(alone=True)`. Fix the scenario; do not rerun until it passes.
- **Unit**: `build/release/unit_tests --gtest_filter='Suite.Name'` prints the whole of one test.

## Not this skill's

Benchmarks and anything about speed: the `perf` skill. Comparing with the vanilla server: the probes in
`tools/vanilla/` (AGENTS.md, "The vanilla server as a reference").
