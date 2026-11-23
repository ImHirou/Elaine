#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Record probes on the vanilla server and on ours at the same time, and diff.

    tools/vanilla/compare.py command_probe component_probe     # both probes, whole
    tools/vanilla/compare.py command_probe -k give -k nbt      # only the parts named so
    tools/vanilla/compare.py component_probe -k potion -v      # -v: the diff in full
    tools/vanilla/compare.py container_probe --noise           # ...and each server against itself

The probes that record a scripted session (command_probe.py, component_probe.py, container_probe.py,
inventory_probe.py, entity_probe.py, swing_probe.py) take a server that runs already, and its
console. This starts both servers at once, records each probe on both side by side, and stops
both at the end, whatever happened. block_probe.py starts its two servers itself; it is run here
on both at once too, before the others (vanilla's port is the same). So is terrain_probe.py, whose
vanilla runs in a world of terrain and whose other side is no server but the generator.

A probe waits for no clock. It freezes the game of the server it records and lets it take the
ticks its script asks for, as fast as the server goes (tools/vanilla/pace.py: `/tick`, which both
servers have): a recording is the same script at the same ticks on either, and takes seconds.

  * The vanilla server is the reference of AGENTS.md: run from .cache/vanilla-server with the
    cached jar, on localhost, and only where its EULA has been accepted (by the person at the
    machine: this script looks, and never writes). It is stopped when the probes are done.
    It runs in a world of this script's own (`--world compare-probe`), made anew every time and
    removed at the end: nothing an earlier recording built, dropped or blew up is in the way.
    An operator a recording that broke off left in its ops.json is taken out again.
  * Ours runs on a free port, in a world of its own that is removed afterwards.
  * The bots get names made of the time, because vanilla keeps what a player it has seen held.
  * `-k word` is handed to the probes that know it (command_probe's and block_probe's sections,
    component_probe's cases): they record only the parts whose names hold one of the words.
  * `--keep dir` leaves the recordings and the servers' logs there.
  * `--noise`: all of it twice - servers and worlds as new the second time as the first, the
    bots of the same names - and the two recordings of each server are diffed as well. What
    differs between two runs of one server is the probe's noise - where a tracker's update
    interval or a random tick fell - and no difference between the servers.
  * `--tick-rate N`: how many times a second a server whose game stands still loops - reads
    packets, ticks its players, reads its console (default 200). That is how soon it answers, and
    most of what a recording takes; the game's own ticks are not waited for at all.

Exits with 1 if a probe's diff does.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "tests" / "integration"))
import mcproto as mc  # noqa: E402
SERVER_DIR = ROOT / ".cache" / "vanilla-server"
JAVA = "/usr/lib/jvm/java-25-openjdk/bin/java"


def vanilla_port() -> int:
    """The port of the reference server, after checking that it may be run and is not exposed."""
    eula = SERVER_DIR / "eula.txt"
    if not eula.exists() or "eula=true" not in eula.read_text():
        raise SystemExit(f"{eula} is missing or not accepted: that is the user's to do (AGENTS.md)")
    props = dict(line.split("=", 1) for line in (SERVER_DIR / "server.properties").read_text().splitlines()
                 if "=" in line and not line.startswith("#"))
    if props.get("server-ip") != "127.0.0.1" or props.get("online-mode") != "false":
        raise SystemExit("server.properties must keep server-ip=127.0.0.1 and online-mode=false")
    return int(props["server-port"])


class Server:
    """A server whose stdin is a FIFO: the probes type on its console by writing to that path."""

    def __init__(self, what: str, cmd: list[str], cwd: Path, ready: str, work: Path):
        self.what, self.ready = what, ready
        self.console = work / f"{what}.console"
        self.log_path = work / f"{what}.log"
        os.mkfifo(self.console)
        self.fd = os.open(self.console, os.O_RDWR)      # held open: a writer that leaves is no end of file
        self.proc = subprocess.Popen(cmd, cwd=cwd, stdin=self.fd, stdout=open(self.log_path, "w"), stderr=subprocess.STDOUT)

    def wait_ready(self, timeout: float) -> None:
        end = time.time() + timeout
        while self.ready not in self.log_path.read_text(errors="replace"):
            if self.proc.poll() is not None or time.time() > end:
                raise SystemExit(f"the {self.what} server did not start:\n" + self.log_path.read_text(errors="replace")[-2000:])
            time.sleep(0.1)

    def type(self, line: str) -> None:
        os.write(self.fd, line.encode() + b"\n")

    def stop(self, gently: str | None) -> int | None:
        if self.proc.poll() is None:
            if gently:
                self.type(gently)
            else:
                self.proc.terminate()
            try:
                self.proc.wait(timeout=60)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        os.close(self.fd)
        return self.proc.returncode


# Probes that start the two servers themselves - `vanilla <out> <names>`, `ours <exe> <out> <names>` - in
# worlds of their own and with rules of their own: run here both at once, before the shared servers are up
# (vanilla's port is the same).
SELF_STARTING = {"block_probe", "terrain_probe"}


def report(probe: str, recordings: dict, work: Path, verbose: bool, took: float, kind: str = "") -> int:
    """The diff of a probe's two recordings: what differs, a little of how, and its last line."""
    diff = subprocess.run([sys.executable, str(HERE / f"{probe}.py"), "diff", str(recordings["vanilla"]), str(recordings["ours"])]
                          + (["-v"] if verbose else []), capture_output=True, text=True)
    lines = (diff.stdout + diff.stderr).splitlines()
    (work / f"{probe}{'.' + kind if kind else ''}.diff").write_text("\n".join(lines) + "\n")
    if not kind:
        print(f"==== {probe}: recorded on both in {took:.0f} s")
    if verbose:
        print("\n".join(lines))
    else:
        shown = 0
        for i, line in enumerate(lines):
            if "DIFFERENT" in line and shown < 40:
                print("\n".join(ln[:400] for ln in lines[i:i + 5]))
                shown += 1
        print(lines[-1] if lines else "(no output)")
    return int(diff.returncode != 0)


def recorded(probe: str, runs: dict, work: Path) -> bool:
    failed = [what for what, run in runs.items() if run.returncode != 0]
    for what in failed:
        print(f"{probe} on {what} FAILED:\n" + (work / f"{probe}.{what}.out").read_text()[-1500:])
    return not failed


def both_at_once(record) -> dict:
    """record(what) on "vanilla" and "ours" side by side; what each run returned."""
    runs = {}
    threads = [threading.Thread(target=lambda what=what: runs.__setitem__(what, record(what))) for what in ("vanilla", "ours")]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    return runs


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("probes", nargs="+", help="command_probe, component_probe, container_probe, ..., block_probe, terrain_probe")
    ap.add_argument("-k", action="append", default=[], metavar="WORD", help="only the parts of a probe named so (repeatable)")
    ap.add_argument("-v", action="store_true", help="print the diffs in full")
    ap.add_argument("--exe", default=str(ROOT / "build" / "release" / "elaine"))
    ap.add_argument("--keep", metavar="DIR", help="leave the recordings and logs in this directory")
    ap.add_argument("--tick-rate", type=float, default=200.0, metavar="N",
                    help="loops a second of a server whose game stands still: how soon it reads what a probe sends (pace.py)")
    ap.add_argument("--noise", action="store_true",
                    help="record every probe twice on each server, and diff the two of a server as well: what differs there is no difference")
    args = ap.parse_args()
    if not 1.0 <= args.tick_rate <= 10000.0:
        ap.error("--tick-rate is vanilla's /tick rate: 1 to 10000")
    for probe in args.probes:
        if not (HERE / f"{probe}.py").exists():
            raise SystemExit(f"no probe {probe}")
    port = vanilla_port()
    work = Path(args.keep) if args.keep else Path(tempfile.mkdtemp(prefix="compare-"))
    t0 = time.time()
    env = dict(os.environ, MC_PROBE_TICK_RATE=f"{args.tick_rate:g}")
    stamp = format(int(time.time()) % 36 ** 5, "x")[-5:]
    status = 0
    try:
        first = record_all(args, work, port, env, stamp)
        # The same once more, from servers and worlds as new as the first had: whatever the two
        # recordings of one server differ by is the probe's noise.
        again = record_all(args, work / "again", port, env, stamp) if args.noise else None
        for probe in args.probes:
            recordings, took = first[probe]
            if recordings is None or (again and again[probe][0] is None):
                status = 1
                continue
            if again:
                for what in ("vanilla", "ours"):
                    print(f"==== {probe}: {what} against itself")
                    report(probe, {"vanilla": recordings[what], "ours": again[probe][0][what]}, work, args.v, took, f"{what}-twice")
            status |= report(probe, recordings, work, args.v, took)
    finally:
        if not args.keep:
            shutil.rmtree(work, ignore_errors=True)
        print(f"done in {time.time() - t0:.0f} s" + (f"; kept in {work}" if args.keep else ""))
    return status


def record_all(args, work: Path, port: int, env: dict, stamp: str) -> dict:
    """Every probe once on both servers: probe -> (its two recordings, or None; the seconds it took)."""
    work.mkdir(parents=True, exist_ok=True)
    for stale in work.glob("*.console"):
        stale.unlink()
    out = {}
    only = [f"--only={','.join(args.k)}"] if args.k else []
    for n, probe in enumerate(p for p in args.probes if p in SELF_STARTING):
        names = [f"Q{n}a{stamp}", f"Q{n}b{stamp}"]
        recordings = {"vanilla": work / f"{probe}.vanilla.json", "ours": work / f"{probe}.ours.json"}
        mode = {"vanilla": ["vanilla"], "ours": ["ours", args.exe]}
        t1 = time.time()
        runs = both_at_once(lambda what: subprocess.run(
            [sys.executable, str(HERE / f"{probe}.py"), *mode[what], str(recordings[what]), *names, *only],
            stdout=open(work / f"{probe}.{what}.out", "w"), stderr=subprocess.STDOUT, env=env))
        out[probe] = (recordings if recorded(probe, runs, work) else None, time.time() - t1)
    session = [p for p in args.probes if p not in SELF_STARTING]
    if session:
        out.update(run_session(session, args, work, port, env, stamp))
    return out


PROBE_WORLD = "compare-probe"                   # vanilla's world for a session: made anew, removed afterwards
PROBE_NAME = re.compile(r"[PQ]\d[a-d][0-9a-z]{5}C?")   # a bot of this script


def remove_probe_world() -> None:
    world = SERVER_DIR / PROBE_WORLD
    if world.exists():
        if not (world / "level.dat").exists():
            raise SystemExit(f"{world} is not a world: it is not this script's to remove")
        shutil.rmtree(world)


def run_session(probes: list[str], args, work: Path, port: int, env: dict, stamp: str) -> dict:
    """The probes that record a session on a server that runs: both servers up at once, each probe
    recorded on the two side by side, both stopped at the end. Vanilla in a world of its own, new
    every time: what an earlier recording built or dropped is in nobody's way. Returns, per probe,
    its two recordings (or None) and the seconds they took."""
    with socket.socket() as free:       # a port nothing listens on: reactors bind with SO_REUSEPORT
        free.bind(("127.0.0.1", 0))
        our_port = free.getsockname()[1]
    world = work / "world"
    shutil.rmtree(world, ignore_errors=True)
    remove_probe_world()
    t0 = time.time()
    jar = ROOT / ".cache" / "mc" / mc.META["version"] / "server.jar"
    vanilla = Server("vanilla", [JAVA, "-jar", str(jar), "--nogui", "--world", PROBE_WORLD], SERVER_DIR, "Done (", work)
    ours = Server("ours", [args.exe, "--server-ip=127.0.0.1", f"--server-port={our_port}", "--online-mode=false", "--view-distance=2",
                           "--max-players=20", f"--ops-file={work / 'ops.json'}", f"--level-name={world}", "--random-tick-speed=0"],
                  work, "listening on", work)
    out = {}
    try:
        ours.wait_ready(30)
        vanilla.wait_ready(180)
        # A recording that broke off left its operator behind: vanilla keeps ops.json for good.
        ops = SERVER_DIR / "ops.json"
        for entry in json.loads(ops.read_text()) if ops.exists() else []:
            if PROBE_NAME.fullmatch(entry.get("name", "")):
                vanilla.type(f"deop {entry['name']}")
        # Whoever joins stands where our server puts everyone: on the block of the world's spawn.
        vanilla.type("setworldspawn 8 -60 8")
        vanilla.type("gamerule minecraft:respawn_radius 0")
        print(f"both servers are up after {time.time() - t0:.0f} s")
        servers = {"vanilla": (vanilla, port), "ours": (ours, our_port)}
        for n, probe in enumerate(probes):
            names = [f"P{n}a{stamp}", f"P{n}b{stamp}"]
            only = [f"--only={','.join(args.k)}"] if args.k else []
            recordings = {what: work / f"{probe}.{what}.json" for what in servers}
            t1 = time.time()
            runs = both_at_once(lambda what: subprocess.run(
                [sys.executable, str(HERE / f"{probe}.py"), "record", str(servers[what][1]), str(recordings[what]), *names,
                 str(servers[what][0].console), *only],
                stdout=open(work / f"{probe}.{what}.out", "w"), stderr=subprocess.STDOUT, env=env))
            out[probe] = (recordings if recorded(probe, runs, work) else None, time.time() - t1)
    finally:
        code = ours.stop(None)
        if vanilla.proc.poll() is None:
            vanilla.type("tick unfreeze")       # (a probe leaves it so; this is for one that broke off)
            vanilla.type("tick rate 20")
        vanilla.stop("stop")
        remove_probe_world()
        if code not in (0, None):
            print(f"our server exited with {code}:\n" + ours.log_path.read_text()[-1500:])
            out = {probe: (None, 0.0) for probe in probes}
    for probe in probes:
        out.setdefault(probe, (None, 0.0))
    return out


if __name__ == "__main__":
    sys.exit(main())
