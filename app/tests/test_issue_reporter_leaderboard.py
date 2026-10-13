#!/usr/bin/env python3
"""Issue reporter results are folded dynamically from tracker records."""

import ast
import asyncio
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import time


ROOT = Path(__file__).resolve().parents[2]
ISSUES = ROOT / ".forkmesh" / "issues"
ENTRY = ROOT / "app" / "src" / "entry.py"
SNAPSHOT = ROOT / "app" / "src" / "issue_reporters.py"
GENERATOR = ROOT / "app" / "tools" / "build_issue_reporters.py"

FOLDS = {
    "_issue_reporter_open_event",
    "_issue_reporter_name",
    "_issue_reporter_day",
    "_issue_reporter_tracker_fold",
    "_issue_reporter_tracker_summary_valid",
    "_issue_reporter_tracker_summary",
    "issue_reporter_leaderboard",
}


def _records():
    records = {}
    for status in ("open", "closed"):
        for path in (ISSUES / status).glob("*/issue-*.json"):
            record = json.loads(path.read_text(encoding="utf-8"))
            records[int(record["number"])] = record
    return records


def _open_event(record):
    return next(event for event in record["events"] if event["type"] == "open")


def _load(extra=None):
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in FOLDS
    ]
    found = {node.name for node in selected}
    assert found == FOLDS, "missing: %s" % sorted(FOLDS - found)
    namespace = {
        "hashlib": hashlib,
        "json": json,
        "re": re,
        "time": time,
        "ISSUE_REPORTER_TRACKER_CACHE_KEY": (
            "https://forkmesh.internal/tracker/issue-reporter-summary"
        ),
        "ISSUE_REPORTER_TRACKER_TTL": 60,
        "ISSUE_REPORTER_TRACKER_OWNER": "forkmesh",
        "ISSUE_REPORTER_TRACKER_REPO": "forkmesh",
    }
    if extra:
        namespace.update(extra)
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


def _expected_activity(records):
    counts = Counter()
    activity = Counter()
    for record in records.values():
        opened = _open_event(record)
        reporter = str(
            record.get("authorName") or opened.get("authorName")
            or record.get("author") or "unknown"
        ).strip().lower() or "unknown"
        day = datetime.fromtimestamp(
            int(record.get("createdAt") or opened.get("ts")) / 1000,
            tz=timezone.utc,
        ).date().isoformat()
        signer = str(opened.get("author") or record.get("author") or "").strip()
        counts[reporter] += 1
        activity[(reporter, day, signer)] += 1
    return counts, activity


def test_dynamic_fold_matches_every_open_and_closed_issue():
    records = _records()
    folded = _load()["_issue_reporter_tracker_fold"](records.values())
    expected_counts, expected_activity = _expected_activity(records)

    assert records
    assert folded["counts"] == dict(sorted(
        expected_counts.items(), key=lambda item: (-item[1], item[0])))
    assert {
        (reporter, day, signer): count
        for reporter, day, signer, count in folded["activity"]
    } == expected_activity
    assert re.fullmatch(r"[0-9a-f]{64}", folded["revision"])


def _recredited_record(number, created_at):
    signer = "w0F5vYijNonu8kkqLSa_jYY4GIV-Q5HpbxYY525ggm4"
    return {
        "number": number,
        "author": signer,
        "authorName": "md-mehboob",
        "createdAt": created_at,
        "events": [{
            "type": "open",
            "author": signer,
            "authorName": "jett",
            "ts": created_at,
            "title": "Report %d" % number,
            "body": "",
        }],
    }


def test_dynamic_fold_preserves_recredited_reporter_and_signed_provenance():
    signer = "w0F5vYijNonu8kkqLSa_jYY4GIV-Q5HpbxYY525ggm4"
    august_12 = 1786536000000
    august_13 = 1786622400000
    records = [
        _recredited_record(1, august_12),
        _recredited_record(2, august_12 + 60_000),
        _recredited_record(3, august_13),
    ]
    folded = _load()["_issue_reporter_tracker_fold"](records)
    activity = {
        (reporter, day, actor): count
        for reporter, day, actor, count in folded["activity"]
    }

    assert folded["counts"] == {"md-mehboob": 3}
    assert activity[("md-mehboob", "2026-08-12", signer)] == 2
    assert activity[("md-mehboob", "2026-08-13", signer)] == 1


def test_dynamic_fold_uses_edge_cache_without_writing_a_source_file():
    records = list(_records().values())[:2]
    cache = {}
    calls = {"load": 0, "put": 0}

    class _Response:
        def __init__(self, data):
            self.data = data

        async def json(self):
            return self.data

    async def edge_cache_match(key):
        return cache.get(key)

    async def edge_cache_put(key, response):
        calls["put"] += 1
        cache[key] = response

    async def response_json(response):
        return await response.json()

    async def all_issue_records(_env, owner, repo):
        calls["load"] += 1
        assert (owner, repo) == ("forkmesh", "forkmesh")
        return records

    def json_response(data, **_kwargs):
        return _Response(data)

    namespace = _load({
        "edge_cache_match": edge_cache_match,
        "edge_cache_put": edge_cache_put,
        "_response_json": response_json,
        "_forkbot_all_issue_records": all_issue_records,
        "json_response": json_response,
    })
    first = asyncio.run(namespace["_issue_reporter_tracker_summary"](object()))
    second = asyncio.run(namespace["_issue_reporter_tracker_summary"](object()))

    assert first == second
    assert calls == {"load": 1, "put": 1}
    assert SNAPSHOT.exists() is False
    assert GENERATOR.exists() is False


def test_build_configuration_has_no_snapshot_or_database_projection():
    assert not SNAPSHOT.exists()
    assert not GENERATOR.exists()
    assert not (ROOT / "app" / "migrations" /
                "0158_issue_reporter_credits.sql").exists()
    assert "build_issue_reporters" not in (
        ROOT / "app" / "wrangler.toml"
    ).read_text(encoding="utf-8")
    source = ENTRY.read_text(encoding="utf-8")
    assert "_forkbot_all_issue_records" in source
    assert "issue_reporter_credits" not in source
