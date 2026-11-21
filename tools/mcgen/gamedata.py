"""mcgen gamedata / data: everything about the game that only exists once vanilla has loaded.

`gamedata` RUNS tools/mcgen/extract/GameData.java against the jar and writes one JSON file per
section into .cache/mc/<version>/gamedata/; `data` reads them back, one record at a time, so a
question about a block, an entity type or an enum is one command and a screenful of output.

  states      per block state: shapes, sturdy faces, redstone/piston/path/heightmap predicates, rotations
  blocks      per block: class chain, overridden hooks, properties -> state id, fields, sounds, offsets
  fluids      fluids and fluid states
  items       per item: class chain, overridden hooks, fields
  entities    per entity type: class, dimensions, default attributes; per class: synchronized data
  registries  every other static registry, entry by entry (attributes, effects, potions, game rules, ...)
  enums       every enum of the server jar: ordinals, ids, serialized names
  statics     static tables and constants of block/item/entity/menu classes (waxables, pose sizes, ...)
  menus       slot layout per menu type

What is NOT here because it already is somewhere else: default item components and block state
ids (`reports`), light and occlusion (`blockdata`), anything data-driven - recipes, loot tables,
enchantments, biomes, tags (JSON under data/minecraft/ in the server jar).

These files are a reference and an input for future generators. Nothing reads them at build time
until gen.py is taught to, and what gen.py takes from them it must check (`die()` when a field it
relies on is gone), exactly as it does for blockdata.json.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
from pathlib import Path

from mcgen import ROOT, die, find_tool, jar_for, vdir, version_info, write_stamp

SECTIONS = ("states", "blocks", "fluids", "items", "entities", "registries", "enums", "statics", "menus")


def out_dir(version: str) -> Path:
    return vdir(version) / "gamedata"


def load(version: str, section: str) -> dict:
    path = out_dir(version) / f"{section}.json"
    if not path.exists():
        die(f"{path.relative_to(ROOT)} is missing - run: mcgen.py gamedata   (it executes the jar: ask the user first)")
    return json.loads(path.read_text())


# ----------------------------------------------------------------------------- gamedata (executes the jar)

def cmd_gamedata(args) -> None:
    for s in args.sections:
        if s not in SECTIONS:
            die(f"no section '{s}' (have: {', '.join(SECTIONS)})")
    d = vdir(args.version)
    work = d / "datagen"
    jars = sorted((work / "versions").rglob("*.jar")) + sorted((work / "libraries").rglob("*.jar"))
    if not jars:
        die("bundled libraries are not unpacked yet - run: mcgen.py reports")
    java = find_tool("java", version_info(args.version)["java_version"])
    src = Path(__file__).resolve().parent / "extract" / "GameData.java"
    out = out_dir(args.version)
    out.mkdir(exist_ok=True)
    # The JDK's source launcher compiles GameData.java and the files next to it in memory: no build step.
    # (The flag silences the JDK's warning about a library's use of sun.misc.Unsafe.)
    cmd = [java, "--sun-misc-unsafe-memory-access=allow", "-cp", os.pathsep.join(str(j) for j in jars), str(src), str(out), *args.sections]
    print(f"mcgen: running {src.relative_to(ROOT)} against {jars[0].name} ({len(jars) - 1} libraries)", flush=True)
    res = subprocess.run(cmd, cwd=work)
    if res.returncode != 0:
        die(f"gamedata failed (exit {res.returncode}): the problems above are vanilla having changed under the extractor")
    sections = args.sections or SECTIONS
    if "entities" in sections:
        add_entity_data_defaults(args.version)
    verify(args.version, sections)
    if not args.sections:  # a run of some sections leaves the others as old as they were
        write_stamp(args.version, "gamedata")
    print(f"mcgen: wrote {out.relative_to(ROOT)}/")


def write(path: Path, root: dict) -> None:
    """The layout of Dump.write(): top-level members, and their elements or members, one per line."""
    js = lambda v: json.dumps(v, separators=(",", ":"), ensure_ascii=False)
    out = ["{\n"]
    for i, (key, v) in enumerate(root.items()):
        out.append(f"{js(key)}: ")
        if isinstance(v, list) and v and isinstance(v[0], (dict, list)):
            out.append("[\n" + ",\n".join(" " + js(e) for e in v) + "\n]")
        elif isinstance(v, dict) and v:
            out.append("{\n" + ",\n".join(f" {js(k)}: {js(e)}" for k, e in v.items()) + "\n}")
        else:
            out.append(js(v))
        out.append(",\n" if i + 1 < len(root) else "\n")
    out.append("}\n")
    path.write_text("".join(out))


def add_entity_data_defaults(version: str) -> None:
    """entities.json, classes[*].data[*].default: the value each class gives its synchronized data.

    The extractor cannot make an entity (that takes a level), so this half is read from the bytecode
    instead: defineSynchedData() - for Entity itself, its constructor - is a list of
    `builder.define(ACCESSOR, value)` calls. The value is kept as the expression vanilla wrote: a
    literal in most cases, sometimes a call that a port has to follow (`this.getDefaultVariant()`)."""
    import pseudo
    path = out_dir(version) / "entities.json"
    root = json.loads(path.read_text())
    classes = root["classes"]
    tool = find_tool("javap", version_info(version)["java_version"])
    res = subprocess.run([tool, "-v", "-p", "-c", "-l", "-constants", "-cp", str(jar_for(version, client=False)), *classes],
                         capture_output=True, text=True)
    if res.returncode != 0:
        die(f"javap failed: {res.stderr.strip()}")
    text = pseudo.pseudo(res.stdout, r"defineSynchedData\(|\.Entity\(", fields=False)
    defaults: dict[str, dict[str, str]] = {}
    cur = None
    for line in text.splitlines():
        m = re.match(r"^(?:public |protected |private |abstract |final |static )*(?:class|interface) ([\w$]+)", line)
        if m:
            cur = defaults.setdefault(m.group(1), {})
            continue
        m = re.match(r"^\s+\w+\.define\((\w+), (.*)\);$", line)
        if m and cur is not None:
            cur[m.group(1)] = m.group(2)
    have = total = 0
    for name, cls in classes.items():
        mine = defaults.get(name.rsplit(".", 1)[1], {})
        for d in cls["data"]:
            total += 1
            if d["field"] in mine:
                d["default"] = mine[d["field"]]
                have += 1
    if have != total:
        missing = [f"{n.rsplit('.', 1)[1]}.{d['field']}" for n, c in classes.items() for d in c["data"] if "default" not in d]
        die(f"entity data defaults: none found for {', '.join(missing[:8])} - read `mcgen.py pseudo <class> --method defineSynchedData`")
    write(path, root)
    print(f"mcgen: entity data defaults read from bytecode for {have} of {total} accessors")


def verify(version: str, sections) -> None:
    """Cross-check against the other two sources of the same facts (Mojang's reports, blockdata)."""
    d = vdir(version)
    checked = []
    reg_path = d / "reports" / "registries.json"
    if reg_path.exists():
        regs = json.loads(reg_path.read_text())
        for section, key, registry in (("blocks", "blocks", "block"), ("items", "items", "item"), ("entities", "types", "entity_type")):
            if section not in sections:
                continue
            ours = {e["name"]: e["id"] for e in load(version, section)[key]}
            theirs = {n: e["protocol_id"] for n, e in regs[f"minecraft:{registry}"]["entries"].items()}
            if ours != theirs:
                die(f"gamedata {section}: ids differ from Mojang's registries.json")
            checked.append(f"{registry} ids")
        if "registries" in sections:
            ours_all = load(version, "registries")
            for name, reg in regs.items():
                if name in ours_all and {e["name"]: e["id"] for e in ours_all[name]} != {n: e["protocol_id"] for n, e in reg["entries"].items()}:
                    die(f"gamedata registries: {name} differs from Mojang's registries.json")
            checked.append(f"{len(ours_all) - 1} registries")
    rep_path = d / "reports" / "blocks.json"
    if rep_path.exists() and "blocks" in sections:
        # The stride of every property, against the state list Mojang's own report spells out.
        ours = {b["name"]: b for b in load(version, "blocks")["blocks"]}
        for name, theirs in json.loads(rep_path.read_text()).items():
            b = ours[name]
            for st in theirs["states"]:
                values = st.get("properties", {})
                if st["id"] != b["first_state"] + sum(p["values"].index(values[p["name"]]) * p["stride"] for p in b["properties"]):
                    die(f"gamedata blocks: {name}: properties do not give state {st['id']} of Mojang's blocks.json")
        checked.append("state ids from properties")
    blk_path = d / "extract" / "blockdata.json"
    if blk_path.exists() and "states" in sections:
        old = json.loads(blk_path.read_text())
        new = load(version, "states")
        cols = new["columns"]
        if old["state_count"] != new["state_count"] or old["states"]["block"] != cols["block"]:
            die("gamedata states: state -> block differs from blockdata.json")
        key = lambda boxes: sorted(tuple(b) for b in boxes)
        old_shapes = [key(s) for s in old["collision_shapes"]]
        new_shapes = [key(s) for s in new["shapes"]]
        bad = [i for i, c in enumerate(old["states"]["collision_shape"]) if c >= 0 and old_shapes[c] != new_shapes[cols["collision_shape"][i]]]
        if bad:
            die(f"gamedata states: {len(bad)} collision shapes differ from blockdata.json (first: state {bad[0]})")
        checked.append("collision shapes")
    if checked:
        print(f"mcgen: gamedata agrees with reports/blockdata on: {', '.join(checked)}")


# ----------------------------------------------------------------------------- data (reads the cache)

def _records(section: str, root: dict):
    """(name, record) pairs of a section, whatever its shape."""
    if section in ("blocks", "items", "menus"):
        for e in root[section]:
            yield e["name"], e
    elif section == "fluids":
        for e in root["fluids"]:
            yield e["name"], e
    elif section == "entities":
        for e in root["types"]:
            yield e["name"], e
        for name, e in root["classes"].items():
            yield name, e
    elif section == "registries":
        for reg, entries in root.items():
            if isinstance(entries, list):
                for e in entries:
                    yield f"{reg.split(':', 1)[1]}/{e['name']}", e
    elif section == "enums":
        yield from root["enums"].items()
    elif section == "statics":
        yield from root["classes"].items()


def _state_rows(version: str, root: dict, what: list[str]) -> list[int]:
    """State ids named by `what`: numbers, `a-b` ranges, or block names (all of the block's states)."""
    out: list[int] = []
    blocks = None
    for w in what:
        if re.fullmatch(r"\d+", w):
            out.append(int(w))
        elif m := re.fullmatch(r"(\d+)-(\d+)", w):
            out += range(int(m.group(1)), int(m.group(2)) + 1)
        else:
            blocks = blocks or {b["name"]: b for b in load(version, "blocks")["blocks"]}
            b = blocks.get(w if ":" in w else f"minecraft:{w}")
            if b is None:
                die(f"no block '{w}'")
            out += range(b["first_state"], b["first_state"] + b["state_count"])
    bad = [i for i in out if not 0 <= i < root["state_count"]]
    if bad:
        die(f"state id {bad[0]} is out of range (0..{root['state_count'] - 1})")
    return out


def _state(root: dict, i: int, fields: list[str] | None) -> dict:
    """One state with the column values decoded through the legend."""
    cols, legend = root["columns"], root["legend"]
    row: dict = {"state": i}
    for name, col in cols.items():
        if fields and name not in fields:
            continue
        v = col[i]
        if name.endswith("_shape"):
            v = root["shapes"][v]
        elif name.startswith("sturdy_"):
            v = [d for b, d in enumerate(legend["directions"]) if v >> b & 1]
        elif name in ("pathfindable", "heightmap_opaque", "offset_shapes"):
            v = [d for b, d in enumerate(legend[name]) if v >> b & 1]
        elif name in ("push_reaction", "instrument", "path_type"):
            v = legend[name][v]
        row[name] = v
    return row


def _pick(record, path: str):
    for part in path.split("."):
        if isinstance(record, dict) and part in record:
            record = record[part]
        else:
            return None
    return record


def cmd_data(args) -> None:
    version = args.version
    if args.section is None:
        manifest = out_dir(version) / "manifest.json"
        if not manifest.exists():
            die(f"{manifest.relative_to(ROOT)} is missing - run: mcgen.py gamedata   (it executes the jar: ask the user first)")
        m = json.loads(manifest.read_text())
        print(f"gamedata for {m['version']} (protocol {m['protocol']}), flags: {', '.join(m['feature_flags'])}")
        for name, counts in m["sections"].items():
            print(f"  {name:<11}" + (", ".join(f"{k}={v}" for k, v in counts.items()) if len(counts) <= 8 else f"{len(counts)} tables"))
        print("usage: mcgen.py data <section> [regex ...] [--field a.b] [--names]     states: ids, a-b, or block names")
        return
    if args.section not in SECTIONS:
        die(f"no section '{args.section}' (have: {', '.join(SECTIONS)})")
    root = load(version, args.section)
    fields = args.field or []

    if args.section == "states":
        if not args.what:
            print(f"{root['state_count']} states, {len(root['shapes'])} shapes; columns: {', '.join(root['columns'])}")
            print("legend: " + json.dumps(root["legend"]))
            return
        for i in _state_rows(version, root, args.what):
            print(json.dumps(_state(root, i, fields)))
        return

    patterns = [re.compile(w) for w in args.what]
    shown = 0
    for name, record in _records(args.section, root):
        if patterns and not any(p.search(name) for p in patterns):
            continue
        shown += 1
        if args.names:
            print(name)
        elif fields:
            print(json.dumps({"name": name, **{f: _pick(record, f) for f in fields}}))
        elif patterns:
            print(f"# {name}")
            if isinstance(record, dict):  # one member per line: readable, and still greppable
                for k, v in record.items():
                    print(f"  {k}: {json.dumps(v)}")
            else:
                for v in record:
                    print(f"  {json.dumps(v)}")
        else:
            print(name)
    if not shown:
        print(f"mcgen: nothing in '{args.section}' matches", file=sys.stderr)
        sys.exit(1)


def register(sub) -> None:
    p = sub.add_parser("gamedata", help="RUN the game-data extractor -> .cache/mc/<version>/gamedata/*.json")
    p.add_argument("sections", nargs="*", help=f"only these sections (default: all of {', '.join(SECTIONS)})")
    p.set_defaults(fn=cmd_gamedata)
    p = sub.add_parser("data", help="look up records in the extracted game data (reads the cache, runs nothing)")
    p.add_argument("section", nargs="?", help="section name; none = list what is there")
    p.add_argument("what", nargs="*", help="name regexes (states: ids, a-b ranges or block names); none = list the names")
    p.add_argument("--field", action="append", metavar="PATH", help="print only this field (dotted path, repeatable)")
    p.add_argument("--names", action="store_true", help="print only the names of the matching records")
    p.set_defaults(fn=cmd_data)
