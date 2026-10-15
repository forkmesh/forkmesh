"""D1-backed participant-only one-to-one direct-message API."""

import re


DIRECT_MESSAGE_BODY_MAX_BYTES = 16 * 1024
DIRECT_MESSAGE_PAGE_DEFAULT = 50
DIRECT_MESSAGE_PAGE_MAX = 100
DIRECT_MESSAGE_SEARCH_MAX = 20
DIRECT_MESSAGE_CREATE_WINDOW_MS = 60 * 60 * 1000
DIRECT_MESSAGE_CREATE_MAX_PER_WINDOW = 20
COLLECTION_RE = re.compile(r"^/api/chat/direct-messages/?$")
USERS_RE = re.compile(r"^/api/chat/direct-messages/users/?$")
ROOM_ACCESS_RE = re.compile(
    r"^/api/chat/direct-messages/([0-9a-f]{32})/room-access/?$")
READ_RE = re.compile(
    r"^/api/chat/direct-messages/([0-9a-f]{32})/read/?$")
PENDING_RE = re.compile(
    r"^/api/chat/direct-messages/([0-9a-f]{32})/pending/?$")
DELIVERED_RE = re.compile(
    r"^/api/chat/direct-messages/([0-9a-f]{32})/delivered/?$")
CURSOR_RE = re.compile(r"^([0-9]{1,20})\.([0-9a-f]{32})$")
REF_RE = re.compile(r"^[0-9a-f]{40}$")


def _response(runtime, data, status=200, allow="", retry_after_ms=0):
    headers = {"x-content-type-options": "nosniff"}
    if allow:
        headers["allow"] = allow
    if retry_after_ms:
        seconds = max(1, (int(retry_after_ms) + 999) // 1000)
        headers["retry-after"] = str(seconds)
    return runtime.response(
        data,
        status=status,
        cache_control="no-store, max-age=0, must-revalidate",
        extra_headers=headers,
    )


async def _body(runtime):
    data, error = await runtime.json_body(DIRECT_MESSAGE_BODY_MAX_BYTES)
    if error:
        return None, _response(
            runtime,
            {"error": error},
            status=413 if error == "payload_too_large" else 400,
        )
    return data, None


def _other_user(record, actor):
    participants = (record or {}).get("participants")
    if not isinstance(participants, list) or len(participants) != 2:
        return ""
    actor = str(actor or "").strip().lower()
    names = [str(value or "").strip().lower() for value in participants]
    if actor not in names:
        return ""
    return names[1] if names[0] == actor else names[0]


def _conversation_payload(row, record, actor):
    other_user = _other_user(record, actor)
    if not other_user:
        return None
    message_count = max(0, int(row.get("message_count") or 0))
    last_read_count = max(0, int(row.get("last_read_count") or 0))
    return {
        "id": str(row.get("conversation_id") or ""),
        "otherUser": other_user,
        "updatedAt": int(row.get("updated_at") or 0),
        "keyVersion": int(row.get("key_version") or 1),
        "unreadCount": max(0, message_count - last_read_count),
    }


async def _conversation_by_pair(runtime, pair_bi, account_bi):
    return await runtime.d1_first(
        "SELECT c.conversation_id,c.data,c.updated_at,c.key_version,"
        "c.message_count,p.last_read_count "
        "FROM chat_direct_conversations c "
        "JOIN chat_direct_participants p "
        "ON p.conversation_id=c.conversation_id "
        "WHERE c.pair_bi=? AND p.participant_bi=?",
        pair_bi,
        account_bi,
    )


async def _opened_payload(runtime, row, actor):
    if not row:
        return None
    try:
        record = await runtime.open(row.get("data"))
    except Exception:
        return None
    return _conversation_payload(row, record, actor)


async def _creation_throttle(runtime, account_bi, now):
    cutoff = int(now) - DIRECT_MESSAGE_CREATE_WINDOW_MS
    row = await runtime.d1_first(
        "SELECT COUNT(*) AS n,MIN(joined_at) AS oldest "
        "FROM chat_direct_participants "
        "WHERE participant_bi=? AND initiated=1 AND joined_at>?",
        account_bi,
        cutoff,
    )
    if int((row or {}).get("n") or 0) < DIRECT_MESSAGE_CREATE_MAX_PER_WINDOW:
        return 0
    oldest = int((row or {}).get("oldest") or now)
    return max(1, oldest + DIRECT_MESSAGE_CREATE_WINDOW_MS - int(now))


async def _start_conversation(runtime, account_bi, actor, data):
    username = str((data or {}).get("username") or "").strip().lower()
    target_bi, target = await runtime.account(username)
    if not target_bi or not target:
        return _response(runtime, {"error": "user_not_found"}, status=404)
    if target_bi == account_bi:
        return _response(
            runtime, {"error": "cannot_message_self"}, status=400)

    pair_bi = await runtime.blind(
        "chat-direct-pair:" + ":".join(sorted((account_bi, target_bi)))
    )
    existing = await _conversation_by_pair(runtime, pair_bi, account_bi)
    payload = await _opened_payload(runtime, existing, actor)
    if payload:
        return _response(
            runtime, {"ok": True, "conversation": payload})

    now = runtime.now()
    retry_after_ms = await _creation_throttle(runtime, account_bi, now)
    if retry_after_ms:
        return _response(
            runtime,
            {"error": "rate_limited", "retryAfterMs": retry_after_ms},
            status=429,
            retry_after_ms=retry_after_ms,
        )

    conversation_id = runtime.new_id()
    record = {"participants": sorted((actor, target))}
    sealed_record = await runtime.seal(record)
    sealed_actor = await runtime.seal({"username": actor})
    sealed_target = await runtime.seal({"username": target})
    statements = [
        (
            "INSERT INTO chat_direct_conversations "
            "(conversation_id,pair_bi,data,created_at,updated_at,key_version,"
            "message_count) VALUES (?,?,?,?,?,1,0)",
            (conversation_id, pair_bi, sealed_record, now, now),
        ),
        (
            "INSERT INTO chat_direct_participants "
            "(conversation_id,participant_bi,data,joined_at,last_read_count,"
            "initiated) VALUES (?,?,?,?,0,1)",
            (conversation_id, account_bi, sealed_actor, now),
        ),
        (
            "INSERT INTO chat_direct_participants "
            "(conversation_id,participant_bi,data,joined_at,last_read_count,"
            "initiated) VALUES (?,?,?,?,0,0)",
            (conversation_id, target_bi, sealed_target, now),
        ),
    ]
    try:
        await runtime.batch(statements)
    except Exception:
        raced = await _conversation_by_pair(runtime, pair_bi, account_bi)
        raced_payload = await _opened_payload(runtime, raced, actor)
        if raced_payload:
            return _response(
                runtime, {"ok": True, "conversation": raced_payload})
        retry_after_ms = await _creation_throttle(
            runtime, account_bi, now)
        if retry_after_ms:
            return _response(
                runtime,
                {"error": "rate_limited", "retryAfterMs": retry_after_ms},
                status=429,
                retry_after_ms=retry_after_ms,
            )
        raise
    # The target now has a conversation they did not create. Their chat page
    # reads that list once when it opens and never on a timer (adhoc #1604).
    await runtime.notify_account(
        target, "direct-messages", started_by=actor)
    row = {
        "conversation_id": conversation_id,
        "updated_at": now,
        "key_version": 1,
        "message_count": 0,
        "last_read_count": 0,
    }
    return _response(
        runtime,
        {
            "ok": True,
            "conversation": _conversation_payload(row, record, actor),
        },
        status=201,
    )


def _page_options(runtime):
    params = runtime.query_params()
    raw_limit = str(params.get("limit") or DIRECT_MESSAGE_PAGE_DEFAULT)
    if not raw_limit.isdigit():
        return None
    limit = int(raw_limit)
    if limit < 1 or limit > DIRECT_MESSAGE_PAGE_MAX:
        return None
    raw_cursor = str(params.get("cursor") or "")
    if not raw_cursor:
        return limit, None, ""
    match = CURSOR_RE.fullmatch(raw_cursor)
    if not match:
        return None
    return limit, int(match.group(1)), match.group(2)


async def _list_conversations(runtime, account_bi, actor):
    options = _page_options(runtime)
    if not options:
        return _response(runtime, {"error": "invalid_page"}, status=400)
    limit, cursor_updated_at, cursor_id = options
    sql = (
        "SELECT c.conversation_id,c.data,c.updated_at,c.key_version,"
        "c.message_count,p.last_read_count "
        "FROM chat_direct_participants p "
        "JOIN chat_direct_conversations c "
        "ON c.conversation_id=p.conversation_id "
        "WHERE p.participant_bi=? "
    )
    args = [account_bi]
    if cursor_updated_at is not None:
        sql += (
            "AND (c.updated_at<? OR "
            "(c.updated_at=? AND c.conversation_id>?)) "
        )
        args.extend((cursor_updated_at, cursor_updated_at, cursor_id))
    sql += "ORDER BY c.updated_at DESC,c.conversation_id LIMIT ?"
    args.append(limit + 1)
    rows = await runtime.d1_all(sql, *args)
    page = rows[:limit]
    conversations = []
    for row in page:
        payload = await _opened_payload(runtime, row, actor)
        if payload:
            conversations.append(payload)
    next_cursor = ""
    if len(rows) > limit and page:
        last = page[-1]
        next_cursor = "%d.%s" % (
            int(last.get("updated_at") or 0),
            str(last.get("conversation_id") or ""),
        )
    return _response(
        runtime,
        {"conversations": conversations, "nextCursor": next_cursor},
    )


async def _search_users(runtime, actor):
    query = str(runtime.query_params().get("query") or "").strip().lower()
    if not query:
        return _response(runtime, {"users": []})
    if len(query) > 63:
        return _response(runtime, {"error": "invalid_query"}, status=400)
    names = await runtime.search_accounts(query, DIRECT_MESSAGE_SEARCH_MAX)
    users = []
    seen = set()
    for value in names or []:
        name = str(value or "").strip().lower()
        if not name or name == actor or name in seen:
            continue
        seen.add(name)
        users.append({"name": name})
    return _response(runtime, {"users": users})


async def _participant_conversation(runtime, account_bi, conversation_id):
    return await runtime.d1_first(
        "SELECT c.conversation_id,c.data,c.updated_at,c.key_version,"
        "c.message_count,p.last_read_count "
        "FROM chat_direct_conversations c "
        "JOIN chat_direct_participants p "
        "ON p.conversation_id=c.conversation_id "
        "WHERE c.conversation_id=? AND p.participant_bi=?",
        conversation_id,
        account_bi,
    )


async def _mark_read(runtime, account_bi, conversation_id):
    row = await _participant_conversation(
        runtime, account_bi, conversation_id)
    if not row:
        return None
    await runtime.d1_run(
        "UPDATE chat_direct_participants "
        "SET last_read_count=(SELECT message_count "
        "FROM chat_direct_conversations WHERE conversation_id=?) "
        "WHERE conversation_id=? AND participant_bi=?",
        conversation_id,
        conversation_id,
        account_bi,
    )
    advanced = (
        int(row.get("last_read_count") or 0)
        < int(row.get("message_count") or 0))
    row["last_read_count"] = row.get("message_count") or 0
    if advanced:
        try:
            await runtime.announce_read(
                conversation_id,
                int(row.get("key_version") or 1),
                account_bi,
            )
        except Exception:
            pass
    return row


async def _mark_read_response(runtime, account_bi, conversation_id):
    row = await _mark_read(runtime, account_bi, conversation_id)
    if not row:
        return _response(runtime, {"error": "not_found"}, status=404)
    return _response(runtime, {"ok": True})


async def _room_access(runtime, account_bi, actor, conversation_id):
    row = await _mark_read(runtime, account_bi, conversation_id)
    payload = await _opened_payload(runtime, row, actor)
    if not payload:
        return _response(runtime, {"error": "not_found"}, status=404)
    access = await runtime.room_access(
        conversation_id,
        int(row.get("key_version") or 1),
        account_bi,
    )
    result = dict(access or {})
    result["conversation"] = payload
    return _response(runtime, result)


async def _pending(runtime, account_bi, conversation_id):
    ref = str(runtime.query_params().get("ref") or "").strip().lower()
    if not REF_RE.fullmatch(ref):
        return _response(runtime, {"error": "invalid_ref"}, status=400)
    row = await _participant_conversation(
        runtime, account_bi, conversation_id)
    if not row:
        return _response(runtime, {"error": "not_found"}, status=404)
    key_version = int(row.get("key_version") or 1)
    room = "chat-direct:" + conversation_id + ":v" + str(key_version)
    stored = await runtime.d1_first(
        "SELECT body FROM chat_history "
        "WHERE room_key=? AND msg_id>=? AND msg_id<? LIMIT 1",
        room,
        ref + ":",
        ref + ";",
    )
    frame = str((stored or {}).get("body") or "")
    if not frame:
        return _response(runtime, {"error": "not_found"}, status=404)
    return _response(
        runtime,
        {
            "conversation": "direct:" + conversation_id,
            "room": room,
            "keyVersion": key_version,
            "passphrase": await runtime.passphrase(
                conversation_id, key_version),
            "frame": frame,
        },
    )


async def _delivered(runtime, account_bi, conversation_id):
    ref = str(runtime.query_params().get("ref") or "").strip().lower()
    if not REF_RE.fullmatch(ref):
        return _response(runtime, {"error": "invalid_ref"}, status=400)
    row = await _participant_conversation(
        runtime, account_bi, conversation_id)
    if not row:
        return _response(runtime, {"error": "not_found"}, status=404)
    key_version = int(row.get("key_version") or 1)
    stored = await runtime.d1_first(
        "SELECT msg_id FROM chat_history "
        "WHERE room_key=? AND msg_id>=? AND msg_id<? "
        "AND substr(msg_id,42,16)<>? LIMIT 1",
        "chat-direct:" + conversation_id + ":v" + str(key_version),
        ref + ":",
        ref + ";",
        str(account_bi)[:16],
    )
    if not stored:
        return _response(runtime, {"error": "not_found"}, status=404)
    await runtime.announce_delivered(
        conversation_id, key_version, account_bi, ref)
    return _response(runtime, {"ok": True})


async def handle(runtime, path):
    """Serve direct-message discovery, collection, and participant access."""
    await runtime.ensure_schema()
    normalized_path = str(path or "")
    collection = COLLECTION_RE.fullmatch(normalized_path)
    users = USERS_RE.fullmatch(normalized_path)
    room_access = ROOM_ACCESS_RE.fullmatch(normalized_path)
    read = READ_RE.fullmatch(normalized_path)
    pending = PENDING_RE.fullmatch(normalized_path)
    delivered = DELIVERED_RE.fullmatch(normalized_path)
    if not (
        collection or users or room_access or read or pending or delivered
    ):
        return _response(runtime, {"error": "not_found"}, status=404)

    method = runtime.method()
    allowed = "GET, POST" if collection else (
        "POST" if read or delivered else "GET"
    )
    if method not in {value.strip() for value in allowed.split(",")}:
        return _response(
            runtime,
            {"error": "method_not_allowed"},
            status=405,
            allow=allowed,
        )

    data = None
    if collection and method == "POST":
        data, error_response = await _body(runtime)
        if error_response is not None:
            return error_response
    account_bi, account = await runtime.session(data)
    actor = str((account or {}).get("name") or "").strip().lower()
    if not account_bi or not actor:
        return _response(
            runtime, {"error": "invalid_session"}, status=401)

    if users:
        return await _search_users(runtime, actor)
    if collection and method == "GET":
        return await _list_conversations(runtime, account_bi, actor)
    if collection and method == "POST":
        return await _start_conversation(
            runtime, account_bi, actor, data)
    if read:
        return await _mark_read_response(
            runtime, account_bi, read.group(1))
    if pending:
        return await _pending(runtime, account_bi, pending.group(1))
    if delivered:
        return await _delivered(runtime, account_bi, delivered.group(1))
    return await _room_access(
        runtime, account_bi, actor, room_access.group(1))
