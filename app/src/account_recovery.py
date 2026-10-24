import base64
import re
import secrets

CODE_PREFIX = "RC"
CODE_GROUPS = 5
CODE_GROUP_LENGTH = 4
CODE_RE = re.compile(r"^RC[A-Z2-7]{20}$")
PASSWORD_MIN_LENGTH = 8
NO_STORE = "no-store, max-age=0, must-revalidate"
SUGGEST_ATTEMPTS = 6


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


def new_code(token_bytes=secrets.token_bytes):
    raw = base64.b32encode(token_bytes(13)).decode("ascii").rstrip("=")[:20]
    return format_code(CODE_PREFIX + raw)


def normalize(value):
    compact = re.sub(r"[^A-Za-z0-9]", "", str(value or "")).upper()
    compact = compact.replace("0", "O").replace("1", "I").replace("8", "B")
    if not compact.startswith(CODE_PREFIX):
        compact = CODE_PREFIX + compact
    return compact if CODE_RE.fullmatch(compact) else ""


def format_code(compact):
    body = compact[len(CODE_PREFIX):]
    return CODE_PREFIX + "-" + "-".join(
        body[index:index + CODE_GROUP_LENGTH]
        for index in range(0, len(body), CODE_GROUP_LENGTH))


async def seal(rec, code):
    compact = normalize(code)
    salt, digest = await hash_password(compact)  # noqa: F821
    rec["recovery_salt"] = salt
    rec["recovery_hash"] = digest
    rec["recovery_created_at"] = int(Date.now())  # noqa: F821
    return rec


async def matches(rec, code):
    compact = normalize(code)
    if not compact or not isinstance(rec, dict):
        return False
    salt = rec.get("recovery_salt") or ""
    digest = rec.get("recovery_hash") or ""
    if not salt or not digest:
        return False
    return bool(await verify_password(compact, salt, digest))  # noqa: F821


def has_code(rec):
    return bool(isinstance(rec, dict) and rec.get("recovery_hash"))


async def issue(rec):
    code = new_code()
    await seal(rec, code)
    return code


def _json(payload, status=200, extra_headers=None):
    return json_response(  # noqa: F821
        payload, status=status, cache_control=NO_STORE,
        extra_headers=extra_headers)


async def recover_handler(env, request):
    try:
        data = await bounded_json_request(request)  # noqa: F821
    except Exception:
        return _json({"error": "invalid_json"}, 400)
    if not isinstance(data, dict):
        return _json({"error": "invalid_json"}, 400)
    name = clean_string(  # noqa: F821
        data.get("nodeName", "") or data.get("identifier", ""),
        MAX_NODE_NAME).strip().lower()  # noqa: F821
    code = str(data.get("recoveryCode", "") or "")[:64]
    password = str(data.get("newPassword", "") or "")[:256]
    if len(password) < PASSWORD_MIN_LENGTH:
        return _json({"error": "password_too_short"}, 400)
    if not valid_node_name(name) or not normalize(code):  # noqa: F821
        return _json({"error": "invalid_recovery_code"}, 401)
    source_keys = await _login_attempt_keys(  # noqa: F821
        env, request, "recovery:" + name)
    account_key = await blind_index(env, "recovery-account:" + name)  # noqa: F821
    attempt_keys = list(source_keys) + [account_key]
    locked = 0
    for key in source_keys:
        locked = max(locked, await _login_locked_until(env, key))  # noqa: F821
    if locked:
        retry_ms = max(1000, locked - int(Date.now()))  # noqa: F821
        return _json({"error": "too_many_attempts", "retryAfterMs": retry_ms},
                     429, {"Retry-After": str(max(1, (retry_ms + 999) // 1000))})
    name_bi, rec = await _account_row(env, name)  # noqa: F821
    usable = bool(rec and rec.get("status") == "active"
                  and _account_kind(rec) == "user"  # noqa: F821
                  and has_code(rec))
    if not usable:
        await seal({}, code)
        return await _login_failure_response(  # noqa: F821
            env, attempt_keys, "invalid_recovery_code")
    if not await matches(rec, code):
        return await _login_failure_response(  # noqa: F821
            env, attempt_keys, "invalid_recovery_code")
    salt, digest = await hash_password(password)  # noqa: F821
    rec["pass_salt"] = salt
    rec["pass_hash"] = digest
    rec["password_changed_at"] = int(Date.now())  # noqa: F821
    fresh = await issue(rec)
    await _save_account(env, name_bi, rec)  # noqa: F821
    for key in attempt_keys:
        await _login_clear(env, key)  # noqa: F821
    await _account_revoke_sessions(env, name_bi)  # noqa: F821
    await _audit_sensitive_action(  # noqa: F821
        env, name, "account.recovery_code_reset", "account", name, "success",
        {})
    return _json({"ok": True, "nodeName": name, "recoveryCode": fresh})


async def regenerate_handler(env, request):
    try:
        data = await bounded_json_request(request)  # noqa: F821
    except Exception:
        return _json({"error": "invalid_json"}, 400)
    account_bi, rec = await _account_session_record(  # noqa: F821
        env, request, data)
    if not account_bi or not rec or rec.get("status") != "active":
        return _json({"error": "invalid_session"}, 401)
    if not rec.get("pass_hash"):
        return _json({"error": "password_not_set"}, 400)
    current = str(data.get("password", "") or "")[:256]
    attempt_key = await blind_index(  # noqa: F821
        env, "recovery-regenerate:" + str(account_bi))
    if await _login_locked_until(env, attempt_key):  # noqa: F821
        return _json({"error": "too_many_attempts"}, 429)
    if not await verify_password(  # noqa: F821
            current, rec.get("pass_salt", ""), rec.get("pass_hash", "")):
        return await _login_failure_response(  # noqa: F821
            env, [attempt_key], "invalid_credentials")
    code = await issue(rec)
    await _save_account(env, account_bi, rec)  # noqa: F821
    await _login_clear(env, attempt_key)  # noqa: F821
    actor = clean_string(rec.get("name", ""), MAX_NODE_NAME).lower()  # noqa: F821
    await _audit_sensitive_action(  # noqa: F821
        env, actor, "account.recovery_code_regenerate", "account", actor,
        "success", {})
    return _json({"ok": True, "recoveryCode": code})


async def _handle_taken(env, handle):
    if username_has_blocked_term(handle):  # noqa: F821
        return True
    _org_bi, org = await _org_row(env, handle)  # noqa: F821
    if org:
        return True
    _bi, rec = await _account_row(env, handle)  # noqa: F821
    return bool(rec)


async def suggest_handle(env):
    for _ in range(SUGGEST_ATTEMPTS):
        handle = _random_handles.suggest(  # noqa: F821
            reject=username_has_blocked_term)  # noqa: F821
        if handle and not await _handle_taken(env, handle):
            return handle
    return ""


async def suggestion_handler(env, request):
    del request
    handle = await suggest_handle(env)
    if not handle:
        return _json({"error": "suggestion_unavailable"}, 503)
    return _json({"ok": True, "handle": handle})
