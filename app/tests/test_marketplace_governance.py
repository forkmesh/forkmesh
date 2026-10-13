#!/usr/bin/env python3
"""Behavioral coverage for the two-stage marketplace publication gate."""

import asyncio
import hashlib
import json
import sqlite3
import sys
from pathlib import Path


SRC = Path(__file__).resolve().parents[1] / "src"
APP = SRC.parent
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import marketplace_governance as governance  # noqa: E402


MANIFEST = json.dumps({
    "schemaVersion": 1,
    "id": "community-clock",
    "name": "Community Clock",
    "version": "1.0.0",
    "type": "virtual.object",
    "summary": "A community-owned clock for ForkMesh World.",
    "creator": {
        "id": "did:forkmesh:alice", "handle": "alice",
        "displayName": "Alice", "verified": False,
    },
    "license": {
        "model": "free-open-source", "spdx": "MIT",
        "creatorRetainsOwnership": True,
    },
    "price": {"amount": 0, "currency": "USD"},
    "entrypoints": {
        "world": "content/world/index.js", "settings": "builtin:package"
    },
    "permissions": [],
    "screenshots": [{"path": "preview/market.png", "alt": "Clock in World"}],
    "lifecycle": {"migrations": []},
}, separators=(",", ":"))


def _submission():
    return {
        "marketplace": "forkmesh-community",
        "package": "community-clock",
        "version": "1.0.0",
        "manifestUrl": "https://plugins.example.com/clock/manifest.json",
        "manifestSha256": hashlib.sha256(MANIFEST.encode()).hexdigest(),
        "sources": [{
            "transport": "git",
            "url": "https://gitlab.example.com/alice/clock.git",
            "ref": "v1.0.0",
            "catalog": "catalog.json",
            "provider": "gitlab",
        }],
        "artifact": {
            "sha256": "a" * 64,
            "size": 1234,
            "locations": [{
                "transport": "https",
                "url": "https://cdn.example.com/community-clock.fmpkg",
            }],
        },
    }


class Runtime:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.executescript("""
            CREATE TABLE marketplace_submissions (
                submission_id TEXT PRIMARY KEY, marketplace_id TEXT NOT NULL,
                package_id TEXT NOT NULL, version TEXT NOT NULL,
                manifest_url TEXT NOT NULL, manifest_sha256 TEXT NOT NULL,
                sources_json TEXT NOT NULL, artifact_sha256 TEXT NOT NULL,
                artifact_size INTEGER NOT NULL,
                artifact_locations_json TEXT NOT NULL,
                submitted_by_bi TEXT NOT NULL, submitted_by_name TEXT NOT NULL,
                state TEXT NOT NULL, owner_approved_by_bi TEXT NOT NULL DEFAULT '',
                owner_approved_by_name TEXT NOT NULL DEFAULT '',
                rejection_reason TEXT NOT NULL DEFAULT '', created_at INTEGER NOT NULL,
                updated_at INTEGER NOT NULL, published_at INTEGER NOT NULL DEFAULT 0,
                UNIQUE(marketplace_id,package_id,version,manifest_sha256)
            );
            CREATE TABLE marketplace_member_approvals (
                submission_id TEXT NOT NULL, reviewer_bi TEXT NOT NULL,
                reviewer_name TEXT NOT NULL, wallet_address TEXT NOT NULL,
                value_lamports INTEGER NOT NULL, value_checked_at INTEGER NOT NULL,
                manifest_sha256 TEXT NOT NULL, approved_at INTEGER NOT NULL,
                PRIMARY KEY(submission_id,reviewer_bi)
            );
        """)
        self.viewer = ("alice-bi", {"name": "alice", "status": "active"})
        self.body = _submission()
        self.http_method = "POST"
        self.clock = 1000
        self.accounts = {}
        self.owners = {"owner"}
        self.audits = []

    def method(self):
        return self.http_method

    def same_origin(self):
        return True

    def now(self):
        self.clock += 1
        return self.clock

    def response(self, payload, status=200, **_kwargs):
        return {"status": status, "payload": payload}

    def public_catalog_url(self):
        return "https://forkmesh.com/api/market/catalog.json"

    async def ensure_schema(self):
        return None

    async def json_body(self, _limit):
        return self.body, ""

    async def session(self, _data=None):
        return self.viewer

    async def d1_all(self, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]

    async def d1_first(self, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row is not None else None

    async def d1_run(self, sql, *args):
        self.db.execute(sql, args)
        self.db.commit()

    async def fetch_manifest(self, _url, _limit):
        return MANIFEST, ""

    async def account(self, account_bi):
        return self.accounts.get(account_bi)

    async def is_forkmesh_repo_owner(self, name):
        return name in self.owners

    async def audit(self, actor, action, submission_id, outcome):
        self.audits.append((actor, action, submission_id, outcome))


def _run(awaitable):
    return asyncio.run(awaitable)


def _submit(runtime):
    response = _run(governance.handle(runtime, "/api/market/submissions"))
    assert response["status"] == 201
    return response["payload"]["id"]


def test_submission_contract_rejects_noncommunity_and_unpaired_migrations():
    wrong_market = _submission()
    wrong_market["marketplace"] = "someone-else"
    assert governance.submission_fields(wrong_market) is None

    fields = governance.submission_fields(_submission())
    broken = json.loads(MANIFEST)
    broken["lifecycle"]["migrations"] = [{
        "id": "0001_create", "up": "migrations/up.sql"
    }]
    text = json.dumps(broken, separators=(",", ":"))
    fields["manifestSha256"] = hashlib.sha256(text.encode()).hexdigest()
    assert governance.validate_manifest_text(text, fields) == "invalid_manifest_migration"


def test_five_active_members_without_wallets_then_a_distinct_owner_publish():
    # Issue #516: no reviewer has a payout address, and the runtime exposes
    # no wallet capability at all; governance must still complete.
    runtime = Runtime()
    submission_id = _submit(runtime)
    for index in range(1, 6):
        name = f"reviewer{index}"
        account_bi = f"reviewer-{index}-bi"
        account = {"name": name, "status": "active"}
        runtime.viewer = (account_bi, account)
        runtime.accounts[account_bi] = account
        runtime.body = {}
        response = _run(governance.handle(
            runtime,
            f"/api/market/submissions/{submission_id}/member-approval",
        ))
        assert response["status"] == 200

    projected = response["payload"]
    assert projected["state"] == "owner_review"
    assert projected["memberApprovalCount"] == 5
    assert [item["name"] for item in projected["memberApprovals"]] == [
        f"reviewer{index}" for index in range(1, 6)
    ]
    for item in projected["memberApprovals"]:
        assert set(item) == {"name", "approvedAt"}
    stored = runtime.db.execute(
        "SELECT DISTINCT wallet_address,value_lamports,value_checked_at "
        "FROM marketplace_member_approvals").fetchall()
    assert [tuple(row) for row in stored] == [("", 0, 0)]

    runtime.viewer = ("owner-bi", {"name": "owner", "status": "active"})
    response = _run(governance.handle(
        runtime,
        f"/api/market/submissions/{submission_id}/owner-approval",
    ))
    assert response["status"] == 200
    assert response["payload"]["state"] == "published"
    assert response["payload"]["ownerApprovedBy"] == "owner"
    runtime.http_method = "GET"
    catalog = _run(governance.handle(runtime, "/api/market/catalog.json"))
    entry = catalog["payload"]["packages"][0]
    assert entry["manifestSha256"] == hashlib.sha256(MANIFEST.encode()).hexdigest()
    assert entry["artifact"]["sha256"] == "a" * 64
    assert entry["sources"][0]["provider"] == "gitlab"


def test_submitter_and_member_reviewer_cannot_make_final_decision():
    runtime = Runtime()
    submission_id = _submit(runtime)
    runtime.body = {}
    response = _run(governance.handle(
        runtime,
        f"/api/market/submissions/{submission_id}/member-approval",
    ))
    assert response["status"] == 403
    assert response["payload"]["error"] == "submitter_cannot_approve"

    runtime.viewer = (
        "owner-bi", {"name": "owner", "status": "active"}
    )
    runtime.accounts["owner-bi"] = runtime.viewer[1]
    response = _run(governance.handle(
        runtime,
        f"/api/market/submissions/{submission_id}/member-approval",
    ))
    assert response["status"] == 200
    response = _run(governance.handle(
        runtime,
        f"/api/market/submissions/{submission_id}/owner-approval",
    ))
    assert response["status"] == 403
    assert response["payload"]["error"] == "member_reviewer_cannot_finalize"


def test_submitter_cannot_grow_an_unbounded_pending_queue():
    runtime = Runtime()
    for index in range(governance.MAX_PENDING_PER_SUBMITTER):
        runtime.db.execute(
            "INSERT INTO marketplace_submissions "
            "(submission_id,marketplace_id,package_id,version,manifest_url,"
            "manifest_sha256,sources_json,artifact_sha256,artifact_size,"
            "artifact_locations_json,submitted_by_bi,submitted_by_name,state,"
            "created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            (
                f"{index:032x}", "forkmesh-community", f"queued-{index}",
                "1.0.0", "https://plugins.example.com/manifest.json",
                f"{index:064x}", "[]", "a" * 64, 1, "[]",
                "alice-bi", "alice", "member_review", 1000, 1000,
            ),
        )
    runtime.db.commit()
    response = _run(governance.handle(runtime, "/api/market/submissions"))
    assert response["status"] == 429
    assert response["payload"]["error"] == "submission_rate_limited"


def test_published_catalog_contains_only_digest_bound_artifact_entries():
    runtime = Runtime()
    runtime.http_method = "GET"
    response = _run(governance.handle(runtime, "/api/market/catalog.json"))
    assert response["status"] == 200
    assert response["payload"]["schemaVersion"] == 2
    assert response["payload"]["packages"] == []
    assert response["payload"]["marketplace"]["locations"] == [{
        "transport": "https",
        "url": "https://forkmesh.com/api/market/catalog.json",
        "provider": "forkmesh",
    }]


def test_worker_routes_governance_and_release_migration_has_a_down_pair():
    entry = (SRC / "entry.py").read_text(encoding="utf-8")
    runtime = (SRC / "app_runtime.py").read_text(encoding="utf-8")
    adapter = (SRC / "marketplace_runtime.py").read_text(encoding="utf-8")
    schema = (SRC / "schema.py").read_text(encoding="utf-8")
    assert 'marketplace_governance = _LazyModule("marketplace_governance")' in entry
    assert 'url.path.startswith("/api/market/submissions/")' in runtime
    assert 'marketplace_runtime = _LazyModule("marketplace_runtime")' in entry
    assert "class _MarketplaceGovernanceRuntime:" in adapter
    assert "CREATE TABLE IF NOT EXISTS marketplace_submissions" in schema
    assert "CREATE TABLE IF NOT EXISTS marketplace_member_approvals" in schema
    assert (APP / "migrations" / "0153_modular_marketplace_governance.sql").is_file()
    assert (
        APP / "migrations" / "down" /
        "0153_modular_marketplace_governance.sql"
    ).is_file()
