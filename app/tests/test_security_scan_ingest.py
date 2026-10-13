#!/usr/bin/env python3
"""End-to-end contract tests for authenticated repository scan ingest."""

import ast
import asyncio
import base64
import copy
from datetime import datetime, timedelta, timezone
import importlib.util
import json
from pathlib import Path
import sqlite3
from urllib.parse import parse_qs, urlparse

import pytest

from worker_test_helpers import json_from_request_double


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
MODULE_PATH = ROOT / "src" / "security_scan_ingest.py"
MIGRATION = ROOT / "migrations" / "0045_repository_security_scans.sql"
REVIEW_MIGRATION = ROOT / "migrations" / "0054_security_scan_reviews.sql"

spec = importlib.util.spec_from_file_location(
    "forkmesh_security_scan_ingest", MODULE_PATH)
ingest = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ingest)


def _iso(offset=0):
    value = datetime(
        2026, 7, 23, 3, 41, tzinfo=timezone.utc
    ) + timedelta(minutes=offset)
    return value.isoformat().replace("+00:00", "Z")


def _envelope(repository="alice/widget", offset=0):
    completed = _iso(offset)
    rich = {
        "schemaVersion": 1,
        "visibility": {
            "public": True,
            "redacted": True,
            "containsSourceExcerpts": False,
            "containsSecretValues": False,
        },
        "generatedAt": completed,
        "repository": {"name": repository, "commit": "a" * 40},
        "scanner": {
            "name": "forkmesh-public-scan",
            "version": "1.0",
            "model": {"used": False, "name": None, "version": None},
        },
        "policy": {
            "id": "forkmesh-daily-public-safe",
            "version": "1",
            "path": "docs/security-scan-policy.json",
            "sha256": "b" * 64,
        },
        "scan": {
            "startedAt": completed,
            "completedAt": completed,
            "durationMs": 20,
            "completion": "completed",
            "areasIncluded": ["static checks"],
            "areasExcluded": ["private diagnostics"],
            "filesInspected": 1,
            "filesExcluded": 0,
            "dependencyInventoryCount": 0,
            "dependencyAdvisorySource": "OSV",
            "scopeLimitations": [],
        },
        "status": "Review required",
        "summary": {
            "total": 1,
            "bySeverity": {
                "critical": 0,
                "high": 0,
                "medium": 0,
                "low": 1,
                "info": 0,
            },
            "byCategory": {
                "dependency": 0,
                "secret": 0,
                "static": 1,
            },
        },
        "findings": [{
            "id": "c" * 16,
            "category": "static",
            "severity": "low",
            "ruleId": "review-rule",
            "path": "src/private-module.py",
            "line": 7,
            "summary": "A controlled public summary.",
            "recommendation": "Review this code path.",
            "evidence": {"redacted": True, "fingerprint": "c" * 16},
            "falsePositiveStatus": "unreviewed",
        }],
        "recommendations": ["Review this code path."],
        "notices": [
            "Automated scans may miss vulnerabilities.",
            "Results apply only to the scanned commit.",
            "A clean scan is not a guarantee of security.",
        ],
    }
    return {
        "schemaVersion": 1,
        "type": "forkmesh.security-scan-ingest",
        "rich": rich,
        "clipboard": ingest.clipboard_from_rich(rich),
    }


def test_runtime_validator_accepts_documented_envelope_and_is_detached():
    original = _envelope()
    validated = ingest.validate_ingest_envelope(
        original, expected_repository="ALICE/WIDGET")
    assert validated == original
    assert validated is not original
    original["rich"]["repository"]["name"] = "mallory/rebound"
    assert validated["rich"]["repository"]["name"] == "alice/widget"
    assert len(ingest.envelope_scan_id(validated)) == 64


@pytest.mark.parametrize(
    "mutate",
    [
        lambda value: value.update({"privateDiagnostic": "secret"}),
        lambda value: value["rich"]["visibility"].update({"redacted": False}),
        lambda value: value["rich"]["findings"][0]["evidence"].update(
            {"excerpt": "private source"}),
        lambda value: value["rich"]["findings"][0].update(
            {"path": "../private/source.py"}),
        lambda value: value["clipboard"]["findings"].update({"low": 99}),
        lambda value: value["rich"]["summary"].update({"total": 2}),
    ],
)
def test_runtime_validator_rejects_extra_sensitive_or_inconsistent_data(mutate):
    value = _envelope()
    mutate(value)
    with pytest.raises(ingest.ScanEnvelopeError):
        ingest.validate_ingest_envelope(
            value, expected_repository="alice/widget")


def test_runtime_validator_binds_body_to_route_repository():
    with pytest.raises(ingest.ScanEnvelopeError):
        ingest.validate_ingest_envelope(
            _envelope("alice/other"), expected_repository="alice/widget")


def test_runtime_validator_requires_human_statuses_to_start_unreviewed():
    value = _envelope()
    value["rich"]["findings"][0]["falsePositiveStatus"] = "dismissed"
    value["clipboard"] = ingest.clipboard_from_rich(value["rich"])
    with pytest.raises(ingest.ScanEnvelopeError):
        ingest.validate_ingest_envelope(
            value, expected_repository="alice/widget")


def test_documented_json_schemas_accept_the_runtime_fixture():
    jsonschema = pytest.importorskip("jsonschema")
    value = _envelope()
    rich_schema = json.loads(
        (ROOT.parent / "docs" / "security-scan.schema.json").read_text())
    clipboard_schema = json.loads(
        (ROOT.parent / "docs" / "security-clipboard.schema.json").read_text())
    jsonschema.validate(value["rich"], rich_schema)
    jsonschema.validate(value["clipboard"], clipboard_schema)

    ingest_schema = json.loads(
        (ROOT.parent / "docs" / "security-ingest.schema.json").read_text())
    resolver = jsonschema.RefResolver(
        base_uri=(ROOT.parent / "docs").resolve().as_uri() + "/",
        referrer=ingest_schema,
        store={
            "https://forkmesh.com/schemas/security-scan-v1.json":
                rich_schema,
            "https://forkmesh.com/schemas/security-clipboard-v1.json":
                clipboard_schema,
        },
    )
    jsonschema.validate(value, ingest_schema, resolver=resolver)
    reviewed = copy.deepcopy(value)
    reviewed["rich"]["findings"][0]["falsePositiveStatus"] = "dismissed"
    reviewed["clipboard"] = ingest.clipboard_from_rich(reviewed["rich"])
    with pytest.raises(jsonschema.ValidationError):
        jsonschema.validate(reviewed, ingest_schema, resolver=resolver)


def test_bearer_secret_comparison_is_fail_closed_and_fixed_digest_length():
    secret = "w" * 48
    assert ingest.bearer_secret_matches("Bearer " + secret, secret)
    assert ingest.bearer_secret_matches("bearer " + secret, secret)
    assert not ingest.bearer_secret_matches("Basic " + secret, secret)
    assert not ingest.bearer_secret_matches("Bearer wrong", secret)
    assert not ingest.bearer_secret_matches("Bearer short", "short")
    assert ingest.distinct_bearer_secrets("a" * 48, "b" * 48)
    assert not ingest.distinct_bearer_secrets("a" * 48, "a" * 48)
    assert not ingest.distinct_bearer_secrets("short", "b" * 48)
    assert "print(" not in MODULE_PATH.read_text(encoding="utf-8")


def _database():
    connection = sqlite3.connect(":memory:")
    connection.row_factory = sqlite3.Row
    connection.executescript(MIGRATION.read_text(encoding="utf-8"))
    connection.executescript(REVIEW_MIGRATION.read_text(encoding="utf-8"))
    return connection


async def _store_fixture(connection, value, received_at):
    async def run(sql, *args):
        connection.execute(sql, args)
        connection.commit()

    async def encrypt(envelope):
        encoded = ingest.canonical_json(envelope).encode()
        return "sealed:" + base64.urlsafe_b64encode(encoded).decode()

    return await ingest.store_scan(
        run, encrypt, "repo:alice/widget", value, received_at)


async def _load_fixture(connection, limit=10):
    async def all_rows(sql, *args):
        return [
            dict(row) for row in connection.execute(sql, args).fetchall()
        ]

    async def decrypt(value):
        if not value.startswith("sealed:"):
            raise ValueError("corrupt")
        return json.loads(
            base64.urlsafe_b64decode(value[7:].encode()).decode())

    return await ingest.load_scans(
        all_rows,
        decrypt,
        "repo:alice/widget",
        "alice/widget",
        limit,
    )


def test_store_is_encrypted_idempotent_and_history_is_bounded():
    async def scenario():
        connection = _database()
        first = _envelope(offset=0)
        first_id = await _store_fixture(connection, first, 1)
        duplicate_id = await _store_fixture(connection, first, 2)
        assert duplicate_id == first_id
        assert connection.execute(
            "SELECT COUNT(*) FROM repo_security_scans"
        ).fetchone()[0] == 1

        stored = connection.execute(
            "SELECT received_at, data FROM repo_security_scans"
        ).fetchone()
        assert stored["received_at"] == 1
        stored = stored["data"]
        assert stored.startswith("sealed:")
        assert "alice/widget" not in stored
        assert "private-module.py" not in stored

        for index in range(1, 96):
            await _store_fixture(
                connection, _envelope(offset=index), index + 2)
        assert connection.execute(
            "SELECT COUNT(*) FROM repo_security_scans"
        ).fetchone()[0] == ingest.MAX_HISTORY_PER_REPOSITORY

        records = await _load_fixture(connection, limit=999)
        assert len(records) == ingest.MAX_HISTORY_RESPONSE
        assert records[0]["envelope"]["rich"]["scan"]["completedAt"] == _iso(95)
        assert "rich" not in ingest.scan_projection(records[0])
        assert "rich" in ingest.scan_projection(records[0], include_rich=True)

    asyncio.run(scenario())


def test_false_positive_review_overlay_is_aggregated_without_mutating_scan():
    envelope = ingest.validate_ingest_envelope(_envelope())
    record = {
        "scanId": ingest.envelope_scan_id(envelope),
        "receivedAt": _iso(),
        "envelope": envelope,
    }
    original = copy.deepcopy(record)
    reviewed = ingest.apply_review_overlays(record, [{
        "scanId": record["scanId"],
        "findingId": "c" * 16,
        "status": "dismissed",
        "reviewedAt": 1_800_000_000_000,
    }])
    assert record == original
    assert reviewed["envelope"]["rich"]["findings"][0][
        "falsePositiveStatus"] == "dismissed"
    clipboard = reviewed["envelope"]["clipboard"]
    assert clipboard["falsePositiveStatus"] == {
        "unreviewed": 0,
        "confirmed": 0,
        "dismissed": 1,
    }
    assert "1 reviewed, 0 unreviewed" in clipboard["reviewStatus"]


def _load_entry_handlers(namespace):
    namespace.setdefault("bounded_json_request", json_from_request_double)
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    wanted = {
        "_security_scan_owner_authorization",
        "_security_scan_repository_row",
        "_security_scan_review_overlays",
        "_security_scan_reviewer_authorization",
        "repository_security_scans_handler",
    }
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in wanted
    ]
    assert {node.name for node in selected} == wanted
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace["repository_security_scans_handler"]


class _Date:
    current = 1_800_000_000_000

    @classmethod
    def now(cls):
        cls.current += 1
        return cls.current


class _Request:
    def __init__(self, method, url, *, headers=None, body="", actor=""):
        self.method = method
        self.url = url
        self.headers = {str(k).lower(): str(v) for k, v in (headers or {}).items()}
        self.body = body
        self.text_calls = 0
        self.owner_signature = False
        self.owner_session = False
        self.actor = actor

    async def text(self):
        self.text_calls += 1
        return self.body

    async def json(self):
        return json.loads(self.body)


class _Env:
    def __init__(self):
        self.repos = {
            "repo:alice/widget": {"is_private": 0},
            "repo:alice/private": {"is_private": 1},
        }


def _handler_harness():
    connection = _database()
    audits = []

    async def d1_run(_env, sql, *args):
        connection.execute(sql, args)
        connection.commit()

    async def d1_all(_env, sql, *args):
        return [dict(row) for row in connection.execute(sql, args).fetchall()]

    async def d1_first(env, sql, *args):
        if "FROM repositories" in sql:
            return env.repos.get(args[0])
        row = connection.execute(sql, args).fetchone()
        return dict(row) if row else None

    async def encrypt_row(_env, value):
        return "sealed:" + base64.urlsafe_b64encode(
            ingest.canonical_json(value).encode()).decode()

    async def decrypt_row(_env, value):
        return json.loads(
            base64.urlsafe_b64decode(value[7:].encode()).decode())

    async def authorize_owner(_env, request, _owner):
        return request.owner_signature

    async def authorize_account(_env, _owner, _data, request=None,
                                allow_admin=True):
        assert allow_admin is False
        return (
            (True, None) if request.owner_session
            else (False, {"status": 403})
        )

    async def audit(_env, actor, action, target_type, target, outcome, details):
        audits.append({
            "actor": actor,
            "action": action,
            "targetType": target_type,
            "target": target,
            "outcome": outcome,
            "details": details,
        })

    async def blind_index(_env, value):
        return "repo:" + value

    async def ensure_schema(_env):
        return None

    async def repo_is_private(env, owner, repo):
        row = env.repos.get("repo:" + owner + "/" + repo)
        return not row or int(row.get("is_private", 1) or 0) != 0

    async def authed_name(_env, request, data=None):
        if request.actor:
            return request.actor
        return "alice" if request.owner_session else ""

    async def has_role(_env, actor, role, scope_type="platform", scope=""):
        return actor == "reviewer" and role == "security_reviewer"

    async def lease_authorization(_env, _request, _repo_bi):
        return None

    def json_response(data, status=200, cache_control=None, extra_headers=None,
                      **_kwargs):
        return {
            "status": status,
            "data": data,
            "cacheControl": cache_control,
            "headers": extra_headers or {},
        }

    namespace = {
        "Date": _Date,
        "json": json,
        "parse_qs": parse_qs,
        "urlparse": urlparse,
        "security_scan_ingest": ingest,
        "method_name": lambda request: request.method,
        "json_response": json_response,
        "_authorize_owner": authorize_owner,
        "_authorize_owner_account": authorize_account,
        "_audit_sensitive_action": audit,
        "blind_index": blind_index,
        "d1_run": d1_run,
        "d1_all": d1_all,
        "d1_first": d1_first,
        "encrypt_row": encrypt_row,
        "decrypt_row": decrypt_row,
        "ensure_schema": ensure_schema,
        "_repo_is_private": repo_is_private,
        "_authed_account_name": authed_name,
        "_has_role": has_role,
        "_security_scan_lease_authorization": lease_authorization,
        "clean_string": lambda value, limit: str(value or "")[:limit],
    }
    return _load_entry_handlers(namespace), connection, audits


def _run(value):
    return asyncio.run(value)


def test_handler_authenticates_before_body_and_ingests_without_audit_details():
    handler, connection, audits = _handler_harness()
    env = _Env()
    body = json.dumps(_envelope())
    url = "https://forkmesh.test/api/repo/alice/widget/security-scans/ingest"

    denied = _Request(
        "POST", url,
        headers={"authorization": "Bearer wrong", "content-type": "application/json"},
        body=body,
    )
    response = _run(handler(env, denied, "alice", "widget", "ingest"))
    assert response["status"] == 401
    assert denied.text_calls == 0

    insecure = _Request(
        "POST",
        "http://forkmesh.test/api/repo/alice/widget/security-scans/ingest",
        headers={"content-type": "application/json"},
        body=body,
    )
    insecure.owner_session = True
    response = _run(handler(env, insecure, "alice", "widget", "ingest"))
    assert response["status"] == 400
    assert response["data"] == {"error": "https_required"}
    assert insecure.text_calls == 0

    oversized = _Request(
        "POST", url,
        headers={
            "content-type": "application/json",
            "content-length": str(ingest.MAX_INGEST_BYTES + 1),
        },
        body=body,
    )
    oversized.owner_session = True
    response = _run(handler(env, oversized, "alice", "widget", "ingest"))
    assert response["status"] == 413
    assert oversized.text_calls == 0

    accepted = _Request(
        "POST", url,
        headers={"content-type": "application/json; charset=utf-8"},
        body=body,
    )
    accepted.owner_session = True
    response = _run(handler(env, accepted, "alice", "widget", "ingest"))
    assert response["status"] == 202
    assert accepted.text_calls == 1
    assert connection.execute(
        "SELECT COUNT(*) FROM repo_security_scans"
    ).fetchone()[0] == 1
    assert audits == [{
        "actor": "alice",
        "action": "repository.security_scan_ingest",
        "targetType": "repository",
        "target": "alice/widget",
        "outcome": "success",
        "details": {
            "status": "Review required",
            "authMode": "owner_session",
            "historyLimit": 90,
        },
    }]
    serialized_audit = json.dumps(audits)
    assert "private-module.py" not in serialized_audit
    assert "authorization" not in serialized_audit.lower()


def test_handler_public_projection_and_private_non_discoverability():
    handler, _connection, _audits = _handler_harness()
    env = _Env()

    public_ingest = _Request(
        "POST",
        "https://forkmesh.test/api/repo/alice/widget/security-scans/ingest",
        headers={"content-type": "application/json"},
        body=json.dumps(_envelope("alice/widget")),
    )
    public_ingest.owner_session = True
    assert _run(
        handler(env, public_ingest, "alice", "widget", "ingest")
    )["status"] == 202

    private_anonymous_ingest = _Request(
        "POST",
        "https://forkmesh.test/api/repo/alice/private/security-scans/ingest",
        headers={"content-type": "application/json"},
        body=json.dumps(_envelope("alice/private")),
    )
    assert _run(
        handler(
            env, private_anonymous_ingest, "alice", "private", "ingest")
    )["status"] == 401
    assert private_anonymous_ingest.text_calls == 0

    private_owner_ingest = _Request(
        "POST",
        "https://forkmesh.test/api/repo/alice/private/security-scans/ingest",
        headers={"content-type": "application/json"},
        body=json.dumps(_envelope("alice/private")),
    )
    private_owner_ingest.owner_session = True
    assert _run(
        handler(env, private_owner_ingest, "alice", "private", "ingest")
    )["status"] == 202

    public = _Request(
        "GET",
        "https://forkmesh.test/api/repo/alice/widget/security-scans/latest",
    )
    response = _run(handler(env, public, "alice", "widget", "latest"))
    assert response["status"] == 200
    assert "clipboard" in response["data"]["latest"]
    assert "rich" not in response["data"]["latest"]

    rich_public = _Request(
        "GET",
        "https://forkmesh.test/api/repo/alice/widget/security-scans/latest"
        "?detail=rich",
    )
    assert _run(
        handler(env, rich_public, "alice", "widget", "latest")
    )["status"] == 403

    private = _Request(
        "GET",
        "https://forkmesh.test/api/repo/alice/private/security-scans/latest",
    )
    missing = _Request(
        "GET",
        "https://forkmesh.test/api/repo/alice/missing/security-scans/latest",
    )
    hidden = _run(handler(env, private, "alice", "private", "latest"))
    absent = _run(handler(env, missing, "alice", "missing", "latest"))
    assert hidden == absent == {
        "status": 404,
        "data": {"error": "not_found"},
        "cacheControl": "no-store",
        "headers": {},
    }

    authorized = _Request(
        "GET",
        "https://forkmesh.test/api/repo/alice/private/security-scans/history"
        "?detail=rich&limit=999",
    )
    authorized.owner_session = True
    response = _run(
        handler(env, authorized, "alice", "private", "history"))
    assert response["status"] == 200
    assert response["data"]["visibility"] == "owner_redacted"
    assert response["data"]["count"] == 1
    assert "rich" in response["data"]["history"][0]


def test_owner_or_security_reviewer_can_triage_and_public_sees_counts():
    handler, connection, audits = _handler_harness()
    env = _Env()
    ingest_request = _Request(
        "POST",
        "https://forkmesh.test/api/repo/alice/widget/security-scans/ingest",
        headers={"content-type": "application/json"},
        body=json.dumps(_envelope()),
    )
    ingest_request.owner_session = True
    accepted = _run(handler(
        env, ingest_request, "alice", "widget", "ingest"))
    scan_id = accepted["data"]["scanId"]

    denied = _Request(
        "GET",
        "https://forkmesh.test/api/repo/alice/widget/security-scans/triage",
        actor="bob",
    )
    assert _run(handler(
        env, denied, "alice", "widget", "triage"))["status"] == 403

    reviewer = _Request(
        "GET",
        "https://forkmesh.test/api/repo/alice/widget/security-scans/triage",
        actor="reviewer",
    )
    review_view = _run(handler(
        env, reviewer, "alice", "widget", "triage"))
    assert review_view["status"] == 200
    assert review_view["data"]["reviewRole"] == "security_reviewer"
    assert review_view["data"]["findings"][0][
        "falsePositiveStatus"] == "unreviewed"

    patch = _Request(
        "PATCH",
        "https://forkmesh.test/api/repo/alice/widget/security-scans/triage",
        actor="reviewer",
        body=json.dumps({
            "scanId": scan_id,
            "findingId": "c" * 16,
            "status": "dismissed",
            "note": "Verified in the authorized review context.",
        }),
    )
    updated = _run(handler(
        env, patch, "alice", "widget", "triage"))
    assert updated["status"] == 200
    assert updated["data"]["falsePositiveStatus"] == {
        "unreviewed": 0,
        "confirmed": 0,
        "dismissed": 1,
    }
    stored = connection.execute(
        "SELECT data FROM repo_security_scan_reviews"
    ).fetchone()[0]
    assert stored.startswith("sealed:")
    assert "Verified in the authorized" not in stored

    public = _Request(
        "GET",
        "https://forkmesh.test/api/repo/alice/widget/security-scans/latest",
    )
    public_view = _run(handler(
        env, public, "alice", "widget", "latest"))
    assert public_view["data"]["latest"]["clipboard"][
        "falsePositiveStatus"]["dismissed"] == 1
    assert any(
        item["action"] == "repository.security_scan_triage"
        and item["outcome"] == "success"
        for item in audits
    )


def test_schema_route_cleanup_and_admin_boundaries_are_wired():
    entry = ENTRY.read_text(encoding="utf-8")
    runtime = ENTRY.with_name("app_runtime.py").read_text(encoding="utf-8")
    admin_console = (ROOT / "src" / "admin_console.py").read_text(
        encoding="utf-8")
    schema = (ROOT / "src" / "schema.py").read_text(encoding="utf-8")
    urls = (ROOT / "src" / "urls.py").read_text(encoding="utf-8")
    migration = MIGRATION.read_text(encoding="utf-8")
    for source in (schema, migration):
        assert "CREATE TABLE IF NOT EXISTS repo_security_scans" in source
        assert "idx_repo_security_scans_latest" in source
        table = source.split(
            "CREATE TABLE IF NOT EXISTS repo_security_scans", 1
        )[1].split(")", 1)[0]
        assert "commit_hash" not in table
        assert "status TEXT" not in table
    assert "REPO_SECURITY_SCANS_RE" in urls
    assert "REPO_SECURITY_SCANS_RE.match(url.path)" in runtime
    handler = entry.split(
        "async def repository_security_scans_handler", 1
    )[1].split("\ndef _admin_path", 1)[0]
    assert "is_private = await _repo_is_private(env, owner, repo)" in handler
    assert '"DELETE FROM repo_security_scans WHERE repo_bi=?"' in entry
    assert "CREATE TABLE IF NOT EXISTS repo_security_scan_reviews" in (
        schema + REVIEW_MIGRATION.read_text(encoding="utf-8"))
    assert '"DELETE FROM repo_security_scan_reviews WHERE repo_bi=?"' in entry
    hidden = admin_console.split(
        "ADMIN_HIDDEN_TABLES = (", 1)[1].split(")", 1)[0]
    assert '"repo_security_scans"' in hidden
    assert '"repo_security_scan_reviews"' in hidden
