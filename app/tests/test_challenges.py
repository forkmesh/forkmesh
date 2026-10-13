#!/usr/bin/env python3

import ast
import asyncio
import importlib.util
import itertools
import json
import sqlite3
import sys
from datetime import date
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import challenges as policy  # noqa: E402
import schema  # noqa: E402

DAY_MS = 86_400_000
NOW = 1_790_000_000_000
TODAY = date(2026, 9, 21)


def _literal(path, name):
    for node in ast.parse(path.read_text(encoding="utf-8")).body:
        if isinstance(node, ast.Assign) and any(
                getattr(target, "id", None) == name
                for target in node.targets):
            return ast.literal_eval(node.value)
    raise AssertionError(name)


class FakeResponse:
    def __init__(self, data, status, headers):
        self.data = data
        self.status = status
        self.headers = headers


class FakeRequest:
    def __init__(self, method="GET", body=None, user=""):
        self.method = method
        self.body = body
        self.user = user


class FakeDate:
    now_ms = NOW

    @classmethod
    def now(cls):
        return cls.now_ms


class Harness:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        entry = SRC / "entry.py"
        for sql in (_literal(entry, "SCHEMA_PRE_CREATE_ALTER_STATEMENTS")
                    + schema.SCHEMA_STATEMENTS
                    + _literal(entry, "SCHEMA_ALTER_STATEMENTS")):
            try:
                self.db.execute(sql)
            except sqlite3.Error:
                pass
        self.accounts = {}
        self.ids = itertools.count(1)
        spec = importlib.util.spec_from_file_location(
            "challenges_runtime_under_test", SRC / "challenges_runtime.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        harness = self

        async def d1_all(_env, sql, *args):
            return [dict(row) for row in harness.db.execute(sql, args)]

        async def d1_first(_env, sql, *args):
            row = harness.db.execute(sql, args).fetchone()
            return dict(row) if row else None

        async def d1_run(_env, sql, *args):
            harness.db.execute(sql, args)

        async def account_session_record(_env, request, _data=None):
            rec = harness.accounts.get(request.user)
            return (request.user + "-bi", rec) if rec else ("", None)

        async def bounded_json_request(request, _limit):
            if request.body == "broken":
                raise ValueError("bad json")
            return request.body

        def json_response(data, status=200, cache_seconds=None,
                          cache_control=None, extra_headers=None):
            return FakeResponse(data, status, dict(extra_headers or {}))

        module._bind_runtime({
            "Date": FakeDate,
            "d1_all": d1_all,
            "d1_first": d1_first,
            "d1_run": d1_run,
            "_account_session_record": account_session_record,
            "_account_kind": lambda rec: rec.get("kind", "user"),
            "bounded_json_request": bounded_json_request,
            "json_response": json_response,
            "method_name": lambda request: request.method,
            "_ap_uuid": lambda: "%032x" % next(harness.ids),
        })
        self.runtime = module

    def user(self, name, kind="user", status="active"):
        self.accounts[name] = {"name": name, "kind": kind, "status": status}

    def call(self, path, method="GET", body=None, user=""):
        return asyncio.run(self.runtime.api_handler(
            None, FakeRequest(method, body, user), path))

    def repo(self, repo_bi, private=0, public=1, generation="g1"):
        self.db.execute(
            "INSERT INTO repositories (key_bi, owner_bi, data, is_private) "
            "VALUES (?,?,?,?)", (repo_bi, "owner", "{}", private))
        self.db.execute(
            "INSERT INTO profile_contribution_projects (source_repo_bi, "
            "source_account_bi, owner_user_bi, project_bi, first_public_day, "
            "is_public, active_generation_bi, captured_at, data) "
            "VALUES (?,?,?,?,?,?,?,?,?)",
            (repo_bi, "owner", "owner", "project-" + repo_bi, "2026-01-01",
             public, generation, 1, "{}"))

    def day(self, repo_bi, user, day, commits=0, pulls=0, reviews=0,
            issues=0, generation="g1"):
        self.db.execute(
            "INSERT INTO profile_contribution_days (generation_bi, "
            "subject_user_bi, source_account_bi, source_repo_bi, project_bi, "
            "day, commits, issues, pulls, reviews, captured_at, data) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
            (generation, user + "-bi", "owner", repo_bi, "project-" + repo_bi,
             day, commits, issues, pulls, reviews, 1, "{}"))


@pytest.fixture(autouse=True)
def _clock():
    FakeDate.now_ms = NOW
    yield
    FakeDate.now_ms = NOW


@pytest.fixture
def harness():
    h = Harness()
    h.user("alice")
    h.user("bob")
    h.user("carol")
    return h


def test_default_preset_is_one_click_week_of_code_points():
    settings = policy.settings_from_request({"preset": "week"}, TODAY)
    assert settings == {
        "name": "7-day code challenge",
        "metric": "points",
        "weights": {"commits": 1, "pulls": 5, "reviews": 2, "issues": 1},
        "dailyCap": 0,
        "maxMembers": 10,
        "startsOn": "2026-09-21",
        "endsOn": "2026-09-27",
    }
    assert policy.options()["defaultPreset"] == "week"


def test_custom_settings_are_validated():
    settings = policy.settings_from_request({
        "name": "  Weekend   sprint ", "metric": "custom",
        "weights": {"commits": 2, "pulls": 10}, "days": "2",
        "start": "tomorrow", "dailyCap": 50, "maxMembers": 4,
    }, TODAY)
    assert settings["name"] == "Weekend sprint"
    assert settings["weights"] == {"commits": 2, "pulls": 10, "reviews": 0,
                                   "issues": 0}
    assert (settings["startsOn"], settings["endsOn"]) == (
        "2026-09-22", "2026-09-23")
    cases = [
        ({"name": ""}, "invalid_name"),
        ({"name": "x" * 61}, "invalid_name"),
        ({"name": "ok", "metric": "lines"}, "invalid_metric"),
        ({"name": "ok", "metric": "custom", "weights": {}}, "invalid_weights"),
        ({"name": "ok", "metric": "custom", "weights": {"lines": 1}},
         "invalid_weights"),
        ({"name": "ok", "days": 0}, "invalid_days"),
        ({"name": "ok", "days": 32}, "invalid_days"),
        ({"name": "ok", "days": True}, "invalid_days"),
        ({"name": "ok", "start": "2026-09-20"}, "invalid_start"),
        ({"name": "ok", "start": "2026-10-22"}, "invalid_start"),
        ({"name": "ok", "maxMembers": 1}, "invalid_max_members"),
        ({"name": "ok", "dailyCap": -1}, "invalid_daily_cap"),
        ({"name": "ok", "owner": "me"}, "invalid_settings"),
        ({"preset": "forever"}, "invalid_preset"),
        ({"preset": "week", "days": 3}, "invalid_preset"),
    ]
    for raw, code in cases:
        with pytest.raises(policy.ChallengeError) as caught:
            policy.settings_from_request(raw, TODAY)
        assert str(caught.value) == code, raw


def test_board_scores_caps_and_ranks_with_ties():
    challenge = {
        "ownerBi": "a", "startsOn": "2026-09-20", "endsOn": "2026-09-22",
        "settings": {"weights": {"commits": 1, "pulls": 5, "reviews": 0,
                                 "issues": 0}, "dailyCap": 10},
    }
    members = [
        {"account_bi": "a", "name": "alice", "joined_at": 1},
        {"account_bi": "b", "name": "bob", "joined_at": 2},
        {"account_bi": "c", "name": "carol", "joined_at": 3},
    ]
    rows = [
        {"user_bi": "a", "day": "2026-09-20", "commits": 3, "pulls": 4},
        {"user_bi": "a", "day": "2026-09-21", "commits": 2},
        {"user_bi": "b", "day": "2026-09-21", "commits": 12},
        {"user_bi": "c", "day": "2026-09-19", "commits": 50},
    ]
    result = policy.board(challenge, members, rows, TODAY, "b")
    assert result["days"] == ["2026-09-20", "2026-09-21"]
    assert [(e["name"], e["total"], e["rank"], e["days"])
            for e in result["entries"]] == [
        ("alice", 12, 1, [10, 2]),
        ("bob", 10, 2, [0, 10]),
        ("carol", 0, 3, [0, 0]),
    ]
    assert result["entries"][1]["you"] is True
    assert result["entries"][1]["today"] == 10
    assert result["phase"] == "active"
    assert result["winners"] == []

    tied = policy.board(challenge, members[:2], [
        {"user_bi": "a", "day": "2026-09-20", "commits": 4},
        {"user_bi": "b", "day": "2026-09-22", "commits": 4},
    ], date(2026, 9, 30))
    assert [e["rank"] for e in tied["entries"]] == [1, 1]
    assert tied["phase"] == "finished"
    assert tied["winners"] == ["alice", "bob"]


def test_one_click_create_join_and_live_scores(harness):
    assert harness.call("/api/challenges").status == 401
    created = harness.call("/api/challenges", "POST", {"preset": "week"},
                           "alice")
    assert created.status == 201
    item = created.data["challenge"]
    assert item["name"] == "7-day code challenge"
    assert item["phase"] == "active" and item["daysLeft"] == 7
    assert item["isOwner"] is True and item["memberCount"] == 1
    challenge_id = item["id"]
    assert item["invitePath"] == "/dashboard/challenges?join=" + challenge_id

    preview = harness.call("/api/challenges/" + challenge_id)
    assert preview.status == 200
    assert preview.data["challenge"]["isMember"] is False
    assert "board" not in preview.data["challenge"]
    assert preview.data["challenge"]["memberNames"] == ["alice"]

    joined = harness.call("/api/challenges/%s/join" % challenge_id, "POST",
                          {}, "bob")
    assert joined.status == 201
    again = harness.call("/api/challenges/%s/join" % challenge_id, "POST",
                         {}, "bob")
    assert again.status == 200

    harness.repo("public-repo")
    harness.repo("private-repo", private=1)
    harness.repo("stale-repo", generation="g2")
    today = "2026-09-21"
    harness.day("public-repo", "alice", today, commits=3, pulls=1)
    harness.day("public-repo", "bob", today, commits=9, reviews=1)
    harness.day("public-repo", "bob", "2026-09-20", commits=100)
    harness.day("private-repo", "alice", today, commits=500)
    harness.day("stale-repo", "alice", today, commits=500, generation="old")

    listed = harness.call("/api/challenges", user="bob")
    assert listed.status == 200
    assert listed.data["options"]["defaultPreset"] == "week"
    board = listed.data["challenges"][0]["board"]
    assert [(e["name"], e["total"], e["you"]) for e in board["entries"]] == [
        ("bob", 11, True), ("alice", 8, False)]
    assert board["entries"][0]["counts"] == {
        "commits": 9, "pulls": 0, "reviews": 1, "issues": 0}

    detail = harness.call("/api/challenges/" + challenge_id, user="alice")
    assert detail.data["challenge"]["isMember"] is True
    assert detail.data["challenge"]["board"]["entries"][1]["name"] == "alice"


def test_membership_rules(harness):
    created = harness.call("/api/challenges", "POST", {
        "name": "Duel", "metric": "commits", "days": 3, "maxMembers": 2,
    }, "alice").data["challenge"]
    path = "/api/challenges/" + created["id"]
    assert harness.call(path + "/join", "POST", {}, "bob").status == 201
    full = harness.call(path + "/join", "POST", {}, "carol")
    assert (full.status, full.data["error"]) == (409, "challenge_full")
    assert harness.call(path + "/leave", "POST", {}, "alice").data[
        "error"] == "owner_cannot_leave"
    assert harness.call(path + "/delete", "POST", {}, "bob").status == 403
    assert harness.call(path + "/leave", "POST", {}, "bob").status == 200
    assert harness.call(path + "/join", "POST", {}, "carol").status == 201
    assert harness.call(path + "/delete", "POST", {}, "alice").status == 200
    assert harness.call(path).status == 404
    assert harness.db.execute(
        "SELECT COUNT(*) FROM code_challenge_members").fetchone()[0] == 0

    harness.user("robot", kind="node")
    assert harness.call("/api/challenges", "POST", {"preset": "week"},
                        "robot").status == 401
    assert harness.call("/api/challenges", "POST", "broken",
                        "alice").status == 400
    assert harness.call("/api/challenges", "POST", {"name": ""},
                        "alice").data["error"] == "invalid_name"
    assert harness.call("/api/challenges", "DELETE", None,
                        "alice").status == 405
    assert harness.call("/api/challenges/nothex").status == 404


def test_finished_challenges_close_and_open_limits(harness):
    created = harness.call("/api/challenges", "POST", {
        "name": "Short", "days": 1}, "alice").data["challenge"]
    for _ in range(policy.MAX_OWNED_OPEN - 1):
        assert harness.call("/api/challenges", "POST", {"preset": "sprint"},
                            "alice").status == 201
    limited = harness.call("/api/challenges", "POST", {"preset": "sprint"},
                           "alice")
    assert (limited.status, limited.data["error"]) == (
        429, "too_many_owned_challenges")

    FakeDate.now_ms = NOW + DAY_MS
    late = harness.call("/api/challenges/%s/join" % created["id"], "POST",
                        {}, "bob")
    assert (late.status, late.data["error"]) == (409, "challenge_finished")
    listed = harness.call("/api/challenges", user="alice").data["challenges"]
    finished = [item for item in listed if item["id"] == created["id"]][0]
    assert finished["phase"] == "finished"
    assert finished["board"]["winners"] == []
    assert harness.call("/api/challenges", "POST", {"preset": "sprint"},
                        "alice").status == 201


def test_upcoming_challenge_has_no_scores_yet(harness):
    item = harness.call("/api/challenges", "POST", {
        "name": "Next week", "start": "2026-09-28", "days": 7,
    }, "alice").data["challenge"]
    assert item["phase"] == "upcoming"
    assert item["daysLeft"] == 7
    assert item["board"]["days"] == []
    assert item["board"]["entries"][0]["total"] == 0
    stored = json.loads(harness.db.execute(
        "SELECT data FROM code_challenges").fetchone()[0])
    assert set(stored) == {"name", "metric", "weights", "dailyCap",
                           "maxMembers"}
