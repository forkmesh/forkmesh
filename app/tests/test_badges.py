#!/usr/bin/env python3
"""Achievement badges contract checks (adhoc #370).

Similar in shape to org teams (test_orgs_teams.py): a fixed public badge
catalog (src/badges.py) that people can earn automatically at a handful of
real platform events, or that a platform administrator can grant/revoke by
hand. These tests load the real helpers out of src/entry.py (AST extraction)
and pin:

  * the /api/badges route table and the badge_awards table/migration exist;
  * badges.py's catalog + slug validation are pure and total;
  * account_badges_handler: GET is public, POST/DELETE require a platform-
    administrator session and a known slug;
  * _award_badge is an idempotent, catalog-validated INSERT OR IGNORE;
  * automatic platform and activity events award their catalog slug at one
    authoritative choke point, while historical public contribution totals can
    still populate the World sleeve marks.

Run: python3 -m pytest cloudflare_worker/tests/test_badges.py
"""

import ast
import asyncio
import importlib.util
from pathlib import Path

from worker_test_helpers import json_from_request_double

ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
# The World and Office route bodies moved into sibling modules so Cloudflare's
# Python startup validation never parses them. These contracts still span the
# whole route family, so their source is read alongside entry.py.
WORLD_ROUTE_SOURCE = "".join(
    (Path(__file__).resolve().parents[2] / "world" / "src" / name).read_text(
        encoding="utf-8")
    for name in ("world_routes.py", "world_office_routes.py")
)
ENTRY_TEXT = ENTRY.read_text(encoding="utf-8") + WORLD_ROUTE_SOURCE
ACCOUNT_USERS = ROOT / "src" / "account_users.py"
ACCOUNT_USERS_TEXT = ACCOUNT_USERS.read_text(encoding="utf-8")
SCHEMA_TEXT = (ROOT / "src" / "schema.py").read_text(encoding="utf-8")


def _module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "src" / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


urls = _module("urls")
badges = _module("badges")


def _load(*names, extra_globals=None):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in names
    ]
    found = {n.name for n in selected}
    missing = set(names) - found
    assert not missing, "missing functions: %s" % sorted(missing)
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))
    namespace = dict(extra_globals or {})
    namespace.setdefault("bounded_json_request", json_from_request_double)
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


def _run(coro):
    return asyncio.new_event_loop().run_until_complete(coro)


class Clock:
    @staticmethod
    def now():
        return 1_700_000_000_000


def _response(data, status=200, **kwargs):
    return {"status": status, "data": data}


# --- Route table + schema ----------------------------------------------------

def test_routes_cover_the_badges_api():
    assert urls.BADGES_RE.match("/api/badges")
    m = urls.BADGE_ACCOUNT_RE.match("/api/badges/jett")
    assert m and m.group(1) == "jett"
    # single-segment BADGES_RE never swallows the per-account resource
    assert not urls.BADGES_RE.match("/api/badges/jett")


def test_schema_defines_badge_awards_table_and_migration_exists():
    assert "CREATE TABLE IF NOT EXISTS badge_awards" in SCHEMA_TEXT
    assert "idx_badge_awards_account" in SCHEMA_TEXT
    assert (ROOT / "migrations" / "0084_badge_awards.sql").exists()


# --- Catalog (pure module) ----------------------------------------------------

def test_catalog_is_a_fixed_known_set():
    slugs = {entry["slug"] for entry in badges.public_catalog()}
    assert slugs == {
        "first_100_users", "world_first_hour", "first_referral",
        "mirror_operator", "qa", "coder", "discord", "poster", "chat",
        "hackathon_builder", "hackathon_connector", "hackathon_finalist",
        "hackathon_winner", "hackathon_ambassador",
        "distinguished_contributor", "stargazer", "genies_gift",
        "six_eyes", "brood", "fan_club", "night_watch", "daily_spinner",
        "bug_catcher", "lift_off", "shopkeeper", "critic", "bounty_hunter",
        "web_weaver", "many_webs", "quick_silk", "carpet_ride", "eight_hands",
    }
    for entry in badges.public_catalog():
        assert entry["name"] and entry["description"] and entry["icon"]


def test_normalize_badge_slug_only_accepts_catalog_entries():
    assert badges.normalize_badge_slug("MIRROR_OPERATOR") == "mirror_operator"
    assert badges.normalize_badge_slug("  first_referral  ") == "first_referral"
    for bogus in ("", None, "nope", "first_100_users; DROP TABLE users", "a"):
        assert badges.normalize_badge_slug(bogus) == ""


def test_badge_definition_round_trips_the_catalog():
    info = badges.badge_definition("world_first_hour")
    assert info["name"] == "World Explorer"
    assert badges.badge_definition("bogus") is None


# --- _award_badge --------------------------------------------------------------

def _award_ns():
    writes = []

    async def ensure_schema(env):
        return None

    async def d1_run(env, sql, *params):
        writes.append((sql, params))

    ns = _load("_award_badge", "_d1_changed", extra_globals={
        "badge_catalog": badges,
        "ensure_schema": ensure_schema,
        "d1_run": d1_run,
        "Date": Clock,
    })
    return ns["_award_badge"], writes


def test_award_badge_inserts_or_ignores_a_valid_catalog_slug():
    award, writes = _award_ns()
    _run(award(None, "jett", "jett-bi", "mirror_operator"))
    assert len(writes) == 1
    sql, params = writes[0]
    assert "INSERT OR IGNORE INTO badge_awards" in sql
    assert params == (
        "mirror_operator", "jett-bi", "jett", "system", 1_700_000_000_000)


def test_award_badge_records_the_granting_admin():
    award, writes = _award_ns()
    _run(award(None, "jett", "jett-bi", "first_referral", granted_by="alice"))
    assert writes[0][1][3] == "alice"


def test_award_badge_is_a_noop_for_unknown_slug_or_missing_account():
    award, writes = _award_ns()
    _run(award(None, "jett", "jett-bi", "not-a-badge"))
    _run(award(None, "jett", "", "mirror_operator"))
    assert writes == []


# --- account_badges_handler ---------------------------------------------------

def _account_badges_ns(is_admin, account_exists=True, existing_awards=None):
    writes = []
    audits = []

    async def ensure_schema(env):
        return None

    async def account_row(env, name):
        if not account_exists:
            return "", None
        return name + "-bi", {"name": name}

    async def session_record(env, request, data=None):
        return "actor-bi", {"name": "actor"}

    async def has_role(env, name, role):
        assert role == "platform_administrator"
        return is_admin

    async def audit(env, actor, action, target_type, target, outcome,
                    details=None):
        audits.append({"actor": actor, "action": action, "target": target,
                       "outcome": outcome, "details": details or {}})

    async def award_badge(env, name, name_bi, slug, granted_by="system"):
        writes.append(("award", name, name_bi, slug, granted_by))

    async def d1_run(env, sql, *params):
        writes.append(("d1_run", sql, params))

    async def d1_all(env, sql, *params):
        return existing_awards or []

    ns = _load(
        "account_badges_handler", "_account_badges",
        extra_globals={
            "MAX_NODE_NAME": 63,
            "badge_catalog": badges,
            "ensure_schema": ensure_schema,
            "method_name": lambda request: request.method,
            "clean_string": lambda value, size: str(value or "")[:size],
            "valid_node_name": lambda name: bool(name) and name.isalnum(),
            "_account_row": account_row,
            "_account_session_record": session_record,
            "_has_role": has_role,
            "_audit_sensitive_action": audit,
            "_award_badge": award_badge,
            "d1_run": d1_run,
            "d1_all": d1_all,
            "json_response": _response,
        },
    )
    return ns["account_badges_handler"], writes, audits


class _Req:
    def __init__(self, method, body=None):
        self.method = method
        self._body = body or {}

    async def json(self):
        return self._body


def test_get_account_badges_is_public_and_lists_catalog_entries():
    handler, writes, audits = _account_badges_ns(
        is_admin=False,
        existing_awards=[{"badge_slug": "mirror_operator",
                          "granted_by": "system", "created_at": 42}])
    response = _run(handler(None, _Req("GET"), "jett"))
    assert response["status"] == 200
    assert response["data"]["account"] == "jett"
    assert response["data"]["badges"] == [{
        "slug": "mirror_operator",
        "name": "Mirror Operator",
        "description": badges.badge_definition("mirror_operator")["description"],
        "icon": badges.badge_definition("mirror_operator")["icon"],
        "grantedBy": "system",
        "awardedAt": 42,
    }]
    assert writes == []
    assert audits == []


def test_get_unknown_account_is_not_found():
    handler, _writes, _audits = _account_badges_ns(
        is_admin=False, account_exists=False)
    response = _run(handler(None, _Req("GET"), "ghost"))
    assert response == {"status": 404, "data": {"error": "not_found"}}


def test_grant_requires_platform_administrator():
    handler, writes, audits = _account_badges_ns(is_admin=False)
    response = _run(handler(
        None, _Req("POST", {"slug": "mirror_operator"}), "jett"))
    assert response["status"] == 403
    assert writes == []
    assert audits[-1]["outcome"] == "denied"


def test_grant_rejects_unknown_slug():
    handler, writes, _audits = _account_badges_ns(is_admin=True)
    response = _run(handler(
        None, _Req("POST", {"slug": "not-a-real-badge"}), "jett"))
    assert response == {"status": 400, "data": {"error": "invalid_badge"}}
    assert writes == []


def test_admin_can_grant_and_revoke_a_catalog_badge():
    handler, writes, audits = _account_badges_ns(is_admin=True)
    granted = _run(handler(
        None, _Req("POST", {"slug": "mirror_operator"}), "jett"))
    assert granted["status"] == 201
    assert granted["data"] == {
        "ok": True, "account": "jett", "slug": "mirror_operator",
        "granted": True}
    assert writes == [("award", "jett", "jett-bi", "mirror_operator", "actor")]
    assert audits[-1] == {
        "actor": "actor", "action": "badge.grant",
        "target": "jett/mirror_operator", "outcome": "success", "details": {}}

    handler, writes, audits = _account_badges_ns(is_admin=True)
    revoked = _run(handler(
        None, _Req("DELETE", {"slug": "mirror_operator"}), "jett"))
    assert revoked["data"] == {
        "ok": True, "account": "jett", "slug": "mirror_operator",
        "revoked": True}
    assert writes == [(
        "d1_run", "DELETE FROM badge_awards WHERE badge_slug=? AND account_bi=?",
        ("mirror_operator", "jett-bi"))]
    assert audits[-1]["action"] == "badge.revoke"


# --- Automatic-award hooks (source-level wiring) ------------------------------

def _function_source(name):
    for source in (ENTRY_TEXT, ACCOUNT_USERS_TEXT):
        fn = next((
            node for node in ast.parse(source).body
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            and node.name == name
        ), None)
        if fn is not None:
            return ast.unparse(fn)
    raise AssertionError("missing function: " + name)


def test_first_100_users_badge_is_awarded_from_the_signup_path():
    source = _function_source("_account_signup")
    assert "'first_100_users'" in source
    assert "FIRST_100_USERS_LIMIT" in source
    assert "_award_badge(env, name, name_bi, 'first_100_users')" in source


def test_first_referral_badge_is_awarded_once_the_referrer_converts_one_signup():
    source = _function_source("_finalize_referral_signup")
    assert "_award_badge(env, ref_name, ref_bi, 'first_referral')" in source
    assert "if signups == 1:" in source


def test_world_first_hour_badge_is_awarded_from_the_world_ticket_handler():
    source = _function_source("world_ticket_handler")
    assert "WORLD_FIRST_HOUR_BADGE_MS" in source
    assert (
        "_award_badge(env, claim['name'], account_bi, 'world_first_hour')"
        in source)


def test_mirror_operator_badge_is_awarded_the_first_time_a_node_is_linked():
    source = _function_source("_link_node_to_user")
    assert "was_operator = bool(nodes)" in source
    assert "if not was_operator:" in source
    assert "_award_badge(env, user_name, user_bi, 'mirror_operator')" in source


# --- Activity awards ---------------------------------------------------------

def _contributor_badge_ns(account=True):
    awards = []

    async def account_row(env, name):
        if not account:
            return "", None
        return "ada-bi", {"name": name, "kind": "user", "status": "active"}

    async def award(env, name, name_bi, slug, granted_by="system"):
        awards.append((name, name_bi, slug, granted_by))

    ns = _load("_award_contributor_badge", extra_globals={
        "ACTIVITY_BADGE_BY_CONTRIBUTION_KIND": {
            "issues": "qa", "pulls": "coder", "discussions": "poster",
        },
        "_account_row": account_row,
        "_account_kind": lambda rec: rec.get("kind", ""),
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "MAX_NODE_NAME": 63,
        "_award_badge": award,
    })
    return ns["_award_contributor_badge"], awards


def test_contribution_activity_awards_qa_coder_and_poster_badges():
    award, awards = _contributor_badge_ns()
    _run(award(None, "ada", "issues"))
    _run(award(None, "ada", "pulls"))
    _run(award(None, "ada", "discussions"))
    assert [entry[2] for entry in awards] == ["qa", "coder", "poster"]


def test_contributor_awards_ignore_non_account_activity():
    award, awards = _contributor_badge_ns(account=False)
    _run(award(None, "external-node", "issues"))
    assert awards == []


def test_contributor_tally_hook_awards_after_a_real_event_is_recorded():
    source = _function_source("_record_contributor")
    assert "await _award_contributor_badge(env, author_key, kind)" in source


def test_discord_link_awards_the_discord_badge_and_legacy_links_are_visible():
    mirror = _function_source("_mirror_account_identity_tables")
    assert "if discord_id:" in mirror
    assert "await _award_badge(env, name, name_bi, 'discord')" in mirror

    payload = _function_source("_account_chat_user_payload")
    assert "if discord_linked:" in payload
    assert "wearable_badges.add('discord')" in payload


def _chat_badge_handler_ns(active=True):
    awards = []
    invalidations = []

    async def session_record(env, request, data):
        if not active:
            return "", None
        return "ada-bi", {"name": "Ada", "kind": "user", "status": "active"}

    async def award(env, name, name_bi, slug, granted_by="system"):
        awards.append((name, name_bi, slug, granted_by))

    async def cache_delete(key):
        invalidations.append(key)

    ns = _load("chat_badge_activity_handler", extra_globals={
        "method_name": lambda request: request.method,
        "_account_session_record": session_record,
        "_account_kind": lambda rec: rec.get("kind", ""),
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "MAX_NODE_NAME": 63,
        "_award_badge": award,
        "edge_cache_delete": cache_delete,
        "USERS_DIRECTORY_CACHE_KEY": "users-directory",
        "json_response": _response,
    })
    return ns["chat_badge_activity_handler"], awards, invalidations


def test_authenticated_chat_send_awards_chat_and_refreshes_the_roster():
    handler, awards, invalidations = _chat_badge_handler_ns()
    response = _run(handler(None, _Req("POST", {})))
    assert response["status"] == 200
    assert response["data"] == {"ok": True, "badge": "chat"}
    assert awards == [("ada", "ada-bi", "chat", "system")]
    assert invalidations == ["users-directory"]


def test_chat_activity_dispatches_post_to_the_badge_handler():
    source = _function_source("chat_activity_handler")
    assert "if method_name(request) == 'POST':" in source
    assert "return await chat_badge_activity_handler(env, request)" in source


# --- Distinguished Contributor ------------------------------------------------

def test_distinguished_contributor_threshold_counts_issues_pulls_and_discussions():
    assert badges.DISTINGUISHED_CONTRIBUTOR_MIN == 15
    assert badges.distinguished_contributor_earned({"issues": 17})
    assert badges.distinguished_contributor_earned(
        {"issues": 5, "pulls": 5, "discussions": 5})
    assert not badges.distinguished_contributor_earned(
        {"issues": 14, "pulls": None, "discussions": "x", "commits": 99})
    assert not badges.distinguished_contributor_earned(None)
    assert badges.contribution_count({"issues": -4, "pulls": "3"}) == 3


class _RunResult:
    def __init__(self, changes):
        self.meta = type("Meta", (), {"changes": changes})()


def _award_email_ns(changes):
    emails = []

    async def ensure_schema(env):
        return None

    async def d1_run(env, sql, *params):
        return _RunResult(changes)

    async def send_badge_email(env, name, slug, tier=0, upgraded=False):
        emails.append((name, slug))
        return True

    ns = _load("_award_badge", "_d1_changed", extra_globals={
        "badge_catalog": badges,
        "ensure_schema": ensure_schema,
        "d1_run": d1_run,
        "Date": Clock,
        "_send_badge_email": send_badge_email,
        "_safe_error_text": str,
    })
    return ns["_award_badge"], emails


def test_new_award_sends_the_badge_email_once():
    award, emails = _award_email_ns(changes=1)
    assert _run(award(None, "spidertest", "s-bi", "distinguished_contributor"))
    assert emails == [("spidertest", "distinguished_contributor")]

    award, emails = _award_email_ns(changes=0)
    assert not _run(award(
        None, "spidertest", "s-bi", "distinguished_contributor"))
    assert emails == []


def _badge_email_ns(rec):
    sent = []
    recorded = []

    async def account_row(env, name):
        return (name + "-bi", rec) if rec else ("", None)

    async def send_email(env, to_email, subject, text, html=None,
                         email_kind="account", **kwargs):
        sent.append({"to": to_email, "subject": subject, "text": text,
                     "html": html, "kind": email_kind})
        return True

    async def record(env, name, kind, ok):
        recorded.append((name, kind, ok))

    def card(heading, intro, body, footer="", action_label="",
             action_url=""):
        return "|".join((heading, intro, body, footer, action_label,
                         action_url))

    ns = _load("_send_badge_email", extra_globals={
        "badge_catalog": badges,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "MAX_NODE_NAME": 63,
        "valid_node_name": lambda name: bool(name) and name.isalnum(),
        "_account_row": account_row,
        "_account_kind": lambda r: r.get("kind", ""),
        "_ap_origin": lambda env: "https://forkmesh.com",
        "quote": lambda value: value,
        "_html_escape": lambda value: str(value),
        "_forkmesh_email_card_html": card,
        "_send_email": send_email,
        "_record_account_email": record,
    })
    return ns["_send_badge_email"], sent, recorded


def _user(**extra):
    rec = {"kind": "user", "status": "active", "email": "s@example.org",
           "email_verified": True}
    rec.update(extra)
    return rec


def test_badge_email_goes_to_the_verified_address_with_a_profile_link():
    send, sent, recorded = _badge_email_ns(_user())
    assert _run(send(None, "SpiderTest", "distinguished_contributor"))
    assert len(sent) == 1
    mail = sent[0]
    assert mail["to"] == "s@example.org"
    assert mail["kind"] == "badge"
    assert mail["subject"] == (
        "You earned the Distinguished Contributor badge on ForkMesh")
    assert "https://forkmesh.com/@spidertest" in mail["text"]
    assert "View your profile|https://forkmesh.com/@spidertest" in mail["html"]
    assert recorded == [("spidertest", "badge", True)]


def test_badge_email_links_a_private_profile_to_the_dashboard():
    send, sent, _ = _badge_email_ns(_user(profile_private=True))
    _run(send(None, "spidertest", "qa"))
    assert "https://forkmesh.com/dashboard/profile" in sent[0]["text"]


def test_badge_email_skips_unverified_opted_out_and_non_user_accounts():
    for rec in (_user(email_verified=False), _user(email=""),
                _user(email_notifications=False), _user(status="disabled"),
                _user(kind="node"), None):
        send, sent, _ = _badge_email_ns(rec)
        assert not _run(send(None, "spidertest", "qa"))
        assert sent == []
    send, sent, _ = _badge_email_ns(_user())
    assert not _run(send(None, "spidertest", "not_a_badge"))
    assert sent == []


def _sweep_ns(rows, accounts, award_result=True):
    queries = []
    awards = []

    async def ensure_schema(env):
        return None

    async def d1_all(env, sql, *params):
        queries.append((sql, params))
        return rows

    async def account_row(env, name):
        return accounts.get(name, ("", None))

    async def award(env, name, name_bi, slug, granted_by="system"):
        awards.append((name, name_bi, slug))
        return award_result

    ns = _load("award_distinguished_contributor_badges", extra_globals={
        "badge_catalog": badges,
        "ensure_schema": ensure_schema,
        "d1_all": d1_all,
        "DISTINGUISHED_CONTRIBUTOR_SWEEP_LIMIT": 25,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "MAX_NODE_NAME": 63,
        "valid_node_name": lambda name: bool(name) and name.isalnum(),
        "_account_row": account_row,
        "_account_kind": lambda r: r.get("kind", ""),
        "_award_badge": award,
    })
    return ns["award_distinguished_contributor_badges"], queries, awards


def test_sweep_awards_only_active_users_whose_own_row_crossed_the_threshold():
    active = {"kind": "user", "status": "active"}
    rows = [
        {"author_bi": "s-bi", "username": "SpiderTest", "issues": 17,
         "pulls": 0, "discussions": 0},
        {"author_bi": "low-bi", "username": "low", "issues": 3,
         "pulls": 0, "discussions": 0},
        {"author_bi": "stale-bi", "username": "renamed", "issues": 20,
         "pulls": 0, "discussions": 0},
        {"author_bi": "off-bi", "username": "off", "issues": 20,
         "pulls": 0, "discussions": 0},
    ]
    accounts = {
        "spidertest": ("s-bi", active),
        "low": ("low-bi", active),
        "renamed": ("other-bi", active),
        "off": ("off-bi", {"kind": "user", "status": "disabled"}),
    }
    sweep, queries, awards = _sweep_ns(rows, accounts)
    result = _run(sweep(None))
    assert awards == [("spidertest", "s-bi", "distinguished_contributor")]
    assert result == {"awarded": ["spidertest"]}
    sql, params = queries[0]
    assert "LEFT JOIN badge_awards" in sql and "b.account_bi IS NULL" in sql
    assert "JOIN users u ON u.user_bi=c.author_bi" in sql
    assert params == ("distinguished_contributor", 15, 25)


def test_sweep_runs_hourly_from_the_cron_dispatcher():
    due = _function_source("_cron_due_phases")
    assert "elif hourly == 57:" in due
    assert "phases.append('badge-sweep')" in due
    run = _function_source("_run_cron_phase")
    assert "return await run_badge_sweep(env)" in run
    sweep = _function_source("run_badge_sweep")
    for name in ("award_distinguished_contributor_badges",
                 "award_stargazer_badges", "award_genies_gift_badges"):
        assert name in sweep


def test_badge_mail_kind_is_categorized_and_unsubscribable():
    assert "\"badge\": \"Badge awarded\"" in ENTRY_TEXT
    tracking = _module("mail_tracking")
    assert "badge" in tracking.UNSUBSCRIBE_KINDS


def test_profile_sidebar_lists_earned_badges():
    public = ROOT / "public" / "dashboard"
    template = (public / "partials" / "main.html").read_text(encoding="utf-8")
    assert "data-profile-badges hidden" in template
    assert "data-profile-badges-list" in template
    script = (public / "js" / "04-account.js").read_text(encoding="utf-8")
    assert "renderProfileBadges(session);" in script
    assert 'fetchJson("/api/badges/" + encodeURIComponent(name))' in script


def test_every_catalog_badge_has_a_medallion_and_the_card_shows_it():
    folder = ROOT / "public" / "assets" / "badges"
    for slug in badges.BADGE_DEFINITIONS:
        art = (folder / (slug + ".svg")).read_text(encoding="utf-8")
        assert art.startswith("<svg") and 'viewBox="0 0 128 128"' in art
        assert "<script" not in art and "href=" not in art
    script = (ROOT / "public" / "dashboard" / "js" / "04-account.js").read_text(
        encoding="utf-8")
    card = script.split("function profileBadgeMedalSources(slug, tier)", 1)[1]
    card = card.split("function renderProfileBadges(session)", 1)[0]
    assert '"/assets/badges/" + file + ".svg"' in card
    assert 'tier ? [slug + "-" + tier, slug] : [slug]' in card
    assert "const icon = profileBadgeMedal(badge);" in card
    assert "badge.icon" not in card


def test_tiered_badges_have_a_medallion_per_level_and_match_the_generator():
    import subprocess
    import sys
    folder = ROOT / "public" / "assets" / "badges"
    for slug, info in badges.BADGE_DEFINITIONS.items():
        if not info.get("tiers"):
            continue
        for tier in badges.TIER_NAMES[1:]:
            art = (folder / ("%s-%s.svg" % (slug, tier))).read_text(
                encoding="utf-8")
            assert art.startswith("<svg") and 'viewBox="0 0 128 128"' in art
            assert "<script" not in art and "href=" not in art
    result = subprocess.run(
        [sys.executable, str(ROOT / "tools" / "build_badge_medallions.py"),
         "--check"], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr


# --- Tiers ---------------------------------------------------------------------

def test_stargazer_uses_githubs_star_thresholds():
    assert badges.badge_tiers("stargazer") == (16, 128, 512, 4096)
    assert badges.tier_for_value("stargazer", 15) == -1
    assert badges.tier_for_value("stargazer", 16) == 0
    assert badges.tier_for_value("stargazer", 127) == 0
    assert badges.tier_for_value("stargazer", 128) == 1
    assert badges.tier_for_value("stargazer", 4095) == 2
    assert badges.tier_for_value("stargazer", 99999) == 3
    assert badges.tier_for_value("stargazer", "x") == -1
    assert badges.badge_tiers("genies_gift") == ()
    assert badges.tier_for_value("genies_gift", 100) == -1


def test_tier_summary_reports_progress_and_the_next_level():
    summary = badges.tier_summary("stargazer", 1, 300)
    assert summary["tier"] == "bronze"
    assert summary["tierLevel"] == 1
    assert summary["progress"] == 300
    assert summary["unit"] == "stars"
    assert summary["nextTier"] == "silver"
    assert summary["nextThreshold"] == 512
    top = badges.tier_summary("stargazer", 3, 5000)
    assert top["tier"] == "gold" and top["nextTier"] == ""
    assert top["nextThreshold"] == 0
    assert badges.tier_summary("qa", 0, 3) == {}


def test_public_catalog_lists_levels_only_for_tiered_badges():
    catalog = {entry["slug"]: entry for entry in badges.public_catalog()}
    assert [t["threshold"] for t in catalog["stargazer"]["tiers"]] == [
        16, 128, 512, 4096]
    assert [t["tier"] for t in catalog["stargazer"]["tiers"]] == [
        "default", "bronze", "silver", "gold"]
    assert "tiers" not in catalog["genies_gift"]
    assert "tiers" not in catalog["qa"]


def _tier_award_ns(changes):
    writes = []
    emails = []

    async def ensure_schema(env):
        return None

    async def d1_run(env, sql, *params):
        writes.append((sql, params))
        return _RunResult(changes.pop(0) if changes else 0)

    async def send_badge_email(env, name, slug, tier=0, upgraded=False):
        emails.append((name, slug, tier, upgraded))
        return True

    ns = _load("_award_badge", "_d1_changed", "_record_badge_progress",
               extra_globals={
                   "badge_catalog": badges,
                   "ensure_schema": ensure_schema,
                   "d1_run": d1_run,
                   "Date": Clock,
                   "_send_badge_email": send_badge_email,
                   "_safe_error_text": str,
               })
    return ns, writes, emails


def test_a_new_tiered_award_starts_at_the_reached_level():
    ns, writes, emails = _tier_award_ns([1])
    assert _run(ns["_award_badge"](None, "ada", "ada-bi", "stargazer", tier=2))
    sql, params = writes[0]
    assert "tier, tier_at" in sql
    assert params == ("stargazer", "ada-bi", "ada", "system",
                      1_700_000_000_000, 2, 1_700_000_000_000)
    assert emails == [("ada", "stargazer", 2, False)]


def test_an_existing_award_only_moves_up_and_mails_the_upgrade():
    ns, writes, emails = _tier_award_ns([0, 1])
    assert _run(ns["_award_badge"](None, "ada", "ada-bi", "stargazer", tier=3))
    sql, params = writes[1]
    assert sql.startswith("UPDATE badge_awards SET tier=?, tier_at=?")
    assert "AND tier<?" in sql
    assert params == (3, 1_700_000_000_000, "stargazer", "ada-bi", 3)
    assert emails == [("ada", "stargazer", 3, True)]

    ns, writes, emails = _tier_award_ns([0, 0])
    assert not _run(ns["_award_badge"](
        None, "ada", "ada-bi", "stargazer", tier=1))
    assert emails == []


def test_untiered_badges_ignore_a_tier_argument():
    ns, writes, _ = _tier_award_ns([1])
    _run(ns["_award_badge"](None, "ada", "ada-bi", "genies_gift", tier=3))
    assert writes[0][1] == (
        "genies_gift", "ada-bi", "ada", "system", 1_700_000_000_000)


def test_progress_keeps_the_best_value_and_awards_the_reached_level():
    ns, writes, emails = _tier_award_ns([1, 1])
    assert _run(ns["_record_badge_progress"](
        None, "ada", "ada-bi", "stargazer", 130))
    sql, params = writes[0]
    assert "INSERT INTO badge_progress" in sql
    assert "value=MAX(value, excluded.value)" in sql
    assert params == ("stargazer", "ada-bi", 130, 1_700_000_000_000)
    assert writes[1][1][5] == 1

    ns, writes, emails = _tier_award_ns([1])
    assert not _run(ns["_record_badge_progress"](
        None, "ada", "ada-bi", "stargazer", 15))
    assert len(writes) == 1 and emails == []

    ns, writes, _ = _tier_award_ns([1])
    assert not _run(ns["_record_badge_progress"](
        None, "ada", "ada-bi", "genies_gift", 15))
    assert writes == []


def test_account_badges_report_tier_and_progress_for_tiered_badges():
    handler, _, _ = _account_badges_ns(
        is_admin=False,
        existing_awards=[{"badge_slug": "stargazer", "granted_by": "system",
                          "created_at": 40, "tier": 2, "tier_at": 90,
                          "progress": 600}])
    badge = _run(handler(None, _Req("GET"), "jett"))["data"]["badges"][0]
    assert badge["tier"] == "silver"
    assert badge["tierLevel"] == 2
    assert badge["tierAt"] == 90
    assert badge["progress"] == 600
    assert badge["nextTier"] == "gold" and badge["nextThreshold"] == 4096
    assert badge["awardedAt"] == 40


def test_upgrade_email_names_the_new_level():
    send, sent, _ = _badge_email_ns(_user())
    assert _run(send(None, "ada", "stargazer", 3, True))
    assert sent[0]["subject"] == (
        "Your Stargazer badge is now Gold on ForkMesh")
    assert "Gold Stargazer" in sent[0]["text"]
    send, sent, _ = _badge_email_ns(_user())
    _run(send(None, "ada", "stargazer", 0, False))
    assert sent[0]["subject"] == "You earned the Stargazer badge on ForkMesh"


# --- Stargazer and Genie's Gift ----------------------------------------------

def _star_ns(account, stars):
    queries = []
    progress = []

    async def account_row(env, name):
        return (name + "-bi", account) if account else ("", None)

    async def d1_first(env, sql, *params):
        queries.append((sql, params))
        return {"n": stars}

    async def record(env, name, name_bi, slug, value):
        progress.append((name, name_bi, slug, value))
        return True

    ns = _load("_active_user_account", "_repo_stargazer_count",
               "_award_stargazer_for_repo", "_award_genies_gift",
               extra_globals={
                   "clean_string": lambda value, limit: str(value or "")[:limit],
                   "MAX_NODE_NAME": 63,
                   "valid_node_name": lambda name: bool(name) and name.isalnum(),
                   "_account_row": account_row,
                   "_account_kind": lambda r: r.get("kind", ""),
                   "d1_first": d1_first,
                   "_record_badge_progress": record,
                   "_award_badge": None,
                   "_safe_error_text": str,
               })
    return ns, queries, progress


def test_a_star_records_the_owners_stargazer_progress_without_their_own_star():
    ns, queries, progress = _star_ns(_user(), 20)
    assert _run(ns["_award_stargazer_for_repo"](None, "Ada", "repo-bi"))
    sql, params = queries[0]
    assert "WHERE repo_bi=? AND account_bi<>?" in sql
    assert params == ("repo-bi", "ada-bi")
    assert progress == [("ada", "ada-bi", "stargazer", 20)]


def test_stargazer_skips_organisations_and_inactive_owners():
    for rec in (_user(kind="org"), _user(status="disabled"), None):
        ns, queries, progress = _star_ns(rec, 500)
        assert not _run(ns["_award_stargazer_for_repo"](None, "acme", "r"))
        assert progress == []


def test_star_handler_checks_stargazer_after_a_new_star():
    source = _function_source("repo_star_handler")
    insert = source.index("INSERT OR IGNORE INTO repo_stars")
    hook = source.index("await _award_stargazer_for_repo(env, public_owner, repo_bi)")
    assert insert < hook < source.index("DELETE FROM repo_stars")


def test_genies_gift_needs_a_confirmed_mainnet_payment_to_someone_else():
    awards = []

    async def award(env, name, name_bi, slug, granted_by="system", tier=0):
        awards.append((name, slug))
        return True

    ns, _, _ = _star_ns(_user(), 0)
    ns["_award_badge"] = award
    gift = ns["_award_genies_gift"]
    assert _run(gift(None, "Ada", "bob", "mainnet", True))
    assert not _run(gift(None, "ada", "bob", "devnet", True))
    assert not _run(gift(None, "ada", "bob", "mainnet", False))
    assert not _run(gift(None, "ada", "ada", "mainnet", True))
    assert awards == [("ada", "genies_gift")]


def test_tip_notice_awards_genies_gift_only_for_verified_payments():
    source = _function_source("_account_tip_notice")
    assert ("await _award_genies_gift(env, sender, recipient, network, "
            "bool(context['task']) or reward is not None)") in source
    assert source.index("INSERT INTO tip_notice") < source.index(
        "_award_genies_gift")


def test_sweeps_backfill_stargazer_and_genies_gift():
    stars = _function_source("award_stargazer_badges")
    assert "JOIN repositories r ON r.key_bi=s.repo_bi" in stars
    assert "r.is_private=0 AND s.account_bi<>r.owner_bi" in stars
    gifts = _function_source("award_genies_gift_badges")
    assert "t.network='mainnet'" in gifts and "t.from_bi<>t.to_bi" in gifts
    assert "e.status='paid'" in gifts


def test_badge_tiers_migration_adds_columns_and_progress_table():
    up = (ROOT / "migrations" / "0264_badge_tiers.sql").read_text()
    down = (ROOT / "migrations" / "down" / "0264_badge_tiers.sql").read_text()
    assert "ADD COLUMN tier INTEGER" in up and "ADD COLUMN tier_at" in up
    assert "CREATE TABLE IF NOT EXISTS badge_progress" in up
    assert "DROP TABLE IF EXISTS badge_progress" in down
    assert "CREATE TABLE IF NOT EXISTS badge_progress" in SCHEMA_TEXT


# --- Activity badges swept from stored data -----------------------------------

ACTIVITY_BADGES = {
    "six_eyes": (1, 16, 64, 256),
    "brood": (2, 16, 64, 256),
    "fan_club": (10, 50, 250, 1000),
    "night_watch": (24, 168, 720, 8760),
    "daily_spinner": (7, 30, 100, 365),
    "bug_catcher": (1, 10, 50, 200),
    "lift_off": (10, 100, 1000, 10000),
    "shopkeeper": (10, 100, 1000, 10000),
    "critic": (1, 10, 50, 200),
    "bounty_hunter": (1, 5, 25, 100),
    "web_weaver": (2, 16, 128, 1024),
    "many_webs": (2, 8, 32, 128),
    "eight_hands": (1, 10, 24, 48),
}


def test_activity_badges_have_levels_and_a_unit():
    for slug, tiers in ACTIVITY_BADGES.items():
        assert badges.badge_tiers(slug) == tiers
        assert badges.BADGE_DEFINITIONS[slug]["unit"]


def test_longest_streak_counts_consecutive_days_only():
    days = ["2026-03-01", "2026-03-02", "2026-03-03", "2026-03-05",
            "2026-03-06", "2026-03-02", "bad", None]
    assert badges.longest_streak(days) == 3
    assert badges.longest_streak(["2025-12-31", "2026-01-01"]) == 2
    assert badges.longest_streak([]) == 0


def _entry_literal(name):
    for node in ast.parse(ENTRY.read_text(encoding="utf-8")).body:
        if isinstance(node, ast.Assign) and any(
                isinstance(t, ast.Name) and t.id == name for t in node.targets):
            return node
    raise AssertionError("missing " + name)


def _badge_sql():
    names = ("BADGE_CONTRIBUTION_SOURCES_SQL", "BADGE_CONTRIBUTION_DAYS_SQL",
             "BADGE_FOREIGN_MERGES_SQL", "BADGE_METRIC_SQL",
             "BADGE_PROGRESS_SELECT_SQL", "BADGE_FACT_CLOSER_SQL",
             "BADGE_FACT_SQL", "BADGE_FACT_SELECT_SQL",
             "BADGE_RECORD_REPOS_SQL", "BADGE_RECORD_UPSERT_SQL",
             "BADGE_RECORD_REPOS_PER_RUN", "BADGE_RECORD_BATCH",
             "BADGE_RECORD_REVIEW_CHECKS")
    module = ast.Module(body=[_entry_literal(n) for n in names],
                        type_ignores=[])
    namespace = {}
    exec(compile(ast.fix_missing_locations(module), str(ENTRY), "exec"),
         namespace)
    return namespace


def test_every_activity_badge_has_a_sweep():
    sql = _badge_sql()
    swept = set(sql["BADGE_METRIC_SQL"]) | {"daily_spinner"}
    assert swept == set(ACTIVITY_BADGES)
    sweep = _function_source("run_badge_sweep")
    assert "award_daily_spinner_badges" in sweep
    assert "for slug in BADGE_METRIC_SQL:" in sweep
    metric = _function_source("award_metric_badges")
    assert "ORDER BY m.value DESC LIMIT ?" in metric


def test_the_online_sampler_credits_every_live_node_before_the_cap():
    source = _function_source("record_online_sample")
    credit = source.index("await _credit_node_uptime(env, list(nodes), now)")
    assert credit < source.index("ONLINE_HISTORY_MAX_NODES")
    uptime = _function_source("_credit_node_uptime")
    assert "minutes=minutes+excluded.minutes" in uptime
    assert "NODE_UPTIME_MAX_KEYS" in uptime


def _sqlite_badge_db():
    import sqlite3
    db = sqlite3.connect(":memory:")
    db.row_factory = sqlite3.Row

    def literal(path, name):
        for node in ast.parse(path.read_text(encoding="utf-8")).body:
            if isinstance(node, ast.Assign) and any(
                    isinstance(t, ast.Name) and t.id == name
                    for t in node.targets):
                return ast.literal_eval(node.value)
        raise AssertionError("missing " + name)

    statements = (
        list(literal(ROOT / "src" / "schema.py", "SCHEMA_STATEMENTS"))
        + list(literal(ENTRY, "SCHEMA_ALTER_STATEMENTS"))
        + list(literal(ROOT / "src" / "marketplace_reviews.py", "SCHEMA")))
    for statement in statements:
        try:
            db.execute(statement)
        except sqlite3.Error:
            pass
    db.executescript(
        (ROOT / "migrations" / "0203_apps.sql").read_text(encoding="utf-8"))
    counter = [0]

    def insert(table, **values):
        for column in db.execute("PRAGMA table_info(%s)" % table):
            name = column["name"]
            if name in values or column["dflt_value"] is not None:
                continue
            if column["pk"] and column["type"].upper() == "INTEGER":
                continue
            if column["notnull"] or column["pk"]:
                counter[0] += 1
                values[name] = (
                    0 if "INT" in column["type"].upper()
                    else "%s-%d" % (name, counter[0]))
        db.execute("INSERT INTO %s (%s) VALUES (%s)" % (
            table, ",".join(values), ",".join("?" * len(values))),
            list(values.values()))

    for name in ("ada", "bob", "cy", "eve"):
        insert("users", user_bi="u_" + name, username=name, data="{}")
    insert("nodes", node_bi="n_ada", user_bi="u_ada", pubkey="key_ada",
           data="{}")
    insert("nodes", node_bi="n_free", user_bi="", pubkey="key_free",
           data="{}")
    for name in ("bob", "cy"):
        insert("nodes", node_bi="u_" + name, user_bi="u_" + name,
               pubkey="key_" + name, data="{}")
    for key, owner, private in (("r1", "u_ada", 0), ("r2", "u_bob", 0),
                                ("kA", "u_ada", 0), ("r3", "u_cy", 0),
                                ("rP", "u_cy", 1)):
        insert("repositories", key_bi=key, owner_bi=owner, data="{}",
               is_private=private)
    for repo, number, merged in (("r1", 1, 1), ("r1", 2, 1), ("r1", 3, 0),
                                 ("kA", 1, 1), ("r3", 1, 1), ("r2", 5, 1),
                                 ("rP", 1, 1)):
        insert("badge_pull_facts", repo_bi=repo, kind="pull", number=number,
               author_key="key_bob", opened_at=1, merged=merged)
    insert("badge_pull_facts", repo_bi="r2", kind="pull", number=1,
           author_key="key_ada", opened_at=1, merged=1)
    insert("badge_pull_facts", repo_bi="r1", kind="pull", number=4,
           author_key="key_free", opened_at=1, merged=1)
    for repo, number in (("r1", 1), ("r1", 2), ("rP", 1)):
        insert("badge_coauthor_facts", repo_bi=repo, number=number,
               account_bi="u_eve", created_at=1)
    for source, generation, captured in (("r1", "g1", 2), ("r2", "g2", 1)):
        insert("profile_contribution_projects", source_repo_bi=source,
               source_account_bi="u_ada", owner_user_bi="u_ada",
               project_bi="p1", first_public_day="2026-01-01", is_public=1,
               active_generation_bi=generation, captured_at=captured,
               data="{}")
    for day in ("01", "02", "03", "04", "05", "06", "07", "09"):
        insert("profile_contribution_days", generation_bi="g1",
               subject_user_bi="u_ada", source_repo_bi="r1", project_bi="p1",
               day="2026-03-" + day, commits=1, reviews=1)
    insert("profile_contribution_days", generation_bi="g2",
           subject_user_bi="u_ada", source_repo_bi="r2", project_bi="p1",
           day="2026-04-01", reviews=100)
    for fork, owner in (("f1", "u_bob"), ("f2", "u_cy"), ("f3", "n_ada")):
        insert("repo_forks", parent_bi="kA", fork_bi=fork, account_bi=owner,
               fork_owner="o", fork_name="a", created_at=1)
    for k in range(10):
        insert("profile_follows", follower_bi="fan%d" % k, target_bi="u_ada",
               follower_name="f", target_name="ada", created_at=1)
    insert("node_uptime_totals", node_key="n_ada", minutes=1500,
           updated_at=1)
    insert("node_uptime_totals", node_key="n_free", minutes=9000,
           updated_at=1)
    insert("microjob_claims", claim_id="c1", account_bi="u_bob",
           job="issue_report", subject_bi="s1", status="paid",
           amount_lamports=1000000)
    insert("microjob_claims", claim_id="c2", account_bi="u_bob",
           job="merged_pull", subject_bi="s2", status="pending_review",
           amount_lamports=1000000)
    insert("contributor_reward_estimates", reward_id="w1",
           recipient_bi="u_bob", recipient_name="bob", repo_owner="o",
           repo_name="r", source_kind="issue", subject_number=4,
           amount_lamports=2000000, condition="accepted_report",
           status="paid")
    for k in range(12):
        insert("release_downloads", repo_bi="kA", sha256="h", ts=1000,
               ua="agent%d" % k)
    insert("release_downloads", repo_bi="kA", sha256="h", ts=2000,
           ua="agent0")
    insert("apps", slug="tool", owner_bi="u_cy", listing="listed")
    for k in range(10):
        insert("marketplace_package_uses", market="forkmesh-apps",
               package_id="tool", account_bi="user%d" % k)
    insert("marketplace_package_uses", market="forkmesh-apps",
           package_id="tool", account_bi="u_cy")
    for review, hidden in (("rv1", 0), ("rv2", 0), ("rv3", 1)):
        insert("marketplace_reviews", review_id=review, market="m",
               package_id=review, reviewer_bi="u_eve", reviewer_name="eve",
               stars=5, hidden=hidden)
    insert("marketplace_review_votes", review_id="rv1", voter_bi="u_bob",
           created_at=1)
    insert("marketplace_review_votes", review_id="rv3", voter_bi="u_bob",
           created_at=1)
    return db


def test_activity_badge_queries_run_on_the_real_schema():
    sql = _badge_sql()
    db = _sqlite_badge_db()
    found = {}
    for slug, metric in sql["BADGE_METRIC_SQL"].items():
        rows = db.execute(
            "WITH " + metric + " " + sql["BADGE_PROGRESS_SELECT_SQL"] +
            "ORDER BY m.value DESC LIMIT ?",
            (slug, ACTIVITY_BADGES[slug][0], 25)).fetchall()
        found[slug] = [(row["username"], row["value"]) for row in rows]
    assert found == {
        "six_eyes": [("ada", 8)],
        "brood": [("ada", 2)],
        "fan_club": [("ada", 10)],
        "night_watch": [("ada", 25)],
        "bug_catcher": [("bob", 2)],
        "lift_off": [("ada", 12)],
        "shopkeeper": [("cy", 10)],
        "critic": [("eve", 1)],
        "bounty_hunter": [("bob", 2)],
        "web_weaver": [("bob", 4)],
        "many_webs": [("bob", 3)],
        "eight_hands": [("eve", 2)],
    }
    db.execute("INSERT INTO badge_progress VALUES ('fan_club','u_ada',10,5)")
    assert not db.execute(
        "WITH " + sql["BADGE_METRIC_SQL"]["fan_club"] + " " +
        sql["BADGE_PROGRESS_SELECT_SQL"] + "LIMIT 25",
        ("fan_club", 10)).fetchall()
    days = [row["day"] for row in db.execute(
        "WITH " + sql["BADGE_CONTRIBUTION_SOURCES_SQL"] + " SELECT DISTINCT "
        "d.day " + sql["BADGE_CONTRIBUTION_DAYS_SQL"] + " AND "
        "d.subject_user_bi=? AND d.commits+d.issues+d.pulls+d.reviews>0",
        ("u_ada",))]
    assert len(days) == 8
    assert badges.longest_streak(days) == 7


def test_a_metric_sweep_records_progress_for_active_matching_users():
    recorded = []

    async def ensure_schema(env):
        return None

    async def d1_all(env, sql, *params):
        assert params == ("fan_club", 10, 25)
        return [{"account_bi": "ada-bi", "username": "Ada", "value": 60},
                {"account_bi": "stale-bi", "username": "renamed", "value": 90}]

    async def active(env, name):
        name = name.lower()
        return name, name + "-bi", {}

    async def record(env, name, name_bi, slug, value):
        recorded.append((name, name_bi, slug, value))
        return True

    ns = _load("award_metric_badges", "_record_swept_progress", extra_globals={
        "ensure_schema": ensure_schema,
        "d1_all": d1_all,
        "badge_catalog": badges,
        "BADGE_SWEEP_LIMIT": 25,
        "BADGE_METRIC_SQL": {"fan_club": "m AS (SELECT 1)"},
        "BADGE_PROGRESS_SELECT_SQL": "SELECT ",
        "_active_user_account": active,
        "_record_badge_progress": record,
    })
    assert _run(ns["award_metric_badges"](None, "fan_club")) == {
        "awarded": ["ada"]}
    assert recorded == [("ada", "ada-bi", "fan_club", 60)]


def test_node_uptime_migration_creates_the_totals_table():
    up = (ROOT / "migrations" / "0265_node_uptime_totals.sql").read_text()
    down = (ROOT / "migrations" / "down" /
            "0265_node_uptime_totals.sql").read_text()
    assert "CREATE TABLE IF NOT EXISTS node_uptime_totals" in up
    assert "DROP TABLE IF EXISTS node_uptime_totals" in down
    assert "CREATE TABLE IF NOT EXISTS node_uptime_totals" in SCHEMA_TEXT


# --- Pull, issue and co-author records ----------------------------------------

def test_pull_record_facts_read_the_merge_fields():
    fact = badges.pull_record_fact({
        "number": "7", "status": "merged", "author": "key_a",
        "ts": "1700000000000", "mergedAt": "1700000200000",
        "mergedBy": "key_m",
        "coAuthors": "Pat@Example.TEST, pat@example.test,nope,<x>@y,b@c.io",
    }, 7)
    assert fact == {
        "author_key": "key_a", "closer_key": "key_m",
        "opened_at": 1700000000000, "closed_at": 1700000200000,
        "merged": True, "co_authors": ["pat@example.test", "b@c.io"]}
    opened = badges.pull_record_fact(
        {"number": "7", "status": "open", "author": "key_a", "ts": "5",
         "mergedBy": "key_m", "coAuthors": "a@b.io"}, 7)
    assert opened["merged"] is False and opened["closer_key"] == ""
    assert opened["co_authors"] == [] and opened["opened_at"] == 5000
    assert badges.pull_record_fact({"number": "8"}, 7) is None
    assert badges.pull_record_fact({}, 7) is None


def test_issue_record_fact_takes_the_first_close_after_opening():
    record = {"events": [
        {"type": "status", "status": "closed", "author": "early", "ts": 1},
        {"type": "open", "author": "key_a", "ts": 1700000000000},
        {"type": "comment", "author": "x", "ts": 1700000001000},
        {"type": "status", "status": "open", "author": "x",
         "ts": 1700000002000},
        {"type": "status", "status": "closed", "author": "key_c",
         "ts": 1700000100000},
        {"type": "status", "status": "closed", "author": "late",
         "ts": 1700000900000},
    ]}
    assert badges.issue_record_fact(record) == {
        "author_key": "key_a", "closer_key": "key_c",
        "opened_at": 1700000000000, "closed_at": 1700000100000}
    assert badges.issue_record_fact({"events": [{"type": "open"}]}) is None
    assert badges.issue_record_fact(None) is None


def test_quick_close_is_five_minutes_and_needs_both_times():
    assert badges.QUICK_SILK_MS == 300000
    assert badges.quick_close(1000, 1000 + badges.QUICK_SILK_MS)
    assert not badges.quick_close(1000, 1001 + badges.QUICK_SILK_MS)
    assert not badges.quick_close(1000, 999)
    assert not badges.quick_close(0, 10)
    assert "f.closed_at-f.opened_at<=%d" % badges.QUICK_SILK_MS in (
        _badge_sql()["BADGE_FACT_SQL"]["quick_silk"])


def test_review_files_are_the_numbered_review_events():
    assert badges.has_review_file([{"name": "0003-review.md"}])
    assert not badges.has_review_file(
        [{"name": "pull.md"}, {"name": "0002-comment.md"},
         {"name": "review.md"}])


def test_one_level_record_badges_run_on_the_real_schema():
    sql = _badge_sql()
    db = _sqlite_badge_db()
    db.execute("DELETE FROM badge_pull_facts")
    for values in (
            ("r1", "issue", 7, "key_cy", "", 1000000, 1200000, 0, 0, 0),
            ("r1", "pull", 8, "", "u_eve", 1000000, 1100000, 1, 1, 0),
            ("r1", "pull", 9, "key_ada", "", 1000000, 1400001, 1, 1, 0),
            ("rP", "issue", 1, "key_ada", "", 1000000, 1000001, 0, 0, 0),
            ("r1", "pull", 10, "", "u_ada", 0, 1, 1, 0, 0),
            ("r2", "pull", 11, "key_bob", "", 1, 9000000, 1, 0, 0),
            ("r2", "pull", 12, "key_ada", "", 1, 9000000, 1, 1, 0),
            ("r3", "pull", 13, "", "u_cy", 1, 9000000, 1, 1, 1),
            ("r3", "pull", 14, "key_free", "", 1, 9000000, 1, 0, 0)):
        db.execute(
            "INSERT INTO badge_pull_facts (repo_bi, kind, number, "
            "closer_key, closer_bi, opened_at, closed_at, merged, reviewed, "
            "review_waived) VALUES (?,?,?,?,?,?,?,?,?,?)", values)

    def winners(slug):
        return sorted(row["username"] for row in db.execute(
            "WITH " + sql["BADGE_FACT_SQL"][slug] + " " +
            sql["BADGE_FACT_SELECT_SQL"] + "LIMIT ?", (slug, 25)))

    assert winners("quick_silk") == ["cy", "eve"]
    assert winners("carpet_ride") == ["bob", "cy"]
    db.execute("INSERT INTO badge_awards (badge_slug, account_bi, name, "
               "granted_by, created_at) VALUES ('quick_silk','u_cy','cy',"
               "'system',1)")
    assert winners("quick_silk") == ["eve"]


def test_record_sweep_picks_public_repos_whose_refs_moved():
    sql = _badge_sql()
    db = _sqlite_badge_db()
    for key, ts in (("r1", 10), ("r1", 20), ("r2", 5), ("rP", 50),
                    ("r3", 30)):
        db.execute("INSERT INTO repo_state_history VALUES (?,?,?)",
                   (key, "h%d" % ts, ts))
    db.execute("INSERT INTO badge_repo_sweeps VALUES ('r1', 10, 100)")
    db.execute("INSERT INTO badge_repo_sweeps VALUES ('r3', 30, 1)")
    rows = db.execute(sql["BADGE_RECORD_REPOS_SQL"], (4,)).fetchall()
    assert [(row["key_bi"], row["state_ts"]) for row in rows] == [
        ("r2", 5), ("r1", 20)]


class _SqliteD1:
    def __init__(self, db):
        self.db = db

    async def run(self, env, sql, *params):
        self.db.execute(sql, params)

    async def all(self, env, sql, *params):
        return [dict(row) for row in self.db.execute(sql, params)]

    async def first(self, env, sql, *params):
        rows = await self.all(env, sql, *params)
        return rows[0] if rows else None


class _MsClock:
    value = 1_700_001_000_000

    @classmethod
    def now(cls):
        return cls.value


def _sweep_namespace(db, gateway):
    import base64
    import json as json_module
    from urllib.parse import quote
    d1 = _SqliteD1(db)
    accounts = {
        "pat@example.test": {"user_bi": "u_eve", "rec": {
            "email": "pat@example.test", "email_verified": True,
            "status": "active", "kind": "user"}},
        "own@example.test": {"user_bi": "u_bob", "rec": {
            "email": "own@example.test", "email_verified": True,
            "status": "active", "kind": "user"}},
        "new@example.test": {"user_bi": "u_cy", "rec": {
            "email": "new@example.test", "email_verified": False,
            "status": "active", "kind": "user"}},
    }
    for email, account in accounts.items():
        db.execute("UPDATE users SET email_bi=?, data=? WHERE user_bi=?",
                   ("bi:" + email, json_module.dumps(account["rec"]),
                    account["user_bi"]))
    db.execute("DELETE FROM badge_pull_facts")
    db.execute("DELETE FROM badge_coauthor_facts")
    db.execute("UPDATE repositories SET data=? WHERE key_bi='r1'",
               (json_module.dumps({"owner": "ada", "repo": "web"}),))

    async def decrypt_row(env, data):
        return json_module.loads(data or "null")

    async def blind_index(env, value):
        return "bi:" + value

    async def ensure_schema(env):
        return None

    async def repo_gateway(env, owner, repo, action):
        gateway["calls"].append((owner, repo, action))
        return gateway["answer"](action)

    def blob_text(blob):
        if not isinstance(blob, dict) or not blob.get("ok"):
            return ""
        return base64.b64decode(blob["content"]).decode()

    sql = _badge_sql()
    namespace = _load(
        "sweep_badge_records", "_sweep_repo_pull_records",
        "_sweep_repo_issue_records", "_badge_record_numbers",
        "_badge_record_blobs", "_record_coauthor_facts",
        "_verified_email_account", "_record_web_merge_badge_fact",
        extra_globals=dict(
            sql, d1_run=d1.run, d1_all=d1.all, d1_first=d1.first,
            ensure_schema=ensure_schema, decrypt_row=decrypt_row,
            blind_index=blind_index, Date=_MsClock, json=json_module,
            quote=quote, badge_catalog=badges, microjobs=_module("microjobs"),
            _forkbot_repo_gateway_json=repo_gateway,
            _forkbot_blob_text=blob_text,
            clean_string=lambda value, limit: str(value or "")[:limit],
            _account_kind=lambda rec: rec.get("kind", "user")))
    return namespace


def _blob(text):
    import base64
    return {"ok": True, "encoding": "base64",
            "content": base64.b64encode(text.encode()).decode()}


def _pull_md(number, status, **fields):
    lines = ["---", "number: %d" % number, "status: " + status]
    lines += ["%s: %s" % item for item in fields.items()]
    return "\n".join(lines + ["---", "", "body"]) + "\n"


def test_record_sweep_stores_each_fact_once_and_feeds_the_badges():
    import json as json_module
    db = _sqlite_badge_db()
    db.execute("INSERT INTO repo_state_history VALUES ('r1','h',20)")
    opened = 1_700_000_000_000
    records = {
        ".forkmesh/pulls/1/pull.md": _pull_md(
            1, "merged", author="key_bob", ts=opened,
            mergedAt=opened + 60000, mergedBy="key_ada",
            coAuthors="pat@example.test,own@example.test,new@example.test"),
        ".forkmesh/pulls/2/pull.md": _pull_md(
            2, "merged", author="key_bob", ts=opened,
            mergedAt=opened + 900000, mergedBy="key_ada"),
        ".forkmesh/pulls/3/pull.md": _pull_md(
            3, "open", author="key_cy", ts=opened),
        ".forkmesh/pulls/4/pull.md": _pull_md(
            4, "merged", author="key_cy", ts=opened),
        ".forkmesh/issues/closed/9/issue-9.json": json_module.dumps(
            {"events": [
                {"type": "open", "author": "key_bob", "ts": opened},
                {"type": "status", "status": "closed", "author": "key_cy",
                 "ts": opened + 1000}]}),
    }
    gateway = {"calls": []}

    def answer(action):
        from urllib.parse import parse_qs
        kind, _, query = action.partition("?")
        params = parse_qs(query)
        path = params.get("path", [""])[0]
        if kind == "tree" and path == ".forkmesh/pulls":
            return {"entries": [{"name": str(n), "type": "tree"}
                                for n in (1, 2, 3, 4)] + [
                {"name": "README.md", "type": "blob"}]}
        if kind == "tree" and path == ".forkmesh/pulls/1":
            return {"entries": [{"name": "pull.md"},
                                {"name": "0002-comment.md"}]}
        if kind == "tree" and path == ".forkmesh/pulls/2":
            return {"entries": [{"name": "0003-review.md"}]}
        if kind == "tree" and path == ".forkmesh/issues/closed":
            return {"entries": [{"name": "9", "type": "tree"}]}
        if kind == "blobs":
            return {"blobs": {p: _blob(records[p])
                              for p in params.get("path", [])
                              if p in records}}
        raise AssertionError(action)

    gateway["answer"] = answer
    ns = _sweep_namespace(db, gateway)
    _run(ns["_record_web_merge_badge_fact"](None, "r1", 4, "u_bob", True))
    assert _run(ns["sweep_badge_records"](None)) == {"awarded": ["ada/web"]}
    facts = {(row["kind"], row["number"]): dict(row) for row in db.execute(
        "SELECT * FROM badge_pull_facts WHERE repo_bi='r1'")}
    assert facts[("pull", 1)]["closer_key"] == "key_ada"
    assert facts[("pull", 1)]["closed_at"] == opened + 60000
    assert facts[("pull", 1)]["reviewed"] == 0
    assert facts[("pull", 2)]["reviewed"] == 1
    assert facts[("pull", 3)]["merged"] == 0
    assert facts[("pull", 4)]["closer_bi"] == "u_bob"
    assert facts[("pull", 4)]["review_waived"] == 1
    assert facts[("pull", 4)]["opened_at"] == opened
    assert facts[("pull", 4)]["closed_at"] == _MsClock.value
    assert facts[("issue", 9)]["closer_key"] == "key_cy"
    assert [tuple(row) for row in db.execute(
        "SELECT repo_bi, number, account_bi FROM badge_coauthor_facts")] == [
        ("r1", 1, "u_eve")]
    assert [tuple(row) for row in db.execute(
        "SELECT repo_bi, state_ts FROM badge_repo_sweeps")] == [("r1", 20)]

    calls = len(gateway["calls"])
    assert _run(ns["sweep_badge_records"](None)) == {"awarded": []}
    assert len(gateway["calls"]) == calls
    db.execute("INSERT INTO repo_state_history VALUES ('r1','h2',30)")
    gateway["calls"].clear()
    _run(ns["sweep_badge_records"](None))
    read = [action for _, _, action in gateway["calls"]
            if action.startswith("blobs?")]
    assert len(read) == 1 and "pulls%2F3%2Fpull.md" in read[0]
    assert db.execute(
        "SELECT COUNT(*) FROM badge_coauthor_facts").fetchone()[0] == 1

    sql = _badge_sql()

    def winners(slug):
        return sorted(row["username"] for row in db.execute(
            "WITH " + sql["BADGE_FACT_SQL"][slug] + " " +
            sql["BADGE_FACT_SELECT_SQL"] + "LIMIT 25", (slug,)))

    assert winners("quick_silk") == ["ada", "cy"]
    assert winners("carpet_ride") == ["ada", "bob"]


def test_record_sweep_keeps_a_repo_pending_when_the_mirror_fails():
    db = _sqlite_badge_db()
    db.execute("INSERT INTO repo_state_history VALUES ('r1','h',20)")
    gateway = {"calls": [], "answer": lambda action: None}
    ns = _sweep_namespace(db, gateway)
    assert _run(ns["sweep_badge_records"](None)) == {"awarded": []}
    assert [tuple(row) for row in db.execute(
        "SELECT repo_bi, state_ts, swept_at FROM badge_repo_sweeps")] == [
        ("r1", 0, _MsClock.value)]


def test_web_merges_record_the_merger_for_the_sweep():
    handler = (ROOT / "src" / "route_handlers.py").read_text(encoding="utf-8")
    hook = handler.index('globals().get("_record_web_merge_badge_fact")')
    assert handler.index('if mutation == "merge":') < hook
    assert "actor_bi, override_review)" in handler[hook:hook + 400]
    sweep = _function_source("run_badge_sweep")
    assert sweep.index("('records', sweep_badge_records)") < sweep.index(
        "for slug in BADGE_METRIC_SQL:")
    assert "for slug in BADGE_FACT_SQL:" in sweep


def test_record_fact_migration_creates_the_tables():
    up = (ROOT / "migrations" / "0266_badge_pull_facts.sql").read_text()
    down = (ROOT / "migrations" / "down" /
            "0266_badge_pull_facts.sql").read_text()
    for table in ("badge_pull_facts", "badge_coauthor_facts",
                  "badge_repo_sweeps"):
        assert "CREATE TABLE IF NOT EXISTS " + table in up
        assert "CREATE TABLE IF NOT EXISTS " + table in SCHEMA_TEXT
        assert "DROP TABLE IF EXISTS " + table in down
