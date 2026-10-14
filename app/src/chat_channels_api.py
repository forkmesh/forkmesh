"""D1-backed encrypted chat channels, memberships, and incoming webhooks."""

import json
import re


CHANNEL_BODY_MAX_BYTES = 64 * 1024
MAX_CHAT_CHANNELS = 100
MAX_CHANNEL_MEMBERS = 500
MAX_CHANNEL_TOPIC_LENGTH = 200
MAX_CHANNEL_WEBHOOKS = 10
MAX_WEBHOOK_NAME_LENGTH = 80
MAX_WEBHOOK_TEXT_LENGTH = 8000
CHANNEL_NAME_RE = re.compile(
    r"^[a-z0-9](?:[a-z0-9-]{0,38}[a-z0-9])?$")
# Everything a channel manager may PATCH from the settings panel. Each one is
# optional, so an older client sending only a topic keeps working.
CHANNEL_SETTINGS_FIELDS = ("name", "visibility", "topic")
CHANNEL_ID_RE = re.compile(r"^[0-9a-f]{32}$")
COLLECTION_RE = re.compile(r"^/api/chat/channels/?$")
CHANNEL_RE = re.compile(r"^/api/chat/channels/([0-9a-f]{32})/?$")
MEMBERS_RE = re.compile(
    r"^/api/chat/channels/([0-9a-f]{32})/members/?$")
ROOM_ACCESS_RE = re.compile(
    r"^/api/chat/channels/([0-9a-f]{32})/room-access/?$")
HISTORY_RE = re.compile(
    r"^/api/chat/channels/([0-9a-f]{32})/history/?$")
PENDING_RE = re.compile(
    r"^/api/chat/channels/([0-9a-f]{32})/pending/?$")
REF_RE = re.compile(r"^[0-9a-f]{40}$")
WEBHOOKS_RE = re.compile(
    r"^/api/chat/channels/([0-9a-f]{32})/webhooks/?$")
WEBHOOK_RE = re.compile(
    r"^/api/chat/channels/([0-9a-f]{32})/webhooks/([0-9a-f]{32})/?$")
WEBHOOK_DELIVERY_RE = re.compile(
    r"^/api/chat/webhooks/([0-9a-f]{64})/?$")
INVITE_RE = re.compile(
    r"^/api/chat/channels/([0-9a-f]{32})/invite/?$")
INVITE_TOKEN_RE = re.compile(
    r"^/api/chat/invites/([0-9a-f]{64})/?$")
HISTORY_MAX_MESSAGES = 200
NOTICES_KIND = "notices"
OFFICE_KIND = "office"


def _response(runtime, data, status=200, allow=""):
    headers = {"x-content-type-options": "nosniff"}
    if allow:
        headers["allow"] = allow
    return runtime.response(
        data,
        status=status,
        cache_control="no-store, max-age=0, must-revalidate",
        extra_headers=headers,
    )


async def _body(runtime):
    data, error = await runtime.json_body(CHANNEL_BODY_MAX_BYTES)
    if error:
        return None, _response(
            runtime,
            {"error": error},
            status=413 if error == "payload_too_large" else 400,
        )
    return data, None


def _channel_payload(row, record, can_manage):
    payload = {
        "id": str(row.get("channel_id") or ""),
        "name": str((record or {}).get("name") or ""),
        "updatedAt": int(row.get("updated_at") or 0),
        "keyVersion": int(row.get("key_version") or 1),
        "canManage": bool(can_manage) and not _is_notices(record),
        "visibility": _visibility(record),
        "topic": str((record or {}).get("topic") or ""),
    }
    if _is_office(record):
        payload["kind"] = OFFICE_KIND
        payload["office"] = {"org": str(record.get("org") or ""),
                             "room": str(record.get("room") or "")}
    if _is_notices(record):
        payload["kind"] = NOTICES_KIND
        payload["serverVisible"] = True
        payload["readOnly"] = True
    return payload


def _is_notices(record):
    return isinstance(record, dict) and record.get("kind") == NOTICES_KIND


def _is_office(record):
    return isinstance(record, dict) and record.get("kind") == OFFICE_KIND


def _owner_only_denied(row, record, account_bi):
    return _is_notices(record) and str(
        (row or {}).get("created_by_bi") or "") != account_bi


def _visibility(record):
    return "public" if (record or {}).get("visibility") == "public" else (
        "private"
    )


def _is_creator(row, account_bi):
    return bool(account_bi) and str(
        (row or {}).get("created_by_bi") or "") == account_bi


def _can_manage(row, record, account_bi, is_admin):
    if _is_notices(record) or _is_office(record):
        return False
    if _is_creator(row, account_bi):
        return True
    return bool(is_admin) and _visibility(record) == "public"


async def _is_member(runtime, channel_id, account_bi):
    if not account_bi:
        return False
    membership = await runtime.d1_first(
        "SELECT 1 AS one FROM chat_channel_members "
        "WHERE channel_id=? AND member_bi=?",
        channel_id,
        account_bi,
    )
    return bool(membership)


async def _mark_read(runtime, channel_id, account_bi):
    if not account_bi:
        return
    try:
        await runtime.d1_run(
            "INSERT INTO chat_channel_reads (channel_id,account_bi,read_at) "
            "VALUES (?,?,?) ON CONFLICT(channel_id,account_bi) DO UPDATE SET "
            "read_at=MAX(chat_channel_reads.read_at,excluded.read_at)",
            channel_id,
            account_bi,
            runtime.now(),
        )
    except Exception:
        pass


async def _create_channel(runtime, account_bi, actor, data):
    name = str((data or {}).get("name") or "").strip().lower()
    if not CHANNEL_NAME_RE.fullmatch(name):
        return _response(
            runtime, {"error": "invalid_channel_name"}, status=400)
    visibility = str(
        (data or {}).get("visibility") or "private"
    ).strip().lower()
    if visibility not in {"private", "public"}:
        return _response(
            runtime, {"error": "invalid_visibility"}, status=400)
    raw_members = (data or {}).get("members", [])
    if not isinstance(raw_members, list):
        return _response(runtime, {"error": "invalid_members"}, status=400)
    if len(raw_members) > MAX_CHANNEL_MEMBERS:
        return _response(runtime, {"error": "too_many_members"}, status=429)
    if visibility == "public" and raw_members:
        return _response(
            runtime, {"error": "members_not_allowed"}, status=400)

    initial_members = []
    seen_members = set()
    for value in raw_members:
        if not isinstance(value, str):
            return _response(
                runtime, {"error": "invalid_members"}, status=400)
        member_bi, canonical = await runtime.account(value)
        if not member_bi or not canonical:
            return _response(
                runtime, {"error": "user_not_found"}, status=404)
        if canonical in seen_members:
            continue
        seen_members.add(canonical)
        if canonical == actor or member_bi == account_bi:
            continue
        initial_members.append((member_bi, canonical))

    name_bi = await runtime.blind("chat-channel-name:" + name)
    existing = await runtime.d1_first(
        "SELECT 1 AS one FROM chat_channels WHERE name_bi=?", name_bi)
    if existing:
        return _response(
            runtime, {"error": "channel_name_taken"}, status=409)
    count = await runtime.d1_first(
        "SELECT COUNT(*) AS n FROM chat_channels WHERE channel_id NOT IN "
        "(SELECT channel_id FROM org_office_channels)")
    if int((count or {}).get("n") or 0) >= MAX_CHAT_CHANNELS:
        return _response(
            runtime, {"error": "too_many_channels"}, status=429)

    now = runtime.now()
    channel_id = runtime.new_id()
    record = {
        "name": name,
        "createdBy": actor,
        "visibility": visibility,
    }
    sealed = await runtime.seal(record)
    statements = [(
        "INSERT INTO chat_channels "
        "(channel_id,name_bi,data,created_by_bi,created_at,updated_at,"
        "key_version) VALUES (?,?,?,?,?,?,1)",
        (channel_id, name_bi, sealed, account_bi, now, now),
    )]
    member_payloads = []
    for member_bi, canonical in initial_members:
        member_payloads.append({"username": canonical, "joinedAt": now})
        member_data = await runtime.seal({"username": canonical})
        statements.append((
            "INSERT INTO chat_channel_members "
            "(channel_id,member_bi,data,invited_by_bi,joined_at) "
            "VALUES (?,?,?,?,?)",
            (channel_id, member_bi, member_data, account_bi, now),
        ))
    await runtime.batch(statements)
    # An invited member's channel list just changed for somebody else's reason.
    # The chat page reads that list once when it opens and never on a timer, so
    # tell them (adhoc #1604).
    for _member_bi, canonical in initial_members:
        if canonical != actor:
            await runtime.notify_account(canonical, "private-channels")
    await runtime.audit(
        actor,
        "chat.channel.create",
        "chat_channel",
        channel_id,
        details={"reason": "account_created"},
    )
    row = {
        "channel_id": channel_id,
        "updated_at": now,
        "key_version": 1,
    }
    payload = _channel_payload(row, record, True)
    if visibility == "private":
        payload["members"] = [actor] + [
            canonical for _member_bi, canonical in initial_members]
    return _response(
        runtime,
        {"ok": True, "channel": payload, "members": member_payloads},
        status=201,
    )


async def _channel(runtime, channel_id):
    if not CHANNEL_ID_RE.fullmatch(str(channel_id or "")):
        return None, None
    row = await runtime.d1_first(
        "SELECT channel_id,data,created_by_bi,updated_at,key_version "
        "FROM chat_channels "
        "WHERE channel_id=?",
        channel_id,
    )
    if not row:
        return None, None
    try:
        record = await runtime.open(row.get("data"))
    except Exception:
        return None, None
    return row, record


async def _update_channel(runtime, actor, channel_id, data, row=None,
                          record=None):
    """Apply the settings a channel manager may change, one PATCH at a time.

    Every field is optional: a body naming only ``topic`` is the original
    /topic slash command, while the settings panel sends the whole set.
    """

    if row is None:
        row, record = await _channel(runtime, channel_id)
    if not row:
        return _response(runtime, {"error": "not_found"}, status=404)
    payload = data if isinstance(data, dict) else {}
    if not any(field in payload for field in CHANNEL_SETTINGS_FIELDS):
        # A body naming no settable field is the topic-only edit with its
        # topic missing.
        return _response(runtime, {"error": "invalid_topic"}, status=400)
    record = dict(record or {})
    changes = []
    name_bi = ""

    if "name" in payload:
        name = str(payload.get("name") or "").strip().lower()
        if not CHANNEL_NAME_RE.fullmatch(name):
            return _response(
                runtime, {"error": "invalid_channel_name"}, status=400)
        if name != str(record.get("name") or ""):
            name_bi = await runtime.blind("chat-channel-name:" + name)
            taken = await runtime.d1_first(
                "SELECT channel_id FROM chat_channels WHERE name_bi=?",
                name_bi,
            )
            if taken and str(taken.get("channel_id") or "") != channel_id:
                return _response(
                    runtime, {"error": "channel_name_taken"}, status=409)
            record["name"] = name
            changes.append(
                ("chat.channel.rename", "channel_manager_renamed_channel"))

    rotate = False
    if "visibility" in payload:
        visibility = str(payload.get("visibility") or "").strip().lower()
        if visibility not in {"private", "public"}:
            return _response(
                runtime, {"error": "invalid_visibility"}, status=400)
        if visibility != _visibility(record):
            # Closing an open channel has to lock out everyone who already
            # holds its passphrase, exactly as removing a member does.
            rotate = visibility == "private"
            record["visibility"] = visibility
            changes.append((
                "chat.channel.visibility.update",
                "channel_manager_changed_visibility",
            ))

    if "topic" in payload:
        raw_topic = payload.get("topic")
        if not isinstance(raw_topic, str):
            return _response(runtime, {"error": "invalid_topic"}, status=400)
        # Topics are one-line labels. Collapsing whitespace keeps the
        # encrypted record and every client header bounded without
        # accepting control text.
        topic = " ".join(raw_topic.split())
        if len(topic) > MAX_CHANNEL_TOPIC_LENGTH:
            return _response(runtime, {"error": "invalid_topic"}, status=400)
        if topic != str(record.get("topic") or ""):
            if topic:
                record["topic"] = topic
            else:
                record.pop("topic", None)
            changes.append(
                ("chat.channel.topic.update", "channel_manager_changed_topic"))

    if not changes:
        return _response(
            runtime,
            {"ok": True, "channel": _channel_payload(row, record, True)},
        )

    now = runtime.now()
    sealed = await runtime.seal(record)
    if name_bi:
        await runtime.d1_run(
            "UPDATE chat_channels SET data=?,name_bi=?,updated_at=? "
            "WHERE channel_id=?",
            sealed,
            name_bi,
            now,
            channel_id,
        )
    else:
        await runtime.d1_run(
            "UPDATE chat_channels SET data=?,updated_at=? WHERE channel_id=?",
            sealed,
            now,
            channel_id,
        )
    updated_row = dict(row)
    updated_row["updated_at"] = now
    if rotate:
        key_version = int(row.get("key_version") or 1)
        await runtime.d1_run(
            "UPDATE chat_channels SET key_version=key_version+1 "
            "WHERE channel_id=?",
            channel_id,
        )
        await runtime.revoke_room(channel_id, key_version)
        # Controller-originated webhooks cannot produce recipient-sealed
        # frames. Closing a public channel therefore revokes every integration
        # capability instead of silently reactivating it if the channel is
        # opened again later.
        await runtime.d1_run(
            "DELETE FROM chat_channel_webhooks WHERE channel_id=?",
            channel_id,
        )
        updated_row["key_version"] = key_version + 1
    for action, reason in changes:
        await runtime.audit(
            actor,
            action,
            "chat_channel",
            channel_id,
            details={"reason": reason},
        )
    return _response(
        runtime,
        {"ok": True, "channel": _channel_payload(
            updated_row, record, True)},
    )


async def _delete_channel(runtime, actor, channel_id, row):
    """Retire a channel with its memberships, webhooks and live room."""

    key_version = int(row.get("key_version") or 1)
    member_rows = await runtime.d1_all(
        "SELECT data,joined_at FROM chat_channel_members WHERE channel_id=? "
        "ORDER BY joined_at,member_bi",
        channel_id,
    )
    await runtime.batch([
        (
            "DELETE FROM chat_channel_webhooks WHERE channel_id=?",
            (channel_id,),
        ),
        (
            "DELETE FROM chat_channel_invites WHERE channel_id=?",
            (channel_id,),
        ),
        (
            "DELETE FROM chat_channel_members WHERE channel_id=?",
            (channel_id,),
        ),
        (
            "DELETE FROM chat_channel_reads WHERE channel_id=?",
            (channel_id,),
        ),
        ("DELETE FROM chat_channels WHERE channel_id=?", (channel_id,)),
    ])
    # Close the live room so open sockets stop rather than linger on a
    # channel that no longer exists in anybody's list.
    await runtime.revoke_room(channel_id, key_version)
    for member_row in member_rows:
        member = await _member_payload(runtime, member_row)
        username = member["username"]
        if username and username != actor:
            await runtime.notify_account(username, "private-channels")
    await runtime.audit(
        actor,
        "chat.channel.delete",
        "chat_channel",
        channel_id,
        details={
            "reason": "channel_manager_deleted_channel",
            "members": len(member_rows),
        },
    )
    return _response(runtime, {"ok": True, "removed": True})


async def _list_channels(runtime, account_bi, actor, is_admin):
    rows = await runtime.d1_all(
        "SELECT c.channel_id,c.data,c.created_by_bi,c.updated_at,"
        "c.key_version,"
        "CASE WHEN m.member_bi IS NULL THEN 0 ELSE 1 END AS joined "
        "FROM chat_channels c LEFT JOIN chat_channel_members m "
        "ON m.channel_id=c.channel_id AND m.member_bi=? "
        "ORDER BY c.created_at,c.channel_id",
        account_bi,
    )
    channels = []
    for row in rows:
        try:
            record = await runtime.open(row.get("data"))
        except Exception:
            continue
        if _owner_only_denied(row, record, account_bi):
            continue
        if (_visibility(record) != "public"
                and not _is_creator(row, account_bi)
                and not bool(int(row.get("joined") or 0))):
            continue
        can_manage = _can_manage(row, record, account_bi, is_admin)
        payload = _channel_payload(row, record, can_manage)
        if _visibility(record) == "private":
            payload["members"] = await _room_member_names(
                runtime, str(row.get("channel_id") or ""), record)
        channels.append(payload)
    return _response(runtime, {"channels": channels})


async def _room_member_names(runtime, channel_id, record):
    member_rows = await runtime.d1_all(
        "SELECT data,joined_at FROM chat_channel_members "
        "WHERE channel_id=? ORDER BY joined_at,member_bi",
        channel_id,
    )
    members = []
    creator = str((record or {}).get("createdBy") or "").strip().lower()
    if creator:
        members.append(creator)
    for member_row in member_rows:
        member = await _member_payload(runtime, member_row)
        username = member["username"]
        if username and username not in members:
            members.append(username)
    return members


async def _require_manager(runtime, account_bi, actor, action, channel_id,
                           is_admin=False):
    row, record = await _readable_channel(
        runtime, account_bi, channel_id, is_admin)
    if not row:
        return None, None, _response(
            runtime, {"error": "not_found"}, status=404)
    if _is_notices(record):
        return None, None, _response(
            runtime, {"error": "notices_channel_read_only"}, status=403)
    if _is_office(record):
        return None, None, _response(
            runtime, {"error": "office_channel_managed_by_org"}, status=403)
    if _can_manage(row, record, account_bi, is_admin):
        return row, record, None
    await runtime.audit(
        actor,
        action,
        "chat_channel",
        channel_id,
        outcome="denied",
        details={"reason": "channel_manager_required"},
    )
    return None, None, _response(
        runtime, {"error": "manager_required"}, status=403)


async def _member_payload(runtime, row):
    try:
        record = await runtime.open(row.get("data"))
    except Exception:
        record = {}
    return {
        "username": str((record or {}).get("username") or ""),
        "joinedAt": int(row.get("joined_at") or 0),
    }


async def _list_members(runtime, channel_id):
    row, record = await _channel(runtime, channel_id)
    if not row:
        return _response(runtime, {"error": "not_found"}, status=404)
    if _visibility(record) != "private":
        return _response(
            runtime, {"error": "members_not_allowed"}, status=409)
    rows = await runtime.d1_all(
        "SELECT data,joined_at FROM chat_channel_members "
        "WHERE channel_id=? ORDER BY joined_at,member_bi",
        channel_id,
    )
    members = []
    for member_row in rows:
        member = await _member_payload(runtime, member_row)
        if member["username"]:
            members.append(member)
    return _response(runtime, {"members": members})


async def _add_member(runtime, account_bi, actor, channel_id, data):
    row, record = await _channel(runtime, channel_id)
    if not row:
        return _response(runtime, {"error": "not_found"}, status=404)
    if _visibility(record) != "private":
        return _response(
            runtime, {"error": "members_not_allowed"}, status=409)
    username = str((data or {}).get("username") or "").strip().lower()
    member_bi, canonical = await runtime.account(username)
    if not member_bi or not canonical:
        return _response(runtime, {"error": "user_not_found"}, status=404)
    if member_bi == str(row.get("created_by_bi") or ""):
        return _response(
            runtime,
            {
                "ok": True,
                "member": {"username": canonical, "joinedAt": 0},
                "implicitCreator": True,
            },
        )
    existing = await runtime.d1_first(
        "SELECT data,joined_at FROM chat_channel_members "
        "WHERE channel_id=? AND member_bi=?",
        channel_id,
        member_bi,
    )
    if existing:
        return _response(
            runtime,
            {"ok": True, "member": await _member_payload(runtime, existing)},
        )
    count = await runtime.d1_first(
        "SELECT COUNT(*) AS n FROM chat_channel_members WHERE channel_id=?",
        channel_id,
    )
    if int((count or {}).get("n") or 0) >= MAX_CHANNEL_MEMBERS:
        return _response(runtime, {"error": "too_many_members"}, status=429)
    now = runtime.now()
    sealed = await runtime.seal({"username": canonical})
    await runtime.d1_run(
        "INSERT INTO chat_channel_members "
        "(channel_id,member_bi,data,invited_by_bi,joined_at) "
        "VALUES (?,?,?,?,?)",
        channel_id,
        member_bi,
        sealed,
        account_bi,
        now,
    )
    if canonical != actor:
        await runtime.notify_account(canonical, "private-channels")
    await runtime.audit(
        actor,
        "chat.channel.member.add",
        "chat_channel",
        channel_id,
        details={"reason": "channel_manager_invited_user"},
    )
    return _response(
        runtime,
        {"ok": True, "member": {"username": canonical, "joinedAt": now}},
        status=201,
    )


async def _remove_member(runtime, actor, channel_id, data):
    row, record = await _channel(runtime, channel_id)
    if not row:
        return _response(runtime, {"error": "not_found"}, status=404)
    if _visibility(record) != "private":
        return _response(
            runtime, {"error": "members_not_allowed"}, status=409)
    username = str((data or {}).get("username") or "").strip().lower()
    member_bi, _canonical = await runtime.account(username)
    if not member_bi:
        return _response(runtime, {"error": "user_not_found"}, status=404)
    deleted = await runtime.d1_first(
        "DELETE FROM chat_channel_members "
        "WHERE channel_id=? AND member_bi=? RETURNING member_bi",
        channel_id,
        member_bi,
    )
    removed = bool(deleted)
    if removed:
        await runtime.d1_run(
            "DELETE FROM chat_channel_reads WHERE channel_id=? AND account_bi=?",
            channel_id,
            member_bi,
        )
        # The delete trigger rotates key_version deterministically; the
        # display timestamp is stamped here (migration 0191), since a 'now'
        # inside a replicated trigger body is not deterministic on rqlite.
        await runtime.d1_run(
            "UPDATE chat_channels SET updated_at=MAX(updated_at,?) "
            "WHERE channel_id=?",
            runtime.now(),
            channel_id,
        )
        await runtime.revoke_room(
            channel_id, int(row.get("key_version") or 1))
    invite = await _drop_invite(runtime, channel_id) if removed else False
    current = await runtime.d1_first(
        "SELECT key_version FROM chat_channels WHERE channel_id=?",
        channel_id,
    )
    await runtime.audit(
        actor,
        "chat.channel.member.remove",
        "chat_channel",
        channel_id,
        details={"reason": "channel_manager_removed_user", "removed": removed},
    )
    return _response(
        runtime,
        {
            "ok": True,
            "removed": removed,
            "inviteReset": bool(invite),
            "keyVersion": int((current or {}).get("key_version") or 1),
        },
    )


async def _readable_channel(runtime, account_bi, channel_id, is_admin):
    row, record = await _channel(runtime, channel_id)
    if not row or _owner_only_denied(row, record, account_bi):
        return None, None
    if _visibility(record) == "public" or _is_creator(row, account_bi):
        return row, record
    if await _is_member(runtime, channel_id, account_bi):
        return row, record
    return None, None


def _webhook_payload(row, record):
    return {
        "id": str(row.get("webhook_id") or ""),
        "name": str((record or {}).get("name") or "Webhook"),
        "createdAt": int(row.get("created_at") or 0),
        "lastUsedAt": int(row.get("last_used_at") or 0),
    }


async def _list_webhooks(runtime, channel_id):
    rows = await runtime.d1_all(
        "SELECT webhook_id,data,created_at,last_used_at "
        "FROM chat_channel_webhooks WHERE channel_id=? "
        "ORDER BY created_at,webhook_id",
        channel_id,
    )
    webhooks = []
    for row in rows:
        try:
            record = await runtime.open(row.get("data"))
        except Exception:
            continue
        webhooks.append(_webhook_payload(row, record))
    return _response(runtime, {"webhooks": webhooks})


async def _create_webhook(runtime, account_bi, actor, channel_id, data):
    name = " ".join(str((data or {}).get("name") or "Webhook").split())
    if not name or len(name) > MAX_WEBHOOK_NAME_LENGTH:
        return _response(runtime, {"error": "invalid_webhook_name"}, status=400)
    count = await runtime.d1_first(
        "SELECT COUNT(*) AS n FROM chat_channel_webhooks WHERE channel_id=?",
        channel_id,
    )
    if int((count or {}).get("n") or 0) >= MAX_CHANNEL_WEBHOOKS:
        return _response(runtime, {"error": "too_many_webhooks"}, status=429)
    webhook_id = runtime.new_id()
    token = runtime.new_webhook_token()
    token_bi = await runtime.blind("chat-webhook-token:" + token)
    now = runtime.now()
    record = {"name": name, "createdBy": actor}
    await runtime.d1_run(
        "INSERT INTO chat_channel_webhooks "
        "(webhook_id,channel_id,token_bi,data,created_by_bi,created_at,"
        "last_used_at) VALUES (?,?,?,?,?,?,0)",
        webhook_id,
        channel_id,
        token_bi,
        await runtime.seal(record),
        account_bi,
        now,
    )
    await runtime.audit(
        actor,
        "chat.channel.webhook.create",
        "chat_channel",
        channel_id,
        details={"webhookId": webhook_id},
    )
    row = {
        "webhook_id": webhook_id,
        "created_at": now,
        "last_used_at": 0,
    }
    return _response(
        runtime,
        {
            "ok": True,
            "webhook": _webhook_payload(row, record),
            # The bearer capability is returned once and never stored in
            # plaintext. A later list therefore cannot recover it.
            "url": runtime.webhook_url(token),
        },
        status=201,
    )


async def _rotate_webhook(runtime, actor, channel_id, webhook_id):
    """Mint a fresh URL for an existing webhook and reveal it once.

    A created token is kept only as a blind index, so a manager who lost the
    URL cannot be shown it again -- rotating is how they get a working one
    back, and it retires the old URL in the same write.
    """

    row = await runtime.d1_first(
        "SELECT webhook_id,data,created_at,last_used_at "
        "FROM chat_channel_webhooks WHERE channel_id=? AND webhook_id=?",
        channel_id,
        webhook_id,
    )
    if not row:
        return _response(runtime, {"error": "not_found"}, status=404)
    try:
        record = await runtime.open(row.get("data"))
    except Exception:
        record = {}
    token = runtime.new_webhook_token()
    token_bi = await runtime.blind("chat-webhook-token:" + token)
    await runtime.d1_run(
        "UPDATE chat_channel_webhooks SET token_bi=?,last_used_at=0 "
        "WHERE channel_id=? AND webhook_id=?",
        token_bi,
        channel_id,
        webhook_id,
    )
    await runtime.audit(
        actor,
        "chat.channel.webhook.rotate",
        "chat_channel",
        channel_id,
        details={"webhookId": webhook_id},
    )
    rotated = dict(row)
    rotated["last_used_at"] = 0
    return _response(
        runtime,
        {
            "ok": True,
            "webhook": _webhook_payload(rotated, record),
            # Revealed once, like creation: the previous URL is dead now.
            "url": runtime.webhook_url(token),
        },
    )


async def _delete_webhook(runtime, actor, channel_id, webhook_id):
    deleted = await runtime.d1_first(
        "DELETE FROM chat_channel_webhooks "
        "WHERE channel_id=? AND webhook_id=? RETURNING webhook_id",
        channel_id,
        webhook_id,
    )
    removed = bool(deleted)
    await runtime.audit(
        actor,
        "chat.channel.webhook.delete",
        "chat_channel",
        channel_id,
        details={"webhookId": webhook_id, "removed": removed},
    )
    return _response(runtime, {"ok": True, "removed": removed})


def _webhook_text(data):
    if isinstance(data, dict):
        for key in ("text", "content", "message"):
            value = data.get(key)
            if isinstance(value, str) and value.strip():
                return value.strip()[:MAX_WEBHOOK_TEXT_LENGTH]
    try:
        value = json.dumps(data, ensure_ascii=False, separators=(",", ":"))
    except (TypeError, ValueError):
        value = str(data or "")
    return value.strip()[:MAX_WEBHOOK_TEXT_LENGTH]


async def _deliver_webhook(runtime, token, data):
    token_bi = await runtime.blind("chat-webhook-token:" + token)
    row = await runtime.d1_first(
        "SELECT w.webhook_id,w.channel_id,w.data AS webhook_data,"
        "c.data AS channel_data,c.created_by_bi,c.key_version "
        "FROM chat_channel_webhooks w JOIN chat_channels c "
        "ON c.channel_id=w.channel_id WHERE w.token_bi=?",
        token_bi,
    )
    if not row:
        return _response(runtime, {"error": "not_found"}, status=404)
    try:
        webhook = await runtime.open(row.get("webhook_data"))
        channel = await runtime.open(row.get("channel_data"))
    except Exception:
        return _response(runtime, {"error": "not_found"}, status=404)
    if _visibility(channel) != "public":
        return _response(
            runtime, {"error": "public_channel_required"}, status=409)
    text = _webhook_text(data)
    if not text:
        return _response(runtime, {"error": "empty_webhook"}, status=400)
    channel_id = str(row.get("channel_id") or "")
    webhook_id = str(row.get("webhook_id") or "")
    now = runtime.now()
    message_id = runtime.new_id()
    plain = {
        "type": "chat",
        "id": message_id,
        "senderId": "webhook:" + webhook_id,
        "sender": str((webhook or {}).get("name") or "Webhook"),
        "accountKind": "user",
        "ts": now,
        "channel": "#" + str((channel or {}).get("name") or "channel"),
        "text": text,
        "webhook": True,
    }
    await runtime.deliver_webhook(
        channel_id, int(row.get("key_version") or 1), plain)
    try:
        await runtime.d1_run(
            "UPDATE chat_channel_webhooks SET last_used_at=? WHERE webhook_id=?",
            now,
            webhook_id,
        )
    except Exception:
        # The encrypted room write is already durable. Do not make the sender
        # retry (and duplicate a live delivery) for display-only metadata.
        pass

    try:
        recipients = []
        creator = str((channel or {}).get("createdBy") or "").strip().lower()
        if creator:
            recipients.append(creator)
        if _visibility(channel) == "private":
            members = await runtime.d1_all(
                "SELECT data FROM chat_channel_members WHERE channel_id=?",
                channel_id,
            )
            for member_row in members:
                member = await _member_payload(runtime, member_row)
                username = member["username"]
                if username and username not in recipients:
                    recipients.append(username)
        else:
            for username in await runtime.public_webhook_recipients():
                if username and username not in recipients:
                    recipients.append(username)
        for recipient in recipients:
            await runtime.notify_webhook(
                recipient,
                str((channel or {}).get("name") or "channel"),
                str((webhook or {}).get("name") or "Webhook"),
                text,
                channel_id,
                message_id,
            )
    except Exception:
        # Notification fan-out is best effort after the encrypted message has
        # been accepted and retained.
        pass
    return _response(runtime, {"ok": True, "messageId": message_id}, status=202)


def _history_message(entry):
    body = str(entry.get("body") or "")
    message = {"ts": int(entry.get("ts") or 0), "body": body}
    try:
        envelope = json.loads(body)
    except ValueError:
        return message
    recipients = (
        envelope.get("recipients") if isinstance(envelope, dict) else None)
    if (
        isinstance(recipients, int)
        and not isinstance(recipients, bool)
        and recipients >= 0
    ):
        message["recipients"] = recipients
        names = envelope.get("recipientNames")
        if isinstance(names, list) and all(
                isinstance(name, str) for name in names):
            message["recipientNames"] = names
    return message


async def _history(runtime, account_bi, channel_id, is_admin):
    # Read-only replay of one channel room for clients that hold no WebSocket
    # into it (the desktop app mirroring the World office's rooms). Bodies stay
    # exactly as the room retained them — AES-GCM envelopes only the room key
    # opens — so this endpoint hands out no plaintext; the caller decrypts with
    # the passphrase returned alongside, which the same authorization gates.
    row, record = await _readable_channel(
        runtime, account_bi, channel_id, is_admin)
    if not row:
        return _response(runtime, {"error": "not_found"}, status=404)
    can_manage = _can_manage(row, record, account_bi, is_admin)
    key_version = int(row.get("key_version") or 1)
    try:
        since = int(str(runtime.query_params().get("since", "0")) or 0)
    except (TypeError, ValueError):
        since = 0
    entries = await runtime.history(channel_id, key_version, max(0, since))
    await _mark_read(runtime, channel_id, account_bi)
    messages = [
        _history_message(entry)
        for entry in (entries or [])[-HISTORY_MAX_MESSAGES:]
    ]
    return _response(
        runtime,
        {
            "channel": _channel_payload(row, record, can_manage),
            "room": (
                "chat-channel:" + str(channel_id) + ":v" + str(key_version)
            ),
            "keyVersion": key_version,
            "passphrase": await runtime.channel_passphrase(
                channel_id, key_version),
            "messages": messages,
            "latestTs": max(
                [int(message["ts"]) for message in messages] + [since]),
        },
    )


async def _pending(runtime, account_bi, channel_id, is_admin):
    ref = str(runtime.query_params().get("ref") or "").strip().lower()
    if not REF_RE.fullmatch(ref):
        return _response(runtime, {"error": "invalid_ref"}, status=400)
    row, _record = await _readable_channel(
        runtime, account_bi, channel_id, is_admin)
    if not row:
        return _response(runtime, {"error": "not_found"}, status=404)
    key_version = int(row.get("key_version") or 1)
    room = "chat-channel:" + str(channel_id) + ":v" + str(key_version)
    stored = await runtime.d1_first(
        "SELECT body FROM chat_history WHERE room_key=? AND msg_id=?",
        room,
        ref,
    )
    frame = str((stored or {}).get("body") or "")
    if not frame:
        return _response(runtime, {"error": "not_found"}, status=404)
    return _response(
        runtime,
        {
            "channel": str(channel_id),
            "room": room,
            "keyVersion": key_version,
            "passphrase": await runtime.channel_passphrase(
                channel_id, key_version),
            "frame": frame,
        },
    )


async def _room_access(runtime, account_bi, actor, channel_id, is_admin):
    row, record = await _readable_channel(
        runtime, account_bi, channel_id, is_admin)
    if not row:
        return _response(runtime, {"error": "not_found"}, status=404)
    can_manage = _can_manage(row, record, account_bi, is_admin)
    access = await runtime.room_access(
        channel_id,
        int(row.get("key_version") or 1),
        account_bi,
        actor,
    )
    await _mark_read(runtime, channel_id, account_bi)
    payload = dict(access or {})
    payload["channel"] = _channel_payload(row, record, can_manage)
    return _response(runtime, payload)


def _invite_payload(runtime, row, record):
    token = str((record or {}).get("token") or "")
    return {
        "url": runtime.invite_url(token) if token else "",
        "createdAt": int((row or {}).get("created_at") or 0),
        "createdBy": str((record or {}).get("createdBy") or ""),
    }


async def _get_invite(runtime, channel_id):
    row = await runtime.d1_first(
        "SELECT data,created_at FROM chat_channel_invites WHERE channel_id=?",
        channel_id,
    )
    if not row:
        return _response(runtime, {"invite": None})
    try:
        record = await runtime.open(row.get("data"))
    except Exception:
        return _response(runtime, {"invite": None})
    return _response(
        runtime, {"invite": _invite_payload(runtime, row, record)})


async def _create_invite(runtime, account_bi, actor, channel_id):
    token = runtime.new_webhook_token()
    token_bi = await runtime.blind("chat-invite-token:" + token)
    now = runtime.now()
    record = {"token": token, "createdBy": actor}
    await runtime.d1_run(
        "INSERT INTO chat_channel_invites "
        "(channel_id,token_bi,data,created_by_bi,created_at) "
        "VALUES (?,?,?,?,?) ON CONFLICT(channel_id) DO UPDATE SET "
        "token_bi=excluded.token_bi,data=excluded.data,"
        "created_by_bi=excluded.created_by_bi,created_at=excluded.created_at",
        channel_id,
        token_bi,
        await runtime.seal(record),
        account_bi,
        now,
    )
    await runtime.audit(
        actor,
        "chat.channel.invite.create",
        "chat_channel",
        channel_id,
        details={"reason": "channel_manager_created_invite_link"},
    )
    return _response(
        runtime,
        {
            "ok": True,
            "invite": _invite_payload(runtime, {"created_at": now}, record),
        },
        status=201,
    )


async def _drop_invite(runtime, channel_id):
    existing = await runtime.d1_first(
        "SELECT channel_id FROM chat_channel_invites WHERE channel_id=?",
        channel_id,
    )
    if not existing:
        return False
    await runtime.d1_run(
        "DELETE FROM chat_channel_invites WHERE channel_id=?", channel_id)
    return True


async def _delete_invite(runtime, actor, channel_id):
    deleted = await _drop_invite(runtime, channel_id)
    await runtime.audit(
        actor,
        "chat.channel.invite.delete",
        "chat_channel",
        channel_id,
        details={"removed": bool(deleted)},
    )
    return _response(runtime, {"ok": True, "removed": bool(deleted)})


async def _invited_channel(runtime, token):
    token_bi = await runtime.blind("chat-invite-token:" + token)
    invite = await runtime.d1_first(
        "SELECT channel_id,data,created_by_bi FROM chat_channel_invites "
        "WHERE token_bi=?",
        token_bi,
    )
    if not invite:
        return None, None, None
    row, record = await _channel(runtime, str(invite.get("channel_id") or ""))
    if (not row or _is_notices(record) or _is_office(record)
            or str(invite.get("created_by_bi") or "")
            != str(row.get("created_by_bi") or "")):
        return None, None, None
    return invite, row, record


async def _invite_preview(runtime, account_bi, token, is_admin):
    invite, row, record = await _invited_channel(runtime, token)
    if not invite:
        return _response(runtime, {"error": "invite_not_found"}, status=404)
    channel_id = str(row.get("channel_id") or "")
    joined = (
        _visibility(record) == "public"
        or _is_creator(row, account_bi)
        or await _is_member(runtime, channel_id, account_bi)
    )
    can_manage = _can_manage(row, record, account_bi, is_admin)
    return _response(
        runtime,
        {
            "channel": _channel_payload(row, record, can_manage),
            "invitedBy": str((record or {}).get("createdBy") or ""),
            "joined": bool(joined),
        },
    )


async def _accept_invite(runtime, account_bi, actor, token, is_admin):
    invite, row, record = await _invited_channel(runtime, token)
    if not invite:
        return _response(runtime, {"error": "invite_not_found"}, status=404)
    channel_id = str(row.get("channel_id") or "")
    can_manage = _can_manage(row, record, account_bi, is_admin)
    joined = {
        "ok": True,
        "channel": _channel_payload(row, record, can_manage),
    }
    if (_visibility(record) == "public" or _is_creator(row, account_bi)
            or await _is_member(runtime, channel_id, account_bi)):
        joined["alreadyMember"] = True
        return _response(runtime, joined)
    count = await runtime.d1_first(
        "SELECT COUNT(*) AS n FROM chat_channel_members WHERE channel_id=?",
        channel_id,
    )
    if int((count or {}).get("n") or 0) >= MAX_CHANNEL_MEMBERS:
        return _response(runtime, {"error": "too_many_members"}, status=429)
    await runtime.d1_run(
        "INSERT INTO chat_channel_members "
        "(channel_id,member_bi,data,invited_by_bi,joined_at) "
        "VALUES (?,?,?,?,?) ON CONFLICT(channel_id,member_bi) DO NOTHING",
        channel_id,
        account_bi,
        await runtime.seal({"username": actor}),
        str(invite.get("created_by_bi") or ""),
        runtime.now(),
    )
    await runtime.notify_account(actor, "private-channels")
    creator = str((record or {}).get("createdBy") or "").strip().lower()
    if creator and creator != actor:
        await runtime.notify_account(creator, "private-channels")
    await runtime.audit(
        actor,
        "chat.channel.member.join",
        "chat_channel",
        channel_id,
        details={"reason": "invite_link"},
    )
    joined["alreadyMember"] = False
    return _response(runtime, joined, status=201)


async def handle(runtime, path):
    """Serve authenticated encrypted-channel collection and resources."""
    await runtime.ensure_schema()
    normalized_path = str(path or "")
    collection = COLLECTION_RE.fullmatch(normalized_path)
    channel = CHANNEL_RE.fullmatch(normalized_path)
    members = MEMBERS_RE.fullmatch(normalized_path)
    room_access = ROOM_ACCESS_RE.fullmatch(normalized_path)
    history = HISTORY_RE.fullmatch(normalized_path)
    pending = PENDING_RE.fullmatch(normalized_path)
    webhooks = WEBHOOKS_RE.fullmatch(normalized_path)
    webhook = WEBHOOK_RE.fullmatch(normalized_path)
    delivery = WEBHOOK_DELIVERY_RE.fullmatch(normalized_path)
    invite = INVITE_RE.fullmatch(normalized_path)
    invite_token = INVITE_TOKEN_RE.fullmatch(normalized_path)
    if not (
        collection or channel or members or room_access or history
        or pending or webhooks or webhook or delivery or invite
        or invite_token
    ):
        return _response(runtime, {"error": "not_found"}, status=404)
    method = runtime.method()
    body_required = method in {"POST", "PATCH", "DELETE"}
    data = None
    if body_required:
        data, error_response = await _body(runtime)
        if error_response is not None:
            return error_response

    # The random 256-bit URL token is the webhook's bearer capability. It is
    # deliberately handled before browser/desktop session authentication.
    if delivery:
        if method != "POST":
            return _response(
                runtime,
                {"error": "method_not_allowed"},
                status=405,
                allow="POST",
            )
        return await _deliver_webhook(runtime, delivery.group(1), data)

    account_bi, account = await runtime.session(data)
    actor = str((account or {}).get("name") or "").strip().lower()
    if not account_bi or not actor:
        return _response(
            runtime, {"error": "invalid_session"}, status=401)
    is_admin = await runtime.is_admin(actor)

    if collection and method == "GET":
        return await _list_channels(runtime, account_bi, actor, is_admin)
    if collection and method == "POST":
        return await _create_channel(runtime, account_bi, actor, data)

    if invite_token:
        if method == "GET":
            return await _invite_preview(
                runtime, account_bi, invite_token.group(1), is_admin)
        if method == "POST":
            return await _accept_invite(
                runtime, account_bi, actor, invite_token.group(1), is_admin)
        return _response(
            runtime,
            {"error": "method_not_allowed"},
            status=405,
            allow="GET, POST",
        )

    if channel and method in {"PATCH", "DELETE"}:
        channel_id = channel.group(1)
        row, record, denied = await _require_manager(
            runtime,
            account_bi,
            actor,
            "chat.channel.delete" if method == "DELETE"
            else "chat.channel.update",
            channel_id,
            is_admin,
        )
        if denied is not None:
            return denied
        if method == "DELETE":
            return await _delete_channel(runtime, actor, channel_id, row)
        return await _update_channel(
            runtime, actor, channel_id, data, row=row, record=record)

    matched_resource = (
        members or room_access or history or pending or webhooks or webhook
        or invite)
    if channel:
        return _response(
            runtime,
            {"error": "method_not_allowed"},
            status=405,
            allow="PATCH, DELETE",
        )
    channel_id = matched_resource.group(1)
    if members and method in {"GET", "POST", "DELETE"}:
        action = {
            "GET": "chat.channel.member.list",
            "POST": "chat.channel.member.add",
            "DELETE": "chat.channel.member.remove",
        }[method]
        _row, _record, denied = await _require_manager(
            runtime, account_bi, actor, action, channel_id, is_admin)
        if denied is not None:
            return denied
        if method == "GET":
            return await _list_members(runtime, channel_id)
        if method == "POST":
            return await _add_member(
                runtime, account_bi, actor, channel_id, data)
        return await _remove_member(runtime, actor, channel_id, data)

    if webhooks and method in {"GET", "POST"}:
        _row, record, denied = await _require_manager(
            runtime,
            account_bi,
            actor,
            "chat.channel.webhook.list" if method == "GET"
            else "chat.channel.webhook.create",
            channel_id,
            is_admin,
        )
        if denied is not None:
            return denied
        if method == "GET":
            return await _list_webhooks(runtime, channel_id)
        if _visibility(record) != "public":
            return _response(
                runtime, {"error": "public_channel_required"}, status=409)
        return await _create_webhook(
            runtime, account_bi, actor, channel_id, data)

    if webhook and method in {"POST", "DELETE"}:
        webhook_id = webhook.group(2)
        _row, record, denied = await _require_manager(
            runtime,
            account_bi,
            actor,
            "chat.channel.webhook.delete" if method == "DELETE"
            else "chat.channel.webhook.rotate",
            channel_id,
            is_admin,
        )
        if denied is not None:
            return denied
        if method == "POST":
            if _visibility(record) != "public":
                return _response(
                    runtime, {"error": "public_channel_required"}, status=409)
            return await _rotate_webhook(
                runtime, actor, channel_id, webhook_id)
        return await _delete_webhook(
            runtime, actor, channel_id, webhook_id)

    if invite and method in {"GET", "POST", "DELETE"}:
        action = {
            "GET": "chat.channel.invite.read",
            "POST": "chat.channel.invite.create",
            "DELETE": "chat.channel.invite.delete",
        }[method]
        _row, record, denied = await _require_manager(
            runtime, account_bi, actor, action, channel_id, is_admin)
        if denied is not None:
            return denied
        if method == "GET":
            return await _get_invite(runtime, channel_id)
        if method == "DELETE":
            return await _delete_invite(runtime, actor, channel_id)
        if _visibility(record) != "private":
            return _response(
                runtime, {"error": "private_channel_required"}, status=409)
        return await _create_invite(runtime, account_bi, actor, channel_id)

    if room_access and method == "GET":
        return await _room_access(
            runtime, account_bi, actor, channel_id, is_admin)

    if history and method == "GET":
        return await _history(runtime, account_bi, channel_id, is_admin)

    if pending and method == "GET":
        return await _pending(runtime, account_bi, channel_id, is_admin)

    allow = "GET, POST" if (collection or webhooks) else (
        "GET, POST, DELETE" if (members or invite) else (
            "POST, DELETE" if webhook else "GET"))
    return _response(
        runtime,
        {"error": "method_not_allowed"},
        status=405,
        allow=allow,
    )
