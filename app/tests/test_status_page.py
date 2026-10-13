#!/usr/bin/env python3
"""Public /status page contracts (30-day per-system uptime history).

record_status_sample (called once a minute by the cron) folds one health
check per system into today's UTC-day bucket; status_history reads those
buckets back into the 30-day series the /status page renders. These tests
load the real functions straight out of src/entry.py (no Workers runtime)
and drive them against in-memory D1 stubs.
"""

import ast
import asyncio
import base64
import hashlib
import json
import sqlite3
import sys
import time
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import quote, urlparse

import pytest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
import public_snapshots as snapshot_contract  # noqa: E402
from _maintenance_stubs import with_maintenance_off

WWW_PUBLIC = ROOT.parent / "www" / "public"
ENTRY = ROOT / "src" / "entry.py"
# SCHEMA_STATEMENTS (D1 DDL) was extracted from entry.py into schema.py;
# concatenate it so the schema source-contract assertions below still resolve.
SCHEMA = ENTRY.parent / "schema.py"
# Tests inspect both startup and request-time source. Keep entry.py first so
# source-order contracts can treat the lazy Default class as the end of the
# router implementation, matching the pre-extraction layout.
ENTRY_TEXT = (
    ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "app_runtime.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.with_name("status_monitoring.py").read_text(encoding="utf-8")
    + "\n"
    + SCHEMA.read_text(encoding="utf-8")
)
# Route regexes now live in the extracted urls.py module (imported by entry.py);
# parse it alongside entry.py so the assign nodes below still resolve.
URLS = ROOT / "src" / "urls.py"
URLS_TEXT = URLS.read_text(encoding="utf-8")

DAY_MS = 86400000


def _load(*names, extra_globals=None):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    urls_tree = ast.parse(URLS_TEXT, filename=str(URLS))
    want_assigns = {
        "ROOM_RE", "REPO_ROOM_RE", "GIT_INFO_RE", "GIT_PACK_RE",
        "RELEASE_BLOB_RE", "REPO_HOST_RE", "GIT_RECEIVE_RE",
        "HOST_PRESENCE_STALE_MS", "HTTPS_MIRROR_STATUS_FRESH_MS",
        "STATUS_SYSTEMS", "STATUS_SYSTEM_CHECKS",
        "STATUS_SYSTEM_CHECK_STEPS", "STATUS_MIRROR_CHECK_STEPS",
        "STATUS_SYSTEM_PROBE_PHASES", "STATUS_SAMPLE_PHASE",
        "STATUS_NEXT_CHECK_SCAN_MINUTES",
        "STATUS_HISTORY_DAYS",
        "STATUS_HISTORY_RETAIN_MS", "STATUS_SAMPLE_WINDOW_MS",
        "STATUS_HOUR_MS", "STATUS_DAY_MS",
        "STATUS_MINUTES_SHOWN", "STATUS_MINUTE_RETAIN_MS",
        "STATUS_BATCH_SCHEMA_VERSION", "STATUS_BATCH_MAX_BYTES",
        "STATUS_COMPACTION_RETRY_MS",
        "STATUS_EDGE_CACHE_TTL", "STATUS_EDGE_CACHE_KEYS",
        "STATUS_ASSET_FETCH_TIMEOUT_MS",
        "STATUS_SYNTHETIC_HEALTHY_INTERVAL_MS",
        "STATUS_SYNTHETIC_FAILED_RETRY_MS",
        "STATUS_HOMEPAGE_MONITOR_ID", "STATUS_WORLD_MOBILE_MONITOR_ID",
        "STATUS_PAGE_MONITOR_ID",
        "STATUS_MIRROR_PREFIX", "STATUS_MIRROR_MAX",
        "STATUS_MIRROR_ROSTER_FRESH_MS",
        "STATUS_RETIRED_MIRRORS", "STATUS_SYSTEM_SUCCESSORS",
        "STATUS_CORE_PREFIX", "STATUS_CORE_MAX",
        "STATUS_CORE_ROSTER_FRESH_MS", "STATUS_CORE_HEARTBEAT_FRESH_MS",
        "STATUS_CORE_VOTE_FRESH_MS", "STATUS_CORE_IDS_FRESH_MS",
        "STATUS_CORE_CHECK_STEPS",
        "STATUS_RUNNER_PREFIX", "STATUS_RUNNER_MAX",
        "STATUS_RUNNER_SEEN_FRESH_MS", "STATUS_RUNNER_CHECK_STEPS",
        "STATUS_DEPLOY_GRACE_MS", "STATUS_DEPLOY_MAX_MS",
        "MOBILE_WORLD_HEALTH_PATH", "MOBILE_WORLD_HEALTH_MAX_BYTES",
        "MOBILE_WORLD_HEALTH_ASSETS", "MOBILE_WORLD_STATUS_USER_AGENT",
        "FLAGSHIP_PUBLIC_READ_ROUTE_GROUP", "STATUS_EVENT_SYSTEMS",
        "FLAGSHIP_PUBLIC_READ_FAILURE_THRESHOLD",
        "WORLD_SATELLITE_REFRESH_CRON_MINUTES",
        "WORLD_SATELLITE_REFRESH_CRON_OFFSET",
        "PUBLIC_SNAPSHOT_FREQUENT_CADENCE_MINUTES",
        "PUBLIC_SNAPSHOT_SLOW_CADENCE_MINUTES",
        "FLAGSHIP_DEEP_MONITOR_ID", "FLAGSHIP_CHECK_INTERVAL_MINUTES",
        "FLAGSHIP_CHECK_INTERVAL_MS",
        "FLAGSHIP_RESULT_STALE_MS",
        "FLAGSHIP_FAILED_RETRY_MS", "FLAGSHIP_FAILURE_CONFIRM_MS",
        "INSTALLER_RELEASE_PUBLISHER_PUBLIC_KEY",
        "RELEASE_PLATFORM_MONITORS",
        "RELEASE_PLATFORM_CHECK_INTERVAL_MINUTES",
        "RELEASE_PLATFORM_CHECK_INTERVAL_MS",
        "RELEASE_PLATFORM_RESULT_STALE_MS",
        "STATUS_PAGE_ASSETS",
        "ONLINE_SAMPLE_CADENCE_MINUTES", "ONLINE_SAMPLE_WEIGHT_MINUTES",
        "ONLINE_HISTORY_MAX_NODES", "ONLINE_HISTORY_RETAIN_MS",
        "EMAIL_STATUS_LOOKBACK_MS", "EMAIL_DELIVERY_GRACE_MS",
        "EMAIL_DELIVERY_FAILURE_STATES",
        "EMAIL_DELIVERY_CONFIRMED_STATES",
        "EMAIL_RECIPIENT_ADDRESS_DSNS", "EMAIL_DSN_CAUSES",
        "STATUS_EMAIL_SMTP_CHECK", "STATUS_EMAIL_SMTP_CHECK_STEPS",
        "STATUS_EMAIL_SMTP_GUIDANCE",
        "MAILTRAP_EMAIL_LOG_URL", "MAILTRAP_LOG_SETTLED_STATUS",
        "CRON_OUTBOUND_FETCH_TIMEOUT_SECONDS",
    }
    helper_names = {
        "_cron_due_phases", "_status_phase_slots", "_status_next_check",
        "_status_expected_checks_for_hour", "_status_effective_hour",
        "_status_minute", "_is_tunnel_content_path",
        "_status_deploy_semaphore_active",
        "_deployment_status_grace_active",
        "_record_status_deploy_sample",
        "_claim_status_sample_minute",
        "_email_delivery_status", "_status_page_api_probe",
        "_mail_over_smtp", "_mail_dsn_cause",
        "_status_page_static_assets_probe",
        "_mailtrap_logged_delivery", "_mailtrap_log_message",
        "_status_batch_system_id", "_status_batch_decode",
        "_status_batch_retired_mirror",
        "_status_batch_encode", "_status_minute_records",
        "_status_hour_records", "_status_day_records",
        "_write_status_minute_batch", "_status_oldest_missing_batch",
        "_compact_status_hour",
        "_compact_status_day", "_status_recent_system_ids",
        "_status_mirror_name", "_status_active_mirror_rows",
        "_status_core_name", "_status_core_rows",
        "_status_core_vote_detail", "_status_core_verdicts",
        "_status_core_health", "_flagship_monitor_duration",
        "_status_runner_rows", "_status_runner_verdicts",
        "_status_runner_health",
        "record_status_compaction",
        "_flagship_public_read_failures", "_status_carried_failures",
        "_status_deploy_recovered_systems",
        "_cached_synthetic_status_probe",
        "_status_probe_failure_confirmed", "_status_probe_verdict",
        "_discard_response_body", "js_nullish",
    }
    selected = []
    for node in list(urls_tree.body) + list(tree.body):
        if isinstance(node, (ast.Import, ast.ImportFrom)) and any(
            alias.name == "re" for alias in node.names
        ):
            selected.append(node)
        elif isinstance(node, ast.Assign):
            targets = {t.id for t in node.targets if isinstance(t, ast.Name)}
            if targets & want_assigns:
                selected.append(node)
        elif (
            isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            and node.name in set(names) | helper_names
        ):
            selected.append(node)
    found = {n.name for n in selected if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))}
    missing = set(names) - found
    assert not missing, "missing functions: %s" % sorted(missing)
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    namespace = {
        "json": json,
        "time": time,
        "quote": quote,
        "public_snapshots": snapshot_contract,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        **dict(extra_globals or {}),
    }
    exec(compile(module, str(ENTRY), "exec"), with_maintenance_off(namespace))
    return namespace


class _Clock:
    value = 1_700_000_000_000  # arbitrary fixed instant

    @classmethod
    def now(cls):
        return cls.value


def _sample_env(
        now, error_paths, host_online=True, db_ok=True, error_rows=None,
        mirror_rows=None, latest_email=None, do_abort_rows=None,
        status_api_ok=True, world_mobile_ok=True, mailtrap_log=None,
        log_calls=None,
        flagship_failures=None, flagship_probe=None, reads=None,
        core_rows=None, vote_rows=None, ids_rows=None,
        runner_rows=None, lease_rows=None, prior_monitor_rows=None):
    """Stub error rows plus the signed direct-HTTPS mirror health count."""
    inserted = []
    hourly = []
    minutely = []

    async def d1_first(_env, sql, *args):
        if "flagship_read_failure_minute" in sql:
            if reads is not None:
                reads.append((sql, args))
            return {"failures": flagship_failures} if flagship_failures else None
        if sql.strip() == "SELECT 1 AS ok":
            if not db_ok:
                raise RuntimeError("db down")
            return {"ok": 1}
        if "mirror_https_endpoints" in sql:
            return {"n": 1 if host_online else 0}
        if "mailtrap_email_sends" in sql:
            return dict(latest_email) if latest_email else None
        if "FROM repository_monitor_state" in sql and prior_monitor_rows:
            row = prior_monitor_rows.get(args[0])
            return dict(row) if row else None
        return {}

    async def d1_all(_env, sql, *args):
        if reads is not None:
            reads.append((sql, args))
        if "durable_object_abort_minute" in sql:
            return list(do_abort_rows or [])
        if "error_log" in sql:
            if error_rows is not None:
                return error_rows
            return [{"path": p} for p in error_paths]
        if "mirror_https_endpoints" in sql:
            return list(mirror_rows or [])
        if "FROM cluster_members" in sql:
            if isinstance(core_rows, Exception):
                raise core_rows
            return [
                row for row in core_rows or []
                if int(row.get("heartbeat_ms") or 0) >= args[0]]
        if "FROM dns_health_votes" in sql:
            return [
                row for row in vote_rows or []
                if int(row.get("observed_ms") or 0) >= args[0]]
        if "FROM ids_heartbeats" in sql:
            return list(ids_rows or [])
        if "FROM runner_nodes" in sql:
            if isinstance(runner_rows, Exception):
                raise runner_rows
            return [
                row for row in runner_rows or []
                if row.get("scope", "pool") == "pool"
                and row.get("status", "active") == "active"]
        if "FROM runner_job_leases" in sql:
            return list(lease_rows or [])
        return []

    async def d1_run(_env, sql, *args):
        # Decode the one physical minute batch back into logical systems so
        # the probe-behaviour assertions below stay independent of storage.
        if sql.startswith("INSERT INTO system_status_minute_batch"):
            payload = json.loads(args[1])
            for system, item in payload["systems"].items():
                ok = 1 if item["ok"] else 0
                minutely.append({
                    "system": system, "ok": ok, "reason": item["reason"]})
                inserted.append({"system": system, "failure": 0 if ok else 1})
                hourly.append({
                    "system": system, "failure": 0 if ok else 1,
                    "reason": item["reason"],
                })
            return
        if sql.startswith((
            "INSERT INTO system_status_hourly_batch",
            "INSERT INTO system_status_daily_batch",
        )):
            return
        # Legacy shapes retained for isolated compatibility fixtures.
        if sql.startswith("INSERT INTO system_status_daily"):
            for i in range(0, len(args), 3):
                inserted.append({"system": args[i + 1], "failure": args[i + 2]})
        elif sql.startswith("INSERT INTO system_status_hourly"):
            for i in range(0, len(args), 4):
                hourly.append({"system": args[i + 1], "failure": args[i + 2],
                               "reason": args[i + 3]})
        elif sql.startswith("INSERT INTO system_status_minute"):
            for i in range(0, len(args), 4):
                minutely.append({"system": args[i + 1], "ok": args[i + 2],
                                 "reason": args[i + 3]})

    async def noop(*_a, **_k):
        return None
    async def repository_probe(_env):
        if flagship_probe is not None:
            return flagship_probe
        return True, ""
    async def repository_status(_env, _now, allow_probe=True):
        assert isinstance(allow_probe, bool)
        return await repository_probe(_env)
    async def release_platform_status(_env, _now, system_id,
                                      allow_probe=True):
        assert system_id.startswith("release_")
        assert isinstance(allow_probe, bool)
        return True, ""
    async def flagship_artifact_counts(_env, _now):
        expected = 0
        counts = {}
        for row in mirror_rows or []:
            node = str(row.get("node_name") or "").strip().lower()
            if not node or "artifact_count" not in row:
                continue
            count = int(row.get("artifact_count") or 0)
            counts[node] = count
            expected = max(
                expected, int(row.get("source_artifact_count") or count))
        return expected, counts
    async def homepage_status(_env):
        return True, ""
    async def mobile_world_status(_env):
        if world_mobile_ok:
            return True, ""
        return False, "Mobile World bootstrap asset is unavailable"

    class _StatusAssetResponse:
        def __init__(self, body):
            self.status = 200
            self._body = body
            self.headers = {"content-length": str(len(body.encode("utf-8")))}

        async def text(self):
            return self._body

    class _StatusAssets:
        async def fetch(self, url):
            path = urlparse(str(url)).path
            if path == "/status.html":
                return _StatusAssetResponse(
                    '<h1 id="status-title"></h1><script>/api/status/ws</script>')
            if path == "/styles.css":
                return _StatusAssetResponse(
                    ".status-banner{} .status-history-row{}")
            return SimpleNamespace(status=404, headers={}, text=lambda: "")

    async def public_snapshot_row(_env, _path):
        if not status_api_ok:
            raise RuntimeError("dedicated status row unavailable")
        return None

    async def mailtrap_log_fetch(url, init, _timeout):
        # Mailtrap's Email Logs API. Absent a fixture it answers "no such
        # message", which is what a genuinely undelivered send looks like.
        if log_calls is not None:
            log_calls.append((url, dict(init.get("headers") or {})))

        async def text():
            return json.dumps(
                mailtrap_log if mailtrap_log is not None else {"messages": []})
        return SimpleNamespace(status=200, text=text)

    extra = {
        "Date": _Clock,
        "ensure_schema": noop,
        "d1_first": d1_first,
        "d1_all": d1_all,
        "d1_run": d1_run,
        "_flagship_repository_probe": repository_probe,
        "_flagship_repository_status": repository_status,
        "_record_status_monitor_transitions": noop,
        "_release_platform_status": release_platform_status,
        "_status_flagship_artifact_counts": flagship_artifact_counts,
        "_homepage_status_probe": homepage_status,
        "_mobile_world_status_probe": mobile_world_status,
        "_public_snapshot_row": public_snapshot_row,
        "js_fetch_with_timeout": mailtrap_log_fetch,
        "_safe_error_text": lambda error: str(error),
        "_cron_subrequest_charge": lambda count=1: None,
        "JsAbortSignal": SimpleNamespace(timeout=lambda value: value),
        "JsRequest": SimpleNamespace(new=lambda url, *_args: url),
        "to_js": lambda value: value,
        "_status_assets": _StatusAssets(),
        "clean_string": lambda value, limit: str(value or "")[:limit],
    }
    return extra, inserted, hourly, minutely


def _run_sample(
        error_paths=(), host_online=True, db_ok=True, error_rows=None,
        mirror_rows=None, latest_email=None, email_configured=True,
        do_abort_rows=None, status_api_ok=True, mailtrap_log=None,
        world_mobile_ok=True, log_calls=None, writes=None,
        flagship_failures=None,
        flagship_probe=None, deployed_at_ms=0, reads=None,
        core_rows=None, vote_rows=None, ids_rows=None,
        runner_rows=None, lease_rows=None, prior_monitor_rows=None):
    extra, inserted, hourly, minutely = _sample_env(
        _Clock.value, error_paths, host_online, db_ok,
        error_rows=error_rows, mirror_rows=mirror_rows,
        latest_email=latest_email,
        do_abort_rows=do_abort_rows,
        status_api_ok=status_api_ok,
        world_mobile_ok=world_mobile_ok,
        mailtrap_log=mailtrap_log,
        log_calls=log_calls,
        flagship_failures=flagship_failures,
        flagship_probe=flagship_probe,
        reads=reads,
        core_rows=core_rows, vote_rows=vote_rows, ids_rows=ids_rows,
        runner_rows=runner_rows, lease_rows=lease_rows,
        prior_monitor_rows=prior_monitor_rows,
    )
    if writes is not None:
        inner = extra["d1_run"]

        async def d1_run(env, sql, *args):
            writes.append((sql, args))
            return await inner(env, sql, *args)
        extra["d1_run"] = d1_run
    g = _load("record_status_sample", extra_globals=extra)
    env = SimpleNamespace(
        MAILTRAP_API_TOKEN="test-token" if email_configured else "",
        MAILTRAP_WEBHOOK_SECRET="test-secret" if email_configured else "",
        DEPLOYED_AT_MS=deployed_at_ms,
        WORLD_ORIGIN="https://world.forkmesh.com",
        ASSETS=extra["_status_assets"],
    )
    asyncio.run(g["record_status_sample"](env))
    return (
        {row["system"]: row["failure"] for row in inserted},
        {row["system"]: row["reason"] for row in hourly},
        {row["system"]: (row["ok"], row["reason"]) for row in minutely},
    )


# --- record_status_sample ---------------------------------------------------

def test_all_systems_recorded_ok_with_no_errors_and_a_live_https_mirror():
    results, reasons, _minutes = _run_sample(error_paths=[], host_online=True, db_ok=True)
    assert set(results) == {
        "website", "world_mobile", "status_page", "api", "errors", "database",
        "flagship_repository", "email", "release_linux", "release_android",
        "release_windows", "release_macos",
        "git_hosting", "realtime", "durable_objects",
    }
    assert all(failure == 0 for failure in results.values())
    assert all(reason is None for reason in reasons.values())


def _failed_last_minute(monitor_id):
    return {monitor_id: {
        "is_up": 0, "reason": "failed last minute",
        "changed_at": _Clock.value - 120_000,
        "checked_at": _Clock.value - 60_000,
    }}


def test_status_page_api_failure_is_a_dedicated_red_monitor():
    results, _reasons, _minutes = _run_sample(status_api_ok=False)
    assert results["status_page"] == 0

    results, reasons, minutes = _run_sample(
        status_api_ok=False,
        prior_monitor_rows=_failed_last_minute("synthetic-status-page-probe"))

    assert results["status_page"] == 1
    assert reasons["status_page"] == (
        "Published status API snapshot is unavailable: "
        "dedicated status row unavailable")
    assert minutes["status_page"] == (
        0, "Published status API snapshot is unavailable: "
           "dedicated status row unavailable")


def test_mobile_world_failure_is_a_dedicated_red_monitor():
    results, _reasons, _minutes = _run_sample(world_mobile_ok=False)
    assert results["world_mobile"] == 0

    results, reasons, minutes = _run_sample(
        world_mobile_ok=False,
        prior_monitor_rows=_failed_last_minute("synthetic-world-mobile-probe"))

    assert results["world_mobile"] == 1
    assert reasons["world_mobile"] == (
        "Mobile World bootstrap asset is unavailable")
    assert minutes["world_mobile"] == (
        0, "Mobile World bootstrap asset is unavailable")
    assert all(
        failure == 0
        for system, failure in results.items()
        if system != "world_mobile"
    )


def test_healthy_synthetic_probes_are_reused_and_failures_retry_next_minute():
    now = _Clock.value
    stored = {"is_up": 1, "reason": "", "checked_at": now - 1_000}
    probes = []
    writes = []

    async def d1_first(_env, _sql, *_args):
        return dict(stored)

    async def d1_run(_env, sql, *args):
        writes.append((sql, args))

    async def probe(_env):
        probes.append(True)
        return True, ""

    runtime = _load(
        "_cached_synthetic_status_probe",
        extra_globals={"d1_first": d1_first, "d1_run": d1_run},
    )
    cached = runtime["_cached_synthetic_status_probe"]
    monitor_id = runtime["STATUS_WORLD_MOBILE_MONITOR_ID"]

    assert asyncio.run(cached(object(), monitor_id, now, probe)) == (True, "")
    assert probes == [] and writes == []

    stored["checked_at"] = (
        now - runtime["STATUS_SYNTHETIC_HEALTHY_INTERVAL_MS"])
    assert asyncio.run(cached(object(), monitor_id, now, probe)) == (True, "")
    assert len(probes) == 1 and len(writes) == 1

    probes.clear()
    writes.clear()
    stored.update({
        "is_up": 0,
        "reason": "world unavailable",
        "checked_at": now - 1_000,
    })
    assert asyncio.run(cached(object(), monitor_id, now, probe)) == (
        False, "world unavailable")
    assert probes == [] and writes == []

    stored["checked_at"] = (
        now - runtime["STATUS_SYNTHETIC_FAILED_RETRY_MS"])
    assert asyncio.run(cached(object(), monitor_id, now, probe)) == (True, "")
    assert len(probes) == 1 and len(writes) == 1


def test_mobile_world_probe_uses_mobile_profile_and_requires_full_graph():
    calls = []
    charges = []
    status = [200]
    reply = {}

    class Headers:
        def __init__(self, values):
            self.values = {
                str(key).lower(): value for key, value in values.items()
            }

        def get(self, name):
            return self.values.get(str(name).lower())

    class Response:
        def __init__(self, body):
            self.status = status[0]
            self.body = body
            self.headers = Headers({
                "content-length": str(len(body.encode("utf-8"))),
                "x-forkmesh-worker": "world",
            })

        async def text(self):
            return self.body

    async def fetch(url, init, timeout):
        calls.append((url, init, timeout))
        return Response(json.dumps(reply))

    runtime = _load(
        "_mobile_world_status_probe",
        extra_globals={
            "_normalized_http_origin": lambda value: str(value).rstrip("/"),
            "_cron_subrequest_charge": lambda: charges.append(1),
            "js_fetch_with_timeout": fetch,
        },
    )
    reply.update({
        "ok": True,
        "check": "mobile-world-startup",
        "profile": "mobile",
        "sameOriginRuntime": True,
        "threeRevision": "184",
        "assets": [
            {"path": path, "ok": True}
            for path in runtime["MOBILE_WORLD_HEALTH_ASSETS"]
        ],
        "reason": None,
    })
    probe = runtime["_mobile_world_status_probe"]
    env = SimpleNamespace(WORLD_ORIGIN="https://world.forkmesh.com/")

    assert asyncio.run(probe(env)) == (True, "")
    assert charges == [1]
    assert len(calls) == 1
    url, init, timeout = calls[0]
    assert url == "https://world.forkmesh.com/health/mobile"
    assert init["redirect"] == "manual"
    assert init["headers"]["x-forkmesh-probe-profile"] == "mobile"
    assert "iPhone" in init["headers"]["user-agent"]
    assert timeout == runtime["CRON_OUTBOUND_FETCH_TIMEOUT_SECONDS"]

    reply["assets"] = reply["assets"][:-1]
    assert asyncio.run(probe(env)) == (
        False, "Mobile World health did not check the complete asset graph")

    status[0] = 503
    reply["reason"] = "three.core.min.js returned HTTP 404"
    assert asyncio.run(probe(env)) == (
        False,
        "Mobile World health returned HTTP 503: "
        "three.core.min.js returned HTTP 404",
    )


def test_status_page_monitor_pages_continually_by_default():
    assert '("status_page", "Status page API")' in ENTRY_TEXT
    assert 'system_id in ("website", "status_page")' in ENTRY_TEXT


def test_deploy_semaphore_records_a_neutral_minute_without_alerting():
    calls = []

    async def d1_first(_env, sql, *_args):
        if "world_deploy_status" in sql:
            return {
                "state": "deploying",
                "started_at": _Clock.value,
                "finished_at": 0,
            }
        return {}

    async def d1_all(_env, _sql, *_args):
        return [{
            "node_name": "mirror10", "forkmesh_active": 1,
            "forkmesh_verified_at": _Clock.value,
        }]

    async def d1_run(_env, sql, *args):
        calls.append((sql, args))

    runtime = _load(
        "_status_deploy_semaphore_active",
        "_record_status_deploy_sample",
        extra_globals={
            "d1_first": d1_first,
            "d1_all": d1_all,
            "d1_run": d1_run,
        },
    )
    assert asyncio.run(
        runtime["_status_deploy_semaphore_active"](object(), _Clock.value)
    ) is True
    asyncio.run(
        runtime["_record_status_deploy_sample"](object(), _Clock.value)
    )
    minute_calls = [
        call for call in calls
        if "system_status_minute_batch" in call[0]
    ]
    assert len(minute_calls) == 1
    minute_sql, minute_args = minute_calls[0]
    assert "system_status_minute_batch" in minute_sql
    body = json.loads(minute_args[1])
    assert body["systems"]["mirror:mirror10"] == {
        "ok": True, "reason": None}


def test_deploy_semaphore_has_post_ready_grace_and_fails_open_when_stale():
    row = {}

    async def d1_first(_env, _sql, *_args):
        return dict(row)

    runtime = _load(
        "_status_deploy_semaphore_active",
        extra_globals={"d1_first": d1_first},
    )
    active = runtime["_status_deploy_semaphore_active"]
    grace = runtime["STATUS_DEPLOY_GRACE_MS"]
    maximum = runtime["STATUS_DEPLOY_MAX_MS"]
    now = _Clock.value

    row.update(state="deploying", started_at=now, finished_at=0)
    assert asyncio.run(active(object(), now + maximum - 1)) is True
    assert asyncio.run(active(object(), now + maximum)) is False

    row.update(state="ready", finished_at=now)
    assert asyncio.run(active(object(), now + grace - 1)) is True
    assert asyncio.run(active(object(), now + grace)) is False

    row.update(state="failed", finished_at=now)
    assert asyncio.run(active(object(), now)) is False


def test_visitor_flagship_api_failures_turn_the_repository_row_red():
    # The deep probe only runs every six hours and its last result says
    # "up". People are getting 5xx from the repository's content reads
    # (/api/repo/forkmesh/forkmesh/tree, blob, ...) right now, though — which
    # is exactly "the repository is not accessible" — so the row must not
    # wait for the next probe (adhoc #68: a mirror outage stayed green for
    # thirteen minutes).
    reads = []
    results, reasons, minutes = _run_sample(
        flagship_failures=3,
        reads=reads,
    )
    assert results["flagship_repository"] == 1
    assert reasons["flagship_repository"] == (
        "At least 3 visitor reads of forkmesh/forkmesh repository content "
        "failed with HTTP 5xx in the last minute")
    assert minutes["flagship_repository"] == (
        0, reasons["flagship_repository"])
    # Only the flagship row reads this evidence; nothing else goes red.
    assert all(
        failure == 0 for system, failure in results.items()
        if system != "flagship_repository")
    # Exactly the previous completed minute of the failure-only counter.
    metrics_reads = [
        (sql, args) for sql, args in reads
        if "flagship_read_failure_minute" in sql]
    assert len(metrics_reads) == 1
    sql, args = metrics_reads[0]
    assert "minute_ts=?" in sql
    minute_ts = (_Clock.value // 60_000) * 60_000
    assert args == (minute_ts - 60_000,)
    # No failure row is no evidence of failure. Neither is one stray failed
    # read: a page load during an outage fails several reads at once, so two
    # is the floor.
    results, reasons, _minutes = _run_sample()
    assert results["flagship_repository"] == 0
    results, reasons, _minutes = _run_sample(flagship_failures=1)
    assert results["flagship_repository"] == 0
    results, reasons, _minutes = _run_sample(flagship_failures=2)
    assert results["flagship_repository"] == 1
    assert reasons["flagship_repository"].startswith(
        "At least 2 visitor reads of forkmesh/forkmesh repository content")


def test_probe_failure_and_visitor_failures_are_reported_together():
    results, reasons, _minutes = _run_sample(
        flagship_probe=(False, "Root repository tree failed (HTTP 503)"),
        flagship_failures=12,
    )
    assert results["flagship_repository"] == 1
    assert reasons["flagship_repository"] == (
        "Root repository tree failed (HTTP 503); at least 12 visitor reads "
        "of forkmesh/forkmesh repository content failed with HTTP 5xx in "
        "the last minute")


def test_deploy_grace_never_hides_a_flagship_failure_visitors_are_seeing():
    # A fresh DEPLOYED_AT_MS forgives the probe's own routed reads failing
    # during the Worker/static handoff — but only while nobody is actually
    # failing to load the repository. Two rollouts landing on top of a real
    # mirror outage used to keep /status green for the whole window.
    deployed_at = _Clock.value - 60_000
    results, reasons, _minutes = _run_sample(
        flagship_probe=(False, "Root repository tree failed (HTTP 503)"),
        deployed_at_ms=deployed_at,
    )
    assert results["flagship_repository"] == 0
    assert reasons["flagship_repository"] is None
    results, reasons, _minutes = _run_sample(
        flagship_probe=(False, "Root repository tree failed (HTTP 503)"),
        deployed_at_ms=deployed_at,
        flagship_failures=2,
    )
    assert results["flagship_repository"] == 1
    assert reasons["flagship_repository"].startswith(
        "Root repository tree failed (HTTP 503); at least 2 visitor")


def test_visitor_evidence_read_failure_is_not_evidence_either_way():
    extra, inserted, _hourly, _minutely = _sample_env(_Clock.value, [])
    inner = extra["d1_first"]

    async def d1_first(env, sql, *args):
        if "flagship_read_failure_minute" in sql:
            raise RuntimeError("D1 hiccup")
        return await inner(env, sql, *args)
    extra["d1_first"] = d1_first
    g = _load("record_status_sample", extra_globals=extra)
    asyncio.run(g["record_status_sample"](SimpleNamespace(
        MAILTRAP_API_TOKEN="test-token", MAILTRAP_WEBHOOK_SECRET="s")))
    failures = {row["system"]: row["failure"] for row in inserted}
    assert failures["flagship_repository"] == 0


def _deploy_minute(
        previous_batch=None, flagship_failures=None, roster=None,
        monitor_rows=None, active_mirror_count=0):
    calls = []
    reads = []
    minute_ts = (_Clock.value // 60_000) * 60_000
    runtime = _load(
        "_status_deploy_semaphore_active",
        "_record_status_deploy_sample",
    )
    schema_version = runtime["STATUS_BATCH_SCHEMA_VERSION"]

    async def d1_first(_env, sql, *args):
        reads.append((sql, args))
        if "flagship_read_failure_minute" in sql:
            return {"failures": flagship_failures} if flagship_failures else None
        if "world_deploy_status" in sql:
            return {
                "state": "deploying",
                "started_at": _Clock.value,
                "finished_at": 0,
            }
        if "system_status_minute_batch" in sql:
            assert args == (minute_ts - 60_000,)
            if previous_batch is None:
                return None
            return {"data": json.dumps({
                "schemaVersion": schema_version,
                "systems": previous_batch,
            })}
        if "COUNT(*) AS n FROM mirror_https_endpoints" in sql:
            return {"n": active_mirror_count}
        return {}

    async def d1_all(_env, sql, *args):
        reads.append((sql, args))
        if "monitor_id IN (?,?,?,?,?)" in sql:
            return list(monitor_rows or [])
        if "mirror_https_endpoints" in sql:
            return list(roster if roster is not None else [{
                "node_name": "mirror10", "forkmesh_active": 1,
                "forkmesh_verified_at": _Clock.value,
            }])
        return []

    async def d1_run(_env, sql, *args):
        calls.append((sql, args))

    runtime["d1_first"] = d1_first
    runtime["d1_all"] = d1_all
    runtime["d1_run"] = d1_run
    assert asyncio.run(
        runtime["_status_deploy_semaphore_active"](object(), _Clock.value)
    ) is True
    asyncio.run(
        runtime["_record_status_deploy_sample"](object(), _Clock.value))
    minute_calls = [
        call for call in calls if "system_status_minute_batch" in call[0]]
    assert len(minute_calls) == 1
    return json.loads(minute_calls[0][1][1])["systems"], reads


def test_deploy_minute_carries_failing_state_rows_but_not_event_buckets():
    # "Rollout handoffs are neither incidents nor recoveries": a row that was
    # already down going into the deploy stays down, with its reason, until
    # the first real sample afterwards re-checks it. Event buckets describe
    # one minute's errors, not a state, and are not replayed.
    systems, _reads = _deploy_minute(previous_batch={
        "flagship_repository": {
            "ok": False, "reason": "Root repository tree failed (HTTP 503)"},
        "git_hosting": {
            "ok": False, "reason": "No healthy direct HTTPS mirror"},
        "errors": {"ok": False, "reason": "500 on /api/x: boom"},
        "api": {"ok": False, "reason": "500 on /api/x: boom"},
        "database": {"ok": True, "reason": None},
        "mirror:mirror10": {
            "ok": False, "reason": "mirror10 failed its check"},
    })
    assert systems["flagship_repository"] == {
        "ok": False, "reason": "Root repository tree failed (HTTP 503)"}
    assert systems["git_hosting"] == {
        "ok": False, "reason": "No healthy direct HTTPS mirror"}
    assert systems["mirror:mirror10"] == {
        "ok": False, "reason": "mirror10 failed its check"}
    assert systems["errors"] == {"ok": True, "reason": None}
    assert systems["api"] == {"ok": True, "reason": None}
    assert systems["database"] == {"ok": True, "reason": None}
    assert systems["website"] == {"ok": True, "reason": None}


def test_deploy_minute_still_reads_visitor_flagship_evidence():
    # People failing to load the repository is an outage whatever the deploy
    # clock says; the deploy minute does no probing, but this one cheap read
    # is what makes the page honest while rollouts land on an outage.
    systems, reads = _deploy_minute(
        flagship_failures=4)
    assert systems["flagship_repository"] == {
        "ok": False,
        "reason": (
            "At least 4 visitor reads of forkmesh/forkmesh repository "
            "content failed with HTTP 5xx in the last minute"),
    }
    assert systems["git_hosting"] == {"ok": True, "reason": None}
    assert systems["mirror:mirror10"] == {"ok": True, "reason": None}
    # No previous minute at all: a plain neutral minute, as before.
    systems, _reads = _deploy_minute()
    assert all(
        item == {"ok": True, "reason": None} for item in systems.values())
    assert "mirror:mirror10" in systems


def test_deploy_minute_stops_carrying_fresh_durable_recoveries():
    systems, _reads = _deploy_minute(
        previous_batch={
            "flagship_repository": {
                "ok": False, "reason": "Root repository tree failed"},
            "release_linux": {
                "ok": False, "reason": "No reachable mirror"},
            "release_macos": {
                "ok": False, "reason": "No reachable mirror"},
            "git_hosting": {
                "ok": False, "reason": "No healthy direct HTTPS mirror"},
        },
        monitor_rows=[
            {
                "monitor_id": "flagship-repository-deep-probe",
                "is_up": 1,
                "checked_at": _Clock.value,
            },
            {
                "monitor_id": "release-linux",
                "is_up": 1,
                "checked_at": _Clock.value,
            },
            {
                "monitor_id": "release-macos",
                "is_up": 1,
                "checked_at": _Clock.value,
            },
        ],
        active_mirror_count=1,
    )

    for system_id in (
            "flagship_repository", "release_linux", "release_macos",
            "git_hosting"):
        assert systems[system_id] == {"ok": True, "reason": None}


def test_database_failure_is_isolated_to_the_database_system():
    results, reasons, _minutes = _run_sample(error_paths=[], host_online=True, db_ok=False)
    assert results["database"] == 1
    assert results["website"] == 0
    assert results["api"] == 0
    assert results["errors"] == 0
    assert "db down" in reasons["database"]
    assert reasons["website"] is None


def test_no_healthy_https_mirror_fails_only_git_hosting():
    results, reasons, _minutes = _run_sample(error_paths=[], host_online=False, db_ok=True)
    assert results["git_hosting"] == 1
    assert results["website"] == 0
    assert results["api"] == 0
    assert "no healthy direct https mirror" in reasons["git_hosting"].lower()


def test_email_status_fails_closed_without_sending_or_webhook_config():
    results, reasons, _minutes = _run_sample(email_configured=False)
    assert results["email"] == 1
    assert "sending API is not configured" in reasons["email"]


def test_email_status_tracks_rejection_delivery_and_missing_delivery_event():
    rejected = {
        "accepted": 0, "status": "failed",
        "sent_at": _Clock.value - 1_000, "status_at": _Clock.value - 1_000,
    }
    results, reasons, _minutes = _run_sample(latest_email=rejected)
    assert results["email"] == 1
    assert "rejected" in reasons["email"]

    delivered = {
        "accepted": 1, "status": "delivery",
        "sent_at": _Clock.value - 60_000,
        "status_at": _Clock.value - 30_000,
    }
    results, reasons, _minutes = _run_sample(latest_email=delivered)
    assert results["email"] == 0
    assert reasons["email"] is None

    stale = {
        "accepted": 1, "status": "accepted",
        "sent_at": _Clock.value - 31 * 60_000,
        "status_at": _Clock.value - 31 * 60_000,
    }
    results, reasons, _minutes = _run_sample(latest_email=stale)
    assert results["email"] == 1
    assert "no delivery event after 30 minutes" in reasons["email"]


def _stale_send(send_id="send-1", message_id=""):
    return {
        "send_id": send_id, "message_id": message_id,
        "accepted": 1, "status": "accepted",
        "sent_at": _Clock.value - 31 * 60_000,
        "status_at": _Clock.value - 31 * 60_000,
    }


def test_a_dropped_delivery_webhook_does_not_fake_an_email_outage():
    # A message Mailtrap delivered but whose webhook never landed used to pin
    # its row at "accepted" forever, and this check read that one pinned row as
    # a total email outage. The sending log settles it instead.
    writes = []
    results, reasons, _minutes = _run_sample(
        latest_email=_stale_send(),
        mailtrap_log={"messages": [{
            "message_id": "mt-9", "status": "delivered",
            "custom_variables": {"forkmesh_send_id": "send-1"},
        }]},
        writes=writes,
    )
    assert results["email"] == 0
    assert reasons["email"] is None

    # The settled status is persisted, so the row stops waiting on a webhook
    # that is never going to arrive and the log is not re-queried every minute.
    settled = [w for w in writes if "UPDATE mailtrap_email_sends" in w[0]]
    assert len(settled) == 1
    assert settled[0][1] == ("delivery", _Clock.value, "mt-9", "send-1")


def test_a_logged_delivery_failure_still_reports_the_email_system_down():
    results, reasons, _minutes = _run_sample(
        latest_email=_stale_send(),
        mailtrap_log={"messages": [{
            "message_id": "mt-9", "status": "not_delivered",
            "custom_variables": {"forkmesh_send_id": "send-1"},
        }]},
    )
    assert results["email"] == 1
    assert reasons["email"] == (
        "Mailtrap reported the most recent email as failed")


def test_a_still_enqueued_message_settles_nothing_and_stays_down():
    writes = []
    results, reasons, _minutes = _run_sample(
        latest_email=_stale_send(),
        mailtrap_log={"messages": [{
            "message_id": "mt-9", "status": "enqueued",
            "custom_variables": {"forkmesh_send_id": "send-1"},
        }]},
        writes=writes,
    )
    assert results["email"] == 1
    assert "no delivery event after 30 minutes" in reasons["email"]
    assert not [w for w in writes if "UPDATE mailtrap_email_sends" in w[0]]


def test_a_known_message_id_is_looked_up_directly_rather_than_by_filter():
    calls = []
    results, _reasons, _minutes = _run_sample(
        latest_email=_stale_send(message_id="mt-9"),
        mailtrap_log={"message_id": "mt-9", "status": "delivered"},
        log_calls=calls,
    )
    assert results["email"] == 0
    assert len(calls) == 1
    url, headers = calls[0]
    assert url.endswith("/api/email_logs/mt-9")
    assert headers["api-token"] == "test-token"


def test_a_send_without_a_message_id_is_matched_by_its_custom_variable():
    calls = []
    _results, _reasons, _minutes = _run_sample(
        latest_email=_stale_send(),
        mailtrap_log={"messages": [{
            "message_id": "mt-9", "status": "delivered",
            "custom_variables": {"forkmesh_send_id": "other-send"},
        }]},
        log_calls=calls,
    )
    # A log window that holds only somebody else's message settles nothing.
    assert _results["email"] == 1
    url, _headers = calls[0]
    assert "filters%5Bsent_after%5D=" in url


def test_signed_mirror_endpoints_get_independent_status_samples():
    fresh = _Clock.value - 30_000
    rows = [
        {"node_name": "mirror10", "checked_at": fresh, "healthy": 1,
         "integrity": "ok", "forkmesh_active": 1,
         "forkmesh_verified_at": fresh},
        {"node_name": "mirror13", "checked_at": fresh, "healthy": 0,
         "integrity": "ok", "forkmesh_active": 1,
         "forkmesh_verified_at": fresh},
        # Invalid registry text can never become a public node/system ID.
        {"node_name": "Jett user", "checked_at": fresh, "healthy": 1,
         "integrity": "ok", "forkmesh_active": 1,
         "forkmesh_verified_at": fresh},
    ]
    results, reasons, minutes = _run_sample(mirror_rows=rows)
    assert results["mirror:mirror10"] == 0
    assert minutes["mirror:mirror10"] == (1, None)
    assert results["mirror:mirror13"] == 1
    assert minutes["mirror:mirror13"][0] == 0
    assert "failed its signed HTTPS health check" in reasons["mirror:mirror13"]
    assert all("jett" not in system for system in results)


def test_mirror_status_requires_the_source_release_artifact_count():
    fresh = _Clock.value - 30_000
    rows = [
        {"node_name": "mirror29", "checked_at": fresh, "healthy": 1,
         "integrity": "ok", "forkmesh_active": 1,
         "forkmesh_verified_at": fresh, "artifact_count": 9,
         "source_artifact_count": 9},
        {"node_name": "mirror30", "checked_at": fresh, "healthy": 1,
         "integrity": "ok", "forkmesh_active": 1,
         "forkmesh_verified_at": fresh, "artifact_count": 0,
         "source_artifact_count": 9},
        {"node_name": "mirror31", "checked_at": fresh, "healthy": 1,
         "integrity": "ok", "forkmesh_active": 1,
         "forkmesh_verified_at": fresh, "artifact_count": 12,
         "source_artifact_count": 9},
    ]
    results, reasons, minutes = _run_sample(mirror_rows=rows)

    assert results["mirror:mirror29"] == 0
    assert minutes["mirror:mirror29"] == (1, None)
    assert results["mirror:mirror30"] == 1
    assert reasons["mirror:mirror30"] == (
        "mirror30 serves only 0 of 9 required forkmesh/forkmesh release artifacts")
    assert results["mirror:mirror31"] == 0


def test_only_current_hosts_get_mirror_status_rows():
    fresh = _Clock.value - 30_000
    stale = _Clock.value - 11 * 60_000
    rows = [
        {"node_name": "mirror10", "checked_at": fresh, "healthy": 1,
         "integrity": "ok", "forkmesh_active": 1,
         "forkmesh_verified_at": fresh},
        {"node_name": "mirror12", "checked_at": fresh, "healthy": 1,
         "integrity": "ok", "forkmesh_active": 0,
         "forkmesh_verified_at": fresh},
        {"node_name": "mirror13", "checked_at": stale, "healthy": 1,
         "integrity": "ok", "forkmesh_active": 1,
         "forkmesh_verified_at": stale},
        {"node_name": "mirror17", "checked_at": fresh, "healthy": 1,
         "integrity": "ok", "forkmesh_active": 1,
         "forkmesh_verified_at": fresh},
    ]
    results, _reasons, _minutes = _run_sample(mirror_rows=rows)
    assert results["mirror:mirror10"] == 0
    assert all(
        f"mirror:mirror{n}" not in results for n in (12, 13, 17))


def test_explicitly_retired_mirrors_cannot_reenter_status_roster():
    runtime = _load("_status_mirror_name")
    mirror_name = runtime["_status_mirror_name"]

    assert mirror_name({"node_name": "mirror20"}) == ""
    assert mirror_name({"node_name": "mirror21"}) == ""
    assert mirror_name({"node_name": "mirror23"}) == "mirror23"


def test_stale_signed_mirror_is_removed_with_inactive_hosts():
    stale = _Clock.value - 11 * 60_000
    rows = [
        {"node_name": "mirror10", "checked_at": stale, "healthy": 1,
         "integrity": "ok", "forkmesh_active": 1,
         "forkmesh_verified_at": stale},
    ]
    results, _reasons, _minutes = _run_sample(mirror_rows=rows)
    assert "mirror:mirror10" not in results


def test_api_error_does_not_fail_website():
    results, reasons, _minutes = _run_sample(error_paths=["/api/repositories"])
    assert results["api"] == 1
    assert results["errors"] == 1
    assert results["website"] == 0
    assert results["realtime"] == 0
    assert "/api/repositories" in reasons["api"]
    assert reasons["website"] is None


def test_static_page_error_does_not_fail_api():
    results, reasons, _minutes = _run_sample(error_paths=["/dashboard/index.html"])
    assert results["website"] == 1
    assert results["api"] == 0
    assert results["errors"] == 1
    assert "/dashboard/index.html" in reasons["website"]


def test_git_clone_and_room_errors_are_bucketed_as_realtime():
    results, reasons, _minutes = _run_sample(error_paths=[
        "/someowner/somerepo/info/refs",
        "/api/repo/owner/repo/rooms/main/ws",
    ])
    assert results["realtime"] == 1
    assert results["errors"] == 1
    assert results["website"] == 0
    assert results["api"] == 0
    assert "info/refs" in reasons["realtime"]


def test_do_duration_abort_fails_its_own_bucket_and_realtime():
    # A free-tier DO duration abort is a real room failure (still counts
    # toward "realtime"), but must also have its own dedicated bucket so it's
    # visible as its own row on /status instead of hiding among other
    # realtime incidents.
    results, reasons, _minutes = _run_sample(error_rows=[
        {"path": "/api/repo/mainnode/forkmesh/rooms/general/ws", "status": 503,
         "message": "durable object aborted: Exceeded allowed duration in "
                     "Durable Objects free tier."},
    ])
    assert results["durable_objects"] == 1
    assert results["realtime"] == 1
    assert results["errors"] == 1
    assert results["api"] == 0
    assert "Exceeded allowed duration" in reasons["durable_objects"]


def test_aggregated_do_aborts_fail_only_realtime_and_do_status_rows():
    results, reasons, _minutes = _run_sample(
        error_rows=[],
        do_abort_rows=[{"aborts": 7, "duration_aborts": 5}],
    )
    assert results["durable_objects"] == 1
    assert results["realtime"] == 1
    assert results["errors"] == 0
    assert results["api"] == 0
    assert results["website"] == 0
    assert "7 Durable Object requests aborted" in reasons["durable_objects"]
    assert "5 exceeded allowed duration" in reasons["durable_objects"]


def test_reason_includes_status_and_message_and_extra_count():
    results, reasons, _minutes = _run_sample(error_rows=[
        {"path": "/api/repositories", "status": 500, "message": "boom"},
        {"path": "/api/other", "status": 502, "message": "boom2"},
    ])
    assert results["api"] == 1
    assert results["errors"] == 1
    assert reasons["api"] == "500 on /api/repositories: boom (+1 more)"
    assert reasons["errors"] == "500 on /api/repositories: boom (+1 more)"


def test_offline_direct_mirror_503s_do_not_fail_any_system():
    # 502/503/504 on direct-mirror content paths mean an upstream endpoint is
    # unreachable — node availability (tracked by signed HTTPS health), not an
    # API outage. A single offline node's
    # release blob being re-requested every few minutes used to paint the
    # whole "api" system red on /status.
    blob = "/api/repo/somenode/forkmesh/releases/blob/sha256/" + "a" * 64
    results, reasons, _minutes = _run_sample(error_rows=[
        {"path": blob, "status": 503, "message": "response status 503"},
        {"path": "/api/repo/somenode/forkmesh/tree", "status": 504,
         "message": "mirror timeout"},
        {"path": "/somenode/forkmesh/info/refs", "status": 502,
         "message": "no healthy mirror"},
    ])
    assert results["api"] == 0
    assert results["realtime"] == 0
    assert results["website"] == 0
    assert results["errors"] == 0
    assert reasons["api"] is None


def test_a_500_on_a_direct_mirror_path_still_fails_the_api_bucket():
    # Only upstream-unavailability statuses are excused; a real worker bug
    # (500) on the same path must still count.
    blob = "/api/repo/somenode/forkmesh/releases/blob/sha256/" + "b" * 64
    results, reasons, _minutes = _run_sample(error_rows=[
        {"path": blob, "status": 500, "message": "boom"},
    ])
    assert results["api"] == 1
    assert results["errors"] == 1
    assert "boom" in reasons["api"]


def test_repository_content_paths_are_classified_and_room_paths_are_not():
    g = _load()
    is_tunnel = g["_is_tunnel_content_path"]
    assert is_tunnel(
        "/api/repo/somenode/forkmesh/releases/blob/sha256/" + "a" * 64)
    assert is_tunnel("/api/repo/somenode/forkmesh/tree")
    assert is_tunnel("/somenode/forkmesh/info/refs")
    assert is_tunnel("/somenode/forkmesh/git-upload-pack")
    assert is_tunnel("/somenode/forkmesh/git-receive-pack")
    # A room 5xx is the worker's own Durable Object failing — must stay
    # visible in error_log / Sentry, so room paths are never excused.
    assert not is_tunnel("/api/repo/mainnode/forkmesh/rooms/general/ws")
    assert not is_tunnel("/api/repositories")


def test_fetch_skips_logging_offline_node_5xx_on_repository_content_paths():
    # Source contract: Default.fetch must not log_error (blocking Sentry call
    # + D1 write per hit) for 502/503/504 on direct-mirror content paths — an
    # offline node's re-requested release blob used to generate hundreds of
    # noise rows a day. Everything else >= 500 still logs.
    default_src = ENTRY_TEXT[ENTRY_TEXT.index("class Default"):]
    fetch_src = default_src.split("async def fetch", 1)[1] \
        .split("async def _admin", 1)[0]
    guard_at = fetch_src.index("_is_tunnel_content_path(url.path)")
    log_at = fetch_src.index("await log_error(")
    assert "status in (502, 503, 504)" in fetch_src
    assert guard_at < log_at


def test_room_do_fetch_is_retried_once_on_a_transient_abort():
    # Source contract: a room DO abort (free-tier duration cap, or a
    # co-located DO resetting the isolate) is transient — the router must
    # retry once with a fresh stub before answering 503, and only the
    # persistent failure is logged.
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    src = None
    for node in ast.walk(tree):
        if isinstance(node, ast.AsyncFunctionDef) and node.name == "_route":
            src = ast.unparse(node)
    assert src, "_route not found in entry.py"
    room_block = src[src.index("room_key_from_path(url.path)"):]
    retry_at = room_block.index("for _attempt in range(2)")
    stub_at = room_block.index("FORKMESH_MAINNODE_ROOM.get(room_id)")
    fetch_at = room_block.index("room_object.fetch")
    # The stub is re-acquired inside the retry loop (a crashed isolate needs
    # a fresh stub), and the fetch happens inside the loop too.
    assert retry_at < stub_at < fetch_at
    assert "log_durable_object_abort" in room_block[fetch_at:]


def test_cron_phase_schedule_is_exact_and_staggered():
    due = _load("_cron_due_phases")["_cron_due_phases"]
    slots = range(720)
    scheduled = {slot: due(slot) for slot in slots}
    always = ("status-sample", "status-alert-pings", "mirror-health")
    for phases in scheduled.values():
        assert phases[:len(always)] == always
        assert len(phases) == len(set(phases))

    expected_slots = {
        "status-alert-email": lambda minute: minute % 5 == 0,
        "mirror-health": lambda _minute: True,
        "status-compact": lambda minute: (
            minute % 60 < 5 or minute % 60 == 30),
        "probe-flagship": lambda minute: True,
        "probe-release-platforms": lambda minute: True,
        "snapshot-status": lambda minute: minute % 3 == 0,
        "snapshot-mirrors": lambda minute: minute % 3 == 1,
        "snapshot-catalog": lambda minute: minute % 5 == 2,
        "snapshot-network": lambda minute: minute % 5 == 4,
        "online-sample": lambda minute: minute % 10 == 0,
        "ap-outbox": lambda minute: minute % 5 == 0,
        "reward-chain": lambda minute: minute % 5 == 1,
        "reward-contributions": lambda minute: minute % 5 == 1,
        "reward-sweeps": lambda minute: minute % 5 == 1,
        "federation": lambda minute: minute % 5 == 2,
        "notification-digest": lambda minute: minute % 5 == 3,
        "chat-digest": lambda minute: minute % 5 == 3,
        "channel-unread-email": lambda minute: minute % 5 == 3,
        "ap-digest": lambda minute: minute % 10 == 8,
        "ap-profiles": lambda minute: minute % 20 == 12,
        "world-satellites": lambda minute: minute % 180 == 11,
        "stale-nodes": lambda minute: minute % 15 == 4,
        "blocked-catalog": lambda minute: minute % 15 == 9,
        "central-fund": lambda minute: minute % 15 == 14,
        "instance-health": lambda minute: minute % 15 == 11,
        "microjob-payouts": lambda minute: minute % 60 == 7,
        "referrer-prune": lambda minute: minute % 60 == 17,
        "feedback-email": lambda minute: minute % 360 == 22,
        "marketing-email": lambda minute: minute % 10 == 9,
        "chat-prune": lambda minute: minute % 60 == 37,
        "org-succession": lambda minute: minute % 60 == 42,
        "account-device-prune": lambda minute: minute % 60 == 47,
        "sensitive-cleanup": lambda minute: minute % 60 == 52,
        "expire-rewards": lambda minute: minute % 60 == 52,
        "world-media-cleanup": lambda minute: minute % 60 == 52,
    }
    for phase, predicate in expected_slots.items():
        assert {
            minute for minute, phases in scheduled.items() if phase in phases
        } == {minute for minute in slots if predicate(minute)}, phase


def test_ensure_schema_skips_ddl_when_fingerprint_matches():
    fake_pre_alters = ["ALTER TABLE t ADD COLUMN indexed_y"]
    fake_statements = ["CREATE TABLE IF NOT EXISTS t (x)"]
    fake_alters = ["ALTER TABLE t ADD COLUMN y"]
    prepared = []

    class _Stmt:
        def __init__(self, sql):
            self.sql = sql

        async def run(self):
            prepared.append(self.sql)

    class _DB:
        @staticmethod
        def prepare(sql):
            return _Stmt(sql)

    class _Env:
        DB = _DB

    fingerprint = "fp-current"
    d1_runs = []
    custody_calls = []
    fingerprint_queries = []

    def make_globals(stored_fingerprint, select_error=None,
                     native_fingerprints=(), schema_meta_exists=True):
        async def d1_first(_env, sql, *args):
            assert "schema_meta" in sql
            fingerprint_queries.append((sql, args))
            if select_error is not None:
                raise RuntimeError(select_error)
            if not schema_meta_exists:
                raise RuntimeError("no such table: schema_meta")
            assert args == (fingerprint, "schema_fingerprint:" + fingerprint)
            if stored_fingerprint == fingerprint:
                return {"v": stored_fingerprint}
            if args[1] in native_fingerprints:
                return {"v": "applied"}
            return None

        async def d1_run(_env, sql, *args):
            d1_runs.append((sql, args))

        async def custody_ready(_env):
            custody_calls.append(_env)

        return {
            "asyncio": asyncio,
            "SCHEMA_PRE_CREATE_ALTER_STATEMENTS": fake_pre_alters,
            "SCHEMA_STATEMENTS": fake_statements,
            "SCHEMA_ALTER_STATEMENTS": fake_alters,
            "_SCHEMA_FINGERPRINT": fingerprint,
            "_schema_ready": False,
            "_schema_lock": None,
            "d1_first": d1_first,
            "d1_run": d1_run,
            "_cron_d1_query_charge": lambda count=1: None,
            "_log_d1_query": lambda *_args: None,
            "_assert_legacy_wallet_custody_ready": custody_ready,
        }

    def load_ensure_schema(**kwargs):
        return _load("ensure_schema", "_apply_schema",
                     extra_globals=make_globals(**kwargs))

    # Fingerprint matches: one SELECT, zero DDL statements replayed.
    g = load_ensure_schema(stored_fingerprint=fingerprint)
    asyncio.run(g["ensure_schema"](_Env()))
    assert prepared == []
    assert d1_runs == []
    assert len(custody_calls) == 1
    assert len(fingerprint_queries) == 1

    # A native migration appends this build's compatibility marker instead of
    # replacing the prior Worker's singleton fingerprint. The new Worker still
    # takes the same one-query fast path, and the custody gate still runs.
    prepared.clear()
    d1_runs.clear()
    custody_calls.clear()
    fingerprint_queries.clear()
    g = load_ensure_schema(
        stored_fingerprint="fp-prior-worker",
        native_fingerprints=("schema_fingerprint:" + fingerprint,),
    )
    asyncio.run(g["ensure_schema"](_Env()))
    assert prepared == []
    assert d1_runs == []
    assert len(custody_calls) == 1
    assert len(fingerprint_queries) == 1

    # No schema_meta yet (first run): full DDL replay + fingerprint recorded.
    prepared.clear()
    d1_runs.clear()
    custody_calls.clear()
    fingerprint_queries.clear()
    g = load_ensure_schema(stored_fingerprint=None, schema_meta_exists=False)
    asyncio.run(g["ensure_schema"](_Env()))
    assert prepared == fake_pre_alters + fake_statements + fake_alters
    assert len(d1_runs) == 1 and "schema_meta" in d1_runs[0][0]
    assert d1_runs[0][1] == (fingerprint,)

    # Stale fingerprint (schema changed since): replay + re-record.
    prepared.clear()
    d1_runs.clear()
    custody_calls.clear()
    fingerprint_queries.clear()
    g = load_ensure_schema(stored_fingerprint="fp-older")
    asyncio.run(g["ensure_schema"](_Env()))
    assert prepared == fake_pre_alters + fake_statements + fake_alters
    assert len(d1_runs) == 1

    # Transient D1 failure (overload / internal error) on the fingerprint
    # SELECT must propagate — NOT fall through to the full DDL replay, which
    # would pile ~110 more statements onto an already-overloaded database.
    prepared.clear()
    d1_runs.clear()
    custody_calls.clear()
    fingerprint_queries.clear()
    g = load_ensure_schema(stored_fingerprint=fingerprint,
                           select_error="D1_ERROR: D1 DB is overloaded.")
    try:
        asyncio.run(g["ensure_schema"](_Env()))
        raise AssertionError("expected the transient D1 error to propagate")
    except RuntimeError as exc:
        assert "overloaded" in str(exc)
    assert prepared == []
    assert d1_runs == []

    # The first no-table apply completes synchronously against this D1 stub and
    # marks the isolate ready, so later concurrent callers do not replay DDL.
    prepared.clear()
    d1_runs.clear()
    custody_calls.clear()
    fingerprint_queries.clear()
    g = load_ensure_schema(stored_fingerprint=None, schema_meta_exists=False)

    async def _concurrent():
        await asyncio.gather(*(g["ensure_schema"](_Env()) for _ in range(5)))

    asyncio.run(_concurrent())
    assert prepared == fake_pre_alters + fake_statements + fake_alters
    assert len(d1_runs) == 1


# --- status_history ----------------------------------------------------------

def _history_env(rows, hour_rows=(), minute_rows=(), mirror_rows=(),
                 core_rows=(), runner_rows=()):
    async def noop(*_a, **_k):
        return None

    async def d1_all(_env, sql, *_args):
        if "system_status_minute_batch" in sql:
            return []
        if "system_status_hourly_batch" in sql:
            return []
        if "system_status_daily_batch" in sql:
            return []
        if "system_status_minute" in sql:
            return list(minute_rows)
        if "system_status_hourly" in sql:
            return list(hour_rows)
        if "system_status_daily" in sql:
            return list(rows)
        if "mirror_https_endpoints" in sql:
            return list(mirror_rows)
        if "FROM cluster_members" in sql:
            return [
                row for row in core_rows
                if int(row.get("heartbeat_ms") or 0) >= _args[0]]
        if "FROM runner_nodes" in sql:
            return list(runner_rows)
        return rows

    captured = {}

    def json_response(payload, cache_seconds=None):
        captured.update(payload)
        return payload

    extra = {
        "Date": _Clock,
        "ensure_schema": noop,
        "d1_all": d1_all,
        "json_response": json_response,
    }
    return extra, captured


def _run_history(rows, hour_rows=(), minute_rows=(), mirror_rows=(),
                 core_rows=(), runner_rows=()):
    extra, captured = _history_env(
        rows, hour_rows, minute_rows, mirror_rows, core_rows, runner_rows)
    g = _load("status_history", extra_globals=extra)
    asyncio.run(g["status_history"](object()))
    return captured


def test_no_data_is_neutral_monitoring_gap_without_fabricated_uptime():
    # Missing telemetry is not evidence that every service failed. Keep the
    # component unknown and report the one shared monitoring problem globally;
    # uptime stays unset because no service probe actually ran.
    out = _run_history([])
    by_id = {s["id"]: s for s in out["systems"]}
    assert by_id["website"]["status"] == "unknown"
    assert by_id["website"]["monitoringState"] is None
    assert out["current"]["monitoringState"] == "empty"
    assert out["current"]["lastCronSampleTs"] is None
    assert by_id["website"]["uptimePct"] is None
    assert by_id["website"]["uptime24hPct"] is None
    assert len(by_id["website"]["days"]) == 30


def test_world_status_projection_keeps_visual_windows_without_nested_history():
    cur_day = (_Clock.value // DAY_MS) * DAY_MS
    cur_hour = (_Clock.value // HOUR_MS) * HOUR_MS
    hour_rows = [
        {
            "hour_ts": cur_hour,
            "system": "website",
            "checks": 60,
            "failures": 1,
            "reason": "one failed check",
        },
    ]
    minute_rows = [
        {
            "minute_ts": (_Clock.value // 60000) * 60000,
            "system": "website",
            "ok": 1,
            "reason": None,
        },
    ]
    extra, captured = _history_env(
        [{"day_ts": cur_day, "system": "website", "checks": 60, "failures": 1}],
        hour_rows,
        minute_rows,
    )
    g = _load("status_history", extra_globals=extra)
    asyncio.run(g["status_history"](object(), "world"))

    website = next(
        system for system in captured["systems"] if system["id"] == "website")
    assert len(website["days"]) == 30
    assert all("hours" not in day for day in website["days"])
    # The World client derives the day colour from these canonical facts.
    assert set(website["days"][-1]) == {"dayTs", "checks", "failures"}
    assert website["days"][-1]["checks"] == 1
    assert website["days"][-1]["failures"] == 0
    assert len(website["hours"]) == 24
    assert len(website["minutes"]) == 60
    assert all(set(hour) == {"hourTs", "status"} for hour in website["hours"])
    assert all(
        set(minute) == {"minuteTs", "status"}
        for minute in website["minutes"]
    )
    assert "checkDescription" not in website


def test_status_history_cache_separates_full_and_world_projections():
    matched = []
    stored = []
    built = []
    full = {
        "ok": True,
        "now": _Clock.value,
        "systems": [{
            "id": "website", "label": "Website", "status": "operational",
            "checkDescription": "full only",
            "days": [{
                "dayTs": _Clock.value, "checks": 1, "failures": 0,
                "hours": [{"hourTs": _Clock.value, "status": "operational",
                            "reason": "full only"}],
            }],
            "minutes": [{"minuteTs": _Clock.value,
                         "status": "operational", "reason": "full only"}],
        }],
        "current": {"lastCronSampleTs": _Clock.value},
    }

    async def edge_cache_match(key):
        matched.append(key)
        return full if key.endswith("/full-v1") else None

    async def edge_cache_put(key, response):
        stored.append((key, response))

    async def status_history(_env, view="full"):
        built.append(view)
        return {"fresh": view}

    def json_response(payload, cache_seconds=None):
        return payload

    runtime = _load(
        "cached_status_history",
        extra_globals={
            "edge_cache_match": edge_cache_match,
            "edge_cache_put": edge_cache_put,
            "status_history": status_history,
            "json_response": json_response,
        },
    )
    cached = runtime["cached_status_history"]

    world = asyncio.run(cached(object(), "world"))
    assert world["ok"] is True
    assert world["systems"][0]["days"] == [{
        "dayTs": _Clock.value, "checks": 1, "failures": 0}]
    assert world["systems"][0]["hours"] == [{
        "hourTs": _Clock.value, "status": "operational"}]
    assert world["systems"][0]["minutes"] == [{
        "minuteTs": _Clock.value, "status": "operational"}]
    assert "checkDescription" not in world["systems"][0]
    assert built == []
    assert matched == [
        "https://forkmesh.internal/api/status/world-v1",
        "https://forkmesh.internal/api/status/full-v1",
    ]
    assert stored == [(
        "https://forkmesh.internal/api/status/world-v1", world)]


def test_public_status_response_cannot_be_held_in_the_cdn_cache():
    class _Headers(dict):
        def items(self):
            return super().items()

    class _Response:
        def __init__(
            self, body, status=200, status_text="", headers=None,
        ):
            self.body = body
            self.status = status
            self.status_text = status_text
            self.headers = _Headers(headers or {})

    runtime = _load(
        "uncached_status_response", extra_globals={"Response": _Response})
    response = runtime["uncached_status_response"](_Response(
        "{}", headers={
            "content-type": "application/json",
            "cache-control": "public, max-age=14400",
        }))
    assert response.headers["cache-control"] == (
        "no-store, max-age=0, must-revalidate")
    assert response.headers["cdn-cache-control"] == "no-store"
    assert response.headers["cloudflare-cdn-cache-control"] == "no-store"
    assert response.headers["content-type"] == "application/json"


def test_explicit_status_refresh_bypasses_and_replaces_the_edge_cache():
    matched = []
    stored = []
    deleted = []
    built = []

    async def edge_cache_match(key):
        matched.append(key)
        return {"stale": True}

    async def edge_cache_put(key, response):
        stored.append((key, response))

    async def edge_cache_delete(key):
        deleted.append(key)

    async def status_history(_env, view="full"):
        built.append(view)
        return {"ok": True, "now": _Clock.value}

    runtime = _load(
        "cached_status_history",
        extra_globals={
            "edge_cache_match": edge_cache_match,
            "edge_cache_put": edge_cache_put,
            "edge_cache_delete": edge_cache_delete,
            "status_history": status_history,
        },
    )
    fresh = asyncio.run(runtime["cached_status_history"](
        object(), "full", force_refresh=True))

    assert fresh == {"ok": True, "now": _Clock.value}
    assert matched == []
    assert built == ["full"]
    assert stored == [(
        "https://forkmesh.internal/api/status/full-v1", fresh)]
    assert deleted == ["https://forkmesh.internal/api/status/world-v1"]


def test_status_snapshot_tick_owns_the_only_history_build_and_warms_full_cache():
    probe = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _status_page_api_probe"):
        ENTRY_TEXT.index("async def cached_status_history")
    ]
    handler = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _public_snapshot_handler_response"):
        ENTRY_TEXT.index("async def _build_public_snapshot_payload")
    ]
    publisher = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _publish_public_snapshot_group"):
        ENTRY_TEXT.index("async def refresh_public_api_snapshots")
    ]
    assert "status_history(" not in probe
    assert "cached_status_history(" not in probe
    assert '_public_snapshot_row(env, "/api/status")' in probe
    assert 'edge_cache_delete(STATUS_EDGE_CACHE_KEYS["full"])' in handler
    assert 'return await cached_status_history(env, "full")' in handler
    assert 'payloads.get("/api/status")' in publisher
    assert "project_world_status_payload(full)" in publisher


def _run_status_asset_probe(overrides=None):
    class _Response:
        def __init__(self, status, body):
            self.status = status
            self._body = body
            self.headers = {"content-length": str(len(body.encode("utf-8")))}

        async def text(self):
            return self._body

    artifacts = {
        "/status.html": (
            200,
            '<!doctype html><h1 id="status-title">Status</h1>'
            '<script>new WebSocket("/api/status/ws")</script>',
        ),
        "/styles.css": (
            200,
            ".status-banner { display:flex } "
            ".status-history-row { display:flex }",
        ),
    }
    artifacts.update(overrides or {})

    class _Assets:
        async def fetch(self, url):
            path = urlparse(str(url)).path
            status, body = artifacts.get(path, (404, ""))
            return _Response(status, body)

    deadlines = []

    class _AbortSignal:
        @staticmethod
        def timeout(value):
            deadlines.append(value)
            return value

    runtime = _load(
        "_status_page_static_assets_probe",
        extra_globals={
            "JsAbortSignal": _AbortSignal,
            "JsRequest": SimpleNamespace(new=lambda url, *_args: url),
            "_cron_subrequest_charge": lambda count=1: None,
            "_safe_error_text": lambda error: str(error),
            "to_js": lambda value: value,
        },
    )
    result = asyncio.run(runtime["_status_page_static_assets_probe"](
        SimpleNamespace(ASSETS=_Assets())))
    return result, deadlines


def test_status_probe_loads_and_recognizes_its_deployed_artifacts():
    result, deadlines = _run_status_asset_probe()
    assert result == (True, "")
    assert deadlines == [5_000, 5_000]

    (ok, reason), _deadlines = _run_status_asset_probe({
        "/styles.css": (404, "")})
    assert ok is False
    assert reason == "Status page artifact /styles.css returned HTTP 404"

    (ok, reason), _deadlines = _run_status_asset_probe({
        "/status.html": (200, "<!doctype html><p>wrong page</p>"),
    })
    assert ok is False
    assert reason == "Status page artifact /status.html failed its content check"


def test_all_checks_passing_today_is_operational():
    cur_day = (_Clock.value // DAY_MS) * DAY_MS
    cur_hour = (_Clock.value // HOUR_MS) * HOUR_MS
    rows = [{"day_ts": cur_day, "system": "website", "checks": 60, "failures": 0}]
    hour_rows = [
        {"hour_ts": cur_hour, "system": "website", "checks": 60,
         "failures": 0, "reason": None},
    ]
    out = _run_history(rows, hour_rows)
    by_id = {s["id"]: s for s in out["systems"]}
    assert by_id["website"]["status"] == "operational"
    # The current hour is green, but the rest of the rolling window has no
    # samples and is visibly a monitoring gap. That gap must prevent the
    # history percentage from claiming 100%.
    assert 0 < by_id["website"]["uptime24hPct"] < 100.0
    assert 0 < by_id["website"]["uptimePct"] < 100.0
    assert by_id["website"]["coverage24hPct"] < 100.0
    assert by_id["website"]["missing24h"] > 0


def test_all_checks_failing_today_is_down():
    cur_day = (_Clock.value // DAY_MS) * DAY_MS
    cur_hour = (_Clock.value // HOUR_MS) * HOUR_MS
    rows = [{"day_ts": cur_day, "system": "api", "checks": 10, "failures": 10}]
    hour_rows = [
        {"hour_ts": cur_hour, "system": "api", "checks": 60,
         "failures": 60, "reason": "all checks failed"},
    ]
    out = _run_history(rows, hour_rows)
    by_id = {s["id"]: s for s in out["systems"]}
    assert by_id["api"]["status"] == "down"
    assert by_id["api"]["uptimePct"] == 0.0


def test_some_checks_failing_today_is_degraded():
    cur_day = (_Clock.value // DAY_MS) * DAY_MS
    cur_hour = (_Clock.value // HOUR_MS) * HOUR_MS
    rows = [{"day_ts": cur_day, "system": "database", "checks": 10, "failures": 3}]
    hour_rows = [
        {"hour_ts": cur_hour, "system": "database", "checks": 60,
         "failures": 18, "reason": "db timeouts"},
    ]
    out = _run_history(rows, hour_rows)
    by_id = {s["id"]: s for s in out["systems"]}
    assert by_id["database"]["status"] == "degraded"
    assert by_id["database"]["uptime24hPct"] < 100.0


def test_current_status_uses_latest_day_not_a_stale_incident_weeks_ago():
    cur_day = (_Clock.value // DAY_MS) * DAY_MS
    old_day = cur_day - 20 * DAY_MS
    rows = [
        {"day_ts": old_day, "system": "website", "checks": 60, "failures": 60},
        {"day_ts": cur_day, "system": "website", "checks": 60, "failures": 0},
    ]
    cur_hour = (_Clock.value // HOUR_MS) * HOUR_MS
    hour_rows = [
        {"hour_ts": old_day, "system": "website", "checks": 60,
         "failures": 60, "reason": "old outage"},
        {"hour_ts": cur_hour, "system": "website", "checks": 60,
         "failures": 0, "reason": None},
    ]
    out = _run_history(rows, hour_rows)
    by_id = {s["id"]: s for s in out["systems"]}
    # Status reflects today (operational), even though the 30-day aggregate
    # uptime is dragged down by the old incident.
    assert by_id["website"]["status"] == "operational"
    assert by_id["website"]["uptimePct"] < 100.0


def test_current_status_uses_latest_hour_not_the_whole_days_aggregate():
    # A blip two hours ago that has since cleared shouldn't keep today's badge
    # degraded for the rest of the day — the banner should track the most
    # recent hour, not the day's cumulative failure count.
    cur_day = (_Clock.value // DAY_MS) * DAY_MS
    cur_hour = (_Clock.value // HOUR_MS) * HOUR_MS
    rows = [{"day_ts": cur_day, "system": "api", "checks": 3, "failures": 1}]
    hour_rows = [
        {"hour_ts": cur_hour - 2 * HOUR_MS, "system": "api", "checks": 60,
         "failures": 60, "reason": "502 on /api/x: boom"},
        {"hour_ts": cur_hour - 1 * HOUR_MS, "system": "api", "checks": 60,
         "failures": 0, "reason": None},
        {"hour_ts": cur_hour, "system": "api", "checks": 60,
         "failures": 0, "reason": None},
    ]
    out = _run_history(rows, hour_rows)
    by_id = {s["id"]: s for s in out["systems"]}
    assert by_id["api"]["status"] == "operational"
    assert by_id["api"]["reason"] is None


HOUR_MS = 3600000


def test_recovered_git_hosting_earlier_today_clears_the_badge():
    # Hosts dropped off overnight (a failing hour) but are back now (the
    # current hour is operational): the headline badge must read operational
    # and drop the stale "no desktop hosts checked in" reason, even though the
    # whole-day aggregate still counts the earlier failures. Regression for
    # "we have hosts online but /status shows git hosting down (1h 34m ago)".
    cur_day = (_Clock.value // DAY_MS) * DAY_MS
    cur_hour = (_Clock.value // HOUR_MS) * HOUR_MS
    prev_hour = cur_hour - HOUR_MS
    rows = [{"day_ts": cur_day, "system": "git_hosting",
             "checks": 120, "failures": 60}]
    hour_rows = [
        {"hour_ts": prev_hour, "system": "git_hosting", "checks": 60,
         "failures": 60,
         "reason": "No desktop hosts have checked in within the last 10 minutes"},
        {"hour_ts": cur_hour, "system": "git_hosting", "checks": 60,
         "failures": 0, "reason": None},
    ]
    out = _run_history(rows, hour_rows)
    by_id = {s["id"]: s for s in out["systems"]}
    assert by_id["git_hosting"]["status"] == "operational"
    assert by_id["git_hosting"]["reason"] is None
    assert by_id["git_hosting"]["reasonTs"] is None


def test_hours_breakdown_present_for_today_with_reason_on_degraded_hour():
    cur_day = (_Clock.value // DAY_MS) * DAY_MS
    cur_hour = (_Clock.value // HOUR_MS) * HOUR_MS
    rows = [{"day_ts": cur_day, "system": "api", "checks": 60, "failures": 30}]
    hour_rows = [
        {"hour_ts": cur_hour, "system": "api", "checks": 60, "failures": 30,
         "reason": "500 on /api/x: boom"},
    ]
    out = _run_history(rows, hour_rows)
    by_id = {s["id"]: s for s in out["systems"]}
    today = next(d for d in by_id["api"]["days"] if d["dayTs"] == cur_day)
    expected_hour_count = (cur_hour - cur_day) // HOUR_MS + 1
    assert len(today["hours"]) == expected_hour_count
    this_hour = today["hours"][-1]
    assert this_hour["status"] == "degraded"
    assert this_hour["reason"] == "500 on /api/x: boom"


def test_hour_with_no_checks_is_neutral_and_explains_monitoring_gap():
    cur_day = (_Clock.value // DAY_MS) * DAY_MS
    out = _run_history([], hour_rows=())
    by_id = {s["id"]: s for s in out["systems"]}
    today = next(d for d in by_id["website"]["days"] if d["dayTs"] == cur_day)
    elapsed = [h for h in today["hours"] if h["expectedChecks"]]
    assert elapsed
    assert all(h["status"] == "unknown" for h in elapsed)
    assert all(h["monitoringState"] == "empty" for h in elapsed)
    assert all("Monitoring failed" in h["reason"] for h in elapsed)
    assert all(h["missingChecks"] == h["expectedChecks"] for h in elapsed)


def test_partially_missing_hour_retains_recorded_passing_state():
    cur_hour = (_Clock.value // HOUR_MS) * HOUR_MS
    hour = _load("_status_effective_hour")["_status_effective_hour"](
        cur_hour, _Clock.value, (1, 0, None))
    assert hour["status"] == "operational"
    assert hour["missingChecks"] > 0
    assert hour["monitoringState"] == "missing"


def test_uptime_includes_monitoring_gaps_and_reports_coverage():
    # 10 samples recorded this hour (5 failed) out of a near-full day of
    # expected minutes: the displayed percentage includes the missing window,
    # while the recorded and failed counts remain available separately.
    cur_hour = (_Clock.value // HOUR_MS) * HOUR_MS
    hour_rows = [
        {"hour_ts": cur_hour, "system": "api", "checks": 10, "failures": 5,
         "reason": "500 on /api/x: boom"},
    ]
    out = _run_history([], hour_rows)
    by_id = {s["id"]: s for s in out["systems"]}
    api = by_id["api"]
    assert api["status"] == "degraded"
    assert api["uptime24hPct"] < 1.0
    assert api["uptimePct"] < 1.0
    assert api["checks24h"] == 10
    assert api["failures24h"] == 5
    assert api["missing24h"] > 0
    assert api["coverage24hPct"] < 100.0


def test_stale_hour_retains_last_observed_state_with_one_monitoring_warning():
    cur_hour = (_Clock.value // HOUR_MS) * HOUR_MS
    stale_hour = cur_hour - 5 * HOUR_MS
    hour_rows = [
        {"hour_ts": stale_hour, "system": "website", "checks": 60,
         "failures": 0, "reason": None},
    ]
    out = _run_history([], hour_rows)
    by_id = {s["id"]: s for s in out["systems"]}
    website = by_id["website"]
    assert website["status"] == "operational"
    assert website["monitoringState"] is None
    assert website["reason"] is None
    assert website["observedAt"] == stale_hour
    assert out["current"]["monitoringState"] == "empty"
    # The stale-but-recorded samples do not hide the missing buckets in the
    # displayed history percentage.
    assert website["uptime24hPct"] < 100.0


def test_each_system_describes_exactly_what_its_check_tests():
    # Every /status row is click-expandable to show what its health check
    # actually verifies; the descriptions must exist, be distinct per system
    # (the rows share one sampler cron, so identical-looking bars need the
    # difference spelled out), and match the real checks in
    # record_status_sample.
    out = _run_history([])
    by_id = {s["id"]: s for s in out["systems"]}
    descriptions = {sid: s["checkDescription"] for sid, s in by_id.items()}
    assert all(d and len(d) > 40 for d in descriptions.values())
    assert len(set(descriptions.values())) == len(descriptions)
    assert "D1" in descriptions["database"]
    assert "10 minutes" in descriptions["git_hosting"]
    assert "/api/" in descriptions["api"]
    assert "error log" in descriptions["errors"]
    assert "Expected degraded" in descriptions["errors"]
    assert "Exceeded allowed duration" in descriptions["durable_objects"]
    assert "Loads the production homepage" in descriptions["website"]
    assert "HTTP 200" in descriptions["website"]
    assert "/health/mobile" in descriptions["world_mobile"]
    assert "same-origin Three.js" in descriptions["world_mobile"]


def test_status_page_wires_the_click_to_expand_check_details():
    status_html = (ROOT / "public" / "status.html").read_text(encoding="utf-8")
    assert "checkDescription" in status_html
    assert "status-row-detail-panel" in status_html
    assert "What this check tests" in status_html
    assert 'head.setAttribute("aria-expanded"' in status_html
    assert 'addMetric("24h coverage"' not in status_html


def test_operational_hour_does_not_carry_a_stale_reason():
    cur_day = (_Clock.value // DAY_MS) * DAY_MS
    cur_hour = (_Clock.value // HOUR_MS) * HOUR_MS
    hour_rows = [
        {"hour_ts": cur_hour, "system": "website", "checks": 60, "failures": 0,
         "reason": "stale reason from an earlier failure this hour"},
    ]
    out = _run_history([], hour_rows)
    by_id = {s["id"]: s for s in out["systems"]}
    today = next(d for d in by_id["website"]["days"] if d["dayTs"] == cur_day)
    this_hour = today["hours"][-1]
    assert this_hour["status"] == "operational"
    assert this_hour["reason"] is None


# --- per-minute strip (60-minute row under the hour strip) -----------------

MINUTE_MS = 60000


def test_record_status_sample_writes_one_minute_row_per_system():
    _results, _reasons, minutes = _run_sample(error_paths=["/api/repositories"])
    # database/git_hosting/website/realtime all pass this tick; only "api"
    # (matched by the /api/ path) fails. minutely's "ok" is the schema's own
    # ok column (1 == passed), the inverse of the daily/hourly "failure" flag.
    ok, reason = minutes["api"]
    assert ok == 0
    assert "/api/repositories" in reason
    ok_db, no_reason = minutes["database"]
    assert ok_db == 1
    assert no_reason is None


def test_client_self_reports_do_not_red_worker_health_systems():
    error_rows = [
        {
            "method": "APP", "path": "/desktop-error/headless",
            "status": 521, "message": "Desktop Toast [headless] reconnecting",
        },
        {
            "method": "JS", "path": "/client-error/world",
            "status": 520, "message": "Browser Error [world] websocket closed",
        },
        # Preserve the path fallback used by the admin source classifier so
        # older rows and partially migrated writers remain harmless too.
        {
            "method": "POST", "path": "/desktop-error/app",
            "status": 521, "message": "Desktop Error [app] update failed",
        },
        {
            "method": "POST", "path": "/client-error/dashboard",
            "status": 520, "message": "Browser Error [dashboard] fetch failed",
        },
    ]

    results, reasons, minutes = _run_sample(error_rows=error_rows)

    for system in ("website", "api", "errors", "realtime", "durable_objects"):
        assert results[system] == 0
        assert reasons[system] is None
        assert minutes[system] == (1, None)


def test_worker_errors_still_fail_worker_health_after_client_filtering():
    error_rows = [
        {
            "method": "APP", "path": "/desktop-error/app",
            "status": 521, "message": "client-only failure",
        },
        {
            "method": "GET", "path": "/api/repositories",
            "status": 500, "message": "worker failure",
        },
    ]

    results, reasons, _minutes = _run_sample(error_rows=error_rows)

    assert results["api"] == 1
    assert results["errors"] == 1
    assert results["website"] == 0
    assert "/api/repositories" in reasons["api"]
    assert "/desktop-error/" not in reasons["errors"]


def test_one_sampler_records_each_minute_via_the_claim_row():
    # Replayed alarm delivery can call record_status_sample more than once for
    # a minute. A completed batch is the receipt that suppresses later work.
    extra, inserted, hourly, minutely = _sample_env(_Clock.value, [])
    claims = {}
    base_d1_run = extra["d1_run"]
    base_d1_first = extra["d1_first"]

    async def d1_run(env, sql, *args):
        if sql.startswith("INSERT INTO system_status_sample_claim"):
            claims.setdefault(args[0], args[1])
            return
        await base_d1_run(env, sql, *args)

    async def d1_first(env, sql, *args):
        if "system_status_sample_claim" in sql:
            return {"claim": claims.get(args[0])}
        return await base_d1_first(env, sql, *args)

    counter = [0]

    class _Uint8Array:
        @staticmethod
        def new(length):
            return length

    class _Crypto:
        @staticmethod
        def getRandomValues(length):
            counter[0] += 1
            return [(counter[0] + i) % 256 for i in range(length)]

    extra.update({
        "d1_run": d1_run, "d1_first": d1_first,
        "js_crypto": _Crypto, "Uint8Array": _Uint8Array,
    })
    g = _load("record_status_sample", extra_globals=extra)
    asyncio.run(g["record_status_sample"](object()))
    recorded = len(minutely)
    assert recorded > 0

    # A second caller inside the same minute loses the read-back token check
    # and must skip the whole sample (no minute rows, no counter increments).
    asyncio.run(g["record_status_sample"](object()))
    assert len(minutely) == recorded
    assert len(inserted) == recorded
    assert len(hourly) == recorded


def test_abandoned_status_claim_can_be_taken_over_until_batch_completes():
    claims = {}
    completed = set()

    async def d1_run(_env, sql, *args):
        assert sql.startswith("INSERT INTO system_status_sample_claim")
        assert "system_status_minute_batch" in sql
        minute_ts, claim, _claimed_at, completion_ts = args
        assert completion_ts == minute_ts
        if minute_ts not in completed:
            claims[minute_ts] = claim

    async def d1_first(_env, sql, minute_ts):
        assert "system_status_sample_claim" in sql
        return {"claim": claims.get(minute_ts)}

    counter = [0]

    class _Uint8Array:
        @staticmethod
        def new(length):
            return length

    class _Crypto:
        @staticmethod
        def getRandomValues(length):
            counter[0] += 1
            return [(counter[0] + index) % 256 for index in range(length)]

    runtime = _load("_claim_status_sample_minute", extra_globals={
        "Date": _Clock,
        "d1_run": d1_run,
        "d1_first": d1_first,
        "js_crypto": _Crypto,
        "Uint8Array": _Uint8Array,
    })
    claim_minute = (_Clock.value // MINUTE_MS) * MINUTE_MS
    claim = runtime["_claim_status_sample_minute"]
    assert asyncio.run(claim(object(), _Clock.value, "trigger")) is True
    first = claims[claim_minute]
    assert asyncio.run(claim(object(), _Clock.value, "runner")) is True
    assert claims[claim_minute] != first

    completed.add(claim_minute)
    winner = claims[claim_minute]
    assert asyncio.run(claim(object(), _Clock.value, "trigger")) is False
    assert claims[claim_minute] == winner


def test_claim_infrastructure_failure_fails_open_and_still_samples():
    # A broken claim table must never blank the public sample — worst case a
    # duplicated minute overwrites cleanly and costs one extra check count,
    # while a skipped minute would paint fake downtime.
    extra, _inserted, _hourly, minutely = _sample_env(_Clock.value, [])
    base_d1_run = extra["d1_run"]

    async def d1_run(env, sql, *args):
        if sql.startswith("INSERT INTO system_status_sample_claim"):
            raise RuntimeError("no such table: system_status_sample_claim")
        await base_d1_run(env, sql, *args)

    extra["d1_run"] = d1_run
    g = _load("record_status_sample", extra_globals=extra)
    asyncio.run(g["record_status_sample"](object()))
    assert len(minutely) > 0


def test_minute_strip_is_sixty_buckets_oldest_to_newest():
    out = _run_history([])
    by_id = {s["id"]: s for s in out["systems"]}
    minutes = by_id["website"]["minutes"]
    assert len(minutes) == 60
    for i in range(59):
        assert minutes[i]["minuteTs"] < minutes[i + 1]["minuteTs"]
    cur_minute = (_Clock.value // MINUTE_MS) * MINUTE_MS
    assert minutes[-1]["minuteTs"] == cur_minute


def test_minute_with_no_row_is_future_only_for_the_current_bucket():
    out = _run_history([])
    by_id = {s["id"]: s for s in out["systems"]}
    minutes = by_id["website"]["minutes"]
    # The newest bucket (this minute) simply hasn't been sampled by the cron
    # yet — that's not evidence of an outage.
    assert minutes[-1]["status"] == "future"
    # Every older bucket was expected to have data. It stays a neutral gap,
    # visually distinct from an observed service failure.
    assert all(m["status"] == "unknown" for m in minutes[:-1])
    assert all(m["monitoringState"] == "missing" for m in minutes[:-1])
    assert all("Monitoring failed" in m["reason"] for m in minutes[:-1])


def test_minute_row_reflects_ok_and_carries_its_failure_reason():
    cur_minute = (_Clock.value // MINUTE_MS) * MINUTE_MS
    prev_minute = cur_minute - MINUTE_MS
    minute_rows = [
        {"minute_ts": prev_minute, "system": "api", "ok": 0,
         "reason": "500 on /api/x: boom"},
        {"minute_ts": cur_minute, "system": "api", "ok": 1, "reason": None},
    ]
    out = _run_history([], minute_rows=minute_rows)
    by_id = {s["id"]: s for s in out["systems"]}
    minutes = {m["minuteTs"]: m for m in by_id["api"]["minutes"]}
    assert minutes[prev_minute]["status"] == "down"
    assert minutes[prev_minute]["reason"] == "500 on /api/x: boom"
    # The current minute already has a row (the cron beat us to it this
    # time), so it reflects that sample rather than reading as "future".
    assert minutes[cur_minute]["status"] == "operational"
    assert minutes[cur_minute]["reason"] is None


def test_recorded_signed_mirror_appears_as_a_full_status_system():
    cur_minute = (_Clock.value // MINUTE_MS) * MINUTE_MS
    minute_rows = [
        {"minute_ts": cur_minute, "system": "mirror:mirror10",
         "ok": 1, "reason": None},
    ]
    out = _run_history(
        [], minute_rows=minute_rows,
        mirror_rows=[{
            "node_name": "mirror10", "forkmesh_active": 1,
            "forkmesh_verified_at": _Clock.value,
        }],
    )
    system = next(s for s in out["systems"] if s["id"] == "mirror:mirror10")
    assert system["label"] == "Mirror node — mirror10"
    assert system["status"] == "operational"
    assert len(system["days"]) == 30
    assert len(system["minutes"]) == 60
    assert "account-bound direct HTTPS endpoint" in system["checkDescription"]
    assert all(s["id"] != "mirror:jett" for s in out["systems"])


def test_mirror_without_recent_online_signal_is_omitted_from_status():
    stale_mirror_last_seen = 25 * 60 * 60 * 1000
    stale_hour = (_Clock.value // HOUR_MS) * HOUR_MS - stale_mirror_last_seen
    hour_rows = [
        {
            "hour_ts": stale_hour,
            "system": "mirror:mirror10",
            "checks": 60,
            "failures": 60,
            "reason": "node down",
        },
    ]
    mirror_rows = [{"node_name": "mirror10", "checked_at": stale_hour}]
    out = _run_history([], hour_rows=hour_rows, mirror_rows=mirror_rows)
    assert all(s["id"] != "mirror:mirror10" for s in out["systems"])


def test_mirror_without_recent_online_signal_is_omitted_from_world_status():
    stale = _Clock.value - 25 * HOUR_MS
    minute_rows = [
        {"minute_ts": (_Clock.value // MINUTE_MS) * MINUTE_MS,
         "system": "mirror:mirror10", "ok": 0, "reason": "old node"},
    ]
    extra, captured = _history_env(
        [], minute_rows=minute_rows,
        mirror_rows=[{"node_name": "mirror10", "seen_at": stale}],
    )
    runtime = _load("status_history", extra_globals=extra)
    asyncio.run(runtime["status_history"](object(), "world"))
    assert all(s["id"] != "mirror:mirror10" for s in captured["systems"])


def test_retired_mirror_history_does_not_resurrect_status_rows():
    cur_minute = (_Clock.value // MINUTE_MS) * MINUTE_MS
    minute_rows = [
        {"minute_ts": cur_minute, "system": "mirror:" + mirror,
         "ok": 0, "reason": "old retired-node failure"}
        for mirror in ("mirror2", "mirror14")
    ]
    out = _run_history([], minute_rows=minute_rows)
    assert all(s["id"] != "mirror:mirror2" for s in out["systems"])
    assert all(s["id"] != "mirror:mirror14" for s in out["systems"])


def test_latest_passing_minute_clears_failure_from_hourly_rollup():
    cur_day = (_Clock.value // DAY_MS) * DAY_MS
    cur_hour = (_Clock.value // HOUR_MS) * HOUR_MS
    cur_minute = (_Clock.value // MINUTE_MS) * MINUTE_MS
    rows = [{"day_ts": cur_day, "system": "flagship_repository",
             "checks": 20, "failures": 5}]
    hour_rows = [
        {"hour_ts": cur_hour, "system": "flagship_repository",
         "checks": 20, "failures": 5,
         "reason": "README.md body did not load"},
    ]
    minute_rows = [
        {"minute_ts": cur_minute, "system": "flagship_repository",
         "ok": 1, "reason": None},
    ]
    out = _run_history(rows, hour_rows, minute_rows)
    system = next(
        s for s in out["systems"] if s["id"] == "flagship_repository")
    # Raw minute buckets own the in-progress hour. The recovered minute is
    # green, but the rest of the rolling window is still a monitoring gap.
    assert system["uptime24hPct"] < 100.0
    assert system["status"] == "operational"
    assert system["reason"] is None
    assert system["reasonTs"] is None


def test_latest_failing_minute_is_down_even_if_hour_is_mostly_green():
    cur_day = (_Clock.value // DAY_MS) * DAY_MS
    cur_hour = (_Clock.value // HOUR_MS) * HOUR_MS
    cur_minute = (_Clock.value // MINUTE_MS) * MINUTE_MS
    rows = [{"day_ts": cur_day, "system": "release_linux",
             "checks": 59, "failures": 1}]
    hour_rows = [
        {"hour_ts": cur_hour, "system": "release_linux",
         "checks": 59, "failures": 1, "reason": "Linux download unreachable"},
    ]
    minute_rows = [
        {"minute_ts": cur_minute, "system": "release_linux",
         "ok": 0, "reason": "Linux download unreachable"},
    ]
    out = _run_history(rows, hour_rows, minute_rows)
    system = next(s for s in out["systems"] if s["id"] == "release_linux")
    assert system["status"] == "down"
    assert system["reason"] == "Linux download unreachable"
    assert system["reasonTs"] == cur_minute


def test_stale_latest_minute_retains_observed_state_and_warns_once():
    cur_minute = (_Clock.value // MINUTE_MS) * MINUTE_MS
    minute_rows = [
        {"minute_ts": cur_minute - 3 * MINUTE_MS, "system": "git_hosting",
         "ok": 1, "reason": None},
    ]
    out = _run_history([], minute_rows=minute_rows)
    system = next(s for s in out["systems"] if s["id"] == "git_hosting")
    assert system["status"] == "operational"
    assert system["monitoringState"] is None
    assert system["reason"] is None
    assert system["observedAt"] == cur_minute - 3 * MINUTE_MS
    assert out["current"]["monitoringState"] == "stale"
    assert out["current"]["lastCronSampleTs"] == (
        cur_minute - 3 * MINUTE_MS)


def test_shared_stale_sampler_does_not_fan_out_into_component_outages():
    cur_minute = (_Clock.value // MINUTE_MS) * MINUTE_MS
    stale_minute = cur_minute - 10 * MINUTE_MS
    system_ids = [
        system_id for system_id, _label in _load()["STATUS_SYSTEMS"]
    ]
    minute_rows = [
        {"minute_ts": stale_minute, "system": system_id,
         "ok": 1, "reason": None}
        for system_id in system_ids
    ]
    out = _run_history([], minute_rows=minute_rows)
    by_id = {system["id"]: system for system in out["systems"]}
    assert out["current"]["monitoringState"] == "stale"
    assert out["current"]["lastCronSampleTs"] == stale_minute
    assert all(by_id[system_id]["status"] == "operational"
               for system_id in system_ids)
    assert all(by_id[system_id]["monitoringState"] is None
               for system_id in system_ids)
    assert all(by_id[system_id]["reason"] is None
               for system_id in system_ids)


def test_stale_latest_failed_minute_remains_a_real_outage():
    cur_minute = (_Clock.value // MINUTE_MS) * MINUTE_MS
    minute_rows = [
        {"minute_ts": cur_minute - 3 * MINUTE_MS, "system": "database",
         "ok": 0, "reason": "Database query failed: overloaded"},
    ]
    out = _run_history([], minute_rows=minute_rows)
    system = next(s for s in out["systems"] if s["id"] == "database")
    assert system["status"] == "down"
    assert system["reason"] == "Database query failed: overloaded"
    assert system["reasonTs"] == cur_minute - 3 * MINUTE_MS
    assert out["current"]["monitoringState"] == "stale"


# --- current-state snapshot (issue #356) ------------------------------------

def _run_history_current(
    repo_count=7, online_labels=("alice", "bob"), error_count=3,
    mainnode_online=True, do_abort_count=0, unaccepted_mirrors=0,
):
    async def noop(*_a, **_k):
        return None

    async def d1_all(_env, sql, *_args):
        if "FROM repositories" in sql:
            return [{"key_bi": "mirror_bi", "data": "enc"}] if mainnode_online else []
        if "FROM host_presence" in sql:
            return [{"repo_bi": "mirror_bi", "ts": _Clock.value}] if mainnode_online else []
        return []

    async def d1_first(_env, sql, *args):
        if "FROM repositories" in sql:
            return {"n": repo_count}
        if "FROM error_log" in sql:
            # Both metrics share one bounded error-log pass. The dedicated DO
            # counter still filters legacy rows on the platform abort text and
            # adds the compact minute aggregates.
            assert sql.count("FROM error_log") == 1
            assert "COUNT(*) AS errors24h" in sql
            assert "message LIKE" in sql
            assert any(
                "Exceeded allowed duration" in str(a) for a in args
            ), sql
            assert "durable_object_abort_minute" in sql
            assert len(args) == 3
            return {
                "errors24h": error_count,
                "duration_aborts24h": do_abort_count,
            }
        if "AS unaccepted" in sql:
            assert args == (_Clock.value - 10 * 60 * 1000,)
            return {
                "ts": _Clock.value if mainnode_online else 0,
                "unaccepted": unaccepted_mirrors,
            }
        return {}

    async def decrypt_row(_env, _data):
        return {"owner": "alice", "name": "forkmesh"}

    async def _live_online_nodes(_env, _now):
        # owner_bi -> label, same shape as the real helper
        return {"bi%d" % i: label for i, label in enumerate(online_labels)}

    async def blind_index(_env, _value):
        return "mainnode_bi"

    captured = {}

    def json_response(payload, cache_seconds=None):
        captured.update(payload)
        return payload

    extra = {
        "Date": _Clock,
        "ensure_schema": noop,
        "d1_all": d1_all,
        "d1_first": d1_first,
        "decrypt_row": decrypt_row,
        "safe_segment": lambda s: str(s or "").strip().lower(),
        "_is_blocked_catalog_identity": lambda *_a: False,
        "_live_online_nodes": _live_online_nodes,
        "blind_index": blind_index,
        "json_response": json_response,
        "_REPOSITORIES_COUNT_MEMO": {"ts": 0, "count": None, "rev": None},
        "REPOSITORIES_COUNT_MEMO_MAX_AGE_MS": 600_000,
        "_CATALOG_REV_PROBE": {"ts": 0, "rev": None},
        "CATALOG_REV_PROBE_TTL_MS": 10_000,
    }
    g = _load(
        "status_history", "_repositories_count", "_catalog_rev_current",
        extra_globals=extra)
    asyncio.run(g["status_history"](object()))
    return captured


def test_current_snapshot_reports_the_four_headline_metrics():
    out = _run_history_current(
        repo_count=42, online_labels=("alice", "bob", "carol"),
        error_count=5, mainnode_online=True,
    )
    current = out["current"]
    assert current["catalogRepos"] == 42
    assert current["onlineNodes"] == 3
    assert current["errors24h"] == 5
    assert current["mainnodeOnline"] is True


def test_current_online_count_dedupes_by_node_label_not_row_count():
    # _live_online_nodes keys by owner, but two entries can carry the same label
    # (e.g. an ad-hoc re-key); the headline count must be distinct labels.
    out = _run_history_current(online_labels=("alice", "alice", "bob"))
    assert out["current"]["onlineNodes"] == 2


def test_current_snapshot_offline_mainnode_is_false():
    out = _run_history_current(mainnode_online=False)
    assert out["current"]["mainnodeOnline"] is False


def test_current_snapshot_counts_reachable_unaccepted_repository_proofs():
    out = _run_history_current(
        mainnode_online=False, unaccepted_mirrors=3)
    assert out["current"]["mainnodeUnacceptedMirrors"] == 3


def test_current_snapshot_counts_do_duration_aborts_separately():
    # AbortError("Exceeded allowed duration in Durable Objects free tier."):
    # the /status page gets its own counter for platform-killed DO requests so
    # plan-limit churn is distinguishable from real bugs in errors24h.
    out = _run_history_current(error_count=9, do_abort_count=4)
    current = out["current"]
    assert current["errors24h"] == 9
    assert current["doDurationAborts24h"] == 4


def test_current_snapshot_uses_one_indexed_24h_error_pass():
    connection = sqlite3.connect(":memory:")
    connection.row_factory = sqlite3.Row
    connection.executescript(
        """
        CREATE TABLE error_log (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            ts INTEGER NOT NULL,
            status INTEGER NOT NULL,
            method TEXT,
            path TEXT,
            message TEXT,
            ray TEXT,
            actor TEXT NOT NULL DEFAULT ''
        );
        CREATE INDEX idx_error_log_ts ON error_log(ts);
        CREATE TABLE durable_object_abort_minute (
            minute_ts INTEGER PRIMARY KEY,
            aborts INTEGER NOT NULL DEFAULT 0,
            duration_aborts INTEGER NOT NULL DEFAULT 0,
            updated_at INTEGER NOT NULL DEFAULT 0
        );
        """
    )
    cutoff = _Clock.value - DAY_MS
    connection.executemany(
        "INSERT INTO error_log(ts,status,message) VALUES (?,?,?)",
        [
            (cutoff - 1, 500, "old error outside the window"),
            (cutoff, 500, "ordinary current error"),
            (
                _Clock.value,
                503,
                "Exceeded allowed duration in Durable Objects free tier.",
            ),
        ],
    )
    connection.execute(
        "INSERT INTO durable_object_abort_minute "
        "(minute_ts,aborts,duration_aborts,updated_at) VALUES (?,?,?,?)",
        (cutoff, 3, 3, _Clock.value),
    )
    error_queries = []

    async def noop(*_args, **_kwargs):
        return None

    async def d1_all(_env, _sql, *_args):
        return []

    async def d1_first(_env, sql, *args):
        if "FROM error_log" not in sql:
            return {}
        error_queries.append((sql, args))
        row = connection.execute(sql, args).fetchone()
        return dict(row) if row is not None else None

    async def live_nodes(_env, _now):
        return {}

    captured = {}
    runtime = _load(
        "status_history",
        extra_globals={
            "Date": _Clock,
            "ensure_schema": noop,
            "d1_all": d1_all,
            "d1_first": d1_first,
            "_live_online_nodes": live_nodes,
            "json_response": (
                lambda payload, cache_seconds=None: captured.update(payload)
                or payload
            ),
        },
    )
    asyncio.run(runtime["status_history"](object()))

    assert captured["current"]["errors24h"] == 2
    assert captured["current"]["doDurationAborts24h"] == 4
    assert len(error_queries) == 1
    sql, args = error_queries[0]
    normalized = " ".join(sql.split())
    assert normalized.count("FROM error_log") == 1
    assert normalized.endswith("FROM error_log WHERE ts>=?")
    plan = [
        row[3] for row in connection.execute(
            "EXPLAIN QUERY PLAN " + sql, args
        )
    ]
    assert any(
        "SEARCH error_log USING INDEX idx_error_log_ts (ts>?)" in step
        for step in plan
    )
    assert any(
        "SEARCH durable_object_abort_minute USING INTEGER PRIMARY KEY" in step
        for step in plan
    )
    assert not any("SCAN error_log" in step for step in plan)
    connection.close()


def test_room_route_turns_a_do_duration_abort_into_a_retryable_503():
    # Regression: a room DO request that outlives the free-tier duration cap
    # dies with pyodide.http.AbortError, which used to escape _route and
    # surface as a Worker Error 1101. The room fetch must be guarded, log the
    # aggregate the cause (so /status sees it), and answer 503 without leaking
    # exception detail to the client or creating a row per reconnect.
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    src = None
    for node in ast.walk(tree):
        if isinstance(node, ast.AsyncFunctionDef) and node.name == "_route":
            src = ast.unparse(node)
    assert src, "_route not found in entry.py"
    room_block = src[src.index("room_key_from_path(url.path)"):]
    fetch_at = room_block.index("room_object.fetch")
    assert "try:" in room_block[:fetch_at]
    after_fetch = room_block[fetch_at:]
    assert "log_durable_object_abort" in after_fetch
    assert "status=503" in after_fetch
    # Repository browse keeps no second DO fetch to guard: it is ordinary
    # direct HTTPS and leaves the multiplayer room socket intact.
    browse_block = src[src.index("REPO_HOST_RE.match"):src.index(
        "room_key_from_path(url.path)")]
    assert "host_object.fetch" not in browse_block
    assert "_https_mirror_proxy" in browse_block


def test_current_mainnode_online_via_signed_forkmesh_https_proof():
    # The banner reflects a fresh, integrity-matching direct HTTPS proof for
    # forkmesh/forkmesh. A control-socket heartbeat alone is not evidence that
    # repository bytes are available.
    def _load_with_mirror():
        async def noop(*_a, **_k):
            return None

        async def d1_all(_env, sql, *_args):
            return []

        async def d1_first(_env, sql, *_args):
            if "FROM repositories" in sql:
                return {"n": 1}
            if "FROM error_log" in sql:
                return {"n": 0}
            if "AS unaccepted" in sql:
                return {"ts": _Clock.value - 1000, "unaccepted": 0}
            return {}

        async def _live_online_nodes(_env, _now):
            return {"bi0": "alice"}

        async def blind_index(_env, _value):
            return "mainnode_bi"

        captured = {}

        def json_response(payload, cache_seconds=None):
            captured.update(payload)
            return payload

        extra = {
            "Date": _Clock, "ensure_schema": noop, "d1_all": d1_all,
            "d1_first": d1_first,
            "_live_online_nodes": _live_online_nodes,
            "blind_index": blind_index, "json_response": json_response,
        }
        g = _load("status_history", extra_globals=extra)
        asyncio.run(g["status_history"](object()))
        return captured

    out = _load_with_mirror()
    assert out["current"]["mainnodeOnline"] is True
    assert out["current"]["mainnodeLastSeenTs"] == _Clock.value - 1000


def test_current_snapshot_survives_a_failing_read():
    # A blank/failing metric is None, and it must not blank the rest of the page.
    def _load_broken():
        async def noop(*_a, **_k):
            return None

        async def d1_all(_env, sql, *_args):
            return []

        async def d1_first(_env, sql, *_args):
            if "FROM repositories" in sql:
                raise RuntimeError("boom")
            return {"n": 0}

        async def _live_online_nodes(_env, _now):
            return {}

        async def blind_index(_env, _value):
            return "bi"

        captured = {}

        def json_response(payload, cache_seconds=None):
            captured.update(payload)
            return payload

        extra = {
            "Date": _Clock, "ensure_schema": noop, "d1_all": d1_all,
            "d1_first": d1_first, "_live_online_nodes": _live_online_nodes,
            "blind_index": blind_index, "json_response": json_response,
        }
        g = _load("status_history", extra_globals=extra)
        asyncio.run(g["status_history"](object()))
        return captured

    out = _load_broken()
    assert out["current"]["catalogRepos"] is None
    assert out["current"]["onlineNodes"] == 0
    # systems still rendered despite the failed metric
    assert len(out["systems"]) == len(_load()["STATUS_SYSTEMS"])


# --- roster-independent batch storage / Free-plan budgets ------------------

def test_status_batch_accepts_the_full_roster_and_rejects_poisoned_rows():
    runtime = _load("_status_batch_encode", "_status_batch_decode")
    systems = {
        system_id: {"ok": True, "reason": None}
        for system_id, _label in runtime["STATUS_SYSTEMS"]
    }
    systems.update({
        f"mirror:mirror{100 + index}": {"ok": True, "reason": None}
        for index in range(runtime["STATUS_MIRROR_MAX"])
    })
    systems.update({
        f"core:n{index + 1}": {"ok": True, "reason": None}
        for index in range(runtime["STATUS_CORE_MAX"])
    })
    systems.update({
        f"runner:fmnext-run-{index + 1}": {"ok": True, "reason": None}
        for index in range(runtime["STATUS_RUNNER_MAX"])
    })
    body = runtime["_status_batch_encode"]("minute", systems)
    decoded = runtime["_status_batch_decode"](body, "minute")
    assert len(decoded) == (
        len(runtime["STATUS_SYSTEMS"]) + runtime["STATUS_MIRROR_MAX"]
        + runtime["STATUS_CORE_MAX"] + runtime["STATUS_RUNNER_MAX"]
    )
    assert len(body.encode("utf-8")) < runtime["STATUS_BATCH_MAX_BYTES"]["minute"]

    poisoned = json.loads(body)
    poisoned["privateViewer"] = {"account": "secret"}
    assert runtime["_status_batch_decode"](
        json.dumps(poisoned), "minute") == {}
    systems["mirror:mirror149"]["reason"] = "x" * 241
    try:
        runtime["_status_batch_encode"]("minute", systems)
        raise AssertionError("oversized reason was accepted")
    except ValueError:
        pass


def test_status_batch_drops_retired_mirrors_and_keeps_the_rest_of_the_row():
    runtime = _load("_status_batch_decode")
    retired = sorted(runtime["STATUS_RETIRED_MIRRORS"])[0]
    body = json.dumps({
        "schemaVersion": 1,
        "systems": {
            "website": {"checks": 1440, "failures": 2},
            "mirror:" + retired: {"checks": 1100, "failures": 0},
            "mirror:mirror400": {"checks": 900, "failures": 1},
        },
    })
    decoded = runtime["_status_batch_decode"](body, "day")
    assert decoded == {
        "website": {"checks": 1440, "failures": 2},
        "mirror:mirror400": {"checks": 900, "failures": 1},
    }
    poisoned = json.loads(body)
    poisoned["systems"]["mirror:Bad Name"] = {"checks": 1, "failures": 0}
    assert runtime["_status_batch_decode"](json.dumps(poisoned), "day") == {}


def test_release_rows_inherit_the_history_of_the_checks_they_replaced():
    runtime = _load("_status_batch_decode")
    releases = ("release_linux", "release_android", "release_windows",
                "release_macos")
    body = json.dumps({
        "schemaVersion": 1,
        "systems": {
            "website": {"checks": 1440, "failures": 2},
            "installer": {"checks": 1440, "failures": 0},
            "homepage_downloads": {"checks": 1440, "failures": 3},
        },
    })
    decoded = runtime["_status_batch_decode"](body, "day")
    assert decoded["website"] == {"checks": 1440, "failures": 2}
    assert "installer" not in decoded and "homepage_downloads" not in decoded
    for release in releases:
        assert decoded[release] == {"checks": 1440, "failures": 3}

    only_installer = json.dumps({
        "schemaVersion": 1,
        "systems": {"installer": {"checks": 1440, "failures": 1}},
    })
    decoded = runtime["_status_batch_decode"](only_installer, "day")
    assert decoded == {release: {"checks": 1440, "failures": 1}
                       for release in releases}

    measured = json.dumps({
        "schemaVersion": 1,
        "systems": {
            "homepage_downloads": {"checks": 900, "failures": 9},
            "release_linux": {"checks": 540, "failures": 0},
        },
    })
    decoded = runtime["_status_batch_decode"](measured, "day")
    assert decoded["release_linux"] == {"checks": 540, "failures": 0}
    assert decoded["release_macos"] == {"checks": 900, "failures": 9}


def test_mid_hour_cutover_compacts_legacy_and_batch_minutes_once():
    current_hour = (_Clock.value // HOUR_MS) * HOUR_MS
    target = current_hour - HOUR_MS
    writes = []
    holder = {}

    async def d1_first(_env, _sql, *_args):
        return None

    async def d1_all(_env, sql, *_args):
        if "system_status_minute_batch" in sql:
            return holder["batch"]
        if "system_status_minute" in sql:
            return [
                {
                    "minute_ts": target + index * MINUTE_MS,
                    "system": "website", "ok": 0 if index == 4 else 1,
                    "reason": "legacy outage" if index == 4 else None,
                }
                for index in range(30)
            ]
        return []

    async def d1_run(_env, sql, *args):
        writes.append((sql, args))

    runtime = _load(
        "_compact_status_hour",
        extra_globals={"d1_first": d1_first, "d1_all": d1_all,
                       "d1_run": d1_run},
    )
    holder["batch"] = [
        {
            "minute_ts": target + index * MINUTE_MS,
            "data": runtime["_status_batch_encode"]("minute", {
                "website": {
                    "ok": index != 52,
                    "reason": "batch outage" if index == 52 else None,
                },
            }),
        }
        for index in range(30, 60)
    ]
    asyncio.run(runtime["_compact_status_hour"](
        object(), current_hour, current_hour + MINUTE_MS))
    assert len(writes) == 1
    assert "system_status_hourly_batch" in writes[0][0]
    payload = json.loads(writes[0][1][1])
    assert payload["systems"]["website"] == {
        "checks": 60, "failures": 2, "reason": "batch outage"}


def test_mid_day_cutover_compacts_legacy_and_seeded_batch_hours_once():
    current_day = (_Clock.value // DAY_MS) * DAY_MS
    target = current_day - DAY_MS
    writes = []
    holder = {}

    async def d1_first(_env, _sql, *_args):
        return None

    async def d1_all(_env, sql, *_args):
        if "system_status_hourly_batch" in sql:
            return holder["batch"]
        if "system_status_hourly" in sql:
            return [
                {
                    "hour_ts": target + index * HOUR_MS,
                    "system": "api", "checks": 60,
                    "failures": 1 if index == 3 else 0,
                    "reason": "legacy failure" if index == 3 else None,
                }
                for index in range(12)
            ]
        return []

    async def d1_run(_env, sql, *args):
        writes.append((sql, args))

    runtime = _load(
        "_compact_status_day",
        extra_globals={"d1_first": d1_first, "d1_all": d1_all,
                       "d1_run": d1_run},
    )
    holder["batch"] = [
        {
            "hour_ts": target + index * HOUR_MS,
            "data": runtime["_status_batch_encode"]("hour", {
                "api": {
                    "checks": 60,
                    "failures": 2 if index == 19 else 0,
                    "reason": "batch failure" if index == 19 else None,
                },
            }),
        }
        for index in range(12, 24)
    ]
    asyncio.run(runtime["_compact_status_day"](
        object(), current_day, current_day + MINUTE_MS))
    assert len(writes) == 1
    assert "system_status_daily_batch" in writes[0][0]
    payload = json.loads(writes[0][1][1])
    assert payload["systems"]["api"] == {
        "checks": 1440, "failures": 3}


def test_day_compaction_clamps_duplicate_legacy_minute_checks():
    current_day = (_Clock.value // DAY_MS) * DAY_MS
    target = current_day - DAY_MS
    writes = []

    async def d1_all(_env, sql, *_args):
        if "FROM system_status_hourly WHERE" in sql:
            return [
                {
                    "hour_ts": target + index * HOUR_MS,
                    "system": "api",
                    "checks": 61,
                    "failures": 61,
                    "reason": "duplicate cron delivery",
                }
                for index in range(24)
            ]
        return []

    async def d1_run(_env, sql, *args):
        writes.append((sql, args))

    runtime = _load(
        "_compact_status_day",
        extra_globals={"d1_all": d1_all, "d1_run": d1_run},
    )
    asyncio.run(runtime["_compact_status_day"](
        object(), current_day, current_day + MINUTE_MS))

    payload = json.loads(writes[0][1][1])
    assert payload["systems"]["api"] == {
        "checks": 1440,
        "failures": 1440,
    }


def test_later_hour_compaction_backfills_oldest_missing_24h_bucket():
    current_hour = (_Clock.value // HOUR_MS) * HOUR_MS
    oldest_missing = current_hour - 8 * HOUR_MS
    newer_missing = current_hour - 2 * HOUR_MS
    discovery_args = []
    writes = []

    async def d1_all(_env, sql, *args):
        if (
            "SELECT hour_ts FROM system_status_hourly_batch" in sql
            and " IN (" in sql
        ):
            discovery_args.extend(args)
            return [
                {"hour_ts": candidate}
                for candidate in args
                if candidate not in {oldest_missing, newer_missing}
            ]
        if "FROM system_status_hourly WHERE" in sql:
            return [{
                "hour_ts": oldest_missing,
                "system": "website",
                "checks": 60,
                "failures": 2,
                "reason": "legacy outage",
            }]
        return []

    async def d1_run(_env, sql, *args):
        writes.append((sql, args))

    runtime = _load(
        "_compact_status_hour",
        extra_globals={"d1_all": d1_all, "d1_run": d1_run},
    )
    asyncio.run(runtime["_compact_status_hour"](
        object(), current_hour, current_hour + 30 * MINUTE_MS))

    assert len(discovery_args) == 24
    assert discovery_args[0] == current_hour - 24 * HOUR_MS
    assert discovery_args[-1] == current_hour - HOUR_MS
    assert len(writes) == 1
    assert writes[0][1][0] == oldest_missing
    payload = json.loads(writes[0][1][1])
    assert payload["systems"]["website"] == {
        "checks": 60,
        "failures": 2,
        "reason": "legacy outage",
    }


def test_later_day_compaction_backfills_oldest_missing_history_bucket():
    current_day = (_Clock.value // DAY_MS) * DAY_MS
    oldest_missing = current_day - 12 * DAY_MS
    newer_missing = current_day - 3 * DAY_MS
    discovery_args = []
    writes = []

    async def d1_all(_env, sql, *args):
        if (
            "SELECT day_ts FROM system_status_daily_batch" in sql
            and " IN (" in sql
        ):
            discovery_args.extend(args)
            return [
                {"day_ts": candidate}
                for candidate in args
                if candidate not in {oldest_missing, newer_missing}
            ]
        if "FROM system_status_hourly WHERE" in sql:
            return [
                {
                    "hour_ts": oldest_missing + index * HOUR_MS,
                    "system": "api",
                    "checks": 60,
                    "failures": 1 if index == 5 else 0,
                    "reason": "legacy failure" if index == 5 else None,
                }
                for index in range(24)
            ]
        return []

    async def d1_run(_env, sql, *args):
        writes.append((sql, args))

    runtime = _load(
        "_compact_status_day",
        extra_globals={"d1_all": d1_all, "d1_run": d1_run},
    )
    asyncio.run(runtime["_compact_status_day"](
        object(), current_day, current_day + 30 * MINUTE_MS))

    assert len(discovery_args) == 29
    assert discovery_args[0] == current_day - 29 * DAY_MS
    assert discovery_args[-1] == current_day - DAY_MS
    assert len(writes) == 1
    assert writes[0][1][0] == oldest_missing
    payload = json.loads(writes[0][1][1])
    assert payload["systems"]["api"] == {
        "checks": 1440,
        "failures": 1,
    }


def test_first_five_minutes_refresh_latest_hour_and_day_without_gap_scan():
    current_hour = (_Clock.value // HOUR_MS) * HOUR_MS
    current_day = (_Clock.value // DAY_MS) * DAY_MS
    hour_target = current_hour - HOUR_MS
    day_target = current_day - DAY_MS
    discovery_queries = []
    writes = []

    async def d1_all(_env, sql, *_args):
        if " IN (" in sql:
            discovery_queries.append(sql)
            return []
        if "FROM system_status_minute WHERE" in sql:
            return [{
                "minute_ts": hour_target,
                "system": "website",
                "ok": 1,
                "reason": None,
            }]
        if "FROM system_status_hourly WHERE" in sql:
            return [{
                "hour_ts": day_target,
                "system": "api",
                "checks": 60,
                "failures": 0,
                "reason": None,
            }]
        return []

    async def d1_run(_env, sql, *args):
        writes.append((sql, args))

    runtime = _load(
        "_compact_status_hour", "_compact_status_day",
        extra_globals={"d1_all": d1_all, "d1_run": d1_run},
    )
    asyncio.run(runtime["_compact_status_hour"](
        object(), current_hour, current_hour + 4 * MINUTE_MS))
    asyncio.run(runtime["_compact_status_day"](
        object(), current_day, current_day + 4 * MINUTE_MS))

    assert discovery_queries == []
    assert [args[0] for _sql, args in writes] == [hour_target, day_target]


def test_repaired_yesterday_hour_rebuilds_existing_day_projection():
    current_day = (_Clock.value // DAY_MS) * DAY_MS
    repaired_hour = current_day - HOUR_MS
    writes = []

    async def noop(*_args, **_kwargs):
        return None

    async def d1_all(_env, sql, *args):
        if (
            "SELECT hour_ts FROM system_status_hourly_batch" in sql
            and " IN (" in sql
        ):
            return [
                {"hour_ts": candidate}
                for candidate in args if candidate != repaired_hour
            ]
        if "FROM system_status_minute WHERE" in sql:
            return [{
                "minute_ts": repaired_hour,
                "system": "api",
                "ok": 0,
                "reason": "late outage",
            }]
        if "FROM system_status_hourly WHERE" in sql:
            return [{
                "hour_ts": repaired_hour,
                "system": "api",
                "checks": 1,
                "failures": 1,
                "reason": "late outage",
            }]
        return []

    async def d1_run(_env, sql, *args):
        writes.append((sql, args))

    runtime = _load(
        "record_status_compaction",
        extra_globals={"ensure_schema": noop, "d1_all": d1_all,
                       "d1_run": d1_run},
    )
    asyncio.run(runtime["record_status_compaction"](
        object(), now=current_day + 30 * MINUTE_MS))

    compact_writes = [
        (sql, args) for sql, args in writes
        if "system_status_hourly_batch" in sql
        or "system_status_daily_batch" in sql
    ]
    assert [args[0] for _sql, args in compact_writes] == [
        repaired_hour, current_day - DAY_MS,
    ]


def test_same_minute_batch_overlays_matching_legacy_and_keeps_disjoint_system():
    minute = (_Clock.value // MINUTE_MS) * MINUTE_MS
    holder = {}

    async def d1_all(_env, sql, *_args):
        if "system_status_minute_batch" in sql:
            return [{"minute_ts": minute, "data": holder["body"]}]
        return [
            {"minute_ts": minute, "system": "website", "ok": 0,
             "reason": "old"},
            {"minute_ts": minute, "system": "api", "ok": 1,
             "reason": None},
        ]

    runtime = _load("_status_minute_records", extra_globals={"d1_all": d1_all})
    holder["body"] = runtime["_status_batch_encode"]("minute", {
        "website": {"ok": True, "reason": None},
    })
    rows = asyncio.run(runtime["_status_minute_records"](
        object(), minute, minute))
    by_system = {row["system"]: row for row in rows}
    assert by_system["website"]["ok"] == 1
    assert by_system["api"]["ok"] == 1


def test_deploy_roster_ignores_historical_mirrors():
    latest = (_Clock.value // MINUTE_MS) * MINUTE_MS
    queries = []

    async def d1_all(_env, sql, *args):
        queries.append((" ".join(sql.split()), args))
        if "mirror_https_endpoints" in sql:
            return [{
                "node_name": "mirror18", "forkmesh_active": 1,
                "forkmesh_verified_at": latest,
            }]
        raise AssertionError(sql)

    runtime = _load(
        "_status_recent_system_ids", extra_globals={"d1_all": d1_all})
    roster = asyncio.run(runtime["_status_recent_system_ids"](
        object(), latest))

    assert "mirror:mirror18" in roster
    assert all("system_status_minute" not in query for query, _args in queries)


def test_deploy_grace_rebuilds_active_mirrors_when_old_batch_has_retired_ids():
    now = _Clock.value
    queries = []

    async def d1_all(_env, sql, *_args):
        queries.append(" ".join(sql.split()))
        if "system_status_minute_batch" in sql:
            return [{
                "minute_ts": now - MINUTE_MS,
                # Any retired ID makes the strict historical batch ineligible
                # under the new deployment's roster contract.
                "data": json.dumps({
                    "schemaVersion": 1,
                    "systems": {
                        "mirror:mirror17": {"ok": False, "reason": "old"},
                    },
                }),
            }]
        if "mirror_https_endpoints" in sql:
            return [
                {"node_name": "mirror18", "forkmesh_active": 1,
                 "forkmesh_verified_at": now - 1_000},
                {"node_name": "mirror17", "forkmesh_active": 1,
                 "forkmesh_verified_at": now - 1_000},
            ]
        return []

    runtime = _load(
        "_status_recent_system_ids", extra_globals={"d1_all": d1_all})
    roster = asyncio.run(runtime["_status_recent_system_ids"](
        object(), now))

    assert "mirror:mirror18" in roster
    assert "mirror:mirror17" not in roster
    assert any("mirror_https_endpoints" in query for query in queries)


def test_status_history_queries_only_exact_bounded_windows():
    queries = []

    async def noop(*_args, **_kwargs):
        return None

    async def d1_all(_env, sql, *args):
        queries.append((" ".join(sql.split()), args))
        return []

    async def d1_first(_env, _sql, *_args):
        return {}

    async def live_nodes(_env, _now):
        return {}

    runtime = _load(
        "status_history",
        extra_globals={
            "Date": _Clock, "ensure_schema": noop, "d1_all": d1_all,
            "d1_first": d1_first, "_live_online_nodes": live_nodes,
            "json_response": lambda payload, cache_seconds=None: payload,
        },
    )
    asyncio.run(runtime["status_history"](object()))
    table_windows = {}
    for sql, args in queries:
        for table in (
            "system_status_daily", "system_status_daily_batch",
            "system_status_hourly", "system_status_hourly_batch",
            "system_status_minute", "system_status_minute_batch",
        ):
            if f"FROM {table} " in sql:
                table_windows[table] = args[:2]
    assert set(table_windows) == {
        "system_status_daily", "system_status_daily_batch",
        "system_status_hourly", "system_status_hourly_batch",
        "system_status_minute", "system_status_minute_batch",
    }
    for table in ("system_status_daily", "system_status_daily_batch"):
        start, end = table_windows[table]
        assert end - start == 28 * DAY_MS
    for table in ("system_status_hourly", "system_status_hourly_batch"):
        start, end = table_windows[table]
        assert 0 <= end - start <= 23 * HOUR_MS
    for table in ("system_status_minute", "system_status_minute_batch"):
        start, end = table_windows[table]
        assert end - start == 59 * MINUTE_MS


def test_free_plan_known_recurring_write_budget_keeps_incident_margin():
    # Changed rows/day, including steady retention. INTEGER PK batch tables
    # have no secondary index. Claim and minute buckets each insert+age out;
    # hour/day buckets do likewise.
    core_status_writes = 2 * 1440 + 2 * 1440 + 2 * 24 + 2
    assert core_status_writes == 5810
    # Ten-minute online persistence is bounded to 50 nodes. Conservatively
    # bill each node change/delete three times (table + composite PK + explicit
    # node-key index), plus the aggregate hour row.
    online_samples = 24 * 60 // 10
    online_writes = online_samples + 24
    online_writes += (online_samples * 50 + 24 * 50) * 3
    snapshot_writes = 3 * (1440 // 3) + 2 * (1440 // 5)
    # Both independent deep probes run four times per day. The shallow sampler
    # only reads their rows, and the follow-up flagship
    # retry phase re-probes (and writes) only while the stored result is a
    # failure, so a healthy day adds nothing here.
    healthy_deep_probe_writes = 2 * (1440 // 360)
    # Three immutable synthetic surfaces refresh their durable healthy result
    # every ten minutes instead of making seven Asset/World subrequests on
    # every minute sample.
    synthetic_probe_writes = 3 * (1440 // 10)
    # One active health lease per minute, conservatively billed as table + health
    # index + node activity. The watchdog's internal heartbeat is intentionally
    # kept in its Durable Object storage and is not duplicated into D1.
    mirror_health_writes = 1 * 3 * 1440
    watchdog_traffic_writes = 0
    # Five managed mirrors publish semantic catalog state at most every 30m;
    # reserve twelve changed/index rows for each accepted publication.
    coalesced_catalog_writes = 5 * (1440 // 30) * 12
    recurring = (
        core_status_writes + online_writes + snapshot_writes
        + healthy_deep_probe_writes + synthetic_probe_writes
        + mirror_health_writes
        + watchdog_traffic_writes + coalesced_catalog_writes)
    assert recurring == 40834
    privacy_transition_ceiling = 2160
    assert recurring + privacy_transition_ceiling == 42994
    assert 100_000 - recurring - privacy_transition_ceiling > 57_000
    # Steady status projection rows: 29 days + <=24 hours + 60 minutes.
    assert (29 + 24 + 60) * 1440 < 200_000
    assert "INSERT INTO system_status_minute (" not in ENTRY_TEXT
    assert "INSERT INTO system_status_hourly (" not in ENTRY_TEXT
    assert "INSERT INTO system_status_daily (" not in ENTRY_TEXT


def test_every_status_probe_runs_every_minute():
    due = _load("_cron_due_phases")["_cron_due_phases"]
    counts = {}
    for minute in range(1440):
        for phase in due(minute):
            counts[phase] = counts.get(phase, 0) + 1

    assert counts["mirror-health"] == 1440
    assert counts["probe-flagship"] == 1440
    assert counts["probe-release-platforms"] == 1440
    assert not any(phase.endswith("-retry") for phase in counts)
    assert _load("_cached_synthetic_status_probe")[
        "STATUS_SYNTHETIC_HEALTHY_INTERVAL_MS"] == 60_000


def test_legacy_hourly_cleanup_preserves_rollout_day_write_headroom():
    sample_source = ENTRY_TEXT[
        ENTRY_TEXT.index("async def record_status_sample"):
        ENTRY_TEXT.index("async def status_history")
    ]
    prune = sample_source[
        sample_source.index(
            '"DELETE FROM system_status_hourly WHERE hour_ts < ?"'
        ):
        sample_source.index(
            '"DELETE FROM system_status_minute WHERE minute_ts < ?"'
        )
    ]
    assert "day_ts - STATUS_HISTORY_RETAIN_MS" in prune
    assert "day_ts - STATUS_DAY_MS" not in prune


def test_status_monitor_state_upsert_skips_unchanged_heartbeats():
    source = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _record_status_monitor_transitions"):
        ENTRY_TEXT.index("async def record_status_sample")
    ]
    conflict = source[source.index("ON CONFLICT(monitor_id)"):]
    assert "checked_at=excluded.checked_at" not in conflict
    assert "WHERE repository_monitor_state.is_up!=excluded.is_up" in conflict
    assert "reason IS NOT excluded.reason" in conflict


def test_online_sample_is_weighted_ten_minutes_and_chunks_fifty_nodes():
    calls = []

    class Clock:
        @staticmethod
        def now():
            return 1_800_000_000_000  # exactly divisible by ten minutes

    async def noop(*_args, **_kwargs):
        return None

    async def live_nodes(_env, _now):
        return {f"node-{index:02d}": f"mirror{index:02d}" for index in range(50)}

    async def d1_run(_env, sql, *args):
        calls.append((sql, args))

    runtime = _load(
        "record_online_sample",
        extra_globals={"Date": Clock, "ensure_schema": noop,
                       "_live_online_nodes": live_nodes, "d1_run": d1_run},
    )
    assert asyncio.run(runtime["record_online_sample"](object())) is True
    node_calls = [
        call for call in calls
        if call[0].startswith("INSERT INTO online_hourly_nodes")]
    assert [len(args) for _sql, args in node_calls] == [80, 80, 40]
    assert all(
        args[index] == 10
        for _sql, args in node_calls for index in range(3, len(args), 4))

    class OffClock:
        @staticmethod
        def now():
            return Clock.now() + MINUTE_MS

    runtime = _load(
        "record_online_sample",
        extra_globals={"Date": OffClock, "ensure_schema": noop,
                       "_live_online_nodes": live_nodes, "d1_run": d1_run},
    )
    before = len(calls)
    assert asyncio.run(runtime["record_online_sample"](object())) is False
    assert len(calls) == before


def test_flagship_deep_probe_uses_healthy_and_failure_cadences():
    now = _Clock.value
    stored = {"is_up": 1, "reason": "", "checked_at": now - 1_000}
    probes = []
    writes = []

    async def d1_first(_env, _sql, *_args):
        return dict(stored)

    async def probe(_env):
        probes.append(True)
        return True, ""

    async def d1_run(_env, sql, *args):
        writes.append((sql, args))

    runtime = _load(
        "_flagship_repository_status",
        extra_globals={"d1_first": d1_first, "d1_run": d1_run,
                       "_flagship_repository_probe": probe},
    )
    status = runtime["_flagship_repository_status"]
    assert asyncio.run(status(object(), now)) == (True, "")
    assert probes == [] and writes == []

    # The explicit six-hour CronRunner phase must refresh even a recent
    # healthy row. Otherwise execution jitter can accept a nearly-six-hour
    # cache entry and let the shallow status sample turn red before the next
    # phase slot.
    assert asyncio.run(
        status(object(), now, allow_probe=True, force_probe=True)
    ) == (True, "")
    assert len(probes) == 1 and len(writes) == 1
    probes.clear()
    writes.clear()

    stored["checked_at"] = now - runtime["FLAGSHIP_CHECK_INTERVAL_MS"]
    assert asyncio.run(status(object(), now)) == (True, "")
    assert len(probes) == 1 and len(writes) == 1

    probes.clear()
    writes.clear()
    stored["checked_at"] = now - runtime["FLAGSHIP_CHECK_INTERVAL_MS"]
    assert asyncio.run(
        status(object(), now, allow_probe=False)
    ) == (True, "")
    assert probes == [] and writes == []

    stored.update({"is_up": 0, "reason": "down", "checked_at": now - 30_000})
    assert asyncio.run(status(object(), now)) == (False, "down")
    assert probes == [] and writes == []

    # The minute sampler is read-only while the deep probe runs in its own
    # six-hour phase. A completed failure remains the actionable result for
    # the full monitoring interval; its one-minute retry cadence applies only
    # to callers that are allowed to run the probe.
    stored["checked_at"] = now - runtime["FLAGSHIP_FAILED_RETRY_MS"]
    assert asyncio.run(
        status(object(), now, allow_probe=False)
    ) == (False, "down")
    assert probes == [] and writes == []

    stored["checked_at"] = now - runtime["FLAGSHIP_CHECK_INTERVAL_MS"]
    assert asyncio.run(
        status(object(), now, allow_probe=False)
    ) == (False, "down")
    assert probes == [] and writes == []

    # At the exact check boundary the scheduled probe and minute sampler run
    # concurrently. The shallow read keeps the prior result for one sample so
    # scheduler ordering cannot manufacture a false missing-probe minute.
    stored["checked_at"] = now - runtime["FLAGSHIP_RESULT_STALE_MS"]
    assert asyncio.run(
        status(object(), now, allow_probe=False)
    ) == (
        False,
        "The forkmesh/forkmesh deep probe did not complete within its "
        "monitoring interval",
    )
    assert probes == [] and writes == []

    stored["checked_at"] = now - runtime["FLAGSHIP_FAILED_RETRY_MS"]
    assert asyncio.run(status(object(), now)) == (True, "")
    assert len(probes) == 1 and len(writes) == 1


def test_flagship_phase_probes_every_minute_and_is_replay_safe():
    runner = ENTRY_TEXT[ENTRY_TEXT.index("async def _run_cron_phase"):]
    assert (
        'if phase == "probe-flagship":\n'
        '        return await _flagship_repository_status(\n'
        '            env, int(Date.now()), allow_probe=True, force_probe=True)'
    ) in runner
    assert '"probe-flagship-retry"' not in ENTRY_TEXT
    replay_safe = ENTRY_TEXT[
        ENTRY_TEXT.index("CRON_PHASE_REPLAY_SAFE = frozenset(("):
        ENTRY_TEXT.index("))", ENTRY_TEXT.index(
            "CRON_PHASE_REPLAY_SAFE = frozenset(("))
    ]
    assert '"probe-flagship"' in replay_safe

    now = _Clock.value
    stored = {"is_up": 1, "reason": "", "checked_at": now - 30_000}
    probes = []

    async def d1_first(_env, _sql, *_args):
        return dict(stored)

    async def probe(_env):
        probes.append(True)
        return True, ""

    async def d1_run(_env, _sql, *_args):
        return None

    runtime = _load(
        "_flagship_repository_status",
        extra_globals={"d1_first": d1_first, "d1_run": d1_run,
                       "_flagship_repository_probe": probe},
    )
    status = runtime["_flagship_repository_status"]
    assert asyncio.run(
        status(object(), now, allow_probe=True, force_probe=True)
    ) == (True, "")
    assert len(probes) == 1
    stored["checked_at"] = now - 61_000
    assert asyncio.run(
        status(object(), now, allow_probe=True, force_probe=False)
    ) == (True, "")
    assert len(probes) == 2


def test_status_transition_rings_immediate_snapshot_republish_doorbell():
    alerts = ENTRY_TEXT[
        ENTRY_TEXT.index("async def record_status_alerts"):
        ENTRY_TEXT.index("async def status_history")
    ]
    assert "transitioned = await _record_status_monitor_transitions(" in alerts
    assert (
        "if transitioned:\n"
        "        await schedule_public_snapshot_republish(\n"
        "            env, public_snapshots.SNAPSHOT_STATUS_PATHS)"
    ) in alerts

    start = ENTRY_TEXT.index("async def schedule_public_snapshot_republish")
    scheduler = ENTRY_TEXT[start:ENTRY_TEXT.index("CATALOG_CACHE_KEY", start)]
    assert "allowed = set(public_snapshots.SNAPSHOT_PATHS)" in scheduler
    assert "SNAPSHOT_REPUBLISH_PATH" in scheduler


def test_new_flagship_deep_failure_waits_for_confirmation():
    now = _Clock.value
    stored = {
        "is_up": 0,
        "reason": "Root repository tree failed (HTTP 503)",
        "changed_at": now - 30_000,
        "checked_at": now - 30_000,
    }

    async def d1_first(_env, sql, *_args):
        assert "changed_at" in sql
        return dict(stored)

    runtime = _load(
        "_flagship_repository_status",
        extra_globals={"d1_first": d1_first},
    )
    status = runtime["_flagship_repository_status"]

    # One isolated deep miss during a revision handoff remains provisional.
    # The sampler does no routed work; the next-minute retry phase confirms it.
    assert asyncio.run(status(object(), now, allow_probe=False)) == (True, "")

    # Repeated failures preserve changed_at in the real UPSERT. Once the
    # confirmation window closes, a recent failed probe is public downtime.
    stored["changed_at"] = now - runtime["FLAGSHIP_FAILURE_CONFIRM_MS"]
    stored["checked_at"] = now - 30_000
    assert asyncio.run(status(object(), now, allow_probe=False)) == (
        False, "Root repository tree failed (HTTP 503)")


def test_flagship_monitor_reads_never_advance_routes_or_serve_counters():
    probe_source = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _flagship_repository_probe"):
        ENTRY_TEXT.index("def _status_monitor_alert_setting")
    ]
    proxy_source = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _https_mirror_proxy"):
        ENTRY_TEXT.index("\n\nclass Default", ENTRY_TEXT.index(
            "async def _https_mirror_proxy"))
    ]
    assert "bypass_cache=True, monitoring=True" in probe_source
    advance = proxy_source.index("await _https_mirror_route_advance(")
    assert "if not monitoring:" in proxy_source[advance - 80:advance]


def test_flagship_repository_monitor_is_public_and_deduplicates_email_states():
    assert '("flagship_repository", "forkmesh/forkmesh repository page")' in ENTRY_TEXT
    assert "FLAGSHIP_REPOSITORY_URL = \"https://forkmesh.com/forkmesh/forkmesh\"" in ENTRY_TEXT
    assert 'str(item.get("name") or "").lower() == "readme.md"' in ENTRY_TEXT
    assert "Repository page shell did not load" in ENTRY_TEXT
    assert "await org_alias_rewrite(env, request, route_url)" in ENTRY_TEXT
    assert "Root repository tree did not contain README.md" in ENTRY_TEXT
    assert 'return "%s body did not load" % label' in ENTRY_TEXT
    assert "repository_monitor_state" in ENTRY_TEXT
    assert "notified_state" in ENTRY_TEXT
    assert "[ForkMesh outage]" in ENTRY_TEXT
    assert "[ForkMesh recovered]" in ENTRY_TEXT
    assert "is passing again after " in ENTRY_TEXT
    assert "Suggested first step" in ENTRY_TEXT
    assert "WHERE monitor_id LIKE 'status:%'" in ENTRY_TEXT


def test_flagship_repository_probe_loads_records_from_eligible_mirrors():
    pulls_ref = "a" * 40
    default_ref = "b" * 40
    calls = []
    call_options = []
    subrequest_charges = []
    missing_provenance = set()
    transient_failures = {}
    transient_statuses = {}

    payloads = {
        ("git-info-refs", "info/refs?service=git-upload-pack"): {
            "advertisement": True,
        },
        ("tree", "tree?path="): {
            "ok": True,
            "commit": default_ref,
            "entries": [{"name": "README.md", "type": "blob"}],
        },
        ("blob", "blob?path=README.md&ref=" + default_ref): {
            "ok": True, "content": "# ForkMesh",
        },
        ("tree", "tree?path=.forkmesh/issues&ref=" + default_ref): {
            "ok": True,
            "entries": [
                {"name": "open", "path": ".forkmesh/issues/open",
                 "type": "tree"},
            ],
        },
        ("tree", "tree?path=.forkmesh/issues/open&ref=" + default_ref): {
            "ok": True,
            "entries": [{"name": "12", "type": "tree"}],
        },
        ("blobs", "blobs?path=.forkmesh/issues/open/12/issue-12.json&ref="
         + default_ref): {
            "ok": True,
            "blobs": {
                ".forkmesh/issues/open/12/issue-12.json": {
                    "content": '{"number":12,"title":"Monitor me"}',
                },
            },
        },
        ("tree", "tree?path=.forkmesh/discussions&ref=" + default_ref): {
            "ok": True,
            "entries": [{"name": "3", "type": "tree"}],
        },
        ("blobs", "blobs?path=.forkmesh/discussions/3/discussion.md&ref="
         + default_ref): {
            "ok": True,
            "blobs": {
                ".forkmesh/discussions/3/discussion.md": {
                    "content": "---\ntitle: Discuss\n---\nBody",
                },
            },
        },
        ("branches", "branches"): {
            "branches": [{"name": "forkmesh/pulls", "commit": pulls_ref}],
        },
        ("tree", "tree?path=.forkmesh&ref=" + pulls_ref): {
            "ok": True,
            "entries": [{"name": "pulls", "type": "tree"}],
        },
        ("tree", "tree?path=.forkmesh%2Fpulls&ref=" + pulls_ref): {
            "ok": True,
            "entries": [{"name": "7", "type": "tree"}],
        },
        ("blobs", "blobs?path=.forkmesh/pulls/7/pull.md&ref=" + pulls_ref): {
            "ok": True,
            "blobs": {
                ".forkmesh/pulls/7/pull.md": {
                    "content": "---\ntitle: Pull\n---\nBody",
                },
            },
        },
    }

    class Headers:
        def __init__(self, values=None):
            self.values = {
                str(key).lower(): value for key, value in (values or {}).items()
            }

        def get(self, name):
            return self.values.get(str(name).lower())

    class Response:
        def __init__(self, body, served=True, status=200):
            self.body = body
            self.status = status
            self.headers = Headers(
                {"X-ForkMesh-Served-By": "mirror2"} if served else {})

        async def text(self):
            return self.body

    class Assets:
        async def fetch(self, _request):
            return Response('<main data-page="repo"></main>', served=False)

    class Env:
        ASSETS = Assets()

    class JsRequest:
        @staticmethod
        def new(url):
            return url

    async def routed(_env, url, operation, **kwargs):
        suffix = url.split("/forkmesh/forkmesh/", 1)[1]
        key = (operation, suffix)
        calls.append(key)
        call_options.append((key, kwargs))
        remaining = int(transient_failures.get(key) or 0)
        if remaining > 0:
            transient_failures[key] = remaining - 1
            status = int(transient_statuses.get(key) or 503)
            return Response(
                json.dumps({"error": (
                    "not_found" if status == 404 else "mirror_unavailable")}),
                served=False,
                status=status,
            )
        return Response(
            json.dumps(payloads[key]),
            served=key not in missing_provenance,
        )

    runtime = _load(
        "_flagship_repository_probe",
        extra_globals={
            "JsRequest": JsRequest,
            "MAX_NODE_NAME": 63,
            "clean_string": lambda value, maximum: str(value)[:maximum],
            "json": json,
            "quote": quote,
            "_routed_repository_read": routed,
            "_cron_subrequest_charge": lambda: subrequest_charges.append(1),
        },
    )
    probe = runtime["_flagship_repository_probe"]
    assert asyncio.run(probe(Env())) == (True, "")
    assert set(calls) == set(payloads)
    assert subrequest_charges == [1]

    # The proxy already exhausts its eligible candidate ring. A 5xx therefore
    # fails this sweep once and waits for the scheduled retry instead of
    # immediately doubling upstream errors and subrequests.
    calls.clear()
    call_options.clear()
    subrequest_charges.clear()
    root_key = ("tree", "tree?path=")
    transient_failures[root_key] = 1
    assert asyncio.run(probe(Env())) == (
        False, "Root repository tree failed (HTTP 503)")
    root_options = [options for key, options in call_options if key == root_key]
    assert len(root_options) == 1
    assert root_options[0]["retry_failed_proofs"] is False
    assert subrequest_charges == [1]

    # Browse/API reads cannot stand in for Git smart HTTP. A Git-only outage
    # must make the flagship probe fail even when the repository tree remains
    # readable.
    calls.clear()
    call_options.clear()
    subrequest_charges.clear()
    git_key = ("git-info-refs", "info/refs?service=git-upload-pack")
    transient_failures[git_key] = 1
    transient_statuses[git_key] = 503
    assert asyncio.run(probe(Env())) == (
        False, "Git clone advertisement failed (HTTP 503)")
    git_options = [
        options for key, options in call_options if key == git_key]
    assert len(git_options) == 1
    assert git_options[0]["monitoring"] is True
    transient_statuses.clear()

    # A single 404 during mirror revision convergence gets the same fresh
    # candidate/proof retry. A genuinely absent file still returns 404 twice
    # and fails closed below through the normal status check.
    calls.clear()
    call_options.clear()
    subrequest_charges.clear()
    readme_key = ("blob", "blob?path=README.md&ref=" + default_ref)
    transient_failures[readme_key] = 1
    transient_statuses[readme_key] = 404
    assert asyncio.run(probe(Env())) == (True, "")
    readme_options = [
        options for key, options in call_options if key == readme_key]
    assert len(readme_options) == 2
    assert readme_options[0]["context_cache"] is not (
        readme_options[1]["context_cache"])
    transient_statuses.clear()

    calls.clear()
    subrequest_charges.clear()
    missing_provenance.add((
        "tree", "tree?path=.forkmesh/discussions&ref=" + default_ref))
    ok, reason = asyncio.run(probe(Env()))
    assert ok is False
    assert reason == "Discussions collection was not served by an eligible mirror"
    assert subrequest_charges == [1]

    # The repository page renders both an absent metadata branch and an absent
    # ledger folder on that branch as an empty pull-request tab. Neither state
    # is evidence that the page or its eligible mirrors are unavailable.
    missing_provenance.clear()
    calls.clear()
    payloads[("branches", "branches")] = {"branches": []}
    assert asyncio.run(probe(Env())) == (True, "")
    assert ("tree", "tree?path=.forkmesh&ref=" + pulls_ref) not in calls

    calls.clear()
    payloads[("branches", "branches")] = {
        "branches": [{"name": "forkmesh/pulls", "commit": pulls_ref}],
    }
    payloads[("tree", "tree?path=.forkmesh&ref=" + pulls_ref)] = {
        "ok": True, "entries": [],
    }
    assert asyncio.run(probe(Env())) == (True, "")
    assert ("tree", "tree?path=.forkmesh%2Fpulls&ref=" + pulls_ref) not in calls

    # A failed routed parent-tree read is still an outage. Only an eligible
    # mirror's successful answer that omits the folder proves an empty ledger.
    calls.clear()
    missing_provenance.add((
        "tree", "tree?path=.forkmesh&ref=" + pulls_ref))
    ok, reason = asyncio.run(probe(Env()))
    assert ok is False
    assert reason == (
        "Pull request metadata tree was not served by an eligible mirror")


class _TrackedBody:
    def __init__(self, url, cancelled):
        self.url = url
        self.cancelled = cancelled

    async def cancel(self):
        self.cancelled.append(self.url)


def test_every_status_system_lists_the_checks_it_runs():
    runtime = _load()
    steps = runtime["STATUS_SYSTEM_CHECK_STEPS"]
    system_ids = [system_id for system_id, _label in runtime["STATUS_SYSTEMS"]]
    assert set(steps) == set(system_ids)
    for system_id in system_ids:
        assert steps[system_id], system_id
        for name, detail in steps[system_id]:
            assert name.strip() and len(name) <= 32, (system_id, name)
            assert detail.strip(), (system_id, name)
    for name, detail in runtime["STATUS_MIRROR_CHECK_STEPS"]:
        assert name.strip() and detail.strip()


def _next_check_runtime():
    return _load()


def test_next_check_counts_down_to_the_scheduled_probe():
    runtime = _next_check_runtime()
    minute = 29843280 + 10
    upcoming, previous = runtime["_status_phase_slots"](minute * 60000)
    next_check = runtime["_status_next_check"]

    for system_id in (
            "flagship_repository", "release_linux", "release_android",
            "release_windows", "release_macos"):
        for failing in (False, True):
            assert next_check(system_id, failing, upcoming, previous) == {
                "at": (minute + 1) * 60000,
                "last": minute * 60000,
                "kind": "probe",
                "everyMs": 60000,
            }


def test_next_check_for_sampled_rows_is_the_next_minute():
    runtime = _next_check_runtime()
    minute = 29843280 + 10
    upcoming, previous = runtime["_status_phase_slots"](minute * 60000)
    for system_id in ("website", "database", "mirror:mirror9"):
        assert runtime["_status_next_check"](
            system_id, True, upcoming, previous) == {
            "at": (minute + 1) * 60000,
            "last": minute * 60000,
            "kind": "sample",
            "everyMs": 60000,
        }


def test_status_page_draws_a_next_check_pie_per_row():
    html = (ROOT / "public" / "status.html").read_text(encoding="utf-8")
    assert "const nextCheck = system.nextCheck;" in html
    assert "function wedgePath(fraction)" in html
    assert "nextCheckMeters.forEach(updateNextCheck)" in html
    assert "window.setTimeout(tickNextChecks" in html
    css = (ROOT.parent / "www" / "public" / "styles.css").read_text(
        encoding="utf-8")
    assert ".status-next-check-remaining {" in css


def test_status_page_renders_each_check_with_hover_detail():
    html = (ROOT / "public" / "status.html").read_text(encoding="utf-8")
    assert "system.checks" in html
    assert 'className = "status-row-checks"' in html
    assert 'setAttribute("role", "tooltip")' in html
    assert '"mouseenter", () => showCheckTip(item, detail)' in html
    assert '"focus", () => showCheckTip(item, detail)' in html
    css = (ROOT.parent / "www" / "public" / "styles.css").read_text(
        encoding="utf-8")
    assert ".status-row-checks {" in css
    assert ".status-check-tip {" in css


def test_status_page_renders_current_state_grid():
    status_html = (ROOT / "public" / "status.html").read_text(encoding="utf-8")
    assert 'id="status-current"' in status_html
    assert "stat-mainnode" in status_html
    assert "stat-nodes" in status_html
    assert "stat-repos" in status_html
    assert "stat-errors" in status_html
    assert "stat-do-aborts" in status_html
    assert "doDurationAborts24h" in status_html
    assert "mainnodeUnacceptedMirrors" in status_html
    assert "repositoryRevisionConverging" in status_html
    assert "This warning does not count as service downtime." in status_html
    assert "host_presence stale" not in status_html
    assert "accepted signed" in status_html.lower()
    assert "data.current" in status_html


# --- static wiring -----------------------------------------------------------

def test_worker_exposes_status_route_and_schema():
    assert 'url.path in ("/api/status", "/api/status/")' in ENTRY_TEXT
    assert "async def status_history" in ENTRY_TEXT
    assert "async def record_status_sample" in ENTRY_TEXT
    assert "CREATE TABLE IF NOT EXISTS system_status_daily" in ENTRY_TEXT
    assert "CREATE TABLE IF NOT EXISTS system_status_hourly" in ENTRY_TEXT
    assert "CREATE TABLE IF NOT EXISTS system_status_minute_batch" in ENTRY_TEXT
    assert "CREATE TABLE IF NOT EXISTS system_status_hourly_batch" in ENTRY_TEXT
    assert "CREATE TABLE IF NOT EXISTS system_status_daily_batch" in ENTRY_TEXT
    assert "idx_system_status_daily_day" not in ENTRY_TEXT
    assert "idx_system_status_hourly_hour" not in ENTRY_TEXT
    assert "idx_system_status_minute_ts" not in ENTRY_TEXT


def test_cron_samples_status_every_tick():
    due = _load("_cron_due_phases")["_cron_due_phases"]
    assert all("status-sample" in due(minute) for minute in range(180))

    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    phase_runner = ast.unparse(next(
        node for node in tree.body
        if isinstance(node, ast.AsyncFunctionDef)
        and node.name == "_run_cron_phase"
    ))
    assert (
        "return await record_status_sample(env, source='runner', "
        "allow_deep_probes=False, deliver_alerts=False, "
        "run_compaction=False)"
    ) in phase_runner
    assert "return await record_status_compaction(env)" in phase_runner
    assert (
        "return await record_status_alerts(env, delivery_channel='ping')"
    ) in phase_runner
    assert (
        "return await record_status_alerts(env, delivery_channel='email')"
    ) in phase_runner


def test_status_page_asset_and_redirect_exist():
    status_html = (ROOT / "public" / "status.html").read_text(encoding="utf-8")
    assert "fetch(statusUrl" in status_html
    assert ': "/api/status";' in status_html
    assert 'navigationEntry?.type === "reload"' in status_html
    assert '"/api/status?fresh=browser-reload&ts=" + Date.now()' in status_html
    assert 'cache: forceFresh ? "no-store" : "default"' in status_html
    assert '"/api/status/ws"' in status_html
    assert 'message.type === "status-update"' in status_html
    assert "STATUS_POLL_MS" not in status_html
    assert "window.setInterval" not in status_html
    assert "status-day" in status_html
    assert "status-day-hour" in status_html
    assert "status-day-stack" in status_html
    assert "status-hour-bar" in status_html
    assert "status-hover-detail" in status_html
    assert "hourTooltip" in status_html
    assert "uptime24hPct" in status_html
    assert "status-history-uptime" in status_html
    assert "last 60 one-minute checks" in status_html
    assert "currentHour - 23 * 3600000" in status_html
    assert "status-minute-bar" in status_html
    assert "status-minute-slice" in status_html
    assert "minuteTooltip" in status_html
    assert status_html.index('minuteHistory.className = "status-history-row"') < status_html.index(
        'hourHistory.className = "status-history-row"') < status_html.index(
        'dayHistory.className = "status-history-row"')
    # Missing samples are red monitoring failures without fabricating an
    # observed component outage or counting the gap as service downtime.
    assert "monitoring gap" in status_html
    assert "count as downtime" not in status_html
    assert "STATUS CONTRACT — DO NOT REVERT" in status_html
    assert "must never turn a missing sample green or neutral" in status_html
    assert "component rows retain their latest observed result" in status_html
    assert "optimizing traffic usage for bots" in status_html
    assert "marker.classList.add(\"is-hovered\")" in status_html
    assert "status-day-hour is-" in status_html
    assert "status-hour-slice is-" in status_html
    assert 'if (!systems.length) return "unknown";' in status_html
    assert "latestRenderedGeneration" in status_html
    assert "generation < latestRenderedGeneration" in status_html

    status_css = (WWW_PUBLIC / "styles.css").read_text(encoding="utf-8")
    for state in ("stale", "missing", "empty"):
        assert ".status-dot.is-monitoring-%s" % state in status_css
    assert ".status-dot.is-rate-limited" in status_css
    assert "var(--danger) 88%" in status_css
    assert "var(--danger) 72%" in status_css
    assert "var(--danger) 56%" in status_css

    redirects = (WWW_PUBLIC / "_redirects").read_text(encoding="utf-8")
    assert "/status /status.html 200" not in redirects
    assert (ROOT / "public" / "status.html").is_file()


def test_status_page_names_cloudflare_rate_limiting_on_429():
    # When the free-plan daily quota runs out Cloudflare answers every route —
    # even /api/status — with an HTML 429. The status page must say exactly
    # that (a plan limit that resets on its own, not an outage) instead of the
    # generic "unavailable" message (adhoc #80).
    status_html = (ROOT / "public" / "status.html").read_text(encoding="utf-8")
    render = status_html[
        status_html.index("async function render()"):
        status_html.index("function scheduleReconnect()")
    ]
    assert "if (res.status === 429) rateLimited = true;" in render
    assert "Rate limited by Cloudflare" in render
    assert "not an outage" in render
    assert "resets automatically" in render
    # Rate limiting is red because monitoring is unhealthy, with its own shade
    # instead of the observed-outage red. Other fetch failures still show their
    # actual HTTP/content/body details instead of a generic message.
    rate_limited = render[
        render.index("if (rateLimited) {"):
        render.index("} else {", render.index("if (rateLimited) {"))
    ]
    assert 'banner.classList.add("is-rate-limited");' in rate_limited
    assert 'banner.classList.add("is-down");' not in rate_limited
    assert "Status unavailable right now." not in render
    assert "failedStatusDetail" in render
    assert "Status API unavailable" in render
    assert "Content-Type:" in status_html
    assert "Body:" in status_html
    assert "escapeHtml(issue.detail)" in render


def test_migration_file_matches_worker_schema():
    migration = (ROOT / "migrations" / "0022_system_status.sql").read_text(encoding="utf-8")
    assert "CREATE TABLE IF NOT EXISTS system_status_daily" in migration
    hourly_migration = (
        ROOT / "migrations" / "0023_system_status_hourly.sql"
    ).read_text(encoding="utf-8")
    assert "CREATE TABLE IF NOT EXISTS system_status_hourly" in hourly_migration
    batch_migration = (
        ROOT / "migrations" / "0124_system_status_batches.sql"
    ).read_text(encoding="utf-8")
    assert "CREATE TABLE IF NOT EXISTS system_status_minute_batch" in batch_migration
    assert "CREATE TABLE IF NOT EXISTS system_status_hourly_batch" in batch_migration
    assert "CREATE TABLE IF NOT EXISTS system_status_daily_batch" in batch_migration
    assert "DROP INDEX IF EXISTS idx_system_status_minute_ts" in batch_migration
    assert "idx_mailtrap_email_sends_sent_at" in batch_migration
    assert "idx_chat_history_ts" in batch_migration
    assert "idx_nodes_name" in batch_migration
    abort_migration = (
        ROOT / "migrations" / "0116_durable_object_abort_minute.sql"
    ).read_text(encoding="utf-8")
    assert "CREATE TABLE IF NOT EXISTS durable_object_abort_minute" in (
        abort_migration)
    assert "CREATE TABLE IF NOT EXISTS durable_object_abort_minute" in (
        ENTRY_TEXT)


def test_quota_sensitive_queries_use_standalone_time_and_roster_indexes():
    connection = sqlite3.connect(":memory:")
    connection.executescript(
        "CREATE TABLE mailtrap_email_sends (account_bi TEXT, sent_at INTEGER);"
        "CREATE TABLE chat_history (room_key TEXT, ts INTEGER);"
        "CREATE TABLE nodes (name TEXT, last_seen INTEGER);"
        "CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL);"
    )
    connection.executescript(
        (ROOT / "migrations" / "0124_system_status_batches.sql").read_text(
            encoding="utf-8"))

    def detail(sql, *args):
        return " ".join(
            str(row[3]) for row in connection.execute(
                "EXPLAIN QUERY PLAN " + sql, args)
        )

    assert "idx_mailtrap_email_sends_sent_at" in detail(
        "SELECT * FROM mailtrap_email_sends WHERE sent_at>=? "
        "ORDER BY sent_at DESC LIMIT 1", 0)
    assert "idx_chat_history_ts" in detail(
        "DELETE FROM chat_history WHERE ts<?", 0)
    assert "idx_nodes_name" in detail(
        "SELECT name FROM nodes WHERE name GLOB 'mirror*' AND last_seen>=?", 0)

    monitor_source = (ROOT / "src" / "status_monitoring.py").read_text(
        encoding="utf-8")
    assert "name GLOB 'mirror*'" in monitor_source
    assert "node_name GLOB 'mirror*'" in monitor_source
    assert "lower(name)" not in monitor_source
    assert "lower(node_name)" not in monitor_source


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
    print("ok")


def test_status_reports_cron_liveness_for_the_banner():
    # current.lastCronSampleTs = newest minute with a recorded sample, so the
    # page can say "the sampling cron is behind" instead of letting missing
    # samples read as a confirmed outage.
    cur_minute = (_Clock.value // MINUTE_MS) * MINUTE_MS
    minute_rows = [
        {"minute_ts": cur_minute - 7 * MINUTE_MS, "system": "website",
         "ok": 1, "reason": None},
        {"minute_ts": cur_minute - 5 * MINUTE_MS, "system": "api",
         "ok": 1, "reason": None},
    ]
    out = _run_history([], minute_rows=minute_rows)
    assert out["current"]["lastCronSampleTs"] == cur_minute - 5 * MINUTE_MS
    # No samples at all -> null, not 0 (the page treats it as "unknown").
    out = _run_history([])
    assert out["current"]["lastCronSampleTs"] is None

    page = (ROOT / "public" / "status.html").read_text(encoding="utf-8")
    assert "lastCronSampleTs" in page
    assert "sampling cron is behind" in page


def test_mirror_roster_uses_only_current_active_host_proofs():
    # Neither historical status nor a nodes.last_seen registration is the
    # Hosts roster. Only a fresh, account-bound active repository proof can
    # make a mirror eligible for a public row.
    monitoring = ENTRY.with_name("status_monitoring.py").read_text(
        encoding="utf-8")
    helper = monitoring[
        monitoring.index("async def _status_active_mirror_rows"):
        monitoring.index("async def _status_recent_system_ids")
    ]
    assert "AND forkmesh_active=1 AND forkmesh_verified_at>=?" in helper
    assert "FROM nodes" not in helper
    assert "system_status_minute" not in helper


# --- per-platform release probes -------------------------------------------

_RELEASE_APPIMAGE = "a" * 64
_RELEASE_APK = "c" * 64
_RELEASE_EXE = "e" * 64
_RELEASE_DMG = "d" * 64
_RELEASE_ASSETS = (
    ("forkmesh-linux-x86_64.AppImage", _RELEASE_APPIMAGE),
    ("forkmesh-android-universal.apk", _RELEASE_APK),
    ("forkmesh-windows-x86_64-setup.exe", _RELEASE_EXE),
    ("forkmesh-macos-arm64.dmg", _RELEASE_DMG),
    ("forkmesh-macos-x86_64.dmg", _RELEASE_DMG),
)


def _release_manifest(tag, assets):
    return {
        ".json": json.dumps({
            "schema": "forkmesh-release-v2",
            "repo": "forkmesh/forkmesh",
            "tag": tag,
            "assets": [
                {"name": name, "blob_sha256": digest, "size": 100}
                for name, digest in assets
            ],
        }),
        ".sig": base64.b64encode(b"s" * 64).decode("ascii"),
    }


def _platform_release_blobs(channel, manifest, extra=None):
    root = ".forkmesh/releases/" + channel + "/release.json"
    blobs = {
        root: {"content": manifest[".json"]},
        root + ".sig": {"content": manifest[".sig"], "encoding": "base64"},
    }
    blobs.update(extra or {})
    return {"ok": True, "blobs": blobs}


def _release_platform_probe(
        system_id="release_linux", version="0.9.0", latest_tag="v0.9.0",
        assets=_RELEASE_ASSETS, candidate=None, candidate_assets=(),
        blob_status=200, bad_signature=False, cancelled=None):
    cmake = {"desktop/CMakeLists.txt": {
        "content": "project(ForkMesh VERSION %s LANGUAGES CXX)" % version}}
    latest = _platform_release_blobs(
        "latest", _release_manifest(latest_tag, assets), cmake)
    issued = []

    class _Response:
        def __init__(self, status, body=""):
            self.status = status
            self._body = body

        async def text(self):
            return self._body

    async def routed_read(_env, url, operation, **kwargs):
        issued.append(url)
        if operation == "release-blob":
            response = _Response(blob_status)
            if cancelled is not None:
                response.body = _TrackedBody(url, cancelled)
            return response
        assert operation == "blobs"
        if "candidate-" in url:
            if candidate is None:
                return _Response(404, json.dumps({"error": "not_found"}))
            return _Response(200, json.dumps(_platform_release_blobs(
                "candidate-" + version,
                _release_manifest(candidate, candidate_assets))))
        return _Response(200, json.dumps(latest))

    async def ed25519_verify(_key, _signature, _message):
        return not bad_signature

    runtime = _load(
        "_release_platform_probe",
        "_release_version_tuple",
        extra_globals={
            "base64": base64,
            "_routed_repository_read": routed_read,
            "ed25519_verify": ed25519_verify,
        },
    )
    probe = runtime["_release_platform_probe"]
    return asyncio.run(probe(SimpleNamespace(), system_id)), issued


def test_one_tick_fetches_the_signed_manifests_once_for_all_platforms():
    cmake = {"desktop/CMakeLists.txt": {
        "content": "project(ForkMesh VERSION 0.9.0 LANGUAGES CXX)"}}
    latest = _platform_release_blobs(
        "latest", _release_manifest("v0.9.0", _RELEASE_ASSETS), cmake)
    issued = []

    class _Response:
        def __init__(self, status, body=""):
            self.status = status
            self._body = body

        async def text(self):
            return self._body

    async def routed_read(_env, url, operation, **kwargs):
        issued.append(operation)
        if operation == "release-blob":
            return _Response(200)
        return _Response(200, json.dumps(latest))

    async def verify(_key, _signature, _message):
        return True

    runtime = _load(
        "_release_platform_probe",
        "_release_version_tuple",
        extra_globals={
            "base64": base64,
            "_routed_repository_read": routed_read,
            "ed25519_verify": verify,
        },
    )
    probe = runtime["_release_platform_probe"]
    memo = {}
    for system_id in _RELEASE_ROWS:
        assert asyncio.run(probe(SimpleNamespace(), system_id, memo)) == (
            True, "")
    assert issued.count("blobs") == 1


_RELEASE_ROWS = {
    "release_linux": "Linux latest version",
    "release_android": "Android latest version",
    "release_windows": "Windows latest version",
    "release_macos": "macOS latest version",
}


def test_each_platform_has_its_own_release_row_probed_every_minute():
    runtime = _load()
    systems = dict(runtime["STATUS_SYSTEMS"])
    assert "homepage_downloads" not in systems
    for system_id, label in _RELEASE_ROWS.items():
        assert systems[system_id] == label
        assert system_id in runtime["STATUS_SYSTEM_CHECKS"]
        assert len(runtime["STATUS_SYSTEM_CHECK_STEPS"][system_id]) == 3
        assert ('"%s": (' % system_id) in ENTRY_TEXT[
            ENTRY_TEXT.index("STATUS_MONITOR_GUIDANCE = {"):]
    monitors = runtime["RELEASE_PLATFORM_MONITORS"]
    assert set(monitors) == set(_RELEASE_ROWS)
    assert len({monitor[0] for monitor in monitors.values()}) == 4
    for system_id in _RELEASE_ROWS:
        assert runtime["STATUS_SYSTEM_PROBE_PHASES"][system_id] == (
            "probe-release-platforms",)
    due = _load("_cron_due_phases")["_cron_due_phases"]
    counts = {}
    for minute in range(1440):
        for phase in due(minute):
            counts[phase] = counts.get(phase, 0) + 1
    assert counts["probe-release-platforms"] == 1440
    assert "probe-homepage-downloads" not in counts


@pytest.mark.parametrize("system_id, digest", [
    ("release_linux", _RELEASE_APPIMAGE),
    ("release_android", _RELEASE_APK),
    ("release_windows", _RELEASE_EXE),
    ("release_macos", _RELEASE_DMG),
])
def test_release_platform_probe_passes_for_its_build_in_the_newest_release(
        system_id, digest):
    (ok, reason), issued = _release_platform_probe(system_id)
    assert (ok, reason) == (True, "")
    blob_reads = [url for url in issued if "releases/blob/" in url]
    assert blob_reads == [
        url for url in blob_reads if url.endswith("sha256/" + digest)]
    assert len(blob_reads) == 1
    assert not any("candidate-" in url for url in issued)


def test_release_platform_probe_names_its_build_missing_from_latest():
    linux_only = (("forkmesh-linux-x86_64.AppImage", _RELEASE_APPIMAGE),)
    assert _release_platform_probe(assets=linux_only)[0] == (True, "")
    (ok, reason), issued = _release_platform_probe(
        "release_windows", assets=linux_only)
    assert not ok
    assert reason == (
        "Windows is not published in v0.9.0: "
        "forkmesh-windows-x86_64-setup.exe")
    assert not any("releases/blob/" in url for url in issued)
    (ok, reason), _issued = _release_platform_probe(
        "release_macos", assets=linux_only + (
            ("forkmesh-macos-arm64.dmg", _RELEASE_DMG),))
    assert reason == (
        "macOS is not published in v0.9.0: forkmesh-macos-x86_64.dmg")


def test_platforms_turn_green_one_by_one_as_a_new_version_ships():
    newer = dict(
        version="1.2.6", latest_tag="v1.2.5", candidate="v1.2.6",
        candidate_assets=(
            ("forkmesh-linux-x86_64.AppImage", _RELEASE_APPIMAGE),))
    (ok, reason), issued = _release_platform_probe("release_linux", **newer)
    assert (ok, reason) == (True, "")
    assert any("candidate-1.2.6" in url for url in issued)
    (ok, reason), _issued = _release_platform_probe("release_android", **newer)
    assert not ok
    assert reason == (
        "v1.2.6 is released without its Android build: "
        "forkmesh-android-universal.apk")


def test_release_platform_probe_ignores_a_version_bump_without_builds():
    (ok, reason), issued = _release_platform_probe(
        "release_android", version="1.2.1", latest_tag="v0.8.10")
    assert (ok, reason) == (True, "")
    assert any("candidate-1.2.1" in url for url in issued)


def test_release_platform_probe_ignores_a_mislabelled_candidate():
    (ok, _reason), _issued = _release_platform_probe(
        version="1.2.1", latest_tag="v0.8.10", candidate="v1.2.0")
    assert ok


def test_release_platform_probe_rejects_an_unverified_manifest():
    (ok, reason), _issued = _release_platform_probe(bad_signature=True)
    assert not ok
    assert "does not verify" in reason


def test_release_platform_probe_reports_unreachable_and_unrouted_blobs():
    (ok, reason), _issued = _release_platform_probe(blob_status=404)
    assert (ok, reason) == (False, (
        "Linux download is unreachable: "
        "forkmesh-linux-x86_64.AppImage v0.9.0"))
    (ok, reason), _issued = _release_platform_probe(
        "release_windows", blob_status=503)
    assert (ok, reason) == (False, (
        "No reachable mirror served the Windows download "
        "forkmesh-windows-x86_64-setup.exe"))


def test_one_platform_probe_crash_does_not_stop_the_others():
    probed = []

    memos = []

    async def status(_env, _now, system_id, allow_probe=True,
                     force_probe=False, manifest_memo=None):
        assert allow_probe and force_probe
        memos.append(manifest_memo)
        probed.append(system_id)
        if system_id == "release_android":
            raise ValueError("boom")
        return True, ""

    run = _load("_probe_release_platforms", extra_globals={
        "_release_platform_status": status,
    })["_probe_release_platforms"]
    with pytest.raises(RuntimeError, match="Android: boom"):
        asyncio.run(run(object(), _Clock.value))
    assert probed == list(_RELEASE_ROWS)
    assert isinstance(memos[0], dict)
    assert all(memo is memos[0] for memo in memos)


def test_core_mail_status_skips_rows_from_before_delivery_tracking():
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "status_monitoring_core_mail", ROOT / "src" / "status_monitoring.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    db = sqlite3.connect(":memory:")
    db.row_factory = sqlite3.Row
    db.execute("CREATE TABLE mail_sends (send_id TEXT, sent_at INTEGER, "
               "status TEXT NOT NULL DEFAULT '', status_at INTEGER NOT NULL DEFAULT 0, "
               "dsn TEXT NOT NULL DEFAULT '')")

    async def d1_first(_env, sql, *params):
        row = db.execute(sql, params).fetchone()
        return dict(row) if row else None

    module._bind_runtime({
        "d1_first": d1_first,
        "clean_string": lambda value, limit: str(value or "")[:limit],
    })
    module._smtp_answers = lambda *a, **k: True
    now = 2_000_000_000_000
    hour = 60 * 60 * 1000
    check = lambda: asyncio.run(module._core_mail_status(None, now))
    db.execute("INSERT INTO mail_sends VALUES ('legacy', ?, '', 0, '')", (now - hour,))
    assert check() == (True, "")
    db.execute("INSERT INTO mail_sends VALUES ('old', ?, 'delivery', 1, '2.0.0')", (now - 2 * hour,))
    assert check() == (True, "")
    db.execute("INSERT INTO mail_sends VALUES ('stuck', ?, 'accepted', 1, '')", (now - hour // 2 - 1,))
    assert check() == (False, "The most recent email has no delivery event after 30 minutes")
    db.execute("INSERT INTO mail_sends VALUES ('fresh', ?, 'accepted', 1, '')", (now - 60_000,))
    assert check() == (True, "")
    db.execute("INSERT INTO mail_sends VALUES ('bounced', ?, 'bounce', 1, '')", (now - 1000,))
    assert check() == (False, "The most recent email was reported as bounce")


def _core_mail_module(rows):
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "status_monitoring_core_mail_dsn", ROOT / "src" / "status_monitoring.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    db = sqlite3.connect(":memory:")
    db.row_factory = sqlite3.Row
    db.execute("CREATE TABLE mail_sends (send_id TEXT, sent_at INTEGER, "
               "status TEXT NOT NULL DEFAULT '', status_at INTEGER NOT NULL DEFAULT 0, "
               "dsn TEXT NOT NULL DEFAULT '')")
    db.executemany("INSERT INTO mail_sends VALUES (?, ?, ?, 1, ?)", rows)

    async def d1_first(_env, sql, *params):
        row = db.execute(sql, params).fetchone()
        return dict(row) if row else None

    module._bind_runtime({
        "d1_first": d1_first,
        "clean_string": lambda value, limit: str(value or "")[:limit],
    })
    module._smtp_answers = lambda *a, **k: True
    return module


@pytest.mark.parametrize("status,dsn,age_minutes,expected", [
    ("soft bounce", "4.4.3", 31, (False,
        "The most recent email is still deferred after 30 minutes: looking up "
        "the recipient's mail server in DNS failed (4.4.3)")),
    ("soft bounce", "4.7.1", 31, (False,
        "The most recent email is still deferred after 30 minutes: the "
        "recipient's server refused it for security or policy reasons (4.7.1)")),
    ("soft bounce", "4.4.3", 5, (True, "")),
    ("bounce", "5.1.1", 1, (True, "")),
    ("bounce", "5.1.10", 1, (True, "")),
    ("bounce", "5.1.8", 1, (False,
        "The most recent email was reported as bounce: the recipient address "
        "was not accepted (5.1.8)")),
    ("bounce", "5.4.4", 1, (False,
        "The most recent email was reported as bounce: the recipient's domain "
        "has no mail server (5.4.4)")),
    ("bounce", "5.9.9", 1, (False,
        "The most recent email was reported as bounce (5.9.9)")),
])
def test_core_mail_status_names_the_postfix_cause(status, dsn, age_minutes,
                                                  expected):
    now = 2_000_000_000_000
    module = _core_mail_module(
        [("latest", now - age_minutes * 60_000, status, dsn)])
    assert asyncio.run(module._core_mail_status(None, now)) == expected


def test_status_page_describes_the_core_mail_check_under_smtp():
    runtime = _load()
    smtp = type("Env", (), {"MAIL_TRANSPORT": "smtp"})()
    mailtrap = type("Env", (), {"MAIL_TRANSPORT": ""})()
    assert runtime["_mail_over_smtp"](smtp)
    assert not runtime["_mail_over_smtp"](mailtrap)
    assert "Postfix" in runtime["STATUS_EMAIL_SMTP_CHECK"]
    assert "Mailtrap" not in runtime["STATUS_EMAIL_SMTP_CHECK"]
    assert all("Mailtrap" not in name + detail
               for name, detail in runtime["STATUS_EMAIL_SMTP_CHECK_STEPS"])
    history = ENTRY_TEXT[
        ENTRY_TEXT.index("async def status_history("):]
    assert ('if system_id == "email" and _mail_over_smtp(env):\n'
            '            check_description = STATUS_EMAIL_SMTP_CHECK') in history


# --- per-core monitors ------------------------------------------------------

def _core_row(name, age_ms=2_000, status="active"):
    return {"node_id": name, "status": status,
            "heartbeat_ms": _Clock.value - age_ms}


def _core_vote(voter, target, app=True, node=True, age_ms=10_000):
    return {"voter": voter, "target": target,
            "healthy": 1 if app else 0,
            "observed_ms": _Clock.value - age_ms,
            "detail": json.dumps({"app": app, "node": node})}


@pytest.mark.parametrize("count", [1, 3, 15])
def test_every_core_in_the_cluster_gets_its_own_monitor(count):
    names = ["n%d" % (index + 1) for index in range(count)]
    votes = [_core_vote(voter, target) for voter in names for target in names]
    results, reasons, minutes = _run_sample(
        core_rows=[_core_row(name) for name in names], vote_rows=votes,
        ids_rows=[{"node_id": name, "beat_ms": _Clock.value - 5_000,
                   "state": "normal"} for name in names])
    cores = sorted(key for key in results if key.startswith("core:"))
    assert cores == sorted("core:" + name for name in names)
    assert all(minutes[key] == (1, None) for key in cores)


def test_core_monitor_fails_on_each_kind_of_degradation():
    rows = [
        _core_row("n1"),
        _core_row("n2", age_ms=5 * 60_000),
        _core_row("n3", status="sealed"),
        _core_row("n4"),
        _core_row("n5"),
        _core_row("n6"),
        _core_row("n7", status="quarantined"),
    ]
    votes = [
        _core_vote("n1", "n4", app=False),
        _core_vote("n5", "n4", app=True),
        _core_vote("n1", "n5", node=False),
        _core_vote("n1", "n1", app=False, age_ms=10 * 60_000),
    ]
    ids = [
        {"node_id": "n6", "beat_ms": _Clock.value - 10 * 60_000,
         "state": "normal"},
        {"node_id": "n1", "beat_ms": _Clock.value - 1_000,
         "state": "normal"},
    ]
    results, reasons, _minutes = _run_sample(
        core_rows=rows, vote_rows=votes, ids_rows=ids)
    assert results["core:n1"] == 0
    assert "has not written a cluster heartbeat for 5 minutes" in (
        reasons["core:n2"])
    assert "sealed" in reasons["core:n3"]
    assert "app HTTPS readiness probe from n1" in reasons["core:n4"]
    assert "node HTTPS readiness probe from n1" in reasons["core:n5"]
    assert "intrusion detector report for 10 minutes" in reasons["core:n6"]
    assert "quarantined" in reasons["core:n7"]
    for name in ("n2", "n3", "n4", "n5", "n6", "n7"):
        assert results["core:" + name] == 1
        assert reasons["core:" + name].startswith(name + " ")


def test_core_in_ids_lockdown_is_red():
    results, reasons, _minutes = _run_sample(
        core_rows=[_core_row("n2")],
        ids_rows=[{"node_id": "n2", "beat_ms": _Clock.value,
                   "state": "lockdown"}])
    assert results["core:n2"] == 1
    assert "lockdown" in reasons["core:n2"]


def test_core_gone_for_a_day_leaves_the_roster_and_unreadable_roster_adds_none():
    results, _reasons, _minutes = _run_sample(
        core_rows=[_core_row("n1"), _core_row("n9", age_ms=25 * HOUR_MS)])
    assert "core:n1" in results and "core:n9" not in results
    results, _reasons, _minutes = _run_sample(
        core_rows=RuntimeError("no such table: cluster_members"))
    assert not any(key.startswith("core:") for key in results)
    assert results["database"] == 0


# --- per-runner monitors ----------------------------------------------------

def _runner_row(name, age_ms=20_000, scope="pool", status="active"):
    return {"name": name, "scope": scope, "status": status,
            "last_seen_at": _Clock.value - age_ms if age_ms is not None else 0}


def test_every_active_pool_runner_gets_its_own_monitor():
    results, reasons, minutes = _run_sample(runner_rows=[
        _runner_row("r1"),
        _runner_row("fmnext-run-2", age_ms=2 * 60_000),
        _runner_row("someones-laptop", scope="personal"),
        _runner_row("fmnext-run-1", status="revoked"),
    ])
    runners = sorted(key for key in results if key.startswith("runner:"))
    assert runners == ["runner:fmnext-run-2", "runner:r1"]
    assert all(minutes[key] == (1, None) for key in runners)


def test_runner_monitor_fails_when_it_stops_claiming():
    results, reasons, _minutes = _run_sample(
        runner_rows=[
            _runner_row("r1", age_ms=10 * 60_000),
            _runner_row("fmnext-run-3", age_ms=4 * 60_000),
            _runner_row("fmnext-run-4", age_ms=None),
        ],
        lease_rows=[{"runner": "r1", "n": 2}])
    assert results["runner:r1"] == 1
    assert reasons["runner:r1"] == (
        "r1 has not claimed or polled for jobs for 10 minutes while "
        "holding 2 job leases")
    assert results["runner:fmnext-run-3"] == 1
    assert "4 minutes" in reasons["runner:fmnext-run-3"]
    assert "holding" not in reasons["runner:fmnext-run-3"]
    assert results["runner:fmnext-run-4"] == 1
    assert "never claimed" in reasons["runner:fmnext-run-4"]


def test_runner_monitor_fails_when_a_provider_login_expired():
    expired = {"claude-code": {"binaryFound": True, "loginState": "expired",
                               "auth": {"state": "expired", "reason": "job_failed"}},
               "codex": {"binaryFound": False, "loginState": "missing"}}
    expiring = {"claude-code": {"binaryFound": True, "loginState": "available",
                                "auth": {"state": "expiring"}}}
    rows = [dict(_runner_row("fmnext-r1"), availability=json.dumps(expired)),
            dict(_runner_row("fmnext-r2"), availability=json.dumps(expiring)),
            dict(_runner_row("fmnext-r3"), availability=None)]
    results, reasons, _minutes = _run_sample(runner_rows=rows)
    assert results["runner:fmnext-r1"] == 1
    assert reasons["runner:fmnext-r1"] == "fmnext-r1: Claude login expired — reconnect"
    assert results["runner:fmnext-r2"] == 0
    assert results["runner:fmnext-r3"] == 0


def test_unreadable_runner_roster_adds_no_rows():
    results, _reasons, _minutes = _run_sample(
        runner_rows=RuntimeError("no such table: runner_nodes"))
    assert not any(key.startswith("runner:") for key in results)
    assert results["database"] == 0


def test_runner_roster_query_is_pool_and_active_only():
    source = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _status_runner_rows"):
        ENTRY_TEXT.index("def _status_runner_verdicts")]
    assert "scope='pool' AND status='active'" in source


def test_recorded_runner_appears_as_a_full_status_system():
    cur_minute = (_Clock.value // MINUTE_MS) * MINUTE_MS
    minute_rows = [
        {"minute_ts": cur_minute, "system": "runner:r1", "ok": 0,
         "reason": "r1 has not claimed or polled for jobs for 10 minutes"},
        {"minute_ts": cur_minute, "system": "runner:fmnext-run-9", "ok": 0,
         "reason": "revoked"},
    ]
    out = _run_history(
        [], minute_rows=minute_rows, runner_rows=[_runner_row("r1")])
    system = next(s for s in out["systems"] if s["id"] == "runner:r1")
    assert system["label"] == "Runner — r1"
    assert system["status"] != "operational"
    assert [check["name"] for check in system["checks"]] == [
        "Registered and active", "Recent claim or poll", "Provider logins"]
    assert "three minutes" in system["checkDescription"]
    assert all(s["id"] != "runner:fmnext-run-9" for s in out["systems"])
    ids = [s["id"] for s in out["systems"]]
    assert ids.index("runner:r1") > max(
        ids.index(system_id) for system_id in ids
        if not system_id.startswith(("runner:", "mirror:", "core:")))


def test_invalid_core_names_are_not_status_ids():
    runtime = _load("_status_batch_system_id")
    assert runtime["_status_batch_system_id"]("core:n1") == "core:n1"
    assert runtime["_status_batch_system_id"]("core:N1") == ""
    assert runtime["_status_batch_system_id"]("core:../x") == ""
    assert runtime["_status_batch_system_id"]("runner:r1") == "runner:r1"
    assert runtime["_status_batch_system_id"]("runner:R1") == ""
    assert runtime["_status_batch_system_id"]("runner:../x") == ""


def test_recorded_core_appears_as_a_full_status_system():
    cur_minute = (_Clock.value // MINUTE_MS) * MINUTE_MS
    minute_rows = [
        {"minute_ts": cur_minute, "system": "core:n2", "ok": 0,
         "reason": "n2 is sealed and waiting for its vault key"},
        {"minute_ts": cur_minute, "system": "core:n8", "ok": 0,
         "reason": "retired"},
    ]
    out = _run_history(
        [], minute_rows=minute_rows,
        core_rows=[_core_row("n2"), _core_row("n8", age_ms=25 * HOUR_MS)])
    system = next(s for s in out["systems"] if s["id"] == "core:n2")
    assert system["label"] == "Core node — n2"
    assert system["status"] != "operational"
    assert [check["name"] for check in system["checks"]] == [
        "Cluster heartbeat", "Unsealed and active", "Peer HTTPS probes",
        "Intrusion detector"]
    assert "pinged every five minutes" in system["checkDescription"]
    assert all(s["id"] != "core:n8" for s in out["systems"])


def test_status_only_release_reads_release_their_bodies():
    cancelled = []
    (ok, reason), issued = _release_platform_probe(cancelled=cancelled)
    assert (ok, reason) == (True, "")
    blob_reads = [url for url in issued if "releases/blob/" in url]
    assert blob_reads and sorted(cancelled) == sorted(blob_reads)


def test_flagship_and_proxy_release_bodies_they_do_not_read():
    probe_source = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _flagship_repository_probe"):
        ENTRY_TEXT.index("def _status_monitor_alert_setting")
    ]
    assert "await _discard_response_body(git_response)" in probe_source
    oversized = probe_source.index("if announced > maximum:")
    assert "await _discard_response_body(response)" in probe_source[
        oversized:oversized + 120]
    proxy_source = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _https_mirror_proxy"):
        ENTRY_TEXT.index("GIT_PUSH_SIGNATURE_PREFIX = ")
    ]
    retry = proxy_source.index("or status in HTTPS_MIRROR_RETRY_STATUSES")
    assert "await _discard_response_body(upstream)" in proxy_source[
        retry:retry + 300]


def test_discard_response_body_cancels_and_tolerates_bodyless_responses():
    discard = _load("_discard_response_body")["_discard_response_body"]
    cancelled = []
    asyncio.run(discard(SimpleNamespace(body=_TrackedBody("x", cancelled))))
    assert cancelled == ["x"]
    asyncio.run(discard(SimpleNamespace(body=None)))
    asyncio.run(discard(SimpleNamespace()))

    class Locked:
        async def cancel(self):
            raise RuntimeError("locked")
    asyncio.run(discard(SimpleNamespace(body=Locked())))


def _monitor_state_db():
    db = sqlite3.connect(":memory:")
    db.row_factory = sqlite3.Row
    db.execute(
        "CREATE TABLE repository_monitor_state ("
        "monitor_id TEXT PRIMARY KEY, is_up INTEGER NOT NULL DEFAULT 1,"
        "changed_at INTEGER NOT NULL,"
        "outage_started_at INTEGER NOT NULL DEFAULT 0,"
        "checked_at INTEGER NOT NULL, reason TEXT,"
        "notified_state TEXT NOT NULL DEFAULT '',"
        "pinged_state TEXT NOT NULL DEFAULT '')")

    async def d1_first(_env, sql, *args):
        row = db.execute(sql, args).fetchone()
        return dict(row) if row else None

    async def d1_run(_env, sql, *args):
        db.execute(sql, args)
    return d1_first, d1_run


@pytest.mark.parametrize("status_name, probe_name, args, reason", [
    ("_release_platform_status", "_release_platform_probe",
     ("release_windows",),
     "No reachable mirror served the release metadata"),
])
def test_one_failed_deep_probe_needs_the_next_minutes_probe_to_confirm(
        status_name, probe_name, args, reason):
    d1_first, d1_run = _monitor_state_db()
    verdicts = []

    async def probe(_env, *probe_args, **_options):
        assert probe_args == args
        return verdicts.pop(0)

    status = _load(status_name, extra_globals={
        "d1_first": d1_first, "d1_run": d1_run, probe_name: probe,
    })[status_name]
    start = _Clock.value

    def minute(offset, verdict):
        at = start + offset * 60_000
        verdicts.append(verdict)
        asyncio.run(status(object(), at + 20_000, *args, allow_probe=True,
                           force_probe=True))
        return asyncio.run(
            status(object(), at + 61_000, *args, allow_probe=False))

    assert minute(0, (True, "")) == (True, "")
    assert minute(1, (False, reason)) == (True, "")
    assert minute(2, (True, "")) == (True, "")
    assert minute(3, (False, reason)) == (True, "")
    assert minute(4, (False, reason)) == (False, reason)
    assert minute(5, (False, reason)) == (False, reason)
    assert minute(6, (True, "")) == (True, "")
    if args:
        assert asyncio.run(status(
            object(), start, "release_linux", allow_probe=False)) == (
                False, "The Linux release probe has not completed")


def test_one_failed_synthetic_probe_needs_the_next_minutes_probe_to_confirm():
    d1_first, d1_run = _monitor_state_db()
    runtime = _load("_cached_synthetic_status_probe", extra_globals={
        "d1_first": d1_first, "d1_run": d1_run,
        "_STATUS_SYNTHETIC_PROBE_MEMO": {},
    })
    cached = runtime["_cached_synthetic_status_probe"]
    monitor_id = runtime["STATUS_WORLD_MOBILE_MONITOR_ID"]
    verdicts = []
    probes = []

    async def probe(_env):
        probes.append(True)
        return verdicts.pop(0)

    start = _Clock.value
    failure = (False, "Mobile World health returned HTTP 503")

    def minute(offset, verdict):
        runtime["_STATUS_SYNTHETIC_PROBE_MEMO"].clear()
        verdicts.append(verdict)
        return asyncio.run(cached(
            object(), monitor_id, start + offset * 60_000, probe))

    assert minute(0, (True, "")) == (True, "")
    assert minute(1, failure) == (True, "")
    assert asyncio.run(cached(
        object(), monitor_id, start + 60_000 + 5_000, probe)) == (True, "")
    assert minute(2, (True, "")) == (True, "")
    assert minute(3, failure) == (True, "")
    assert minute(4, failure) == failure
    assert asyncio.run(cached(
        object(), monitor_id, start + 4 * 60_000 + 5_000, probe)) == failure
    assert minute(5, (True, "")) == (True, "")
    assert len(probes) == 6


def test_a_raising_synthetic_probe_needs_the_next_minutes_probe_to_confirm():
    d1_first, d1_run = _monitor_state_db()
    runtime = _load("_cached_synthetic_status_probe", extra_globals={
        "d1_first": d1_first, "d1_run": d1_run,
        "_STATUS_SYNTHETIC_PROBE_MEMO": {},
    })
    cached = runtime["_cached_synthetic_status_probe"]
    monitor_id = runtime["STATUS_WORLD_MOBILE_MONITOR_ID"]
    outcomes = []

    async def probe(_env):
        outcome = outcomes.pop(0)
        if isinstance(outcome, Exception):
            raise outcome
        return outcome

    start = _Clock.value
    dns = OSError(
        "cannot resolve world.forkmesh.com: [Errno -3] "
        "Temporary failure in name resolution")
    prefix = "Mobile World probe failed: "
    failure = (False, prefix + str(dns))

    def minute(offset, outcome):
        runtime["_STATUS_SYNTHETIC_PROBE_MEMO"].clear()
        outcomes.append(outcome)
        return asyncio.run(cached(
            object(), monitor_id, start + offset * 60_000, probe, prefix))

    assert minute(0, (True, "")) == (True, "")
    assert minute(1, dns) == (True, "")
    assert minute(2, (True, "")) == (True, "")
    assert minute(3, dns) == (True, "")
    assert minute(4, dns) == failure
    assert minute(5, (True, "")) == (True, "")
