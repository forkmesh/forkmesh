"""Self-set away flag and custom status (emoji, short text, auto-clear time).

Every signed-in client — web chat, the dashboard, the desktop app and both
phone apps — lets the member mark themselves away and wear a status such as
"🌴 On holiday until Monday" that clears itself at a chosen time. The state
lives in the encrypted account record (``user_status``) and rides the public
profile lookup and the chat user directory as ``userStatus``, so every surface
that paints a member's name can put the emoji beside it.

Split out of entry.py so Worker startup validation never parses this domain;
entry.py reaches it through a _LazyModule proxy.
"""

USER_STATUS_TEXT_MAX = 100
# The longest "clear after" a client may ask for. Anything longer is a
# status the member means to keep, which is what expiresAt=0 already says.
USER_STATUS_MAX_DURATION_MS = 366 * 24 * 60 * 60 * 1000
USER_STATUS_FIELDS = ("away", "emoji", "text", "expiresAt", "clear")


def _bind_runtime(runtime):
    """Supply the entrypoint primitives used by this domain."""
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


def clean_user_status_text(value):
    """One line of at most USER_STATUS_TEXT_MAX characters."""
    text = clean_string(value or "", USER_STATUS_TEXT_MAX * 4)
    text = " ".join(text.replace("\r", " ").replace("\n", " ").split())
    return text[:USER_STATUS_TEXT_MAX].strip()


def public_user_status(rec, now_ms=None):
    """The member's current away flag and unexpired custom status.

    Expiry is applied here, at read time, so an edge-cached profile or
    directory response can at most lag by its own cache lifetime; clients
    also compare ``expiresAt`` against their clock before painting.
    """
    stored = (rec or {}).get("user_status")
    if not isinstance(stored, dict):
        return {"away": False, "emoji": "", "text": "", "expiresAt": 0}
    now_ms = int(Date.now()) if now_ms is None else int(now_ms)
    try:
        expires_at = int(stored.get("expiresAt") or 0)
    except (TypeError, ValueError):
        expires_at = 0
    emoji = world_protocol.clean_status_emoji(stored.get("emoji", ""))
    text = clean_user_status_text(stored.get("text", ""))
    if expires_at and expires_at <= now_ms:
        emoji, text, expires_at = "", "", 0
    if not emoji and not text:
        expires_at = 0
    return {
        "away": bool(stored.get("away")),
        "emoji": emoji,
        "text": text,
        "expiresAt": expires_at,
    }


def _apply_user_status(current, data, now_ms):
    """Merge one request into the current status; return (status, error).

    Only the keys the request carries change: ``{"away": true}`` leaves the
    custom status alone, and ``{"emoji": ..., "text": ...}`` leaves the away
    flag alone. ``clear`` drops the custom status (never the away flag).
    """
    status = dict(current)
    if "away" in data:
        status["away"] = bool(data.get("away"))
    custom_keys = ("emoji", "text", "expiresAt")
    if data.get("clear"):
        status.update({"emoji": "", "text": "", "expiresAt": 0})
    elif any(key in data for key in custom_keys):
        raw_emoji = str(data.get("emoji", status["emoji"]) or "").strip()
        emoji = world_protocol.clean_status_emoji(raw_emoji)
        if raw_emoji and not emoji:
            return None, "bad_status_emoji"
        raw_text = data.get("text", status["text"])
        if len(str(raw_text or "")) > USER_STATUS_TEXT_MAX * 4:
            return None, "status_text_too_long"
        text = clean_user_status_text(raw_text)
        try:
            expires_at = int(data.get("expiresAt", status["expiresAt"]) or 0)
        except (TypeError, ValueError):
            return None, "bad_status_expiry"
        if expires_at < 0:
            return None, "bad_status_expiry"
        if expires_at and expires_at <= now_ms:
            return None, "status_expiry_in_past"
        if expires_at > now_ms + USER_STATUS_MAX_DURATION_MS:
            return None, "status_expiry_too_far"
        if not emoji and not text:
            expires_at = 0
        status.update({"emoji": emoji, "text": text, "expiresAt": expires_at})
    return status, ""


async def account_user_status(env, request):
    """POST /api/accounts/user-status — set away and/or the custom status.

    Session-authenticated (cookie, bearer or ``sessionToken``). The body
    carries any of ``away`` (bool), ``emoji``, ``text``, ``expiresAt`` (epoch
    ms, 0 = never clears) and ``clear`` (drop the custom status). An empty
    body is a read of the caller's own current status.
    """
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    if not isinstance(data, dict):
        return json_response({"error": "invalid_json"}, status=400)
    name_bi, rec = await _account_session_record(env, request, data)
    if not rec:
        return json_response({"error": "unauthorized"}, status=401)
    now_ms = int(Date.now())
    current = public_user_status(rec, now_ms)
    status, error = _apply_user_status(current, data, now_ms)
    if error:
        return json_response({"error": error}, status=400)
    keep = bool(status["away"] or status["emoji"] or status["text"])
    # An expired status still stored in the record is pruned on the next
    # write-or-read through here, not left to accumulate.
    if status != current or (not keep and "user_status" in rec):
        if keep:
            rec["user_status"] = {**status, "updatedAt": now_ms}
        else:
            rec.pop("user_status", None)
        await _save_account(env, name_bi, rec)
        profile_name = clean_string(
            rec.get("name", ""), MAX_NODE_NAME).lower()
        # Sequential — see the no-concurrent-tasks rule in entry.py.
        for cache_key in (
            ACCOUNT_LOOKUP_CACHE_PREFIX + quote(profile_name),
            USERS_DIRECTORY_CACHE_KEY,
            USERS_WORLD_CACHE_KEY,
        ):
            try:
                await edge_cache_delete(cache_key)
            except Exception:
                pass
    return json_response({
        "ok": True,
        "nodeName": rec.get("name", ""),
        "userStatus": public_user_status(rec, now_ms),
    })


CHAT_RETENTION_MIN_MS = 60 * 60 * 1000
CHAT_RETENTION_MAX_MS = 7 * 24 * 60 * 60 * 1000
CHAT_RETENTION_KEY_MAX = 160
CHAT_RETENTION_MAX_CHATS = 500


def _clamp_chat_retention_ms(value):
    if isinstance(value, bool):
        raise ValueError("bad_chat_retention")
    if isinstance(value, float) and value != value:
        raise ValueError("bad_chat_retention")
    ms = int(value)
    if ms <= 0:
        raise ValueError("bad_chat_retention")
    return max(CHAT_RETENTION_MIN_MS, min(CHAT_RETENTION_MAX_MS, ms))


def _clean_chat_retention_key(value):
    if not isinstance(value, str):
        return ""
    key = value.strip()
    if not key or len(key) > CHAT_RETENTION_KEY_MAX:
        return ""
    if any(ord(ch) < 32 or ord(ch) == 127 for ch in key):
        return ""
    return key


def account_chat_retention(rec):
    stored = (rec or {}).get("chat_retention")
    if not isinstance(stored, dict):
        stored = {}
    try:
        default_ms = _clamp_chat_retention_ms(stored.get("defaultMs"))
    except (TypeError, ValueError):
        default_ms = CHAT_RETENTION_MAX_MS
    chats = {}
    raw_chats = stored.get("chats")
    if isinstance(raw_chats, dict):
        for raw_key, raw_ms in raw_chats.items():
            key = _clean_chat_retention_key(raw_key)
            if not key:
                continue
            try:
                chats[key] = _clamp_chat_retention_ms(raw_ms)
            except (TypeError, ValueError):
                continue
            if len(chats) >= CHAT_RETENTION_MAX_CHATS:
                break
    try:
        updated_at = max(0, int(stored.get("updatedAt") or 0))
    except (TypeError, ValueError):
        updated_at = 0
    return {"defaultMs": default_ms, "chats": chats, "updatedAt": updated_at}


def _apply_chat_retention(current, data):
    stored = {"chats": dict(current["chats"])}
    if current["defaultMs"] != CHAT_RETENTION_MAX_MS:
        stored["defaultMs"] = current["defaultMs"]
    if "defaultMs" in data:
        raw = data.get("defaultMs")
        if raw is None:
            stored.pop("defaultMs", None)
        else:
            try:
                ms = _clamp_chat_retention_ms(raw)
            except (TypeError, ValueError):
                return None, "invalid_chat_retention"
            if ms == CHAT_RETENTION_MAX_MS:
                stored.pop("defaultMs", None)
            else:
                stored["defaultMs"] = ms
    if "chat" in data or "retentionMs" in data:
        key = _clean_chat_retention_key(data.get("chat"))
        if not key:
            return None, "invalid_chat_retention"
        raw = data.get("retentionMs")
        if raw is None:
            stored["chats"].pop(key, None)
        else:
            try:
                ms = _clamp_chat_retention_ms(raw)
            except (TypeError, ValueError):
                return None, "invalid_chat_retention"
            if key not in stored["chats"] and (
                    len(stored["chats"]) >= CHAT_RETENTION_MAX_CHATS):
                return None, "too_many_chat_retention_overrides"
            stored["chats"][key] = ms
    return stored, ""


async def account_chat_retention_route(env, request):
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    if not isinstance(data, dict):
        return json_response({"error": "invalid_json"}, status=400)
    name_bi, rec = await _account_session_record(env, request, data)
    if not rec:
        return json_response({"error": "unauthorized"}, status=401)
    current = account_chat_retention(rec)
    stored, error = _apply_chat_retention(current, data)
    if error:
        return json_response({"error": error}, status=400)
    before = {"chats": current["chats"]}
    if current["defaultMs"] != CHAT_RETENTION_MAX_MS:
        before["defaultMs"] = current["defaultMs"]
    if stored != before:
        if stored["chats"] or "defaultMs" in stored:
            rec["chat_retention"] = {**stored, "updatedAt": int(Date.now())}
        else:
            rec.pop("chat_retention", None)
        await _save_account(env, name_bi, rec)
    return json_response({
        "ok": True,
        "nodeName": rec.get("name", ""),
        "chatRetention": account_chat_retention(rec),
    })
