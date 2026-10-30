#!/usr/bin/env python3
"""Contracts for the internal product dashboard (/product-analysis).

The module is exercised against an in-memory SQLite database built from the
real runtime DDL, with the Worker primitives it receives through
``_bind_runtime`` replaced by small doubles. Nothing here needs Pyodide.
"""

import ast
import asyncio
import hashlib
import importlib.util
import json
import re
import sqlite3
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"


def _entry_function(name):
    import ast
    import ipaddress
    entry = SRC / "entry.py"
    tree = ast.parse(entry.read_text(encoding="utf-8"), filename=str(entry))
    node = next(item for item in tree.body
                if isinstance(item, ast.FunctionDef) and item.name == name)
    namespace = {"ipaddress": ipaddress}
    exec(compile(ast.Module(body=[node], type_ignores=[]), str(entry), "exec"),
         namespace)
    return namespace[name]
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import schema  # noqa: E402

NOW = 1_788_000_000_000  # 2026-08-29T10:40:00Z
DAY = 86_400_000
PATH = "/product-analysis"


def _literal_assignment(path, name):
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(
                isinstance(target, ast.Name) and target.id == name
                for target in node.targets):
            return ast.literal_eval(node.value)
    raise AssertionError("missing " + name)


_ENTRY = SRC / "entry.py"
_PRE_CREATE = _literal_assignment(_ENTRY, "SCHEMA_PRE_CREATE_ALTER_STATEMENTS")
_ALTER = _literal_assignment(_ENTRY, "SCHEMA_ALTER_STATEMENTS")


def _database():
    """An in-memory database built from the exact DDL the Worker applies."""
    connection = sqlite3.connect(":memory:")
    connection.row_factory = sqlite3.Row
    for sql in _PRE_CREATE:
        try:
            connection.execute(sql)
        except sqlite3.Error:
            pass
    for sql in schema.SCHEMA_STATEMENTS:
        connection.execute(sql)
    for sql in _ALTER:
        try:
            connection.execute(sql)
        except sqlite3.Error:
            pass
    return connection


class FakeResponse:
    def __init__(self, body, status=200, headers=None):
        self.body = body
        self.status = status
        self.headers = dict(headers or {})


def _blind(value):
    return hashlib.sha256(str(value).encode("utf-8")).hexdigest()


class Harness:
    """A fresh module instance bound to SQLite-backed Worker doubles."""

    def __init__(self, env=None, now=NOW, scan_page=None, admin=""):
        self.connection = _database()
        self.admin = admin
        self.env = env if env is not None else SimpleNamespace(
            PRODUCT_ANALYSIS_USER="founder",
            PRODUCT_ANALYSIS_PASS="correct horse battery staple",
            DATA_KEY="data-key")
        self.clock = {"now": now}
        self.fails = {}
        self.locked = {}
        self.queries = []
        spec = importlib.util.spec_from_file_location(
            "product_analysis_under_test", SRC / "product_analysis.py")
        self.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.module)
        if scan_page:
            self.module.PRODUCT_ANALYSIS_SCAN_PAGE = scan_page
        shell_spec = importlib.util.spec_from_file_location(
            "admin_shell_under_test", SRC / "admin_shell.py")
        self.shell = importlib.util.module_from_spec(shell_spec)
        shell_spec.loader.exec_module(self.shell)
        self.shell._bind_runtime({})
        self.module._bind_runtime(self.primitives())

    # -- Worker primitive doubles ------------------------------------------
    def primitives(self):
        harness = self

        async def d1_all(_env, sql, *args):
            harness.queries.append(sql)
            rows = harness.connection.execute(sql, args).fetchall()
            return [dict(row) for row in rows]

        async def d1_first(_env, sql, *args):
            harness.queries.append(sql)
            row = harness.connection.execute(sql, args).fetchone()
            return dict(row) if row is not None else None

        async def d1_run(_env, sql, *args):
            harness.queries.append(sql)
            harness.connection.execute(sql, args)
            harness.connection.commit()
            return SimpleNamespace()

        async def decrypt_row(_env, blob):
            try:
                return json.loads(blob)
            except (TypeError, ValueError):
                return None

        async def blind_index(_env, value):
            return _blind(value)

        async def ensure_schema(_env):
            return None

        async def login_locked_until(_env, key):
            locked = harness.locked.get(key, 0)
            return locked if locked > harness.clock["now"] else 0

        async def login_record_fail(_env, key):
            harness.fails[key] = harness.fails.get(key, 0) + 1
            if harness.fails[key] >= 10:
                harness.locked[key] = harness.clock["now"] + 15 * 60 * 1000
            return {"fails": harness.fails[key],
                    "lockedUntil": harness.locked.get(key, 0)}

        async def login_clear(_env, key):
            harness.fails.pop(key, None)
            harness.locked.pop(key, None)

        async def check_admin_page_auth(_env, request):
            # The real check reads the account session; the double treats
            # an "account" cookie naming the configured admin as that.
            return bool(harness.admin) and (
                "account=" + harness.admin) in request.headers.get("cookie")

        async def account_cookie_name(_env, request):
            return harness.admin

        return {
            "Date": SimpleNamespace(now=lambda: harness.clock["now"]),
            "Response": FakeResponse,
            "_admin_shell": self.shell,
            "_check_admin_page_auth": check_admin_page_auth,
            "_account_cookie_name": account_cookie_name,
            "d1_all": d1_all,
            "d1_first": d1_first,
            "d1_run": d1_run,
            "decrypt_row": decrypt_row,
            "blind_index": blind_index,
            "ensure_schema": ensure_schema,
            "method_name": lambda request: str(request.method).upper(),
            "_transient_client_address": lambda request: (
                request.headers.get("cf-connecting-ip") or ""),
            "_client_source_bucket": _entry_function("_client_source_bucket"),
            "_login_locked_until": login_locked_until,
            "_login_record_fail": login_record_fail,
            "_login_clear": login_clear,
            "PRODUCT_ANALYSIS_PATH": PATH,
        }

    # -- fixture writers ------------------------------------------------------
    def add_user(self, name, created_at, signup=None, kind="user",
                 origin="unknown", nodes=None, status="active"):
        rec = {"name": name, "kind": kind, "status": status,
               "created_at": created_at, "pass_hash": "x" if kind == "user" else ""}
        if signup is not None:
            rec["signup"] = signup
        if nodes:
            rec["nodes"] = list(nodes)
        self.connection.execute(
            "INSERT INTO users (user_bi, data, username, signup_origin) "
            "VALUES (?,?,?,?)", (_blind(name), json.dumps(rec), name, origin))
        self.connection.commit()
        return _blind(name)

    def add_repo(self, owner, name, hosted_since=None, visibility="public",
                 size=0, root=""):
        rec = {"owner": owner, "name": name, "visibility": visibility,
               "sizeBytes": size, "rootCommit": root}
        if hosted_since is not None:
            rec["hostedSince"] = hosted_since
        self.connection.execute(
            "INSERT INTO repositories (key_bi, owner_bi, data) VALUES (?,?,?)",
            (_blind(owner + "/" + name), _blind(owner), json.dumps(rec)))
        self.connection.commit()

    def add_clone_day(self, day, owner, repo, clones):
        self.connection.execute(
            "INSERT INTO product_clone_daily (day, owner, repo, clones) "
            "VALUES (?,?,?,?)", (day, owner, repo, clones))
        self.connection.commit()

    # -- request helpers ----------------------------------------------------------
    def request(self, method="GET", query="", cookie="", body="", ip="203.0.113.7",
                path=PATH):
        headers = {"cookie": cookie, "cf-connecting-ip": ip}

        async def text():
            return body

        return SimpleNamespace(
            method=method,
            url="https://app.forkmesh.com" + path + (("?" + query) if query else ""),
            headers=SimpleNamespace(get=lambda name: headers.get(name, "")),
            text=text,
        )

    def call(self, request):
        return asyncio.run(self.module.product_analysis_handler(self.env, request))

    def login_cookie(self):
        response = self.call(self.request(
            method="POST", body="action=login&username=founder"
            "&password=correct+horse+battery+staple"))
        assert response.status == 303
        return response.headers["set-cookie"].split(";")[0]

    def page(self, query="", cookie=None):
        cookie = self.login_cookie() if cookie is None else cookie
        response = self.call(self.request(query=query, cookie=cookie))
        assert response.status == 200, response.status
        return response.body


def _cookie_only(header):
    return header.split(";")[0]


# --- access ---------------------------------------------------------------------

def test_route_does_not_exist_without_configured_credentials():
    harness = Harness(env=SimpleNamespace(DATA_KEY="k"))
    assert harness.call(harness.request()) is None
    harness = Harness(env=SimpleNamespace(
        PRODUCT_ANALYSIS_USER="founder", PRODUCT_ANALYSIS_PASS="",
        DATA_KEY="k"))
    assert harness.call(harness.request()) is None


def test_no_credential_is_written_in_the_source():
    text = (SRC / "product_analysis.py").read_text(encoding="utf-8")
    for needle in ("password=", "PASS =", "PASS=\"", "basic "):
        assert needle not in text.lower().replace("'", "\""), needle
    assert "PRODUCT_ANALYSIS_PASS" in text
    assert "getattr(env, \"PRODUCT_ANALYSIS_PASS\"" in text


def test_unauthenticated_get_shows_only_a_noindexed_login_form():
    harness = Harness()
    response = harness.call(harness.request())
    assert response.status == 200
    assert "name=\"password\"" in response.body
    assert "noindex, nofollow" in response.body
    assert response.headers["x-robots-tag"] == "noindex, nofollow"
    assert response.headers["cache-control"].startswith("no-store")
    assert "script-src" not in response.headers["content-security-policy"]
    assert "default-src 'none'" in response.headers["content-security-policy"]
    # No figure, table or section name leaks before login.
    for word in ("Overview", "Signup sources", "Total accounts"):
        assert word not in response.body


def test_sub_paths_other_methods_and_unknown_views_are_not_found():
    harness = Harness()
    cookie = harness.login_cookie()
    assert harness.call(harness.request(path=PATH + "/users", cookie=cookie)) is None
    assert harness.call(harness.request(method="PUT", cookie=cookie)) is None
    assert harness.call(harness.request(query="view=admin", cookie=cookie)) is None
    assert harness.call(harness.request(
        method="POST", body="action=export", cookie=cookie)) is None


def test_wrong_password_is_rejected_and_spends_the_source_budget():
    harness = Harness()
    response = harness.call(harness.request(
        method="POST", body="action=login&username=founder&password=nope"))
    assert response.status == 401
    assert "did not match" in response.body
    assert "set-cookie" not in response.headers
    assert sum(harness.fails.values()) == 1
    response = harness.call(harness.request(
        method="POST", body="action=login&username=someone"
        "&password=correct+horse+battery+staple"))
    assert response.status == 401
    assert sum(harness.fails.values()) == 2


def test_locked_source_gets_429_before_the_password_is_compared():
    harness = Harness()
    for _attempt in range(10):
        harness.call(harness.request(
            method="POST", body="action=login&username=founder&password=nope"))
    response = harness.call(harness.request(
        method="POST", body="action=login&username=founder"
        "&password=correct+horse+battery+staple"))
    assert response.status == 429
    assert int(response.headers["retry-after"]) > 0
    assert "Too many failed attempts" in response.body
    # Another address is unaffected.
    response = harness.call(harness.request(
        method="POST", ip="198.51.100.9", body="action=login&username=founder"
        "&password=correct+horse+battery+staple"))
    assert response.status == 303


def test_one_ipv6_64_shares_one_login_budget():
    harness = Harness()
    for index in range(10):
        harness.call(harness.request(
            method="POST", ip="2001:db8:7:8::%x" % (index + 1),
            body="action=login&username=founder&password=nope"))
    response = harness.call(harness.request(
        method="POST", ip="2001:db8:7:8:abcd::1", body="action=login"
        "&username=founder&password=correct+horse+battery+staple"))
    assert response.status == 429
    response = harness.call(harness.request(
        method="POST", ip="2001:db8:7:9::1", body="action=login"
        "&username=founder&password=correct+horse+battery+staple"))
    assert response.status == 303


def test_the_shared_credential_has_a_global_failure_cap():
    harness = Harness()
    cap = harness.module.PRODUCT_ANALYSIS_GLOBAL_MAX_FAILS
    for index in range(cap):
        harness.call(harness.request(
            method="POST", ip="198.18.%d.%d" % (index // 200, index % 200 + 1),
            body="action=login&username=founder&password=nope"))
    response = harness.call(harness.request(
        method="POST", ip="203.0.113.99", body="action=login"
        "&username=founder&password=correct+horse+battery+staple"))
    assert response.status == 429
    assert "Too many failed attempts" in response.body
    harness.clock["now"] += harness.module.PRODUCT_ANALYSIS_GLOBAL_WINDOW_MS
    response = harness.call(harness.request(
        method="POST", ip="203.0.113.99", body="action=login"
        "&username=founder&password=correct+horse+battery+staple"))
    assert response.status == 303


def test_correct_login_sets_a_scoped_httponly_cookie_and_redirects():
    harness = Harness()
    response = harness.call(harness.request(
        method="POST", body="action=login&username=founder"
        "&password=correct+horse+battery+staple"))
    assert response.status == 303
    assert response.headers["location"] == PATH
    cookie = response.headers["set-cookie"]
    assert cookie.startswith("forkmesh_product_analysis=")
    for attribute in ("HttpOnly", "Secure", "SameSite=Strict",
                      "Path=" + PATH):
        assert attribute in cookie
    assert harness.fails == {}
    page = harness.call(harness.request(cookie=_cookie_only(cookie)))
    assert page.status == 200
    assert "Total accounts" in page.body
    assert page.headers["x-robots-tag"] == "noindex, nofollow"


def test_session_cookie_rejects_tampering_expiry_and_password_rotation():
    harness = Harness()
    cookie = harness.login_cookie()
    name, _sep, value = cookie.partition("=")
    expires, _sep, signature = value.partition(".")
    tampered = "%s=%d.%s" % (name, int(expires) + 1, signature)
    assert harness.call(harness.request(cookie=tampered)).status == 200
    assert "name=\"password\"" in harness.call(
        harness.request(cookie=tampered)).body
    forged = "%s=%s.%s" % (name, expires, "0" * 64)
    assert "name=\"password\"" in harness.call(
        harness.request(cookie=forged)).body
    harness.clock["now"] = NOW + 13 * 60 * 60 * 1000
    assert "name=\"password\"" in harness.call(
        harness.request(cookie=cookie)).body
    harness.clock["now"] = NOW
    harness.env.PRODUCT_ANALYSIS_PASS = "a new password"
    assert "name=\"password\"" in harness.call(
        harness.request(cookie=cookie)).body


def test_logout_clears_the_cookie():
    harness = Harness()
    cookie = harness.login_cookie()
    response = harness.call(harness.request(
        method="POST", body="action=logout", cookie=cookie))
    assert response.status == 303
    assert "Max-Age=0" in response.headers["set-cookie"]


def test_head_requests_carry_headers_without_a_body():
    harness = Harness()
    response = harness.call(harness.request(method="HEAD"))
    assert response.status == 200
    assert response.body == ""
    assert response.headers["x-robots-tag"] == "noindex, nofollow"


# --- complete reads --------------------------------------------------------------

def test_reports_cover_every_row_beyond_one_page():
    harness = Harness(scan_page=100)
    for index in range(250):
        harness.add_user("user%03d" % index, NOW - index * DAY)
    report = asyncio.run(harness.module.build_report(harness.env, NOW))
    assert report["errors"] == {}
    assert len(report["accounts"]) == 250
    scans = [sql for sql in harness.queries
             if sql.startswith("SELECT user_bi,username")]
    assert len(scans) == 3  # 100 + 100 + 50: the loop ran to exhaustion
    body = harness.page()
    assert ">250<" in body


def test_scan_stops_only_on_a_short_page_and_never_on_a_default_cap():
    harness = Harness(scan_page=50)
    for index in range(100):  # exactly two full pages, then an empty one
        harness.add_user("user%03d" % index, NOW)
    rows = asyncio.run(harness.module.scan_table(
        harness.env, "user_bi", "users", "user_bi"))
    assert len(rows) == 100
    assert "ORDER BY user_bi LIMIT ?" in harness.queries[0]


def test_count_mismatch_renders_an_error_instead_of_a_number():
    harness = Harness()
    for index in range(5):
        harness.add_user("user%d" % index, NOW)
    real_first = harness.module.d1_first

    async def lying_first(env, sql, *args):
        row = await real_first(env, sql, *args)
        if "COUNT(*) AS n FROM users" in sql:
            return {"n": 7}
        return row

    harness.module.d1_first = lying_first
    with pytest.raises(harness.module.ProductAnalysisDataError) as failure:
        asyncio.run(harness.module.collect_accounts(harness.env))
    assert "collected 5 rows but D1 reports 7" in str(failure.value)
    body = harness.page()
    assert "accounts failed." in body
    assert "collected 5 rows but D1 reports 7" in body
    assert "Check D1 for replica lag" in body
    assert ">5<" not in body


def test_grouped_reads_verify_their_group_count():
    harness = Harness()
    harness.connection.execute(
        "INSERT INTO mirror_serve_counters (node_name,owner,repo,clones,"
        "website,updated_at) VALUES ('m1','alice','repo',3,0,1),"
        "('m2','alice','repo',4,0,1),('m1','bob','other',1,0,1)")
    harness.connection.commit()
    totals = asyncio.run(harness.module.collect_clone_totals(harness.env))
    assert totals == {("alice", "repo"): 7, ("bob", "other"): 1}


# --- signup origin -------------------------------------------------------------

def test_classification_never_guesses():
    classify = Harness().module.classify_signup_origin
    assert classify({}) == "unknown"
    assert classify({"created_at": NOW}) == "unknown"
    assert classify({"signup": {"referralCaptured": False, "referrer": ""}}
                    ) == "unknown"
    assert classify({"signup": {"referralCaptured": True, "referrer": ""}}
                    ) == "organic"
    assert classify({"signup": {"referralCaptured": True, "referrer": "ann"}}
                    ) == "referral"
    assert classify({"signup": {"referralCaptured": True, "referrer": ""}},
                    has_referral_attribution=True) == "referral"
    windows = ((NOW - DAY, NOW + DAY),)
    assert classify({"created_at": NOW,
                     "signup": {"referralCaptured": True, "referrer": ""}},
                    job_flow_windows=windows) == "job_flow"
    assert classify({"created_at": NOW - 5 * DAY,
                     "signup": {"referralCaptured": True, "referrer": ""}},
                    job_flow_windows=windows) == "organic"
    # No job-flow window is configured yet, by the founders' decision.
    assert Harness().module.PRODUCT_ANALYSIS_JOB_FLOW_WINDOWS == ()


def test_backfill_runs_once_writes_proof_and_never_downgrades():
    harness = Harness()
    harness.add_user("old", NOW - 40 * DAY)
    harness.add_user("direct", NOW - 3 * DAY,
                     signup={"referralCaptured": True, "referrer": ""})
    harness.add_user("linked", NOW - 2 * DAY,
                     signup={"referralCaptured": True, "referrer": "old"})
    harness.add_user("stamped", NOW - DAY, origin="referral")
    harness.add_user("bot", NOW, kind="node")
    report = asyncio.run(harness.module.build_report(harness.env, NOW))
    # "classified" is what the rules could prove from the records alone;
    # the stamped account keeps the origin migration 0175 wrote for it.
    assert report["backfill"] == {
        "ran": True, "updated": 2,
        "classified": {"unknown": 2, "organic": 1, "referral": 1}}
    origins = dict(harness.connection.execute(
        "SELECT username, signup_origin FROM users"))
    assert origins == {"old": "unknown", "direct": "organic",
                       "linked": "referral", "stamped": "referral",
                       "bot": "unknown"}
    marker = harness.connection.execute(
        "SELECT v FROM schema_meta WHERE k=?",
        (harness.module.PRODUCT_ANALYSIS_BACKFILL_MARKER,)).fetchone()[0]
    assert marker == "applied"
    # A second load trusts the column and does not touch the rows again.
    harness.connection.execute(
        "UPDATE users SET signup_origin='referral' WHERE username='direct'")
    harness.connection.commit()
    report = asyncio.run(harness.module.build_report(harness.env, NOW))
    assert report["backfill"] == {"ran": False, "updated": 0, "classified": {}}
    assert [account["origin"] for account in report["accounts"]
            if account["name"] == "direct"] == ["referral"]
    assert all("rec" not in account for account in report["accounts"])


def test_sources_view_breaks_accounts_down_by_origin_and_activation():
    harness = Harness()
    harness.add_user("a", NOW - 9 * DAY, origin="organic")
    harness.add_user("b", NOW - 8 * DAY, origin="organic")
    harness.add_user("c", NOW - 7 * DAY, origin="referral")
    harness.add_user("d", NOW - 6 * DAY)
    harness.add_repo("a", "one", hosted_since=NOW - 5 * DAY)
    harness.add_repo("a", "two", hosted_since=NOW - 4 * DAY)
    harness.add_repo("c", "three", hosted_since=NOW - 3 * DAY)
    report = asyncio.run(harness.module.build_report(harness.env, NOW))
    breakdown = harness.module.source_breakdown(report)
    rows = {row["origin"]: row for row in breakdown["rows"]}
    assert rows["organic"]["count"] == 2
    assert rows["organic"]["activated"] == 1
    assert rows["organic"]["share"] == 50.0
    assert rows["referral"]["activated_share"] == 100.0
    assert rows["job_flow"]["count"] == 0
    assert rows["unknown"]["count"] == 1
    body = harness.page("view=sources")
    assert "Referral programme" in body
    assert "pa-hatch" in body  # the second series is hatched, not only recoloured
    assert "no rule yet" in body.lower()


# --- overview figures ------------------------------------------------------------

def test_overview_figures_state_windows_and_plain_changes():
    harness = Harness()
    harness.add_user("a", NOW - 2 * DAY, origin="organic")
    harness.add_user("b", NOW - 3 * DAY)
    harness.add_user("c", NOW - 45 * DAY)
    harness.add_user("d", NOW - 400 * DAY)
    harness.add_repo("a", "one", hosted_since=NOW - DAY)
    harness.add_repo("c", "old", hosted_since=NOW - 50 * DAY)
    harness.add_repo("d", "undated")
    figures = {figure["label"]: figure for figure in
               harness.module.overview_figures(
                   asyncio.run(harness.module.build_report(harness.env, NOW)))}
    assert figures["Total accounts"]["value"] == "4"
    assert figures["Total accounts"]["window"] == "All time"
    assert figures["Total accounts"]["change"] == (
        "+2 in the last 30 days, 1 more than the previous 30 days.")
    assert figures["Accounts with at least one mirror"]["value"] == "3"
    assert figures["Mirrors created"]["value"] == "3"
    assert "1 mirrors carry no creation date" in figures["Mirrors created"]["change"]
    assert figures["Repositories cloned"]["empty"].startswith("No clone data yet")
    assert figures["Anonymous clones"]["empty"].startswith("No clone data yet")
    assert figures["Clones"]["value"] == "0"
    assert figures["Clones"]["window"].startswith("All time")
    for figure in figures.values():
        assert figure["window"], figure  # every count states its window
        assert "%" not in figure.get("change", "")


def test_activation_figure_counts_first_mirrors_within_seven_days_of_signup():
    harness = Harness()
    harness.add_user("fast", NOW - 20 * DAY)
    harness.add_user("slow", NOW - 25 * DAY)
    harness.add_user("none", NOW - 10 * DAY)
    harness.add_user("young", NOW - 2 * DAY)
    harness.add_user("before", NOW - 45 * DAY)
    harness.add_repo("fast", "one", hosted_since=NOW - 19 * DAY)
    harness.add_repo("slow", "two", hosted_since=NOW - 5 * DAY)
    harness.add_repo("before", "three", hosted_since=NOW - 44 * DAY)
    figures = {figure["label"]: figure for figure in
               harness.module.overview_figures(
                   asyncio.run(harness.module.build_report(harness.env, NOW)))}
    figure = figures["New accounts activated"]
    assert figure["value"] == "1"
    assert "within 7 days of signup" in figure["window"]
    assert figure["change"] == (
        "1 of 4 new accounts; 1 of 1 in the previous 30 days. 1 of the new "
        "accounts are younger than 7 days and can still activate.")
    assert "%" not in figure["change"]


def test_retention_figure_counts_old_accounts_active_in_the_window():
    harness = Harness()
    harness.add_user("back", NOW - 90 * DAY)
    harness.add_user("gone", NOW - 60 * DAY)
    harness.add_user("new", NOW - 3 * DAY)
    harness.connection.execute(
        "INSERT INTO account_presence (name_bi, ts) VALUES (?,?)",
        (_blind("back"), NOW - 2 * DAY))
    harness.connection.execute(
        "INSERT INTO account_presence (name_bi, ts) VALUES (?,?)",
        (_blind("gone"), NOW - 40 * DAY))
    harness.connection.execute(
        "INSERT INTO account_presence (name_bi, ts) VALUES (?,?)",
        (_blind("new"), NOW - DAY))
    harness.connection.commit()
    figures = {figure["label"]: figure for figure in
               harness.module.overview_figures(
                   asyncio.run(harness.module.build_report(harness.env, NOW)))}
    figure = figures["Returning accounts"]
    assert figure["value"] == "1"
    assert figure["change"].startswith("1 of 2 accounts older than 30 days.")
    assert "no previous period" in figure["change"]


def test_change_sentences_never_render_a_bare_zero_percent():
    module = Harness().module
    assert module.change_sentence(0, 0) == (
        "None in the last 30 days or the 30 days before.")
    assert module.change_sentence(4, 4) == (
        "+4 in the last 30 days, the same as the previous 30 days.")
    assert module.change_sentence(4, 11) == (
        "+4 in the last 30 days, 7 fewer than the previous 30 days.")
    assert module.change_sentence(12, 0, cumulative=False) == (
        "12 in the last 30 days, 12 more than the previous 30 days.")


def test_clone_figures_decline_a_comparison_the_data_cannot_support():
    harness = Harness()
    days = harness.module.recent_days(NOW, 30)
    harness.add_clone_day(days[-1], "alice", "repo", 3)
    harness.add_clone_day(days[-2], "alice", "repo", 2)
    harness.add_clone_day(days[-2], "bob", "other", 1)
    report = asyncio.run(harness.module.build_report(harness.env, NOW))
    figures = {figure["label"]: figure for figure in
               harness.module.overview_figures(report)}
    assert figures["Anonymous clones"]["value"] == "6"
    assert figures["Repositories cloned"]["value"] == "2"
    assert figures["Anonymous clones"]["change"].startswith(
        "No comparison: counting started on " + days[-2])
    assert "only covered from " + days[-2] in figures["Anonymous clones"]["change"]
    # Once both windows are covered the comparison is stated plainly.
    harness.add_clone_day(harness.module.recent_days(NOW, 60)[0], "bob",
                          "other", 4)
    report = asyncio.run(harness.module.build_report(harness.env, NOW))
    figures = {figure["label"]: figure for figure in
               harness.module.overview_figures(report)}
    assert figures["Anonymous clones"]["change"] == (
        "6 in the last 30 days, 2 more than the previous 30 days.")


# --- charts ----------------------------------------------------------------------

def test_sparse_series_render_a_table_instead_of_a_chart():
    harness = Harness()
    for index in range(4):
        harness.add_user("u%d" % index, NOW - index * DAY, origin="organic")
    body = harness.page()
    assert "Only 4 of the last 90 days have signups" in body
    assert "<polyline" not in body.split("Mirrors created per day")[0]
    for index in range(4, 12):
        harness.add_user("u%d" % index, NOW - index * DAY)
    body = harness.page()
    overview = body.split("Mirrors created per day")[0]
    assert "<polyline" in overview
    assert "stroke-dasharray" in overview  # second series is dashed, not just recoloured
    assert "<title>" in overview  # hover reads the exact date and values
    assert "gradient" not in overview.lower().split("legend")[0]
    assert "animation" not in overview or "animation:none" in overview


def test_gridlines_are_whole_numbers_for_counts():
    module = Harness().module
    days = module.recent_days(NOW, 10)
    svg = module.line_chart_svg(
        days, [{"label": "Signups", "values": [0, 1, 5, 0, 2, 0, 0, 3, 1, 0],
                "style": "accent"}])
    ticks = re.findall(r'class="tick"[^>]*text-anchor="end">([^<]+)<', svg)
    assert ticks == ["0", "2", "4", "6"]
    svg = module.line_chart_svg(
        days, [{"label": "Signups", "values": [0] * 9 + [100],
                "style": "accent"}])
    ticks = re.findall(r'class="tick"[^>]*text-anchor="end">([^<]+)<', svg)
    assert ticks == ["0", "50", "100", "150"]


def test_empty_series_say_what_would_appear():
    harness = Harness()
    body = harness.page()
    assert "No accounts were created in the last 90 days" in body
    assert "No mirrors with a creation date in the last 90 days" in body
    body = harness.page("view=repositories")
    assert "No clone data yet" in body
    body = harness.page("view=users")
    assert "No user accounts yet" in body


# --- users and repositories tables -----------------------------------------------

def test_users_table_is_read_only_sortable_searchable_and_paginated():
    harness = Harness()
    for index in range(120):
        harness.add_user("person%03d" % index, NOW - index * DAY,
                         nodes=["node%03d" % index] if index < 3 else None)
    harness.connection.execute(
        "INSERT INTO nodes (node_bi, user_bi, data, name, last_seen) "
        "VALUES (?,?,?,?,?)",
        (_blind("node001"), _blind("person001"), "{}", "node001", NOW - 1000))
    harness.connection.execute(
        "INSERT INTO account_sessions (session_id,account_bi,token_digest,"
        "created_at,last_seen_at,expires_at,revoked_at,device_label,client_ip,"
        "device_bi) VALUES ('s1',?,'d',1,?,9,0,'','','')",
        (_blind("person002"), NOW - 5000))
    harness.connection.execute(
        "INSERT INTO account_presence (name_bi, ts) VALUES (?,?)",
        (_blind("person000"), NOW - 250))
    harness.connection.commit()
    harness.add_repo("node001", "hosted", hosted_since=NOW - DAY)
    body = harness.page("view=users")
    assert "120 accounts, page 1 of 3" in body
    assert "person000" in body and "person050" not in body
    assert "Next page" in body and "Previous page" not in body
    # Read-only table: no row selection or actions. The navigation drawer's
    # toggle lives outside <main>, so the guard looks at the content only.
    for forbidden in ("delete", "impersonate", "type=\"checkbox\""):
        assert forbidden not in body.split("<main")[1].lower()
    body = harness.page("view=users&page=3")
    assert "person119" in body
    body = harness.page("view=users&q=person11")
    assert "10 accounts, page 1 of 1" in body
    body = harness.page("view=users&sort=mirrors&dir=desc")
    first_row = body.split("<tbody>")[1].split("</tr>")[0]
    assert "person001" in first_row  # the mirror is credited through its node
    assert ">1<" in first_row
    report = asyncio.run(harness.module.build_report(harness.env, NOW))
    activity = {account["name"]: account["last_activity"]
                for account in report["accounts"]}
    assert activity["person000"] == NOW - 250
    assert activity["person001"] == NOW - 1000
    assert activity["person002"] == NOW - 5000
    assert activity["person003"] == 0


def test_repositories_table_groups_mirrors_and_counts_window_clones():
    harness = Harness()
    harness.add_user("alice", NOW - 10 * DAY)
    harness.add_repo("alice", "repo", hosted_since=NOW - 9 * DAY, size=2048,
                     root="abc")
    harness.add_repo("mirror7", "repo", hosted_since=NOW - 8 * DAY, root="abc")
    harness.add_repo("alice", "solo", hosted_since=str(NOW - 7 * DAY),
                     visibility="private", size=1)
    days = harness.module.recent_days(NOW, 60)
    harness.add_clone_day(days[-1], "alice", "repo", 5)
    harness.add_clone_day(days[0], "alice", "repo", 9)  # outside the window
    report = asyncio.run(harness.module.build_report(harness.env, NOW))
    by_name = {repo["owner"] + "/" + repo["name"]: repo
               for repo in report["repositories"]}
    assert by_name["alice/repo"]["mirror_count"] == 2
    assert by_name["mirror7/repo"]["mirror_count"] == 2
    assert by_name["alice/solo"]["mirror_count"] == 1
    assert by_name["alice/repo"]["clones_30d"] == 5
    assert by_name["alice/solo"]["created_at"] == NOW - 7 * DAY
    body = harness.page("view=repositories")
    assert "3 repositories, page 1 of 1" in body
    assert "2.0 KB" in body
    assert "private" in body
    assert "Clones, last 30 days" in body
    assert "Clones per day, last 30 days" in body


def test_hosted_since_accepts_milliseconds_seconds_and_dates_only():
    parse = Harness().module.parse_hosted_since
    assert parse(NOW) == NOW
    assert parse(str(NOW)) == NOW
    assert parse(NOW // 1000) == (NOW // 1000) * 1000
    assert parse("2026-08-29T13:20:00Z") == 1_787_961_600_000
    assert parse("") == 0
    assert parse("soon") == 0
    assert parse(-5) == 0


# --- acquisition and layout ------------------------------------------------------

def test_acquisition_is_pending_without_fake_data():
    harness = Harness()
    body = harness.page("view=acquisition")
    assert "Pending" in body
    assert "utm_source" in body
    assert "intent signal" in body
    main = body.split("<main")[1]
    assert "<table" not in main
    assert "<svg" not in main  # sidebar icons live outside <main>


def test_a_view_with_no_figures_reads_no_rows():
    # Building the report decrypts every users and repositories row, which is
    # what made this page exceed the Worker CPU limit in production. The
    # acquisition view shows fixed copy, so it must read nothing at all.
    harness = Harness()
    for index in range(20):
        harness.add_user("user%02d" % index, NOW)
        harness.add_repo("user%02d" % index, "repo")
    cookie = harness.login_cookie()
    harness.queries = []
    assert "Pending" in harness.call(
        harness.request(query="view=acquisition", cookie=cookie)).body
    assert harness.queries == []

    # The same assertion over a view that does show figures, so a report that
    # stopped being built at all would fail this test rather than pass it.
    harness.queries = []
    harness.call(harness.request(cookie=cookie))
    assert any("FROM users" in sql for sql in harness.queries)


def test_head_does_not_build_a_report_it_would_not_send():
    harness = Harness()
    harness.add_user("founder", NOW)
    cookie = harness.login_cookie()
    harness.queries = []
    response = harness.call(harness.request(method="HEAD", cookie=cookie))
    assert response.status == 200
    assert response.body == ""
    assert response.headers["content-type"].startswith("text/html")
    assert harness.queries == []


def test_every_view_without_a_report_is_a_real_view():
    module = Harness().module
    assert module.VIEWS_WITHOUT_REPORT <= set(module.VIEW_KEYS)
    assert module.VIEWS_WITHOUT_REPORT
    for view in module.VIEWS_WITHOUT_REPORT:
        # report=None is what the handler passes: the renderer must not read it.
        assert module.render_dashboard_html(view, None, {})


def test_layout_has_five_sections_and_no_generated_look():
    harness = Harness()
    body = harness.page()
    for label in ("Overview", "Signup sources", "Users", "Repositories",
                  "Acquisition"):
        assert ">%s</a>" % label in body
    # One sidebar and one row of section tabs.
    assert body.count('<nav class="ab-nav"') == 1
    assert body.count('<nav class="tabs"') == 1
    assert "width:248px" in body
    assert "tabular-nums" in body
    assert "prefers-reduced-motion" in body
    assert ":focus-visible" in body
    assert "text-transform:uppercase" not in body
    assert "letter-spacing" not in body
    assert "box-shadow:0" not in body  # no soft drop shadows
    assert "gradient" not in body  # solid colours everywhere, hatch is an SVG pattern
    assert "<script" not in body
    assert "/assets/logo.svg" in body
    assert '<a href="?view=overview" aria-current="page">Overview</a>' in body
    assert '<a class="ab-item on" href="/product-analysis"' in body
    assert "<section class=\"card\">" in body
    assert body.count("name=\"theme\"") == 3


def test_partial_source_failure_blanks_only_dependent_figures():
    harness = Harness()
    harness.add_user("a", NOW - DAY)
    real_all = harness.module.d1_all

    async def flaky_all(env, sql, *args):
        if "product_clone_daily" in sql:
            raise RuntimeError("no such table: product_clone_daily")
        return await real_all(env, sql, *args)

    harness.module.d1_all = flaky_all
    body = harness.page()
    assert "product_clone_daily failed." in body
    assert "no such table" in body
    assert ">1<" in body  # accounts still render


# --- appearance --------------------------------------------------------------------

def test_theme_defaults_to_system_and_the_admin_theme_cookie_pins_it():
    harness = Harness()
    cookie = harness.login_cookie()
    body = harness.page(cookie=cookie)
    assert "<html lang=\"en\">" in body
    assert "prefers-color-scheme:dark" in body
    assert "#0d1117" in body and "#ffffff" in body  # GitHub dark and light
    dark = harness.page(cookie=cookie + "; forkmesh_admin_theme=dark")
    assert "<html lang=\"en\" class=\"theme-dark\">" in dark
    assert 'value="dark" title="Dark theme" aria-label="Dark theme" aria-pressed="true"' in dark
    light = harness.page(cookie=cookie + "; forkmesh_admin_theme=light")
    assert "class=\"theme-light\"" in light
    odd = harness.page(cookie=cookie + "; forkmesh_admin_theme=neon")
    assert "<html lang=\"en\">" in odd
    # The login page honours the pin too, still without any script.
    login = harness.call(harness.request(cookie="forkmesh_admin_theme=dark"))
    assert "class=\"theme-dark\"" in login.body
    assert "<script" not in login.body
    assert "script-src" not in login.headers["content-security-policy"]


def test_theme_switch_posts_to_the_shared_admin_route_and_returns_here():
    harness = Harness()
    cookie = harness.login_cookie()
    body = harness.page("view=users&sort=name&dir=asc&page=2", cookie=cookie)
    assert '<form class="seg" method="post" action="/admin/theme"' in body
    assert ('<input type="hidden" name="next" value="/product-analysis'
            '?view=users&amp;sort=name&amp;dir=asc&amp;page=2">') in body
    login = harness.call(harness.request()).body
    assert '<input type="hidden" name="next" value="/product-analysis">' in login
    # The route's own form handler no longer takes theme posts.
    assert harness.call(harness.request(
        method="POST", body="action=theme&theme=dark", cookie=cookie)) is None


def test_tables_and_sidebar_follow_the_shadcn_structure_without_gradients():
    harness = Harness()
    harness.add_user("a", NOW - DAY)
    harness.add_repo("a", "r", hosted_since=NOW - DAY, size=2048)
    for view in ("users", "repositories", "sources"):
        body = harness.page("view=" + view)
        main = body.split("<main")[1]
        assert "<section class=\"card\"><div class=\"card-t\"><table>" in main
        # A founder signed in with the password sees only this section.
        assert body.count("<a class=\"ab-item") == 1
        assert "<span>Users</span>" not in body
        assert "gradient" not in body
        assert "box-shadow" not in body
        assert "letter-spacing" not in body
    sources = harness.page("view=sources")
    assert "fill=\"url(#pa-hatch)\" class=\"s-second\"/></svg>No mirror yet" in sources


def test_mobile_navigation_is_a_drawer_without_any_script():
    harness = Harness()
    body = harness.page()
    # One hidden checkbox drives the drawer; the menu button, the scrim and
    # the close button are labels for it, so no script is needed.
    assert body.count("id=\"ab-nav\"") == 1
    assert body.count("for=\"ab-nav\"") == 3
    assert body.index("id=\"ab-nav\"") < body.index("<nav")
    assert "#ab-nav:checked~.ab-side{transform:none}" in body
    assert "transform:translateX(-100%)" in body
    assert "@media (max-width:767px)" in body
    assert ".ab-menu,.ab-close,.ab-scrim{display:none}" in body  # desktop untouched
    assert "<script" not in body
    assert body.count('<nav class="ab-nav"') == 1
    # The login page has no drawer to open.
    login = harness.call(harness.request()).body
    assert "id=\"ab-nav\"" not in login


def test_a_signed_in_platform_admin_needs_no_founder_password():
    harness = Harness(admin="ada")
    harness.env.ADMIN_PATH = "ops-console"
    harness.add_user("a", NOW - DAY)
    response = harness.call(harness.request(cookie="account=ada"))
    assert response.status == 200
    body = response.body
    assert "Total accounts" in body
    assert 'name="password"' not in body
    # The admin gets the whole admin sidebar and no founder sign-out.
    assert "<span>Users</span>" in body and "<span>Hackathon</span>" in body
    assert 'href="/ops-console?view=users"' in body
    assert 'value="logout"' not in body
    assert 'title="Signed in as ada"' in body

    # Without the session the password login is still the only way in.
    anonymous = harness.call(harness.request())
    assert 'name="password"' in anonymous.body


def test_admin_sessions_open_the_page_even_without_founder_secrets():
    harness = Harness(env=SimpleNamespace(DATA_KEY="k"), admin="ada")
    assert harness.call(harness.request(cookie="account=ada")).status == 200
    # Everyone else still gets the ordinary 404, including a login attempt.
    assert harness.call(harness.request()) is None
    assert harness.call(harness.request(
        method="POST", cookie="account=ada",
        body="action=login&username=a&password=b")) is None
