"""Reach scoring, URL safety and persistence for the link rewards API."""

import asyncio
import importlib.util
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2] / "app"
SPEC = importlib.util.spec_from_file_location(
    "forkmesh_world_link_kiosk", ROOT.parent / "world" / "src" / "world_link_kiosk.py"
)
KIOSK = importlib.util.module_from_spec(SPEC)
assert SPEC.loader
SPEC.loader.exec_module(KIOSK)


def test_public_url_normalization_never_accepts_local_or_credentialed_targets():
    assert KIOSK.normalize_public_url("http://example.com/post") == ("", "")
    assert KIOSK.normalize_public_url("https://localhost/post") == ("", "")
    assert KIOSK.normalize_public_url("https://127.0.0.1/post") == ("", "")
    assert KIOSK.normalize_public_url("https://user:pass@example.com/") == ("", "")
    assert KIOSK.normalize_public_url("https://example.com:8443/") == ("", "")

    url, host = KIOSK.normalize_public_url(
        "https://Example.COM/launch?utm_source=world&token=secret#private"
    )
    assert host == "example.com"
    assert url == (
        "https://example.com/launch?"
        "utm_source=world&token=%5Bredacted%5D"
    )


def test_reach_estimate_is_bounded_monotonic_and_explainable():
    small = KIOSK.reach_estimate(10, 0, "social")
    larger = KIOSK.reach_estimate(10_000, 500, "social", True)

    assert 0 <= small["score"] < larger["score"] <= 100
    assert larger["potentialTraffic"]["high"] >= larger["potentialTraffic"]["low"]
    assert larger["factors"] == {
        "followerPoints": 60,
        "trafficPoints": 27,
        "verifiedDomainPoints": 10,
        "followers": 10_000,
        "observedAggregateVisits": 500,
        "channel": "social",
    }
    assert KIOSK.reward_sol(0) == 0.00005
    assert KIOSK.reward_sol(100) == 0.0005
    assert KIOSK.reward_sol(75) > KIOSK.reward_sol(25)


def test_public_links_include_current_wallet_reward_and_explicit_payment_state():
    wallet = "4Nd1mJ9VLUyV9K1wMYHTRZDNzzWQYTf1L8YgJnU3hFJH"

    class Runtime:
        async def d1_all(self, _sql, _limit):
            return [{
                "link_id": "link-1",
                "account_bi": "account-1",
                "data": {
                    "url": "https://example.com/story",
                    "title": "Story",
                    "account": "alice",
                    "channel": "article",
                    "paidAt": 123,
                },
                "account_data": {
                    "status": "active",
                    "solana": wallet,
                },
                "score": 80,
                "potential_low": 4,
                "potential_high": 12,
                "created_at": 100,
            }]

        async def open(self, value):
            return value

    links = asyncio.run(KIOSK._public_links(Runtime()))
    assert links[0]["submittedBy"] == "alice"
    assert links[0]["walletAddress"] == wallet
    assert links[0]["rewardSol"] == KIOSK.reward_sol(80)
    assert links[0]["paymentStatus"] == "paid"
    assert links[0]["paidAt"] == 123


def test_link_api_is_migrated_routed_and_audited():
    migration = (
        ROOT / "migrations/0103_world_lobby_link_kiosk.sql"
    ).read_text(encoding="utf-8")
    schema = (ROOT / "src/schema.py").read_text(encoding="utf-8")
    entry = (
        (ROOT / "src/entry.py").read_text(encoding="utf-8")
        + (ROOT / "src/app_runtime.py").read_text(encoding="utf-8")
    )
    kiosk = (
        ROOT.parent / "world" / "src" / "world_link_kiosk.py"
    ).read_text(encoding="utf-8")

    assert "CREATE TABLE IF NOT EXISTS world_lobby_links" in migration
    assert "UNIQUE(account_bi, url_bi)" in migration
    assert "world_lobby_links" in schema
    assert "world_link_kiosk_handler" in entry
    assert '"/api/world/link-kiosk"' in entry
    assert "world.link_kiosk_submit" in kiosk
    assert "paymentStatus" in kiosk


def test_team_onboarding_documents_roles_review_gate_and_link_rewards():
    docs = (
        ROOT.parent / "www/public/docs/onboarding/index.html"
    ).read_text(encoding="utf-8")
    home = (ROOT.parent / "www/public/docs/index.html").read_text(
        encoding="utf-8")

    for phrase in (
        "QA onboarding",
        "Developer onboarding",
        "Marketing onboarding",
        "Peer-review and merge policy",
        "Link rewards",
        "/dashboard/links",
    ):
        assert phrase in docs
    assert "/docs/onboarding" in home
