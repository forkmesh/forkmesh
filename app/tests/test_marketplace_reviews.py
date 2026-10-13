#!/usr/bin/env python3
"""Account-based Market star ratings with required improvement reviews.

Drives app/src/marketplace_reviews.py through a sqlite3-backed runtime that
applies the real 0198 migration, so the SQL here is the SQL the Worker runs.
"""

import asyncio
import json
import re
import sqlite3
import sys
from pathlib import Path

import pytest


APP = Path(__file__).resolve().parents[1]
SRC = APP / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import marketplace_reviews as reviews  # noqa: E402


MIGRATION = APP / "migrations" / "0198_marketplace_reviews.sql"
DOWN = APP / "migrations" / "down" / "0198_marketplace_reviews.sql"
ACTIVITY = APP / "migrations" / "0256_marketplace_package_activity.sql"
ACTIVITY_DOWN = (APP / "migrations" / "down"
                 / "0256_marketplace_package_activity.sql")
PROBLEM = ("The contact import stopped halfway through my CSV file and "
           "silently dropped every row after the first empty line.")
IMPROVEMENT = ("Skip blank lines instead of stopping, and show a summary of "
               "which rows were imported and which ones failed.")


class Runtime:
    """The narrow adapter surface, over one in-memory sqlite database."""

    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.executescript("""
            CREATE TABLE org_members (
                org_bi TEXT NOT NULL, member_bi TEXT NOT NULL,
                role TEXT NOT NULL DEFAULT 'member', name TEXT NOT NULL,
                created_at INTEGER NOT NULL, PRIMARY KEY (org_bi, member_bi));
            CREATE TABLE org_plugin_installs (
                org_bi TEXT NOT NULL, plugin TEXT NOT NULL,
                org_name TEXT NOT NULL DEFAULT '',
                installed_by_bi TEXT NOT NULL DEFAULT '',
                installed_by_name TEXT NOT NULL DEFAULT '',
                installed_at INTEGER NOT NULL DEFAULT 0,
                PRIMARY KEY (org_bi, plugin));
            CREATE TABLE marketplace_submissions (
                submission_id TEXT PRIMARY KEY, marketplace_id TEXT NOT NULL,
                package_id TEXT NOT NULL, version TEXT NOT NULL,
                submitted_by_bi TEXT NOT NULL, submitted_by_name TEXT NOT NULL,
                state TEXT NOT NULL, published_at INTEGER NOT NULL DEFAULT 0);
        """)
        self.db.executescript(MIGRATION.read_text(encoding="utf-8"))
        self.db.executescript(ACTIVITY.read_text(encoding="utf-8"))
        self.accounts = {}
        self.admins = set()
        self.viewer = ""
        self.http_method = "GET"
        self.body = {}
        self.params = {}
        self.clock = 1_800_000_000_000
        self.pings = []
        self.audits = []
        reviews._SCHEMA_READY = False

    # -- fixtures -------------------------------------------------------------
    def account(self, name, verified=True, status="active"):
        self.accounts[name] = {"name": name, "email_verified": verified,
                               "status": status, "kind": "user"}
        return name

    def maintain_first_party(self, name, role="owner"):
        self.db.execute(
            "INSERT INTO org_members VALUES (?,?,?,?,?)",
            ("bi:org:forkmesh", "bi:" + name, role, name, 1))

    def publish_community(self, package, submitter, version="1.2.0"):
        self.db.execute(
            "INSERT INTO marketplace_submissions VALUES (?,?,?,?,?,?,?,?)",
            (package + version, "forkmesh-community", package, version,
             "bi:" + submitter, submitter, "published", 5))

    def call(self, method, path, body=None, viewer="", params=None):
        self.http_method = method
        self.body = body if body is not None else {}
        self.viewer = viewer
        self.params = params or {}
        result = asyncio.run(reviews.handle(self, path))
        return result["status"], result["payload"]

    # -- adapter ----------------------------------------------------------------
    def method(self):
        return self.http_method

    def same_origin(self):
        return True

    def now(self):
        self.clock += 1000
        return self.clock

    def response(self, payload, status=200, cache_control=None,
                 extra_headers=None):
        return {"status": status, "payload": payload,
                "cache": cache_control, "headers": extra_headers}

    async def json_body(self, limit):
        raw = json.dumps(self.body)
        if len(raw) > limit:
            return None, "payload_too_large"
        return self.body, ""

    def query(self):
        return dict(self.params)

    async def session(self, data=None):
        rec = self.accounts.get(self.viewer)
        return ("bi:" + self.viewer, dict(rec)) if rec else ("", None)

    async def ensure_schema(self):
        return None

    async def d1_all(self, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]

    async def d1_first(self, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row else None

    async def d1_run(self, sql, *args):
        self.db.execute(sql, args)

    async def blind_index(self, value):
        return "bi:" + value

    async def is_admin(self, name):
        return name in self.admins

    async def notify(self, recipient, title, body="", href="", actor="",
                     dedupe="", meta=None):
        self.pings.append({"to": recipient, "title": title, "body": body,
                           "href": href, "actor": actor, "dedupe": dedupe,
                           "meta": meta})
        return True

    async def audit(self, actor, action, review_id, outcome="success"):
        self.audits.append((actor, action, review_id))


@pytest.fixture
def rt():
    runtime = Runtime()
    runtime.maintain_first_party(runtime.account("owner"))
    for name in ("alice", "bob", "carol", "dave", "erin"):
        runtime.account(name)
    runtime.account("unverified", verified=False)
    runtime.admins.add(runtime.account("admin"))
    return runtime


def _rate(rt, viewer, stars, package="crm", market="forkmesh", **fields):
    body = {"stars": stars, **fields}
    return rt.call("POST", f"/api/market/reviews/{market}/{package}", body,
                   viewer=viewer)


def _low(rt, viewer, stars=2, **extra):
    return _rate(rt, viewer, stars, problem=PROBLEM, improvement=IMPROVEMENT,
                 **extra)


# -- Validation ------------------------------------------------------------------

def test_below_five_without_a_review_is_rejected_server_side(rt):
    for stars in (1, 2, 3, 4):
        status, payload = _rate(rt, "alice", stars)
        assert status == 400
        assert payload["error"] == "review_required"
        assert payload["fields"] == {"problem": "required",
                                     "improvement": "required"}
    status, payload = _rate(rt, "alice", 3, problem=PROBLEM)
    assert status == 400 and payload["error"] == "review_invalid"
    assert payload["fields"] == {"improvement": "required"}
    assert rt.db.execute("SELECT COUNT(*) FROM marketplace_reviews").fetchone()[0] == 0


@pytest.mark.parametrize("text, issue", [
    ("Too short to be useful here.", "too_short"),
    ("Supercalifragilisticexpialidocious antidisestablishmentarianism "
     "incomprehensibilities", "too_few_words"),
    ("It was baaaaaad and the import broke on my big contacts file today",
     "repeated_characters"),
    ("This is broken!!!!!!!!!!!! the import fails on every file I try",
     "repeated_characters"),
    ("bad bad bad bad bad bad bad bad bad bad bad bad bad bad", "repetitive"),
    ("import fails import fails import fails import fails import fails",
     "repetitive"),
])
def test_low_effort_required_fields_are_rejected(rt, text, issue):
    assert reviews.field_issue(reviews.clean_text(text), True) == issue
    status, payload = _rate(rt, "alice", 2, problem=text,
                            improvement=IMPROVEMENT)
    assert status == 400
    assert payload["fields"]["problem"] == issue


def test_minimums_are_forty_characters_and_eight_words(rt):
    assert reviews.MIN_FIELD_CHARS == 40 and reviews.MIN_FIELD_WORDS == 8
    seven = "Import stops early whenever blank rows appear inside."
    assert len(seven) >= 40 and reviews.count_words(seven) == 8
    assert reviews.field_issue(seven, True) == ""
    assert reviews.field_issue("Import stops early whenever blank rows appear.",
                               True) == "too_few_words"
    same = _rate(rt, "alice", 2, problem=PROBLEM, improvement=PROBLEM.upper())
    assert same[0] == 400 and same[1]["fields"] == {
        "improvement": "same_as_problem"}
    long = _rate(rt, "alice", 2, problem=PROBLEM, improvement=IMPROVEMENT,
                 context="x " * 2001)
    assert long[0] == 400 and long[1]["fields"] == {"context": "too_long"}
    bad_category = _low(rt, "alice", category="rant")
    assert bad_category[0] == 400
    assert bad_category[1]["fields"] == {"category": "invalid"}
    assert _low(rt, "alice", version="latest!")[1]["fields"] == {
        "version": "invalid"}


def test_star_values_are_strict(rt):
    for stars in (0, 6, True, 2.5, "five", None):
        status, payload = _rate(rt, "alice", stars)
        assert status == 400 and payload["error"] == "invalid_rating"
    assert reviews.parse_stars("4") == 4


def test_five_stars_without_a_review_is_accepted(rt):
    status, payload = _rate(rt, "alice", 5)
    assert status == 201, payload
    assert payload["review"]["stars"] == 5
    assert payload["review"]["problem"] == ""
    assert payload["summary"] == {
        "average": 5.0, "count": 1,
        "histogram": {"1": 0, "2": 0, "3": 0, "4": 0, "5": 1}}
    assert rt.pings == []


def test_structured_review_is_stored_with_category_and_version(rt):
    status, payload = _low(rt, "alice", 3, category="missing_feature",
                           context="Steps: open CRM, import a 2k row CSV.",
                           version="1.13")
    assert status == 201, payload
    review = payload["review"]
    assert review["category"] == "missing-feature"
    assert review["version"] == "v1.13"
    assert review["problem"] == PROBLEM
    assert review["improvement"] == IMPROVEMENT
    assert review["context"].startswith("Steps:")


# -- One rating per account, history ----------------------------------------------

def test_one_rating_per_account_and_updates_keep_history(rt):
    first = _low(rt, "alice", 2)
    assert first[0] == 201
    review_id = first[1]["review"]["id"]
    second = _rate(rt, "alice", 5)
    assert second[0] == 200 and second[1]["created"] is False
    assert second[1]["review"]["id"] == review_id
    assert second[1]["review"]["revision"] == 2
    assert second[1]["review"]["edited"] is True
    assert rt.db.execute(
        "SELECT COUNT(*) FROM marketplace_reviews").fetchone()[0] == 1
    unchanged = _rate(rt, "alice", 5)
    assert unchanged[0] == 200 and unchanged[1]["unchanged"] is True
    history = rt.call("GET", f"/api/market/reviews/items/{review_id}/history",
                      viewer="alice")
    assert history[0] == 200
    revisions = history[1]["revisions"]
    assert [r["stars"] for r in revisions] == [2, 5]
    assert revisions[0]["improvement"] == IMPROVEMENT
    assert second[1]["summary"]["count"] == 1
    assert second[1]["summary"]["average"] == 5.0
    # The review id is stable per (market, package, account).
    assert review_id == reviews.review_id_for("forkmesh", "crm", "bi:alice")
    # Strangers cannot read someone's edit history; maintainers can.
    assert rt.call("GET", f"/api/market/reviews/items/{review_id}/history",
                   viewer="bob")[0] == 403
    assert rt.call("GET", f"/api/market/reviews/items/{review_id}/history",
                   viewer="owner")[0] == 200


def test_rating_writes_are_rate_limited(rt):
    for index in range(reviews.RATING_WRITES_PER_HOUR):
        assert _rate(rt, "alice", 5 if index % 2 else 4,
                     problem=PROBLEM, improvement=IMPROVEMENT)[0] in (200, 201)
    status, payload = _rate(rt, "alice", 5)
    assert status == 429 and payload["error"] == "rating_rate_limited"
    rt.clock += reviews.HOUR_MS
    assert _rate(rt, "alice", 5)[0] == 200


# -- Aggregates -------------------------------------------------------------------

def test_aggregates_average_count_and_histogram_are_cached_on_write(rt):
    _rate(rt, "alice", 5)
    _rate(rt, "bob", 5)
    _low(rt, "carol", 4)
    _low(rt, "dave", 2)
    status, payload = rt.call("GET", "/api/market/reviews/forkmesh/crm")
    assert status == 200
    assert payload["summary"] == {
        "average": 4.0, "count": 4,
        "histogram": {"1": 0, "2": 1, "3": 0, "4": 1, "5": 2}}
    _low(rt, "erin", 1)
    summary = rt.call("GET", "/api/market/reviews/summary")[1]
    assert summary["packages"]["crm"] == {
        "average": 3.4, "count": 5,
        "histogram": {"1": 1, "2": 1, "3": 0, "4": 1, "5": 2}}
    cached = rt.db.execute(
        "SELECT rating_count, rating_total, stars_5 FROM "
        "marketplace_package_rating_stats WHERE package_id='crm'").fetchone()
    assert tuple(cached) == (5, 17, 2)
    assert summary["marketplaces"]["forkmesh"]["crm"]["count"] == 5


def test_public_list_is_paginated_newest_and_most_helpful_first(rt):
    for name in ("alice", "bob", "carol"):
        _low(rt, name, 3)
    alice_id = reviews.review_id_for("forkmesh", "crm", "bi:alice")
    for voter in ("dave", "erin"):
        assert rt.call("POST", f"/api/market/reviews/items/{alice_id}/helpful",
                       viewer=voter)[0] == 200
    newest = rt.call("GET", "/api/market/reviews/forkmesh/crm",
                     params={"limit": "2"})[1]
    assert [r["reviewer"]["name"] for r in newest["reviews"]] == ["carol", "bob"]
    assert newest["hasMore"] is True and newest["total"] == 3
    page2 = rt.call("GET", "/api/market/reviews/forkmesh/crm",
                    params={"limit": "2", "page": "2"})[1]
    assert [r["reviewer"]["name"] for r in page2["reviews"]] == ["alice"]
    helpful = rt.call("GET", "/api/market/reviews/forkmesh/crm",
                      params={"sort": "helpful"}, viewer="dave")[1]
    assert helpful["reviews"][0]["reviewer"]["name"] == "alice"
    assert helpful["reviews"][0]["helpful"] == 2
    assert helpful["reviews"][0]["viewerFoundHelpful"] is True
    item = helpful["reviews"][0]
    for key in ("stars", "createdAt", "problem", "improvement"):
        assert key in item
    assert "hidden" not in item and "reportCount" not in item
    # Voting twice counts once; your own review takes no vote.
    rt.call("POST", f"/api/market/reviews/items/{alice_id}/helpful", viewer="dave")
    assert rt.db.execute("SELECT helpful_count FROM marketplace_reviews WHERE "
                         "review_id=?", (alice_id,)).fetchone()[0] == 2
    assert rt.call("POST", f"/api/market/reviews/items/{alice_id}/helpful",
                   viewer="alice")[0] == 403
    assert rt.call("DELETE", f"/api/market/reviews/items/{alice_id}/helpful",
                   viewer="dave")[1]["helpful"] == 1


# -- Authorization ------------------------------------------------------------------

def test_rating_needs_a_signed_in_verified_active_account(rt):
    status, payload = _rate(rt, "", 5)
    assert status == 401 and payload["error"] == "sign_in_required"
    status, payload = _rate(rt, "unverified", 5)
    assert status == 403 and payload["error"] == "email_not_verified"
    rt.account("disabled", status="disabled")
    assert _rate(rt, "disabled", 5)[1]["error"] == "email_not_verified"
    view = rt.call("GET", "/api/market/reviews/forkmesh/crm", viewer="unverified")
    assert view[1]["viewer"]["canRate"] is False
    assert view[1]["viewer"]["reason"] == "email_not_verified"


def test_maintainers_cannot_rate_their_own_package(rt):
    status, payload = _rate(rt, "owner", 5)
    assert status == 403 and payload["error"] == "maintainer_cannot_rate"
    rt.publish_community("community-clock", "alice")
    status, payload = _rate(rt, "alice", 5, package="community-clock",
                            market="forkmesh-community")
    assert status == 403 and payload["error"] == "maintainer_cannot_rate"
    assert _rate(rt, "bob", 5, package="community-clock",
                 market="forkmesh-community")[0] == 201


def test_unknown_packages_and_markets_are_refused(rt):
    assert _rate(rt, "alice", 5, package="nope")[0] == 404
    assert _rate(rt, "alice", 5, package="community-clock",
                 market="forkmesh-community")[0] == 404
    assert rt.call("GET", "/api/market/reviews/elsewhere/crm")[0] == 404
    assert rt.call("GET", "/api/market/reviews/forkmesh/CRM!")[0] == 404


def test_install_use_record_is_required_once_the_package_has_any(rt):
    # No records yet: the rating is accepted but not a verified install.
    first = _rate(rt, "alice", 5)
    assert first[0] == 201 and first[1]["review"]["verifiedUse"] is False
    # Bob's client reports that his account installed CRM.
    assert rt.call("POST", "/api/market/reviews/uses",
                   {"market": "forkmesh", "package": "crm",
                    "source": "desktop"}, viewer="bob")[0] == 200
    verified = _rate(rt, "bob", 5)
    assert verified[0] == 201 and verified[1]["review"]["verifiedUse"] is True
    # From now on a drive-by account without a record is refused.
    status, payload = _rate(rt, "carol", 5)
    assert status == 403 and payload["error"] == "package_use_required"
    view = rt.call("GET", "/api/market/reviews/forkmesh/crm", viewer="carol")[1]
    assert view["viewer"]["useRequired"] is True
    assert view["viewer"]["reason"] == "package_use_required"
    # An organization install counts for its members.
    rt.db.execute("INSERT INTO org_members VALUES ('bi:org:acme','bi:carol',"
                  "'member','carol',1)")
    rt.db.execute("INSERT INTO org_plugin_installs (org_bi,plugin) VALUES "
                  "('bi:org:acme','crm')")
    assert _rate(rt, "carol", 5)[0] == 201
    assert rt.call("POST", "/api/market/reviews/uses",
                   {"package": "crm"}, viewer="")[0] == 401
    assert rt.call("POST", "/api/market/reviews/uses",
                   {"package": "not-a-package"}, viewer="bob")[0] == 404


def _activity(rt, viewer, kind, package="crm"):
    return rt.call("POST", "/api/market/reviews/activity",
                   {"market": "forkmesh", "package": package, "kind": kind,
                    "source": "mobile"}, viewer=viewer)


def test_views_and_opens_are_counted_per_person(rt):
    assert _activity(rt, "", "view")[0] == 401
    assert _activity(rt, "alice", "launch")[1]["error"] == "invalid_kind"
    assert _activity(rt, "alice", "view", package="not-a-package")[0] == 404
    for _ in range(3):
        assert _activity(rt, "alice", "view")[0] == 200
    status, payload = _activity(rt, "alice", "open")
    assert status == 200
    assert (payload["views"], payload["opens"]) == (3, 1)
    _activity(rt, "alice", "open")
    _activity(rt, "bob", "view")
    view = rt.call("GET", "/api/market/reviews/forkmesh/crm",
                   viewer="alice")[1]
    assert view["activity"] == {"views": 4, "opens": 2, "people": 2}
    assert (view["viewer"]["views"], view["viewer"]["opens"]) == (3, 2)
    anonymous = rt.call("GET", "/api/market/reviews/forkmesh/crm")[1]
    assert (anonymous["viewer"]["views"], anonymous["viewer"]["opens"]) == (
        0, 0)
    path = "/api/market/reviews/forkmesh/crm/activity"
    assert rt.call("GET", path)[0] == 401
    assert rt.call("GET", path, viewer="bob")[1]["error"] == (
        "maintainer_required")
    status, people = rt.call("GET", path, viewer="owner")
    assert status == 200
    assert people["totals"] == {"views": 4, "opens": 2, "people": 2}
    assert [(p["name"], p["views"], p["opens"]) for p in people["people"]] == [
        ("alice", 3, 2), ("bob", 1, 0)]
    page = rt.call("GET", path, viewer="admin", params={"limit": "1"})[1]
    assert page["hasMore"] is True and len(page["people"]) == 1
    summary = rt.call("GET", "/api/market/reviews/summary")[1]
    assert summary["usage"]["crm"] == {"views": 4, "opens": 2, "people": 2}
    assert summary["usageByMarketplace"]["forkmesh"]["crm"]["people"] == 2


def test_an_open_counts_as_a_use_but_a_view_does_not(rt):
    _activity(rt, "alice", "open")
    _activity(rt, "bob", "view")
    status, payload = _rate(rt, "bob", 5)
    assert status == 403 and payload["error"] == "package_use_required"
    status, payload = _rate(rt, "alice", 5)
    assert status == 201 and payload["review"]["verifiedUse"] is True


def test_owner_views_and_moderation_are_role_gated(rt):
    review_id = _low(rt, "alice", 2)[1]["review"]["id"]
    base = f"/api/market/reviews/items/{review_id}"
    assert rt.call("GET", "/api/market/reviews/forkmesh/crm/feedback",
                   viewer="bob")[0] == 403
    assert rt.call("GET", "/api/market/reviews/forkmesh/crm/feedback")[0] == 401
    assert rt.call("POST", base + "/reply", {"text": "Thanks for this!"},
                   viewer="bob")[0] == 403
    assert rt.call("POST", base + "/addressed", {"version": "1.4"},
                   viewer="bob")[0] == 403
    assert rt.call("POST", base + "/moderation", {"hidden": True,
                   "reason": "spam"}, viewer="owner")[0] == 403
    assert rt.call("GET", "/api/market/reviews/reports", viewer="owner")[0] == 403
    assert rt.call("POST", base + "/reply", {"text": "Thanks!"},
                   viewer="unverified")[0] == 403
    assert rt.call("GET", "/api/market/reviews/forkmesh/crm/feedback",
                   viewer="admin")[0] == 200


# -- Owner replies and the feedback view ------------------------------------------

def test_owner_reply_marks_addressed_and_pings_the_reviewer(rt):
    review_id = _low(rt, "alice", 2, category="bug")[1]["review"]["id"]
    rt.pings.clear()
    status, payload = rt.call(
        "POST", f"/api/market/reviews/items/{review_id}/reply",
        {"text": "Fixed: blank lines are skipped now.", "addressedIn": "1.14"},
        viewer="owner")
    assert status == 200, payload
    review = payload["review"]
    assert review["reply"]["text"] == "Fixed: blank lines are skipped now."
    assert review["reply"]["by"] == "owner"
    assert review["addressedIn"] == "v1.14"
    assert [ping["to"] for ping in rt.pings] == ["alice"]
    assert "replied to your review" in rt.pings[0]["title"]
    public = rt.call("GET", "/api/market/reviews/forkmesh/crm")[1]["reviews"][0]
    assert public["reply"]["text"].startswith("Fixed")
    assert public["addressedIn"] == "v1.14"
    assert ("owner", "marketplace.review_reply", review_id) in rt.audits
    bad = rt.call("POST", f"/api/market/reviews/items/{review_id}/addressed",
                  {"version": "soon"}, viewer="owner")
    assert bad[0] == 400 and bad[1]["error"] == "invalid_version"
    cleared = rt.call("DELETE",
                      f"/api/market/reviews/items/{review_id}/addressed",
                      viewer="owner")
    assert cleared[1]["review"]["addressedIn"] == ""
    assert rt.call("POST", f"/api/market/reviews/items/{review_id}/reply",
                   {"text": "   "}, viewer="owner")[1]["error"] == "reply_required"


def test_feedback_view_aggregates_and_filters_improvements(rt):
    bug = _low(rt, "alice", 2, category="bug")[1]["review"]["id"]
    _low(rt, "bob", 3, category="docs")
    _low(rt, "carol", 1)
    _rate(rt, "dave", 5)
    rt.call("POST", f"/api/market/reviews/items/{bug}/addressed",
            {"version": "v1.14.0"}, viewer="owner")
    status, payload = rt.call("GET", "/api/market/reviews/forkmesh/crm/feedback",
                              viewer="owner")
    assert status == 200
    assert payload["statuses"] == {"open": 2, "addressed": 1}
    assert payload["categories"]["bug"] == 1
    assert payload["categories"]["docs"] == 1
    assert payload["categories"]["uncategorized"] == 1
    assert len(payload["items"]) == 3  # five-star ratings are not feedback
    open_docs = rt.call("GET", "/api/market/reviews/forkmesh/crm/feedback",
                        viewer="owner",
                        params={"category": "docs", "status": "open"})[1]
    assert [i["reviewer"]["name"] for i in open_docs["items"]] == ["bob"]
    addressed = rt.call("GET", "/api/market/reviews/forkmesh/crm/feedback",
                        viewer="owner", params={"status": "addressed"})[1]
    assert [i["addressedIn"] for i in addressed["items"]] == ["v1.14.0"]
    lowest = rt.call("GET", "/api/market/reviews/forkmesh/crm/feedback",
                     viewer="owner", params={"sort": "lowest"})[1]
    assert [i["stars"] for i in lowest["items"]] == [1, 2, 3]
    maintained = rt.call("GET", "/api/market/reviews/maintained",
                         viewer="owner")[1]["packages"]
    crm = next(p for p in maintained if p["package"] == "crm")
    assert crm["openFeedback"] == 2 and crm["summary"]["count"] == 4
    assert rt.call("GET", "/api/market/reviews/maintained",
                   viewer="bob")[1]["packages"] == []


# -- Moderation ---------------------------------------------------------------------

def test_report_abuse_and_admin_hide_removes_the_review_from_public_reads(rt):
    review_id = _low(rt, "alice", 1)[1]["review"]["id"]
    _rate(rt, "bob", 5)
    base = f"/api/market/reviews/items/{review_id}"
    assert rt.call("POST", base + "/report", {"reason": "x"},
                   viewer="bob")[1]["error"] == "report_reason_required"
    assert rt.call("POST", base + "/report", {"reason": "Harassing the author"},
                   viewer="alice")[0] == 403
    for reporter in ("bob", "carol", "bob"):
        assert rt.call("POST", base + "/report",
                       {"reason": "Abusive language toward the maintainer"},
                       viewer=reporter)[0] == 200
    queue = rt.call("GET", "/api/market/reviews/reports", viewer="admin")[1]
    assert queue["items"][0]["id"] == review_id
    assert queue["items"][0]["reportCount"] == 2
    assert {r["by"] for r in queue["items"][0]["reports"]} == {"bob", "carol"}
    assert rt.call("POST", base + "/moderation", {"hidden": True},
                   viewer="admin")[1]["error"] == "hide_reason_required"
    hidden = rt.call("POST", base + "/moderation",
                     {"hidden": True, "reason": "Abusive"}, viewer="admin")
    assert hidden[0] == 200
    assert hidden[1]["summary"]["count"] == 1
    assert hidden[1]["summary"]["average"] == 5.0
    public = rt.call("GET", "/api/market/reviews/forkmesh/crm")[1]
    assert [r["stars"] for r in public["reviews"]] == [5]
    assert public["total"] == 1
    # The author still sees their own (hidden) review; admins can list it.
    own = rt.call("GET", "/api/market/reviews/forkmesh/crm", viewer="alice")[1]
    assert own["viewer"]["review"]["hidden"] is True
    admin_view = rt.call("GET", "/api/market/reviews/forkmesh/crm",
                         viewer="admin", params={"includeHidden": "1"})[1]
    assert len(admin_view["reviews"]) == 2
    feedback = rt.call("GET", "/api/market/reviews/forkmesh/crm/feedback",
                       viewer="owner")[1]
    assert feedback["items"] == []
    # The decision resolves the open reports, so the queue is empty again.
    assert rt.call("GET", "/api/market/reviews/reports",
                   viewer="admin")[1]["items"] == []
    restored = rt.call("POST", base + "/moderation", {"hidden": False},
                       viewer="admin")
    assert restored[1]["summary"]["count"] == 2
    assert ("admin", "marketplace.review_hide", review_id) in rt.audits


# -- Notifications --------------------------------------------------------------------

def test_every_review_under_five_stars_pings_the_maintainers(rt):
    rt.maintain_first_party(rt.account("maint"), role="maintainer")
    rt.maintain_first_party(rt.account("member"), role="member")
    status, payload = _low(rt, "alice", 2, category="performance")
    assert status == 201 and payload["notified"] == 2
    assert sorted(ping["to"] for ping in rt.pings) == ["maint", "owner"]
    ping = rt.pings[0]
    assert ping["title"] == "2-star review of CRM: how to improve"
    assert ping["body"] == "[Performance] " + IMPROVEMENT
    assert ping["href"] == (
        "/market-reviews?market=forkmesh&package=crm#review-"
        + payload["review"]["id"])
    assert ping["actor"] == "alice"
    assert ping["dedupe"] == "package-review:%s:1" % payload["review"]["id"]
    assert ping["meta"]["stars"] == 2
    rt.pings.clear()
    _rate(rt, "bob", 5)
    assert rt.pings == []
    # An edit that is still under five stars pings again (new revision).
    _low(rt, "alice", 3)
    assert {ping["dedupe"] for ping in rt.pings} == {
        "package-review:%s:2" % payload["review"]["id"]}


def test_community_package_owner_is_pinged(rt):
    rt.publish_community("community-clock", "carol", version="2.0.1")
    status, payload = _low(rt, "alice", 4, package="community-clock",
                           market="forkmesh-community")
    assert status == 201
    assert payload["review"]["version"] == "v2.0.1"
    assert [ping["to"] for ping in rt.pings] == ["carol"]
    feedback = rt.call(
        "GET", "/api/market/reviews/forkmesh-community/community-clock/feedback",
        viewer="carol")
    assert feedback[0] == 200 and len(feedback[1]["items"]) == 1


# -- Contracts ---------------------------------------------------------------------

def test_routes_and_methods():
    assert reviews.route("/api/market/reviews/summary") == ("summary", ())
    assert reviews.route("/api/market/reviews/forkmesh/crm") == (
        "package", ("forkmesh", "crm"))
    assert reviews.route("/api/market/reviews/forkmesh/crm/feedback") == (
        "feedback", ("forkmesh", "crm"))
    assert reviews.route("/api/market/reviews/activity") == ("activity", ())
    assert reviews.route("/api/market/reviews/forkmesh/crm/activity") == (
        "people", ("forkmesh", "crm"))
    assert reviews.route("/api/market/reviews/items/" + "a" * 32 + "/reply") == (
        "item", ("a" * 32, "reply"))
    for bad in ("/api/market/reviews", "/api/market/reviews/items/x/reply",
                "/api/market/reviewsx", "/api/market/reviews/forkmesh/crm/x",
                "/api/market/reviews/other/crm"):
        assert reviews.route(bad) is None


def test_method_origin_and_body_limits(rt):
    assert rt.call("DELETE", "/api/market/reviews/forkmesh/crm",
                   viewer="alice")[0] == 405
    rt.same_origin = lambda: False
    assert rt.call("POST", "/api/market/reviews/forkmesh/crm", {"stars": 5},
                   viewer="alice")[1]["error"] == "origin_not_allowed"
    rt.same_origin = lambda: True
    big = {"stars": 2, "problem": "word " * 10000}
    assert rt.call("POST", "/api/market/reviews/forkmesh/crm", big,
                   viewer="alice")[0] == 413


def test_first_party_package_map_matches_the_catalog():
    catalog = json.loads((APP.parent / "marketplace" / "catalog.json")
                         .read_text(encoding="utf-8"))
    expected = {}
    for entry in catalog["packages"]:
        manifest = json.loads((APP.parent / "marketplace" / entry["manifest"])
                              .read_text(encoding="utf-8"))
        expected[entry["id"]] = manifest["name"]
    assert reviews.FIRST_PARTY_PACKAGES == expected, (
        "Add new catalog packages to marketplace_reviews.FIRST_PARTY_PACKAGES")


def _statements(sql):
    sql = re.sub(r"--[^\n]*", "", sql)
    return [" ".join(part.split()) for part in sql.split(";") if part.strip()]


def test_migration_matches_the_module_schema_and_reverses():
    assert _statements(MIGRATION.read_text(encoding="utf-8")) == [
        " ".join(statement.split()) for statement in reviews.SCHEMA]
    assert _statements(ACTIVITY.read_text(encoding="utf-8")) == [
        " ".join(statement.split()) for statement in reviews.ACTIVITY_SCHEMA]
    db = sqlite3.connect(":memory:")
    db.executescript(MIGRATION.read_text(encoding="utf-8"))
    db.executescript(ACTIVITY.read_text(encoding="utf-8"))
    db.executescript(ACTIVITY_DOWN.read_text(encoding="utf-8"))
    db.executescript(DOWN.read_text(encoding="utf-8"))
    assert not db.execute(
        "SELECT name FROM sqlite_master WHERE name LIKE 'marketplace_%'"
    ).fetchall()


def test_worker_wiring_and_ping_kind():
    runtime_src = (SRC / "app_runtime.py").read_text(encoding="utf-8")
    entry_src = (SRC / "entry.py").read_text(encoding="utf-8")
    adapter = (SRC / "marketplace_runtime.py").read_text(encoding="utf-8")
    pings = (SRC / "ping_settings.py").read_text(encoding="utf-8")
    assert 'url.path.startswith("/api/market/reviews/")' in runtime_src
    assert "marketplace_reviews.handle(" in runtime_src
    assert 'marketplace_reviews = _LazyModule("marketplace_reviews")' in entry_src
    kinds = entry_src[entry_src.index("NOTIFICATION_KINDS = frozenset({"):]
    assert '"package_review",' in kinds[:kinds.index("})")]
    assert '"package_review": True,' in entry_src
    assert '"package_review", self.env' not in adapter
    assert 'self.env, recipient, "package_review", title' in adapter
    assert '("package_review", "repositories",' in pings
