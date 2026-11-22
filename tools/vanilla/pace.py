# SPDX-License-Identifier: GPL-3.0-or-later
"""How long a server tick is, for the probes: they cut what they record by the clock, a tick at a
time, and wait so many ticks for a join or an answer.

Vanilla's twenty a second - unless both servers were told to tick faster (compare.py --tick-rate:
`tick rate` on vanilla's console, --tick-rate on ours), which compare.py says to the probes it runs
through MC_PROBE_TICK_RATE. A recording counts ticks, not seconds, so the two servers still see the
same script; what changes is how long it takes.
"""
from __future__ import annotations

import os

RATE = float(os.environ.get("MC_PROBE_TICK_RATE", "20"))
TICK = 1.0 / RATE   # seconds a tick


def ticks(n: float) -> float:
    """n ticks, in seconds."""
    return n * TICK
