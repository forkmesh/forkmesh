#!/usr/bin/env python3
"""End-to-end fixture evidence for issue #477 repository migration."""

import asyncio
import hashlib
import hmac
import json
import sqlite3
import sys
from copy import deepcopy
from pathlib import Path
from urllib.parse import parse_qs, urlparse


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import repository_imports as imports  # noqa: E402
import repository_migration as migration  # noqa: E402

TOOLS = ROOT.parent / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import verify_migration_report as report_verifier  # noqa: E402

REPORT_SEED = hashlib.sha256(b"fixture-report-signing-seed").digest()


def ed25519_sign(seed, message):
    """RFC 8032 signing on the verifier's own curve arithmetic (tests only)."""
    def encode(point):
        x, y = point
        return (y | ((x & 1) << 255)).to_bytes(32, "little")

    digest = hashlib.sha512(seed).digest()
    scalar = int.from_bytes(digest[:32], "little")
    scalar &= (1 << 254) - 8
    scalar |= 1 << 254
    public = encode(report_verifier._mult(report_verifier._BASE, scalar))
    nonce = int.from_bytes(
        hashlib.sha512(digest[32:] + message).digest(), "little") % report_verifier._L
    point = encode(report_verifier._mult(report_verifier._BASE, nonce))
    challenge = int.from_bytes(
        hashlib.sha512(point + public + message).digest(), "little") % report_verifier._L
    signature = point + ((nonce + challenge * scalar) % report_verifier._L).to_bytes(32, "little")
    return public, signature


def b64url(raw):
    import base64
    return base64.urlsafe_b64encode(raw).decode().rstrip("=")


REPORT_PUBLIC_KEY = b64url(ed25519_sign(REPORT_SEED, b"")[0])


def run(awaitable):
    return asyncio.run(awaitable)


class Request:
    def __init__(self, method="GET", body=None, actor="alice", path=""):
        self.method = method
        self._body = deepcopy(body)
        self.actor = actor
        self.url = "https://forkmesh.test" + (
            path or "/api/repository-imports/migrations")

    async def json(self):
        if self._body is None:
            raise ValueError("no body")
        return deepcopy(self._body)


class ProviderFixture:
    """Three provider-shaped repositories containing every supported family."""

    def __init__(self, provider="github", repository_number=101):
        self.provider = provider
        self.repository_number = repository_number
        self.calls = []
        self.rate_limit_once = ""
        self.removed_streams = set()

    def source_url(self):
        if self.provider == "gitlab":
            return "https://gitlab.com/acme/project-%d" % self.repository_number
        if self.provider == "codeberg":
            return "https://codeberg.org/acme/project-%d" % self.repository_number
        return "https://github.com/acme/project-%d" % self.repository_number

    def root(self):
        common = {
            "id": self.repository_number,
            "default_branch": "main",
            "description": "Fixture with every collaboration object",
            "created_at": "2024-01-01T00:00:00Z",
            "archived": False,
        }
        if self.provider == "gitlab":
            return {
                **common,
                "path_with_namespace": "acme/project-%d" % self.repository_number,
                "web_url": self.source_url(),
                "visibility": "public",
                "last_activity_at": "2026-08-20T10:00:00Z",
                "namespace": {"full_path": "acme", "kind": "group"},
            }
        return {
            **common,
            "full_name": "acme/project-%d" % self.repository_number,
            "html_url": self.source_url(),
            "private": False,
            "updated_at": "2026-08-20T10:00:00Z",
            "owner": {"id": 7, "login": "acme", "type": "Organization"},
        }

    @staticmethod
    def _item(identifier, **values):
        return {
            "id": identifier,
            "title": values.pop("title", "Fixture object"),
            "body": values.pop("body", "Body preserved exactly.\nSecond line."),
            "state": values.pop("state", "open"),
            "created_at": "2025-01-02T03:04:05Z",
            "updated_at": "2026-08-20T11:12:13Z",
            "user": {"id": 9, "login": "maintainer"},
            **values,
        }

    def _github(self, path):
        if "/branches" in path:
            return [{"name": "main", "protected": True,
                     "commit": {"sha": "a" * 40}}]
        if "/tags" in path:
            return [{"name": "v1.0.0", "commit": {"sha": "b" * 40}}]
        if "/issues/comments" in path:
            return [self._item(801, issue_url="/issues/11")]
        if "/issues/events" in path:
            return [self._item(802, event="labeled", issue={"number": 11})]
        if "/issues?" in path:
            return [self._item(201, number=11, labels=[{"id": 401, "name": "bug"}])]
        if "/pulls/comments" in path:
            return [self._item(601, pull_request_review_id=501, path="src/a.py")]
        if "/pulls/22/reviews" in path:
            return [self._item(501, state="APPROVED", commit_id="c" * 40)]
        if "/pulls?" in path:
            return [self._item(301, number=22, head={"sha": "c" * 40})]
        if "/releases" in path:
            return [self._item(701, tag_name="v1.0.0", assets=[{
                "id": 702, "name": "fixture.tar.gz", "size": 123,
                "browser_download_url": "https://downloads.test/fixture",
            }])]
        if "/readme" in path:
            return {"path": "README.md", "sha": "d" * 40,
                    "encoding": "base64", "content": "IyBGaXh0dXJlCg=="}
        if "/labels" in path:
            return [self._item(401, name="bug", color="ff0000")]
        if "/contributors" in path:
            return [{"id": 9, "login": "maintainer", "contributions": 12,
                     "created_at": "2020-01-01T00:00:00Z"}]
        if path.split("?", 1)[0].endswith("/comments"):
            return [self._item(803, commit_id="d" * 40)]
        raise AssertionError("unhandled GitHub path " + path)

    def _gitlab(self, path):
        if "/repository/branches" in path:
            return [{"name": "main", "protected": True,
                     "commit": {"id": "a" * 40}}]
        if "/repository/tags" in path:
            return [{"name": "v1.0.0", "commit": {"id": "b" * 40}}]
        if "/issues?" in path:
            return [self._item(201, iid=11, labels=["bug"])]
        if "/merge_requests/22/discussions" in path:
            return [self._item("discussion-501", notes=[self._item(501)])]
        if "/merge_requests/22/notes" in path:
            return [self._item(801, noteable_iid=22)]
        if "/issues/11/notes" in path:
            return [self._item(803, noteable_iid=11)]
        if "/merge_requests?" in path:
            return [self._item(301, iid=22, sha="c" * 40)]
        if "/releases" in path:
            return [self._item("v1.0.0", tag_name="v1.0.0", assets={
                "links": [{"id": 702, "name": "fixture", "url": "/asset"}],
            })]
        if "/repository/files/README.md" in path:
            return {"file_path": "README.md", "blob_id": "d" * 40,
                    "encoding": "base64", "content": "IyBGaXh0dXJlCg=="}
        if "/labels" in path:
            return [self._item(401, name="bug", color="#ff0000")]
        if "/repository/contributors" in path:
            return [{"id": 9, "name": "Maintainer", "username": "maintainer",
                     "commits": 12}]
        if "/events" in path:
            return [self._item(802, action_name="pushed to", target_id=11)]
        raise AssertionError("unhandled GitLab path " + path)

    def _codeberg(self, path):
        if "/branches" in path:
            return [{"name": "main", "protected": True,
                     "commit": {"id": "a" * 40}}]
        if "/tags" in path:
            return [{"name": "v1.0.0", "commit": {"id": "b" * 40}}]
        if "/issues/comments" in path:
            return [self._item(801, issue_url="/issues/11")]
        if "/issues?" in path:
            return [self._item(201, number=11, labels=[{"id": 401, "name": "bug"}])]
        if "/pulls/22/reviews" in path:
            return [self._item(501, state="APPROVED", commit_id="c" * 40)]
        if "/pulls?" in path:
            return [self._item(301, number=22, head={"sha": "c" * 40})]
        if "/releases" in path:
            return [self._item(701, tag_name="v1.0.0", assets=[{
                "id": 702, "name": "fixture.tar.gz", "size": 123,
                "browser_download_url": "https://downloads.test/fixture",
            }])]
        if "/contents/README.md" in path:
            return {"path": "README.md", "sha": "d" * 40,
                    "encoding": "base64", "content": "IyBGaXh0dXJlCg=="}
        if "/labels" in path:
            return [self._item(401, name="bug", color="ff0000")]
        if "/contributors" in path:
            return [{"id": 9, "login": "maintainer", "permissions": {
                "admin": True, "push": True,
            }}]
        raise AssertionError("unhandled Codeberg path " + path)

    async def fetch(self, env, provider, path, token):
        assert provider == self.provider
        self.calls.append((path, token))
        parsed = urlparse(path)
        clean_path = parsed.path + ("?" + parsed.query if parsed.query else "")
        root_paths = {
            "github": "/repos/acme/project-%d" % self.repository_number,
            "gitlab": f"/projects/acme%2Fproject-{self.repository_number}",
            "codeberg": "/repos/acme/project-%d" % self.repository_number,
        }
        if clean_path == root_paths[self.provider] or (
                self.provider == "gitlab" and clean_path == root_paths[self.provider] + "?license=true"):
            return {"status": 200, "data": self.root(), "headers": {
                "x-ratelimit-remaining": "5000",
            }}
        if self.rate_limit_once and self.rate_limit_once in path:
            self.rate_limit_once = ""
            return {"status": 429, "data": {}, "headers": {
                "retry-after": "1", "x-ratelimit-remaining": "0",
            }}
        for stream in self.removed_streams:
            if stream in path:
                return {"status": 200, "data": [], "headers": {}}
        if self.provider == "gitlab":
            data = self._gitlab(path)
        elif self.provider == "codeberg":
            data = self._codeberg(path)
        else:
            data = self._github(path)
        return {"status": 200, "data": data, "headers": {
            "x-ratelimit-remaining": "4999",
        }}


class Harness:
    def __init__(self, provider="github", connection=None,
                 repository_number=101):
        self.db = connection or sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self._schema()
        self.now = 1_900_000_000_000
        self.fixture = ProviderFixture(provider, repository_number)
        self.audit = []
        self.service = imports.RepositoryImportService({
            "json_response": self.json_response,
            "ensure_schema": self.ensure_schema,
            "d1_all": self.d1_all,
            "d1_first": self.d1_first,
            "d1_run": self.d1_run,
            "encrypt_row": self.encrypt_row,
            "decrypt_row": self.decrypt_row,
            "blind_index": self.blind_index,
            "session_record": self.session_record,
            "provider_fetch": self.fixture.fetch,
            "audit": self.audit_event,
            "migration_sign": self.sign,
            "report_sign": self.report_sign,
            "is_moderator": self.false,
            "now_ms": lambda: self.now,
        })

    def _schema(self):
        self.db.execute(
            "CREATE TABLE IF NOT EXISTS schema_meta "
            "(k TEXT PRIMARY KEY, v TEXT NOT NULL)"
        )
        base = (ROOT / "migrations" / "0044_repository_imports.sql").read_text()
        migration_sql = (
            ROOT / "migrations" / "0152_repository_migration_jobs.sql"
        ).read_text()
        self.db.executescript(base + "\n" + migration_sql)
        self.db.commit()

    @staticmethod
    def json_response(data, status=200, **kwargs):
        return {"status": status, "data": deepcopy(data), "options": kwargs}

    @staticmethod
    async def ensure_schema(env):
        return None

    @staticmethod
    async def encrypt_row(env, value):
        return json.dumps(value, sort_keys=True, separators=(",", ":"))

    @staticmethod
    async def decrypt_row(env, value, key=None):
        try:
            return json.loads(value)
        except (TypeError, ValueError):
            return None

    @staticmethod
    async def blind_index(env, value):
        return "bi:" + str(value).casefold()

    @staticmethod
    async def session_record(env, request, data=None):
        actor = getattr(request, "actor", "")
        return ("bi:" + actor, {"name": actor}) if actor else ("", None)

    @staticmethod
    async def false(env, actor):
        return False

    @staticmethod
    async def sign(env, digest):
        return {
            "algorithm": "hmac-sha256",
            "keyId": "fixture-relay",
            "signature": hmac.new(b"fixture-secret", digest.encode(), hashlib.sha256).hexdigest(),
        }

    @staticmethod
    async def report_sign(env, message):
        public, signature = ed25519_sign(REPORT_SEED, bytes(message))
        return {
            "algorithm": "ed25519",
            "keyId": "fixture-router",
            "publicKey": b64url(public),
            "signature": b64url(signature),
        }

    async def audit_event(self, env, actor, action, target_type, target,
                          outcome, details):
        self.audit.append((action, target, outcome, deepcopy(details)))

    async def d1_first(self, env, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row else None

    async def d1_all(self, env, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]

    async def d1_run(self, env, sql, *args):
        self.db.execute(sql, args)
        self.db.commit()

    def call(self, method, path, body=None, actor="alice"):
        self.now += 25
        return run(self.service.handle(
            None, Request(method, body, actor, path), path.split("?", 1)[0]))

    def start(self, setup_ms=45_000, token="fixture-token"):
        return self.call("POST", "/api/repository-imports/migrations", {
            "sourceUrl": self.fixture.source_url(),
            "providerToken": token,
            "setupDurationMs": setup_ms,
            "sessionToken": "alice",
        })

    def finish(self, job_id, token="fixture-token", limit=100):
        responses = []
        for _ in range(limit):
            response = self.call(
                "POST", "/api/repository-imports/migrations/%s/resume" % job_id,
                {"providerToken": token, "sessionToken": "alice"},
            )
            responses.append(response)
            if response["status"] != 200:
                break
            if response["data"]["migration"]["status"] == "completed":
                break
        else:
            raise AssertionError("fixture migration did not complete")
        return responses


def test_adapter_contract_covers_every_requested_object_type_for_each_provider():
    for provider in ("github", "gitlab", "codeberg"):
        fixture = ProviderFixture(provider)
        source = imports.parse_provider_source(fixture.source_url())
        adapter = migration.provider_adapter(
            source, fixture.root(), {"pull_requests": ["22"]})
        assert tuple(adapter["streams"]) == migration.OBJECT_TYPES
        assert all(adapter["streams"][kind] for kind in migration.OBJECT_TYPES)
        assert adapter["provider"] == provider
        assert adapter["unsupported"]  # lossy/out-of-band areas are explicit


def test_normalized_object_preserves_payload_ids_timestamps_and_redactions():
    fixture = ProviderFixture("github")
    source = imports.parse_provider_source(fixture.source_url())
    adapter = migration.provider_adapter(source, fixture.root(), {})
    stream = adapter["streams"]["issues"][0]
    source_item = fixture._item(
        201, number=11, title="Exact title", body="Exact\nbody",
        token="must-not-survive", labels=[{"id": 401, "name": "bug"}],
    )
    normalized = migration.normalize_object(adapter, stream, source_item, "101")
    record = normalized["record"]
    assert record["source"]["id"] == "issue:201"
    assert record["source"]["createdAt"] == "2025-01-02T03:04:05Z"
    assert record["common"]["body"] == "Exact\nbody"
    assert record["providerPayload"]["labels"] == [{"id": 401, "name": "bug"}]
    assert record["providerPayload"]["token"] == "[redacted credential field]"
    assert record["redactedFields"] == ["token"]
    assert normalized["payloadHash"].startswith("sha256:")


def test_release_coverage_never_confuses_provider_metadata_with_retained_bytes():
    fixture = ProviderFixture("github")
    source = imports.parse_provider_source(fixture.source_url())
    adapter = migration.provider_adapter(source, fixture.root(), {})
    tag_stream = adapter["streams"]["git_refs"][1]
    release_stream = adapter["streams"]["releases"][0]
    tag = migration.normalize_object(
        adapter, tag_stream, fixture._github("/tags")[0], "101")["record"]
    release_item = fixture._github("/releases")[0]
    # Provider-only fields and arbitrary nested release metadata are preserved
    # exactly in the fidelity payload; the report must still say the binary
    # itself was not retained merely because its download URL was copied.
    release_item["discussion_url"] = "https://forge.test/releases/v1#notes"
    release_item["assets"][0]["custom_provider_field"] = {
        "nested": ["value", 7],
    }
    release = migration.normalize_object(
        adapter, release_stream, release_item, "101")["record"]
    assert release["providerPayload"] == release_item

    coverage = migration.release_coverage([tag, release])
    assert coverage["missingTags"] == []
    assert coverage["missingArtifacts"] == [
        {"release": "v1.0.0", "name": "fixture.tar.gz"}]
    assert coverage["missingChecksums"] == [
        {"release": "v1.0.0", "name": "fixture.tar.gz"}]
    assert coverage["missingProvenance"] == [
        {"release": "v1.0.0", "name": "fixture.tar.gz"}]
    assert coverage["missingManifests"] == ["v1.0.0"]
    assert coverage["documentationSiteInventory"] == "unavailable"


def test_all_provider_fixtures_round_trip_every_supported_type_without_duplicates():
    for provider in ("github", "gitlab", "codeberg"):
        harness = Harness(provider)
        started = harness.start(setup_ms=75_000)
        assert started["status"] == 202
        job = started["data"]["migration"]
        assert job["setup"]["durationMs"] == 75_000
        assert job["transfer"]["startedAt"] == 0
        responses = harness.finish(job["id"])
        assert responses[-1]["data"]["migration"]["status"] == "completed"
        completed = responses[-1]["data"]["migration"]
        assert set(completed["counts"]) == set(migration.OBJECT_TYPES)
        assert all(completed["counts"][kind] > 0 for kind in migration.OBJECT_TYPES)
        rows = harness.db.execute(
            "SELECT object_type,source_key,COUNT(*) AS n FROM "
            "repository_migration_objects GROUP BY object_type,source_key"
        ).fetchall()
        assert rows and all(row["n"] == 1 for row in rows)
        exported = harness.call(
            "GET", "/api/repository-imports/migrations/%s/objects" % job["id"])
        assert exported["status"] == 200
        assert exported["data"]["count"] == completed["processed"]
        assert exported["data"]["state"]["signature"]
        coverage = completed["report"]["releaseCoverage"]
        assert coverage["schema"] == "forkmesh-release-coverage-v1"
        # The migration copied release JSON but no artifact byte store is wired
        # to this metadata-only transfer, so it must remain visibly incomplete.
        assert coverage["missingArtifacts"]
        assert coverage["missingManifests"]
        hidden = harness.call(
            "GET", "/api/repository-imports/migrations/%s/objects" % job["id"],
            actor="bob",
        )
        assert hidden["status"] == 404
        first_page = harness.call(
            "GET",
            "/api/repository-imports/migrations/%s/objects?limit=2" % job["id"],
        )
        assert first_page["data"]["count"] == 2
        assert first_page["data"]["hasMore"] is True
        second_page = harness.call(
            "GET",
            "/api/repository-imports/migrations/%s/objects?limit=2&after=%s"
            % (job["id"], first_page["data"]["nextCursor"]),
        )
        first_ids = {item["source"]["id"] for item in first_page["data"]["objects"]}
        second_ids = {item["source"]["id"] for item in second_page["data"]["objects"]}
        assert first_ids.isdisjoint(second_ids)


def test_interruption_resumes_exact_checkpoint_and_rate_limit_does_not_advance():
    harness = Harness("github")
    started = harness.start()
    job_id = started["data"]["migration"]["id"]
    first = [
        harness.call(
            "POST", "/api/repository-imports/migrations/%s/resume" % job_id,
            {"providerToken": "fixture-token", "sessionToken": "alice"},
        )
        for _ in range(2)
    ]
    before = first[-1]["data"]["migration"]["checkpoint"]

    # A new service instance over the same database models Worker/process loss.
    resumed = Harness("github", connection=harness.db)
    resumed.fixture.rate_limit_once = "/issues?"
    while True:
        response = resumed.call(
            "POST", "/api/repository-imports/migrations/%s/resume" % job_id,
            {"providerToken": "fixture-token", "sessionToken": "alice"},
        )
        if response["status"] == 429:
            limited = response
            break
        assert response["status"] == 200
    limited_job = limited["data"]["migration"]
    assert limited_job["status"] == "rate_limited"
    assert limited_job["checkpoint"]["phase"] == "issues"
    assert limited_job["retryCount"] == 1
    after = resumed.call(
        "POST", "/api/repository-imports/migrations/%s/resume" % job_id,
        {"providerToken": "fixture-token", "sessionToken": "alice"},
    )
    assert after["status"] == 200
    assert after["data"]["migration"]["checkpoint"] != limited_job["checkpoint"]
    assert before["phaseIndex"] <= limited_job["checkpoint"]["phaseIndex"]
    assert resumed.finish(job_id)[-1]["data"]["migration"]["status"] == "completed"


def test_repeated_import_converges_and_prunes_source_deletions():
    harness = Harness("github")
    first_start = harness.start()
    job_id = first_start["data"]["migration"]["id"]
    first = harness.finish(job_id)[-1]["data"]["migration"]
    first_state = first["state"]

    second_start = harness.start()
    assert second_start["data"]["migration"]["generation"] == 2
    second = harness.finish(job_id)[-1]["data"]["migration"]
    assert second["state"] == first_state
    assert harness.db.execute(
        "SELECT COUNT(*) FROM repository_migration_objects WHERE job_id=?",
        (job_id,),
    ).fetchone()[0] == first["objectCount"] if "objectCount" in first else first["processed"]

    harness.fixture.removed_streams.add("/labels")
    harness.start()
    third = harness.finish(job_id)[-1]["data"]["migration"]
    # One missing scan is retained to protect against an active source moving
    # an item between offset pages while the transfer is running.
    assert third["counts"]["labels"] == 1
    assert third["state"] == first_state
    harness.start()
    fourth = harness.finish(job_id)[-1]["data"]["migration"]
    assert fourth["counts"]["labels"] == 0
    assert fourth["state"]["hash"] != first_state["hash"]
    harness.start()
    fifth = harness.finish(job_id)[-1]["data"]["migration"]
    assert fifth["state"] == fourth["state"]


def test_abandon_recover_rollback_and_credential_non_persistence():
    harness = Harness("github")
    secret = "ghp_DO_NOT_STORE_THIS_VALUE"
    started = harness.start(token=secret)
    job_id = started["data"]["migration"]["id"]
    for _ in range(2):
        harness.call(
            "POST", "/api/repository-imports/migrations/%s/resume" % job_id,
            {"providerToken": secret, "sessionToken": "alice"},
        )
    abandoned = harness.call(
        "POST", "/api/repository-imports/migrations/%s/abandon" % job_id,
        {"sessionToken": "alice"},
    )
    assert abandoned["data"]["migration"]["status"] == "abandoned"
    refused = harness.call(
        "POST", "/api/repository-imports/migrations/%s/resume" % job_id,
        {"providerToken": secret, "sessionToken": "alice"},
    )
    assert refused["status"] == 409
    recovered = harness.call(
        "POST", "/api/repository-imports/migrations/%s/resume" % job_id,
        {"providerToken": secret, "sessionToken": "alice", "recover": True},
    )
    assert recovered["status"] == 200
    stored = "\n".join(
        str(value)
        for table in ("repository_imports", "repository_migration_jobs",
                      "repository_migration_objects", "repository_migration_runs")
        for row in harness.db.execute("SELECT * FROM " + table).fetchall()
        for value in row
    )
    assert secret not in stored
    rolled_back = harness.call(
        "POST", "/api/repository-imports/migrations/%s/rollback" % job_id,
        {"sessionToken": "alice"},
    )
    assert rolled_back["data"]["rolledBack"] is True
    assert rolled_back["data"]["repositoryListingPreserved"] is True
    assert harness.db.execute(
        "SELECT COUNT(*) FROM repository_imports").fetchone()[0] == 1
    assert harness.db.execute(
        "SELECT COUNT(*) FROM repository_migration_jobs").fetchone()[0] == 0


def test_setup_p50_and_p95_are_reported_separately_from_transfer():
    connection = sqlite3.connect(":memory:")
    connection.row_factory = sqlite3.Row
    durations = (60_000, 120_000, 240_000)
    for provider_index, provider in enumerate(("github", "gitlab", "codeberg")):
        for index, duration in enumerate(durations, 1):
            harness = Harness(
                provider, connection=connection,
                repository_number=100 + provider_index * 10 + index)
            started = harness.start(setup_ms=duration)
            harness.finish(started["data"]["migration"]["id"])
    metrics = harness.call(
        "GET", "/api/repository-imports/migration-metrics")
    assert metrics["status"] == 200
    all_metrics = metrics["data"]["all"]
    assert all_metrics["setupP50Ms"] == 120_000
    assert all_metrics["setupP95Ms"] == 240_000
    assert all_metrics["setupMedianWithinThreeMinutes"] is True
    assert all_metrics["transferP50Ms"] is not None
    assert all_metrics["completionRate"] == 1.0
    assert all_metrics["runs"] == 9
    for provider in ("github", "gitlab", "codeberg"):
        provider_metrics = metrics["data"]["providers"][provider]
        assert provider_metrics["runs"] == 3
        assert provider_metrics["setupP50Ms"] == 120_000
        assert provider_metrics["setupP95Ms"] == 240_000
        assert provider_metrics["setupMedianWithinThreeMinutes"] is True


def test_runtime_and_upgrade_schemas_and_worker_signer_are_linked():
    schema = (ROOT / "src" / "schema.py").read_text()
    upgrade = (
        ROOT / "migrations" / "0152_repository_migration_jobs.sql"
    ).read_text()
    entry = (ROOT / "src" / "entry.py").read_text()
    for table in (
            "repository_migration_jobs", "repository_migration_objects",
            "repository_migration_runs"):
        assert table in schema
        assert table in upgrade
    assert '"migration_sign": _repository_migration_sign' in entry
    assert '"report_sign": _repository_migration_report_sign' in entry
    assert '"managed_owner_bis": _repository_import_managed_owner_bis' in entry
    assert '"link", "x-next-page"' in entry
    assert "credentialStored" in (ROOT / "src" / "repository_imports.py").read_text()


def test_signed_completion_never_claims_the_repository_is_mirrored():
    """A completed migration copies provider records, not Git objects.

    The modal reported "Signed import complete" and "reached signed
    completion", and stopped there. A reporter read that as a finished mirror
    and then found the repository under "External repositories & stubs",
    marked as not mirrored by ForkMesh (issue #678). The copy now says what
    completed, and the flow ends on the desktop steps that do produce a
    mirror.
    """
    script = (
        ROOT / "public" / "dashboard" / "js" / "05-repo-list-explorer.js"
    ).read_text()
    modal = (
        ROOT / "public" / "dashboard" / "partials" / "new-repo-modal.html"
    ).read_text()
    built_js = (ROOT / "public" / "dashboard.js").read_text()

    # Nothing in the flow calls a metadata import a completed import, and no
    # provider copy promises a mirror.
    assert "Signed import complete" not in script
    assert "reached signed completion" not in script
    assert "Signed metadata import complete" in script
    assert "reached signed metadata completion" in script
    assert "Git contents not mirrored" in script
    assert "metadata only, no Git contents mirrored" in script
    assert "No Git contents were mirrored" in script

    # The warning precedes the import: the provider tab restates it in the
    # modal subtitle and the source hint before anything is started.
    assert "NEW_REPO_PROVIDER_SUBTITLE" in script
    assert "Mirroring the Git contents is a separate step" in script
    assert "an external listing, not a ForkMesh mirror" in script
    assert "(no Git contents)" in modal

    # And the receipt hands off to the steps that actually mirror it, reusing
    # the external-mirror handoff list rather than a second copy of it.
    assert "function showNewRepoMirrorSteps(sourceUrls)" in script
    assert "showNewRepoMirrorSteps(sourceUrls);" in script
    steps = script[
        script.index("function showNewRepoMirrorSteps(sourceUrls)"):
        script.index("// A bare organization link")
    ]
    assert "externalMirrorHandoffSteps(" in steps
    assert "data-new-repo-steps-intro" in steps
    # The built asset production serves carries the same copy.
    assert "reached signed metadata completion" in built_js
    assert "Signed import complete" not in built_js


def test_dashboard_starts_and_resumes_jobs_without_mixing_setup_and_transfer():
    script = (
        ROOT / "public" / "dashboard" / "js" / "05-repo-list-explorer.js"
    ).read_text()
    wiring = (
        ROOT / "public" / "dashboard" / "js" / "08-repo-detail-network.js"
    ).read_text()
    modal = (
        ROOT / "public" / "dashboard" / "partials" / "new-repo-modal.html"
    ).read_text()
    built = (ROOT / "public" / "dashboard" / "repos" / "index.html").read_text()
    assert '"/api/repository-imports/migrations"' in script
    assert "setupDurationMs" in script
    assert "advanceRepositoryMigration" in script
    assert "safe to close this page" in script
    assert "data-repository-migration-resume" in script
    assert "continueRepositoryMigration" in wiring
    assert "Hands-on setup" in script
    assert "transfer excluded" in script
    assert "data-new-repo-migration-status" in modal
    # The import mode names what it copies. It used to read "Full
    # repository", which a reporter reasonably took for a mirror of the
    # Git contents (issue #678).
    assert "Full provider metadata &amp; collaboration history (no Git contents)" in modal
    assert "Full repository &amp;" not in modal
    assert "data-new-repo-migration-status" in built
