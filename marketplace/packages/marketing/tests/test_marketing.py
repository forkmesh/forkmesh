"""Marketing package API, schema, dashboard, and native-surface contracts."""

import ast
import asyncio
from datetime import datetime
import hashlib
import json
import re
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys
from urllib.parse import parse_qs, urlparse

import pytest


PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
APP = REPO / "app"
sys.path.insert(0, str(PACKAGE / "content" / "worker"))
import marketing


class Runtime:
    def __init__(self, installed=True, legacy_channels=False):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.execute("CREATE TABLE schema_meta(k TEXT PRIMARY KEY,v TEXT NOT NULL)")
        self.db.executescript(sorted(
            (APP / "migrations").glob("0*_org_plugin_installs.sql"))[-1].read_text())
        if installed:
            schema = ";\n".join(marketing.DATA_SCHEMA_UP)
            if legacy_channels:
                schema = schema.replace(
                    "CHECK (length(channel) BETWEEN 1 AND 32)",
                    "CHECK (channel IN ('blog','reddit','x','youtube','mastodon'))")
            self.db.executescript(schema)
            self.db.execute("INSERT INTO org_plugin_installs VALUES(?,?,?,?,?,?)",
                            ("org:acme", "marketing", "acme", "bi:alice", "alice", 1))
        self.db.executescript("""
          CREATE TABLE orgs(org_bi TEXT PRIMARY KEY,name TEXT,data TEXT,created_at INTEGER);
          CREATE TABLE org_members(org_bi TEXT,member_bi TEXT,role TEXT,name TEXT,
            created_at INTEGER,PRIMARY KEY(org_bi,member_bi));
          INSERT INTO orgs VALUES('org:acme','acme','sealed:{}',1);
          INSERT INTO org_members VALUES('org:acme','bi:alice','owner','alice',1);
          INSERT INTO org_members VALUES('org:acme','bi:charlie','member','charlie',1);
        """)
        self.accounts = {name: "bi:" + name for name in ("alice", "bob", "charlie")}
        self.actor = "alice"
        self.method_value = "GET"
        self.body = {}
        self.clock = 2_000_000_000_000
        self.ids = 0
        self.audits = []
        self.outbound = {}
        self.outbound_calls = []

    def use(self, method, actor="alice", body=None, params=None):
        self.method_value, self.actor, self.body = method, actor, body or {}
        self.params = params or {}
        return self
    def method(self): return self.method_value
    def query_params(self): return dict(getattr(self, "params", {}))
    def same_origin(self): return True
    def now(self): self.clock += 1; return self.clock
    def new_id(self): self.ids += 1; return f"{self.ids:032x}"
    def response(self, payload, status=200, cache_control=None, extra_headers=None):
        return {"status": status, "data": payload, "headers": extra_headers or {}}
    async def ensure_schema(self): pass
    async def json_body(self, _limit): return self.body, ""
    async def session(self, _data=None):
        bi = self.accounts.get(self.actor, "")
        return (bi, {"name": self.actor}) if bi else ("", None)
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
    async def blind(self, value): return hashlib.sha256(str(value).encode()).hexdigest()
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
    async def outbound_text(self, resource, method="GET", headers=None, body=""):
        self.outbound_calls.append((resource, method, headers or {}, body))
        value = self.outbound.get(resource)
        if callable(value):
            value = value()
        if value is None:
            return {"ok": False, "error": "outbound_origin_not_allowed"}
        status, document, *link = value if isinstance(value, tuple) else (200, value)
        return {"ok": True, "status": status, "data": document,
                **({"link": link[0]} if link else {})}


def call(runtime, path):
    return asyncio.run(marketing.handle(runtime, path))


def test_content_pipeline_is_encrypted_shared_and_filter_ready():
    runtime = Runtime()
    root = call(runtime.use("GET"), "/api/marketing")
    assert root["data"]["organizations"] == [
        {"name": "acme", "role": "owner", "installed": True}]
    created = call(runtime.use("POST", body={
        "title": "Launch story", "body": "Private draft copy", "channel": "blog",
        "status": "review", "campaign": "Launch", "owner": "alice",
        "tags": ["Product", "Launch"], "scheduledAt": 2_000_000_100_000,
    }), "/api/marketing/acme/content")
    assert created["status"] == 201
    item = created["data"]["content"]
    assert (item["channel"], item["status"], item["tags"]) == (
        "blog", "review", ["product", "launch"])
    stored = runtime.db.execute("SELECT data FROM marketing_content").fetchone()[0]
    assert stored.startswith("sealed:") and "Private draft copy" in stored
    member = call(runtime.use("PATCH", "charlie", {
        "channel": "reddit", "status": "scheduled",
    }), f"/api/marketing/acme/content/{item['id']}")
    assert (member["data"]["content"]["channel"],
            member["data"]["content"]["status"]) == ("reddit", "scheduled")
    assert call(runtime.use("GET", "bob"), "/api/marketing/acme")["status"] == 404


def test_latest_channel_metrics_power_dashboard_totals():
    runtime = Runtime()
    for body in (
        {"channel": "x", "reach": 100, "engagements": 5, "impressions": 300},
        {"channel": "x", "reach": 180, "engagements": 12, "impressions": 500},
        {"channel": "youtube", "reach": 400, "engagements": 40, "views": 700},
    ):
        assert call(runtime.use("POST", body=body),
                    "/api/marketing/acme/metrics")["status"] == 201
    summary = call(runtime.use("GET"), "/api/marketing/acme")["data"]["summary"]
    assert (summary["reach"], summary["engagements"]) == (580, 52)
    by_channel = {item["channel"]: item for item in summary["channels"]}
    assert by_channel["x"]["reach"] == 180
    assert by_channel["youtube"]["views"] == 700


def test_x_profile_snapshot_keeps_its_counters_beside_the_channel_metrics():
    runtime = Runtime()
    body = {"channel": "x", "followers": 4567, "following": 123, "posts": 890,
            "replies": 12, "reposts": 7, "likes": 55, "mentions": 9, "media": 40,
            "engagements": 74, "views": 3000, "impressions": 3000,
            "source": "x-profile"}
    created = call(runtime.use("POST", body=body), "/api/marketing/acme/metrics")
    assert created["status"] == 201
    metric = created["data"]["metric"]
    for key, value in body.items():
        assert metric[key] == value
    # A hand-logged snapshot has no profile counters.
    manual = call(runtime.use("POST", body={"channel": "x", "reach": 5}),
                  "/api/marketing/acme/metrics")["data"]["metric"]
    assert (manual["posts"], manual["mentions"], manual["source"]) == (0, 0, "")
    summary = call(runtime.use("GET"), "/api/marketing/acme")["data"]["summary"]
    assert {item["channel"]: item for item in summary["channels"]}["x"]["reach"] == 5
    web = (PACKAGE / "content" / "web" / "page.js").read_text()
    assert 'latest?.source === "x-profile"' in web


def test_channel_settings_are_manager_only_and_https_only():
    runtime = Runtime()
    saved = call(runtime.use("PUT", body={"channels": [
        {"id": "reddit", "enabled": False, "displayName": "Community",
         "profileUrl": "https://reddit.com/r/forkmesh"},
    ]}), "/api/marketing/acme/settings")
    channels = {item["id"]: item for item in saved["data"]["settings"]["channels"]}
    assert channels["reddit"]["enabled"] is False
    assert channels["reddit"]["profileUrl"].startswith("https://")
    assert set(channels) == set(marketing.STARTER_CHANNELS)
    denied = call(runtime.use("PUT", "charlie", {"channels": []}),
                  "/api/marketing/acme/settings")
    assert denied["status"] == 403
    unsafe = call(runtime.use("PUT", body={"channels": [
        {"id": "x", "profileUrl": "http://example.com/profile"},
    ]}), "/api/marketing/acme/settings")
    assert (unsafe["status"], unsafe["data"]) == (
        400, {"error": "invalid_channel_url", "channel": "x"})
    kept = call(runtime.use("GET"), "/api/marketing/acme/settings")["data"]["settings"]
    assert next(item for item in kept["channels"]
                if item["id"] == "reddit")["profileUrl"] == "https://reddit.com/r/forkmesh"


def test_added_channel_links_save_and_scheme_less_urls_become_https():
    runtime = Runtime()
    channels = call(runtime.use("GET"), "/api/marketing/acme")["data"]["settings"]["channels"]
    channels = channels + [
        {"id": "discord", "enabled": True, "displayName": "Discord",
         "profileUrl": "discord.gg/forkmesh"},
        {"id": "github", "enabled": True, "displayName": "GitHub",
         "profileUrl": " https://github.com/forkmesh "},
    ]
    saved = call(runtime.use("PUT", body={"channels": channels}),
                 "/api/marketing/acme/settings")
    assert saved["status"] == 200
    reloaded = call(runtime.use("GET"), "/api/marketing/acme")["data"]
    urls = {item["id"]: item["profileUrl"] for item in reloaded["settings"]["channels"]}
    assert urls["discord"] == "https://discord.gg/forkmesh"
    assert urls["github"] == "https://github.com/forkmesh"
    assert [item["channel"] for item in reloaded["summary"]["channels"]][-2:] == [
        "discord", "github"]
    assert set(reloaded["refreshableChannels"]) == {
        "blog", "reddit", "x", "youtube", "mastodon", "discord", "github",
        "google-search", "email", "app-store"}


def test_a_deleted_channel_leaves_the_dashboard_with_its_numbers_until_added_again():
    runtime = Runtime()
    _configure(runtime, github="https://github.com/forkmesh")
    for channel in ("x", "github", "youtube"):
        call(runtime.use("POST", body={"channel": channel, "reach": 10}),
             "/api/marketing/acme/metrics")
    call(runtime.use("POST", body={"title": "Thread", "channel": "x"}),
         "/api/marketing/acme/content")
    assert call(runtime.use("DELETE", "charlie"),
                "/api/marketing/acme/channels/x")["status"] == 403
    assert call(runtime.use("DELETE"),
                "/api/marketing/acme/channels/nowhere")["status"] == 404
    assert call(runtime.use("GET"), "/api/marketing/acme/channels/x")["status"] == 405
    for channel in ("x", "github"):
        deleted = call(runtime.use("DELETE"), f"/api/marketing/acme/channels/{channel}")
        assert (deleted["status"], deleted["data"]["deleted"]) == (200, channel)
    overview = call(runtime.use("GET"), "/api/marketing/acme")["data"]
    configured = [item["id"] for item in overview["settings"]["channels"]]
    assert "x" not in configured and "github" not in configured
    assert {item["channel"] for item in overview["summary"]["channels"]}.isdisjoint(
        {"x", "github"})
    assert {item["channel"] for item in overview["metrics"]} == {"youtube"}
    assert [item["channel"] for item in overview["content"]] == ["x"]
    assert ("marketing.channel.delete", "organization", "org:acme") == runtime.audits[-1][0][1:]
    channels = [dict(item) for item in overview["settings"]["channels"]]
    call(runtime.use("PUT", body={"channels": channels}), "/api/marketing/acme/settings")
    assert "x" not in [item["id"] for item in call(runtime.use("GET"),
        "/api/marketing/acme/settings")["data"]["settings"]["channels"]]
    channels.append({"id": "x", "enabled": True, "displayName": "X", "profileUrl": ""})
    call(runtime.use("PUT", body={"channels": channels}), "/api/marketing/acme/settings")
    settings = call(runtime.use("GET"), "/api/marketing/acme/settings")["data"]["settings"]
    assert "x" in [item["id"] for item in settings["channels"]]
    assert settings["removedChannels"] == ["github"]
    web = (PACKAGE / "content" / "web" / "page.js").read_text()
    desktop = (PACKAGE / "content" / "desktop" / "MarketingFeature.cpp").read_text()
    assert "data-marketing-channel-delete" in web and "/channels/${" in web
    assert 'QStringLiteral("/channels/") + id' in desktop


def _configure(runtime, **urls):
    channels = call(runtime.use("GET"), "/api/marketing/acme")["data"]["settings"]["channels"]
    configured = {item["id"] for item in channels}
    channels = [dict(item, profileUrl=urls.get(item["id"], item["profileUrl"]))
                for item in channels]
    channels += [{"id": channel, "enabled": True, "displayName": channel,
                  "profileUrl": url} for channel, url in urls.items()
                 if channel not in configured]
    assert call(runtime.use("PUT", body={"channels": channels}),
                "/api/marketing/acme/settings")["status"] == 200


def _refresh(runtime, channel):
    return call(runtime.use("POST", body={"channel": channel}),
                "/api/marketing/acme/refresh")


def test_mastodon_refresh_records_followers_and_imports_timed_posts():
    runtime = Runtime()
    _configure(runtime, mastodon="https://mastodon.social/@forkmesh")
    runtime.outbound["https://mastodon.social/api/v1/accounts/lookup?acct=forkmesh"] = json.dumps({
        "id": "42", "display_name": "Forkmesh", "followers_count": 253,
        "following_count": 81, "statuses_count": 120,
        "note": "<p>Git forge</p>", "url": "https://mastodon.social/@forkmesh"})
    runtime.outbound["https://mastodon.social/api/v1/accounts/42/statuses?limit=40"
                     "&exclude_replies=true&exclude_reblogs=true"] = json.dumps([
        {"id": "1", "created_at": "2026-09-13T22:39:05.685Z",
         "url": "https://mastodon.social/@forkmesh/1", "content": "<p>Custom tools</p>",
         "reblogs_count": 5, "favourites_count": 2, "replies_count": 1, "tags": []}])
    assert call(runtime.use("POST", body={"channel": "mastodon", "reach": 900}),
                "/api/marketing/acme/metrics")["status"] == 201
    refreshed = _refresh(runtime, "mastodon")
    assert refreshed["status"] == 200
    assert refreshed["data"]["refresh"]["imported"] == 1
    assert refreshed["data"]["refresh"]["metrics"]["followers"] == 253
    assert refreshed["data"]["refresh"]["metrics"]["engagements"] == 8
    # Mastodon cannot see reach, so the logged figure carries forward.
    assert refreshed["data"]["refresh"]["metrics"]["reach"] == 900
    assert (refreshed["data"]["refresh"]["metrics"]["following"],
            refreshed["data"]["refresh"]["metrics"]["posts"]) == (81, 120)
    latest = next(item for item in refreshed["data"]["metrics"] if item["channel"] == "mastodon")
    assert (latest["following"], latest["posts"], latest["source"]) == (81, 120, "refresh")
    post = refreshed["data"]["content"][0]
    assert (post["channel"], post["status"], post["title"]) == (
        "mastodon", "published", "Custom tools")
    assert post["publishedAt"] == int(datetime.fromisoformat(
        "2026-09-13T22:39:05.685+00:00").timestamp() * 1000)
    assert _refresh(runtime, "mastodon")["data"]["refresh"]["imported"] == 0


def _mastodon_account(account_id, acct, followers, **extra):
    return {"id": account_id, "acct": acct, "username": acct.split("@")[0],
            "display_name": acct.title(), "url": "https://example.social/@" + acct,
            "note": "<p>Builds things</p>", "followers_count": followers,
            "following_count": 10, "statuses_count": 40,
            "last_status_at": "2033-05-17", **extra}


def test_mastodon_suggestions_rank_people_and_link_searches():
    runtime = Runtime()
    _configure(runtime, mastodon="https://mastodon.social/@forkmesh")
    base = "https://mastodon.social"
    runtime.outbound[base + "/api/v1/timelines/tag/rust?limit=40"] = json.dumps([
        {"account": _mastodon_account("7", "ana@hachyderm.io", 900),
         "tags": [{"name": "Rust"}, {"name": "git"}]},
        {"account": _mastodon_account("8", "forkmesh", 250)},
        {"account": _mastodon_account("9", "botty", 5000, bot=True)}])
    runtime.outbound[base + "/api/v2/search?q=rust&type=accounts&limit=40"] = json.dumps({
        "accounts": [_mastodon_account("7", "ana@hachyderm.io", 900),
                     _mastodon_account("10", "ben", 30)]})
    runtime.outbound[base + "/api/v1/directory?order=active&limit=40"] = json.dumps([
        _mastodon_account("11", "cy", 2, statuses_count=0),
        _mastodon_account("13", "fay", 4, note="<p>Self-hosted everything</p>")])
    runtime.outbound[base + "/api/v1/accounts/lookup?acct=forkmesh"] = json.dumps({"id": "42"})
    runtime.outbound[base + "/api/v1/accounts/42/featured_tags"] = json.dumps([
        {"name": "Forgejo"}])
    runtime.outbound[base + "/api/v1/accounts/42/following?limit=80"] = json.dumps([
        _mastodon_account("12", "dee", 70)])
    runtime.outbound[base + "/api/v1/accounts/42/followers?limit=40"] = json.dumps([
        _mastodon_account("12", "dee", 70), _mastodon_account("14", "gus", 3)])
    runtime.outbound[base + "/api/v1/accounts/42/statuses?limit=20&exclude_replies=true"
                     "&exclude_reblogs=true"] = json.dumps([
        {"id": "500", "reblogs_count": 0, "favourites_count": 0},
        {"id": "501", "reblogs_count": 2, "favourites_count": 0}])
    runtime.outbound[base + "/api/v1/statuses/501/reblogged_by?limit=40"] = json.dumps([
        _mastodon_account("15", "hal", 8)])
    runtime.outbound[base + "/api/v2/search?q=git&type=accounts&limit=40"] = json.dumps({
        "accounts": [_mastodon_account("12", "dee", 70)]})
    runtime.outbound[base + "/api/v1/trends/statuses?limit=40"] = json.dumps([
        {"account": _mastodon_account("16", "ida", 50), "tags": [{"name": "forgejo"}]},
        {"account": _mastodon_account("17", "jo", 90000), "tags": [{"name": "cats"}]}])
    result = call(runtime.use("GET", params={"topics": "Rust, #git"}),
                  "/api/marketing/acme/mastodon")
    assert result["status"] == 200
    mastodon = result["data"]["mastodon"]
    assert (mastodon["server"], mastodon["account"], mastodon["hasToken"],
            mastodon["canFollow"]) == ("mastodon.social", "forkmesh@mastodon.social",
                                      False, False)
    assert mastodon["topics"][:4] == ["rust", "git", "forgejo", "opensource"]
    people = mastodon["suggestions"]
    assert [item["handle"] for item in people] == [
        "ana@hachyderm.io", "gus@mastodon.social", "hal@mastodon.social",
        "ida@mastodon.social", "ben@mastodon.social", "fay@mastodon.social"]
    assert people[0]["reasons"] == ["#rust", "#git"]
    assert people[1]["reasons"] == ["follows you"]
    assert people[2]["reasons"] == ["boosted your post"]
    assert people[5]["reasons"] == ["#selfhosted"]
    assert people[0]["followUrl"] == "https://mastodon.social/@ana@hachyderm.io"
    asked = {url for url, _method, _headers, _body in runtime.outbound_calls}
    assert base + "/api/v1/statuses/500/reblogged_by?limit=40" not in asked
    assert any(url.startswith(base + "/api/v1/timelines/tag/selfhosted?any%5B%5D=devops")
               for url in asked)
    assert mastodon["searches"][0] == {
        "topic": "rust", "tagUrl": "https://mastodon.social/tags/rust",
        "peopleUrl": "https://mastodon.social/search?q=rust&type=accounts"}
    assert any(place["url"] == "https://mastodon.social/directory"
               for place in mastodon["places"])
    assert call(runtime.use("POST", body={"follow": "7"}),
                "/api/marketing/acme/mastodon")["data"]["error"] == (
        "mastodon_access_token_required")


def test_mastodon_follow_uses_the_channel_token_and_hides_followed_people():
    runtime = Runtime()
    _configure(runtime, mastodon="https://mastodon.social/@forkmesh")
    channels = call(runtime.use("GET"), "/api/marketing/acme")["data"]["settings"]["channels"]
    channels = [dict(item, accessToken="secret") if item["id"] == "mastodon" else item
                for item in channels]
    call(runtime.use("PUT", body={"channels": channels}), "/api/marketing/acme/settings")
    base = "https://mastodon.social"
    runtime.outbound[base + "/api/v2/suggestions?limit=40"] = json.dumps([
        {"sources": ["global"], "account": _mastodon_account("7", "ana@hachyderm.io", 900)},
        {"sources": ["global"], "account": _mastodon_account("8", "dee", 90)}])
    runtime.outbound[base + "/api/v1/accounts/relationships?id%5B%5D=7&id%5B%5D=8"] = (
        json.dumps([{"id": "7", "following": False}, {"id": "8", "following": True}]))
    mastodon = call(runtime.use("GET"), "/api/marketing/acme/mastodon")["data"]["mastodon"]
    assert mastodon["canFollow"] is True
    assert [item["handle"] for item in mastodon["suggestions"]] == ["ana@hachyderm.io"]
    assert all(headers.get("authorization") == "Bearer secret"
               for _url, _method, headers, _body in runtime.outbound_calls
               if _url.startswith(base))
    runtime.outbound[base + "/api/v1/accounts/7/follow"] = json.dumps(
        {"id": "7", "following": False, "requested": True})
    followed = call(runtime.use("POST", body={"follow": "7"}), "/api/marketing/acme/mastodon")
    assert followed["data"] == {"ok": True, "id": "7", "relationship": "requested"}
    assert runtime.outbound_calls[-1][:2] == (base + "/api/v1/accounts/7/follow", "POST")
    assert call(runtime.use("POST", "charlie", {"follow": "7"}),
                "/api/marketing/acme/mastodon")["status"] == 403


def test_mastodon_drops_people_the_relationship_check_missed():
    runtime = Runtime()
    _configure(runtime, mastodon="https://mastodon.social/@forkmesh")
    channels = call(runtime.use("GET"), "/api/marketing/acme")["data"]["settings"]["channels"]
    channels = [dict(item, accessToken="secret") if item["id"] == "mastodon" else item
                for item in channels]
    call(runtime.use("PUT", body={"channels": channels}), "/api/marketing/acme/settings")
    base = "https://mastodon.social"
    runtime.outbound[base + "/api/v2/suggestions?limit=40"] = json.dumps([
        {"account": _mastodon_account("7", "ana@hachyderm.io", 900)},
        {"account": _mastodon_account("8", "dee", 90)},
        {"account": _mastodon_account("9", "eve", 500)}])
    runtime.outbound[base + "/api/v1/accounts/relationships?id%5B%5D=7&id%5B%5D=9"] = (
        json.dumps([{"id": "7", "following": False}, {"id": "9", "following": False}]))
    runtime.outbound[base + "/api/v1/accounts/lookup?acct=forkmesh"] = json.dumps({"id": "42"})
    runtime.outbound[base + "/api/v1/accounts/42/following?limit=80"] = json.dumps([
        _mastodon_account("8", "dee", 90)])
    mastodon = call(runtime.use("GET"), "/api/marketing/acme/mastodon")["data"]["mastodon"]
    assert [item["handle"] for item in mastodon["suggestions"]] == [
        "ana@hachyderm.io", "eve@mastodon.social"]
    asked = {url for url, _method, _headers, _body in runtime.outbound_calls}
    assert base + "/api/v1/accounts/relationships?id%5B%5D=7&id%5B%5D=9" in asked
    assert base + "/api/v1/accounts/42/following?limit=80" in asked
    runtime.outbound[base + "/api/v1/accounts/relationships?id%5B%5D=7&id%5B%5D=9"] = (
        json.dumps([{"id": "7", "following": False}]))
    runtime.outbound[base + "/api/v1/accounts/42/following?limit=80"] = json.dumps([
        _mastodon_account("9", "eve", 500), _mastodon_account("8", "dee", 90)])
    mastodon = call(runtime.use("GET"), "/api/marketing/acme/mastodon")["data"]["mastodon"]
    assert [item["handle"] for item in mastodon["suggestions"]] == ["ana@hachyderm.io"]
    assert all(headers.get("authorization") == "Bearer secret"
               for _url, _method, headers, _body in runtime.outbound_calls
               if _url.startswith(base))


def test_mastodon_refresh_keeps_every_followed_handle_and_suggestions_skip_them():
    runtime = Runtime()
    _configure(runtime, mastodon="https://mastodon.social/@forkmesh")
    base = "https://mastodon.social"
    runtime.outbound[base + "/api/v1/accounts/lookup?acct=forkmesh"] = json.dumps({
        "id": "42", "followers_count": 253, "following_count": 81})
    runtime.outbound[base + "/api/v1/accounts/42/statuses?limit=40"
                     "&exclude_replies=true&exclude_reblogs=true"] = json.dumps([])
    first = base + "/api/v1/accounts/42/following?limit=80"
    second = base + "/api/v1/accounts/42/following?limit=80&max_id=900"
    runtime.outbound[first] = (200, json.dumps(
        [_mastodon_account(str(100 + n), "p" + str(n), 5) for n in range(80)]),
        f'<{second}>; rel="next", <{base}/api/v1/accounts/42/following?since_id=1>; rel="prev"')
    runtime.outbound[second] = (200, json.dumps([_mastodon_account("12", "dee", 70)]),
                                f'<{base}/api/v1/accounts/42/following?since_id=2>; rel="prev"')
    refreshed = _refresh(runtime, "mastodon")
    assert refreshed["status"] == 200
    assert "mastodonFollowing" not in refreshed["data"]["settings"]
    stored = json.loads(runtime.db.execute(
        "SELECT data FROM marketing_settings").fetchone()[0].removeprefix("sealed:"))
    assert stored["mastodonFollowing"]["account"] == "forkmesh@mastodon.social"
    assert len(stored["mastodonFollowing"]["handles"]) == 81
    assert "dee@mastodon.social" in stored["mastodonFollowing"]["handles"]

    runtime.outbound[base + "/api/v2/search?q=rust&type=accounts&limit=40"] = json.dumps({
        "accounts": [_mastodon_account("12", "dee", 70), _mastodon_account("10", "ben", 30)]})
    del runtime.outbound[second]
    runtime.outbound_calls.clear()
    mastodon = call(runtime.use("GET", params={"topics": "rust"}),
                    "/api/marketing/acme/mastodon")["data"]["mastodon"]
    assert [item["handle"] for item in mastodon["suggestions"]] == ["ben@mastodon.social"]
    asked = [url for url, _method, _headers, _body in runtime.outbound_calls]
    assert first in asked and second not in asked
    assert "mastodonFollowing" not in call(runtime.use("GET"),
                                           "/api/marketing/acme")["data"]["settings"]
    channels = call(runtime.use("GET"), "/api/marketing/acme")["data"]["settings"]["channels"]
    call(runtime.use("PUT", body={"channels": channels}), "/api/marketing/acme/settings")
    stored = json.loads(runtime.db.execute(
        "SELECT data FROM marketing_settings").fetchone()[0].removeprefix("sealed:"))
    assert len(stored["mastodonFollowing"]["handles"]) == 81


def test_mastodon_suggestions_read_the_whole_following_list_once():
    runtime = Runtime()
    _configure(runtime, mastodon="https://mastodon.social/@forkmesh")
    base = "https://mastodon.social"
    runtime.outbound[base + "/api/v1/accounts/lookup?acct=forkmesh"] = json.dumps({"id": "42"})
    first = base + "/api/v1/accounts/42/following?limit=80"
    second = base + "/api/v1/accounts/42/following?limit=80&max_id=900"
    runtime.outbound[first] = (200, json.dumps([_mastodon_account("13", "fay", 5)]),
                               f'<{second}>; rel="next"')
    runtime.outbound[second] = (200, json.dumps([_mastodon_account("12", "dee", 70)]))
    runtime.outbound[base + "/api/v2/search?q=rust&type=accounts&limit=40"] = json.dumps({
        "accounts": [_mastodon_account("12", "dee", 70), _mastodon_account("10", "ben", 30)]})
    mastodon = call(runtime.use("GET", params={"topics": "rust"}),
                    "/api/marketing/acme/mastodon")["data"]["mastodon"]
    assert [item["handle"] for item in mastodon["suggestions"]] == ["ben@mastodon.social"]
    runtime.outbound_calls.clear()
    call(runtime.use("GET", params={"topics": "rust"}), "/api/marketing/acme/mastodon")
    asked = [url for url, _method, _headers, _body in runtime.outbound_calls]
    assert second not in asked


def test_discord_refresh_prefers_the_organization_connector():
    runtime = Runtime()
    _configure(runtime, discord="https://discord.gg/forkmesh")
    asked = []

    async def discord_guild(org_bi):
        asked.append(org_bi)
        return guild

    runtime.discord_guild = discord_guild
    guild = {"ok": True, "guildId": "100000000000000001", "name": "ForkMesh",
             "description": "Git without the middleman", "vanityCode": "forkmesh",
             "memberCount": 412, "presenceCount": 57}
    refreshed = _refresh(runtime, "discord")
    assert refreshed["status"] == 200
    result = refreshed["data"]["refresh"]
    assert (result["metrics"]["followers"], result["metrics"]["reach"]) == (412, 57)
    assert (result["feed"]["title"], result["feed"]["siteUrl"]) == (
        "ForkMesh", "https://discord.gg/forkmesh")
    assert asked == ["org:acme"] and runtime.outbound_calls == []
    settings = call(runtime.use("GET"), "/api/marketing/acme/settings")["data"]["settings"]
    assert "orgBi" not in next(item for item in settings["channels"]
                               if item["id"] == "discord")

    guild = {"ok": False, "error": "discord_connector_not_connected"}
    runtime.outbound["https://discord.com/api/v10/invites/forkmesh?with_counts=true"] = json.dumps({
        "guild": {"name": "ForkMesh"}, "approximate_member_count": 120,
        "approximate_presence_count": 30})
    fallback = _refresh(runtime, "discord")["data"]["refresh"]
    assert (fallback["metrics"]["followers"], fallback["metrics"]["reach"]) == (120, 30)

    guild = {"ok": False, "error": "discord_authorization_required"}
    del runtime.outbound["https://discord.com/api/v10/invites/forkmesh?with_counts=true"]
    failed = _refresh(runtime, "discord")
    assert (failed["status"], failed["data"]["error"]) == (
        502, "discord_authorization_required")


def test_discord_refresh_counts_connector_messages_as_posting():
    runtime = Runtime()
    _configure(runtime, discord="https://discord.gg/forkmesh")
    asked = []

    async def discord_guild(org_bi):
        return {"ok": True, "guildId": "100000000000000001", "name": "ForkMesh",
                "vanityCode": "forkmesh", "memberCount": 412, "presenceCount": 57}

    async def discord_recent_messages(org_bi, days):
        asked.append((org_bi, days))
        return recent

    runtime.discord_guild = discord_guild
    runtime.discord_recent_messages = discord_recent_messages
    posted = runtime.clock - marketing.DAY_MS
    recent = {"ok": True, "messages": [{"id": "3", "postedAt": posted},
                                       {"id": "2", "postedAt": posted - 5}]}

    def health():
        summary = call(runtime.use("GET"), "/api/marketing/acme")["data"]["summary"]
        return next(item["health"] for item in summary["channels"]
                    if item["channel"] == "discord")

    assert _refresh(runtime, "discord")["status"] == 200
    assert asked == [("org:acme", marketing.DISCORD_ACTIVITY_DAYS)]
    checks = {check["id"]: check["ok"] for check in health()["checks"]}
    assert checks["posting"] and health()["state"] == "good"
    assert health()["lastPostAt"] == posted

    recent = {"ok": False, "error": "discord_channel_required"}
    assert _refresh(runtime, "discord")["status"] == 200
    assert health()["lastPostAt"] == posted


def test_discord_posts_lists_the_connector_posts_of_the_last_six_days():
    runtime = Runtime()
    asked = []

    async def discord_recent_messages(org_bi, days):
        asked.append((org_bi, days))
        return recent

    runtime.discord_recent_messages = discord_recent_messages
    recent = {"ok": True, "guildId": "100000000000000001", "days": 6, "since": 1_000,
              "channels": [{"id": "200000000000000001", "name": "general"}],
              "messages": [{
                  "id": "300000000000000001", "channelId": "200000000000000001",
                  "channelName": "general",
                  "author": {"name": "ada", "bot": False, "avatarUrl": "https://cdn.example/a"},
                  "content": "Shipped 1.29\nRelease notes inside", "createdAt": "x",
                  "editedAt": "2026-10-07T10:00:00+00:00", "postedAt": 1_759_000_000_000,
                  "url": "https://discord.com/channels/1/2/3"}],
              "truncated": False, "contentMissing": False}
    result = call(runtime.use("GET", "charlie"), "/api/marketing/acme/discord-posts")
    assert result["status"] == 200, result
    assert asked == [("org:acme", 6)]
    assert result["data"]["discord"] == {
        "days": 6, "since": 1_000, "channels": ["general"], "truncated": False,
        "contentMissing": False, "posts": [{
            "id": "300000000000000001", "channel": "general", "author": "ada",
            "bot": False, "content": "Shipped 1.29\nRelease notes inside",
            "postedAt": 1_759_000_000_000, "edited": True,
            "url": "https://discord.com/channels/1/2/3"}]}

    for error, status in (("discord_connector_not_connected", 409),
                          ("discord_connector_not_allowed", 409),
                          ("discord_channel_required", 409),
                          ("discord_rate_limited", 502)):
        recent = {"ok": False, "error": error}
        failed = call(runtime.use("GET"), "/api/marketing/acme/discord-posts")
        assert (failed["status"], failed["data"]["error"]) == (status, error)
    del runtime.discord_recent_messages
    absent = call(runtime.use("GET"), "/api/marketing/acme/discord-posts")
    assert (absent["status"], absent["data"]["error"]) == (
        409, "discord_connector_not_connected")
    assert call(runtime.use("POST", body={}),
                "/api/marketing/acme/discord-posts")["status"] == 405
    runtime.db.execute("INSERT INTO org_members VALUES('org:acme','bi:bob','guest','bob',1)")
    runtime.db.commit()
    guest = call(runtime.use("GET", "bob"), "/api/marketing/acme/discord-posts")
    assert (guest["status"], guest["data"]["error"]) == (403, "organization_member_required")


def test_desktop_and_web_show_the_discord_posts_on_the_discord_channel():
    native = (PACKAGE / "content" / "desktop" / "MarketingFeature.cpp").read_text()
    page = (PACKAGE / "content" / "web" / "page.js").read_text()
    manifest = json.loads((PACKAGE / "manifest.json").read_text())
    assert "organization.discord.messages.read" in {
        item["id"] for item in manifest["permissions"]}
    load = native.split("void MarketingFeature::loadDiscordPosts(bool force)", 1)[1].split(
        "\nvoid MarketingFeature::", 1)[0]
    assert 'organizationPath(QStringLiteral("/discord-posts"))' in load
    assert "m_detailDiscordPanel->setVisible(discord);" in native
    assert 'request(orgPath("/discord-posts"))' in page
    assert '${item.channel === "discord" ? discordPostsList() : ""}' in page


def test_the_discord_page_charts_posts_over_time_next_to_views_over_time():
    native = (PACKAGE / "content" / "desktop" / "MarketingFeature.cpp").read_text()
    assert 'm_detailDiscordChartPanel = new DashboardPanel(QStringLiteral("Posts over time"));' in native
    assert """    detailChartsLayout->addWidget(m_detailViewsPanel, 1);
    detailChartsLayout->addWidget(m_detailDiscordChartPanel, 1);""" in native
    assert "m_detailDiscordChartPanel->setVisible(discord);" in native
    render = native.split("void MarketingFeature::renderDiscordPosts()", 1)[1].split(
        "\nvoid MarketingFeature::", 1)[0]
    assert render.rstrip().endswith("renderDiscordChart();\n}")
    chart = native.split("void MarketingFeature::renderDiscordChart()", 1)[1].split(
        "\nvoid MarketingFeature::", 1)[0]
    assert "counts[qMin(days - 1, int((at - start) / kDay))] += 1;" in chart
    assert "m_detailDiscordChart->setSeries(lines, start, end," in chart


def test_every_channel_copies_an_ai_context_that_checks_its_last_advice():
    native = (PACKAGE / "content" / "desktop" / "MarketingFeature.cpp").read_text()
    page = (PACKAGE / "content" / "web" / "page.js").read_text()
    detail = native.split("void MarketingFeature::renderChannelDetail()", 1)[1].split(
        "\nvoid MarketingFeature::", 1)[0]
    assert 'auto *context = action(QStringLiteral("AI context"), QStringLiteral(' in detail
    assert "copyChannelContext(id); });" in detail
    context = native.split("QString MarketingFeature::channelContext(const QString &id) const", 1)[1].split(
        "\nvoid MarketingFeature::", 1)[0]
    copy = native.split("void MarketingFeature::copyChannelContext(const QString &id)", 1)[1].split(
        "\nvoid MarketingFeature::", 1)[0]
    for text in ("## What to do", "Notes for next time", "## Since the last copy",
                 "## Discord posts in the last %1 days", "## Playbook for %1 channels"):
        assert text in context
    assert "QSettings().value(contextKey(organization, id)).toMap()" in context
    assert "QGuiApplication::clipboard()->setText(channelContext(id));" in copy
    assert "QSettings().setValue(contextKey(organization, id)," in copy
    card = page.split("function channelCard(", 1)[1].split("\n  }\n", 1)[0]
    assert '<button data-marketing-channel-context="${escapeHtml(item.channel)}"' in card
    assert "else if (data.marketingChannelContext) copyChannelContext(data.marketingChannelContext);" in page
    web = page.split("function channelContext(channel) {", 1)[1].split("\n  }\n", 1)[0]
    for text in ("## What to do", "Notes for next time", "## Since the last copy"):
        assert text in web
    assert "localStorage.setItem(contextKey(channel)" in page


def test_the_discord_posts_are_read_again_when_the_channel_page_is_opened():
    native = (PACKAGE / "content" / "desktop" / "MarketingFeature.cpp").read_text()
    page = (PACKAGE / "content" / "web" / "page.js").read_text()
    load = native.split("void MarketingFeature::loadDiscordPosts(bool force)", 1)[1].split(
        "\nvoid MarketingFeature::", 1)[0]
    assert "constexpr qint64 kDiscordPostsFreshMs = 60 * 1000;" in native
    assert "if (!force && m_discordPostsOrg == organization && m_discordPostsAt > 0 &&" in load
    assert "now - m_discordPostsAt < kDiscordPostsFreshMs)" in load
    assert "m_discordPostsAt = QDateTime::currentMSecsSinceEpoch();" in load
    assert """    if (discord) {
        renderDiscordPosts();
        loadDiscordPosts();
    }""" in native
    assert native.count("loadDiscordPosts(true)") == 2
    assert "const DISCORD_POSTS_FRESH = 60000;" in page
    assert page.split("function discordPostsList() {", 1)[1].split("}", 1)[0].strip() \
        .startswith("void loadDiscordPosts();")
    assert "if (held && (held.loading || (!force && Date.now() - (held.readAt || 0) " \
        "< DISCORD_POSTS_FRESH))) return;" in page
    assert "{ org, readAt: Date.now(), ...payload.discord }" in page


def _discord_open_url(runtime):
    channels = call(runtime.use("GET"), "/api/marketing/acme")["data"]["settings"]["channels"]
    return next(item["openUrl"] for item in channels if item["id"] == "discord")


def test_discord_open_link_is_a_public_invite_not_a_sign_in_channel_url():
    runtime = Runtime()
    _configure(runtime, discord="https://discord.com/channels/100000000000000001/200")
    assert _discord_open_url(runtime) == "https://discord.com/channels/100000000000000001/200"

    async def discord_guild(org_bi):
        return {"ok": True, "guildId": "100000000000000001", "name": "ForkMesh",
                "vanityCode": "", "memberCount": 412, "presenceCount": 57}

    runtime.discord_guild = discord_guild
    runtime.outbound["https://discord.com/api/guilds/100000000000000001/widget.json"] = json.dumps({
        "name": "ForkMesh", "presence_count": 57,
        "instant_invite": "https://discord.com/invite/AbC123"})
    refreshed = _refresh(runtime, "discord")["data"]["refresh"]
    assert refreshed["feed"]["siteUrl"] == "https://discord.gg/AbC123"
    assert _discord_open_url(runtime) == "https://discord.gg/AbC123"

    runtime.discord_guild = None
    refreshed = _refresh(runtime, "discord")["data"]["refresh"]
    assert refreshed["feed"]["siteUrl"] == "https://discord.gg/AbC123"

    _configure(runtime, discord="https://discord.com/invite/forkmesh")
    assert _discord_open_url(runtime) == "https://discord.gg/forkmesh"


def test_discord_github_youtube_and_reddit_refreshers_read_public_sources():
    runtime = Runtime()
    _configure(runtime, discord="https://discord.gg/forkmesh",
               github="https://github.com/forkmesh",
               youtube="https://www.youtube.com/@forkmesh",
               reddit="https://www.reddit.com/r/Forkmesh/")
    runtime.outbound["https://discord.com/api/v10/invites/forkmesh?with_counts=true"] = json.dumps({
        "guild": {"name": "ForkMesh"}, "approximate_member_count": 120,
        "approximate_presence_count": 30})
    discord = _refresh(runtime, "discord")["data"]["refresh"]
    assert (discord["metrics"]["followers"], discord["metrics"]["reach"]) == (120, 30)

    runtime.outbound["https://api.github.com/users/forkmesh"] = json.dumps({
        "login": "forkmesh", "followers": 7, "html_url": "https://github.com/forkmesh"})
    runtime.outbound["https://api.github.com/users/forkmesh/repos?per_page=100&sort=pushed"] = json.dumps([
        {"full_name": "forkmesh/forkmesh", "stargazers_count": 2, "forks_count": 4},
        {"full_name": "forkmesh/site", "stargazers_count": 1, "forks_count": 0},
        {"full_name": "forkmesh/linux", "stargazers_count": 900, "forks_count": 90, "fork": True}])
    runtime.outbound["https://api.github.com/repos/forkmesh/forkmesh/releases?per_page=20"] = json.dumps([
        {"name": "v1", "html_url": "https://github.com/forkmesh/forkmesh/releases/v1",
         "published_at": "2026-09-01T00:00:00Z", "body": "notes"}])
    runtime.outbound["https://api.github.com/repos/forkmesh/site/releases?per_page=20"] = "[]"
    github = _refresh(runtime, "github")["data"]["refresh"]
    assert (github["imported"], github["metrics"]["followers"], github["metrics"]["stars"],
            github["metrics"]["forks"], github["metrics"]["engagements"]) == (1, 7, 3, 4, 7)
    assert all(call[2]["user-agent"].startswith("ForkMesh-Marketing")
               for call in runtime.outbound_calls if "api.github.com" in call[0])

    channel_id = "UCYPmbLXPNi4X6aDKxodcPfg"
    runtime.outbound["https://www.youtube.com/@forkmesh"] = (
        '<link rel="alternate" type="application/rss+xml" href="https://www.youtube.com/'
        'feeds/videos.xml?channel_id=' + channel_id + '"> 1.2K subscribers')
    runtime.outbound["https://www.youtube.com/feeds/videos.xml?channel_id=" + channel_id] = """
      <feed xmlns="http://www.w3.org/2005/Atom"><title>Forkmesh</title>
      <entry><id>yt:video:a</id><title>Clip</title>
        <link rel="alternate" href="https://www.youtube.com/shorts/a"/>
        <published>2026-09-14T03:11:02+00:00</published>
        <media:group><media:community><media:starRating count="3"/>
        <media:statistics views="240"/></media:community></media:group></entry></feed>"""
    youtube = _refresh(runtime, "youtube")["data"]["refresh"]
    assert (youtube["imported"], youtube["metrics"]["views"],
            youtube["metrics"]["followers"], youtube["metrics"]["engagements"]) == (
                1, 240, 1200, 3)

    runtime.outbound["https://www.reddit.com/r/Forkmesh/.rss"] = """
      <feed xmlns="http://www.w3.org/2005/Atom"><title>r/Forkmesh</title>
      <entry><id>t3_a</id><title>Hello</title>
        <link href="https://www.reddit.com/r/Forkmesh/comments/a/hello/"/>
        <published>2026-09-10T00:00:00+00:00</published></entry></feed>"""
    reddit = _refresh(runtime, "reddit")
    assert reddit["status"] == 200
    assert reddit["data"]["refresh"]["imported"] == 1
    assert "metrics" not in reddit["data"]["refresh"]
    channels = {item["channel"] for item in reddit["data"]["content"]}
    assert channels == {"github", "youtube", "reddit"}


def test_x_refresh_needs_a_write_only_token():
    runtime = Runtime()
    _configure(runtime, x="https://x.com/forkmesh")
    missing = _refresh(runtime, "x")
    assert (missing["status"], missing["data"]["error"]) == (502, "x_access_token_required")
    channels = call(runtime.use("GET"), "/api/marketing/acme")["data"]["settings"]["channels"]
    channels = [dict(item, accessToken="secret-token") if item["id"] == "x" else item
                for item in channels]
    saved = call(runtime.use("PUT", body={"channels": channels}),
                 "/api/marketing/acme/settings")
    x = next(item for item in saved["data"]["settings"]["channels"] if item["id"] == "x")
    assert "accessToken" not in x and x["hasAccessToken"] is True
    assert "secret-token" not in json.dumps(call(runtime.use("GET"), "/api/marketing/acme"))
    # A later save from a client that never saw the token keeps it.
    resaved = call(runtime.use("PUT", body={"channels": saved["data"]["settings"]["channels"]}),
                   "/api/marketing/acme/settings")
    assert next(item for item in resaved["data"]["settings"]["channels"]
                if item["id"] == "x")["hasAccessToken"] is True
    runtime.outbound["https://api.x.com/2/users/by/username/forkmesh"
                     "?user.fields=public_metrics,description,name"] = json.dumps({
        "data": {"id": "9", "name": "ForkMesh", "public_metrics": {"followers_count": 50}}})
    runtime.outbound["https://api.x.com/2/users/9/tweets?max_results=50"
                     "&exclude=replies,retweets&tweet.fields=created_at,public_metrics"] = json.dumps({
        "data": [{"id": "77", "text": "Shipped", "created_at": "2026-09-14T10:00:00.000Z",
                  "public_metrics": {"impression_count": 400, "like_count": 9}}]})
    refreshed = _refresh(runtime, "x")["data"]["refresh"]
    assert (refreshed["imported"], refreshed["metrics"]["followers"],
            refreshed["metrics"]["impressions"]) == (1, 50, 400)
    assert runtime.outbound_calls[-1][2]["authorization"] == "Bearer secret-token"
    exported = asyncio.run(marketing.export_org_data(runtime, "org:acme", "acme"))
    assert "secret-token" not in json.dumps(exported)


def test_refresh_does_not_overwrite_channel_links_saved_while_it_reads():
    runtime = Runtime()
    source = "https://forkmesh.com/blog/rss.xml"
    _configure(runtime, blog=source)

    def save_github_mid_fetch():
        # Another member's save lands while the feed is still being read.
        row = runtime.db.execute(
            "SELECT data FROM marketing_settings WHERE org_bi='org:acme'").fetchone()
        stored = json.loads(row["data"].removeprefix("sealed:"))
        stored["channels"].append({"id": "github", "enabled": True, "displayName": "GitHub",
                                   "profileUrl": "https://github.com/forkmesh"})
        runtime.db.execute(
            "UPDATE marketing_settings SET data=? WHERE org_bi='org:acme'",
            ("sealed:" + json.dumps(stored, sort_keys=True),))
        return """<rss><channel><title>Blog</title>
          <item><title>Post</title><guid>p</guid></item></channel></rss>"""

    runtime.outbound[source] = save_github_mid_fetch
    refreshed = call(runtime.use("POST"), "/api/marketing/acme/refresh")
    assert refreshed["status"] == 200
    urls = {item["id"]: item["profileUrl"] for item in refreshed["data"]["settings"]["channels"]}
    assert urls["github"] == "https://github.com/forkmesh"


def test_catalog_has_100_unique_channels_and_first_class_choices():
    assert len(marketing.CHANNEL_CATALOG) == 100
    assert len(set(marketing.CHANNELS)) == 100
    assert marketing.CHANNEL_CATALOG[0][:2] == ("facebook", "Facebook")
    assert {"email", "sms", "google-ads", "apple-business-connect"} <= set(
        marketing.CHANNELS)

    runtime = Runtime()
    overview = call(runtime.use("GET"), "/api/marketing/acme")["data"]
    assert len(overview["channelCatalog"]) == 100
    assert overview["channelCatalog"][0] == {
        "id": "facebook", "name": "Facebook",
        "category": "Social networks", "rank": 1, "hosts": ["facebook.com", "fb.com"],
    }
    assert overview["channelCatalogMeta"] == marketing.CHANNEL_CATALOG_META
    assert all("market" not in item for item in overview["channelCatalog"])
    assert [item["channel"] for item in overview["summary"]["channels"]] == list(
        marketing.STARTER_CHANNELS)

    channels = overview["settings"]["channels"] + [{
        "id": "linkedin", "enabled": True,
        "displayName": "LinkedIn", "profileUrl": "",
    }]
    saved = call(runtime.use("PUT", body={"channels": channels}),
                 "/api/marketing/acme/settings")
    assert saved["status"] == 200
    assert saved["data"]["settings"]["channels"][-1]["id"] == "linkedin"

    content = call(runtime.use("POST", body={
        "title": "A company update", "channel": "linkedin",
    }), "/api/marketing/acme/content")
    metric = call(runtime.use("POST", body={
        "channel": "linkedin", "reach": 42,
    }), "/api/marketing/acme/metrics")
    assert content["data"]["content"]["channel"] == "linkedin"
    assert metric["data"]["metric"]["channel"] == "linkedin"
    refreshed = call(runtime.use("GET"), "/api/marketing/acme")["data"]
    assert refreshed["summary"]["channels"][-1]["channel"] == "linkedin"
    assert refreshed["summary"]["channels"][-1]["reach"] == 42


def test_unknown_channel_settings_are_ignored_instead_of_becoming_blog():
    runtime = Runtime()
    saved = call(runtime.use("PUT", body={"channels": [{
        "id": "not-a-catalog-channel", "enabled": True,
        "displayName": "Nope", "profileUrl": "",
    }]}), "/api/marketing/acme/settings")
    assert [item["id"] for item in saved["data"]["settings"]["channels"]] == list(
        marketing.STARTER_CHANNELS)


def test_new_channels_remain_compatible_with_legacy_five_channel_tables():
    runtime = Runtime(legacy_channels=True)
    content = call(runtime.use("POST", body={
        "title": "A company update", "channel": "linkedin",
    }), "/api/marketing/acme/content")
    metric = call(runtime.use("POST", body={
        "channel": "linkedin", "reach": 9,
    }), "/api/marketing/acme/metrics")
    assert content["data"]["content"]["channel"] == "linkedin"
    assert metric["data"]["metric"]["channel"] == "linkedin"
    assert runtime.db.execute(
        "SELECT channel FROM marketing_content").fetchone()[0] == "blog"
    assert runtime.db.execute(
        "SELECT channel FROM marketing_metrics").fetchone()[0] == "blog"
    updated = call(runtime.use("PATCH", body={"status": "scheduled"}),
                   "/api/marketing/acme/content/" +
                   content["data"]["content"]["id"])
    assert (updated["data"]["content"]["channel"],
            updated["data"]["content"]["status"]) == ("linkedin", "scheduled")


def test_blog_refresh_discovers_rss_imports_posts_and_deduplicates():
    runtime = Runtime()
    source = "https://forkmesh.com/blog"
    feed_url = "https://forkmesh.com/blog/rss.xml"
    runtime.outbound[source] = (
        '<html><head><link rel="alternate" type="application/rss+xml" '
        'href="/blog/rss.xml"></head></html>')
    runtime.outbound[feed_url] = """<?xml version="1.0"?>
      <rss version="2.0"><channel>
        <title>ForkMesh Blog</title>
        <link>https://forkmesh.com/blog</link>
        <description>Technical product updates.</description>
        <item><title>Newest post</title><link>https://forkmesh.com/blog/new</link>
          <guid>post:new</guid><pubDate>Sun, 30 Aug 2026 12:00:00 GMT</pubDate>
          <description><![CDATA[<p>The useful summary.</p>]]></description>
          <category>Product</category></item>
        <item><title>Older post</title><link>https://forkmesh.com/blog/old</link>
          <guid>post:old</guid><pubDate>Sat, 29 Aug 2026 12:00:00 GMT</pubDate>
          <description>Another update.</description></item>
      </channel></rss>"""
    saved = call(runtime.use("PUT", body={"channels": [{
        "id": "blog", "enabled": True, "displayName": "ForkMesh Blog",
        "profileUrl": source,
    }]}), "/api/marketing/acme/settings")
    assert saved["status"] == 200

    refreshed = call(runtime.use("POST"), "/api/marketing/acme/refresh")
    assert refreshed["status"] == 200
    assert refreshed["data"]["refresh"]["imported"] == 2
    assert refreshed["data"]["summary"]["published"] == 2
    assert [item["title"] for item in refreshed["data"]["content"]] == [
        "Newest post", "Older post"]
    assert refreshed["data"]["content"][0]["body"] == "The useful summary."
    blog = next(item for item in refreshed["data"]["settings"]["channels"]
                if item["id"] == "blog")
    assert blog["feed"]["title"] == "ForkMesh Blog"
    assert blog["feed"]["description"] == "Technical product updates."
    assert blog["feed"]["url"] == feed_url
    assert blog["feed"]["itemCount"] == 2
    assert runtime.outbound_calls[0][0] == source
    assert runtime.outbound_calls[1][0] == feed_url

    assert refreshed["data"]["refresh"]["metrics"]["posts"] == 2

    repeated = call(runtime.use("POST"), "/api/marketing/acme/refresh")
    assert repeated["data"]["refresh"] == {
        "channel": "blog", "imported": 0, "items": 2,
        "feed": repeated["data"]["refresh"]["feed"],
        "metrics": repeated["data"]["refresh"]["metrics"],
    }
    assert repeated["data"]["refresh"]["metrics"]["posts"] == 2
    assert len(repeated["data"]["content"]) == 2
    health = next(item for item in repeated["data"]["summary"]["channels"]
                  if item["channel"] == "blog")["health"]
    assert {check["id"]: check["ok"] for check in health["checks"]}["fresh"] is True
    assert [metric["source"] for metric in repeated["data"]["metrics"]
            if metric["channel"] == "blog"][:1] == ["refresh"]


def test_ads_manager_plans_an_ad_on_any_catalog_channel():
    runtime = Runtime()
    overview = call(runtime.use("GET"), "/api/marketing/acme")["data"]
    assert (overview["ads"], overview["adsSummary"]["ads"]) == ([], 0)
    platforms = {item["channel"]: item for item in overview["adPlatforms"]}
    assert platforms["youtube"] == {
        "channel": "youtube", "name": "Google Ads", "url": "https://ads.google.com/"}
    assert platforms["reddit"]["url"] == "https://ads.reddit.com/"
    assert set(platforms) <= set(marketing.CHANNELS)
    assert all(item["url"].startswith("https://") for item in platforms.values())

    # Every catalog channel takes an ad, with or without a self-serve console.
    for channel in marketing.CHANNELS:
        created = call(runtime.use("POST", body={"name": "Ad on " + channel,
                                                 "channel": channel}),
                       "/api/marketing/acme/ads")
        assert created["status"] == 201, channel
        ad = created["data"]["ad"]
        assert ad["channel"] == channel
        assert ad["platformUrl"] == platforms.get(channel, {}).get("url", "")
    listed = call(runtime.use("GET"), "/api/marketing/acme/ads")["data"]
    assert len(listed["ads"]) == len(marketing.CHANNELS) == listed["adsSummary"]["ads"]

    for body, error in (
            ({"name": "No channel"}, "invalid_ad_channel"),
            ({"name": "Bad", "channel": "unlisted-channel"}, "invalid_ad_channel"),
            ({"channel": "reddit"}, "ad_name_required"),
            ({"name": "Bad", "channel": "reddit",
              "destinationUrl": "http://example.com"}, "invalid_ad_url"),
            ({"name": "Bad", "channel": "reddit", "currency": "dollars"},
             "invalid_ad_currency"),
            ({"name": "Bad", "channel": "reddit", "startsAt": 20, "endsAt": 10},
             "invalid_ad_dates")):
        denied = call(runtime.use("POST", body=body), "/api/marketing/acme/ads")
        assert (denied["status"], denied["data"]["error"]) == (400, error)


def test_ad_results_are_encrypted_rated_and_paced_against_the_flight():
    runtime = Runtime()
    start = runtime.clock - 5 * marketing.DAY_MS
    created = call(runtime.use("POST", body={
        "name": "Launch video", "channel": "youtube", "status": "active",
        "objective": "video-views", "campaign": "Launch", "owner": "alice",
        "audience": "Private audience note", "headline": "Ship faster",
        "destinationUrl": "forkmesh.com/launch", "currency": "eur",
        "budgetType": "lifetime", "budgetCents": 100_000,
        "startsAt": start, "endsAt": start + 10 * marketing.DAY_MS,
        "tags": ["Video", "Launch"],
    }), "/api/marketing/acme/ads")
    ad = created["data"]["ad"]
    assert (ad["platform"], ad["currency"], ad["objective"], ad["tags"]) == (
        "Google Ads", "EUR", "video-views", ["video", "launch"])
    assert ad["destinationUrl"] == "https://forkmesh.com/launch"
    assert (ad["resultsAt"], ad["pacing"]["state"]) == (0, "under")
    stored = runtime.db.execute("SELECT data FROM marketing_ads").fetchone()[0]
    assert stored.startswith("sealed:") and "Private audience note" in stored

    # Any member logs results; only the sent fields change.
    logged = call(runtime.use("PATCH", "charlie", {
        "spendCents": 50_000, "impressions": 200_000, "clicks": 4_000,
        "conversions": 100, "revenueCents": 150_000,
    }), "/api/marketing/acme/ads/" + ad["id"])
    assert logged["status"] == 200
    ad = logged["data"]["ad"]
    assert (ad["name"], ad["headline"], ad["updatedBy"]) == (
        "Launch video", "Ship faster", "charlie")
    assert (ad["ctr"], ad["cpcCents"], ad["cpmCents"], ad["cpaCents"], ad["roas"]) == (
        2.0, 12, 250, 500, 3.0)
    assert ad["resultsAt"] > 0
    assert ad["pacing"]["totalBudgetCents"] == 100_000
    assert 49_000 < ad["pacing"]["expectedCents"] < 51_000
    assert ad["pacing"]["state"] == "on-track"

    over = call(runtime.use("PATCH", body={"spendCents": 80_000}),
                "/api/marketing/acme/ads/" + ad["id"])["data"]["ad"]
    assert over["pacing"]["state"] == "over"
    spent = call(runtime.use("PATCH", body={"spendCents": 100_001}),
                 "/api/marketing/acme/ads/" + ad["id"])["data"]["ad"]
    assert spent["pacing"]["state"] == "overspent"
    paused = call(runtime.use("PATCH", body={"status": "paused", "spendCents": 10}),
                  "/api/marketing/acme/ads/" + ad["id"])["data"]["ad"]
    assert (paused["status"], paused["pacing"]["state"]) == ("paused", "")

    daily = call(runtime.use("POST", body={
        "name": "Always on", "channel": "reddit", "status": "active",
        "budgetType": "daily", "budgetCents": 1_000, "startsAt": start,
        "endsAt": start + 10 * marketing.DAY_MS, "spendCents": 5_000,
    }), "/api/marketing/acme/ads")["data"]["ad"]
    assert daily["pacing"]["totalBudgetCents"] == 10_000
    assert daily["pacing"]["state"] == "on-track"

    assert call(runtime.use("GET", "bob"), "/api/marketing/acme/ads")["status"] == 404
    assert call(runtime.use("GET"), "/api/marketing/acme/ads/" + "f" * 32)["status"] == 404
    deleted = call(runtime.use("DELETE"), "/api/marketing/acme/ads/" + daily["id"])
    assert deleted["data"] == {"ok": True, "deleted": True}
    assert len(call(runtime.use("GET"), "/api/marketing/acme")["data"]["ads"]) == 1


def test_ads_summary_never_adds_different_currencies_together():
    runtime = Runtime()
    for body in (
        {"name": "A", "channel": "reddit", "status": "active", "budgetCents": 10_000,
         "spendCents": 4_000, "impressions": 10_000, "clicks": 100, "conversions": 4},
        {"name": "B", "channel": "youtube", "status": "paused", "budgetCents": 20_000,
         "spendCents": 6_000, "impressions": 30_000, "clicks": 300, "conversions": 6,
         "revenueCents": 30_000},
        {"name": "C", "channel": "reddit", "status": "active", "currency": "JPY",
         "budgetCents": 500_000, "spendCents": 100_000},
        {"name": "Old", "channel": "x", "status": "archived", "spendCents": 999_999},
    ):
        assert call(runtime.use("POST", body=body),
                    "/api/marketing/acme/ads")["status"] == 201
    summary = call(runtime.use("GET"), "/api/marketing/acme")["data"]["adsSummary"]
    assert (summary["ads"], summary["active"]) == (3, 2)
    usd, jpy = summary["currencies"]
    assert (usd["currency"], usd["ads"], usd["active"]) == ("USD", 2, 1)
    assert (usd["budgetCents"], usd["spendCents"], usd["revenueCents"]) == (
        30_000, 10_000, 30_000)
    assert (usd["ctr"], usd["cpcCents"], usd["cpaCents"], usd["roas"]) == (
        1.0, 25, 1000, 3.0)
    assert [item["channel"] for item in usd["channels"]] == ["youtube", "reddit"]
    assert usd["channels"][1]["cpaCents"] == 1000
    assert (jpy["currency"], jpy["spendCents"]) == ("JPY", 100_000)


def test_experiment_backlog_scores_ranks_and_flags_reviews():
    runtime = Runtime()

    def create(**body):
        result = call(runtime.use("POST", body=body), "/api/marketing/acme/experiments")
        assert result["status"] == 201, result
        return result["data"]["experiment"]

    low = create(title="Comparison page", stage="acquisition", impact=5, confidence=5, ease=4)
    high = create(title="Empty dashboard copy", stage="activation", impact=8,
                  confidence=6, ease=9, hypothesis="Private hypothesis")
    unscored = create(title="Launch week", impact=9)
    clamped = create(title="Clamp", impact=50, confidence=-3, ease=7)
    assert (high["score"], low["score"], unscored["score"]) == (432, 100, 0)
    assert (clamped["impact"], clamped["confidence"], clamped["score"]) == (10, 1, 70)
    assert (high["status"], high["decision"]) == ("idea", "")
    stored = runtime.db.execute(
        "SELECT data FROM marketing_experiments WHERE experiment_id=?", (high["id"],)).fetchone()[0]
    assert stored.startswith("sealed:") and "Private hypothesis" in stored
    summary = call(runtime.use("GET"), "/api/marketing/acme/experiments")["data"]["experimentsSummary"]
    assert summary["top"] == [high["id"], low["id"], clamped["id"]]
    assert (summary["unscored"], summary["byStage"]["activation"]) == (1, 1)

    past = runtime.clock - 10
    patched = call(runtime.use("PATCH", "charlie", {
        "status": "running", "startsAt": past - 100, "endsAt": past, "reviewAt": past,
    }), f"/api/marketing/acme/experiments/{high['id']}")["data"]["experiment"]
    assert (patched["status"], patched["updatedBy"], patched["reviewDue"], patched["ended"]) == (
        "running", "charlie", True, True)
    assert patched["hypothesis"] == "Private hypothesis"
    for item in (low, clamped, unscored):
        call(runtime.use("PATCH", body={"status": "running"}),
             f"/api/marketing/acme/experiments/{item['id']}")
    summary = call(runtime.use("GET"), "/api/marketing/acme")["data"]["experimentsSummary"]
    assert (summary["running"], summary["overLimit"], summary["top"]) == (4, True, [])
    assert summary["reviewsDue"] == [high["id"]]

    for body, error in (({"impact": 3}, "experiment_title_required"),
                        ({"title": "X", "startsAt": 20, "endsAt": 10}, "invalid_experiment_dates")):
        denied = call(runtime.use("POST", body=body), "/api/marketing/acme/experiments")
        assert (denied["status"], denied["data"]["error"]) == (400, error)
    assert call(runtime.use("GET", "bob"), "/api/marketing/acme/experiments")["status"] == 404
    deleted = call(runtime.use("DELETE"), f"/api/marketing/acme/experiments/{high['id']}")
    assert deleted["data"] == {"ok": True, "deleted": True}
    assert call(runtime.use("GET"), f"/api/marketing/acme/experiments/{high['id']}")["status"] == 404


def test_funnel_weeks_keep_unmeasured_numbers_and_name_the_bottleneck():
    runtime = Runtime()
    monday = marketing._week_start(runtime.clock)
    previous = monday - 7 * marketing.DAY_MS
    first = call(runtime.use("POST", body={
        "weekStart": previous + 3 * marketing.DAY_MS, "visits": 1000, "signups": 40,
        "activated": 4, "northStar": 4, "source": "analytics export",
    }), "/api/marketing/acme/funnel")
    assert first["status"] == 201
    week = first["data"]["funnel"][0]
    assert week["weekStart"] == previous
    assert (week["retained"], week["referrals"], week["revenueCents"]) == (None, None, None)
    assert (week["rates"]["activationRate"], week["rates"]["retentionRate"]) == (10.0, None)
    assert first["data"]["funnelReport"]["bottleneck"] == ""
    assert "Set a target" in first["data"]["funnelReport"]["bottleneckReason"]

    call(runtime.use("POST", body={
        "weekStart": monday, "visits": 1200, "signups": 30, "activated": 6, "retained": 3,
        "retainedBase": 12, "northStar": 10, "referrals": "", "source": "analytics export",
    }), "/api/marketing/acme/funnel")
    call(runtime.use("PUT", body={"targets": {
        "signupRate": 5, "activationRate": 25, "retentionRate": 30, "referralRate": 0}}),
        "/api/marketing/acme/settings")
    report = call(runtime.use("GET"), "/api/marketing/acme/funnel")["data"]["funnelReport"]
    rates = {item["key"]: item for item in report["rates"]}
    assert report["bottleneck"] == "signupRate"
    assert report["bottleneckReason"] == (
        "Signup rate is 2.5% against a 5% target, the furthest below target.")
    assert (rates["retentionRate"]["value"], rates["retentionRate"]["small"]) == (25.0, True)
    assert (rates["activationRate"]["change"], rates["referralRate"]["value"]) == (10.0, None)
    north = next(item for item in report["counts"] if item["key"] == "northStar")
    assert (north["value"], north["previous"], north["change"]) == (10, 4, 6)

    again = call(runtime.use("POST", "charlie", {
        "weekStart": monday + marketing.DAY_MS, "referrals": 2}), "/api/marketing/acme/funnel")
    assert again["status"] == 200
    weeks = again["data"]["funnel"]
    assert (len(weeks), weeks[0]["referrals"], weeks[0]["visits"], weeks[0]["recordedBy"]) == (
        2, 2, 1200, "charlie")
    stored = runtime.db.execute(
        "SELECT data FROM marketing_funnel WHERE week_start=?", (monday,)).fetchone()[0]
    assert stored.startswith("sealed:") and "analytics export" in stored
    denied = call(runtime.use("POST", body={"visits": 5}), "/api/marketing/acme/funnel")
    assert (denied["status"], denied["data"]["error"]) == (400, "funnel_week_required")
    deleted = call(runtime.use("DELETE"), f"/api/marketing/acme/funnel/{weeks[1]['id']}")
    assert (deleted["data"]["deleted"], len(deleted["data"]["funnel"])) == (True, 1)


def test_link_builder_tags_links_and_flags_untracked_values():
    runtime = Runtime()
    saved = call(runtime.use("PUT", body={"attribution": {
        "sources": "blog, Mastodon, bad!", "mediums": ["social", "referral"],
        "campaigns": "changelog", "untrackedPaths": "/blog/, docs",
        "domains": "forkmesh.com", "landingUrl": "forkmesh.com/signup"}}),
        "/api/marketing/acme/settings")
    assert saved["data"]["settings"]["attribution"] == {
        "sources": ["blog", "mastodon"], "mediums": ["social", "referral"],
        "campaigns": ["changelog"], "domains": ["forkmesh.com"],
        "untrackedPaths": ["/blog", "/docs"], "landingUrl": "https://forkmesh.com/signup"}
    assert call(runtime.use("PUT", "charlie", {"attribution": {}}),
                "/api/marketing/acme/settings")["status"] == 403

    built = call(runtime.use("POST", body={
        "source": "Mastodon", "medium": "social", "campaign": "changelog",
        "content": "The Work Keeps"}), "/api/marketing/acme/links")["data"]
    assert built["url"] == ("https://forkmesh.com/signup?utm_source=mastodon&utm_medium=social"
                            "&utm_campaign=changelog&utm_content=the-work-keeps")
    assert built["checks"] == []
    off = call(runtime.use("POST", body={
        "url": "https://forkmesh.com/blog/post?ref=x&utm_source=old", "source": "reddit",
        "medium": "social", "campaign": "changelog"}), "/api/marketing/acme/links")["data"]
    assert off["url"] == ("https://forkmesh.com/blog/post?ref=x&utm_source=reddit"
                          "&utm_medium=social&utm_campaign=changelog")
    assert [check["code"] for check in off["checks"]] == ["unlisted_utm_source", "untracked_path"]
    elsewhere = call(runtime.use("POST", body={"url": "https://apps.apple.com/app/x"}),
                     "/api/marketing/acme/links")["data"]
    assert elsewhere["checks"] == []
    for body, error in (({"url": "http://forkmesh.com"}, "invalid_link_url"),
                        ({"url": "https://forkmesh.com", "source": "a/b"}, "invalid_utm_value")):
        denied = call(runtime.use("POST", body=body), "/api/marketing/acme/links")
        assert (denied["status"], denied["data"]["error"]) == (400, error)

    ad = call(runtime.use("POST", body={
        "name": "Docs promo", "channel": "reddit",
        "destinationUrl": "https://app.forkmesh.com/docs/start"}),
        "/api/marketing/acme/ads")["data"]["ad"]
    assert [check["code"] for check in ad["linkChecks"]] == [
        "missing_utm_source", "missing_utm_medium", "missing_utm_campaign", "untracked_path"]


def test_weekly_report_is_markdown_with_scorecard_backlog_ads_and_content():
    runtime = Runtime()
    call(runtime.use("PUT", body={"targets": {"activationRate": 30}}),
         "/api/marketing/acme/settings")
    call(runtime.use("POST", body={
        "weekStart": runtime.clock, "visits": 500, "signups": 25, "activated": 5,
        "northStar": 5, "source": "weekly export"}), "/api/marketing/acme/funnel")
    call(runtime.use("POST", body={
        "title": "Empty dashboard copy", "stage": "activation", "impact": 8,
        "confidence": 6, "ease": 9}), "/api/marketing/acme/experiments")
    call(runtime.use("POST", body={
        "name": "Promoted post", "channel": "reddit", "status": "active",
        "budgetCents": 1000, "spendCents": 5000}), "/api/marketing/acme/ads")
    call(runtime.use("POST", body={"title": "Post", "status": "published",
                                   "publishedAt": runtime.clock}), "/api/marketing/acme/content")
    report = call(runtime.use("GET"), "/api/marketing/acme/report")
    assert report["status"] == 200
    markdown = report["data"]["markdown"]
    assert markdown.startswith("# Marketing report: acme\n")
    for line in (
            "| Signups | 25 | no data | n/a |",
            "| Revenue (USD) | no data | no data | n/a |",
            "| Activation rate | 20% | no data | n/a |",
            "**Bottleneck:** Activation rate is 20% against a 30% target, the furthest below target.",
            "1. Empty dashboard copy (activation): 8 × 6 × 9 = 432",
            "Running: 0 of a limit of 3.",
            "- Promoted post (reddit): over budget",
            "Published in the last 7 days: 1. Scheduled for the next 7 days: 0."):
        assert line in markdown, line
    assert call(runtime.use("POST"), "/api/marketing/acme/report")["status"] == 405


def test_growth_page_is_wired_on_the_web_surface():
    web = (PACKAGE / "content" / "web" / "page.js").read_text()
    view = (PACKAGE / "content" / "web" / "view.html").read_text()
    assert 'data-marketing-page="growth"' in view and "data-marketing-growth-page" in view
    for hook in ("data-marketing-experiment-form", "data-marketing-funnel-form",
                 "data-marketing-link-form", "data-marketing-report-dialog",
                 "data-marketing-attribution-field", "data-marketing-target",
                 "empty is not zero"):
        assert hook in view, hook
    for hook in ('orgPath("/links")', 'orgPath("/report")', 'orgPath("/funnel")',
                 'orgPath(`/experiments${id ? `/${id}` : ""}`)', "marketingGrowthOnly",
                 "funnelReport", "experimentsSummary"):
        assert hook in web, hook
    for status in marketing.EXPERIMENT_STATUSES:
        assert f'["{status}",' in web
    for stage in marketing.EXPERIMENT_STAGES:
        assert f'["{stage}",' in web
    manifest = json.loads((PACKAGE / "manifest.json").read_text())
    catalog = json.loads((REPO / "marketplace" / "catalog.json").read_text())
    listed = next(item for item in catalog["packages"] if item["id"] == "marketing")
    assert listed["version"] == manifest["version"]


def test_uninstalled_api_is_gated_without_leaving_marketing_tables():
    runtime = Runtime(installed=False)
    denied = call(runtime.use("GET"), "/api/marketing/acme")
    assert (denied["status"], denied["data"]["canInstall"]) == (409, True)
    tables = [row[0] for row in runtime.db.execute(
        "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'marketing_%'")]
    assert tables == []


def test_manifest_web_native_and_reversible_data_hooks_are_complete():
    manifest = json.loads((PACKAGE / "manifest.json").read_text())
    assert manifest["worker"]["module"] == "marketing"
    assert manifest["worker"]["api"] == "/api/marketing"
    assert "https://forkmesh.com" in manifest["worker"]["outboundOrigins"]
    assert manifest["dataLifecycle"] == {"scope": "organization"}
    assert manifest["entrypoints"]["desktop"] == "builtin:marketing"
    web = (PACKAGE / "content" / "web" / "page.js").read_text()
    view = (PACKAGE / "content" / "web" / "view.html").read_text()
    assert "data-marketing-catalog-page" in view
    assert "Add channels" in view
    assert "not a compliance certification" in view
    assert "data-marketing-channel-add" in web
    assert "channelCatalog" in web
    native = (PACKAGE / "content" / "desktop" / "MarketingFeature.cpp").read_text()
    assert 'QStringLiteral("builtin:marketing")' in native
    assert 'QStringLiteral("forkmesh-marketing-request-v1")' in native
    assert 'QStringLiteral("/refresh")' in native
    assert 'QStringLiteral("Save channels")' in native
    assert 'm_channelsTable->setCellWidget(row, 3, profile)' in native
    assert 'QDesktopServices::openUrl(url)' in native
    assert 'Updating dashboard statistics' in native
    assert 'void MarketingFeature::editChannels()' not in native
    assert 'QStringLiteral("Add channels")' in native
    assert "m_catalogTable->setCellWidget(row, 3, button)" in native
    assert 'data-marketing-refresh' in web
    for label in ("30 days", "7 days", "24 hours", "30 minutes"):
        assert f'QStringLiteral("{label}")' in native
    assert "bool MarketingFeature::refreshChannel(const QString &id)" in native
    # The Ads manager is one feature on both surfaces, driven by the same API.
    assert "data-marketing-ads-page" in view
    assert 'data-marketing-page="ads"' in view
    assert "never spends money or connects to an ad account" in view
    # .mk-page sets display after the utility sheet; it must still yield to .hidden.
    assert '[data-view="marketing"] .mk-page.hidden { display: none; }' in view
    for hook in ("data-marketing-ad-edit", "data-marketing-ad-results",
                 "data-marketing-ads-channel", 'orgPath(`/ads${id ? `/${id}` : ""}`)'):
        assert hook in web
    assert "adsSummary.currencies" in web
    assert 'm_adsPanel = new DashboardPanel(QStringLiteral("Ads manager"))' in native
    assert 'organizationPath(QStringLiteral("/ads")' in native
    for label in ("New ad", "Log results", "Open console"):
        assert f'QStringLiteral("{label}")' in native
    for status in marketing.AD_STATUSES:
        assert f'["{status}",' in web and f'QStringLiteral("{status}")' in native
    for objective in marketing.AD_OBJECTIVES:
        assert f'["{objective}",' in web and f'QStringLiteral("{objective}")' in native
    assert 'QJsonObject{{QStringLiteral("channel"), id}}' in native
    assert "m_channelsTable->setCellWidget(row, 4, velocity)" in native
    assert "QLineEdit::editingFinished" in native
    for origin in ("https://api.github.com", "https://discord.com",
                   "https://mastodon.social", "https://www.youtube.com",
                   "https://www.reddit.com", "https://api.x.com"):
        assert origin in manifest["worker"]["outboundOrigins"]
    created = {statement.split("CREATE TABLE IF NOT EXISTS ", 1)[1].split()[0]
               for statement in marketing.DATA_SCHEMA_UP
               if "CREATE TABLE IF NOT EXISTS" in statement}
    dropped = {statement.split("DROP TABLE IF EXISTS ", 1)[1]
               for statement in marketing.DATA_SCHEMA_DOWN}
    assert created == dropped


def test_export_and_purge_cover_all_owned_rows():
    runtime = Runtime()
    call(runtime.use("POST", body={"title": "A"}), "/api/marketing/acme/content")
    call(runtime.use("POST", body={"channel": "blog", "reach": 10}),
         "/api/marketing/acme/metrics")
    call(runtime.use("POST", body={"name": "Ad", "channel": "linkedin"}),
         "/api/marketing/acme/ads")
    call(runtime.use("POST", body={"title": "Idea"}), "/api/marketing/acme/experiments")
    call(runtime.use("POST", body={"weekStart": runtime.clock, "signups": 3}),
         "/api/marketing/acme/funnel")
    call(runtime.use("POST", body={"url": THREAD_URL}), "/api/marketing/acme/reddit")
    call(runtime.use("POST", "charlie", {"subreddit": "git", "rules": [{"title": "Be kind"}]}),
         "/api/marketing/acme/reddit-rules")
    exported = asyncio.run(marketing.export_org_data(runtime, "org:acme", "acme"))
    assert [item["postId"] for item in exported["redditThreads"]] == ["abc123"]
    assert [item["subreddit"] for item in exported["redditRules"]] == ["git"]
    assert "redditRules" not in exported["settings"]
    assert exported["format"] == "forkmesh-marketing-export-v1"
    assert (len(exported["content"]), len(exported["metrics"])) == (1, 1)
    assert [item["name"] for item in exported["ads"]] == ["Ad"]
    assert [item["title"] for item in exported["experiments"]] == ["Idea"]
    assert [item["signups"] for item in exported["funnel"]] == [3]
    asyncio.run(marketing.purge_org_data(runtime, "org:acme"))
    assert asyncio.run(marketing.org_has_data(runtime, "org:acme")) is False


def test_browser_activity_is_the_readers_own_and_rings_their_devices():
    runtime = Runtime()
    rung = []

    async def notify_account(owner, topic):
        rung.append((owner, topic))

    runtime.notify_account = notify_account
    created = call(runtime.use("POST", "charlie", {
        "kind": "read", "site": "Reddit", "page": "r/selfhosted",
        "url": "https://www.reddit.com/r/selfhosted/",
        "text": "Read r/selfhosted: 25 posts. 2 waiting for the Reddit desk."}),
        "/api/marketing/acme/activity")
    assert created["status"] == 201
    entry = created["data"]["browserActivity"][0]
    assert (entry["kind"], entry["site"], entry["page"], entry["url"]) == (
        "read", "reddit", "r/selfhosted", "https://www.reddit.com/r/selfhosted/")
    assert rung == [("charlie", "package:marketing")]
    stored = runtime.db.execute("SELECT data FROM marketing_browser_activity").fetchone()[0]
    assert stored.startswith("sealed:")
    for body in ({"kind": "browsed", "text": "x"}, {"kind": "read", "text": " "}):
        denied = call(runtime.use("POST", "charlie", body), "/api/marketing/acme/activity")
        assert (denied["status"], denied["data"]["error"]) == (400, "invalid_browser_activity")
    insecure = call(runtime.use("POST", "charlie", {
        "kind": "sent", "text": "Sent", "url": "http://example.com/"}),
        "/api/marketing/acme/activity")["data"]["browserActivity"][0]
    assert insecure["url"] == ""
    overview = call(runtime.use("GET", "charlie"), "/api/marketing/acme")["data"]
    assert [item["text"] for item in overview["browserActivity"]] == [
        "Sent", "Read r/selfhosted: 25 posts. 2 waiting for the Reddit desk."]
    assert call(runtime.use("GET"), "/api/marketing/acme")["data"]["browserActivity"] == []
    for number in range(marketing.BROWSER_ACTIVITY_KEEP + 5):
        call(runtime.use("POST", "charlie", {"kind": "read", "text": f"Read {number}"}),
             "/api/marketing/acme/activity")
    kept = runtime.db.execute("SELECT COUNT(*) FROM marketing_browser_activity").fetchone()[0]
    assert kept == marketing.BROWSER_ACTIVITY_KEEP
    listed = call(runtime.use("GET", "charlie"), "/api/marketing/acme/activity")["data"]
    assert len(listed["browserActivity"]) == marketing.BROWSER_ACTIVITY_SHOWN
    rung.clear()
    call(runtime.use("POST", "charlie", {"url": THREAD_URL}), "/api/marketing/acme/reddit")
    call(runtime.use("GET", "charlie"), "/api/marketing/acme/reddit")
    assert rung == [("charlie", "package:marketing")]
    asyncio.run(marketing.purge_org_data(runtime, "org:acme"))
    assert asyncio.run(marketing.org_has_data(runtime, "org:acme")) is False


def test_desktop_reloads_marketing_when_the_package_rings():
    native = (PACKAGE / "content" / "desktop" / "MarketingFeature.cpp").read_text()
    host = (REPO / "desktop" / "src" / "NativePackageExtension.h").read_text()
    window = (REPO / "desktop" / "src" / "MainWindowRepoSync.cpp").read_text()
    assert "virtual void packageChanged() {}" in host
    assert 'topic.startsWith(QLatin1String("package:"))' in window
    assert "page.widget->packageChanged();" in window
    assert "void MarketingFeature::packageChanged()" in native
    assert 'payload.value(QStringLiteral("browserActivity"))' in native
    assert 'QStringLiteral("<b>From your browser</b><br>")' in native
    assert marketing.CHANGED_TOPIC == "package:marketing"


THREAD_URL = "https://old.reddit.com/r/selfhosted/comments/abc123/mirror_my_repos/?utm=x"
REPLY_URL = "https://www.reddit.com/r/selfhosted/comments/abc123/mirror_my_repos/def456/"


def test_reddit_desk_logs_threads_once_and_members_may_write():
    runtime = Runtime()
    created = call(runtime.use("POST", "charlie", {
        "url": THREAD_URL, "title": "  Mirror   my repos? ", "note": "Asks for a mirror",
        "landingPath": "download"}), "/api/marketing/acme/reddit")
    assert created["status"] == 201
    thread = created["data"]["thread"]
    assert (thread["url"], thread["subreddit"], thread["postId"], thread["title"]) == (
        "https://www.reddit.com/r/selfhosted/comments/abc123/mirror_my_repos/",
        "selfhosted", "abc123", "Mirror my repos?")
    assert (thread["status"], thread["landingPath"], thread["createdBy"]) == (
        "to_review", "/download", "charlie")
    assert (thread["source"], thread["author"], thread["comments"], thread["score"],
            thread["postedAt"]) == ("manual", "", None, None, 0)
    assert thread["taggedLink"].startswith("https://forkmesh.com/download?utm_source=reddit")
    assert "utm_content=selfhosted" in thread["taggedLink"]
    stored = runtime.db.execute("SELECT data FROM marketing_reddit_threads").fetchone()[0]
    assert stored.startswith("sealed:") and "Asks for a mirror" in stored
    again = call(runtime.use("POST", body={
        "url": "https://www.reddit.com/r/selfhosted/comments/ABC123/"}),
        "/api/marketing/acme/reddit")
    assert (again["status"], again["data"]["error"], again["data"]["threadId"]) == (
        409, "reddit_thread_exists", thread["id"])
    for body, error in (
            ({"url": "https://example.com/r/git/comments/abc/x/"}, "invalid_reddit_thread_url"),
            ({"url": "http://www.reddit.com/r/git/comments/abc/x/"}, "invalid_reddit_thread_url"),
            ({"url": "https://www.reddit.com/r/git/"}, "invalid_reddit_thread_url"),
            ({"url": THREAD_URL.replace("abc123", "zzz9"), "landingPath": "/docs/x/y"},
             "invalid_landing_path"),
            ({"url": THREAD_URL.replace("abc123", "zzz9"), "status": "posted"},
             "invalid_reddit_status")):
        denied = call(runtime.use("POST", body=body), "/api/marketing/acme/reddit")
        assert (denied["status"], denied["data"]["error"]) == (400, error), body
    listed = call(runtime.use("GET", "charlie"), "/api/marketing/acme/reddit")["data"]
    assert [item["id"] for item in listed["redditThreads"]] == [thread["id"]]
    assert listed["redditDesk"]["summary"]["open"] == 1
    assert call(runtime.use("GET", "bob"), "/api/marketing/acme/reddit")["status"] == 404
    path = f"/api/marketing/acme/reddit/{thread['id']}"
    assert call(runtime.use("DELETE", "charlie"), path)["data"]["deleted"] is True
    assert call(runtime.use("GET"), path)["status"] == 404


def test_reddit_reply_needs_both_checks_and_a_reply_in_the_thread():
    runtime = Runtime()
    thread = call(runtime.use("POST", body={"url": THREAD_URL}),
                  "/api/marketing/acme/reddit")["data"]["thread"]
    path = f"/api/marketing/acme/reddit/{thread['id']}"
    early = call(runtime.use("PATCH", body={"status": "replied", "rulesChecked": True}), path)
    assert (early["status"], early["data"]["error"], early["data"]["missing"]) == (
        400, "reddit_reply_checks_required", ["disclosed", "replyUrl"])
    elsewhere = call(runtime.use("PATCH", body={
        "replyUrl": REPLY_URL.replace("abc123", "zzz999")}), path)
    assert elsewhere["data"]["error"] == "reddit_reply_not_in_thread"
    replied = call(runtime.use("PATCH", body={
        "status": "replied", "rulesChecked": True, "disclosed": True, "replyUrl": REPLY_URL}),
        path)
    assert replied["status"] == 200
    assert (replied["data"]["thread"]["status"], replied["data"]["thread"]["replyUrl"]) == (
        "replied", REPLY_URL)
    assert replied["data"]["thread"]["decidedAt"] > 0
    report = call(runtime.use("GET"), "/api/marketing/acme/report")["data"]
    assert report["redditSummary"]["repliedThisWeek"] == 1
    assert "## Reddit desk" in report["markdown"]
    assert (f"- r/selfhosted: {thread['url']} ({REPLY_URL})") in report["markdown"]


def test_reddit_threads_keep_what_the_browser_extension_read():
    runtime = Runtime()
    created = call(runtime.use("POST", "charlie", {
        "url": THREAD_URL, "title": "Mirror my repos?", "source": "extension",
        "author": "u/grace_h", "comments": 12, "score": -3, "postedAt": 1_999_000_000_000,
        "rulesChecked": True, "disclosed": True}), "/api/marketing/acme/reddit")
    assert created["status"] == 201
    thread = created["data"]["thread"]
    assert (thread["source"], thread["author"], thread["comments"], thread["score"],
            thread["postedAt"]) == ("extension", "grace_h", 12, -3, 1_999_000_000_000)
    path = f"/api/marketing/acme/reddit/{thread['id']}"
    edited = call(runtime.use("PATCH", body={"note": "Answer about mirrors"}), path)
    kept = edited["data"]["thread"]
    assert (kept["source"], kept["author"], kept["comments"], kept["note"]) == (
        "extension", "grace_h", 12, "Answer about mirrors")
    for body, error in (({"author": "not a name!"}, "invalid_reddit_author"),
                        ({"comments": -1}, "invalid_reddit_comments"),
                        ({"comments": True}, "invalid_reddit_comments"),
                        ({"score": "lots"}, "invalid_reddit_score"),
                        ({"postedAt": -5}, "invalid_reddit_postedat"),
                        ({"source": "scraper"}, "invalid_reddit_source")):
        denied = call(runtime.use("PATCH", body=body), path)
        assert (denied["status"], denied["data"]["error"]) == (400, error), body


def test_monitored_reddit_threads_keep_a_view_history_and_their_comment():
    runtime = Runtime()
    created = call(runtime.use("POST", "charlie", {
        "url": "https://www.reddit.com/r/devops/comments/1wy1g5i/comment/pec00w1/",
        "source": "extension", "monitored": True}), "/api/marketing/acme/reddit")
    assert created["status"] == 201
    thread = created["data"]["thread"]
    assert (thread["url"], thread["postId"], thread["monitored"], thread["replyUrl"]) == (
        "https://www.reddit.com/r/devops/comments/1wy1g5i/", "1wy1g5i", True, "")
    assert thread["commentUrl"] == thread["monitorCommentUrl"] == (
        "https://www.reddit.com/r/devops/comments/1wy1g5i/comment/pec00w1/")
    assert (thread["monitorCommentId"], thread["views"], thread["checkedAt"],
            thread["history"]) == ("pec00w1", None, 0, [])
    path = f"/api/marketing/acme/reddit/{thread['id']}"
    hour = 3600 * 1000
    first = call(runtime.use("PATCH", "charlie", {
        "checkedAt": 1_000 * hour, "views": 120, "score": 8, "comments": 40,
        "commentScore": 1, "commentViews": 15, "title": "Weekly Self Promotion Thread"}), path)
    assert first["status"] == 200
    merged = call(runtime.use("PATCH", "charlie", {
        "checkedAt": 1_000 * hour + 60_000, "views": 130, "commentScore": 2}), path)
    later = call(runtime.use("PATCH", "charlie", {
        "checkedAt": 1_030 * hour, "views": 900, "score": 9}), path)["data"]["thread"]
    assert merged["data"]["thread"]["history"] == [{
        "at": 1_000 * hour + 60_000, "views": 130, "score": 8, "comments": 40,
        "commentViews": 15, "commentScore": 2}]
    assert [point["views"] for point in later["history"]] == [130, 900]
    assert (later["views"], later["score"], later["comments"], later["commentScore"],
            later["checkedAt"], later["title"]) == (
        900, 9, 40, 2, 1_030 * hour, "Weekly Self Promotion Thread")
    stale = call(runtime.use("PATCH", "charlie", {"checkedAt": 5 * hour, "views": 1}),
                 path)["data"]["thread"]
    assert (stale["views"], len(stale["history"])) == (900, 2)
    for body, error in (({"checkedAt": 0}, "invalid_reddit_checkedat"),
                        ({"checkedAt": hour, "views": -1}, "invalid_reddit_views"),
                        ({"commentUrl": "https://www.reddit.com/r/devops/"},
                         "invalid_reddit_comment_url"),
                        ({"commentUrl": REPLY_URL}, "reddit_comment_not_in_thread")):
        denied = call(runtime.use("PATCH", "charlie", body), path)
        assert (denied["status"], denied["data"]["error"]) == (400, error), body
    desk = call(runtime.use("GET", "charlie"), "/api/marketing/acme/reddit")["data"]
    assert desk["redditDesk"]["summary"]["monitored"] == 1
    report = call(runtime.use("GET"), "/api/marketing/acme/report")["data"]["markdown"]
    assert ("- Monitored r/devops: Weekly Self Promotion Thread: 900 views, 9 points, "
            "40 comments, 15 comment views, 2 comment points") in report
    stopped = call(runtime.use("PATCH", "charlie", {"monitored": False}), path)["data"]["thread"]
    assert (stopped["monitored"], stopped["views"], stopped["commentUrl"]) == (
        False, 900, thread["commentUrl"])
    replied = call(runtime.use("POST", body={"url": THREAD_URL, "replyUrl": REPLY_URL}),
                   "/api/marketing/acme/reddit")["data"]["thread"]
    assert (replied["commentUrl"], replied["monitorCommentId"]) == ("", "def456")


def test_posted_links_are_added_by_hand_on_any_channel_and_keep_their_stats():
    runtime = Runtime()
    path = "/api/marketing/acme/posts"
    created = call(runtime.use("POST", "charlie", {
        "url": "linkedin.com/posts/acme_launch-123/", "title": "Launch post",
        "note": "Shared by the founder", "postedAt": 1_999_000_000_000, "likes": 12}), path)
    assert created["status"] == 201
    post = created["data"]["post"]
    assert (post["channel"], post["url"], post["source"], post["likes"], post["views"],
            post["engagements"], post["postedAt"], post["createdBy"]) == (
        "linkedin", "https://linkedin.com/posts/acme_launch-123", "manual", 12, None, 12,
        1_999_000_000_000, "charlie")
    assert [point["likes"] for point in post["history"]] == [12]
    stored = runtime.db.execute("SELECT data FROM marketing_posts").fetchone()[0]
    assert stored.startswith("sealed:") and "Shared by the founder" in stored
    newsletter = call(runtime.use("POST", body={
        "url": "https://news.example.com/issue/7", "channel": "email"}), path)
    assert newsletter["data"]["post"]["channel"] == "email"
    for body, status, error in (
            ({"url": "https://news.example.com/issue/8"}, 400, "post_channel_required"),
            ({"url": "https://news.example.com/issue/8", "channel": "nope"}, 400,
             "invalid_post_channel"),
            ({"url": "not a link"}, 400, "invalid_post_url"),
            ({"url": "https://www.linkedin.com/feed/", "views": -4}, 400, "invalid_post_views"),
            ({"url": "https://linkedin.com/posts/acme_launch-123/"}, 409, "post_exists")):
        denied = call(runtime.use("POST", body=body), path)
        assert (denied["status"], denied["data"]["error"]) == (status, error), body
    assert denied["data"]["postId"] == post["id"]
    item = f"{path}/{post['id']}"
    edited = call(runtime.use("PATCH", body={"title": "Launch day", "views": 400}),
                  item)["data"]["post"]
    assert (edited["title"], edited["views"], edited["likes"], edited["channel"],
            len(edited["history"])) == ("Launch day", 400, 12, "linkedin", 1)
    overview = call(runtime.use("GET"), "/api/marketing/acme")["data"]
    assert [entry["id"] for entry in overview["posts"]] == [
        newsletter["data"]["post"]["id"], post["id"]]
    assert overview["postsSummary"]["channels"][0]["channel"] == "email"
    assert (overview["postsSummary"]["total"], overview["postsSummary"]["views"]) == (2, 400)
    health = {item["channel"]: item["health"]
              for item in overview["summary"]["channels"]}["linkedin"]
    assert (health["postsLast30d"], health["lastPostAt"]) == (1, 1_999_000_000_000)
    assert call(runtime.use("DELETE", "charlie"), item)["data"]["deleted"] is True
    assert call(runtime.use("GET"), item)["status"] == 404
    assert call(runtime.use("GET", "bob"), path)["status"] == 404


def test_extension_reads_add_x_posts_and_update_their_stats():
    runtime = Runtime()
    path = "/api/marketing/acme/posts"
    hour = 3600 * 1000
    manual = call(runtime.use("POST", body={
        "url": "https://twitter.com/ForkMesh/status/1900000000000000001?s=20",
        "title": "Our own words"}), path)["data"]["post"]
    assert (manual["channel"], manual["url"]) == (
        "x", "https://x.com/forkmesh/status/1900000000000000001")
    first = call(runtime.use("POST", "charlie", {"posts": [
        {"url": "https://x.com/forkmesh/status/1900000000000000001", "title": "Read text",
         "postedAt": 1_990_000_000_000, "checkedAt": 1_000 * hour, "views": 50, "likes": 3,
         "replies": 1, "reposts": 0},
        {"url": "https://x.com/forkmesh/status/1900000000000000002", "channel": "x",
         "title": "Second post", "postedAt": 1_995_000_000_000, "checkedAt": 1_000 * hour,
         "views": 900, "likes": 40, "replies": 4, "reposts": 6},
        {"url": "javascript:alert(1)"}, "junk"]}), path)
    assert first["status"] == 200
    assert {key: first["data"][key] for key in ("created", "updated", "skipped", "full")} == {
        "created": 1, "updated": 1, "skipped": 2, "full": False}
    assert first["data"]["postsSummary"]["views"] == 950
    posts = {item["url"]: item for item in call(runtime.use("GET"), path)["data"]["posts"]}
    kept = posts[manual["url"]]
    assert (kept["title"], kept["source"], kept["views"], kept["postedAt"]) == (
        "Our own words", "manual", 50, 1_990_000_000_000)
    added = posts["https://x.com/forkmesh/status/1900000000000000002"]
    assert (added["source"], added["title"], added["engagements"], added["createdBy"]) == (
        "extension", "Second post", 50, "charlie")
    again = call(runtime.use("POST", "charlie", {"posts": [
        {"url": added["url"], "title": "Changed", "checkedAt": 1_002 * hour, "views": 1500},
        {"url": added["url"], "checkedAt": 10 * hour, "views": 1}]}), path)["data"]
    assert (again["created"], again["updated"]) == (0, 1)
    latest = call(runtime.use("GET"), f"{path}/{added['id']}")["data"]["post"]
    assert (latest["title"], latest["views"], latest["likes"],
            [point["views"] for point in latest["history"]]) == (
        "Second post", 1500, 40, [900, 1500])
    too_many = call(runtime.use("POST", body={"posts": [{}] * 501}), path)
    assert (too_many["status"], too_many["data"]["error"]) == (400, "invalid_posts")
    export = asyncio.run(marketing.export_org_data(runtime, "org:acme", "acme"))
    assert len(export["posts"]) == 2
    asyncio.run(marketing.purge_org_data(runtime, "org:acme"))
    assert not asyncio.run(marketing.org_has_data(runtime, "org:acme"))


def test_x_post_images_are_kept_from_x_only_and_survive_a_read_without_them():
    runtime = Runtime()
    path = "/api/marketing/acme/posts"
    hour = 3600 * 1000
    photo = "https://pbs.twimg.com/media/GlaunchA?format=jpg&name=small"
    poster = "https://pbs.twimg.com/ext_tw_video_thumb/19/pu/img/demo.jpg"
    first = call(runtime.use("POST", "charlie", {"posts": [
        {"url": "https://x.com/forkmesh/status/1900000000000000001", "title": "Launch",
         "checkedAt": 1_000 * hour, "views": 10,
         "images": [photo, photo, "http://pbs.twimg.com/media/plain.jpg",
                    "https://evil.example/track.png", 42, poster,
                    "https://pbs.twimg.com/media/c.jpg", "https://pbs.twimg.com/media/d.jpg",
                    "https://pbs.twimg.com/media/e.jpg"]}]}), path)["data"]
    assert first["created"] == 1
    post = call(runtime.use("GET"), path)["data"]["posts"][0]
    assert post["images"] == [photo, poster, "https://pbs.twimg.com/media/c.jpg",
                              "https://pbs.twimg.com/media/d.jpg"]
    again = call(runtime.use("POST", "charlie", {"posts": [
        {"url": post["url"], "checkedAt": 1_002 * hour, "views": 20, "images": []}]}), path)["data"]
    assert again["updated"] == 1
    kept = call(runtime.use("GET"), f"{path}/{post['id']}")["data"]["post"]
    assert (kept["views"], kept["images"]) == (20, post["images"])
    call(runtime.use("POST", "charlie", {"posts": [
        {"url": post["url"], "checkedAt": 1_004 * hour, "images": [poster]}]}), path)
    assert call(runtime.use("GET"), f"{path}/{post['id']}")["data"]["post"]["images"] == [poster]
    manual = call(runtime.use("POST", body={"url": "https://x.com/forkmesh/status/1900000000000000002"}),
                  path)["data"]["post"]
    assert manual["images"] == []


def test_subreddit_rules_are_member_written_and_settings_cannot_overwrite_them():
    runtime = Runtime()
    saved = call(runtime.use("POST", "charlie", {"subreddit": "r/SelfHosted", "rules": [
        {"title": "  No   spam ", "description": "Self-promotion only in the\n\n\n\nmegathread."},
        {"title": "", "description": ""},
        {"title": "x" * 300, "description": "y" * 3000}]}),
        "/api/marketing/acme/reddit-rules")
    assert saved["status"] == 200
    rules = saved["data"]["rules"]
    assert (rules["subreddit"], rules["readBy"]) == ("SelfHosted", "charlie")
    assert rules["rules"][0] == {"title": "No spam",
                                 "description": "Self-promotion only in the\n\nmegathread."}
    assert len(rules["rules"]) == 2
    assert (len(rules["rules"][1]["title"]), len(rules["rules"][1]["description"])) == (200, 2000)
    assert runtime.audits[-1][0][1] == "marketing.reddit_rules.update"
    empty = call(runtime.use("POST", "charlie", {"subreddit": "git", "rules": []}),
                 "/api/marketing/acme/reddit-rules")
    assert empty["data"]["rules"]["rules"] == []
    for body, error in (({"subreddit": "no spaces", "rules": []}, "invalid_subreddit"),
                        ({"subreddit": "git", "rules": "none"}, "invalid_reddit_rules"),
                        ({"subreddit": "git", "rules": [{"title": "a"}] * 31},
                         "too_many_reddit_rules")):
        denied = call(runtime.use("POST", "charlie", body), "/api/marketing/acme/reddit-rules")
        assert (denied["status"], denied["data"]["error"]) == (400, error), body
    assert call(runtime.use("PUT", "charlie", {"subreddit": "git"}),
                "/api/marketing/acme/reddit-rules")["status"] == 405

    call(runtime.use("PUT", body={"redditRules": {}, "reddit": {
        "subreddits": ["selfhosted", "git", "linux"], "queries": ["self-hosted git"]}}),
        "/api/marketing/acme/settings")
    call(runtime.use("POST", body={"channel": "blog", "enabled": True}),
         "/api/marketing/acme/refresh")
    listed = call(runtime.use("GET", "charlie"), "/api/marketing/acme/reddit-rules")["data"]
    assert [item["subreddit"] for item in listed["redditRules"]] == ["git", "SelfHosted"]
    overview = call(runtime.use("GET"), "/api/marketing/acme")["data"]
    assert "redditRules" not in overview["settings"]
    desk = overview["redditDesk"]
    by_name = {item["name"]: item for item in desk["subreddits"]}
    assert by_name["selfhosted"]["rulesRead"] is True
    assert by_name["selfhosted"]["rules"][0]["title"] == "No spam"
    assert by_name["selfhosted"]["rulesReadBy"] == "charlie"
    assert by_name["git"]["rulesRead"] is True and by_name["git"]["rules"] == []
    assert by_name["linux"]["rulesRead"] is False
    assert by_name["linux"]["rulesUrl"] == "https://old.reddit.com/r/linux/about/rules"
    assert desk["rulesOnFile"] == 2
    report = call(runtime.use("GET"), "/api/marketing/acme/report")["data"]
    assert "Watched subreddits with rules on file: 2 of 3." in report["markdown"]


def test_the_reddit_desk_keeps_your_own_accounts_beside_the_watch_list():
    runtime = Runtime()
    assert call(runtime.use("GET"), "/api/marketing/acme")["data"]["redditDesk"]["accounts"] == []
    saved = call(runtime.use("PUT", body={"reddit": {"accounts": [
        "ada_lovelace", "u/forkmesh", "https://www.reddit.com/user/ForkMesh/submitted/",
        "https://old.reddit.com/u/grace_h", ""]}}), "/api/marketing/acme/settings")
    assert saved["status"] == 200
    watch = saved["data"]["settings"]["reddit"]
    assert watch["accounts"] == ["ada_lovelace", "forkmesh", "grace_h"]
    assert watch["subreddits"] == list(marketing.REDDIT_DEFAULT_SUBREDDITS)
    call(runtime.use("PUT", body={"reddit": {"subreddits": ["git"], "queries": ["self-hosted git"]}}),
         "/api/marketing/acme/settings")
    desk = call(runtime.use("GET", "charlie"), "/api/marketing/acme/reddit")["data"]["redditDesk"]
    assert [item["name"] for item in desk["subreddits"]] == ["git"]
    assert desk["accounts"] == [
        {"name": "ada_lovelace", "url": "https://www.reddit.com/user/ada_lovelace/submitted/"},
        {"name": "forkmesh", "url": "https://www.reddit.com/user/forkmesh/submitted/"},
        {"name": "grace_h", "url": "https://www.reddit.com/user/grace_h/submitted/"}]
    for accounts, error in ((["not a name!"], "invalid_reddit_account"),
                            (["user%d" % index for index in range(11)], "too_many_reddit_accounts")):
        denied = call(runtime.use("PUT", body={"reddit": {"accounts": accounts}}),
                      "/api/marketing/acme/settings")
        assert (denied["status"], denied["data"]["error"]) == (400, error), accounts
    member = call(runtime.use("PUT", "charlie", {"reddit": {"accounts": ["charlie"]}}),
                  "/api/marketing/acme/settings")
    assert (member["status"], member["data"]["error"]) == (403, "organization_admin_required")
    web = (PACKAGE / "content" / "web" / "page.js").read_text()
    view = (PACKAGE / "content" / "web" / "view.html").read_text()
    assert "data-marketing-reddit-watch-accounts" in view and "data-marketing-reddit-accounts" in view
    assert 'accounts: lines("accounts")' in web and "desk.accounts" in web


def test_reddit_desk_is_wired_on_the_web_surface_with_the_extension():
    web = (PACKAGE / "content" / "web" / "page.js").read_text()
    view = (PACKAGE / "content" / "web" / "view.html").read_text()
    for hook in ("data-marketing-reddit-page", "data-marketing-reddit-rules-list",
                 "data-marketing-reddit-rules-copy", "Copy rules for the scout",
                 'href="/dashboard/extension"', "ForkMesh browser extension"):
        assert hook in view, hook
    for hook in ('orgPath(`/reddit${id ? `/${id}` : ""}`)', "rulesUrl", "rulesReadAt",
                 'item.source === "extension"', "from the browser extension",
                 "function redditRulesMarkdown",
                 'addEventListener("click", copyRedditRules)'):
        assert hook in web, hook
    rules_copy = web[web.index("function redditRulesMarkdown"):web.index("function copyRedditRules")]
    assert "rulesReadBy" not in rules_copy
    assert "ForkMesh never fetches, reads or posts to Reddit" not in view


def test_every_catalog_category_has_a_playbook_and_known_channels_have_hosts():
    categories = {item[2] for item in marketing.CHANNEL_CATALOG}
    assert categories == set(marketing.CHANNEL_PLAYBOOKS)
    for playbook in marketing.CHANNEL_PLAYBOOKS.values():
        assert playbook["goal"] and len(playbook["kpis"]) == 3 and playbook["tips"]
        assert 1 <= playbook["cadenceDays"] <= 30
    assert set(marketing.CHANNEL_HOSTS) <= set(marketing.CHANNELS)
    for hosts in marketing.CHANNEL_HOSTS.values():
        assert hosts and all("/" not in host and host == host.lower() for host in hosts)
    overview = call(Runtime().use("GET"), "/api/marketing/acme")["data"]
    assert set(overview["channelPlaybooks"]) == categories


def test_a_page_read_by_the_extension_becomes_a_snapshot_and_channel_profile():
    runtime = Runtime()
    call(runtime.use("PUT", body={"channels": [
        {"id": "instagram", "profileUrl": "https://www.instagram.com/acme/"}]}),
        "/api/marketing/acme/settings")
    call(runtime.use("POST", body={"channel": "instagram", "reach": 900, "clicks": 12,
                                   "followers": 1000}), "/api/marketing/acme/metrics")
    body = {"channel": "instagram", "url": "https://www.instagram.com/acme/",
            "title": "Acme (@acme)", "description": "1,234 Followers", "handle": "@acme",
            "image": "http://insecure.example/a.png",
            "metrics": {"followers": 1234, "following": 56, "posts": 78, "likes": 10,
                        "replies": 3, "views": -5, "bogus": 9},
            "rating": 4.567, "lastPostAt": runtime.clock - 86400000, "postsLast30d": 6,
            "recent": [{"url": "https://www.instagram.com/p/abc/", "title": "Launch",
                        "at": runtime.clock - 86400000, "likes": 4}] * 8,
            "signals": ["followers <- page"]}
    created = call(runtime.use("POST", "charlie", body), "/api/marketing/acme/capture")
    assert created["status"] == 201, created
    metrics = created["data"]["metrics"]
    assert (metrics["followers"], metrics["following"], metrics["posts"]) == (1234, 56, 78)
    assert (metrics["reach"], metrics["clicks"]) == (900, 12)
    assert metrics["engagements"] == 13 and metrics["views"] == 0 and metrics["rating"] == 4.57
    snapshot = call(runtime.use("GET"), "/api/marketing/acme/metrics")["data"]["metrics"][0]
    assert snapshot["source"] == "extension" and snapshot["note"] == "Read from www.instagram.com"
    overview = call(runtime.use("GET"), "/api/marketing/acme")["data"]
    profile = next(item for item in overview["settings"]["channels"] if item["id"] == "instagram")["capture"]
    assert profile["by"] == "charlie" and profile["image"] == ""
    assert len(profile["recent"]) == marketing.CAPTURE_RECENT
    assert profile["metrics"] == {"followers": 1234, "following": 56, "posts": 78,
                                  "likes": 10, "replies": 3}
    stored = runtime.db.execute("SELECT data FROM marketing_settings").fetchone()[0]
    assert stored.startswith("sealed:")
    row = next(item for item in overview["summary"]["channels"] if item["channel"] == "instagram")
    health = row["health"]
    assert (row["followers"], row["rating"], health["followersChange"]) == (1234, 4.57, 234)
    assert health["source"] == "extension" and health["postsLast30d"] == 6
    assert health["state"] == "good", health
    resaved = call(runtime.use("PUT", body={"channels": [
        {"id": "instagram", "profileUrl": "https://www.instagram.com/acme/", "displayName": "IG"}]}),
        "/api/marketing/acme/settings")["data"]["settings"]["channels"]
    assert next(item for item in resaved if item["id"] == "instagram")["capture"]["handle"] == "@acme"


def test_a_capture_must_come_from_the_channels_own_site_and_carry_numbers():
    runtime = Runtime()
    call(runtime.use("PUT", body={"channels": [
        {"id": "blog", "profileUrl": "https://acme.example/blog"}]}), "/api/marketing/acme/settings")
    capture = lambda actor, body: call(runtime.use("POST", actor, body), "/api/marketing/acme/capture")
    numbers = {"metrics": {"followers": 5}}
    assert capture("charlie", {"channel": "x", "url": "https://evil.example/x", **numbers})[
        "data"]["error"] == "capture_host_mismatch"
    assert capture("charlie", {"channel": "x", "url": "https://x.com/acme"})[
        "data"]["error"] == "capture_found_no_numbers"
    assert capture("charlie", {"channel": "x", "url": "http://x.com/acme", **numbers})[
        "data"]["error"] == "invalid_capture_url"
    assert capture("charlie", {"channel": "nope", "url": "https://x.com/acme", **numbers})[
        "data"]["error"] == "invalid_channel"
    assert capture("charlie", {"channel": "blog", "url": "https://acme.example/blog", **numbers})["status"] == 201
    assert capture("charlie", {"channel": "blog", "url": "https://www.acme.example/", **numbers})["status"] == 201
    assert capture("charlie", {"channel": "medium", "url": "https://medium.com/@acme", **numbers})[
        "data"]["error"] == "channel_not_configured"
    added = capture("alice", {"channel": "medium", "url": "https://medium.com/@acme", **numbers})
    assert added["status"] == 201
    settings = call(runtime.use("GET"), "/api/marketing/acme/settings")["data"]["settings"]
    medium = next(item for item in settings["channels"] if item["id"] == "medium")
    assert medium["profileUrl"] == "https://medium.com/@acme"
    assert call(runtime.use("GET"), "/api/marketing/acme/capture")["status"] == 405


def test_github_repo_refresh_counts_repo_stars_forks_and_owner_followers():
    runtime = Runtime()
    day = marketing.DAY_MS
    _configure(runtime, github="https://github.com/forkmesh/forkmesh")
    call(runtime.use("POST", body={"channel": "github", "followers": 40,
                                   "observedAt": runtime.clock - 40 * day}),
         "/api/marketing/acme/metrics")
    runtime.outbound["https://api.github.com/repos/forkmesh/forkmesh"] = json.dumps({
        "full_name": "forkmesh/forkmesh", "stargazers_count": 52, "forks_count": 8,
        "html_url": "https://github.com/forkmesh/forkmesh"})
    runtime.outbound["https://api.github.com/users/forkmesh"] = json.dumps({
        "login": "forkmesh", "followers": 7})
    runtime.outbound["https://api.github.com/repos/forkmesh/forkmesh/releases?per_page=20"] = "[]"
    metrics = _refresh(runtime, "github")["data"]["refresh"]["metrics"]
    assert (metrics["followers"], metrics["stars"], metrics["forks"],
            metrics["engagements"]) == (7, 52, 8, 60)
    summary = call(runtime.use("GET"), "/api/marketing/acme")["data"]["summary"]
    row = next(item for item in summary["channels"] if item["channel"] == "github")
    assert (row["followers"], row["stars"], row["forks"]) == (7, 52, 8)
    assert row["health"]["followersChange"] is None and row["health"]["starsChange"] is None
    assert "growing" not in {check["id"] for check in row["health"]["checks"]}
    markdown = call(runtime.use("GET"), "/api/marketing/acme/report")["data"]["markdown"]
    assert "7 (52 stars, 8 forks)" in markdown


def test_github_audience_growth_follows_stars():
    runtime = Runtime()
    day = marketing.DAY_MS
    _configure(runtime, github="https://github.com/forkmesh")
    for observed, followers, stars in ((runtime.clock - 40 * day, 5, 100),
                                       (runtime.clock - 1 * day, 3, 130)):
        call(runtime.use("POST", body={"channel": "github", "followers": followers,
                                       "stars": stars, "forks": 9, "observedAt": observed}),
             "/api/marketing/acme/metrics")
    summary = call(runtime.use("GET"), "/api/marketing/acme")["data"]["summary"]
    row = next(item for item in summary["channels"] if item["channel"] == "github")
    health = row["health"]
    assert (row["stars"], row["forks"]) == (130, 9)
    assert (health["followersChange"], health["starsChange"]) == (-2, 30)
    assert health["engagementRate"] is None
    assert next(check for check in health["checks"] if check["id"] == "growing")["ok"]
    x = next(item for item in summary["channels"] if item["channel"] == "x")
    assert x["health"]["starsChange"] is None


def test_channel_health_flags_stale_unlinked_quiet_and_shrinking_channels():
    runtime = Runtime()
    day = marketing.DAY_MS
    now = runtime.clock
    call(runtime.use("PUT", body={"channels": [
        {"id": "x", "profileUrl": "https://x.com/acme"},
        {"id": "reddit", "profileUrl": ""},
        {"id": "youtube", "profileUrl": "https://www.youtube.com/@acme", "enabled": False},
        {"id": "mastodon", "profileUrl": "https://mastodon.social/@acme"}]}),
        "/api/marketing/acme/settings")
    for observed, followers in ((now - 40 * day, 500), (now - 1 * day, 450)):
        call(runtime.use("POST", body={"channel": "x", "followers": followers,
                                       "observedAt": observed}), "/api/marketing/acme/metrics")
    call(runtime.use("POST", body={"channel": "mastodon", "followers": 80,
                                   "observedAt": now - 20 * day}), "/api/marketing/acme/metrics")
    summary = call(runtime.use("GET"), "/api/marketing/acme")["data"]["summary"]
    rows = {item["channel"]: item["health"] for item in summary["channels"]}
    assert rows["x"]["state"] == "attention" and rows["x"]["followersChange"] == -50
    failing = {check["id"] for check in rows["x"]["checks"] if not check["ok"]}
    assert failing == {"posting", "growing"}
    assert rows["reddit"]["state"] == "setup"
    assert rows["youtube"]["state"] == "paused"
    assert rows["mastodon"]["state"] == "stale"
    assert summary["health"]["stale"] >= 1 and summary["health"]["setup"] >= 1
    markdown = call(runtime.use("GET"), "/api/marketing/acme/report")["data"]["markdown"]
    assert "## Channels" in markdown
    assert "| X | Needs attention | 450 | -50 | 0 |" in markdown
    assert "- X: posted in the last 4 days; audience growing over 30 days" in markdown


def test_desktop_marketing_is_one_dashboard_page_like_the_web():
    desktop = PACKAGE / "content" / "desktop"
    native = (desktop / "MarketingFeature.cpp").read_text()
    header = (desktop / "MarketingFeature.h").read_text()
    view = (PACKAGE / "content" / "web" / "view.html").read_text()
    build = json.loads((desktop / "build.json").read_text())
    assert {"MarketingDashboard.h", "MarketingDashboard.cpp"} <= set(build["sources"])
    assert "QTabWidget" not in native and "QTabWidget" not in header
    assert "m_scroll->setWidget(page)" in native
    for title in ("Web traffic by channel", "Search keywords", "Campaigns", "Content flow",
                  "Content pipeline", "This week", "Top landing pages"):
        assert f"<h2>{title}</h2>" in view, title
        assert f'DashboardPanel(QStringLiteral("{title}"))' in native, title
    for title in ("Marketing agent", "Ads", "Post to X", "Reach by channel",
                  "Ads manager", "Manage channels", "Add channels"):
        assert f'DashboardPanel(QStringLiteral("{title}"))' in native, title
    assert 'DashboardPanel(QStringLiteral("Channels"))' not in native
    assert "QWidget *chip = channelChip(item, agents);" in native
    assert "</span>&nbsp;need&nbsp;a&nbsp;look" in native
    for path in ("/performance", "/assistant", "/x-post", "/campaigns", "/keywords"):
        assert f'organizationPath(QStringLiteral("{path}")' in native, path
    for label in ("Visits · %1D", "Conversions", "Audience", "Reach", "Published · %1D",
                  "Live campaigns"):
        assert f'QStringLiteral("{label}")' in native, label
    for status in marketing.CAMPAIGN_STATUSES:
        assert f'QStringLiteral("{status}")' in native
    for objective in marketing.CAMPAIGN_OBJECTIVES:
        assert f'QStringLiteral("{objective}")' in native


def test_desktop_has_every_web_marketing_section():
    desktop = PACKAGE / "content" / "desktop"
    pages = (desktop / "MarketingPages.cpp").read_text()
    native = (desktop / "MarketingFeature.cpp").read_text()
    view = (PACKAGE / "content" / "web" / "view.html").read_text()
    build = json.loads((desktop / "build.json").read_text())
    assert {"MarketingCommon.h", "MarketingCommon.cpp", "MarketingPages.cpp"} <= set(build["sources"])
    for page, label in (("dashboard", "Dashboard"), ("performance", "Performance"),
                        ("channels", "Channels"), ("ads", "Ads manager"),
                        ("growth", "Growth")):
        assert f'data-marketing-page="{page}" type="button" aria-pressed' in view, page
        assert f'QStringLiteral("{label}")' in pages, label
    for title in ("Where to improve", "Ads measured by Analytics", "Landing pages",
                  "Spend by channel", "Needs attention", "Funnel",
                  "Link builder", "Experiment backlog", "Weekly funnel"):
        assert f"<h2>{title}</h2>" in view or f"{title}</h2>" in view, title
        assert f'DashboardPanel(QStringLiteral("{title}"))' in pages, title
    for title in ("Watch", "Before you reply", "Subreddit rules", "Thread log",
                  "Where to post next", "What to talk about"):
        assert f"<h2>{title}</h2>" in view or f"{title}</h2>" in view, title
        assert f'redditPanel(QStringLiteral("{title}"))' in pages, title
    assert 'redditPanel(QStringLiteral("Results by subreddit"))' in pages
    for path in ("/experiments", "/funnel", "/links", "/report", "/reddit", "/settings",
                 "/x-connection", "/keywords", "/performance"):
        assert f'organizationPath(QStringLiteral("{path}")' in pages, path
    assert "reddit_thread_exists" in pages
    assert "editXConnection();" in native and "importKeywords();" in native
    assert "openWebDashboard();\n        setStatus" not in native


def test_the_reddit_desk_lives_on_the_reddit_channel_and_every_channel_has_a_desk():
    desktop = PACKAGE / "content" / "desktop"
    pages = (desktop / "MarketingPages.cpp").read_text()
    native = (desktop / "MarketingFeature.cpp").read_text()
    view = (PACKAGE / "content" / "web" / "view.html").read_text()
    web = (PACKAGE / "content" / "web" / "page.js").read_text()
    assert 'QStringLiteral("Reddit desk")}' not in pages and "redditTools" not in pages
    assert '"marketingRedditPage"' not in pages
    assert 'data-marketing-page="reddit" type="button" aria-pressed' not in view
    build = pages.split("void MarketingFeature::buildPages(", 1)[1].split("\nvoid MarketingFeature::", 1)[0]
    assert "static_cast<QVBoxLayout *>(m_detailRedditBox->layout())" in build
    assert build.index('redditPanel(QStringLiteral("Thread log"))') < build.index(
        'redditPanel(QStringLiteral("Results by subreddit"))')
    detail = native.split("void MarketingFeature::renderChannelDetail()", 1)[1].split("\nvoid MarketingFeature::", 1)[0]
    assert "m_detailRedditBox->setVisible(reddit);" in detail and "renderChannelDesk(id);" in detail
    assert 'id == QLatin1String("reddit") && m_performance.isEmpty()' in native
    desk = pages.split("void MarketingFeature::renderChannelDesk(", 1)[1].split("\nvoid MarketingFeature::", 1)[0]
    for label in ("Next post due", "Queued", "Posted links", "Campaigns", "Referrals",
                  "Threads · 7D", "Rules on file", "Best subreddit"):
        assert f'QStringLiteral("{label}")' in desk, label
    assert "${channelDesk(item, health)}" in web
    card = web.split("function channelDesk(", 1)[1].split("\n  function channelCard(", 1)[0]
    assert 'data-marketing-page="reddit"' in card and "Open the Reddit desk" in card
    assert 'data-marketing-page="channels" type="button" class="mk-link">‹ All channels' in view


def test_desktop_dashboard_leads_with_what_needs_attention():
    desktop = PACKAGE / "content" / "desktop"
    native = (desktop / "MarketingFeature.cpp").read_text()
    pages = (desktop / "MarketingPages.cpp").read_text()
    build = native.split("MarketingFeature::MarketingFeature(", 1)[1].split("\nvoid MarketingFeature::", 1)[0]
    attention = native.split("void MarketingFeature::renderAttention()", 1)[1].split("\nvoid MarketingFeature::", 1)[0]
    assert build.index("grid->addPanel(m_attentionPanel, 3);") < build.index("grid->addPanel(m_trafficPanel, 2);")
    assert "m_kpis->setCompact(true);" in build
    assert "m_suggestionsBox" not in native and "renderSuggestions" not in native
    for source in ("m_suggestions.at(index)", "adAttention()", "runSuggestion(index)", "editAd(ad)", "editContent(item)"):
        assert source in attention, source
    assert "QList<MarketingFeature::AdAttention> MarketingFeature::adAttention() const" in pages
    assert "const QList<AdAttention> attention = adAttention();" in pages


def test_desktop_channel_click_opens_the_channel_page():
    desktop = PACKAGE / "content" / "desktop"
    native = (desktop / "MarketingFeature.cpp").read_text()
    host = (REPO / "desktop" / "src" / "MainWindowPackageRail.cpp").read_text()
    assert "tile.opensPage = true;" in native
    assert "void MarketingFeature::openRailTile(const QString &tile)" in native
    assert "tile->onClicked = [this, id] { openChannel(id); };" in native
    assert "connect(nameLabel, &QLabel::linkActivated, this, [this, id] { openChannel(id); });" in native
    for title in ("Health and settings", "Playbook"):
        assert f'detailBox(QStringLiteral("{title}"), 1)' in native, title
    assert 'new DashboardPanel(QStringLiteral("Next steps"))' in native
    assert "detailSideLayout->addWidget(m_detailHead);" in native
    assert 'sideBox(detailWorkLayout, QStringLiteral("People to follow"), &m_detailPeoplePanel)' in native
    assert "detailWorkLayout->addWidget(m_detailContentPanel, 1);" in native
    assert "detailGrid->addPanel(detailWork, 2);" in native
    assert 'sideBox(detailFindLayout, QStringLiteral("Searches to check"), &m_detailSearchesPanel)' in native
    assert "detailFindLayout->addWidget(m_detailSnapshotsPanel);" in native
    assert "detailGrid->addPanel(detailFind, 1);" in native
    assert 'DashboardPanel(QStringLiteral("Posted links"))' not in native
    assert "m_detailContentPanel->body()->addWidget(m_detailPosts);" in native
    assert 'organizationPath(QStringLiteral("/mastodon?topics="))' in native
    assert 'organizationPath(QStringLiteral("/mastodon")),' in native
    assert "m_detailKpis->setCompact(true);" in native
    for title in ("Followers over time", "Posts and content", "Ads", "Recorded numbers"):
        assert f'DashboardPanel(QStringLiteral("{title}"))' in native, title
    assert "openPackageRailTile(packageId, tileId);" in host
    assert "page.widget->openRailTile(tileId);" in host


def test_desktop_mastodon_follow_button_greys_out_once_clicked():
    native = (PACKAGE / "content" / "desktop" / "MarketingFeature.cpp").read_text()
    render = native.split("void MarketingFeature::renderMastodon()", 1)[1].split("\nvoid MarketingFeature::", 1)[0]
    follow = native.split("void MarketingFeature::followMastodon(", 1)[1].split("\nvoid MarketingFeature::", 1)[0]
    assert 'follow->setObjectName(QStringLiteral("primaryButton"));' in render
    assert "ghostButton" not in render
    assert render.count("follow->setEnabled(false);") == 2
    assert "if (m_mastodonFollowed.contains(personId))\n            continue;" in render
    assert "auto *scroller = new QScrollArea;" in render
    assert "scroller->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);" in render
    assert "TileGrid(340" not in render
    assert "m_mastodonOpened.contains(personId)" in render
    assert "m_mastodonOpened.insert(personId);" in follow
    assert "m_mastodonPending.insert(personId);" in follow


def test_channel_page_shows_the_attached_special_agent():
    native = (PACKAGE / "content" / "desktop" / "MarketingFeature.cpp").read_text()
    market = (REPO / "desktop" / "src" / "MainWindowMarket.cpp").read_text()
    manifest = json.loads((PACKAGE / "manifest.json").read_text())
    assert "desktop.agents.run" in {item["id"] for item in manifest["permissions"]}
    assert 'permissionIds.contains(QStringLiteral("desktop.agents.run"))' in market
    assert 'detailBox(QStringLiteral("Special agent"), 3, &m_detailAgentPanel)' in native
    assert "renderChannelAgents(id);" in native
    assert 'agent.value(QStringLiteral("marketingChannels")).toArray()' in native
    for action in ("open", "run", "prompt"):
        assert f'runSpecialAgentAction(agentId, QStringLiteral("{action}")' in native, action
        assert f'action == QLatin1String("{action}")' in market, action
    attached = {}
    for path in sorted((REPO / ".forkmesh" / "teammates").glob("*.json")):
        for channel in json.loads(path.read_text()).get("marketingChannels", []):
            assert channel in marketing.CHANNELS, (path.name, channel)
            attached.setdefault(channel, []).append(path.stem)
    assert attached == {"reddit": ["reddit-agent"], "discord": ["hearth"],
                        "hacker-news": ["y-combinator-news-agent"], "blog": ["blogger"],
                        "x": ["herald"]}


def test_channels_page_and_update_buttons_are_wired_on_web_desktop_and_phones():
    web = (PACKAGE / "content" / "web" / "page.js").read_text()
    view = (PACKAGE / "content" / "web" / "view.html").read_text()
    native = (PACKAGE / "content" / "desktop" / "MarketingFeature.cpp").read_text()
    assert 'data-marketing-page="channels"' in view and "data-marketing-channels-page" in view
    for hook in ("data-marketing-health-stat", "data-marketing-channel-groups",
                 "data-marketing-extension-banner", "data-marketing-capture-stale",
                 'href="/dashboard/extension"'):
        assert hook in view, hook
    for hook in ("data-marketing-channel-capture", 'type: "marketing-capture"',
                 'source: "forkmesh-marketing-page"', '"marketing-capture-update"',
                 "dataset.forkmeshMarketingCapture", 'params.get("capture")', "channelPlaybooks",
                 "renderChannelHealth", "captureStale"):
        assert hook in web, hook
    for state in ("good", "attention", "stale", "setup", "paused"):
        assert f'["{state}",' in web and f'QLatin1String("{state}")' in native
    assert 'constexpr char kMarketingPage[] = "https://app.forkmesh.com/dashboard/marketing";' in native
    assert 'QStringLiteral("org=%1&page=channels&capture=%2")' in native
    assert 'QStringLiteral("Update")' in native and 'QStringLiteral("Status")' in native
    assert "m_channelsTable->setItem(row, 17, statusItem)" in native
    ios = (REPO / "native" / "ios" / "ForkMesh" / "Features" / "Packages" / "MarketingScreen.swift").read_text()
    ios_model = (REPO / "native" / "ios" / "ForkMeshCore" / "Sources" / "FMPackages" / "MarketingModels.swift").read_text()
    android = (REPO / "native" / "android" / "app" / "src" / "main" / "kotlin" / "com" / "forkmesh"
               / "nativeapp" / "feature" / "market" / "BusinessScreens.kt").read_text()
    android_model = (REPO / "native" / "android" / "core" / "src" / "main" / "kotlin" / "com" / "forkmesh"
                     / "nativeapp" / "market" / "Business.kt").read_text()
    for source in (ios_model, android_model):
        assert "https://app.forkmesh.com/dashboard/marketing" in source
        assert '"capture"' in source or "capture=" in source
        assert '"followersChange"' in source
    assert "Update with extension" in ios and "Update with extension" in android


def test_reddit_rules_copy_for_the_scout_is_dated_markdown_without_the_reader():
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    web = (PACKAGE / "content" / "web" / "page.js").read_text()
    body = web[web.index("function redditRulesMarkdown"):web.index("function copyRedditRules")]
    day = 1791244800000
    desk = {"subreddits": [
        {"name": "selfhosted", "rulesRead": True, "rulesReadAt": day, "rulesReadBy": "Reader Name",
         "rulesUrl": "https://old.reddit.com/r/selfhosted/about/rules",
         "rules": [{"title": "No spam", "description": "New projects\n  go in the megathread."},
                   {"title": "Be nice"}]},
        {"name": "git", "rulesRead": True, "rulesReadAt": day,
         "rulesUrl": "https://old.reddit.com/r/git/about/rules", "rules": []},
        {"name": "devops", "rulesRead": False, "rulesUrl": "https://old.reddit.com/r/devops/about/rules"}]}
    script = ("const utcDate = (ts) => new Date(ts).toISOString().slice(0, 10);\n" + body +
              "\nconst desk = JSON.parse(process.argv[1]);"
              "\nprocess.stdout.write(JSON.stringify([redditRulesMarkdown(desk),"
              " redditRulesMarkdown({subreddits: [{name: 'x', rulesRead: false}]})]));")
    result = subprocess.run([node, "-e", script, json.dumps(desk)], capture_output=True,
                            text=True, timeout=30, check=True)
    text, empty = json.loads(result.stdout)
    assert empty == ""
    assert text.startswith("# Subreddit rules from the Reddit desk (copied ")
    assert "## r/selfhosted\n\nRead 2026-10-06 from https://old.reddit.com/r/selfhosted/about/rules" in text
    assert "1. No spam: New projects go in the megathread.\n2. Be nice\n" in text
    assert "## r/git\n\nRead 2026-10-06 from https://old.reddit.com/r/git/about/rules\n\nThe rules page listed no rules." in text
    assert text.endswith("Not read yet: r/devops")
    assert "Reader Name" not in text


def _analytics_runtime():
    runtime = Runtime()
    analytics = marketing._analytics_module()
    runtime.db.executescript(";\n".join(analytics.DATA_SCHEMA_UP))
    runtime.db.execute("INSERT INTO org_plugin_installs VALUES(?,?,?,?,?,?)",
                       ("org:acme", "analytics", "acme", "bi:alice", "alice", 1))
    return runtime, analytics


def _seed_visits(runtime, analytics, sessions):
    events = []
    for index, (tags, paths) in enumerate(sessions):
        start = runtime.clock - 3 * marketing.DAY_MS + index * 60000
        for step, path in enumerate(paths):
            events.append({"e": "$pageview", "d": "visitor-%d" % index,
                           "s": "session-%04d" % index, "t": start + step * 20000,
                           "p": dict({"$pathname": path}, **tags)})
    runtime.db.execute(
        "INSERT INTO analytics_batches VALUES(?,?,?,?,?,?,?,?,?)",
        ("b" * 32, "a" * 32, "org:acme", analytics._day(events[0]["t"]),
         events[0]["t"], events[-1]["t"], len(events), 0,
         "sealed:" + json.dumps(events)))


def test_performance_joins_analytics_campaigns_with_ads_reddit_and_the_report():
    runtime, analytics = _analytics_runtime()
    empty = call(runtime.use("GET"), "/api/marketing/acme/performance")
    assert empty["data"]["performance"]["status"] == "no_project"
    assert [item["code"] for item in empty["data"]["performance"]["insights"]] == [
        "analytics_no_project"]
    runtime.db.execute(
        "INSERT INTO analytics_projects VALUES(?,?,?,?,?,?,?,?,?)",
        ("a" * 32, "org:acme", "acme", "fma_" + "0" * 40, 1, "alice", 1, 1,
         'sealed:{"name": "Site"}'))
    saved = call(runtime.use("PUT", body={
        "attribution": {"campaigns": ["launch", "reddit_scout"],
                        "landingUrl": "https://forkmesh.com/"},
        "analytics": {"goals": [
            {"label": "Signup", "event": "$pageview", "property": "$pathname",
             "operator": "icontains", "value": "/signup"},
            {"label": "Broken", "event": ""},
            {"event": "download", "operator": "regex"}]}}),
        "/api/marketing/acme/settings")
    goals = saved["data"]["settings"]["analytics"]["goals"]
    assert [goal["label"] for goal in goals] == ["Signup", "download"]
    assert goals[1]["operator"] == "exact" and goals[1]["property"] == ""
    call(runtime.use("PUT", body={"analytics": {"goals": goals[:1]}}),
         "/api/marketing/acme/settings")
    call(runtime.use("POST", body={
        "name": "Launch promo", "channel": "reddit", "status": "active",
        "destinationUrl": "https://forkmesh.com/?utm_source=reddit&utm_medium=paid"
                          "&utm_campaign=launch", "spendCents": 5000, "clicks": 100}),
        "/api/marketing/acme/ads")
    call(runtime.use("POST", body={
        "name": "Bare link", "channel": "x", "status": "active",
        "destinationUrl": "https://forkmesh.com/"}), "/api/marketing/acme/ads")
    call(runtime.use("POST", body={"url": THREAD_URL}), "/api/marketing/acme/reddit")
    launch = {"utm_source": "reddit", "utm_medium": "paid", "utm_campaign": "launch"}
    scout = {"utm_source": "reddit", "utm_medium": "social",
             "utm_campaign": "reddit_scout", "utm_content": "selfhosted"}
    _seed_visits(runtime, analytics,
                 [(launch, ["/"])] * 20 + [(launch, ["/", "/signup"])] * 5
                 + [({}, ["/", "/docs"])] * 10 + [(scout, ["/"])] * 2
                 + [({"utm_source": "x", "utm_campaign": "rogue"}, ["/"])])
    perf = call(runtime.use("GET", params={"from": runtime.clock - 7 * marketing.DAY_MS}),
                "/api/marketing/acme/performance")["data"]["performance"]
    assert perf["status"] == "ok" and perf["project"]["siteDefault"] is True
    assert perf["host"] == "forkmesh.com" and perf["goals"] == ["Signup"]
    assert perf["totals"]["sessions"] == 38 and perf["totals"]["conversions"] == 5
    row = next(item for item in perf["campaigns"] if item["campaign"] == "launch")
    assert (row["sessions"], row["conversions"], row["bounceRate"]) == (25, 5, 0.8)
    assert row["ads"] == 1 and row["spend"] == [
        {"currency": "USD", "cents": 5000, "perSessionCents": 200,
         "perConversionCents": 1000}]
    assert next(item for item in perf["campaigns"]
                if item["campaign"] == "rogue")["listed"] is False
    promo, bare = sorted(perf["ads"], key=lambda item: item["name"], reverse=True)
    assert (promo["sessions"], promo["conversions"], promo["costPerSessionCents"],
            promo["costPerConversionCents"], promo["clickToVisit"]) == (25, 5, 200, 1000, 0.25)
    assert bare["tagged"] is False and bare["sessions"] is None
    assert perf["reddit"][0]["subreddit"] == "selfhosted"
    assert perf["reddit"][0]["sessions"] == 2 and perf["reddit"][0]["threads"] == 1
    codes = [item["code"] for item in perf["insights"]]
    assert codes[0] == "ad_untagged"
    for code in ("high_bounce", "ad_click_gap", "unlisted_campaign", "reddit_best"):
        assert code in codes
    assert "no_goal" not in codes
    report = call(runtime.use("GET"), "/api/marketing/acme/report")["data"]
    assert report["performanceStatus"] == "ok"
    markdown = report["markdown"]
    assert "## Campaign results (Analytics)" in markdown
    assert "| launch (reddit / paid) | 25 |" in markdown
    assert "- Launch promo: 25 visits at USD 2 per visit, 5 conversions" in markdown
    assert "Where to improve:" in markdown and "links without campaign tags" in markdown


def test_performance_counts_views_for_each_blog_post_and_per_day():
    runtime, analytics = _analytics_runtime()
    runtime.db.execute(
        "INSERT INTO analytics_projects VALUES(?,?,?,?,?,?,?,?,?)",
        ("a" * 32, "org:acme", "acme", "fma_" + "0" * 40, 1, "alice", 1, 1,
         'sealed:{"name": "Site"}'))
    call(runtime.use("PUT", body={"channels": [
        {"id": "blog", "enabled": True, "profileUrl": "https://forkmesh.com/blog/"},
        {"id": "github", "enabled": True, "profileUrl": "https://github.com/forkmesh"}]}),
        "/api/marketing/acme/settings")
    site = {"$host": "www.forkmesh.com"}
    _seed_visits(runtime, analytics,
                 [(site, ["/blog/new", "/blog/old"])] * 3 + [(site, ["/blog/new/"])] * 2
                 + [(site, ["/", "/docs"])] * 4 + [({"$host": "elsewhere.test"}, ["/blog/new"])])
    perf = call(runtime.use("GET", params={"from": runtime.clock - 7 * marketing.DAY_MS}),
                "/api/marketing/acme/performance")["data"]["performance"]
    blog = perf["channelViews"]["blog"]
    assert (blog["host"], blog["path"], blog["views"], blog["visitors"]) == (
        "forkmesh.com", "/blog", 8, 5)
    assert sum(count for _, count in blog["days"]) == 8
    assert all(day % marketing.DAY_MS == 0 for day, _ in blog["days"])
    assert set(perf["channelViews"]) == {"blog"}
    pages = {(row["host"], row["path"]): (row["views"], row["visitors"]) for row in perf["pages"]}
    assert pages == {("forkmesh.com", "/blog/new"): (5, 5), ("forkmesh.com", "/blog/old"): (3, 3)}


def test_performance_explains_missing_analytics_and_the_report_still_renders():
    runtime = Runtime()
    perf = call(runtime.use("GET"), "/api/marketing/acme/performance")
    assert perf["data"]["performance"]["status"] == "not_installed"
    assert perf["data"]["performance"]["insights"][0]["code"] == "analytics_not_installed"
    report = call(runtime.use("GET"), "/api/marketing/acme/report")["data"]
    assert report["performanceStatus"] == "not_installed"
    assert report["markdown"].rstrip().endswith(
        "- Install Analytics for this organization. Marketing reads visits and "
        "conversions from the organization's Analytics package; it is free and bundled.")


def _referrer_runtime(origin="https://forkmesh.com"):
    runtime = Runtime()
    runtime.public_origin = lambda: origin
    runtime.db.executescript("""
      CREATE TABLE site_referrers(host TEXT PRIMARY KEY,visits INTEGER NOT NULL DEFAULT 0,
        first_ts INTEGER NOT NULL DEFAULT 0,last_ts INTEGER NOT NULL DEFAULT 0,
        last_url TEXT NOT NULL DEFAULT '');
      INSERT INTO site_referrers VALUES('google.com',225,1,1759723260000,'https://google.com/');
      INSERT INTO site_referrers VALUES('wellfound.com',291,1,1754900000000,
        'https://wellfound.com/company/forkmesh');
      INSERT INTO site_referrers VALUES('t.co',22,1,2,'javascript:alert(1)');
      INSERT INTO site_referrers VALUES('gone.example',0,1,3,'https://gone.example/');
      INSERT INTO site_referrers VALUES('forkmesh.com',900,1,4,'https://forkmesh.com/blog/');
      INSERT INTO site_referrers VALUES('app.forkmesh.com',400,1,5,'https://app.forkmesh.com/');
      INSERT INTO site_referrers VALUES('notforkmesh.com',7,1,6,'https://notforkmesh.com/');
    """)
    return runtime


def test_overview_and_report_show_the_sites_http_referrers():
    runtime = _referrer_runtime()
    sites = call(runtime.use("GET"), "/api/marketing/acme")["data"]["referringSites"]
    assert (sites["status"], sites["host"], sites["sites"], sites["visits"]) == (
        "ok", "forkmesh.com", 4, 545)
    assert [(row["host"], row["visits"]) for row in sites["board"]] == [
        ("wellfound.com", 291), ("google.com", 225), ("t.co", 22), ("notforkmesh.com", 7)]
    assert [row["url"] for row in sites["board"]] == [
        "https://wellfound.com/company/forkmesh", "https://google.com/", "",
        "https://notforkmesh.com/"]
    markdown = call(runtime.use("GET"), "/api/marketing/acme/report")["data"]["markdown"]
    assert "## Referring websites (all time)" in markdown
    assert "545 visits from 4 other websites to forkmesh.com" in markdown
    assert "| wellfound.com | 291 | 2025-08-11 |" in markdown

    call(runtime.use("PUT", body={"analytics": {"host": "app.forkmesh.com"}}),
         "/api/marketing/acme/settings")
    assert call(runtime.use("GET"), "/api/marketing/acme")["data"][
        "referringSites"]["status"] == "ok"
    call(runtime.use("PUT", body={"analytics": {"host": "www.acme.com"}}),
         "/api/marketing/acme/settings")
    other = call(runtime.use("GET"), "/api/marketing/acme")["data"]["referringSites"]
    assert (other["status"], other["siteHost"], other["board"]) == ("other_site", "acme.com", [])
    assert "Referring websites" not in call(
        runtime.use("GET"), "/api/marketing/acme/report")["data"]["markdown"]


def test_referrers_leave_out_the_instance_and_its_zone():
    runtime = _referrer_runtime("https://world.forkmesh.com")
    sites = call(runtime.use("GET"), "/api/marketing/acme")["data"]["referringSites"]
    assert "forkmesh.com" not in [row["host"] for row in sites["board"]]
    assert "app.forkmesh.com" not in [row["host"] for row in sites["board"]]
    assert (sites["sites"], sites["visits"]) == (4, 545)


def test_referrers_degrade_without_the_core_table_or_origin():
    assert call(Runtime().use("GET"), "/api/marketing/acme")["data"][
        "referringSites"]["status"] == "unavailable"
    runtime = Runtime()
    runtime.public_origin = lambda: "https://forkmesh.com"
    sites = call(runtime.use("GET"), "/api/marketing/acme")["data"]["referringSites"]
    assert (sites["status"], sites["board"]) == ("unavailable", [])


def test_referrer_panel_contracts():
    view = (PACKAGE / "content" / "web" / "view.html").read_text()
    page = (PACKAGE / "content" / "web" / "page.js").read_text()
    native = (PACKAGE / "content" / "desktop" / "MarketingFeature.cpp").read_text()
    for hook in ("data-marketing-referrers-panel", "data-marketing-referrers-hint",
                 "data-marketing-referrers"):
        assert hook in view and hook in page, hook
    assert "referringSites: payload.referringSites" in page and "renderReferrers();" in page
    assert 'payload.value(QStringLiteral("referringSites"))' in native
    assert "renderReferrers();" in native and '"other_site"' in native


def test_overview_panels_are_movable_and_referrers_sit_high():
    view = (PACKAGE / "content" / "web" / "view.html").read_text()
    page = (PACKAGE / "content" / "web" / "page.js").read_text()
    native = (PACKAGE / "content" / "desktop" / "MarketingFeature.cpp").read_text()
    dashboard = (PACKAGE / "content" / "desktop" / "MarketingDashboard.cpp").read_text()
    grid = view[view.index("data-marketing-panels"):view.index("data-marketing-performance-page")]
    keys = re.findall(r'data-marketing-panel="([a-z]+)"', grid)
    assert len(keys) == len(set(keys)) == 13
    assert keys.index("referrers") < keys.index("campaigns") < keys.index("landing")
    assert "setupMovablePanels();" in page and "forkmesh.marketing.overviewPanels" in page
    assert native.index('DashboardPanel(QStringLiteral("Referring websites"))') < native.index(
        'DashboardPanel(QStringLiteral("Campaigns"))')
    assert 'grid->setMovable(QStringLiteral("market/packages/marketing/data/overviewPanels"))' in native
    assert "landingColumn" not in native and "mk-stack" not in view
    assert 'marketingDropTarget=\\"true\\"' in dashboard


def test_performance_page_contracts():
    view = (PACKAGE / "content" / "web" / "view.html").read_text()
    page = (PACKAGE / "content" / "web" / "page.js").read_text()
    for hook in ('data-marketing-page="performance"', "data-marketing-performance-page",
                 "data-marketing-performance-insights", "data-marketing-goals-dialog",
                 "data-marketing-funnel-analytics", 'href="/dashboard/analytics"'):
        assert hook in view, hook
    assert 'orgPath("/performance")' in page and "renderPerformance()" in page
    assert '{ analytics }' in page
    for level in marketing.INSIGHT_LEVELS:
        assert level in page, level
    for level in ("setup", "action", "warning", "win"):
        assert '[data-level="%s"]' % level in view, level
    manifest = json.loads((PACKAGE / "manifest.json").read_text())
    assert "organization.analytics.read" in {item["id"] for item in manifest["permissions"]}


def test_start_instance_template_links_the_start_page_post():
    runtime = Runtime()
    templates = call(runtime.use("GET"), "/api/marketing/acme")["data"]["campaignTemplates"]
    assert [item["slug"] for item in templates] == ["revenue_contest", "start_instance"]
    template = templates[1]
    post = "https://forkmesh.com/blog/start-your-own-forkmesh/"
    assert template["source"] == post and template["landingUrl"] == post
    assert template["objective"] in marketing.CAMPAIGN_OBJECTIVES
    created = call(runtime.use("POST", "charlie", template), "/api/marketing/acme/campaigns")
    assert created["status"] == 201
    campaign = created["data"]["campaign"]
    assert (campaign["name"], campaign["status"], campaign["slug"]) == (
        "Start your own ForkMesh", "planned", "start_instance")
    assert campaign["channels"] == ["blog", "mastodon", "x", "discord"]
    assert campaign["targetVisits"] is None and len(campaign["tasks"]) == 6
    assert campaign["stats"]["taggedLink"].startswith(post + "?utm_source=forkmesh")
    assert "utm_campaign=start_instance" in campaign["stats"]["taggedLink"]
    page = (PACKAGE / "content" / "web" / "page.js").read_text()
    assert ".map((item, index) => ({ item, index })).filter(({ item }) => !sources.has(item.source))" in page
    assert "templates.map(({ item, index }) =>" in page


def test_campaigns_are_sealed_join_content_and_ads_and_drive_suggestions():
    runtime = Runtime()
    template = call(runtime.use("GET"), "/api/marketing/acme")["data"]["campaignTemplates"][0]
    assert template["landingUrl"] == "https://forkmesh.com/hackathon"
    assert template["startsAt"] == 1789430400000 and template["slug"] == "revenue_contest"
    created = call(runtime.use("POST", "charlie", {**template, "targetVisits": 500}),
                   "/api/marketing/acme/campaigns")
    assert created["status"] == 201
    campaign = created["data"]["campaign"]
    assert campaign["name"] == "September Revenue Contest" and campaign["status"] == "active"
    assert campaign["channels"] == ["x", "reddit", "blog", "discord", "linkedin"]
    assert campaign["targetVisits"] == 500 and campaign["targetConversions"] is None
    assert campaign["stats"]["taggedLink"].startswith(
        "https://forkmesh.com/hackathon?utm_source=forkmesh&utm_medium=social"
        "&utm_campaign=revenue_contest")
    stored = runtime.db.execute("SELECT data FROM marketing_campaigns").fetchone()[0]
    assert stored.startswith("sealed:")
    call(runtime.use("POST", body={"title": "Leaderboard", "channel": "x", "status": "scheduled",
                                   "campaign": "September Revenue Contest",
                                   "scheduledAt": runtime.clock + marketing.DAY_MS}),
         "/api/marketing/acme/content")
    call(runtime.use("POST", body={"name": "Promo", "channel": "reddit", "status": "active",
                                   "spendCents": 900, "destinationUrl":
                                   "https://forkmesh.com/hackathon?utm_campaign=revenue_contest"}),
         "/api/marketing/acme/ads")
    overview = call(runtime.use("GET"), "/api/marketing/acme")["data"]
    stats = overview["campaigns"][0]["stats"]
    assert (stats["content"], stats["ads"], stats["nextTitle"]) == (1, 1, "Leaderboard")
    assert stats["spend"] == [{"currency": "USD", "cents": 900}]
    assert stats["uncoveredChannels"] == ["reddit", "blog", "discord", "linkedin"]
    assert (stats["tasksDone"], stats["tasksTotal"]) == (2, 8)
    assert stats["posts"] == 0 and stats["postChannels"] == []
    codes = {item["code"] for item in overview["suggestions"]}
    assert {"campaign_channel_uncovered", "campaign_ended", "no_keywords"} <= codes
    assert "no_campaign" not in codes
    item = campaign["id"]
    tasks = [dict(task, done=True) for task in campaign["tasks"]]
    patched = call(runtime.use("PATCH", body={"tasks": tasks, "status": "completed"}),
                   f"/api/marketing/acme/campaigns/{item}")["data"]["campaign"]
    assert patched["stats"]["tasksDone"] == 8 and patched["status"] == "completed"
    assert patched["name"] == "September Revenue Contest"
    bad = call(runtime.use("POST", body={"name": "X", "startsAt": 5, "endsAt": 1}),
               "/api/marketing/acme/campaigns")
    assert bad["data"]["error"] == "invalid_campaign_dates"
    assert call(runtime.use("POST", body={}), "/api/marketing/acme/campaigns")["data"][
        "error"] == "campaign_name_required"
    assert call(runtime.use("DELETE"), f"/api/marketing/acme/campaigns/{item}")["data"]["deleted"]
    empty = call(runtime.use("GET"), "/api/marketing/acme")["data"]
    assert empty["campaigns"] == []
    assert "no_campaign" in {entry["code"] for entry in empty["suggestions"]}


def test_contest_template_is_active_and_its_posted_links_join_it():
    runtime = Runtime()
    template = call(runtime.use("GET"), "/api/marketing/acme")["data"]["campaignTemplates"][0]
    assert template["status"] == "active" and template["landingUrl"] == "https://forkmesh.com/hackathon"
    assert "$5 / $10 / $50 / $100" in template["offer"] and "$25 plans" not in template["offer"]
    done = {task["title"] for task in template["tasks"] if task["done"]}
    assert done == {"Announce the contest on X and r/forkmesh", "Announce the contest on Discord"}
    campaign = call(runtime.use("POST", "charlie", template),
                    "/api/marketing/acme/campaigns")["data"]["campaign"]
    path = "/api/marketing/acme/posts"
    named = call(runtime.use("POST", body={
        "url": "https://x.com/forkmesh/status/1900000000000000001", "views": 300, "likes": 9,
        "campaign": "September Revenue Contest"}), path)["data"]["post"]
    assert named["campaign"] == "September Revenue Contest"
    call(runtime.use("POST", body={
        "url": "https://discord.com/channels/1/2/3", "campaign": "revenue_contest",
        "replies": 4}), path)
    call(runtime.use("POST", body={
        "url": "https://forkmesh.com/hackathon/", "channel": "blog"}), path)
    call(runtime.use("POST", body={
        "url": "https://forkmesh.com/blog/x/?utm_campaign=revenue_contest", "channel": "blog"}),
        path)
    call(runtime.use("POST", body={"url": "https://x.com/forkmesh/status/1900000000000000002"}),
         path)
    stats = call(runtime.use("GET"), f"/api/marketing/acme/campaigns/{campaign['id']}")[
        "data"]["campaign"]["stats"]
    assert (stats["posts"], stats["postViews"], stats["postEngagements"]) == (4, 300, 13)
    assert stats["postChannels"] == ["blog", "discord", "x"]
    assert stats["uncoveredChannels"] == ["reddit", "linkedin"]
    read = call(runtime.use("POST", body={"posts": [{
        "url": "https://x.com/forkmesh/status/1900000000000000001", "views": 500}]}), path)
    assert read["data"]["updated"] == 1
    posts = call(runtime.use("GET"), path)["data"]["posts"]
    kept = next(post for post in posts if post["id"] == named["id"])
    assert (kept["campaign"], kept["views"]) == ("September Revenue Contest", 500)
    unlinked = call(runtime.use("PATCH", body={"campaign": ""}), f"{path}/{named['id']}")
    assert unlinked["data"]["post"]["campaign"] == ""
    overview = call(runtime.use("GET"), "/api/marketing/acme")["data"]
    assert overview["campaigns"][0]["stats"]["posts"] == 3
    desktop = (PACKAGE / "content" / "desktop" / "MarketingPages.cpp").read_text()
    assert '{QStringLiteral("campaign"), campaignBox->currentData().toString()}' in desktop


def test_keywords_track_readings_import_search_console_rows_and_stay_unique():
    runtime = Runtime()
    created = call(runtime.use("POST", "charlie", {
        "term": "  Git   Mirror ", "targetUrl": "forkmesh.com/mirror", "priority": 2,
        "snapshot": {"position": "8.4", "impressions": 300, "clicks": 9,
                     "at": runtime.clock - 7 * marketing.DAY_MS}}),
        "/api/marketing/acme/keywords")
    assert created["status"] == 201
    keyword = created["data"]["keyword"]
    assert (keyword["term"], keyword["targetUrl"], keyword["position"]) == (
        "git mirror", "https://forkmesh.com/mirror", 8.4)
    duplicate = call(runtime.use("POST", body={"term": "GIT MIRROR"}),
                     "/api/marketing/acme/keywords")
    assert (duplicate["status"], duplicate["data"]["id"]) == (409, keyword["id"])
    imported = call(runtime.use("POST", body={"source": "Search Console", "rows": [
        {"term": "git mirror", "clicks": 20, "impressions": 500, "position": 5.2},
        {"term": "self hosted git", "clicks": 3, "impressions": 900, "position": 14},
        {"term": ""}]}), "/api/marketing/acme/keywords")["data"]
    assert (imported["created"], imported["updated"], imported["skipped"]) == (1, 1, 1)
    mirror = next(item for item in imported["keywords"] if item["term"] == "git mirror")
    assert (mirror["position"], mirror["positionChange"], mirror["ctr"]) == (5.2, 3.2, 0.04)
    assert len(mirror["history"]) == 2 and mirror["source"] == "Search Console"
    assert imported["keywords"][0]["term"] == "git mirror"
    stored = runtime.db.execute("SELECT data FROM marketing_keywords").fetchall()
    assert all(row[0].startswith("sealed:") for row in stored)
    assert "git mirror" not in json.dumps([row[0] for row in runtime.db.execute(
        "SELECT term_bi FROM marketing_keywords").fetchall()])
    item = mirror["id"]
    assert call(runtime.use("DELETE"), f"/api/marketing/acme/keywords/{item}")["data"]["deleted"]
    exported = asyncio.run(marketing.export_org_data(runtime, "org:acme", "acme"))
    assert [entry["term"] for entry in exported["keywords"]] == ["self hosted git"]


def test_x_oauth_signature_matches_the_documented_example():
    header = marketing._oauth1_header(
        "POST", "https://api.twitter.com/1.1/statuses/update.json?include_entities=true"
        "&status=Hello%20Ladies%20%2B%20Gentlemen%2C%20a%20signed%20OAuth%20request%21",
        {"apiKey": "xvz1evFS4wEEPTGEFPHBog",
         "apiSecret": "kAcSOqF21Fu85e7zjz7ZN2U4ZRhfV3WpwPAoE3Z7kBw",
         "accessToken": "370773112-GmHxMAgYyLbNEtIKZeRNFsMKPR9EyMZeS9weJAEb",
         "accessSecret": "LswwdoUaIvS8ltyTt5jkRh4J50vUPVVHtR2YPi5kE"},
        "kYjzVBB8Y0ZFabxSWbWovY3uYSQ2pTgmZeNu2VS4cg", 1318622958)
    assert 'oauth_signature="hCtSmYh%2BiHYCEqBWrE7C7hYmtUk%3D"' in header


X_KEYS = {"apiKey": "key-1", "apiSecret": "secret-2", "accessToken": "token-3",
          "accessSecret": "secret-4"}


def test_connect_x_offers_the_browser_extension_guide_on_web_and_desktop():
    web = (PACKAGE / "content" / "web" / "page.js").read_text()
    view = (PACKAGE / "content" / "web" / "view.html").read_text()
    pages = (PACKAGE / "content" / "desktop" / "MarketingPages.cpp").read_text()
    assert "data-marketing-x-guide" in view and "data-marketing-x-guide-status" in view
    assert 'type: "marketing-x-setup", request, organization: state.marketing.organization' in web
    assert 'data.type === "marketing-x-setup-update"' in web
    assert 'params.get("xsetup") === "1"' in web and 'params.delete("xsetup")' in web
    assert 'QStringLiteral("Guide me in the browser")' in pages
    assert 'openWebDashboard(QStringLiteral("xsetup=1"))' in pages
    assert "qint64(number(x, \"connectedAt\")) <= m_xGuideSince" in pages


def test_x_connection_is_admin_only_write_only_and_posts_mark_content_published():
    runtime = Runtime()
    denied = call(runtime.use("POST", "charlie", X_KEYS), "/api/marketing/acme/x-connection")
    assert denied["status"] == 403
    runtime.outbound["https://api.x.com/2/users/me"] = (401, json.dumps({"title": "Unauthorized"}))
    refused = call(runtime.use("POST", body=X_KEYS), "/api/marketing/acme/x-connection")
    assert (refused["status"], refused["data"]["error"]) == (502, "x_not_authorized")
    runtime.outbound["https://api.x.com/2/users/me"] = json.dumps(
        {"data": {"id": "9", "username": "forkmesh", "name": "ForkMesh"}})
    connected = call(runtime.use("POST", body=X_KEYS), "/api/marketing/acme/x-connection")
    assert connected["data"]["x"]["connected"] is True
    assert connected["data"]["x"]["username"] == "forkmesh"
    assert runtime.outbound_calls[-1][2]["authorization"].startswith('OAuth oauth_consumer_key="key-1"')
    overview = call(runtime.use("GET"), "/api/marketing/acme")
    assert overview["data"]["settings"]["x"]["connected"] is True
    for secret in X_KEYS.values():
        assert secret not in json.dumps(overview)
    call(runtime.use("PUT", body={"channels": overview["data"]["settings"]["channels"],
                                  "x": {}}), "/api/marketing/acme/settings")
    assert call(runtime.use("GET"), "/api/marketing/acme/x-connection")["data"]["x"]["connected"]
    item = call(runtime.use("POST", body={"title": "Ship", "channel": "x", "status": "queued",
                                          "body": "We shipped"}),
                "/api/marketing/acme/content")["data"]["content"]["id"]
    assert call(runtime.use("POST", "charlie", {"text": "hi"}),
                "/api/marketing/acme/x-post")["status"] == 403
    runtime.outbound["https://api.x.com/2/tweets"] = (201, json.dumps({"data": {"id": "777"}}))
    posted = call(runtime.use("POST", body={"text": "We shipped", "contentId": item}),
                  "/api/marketing/acme/x-post")
    assert posted["status"] == 201 and posted["data"]["url"] == "https://x.com/forkmesh/status/777"
    resource, method, headers, body = runtime.outbound_calls[-1]
    assert (method, json.loads(body)) == ("POST", {"text": "We shipped"})
    assert headers["content-type"] == "application/json"
    content = posted["data"]["content"]
    assert (content["status"], content["url"]) == ("published", "https://x.com/forkmesh/status/777")
    assert call(runtime.use("POST", body={"text": " "}),
                "/api/marketing/acme/x-post")["data"]["error"] == "x_post_text_required"
    exported = asyncio.run(marketing.export_org_data(runtime, "org:acme", "acme"))
    assert "secret-2" not in json.dumps(exported)
    gone = call(runtime.use("DELETE"), "/api/marketing/acme/x-connection")
    assert gone["data"]["x"]["connected"] is False
    assert call(runtime.use("POST", body={"text": "hi"}),
                "/api/marketing/acme/x-post")["data"]["error"] == "x_not_connected"


def test_assistant_asks_the_marketing_agent_and_cleans_its_suggestions():
    runtime = Runtime()
    assert call(runtime.use("POST", body={}), "/api/marketing/acme/assistant")["status"] == 503
    prompts = []

    async def ai_json(account_bi, system_prompt, user_prompt, schema, max_tokens=700):
        prompts.append((account_bi, system_prompt, user_prompt, schema))
        return {"ok": True, "model": "test", "data": {"summary": "Post the leaderboard.", "suggestions": [
            {"kind": "content", "title": "Leaderboard", "channel": "nope", "copy": "Top 3!", "inDays": 2.7},
            {"kind": "task", "title": "Email entrants"}, {"kind": "spam", "title": "Buy followers"},
            {"kind": "keyword", "title": ""}]}}
    runtime.ai_json = ai_json
    campaign = call(runtime.use("POST", body={"name": "Contest", "channels": ["x"]}),
                    "/api/marketing/acme/campaigns")["data"]["campaign"]
    assert call(runtime.use("GET"), "/api/marketing/acme")["data"]["assistantAvailable"] is True
    answer = call(runtime.use("POST", "charlie", {"campaignId": campaign["id"],
                                                  "question": "What next?"}),
                  "/api/marketing/acme/assistant")["data"]
    assert answer["summary"] == "Post the leaderboard."
    assert answer["suggestions"] == [
        {"kind": "content", "title": "Leaderboard", "detail": "", "channel": "x",
         "copy": "Top 3!", "inDays": 2},
        {"kind": "task", "title": "Email entrants", "detail": "", "channel": "", "copy": "",
         "inDays": 0}]
    account, system, prompt, schema = prompts[0]
    assert account == "bi:charlie" and "Marketing special agent" in system
    assert '"name": "Contest"' in prompt and "<question>What next?</question>" in prompt
    missing = call(runtime.use("POST", body={"campaignId": "f" * 32}),
                   "/api/marketing/acme/assistant")
    assert missing["status"] == 404


def test_traffic_channels_group_sources_and_analytics_reports_search_terms():
    runtime, analytics = _analytics_runtime()
    runtime.db.execute(
        "INSERT INTO analytics_projects VALUES(?,?,?,?,?,?,?,?,?)",
        ("a" * 32, "org:acme", "acme", "fma_" + "0" * 40, 1, "alice", 1, 1,
         'sealed:{"name": "Site"}'))
    paid = {"utm_source": "google", "utm_medium": "cpc", "utm_campaign": "brand",
            "utm_term": "Git  Mirror"}
    _seed_visits(runtime, analytics,
                 [(paid, ["/"])] * 3 + [({"$referrer": "https://www.google.com/"}, ["/"])] * 2
                 + [({"utm_source": "reddit", "utm_medium": "social"}, ["/", "/docs"])] * 4
                 + [({}, ["/"])])
    perf = call(runtime.use("GET", params={"from": runtime.clock - 7 * marketing.DAY_MS}),
                "/api/marketing/acme/performance")["data"]["performance"]
    channels = {row["channel"]: row for row in perf["channels"]}
    assert {key: row["sessions"] for key, row in channels.items()} == {
        "social": 4, "paid": 3, "organic": 2, "direct": 1}
    assert channels["social"]["pagesPerSession"] == 2.0
    assert channels["paid"]["topSources"] == [{"value": "google", "count": 3}]
    terms = {row["term"]: row["sessions"] for row in perf["keywords"]}
    assert terms == {"git mirror": 3, "(not provided)": 2}


def test_dashboard_command_center_is_wired_on_the_web_surface():
    web = (PACKAGE / "content" / "web" / "page.js").read_text()
    view = (PACKAGE / "content" / "web" / "view.html").read_text()
    for hook in ("data-marketing-traffic", "data-marketing-keywords", "data-marketing-channel-grid",
                 "data-marketing-campaigns", "data-marketing-campaign-dialog",
                 "data-marketing-suggestions", "data-marketing-assistant-form",
                 "data-marketing-ads-overview", "data-marketing-x-form", "data-marketing-x-dialog",
                 "data-marketing-keyword-import-dialog", "data-marketing-landing"):
        assert hook in view, hook
    for hook in ('orgPath("/x-post")', 'orgPath("/x-connection")', 'orgPath("/assistant")',
                 'orgPath(`/campaigns${id ? `/${id}` : ""}`)', 'orgPath("/keywords")',
                 '"/api/tasks"', "applyOverview(payload)", "loadTraffic()"):
        assert hook in web, hook
    for status in marketing.CAMPAIGN_STATUSES:
        assert f'["{status}",' in web
    for objective in marketing.CAMPAIGN_OBJECTIVES:
        assert f'["{objective}",' in web


def test_google_channel_counts_google_visits_and_pages_on_google():
    runtime, analytics = _analytics_runtime()
    runtime.db.execute(
        "INSERT INTO analytics_projects VALUES(?,?,?,?,?,?,?,?,?)",
        ("a" * 32, "org:acme", "acme", "fma_" + "0" * 40, 1, "alice", 1, 1,
         'sealed:{"name": "Site"}'))
    _seed_visits(runtime, analytics,
                 [({"$referrer": "https://www.google.com/"}, ["/"])] * 2
                 + [({"$referrer": "https://www.google.co.uk/search"}, ["/"])]
                 + [({"utm_source": "google", "utm_medium": "cpc"}, ["/"])] * 3
                 + [({"$referrer": "https://mail.google.com/"}, ["/"])]
                 + [({"utm_source": "reddit", "utm_medium": "social"}, ["/"])])
    catalog = {item[0]: item[1] for item in marketing.CHANNEL_CATALOG}
    assert catalog["google-search"] == "Google"
    saved = call(runtime.use("PUT", body={
        "analytics": {"host": "www.acme.com"},
        "channels": [{"id": "google-search", "profileUrl": ""}]}),
        "/api/marketing/acme/settings")["data"]["settings"]["channels"]
    google = next(item for item in saved if item["id"] == "google-search")
    assert google["profileUrl"] == "https://www.google.com/search?q=site:acme.com"
    assert "google-search" in call(runtime.use("GET"), "/api/marketing/acme")["data"][
        "refreshableChannels"]

    refreshed = call(runtime.use("POST", body={"channel": "google-search"}),
                     "/api/marketing/acme/refresh")
    assert refreshed["status"] == 200, refreshed
    assert refreshed["data"]["refresh"]["metrics"]["visits"] == 6
    assert refreshed["data"]["refresh"]["feed"]["title"] == "site:acme.com"

    captured = call(runtime.use("POST", "charlie", {
        "channel": "google-search", "url": google["profileUrl"], "title": "site:acme.com",
        "metrics": {"indexedPages": 1230}}), "/api/marketing/acme/capture")
    assert captured["status"] == 201, captured
    assert (captured["data"]["metrics"]["indexedPages"],
            captured["data"]["metrics"]["visits"]) == (1230, 6)
    row = next(item for item in call(runtime.use("GET"), "/api/marketing/acme")["data"][
        "summary"]["channels"] if item["channel"] == "google-search")
    assert (row["visits"], row["indexedPages"]) == (6, 1230)
    assert "posting" not in {check["id"] for check in row["health"]["checks"]}
    fresh = Runtime()
    assert "google-search" in [item["channel"] for item in call(
        fresh.use("GET"), "/api/marketing/acme")["data"]["summary"]["channels"]]

    again = call(runtime.use("POST", body={"channel": "google-search"}),
                 "/api/marketing/acme/refresh")["data"]["refresh"]["metrics"]
    assert (again["visits"], again["indexedPages"]) == (6, 1230)
    elsewhere = call(runtime.use("POST", "charlie", {
        "channel": "google-search", "url": "https://www.bing.com/search?q=site:acme.com",
        "metrics": {"indexedPages": 5}}), "/api/marketing/acme/capture")
    assert elsewhere["data"]["error"] == "capture_host_mismatch"


def test_google_channel_link_falls_back_to_the_instance_host():
    runtime = Runtime()
    runtime.public_origin = lambda: "https://www.forkmesh.com"
    link = lambda: next(item for item in call(runtime.use("GET"), "/api/marketing/acme/settings")[
        "data"]["settings"]["channels"] if item["id"] == "google-search")["profileUrl"]
    assert link() == "https://www.google.com/search?q=site:forkmesh.com"
    row = lambda: next(item for item in call(runtime.use("GET"), "/api/marketing/acme")["data"][
        "summary"]["channels"] if item["channel"] == "google-search")
    assert {check["id"]: check["ok"] for check in row()["health"]["checks"]}["link"]
    failed = call(runtime.use("POST", body={"channel": "google-search"}),
                  "/api/marketing/acme/refresh")
    assert failed["data"]["error"] == "analytics_not_installed"
    assert next(item for item in call(Runtime().use("GET"), "/api/marketing/acme/settings")[
        "data"]["settings"]["channels"] if item["id"] == "google-search")["profileUrl"] == ""


def test_google_refresh_explains_missing_analytics():
    runtime = Runtime()
    call(runtime.use("PUT", body={"channels": [
        {"id": "google-search", "profileUrl": "https://www.google.com/search?q=site:acme.com"}]}),
        "/api/marketing/acme/settings")
    failed = call(runtime.use("POST", body={"channel": "google-search"}),
                  "/api/marketing/acme/refresh")
    assert failed["status"] == 502 and failed["data"]["error"] == "analytics_not_installed"


def test_the_reddit_channel_guides_people_to_its_numbers_through_the_extension():
    runtime = Runtime()
    guide = lambda: {step["id"]: step for step in next(
        item for item in call(runtime.use("GET"), "/api/marketing/acme")["data"]["summary"][
            "channels"] if item["channel"] == "reddit")["health"]["guide"]}
    assert [step["done"] for step in guide().values()] == [False, False, False]
    assert {step["action"] for step in guide().values()} == {"link", "extension", "capture"}
    call(runtime.use("PUT", body={"channels": [
        {"id": "reddit", "profileUrl": "https://www.reddit.com/r/acme/"}]}),
        "/api/marketing/acme/settings")
    steps = guide()
    assert steps["link"]["done"] and not steps["update"]["done"]
    assert "r/acme" in steps["update"]["label"]
    call(runtime.use("POST", body={"channel": "reddit", "followers": 10}),
         "/api/marketing/acme/metrics")
    assert not guide()["extension"]["done"]
    captured = call(runtime.use("POST", "charlie", {
        "channel": "reddit", "url": "https://www.reddit.com/r/acme/", "handle": "r/acme",
        "metrics": {"followers": 5321, "reach": 1840, "engagements": 212}}),
        "/api/marketing/acme/capture")
    assert captured["status"] == 201, captured
    metrics = captured["data"]["metrics"]
    assert (metrics["followers"], metrics["reach"], metrics["engagements"]) == (5321, 1840, 212)
    assert all(step["done"] for step in guide().values())
    row = next(item for item in call(runtime.use("GET"), "/api/marketing/acme")["data"][
        "summary"]["channels"] if item["channel"] == "reddit")
    assert row["health"]["engagementRate"] == 11.52
    other = next(item for item in call(runtime.use("GET"), "/api/marketing/acme")["data"][
        "summary"]["channels"] if item["channel"] == "x")
    assert other["health"]["guide"] == []


def _crm_runtime(zoho=True):
    runtime = Runtime()
    crm = marketing._crm_module()
    runtime.db.executescript(";\n".join(crm.DATA_SCHEMA_UP))
    runtime.db.execute("INSERT INTO org_plugin_installs VALUES(?,?,?,?,?,?)",
                       ("org:acme", "crm", "acme", "bi:alice", "alice", 1))
    settings = {"zoho": {"accountId": "acct", "refreshToken": "token", "managed": True,
                         "region": "eu", "lastSyncAt": 5}} if zoho else {}
    runtime.db.execute("INSERT INTO crm_settings VALUES(?,?,?,?,?)",
                       ("org:acme", "bi:alice", "alice", 1, "sealed:" + json.dumps(settings)))
    now = runtime.clock
    old = now - 40 * 86_400_000
    for index, (status, sent_at, external_id, data) in enumerate((
            ("external", now - 1000, "zoho-1", {"direction": "outbound"}),
            ("delivered", now - 2000, "", {}),
            ("external", now - 3000, "", {"direction": "inbound"}),
            ("draft", now - 4000, "", {}),
            ("failed", now - 5000, "", {}),
            ("external", old, "", {"direction": "outbound"}))):
        runtime.db.execute(
            "INSERT INTO crm_email_sends(send_id,org_bi,org_name,contact_id,sender_bi,"
            "sender_name,transport,status,external_id,created_at,sent_at,status_at,data) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
            (f"{index:032x}", "org:acme", "acme", None, "bi:alice", "alice", "external",
             status, external_id, sent_at, sent_at, sent_at, "sealed:" + json.dumps(data)))
    for ts, ok in ((now - 100, 1), (now - 200, 0), (old, 1)):
        runtime.db.execute(
            "INSERT INTO crm_outreach_log(org_bi,ts,sender_bi,sender_name,ok,data) "
            "VALUES(?,?,?,?,?,?)", ("org:acme", ts, "bi:alice", "alice", ok, "sealed:{}"))
    for index, (provider_id, received_at, data) in enumerate((
            ("zoho-1", now - 1000, {"sent": True, "sourceFolder": "Sent"}),
            ("zoho-2", now - 1500, {"sent": True, "sourceFolder": "Sent"}),
            ("zoho-3", now - 1600, {"sourceFolder": "Sent Items"}),
            ("zoho-4", now - 1700, {"sourceFolder": "Inbox"}),
            ("zoho-5", old, {"sent": True, "sourceFolder": "Sent"}))):
        runtime.db.execute(
            "INSERT INTO crm_mail_messages VALUES(?,?,?,?,?,?,?,?,?,?,?)",
            (f"{index:032x}", "org:acme", "acme", "zoho", "acct", provider_id, "Inbox",
             "g", received_at, now, "sealed:" + json.dumps(data)))
    for index, (stage, value) in enumerate((
            ("customer", 1200), ("won", 800), ("committed", 0), ("lead", 5000),
            ("hired", 0), ("closed", 300))):
        runtime.db.execute(
            "INSERT INTO crm_contacts VALUES(?,?,?,?,?,?,?,?,?,?,?)",
            (f"{index:032x}", "org:acme", "acme", "", "bi:alice", "alice", "bi:alice",
             "alice", 1, 1, "sealed:" + json.dumps({"stage": stage, "value": value})))
    runtime.db.commit()
    return runtime


def test_email_channel_counts_outbound_email_and_closed_deals_from_crm_and_zoho():
    runtime = _crm_runtime()
    catalog = {item[0]: item[1] for item in marketing.CHANNEL_CATALOG}
    assert catalog["email"] == "Email"
    overview = call(runtime.use("GET"), "/api/marketing/acme")["data"]
    assert "email" in overview["refreshableChannels"]
    email = next(item for item in overview["summary"]["channels"] if item["channel"] == "email")
    assert email["health"]["state"] == "setup"
    assert [step["done"] for step in email["health"]["guide"]] == [False] * 4

    refreshed = call(runtime.use("POST", "charlie", {"channel": "email"}),
                     "/api/marketing/acme/refresh")
    assert refreshed["status"] == 200, refreshed
    metrics = refreshed["data"]["refresh"]["metrics"]
    assert (metrics["outbound"], metrics["replies"], metrics["closedDeals"],
            metrics["closedValue"]) == (5, 1, 3, 2000)
    assert refreshed["data"]["refresh"]["feed"]["siteUrl"] == "https://mail.zoho.eu/zm/"
    assert refreshed["data"]["refresh"]["feed"]["title"] == "Zoho Mail"
    email = next(item for item in refreshed["data"]["summary"]["channels"]
                 if item["channel"] == "email")
    assert (email["outbound"], email["closedDeals"]) == (5, 3)
    checks = {check["id"]: check["ok"] for check in email["health"]["checks"]}
    assert checks == {"zoho": True, "fresh": True, "outbound": True}
    assert email["health"]["state"] == "good"
    assert all(step["done"] for step in email["health"]["guide"])


def test_email_channel_without_zoho_still_counts_crm_mail_and_asks_for_zoho():
    runtime = _crm_runtime(zoho=False)
    refreshed = call(runtime.use("POST", body={"channel": "email"}),
                     "/api/marketing/acme/refresh")
    assert refreshed["status"] == 200, refreshed
    assert refreshed["data"]["refresh"]["feed"]["title"] == "CRM email"
    email = next(item for item in refreshed["data"]["summary"]["channels"]
                 if item["channel"] == "email")
    assert email["outbound"] == 5
    guide = {step["id"]: step["done"] for step in email["health"]["guide"]}
    assert guide == {"crm": True, "zoho": False, "deals": True, "update": True}
    assert email["health"]["state"] == "attention"


def test_email_refresh_explains_missing_crm():
    runtime = Runtime()
    failed = call(runtime.use("POST", body={"channel": "email"}),
                  "/api/marketing/acme/refresh")
    assert failed["status"] == 502 and failed["data"]["error"] == "crm_not_installed"
    manifest = json.loads((PACKAGE / "manifest.json").read_text())
    assert "organization.crm.read" in {item["id"] for item in manifest["permissions"]}


def test_worker_moves_no_money_and_calls_no_ad_or_payment_api():
    tree = ast.parse((PACKAGE / "content" / "worker" / "marketing.py").read_text())
    callers = set()
    for function in ast.walk(tree):
        if isinstance(function, (ast.FunctionDef, ast.AsyncFunctionDef)):
            for node in ast.walk(function):
                if (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
                        and node.func.attr == "outbound_text"):
                    callers.add(function.name)
    assert callers == {"_read_feed", "_fetch_response", "_x_call", "_mastodon_follow"}
    api_hosts = {node.value.split("/")[2] for node in ast.walk(tree)
                 if isinstance(node, ast.Constant) and isinstance(node.value, str)
                 and node.value.startswith("https://api.")}
    assert api_hosts == {"api.github.com", "api.x.com"}
    source = (PACKAGE / "content" / "worker" / "marketing.py").read_text().lower()
    for marker in ("stripe", "paypal", "checkout", "/payments", "googleads.googleapis",
                   "graph.facebook.com", "adsapi", "ads-api"):
        assert marker not in source



def _suggestion_runtime(settings):
    runtime = Runtime()
    crm = marketing._crm_module()
    runtime.db.executescript(";\n".join(crm.DATA_SCHEMA_UP))
    runtime.db.execute("INSERT INTO org_plugin_installs VALUES(?,?,?,?,?,?)",
                       ("org:acme", "crm", "acme", "bi:alice", "alice", 1))
    runtime.db.execute("INSERT INTO crm_settings VALUES(?,?,?,?,?)",
                       ("org:acme", "bi:alice", "alice", 1, "sealed:" + json.dumps(settings)))
    now = runtime.clock
    day = 86_400_000
    contacts = (
        ("replied", {"name": "Rita Reply", "email": "rita@example.com", "stage": "customer",
                     "lastContactedAt": now - day}),
        ("answered", {"name": "Ann Answered", "email": "ann@example.com", "stage": "lead",
                      "lastContactedAt": now - day}),
        ("quiet", {"name": "Quinn Quiet", "email": "quinn@example.com", "stage": "lead",
                   "flow": "customers", "lastContactedAt": now - 20 * day}),
        ("stepped", {"name": "Sam Step", "email": "sam@example.com", "stage": "lead",
                     "lastContactedAt": now - 30 * day, "nextStep": "send the deck"}),
        ("recent", {"name": "Rob Recent", "email": "rob@example.com", "stage": "lead",
                    "lastContactedAt": now - 2 * day}),
        ("investor", {"name": "Ivy Investor", "email": "ivy@fund.example", "company": "Fund",
                      "flow": "investment", "stage": "sourced"}),
        ("lead", {"name": "Lee Lead", "email": "lee@example.com", "flow": "customers",
                  "stage": "lead"}),
        ("passed", {"name": "Pat Passed", "email": "pat@example.com", "flow": "investment",
                    "stage": "passed", "lastContactedAt": now - 40 * day}),
        ("won", {"name": "Wes Won", "email": "wes@example.com", "stage": "won",
                 "lastContactedAt": now - 40 * day}),
        ("nomail", {"name": "No Mail", "stage": "lead", "lastContactedAt": now - 40 * day}),
    )
    ids = {}
    for index, (key, data) in enumerate(contacts):
        ids[key] = f"{index:032x}"
        runtime.db.execute(
            "INSERT INTO crm_contacts VALUES(?,?,?,?,?,?,?,?,?,?,?)",
            (ids[key], "org:acme", "acme", "", "bi:alice", "alice", "bi:alice",
             "alice", 1, 1, "sealed:" + json.dumps(data)))
    for index, (key, at, data) in enumerate((
            ("replied", now - 2 * day, {"direction": "outbound"}),
            ("replied", now - day, {"direction": "inbound", "subject": "Pricing question"}),
            ("answered", now - 2 * day, {"direction": "inbound", "subject": "Hi"}),
            ("answered", now - day, {"direction": "outbound"}))):
        runtime.db.execute(
            "INSERT INTO crm_email_sends(send_id,org_bi,org_name,contact_id,sender_bi,"
            "sender_name,transport,status,external_id,created_at,sent_at,status_at,data) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
            (f"{index:032x}", "org:acme", "acme", ids[key], "bi:alice", "alice", "external",
             "external", "", at, at, at, "sealed:" + json.dumps(data)))
    runtime.db.commit()
    return runtime


def test_email_channel_suggests_emails_with_a_filled_zoho_compose_link():
    runtime = _suggestion_runtime({
        "fromName": "Jett", "fromEmail": "jett@acme.example",
        "zoho": {"accountId": "acct", "refreshToken": "token", "managed": True, "region": "eu"}})
    result = call(runtime.use("GET", "charlie"), "/api/marketing/acme/email-suggestions")
    assert result["status"] == 200, result
    email = result["data"]["email"]
    assert email["provider"] == "zoho" and email["zohoConnected"] is True
    suggestions = email["suggestions"]
    assert [item["email"] for item in suggestions] == [
        "rita@example.com", "sam@example.com", "quinn@example.com", "ivy@fund.example"]
    assert [item["kind"] for item in suggestions] == ["reply", "follow_up", "follow_up", "intro"]
    reply, stepped, quiet, intro = suggestions
    assert reply["subject"] == "Re: Pricing question"
    assert reply["body"].startswith("Hi Rita,") and "Jett" in reply["body"]
    assert "jett@acme.example" in reply["body"]
    assert stepped["reason"] == "No contact for 30 days · next step: send the deck"
    assert quiet["reason"] == "No contact for 20 days"
    assert intro["subject"] == templates_subject("investor")
    assert "Hi Ivy," in intro["body"] and "[name]" not in intro["body"]
    url = urlparse(reply["composeUrl"])
    assert (url.scheme, url.netloc, url.path) == ("https", "mail.zoho.eu", "/zm/comp.do")
    mailto = urlparse(parse_qs(url.query)["ct"][0])
    assert (mailto.scheme, mailto.path) == ("mailto", "rita@example.com")
    fields = parse_qs(mailto.query)
    assert fields["subject"] == ["Re: Pricing question"]
    assert fields["body"] == [reply["body"]]


def test_email_suggestions_follow_the_crm_mail_provider_and_need_crm():
    runtime = _suggestion_runtime({"mailProvider": "gmail"})
    email = call(runtime.use("GET"), "/api/marketing/acme/email-suggestions")["data"]["email"]
    assert email["provider"] == "gmail" and email["zohoConnected"] is False
    url = urlparse(email["suggestions"][0]["composeUrl"])
    query = parse_qs(url.query)
    assert url.netloc == "mail.google.com" and query["to"] == ["rita@example.com"]
    assert query["su"] == ["Re: Pricing question"]
    assert "[your name]" in query["body"][0]
    missing = call(Runtime().use("GET"), "/api/marketing/acme/email-suggestions")
    assert missing["status"] == 409 and missing["data"]["error"] == "crm_not_installed"
    posted = call(runtime.use("POST", body={}), "/api/marketing/acme/email-suggestions")
    assert posted["status"] == 405


def templates_subject(key):
    crm = marketing._crm_module()
    return next(item["subject"] for item in crm.OUTREACH_TEMPLATES if item["key"] == key)


def test_desktop_and_web_list_suggested_emails_with_one_click_compose():
    native = (PACKAGE / "content" / "desktop" / "MarketingFeature.cpp").read_text()
    header = (PACKAGE / "content" / "desktop" / "MarketingFeature.h").read_text()
    page = (PACKAGE / "content" / "web" / "page.js").read_text()
    assert 'detailBox(QStringLiteral("Suggested emails"), 3, &m_detailEmailsPanel)' in native
    assert "void loadEmailSuggestions();" in header
    load = native.split("void MarketingFeature::loadEmailSuggestions()", 1)[1].split("\nvoid MarketingFeature::", 1)[0]
    render = native.split("void MarketingFeature::renderEmailSuggestions()", 1)[1].split("\nvoid MarketingFeature::", 1)[0]
    opener = native.split("void MarketingFeature::openEmailSuggestion(", 1)[1].split("\nvoid MarketingFeature::", 1)[0]
    assert 'organizationPath(QStringLiteral("/email-suggestions"))' in load
    assert 'QStringLiteral("Open in %1 ↗").arg(mailbox)' in render
    assert "open->setEnabled(false);" in render and "ghostButton" not in render
    assert 'text(suggestion, "composeUrl")' in opener
    assert "QDesktopServices::openUrl(url);" in opener
    assert "m_emailSuggestionsOpened.insert(" in opener
    assert 'm_detailEmailsPanel->setVisible(email);' in native
    assert 'request(orgPath("/email-suggestions"))' in page
    assert 'href="${escapeHtml(item.composeUrl)}" target="_blank" rel="noopener noreferrer"' in page
    assert '${item.channel === "email" ? emailSuggestionsList() : ""}' in page


PNG_DATA_URL = "data:image/png;base64," + __import__("base64").b64encode(
    b"\x89PNG\r\n\x1a\n" + b"\x00" * 64).decode()


def test_ready_x_post_is_sealed_replaced_and_closed_with_history():
    runtime = Runtime()
    empty = call(runtime.use("GET", params={"channel": "x"}), "/api/marketing/acme/ready-post")
    assert empty["data"]["readyPost"] == {"channel": "x", "post": None, "needsPost": True,
                                          "history": []}
    assert call(runtime.use("GET", params={"channel": "blog"}),
                "/api/marketing/acme/ready-post")["data"]["error"] == "invalid_ready_post_channel"
    text = "Your repos, your mesh. Self-host git in one click https://forkmesh.com/?utm_source=x"
    saved = call(runtime.use("PUT", "charlie", {
        "channel": "x", "text": text, "imageDataUrl": PNG_DATA_URL, "imageAlt": "A mesh",
        "why": "Self-hosting posts drew the most replies", "model": "claude-opus-5-5",
    }), "/api/marketing/acme/ready-post")["data"]["readyPost"]
    post = saved["post"]
    assert saved["needsPost"] is False and post["imageDataUrl"] == PNG_DATA_URL
    assert (post["author"], post["model"], post["hasImage"]) == ("charlie", "claude-opus-5-5", True)
    assert post["length"] == len(text) - len("https://forkmesh.com/?utm_source=x") + 23
    assert post["intentUrl"].startswith("https://x.com/intent/post?text=Your%20repos")
    stored = runtime.db.execute("SELECT data FROM marketing_ready_posts").fetchone()[0]
    assert stored.startswith("sealed:") and "Self-host git" in stored
    overview = call(runtime.use("GET"), "/api/marketing/acme")["data"]["readyPosts"]["x"]
    assert overview["post"]["text"] == text and "imageDataUrl" not in overview["post"]
    second = call(runtime.use("PUT", body={"channel": "x", "text": "Second idea"}),
                  "/api/marketing/acme/ready-post")["data"]["readyPost"]
    assert second["history"][0]["status"] == "replaced" and second["post"]["hasImage"] is False
    stale = call(runtime.use("PATCH", body={"channel": "x", "id": post["id"],
                                            "status": "posted"}),
                 "/api/marketing/acme/ready-post")
    assert (stale["status"], stale["data"]["error"]) == (409, "ready_post_changed")
    closed = call(runtime.use("PATCH", body={
        "channel": "x", "id": second["post"]["id"], "status": "posted",
        "url": "https://twitter.com/forkmesh/status/55555?s=20"}),
        "/api/marketing/acme/ready-post")["data"]["readyPost"]
    assert closed["post"] is None and closed["needsPost"] is True
    assert closed["history"][0] == {**closed["history"][0], "status": "posted",
                                    "text": "Second idea", "closedBy": "alice",
                                    "url": "https://x.com/forkmesh/status/55555"}
    assert call(runtime.use("PATCH", body={"channel": "x", "status": "skipped"}),
                "/api/marketing/acme/ready-post")["status"] == 404
    exported = asyncio.run(marketing.export_org_data(runtime, "org:acme", "acme"))
    assert exported["readyPosts"]["x"]["history"][0]["text"] == "Second idea"
    asyncio.run(marketing.purge_org_data(runtime, "org:acme"))
    assert not runtime.db.execute("SELECT 1 FROM marketing_ready_posts").fetchone()


def test_ready_x_post_validates_its_text_image_and_roles():
    runtime = Runtime()
    runtime.db.execute("INSERT INTO org_members VALUES('org:acme','bi:bob','guest','bob',1)")
    put = lambda body, actor="alice": call(runtime.use("PUT", actor, {"channel": "x", **body}),
                                           "/api/marketing/acme/ready-post")
    assert put({"text": " "})["data"]["error"] == "ready_post_text_required"
    assert put({"text": "x" * (marketing.X_POST_LIMIT + 1)})["data"]["error"] == "x_post_too_long"
    assert put({"text": "hi", "imageDataUrl": "data:image/gif;base64,R0lGOD"})["data"][
        "error"] == "invalid_ready_post_image"
    assert put({"text": "hi", "imageDataUrl": "data:image/jpeg;base64,"
                + PNG_DATA_URL.split(",", 1)[1]})["data"]["error"] == "invalid_ready_post_image"
    assert put({"text": "hi", "imageDataUrl": "data:image/png;base64," + "A" * 960000})[
        "data"]["error"] == "ready_post_image_too_large"
    assert put({"text": "hi", "link": "javascript:alert(1)"})["data"][
        "error"] == "invalid_ready_post_link"
    assert put({"text": "hi"}, "bob")["status"] == 403
    assert call(runtime.use("GET", "bob"), "/api/marketing/acme/ready-post")["status"] == 403
    assert call(runtime.use("DELETE"), "/api/marketing/acme/ready-post")["status"] == 405


def test_ready_x_post_closes_itself_when_posted_through_x_or_the_extension():
    runtime = Runtime()
    runtime.outbound["https://api.x.com/2/users/me"] = json.dumps(
        {"data": {"id": "9", "username": "forkmesh", "name": "ForkMesh"}})
    call(runtime.use("POST", body=X_KEYS), "/api/marketing/acme/x-connection")
    text = "Every repo you fork stays yours, even offline. Here is how the mesh keeps them in sync."
    call(runtime.use("PUT", body={"channel": "x", "text": text}), "/api/marketing/acme/ready-post")
    runtime.outbound["https://api.x.com/2/tweets"] = (201, json.dumps({"data": {"id": "778"}}))
    posted = call(runtime.use("POST", body={"text": text}), "/api/marketing/acme/x-post")
    assert posted["data"]["readyPost"]["needsPost"] is True
    assert posted["data"]["readyPost"]["history"][0]["url"] == "https://x.com/forkmesh/status/778"
    call(runtime.use("PUT", body={"channel": "x", "text": text + " Try it today."}),
         "/api/marketing/acme/ready-post")
    unrelated = call(runtime.use("POST", body={"posts": [{
        "url": "https://x.com/forkmesh/status/900", "title": "Something else entirely"}]}),
        "/api/marketing/acme/posts")
    assert unrelated["data"]["readyPostClosed"] is False
    imported = call(runtime.use("POST", body={"posts": [{
        "url": "https://x.com/forkmesh/status/901",
        "title": "Every repo you fork stays yours, even offline. Here is how the mesh keeps…"}]}),
        "/api/marketing/acme/posts")
    assert imported["data"]["readyPostClosed"] is True
    state = call(runtime.use("GET"), "/api/marketing/acme/ready-post")["data"]["readyPost"]
    assert state["post"] is None
    assert state["history"][0]["url"] == "https://x.com/forkmesh/status/901"


def test_every_surface_shows_the_ready_x_post_with_a_one_tap_post():
    desktop = (PACKAGE / "content" / "desktop" / "MarketingFeature.cpp").read_text()
    web = (PACKAGE / "content" / "web" / "page.js").read_text()
    ios = (REPO / "native" / "ios" / "ForkMesh" / "Features" / "Packages"
           / "MarketingSections.swift").read_text()
    ios_core = (REPO / "native" / "ios" / "ForkMeshCore" / "Sources" / "FMPackages"
                / "PackagesClient+Marketing.swift").read_text()
    android = (REPO / "native" / "android" / "app" / "src" / "main" / "kotlin" / "com"
               / "forkmesh" / "nativeapp" / "feature" / "market" / "MarketingScreens.kt").read_text()
    android_core = (REPO / "native" / "android" / "core" / "src" / "main" / "kotlin" / "com"
                    / "forkmesh" / "nativeapp" / "market" / "MarketingClient.kt").read_text()
    for source in (desktop, web, ios_core, android_core):
        assert "ready-post" in source
    assert "setImage" in desktop and "intentUrl" in desktop and "Write one now" in desktop
    assert "navigator.share" in web and "Download image" in web
    assert "Post to X" in ios and "Ready for X" in ios
    assert "ACTION_SEND" in android and "com.twitter.android" in android


def test_a_rate_limited_reddit_refresh_is_named_falls_back_and_backs_off():
    runtime = Runtime()
    _configure(runtime, reddit="https://www.reddit.com/r/Forkmesh/")
    feed = """
      <feed xmlns="http://www.w3.org/2005/Atom"><title>r/Forkmesh</title>
      <entry><id>t3_a</id><title>Hello</title>
        <link href="https://www.reddit.com/r/Forkmesh/comments/a/hello/"/>
        <published>2026-09-10T00:00:00+00:00</published></entry></feed>"""
    runtime.outbound["https://www.reddit.com/r/Forkmesh/.rss"] = (429, "")
    runtime.outbound["https://old.reddit.com/r/Forkmesh/.rss"] = feed
    fell_back = _refresh(runtime, "reddit")
    assert fell_back["status"] == 200, fell_back
    assert fell_back["data"]["refresh"]["imported"] == 1
    assert [call[0] for call in runtime.outbound_calls] == [
        "https://www.reddit.com/r/Forkmesh/.rss", "https://old.reddit.com/r/Forkmesh/.rss"]

    runtime.outbound["https://old.reddit.com/r/Forkmesh/.rss"] = (429, "")
    refused = _refresh(runtime, "reddit")
    assert (refused["status"], refused["data"]["error"]) == (502, "reddit_rate_limited")
    feed_state = next(item for item in call(runtime.use("GET"), "/api/marketing/acme")[
        "data"]["settings"]["channels"] if item["id"] == "reddit")["feed"]
    assert feed_state["lastError"] == "reddit_rate_limited"
    assert feed_state["lastErrorAt"] > 0

    before = len(runtime.outbound_calls)
    again = _refresh(runtime, "reddit")
    assert (again["status"], again["data"]["error"]) == (502, "reddit_rate_limited")
    assert len(runtime.outbound_calls) == before, (
        "pressing Refresh inside the back-off must not touch Reddit")
    assert next(item for item in call(runtime.use("GET"), "/api/marketing/acme")[
        "data"]["settings"]["channels"] if item["id"] == "reddit")[
        "feed"]["lastErrorAt"] == feed_state["lastErrorAt"], (
        "and must not restart the back-off clock")

    runtime.clock += marketing.REDDIT_BACKOFF_MS + 1
    runtime.outbound["https://www.reddit.com/r/Forkmesh/.rss"] = feed
    cleared = _refresh(runtime, "reddit")
    assert cleared["status"] == 200
    assert next(item for item in cleared["data"]["settings"]["channels"]
                if item["id"] == "reddit")["feed"]["lastError"] == ""


def test_the_engagement_rate_names_what_it_divided_by():
    runtime = Runtime()
    _configure(runtime, reddit="https://www.reddit.com/r/acme/",
               x="https://x.com/acme", youtube="https://www.youtube.com/channel/" + "U" * 23)
    for channel, metrics in (
            ("reddit", {"followers": 5321, "reach": 1840, "engagements": 212}),
            ("x", {"followers": 400, "impressions": 2000, "engagements": 60}),
            ("youtube", {"followers": 900, "views": 5000, "engagements": 100})):
        assert call(runtime.use("POST", body={"channel": channel, **metrics}),
                    "/api/marketing/acme/metrics")["status"] == 201
    rows = {item["channel"]: item["health"] for item in
            call(runtime.use("GET"), "/api/marketing/acme")["data"]["summary"]["channels"]}

    reddit = rows["reddit"]["engagement"]
    assert rows["reddit"]["engagementRate"] == 11.52
    assert (reddit["base"], reddit["baseValue"], reddit["baseLabel"]) == (
        "reach", 1840, "weekly visitors")
    assert reddit["engagementsLabel"] == "weekly contributions"
    assert reddit["formula"] == "212 weekly contributions ÷ 1,840 weekly visitors × 100"
    assert reddit["readings"] == 1

    assert (rows["x"]["engagement"]["base"], rows["x"]["engagementRate"]) == ("impressions", 3.0), (
        "impressions beat the follower count: a rate over followers is not one")
    assert (rows["youtube"]["engagement"]["base"], rows["youtube"]["engagementRate"]) == ("views", 2.0)
    assert rows["youtube"]["engagement"]["engagementsLabel"] == "likes"

    runtime.clock += 86_400_000
    assert call(runtime.use("POST", body={
        "channel": "reddit", "followers": 5400, "reach": 1840, "engagements": 212}),
        "/api/marketing/acme/metrics")["status"] == 201
    again = next(item for item in call(runtime.use("GET"), "/api/marketing/acme")[
        "data"]["summary"]["channels"] if item["channel"] == "reddit")["health"]["engagement"]
    assert again["readings"] == 2 and again["unchangedSince"] < again["observedAt"], (
        "the same two numbers coming back says the source stopped refreshing them")


def test_the_reddit_desk_says_where_to_post_and_what_to_talk_about():
    runtime = Runtime()
    call(runtime.use("PUT", body={"reddit": {
        "subreddits": ["selfhosted", "git"], "queries": ["self-hosted git"]}}),
        "/api/marketing/acme/settings")
    logged = call(runtime.use("POST", body={
        "url": "https://www.reddit.com/r/selfhosted/comments/aaa111/best_self_hosted_git/",
        "title": "Best self-hosted git server for a small team?"}),
        "/api/marketing/acme/reddit")
    assert logged["status"] == 201, logged
    assert call(runtime.use("POST", body={
        "url": "https://www.reddit.com/r/selfhosted/comments/bbb222/self_hosted_git_backups/",
        "title": "Self-hosted git backups that survive a dead disk"}),
        "/api/marketing/acme/reddit")["status"] == 201
    call(runtime.use("POST", body={"subreddit": "selfhosted", "rules": [
        {"title": "No spam", "description": "Self-promotion in the megathread only."}]}),
        "/api/marketing/acme/reddit-rules")
    assert call(runtime.use("POST", body={
        "title": "How we run self-hosted git for a small team", "channel": "blog",
        "status": "published", "url": "https://forkmesh.com/blog/self-hosted-git/",
        "publishedAt": runtime.clock}), "/api/marketing/acme/content")["status"] == 201

    ideas = call(runtime.use("GET"), "/api/marketing/acme")["data"]["redditDesk"]["ideas"]
    places = {item["name"]: item for item in ideas["subreddits"]}
    assert ideas["subreddits"][0]["name"] == "selfhosted"
    assert places["selfhosted"]["open"] == 2 and places["selfhosted"]["ready"] is True
    assert "2 open threads" in places["selfhosted"]["reason"]
    assert places["git"]["ready"] is False and places["git"]["threads"] == 0
    assert places["git"]["blockers"] == ["Read r/git's rules on self-promotion first"]
    assert ideas["ready"] == ["selfhosted"] and ideas["rulesMissing"] == ["git"]

    topics = {item["topic"]: item for item in ideas["topics"]}
    assert topics["self-hosted git"]["source"] == "watch"
    assert topics["self-hosted git"]["threads"] == 2
    assert topics["self-hosted git"]["searchUrl"].startswith(
        "https://www.reddit.com/search/?q=self-hosted%20git")
    assert [item["subreddit"] for item in topics["self-hosted git"]["examples"]] == [
        "selfhosted", "selfhosted"]
    assert "self hosted" in topics and topics["self hosted"]["source"] == "threads", (
        "a phrase two logged threads share becomes a topic of its own")
    assert all(word not in topics for word in ("best for", "that survive"))
    assert topics["self-hosted git"]["evidence"] == [{
        "title": "How we run self-hosted git for a small team",
        "url": "https://forkmesh.com/blog/self-hosted-git/", "channel": "blog"}], (
        "a published post on the topic is offered as something to link")

    report = call(runtime.use("GET"), "/api/marketing/acme/report")["data"]["markdown"]
    assert "### Where to post next" in report and "### What to talk about" in report
    assert "- r/selfhosted: 2 open threads waiting for an answer" in report


def test_the_clues_the_walk_and_the_engagement_formula_reach_every_surface():
    view = (PACKAGE / "content" / "web" / "view.html").read_text()
    web = (PACKAGE / "content" / "web" / "page.js").read_text()
    native = (PACKAGE / "content" / "desktop" / "MarketingPages.cpp").read_text()
    feature = (PACKAGE / "content" / "desktop" / "MarketingFeature.cpp").read_text()
    for hook in ("data-marketing-reddit-places", "data-marketing-reddit-topics",
                 "data-marketing-reddit-walk", "data-marketing-reddit-walk-status"):
        assert hook in view, hook
    for hook in ("renderRedditIdeas", "renderRedditWalk", "engagementExplained",
                 '"marketing-reddit-walk-stop"', '"marketing-reddit-walk-update"'):
        assert hook in web, hook
    for panel in ("Where to post next", "What to talk about"):
        assert panel in native and panel in view
    assert 'value(QStringLiteral("ideas"))' in native
    assert "engagementFormula(health)" in feature and "engagementBaseLabel(health)" in feature
    for code in ("reddit_rate_limited", "reddit_refused", "channel_http_"):
        assert code in feature, code

    extension = REPO / "marketplace" / "packages" / "browser-extension" / "content" / "extension"
    walk = (extension / "reddit-walk.js").read_text()
    body = walk.split("globalThis.__forkmeshRedditWalk", 1)[1]
    for forbidden in ("chrome.", "fetch(", "XMLHttpRequest", "postMessage", "innerHTML",
                      ".click(", "KeyboardEvent", "submit", "textContent", "api/"):
        assert forbidden not in body, forbidden
    for event in ("pointermove", "mousemove", "pointerover", "mouseover"):
        assert event in walk, event
    background = (extension / "background.js").read_text()
    assert 'files: ["reddit-walk.js"]' in background
    assert '{ type: "fm-reddit-scan" }' in background
    assert '"fm-reddit-scan"' in (extension / "reddit.js").read_text()


class PingRuntime(Runtime):
    def __init__(self):
        super().__init__()
        self.pings = []

    async def notify(self, recipient, kind, title, **kwargs):
        self.pings.append(dict(kwargs, recipient=recipient, kind=kind, title=title))


def test_marketing_activity_and_browser_plugin_updates_raise_pings(monkeypatch):
    monkeypatch.setattr(marketing, "_activity_clock", lambda: 5_000_000)
    marketing._ACTIVITY_SEEN.clear()
    runtime = PingRuntime()
    created = call(runtime.use("POST", "charlie", {
        "title": "Launch story", "body": "Draft", "channel": "blog"}),
        "/api/marketing/acme/content")
    assert created["status"] == 201
    assert [(ping["recipient"], ping["kind"], ping["title"], ping["body"], ping["href"])
            for ping in runtime.pings] == [
        ("charlie", "plugin_activity", "New marketing content",
         "By you in acme: Launch story", "/dashboard/marketing"),
        ("alice", "plugin_activity", "New marketing content",
         "By charlie in acme: Launch story", "/dashboard/marketing")]

    runtime.pings.clear()
    for page in ("home", "profile"):
        assert call(runtime.use("POST", "charlie", {
            "kind": "read", "site": "x", "page": page,
            "text": f"Read @forkmesh on X ({page})."}),
            "/api/marketing/acme/activity")["status"] == 201
    assert [(ping["recipient"], ping["title"], ping["body"], ping["push"])
            for ping in runtime.pings] == [
        ("charlie", "Browser plugin read X",
         "By you in acme: Read @forkmesh on X (home).", True),
        ("charlie", "Browser plugin read X",
         "By you in acme: Read @forkmesh on X (profile).", False)]
    assert runtime.pings[0]["dedupe"] == runtime.pings[1]["dedupe"] != ""

    runtime.pings.clear()
    assert call(runtime.use("POST", "charlie", {
        "kind": "sent", "site": "reddit", "page": "r/selfhosted",
        "text": "Sent the r/selfhosted numbers to Marketing."}),
        "/api/marketing/acme/activity")["status"] == 201
    assert [(ping["recipient"], ping["title"], ping["dedupe"], ping["push"])
            for ping in runtime.pings] == [
        ("charlie", "Browser plugin sent an update to Reddit", "", True),
        ("alice", "Browser plugin sent an update to Reddit", "", True)]

    runtime.pings.clear()
    path = "/api/marketing/acme/posts"
    post = {"url": "https://x.com/forkmesh/status/1900000000000000002", "channel": "x",
            "title": "Second post", "postedAt": 1_995_000_000_000, "views": 900}
    assert call(runtime.use("POST", "charlie", {"posts": [post]}), path)["status"] == 200
    assert [ping["title"] for ping in runtime.pings] == ["1 new channel post found"] * 2
    runtime.pings.clear()
    assert call(runtime.use("POST", "charlie", {"posts": [dict(post, views=1500)]}),
                path)["status"] == 200
    assert call(runtime.use("POST", "charlie", {"kind": "nope"}),
                "/api/marketing/acme/activity")["status"] == 400
    assert call(runtime.use("GET", "charlie"), "/api/marketing/acme/content")["status"] == 200
    assert runtime.pings == []
    assert marketing._activity_rule(
        "x-connection", "", "POST", {"name": "secret"})[3] == ""
    assert marketing._activity_rule("app-store", "", "PUT", {})[:3] == (
        "App Store numbers updated", "team", True)


APP_STORE_ID = "6803060735"


def _app_store_days(runtime, count, offset=1):
    from datetime import timedelta, timezone
    today = datetime.fromtimestamp(runtime.clock / 1000, timezone.utc).date()
    return [{"day": (today - timedelta(days=offset + index)).isoformat(),
             "downloads": index % 4, "redownloads": 1, "updates": 2,
             "territories": {"US": index % 4}, "devices": {"iPhone": index % 4}}
            for index in reversed(range(count))]


def _send_app_store(runtime, days, actor="alice", **extra):
    body = {"app": {"id": APP_STORE_ID, "name": "ForkMesh",
                    "bundleId": "com.forkmesh.forkmesh"}, "days": days, **extra}
    return call(runtime.use("PUT", actor, body), "/api/marketing/acme/app-store")


def test_app_store_downloads_are_owner_sent_sealed_and_summarised_for_members():
    runtime = Runtime()
    days = _app_store_days(runtime, 40)
    del days[5]
    review = {"createdAt": "2026-10-01T10:00:00-07:00", "rating": 9, "title": "Great",
              "territory": "usa", "body": "Mirrors   my repos"}
    version = {"version": "1.4.0", "build": "18", "createdAt": "2026-10-01T10:00:00Z",
               "status": {"state": "READY_FOR_SALE", "label": "Ready for sale", "tone": "good"}}
    assert _send_app_store(runtime, days, "charlie")["status"] == 403
    sent = _send_app_store(runtime, days, reviews=[review, {}], versions=[version])
    assert sent["status"] == 200 and sent["data"]["stored"] == 39
    rows = runtime.db.execute("SELECT data FROM marketing_app_store_days").fetchall()
    assert len(rows) == 39 and all(row["data"].startswith("sealed:") for row in rows)
    read = call(runtime.use("GET", "charlie"), "/api/marketing/acme/app-store")["data"]
    assert read["canPublish"] is False
    store = read["appStore"]
    assert store["app"] == {"id": APP_STORE_ID, "name": "ForkMesh",
                            "bundleId": "com.forkmesh.forkmesh"}
    assert (store["firstDay"], store["lastDay"]) == (days[0]["day"], days[-1]["day"])
    assert len(store["days"]) == 40 and store["days"][5]["downloads"] == 0
    assert set(store["days"][0]) == {"day", "downloads", "redownloads", "updates"}
    month = next(window for window in store["windows"] if window["days"] == 30)
    assert month["to"] == store["lastDay"] and month["redownloads"] == 30
    assert month["downloads"] == sum(day["downloads"] for day in days[-30:])
    assert month["territories"] == [{"code": "US", "downloads": month["downloads"]}]
    assert [window["comparable"] for window in store["windows"]] == [True, False, False, False]
    assert store["total"]["updates"] == 78 and store["bestDay"]["downloads"] == 3
    assert store["reviews"] == [{"createdAt": 1790874000000, "rating": 5, "title": "Great",
                                 "territory": "USA", "body": "Mirrors my repos"}]
    assert store["versions"][0]["status"]["label"] == "Ready for sale"
    assert store["publishedBy"] == "alice" and store["fresh"] is True
    overview = call(runtime.use("GET"), "/api/marketing/acme")["data"]
    channel = next(item for item in overview["summary"]["channels"]
                   if item["channel"] == "app-store")
    assert (channel["downloads"], channel["redownloads"]) == (month["downloads"], 30)
    assert channel["health"]["source"] == "app-store-connect"
    assert [check["id"] for check in channel["health"]["checks"]] == ["link", "fresh"]
    assert channel["health"]["state"] == "good"
    settings = next(item for item in overview["settings"]["channels"] if item["id"] == "app-store")
    assert settings["profileUrl"] == "https://apps.apple.com/app/id" + APP_STORE_ID
    before = len(runtime.db.execute("SELECT 1 FROM marketing_metrics").fetchall())
    _send_app_store(runtime, days[-3:])
    assert len(runtime.db.execute("SELECT 1 FROM marketing_metrics").fetchall()) == before


def test_app_store_days_are_validated_pruned_and_reset_for_another_app():
    runtime = Runtime()
    assert _send_app_store(runtime, [{"day": "2026-02-30"}])["data"]["error"] == (
        "invalid_app_store_day")
    assert _send_app_store(runtime, _app_store_days(runtime, 1, 900))["status"] == 400
    assert _send_app_store(runtime, _app_store_days(runtime, 401))["data"]["error"] == (
        "invalid_app_store_days")
    assert call(runtime.use("PUT", body={"days": []}),
                "/api/marketing/acme/app-store")["data"]["error"] == "app_store_app_required"
    days = _app_store_days(runtime, 3)
    days[0].update(downloads=-4, updates=True, territories={"usa": 2, "DE": "x", "FR": 2.9},
                   devices={"": 3, "iPad": 1})
    stored = _send_app_store(runtime, days)["data"]["appStore"]
    assert stored["days"][0]["downloads"] == 0 and stored["days"][0]["updates"] == 0
    week = stored["windows"][0]
    assert week["territories"] == [{"code": "FR", "downloads": 2}, {"code": "US", "downloads": 1}]
    assert {"name": "iPad", "downloads": 1} in week["devices"]
    other = call(runtime.use("PUT", body={"app": {"id": "42"}, "days": days[-1:]}),
                 "/api/marketing/acme/app-store")["data"]["appStore"]
    assert other["app"]["id"] == "42" and len(other["days"]) == 1


def test_app_store_refresh_reads_the_rating_and_listing_from_the_public_lookup():
    runtime = Runtime()
    _configure(runtime, **{"app-store": "https://apps.apple.com/de/app/forkmesh/id" + APP_STORE_ID})
    lookup = "https://itunes.apple.com/lookup?id=%s&country=de" % APP_STORE_ID
    runtime.outbound[lookup] = json.dumps({"resultCount": 1, "results": [{
        "trackId": int(APP_STORE_ID), "trackName": "ForkMesh", "averageUserRating": 4.62,
        "userRatingCount": 12, "version": "1.4.0", "primaryGenreName": "Developer Tools",
        "currentVersionReleaseDate": "2026-10-10T07:00:00Z", "formattedPrice": "Free",
        "trackViewUrl": "https://apps.apple.com/de/app/forkmesh/id6803060735?uo=4"}]})
    refreshed = _refresh(runtime, "app-store")["data"]
    assert refreshed["refresh"]["metrics"]["ratings"] == 12
    channel = next(item for item in refreshed["summary"]["channels"]
                   if item["channel"] == "app-store")
    assert channel["rating"] == 4.62 and channel["health"]["source"] == "app-store"
    guide = {step["id"]: step["done"] for step in channel["health"]["guide"]}
    assert guide == {"link": True, "rating": True, "downloads": False}
    store = call(runtime.use("GET"), "/api/marketing/acme/app-store")["data"]["appStore"]
    assert store["lookup"]["version"] == "1.4.0" and store["lookup"]["country"] == "DE"
    assert store["lastDay"] == "" and store["days"] == [] and store["windows"] == []
    sent = _send_app_store(runtime, _app_store_days(runtime, 2))["data"]["appStore"]
    assert sent["lookup"]["ratings"] == 12
    overview = call(runtime.use("GET"), "/api/marketing/acme")["data"]
    channel = next(item for item in overview["summary"]["channels"]
                   if item["channel"] == "app-store")
    assert channel["rating"] == 4.62 and channel["ratings"] == 12
    _configure(runtime, **{"app-store": "https://apps.apple.com/app/forkmesh"})
    assert _refresh(runtime, "app-store")["data"]["error"] == "app_store_url_unsupported"
    assert asyncio.run(marketing.org_has_data(runtime, "org:acme"))
    exported = asyncio.run(marketing.export_org_data(runtime, "org:acme", "acme"))
    assert len(exported["appStore"]["days"]) == 2 and exported["appStore"]["lookup"]["ratings"] == 12
    asyncio.run(marketing.purge_org_data(runtime, "org:acme"))
    assert asyncio.run(marketing.org_has_data(runtime, "org:acme")) is False


def test_the_app_store_desk_is_wired_on_the_web_surface():
    view = (PACKAGE / "content" / "web" / "view.html").read_text()
    web = (PACKAGE / "content" / "web" / "page.js").read_text()
    manifest = json.loads((PACKAGE / "manifest.json").read_text())
    assert "https://itunes.apple.com" in manifest["worker"]["outboundOrigins"]
    assert marketing.APP_STORE_LOOKUP_ORIGIN in manifest["worker"]["outboundOrigins"]
    for hook in ("data-marketing-app-store-page", "data-marketing-app-store-chart",
                 'data-marketing-app-store-range="365"', 'data-marketing-app-store-metric="updates"',
                 "data-marketing-app-store-countries", "data-marketing-app-store-devices",
                 "data-marketing-app-store-reviews", "data-marketing-app-store-versions",
                 "data-marketing-app-store-listing", "asc_store_report.py --publish"):
        assert hook in view, hook
    for hook in ('orgPath("/app-store")', 'name === "app-store"', '"app-store": `<button data-marketing-page="app-store"',
                 'channel === "app-store") return appStoreDesk(item)', '["downloads", "Downloads · 30D"]',
                 '"app-store-connect": "from App Store Connect"', '"channels", "app-store"].includes(params.get("page"))'):
        assert hook in web, hook
    for step in marketing._app_store_guide({}, [], 0):
        assert step["action"] in ("link", "refresh", "app-store")
