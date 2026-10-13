import asyncio
import json
from pathlib import Path
import sqlite3
import sys


ROOT = Path(__file__).resolve().parents[2] / "app"
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import community_ads as policy  # noqa: E402
import community_ads_api as api  # noqa: E402


NOW = 1_800_000_000_000


def proposal(**updates):
    value = {
        "title": "Open tooling sponsor",
        "sponsor": "Example Cooperative",
        "sponsorDisclosure": (
            "Worker-owned cooperative; no parent or beneficial owner."
        ),
        "copy": "Auditable build infrastructure for open-source teams.",
        "destinationUrl": "https://sponsor.example.org/products/builds",
        "contextTags": ["developer-tools", "open-source"],
        "evidence": [
            {
                "url": "https://evidence.example.org/reports/labor",
                "title": "Labor report",
                "publisher": "Independent Observer",
                "observedAt": NOW - 1000,
                "supports": "Published workforce and pay-band evidence.",
            },
            {
                "url": "https://audit.example.com/privacy/example-coop",
                "title": "Privacy audit",
                "publisher": "Audit Group",
                "observedAt": NOW - 2000,
                "supports": "Documents contextual advertising and no profiles.",
            },
        ],
        "prohibitedCategories": [],
        "conflictDisclosure": "The proposer is not paid by the sponsor.",
        "votingDays": 14,
        "quorum": 5,
    }
    value.update(updates)
    return value


def test_proposal_requires_bounded_independent_public_evidence():
    normalized, error = policy.normalize_proposal(proposal(), NOW)
    assert error == ""
    assert normalized["tracking"] == "none"
    assert normalized["targeting"] == "context-only"
    assert len(normalized["evidence"]) == 2

    assert policy.normalize_proposal(
        proposal(evidence=proposal()["evidence"][:1]), NOW
    )[1] == "evidence_required"
    duplicate = proposal()["evidence"]
    duplicate[1]["url"] = duplicate[0]["url"]
    assert policy.normalize_proposal(
        proposal(evidence=duplicate), NOW
    )[1] == "invalid_evidence"
    assert policy.normalize_proposal(
        proposal(destinationUrl="http://tracking.example.org/ad"), NOW
    )[1] == "invalid_proposal"
    assert policy.normalize_proposal(
        proposal(prohibitedCategories=["behavioral-surveillance"]), NOW
    )[1] == "prohibited_category"


def test_vote_is_one_account_not_wallet_weighted_and_conflicts_abstain():
    eligible = {
        "status": "active",
        "email_verified": True,
        "created_at": NOW - policy.MIN_ACCOUNT_AGE_MS - 1,
        "wallet": "not-consulted",
    }
    assert policy.voter_eligible(eligible, NOW) == (True, "")
    assert policy.voter_eligible(
        {**eligible, "created_at": NOW - 1000}, NOW
    )[1] == "minimum_account_age"
    assert policy.normalize_vote({
        "choice": "approve",
        "conflictOfInterest": True,
        "conflictDisclosure": "Sponsor employee.",
    })[1] == "conflicted_voter_must_abstain"
    vote, error = policy.normalize_vote({
        "choice": "abstain",
        "conflictOfInterest": True,
        "conflictDisclosure": "Sponsor employee.",
    })
    assert error == ""
    assert vote["conflictOfInterest"] is True


def test_tally_requires_quorum_and_two_thirds_decisive_approval():
    rows = [{"choice": "approve"}] * 4 + [{"choice": "reject"}]
    result = policy.tally(rows, quorum=5)
    assert result["quorumMet"] is True
    assert result["approved"] is True
    assert policy.tally(
        [{"choice": "approve"}] * 3 + [{"choice": "reject"}] * 2,
        quorum=5,
    )["approved"] is False
    assert policy.tally(rows[:4], quorum=5)["approved"] is False


def test_instance_policy_is_explicit_and_ad_revenue_schema_is_separate():
    normalized, error = policy.normalize_instance_policy({
        "enabled": False,
        "contexts": ["town-square", "repository"],
        "revenueDestination": "Public infrastructure operating costs",
        "revenueDestinationType": "instance-operations",
    })
    assert error == ""
    assert normalized["ledgerClass"] == "advertising-revenue"

    db = sqlite3.connect(":memory:")
    db.executescript(
        (ROOT / "migrations" / "0063_community_ad_governance.sql")
        .read_text(encoding="utf-8")
    )
    tables = {
        row[0]
        for row in db.execute(
            "SELECT name FROM sqlite_master WHERE type='table'"
        )
    }
    assert {
        "community_ad_instance_policy",
        "community_ad_proposals",
        "community_ad_votes",
        "community_ad_moderation",
        "community_ad_revenue",
    } <= tables
    migration = (
        ROOT / "migrations" / "0063_community_ad_governance.sql"
    ).read_text(encoding="utf-8")
    assert "advertising-revenue" in migration
    assert "reward_pool" not in migration.lower()
    assert "pending_rewards" not in migration.lower()
    assert "wallet" in migration.lower()


def test_public_criteria_never_claim_universal_ethics_or_target_people():
    rendered = json.dumps(policy.ELIGIBILITY_CRITERIA).lower()
    assert "not a universal claim" in rendered
    assert '"behavioraltracking": false' in rendered
    assert '"sensitivetargeting": false' in rendered
    assert '"walletbalanceweight": false' in rendered


class FakeRuntime:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.executescript(
            (ROOT / "migrations" / "0063_community_ad_governance.sql")
            .read_text(encoding="utf-8")
        )
        self.request_method = "GET"
        self.request_data = {}
        self.params = {}
        self.actor = ""
        self.clock = NOW
        self.ids = 0
        self.admins = {"admin"}
        self.accounts = {
            name: {
                "name": name,
                "status": "active",
                "email_verified": True,
                "created_at": NOW - policy.MIN_ACCOUNT_AGE_MS - 1,
            }
            for name in ["admin", "alice", "bob", "carol", "dan", "erin"]
        }
        self.audits = []

    def use(self, method, actor="", data=None, params=None, now=None):
        self.request_method = method
        self.actor = actor
        self.request_data = {} if data is None else data
        self.params = dict(params or {})
        if now is not None:
            self.clock = now
        return self

    def method(self):
        return self.request_method

    def now(self):
        return self.clock

    def new_id(self):
        self.ids += 1
        return f"{self.ids:032x}"

    def instance_id(self):
        return "relay.example.org"

    def query_params(self):
        return dict(self.params)

    def response(self, data, status=200, cache_control=None,
                 extra_headers=None):
        return {
            "status": status,
            "data": data,
            "cacheControl": cache_control,
            "headers": dict(extra_headers or {}),
        }

    async def ensure_schema(self):
        return None

    async def json_body(self, _limit):
        return (
            (self.request_data, "")
            if isinstance(self.request_data, dict)
            else (None, "invalid_json")
        )

    async def session(self, _data):
        account = self.accounts.get(self.actor)
        return (
            (f"bi-{self.actor}", dict(account))
            if account else ("", None)
        )

    async def is_admin(self, name):
        return name in self.admins

    async def audit(self, actor, action, target_type="", target="",
                    outcome="success", details=None):
        self.audits.append({
            "actor": actor,
            "action": action,
            "targetType": target_type,
            "target": target,
            "outcome": outcome,
            "details": details or {},
        })

    async def d1_all(self, sql, *args):
        return [
            dict(row) for row in self.db.execute(sql, args).fetchall()
        ]

    async def d1_first(self, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row is not None else None

    async def d1_run(self, sql, *args):
        self.db.execute(sql, args)
        self.db.commit()


def run(awaitable):
    return asyncio.run(awaitable)


def test_governance_api_votes_expires_and_delivers_context_without_profiles():
    runtime = FakeRuntime()
    created = run(api.handle(
        runtime.use("POST", "alice", proposal()),
        "/api/world/community-ads/proposals",
    ))
    assert created["status"] == 201
    proposal_id = created["data"]["proposal"]["id"]

    for voter in ["alice", "bob", "carol", "dan", "erin"]:
        voted = run(api.handle(
            runtime.use("POST", voter, {
                "choice": "approve",
                "conflictOfInterest": False,
                "conflictDisclosure": "",
            }),
            f"/api/world/community-ads/proposals/{proposal_id}/vote",
        ))
        assert voted["status"] == 200
    closed_at = created["data"]["proposal"]["closesAt"]
    approved = run(api.handle(
        runtime.use("GET", now=closed_at + 1),
        f"/api/world/community-ads/proposals/{proposal_id}",
    ))
    assert approved["data"]["proposal"]["status"] == "approved"

    configured = run(api.handle(
        runtime.use("PUT", "admin", {
            "enabled": True,
            "contexts": ["town-square"],
            "revenueDestination": "Published instance operating costs",
            "revenueDestinationType": "instance-operations",
        }),
        "/api/world/community-ads/policy",
    ))
    assert configured["data"]["policy"]["enabled"] is True
    placement = run(api.handle(
        runtime.use(
            "GET", params={
                "context": "town-square",
                "country": "ignored",
                "browser": "ignored",
                "wallet": "ignored",
            }
        ),
        "/api/world/community-ads/placements",
    ))
    assert placement["data"]["personalDataUsed"] is False
    assert placement["data"]["tracking"] == "none"
    assert placement["data"]["placements"][0]["proposalId"] == proposal_id
    assert "country" not in json.dumps(placement["data"]).lower()
    assert placement["data"]["revenue"]["separateFromRewardPool"] is True


def test_governance_api_denies_unverified_vote_and_records_external_revenue():
    runtime = FakeRuntime()
    created = run(api.handle(
        runtime.use("POST", "alice", proposal()),
        "/api/world/community-ads/proposals",
    ))
    proposal_id = created["data"]["proposal"]["id"]
    runtime.accounts["new"] = {
        "name": "new",
        "status": "active",
        "email_verified": False,
        "created_at": NOW,
    }
    denied = run(api.handle(
        runtime.use("POST", "new", {
            "choice": "approve",
            "conflictOfInterest": False,
            "conflictDisclosure": "",
        }),
        f"/api/world/community-ads/proposals/{proposal_id}/vote",
    ))
    assert denied["status"] == 403

    run(api.handle(
        runtime.use("PUT", "admin", {
            "enabled": False,
            "contexts": ["town-square"],
            "revenueDestination": "Published project operations",
            "revenueDestinationType": "project-operations",
        }),
        "/api/world/community-ads/policy",
    ))
    recorded = run(api.handle(
        runtime.use("POST", "admin", {
            "proposalId": proposal_id,
            "amountMinor": 1250,
            "currency": "USD",
            "externalReference": "invoice-public-42",
        }),
        "/api/world/community-ads/revenue",
    ))
    assert recorded["status"] == 201
    assert recorded["data"]["entry"]["custody"] == (
        "external-accounting-record-only")
    row = runtime.db.execute(
        "SELECT ledger_class FROM community_ad_revenue"
    ).fetchone()
    assert row[0] == "advertising-revenue"


def test_worker_route_schema_docs_and_cleanup_are_wired():
    entry = (
        (SRC / "entry.py").read_text(encoding="utf-8")
        + (SRC / "app_runtime.py").read_text(encoding="utf-8")
    )
    schema = (SRC / "schema.py").read_text(encoding="utf-8")
    docs = (
            ROOT.parent / "www" / "docs" / "community-ad-governance.md"
    ).read_text(encoding="utf-8")
    assert 'community_ads_api = _LazyModule("community_ads_api")' in entry
    assert 'url.path == "/api/world/community-ads"' in entry
    assert "world_community_ads_handler" in entry
    assert "await community_ads_api.cleanup(runtime)" in entry
    assert "CREATE TABLE IF NOT EXISTS community_ad_proposals" in schema
    assert "one eligible verified account" in docs
    assert "does not use profiles" in " ".join(docs.split())
    assert "not user funds" in docs
