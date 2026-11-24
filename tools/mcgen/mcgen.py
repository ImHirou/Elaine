#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""mcgen - Minecraft protocol data pipeline (stdlib only).

Inputs come from Mojang's official jars (sha1-verified against piston-meta).
Everything downloaded or derived lands in .cache/mc/<version>/, and the generated C++ in
src/proto/gen/v<protocol>/ - both gitignored: they are Mojang's data, and every clone makes them
from the jar. What is committed of the generated code is its lock, src/proto/gen/v<protocol>.sha256.

Commands:
  fetch     download + verify server.jar (and client.jar with --client), unpack inner jar
  find      list jar classes matching a regex            (static, nothing is executed)
  javap     disassemble classes, filtered to codec-relevant lines (static)
  codec     one-line-per-method wire format summary of classes   (static)
  packets   derive packets.json from *Protocols bytecode  (static, same format as Mojang's report)
  reports   RUN Mojang's data generator -> packets.json, registries.json, blocks.json, commands.json, item components
  blockdata RUN tools/mcgen/extract/Extract.java against the jar -> per-state light/fluid/shape tables
  gamedata  RUN tools/mcgen/extract/GameData.java against the jar -> everything else that exists only at
            runtime: shapes, hooks, block/item/entity fields, attributes, registries, enums, menus
  data      look up a record in what `gamedata` extracted (reads the cache, runs nothing)
  worldgen  RUN tools/mcgen/extract/WorldgenRef.java against the jar -> the overworld's density functions as
            vanilla compiled them, its biome parameters and their search tree
  pseudo    methods as compact pseudo-code with real local names (static, for reading logic)
  generate  emit src/proto/gen/v<protocol>/ from the cache, and write its lock
  check     verify that the generated code is what the committed lock says (the build runs this)
  setup     RUN everything a fresh clone needs, in order: fetch, reports, blockdata, gamedata, worldgen -
            each only if it is not already done - then generate, then check

`packets` exists so packet ids can be obtained without executing the jar; when
`reports` has been run its packets.json wins and the two must agree.

`reports`, `blockdata`, `gamedata` and `worldgen` - and `setup`, which runs them - are the only steps that
execute downloaded code (the sha1-verified official jar). The first two only build vanilla's static registries; `gamedata` and
`worldgen` also load the vanilla data pack, as a server does before it creates a world. None starts a server.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CACHE = ROOT / ".cache" / "mc"
MANIFEST_URL = "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json"
DEFAULT_VERSION = "26.3"

STATES = {  # Protocols class -> state name used in Mojang's packets.json
    "net.minecraft.network.protocol.handshake.HandshakeProtocols": "handshake",
    "net.minecraft.network.protocol.status.StatusProtocols": "status",
    "net.minecraft.network.protocol.login.LoginProtocols": "login",
    "net.minecraft.network.protocol.configuration.ConfigurationProtocols": "configuration",
    "net.minecraft.network.protocol.game.GameProtocols": "play",
}


def die(msg: str) -> "None":
    print(f"mcgen: error: {msg}", file=sys.stderr)
    sys.exit(1)


def vdir(version: str) -> Path:
    return CACHE / version


def sha1_of(path: Path) -> str:
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def http_json(url: str):
    with urllib.request.urlopen(url, timeout=60) as r:
        return json.load(r)


def download(url: str, dest: Path, sha1: str) -> None:
    if dest.exists() and sha1_of(dest) == sha1:
        return
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_suffix(dest.suffix + ".part")
    print(f"mcgen: downloading {url}")
    with urllib.request.urlopen(url, timeout=600) as r, open(tmp, "wb") as f:
        shutil.copyfileobj(r, f, 1 << 20)
    got = sha1_of(tmp)
    if got != sha1:
        tmp.unlink()
        die(f"sha1 mismatch for {url}: got {got}, expected {sha1}")
    tmp.rename(dest)


# ----------------------------------------------------------------------------- fetch

def cmd_fetch(args) -> None:
    d = vdir(args.version)
    d.mkdir(parents=True, exist_ok=True)
    meta_path = d / "version_meta.json"
    if not meta_path.exists():
        manifest = http_json(MANIFEST_URL)
        entry = next((v for v in manifest["versions"] if v["id"] == args.version), None)
        if entry is None:
            die(f"version {args.version} not in Mojang manifest (latest: {manifest['latest']})")
        meta_path.write_text(json.dumps(http_json(entry["url"]), indent=1))
    meta = json.loads(meta_path.read_text())
    if "server_mappings" in meta["downloads"]:
        die("this version ships obfuscated jars; mcgen's static steps need 26.1+")

    kinds = ["server"] + (["client"] if args.client else [])
    for kind in kinds:
        dl = meta["downloads"][kind]
        download(dl["url"], d / f"{kind}.jar", dl["sha1"])

    # server.jar is a bundler; the real classes + vanilla datapack are in an inner jar.
    inner = d / "server-inner.jar"
    if not inner.exists():
        with zipfile.ZipFile(d / "server.jar") as z:
            names = [n for n in z.namelist() if re.fullmatch(r"META-INF/versions/[^/]+/server-[^/]+\.jar", n)]
            if len(names) != 1:
                die(f"expected one inner server jar, found {names}")
            with z.open(names[0]) as src, open(inner, "wb") as dst:
                shutil.copyfileobj(src, dst, 1 << 20)
            (d / "version.json").write_bytes(z.read("version.json"))
    v = json.loads((d / "version.json").read_text())
    print(f"mcgen: {v['id']} protocol={v['protocol_version']} world={v['world_version']} "
          f"java={v['java_version']} cache={d.relative_to(ROOT)}")


def jar_for(version: str, client: bool) -> Path:
    p = vdir(version) / ("client.jar" if client else "server-inner.jar")
    if not p.exists():
        die(f"{p.relative_to(ROOT)} missing - run: mcgen.py fetch{' --client' if client else ''}")
    return p


def version_info(version: str) -> dict:
    p = vdir(version) / "version.json"
    if not p.exists():
        die("version.json missing - run: mcgen.py fetch")
    return json.loads(p.read_text())


def find_tool(name: str, need_major: int) -> str:
    """Locate a JDK tool whose major version is >= need_major (class files are that new)."""
    cands = []
    if os.environ.get("JAVA_HOME"):
        cands.append(Path(os.environ["JAVA_HOME"]) / "bin" / name)
    jvm = Path("/usr/lib/jvm")
    if jvm.is_dir():
        cands += sorted((p / "bin" / name for p in jvm.iterdir()), reverse=True)
    if shutil.which(name):
        cands.append(Path(shutil.which(name)))
    for c in cands:
        if not c.exists():
            continue
        try:
            out = subprocess.run([str(c), "-version"], capture_output=True, text=True, timeout=20)
        except OSError:
            continue
        m = re.search(r"(\d+)", out.stdout + out.stderr)  # 'openjdk version "25.0.1"' / javap prints '25.0.1'
        if m and int(m.group(1)) >= need_major:
            return str(c)
    die(f"no {name} >= {need_major} found (set JAVA_HOME)")


# ----------------------------------------------------------------------------- stamps
#
# The steps that run the jar take minutes and give the same files every time. Each leaves a
# stamp - the jar it ran and the sources it ran it with - so that `setup` runs a step again only
# when one of those changed, or its files are gone.

EXTRACT = Path(__file__).resolve().parent / "extract"
# step -> (sources whose change makes its output stale - a pattern that begins with `!` takes files out
# of what the patterns before it named - and what it must have left under the version's directory)
STEPS = {
    "reports": ((), ("reports/packets.json", "reports/registries.json", "reports/blocks.json", "reports/commands.json",
                     "reports/item_components", "datagen/libraries")),
    "blockdata": (("extract/Extract.java",), ("extract/blockdata.json",)),
    "gamedata": (("extract/*.java", "!extract/Worldgen*.java", "gamedata.py"),
                 ("gamedata/states.json", "gamedata/blocks.json", "gamedata/entities.json")),
    "worldgen": (("extract/Worldgen*.java", "extract/GameData.java", "extract/Dump.java", "worldgen.py"),
                 ("worldgen/program.json", "worldgen/climate.json", "worldgen/features.json")),
}


def stamp_key(version: str, step: str) -> str:
    h = hashlib.sha256()
    h.update(json.loads((vdir(version) / "version_meta.json").read_text())["downloads"]["server"]["sha1"].encode())
    here = Path(__file__).resolve().parent
    sources: list[Path] = []
    for pattern in STEPS[step][0]:
        if pattern.startswith("!"):
            sources = [src for src in sources if not src.match(pattern[1:])]
        else:
            sources += sorted(here.glob(pattern))
    for src in sources:
        h.update(src.name.encode() + b"\0" + src.read_bytes())
    return h.hexdigest()


def stamp_is_current(version: str, step: str) -> bool:
    d = vdir(version)
    stamp = d / "stamps" / step
    return (stamp.is_file() and stamp.read_text().strip() == stamp_key(version, step)
            and all((d / out).exists() for out in STEPS[step][1]))


def write_stamp(version: str, step: str) -> None:
    stamp = vdir(version) / "stamps" / step
    stamp.parent.mkdir(exist_ok=True)
    stamp.write_text(stamp_key(version, step) + "\n")


# ----------------------------------------------------------------------------- javap

def javap(version: str, classes: list[str], client: bool = False) -> str:
    tool = find_tool("javap", version_info(version)["java_version"])
    out = subprocess.run([tool, "-p", "-c", "-constants", "-cp", str(jar_for(version, client)), *classes],
                         capture_output=True, text=True)
    if out.returncode != 0:
        die(f"javap failed: {out.stderr.strip()}")
    return out.stdout


_NOISE = re.compile(
    r"^\s+\d+: (aload|astore|iload|istore|lload|lstore|fload|fstore|dload|dstore|areturn|ireturn|lreturn|return|dup|pop|"
    r"new|checkcast|goto|nop|aconst_null|athrow|monitorenter|monitorexit|arraylength|i2l|l2i|i2b|i2f|f2d|d2f|i2d)"
    r"(_\w+)?\b")
_BOILER = re.compile(
    r"(toString|hashCode|equals):?\(|ObjectMethods|^\s*$|^\s+Code:$|^Compiled from|^}$|"
    r'java/lang/(Record|Object)\."<init>"|^\s+\d+: (getfield|putfield)\b|^\s+public \S+ \w+\(\);$|'
    r"PacketType<|\.type:\(\)|Listener\.handle|void handle\(")
_HEADER = re.compile(r"^  \S")
_SHORT = [(re.compile(r"#\d+(,\s*\d+)?\s+// "), ""),
          (re.compile(r"net[/.]minecraft[/.]network[/.]protocol[/.]\w+[/.]"), ""),
          (re.compile(r"net[/.]minecraft[/.]network[/.](codec[/.])?"), ""),
          (re.compile(r"java[/.](lang|util)[/.](function[/.])?"), ""),
          (re.compile(r"(Interface)?Method "), "")]


def brief(text: str) -> str:
    """Reduce javap output to what describes a wire format: fields in declaration
    order, constants, and the read*/write*/codec calls in execution order."""
    keep: list[str] = []
    for line in text.splitlines():
        if _NOISE.match(line) or _BOILER.search(line):
            continue
        for rx, rep in _SHORT:
            line = rx.sub(rep, line)
        line = re.sub(r"^(\s+)\d+: ", r"\1  ", line)
        if _HEADER.match(line) and keep and _HEADER.match(keep[-1]) and "(" in keep[-1]:
            keep.pop()  # previous method had nothing of interest (accessor)
        keep.append(line[:200])
    if keep and _HEADER.match(keep[-1]) and "(" in keep[-1]:
        keep.pop()
    return "\n".join(keep)


def only_methods(text: str, pattern: str) -> str:
    """Keep class headers plus the methods whose signature matches `pattern`."""
    rx = re.compile(pattern)
    out: list[str] = []
    on = False
    for line in text.splitlines():
        if not line.startswith(" "):
            on = False
            if line.strip() and line != "}" and not line.startswith("Compiled"):
                out.append(line)
        elif re.match(r"^  \S", line):
            on = bool(rx.search(line))
            if on:
                out.append(line)
        elif on:
            out.append(line)
    return "\n".join(out)


def cmd_javap(args) -> None:
    text = javap(args.version, args.classes, args.client)
    if args.method:
        text = only_methods(text, args.method)
    print(text if args.full else brief(text))


_TOK = [
    (re.compile(r"getstatic .*Field (?:[\w/$]+/)?([\w$]+)\.(\w+):L[\w/$]*StreamCodec"), lambda m: f"{m.group(1)}.{m.group(2)}"),
    (re.compile(r"getstatic .*Field (\w+):L[\w/$]*StreamCodec"), lambda m: f"this.{m.group(1)}"),
    (re.compile(r"Method (?:[\w/$]+/)?(?:FriendlyByteBuf|RegistryFriendlyByteBuf|ByteBuf|VarInt|VarLong|Utf8String)\.((?:read|write)\w*):"), lambda m: m.group(1)),
    (re.compile(r"Method (?:[\w/$]+/)?(ByteBufCodecs|StreamCodec|ResourceKey|Packet)\.(\w+):"), lambda m: f"{m.group(2)}()"),
    (re.compile(r"Method ([\w/$]+)\.((?:read|write|encode|decode|streamCodec)\w*):"), lambda m: f"{m.group(1).split('/')[-1]}.{m.group(2)}()"),
    (re.compile(r"(?:bipush|sipush)\s+(-?\d+)"), lambda m: m.group(1)),
    (re.compile(r"ldc(?:_w)?\s+#\d+\s+// (?:int|String|float|long) (.+)"), lambda m: repr(m.group(1))),
]


def codec_summary(text: str) -> str:
    """One line per codec-bearing method: the ordered codec/read/write tokens.
    This is the wire format in a form short enough to read dozens of packets at once."""
    out: list[str] = []
    sig = None
    toks: list[str] = []

    def flush():
        if sig and toks and not re.search(r"lambda\$|toString|hashCode|equals|values\(|valueOf\(", sig):
            name = re.sub(r"\(.*", "", sig.split()[-1] if "(" not in sig.split()[-1] else sig[:sig.index("(")].split()[-1])
            name = "static" if sig.startswith("static {}") else name.split(".")[-1]
            out.append(f"    {name}: " + " ".join(toks))

    for line in text.splitlines():
        if not line.startswith(" "):
            flush(); sig, toks = None, []
            m = re.search(r"(?:class|interface|enum) ([\w.$]+)", line)
            if m:
                out.append(m.group(1).split(".")[-1])
        elif re.match(r"^  \S", line):
            flush(); toks = []
            sig = line.strip()
            m = re.match(r"^  (?:private|public|protected)? ?(?:final )?([\w.$<>\[\], ?]+) (\w+);$", line)
            if m and " static " not in line:
                out.append(f"  .{m.group(2)}: {re.sub(r'[a-z]+\.', '', m.group(1))}")
                sig = None
        elif sig:
            for rx, fmt in _TOK:
                m = rx.search(line)
                if m:
                    toks.append(fmt(m))
                    break
    flush()
    return "\n".join(out)


def cmd_codec(args) -> None:
    print(codec_summary(javap(args.version, args.classes, args.client)))


def cmd_find(args) -> None:
    rx = re.compile(args.pattern)
    with zipfile.ZipFile(jar_for(args.version, args.client)) as z:
        for n in z.namelist():
            if rx.search(n):
                print(n[:-6].replace("/", ".") if n.endswith(".class") else n)


# ----------------------------------------------------------------------------- packets (static)

def _methods(text: str) -> dict[str, list[str]]:
    """Split javap -c output into {method signature: [instruction lines]}."""
    methods: dict[str, list[str]] = {}
    cur = None
    for line in text.splitlines():
        if re.match(r"^  \S.*[\(\{].*;?$", line) and not line.strip().startswith("//"):
            cur = line.strip()
            methods[cur] = []
        elif cur is not None and re.match(r"^\s+\d+: ", line):
            methods[cur].append(line)
    return methods


def _packet_type_names(version: str, holders: set[str]) -> dict[str, tuple[str, str]]:
    """{'pkg/XPacketTypes.FIELD': (direction, 'minecraft:name')} from the holders' static init."""
    res: dict[str, tuple[str, str]] = {}
    text = javap(version, sorted(h.replace("/", ".") for h in holders))
    holder = None
    pending = None
    direction = None
    for line in text.splitlines():
        m = re.match(r"^(?:public |final |abstract )*class (\S+)", line)
        if m:
            holder = m.group(1).replace(".", "/")
            pending = None
            continue
        m = re.search(r"\d+: ldc(?:_w)?\s+#\d+\s+// String (\S+)", line)
        if m:
            pending = m.group(1)
            continue
        m = re.search(r"invokestatic .*Method (?:\S+\.)?create(Clientbound|Serverbound):", line)
        if m:
            direction = m.group(1).lower()
            continue
        m = re.search(r"putstatic .*Field (\w+):Lnet/minecraft/network/protocol/PacketType;", line)
        if m and pending and direction:
            res[f"{holder}.{m.group(1)}"] = (direction, f"minecraft:{pending}")
            pending = direction = None
    return res


def static_packets(version: str) -> dict:
    refs: dict[str, list[tuple[str, bool]]] = {}   # state -> [(field ref, is_bundle)] per lambda, kept in order
    lambdas: list[tuple[str, list[tuple[str, bool]]]] = []
    holders: set[str] = set()
    for cls, state in STATES.items():
        for sig, ins in _methods(javap(version, [cls])).items():
            if "ProtocolInfoBuilder)" not in sig or "lambda$" not in sig:
                continue
            seq: list[tuple[str, bool]] = []
            last = None
            for line in ins:
                m = re.search(r"getstatic .*Field (\S+)\.(\w+):Lnet/minecraft/network/protocol/PacketType;", line)
                if m:
                    last = f"{m.group(1)}.{m.group(2)}"
                    holders.add(m.group(1))
                    continue
                m = re.search(r"Method net/minecraft/network/protocol/ProtocolInfoBuilder\.(addPacket|withBundlePacket):", line)
                if m:
                    if last is None:
                        die(f"{cls}: registration without a PacketType in {sig}")
                    seq.append((last, m.group(1) == "withBundlePacket"))
                    last = None
            if seq:
                lambdas.append((state, seq))
    names = _packet_type_names(version, holders)

    out: dict = {}
    for state, seq in lambdas:
        dirs = {names[ref][0] for ref, _ in seq if ref in names}
        missing = [ref for ref, _ in seq if ref not in names]
        if missing or len(dirs) != 1:
            die(f"{state}: cannot resolve packet types (missing={missing[:3]}, directions={dirs})")
        direction = dirs.pop()
        table = out.setdefault(state, {}).setdefault(direction, {})
        if table:
            die(f"{state}/{direction}: registered twice")
        for pid, (ref, is_bundle) in enumerate(seq):
            # withBundlePacket registers the delimiter's type, not the bundle pseudo-type.
            name = "minecraft:bundle_delimiter" if is_bundle else names[ref][1]
            if name in table:
                die(f"{state}/{direction}: duplicate {name}")
            table[name] = {"protocol_id": pid}
    return {s: {d: out[s][d] for d in sorted(out[s])} for s in sorted(out)}


def cmd_packets(args) -> None:
    d = vdir(args.version)
    pk = static_packets(args.version)
    dest = d / "static" / "packets.json"
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text(json.dumps(pk, indent=2) + "\n")
    official = d / "reports" / "packets.json"
    for state, dirs in pk.items():
        print("  " + state + ": " + ", ".join(f"{k}={len(v)}" for k, v in dirs.items()))
    if official.exists():
        same = json.loads(official.read_text()) == pk
        print(f"mcgen: static extraction {'MATCHES' if same else 'DIFFERS FROM'} Mojang's report")
        if not same:
            sys.exit(2)
    print(f"mcgen: wrote {dest.relative_to(ROOT)}")


# ----------------------------------------------------------------------------- reports (executes the jar)

def cmd_reports(args) -> None:
    d = vdir(args.version)
    if not (d / "server.jar").exists():
        die("server.jar missing - run: mcgen.py fetch")
    java = find_tool("java", version_info(args.version)["java_version"])
    work = d / "datagen"
    work.mkdir(exist_ok=True)
    cmd = [java, "-DbundlerMainClass=net.minecraft.data.Main", "-jar", str(d / "server.jar"),
           "--reports", "--output", str(work / "out")]
    print("mcgen: running Mojang data generator:", " ".join(cmd))
    subprocess.run(cmd, cwd=work, check=True)
    rep = d / "reports"
    rep.mkdir(exist_ok=True)
    # commands.json is the whole command tree (what the `commands` packet is built from).
    for name in ("packets.json", "registries.json", "blocks.json", "commands.json"):
        shutil.copyfile(work / "out" / "reports" / name, rep / name)
    # Default data components per item (max stack size, tool rules, ...): one small json per item.
    comp = rep / "item_components"
    if comp.exists():
        shutil.rmtree(comp)
    shutil.copytree(work / "out" / "reports" / "minecraft" / "components" / "item", comp)
    write_stamp(args.version, "reports")
    print(f"mcgen: reports in {rep.relative_to(ROOT)}")


# ----------------------------------------------------------------------------- blockdata (executes the jar)

def cmd_blockdata(args) -> None:
    """Per-state properties that vanilla only computes at class-init time (light, fluids, shapes).

    Uses the libraries the bundler unpacked for `reports`, and the JDK's source launcher, so there
    is no build step and nothing to keep in sync besides Extract.java itself."""
    d = vdir(args.version)
    work = d / "datagen"
    jars = sorted((work / "versions").rglob("*.jar")) + sorted((work / "libraries").rglob("*.jar"))
    if not jars:
        die("bundled libraries are not unpacked yet - run: mcgen.py reports")
    java = find_tool("java", version_info(args.version)["java_version"])
    src = Path(__file__).resolve().parent / "extract" / "Extract.java"
    out = d / "extract"
    out.mkdir(exist_ok=True)
    cmd = [java, "-cp", os.pathsep.join(str(j) for j in jars), str(src), str(out / "blockdata.json")]
    print(f"mcgen: running {src.relative_to(ROOT)} against {jars[0].name} ({len(jars) - 1} libraries)")
    subprocess.run(cmd, cwd=work, check=True)
    write_stamp(args.version, "blockdata")
    print(f"mcgen: wrote {(out / 'blockdata.json').relative_to(ROOT)}")


# ----------------------------------------------------------------------------- main

def main() -> None:
    ap = argparse.ArgumentParser(prog="mcgen", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", default=DEFAULT_VERSION, help=f"Minecraft version (default {DEFAULT_VERSION})")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("fetch"); p.add_argument("--client", action="store_true"); p.set_defaults(fn=cmd_fetch)
    p = sub.add_parser("find"); p.add_argument("pattern"); p.add_argument("--client", action="store_true"); p.set_defaults(fn=cmd_find)
    p = sub.add_parser("javap"); p.add_argument("classes", nargs="+"); p.add_argument("--client", action="store_true")
    p.add_argument("--full", action="store_true", help="unfiltered javap output")
    p.add_argument("--method", metavar="REGEX", help="only methods whose signature matches"); p.set_defaults(fn=cmd_javap)
    p = sub.add_parser("codec", help="compact wire-format summary of classes"); p.add_argument("classes", nargs="+")
    p.add_argument("--client", action="store_true"); p.set_defaults(fn=cmd_codec)
    p = sub.add_parser("packets"); p.set_defaults(fn=cmd_packets)
    p = sub.add_parser("reports"); p.set_defaults(fn=cmd_reports)
    p = sub.add_parser("blockdata"); p.set_defaults(fn=cmd_blockdata)

    from pseudo import cmd_pseudo  # noqa: E402
    p = sub.add_parser("pseudo", help="methods as compact pseudo-code with real local names (for reading logic)")
    p.add_argument("classes", nargs="+"); p.add_argument("--client", action="store_true")
    p.add_argument("--method", metavar="REGEX", help="only methods whose signature matches"); p.set_defaults(fn=cmd_pseudo)

    import gamedata  # noqa: E402
    gamedata.register(sub)
    import worldgen  # noqa: E402
    worldgen.register(sub)

    from gen import cmd_generate, cmd_check, cmd_setup  # noqa: E402  (kept separate: it is the part that changes per version)
    p = sub.add_parser("generate", help="emit the tables from the cache, and their lock"); p.set_defaults(fn=cmd_generate)
    p = sub.add_parser("check", help="are the generated tables what their lock says? (reads nothing else)")
    p.add_argument("--quiet", action="store_true", help="say nothing when they are"); p.set_defaults(fn=cmd_check)
    p = sub.add_parser("setup", help="RUN every step a fresh clone needs: fetch, reports, blockdata, gamedata, worldgen, generate")
    p.set_defaults(fn=cmd_setup)

    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    main()
