#!/usr/bin/env python3
"""Bounded public CelesTrak OMM snapshot policy tests."""

import ast
import asyncio
import hmac
import json
from pathlib import Path
import re
import sqlite3
import sys
from types import SimpleNamespace

import pytest


ROOT = Path(__file__).resolve().parents[2] / "app"
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import world_satellites as satellites  # noqa: E402
from schema import SCHEMA_STATEMENTS  # noqa: E402


FETCHED_AT = 1_800_000_000_000
ENTRY = SRC / "entry.py"
# The World and Office route bodies moved into sibling modules so Cloudflare's
# Python startup validation never parses them. This contract still covers the
# whole route family, so their source is read alongside entry.py.
WORLD_ROUTE_SOURCE = "".join(
    (Path(__file__).resolve().parents[1] / "src" / name).read_text(
        encoding="utf-8")
    for name in ("world_routes.py", "world_office_routes.py")
)
ENTRY_TEXT = (
    ENTRY.read_text(encoding="utf-8")
    + WORLD_ROUTE_SOURCE
    + (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
)


def _load_entry_functions(*names, extra_globals=None):
    tree = ast.parse(ENTRY_TEXT)
    nodes = [
        node
        for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in names
    ]
    assert {node.name for node in nodes} == set(names)
    namespace = dict(extra_globals or {})
    exec(
        compile(
            ast.fix_missing_locations(ast.Module(body=nodes, type_ignores=[])),
            str(ENTRY),
            "exec",
        ),
        namespace,
    )
    return namespace


def _entry_function_source(name):
    tree = ast.parse(ENTRY_TEXT)
    node = next(
        item
        for item in tree.body
        if isinstance(item, (ast.FunctionDef, ast.AsyncFunctionDef))
        and item.name == name
    )
    return ast.get_source_segment(ENTRY_TEXT, node)


class Upstream:
    def __init__(self, body="", status=200, content_length=None):
        self.status = status
        self.body = body
        self.headers = {}
        if content_length is not None:
            self.headers["content-length"] = str(content_length)

    async def text(self):
        return self.body


class EntryHarness:
    def __init__(self, upstream=None, row=None):
        self.upstream = upstream
        self.row = dict(row) if row else None
        self.fetches = []
        self.runs = []
        self.schema_calls = 0
        self.cache_hit = None
        self.cache_puts = []
        self.cache_deletes = []
        self.first_calls = 0

    async def ensure_schema(self, _env):
        self.schema_calls += 1

    async def d1_first(self, _env, _sql, *_args):
        self.first_calls += 1
        return dict(self.row) if self.row else None

    async def d1_run(self, _env, sql, *args):
        self.runs.append((sql, args))
        if "INSERT INTO world_satellite_snapshot" in sql:
            document, digest, fetched_at, source_epoch, attempted_at = args
            self.row = {
                "data": document,
                "digest": digest,
                "fetched_at": fetched_at,
                "source_epoch": source_epoch,
                "last_attempt_at": attempted_at,
                "last_status": 200,
                "last_error": "",
            }
        elif "last_status=0" in sql and self.row:
            self.row["last_attempt_at"] = args[0]
            self.row["last_status"] = 0
            self.row["last_error"] = ""
        elif "last_status=?,last_error=?" in sql and self.row:
            self.row["last_attempt_at"] = args[0]
            self.row["last_status"] = args[1]
            self.row["last_error"] = args[2]

    async def fetch(self, resource, init, timeout):
        self.fetches.append((resource, init, timeout))
        if isinstance(self.upstream, BaseException):
            raise self.upstream
        return self.upstream

    async def edge_match(self, _key):
        return self.cache_hit

    async def edge_put(self, key, response):
        self.cache_puts.append((key, response))

    async def edge_delete(self, key):
        self.cache_deletes.append(key)


def _entry_namespace(harness):
    def json_response(
        data,
        status=200,
        cache_seconds=None,
        cache_control=None,
        extra_headers=None,
    ):
        return {
            "status": status,
            "data": data,
            "cacheSeconds": cache_seconds,
            "cacheControl": cache_control,
            "headers": dict(extra_headers or {}),
        }

    return _load_entry_functions(
        "_world_satellite_refresh_error_code",
        "_world_satellite_refresh_failure",
        "refresh_world_satellite_snapshot",
        "world_satellites_handler",
        extra_globals={
            "Date": SimpleNamespace(now=lambda: FETCHED_AT),
            "EXPECTED_DEGRADED_HEADERS": {
                "x-forkmesh-expected-degraded": "1"
            },
            "WORLD_SATELLITE_CACHE_KEY": "satellite-cache-v1",
            "WORLD_SATELLITE_CACHE_SECONDS": 7200,
            "WORLD_SATELLITE_FETCH_TIMEOUT_SECONDS": 20,
            "WORLD_SATELLITE_REFRESH_MIN_MS": 2 * 60 * 60 * 1000,
            "d1_first": harness.d1_first,
            "d1_run": harness.d1_run,
            "edge_cache_delete": harness.edge_delete,
            "edge_cache_match": harness.edge_match,
            "edge_cache_put": harness.edge_put,
            "ensure_schema": harness.ensure_schema,
            "hmac": hmac,
            "js_fetch_with_timeout": harness.fetch,
            "json_response": json_response,
            "method_name": lambda request: request.method,
            "re": re,
            "world_satellites": satellites,
        },
    )


def omm_record(**updates):
    record = {
        "OBJECT_NAME": "ISS (ZARYA)",
        "OBJECT_ID": "1998-067A",
        "EPOCH": "2026-07-26T12:34:56.123456",
        "MEAN_MOTION": 15.5,
        "ECCENTRICITY": 0.0004,
        "INCLINATION": 51.64,
        "RA_OF_ASC_NODE": 123.4,
        "ARG_OF_PERICENTER": 24.5,
        "MEAN_ANOMALY": 335.5,
        "NORAD_CAT_ID": "25544",
        "BSTAR": 0.0002,
        "MEAN_MOTION_DOT": 0.00001,
        "MEAN_MOTION_DDOT": 0,
        # Valid CelesTrak fields not needed by satellite.js are deliberately
        # removed from ForkMesh's compact public projection.
        "CLASSIFICATION_TYPE": "U",
        "ELEMENT_SET_NO": 999,
        "REV_AT_EPOCH": 54321,
    }
    record.update(updates)
    return record


def test_fixed_official_source_and_forward_compatible_catalog_identifier():
    assert satellites.CELESTRAK_VISUAL_OMM_URL == (
        "https://celestrak.org/NORAD/elements/"
        "gp.php?GROUP=VISUAL&FORMAT=JSON"
    )
    record = satellites.normalize_omm_record(
        omm_record(
            OBJECT_NAME="SIX DIGIT CATALOG",
            OBJECT_ID="2026-999A",
            NORAD_CAT_ID=100147,
        )
    )
    assert record["NORAD_CAT_ID"] == "100147"
    assert record["EPOCH"].endswith("Z")
    assert set(record) == set(satellites._OUTPUT_FIELDS)
    assert "ELEMENT_SET_NO" not in record


def test_raw_document_is_bounded_parsed_deduplicated_and_sorted():
    older_duplicate = omm_record(EPOCH="2026-07-25T12:00:00Z")
    newer_duplicate = omm_record(
        OBJECT_NAME="ISS newest", EPOCH="2026-07-26T13:00:00Z"
    )
    six_digit = omm_record(
        OBJECT_NAME="NEW OBJECT",
        OBJECT_ID="2026-999B",
        NORAD_CAT_ID="100148",
    )
    raw = json.dumps([six_digit, older_duplicate, newer_duplicate])
    records = satellites.parse_visual_omm_json(raw)
    assert [item["NORAD_CAT_ID"] for item in records] == [
        "25544",
        "100148",
    ]
    assert records[0]["OBJECT_NAME"] == "ISS newest"

    with pytest.raises(
        satellites.SatelliteDataError, match="celestrak_body_too_large"
    ):
        satellites.parse_visual_omm_json(
            b" " * (satellites.MAX_CELESTRAK_RESPONSE_BYTES + 1)
        )

    too_many = [
        omm_record(
            NORAD_CAT_ID=str(index + 1),
            OBJECT_NAME=f"SAT {index + 1}",
        )
        for index in range(satellites.MAX_SATELLITE_RECORDS + 1)
    ]
    with pytest.raises(
        satellites.SatelliteDataError, match="too_many_satellites"
    ):
        satellites.parse_visual_omm_json(json.dumps(too_many))


@pytest.mark.parametrize(
    ("updates", "error"),
    [
        ({"NORAD_CAT_ID": "001001"}, "invalid_catalog_id"),
        ({"NORAD_CAT_ID": "1000000000"}, "invalid_catalog_id"),
        ({"EPOCH": "2026-07-26 12:34:56"}, "invalid_epoch"),
        ({"MEAN_MOTION": 0}, "invalid_mean_motion"),
        ({"ECCENTRICITY": 1}, "invalid_eccentricity"),
        ({"INCLINATION": 181}, "invalid_inclination"),
        ({"RA_OF_ASC_NODE": -1}, "invalid_ra_of_asc_node"),
        ({"MEAN_ANOMALY": float("inf")}, "invalid_mean_anomaly"),
        ({"OBJECT_NAME": ""}, "missing_satellite_text"),
        ({"OBJECT_ID": "../secret"}, "invalid_object_id"),
    ],
)
def test_record_validation_fails_closed(updates, error):
    with pytest.raises(satellites.SatelliteDataError, match=error):
        satellites.normalize_omm_record(omm_record(**updates))


def test_json_parser_rejects_non_json_constants_and_wrong_top_level_shapes():
    with pytest.raises(
        satellites.SatelliteDataError, match="invalid_celestrak_number"
    ):
        satellites.parse_visual_omm_json(
            json.dumps([omm_record()]).replace("15.5", "NaN")
        )
    with pytest.raises(
        satellites.SatelliteDataError, match="invalid_celestrak_payload"
    ):
        satellites.parse_visual_omm_json("{}")
    with pytest.raises(
        satellites.SatelliteDataError, match="empty_celestrak_payload"
    ):
        satellites.parse_visual_omm_json("[]")


def test_snapshot_is_canonical_compact_and_round_trips_with_digest():
    raw = json.dumps(
        [
            omm_record(),
            omm_record(
                OBJECT_NAME="SIX DIGIT",
                OBJECT_ID="2026-999A",
                EPOCH="2026-07-26T14:00:00Z",
                NORAD_CAT_ID="100147",
            ),
        ]
    )
    payload = satellites.build_snapshot_from_json(raw, FETCHED_AT)
    assert payload["ok"] is True
    assert payload["schemaVersion"] == 1
    assert payload["recordCount"] == 2
    assert payload["source"]["url"] == satellites.CELESTRAK_VISUAL_OMM_URL
    assert payload["sourceEpoch"] == "2026-07-26T14:00:00Z"

    document, digest = satellites.serialize_snapshot_payload(payload)
    assert "\n" not in document
    assert len(document.encode("utf-8")) <= 512 * 1024
    assert len(digest) == 64
    assert satellites.parse_snapshot_document(document) == payload
    assert satellites.serialize_snapshot_payload(payload) == (
        document,
        digest,
    )


def test_snapshot_rejects_changed_source_and_unbounded_or_invalid_time():
    payload = satellites.build_snapshot_payload([omm_record()], FETCHED_AT)
    payload["source"] = {
        **payload["source"],
        "url": "https://attacker.invalid/orbits.json",
    }
    with pytest.raises(
        satellites.SatelliteDataError, match="invalid_snapshot_source"
    ):
        satellites.normalize_snapshot_payload(payload)

    for invalid in (0, -1, True, float(FETCHED_AT), 10**20):
        value = satellites.build_snapshot_payload(
            [omm_record()], FETCHED_AT
        )
        value["fetchedAt"] = invalid
        with pytest.raises(
            satellites.SatelliteDataError, match="invalid_snapshot_time"
        ):
            satellites.normalize_snapshot_payload(value)


def test_stored_snapshot_rejects_changed_schema_or_summary():
    payload = satellites.build_snapshot_payload([omm_record()], FETCHED_AT)
    payload["schemaVersion"] = 2
    with pytest.raises(
        satellites.SatelliteDataError, match="invalid_snapshot_schema"
    ):
        satellites.normalize_snapshot_payload(payload)

    payload = satellites.build_snapshot_payload([omm_record()], FETCHED_AT)
    payload["recordCount"] = 99
    with pytest.raises(
        satellites.SatelliteDataError, match="invalid_snapshot_summary"
    ):
        satellites.normalize_snapshot_payload(payload)


def test_migration_enforces_one_bounded_public_snapshot_row():
    db = sqlite3.connect(":memory:")
    db.executescript(
        (ROOT / "migrations" / "0084_world_satellite_snapshot.sql").read_text(
            encoding="utf-8"
        )
    )
    payload = satellites.build_snapshot_payload([omm_record()], FETCHED_AT)
    document, digest = satellites.serialize_snapshot_payload(payload)
    db.execute(
        "INSERT INTO world_satellite_snapshot "
        "(snapshot_id,data,digest,fetched_at,source_epoch,last_attempt_at) "
        "VALUES (1,?,?,?,?,?)",
        (
            document,
            digest,
            FETCHED_AT,
            payload["sourceEpoch"],
            FETCHED_AT,
        ),
    )
    assert db.execute(
        "SELECT COUNT(*) FROM world_satellite_snapshot"
    ).fetchone()[0] == 1

    with pytest.raises(sqlite3.IntegrityError):
        db.execute(
            "INSERT INTO world_satellite_snapshot "
            "(snapshot_id,data,digest,fetched_at,source_epoch,last_attempt_at) "
            "VALUES (2,?,?,?,?,?)",
            (
                document,
                digest,
                FETCHED_AT,
                payload["sourceEpoch"],
                FETCHED_AT,
            ),
        )
    with pytest.raises(sqlite3.IntegrityError):
        db.execute(
            "UPDATE world_satellite_snapshot SET last_error=? "
            "WHERE snapshot_id=1",
            ("x" * 241,),
        )


def test_runtime_schema_contains_the_same_single_row_snapshot_contract():
    statement = next(
        sql
        for sql in SCHEMA_STATEMENTS
        if "CREATE TABLE IF NOT EXISTS world_satellite_snapshot" in sql
    )
    db = sqlite3.connect(":memory:")
    db.execute(statement)
    columns = {
        row[1] for row in db.execute(
            "PRAGMA table_info(world_satellite_snapshot)"
        )
    }
    assert columns == {
        "snapshot_id",
        "data",
        "digest",
        "fetched_at",
        "source_epoch",
        "last_attempt_at",
        "last_status",
        "last_error",
    }


def test_cron_and_route_are_wired_without_visitor_upstream_fetching():
    refresh_source = _entry_function_source(
        "refresh_world_satellite_snapshot"
    )
    handler_source = _entry_function_source("world_satellites_handler")
    assert "world_satellites.CELESTRAK_VISUAL_OMM_URL" in refresh_source
    assert "js_fetch_with_timeout" in refresh_source
    assert "js_fetch" not in handler_source
    assert "refresh_world_satellite_snapshot" not in handler_source
    assert '"/api/world/satellites"' in ENTRY_TEXT
    assert "WORLD_SATELLITE_REFRESH_CRON_MINUTES = 3 * 60" in ENTRY_TEXT
    assert (
        "minute % WORLD_SATELLITE_REFRESH_CRON_MINUTES" in ENTRY_TEXT
    )
    assert "await refresh_world_satellite_snapshot(" in ENTRY_TEXT


def test_scheduled_refresh_writes_valid_snapshot_and_honors_two_hour_gate():
    raw = json.dumps(
        [
            omm_record(),
            omm_record(
                OBJECT_NAME="SIX DIGIT",
                OBJECT_ID="2026-999A",
                NORAD_CAT_ID=100147,
            ),
        ]
    )
    harness = EntryHarness(Upstream(raw))
    namespace = _entry_namespace(harness)
    refresh = namespace["refresh_world_satellite_snapshot"]

    result = asyncio.run(refresh(object(), FETCHED_AT))
    assert result["status"] == "updated"
    assert result["recordCount"] == 2
    assert harness.fetches == [
        (
            satellites.CELESTRAK_VISUAL_OMM_URL,
            {
                "method": "GET",
                "headers": {"accept": "application/json"},
                "redirect": "manual",
            },
            20,
        )
    ]
    stored = satellites.parse_snapshot_document(harness.row["data"])
    assert stored["satellites"][1]["NORAD_CAT_ID"] == "100147"
    assert harness.row["digest"] == result["digest"]
    assert harness.cache_deletes == ["satellite-cache-v1"]

    fresh = asyncio.run(refresh(object(), FETCHED_AT + 60 * 60 * 1000))
    assert fresh["status"] == "fresh"
    assert len(harness.fetches) == 1


def test_upstream_failure_preserves_last_good_document_and_records_status():
    payload = satellites.build_snapshot_payload(
        [omm_record()], FETCHED_AT
    )
    document, digest = satellites.serialize_snapshot_payload(payload)
    row = {
        "data": document,
        "digest": digest,
        "fetched_at": FETCHED_AT,
        "source_epoch": payload["sourceEpoch"],
        "last_attempt_at": FETCHED_AT,
        "last_status": 200,
        "last_error": "",
    }
    harness = EntryHarness(
        Upstream("rate limited", status=403),
        row=row,
    )
    namespace = _entry_namespace(harness)
    result = asyncio.run(
        namespace["refresh_world_satellite_snapshot"](
            object(), FETCHED_AT + 3 * 60 * 60 * 1000
        )
    )
    assert result == {
        "ok": False,
        "status": "upstream_error",
        "upstreamStatus": 403,
        "error": "upstream_http_status",
    }
    assert harness.row["data"] == document
    assert harness.row["digest"] == digest
    assert harness.row["last_status"] == 403
    assert harness.row["last_error"] == "upstream_http_status"
    assert harness.cache_deletes == []


def test_oversize_success_response_preserves_last_good_without_reading_body():
    class UnreadableUpstream(Upstream):
        async def text(self):
            raise AssertionError("oversize body must not be read")

    payload = satellites.build_snapshot_payload(
        [omm_record()], FETCHED_AT
    )
    document, digest = satellites.serialize_snapshot_payload(payload)
    harness = EntryHarness(
        UnreadableUpstream(
            status=200,
            content_length=satellites.MAX_CELESTRAK_RESPONSE_BYTES + 1,
        ),
        row={
            "data": document,
            "digest": digest,
            "fetched_at": FETCHED_AT,
            "source_epoch": payload["sourceEpoch"],
            "last_attempt_at": FETCHED_AT,
            "last_status": 200,
            "last_error": "",
        },
    )
    namespace = _entry_namespace(harness)
    result = asyncio.run(
        namespace["refresh_world_satellite_snapshot"](
            object(), FETCHED_AT + 3 * 60 * 60 * 1000
        )
    )
    assert result["upstreamStatus"] == 502
    assert result["error"] == "celestrak_body_too_large"
    assert harness.row["data"] == document
    assert harness.row["digest"] == digest


def test_public_handler_serves_d1_snapshot_then_edge_cache_without_fetch():
    payload = satellites.build_snapshot_payload(
        [omm_record(NORAD_CAT_ID=100147)], FETCHED_AT
    )
    document, digest = satellites.serialize_snapshot_payload(payload)
    harness = EntryHarness(
        # Any accidental visitor-path fetch would fail this test.
        upstream=AssertionError("visitor handler must not fetch upstream"),
        row={"data": document, "digest": digest},
    )
    namespace = _entry_namespace(harness)
    handler = namespace["world_satellites_handler"]
    request = SimpleNamespace(method="GET")

    response = asyncio.run(handler(object(), request))
    assert response["status"] == 200
    assert response["data"]["satellites"][0]["NORAD_CAT_ID"] == "100147"
    assert response["cacheControl"] == (
        "public, max-age=7200, stale-while-revalidate=86400"
    )
    assert harness.fetches == []
    assert len(harness.cache_puts) == 1

    cached = {"cached": True}
    harness.cache_hit = cached
    first_calls = harness.first_calls
    assert asyncio.run(handler(object(), request)) is cached
    assert harness.first_calls == first_calls
    assert harness.fetches == []


def test_public_handler_returns_empty_degraded_sky_for_missing_or_tampered_snapshot():
    harness = EntryHarness(row=None)
    namespace = _entry_namespace(harness)
    handler = namespace["world_satellites_handler"]
    response = asyncio.run(
        handler(object(), SimpleNamespace(method="GET"))
    )
    assert response["status"] == 200
    assert response["data"]["ok"] is True
    assert response["data"]["status"] == "warming"
    assert response["data"]["recordCount"] == 0
    assert response["data"]["satellites"] == []
    assert response["headers"]["x-forkmesh-expected-degraded"] == "1"
    assert harness.fetches == []

    payload = satellites.build_snapshot_payload(
        [omm_record()], FETCHED_AT
    )
    document, _digest = satellites.serialize_snapshot_payload(payload)
    harness.row = {"data": document, "digest": "0" * 64}
    harness.cache_hit = None
    response = asyncio.run(
        handler(object(), SimpleNamespace(method="GET"))
    )
    assert response["status"] == 200
    assert response["data"]["satellites"] == []
    assert harness.fetches == []

    denied = asyncio.run(
        handler(object(), SimpleNamespace(method="POST"))
    )
    assert denied["status"] == 405
    assert denied["headers"]["allow"] == "GET"
