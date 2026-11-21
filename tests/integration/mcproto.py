# SPDX-License-Identifier: GPL-3.0-or-later
"""A deliberately independent Minecraft protocol client for integration tests (stdlib only).

Nothing here shares code with the server: wire primitives, framing, zlib, AES-CFB8 (libcrypto
through ctypes) and RSA are re-implemented, and packets are addressed by *name* using the id
tables parsed from the generated header. If the server and this client agree, a framing, crypto
or compression bug on either side would have to be mirrored exactly to go unnoticed.

It does NOT prove a packet layout matches the vanilla client - layouts are checked against the
vanilla jar when they are written (see the mc-reference skill).
"""
from __future__ import annotations

import ctypes
import ctypes.util
import hashlib
import os
import re
import socket
import struct
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


# ----------------------------------------------------------------------------- generated ids

def _gen_dir() -> Path:
    cur = (ROOT / "src/proto/gen/current.hpp").read_text()
    return ROOT / "src" / re.search(r'#define PROTO_GEN_DIR "([^"]+)"', cur).group(1)


def load_ids() -> dict:
    """{'play': {'cb': {'login': 0x32, ...}, 'sb': {...}}, ...} from packet_ids.hpp."""
    text = (_gen_dir() / "packet_ids.hpp").read_text()
    out: dict = {}
    for state, body in re.findall(r"\nnamespace (\w+) \{\n(.*?)\n\}  // namespace \1\n", text, re.S):
        if state in ("cb", "sb"):
            continue
        for d, inner in re.findall(r"namespace (cb|sb) \{\n(.*?)\}  // namespace \1", body, re.S):
            names = re.findall(r'^\s+"([^"]+)",$', inner, re.M)
            out.setdefault(state, {})[d] = {n: i for i, n in enumerate(names)}
    return out


def load_meta() -> dict:
    text = (_gen_dir() / "meta.hpp").read_text()
    return {
        "protocol": int(re.search(r"kProtocolVersion = (\d+)", text).group(1)),
        "version": re.search(r'kVersionName = "([^"]+)"', text).group(1),
        "world_version": int(re.search(r"kWorldVersion = (\d+)", text).group(1)),
    }


def _constants(header: str, namespace: str) -> dict[str, int]:
    """{'kStone': 1, ...} from one namespace of a generated header."""
    text = (_gen_dir() / header).read_text()
    body = re.search(r"namespace %s \{(.*?)\}  // namespace %s" % (re.escape(namespace), re.escape(namespace)), text, re.S).group(1)
    return {m.group(1): int(m.group(2)) for m in re.finditer(r"inline constexpr unsigned (k\w+) = (\d+);", body)}


def load_registry(ns: str) -> list[str]:
    text = (_gen_dir() / "registries.hpp").read_text()
    body = re.search(r"\nnamespace %s \{.*?\n\}  // namespace %s\n" % (ns, ns), text, re.S).group(0)
    return re.findall(r'^\s+"([^"]+)",$', body, re.M)


IDS = load_ids()
META = load_meta()
BLOCK = _constants("blocks.hpp", "proto::gen::block")   # default block state id per block: BLOCK["kStone"]
ITEM = _constants("items.hpp", "proto::gen::item")      # item registry id: ITEM["kStone"]
ENTITY_TYPE = _constants("entities.hpp", "proto::gen::entity_type")  # ENTITY_TYPE["kPlayer"]
SERIALIZER = _constants("entities.hpp", "serializer")   # entity-data value types: SERIALIZER["kByte"]
POSE = _constants("entities.hpp", "proto::gen::pose")
# Entity-data accessor ids of a player, by the class that defines them.
DATA = {**_constants("entities.hpp", "entity"), **_constants("entities.hpp", "living_entity"),
        **_constants("entities.hpp", "avatar"), **_constants("entities.hpp", "player")}
ITEM_DATA = _constants("entities.hpp", "item_entity")   # ...and of an item entity: ITEM_DATA["kItem"]
COMPONENT = _constants("items.hpp", "proto::gen::component")  # data component type ids: COMPONENT["kDamage"]
EQUIPMENT_SLOT = _constants("entities.hpp", "proto::gen::equipment_slot")
SWING = _constants("entities.hpp", "proto::gen::swing_animation")   # SwingAnimationType ids: SWING["kWhack"]
TNT_DATA = _constants("entities.hpp", "primed_tnt")     # entity-data accessors of primed TNT: TNT_DATA["kFuse"]
FALLING_DATA = _constants("entities.hpp", "falling_block")   # ...of a falling block: FALLING_DATA["kStartPos"]
MENUS = _constants("static_ids.hpp", "menu")            # menu types: MENUS["kGeneric9x3"]
BLOCK_ENTITY = _constants("static_ids.hpp", "block_entity_type")
SOUND = _constants("static_ids.hpp", "sound")           # the sounds the server sends by itself: SOUND["kBlockChestOpen"]
PARTICLE = _constants("static_ids.hpp", "particle")
ARGUMENT = _constants("command_data.hpp", "argument")   # command argument types the server names: ARGUMENT["kVec3"]


def load_item_table() -> dict[str, dict]:
    """{'diamond_helmet': {'max_stack': 1, 'equip_sound': 68, 'equip_slot': 5, ...}} from the generated kItems table."""
    text = (_gen_dir() / "block_tables.hpp").read_text()
    body = re.search(r"ItemInfo kItems\[\d+\] = \{\n(.*?)\n\};", text, re.S).group(1)
    out = {}
    for m in re.finditer(r"\{(\d+), (\d+), (\d+), 0x([0-9A-F]+), (\d+), 0x([0-9A-F]+), (\d+), (\d+)\},  // (\w+)", body):
        block, sound, stack, flags, slot, swing, fuel, remainder, name = m.groups()
        out[name] = {"block": int(block), "equip_sound": int(sound), "max_stack": int(stack), "flags": int(flags, 16), "equip_slot": int(slot),
                     "attack_swing": (int(swing, 16) >> 6, int(swing, 16) & 0x3F),   # (SwingAnimationType, ticks)
                     "fuel_ticks": int(fuel), "remainder": int(remainder)}
    assert len(out) == ITEM["kCount"], (len(out), ITEM["kCount"])
    return out


ITEM_INFO = load_item_table()   # ITEM_INFO["diamond_helmet"]["equip_sound"]


# ----------------------------------------------------------------------------- wire primitives

def varint(n: int) -> bytes:
    n &= 0xFFFFFFFF
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def mc_str(s: str) -> bytes:
    raw = s.encode()
    return varint(len(raw)) + raw


class Reader:
    def __init__(self, data: bytes):
        self.d = data
        self.i = 0

    def take(self, n: int) -> bytes:
        if self.i + n > len(self.d):
            raise AssertionError(f"short packet: want {n} bytes at {self.i}, have {len(self.d)}")
        out = self.d[self.i:self.i + n]
        self.i += n
        return out

    def varint(self) -> int:
        v = 0
        for shift in range(0, 35, 7):
            b = self.take(1)[0]
            v |= (b & 0x7F) << shift
            if not b & 0x80:
                return v
        raise AssertionError("varint too long")

    def u8(self) -> int: return self.take(1)[0]
    def i8(self) -> int: return struct.unpack(">b", self.take(1))[0]
    def u32(self) -> int: return struct.unpack(">I", self.take(4))[0]
    def bool(self) -> bool: return self.take(1)[0] != 0
    def i16(self) -> int: return struct.unpack(">h", self.take(2))[0]
    def i32(self) -> int: return struct.unpack(">i", self.take(4))[0]
    def i64(self) -> int: return struct.unpack(">q", self.take(8))[0]
    def u64(self) -> int: return struct.unpack(">Q", self.take(8))[0]
    def f32(self) -> float: return struct.unpack(">f", self.take(4))[0]
    def f64(self) -> float: return struct.unpack(">d", self.take(8))[0]
    def svarint(self) -> int: return struct.unpack(">i", struct.pack(">I", self.varint()))[0]
    def str(self) -> str: return self.take(self.varint()).decode()
    def uuid(self) -> str: return self.take(16).hex()
    def byte_array(self) -> bytes: return self.take(self.varint())
    def long_array(self) -> list[int]: return [self.u64() for _ in range(self.varint())]
    def remaining(self) -> int: return len(self.d) - self.i

    def nbt_text(self) -> str:
        """A text component sent as a nameless TAG_String."""
        tag = self.u8()
        assert tag == 8, f"expected TAG_String, got tag {tag}"
        n = struct.unpack(">H", self.take(2))[0]
        return self.take(n).decode()

    def nbt(self):
        """One nameless tag (NbtIo.readAnyTag) as a Python value; None for TAG_End."""
        t = self.u8()
        return None if t == 0 else self._nbt_payload(t)

    def _nbt_payload(self, t: int):
        if t == 1: return Byte(self.i8())
        if t == 2: return Short(self.i16())
        if t == 3: return self.i32()
        if t == 4: return Long(self.i64())
        if t == 5: return Float(self.f32())
        if t == 6: return Double(self.f64())
        if t == 7: return ByteArray(self.take(self.i32()))
        if t == 8: return _mutf8_decode(self.take(struct.unpack(">H", self.take(2))[0]))
        if t == 9:
            of, n = self.u8(), self.i32()
            assert n >= 0 and (of != 0 or n == 0), (of, n)
            out = [self._nbt_payload(of) for _ in range(n)]
            # ListTag.addAndUnwrap: {"": x} in a list of compounds stands for x.
            return [x[""] if isinstance(x, dict) and list(x) == [""] else x for x in out] if of == 10 else out
        if t == 10:
            out = {}
            while True:
                of = self.u8()
                if of == 0:
                    return out
                name = _mutf8_decode(self.take(struct.unpack(">H", self.take(2))[0]))
                out[name] = self._nbt_payload(of)
        if t == 11: return IntArray(self.i32() for _ in range(self.i32()))
        if t == 12: return LongArray(self.i64() for _ in range(self.i32()))
        raise AssertionError(f"no such tag type: {t}")

    def done(self) -> None:
        assert self.i == len(self.d), f"{len(self.d) - self.i} trailing bytes"


# ----------------------------------------------------------------------------- NBT
#
# Tags as Python values, both ways, written from NbtIo and the TagTypes of the jar. A string is
# a str, a compound a dict, a list a list, TAG_Int an int; the other numbers and the arrays are
# the small classes below, so that a value says which tag it is. snbt() prints a value the way
# /data would, compounds sorted by name: the thing to compare.

class Byte(int): pass
class Short(int): pass
class Long(int): pass
class Float(float): pass
class Double(float): pass
class ByteArray(bytes): pass
class IntArray(tuple): pass
class LongArray(tuple): pass


def _mutf8_decode(b: bytes) -> str:
    """DataInput.readUTF: UTF-16 units in one, two or three bytes each."""
    units = []
    i = 0
    while i < len(b):
        c = b[i]
        if c < 0x80:
            units.append(c)
            i += 1
        elif c & 0xE0 == 0xC0:
            units.append((c & 0x1F) << 6 | (b[i + 1] & 0x3F))
            i += 2
        else:
            assert c & 0xF0 == 0xE0, f"not modified UTF-8: {b.hex()}"
            units.append((c & 0x0F) << 12 | (b[i + 1] & 0x3F) << 6 | (b[i + 2] & 0x3F))
            i += 3
    return struct.pack(f"<{len(units)}H", *units).decode("utf-16-le", "surrogatepass")


def _mutf8_encode(s: str) -> bytes:
    """DataOutput.writeUTF."""
    out = bytearray()
    raw = s.encode("utf-16-le", "surrogatepass")
    for (u,) in struct.iter_unpack("<H", raw):
        if 1 <= u <= 0x7F:
            out.append(u)
        elif u <= 0x7FF:
            out += bytes([0xC0 | u >> 6, 0x80 | u & 0x3F])
        else:
            out += bytes([0xE0 | u >> 12, 0x80 | (u >> 6) & 0x3F, 0x80 | u & 0x3F])
    assert len(out) <= 0xFFFF
    return struct.pack(">H", len(out)) + bytes(out)


def _nbt_type(v) -> int:
    if isinstance(v, bool): return 1
    for cls, t in ((Byte, 1), (Short, 2), (Long, 4), (Float, 5), (Double, 6), (ByteArray, 7), (IntArray, 11), (LongArray, 12)):
        if isinstance(v, cls): return t
    if isinstance(v, int): return 3
    if isinstance(v, float): return 6
    if isinstance(v, str): return 8
    if isinstance(v, (list, tuple)): return 9
    if isinstance(v, dict): return 10
    raise TypeError(f"no tag for {v!r}")


def _nbt_payload(v) -> bytes:
    t = _nbt_type(v)
    if t == 1: return struct.pack(">b", int(v))
    if t == 2: return struct.pack(">h", v)
    if t == 3: return struct.pack(">i", v)
    if t == 4: return struct.pack(">q", v)
    if t == 5: return struct.pack(">f", v)
    if t == 6: return struct.pack(">d", v)
    if t == 7: return struct.pack(">i", len(v)) + bytes(v)
    if t == 8: return _mutf8_encode(v)
    if t == 11: return struct.pack(f">i{len(v)}i", len(v), *v)
    if t == 12: return struct.pack(f">i{len(v)}q", len(v), *v)
    if t == 10:
        return b"".join(bytes([_nbt_type(x)]) + _mutf8_encode(k) + _nbt_payload(x) for k, x in v.items()) + b"\x00"
    # ListTag.write: the elements' type if they agree, else compounds - with everything that is
    # not a compound, and every compound that looks like such a wrapper, inside {"": x}.
    types = {_nbt_type(x) for x in v}
    of = 0 if not v else types.pop() if len(types) == 1 else 10

    def element(x):
        if of == 10 and (not isinstance(x, dict) or list(x) == [""]):
            return _nbt_payload({"": x})
        return _nbt_payload(x)
    return bytes([of]) + struct.pack(">i", len(v)) + b"".join(element(x) for x in v)


def nbt(v) -> bytes:
    """A value as a nameless tag - the network form. bytes pass through: a tag made by hand."""
    if isinstance(v, bytes) and not isinstance(v, ByteArray):
        return v
    return bytes([_nbt_type(v)]) + _nbt_payload(v)


def snbt(v) -> str:
    t = _nbt_type(v)
    if t == 10: return "{" + ",".join(f"{k}:{snbt(v[k])}" for k in sorted(v)) + "}"
    if t == 9: return "[" + ",".join(snbt(x) for x in v) + "]"
    if t == 8: return '"' + v.replace("\\", "\\\\").replace('"', '\\"') + '"'
    if t == 7: return "[B;" + ",".join(str(x if x < 128 else x - 256) for x in v) + "]"
    if t == 11: return "[I;" + ",".join(map(str, v)) + "]"
    if t == 12: return "[L;" + ",".join(map(str, v)) + "]"
    if t in (5, 6): return repr(float(v)) + "fd"[t - 5]
    return str(int(v)) + {1: "b", 2: "s", 3: "", 4: "L"}[t]


# ----------------------------------------------------------------------------- crypto

_crypto = ctypes.CDLL(ctypes.util.find_library("crypto") or "libcrypto.so.3")
_crypto.EVP_CIPHER_CTX_new.restype = ctypes.c_void_p
_crypto.EVP_aes_128_cfb8.restype = ctypes.c_void_p
_crypto.EVP_CipherInit_ex.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
_crypto.EVP_CipherUpdate.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_int), ctypes.c_char_p, ctypes.c_int]


class Cfb8:
    def __init__(self, key: bytes, encrypt: bool):
        self.ctx = _crypto.EVP_CIPHER_CTX_new()
        assert _crypto.EVP_CipherInit_ex(self.ctx, _crypto.EVP_aes_128_cfb8(), None, key, key, 1 if encrypt else 0) == 1

    def update(self, data: bytes) -> bytes:
        if not data:
            return b""
        out = ctypes.create_string_buffer(len(data) + 32)
        n = ctypes.c_int(0)
        assert _crypto.EVP_CipherUpdate(self.ctx, out, ctypes.byref(n), data, len(data)) == 1
        return out.raw[:n.value]


def _der_tlv(data: bytes, i: int) -> tuple[int, bytes, int]:
    tag = data[i]
    n = data[i + 1]
    i += 2
    if n & 0x80:
        k = n & 0x7F
        n = int.from_bytes(data[i:i + k], "big")
        i += k
    return tag, data[i:i + n], i + n


def rsa_public_from_der(spki: bytes) -> tuple[int, int]:
    _, outer, _ = _der_tlv(spki, 0)             # SEQUENCE
    _, _, i = _der_tlv(outer, 0)                # AlgorithmIdentifier
    _, bits, _ = _der_tlv(outer, i)             # BIT STRING (leading unused-bits byte)
    _, key, _ = _der_tlv(bits[1:], 0)           # SEQUENCE { n, e }
    _, n_raw, j = _der_tlv(key, 0)
    _, e_raw, _ = _der_tlv(key, j)
    return int.from_bytes(n_raw, "big"), int.from_bytes(e_raw, "big")


def rsa_encrypt_pkcs1(spki: bytes, msg: bytes) -> bytes:
    n, e = rsa_public_from_der(spki)
    k = (n.bit_length() + 7) // 8
    pad = bytes(b % 255 + 1 for b in os.urandom(k - 3 - len(msg)))
    em = b"\x00\x02" + pad + b"\x00" + msg
    return pow(int.from_bytes(em, "big"), e, n).to_bytes(k, "big")


def server_hash(server_id: str, secret: bytes, spki: bytes) -> str:
    digest = hashlib.sha1(server_id.encode() + secret + spki).digest()
    return format(int.from_bytes(digest, "big", signed=True), "x")


def offline_uuid(name: str) -> str:
    d = bytearray(hashlib.md5(("OfflinePlayer:" + name).encode()).digest())
    d[6] = (d[6] & 0x0F) | 0x30
    d[8] = (d[8] & 0x3F) | 0x80
    return d.hex()


# ----------------------------------------------------------------------------- connection

class Client:
    def __init__(self, host: str, port: int, timeout: float = 5.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = bytearray()
        self.threshold = -1
        self.enc = self.dec = None
        self.state = "handshake"
        self.stored = self.deflated = 0  # frame statistics, for assertions about compression
        # What a client knows about the entities around it, kept up to date by recv(): tests that
        # are not about entities never see an entity packet, tests that are look here.
        self.entity_id = None            # the player's own id, from the login packet
        self.entities: dict[int, Entity] = {}
        self.own_data: dict[int, tuple] = {}   # set_entity_data about the player itself
        self.entity_log: list[tuple] = []      # (packet name, entity id, details) in arrival order
        self.in_bundle = False
        self.ignore: set[str] = set()          # play packets recv() skips, for tests with bystanders
        # The player's own menu as the server has described it (InventoryMenu: 46 slots), kept up
        # to date by recv() like the entities. Stacks are (item, count, components) or None.
        self.slots: list = [None] * 46
        self.cursor = None
        self.state_id = 0                      # of the last container packet
        self.held_slot = None                  # from set_held_slot
        self.menu_log: list[tuple] = []        # (packet name, details) in arrival order
        self.sounds: list[tuple] = []          # (sound id, source, (x, y, z), volume, pitch)
        # The menu a block opened (open_screen), None while only the player's own is there:
        # {"id", "type", "title", "slots", "data"}. Its last 36 slots are the inventory - the
        # very slots 9..44 of self.slots, as in the client, where both menus show one Inventory.
        self.window = None
        self.block_events: list[tuple] = []    # ((x, y, z), b0, b1, block id)
        self.level_events: list[tuple] = []    # (type, (x, y, z), data, global)
        self.transient_blocks: list[tuple] = []  # add_transient_block: ((x, y, z), state)
        self.explosions: list[dict] = []
        # The chunks the client holds (ClientChunkCache), kept up to date by recv() like the
        # entities: position -> the chunk as parse_chunk() reads it. A batch is answered as a
        # client answers it (ClientPacketListener.handleChunkBatchFinished), asking for
        # chunks_per_tick more.
        self.chunks = ChunkCache()
        self.chunk_center = None               # set_chunk_cache_center
        self.chunk_log: list[tuple] = []       # ("center", x, z) | ("batch", [positions]) | ("forget", x, z) in arrival order
        self.chunks_per_tick = 64.0
        self._batch = None
        self._backlog: list[tuple] = []        # packets sync_chunks() read past: recv() hands them out first
        self.recipe_sets: dict[str, set] = {}  # update_recipes: property set name -> item ids
        self.permission_level = None           # entity event 24 + level about the player itself
        self.commands = None                   # the last `commands` packet, as parse_commands() returns it
        self.stopped_sounds: list[tuple] = []  # stop_sound: (source or None, sound name or None)
        # What the chat has shown, kept by recv() like the rest: ("system", component, overlay),
        # ("player", {...}) and ("disguised", {...}) in arrival order. chat_index is the client's
        # own count of the player_chat packets it was sent (ClientPacketListener.nextChatIndex):
        # the index it expects the next one to carry.
        self.chat: list[tuple] = []
        self.chat_index = 0
        self._chat_read = 0                    # recv_pending_chat(): how far self.chat has been handed out

    def close(self) -> None:
        self.sock.close()

    # -- raw frames
    def send_raw(self, data: bytes) -> None:
        self.sock.sendall(self.enc.update(data) if self.enc else data)

    def frame(self, name: str, payload: bytes = b"") -> bytes:
        body = varint(IDS[self.state]["sb"][name]) + payload
        if self.threshold >= 0:
            if len(body) >= self.threshold:
                body = varint(len(body)) + zlib.compress(body)
            else:
                body = b"\x00" + body
        return varint(len(body)) + body

    def send(self, name: str, payload: bytes = b"") -> None:
        self.send_raw(self.frame(name, payload))

    def send_together(self, *packets: tuple) -> None:
        """Several packets, (name, payload) or (name,), in one write: a server reads them in one
        tick, which separate writes only do nearly always."""
        self.send_raw(b"".join(self.frame(*p) for p in packets))

    def _fill(self) -> None:
        chunk = self.sock.recv(65536)
        if not chunk:
            raise ConnectionError("connection closed by server")
        self.buf += self.dec.update(bytes(chunk)) if self.dec else chunk

    def recv(self) -> tuple[str, Reader]:
        """The next packet that is not about an entity; those are applied to self.entities."""
        while True:
            if self._backlog:
                name, r = self._backlog.pop(0)
                if name in self.ignore:
                    continue
                return name, r
            name, r = self.recv_any()
            if self.state != "play":
                return name, r
            if name in ENTITY_PACKETS:
                self.apply_entity_packet(name, r)
            elif name in MENU_PACKETS:
                self.apply_menu_packet(name, r)
            elif name in CHUNK_PACKETS:
                self.apply_chunk_packet(name, r)
            elif name in CHAT_PACKETS:
                self.apply_chat_packet(name, r)
            elif name not in self.ignore:
                return name, r

    def apply_chat_packet(self, name: str, r: Reader) -> None:
        """handleSystemChat / handlePlayerChat / handleDisguisedChat, as far as a client checks
        what it is sent before it shows anything."""
        if name == "system_chat":
            self.chat.append(("system", r.nbt(), r.bool()))
        elif name == "disguised_chat":
            self.chat.append(("disguised", {"message": r.nbt(), **read_chat_type(r)}))
        else:
            # A client counts the player_chat packets it gets, from 0; a server that counts
            # otherwise is left at once (multiplayer.disconnect.bad_chat_index).
            index = r.varint()
            assert index == self.chat_index, f"player_chat with global index {index}, the client is at {self.chat_index}"
            self.chat_index += 1
            m = {"sender": r.uuid(), "index": r.varint(), "signature": r.take(256) if r.bool() else None,
                 "content": r.str(), "timestamp": r.i64(), "salt": r.i64()}
            assert len(m["content"].encode("utf-16-le")) // 2 <= 256, "content longer than SignedMessageBody takes"
            seen = r.varint()
            assert seen <= 20, f"{seen} last seen messages"
            m["last_seen"] = [r.take(256) if (packed := r.varint()) == 0 else packed - 1 for _ in range(seen)]
            m["unsigned"] = r.nbt() if r.bool() else None
            mask = r.varint()                  # FilterMask: 0 passes everything
            assert mask == 0, f"filter mask type {mask}"
            m.update(read_chat_type(r))
            self.chat.append(("player", m))
        r.done()

    def mark_chat_read(self) -> None:
        """Everything in self.chat has been looked at: recv_pending_chat() begins after it."""
        self._chat_read = len(self.chat)

    def said(self, since: int = 0) -> list:
        """The system messages in self.chat[since:], without the ones about who came and went."""
        return [m[1] for m in self.chat[since:] if m[0] == "system" and not is_announcement(m[1])]

    def apply_chunk_packet(self, name: str, r: Reader) -> None:
        """What ClientPacketListener does with the packets that bring and take chunks."""
        def signed(v: int) -> int:
            return struct.unpack(">i", struct.pack(">I", v & 0xFFFFFFFF))[0]
        if name == "set_chunk_cache_center":
            self.chunk_center = (signed(r.varint()), signed(r.varint()))
            r.done()
            self.chunk_log.append(("center", *self.chunk_center))
        elif name == "chunk_batch_start":
            r.done()
            assert self._batch is None, "a chunk batch inside a chunk batch"
            self._batch = []
        elif name == "level_chunk_with_light":
            assert self._batch is not None, "a chunk outside a batch"
            pos = struct.unpack(">ii", r.d[r.i:r.i + 8])
            assert pos not in self.chunks, f"chunk {pos} sent twice"
            self.chunks.put(pos, bytes(r.d[r.i:]))
            self._batch.append(pos)
        elif name == "chunk_batch_finished":
            assert self._batch is not None and r.varint() == len(self._batch), "chunk_batch_finished does not count its batch"
            r.done()
            self.chunk_log.append(("batch", self._batch))
            self._batch = None
            self.send("chunk_batch_received", struct.pack(">f", self.chunks_per_tick))
        else:  # forget_level_chunk: a ChunkPos as one long, z in the high half
            packed = r.u64()
            r.done()
            pos = (signed(packed), signed(packed >> 32))
            self.chunks.drop(pos)
            self.chunk_log.append(("forget", *pos))

    def sync_chunks(self, distance: int, center: tuple | None = None, ticks: int = 600) -> None:
        """Wait until the chunks held are exactly the view (ChunkTrackingView) at this view distance
        around chunk_center - which has to be `center`, if one is given. Whatever else arrives
        meanwhile is kept for recv(), in order."""
        def settled() -> bool:
            return (self.chunk_center is not None and (center is None or self.chunk_center == tuple(center))
                    and set(self.chunks) == view_chunks(*self.chunk_center, distance))
        seen = []
        backlog, self._backlog = self._backlog, []
        try:
            for i in range(ticks):
                if settled():
                    return
                self.send("ping_request", struct.pack(">q", 0xC4000 + i))
                while True:
                    name, r = self.recv()
                    if name == "pong_response" and r.d[r.i:r.i + 8] == struct.pack(">q", 0xC4000 + i):
                        break
                    if name == "keep_alive":
                        self.send("keep_alive", r.take(8))
                    else:
                        seen.append((name, r))
        finally:
            self._backlog = backlog + seen
        want = view_chunks(*self.chunk_center, distance) if self.chunk_center else set()
        raise AssertionError(f"the view never settled around {center or self.chunk_center}: {len(self.chunks)} chunks held, "
                             f"{len(want)} wanted; missing {sorted(want - set(self.chunks))[:8]}, "
                             f"extra {sorted(set(self.chunks) - want)[:8]}")

    def ticks(self, n: int) -> None:
        """Let at least n - 1 whole server ticks pass, and receive everything they sent.

        A ping is answered in the tick that reads it, ahead of that tick's entity updates; so
        after the n-th answer everything up to the tick before it has arrived."""
        for i in range(n):
            self.send("ping_request", struct.pack(">q", 0x7E570000 + i))
            assert self.expect("pong_response").i64() == 0x7E570000 + i

    def pump(self, seconds: float = 0.0) -> int:
        """Apply every entity packet that is already here or arrives within `seconds`; answer
        keep-alives; drop the rest. For clients that only watch. Returns the packets read."""
        import time
        n = 0
        end = time.time() + seconds
        old = self.sock.gettimeout()
        try:
            while True:
                self.sock.settimeout(max(end - time.time(), 0.0005))
                try:
                    name, r = self.recv_any()
                except (socket.timeout, BlockingIOError):
                    return n
                n += 1
                if name in ENTITY_PACKETS:
                    self.apply_entity_packet(name, r)
                elif name in MENU_PACKETS:
                    self.apply_menu_packet(name, r)
                elif name in CHUNK_PACKETS:
                    self.apply_chunk_packet(name, r)
                elif name in CHAT_PACKETS:
                    self.apply_chat_packet(name, r)
                elif name == "keep_alive":
                    self.send("keep_alive", r.take(8))
        finally:
            self.sock.settimeout(old)

    def recv_pending_chat(self) -> list:
        """The system messages that have come since this was last asked - their text components,
        without the ones about who came and went. Block changes and the tab list are passed
        over; whatever else is waiting is an error."""
        self.send("ping_request", struct.pack(">q", 0x5A1D))
        while True:
            name, r = self.recv()
            if name == "pong_response":
                assert r.i64() == 0x5A1D
                said = self.said(self._chat_read)
                self.mark_chat_read()
                return said
            assert name in ("block_update", "section_blocks_update", "light_update", "block_changed_ack", "player_info_update",
                            "player_info_remove"), f"expected only chat, got {name}"

    def recv_any(self) -> tuple[str, Reader]:
        while True:
            length = shift = 0
            n = 0
            complete = False
            while n < len(self.buf) and n < 3:
                b = self.buf[n]
                length |= (b & 0x7F) << shift
                shift += 7
                n += 1
                if not b & 0x80:
                    complete = True
                    break
            if complete and len(self.buf) >= n + length:
                break
            if not complete and n >= 3:
                raise AssertionError("frame length varint longer than 3 bytes")
            self._fill()
        frame = bytes(self.buf[n:n + length])
        del self.buf[:n + length]
        if self.threshold >= 0:
            r = Reader(frame)
            usize = r.varint()
            if usize == 0:
                self.stored += 1
                body = frame[r.i:]
                # A vanilla client takes a stored frame of any size (CompressionDecoder); a vanilla
                # server never sends one that reaches the threshold, and neither does ours - but
                # for player_chat, each recipient's own frame of one message, which it leaves as it is.
                assert len(body) < self.threshold or (self.state == "play" and body[0] == IDS["play"]["cb"]["player_chat"]), \
                    f"stored body of {len(body)} bytes >= threshold {self.threshold}"
            else:
                self.deflated += 1
                assert usize >= self.threshold, f"deflated body of {usize} bytes < threshold {self.threshold}"
                body = zlib.decompress(frame[r.i:])
                assert len(body) == usize
        else:
            body = frame
        r = Reader(body)
        pid = r.varint()
        names = IDS[self.state]["cb"]
        name = next((k for k, v in names.items() if v == pid), None)
        assert name is not None, f"unknown {self.state} packet id {pid:#x}"
        return name, r

    def expect(self, name: str) -> Reader:
        got, r = self.recv()
        # A scenario that runs for more than 15 seconds meets a keep-alive on the way: answered,
        # as a client does, unless it is the keep-alive that the scenario is about.
        while got == "keep_alive" and name != "keep_alive" and self.state == "play":
            self.send("keep_alive", r.take(8))
            got, r = self.recv()
        assert got == name, f"expected {self.state}/{name}, got {got}"
        return r

    def expect_closed(self) -> None:
        """The server must close the connection; a read timeout is a failure, not a close."""
        try:
            while True:
                self._fill()
        except socket.timeout:
            raise AssertionError("server kept the connection open") from None
        except (ConnectionError, OSError):
            return

    # -- flows
    def handshake(self, intent: int, protocol: int | None = None, host: str = "localhost", port: int = 25565) -> None:
        self.send("intention", varint(META["protocol"] if protocol is None else protocol) + mc_str(host) +
                  struct.pack(">H", port) + varint(intent))
        self.state = {1: "status", 2: "login", 3: "login"}[intent]

    def login(self, name: str, online: bool = False) -> dict:
        """Run login up to and including login_acknowledged. Returns the profile the server sent."""
        self.handshake(2)
        self.send("hello", mc_str(name) + bytes(16))
        got, r = self.recv()
        info: dict = {"encrypted": False}
        if got == "hello":
            assert online, "server asked for encryption in offline mode"
            server_id, spki, challenge, should_auth = r.str(), r.byte_array(), r.byte_array(), r.bool()
            r.done()
            secret = os.urandom(16)
            info.update(encrypted=True, should_authenticate=should_auth, hash=server_hash(server_id, secret, spki))
            self.send("key", varint(128) + rsa_encrypt_pkcs1(spki, secret) + varint(128) + rsa_encrypt_pkcs1(spki, challenge))
            self.enc, self.dec = Cfb8(secret, True), Cfb8(secret, False)
            got, r = self.recv()
        if got == "login_compression":
            self.threshold = r.varint()
            r.done()
            got, r = self.recv()
        if got == "login_disconnect":
            raise ConnectionRefusedError(r.str())
        assert got == "login_finished", got
        info["uuid"], info["name"] = r.uuid(), r.str()
        info["props"] = []
        for _ in range(r.varint()):
            pname, value = r.str(), r.str()
            info["props"].append((pname, value, r.str() if r.bool() else None))
        info["session_id"] = r.uuid()
        r.done()
        self.send("login_acknowledged")
        self.state = "configuration"
        return info

    def configure(self, view_distance: int = 10, known_packs: list | None = None, chat: int = 0) -> dict:
        """Run the configuration phase. Returns what the server sent. `chat`: ChatVisiblity - 0
        everything, 1 system messages only, 2 nothing."""
        cfg: dict = {"registries": {}, "tags": None}
        r = self.expect("custom_payload")
        assert r.str() == "minecraft:brand"
        cfg["brand"] = r.str()
        r.done()
        r = self.expect("update_enabled_features")
        cfg["features"] = [r.str() for _ in range(r.varint())]
        r.done()
        r = self.expect("select_known_packs")
        cfg["known_packs"] = [(r.str(), r.str(), r.str()) for _ in range(r.varint())]
        r.done()

        self.send("client_information", mc_str("en_us") + struct.pack(">b", view_distance) + varint(chat) + b"\x01" +
                  b"\x7f" + varint(1) + b"\x00\x01" + varint(0))
        self.send("custom_payload", mc_str("minecraft:brand") + mc_str("vanilla"))
        packs = cfg["known_packs"] if known_packs is None else known_packs
        self.send("select_known_packs", varint(len(packs)) + b"".join(mc_str(a) + mc_str(b) + mc_str(c) for a, b, c in packs))

        while True:
            got, r = self.recv()
            if got == "registry_data":
                key = r.str()
                entries = []
                for _ in range(r.varint()):
                    entries.append(r.str())
                    assert not r.bool(), f"{key}: unexpected inline NBT"
                r.done()
                assert key not in cfg["registries"], f"{key} sent twice"
                cfg["registries"][key] = entries
            elif got == "update_tags":
                tags = {}
                for _ in range(r.varint()):
                    key = r.str()
                    tags[key] = {}
                    for _ in range(r.varint()):
                        tname = r.str()
                        tags[key][tname] = [r.varint() for _ in range(r.varint())]
                r.done()
                cfg["tags"] = tags
            elif got == "finish_configuration":
                r.done()
                break
            elif got == "disconnect":
                raise ConnectionRefusedError(r.nbt_text())
            else:
                raise AssertionError(f"unexpected configuration packet {got}")
        self.send("finish_configuration")
        self.state = "play"
        return cfg


# ----------------------------------------------------------------------------- entities
#
# The client side of entity tracking, written from the vanilla client's handlers
# (ClientPacketListener.handleAddEntity / handleMoveEntity / handleEntityPositionSync / ...,
# VecDeltaCodec, LpVec3) - not from the server's encoders.

ENTITY_PACKETS = {"bundle_delimiter", "add_entity", "remove_entities", "set_entity_data", "move_entity_pos",
                  "move_entity_pos_rot", "move_entity_rot", "entity_position_sync", "rotate_head", "set_entity_motion",
                  "set_equipment", "take_item_entity", "swing_animation", "entity_event", "teleport_entity"}
CHUNK_PACKETS = {"set_chunk_cache_center", "chunk_batch_start", "level_chunk_with_light", "chunk_batch_finished",
                 "forget_level_chunk"}
CHAT_PACKETS = {"system_chat", "player_chat", "disguised_chat"}


def read_chat_type(r: "Reader") -> dict:
    """ChatType.Bound: the type as a holder (its id in minecraft:chat_type + 1; a type spelled out
    in the packet is nothing a vanilla server sends), the sender's name, a target's if any."""
    holder = r.varint()
    assert holder > 0, "a chat type that is not a registry entry"
    return {"type": holder - 1, "name": r.nbt(), "target": r.nbt() if r.bool() else None}


def is_announcement(component) -> bool:
    """"Steve joined the game", "Steve left the game": said to everyone, whatever they are doing."""
    return isinstance(component, dict) and str(component.get("translate", "")).startswith("multiplayer.player.")


def view_chunks(cx: int, cz: int, distance: int) -> set:
    """ChunkTrackingView.Positioned: the chunks a player at chunk (cx, cz) is shown with this view
    distance - isWithinDistance(..., includeNeighbors = true): a square one chunk wider than the
    distance, with its corners cut."""
    return {(x, z) for x in range(cx - distance - 1, cx + distance + 2) for z in range(cz - distance - 1, cz + distance + 2)
            if max(0, abs(x - cx) - 2) ** 2 + max(0, abs(z - cz) - 2) ** 2 < distance * distance}


class ChunkCache(dict):
    """position -> chunk. A chunk is kept as its packet's bytes and parsed when it is asked for:
    a scenario joins with hundreds of chunks and looks at two."""

    def __init__(self):
        super().__init__()
        self._parsed = {}

    def put(self, pos: tuple, packet: bytes) -> None:
        super().__setitem__(pos, packet)
        self._parsed.pop(pos, None)

    def drop(self, pos: tuple) -> None:
        super().pop(pos, None)
        self._parsed.pop(pos, None)

    def raw(self, pos: tuple) -> bytes:
        return super().__getitem__(pos)

    def __getitem__(self, pos: tuple) -> dict:
        if pos not in self._parsed:
            self._parsed[pos] = parse_chunk(Reader(super().__getitem__(pos)))
        return self._parsed[pos]

    def values(self):
        return [self[k] for k in self]

    def items(self):
        return [(k, self[k]) for k in self]


MENU_PACKETS = {"container_set_content", "container_set_slot", "set_cursor_item", "set_player_inventory", "set_held_slot",
                "sound", "open_screen", "container_set_data", "container_close", "update_recipes", "block_event", "explode",
                "commands", "stop_sound", "level_event", "add_transient_block"}
# Slots of the menus a block opens, by menu type (the menu registry): the container's own, then
# the 36 of the inventory. From the constructors of ChestMenu, AbstractFurnaceMenu, CraftingMenu.
WINDOW_SLOTS = {"kGeneric9x3": 27, "kGeneric9x6": 54, "kFurnace": 3, "kCrafting": 10}


# ----------------------------------------------------------------------------- item stacks
#
# ItemStack.OPTIONAL_STREAM_CODEC as a client reads it. A component's value has no length on
# this side of the wire, so reading a stack means knowing the layout of every component in it:
# the readers below are written from the components' StreamCodecs in the jar, one by one.

def _effect_details(r: Reader) -> dict:
    """MobEffectInstance.Details.STREAM_CODEC"""
    d = {"amplifier": r.varint(), "duration": r.svarint(), "ambient": r.bool(), "particles": r.bool(), "icon": r.bool()}
    if r.bool():
        d["hidden"] = _effect_details(r)
    return d


def _potion_contents(r: Reader) -> dict:
    out = {"potion": r.varint() if r.bool() else None, "color": r.i32() if r.bool() else None,
           "effects": [(r.varint(), _effect_details(r)) for _ in range(r.varint())]}
    out["name"] = r.str() if r.bool() else None
    return out


def _firework_explosion(r: Reader) -> dict:
    return {"shape": r.varint(), "colors": [r.i32() for _ in range(r.varint())], "fade": [r.i32() for _ in range(r.varint())],
            "trail": r.bool(), "twinkle": r.bool()}


def _holder_reference(r: Reader) -> int:
    """ByteBufCodecs.holder: id + 1; 0 would be an inline value, which the server never sends."""
    v = r.varint()
    assert v != 0, "an inline holder"
    return v - 1


def _filterable(r: Reader, value):
    """Filterable.streamCodec: (raw, filtered or None)."""
    raw = value(r)
    return (raw, value(r) if r.bool() else None)


def _sign_text(r: Reader) -> dict:
    """SignText.STREAM_CODEC: four lines, perhaps four filtered ones, the dye, the glow."""
    return {"messages": [r.nbt() for _ in range(4)], "filtered": [r.nbt() for _ in range(4)] if r.bool() else None,
            "color": r.varint(), "glowing": r.bool()}


def _written_book(r: Reader) -> dict:
    return {"title": _filterable(r, Reader.str), "author": r.str(), "generation": r.varint(),
            "pages": [_filterable(r, Reader.nbt) for _ in range(r.varint())], "resolved": r.bool()}


_UNIT = lambda r: True                                        # noqa: E731
_DYE = lambda r: r.varint()                                   # noqa: E731
COMPONENT_READERS = {
    "kMaxStackSize": Reader.varint, "kMaxDamage": Reader.varint, "kDamage": Reader.varint, "kRepairCost": Reader.varint,
    "kOminousBottleAmplifier": Reader.varint, "kMapId": Reader.svarint, "kRarity": Reader.varint,
    "kUnbreakable": _UNIT, "kGlider": _UNIT, "kWaxed": _UNIT,
    "kEnchantmentGlintOverride": Reader.bool, "kMinimumAttackCharge": Reader.f32, "kPotionDurationScale": Reader.f32,
    "kDyedColor": Reader.i32,
    "kDye": _DYE, "kBaseColor": _DYE, "kWolfCollar": _DYE, "kCatCollar": _DYE, "kSheepColor": _DYE, "kShulkerColor": _DYE,
    "kTropicalFishBaseColor": _DYE, "kTropicalFishPatternColor": _DYE, "kCushionColor": _DYE,
    "kItemModel": Reader.str, "kTooltipStyle": Reader.str, "kNoteBlockSound": Reader.str,
    "kEnchantments": lambda r: {r.varint(): r.varint() for _ in range(r.varint())},
    "kStoredEnchantments": lambda r: {r.varint(): r.varint() for _ in range(r.varint())},
    "kTooltipDisplay": lambda r: (r.bool(), [r.varint() for _ in range(r.varint())]),
    "kPotionContents": _potion_contents,
    "kSuspiciousStewEffects": lambda r: [(r.varint(), r.svarint()) for _ in range(r.varint())],
    "kFireworkExplosion": _firework_explosion,
    "kFireworks": lambda r: (r.varint(), [_firework_explosion(r) for _ in range(r.varint())]),
    "kInstrument": _holder_reference, "kPaintingVariant": _holder_reference,
    "kBannerPatterns": lambda r: [(_holder_reference(r), r.varint()) for _ in range(r.varint())],
    "kBlockState": lambda r: {r.str(): r.str() for _ in range(r.varint())},
    # Text and NBT: a text component is a tag (ComponentSerialization.STREAM_CODEC), read here as
    # the tag it is - what it means is the client's business, what it is on the wire is the test's.
    "kCustomName": Reader.nbt, "kItemName": Reader.nbt,
    "kLore": lambda r: [r.nbt() for _ in range(r.varint())],
    "kCustomData": Reader.nbt, "kBucketEntityData": Reader.nbt,
    "kEntityData": lambda r: (r.varint(), r.nbt()), "kBlockEntityData": lambda r: (r.varint(), r.nbt()),
    "kWritableBookContent": lambda r: [_filterable(r, Reader.str) for _ in range(r.varint())],
    "kWrittenBookContent": _written_book,
    "kSignTextFront": _sign_text, "kSignTextBack": _sign_text,
}
_COMPONENT_NAME = {v: k for k, v in COMPONENT.items() if k != "kCount"}


def read_item(r: Reader):
    """None for an empty stack, else (item id, count, components): {name: value}, None = removed."""
    count = r.varint()
    if count == 0:
        return None
    item = r.varint()
    added, removed = r.varint(), r.varint()
    comps: dict = {}
    for _ in range(added):
        name = _COMPONENT_NAME[r.varint()]
        assert name in COMPONENT_READERS, f"component {name} is not known to the test client"
        assert name not in comps, f"component {name} twice in one stack"
        comps[name] = COMPONENT_READERS[name](r)
    for _ in range(removed):
        name = _COMPONENT_NAME[r.varint()]
        assert name not in comps, f"component {name} both set and removed"
        comps[name] = None
    return (item, count, comps)


def _apply_menu_packet(self: "Client", name: str, r: Reader) -> None:
    """ClientPacketListener.handleContainerContent / handleContainerSetSlot / handleSetCursorItem /
    handleSetPlayerInventory / handleSetHeldSlot / handleOpenScreen / handleContainerSetData /
    handleContainerClose, for the player's own menu (container id 0) and the one a block opened;
    and the packets about the world around a menu: handleBlockEvent, handleExplosion,
    handleUpdateRecipes, handleSoundEvent."""
    def window_slot(slot: int, stack) -> None:
        w = self.window
        w["slots"][slot] = stack
        if slot >= w["base"]:  # the inventory part: one Inventory behind both menus
            self.slots[9 + slot - w["base"]] = stack

    def own_slot(slot: int, stack) -> None:
        self.slots[slot] = stack
        if self.window and 9 <= slot < 45:
            self.window["slots"][self.window["base"] + slot - 9] = stack

    if name == "container_set_content":
        cid, state = r.varint(), r.varint()
        items = [read_item(r) for _ in range(r.varint())]
        carried = read_item(r)
        if cid == 0:
            assert len(items) == 46, len(items)
            for slot, stack in enumerate(items):
                own_slot(slot, stack)
        else:
            assert self.window and cid == self.window["id"] and len(items) == len(self.window["slots"]), (cid, len(items))
            for slot, stack in enumerate(items):
                window_slot(slot, stack)
        self.cursor, self.state_id = carried, state
        self.menu_log.append((name, state))
    elif name == "container_set_slot":
        cid, state, slot = r.varint(), r.varint(), r.i16()
        stack = read_item(r)
        if cid == 0:
            assert 0 <= slot < 46, slot
            own_slot(slot, stack)
        else:
            assert self.window and cid == self.window["id"] and 0 <= slot < len(self.window["slots"]), (cid, slot)
            window_slot(slot, stack)
        self.state_id = state
        self.menu_log.append((name, slot, stack))
    elif name == "set_cursor_item":
        self.cursor = read_item(r)
        self.menu_log.append((name, self.cursor))
    elif name == "set_player_inventory":
        index = r.varint()
        stack = read_item(r)
        # Inventory index -> menu slot: 0..8 hotbar, 9..35 as is, 36..39 feet..head, 40 off hand.
        slot = 36 + index if index < 9 else index if index < 36 else 8 - (index - 36) if index < 40 else 45
        assert index <= 40, index
        own_slot(slot, stack)
        self.menu_log.append((name, index, stack))
    elif name == "set_held_slot":
        self.held_slot = r.varint()
        self.menu_log.append((name, self.held_slot))
    elif name == "open_screen":
        cid, kind = r.varint(), r.varint()
        title = r.nbt()
        types = {v: k for k, v in MENUS.items()}
        assert 1 <= cid <= 100 and types.get(kind) in WINDOW_SLOTS, (cid, kind)
        base = WINDOW_SLOTS[types[kind]]
        # A new AbstractContainerMenu on the client: empty slots over the one inventory.
        self.window = {"id": cid, "type": types[kind], "title": title, "base": base, "data": {},
                       "slots": [None] * base + self.slots[9:45]}
        self.menu_log.append((name, cid, types[kind], title))
    elif name == "container_set_data":
        cid, index, value = r.varint(), r.i16(), r.i16()
        assert self.window and cid == self.window["id"], cid
        self.window["data"][index] = value
        self.menu_log.append((name, index, value))
    elif name == "container_close":
        cid = r.varint()
        assert self.window and cid == self.window["id"], (cid, self.window)
        self.window = None
        self.menu_log.append((name, cid))
    elif name == "update_recipes":
        for _ in range(r.varint()):
            key = r.str()
            self.recipe_sets[key] = {r.varint() for _ in range(r.varint())}
        self.stonecutter_recipes = r.varint()
        if self.stonecutter_recipes:
            r.take(r.remaining())  # SelectableRecipe entries: slot displays, which this client does not read
    elif name == "block_event":
        pos = unpack_block_pos(r.u64())
        self.block_events.append((pos, r.u8(), r.u8(), r.varint()))
    elif name == "level_event":   # ClientboundLevelEventPacket: int type, BlockPos, int data, boolean globalEvent
        kind = r.i32()
        self.level_events.append((kind, unpack_block_pos(r.u64()), r.i32(), r.bool()))
    elif name == "add_transient_block":   # ClientboundAddTransientBlockPacket: BlockPos, the state's id
        pos = unpack_block_pos(r.u64())
        self.transient_blocks.append((pos, r.varint()))
    elif name == "explode":
        e = {"center": (r.f64(), r.f64(), r.f64()), "radius": r.f32(), "blocks": r.i32()}
        e["knockback"] = (r.f64(), r.f64(), r.f64()) if r.bool() else None
        e["particle"] = r.varint()
        holder = r.varint()
        assert holder != 0, "an inline sound event"
        e["sound"] = holder - 1
        e["block_particles"] = [(r.varint(), r.f32(), r.f32(), r.varint()) for _ in range(r.varint())]
        e["play_sound"] = r.bool()
        self.explosions.append(e)
    elif name == "sound":
        # Holder<SoundEvent>: a registry id + 1, or 0 and the event itself (its name, and a
        # range if it has a fixed one) - then the sound is listed by its name.
        holder = r.varint()
        sound = holder - 1
        if holder == 0:
            sound = r.str()
            assert not r.bool(), "a sound event with a fixed range"
        self.sounds.append((sound, r.varint(), (r.i32() / 8, r.i32() / 8, r.i32() / 8), r.f32(), r.f32()))
        r.i64()  # seed
    elif name == "commands":
        self.commands = parse_commands(r)
        return
    elif name == "stop_sound":   # ClientboundStopSoundPacket: bit 0 a source follows, bit 1 a sound
        flags = r.u8()
        source = r.varint() if flags & 1 else None
        self.stopped_sounds.append((source, r.str() if flags & 2 else None))
    r.done()


Client.apply_menu_packet = _apply_menu_packet


def java_round(v: float) -> int:
    """Math.round: nearest, ties towards positive infinity."""
    import math
    return math.floor(v + 0.5)


def pack_degrees(angle: float) -> int:
    """Mth.packDegrees as a signed byte. Float arithmetic, as the server must do it."""
    import math
    f32 = lambda v: struct.unpack(">f", struct.pack(">f", v))[0]  # noqa: E731
    b = math.floor(f32(f32(f32(angle) * 256.0) / 360.0)) & 0xFF
    return b - 256 if b >= 128 else b


def read_lp_vec3(r: Reader) -> tuple[float, float, float]:
    """LpVec3.read"""
    lowest = r.u8()
    if lowest == 0:
        return (0.0, 0.0, 0.0)
    middle = r.u8()
    buffer = (r.u32() << 16) | (middle << 8) | lowest
    scale = lowest & 3
    if lowest & 4:
        scale |= r.varint() << 2
    unpack = lambda v: min(v & 32767, 32766) * 2.0 / 32766.0 - 1.0  # noqa: E731
    return (unpack(buffer >> 3) * scale, unpack(buffer >> 18) * scale, unpack(buffer >> 33) * scale)


def read_entity_data(r: Reader) -> dict[int, tuple]:
    """{accessor id: (serializer id, value)} up to the 0xFF end marker."""
    out = {}
    while True:
        index = r.u8()
        if index == 0xFF:
            return out
        ser = r.varint()
        if ser == SERIALIZER["kByte"]:
            value = r.u8()
        elif ser == SERIALIZER["kFloat"]:
            value = r.f32()
        elif ser == SERIALIZER["kBoolean"]:
            value = r.bool()
        elif ser in (SERIALIZER["kInt"], SERIALIZER["kPose"], SERIALIZER["kHumanoidArm"]):
            value = r.varint()
        elif ser == SERIALIZER["kItemStack"]:
            value = read_item(r)
        elif ser == SERIALIZER["kBlockPos"]:
            value = unpack_block_pos(r.u64())
        else:
            raise AssertionError(f"entity data of serializer {ser} is not known to the test client")
        assert index not in out, f"entity data {index} twice in one packet"
        out[index] = (ser, value)


class Entity:
    """One entity as a client holds it."""

    def __init__(self, eid: int, uuid: str, etype: int, pos: tuple, velocity: tuple, pitch: int, yaw: int, head: int, data: int):
        self.id, self.uuid, self.type = eid, uuid, etype
        self.base = pos          # VecDeltaCodec base: what deltas are relative to
        self.pos = pos
        self.velocity = velocity
        self.spawned = (pos, velocity)   # as add_entity gave them
        self.yaw, self.pitch = yaw * 360 / 256, pitch * 360 / 256   # degrees, as unpacked or as sent in full
        self.head = head         # packed
        self.on_ground = False
        self.object_data = data
        self.data: dict[int, tuple] = {}
        self.bundled = False     # arrived between two bundle delimiters
        self.steps: list[int] = []   # tick counts of the path steps received, in order
        self.equipment: dict[int, tuple] = {}   # EquipmentSlot ordinal -> stack, for the slots that hold something

    def delta(self, xa: int, ya: int, za: int) -> None:
        """VecDeltaCodec.decode, then the result becomes the new base."""
        if xa or ya or za:
            self.base = tuple(b if d == 0 else (java_round(b * 4096.0) + d) / 4096.0 for b, d in zip(self.base, (xa, ya, za)))
        self.pos = self.base


def _apply_entity_packet(self: "Client", name: str, r: Reader) -> None:
    log = self.entity_log
    if name == "bundle_delimiter":
        r.done()
        self.in_bundle = not self.in_bundle
        log.append((name, None, self.in_bundle))
        return
    if name == "remove_entities":
        ids = [r.varint() for _ in range(r.varint())]
        r.done()
        for eid in ids:
            assert eid in self.entities, f"remove_entities for {eid}, which this client was never shown"
            del self.entities[eid]
            log.append((name, eid, None))
        return
    if name == "entity_event":   # the one entity packet whose id is a plain int
        eid, event = r.i32(), r.u8()
        r.done()
        assert eid == self.entity_id or eid in self.entities, f"entity_event for unknown entity {eid}"
        if 24 <= event <= 28 and eid == self.entity_id:   # LocalPlayer.handleEntityEvent: its permission level
            self.permission_level = event - 24
        if event == 55 and eid in self.entities:   # LivingEntity.handleEntityEvent: swap the hands
            eq = self.entities[eid].equipment
            main, off = eq.pop(0, None), eq.pop(1, None)
            if off is not None:
                eq[0] = off
            if main is not None:
                eq[1] = main
        log.append((name, eid, event))
        return
    eid = r.varint()
    if name == "take_item_entity":   # handleTakeItemEntity
        collector, amount = r.varint(), r.varint()
        r.done()
        assert eid in self.entities, f"take_item_entity for unknown entity {eid}"
        assert collector == self.entity_id or collector in self.entities, f"collector {collector} is unknown"
        log.append((name, eid, (collector, amount)))
        return
    if name == "swing_animation":
        hand, kind, duration = r.varint(), r.varint(), r.varint()
        r.done()
        assert eid == self.entity_id or eid in self.entities, f"swing_animation for unknown entity {eid}"
        log.append((name, eid, (hand, kind, duration)))
        return
    if name == "set_equipment":      # handleSetEquipment
        assert eid in self.entities, f"set_equipment for unknown entity {eid}"
        changed = {}
        while True:
            slot = r.u8()
            assert slot & 0x7F < 8, f"equipment slot {slot & 0x7F}"
            assert slot & 0x7F not in changed, "an equipment slot twice in one packet"
            changed[slot & 0x7F] = read_item(r)
            if not slot & 0x80:
                break
        r.done()
        for slot, stack in changed.items():
            if stack is None:
                self.entities[eid].equipment.pop(slot, None)
            else:
                self.entities[eid].equipment[slot] = stack
        log.append((name, eid, changed))
        return
    if name == "add_entity":
        e = Entity(eid, r.uuid(), r.varint(), (r.f64(), r.f64(), r.f64()), read_lp_vec3(r), r.i8(), r.i8(), r.i8(), r.varint())
        r.done()
        assert eid != self.entity_id, "the player was shown itself"
        assert eid not in self.entities, f"add_entity for {eid}, which this client already has"
        e.bundled = self.in_bundle
        self.entities[eid] = e
        log.append((name, eid, e.pos))
        return
    if name == "set_entity_data":
        data = read_entity_data(r)
        r.done()
        if eid == self.entity_id:
            self.own_data.update(data)
        else:
            assert eid in self.entities, f"set_entity_data for unknown entity {eid}"
            self.entities[eid].data.update(data)
        log.append((name, eid, data))
        return
    assert eid in self.entities, f"{name} for unknown entity {eid}"
    e = self.entities[eid]
    if name in ("move_entity_pos", "move_entity_pos_rot"):
        props = r.varint()
        steps = props >> 1
        if steps:
            for _ in range(steps):
                e.steps.append(r.varint())
                e.delta(r.i16(), r.i16(), r.i16())
        else:
            e.delta(r.i16(), r.i16(), r.i16())
        if name == "move_entity_pos_rot":
            e.yaw, e.pitch = r.i8() * 360 / 256, r.i8() * 360 / 256
        e.on_ground = bool(props & 1)
        log.append((name, eid, (steps, e.pos)))
    elif name == "move_entity_rot":
        e.on_ground = r.bool()
        e.yaw, e.pitch = r.i8() * 360 / 256, r.i8() * 360 / 256
        log.append((name, eid, (e.yaw, e.pitch)))
    elif name == "entity_position_sync":
        kind = r.varint()
        assert kind in (0, 1), f"position path type {kind}"
        if kind == 0:
            pos = (r.f64(), r.f64(), r.f64())
        else:
            count = r.varint()
            assert count >= 1
            for _ in range(count):
                pos = (r.f64(), r.f64(), r.f64())
                e.steps.append(r.varint())
        e.base = e.pos = pos
        e.yaw, e.pitch = r.f32(), r.f32()
        e.on_ground = r.bool()
        log.append((name, eid, (kind, pos)))
    elif name == "rotate_head":
        e.head = r.i8()
        log.append((name, eid, e.head))
    elif name == "set_entity_motion":
        e.velocity = read_lp_vec3(r)
        log.append((name, eid, e.velocity))
    elif name == "teleport_entity":
        # ClientPacketListener.setValuesFromPositionPacket: a PositionMoveRotation (position,
        # velocity, angles) and which of its parts are offsets from what the entity has - a set
        # of Relative as an int. The base that deltas count from is not touched.
        pos, delta = (r.f64(), r.f64(), r.f64()), (r.f64(), r.f64(), r.f64())
        yaw, pitch, relative = r.f32(), r.f32(), r.i32()
        assert relative == 0, f"teleport_entity with relative parts {relative:#x}: not modelled"
        e.pos, e.velocity, e.yaw, e.pitch = pos, delta, yaw, pitch
        e.on_ground = r.bool()
        log.append((name, eid, (pos, delta)))
    r.done()


Client.apply_entity_packet = _apply_entity_packet


# ----------------------------------------------------------------------------- play-state parsers

_ARGUMENT_NAME = {v: k for k, v in ARGUMENT.items()}


def parse_commands(r: Reader) -> dict:
    """ClientboundCommandsPacket as a tree: {"type", "name", "executable", "restricted", "children":
    {name: node}} and, for an argument, "parser" (a key of ARGUMENT), "properties", "suggestions";
    for a node that redirects, "redirect": the name of the root's child it goes to.

    Per node: a flags byte (bits 0-1 root / literal / argument, 4 executable, 8 redirects, 16 has
    suggestions, 32 restricted), the indices of its children, the index it redirects to, then
    its name and - an argument - its parser by id with that parser's properties."""
    nodes = []
    for _ in range(r.varint()):
        flags = r.u8()
        node = {"type": ("root", "literal", "argument")[flags & 3], "executable": bool(flags & 4),
                "restricted": bool(flags & 32), "child_ids": [r.varint() for _ in range(r.varint())]}
        if flags & 8:
            node["redirect_id"] = r.varint()
        if flags & 3:
            node["name"] = r.str()
        if flags & 3 == 2:
            parser = _ARGUMENT_NAME.get(r.varint())
            assert parser is not None, "a command argument type this client cannot read"
            node["parser"] = parser
            props = {}
            if parser in ("kInteger", "kFloat", "kDouble"):   # ArgumentUtils.createNumberFlags: 1 min, 2 max
                bounds = r.u8()
                read = {"kInteger": r.i32, "kFloat": r.f32, "kDouble": r.f64}[parser]
                if bounds & 1:
                    props["min"] = read()
                if bounds & 2:
                    props["max"] = read()
            elif parser == "kString":
                props["type"] = ("word", "phrase", "greedy")[r.varint()]
            elif parser == "kEntity":   # bit 0: one at most, bit 1: players only
                mask = r.u8()
                props = {"amount": "single" if mask & 1 else "multiple", "type": "players" if mask & 2 else "entities"}
            elif parser == "kResource":
                props["registry"] = r.str()
            node["properties"] = props
            if flags & 16:
                node["suggestions"] = r.str()
        nodes.append(node)
    root = nodes[r.varint()]
    r.done()
    assert root["type"] == "root"
    for node in nodes:
        node["children"] = {nodes[i]["name"]: nodes[i] for i in node.pop("child_ids")}
        if "redirect_id" in node:
            node["redirect"] = nodes[node.pop("redirect_id")].get("name", "")   # "" is the root (execute run)
    return root


def parse_login(r: Reader) -> dict:
    p = {"entity_id": r.i32(), "hardcore": r.bool(), "dimensions": [r.str() for _ in range(r.varint())],
         "max_players": r.varint(), "view_distance": r.varint(), "simulation_distance": r.varint(),
         "reduced_debug": r.bool(), "respawn_screen": r.bool(), "limited_crafting": r.bool(),
         "dimension_type": r.varint(), "dimension": r.str(), "seed": r.i64(), "game_mode": r.varint(),
         "previous_game_mode": r.varint() - 1, "is_debug": r.bool(), "is_flat": r.bool()}
    if r.bool():
        p["death"] = (r.str(), r.i64())
    p.update(portal_cooldown=r.varint(), sea_level=r.varint(), online_mode=r.bool(), secure_chat=r.bool())
    r.done()
    return p


def _paletted(r: Reader, entries: int, direct_bits_from: int) -> dict:
    bits = r.u8()
    if bits == 0:
        return {"bits": 0, "palette": [r.varint()], "longs": []}
    palette = None
    if bits < direct_bits_from:
        palette = [r.varint() for _ in range(r.varint())]
    per_long = 64 // bits
    longs = [r.u64() for _ in range((entries + per_long - 1) // per_long)]
    return {"bits": bits, "palette": palette, "longs": longs}


def paletted_get(c: dict, index: int) -> int:
    if c["bits"] == 0:
        return c["palette"][0]
    per_long = 64 // c["bits"]
    v = (c["longs"][index // per_long] >> ((index % per_long) * c["bits"])) & ((1 << c["bits"]) - 1)
    return c["palette"][v] if c["palette"] is not None else v


def pack_block_pos(x: int, y: int, z: int) -> bytes:
    """BlockPos.asLong: x:26 | z:26 | y:12."""
    return struct.pack(">Q", ((x & 0x3FFFFFF) << 38) | ((z & 0x3FFFFFF) << 12) | (y & 0xFFF))


def unpack_block_pos(v: int) -> tuple[int, int, int]:
    def signed(n: int, bits: int) -> int:
        return n - (1 << bits) if n >> (bits - 1) else n
    return signed(v >> 38, 26), signed(v & 0xFFF, 12), signed((v >> 12) & 0x3FFFFFF, 26)


def item_stack(item: int, count: int = 1, components: dict | None = None, removed: tuple = ()) -> bytes:
    """ItemStack as the client sends it in set_creative_mode_slot (OPTIONAL_UNTRUSTED_STREAM_CODEC):
    count, item, then the component patch with every value length-prefixed. `components` maps a
    component name ("kDamage") to its already encoded value; `removed` names components taken away."""
    if count <= 0:
        return varint(0)
    components = components or {}
    out = varint(count) + varint(item) + varint(len(components)) + varint(len(removed))
    for name, value in components.items():
        out += varint(COMPONENT[name]) + varint(len(value)) + value
    for name in removed:
        out += varint(COMPONENT[name])
    return out


def hashed_stack(stack) -> bytes:
    """HashedStack as container_click carries it, for a stack without components: (item, count) or None."""
    if stack is None:
        return b"\x00"
    return b"\x01" + varint(stack[0]) + varint(stack[1]) + varint(0) + varint(0)


def parse_block_update(r: Reader) -> tuple[tuple[int, int, int], int]:
    pos = unpack_block_pos(r.u64())
    state = r.varint()
    r.done()
    return pos, state


def parse_section_blocks_update(r: Reader) -> dict[tuple[int, int, int], int]:
    """{(x, y, z): state} in world coordinates."""
    packed = r.u64()

    def signed(n: int, bits: int) -> int:
        return n - (1 << bits) if n >> (bits - 1) else n
    sx, sy, sz = signed(packed >> 42, 22), signed(packed & 0xFFFFF, 20), signed((packed >> 20) & 0x3FFFFF, 22)
    out = {}
    for _ in range(r.varint()):
        v = 0
        for shift in range(0, 70, 7):  # VarLong
            b = r.u8()
            v |= (b & 0x7F) << shift
            if not b & 0x80:
                break
        local = v & 0xFFF
        out[(sx * 16 + (local >> 8), sy * 16 + (local & 15), sz * 16 + ((local >> 4) & 15))] = v >> 12
    r.done()
    return out


def _light_data(r: Reader) -> dict:
    # ByteBufCodecs.BIT_SET: BitSet.toByteArray() as a byte array (little-endian, no trailing zero byte).
    masks = [r.byte_array() for _ in range(4)]
    for m in masks:
        assert not m or m[-1] != 0, f"bit set with a trailing zero byte: {m.hex()}"
    sky_mask, block_mask, empty_sky, empty_block = [int.from_bytes(m, "little") for m in masks]
    sky = [r.byte_array() for _ in range(r.varint())]
    block = [r.byte_array() for _ in range(r.varint())]
    assert all(len(a) == 2048 for a in sky + block)
    assert len(sky) == bin(sky_mask).count("1") and len(block) == bin(block_mask).count("1")

    def by_section(mask: int, arrays: list) -> dict:
        it = iter(arrays)
        return {i: next(it) for i in range(64) if mask >> i & 1}
    return {"sky_mask": sky_mask, "block_mask": block_mask, "empty_sky": empty_sky, "empty_block": empty_block,
            "sky": by_section(sky_mask, sky), "block": by_section(block_mask, block)}


def light_get(array: bytes, x: int, y: int, z: int) -> int:
    """One nibble of a 2048-byte light array; x, y, z are section-local."""
    i = (y << 8) | (z << 4) | x
    return (array[i >> 1] >> ((i & 1) * 4)) & 15


def parse_light_update(r: Reader) -> dict:
    out = {"x": struct.unpack(">i", struct.pack(">I", r.varint()))[0], "z": struct.unpack(">i", struct.pack(">I", r.varint()))[0]}
    out.update(_light_data(r))
    r.done()
    return out


def parse_chunk(r: Reader, sections: int = 24) -> dict:
    c = {"x": r.i32(), "z": r.i32(), "heightmaps": {}}
    for _ in range(r.varint()):
        kind = r.varint()
        c["heightmaps"][kind] = r.long_array()
    data = Reader(r.byte_array())
    c["sections"] = []
    for _ in range(sections):
        c["sections"].append({"non_air": data.i16(), "fluids": data.i16(),
                              "blocks": _paletted(data, 4096, 9), "biomes": _paletted(data, 64, 4)})
    data.done()
    # BlockEntityInfo: packed x << 4 | z, y, type, and the update tag (TAG_End for none).
    c["block_entities"] = []
    for _ in range(r.varint()):
        xz, y, kind = r.u8(), r.i16(), r.varint()
        tag = None
        if r.d[r.i] == 0:
            r.take(1)
        else:
            tag = r.nbt()
        c["block_entities"].append(((xz >> 4, y, xz & 15), kind, tag))
    c["light"] = _light_data(r)
    r.done()
    return c


def chunk_block(c: dict, x: int, y: int, z: int, min_y: int = -64) -> int:
    """Block state at chunk-local x, z and world y of a parsed chunk."""
    s = c["sections"][(y - min_y) >> 4]
    return paletted_get(s["blocks"], (((y - min_y) & 15) << 8) | (z << 4) | x)


def heightmap_get(longs: list[int], index: int, bits: int = 9) -> int:
    per_long = 64 // bits
    return (longs[index // per_long] >> ((index % per_long) * bits)) & ((1 << bits) - 1)
