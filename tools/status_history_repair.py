#!/usr/bin/env python3
from __future__ import annotations

import argparse
import ast
import json
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "deploy" / "core"))

import next_common as common
from next_common import ToolError

REPO = Path(__file__).resolve().parents[1]
ENTRY = REPO / "app" / "src" / "entry.py"
HOUR_MS = 3600000
DAY_MS = 86400000
MIRROR_PREFIX = "mirror:"
SCHEMA_VERSION = 1
REASON_MAX = 240


def retired_mirrors(entry=ENTRY):
    tree = ast.parse(Path(entry).read_text(encoding="utf-8"))
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(
                isinstance(t, ast.Name) and t.id == "STATUS_RETIRED_MIRRORS"
                for t in node.targets):
            call = node.value
            if isinstance(call, ast.Call) and call.args:
                return frozenset(ast.literal_eval(call.args[0]))
            return frozenset(ast.literal_eval(call))
    raise ToolError("STATUS_RETIRED_MIRRORS not found in %s" % entry)


def is_retired(system_id, retired):
    system_id = str(system_id or "")
    return (system_id.startswith(MIRROR_PREFIX)
            and system_id[len(MIRROR_PREFIX):] in retired)


def decode(raw, retired):
    try:
        payload = json.loads(str(raw or ""))
    except ValueError:
        return None
    if (not isinstance(payload, dict)
            or payload.get("schemaVersion") != SCHEMA_VERSION
            or not isinstance(payload.get("systems"), dict)):
        return None
    return {k: v for k, v in payload["systems"].items()
            if isinstance(v, dict) and not is_retired(k, retired)}


def encode(systems):
    return json.dumps({"schemaVersion": SCHEMA_VERSION, "systems": systems},
                      ensure_ascii=True, separators=(",", ":"),
                      sort_keys=True, allow_nan=False)


def hour_from_minutes(minute_rows, hour_ts, retired):
    systems = {}
    for row in sorted(minute_rows, key=lambda r: int(r["minute_ts"])):
        ts = int(row["minute_ts"])
        if not hour_ts <= ts < hour_ts + HOUR_MS:
            continue
        for system_id, item in (decode(row["data"], retired) or {}).items():
            agg = systems.setdefault(
                system_id, {"checks": 0, "failures": 0, "reason": None})
            agg["checks"] += 1
            if item.get("ok") is not True:
                agg["failures"] += 1
                agg["reason"] = (str(item.get("reason") or "")[:REASON_MAX]
                                 or None)
    for agg in systems.values():
        agg["checks"] = min(60, agg["checks"])
        agg["failures"] = min(agg["checks"], agg["failures"])
    return systems


def day_from_hours(hour_systems_by_ts, day_ts):
    systems = {}
    for hour_ts, hour in hour_systems_by_ts.items():
        if not day_ts <= hour_ts < day_ts + DAY_MS:
            continue
        for system_id, item in hour.items():
            agg = systems.setdefault(system_id, {"checks": 0, "failures": 0})
            agg["checks"] += max(0, int(item.get("checks") or 0))
            agg["failures"] += max(0, int(item.get("failures") or 0))
    for agg in systems.values():
        agg["checks"] = min(1440, agg["checks"])
        agg["failures"] = min(agg["checks"], agg["failures"])
    return systems


def checks(systems):
    return sum(int((v or {}).get("checks") or 0) for v in (systems or {}).values())


def dominates(candidate, existing):
    if existing is None:
        return checks(candidate) > 0
    for system_id, item in existing.items():
        if int((candidate.get(system_id) or {}).get("checks") or 0) < int(
                item.get("checks") or 0):
            return False
    return checks(candidate) > checks(existing)


def plan(minute_rows, hour_rows, day_rows, now_ms, retired):
    current_hour = now_ms // HOUR_MS * HOUR_MS
    current_day = now_ms // DAY_MS * DAY_MS
    oldest_minute = min((int(r["minute_ts"]) for r in minute_rows), default=None)
    hours_raw = {int(r["hour_ts"]): r["data"] for r in hour_rows}
    days_raw = {int(r["day_ts"]): r["data"] for r in day_rows}
    hours = {ts: decode(raw, retired) for ts, raw in hours_raw.items()}
    changes = []
    if oldest_minute is not None:
        first = -(-oldest_minute // HOUR_MS) * HOUR_MS
        for hour_ts in range(first, current_hour, HOUR_MS):
            candidate = hour_from_minutes(minute_rows, hour_ts, retired)
            existing = hours.get(hour_ts)
            if hour_ts in hours_raw and existing is None:
                continue
            if dominates(candidate, existing):
                changes.append(("hour", hour_ts, hours_raw.get(hour_ts),
                                encode(candidate), checks(existing), checks(candidate)))
                hours[hour_ts] = candidate
    for day_ts in sorted(days_raw) + [d for d in sorted({h // DAY_MS * DAY_MS for h in hours}) if d not in days_raw]:
        if day_ts >= current_day:
            continue
        day_hours = {h: s for h, s in hours.items()
                     if s is not None and day_ts <= h < day_ts + DAY_MS}
        if len(day_hours) < 24:
            continue
        candidate = day_from_hours(day_hours, day_ts)
        existing = decode(days_raw.get(day_ts), retired) if day_ts in days_raw else None
        if day_ts in days_raw and existing is None:
            continue
        if dominates(candidate, existing):
            changes.append(("day", day_ts, days_raw.get(day_ts), encode(candidate),
                            checks(existing), checks(candidate)))
    return changes


def statements(changes):
    out = []
    for bucket, ts, old, new, _before, _after in changes:
        table, column = (("system_status_hourly_batch", "hour_ts") if bucket == "hour"
                         else ("system_status_daily_batch", "day_ts"))
        if old is None:
            out.append(["INSERT OR IGNORE INTO %s (%s,data) VALUES (?,?)" % (table, column), ts, new])
        else:
            out.append(["UPDATE %s SET data=? WHERE %s=? AND data=?" % (table, column), new, ts, old])
    return out


def stamp(ms):
    return time.strftime("%Y-%m-%d %H:%M", time.gmtime(ms / 1000))


class Tunnel:
    def __init__(self, ssh=None):
        import next_deploy
        self.nd = next_deploy
        self.ssh = ssh or common.Ssh()

    def __enter__(self):
        import rqlite_migrate
        fleet = common.load_fleet()
        target = self.nd.find_rqlite_node(fleet, self.ssh)
        url, self.close = self.nd.open_rqlite_tunnel(self.ssh, target)
        values = common.read_env_file(common.config_dir() / "secrets.env")
        return rqlite_migrate.Rqlite(
            url,
            user=(fleet.get("rqlite") or {}).get("user", "fmnext"),
            password=values.get("RQLITE_PASSWORD", ""))

    def __exit__(self, *exc):
        self.close()


def rows(db, sql):
    return (db.request([[sql]], level="strong")[0].get("rows") or [])


def run(db, runner, now_ms=None, retired=None):
    retired = retired if retired is not None else retired_mirrors()
    now_ms = int(now_ms if now_ms is not None else time.time() * 1000)
    minute_rows = rows(db, "SELECT minute_ts,data FROM system_status_minute_batch")
    hour_rows = rows(db, "SELECT hour_ts,data FROM system_status_hourly_batch")
    day_rows = rows(db, "SELECT day_ts,data FROM system_status_daily_batch")
    changes = plan(minute_rows, hour_rows, day_rows, now_ms, retired)
    runner.say("status batches: %d minute, %d hour, %d day rows; retired mirrors %s"
               % (len(minute_rows), len(hour_rows), len(day_rows), ",".join(sorted(retired))))
    for bucket, ts, old, _new, before, after in changes:
        runner.say("  %s %s: %d -> %d checks (%s)" % (
            bucket, stamp(ts), before, after, "insert" if old is None else "compare-and-swap"))
    if not changes:
        runner.say("nothing to repair")
        return []
    result = runner.step(
        "write %d recomputed status batch rows (insert-or-ignore / update only "
        "where the stored value is unchanged)" % len(changes),
        lambda: db.request(statements(changes), transaction=True, level=None))
    if result is not None:
        applied = sum(int(r.get("rows_affected") or 0) for r in result)
        runner.say("applied %d of %d" % (applied, len(changes)))
    return changes


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Recompute /status hour and day batches that lost coverage.")
    parser.add_argument("--execute", action="store_true")
    parser.add_argument("--entry", default=str(ENTRY))
    args = parser.parse_args(argv)
    runner = common.runner_from_args(args)
    try:
        retired = retired_mirrors(args.entry)
        with Tunnel() as db:
            run(db, runner, retired=retired)
    except ToolError as error:
        print("error: %s" % error, file=sys.stderr)
        return 1
    if not args.execute:
        print("(dry run: nothing was changed; pass --execute to apply)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
