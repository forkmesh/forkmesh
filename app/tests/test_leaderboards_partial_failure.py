#!/usr/bin/env python3
"""One broken source must not take the whole leaderboard hub down.

adhoc #1205: GET /api/leaderboards opened a 500 error group carrying
TypeError('_account_chat_user_payload() takes from 1 to 3 positional
arguments but 4 were given') — the same failure adhoc #225 had already
logged under /api/accounts/users. That arity mismatch is fixed and gated
by test_entry_call_arity.py, but the second error group is the more
interesting half of the story: the fault was in the users directory, and
it took out fourteen boards that never read a user row.

leaderboards_overview reads several independent public sources one at a time.
The earlier gather-based implementation re-raised the first exception, so any
one source failing returned a 500 for the whole endpoint — and that endpoint
is the only one both the website grid and the World island read. This test
pins the isolation: a failed source costs its own boards, the rest still
render, the gap is named in the payload rather than passed off as an empty
board, and the failure is reported instead of swallowed.

AST-extraction harness in the style of test_error_group_notifications.py.
"""

import ast
import asyncio
from pathlib import Path


ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
ENTRY_TEXT = ENTRY.read_text(encoding="utf-8")
CLIENT_TEXT = (
    Path(__file__).resolve().parents[1] / "public" / "leaderboards.js"
).read_text(encoding="utf-8")

FUNCS = {
    "leaderboards_overview",
    "_world_public_total_active_ms",
}

# Every source name leaderboards_overview fans out to, and boards that depend
# on it. The reporter board also requires the public user roster to filter out
# bot/node/external identities.
SOURCE_BOARDS = {
    "network": ("uptime",),
    "referrals": ("referrals",),
    "sites": ("referring-sites",),
    "users": ("activity", "issue-reporters"),
    "issue-reporters": ("issue-reporters",),
    "wallets": ("wallets",),
    "tips": ("tip-recipients",),
}


class _Date:
    @staticmethod
    def now():
        return 1700000000000


def _load(extra):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in FUNCS
    ]
    found = {node.name for node in selected}
    assert found == FUNCS, "missing: %s" % sorted(FUNCS - found)
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))
    namespace = dict(extra)
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


def _rows(name):
    return [{"name": name + "-one", "value": 2}, {"name": name + "-two"}]


def _harness(broken=()):
    """Stub the five sources; the named ones raise the way a real fault does."""
    reports = []

    def _source(name, payload):
        async def call(*_args, **_kwargs):
            if name in broken:
                raise TypeError(name + " takes 3 positional arguments")
            return dict(payload)
        return call

    async def _response_json(response):
        return response if isinstance(response, dict) else {}

    async def capture_sentry_error(_env, status, method, path, message,
                                   **kwargs):
        reports.append({
            "status": status, "method": method, "path": path,
            "message": message, "error": kwargs.get("error"),
        })
        return True

    captured = {}

    def json_response(data, status=200, cache_seconds=None, **_kwargs):
        captured["data"] = data
        captured["status"] = status
        captured["cache_seconds"] = cache_seconds
        return captured

    namespace = {
        "asyncio": asyncio,
        "Date": _Date,
        "MAX_NODE_NAME": 63,
        "LEADERBOARD_LIMIT": 10,
        "NETWORK_STATS_TTL": 20,
        "WORLD_USER_ACTIVITY_MAX_TOTAL_MS": 9_000_000_000_000_000,
        "WORLD_USER_ACTIVITY_PUBLIC_BUCKET_MS": 60 * 1000,
        "clean_string": lambda value, limit=240: str(value or "")[:limit],
        "quote": lambda value: str(value),
        "_clean_mastodon_handle": lambda value: str(value or ""),
        "_mastodon_url": lambda value: (
            "https://mastodon.example/@" + str(value).lstrip("@")
            if value else ""),
        "_ap_origin": lambda _env: "https://forkmesh.com",
        "_response_json": _response_json,
        "capture_sentry_error": capture_sentry_error,
        "_safe_error_text": lambda error: str(error),
        "json_response": json_response,
        "network_leaderboards": _source("network", {
            "windowHours": 48,
            "uptime": _rows("uptime"),
            "nodes": _rows("nodes"),
            "repos": _rows("repos"),
            "mirrors": _rows("mirrors"),
            "hosted": _rows("hosted"),
            "largest": _rows("largest"),
            "dataHosted": _rows("data"),
            "contributors": _rows("contributors"),
            "fundsMainnodes": _rows("funds"),
            "fundsContributors": _rows("funds"),
            "fundsProjects": _rows("funds"),
        }),
        "referral_leaderboard": _source(
            "referrals", {"board": _rows("referrals")}),
        "site_referrer_leaderboard": _source(
            "sites", {"board": _rows("sites")}),
        "_account_users_directory": _source("users", {"users": [
            {"name": "ada", "totalActiveMs": 600000, "activityBucket": "hot"},
            {"name": "grace", "totalActiveMs": 120000},
        ]}),
        "issue_reporter_leaderboard": _source(
            "issue-reporters", {"counts": {"ada": 3, "grace": 1}}),
        "wallet_leaderboard": _source(
            "wallets", {"board": _rows("wallets")}),
        "tip_recipient_leaderboard": _source(
            "tips", {"board": _rows("tips")}),
    }
    return _load(namespace)["leaderboards_overview"], reports


def _overview(broken=()):
    overview, reports = _harness(broken)
    return asyncio.run(overview(object())), reports


def _by_id(payload):
    return {board["id"]: board for board in payload["data"]["boards"]}


def test_every_board_is_populated_when_no_source_fails():
    payload, reports = _overview()
    boards = _by_id(payload)
    assert payload["data"]["ok"] is True
    assert payload["data"]["degraded"] == []
    assert payload["status"] == 200
    for board in boards.values():
        assert board["rows"], board["id"]
        assert board["degraded"] is False, board["id"]
    # A healthy rebuild must not manufacture telemetry.
    assert reports == []


def test_one_broken_source_costs_only_its_dependent_boards():
    for source, board_ids in SOURCE_BOARDS.items():
        payload, _reports = _overview(broken=(source,))
        boards = _by_id(payload)
        unavailable = set(board_ids)
        # Still a 200 with the full board list — the client renders the
        # survivors instead of the whole grid falling back to "unavailable".
        assert payload["status"] == 200, source
        assert payload["data"]["ok"] is True, source
        assert len(boards) == 17, source
        for board_id in unavailable:
            assert boards[board_id]["rows"] == [], source
        # Every board belonging to a healthy source still carries its rows.
        for other, other_board_ids in SOURCE_BOARDS.items():
            if other != source:
                for board_id in set(other_board_ids) - unavailable:
                    assert boards[board_id]["rows"], (source, other)


def test_a_failed_source_is_named_rather_than_read_as_empty():
    for source, board_ids in SOURCE_BOARDS.items():
        payload, _reports = _overview(broken=(source,))
        boards = _by_id(payload)
        unavailable = set(board_ids)
        assert payload["data"]["degraded"] == [source], source
        for board_id in unavailable:
            assert boards[board_id]["degraded"] is True, source
        for other, other_board_ids in SOURCE_BOARDS.items():
            if other != source:
                for board_id in set(other_board_ids) - unavailable:
                    assert boards[board_id]["degraded"] is False, (source, other)


def test_a_failed_source_is_reported_to_sentry():
    _payload, reports = _overview(broken=("users",))
    assert len(reports) == 1
    report = reports[0]
    # Workers Logs is off worker-wide, so Sentry is the only place a degraded
    # board can surface. Named source + real exception, at error level.
    assert report["path"] == "/api/leaderboards"
    assert report["status"] >= 500
    assert "users" in report["message"]
    assert isinstance(report["error"], TypeError)


def test_telemetry_failure_cannot_sink_the_response():
    overview, _reports = _harness(broken=("wallets",))

    async def explode(*_args, **_kwargs):
        raise RuntimeError("sentry unreachable")

    overview.__globals__["capture_sentry_error"] = explode
    payload = asyncio.run(overview(object()))
    assert payload["status"] == 200
    assert payload["data"]["degraded"] == ["wallets"]


def test_the_hub_page_tells_the_two_empty_boards_apart():
    # The flag is only worth carrying if a visitor can see the difference.
    assert "board.degraded" in CLIENT_TEXT
    assert '"Temporarily unavailable."' in CLIENT_TEXT
    assert '"No public data yet."' in CLIENT_TEXT


def test_every_source_failing_still_answers():
    payload, reports = _overview(broken=tuple(SOURCE_BOARDS))
    boards = _by_id(payload)
    assert payload["status"] == 200
    assert sorted(payload["data"]["degraded"]) == sorted(SOURCE_BOARDS)
    assert len(boards) == 17
    assert all(board["rows"] == [] for board in boards.values())
    assert all(board["degraded"] for board in boards.values())
    assert len(reports) == len(SOURCE_BOARDS)
