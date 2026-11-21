<!-- What this changes, and why. One change per pull request. -->

## Checked against vanilla

<!-- One of: verified against the jar (which class) / compared with a vanilla server (which probe, what it showed) / not verified.
     For a change no client can observe, say so. -->

## Done means

- [ ] `ctest --preset release` passes
- [ ] the integration suite is clean under the `dev` preset (ASan/UBSan/LSan)
- [ ] new behaviour has its test; what a client can observe was compared with vanilla as said above
- [ ] `tools/ci/checks.sh` is clean (SPDX headers, `clang-format`, the lock of the generated tables, the roadmap)
- [ ] a change of `tools/mcgen` comes with the lock it rewrote (`src/proto/gen/v*.sha256`)
- [ ] the Status table in AGENTS.md and the goals in README.md still tell the truth
- [ ] a performance claim comes with its numbers (`bench`, release build, same machine)
- [ ] every commit is signed off (`git commit -s`): see CONTRIBUTING.md
