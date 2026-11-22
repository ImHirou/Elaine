#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Record probes on the vanilla server and on ours at the same time, and diff.

    tools/vanilla/compare.py command_probe component_probe     # both probes, whole
    tools/vanilla/compare.py command_probe -k give -k nbt      # only the parts named so
    tools/vanilla/compare.py component_probe -k potion -v      # -v: the diff in full

The probes that record a scripted session (command_probe.py, component_probe.py) take a server
that runs already. Starting the vanilla server, recording, stopping it, the same for ours and
then the diff is five things to wait for one after another - and most of the waiting is the
same script played twice. This does it in one go: both servers are started at once, each probe
is recorded on both side by side, and both are stopped at the end, whatever happened.

  * The vanilla server is the reference of AGENTS.md: run from .cache/vanilla-server with the
    cached jar, on localhost, and only where its EULA has been accepted (by the person at the
    machine: this script looks, and never writes). It is stopped when the probes are done.
  * Ours runs on a free port, in a world of its own that is removed afterwards.
  * The bots get names made of the time, because vanilla keeps what a player it has seen held.
  * `-k word` is handed to the probes: they record only the parts whose names hold one of the
    words (a probe's sections, a component probe's cases). While working on one command that is
    seconds instead of minutes; run the whole of a probe once before the work is called done.
  * `--keep dir` leaves the recordings and the servers' logs there.

Exits with 1 if a probe's diff does.
"""
from __future__ import annotations

import argparse
import os
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


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("probes", nargs="+", help="command_probe, component_probe")
    ap.add_argument("-k", action="append", default=[], metavar="WORD", help="only the parts of a probe named so (repeatable)")
    ap.add_argument("-v", action="store_true", help="print the diffs in full")
    ap.add_argument("--exe", default=str(ROOT / "build" / "release" / "elaine"))
    ap.add_argument("--keep", metavar="DIR", help="leave the recordings and logs in this directory")
    args = ap.parse_args()
    for probe in args.probes:
        if not (HERE / f"{probe}.py").exists():
            raise SystemExit(f"no probe {probe}")

    port = vanilla_port()
    work = Path(args.keep) if args.keep else Path(tempfile.mkdtemp(prefix="compare-"))
    work.mkdir(parents=True, exist_ok=True)
    for stale in work.glob("*.console"):
        stale.unlink()
    with socket.socket() as free:       # a port nothing listens on: reactors bind with SO_REUSEPORT
        free.bind(("127.0.0.1", 0))
        our_port = free.getsockname()[1]
    world = work / "world"
    shutil.rmtree(world, ignore_errors=True)
    t0 = time.time()
    jar = ROOT / ".cache" / "mc" / mc.META["version"] / "server.jar"
    vanilla = Server("vanilla", [JAVA, "-jar", str(jar), "--nogui"], SERVER_DIR, "Done (", work)
    ours = Server("ours", [args.exe, "--server-ip=127.0.0.1", f"--server-port={our_port}", "--online-mode=false", "--view-distance=2",
                           "--max-players=20", f"--ops-file={work / 'ops.json'}", f"--level-name={world}", "--random-tick-speed=0"],
                  work, "listening on", work)
    status = 0
    try:
        ours.wait_ready(30)
        vanilla.wait_ready(180)
        print(f"both servers are up after {time.time() - t0:.0f} s")
        stamp = format(int(time.time()) % 36 ** 5, "x")[-5:]
        for n, probe in enumerate(args.probes):
            names = [f"P{n}a{stamp}", f"P{n}b{stamp}"]
            only = [f"--only={','.join(args.k)}"] if args.k else []
            runs = {}

            def record(server: Server, at: int, out: Path) -> None:
                runs[server.what] = subprocess.run(
                    [sys.executable, str(HERE / f"{probe}.py"), "record", str(at), str(out), *names, str(server.console), *only],
                    stdout=open(work / f"{probe}.{server.what}.out", "w"), stderr=subprocess.STDOUT)

            t1 = time.time()
            recordings = {"vanilla": work / f"{probe}.vanilla.json", "ours": work / f"{probe}.ours.json"}
            threads = [threading.Thread(target=record, args=(vanilla, port, recordings["vanilla"])),
                       threading.Thread(target=record, args=(ours, our_port, recordings["ours"]))]
            for t in threads:
                t.start()
            for t in threads:
                t.join()
            failed = [what for what, run in runs.items() if run.returncode != 0]
            if failed:
                for what in failed:
                    print(f"{probe} on {what} FAILED:\n" + (work / f"{probe}.{what}.out").read_text()[-1500:])
                status = 1
                continue
            diff = subprocess.run([sys.executable, str(HERE / f"{probe}.py"), "diff", str(recordings["vanilla"]), str(recordings["ours"])]
                                  + (["-v"] if args.v else []), capture_output=True, text=True)
            lines = (diff.stdout + diff.stderr).splitlines()
            (work / f"{probe}.diff").write_text("\n".join(lines) + "\n")
            print(f"==== {probe}: recorded on both in {time.time() - t1:.0f} s")
            if args.v:
                print("\n".join(lines))
            else:       # what differs, a little of how, and the last line (the count)
                shown = 0
                for i, line in enumerate(lines):
                    if "DIFFERENT" in line and shown < 40:
                        print("\n".join(ln[:400] for ln in lines[i:i + 5]))
                        shown += 1
                print(lines[-1] if lines else "(no output)")
            status |= diff.returncode != 0
    finally:
        code = ours.stop(None)
        vanilla.stop("stop")
        if code not in (0, None):
            print(f"our server exited with {code}:\n" + ours.log_path.read_text()[-1500:])
            status = 1
        if not args.keep:
            shutil.rmtree(work, ignore_errors=True)
        print(f"done in {time.time() - t0:.0f} s" + (f"; kept in {work}" if args.keep else ""))
    return status


if __name__ == "__main__":
    sys.exit(main())
