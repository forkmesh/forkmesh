from __future__ import annotations

import asyncio
import importlib.util
import json
import sqlite3
import sys
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
MIGRATIONS = ROOT / "migrations"

sys.path.insert(0, str(SRC))
import plugin_race  # noqa: E402


def _module(name):
    spec = importlib.util.spec_from_file_location(name, SRC / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


urls = _module("urls")
static_routes = _module("static_routes")

HOUR = 60 * 60 * 1000
OPEN = plugin_race.STARTS_AT + HOUR
POSTS = {
    "x": "https://twitter.com/ana_builds/status/1840000000000000001",
    "reddit": "https://old.reddit.com/r/selfhosted/comments/1abc23/my_forkmesh_plugin/",
    "mastodon": "https://mastodon.social/@ana/113000000000000001",
}
WALLET = "3n1Xd9K7bTFVJdWXsZbLuiPGgCwXsBAmyYq5tE2rvqPz"
SIGNATURE = "5" * 88


class Response:
    def __init__(self, data, status):
        self.data = data
        self.status = status


class Runtime:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.executescript(
            "CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL);")
        self.db.executescript(
            (MIGRATIONS / "0198_marketplace_reviews.sql").read_text(encoding="utf-8"))
        self.db.executescript(
            (MIGRATIONS / "0262_affiliates_and_plugin_race.sql").read_text(encoding="utf-8"))
        self._method = "POST"
        self._body = {}
        self._now = OPEN
        self.same = True
        self.records = {}
        self.flags = {}
        self.discord = set()
        self.pages = {}
        self.published = set()
        self.wallets = {}
        self.admins = {"boss"}
        self.current = None
        self.audits = []

    def person(self, name, verified=True):
        self.records["acct-" + name] = {"name": name, "email_verified": verified}
        self.current = "acct-" + name
        return self

    def call(self, path, body=None, method="POST"):
        self._method = method
        self._body = body or {}
        return asyncio.run(plugin_race.handle(self, path))

    def method(self):
        return self._method

    def same_origin(self):
        return self.same

    def now(self):
        return self._now

    def response(self, data, status=200, cache_control=None, extra_headers=None):
        return Response(json.loads(json.dumps(data)), status)

    async def ensure_schema(self):
        return None

    async def json_body(self, limit):
        return dict(self._body), ""

    async def session(self, data=None):
        if not self.current:
            return "", None
        return self.current, self.records[self.current]

    async def is_admin(self, name):
        return name in self.admins

    async def blind(self, value):
        value = str(value)
        if value in {record["name"] for record in self.records.values()}:
            return "acct-" + value
        return "bi:" + value

    async def seal(self, value):
        return json.dumps(value)

    async def open(self, value):
        try:
            return json.loads(value)
        except Exception:
            return None

    async def d1_all(self, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]

    async def d1_first(self, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row else None

    async def d1_run(self, sql, *args):
        self.db.execute(sql, args)
        self.db.commit()

    async def audit(self, actor, action, resource_type, resource_id, details):
        self.audits.append((actor, action, resource_id))

    async def account_record(self, account_bi):
        return self.records.get(account_bi)

    async def member_flags(self, account_bi, record):
        return list(self.flags.get(account_bi, []))

    async def discord_linked(self, account_bi):
        return account_bi in self.discord

    async def publishes_package(self, account_bi, market, package, since):
        return (account_bi, market, package) in self.published

    async def payout_wallet(self, account_bi):
        return self.wallets.get(account_bi, "")

    async def read_public_page(self, url, max_bytes):
        for prefix, text in self.pages.items():
            if url.startswith(prefix):
                return text
        return None


def tagged_pages(runtime):
    runtime.pages = {
        "https://publish.twitter.com/oembed": '{"html":"<a href=\\"https://twitter.com/hashtag/ForkMeshPluginRace\\">#ForkMeshPluginRace</a>"}',
        "https://www.reddit.com/": '[{"data":{"children":[{"data":{"title":"Building for #forkmeshpluginrace"}}]}}]',
        "https://mastodon.social/api/v1/statuses/": '{"content":"<p>Joining <a>#<span>ForkMeshPluginRace</span></a></p>"}',
    }


def qualify(runtime, name, posts=None):
    runtime.person(name)
    runtime.discord.add("acct-" + name)
    suffix = str(sum(ord(char) for char in name))
    body = posts or {
        "x": "https://x.com/%s/status/18400000000%s" % (name, suffix.zfill(6)),
        "reddit": "https://www.reddit.com/r/forkmesh/comments/%s/race/" % ("p" + suffix),
        "mastodon": "https://mastodon.social/@%s/1130000000%s" % (name, suffix.zfill(6)),
    }
    response = runtime.call(plugin_race.ENTRANT_PATH, dict(body, acceptRules=True))
    assert response.status in (200, 201), response.data
    return response


def rate(runtime, package, reviewer, stars=5, at=None, hidden=0):
    runtime.db.execute(
        "INSERT INTO marketplace_reviews (review_id,market,package_id,reviewer_bi,"
        "reviewer_name,stars,hidden,created_at,updated_at) VALUES (?,?,?,?,?,?,?,?,?)",
        ("r-%s-%s" % (package, reviewer), plugin_race.MARKET, package,
         "acct-" + reviewer, reviewer, stars, hidden, at or runtime._now,
         at or runtime._now))
    runtime.db.commit()


def enter_plugin(runtime, name, package):
    runtime.published.add(("acct-" + name, plugin_race.MARKET, package))
    runtime.current = "acct-" + name
    return runtime.call(plugin_race.PLUGINS_PATH, {
        "package": package,
        "summary": "Shows the repositories you have not pushed this week.",
        "platforms": {"desktop": "Sidebar, Stale repos", "ios": "More tab",
                      "web": "Dashboard sidebar"},
    })


def admin(runtime, body):
    runtime.person("boss")
    return runtime.call(plugin_race.ADMIN_PATH, body)


def test_post_links_are_reduced_to_one_canonical_public_post():
    url, check = plugin_race.post_link("x", POSTS["x"])
    assert url == "https://x.com/ana_builds/status/1840000000000000001"
    assert check.startswith("https://publish.twitter.com/oembed?omit_script=1&url=https%3A%2F%2Fx.com")
    url, check = plugin_race.post_link("reddit", POSTS["reddit"])
    assert url == "https://www.reddit.com/r/selfhosted/comments/1abc23/"
    assert check == url + ".json"
    url, check = plugin_race.post_link("mastodon", POSTS["mastodon"])
    assert url == POSTS["mastodon"]
    assert check == "https://mastodon.social/api/v1/statuses/113000000000000001"
    for network, bad in (
            ("x", "http://x.com/ana/status/1840000000000000001"),
            ("x", "https://x.com/ana"),
            ("x", "https://evil.example/ana/status/1840000000000000001"),
            ("reddit", "https://www.reddit.com/r/forkmesh/"),
            ("reddit", "https://reddit.evil.example/r/a/comments/abc/"),
            ("mastodon", "https://127.0.0.1/@ana/113000000000000001"),
            ("mastodon", "https://mastodon.local/@ana/113000000000000001"),
            ("mastodon", "https://x.com/@ana/113000000000000001"),
            ("mastodon", "https://mastodon.social:8443/@ana/113000000000000001"),
            ("discord", "https://discord.com/channels/1/2/3")):
        assert plugin_race.post_link(network, bad) == ("", ""), bad


def test_the_hashtag_check_ignores_case_and_markup():
    assert plugin_race.carries_hashtag("Building for #forkmeshpluginrace!")
    assert plugin_race.carries_hashtag('<a>#<span>ForkMeshPluginRace</span></a>')
    assert not plugin_race.carries_hashtag("#ForkMesh plugin race")


def test_an_entrant_qualifies_only_after_all_four_checks():
    runtime = Runtime()
    tagged_pages(runtime)
    runtime.person("ana")
    assert runtime.call(plugin_race.ENTRANT_PATH, {"x": POSTS["x"]}).data["error"] == "rules_required"
    runtime.flags["acct-ana"] = ["desktop_activity_unproven"]
    response = runtime.call(plugin_race.ENTRANT_PATH, dict(POSTS, acceptRules=True))
    assert response.status == 201
    entrant = response.data["me"]["entrant"]
    assert entrant["status"] == "pending"
    assert entrant["checks"] == {"account": "confirmed", "member": "pending",
                                 "discord": "missing", "x": "confirmed",
                                 "reddit": "confirmed", "mastodon": "confirmed"}
    response = runtime.call(plugin_race.ENTRANT_PATH, {"discord": "@Ana.Builds"})
    assert response.data["me"]["entrant"]["checks"]["discord"] == "pending"
    assert response.data["me"]["entrant"]["discord"] == "ana.builds"
    runtime.discord.add("acct-ana")
    runtime.flags["acct-ana"] = []
    runtime._now += HOUR
    response = runtime.call(plugin_race.ENTRANT_PATH, {})
    assert response.data["me"]["entrant"]["status"] == "qualified"
    assert response.data["me"]["entrant"]["qualifiedAt"] == runtime._now


def test_unverified_accounts_cannot_enter():
    runtime = Runtime()
    runtime.person("ana", verified=False)
    assert runtime.call(plugin_race.ENTRANT_PATH, dict(POSTS, acceptRules=True)).status == 403


def test_a_post_without_the_hashtag_or_out_of_reach_is_not_confirmed():
    runtime = Runtime()
    runtime.person("ana")
    runtime.pages = {"https://publish.twitter.com/oembed": '{"html":"just a post"}'}
    response = runtime.call(plugin_race.ENTRANT_PATH, dict(POSTS, acceptRules=True))
    posts = response.data["me"]["entrant"]["posts"]
    assert posts["x"]["state"] == "missing"
    assert "#ForkMeshPluginRace" in posts["x"]["detail"]
    assert posts["reddit"]["state"] == "pending"
    assert posts["mastodon"]["state"] == "pending"


def test_one_post_cannot_be_used_by_two_people():
    runtime = Runtime()
    tagged_pages(runtime)
    runtime.person("ana")
    runtime.call(plugin_race.ENTRANT_PATH, dict(POSTS, acceptRules=True))
    runtime.person("bo")
    response = runtime.call(plugin_race.ENTRANT_PATH, {"x": POSTS["x"], "acceptRules": True})
    assert response.status == 409
    assert response.data == {"error": "post_already_used", "network": "x"}


def test_admin_confirms_a_post_the_network_would_not_show_us():
    runtime = Runtime()
    runtime.person("ana")
    runtime.discord.add("acct-ana")
    tagged_pages(runtime)
    del runtime.pages["https://www.reddit.com/"]
    runtime.call(plugin_race.ENTRANT_PATH, dict(POSTS, acceptRules=True))
    assert admin(runtime, {"action": "post", "account": "ana", "network": "reddit",
                           "state": "confirmed"}).data["status"] == "qualified"
    runtime.person("mia")
    assert runtime.call(plugin_race.ADMIN_PATH, {"action": "award"}).status == 403


def test_plugin_entries_need_a_qualified_builder_an_open_race_and_all_three_platforms():
    runtime = Runtime()
    tagged_pages(runtime)
    runtime.person("ana")
    assert enter_plugin(runtime, "ana", "stale-repos").data["error"] == "qualify_first"
    qualify(runtime, "ana")
    runtime._now = plugin_race.STARTS_AT - 1
    assert enter_plugin(runtime, "ana", "stale-repos").data["error"] == "race_not_open"
    runtime._now = OPEN
    runtime.current = "acct-ana"
    assert runtime.call(plugin_race.PLUGINS_PATH, {
        "package": "someone-else", "summary": "x",
        "platforms": {"desktop": "a", "ios": "b", "web": "c"}}).data["error"] == "package_not_yours"
    runtime.published.add(("acct-ana", plugin_race.MARKET, "half-done"))
    response = runtime.call(plugin_race.PLUGINS_PATH, {
        "package": "half-done", "summary": "x", "platforms": {"desktop": "a"}})
    assert response.data == {"error": "platforms_required", "missing": ["ios", "web"]}
    response = enter_plugin(runtime, "ana", "stale-repos")
    assert response.status == 201
    assert response.data["me"]["plugins"][0]["status"] == "review"
    assert enter_plugin(runtime, "ana", "stale-repos").data["error"] == "already_entered"
    for package in ("two", "three"):
        assert enter_plugin(runtime, "ana", package).status == 201
    assert enter_plugin(runtime, "ana", "four").data["error"] == "plugin_limit"
    runtime._now = plugin_race.ENDS_AT + 1
    assert enter_plugin(runtime, "ana", "late").data["error"] == "race_closed"


def board(runtime):
    runtime.current = None
    return runtime.call(plugin_race.PREFIX, method="GET").data


def test_only_qualified_raters_with_three_stars_or_more_move_a_plugin():
    runtime = Runtime()
    tagged_pages(runtime)
    qualify(runtime, "ana")
    enter_plugin(runtime, "ana", "stale-repos")
    admin(runtime, {"action": "plugin", "package": "stale-repos", "status": "accepted"})
    for index in range(8):
        qualify(runtime, "rater%d" % index)
        rate(runtime, "stale-repos", "rater%d" % index)
    qualify(runtime, "grump")
    rate(runtime, "stale-repos", "grump", stars=2)
    qualify(runtime, "abuser")
    rate(runtime, "stale-repos", "abuser", hidden=1)
    runtime.person("stranger")
    rate(runtime, "stale-repos", "stranger")
    entry = board(runtime)["plugins"][0]
    assert entry["ratings"] == 8
    assert entry["rank"] == 0 and entry["reachedAt"] == 0
    assert "ownerBi" not in entry


def test_the_tenth_counted_rating_sets_the_finish_time():
    runtime = Runtime()
    tagged_pages(runtime)
    for builder, package in (("ana", "alpha"), ("bo", "beta")):
        qualify(runtime, builder)
        enter_plugin(runtime, builder, package)
        admin(runtime, {"action": "plugin", "package": package, "status": "accepted"})
    raters = ["r%d" % index for index in range(10)]
    for rater in raters:
        qualify(runtime, rater)
    for index, rater in enumerate(raters):
        rate(runtime, "alpha", rater, at=OPEN + (index + 1) * HOUR)
        rate(runtime, "beta", rater, at=OPEN + index * HOUR)
    runtime.person("late")
    runtime._now = OPEN + 30 * HOUR
    rate(runtime, "alpha", "late", at=OPEN + 1)
    qualify(runtime, "late")
    data = board(runtime)
    order = [(entry["package"], entry["rank"]) for entry in data["plugins"]]
    assert order == [("beta", 1), ("alpha", 2)]
    alpha = data["plugins"][1]
    assert alpha["ratings"] == 11
    assert alpha["reachedAt"] == OPEN + 10 * HOUR
    assert data["prizesLeft"] == 8


def test_ratings_before_the_race_count_from_the_start_and_after_it_never():
    times = [plugin_race.STARTS_AT - HOUR] * 9 + [plugin_race.ENDS_AT + 1]
    ratings = [{"market": plugin_race.MARKET, "package_id": "p",
                "reviewer_bi": "r%d" % index, "created_at": at, "qualified_at": 0}
               for index, at in enumerate(times)]
    entry = plugin_race.standings(
        [{"market": plugin_race.MARKET, "package": "p", "ownerBi": "o"}], ratings)[0]
    assert entry["ratings"] == 9
    ratings[-1]["created_at"] = plugin_race.STARTS_AT - 5
    entry = plugin_race.standings(
        [{"market": plugin_race.MARKET, "package": "p", "ownerBi": "o"}], ratings)[0]
    assert entry["reachedAt"] == plugin_race.STARTS_AT


def test_only_ten_prizes_and_frozen_winners_keep_their_place():
    plugins = [{"market": plugin_race.MARKET, "package": "p%02d" % index, "ownerBi": "o"}
               for index in range(12)]
    ratings = [{"market": plugin_race.MARKET, "package_id": plugin["package"],
                "reviewer_bi": "r%d" % rater,
                "created_at": OPEN + index * HOUR + rater, "qualified_at": 0}
               for index, plugin in enumerate(plugins) for rater in range(10)]
    board_now = plugin_race.standings(plugins, ratings)
    winners = [entry["package"] for entry in board_now if entry["rank"]]
    assert winners == ["p%02d" % index for index in range(10)]
    frozen = {(plugin_race.MARKET, "p11"): 1}
    board_frozen = plugin_race.standings(plugins, ratings, frozen)
    ranks = {entry["package"]: entry["rank"] for entry in board_frozen}
    assert ranks["p11"] == 1 and ranks["p00"] == 2 and ranks["p09"] == 0
    assert sorted(rank for rank in ranks.values() if rank) == list(range(1, 11))


def test_award_freezes_winners_and_payment_needs_an_award():
    runtime = Runtime()
    tagged_pages(runtime)
    qualify(runtime, "ana")
    enter_plugin(runtime, "ana", "alpha")
    runtime.wallets["acct-ana"] = WALLET
    admin(runtime, {"action": "plugin", "package": "alpha", "status": "accepted"})
    assert admin(runtime, {"action": "paid", "package": "alpha", "tx": SIGNATURE}).data["error"] == "not_awarded"
    for index in range(10):
        qualify(runtime, "r%d" % index)
        rate(runtime, "alpha", "r%d" % index)
    response = admin(runtime, {"action": "award"})
    assert response.data["awarded"] == [{"package": "alpha", "rank": 1}]
    assert admin(runtime, {"action": "award"}).data["awarded"] == []
    listing = runtime.call(plugin_race.ADMIN_PATH, method="GET").data
    assert listing["standings"][0]["wallet"] == WALLET
    assert admin(runtime, {"action": "paid", "package": "alpha", "tx": "nope"}).status == 400
    assert admin(runtime, {"action": "paid", "package": "alpha", "tx": SIGNATURE}).status == 200
    entry = board(runtime)["plugins"][0]
    assert entry["final"] and entry["paid"] and entry["paidTx"] == SIGNATURE
    assert admin(runtime, {"action": "plugin", "package": "alpha", "status": "rejected"}).status == 409


def test_an_unpaid_winner_can_be_dropped_and_the_slot_passes_on():
    runtime = Runtime()
    tagged_pages(runtime)
    for builder, package in (("ana", "alpha"), ("bo", "beta")):
        qualify(runtime, builder)
        enter_plugin(runtime, builder, package)
        admin(runtime, {"action": "plugin", "package": package, "status": "accepted"})
    for index in range(10):
        qualify(runtime, "r%d" % index)
        rate(runtime, "alpha", "r%d" % index, at=OPEN + index)
        rate(runtime, "beta", "r%d" % index, at=OPEN + 100 + index)
    admin(runtime, {"action": "award"})
    assert admin(runtime, {"action": "plugin", "package": "alpha", "status": "rejected",
                           "note": "No payout address after 30 days"}).status == 200
    data = board(runtime)
    assert [(entry["package"], entry["rank"], entry["final"]) for entry in data["plugins"]] == [
        ("beta", 2, True)]
    assert admin(runtime, {"action": "award"}).data["awarded"] == []
    assert data["prizesLeft"] == 9


def test_disqualified_raters_stop_counting():
    runtime = Runtime()
    tagged_pages(runtime)
    qualify(runtime, "ana")
    enter_plugin(runtime, "ana", "alpha")
    admin(runtime, {"action": "plugin", "package": "alpha", "status": "accepted"})
    qualify(runtime, "cheat")
    rate(runtime, "alpha", "cheat")
    assert board(runtime)["plugins"][0]["ratings"] == 1
    admin(runtime, {"action": "entrant", "account": "cheat", "status": "disqualified"})
    assert board(runtime)["plugins"][0]["ratings"] == 0
    runtime.current = "acct-cheat"
    assert runtime.call(plugin_race.ENTRANT_PATH, {}).data["error"] == "disqualified"


def test_review_plugins_stay_off_the_public_board():
    runtime = Runtime()
    tagged_pages(runtime)
    qualify(runtime, "ana")
    enter_plugin(runtime, "ana", "alpha")
    data = board(runtime)
    assert data["plugins"] == []
    assert data["race"]["hashtag"] == "#ForkMeshPluginRace"
    assert data["race"]["prizeSol"] == "0.1" and data["race"]["prizeSlots"] == 10
    assert data["qualifiedEntrants"] == 1


@pytest.mark.parametrize("path", ["/api/plugin-race", "/api/plugin-race/entrant",
                                  "/api/plugin-race/plugins", "/api/admin/plugin-race"])
def test_routes_are_wired(path):
    assert urls.PLUGIN_RACE_RE.match(path)
    assert "plugin_race_handler(self.env, request, url.path)" in (
        SRC / "app_runtime.py").read_text(encoding="utf-8")


def test_the_race_pages_are_served_by_the_website():
    for path in ("/hackathon", "/hackathon/rules", "/hackathon/archive",
                 "/hackathon/archive/revenue-contest-2026",
                 "/hackathon/archive/revenue-contest-2026-rules"):
        assert static_routes.external_site_route(path) == "www", path
    assert static_routes.external_site_route("/hackathon/archive/other") != "www"


def test_the_prize_is_one_tenth_of_a_sol():
    assert plugin_race.PRIZE_LAMPORTS == 100_000_000
    assert plugin_race.RATINGS_NEEDED == 10 and plugin_race.PRIZE_SLOTS == 10
