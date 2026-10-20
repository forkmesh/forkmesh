"""World HTTP routes, parsed only once a request selects one of them.

Cloudflare validates a Python Worker inside a fixed startup memory allowance
that pays for the source of every attached module. The World district, admin,
social, and element-store routes are a large slice of that source which no
other route family needs, so entry.py reaches them through a ``_LazyModule``
proxy and this module is imported on the first World request.

The routes keep using the entrypoint's sessions, D1 helpers, crypto, and
metadata-only audit trail; ``_bind_runtime`` supplies those primitives on first
use, exactly as the other extracted route modules do.
"""


def _bind_runtime(runtime):
    """Supply the initialized entrypoint primitives used by these routes."""
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


def world_context_handler(request):
    """Return the visitor's context and the shared world clock.

    Nothing here identifies the requester: the coarse country hint and the
    shared clock are all that leave the edge. Connection details belong to
    account session management (``/api/accounts/sessions``), not to a payload
    every guest receives.
    """
    if method_name(request) != "GET":
        return json_response(
            {"error": "method_not_allowed"},
            status=405,
            cache_control="no-store, max-age=0, must-revalidate",
            extra_headers={"allow": "GET"},
        )
    payload = world_protocol.context_payload(
        world_request_country(request), int(Date.now()), MAX_CONNECTIONS)
    return json_response(
        payload,
        cache_control="no-store, max-age=0, must-revalidate",
        extra_headers={"x-content-type-options": "nosniff"},
    )


async def world_durable_object_request(
        request, trusted_claim=None, moderation_tokens=None, mesh_home="",
        watch=False):
    """Rebuild the world upgrade with tightly scoped connection headers.

    The general tunnel helper forwards Authorization and User-Agent because Git
    hosts need them. Public World presence does not. The live room receives
    WebSocket handshake headers, the approximate country code, opaque
    moderation subjects, and an ephemeral edge IP/raw agent pair that its
    server-verified admin projection may reveal for guest moderation only.
    Those two values are never persisted or emitted to ordinary sockets.
    """
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
    country = world_request_country(request)
    if country:
        # Clients cannot spoof this internal header: the incoming request is
        # rebuilt from the explicit allowlist above.
        headers["x-forkmesh-country"] = country
    claim = trusted_claim if isinstance(trusted_claim, dict) else {}
    if claim:
        # This compact public claim is constructed by this Worker only after a
        # short-lived HMAC ticket has been verified. The incoming request's
        # similarly named headers are deliberately never copied.
        public_claim = world_protocol.trusted_presence_claim(
            claim.get("name", ""), claim.get("accountStatus", "Guest"),
            claim.get("nodeCount", 0))
        private_claim = {
            **public_claim,
            # This flag is verified from the signed, short-lived World ticket.
            # The Durable Object uses it only to tailor moderation handles to
            # an administrator's socket; it is not public presence metadata.
            "isAdmin": claim.get("isAdmin") is True,
            # One live use per signed ticket prevents a copied socket URL from
            # creating a second verified-looking avatar in this room.
            "ticketNonce": str(claim.get("ticketNonce") or ""),
            # Private blind account key used only to add server-counted steps
            # to the aggregate row. It is stripped from every presence frame.
            "accountBi": (
                str(claim.get("accountBi") or "")
                if re.fullmatch(
                    r"[a-f0-9]{64}", str(claim.get("accountBi") or ""))
                else ""),
        }
        encoded_claim = base64.urlsafe_b64encode(
            json.dumps(private_claim, separators=(",", ":")).encode()
        ).decode().rstrip("=")
        headers["x-forkmesh-world-claim"] = encoded_claim
    tokens = moderation_tokens if isinstance(moderation_tokens, dict) else {}
    for target_type in ("ip", "agent"):
        token = str(tokens.get(target_type) or "")
        if re.fullmatch(r"[a-f0-9]{64}", token):
            # Opaque tokens are the only values used by the persistent manual
            # block plane. Raw connection detail crosses separately below only
            # into ephemeral socket memory for the verified-admin guest view.
            headers["x-forkmesh-world-%s-token" % target_type] = token
    home = str(mesh_home or "")
    if world_mesh.HOST_RE.fullmatch(home):
        headers["x-forkmesh-world-home"] = home
        if watch:
            headers["x-forkmesh-world-watch"] = "1"
    client_ip = _account_session_client_ip(request)
    if client_ip:
        headers["x-forkmesh-world-private-ip"] = client_ip
    try:
        raw_agent = clean_string(
            request.headers.get("user-agent") or "", 1024)
    except Exception:
        raw_agent = ""
    if raw_agent:
        headers["x-forkmesh-world-private-agent"] = raw_agent
    source_url = urlparse(request.url)
    # Strip query/fragment data as an additional privacy boundary. The world
    # protocol has no tokens or user-selected URL state.
    target_url = (
        source_url.scheme + "://" + source_url.netloc + source_url.path)
    return JsRequest.new(
        target_url,
        to_js({"method": "GET", "headers": headers}),
    )


def _world_ticket_signature(env, payload):
    return hmac.new(
        _account_session_secret(env),
        b"forkmesh-world-ticket-v1\n" + payload.encode(),
        "sha256",
    ).hexdigest()


def _world_ticket_encode(env, claim):
    payload = base64.urlsafe_b64encode(
        json.dumps(claim, separators=(",", ":"), sort_keys=True).encode()
    ).decode().rstrip("=")
    return payload + "." + _world_ticket_signature(env, payload)


def _world_ticket_decode(env, ticket):
    value = clean_string(ticket or "", 2048).strip()
    if "." not in value:
        return None
    payload, signature = value.rsplit(".", 1)
    if not payload or not re.fullmatch(r"[A-Za-z0-9_-]+", payload):
        return None
    if not hmac.compare_digest(
            signature, _world_ticket_signature(env, payload)):
        return None
    try:
        padded = payload + "=" * ((4 - len(payload) % 4) % 4)
        claim = json.loads(base64.urlsafe_b64decode(padded).decode())
    except Exception:
        return None
    if not isinstance(claim, dict):
        return None
    now = int(Date.now())
    try:
        issued = int(claim.get("issuedAt", 0) or 0)
        expires = int(claim.get("expiresAt", 0) or 0)
    except (TypeError, ValueError):
        return None
    if issued <= 0 or expires <= now or expires - issued > WORLD_TICKET_TTL_MS:
        return None
    if issued > now + 5000:
        return None
    public = world_protocol.trusted_presence_claim(
        claim.get("name", ""), claim.get("accountStatus", "Guest"),
        claim.get("nodeCount", 0))
    if not public["name"] or public["accountStatus"] == "Guest":
        return None
    # `isAdmin` is accepted only from the HMAC-authenticated ticket body. It is
    # never accepted from a browser presence frame.
    public["isAdmin"] = claim.get("isAdmin") is True
    nonce = str(claim.get("nonce") or "")
    if not re.fullmatch(r"[A-Za-z0-9_-]{8,32}", nonce):
        return None
    public["ticketNonce"] = nonce
    return public


def _world_activity_continuation_claim(
        env, request, expected_name, now):
    """Validate a single-use activity continuation carried by the browser.

    The continuation is the previous server-signed World ticket. The browser
    supplies no elapsed value: both endpoints of the interval and its
    generation were signed or observed by the Worker.
    """
    try:
        value = request.headers.get(WORLD_USER_ACTIVITY_HEADER) or ""
    except Exception:
        return None
    value = clean_string(value, 2048).strip()
    if "." not in value:
        return None
    payload, signature = value.rsplit(".", 1)
    if not payload or not re.fullmatch(r"[A-Za-z0-9_-]+", payload):
        return None
    if not hmac.compare_digest(
            signature, _world_ticket_signature(env, payload)):
        return None
    try:
        padded = payload + "=" * ((4 - len(payload) % 4) % 4)
        claim = json.loads(base64.urlsafe_b64decode(padded).decode())
        issued = int(claim.get("issuedAt", 0) or 0)
        generation = int(claim.get("activityGeneration", 0) or 0)
    except (AttributeError, TypeError, ValueError, UnicodeError):
        return None
    if not isinstance(claim, dict):
        return None
    expected = clean_string(expected_name, MAX_NODE_NAME).strip().lower()
    ticket_name = clean_string(
        claim.get("name", ""), MAX_NODE_NAME).strip().lower()
    if not expected or ticket_name != expected:
        return None
    if (
        issued <= 0
        or issued > now
        or now - issued > WORLD_USER_ACTIVITY_CONTINUATION_MAX_MS
        or generation <= 0
        or generation > WORLD_USER_ACTIVITY_MAX_GENERATION
    ):
        return None
    return {"issuedAt": issued, "generation": generation}


async def _world_user_activity_touch(
        env, account_bi, now, continuation=None):
    """Advance one aggregate with a race-safe, signed continuity proof."""
    try:
        continued_from = int(
            (continuation or {}).get("issuedAt", 0) or 0)
        generation = int(
            (continuation or {}).get("generation", 0) or 0)
    except (AttributeError, TypeError, ValueError):
        continued_from = 0
        generation = 0
    candidate_credit = now - continued_from
    if (
        continued_from <= 0
        or generation <= 0
        or candidate_credit <= 0
        or candidate_credit > WORLD_USER_ACTIVITY_CONTINUATION_MAX_MS
    ):
        continued_from = 0
        generation = 0
        candidate_credit = 0
    row = await d1_first(
        env,
        "INSERT INTO world_user_activity "
        "(account_bi,total_active_ms,last_touch_at,generation,"
        "last_credit_ms,updated_at) VALUES (?,0,?,1,0,?) "
        "ON CONFLICT(account_bi) DO UPDATE SET "
        "total_active_ms=MIN(?,world_user_activity.total_active_ms+"
        "CASE WHEN ?>0 AND ?=world_user_activity.generation "
        "AND ?=world_user_activity.last_touch_at THEN ? ELSE 0 END),"
        "last_credit_ms=CASE WHEN ?>0 "
        "AND ?=world_user_activity.generation "
        "AND ?=world_user_activity.last_touch_at THEN ? ELSE 0 END,"
        "last_touch_at=excluded.last_touch_at,"
        "generation=CASE WHEN world_user_activity.generation>=? "
        "THEN 1 ELSE world_user_activity.generation+1 END,"
        "updated_at=excluded.updated_at "
        "RETURNING total_active_ms,total_steps,last_touch_at,generation,"
        "last_credit_ms",
        account_bi, now, now,
        WORLD_USER_ACTIVITY_MAX_TOTAL_MS,
        candidate_credit, generation, continued_from, candidate_credit,
        candidate_credit, generation, continued_from, candidate_credit,
        WORLD_USER_ACTIVITY_MAX_GENERATION,
    )
    try:
        total_active_ms = int((row or {}).get("total_active_ms", 0) or 0)
        total_steps = int((row or {}).get("total_steps", 0) or 0)
        next_generation = int((row or {}).get("generation", 0) or 0)
        credited_ms = int((row or {}).get("last_credit_ms", 0) or 0)
    except (AttributeError, TypeError, ValueError):
        total_active_ms = 0
        total_steps = 0
        next_generation = 0
        credited_ms = 0
    return {
        "totalActiveMs": max(
            0, min(WORLD_USER_ACTIVITY_MAX_TOTAL_MS, total_active_ms)),
        "totalSteps": max(0, min(WORLD_USER_STEP_MAX_TOTAL, total_steps)),
        "generation": max(
            0, min(WORLD_USER_ACTIVITY_MAX_GENERATION, next_generation)),
        "creditedMs": max(
            0, min(WORLD_USER_ACTIVITY_CONTINUATION_MAX_MS, credited_ms)),
        "observedAt": now,
    }


async def _world_account_claim(env, request, data=None):
    """Return a server-derived public world role for a valid user session."""
    account_bi, rec = await _account_session_record(env, request, data)
    if not rec:
        return None
    name = clean_string(rec.get("name", ""), MAX_NODE_NAME).strip().lower()
    if not name:
        return None
    roles = set()
    try:
        now = int(Date.now())
        rows = await d1_all(
            env,
            "SELECT role FROM role_grants WHERE account_bi=? "
            "AND role IN ('supporting_member','mirror_operator',"
            "'organization_administrator','automated_agent') "
            "AND revoked_at=0 AND (expires_at=0 OR expires_at>?)",
            account_bi, now,
        )
        roles = {
            str(row.get("role") or "") for row in (rows or [])
        }
    except Exception:
        roles = set()
    try:
        org_admin = await d1_first(
            env,
            "SELECT 1 AS ok FROM org_members WHERE member_bi=? "
            "AND role IN ('owner','admin') LIMIT 1",
            account_bi,
        )
    except Exception:
        org_admin = None
    node_count = len(_owned_nodes(rec))
    if "automated_agent" in roles or (
            _account_kind(rec) in ("bot", "agent")
            and bool(rec.get("verified"))):
        account_status = "Verified bot"
    elif org_admin or "organization_administrator" in roles:
        account_status = "Organization admin"
    elif node_count or "mirror_operator" in roles:
        account_status = "Mirror operator"
    elif (
        "supporting_member" in roles
        or bool(rec.get("supporting_member"))
        or str(rec.get("account_tier") or "").lower()
            in ("supporter", "supporting", "paid")
    ):
        account_status = "Supporting member"
    elif bool(rec.get("email_verified")):
        account_status = "Registered"
    else:
        # An active but unverified account receives no elevated public badge.
        account_status = "Guest"
    if account_status == "Guest":
        return None
    claim = world_protocol.trusted_presence_claim(
        name, account_status, node_count)
    claim["isAdmin"] = bool(
        await _has_role(env, name, "platform_administrator"))
    # The account creation timestamp is already public on /api/accounts/{name};
    # carrying it on the ticket lets the world badge render "joined … ago"
    # without every avatar firing its own profile lookup.
    try:
        joined_at = int(rec.get("created_at") or 0)
    except (TypeError, ValueError):
        joined_at = 0
    claim["joinedAt"] = max(0, joined_at)
    return claim


async def _world_system_capacity(env):
    """Delegate the bounded table inventory to its infrastructure module.

    The inventory COUNTs every table, which bills a read for every row in the
    database, so admin tickets share one colo-wide copy and a conserving read
    guard serves an empty inventory rather than recounting.
    """
    inventory = _world_infrastructure_module().d1_table_inventory
    shared = globals().get("d1_shared_read")
    if shared is None:
        return await inventory(env, d1_all, d1_first)
    return await shared(
        env, "table-inventory", lambda: inventory(env, d1_all, d1_first),
        fallback=[])


async def _world_d1_storage(env):
    """Return current D1 bytes alongside documented Free/Paid limits."""
    return await _world_infrastructure_module().d1_storage_usage(env)


async def world_ticket_handler(env, request):
    if method_name(request) != "GET":
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            cache_control="no-store, max-age=0, must-revalidate",
            extra_headers={"allow": "GET"})
    claim = await _world_account_claim(env, request)
    if not claim:
        return json_response(
            {
                "ok": True,
                "authenticated": False,
                "accountStatus": "Guest",
                "nodeCount": 0,
                "ticket": "",
            },
            cache_control="no-store, max-age=0, must-revalidate",
        )
    now = int(Date.now())
    account_bi = ""
    try:
        await ensure_schema(env)
        account_bi = await blind_index(env, claim["name"])
    except Exception:
        # Tickets remain available during a rolling D1 migration. Activity is
        # conservatively uncounted until its aggregate table is ready.
        account_bi = ""

    # If this account previously opted into the inactive seating area, a fresh
    # authenticated world visit makes that card ineligible immediately. Keep
    # the consent/configuration record so it can reappear after the account has
    # actually been away; do not require the user to opt in on every visit.
    try:
        if not account_bi:
            raise RuntimeError("world_activity_schema_unavailable")
        await d1_run(
            env,
            "UPDATE world_inactive_presence SET updated_at=?, expires_at=? "
            "WHERE account_bi=?",
            now, now + WORLD_INACTIVE_RETAIN_MS, account_bi,
        )
    except Exception:
        # Presence tickets must still work if a rolling deployment has not yet
        # installed the optional inactivity table.
        pass
    activity = {
        "totalActiveMs": 0,
        "totalSteps": 0,
        "generation": 0,
        "creditedMs": 0,
        "observedAt": now,
    }
    try:
        if not account_bi:
            raise RuntimeError("world_activity_schema_unavailable")
        continuation = _world_activity_continuation_claim(
            env, request, claim["name"], now)
        activity = await _world_user_activity_touch(
            env, account_bi, now, continuation)
        if activity["creditedMs"] > 0:
            await edge_cache_delete(USERS_DIRECTORY_CACHE_KEY)
        # "World Explorer" badge: _award_badge is an idempotent INSERT OR
        # IGNORE, so re-checking this on every ticket past the threshold is
        # harmless — no need to detect the exact crossing tick.
        if activity["totalActiveMs"] >= WORLD_FIRST_HOUR_BADGE_MS:
            await _award_badge(env, claim["name"], account_bi,
                               "world_first_hour")
    except Exception:
        # Aggregate activity is optional telemetry. Authentication and
        # multiplayer entry must continue while D1 is unavailable or migrating.
        pass
    ticket_claim = {
        **claim,
        "issuedAt": now,
        "expiresAt": now + WORLD_TICKET_TTL_MS,
        "activityGeneration": activity["generation"],
        "nonce": new_world_peer_id(),
    }
    response_data = {
        "ok": True,
        "authenticated": True,
        **claim,
        "expiresAt": ticket_claim["expiresAt"],
        "ticket": _world_ticket_encode(env, ticket_claim),
        "totalActiveMs": activity["totalActiveMs"],
        "totalSteps": activity["totalSteps"],
        "activityObservedAt": activity["observedAt"],
    }
    # Platform-administrator status is derived server-side from the verified
    # account session. Omit the field entirely for every other caller so the
    # table inventory cannot be inferred from a guest or regular-user ticket.
    if claim.get("isAdmin") is True:
        try:
            response_data["systemCapacity"] = {
                "tables": await _world_system_capacity(env),
                "durableObjects": await _world_durable_objects(env),
                "d1Storage": await _world_d1_storage(env),
            }
        except Exception:
            # Capacity telemetry is optional; authentication and multiplayer
            # entry continue to work while D1 is unavailable or migrating.
            response_data["systemCapacity"] = {
                "tables": [],
                "durableObjects": [],
                "d1Storage": {},
            }
    return json_response(
        response_data,
        cache_control="no-store, max-age=0, must-revalidate",
        extra_headers={"x-content-type-options": "nosniff"},
    )


async def world_client_profile_handler(env, request):
    """Store the signed-in visitor's coarse country/browser/OS on the account.

    Every registered account owns a bench around the campfire whether or not
    it is signed in, and an away member publishes no presence frame — so
    without a stored copy their figure sat there with no flag and a hidden
    client badge. The client posts this once per change (a privacy toggle
    included), so an ordinary session costs no extra write.

    The country is read from the edge, never from the body: the client only
    says whether it may be shared. Browser and OS are the same coarse
    families live presence accepts; an unknown or ``hidden`` value stores as
    "" and simply clears the previous one.
    """
    if method_name(request) != "POST":
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            cache_control="no-store, max-age=0, must-revalidate",
            extra_headers={"allow": "POST"})
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    name_bi, rec = await _account_session_record(env, request, data)
    if not name_bi or not rec:
        return json_response(
            {"error": "invalid_session"}, status=401,
            cache_control="no-store, max-age=0, must-revalidate")
    profile = world_protocol.clean_client_profile(
        world_request_country(request) if data.get("shareCountry") is True
        else "",
        data.get("browser"),
        data.get("os"),
    )
    if profile != _account_world_client_fields(rec):
        rec["world_country"] = profile["countryCode"]
        rec["world_browser"] = profile["browser"]
        rec["world_os"] = profile["os"]
        rec["world_client_at"] = int(Date.now())
        await _save_account(env, name_bi, rec)
        # The bench figures are drawn from the edge-cached public directory.
        await _users_directory_cache_forget()
    return json_response(
        {"ok": True, **profile},
        cache_control="no-store, max-age=0, must-revalidate",
        extra_headers={"x-content-type-options": "nosniff"},
    )


def _world_inactive_public_id(account_bi):
    return hashlib.sha256(
        ("forkmesh-world-inactive-v1:" + str(account_bi or "")).encode()
    ).hexdigest()[:20]


# Coarse recency ladder for the avatar chest activity light, the wallet QR's
# transaction ring, and the Members Circle lifecycle bands. Each label reads
# "within this window"; anything past 120 days is "stale". These are buckets,
# never timestamps: the finest public step is a whole hour, far coarser than a
# last-seen instant.


def _world_activity_light_bucket(age_ms):
    try:
        age = max(0, int(age_ms))
    except (TypeError, ValueError):
        return "stale"
    for limit, bucket in WORLD_ACTIVITY_LIGHT_BUCKETS:
        if age < limit:
            return bucket
    return "stale"


async def _world_user_activity_buckets(env):
    """Map account_bi -> coarse chest-light recency bucket, all accounts.

    Kept as its own query, well apart from the public user-directory payload
    builder, so that function never sees (or could leak) a raw touch
    timestamp -- only the bucket label crosses that boundary.
    """
    now = int(Date.now())
    rows = await d1_all(
        env, "SELECT account_bi,last_touch_at FROM world_user_activity")
    buckets = {}
    for row in rows or []:
        try:
            touch = int(row.get("last_touch_at") or 0)
        except (TypeError, ValueError):
            touch = 0
        if touch <= 0:
            continue
        buckets[row.get("account_bi")] = _world_activity_light_bucket(
            now - touch)
    return buckets


def _world_inactive_recency(updated_at, now):
    try:
        age = max(0, int(now) - int(updated_at or 0))
    except (TypeError, ValueError):
        age = WORLD_INACTIVE_RETAIN_MS
    if age < 24 * 60 * 60 * 1000:
        return "Last active recently"
    if age < 7 * 24 * 60 * 60 * 1000:
        return "Away this week"
    return "Returning contributor"


async def world_inactive_handler(env, request):
    """Manage the user's optional, generalized Town Square seating card."""
    await ensure_schema(env)
    method = method_name(request)
    now = int(Date.now())
    await d1_run(
        env, "DELETE FROM world_inactive_presence WHERE expires_at<=?", now)
    if method == "GET":
        rows = await d1_all(
            env,
            "SELECT account_bi, data, updated_at FROM world_inactive_presence "
            "WHERE expires_at>? AND updated_at<=? "
            "ORDER BY updated_at DESC LIMIT 64",
            now, now - WORLD_INACTIVE_DELAY_MS,
        )
        people = []
        for row in rows or []:
            try:
                rec = await decrypt_row(env, row.get("data"))
            except Exception:
                rec = None
            if not isinstance(rec, dict):
                continue
            status = str(rec.get("status") or "")
            if status not in world_protocol.WORLD_INACTIVITY_VALUES:
                continue
            claim = world_protocol.trusted_presence_claim(
                rec.get("name", ""), rec.get("accountStatus", "Guest"),
                rec.get("nodeCount", 0))
            try:
                inactive_age = now - int(row.get("updated_at") or 0)
            except (TypeError, ValueError):
                inactive_age = None
            people.append({
                "id": _world_inactive_public_id(row.get("account_bi")),
                "name": claim["name"] or "Private contributor",
                "accountStatus": claim["accountStatus"],
                "nodeCount": claim["nodeCount"],
                "availability": status,
                "lastActive": _world_inactive_recency(
                    row.get("updated_at"), now),
                # Drives the seating card's chest activity light.
                "activityBucket": _world_activity_light_bucket(inactive_age),
            })
        return json_response(
            {
                "ok": True,
                "people": people,
                "retentionDays": 30,
                "visibilityDelayMinutes": (
                    WORLD_INACTIVE_DELAY_MS // (60 * 1000)),
            },
            cache_control="no-store, max-age=0, must-revalidate",
            extra_headers={"x-content-type-options": "nosniff"},
        )
    if method == "DELETE":
        account_bi, rec = await _account_session_record(env, request)
        if not rec:
            return json_response({"error": "invalid_session"}, status=401)
        await d1_run(
            env, "DELETE FROM world_inactive_presence WHERE account_bi=?",
            account_bi)
        return json_response(
            {"ok": True, "visible": False},
            cache_control="no-store, max-age=0, must-revalidate")
    if method != "POST":
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            extra_headers={"allow": "GET, POST, DELETE"})
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    account_bi, rec = await _account_session_record(env, request, data)
    if not rec:
        return json_response({"error": "invalid_session"}, status=401)
    selection = world_protocol.sanitize_inactivity_record(data)
    if selection is None:
        # A false/absent consent flag is an explicit removal, never a hidden
        # retained record.
        await d1_run(
            env, "DELETE FROM world_inactive_presence WHERE account_bi=?",
            account_bi)
        return json_response(
            {"ok": True, "visible": False},
            cache_control="no-store, max-age=0, must-revalidate")
    claim = await _world_account_claim(env, request, data)
    if not claim:
        return json_response({"error": "verified_account_required"}, status=403)
    public_rec = {
        "name": claim["name"] if selection["shareName"]
                else "Private contributor",
        "status": selection["status"],
        "accountStatus": claim["accountStatus"],
        "nodeCount": claim["nodeCount"] if selection["shareNodes"] else 0,
    }
    encrypted = await encrypt_row(env, public_rec)
    expires = now + WORLD_INACTIVE_RETAIN_MS
    await d1_run(
        env,
        "INSERT INTO world_inactive_presence "
        "(account_bi,data,updated_at,expires_at) VALUES (?,?,?,?) "
        "ON CONFLICT(account_bi) DO UPDATE SET "
        "data=excluded.data,updated_at=excluded.updated_at,"
        "expires_at=excluded.expires_at",
        account_bi, encrypted, now, expires,
    )
    return json_response(
        {
            "ok": True,
            "visible": False,
            "enrolled": True,
            "availability": selection["status"],
            "visibleAfter": now + WORLD_INACTIVE_DELAY_MS,
            "expiresAt": expires,
        },
        cache_control="no-store, max-age=0, must-revalidate",
    )


async def world_wallet_badge_handler(env, request):
    """Public data for an avatar's chest wallet QR chip.

    Read-only on-chain lookups for an address the account holder already chose
    to publish: the SOL balance plus the same coarse recency bucket the chest
    activity light uses, keyed to the wallet's newest transaction.
    """
    if method_name(request) != "GET":
        return json_response({"error": "method_not_allowed"}, status=405)
    params = parse_qs(urlparse(request.url).query)
    address = str(params.get("address", [""])[0] or "").strip()
    if not SOLANA_RE.match(address):
        return json_response({"error": "bad_address"}, status=400)
    cache_key = WORLD_WALLET_CACHE_PREFIX + address
    refresh = _wants_balance_refresh(request, scope="world-wallet:" + address)
    cached = None if refresh else await edge_cache_match(cache_key)
    if cached is not None:
        return cached
    lamports, checked_at = await _display_balance_lamports(
        env, address, force=refresh)
    tx_bucket = ""
    signatures = await _solana_rpc(
        env, "getSignaturesForAddress", [address, {"limit": 1}])
    result = signatures.get("result") if isinstance(signatures, dict) else None
    if isinstance(result, list):
        tx_bucket = "stale"
        newest = result[0] if result else None
        block_time = newest.get("blockTime") if isinstance(newest, dict) else None
        try:
            tx_bucket = _world_activity_light_bucket(
                int(Date.now()) - int(block_time) * 1000)
        except (TypeError, ValueError):
            tx_bucket = "stale"
    resp = json_response({
        "ok": True,
        "address": address,
        "lamports": lamports,
        "sol": (lamports / LAMPORTS_PER_SOL) if lamports is not None else None,
        "checkedAt": checked_at,
        "txBucket": tx_bucket,
    }, cache_seconds=WORLD_WALLET_BADGE_TTL)
    # Only a successful read is worth pinning for the whole TTL; a dead RPC
    # window should retry on the next request instead of caching the outage.
    if lamports is not None or tx_bucket:
        await edge_cache_put(cache_key, resp)
    return resp


async def _world_unique_register_sets(env, now):
    """Read six merged aggregate sketches in one bounded D1 query."""
    windows = world_visitor_metrics.unique_visit_window_bounds(now)
    today_start, _ = windows["today"]
    yesterday_start, yesterday_end = windows["yesterday"]
    same_start, same_end = windows["yesterdaySameTime"]
    hour_start, _ = windows["pastHour"]
    previous_hour_start, previous_hour_end = windows["pastHourYesterday"]
    cutoff = int(now) - WORLD_VISIT_RETAIN_MS

    # Old temporal rows have no identity field and can be deleted outright.
    # The all-time sketch at bucket -1 is fixed at 1,024 possible rows.
    await d1_run(
        env,
        "DELETE FROM world_visit_unique_hll "
        "WHERE bucket_start>=0 AND bucket_start<?",
        cutoff,
    )
    rows = await d1_all(
        env,
        "SELECT register_id,"
        "MAX(CASE WHEN bucket_start=? THEN rank END) AS total_rank,"
        "MAX(CASE WHEN bucket_start>=? THEN rank END) AS today_rank,"
        "MAX(CASE WHEN bucket_start>=? AND bucket_start<? "
        "THEN rank END) AS yesterday_rank,"
        "MAX(CASE WHEN bucket_start>=? AND bucket_start<? "
        "THEN rank END) AS yesterday_same_time_rank,"
        "MAX(CASE WHEN bucket_start>=? THEN rank END) AS past_hour_rank,"
        "MAX(CASE WHEN bucket_start>=? AND bucket_start<? "
        "THEN rank END) AS past_hour_yesterday_rank "
        "FROM world_visit_unique_hll "
        "WHERE bucket_start=? OR bucket_start>=? "
        "GROUP BY register_id",
        world_visitor_metrics.HLL_ALL_TIME_BUCKET,
        today_start,
        yesterday_start, yesterday_end,
        same_start, same_end,
        hour_start,
        previous_hour_start, previous_hour_end,
        world_visitor_metrics.HLL_ALL_TIME_BUCKET, cutoff,
    )
    columns = {
        "total": "total_rank",
        "today": "today_rank",
        "yesterday": "yesterday_rank",
        "yesterdaySameTime": "yesterday_same_time_rank",
        "pastHour": "past_hour_rank",
        "pastHourYesterday": "past_hour_yesterday_rank",
    }
    register_sets = {key: [] for key in columns}
    for row in rows or []:
        try:
            register_id = int(row.get("register_id"))
        except (AttributeError, TypeError, ValueError):
            continue
        for key, column in columns.items():
            try:
                rank = int(row.get(column))
            except (TypeError, ValueError):
                continue
            if rank > 0:
                register_sets[key].append({
                    "register_id": register_id,
                    "rank": rank,
                })
    return register_sets


async def world_visitors_handler(env, request):
    """Public approximate unique counts for the Arrival Grid plaque."""
    if method_name(request) == "POST":
        counted = await record_world_visit(env, request)
        if counted:
            await edge_cache_delete(WORLD_VISITORS_CACHE_KEY)
        return json_response({"ok": True}, cache_control="no-store")
    if method_name(request) != "GET":
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            extra_headers={"allow": "GET, POST"})
    cached = await edge_cache_match(WORLD_VISITORS_CACHE_KEY)
    if cached is not None:
        return cached
    await ensure_schema(env)
    now = int(Date.now())
    register_sets = await _world_unique_register_sets(env, now)
    payload = {
        "ok": True,
        "now": now,
        "metric": "approximate_unique_visitors",
        "approximate": True,
        "timeZone": "UTC",
    }
    payload.update(world_visitor_metrics.unique_visit_summary(register_sets))
    resp = json_response(payload, cache_seconds=WORLD_VISITORS_TTL)
    await edge_cache_put(WORLD_VISITORS_CACHE_KEY, resp)
    return resp


async def _world_social_fetch_text(url, headers):
    try:
        resp = await js_fetch(url, to_js({
            "method": "GET",
            "headers": headers,
            "redirect": "follow",
        }))
        if int(getattr(resp, "status", 0)) != 200:
            return None
        return str(await resp.text())
    except Exception:
        return None


async def _world_x_api_posts(env):
    """Read @forkmesh through the official X API when a token is configured."""
    token = clean_string(
        getattr(env, "X_API_BEARER_TOKEN", ""), 512).strip()
    if not token:
        return [], "X public embed returned no posts and no API token is configured."
    headers = {
        "accept": "application/json",
        "authorization": "Bearer " + token,
    }
    user_text = await _world_social_fetch_text(
        world_social_feeds.X_API_USER_URL, headers)
    try:
        user_payload = json.loads(user_text) if user_text else {}
    except ValueError:
        user_payload = {}
    user_id = str((user_payload.get("data") or {}).get("id") or "")
    if not re.fullmatch(r"[0-9]{1,32}", user_id):
        return [], "The configured X API token could not resolve @forkmesh."
    posts_url = (
        world_social_feeds.X_API_POSTS_URL.format(user_id=quote(user_id)) +
        "?max_results=10&exclude=replies,retweets&"
        "tweet.fields=created_at,public_metrics"
    )
    posts_text = await _world_social_fetch_text(posts_url, headers)
    try:
        posts_payload = json.loads(posts_text) if posts_text else {}
    except ValueError:
        posts_payload = {}
    posts = world_social_feeds.normalize_x_api_timeline(
        posts_payload, world_social_feeds.TWITTER_HANDLE)
    if not posts:
        return [], "The X API returned no recent public @forkmesh posts."
    return posts, ""


async def world_social_posts_handler(env, request):
    """Public read-only proxy feeding the Twitter, Reddit, and blog world
    banners.

    The external upstreams are best-effort: a network refusal (X retiring
    the syndication page, Reddit rate-limiting the colo) downgrades that
    banner to its static sign via state=unavailable instead of failing the
    read. The blog board reads the blog's own RSS document, built here from
    our static index through env.ASSETS, so it never leaves the Worker and
    shows exactly what subscribers get.
    """
    if method_name(request) != "GET":
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            extra_headers={"allow": "GET"})
    cached = await edge_cache_match(WORLD_SOCIAL_POSTS_CACHE_KEY)
    if cached is not None:
        return cached
    twitter_posts, twitter_ok = [], False
    twitter_source = "public-syndication"
    twitter_reason = "The public X timeline could not be reached."
    twitter_html = await _world_social_fetch_text(
        world_social_feeds.TWITTER_SYNDICATION_URL,
        {"accept": "text/html,application/json"},
    )
    if twitter_html is not None:
        next_data = world_social_feeds.extract_next_data(twitter_html)
        if next_data is not None:
            twitter_posts = world_social_feeds.normalize_twitter_timeline(
                next_data)
            twitter_ok = bool(twitter_posts)
            if not twitter_ok:
                twitter_reason = (
                    "X's public embed returned no recent @forkmesh posts.")
    if not twitter_ok:
        api_posts, api_reason = await _world_x_api_posts(env)
        if api_posts:
            twitter_posts = api_posts
            twitter_ok = True
            twitter_source = "x-api-v2"
            twitter_reason = ""
        elif api_reason:
            twitter_reason = api_reason
    reddit_posts, reddit_ok = [], False
    reddit_text = await _world_social_fetch_text(
        world_social_feeds.REDDIT_LISTING_URL,
        {
            "accept": "application/json",
            "user-agent": world_social_feeds.REDDIT_USER_AGENT,
        },
    )
    if reddit_text is not None:
        try:
            listing = json.loads(reddit_text)
        except ValueError:
            listing = None
        if listing is not None:
            reddit_posts = world_social_feeds.normalize_reddit_listing(
                listing)
            reddit_ok = True
    blog_posts, blog_ok = [], False
    try:
        blog_rss = await _blog_feed_published_document(env, request)
        if blog_rss:
            blog_posts = world_social_feeds.normalize_blog_feed(blog_rss)
            # One post at a time, never gathered: this endpoint is public and
            # each post costs its own D1 reads, so the fan-out was two tasks
            # per published post inside one request — see the
            # no-concurrent-tasks rule at the top of this module. A failing
            # post still degrades to the same empty shapes the gathered
            # return_exceptions=True produced.
            for post in blog_posts:
                try:
                    distribution = await _blog_post_distribution(
                        env, request, post)
                except Exception:
                    distribution = None
                try:
                    reach = await _blog_post_reach_summary(env, post)
                except Exception:
                    reach = None
                post["distribution"] = (
                    distribution
                    if isinstance(distribution, dict)
                    else {"known": False, "networks": []}
                )
                post["reach"] = reach if isinstance(reach, dict) else {}
            blog_ok = bool(blog_posts)
    except Exception:
        blog_posts, blog_ok = [], False
    resp = json_response(
        world_social_feeds.social_posts_payload(
            int(Date.now()), twitter_posts, reddit_posts, blog_posts,
            twitter_ok, reddit_ok, blog_ok,
            twitter_reason=twitter_reason,
            twitter_source=twitter_source),
        cache_seconds=WORLD_SOCIAL_POSTS_TTL,
    )
    await edge_cache_put(WORLD_SOCIAL_POSTS_CACHE_KEY, resp)
    return resp


async def world_satellites_handler(env, request):
    """Serve only the cached last-good snapshot; never fetch per visitor."""
    if method_name(request) != "GET":
        return json_response(
            {"error": "method_not_allowed"},
            status=405,
            cache_control="no-store",
            extra_headers={"allow": "GET"},
        )
    cached = await edge_cache_match(WORLD_SATELLITE_CACHE_KEY)
    if cached is not None:
        return cached

    await ensure_schema(env)
    row = await d1_first(
        env,
        """SELECT data,digest FROM world_satellite_snapshot
             WHERE snapshot_id=1""",
    )
    try:
        payload = world_satellites.parse_snapshot_document(
            str((row or {}).get("data") or ""))
        _document, digest = world_satellites.serialize_snapshot_payload(
            payload)
        stored_digest = str((row or {}).get("digest") or "").lower()
        if not hmac.compare_digest(digest, stored_digest):
            raise world_satellites.SatelliteDataError(
                "satellite_snapshot_digest_mismatch")
    except world_satellites.SatelliteDataError:
        # Orbit data is optional scenery. A fresh deployment may legitimately
        # wait for its first scheduled CelesTrak refresh, and a corrupt cached
        # snapshot must still fail closed, but neither condition should turn the
        # World's normal GET into a noisy 503. Serve a bounded empty snapshot;
        # deterministic stars and planets remain visible until cron seeds D1.
        response = json_response(
            {
                "ok": True,
                "status": "warming",
                "schemaVersion": 1,
                "sourceEpoch": 0,
                "fetchedAt": 0,
                "recordCount": 0,
                "satellites": [],
            },
            cache_control=(
                "public, max-age=60, stale-while-revalidate=300"),
            extra_headers={
                **EXPECTED_DEGRADED_HEADERS,
                "x-content-type-options": "nosniff",
            },
        )
        await edge_cache_put(WORLD_SATELLITE_CACHE_KEY, response)
        return response

    response = json_response(
        payload,
        cache_control=(
            "public, max-age=%d, stale-while-revalidate=86400"
            % WORLD_SATELLITE_CACHE_SECONDS
        ),
        extra_headers={"x-content-type-options": "nosniff"},
    )
    await edge_cache_put(WORLD_SATELLITE_CACHE_KEY, response)
    return response


async def world_fediverse_directory_handler(env, request, path):
    return await world_community_api.handle_fediverse(
        _WorldCommunityRuntime(env, request), path)


async def world_media_handler(env, request, path):
    return await world_community_api.handle_media(
        _WorldCommunityRuntime(env, request), path)


async def world_events_handler(env, request, path):
    return await world_events_api.handle(
        _WorldCommunityRuntime(env, request), path)


# Kept alongside the adapter so the legacy Office wall and the universal task
# route are visibly wired to the same encrypted task service.


async def world_build_board_handler(env, request, path):
    runtime = _OfficeMarketingTasksRuntime(env, request)
    # Reject unauthorized mutations before touching a repository mirror. A
    # forged POST must not turn the issue list into a gateway-probing primitive.
    if method_name(request) == "POST":
        account_bi, record = await runtime.session({})
        actor = clean_string(
            (record or {}).get("name") or "", MAX_NODE_NAME
        ).strip().lower()
        org_bi, organization = await runtime.organization()
        role, permission = (
            await runtime.membership(org_bi, actor)
            if account_bi and actor and org_bi and organization
            else ("", "")
        )
        if (
            not account_bi
            or not actor
            or (
                role not in ("owner", "admin")
                and permission not in ("maintain", "admin")
            )
        ):
            return await world_build_board.handle(runtime, path, [])
    return await world_build_board.handle(runtime, path, [])


async def world_link_kiosk_handler(env, request, path):
    return await world_link_kiosk.handle(
        _WorldCommunityRuntime(env, request), path)


async def world_deploy_status_handler(env, request):
    if method_name(request) != "GET":
        return json_response(
            {"ok": False, "error": "method_not_allowed"},
            status=405,
            extra_headers={"Allow": "GET"},
        )
    row = await d1_first(
        env,
        "SELECT state,revision,started_at,finished_at "
        "FROM world_deploy_status WHERE singleton=1",
    )
    row = row or {}
    state = str(row.get("state") or "idle")
    if state not in ("idle", "deploying", "ready", "failed"):
        state = "idle"
    return json_response(
        {
            "ok": True,
            "state": state,
            "revision": clean_string(row.get("revision") or "", 96),
            "startedAt": max(0, int(row.get("started_at") or 0)),
            "finishedAt": max(0, int(row.get("finished_at") or 0)),
            "now": Date.now(),
        },
        status=200,
        cache_control="no-store, max-age=0",
    )


async def world_preferences_handler(env, request):
    await ensure_schema(env)
    method = method_name(request)
    if method not in ("GET", "POST"):
        return json_response({"error": "method_not_allowed"}, status=405)
    data = {}
    if method == "POST":
        if not _request_origin_allowed(env, request):
            return json_response({"error": "origin_not_allowed"}, status=403)
        try:
            data = await bounded_json_request(
                request, WORLD_PREFERENCES_MAX_BYTES)
        except RequestBodyTooLarge:
            return json_response({"error": "payload_too_large"}, status=413)
        except Exception:
            return json_response({"error": "invalid_json"}, status=400)
    account_bi, rec = await _account_session_record(env, request, data)
    if not account_bi or not rec:
        return json_response({"error": "invalid_session"}, status=401)
    now = int(Date.now())
    stored = _clean_world_preferences(
        rec.get("world_preferences"), now)
    if method == "POST":
        incoming = _clean_world_preferences(data, now)
        settings = stored["settings"]
        settings_updated_at = stored["settingsUpdatedAt"]
        if incoming["settingsUpdatedAt"] >= settings_updated_at:
            settings = incoming["settings"]
            settings_updated_at = incoming["settingsUpdatedAt"]
        by_id = {view["id"]: view for view in stored["savedViews"]}
        for view in incoming["savedViews"]:
            previous = by_id.get(view["id"])
            if not previous or view["updatedAt"] >= previous["updatedAt"]:
                by_id[view["id"]] = view
        views = sorted(
            by_id.values(),
            key=lambda item: item["updatedAt"],
            reverse=True,
        )[:WORLD_PREFERENCES_MAX_VIEWS]
        stored = {
            "settings": settings,
            "settingsUpdatedAt": settings_updated_at,
            "savedViews": views,
            "updatedAt": now,
        }
        rec["world_preferences"] = stored
        await _save_account(env, account_bi, rec)
    return json_response({
        "ok": True,
        **stored,
        "storage": "account-encrypted",
    }, cache_control="no-store")


async def world_community_ads_handler(env, request, path):
    return await community_ads_api.handle(
        _WorldCommunityRuntime(env, request), path)


async def world_fediverse_mentions_handler(env, request, path):
    return await fediverse_mentions_api.handle(
        _WorldCommunityRuntime(env, request), path)


async def _world_moderation_tokens(env, request, now=None):
    """Derive rotating moderation subjects while raw request data is transient."""
    now = int(Date.now()) if now is None else int(now)
    day = now // WORLD_MANUAL_TOKEN_DAY_MS
    address = _transient_client_address(request)
    try:
        # The exact value is used only as keyed-HMAC input in this call frame.
        # Capping it prevents an oversized header from becoming hash work; it is
        # never returned, forwarded, logged, or persisted.
        transient_agent = str(
            request.headers.get("user-agent") or "")[:2048]
    except Exception:
        transient_agent = ""
    tokens = {"ip": "", "agent": ""}
    if address:
        tokens["ip"] = await blind_index(
            env, "world-manual-ip-v1:%d:%s" % (day, address))
    if transient_agent:
        tokens["agent"] = await blind_index(
            env, "world-manual-agent-v1:%d:%s" % (day, transient_agent))
    # `address` and `transient_agent` die with this call. Only keyed, rotating
    # tokens may be forwarded or persisted; public badge display continues to
    # use the existing generalized browser/OS allowlist.
    return tokens


async def _world_active_manual_block(env, tokens, now=None):
    """Return an active explicit block matching either opaque subject token."""
    now = int(Date.now()) if now is None else int(now)
    values = tokens if isinstance(tokens, dict) else {}
    ip_token = str(values.get("ip") or "")
    agent_token = str(values.get("agent") or "")
    if not (
        re.fullmatch(r"[a-f0-9]{64}", ip_token)
        or re.fullmatch(r"[a-f0-9]{64}", agent_token)
    ):
        return None
    await ensure_schema(env)
    row = await d1_first(
        env,
        "SELECT target_type,expires_at FROM world_manual_blocks "
        "WHERE revoked_at=0 AND expires_at>? AND "
        "((target_type='ip' AND subject_token=?) OR "
        "(target_type='agent' AND subject_token=?)) "
        "ORDER BY expires_at ASC LIMIT 1",
        now, ip_token, agent_token,
    )
    if not row:
        return None
    return {
        "targetType": (
            str(row.get("target_type") or "")
            if str(row.get("target_type") or "") in ("ip", "agent")
            else "unknown"),
        "expiresAt": int(row.get("expires_at") or 0),
    }


def _world_manual_control_signature(env, target_type, subject_token, expires_at):
    canonical = (
        "forkmesh-world-manual-block-v1\n%s\n%s\n%d"
        % (target_type, subject_token, int(expires_at))
    ).encode()
    return hmac.new(
        _account_session_secret(env), canonical, "sha256").hexdigest()


async def _world_disconnect_manual_block(
        env, target_type, subject_token, expires_at):
    """Ask the World DO to eject matching live sockets after persistence."""
    world_id = env.FORKMESH_WORLD.idFromName("town-square-v1")
    world_object = env.FORKMESH_WORLD.get(world_id)
    payload = {
        "targetType": target_type,
        "subjectToken": subject_token,
        "expiresAt": int(expires_at),
    }
    control_request = JsRequest.new(
        "https://forkmesh.internal/api/world/manual-block",
        to_js({
            "method": "POST",
            "headers": {
                "content-type": "application/json",
                "x-forkmesh-world-control": _world_manual_control_signature(
                    env, target_type, subject_token, expires_at),
            },
            "body": json.dumps(payload, separators=(",", ":")),
        }),
    )
    response = await world_object.fetch(control_request)
    if int(getattr(response, "status", 0) or 0) != 200:
        return 0
    try:
        result = json.loads(await response.text())
    except Exception:
        return 0
    return max(0, min(64, int(result.get("disconnected") or 0)))


def _world_mirror_push_changed_files(value):
    files = []
    if isinstance(value, list):
        for item in value[:16]:
            path = clean_string(item, 160).strip().replace("\\", "/")
            if (
                not path
                or path.startswith("/")
                or path == ".."
                or path.startswith("../")
                or path in files
            ):
                continue
            files.append(path)
            if len(files) >= 8:
                break
    return files


def _world_mirror_push_signature(env, node, repo, commit, changed_files=None):
    files = _world_mirror_push_changed_files(changed_files)
    canonical = (
        "forkmesh-world-mirror-push-v2\n%s\n%s\n%s\n%s"
        % (node, repo, commit, "\n".join(files))
    ).encode()
    return hmac.new(
        _account_session_secret(env), canonical, "sha256").hexdigest()


async def _world_broadcast_mirror_push(
        env, node, repo, commit, changed_files=None):
    """Tell the live World room a mirror node's served head just advanced."""
    world_id = env.FORKMESH_WORLD.idFromName("town-square-v1")
    world_object = env.FORKMESH_WORLD.get(world_id)
    files = _world_mirror_push_changed_files(changed_files)
    payload = {
        "node": node,
        "repo": repo,
        "commit": commit,
        "changedFiles": files,
    }
    control_request = JsRequest.new(
        "https://forkmesh.internal/api/world/mirror-push",
        to_js({
            "method": "POST",
            "headers": {
                "content-type": "application/json",
                "x-forkmesh-world-control": _world_mirror_push_signature(
                    env, node, repo, commit, files),
            },
            "body": json.dumps(payload, separators=(",", ":")),
        }),
    )
    response = await world_object.fetch(control_request)
    return int(getattr(response, "status", 0) or 0) == 200



WORLD_MIRROR_TRAFFIC_FLUSH_MS = 10_000
WORLD_MIRROR_TRAFFIC_MAX_NODES = 64
WORLD_MIRROR_TRAFFIC_MAX_COUNT = 10_000
_WORLD_MIRROR_TRAFFIC_NODE_PATTERN = r"[a-z0-9][a-z0-9.-]{0,79}"
_world_mirror_traffic = {"counts": {}, "flushed_at": 0}


def _world_mirror_traffic_entries(value):
    counts = {}
    if isinstance(value, dict):
        value = list(value.items())
    if not isinstance(value, list):
        return []
    for item in value[:WORLD_MIRROR_TRAFFIC_MAX_NODES * 2]:
        if not isinstance(item, (list, tuple)) or len(item) != 2:
            continue
        name = str(item[0] or "").strip().lower()
        try:
            count = int(item[1])
        except (TypeError, ValueError, OverflowError):
            continue
        if count <= 0 or not re.fullmatch(_WORLD_MIRROR_TRAFFIC_NODE_PATTERN, name):
            continue
        counts[name] = min(
            WORLD_MIRROR_TRAFFIC_MAX_COUNT, counts.get(name, 0) + count)
    return sorted(counts.items())[:WORLD_MIRROR_TRAFFIC_MAX_NODES]


def _world_mirror_traffic_signature(env, at, entries):
    canonical = "forkmesh-world-mirror-traffic-v1\n%d\n%s" % (
        int(at),
        "\n".join("%s=%d" % (name, count) for name, count in entries),
    )
    return hmac.new(
        _account_session_secret(env), canonical.encode(), "sha256").hexdigest()


def _world_mirror_traffic_take(node, now, state=None):
    state = _world_mirror_traffic if state is None else state
    name = str(node or "").strip().lower()
    counts = state["counts"]
    if re.fullmatch(_WORLD_MIRROR_TRAFFIC_NODE_PATTERN, name) and (
            name in counts or len(counts) < WORLD_MIRROR_TRAFFIC_MAX_NODES):
        counts[name] = min(
            WORLD_MIRROR_TRAFFIC_MAX_COUNT, counts.get(name, 0) + 1)
    if not counts or now - state["flushed_at"] < WORLD_MIRROR_TRAFFIC_FLUSH_MS:
        return []
    state["flushed_at"] = now
    entries = sorted(counts.items())
    counts.clear()
    return entries


async def world_note_mirror_traffic(env, node):
    now = int(Date.now())
    entries = _world_mirror_traffic_take(node, now)
    if not entries:
        return False
    try:
        world_id = env.FORKMESH_WORLD.idFromName("town-square-v1")
        world_object = env.FORKMESH_WORLD.get(world_id)
        control_request = JsRequest.new(
            "https://forkmesh.internal/api/world/mirror-traffic",
            to_js({
                "method": "POST",
                "headers": {
                    "content-type": "application/json",
                    "x-forkmesh-world-control":
                        _world_mirror_traffic_signature(env, now, entries),
                },
                "body": json.dumps(
                    {"at": now, "nodes": [list(item) for item in entries]},
                    separators=(",", ":")),
            }),
        )
        response = await world_object.fetch(control_request)
        return int(getattr(response, "status", 0) or 0) == 200
    except Exception:
        return False


async def world_moderation_handler(env, request):
    """Create one explicit, temporary World block as a platform admin."""
    if method_name(request) != "POST":
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            cache_control="no-store, max-age=0, must-revalidate",
            extra_headers={"allow": "POST"})
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response(
            {"error": "invalid_json"}, status=400,
            cache_control="no-store, max-age=0, must-revalidate")
    if not isinstance(data, dict):
        return json_response(
            {"error": "invalid_json"}, status=400,
            cache_control="no-store, max-age=0, must-revalidate")
    account_bi, rec = await _account_session_record(env, request, data)
    actor = clean_string(
        rec.get("name", "") if rec else "", MAX_NODE_NAME).strip().lower()
    if not actor:
        return json_response(
            {"error": "invalid_session"}, status=401,
            cache_control="no-store, max-age=0, must-revalidate")
    if not await _has_role(env, actor, "platform_administrator"):
        await _audit_sensitive_action(
            env, actor, "world.manual_block", "world_peer", "", "denied",
            {"reason": "platform_administrator_required"})
        return json_response(
            {"error": "forbidden"}, status=403,
            cache_control="no-store, max-age=0, must-revalidate")

    target_type = str(data.get("targetType") or "").strip().lower()
    subject_token = str(data.get("handle") or "").strip().lower()
    duration = data.get("durationMs", WORLD_MANUAL_BLOCK_DEFAULT_MS)
    if target_type not in ("ip", "agent"):
        return json_response(
            {"error": "invalid_target_type"}, status=400,
            cache_control="no-store, max-age=0, must-revalidate")
    if not re.fullmatch(r"[a-f0-9]{64}", subject_token):
        return json_response(
            {"error": "invalid_handle"}, status=400,
            cache_control="no-store, max-age=0, must-revalidate")
    if (
        isinstance(duration, bool)
        or not isinstance(duration, int)
        or duration < WORLD_MANUAL_BLOCK_MIN_MS
        or duration > WORLD_MANUAL_BLOCK_MAX_MS
    ):
        return json_response(
            {"error": "invalid_duration"}, status=400,
            cache_control="no-store, max-age=0, must-revalidate")

    now = int(Date.now())
    # Subject tokens rotate daily. Do not pretend a block can outlive the token
    # it targets, even when the requested bounded duration crosses UTC midnight.
    token_expires = ((now // WORLD_MANUAL_TOKEN_DAY_MS) + 1
                     ) * WORLD_MANUAL_TOKEN_DAY_MS
    expires_at = min(now + duration, token_expires)
    block_id = new_world_peer_id() + new_world_peer_id()
    try:
        await ensure_schema(env)
        await d1_run(
            env,
            "DELETE FROM world_manual_blocks WHERE expires_at<?",
            now - WORLD_MANUAL_BLOCK_RETAIN_MS,
        )
        await d1_run(
            env,
            "INSERT INTO world_manual_blocks "
            "(block_id,target_type,subject_token,created_by_bi,created_at,"
            "expires_at,revoked_at) VALUES (?,?,?,?,?,?,0)",
            block_id, target_type, subject_token, account_bi, now, expires_at,
        )
    except Exception:
        await _audit_sensitive_action(
            env, actor, "world.manual_block", "world_" + target_type,
            subject_token, "failed",
            {"reason": "persistence_failed", "durationMs": duration})
        raise

    disconnected = 0
    try:
        disconnected = await _world_disconnect_manual_block(
            env, target_type, subject_token, expires_at)
    except Exception:
        # Admission checks are D1-backed, so a transient DO control failure does
        # not undo the block. Existing sockets will also disappear on their
        # normal stale/close boundary.
        disconnected = 0
    await _audit_sensitive_action(
        env, actor, "world.manual_block", "world_" + target_type,
        subject_token, "success", {
            "manual": True,
            "targetType": target_type,
            "durationMs": duration,
            "expiresAt": expires_at,
            "disconnectedCount": disconnected,
        })
    return json_response(
        {
            "ok": True,
            "blockId": block_id,
            "targetType": target_type,
            "expiresAt": expires_at,
            "disconnected": disconnected,
            "notice": (
                "This is an explicit temporary administrator action. "
                "ForkMesh automatic abuse detection and quarantine remain off."
            ),
        },
        cache_control="no-store, max-age=0, must-revalidate",
        extra_headers={"x-content-type-options": "nosniff"},
    )


async def _world_node_delete_signed_actor(env, request, data):
    """Resolve the admin account behind a key-signed node deletion, or ""."""
    if method_name(request) != "POST":
        return ""
    params = parse_qs(urlparse(request.url).query)
    actor = clean_string(params.get("node", [""])[0], MAX_NODE_NAME).lower()
    ts = clean_string(params.get("ts", [""])[0], 20)
    sig = clean_string(params.get("sig", [""])[0], 200)
    target = clean_string(
        (data or {}).get("nodeName", ""), MAX_NODE_NAME).lower()
    if not actor or not sig or not target or not _ts_ok(ts):
        return ""
    canonical = (
        WORLD_NODE_DELETE_PROOF + "\n" + actor + "\n" + target + "\n" + str(ts)
    ).encode()
    # Every key the account may sign as, not just its primary pubkey — same
    # durable desktop identity as _org_task_signed_session.
    if not await _verify_owner_signature(env, actor, sig, canonical):
        return ""
    _, record = await _account_row(env, actor)
    if not record or record.get("status") != "active":
        return ""
    return actor


async def world_admin_delete_node_handler(env, request):
    """Permanently remove one named node after an exact admin confirmation."""
    if method_name(request) != "POST":
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            cache_control="no-store, max-age=0, must-revalidate",
            extra_headers={"allow": "POST"})
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response(
            {"error": "invalid_json"}, status=400,
            cache_control="no-store, max-age=0, must-revalidate")
    actor = await _authed_account_name(env, request, data)
    if not actor:
        actor = await _world_node_delete_signed_actor(env, request, data)
    if not actor or not await _is_admin(env, actor):
        return json_response(
            {"error": "not_admin"}, status=403,
            cache_control="no-store, max-age=0, must-revalidate")
    target = clean_string(data.get("nodeName", ""), MAX_NODE_NAME).lower()
    requested_target = target
    node_id = clean_string(data.get("nodeId", ""), 120)
    machine_name = clean_string(
        data.get("machineName", ""), MAX_NODE_NAME).lower()
    confirmation = clean_string(data.get("confirmation", ""), 128)
    if not valid_node_name(target):
        return json_response({"error": "invalid_node_name"}, status=400)
    required = "DELETE " + requested_target
    if confirmation != required:
        return json_response(
            {"error": "confirmation_mismatch",
             "requiredConfirmation": required}, status=400)
    if target == actor or target in ("forkmesh", "forkmesh-mainnode"):
        await _audit_sensitive_action(
            env, actor, "world.node_delete", "node", target, "denied",
            {"reason": "protected_node"})
        return json_response({"error": "protected_node"}, status=409)
    target_bi, record = await _account_row(env, target)
    requested_bi = target_bi
    node_row = await d1_first(
        env, "SELECT node_bi,name FROM nodes WHERE node_bi=?", target_bi)
    if not record or not node_row:
        # Public mirror catalogs distinguish an operator account name, a
        # machine label, and a cryptographic node id. Older World panels used
        # the machine label as if it were the account key, so deletion could
        # fail with node_not_found even while the cabinet was visibly online.
        # Resolve either bounded public identifier back to the canonical node
        # row, then perform every destructive operation on that canonical key.
        resolved = await d1_first(
            env,
            "SELECT node_bi,name FROM nodes "
            "WHERE lower(name) IN (?,?) OR pubkey=? LIMIT 1",
            requested_target, machine_name, node_id)
        canonical = clean_string(
            (resolved or {}).get("name", ""), MAX_NODE_NAME).lower()
        if resolved and canonical and valid_node_name(canonical):
            resolved_bi, resolved_record = await _account_row(env, canonical)
            if resolved_bi == resolved.get("node_bi"):
                target = canonical
                target_bi = resolved_bi
                record = resolved_record
                node_row = resolved
    if not node_row:
        # A half-removed node can remain visible through its durable repository
        # catalog or signed HTTPS endpoint after the account row is gone.
        # Resolve that endpoint without requiring the already-missing account
        # ciphertext, then finish the same idempotent scoped cleanup.
        endpoint = await d1_first(
            env,
            "SELECT node_bi,node_name FROM mirror_https_endpoints "
            "WHERE lower(node_name) IN (?,?) OR public_key=? LIMIT 1",
            requested_target, machine_name, node_id)
        endpoint_name = clean_string(
            (endpoint or {}).get("node_name", ""), MAX_NODE_NAME).lower()
        if endpoint and endpoint_name and valid_node_name(endpoint_name):
            target = endpoint_name
            target_bi = endpoint.get("node_bi") or target_bi
            _, endpoint_record = await _account_row(env, endpoint_name)
            record = record or endpoint_record
            node_row = endpoint
    if (
        record
        and not node_row
        and (
            clean_string(record.get("kind", ""), 16).lower() == "user"
            or bool(record.get("pass_hash"))
        )
    ):
        return json_response({"error": "not_a_node"}, status=409)
    if target == actor or target in ("forkmesh", "forkmesh-mainnode"):
        await _audit_sensitive_action(
            env, actor, "world.node_delete", "node", target, "denied",
            {"reason": "protected_node"})
        return json_response({"error": "protected_node"}, status=409)
    if record and await _is_admin(env, target):
        await _audit_sensitive_action(
            env, actor, "world.node_delete", "node", target, "denied",
            {"reason": "admin_node"})
        return json_response({"error": "protected_node"}, status=409)

    owner = clean_string(
        (record or {}).get("owner", ""), MAX_NODE_NAME).lower()

    alias_handoffs, blocked_repo = (
        await rehome_organization_aliases_before_node_delete(
            env, target, owner))
    if blocked_repo:
        await _audit_sensitive_action(
            env, actor, "world.node_delete", "node", target, "denied",
            {"reason": "organization_alias_requires_replacement",
             "repo": blocked_repo})
        return json_response({
            "error": "organization_alias_requires_replacement",
            "repo": blocked_repo,
        }, status=409)

    if owner:
        owner_bi, owner_record = await _account_row(env, owner)
        if owner_record:
            owner_record["nodes"] = [
                node for node in _owned_nodes(owner_record)
                if str(node).lower() != target
            ]
            await _save_account(env, owner_bi, owner_record)

    identifiers = {
        value for value in (requested_target, machine_name, target)
        if value and valid_node_name(value)
    }
    identity_bis = {
        value for value in (requested_bi, target_bi) if value
    }
    # Finish every node-scoped namespace even when an earlier attempt already
    # removed one of the primary rows. Deleting twice is intentionally safe.
    for identity_bi in identity_bis:
        await d1_run(
            env, "DELETE FROM reward_node_observations WHERE node_bi=?",
            identity_bi)
        await d1_run(
            env, "DELETE FROM private_mirror_routes WHERE node_bi=?",
            identity_bi)
        await d1_run(
            env, "DELETE FROM mirror_https_endpoints WHERE node_bi=?",
            identity_bi)
    for identifier in identifiers:
        await d1_run(
            env, "DELETE FROM org_agent_jobs WHERE target_node=?",
            identifier)
        await d1_run(
            env, "DELETE FROM org_agent_sessions WHERE target_node=?",
            identifier)
        await d1_run(
            env, "DELETE FROM mirror_https_endpoints "
            "WHERE lower(node_name)=?",
            identifier)
        # The public /status page builds its mirror roster from recorded
        # samples (status_history), not from the live endpoint table, so a
        # deleted node kept a "Mirror node — <name>" row with 30 days of
        # history — and a permanent "down" verdict — long after every other
        # trace of it was gone. Drop its samples too so deletion leaves none.
        status_system = STATUS_MIRROR_PREFIX + identifier
        await d1_run(
            env, "DELETE FROM system_status_daily WHERE system=?",
            status_system)
        await d1_run(
            env, "DELETE FROM system_status_hourly WHERE system=?",
            status_system)
        await d1_run(
            env, "DELETE FROM system_status_minute WHERE system=?",
            status_system)
    for identity_bi in identity_bis:
        cleanup_name = (
            target if identity_bi == target_bi else requested_target
        )
        cleanup_record = (
            record if identity_bi == target_bi and record
            else {"name": cleanup_name, "kind": "node"}
        )
        await _delete_account_namespace(
            env, identity_bi, cleanup_record)
    await _audit_sensitive_action(
        env, actor, "world.node_delete", "node", target, "success",
        {
            "owner": owner,
            "requestedTarget": requested_target,
            "identifiers": sorted(identifiers),
            "organizationAliasHandoffs": alias_handoffs,
            "idempotent": not bool(node_row),
        })
    return json_response(
        {
            "ok": True,
            "nodeDeleted": target,
            "identifiers": sorted(identifiers),
            "alreadyAbsent": not bool(node_row),
        },
        cache_control="no-store, max-age=0, must-revalidate")


async def world_element_store_handler(env, request, path):
    """Browse, buy and configure modular World elements."""
    await ensure_schema(env)
    method = method_name(request)
    route = path.rstrip("/")
    now = int(Date.now())
    fund = await _central_fund_record(env)

    if route.endswith("/catalog"):
        if method != "GET":
            return json_response({"error": "method_not_allowed"}, status=405)
        deposits = _custodial_deposits_enabled(env)
        return json_response({
            "ok": True,
            "elements": world_element_store.catalog_public(),
            "split": world_element_store.split_policy_public(),
            "poolAddress": (fund or {}).get("address", ""),
            "network": (fund or {}).get("network", ""),
            "purchasesEnabled": bool(fund) or deposits,
            "paymentMethods": world_element_store.payment_methods_public(
                direct=bool(fund), deposit=deposits),
            "treasuryAddress": _deposit_treasury_address(env),
        }, cache_control="no-store")

    if route.endswith("/library"):
        if method != "GET":
            return json_response({"error": "method_not_allowed"}, status=405)
        account_bi, rec = await _account_session_record(env, request)
        if not account_bi or not rec:
            return json_response({"error": "invalid_session"}, status=401)
        return json_response({
            "ok": True,
            "owned": _world_element_library(rec, now),
            "storage": "account-encrypted",
        }, cache_control="no-store")

    if method != "POST":
        return json_response({"error": "method_not_allowed"}, status=405)
    if not _request_origin_allowed(env, request):
        return json_response({"error": "origin_not_allowed"}, status=403)
    try:
        data = await bounded_json_request(
            request, WORLD_ELEMENT_STORE_BODY_MAX_BYTES)
    except RequestBodyTooLarge:
        return json_response({"error": "payload_too_large"}, status=413)
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    account_bi, rec = await _account_session_record(env, request, data)
    if not account_bi or not rec:
        return json_response({"error": "invalid_session"}, status=401)
    action = clean_string(data.get("action", ""), 20).lower()

    if action == "configure":
        element_id = clean_string(data.get("elementId", ""), 64).lower()
        item = world_element_store.element(element_id)
        owned = _world_element_library(rec, now)
        if not item or element_id not in owned:
            return json_response({"error": "element_not_owned"}, status=403)
        entry = dict(owned[element_id])
        entry["params"] = world_element_store.clean_params(
            item, data.get("params"))
        if isinstance(data.get("placement"), dict):
            entry["placement"] = world_element_store.clean_placement(
                data.get("placement"))
        if isinstance(data.get("enabled"), bool):
            entry["enabled"] = data.get("enabled")
        entry["updatedAt"] = now
        owned[element_id] = entry
        rec["world_elements"] = owned
        await _save_account(env, account_bi, rec)
        return json_response({
            "ok": True, "elementId": element_id, "element": entry,
        }, cache_control="no-store")

    deposits_enabled = _custodial_deposits_enabled(env)
    if not fund and not deposits_enabled:
        return json_response(
            {"error": "community_pool_not_configured"}, status=503)

    if action == "deposit-status":
        return await _element_deposit_status(env, request, data, account_bi,
                                             rec, now)

    if action == "prepare":
        element_id = clean_string(data.get("elementId", ""), 64).lower()
        item = world_element_store.element(element_id)
        if not item:
            return json_response({"error": "unknown_element"}, status=404)
        method = clean_string(data.get("method", "direct"), 16).lower()
        if method not in ("direct", "deposit"):
            return json_response({"error": "invalid_method"}, status=400)
        if method == "deposit" and not deposits_enabled:
            return json_response(
                {"error": "custodial_deposits_disabled"}, status=503)
        if method == "direct" and not fund:
            return json_response(
                {"error": "community_pool_not_configured"}, status=503)
        if element_id in _world_element_library(rec, now):
            return json_response(
                {"error": "element_already_owned"}, status=409)
        pending = await d1_first(
            env,
            "SELECT COUNT(*) AS c FROM world_element_purchases "
            "WHERE account_bi=? AND status='prepared' AND expires_at>?",
            account_bi, now,
        )
        if int((pending or {}).get("c") or 0) >= 5:
            return json_response(
                {"error": "too_many_pending_purchases"}, status=429)
        amount = int(item["priceLamports"])
        split = world_element_store.split_lamports(amount)
        purchase_id = _random_bytes(16).hex()
        reference = _base58_encode(_random_bytes(32))
        expires_at = now + world_element_store.PURCHASE_EXPIRES_MS
        deposit_address = ""
        deposit_secret = ""
        if method == "deposit":
            # A fresh Ed25519 keypair per purchase. The seed is stored
            # encrypted and destroyed by the sweep; nothing else may read it.
            deposit_address, seed = await _new_solana_keypair()
            if not deposit_address or not seed:
                return json_response(
                    {"error": "deposit_address_unavailable"}, status=503)
            deposit_secret = await encrypt_row(env, {"seed": seed})
        pay_to = deposit_address if method == "deposit" else fund.get(
            "address", "")
        await d1_run(
            env,
            "INSERT INTO world_element_purchases "
            "(purchase_id,account_bi,element_id,amount_lamports,"
            "treasury_lamports,mirror_lamports,reference_address,status,"
            "created_at,expires_at,method,deposit_address,deposit_secret) "
            "VALUES (?,?,?,?,?,?,?,'prepared',?,?,?,?,?)",
            purchase_id, account_bi, element_id, amount,
            split["treasuryLamports"], split["mirrorLamports"], reference,
            now, expires_at, method, deposit_address, deposit_secret,
        )
        return json_response({
            "ok": True,
            "purchaseId": purchase_id,
            "elementId": element_id,
            "method": method,
            "amountLamports": amount,
            "amountSol": _amount_sol(amount),
            "payToAddress": pay_to,
            "poolAddress": fund.get("address", "") if fund else "",
            "depositAddress": deposit_address,
            "network": (fund or {}).get("network", _reward_network(env)),
            "referenceAddress": reference,
            "expiresAt": expires_at,
            "uri": _solana_pay_uri(
                pay_to, amount,
                reference="" if method == "deposit" else reference,
                message=f"ForkMesh world element: {item['label']}",
            ),
            "custody": world_element_store.custody_notice(method),
            "split": {
                **world_element_store.split_policy_public(),
                "treasuryLamports": split["treasuryLamports"],
                "mirrorNodeLamports": split["mirrorLamports"],
            },
        }, status=201, cache_control="no-store")

    if action != "confirm":
        return json_response({"error": "invalid_action"}, status=400)
    purchase_id = clean_string(data.get("purchaseId", ""), 32).lower()
    signature = clean_string(data.get("transactionSignature", ""), 120).strip()
    if (not WORLD_ELEMENT_PURCHASE_RE.fullmatch(purchase_id)
            or not re.fullmatch(r"[1-9A-HJ-NP-Za-km-z]{64,120}", signature)):
        return json_response({"error": "invalid_confirmation"}, status=400)
    row = await d1_first(
        env,
        "SELECT * FROM world_element_purchases WHERE purchase_id=?",
        purchase_id,
    )
    if not row or str(row.get("account_bi") or "") != account_bi:
        return json_response({"error": "not_found"}, status=404)
    if row.get("status") in ("confirmed", "awaiting_distribution"):
        if row.get("tx_signature") != signature:
            return json_response(
                {"error": "purchase_already_confirmed"}, status=409)
        return json_response({
            "ok": True,
            "status": row.get("status"),
            "elementId": str(row.get("element_id") or ""),
            "owned": _world_element_library(rec, now),
        }, cache_control="no-store")
    if int(row.get("expires_at") or 0) <= now:
        await d1_run(
            env,
            "UPDATE world_element_purchases SET status='expired' "
            "WHERE purchase_id=? AND status='prepared'",
            purchase_id,
        )
        return json_response({"error": "purchase_expired"}, status=410)
    replay = await d1_first(
        env,
        "SELECT purchase_id FROM world_element_purchases "
        "WHERE tx_signature=? AND purchase_id<>?",
        signature, purchase_id,
    )
    if replay:
        return json_response({"error": "transaction_replay"}, status=409)
    details = await _solana_contribution_details(
        env, signature,
        pool_address=fund.get("address", ""),
        reference_address=row.get("reference_address", ""),
        amount_lamports=int(row.get("amount_lamports") or 0),
    )
    if details is False:
        return json_response(
            {"error": "purchase_transaction_mismatch"}, status=409)
    if details is None:
        await d1_run(
            env,
            "UPDATE world_element_purchases "
            "SET status='awaiting_finality', tx_signature=? "
            "WHERE purchase_id=? AND status='prepared'",
            signature, purchase_id,
        )
        return json_response({
            "ok": True,
            "status": "awaiting_finality",
            "notice": (
                "Non-custodial: ForkMesh retained no wallet key. The element "
                "unlocks once the direct public transfer is finalized."
            ),
        }, status=202, cache_control="no-store")
    row, owned = await _grant_purchased_element(
        env, account_bi, rec, row, signature, details)
    return json_response({
        "ok": True,
        "status": row.get("status"),
        "elementId": str(row.get("element_id") or ""),
        "transactionSignature": signature,
        "explorerUrl": _reward_explorer_url(
            fund.get("network", "mainnet-beta"), signature=signature),
        "distributionIntentId": str(row.get("distribution_intent_id") or ""),
        "owned": owned,
    }, cache_control="no-store")


async def world_organizations_handler(env, request):
    """List only organization lobbies this world viewer may enter.

    The general ``/api/orgs`` collection is a signed-in management list. World
    guests need a separate, deliberately narrow directory so a public lobby is
    discoverable without making restricted/private organizations enumerable.
    Floor repositories, team layouts, and office rosters remain behind their
    independent organization handlers and access checks.
    """
    if method_name(request) != "GET":
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            extra_headers={"allow": "GET"})
    await ensure_schema(env)
    _, viewer_rec = await _account_session_record(env, request)
    viewer = (
        viewer_rec.get("name", "") if viewer_rec else ""
    ).strip().lower()
    rows = await d1_all(
        env,
        "SELECT org_bi, name, data, created_at FROM orgs "
        "ORDER BY created_at DESC LIMIT 64",
    )
    organizations = []
    for row in rows or []:
        org_bi = str(row.get("org_bi") or "")
        rec = await decrypt_row(env, row.get("data"))
        if not org_bi or not isinstance(rec, dict):
            continue
        viewer_role = (
            await _org_role(env, org_bi, viewer) if viewer else "")
        access = _org_world_access(rec.get("worldAccess"))
        if not _org_world_access_allowed(access["lobby"], viewer_role):
            continue
        organizations.append({
            "name": str(row.get("name") or ""),
            "displayName": clean_string(rec.get("displayName", ""), 80),
            "description": clean_string(rec.get("description", ""), 500),
            "logoUrl": _org_logo_url(rec.get("logoUrl", "")),
            "role": viewer_role,
            "worldAccess": access,
            "worldCapabilities": {
                "lobby": True,
                "floors": _org_world_access_allowed(
                    access["floors"], viewer_role),
                "offices": _org_world_access_allowed(
                    access["offices"], viewer_role),
            },
        })
    return json_response(
        {
            "ok": True,
            "organizations": organizations,
            "visibility": "enterable-lobbies-only",
        },
        cache_control="no-store, max-age=0, must-revalidate",
        extra_headers={"x-content-type-options": "nosniff"},
    )


def _world_relay_public_origin(value):
    """Return one public HTTPS relay origin without credentials or path data."""
    raw = clean_string(value, 300).strip()
    try:
        parsed = urlparse(raw)
    except Exception:
        return ""
    if (
        parsed.scheme.lower() != "https"
        or not parsed.hostname
        or parsed.username
        or parsed.password
        or parsed.query
        or parsed.fragment
    ):
        return ""
    host = parsed.hostname.lower().rstrip(".")
    if not host or host in ("localhost",) or host.endswith(".local"):
        return ""
    try:
        port = parsed.port
    except ValueError:
        return ""
    authority = host if port in (None, 443) else host + ":" + str(port)
    return "https://" + authority


async def world_relay_instances_handler(env, request):
    """Expose approved relay instances and generalized signed-health state.

    This public World projection deliberately omits federation public keys,
    signatures, node/account/wallet identifiers, operator tokens, exact health
    messages, repository identities, and request telemetry.
    """
    if method_name(request) != "GET":
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            cache_control="no-store", extra_headers={"allow": "GET"})
    await ensure_schema(env)
    now = int(Date.now())
    cutoff = now - WORLD_RELAY_HEALTH_FRESH_MS
    rows = await d1_all(
        env,
        "SELECT r.relay_bi,r.label,r.base_url,r.approved_at,"
        "MAX(fp.ts) AS last_seen,"
        "MAX(CASE WHEN fp.checked_at>=? AND fp.forkmesh_verified_at>=? "
        "AND fp.healthy=1 AND fp.forkmesh_active=1 "
        "AND fp.integrity='ok' AND fp.abuse_blocked=0 "
        "THEN 1 ELSE 0 END) AS fresh_verified "
        "FROM relays r LEFT JOIN federated_presence fp "
        "ON fp.relay_bi=r.relay_bi "
        "WHERE r.status='approved' "
        "GROUP BY r.relay_bi,r.label,r.base_url,r.approved_at "
        "ORDER BY r.approved_at ASC,r.relay_bi ASC",
        cutoff, cutoff,
    )
    instances = []
    for row in rows or []:
        origin = _world_relay_public_origin(row.get("base_url"))
        relay_bi = str(row.get("relay_bi") or "")
        if not origin or not relay_bi:
            continue
        last_seen = max(0, int(row.get("last_seen") or 0))
        verified_online = bool(int(row.get("fresh_verified") or 0))
        health = (
            "online"
            if verified_online
            else "offline"
            if last_seen and last_seen < cutoff
            else "awaiting_verified_health"
        )
        instances.append({
            "id": hashlib.sha256(
                ("forkmesh-world-relay-v1\0" + relay_bi).encode()
            ).hexdigest()[:24],
            "label": clean_string(
                row.get("label") or "ForkMesh instance", 80),
            "origin": origin,
            "approved": True,
            "joinedAt": max(0, int(row.get("approved_at") or 0)),
            "health": health,
            "online": verified_online,
            "healthEvidence": (
                "approved-relay-signed-fresh-node-health"
                if verified_online
                else "no-fresh-verified-node-health"
            ),
        })
    return json_response({
        "ok": True,
        "instances": instances,
        "count": len(instances),
        "observedAt": now,
        "privacy": (
            "Approved public relay origins and generalized health only. "
            "No federation keys, signatures, tokens, wallets, node identities, "
            "IP addresses, private repositories, or exact activity are exposed."
        ),
    }, cache_control="public, max-age=30",
       extra_headers={"x-content-type-options": "nosniff"})


async def world_profile_social_handler(env, request):
    """Authenticated self-profile social controls used by the World drawer.

    The follower list exposes only the same active, non-private public avatar
    fields used elsewhere in the World. Publishing writes the Note to the
    user's durable ActivityPub object collection before attempting delivery,
    so a temporarily unavailable remote inbox cannot lose the update.
    """
    if method_name(request) != "POST":
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            extra_headers={"allow": "POST"})
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    account_bi, account = await _account_session_record(env, request, data)
    if not account_bi or not account:
        return json_response({"error": "invalid_session"}, status=401)
    name = clean_string(
        account.get("name", ""), MAX_NODE_NAME).strip().lower()
    if not valid_node_name(name):
        return json_response({"error": "invalid_session"}, status=401)

    action = clean_string(data.get("action", "load"), 24).strip().lower()
    if action == "load":
        profile_origin = _ap_origin(env, request)
        rows = await d1_all(
            env,
            "SELECT follower_name FROM profile_follows "
            "WHERE target_bi=? ORDER BY created_at DESC LIMIT 24",
            account_bi)
        followers = []
        seen = set()
        for row in rows or []:
            follower_name = clean_string(
                row.get("follower_name", ""), MAX_NODE_NAME
            ).strip().lower()
            if not valid_node_name(follower_name) or follower_name in seen:
                continue
            seen.add(follower_name)
            _, follower = await _account_row(env, follower_name)
            if (
                not follower
                or follower.get("status") != "active"
                or bool(follower.get("profile_private"))
            ):
                continue
            followers.append({
                "name": follower_name,
                "avatarPng": follower.get("avatar_png", ""),
                "profileUrl": profile_origin + "/@" + quote(follower_name),
            })
        actor_bi = await _ap_actor_bi(env, AP_ACTOR_USER, name)
        remote_count_row = await d1_first(
            env,
            "SELECT COUNT(*) AS n FROM ap_followers WHERE actor_bi=?",
            actor_bi)
        remote_rows = await d1_all(
            env,
            "SELECT follower_id, follower_handle, display_name, avatar_url,"
            " url FROM ap_followers "
            "LEFT JOIN ap_remote_actors ON actor_id=follower_id "
            "WHERE actor_bi=? ORDER BY created_at DESC LIMIT ?",
            actor_bi, max(0, 24 - len(followers)))
        for follower in remote_rows or []:
            actor_id = str(follower.get("follower_id") or "").strip()
            profile_url = (
                ap.public_media_url(follower.get("url") or "") or actor_id)
            handle = clean_string(
                follower.get("follower_handle") or "", 120).strip()
            label = clean_string(
                follower.get("display_name") or handle or "Fediverse follower",
                80).strip()
            if not profile_url:
                continue
            followers.append({
                "name": label,
                "handle": handle,
                "avatarUrl": ap.public_media_url(
                    follower.get("avatar_url") or ""),
                "profileUrl": profile_url,
            })
        social = await _account_social_counts(env, name)
        return json_response({
            "ok": True,
            "followers": followers,
            "followerCount": (
                social["followers"]
                + int((remote_count_row or {}).get("n", 0) or 0)
            ),
        }, cache_control="no-store")

    if action != "publish":
        return json_response({"error": "invalid_action"}, status=400)
    if not await _ap_enabled(env):
        return _ap_disabled_response()
    if not await _ap_user_federates(env, name):
        return json_response({"error": "profile_not_public"}, status=403)

    text = clean_string(data.get("text", ""), 500).strip()
    image_data = clean_string(
        data.get("imageData", ""), ap.MAX_NOTE_IMAGE_BYTES * 2).strip()
    alt_text = clean_string(data.get("altText", ""), 420).strip()
    if not text and not image_data:
        return json_response({"error": "empty_update"}, status=400)

    images = []
    if image_data:
        _unused, images = ap.extract_body_images(
            "![world-selfie](%s)" % image_data, max_images=1)
        if not images:
            return json_response({"error": "invalid_or_large_image"}, status=400)
        images[0]["name"] = alt_text or "A selfie from ForkMesh World."

    origin = _ap_origin(env, request)
    actor = await _ap_local_actor(env, AP_ACTOR_USER, name, create=True)
    actor_bi = actor.get("actorBi") if actor else ""
    if not actor or not actor_bi:
        return json_response({"error": "actor_unavailable"}, status=503)

    # Keep accidental double-clicks bounded without making the composer feel
    # sluggish. This count is over encrypted objects' public timestamps only.
    now = int(Date.now())
    recent = await d1_first(
        env,
        "SELECT COUNT(*) AS n FROM ap_objects "
        "WHERE actor_bi=? AND published>=?",
        actor_bi, now - 60 * 1000)
    if int((recent or {}).get("n", 0) or 0) >= 5:
        return json_response({"error": "publish_rate_limited"}, status=429)

    actor_url = _ap_actor_url(origin, AP_ACTOR_USER, name)
    object_uuid = _ap_uuid()
    object_url = origin + "/ap/o/" + object_uuid
    web_url = origin + "/@" + quote(name)
    attachments = []
    for index, image in enumerate(images):
        item = ap.image_object(
            "%s/media/%d" % (object_url, index),
            media_type=image["mediaType"])
        item["name"] = image.get("name", "")
        attachments.append(item)
    note = ap.note_doc(
        object_url,
        actor_url,
        actor_url + "/followers",
        ap.note_html_from_text(text),
        now,
        web_url=web_url,
        attachments=attachments,
    )
    context_bi = await blind_index(
        env, "ap-context:user-update:" + name + ":" + object_uuid)
    await d1_run(
        env,
        "INSERT INTO ap_objects (object_uuid, actor_bi, context_bi, data,"
        " published) VALUES (?,?,?,?,?)",
        object_uuid,
        actor_bi,
        context_bi,
        await encrypt_row(env, {
            "note": note,
            "context": {
                "kind": "user-update",
                "author": name,
                "key": object_uuid,
            },
            "media": images,
        }),
        now,
    )

    followers = await d1_all(
        env,
        "SELECT inbox, shared_inbox FROM ap_followers WHERE actor_bi=?",
        actor_bi)
    body_str = json.dumps(ap.create_activity(note))
    out_data = await encrypt_row(env, {
        "body": body_str,
        "actorKind": AP_ACTOR_USER,
        "actorHandle": name,
        "actorUrl": actor_url,
    })
    queued = 0
    seen_inboxes = set()
    for follower in followers or []:
        inbox = (
            str(follower.get("shared_inbox") or "").strip()
            or str(follower.get("inbox") or "").strip()
        )
        if not inbox or inbox in seen_inboxes:
            continue
        seen_inboxes.add(inbox)
        await d1_run(
            env,
            "INSERT INTO ap_outbox (inbox, data, attempts, next_ts, created_at)"
            " VALUES (?,?,0,?,?)",
            inbox, out_data, now, now)
        queued += 1

    await _audit_sensitive_action(
        env,
        name,
        "world.activitypub.publish",
        "activitypub_note",
        object_uuid,
        details={
            "hasImage": bool(images),
            "queuedDeliveries": queued,
        },
    )
    if queued:
        await _ap_drain_outbox(env, AP_IMMEDIATE_DELIVERIES)
    return json_response({
        "ok": True,
        "published": True,
        "objectUrl": object_url,
        "queuedDeliveries": queued,
    }, status=201, cache_control="no-store")


async def world_profile_activity_feed_handler(env, request):
    if method_name(request) != "GET":
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            cache_control="no-store", extra_headers={"allow": "GET"})
    await ensure_schema(env)
    rows = await d1_all(
        env,
        "SELECT o.object_uuid,o.data,o.published,a.data AS actor_data "
        "FROM ap_objects o JOIN ap_actors a ON a.actor_bi=o.actor_bi "
        "WHERE a.kind=? ORDER BY o.published DESC LIMIT 80",
        AP_ACTOR_USER,
    )
    items = []
    for row in rows or []:
        actor = await decrypt_row(env, row.get("actor_data"))
        name = clean_string(
            (actor or {}).get("handle", ""), MAX_NODE_NAME).lower()
        if not valid_node_name(name):
            continue
        _account_bi, account = await _account_row(env, name)
        if (
            not account or account.get("status") != "active"
            or bool(account.get("profile_private"))
        ):
            continue
        record = await decrypt_row(env, row.get("data"))
        note = (record or {}).get("note")
        if not isinstance(note, dict):
            continue
        content = clean_string(
            re.sub(r"<[^>]*>", " ", str(note.get("content") or "")), 500)
        if not content:
            continue
        object_uuid = clean_string(row.get("object_uuid"), 80)
        if not object_uuid:
            continue
        items.append({
            "id": object_uuid,
            "kind": "update",
            "author": name,
            "authorName": name,
            "authorUrl": "/@" + quote(name),
            "excerpt": content,
            "published": max(0, int(row.get("published") or 0)),
            "remoteUrl": _public_https_url(note.get("url")),
        })
        if len(items) >= 50:
            break
    return json_response(
        {"ok": True, "items": items},
        cache_control="public, max-age=30, stale-while-revalidate=60",
        extra_headers={
            "access-control-allow-origin": "*",
            "x-content-type-options": "nosniff",
        },
    )


async def world_admin_errors_handler(env, request):
    """Return the admin HUD cursor and, on demand, a bounded error table."""
    method = method_name(request)
    if method not in ("GET", "POST", "DELETE"):
        return json_response(
            {"error": "method_not_allowed"},
            status=405,
            cache_control="no-store",
            extra_headers={"allow": "GET, POST, DELETE"},
        )
    _, rec = await _account_session_record(env, request)
    actor = clean_string(
        (rec or {}).get("name", ""), MAX_NODE_NAME).strip().lower()
    if not actor:
        return json_response(
            {"error": "invalid_session"}, status=401,
            cache_control="no-store")
    if not await _has_role(env, actor, "platform_administrator"):
        return json_response(
            {"error": "forbidden"}, status=403,
            cache_control="no-store")
    if method in ("POST", "DELETE"):
        try:
            data = await bounded_json_request(request)
            row_id = int(data.get("id") or 0)
        except (AttributeError, TypeError, ValueError):
            data = {}
            row_id = 0
        group = data.get("group") if isinstance(data, dict) else None
        if row_id <= 0 and not isinstance(group, dict):
            return json_response(
                {"error": "invalid_error_id"},
                status=400,
                cache_control="no-store",
            )
        if method == "POST":
            if isinstance(group, dict):
                form = {
                    "group_status": [str(group.get("status") or "")[:12]],
                    "group_method": [str(group.get("method") or "")[:12]],
                    "group_path": [str(group.get("path") or "")[:2000]],
                    "group_message": [str(group.get("message") or "")[:1000]],
                    "group_users": [clean_string(
                        group.get("users") or "", 300)],
                }
            else:
                form = {"error_id": [str(row_id)]}
            message, details = await _admin_error_create_bot_task(
                env, form, actor)
            if details.get("taskId"):
                return json_response(
                    {"ok": True, "message": message, **details},
                    status=201,
                    cache_control="no-store",
                )
            return json_response(
                {"error": "task_create_failed", "message": message},
                status=403,
                cache_control="no-store",
            )
        await ensure_schema(env)
        if isinstance(group, dict):
            status = str(group.get("status") or "")[:12]
            request_method = str(group.get("method") or "")[:12].upper()
            path = str(group.get("path") or "")[:2000]
            message = str(group.get("message") or "")[:1000]
            if not status or not request_method:
                return json_response(
                    {"error": "invalid_error_group"},
                    status=400,
                    cache_control="no-store",
                )
            await d1_run(
                env,
                "DELETE FROM error_log WHERE CAST(status AS TEXT)=? "
                "AND UPPER(method)=? AND path=? AND message=?",
                status, request_method, path, message,
            )
        else:
            await d1_run(env, "DELETE FROM error_log WHERE id=?", row_id)
        return json_response({"ok": True}, cache_control="no-store")
    try:
        query = parse_qs(urlparse(request.url).query)
        raw_after = query.get("after", ["0"])[0]
        after = max(0, int(raw_after or 0))
    except (TypeError, ValueError):
        after = 0
    await ensure_schema(env)
    latest = await d1_first(
        env,
        "SELECT id,ts,status,method,path FROM error_log ORDER BY id DESC "
        "LIMIT 1")
    count = await d1_first(
        env, "SELECT COUNT(*) AS n FROM error_log WHERE id>?", after)
    try:
        latest_status = max(0, int((latest or {}).get("status") or 0))
    except (TypeError, ValueError):
        latest_status = 0
    payload = {
        "ok": True,
        "latestId": max(0, int((latest or {}).get("id") or 0)),
        "latestAt": max(0, int((latest or {}).get("ts") or 0)),
        "newCount": min(9999, max(0, int((count or {}).get("n") or 0))),
        # Enough for the World HUD to say what just broke without putting a
        # route, a message, or an actor into the notice: the same coarse
        # (status, source) pair the new-error-group Ping carries.
        "latestStatus": latest_status,
        "latestSource": _admin_error_source(
            (latest or {}).get("method"), (latest or {}).get("path")),
    }
    if query.get("include", ["0"])[0] == "1":
        rows = await d1_all(
            env,
            "SELECT id,ts,status,method,path,message,ray,actor "
            "FROM error_log ORDER BY id DESC LIMIT 100",
        )
        # The World arrival notice links to the specific newest record. Keep
        # that record in the focused detail response even if an unusually busy
        # error storm has already pushed it just beyond the ordinary 100-row
        # board window.
        try:
            focused_id = max(0, int(query.get("id", ["0"])[0] or 0))
        except (TypeError, ValueError):
            focused_id = 0
        if focused_id and not any(
                int(row.get("id") or 0) == focused_id for row in rows or []):
            focused = await d1_first(
                env,
                "SELECT id,ts,status,method,path,message,ray,actor "
                "FROM error_log WHERE id=? LIMIT 1",
                focused_id,
            )
            if focused:
                rows = (rows or []) + [focused]
        payload["errors"] = rows or []
        grouped_rows = await d1_all(
            env,
            "SELECT id,ts,status,method,path,message,ray,actor "
            "FROM error_log WHERE ts>=? ORDER BY id DESC LIMIT 1000",
            int(Date.now()) - 24 * 60 * 60 * 1000,
        )
        now = int(Date.now())
        hourly = [0] * 24
        groups = {}
        for row in grouped_rows or []:
            signature = (
                str(row.get("status") or ""),
                str(row.get("method") or "").upper(),
                str(row.get("path") or ""),
                str(row.get("message") or ""),
            )
            group = groups.setdefault(signature, {
                "count": 0,
                "firstSeen": 0,
                "lastSeen": 0,
                "hours": [0] * 24,
                "actors": {},
                "anonymous": 0,
            })
            timestamp = max(0, int(row.get("ts") or 0))
            if 0 < timestamp < 10 ** 11:
                timestamp *= 1000
            age_hours = max(0, (now - timestamp) // (60 * 60 * 1000))
            if age_hours < 24:
                bucket = 23 - int(age_hours)
                hourly[bucket] += 1
                group["hours"][bucket] += 1
            group["count"] += 1
            group["firstSeen"] = min(group["firstSeen"] or timestamp, timestamp)
            group["lastSeen"] = max(group["lastSeen"], timestamp)
            related_actor = clean_string(
                row.get("actor") or "", MAX_NODE_NAME).strip().lower()
            if related_actor:
                group["actors"][related_actor] = (
                    group["actors"].get(related_actor, 0) + 1)
            else:
                group["anonymous"] += 1
        payload["hourly"] = hourly
        payload["groups"] = [
            {
                "status": signature[0],
                "method": signature[1],
                "path": signature[2],
                "message": signature[3],
                "count": group["count"],
                "firstSeen": group["firstSeen"],
                "lastSeen": group["lastSeen"],
                "hours": group["hours"],
                "actors": [
                    {"name": name, "count": count}
                    for name, count in sorted(
                        group["actors"].items(),
                        key=lambda item: (-item[1], item[0]),
                    )[:16]
                ],
                "anonymous": group["anonymous"],
            }
            for signature, group in sorted(
                groups.items(),
                key=lambda item: (
                    -item[1]["count"],
                    -item[1]["lastSeen"],
                ),
            )[:25]
        ]
    return json_response(
        payload,
        cache_control="no-store",
    )


async def world_recent_installs_handler(env, request):
    """Fresh successful installer runs, for the World's firework display.

    Anonymous coarse platform tokens only — the install_diag table stores no
    account, IP, or hostname (see SCHEMA note) — and the installer's random
    run id is hashed so the raw telemetry key never leaves the Worker.
    """
    if method_name(request) != "GET":
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            cache_control="no-store", extra_headers={"allow": "GET"})
    await ensure_schema(env)
    now = int(Date.now())
    cutoff = now - WORLD_INSTALL_CELEBRATION_MS
    rows = await d1_all(
        env,
        "SELECT run, MAX(ts) AS ts, MAX(os) AS os, MAX(arch) AS arch "
        "FROM install_diag WHERE step='done' AND ok=1 AND ts>=? "
        "GROUP BY run ORDER BY ts DESC LIMIT ?",
        cutoff, MAX_WORLD_INSTALL_CELEBRATIONS,
    )
    installs = []
    for row in rows or []:
        run = str(row.get("run") or "")
        if not run:
            continue
        installs.append({
            "id": hashlib.sha256(
                ("forkmesh-world-install-v1\0" + run).encode()
            ).hexdigest()[:24],
            "installedAt": max(0, int(row.get("ts") or 0)),
            "os": clean_string(row.get("os"), 32),
            "arch": clean_string(row.get("arch"), 32),
        })
    return json_response({
        "ok": True,
        "installs": installs,
        "count": len(installs),
        "observedAt": now,
        "privacy": (
            "Anonymous installer telemetry only: coarse platform tokens and "
            "a hashed per-run id. No accounts, IP addresses, or hostnames "
            "are stored or exposed."
        ),
    }, cache_control="public, max-age=30",
       extra_headers={"x-content-type-options": "nosniff"})


WORLD_FLOOR_NOTE_TEXT_MAX = 500
WORLD_FLOOR_NOTE_LIST_LIMIT = 80
WORLD_FLOOR_NOTE_REPLY_LIMIT = 100
WORLD_FLOOR_NOTE_ACCOUNT_LIMIT = 20
WORLD_FLOOR_NOTE_MIN_INTERVAL_MS = 5 * 1000
WORLD_FLOOR_NOTE_MAX_MENTIONS = 10
WORLD_FLOOR_NOTE_MAX_PINGS = 25
WORLD_FLOOR_NOTE_EXTENT = 1500.0
WORLD_FLOOR_NOTE_MIN_Y = -20.0
WORLD_FLOOR_NOTE_MAX_Y = 200.0
WORLD_FLOOR_NOTE_ANNOUNCE_MAX_AGE_MS = 60 * 1000


def _world_floor_note_id():
    return bytes(js_crypto.getRandomValues(Uint8Array.new(16)).to_py()).hex()


def _world_floor_note_text(value):
    if not isinstance(value, str):
        return ""
    text = value.replace("\r\n", "\n").replace("\r", "\n")
    text = re.sub(r"[\x00-\x08\x0b-\x1f\x7f]", "", text)
    return re.sub(r"\n{3,}", "\n\n", text).strip()


def _world_floor_note_coordinate(value, low, high):
    if isinstance(value, bool):
        return None
    try:
        number = float(value)
    except (TypeError, ValueError):
        return None
    if not math.isfinite(number) or number < low or number > high:
        return None
    return round(number, 2)


def _world_floor_note_mentions(text, actor):
    return [
        name for name in notification_mentions(text)
        if name != actor
    ][:WORLD_FLOOR_NOTE_MAX_MENTIONS]


def _world_floor_notes_signature(env, note_id, at):
    canonical = (
        "forkmesh-world-floor-notes-v1\n%s\n%d" % (note_id, int(at))
    ).encode()
    return hmac.new(
        _account_session_secret(env), canonical, "sha256").hexdigest()


def _world_floor_notes_announcement(env, data, signature, now):
    if not isinstance(data, dict):
        return ""
    note_id = str(data.get("id") or "")
    try:
        announced_at = int(data.get("at") or 0)
    except (TypeError, ValueError):
        return ""
    if (
        not re.fullmatch(r"[a-f0-9]{32}", note_id)
        or abs(int(now) - announced_at)
        > WORLD_FLOOR_NOTE_ANNOUNCE_MAX_AGE_MS
    ):
        return ""
    if not hmac.compare_digest(
            str(signature or ""),
            _world_floor_notes_signature(env, note_id, announced_at)):
        return ""
    return note_id


async def _world_floor_notes_broadcast(env, note_id):
    announced_at = int(Date.now())
    try:
        world_object = env.FORKMESH_WORLD.get(
            env.FORKMESH_WORLD.idFromName("town-square-v1"))
        response = await world_object.fetch(JsRequest.new(
            "https://forkmesh.internal/api/world/floor-notes-changed",
            to_js({
                "method": "POST",
                "headers": {
                    "content-type": "application/json",
                    "x-forkmesh-world-control": _world_floor_notes_signature(
                        env, note_id, announced_at),
                },
                "body": json.dumps(
                    {"id": note_id, "at": announced_at},
                    separators=(",", ":")),
            }),
        ))
        return int(getattr(response, "status", 0) or 0) == 200
    except Exception:
        return False


async def _world_floor_note_existing_accounts(env, names):
    found = []
    for name in names:
        if not valid_node_name(name):
            continue
        row = await d1_first(
            env, "SELECT 1 AS present FROM users WHERE user_bi=? LIMIT 1",
            await blind_index(env, name))
        if row:
            found.append(name)
    return found


def _world_floor_note_link(env, note_id):
    origin = _normalized_http_origin(getattr(env, "WORLD_ORIGIN", ""))
    return (origin.rstrip("/") if origin else "") + "/world/?note=" + note_id


async def _world_floor_note_ping(env, recipients, actor, title, text,
                                 note_id, dedupe):
    delivered = 0
    for name in recipients[:WORLD_FLOOR_NOTE_MAX_PINGS]:
        try:
            if await enqueue_notification(
                    env, name, "mention", title,
                    body=text,
                    href=_world_floor_note_link(env, note_id),
                    actor=actor,
                    source="world-floor-note",
                    dedupe="world-floor-note:" + dedupe + ":" + name):
                delivered += 1
        except Exception:
            continue
    return delivered


async def _world_floor_note_data(env, row):
    try:
        data = await decrypt_row(env, row.get("data"))
    except Exception:
        data = None
    if not isinstance(data, dict):
        return None
    author = clean_string(data.get("author"), MAX_NODE_NAME).lower()
    text = _world_floor_note_text(data.get("text"))[
        :WORLD_FLOOR_NOTE_TEXT_MAX]
    if not author or not text:
        return None
    return {"author": author, "text": text}


async def _world_floor_note_project(env, row, account_bi, is_admin=False):
    data = await _world_floor_note_data(env, row)
    if not data:
        return None
    mine = bool(account_bi) and str(row.get("account_bi") or "") == account_bi
    return {
        "id": str(row.get("note_id") or ""),
        "author": data["author"],
        "text": data["text"],
        "x": float(row.get("x") or 0),
        "y": float(row.get("y") or 0),
        "z": float(row.get("z") or 0),
        "createdAt": max(0, int(row.get("created_at") or 0)),
        "updatedAt": max(0, int(row.get("updated_at") or 0)),
        "replyCount": max(0, int(row.get("reply_count") or 0)),
        "mine": mine,
        "canDelete": mine or bool(is_admin),
    }


async def _world_floor_note_replies(env, note_id, account_bi, is_admin):
    rows = await d1_all(
        env,
        "SELECT reply_id,account_bi,created_at,data "
        "FROM world_floor_note_replies WHERE note_id=? "
        "ORDER BY created_at DESC, reply_id DESC LIMIT ?",
        note_id, WORLD_FLOOR_NOTE_REPLY_LIMIT,
    )
    replies = []
    for row in reversed(rows or []):
        data = await _world_floor_note_data(env, row)
        if not data:
            continue
        mine = (
            bool(account_bi)
            and str(row.get("account_bi") or "") == account_bi)
        replies.append({
            "id": str(row.get("reply_id") or ""),
            "author": data["author"],
            "text": data["text"],
            "createdAt": max(0, int(row.get("created_at") or 0)),
            "mine": mine,
            "canDelete": mine or bool(is_admin),
        })
    return replies


async def _world_floor_note_thread(env, note_id, account_bi, actor):
    row = await d1_first(
        env,
        "SELECT note_id,account_bi,x,y,z,created_at,updated_at,reply_count,"
        "data FROM world_floor_notes WHERE note_id=?",
        note_id,
    )
    if not row:
        return None
    is_admin = bool(actor) and await _is_admin(env, actor)
    note = await _world_floor_note_project(env, row, account_bi, is_admin)
    if not note:
        return None
    return {
        "note": note,
        "replies": await _world_floor_note_replies(
            env, note_id, account_bi, is_admin),
    }


def _world_floor_notes_response(payload, status=200):
    return json_response(
        payload, status=status, cache_control="no-store",
        extra_headers={"x-content-type-options": "nosniff"})


async def world_floor_notes_handler(env, request):
    await ensure_schema(env)
    method = method_name(request)
    if method not in ("GET", "POST"):
        return json_response(
            {"ok": False, "error": "method_not_allowed"}, status=405,
            extra_headers={"allow": "GET, POST"})
    data = {}
    if method == "POST":
        if not _request_origin_allowed(env, request):
            return _world_floor_notes_response(
                {"ok": False, "error": "origin_not_allowed"}, 403)
        try:
            data = await bounded_json_request(request, 16 * 1024)
        except RequestBodyTooLarge:
            return _world_floor_notes_response(
                {"ok": False, "error": "payload_too_large"}, 413)
        except Exception:
            return _world_floor_notes_response(
                {"ok": False, "error": "invalid_json"}, 400)
        if not isinstance(data, dict):
            return _world_floor_notes_response(
                {"ok": False, "error": "invalid_json"}, 400)
    account_bi, record = await _account_session_record(env, request, data)
    actor = (
        clean_string((record or {}).get("name"), MAX_NODE_NAME).lower()
        if account_bi and record else "")
    if not actor:
        account_bi = ""
    limits = {
        "textMax": WORLD_FLOOR_NOTE_TEXT_MAX,
        "mentionsMax": WORLD_FLOOR_NOTE_MAX_MENTIONS,
    }
    if method == "GET":
        params = parse_qs(urlparse(request.url).query)
        note_id = str((params.get("id") or [""])[0]).lower()
        if note_id:
            if not re.fullmatch(r"[a-f0-9]{32}", note_id):
                return _world_floor_notes_response(
                    {"ok": False, "error": "invalid_note"}, 400)
            thread = await _world_floor_note_thread(
                env, note_id, account_bi, actor)
            if not thread:
                return _world_floor_notes_response(
                    {"ok": False, "error": "note_not_found"}, 404)
            return _world_floor_notes_response({
                "ok": True, **thread, "viewer": actor,
                "canPost": bool(actor), "limits": limits,
            })
        rows = await d1_all(
            env,
            "SELECT note_id,account_bi,x,y,z,created_at,updated_at,"
            "reply_count,data FROM world_floor_notes "
            "ORDER BY updated_at DESC, note_id DESC LIMIT ?",
            WORLD_FLOOR_NOTE_LIST_LIMIT,
        )
        notes = []
        for row in rows or []:
            note = await _world_floor_note_project(env, row, account_bi)
            if note:
                notes.append(note)
        return _world_floor_notes_response({
            "ok": True, "notes": notes, "viewer": actor,
            "canPost": bool(actor), "limits": limits,
        })
    if not actor:
        return _world_floor_notes_response(
            {"ok": False, "error": "sign_in_required"}, 401)
    if await _world_active_manual_block(
            env, await _world_moderation_tokens(env, request)):
        return _world_floor_notes_response(
            {"ok": False, "error": "world_access_blocked"}, 403)
    action = clean_string(data.get("action"), 16).lower()
    now = int(Date.now())
    if action in ("create", "reply"):
        text = _world_floor_note_text(data.get("text"))
        if not text:
            return _world_floor_notes_response(
                {"ok": False, "error": "note_text_required"}, 400)
        if len(text) > WORLD_FLOOR_NOTE_TEXT_MAX:
            return _world_floor_notes_response(
                {"ok": False, "error": "note_too_long"}, 400)
        latest = await d1_first(
            env,
            "SELECT MAX(created_at) AS at FROM ("
            "SELECT created_at FROM world_floor_notes WHERE account_bi=? "
            "UNION ALL SELECT created_at FROM world_floor_note_replies "
            "WHERE account_bi=?)",
            account_bi, account_bi,
        )
        if now - int((latest or {}).get("at") or 0) < (
                WORLD_FLOOR_NOTE_MIN_INTERVAL_MS):
            return _world_floor_notes_response(
                {"ok": False, "error": "note_rate_limited"}, 429)
        mentioned = await _world_floor_note_existing_accounts(
            env, _world_floor_note_mentions(text, actor))
        payload = await encrypt_row(env, {"author": actor, "text": text})
    if action == "create":
        x = _world_floor_note_coordinate(
            data.get("x"), -WORLD_FLOOR_NOTE_EXTENT, WORLD_FLOOR_NOTE_EXTENT)
        y = _world_floor_note_coordinate(
            data.get("y", 0), WORLD_FLOOR_NOTE_MIN_Y, WORLD_FLOOR_NOTE_MAX_Y)
        z = _world_floor_note_coordinate(
            data.get("z"), -WORLD_FLOOR_NOTE_EXTENT, WORLD_FLOOR_NOTE_EXTENT)
        if x is None or y is None or z is None:
            return _world_floor_notes_response(
                {"ok": False, "error": "invalid_note_position"}, 400)
        owned = await d1_first(
            env,
            "SELECT COUNT(*) AS count FROM world_floor_notes "
            "WHERE account_bi=?",
            account_bi,
        )
        if int((owned or {}).get("count") or 0) >= (
                WORLD_FLOOR_NOTE_ACCOUNT_LIMIT):
            return _world_floor_notes_response(
                {"ok": False, "error": "note_limit_reached"}, 409)
        note_id = _world_floor_note_id()
        await d1_run(
            env,
            "INSERT INTO world_floor_notes(note_id,account_bi,x,y,z,"
            "created_at,updated_at,reply_count,data) "
            "VALUES(?,?,?,?,?,?,?,0,?)",
            note_id, account_bi, x, y, z, now, now, payload,
        )
        pinged = await _world_floor_note_ping(
            env, mentioned, actor,
            "@" + actor + " mentioned you in a World note", text,
            note_id, note_id)
        await _world_floor_notes_broadcast(env, note_id)
        thread = await _world_floor_note_thread(
            env, note_id, account_bi, actor)
        return _world_floor_notes_response({
            "ok": True, **(thread or {}), "mentioned": mentioned,
            "pinged": pinged,
        }, 201)
    note_id = str(data.get("id") or "").lower()
    if not re.fullmatch(r"[a-f0-9]{32}", note_id):
        return _world_floor_notes_response(
            {"ok": False, "error": "invalid_note"}, 400)
    root = await d1_first(
        env,
        "SELECT note_id,account_bi,data FROM world_floor_notes "
        "WHERE note_id=?",
        note_id,
    )
    if not root:
        return _world_floor_notes_response(
            {"ok": False, "error": "note_not_found"}, 404)
    if action == "reply":
        count = await d1_first(
            env,
            "SELECT COUNT(*) AS count FROM world_floor_note_replies "
            "WHERE note_id=?",
            note_id,
        )
        if int((count or {}).get("count") or 0) >= (
                WORLD_FLOOR_NOTE_REPLY_LIMIT):
            return _world_floor_notes_response(
                {"ok": False, "error": "note_thread_full"}, 409)
        reply_id = _world_floor_note_id()
        await d1_run(
            env,
            "INSERT INTO world_floor_note_replies(reply_id,note_id,"
            "account_bi,created_at,data) VALUES(?,?,?,?,?)",
            reply_id, note_id, account_bi, now, payload,
        )
        await d1_run(
            env,
            "UPDATE world_floor_notes SET updated_at=?,reply_count=("
            "SELECT COUNT(*) FROM world_floor_note_replies WHERE note_id=?) "
            "WHERE note_id=?",
            now, note_id, note_id,
        )
        root_data = await _world_floor_note_data(env, root) or {}
        participants = []
        author = str(root_data.get("author") or "")
        if author and author != actor:
            participants.append(author)
        for row in await d1_all(
                env,
                "SELECT data FROM world_floor_note_replies "
                "WHERE note_id=? AND account_bi<>? "
                "ORDER BY created_at DESC LIMIT ?",
                note_id, account_bi, WORLD_FLOOR_NOTE_MAX_PINGS) or []:
            reply_data = await _world_floor_note_data(env, row) or {}
            name = str(reply_data.get("author") or "")
            if name and name != actor and name not in participants:
                participants.append(name)
        pinged = await _world_floor_note_ping(
            env, mentioned, actor,
            "@" + actor + " mentioned you in a World note", text,
            note_id, reply_id)
        pinged += await _world_floor_note_ping(
            env, [name for name in participants if name not in mentioned],
            actor, "@" + actor + " replied to a World note", text,
            note_id, reply_id)
        await _world_floor_notes_broadcast(env, note_id)
        thread = await _world_floor_note_thread(
            env, note_id, account_bi, actor)
        return _world_floor_notes_response({
            "ok": True, **(thread or {}), "mentioned": mentioned,
            "pinged": pinged,
        }, 201)
    if action == "delete":
        reply_id = str(data.get("replyId") or "").lower()
        if reply_id and not re.fullmatch(r"[a-f0-9]{32}", reply_id):
            return _world_floor_notes_response(
                {"ok": False, "error": "invalid_note"}, 400)
        if reply_id:
            target = await d1_first(
                env,
                "SELECT account_bi FROM world_floor_note_replies "
                "WHERE note_id=? AND reply_id=?",
                note_id, reply_id,
            )
        else:
            target = root
        if not target:
            return _world_floor_notes_response(
                {"ok": False, "error": "note_not_found"}, 404)
        if (
            str(target.get("account_bi") or "") != account_bi
            and not await _is_admin(env, actor)
        ):
            return _world_floor_notes_response(
                {"ok": False, "error": "not_note_author"}, 403)
        if reply_id:
            await d1_run(
                env,
                "DELETE FROM world_floor_note_replies "
                "WHERE note_id=? AND reply_id=?",
                note_id, reply_id,
            )
            await d1_run(
                env,
                "UPDATE world_floor_notes SET reply_count=("
                "SELECT COUNT(*) FROM world_floor_note_replies "
                "WHERE note_id=?) WHERE note_id=?",
                note_id, note_id,
            )
        else:
            await d1_run(
                env, "DELETE FROM world_floor_note_replies WHERE note_id=?",
                note_id)
            await d1_run(
                env, "DELETE FROM world_floor_notes WHERE note_id=?",
                note_id)
        await _world_floor_notes_broadcast(env, note_id)
        thread = (
            await _world_floor_note_thread(env, note_id, account_bi, actor)
            if reply_id else None)
        return _world_floor_notes_response({
            "ok": True, "deleted": reply_id or note_id, **(thread or {}),
        })
    return _world_floor_notes_response(
        {"ok": False, "error": "invalid_action"}, 400)


_WORLD_MESH_CACHE = {"at": 0, "ttl": 0, "worlds": None}
WORLD_MESH_FETCH_TIMEOUT_SECONDS = 4
WORLD_MESH_BODY_MAX_BYTES = 4096


async def _world_mesh_load(env):
    if world_mesh.is_hub(env):
        try:
            rows = await d1_all(
                env,
                "SELECT id,name,url,logo<>'' AS has_logo,slot,updated_at "
                "FROM instance_directory WHERE hidden=0 AND failures<2 "
                "AND world=1 AND slot>=0 ORDER BY created_at, domain LIMIT ?",
                200)
        except Exception:
            rows = []
        return world_mesh.build_directory(
            world_mesh.hub_entry(env), rows,
            world_mesh.own_api_origin(env)), world_mesh.DIRECTORY_CACHE_MS
    try:
        resp = await js_fetch_with_timeout(
            world_mesh.hub_api(env) + "/api/world/mesh",
            {"method": "GET", "headers": {"accept": "application/json"},
             "redirect": "error"},
            WORLD_MESH_FETCH_TIMEOUT_SECONDS)
        if int(getattr(resp, "status", 0)) != 200:
            raise ValueError("mesh_unavailable")
        payload = json.loads(str(await resp.text())[:65536])
    except Exception:
        return [], world_mesh.DIRECTORY_FAILURE_CACHE_MS
    worlds = world_mesh.normalize_directory(payload)
    return worlds, (
        world_mesh.DIRECTORY_CACHE_MS if worlds
        else world_mesh.DIRECTORY_FAILURE_CACHE_MS)


async def world_mesh_directory(env):
    now = int(Date.now())
    cache = _WORLD_MESH_CACHE
    if cache["worlds"] is not None and now - cache["at"] < cache["ttl"]:
        return cache["worlds"]
    worlds, ttl = await _world_mesh_load(env)
    cache.update({"at": now, "ttl": ttl, "worlds": worlds})
    return worlds


async def world_mesh_handler(env, request):
    if method_name(request) != "GET":
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            extra_headers={"allow": "GET"})
    worlds = await world_mesh_directory(env)
    return json_response(
        world_mesh.directory_payload(env, worlds),
        cache_seconds=world_mesh.DIRECTORY_MAX_AGE)


async def world_visa_handler(env, request):
    no_store = "no-store, max-age=0, must-revalidate"
    if method_name(request) != "POST":
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            cache_control=no_store, extra_headers={"allow": "POST"})
    if not _request_origin_allowed(env, request):
        return json_response(
            {"error": "origin_not_allowed"}, status=403,
            cache_control=no_store)
    try:
        data = await bounded_json_request(request, WORLD_MESH_BODY_MAX_BYTES)
    except RequestBodyTooLarge:
        return json_response(
            {"error": "payload_too_large"}, status=413, cache_control=no_store)
    except Exception:
        return json_response(
            {"error": "invalid_json"}, status=400, cache_control=no_store)
    data = data if isinstance(data, dict) else {}
    claim = await _world_account_claim(env, request, data)
    if not claim:
        return json_response(
            {"ok": True, "authenticated": False, "visa": ""},
            cache_control=no_store)
    worlds = await world_mesh_directory(env)
    neighbor = world_mesh.neighbor_for_origin(
        env, worlds, data.get("destination"))
    if not neighbor:
        return json_response(
            {"error": "unknown_world"}, status=404, cache_control=no_store)
    now = int(Date.now())
    visa = world_mesh.visa_for(
        claim["name"], world_mesh.own_world_origin(env), neighbor["world"],
        new_world_peer_id(), now)
    return json_response(
        {
            "ok": True,
            "authenticated": True,
            "visa": world_mesh.visa_encode(
                _account_session_secret(env), visa),
            "expiresAt": visa["expiresAt"],
        },
        cache_control=no_store)


async def world_visa_verify_handler(env, request):
    no_store = "no-store, max-age=0, must-revalidate"
    if method_name(request) != "POST":
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            cache_control=no_store, extra_headers={"allow": "POST"})
    try:
        data = await bounded_json_request(request, WORLD_MESH_BODY_MAX_BYTES)
    except Exception:
        data = None
    data = data if isinstance(data, dict) else {}
    verified = world_mesh.visa_verify(
        _account_session_secret(env), data.get("visa"), int(Date.now()),
        world_mesh.own_world_origin(env), data.get("destination"))
    if not verified:
        return json_response(
            {"ok": False, "error": "invalid_visa"}, status=401,
            cache_control=no_store)
    return json_response(
        {"ok": True, "name": verified["name"], "nonce": verified["nonce"]},
        cache_control=no_store)


async def world_mesh_visitor(env, request, visa="", arrival=False):
    """Resolve a neighbouring World's walker from its visa.

    A cross-world socket names its home by its Origin. An arrival is a walker
    who loaded this World's own page from a neighbour's: its home is the one
    the visa claims, which must be a listed neighbour, and that neighbour's
    server is still the one asked to vouch for it.
    """
    try:
        origin = request.headers.get("origin") or ""
    except Exception:
        origin = ""
    if arrival:
        if not visa:
            return None
        origin = world_mesh.visa_unverified_home(visa)
    worlds = await world_mesh_directory(env)
    home = world_mesh.neighbor_for_origin(env, worlds, origin)
    if not home:
        return None
    visitor = {"home": home["host"], "claim": {}}
    if not visa:
        return visitor
    if world_mesh.visa_unverified_home(visa) != home["world"]:
        return {"error": "invalid_visa"}
    destination = world_mesh.own_world_origin(env)
    try:
        resp = await js_fetch_with_timeout(
            home["api"] + "/api/world/visa/verify",
            {
                "method": "POST",
                "headers": {
                    "accept": "application/json",
                    "content-type": "application/json",
                },
                "body": json.dumps(
                    {"visa": visa, "destination": destination},
                    separators=(",", ":")),
                "redirect": "error",
            },
            WORLD_MESH_FETCH_TIMEOUT_SECONDS)
        if int(getattr(resp, "status", 0)) != 200:
            return {"error": "invalid_visa"}
        payload = json.loads(str(await resp.text())[:4096])
    except Exception:
        return {"error": "visa_unverified"}
    if not isinstance(payload, dict) or payload.get("ok") is not True:
        return {"error": "invalid_visa"}
    name = world_protocol.clean_display_name(payload.get("name"), "")
    nonce = str(payload.get("nonce") or "")
    if not name or not world_mesh.NONCE_RE.fullmatch(nonce):
        return {"error": "invalid_visa"}
    visitor["claim"] = {
        "name": name,
        "accountStatus": world_mesh.VISITOR_STATUS,
        "nodeCount": 0,
        "isAdmin": False,
        "ticketNonce": nonce,
    }
    return visitor
