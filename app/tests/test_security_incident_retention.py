"""IDS incident retention: info after 90 days, the rest after a year, never
lockdowns or incidents a quarantine record names."""

import ast
import asyncio
import json
from pathlib import Path
import sqlite3


APP = Path(__file__).resolve().parents[1]
ENTRY = APP / "src" / "entry.py"
DAY = 24 * 60 * 60 * 1000
NOW = 1_800_000_000_000


def _load():
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    wanted = [n for n in tree.body if (
        isinstance(n, ast.AsyncFunctionDef) and n.name == "prune_security_incidents"
    ) or (
        isinstance(n, ast.Assign)
        and any(getattr(t, "id", "").startswith("SECURITY_INCIDENT_")
                for t in n.targets))]
    assert len(wanted) == 4
    db = sqlite3.connect(":memory:")
    db.executescript((APP / "migrations" / "0197_security_ids.sql").read_text())

    async def d1_run(_env, sql, *args):
        db.execute(sql, args)

    namespace = {"d1_run": d1_run}
    module = ast.fix_missing_locations(ast.Module(body=wanted, type_ignores=[]))
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return db, namespace


def _incident(db, incident_id, severity, age_days):
    db.execute(
        "INSERT INTO security_incidents (id,node_id,created_ms,severity,detector,"
        "summary) VALUES (?,?,?,?,?,?)",
        (incident_id, "n2", NOW - age_days * DAY, severity, "test", "summary"))


def test_retention_keeps_recent_lockdown_and_quarantined_incidents():
    db, ns = _load()
    _incident(db, "info-old", "info", 91)
    _incident(db, "info-recent", "info", 30)
    _incident(db, "alert-mid", "alert", 200)
    _incident(db, "alert-old", "alert", 400)
    _incident(db, "lockdown-old", "lockdown", 800)
    _incident(db, "quarantined-old", "alert", 800)
    _incident(db, "quarantined-info", "info", 200)
    for record_id, incident_id in (("q1", "quarantined-old"), ("q2", "quarantined-info")):
        db.execute(
            "INSERT INTO cluster_quarantine (id,node_id,kind,signer,created_ms,body,sig) "
            "VALUES (?,?,?,?,?,?,?)",
            (record_id, "n2", "self", "n2", NOW, json.dumps(
                {"incident": incident_id, "node": "n2", "ts": NOW},
                sort_keys=True, separators=(",", ":")), "sig"))
    asyncio.run(ns["prune_security_incidents"](None, NOW))
    left = {row[0] for row in db.execute("SELECT id FROM security_incidents")}
    assert left == {"info-recent", "alert-mid", "lockdown-old",
                    "quarantined-old", "quarantined-info"}


def test_retention_deletes_at_most_one_batch_oldest_first():
    db, ns = _load()
    ns["SECURITY_INCIDENT_PRUNE_BATCH"] = 2
    for index in range(4):
        _incident(db, "old-%d" % index, "info", 100 + index)
    asyncio.run(ns["prune_security_incidents"](None, NOW))
    left = {row[0] for row in db.execute("SELECT id FROM security_incidents")}
    assert left == {"old-0", "old-1"}
