"""Analytics package: ingest, identity, insights, flags, replay, lifecycle."""

import asyncio
import base64
import gzip
import hashlib
import json
from pathlib import Path
import sqlite3
import subprocess
import sys


PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
APP = REPO / "app"
sys.path.insert(0, str(PACKAGE / "content" / "worker"))
import analytics


NOW = 1_790_000_000_000
HOUR = 3_600_000
DAY = analytics.DAY_MS


class Runtime:
    def __init__(self, installed=True):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.execute("CREATE TABLE schema_meta(k TEXT PRIMARY KEY,v TEXT NOT NULL)")
        self.db.executescript(sorted(
            (APP / "migrations").glob("0*_org_plugin_installs.sql"))[-1].read_text())
        self.db.executescript("""
          CREATE TABLE orgs(org_bi TEXT PRIMARY KEY,name TEXT,data TEXT,created_at INTEGER);
          CREATE TABLE org_members(org_bi TEXT,member_bi TEXT,role TEXT,name TEXT,
            created_at INTEGER,PRIMARY KEY(org_bi,member_bi));
          INSERT INTO orgs VALUES('org:acme','acme','sealed:{}',1);
          INSERT INTO org_members VALUES('org:acme','bi:alice','owner','alice',1);
          INSERT INTO org_members VALUES('org:acme','bi:charlie','member','charlie',1);
        """)
        if installed:
            self.db.executescript(";\n".join(analytics.DATA_SCHEMA_UP))
            self.db.execute("INSERT INTO org_plugin_installs VALUES(?,?,?,?,?,?)",
                            ("org:acme", "analytics", "acme", "bi:alice", "alice", 1))
        self.accounts = {name: "bi:" + name for name in ("alice", "bob", "charlie")}
        self.admins = {"alice"}
        self.actor = "alice"
        self.method_value = "GET"
        self.body = {}
        self.params = {}
        self.origin = ""
        self.clock = NOW
        self.ids = 0
        self.audits = []
        analytics._PROJECT_CACHE.clear()
        analytics._DECIDE_CACHE.clear()
        analytics._EVENT_CACHE.clear()

    def use(self, method, actor="alice", body=None, params=None, origin=""):
        self.method_value, self.actor, self.body = method, actor, body or {}
        self.params, self.origin = params or {}, origin
        return self
    def method(self): return self.method_value
    def same_origin(self): return True
    def request_origin(self): return self.origin
    def country(self): return "NL"
    def now(self): self.clock += 1; return self.clock
    def new_id(self): self.ids += 1; return f"{self.ids:032x}"
    def query_params(self): return dict(self.params)
    def response(self, payload, status=200, cache_control=None, extra_headers=None):
        return {"status": status, "data": payload, "headers": extra_headers or {},
                "cache": cache_control}
    async def ensure_schema(self): pass
    async def json_body(self, _limit): return self.body, ""
    async def session(self, _data=None):
        bi = self.accounts.get(self.actor, "")
        return (bi, {"name": self.actor}) if bi else ("", None)
    async def is_admin(self, name): return name in self.admins
    async def organization(self, name, account_bi):
        row = self.db.execute(
            "SELECT o.org_bi,m.role FROM orgs o JOIN org_members m ON "
            "m.org_bi=o.org_bi WHERE o.name=? AND m.member_bi=?", (name, account_bi)
        ).fetchone()
        return (row["org_bi"], row["role"]) if row else ("", "")
    async def plugin_installed(self, org_bi, plugin):
        return bool(self.db.execute(
            "SELECT 1 FROM org_plugin_installs WHERE org_bi=? AND plugin=?",
            (org_bi, plugin)).fetchone())
    async def seal(self, value): return "sealed:" + json.dumps(value, sort_keys=True)
    async def open(self, value): return json.loads(str(value).removeprefix("sealed:"))
    async def d1_all(self, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]
    async def d1_first(self, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row else None
    async def d1_run(self, sql, *args):
        cursor = self.db.execute(sql, args); self.db.commit(); return cursor
    async def audit(self, *args, **kwargs): self.audits.append((args, kwargs))


def call(runtime, path):
    return asyncio.run(analytics.handle(runtime, path))


def project(runtime, **settings):
    created = call(runtime.use("POST", body=dict({"name": "Site"}, **settings)),
                   "/api/analytics/acme/projects")
    assert created["status"] == 201, created
    return created["data"]["project"]


def event(name, distinct="anon-1", ago=HOUR, session="sess-aaaa-0001", **properties):
    return {"event": name, "distinctId": distinct, "ts": NOW - ago,
            "sessionId": session, "properties": properties}


def capture(runtime, key, batch=None, replay=None, origin=""):
    body = {"key": key, "batch": batch or []}
    if replay:
        body["replay"] = replay
    return call(runtime.use("POST", actor="", body=body, origin=origin),
                "/api/analytics/_/capture")


def query(runtime, item, kind, **body):
    body = dict({"kind": kind, "from": NOW - 7 * DAY, "to": NOW + HOUR}, **body)
    result = call(runtime.use("POST", body=body),
                  f"/api/analytics/acme/projects/{item['id']}/query")
    assert result["status"] == 200, result
    return result["data"]["result"]


def pageview(path, distinct="anon-1", ago=HOUR, session="sess-aaaa-0001", **extra):
    return event("$pageview", distinct, ago, session, **dict({
        "$pathname": path, "$current_url": "https://example.test" + path,
        "$browser": "Firefox", "$os": "Linux", "$device_type": "Desktop",
        "$viewport_width": 1280, "$viewport_height": 720}, **extra))


def test_capture_is_public_sealed_and_never_needs_a_session():
    runtime = Runtime()
    item = project(runtime)
    assert item["writeKey"].startswith("fma_") and len(item["writeKey"]) == 44
    sent = capture(runtime, item["writeKey"], [
        pageview("/pricing", **{"$referrer": "https://news.example/item?id=1"}),
        event("signup", email_hint="a@example.test")])
    assert sent["status"] == 200 and sent["data"]["accepted"] == 2
    assert sent["headers"]["access-control-allow-origin"] == "*"
    stored = runtime.db.execute("SELECT data,events FROM analytics_batches").fetchone()
    assert stored["events"] == 2 and stored["data"].startswith("sealed:")
    # The edge country is attached server-side; no address is ever recorded.
    assert '"$geo_country": "NL"' in stored["data"]
    columns = {row[1] for table in analytics.DATA_TABLES
               for row in runtime.db.execute(f"PRAGMA table_info({table})")}
    assert not {c for c in columns if "ip" == c or c.startswith("ip_") or "address" in c}
    assert capture(runtime, "fma_" + "0" * 40, [pageview("/")])["status"] == 401
    assert capture(runtime, "nonsense", [pageview("/")])["status"] == 401


def test_allowed_origins_and_daily_quota_bound_a_public_key():
    runtime = Runtime()
    item = project(runtime, allowedOrigins=["https://example.test/"], eventQuota=3)
    key = item["writeKey"]
    assert item["settings"]["allowedOrigins"] == ["https://example.test"]
    assert capture(runtime, key, [pageview("/")], origin="https://evil.test")["status"] == 403
    assert capture(runtime, key, [pageview("/")], origin="https://example.test")["status"] == 200
    # A server-side caller has no Origin header; the key is its credential.
    assert capture(runtime, key, [pageview("/a"), pageview("/b")])["status"] == 200
    assert capture(runtime, key, [pageview("/c")])["status"] == 429
    usage = call(runtime.use("GET"), f"/api/analytics/acme/projects/{item['id']}")
    assert usage["data"]["project"]["usage"]["events"] == 3


def test_identify_merges_the_anonymous_history_into_one_person():
    runtime = Runtime()
    item = project(runtime)
    key = item["writeKey"]
    capture(runtime, key, [pageview("/", "anon-1", 5 * HOUR)])
    capture(runtime, key, [dict(event("$identify", "user-42", 4 * HOUR,
                                      **{"$anon_distinct_id": "anon-1"}),
                                set={"email": "a@example.test", "plan": "pro"},
                                setOnce={"first_plan": "free"})])
    capture(runtime, key, [event("upgrade", "user-42", 3 * HOUR)])
    people = call(runtime.use("GET", params={"identified": "1"}),
                  f"/api/analytics/acme/projects/{item['id']}/persons")["data"]
    assert people["total"] == 1 and people["identified"] == 1
    person = people["persons"][0]
    assert person["properties"] == {"email": "a@example.test", "plan": "pro",
                                    "first_plan": "free"}
    assert set(person["distinctIds"]) == {"anon-1", "user-42"} and person["events"] == 3
    # Insights count the merged visitor once.
    trend = query(runtime, item, "trends", series=[{"event": "*", "math": "dau"}])
    assert trend["series"][0]["total"] == 1
    found = call(runtime.use("GET", params={"search": "A@EXAMPLE"}),
                 f"/api/analytics/acme/projects/{item['id']}/persons")["data"]
    assert [p["id"] for p in found["persons"]] == [person["id"]]
    # A second identified user on the same browser stays a separate person.
    capture(runtime, key, [event("$identify", "user-77", 2 * HOUR,
                                 **{"$anon_distinct_id": "user-42"})])
    assert call(runtime.use("GET", params={"identified": "1"}),
                f"/api/analytics/acme/projects/{item['id']}/persons")["data"]["total"] == 2


def test_trends_breakdown_maths_and_person_filters():
    runtime = Runtime()
    item = project(runtime)
    key = item["writeKey"]
    capture(runtime, key, [dict(event("$identify", "u1", 30 * HOUR), set={"plan": "pro"})])
    capture(runtime, key, [dict(event("$identify", "u2", 30 * HOUR), set={"plan": "free"})])
    capture(runtime, key, [event("purchase", "u1", 26 * HOUR, amount=10, region="eu"),
                           event("purchase", "u1", 2 * HOUR, amount=30, region="eu")])
    capture(runtime, key, [event("purchase", "u2", 2 * HOUR, amount=5, region="us")])
    total = query(runtime, item, "trends", interval="day", series=[
        {"event": "purchase", "math": "total"},
        {"event": "purchase", "math": "sum", "mathProperty": "amount"},
        {"event": "purchase", "math": "dau"}])
    assert [row["total"] for row in total["series"]] == [3, 45, 2]
    assert sum(total["series"][0]["data"]) == 3 and len(total["labels"]) >= 7
    by_region = query(runtime, item, "trends", series=[{"event": "purchase"}],
                      breakdown={"type": "event", "property": "region"})
    assert {row["breakdown"]: row["total"] for row in by_region["series"]} == {
        "eu": 2, "us": 1}
    pro = query(runtime, item, "trends", series=[{"event": "purchase"}], filters=[
        {"type": "person", "property": "plan", "operator": "exact", "value": "pro"}])
    assert pro["series"][0]["total"] == 2
    big = query(runtime, item, "trends", series=[{"event": "purchase", "properties": [
        {"property": "amount", "operator": "gte", "value": 10}]}])
    assert big["series"][0]["total"] == 2


def test_concurrent_insights_share_one_batch_read_and_see_new_captures():
    runtime = Runtime()
    item = project(runtime)
    key = item["writeKey"]
    capture(runtime, key, [event("purchase", "u1", 2 * HOUR)])
    reads = []
    plain = runtime.d1_all

    async def counted(sql, *args):
        if "FROM analytics_batches WHERE project_id=? AND day>=?" in sql:
            reads.append(args)
            await asyncio.sleep(0.01)
        return await plain(sql, *args)

    runtime.d1_all = counted

    async def burst():
        return await asyncio.gather(*[
            analytics._load_events(runtime, item["id"], NOW - days * DAY, NOW + HOUR)
            for days in (7, 7, 3, 1, 7, 2)])

    loaded = asyncio.run(burst())
    assert len(reads) == 1
    assert [len(events) for events, _ in loaded] == [1] * 6
    assert all(truncated is False for _, truncated in loaded)
    loaded[0][0].clear()
    assert len(asyncio.run(analytics._load_events(
        runtime, item["id"], NOW - DAY, NOW + HOUR))[0]) == 1
    assert len(reads) == 1
    capture(runtime, key, [event("purchase", "u2", HOUR)])
    assert query(runtime, item, "trends", series=[
        {"event": "purchase"}])["series"][0]["total"] == 2
    assert len(reads) == 2
    assert len(asyncio.run(analytics._load_events(
        runtime, item["id"], NOW - 30 * DAY, NOW + HOUR))[0]) == 2
    assert len(reads) == 3


def test_funnel_orders_steps_inside_the_conversion_window():
    runtime = Runtime()
    item = project(runtime)
    key = item["writeKey"]
    steps = [{"event": "$pageview", "properties": [
        {"property": "$pathname", "operator": "exact", "value": "/pricing"}]},
        {"event": "signup"}, {"event": "purchase"}]
    capture(runtime, key, [pageview("/pricing", "a", 10 * HOUR, "sess-aaaa-000a"),
                           event("signup", "a", 9 * HOUR, "sess-aaaa-000a"),
                           event("purchase", "a", 8 * HOUR, "sess-aaaa-000a")])
    capture(runtime, key, [pageview("/pricing", "b", 10 * HOUR, "sess-aaaa-000b"),
                           event("signup", "b", 9 * HOUR, "sess-aaaa-000b")])
    # c signs up *before* viewing pricing: step two never follows step one.
    capture(runtime, key, [event("signup", "c", 10 * HOUR, "sess-aaaa-000c"),
                           pageview("/pricing", "c", 9 * HOUR, "sess-aaaa-000c")])
    result = query(runtime, item, "funnel", steps=steps)
    assert [step["count"] for step in result["steps"]] == [3, 2, 1]
    assert result["steps"][1]["dropoff"] == 1
    assert result["steps"][2]["conversion"] == round(1 / 3, 4)
    assert result["steps"][1]["medianTimeMs"] == HOUR
    tight = query(runtime, item, "funnel", steps=steps, windowSeconds=1800)
    assert [step["count"] for step in tight["steps"]] == [3, 0, 0]
    loose = query(runtime, item, "funnel", steps=steps, order="unordered")
    assert loose["steps"][1]["count"] == 3
    assert query(runtime, item, "funnel", steps=steps[:1])["error"] == "funnel_needs_two_steps"


def test_retention_paths_stickiness_and_lifecycle():
    runtime = Runtime()
    item = project(runtime)
    key = item["writeKey"]
    for distinct, days in (("a", (3, 2, 1)), ("b", (3, 1)), ("c", (2,))):
        for day in days:
            capture(runtime, key, [
                pageview("/", distinct, day * DAY, f"sess-{distinct}-day{day}"),
                pageview("/docs", distinct, day * DAY - 60000, f"sess-{distinct}-day{day}")])
    kept = query(runtime, item, "retention", period="day", periods=5,
                 targetEvent={"event": "$pageview"})
    first = next(row for row in kept["rows"] if row["size"] == 2)
    assert first["values"][:3] == [2, 1, 2] and first["percentages"][1] == 0.5
    flow = query(runtime, item, "paths")
    assert flow["edges"][0] == {"source": "1_/", "target": "2_/docs", "value": 6}
    sticky = query(runtime, item, "stickiness", series=[{"event": "$pageview"}])
    assert sticky["series"][0]["data"][:3] == [1, 1, 1]
    life = query(runtime, item, "lifecycle", series=[{"event": "$pageview"}])
    rows = {row["name"]: row["data"] for row in life["series"]}
    assert sum(rows["new"]) == 3 and sum(rows["returning"]) == 2
    assert sum(rows["resurrecting"]) == 1 and min(rows["dormant"]) < 0


def test_overview_reports_web_analytics_with_the_previous_period():
    runtime = Runtime()
    item = project(runtime)
    key = item["writeKey"]
    capture(runtime, key, [
        pageview("/", "a", 3 * HOUR, "sess-aaaa-000a",
                 **{"$referrer": "https://news.example/x", "utm_source": "hn"}),
        pageview("/pricing", "a", 3 * HOUR - 30000, "sess-aaaa-000a")])
    capture(runtime, key, [pageview("/", "b", 2 * HOUR, "sess-aaaa-000b")])
    result = query(runtime, item, "overview", **{"from": NOW - DAY})
    assert result["totals"]["visitors"] == 2 and result["totals"]["pageviews"] == 3
    assert result["totals"]["sessions"] == 2 and result["totals"]["bounceRate"] == 0.5
    assert result["previous"]["visitors"] == 0
    breakdowns = result["breakdowns"]
    assert breakdowns["pages"][0] == {"value": "/", "count": 2, "visitors": 2}
    assert {row["value"] for row in breakdowns["referrers"]} == {"news.example", "(direct)"}
    assert breakdowns["sources"] == [{"value": "hn", "count": 1}]
    assert breakdowns["countries"] == [{"value": "NL", "count": 3}]
    assert breakdowns["exitPages"][0]["value"] in ("/pricing", "/")


def test_overview_reports_referrals_channels_sessions_and_engagement():
    runtime = Runtime()
    item = project(runtime)
    key = item["writeKey"]
    own = {"$host": "example.test", "$language": "en-US",
           "$screen_width": 1920, "$screen_height": 1080}
    capture(runtime, key, [
        pageview("/", "a", 3 * HOUR, "sess-ref-000a", **own,
                 **{"$referrer": "https://news.ycombinator.com/item?id=7#top"}),
        pageview("/docs", "a", 3 * HOUR - 30000, "sess-ref-000a", **own,
                 **{"$referrer": "https://example.test/"}),
        event("$pageleave", "a", 3 * HOUR - 20000, "sess-ref-000a",
              **{"$pathname": "/docs", "$duration_ms": 10000,
                 "$max_scroll_y": 300, "$viewport_height": 700,
                 "$page_height": 2000})])
    capture(runtime, key, [
        pageview("/", "b", 2 * HOUR, "sess-ref-000b", **own,
                 **{"$referrer": "https://www.google.com/search"}),
        pageview("/", "c", HOUR, "sess-ref-000c", **own,
                 **{"$referrer": "https://www.example.test/blog"}),
        pageview("/", "c", 4 * DAY, "sess-ref-000d", **own)])
    result = query(runtime, item, "overview", **{"from": NOW - 3 * DAY})
    breakdowns = result["breakdowns"]
    assert breakdowns["referringDomains"] == [
        {"value": "news.ycombinator.com", "count": 1},
        {"value": "google.com", "count": 1}]
    assert breakdowns["referringUrls"][0] == {
        "value": "https://news.ycombinator.com/item?id=7", "count": 1}
    assert {row["value"]: row["count"] for row in breakdowns["channels"]} == {
        "Social": 1, "Organic search": 1, "Direct": 1}
    assert breakdowns["screens"] == [{"value": "1920×1080", "count": 4}]
    assert breakdowns["languages"] == [{"value": "en-US", "count": 4}]
    assert result["visitorKinds"] == {"new": 2, "returning": 1}
    depth = {row["value"]: row["count"] for row in result["sessionDepth"]}
    assert depth["1"] == 2 and depth["2"] == 1
    assert sum(sum(row) for row in result["hours"]) == 4
    assert result["engagement"][0] == {
        "value": "/docs", "count": 1, "medianMs": 10000, "scroll": 0.5}
    assert len(result["sessionSeries"]["bounceRate"]) == len(
        result["timeseries"]["labels"])


def test_campaigns_attribute_sessions_bounces_and_goals_to_their_first_touch():
    runtime = Runtime()
    item = project(runtime)
    key = item["writeKey"]
    tagged = {"utm_source": "reddit", "utm_medium": "social",
              "utm_campaign": "launch", "utm_content": "post-a"}
    capture(runtime, key, [
        pageview("/", "a", 3 * HOUR, "sess-camp-000a", **tagged),
        pageview("/download", "a", 3 * HOUR - 40000, "sess-camp-000a", **tagged),
        event("forkmesh_product_action", "a", 3 * HOUR - 39000, "sess-camp-000a",
              action="home-install-hero", **tagged)])
    capture(runtime, key, [pageview("/", "b", 2 * HOUR, "sess-camp-000b", **tagged)])
    capture(runtime, key, [
        pageview("/docs", "c", 2 * HOUR, "sess-camp-000c",
                 **{"$referrer": "https://www.google.com/search?q=x"}),
        pageview("/", "d", HOUR, "sess-camp-000d",
                 **{"$referrer": "https://example.test/blog"}),
        pageview("/", "e", 4 * DAY, "sess-camp-000e", **tagged)])
    goals = [{"event": "forkmesh_product_action", "label": "Install click"}]
    window = {"from": NOW - 3 * DAY}
    result = query(runtime, item, "campaigns", goals=goals, host="example.test",
                   **window)
    rows = {row["key"]: row for row in result["rows"]}
    launch = rows["reddit / social / launch"]
    assert (launch["sessions"], launch["visitors"], launch["bounces"]) == (2, 2, 1)
    assert (launch["conversions"], launch["conversionRate"]) == (1, 0.5)
    assert launch["goals"] == [1] and launch["previousSessions"] == 1
    assert launch["topLanding"] == [{"value": "/", "count": 2}]
    assert rows["google.com / organic / (none)"]["tagged"] is False
    assert rows["(direct) / (none) / (none)"]["sessions"] == 1
    assert result["goals"] == ["Install click"]
    assert result["totals"]["sessions"] == 4 and result["totals"]["taggedShare"] == 0.5
    assert result["previous"]["sessions"] == 1
    content = query(runtime, item, "campaigns", groupBy="content", **window)
    assert content["rows"][0]["content"] == "post-a"
    assert content["rows"][0]["conversions"] is None
    landing = query(runtime, item, "campaigns", groupBy="landing", **window)
    assert {row["landing"] for row in landing["rows"]} == {"/", "/docs"}
    terms = query(runtime, item, "campaigns", groupBy="term", host="example.test", **window)
    assert {row["term"]: row["sessions"] for row in terms["rows"]} == {
        "(none)": 3, "(not provided)": 1}
    assert next(row for row in terms["rows"] if row["term"] == "(not provided)")[
        "topSources"] == [{"value": "google.com / organic", "count": 1}]
    capture(runtime, key, [pageview("/", "f", HOUR, "sess-camp-000f", utm_source="google",
                                    utm_medium="cpc", utm_term="Git  Mirror")])
    terms = query(runtime, item, "campaigns", groupBy="term", **window)
    assert {row["term"] for row in terms["rows"]} >= {"git mirror"}

    report = asyncio.run(analytics.campaign_report(
        runtime, "org:acme", {"from": NOW - 3 * DAY, "to": NOW + HOUR,
                              "goals": goals, "groups": ["landing", "content"]}))
    assert report["status"] == "ok" and report["project"]["id"] == item["id"]
    assert report["lastEventAt"] > 0 and set(report["results"]) == {"content", "landing"}
    assert report["results"]["content"]["totals"]["conversions"] == 1
    assert report["results"]["landing"]["rows"][0]["landing"] == "/"
    assert asyncio.run(analytics.campaign_report(
        Runtime(installed=False), "org:acme", {}))["status"] == "not_installed"
    assert asyncio.run(analytics.campaign_report(
        Runtime(), "org:acme", {}))["status"] == "no_project"


def test_flags_roll_out_deterministically_and_target_people():
    runtime = Runtime()
    item = project(runtime)
    base = f"/api/analytics/acme/projects/{item['id']}/objects"
    created = call(runtime.use("POST", body={
        "kind": "flag", "key": "pricing-test", "name": "Pricing", "active": True,
        "variants": [{"key": "control", "percent": 50}, {"key": "test", "percent": 50}],
        "payloads": {"test": {"price": 12}}}), base)
    assert created["status"] == 201
    assert call(runtime.use("POST", body={"kind": "flag", "key": "pricing-test"}),
                base)["data"]["error"] == "flag_key_taken"
    assert call(runtime.use("POST", body={"kind": "flag", "key": "bad", "variants": [
        {"key": "a", "percent": 10}]}), base)["data"]["error"] == \
        "variant_percentages_must_total_100"
    call(runtime.use("POST", body={"kind": "flag", "key": "pro-only", "active": True,
                                   "conditions": [{"rolloutPercent": 100, "properties": [
                                       {"property": "plan", "operator": "exact",
                                        "value": "pro"}]}]}), base)
    call(runtime.use("POST", body={"kind": "flag", "key": "off", "active": False}), base)
    seen = {}
    for number in range(200):
        decided = call(runtime.use("POST", actor="", body={
            "key": item["writeKey"], "distinctId": f"visitor-{number}"}),
            "/api/analytics/_/decide")["data"]
        assert "off" not in decided["flags"] and "pro-only" not in decided["flags"]
        variant = decided["flags"]["pricing-test"]
        seen[variant] = seen.get(variant, 0) + 1
        if variant == "test":
            assert decided["payloads"]["pricing-test"] == {"price": 12}
    assert set(seen) == {"control", "test"} and min(seen.values()) > 60
    again = call(runtime.use("POST", actor="", body={
        "key": item["writeKey"], "distinctId": "visitor-7"}), "/api/analytics/_/decide")
    assert again["data"]["flags"]["pricing-test"] == analytics.evaluate_flag(
        {"key": "pricing-test", "active": True, "conditions": [
            {"properties": [], "rolloutPercent": 100, "variant": ""}],
         "variants": [{"key": "control", "percent": 50}, {"key": "test", "percent": 50}]},
        "visitor-7", {})
    pro = call(runtime.use("POST", actor="", body={
        "key": item["writeKey"], "distinctId": "v", "personProperties": {"plan": "pro"}}),
        "/api/analytics/_/decide")["data"]
    assert pro["flags"]["pro-only"] is True
    assert pro["replay"]["enabled"] is False and pro["autocapture"] is True
    half = {"key": "half", "active": True, "variants": [],
            "conditions": [{"properties": [], "rolloutPercent": 50, "variant": ""}]}
    share = sum(1 for n in range(2000) if analytics.evaluate_flag(half, f"u{n}", {}))
    assert 880 < share < 1120


def test_experiment_compares_variants_against_control():
    runtime = Runtime()
    item = project(runtime)
    batch = []
    for number in range(300):
        variant = "control" if number % 2 == 0 else "test"
        distinct = f"u{number}"
        batch.append(event("$feature_flag_called", distinct, 5 * HOUR, f"sess-exp-{number:04d}",
                           **{"$feature_flag": "pricing-test",
                              "$feature_flag_response": variant}))
        converts = number % 10 == 0 if variant == "control" else number % 3 == 0
        if converts:
            batch.append(event("purchase", distinct, 4 * HOUR, f"sess-exp-{number:04d}"))
    for start in range(0, len(batch), 100):
        assert capture(runtime, item["writeKey"], batch[start:start + 100])["status"] == 200
    result = query(runtime, item, "experiment", flagKey="pricing-test",
                   goal={"event": "purchase"})
    rows = {row["variant"]: row for row in result["variants"]}
    assert rows["control"]["exposures"] == 150 and rows["test"]["exposures"] == 150
    assert rows["test"]["conversionRate"] > rows["control"]["conversionRate"]
    assert rows["test"]["significant"] and rows["test"]["probabilityToBeatControl"] > 0.975
    assert rows["control"]["probabilityToBeatControl"] is None


def site_action(action, stage, distinct, ago, session):
    return event("forkmesh_product_action", distinct, ago, session,
                 action=action, stage=stage, **{"$pathname": "/"})


def test_playbook_on_a_project_keeps_an_existing_flag_with_the_same_key():
    runtime = Runtime()
    item = project(runtime)
    path = f"/api/analytics/acme/projects/{item['id']}"
    call(runtime.use("POST", body={"kind": "flag", "key": "home-hero-headline",
                                   "active": False}), path + "/objects")
    added = call(runtime.use("POST"), path + "/playbook")["data"]["added"]
    assert added == {"goal": 4, "insight": 2, "flag": 0, "experiment": 1}
    flags = call(runtime.use("GET", params={"kind": "flag"}),
                 path + "/objects")["data"]["objects"]
    assert len(flags) == 1 and flags[0]["active"] is False


def test_cohorts_actions_and_groups_feed_the_insights():
    runtime = Runtime()
    item = project(runtime)
    key = item["writeKey"]
    base = f"/api/analytics/acme/projects/{item['id']}/objects"
    capture(runtime, key, [dict(event("$identify", "u1", 9 * HOUR), set={"plan": "pro"})])
    capture(runtime, key, [dict(event("$identify", "u2", 9 * HOUR), set={"plan": "free"})])
    capture(runtime, key, [
        dict(event("$groupidentify", "u1", 8 * HOUR, **{
            "$group_type": "company", "$group_key": "acme",
            "$group_set": {"seats": 40}})),
        dict(event("$autocapture", "u1", 7 * HOUR, **{
            "$event_type": "click", "$el_text": "Start free trial",
            "$current_url": "https://example.test/pricing",
            "$elements": [{"tag": "button", "id": "cta", "classes": ["btn", "primary"],
                           "attrs": {"data-fm-id": "trial"}}]}),
             groups={"company": "acme"}),
        dict(event("$autocapture", "u2", 7 * HOUR, **{
            "$event_type": "click", "$el_text": "Docs",
            "$elements": [{"tag": "a", "classes": ["nav"]}]}),
             groups={"company": "globex"})])
    cohort = call(runtime.use("POST", body={
        "kind": "cohort", "name": "Pro who clicked", "conditions": [
            {"kind": "property", "property": "plan", "operator": "exact", "value": "pro"},
            {"kind": "event", "event": "$autocapture", "days": 7}]}), base)["data"]["object"]
    action = call(runtime.use("POST", body={
        "kind": "action", "name": "Trial CTA", "steps": [
            {"event": "$autocapture", "selector": "button#cta.primary",
             "url": "/pricing", "text": "free trial"}]}), base)["data"]["object"]
    assert action["steps"][0]["selector"] == "button#cta.primary"
    clicks = query(runtime, item, "trends", series=[{"event": "action:" + action["id"]}])
    assert clicks["series"][0]["total"] == 1 and clicks["series"][0]["name"] == "Trial CTA"
    inside = query(runtime, item, "trends", series=[{"event": "$autocapture"}], filters=[
        {"type": "cohort", "value": cohort["id"]}])
    assert inside["series"][0]["total"] == 1
    outside = query(runtime, item, "trends", series=[{"event": "$autocapture"}], filters=[
        {"type": "cohort", "operator": "is_not", "value": cohort["id"]}])
    assert outside["series"][0]["total"] == 1
    companies = query(runtime, item, "trends", series=[
        {"event": "$autocapture", "math": "unique_group", "groupType": "company"}])
    assert companies["series"][0]["total"] == 2
    groups = call(runtime.use("GET", params={"type": "company"}),
                  f"/api/analytics/acme/projects/{item['id']}/groups")["data"]["groups"]
    assert groups == [dict(groups[0], type="company", key="acme",
                           properties={"seats": 40})]
    big = query(runtime, item, "trends", series=[{"event": "$autocapture"}], filters=[
        {"type": "group", "groupType": "company", "property": "seats",
         "operator": "gt", "value": 10}])
    assert big["series"][0]["total"] == 1


def test_recordings_keep_the_trail_of_pages_viewed():
    runtime = Runtime()
    item = project(runtime)
    call(runtime.use("PATCH", body={"replay": {"enabled": True}}),
         f"/api/analytics/acme/projects/{item['id']}")
    chunk = {"sessionId": "sess-pages-0001", "distinctId": "anon-2", "seq": 0,
             "startedAt": NOW - 60000, "encoding": "json",
             "events": json.dumps([{"t": NOW - 60000, "k": "meta", "d": {}}])}
    for seq, url in enumerate(["https://example.test/", "https://example.test/",
                               "https://example.test/docs", "https://example.test/pricing",
                               "https://example.test/"]):
        capture(runtime, item["writeKey"], [], dict(chunk, seq=seq, url=url))
    listing = call(runtime.use("GET"),
                   f"/api/analytics/acme/projects/{item['id']}/recordings")["data"]
    trail = ["https://example.test/", "https://example.test/docs",
             "https://example.test/pricing", "https://example.test/"]
    assert listing["recordings"][0]["pages"] == trail
    assert listing["recordings"][0]["startUrl"] == "https://example.test/"
    played = call(runtime.use("GET"), f"/api/analytics/acme/projects/{item['id']}"
                  "/recordings/sess-pages-0001")["data"]
    assert played["recording"]["pages"] == trail


def test_replay_chunks_are_sealed_paged_and_gated_by_the_project_setting():
    runtime = Runtime()
    item = project(runtime)
    key = item["writeKey"]
    chunk = {"sessionId": "sess-aaaa-0001", "distinctId": "anon-1", "seq": 0,
             "startedAt": NOW - 60000, "url": "https://example.test/", "encoding": "json",
             "events": json.dumps([{"t": NOW - 60000, "k": "meta", "d": {}}]),
             "activeMs": 5000, "clicks": 2, "keys": 4,
             "viewport": {"width": 1280, "height": 720}}
    off = capture(runtime, key, [pageview("/")], chunk)
    assert off["data"]["replayError"] == "replay_disabled"
    call(runtime.use("PATCH", body={"replay": {"enabled": True, "sampleRate": 50}}),
         f"/api/analytics/acme/projects/{item['id']}")
    assert capture(runtime, key, [event("$exception", **{
        "$exception_type": "TypeError", "$exception_message": "x is undefined"})],
        chunk)["data"]["replayBytes"] == len(chunk["events"])
    capture(runtime, key, [], dict(chunk, seq=1, clicks=1))
    # A retried chunk is stored once.
    capture(runtime, key, [], dict(chunk, seq=1, clicks=1))
    listing = call(runtime.use("GET", params={"errors": "1"}),
                   f"/api/analytics/acme/projects/{item['id']}/recordings")["data"]
    recording = listing["recordings"][0]
    assert (recording["clicks"], recording["errors"], recording["startUrl"]) == (
        4, 1, "https://example.test/")
    raw = runtime.db.execute("SELECT data FROM analytics_recording_chunks").fetchall()
    assert len(raw) == 2 and all(row["data"].startswith("sealed:") for row in raw)
    played = call(runtime.use("GET"), f"/api/analytics/acme/projects/{item['id']}"
                  "/recordings/sess-aaaa-0001")["data"]
    assert [c["seq"] for c in played["chunks"]] == [0, 1] and played["nextAfterSeq"] == -1
    assert played["recording"]["viewport"] == {"width": 1280, "height": 720}
    failures = query(runtime, item, "errors")["groups"]
    assert failures[0]["type"] == "TypeError" and failures[0]["sessionId"] == "sess-aaaa-0001"
    assert call(runtime.use("DELETE"), f"/api/analytics/acme/projects/{item['id']}"
                "/recordings/sess-aaaa-0001")["data"]["deleted"]
    assert runtime.db.execute(
        "SELECT COUNT(*) FROM analytics_recording_chunks").fetchone()[0] == 0


def test_motion_view_decodes_chunks_into_cursor_scroll_and_page_steps_only():
    runtime = Runtime()
    item = project(runtime)
    call(runtime.use("PATCH", body={"replay": {"enabled": True}}),
         f"/api/analytics/acme/projects/{item['id']}")
    steps = [
        {"t": NOW - 9000, "k": "meta", "d": {"href": "https://example.test/", "width": 1280,
                                             "height": 720, "pageHeight": 3000}},
        {"t": NOW - 9000, "k": "full", "d": {"node": {"id": 1, "type": "doc", "children": [
            {"id": 2, "type": "text", "text": "secret page text"}]}, "scroll": {"x": 0, "y": 40}}},
        {"t": NOW - 8000, "k": "move", "d": {"positions": [[10, 20, 0], [30, 40, 50], ["x"]]}},
        {"t": NOW - 7000, "k": "mouse", "d": {"type": "click", "id": 2, "x": 30, "y": 40}},
        {"t": NOW - 6900, "k": "mouse", "d": {"type": "focus", "id": 2}},
        {"t": NOW - 6000, "k": "scroll", "d": {"id": 1, "x": 0, "y": 500, "h": 3100}},
        {"t": NOW - 5000, "k": "input", "d": {"id": 2, "value": "****"}},
        {"t": NOW - 4000, "k": "mut", "d": {"texts": [{"id": 2, "text": "changed"}]}},
    ]
    raw = json.dumps(steps).encode()
    chunk = {"sessionId": "sess-motion-01", "distinctId": "anon-9", "seq": 0,
             "startedAt": NOW - 9000, "url": "https://example.test/", "encoding": "gzip-b64",
             "events": base64.b64encode(gzip.compress(raw)).decode()}
    capture(runtime, item["writeKey"], [], chunk)
    capture(runtime, item["writeKey"], [], dict(chunk, seq=1, encoding="gzip-b64",
                                                events=base64.b64encode(b"not gzip").decode()))
    path = f"/api/analytics/acme/projects/{item['id']}/recordings"
    listing = call(runtime.use("GET"), path)["data"]["recordings"]
    assert listing[0]["country"] == "NL"
    played = call(runtime.use("GET", params={"view": "motion"}),
                  path + "/sess-motion-01")["data"]
    assert played["recording"]["country"] == "NL"
    first, broken = played["chunks"]
    assert first["encoding"] == "motion" and broken["events"] == []
    events = first["events"]
    assert [event["k"] for event in events] == ["meta", "full", "move", "mouse", "scroll", "input"]
    assert events[0]["d"] == {"href": "https://example.test/", "width": 1280, "height": 720,
                              "pageHeight": 3000}
    assert events[1]["d"] == {"doc": 1, "x": 0, "y": 40}
    assert events[2]["d"]["positions"] == [[10, 20, 0], [30, 40, 50]]
    assert events[3]["d"] == {"type": "click", "x": 30, "y": 40}
    assert events[4]["d"] == {"id": 1, "x": 0, "y": 500, "h": 3100}
    assert events[5]["d"] == {}
    assert "secret page text" not in json.dumps(played) and "****" not in json.dumps(played)
    plain = call(runtime.use("GET"), path + "/sess-motion-01")["data"]
    assert plain["chunks"][0]["encoding"] == "gzip-b64"


def test_heatmap_buckets_clicks_moves_and_scroll_depth_for_one_page():
    runtime = Runtime()
    item = project(runtime)
    click = {"$event_type": "click", "$pathname": "/pricing", "$viewport_width": 1280,
             "$x_ratio": 0.5, "$page_y": 410}
    capture(runtime, item["writeKey"], [
        pageview("/pricing"),
        event("$autocapture", **click), event("$autocapture", **click),
        event("$autocapture", **dict(click, **{"$pathname": "/other"})),
        event("$autocapture", **dict(click, **{"$viewport_width": 390, "$x_ratio": 0.1})),
        event("$heatmap", **{"$pathname": "/pricing", "$viewport_width": 1280,
                             "$heatmap_points": [[0.25, 100], [0.25, 105]]}),
        event("$pageleave", **{"$pathname": "/pricing", "$viewport_width": 1280,
                               "$viewport_height": 700, "$max_scroll_y": 900,
                               "$page_height": 3000})])
    desktop = query(runtime, item, "heatmap", path="/pricing", viewportMin=1024)
    assert desktop["points"] == [[50, 400, 2]] and desktop["pageviews"] == 1
    assert query(runtime, item, "heatmap", path="/pricing")["points"][0] == [50, 400, 2]
    moves = query(runtime, item, "heatmap", path="/pricing", type="move")
    assert moves["points"] == [[25, 100, 2]]
    scroll = query(runtime, item, "heatmap", path="/pricing", type="scroll")
    assert scroll["pageHeight"] == 3000
    assert scroll["scroll"][0]["reached"] == 1 and scroll["scroll"][-1]["reached"] == 0


def test_surveys_are_served_by_decide_and_summarized():
    runtime = Runtime()
    item = project(runtime)
    base = f"/api/analytics/acme/projects/{item['id']}/objects"
    survey = call(runtime.use("POST", body={
        "kind": "survey", "name": "NPS", "active": True,
        "questions": [{"type": "nps", "question": "How likely?"},
                      {"type": "open", "question": "Why?", "optional": True},
                      {"type": "link", "question": "Docs", "link": "javascript:alert(1)"}],
        "targeting": {"url": "/pricing", "waitDays": 14},
        "appearance": {"accent": "red;}"}}), base)["data"]["object"]
    assert survey["questions"][2]["link"] == "" and survey["appearance"]["accent"] == ""
    served = call(runtime.use("POST", actor="", body={
        "key": item["writeKey"], "distinctId": "v"}), "/api/analytics/_/decide")["data"]
    assert served["surveys"][0]["id"] == survey["id"]
    assert served["surveys"][0]["targeting"]["waitDays"] == 14
    long_answer = "Because the onboarding explained every step and the docs are clear."
    batch = []
    for number, score in enumerate((10, 9, 6, 8)):
        distinct = f"r{number}"
        batch.append(event("survey shown", distinct, **{"$survey_id": survey["id"]}))
        batch.append(event("survey sent", distinct, **{
            "$survey_id": survey["id"], "$survey_response": score,
            "$survey_response_1": long_answer}))
    batch.append(event("survey shown", "r9", **{"$survey_id": survey["id"]}))
    batch.append(event("survey dismissed", "r9", **{"$survey_id": survey["id"]}))
    capture(runtime, item["writeKey"], batch)
    result = query(runtime, item, "survey", surveyId=survey["id"])
    assert (result["shown"], result["sent"], result["dismissed"]) == (5, 4, 1)
    assert result["responseRate"] == 0.8 and result["questions"]["0"]["nps"] == 25.0
    assert result["questions"]["1"]["texts"][0]["text"] == long_answer


def test_saved_insights_dashboards_and_annotations_round_trip():
    runtime = Runtime()
    item = project(runtime)
    base = f"/api/analytics/acme/projects/{item['id']}/objects"
    insight = call(runtime.use("POST", actor="charlie", body={
        "kind": "insight", "name": "Signups", "query": {
            "kind": "trends", "series": [{"event": "signup"}]}}), base)["data"]["object"]
    board = call(runtime.use("POST", body={
        "kind": "dashboard", "name": "Growth", "tiles": [
            {"insightId": insight["id"], "width": 9}, {"insightId": "nope"}]}),
        base)["data"]["object"]
    assert board["tiles"] == [{"insightId": insight["id"], "width": 4}]
    renamed = call(runtime.use("PATCH", body={"name": "Weekly signups"}),
                   f"{base}/{insight['id']}")["data"]["object"]
    assert renamed["name"] == "Weekly signups" and renamed["query"]["kind"] == "trends"
    assert renamed["createdBy"] == "charlie" and renamed["updatedBy"] == "alice"
    call(runtime.use("POST", body={"kind": "annotation", "content": "Launch",
                                   "date": NOW}), base)
    listed = call(runtime.use("GET", params={"kind": "annotation"}), base)["data"]
    assert [row["content"] for row in listed["objects"]] == ["Launch"]
    stored = runtime.db.execute("SELECT data FROM analytics_objects").fetchall()
    assert all(row["data"].startswith("sealed:") for row in stored)
    assert call(runtime.use("GET", params={"kind": "sql"}), base)["status"] == 400
    assert call(runtime.use("DELETE"), f"{base}/{insight['id']}")["data"]["deleted"]


def test_membership_roles_and_the_instance_site_key():
    runtime = Runtime()
    assert call(runtime.use("GET", "bob"), "/api/analytics/acme")["status"] == 404
    assert call(runtime.use("GET", ""), "/api/analytics/acme")["status"] == 401
    assert call(runtime.use("POST", "charlie", {"name": "x"}),
                "/api/analytics/acme/projects")["status"] == 403
    item = project(runtime)
    path = f"/api/analytics/acme/projects/{item['id']}"
    assert call(runtime.use("PATCH", "charlie", {"name": "x"}), path)["status"] == 403
    assert call(runtime.use("GET", "charlie"), path)["status"] == 200
    assert capture(runtime, "site", [pageview("/")])["status"] == 401
    runtime.admins = set()
    assert call(runtime.use("PATCH", body={"siteDefault": True}), path)["data"]["error"] == \
        "instance_admin_required"
    runtime.admins = {"alice"}
    assert call(runtime.use("PATCH", body={"siteDefault": True}),
                path)["data"]["project"]["siteDefault"] is True
    assert capture(runtime, "site", [pageview("/")])["status"] == 200
    rotated = call(runtime.use("POST"), path + "/rotate-key")["data"]["project"]
    assert rotated["writeKey"] != item["writeKey"]
    assert capture(runtime, item["writeKey"], [pageview("/")])["status"] == 401
    assert capture(runtime, rotated["writeKey"], [pageview("/")])["status"] == 200
    missing = Runtime(installed=False)
    assert call(missing.use("GET"), "/api/analytics/acme")["data"]["error"] == \
        "plugin_not_installed"
    # A snippet left on a site after uninstall sees an unknown key, not a 500.
    assert capture(missing, "fma_" + "1" * 40, [pageview("/")])["status"] == 401


def pages(runtime, path, method="GET"):
    return call(runtime.use(method, actor="", params={"path": path}),
                "/api/analytics/_/pages")


def test_pages_count_unique_visitors_per_url_for_the_public_docs():
    runtime = Runtime()
    assert pages(runtime, "/docs/intro/")["status"] == 404
    item = project(runtime)
    path = f"/api/analytics/acme/projects/{item['id']}"
    call(runtime.use("PATCH", body={"siteDefault": True}), path)
    assert pages(runtime, "/docs/intro/")["data"] == {
        "ok": True, "path": "/docs/intro", "views": 0, "visitors": 0}
    runtime.db.execute("DROP TABLE analytics_page_visitors")
    runtime.db.execute("DROP TABLE analytics_pages")
    analytics._PAGE_SCHEMA["ready"] = False
    capture(runtime, "site", [pageview("/docs/intro/", "anon-1"),
                              pageview("/docs/intro", "anon-1", HOUR - 10),
                              pageview("/docs/other/", "anon-1"),
                              event("$autocapture", "anon-1", **{"$pathname": "/docs/intro"})])
    capture(runtime, "site", [pageview("/docs/intro/", "anon-2")])
    capture(runtime, "site", [pageview("/docs/intro/", "anon-2")])
    capture(runtime, "site", [pageview("/dashboard", "anon-2")])
    intro = pages(runtime, "/docs/intro/?utm=x#top")
    assert intro["status"] == 200
    assert intro["headers"]["access-control-allow-origin"] == "*"
    assert intro["cache"] == "public, max-age=60"
    assert intro["data"] == {"ok": True, "path": "/docs/intro", "views": 4, "visitors": 2}
    assert pages(runtime, "/docs/other")["data"]["visitors"] == 1
    assert pages(runtime, "/dashboard")["data"]["error"] == "page_not_public"
    assert pages(runtime, "/docsx")["status"] == 404
    assert pages(runtime, "/docs/intro", "POST")["status"] == 405
    capture(runtime, "site", [event("$identify", "user-1", **{"$anon_distinct_id": "anon-1"}),
                              pageview("/docs/intro/", "user-1")])
    assert pages(runtime, "/docs/intro")["data"]["visitors"] == 2
    people = call(runtime.use("GET"), path + "/persons")["data"]["persons"]
    anon_2 = next(p for p in people if p["distinctIds"] == ["anon-2"])
    assert call(runtime.use("DELETE"), f"{path}/persons/{anon_2['id']}")["status"] == 200
    assert pages(runtime, "/docs/intro")["data"]["visitors"] == 1
    exported = asyncio.run(analytics.export_org_data(runtime, "org:acme", "acme"))
    assert {"path": "/docs/intro", "views": 5, "visitors": 1} in \
        exported["projects"][0]["pages"]
    asyncio.run(analytics.purge_org_data(runtime, "org:acme"))
    assert runtime.db.execute("SELECT COUNT(*) FROM analytics_page_visitors").fetchone()[0] == 0
    assert pages(runtime, "/docs/intro")["status"] == 404


def test_erasure_compaction_retention_and_the_organization_lifecycle():
    runtime = Runtime()
    item = project(runtime)
    key = item["writeKey"]
    for number in range(4):
        capture(runtime, key, [pageview("/", "keep", 2 * DAY + number * 1000),
                               pageview("/", "forget", 2 * DAY + number * 1000)])
    assert runtime.db.execute("SELECT COUNT(*) FROM analytics_batches").fetchone()[0] == 4
    asyncio.run(analytics.deliver_reminders(runtime))
    merged = runtime.db.execute(
        "SELECT events,compacted FROM analytics_batches").fetchall()
    assert [(row["events"], row["compacted"]) for row in merged] == [(8, 1)]
    assert query(runtime, item, "trends", series=[{"event": "$pageview"}])[
        "series"][0]["total"] == 8
    people = call(runtime.use("GET"), f"/api/analytics/acme/projects/{item['id']}"
                  "/persons")["data"]["persons"]
    forget = next(p for p in people if p["distinctIds"] == ["forget"])
    path = f"/api/analytics/acme/projects/{item['id']}/persons/{forget['id']}"
    assert call(runtime.use("DELETE", "charlie"), path)["status"] == 403
    assert call(runtime.use("DELETE"), path)["data"]["eventsRemoved"] == 4
    assert "forget" not in runtime.db.execute(
        "SELECT data FROM analytics_batches").fetchone()["data"]
    exported = asyncio.run(analytics.export_org_data(runtime, "org:acme", "acme"))
    assert exported["format"] == "forkmesh-analytics-export-v1"
    assert len(exported["projects"][0]["events"]) == 4
    # Retention drops whole days older than the project's setting.
    call(runtime.use("PATCH", body={"retentionDays": 1}),
         f"/api/analytics/acme/projects/{item['id']}")
    analytics._MAINTENANCE["expired_at"] = 0
    asyncio.run(analytics.deliver_reminders(runtime))
    assert runtime.db.execute("SELECT COUNT(*) FROM analytics_batches").fetchone()[0] == 0
    assert asyncio.run(analytics.org_has_data(runtime, "org:acme"))
    asyncio.run(analytics.purge_org_data(runtime, "org:acme"))
    assert not asyncio.run(analytics.org_has_data(runtime, "org:acme"))
    for statement in analytics.DATA_SCHEMA_DOWN:
        runtime.db.execute(statement)
    left = runtime.db.execute(
        "SELECT name FROM sqlite_master WHERE name LIKE 'analytics_%'").fetchall()
    assert left == []


def test_hostile_input_is_bounded_before_it_is_stored():
    runtime = Runtime()
    item = project(runtime)
    huge = {f"k{n}": "v" * 5000 for n in range(400)}
    sent = capture(runtime, item["writeKey"], [
        dict(event("x" * 900, "d" * 900, **huge), ts=NOW + 400 * DAY),
        {"event": "", "distinctId": "a"}, "not-an-object",
        dict(event("ok"), sessionId="bad session id!")] + [event("bulk")] * 200)
    # The cap applies to what was sent; the two malformed entries inside it
    # are dropped rather than replaced by later ones.
    assert sent["data"]["accepted"] == 98
    stored = json.loads(runtime.db.execute(
        "SELECT data FROM analytics_batches").fetchone()["data"].removeprefix("sealed:"))
    first = stored[0]
    assert len(first["e"]) == 200 and len(first["d"]) == 200
    assert len(first["p"]) <= analytics.MAX_PROPERTIES + 1
    assert all(len(v) <= 1000 for v in first["p"].values() if isinstance(v, str))
    assert abs(first["t"] - NOW) < 1000          # a future clock is corrected
    assert "s" not in stored[1]
    # A pathological regex filter is contained rather than executed unbounded.
    result = query(runtime, item, "trends", series=[{"event": "ok", "properties": [
        {"property": "missing", "operator": "regex", "value": "(a+)+$"}]}])
    assert result["series"][0]["total"] == 0


def test_manifest_page_and_bundle_contracts():
    manifest = json.loads((PACKAGE / "manifest.json").read_text())
    assert manifest["worker"] == {"module": "analytics", "api": "/api/analytics",
                                  "outboundOrigins": []}
    assert manifest["dataLifecycle"] == {"scope": "organization"}
    assert manifest["install"] == {"bundled": True, "default": True}
    page = (PACKAGE / "content" / "web" / "page.js").read_text()
    assert 'registerPage("analytics"' in page and "ForkMeshReplayPlayer" in page
    view = (PACKAGE / "content" / "web" / "view.html").read_text()
    assert 'data-view="analytics"' in view
    sdk = (PACKAGE / "content" / "sdk" / "analytics-sdk.js").read_text()
    assert "/api/analytics/_/capture" in sdk and "/api/analytics/_/decide" in sdk
    for forbidden in ("posthog", "innerHTML = decide", "document.write("):
        assert forbidden not in sdk.lower()
    check = subprocess.run(
        [sys.executable, str(PACKAGE / "tools" / "sync_assets.py"), "--check"],
        capture_output=True, text=True)
    assert check.returncode == 0, check.stdout + check.stderr
    for name in ("page.js", "src/page.ui.js", "src/replay-player.js"):
        digest = hashlib.sha256((PACKAGE / "content" / "web" / name).read_bytes())
        assert digest.hexdigest()


def product_action(action, stage, distinct, ago, session, **extra):
    return event("forkmesh_product_action", distinct, ago, session,
                 action=action, stage=stage, **extra)


def test_goals_live_in_settings_behind_the_objects_api():
    runtime = Runtime()
    item = project(runtime)
    path = f"/api/analytics/acme/projects/{item['id']}/objects"
    created = call(runtime.use("POST", body={
        "kind": "goal", "name": "Install intent", "event": "forkmesh_product_action",
        "properties": [{"property": "stage", "operator": "exact", "value": "install"}]}),
        path)
    assert created["status"] == 201
    goal = created["data"]["object"]
    assert goal["kind"] == "goal" and goal["properties"][0]["type"] == "event"
    listed = call(runtime.use("GET", params={"kind": "goal"}), path)["data"]["objects"]
    assert [entry["id"] for entry in listed] == [goal["id"]]
    renamed = call(runtime.use("PATCH", body={"name": "Install"}), f"{path}/{goal['id']}")
    assert renamed["data"]["object"]["name"] == "Install"
    assert renamed["data"]["object"]["event"] == "forkmesh_product_action"
    settings = call(runtime.use("PATCH", body={"name": "Renamed site"}),
                    f"/api/analytics/acme/projects/{item['id']}")
    assert settings["data"]["project"]["settings"]["goals"][0]["name"] == "Install"
    assert call(runtime.use("DELETE"), f"{path}/{goal['id']}")["data"]["deleted"] is True
    assert call(runtime.use("GET", params={"kind": "goal"}), path)["data"]["objects"] == []
    assert runtime.db.execute("SELECT COUNT(*) FROM analytics_objects").fetchone()[0] == 0


def test_conversions_rate_goals_per_visitor_channel_and_landing_page():
    runtime = Runtime()
    item = project(runtime)
    key = item["writeKey"]
    capture(runtime, key, [
        pageview("/", "a", 5 * HOUR, "sess-aaaa-0001",
                 **{"$referrer": "https://www.google.com/", "$host": "example.test"}),
        product_action("home-install-hero", "install", "a", 5 * HOUR - 10, "sess-aaaa-0001"),
        product_action("home-install-final", "install", "a", 5 * HOUR - 5, "sess-aaaa-0001"),
        pageview("/", "b", 4 * HOUR, "sess-bbbb-0001", **{"$host": "example.test"}),
        pageview("/pricing", "c", 3 * HOUR, "sess-cccc-0001", **{"$host": "example.test"}),
        product_action("home-signup-pricing", "signup", "c", 3 * HOUR - 5, "sess-cccc-0001"),
        pageview("/", "d", 4 * DAY, "sess-dddd-0001", **{"$host": "example.test"}),
        product_action("home-install-hero", "install", "d", 4 * DAY - 5, "sess-dddd-0001"),
    ])
    goals = [{"event": "forkmesh_product_action", "label": "Install intent",
              "properties": [{"property": "stage", "operator": "exact", "value": "install"}]},
             {"event": "forkmesh_product_action", "label": "Signup intent",
              "properties": [{"property": "stage", "operator": "exact", "value": "signup"}]}]
    result = query(runtime, item, "conversions", goals=goals, **{"from": NOW - 3 * DAY})
    assert result["totals"] == {"visitors": 3, "sessions": 3}
    install, signup = result["goals"]
    assert (install["conversions"], install["events"], install["sessions"]) == (1, 2, 1)
    assert install["rate"] == round(1 / 3, 4) and signup["conversions"] == 1
    assert install["previous"] == {"conversions": 1, "sessions": 1, "rate": 1.0}
    assert sum(result["series"][0]["data"]) == 1 and len(result["labels"]) >= 3
    channels = {row["value"]: row for row in result["channels"]}
    assert channels["Organic search"]["conversions"] == [1, 0]
    assert channels["Direct"]["sessions"] == 2
    landing = {row["value"]: row for row in result["landing"]}
    assert landing["/"]["rates"] == [0.5, 0] and landing["/pricing"]["conversions"] == [0, 1]
    assert query(runtime, item, "conversions")["goals"] == []


def test_website_playbook_sets_up_goals_funnels_and_a_running_experiment():
    runtime = Runtime()
    item = project(runtime)
    base = f"/api/analytics/acme/projects/{item['id']}"
    assert item["playbook"]["applied"] == [] and item["playbook"]["latest"] == 1
    assert call(runtime.use("POST", actor="charlie"), base + "/playbook")["status"] == 403
    applied = call(runtime.use("POST"), base + "/playbook")["data"]
    assert applied["added"] == {"goal": 4, "insight": 2, "flag": 1, "experiment": 1}
    assert len(applied["project"]["playbook"]["applied"]) == len(analytics.SITE_PLAYBOOK)
    objects = lambda kind: call(runtime.use("GET", params={"kind": kind}),
                                base + "/objects")["data"]["objects"]
    flag = objects("flag")[0]
    assert flag["key"] == "home-hero-headline" and flag["active"] is True
    assert [v["key"] for v in flag["variants"]] == ["control", "concrete"]
    experiment = objects("experiment")[0]
    assert experiment["status"] == "running" and experiment["startedAt"] > 0
    assert experiment["flagKey"] == "home-hero-headline"
    funnels = [entry for entry in objects("insight") if entry["query"]["kind"] == "funnel"]
    assert sorted(entry["name"] for entry in funnels) == [
        "Homepage → account", "Homepage → install"]
    goal = next(entry for entry in objects("goal") if entry["name"] == "Install intent")
    call(runtime.use("DELETE"), base + "/objects/" + goal["id"])
    again = call(runtime.use("POST"), base + "/playbook")["data"]
    assert again["added"] == {"goal": 0, "insight": 0, "flag": 0, "experiment": 0}
    decided = call(runtime.use("POST", actor="", body={
        "key": item["writeKey"], "distinctId": "visitor-1"}), "/api/analytics/_/decide")
    assert decided["data"]["flags"]["home-hero-headline"] in ("control", "concrete")
    capture(runtime, item["writeKey"], [
        event("$feature_flag_called", "visitor-1", 2 * HOUR, "sess-eeee-0001",
              **{"$feature_flag": "home-hero-headline",
                 "$feature_flag_response": "concrete"}),
        product_action("home-install-hero", "install", "visitor-1", HOUR, "sess-eeee-0001")])
    result = query(runtime, item, "experiment", flagKey="home-hero-headline",
                   goal=experiment["goal"])
    assert result["variants"][0]["variant"] == "concrete"
    assert result["variants"][0]["conversions"] == 1


def test_maintenance_applies_the_playbook_to_the_site_project_once():
    runtime = Runtime()
    other = project(runtime)
    item = project(runtime)
    call(runtime.use("PATCH", body={"siteDefault": True}),
         f"/api/analytics/acme/projects/{item['id']}")
    analytics._MAINTENANCE["expired_at"] = 0
    asyncio.run(analytics.deliver_reminders(runtime))
    kinds = dict(runtime.db.execute(
        "SELECT kind,COUNT(*) FROM analytics_objects WHERE project_id=? GROUP BY kind",
        (item["id"],)).fetchall())
    assert kinds == {"insight": 2, "flag": 1, "experiment": 1}
    assert runtime.db.execute("SELECT COUNT(*) FROM analytics_objects WHERE project_id=?",
                              (other["id"],)).fetchone()[0] == 0
    analytics._MAINTENANCE["expired_at"] = 0
    asyncio.run(analytics.deliver_reminders(runtime))
    assert runtime.db.execute("SELECT COUNT(*) FROM analytics_objects").fetchone()[0] == 4
    fresh = call(runtime.use("GET"), f"/api/analytics/acme/projects/{item['id']}")
    assert fresh["data"]["project"]["playbook"]["version"] == analytics.PLAYBOOK_VERSION
    assert len(fresh["data"]["project"]["settings"]["goals"]) == 4


def test_desktop_overview_lists_recordings_and_plays_back_their_motion():
    desktop = PACKAGE / "content" / "desktop"
    build = json.loads((desktop / "build.json").read_text())
    assert {"AnalyticsReplay.h", "AnalyticsReplay.cpp"} <= set(build["sources"])
    assert "tests/test_analytics_replay.cpp" in build["testSources"]
    feature = (desktop / "AnalyticsFeature.cpp").read_text()
    assert 'projectPath(QStringLiteral("/recordings"))' in feature
    assert "loadRecordings(serial);" in feature
    assert "columns->addWidget(recordingsPanel);" in feature
    assert 'QStringLiteral("Entry page"), QStringLiteral("Pages")' in feature
    replay = (desktop / "AnalyticsReplay.cpp").read_text()
    assert 'QStringLiteral("?afterSeq=%1&view=motion")' in replay


def test_desktop_manages_conversions_funnels_and_experiments():
    desktop = PACKAGE / "content" / "desktop"
    build = json.loads((desktop / "build.json").read_text())
    assert "AnalyticsManage.cpp" in build["sources"]
    feature = (desktop / "AnalyticsFeature.cpp").read_text()
    manage = (desktop / "AnalyticsManage.cpp").read_text()
    for section in ("Overview", "Conversions", "Experiments"):
        assert 'QStringLiteral("%s")' % section in feature
    assert "m_sections->addWidget(buildConversionsPage());" in feature
    assert "m_sections->addWidget(buildExperimentsPage());" in feature
    assert '{QStringLiteral("kind"), QStringLiteral("conversions")}' in manage
    assert 'QStringLiteral("/objects?kind=") + kind' in manage
    assert 'projectPath(QStringLiteral("/playbook"))' in manage
    assert 'body.insert(QStringLiteral("kind"), QStringLiteral("goal"));' in manage
    running = manage.split("void AnalyticsFeature::setExperimentRunning(bool running)", 1)[1].split(
        "\nvoid AnalyticsFeature::", 1)[0]
    assert 'QJsonObject{{QStringLiteral("active"), running}}' in running
    assert 'body.insert(QStringLiteral("endedAt"), now);' in running
    created = manage.split("void AnalyticsFeature::newExperiment()", 1)[1]
    assert '{QStringLiteral("key"), QStringLiteral("control")}' in created
    assert '{QStringLiteral("status"), QStringLiteral("running")}' in created
    assert "if (!canManage())" in created
