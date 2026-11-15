#!/usr/bin/env python3
"""What does a server make of the text and NBT in an item's components? Record it, on vanilla
and on ours, and diff.

    component_probe.py record <port> <out.json> [nameA nameB]
    component_probe.py diff <vanilla.json> <ours.json>

A text component has many spellings and a server passes none of them on: it decodes what a
client sent and encodes it again. `record` joins two bots (creative): B puts one stack after
another into its hand, each with one component spelled one way - sensible, odd or wrong; A
writes down the stack it is then shown in B's hand (set_equipment), which is the server's own
encoding. A stack B is thrown out for is written down as that, and B comes back under another
name. `diff` compares two recordings case by case and exits 0 if nothing is wrong.

The reference is the vanilla server, run under the rules in AGENTS.md (see entity_probe.py for
the properties it needs; gamemode=creative matters here):

    cd .cache/vanilla-server && /usr/lib/jvm/java-25-openjdk/bin/java -jar ../mc/26.3/server.jar --nogui
    tools/vanilla/component_probe.py record 25577 /tmp/vanilla.json      # then stop the server
    build/release/craftc --server-ip=127.0.0.1 --server-port=25580 --online-mode=false --view-distance=2 &
    tools/vanilla/component_probe.py record 25580 /tmp/ours.json
    tools/vanilla/component_probe.py diff /tmp/vanilla.json /tmp/ours.json

Reading the diff - per case one of:
  same              the two servers sent the same bytes
  same value        the same tags in another order: a compound that is data (custom_data, entity
                    data) leaves vanilla in the order of its HashMap and ours sorted by name
  not read here     vanilla took it, ours refused the stack (sent the slot back): a part of the
                    component this server does not read yet, or more than a stack may carry here
  refused here      vanilla threw the player out, ours only refused the stack: harmless
  default kept      vanilla dropped the component because it says what the item says anyway (an
                    empty lore); a patch is not held against the item's defaults here - a known
                    difference, see docs/architecture.md
  DIFFERENT         the bug this probe is for: other bytes, or one took what the other threw out

Without names the bots get random ones: vanilla saves players, and one it knows comes back
holding what it held.
"""
from __future__ import annotations

import json
import random
import string
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tests" / "integration"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import mcproto as mc  # noqa: E402
from inventory_probe import Actor  # noqa: E402
from mcproto import Byte, Double, Float, IntArray, Long, LongArray, Short, nbt, varint  # noqa: E402

PIG, CHEST = mc.ENTITY_TYPE["kPig"], 1   # an entity type; a block entity type (any that exists)


# ---- NBT by hand, for what mc.nbt() would not write: repeated names, wrappers, wrong strings

def raw_string(b: bytes) -> bytes:
    return struct.pack(">H", len(b)) + b


def compound(*entries: tuple) -> bytes:
    """A compound's payload from (name, tag) pairs - a tag being a whole nameless tag, as nbt() makes."""
    return b"".join(tag[:1] + raw_string(name.encode()) + tag[1:] for name, tag in entries) + b"\x00"


def record_of(*entries: tuple) -> bytes:
    return b"\x0a" + compound(*entries)


def list_of(of: int, *payloads: bytes) -> bytes:
    return b"\x09" + bytes([of]) + struct.pack(">i", len(payloads)) + b"".join(payloads)


def utf8(s: str) -> bytes:
    return mc.mc_str(s)


def cases() -> list[tuple[str, str, bytes]]:
    """(what it shows, component, the value as a client sends it)"""
    out: list[tuple[str, str, bytes]] = []

    def name(label: str, value) -> None:
        out.append((label, "kCustomName", nbt(value)))

    # ---- plain text and its spellings
    name("a string", "hi")
    name("an empty string", "")
    name("cyrillic and an emoji", "Меч \U0001F600")
    name("{text}", {"text": "hi"})
    name("{type, text}", {"type": "text", "text": "hi"})
    name("{text} with unknown keys", {"text": "hi", "unknown": "x", "another": 5})
    name("{text} twice", record_of(("text", nbt("first")), ("text", nbt("hi"))))
    name("{text: ''}", {"text": ""})
    out.append(("item_name is read the same", "kItemName", nbt({"text": "hi", "color": "red"})))

    # ---- style, and the order of a record's entries
    name("text + color", {"text": "hi", "color": "red"})
    name("color + text", {"color": "red", "text": "hi"})
    name("italic, bold, text", {"italic": Byte(0), "bold": Byte(1), "text": "hi"})
    name("text, italic false", {"text": "hi", "italic": False})
    name("empty text, bold", {"text": "", "bold": True})
    everything = {"text": "t", "extra": ["e"], "color": "gold", "shadow_color": -2146360781, "bold": True, "italic": True,
                  "underlined": True, "strikethrough": True, "obfuscated": True,
                  "click_event": {"action": "copy_to_clipboard", "value": "v"},
                  "hover_event": {"action": "show_text", "value": "h"}, "insertion": "ins", "font": "minecraft:alt"}
    name("every field: 13 entries", everything)
    name("12 entries", {k: v for k, v in everything.items() if k != "extra"} | {"shadow_color": 1})
    for n in (2, 3, 5, 8, 11):
        name(f"the first {n} style fields", {"text": "t"} | dict(list(everything.items())[2:2 + n]))
    name("translatable with everything", {"translate": "k", "fallback": "f", "with": ["a"]} | dict(list(everything.items())[1:]))

    # ---- lists
    name("[a]", ["a"])
    name("[[[a]]]", [[["a"]]])
    name("[a, b, c]", ["a", "b", "c"])
    name("[{a, extra: [x]}, b]", [{"text": "a", "extra": ["x"]}, "b"])
    name("[[a, b], [c, d]]", [["a", "b"], ["c", "d"]])
    name("extra of a string and a record", {"text": "a", "extra": ["b", {"text": "c", "bold": True}]})
    name("extra of two plain records", {"text": "a", "extra": [{"text": "b"}, {"text": "c"}]})
    name("an empty list", nbt([]))
    name("a list of a number", [1])
    name("an empty extra", record_of(("text", nbt("a")), ("extra", list_of(0))))
    name("extra that is a string", {"text": "a", "extra": "b"})
    name("extra: [5]", {"text": "a", "extra": [5]})

    # ---- contents
    name("keybind", {"keybind": "key.jump"})
    name("typed keybind", {"type": "keybind", "keybind": "key.jump"})
    name("translate", {"translate": "block.minecraft.stone"})
    name("keybind + translate + text", {"keybind": "k", "translate": "t", "text": "x"})
    name("keybind + translate, text a number", {"keybind": "k", "translate": "t", "text": 5})
    name("keybind, translate a number", {"keybind": "k", "translate": 5})
    name("keybind + translate with a bad with", {"keybind": "k", "translate": "t", "with": "not a list"})
    name("keybind + translate with {} in with", {"keybind": "k", "translate": "t", "with": [{}]})
    name("translate with a bad with", {"translate": "t", "with": "not a list"})
    name("typed translatable, only text", {"type": "translatable", "text": "x"})
    name("typed text, only translate", {"type": "text", "translate": "t"})
    name("typed translatable + text", {"type": "translatable", "translate": "t", "text": "x"})
    name("a type that is none", {"type": "nope", "text": "x"})
    name("a type that is a number", {"type": 1, "text": "x"})
    name("{}", {})
    name("{bold} alone", {"bold": True})
    name("{'': x} at the root", {"": "x"})
    name("a number", 5)
    name("a byte", Byte(1))
    name("an empty byte array", mc.ByteArray(b""))
    name("an int array", IntArray((5,)))

    # ---- translatable arguments
    name("with: mixed", {"fallback": "fb", "translate": "key",
                         "with": ["s", 7, Float(2.5), {"text": "x", "bold": True}, ["y"], {"text": "z"}]})
    name("with: ints", {"translate": "k", "with": [1, 2]})
    name("with: an int array", {"translate": "k", "with": IntArray((1, 2))})
    name("with: a byte array", {"translate": "k", "with": mc.ByteArray(b"\x01\x02")})
    name("with: a long array", {"translate": "k", "with": LongArray((1, 2))})
    name("with: strings", {"translate": "k", "with": ["a", "b"]})
    name("with: numbers of every type", {"translate": "k", "with": [Byte(1), Short(2), 3, Long(4), Float(5), Double(6)]})
    name("with: a styled record", {"translate": "k", "with": [{"text": "a", "bold": False}]})
    name("with: empty", record_of(("translate", nbt("k")), ("with", list_of(0))))
    name("with: an empty long array", {"translate": "k", "with": LongArray(())})
    name("fallback a number", {"translate": "k", "fallback": 5})
    name("with: a list of int arrays", {"translate": "k", "with": [IntArray(())]})
    name("with: a record that is no component", {"translate": "k", "with": [{"x": 1}]})
    name("translate + color + extra", {"translate": "k", "color": "aqua", "extra": ["e"]})

    # ---- colours
    for c in ("black", "dark_blue", "dark_green", "dark_aqua", "dark_red", "dark_purple", "gold", "gray", "dark_gray", "blue",
              "green", "aqua", "red", "light_purple", "yellow", "white", "#FF5555", "#ff5555", "#f", "#000000000abc", "#+ff",
              "#-0", "#FFFFFF", "#1000000", "#-1", "#", "#+", "#12345G", "#FFFFFFFFFFFFFFFFFFFF", "", "Red", "reset", "#１"):
        name(f"color {c!r}", {"text": "t", "color": c})
    name("color a number", {"text": "t", "color": 0xFF5555})

    # ---- flags and the shadow colour: any number
    for label, v in (("1b", Byte(1)), ("0b", Byte(0)), ("-128b", Byte(-128)), ("256", 256), ("2^32 L", Long(1 << 32)),
                     ("0.25f", Float(0.25)), ("-0.0f", Float(-0.0)), ("0.0d", Double(0.0)), ("NaN", Float(float("nan"))),
                     ("a string", "true"), ("a list", [Byte(1)])):
        name(f"bold {label}", {"text": "t", "bold": v})
    for label, v in (("an int", -16777216), ("-1b", Byte(-1)), ("2^32+2 L", Long((1 << 32) + 2)), ("2.9f", Float(2.9)),
                     ("-1e300", Double(-1e300)), ("a string", "#FFFFFF"), ("four floats", [Float(1.0), Float(0.5), Float(0.25), Float(1.0)]),
                     ("three floats", [Float(1.0), Float(0.5), Float(0.25)])):
        name(f"shadow_color {label}", {"text": "t", "shadow_color": v})

    # ---- insertion and font
    name("insertion", {"text": "t", "insertion": "typed"})
    name("insertion a number", {"text": "t", "insertion": 1})
    name("font without a namespace", {"text": "t", "font": "uniform"})
    name("font with one", {"text": "t", "font": "my:fonts/big"})
    name("font in capitals", {"text": "t", "font": "Uniform"})
    name("font a byte", {"text": "t", "font": Byte(1)})

    # ---- click events
    def click(label: str, event) -> None:
        name(f"click: {label}", {"text": "t", "click_event": event})
    click("open_url", {"url": "https://example.com/a-b_c/d.html?x=1&y=%20#top", "action": "open_url"})
    click("open_url, capitals and a port", {"action": "open_url", "url": "HTTP://127.0.0.1:8080"})
    click("open_url, odd but plain", {"action": "open_url", "url": "https://-.-:1/~a;b(c)'!*,$+=&@:/?q/?"})
    click("run_command", {"action": "run_command", "command": "/say hi"})
    click("suggest_command, empty", {"action": "suggest_command", "command": ""})
    click("copy_to_clipboard with a section sign", {"action": "copy_to_clipboard", "value": "§x"})
    click("change_page", {"action": "change_page", "page": 3})
    click("change_page 2.9f", {"action": "change_page", "page": Float(2.9)})
    click("change_page with a stray url", {"action": "change_page", "page": 3, "url": "ignored"})
    click("change_page 0", {"action": "change_page", "page": 0})
    click("change_page a string", {"action": "change_page", "page": "1"})
    click("run_command with a section sign", {"action": "run_command", "command": "a§b"})
    click("run_command with DEL", {"action": "run_command", "command": "a\x7f"})
    click("run_command with a newline", {"action": "run_command", "command": "a\nb"})
    click("run_command without a command", {"action": "run_command"})
    click("open_url, url a number", {"action": "open_url", "url": 1})
    click("open_file", {"action": "open_file", "path": "/etc/passwd"})
    click("no such action", {"action": "explode"})
    click("no action", {"url": "https://example.com"})
    name("click: not a record", {"text": "t", "click_event": "open_url"})
    click("open_url with a space", {"action": "open_url", "url": "https://example.com/a b"})
    click("open_url, IPv6", {"action": "open_url", "url": "https://[::1]/"})
    click("open_url, a bad escape", {"action": "open_url", "url": "https://example.com/%zz"})
    click("open_url, two fragments", {"action": "open_url", "url": "https://example.com/#a#b"})
    click("open_url, ftp", {"action": "open_url", "url": "ftp://example.com"})
    click("open_url, no host", {"action": "open_url", "url": "https://"})
    click("custom", {"action": "custom", "id": "a:b"})

    # ---- hover events
    def hover(label: str, event) -> None:
        name(f"hover: {label}", {"text": "t", "hover_event": event})
    hover("show_text, a string", {"value": "tip", "action": "show_text"})
    hover("show_text, a plain record", {"action": "show_text", "value": {"text": "tip"}})
    hover("show_text, a styled record", {"action": "show_text", "value": {"text": "tip", "color": "green"}})
    hover("show_text, a list", {"action": "show_text", "value": ["a", "b"]})
    hover("show_text without a value", {"action": "show_text"})
    hover("show_text, a number", {"action": "show_text", "value": 1})
    hover("no such action", {"action": "show_nothing", "value": "x"})
    hover("no action", {"value": "x"})
    hover("show_item", {"action": "show_item", "id": "minecraft:stone"})
    hover("show_entity", {"action": "show_entity", "id": "minecraft:pig", "uuid": IntArray((1, 2, 3, 4))})

    # ---- contents this server does not read
    name("selector", {"selector": "@a"})
    name("score", {"score": {"name": "x", "objective": "y"}})
    name("nbt", {"nbt": "a.b", "entity": "@s"})
    name("object: an atlas sprite", {"object": "atlas", "sprite": "minecraft:block/stone"})
    name("sprite alone", {"sprite": "minecraft:block/stone"})
    name("typed selector", {"type": "selector", "selector": "@a"})
    name("selector + text", {"selector": "@a", "text": "a"})
    name("selector + keybind", {"selector": "@a", "keybind": "k"})
    name("[a, {selector}]", ["a", {"selector": "@a"}])

    # ---- strings Java would write differently, tags that are none
    name("a raw NUL in a string", b"\x08" + raw_string(b"a\x00b"))
    name("'A' in two bytes", b"\x08" + raw_string(b"\xc1\x81"))
    name("a name in two bytes: 'text'", b"\x0a\x08" + raw_string(b"te\xc1\xb8t") + raw_string(b"hi") + b"\x00")
    name("half a tag", b"\x08\x00\x05hi")
    name("TAG_End", b"\x00")
    name("a string that is not modified UTF-8", b"\x08" + raw_string(b"\x80"))
    name("513 lists deep", b"\x09" + b"\x09\x00\x00\x00\x01" * 512 + b"\x00\x00\x00\x00\x00")
    name("40 lists deep", b"\x09" + b"\x09\x00\x00\x00\x01" * 39 + b"\x08\x00\x00\x00\x01" + raw_string(b"a"))
    name("20 lists deep", b"\x09" + b"\x09\x00\x00\x00\x01" * 19 + b"\x08\x00\x00\x00\x01" + raw_string(b"a"))

    # ---- lore
    out.append(("lore: two lines", "kLore", varint(2) + nbt("one") + nbt({"text": "two", "color": "gray"})))
    out.append(("lore: none", "kLore", varint(0)))
    out.append(("lore: a line that is a number", "kLore", varint(1) + nbt(Byte(1))))
    out.append(("lore: 257 lines", "kLore", varint(257) + nbt("") * 257))
    out.append(("lore: 256 lines", "kLore", varint(256) + nbt("") * 256))

    # ---- tags that stay tags
    data = record_of(("uses", nbt(3)), ("owner", nbt("Notch")), ("nested", nbt({"z": Byte(1), "a": Byte(2)})), ("uses", nbt(4)),
                     ("tags", list_of(10, compound(("", nbt("x"))))), ("mixed", nbt(["a", 1, {"k": Byte(1)}])),
                     ("longs", nbt(LongArray((1, 2)))), ("empty", list_of(8)), ("wrapped", list_of(10, compound(("", nbt({"": Byte(9)}))))))
    out.append(("custom_data", "kCustomData", data))
    out.append(("custom_data: {}", "kCustomData", nbt({})))
    out.append(("custom_data: one entry", "kCustomData", nbt({"a": 1})))
    out.append(("custom_data: SNBT in a string", "kCustomData", nbt("{a:1}")))
    out.append(("custom_data: a number", "kCustomData", nbt(1)))
    out.append(("custom_data: a list", "kCustomData", nbt([])))
    out.append(("custom_data: TAG_End", "kCustomData", b"\x00"))
    out.append(("bucket_entity_data", "kBucketEntityData", nbt({"NoAI": Byte(1), "Health": Float(3.0)})))
    out.append(("bucket_entity_data: a string", "kBucketEntityData", nbt("{a:1}")))
    out.append(("entity_data", "kEntityData", varint(PIG) + nbt({"CustomName": "Bob", "NoAI": Byte(1)})))
    out.append(("entity_data: no such type", "kEntityData", varint(mc.ENTITY_TYPE["kCount"]) + nbt({})))
    out.append(("entity_data: not a compound", "kEntityData", varint(PIG) + nbt("x")))
    out.append(("block_entity_data", "kBlockEntityData", varint(CHEST) + nbt({"Lock": "key"})))
    out.append(("block_entity_data: no such type", "kBlockEntityData", varint(200) + nbt({})))

    # ---- books
    out.append(("writable book", "kWritableBookContent",
                varint(2) + utf8("page one") + b"\x00" + utf8("page two") + b"\x01" + utf8("p*** two")))
    out.append(("writable book: no pages", "kWritableBookContent", varint(0)))
    out.append(("writable book: a page of 900", "kWritableBookContent", varint(1) + utf8("x" * 900) + b"\x00"))
    out.append(("writable book: a page of 1024", "kWritableBookContent", varint(1) + utf8("x" * 1024) + b"\x00"))
    out.append(("writable book: a page of 1025", "kWritableBookContent", varint(1) + utf8("x" * 1025) + b"\x00"))
    out.append(("writable book: 101 pages", "kWritableBookContent", varint(101) + (utf8("") + b"\x00") * 101))
    out.append(("writable book: a page that is not UTF-8", "kWritableBookContent", varint(1) + varint(1) + b"\xff" + b"\x00"))
    page = nbt({"text": "Once", "bold": True})
    out.append(("written book", "kWrittenBookContent",
                utf8("Title") + b"\x01" + utf8("T****") + utf8("Author") + varint(2) + varint(2) + page + b"\x00" +
                nbt("The end") + b"\x01" + nbt("The ***") + b"\x07"))

    def titled(title: str, generation: int) -> bytes:
        return utf8(title) + b"\x00" + utf8("A") + varint(generation) + varint(0) + b"\x00"
    out.append(("written book: a title of 32", "kWrittenBookContent", titled("t" * 32, 3)))
    out.append(("written book: a title of 33", "kWrittenBookContent", titled("t" * 33, 0)))
    out.append(("written book: a title of 16 emoji", "kWrittenBookContent", titled("\U0001F600" * 16, 0)))
    out.append(("written book: a title of 16 emoji and a letter", "kWrittenBookContent", titled("\U0001F600" * 16 + "x", 0)))
    out.append(("written book: generation 4", "kWrittenBookContent", titled("t", 4)))
    out.append(("written book: generation -1", "kWrittenBookContent", titled("t", -1)))

    # ---- signs
    lines = nbt("a") + nbt("b") + nbt("") + nbt("")
    out.append(("sign text", "kSignTextFront", lines + b"\x00" + varint(4) + b"\x01"))
    out.append(("sign text: a dye that is none, glow 9", "kSignTextBack", lines + b"\x00" + varint(99) + b"\x09"))
    out.append(("sign text: filtered lines that say the same", "kSignTextFront",
                lines + b"\x01" + nbt({"text": "a"}) + nbt("b") + nbt("") + nbt("") + varint(0) + b"\x00"))
    out.append(("sign text: filtered lines that differ", "kSignTextFront",
                lines + b"\x01" + nbt("a") + nbt("*") + nbt("") + nbt("") + varint(0) + b"\x00"))
    out.append(("sign text: three lines", "kSignTextFront", nbt("a") + nbt("b") + nbt("") + b"\x00" + varint(0) + b"\x00"))
    return out


def record(port: int, out_path: str, name_a: str, name_b: str) -> None:
    a = Actor(port, name_a)
    a.join()
    a.pump(1.5)
    a.c.send("player_loaded")
    a.tick_end()
    a.pump(0.5)

    joined = 0

    def join_b() -> Actor:
        nonlocal joined
        b = Actor(port, name_b if joined == 0 else f"{name_b[:12]}{joined}")
        joined += 1
        b.join()
        b.pump(1.0)
        b.c.send("player_loaded")
        b.tick_end()
        for _ in range(10):
            b.pump(0.05)
            a.pump(0.05)
        return b

    def shown(since: int, eid: int) -> str | None:
        """The stack A was last shown in B's main hand: the bytes after the entity id and the slot."""
        head = varint(eid) + b"\x00"   # EquipmentSlot.MAINHAND, and no further slot
        for kind, data in reversed(a.log[since:]):
            if kind == "set_equipment" and bytes.fromhex(data).startswith(head):
                return data[2 * len(head):]
        return None

    b = join_b()
    results = []
    items = ("kStone", "kDirt")   # in turn, so that every stack differs from the one before it
    for i, (label, component, value) in enumerate(cases()):
        mark_a, mark_b = len(a.log), len(b.log)
        b.give(36, items[i % 2], 1, {component: value})
        kicked = False
        try:
            for _ in range(5):
                b.tick_end()
                b.pump(0.05)
                a.pump(0.01)
        except (OSError, EOFError, AssertionError, struct.error):
            kicked = True
        kicked = kicked or any(kind == "disconnect" for kind, _ in b.log[mark_b:])
        a.pump(0.1)
        result = {"case": label, "component": component, "sent": value.hex()}
        if kicked:
            result["kicked"] = True
            b.c.close()
            a.pump(0.3)
            b = join_b()
        else:
            result["stack"] = shown(mark_a, b.entity_id)
            result["sent_back"] = any(kind == "container_set_slot" for kind, _ in b.log[mark_b:])
        results.append(result)
        print(f"{label}: {'kicked' if kicked else result['stack'] or ('sent back' if result['sent_back'] else 'nothing')}")
    with open(out_path, "w") as f:
        json.dump(results, f, indent=1)
    b.c.close()
    a.c.close()


def diff(path_ref: str, path_ours: str) -> int:
    ref, ours = json.load(open(path_ref)), json.load(open(path_ours))
    assert [r["case"] for r in ref] == [r["case"] for r in ours], "the two recordings are of different cases"
    wrong = 0
    counts: dict[str, int] = {}
    for v, o in zip(ref, ours):
        if v.get("kicked") and o.get("kicked"):
            verdict = "same"
        elif v.get("kicked"):
            verdict = "refused here" if not o["stack"] else "DIFFERENT"
        elif o.get("kicked") or not v["stack"]:
            verdict = "DIFFERENT"   # ...or vanilla showed nothing, which no case here is meant to do
        elif not o["stack"]:
            verdict = "not read here"
        elif v["stack"] == o["stack"]:
            verdict = "same"
        else:
            def parsed(stack_hex: str):
                r = mc.Reader(bytes.fromhex(stack_hex))
                stack = mc.read_item(r)
                r.done()
                return stack
            theirs, here = parsed(v["stack"]), parsed(o["stack"])
            if json.dumps(theirs, sort_keys=True, default=repr) == json.dumps(here, sort_keys=True, default=repr):
                verdict = "same value"
            elif theirs[:2] == here[:2] and not theirs[2] and list(here[2]) == [o["component"]]:
                verdict = "default kept"
            else:
                verdict = "DIFFERENT"
        counts[verdict] = counts.get(verdict, 0) + 1
        if verdict == "same":
            continue
        wrong += verdict == "DIFFERENT"
        print(f"{verdict}: {v['case']} [{v['component']}]")
        if verdict in ("DIFFERENT", "same value", "default kept"):
            print(f"    sent    {v['sent']}")
            print(f"    vanilla {'kicked' if v.get('kicked') else v['stack']}")
            print(f"    ours    {'kicked' if o.get('kicked') else o['stack']}")
    print(", ".join(f"{n} {k}" for k, n in sorted(counts.items())))
    return 1 if wrong else 0


def main() -> int:
    if len(sys.argv) >= 4 and sys.argv[1] == "record":
        tail = "".join(random.choices(string.ascii_lowercase, k=5))
        names = (sys.argv[4], sys.argv[5]) if len(sys.argv) >= 6 else (f"Watch{tail}", f"Hold{tail}")
        record(int(sys.argv[2]), sys.argv[3], *names)
        return 0
    if len(sys.argv) == 4 and sys.argv[1] == "diff":
        return diff(sys.argv[2], sys.argv[3])
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
