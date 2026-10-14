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

    ns = _load("_award_badge", extra_globals={
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

    async def send_badge_email(env, name, slug):
        emails.append((name, slug))
        return True

    ns = _load("_award_badge", extra_globals={
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
    assert "return await award_distinguished_contributor_badges(env)" in run


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
    card = script.split("function profileBadgeMedalSources(slug)", 1)[1]
    card = card.split("function renderProfileBadges(session)", 1)[0]
    assert '"/assets/badges/" + slug + ".svg"' in card
    assert "const icon = profileBadgeMedal(badge);" in card
    assert "badge.icon" not in card
