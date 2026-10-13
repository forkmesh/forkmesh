"""Privacy-preserving unique-visitor helpers for the World Arrival Grid.

The Worker transiently combines Cloudflare's edge-observed source address with
this module's deliberately coarse user-agent category, then applies keyed,
domain-separated HMACs.  Neither raw input nor either digest is stored.  Only
each digest's HyperLogLog register and rank enter D1. Temporal digests rotate
at UTC midnight; the all-time digest uses a separate stable context.

This is intentionally an *approximate* unique count:

* people sharing an address and the same coarse client category collapse into
  one visitor (common behind offices, carrier NAT, VPNs, and shared devices);
* one person changing network or client category can count more than once;
* a rolling-hour window crossing UTC midnight can count the same returning
  visitor once on each side of the privacy rotation;
* rotating ForkMesh's data/HMAC secret improves unlinkability but can make a
  returning visitor look new to the all-time sketch.

Those trade-offs avoid turning a playful public counter into an indefinitely
linkable visitor ledger.  A precision of 10 bounds each sketch to 1,024 rows.
"""

from __future__ import annotations

import ipaddress
import math
import re


HLL_PRECISION = 10
HLL_REGISTER_COUNT = 1 << HLL_PRECISION
HLL_MAX_RANK = (256 - HLL_PRECISION) + 1
HLL_ALL_TIME_BUCKET = -1


def canonical_edge_address(value):
    """Canonicalize one edge-provided IP address or return an empty string.

    Comma-separated forwarding chains, hostnames, ports, zone identifiers, and
    other client-provided lookalikes are rejected rather than guessed at.
    Callers must pass Cloudflare's edge-authored ``CF-Connecting-IP`` value,
    never a body/query field or an untrusted forwarding header.
    """
    text = str(value or "").strip()
    if not text or len(text) > 64 or "," in text or "%" in text:
        return ""
    try:
        return str(ipaddress.ip_address(text))
    except ValueError:
        return ""


def generalized_user_agent(raw_user_agent):
    """Return a coarse, non-identifying browser/OS/device category.

    Version numbers, model names, extension markers, locale details, build
    identifiers, and the original string are deliberately discarded.  The
    result is suitable only for approximate visitor deduplication.
    """
    ua = str(raw_user_agent or "").lower()[:2048]
    if not ua:
        return "unknown-agent"

    if any(marker in ua for marker in (
            "bot", "crawler", "spider", "headless", "scanner")):
        client = "automated"
    elif "curl/" in ua or "wget/" in ua:
        client = "command-line"
    elif "edg/" in ua or "edge/" in ua:
        client = "edge"
    elif "opr/" in ua or "opera" in ua:
        client = "opera"
    elif "firefox/" in ua or "fxios/" in ua:
        client = "firefox"
    elif "chrome/" in ua or "crios/" in ua or "chromium/" in ua:
        client = "chromium"
    elif "safari/" in ua:
        client = "safari"
    elif "mozilla/" in ua:
        client = "browser"
    else:
        client = "other"

    if any(marker in ua for marker in (
            "android", "iphone", "ipad", "ipod", "mobile")):
        device = "mobile"
    else:
        device = "desktop"

    if "android" in ua:
        system = "android"
    elif any(marker in ua for marker in ("iphone", "ipad", "ipod")):
        system = "ios"
    elif "windows" in ua:
        system = "windows"
    elif "cros" in ua:
        system = "chromeos"
    elif "mac os" in ua or "macintosh" in ua:
        system = "macos"
    elif "linux" in ua or "x11" in ua:
        system = "linux"
    else:
        system = "other-os"

    # Keep the allowlist explicit even if this helper changes in the future.
    category = "%s:%s:%s" % (client, system, device)
    return (
        category
        if re.fullmatch(r"[a-z-]+:[a-z0-9-]+:[a-z-]+", category)
        else "other:other-os:desktop"
    )


def hll_register(token_hex, precision=HLL_PRECISION):
    """Project one 256-bit opaque HMAC digest into an HLL register update."""
    if precision < 4 or precision > 18:
        raise ValueError("unsupported HLL precision")
    text = str(token_hex or "").strip().lower()
    if not re.fullmatch(r"[0-9a-f]{64}", text):
        raise ValueError("visitor token must be a 256-bit hexadecimal digest")

    bits = 256
    value = int(text, 16)
    register_id = value >> (bits - precision)
    remainder_bits = bits - precision
    remainder = value & ((1 << remainder_bits) - 1)
    rank = (
        remainder_bits + 1
        if remainder == 0
        else remainder_bits - remainder.bit_length() + 1
    )
    return register_id, rank


def hll_estimate(register_rows, precision=HLL_PRECISION):
    """Estimate unique values represented by merged HLL register rows."""
    if precision < 4 or precision > 18:
        raise ValueError("unsupported HLL precision")
    register_count = 1 << precision
    registers = {}
    for row in register_rows or []:
        try:
            register_id = int(row.get("register_id"))
            rank = int(row.get("rank"))
        except (AttributeError, TypeError, ValueError):
            continue
        if not 0 <= register_id < register_count or rank <= 0:
            continue
        registers[register_id] = max(
            registers.get(register_id, 0),
            min(rank, (256 - precision) + 1),
        )

    zero_count = register_count - len(registers)
    harmonic = zero_count + sum(
        2.0 ** (-rank) for rank in registers.values())
    alpha = 0.7213 / (1.0 + 1.079 / register_count)
    estimate = alpha * register_count * register_count / harmonic

    # Linear counting removes the large small-cardinality bias and gives the
    # Arrival Grid sensible 0/1/2 counts while retaining HLL's fixed footprint.
    if estimate <= 2.5 * register_count and zero_count:
        estimate = register_count * math.log(register_count / zero_count)
    return max(0, int(round(estimate)))


def unique_visit_window_bounds(now_ms):
    """Return the UTC ranges represented by the public Arrival Grid plaque."""
    now = int(now_ms)
    day_ms = 24 * 60 * 60 * 1000
    hour_ms = 60 * 60 * 1000
    midnight = now - (now % day_ms)
    return {
        "today": (midnight, None),
        "yesterday": (midnight - day_ms, midnight),
        "yesterdaySameTime": (midnight - day_ms, now - day_ms),
        "pastHour": (now - hour_ms, None),
        "pastHourYesterday": (
            now - day_ms - hour_ms,
            now - day_ms,
        ),
    }


def unique_visit_summary(register_sets):
    """Estimate the six aggregate-only counters returned by the public API."""
    rows = register_sets if isinstance(register_sets, dict) else {}
    return {
        key: hll_estimate(rows.get(key, []))
        for key in (
            "total",
            "today",
            "yesterday",
            "yesterdaySameTime",
            "pastHour",
            "pastHourYesterday",
        )
    }
