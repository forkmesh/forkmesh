"""Patreon webhook runtime handler.

This module imports the Workers runtime (js/workers) and is responsible for
the live database updates. Pure-Python helpers live in patreon.py.
"""

import base64
import hashlib
import hmac
import json
import re
from urllib.parse import parse_qs, urlencode, urlparse

import patreon


_ENTRY_SYMBOLS = frozenset({
    "Date", "Response", "_account_session_record", "_audit_sensitive_action",
    "_contribution_run_batch", "_random_bytes", "_request_account_session_id",
    "_request_same_origin", "blind_index", "decrypt_row", "d1_first", "d1_run",
    "ensure_schema", "encrypt_row", "js_fetch_with_timeout", "json_response",
    "method_name",
})

OAUTH_STATE_TTL_MS = 10 * 60 * 1000
MAX_PROVIDER_RESPONSE_BYTES = 256 * 1024
PROVIDER_TIMEOUT_SECONDS = 12
PATREON_AUTHORIZE_URL = "https://www.patreon.com/oauth2/authorize"
PATREON_TOKEN_URL = "https://www.patreon.com/api/oauth2/token"
PATREON_IDENTITY_URL = "https://www.patreon.com/api/oauth2/v2/identity"
PATREON_MEMBER_API_BASE = "https://www.patreon.com/api/oauth2/v2/members/"
PATREON_CALLBACK_OUTCOMES = frozenset({
    "connected", "denied", "duplicate_identity", "expired", "failed",
    "invalid", "invalid_membership", "mismatch", "not_configured", "replayed",
    "wrong_campaign",
})
_STATE_RE = re.compile(r"^[A-Za-z0-9_-]{43}$")
_MEMBER_ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,128}$")
_MEMBER_URL_RE = re.compile(
    r"^https://www\.patreon\.com/api/oauth2/v2/members/[A-Za-z0-9_-]{1,128}$")


def __getattr__(name):
    """Late-bind entry.py symbols so module import stays cycle-free.

    entry.py imports this module lazily inside the route dispatcher, so by
    the time any handler code runs, entry is fully initialized. Plain module
    attribute access keeps the handler bodies free of indirection.
    """
    if name in _ENTRY_SYMBOLS:
        import entry
        return getattr(entry, name)
    raise AttributeError(name)


def _bind_entry_symbols():
    """Populate late runtime globals after entry.py has finished importing."""
    import entry
    namespace = globals()
    for name in _ENTRY_SYMBOLS:
        namespace[name] = getattr(entry, name)


# Mirrors entry.EXPECTED_DEGRADED_HEADER: a 503 answered on purpose (Patreon
# not configured, reconciliation unavailable) is not a Worker error.
_EXPECTED_DEGRADED_HEADER = "x-forkmesh-expected-degraded"


def _no_store(data, status=200, allow=""):
    headers = {"x-content-type-options": "nosniff"}
    if allow:
        headers["allow"] = allow
    if int(status) == 503:
        headers[_EXPECTED_DEGRADED_HEADER] = "1"
    return json_response(
        data, status=status,
        cache_control="no-store, max-age=0, must-revalidate",
        extra_headers=headers)


def _query_value(request, name):
    try:
        return str(parse_qs(
            urlparse(str(request.url)).query,
            keep_blank_values=True).get(name, [""])[0] or "")
    except Exception:
        return ""


def _oauth_config(env):
    client_id = str(getattr(env, "PATREON_CLIENT_ID", "") or "").strip()
    client_secret = str(
        getattr(env, "PATREON_CLIENT_SECRET", "") or "").strip()
    campaign_id = str(
        getattr(env, "PATREON_CAMPAIGN_ID", "") or "").strip()
    redirect_uri = str(
        getattr(env, "PATREON_OAUTH_REDIRECT_URI", "") or "").strip()
    if (
        not client_id or len(client_id) > 256
        or not client_secret or len(client_secret) > 4096
        or not campaign_id or len(campaign_id) > 128
        or not redirect_uri or len(redirect_uri) > 2048
    ):
        return None
    parsed = urlparse(redirect_uri)
    if (
        parsed.scheme != "https"
        or not parsed.hostname
        or parsed.username
        or parsed.password
        or parsed.query
        or parsed.fragment
        or parsed.path.rstrip("/") != "/api/integrations/patreon/callback"
    ):
        return None
    return {
        "clientId": client_id,
        "clientSecret": client_secret,
        "campaignId": campaign_id,
        "redirectUri": redirect_uri,
    }


def _new_state():
    return base64.urlsafe_b64encode(_random_bytes(32)).decode("ascii").rstrip("=")


def _state_hash(value):
    return hashlib.sha256(str(value or "").encode("utf-8")).hexdigest()


def _callback_response(outcome):
    outcome = str(outcome or "invalid")
    if outcome not in PATREON_CALLBACK_OUTCOMES:
        outcome = "invalid"
    return Response("", status=303, headers={
        "location": "/dashboard/settings/account?patreon=" + outcome,
        "cache-control": "no-store, max-age=0, must-revalidate",
        "referrer-policy": "no-referrer",
        "x-content-type-options": "nosniff",
    })


def _stream_result_field(result, name, default=None):
    if isinstance(result, dict):
        return result.get(name, default)
    return getattr(result, name, default)


def _chunk_bytes(value):
    if value is None:
        return b""
    if isinstance(value, bytes):
        return value
    if isinstance(value, bytearray):
        return bytes(value)
    if isinstance(value, str):
        return value.encode("utf-8")
    converter = getattr(value, "to_py", None)
    if callable(converter):
        value = converter()
    return bytes(value)


async def _cancel_reader(reader):
    try:
        result = reader.cancel()
        if hasattr(result, "__await__"):
            await result
    except Exception:
        pass


async def _bounded_provider_body(response):
    """Read a provider response without ever buffering beyond the cap."""
    body = getattr(response, "body", None)
    get_reader = getattr(body, "getReader", None)
    if callable(get_reader):
        reader = get_reader()
        chunks = []
        total = 0
        while True:
            result = await reader.read()
            if bool(_stream_result_field(result, "done", False)):
                break
            chunk = _chunk_bytes(_stream_result_field(result, "value"))
            total += len(chunk)
            if total > MAX_PROVIDER_RESPONSE_BYTES:
                await _cancel_reader(reader)
                return None
            chunks.append(chunk)
        return b"".join(chunks)

    # Lightweight test/runtime adapters may expose an async iterable rather
    # than the JavaScript ReadableStream reader API.
    if hasattr(body, "__aiter__"):
        chunks = []
        total = 0
        async for value in body:
            chunk = _chunk_bytes(value)
            total += len(chunk)
            if total > MAX_PROVIDER_RESPONSE_BYTES:
                return None
            chunks.append(chunk)
        return b"".join(chunks)

    # Legacy fakes without a body stream remain supported. Production
    # Responses use one of the bounded streaming paths above.
    raw = str(await response.text()).encode("utf-8")
    return raw if len(raw) <= MAX_PROVIDER_RESPONSE_BYTES else None


async def _provider_json(url, method="GET", form=None, access_token=""):
    member_request = bool(_MEMBER_URL_RE.fullmatch(url))
    if url not in {PATREON_TOKEN_URL, PATREON_IDENTITY_URL} and not member_request:
        return 400, None
    headers = {
        "accept": "application/json",
        "user-agent": "ForkMesh-Patreon-oauth/1.0",
    }
    options = {"method": method, "headers": headers, "redirect": "manual"}
    if method == "POST" and url == PATREON_TOKEN_URL and isinstance(form, dict):
        headers["content-type"] = "application/x-www-form-urlencoded"
        options["body"] = urlencode(form)
    elif method == "GET" and url == PATREON_IDENTITY_URL:
        token = str(access_token or "")
        if not token or len(token) > 4096 or any(
                not char.isprintable() or char.isspace() for char in token):
            return 400, None
        headers["authorization"] = "Bearer " + token
        url += (
            "?include=memberships&fields%5Bmember%5D="
            "currently_entitled_amount_cents,patron_status,next_charge_date")
    elif method == "GET" and member_request:
        token = str(access_token or "")
        if not token or len(token) > 4096 or any(
                not char.isprintable() or char.isspace() for char in token):
            return 400, None
        headers["authorization"] = "Bearer " + token
        url += (
            "?include=user,campaign&fields%5Bmember%5D="
            "currently_entitled_amount_cents,email,patron_status,next_charge_date"
            "&fields%5Buser%5D=email")
    else:
        return 400, None
    try:
        response = await js_fetch_with_timeout(
            url, options, PROVIDER_TIMEOUT_SECONDS)
        status = max(0, min(599, int(getattr(response, "status", 0) or 0)))
        try:
            announced = int(response.headers.get("content-length") or 0)
        except (AttributeError, TypeError, ValueError, OverflowError):
            announced = 0
        if announced > MAX_PROVIDER_RESPONSE_BYTES:
            return 502, None
        raw_bytes = await _bounded_provider_body(response)
        if raw_bytes is None:
            return 502, None
        raw = raw_bytes.decode("utf-8")
        data = json.loads(raw) if raw else None
        return status, data if isinstance(data, dict) else None
    except Exception:
        return 0, None


def _member_api_url(member_id):
    member_id = str(member_id or "").strip()
    if not _MEMBER_ID_RE.fullmatch(member_id):
        return ""
    return PATREON_MEMBER_API_BASE + member_id


async def _find_user_by_patreon_id(env, patreon_user_id):
    if not patreon_user_id:
        return None, None
    patreon_user_bi = await blind_index(env, patreon_user_id)
    link = await d1_first(
        env,
        "SELECT account_bi FROM patreon_account_links WHERE patreon_user_bi=?",
        patreon_user_bi)
    account_bi = str((link or {}).get("account_bi") or "")
    if not account_bi:
        return None, None
    row = await d1_first(
        env, "SELECT data FROM users WHERE user_bi=?", account_bi)
    rec = await decrypt_row(env, (row or {}).get("data", ""))
    # A durable identity match suppresses email fallback even if its account
    # row is temporarily unavailable or corrupt.
    return account_bi, rec or None


async def _find_user_by_email(env, email):
    """Look up a ForkMesh user by email blind index.

    Returns ``(name_bi, rec)`` on success, ``(None, None)`` when no match.
    """
    if not email:
        return None, None
    email_bi = await blind_index(env, email)
    row = await d1_first(
        env,
        "SELECT user_bi AS name_bi, data FROM users WHERE email_bi=?",
        email_bi,
    )
    if not row:
        return None, None
    name_bi = row.get("name_bi", "") or ""
    rec = await decrypt_row(env, row.get("data", "")) or {}
    if not name_bi or not rec:
        return None, None
    return name_bi, rec


class _StaleAccountState(Exception):
    pass


class _SupersededReconciliation(Exception):
    pass


def _guard_exists_sql():
    return (
        "EXISTS (SELECT 1 FROM users WHERE user_bi=? AND data=?) "
        "AND EXISTS (SELECT 1 FROM account_sessions WHERE account_bi=? "
        "AND session_id=? AND revoked_at=0 AND expires_at>?)"
    )


def _guard_params(guard):
    return (
        guard["account_bi"], guard["user_data"], guard["account_bi"],
        guard["session_id"], guard["now"],
    )


def _generation_exists_sql():
    return (
        "EXISTS (SELECT 1 FROM patreon_reconciliation_generations "
        "WHERE member_bi=? AND generation=?)"
    )


def _generation_params(generation_guard):
    return (
        generation_guard["member_bi"], generation_guard["generation"],
    )


def _append_guards(sql, params, guard=None, generation_guard=None, where=False):
    clauses = []
    if guard:
        clauses.append(_guard_exists_sql())
        params += _guard_params(guard)
    if generation_guard:
        clauses.append(_generation_exists_sql())
        params += _generation_params(generation_guard)
    if not clauses:
        return sql, params
    joiner = " WHERE " if where else " AND "
    return sql + joiner + " AND ".join(clauses), params


async def _claim_reconciliation_generation(env, member_id):
    """Bump the per-member reconciliation generation before provider I/O."""
    member_bi = await blind_index(env, "patreon-member-v1:" + str(member_id))
    now = int(Date.now())
    row = await d1_first(
        env,
        "INSERT INTO patreon_reconciliation_generations "
        "(member_bi,generation,updated_at) VALUES (?,1,?) "
        "ON CONFLICT(member_bi) DO UPDATE SET "
        "generation=generation+1,updated_at=excluded.updated_at "
        "RETURNING member_bi, generation",
        member_bi, now,
    )
    if not row:
        return None
    try:
        generation = int(row.get("generation") or 0)
    except (TypeError, ValueError):
        return None
    if generation < 1:
        return None
    return {
        "member_bi": str(row.get("member_bi") or member_bi),
        "generation": generation,
    }


async def _generation_is_current(env, generation_guard):
    row = await d1_first(
        env,
        "SELECT generation FROM patreon_reconciliation_generations "
        "WHERE member_bi=?",
        generation_guard["member_bi"],
    )
    try:
        current = int((row or {}).get("generation") or 0)
    except (TypeError, ValueError):
        return False
    return current == generation_guard["generation"]


async def _apply_membership(
        env, account_bi, rec, amount_cents, expires_at, deleted=False,
        event_type="sync", extra_statements=None, guard=None,
        generation_guard=None):
    """Atomically apply roles, account projection, link/event writes, and audit."""
    now = int(Date.now())
    if not deleted and amount_cents > 0 and expires_at <= now:
        raise ValueError("invalid_membership_expiry")
    tier = (
        "free" if deleted
        else patreon._tier_from_amount_cents(amount_cents)
    )

    next_rec = dict(rec or {})
    if tier in ("pro", "builder", "studio"):
        next_rec["account_tier"] = "pro"
        next_rec["supporting_member"] = True
        next_rec["patreon_pro_until"] = expires_at
    elif tier == "supporter":
        next_rec["supporting_member"] = True
        next_rec.pop("account_tier", None)
        next_rec.pop("patreon_pro_until", None)
    else:
        next_rec.pop("account_tier", None)
        next_rec.pop("supporting_member", None)
        next_rec.pop("patreon_pro_until", None)
    if tier == "free":
        next_rec.pop("patreon_tier", None)
    else:
        next_rec["patreon_tier"] = tier
    next_rec["patreon_synced_at"] = now
    encrypted_user = await encrypt_row(env, next_rec)

    statements = list(extra_statements or [])
    for role in (
        patreon.STUDIO_ROLE,
        patreon.BUILDER_ROLE,
        patreon.PRO_ROLE,
        patreon.SUPPORTER_ROLE,
    ):
        sql = (
            "UPDATE role_grants SET revoked_at=? WHERE account_bi=? "
            "AND role=? AND scope_type=? AND revoked_at=0"
        )
        params = (now, account_bi, role, patreon.SCOPE_TYPE_PLATFORM)
        sql, params = _append_guards(
            sql, params, guard=guard, generation_guard=generation_guard)
        statements.append((
            sql,
            params,
        ))
    roles_to_grant = patreon._roles_for_tier(tier)
    for role in roles_to_grant:
        sql = (
            "INSERT INTO role_grants "
            "(account_bi,role,scope_type,scope_bi,granted_by_bi,"
            "granted_at,expires_at,revoked_at) SELECT ?,?,?,?,?,?,?,0"
        )
        params = (
            account_bi, role, patreon.SCOPE_TYPE_PLATFORM, "", account_bi,
            now, expires_at,
        )
        sql, params = _append_guards(
            sql, params, guard=guard, generation_guard=generation_guard,
            where=True)
        sql += (
            " ON CONFLICT(account_bi,role,scope_type,scope_bi) DO UPDATE SET "
            "granted_by_bi=excluded.granted_by_bi,"
            "granted_at=excluded.granted_at,"
            "expires_at=excluded.expires_at,revoked_at=0"
        )
        statements.append((
            sql,
            params,
        ))
    audit_sql = (
        "INSERT INTO sensitive_audit_log "
        "(ts,actor_bi,actor_label,action,target_type,target_token,"
        "outcome,details) SELECT ?,?,?,?,?,?,?,?"
    )
    audit_params = (
        now, account_bi, "", "patreon.membership.sync", "account", "",
        "success", json.dumps(
            _audit_event(event_type, account_bi, tier, amount_cents),
            sort_keys=True, separators=(",", ":")),
    )
    audit_sql, audit_params = _append_guards(
        audit_sql, audit_params, guard=guard,
        generation_guard=generation_guard, where=True)
    statements.append((audit_sql, audit_params))
    if guard:
        sql = (
            "UPDATE users SET data=? WHERE user_bi=? AND data=? "
            "AND EXISTS (SELECT 1 FROM account_sessions WHERE account_bi=? "
            "AND session_id=? AND revoked_at=0 AND expires_at>?)"
        )
        params = (
            encrypted_user, account_bi, guard["user_data"], account_bi,
            guard["session_id"], guard["now"],
        )
        sql, params = _append_guards(
            sql, params, generation_guard=generation_guard)
        statements.append((sql, params))
    else:
        sql = "UPDATE users SET data=? WHERE user_bi=?"
        params = (encrypted_user, account_bi)
        sql, params = _append_guards(
            sql, params, generation_guard=generation_guard)
        statements.append((sql, params))
    await _contribution_run_batch(env, statements)
    if generation_guard and not await _generation_is_current(
            env, generation_guard):
        raise _SupersededReconciliation()
    if guard:
        persisted = await d1_first(
            env, "SELECT data FROM users WHERE user_bi=?", account_bi)
        if not hmac.compare_digest(
                str((persisted or {}).get("data") or ""), encrypted_user):
            raise _StaleAccountState()
    return tier


def _link_projection(
        tier, patron_status, amount_cents, expires_at, now):
    return {
        "tier": tier,
        "patronStatus": str(patron_status or "")[:64],
        "amountCents": amount_cents,
        "membershipExpiresAt": expires_at,
        "verifiedAt": now,
    }


def _audit_event(event_type, account_bi, tier, amount_cents):
    """Build a privacy-safe audit detail dict (no PII)."""
    return {
        "source": "patreon",
        "event": event_type,
        "tier": tier,
        "amountCents": amount_cents,
    }


async def patreon_account_renamed(env, old_account_bi, new_account_bi):
    """Atomically retarget durable Patreon state after an account rename."""
    _bind_entry_symbols()
    await _contribution_run_batch(env, [
        (
            "DELETE FROM patreon_oauth_states WHERE account_bi=?",
            (old_account_bi,),
        ),
        (
            "UPDATE patreon_account_links SET account_bi=? WHERE account_bi=?",
            (new_account_bi, old_account_bi),
        ),
        (
            "UPDATE patreon_webhook_events SET account_bi=? WHERE account_bi=?",
            (new_account_bi, old_account_bi),
        ),
        (
            "UPDATE role_grants SET account_bi=? WHERE account_bi=?",
            (new_account_bi, old_account_bi),
        ),
    ])


async def patreon_account_deleted(env, account_bi):
    """Atomically remove Patreon state and anonymize retained event metadata."""
    _bind_entry_symbols()
    await _contribution_run_batch(env, [
        (
            "DELETE FROM patreon_oauth_states WHERE account_bi=?",
            (account_bi,),
        ),
        (
            "DELETE FROM patreon_account_links WHERE account_bi=?",
            (account_bi,),
        ),
        (
            "UPDATE patreon_webhook_events SET account_bi='' WHERE account_bi=?",
            (account_bi,),
        ),
        (
            "DELETE FROM role_grants WHERE account_bi=?",
            (account_bi,),
        ),
    ])


async def patreon_connection_handler(env, request):
    """GET the authenticated account's display-safe Patreon projection."""
    _bind_entry_symbols()
    if method_name(request) != "GET":
        return _no_store({"error": "method_not_allowed"}, 405, "GET")
    await ensure_schema(env)
    account_bi, _rec = await _account_session_record(env, request, {})
    if not account_bi:
        return _no_store({"error": "invalid_session"}, 401)
    row = await d1_first(
        env,
        "SELECT data,linked_at,updated_at FROM patreon_account_links "
        "WHERE account_bi=?",
        account_bi)
    projection = {}
    if row:
        projection = await decrypt_row(env, row.get("data", "")) or {}
    return _no_store({
        "ok": True,
        "configured": bool(_oauth_config(env)),
        "connected": bool(row),
        "tier": str(projection.get("tier") or "core")[:16],
        "patronStatus": str(projection.get("patronStatus") or "")[:64],
        "linkedAt": int((row or {}).get("linked_at") or 0),
        "updatedAt": int((row or {}).get("updated_at") or 0),
    })


async def patreon_oauth_start_handler(env, request):
    """POST an authenticated, same-origin, account-bound OAuth start."""
    _bind_entry_symbols()
    if method_name(request) != "POST":
        return _no_store({"error": "method_not_allowed"}, 405, "POST")
    if not _request_same_origin(request):
        return _no_store({"error": "origin_not_allowed"}, 403)
    try:
        raw = str(await request.text())
        if len(raw.encode("utf-8")) > 4096:
            return _no_store({"error": "payload_too_large"}, 413)
        data = json.loads(raw) if raw.strip() else {}
    except Exception:
        return _no_store({"error": "invalid_json"}, 400)
    if not isinstance(data, dict):
        return _no_store({"error": "invalid_json"}, 400)
    if set(data) - {"sessionToken"}:
        return _no_store({"error": "unsupported_oauth_start_field"}, 400)
    await ensure_schema(env)
    account_bi, rec = await _account_session_record(env, request, data)
    session_id = _request_account_session_id(request, data)
    if not account_bi or not rec or not session_id:
        return _no_store({"error": "invalid_session"}, 401)
    config = _oauth_config(env)
    if not config:
        return _no_store({"error": "patreon_oauth_not_configured"}, 503)
    state = _new_state()
    if not _STATE_RE.fullmatch(state):
        return _no_store({"error": "state_generation_failed"}, 500)
    now = int(Date.now())
    state_record = {
        "state": state,
        "accountBi": account_bi,
        "sessionId": session_id,
        "redirectUri": config["redirectUri"],
        "createdAt": now,
    }
    encrypted = await encrypt_row(env, state_record)
    await d1_run(
        env, "DELETE FROM patreon_oauth_states WHERE expires_at<=?", now)
    await d1_run(
        env,
        "INSERT INTO patreon_oauth_states "
        "(state_hash,account_bi,data,created_at,expires_at) VALUES (?,?,?,?,?)",
        _state_hash(state), account_bi, encrypted, now,
        now + OAUTH_STATE_TTL_MS)
    authorization_url = PATREON_AUTHORIZE_URL + "?" + urlencode({
        "response_type": "code",
        "client_id": config["clientId"],
        "redirect_uri": config["redirectUri"],
        "scope": "identity identity.memberships",
        "state": state,
    })
    await _audit_sensitive_action(
        env, account_bi, "patreon.oauth.start",
        target_type="account", target=account_bi, outcome="success",
        details={"source": "patreon"})
    return _no_store({
        "ok": True,
        "authorizationUrl": authorization_url,
        "expiresAt": now + OAUTH_STATE_TTL_MS,
    })


async def _consume_oauth_state(env, state):
    if not _STATE_RE.fullmatch(state):
        return None, "invalid"
    state_hash = _state_hash(state)
    row = await d1_first(
        env,
        "SELECT account_bi,data,expires_at FROM patreon_oauth_states "
        "WHERE state_hash=?",
        state_hash)
    if not row:
        return None, "replayed"
    expires_at = int(row.get("expires_at") or 0)
    encrypted = str(row.get("data") or "")
    if expires_at <= int(Date.now()):
        await d1_run(
            env, "DELETE FROM patreon_oauth_states WHERE state_hash=? AND data=?",
            state_hash, encrypted)
        return None, "expired"
    claim = "consumed:" + _new_state()
    await d1_run(
        env,
        "UPDATE patreon_oauth_states SET data=?,expires_at=0 "
        "WHERE state_hash=? AND data=? AND expires_at=?",
        claim, state_hash, encrypted, expires_at)
    claimed = await d1_first(
        env, "SELECT data FROM patreon_oauth_states WHERE state_hash=?",
        state_hash)
    if not claimed or not hmac.compare_digest(
            str(claimed.get("data") or ""), claim):
        return None, "replayed"
    await d1_run(
        env, "DELETE FROM patreon_oauth_states WHERE state_hash=? AND data=?",
        state_hash, claim)
    record = await decrypt_row(env, encrypted)
    if (
        not isinstance(record, dict)
        or not hmac.compare_digest(str(record.get("state") or ""), state)
        or not hmac.compare_digest(
            str(record.get("accountBi") or ""),
            str(row.get("account_bi") or ""))
    ):
        return None, "mismatch"
    return record, ""


async def patreon_oauth_callback_handler(env, request):
    """GET the Patreon callback and discard all provider bearer credentials."""
    _bind_entry_symbols()
    try:
        return await _complete_oauth_callback(env, request)
    except Exception:
        return _callback_response("failed")


async def _complete_oauth_callback(env, request):
    if method_name(request) != "GET":
        return _callback_response("invalid")
    await ensure_schema(env)
    state = _query_value(request, "state")
    record, state_outcome = await _consume_oauth_state(env, state)
    if _query_value(request, "error"):
        return _callback_response("denied")
    if not record:
        return _callback_response(state_outcome)
    config = _oauth_config(env)
    if not config:
        return _callback_response("not_configured")
    if not hmac.compare_digest(
            str(record.get("redirectUri") or ""), config["redirectUri"]):
        return _callback_response("mismatch")
    account_bi = str(record.get("accountBi") or "")
    session_id = str(record.get("sessionId") or "")
    session = await d1_first(
        env,
        "SELECT session_id FROM account_sessions WHERE account_bi=? "
        "AND session_id=? AND revoked_at=0 AND expires_at>?",
        account_bi, session_id, int(Date.now()))
    user_row = await d1_first(
        env, "SELECT data FROM users WHERE user_bi=?", account_bi)
    rec = await decrypt_row(env, (user_row or {}).get("data", "")) or {}
    if not session or not rec or rec.get("status") != "active":
        return _callback_response("mismatch")
    code = _query_value(request, "code").strip()
    if (
        len(code) < 8 or len(code) > 2048
        or any(not char.isprintable() or char.isspace() for char in code)
    ):
        return _callback_response("invalid")
    token_status, token_data = await _provider_json(
        PATREON_TOKEN_URL, "POST", {
            "code": code,
            "grant_type": "authorization_code",
            "client_id": config["clientId"],
            "client_secret": config["clientSecret"],
            "redirect_uri": config["redirectUri"],
        })
    access_token = str((token_data or {}).get("access_token") or "")
    token_data = None
    if token_status != 200 or not access_token or len(access_token) > 4096:
        return _callback_response("failed")
    identity_status, identity_data = await _provider_json(
        PATREON_IDENTITY_URL, "GET", access_token=access_token)
    identity = (
        patreon.parse_oauth_identity(identity_data, config["campaignId"])
        if identity_status == 200 else None)
    access_token = ""
    if not identity:
        return _callback_response(
            "wrong_campaign" if identity_status == 200 else "failed")
    patreon_user_bi = await blind_index(env, identity["patreon_user_id"])
    existing = await d1_first(
        env,
        "SELECT account_bi FROM patreon_account_links "
        "WHERE patreon_user_bi=?",
        patreon_user_bi)
    if existing and not hmac.compare_digest(
            str(existing.get("account_bi") or ""), account_bi):
        return _callback_response("duplicate_identity")
    expires_at = patreon._normalize_expires_at_iso(identity["next_charge_at"])
    if expires_at:
        expires_at += patreon.SUBSCRIPTION_GRACE_MS
    tier = patreon._tier_from_amount_cents(identity["amount_cents"])
    if identity["amount_cents"] > 0 and expires_at <= int(Date.now()):
        return _callback_response("invalid_membership")
    now = int(Date.now())
    link_data = await encrypt_row(
        env, _link_projection(
            tier, identity["patron_status"], identity["amount_cents"],
            expires_at, now))
    # Provider calls are an untrusted scheduling boundary. Reload the
    # authoritative account and session only after all network I/O, then gate
    # every write on the exact encrypted account snapshot and active session.
    session = await d1_first(
        env,
        "SELECT session_id FROM account_sessions WHERE account_bi=? "
        "AND session_id=? AND revoked_at=0 AND expires_at>?",
        account_bi, session_id, now)
    user_row = await d1_first(
        env, "SELECT data FROM users WHERE user_bi=?", account_bi)
    user_data = str((user_row or {}).get("data") or "")
    rec = await decrypt_row(env, user_data) or {}
    if not session or not rec or rec.get("status") != "active":
        return _callback_response("mismatch")
    guard = {
        "account_bi": account_bi,
        "session_id": session_id,
        "user_data": user_data,
        "now": now,
    }
    link_statement = (
        "INSERT INTO patreon_account_links "
        "(account_bi,patreon_user_bi,data,linked_at,updated_at) "
        "SELECT ?,?,?,?,? WHERE " + _guard_exists_sql() + " "
        "ON CONFLICT(account_bi) DO UPDATE SET "
        "patreon_user_bi=excluded.patreon_user_bi,data=excluded.data,"
        "updated_at=excluded.updated_at",
        (
            account_bi, patreon_user_bi, link_data, now, now,
            *_guard_params(guard),
        ),
    )
    try:
        tier = await _apply_membership(
            env, account_bi, rec, identity["amount_cents"], expires_at,
            event_type="oauth", extra_statements=[link_statement], guard=guard)
    except _StaleAccountState:
        return _callback_response("mismatch")
    return _callback_response("connected")


async def patreon_webhook_handler(env, request):
    """POST /api/integrations/patreon/webhook

    Validates the signature, dedupes the event, and applies the matching role
    grant to the corresponding ForkMesh user.
    """
    _bind_entry_symbols()
    if method_name(request) != "POST":
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            extra_headers={"allow": "POST"})

    secret = str(
        getattr(env, "PATREON_WEBHOOK_SECRET", "") or "").strip()
    if not secret:
        return _no_store(
            {"error": "patreon_webhook_not_configured"}, status=503)

    try:
        raw = await request.text()
    except Exception:
        return json_response({"error": "invalid_payload"}, status=400)
    raw_bytes = raw.encode("utf-8")
    if len(raw_bytes) > patreon.MAX_BODY_BYTES:
        return json_response({"error": "payload_too_large"}, status=413)

    presented = str(
        request.headers.get("x-patreon-signature")
        or request.headers.get("X-Patreon-Signature")
        or "").strip().lower()
    if not patreon.verify_patreon_signature(secret, raw, presented):
        return json_response({"error": "invalid_signature"}, status=401)

    try:
        payload = json.loads(raw)
    except Exception:
        return json_response({"error": "invalid_payload"}, status=400)

    event_type = str(
        request.headers.get("x-patreon-event")
        or request.headers.get("X-Patreon-Event")
        or "").strip().lower()
    event = patreon._extract_patreon_event(payload, event_type)
    if event is None or event["event"] not in patreon.PATREON_EVENT_TYPES:
        return json_response(
            {"ok": True, "ignored": True},
            cache_control="no-store, max-age=0")

    if not event["pledge_id"]:
        return json_response({"error": "missing_pledge_id"}, status=400)

    await ensure_schema(env)

    # The member resource id is stable across updates, so it cannot identify a
    # delivery. Hash the event type and exact signed bytes instead.
    dedupe_key = hashlib.sha256(
        event["event"].encode("utf-8") + b"\n" + raw_bytes).hexdigest()
    now = int(Date.now())
    prior = await d1_first(
        env,
        "SELECT event_id FROM patreon_webhook_events WHERE event_id=?",
        dedupe_key)
    if prior:
        return json_response(
            {"ok": True, "deduped": True},
            cache_control="no-store, max-age=0")

    campaign_id = str(
        getattr(env, "PATREON_CAMPAIGN_ID", "") or "").strip()
    creator_token = str(
        getattr(env, "PATREON_CREATOR_ACCESS_TOKEN", "") or "").strip()
    member_url = _member_api_url(event["pledge_id"])
    if (
        not campaign_id or len(campaign_id) > 128
        or not creator_token or len(creator_token) > 4096
    ):
        return _no_store(
            {"error": "patreon_reconciliation_not_configured"}, status=503)
    if not member_url:
        return json_response({"error": "invalid_member_id"}, status=400)
    # Claim a per-member generation before any provider I/O so concurrent
    # notifications cannot commit an older authoritative snapshot later.
    generation_guard = await _claim_reconciliation_generation(
        env, event["pledge_id"])
    if not generation_guard:
        return json_response(
            {"error": "patreon_reconciliation_failed"}, status=503,
            cache_control="no-store, max-age=0")
    try:
        member_status, member_data = await _provider_json(
            member_url, "GET", access_token=creator_token)
    except Exception:
        member_status, member_data = 0, None
    creator_token = ""
    snapshot = (
        patreon.parse_member_snapshot(
            member_data, event["pledge_id"], campaign_id)
        if member_status == 200 else None)
    member_data = None
    if not snapshot:
        return json_response(
            {"error": "patreon_reconciliation_failed"}, status=503,
            cache_control="no-store, max-age=0")

    deleted = (
        snapshot["amount_cents"] <= 0
        or snapshot["patron_status"] != "active_patron")
    tier = (
        "free" if deleted
        else patreon._tier_from_amount_cents(snapshot["amount_cents"]))
    expires_at = patreon._normalize_expires_at_iso(
        snapshot["next_charge_at"])
    if expires_at:
        expires_at = expires_at + patreon.SUBSCRIPTION_GRACE_MS

    matched = False
    matched_by_link = False
    name_bi = ""
    rec = None
    if snapshot["patreon_user_id"]:
        name_bi, rec = await _find_user_by_patreon_id(
            env, snapshot["patreon_user_id"])
        matched_by_link = bool(name_bi)
    if not name_bi and snapshot["email"]:
        name_bi, rec = await _find_user_by_email(env, snapshot["email"])
    matched = bool(name_bi and rec)
    if not deleted and snapshot["amount_cents"] > 0 and expires_at <= now:
        return json_response(
            {"error": "invalid_membership_expiry"}, status=400,
            cache_control="no-store, max-age=0")
    if matched:
        if not await _generation_is_current(env, generation_guard):
            return json_response(
                {"ok": True, "superseded": True},
                cache_control="no-store, max-age=0")
        event_sql = (
            "INSERT INTO patreon_webhook_events"
            "(event_id,received_at,event_type,account_bi) "
            "SELECT ?,?,?,?"
        )
        event_params = (dedupe_key, now, event["event"], name_bi)
        event_sql, event_params = _append_guards(
            event_sql, event_params, generation_guard=generation_guard,
            where=True)
        extra_statements = [(event_sql, event_params)]
        if matched_by_link:
            link_data = await encrypt_row(
                env, _link_projection(
                    tier, snapshot["patron_status"], snapshot["amount_cents"],
                    0 if deleted else expires_at, now))
            link_sql = (
                "UPDATE patreon_account_links SET data=?,updated_at=? "
                "WHERE account_bi=?"
            )
            link_params = (link_data, now, name_bi)
            link_sql, link_params = _append_guards(
                link_sql, link_params, generation_guard=generation_guard)
            extra_statements.append((link_sql, link_params))
        try:
            tier = await _apply_membership(
                env, name_bi, rec, snapshot["amount_cents"], expires_at,
                deleted=deleted, event_type=event["event"],
                extra_statements=extra_statements,
                generation_guard=generation_guard)
        except _SupersededReconciliation:
            return json_response(
                {"ok": True, "superseded": True},
                cache_control="no-store, max-age=0")
        except Exception:
            raced = await d1_first(
                env,
                "SELECT event_id FROM patreon_webhook_events WHERE event_id=?",
                dedupe_key)
            if raced:
                return json_response(
                    {"ok": True, "deduped": True},
                    cache_control="no-store, max-age=0")
            if not await _generation_is_current(env, generation_guard):
                return json_response(
                    {"ok": True, "superseded": True},
                    cache_control="no-store, max-age=0")
            return json_response(
                {"error": "membership_sync_failed"}, status=503,
                cache_control="no-store, max-age=0")

    # Prune events older than 90 days, matching the mailtrap retention policy.
    cutoff = now - 90 * 24 * 60 * 60 * 1000
    await d1_run(
        env, "DELETE FROM patreon_webhook_events WHERE received_at<?", cutoff)

    return json_response(
        {
            "ok": True,
            "matched": matched,
            "tier": tier,
            "event": event["event"],
        },
        cache_control="no-store, max-age=0")
