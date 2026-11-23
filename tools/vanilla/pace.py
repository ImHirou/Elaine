# SPDX-License-Identifier: GPL-3.0-or-later
"""How a probe keeps step with a server: by the server's own ticks, not by the clock.

A probe plays one script to two servers and compares what each sent, step by step. Cut by the
clock - "five ticks of fifty milliseconds" - a step is as long as the slower server needs with
room to spare, and still whatever comes late falls into the next one. So the servers are frozen
(`/tick freeze`: both have it, see docs/architecture.md, "Time") and a step is what the script
asks for:

  * `Lockstep.sync()`   whatever the bots have sent has been read, acted on and shown: a whole
                        loop of the server has begun and ended since the call.
  * `Lockstep.run(k)`   the game takes k ticks - exactly k, on either server - as fast as it
                        can, and what they left has been shown.
  * `Lockstep.chunks(bot)`  the chunks of a bot's view are there. Workers make those in their own
                        time; that is waited for, by the clock, with a limit.

Nothing sleeps: a bot reads until the server says that it is done. What says so is a
`ticking_state` packet - every player is sent one whenever the tick rate or the freeze is set,
changed or not (ServerTickRateManager.updateStateToClients) - so the console sets the rate to
what it is but for a thousandth that names the mark (`tick rate 200.007`), and each bot reads up
to the packet with that rate. A frozen game still loops at its rate - packets, players, the
tracker - which is how often a mark can come: RATE is that, not the pace of the game.

  the order of a loop (MinecraftServer.runServer, the same in Game::run):
      the end of a sprint | the players' packets | the level (the tracker shows what moved) |
      the connections | the console | flush
  one mark     the console was read once more: in the loop it was typed in, or the next
  two marks    ...so a loop began after the call, and ran to its end: sync()
  a sprint     asked of a frozen game from the console: one tick at the usual pace, then the
               sprint's own without a wait, then frozen again (a `ticking_state` unfrozen, one
               frozen: the bots read up to that) - run(k) asks for k - 1
  one tick     `tick step 1`, which says nothing when it is done: its mark, then two more.
               Tick after tick, each armed as the mark of the one before arrives: tick()

What the marks bring with them - the packets themselves, and what an operator is told of the
console's commands - never reaches a bot's log.

MC_PROBE_TICK_RATE is the rate of those loops (compare.py --tick-rate).
"""
from __future__ import annotations

import os
import socket
import struct
import sys
import time
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tests" / "integration"))
import mcproto as mc  # noqa: E402

# Loops a second while the game stands: how soon a server reads what it is sent. Two hundred is
# five milliseconds a loop - a mark, there and back, is one or two of them - and leaves a bot the
# while it takes to answer (vanilla asks again for a teleport that is not confirmed in twenty).
RATE = float(os.environ.get("MC_PROBE_TICK_RATE", "200"))
TICK = 1.0 / RATE   # seconds a loop of the server takes
VIEW = 2            # the view distance both servers are run with
PATIENCE = 30.0     # seconds before a server that does not answer is given up


def ticks(n: float) -> float:
    """n loops of the server, in seconds: for what is waited for by the clock."""
    return n * TICK


def console_of(path: str):
    """Types on a server's console: compare.py makes its stdin a FIFO and hands the probes its path."""
    fd = os.open(path, os.O_WRONLY)

    def type_line(line: str) -> None:
        os.write(fd, line.encode() + b"\n")
    return type_line


class _Reached(socket.timeout):
    """What a bot was reading up to has come: its pump() takes this for a quiet socket and returns."""


class _Watch:
    """What one bot is reading up to, and what it has been sent of its view."""

    def __init__(self):
        self.rate = None        # a mark: the tick rate that names it
        self.pong = None        # the payload of a ping
        self.sprint = None      # "begins" | "ends"
        self.driving = False    # the console is stepping or sprinting: what it is told of that is not the script's
        self.centre = None
        self.chunks: set = set()


def _i32(v: int) -> int:
    return v - (1 << 32) if v & 0x80000000 else v


class Lockstep:
    def __init__(self, console, rate: float = RATE):
        """`console(line)` types on the server's console. The game is frozen from here on."""
        self.console = console
        self.rate = rate
        self.bots: list = []
        self.seq = 0
        self.pings = 0
        self.spent = {"marks": [0, 0.0], "pongs": [0, 0.0], "sprints": [0, 0.0], "chunks": [0, 0.0]}   # how often, how long
        console(f"tick freeze\ntick rate {rate:g}")

    def close(self) -> None:
        """The server as it was found: running, at twenty ticks a second. And where the time went."""
        self.console("tick unfreeze\ntick rate 20")
        print("in step: " + ", ".join(f"{n} {what} {took:.1f} s" for what, (n, took) in self.spent.items()))

    def _took(self, what: str, since: float) -> None:
        self.spent[what][0] += 1
        self.spent[what][1] += time.time() - since

    # ------------------------------------------------------------------ the bots

    def join(self, bot) -> None:
        """A bot that has logged in keeps step from here on. (Before its first packet of the
        game is read: the chunks of its view are counted from the start.)"""
        watch = bot.watch = _Watch()
        bot.lock = self
        inner = bot.c.recv_any

        def recv_any():
            while True:
                name, r = inner()
                body = r.d[r.i:]
                if name == "ticking_state":
                    rate, frozen = struct.unpack(">f?", body[:5])
                    if watch.rate is not None and rate == watch.rate:
                        watch.rate = None
                        raise _Reached()
                    if watch.sprint == "begins" and not frozen:
                        watch.sprint = "ends"
                        continue
                    if watch.sprint == "ends" and frozen:
                        watch.sprint = None
                        raise _Reached()
                elif name == "ticking_step" and watch.driving:
                    continue
                elif name == "pong_response" and watch.pong is not None and body[:8] == watch.pong:
                    watch.pong = None
                    raise _Reached()
                elif name == "system_chat" and b"chat.type.admin" in body and b"Server" in body:
                    # broadcastToAdmins of what the console did to keep step.
                    if b"commands.tick.rate.success" in body:
                        continue
                    if watch.driving and (b"commands.tick.status.sprinting" in body or b"commands.tick.sprint.report" in body
                                          or b"commands.tick.step.success" in body):
                        continue
                elif name == "set_chunk_cache_center":
                    reader = mc.Reader(body)
                    watch.centre = (_i32(reader.varint()), _i32(reader.varint()))
                elif name == "level_chunk_with_light":
                    watch.chunks.add(struct.unpack(">ii", body[:8]))
                elif name == "forget_level_chunk":
                    packed = struct.unpack(">Q", body[:8])[0]
                    watch.chunks.discard((_i32(packed & 0xFFFFFFFF), _i32(packed >> 32)))
                return name, r

        bot.c.recv_any = recv_any
        self.bots.append(bot)

    def leave(self, bot) -> None:
        if bot in self.bots:
            self.bots.remove(bot)

    def _read(self, bot, pending, what: str) -> None:
        """The bot reads until `pending()` is false. One that the server has let go keeps step no more."""
        end = time.time() + PATIENCE
        try:
            while pending():
                if time.time() > end:
                    raise SystemExit(f"{bot.name} was not told of {what} within {PATIENCE:.0f} s: is the server frozen, and its console read?")
                bot.pump(1.0)   # (ends with what is read up to: _Reached)
        except (ConnectionError, EOFError, OSError, AssertionError, RuntimeError, struct.error):
            self.leave(bot)

    # ------------------------------------------------------------------ keeping step

    def mark(self, first: str | None = None) -> None:
        """The console was read once more, and every bot has what was sent up to then. `first`: a
        line typed just ahead of the mark's - read by the loop that reads the mark, or an earlier
        one: when the mark is here, that line has been run."""
        self.seq = self.seq % 40 + 1
        text = f"{self.rate + self.seq / 1000:.3f}"     # (shown as the rate it was: "%.1f")
        rate = struct.unpack(">f", struct.pack(">f", float(text)))[0]
        for bot in self.bots:
            bot.watch.rate = rate
        t0 = time.time()
        self.console((f"{first}\n" if first else "") + f"tick rate {text}")
        for bot in list(self.bots):
            self._read(bot, lambda bot=bot: bot.watch.rate is not None, f"mark {text}")
        self._took("marks", t0)

    def _pongs(self) -> None:
        """What the bots sent is in: a pong is behind everything sent before its ping."""
        t0 = time.time()
        for bot in list(self.bots):
            self.pings += 1
            bot.watch.pong = struct.pack(">q", 0x10C4_0000_0000 + self.pings)
            try:
                bot.tick_end()
                bot.c.send("ping_request", bot.watch.pong)
            except (ConnectionError, OSError):
                self.leave(bot)
        for bot in list(self.bots):
            self._read(bot, lambda bot=bot: bot.watch.pong is not None, "its pong")
        self._took("pongs", t0)

    def sync(self) -> None:
        """What the bots sent has been read, acted on and shown: it was in, and a whole loop has
        run since."""
        self._pongs()
        self.mark()
        self.mark()

    def run(self, k: int) -> None:
        """The game takes exactly k ticks, and what the last of them moved has been shown. (Of a
        game that stands, with nothing waiting to be read: after sync(), or another run().)"""
        if k <= 0:
            return
        for bot in self.bots:
            bot.watch.driving = True
        if k == 1:
            self.tick()
            self.settle()
        else:
            for bot in self.bots:
                bot.watch.sprint = "begins"
            self.mark(f"tick sprint {k - 1}")
            t0 = time.time()
            for bot in list(self.bots):
                self._read(bot, lambda bot=bot: bot.watch.sprint is not None, f"the end of a sprint of {k - 1}")
            self._took("sprints", t0)
            self.mark()
        for bot in self.bots:
            bot.watch.driving = False

    def tick(self) -> None:
        """One tick is armed (`tick step 1`), and what the server sent before it read that is
        here: the loop after takes the tick. So, one tick() after another - each typed as the
        mark of the one before arrives, which is a loop's time - what is read by the n-th is
        the game's tick n - 1, and the server loops once a tick: a tracker that counts loops
        counts ticks. (A bot that is late for a loop gets a loop of a game that stands in
        between: the ticks are what they are, a tracker's interval falls a loop later.)
        settle() after the last."""
        for bot in self.bots:
            bot.watch.driving = True
        self.mark("tick step 1")
        for bot in self.bots:
            bot.watch.driving = False

    def settle(self) -> None:
        """After tick(): the tick that was armed has been taken - the first mark is read by its
        loop, behind its level's tick, or a later one - and what it moved has been shown, by the
        loop after."""
        self.mark()
        self.mark()

    def step(self, k: int = 1) -> None:
        """What was sent is acted on in a game that stands, then k ticks pass: sync(), run(k) -
        with the second mark of the one and the first of the other the same."""
        self._pongs()
        self.mark()
        if k <= 0:
            self.mark()
        else:
            self.run(k)

    def chunks(self, bot, view: int = VIEW) -> None:
        """The chunks of the bot's view have come: where it joined, where it was taken. By the
        clock - they are made by workers, in no tick - while everyone goes on reading."""
        def missing() -> set:
            watch = bot.watch
            if watch.centre is None:
                return {None}
            return mc.view_chunks(*watch.centre, view) - watch.chunks
        t0 = time.time()
        end = t0 + PATIENCE
        while bot in self.bots and missing():
            if time.time() > end:
                raise SystemExit(f"{bot.name} was not sent its view around {bot.watch.centre}: {len(missing())} chunks short")
            self._turn()
        self._took("chunks", t0)

    def place(self, bots, key: str) -> None:
        """The console takes these bots to a spot that is `key`'s alone, on the grass of a flat
        world and far from everything: what another recording built, dropped or blew up in this
        world is not there, and both servers have the bots stand on the same block (vanilla
        would spread them around the world's spawn)."""
        spot = zlib.crc32(key.encode())
        x, z = -8000 - spot % 400 * 48, -8000 - spot // 400 % 400 * 48
        for bot in bots:
            self.console(f"tp {bot.name} {x}.5 -60 {z}.5")
        self.sync()
        for bot in bots:
            self.chunks(bot)
        for _ in range(4):      # (who stands there already is shown a loop or two after the chunk it stands in)
            self.mark()

    def idle(self, seconds: float) -> None:
        """Everyone reads for so long, by the clock: for what no tick brings (a file, a kick)."""
        end = time.time() + seconds
        while time.time() < end:
            self._turn()

    def _turn(self) -> None:
        """Every bot reads what is there, for a moment."""
        for bot in list(self.bots):
            try:
                bot.pump(0.002)
            except (ConnectionError, EOFError, OSError, AssertionError, RuntimeError, struct.error):
                self.leave(bot)
