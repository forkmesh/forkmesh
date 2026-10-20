import json
import re
import ast
import sqlite3
import sys
from pathlib import Path

import pytest


APP = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(APP / "src"))
import public_snapshots as snapshots  # noqa: E402


def public_repository(**overrides):
    record = {
        "owner": "alice",
        "name": "demo",
        "visibility": "public",
        "isPrivate": False,
        "sharedWithMe": False,
        "website": "https://example.test/demo",
        "logoMetadata": {
            "description": "demo",
            "languages": {"Python": 20},
            "topics": ["mesh"],
            "fileStructure": ["src"],
            "frameworks": [],
            "projectCategory": "tool",
        },
        "logicalOwners": [{"kind": "user", "owner": "alice"}],
    }
    record.update(overrides)
    return record


def catalog(*records, **overrides):
    payload = {
        "ok": True,
        "repositories": list(records),
        "sshGatewayConfigured": False,
        "sshTransport": "not-configured",
        "payoutCustody": "external",
        "payoutNotice": "public addresses only",
    }
    payload.update(overrides)
    return payload


def test_catalog_validator_accepts_canonical_and_legacy_device_shapes():
    assert snapshots.validate_snapshot_payload(
        "/api/repositories", catalog(public_repository()))
    # Older public device-mirror rows legitimately lack derived canonical-only
    # fields. Optional means absent, never an unchecked null/value.
    device = {
        "owner": "mirror2", "name": "forkmesh", "visibility": "public",
        "isPrivate": False, "source": "remote-clone",
    }
    assert snapshots.validate_snapshot_payload(
        "/api/repositories", catalog(device))


def test_catalog_validator_accepts_mirror_and_fork_records():
    mirror = public_repository(
        mirrorOf="https://upstream.example/alice/demo",
        upstreamCommit="520ba6a3a36f0850b586c9122c7607a3eb2cf7c1",
        lastSyncedAt="1792359203233",
    )
    fork = public_repository(name="demo-fork", forkedFrom="alice/demo")
    assert snapshots.validate_snapshot_payload(
        "/api/repositories", catalog(mirror, fork))


@pytest.mark.parametrize("poison", [
    {"privateRepositories": [{"name": "secret"}]},
    {"repositories": [public_repository(accessToken="secret")]},
    {"repositories": [public_repository(
        logoMetadata={"languages": {}, "sourceContent": "secret"})]},
    {"repositories": [public_repository(
        logicalOwners=[{"kind": "user", "owner": "alice", "email": "x"}])]},
    {"repositories": [public_repository(isPrivate=True)]},
    {"repositories": [public_repository(sharedWithMe=True)]},
    {"repositories": [public_repository(privateAccessId="secret")]},
    {"repositories": [public_repository(logoMetadata=None)]},
    {"repositories": [public_repository(logicalOwners=None)]},
])
def test_catalog_validator_rejects_private_or_future_schema_poison(poison):
    payload = catalog(public_repository())
    payload.update(poison)
    assert not snapshots.validate_snapshot_payload("/api/repositories", payload)


def test_snapshot_rows_are_ascii_byte_counted_versioned_and_fresh_only():
    generated = 1_800_000_000_000
    payload = catalog(public_repository(description="maillage ☃"))
    row = snapshots.build_snapshot_rows(
        {"/api/repositories": payload}, generated, "a" * 64)[0]
    assert row["schemaVersion"] == 1
    assert row["kind"] == snapshots.SNAPSHOT_KIND
    assert row["staleUntil"] == row["freshUntil"]
    assert row["contentLength"] == len(row["payload"].encode("utf-8"))
    assert "\\u2603" in row["payload"]


def test_mirror_snapshot_accepts_only_the_logical_working_copy_authority():
    now = 1_800_000_000_000
    payload = {
        "ok": True,
        "owner": "forkmesh",
        "repo": "forkmesh",
        "groupKey": "root:abc",
        "generatedAt": now,
        "sourceOfTruth": {
            "owner": "forkmesh",
            "repo": "forkmesh",
            "kind": "working-copy",
            "isSourceOfTruth": True,
            "referenceForMirrors": False,
        },
        "summary": {
            "mirrors": 0,
            "online": 0,
            "cloneAvailable": 0,
            "dataHostedBytes": 0,
            "longestHostedSince": None,
            "freshestSync": None,
        },
        "mirrors": [],
    }
    path = "/api/repo/forkmesh/forkmesh/mirrors"
    assert snapshots.validate_snapshot_payload(path, payload, now)
    payload["sourceOfTruth"]["owner"] = "mirror24"
    assert not snapshots.validate_snapshot_payload(path, payload, now)


def test_mirror_snapshot_accepts_the_availability_and_sync_status_verdicts():
    # Issues #492 and #494 added these two objects to the live mirrors
    # response. The allowlist did not know them, so from 2026-09-14 11:55
    # every republish failed as an unsafe payload and the error log turned
    # /status red. They are admitted only under their exact schema ids.
    now = 1_800_000_000_000
    payload = {
        "ok": True,
        "owner": "forkmesh",
        "repo": "forkmesh",
        "groupKey": "root:abc",
        "generatedAt": now,
        "sourceOfTruth": {
            "owner": "forkmesh",
            "repo": "forkmesh",
            "kind": "working-copy",
            "isSourceOfTruth": True,
        },
        "summary": {
            "mirrors": 0,
            "online": 0,
            "cloneAvailable": 0,
            "dataHostedBytes": 0,
            "longestHostedSince": None,
            "freshestSync": None,
        },
        "mirrors": [],
        "availability": {
            "schema": "forkmesh-availability-proof-v1",
            "proof": "partial",
            "score": 53,
        },
        "syncStatus": {
            "schema": "forkmesh-sync-status-v1",
            "state": "degraded",
        },
    }
    path = "/api/repo/forkmesh/forkmesh/mirrors"
    assert snapshots.validate_snapshot_payload(path, payload, now)
    without = {k: v for k, v in payload.items() if k not in ("availability", "syncStatus")}
    assert snapshots.validate_snapshot_payload(path, without, now)
    wrong = dict(payload, syncStatus={"schema": "forkmesh-sync-status-v2"})
    assert not snapshots.validate_snapshot_payload(path, wrong, now)
    not_object = dict(payload, availability=["forkmesh-availability-proof-v1"])
    assert not snapshots.validate_snapshot_payload(path, not_object, now)
    unknown = dict(payload, privateAccessPath="/secret")
    assert not snapshots.validate_snapshot_payload(path, unknown, now)


def test_mirror_snapshot_accepts_public_direct_https_verdict_fields():
    now = 1_800_000_000_000
    payload = {
        "ok": True,
        "owner": "forkmesh",
        "repo": "forkmesh",
        "groupKey": "root:abc",
        "generatedAt": now,
        "sourceOfTruth": {
            "owner": "forkmesh",
            "repo": "forkmesh",
            "kind": "working-copy",
            "isSourceOfTruth": True,
        },
        "summary": {
            "mirrors": 1,
            "online": 1,
            "cloneAvailable": 1,
            "dataHostedBytes": 1,
            "longestHostedSince": now - 1_000,
            "freshestSync": now - 100,
        },
        "mirrors": [{
            "node": "mirror35",
            "status": "online",
            "endpointFresh": True,
            "endpointHealthy": True,
            "checkedAt": now,
            "directVerified": True,
            "directReachable": True,
            "directIntegrity": "ok",
            "directRefsSha256": "a" * 64,
            "directCheckedAt": now,
            "directLatencyMs": 12,
            "directStatus": 200,
            "directReason": "verified",
        }],
    }
    path = "/api/repo/forkmesh/forkmesh/mirrors"
    assert snapshots.validate_snapshot_payload(path, payload, now)

    payload["mirrors"][0]["directSecret"] = "must fail closed"
    assert not snapshots.validate_snapshot_payload(path, payload, now)


def test_mirror_snapshot_accepts_a_declared_replica_scope():
    # mirrors.py forwards a filtered or metadata-only replica's declared
    # scope on its row. Without the key the whole projection fails closed and
    # the flagship snapshot keeps the previous deploy's fingerprint.
    now = 1_800_000_000_000
    payload = {
        "ok": True,
        "owner": "forkmesh",
        "repo": "forkmesh",
        "groupKey": "root:abc",
        "generatedAt": now,
        "sourceOfTruth": {
            "owner": "forkmesh",
            "repo": "forkmesh",
            "kind": "working-copy",
            "isSourceOfTruth": True,
        },
        "summary": {
            "mirrors": 1,
            "online": 1,
            "cloneAvailable": 1,
            "dataHostedBytes": 1,
            "longestHostedSince": now - 1_000,
            "freshestSync": now - 100,
        },
        "mirrors": [{
            "node": "mirror35",
            "status": "online",
            "endpointFresh": True,
            "endpointHealthy": True,
            "checkedAt": now,
            "replicaScope": "metadata-only",
        }],
    }
    path = "/api/repo/forkmesh/forkmesh/mirrors"
    assert snapshots.validate_snapshot_payload(path, payload, now)


def test_worker_mirrors_payload_annotations_are_snapshot_keys():
    from sync_status import mirrors_payload_status
    from availability_proof import evaluate_mirrors_payload

    payload = {"ok": True, "mirrors": [], "generatedAt": 1_800_000_000_000}
    for key, value in (
        ("syncStatus", mirrors_payload_status(payload)),
        ("availability", evaluate_mirrors_payload(payload)),
    ):
        assert (
            value["schema"] == snapshots.MIRROR_OPTIONAL_RESPONSE_SCHEMAS[key]
        )


def test_payload_caps_and_free_plan_scheduled_write_budget_are_pinned():
    assert set(snapshots.SNAPSHOT_PATHS) == {
        "/api/repositories",
        "/api/network/overview",
        "/api/repo/forkmesh/forkmesh/mirrors",
        "/api/status",
        "/api/status/world",
    }
    assert max(snapshots.SNAPSHOT_PATH_MAX_BYTES.values()) == 1536 * 1024
    entry = (APP / "src/entry.py").read_text()
    assert "PUBLIC_SNAPSHOT_FREQUENT_CADENCE_MINUTES = 3" in entry
    assert "PUBLIC_SNAPSHOT_SLOW_CADENCE_MINUTES = 5" in entry
    scheduled_rows_per_day = (
        len(snapshots.SNAPSHOT_FREQUENT_PATHS) * 24 * 60
        // snapshots.SNAPSHOT_FREQUENT_CADENCE_MINUTES
        + len(snapshots.SNAPSHOT_SLOW_PATHS) * 24 * 60
        // snapshots.SNAPSHOT_SLOW_CADENCE_MINUTES
    )
    assert scheduled_rows_per_day == 2016
    assert scheduled_rows_per_day < 100_000

    migration = (
        APP / "migrations/0125_public_api_snapshots.sql"
    ).read_text()
    assert "content_length <= 1572864" in migration
    assert "WITHOUT ROWID" in migration
    assert "stale_until = fresh_until" in migration


def test_world_projection_is_one_build_compact_and_safe_at_max_roster():
    now = 1_800_000_000_000
    systems = []
    for index in range(61):
        days = []
        for day_index in range(30):
            hours = []
            if day_index == 29:
                hours = [{
                    "hourTs": now - (23 - hour_index) * 3_600_000,
                    "checks": 60,
                    "failures": 60,
                    "expectedChecks": 60,
                    "missingChecks": 0,
                    "status": "down",
                    "reason": "broad incident " + "x" * 220,
                } for hour_index in range(24)]
            days.append({
                "dayTs": now - (29 - day_index) * 86_400_000,
                "checks": 1440,
                "failures": 1440,
                "expectedChecks": 1440,
                "missingChecks": 0,
                "uptimePct": 0.0,
                "coveragePct": 100.0,
                "hours": hours,
                "hoursElapsed": 24,
                "hoursCompacted": day_index < 29,
            })
        systems.append({
            "id": "website" if index == 0 else f"mirror:mirror{index}",
            "label": "Mirror node — " + "m" * 50,
            "status": "down",
            "checkDescription": "verbose full-only check " + "x" * 900,
            "uptimePct": 0.0,
            "uptime24hPct": 0.0,
            "coveragePct": 100.0,
            "coverage24hPct": 100.0,
            "checks24h": 1440,
            "failures24h": 1440,
            "missing24h": 0,
            "days": days,
            "minutes": [{
                "minuteTs": now - (59 - minute_index) * 60_000,
                "status": "down",
                "reason": "broad incident " + "x" * 220,
            } for minute_index in range(60)],
            "reason": "broad incident " + "x" * 220,
            "reasonTs": now,
        })
    full = {
        "ok": True,
        "now": now,
        "systems": systems,
        "current": {"lastCronSampleTs": now},
    }
    projected = snapshots.project_world_status_payload(full)
    assert snapshots.validate_snapshot_payload(
        "/api/status/world", projected, now)
    body = json.dumps(projected, separators=(",", ":"), ensure_ascii=True)
    assert len(body.encode("utf-8")) < snapshots.SNAPSHOT_PATH_MAX_BYTES[
        "/api/status/world"]
    assert all(
        set(day) == {"dayTs", "checks", "failures"}
        for system in projected["systems"] for day in system["days"]
    )
    assert all(
        set(hour) == {"hourTs", "status"}
        for system in projected["systems"] for hour in system["hours"]
    )
    assert all(
        set(minute) == {"minuteTs", "status"}
        for system in projected["systems"] for minute in system["minutes"]
    )
    rows = snapshots.build_snapshot_rows(
        {"/api/status/world": projected}, now, "a" * 64)
    assert rows[0]["path"] == "/api/status/world"


def test_main_database_owns_snapshot_schema_and_python_access():
    migration = (
        APP / "migrations/0125_public_api_snapshots.sql"
    ).read_text()
    schema = (APP / "src/schema.py").read_text()
    entry = (APP / "src/entry.py").read_text()

    assert "CREATE TABLE IF NOT EXISTS public_api_snapshots" in migration
    assert "CREATE TABLE IF NOT EXISTS public_api_snapshots" in schema
    assert 'database = getattr(env, "DB", None)' in entry
    assert "PUBLIC_SNAPSHOTS" not in entry


def test_snapshot_upsert_is_monotonic_and_tombstone_guarded():
    entry = (APP / "src/entry.py").read_text()
    sql = re.search(
        r"PUBLIC_SNAPSHOT_UPSERT_SQL = \((.*?)\n\)", entry, re.S
    ).group(1)
    assert "generated_at<=excluded.generated_at" in sql
    assert "snapshot-tombstone" in sql
    assert "database.batch(statements)" in entry
    assert "database.batch(to_js(statements))" not in entry
    assert "blocked_until = ((now // bucket_ms) + 2) * bucket_ms" in entry
    tombstone_sql = re.search(
        r"PUBLIC_SNAPSHOT_TOMBSTONE_UPSERT_SQL = \((.*?)\n\)", entry, re.S
    ).group(1)
    assert "stale_until<excluded.stale_until" in tombstone_sql


def _entry_sql(name):
    tree = ast.parse((APP / "src/entry.py").read_text())
    for node in tree.body:
        if (
            isinstance(node, ast.Assign)
            and any(isinstance(target, ast.Name) and target.id == name
                    for target in node.targets)
        ):
            return ast.literal_eval(node.value)
    raise AssertionError("missing SQL constant " + name)


def test_sqlite_tombstone_coalescing_monotonic_publish_and_absent_cleanup():
    connection = sqlite3.connect(":memory:")
    connection.execute(
        "CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL)"
    )
    connection.executescript(
        (APP / "migrations/0125_public_api_snapshots.sql").read_text()
    )
    normal = _entry_sql("PUBLIC_SNAPSHOT_UPSERT_SQL")
    tombstone = _entry_sql("PUBLIC_SNAPSHOT_TOMBSTONE_UPSERT_SQL")
    absent = _entry_sql("PUBLIC_SNAPSHOT_DELETE_ABSENT_SQL")
    path = "/api/repositories"

    def values(kind, generated, deadline, payload="{}"):
        return (
            path, 1, kind, generated, deadline, deadline, "a" * 64,
            len(payload.encode()), payload,
        )

    connection.execute(normal, values(snapshots.SNAPSHOT_KIND, 1000, 2000))
    connection.execute(
        tombstone,
        values(snapshots.SNAPSHOT_TOMBSTONE_KIND, 3000, 120_000),
    )
    assert connection.execute("SELECT changes()").fetchone()[0] == 1
    # A repeated transition in the same deadline bucket changes zero rows.
    connection.execute(
        tombstone,
        values(snapshots.SNAPSHOT_TOMBSTONE_KIND, 4000, 120_000),
    )
    assert connection.execute("SELECT changes()").fetchone()[0] == 0
    assert connection.execute(
        "SELECT generated_at FROM public_api_snapshots WHERE path=?", (path,)
    ).fetchone()[0] == 3000

    # A later bucket extends the race barrier, and an older in-flight normal
    # generation cannot overwrite it.
    connection.execute(
        tombstone,
        values(snapshots.SNAPSHOT_TOMBSTONE_KIND, 5000, 240_000),
    )
    assert connection.execute("SELECT changes()").fetchone()[0] == 1
    connection.execute(normal, values(snapshots.SNAPSHOT_KIND, 4500, 250_000))
    assert connection.execute("SELECT changes()").fetchone()[0] == 0
    assert connection.execute(
        "SELECT kind FROM public_api_snapshots WHERE path=?", (path,)
    ).fetchone()[0] == snapshots.SNAPSHOT_TOMBSTONE_KIND

    # Expected handler absence cannot clear an active barrier, but it does
    # remove it once the captured generation is beyond the deadline.
    connection.execute(absent, (path, 239_999))
    assert connection.execute("SELECT changes()").fetchone()[0] == 0
    connection.execute(absent, (path, 240_000))
    assert connection.execute("SELECT changes()").fetchone()[0] == 1
    assert connection.execute(
        "SELECT COUNT(*) FROM public_api_snapshots"
    ).fetchone()[0] == 0
