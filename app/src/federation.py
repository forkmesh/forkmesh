"""federation routes, loaded on demand.

Split out of entry.py so Cloudflare's startup-memory validation (error
10021) does not pay for this domain on every isolate; entry.py loads it
through a _LazyModule proxy on the first request that needs it.
"""


def _bind_runtime(runtime):
    """Supply the entrypoint primitives used by this domain."""
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


def _federation_canonical(pubkey, ts, body_hash):
    return (FEDERATION_CANON + "\n" + pubkey + "\n" + str(ts) + "\n" +
            body_hash).encode()
async def _federation_register(env, request):
    if not _is_main_relay(env):
        return json_response({"error": "not_main_relay"}, status=404)
    body = await request.text()
    pubkey = await _verify_relay_sig(env, request, body)
    if not pubkey:
        return json_response({"error": "bad_signature"}, status=401)
    try:
        data = json.loads(body or "{}")
    except Exception:
        data = {}
    relay_bi = await blind_index(env, pubkey)
    label = clean_string(data.get("label", ""), 80)
    base_url = clean_string(data.get("baseUrl", ""), 200)
    now = int(Date.now())
    existing = await d1_first(
        env, "SELECT status FROM relays WHERE relay_bi=?", relay_bi)
    if existing:
        await d1_run(
            env, "UPDATE relays SET pubkey=?, label=?, base_url=? WHERE relay_bi=?",
            pubkey, label, base_url, relay_bi)
        return json_response({"ok": True, "status": existing.get("status", "pending")})
    await d1_run(
        env,
        "INSERT INTO relays (relay_bi, pubkey, label, base_url, status, "
        "registered_at) VALUES (?,?,?,?, 'pending', ?)",
        relay_bi, pubkey, label, base_url, now)
    return json_response({"ok": True, "status": "pending"})
async def _federation_announce(env, request):
    """Register with the main relay now instead of on the next staggered cron.

    A freshly deployed federated instance is POSTed here (on its own origin)
    by the launch bootstrap the moment it passes its health check, so its
    request to join shows up on the main relay — and as the red dot in the
    operator's desktop — immediately. Unauthenticated by design: the request
    body is ignored and the only effect is re-sending this relay's own signed
    registration upstream (an idempotent upsert there), throttled per isolate.
    """
    del request
    if _is_main_relay(env):
        return json_response({"error": "not_federated_relay"}, status=404)
    now = int(Date.now())
    if now - _federation_announce_at[0] < _FEDERATION_ANNOUNCE_MIN_MS:
        return json_response({"ok": True, "status": "throttled"},
                             cache_control="no-store")
    _federation_announce_at[0] = now
    label = clean_string(getattr(env, "RELAY_LABEL", "") or "", 80)
    base = clean_string(getattr(env, "PUBLIC_BASE_URL", "") or "", 200)
    reply = await _call_main_relay(
        env, "/api/federation/register", {"label": label, "baseUrl": base})
    if not reply or not reply.get("ok"):
        return json_response(
            {"error": "main_relay_unreachable"}, status=502,
            extra_headers=EXPECTED_DEGRADED_HEADERS)
    return json_response(
        {"ok": True,
         "status": clean_string(reply.get("status", "") or "pending", 20)},
        cache_control="no-store")
async def _federation_donation_address(env, request):
    del env, request
    return json_response({
        "error": "legacy_custody_disabled",
        "custody": "self-custodial-only",
        "notice": (
            "ForkMesh signup is free. The legacy federated deposit-wallet "
            "flow is disabled; no Worker creates or retains a wallet key."
        ),
    }, status=410, cache_control="no-store")
async def _federation_donation_status(env, request):
    if not _is_main_relay(env):
        return json_response({"error": "not_main_relay"}, status=404)
    body = await request.text()
    relay_bi = await _relay_authorized(env, request, body)
    if not relay_bi:
        return json_response({"error": "relay_not_approved"}, status=401)
    try:
        data = json.loads(body or "{}")
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    reference = clean_string(data.get("reference", ""), 64)
    row = await d1_first(
        env, "SELECT data FROM federated_signup WHERE reference=? AND relay_bi=?",
        reference, relay_bi)
    if not row:
        return json_response({"error": "no_such_signup"}, status=404)
    rec = await decrypt_row(env, row.get("data"))
    if not rec:
        return json_response({"error": "no_such_signup"}, status=404)
    required = int(rec.get("donation_required_lamports", MIN_JOIN_LAMPORTS))
    balance = await _solana_balance_lamports(env, rec.get("donation_address", ""))
    received = (
        int(rec.get("donation_received_lamports", 0) or 0)
        if balance is None else int(balance)
    )
    return json_response({
        "ok": True,
        "paid": bool(rec.get("donation_confirmed")) or
                (required > 0 and received >= required),
        "receivedLamports": received,
        "requiredLamports": required,
        "checking": balance is None,
        "sweepSig": rec.get("donation_sweep_sig", ""),
        "status": "legacy_custody_frozen",
        "custody": "migration-required",
        "notice": (
            "Read-only legacy status: no Worker can use the retained key "
            "material to sign or move funds. Reconcile and migrate it through "
            "the documented offline custody process."
        ),
    }, cache_control="no-store")
async def _federation_nodes(env, request):
    if not _is_main_relay(env):
        return json_response({"error": "not_main_relay"}, status=404)
    try:
        announced = int(request.headers.get("content-length") or 0)
    except Exception:
        announced = 0
    if announced > FEDERATION_NODES_MAX_BYTES:
        return json_response({"error": "request_too_large"}, status=413)
    body = await request.text()
    if len(body.encode("utf-8")) > FEDERATION_NODES_MAX_BYTES:
        return json_response({"error": "request_too_large"}, status=413)
    relay_bi = await _relay_authorized(env, request, body)
    if not relay_bi:
        return json_response({"error": "relay_not_approved"}, status=401)
    try:
        data = json.loads(body or "{}")
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    nodes = data.get("nodes") or []
    now = int(Date.now())
    freshness = _reward_config(env)["healthFreshMs"]
    expected_refs = await _https_mirror_expected_forkmesh_refs(env)
    count = 0
    rejected = 0
    for node in nodes[:500]:
        item = await _verified_federated_reward_attestation(
            env, node, now, expected_refs, freshness)
        if not item:
            rejected += 1
            continue
        operator_id = await blind_index(
            env,
            "federated-reward-operator:"
            + str(relay_bi) + ":" + item["operator"],
        )
        existing = await d1_first(
            env,
            "SELECT first_verified_at,last_verified_at,consecutive_checks "
            "FROM federated_presence WHERE relay_bi=? AND name=? "
            "ORDER BY last_verified_at DESC LIMIT 1",
            relay_bi, item["name"],
        )
        last_verified = int((existing or {}).get("last_verified_at") or 0)
        if last_verified and now - last_verified <= REWARD_OBSERVATION_GAP_MS:
            first_verified = int(
                (existing or {}).get("first_verified_at") or now)
            consecutive = int(
                (existing or {}).get("consecutive_checks") or 0) + 1
        else:
            first_verified = now
            consecutive = 1
        attestation_id = hashlib.sha256(
            (
                str(relay_bi) + "\0" + item["healthMessage"]
                + "\0" + item["healthSignature"]
            ).encode("utf-8")
        ).hexdigest()
        # One node identity cannot retain an older wallet row alongside its
        # current report; duplicate-wallet/operator/device checks still apply
        # globally when the reward snapshot is frozen.
        await d1_run(
            env,
            "DELETE FROM federated_presence "
            "WHERE relay_bi=? AND name=? AND wallet<>?",
            relay_bi, item["name"], item["wallet"],
        )
        await d1_run(
            env,
            "INSERT INTO federated_presence "
            "(relay_bi,wallet,name,operator_id,device_id,public_key,base_url,"
            "registration_sig,registration_issued_at,health_message,health_sig,"
            "checked_at,healthy,integrity,forkmesh_active,forkmesh_verified_at,"
            "forkmesh_refs_sha256,forkmesh_operations_sha256,abuse_blocked,"
            "attestation_id,first_verified_at,last_verified_at,"
            "consecutive_checks,ts) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,1,'ok',1,?,?,?,0,?,?,?,?,?,?) "
            "ON CONFLICT(relay_bi,wallet) DO UPDATE SET "
            "name=excluded.name,operator_id=excluded.operator_id,"
            "device_id=excluded.device_id,public_key=excluded.public_key,"
            "base_url=excluded.base_url,"
            "registration_sig=excluded.registration_sig,"
            "registration_issued_at=excluded.registration_issued_at,"
            "health_message=excluded.health_message,"
            "health_sig=excluded.health_sig,checked_at=excluded.checked_at,"
            "healthy=excluded.healthy,integrity=excluded.integrity,"
            "forkmesh_active=excluded.forkmesh_active,"
            "forkmesh_verified_at=excluded.forkmesh_verified_at,"
            "forkmesh_refs_sha256=excluded.forkmesh_refs_sha256,"
            "forkmesh_operations_sha256="
            "excluded.forkmesh_operations_sha256,"
            "abuse_blocked=excluded.abuse_blocked,"
            "attestation_id=excluded.attestation_id,"
            "first_verified_at=excluded.first_verified_at,"
            "last_verified_at=excluded.last_verified_at,"
            "consecutive_checks=excluded.consecutive_checks,ts=excluded.ts",
            relay_bi,
            item["wallet"],
            item["name"],
            operator_id,
            item["deviceId"],
            item["publicKey"],
            item["baseUrl"],
            item["registrationSignature"],
            item["registrationIssuedAt"],
            item["healthMessage"],
            item["healthSignature"],
            item["checkedAt"],
            item["forkmeshVerifiedAt"],
            item["refsSha256"],
            item["operationsSha256"],
            attestation_id,
            first_verified,
            now,
            consecutive,
            now,
        )
        count += 1
    return json_response({
        "ok": True,
        "count": count,
        "rejected": rejected,
        "eligibility": "signed-fresh-forkmesh-mirror-attestations-only",
    })
async def federation_handler(env, request):
    # Solana payout relay mesh. Canonical routes live under /api/relay-mesh/*;
    # /api/federation/* is the legacy alias kept so not-yet-updated federated
    # relays keep working (normalized to the legacy names matched below).
    await ensure_schema(env)
    url = urlparse(request.url)
    if method_name(request) != "POST":
        return json_response({"error": "method_not_allowed"}, status=405)
    if url.path.startswith("/api/relay-mesh/"):
        url = url._replace(path=url.path.replace(
            "/api/relay-mesh/", "/api/federation/", 1))
    if url.path == "/api/federation/register":
        return await _federation_register(env, request)
    if url.path == "/api/federation/announce":
        return await _federation_announce(env, request)
    if url.path == "/api/federation/donation-address":
        return await _federation_donation_address(env, request)
    if url.path == "/api/federation/donation-status":
        return await _federation_donation_status(env, request)
    if url.path == "/api/federation/nodes":
        return await _federation_nodes(env, request)
    if url.path == "/api/federation/treasury-address":
        return await _federation_treasury_address(env, request)
    if url.path == "/api/federation/central-fund":
        return await _federation_central_fund(env, request)
    return json_response({"error": "not_found"}, status=404)
async def _federation_treasury_address(env, request):
    """Frozen alias for the removed Worker-custodied treasury flow."""
    del env, request
    return _legacy_treasury_disabled_response()
async def _federation_central_fund(env, request):
    """Return only canonical non-custodial community-pool public metadata."""
    if not _is_main_relay(env):
        return json_response({"error": "not_main_relay"}, status=404)
    payload = await _central_fund_public(
        env, refresh=_wants_balance_refresh(request))
    if not payload or not payload.get("address"):
        return json_response(
            {"error": "central_fund_unavailable"}, status=503,
            extra_headers=EXPECTED_DEGRADED_HEADERS)
    return json_response(payload)
async def _federation_report_nodes(env):
    # Federated relay: forward only evidence this relay independently verified:
    # account-bound registration plus the exact fresh node-signed
    # forkmesh/forkmesh health challenge. A wallet-only presence report is
    # deliberately impossible.
    now = int(Date.now())
    cutoff = now - ACCOUNT_PRESENCE_STALE_MS
    health_cutoff = now - _reward_config(env)["healthFreshMs"]
    rows = await d1_all(
        env,
        "SELECT e.node_bi,e.node_name,e.base_url,e.public_key,"
        "e.registration_sig,e.issued_at,e.checked_at,e.integrity,"
        "e.health_sig,e.health_message,e.forkmesh_verified_at,"
        "e.forkmesh_refs_sha256,e.forkmesh_operations_sha256,"
        "e.abuse_blocked,n.data "
        "FROM mirror_https_endpoints e "
        "JOIN account_presence p ON p.name_bi=e.node_bi "
        "JOIN nodes n ON n.node_bi=e.node_bi "
        "WHERE p.ts>=? AND e.checked_at>=? "
        "AND e.forkmesh_verified_at>=? AND e.healthy=1 "
        "AND e.integrity='ok' AND e.forkmesh_active=1 "
        "AND e.abuse_blocked=0 AND e.health_message<>'' "
        "ORDER BY e.checked_at DESC LIMIT 500",
        cutoff, health_cutoff, health_cutoff,
    )
    nodes = []
    seen = set()
    for r in (rows or []):
        rec = await decrypt_row(env, r.get("data"))
        if not rec or rec.get("status") != "active":
            continue
        wallet = (rec.get("solana") or "").strip()
        if not wallet or not SOLANA_RE.match(wallet) or wallet in seen:
            continue
        seen.add(wallet)
        nodes.append({
            "name": clean_string(
                r.get("node_name") or rec.get("name", ""),
                MAX_NODE_NAME).lower(),
            "operator": clean_string(
                rec.get("owner") or rec.get("name", ""),
                MAX_NODE_NAME).lower(),
            "wallet": wallet,
            "publicKey": str(r.get("public_key") or ""),
            "baseUrl": str(r.get("base_url") or ""),
            "registrationSignature": str(
                r.get("registration_sig") or ""),
            "registrationIssuedAt": int(r.get("issued_at") or 0),
            "healthMessage": str(r.get("health_message") or ""),
            "healthSignature": str(r.get("health_sig") or ""),
            "checkedAt": int(r.get("checked_at") or 0),
            "healthy": True,
            "integrity": "ok",
            "mirrorsForkMesh": True,
            "forkmeshVerifiedAt": int(
                r.get("forkmesh_verified_at") or 0),
            "refsSha256": str(r.get("forkmesh_refs_sha256") or ""),
            "operationsSha256": str(
                r.get("forkmesh_operations_sha256") or ""),
            "requiredOperationsVerified": True,
            "abuseFlagged": False,
        })
    if nodes:
        await _call_main_relay(env, "/api/federation/nodes", {"nodes": nodes})
async def _federation_cron(env):
    if not _is_main_relay(env):
        ident = await _relay_identity(env)
        if ident:
            label = clean_string(getattr(env, "RELAY_LABEL", "") or "", 80)
            base = clean_string(getattr(env, "PUBLIC_BASE_URL", "") or "", 200)
            await _call_main_relay(
                env, "/api/federation/register",
                {"label": label, "baseUrl": base})
            await _federation_report_nodes(env)
        return
    # Main relay only expires stale presence. Legacy federated deposit rows are
    # intentionally untouched: moving their funds requires the offline custody
    # migration and no scheduled Worker path may consume their retained seeds.
    cutoff = int(Date.now()) - ACCOUNT_PRESENCE_STALE_MS
    try:
        await d1_run(env, "DELETE FROM federated_presence WHERE ts < ?", cutoff)
    except Exception:
        pass
