"""Request-lazy password-reset abuse controls and first-step handler."""


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


def _password_reset_requester_bucket(requester):
    """Return the unit one edge-observed address spends its reset budget from.

    IPv4 is charged as itself, which is already the shared unit sitting behind
    one NAT. IPv6 is charged to its /64, because that is the smallest block a
    provider routes to a single customer: keyed per address, an attacker on an
    ordinary residential or VPS allocation draws a fresh budget 2**64 times
    over and the requester limit only ever binds on IPv4. The rendered "/64"
    suffix also keeps the two families' keys from colliding.
    """
    if not requester:
        return ""
    try:
        address = ipaddress.ip_address(requester)
        if address.version != 6:
            return requester
        return str(ipaddress.ip_network(
            (address, PASSWORD_RESET_REQUESTER_V6_PREFIX), strict=False))
    except (ValueError, TypeError):
        return ""


async def _password_reset_requester_allowed(env, requester_bi):
    """Atomically consume one anonymous requester's hourly reset budget."""
    if not requester_bi:
        return True
    now = int(Date.now())
    row = await d1_first(
        env,
        "INSERT INTO password_reset_rate"
        " (key_bi,count,window_start_ts,last_sent_ts) VALUES (?,1,?,0)"
        " ON CONFLICT(key_bi) DO UPDATE SET "
        "count=CASE WHEN ?-password_reset_rate.window_start_ts>=? "
        "THEN 1 ELSE MIN(password_reset_rate.count+1,?) END,"
        "window_start_ts=CASE WHEN ?-password_reset_rate.window_start_ts>=? "
        "THEN ? ELSE password_reset_rate.window_start_ts END "
        "RETURNING count,window_start_ts",
        requester_bi, now,
        now, PASSWORD_RESET_REQUESTER_WINDOW_MS,
        PASSWORD_RESET_REQUESTER_WINDOW_LIMIT + 1,
        now, PASSWORD_RESET_REQUESTER_WINDOW_MS, now)
    return int((row or {}).get("count") or 1) <= \
        PASSWORD_RESET_REQUESTER_WINDOW_LIMIT


async def _password_reset_claim_account(env, account_bi):
    """Atomically claim one permitted reset send for an account."""
    if not account_bi:
        return False
    now = int(Date.now())
    inserted = await d1_first(
        env,
        "INSERT OR IGNORE INTO password_reset_rate"
        " (key_bi,count,window_start_ts,last_sent_ts) VALUES (?,1,?,?)"
        " RETURNING key_bi",
        account_bi, now, now)
    if inserted:
        return True
    claimed = await d1_first(
        env,
        "UPDATE password_reset_rate SET "
        "count=CASE WHEN ?-window_start_ts>=? THEN 1 ELSE count+1 END,"
        "window_start_ts=CASE WHEN ?-window_start_ts>=? "
        "THEN ? ELSE window_start_ts END,last_sent_ts=? "
        "WHERE key_bi=? AND ?-last_sent_ts>=? AND "
        "(?-window_start_ts>=? OR count<?) RETURNING key_bi",
        now, PASSWORD_RESET_ACCOUNT_WINDOW_MS,
        now, PASSWORD_RESET_ACCOUNT_WINDOW_MS, now, now,
        account_bi, now, PASSWORD_RESET_ACCOUNT_COOLDOWN_MS,
        now, PASSWORD_RESET_ACCOUNT_WINDOW_MS,
        PASSWORD_RESET_ACCOUNT_WINDOW_LIMIT)
    return bool(claimed)


async def _account_forgot_password(env, request):
    """Send an opaque, rate-limited password-reset response."""
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    # Turnstile first (turnstile.py): its refusal says nothing about whether
    # the identifier exists, so the opaque {"ok": true} contract holds.
    refused = await _turnstile_gate(
        env, request, data, _turnstile.ACTION_PASSWORD_RESET,
        "/forgot-password")
    if refused is not None:
        return refused
    identifier = clean_string(
        data.get("identifier", "") or data.get("email", "") or
        data.get("nodeName", ""), 254).strip().lower()
    requester = _password_reset_requester_bucket(
        _account_session_client_ip(request))
    if requester:
        requester_bi = await blind_index(
            env, "password-reset-requester-v1\n" + requester)
        if not await _password_reset_requester_allowed(env, requester_bi):
            return json_response(
                {"ok": True},
                cache_control="no-store, max-age=0, must-revalidate")
    if identifier:
        rec = None
        name_bi = ""
        if "@" in identifier:
            email_bi = await blind_index(env, identifier)
            row = await d1_first(
                env, "SELECT user_bi,data FROM users WHERE email_bi=?",
                email_bi)
            if row:
                name_bi = str(row.get("user_bi") or "")
                rec = await decrypt_row(env, row.get("data"))
        elif valid_node_name(identifier):
            name_bi, rec = await _account_row(env, identifier)
        if (rec and rec.get("status") == "active" and rec.get("email") and
                rec.get("pass_hash")):
            account_rate_bi = await blind_index(
                env, "password-reset-account-v1\n" + str(name_bi or ""))
            if await _password_reset_claim_account(env, account_rate_bi):
                await _send_password_reset_email(
                    env, request, rec.get("name", ""), rec.get("email", ""),
                    rec.get("pass_hash", ""))
    return json_response(
        {"ok": True},
        cache_control="no-store, max-age=0, must-revalidate")


# --- Authenticated password change (issue #648) ------------------------------
# The flow above is the signed-OUT recovery path. Settings had no signed-IN one:
# every "Current password" box on that screen only CONFIRMS some other action
# (rename, payout, notifications, claim, delete), so the only route to a new
# password was to log out and wait for a reset email. A live session alone is
# still not enough here — the current password is verified again, so a borrowed
# tab cannot take the account over.
PASSWORD_CHANGE_MIN_LENGTH = 8


async def _password_change_attempt_key(env, account_bi):
    """Brute-force budget for one account's current-password checks.

    Deliberately its own namespace: a signed-in user fumbling their current
    password must not spend the shared login budget for their address (and so
    lock themselves out of the login form), while a stolen session still gets
    only a bounded number of guesses at the password it cannot read.
    """
    return await blind_index(
        env, "password-change-v1\n" + str(account_bi or ""))


async def _send_password_changed_notice(env, request, name, email):
    """Tell the account's address that its password just changed.

    This is how somebody learns their account was taken over, so the send is
    best effort: the password is already replaced by the time it runs, and a
    mail outage must not surface as a failed change the user then repeats.
    """
    if not email:
        return False
    reset_url = _public_base_url(env, request) + "/forgot-password"
    subject = "Your ForkMesh password was changed"
    text = ("The password for your ForkMesh account \"" + (name or "") +
            "\" was just changed from a signed-in session. Every other "
            "signed-in device was signed out.\n\nIf this wasn't you, reset "
            "your password now:\n" + reset_url)
    safe_name = _html_escape(name or "")
    safe_reset = _html_escape(reset_url)
    intro = ("The password for your ForkMesh account "
             "<strong class=\"fm-strong\" style=\"color:#f5f5f5\">" + safe_name +
             "</strong> was just changed from a signed-in session.")
    body_html = (
        "<p style=\"margin:0 0 22px\">Every other signed-in device was signed "
        "out.</p>")
    footer_html = (
        "<p class=\"fm-muted\" style=\"margin:22px 0 0;color:#8a8a93;"
        "font-size:12px\">If this wasn't you, reset your password now: "
        "<a href=\"" + safe_reset + "\">" + safe_reset + "</a></p>")
    html = _forkmesh_email_card_html(
        "Your password was changed", intro, body_html, footer_html)
    try:
        return await _send_email(
            env, email, subject, text, html, email_kind="password_change")
    except Exception:
        return False


async def _account_change_password(env, request):
    """Replace this session's own account password."""
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    account_bi, rec = await _account_session_record(env, request, data)
    if not account_bi or not rec or rec.get("status") != "active":
        return json_response(
            {"error": "invalid_session"}, status=401,
            cache_control="no-store, max-age=0, must-revalidate")
    if not rec.get("pass_hash"):
        # A key-bound node record has no password to replace; setting a first
        # one belongs to the account flows that index the address (finalize).
        return json_response({"error": "password_not_set"}, status=400)
    current = (data.get("currentPassword", "") or
               data.get("password", "") or "")[:256]
    new_password = (data.get("newPassword", "") or "")[:256]
    if len(new_password) < PASSWORD_CHANGE_MIN_LENGTH:
        return json_response({"error": "password_too_short"}, status=400)

    actor = clean_string(rec.get("name", ""), MAX_NODE_NAME).strip().lower()
    attempt_key = await _password_change_attempt_key(env, account_bi)
    locked_until = await _login_locked_until(env, attempt_key)
    if locked_until:
        # Unlike the login form there is nothing to enumerate here, so a locked
        # budget refuses the guess outright instead of still spending PBKDF2 on
        # it.
        retry_ms = max(1000, locked_until - int(Date.now()))
        return json_response(
            {"error": "too_many_attempts", "retryAfterMs": retry_ms},
            status=429,
            extra_headers={
                "Retry-After": str(max(1, (retry_ms + 999) // 1000))
            },
            cache_control="no-store, max-age=0, must-revalidate")
    if not await verify_password(
            current, rec.get("pass_salt", ""), rec.get("pass_hash", "")):
        await _audit_sensitive_action(
            env, actor, "account.password_change", "account", actor, "denied",
            {"reason": "invalid_credentials"})
        return await _login_failure_response(
            env, [attempt_key], "invalid_credentials")
    if new_password == current:
        return json_response({"error": "password_unchanged"}, status=400)

    salt, phash = await hash_password(new_password)
    rec["pass_salt"] = salt
    rec["pass_hash"] = phash
    rec["password_changed_at"] = int(Date.now())
    await _save_account(env, account_bi, rec)
    await _login_clear(env, attempt_key)
    # Any reset link still in a mailbox is bound to the OLD hash and stops
    # verifying the moment it is replaced, so only sessions need clearing here:
    # every other device is signed out, and the device that made the change
    # keeps its session so the settings screen it was typed on stays usable.
    keep_session_id = _request_account_session_id(request, data)
    if keep_session_id:
        await d1_run(
            env,
            "UPDATE account_sessions SET revoked_at=? "
            "WHERE account_bi=? AND session_id<>? AND revoked_at=0",
            int(Date.now()), account_bi, keep_session_id)
    else:
        await _account_revoke_sessions(env, account_bi)
    await _audit_sensitive_action(
        env, actor, "account.password_change", "account", actor, "success",
        {"currentRevoked": not keep_session_id})
    if rec.get("email"):
        sent = await _send_password_changed_notice(
            env, request, actor, rec.get("email", ""))
        # Stamp the record already in hand rather than calling the shared
        # _record_account_email: that helper re-reads the account, and a D1
        # replica still serving the pre-change row would write the OLD
        # password hash straight back over the one just saved.
        await _save_account(
            env, account_bi,
            _stamp_account_email(rec, "password_change", sent))
    return json_response(
        {"ok": True, "currentRevoked": not keep_session_id},
        cache_control="no-store, max-age=0, must-revalidate")
