"""Self-serve plan usage, trial register, storage, and wiring contracts."""

import asyncio
import re
import sqlite3
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import plan_services as plans
import schema
import urls


NOW = 1_800_000_000_000
DAY_MS = 24 * 60 * 60 * 1000


class Runtime:
    """Narrow fake of the entry.py adapter plan_services runs against."""

    def __init__(self, method="GET", body=None, session=("account-bi", {"name": "Alice"})):
        self._method = method
        self._body = body if body is not None else {}
        self._session = session
        self.first_calls = []
        self.statements = []
        self.rows = {}
        # Membership and promotion are the same query over two grant scopes.
        self.grants = {}

    def method(self):
        return self._method

    def now(self):
        return NOW

    def response(self, data, status=200, cache_control=None, extra_headers=None):
        return {"data": data, "status": status, "headers": extra_headers or {}}

    async def ensure_schema(self):
        return None

    async def json_body(self, _limit):
        return dict(self._body), ""

    async def session(self, _data):
        return self._session

    async def blind(self, value):
        return "bi:" + str(value)

    async def d1_first(self, sql, *args):
        self.first_calls.append((sql, args))
        if "FROM role_grants" in sql:
            return self.grants.get(args[1] if len(args) > 1 else "")
        for marker, row in self.rows.items():
            if marker in sql:
                return row
        return None

    async def batch(self, statements):
        self.statements = statements


def test_usage_requires_a_session_and_reports_enforced_limits():
    anonymous = Runtime(session=("", None))
    denied = asyncio.run(plans.handle(anonymous, "usage"))
    assert denied["status"] == 401

    runtime = Runtime()
    runtime.rows["FROM repositories"] = {
        "repositories": 3, "orgs": 1, "keys": 2, "notes": 40}
    response = asyncio.run(plans.handle(runtime, "usage"))
    assert response["status"] == 200
    meters = response["data"]["meters"]
    assert meters["repositories"] == {
        "used": 3, "limit": 100, "label": "Published repositories"}
    assert meters["organizationsOwned"]["used"] == 1
    assert meters["organizationsOwned"]["limit"] == 10
    assert meters["sshKeys"] == {"used": 2, "limit": 20, "label": "SSH keys"}
    assert meters["storedNotifications"]["used"] == 40
    assert meters["storedNotifications"]["limit"] == 500
    assert response["data"]["limitsPlanIndependent"] is True
    # Catalog rows key on the blind of the lowercase owner name; everything
    # else keys on the session's account_bi.
    sql, args = next(call for call in runtime.first_calls if "COUNT(*)" in call[0])
    assert args == ("bi:alice", "account-bi", "account-bi", "account-bi")

    wrong_method = Runtime(method="POST")
    blocked = asyncio.run(plans.handle(wrong_method, "usage"))
    assert blocked["status"] == 405
    assert blocked["headers"]["allow"] == "GET"


def test_reported_limits_match_the_constants_their_routes_enforce():
    entry = (SRC / "entry.py").read_text()
    ssh = (SRC / "ssh_keys.py").read_text()

    def constant(text, name):
        return int(re.search(name + r"\s*=\s*(\d+)", text).group(1))

    assert plans.USAGE_METERS["repositories"][0] == constant(
        entry, "CATALOG_MAX_RECORDS_PER_OWNER")
    assert plans.USAGE_METERS["organizationsOwned"][0] == constant(
        entry, "MAX_ORGS_PER_ACCOUNT")
    assert plans.USAGE_METERS["sshKeys"][0] == constant(
        ssh, "MAX_KEYS_PER_ACCOUNT")
    assert plans.USAGE_METERS["storedNotifications"][0] == constant(
        entry, "MAX_NOTIFICATIONS_PER_RECIPIENT")


def test_trial_status_reports_eligibility_used_and_active_states():
    fresh = Runtime()
    response = asyncio.run(plans.handle(fresh, "trial"))
    assert response["status"] == 200
    assert response["data"]["eligible"] is True
    assert response["data"]["used"] is False
    assert response["data"]["durationDays"] == 14

    running = Runtime()
    running.rows["FROM plan_trials"] = {
        "tier": "supporter", "started_at": NOW - DAY_MS,
        "ends_at": NOW + 13 * DAY_MS}
    state = asyncio.run(plans.handle(running, "trial"))["data"]
    assert state["used"] and state["active"] and not state["eligible"]
    assert state["endsAt"] == NOW + 13 * DAY_MS

    subscribed = Runtime()
    subscribed.grants["platform"] = {"pro": NOW + 30 * DAY_MS, "supporter": None}
    assert asyncio.run(
        plans.handle(subscribed, "trial"))["data"]["eligible"] is False


def test_trial_start_writes_register_and_promotion_grant_atomically():
    runtime = Runtime(method="POST", body={"sessionToken": "token"})
    response = asyncio.run(plans.handle(runtime, "trial"))
    assert response["status"] == 201
    assert response["data"] == {
        "ok": True, "tier": "supporter",
        "startedAt": NOW, "endsAt": NOW + 14 * DAY_MS,
    }
    assert len(runtime.statements) == 2
    register_sql, register_args = runtime.statements[0]
    grant_sql, grant_args = runtime.statements[1]
    assert register_sql.startswith("INSERT INTO plan_trials")
    assert "ON CONFLICT" not in register_sql
    assert register_args == ("account-bi", "supporter", NOW, NOW + 14 * DAY_MS)
    # The entitlement is the provider-proof promotion lane, never the platform
    # scope a payment provider recomputes on every sync.
    assert "'promotion'" in grant_sql
    assert "'platform'" not in grant_sql
    assert "'supporting_member'" in grant_sql
    assert grant_args == ("account-bi", "bi:plan-trial", NOW, NOW + 14 * DAY_MS)


def test_trial_start_rejections_are_specific_and_write_nothing():
    used = Runtime(method="POST")
    used.rows["FROM plan_trials"] = {"ends_at": NOW - DAY_MS}
    repeat = asyncio.run(plans.handle(used, "trial"))
    assert repeat["status"] == 409
    assert repeat["data"] == {
        "error": "trial_already_used", "endsAt": NOW - DAY_MS}
    assert used.statements == []

    paying = Runtime(method="POST")
    paying.grants["platform"] = {"pro": None, "supporter": NOW + DAY_MS}
    assert asyncio.run(plans.handle(paying, "trial"))["data"] == {
        "error": "already_subscribed"}
    assert paying.statements == []

    assert asyncio.run(plans.handle(
        Runtime(method="POST", body={"tier": "pro"}), "trial"),
    )["data"] == {"error": "invalid_tier"}
    assert asyncio.run(plans.handle(
        Runtime(method="POST", body={"upgrade": True}), "trial"),
    )["data"] == {"error": "unsupported_trial_field"}
    assert asyncio.run(plans.handle(
        Runtime(method="POST", session=("", None)), "trial"))["status"] == 401
    assert asyncio.run(plans.handle(Runtime(), "unknown"))["status"] == 404


def _trial_db():
    db = sqlite3.connect(":memory:")
    db.execute("CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL)")
    db.executescript((ROOT / "migrations" / "0184_plan_trials.sql").read_text())
    db.execute("""CREATE TABLE role_grants (
        account_bi TEXT NOT NULL, role TEXT NOT NULL, scope_type TEXT NOT NULL,
        scope_bi TEXT NOT NULL, granted_by_bi TEXT, granted_at INTEGER NOT NULL,
        expires_at INTEGER NOT NULL, revoked_at INTEGER NOT NULL,
        PRIMARY KEY (account_bi, role, scope_type, scope_bi))""")
    return db


def test_trial_sql_enforces_one_trial_and_never_shortens_a_prize():
    db = _trial_db()
    # A hackathon prize in the same promotion scope type but its own scope
    # subject must be untouched by a trial.
    prize_end = NOW + 90 * DAY_MS
    db.execute(
        "INSERT INTO role_grants VALUES(?,?,?,?,?,?,?,0)",
        ("account-bi", "supporting_member", "promotion", "bi:hackathon:one",
         "bi:hackathon:one", NOW - DAY_MS, prize_end))

    runtime = Runtime(method="POST")
    asyncio.run(plans.handle(runtime, "trial"))
    for sql, args in runtime.statements:
        db.execute(sql, args)
    db.commit()

    assert db.execute("SELECT tier,started_at,ends_at FROM plan_trials").fetchall() == [
        ("supporter", NOW, NOW + 14 * DAY_MS)]
    grants = db.execute(
        "SELECT scope_bi,expires_at,revoked_at FROM role_grants "
        "ORDER BY scope_bi").fetchall()
    assert grants == [
        ("bi:hackathon:one", prize_end, 0),
        ("bi:plan-trial", NOW + 14 * DAY_MS, 0),
    ]

    # The register's primary key is the race arbiter: a duplicate start aborts.
    second = Runtime(method="POST")
    asyncio.run(plans.handle(second, "trial"))
    try:
        db.execute(*second.statements[0])
        raise AssertionError("a second trial insert was accepted")
    except sqlite3.IntegrityError:
        pass

    # Replaying the grant upsert against a revoked row re-grants cleanly.
    db.execute(
        "UPDATE role_grants SET revoked_at=? WHERE scope_bi='bi:plan-trial'",
        (NOW,))
    db.execute(*runtime.statements[1])
    assert db.execute(
        "SELECT expires_at,revoked_at FROM role_grants "
        "WHERE scope_bi='bi:plan-trial'").fetchone() == (NOW + 14 * DAY_MS, 0)
    db.close()


def test_schema_migration_routes_lifecycle_and_browser_surfaces_are_wired():
    joined_schema = "\n".join(schema.SCHEMA_STATEMENTS)
    assert "CREATE TABLE IF NOT EXISTS plan_trials" in joined_schema

    assert urls.PLAN_SERVICES_RE.fullmatch("/api/plans/usage")
    assert urls.PLAN_SERVICES_RE.fullmatch("/api/plans/trial/")
    assert not urls.PLAN_SERVICES_RE.fullmatch("/api/plans/checkout")

    entry = (SRC / "entry.py").read_text()
    app_runtime = (SRC / "app_runtime.py").read_text()
    assert "plan_services_handler" in entry
    assert '"PLAN_SERVICES_RE",' in entry
    assert "PLAN_SERVICES_RE.match" in app_runtime
    assert "plan_services_handler" in app_runtime
    # Account rename moves the register; account deletion purges it.
    assert "UPDATE plan_trials SET account_bi=? WHERE account_bi=?" in entry
    assert '"plan_trials",' in entry

    footprint = (
        ROOT.parent / "world" / "tools" / "build_worker_footprint.py"
    ).read_text()
    assert '"plan_services.py",' in footprint

    settings = (
        ROOT / "public" / "dashboard" / "partials" / "views" / "settings.html"
    ).read_text()
    account_js = (
        ROOT / "public" / "dashboard" / "js" / "04-account.js").read_text()
    built_settings = (
        ROOT / "public" / "dashboard" / "settings" / "index.html").read_text()
    built_js = (ROOT / "public" / "dashboard.js").read_text()
    for text in (settings, built_settings):
        assert "data-plan-usage-summary" in text
        assert "data-plan-trial-start" in text
    for text in (account_js, built_js):
        assert "/api/plans/" in text
        assert "loadPlanUsage" in text
        assert "trial_already_used" in text
