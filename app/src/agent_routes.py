"""agent routes routes, loaded on demand.

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


async def _ring_agent_readers(env, owner):
    """Tell the owner's other devices the stored sessions changed.

    A payload-free "agents" frame on the account event socket, so the phone's
    Agents tab reloads when a desktop starts or advances a run instead of
    waiting for a pull to refresh. Only a changed snapshot rings: the 30s
    safety-net re-push short-circuits on its digest above. Best-effort — the
    snapshot is already stored.
    """
    try:
        await notify_account_event(env, owner, "agents")
    except Exception:
        pass


async def agents_handler(env, request, owner, repo):
    await ensure_schema(env)
    method = method_name(request)
    repo_bi = await blind_index(env, owner + "/" + repo)
    policy_reader = globals().get("_repo_privacy_policy")
    policy = (
        await policy_reader(env, owner, repo)
        if callable(policy_reader) else {
            "ownerKeyId": "", "requireAgentE2EE": True,
            "requireMirrorEncryption": True,
        }
    )

    if method == "POST":
        if not await _authorize_owner(env, request, owner):
            return json_response({"error": "unauthorized"}, status=401)
        try:
            data = await bounded_json_request(request)
        except Exception:
            return json_response({"error": "invalid_json"}, status=400)
        if policy.get("requireAgentE2EE") and not policy.get("ownerKeyId"):
            return json_response({
                "error": "active_owner_key_required",
                "setupPath": "/api/security/owner-keys",
            }, status=428)
        encrypted_in = data.get("encryptedSessions")
        encrypted_rows = []
        if isinstance(encrypted_in, list):
            for item in encrypted_in[:MAX_AGENT_SESSIONS]:
                if not isinstance(item, dict):
                    return json_response(
                        {"error": "invalid_encrypted_session"}, status=400)
                try:
                    agent_id = int(item.get("id", 0))
                except (TypeError, ValueError):
                    agent_id = 0
                owner_crypto = globals().get("security_control")
                envelope = (
                    owner_crypto.validate_owner_envelope(item.get("envelope"))
                    if owner_crypto is not None else None)
                if not agent_id or not envelope:
                    return json_response(
                        {"error": "invalid_encrypted_session"}, status=400)
                # The policy key (the owner desktop's vault) must be able to
                # open every stored session; companion owner devices may be
                # additional recipients of the same envelope.
                required_kid = policy.get("ownerKeyId", "")
                envelope_kids = owner_crypto.owner_envelope_key_ids(envelope)
                if required_kid and required_kid not in envelope_kids:
                    return json_response(
                        {"error": "wrong_owner_key",
                         "requiredKeyId": required_kid},
                        status=409)
                encoded = owner_crypto.encode_owner_envelope(
                    envelope, {"agentId": agent_id})
                encrypted_rows.append((str(agent_id), encoded))
        if policy.get("requireAgentE2EE") and not isinstance(encrypted_in, list):
            return json_response(
                {
                    "error": "owner_encryption_required",
                    "requiredKeyId": policy.get("ownerKeyId", ""),
                },
                status=426,
            )
        if isinstance(encrypted_in, list):
            # Full replacement is safe because every row remains opaque to the
            # relay and its administrators. Duplicate ids keep the last entry.
            by_id = {agent_id: encoded for agent_id, encoded in encrypted_rows}
            encrypted_rows = list(by_id.items())
            digest = hashlib.sha256(
                "\n".join(encoded for _, encoded in encrypted_rows).encode()
            ).hexdigest()
            if _AGENT_PUSH_DIGESTS.get(repo_bi) == digest:
                return json_response({
                    "ok": True,
                    "unchanged": True,
                    "privacyBoundary": "owner-only-e2ee",
                })
            await d1_run(env, "DELETE FROM repo_agents WHERE repo_bi=?", repo_bi)
            now = int(Date.now())
            for start in range(0, len(encrypted_rows), 20):
                chunk = encrypted_rows[start:start + 20]
                placeholders = ",".join("(?,?,?,?)" for _ in chunk)
                args = []
                for agent_id, encoded in chunk:
                    args.extend((repo_bi, agent_id, encoded, now))
                if chunk:
                    await d1_run(
                        env,
                        "INSERT INTO repo_agents "
                        "(repo_bi, agent_id, data, updated_at) VALUES "
                        + placeholders,
                        *args)
            _AGENT_PUSH_DIGESTS[repo_bi] = digest
            if len(_AGENT_PUSH_DIGESTS) > 2000:
                _AGENT_PUSH_DIGESTS.clear()
            await _ring_agent_readers(env, owner)
            return json_response({
                "ok": True,
                "privacyBoundary": "owner-only-e2ee",
                "serverCanDecrypt": False,
            })

        sessions_in = data.get("sessions")
        if not isinstance(sessions_in, list):
            sessions_in = []
        # Cap to the most recent MAX_AGENT_SESSIONS entries rather than
        # rejecting the whole push outright (same truncate-not-reject
        # convention as the labels/assignees lists in issues_handler).
        sessions = [s for s in
                    (_clean_agent_session(item) for item in sessions_in[:MAX_AGENT_SESSIONS]) if s]
        # Dedupe by id, keeping the last occurrence: repo_agents is keyed on
        # (repo_bi, agent_id), so a payload with a repeated id would otherwise
        # violate that primary key on the second INSERT below.
        by_id = {}
        for session in sessions:
            by_id[session["id"]] = session
        sessions = list(by_id.values())
        # No-op short-circuit: the desktop's 30s safety-net timer mostly
        # re-pushes an identical snapshot. Re-encrypting and rewriting up to
        # MAX_AGENT_SESSIONS rows (hundreds of AES-GCM ops + inserts) for a
        # byte-identical payload was the single strongest CPU spike in the
        # 2026-07-11 free-plan overload. The digest memo is per-isolate: a
        # cold isolate rewrites once, then skips.
        digest = hashlib.sha256(
            json.dumps(sessions, sort_keys=True,
                       separators=(",", ":")).encode()).hexdigest()
        if _AGENT_PUSH_DIGESTS.get(repo_bi) == digest:
            return json_response({"ok": True, "unchanged": True})
        # Full-replace semantics: the desktop always pushes its whole current
        # view of this repo's sessions, so the stored set is exactly that.
        await d1_run(env, "DELETE FROM repo_agents WHERE repo_bi=?", repo_bi)
        now = int(Date.now())
        # Multi-row INSERT (chunked under D1's bound-parameter limit) instead
        # of one awaited roundtrip per session.
        encrypted = []
        for session in sessions:
            encrypted.append(
                (str(session["id"]), await encrypt_row(env, session)))
        for start in range(0, len(encrypted), 20):
            chunk = encrypted[start:start + 20]
            placeholders = ",".join("(?,?,?,?)" for _ in chunk)
            args = []
            for agent_id, enc in chunk:
                args.extend((repo_bi, agent_id, enc, now))
            await d1_run(
                env,
                "INSERT INTO repo_agents (repo_bi, agent_id, data, updated_at)"
                " VALUES " + placeholders,
                *args)
        _AGENT_PUSH_DIGESTS[repo_bi] = digest
        if len(_AGENT_PUSH_DIGESTS) > 2000:
            _AGENT_PUSH_DIGESTS.clear()
        await _ring_agent_readers(env, owner)
        # Keep the legacy response shape for older Qt nodes. The privacy
        # boundary is advertised by the policy/list endpoints; adding fields
        # here broke clients that compare this acknowledgement exactly.
        return json_response({"ok": True})

    if method == "GET":
        if not await _authorize_owner(env, request, owner):
            return json_response({"error": "unauthorized"}, status=401)
        if policy.get("requireAgentE2EE") and not policy.get("ownerKeyId"):
            return json_response({
                "error": "active_owner_key_required",
                "setupPath": "/api/security/owner-keys",
            }, status=428)
        rows = await d1_all(
            env,
            "SELECT id, data FROM agent_prompts WHERE repo_bi=? ORDER BY id ASC",
            repo_bi,
        )
        prompts = []
        encrypted_prompts = []
        legacy_rows = []
        legacy_held = 0
        for row in rows or []:
            owner_crypto = globals().get("security_control")
            sealed = (
                owner_crypto.decode_owner_envelope(row.get("data"))
                if owner_crypto is not None else None)
            if sealed:
                routing = sealed.get("routing") or {}
                encrypted_prompts.append({
                    "queueId": int(row.get("id") or 0),
                    "agentId": int(routing.get("agentId") or 0),
                    "queuedAt": int(routing.get("queuedAt") or 0),
                    "envelope": sealed["envelope"],
                })
                continue
            if policy.get("requireAgentE2EE"):
                # Never decrypt pre-boundary prompt rows in the Worker. They
                # remain held for an explicit offline owner migration.
                legacy_held += 1
                continue
            rec = await decrypt_row(env, row.get("data"))
            if rec:
                rec["queueId"] = int(row.get("id") or 0)
                prompts.append(rec)
            legacy_rows.append(row)
        # Legacy clients keep drain-on-read compatibility. Owner-sealed prompts
        # require an explicit ack after local decryption, so a wrong/revoked key
        # cannot destroy the only queued ciphertext.
        if legacy_rows:
            await d1_run(
                env,
                "DELETE FROM agent_prompts WHERE id IN (%s)"
                % ",".join("?" for _ in legacy_rows),
                *[r["id"] for r in legacy_rows])
        return json_response({
            "ok": True,
            "prompts": prompts,
            "encryptedPrompts": encrypted_prompts,
            "ackPath": "/api/repo/%s/%s/agents/ack" % (owner, repo),
            "legacyRowsHeldForOwnerMigration": legacy_held,
            "privacyBoundary": (
                "owner-only-e2ee" if policy.get("requireAgentE2EE")
                else "owner-only-e2ee" if encrypted_prompts and not prompts
                else "mixed" if encrypted_prompts else "legacy-server-readable"),
            "serverCanDecryptEncryptedPrompts": False,
        })

    return json_response({"error": "method_not_allowed"}, status=405)
async def agents_list_handler(env, request, owner, repo):
    await ensure_schema(env)
    if method_name(request) != "POST":
        return json_response({"error": "method_not_allowed"}, status=405)
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    ok, err = await _authorize_owner_account(
        env, owner, data, request, allow_admin=False)
    if not ok:
        return err
    policy_reader = globals().get("_repo_privacy_policy")
    policy = (
        await policy_reader(env, owner, repo)
        if callable(policy_reader) else {
            "ownerKeyId": "", "requireAgentE2EE": True,
            "requireMirrorEncryption": True,
        }
    )
    repo_bi = await blind_index(env, owner + "/" + repo)
    rows = await d1_all(
        env, "SELECT data FROM repo_agents WHERE repo_bi=? ORDER BY updated_at DESC",
        repo_bi,
    )
    agents = []
    encrypted_agents = []
    legacy_held = 0
    for row in rows or []:
        owner_crypto = globals().get("security_control")
        sealed = (
            owner_crypto.decode_owner_envelope(row.get("data"))
            if owner_crypto is not None else None)
        if sealed:
            routing = sealed.get("routing") or {}
            encrypted_agents.append({
                "id": int(routing.get("agentId") or 0),
                "envelope": sealed["envelope"],
            })
            continue
        if policy.get("requireAgentE2EE"):
            legacy_held += 1
            continue
        rec = await decrypt_row(env, row.get("data"))
        if rec:
            agents.append(rec)
    # The transcript tail can be large; keep it out of the list payload and let
    # the detail page fetch just the selected agent's transcript on demand.
    for rec in agents:
        rec.pop("transcript", None)
    return json_response({
        "ok": True,
        "agents": agents,
        "encryptedAgents": encrypted_agents,
        "legacyRowsHeldForOwnerMigration": legacy_held,
        "privacyBoundary": (
            "owner-only-e2ee" if policy.get("requireAgentE2EE")
            else "owner-only-e2ee" if encrypted_agents and not agents
            else "mixed" if encrypted_agents else "legacy-server-readable"),
        "serverCanDecryptEncryptedAgents": False,
    }, cache_control="no-store")
async def agents_transcript_handler(env, request, owner, repo, agent_id):
    await ensure_schema(env)
    if method_name(request) != "POST":
        return json_response({"error": "method_not_allowed"}, status=405)
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    ok, err = await _authorize_owner_account(
        env, owner, data, request, allow_admin=False)
    if not ok:
        return err
    policy_reader = globals().get("_repo_privacy_policy")
    policy = (
        await policy_reader(env, owner, repo)
        if callable(policy_reader) else {
            "ownerKeyId": "", "requireAgentE2EE": True,
            "requireMirrorEncryption": True,
        }
    )
    repo_bi = await blind_index(env, owner + "/" + repo)
    row = await d1_first(
        env, "SELECT data FROM repo_agents WHERE repo_bi=? AND agent_id=?",
        repo_bi, str(agent_id),
    )
    owner_crypto = globals().get("security_control")
    sealed = (
        owner_crypto.decode_owner_envelope(row.get("data"))
        if row and owner_crypto is not None else None)
    if sealed:
        return json_response({
            "ok": True,
            "encrypted": True,
            "envelope": sealed["envelope"],
            "privacyBoundary": "owner-only-e2ee",
            "serverCanDecrypt": False,
        }, cache_control="no-store")
    if policy.get("requireAgentE2EE") and row:
        return json_response({
            "error": "legacy_data_unavailable",
            "migration": "owner-device-required",
        }, status=410, cache_control="no-store")
    rec = await decrypt_row(env, row["data"]) if row else None
    if not rec:
        return json_response({"error": "not_found"}, status=404)
    return json_response({
        "ok": True,
        "transcript": rec.get("transcript", ""),
        "status": rec.get("status", ""),
        "privacyBoundary": "legacy-server-readable",
        "serverCanDecrypt": True,
    })
async def agents_prompt_handler(env, request, owner, repo, agent_id):
    await ensure_schema(env)
    if method_name(request) != "POST":
        return json_response({"error": "method_not_allowed"}, status=405)
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    ok, err = await _authorize_owner_account(
        env, owner, data, request, allow_admin=False)
    if not ok:
        return err
    policy_reader = globals().get("_repo_privacy_policy")
    policy = (
        await policy_reader(env, owner, repo)
        if callable(policy_reader) else {
            "ownerKeyId": "", "requireAgentE2EE": True,
            "requireMirrorEncryption": True,
        }
    )
    owner_crypto = globals().get("security_control")
    if policy.get("requireAgentE2EE") and not policy.get("ownerKeyId"):
        return json_response({
            "error": "active_owner_key_required",
            "setupPath": "/api/security/owner-keys",
        }, status=428)
    envelope = (
        owner_crypto.validate_owner_envelope(data.get("envelope"))
        if owner_crypto is not None else None)
    if envelope:
        # A queued prompt is executed by the owner desktop, so the envelope
        # must be openable by the policy key; sealing to further enrolled
        # owner devices alongside it is allowed.
        required_kid = policy.get("ownerKeyId", "")
        envelope_kids = owner_crypto.owner_envelope_key_ids(envelope)
        if required_kid and required_kid not in envelope_kids:
            return json_response(
                {"error": "wrong_owner_key", "requiredKeyId": required_kid},
                status=409)
        repo_bi = await blind_index(env, owner + "/" + repo)
        count = await d1_first(
            env,
            "SELECT COUNT(*) AS c FROM agent_prompts WHERE repo_bi=?",
            repo_bi,
        )
        if count and count.get("c", 0) >= MAX_PENDING_AGENT_PROMPTS:
            return json_response({"error": "prompt_queue_full"}, status=429)
        now = int(Date.now())
        encoded = owner_crypto.encode_owner_envelope(
            envelope, {"agentId": agent_id, "queuedAt": now})
        await d1_run(
            env,
            "INSERT INTO agent_prompts (repo_bi, agent_id, data, queued_at) "
            "VALUES (?,?,?,?)",
            repo_bi, agent_id, encoded, now,
        )
        await notify_repo_host(env, owner, repo, "agents")
        return json_response({
            "ok": True,
            "privacyBoundary": "owner-only-e2ee",
            "serverCanDecrypt": False,
        })
    if policy.get("requireAgentE2EE"):
        return json_response(
            {
                "error": "owner_encryption_required",
                "requiredKeyId": policy.get("ownerKeyId", ""),
            },
            status=426,
        )
    text = str(data.get("text", "") or "").strip()
    if not text:
        return json_response({"error": "text_required"}, status=400)
    if len(text) > MAX_AGENT_PROMPT_TEXT:
        return json_response({"error": "text_too_long"}, status=400)
    repo_bi = await blind_index(env, owner + "/" + repo)
    count = await d1_first(
        env, "SELECT COUNT(*) AS c FROM agent_prompts WHERE repo_bi=?", repo_bi
    )
    if count and count.get("c", 0) >= MAX_PENDING_AGENT_PROMPTS:
        return json_response({"error": "prompt_queue_full"}, status=429)
    now = int(Date.now())
    item = {"agentId": agent_id, "text": text, "queuedAt": now}
    # Optional agent-provider choice from the website's top-of-list composer
    # (adhoc #271): only meaningful when starting a brand-new agent, and only a
    # known provider is honored — else the node falls back to its own default.
    provider_in = clean_string(data.get("provider", ""), 40)
    if provider_in in ("claude-code", "claude-api", "openai", "local-llama"):
        item["provider"] = provider_in
    # Optional agent-model choice (adhoc #276): lets the owner choose which
    # Claude model the agent uses (e.g. "claude-opus-5", "claude-sonnet-5",
    # "claude-haiku-4-5", "fable-5"). Only meaningful when starting a brand-new
    # agent, and empty leaves the provider's own default in place.
    model_in = clean_string(data.get("model", ""), 60)
    if model_in:
        item["model"] = model_in
    # Optional pasted/attached screenshots (adhoc #78): data: URLs the node
    # writes into the agent's working tree so the agent can see them. Only
    # meaningful when starting a brand-new agent; bounded in count and size.
    images_in = data.get("images", [])
    if isinstance(images_in, list) and images_in:
        images = []
        total = 0
        for entry in images_in:
            if len(images) >= MAX_AGENT_PROMPT_IMAGES:
                break
            src = entry if isinstance(entry, str) else ""
            if not src.startswith("data:image/"):
                continue
            if len(src) > MAX_AGENT_PROMPT_IMAGE_BYTES:
                return json_response({"error": "image_too_large"}, status=400)
            total += len(src)
            if total > MAX_AGENT_PROMPT_IMAGES_TOTAL_BYTES:
                return json_response({"error": "images_too_large"}, status=400)
            images.append(src)
        if images:
            item["images"] = images
    await d1_run(
        env,
        "INSERT INTO agent_prompts (repo_bi, agent_id, data, queued_at) "
        "VALUES (?,?,?,?)",
        repo_bi, agent_id, await encrypt_row(env, item), now,
    )
    await notify_repo_host(env, owner, repo, "agents")
    return json_response({"ok": True})
async def agents_ack_handler(env, request, owner, repo):
    """Delete only owner-sealed prompts accepted by the owner's local client."""
    await ensure_schema(env)
    if method_name(request) != "POST":
        return json_response({"error": "method_not_allowed"}, status=405)
    if not await _authorize_owner(env, request, owner):
        return json_response({"error": "unauthorized"}, status=401)
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    raw_ids = data.get("queueIds")
    if not isinstance(raw_ids, list):
        return json_response({"error": "queue_ids_required"}, status=400)
    queue_ids = []
    for value in raw_ids[:MAX_PENDING_AGENT_PROMPTS]:
        try:
            value = int(value)
        except (TypeError, ValueError):
            continue
        if value > 0 and value not in queue_ids:
            queue_ids.append(value)
    if not queue_ids:
        return json_response({"ok": True, "acknowledged": 0})
    repo_bi = await blind_index(env, owner + "/" + repo)
    rows = await d1_all(
        env,
        "SELECT id, data FROM agent_prompts WHERE repo_bi=? AND id IN (%s)"
        % ",".join("?" for _ in queue_ids),
        repo_bi, *queue_ids,
    )
    sealed_ids = [
        int(row.get("id") or 0) for row in rows or []
        if (globals().get("security_control") is not None
            and globals()["security_control"].decode_owner_envelope(
                row.get("data")))
    ]
    if sealed_ids:
        await d1_run(
            env,
            "DELETE FROM agent_prompts WHERE repo_bi=? AND id IN (%s)"
            % ",".join("?" for _ in sealed_ids),
            repo_bi, *sealed_ids,
        )
    return json_response({"ok": True, "acknowledged": len(sealed_ids)})
