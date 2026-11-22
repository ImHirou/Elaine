#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build and test in one command that says little: a line a step, and of a step that failed only
what failed.

    tools/check.py                        release: build, every unit test, every scenario
    tools/check.py -k chest -k furnace    release: build the server, these scenarios
    tools/check.py --unit 'Light*'        release: build the unit tests, these suites
    tools/check.py --failed               release: build the server, the scenarios that failed last time
    tools/check.py --dev [-k ...]         the same under ASan/UBSan/LSan (dev preset): scenarios only
    tools/check.py full                   what "done" asks for: release and dev built, the unit tests,
                                          every scenario on both

The builds come one after the other and it stops at the first that fails. What is tested then runs
side by side: the unit tests split in shards, one a core, and the scenarios of every binary in one pool
(tests/integration/run.py -j 0) - a scenario mostly waits for the server's ticks, so the cores are free
for the rest. Each of those reports when it is over; it exits with 1 if one failed. `full` takes
--slow as run.py does.
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROGRESS = re.compile(r"^\[\d+/\d+\] ")


class Job:
    """A command running in the background, its output going to a file (two of them writing into
    pipes nobody reads at the time would block)."""

    def __init__(self, cmd: list[str], env: dict | None = None):
        self.cmd = cmd
        self.out = tempfile.TemporaryFile("w+", errors="replace")
        self.t0 = time.time()
        self.proc = subprocess.Popen(cmd, cwd=ROOT, env=env, stdout=self.out, stderr=subprocess.STDOUT)

    def wait(self) -> tuple[int, list[str], float]:
        code = self.proc.wait()
        took = time.time() - self.t0
        self.out.seek(0)
        lines = self.out.read().splitlines()
        self.out.close()
        return code, lines, took


def failed(name: str, cmd: list[str], took: float, lines: list[str], limit: int) -> None:
    print(f"{name}: FAILED, {took:.0f}s   ({' '.join(cmd)})")
    if len(lines) > limit:
        print(f"... ({len(lines) - limit} lines before these)")
    print("\n".join(lines[-limit:]))


def build(preset: str, targets: list[str]) -> None:
    if not os.path.exists(os.path.join(ROOT, "build", preset, "build.ninja")):
        code, lines, took = Job(["cmake", "--preset", preset]).wait()
        if code:
            failed(f"configure {preset}", ["cmake", "--preset", preset], took, lines, 150)
            sys.exit(1)
        print(f"configure {preset}: ok, {took:.0f}s")
    cmd = ["cmake", "--build", "--preset", preset, "--target", *targets]
    code, lines, took = Job(cmd).wait()
    if code:   # the commands that failed and what the compiler said, not the progress
        failed(f"build {preset} ({' '.join(targets)})", cmd, took, [ln for ln in lines if not PROGRESS.match(ln)], 150)
        sys.exit(1)
    print(f"build {preset} ({' '.join(targets)}): ok, {took:.0f}s")


def unit(preset: str, pattern: str | None) -> list[Job]:
    """The unit tests in shards (gtest's own: test i runs in shard i mod n), one a core: the longest
    test takes 2.4 s of the 7.3 all of them take in one process."""
    shards = max(1, min(8, os.cpu_count() or 1))
    cmd = [os.path.join("build", preset, "unit_tests"), "--gtest_brief=1"] + ([f"--gtest_filter={pattern}"] if pattern else [])
    return [Job(cmd, dict(os.environ, GTEST_TOTAL_SHARDS=str(shards), GTEST_SHARD_INDEX=str(i))) for i in range(shards)]


def unit_report(pattern: str | None, jobs: list[Job]) -> bool:
    name = "unit tests" + (f" {pattern}" if pattern else "")
    results = [job.wait() for job in jobs]
    took = max(took for _, _, took in results)
    bad = [(job, lines) for job, (code, lines, _) in zip(jobs, results) if code]
    if bad:
        for job, lines in bad:
            failed(name, job.cmd, took, lines, 150)
        return False
    passed = sum(int(m[1]) for _, lines, _ in results for ln in lines if (m := re.search(r"\[  PASSED  \] (\d+) test", ln)))
    if pattern and not passed:
        print(f"{name}: no unit test is named {pattern}")
        return False
    print(f"{name}: ok, {took:.0f}s  {passed} tests passed")
    return True


def scenarios(presets: list[str], exe: str, args: list[str]) -> Job:
    # ASAN_OPTIONS means nothing to a binary without the sanitizers: it may be set for all of them.
    env = dict(os.environ)
    env.setdefault("ASAN_OPTIONS", "detect_leaks=1")
    cmd = [sys.executable, "tests/integration/run.py"] + [os.path.join("build", p, exe) for p in presets] + ["-j", "0", "-q"] + args
    job = Job(cmd, env)
    job.presets = presets
    return job


def scenarios_report(job: Job, args: list[str]) -> bool:
    name = f"scenarios {'+'.join(job.presets)}" + (f" {' '.join(args)}" if args else "")
    code, lines, took = job.wait()
    if code:
        failed(name, job.cmd, took, lines, 250)
        return False
    print(f"{name}: ok, {took:.0f}s  {lines[-1] if lines else ''}")
    return True


def main() -> int:
    sys.stdout.reconfigure(line_buffering=True)  # a line a step as it ends, also into a file (a run in the background)
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("what", nargs="?", choices=["full"])
    ap.add_argument("-k", action="append", default=[], help="scenarios whose name contains this")
    ap.add_argument("--unit", metavar="FILTER", help="unit tests by --gtest_filter")
    ap.add_argument("--failed", action="store_true", help="the scenarios that failed the last time")
    ap.add_argument("--dev", action="store_true", help="the dev preset (sanitizers) instead of release")
    ap.add_argument("--slow", action="store_true", help="also the scenarios that wait on real timers")
    args = ap.parse_args()

    with open(os.path.join(ROOT, "CMakeLists.txt")) as f:
        exe = re.search(r'^set\(MC_EXE "([^"]+)"', f.read(), re.M).group(1)
    run_args = [a for k in args.k for a in ("-k", k)] + (["--failed"] if args.failed else []) + (["--slow"] if args.slow else [])
    t0 = time.time()

    if args.what == "full":
        if args.k or args.unit or args.failed or args.dev:
            ap.error("full is everything: it takes --slow and nothing else")
        build("release", ["server_exe", "unit_tests"])
        build("dev", ["server_exe"])
        units, scen = unit("release", None), scenarios(["release", "dev"], exe, run_args)
        ok = unit_report(None, units)
        ok = scenarios_report(scen, run_args) and ok
    elif args.dev:
        if args.unit:
            ap.error("the unit tests are run on the release preset")
        build("dev", ["server_exe"])
        ok = scenarios_report(scenarios(["dev"], exe, run_args), run_args)
    else:
        some = bool(args.k or args.unit or args.failed)
        want_unit = bool(args.unit) or not some
        want_scenarios = bool(args.k or args.failed) or not some
        build("release", (["server_exe"] if want_scenarios else []) + (["unit_tests"] if want_unit else []))
        units = unit("release", args.unit) if want_unit else []
        scen = scenarios(["release"], exe, run_args) if want_scenarios else None
        ok = unit_report(args.unit, units) if want_unit else True
        ok = (scenarios_report(scen, run_args) if scen else True) and ok
    if not ok:
        return 1
    print(f"all good, {time.time() - t0:.0f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
