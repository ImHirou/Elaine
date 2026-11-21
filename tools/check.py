#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build and test in one command that says little: a line a step, and of a step that failed only
what failed.

    tools/check.py                        release: build, every unit test, every scenario
    tools/check.py -k chest -k furnace    release: build the server, these scenarios
    tools/check.py --unit 'Light*'        release: build the unit tests, these suites
    tools/check.py --failed               release: build the server, the scenarios that failed last time
    tools/check.py --dev [-k ...]         the same under ASan/UBSan/LSan (dev preset): scenarios only
    tools/check.py full                   what "done" asks for: all of release, then all scenarios under dev

Scenarios run side by side (tests/integration/run.py -j 0). It stops at the first step that fails
and exits with 1. `full` takes --slow as run.py does.
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROGRESS = re.compile(r"^\[\d+/\d+\] ")


def step(name: str, cmd: list[str], env: dict | None = None, keep=lambda lines: lines, limit: int = 150) -> list[str]:
    """Run one step; a line if it passed, its output (what `keep` leaves of it) and exit(1) if not."""
    t0 = time.time()
    p = subprocess.run(cmd, cwd=ROOT, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    lines = p.stdout.splitlines()
    took = f"{time.time() - t0:.0f}s"
    if p.returncode == 0:
        return [f"{name}: ok, {took}"] + lines
    print(f"{name}: FAILED, {took}   ({' '.join(cmd)})")
    lines = keep(lines)
    if len(lines) > limit:
        print(f"... ({len(lines) - limit} lines before these)")
    print("\n".join(lines[-limit:]))
    sys.exit(1)


def build(preset: str, targets: list[str]) -> None:
    if not os.path.exists(os.path.join(ROOT, "build", preset, "build.ninja")):
        print(step(f"configure {preset}", ["cmake", "--preset", preset])[0])
    # Of a build that failed: the commands that failed and what the compiler said, not the progress.
    out = step(f"build {preset} ({' '.join(targets)})", ["cmake", "--build", "--preset", preset, "--target", *targets],
               keep=lambda lines: [ln for ln in lines if not PROGRESS.match(ln)])
    print(out[0])


def unit(preset: str, pattern: str | None) -> None:
    cmd = [os.path.join("build", preset, "unit_tests"), "--gtest_brief=1"] + ([f"--gtest_filter={pattern}"] if pattern else [])
    out = step("unit tests" + (f" {pattern}" if pattern else ""), cmd)
    passed = [ln for ln in out if "PASSED" in ln]
    print(out[0] + (f"  {passed[-1].strip()}" if passed else ""))
    if pattern and (not passed or " 0 tests" in passed[-1]):
        print(f"no unit test is named {pattern}")
        sys.exit(1)


def scenarios(preset: str, exe: str, args: list[str]) -> None:
    env = dict(os.environ)
    if preset == "dev":
        env.setdefault("ASAN_OPTIONS", "detect_leaks=1")
    cmd = [sys.executable, "tests/integration/run.py", os.path.join("build", preset, exe), "-j", "0", "-q"] + args
    out = step(f"scenarios {preset}" + (f" {' '.join(args)}" if args else ""), cmd, env=env, limit=250)
    print(f"{out[0]}  {out[-1]}")


def main() -> int:
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
        unit("release", None)
        scenarios("release", exe, run_args)
        build("dev", ["server_exe"])
        scenarios("dev", exe, run_args)
    elif args.dev:
        if args.unit:
            ap.error("the unit tests are run on the release preset")
        build("dev", ["server_exe"])
        scenarios("dev", exe, run_args)
    else:
        some = bool(args.k or args.unit or args.failed)
        want_unit = bool(args.unit) or not some
        want_scenarios = bool(args.k or args.failed) or not some
        build("release", (["server_exe"] if want_scenarios else []) + (["unit_tests"] if want_unit else []))
        if want_unit:
            unit("release", args.unit)
        if want_scenarios:
            scenarios("release", exe, run_args)
    print(f"all good, {time.time() - t0:.0f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
