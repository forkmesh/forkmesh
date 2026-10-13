"""Request-lazy account directory and referral-attribution implementation.

The public roster, administrator evidence view, and referral decision paths
run only for account-specific requests. Keeping their implementation out of
entry.py avoids compiling this sizeable route domain during Worker version
validation while retaining the same bound runtime primitives.
"""

import contextvars


REFERRAL_ACCOUNT_MATURITY_MS = 7 * 24 * 60 * 60 * 1000
REFERRAL_DEVICE_OBSERVATION_MS = 24 * 60 * 60 * 1000
REFERRAL_DEVICE_RECENT_MS = 48 * 60 * 60 * 1000
REFERRAL_BURST_WINDOW_MS = 60 * 60 * 1000
REFERRAL_BURST_MAX_AUTOMATIC_CREDITS = 14
REFERRAL_BURST_SCAN_LIMIT = 128
REFERRAL_QUALIFICATION_FLAGS = (
    "account_maturity_pending", "desktop_activity_unproven")
REFERRAL_QUARANTINE_REVIEW_MS = 72 * 60 * 60 * 1000
# Linked node accounts erased per administrator delete request. Each one
# is a full namespace delete (dozens of sequential D1 statements), so a
# member with an unusual number of machines finishes over two presses
# rather than timing out halfway through the first.
ACCOUNT_ADMIN_DELETE_NODE_BATCH = 8
# Accounts quarantined per press of "Quarantine all" on the email
# confirmations dialog. Each one is its own D1 batch plus cache
# invalidations, so a large unconfirmed signup wave finishes over a few
# presses rather than timing out halfway through the first.
ACCOUNT_ADMIN_QUARANTINE_PENDING_BATCH = 12
REFERRAL_CLICK_CLAIM_TTL_MS = 7 * 24 * 60 * 60 * 1000
REFERRAL_RISK_REVIEW_SCORE = 60
REFERRAL_RAPID_SIGNUP_WINDOW_MS = 10 * 60 * 1000
REFERRAL_RAPID_SIGNUP_COUNT = 3
REFERRAL_RISK_WEIGHTS = {
    "network_unavailable": 100,
    "user_agent_unavailable": 100,
    "same_network_as_referrer": 45,
    "automated_client": 100,
    "join_chronology_unavailable": 100,
    "referrer_joined_after_referred_user": 100,
    "shared_signup_network": 25,
    "shared_client_fingerprint": 35,
    "shared_user_agent": 5,
    "rapid_referrer_signups": 40,
    "referrer_rewards_frozen": 100,
}


def _bind_runtime(runtime):
    """Supply the initialized entrypoint primitives used by this domain."""
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


def _referral_user_agent(request):
    """Bound a User-Agent for encrypted storage and blind indexing."""
    try:
        raw = str(request.headers.get("user-agent") or "")
    except Exception:
        raw = ""
    return " ".join(raw.split())[:512]


async def _referral_client_fingerprint(env, request):
    """Keyed browser/device cohort signal; never a standalone verdict."""
    if request is None:
        return ""
    try:
        headers = request.headers
        values = [
            _referral_user_agent(request).lower(),
            clean_string(headers.get("accept-language") or "", 128).lower(),
            clean_string(headers.get("sec-ch-ua") or "", 256).lower(),
            clean_string(headers.get("sec-ch-ua-platform") or "", 64).lower(),
            clean_string(headers.get("sec-ch-ua-mobile") or "", 16).lower(),
        ]
    except Exception:
        values = []
    material = "\n".join(" ".join(value.split()) for value in values)
    if not material.strip():
        return ""
    return await blind_index(env, "referral-client-v1:" + material)


def _referral_risk_score(flags):
    return min(1000, sum(
        REFERRAL_RISK_WEIGHTS.get(str(flag or "").lower(), 0)
        for flag in set(flags or ())))


async def _referral_dynamic_risk_flags(env, row, referred_bi):
    """Re-evaluate cohort signals that can appear after the initial signup."""
    flags = []
    signup_ip_bi = str((row or {}).get("signup_ip_bi") or "")
    if signup_ip_bi:
        reused_network = await d1_first(
            env,
            "SELECT 1 AS reused FROM users WHERE ip_bi=? AND user_bi<>? "
            "LIMIT 1",
            signup_ip_bi, referred_bi,
        )
        if reused_network:
            flags.append("shared_signup_network")

    user_agent_bi = str((row or {}).get("user_agent_bi") or "")
    if user_agent_bi:
        reused_agent = await d1_first(
            env,
            "SELECT 1 AS reused FROM referral_attributions "
            "WHERE user_agent_bi=? AND referred_bi<>? LIMIT 1",
            user_agent_bi, referred_bi,
        )
        if reused_agent:
            flags.append("shared_user_agent")

    client_fingerprint_bi = str(
        (row or {}).get("client_fingerprint_bi") or "")
    referrer_bi = str((row or {}).get("referrer_bi") or "")
    if client_fingerprint_bi and referrer_bi:
        reused_client = await d1_first(
            env,
            "SELECT 1 AS reused FROM referral_attributions "
            "WHERE referrer_bi=? AND client_fingerprint_bi=? "
            "AND referred_bi<>? LIMIT 1",
            referrer_bi, client_fingerprint_bi, referred_bi,
        )
        if reused_client:
            flags.append("shared_client_fingerprint")

    try:
        created_at = int((row or {}).get("created_at") or 0)
    except (TypeError, ValueError):
        created_at = 0
    if referrer_bi and created_at:
        rapid = await d1_first(
            env,
            "SELECT COUNT(*) AS n FROM referral_attributions "
            "WHERE referrer_bi=? AND created_at BETWEEN ? AND ?",
            referrer_bi,
            created_at - REFERRAL_RAPID_SIGNUP_WINDOW_MS,
            created_at + REFERRAL_RAPID_SIGNUP_WINDOW_MS,
        )
        if int((rapid or {}).get("n") or 0) > REFERRAL_RAPID_SIGNUP_COUNT:
            flags.append("rapid_referrer_signups")
    return flags


def _referral_click_token_signature(env, payload):
    key = (_require_data_secret(env) + ":referral-click-v1").encode()
    return hmac.new(key, payload.encode(), "sha256").hexdigest()


def _referral_click_token_claims(env, token):
    value = clean_string(token or "", 1024).strip()
    if "." not in value:
        return None
    payload, signature = value.rsplit(".", 1)
    if (not payload or not re.fullmatch(r"[A-Za-z0-9_-]+", payload)
            or not re.fullmatch(r"[0-9a-f]{64}", signature)
            or not hmac.compare_digest(
                signature, _referral_click_token_signature(env, payload))):
        return None
    try:
        padded = payload + "=" * ((4 - len(payload) % 4) % 4)
        claim = json.loads(base64.urlsafe_b64decode(padded).decode())
        expires_at = int(claim.get("expiresAt") or 0)
        issued_at = int(claim.get("issuedAt") or 0)
    except Exception:
        return None
    now = int(Date.now())
    click_id = clean_string(claim.get("clickId", ""), 64)
    referrer_bi = clean_string(claim.get("referrerBi", ""), 128)
    if (not re.fullmatch(r"[A-Za-z0-9:_-]{12,128}", click_id)
            or not referrer_bi or issued_at <= 0 or expires_at <= now
            or issued_at > now + 5000
            or expires_at - issued_at > REFERRAL_CLICK_CLAIM_TTL_MS):
        return None
    return {
        "clickId": click_id, "referrerBi": referrer_bi,
        "issuedAt": issued_at, "expiresAt": expires_at,
    }


async def _issue_referral_click_token(env, referrer_bi):
    now = int(Date.now())
    expires_at = now + REFERRAL_CLICK_CLAIM_TTL_MS
    click_id = _b64url_encode(_random_bytes(18))
    claim = {
        "clickId": click_id, "referrerBi": str(referrer_bi or ""),
        "issuedAt": now, "expiresAt": expires_at,
    }
    payload = base64.urlsafe_b64encode(
        json.dumps(claim, separators=(",", ":"), sort_keys=True).encode()
    ).decode().rstrip("=")
    await d1_run(
        env,
        "INSERT INTO referral_click_claims "
        "(click_id,referrer_bi,created_at,expires_at) VALUES (?,?,?,?)",
        click_id, referrer_bi, now, expires_at,
    )
    await d1_run(
        env,
        "DELETE FROM referral_click_claims WHERE expires_at<?",
        now - REFERRAL_CLICK_CLAIM_TTL_MS,
    )
    return payload + "." + _referral_click_token_signature(env, payload)


async def _referral_click_source(env, raw_ref, token):
    claim = _referral_click_token_claims(env, token)
    if not claim:
        return "", "", ""
    ref_name, ref_bi = await _referral_account(env, raw_ref)
    if not ref_bi or not hmac.compare_digest(
            str(ref_bi), str(claim["referrerBi"])):
        return "", "", ""
    row = await d1_first(
        env,
        "SELECT click_id FROM referral_click_claims WHERE click_id=? "
        "AND referrer_bi=? AND consumed_at=0 AND expires_at>=?",
        claim["clickId"], ref_bi, int(Date.now()),
    )
    if not row:
        return "", "", ""
    return ref_name, ref_bi, claim["clickId"]


async def _consume_referral_click(env, raw_ref, token, referred_bi):
    ref_name, ref_bi, click_id = await _referral_click_source(
        env, raw_ref, token)
    if not click_id:
        return "", "", ""
    claimed = await d1_first(
        env,
        "UPDATE referral_click_claims SET consumed_at=?,referred_bi=? "
        "WHERE click_id=? AND referrer_bi=? AND consumed_at=0 "
        "AND expires_at>=? RETURNING click_id",
        int(Date.now()), referred_bi, click_id, ref_bi, int(Date.now()),
    )
    return (ref_name, ref_bi, click_id) if claimed else ("", "", "")


def _referral_flags(value):
    if isinstance(value, (list, tuple)):
        parsed = value
    else:
        try:
            parsed = json.loads(str(value or "[]"))
        except Exception:
            parsed = []
    if not isinstance(parsed, list):
        return []
    return list(dict.fromkeys(
        clean_string(flag, 64).strip().lower()
        for flag in parsed if clean_string(flag, 64).strip()
    ))[:12]


def _referral_updated_flags(value, add=(), remove=()):
    """Return one bounded, stable risk/qualification flag list."""
    removed = {str(flag or "").strip().lower() for flag in remove}
    flags = [flag for flag in _referral_flags(value) if flag not in removed]
    for flag in add:
        clean = clean_string(flag, 64).strip().lower()
        if clean and clean not in flags:
            flags.append(clean)
    return flags[:12]


async def _referral_qualification_flags(env, referred_bi, referred, now):
    """Why this verified account is not yet an independently active member."""
    try:
        account_created_at = int((referred or {}).get("created_at") or 0)
    except (TypeError, ValueError):
        account_created_at = 0
    flags = []
    if (not account_created_at
            or account_created_at > now - REFERRAL_ACCOUNT_MATURITY_MS):
        flags.append("account_maturity_pending")

    # Password login and an email click are both cheap to automate. A
    # qualifying desktop has additionally proved possession of its Ed25519 key
    # on two server-observed occasions separated by a full day, and must have
    # been seen recently. The same proved key cannot belong to two accounts.
    device = await d1_first(
        env,
        "SELECT 1 AS qualified FROM account_devices "
        "WHERE account_bi=? AND kind='desktop_node' AND enabled=1 "
        "AND revoked_at=0 "
        "AND instr(',' || capabilities || ',',"
        "',contribution_key_proof,')>0 "
        "AND instr(',' || capabilities || ',',',owner_sign,')>0 "
        "AND created_at<=? AND last_seen>=created_at+? AND last_seen>=? "
        "LIMIT 1",
        referred_bi, now - REFERRAL_DEVICE_OBSERVATION_MS,
        REFERRAL_DEVICE_OBSERVATION_MS, now - REFERRAL_DEVICE_RECENT_MS,
    )
    if not device:
        flags.append("desktop_activity_unproven")
    return flags


async def _referral_burst_cohort(
        env, referrer_bi, referred_bi, created_at):
    """A rapid attributed-signup cohort containing this member."""
    rows = await d1_all(
        env,
        "SELECT referred_bi,risk_flags,decision,created_at "
        "FROM referral_attributions WHERE referrer_bi=? "
        "AND created_at>=? AND created_at<=? "
        "ORDER BY created_at,referred_bi LIMIT ?",
        referrer_bi,
        created_at - REFERRAL_BURST_WINDOW_MS,
        created_at + REFERRAL_BURST_WINDOW_MS,
        REFERRAL_BURST_SCAN_LIMIT,
    )
    rows = rows or []
    for start in range(len(rows)):
        window_end = int(rows[start].get("created_at") or 0) \
            + REFERRAL_BURST_WINDOW_MS
        cohort = []
        contains_referred = False
        for candidate in rows[start:]:
            candidate_at = int(candidate.get("created_at") or 0)
            if candidate_at >= window_end:
                break
            cohort.append(candidate)
            if str(candidate.get("referred_bi") or "") == referred_bi:
                contains_referred = True
        if (contains_referred
                and len(cohort) > REFERRAL_BURST_MAX_AUTOMATIC_CREDITS):
            return cohort
    return []


async def _quarantine_referral_burst(env, referrer_bi, referrer_name, cohort):
    """Remove an entire rapid cohort from automatic rank and rewards."""
    if not cohort or not referrer_bi:
        return False
    now = int(Date.now())
    reasons = json.dumps(
        ["referrer_signup_burst", "referrer_rewards_frozen"],
        separators=(",", ":"))
    # Freeze every credit owned by this referrer. Moving all of its referred
    # accounts (not just the latest rows) prevents the first few members of a detected
    # automation run from remaining active merely because they won a race. One
    # D1 batch makes the freeze, evidence copy, revocation, and removal atomic.
    membership = (
        "SELECT user_bi FROM quarantined_users WHERE referrer_bi=? "
        "AND status<>'restored'")
    await _contribution_run_batch(env, [
        (
            "INSERT INTO referral_referrer_controls "
            "(referrer_bi,state,reason_codes,updated_at,review_after) "
            "VALUES (?,'frozen',?,?,?) ON CONFLICT(referrer_bi) DO UPDATE SET "
            "state='frozen',reason_codes=excluded.reason_codes,"
            "updated_at=excluded.updated_at,"
            "review_after=excluded.review_after,reviewed_at=0,reviewed_by=''",
            (referrer_bi, reasons, now,
             now + REFERRAL_QUARANTINE_REVIEW_MS),
        ),
        (
            "UPDATE referral_attributions SET decision='pending',"
            "credited_at=0,risk_score=MAX(risk_score,100),risk_flags=CASE "
            "WHEN NOT json_valid(risk_flags) THEN ? "
            "WHEN instr(risk_flags,'\"referrer_signup_burst\"')>0 AND "
            "instr(risk_flags,'\"referrer_rewards_frozen\"')>0 "
            "THEN risk_flags "
            "WHEN instr(risk_flags,'\"referrer_signup_burst\"')>0 THEN "
            "json_insert(risk_flags,'$[#]','referrer_rewards_frozen') "
            "WHEN instr(risk_flags,'\"referrer_rewards_frozen\"')>0 THEN "
            "json_insert(risk_flags,'$[#]','referrer_signup_burst') "
            "ELSE json_insert(risk_flags,'$[#]','referrer_signup_burst',"
            "'$[#]','referrer_rewards_frozen') END WHERE referrer_bi=? "
            "AND decision IN ('pending','credited')",
            (reasons, referrer_bi),
        ),
        (
            "INSERT INTO quarantined_users "
            "(user_bi,data,email_bi,username,is_admin,ip_bi,enable_outreach,"
            "discord_bi,referrer_bi,reason_codes,status,quarantined_at,"
            "review_after) "
            "SELECT u.user_bi,u.data,u.email_bi,u.username,u.is_admin,u.ip_bi,"
            "u.enable_outreach,u.discord_bi,?,a.risk_flags,"
            "CASE WHEN a.decision='rejected' "
            "THEN 'rejected' ELSE 'pending_verification' END,?,? FROM users u "
            "JOIN referral_attributions a ON a.referred_bi=u.user_bi "
            "WHERE a.referrer_bi=? ON CONFLICT(user_bi) DO UPDATE SET "
            "data=excluded.data,email_bi=excluded.email_bi,"
            "username=excluded.username,is_admin=excluded.is_admin,"
            "ip_bi=excluded.ip_bi,"
            "enable_outreach=excluded.enable_outreach,"
            "discord_bi=excluded.discord_bi,"
            "referrer_bi=excluded.referrer_bi,"
            "reason_codes=excluded.reason_codes,status=excluded.status,"
            "quarantined_at=excluded.quarantined_at,"
            "review_after=excluded.review_after,reviewed_at=0,reviewed_by=''",
            (referrer_bi, now,
             now + REFERRAL_QUARANTINE_REVIEW_MS, referrer_bi),
        ),
        (
            "INSERT INTO quarantined_nodes "
            "(node_bi,user_bi,pubkey,data,name,last_seen,"
            "quarantined_user_bi,status,quarantined_at) "
            "SELECT n.node_bi,n.user_bi,n.pubkey,n.data,n.name,n.last_seen,"
            "CASE WHEN n.user_bi IN (" + membership + ") THEN n.user_bi "
            "ELSE n.node_bi END,'pending_verification',? FROM nodes n "
            "WHERE n.user_bi IN (" + membership + ") OR n.node_bi IN ("
            + membership + ") ON CONFLICT(node_bi) DO UPDATE SET "
            "status='pending_verification',"
            "quarantined_at=excluded.quarantined_at,reviewed_at=0,"
            "reviewed_by=''",
            (referrer_bi, now, referrer_bi, referrer_bi),
        ),
        (
            "DELETE FROM nodes WHERE user_bi IN (" + membership + ") "
            "OR node_bi IN (" + membership + ")",
            (referrer_bi, referrer_bi),
        ),
        (
            "UPDATE account_devices SET enabled=0,revoked_at=? "
            "WHERE account_bi IN (" + membership + ")",
            (now, referrer_bi),
        ),
        (
            "UPDATE account_sessions SET revoked_at=? WHERE account_bi IN ("
            + membership + ")",
            (now, referrer_bi),
        ),
        (
            "DELETE FROM account_presence WHERE name_bi IN ("
            + membership + ")",
            (referrer_bi,),
        ),
        (
            "DELETE FROM users WHERE user_bi IN (" + membership + ")",
            (referrer_bi,),
        ),
        (
            "UPDATE referral_stats SET signups=0,last_ts=? "
            "WHERE referrer_bi=?",
            (now, referrer_bi),
        ),
        (
            "DELETE FROM badge_awards WHERE badge_slug='first_referral' "
            "AND account_bi=? AND granted_by='system'",
            (referrer_bi,),
        ),
    ])
    memos_clear = globals().get("_users_directory_memos_clear")
    if callable(memos_clear):
        memos_clear()
    await _users_directory_cache_forget()
    await edge_cache_delete(REFERRAL_LEADERBOARD_CACHE_KEY)
    await edge_cache_delete(
        ACCOUNT_LOOKUP_CACHE_PREFIX + quote(referrer_name))
    return True


async def _quarantine_invalid_referral(
        env, referred_bi, referred_name, referrer_bi):
    """Move one hard-rejected referred identity out of the active roster."""
    if not referred_bi or not referrer_bi:
        return False
    now = int(Date.now())
    await _contribution_run_batch(env, [
        (
            "INSERT INTO quarantined_users "
            "(user_bi,data,email_bi,username,is_admin,ip_bi,enable_outreach,"
            "discord_bi,referrer_bi,reason_codes,status,quarantined_at,"
            "review_after) "
            "SELECT u.user_bi,u.data,u.email_bi,u.username,u.is_admin,u.ip_bi,"
            "u.enable_outreach,u.discord_bi,a.referrer_bi,a.risk_flags,"
            "'rejected',?,? "
            "FROM users u JOIN referral_attributions a "
            "ON a.referred_bi=u.user_bi WHERE u.user_bi=? "
            "AND a.referrer_bi=? ON CONFLICT(user_bi) DO UPDATE SET "
            "data=excluded.data,email_bi=excluded.email_bi,"
            "username=excluded.username,is_admin=excluded.is_admin,"
            "ip_bi=excluded.ip_bi,"
            "enable_outreach=excluded.enable_outreach,"
            "discord_bi=excluded.discord_bi,"
            "referrer_bi=excluded.referrer_bi,"
            "reason_codes=excluded.reason_codes,status='rejected',"
            "quarantined_at=excluded.quarantined_at,"
            "review_after=excluded.review_after,reviewed_at=0,reviewed_by=''",
            (now, now + REFERRAL_QUARANTINE_REVIEW_MS,
             referred_bi, referrer_bi),
        ),
        (
            "INSERT INTO quarantined_nodes "
            "(node_bi,user_bi,pubkey,data,name,last_seen,"
            "quarantined_user_bi,status,quarantined_at) "
            "SELECT n.node_bi,n.user_bi,n.pubkey,n.data,n.name,n.last_seen,"
            "?,'rejected',? FROM nodes n WHERE n.user_bi=? OR n.node_bi=? "
            "ON CONFLICT(node_bi) DO UPDATE SET status='rejected',"
            "quarantined_at=excluded.quarantined_at,reviewed_at=0,"
            "reviewed_by=''",
            (referred_bi, now, referred_bi, referred_bi),
        ),
        (
            "DELETE FROM nodes WHERE user_bi=? OR node_bi=?",
            (referred_bi, referred_bi),
        ),
        (
            "UPDATE account_devices SET enabled=0,revoked_at=? "
            "WHERE account_bi=?",
            (now, referred_bi),
        ),
        (
            "UPDATE account_sessions SET revoked_at=? WHERE account_bi=?",
            (now, referred_bi),
        ),
        (
            "DELETE FROM account_presence WHERE name_bi=?",
            (referred_bi,),
        ),
        (
            "DELETE FROM users WHERE user_bi=?",
            (referred_bi,),
        ),
    ])
    memos_clear = globals().get("_users_directory_memos_clear")
    if callable(memos_clear):
        memos_clear()
    await _users_directory_cache_forget()
    await edge_cache_delete(
        ACCOUNT_LOOKUP_CACHE_PREFIX + quote(referred_name))
    return True


async def _referral_control_state(env, referrer_bi):
    row = await d1_first(
        env,
        "SELECT state FROM referral_referrer_controls WHERE referrer_bi=?",
        referrer_bi,
    )
    return clean_string((row or {}).get("state", ""), 16).lower()


async def _record_unique_referral_click(env, referrer_bi, request):
    """Audit one source/referrer/day and count it only when it is valid."""
    if request is None:
        return False
    # Unlike signup compatibility paths, referral credit trusts only the
    # edge-set address and never a caller-controlled X-Forwarded-For value.
    raw_ip = clean_string(_account_session_client_ip(request), 64).strip()
    user_agent = _referral_user_agent(request)
    client_category = _generalized_client_category(request)
    flags = []
    if not raw_ip:
        flags.append("network_unavailable")
    if not user_agent:
        flags.append("user_agent_unavailable")
    if client_category == "automated-client":
        flags.append("automated_client")

    ip_bi = await blind_index(env, raw_ip) if raw_ip else ""
    user_agent_bi = (
        await blind_index(env, "referral-user-agent:" + user_agent.lower())
        if user_agent else ""
    )
    if ip_bi:
        referrer = await d1_first(
            env, "SELECT ip_bi FROM users WHERE user_bi=?", referrer_bi)
        if (referrer and str(referrer.get("ip_bi") or "")
                == str(ip_bi)):
            flags.append("same_network_as_referrer")

    now = int(Date.now())
    bucket = now // REFERRAL_CLICK_WINDOW_MS
    visit_bi = await blind_index(
        env,
        "referral-click:%s:%s:%s:%s" % (
            referrer_bi, ip_bi or "unavailable",
            user_agent_bi or "unavailable", bucket),
    )
    rejected = any(flag in flags for flag in (
        "network_unavailable", "user_agent_unavailable",
        "automated_client"))
    claimed = None
    if not rejected:
        claimed = await d1_first(
            env,
            "INSERT INTO referral_click_seen (visit_bi,referrer_bi,seen_at) "
            "VALUES (?,?,?) ON CONFLICT(visit_bi) DO NOTHING "
            "RETURNING visit_bi",
            visit_bi, referrer_bi, now,
        )
        if not claimed:
            flags.append("duplicate_window")
    decision = "rejected" if rejected else (
        "accepted" if claimed else "duplicate")
    protected = await encrypt_row(env, {
        "ip": raw_ip,
        "userAgent": user_agent,
        "country": clean_string(world_request_country(request), 8).upper(),
        "clientCategory": clean_string(client_category, 40),
    })
    await d1_run(
        env,
        "INSERT INTO referral_click_audit "
        "(visit_bi,referrer_bi,ip_bi,user_agent_bi,decision,risk_flags,"
        "data,first_seen_at,last_seen_at,repeat_count) "
        "VALUES (?,?,?,?,?,?,?,?,?,1) "
        "ON CONFLICT(visit_bi) DO UPDATE SET "
        "last_seen_at=excluded.last_seen_at,"
        "repeat_count=referral_click_audit.repeat_count+1",
        visit_bi, referrer_bi, ip_bi, user_agent_bi, decision,
        json.dumps(flags, separators=(",", ":")), protected, now, now,
    )
    if claimed:
        await d1_run(
            env, "DELETE FROM referral_click_seen WHERE seen_at<?",
            now - REFERRAL_CLICK_RETENTION_MS)
        await d1_run(
            env, "DELETE FROM referral_click_audit WHERE last_seen_at<?",
            now - REFERRAL_CLICK_AUDIT_RETENTION_MS)
    if rejected:
        return False
    return "accepted" if claimed else "duplicate"


async def _capture_referral_source(env, new_name, raw_ref, referral_token=""):
    """Store an explicit direct/referrer marker with every new signup.

    Older account records predate per-user attribution, so an absent marker is
    intentionally distinguishable from a known direct signup.
    """
    marker = {"referralCaptured": True, "referrer": ""}
    try:
        ref_name, ref_bi, _click_id = await _referral_click_source(
            env, raw_ref, referral_token)
        new_name = clean_string(new_name, MAX_NODE_NAME).strip().lower()
        if ref_bi and ref_name != new_name:
            marker["referrer"] = ref_name
    except Exception:
        marker["referralCaptured"] = False
    return marker


def _referral_join_chronology_flags(referred, referrer):
    """Reject retroactive credit when the referrer was not yet a member."""
    try:
        referred_at = int((referred or {}).get("created_at") or 0)
        referrer_at = int((referrer or {}).get("created_at") or 0)
    except (TypeError, ValueError):
        referred_at = referrer_at = 0
    if not referred_at or not referrer_at:
        return ["join_chronology_unavailable"]
    if referred_at <= referrer_at:
        return ["referrer_joined_after_referred_user"]
    return []


async def _record_referral_signup(
        env, new_name, raw_ref, request=None, signup_ip="", signup_ip_bi="",
        referral_token=""):
    """Persist an auditable referral decision without crediting it yet."""
    try:
        new_name = clean_string(new_name, MAX_NODE_NAME).strip().lower()
        if not valid_node_name(new_name):
            return
        referred_bi = await blind_index(env, new_name)
        ref_name, ref_bi, click_id = await _consume_referral_click(
            env, raw_ref, referral_token, referred_bi)
        if not ref_bi or ref_name == new_name:
            return
        raw_ip = clean_string(
            signup_ip or (_transient_client_address(request) if request else ""),
            64,
        ).strip()
        ip_bi = signup_ip_bi or (
            await blind_index(env, raw_ip) if raw_ip else "")
        user_agent = _referral_user_agent(request) if request else ""
        user_agent_bi = (
            await blind_index(env, "referral-user-agent:" + user_agent.lower())
            if user_agent else ""
        )
        client_fingerprint_bi = await _referral_client_fingerprint(
            env, request)
        signup_meta = _signup_metadata(request) if request else {
            "country": "", "clientCategory": "", "at": int(Date.now())}
        created_at = int(signup_meta.get("at") or Date.now())
        network_window_bi = (
            await blind_index(
                env,
                "referral-network-window:%s:%s:%s" % (
                    ref_bi, ip_bi,
                    created_at // REFERRAL_NETWORK_WINDOW_MS),
            )
            if ip_bi else ""
        )
        flags = []
        _, referred_account = await _account_row(env, new_name)
        _, referrer_account = await _account_row(env, ref_name)
        flags.extend(_referral_join_chronology_flags(
            referred_account, referrer_account))
        if not ip_bi:
            flags.append("network_unavailable")
        if not user_agent_bi:
            flags.append("user_agent_unavailable")
        referrer_row = await d1_first(
            env, "SELECT ip_bi FROM users WHERE user_bi=?", ref_bi)
        if (ip_bi and referrer_row
                and str(referrer_row.get("ip_bi") or "") == str(ip_bi)):
            flags.append("same_network_as_referrer")
        if signup_meta.get("clientCategory") == "automated-client":
            flags.append("automated_client")
        if user_agent_bi:
            reused_agent = await d1_first(
                env,
                "SELECT 1 AS reused FROM referral_attributions "
                "WHERE user_agent_bi=? AND referred_bi<>? LIMIT 1",
                user_agent_bi, referred_bi,
            )
            if reused_agent:
                # A modern browser UA is deliberately coarse, so this is an
                # investigation signal rather than an automatic rejection.
                flags.append("shared_user_agent")
        if client_fingerprint_bi:
            reused_client = await d1_first(
                env,
                "SELECT 1 AS reused FROM referral_attributions "
                "WHERE referrer_bi=? AND client_fingerprint_bi=? "
                "AND referred_bi<>? LIMIT 1",
                ref_bi, client_fingerprint_bi, referred_bi,
            )
            if reused_client:
                flags.append("shared_client_fingerprint")
        if ip_bi:
            reused_network = await d1_first(
                env,
                "SELECT 1 AS reused FROM users "
                "WHERE ip_bi=? AND user_bi<>? LIMIT 1",
                ip_bi, referred_bi,
            )
            if reused_network:
                flags.append("shared_signup_network")
        recent = await d1_first(
            env,
            "SELECT COUNT(*) AS n FROM referral_attributions "
            "WHERE referrer_bi=? AND created_at>=?",
            ref_bi, created_at - REFERRAL_RAPID_SIGNUP_WINDOW_MS,
        )
        if int((recent or {}).get("n") or 0) >= REFERRAL_RAPID_SIGNUP_COUNT:
            flags.append("rapid_referrer_signups")
        if await _referral_control_state(env, ref_bi) in ("frozen", "blocked"):
            flags.append("referrer_rewards_frozen")
        rejected = any(flag in flags for flag in (
            "network_unavailable", "user_agent_unavailable",
            "automated_client", "join_chronology_unavailable",
            "referrer_joined_after_referred_user"))
        if rejected:
            flags.append("hard_referral_check_failed")
        risk_score = _referral_risk_score(flags)
        if not rejected and risk_score >= REFERRAL_RISK_REVIEW_SCORE:
            flags.append("risk_review_required")
        decision = "rejected" if rejected else "pending"
        protected = await encrypt_row(env, {
            "ip": raw_ip,
            "userAgent": user_agent,
            "country": clean_string(signup_meta.get("country", ""), 8),
            "clientCategory": clean_string(
                signup_meta.get("clientCategory", ""), 40),
        })
        await d1_run(
            env,
            "INSERT INTO referral_attributions "
            "(referred_bi,referred_name,referrer_bi,referrer_name,"
            "signup_ip_bi,user_agent_bi,client_fingerprint_bi,"
            "referral_click_id,network_window_bi,risk_score,decision,"
            "risk_flags,data,created_at,verified_at,credited_at) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,0,0) "
            "ON CONFLICT(referred_bi) DO NOTHING",
            referred_bi, new_name, ref_bi, ref_name, ip_bi, user_agent_bi,
            client_fingerprint_bi, click_id, network_window_bi, risk_score,
            decision,
            json.dumps(flags, separators=(",", ":")), protected, created_at,
        )
        # Count every attributable signup in the velocity rule. Excluding
        # already-rejected rows would let an automated cohort evade review by
        # intentionally tripping an individual risk check on some members.
        cohort = await _referral_burst_cohort(
            env, ref_bi, referred_bi, created_at)
        if cohort or "referrer_rewards_frozen" in flags:
            await _quarantine_referral_burst(
                env, ref_bi, ref_name,
                cohort or [{"referred_bi": referred_bi}])
            return "quarantined"
        if rejected:
            await _quarantine_invalid_referral(
                env, referred_bi, new_name, ref_bi)
            return "quarantined"
        return "pending"
    except Exception:
        # Attribution remains best effort; a telemetry or counter failure must
        # never roll back an otherwise valid account signup.
        return "error"


async def _finalize_referral_signup(env, referred_bi):
    """Credit one independently active referred member, at most once."""
    try:
        row = await d1_first(
            env,
            "SELECT referred_name,referrer_bi,referrer_name,signup_ip_bi,"
            "user_agent_bi,client_fingerprint_bi,risk_score,decision,"
            "risk_flags,created_at,verified_at "
            "FROM referral_attributions "
            "WHERE referred_bi=?",
            referred_bi,
        )
        if not row:
            return "none"
        referred_name = clean_string(
            row.get("referred_name", ""), MAX_NODE_NAME).lower()
        _, referred = await _account_row(env, referred_name)
        if not referred or referred.get("email_verified") is not True:
            return "pending"
        now = int(Date.now())
        if not int(row.get("verified_at") or 0):
            await d1_run(
                env,
                "UPDATE referral_attributions SET verified_at=? "
                "WHERE referred_bi=? AND verified_at=0",
                now, referred_bi,
            )
        if row.get("decision") != "pending":
            return str(row.get("decision") or "none")
        qualification_flags = await _referral_qualification_flags(
            env, referred_bi, referred, now)
        if qualification_flags:
            pending_flags = _referral_updated_flags(
                row.get("risk_flags"), add=qualification_flags,
                remove=REFERRAL_QUALIFICATION_FLAGS)
            await d1_run(
                env,
                "UPDATE referral_attributions SET risk_flags=? "
                "WHERE referred_bi=? AND decision='pending'",
                json.dumps(pending_flags, separators=(",", ":")),
                referred_bi,
            )
            return "pending"
        active_flags = _referral_updated_flags(
            row.get("risk_flags"), remove=REFERRAL_QUALIFICATION_FLAGS)
        active_flags = _referral_updated_flags(
            active_flags,
            add=await _referral_dynamic_risk_flags(env, row, referred_bi),
        )
        try:
            created_at = int(row.get("created_at") or 0)
        except (TypeError, ValueError):
            created_at = 0
        cohort = await _referral_burst_cohort(
            env, str(row.get("referrer_bi") or ""), referred_bi,
            created_at)
        if cohort:
            await _quarantine_referral_burst(
                env, str(row.get("referrer_bi") or ""),
                clean_string(
                    row.get("referrer_name", ""), MAX_NODE_NAME).lower(),
                cohort)
            return "quarantined"
        referrer_bi = str(row.get("referrer_bi") or "")
        control_state = await _referral_control_state(env, referrer_bi)
        risk_score = _referral_risk_score(active_flags)
        await d1_run(
            env,
            "UPDATE referral_attributions SET risk_score=?,risk_flags=? "
            "WHERE referred_bi=? AND decision='pending'",
            risk_score, json.dumps(active_flags, separators=(",", ":")),
            referred_bi,
        )
        if (control_state in ("frozen", "blocked")
                or risk_score >= REFERRAL_RISK_REVIEW_SCORE):
            review_flags = active_flags
            if control_state in ("frozen", "blocked"):
                review_flags = _referral_updated_flags(
                    review_flags, add=("referrer_rewards_frozen",))
            review_flags = _referral_updated_flags(
                review_flags, add=("risk_review_required",))
            await d1_run(
                env,
                "UPDATE referral_attributions SET risk_flags=? "
                "WHERE referred_bi=? AND decision='pending'",
                json.dumps(review_flags, separators=(",", ":")),
                referred_bi,
            )
            return "pending_review"
        ref_name, ref_bi = await _referral_account(
            env, row.get("referrer_name", ""))
        if not ref_bi or ref_bi != row.get("referrer_bi"):
            await d1_run(
                env,
                "UPDATE referral_attributions SET decision='rejected',"
                "risk_flags=? WHERE referred_bi=? AND decision='pending'",
                json.dumps(
                    active_flags
                    + ["referrer_unavailable"], separators=(",", ":")),
                referred_bi,
            )
            return "rejected"
        _, referrer = await _account_row(env, ref_name)
        chronology_flags = _referral_join_chronology_flags(
            referred, referrer)
        if chronology_flags:
            await d1_run(
                env,
                "UPDATE referral_attributions SET decision='rejected',"
                "verified_at=?,credited_at=0,risk_flags=? "
                "WHERE referred_bi=? AND decision='pending'",
                now,
                json.dumps(
                    active_flags + chronology_flags,
                    separators=(",", ":")),
                referred_bi,
            )
            return "rejected"
        claimed = await d1_first(
            env,
            "UPDATE referral_attributions SET decision='credited',"
            "verified_at=?,credited_at=?,risk_flags=? WHERE referred_bi=? "
            "AND decision='pending' RETURNING referred_bi",
            now, now, json.dumps(active_flags, separators=(",", ":")),
            referred_bi,
        )
        if not claimed:
            return "none"
        await _refresh_referral_signup_count(env, ref_name, ref_bi)
        # "Connector" badge: award the referrer the first time one of their
        # verified, accepted referrals converts.
        _clicks, signups, _last_ts = await _referral_counts(env, ref_bi)
        if signups == 1:
            await _award_badge(env, ref_name, ref_bi, "first_referral")
        memos_clear = globals().get("_users_directory_memos_clear")
        if callable(memos_clear):
            memos_clear()
        # Hackathon: a credited referral is the only thing that earns
        # qualified-referral points, so this is the single place they can be
        # written. The gate is this function's own gate; the hackathon never
        # re-decides a referral. Optional at runtime and best effort, like
        # the attribution hooks, so the credit itself can never fail here.
        hackathon = globals().get("_hackathon")
        if hackathon is not None:
            try:
                await hackathon.on_referral_credited(
                    env, ref_bi, ref_name, referred_bi)
            except Exception:
                pass
        await _users_directory_cache_forget()
        await edge_cache_delete(
            ACCOUNT_LOOKUP_CACHE_PREFIX + quote(ref_name))
        return "credited"
    except Exception:
        return "error"


async def _credited_referrers_by_user(env):
    """Public referrer names for verified, automatically accepted signups."""
    # No index covers decision alone, so this is a table scan that returns
    # nothing most days; memoized per isolate and dropped when a referral is
    # credited (_users_directory_memos_clear).
    memo = globals().get("_CREDITED_REFERRERS_MEMO")
    clock = globals().get("Date")
    now = int(clock.now()) if clock is not None else 0
    if (
        isinstance(memo, dict) and memo.get("map") is not None
        and 0 <= now - int(memo.get("ts") or 0)
        < int(globals().get("CREDITED_REFERRERS_MEMO_TTL_MS", 0) or 0)
    ):
        return dict(memo["map"])
    rows = await d1_all(
        env,
        "SELECT referred_bi,referrer_name FROM referral_attributions "
        "WHERE decision='credited' ORDER BY created_at DESC LIMIT ?",
        1000,
    )
    mapping = {
        str(row.get("referred_bi") or ""): clean_string(
            row.get("referrer_name", ""), MAX_NODE_NAME).lower()
        for row in rows or []
        if row.get("referred_bi") and valid_node_name(clean_string(
            row.get("referrer_name", ""), MAX_NODE_NAME).lower())
    }
    if isinstance(memo, dict):
        memo.update({"ts": now, "map": dict(mapping)})
    return mapping


async def _credited_referrals_by_referrer(env, referrer_bi=""):
    """Exact public usernames attached to automatically credited referrals."""
    if referrer_bi:
        rows = await d1_all(
            env,
            "SELECT referrer_bi,referred_name FROM referral_attributions "
            "WHERE decision='credited' AND referrer_bi=? "
            "ORDER BY credited_at DESC,referred_name LIMIT ?",
            referrer_bi, 1000,
        )
    else:
        rows = await d1_all(
            env,
            "SELECT referrer_bi,referred_name FROM referral_attributions "
            "WHERE decision='credited' "
            "ORDER BY credited_at DESC,referred_name LIMIT ?",
            1000,
        )
    out = {}
    for row in rows or []:
        key = str(row.get("referrer_bi") or "")
        name = clean_string(
            row.get("referred_name", ""), MAX_NODE_NAME).strip().lower()
        if not key or not valid_node_name(name):
            continue
        names = out.setdefault(key, [])
        if name not in names:
            names.append(name)
    return out


async def _referral_public_summary(env, referrer_bi):
    """Public totals plus the exact identities retained by modern signups."""
    clicks, signups, last_ts = await _referral_counts(env, referrer_bi)
    referred = await _credited_referrals_by_referrer(env, referrer_bi)
    names = referred.get(str(referrer_bi or ""), [])
    legacy = await d1_first(
        env,
        "SELECT unattributed_signups FROM referral_legacy_totals "
        "WHERE referrer_bi=?",
        referrer_bi,
    )
    return {
        "referralClicks": clicks,
        "creditedReferrals": signups,
        "referralLastAt": last_ts,
        "referredUsers": names,
        "legacyUnattributed": max(
            0, int((legacy or {}).get("unattributed_signups") or 0)),
    }


async def referral_leaderboard(env):
    cached = await edge_cache_match(REFERRAL_LEADERBOARD_CACHE_KEY)
    if cached is not None:
        return cached
    await ensure_schema(env)
    rows = await d1_all(
        env,
        "SELECT s.referrer_bi,s.name,s.clicks,"
        "COALESCE(c.signups,0) AS signups,"
        "COALESCE(l.unattributed_signups,0) AS legacy_unattributed "
        "FROM referral_stats s LEFT JOIN ("
        "SELECT referrer_bi,COUNT(*) AS signups FROM referral_attributions "
        "WHERE decision='credited' GROUP BY referrer_bi) c "
        "ON c.referrer_bi=s.referrer_bi "
        "LEFT JOIN referral_legacy_totals l "
        "ON l.referrer_bi=s.referrer_bi "
        "WHERE s.clicks>0 OR COALESCE(c.signups,0)>0 "
        "ORDER BY signups DESC,s.clicks DESC,s.name LIMIT ?",
        REFERRAL_LEADERBOARD_LIMIT,
    )
    referred = await _credited_referrals_by_referrer(env)
    board = []
    for row in rows or []:
        names = referred.get(str(row.get("referrer_bi") or ""), [])
        signups = int(row.get("signups") or 0)
        board.append({
            "name": clean_string(
                row.get("name", ""), MAX_NODE_NAME) or "user",
            "clicks": int(row.get("clicks") or 0),
            "signups": signups,
            "referredUsers": names,
            "legacyUnattributed": max(
                0, int(row.get("legacy_unattributed") or 0)),
        })
    resp = json_response(
        {"ok": True, "board": board},
        cache_seconds=REFERRAL_LEADERBOARD_TTL,
    )
    await edge_cache_put(REFERRAL_LEADERBOARD_CACHE_KEY, resp)
    return resp


async def _account_users_admin_actor(
        env, request, proof_prefix=None, resource=""):
    """Return the proven administrator for a directory read or user action."""
    if request is None:
        return ""
    try:
        headers = request.headers
        authorization = str(headers.get("authorization") or "")
        params = parse_qs(urlparse(request.url).query)
    except Exception:
        return ""
    actor = ""
    # Only an explicit credential (bearer session or the signed proof below)
    # opens the protected projection. The browser session cookie used to
    # qualify for the directory read (resource=""), which turned every chat
    # and dashboard tab of a signed-in administrator into a five-minute
    # trigger for the four whole-table referral/email scans behind
    # _account_admin_signup_fields; those tabs only ever need the public
    # roster, and the desktop Users page always sends the bearer token or
    # the signed proof.
    if authorization:
        actor = await _authed_account_name(env, request)
    if not actor:
        actor = clean_string(
            params.get("node", [""])[0], MAX_NODE_NAME).strip().lower()
        ts = clean_string(params.get("ts", [""])[0], 20)
        sig = clean_string(params.get("sig", [""])[0], 200)
        canonical = (
            (proof_prefix or ACCOUNT_USERS_ADMIN_PROOF) + "\n" + actor +
            "\n" + ((resource + "\n") if resource else "") + ts
        ).encode()
        if (not actor or not sig or not _ts_ok(ts)
                or not await _verify_owner_signature(
                    env, actor, sig, canonical)):
            actor = ""
    return actor if actor and await _is_admin(env, actor) else ""


async def _quarantine_user_account(env, actor, target, action):
    """Atomically remove one non-admin user from every active account surface.

    Returns "" once the account is quarantined, otherwise the error code that
    stopped it. ``action`` names the audit entry so the single-user button and
    the pending-queue sweep stay distinguishable in the log.
    """
    target_bi = await blind_index(env, target)
    row = await d1_first(
        env,
        "SELECT data,is_admin FROM users WHERE user_bi=? LIMIT 1",
        target_bi,
    )
    rec = await decrypt_row(env, (row or {}).get("data", "")) if row else None
    if (not rec or rec.get("status") != "active"
            or _account_kind(rec) != "user"):
        return "no_such_user"
    if bool(int((row or {}).get("is_admin") or 0)):
        await _audit_sensitive_action(
            env, actor, action, "account", target,
            "denied", {"reason": "target_is_admin"})
        return "target_is_admin"

    now = int(Date.now())
    reasons = json.dumps(
        ["manual_admin_quarantine"], separators=(",", ":"))
    linked_nodes = await d1_all(
        env,
        "SELECT name FROM nodes WHERE user_bi=? OR node_bi=?",
        target_bi, target_bi,
    )
    try:
        await _contribution_run_batch(env, [
            (
                "INSERT INTO quarantined_users "
                "(user_bi,data,email_bi,username,is_admin,ip_bi,"
                "enable_outreach,discord_bi,referrer_bi,reason_codes,status,"
                "quarantined_at,review_after) SELECT u.user_bi,u.data,"
                "u.email_bi,u.username,u.is_admin,u.ip_bi,u.enable_outreach,"
                "u.discord_bi,"
                "COALESCE((SELECT referrer_bi FROM referral_attributions "
                "WHERE referred_bi=u.user_bi LIMIT 1),''),?,"
                "'pending_verification',?,? FROM users u WHERE u.user_bi=? "
                "ON CONFLICT(user_bi) DO UPDATE SET data=excluded.data,"
                "email_bi=excluded.email_bi,username=excluded.username,"
                "is_admin=excluded.is_admin,ip_bi=excluded.ip_bi,"
                "enable_outreach=excluded.enable_outreach,"
                "discord_bi=excluded.discord_bi,"
                "referrer_bi=excluded.referrer_bi,"
                "reason_codes=excluded.reason_codes,"
                "status='pending_verification',"
                "quarantined_at=excluded.quarantined_at,"
                "review_after=excluded.review_after,reviewed_at=0,"
                "reviewed_by=''",
                (reasons, now, now, target_bi),
            ),
            (
                "INSERT INTO quarantined_nodes "
                "(node_bi,user_bi,pubkey,data,name,last_seen,"
                "quarantined_user_bi,status,quarantined_at) "
                "SELECT n.node_bi,n.user_bi,n.pubkey,n.data,n.name,"
                "n.last_seen,?,'pending_verification',? FROM nodes n "
                "WHERE n.user_bi=? OR n.node_bi=? "
                "ON CONFLICT(node_bi) DO UPDATE SET "
                "status='pending_verification',"
                "quarantined_at=excluded.quarantined_at,reviewed_at=0,"
                "reviewed_by=''",
                (target_bi, now, target_bi, target_bi),
            ),
            (
                "UPDATE referral_attributions SET decision='rejected',"
                "credited_at=0,risk_score=MAX(risk_score,100),"
                "risk_flags=CASE WHEN instr(risk_flags,"
                "'\"manual_admin_quarantine\"')>0 THEN risk_flags "
                "WHEN json_valid(risk_flags) THEN json_insert("
                "risk_flags,'$[#]','manual_admin_quarantine') ELSE ? END "
                "WHERE referred_bi=?",
                (reasons, target_bi),
            ),
            (
                "DELETE FROM nodes WHERE user_bi=? OR node_bi=?",
                (target_bi, target_bi),
            ),
            (
                "UPDATE account_devices SET enabled=0,revoked_at=? "
                "WHERE account_bi=?",
                (now, target_bi),
            ),
            (
                "UPDATE account_sessions SET revoked_at=? WHERE account_bi=?",
                (now, target_bi),
            ),
            (
                "DELETE FROM account_presence WHERE name_bi=?",
                (target_bi,),
            ),
            # The encrypted quarantine row above already carries the address,
            # so the confirmation queue keeps no separate copy of an account
            # that is no longer waiting on anything.
            (
                "DELETE FROM pending_verifications WHERE name_bi=?",
                (target_bi,),
            ),
            ("DELETE FROM users WHERE user_bi=?", (target_bi,)),
        ])
    except Exception:
        await _audit_sensitive_action(
            env, actor, action, "account", target, "failed", {})
        raise

    memos_clear = globals().get("_users_directory_memos_clear")
    if callable(memos_clear):
        memos_clear()
    await _users_directory_cache_forget()
    await edge_cache_delete(REFERRAL_LEADERBOARD_CACHE_KEY)
    await edge_cache_delete(ACCOUNT_LOOKUP_CACHE_PREFIX + quote(target))
    for node in linked_nodes or []:
        node_name = clean_string(
            node.get("name", ""), MAX_NODE_NAME).strip().lower()
        if valid_node_name(node_name):
            await edge_cache_delete(
                ACCOUNT_LOOKUP_CACHE_PREFIX + quote(node_name))
    if isinstance(_MEMBER_COUNT_MEMO, dict):
        _MEMBER_COUNT_MEMO.update(
            {"count": None, "ts": 0, "refresh_ts": 0})
    await _audit_sensitive_action(
        env, actor, action, "account", target, "success", {})
    return ""


async def _account_admin_quarantine_user(env, request):
    """Quarantine the one user an administrator selected on the Users page."""
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    target = clean_string(
        data.get("target", ""), MAX_NODE_NAME).strip().lower()
    if not valid_node_name(target):
        return json_response({"error": "invalid_user"}, status=400)
    actor = await _account_users_admin_actor(
        env, request, ACCOUNT_USER_QUARANTINE_PROOF, target)
    if not actor:
        return json_response({"error": "unauthorized"}, status=401)
    if target == actor:
        await _audit_sensitive_action(
            env, actor, "admin.user_quarantine", "account", target,
            "denied", {"reason": "self_quarantine_forbidden"})
        return json_response(
            {"error": "self_quarantine_forbidden"}, status=409)

    failure = await _quarantine_user_account(
        env, actor, target, "admin.user_quarantine")
    if failure == "no_such_user":
        return json_response({"error": failure}, status=404)
    if failure:
        return json_response({"error": failure}, status=409)
    return json_response({"ok": True, "target": target})


async def _account_admin_quarantine_pending(env, request):
    """Quarantine every user still sitting in the email-confirmation queue.

    A signup wave that never confirms is the shape abuse takes here, so the
    administrator dialog offers one press instead of one per row. The work is
    capped per request the way the delete sweep is -- each account is its own
    D1 batch -- and the reply reports what is left so the caller can finish.
    """
    # Naming a resource is what keeps a bare session cookie out of this
    # path: the sweep needs the same signed proof or bearer token the
    # single-user button needs, so no browser can be walked into it.
    actor = await _account_users_admin_actor(
        env, request, ACCOUNT_PENDING_QUARANTINE_PROOF, "pending")
    if not actor:
        return json_response({"error": "unauthorized"}, status=401)
    # Do not let protected rows at the head of the queue starve every account
    # behind them.  Select only non-admin candidates, and fetch no more than a
    # single bounded batch; account rows are the authority for the current
    # name and verification state, not a possibly stale queue payload.
    actor_bi = await blind_index(env, actor)
    rows = await d1_all(
        env,
        "SELECT p.name_bi FROM pending_verifications p "
        "LEFT JOIN users u ON u.user_bi=p.name_bi "
        "WHERE p.name_bi<>? AND COALESCE(u.is_admin,0)=0 "
        "ORDER BY p.name_bi LIMIT ?",
        actor_bi, ACCOUNT_ADMIN_QUARANTINE_PENDING_BATCH,
    )
    quarantined = []
    cleared = 0
    for row in rows or []:
        target_bi = str(row.get("name_bi") or "")
        account_row = await d1_first(
            env, "SELECT data FROM users WHERE user_bi=? LIMIT 1", target_bi)
        rec = await decrypt_row(env, (account_row or {}).get("data", "")) or {}
        target = clean_string(
            rec.get("name", ""), MAX_NODE_NAME).strip().lower()
        # An address that is already verified (or a queue row whose identity
        # was deleted, renamed inconsistently, or otherwise became invalid)
        # must not turn a bulk cleanup into an unexpected quarantine.  Delete
        # only the stale queue entry and leave the active account untouched.
        if (
                not target_bi or not rec
                or rec.get("status") != "active"
                or _account_kind(rec) != "user"
                or rec.get("email_verified")
                or not clean_string(rec.get("email", ""), 254).strip()
                or not valid_node_name(target)
                or await blind_index(env, target) != target_bi):
            await d1_run(
                env, "DELETE FROM pending_verifications WHERE name_bi=?",
                target_bi)
            cleared += 1
            continue
        failure = await _quarantine_user_account(
            env, actor, target, "admin.pending_quarantine")
        if not failure:
            quarantined.append(target)
            continue
        if failure == "no_such_user":
            # A concurrent account removal raced this request. Its queue row
            # is stale for the same reason as the rows handled above.
            await d1_run(
                env, "DELETE FROM pending_verifications WHERE name_bi=?",
                target_bi)
            cleared += 1

    # Calculate after the mutations so the client can continue through a
    # large wave without repeatedly selecting protected admins.  ``held`` is
    # intentionally separate from ``remaining``: admins stay visible for a
    # human decision but never make the one-button cleanup loop forever.
    held_row = await d1_first(
        env,
        "SELECT COUNT(*) AS n FROM pending_verifications p "
        "LEFT JOIN users u ON u.user_bi=p.name_bi "
        "WHERE p.name_bi=? OR COALESCE(u.is_admin,0)<>0",
        actor_bi,
    )
    remaining_row = await d1_first(
        env,
        "SELECT COUNT(*) AS n FROM pending_verifications p "
        "LEFT JOIN users u ON u.user_bi=p.name_bi "
        "WHERE p.name_bi<>? AND COALESCE(u.is_admin,0)=0",
        actor_bi,
    )
    try:
        held = max(0, int((held_row or {}).get("n") or 0))
    except (TypeError, ValueError):
        held = 0
    try:
        remaining = max(0, int((remaining_row or {}).get("n") or 0))
    except (TypeError, ValueError):
        remaining = 0
    await _audit_sensitive_action(
        env, actor, "admin.pending_quarantine_sweep", "account", "",
        "success", {"quarantined": len(quarantined), "cleared": cleared,
                    "held": held, "remaining": remaining})
    return json_response(
        {"ok": True, "quarantined": quarantined, "cleared": cleared,
         "held": held, "remaining": remaining},
        cache_control="no-store, max-age=0, must-revalidate")


async def _account_admin_unquarantine_user(env, request):
    """Restore one preserved user and its nodes to the active directory.

    Quarantine revokes every session and desktop device. Restoration does not
    revive either credential class: the member signs in again and explicitly
    re-enables any desktop they still trust. Referral credit also remains a
    separate review decision, so restoring access cannot award a quarantined
    referral as a side effect.
    """
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    target = clean_string(
        data.get("target", ""), MAX_NODE_NAME).strip().lower()
    if not valid_node_name(target):
        return json_response({"error": "invalid_user"}, status=400)
    actor = await _account_users_admin_actor(
        env, request, ACCOUNT_USER_UNQUARANTINE_PROOF, target)
    if not actor:
        return json_response({"error": "unauthorized"}, status=401)

    target_bi = await blind_index(env, target)
    row = await d1_first(
        env,
        "SELECT data,status FROM quarantined_users "
        "WHERE user_bi=? LIMIT 1",
        target_bi,
    )
    if not row:
        return json_response({"error": "no_such_quarantined_user"},
                             status=404)
    status = clean_string(row.get("status", ""), 32)
    if status == "restored":
        return json_response({"error": "already_restored"}, status=409)
    rec = await decrypt_row(env, row.get("data", "")) or {}
    if (rec.get("status") != "active"
            or _account_kind(rec) != "user"
            or clean_string(rec.get("name", ""), MAX_NODE_NAME).lower()
            != target):
        await _audit_sensitive_action(
            env, actor, "admin.user_unquarantine", "account", target,
            "denied", {"reason": "invalid_quarantine_record"})
        return json_response(
            {"error": "invalid_quarantine_record"}, status=409)

    if await d1_first(
            env, "SELECT 1 AS active FROM users WHERE user_bi=? LIMIT 1",
            target_bi):
        return json_response({"error": "user_already_active"}, status=409)

    account_conflict = await d1_first(
        env,
        "SELECT u.username FROM users u JOIN quarantined_users q ON "
        "q.user_bi=? WHERE u.user_bi<>q.user_bi AND "
        "((q.email_bi IS NOT NULL AND q.email_bi<>'' "
        "AND u.email_bi=q.email_bi) OR "
        "(q.discord_bi IS NOT NULL AND q.discord_bi<>'' "
        "AND u.discord_bi=q.discord_bi)) LIMIT 1",
        target_bi,
    )
    if account_conflict:
        conflict_name = clean_string(
            account_conflict.get("username", ""), MAX_NODE_NAME,
        ).strip().lower()
        await _audit_sensitive_action(
            env, actor, "admin.user_unquarantine", "account", target,
            "denied", {"reason": "restore_conflict",
                       "account": conflict_name})
        return json_response(
            {"error": "restore_conflict", "account": conflict_name},
            status=409)

    linked_nodes = await d1_all(
        env,
        "SELECT name,node_bi FROM quarantined_nodes "
        "WHERE quarantined_user_bi=? AND status<>'restored' "
        "ORDER BY name COLLATE NOCASE",
        target_bi,
    )
    node_conflict = await d1_first(
        env,
        "SELECT q.name FROM quarantined_nodes q JOIN nodes n "
        "ON n.node_bi=q.node_bi WHERE q.quarantined_user_bi=? "
        "AND q.status<>'restored' LIMIT 1",
        target_bi,
    )
    if node_conflict:
        conflict_name = clean_string(
            node_conflict.get("name", ""), MAX_NODE_NAME).strip().lower()
        await _audit_sensitive_action(
            env, actor, "admin.user_unquarantine", "account", target,
            "denied", {"reason": "restore_conflict", "node": conflict_name})
        return json_response(
            {"error": "restore_conflict", "node": conflict_name}, status=409)

    now = int(Date.now())
    try:
        await _contribution_run_batch(env, [
            (
                "INSERT INTO users "
                "(user_bi,data,email_bi,username,is_admin,ip_bi,"
                "enable_outreach,discord_bi) SELECT user_bi,data,email_bi,"
                "username,is_admin,ip_bi,enable_outreach,discord_bi "
                "FROM quarantined_users WHERE user_bi=? "
                "AND status IN ('pending_verification','rejected')",
                (target_bi,),
            ),
            (
                "INSERT INTO nodes (node_bi,user_bi,pubkey,data,name,last_seen) "
                "SELECT node_bi,user_bi,pubkey,data,name,last_seen FROM "
                "quarantined_nodes WHERE quarantined_user_bi=? "
                "AND status<>'restored'",
                (target_bi,),
            ),
            (
                "UPDATE quarantined_users SET status='restored',"
                "reviewed_at=?,reviewed_by=? WHERE user_bi=? "
                "AND status IN ('pending_verification','rejected')",
                (now, actor, target_bi),
            ),
            (
                "UPDATE quarantined_nodes SET status='restored',"
                "reviewed_at=?,reviewed_by=? WHERE quarantined_user_bi=? "
                "AND status<>'restored'",
                (now, actor, target_bi),
            ),
        ])
    except Exception:
        await _audit_sensitive_action(
            env, actor, "admin.user_unquarantine", "account", target,
            "failed", {})
        raise

    memos_clear = globals().get("_users_directory_memos_clear")
    if callable(memos_clear):
        memos_clear()
    await _users_directory_cache_forget()
    await edge_cache_delete(REFERRAL_LEADERBOARD_CACHE_KEY)
    await edge_cache_delete(ACCOUNT_LOOKUP_CACHE_PREFIX + quote(target))
    restored_nodes = []
    for node in linked_nodes or []:
        node_name = clean_string(
            node.get("name", ""), MAX_NODE_NAME).strip().lower()
        if valid_node_name(node_name):
            restored_nodes.append(node_name)
            await edge_cache_delete(
                ACCOUNT_LOOKUP_CACHE_PREFIX + quote(node_name))
    if isinstance(_MEMBER_COUNT_MEMO, dict):
        _MEMBER_COUNT_MEMO.update(
            {"count": None, "ts": 0, "refresh_ts": 0})
    await _audit_sensitive_action(
        env, actor, "admin.user_unquarantine", "account", target,
        "success", {"nodesRestored": len(restored_nodes)})
    return json_response(
        {"ok": True, "target": target, "nodesRestored": restored_nodes,
         "reviewedAt": now, "reviewedBy": actor},
        cache_control="no-store, max-age=0, must-revalidate")


async def _account_admin_delete_user(env, request):
    """Irreversibly erase one non-admin user and everything it owns.

    Quarantine keeps the account as reviewable evidence; this is its
    counterpart for records with nothing worth keeping. Deletion reuses
    ``_delete_account_namespace`` -- the same routine a member's own
    "delete my account" runs -- so repos, sessions, devices, SSH keys,
    referrals, follows, chat memberships, and World rows all go with the
    identity instead of being merely hidden.
    """
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    target = clean_string(
        data.get("target", ""), MAX_NODE_NAME).strip().lower()
    if not valid_node_name(target):
        return json_response({"error": "invalid_user"}, status=400)
    actor = await _account_users_admin_actor(
        env, request, ACCOUNT_USER_DELETE_PROOF, target)
    if not actor:
        return json_response({"error": "unauthorized"}, status=401)
    if target == actor:
        await _audit_sensitive_action(
            env, actor, "admin.user_delete", "account", target,
            "denied", {"reason": "self_delete_forbidden"})
        return json_response({"error": "self_delete_forbidden"}, status=409)

    target_bi = await blind_index(env, target)
    row = await d1_first(
        env,
        "SELECT data,is_admin FROM users WHERE user_bi=? LIMIT 1",
        target_bi,
    )
    rec = await decrypt_row(env, (row or {}).get("data", "")) if row else None
    if (not rec or rec.get("status") != "active"
            or _account_kind(rec) != "user"):
        return json_response({"error": "no_such_user"}, status=404)
    if bool(int((row or {}).get("is_admin") or 0)):
        await _audit_sensitive_action(
            env, actor, "admin.user_delete", "account", target,
            "denied", {"reason": "target_is_admin"})
        return json_response({"error": "target_is_admin"}, status=409)

    # Repos belong to a member's NODE accounts, not to the user account they
    # log in with, so deleting the user row alone would orphan them rather
    # than remove them. Take the linked nodes first, a bounded batch per
    # request: one namespace delete is dozens of sequential D1 statements and
    # the request still has to finish. While nodes remain the user row is
    # left in place, so pressing Delete again resumes from here.
    linked = await d1_all(
        env,
        "SELECT node_bi,name,data FROM nodes WHERE user_bi=? AND node_bi<>?",
        target_bi, target_bi,
    )
    pending = []
    deferred = 0
    for node in linked or []:
        if len(pending) >= ACCOUNT_ADMIN_DELETE_NODE_BATCH:
            # Past the batch, do not even resolve the record: the point of the
            # cap is to bound this request's D1 work, reads included.
            deferred += 1
            continue
        node_bi = str(node.get("node_bi") or "")
        node_rec = await _account_identity_rec_by_bi(env, node_bi)
        if not node_rec:
            node_rec = await decrypt_row(env, node.get("data", "")) or {}
        # A linked row that is itself a human account belongs to someone who
        # can still log in. Leave it alone rather than deleting a second
        # person; the sweep below spares its identity row too.
        if not node_bi or _account_kind(node_rec) == "user":
            continue
        node_name = clean_string(
            node_rec.get("name", "") or node.get("name", ""),
            MAX_NODE_NAME).strip().lower()
        if not valid_node_name(node_name):
            continue
        node_rec = dict(node_rec)
        node_rec["name"] = node_name
        pending.append((node_bi, node_rec))

    deleted_nodes = []
    try:
        for node_bi, node_rec in pending:
            await _delete_account_namespace(env, node_bi, node_rec)
            deleted_nodes.append(node_rec.get("name", ""))
    except Exception:
        await _audit_sensitive_action(
            env, actor, "admin.user_delete", "account", target,
            "failed", {"nodesDeleted": len(deleted_nodes)})
        raise
    # Only rows this pass never looked at hold the account open. Rows it
    # examined and skipped are permanent, so counting them would leave the
    # deletion impossible to finish.
    remaining = deferred
    if remaining:
        await _audit_sensitive_action(
            env, actor, "admin.user_delete", "account", target,
            "requested", {"nodesDeleted": len(deleted_nodes),
                          "nodesRemaining": remaining})
        return json_response(
            {"ok": True, "target": target, "accountDeleted": False,
             "nodesDeleted": deleted_nodes, "nodesRemaining": remaining},
            cache_control="no-store, max-age=0, must-revalidate")

    try:
        await _delete_account_namespace(env, target_bi, rec)
        # Registry rows for nodes that never had an account of their own are
        # the last thing keyed to this member. The NOT IN guard is what keeps
        # a linked row that belongs to a second human -- someone who can still
        # log in -- from losing its identity mirror as collateral.
        await d1_run(
            env,
            "DELETE FROM nodes WHERE user_bi=? AND node_bi NOT IN "
            "(SELECT user_bi FROM users)",
            target_bi)
    except Exception:
        await _audit_sensitive_action(
            env, actor, "admin.user_delete", "account", target,
            "failed", {"nodesDeleted": len(deleted_nodes)})
        raise

    await edge_cache_delete(REFERRAL_LEADERBOARD_CACHE_KEY)
    for node_name in deleted_nodes:
        await edge_cache_delete(ACCOUNT_LOOKUP_CACHE_PREFIX + quote(node_name))
    if isinstance(_MEMBER_COUNT_MEMO, dict):
        _MEMBER_COUNT_MEMO.update(
            {"count": None, "ts": 0, "refresh_ts": 0})
    await _audit_sensitive_action(
        env, actor, "admin.user_delete", "account", target,
        "success", {"nodesDeleted": len(deleted_nodes)})
    return json_response(
        {"ok": True, "target": target, "accountDeleted": True,
         "nodesDeleted": deleted_nodes, "nodesRemaining": 0},
        cache_control="no-store, max-age=0, must-revalidate")


# Per-isolate copy of the admin signup/referral evidence. Its four
# whole-table aggregates were the third-heaviest D1 read of 2026-09-11 even
# though the Users page only changes when someone signs up. Looked up through
# globals() so the AST test harnesses keep the uncached build.
_ADMIN_SIGNUP_FIELDS_MEMO = {"ts": 0, "fields": None}
ADMIN_SIGNUP_FIELDS_MEMO_TTL_MS = 60 * 1000


async def _account_admin_signup_fields(env):
    """Protected referral evidence for the authenticated desktop Users page."""
    memo = globals().get("_ADMIN_SIGNUP_FIELDS_MEMO")
    now = None
    if isinstance(memo, dict):
        now = int(Date.now())
        if (
            isinstance(memo.get("fields"), dict)
            and 0 <= now - int(memo.get("ts") or 0)
            < ADMIN_SIGNUP_FIELDS_MEMO_TTL_MS
        ):
            return {
                key: dict(value) for key, value in memo["fields"].items()}
    referred = await _credited_referrals_by_referrer(env)
    duplicate_ip_rows = await d1_all(
        env,
        "SELECT ip_bi FROM users WHERE ip_bi IS NOT NULL AND ip_bi<>'' "
        "GROUP BY ip_bi HAVING COUNT(*)>1",
    )
    duplicate_ip_bis = {
        str(row.get("ip_bi") or "") for row in duplicate_ip_rows or []
        if row.get("ip_bi")
    }
    click_rows = await d1_all(
        env,
        "SELECT referrer_bi,"
        "SUM(CASE WHEN decision='accepted' THEN 1 ELSE 0 END) "
        "AS unique_clicks,"
        "SUM(CASE WHEN decision='accepted' THEN repeat_count-1 "
        "WHEN decision='duplicate' THEN repeat_count ELSE 0 END) "
        "AS duplicate_clicks,"
        "SUM(CASE WHEN decision='rejected' THEN repeat_count ELSE 0 END) "
        "AS rejected_clicks "
        "FROM referral_click_audit GROUP BY referrer_bi",
    )
    click_audit = {
        str(row.get("referrer_bi") or ""): row for row in click_rows or []
    }
    try:
        email_rows = await d1_all(
            env,
            "SELECT account_bi,MAX(opened_at) AS last_opened_at,"
            "MAX(clicked_at) AS last_clicked_at,SUM(open_count) AS opens,"
            "SUM(click_count) AS clicks FROM mailtrap_email_sends "
            "GROUP BY account_bi",
        )
    except Exception:
        # A rolling deploy may briefly serve the new Worker before the native
        # migration reaches an old database. Referral evidence remains useful;
        # lifecycle counts appear on the next refresh after schema readiness.
        email_rows = []
    email_activity = {
        str(row.get("account_bi") or ""): row for row in email_rows or []
    }
    rows = await d1_all(
        env,
        "SELECT u.user_bi,u.ip_bi,r.referrer_name,r.decision,r.risk_flags,"
        "r.risk_score,r.signup_ip_bi,r.user_agent_bi,"
        "r.client_fingerprint_bi,r.referral_click_id,"
        "r.network_window_bi,r.data,"
        "r.created_at,r.verified_at,r.credited_at,s.clicks AS referral_clicks,"
        "COALESCE(c.signups,0) AS credited_referrals,"
        "s.last_ts AS referral_last_at,"
        "COALESCE(l.unattributed_signups,0) AS legacy_unattributed "
        "FROM users u LEFT JOIN referral_attributions r "
        "ON r.referred_bi=u.user_bi "
        "LEFT JOIN referral_stats s ON s.referrer_bi=u.user_bi "
        "LEFT JOIN (SELECT referrer_bi,COUNT(*) AS signups "
        "FROM referral_attributions WHERE decision='credited' "
        "GROUP BY referrer_bi) c ON c.referrer_bi=u.user_bi "
        "LEFT JOIN referral_legacy_totals l ON l.referrer_bi=u.user_bi "
        "ORDER BY u.username COLLATE NOCASE LIMIT ?",
        1000,
    )
    out = {}
    for row in rows or []:
        protected = (
            await decrypt_row(env, row.get("data", ""))
            if row.get("data") else {}) or {}
        user_bi = str(row.get("user_bi") or "")
        referred_users = referred.get(user_bi, [])
        credited_referrals = int(row.get("credited_referrals") or 0)
        audited = click_audit.get(user_bi, {})
        email_events = email_activity.get(user_bi, {})
        unique_clicks = int(audited.get("unique_clicks") or 0)
        referral_clicks = int(row.get("referral_clicks") or 0)
        risk_flags = _referral_flags(row.get("risk_flags"))
        effective_ip_bi = str(
            row.get("signup_ip_bi") or row.get("ip_bi") or "")
        if (effective_ip_bi in duplicate_ip_bis
                and "shared_signup_network" not in risk_flags):
            risk_flags.append("shared_signup_network")
        displayed_risk_score = max(
            int(row.get("risk_score") or 0),
            _referral_risk_score(risk_flags),
        )
        out[user_bi] = {
            "referredBy": clean_string(
                row.get("referrer_name", ""), MAX_NODE_NAME).lower(),
            "referralStatus": clean_string(row.get("decision", ""), 20),
            "referralRiskFlags": risk_flags,
            "referralRiskScore": displayed_risk_score,
            "referralClickId": clean_string(
                row.get("referral_click_id", ""), 64),
            "referralCreatedAt": int(row.get("created_at") or 0),
            "referralVerifiedAt": int(row.get("verified_at") or 0),
            "referralCreditedAt": int(row.get("credited_at") or 0),
            "referralClicks": referral_clicks,
            "referralAuditedUniqueClicks": unique_clicks,
            "referralDuplicateClickAttempts": int(
                audited.get("duplicate_clicks") or 0),
            "referralRejectedClickAttempts": int(
                audited.get("rejected_clicks") or 0),
            "referralLegacyClickCount": max(
                0, referral_clicks - unique_clicks),
            "creditedReferrals": credited_referrals,
            "referralLastAt": int(row.get("referral_last_at") or 0),
            "referredUsers": referred_users,
            "legacyUnattributed": max(
                0, int(row.get("legacy_unattributed") or 0)),
            "signupIp": clean_string(protected.get("ip", ""), 64),
            "signupIpFingerprint": clean_string(
                row.get("signup_ip_bi") or row.get("ip_bi") or "", 128),
            "signupUserAgent": clean_string(
                protected.get("userAgent", ""), 512),
            "signupUserAgentFingerprint": clean_string(
                row.get("user_agent_bi", ""), 128),
            "signupClientFingerprint": clean_string(
                row.get("client_fingerprint_bi", ""), 128),
            "referralNetworkFingerprint": clean_string(
                row.get("network_window_bi", ""), 128),
            "signupCountry": clean_string(
                protected.get("country", ""), 8).upper(),
            "signupClientCategory": clean_string(
                protected.get("clientCategory", ""), 40),
            "emailOpens": max(0, int(email_events.get("opens") or 0)),
            "emailClicks": max(0, int(email_events.get("clicks") or 0)),
            "lastEmailOpenedAt": max(
                0, int(email_events.get("last_opened_at") or 0)),
            "lastEmailClickedAt": max(
                0, int(email_events.get("last_clicked_at") or 0)),
        }
    if isinstance(memo, dict):
        memo["fields"] = {key: dict(value) for key, value in out.items()}
        memo["ts"] = now
    return out


async def _account_admin_quarantined_users(env):
    """Return preserved verification evidence for the admin-only Users tab."""
    rows = await d1_all(
        env,
        "SELECT q.data AS account_data,q.user_bi,q.email_bi,q.ip_bi,"
        "q.discord_bi,q.is_admin,q.enable_outreach,q.referrer_bi,"
        "q.username,q.reason_codes,"
        "q.status AS quarantine_status,q.quarantined_at,q.review_after,"
        "q.reviewed_at,q.reviewed_by,r.referrer_name,r.decision,"
        "r.risk_flags,r.risk_score,r.referral_click_id,"
        "r.created_at AS referral_created_at,"
        "r.verified_at AS referral_verified_at,"
        "r.credited_at AS referral_credited_at,"
        "r.signup_ip_bi,r.user_agent_bi,r.client_fingerprint_bi,"
        "r.network_window_bi,r.data AS referral_data,"
        "(SELECT COUNT(*) FROM quarantined_nodes n "
        "WHERE n.quarantined_user_bi=q.user_bi) AS node_count,"
        "(SELECT GROUP_CONCAT(n.name, ',') FROM quarantined_nodes n "
        "WHERE n.quarantined_user_bi=q.user_bi) AS node_names "
        "FROM quarantined_users q LEFT JOIN referral_attributions r "
        "ON r.referred_bi=q.user_bi "
        "ORDER BY q.quarantined_at DESC,q.username COLLATE NOCASE LIMIT ?",
        1000,
    )
    out = []
    for row in rows or []:
        rec = await decrypt_row(env, row.get("account_data", "")) or {}
        if not isinstance(rec, dict):
            rec = {}
        protected = (
            await decrypt_row(env, row.get("referral_data", ""))
            if row.get("referral_data") else {}) or {}
        if not isinstance(protected, dict):
            protected = {}
        signup = rec.get("signup") if isinstance(rec.get("signup"), dict) else {}
        name = clean_string(
            rec.get("name") or row.get("username") or "",
            MAX_NODE_NAME,
        ).strip().lower()
        if not valid_node_name(name):
            continue
        reasons = _referral_flags(row.get("reason_codes"))
        risk_flags = _referral_flags(row.get("risk_flags"))
        for reason in reasons:
            if reason not in risk_flags:
                risk_flags.append(reason)
        referred_by = clean_string(
            row.get("referrer_name") or signup.get("referrer") or "",
            MAX_NODE_NAME,
        ).strip().lower()
        world_fields = _account_world_client_fields(rec)
        discord_id = _clean_discord_id(rec.get("discordId", ""))
        node_names = []
        for node_name in str(row.get("node_names") or "").split(","):
            node_name = clean_string(
                node_name, MAX_NODE_NAME).strip().lower()
            if valid_node_name(node_name) and node_name not in node_names:
                node_names.append(node_name)
        out.append({
            "name": name,
            "email": clean_string(rec.get("email", ""), 254),
            "emailVerified": bool(rec.get("email_verified")),
            "createdAt": max(0, int(rec.get("created_at") or 0)),
            "avatarUpdatedAt": max(
                0, int(rec.get("avatar_updated_at") or 0)),
            "accountStatus": clean_string(
                rec.get("status", "active"), 32),
            "profilePrivate": bool(rec.get("profile_private")),
            "isAdmin": bool(int(row.get("is_admin") or 0)),
            "outreachEnabled": bool(
                int(row.get("enable_outreach") or 0)),
            "emailNotifications": rec.get("email_notifications") is not False,
            "discordId": discord_id,
            "discordUsername": clean_string(
                rec.get("discordUsername", ""), 80),
            "discordDisplayName": clean_string(
                rec.get("discordDisplayName", ""), 100),
            "discordLinked": bool(row.get("discord_bi") or discord_id),
            "solana": clean_string(rec.get("solana", ""), 64).strip(),
            "mastodon": clean_string(rec.get("mastodon", ""), 120).strip(),
            "countryCode": clean_string(
                world_fields.get("countryCode", ""), 8).upper(),
            "browser": clean_string(world_fields.get("browser", ""), 40),
            "os": clean_string(world_fields.get("os", ""), 40),
            "userFingerprint": clean_string(row.get("user_bi", ""), 128),
            "emailFingerprint": clean_string(
                row.get("email_bi", ""), 128),
            "discordFingerprint": clean_string(
                row.get("discord_bi", ""), 128),
            "referrerFingerprint": clean_string(
                row.get("referrer_bi", ""), 128),
            "quarantineStatus": clean_string(
                row.get("quarantine_status", ""), 32),
            "quarantineReasons": reasons,
            "quarantinedAt": max(0, int(row.get("quarantined_at") or 0)),
            "reviewAfter": max(0, int(row.get("review_after") or 0)),
            "reviewedAt": max(0, int(row.get("reviewed_at") or 0)),
            "reviewedBy": clean_string(row.get("reviewed_by", ""), 80),
            "referredBy": referred_by,
            "referralStatus": clean_string(row.get("decision", ""), 20),
            "referralRiskFlags": risk_flags,
            "referralRiskScore": max(0, int(row.get("risk_score") or 0)),
            "referralClickId": clean_string(
                row.get("referral_click_id", ""), 64),
            "referralCreatedAt": max(
                0, int(row.get("referral_created_at") or 0)),
            "referralVerifiedAt": max(
                0, int(row.get("referral_verified_at") or 0)),
            "referralCreditedAt": max(
                0, int(row.get("referral_credited_at") or 0)),
            "signupIp": clean_string(
                protected.get("ip") or signup.get("ip") or "", 64),
            "signupIpFingerprint": clean_string(
                row.get("signup_ip_bi") or row.get("ip_bi") or "", 128),
            "signupUserAgent": clean_string(
                protected.get("userAgent") or signup.get("userAgent") or "",
                512,
            ),
            "signupUserAgentFingerprint": clean_string(
                row.get("user_agent_bi", ""), 128),
            "signupClientFingerprint": clean_string(
                row.get("client_fingerprint_bi", ""), 128),
            "referralNetworkFingerprint": clean_string(
                row.get("network_window_bi", ""), 128),
            "signupCountry": clean_string(
                protected.get("country") or signup.get("country") or "", 8,
            ).upper(),
            "signupClientCategory": clean_string(
                protected.get("clientCategory")
                or signup.get("clientCategory") or "",
                40,
            ),
            "nodeCount": max(0, int(row.get("node_count") or 0)),
            "nodes": node_names,
        })
    return out


async def _users_directory_cache_get():
    # Return the edge-held roster as a fresh no-store response, the same
    # rebuild git_advert_cache_get does: the stored copy carries a public
    # max-age so the Cache API will keep it, which must not reach the browser.
    try:
        hit = await js_caches.default.match(USERS_DIRECTORY_CACHE_KEY)
    except Exception:
        hit = None
    if hit is None:
        return None
    return JsResponse.new(hit.body, to_js({
        "status": 200,
        "headers": {
            "content-type": "application/json; charset=utf-8",
            "cache-control": USERS_DIRECTORY_CLIENT_CACHE_CONTROL,
        },
    }))


async def _public_member_count(env):
    # The one member count every surface should quote (site-wide chat badge,
    # World campfire HUD): what _is_public_roster_member admits, deduped by
    # name. Prefers the roster the directory endpoint already cached at the
    # edge; the decrypt scan runs at most once per isolate per TTL.
    now = int(Date.now())
    memo = _MEMBER_COUNT_MEMO
    if memo["count"] is not None and now - memo["ts"] < MEMBER_COUNT_MEMO_TTL_MS:
        return memo["count"]
    cached = await edge_cache_match(USERS_DIRECTORY_CACHE_KEY)
    if cached is not None:
        try:
            payload = json.loads(await cached.text())
            count = len(payload.get("users") or [])
            memo["count"] = count
            memo["ts"] = now
            return count
        except Exception:
            pass
    # Single-flight refill: a request resuming inside I/O started by another
    # request is not legal in Python Workers, so losing callers serve stale or
    # a cheap approximate count rather than sharing a future.
    if now - memo["refresh_ts"] < MEMBER_COUNT_MEMO_TTL_MS:
        if memo["count"] is not None:
            return memo["count"]
        row = await d1_first(
            env,
            "SELECT COUNT(*) AS c FROM users "
            "WHERE email_bi IS NOT NULL AND email_bi <> ''",
        )
        return int((row or {}).get("c") or 0)
    memo["refresh_ts"] = now
    try:
        seen = set()
        count = 0
        rows = await d1_all(env, "SELECT data FROM users LIMIT ?", 1000)
        for row in rows or []:
            rec = await decrypt_row(env, row.get("data", ""))
            if not _is_public_roster_member(rec):
                continue
            name = clean_string(rec.get("name", ""), MAX_NODE_NAME).lower()
            if not name or name in seen:
                continue
            seen.add(name)
            count += 1
    except BaseException:
        memo["refresh_ts"] = 0
        raise
    memo["count"] = count
    memo["ts"] = now
    return count


async def _account_users_directory(env, request, public_payload=False):
    # Public chat roster directory. The authenticated administrator variant
    # adds a complete users-table projection plus protected signup/referral
    # evidence and is never edge-cached. Its separate publicUsers array keeps
    # private/inactive accounts out of Chat and World even when the requesting
    # desktop is an administrator.
    admin_actor = ""
    if not public_payload:
        admin_actor = await _account_users_admin_actor(env, request)
        if not admin_actor:
            cached = await _users_directory_cache_get()
            if cached is not None:
                return cached
    directory_epoch = edge_cache_epoch(USERS_DIRECTORY_CACHE_KEY)
    credited_referrers = await _credited_referrers_by_user(env)
    admin_signup_fields = (
        await _account_admin_signup_fields(env) if admin_actor else {})
    quarantined_users = (
        await _account_admin_quarantined_users(env) if admin_actor else [])
    out = []
    public_out = []
    seen = set()
    async def scan():
        return list(await d1_all(
            env,
            "SELECT u.user_bi,"
            "a.total_active_ms,a.total_steps,"
            "c.issues,c.pulls,c.discussions,b.achievement_badges,"
            "COALESCE(o.office_visits,0) AS office_visits FROM users u "
            "LEFT JOIN world_user_activity a ON a.account_bi=u.user_bi "
            "LEFT JOIN contributor_activity c ON c.author_bi=u.user_bi "
            "LEFT JOIN (SELECT account_bi,GROUP_CONCAT(badge_slug) "
            "AS achievement_badges FROM badge_awards WHERE badge_slug IN "
            "('qa','coder','discord','poster','chat') GROUP BY account_bi) b "
            "ON b.account_bi=u.user_bi "
            "LEFT JOIN (SELECT account_bi,COUNT(*) AS office_visits "
            "FROM world_office_attendance WHERE visit_scope='office' "
            "GROUP BY account_bi) o ON o.account_bi=u.user_bi "
            "ORDER BY u.username COLLATE NOCASE LIMIT ?",
            1000,
        ) or [])

    shared = globals().get("d1_shared_read")
    activity = {}
    for activity_row in await (
            shared(env, "users-directory", scan) if shared else scan()):
        activity.setdefault(
            str(activity_row.get("user_bi") or ""), activity_row)
    rows = []
    for user_row, rec in await _users_directory_records(env):
        row = dict(activity.get(str(user_row.get("user_bi") or ""), {}))
        row.update(user_row)
        rows.append((row, rec))
    activity_buckets = await _world_user_activity_buckets(env)
    commit_totals = await _contribution_commit_totals(env)
    for row, rec in rows:
        public_roster_member = True
        if not _is_public_roster_member(rec):
            public_roster_member = False
            # The public response remains the active, non-private member
            # roster. A proven administrator gets every actual user record,
            # including private and non-active accounts, but never a malformed
            # node record that somehow landed in the users table.
            if not admin_actor or _account_kind(rec) != "user":
                continue
        name = clean_string(rec.get("name", ""), MAX_NODE_NAME).lower()
        if not name or name in seen:
            continue
        seen.add(name)
        user_payload = _account_chat_user_payload(
            rec, row.get("total_active_ms", 0),
            activity_buckets.get(row.get("user_bi"), ""),
            issues=row.get("issues", 0),
            pulls=row.get("pulls", 0),
            commits=commit_totals.get(row.get("user_bi"), 0),
            discussions=row.get("discussions", 0),
            total_steps=row.get("total_steps", 0),
            office_visits=row.get("office_visits", 0),
            discord_linked=bool(
                row.get("discord_bi")
                or _clean_discord_id(rec.get("discordId", ""))),
            achievement_badges=str(
                row.get("achievement_badges") or "").split(","),
        )
        user_payload["referredBy"] = credited_referrers.get(
            str(row.get("user_bi") or ""), "")
        if public_roster_member:
            # Copy before attaching any administrator-only identity or signup
            # fields below. The desktop feeds this array to shared Chat/World
            # presentation and the complete `users` array only to its guarded
            # administrator table.
            public_out.append(dict(user_payload))
        if admin_actor:
            signup_fields = dict(admin_signup_fields.get(
                str(row.get("user_bi") or ""), {}))
            signup_meta = (
                rec.get("signup") if isinstance(rec.get("signup"), dict)
                else {})
            if not signup_fields.get("referralStatus"):
                captured = signup_meta.get("referralCaptured")
                captured_referrer = clean_string(
                    signup_meta.get("referrer", ""), MAX_NODE_NAME,
                ).strip().lower()
                if captured is True and valid_node_name(captured_referrer):
                    signup_fields["referredBy"] = captured_referrer
                    signup_fields["referralStatus"] = "capture-only"
                    flags = list(signup_fields.get("referralRiskFlags") or [])
                    if "attribution_unavailable" not in flags:
                        flags.append("attribution_unavailable")
                    signup_fields["referralRiskFlags"] = flags
                elif captured is True:
                    signup_fields["referralStatus"] = "direct"
                elif captured is False:
                    signup_fields["referralStatus"] = "capture-failed"
                else:
                    signup_fields["referralStatus"] = "legacy-unattributed"
            signup_fields.update({
                "email": clean_string(rec.get("email", ""), 254),
                "signupIp": (
                    signup_fields.get("signupIp")
                    or clean_string(signup_meta.get("ip", ""), 64)),
                "signupUserAgent": (
                    signup_fields.get("signupUserAgent")
                    or clean_string(signup_meta.get("userAgent", ""), 512)),
                "signupCountry": (
                    signup_fields.get("signupCountry")
                    or clean_string(
                        signup_meta.get("country", ""), 8).upper()),
                "signupClientCategory": (
                    signup_fields.get("signupClientCategory")
                    or clean_string(
                        signup_meta.get("clientCategory", ""), 40)),
                "discordUsername": clean_string(
                    rec.get("discordUsername", ""), 80),
                "discordDisplayName": clean_string(
                    rec.get("discordDisplayName", ""), 100),
                "discordId": _clean_discord_id(rec.get("discordId", "")),
                "discordLinked": bool(
                    row.get("discord_bi")
                    or _clean_discord_id(rec.get("discordId", ""))),
                "profilePrivate": bool(rec.get("profile_private")),
                "isAdmin": bool(int(row.get("is_admin") or 0)),
                "outreachEnabled": bool(
                    int(row.get("enable_outreach") or 0)),
                "emailNotifications": (
                    rec.get("email_notifications") is not False),
                "userFingerprint": clean_string(
                    row.get("user_bi", ""), 128),
                "emailFingerprint": clean_string(
                    row.get("email_bi", ""), 128),
                "discordFingerprint": clean_string(
                    row.get("discord_bi", ""), 128),
            })
            user_payload.update(signup_fields)
        out.append(user_payload)

    out.sort(key=lambda user: user.get("createdAt", 0))
    public_out.sort(key=lambda user: user.get("createdAt", 0))
    payload = {"ok": True, "users": out}
    if admin_actor:
        payload["publicUsers"] = public_out
        payload["quarantinedUsers"] = quarantined_users
        payload["signupReferralCode"] = (
            await _admin_signup_referral_code_state(env))
    if (not admin_actor and directory_epoch
            == edge_cache_epoch(USERS_DIRECTORY_CACHE_KEY)):
        await edge_cache_put(
            USERS_DIRECTORY_CACHE_KEY,
            json_response(payload, cache_seconds=USERS_DIRECTORY_TTL),
        )
    if public_payload:
        return payload
    return json_response(
        payload, cache_control=USERS_DIRECTORY_CLIENT_CACHE_CONTROL)


_USERS_DIRECTORY_RECORDS = {}


def _users_directory_tag(row):
    return "%s:%s" % (row.get("data_tag") or "", int(row.get("data_len") or 0))


async def _users_directory_records(env):
    rows = list(await d1_all(
        env,
        "SELECT user_bi,email_bi,ip_bi,is_admin,enable_outreach,discord_bi,"
        "length(data) AS data_len,substr(data,1,24) AS data_tag FROM users "
        "ORDER BY username COLLATE NOCASE LIMIT ?",
        1000,
    ) or [])
    memo = _USERS_DIRECTORY_RECORDS
    present = set()
    wanted = []
    for row in rows:
        key = str(row.get("user_bi") or "")
        if not key or key in present:
            continue
        present.add(key)
        hit = memo.get(key)
        if hit is None or hit[0] != _users_directory_tag(row):
            wanted.append(key)
    batch = int(globals().get("USERS_DIRECTORY_DATA_BATCH", 50) or 50)
    for offset in range(0, len(wanted), batch):
        chunk = wanted[offset:offset + batch]
        fetched = await d1_all(
            env,
            "SELECT user_bi,length(data) AS data_len,"
            "substr(data,1,24) AS data_tag,data FROM users "
            "WHERE user_bi IN (%s)" % ",".join("?" * len(chunk)),
            *chunk,
        )
        for row in fetched or []:
            key = str(row.get("user_bi") or "")
            if key in present:
                memo[key] = (
                    _users_directory_tag(row),
                    await decrypt_row(env, row.get("data", "")))
    for key in [key for key in memo if key not in present]:
        memo.pop(key, None)
    out = []
    seen = set()
    for row in rows:
        key = str(row.get("user_bi") or "")
        hit = memo.get(key)
        if key in seen or hit is None:
            continue
        seen.add(key)
        out.append((row, hit[1]))
    return out


async def _users_directory_public_payload(env):
    cached = await edge_cache_match(USERS_DIRECTORY_CACHE_KEY)
    if cached is not None:
        try:
            payload = json.loads(await cached.text())
        except Exception:
            payload = None
        if isinstance(payload, dict) and isinstance(payload.get("users"), list):
            return payload
    return await _account_users_directory(env, None, public_payload=True)


def _users_world_etag(body):
    return 'W/"%s"' % hashlib.sha256(body.encode("utf-8")).hexdigest()[:32]


def _users_world_not_modified(request, etag):
    try:
        header = str(request.headers.get("if-none-match") or "")
    except Exception:
        return False
    if not header or not etag:
        return False
    wanted = etag[2:] if etag.startswith("W/") else etag
    for candidate in header.split(","):
        candidate = candidate.strip()
        if candidate == "*":
            return True
        if candidate.startswith("W/"):
            candidate = candidate[2:]
        if candidate == wanted:
            return True
    return False


def _users_world_response(body, etag="", request=None):
    headers = {
        "content-type": "application/json; charset=utf-8",
        "cache-control": USERS_WORLD_CACHE_CONTROL,
        "cdn-cache-control": "no-store",
    }
    if etag:
        headers["etag"] = etag
        if request is not None and _users_world_not_modified(request, etag):
            del headers["content-type"]
            return Response(None, status=304, headers=headers)
    return Response(body, status=200, headers=headers)


async def _users_world_build(env):
    epoch = edge_cache_epoch(USERS_WORLD_CACHE_KEY)
    payload = _users_world_payload(await _users_directory_public_payload(env))
    body = json.dumps(payload, separators=(",", ":"))
    etag = _users_world_etag(body)
    if epoch == edge_cache_epoch(USERS_WORLD_CACHE_KEY):
        await edge_cache_put(USERS_WORLD_CACHE_KEY, Response(
            body, status=200, headers={
                "content-type": "application/json; charset=utf-8",
                "cache-control": "public, max-age=%d" % (
                    USERS_WORLD_STALE_SECONDS),
                "etag": etag,
                USERS_WORLD_BUILT_HEADER: str(int(Date.now())),
            }))
    return body, etag


async def _users_world_refresh(env):
    await _users_world_build(env)


async def _account_users_world_directory(env, request):
    cached = await edge_cache_match(USERS_WORLD_CACHE_KEY)
    if cached is not None:
        try:
            body = await cached.text()
            etag = str(cached.headers.get("etag") or "") or (
                _users_world_etag(body))
            built = int(cached.headers.get(USERS_WORLD_BUILT_HEADER) or 0)
        except Exception:
            body = None
        if body is not None:
            age = int(Date.now()) - built
            if 0 <= age < USERS_WORLD_FRESH_MS or _host_background(
                    env, "users-world",
                    lambda: _users_world_refresh(env)):
                return _users_world_response(body, etag, request)
    body, etag = await _users_world_build(env)
    return _users_world_response(body, etag, request)


def _users_world_payload(payload):
    users = []
    for user in payload.get("users") or []:
        row = dict(user)
        avatar_b64 = _avatar_b64(row.pop("avatarPng", ""))
        if avatar_b64:
            digest = _avatar_digest(avatar_b64)
            thumb = _avatar_thumbnail_webp(avatar_b64, digest)
            if thumb:
                row["avatarThumb"] = "data:image/webp;base64," + (
                    base64.b64encode(thumb).decode("ascii"))
            if thumb or _avatar_png_bytes(avatar_b64):
                row["avatarUrl"] = _avatar_url(row.get("name", ""), digest)
        users.append(row)
    return {**payload, "users": users}


AVATAR_THUMB_SIZE = 128
AVATAR_THUMB_MAX_SOURCE_PIXELS = 4096 * 4096
AVATAR_THUMB_CACHE_MAX = 512
AVATAR_IMMUTABLE_CACHE_CONTROL = "public, max-age=31536000, immutable"
AVATAR_SHORT_CACHE_CONTROL = "public, max-age=60"
_AVATAR_THUMBS = {}


def _avatar_b64(value):
    if not isinstance(value, str):
        return ""
    value = value.strip()
    if value.startswith("data:image/png;base64,"):
        value = value.split(",", 1)[1].strip()
    return value


def _avatar_digest(avatar_b64):
    return hashlib.sha256(avatar_b64.encode("utf-8")).hexdigest()[:16]


def _avatar_url(name, digest):
    return "/api/accounts/%s/avatar?v=%s" % (quote(str(name or "")), digest)


def _avatar_png_bytes(avatar_b64):
    if not avatar_b64 or not AVATAR_B64_RE.match(avatar_b64):
        return b""
    try:
        raw = base64.b64decode(avatar_b64, validate=True)
    except Exception:
        return b""
    return raw if raw.startswith(PNG_HEADER) else b""


def _avatar_image_module():
    try:
        from PIL import Image
    except Exception:
        return None
    return Image


def _avatar_render_thumbnail(image_module, raw):
    try:
        with image_module.open(io.BytesIO(raw)) as source:
            width, height = source.size
            if (source.format != "PNG" or width <= 0 or height <= 0
                    or width * height > AVATAR_THUMB_MAX_SOURCE_PIXELS):
                return b""
            image = source.convert("RGBA")
        side = min(width, height)
        left = (width - side) // 2
        top = (height - side) // 2
        image = image.crop((left, top, left + side, top + side))
        if side != AVATAR_THUMB_SIZE:
            image = image.resize(
                (AVATAR_THUMB_SIZE, AVATAR_THUMB_SIZE),
                image_module.Resampling.LANCZOS)
        out = io.BytesIO()
        image.save(out, "WEBP", quality=80, method=6)
        return out.getvalue()
    except Exception:
        return b""


def _avatar_thumbnail_webp(avatar_b64, digest):
    cached = _AVATAR_THUMBS.pop(digest, None)
    if cached is not None:
        _AVATAR_THUMBS[digest] = cached
        return cached
    image_module = _avatar_image_module()
    if image_module is None:
        return b""
    raw = _avatar_png_bytes(avatar_b64)
    thumb = _avatar_render_thumbnail(image_module, raw) if raw else b""
    _AVATAR_THUMBS[digest] = thumb
    while len(_AVATAR_THUMBS) > AVATAR_THUMB_CACHE_MAX:
        _AVATAR_THUMBS.pop(next(iter(_AVATAR_THUMBS)))
    return thumb


def _avatar_not_found():
    return json_response(
        {"error": "not_found"}, status=404, cache_control="no-store")


async def _account_avatar(env, request, raw_name):
    name = clean_string(raw_name, MAX_NODE_NAME).strip().lower()
    if not valid_node_name(name):
        return _avatar_not_found()
    query = parse_qs(urlparse(request.url).query)
    size = str(query.get("size", [""])[0] or "")
    version = str(query.get("v", [""])[0] or "")
    if size and size != str(AVATAR_THUMB_SIZE):
        return json_response(
            {"error": "bad_size"}, status=400, cache_control="no-store")
    rec = await _account_identity_rec_by_bi(env, await blind_index(env, name))
    if not _is_public_roster_member(rec):
        return _avatar_not_found()
    if clean_string(rec.get("name", ""), MAX_NODE_NAME).lower() != name:
        return _avatar_not_found()
    avatar_b64 = _avatar_b64(rec.get("avatar_png", ""))
    raw = _avatar_png_bytes(avatar_b64)
    if not raw:
        return _avatar_not_found()
    digest = _avatar_digest(avatar_b64)
    if size:
        body = _avatar_thumbnail_webp(avatar_b64, digest)
        content_type = "image/webp"
    else:
        body = raw
        content_type = "image/png"
    if not body:
        return _avatar_not_found()
    return JsResponse.new(Uint8Array.new(_to_js(bytes(body))), to_js({
        "status": 200,
        "headers": {
            "content-type": content_type,
            "cache-control": (
                AVATAR_IMMUTABLE_CACHE_CONTROL if version == digest
                else AVATAR_SHORT_CACHE_CONTROL),
            "cdn-cache-control": "no-store",
            "x-content-type-options": "nosniff",
        },
    }))


async def _account_session_lookup_memo(env, token):
    if not ACCOUNT_SESSION_TOKEN_RE.fullmatch(
            clean_string(token or "", 512).strip()):
        return "", None, ""
    digest = _account_session_token_digest(env, token)
    now = int(Date.now())
    hit = _ACCOUNT_SESSION_MEMO.get(digest)
    if hit is None or not 0 <= now - hit[0] < ACCOUNT_SESSION_MEMO_MS:
        epoch = _ACCOUNT_SESSION_MEMO_EPOCH[0]

        async def load():
            return await _account_session_lookup(env, token)

        join = _host_single_flight(env)
        account_bi, rec, session_id = await (
            join("account-session:" + digest, load) if join is not None
            else load())
        if not account_bi or not rec:
            return "", None, ""
        hit = (now, account_bi, rec, session_id)
        _account_session_memo_store(_ACCOUNT_SESSION_MEMO, digest, hit, epoch)
    caller = globals().get("_D1_QUERY_CALLER")
    if caller is not None:
        caller.note_session(hit[2].get("name"), hit[3])
    return hit[1], json.loads(json.dumps(hit[2])), hit[3]


ACCOUNT_LOOKUP_CLIENT_SECONDS = 60
ACCOUNT_LOOKUP_FRESH_MS = 60 * 1000
ACCOUNT_LOOKUP_STALE_SECONDS = 15 * 60
ACCOUNT_LOOKUP_BUILT_HEADER = "x-forkmesh-built"
ACCOUNT_LOOKUP_REFRESH_ORIGIN = "https://forkmesh.internal"
_ACCOUNT_LOOKUP_REFRESH = contextvars.ContextVar(
    "forkmesh_account_lookup_refresh", default=None)


async def _account_lookup_store(env, key, payload):
    if getattr(env, "HOST_BACKGROUND", None) is None:
        await edge_cache_put(key, json_response(
            payload, cache_seconds=ACCOUNT_LOOKUP_CLIENT_SECONDS))
        return
    epoch = _ACCOUNT_LOOKUP_REFRESH.get()
    if epoch is not None and epoch != edge_cache_epoch(key):
        return
    await edge_cache_put(key, json_response(
        payload, cache_seconds=ACCOUNT_LOOKUP_STALE_SECONDS,
        extra_headers={ACCOUNT_LOOKUP_BUILT_HEADER: str(int(Date.now()))}))


async def _account_lookup_cached(env, key, name):
    if getattr(env, "HOST_BACKGROUND", None) is None:
        return await edge_cache_match(key)
    if _ACCOUNT_LOOKUP_REFRESH.get() is not None:
        return None
    cached = await edge_cache_match(key)
    if cached is None:
        return None
    try:
        built = int(cached.headers.get(ACCOUNT_LOOKUP_BUILT_HEADER) or 0)
        body = await cached.text()
    except Exception:
        return None
    age = int(Date.now()) - built
    if not 0 <= age < ACCOUNT_LOOKUP_FRESH_MS and not _host_background(
            env, "account-lookup:" + key,
            lambda: _account_lookup_refresh(env, key, name)):
        return None
    return Response(body, status=200, headers={
        "content-type": "application/json; charset=utf-8",
        "cache-control": "public, max-age=%d" % ACCOUNT_LOOKUP_CLIENT_SECONDS,
    })


async def _account_lookup_refresh(env, key, name):
    token = _ACCOUNT_LOOKUP_REFRESH.set(edge_cache_epoch(key))
    try:
        await accounts_handler(env, JsRequest.new(
            ACCOUNT_LOOKUP_REFRESH_ORIGIN + "/api/accounts/"
            + quote(clean_string(name, MAX_NODE_NAME).lower())))
    finally:
        _ACCOUNT_LOOKUP_REFRESH.reset(token)


WORLD_WALLET_WARM_MAX = 32


async def _world_wallet_warm(env):
    cached = await edge_cache_match(USERS_WORLD_CACHE_KEY)
    if cached is None:
        return 0
    try:
        payload = json.loads(await cached.text())
    except Exception:
        return 0
    addresses = []
    for user in payload.get("users") or []:
        address = str((user or {}).get("solana") or "").strip() if (
            isinstance(user, dict)) else ""
        if (SOLANA_RE.match(address) and address not in addresses
                and address not in _WORLD_WALLET_LAST):
            addresses.append(address)
    warmed = 0
    for address in addresses[:WORLD_WALLET_WARM_MAX]:
        try:
            await _world_wallet_remember(
                env, JsRequest.new(WORLD_WALLET_CACHE_PREFIX + address),
                address)
        except Exception:
            continue
        warmed += 1
    return warmed
