import json
import re
from datetime import datetime, timezone

import challenges as policy


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


PREFIX = "/api/challenges"
BODY_MAX_BYTES = 4 * 1024
LIST_LIMIT = 30
PAGE_PATH = "/dashboard/challenges"
DETAIL_RE = re.compile(
    r"^/api/challenges/([0-9a-f]{32})(?:/(join|leave|delete))?/?$")
SCORE_SQL = """WITH members(user_bi) AS (
       SELECT CAST(value AS TEXT) FROM json_each(?)
     ), ranked_sources AS (
       SELECT projects.source_repo_bi,
              projects.active_generation_bi,
              ROW_NUMBER() OVER (
                PARTITION BY projects.project_bi
                ORDER BY projects.captured_at DESC,
                         projects.source_repo_bi ASC) AS source_rank
         FROM profile_contribution_projects AS projects
         JOIN repositories
           ON repositories.key_bi=projects.source_repo_bi
        WHERE projects.is_public=1 AND repositories.is_private=0
          AND projects.active_generation_bi IS NOT NULL
     ), selected_sources AS (
       SELECT * FROM ranked_sources WHERE source_rank=1
     )
     SELECT days.subject_user_bi AS user_bi, days.day AS day,
            SUM(days.commits) AS commits, SUM(days.pulls) AS pulls,
            SUM(days.reviews) AS reviews, SUM(days.issues) AS issues
       FROM selected_sources AS sources
       JOIN profile_contribution_days AS days
         ON days.source_repo_bi=sources.source_repo_bi
        AND days.generation_bi=sources.active_generation_bi
      WHERE days.subject_user_bi IN (SELECT user_bi FROM members)
        AND days.day>=? AND days.day<=?
      GROUP BY days.subject_user_bi, days.day"""


def _now():
    return int(Date.now())


def _today(now):
    return datetime.fromtimestamp(now / 1000, timezone.utc).date()


def _error(code, status):
    return json_response({"ok": False, "error": code}, status=status,
                         cache_control="no-store")


def _ok(payload, status=200):
    return json_response(dict(payload, ok=True), status=status,
                         cache_control="no-store")


async def _session_account(env, request, data=None):
    try:
        name_bi, rec = await _account_session_record(env, request, data)
    except RuntimeError:
        return "", ""
    if not name_bi or not rec:
        return "", ""
    if rec.get("status") != "active" or _account_kind(rec) != "user":
        return "", ""
    return name_bi, str(rec.get("name") or "").strip().lower()[:64]


def _challenge(row):
    try:
        data = json.loads(row.get("data") or "{}")
    except (TypeError, ValueError):
        data = {}
    return {
        "id": row.get("challenge_id") or "",
        "ownerBi": row.get("owner_bi") or "",
        "ownerName": row.get("owner_name") or "",
        "startsOn": row.get("starts_on") or "",
        "endsOn": row.get("ends_on") or "",
        "createdAt": int(row.get("created_at") or 0),
        "settings": policy.stored_settings(data),
    }


async def _challenge_by_id(env, challenge_id):
    row = await d1_first(
        env, "SELECT * FROM code_challenges WHERE challenge_id=?",
        challenge_id)
    return _challenge(dict(row)) if row else None


async def _members(env, challenge_ids):
    if not challenge_ids:
        return {}
    rows = await d1_all(
        env,
        "SELECT challenge_id, account_bi, name, joined_at "
        "FROM code_challenge_members WHERE challenge_id IN "
        "(SELECT CAST(value AS TEXT) FROM json_each(?)) "
        "ORDER BY joined_at ASC",
        json.dumps(sorted(challenge_ids)))
    grouped = {}
    for row in rows or []:
        row = dict(row)
        grouped.setdefault(row.get("challenge_id"), []).append(row)
    return grouped


async def _score_rows(env, user_bis, from_day, to_day):
    if not user_bis or from_day > to_day:
        return []
    rows = await d1_all(env, SCORE_SQL, json.dumps(sorted(user_bis)),
                        from_day, to_day)
    return [dict(row) for row in rows or []]


def _base(challenge, member_count, today, viewer_bi):
    return {
        "id": challenge["id"],
        **policy.public_settings(challenge["settings"]),
        "ownerName": challenge["ownerName"],
        "startsOn": challenge["startsOn"],
        "endsOn": challenge["endsOn"],
        "phase": policy.phase(challenge["startsOn"], challenge["endsOn"],
                              today),
        "daysLeft": policy.days_left(challenge["startsOn"],
                                     challenge["endsOn"], today),
        "memberCount": member_count,
        "isOwner": bool(viewer_bi) and viewer_bi == challenge["ownerBi"],
        "invitePath": "%s?join=%s" % (PAGE_PATH, challenge["id"]),
    }


def _detail(challenge, members, rows, today, viewer_bi):
    payload = _base(challenge, len(members), today, viewer_bi)
    payload["isMember"] = True
    payload["board"] = policy.board(challenge, members, rows, today,
                                    viewer_bi)
    return payload


async def _details(env, challenges, today, viewer_bi):
    grouped = await _members(env, [item["id"] for item in challenges])
    user_bis = set()
    for members in grouped.values():
        user_bis.update(member.get("account_bi") for member in members)
    user_bis.discard(None)
    today_text = today.isoformat()
    started = [item for item in challenges if item["startsOn"] <= today_text]
    rows = []
    if started:
        rows = await _score_rows(
            env, user_bis, min(item["startsOn"] for item in started),
            min(max(item["endsOn"] for item in started), today_text))
    return [
        _detail(item, grouped.get(item["id"], []),
                _rows_for(rows, grouped.get(item["id"], [])), today,
                viewer_bi)
        for item in challenges
    ]


def _rows_for(rows, members):
    wanted = {member.get("account_bi") for member in members}
    return [row for row in rows if row.get("user_bi") in wanted]


async def _open_counts(env, account_bi, today):
    today_text = today.isoformat()
    owned = await d1_first(
        env,
        "SELECT COUNT(*) AS n FROM code_challenges "
        "WHERE owner_bi=? AND ends_on>=?",
        account_bi, today_text)
    joined = await d1_first(
        env,
        "SELECT COUNT(*) AS n FROM code_challenge_members AS members "
        "JOIN code_challenges AS challenges "
        "ON challenges.challenge_id=members.challenge_id "
        "WHERE members.account_bi=? AND challenges.ends_on>=?",
        account_bi, today_text)
    return (int((owned or {}).get("n") or 0),
            int((joined or {}).get("n") or 0))


async def _list(env, account_bi, today):
    rows = await d1_all(
        env,
        "SELECT challenges.* FROM code_challenge_members AS members "
        "JOIN code_challenges AS challenges "
        "ON challenges.challenge_id=members.challenge_id "
        "WHERE members.account_bi=? "
        "ORDER BY challenges.ends_on DESC, challenges.created_at DESC "
        "LIMIT ?",
        account_bi, LIST_LIMIT)
    challenges = [_challenge(dict(row)) for row in rows or []]
    return await _details(env, challenges, today, account_bi)


async def _create(env, account_bi, name, data, now, today):
    try:
        settings = policy.settings_from_request(
            {key: value for key, value in data.items()
             if key != "sessionToken"}, today)
    except policy.ChallengeError as error:
        return _error(str(error), 400)
    owned, joined = await _open_counts(env, account_bi, today)
    if owned >= policy.MAX_OWNED_OPEN:
        return _error("too_many_owned_challenges", 429)
    if joined >= policy.MAX_JOINED_OPEN:
        return _error("too_many_joined_challenges", 429)
    challenge_id = _ap_uuid()
    stored = {key: settings[key] for key in
              ("name", "metric", "weights", "dailyCap", "maxMembers")}
    await d1_run(
        env,
        "INSERT INTO code_challenges (challenge_id, owner_bi, owner_name, "
        "starts_on, ends_on, created_at, data) VALUES (?,?,?,?,?,?,?)",
        challenge_id, account_bi, name, settings["startsOn"],
        settings["endsOn"], now, json.dumps(stored, separators=(",", ":")))
    await d1_run(
        env,
        "INSERT INTO code_challenge_members (challenge_id, account_bi, name, "
        "joined_at) VALUES (?,?,?,?)",
        challenge_id, account_bi, name, now)
    challenge = await _challenge_by_id(env, challenge_id)
    details = await _details(env, [challenge], today, account_bi)
    return _ok({"challenge": details[0]}, 201)


async def _show(env, request, challenge_id, today):
    challenge = await _challenge_by_id(env, challenge_id)
    if not challenge:
        return _error("not_found", 404)
    account_bi, _name = await _session_account(env, request)
    grouped = await _members(env, [challenge_id])
    members = grouped.get(challenge_id, [])
    if account_bi and any(member.get("account_bi") == account_bi
                          for member in members):
        details = await _details(env, [challenge], today, account_bi)
        return _ok({"challenge": details[0]})
    payload = _base(challenge, len(members), today, account_bi)
    payload["isMember"] = False
    payload["memberNames"] = [member.get("name") or "" for member in members]
    return _ok({"challenge": payload})


async def _join(env, challenge_id, account_bi, name, now, today):
    challenge = await _challenge_by_id(env, challenge_id)
    if not challenge:
        return _error("not_found", 404)
    if challenge["endsOn"] < today.isoformat():
        return _error("challenge_finished", 409)
    existing = await d1_first(
        env,
        "SELECT joined_at FROM code_challenge_members "
        "WHERE challenge_id=? AND account_bi=?",
        challenge_id, account_bi)
    if not existing:
        _owned, joined = await _open_counts(env, account_bi, today)
        if joined >= policy.MAX_JOINED_OPEN:
            return _error("too_many_joined_challenges", 429)
        await d1_run(
            env,
            "INSERT INTO code_challenge_members (challenge_id, account_bi, "
            "name, joined_at) SELECT ?,?,?,? WHERE (SELECT COUNT(*) FROM "
            "code_challenge_members WHERE challenge_id=?)<? "
            "ON CONFLICT(challenge_id, account_bi) DO NOTHING",
            challenge_id, account_bi, name, now, challenge_id,
            challenge["settings"]["maxMembers"])
        joined_row = await d1_first(
            env,
            "SELECT joined_at FROM code_challenge_members "
            "WHERE challenge_id=? AND account_bi=?",
            challenge_id, account_bi)
        if not joined_row:
            return _error("challenge_full", 409)
    details = await _details(env, [challenge], today, account_bi)
    return _ok({"challenge": details[0]}, 200 if existing else 201)


async def _leave(env, challenge_id, account_bi):
    challenge = await _challenge_by_id(env, challenge_id)
    if not challenge:
        return _error("not_found", 404)
    if challenge["ownerBi"] == account_bi:
        return _error("owner_cannot_leave", 409)
    await d1_run(
        env,
        "DELETE FROM code_challenge_members "
        "WHERE challenge_id=? AND account_bi=?",
        challenge_id, account_bi)
    return _ok({"left": challenge_id})


async def _delete(env, challenge_id, account_bi):
    challenge = await _challenge_by_id(env, challenge_id)
    if not challenge:
        return _error("not_found", 404)
    if challenge["ownerBi"] != account_bi:
        return _error("forbidden", 403)
    await d1_run(
        env, "DELETE FROM code_challenge_members WHERE challenge_id=?",
        challenge_id)
    await d1_run(
        env, "DELETE FROM code_challenges WHERE challenge_id=?",
        challenge_id)
    return _ok({"deleted": challenge_id})


async def _body(request):
    try:
        data = await bounded_json_request(request, BODY_MAX_BYTES)
    except Exception:
        return None
    return data if isinstance(data, dict) else ({} if data is None else None)


async def api_handler(env, request, path):
    method = method_name(request)
    now = _now()
    today = _today(now)
    if path in (PREFIX, PREFIX + "/"):
        if method == "GET":
            account_bi, _name = await _session_account(env, request)
            if not account_bi:
                return _error("unauthorized", 401)
            return _ok({"challenges": await _list(env, account_bi, today),
                        "options": policy.options(),
                        "today": today.isoformat()})
        if method == "POST":
            data = await _body(request)
            if data is None:
                return _error("invalid_json", 400)
            account_bi, name = await _session_account(env, request, data)
            if not account_bi:
                return _error("unauthorized", 401)
            return await _create(env, account_bi, name, data, now, today)
        return json_response({"ok": False, "error": "method_not_allowed"},
                             status=405, extra_headers={"allow": "GET, POST"})
    match = DETAIL_RE.match(path)
    if not match:
        return _error("not_found", 404)
    challenge_id, action = match.group(1), match.group(2)
    if not action:
        if method != "GET":
            return json_response(
                {"ok": False, "error": "method_not_allowed"}, status=405,
                extra_headers={"allow": "GET"})
        return await _show(env, request, challenge_id, today)
    if method != "POST":
        return json_response({"ok": False, "error": "method_not_allowed"},
                             status=405, extra_headers={"allow": "POST"})
    data = await _body(request)
    if data is None:
        return _error("invalid_json", 400)
    account_bi, name = await _session_account(env, request, data)
    if not account_bi:
        return _error("unauthorized", 401)
    if action == "join":
        return await _join(env, challenge_id, account_bi, name, now, today)
    if action == "leave":
        return await _leave(env, challenge_id, account_bi)
    return await _delete(env, challenge_id, account_bi)
