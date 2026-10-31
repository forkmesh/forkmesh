#!/usr/bin/env python3
"""Account-private one-card World QA bulletin contracts."""

import ast
import asyncio
import json
import re
import sqlite3
from pathlib import Path
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
ENTRY_PATH = ROOT / "src" / "entry.py"
WORLD_QA_PATH = ROOT.parent / "world" / "src" / "world_qa.py"
# The QA deck/handler live in the on-demand world_qa module (kept out of
# Worker startup); the board's contracts span both files.
ENTRY = (
    ENTRY_PATH.read_text(encoding="utf-8") + "\n"
    + WORLD_QA_PATH.read_text(encoding="utf-8")
)
SCHEMA = (ROOT / "src" / "schema.py").read_text(encoding="utf-8")
WORLD = (ROOT.parent / "world" / "public" / "world" / "world.js").read_text(encoding="utf-8")
SCENE = world_scene_source()
CSS = (ROOT.parent / "world" / "public" / "world" / "world.css").read_text(encoding="utf-8")
MIGRATION = ROOT / "migrations" / "0098_world_qa_reviews.sql"
ITEM_MIGRATION = ROOT / "migrations" / "0099_world_qa_items.sql"


def _run(awaitable):
    return asyncio.run(awaitable)


class _Date:
    @staticmethod
    def now():
        return 1_700_000_000_000


class _Request:
    def __init__(self, method="GET", data=None, account="bi:alice",
                 same_origin=True):
        self.method = method
        self.data = data or {}
        self.account = account
        self.same_origin = same_origin


def _handler_runtime(task_rows=None):
    tree = ast.parse(ENTRY, filename=str(ENTRY_PATH))
    wanted_assignments = {
        "WORLD_QA_DECK_REVISION", "WORLD_QA_MAX_CARDS",
        "WORLD_QA_CARDS", "WORLD_QA_CARD_KEYS",
    }
    selected = []
    for node in tree.body:
        if isinstance(node, ast.Assign):
            names = {
                target.id for target in node.targets
                if isinstance(target, ast.Name)
            }
            if names & wanted_assignments:
                selected.append(node)
        elif isinstance(node, ast.AsyncFunctionDef) \
                and node.name == "world_qa_handler":
            selected.append(node)
    assert any(
        isinstance(node, ast.AsyncFunctionDef) for node in selected)
    rows = {}

    async def ensure_schema(_env):
        return None

    async def bounded_json_request(request, _limit):
        return request.data

    async def account_session(_env, request, _data):
        if not request.account:
            return None, None
        return request.account, {"name": request.account.split(":")[-1]}

    async def org_row(_env, _org):
        return "org:forkmesh", {"name": "forkmesh"}

    async def org_role(_env, _org_bi, actor):
        return "owner" if actor == "alice" else "member"

    async def org_permission(_env, _org_bi, actor):
        return "admin" if actor == "alice" else "read"

    async def d1_run(_env, _sql, account_bi, key, verdict, reviewed_at):
        rows[(account_bi, key)] = {
            "item_key": key,
            "verdict": verdict,
            "reviewed_at": reviewed_at,
        }

    async def d1_all(_env, sql, *params):
        if "FROM world_qa_items" in sql:
            return []
        if "FROM organization_tasks" in sql:
            return list(task_rows or [])
        if "FROM organization_task_qa_reviews" in sql:
            return []
        if "GROUP BY item_key,verdict" in sql:
            totals = {}
            for value in rows.values():
                pair = (value["item_key"], value["verdict"])
                totals[pair] = totals.get(pair, 0) + 1
            return [
                {"item_key": key, "verdict": verdict, "count": count}
                for (key, verdict), count in totals.items()
            ]
        if "COUNT(DISTINCT account_bi)" in sql:
            return [{"testers": len({owner for owner, _key in rows})}]
        account_bi = params[0]
        return [
            dict(value)
            for (owner, _key), value in rows.items()
            if owner == account_bi
        ]

    async def d1_first(_env, sql, *params):
        if "FROM org_team_members" in sql:
            return {"one": 1} if params[-1] == "bi:bob" else None
        if "FROM org_team_collaborators" in sql:
            return {"one": 1} if params[-1] == "bi:eve" else None
        return {"priority": 0}

    def response(payload, status=200, **_kwargs):
        return {"payload": payload, "status": status}

    class RequestBodyTooLarge(BaseException):
        pass

    namespace = {
        "Date": _Date,
        "MAX_NODE_NAME": 80,
        "RequestBodyTooLarge": RequestBodyTooLarge,
        "re": re,
        "ensure_schema": ensure_schema,
        "method_name": lambda request: request.method,
        "_request_same_origin": lambda request: request.same_origin,
        "bounded_json_request": bounded_json_request,
        "_account_session_record": account_session,
        "_org_row": org_row,
        "_org_role": org_role,
        "_org_permission": org_permission,
        "d1_run": d1_run,
        "d1_all": d1_all,
        "d1_first": d1_first,
        "decrypt_row": lambda _env, value: _async_value(value),
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "json_response": response,
    }
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[])),
        str(ENTRY_PATH),
        "exec",
    ), namespace)
    return namespace["world_qa_handler"], rows


async def _async_value(value):
    return value


def test_qa_schema_is_account_scoped_and_bounded():
    db = sqlite3.connect(":memory:")
    migration = MIGRATION.read_text(encoding="utf-8")
    db.executescript(migration)
    db.executescript(migration)
    columns = {
        row[1] for row in db.execute("PRAGMA table_info(world_qa_reviews)")
    }
    assert columns == {"account_bi", "item_key", "verdict", "reviewed_at"}
    assert "PRIMARY KEY (account_bi, item_key)" in migration
    assert "username" not in migration.lower()
    assert "ip" not in migration.lower().split("create table", 1)[1]
    assert "world_qa_reviews" in SCHEMA
    item_sql = ITEM_MIGRATION.read_text(encoding="utf-8")
    assert "CREATE TABLE IF NOT EXISTS world_qa_items" in item_sql
    assert "how_to_test TEXT NOT NULL" in item_sql
    assert "world_qa_items" in SCHEMA


def test_qa_votes_are_per_account_with_global_aggregate_totals():
    handler, rows = _handler_runtime()
    anonymous = _run(handler(None, _Request(account="")))
    assert anonymous["status"] == 200
    assert anonymous["payload"]["authenticated"] is False
    assert anonymous["payload"]["reviews"] == {}
    assert anonymous["payload"]["authorized"] is False
    assert anonymous["payload"]["requiredTeam"] == ""
    assert anonymous["payload"]["requiresAuthentication"] is True
    assert anonymous["payload"]["cards"] == []

    saved = _run(handler(None, _Request(
        method="POST",
        account="bi:alice",
        data={"key": "office-doorway", "verdict": "pass"},
    )))
    assert saved["status"] == 200
    assert saved["payload"]["authenticated"] is True
    assert saved["payload"]["reviews"]["office-doorway"]["verdict"] == "pass"
    assert saved["payload"]["stats"]["pass"] == 1
    assert saved["payload"]["stats"]["reviewed"] == 1
    assert saved["payload"]["globalStats"]["pass"] == 1
    assert saved["payload"]["globalStats"]["testers"] == 1
    assert saved["payload"]["globalReviews"]["office-doorway"] == {
        "pass": 1, "fail": 0, "unsure": 0, "total": 1,
    }
    assert ("bi:alice", "office-doorway") in rows

    bob = _run(handler(None, _Request(account="bi:bob")))
    assert bob["payload"]["reviews"] == {}
    assert bob["payload"]["stats"]["reviewed"] == 0
    assert bob["payload"]["globalStats"]["pass"] == 1
    reviewed = next(
        card for card in bob["payload"]["cards"]
        if card["key"] == "office-doorway"
    )
    assert reviewed["reviewedByAnyone"] is True

    non_qa = _run(handler(None, _Request(account="bi:charlie")))
    assert non_qa["payload"]["authenticated"] is True
    assert non_qa["payload"]["authorized"] is True
    assert non_qa["payload"]["cards"]
    assert non_qa["payload"]["canViewPrivateTasks"] is True
    assert non_qa["payload"]["requiredTeam"] == ""
    assert non_qa["payload"]["requiresAuthentication"] is True


def test_qa_write_rejects_cross_origin_unknown_items_and_bad_verdicts():
    handler, rows = _handler_runtime()
    cross_origin = _run(handler(None, _Request(
        method="POST",
        same_origin=False,
        data={"key": "office-doorway", "verdict": "pass"},
    )))
    assert cross_origin["status"] == 403
    for data in (
        {"key": "invented-task", "verdict": "pass"},
        {"key": "office-doorway", "verdict": "perfect"},
    ):
        response = _run(handler(None, _Request(method="POST", data=data)))
        assert response["status"] == 400
    assert rows == {}


def test_logged_in_non_qa_user_can_review_a_card():
    handler, rows = _handler_runtime()
    response = _run(handler(None, _Request(
        method="POST",
        account="bi:charlie",
        data={"key": "office-doorway", "verdict": "unsure"},
    )))
    assert response["status"] == 200
    assert response["payload"]["authorized"] is True
    assert response["payload"]["reviews"]["office-doorway"][
        "verdict"] == "unsure"
    assert rows[("bi:charlie", "office-doorway")]["verdict"] == "unsure"

    route = _run(handler(None, _Request(
        method="POST",
        account="bi:charlie",
        data={"key": "office-doorway", "action": "route_issue"},
    )))
    assert route["status"] == 403
    assert route["payload"]["error"] == "forbidden"


def test_completed_organization_task_is_marked_globally_reviewed():
    task_id = "a" * 32
    handler, _rows = _handler_runtime([{
        "task_id": task_id,
        "department": "Engineering",
        "team": "quality-assurance",
        "qa_status": "passed",
        "qa_reviewed_at": 1_699_999_999_000,
        "data": {
            "title": "Already checked task",
            "howToTest": "Confirm the completed behavior.",
            "qaReviewer": "bob",
        },
    }])
    response = _run(handler(None, _Request(account="bi:alice")))
    card = next(
        item for item in response["payload"]["cards"]
        if item["key"] == f"task:{task_id}"
    )
    assert card["reviewedByAnyone"] is True
    assert card["global"] == {
        "pass": 1, "fail": 0, "unsure": 0, "total": 1,
    }
    assert response["payload"]["globalStats"]["pass"] == 1


def test_world_has_one_direct_physical_card_with_swipes_and_stats():
    for contract in (
        'const WORLD_QA_ENDPOINT = "/api/world/qa"',
        "async refreshQaDeck(",
            "async recordQaVerdict(verdict)",
            "onQaVerdict: ({ verdict }) => void this.recordQaVerdict(verdict)",
            "currentIndex: this.qaCardIndex",
            "globalStats: this.qaDeck.globalStats",
            "handleQaAction(detail = {})",
            "routeQaCard(target)",
            'action: "route_issue"',
    ):
        assert contract in WORLD
    for contract in (
        "function worldQaCardTexture(",
        'worldQaBoard.name = "forkmesh-world-qa-board"',
        'qaBoardFace.userData.interactive = "world-qa-board"',
        'arrow: "←", label: "FAIL"',
        'arrow: "→", label: "PASS"',
        'arrow: "↓", label: "UNSURE"',
        'color: "#ff3f46"',
        'color: "#2ea043"',
        'color: "#aeb7b2"',
        "qaSwipeCues.visible = true",
        'verdict = deltaX > 0 ? "pass" : "fail"',
        'verdict = "unsure"',
        "onQaVerdict({ verdict })",
        "updateQaBoard,",
        "HOW TO TEST",
        "function qaBoardHitAction(",
        'const views = ["cards", "pass", "fail", "unsure"]',
        'return { action: "route", target: "issues" }',
        "SEND TO ISSUES",
        "QA RESULT DETAIL · REVIEW OR RETURN",
        "BACK TO CARDS",
        'return { action: "verdict", verdict: "pass" }',
        'return { action: "back" }',
        "onQaAction(qaAction)",
    ):
        assert contract in SCENE
    assert "SEND TO TODO" not in SCENE
    assert 'target: "todo"' not in SCENE
    assert "route_todo" not in WORLD
    assert "renderQaBoardPanel" not in WORLD
    assert "data-world-qa-verdict" not in WORLD
    assert ".world-qa-playing-card" not in CSS


def test_cards_stack_only_deals_tasks_that_nobody_has_reviewed():
    for contract in (
        '"reviewedByAnyone": (',
        'int(global_reviews.get(key, {}).get("total") or 0) > 0',
        "include_private_task_reviews(global_reviews, global_stats)",
    ):
        assert contract in ENTRY
    for contract in (
        "const stack = cards.filter((card) => !card.reviewedByAnyone);",
        "stack,",
        "total: stack.length,",
        "this.qaDeck.stack[this.qaCardIndex]",
        "Math.min(previousStackIndex, stack.length - 1)",
    ):
        assert contract in WORLD
    assert "NO QA TASKS ARE WAITING" in SCENE
    assert "SIGN IN TO WORK ON QA" in SCENE
    assert "CARDS WAITING" in SCENE


def test_shared_qa_deck_refreshes_while_the_visitor_stands_at_the_board():
    for contract in (
        "const WORLD_QA_POLL_MS = 15 * 1000;",
        "this.qaTimer = window.setInterval(() => {",
        "void this.refreshQaDeck({ quiet: true });",
        "}, WORLD_QA_POLL_MS);",
        "window.clearInterval(this.qaTimer);",
    ):
        assert contract in WORLD
    # A fifteen-second cadence is only affordable because it is scoped to a
    # visitor actually standing at the board — it must start on approach and
    # be cleared on departure, never run for the life of the page.
    assert 'placeBillboardOnIsland(worldQaBoard, {\n    circle: "hub",\n    feed: "qa-board",' in SCENE
    assert "onBillboardNearby({ feed: record.feed, refetch });" in SCENE
    assert "onBillboardAway({ feed: record.feed });" in SCENE
    assert 'if (feed === "qa-board") {\n      this.startQaDeckWatch({ refetch });' in WORLD
    assert 'if (feed === "qa-board") this.stopQaDeckWatch();' in WORLD
    watch = WORLD[
        WORLD.index("  startQaDeckWatch("):
        WORLD.index("  stopQaDeckWatch() {") + 120
    ]
    assert "}, WORLD_QA_POLL_MS);" in watch
    assert "window.clearInterval(this.qaTimer);" in watch
    visibility = WORLD[
        WORLD.index("handleVisibility = () => {"):
        WORLD.index("handleStorage = (event) => {")
    ]
    assert "void this.refreshQaDeck({ quiet: true });" in visibility


def test_qa_catalog_is_not_truncated_to_the_first_64_cards():
    handler = ENTRY[ENTRY.index("async def world_qa_handler"):]
    assert "WORLD_QA_MAX_CARDS = 4096" in ENTRY
    assert "deck_cards = deck_cards[:64]" not in handler
    assert "deck_cards = deck_cards[:128]" not in handler
    assert "LIMIT 64" not in handler
    assert handler.count("WORLD_QA_MAX_CARDS") >= 6
    qa_client = WORLD[
        WORLD.index("applyQaDeck(payload"):
        WORLD.index("qaDeckCardsForView(")
    ]
    assert ".slice(0, 64)" not in qa_client
    assert ".slice(0, 4096)" in qa_client
    # The 3D desk remains constant-cost even when the catalog grows.
    assert "const pageSize = 5;" in WORLD
    assert "Math.ceil(filtered.length / pageSize)" in WORLD


def test_private_task_failures_require_a_reason_and_accept_one_screenshot():
    handler = ENTRY[ENTRY.index("async def world_qa_handler"):]
    for contract in (
        '"failure_reason_required"',
        '"invalid_qa_screenshot"',
        "len(failure_screenshot) > 480_000",
        '"qaFailureReason"',
        '"qaFailureScreenshot"',
        '"hasScreenshot": bool(failure_screenshot)',
    ):
        assert contract in handler
    for contract in (
        "collectQaFailureEvidence(card)",
        "compactQaFailureScreenshot(file)",
        'accept="image/png,image/jpeg,image/webp"',
        'placeholder="Describe what failed, what you expected, and how to reproduce it."',
        "failureReason: evidence.failureReason",
        "screenshot: evidence.screenshot",
    ):
        assert contract in WORLD
    assert ".world-qa-failure-dialog" in CSS
    assert "FAIL REASON" in SCENE


def test_qa_routing_is_privileged_audited_and_targets_real_work_queues():
    handler = ENTRY[ENTRY.index("async def world_qa_handler"):]
    for contract in (
        'action in ("route_todo", "route_issue")',
        "if not can_route:",
        "INSERT INTO world_build_board_items",
        "completed_at=0",
        "_forkbot_enqueue_issue(",
        '"forkmesh",',
        '"QA follow-up: " + title',
        '"world.qa_" + action',
    ):
        assert contract in handler
    board_api = (ROOT.parent / "world" / "src" / "world_build_board.py").read_text(
        encoding="utf-8")
    assert '"customTasks": [' in board_api


def test_build_board_api_can_still_send_completed_tasks_to_qa():
    for contract in (
        'action == "send_qa"',
        "INSERT INTO world_qa_items(",
        "how_to_test=excluded.how_to_test",
    ):
        assert contract in (
            ROOT.parent / "world" / "src" / "world_build_board.py"
        ).read_text(encoding="utf-8")


def test_share_and_saved_views_live_in_the_fixed_square_right_rail():
    assert "data-world-share-menu" not in WORLD
    assert "data-world-share-current" in WORLD
    assert "shareCurrentWorldView()" in WORLD
    assert "data-world-saved-views-toggle" not in WORLD
    assert 'data-expanded="true"' in WORLD
    assert "Share view" in WORLD
    assert "Remember" in WORLD
    assert "setSavedViewsExpanded(expanded)" in WORLD
    assert '<div class="world-saved-view-list" data-world-saved-view-list>' in WORLD
    fixed_rail = CSS.rsplit("/* Fixed launcher geometry.", 1)[1]
    assert "grid-template-columns: 48px;" in fixed_rail
    assert ".world-saved-view-list:empty" in fixed_rail
    # The ordinary right-click menu is untouched.
    assert 'renderer.domElement.addEventListener("contextmenu"' not in SCENE
    assert ".world-share-view-button" in CSS
