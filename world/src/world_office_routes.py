"""Office room HTTP routes and the Office marketing-tasks runtime.

Offices are an organization-scoped World district: attendance, floor and
general-room access tickets, their Durable Object handoffs, and the marketing
task runtime. None of it is reachable before a request has already been routed
to an Office surface, so it stays out of the entrypoint's startup source for
the same reason the other World routes do.

``_bind_runtime`` supplies the entrypoint primitives on first use.
"""

import world_task_pings


def _bind_runtime(runtime):
    """Supply the initialized entrypoint primitives used by these routes."""
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


async def office_durable_object_request(request, claims, target_url=None):
    """Rebuild an Office upgrade from one explicit handshake allowlist."""
    headers = {}
    for name in (
        "upgrade", "connection", "sec-websocket-version", "sec-websocket-key",
    ):
        try:
            value = request.headers.get(name)
        except Exception:
            value = None
        if value:
            headers[name] = value
    safe_claim = {
        "scope": str((claims or {}).get("scope") or ""),
        "version": int((claims or {}).get("version") or 0),
        "account_bi": str((claims or {}).get("account_bi") or ""),
        "session_id": str((claims or {}).get("session_id") or ""),
        "name": world_protocol.clean_display_name(
            (claims or {}).get("name"), "Guest"),
        "accountStatus": str(
            (claims or {}).get("accountStatus") or "Guest"),
        "nonce": str((claims or {}).get("nonce") or ""),
    }
    encoded_claim = base64.urlsafe_b64encode(
        json.dumps(safe_claim, separators=(",", ":")).encode()
    ).decode().rstrip("=")
    headers["x-forkmesh-office-claim"] = encoded_claim
    source_url = urlparse(str(target_url or request.url))
    safe_target_url = (
        source_url.scheme + "://" + source_url.netloc + source_url.path)
    return JsRequest.new(
        safe_target_url,
        to_js({"method": "GET", "headers": headers}),
    )


def _office_entry_ticket(env, account_bi):
    """Issue a short-lived, account-bound proof of Office admission."""
    account_bi = str(account_bi or "")
    if not re.fullmatch(r"[0-9a-f]{64}", account_bi):
        raise ValueError("registered Office account required")
    expires = int(Date.now()) + OFFICE_ENTRY_TICKET_TTL_MS
    nonce = new_world_peer_id()
    canonical = ".".join((
        "v2",
        "world-general",
        account_bi,
        str(expires),
        nonce,
    ))
    signature = hmac.new(
        (_require_data_secret(env) + ":office-entry-ticket-v1").encode(),
        canonical.encode(),
        "sha256",
    ).hexdigest()
    return canonical + "." + signature


def _office_entry_ticket_claims(env, token):
    """Verify an account-bound Office entry proof without URL credentials."""
    raw = str(token or "")
    if len(raw) > 384:
        return None
    parts = raw.split(".")
    if len(parts) != 6:
        return None
    version_tag, scope, account_bi, expires_raw, nonce, signature = parts
    if version_tag != "v2" or scope != "world-general":
        return None
    if not re.fullmatch(r"[0-9a-f]{64}", account_bi):
        return None
    if not re.fullmatch(r"[A-Za-z0-9_-]{8,32}", nonce):
        return None
    if not re.fullmatch(r"[0-9a-f]{64}", signature):
        return None
    try:
        expires = int(expires_raw)
    except (TypeError, ValueError):
        return None
    now = int(Date.now())
    if (
        expires <= now
        or expires - now > OFFICE_ENTRY_TICKET_TTL_MS
    ):
        return None
    canonical = ".".join(parts[:5])
    expected = hmac.new(
        (_require_data_secret(env) + ":office-entry-ticket-v1").encode(),
        canonical.encode(),
        "sha256",
    ).hexdigest()
    if not hmac.compare_digest(signature, expected):
        return None
    return {
        "scope": scope,
        "accountBi": account_bi,
        "expires": expires,
        "nonce": nonce,
    }


def _office_meeting_ticket(
        env, scope, key_version, account_bi="", name="", session_id=""):
    """Issue a short-lived claim for one authorized spatial meeting."""
    scope = str(scope or "").strip()
    if scope != "world-general" and not re.fullmatch(
            r"[0-9a-f]{32}", scope):
        raise ValueError("invalid_office_scope")
    try:
        key_version = int(key_version)
    except (TypeError, ValueError):
        raise ValueError("invalid_office_version") from None
    if key_version < 1 or key_version > 999999999:
        raise ValueError("invalid_office_version")
    account_bi = str(account_bi or "").strip()
    if account_bi and not re.fullmatch(r"[0-9a-f]{64}", account_bi):
        raise ValueError("invalid_office_account")
    session_id = str(session_id or "").strip()
    if not re.fullmatch(r"[A-Za-z0-9_-]{24,64}", session_id):
        raise ValueError("invalid_office_session")
    name = str(name or "").strip()
    if len(name) > 64 or any(ord(ch) < 32 for ch in name):
        raise ValueError("invalid_office_name")
    encoded_name = base64.urlsafe_b64encode(name.encode()).decode().rstrip("=")
    expires = int(Date.now()) + OFFICE_MEETING_TICKET_TTL_MS
    nonce = new_world_peer_id()
    canonical = ".".join((
        "v2",
        scope,
        str(key_version),
        account_bi or "-",
        session_id,
        encoded_name or "-",
        str(expires),
        nonce,
    ))
    signature = hmac.new(
        (_require_data_secret(env) + ":office-meeting-ticket-v2").encode(),
        canonical.encode(),
        "sha256",
    ).hexdigest()
    return canonical + "." + signature


def _office_meeting_ticket_claims(env, token):
    """Verify and decode one Office meeting claim without side effects."""
    parts = str(token or "").split(".")
    if len(parts) != 9:
        return None
    (version_tag, scope, version_raw, account_raw, session_id, name_raw,
     expires_raw, nonce, signature) = parts
    if version_tag != "v2":
        return None
    if scope != "world-general" and not re.fullmatch(
            r"[0-9a-f]{32}", scope):
        return None
    account_bi = "" if account_raw == "-" else account_raw
    if account_bi and not re.fullmatch(r"[0-9a-f]{64}", account_bi):
        return None
    if not re.fullmatch(r"[A-Za-z0-9_-]{24,64}", session_id):
        return None
    if not re.fullmatch(r"[A-Za-z0-9_-]{8,32}", nonce):
        return None
    if not re.fullmatch(r"[0-9a-f]{64}", signature):
        return None
    try:
        key_version = int(version_raw)
        expires = int(expires_raw)
    except (TypeError, ValueError):
        return None
    now = int(Date.now())
    if (
        key_version < 1
        or key_version > 999999999
        or expires <= now
        or expires - now > OFFICE_MEETING_TICKET_TTL_MS
    ):
        return None
    canonical = ".".join(parts[:8])
    expected = hmac.new(
        (_require_data_secret(env) + ":office-meeting-ticket-v2").encode(),
        canonical.encode(),
        "sha256",
    ).hexdigest()
    if not hmac.compare_digest(signature, expected):
        return None
    try:
        encoded_name = "" if name_raw == "-" else name_raw
        padded = encoded_name + "=" * ((4 - len(encoded_name) % 4) % 4)
        name = base64.urlsafe_b64decode(padded).decode()
    except Exception:
        return None
    if len(name) > 64 or any(ord(ch) < 32 for ch in name):
        return None
    return {
        "scope": scope,
        "version": key_version,
        "account_bi": account_bi,
        "session_id": session_id,
        "name": name,
        "expires": expires,
        "nonce": nonce,
    }


def _office_attendance_floor(value):
    floor_id = str(value or "").strip().lower()
    return floor_id if floor_id in OFFICE_ATTENDANCE_FLOOR_LABELS else ""


def _office_attendance_visit(row, observed_at=None):
    """Project one D1 attendance row into a bounded public lobby record."""
    if not isinstance(row, dict):
        return None
    visit_id = str(row.get("visit_id") or "")
    if not re.fullmatch(r"[0-9a-f]{32}", visit_id):
        return None
    account = world_protocol.clean_display_name(
        row.get("account_name"), "Contributor")
    try:
        in_at = int(row.get("in_at") or 0)
        raw_out_at = row.get("out_at")
        out_at = None if raw_out_at is None else int(raw_out_at)
    except (TypeError, ValueError):
        return None
    max_safe_integer = 9007199254740991
    if in_at <= 0 or in_at > max_safe_integer:
        return None
    if (
        out_at is not None
        and (out_at < in_at or out_at > max_safe_integer)
    ):
        return None
    try:
        observed_at = int(
            Date.now() if observed_at is None else observed_at)
    except (TypeError, ValueError):
        observed_at = in_at
    if observed_at <= 0 or observed_at > max_safe_integer:
        observed_at = in_at
    duration_end = out_at if out_at is not None else max(in_at, observed_at)
    return {
        "id": visit_id,
        "account": account,
        "inAt": in_at,
        "outAt": out_at,
        "durationMs": duration_end - in_at,
        "floor": (
            OFFICE_ATTENDANCE_FLOOR_LABELS.get(
                str(row.get("floor_id") or "").strip().lower(), "")
            if out_at is None
            else ""
        ),
    }


async def _office_attendance_recent(env, observed_at=None):
    try:
        observed_at = int(
            Date.now() if observed_at is None else observed_at)
    except (TypeError, ValueError):
        observed_at = 0
    # An OUT request can be interrupted by navigation or a lost connection.
    # A bounded server heartbeat is the live-presence source of truth: expire
    # those abandoned rows before projecting the public board.
    live_cutoff = observed_at - OFFICE_ATTENDANCE_LIVE_TTL_MS
    await d1_run(
        env,
        "UPDATE world_office_attendance "
        "SET out_at=MAX(in_at,COALESCE(NULLIF(last_seen_at,0),in_at)), "
        "floor_id='' "
        "WHERE out_at IS NULL "
        "AND COALESCE(NULLIF(last_seen_at,0),in_at)<?",
        live_cutoff,
    )
    rows = await d1_all(
        env,
        "SELECT visit_id, account_name, in_at, out_at, floor_id "
        "FROM world_office_attendance "
        "ORDER BY in_at DESC, visit_id DESC LIMIT 20",
    )
    visits = []
    for row in rows or []:
        visit = _office_attendance_visit(row, observed_at)
        if visit is not None:
            visits.append(visit)
    return visits


async def _office_attendance_leaderboard(env, observed_at=None):
    """Return one bounded row per member, ranked by their longest Office stay."""
    try:
        observed_at = int(
            Date.now() if observed_at is None else observed_at)
    except (TypeError, ValueError):
        observed_at = 0
    rows = await d1_all(
        env,
        "WITH office_visits AS ("
        "SELECT account_bi, account_name, in_at, out_at, floor_id, visit_id, "
        "MAX(0, COALESCE(out_at, ?) - in_at) AS duration_ms, "
        "ROW_NUMBER() OVER ("
        "PARTITION BY account_bi "
        "ORDER BY in_at DESC, visit_id DESC"
        ") AS newest_rank "
        "FROM world_office_attendance "
        "WHERE visit_scope='office'"
        "), member_totals AS ("
        "SELECT account_bi, MAX(duration_ms) AS longest_duration_ms, "
        "MAX(CASE WHEN out_at IS NULL THEN duration_ms ELSE 0 END) "
        "AS active_duration_ms, "
        "MAX(CASE WHEN out_at IS NULL THEN 1 ELSE 0 END) AS is_present "
        "FROM office_visits GROUP BY account_bi"
        ") "
        "SELECT office_visits.account_name, "
        "member_totals.longest_duration_ms, "
        "member_totals.active_duration_ms, member_totals.is_present, "
        "CASE WHEN member_totals.is_present=1 "
        "THEN office_visits.floor_id ELSE '' END AS floor_id "
        "FROM member_totals "
        "JOIN office_visits "
        "ON office_visits.account_bi=member_totals.account_bi "
        "AND office_visits.newest_rank=1 "
        "ORDER BY member_totals.longest_duration_ms DESC, "
        "office_visits.account_name COLLATE NOCASE ASC "
        "LIMIT 20",
        observed_at,
    )
    leaderboard = []
    for row in rows or []:
        if not isinstance(row, dict):
            continue
        account = world_protocol.clean_display_name(
            row.get("account_name"), "Contributor")
        try:
            longest_duration_ms = max(
                0, int(row.get("longest_duration_ms") or 0))
            active_duration_ms = max(
                0, int(row.get("active_duration_ms") or 0))
        except (TypeError, ValueError):
            continue
        present = bool(row.get("is_present"))
        leaderboard.append({
            "account": account,
            "longestDurationMs": longest_duration_ms,
            "activeDurationMs": active_duration_ms if present else 0,
            "present": present,
            "floor": (
                OFFICE_ATTENDANCE_FLOOR_LABELS.get(
                    str(row.get("floor_id") or "").strip().lower(), "")
                if present
                else ""
            ),
        })
    return leaderboard


async def office_attendance_handler(env, request):
    """Read the shared lobby board or punch an authenticated visit IN/OUT."""
    method = method_name(request)
    if method not in ("GET", "POST"):
        return json_response(
            {"error": "method_not_allowed"},
            status=405,
            cache_control="no-store, max-age=0, must-revalidate",
            extra_headers={
                "allow": "GET, POST",
                "x-content-type-options": "nosniff",
            },
        )
    if method == "GET":
        await ensure_schema(env)
        observed_at = int(Date.now())
        visits = await _office_attendance_recent(env, observed_at)
        return json_response(
            {
                "ok": True,
                "asOfAt": observed_at,
                "visits": visits,
                "leaderboard": await _office_attendance_leaderboard(
                    env, observed_at),
            },
            cache_control="no-store, max-age=0, must-revalidate",
            extra_headers={"x-content-type-options": "nosniff"},
        )

    if (world_bearer_visitor(env, request)
            if "world_bearer_visitor" in globals() else None):
        return json_response(
            {"ok": True, "recorded": False, "visitor": True},
            cache_control="no-store, max-age=0, must-revalidate",
            extra_headers={"x-content-type-options": "nosniff"},
        )
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response(
            {"error": "invalid_json"},
            status=400,
            cache_control="no-store, max-age=0, must-revalidate",
            extra_headers={"x-content-type-options": "nosniff"},
        )
    if not isinstance(data, dict):
        return json_response(
            {"error": "invalid_json"},
            status=400,
            cache_control="no-store, max-age=0, must-revalidate",
            extra_headers={"x-content-type-options": "nosniff"},
        )
    try:
        account_bi, account = await _account_session_record(
            env, request, data)
    except Exception:
        account_bi, account = "", None
    if (
        not re.fullmatch(r"[0-9a-f]{64}", str(account_bi or ""))
        or not account
        or account.get("status") != "active"
        or _account_kind(account) != "user"
    ):
        return json_response(
            {"error": "login_required"},
            status=401,
            cache_control="no-store, max-age=0, must-revalidate",
            extra_headers={"x-content-type-options": "nosniff"},
        )
    action = str(data.get("action") or "").strip().lower()
    if action not in ("in", "heartbeat", "out"):
        return json_response(
            {"error": "invalid_action"},
            status=400,
            cache_control="no-store, max-age=0, must-revalidate",
            extra_headers={"x-content-type-options": "nosniff"},
        )
    visit_id = str(data.get("visitId") or "").strip().lower()
    if visit_id and not re.fullmatch(r"[0-9a-f]{32}", visit_id):
        return json_response(
            {"error": "invalid_visit"},
            status=400,
            cache_control="no-store, max-age=0, must-revalidate",
            extra_headers={"x-content-type-options": "nosniff"},
        )

    await ensure_schema(env)
    now = int(Date.now())
    floor_id = _office_attendance_floor(data.get("floor"))
    initial_floor_id = floor_id or "lobby"
    if action == "in":
        # A page/physical entry owns one visit id. Retrying that same IN remains
        # idempotent, while a new page entry closes an abandoned open punch at
        # its last heartbeat instead of inheriting its elapsed time.
        if visit_id:
            await d1_run(
                env,
                "UPDATE world_office_attendance "
                "SET out_at=MAX(in_at,COALESCE(NULLIF(last_seen_at,0),in_at)), "
                "floor_id='' "
                "WHERE account_bi=? AND out_at IS NULL AND visit_id<>? "
                "AND NOT EXISTS ("
                "SELECT 1 FROM world_office_attendance WHERE visit_id=?"
                ")",
                str(account_bi),
                visit_id,
                visit_id,
            )
        await d1_run(
            env,
            "INSERT OR IGNORE INTO world_office_attendance "
            "(visit_id, account_bi, account_name, in_at, out_at, "
            "last_seen_at, floor_id, visit_scope) "
            "VALUES (?, ?, ?, ?, NULL, ?, ?, 'office')",
            visit_id or new_world_peer_id(),
            str(account_bi),
            world_protocol.clean_display_name(
                account.get("name"), "Contributor"),
            now,
            now,
            initial_floor_id,
        )
        await d1_run(
            env,
            "UPDATE world_office_attendance "
            "SET last_seen_at=?, "
            "floor_id=CASE WHEN ?<>'' THEN ? ELSE floor_id END "
            "WHERE account_bi=? AND out_at IS NULL "
            "AND (?='' OR visit_id=?)",
            now,
            floor_id,
            floor_id,
            str(account_bi),
            visit_id,
            visit_id,
        )
    elif action == "heartbeat":
        await d1_run(
            env,
            "UPDATE world_office_attendance "
            "SET last_seen_at=?, floor_id=? "
            "WHERE account_bi=? AND out_at IS NULL "
            "AND (?='' OR visit_id=?)",
            now,
            initial_floor_id,
            str(account_bi),
            visit_id,
            visit_id,
        )
    else:
        # Close, in place, only this account's newest open visit. Repeated OUT
        # requests are harmless because the row no longer matches out_at NULL.
        await d1_run(
            env,
            "UPDATE world_office_attendance "
            "SET out_at=?, last_seen_at=?, floor_id='' "
            "WHERE visit_id=("
            "SELECT visit_id FROM world_office_attendance "
            "WHERE account_bi=? AND out_at IS NULL "
            "AND (?='' OR visit_id=?) "
            "ORDER BY in_at DESC, visit_id DESC LIMIT 1"
            ") AND out_at IS NULL",
            now,
            now,
            str(account_bi),
            visit_id,
            visit_id,
        )
    visits = await _office_attendance_recent(env, now)
    return json_response(
        {
            "ok": True,
            "asOfAt": now,
            "visits": visits,
            "leaderboard": await _office_attendance_leaderboard(env, now),
        },
        cache_control="no-store, max-age=0, must-revalidate",
        extra_headers={"x-content-type-options": "nosniff"},
    )


async def office_general_status_handler(env, request):
    """Return the login-gate state; the retired coordination code is gone."""
    if method_name(request) != "GET":
        return json_response(
            {"error": "method_not_allowed"},
            status=405,
            cache_control="no-store, max-age=0, must-revalidate",
            extra_headers={"allow": "GET"},
        )
    try:
        _, account = await _account_session_record(env, request)
    except Exception:
        account = None
    authenticated = bool(
        account
        and account.get("status") == "active"
        and _account_kind(account) == "user"
    )
    return json_response(
        {
            "ok": True,
            "authenticated": authenticated,
            "requiresLogin": True,
        },
        cache_control="no-store, max-age=0, must-revalidate",
        extra_headers={"x-content-type-options": "nosniff"},
    )


async def office_general_entry_handler(env, request):
    """Admit an active registered account and mint its short-lived proof."""
    if method_name(request) != "POST":
        return json_response(
            {"error": "method_not_allowed"},
            status=405,
            cache_control="no-store, max-age=0, must-revalidate",
            extra_headers={"allow": "POST"},
        )
    if (world_bearer_visitor(env, request)
            if "world_bearer_visitor" in globals() else None):
        return json_response(
            {"ok": False, "error": "members_only", "visitor": True},
            status=403,
            cache_control="no-store, max-age=0, must-revalidate",
            extra_headers={"x-content-type-options": "nosniff"},
        )
    if not _request_origin_allowed(env, request):
        return json_response(
            {"error": "origin_not_allowed"},
            status=403,
            cache_control="no-store, max-age=0, must-revalidate",
        )
    try:
        account_bi, account = await _account_session_record(env, request)
    except Exception:
        account_bi, account = "", None
    if (
        not re.fullmatch(r"[0-9a-f]{64}", str(account_bi or ""))
        or not account
        or account.get("status") != "active"
        or _account_kind(account) != "user"
    ):
        return json_response(
            {"error": "login_required"},
            status=401,
            cache_control="no-store, max-age=0, must-revalidate",
            extra_headers={"x-content-type-options": "nosniff"},
        )
    try:
        token = _office_entry_ticket(env, str(account_bi))
        claims = _office_entry_ticket_claims(env, token) or {}
        expires = int(claims.get("expires") or 0)
    except Exception:
        token, expires = "", 0
    if not token or not expires:
        return json_response(
            {"error": "office_unavailable"},
            status=503,
            cache_control="no-store, max-age=0, must-revalidate",
        )
    return json_response(
        {
            "ok": True,
            "room": "general",
            "entryTicket": token,
            "expiresAt": expires,
            "requiresLogin": True,
        },
        cache_control="no-store, max-age=0, must-revalidate",
        extra_headers={"x-content-type-options": "nosniff"},
    )


def _office_team_slug(value):
    return re.sub(
        r"-+", "-",
        re.sub(r"[^a-z0-9]+", "-", str(value or "").strip().lower()),
    ).strip("-")[:64]


async def office_floor_access_handler(env, request):
    """Return only the signed-in viewer's server-derived elevator grants.

    The shared tower belongs to one authoritative organization.  A matching
    team name in a user-created organization must never unlock its floors, and
    a stale ``org_team_members`` row must not survive deletion of any parent.
    """
    if method_name(request) != "GET":
        return json_response(
            {"error": "method_not_allowed"},
            status=405,
            cache_control="no-store, max-age=0, must-revalidate",
            extra_headers={"allow": "GET"},
        )
    try:
        account_bi, account = await _account_session_record(env, request)
    except Exception:
        account_bi, account = "", None
    visitor = None if account else (
        world_bearer_visitor(env, request)
        if "world_bearer_visitor" in globals() else None)
    if visitor:
        return json_response(
            {
                "authenticated": True,
                "visitor": True,
                "account": "%s@%s" % (visitor["name"], visitor["home"]),
                "allowedFloorIds": ["lobby", "rooftop"],
                "teams": [],
                "organizationRole": "",
            },
            cache_control="private, no-store, max-age=0, must-revalidate",
            extra_headers={"x-content-type-options": "nosniff"},
        )
    if (
        not re.fullmatch(r"[0-9a-f]{64}", str(account_bi or ""))
        or not account
        or account.get("status") != "active"
        or _account_kind(account) != "user"
    ):
        return json_response(
            {
                "authenticated": False,
                "allowedFloorIds": [],
                "teams": [],
            },
            status=401,
            cache_control="no-store, max-age=0, must-revalidate",
        )
    await ensure_schema(env)
    office_org = str(
        getattr(env, "OFFICE_MARKETING_ORG", "") or "forkmesh"
    ).strip().lower()
    office_org_bi, office_org_row = (
        await _org_row(env, office_org)
        if valid_node_name(office_org)
        else ("", None)
    )
    # Plain membership of the authoritative organization, independent of any
    # team floor. The Office chat embed opens channel creation on this, so it
    # is re-derived here rather than inferred from `teams`: a member on no
    # team still belongs to the org, and a team row in a user-created org
    # never counts.
    membership = None
    if office_org_row:
        membership = await d1_first(
            env,
            "SELECT om.role AS role FROM org_members om "
            "INNER JOIN orgs o ON o.org_bi=om.org_bi "
            "WHERE om.org_bi=? AND o.name=? AND om.member_bi=? "
            "AND om.role IN ('owner','admin','maintainer','member','auditor') LIMIT 1",
            str(office_org_bi),
            office_org,
            str(account_bi),
        )
    organization_role = str((membership or {}).get("role") or "")
    rows = []
    if office_org_row:
        rows = await d1_all(
            env,
            "SELECT DISTINCT tm.team AS team "
            "FROM org_team_members tm "
            "INNER JOIN orgs o ON o.org_bi=tm.org_bi "
            "INNER JOIN org_members om "
            "ON om.org_bi=tm.org_bi AND om.member_bi=tm.member_bi "
            "INNER JOIN org_teams ot "
            "ON ot.org_bi=tm.org_bi AND ot.team=tm.team "
            "WHERE tm.org_bi=? AND o.name=? AND tm.member_bi=? "
            "AND om.role IN ('owner','admin','maintainer','member','auditor') "
            "AND ot.permission IN ('read','write','maintain','admin') "
            "ORDER BY tm.team LIMIT 50",
            str(office_org_bi),
            office_org,
            str(account_bi),
        )
    teams = sorted({
        slug
        for slug in (
            _office_team_slug((row or {}).get("team")) for row in rows or []
        )
        if slug
    })
    team_set = set(teams)
    # Only the lobby and the rooftop patio are common floors. Marketing is a
    # department story like every other one: it is granted below solely when
    # this account actually belongs to that team.
    allowed = ["lobby", "rooftop"]
    for floor_id, aliases in OFFICE_FLOOR_TEAM_ALIASES.items():
        if team_set.intersection(aliases):
            allowed.append(floor_id)
    return json_response(
        {
            "authenticated": True,
            "account": clean_string(
                account.get("name", ""), MAX_NODE_NAME).lower(),
            "allowedFloorIds": allowed,
            "teams": teams,
            "organizationRole": organization_role,
        },
        cache_control="private, no-store, max-age=0, must-revalidate",
        extra_headers={"x-content-type-options": "nosniff"},
    )


async def office_general_access_handler(env, request):
    """Issue a meeting claim for the same account admitted at the lobby."""
    if method_name(request) != "GET":
        return json_response(
            {"error": "method_not_allowed"},
            status=405,
            cache_control="no-store, max-age=0, must-revalidate",
            extra_headers={"allow": "GET"},
        )
    try:
        account_bi, record = await _account_session_record(env, request)
        session_id = _request_account_session_id(request)
        entry_claims = _office_entry_ticket_claims(
            env, request.headers.get("x-forkmesh-office-entry") or "")
    except Exception:
        account_bi, record, session_id, entry_claims = "", None, "", None
    if (
        not re.fullmatch(r"[0-9a-f]{64}", str(account_bi or ""))
        or not re.fullmatch(r"[A-Za-z0-9_-]{24,64}", session_id)
        or not record
        or record.get("status") != "active"
        or _account_kind(record) != "user"
        or not entry_claims
        or entry_claims.get("scope") != "world-general"
        or not hmac.compare_digest(
            str(entry_claims.get("accountBi") or ""),
            str(account_bi),
        )
    ):
        return json_response(
            {"error": "registered_office_entry_required"},
            status=401,
            cache_control="no-store, max-age=0, must-revalidate",
            extra_headers={"x-content-type-options": "nosniff"},
        )
    name = world_protocol.clean_display_name(
        record.get("name"), "Contributor")
    token = _office_meeting_ticket(
        env, "world-general", 1, str(account_bi), name, session_id)
    claims = _office_meeting_ticket_claims(env, token) or {}
    return json_response(
        {
            "room": {
                "id": "general",
                "name": "general",
                "visibility": "public",
            },
            "meetingWebSocketUrl": (
                "/api/world/office/general/ws?ticket="
                + quote(token, safe="")
            ),
            "expiresAt": int(claims.get("expires") or 0),
            "accountStatus": "Registered",
        },
        cache_control="no-store, max-age=0, must-revalidate",
        extra_headers={"x-content-type-options": "nosniff"},
    )


async def _office_general_socket_handler(env, request):
    if (
        method_name(request) != "GET"
        or (request.headers.get("upgrade") or "").lower() != "websocket"
        or not world_websocket_origin_allowed(request, env)
    ):
        return _private_replica_not_found()
    url = urlparse(request.url)
    ticket_values = parse_qs(
        url.query, keep_blank_values=False).get("ticket") or []
    claims = (
        _office_meeting_ticket_claims(env, ticket_values[0])
        if len(ticket_values) == 1 else None
    )
    if (
        not claims
        or claims["scope"] != "world-general"
        or not re.fullmatch(
            r"[0-9a-f]{64}", str(claims.get("account_bi") or ""))
    ):
        return _private_replica_not_found()
    try:
        session_account_bi, _session_record = (
            await _account_session_record(env, request))
        session_id = _request_account_session_id(request)
    except Exception:
        session_account_bi, session_id = "", ""
    if (
        not hmac.compare_digest(
            str(session_account_bi or ""),
            str(claims.get("account_bi") or ""))
        or not hmac.compare_digest(
            str(session_id or ""),
            str(claims.get("session_id") or ""))
    ):
        return _private_replica_not_found()
    claims["accountStatus"] = "Registered"
    return await _forward_office_socket(
        env,
        request,
        claims,
        "office:world-general:v1",
        "/api/world/office/world-general/v1/ws",
    )


async def _office_channel_socket_handler(env, request, channel_id):
    if (
        method_name(request) != "GET"
        or (request.headers.get("upgrade") or "").lower() != "websocket"
        or not world_websocket_origin_allowed(request, env)
    ):
        return _private_replica_not_found()
    url = urlparse(request.url)
    ticket_values = parse_qs(
        url.query, keep_blank_values=False).get("ticket") or []
    claims = (
        _office_meeting_ticket_claims(env, ticket_values[0])
        if len(ticket_values) == 1 else None
    )
    if not claims or claims["scope"] != channel_id:
        return _private_replica_not_found()
    try:
        session_account_bi, _session_record = (
            await _account_session_record(env, request))
        session_id = _request_account_session_id(request)
    except Exception:
        session_account_bi, session_id = "", ""
    if (
        not hmac.compare_digest(
            str(session_account_bi or ""),
            str(claims.get("account_bi") or ""))
        or not hmac.compare_digest(
            str(session_id or ""),
            str(claims.get("session_id") or ""))
    ):
        return _private_replica_not_found()
    try:
        await ensure_schema(env)
        channel = await d1_first(
            env,
            "SELECT data,created_by_bi,key_version FROM chat_channels "
            "WHERE channel_id=?",
            channel_id,
        )
        if (
            not channel
            or int(channel.get("key_version") or 0)
                != int(claims["version"])
        ):
            return _private_replica_not_found()
        channel_record = await decrypt_row(env, channel.get("data"))
        if not isinstance(channel_record, dict):
            return _private_replica_not_found()
        visibility = (
            "public"
            if channel_record.get("visibility") == "public"
            else "private"
        )
        account = await d1_first(
            env,
            "SELECT data FROM users WHERE user_bi=?",
            claims["account_bi"],
        )
        if not account:
            return _private_replica_not_found()
        account_record = await decrypt_row(env, account.get("data"))
        if (
            not account_record
            or account_record.get("status") != "active"
            or _account_kind(account_record) != "user"
        ):
            return _private_replica_not_found()
        is_creator = hmac.compare_digest(
            str(channel.get("created_by_bi") or ""),
            str(claims["account_bi"]),
        )
        if channel_record.get("kind") == "notices" and not is_creator:
            return _private_replica_not_found()
        if not is_creator and visibility != "public":
            membership = await d1_first(
                env,
                "SELECT 1 AS allowed FROM chat_channel_members "
                "WHERE channel_id=? AND member_bi=?",
                channel_id,
                claims["account_bi"],
            )
            if not membership:
                return _private_replica_not_found()
        claims["name"] = world_protocol.clean_display_name(
            account_record.get("name"), "Contributor")
        claims["accountStatus"] = "Registered"
    except Exception:
        return _private_replica_not_found()

    version = int(claims["version"])
    return await _forward_office_socket(
        env,
        request,
        claims,
        "office:chat-channel:" + channel_id + ":v" + str(version),
        "/api/world/office/" + channel_id + "/v" + str(version) + "/ws",
    )


class _TaskPingStore:
    def __init__(self, env):
        self.env = env

    async def muted(self, recipient):
        settings = globals().get("_ping_delivery_settings")
        if not callable(settings):
            return False
        muted, _paused = await settings(self.env, recipient, int(Date.now()))
        return world_task_pings.KIND in muted

    async def window(self, recipient, now):
        rows = await d1_all(
            self.env,
            "SELECT dedupe_bi,ts,read_at,data FROM notifications "
            "WHERE recipient_bi=? AND ts>=? ORDER BY ts DESC LIMIT ?",
            await blind_index(self.env, recipient),
            int(now) - world_task_pings.WINDOW_MS,
            world_task_pings.SCAN_ROWS,
        )
        found = []
        for row in rows or []:
            try:
                record = await decrypt_row(self.env, row.get("data", ""))
            except Exception:
                continue
            if (
                not isinstance(record, dict)
                or record.get("kind") != world_task_pings.KIND
                or not isinstance(record.get("meta"), dict)
            ):
                continue
            found.append({
                "dedupeBi": str(row.get("dedupe_bi") or ""),
                "ts": int(row.get("ts") or 0),
                "read": int(row.get("read_at") or 0) > 0,
                "meta": record["meta"],
            })
        return found

    async def file(self, recipient, title, body, href, actor, source,
                   dedupe, meta, push):
        return await enqueue_notification(
            self.env,
            recipient,
            world_task_pings.KIND,
            title,
            body=body,
            href=href,
            actor=actor if valid_node_name(actor) else "",
            source=source,
            dedupe=dedupe,
            meta=meta,
            push=push,
        )

    async def drop(self, recipient, dedupe_bi):
        if not dedupe_bi:
            return
        await d1_run(
            self.env,
            "DELETE FROM notifications WHERE dedupe_bi=? AND recipient_bi=?",
            dedupe_bi,
            await blind_index(self.env, recipient),
        )


async def _organization_task_participants(env, org_bi, task_id):
    try:
        rows = await d1_all(
            env,
            "SELECT data FROM organization_task_events "
            "WHERE org_bi=? AND task_id=? AND kind IN ('replied','checkin') "
            "ORDER BY created_at DESC LIMIT ?",
            str(org_bi or ""),
            str(task_id or ""),
            50,
        )
    except Exception:
        return []
    names = []
    for row in rows or []:
        try:
            record = await decrypt_row(env, row.get("data", ""))
        except Exception:
            continue
        name = clean_string(
            (record or {}).get("actor") if isinstance(record, dict) else "",
            MAX_NODE_NAME,
        ).strip().lower()
        if valid_node_name(name) and name not in names:
            names.append(name)
    return names


def _task_ping_recipients(task, changes, participants, roster, actor, skip):
    task = task if isinstance(task, dict) else {}
    candidates = [task.get("createdBy")]
    if str(task.get("assigneeKind") or "user") == "user":
        candidates.append(task.get("assignee"))
    for change in changes or []:
        if isinstance(change, dict) and change.get("field") == "assignee":
            candidates.append(change.get("from"))
    candidates.extend(participants or [])
    candidates.extend(
        name for name, role in roster.items() if role == "owner")
    excluded = {
        clean_string(name or "", MAX_NODE_NAME).strip().lower()
        for name in (skip or [])
    }
    recipients = []
    for name in candidates:
        name = clean_string(name or "", MAX_NODE_NAME).strip().lower()
        if (
            not valid_node_name(name)
            or name == actor
            or name in excluded
            or name in recipients
            or name not in roster
        ):
            continue
        recipients.append(name)
    return recipients


async def notify_organization_task_update(
        env, org_bi, actor, task_id, task, action, changes=None,
        participants=None, skip=()):
    record = dict(task if isinstance(task, dict) else {})
    record.setdefault("id", str(task_id or ""))
    safe_actor = clean_string(actor or "", MAX_NODE_NAME).strip().lower()
    safe_action = clean_string(
        action or "updated", 32).strip().lower().replace("_", " ")
    org_row = await d1_first(
        env, "SELECT name FROM orgs WHERE org_bi=?", str(org_bi or ""))
    member_rows = await d1_all(
        env,
        "SELECT name,role FROM org_members WHERE org_bi=? LIMIT ?",
        str(org_bi or ""),
        1000,
    )
    roster = {}
    for row in member_rows or []:
        name = clean_string(
            row.get("name") or "", MAX_NODE_NAME).strip().lower()
        if valid_node_name(name):
            roster[name] = str(row.get("role") or "")
    if participants is None:
        participants = await _organization_task_participants(
            env, org_bi, task_id)
    recipients = _task_ping_recipients(
        record, changes, participants, roster, safe_actor, skip)
    org_name = (org_row or {}).get("name") or ""
    copy = organization_task_ping_copy(
        org_name, safe_actor, safe_action, record)
    filed = 0
    if recipients:
        now = int(Date.now())
        entries = world_task_pings.entries_for(
            safe_action, changes, safe_actor, now, str(task_id or ""),
            copy["meta"].get("taskTitle") or "")
        filed = await world_task_pings.deliver(
            _TaskPingStore(env), recipients, entries, copy["meta"], now)
        print("task ping: task %s action %s by %s to %s filed %d" % (
            str(task_id or "")[:8], safe_action, safe_actor or "-",
            ",".join(recipients), filed))
    if safe_action == "deleted":
        return filed
    try:
        emailed = await task_email.send_activity(
            env, str(org_bi or ""), org_name, safe_actor, task_id,
            safe_action, changes,
            task_email.recipients(
                _task_ping_recipients(
                    record, changes, participants, roster, safe_actor, ()),
                safe_actor, safe_action),
            copy["meta"], copy["body"])
        if emailed:
            print("task email: task %s action %s emailed %d" % (
                str(task_id or "")[:8], safe_action, emailed))
    except Exception as error:
        print("task email failed: %s" % type(error).__name__)
    return filed


def _task_deletion_plan(tasks, roster, actor):
    plan = []
    for task in tasks or []:
        if not isinstance(task, dict):
            continue
        names = [task.get("createdBy")]
        if str(task.get("assigneeKind") or "user") == "user":
            names.append(task.get("assignee"))
        for name in names:
            name = clean_string(name or "", MAX_NODE_NAME).strip().lower()
            if (
                not valid_node_name(name)
                or name == actor
                or name not in roster
            ):
                continue
            entry = next((item for item in plan if item[0] == name), None)
            if entry is None:
                entry = (name, [])
                plan.append(entry)
            if task not in entry[1]:
                entry[1].append(task)
    return plan


async def notify_organization_task_deletions(
        env, org_bi, actor, tasks, reason=""):
    safe_actor = clean_string(actor or "", MAX_NODE_NAME).strip().lower()
    org_row = await d1_first(
        env, "SELECT name FROM orgs WHERE org_bi=?", str(org_bi or ""))
    member_rows = await d1_all(
        env,
        "SELECT name,role FROM org_members WHERE org_bi=? LIMIT ?",
        str(org_bi or ""),
        1000,
    )
    roster = set()
    for row in member_rows or []:
        name = clean_string(
            row.get("name") or "", MAX_NODE_NAME).strip().lower()
        if valid_node_name(name):
            roster.add(name)
    plan = _task_deletion_plan(tasks, roster, safe_actor)
    if not plan:
        return 0
    try:
        emailed = await task_email.send_deleted(
            env, str(org_bi or ""), (org_row or {}).get("name") or "",
            safe_actor, plan, reason)
        print("task email: %d deleted task(s) by %s emailed %d" % (
            len(tasks or []), safe_actor or "-", emailed))
        return emailed
    except Exception as error:
        print("task delete email failed: %s" % type(error).__name__)
        return 0


class _OfficeMarketingTasksRuntime:
    """Least-privilege adapter for the configured organization's task board."""

    def __init__(self, env, request, bot_context=None):
        self.env = env
        self.request = request
        self.bot_context = (
            bot_context if isinstance(bot_context, dict) else None
        )

    def method(self):
        return method_name(self.request)

    def now(self):
        return int(Date.now())

    def new_id(self):
        return _ap_uuid()

    def _signed_query(self):
        """The node/ts/sig triple a desktop puts on a key-signed task write.

        Presence only — session() is what verifies it. Returning a triple here
        also means session() will refuse to fall back to cookie auth for this
        request, which is what makes relaxing the origin check below safe.
        """
        params = parse_qs(urlparse(self.request.url).query)
        node = clean_string(
            params.get("node", [""])[0], MAX_NODE_NAME).lower()
        ts = clean_string(params.get("ts", [""])[0], 20)
        sig = clean_string(params.get("sig", [""])[0], 200)
        return (node, ts, sig) if node and ts and sig else None

    def same_origin(self):
        # A credential the caller had to present explicitly is not ambient
        # browser authority and therefore is not vulnerable to cross-site
        # request forgery: an organization bot token, the account-session
        # bearer, or a key-signed desktop write. Each still has to validate in
        # session(). Browser sessions carry the session in a cookie, so they
        # retain the strict origin check.
        if self.bot_context or self._signed_query():
            return True
        header = str(
            self.request.headers.get("authorization") or ""
        ).strip()
        if header.lower().startswith("bearer "):
            presented = header[7:].strip()
            if presented and presented != "cookie":
                return True
        return _request_origin_allowed(self.env, self.request)

    def query(self, name):
        try:
            values = parse_qs(urlparse(str(self.request.url)).query).get(
                str(name), [""])
            return str(values[0] or "") if values else ""
        except Exception:
            return ""

    def response(self, data, status=200, cache_control=None,
                 extra_headers=None):
        return json_response(
            data,
            status=status,
            cache_control=cache_control,
            extra_headers=extra_headers,
        )

    def binary_response(self, data, mime, name):
        safe_name = re.sub(
            r"[^A-Za-z0-9._ -]+", "_", str(name or "task-image"))[:120]
        return JsResponse.new(Uint8Array.new(_to_js(bytes(data))), to_js({
            "status": 200,
            "headers": {
                "content-type": str(mime),
                "content-disposition": (
                    'inline; filename="' + safe_name.replace('"', "_") + '"'
                ),
                "cache-control": "private, no-store, max-age=0",
                "content-security-policy": "default-src 'none'; sandbox",
                "x-content-type-options": "nosniff",
            },
        }))

    async def ensure_schema(self):
        await ensure_schema(self.env)

    async def json_body(self, limit):
        try:
            raw_length = self.request.headers.get("content-length") or ""
            if raw_length and int(raw_length) > int(limit):
                return None, "payload_too_large"
        except (TypeError, ValueError):
            return None, "invalid_content_length"
        try:
            raw = await self.request.text()
        except Exception:
            return None, "invalid_json"
        if len(str(raw or "").encode("utf-8")) > int(limit):
            return None, "payload_too_large"
        if not str(raw or "").strip():
            return {}, ""
        try:
            data = json.loads(raw)
        except Exception:
            return None, "invalid_json"
        return (data, "") if isinstance(data, dict) else (None, "invalid_json")

    async def session(self, data=None):
        if self.bot_context:
            provider = clean_string(
                self.bot_context.get("provider"), 40
            ).strip().lower()
            return self.bot_context["orgBi"], {
                "name": (provider or "organization") + "-bot",
                "status": "active",
            }
        # A desktop that authenticated silently holds its account's Ed25519 key
        # and no session token at all, so a prompt could never open its task
        # without this. Once a triple is on the URL it is the only credential
        # considered: never fall through to the cookie, or a cross-site POST
        # could borrow one by appending a junk signature.
        if self._signed_query():
            return await _org_task_signed_session(
                self.env, self.request,
                data if isinstance(data, dict) else None)
        return await _account_session_record(
            self.env,
            self.request,
            data if isinstance(data, dict) else {},
        )

    async def organization(self):
        if self.bot_context:
            return await _org_row(
                self.env, str(self.bot_context.get("org") or "")
            )
        configured = str(
            getattr(self.env, "OFFICE_MARKETING_ORG", "")
            or getattr(self.env, "INSTANCE_OWNER", "") or "forkmesh"
        ).strip().lower()
        if not valid_node_name(configured):
            return "", None
        return await _org_row(self.env, configured)

    async def membership(self, org_bi, account):
        if self.bot_context:
            if str(org_bi or "") != str(self.bot_context.get("orgBi") or ""):
                return "", ""
            scopes = set(self.bot_context.get("scopes") or [])
            if "organization.tasks.write" in scopes:
                return "admin", "maintain"
            if "organization.tasks.read" in scopes:
                return "member", "read"
            return "", ""
        role = await _org_role(self.env, org_bi, account)
        if not role:
            return "", ""
        return role, await _org_permission(self.env, org_bi, account)

    async def delegated_principal(self, org_bi):
        if not self.bot_context:
            return None
        creator_bi = str(self.bot_context.get("createdByBi") or "")
        if (
            not creator_bi
            or str(org_bi or "") != str(self.bot_context.get("orgBi") or "")
        ):
            return {"accountBi": "", "role": "", "permission": ""}
        row = await d1_first(
            self.env,
            "SELECT name FROM org_members WHERE org_bi=? AND member_bi=?",
            str(org_bi or ""),
            creator_bi,
        )
        member = await self.active_user((row or {}).get("name"))
        if not member or member["bi"] != creator_bi:
            return {"accountBi": "", "role": "", "permission": ""}
        role = await _org_role(self.env, org_bi, member["name"])
        if not role:
            return {"accountBi": "", "role": "", "permission": ""}
        return {
            "accountBi": creator_bi,
            "role": role,
            "permission": await _org_permission(
                self.env, org_bi, member["name"]),
        }

    async def agent_session_running(self, org_bi, session_id):
        session_id = clean_string(session_id, 64).strip().lower()
        if not re.fullmatch(r"[a-f0-9-]{16,64}", session_id):
            return False
        row = await d1_first(
            self.env,
            "SELECT status FROM org_agent_sessions "
            "WHERE session_id=? AND org_bi=?",
            session_id,
            str(org_bi or ""),
        )
        return str((row or {}).get("status") or "") in (
            "security_pending", "queued", "running")

    def request_context(self):
        signed = self._signed_query()
        try:
            headers = self.request.headers
            user_agent = str(headers.get("user-agent") or "")
            agent = str(headers.get("x-forkmesh-agent") or "")
        except Exception:
            user_agent = agent = ""
        provider = clean_string(
            (self.bot_context or {}).get("provider"), 40).strip().lower()
        return {
            "via": (
                "bot-token" if self.bot_context
                else "signed-key" if signed
                else "session"
            ),
            "node": signed[0] if signed and not self.bot_context else "",
            "bot": (provider or "organization") if self.bot_context else "",
            "agent": " ".join(clean_string(agent, 80).split()),
            "ip": _account_session_client_ip(self.request),
            "userAgent": " ".join(clean_string(user_agent, 200).split()),
            "client": _generalized_client_category(self.request),
        }

    async def active_user(self, name):
        name = clean_string(name or "", MAX_NODE_NAME).strip().lower()
        if not valid_node_name(name):
            return None
        account_bi, record = await _account_row(self.env, name)
        if (
            not record
            or record.get("status") != "active"
            or _account_kind(record) != "user"
        ):
            return None
        return {
            "bi": str(account_bi or ""),
            "name": clean_string(
                record.get("name") or name, MAX_NODE_NAME
            ).strip().lower(),
        }

    async def eligible_users(self):
        rows = await d1_all(
            self.env,
            "SELECT user_bi,data FROM users "
            "ORDER BY username COLLATE NOCASE LIMIT 1000",
        )
        members = []
        for row in rows or []:
            try:
                record = await decrypt_row(
                    self.env, row.get("data"))
            except Exception:
                record = None
            name = clean_string(
                (record or {}).get("name") or "", MAX_NODE_NAME
            ).strip().lower()
            if (
                record
                and record.get("status") == "active"
                and _account_kind(record) == "user"
                and valid_node_name(name)
                and name not in members
            ):
                members.append(name)
        return members

    async def organization_members(self, org_bi):
        # One roster read per request, not per caller. Resolving it decrypts a
        # row per member, and a comment naming ten people used to pay for that
        # scan ten times over.
        cache = getattr(self, "_members_cache", None)
        if cache is None:
            cache = {}
            self._members_cache = cache
        key = str(org_bi or "")
        if key in cache:
            return cache[key]
        rows = await d1_all(
            self.env,
            "SELECT name FROM org_members WHERE org_bi=? "
            "ORDER BY name COLLATE NOCASE LIMIT 1000",
            key,
        )
        members = []
        for row in rows or []:
            member = await self.active_user(row.get("name"))
            if member and member["name"] not in {
                    item["name"] for item in members}:
                members.append(member)
        cache[key] = members
        return members

    async def organization_member(self, org_bi, name):
        requested = clean_string(
            name or "", MAX_NODE_NAME).strip().lower()
        if not requested:
            return None
        for member in await self.organization_members(org_bi):
            if member["name"] == requested:
                return member
        return None

    async def organization_teams(self, org_bi):
        rows = await d1_all(
            self.env,
            "SELECT team FROM org_teams WHERE org_bi=? "
            "ORDER BY team COLLATE NOCASE LIMIT 200",
            str(org_bi or ""),
        )
        return [
            clean_string(row.get("team"), MAX_NODE_NAME).strip().lower()
            for row in rows or []
            if valid_node_name(clean_string(
                row.get("team"), MAX_NODE_NAME).strip().lower())
        ]

    async def team_member(self, org_bi, team, name):
        member = await self.organization_member(org_bi, name)
        if not member:
            return False
        row = await d1_first(
            self.env,
            "SELECT 1 AS one FROM org_team_members "
            "WHERE org_bi=? AND team=? AND member_bi=?",
            str(org_bi or ""),
            clean_string(team, MAX_NODE_NAME).strip().lower(),
            member["bi"],
        )
        return bool(row)

    async def notify_organization_task_activity(
            self, org_bi, actor, task_id, task, action, changes=None,
            participants=None, skip=()):
        return await notify_organization_task_update(
            self.env, org_bi, actor, task_id, task, action,
            changes=changes, participants=participants, skip=skip)

    async def notify_organization_task_deletions(
            self, org_bi, actor, tasks, reason=""):
        return await notify_organization_task_deletions(
            self.env, org_bi, actor, tasks, reason)

    async def task_email_viewed(self, org_bi, task_id, token, viewer, now):
        return await task_email.viewed(
            self.env, org_bi, task_id, token, viewer, now)

    async def task_participants(self, org_bi, task_id):
        return await _organization_task_participants(
            self.env, org_bi, task_id)

    async def notify_engineering_task_started(
            self, org_bi, actor, task_id, task):
        """Compatibility wrapper for older task module deployments."""
        return await self.notify_organization_task_activity(
            org_bi, actor, task_id, task, "started")

    async def notify_organization_task_reminder(
            self, org_bi, actor, task_id, task, recipient, note, ts=0):
        safe_actor = clean_string(
            actor or "", MAX_NODE_NAME).strip().lower()
        recipient = clean_string(
            recipient or "", MAX_NODE_NAME).strip().lower()
        if (
            not valid_node_name(recipient)
            or recipient == safe_actor
            or not await self.organization_member(org_bi, recipient)
        ):
            return False
        org_row = await d1_first(
            self.env, "SELECT name FROM orgs WHERE org_bi=?", str(org_bi or ""))
        record = dict(task if isinstance(task, dict) else {})
        record.setdefault("id", str(task_id or ""))
        copy = organization_task_reminder_copy(
            (org_row or {}).get("name") or "", safe_actor, record, note)
        receipt = {}
        filed = await enqueue_notification(
            self.env,
            recipient,
            "mention",
            copy["title"],
            body=copy["body"],
            actor=safe_actor if valid_node_name(safe_actor) else "",
            href=world_task_pings.TASKS_HREF + "?task=" + str(task_id or ""),
            source=str(task_id or ""),
            dedupe="organization-task-reminder:%s:%s" % (
                str(task_id or ""), str(int(ts or 0))),
            meta=copy["meta"],
            force=True,
            receipt=receipt,
        )
        if not filed:
            return False
        return {
            "pingId": str(receipt.get("id") or ""),
            "phones": int(receipt.get("phones") or 0),
        }

    async def notify_organization_task_persistent_reminder(
            self, org_bi, task_id, task, recipient, every_minutes, ts=0):
        recipient = clean_string(
            recipient or "", MAX_NODE_NAME).strip().lower()
        if (
            not valid_node_name(recipient)
            or not await self.organization_member(org_bi, recipient)
        ):
            return False
        org_row = await d1_first(
            self.env, "SELECT name FROM orgs WHERE org_bi=?", str(org_bi or ""))
        record = dict(task if isinstance(task, dict) else {})
        record.setdefault("id", str(task_id or ""))
        copy = organization_task_persistent_reminder_copy(
            (org_row or {}).get("name") or "", record, every_minutes)
        return bool(await enqueue_notification(
            self.env,
            recipient,
            "mention",
            copy["title"],
            body=copy["body"],
            href=world_task_pings.TASKS_HREF + "?task=" + str(task_id or ""),
            source=str(task_id or ""),
            dedupe="organization-task-persistent-reminder:%s:%s" % (
                str(task_id or ""), str(int(ts or 0))),
            meta=copy["meta"],
            force=True,
        ))

    async def reminder_ping_read_at(self, recipient, ping_id):
        recipient = clean_string(
            recipient or "", MAX_NODE_NAME).strip().lower()
        ping_id = clean_string(ping_id or "", 160).strip().lower()
        if not valid_node_name(recipient) or not ping_id:
            return 0
        row = await d1_first(
            self.env,
            "SELECT read_at FROM notifications "
            "WHERE dedupe_bi=? AND recipient_bi=?",
            ping_id,
            await blind_index(self.env, recipient),
        )
        try:
            return max(0, int((row or {}).get("read_at") or 0))
        except (TypeError, ValueError):
            return 0

    async def notify_organization_task_mentions(
            self, org_bi, actor, task_id, task, names, context, excerpt,
            ts=0):
        """Ping exactly the organization members a task comment named.

        Deliberately independent of the floor-team fan-out: being addressed by
        name is what makes this ping wanted, so it reaches a member who is on
        no team the task routes through — and it reaches nobody who is not a
        current member of this organization.
        """
        safe_actor = clean_string(
            actor or "", MAX_NODE_NAME).strip().lower()
        org_row = await d1_first(
            self.env, "SELECT name FROM orgs WHERE org_bi=?", str(org_bi or ""))
        record = dict(task if isinstance(task, dict) else {})
        record.setdefault("id", str(task_id or ""))
        copy = organization_task_mention_copy(
            (org_row or {}).get("name") or "",
            safe_actor,
            context,
            record,
            excerpt,
        )
        pinged = set()
        for name in list(names or [])[:10]:
            recipient = clean_string(
                name or "", MAX_NODE_NAME).strip().lower()
            if (
                not valid_node_name(recipient)
                or recipient in pinged
                or recipient == safe_actor
            ):
                continue
            if not await self.organization_member(org_bi, recipient):
                continue
            pinged.add(recipient)
            # enqueue_notification rings the account's live sockets (web and
            # desktop) and its APNs devices, so a mention reaches a phone that
            # is asleep as well as an open tab.
            await enqueue_notification(
                self.env,
                recipient,
                "mention",
                copy["title"],
                body=copy["body"],
                actor=safe_actor if valid_node_name(safe_actor) else "",
                href="/world/",
                source=str(task_id or ""),
                # The event's own timestamp, not the task's updatedAt: a
                # comment does not touch the task row, so deduping on updatedAt
                # would silently collapse repeat mentions.
                dedupe="organization-task-mention:%s:%s:%s" % (
                    str(task_id or ""),
                    clean_string(context or "", 32).strip().lower(),
                    str(int(ts or 0)),
                ),
                meta=copy["meta"],
            )

    async def marketing_members(self, org_bi):
        """Return active users on an authoritative Marketing floor team."""
        rows = await d1_all(
            self.env,
            "SELECT DISTINCT tm.member_bi AS member_bi,tm.name AS name,"
            "tm.team AS team "
            "FROM org_team_members tm "
            "INNER JOIN org_members om "
            "ON om.org_bi=tm.org_bi AND om.member_bi=tm.member_bi "
            "INNER JOIN org_teams ot "
            "ON ot.org_bi=tm.org_bi AND ot.team=tm.team "
            "WHERE tm.org_bi=? "
            "AND om.role IN ('owner','admin','maintainer','member','auditor') "
            "AND ot.permission IN ('read','write','maintain','admin') "
            "ORDER BY tm.name COLLATE NOCASE LIMIT 1000",
            str(org_bi or ""),
        )
        aliases = OFFICE_FLOOR_TEAM_ALIASES["marketing"]
        members = []
        seen = set()
        for row in rows or []:
            if _office_team_slug(row.get("team")) not in aliases:
                continue
            name = clean_string(
                row.get("name") or "", MAX_NODE_NAME
            ).strip().lower()
            member = await self.active_user(name)
            if not member or name in seen:
                continue
            seen.add(name)
            members.append(member)
        return members

    async def marketing_member(self, org_bi, name):
        requested = clean_string(
            name or "", MAX_NODE_NAME
        ).strip().lower()
        if not requested:
            return None
        for member in await self.marketing_members(org_bi):
            if str(member.get("name") or "").lower() == requested:
                return member
        return None

    async def marketing_attendance(self, members, now):
        """Aggregate the last seven UTC calendar days for Marketing desks."""
        day_ms = 24 * 60 * 60 * 1000
        now = max(0, int(now or 0))
        today = now // day_ms
        first_day = max(0, today - 6)
        start_ms = first_day * day_ms
        totals = {}
        valid_members = []
        for member in members or []:
            account_bi = str((member or {}).get("bi") or "")
            name = clean_string(
                (member or {}).get("name") or "", MAX_NODE_NAME
            ).strip().lower()
            if not account_bi or not name:
                continue
            valid_members.append(name)
            rows = await d1_all(
                self.env,
                "SELECT in_at,out_at,last_seen_at "
                "FROM world_office_attendance "
                "WHERE account_bi=? "
                "AND visit_scope='office' "
                "AND in_at<? "
                "AND COALESCE(out_at,NULLIF(last_seen_at,0),?)>=? "
                "ORDER BY in_at ASC LIMIT 500",
                account_bi,
                now + 1,
                now,
                start_ms,
            )
            for row in rows or []:
                entered = max(start_ms, int(row.get("in_at") or 0))
                exited = row.get("out_at")
                if exited is None:
                    exited = min(
                        now,
                        max(
                            entered,
                            int(row.get("last_seen_at") or now),
                        ),
                    )
                exited = min(now, max(entered, int(exited or entered)))
                cursor = entered
                while cursor < exited:
                    bucket = cursor // day_ms
                    boundary = (bucket + 1) * day_ms
                    portion = min(exited, boundary) - cursor
                    totals[(name, bucket)] = (
                        totals.get((name, bucket), 0) + portion
                    )
                    cursor += portion
        days = []
        for bucket in range(first_day, today + 1):
            stamp = time.gmtime(bucket * 24 * 60 * 60)
            days.append({
                "date": time.strftime("%Y-%m-%d", stamp),
                "label": time.strftime("%a %m/%d", stamp).upper(),
                "hours": [
                    {
                        "member": name,
                        "hours": round(
                            totals.get((name, bucket), 0) / 3600000, 2),
                    }
                    for name in valid_members
                ],
            })
        return days

    async def blind(self, value):
        return await blind_index(self.env, str(value or ""))

    async def seal(self, value):
        return await encrypt_row(self.env, value)

    async def open(self, value):
        return await decrypt_row(self.env, value)

    async def audit(self, actor, action, target_type="", target="",
                    outcome="success", details=None):
        await _audit_sensitive_action(
            self.env,
            actor,
            action,
            target_type,
            target,
            outcome=outcome,
            details=details,
        )

    async def log_error(self, path, message):
        await log_error(self.env, 500, "POST", path, message,
                        request=self.request)

    async def d1_all(self, sql, *args):
        return await d1_all(self.env, sql, *args)

    async def d1_first(self, sql, *args):
        return await d1_first(self.env, sql, *args)

    async def d1_run(self, sql, *args):
        return await d1_run(self.env, sql, *args)

    async def cancel_agent_session(self, org_bi, session_id):
        """Retire the node-queued bot run behind a returned task."""

        session_id = clean_string(session_id, 64).strip().lower()
        if not re.fullmatch(r"[a-f0-9-]{16,64}", session_id):
            return False
        now = int(Date.now())
        await d1_run(
            self.env,
            "UPDATE org_agent_jobs SET status='cancelled',updated_at=? "
            "WHERE session_id=? AND org_bi=? AND status IN ('queued','leased')",
            now, session_id, str(org_bi or ""),
        )
        await d1_run(
            self.env,
            "UPDATE org_agent_sessions SET status='cancelled',updated_at=?,"
            "completed_at=? WHERE session_id=? AND org_bi=? "
            "AND status IN ('security_pending','queued','running')",
            now, now, session_id, str(org_bi or ""),
        )
        return True
