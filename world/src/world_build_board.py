"""Shared public World build-board ordering with authorized audited writes."""

import re


PREFIX = "/api/world/build-board"
MAX_ITEMS = 64
BODY_LIMIT = 8 * 1024
KEY_RE = re.compile(r"^(?:task:[a-z0-9-]{1,48}|issue:[a-z0-9-]{1,40}/[a-z0-9._-]{1,60}#[1-9][0-9]{0,8})$")
BUILTIN_KEYS = frozenset({
    "task:world-stall-flood",
    "task:qa-full-catalog",
    "task:task-completion-note",
    "task:world-orb-hud",
    "task:avatar-hud-launcher",
    "task:fixed-square-hud-shortcuts",
    "task:avatar-selection-runtime",
    "task:member-circle-fire",
    "task:aquarium-fixed-controls",
    "task:recent-public-chat-card",
    "task:verification-pin-state",
    "task:office-floor-warps",
    "task:world-console-cleanup",
    "task:admin-record-detail",
    "task:member-verification-admin-link",
    "task:engineering-debug-panel",
    "task:compact-leaderboard",
    # The 5x5 wall this key named became three billboard circles of individual
    # cards. Both keys stay accepted: a stored board order still references the
    # old one, and dropping it would 409 every reorder that carries it.
    "task:leaderboard-grid",
    "task:billboard-circles",
    "task:reward-node-download",
    "task:world-ground-cleanup",
    "task:world-bike-groove",
    "task:world-record-detail",
    "task:world-node-delete-cleanup",
    "task:world-sky-hud",
    "task:repo-record-tower-scale",
    "task:world-primer-hud",
    "task:world-chat-composer",
    "task:world-member-path",
    "task:world-office-path",
    "task:world-circular-foundation",
    "task:world-frame-hot-loop",
    "task:web-pull-workbench",
    "task:session-audit",
    "task:world-board-detail",
    "task:world-mobile-pan-stability",
    "task:world-stable-hydration",
    "task:qt-execution-aware-stalls",
    "task:world-profile-social",
    "task:world-reactive-reef",
    "task:discord-human-authorization",
    "task:discord-public-chat-connector",
    "task:repo-code-landing",
    "task:repo-board-view-pads",
    "task:world-continuous-city",
    "task:world-start-here-map",
    "task:world-roof-and-seating",
    "task:world-beach-road",
    "task:world-bike-perimeter",
    "task:world-panel-layout",
    "task:world-github-theme",
    "task:world-time-stars-textures",
    "task:world-node-delete-regression",
    "task:todo-priority",
    "task:repo-issue-board",
    "task:saved-views",
    "task:blog-board-detail",
    "task:repo-circle-lists",
    "task:mirror-issue-intake",
    "task:mirror-status",
    "task:pr-57",
    "task:claude-agent",
    "task:claude-world",
    "task:codex-agent",
    "task:codex-world",
    "task:codex-board-done",
})


def _response(runtime, payload, status=200):
    return runtime.response(
        payload,
        status=status,
        cache_control="no-store, max-age=0, must-revalidate",
        extra_headers={"x-content-type-options": "nosniff"},
    )


def _issue_key(owner, repo, number):
    return "issue:%s/%s#%d" % (owner, repo, number)


async def _viewer(runtime, data=None):
    account_bi, record = await runtime.session(data or {})
    actor = str((record or {}).get("name") or "").strip().lower()
    if not account_bi or not actor:
        return "", "", False
    org_bi, organization = await runtime.organization()
    if not org_bi or not organization:
        return str(account_bi), actor, False
    role, permission = await runtime.membership(org_bi, actor)
    can_manage = (
        role in ("owner", "admin")
        or permission in ("maintain", "admin")
    )
    return str(account_bi), actor, can_manage


async def handle(runtime, path, issues=None):
    if str(path or "").rstrip("/") != PREFIX:
        return _response(runtime, {"error": "not_found"}, status=404)
    method = runtime.method()
    if method not in ("GET", "POST"):
        return _response(runtime, {"error": "method_not_allowed"}, status=405)
    await runtime.ensure_schema()
    data = {}
    if method == "POST":
        if not runtime.same_origin():
            return _response(runtime, {"error": "origin_not_allowed"}, status=403)
        data, error = await runtime.json_body(BODY_LIMIT)
        if error:
            return _response(
                runtime,
                {"error": error},
                status=413 if error == "payload_too_large" else 400,
            )
    account_bi, actor, can_manage = await _viewer(runtime, data)
    if method == "POST" and not account_bi:
        return _response(runtime, {"error": "invalid_session"}, status=401)
    if method == "POST" and not can_manage:
        return _response(runtime, {"error": "forbidden"}, status=403)

    available_issues = {}
    for issue in (issues or [])[:32]:
        try:
            number = int(issue.get("number") or 0)
        except (TypeError, ValueError):
            continue
        owner = str(issue.get("owner") or "").lower()
        repo = str(issue.get("repo") or "").lower()
        key = _issue_key(owner, repo, number)
        if KEY_RE.fullmatch(key):
            available_issues[key] = {
                "key": key,
                "owner": owner,
                "repo": repo,
                "number": number,
                "title": str(issue.get("title") or ("Issue #%d" % number))[:160],
                "status": str(issue.get("status") or "open")[:20],
            }

    if method == "POST":
        action = str(data.get("action") or "")
        if action == "assign":
            key = str(data.get("key") or "")
            match = re.fullmatch(
                r"issue:forkmesh/forkmesh#([1-9][0-9]{0,8})", key)
            title = " ".join(str(data.get("title") or "").split())[:160]
            if not match or not title:
                return _response(runtime, {"error": "issue_not_available"}, status=409)
            issue = {
                "key": key,
                "owner": "forkmesh",
                "repo": "forkmesh",
                "number": int(match.group(1)),
                "title": title,
            }
            count = await runtime.d1_first(
                "SELECT COUNT(*) AS count FROM world_build_board_items")
            if int((count or {}).get("count") or 0) >= MAX_ITEMS:
                return _response(runtime, {"error": "board_full"}, status=409)
            peak = await runtime.d1_first(
                "SELECT COALESCE(MAX(priority),0) AS priority "
                "FROM world_build_board_items")
            priority = min(MAX_ITEMS, int((peak or {}).get("priority") or 0) + 1)
            await runtime.d1_run(
                "INSERT INTO world_build_board_items "
                "(item_key,kind,owner,repo,issue_number,title,priority,updated_by_bi,"
                "updated_at,completed_at,completed_by_bi) "
                "VALUES (?,?,?,?,?,?,?,?,?,0,'') "
                "ON CONFLICT(item_key) DO UPDATE SET priority=excluded.priority,"
                "title=excluded.title,updated_by_bi=excluded.updated_by_bi,"
                "updated_at=excluded.updated_at,completed_at=0,completed_by_bi=''",
                key, "issue", issue["owner"], issue["repo"], issue["number"],
                issue["title"], priority, account_bi, int(runtime.now()))
        elif action == "send_qa":
            key = str(data.get("key") or "")
            title = " ".join(str(data.get("title") or "").split())[:160]
            how_to_test = " ".join(
                str(data.get("howToTest") or "").split())[:720]
            if (
                not KEY_RE.fullmatch(key)
                or not title
                or not how_to_test
            ):
                return _response(
                    runtime, {"error": "invalid_qa_item"}, status=400)
            now = int(runtime.now())
            kind = "issue" if key.startswith("issue:") else "task"
            await runtime.d1_run(
                "INSERT INTO world_build_board_items "
                "(item_key,kind,owner,repo,issue_number,title,priority,"
                "updated_by_bi,updated_at,completed_at,completed_by_bi) "
                "VALUES (?,?, '', '',0,?,64,?,?,?,?) "
                "ON CONFLICT(item_key) DO UPDATE SET "
                "title=excluded.title,updated_by_bi=excluded.updated_by_bi,"
                "updated_at=excluded.updated_at,"
                "completed_at=excluded.completed_at,"
                "completed_by_bi=excluded.completed_by_bi",
                key, kind, title, account_bi, now, now, account_bi,
            )
            await runtime.d1_run(
                "INSERT INTO world_qa_items("
                "item_key,title,how_to_test,source_key,added_at,active) "
                "VALUES(?,?,?,?,?,1) ON CONFLICT(item_key) DO UPDATE SET "
                "title=excluded.title,how_to_test=excluded.how_to_test,"
                "source_key=excluded.source_key,added_at=excluded.added_at,"
                "active=1",
                key, title, how_to_test, key, now,
            )
        elif action == "reorder":
            order = data.get("order")
            if not isinstance(order, list) or not order or len(order) > MAX_ITEMS:
                return _response(runtime, {"error": "invalid_order"}, status=400)
            order = [str(key or "") for key in order]
            if len(set(order)) != len(order) or any(
                    not KEY_RE.fullmatch(key) for key in order):
                return _response(runtime, {"error": "invalid_order"}, status=400)
            existing = await runtime.d1_all(
                "SELECT item_key,kind,owner,repo,issue_number,title "
                "FROM world_build_board_items")
            known = {row["item_key"]: row for row in (existing or [])}
            if any(key not in BUILTIN_KEYS and key not in known for key in order):
                return _response(runtime, {"error": "unknown_item"}, status=409)
            now = int(runtime.now())
            for priority, key in enumerate(order, 1):
                row = known.get(key) or {}
                await runtime.d1_run(
                    "INSERT INTO world_build_board_items "
                    "(item_key,kind,owner,repo,issue_number,title,priority,updated_by_bi,"
                    "updated_at,completed_at,completed_by_bi) "
                    "VALUES (?,?,?,?,?,?,?,?,?,0,'') "
                    "ON CONFLICT(item_key) DO UPDATE SET priority=excluded.priority,"
                    "updated_by_bi=excluded.updated_by_bi,updated_at=excluded.updated_at",
                    key, row.get("kind") or "task", row.get("owner") or "",
                    row.get("repo") or "", int(row.get("issue_number") or 0),
                    str(row.get("title") or "")[:160],
                    priority, account_bi, now)
        else:
            return _response(runtime, {"error": "invalid_action"}, status=400)
        await runtime.audit(
            actor,
            "world.build_board_" + action,
            "world_build_board",
            str(data.get("key") or "order")[:96],
            details={"count": len(data.get("order") or [])},
        )

    rows = await runtime.d1_all(
        "SELECT item_key,kind,owner,repo,issue_number,title,priority,updated_at,"
        "completed_at "
        "FROM world_build_board_items ORDER BY priority,item_key LIMIT ?",
        MAX_ITEMS,
    )
    active_rows = [
        row for row in (rows or []) if int(row.get("completed_at") or 0) <= 0
    ]
    completed_rows = [
        row for row in (rows or []) if int(row.get("completed_at") or 0) > 0
    ]
    assigned = {str(row.get("item_key") or "") for row in active_rows}
    assigned_issues = []
    for row in active_rows:
        if str(row.get("kind") or "") != "issue":
            continue
        key = str(row.get("item_key") or "")
        assigned_issues.append(available_issues.get(key) or {
            "key": key,
            "owner": str(row.get("owner") or ""),
            "repo": str(row.get("repo") or ""),
            "number": int(row.get("issue_number") or 0),
            "title": str(row.get("title") or "Repository issue")[:160],
            "status": "assigned",
        })
    return _response(runtime, {
        "ok": True,
        "canManage": bool(can_manage),
        "order": [str(row.get("item_key") or "") for row in active_rows],
        "completedKeys": [
            str(row.get("item_key") or "") for row in completed_rows
        ],
        "assignedIssues": assigned_issues,
        "customTasks": [
            {
                "key": str(row.get("item_key") or ""),
                "title": str(row.get("title") or "QA follow-up")[:160],
            }
            for row in active_rows
            if str(row.get("kind") or "") == "task"
            and str(row.get("item_key") or "") not in BUILTIN_KEYS
        ],
        "issues": [
            {**issue, "assigned": key in assigned}
            for key, issue in available_issues.items()
        ],
    })
