# SPDX-License-Identifier: GPL-3.0-or-later
"""mcgen worldgen: what vanilla's world generation is once the game has loaded.

`worldgen` RUNS tools/mcgen/extract/WorldgenRef.java against the jar and writes one JSON file per
section into .cache/mc/<version>/worldgen/:

  program   the overworld's density functions as vanilla compiled them: a graph of sampler objects
  climate   the overworld's biome parameters and the R-tree they are searched with
  golden    what vanilla computes, as the scripts the unit tests of src/worldgen replay - written
            only when asked for (`worldgen golden`); `--emit` copies them into tests/unit/data/

The JSON under data/minecraft/worldgen/ in the jar says what a world is made of; these files say
what vanilla made of that - which is what decides the bits (a division by a constant compiled into
a multiplication, a `min` that is gone because its ranges do not overlap). gen_worldgen.py turns
them into the tables of src/worldgen and stops where a class or a field it relies on is gone.
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
from pathlib import Path

from mcgen import ROOT, die, find_tool, vdir, version_info, write_stamp

SECTIONS = ("program", "climate", "golden")
TABLES = ("program", "climate")  # what a run without arguments writes, and `generate` reads


def out_dir(version: str) -> Path:
    return vdir(version) / "worldgen"


def load(version: str, section: str) -> dict:
    path = out_dir(version) / f"{section}.json"
    if not path.exists():
        die(f"{path.relative_to(ROOT)} is missing - run: mcgen.py worldgen   (it executes the jar: ask the user first)")
    return json.loads(path.read_text())


def cmd_worldgen(args) -> None:
    for s in args.sections:
        if s not in SECTIONS:
            die(f"no section '{s}' (have: {', '.join(SECTIONS)})")
    d = vdir(args.version)
    work = d / "datagen"
    jars = sorted((work / "versions").rglob("*.jar")) + sorted((work / "libraries").rglob("*.jar"))
    if not jars:
        die("bundled libraries are not unpacked yet - run: mcgen.py reports")
    java = find_tool("java", version_info(args.version)["java_version"])
    src = Path(__file__).resolve().parent / "extract" / "WorldgenRef.java"
    out = out_dir(args.version)
    out.mkdir(exist_ok=True)
    # The JDK's source launcher compiles WorldgenRef.java and the files next to it in memory, as for gamedata.
    cmd = [java, "--sun-misc-unsafe-memory-access=allow", "-cp", os.pathsep.join(str(j) for j in jars), str(src), str(out), *args.sections]
    print(f"mcgen: running {src.relative_to(ROOT)} against {jars[0].name} ({len(jars) - 1} libraries)", flush=True)
    res = subprocess.run(cmd, cwd=work)
    if res.returncode != 0:
        die(f"worldgen failed (exit {res.returncode}): the problems above are vanilla having changed under the extractor")
    if not args.sections:  # a run of some sections leaves the others as old as they were
        write_stamp(args.version, "worldgen")
    print(f"mcgen: wrote {out.relative_to(ROOT)}/")
    if getattr(args, "emit", False):
        if "golden" not in args.sections:
            die("--emit goes with the golden section: mcgen.py worldgen golden --emit")
        # The scripts are drawn with fixed seeds: the same bytes every run, so this changes a
        # tracked file only when the extractor or vanilla changed.
        dest = ROOT / "tests" / "unit" / "data"
        for src in sorted((out / "golden").glob("*.bin")):
            shutil.copyfile(src, dest / f"worldgen_{src.name}")
            print(f"mcgen: tests/unit/data/worldgen_{src.name}: {src.stat().st_size} bytes")


def register(sub) -> None:
    p = sub.add_parser("worldgen", help="RUN the world generation extractor -> .cache/mc/<version>/worldgen/*.json")
    p.add_argument("sections", nargs="*", help=f"only these sections (default: {', '.join(TABLES)}; also: golden)")
    p.add_argument("--emit", action="store_true", help="with golden: copy the scripts into tests/unit/data/worldgen_*.bin")
    p.set_defaults(fn=cmd_worldgen)
