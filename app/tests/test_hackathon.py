#!/usr/bin/env python3
"""ForkMesh Hackathon: policy, runtime behaviour and routing contracts.

The runtime half runs against an in-memory SQLite standing in for D1, with
the Worker primitives it borrows from entry.py replaced by small fakes --
the same shape as test_attribution.py. The claims worth running rather than
reading are the ones that decide whether the growth loop is honest:

* one event pays exactly once, however many times its hook fires;
* a friend who verifies counts as *joined*, and only the referral ledger's
  own credit decision produces a *qualified* referral;
* pressing a share button is measured and is worth nothing;
* a draft campaign is invisible on every public route.
"""

import ast
import asyncio
import importlib.util
import itertools
import json
import re
import sqlite3
import sys
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import parse_qs, quote, unquote, urlparse

import pytest

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import dashboard_bundle  # noqa: E402
import dashboard_shell  # noqa: E402
import hackathon as policy  # noqa: E402
import static_routes  # noqa: E402
from schema import SCHEMA_STATEMENTS  # noqa: E402

_OG_SPEC = importlib.util.spec_from_file_location("og_card", SRC / "og_card.py")
og_card = importlib.util.module_from_spec(_OG_SPEC)
_OG_SPEC.loader.exec_module(og_card)

ENTRY_TEXT = (SRC / "entry.py").read_text(encoding="utf-8")
RUNTIME_TEXT = (SRC / "app_runtime.py").read_text(encoding="utf-8")
ACCOUNT_USERS_TEXT = (SRC / "account_users.py").read_text(encoding="utf-8")
CATALOG_TEXT = (SRC / "catalog_routes.py").read_text(encoding="utf-8")
HACKATHON_JS = (
    ROOT / "public" / "dashboard" / "js" / "09-hackathon.js"
).read_text(encoding="utf-8")
WWW_PUBLIC = REPO / "www" / "public"
LANDING = (WWW_PUBLIC / "hackathon.html").read_text(encoding="utf-8")
LANDING_CSS = (WWW_PUBLIC / "hackathon.css").read_text(encoding="utf-8")
LANDING_JS = (WWW_PUBLIC / "hackathon.js").read_text(encoding="utf-8")
PROJECTS_PAGE = (WWW_PUBLIC / "hackathon" / "projects.html").read_text(
    encoding="utf-8")
RULES_PAGE = (WWW_PUBLIC / "hackathon" / "rules.html").read_text(encoding="utf-8")
BOARD_PAGE = (
    WWW_PUBLIC / "hackathon" / "leaderboard.html"
).read_text(encoding="utf-8")
WWW_WORKER = (REPO / "www" / "worker.js").read_text(encoding="utf-8")
APP_WRANGLER = (ROOT / "wrangler.toml").read_text(encoding="utf-8")
WWW_WRANGLER = (REPO / "www" / "wrangler.toml").read_text(encoding="utf-8")
REDIRECTS = (WWW_PUBLIC / "_redirects").read_text(encoding="utf-8")

NOW = 1_760_000_000_000


def code_only(source, comment="//"):
    """Source with its comments removed, so a test cannot match its own prose.

    Several claims below are "this string never appears in the code". A
    comment explaining *why* it never appears would satisfy a naive substring
    search, so comments are dropped before the search runs.
    """
    lines = []
    for line in source.splitlines():
        marker = line.find(comment)
        if marker >= 0 and line[:marker].count('"') % 2 == 0 and (
                line[:marker].count("'") % 2 == 0):
            line = line[:marker]
        lines.append(line)
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Pure policy
# ---------------------------------------------------------------------------

def test_no_point_kind_is_client_reportable():
    # The whole anti-spam posture rests on this: every point comes from an
    # event the server saw. A browser must never be able to assert one.
    assert policy.CLIENT_REPORTABLE_POINT_KINDS == frozenset()
    assert set(policy.POINT_KINDS) >= {
        "joined", "verified", "referral_joined", "referral_qualified",
        "first_mirror", "pull_accepted", "submission",
    }
    for kind, entry in policy.POINT_KINDS.items():
        assert entry["points"] > 0, kind
        assert entry["label"], kind


def test_building_outscores_recruiting():
    # A submission must be worth more than a qualified referral, or the
    # Builders board becomes winnable by recruiting alone.
    assert (policy.point_value("submission")
            > policy.point_value("referral_qualified"))
    assert (policy.point_value("first_mirror")
            > policy.point_value("referral_qualified"))


def test_unique_kinds_collapse_their_reference():
    assert policy.is_unique_point_kind("first_mirror")
    assert policy.point_reference("first_mirror", "a/b") == ""
    assert not policy.is_unique_point_kind("pull_accepted")
    assert policy.point_reference("pull_accepted", "a/b#1") == "a/b#1"


def test_mission_progress_and_next_nudge():
    missions = policy.default_config()["missions"]
    state = policy.missions_state(missions, {"referrals_joined": 1})
    invite_one = next(m for m in state if m["key"] == "invite_one")
    assert invite_one["complete"] is True
    nudge = policy.next_incomplete_mission(
        missions, {"referrals_joined": 1, "referrals_qualified": 2})
    assert nudge["key"] == "invite_three"
    assert nudge["remaining"] == 1


def test_milestone_meter_and_unlocks():
    milestones = [
        {"key": "a", "metric": "participants", "threshold": 250,
         "label": "A", "unlocked_at": 0},
        {"key": "b", "metric": "participants", "threshold": 500,
         "label": "B", "unlocked_at": 0},
    ]
    nxt = policy.next_milestone(milestones, {"participants": 312})
    assert nxt["key"] == "b" and nxt["percent"] == 62
    unlocking = policy.milestones_to_unlock(
        milestones, {"participants": 312}, NOW)
    assert [entry["key"] for entry in unlocking] == ["a"]
    # An already-unlocked milestone is never re-announced.
    milestones[0]["unlocked_at"] = NOW
    assert policy.milestones_to_unlock(
        milestones, {"participants": 312}, NOW) == []


def test_share_text_carries_the_participant_standing():
    copy = policy.DEFAULT_SHARE_COPY
    ranked = policy.share_text(
        copy, {"rank": 12, "total": 2431, "points": 1240})
    assert "#12" in ranked and "2,431" in ranked and "1,240" in ranked
    unranked = policy.share_text(copy, {"rank": 0})
    assert "#" not in unranked


def test_share_channels_build_real_targets_and_never_a_recipient():
    channels = policy.share_channels("hello", "https://forkmesh.com/r/a")
    by_id = {entry["id"]: entry for entry in channels}
    assert by_id["x"]["url"].startswith("https://x.com/intent/tweet?")
    assert "forkmesh.com" in unquote(by_id["x"]["url"])
    # copy/native/qr are client-owned and carry no URL to open.
    for local in ("copy", "native", "qr", "discord", "story"):
        assert "url" not in by_id[local]
    # No channel template can address a person.
    for entry in channels:
        assert "to=" not in entry.get("url", "")
        assert "phone" not in entry.get("url", "")
    # The mobile-only channel is absent on desktop.
    assert "sms" not in by_id
    assert "sms" in {
        entry["id"] for entry in
        policy.share_channels("hi", "https://forkmesh.com/r/a", mobile=True)}


def test_mastodon_instance_is_validated_not_trusted():
    assert policy.mastodon_share_url(
        "mastodon.social", "hi", "u").startswith("https://mastodon.social/share")
    assert policy.mastodon_share_url("https://hachyderm.io/", "hi", "u")
    for bogus in ("", "javascript:alert(1)", "not a host", "localhost",
                  "evil.com/path?x"):
        assert policy.mastodon_share_url(bogus, "hi", "u") == ""


def test_config_validator_refuses_anything_that_could_mint_value():
    config = policy.default_config()
    assert policy.validate_config(config)["theme"] == config["theme"]
    assert policy.validate_config(json.dumps(config))["tracks"]

    def refused(**patch):
        broken = policy.default_config()
        broken.update(patch)
        with pytest.raises(policy.ConfigError):
            policy.validate_config(broken)

    refused(missions=[{"key": "x", "metric": "nope", "threshold": 1}])
    refused(missions=[{"key": "x", "metric": "shares", "threshold": 1,
                       "reward": {"kind": "pro_months", "months": 3}}])
    refused(missions=[{"key": "x", "metric": "referrals_joined",
                       "threshold": 1,
                       "reward": {"kind": "cash", "amount": 500}}])
    refused(missions=[{"key": "x", "metric": "referrals_joined",
                       "threshold": 1,
                       "reward": {"kind": "pro_months", "months": 99}}])
    refused(missions=[{"key": "x", "metric": "referrals_joined",
                       "threshold": 0}])
    refused(milestones=[{"key": "m", "metric": "nope", "threshold": 10}])
    refused(tracks=[])
    refused(tracks=[{"id": "Bad Id", "name": "x"}])
    refused(shareCopy={"unknown": "x"})


def test_no_reward_kind_can_pay_cash():
    assert "cash" not in policy.REWARD_KINDS
    for prize in policy.default_prizes():
        # The published prize table may *describe* cash, but the machine
        # fields a mission or milestone acts on never carry an amount.
        assert set(prize) >= {"id", "proMonths"}
    for mission in policy.default_config()["missions"]:
        assert mission["reward"]["kind"] in policy.REWARD_KINDS


def test_squad_goal_ladder_is_monotonic():
    tiers = policy.squad_goal_tiers()
    assert [tier["size"] for tier in tiers] == sorted(
        tier["size"] for tier in tiers)
    goal, percent = policy.squad_goal(37)
    assert goal["size"] == 50 and percent == 74
    assert policy.squad_goal(10_000) == (None, 100)


def test_onboarding_puts_the_first_share_before_the_finish():
    keys = [step["key"] for step in policy.ONBOARDING_STEPS]
    assert keys.index("invite") < keys.index("build")
    state = policy.onboarding_state(2, "preserve", {"referrals_joined": 0})
    assert state["step"] == 3 and state["invitesNeeded"] == 3
    assert policy.onboarding_state(
        3, "preserve", {"referrals_joined": 3})["step"] == 4


# ---------------------------------------------------------------------------
# Runtime harness
# ---------------------------------------------------------------------------

class FakeD1:
    def __init__(self):
        self.connection = sqlite3.connect(":memory:")
        self.connection.row_factory = sqlite3.Row
        for statement in SCHEMA_STATEMENTS:
            lowered = statement.lower()
            if ("hackathon" in lowered
                    or "create table if not exists repositories " in lowered
                    or "create table if not exists repo_first_hosted" in lowered
                    or "create table if not exists referral_click_audit" in lowered
                    or "referral_attributions" in lowered
                    or "create table if not exists role_grants" in lowered
                    or "on role_grants" in lowered
                    or "create table if not exists users " in lowered):
                self.connection.execute(statement)

    async def run(self, env, sql, *args):
        self.connection.execute(sql, args)
        self.connection.commit()

    async def first(self, env, sql, *args):
        row = self.connection.execute(sql, args).fetchone()
        self.connection.commit()
        return dict(row) if row is not None else None

    async def all(self, env, sql, *args):
        rows = [dict(row) for row in self.connection.execute(sql, args)]
        self.connection.commit()
        return rows

    def count(self, table, where="", *args):
        sql = "SELECT COUNT(*) FROM " + table + (
            (" WHERE " + where) if where else "")
        return self.connection.execute(sql, args).fetchone()[0]

    def scalar(self, sql, *args):
        row = self.connection.execute(sql, args).fetchone()
        return row[0] if row else None


class Headers:
    def __init__(self, values=None):
        self._values = {k.lower(): v for k, v in (values or {}).items()}

    def get(self, name):
        return self._values.get(name.lower())


class Request:
    def __init__(self, url, method="GET", headers=None, body=None):
        self.url = url
        self.method = method
        self.headers = Headers(headers)
        self._body = body

    async def text(self):
        return self._body or ""


class Response:
    def __init__(self, body="", status=200, headers=None):
        self.body = body
        self.status = status
        self.headers = dict(headers or {})

    async def json(self):
        return json.loads(self.body)


def body_of(response):
    return json.loads(response.body)


@pytest.fixture
def harness(monkeypatch):
    db = FakeD1()
    clock = {"now": NOW}
    env = SimpleNamespace(PUBLIC_BASE_URL="https://forkmesh.com")
    session = {"name": ""}
    admins = set()
    accounts = {}
    notifications = []
    badges = []
    ids = itertools.count(1)
    cache = {}

    spec = importlib.util.spec_from_file_location(
        "hackathon_runtime", SRC / "hackathon_runtime.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    async def blind_index(_env, value):
        return "bi:" + str(value)

    async def account_session_record(_env, _request, _data=None):
        name = session["name"]
        if not name:
            return "", None
        return await blind_index(None, name), accounts.get(name)

    async def account_row(_env, name):
        name = str(name or "").lower()
        return await blind_index(None, name), accounts.get(name)

    async def bounded_json_request(request, _limit=None):
        return json.loads(await request.text() or "{}")

    async def is_admin(_env, name):
        return str(name or "") in admins

    async def enqueue_notification(_env, recipient, kind, title, **kwargs):
        notifications.append(
            {"to": recipient, "kind": kind, "title": title, **kwargs})
        return True

    def json_response(data, status=200, cache_seconds=None,
                      cache_control=None, extra_headers=None):
        headers = {"content-type": "application/json"}
        if cache_control is not None:
            headers["cache-control"] = cache_control
        elif cache_seconds is not None:
            headers["cache-control"] = "public, max-age=%d" % cache_seconds
        headers.update(extra_headers or {})
        return Response(json.dumps(data), status=status, headers=headers)

    module._bind_runtime({
        "Date": SimpleNamespace(now=lambda: clock["now"]),
        "Response": Response,
        "json_response": json_response,
        "method_name": lambda request: request.method,
        "d1_all": db.all,
        "d1_first": db.first,
        "d1_run": db.run,
        "ensure_schema": lambda _env: asyncio.sleep(0),
        "blind_index": blind_index,
        "edge_cache_match": lambda key: asyncio.sleep(0, cache.get(key)),
        "edge_cache_put": lambda key, value: asyncio.sleep(
            0, cache.__setitem__(key, value)),
        "edge_cache_delete": lambda key: asyncio.sleep(
            0, cache.pop(key, None)),
        "edge_cache_match_media": lambda key, _type: asyncio.sleep(0, None),
        "_account_session_record": account_session_record,
        "_account_row": account_row,
        "_account_kind": lambda rec: (rec or {}).get("kind", "user"),
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "valid_node_name": lambda value: bool(
            re.fullmatch(r"[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?",
                         str(value or ""))),
        "MAX_NODE_NAME": 63,
        "_public_base_url": lambda _env, _request=None: "https://forkmesh.com",
        "_ap_origin": lambda _env, _request=None: "https://forkmesh.com",
        "_html_escape": lambda value: (
            str(value).replace("&", "&amp;").replace("<", "&lt;")
            .replace(">", "&gt;").replace('"', "&quot;")),
        "og_card": og_card,
        "_award_badge": lambda _env, name, name_bi, slug, granted_by="system": (
            asyncio.sleep(0, badges.append((name, name_bi, slug, granted_by)))),
        "enqueue_notification": enqueue_notification,
        "bounded_json_request": bounded_json_request,
        "_is_admin": is_admin,
        "_audit_sensitive_action": lambda *a, **k: asyncio.sleep(0),
        # Same shape as the real _ap_uuid (32 lowercase hex), and unique per
        # call, so ids that double as public tokens validate like production.
        "_ap_uuid": lambda: "%032x" % next(ids),
        "quote": quote,
        "unquote": unquote,
        "urlparse": urlparse,
        "parse_qs": parse_qs,
    })

    def account(name, verified=True, kind="user"):
        accounts[name] = {
            "name": name, "status": "active", "kind": kind,
            "email_verified": verified, "created_at": NOW,
        }
        return name

    def login(name):
        session["name"] = name

    def campaign(status="registration", slug="mesh-2026"):
        config = policy.default_config()
        db.connection.execute(
            "INSERT INTO hackathons (slug,title,status,config,starts_at,"
            "ends_at,created_at,updated_at) VALUES (?,?,?,?,?,?,?,?)",
            (slug, "ForkMesh Hackathon", status,
             json.dumps(config), NOW, NOW + 86_400_000, NOW, NOW))
        for entry in config["milestones"]:
            db.connection.execute(
                "INSERT INTO hackathon_milestones (hackathon_slug,key,metric,"
                "threshold,label,description,reward,unlocked_at) "
                "VALUES (?,?,?,?,?,?,?,0)",
                (slug, entry["key"], entry["metric"], entry["threshold"],
                 entry["label"], entry["description"],
                 json.dumps(entry["reward"])))
        db.connection.commit()
        return slug

    def request(path, method="GET", body=None):
        return Request(
            "https://forkmesh.com" + path, method,
            {"content-type": "application/json"},
            json.dumps(body) if body is not None else None)

    return SimpleNamespace(
        module=module, db=db, env=env, clock=clock, account=account,
        login=login, campaign=campaign, request=request,
        notifications=notifications, admins=admins, cache=cache,
        accounts=accounts, badges=badges,
    )


def run(coro):
    return asyncio.run(coro)


# ---------------------------------------------------------------------------
# Runtime behaviour
# ---------------------------------------------------------------------------

def test_draft_campaign_is_invisible_on_every_public_route(harness):
    module = harness.module
    harness.campaign(status="draft")
    harness.account("alice")
    harness.login("alice")

    campaign = run(module.campaign_handler(
        harness.env, harness.request("/api/hackathon")))
    assert body_of(campaign)["campaign"] is None

    board = run(module.board_handler(
        harness.env, harness.request("/api/hackathon/leaderboards/builders"),
        "builders"))
    assert body_of(board)["rows"] == []

    joined = run(module.join_handler(
        harness.env, harness.request("/api/hackathon/join", "POST", {})))
    assert joined.status == 404

    invite = run(module.invite_page_handler(
        harness.env, harness.request("/hackathon/join/alice"), "alice"))
    assert invite is None, "a draft campaign must render no invitation"
    assert harness.db.count("hackathon_participants") == 0


def test_a_relay_without_the_tables_reports_no_campaign(harness):
    # A deploy that lands the routes before the migration must degrade to
    # "no campaign" -- the same state a draft produces -- rather than 500.
    module = harness.module
    harness.db.connection.execute("DROP TABLE hackathons")
    harness.db.connection.commit()
    response = run(module.campaign_handler(
        harness.env, harness.request("/api/hackathon")))
    assert response.status == 200
    assert body_of(response)["campaign"] is None


def test_join_is_idempotent_and_scores_once(harness):
    module = harness.module
    harness.campaign()
    harness.account("alice")
    harness.login("alice")

    first = run(module.join_handler(
        harness.env, harness.request("/api/hackathon/join", "POST", {})))
    assert first.status == 201
    payload = body_of(first)
    assert payload["joined"] is True
    assert payload["you"]["points"] == (
        policy.point_value("joined") + policy.point_value("verified"))

    run(module.join_handler(
        harness.env, harness.request("/api/hackathon/join", "POST", {})))
    assert harness.db.count("hackathon_participants") == 1
    assert harness.db.scalar(
        "SELECT participant_count FROM hackathons") == 1
    assert harness.db.scalar(
        "SELECT points FROM hackathon_participants WHERE name='alice'") == (
        policy.point_value("joined") + policy.point_value("verified"))


def test_one_event_pays_once_however_often_its_hook_fires(harness):
    module = harness.module
    slug = harness.campaign()
    harness.account("alice")
    harness.login("alice")
    run(module.join_handler(
        harness.env, harness.request("/api/hackathon/join", "POST", {})))
    before = harness.db.scalar(
        "SELECT points FROM hackathon_participants WHERE name='alice'")

    for _ in range(4):
        run(module.on_first_mirror(harness.env, "bi:alice", "alice/repo"))
    assert harness.db.count(
        "hackathon_points", "kind='first_mirror'") == 1
    assert harness.db.scalar(
        "SELECT points FROM hackathon_participants WHERE name='alice'") == (
        before + policy.point_value("first_mirror"))

    # A repeatable kind pays per distinct event and never twice for one.
    run(module.on_pull_accepted(harness.env, "bi:alice", "a/b#1"))
    run(module.on_pull_accepted(harness.env, "bi:alice", "a/b#1"))
    run(module.on_pull_accepted(harness.env, "bi:alice", "a/b#2"))
    assert harness.db.count("hackathon_points", "kind='pull_accepted'") == 2
    assert slug


def test_signup_through_an_invite_joins_and_places_in_the_squad(harness):
    module = harness.module
    harness.campaign()
    harness.account("alice")
    harness.login("alice")
    run(module.join_handler(
        harness.env, harness.request("/api/hackathon/join", "POST", {})))

    harness.account("bob", verified=False)
    joined = run(module.on_signup(harness.env, "bob", "bi:bob", "alice"))
    assert joined == "mesh-2026"
    row = harness.db.first
    squad_id = harness.db.scalar(
        "SELECT squad_id FROM hackathon_participants WHERE name='bob'")
    assert squad_id
    assert harness.db.scalar(
        "SELECT owner_name FROM hackathon_squads WHERE squad_id=?",
        squad_id) == "alice"
    assert harness.db.scalar(
        "SELECT member_count FROM hackathon_squads WHERE squad_id=?",
        squad_id) == 2
    assert row  # the harness exposes a reader; silence the linter

    # Somebody with no referrer is never joined silently.
    harness.account("carol", verified=False)
    assert run(module.on_signup(harness.env, "carol", "bi:carol", "")) == ""
    assert harness.db.count(
        "hackathon_participants", "name='carol'") == 0


def test_verification_makes_a_friend_joined_but_not_qualified(harness):
    module = harness.module
    harness.campaign()
    harness.account("alice")
    harness.login("alice")
    run(module.join_handler(
        harness.env, harness.request("/api/hackathon/join", "POST", {})))
    harness.account("bob", verified=False)
    run(module.on_signup(harness.env, "bob", "bi:bob", "alice"))

    run(module.on_email_verified(harness.env, "bi:bob", "bob"))
    assert harness.db.scalar(
        "SELECT referrals_joined FROM hackathon_participants "
        "WHERE name='alice'") == 1
    # Nothing of value yet: qualification is the referral ledger's decision.
    assert harness.db.scalar(
        "SELECT referrals_qualified FROM hackathon_participants "
        "WHERE name='alice'") == 0
    assert harness.db.count(
        "hackathon_points", "kind='referral_qualified'") == 0
    assert any(note["kind"] == "hackathon_squad"
               and note["to"] == "alice"
               for note in harness.notifications)

    # Verifying twice neither double-counts nor re-notifies.
    before = len(harness.notifications)
    run(module.on_email_verified(harness.env, "bi:bob", "bob"))
    assert harness.db.scalar(
        "SELECT referrals_joined FROM hackathon_participants "
        "WHERE name='alice'") == 1
    assert len(harness.notifications) == before


def test_only_a_credited_referral_produces_qualified_points(harness):
    module = harness.module
    harness.campaign()
    harness.account("alice")
    harness.login("alice")
    run(module.join_handler(
        harness.env, harness.request("/api/hackathon/join", "POST", {})))
    harness.account("bob", verified=False)
    run(module.on_signup(harness.env, "bob", "bi:bob", "alice"))
    run(module.on_email_verified(harness.env, "bi:bob", "bob"))
    before = harness.db.scalar(
        "SELECT points FROM hackathon_participants WHERE name='alice'")

    assert run(module.on_referral_credited(
        harness.env, "bi:alice", "alice", "bi:bob")) is True
    # A second credit for the same referred account pays nothing.
    assert run(module.on_referral_credited(
        harness.env, "bi:alice", "alice", "bi:bob")) is False
    assert harness.db.scalar(
        "SELECT referrals_qualified FROM hackathon_participants "
        "WHERE name='alice'") == 1
    assert harness.db.scalar(
        "SELECT points FROM hackathon_participants WHERE name='alice'") == (
        before + policy.point_value("referral_qualified"))


def test_share_presses_are_measured_and_worth_nothing(harness):
    module = harness.module
    harness.campaign()
    harness.account("alice")
    harness.login("alice")
    run(module.join_handler(
        harness.env, harness.request("/api/hackathon/join", "POST", {})))
    before = harness.db.scalar(
        "SELECT points FROM hackathon_participants WHERE name='alice'")

    first = run(module.share_handler(harness.env, harness.request(
        "/api/hackathon/share", "POST", {"channel": "x", "surface": "hq"})))
    assert body_of(first)["counted"] is True
    # Same channel, same minute: counted once, so a jammed button cannot
    # inflate the one ratio the whole feature is measured by.
    again = run(module.share_handler(harness.env, harness.request(
        "/api/hackathon/share", "POST", {"channel": "x"})))
    assert body_of(again)["counted"] is False
    harness.clock["now"] += 61_000
    later = run(module.share_handler(harness.env, harness.request(
        "/api/hackathon/share", "POST", {"channel": "x"})))
    assert body_of(later)["counted"] is True

    assert harness.db.scalar(
        "SELECT points FROM hackathon_participants WHERE name='alice'") == before
    assert harness.db.count("hackathon_points", "kind LIKE 'share%'") == 0

    unknown = run(module.share_handler(harness.env, harness.request(
        "/api/hackathon/share", "POST", {"channel": "carrier-pigeon"})))
    assert unknown.status == 400


def test_share_events_hold_no_recipient(harness):
    columns = {
        row[1] for row in harness.db.connection.execute(
            "PRAGMA table_info(hackathon_share_events)")
    }
    assert columns == {
        "hackathon_slug", "account_bi", "channel", "surface",
        "minute_bucket", "created_at",
    }
    for forbidden in ("recipient", "email", "phone", "address", "message",
                      "contact", "url", "ip"):
        assert not any(forbidden in column for column in columns)


def test_node_sessions_cannot_participate(harness):
    module = harness.module
    harness.campaign()
    harness.account("mirror29", kind="node")
    harness.login("mirror29")
    response = run(module.join_handler(
        harness.env, harness.request("/api/hackathon/join", "POST", {})))
    assert response.status == 401
    assert harness.db.count("hackathon_participants") == 0


def test_boards_are_bounded_public_and_separate_pending_from_credited(harness):
    module = harness.module
    slug = harness.campaign()
    for index in range(60):
        name = "user%d" % index
        harness.db.connection.execute(
            "INSERT INTO hackathon_participants (hackathon_slug,account_bi,"
            "name,referrer_bi,referrer_name,squad_id,track,points,"
            "onboarding_step,share_count,referrals_joined,"
            "referrals_qualified,joined_at,verified_at) "
            "VALUES (?,?,?,'','','','',?,1,0,?,?,?,0)",
            (slug, "bi:" + name, name, index * 10, index, max(0, index - 5),
             NOW + index))
    harness.db.connection.commit()

    builders = body_of(run(module.board_handler(
        harness.env, harness.request("/api/hackathon/leaderboards/builders"),
        "builders")))
    assert len(builders["rows"]) == policy.BOARD_LIMIT
    assert builders["rows"][0]["points"] > builders["rows"][-1]["points"]
    assert set(builders["rows"][0]) == {"name", "points", "track"}

    recruiters = body_of(run(module.board_handler(
        harness.env, harness.request("/api/hackathon/leaderboards/recruiters"),
        "recruiters")))
    top = recruiters["rows"][0]
    assert top["joined"] >= top["qualified"], (
        "pending must never be reported as credited")
    assert set(top) == {"name", "qualified", "joined", "points"}

    assert run(module.board_handler(
        harness.env, harness.request("/api/hackathon/leaderboards/nope"),
        "nope")).status == 404


def test_invite_page_shows_the_inviter_and_escapes_everything(harness):
    module = harness.module
    harness.campaign()
    harness.account("alice")
    harness.login("alice")
    run(module.join_handler(
        harness.env, harness.request("/api/hackathon/join", "POST", {})))

    page = run(module.invite_page_handler(
        harness.env, harness.request("/hackathon/join/alice"), "alice"))
    assert page.status == 200
    assert "text/html" in page.headers["content-type"]
    assert "alice invited you to build" in page.body
    assert 'property="og:image"' in page.body
    assert "/api/hackathon/cards/invite/alice.png" in page.body
    # Accepting goes through the ordinary counted referral link, so the
    # signed one-time claim and the attribution ledger still apply.
    assert 'href="/r/alice"' in page.body
    assert "<script" not in page.body

    # An account that is not a participant has no invitation to render.
    harness.account("mallory")
    redirected = run(module.invite_page_handler(
        harness.env, harness.request("/hackathon/join/mallory"), "mallory"))
    assert redirected.status == 302
    assert redirected.headers["location"] == "/hackathon"


def test_invite_page_escapes_a_hostile_campaign_title(harness):
    module = harness.module
    harness.campaign()
    harness.db.connection.execute(
        "UPDATE hackathons SET title=?",
        ('<img src=x onerror="alert(1)">',))
    harness.db.connection.commit()
    harness.account("alice")
    harness.login("alice")
    run(module.join_handler(
        harness.env, harness.request("/api/hackathon/join", "POST", {})))
    page = run(module.invite_page_handler(
        harness.env, harness.request("/hackathon/join/alice"), "alice"))
    # The payload survives only as inert text: no tag is opened and no
    # attribute is broken out of. (The page's own logo <img> is legitimate,
    # so the assertion names the injected markup rather than the element.)
    assert "<img src=x" not in page.body
    assert 'onerror="alert' not in page.body
    assert "&lt;img src=x onerror=&quot;alert(1)&quot;&gt;" in page.body


def test_me_requires_a_session_and_never_caches(harness):
    module = harness.module
    harness.campaign()
    anonymous = run(module.me_handler(
        harness.env, harness.request("/api/hackathon/me")))
    assert anonymous.status == 401
    assert anonymous.headers["cache-control"] == "no-store"

    harness.account("alice")
    harness.login("alice")
    before_join = run(module.me_handler(
        harness.env, harness.request("/api/hackathon/me")))
    assert body_of(before_join)["joined"] is False

    run(module.join_handler(
        harness.env, harness.request("/api/hackathon/join", "POST", {})))
    joined = body_of(run(module.me_handler(
        harness.env, harness.request("/api/hackathon/me"))))
    assert joined["share"]["link"] == "https://forkmesh.com/hackathon/join/alice"
    assert joined["missions"]
    assert joined["onboarding"]["step"] >= 1
    assert joined["you"]["rank"] == 1


def test_track_choice_is_validated_against_the_campaign(harness):
    module = harness.module
    harness.campaign()
    harness.account("alice")
    harness.login("alice")
    run(module.join_handler(
        harness.env, harness.request("/api/hackathon/join", "POST", {})))

    run(module.me_handler(harness.env, harness.request(
        "/api/hackathon/me", "POST", {"track": "preserve"})))
    assert harness.db.scalar(
        "SELECT track FROM hackathon_participants WHERE name='alice'") == (
        "preserve")
    run(module.me_handler(harness.env, harness.request(
        "/api/hackathon/me", "POST", {"track": "'; DROP TABLE users; --"})))
    assert harness.db.scalar(
        "SELECT track FROM hackathon_participants WHERE name='alice'") == (
        "preserve")


def test_squad_page_and_card_render_for_a_real_squad(harness):
    module = harness.module
    harness.campaign()
    harness.account("alice")
    harness.login("alice")
    run(module.join_handler(
        harness.env, harness.request("/api/hackathon/join", "POST", {})))
    harness.account("bob", verified=False)
    run(module.on_signup(harness.env, "bob", "bi:bob", "alice"))
    code = harness.db.scalar("SELECT code FROM hackathon_squads")
    assert code == "alice"

    page = run(module.squad_page_handler(
        harness.env, harness.request("/hackathon/squad/" + code), code))
    assert page.status == 200
    assert "alice&#x27;s squad" in page.body or "alice's squad" in page.body
    assert "<script" not in page.body

    data = body_of(run(module.squad_handler(
        harness.env, harness.request("/api/hackathon/squads/" + code), code)))
    assert data["squad"]["members"] == 2
    assert {entry["name"] for entry in data["squad"]["roster"]} == {
        "alice", "bob"}

    assert run(module.squad_page_handler(
        harness.env, harness.request("/hackathon/squad/nope"), "nope")) is None


def test_squad_points_equal_the_sum_of_its_members(harness):
    # The Squads board ranks on this cached total, so it must not drift: a
    # squad is seeded with its owner's existing points, and a joiner's first
    # points land after they have been placed in it.
    module = harness.module
    slug = harness.campaign()
    harness.account("alice")
    harness.login("alice")
    run(module.join_handler(
        harness.env, harness.request("/api/hackathon/join", "POST", {})))
    for name in ("bob", "carol"):
        harness.account(name, verified=False)
        run(module.on_signup(harness.env, name, "bi:" + name, "alice"))
        run(module.on_email_verified(harness.env, "bi:" + name, name))

    squad_points = harness.db.scalar("SELECT points FROM hackathon_squads")
    member_points = harness.db.scalar(
        "SELECT SUM(points) FROM hackathon_participants "
        "WHERE hackathon_slug=? AND squad_id<>''", slug)
    assert squad_points == member_points
    assert squad_points > 0
    assert harness.db.scalar(
        "SELECT member_count FROM hackathon_squads") == 3


def test_two_owners_who_truncate_alike_still_get_distinct_squad_codes(harness):
    # A squad code is the owner's username truncated to eight characters, so
    # two long similar names collide. The second squad must still get a
    # reachable code, or its page would 404 for everyone in it.
    module = harness.module
    slug = harness.campaign()
    now = 1
    first = run(module._ensure_squad(
        harness.env, slug, "bi:a", "sarahjohnson", now))
    second = run(module._ensure_squad(
        harness.env, slug, "bi:b", "sarahjohnston", now))
    assert first and second and first != second
    codes = [row["code"] for row in harness.db.connection.execute(
        "SELECT code FROM hackathon_squads ORDER BY rowid")]
    assert codes == ["sarahjoh", "sarahjoh2"]
    for code in codes:
        assert policy.SQUAD_CODE_RE.match(code), code


def test_admin_surface_is_gated_and_validates_before_it_writes(harness):
    module = harness.module
    harness.account("alice")
    harness.login("alice")
    denied = run(module.admin_handler(harness.env, harness.request(
        "/api/admin/hackathon", "POST", {"slug": "mesh-2026"})))
    assert denied.status == 403
    assert harness.db.count("hackathons") == 0

    harness.admins.add("alice")
    bad = run(module.admin_handler(harness.env, harness.request(
        "/api/admin/hackathon", "POST", {
            "slug": "mesh-2026",
            "config": {"missions": [{"key": "x", "metric": "shares",
                                     "threshold": 1,
                                     "reward": {"kind": "pro_months",
                                                "months": 12}}]}})))
    assert bad.status == 400
    assert body_of(bad)["error"] == "invalid_config"
    assert harness.db.count("hackathons") == 0

    created = run(module.admin_handler(harness.env, harness.request(
        "/api/admin/hackathon", "POST", {
            "slug": "mesh-2026", "title": "ForkMesh Hackathon",
            "status": "registration", "startsAt": NOW,
            "endsAt": NOW + 86_400_000})))
    assert created.status == 200
    assert harness.db.scalar(
        "SELECT status FROM hackathons WHERE slug='mesh-2026'") == (
        "registration")
    assert harness.db.count("hackathon_milestones") == len(
        policy.DEFAULT_MILESTONES)

    bad_slug = run(module.admin_handler(harness.env, harness.request(
        "/api/admin/hackathon", "POST", {"slug": "Not A Slug"})))
    assert bad_slug.status == 400


def test_campaign_payload_is_bounded_and_cached(harness):
    module = harness.module
    harness.campaign()
    response = run(module.campaign_handler(
        harness.env, harness.request("/api/hackathon")))
    payload = body_of(response)["campaign"]
    assert set(payload) == {
        "slug", "title", "status", "theme", "startsAt", "endsAt",
        "judgingEndsAt", "joinable", "tracks", "prizes", "participants",
        "verified", "squads", "submissions", "nextMilestone", "milestones",
    }
    assert "max-age" in response.headers["cache-control"]
    assert harness.cache, "the campaign read must be edge-cached"


def test_api_dispatch_covers_the_whole_subtree(harness):
    module = harness.module
    harness.campaign()
    for path, expected in (
            ("/api/hackathon", 200),
            ("/api/hackathon/leaderboards/builders", 200),
            ("/api/hackathon/me", 401),
            ("/api/hackathon/nonsense", 404),
    ):
        response = run(module.api_handler(
            harness.env, harness.request(path), path))
        assert response.status == expected, path


def test_hackathon_card_renders_a_png():
    png = og_card.render_hackathon_card({
        "kind": "invite", "title": "ForkMesh Hackathon", "name": "alice",
        "headline": "@alice invites you to build",
        "cells": (("RANK", "#7"), ("POINTS", 1240), ("BUILDERS", 2431)),
        "host": "forkmesh.com", "path": "/hackathon/join/alice",
    })
    assert png[:8] == b"\x89PNG\r\n\x1a\n"
    assert len(png) > 1000


# ---------------------------------------------------------------------------
# Wiring and routing contracts
# ---------------------------------------------------------------------------

def test_hooks_are_wired_into_the_paths_that_own_each_event():
    # Signup, both verification paths, an accepted pull and a first mirror.
    assert "_hackathon.on_signup(" in ENTRY_TEXT
    assert ENTRY_TEXT.count("_hackathon.on_email_verified(") == 2
    assert "hackathon.on_pull_accepted(" in ENTRY_TEXT
    assert "hackathon.on_first_mirror(" in CATALOG_TEXT
    # Qualified-referral points exist in exactly one place: the referral
    # ledger's own credit path.
    assert ACCOUNT_USERS_TEXT.count("on_referral_credited(") == 1
    assert "on_referral_credited" not in ENTRY_TEXT
    # Every hook is best effort, so campaign bookkeeping can never fail an
    # account creation, a verification, a pull or a publish.
    for source, name in (
            (ENTRY_TEXT, "_hackathon.on_signup("),
            (ACCOUNT_USERS_TEXT, "on_referral_credited("),
            (CATALOG_TEXT, "hackathon.on_first_mirror("),
    ):
        index = source.index(name)
        assert "try:" in source[max(0, index - 400):index], name
        assert "except Exception:" in source[index:index + 400], name


def test_runtime_module_is_lazy_like_every_other_route_domain():
    assert '_hackathon = _LazyModule("hackathon_runtime")' in ENTRY_TEXT
    # The pure policy module must stay free of Worker/D1 imports so it runs
    # under plain CPython, exactly like badges.py and microjobs.py.
    source = (SRC / "hackathon.py").read_text(encoding="utf-8")
    tree = ast.parse(source)
    imported = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            imported.update(alias.name.split(".")[0] for alias in node.names)
        elif isinstance(node, ast.ImportFrom) and node.module:
            imported.add(node.module.split(".")[0])
    assert imported <= {"json", "re", "urllib"}, imported
    body = code_only(source, "#")
    for forbidden in ("import js\n", "from js import", "from workers import",
                      "d1_run(", "await "):
        assert forbidden not in body, forbidden


def test_routes_are_registered_and_ordered_before_the_repository_shortcut():
    assert "HACKATHON_API_RE" in RUNTIME_TEXT
    assert "hackathon_api_handler(" in RUNTIME_TEXT
    assert "hackathon_admin_handler(" in RUNTIME_TEXT
    # /hackathon/join/<name> must be matched before the generic two-segment
    # repository shortcut, or it would be read as owner/repo.
    assert (RUNTIME_TEXT.index("HACKATHON_INVITE_RE")
            < RUNTIME_TEXT.index("REFERRAL_LINK_RE.match"))
    assert "hackathon" in static_routes.RESERVED_ROUTE_PREFIXES


def test_static_route_ownership_splits_the_subtree_correctly():
    assert static_routes.external_site_route("/hackathon") == "www"
    assert static_routes.external_site_route(
        "/hackathon/leaderboard") == "www"
    assert static_routes.external_site_route("/hackathon/rules") == "www"
    # Everything with live data stays with the Worker that has the data.
    assert static_routes.external_site_route("/hackathon/me") is None
    assert static_routes.external_site_route("/hackathon/join/alice") is None
    assert static_routes.external_site_route("/hackathon/squad/alice") is None
    assert not static_routes.looks_like_repo_route("/hackathon/me")
    assert not static_routes.looks_like_repo_route("/hackathon/join/alice")


def test_the_hq_page_is_a_composed_dashboard_page():
    page = dashboard_shell.PAGES["hackathon"]
    assert page["route"] == "/hackathon/me"
    assert static_routes.DASHBOARD_PAGE_ASSETS["/hackathon/me"] == page["asset"]
    assert ("/" + page["asset"]) in static_routes.BLOCKED_STATIC_HTML_PATHS
    assert (ROOT / "public" / page["asset"]).is_file()
    assert "09-hackathon.js" in dashboard_bundle.FRAGMENTS
    assert list(dashboard_bundle.FRAGMENTS) == sorted(
        dashboard_bundle.FRAGMENTS)
    built = (ROOT / "public" / page["asset"]).read_text(encoding="utf-8")
    assert 'data-view="hackathon"' in built
    assert 'data-page="hackathon"' in built


def test_worker_configuration_keeps_the_invite_link_on_the_apex():
    # The App owns the subtree except the composed HQ page.
    for line in ('  "/hackathon",', '  "/hackathon/*",',
                 '  "!/hackathon/me",'):
        assert line in APP_WRANGLER, line
    # The marketing Worker runs first for the landing page (attribution) and
    # for the two documents it proxies.
    for line in ('  "/hackathon",', '  "/hackathon/join/*",',
                 '  "/hackathon/squad/*",'):
        assert line in WWW_WRANGLER, line
    # Proxied, not redirected: a shared invite link keeps forkmesh.com.
    assert "HACKATHON_APP_DOCUMENTS" in WWW_WORKER
    assert "proxyToApp(request, env)" in WWW_WORKER
    assert ("/hackathon/me https://app.forkmesh.com/hackathon/me 308"
            in REDIRECTS)


def test_the_landing_page_reaches_the_pages_this_feature_serves():
    # /hackathon itself is main's page and is not styled from here. What this
    # feature does own is the contract between that page and the runtime:
    # every place the landing page sends a visitor must exist.
    assert 'href="/hackathon/rules"' in LANDING
    # The revenue contest sends people to the Patreon link on the homepage.
    assert 'href="/#support-forkmesh"' in LANDING
    assert 'id="support-forkmesh"' in (WWW_PUBLIC / "index.html").read_text(
        encoding="utf-8")
    assert "/hackathon/me" in static_routes.DASHBOARD_PAGE_ASSETS
    assert (WWW_PUBLIC / "hackathon" / "leaderboard.html").is_file()
    assert (WWW_PUBLIC / "hackathon" / "rules.html").is_file()


def test_subpages_reuse_the_landing_design_system():
    for page in (BOARD_PAGE, PROJECTS_PAGE, RULES_PAGE):
        assert 'data-theme-default="light"' in page
        assert '<link rel="stylesheet" href="/hackathon.css" />' in page
        assert 'src="/home-theme.js' in page
        assert 'data-forkmesh-footer="landing"' in page
        assert 'class="home-nav-shell"' in page
    # The exact tokens, fonts and component radii the homepage uses.
    for token in ("--landing-heading-font", "--landing-hero-font",
                  "--home-surface-2", "--home-shadow-raised"):
        assert token in LANDING_CSS, token
    assert "ABCFavorit" in LANDING_CSS and "Domine" in LANDING_CSS
    assert "#1a7f37" in LANDING_CSS
    assert "border-radius: 1.125rem" in LANDING_CSS
    # Both themes are defined at token level, never inside a component rule.
    assert LANDING_CSS.index(":root {") < LANDING_CSS.index("html.light {")
    assert "prefers-reduced-motion" in LANDING_CSS


def test_landing_page_states_the_rules_that_keep_the_contest_honest():
    # Every term that decides who gets paid is stated on the page that
    # advertises the money, not only in the linked rules.
    text = " ".join(re.sub(r"<[^>]+>", "", LANDING).lower().split())
    assert "15 september 2026, 00:00 utc" in text
    assert "15 october 2026, 23:59 utc" in text
    assert "50% of each eligible customer’s first month" in text
    assert "$10 plan" in text
    assert "$50 plan" in text
    assert "$25 plan" not in text
    assert "not number of signups" in text
    assert "maximum of 10 companies" in text
    assert "no self-referrals" in text
    assert "refunds, chargebacks" in text
    assert "30 days" in text
    assert "minimum $200 in eligible first-month revenue" in text
    assert "minimum $100 in eligible first-month revenue" in text
    assert "minimum $50 in eligible first-month revenue" in text
    assert "at least 3 different eligible customers" in text
    assert "own payment method" in text
    # The rules page is linked from the page that advertises the prizes.
    assert '/hackathon/rules' in LANDING


def test_prize_minimums_stop_one_small_payment_winning_a_prize():
    text = " ".join(re.sub(r"<[^>]+>", "", RULES_PAGE).lower().split())
    for prize, minimum in (("$100", "$200"), ("$50", "$100"), ("$25", "$50")):
        assert (prize + ", with a minimum of " + minimum
                + " in eligible first-month revenue") in text
    assert "at least 3 different eligible customers" in text
    assert "never more than half of the revenue" in text
    assert "not awarded" in text
    assert "made or funded by the entrant" in text
    assert "last updated 1 october 2026" in text


def test_landing_script_degrades_to_truthful_static_copy():
    # A draft (or unreachable) campaign must never read as open registration.
    assert "Dates coming soon" in LANDING_JS
    # An unreachable API is treated exactly like no published campaign:
    # the page never points somebody at a join flow it could not confirm.
    assert "applyJoinTargets(null, false)" not in LANDING_JS
    assert LANDING_JS.count("applyJoinTargets(null, true)") == 1
    # Other people's usernames are rendered as text, never as markup.
    assert "innerHTML" not in code_only(LANDING_JS)
    assert "textContent = who" in LANDING_JS
    assert 'credentials: "omit"' in LANDING_JS


def test_hq_separates_joined_from_qualified_and_never_pays_for_sharing():
    assert "Friends joined" in HACKATHON_JS
    assert "Qualified" in HACKATHON_JS
    assert "Credited by the referral ledger" in HACKATHON_JS
    # The share reporter is fire-and-forget and carries no recipient.
    assert 'hackathonPost("/api/hackathon/share", { channel, surface })' in (
        HACKATHON_JS)
    body = code_only(HACKATHON_JS)
    for forbidden in ("recipient", "contacts", "navigator.contacts"):
        assert forbidden not in body, forbidden


def test_badges_exist_for_every_recognition_the_pages_promise():
    import badges

    slugs = {entry["slug"] for entry in badges.public_catalog()}
    assert {"hackathon_builder", "hackathon_connector", "hackathon_finalist",
            "hackathon_winner"} <= slugs
    for mission in policy.default_config()["missions"]:
        badge = mission["reward"].get("badge")
        if badge:
            assert badge in slugs, badge
