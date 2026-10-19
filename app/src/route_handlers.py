"""Request-lazy App route handlers that run inside bounded Durable Objects.

These handlers are not part of the stateless control fast path. entry.py binds
its initialized runtime here only after routing has entered the App ingress or
a scheduled Durable Object, keeping their bytecode out of Worker startup while
preserving the existing single-Worker API surface.
"""


def _bind_runtime(runtime):
    """Supply the initialized entrypoint primitives used by these handlers."""
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


_ORG_TASK_COMPLETION_HISTORY_LIMIT = 20
_ORG_AGENT_RUN_MODES = {
    "auto": "Auto", "ask": "Ask", "plan": "Plan", "edit": "Edit",
}
_ORG_AGENT_RUN_EFFORTS = ("low", "medium", "high", "xhigh", "max")
_ORG_TASK_COMPLETION_NOTE_LIMIT = 4000


def _org_task_completion_history(value):
    if not isinstance(value, list):
        return []
    history = []
    for item in value[-_ORG_TASK_COMPLETION_HISTORY_LIMIT:]:
        if not isinstance(item, dict):
            continue
        try:
            completed_at = max(0, int(item.get("completedAt") or 0))
        except (TypeError, ValueError, OverflowError):
            completed_at = 0
        note = clean_string(
            item.get("note"), _ORG_TASK_COMPLETION_NOTE_LIMIT)
        if not completed_at and not note:
            continue
        qa_status = str(item.get("qaStatus") or "unknown").lower()
        if qa_status not in ("unknown", "passed", "failed"):
            qa_status = "unknown"
        try:
            qa_reviewed_at = max(0, int(item.get("qaReviewedAt") or 0))
        except (TypeError, ValueError, OverflowError):
            qa_reviewed_at = 0
        entry = {
            "completedAt": completed_at,
            "completedBy": clean_string(item.get("completedBy"), 64).lower(),
            "note": note,
            "qaStatus": qa_status,
            "qaReviewer": clean_string(item.get("qaReviewer"), 64).lower(),
            "qaReviewedAt": qa_reviewed_at,
        }
        try:
            undone_at = max(0, int(item.get("undoneAt") or 0))
        except (TypeError, ValueError, OverflowError):
            undone_at = 0
        if undone_at:
            entry["undoneAt"] = undone_at
            entry["undoneBy"] = clean_string(item.get("undoneBy"), 64).lower()
        history.append(entry)
    return history[-_ORG_TASK_COMPLETION_HISTORY_LIMIT:]


def _org_task_routing_snapshot(row, data):
    kind = str(row.get("assignee_kind") or "user").lower()
    if kind in ("bot", "claude", "codex"):
        kind = "agent"
    if kind not in ("user", "unassigned", "agent"):
        kind = "user"
    destination = str(row.get("destination") or "department").lower()
    if destination not in (
            "department", "personal", "repository", "qa", "agent"):
        destination = "department"
    return {
        "department": clean_string(
            row.get("department") or "general", 64).lower(),
        "team": clean_string(row.get("team"), 64).lower(),
        "destination": destination,
        "assigneeKind": kind,
        "assignee": (
            "agent" if kind == "agent"
            else clean_string(data.get("assignee"), 64).lower()
        ),
        "repository": clean_string(data.get("repository"), 201),
    }


async def _contribution_activity_rows(
        env, subject_user_bi, from_value, to_value):
    return await d1_all(
        env,
        """WITH public_projects AS (
               SELECT projects.*
                 FROM profile_contribution_projects AS projects
                 JOIN repositories
                   ON repositories.key_bi=projects.source_repo_bi
                WHERE projects.is_public=1 AND repositories.is_private=0
             ), candidate_days AS (
               SELECT DISTINCT sources.project_bi, days.day
                 FROM public_projects AS sources
                 JOIN profile_contribution_days AS days
                   ON days.source_repo_bi=sources.source_repo_bi
                  AND days.generation_bi=sources.active_generation_bi
                WHERE sources.active_generation_bi IS NOT NULL
                  AND days.subject_user_bi=?
                  AND days.day>=? AND days.day<=?
             ), ranked_day_sources AS (
               SELECT candidates.project_bi, candidates.day,
                      sources.source_repo_bi,
                      sources.active_generation_bi,
                      ROW_NUMBER() OVER (
                        PARTITION BY candidates.project_bi, candidates.day
                        ORDER BY sources.captured_at DESC,
                                 sources.source_repo_bi ASC) AS source_rank
                 FROM candidate_days AS candidates
                 JOIN public_projects AS sources
                   ON sources.project_bi=candidates.project_bi
                  AND sources.active_generation_bi IS NOT NULL
                  AND sources.verified_from<=candidates.day
             ), active_day_sources AS (
               SELECT * FROM ranked_day_sources WHERE source_rank=1
             ), selected_days AS (
               SELECT sources.project_bi, days.day,
                      days.commits, days.issues,
                      days.pulls, days.reviews, days.source_repo_bi,
                      days.captured_at, days.data
                 FROM active_day_sources AS sources
                 JOIN profile_contribution_days AS days
                   ON days.source_repo_bi=sources.source_repo_bi
                  AND days.generation_bi=sources.active_generation_bi
                  AND days.day=sources.day
                WHERE days.subject_user_bi=?
             ), owned_ranked AS (
               SELECT sources.*,
                      MIN(sources.first_public_day) OVER (
                        PARTITION BY sources.project_bi
                      ) AS logical_first_public_day,
                      ROW_NUMBER() OVER (
                        PARTITION BY sources.project_bi
                        ORDER BY sources.captured_at DESC,
                                 sources.source_repo_bi ASC) AS source_rank
                 FROM public_projects AS sources
                WHERE sources.owner_user_bi=?
             ), owned_projects AS (
               SELECT * FROM owned_ranked WHERE source_rank=1
             ), activity_days AS (
               SELECT day,
                      SUM(commits) AS commits,
                      SUM(issues) AS issues,
                      SUM(pulls) AS pulls,
                      SUM(reviews) AS reviews,
                      0 AS repositories
                 FROM selected_days GROUP BY day
             ), repository_days AS (
               SELECT logical_first_public_day AS day,
                      0 AS commits, 0 AS issues, 0 AS pulls, 0 AS reviews,
                      COUNT(*) AS repositories
                 FROM owned_projects
                WHERE logical_first_public_day>=?
                  AND logical_first_public_day<=?
                GROUP BY logical_first_public_day
             ), combined_days AS (
               SELECT day, SUM(commits) AS commits, SUM(issues) AS issues,
                      SUM(pulls) AS pulls, SUM(reviews) AS reviews,
                      SUM(repositories) AS repositories
                 FROM (
                   SELECT * FROM activity_days
                   UNION ALL
                   SELECT * FROM repository_days
                 ) GROUP BY day
             )
             SELECT 0 AS row_order, 'summary' AS row_kind, '' AS day,
                    COALESCE(SUM(commits), 0) AS commits,
                    COALESCE(SUM(issues), 0) AS issues,
                    COALESCE(SUM(pulls), 0) AS pulls,
                    COALESCE(SUM(reviews), 0) AS reviews,
                    COALESCE(SUM(repositories), 0) AS repositories,
                    (SELECT COUNT(*) FROM owned_projects)
                      AS repository_count
               FROM combined_days
             UNION ALL
             SELECT 1 AS row_order, 'day' AS row_kind, day,
                    commits, issues, pulls, reviews, repositories,
                    0 AS repository_count
               FROM combined_days
             ORDER BY row_order ASC, day ASC""",
        subject_user_bi, from_value, to_value, subject_user_bi,
        subject_user_bi,
        from_value, to_value,
    )


async def _contribution_recent_rows(
        env, subject_user_bi, from_value, to_value):
    return await d1_all(
        env,
        """WITH public_projects AS (
               SELECT projects.*
                 FROM profile_contribution_projects AS projects
                 JOIN repositories
                   ON repositories.key_bi=projects.source_repo_bi
                WHERE projects.is_public=1 AND repositories.is_private=0
             ), candidate_days AS (
               SELECT DISTINCT sources.project_bi, days.day
                 FROM public_projects AS sources
                 JOIN profile_contribution_days AS days
                   ON days.source_repo_bi=sources.source_repo_bi
                  AND days.generation_bi=sources.active_generation_bi
                WHERE sources.active_generation_bi IS NOT NULL
                  AND days.subject_user_bi=?
                  AND days.day>=? AND days.day<=?
             ), ranked_day_sources AS (
               SELECT candidates.project_bi, candidates.day,
                      sources.source_repo_bi,
                      sources.active_generation_bi,
                      ROW_NUMBER() OVER (
                        PARTITION BY candidates.project_bi, candidates.day
                        ORDER BY sources.captured_at DESC,
                                 sources.source_repo_bi ASC) AS source_rank
                 FROM candidate_days AS candidates
                 JOIN public_projects AS sources
                   ON sources.project_bi=candidates.project_bi
                  AND sources.active_generation_bi IS NOT NULL
                  AND sources.verified_from<=candidates.day
             ), active_day_sources AS (
               SELECT * FROM ranked_day_sources WHERE source_rank=1
             ), selected_days AS (
               SELECT days.*
                 FROM active_day_sources AS sources
                 JOIN profile_contribution_days AS days
                   ON days.source_repo_bi=sources.source_repo_bi
                  AND days.generation_bi=sources.active_generation_bi
                  AND days.day=sources.day
                WHERE days.subject_user_bi=?
             ), owned_ranked AS (
               SELECT sources.*,
                      MIN(sources.first_public_day) OVER (
                        PARTITION BY sources.project_bi
                      ) AS logical_first_public_day,
                      ROW_NUMBER() OVER (
                        PARTITION BY sources.project_bi
                        ORDER BY sources.captured_at DESC,
                                 sources.source_repo_bi ASC) AS source_rank
                 FROM public_projects AS sources
                WHERE sources.owner_user_bi=?
             ), owned_projects AS (
               SELECT * FROM owned_ranked WHERE source_rank=1
             ), recent_candidates AS (
               SELECT day AS activity_day, 'commits' AS kind,
                      0 AS kind_order, commits AS activity_count,
                      project_bi, source_repo_bi, data
                 FROM selected_days WHERE commits>0
               UNION ALL
               SELECT day, 'issues', 1, issues,
                      project_bi, source_repo_bi, data
                 FROM selected_days WHERE issues>0
               UNION ALL
               SELECT day, 'pulls', 2, pulls,
                      project_bi, source_repo_bi, data
                 FROM selected_days WHERE pulls>0
               UNION ALL
               SELECT day, 'reviews', 3, reviews,
                      project_bi, source_repo_bi, data
                 FROM selected_days WHERE reviews>0
               UNION ALL
               SELECT logical_first_public_day, 'repositories', 4, 1,
                      project_bi, source_repo_bi, data
                 FROM owned_projects
                WHERE logical_first_public_day>=?
                  AND logical_first_public_day<=?
             )
             SELECT activity_day, kind, activity_count, project_bi,
                    source_repo_bi, data
               FROM recent_candidates
              ORDER BY activity_day DESC, kind_order ASC,
                       project_bi ASC, source_repo_bi ASC
              LIMIT 20""",
        subject_user_bi, from_value, to_value, subject_user_bi,
        subject_user_bi,
        from_value, to_value,
    )


async def _contribution_coverage_rows(
        env, subject_user_bi, from_value, to_value):
    return await d1_all(
        env,
        """WITH public_projects AS (
               SELECT projects.*
                 FROM profile_contribution_projects AS projects
                 JOIN repositories
                   ON repositories.key_bi=projects.source_repo_bi
                WHERE projects.is_public=1 AND repositories.is_private=0
             ), candidate_days AS (
               SELECT DISTINCT sources.project_bi, days.day
                 FROM public_projects AS sources
                 JOIN profile_contribution_days AS days
                   ON days.source_repo_bi=sources.source_repo_bi
                  AND days.generation_bi=sources.active_generation_bi
                WHERE sources.active_generation_bi IS NOT NULL
                  AND days.subject_user_bi=?
                  AND days.day>=? AND days.day<=?
             ), ranked_day_sources AS (
               SELECT candidates.project_bi, candidates.day,
                      sources.source_repo_bi,
                      ROW_NUMBER() OVER (
                        PARTITION BY candidates.project_bi, candidates.day
                        ORDER BY sources.captured_at DESC,
                                 sources.source_repo_bi ASC) AS source_rank
                 FROM candidate_days AS candidates
                 JOIN public_projects AS sources
                   ON sources.project_bi=candidates.project_bi
                  AND sources.active_generation_bi IS NOT NULL
                  AND sources.verified_from<=candidates.day
             ), selected_day_sources AS (
               SELECT * FROM ranked_day_sources WHERE source_rank=1
             ), activity_sources AS (
               SELECT DISTINCT project_bi, source_repo_bi
                 FROM selected_day_sources
             ), owned_projects AS (
               SELECT DISTINCT project_bi
                 FROM public_projects
                WHERE owner_user_bi=?
             ), owned_ranked_sources AS (
               SELECT sources.*,
                      ROW_NUMBER() OVER (
                        PARTITION BY sources.project_bi
                        ORDER BY CASE
                           WHEN sources.active_generation_bi IS NOT NULL
                            AND sources.verified_from IS NOT NULL
                            AND sources.verified_from<=? THEN 0
                           WHEN sources.active_generation_bi IS NOT NULL
                             THEN 1
                           ELSE 2
                         END ASC,
                                 sources.captured_at DESC,
                                 sources.source_repo_bi ASC) AS source_rank
                 FROM public_projects AS sources
                 JOIN owned_projects
                   ON owned_projects.project_bi=sources.project_bi
             ), coverage_source_keys AS (
               SELECT source_repo_bi, project_bi
                 FROM owned_ranked_sources
                WHERE source_rank=1
               UNION
               SELECT source_repo_bi, project_bi FROM activity_sources
             )
             SELECT sources.source_repo_bi, sources.project_bi,
                    sources.owner_user_bi, sources.verified_from,
                    sources.captured_at, sources.data,
                    CASE WHEN owned.project_bi IS NOT NULL
                         THEN 1 ELSE 0 END AS is_owned
               FROM coverage_source_keys AS keys
               JOIN public_projects AS sources
                 ON sources.source_repo_bi=keys.source_repo_bi
               LEFT JOIN owned_projects AS owned
                 ON owned.project_bi=keys.project_bi
              ORDER BY sources.project_bi ASC, sources.source_repo_bi ASC
              LIMIT ?""",
        subject_user_bi, from_value, to_value, subject_user_bi,
        from_value, MAX_CATALOG_REPOS,
    )


async def _account_finalize(env, request):
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    name = clean_string(data.get("nodeName", ""), MAX_NODE_NAME).lower()
    email = clean_string(data.get("email", ""), 254)
    password = (data.get("password", "") or "")[:256]
    pubkey = clean_string(data.get("pubkey", ""), 120)
    solana = clean_string(data.get("solana", ""), 64)
    ts = clean_string(data.get("ts", ""), 20)
    signature = clean_string(data.get("sig", ""), 200)

    name_bi, rec = await _account_row(env, name)
    if not rec:
        return json_response({"error": "no_such_account"}, status=404)
    if not rec.get("pubkey"):
        # Historical keyless reservations must not remain a second web-signup
        # path. A desktop node can first re-reserve the name with its signed
        # key; password-based users can only be created through /signup.
        await log_blocked_signup(
            env, request, NODE_FINALIZE_PATH, 403, "signup_page_required")
        return _signup_page_required_response()

    # Installer link code (adhoc #53): a hosts/SSH install prints a code on the
    # fresh machine and hands it to the daemon, which presents it here when it
    # registers. Read early — a reused node name leans on it as proof below.
    link_code = clean_string(data.get("linkCode", ""), 16).strip()

    # This compatibility endpoint is now exclusively key-bound node setup. A
    # desktop account proves ownership with its Ed25519 signature; optional
    # email/password credentials do not turn the node into a web user.
    key_bound = True
    if not _ts_ok(ts):
        return json_response({"error": "stale_request"}, status=401)
    canonical = ("forkmesh-finalize-v1\n" + name + "\n" + email + "\n" +
                 ts).encode()
    if not await ed25519_verify(rec["pubkey"], signature, canonical):
        # A rebuilt host reusing a node name (mirror20 re-provisioned, adhoc
        # #1704) presents a fresh key the still-active account isn't bound
        # to, so this signature can never verify and the install dies on
        # bad_signature. The installer link code is the other proof: accept
        # the request as a key rotation when it is self-signed by the key it
        # presents AND that code already carries the installing user's
        # signed offer. That is exactly the evidence /api/accounts/
        # reclaim-node takes, honored here as well so mirror-node builds
        # that predate that endpoint can still finish their install.
        parked_user = await _parked_link_code_user(env, link_code)
        if not (valid_node_pubkey(pubkey)
                and _account_kind(rec) == "node"
                and parked_user
                and _link_code_rotation_allowed(rec, parked_user, pubkey)
                and await ed25519_verify(pubkey, signature, canonical)):
            return json_response({"error": "bad_signature"}, status=401)
        rec["pubkey"] = pubkey
    # Finalize can never introduce a new name — it 404s above unless the
    # reservation already exists — so the name was moderated when it was
    # reserved. Only the deterministic filter re-runs here; re-asking the AI
    # classifier is what stranded owners mid-install on a flipped verdict.
    moderation_error = established_username_moderation_error(name)
    if moderation_error:
        await log_blocked_signup(
            env, request, NODE_FINALIZE_PATH, 400, moderation_error)
        return json_response({"error": moderation_error}, status=400)

    # Email + password are optional for a key-bound node. When either is
    # supplied both are validated and the email is indexed for login.
    set_credentials = bool(email) or bool(password)
    email_bi = None
    if set_credentials:
        if not valid_account_email(email):
            return json_response({"error": "valid_email_required"}, status=400)
        if len(password) < 8:
            return json_response({"error": "password_too_short"}, status=400)
        email_bi = await blind_index(env, email)
        dup = await d1_first(
            env, "SELECT user_bi AS name_bi FROM users WHERE email_bi=?",
            email_bi)
        if dup and dup.get("name_bi") != name_bi:
            return json_response({"error": "email_taken"}, status=409)
        salt, phash = await hash_password(password)
        # Consent to publish an address on the profile was given for THAT
        # address; a finalize that swaps a different address onto the record
        # must not carry the opt-in over to the new one.
        if (clean_string(rec.get("email", ""), 254).strip().lower()
                != email.strip().lower()):
            rec.pop("profile_email_public", None)
        rec["email"] = email
        rec["pass_salt"] = salt
        rec["pass_hash"] = phash
    rec["kind"] = "node"
    rec["status"] = "active"
    # Optional public self-custodial payout address for reward consideration.
    if solana and SOLANA_RE.match(solana):
        rec["solana"] = solana
    rec["email_verified"] = bool(rec.get("email_verified", False))
    rec.setdefault("created_at", int(Date.now()))
    # Keep only coarse signup metadata; the transient address is converted to a
    # keyed blind index and never persisted in the account's decryptable blob.
    signup_ip = _transient_client_address(request)
    signup_meta = _signup_metadata(request)
    old_signup = rec.get("signup")
    if isinstance(old_signup, dict) and old_signup.get("at"):
        signup_meta["at"] = int(old_signup.get("at") or signup_meta["at"])
    rec["signup"] = signup_meta
    ip_bi = await blind_index(env, signup_ip) if signup_ip else None
    await _save_account(env, name_bi, rec, email_bi=email_bi, ip_bi=ip_bi)
    if rec.get("pubkey"):
        try:
            await touch_registered_node(env, name_bi, rec)
        except Exception:
            pass
    # Send a verification email (Mailtrap). If delivery is unavailable, retain
    # the account in the support queue for resend/follow-up; administrators can
    # no longer mark the address verified on the recipient's behalf.
    if rec.get("email") and not rec["email_verified"]:
        sent = await _send_verification_email(
            env, request, name, rec["email"], rec=rec)
        if not sent:
            await _enqueue_verification(env, name_bi, name, rec["email"])
    # Redeem the code read above: if the installing desktop's signed offer
    # already arrived the node is linked to that user right now; otherwise the
    # node's half is parked until the offer lands (see
    # _redeem_or_park_link_code).
    if key_bound and LINK_CODE_RE.match(link_code):
        result = await _redeem_or_park_link_code(
            env, link_code, node=name, pubkey=rec.get("pubkey", ""))
        if result.get("linked"):
            rec["owner"] = result.get("user", "")
    payload = await _account_public_payload(
        env, rec,
        session_device_label=_account_session_device_label(
            request, desktop=bool(pubkey)),
        session_client_ip=_account_session_client_ip(request))
    return json_response(
        payload,
        status=201,
        extra_headers={
            "Set-Cookie": _account_session_cookie(payload["sessionToken"])
        },
    )


async def _solana_transaction_matches_plan(env, signature, plan):
    """Return True/False for a finalized exact System transfer, None if absent."""
    if not signature or not isinstance(plan, dict):
        return False
    rpc = (
        _reward_solana_rpc
        if str(plan.get("network") or "") in (
            "devnet", "testnet", "mainnet-beta")
        else _solana_rpc
    )
    response = await rpc(
        env,
        "getTransaction",
        [signature, {
            "encoding": "jsonParsed",
            "commitment": "finalized",
            "maxSupportedTransactionVersion": 0,
        }],
    )
    if not isinstance(response, dict):
        return None
    result = response.get("result")
    if result is None:
        return None
    if not isinstance(result, dict):
        return False
    meta = result.get("meta")
    transaction = result.get("transaction")
    if (not isinstance(meta, dict) or meta.get("err") is not None
            or not isinstance(transaction, dict)):
        return False
    signatures = transaction.get("signatures")
    if not isinstance(signatures, list) or signature not in signatures:
        return False
    message = transaction.get("message")
    instructions = (
        message.get("instructions") if isinstance(message, dict) else None)
    if not isinstance(instructions, list):
        return False
    actual = []
    for instruction in instructions:
        if not isinstance(instruction, dict):
            return False
        # The local signer is intentionally limited to ordinary System Program
        # transfers. Any opaque/custom instruction makes the intent unverifiable.
        program = str(instruction.get("program") or "").lower()
        parsed = instruction.get("parsed")
        if program != "system" or not isinstance(parsed, dict):
            return False
        if parsed.get("type") != "transfer":
            return False
        info = parsed.get("info")
        if not isinstance(info, dict):
            return False
        source = str(info.get("source") or "")
        destination = str(info.get("destination") or "")
        try:
            lamports = int(info.get("lamports") or 0)
        except (TypeError, ValueError):
            return False
        if (source != plan.get("sourceAddress")
                or not SOLANA_RE.match(destination) or lamports <= 0):
            return False
        actual.append((destination, lamports))
    expected = []
    for transfer in plan.get("transfers") or []:
        if not isinstance(transfer, dict):
            return False
        destination = str(transfer.get("address") or "")
        try:
            lamports = int(transfer.get("lamports") or 0)
        except (TypeError, ValueError):
            return False
        if not SOLANA_RE.match(destination) or lamports <= 0:
            return False
        expected.append((destination, lamports))
    return bool(expected) and actual == expected


async def pending_reward_award_handler(env, request):
    """Create a 24-hour ledger allocation authorized by the local signer owner."""
    await ensure_schema(env)
    if method_name(request) != "POST":
        return json_response({"error": "method_not_allowed"}, status=405)
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    signer = clean_string(
        data.get("signer", ""), MAX_NODE_NAME).strip().lower()
    recipient = clean_string(
        data.get("recipient", ""), MAX_NODE_NAME).strip().lower()
    ts = clean_string(data.get("ts", ""), 20)
    sig = clean_string(data.get("sig", ""), 200)
    try:
        amount = int(data.get("amountLamports") or 0)
    except (TypeError, ValueError):
        amount = 0
    configured = (
        getattr(env, "REWARD_SIGNER_ACCOUNT", "")
        or getattr(env, "INSTANCE_OWNER_ACCOUNT", "")
        or ""
    ).strip().lower()
    canonical = (
        "forkmesh-pending-award-v1\n" + signer + "\n" + recipient
        + "\n" + str(amount) + "\n" + ts
    ).encode()
    if (not configured or signer != configured or not recipient
            or not _ts_ok(ts)
            or not await _verify_owner_signature(
                env, signer, sig, canonical)):
        return json_response({"error": "unauthorized"}, status=401)
    if not (1_000 <= amount <= 10 * LAMPORTS_PER_SOL):
        return json_response({"error": "invalid_reward_amount"}, status=400)
    recipient_bi, recipient_rec = await _account_row(env, recipient)
    if (not recipient_rec
            or recipient_rec.get("status") != "active"):
        return json_response({"error": "recipient_not_found"}, status=404)
    if SOLANA_RE.match(str(recipient_rec.get("solana") or "")):
        return json_response(
            {"error": "recipient_already_has_payout_wallet"}, status=409)
    source = await _central_fund_record(env)
    if not source:
        return json_response(
            {"error": "community_pool_not_configured"}, status=503)
    now = int(Date.now())
    reward_id = _random_bytes(16).hex()
    expires_at = now + REWARD_PENDING_EXPIRES_MS
    await d1_run(
        env,
        "INSERT INTO pending_rewards "
        "(reward_id,recipient_bi,recipient_name,source_address,"
        "amount_lamports,status,created_at,expires_at) "
        "VALUES (?,?,?, ?,?,'pending_wallet',?,?)",
        reward_id, recipient_bi, "", source.get("address", ""),
        amount, now, expires_at,
    )
    await enqueue_notification(
        env, recipient, "pending_reward",
        "Community reward pending",
        body=(
            "Add a public self-custodial Solana address within 24 hours "
            "to claim this allocation."
        ),
        source="community_reward",
        dedupe="pending_reward:" + reward_id,
    )
    await _audit_sensitive_action(
        env, signer, "reward.pending_allocate", "pending_reward",
        reward_id, "requested", {
            "recipientToken": await blind_index(
                env, "reward-recipient:" + recipient),
            "amountLamports": amount,
            "expiresAt": expires_at,
            "fundsLocation": "external-community-pool",
        })
    return json_response({
        "ok": True,
        "rewardId": reward_id,
        "status": "pending_wallet",
        "amountLamports": amount,
        "createdAt": now,
        "expiresAt": expires_at,
        "fundsLocation": "external-community-pool",
        "custody": "ledger-allocation-only",
        "notice": (
            "No wallet was created and no funds moved. The allocation expires "
            "after 24 hours and remains in its external source wallet unless "
            "the recipient supplies a self-custodial public address."
        ),
    }, status=201, cache_control="no-store")


async def orgs_handler(env, request):
    # POST /api/orgs creates an organization for the logged-in account (who
    # becomes its first 'owner' member); GET lists the orgs the session
    # account belongs to. Org names share the /<name>/<repo> URL namespace
    # with accounts, so creation rejects any name an account holds — and the
    # account reserve/signup paths reject org names — keeping the org-alias
    # URL rewrite collision-free.
    await ensure_schema(env)
    method = method_name(request)
    if method == "GET":
        account_bi, account_rec = await _org_management_session(env, request)
        if not account_rec:
            return json_response({"error": "invalid_session"}, status=401)
        rows = await d1_all(
            env,
            "SELECT o.name AS name, m.role AS role FROM org_members m "
            "JOIN orgs o ON o.org_bi = m.org_bi WHERE m.member_bi=? "
            "ORDER BY o.name",
            account_bi)
        return json_response({"ok": True, "orgs": [
            {"name": str(r.get("name") or ""), "role": str(r.get("role") or "")}
            for r in rows or []]})
    if method != "POST":
        return json_response({"error": "method_not_allowed"}, status=405)
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    account_bi, account_rec = await _org_management_session(env, request, data)
    if not account_rec:
        return json_response({"error": "invalid_session"}, status=401)
    account = (account_rec.get("name", "") or "").strip().lower()
    name = clean_string(data.get("name", ""), MAX_NODE_NAME).lower()
    if not valid_node_name(name):
        return json_response({"error": "invalid_org_name"}, status=400)
    # The name must be free in BOTH namespaces: any account row (active or
    # mid-signup) blocks it, and so does an existing org.
    _, existing_account = await _account_row(env, name)
    if existing_account:
        return json_response({"error": "org_name_taken"}, status=409)
    org_bi, existing_org = await _org_row(env, name)
    if existing_org:
        return json_response({"error": "org_name_taken"}, status=409)
    owned = await d1_first(
        env,
        "SELECT COUNT(*) AS n FROM org_members WHERE member_bi=? AND role='owner'",
        account_bi)
    if owned and int(owned.get("n") or 0) >= MAX_ORGS_PER_ACCOUNT:
        return json_response({"error": "too_many_orgs"}, status=429)
    now = int(Date.now())
    enc = await encrypt_row(env, {
        "name": name,
        "displayName": clean_string(data.get("displayName", ""), 80),
        "description": clean_string(data.get("description", ""), 500),
        "logoUrl": _org_logo_url(data.get("logoUrl", "")),
        "worldAccess": _org_world_access(data.get("worldAccess")),
        "creator": account,
        "createdAt": now,
    })
    await d1_run(
        env, "INSERT INTO orgs (org_bi, name, data, created_at) VALUES (?,?,?,?)",
        org_bi, name, enc, now)
    await d1_run(
        env,
        "INSERT OR REPLACE INTO org_members "
        "(org_bi, member_bi, role, name, created_at) VALUES (?,?,?,?,?)",
        org_bi, account_bi, "owner", account, now)
    return json_response({"ok": True, "org": name, "role": "owner"}, status=201)


class _PluginWebhookRuntime:
    """Minimal D1 adapter handed to plugin webhook hooks.

    ``plugin_data`` and ``d1_run`` arrive through entry.py's ``_bind_runtime``
    like every other primitive this module uses.
    """

    def __init__(self, env):
        self.env = env

    async def d1_run(self, sql, *args):
        return await d1_run(self.env, sql, *args)


async def process_mail_event(env, event, now):
    if not isinstance(event, dict):
        return False
    event_id = clean_string(
        event.get("event_id") or event.get("id") or "", 160)
    status = clean_string(
        event.get("event") or event.get("type") or "", 40).lower()
    variables = event.get("custom_variables")
    if not isinstance(variables, dict):
        variables = event.get("customVariables")
    variables = variables if isinstance(variables, dict) else {}
    send_id = clean_string(
        variables.get("forkmesh_send_id") or "", 64).lower()
    account_bi = clean_string(
        variables.get("forkmesh_account_bi") or "", 128)
    if not event_id or status not in MAILTRAP_WEBHOOK_EVENTS:
        return False
    prior = await d1_first(
        env,
        "SELECT event_id FROM mailtrap_webhook_events WHERE event_id=?",
        event_id)
    if prior:
        return True
    try:
        await d1_run(
            env,
            "INSERT INTO mailtrap_webhook_events(event_id,received_at) "
            "VALUES(?,?)",
            event_id, now)
    except Exception:
        return False
    if (account_bi and status in ("unsubscribe", "spam")
            and mail_budget.is_marketing_kind(
                variables.get("forkmesh_kind"))):
        try:
            await marketing_email_withdraw(env, account_bi)
        except Exception as error:
            print("marketing consent withdrawal failed: %s" % (
                type(error).__name__))
    if send_id and account_bi:
        is_open = 1 if status == "open" else 0
        is_click = 1 if status == "click" else 0
        await d1_run(
            env,
            "UPDATE mailtrap_email_sends SET status=?,status_at=?,"
            "message_id=?,"
            "opened_at=CASE WHEN ?=1 THEN ? ELSE opened_at END,"
            "clicked_at=CASE WHEN ?=1 THEN ? ELSE clicked_at END,"
            "open_count=open_count+?,click_count=click_count+? "
            "WHERE send_id=? AND account_bi=?",
            status,
            now,
            clean_string(event.get("message_id") or "", 160),
            is_open,
            now,
            is_click,
            now,
            is_open,
            is_click,
            send_id,
            account_bi,
        )
    if send_id and status in ("open", "click"):
        try:
            await mail_activity.tracked(
                env, send_id, status, now,
                message_id=event.get("message_id") or "")
        except Exception as error:
            print("mail activity tracking failed: %s" % type(error).__name__)
    # Installed market plugins receive the authenticated event through
    # the shared lifecycle engine (plugins link their own send statuses);
    # an uninstalled plugin's dropped tables never fail the batch.
    await plugin_data.deliver_mailtrap_event(
        _PluginWebhookRuntime(env), event, status, now)
    try:
        sent = {}
        if send_id and status in mail_log.WEBHOOK_RESULTS:
            try:
                sent = await mail_activity.send_details(env, send_id)
            except Exception as error:
                print("mail send details unavailable: %s" % (
                    type(error).__name__))
        follow_up = mail_log.webhook_note(
            event, now, subject=sent.get("subject") or "",
            body=sent.get("body") or "")
        if follow_up is not None:
            await mail_log.enqueue(env, follow_up)
    except Exception:
        pass
    return True


async def _mail_send_row(env, send_id="", message_id=""):
    if send_id:
        return await d1_first(
            env, "SELECT send_id,message_id,kind,account_bi,recipient_bi "
            "FROM mail_sends WHERE send_id=?", send_id)
    if message_id:
        return await d1_first(
            env, "SELECT send_id,message_id,kind,account_bi,recipient_bi "
            "FROM mail_sends WHERE message_id=?", message_id)
    return None


async def prune_mail_records(env, now):
    cutoff = now - 90 * 24 * 60 * 60 * 1000
    await d1_run(env, "DELETE FROM mail_sends WHERE sent_at<?", cutoff)
    await d1_run(
        env, "DELETE FROM mailtrap_webhook_events WHERE received_at<?", cutoff)
    await d1_run(
        env, "DELETE FROM mailtrap_email_sends WHERE sent_at<?", cutoff)
    await d1_run(env, "DELETE FROM email_sends WHERE sent_at<?", cutoff)


async def record_core_mail_event(env, event, now=None):
    if not isinstance(event, dict):
        return False
    now = int(Date.now()) if now is None else now
    event = dict(event)
    row = await _mail_send_row(
        env, clean_string(event.pop("send_id", "") or "", 64).lower(),
        clean_string(event.get("message_id") or "", 200))
    if row:
        try:
            variables = await mail_activity.send_variables(
                env, row.get("send_id") or "")
        except Exception:
            variables = {}
        event["custom_variables"] = dict(variables, **{
            "forkmesh_send_id": row.get("send_id") or "",
            "forkmesh_account_bi": row.get("account_bi") or "",
            "forkmesh_kind": row.get("kind") or ""})
        event["message_id"] = row.get("message_id") or ""
        if str(event.get("event") or "") in mail_tracking.DELIVERY_EVENTS:
            await d1_run(
                env, "UPDATE mail_sends SET status=?,status_at=?,dsn=? "
                "WHERE send_id=?", str(event.get("event")), now,
                mail_tracking.dsn(event.get("dsn")), row.get("send_id") or "")
        reason = mail_tracking.SUPPRESS_EVENTS.get(
            str(event.get("event") or ""))
        if reason == "unsubscribe" and mail_budget.is_marketing_kind(
                row.get("kind")):
            reason = "marketing_unsubscribe"
        if reason and row.get("recipient_bi"):
            await d1_run(
                env, "INSERT INTO mail_suppressions"
                "(recipient_bi,reason,created_at) VALUES(?,?,?) "
                "ON CONFLICT(recipient_bi) DO UPDATE SET "
                "reason=excluded.reason,created_at=excluded.created_at "
                "WHERE mail_suppressions.reason='marketing_unsubscribe' "
                "AND excluded.reason<>'marketing_unsubscribe'",
                row["recipient_bi"], reason, now)
    elif event.get("node"):
        event["forkmesh_outside_app"] = True
    recorded = await process_mail_event(env, event, now)
    await prune_mail_records(env, now)
    return recorded


async def _mail_token_ok(env, send_id, signature):
    if not send_id:
        return False
    expected = (await blind_index(env, "mail-track:" + send_id))[:24]
    return hmac.compare_digest(expected, str(signature or ""))


def _mail_page(text, status=200):
    return Response(
        "<!doctype html><meta charset=utf-8><meta name=viewport "
        "content='width=device-width'><title>ForkMesh</title>"
        "<body style='font:16px system-ui;margin:40px auto;max-width:520px'>"
        + text + "</body>", status=status,
        headers={"content-type": "text/html; charset=utf-8",
                 "cache-control": "no-store", "x-robots-tag": "noindex"})


def _mail_logo_response():
    return Response("", status=302, headers={
        "location": mail_activity.FOOTER_LOGO,
        "cache-control": "no-store, max-age=0",
        "referrer-policy": "no-referrer"})


async def mail_footer_image_handler(env, request, value):
    await ensure_schema(env)
    try:
        await mail_activity.footer_opened(env, value, int(Date.now()))
    except Exception as error:
        print("mail footer open not recorded: %s" % type(error).__name__)
    return _mail_logo_response()


async def mail_footer_handler(env, request):
    await ensure_schema(env)
    if method_name(request) != "GET":
        return json_response({"error": "method_not_allowed"}, status=405,
                             extra_headers={"allow": "GET"})
    account_bi, rec = await _account_session_record(env, request, None)
    account = (rec.get("name", "") if rec else "").strip().lower()
    if not account:
        return json_response({"error": "invalid_session"}, status=401)
    row = await mail_activity.footer(
        env, account_bi, account,
        clean_string(rec.get("email", ""), 254).strip().lower(),
        int(Date.now()))
    if not row:
        return json_response({"error": "footer_unavailable"}, status=503)
    base = (getattr(env, "API_ORIGIN", "") or
            "https://app.forkmesh.com").rstrip("/")
    image = "%s/api/mail/t/f/%s.png" % (
        base, await mail_activity.footer_token(env, row.get("footer_id")))
    return json_response({
        "ok": True, "imageUrl": image,
        "html": mail_activity.footer_html(image),
        "previewHtml": mail_activity.footer_html(mail_activity.FOOTER_LOGO),
        "opens": int(row.get("open_count") or 0),
        "firstOpenedAt": int(row.get("opened_at") or 0),
        "lastOpenedAt": int(row.get("last_open_at") or 0)},
        cache_control="no-store, max-age=0")


async def mail_tracking_handler(env, request, action, value):
    await ensure_schema(env)
    send_id, signature = mail_tracking.parse_token(value)
    if not await _mail_token_ok(env, send_id, signature):
        if action == "l":
            return _mail_logo_response()
        return _mail_page("This link is not valid.", 404)
    now = int(Date.now())
    minute = str(now // 60000)
    method = method_name(request)
    if action in ("o", "l"):
        await record_core_mail_event(env, {
            "event_id": "open:%s:%s" % (send_id, minute), "event": "open",
            "send_id": send_id}, now)
        if action == "l":
            return _mail_logo_response()
        return JsResponse.new(
            Uint8Array.new(_to_js(bytes(mail_tracking.PIXEL))), to_js({
                "status": 200, "headers": {
                    "content-type": "image/gif",
                    "cache-control": "no-store, max-age=0"}}))
    if action == "c":
        url = JsURL.new(request.url)
        target = mail_tracking.safe_target(url.searchParams.get("u") or "")
        expected = (await blind_index(
            env, "mail-click:%s:%s" % (send_id, target)))[:24]
        if not target or not hmac.compare_digest(
                expected, str(url.searchParams.get("s") or "")):
            return _mail_page("This link is not valid.", 404)
        await record_core_mail_event(env, {
            "event_id": "click:%s:%s" % (send_id, minute), "event": "click",
            "send_id": send_id}, now)
        return Response("", status=302, headers={
            "location": target, "cache-control": "no-store",
            "referrer-policy": "no-referrer"})
    if action == "u" and method == "POST":
        await record_core_mail_event(env, {
            "event_id": "unsubscribe:" + send_id, "event": "unsubscribe",
            "send_id": send_id}, now)
        return _mail_page("You are unsubscribed from these ForkMesh emails.")
    if action == "u":
        return _mail_page(
            "<form method=post><p>Stop these ForkMesh emails?</p>"
            "<button type=submit>Unsubscribe</button></form>")
    return _mail_page("Not found.", 404)


async def mailtrap_webhook_handler(env, request):
    if method_name(request) != "POST":
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            extra_headers={"allow": "POST"})
    secret = str(
        getattr(env, "MAILTRAP_WEBHOOK_SECRET", "") or "").strip()
    if not secret:
        return json_response(
            {"error": "mailtrap_webhook_not_configured"}, status=503)
    try:
        raw = await request.text()
    except Exception:
        return json_response({"error": "invalid_payload"}, status=400)
    if len(raw.encode("utf-8")) > 1024 * 1024:
        return json_response({"error": "payload_too_large"}, status=413)
    presented = str(
        request.headers.get("mailtrap-signature") or "").strip().lower()
    expected = hmac.new(
        secret.encode("utf-8"), raw.encode("utf-8"), hashlib.sha256
    ).hexdigest()
    if not presented or not hmac.compare_digest(presented, expected):
        return json_response({"error": "invalid_signature"}, status=401)
    events = _mailtrap_webhook_payload(raw)
    if events is None or len(events) > 500:
        return json_response({"error": "invalid_payload"}, status=400)
    await ensure_schema(env)
    accepted = 0
    now = int(Date.now())
    for event in events:
        if await process_mail_event(env, event, now):
            accepted += 1
    await prune_mail_records(env, now)
    for cache_key in (USERS_DIRECTORY_CACHE_KEY, USERS_WORLD_CACHE_KEY):
        await edge_cache_delete(cache_key)
    return json_response(
        {"ok": True, "accepted": accepted},
        cache_control="no-store, max-age=0")


async def owner_encryption_keys_handler(env, request):
    """Register or revoke public recipient bundles for the signed-in owner.

    The private X25519/ML-KEM halves never cross this API. Platform
    administrators intentionally have no read or write override.
    """
    await ensure_schema(env)
    method = method_name(request)
    data = {}
    if method in ("POST", "DELETE"):
        try:
            data = await bounded_json_request(request)
        except Exception:
            return json_response({"error": "invalid_json"}, status=400)
    account_bi, rec = await _account_session_record(env, request, data)
    actor = (rec.get("name", "") if rec else "").strip().lower()
    if not actor:
        return json_response({"error": "invalid_session"}, status=401)

    if method == "GET":
        rows = await d1_all(
            env,
            "SELECT key_id, public_bundle, created_at FROM "
            "owner_encryption_keys WHERE account_bi=? AND revoked_at=0 "
            "ORDER BY created_at DESC",
            account_bi,
        )
        keys = []
        for row in rows or []:
            try:
                bundle = json.loads(str(row.get("public_bundle") or ""))
            except Exception:
                continue
            valid = security_control.validate_owner_public_bundle(bundle)
            if valid and valid["kid"] == row.get("key_id"):
                keys.append({
                    "keyId": valid["kid"],
                    "publicBundle": {
                        "v": valid["v"],
                        "x25519": valid["x25519"],
                        "mlkem768": valid["mlkem768"],
                    },
                    "createdAt": int(row.get("created_at") or 0),
                })
        return json_response({
            "ok": True,
            "keys": keys,
            "custody": "owner-device-only",
            "privateKeysStored": False,
        }, cache_control="no-store")

    if method == "POST":
        valid = security_control.validate_owner_public_bundle(
            data.get("publicBundle"))
        if not valid:
            return json_response({"error": "invalid_public_bundle"}, status=400)
        now = int(Date.now())
        public_bundle = {
            "v": valid["v"],
            "x25519": valid["x25519"],
            "mlkem768": valid["mlkem768"],
        }
        await d1_run(
            env,
            "INSERT INTO owner_encryption_keys "
            "(account_bi, key_id, public_bundle, created_at, revoked_at) "
            "VALUES (?,?,?,?,0) ON CONFLICT(account_bi,key_id) DO UPDATE SET "
            "public_bundle=excluded.public_bundle, revoked_at=0",
            account_bi, valid["kid"],
            json.dumps(public_bundle, sort_keys=True, separators=(",", ":")),
            now,
        )
        await _audit_sensitive_action(
            env, actor, "owner_encryption_key.register", "account", actor,
            "success", {"keyId": valid["kid"], "publicOnly": True})
        return json_response({
            "ok": True,
            "keyId": valid["kid"],
            "custody": "owner-device-only",
            "privateKeysStored": False,
        }, status=201, cache_control="no-store")

    if method == "DELETE":
        key_id = clean_string(data.get("keyId", ""), 128)
        if not re.fullmatch(r"[A-Za-z0-9_-]{16,128}", key_id):
            return json_response({"error": "invalid_key_id"}, status=400)
        now = int(Date.now())
        await d1_run(
            env,
            "UPDATE owner_encryption_keys SET revoked_at=? "
            "WHERE account_bi=? AND key_id=?",
            now, account_bi, key_id,
        )
        # A policy still naming the revoked key remains strict and therefore
        # rejects new ciphertext until the owner rotates it; it never silently
        # falls back to Worker-readable plaintext.
        await _audit_sensitive_action(
            env, actor, "owner_encryption_key.revoke", "account", actor,
            "success", {"keyId": key_id})
        return json_response({"ok": True, "revoked": True},
                             cache_control="no-store")

    return json_response({"error": "method_not_allowed"}, status=405)


async def repo_privacy_handler(env, request, owner, repo):
    await ensure_schema(env)
    method = method_name(request)
    if method not in ("GET", "POST"):
        return json_response({"error": "method_not_allowed"}, status=405)
    data = {}
    if method == "POST":
        try:
            data = await bounded_json_request(request)
        except Exception:
            return json_response({"error": "invalid_json"}, status=400)
    ok, err = await _authorize_owner_account(
        env, owner, data, request, allow_admin=False)
    if not ok:
        return err
    actor_bi, actor_rec = await _account_session_record(env, request, data)
    actor = (actor_rec.get("name", "") if actor_rec else "").strip().lower()
    policy_reader = globals().get("_repo_privacy_policy")
    policy = (
        await policy_reader(env, owner, repo)
        if callable(policy_reader) else {
            # A missing policy reader is a control-plane failure, never a
            # reason to reactivate relay-readable private data.
            "ownerKeyId": "", "requireAgentE2EE": True,
            "requireMirrorEncryption": True,
        }
    )

    if method == "POST":
        require_agent = True
        require_mirror = True
        key_id = clean_string(
            data.get("ownerKeyId", "") or policy.get("ownerKeyId", ""), 128)
        key = await d1_first(
            env,
            "SELECT 1 AS one FROM owner_encryption_keys "
            "WHERE account_bi=? AND key_id=? AND revoked_at=0",
            actor_bi, key_id,
        )
        if not key:
            return json_response(
                {"error": "active_owner_key_required"}, status=400)
        now = int(Date.now())
        await d1_run(
            env,
            "INSERT INTO repo_privacy_policy "
            "(repo_bi, owner_bi, owner_key_id, require_agent_e2ee, "
            "require_mirror_encryption, updated_at) VALUES (?,?,?,?,?,?) "
            "ON CONFLICT(repo_bi) DO UPDATE SET "
            "owner_bi=excluded.owner_bi, owner_key_id=excluded.owner_key_id, "
            "require_agent_e2ee=excluded.require_agent_e2ee, "
            "require_mirror_encryption=excluded.require_mirror_encryption, "
            "updated_at=excluded.updated_at",
            policy["repoBi"], actor_bi, key_id,
            1 if require_agent else 0, 1 if require_mirror else 0, now,
        )
        await _audit_sensitive_action(
            env, actor, "repository.privacy_policy", "repository",
            owner + "/" + repo, "success",
            {"agentE2EE": require_agent, "mirrorEncryption": require_mirror,
             "keyId": key_id})
        policy = await _repo_privacy_policy(env, owner, repo)

    return json_response({
        "ok": True,
        "ownerKeyId": policy.get("ownerKeyId", ""),
        "requireAgentE2EE": bool(policy.get("requireAgentE2EE")),
        "requireMirrorEncryption": bool(
            policy.get("requireMirrorEncryption")),
        "agentBoundary": "owner-only-e2ee",
        # This policy is an enforcement requirement/attestation gate. Actual
        # encrypted-at-rest mirror storage is performed and verified locally by
        # an upgraded Qt node; the Worker cannot inspect a node's disk.
        "mirrorBoundary": "client-encryption-required",
        "updatedAt": int(policy.get("updatedAt") or 0),
    }, cache_control="no-store")


def _org_agent_security_envelope(context, owner_scope):
    """Session security record, job envelope, and queue status for a request.

    Agent prompts no longer pass through a model safety preflight: every
    request queues straight to its node, which accepts the run as work for the
    account that owns it.  Owner-scope envelopes keep the owner waiver fields
    so a node that predates this change still accepts owner-queued jobs while
    it updates; current nodes and desktops ignore the envelope.
    """
    record = {"state": "not_required", "model": "", "reason": ""}
    if owner_scope:
        return (
            record,
            {
                "required": False,
                "waiver": "repository_owner",
                "waivedFor": context["actor"],
                "tools": False,
                "failClosed": True,
            },
            "queued",
        )
    return (record, {"required": False}, "queued")


def _org_agent_session_projection(row, record):
    import agent_branches
    history = (
        record.get("history") if isinstance(record.get("history"), list) else []
    )
    status = str(row.get("status") or "")
    updated_at = int(row.get("updated_at") or 0)
    age_ms = max(0, int(Date.now()) - updated_at) if updated_at else 0
    availability = _org_agent_availability_projection(
        record.get("availability"))
    agent_info = _org_agent_info_projection(record.get("agentInfo"))
    diagnostic = {
        "code": "waiting_for_mirror",
        "level": "pending",
        "message": "Waiting for the selected mirror to claim this job.",
    }
    if (
        status in ("security_pending", "queued")
        and int(record.get("localAgentId") or 0) <= 0
        and not availability.get("message")
        and age_ms >= 90 * 1000
    ):
        diagnostic = {
            "code": "mirror_not_claiming_jobs",
            "level": "attention",
            "message": (
                "The selected node has not claimed this job. Confirm the node "
                "is online (the ForkMesh desktop or the forkmesh-mirror-node "
                "service), serves this repository, has the selected provider "
                "CLI installed and signed in, and can sign for its account."
            ),
        }
    elif status not in ("running", "completed") and (
        availability.get("binaryFound") is False
        or availability.get("loginState") == "missing"
    ):
        diagnostic = {
            "code": (
                "provider_binary_missing"
                if availability.get("binaryFound") is False
                else "provider_credential_missing"
            ),
            "level": "attention",
            "message": availability.get("message") or (
                "The selected node reported that the provider CLI is not "
                "installed or not signed in."
            ),
        }
    elif status in ("failed", "rejected"):
        diagnostic = {
            "code": "agent_" + status,
            "level": "attention",
            "message": (
                agent_info.get("lastError")
                or str((record.get("security") or {}).get("reason") or "")
                or "The mirror reported that this session needs attention."
            ),
        }
    elif status in ("running", "queued", "completed"):
        diagnostic = {
            "code": status,
            "level": "ok" if status == "completed" else "active",
            "message": {
                "running": "The mirror is running this agent session.",
                "queued": "The mirror accepted the job and is preparing it.",
                "completed": "The mirror completed this agent session.",
            }[status],
        }
    return {
        "id": str(row.get("session_id") or ""),
        "org": str(record.get("org") or ""),
        "repo": str(row.get("repo") or ""),
        "provider": str(row.get("provider") or ""),
        "status": status,
        "displayStatus": (
            "attention"
            if diagnostic.get("level") == "attention"
            else status
        ),
        "targetNode": str(
            record.get("runnerNode") or row.get("target_node") or ""),
        "onRunner": record.get("route") == "runner_pool" or bool(
            str(record.get("runnerNode") or "").strip().lower()
            not in ("", str(row.get("target_node") or "").strip().lower())),
        "title": clean_string(record.get("title"), 160),
        "createdBy": clean_string(record.get("createdBy"), MAX_NODE_NAME),
        "createdAt": int(row.get("created_at") or 0),
        "updatedAt": updated_at,
        "completedAt": int(row.get("completed_at") or 0),
        "localAgentId": int(record.get("localAgentId") or 0),
        "taskKey": clean_string(record.get("taskKey"), 96),
        "issueNumber": max(0, min(
            int(record.get("issueNumber") or 0), 10_000_000)),
        "requestedModel": clean_string(record.get("requestedModel"), 60),
        "createPr": record.get("createPr") is True,
        "history": history[-ORG_AGENT_MAX_HISTORY:],
        "live": _org_agent_live_projection(record, status),
        "security": record.get("security") or {},
        "agentInfo": agent_info,
        "availability": availability,
        "diagnostic": diagnostic,
        # Set when the owner asked the node to stop this run; the status
        # follows once the node reports how it ended.
        "stopRequestedAt": max(0, int(record.get("stopRequestedAt") or 0)),
        **agent_branches.projection_extras(row, record),
    }


def _org_agent_hide_private_runner(projection, row, record, viewer):
    if not record.get("runnerPrivate"):
        return projection
    if str(record.get("runnerOwner") or "") == viewer:
        return projection
    projection["runner"] = ""
    projection["targetNode"] = str(row.get("target_node") or "")
    if isinstance(projection.get("queue"), dict):
        projection["queue"].pop("runner", None)
    return projection


async def _org_agent_private_runner_block(env, row, record, actor):
    import agent_branches
    name = agent_branches.session_runner(row, record)
    if not name:
        return None
    runner = await d1_first(
        env,
        "SELECT scope,registered_by FROM runner_nodes WHERE name=?", name,
    ) or {}
    if (str(runner.get("scope") or "") != "personal"
            or str(runner.get("registered_by") or "") == actor):
        return None
    return _org_agent_error(
        "runner_private",
        "This session runs on another member's personal runner, so only "
        "they can send it a follow-up. Start a new session to run your own.",
        403,
        required_action="start_new_session",
    )


_ORG_AGENT_QUEUE_STATUSES = ("queued", "security_pending")
_ORG_AGENT_RUNNER_ONLINE_GRACE_MS = 30 * 1000


async def _org_agent_queue_projection(env, row, record):
    import agent_branches
    import runner_leases
    if str(row.get("status") or "") not in _ORG_AGENT_QUEUE_STATUSES:
        return {}
    now = int(Date.now())
    mine = await d1_first(
        env,
        "SELECT id,target_node,repo,created_at FROM org_agent_jobs "
        "WHERE session_id=? AND status='queued' ORDER BY id LIMIT 1",
        str(row.get("session_id") or ""),
    )
    if not mine:
        return {}
    job_id = int(mine.get("id") or 0)
    counts = await d1_first(
        env,
        "SELECT COALESCE(SUM(status='queued'),0) AS waiting,"
        "COALESCE(SUM(status='queued' AND id<?),0) AS ahead,"
        "COALESCE(SUM(status IN ('leased','running')),0) AS active,"
        "COALESCE(SUM(status='leased' AND repo=?),0) AS repo_leased "
        "FROM org_agent_jobs WHERE target_node=? "
        "AND status IN ('queued','leased','running')",
        job_id, str(mine.get("repo") or ""), str(mine.get("target_node") or ""),
    ) or {}
    ahead = int(counts.get("ahead") or 0)
    queue = {
        "jobId": job_id,
        "position": ahead + 1,
        "ahead": ahead,
        "waiting": max(ahead + 1, int(counts.get("waiting") or 0)),
        "running": int(counts.get("active") or 0),
        "repoBusy": int(counts.get("repo_leased") or 0) > 0,
        "queuedAt": int(mine.get("created_at") or 0),
        "now": now,
        "checkEveryMs": runner_leases.CLAIM_WAIT_MAX_MS,
        "leaseTimeoutMs": runner_leases.JOB_TIMEOUT_MS,
    }
    name = agent_branches.session_runner(row, record)
    if not name:
        return queue
    node = await d1_first(
        env,
        "SELECT status,last_seen_at FROM runner_nodes WHERE name=?", name,
    ) or {}
    leases = await d1_all(
        env,
        "SELECT created_at,expires_at FROM runner_job_leases "
        "WHERE runner=? AND lease_id!='' AND expires_at>? "
        "ORDER BY created_at",
        name, now,
    ) or []
    last_seen = int(node.get("last_seen_at") or 0)
    busy = bool(leases)
    next_check = last_seen + runner_leases.CLAIM_WAIT_MAX_MS if last_seen else 0
    online = str(node.get("status") or "") == "active" and (
        busy or (bool(last_seen) and now - next_check
                 <= _ORG_AGENT_RUNNER_ONLINE_GRACE_MS))
    queue["runner"] = {
        "name": name,
        "online": online,
        "busy": busy,
        "activeJobs": len(leases),
        "lastSeenAt": last_seen,
        "nextCheckAt": 0 if busy else next_check,
        "busySince": min(int(item.get("created_at") or 0) for item in leases)
        if busy else 0,
        "freeBy": min(int(item.get("expires_at") or 0) for item in leases)
        if busy else 0,
    }
    return queue


async def _org_agent_stop_session(env, context, row, record, owner_scope):
    """Stop one cloud run: drop its queued work, and ask the node to end it.

    A session no node has started (no localAgentId) has nothing running, so
    its queued jobs are withdrawn and it ends here as failed. A started run
    gets a ``kind:"stop"`` job ahead of any follow-up still waiting: the node
    that runs the session kills the provider process and reports the run as
    failed with the reason, through the same signed result route as every
    other job. Nothing here reaches into a node; the node decides.
    """
    now = int(Date.now())
    session_id = str(row.get("session_id") or "")
    status = str(row.get("status") or "")
    local_agent_id = int(record.get("localAgentId") or 0)
    if status not in ("queued", "running", "security_pending"):
        return _org_agent_error(
            "session_not_running",
            "This agent session is not running, so there is nothing to stop.",
            409,
            required_action="refresh_session",
        )
    history = record.get("history")
    if not isinstance(history, list):
        history = []
    history.append({
        "role": "user",
        "author": context["actor"],
        "text": "Stop this run.",
        "at": now,
        "state": "stopping",
    })
    record["history"] = history[-ORG_AGENT_MAX_HISTORY:]
    record["stopRequestedAt"] = now
    # Follow-ups nobody has claimed yet would only restart what is being
    # stopped; withdraw them in the same batch.
    withdraw = (
        "UPDATE org_agent_jobs SET status='failed',updated_at=? "
        "WHERE session_id=? AND org_bi=? AND repo=? AND status='queued'",
        (now, session_id, context["orgBi"], context["repo"]),
    )
    if local_agent_id <= 0:
        statements = [
            withdraw,
            (
                "UPDATE org_agent_jobs SET status='failed',updated_at=? "
                "WHERE session_id=? AND org_bi=? AND repo=? "
                "AND status='leased'",
                (now, session_id, context["orgBi"], context["repo"]),
            ),
            (
                "UPDATE org_agent_sessions SET status='failed',data=?,"
                "updated_at=?,completed_at=? WHERE session_id=? AND org_bi=?",
                (
                    await encrypt_row(env, record), now, now, session_id,
                    context["orgBi"],
                ),
            ),
        ]
        outcome = "withdrawn"
    else:
        job = {
            "kind": "stop",
            "sessionId": session_id,
            "organization": context["org"],
            "repositoryOwner": str(row.get("target_node") or ""),
            "repositoryName": context["repo"],
            "provider": str(row.get("provider") or ""),
            # Older nodes validate every envelope's prompt before its kind;
            # they then refuse the unknown kind instead of running anything.
            "prompt": "Stop this run.",
            "localAgentId": local_agent_id,
            "requestedBy": context["actor"],
        }
        statements = [
            withdraw,
            (
                "UPDATE org_agent_sessions SET data=?,updated_at=? "
                "WHERE session_id=? AND org_bi=?",
                (
                    await encrypt_row(env, record), now, session_id,
                    context["orgBi"],
                ),
            ),
            (
                "INSERT INTO org_agent_jobs "
                "(session_id,org_bi,target_node,repo,provider,status,"
                "lease_id,data,created_at,updated_at) "
                "VALUES (?,?,?,?,?,'queued','',?,?,?)",
                (
                    session_id, context["orgBi"], row["target_node"],
                    context["repo"], row["provider"],
                    await encrypt_row(env, job), now, now,
                ),
            ),
        ]
        outcome = "stop_requested"
    try:
        await _contribution_run_batch(env, statements)
    except Exception:
        return _org_agent_error(
            "queue_persistence_failed",
            "The stop request could not be saved. Try again.",
            503,
            retryable=True,
            required_action="retry_stop",
            degraded=True,
        )
    audit_scope = "owner" if owner_scope else "organization"
    await _audit_sensitive_action(
        env, context["actor"], audit_scope + ".agent_stop",
        audit_scope + "_agent", context["org"] + "/" + context["repo"] +
        "/" + session_id, "success", {
            "outcome": outcome,
            "targetNode": str(row.get("target_node") or ""),
        })
    if local_agent_id > 0:
        await notify_repo_host(
            env, row["target_node"], context["repo"], "org-agents")
    await _ring_cloud_agent_readers(env, record)
    return json_response({
        "ok": True,
        "stopped": local_agent_id <= 0,
        "outcome": outcome,
        "message": (
            "The run had not started; it was withdrawn."
            if local_agent_id <= 0 else
            "Stop sent. The node ends the run as soon as it picks this up."
        ),
    }, status=202, cache_control="no-store")


async def _ring_cloud_agent_readers(env, record, topic="cloud-agents"):
    """Tell the account's own devices that one of its cloud runs moved.

    A payload-free "cloud-agents" frame on the account event socket. Without
    it nothing tells a desktop or a phone that a mirror claimed, ran or
    finished a job: no client polls for it
    (www/docs/operations/polling-elimination.md), so the run would stay
    invisible until somebody opened that exact repository and refreshed.

    Its own topic, not the "agents" one a desktop's snapshot push rings
    (agent_routes._ring_agent_readers): that frame is this desktop hearing its
    own work back, and answering it with a relay read would turn every local
    agent step into a request. Not an APNs topic either — the queue is read
    when a client is running, never worth waking a sleeping phone for.

    Best-effort, and coalesced by notify_account_event; the session row is
    already written by the time it is called.
    """
    if not isinstance(record, dict):
        return
    owner_scope = str(record.get("scope") or "organization") == "owner"
    rung = set()
    names = [record.get("createdBy")]
    if owner_scope:
        names.append(record.get("org"))
    for name in names:
        owner = clean_string(name, MAX_NODE_NAME).strip().lower()
        if not owner or owner in rung:
            continue
        rung.add(owner)
        try:
            await notify_account_event(env, owner, topic)
        except Exception:
            pass


# Canonical prefixes a desktop signs with its account's Ed25519 key to read and
# work with the cloud agent sessions of a repository it owns. The ordinary
# desktop launch is authenticateSilently(), which mints no session token, so
# without these the Agents list could never show the runs its owner queued on
# a mirror. Same discipline as the task-board proofs: one prefix per operation,
# the canonical binds the exact path (owner, repository and session id), and a
# follow-up also binds the digest of its prompt — a captured proof can never be
# replayed as a different request. Owner scope only: the organization route
# keeps requiring a real session. The signer travels as `user`, because an
# account is a person and the machine is only one of that person's nodes.
OWNER_AGENT_LIST_PROOF = "forkmesh-owner-agent-list-v1"
OWNER_AGENT_ACCOUNT_LIST_PROOF = "forkmesh-owner-agent-account-list-v1"
OWNER_AGENT_READ_PROOF = "forkmesh-owner-agent-read-v1"
OWNER_AGENT_PROMPT_PROOF = "forkmesh-owner-agent-prompt-v1"
OWNER_AGENT_DELETE_PROOF = "forkmesh-owner-agent-delete-v1"
OWNER_AGENT_STOP_PROOF = "forkmesh-owner-agent-stop-v1"
OWNER_AGENT_QUEUE_PROOF = "forkmesh-owner-agent-queue-v1"
OWNER_AGENT_BRANCH_PROOF = "forkmesh-owner-agent-branch-v1"
OWNER_AGENT_QUEUE_KEYS = frozenset(
    ("prompt", "provider", "model", "createPr", "fromDevice"))
OWNER_AGENT_BRANCH_ACTIONS = frozenset(
    ("merge", "cleanup", "link-pull", "update"))
OWNER_AGENT_BRANCH_KEYS = frozenset(
    ("action", "cleanup", "discard", "requestId", "number"))


def _owner_agent_branch_text(body):
    if not set(body) <= OWNER_AGENT_BRANCH_KEYS:
        return None
    action = body.get("action")
    if action not in OWNER_AGENT_BRANCH_ACTIONS:
        return None
    flags = []
    for key in ("cleanup", "discard"):
        value = body.get(key)
        if key in body and not isinstance(value, bool):
            return None
        flags.append("" if key not in body else "1" if value else "0")
    request_id = body.get("requestId", "")
    if not isinstance(request_id, str):
        return None
    number = body.get("number", 0)
    if isinstance(number, bool) or not isinstance(number, int):
        return None
    return "\n".join(
        (action, flags[0], flags[1], request_id,
         str(number) if "number" in body else ""))


def _owner_agent_queue_text(body):
    if set(body) != OWNER_AGENT_QUEUE_KEYS:
        return None
    text_fields = [body.get(key) for key in (
        "provider", "model", "fromDevice", "prompt")]
    if not all(isinstance(value, str) for value in text_fields):
        return None
    if not isinstance(body.get("createPr"), bool):
        return None
    provider, model, from_device, prompt = text_fields
    return "\n".join((
        provider, model, "1" if body["createPr"] else "0", from_device,
        prompt))


def _owner_agent_signed_triple(request):
    """True when this URL carries a desktop user/ts/sig triple."""
    try:
        params = parse_qs(urlparse(request.url).query)
    except Exception:
        return False
    return bool(
        clean_string(params.get("user", [""])[0], MAX_NODE_NAME)
        and clean_string(params.get("ts", [""])[0], 20)
        and clean_string(params.get("sig", [""])[0], 200))


async def _owner_agent_signed_session(env, request, data=None):
    """Resolve the account behind a key-signed owner agent-session request.

    Always returns a pair, ("", None) when the proof does not hold, so the
    caller can hand it to _org_agent_member_context as the only credential.
    """
    method = method_name(request)
    url = urlparse(request.url)
    collection = REPO_OWNER_AGENT_SESSIONS_RE.match(url.path)
    item = (REPO_OWNER_AGENT_SESSION_RE.match(url.path)
            or ORG_AGENT_BOT_RE.match(url.path))
    stream = (REPO_OWNER_AGENT_SESSION_STREAM_RE.match(url.path)
              or ORG_AGENT_BOT_STREAM_RE.match(url.path))
    attachment = (
        REPO_OWNER_AGENT_SESSION_ATTACHMENT_RE.match(url.path)
        or ORG_AGENT_BOT_ATTACHMENT_RE.match(url.path))
    account = ACCOUNT_AGENT_SESSIONS_RE.match(url.path)
    org_collection = ORG_AGENT_BOTS_RE.match(url.path)
    diff = REPO_OWNER_AGENT_SESSION_DIFF_RE.match(url.path)
    body = data if isinstance(data, dict) else {}
    digest = ""
    if (collection or org_collection) and method == "GET":
        proof = OWNER_AGENT_LIST_PROOF
    elif diff and method == "GET":
        proof = OWNER_AGENT_READ_PROOF
    elif account and method == "GET":
        # Reads nothing a per-repository list would not, only all of it at
        # once, so it is its own prefix on the same one-proof-per-operation
        # rule: a list proof for one repository never answers for the account.
        proof = OWNER_AGENT_ACCOUNT_LIST_PROOF
    elif (item or stream or attachment) and method == "GET":
        proof = OWNER_AGENT_READ_PROOF
    elif item and method == "DELETE":
        proof = OWNER_AGENT_DELETE_PROOF
    elif item and method == "POST" and "action" in body:
        if not REPO_OWNER_AGENT_SESSION_RE.match(url.path):
            return "", None
        if body.get("action") == "stop":
            if set(body) != {"action"}:
                return "", None
            proof = OWNER_AGENT_STOP_PROOF
        else:
            text = _owner_agent_branch_text(body)
            if text is None:
                return "", None
            proof = OWNER_AGENT_BRANCH_PROOF
            digest = hashlib.sha256(text.encode("utf-8")).hexdigest()
    elif item and method == "POST":
        # The follow-up is the whole write, so it is the whole signed body.
        prompt = body.get("prompt")
        if set(body) != {"prompt"} or not isinstance(prompt, str):
            return "", None
        proof = OWNER_AGENT_PROMPT_PROOF
        digest = hashlib.sha256(prompt.encode("utf-8")).hexdigest()
    elif collection and method == "POST":
        text = _owner_agent_queue_text(body)
        if text is None:
            return "", None
        proof = OWNER_AGENT_QUEUE_PROOF
        digest = hashlib.sha256(text.encode("utf-8")).hexdigest()
    else:
        return "", None
    params = parse_qs(url.query)
    user = clean_string(params.get("user", [""])[0], MAX_NODE_NAME).lower()
    ts = clean_string(params.get("ts", [""])[0], 20)
    sig = clean_string(params.get("sig", [""])[0], 200)
    if not user or not sig or not _ts_ok(ts):
        return "", None
    canonical = proof + "\n" + user + "\n" + url.path + "\n"
    if digest:
        canonical += digest + "\n"
    canonical += str(ts)
    account_bi, record = await _signed_account_session(
        env, user, sig, canonical.encode())
    if not account_bi or not record:
        return "", None
    # Same hold-back the session path applies: an account with an unconfirmed
    # address on file cannot write, however it proves itself.
    if (
        method not in UNVERIFIED_SAFE_METHODS
        and _account_email_unverified(record)
    ):
        raise RuntimeError(UNVERIFIED_EMAIL_ERROR)
    return account_bi, record


# --- Image attachments on website-queued agent prompts ---------------------
#
# A launch or follow-up may carry up to five images (the phone's composer
# attaches photos and screenshots). They are validated here, sealed with the
# same row key as the prompt, and kept in org_agent_attachments in chunks small
# enough for a D1 row. The job record carries only their metadata; the node
# that holds the job's lease downloads each one from
# org-agent-jobs/<id>/attachments/<attachmentId> before it runs the CLI.
ORG_AGENT_ATTACHMENT_MAX_COUNT = 5
ORG_AGENT_ATTACHMENT_MAX_BYTES = 5 * 1024 * 1024
# One request's images together. Every byte is held several times over while
# the body is parsed, decoded and sealed, inside a 128 MB isolate.
ORG_AGENT_ATTACHMENT_MAX_TOTAL_BYTES = 10 * 1024 * 1024
# Raw bytes per D1 row: base64 inside the sealed JSON, then base64 of the
# ciphertext, stays near 1.3 MB -- well under D1's 2 MB value limit.
ORG_AGENT_ATTACHMENT_CHUNK_BYTES = 700 * 1024
# The JSON body of a launch/steer that carries images: their base64 plus room
# for the prompt and the envelope.
ORG_AGENT_ATTACHMENT_BODY_LIMIT = (
    (ORG_AGENT_ATTACHMENT_MAX_TOTAL_BYTES + 2) // 3 * 4 + 256 * 1024)
# Rows older than this whose session has no queued/leased/running job left are
# swept the next time anybody stores attachments.
ORG_AGENT_ATTACHMENT_KEEP_MS = 7 * 24 * 60 * 60 * 1000
ORG_AGENT_ATTACHMENT_TYPES = {
    "image/jpeg": "jpg",
    "image/png": "png",
    "image/webp": "webp",
    "image/gif": "gif",
}


def _org_agent_attachment_magic_ok(media_type, data):
    """True when the decoded bytes really are the declared image type."""
    if media_type == "image/jpeg":
        return data[:3] == b"\xff\xd8\xff"
    if media_type == "image/png":
        return data[:8] == b"\x89PNG\r\n\x1a\n"
    if media_type == "image/gif":
        return data[:6] in (b"GIF87a", b"GIF89a")
    if media_type == "image/webp":
        return len(data) >= 12 and data[:4] == b"RIFF" and \
            data[8:12] == b"WEBP"
    return False


def _org_agent_attachment_name(value, index, media_type):
    """A display name only: never a path anywhere (the node picks its own)."""
    name = str(value or "") if isinstance(value, str) else ""
    name = name.replace("\\", "/").rsplit("/", 1)[-1]
    name = "".join(
        ch for ch in name if ch.isprintable() and ch not in "\"<>|:*?")
    name = name.strip().strip(".")[:120]
    return name or "image-%d.%s" % (
        index + 1, ORG_AGENT_ATTACHMENT_TYPES.get(media_type, "img"))


def _org_agent_parse_attachments(value):
    """Validate a request's ``attachments``.

    Returns (items, None) or (None, (code, message)). Each item keeps its
    decoded bytes under "bytes" until it is stored.
    """
    if value is None:
        return [], None
    if not isinstance(value, list):
        return None, (
            "attachment_invalid", "Attachments must be a list of images.")
    if len(value) > ORG_AGENT_ATTACHMENT_MAX_COUNT:
        return None, (
            "too_many_attachments",
            "Attach at most %d images to one prompt."
            % ORG_AGENT_ATTACHMENT_MAX_COUNT)
    max_mb = ORG_AGENT_ATTACHMENT_MAX_BYTES // (1024 * 1024)
    max_encoded = (ORG_AGENT_ATTACHMENT_MAX_BYTES + 2) // 3 * 4
    items = []
    total = 0
    for index, entry in enumerate(value):
        if not isinstance(entry, dict):
            return None, (
                "attachment_invalid", "Attachment %d is not an image."
                % (index + 1))
        media_type = str(entry.get("mediaType") or "").strip().lower()
        if media_type not in ORG_AGENT_ATTACHMENT_TYPES:
            return None, (
                "attachment_invalid",
                "Attachment %d must be a JPEG, PNG, WebP or GIF image."
                % (index + 1))
        encoded = entry.get("data")
        if not isinstance(encoded, str) or not encoded:
            return None, (
                "attachment_invalid", "Attachment %d has no image data."
                % (index + 1))
        if len(encoded) > max_encoded + 4:
            return None, (
                "attachment_too_large",
                "Each image must be %d MB or smaller." % max_mb)
        try:
            data = base64.b64decode(encoded, validate=True)
        except (ValueError, TypeError):
            return None, (
                "attachment_invalid",
                "Attachment %d is not valid base64." % (index + 1))
        if not data:
            return None, (
                "attachment_invalid", "Attachment %d is empty." % (index + 1))
        if len(data) > ORG_AGENT_ATTACHMENT_MAX_BYTES:
            return None, (
                "attachment_too_large",
                "Each image must be %d MB or smaller." % max_mb)
        total += len(data)
        if total > ORG_AGENT_ATTACHMENT_MAX_TOTAL_BYTES:
            return None, (
                "attachment_too_large",
                "The attached images must total %d MB or less." % (
                    ORG_AGENT_ATTACHMENT_MAX_TOTAL_BYTES // (1024 * 1024)))
        if not _org_agent_attachment_magic_ok(media_type, data):
            return None, (
                "attachment_invalid",
                "Attachment %d is not the %s image it claims to be."
                % (index + 1, media_type))
        items.append({
            "id": _ap_uuid(),
            "name": _org_agent_attachment_name(
                entry.get("name"), index, media_type),
            "mediaType": media_type,
            "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
            "bytes": data,
        })
    return items, None


def _org_agent_attachment_meta(items, with_digest=False):
    """What a job (with digest) or a session turn (without) records."""
    keys = ("id", "name", "mediaType", "size") + (
        ("sha256",) if with_digest else ())
    return [{key: item[key] for key in keys} for item in items or []]


async def _org_agent_delete_attachments(env, session_id, ids=None):
    """Drop a session's stored images (only ``ids`` when given). Best-effort:
    the sweep in _org_agent_store_attachments catches whatever this misses."""
    if ids is not None and not ids:
        return
    try:
        if ids is None:
            await d1_run(
                env,
                "DELETE FROM org_agent_attachments WHERE session_id=?",
                session_id,
            )
        else:
            await d1_run(
                env,
                "DELETE FROM org_agent_attachments WHERE session_id=? "
                "AND attachment_id IN (%s)" % ",".join("?" * len(ids)),
                session_id, *ids,
            )
    except Exception:
        pass


async def _org_agent_store_attachments(env, org_bi, repo, session_id, items):
    """Seal and store each image in D1-row-sized chunks. True on success; on
    failure nothing of this call is left behind (best-effort) and the caller
    must not queue the job."""
    if not items:
        return True
    now = int(Date.now())
    try:
        # Orphans: images of a session no job will ever fetch again (the node
        # never claimed, the session was cancelled or purged elsewhere).
        await d1_run(
            env,
            "DELETE FROM org_agent_attachments WHERE created_at<? "
            "AND NOT EXISTS (SELECT 1 FROM org_agent_jobs j "
            "WHERE j.session_id=org_agent_attachments.session_id "
            "AND j.status IN ('queued','leased','running'))",
            now - ORG_AGENT_ATTACHMENT_KEEP_MS,
        )
    except Exception:
        pass
    chunk = ORG_AGENT_ATTACHMENT_CHUNK_BYTES
    try:
        for item in items:
            data = item.pop("bytes", b"") or b""
            for index, start in enumerate(range(0, len(data), chunk)):
                sealed = await encrypt_row(env, {
                    "d": base64.b64encode(
                        data[start:start + chunk]).decode("ascii"),
                })
                await d1_run(
                    env,
                    "INSERT INTO org_agent_attachments "
                    "(session_id,attachment_id,chunk_index,org_bi,repo,data,"
                    "created_at) VALUES (?,?,?,?,?,?,?)",
                    session_id, item["id"], index, org_bi, repo, sealed, now,
                )
    except Exception:
        await _org_agent_delete_attachments(
            env, session_id, [item["id"] for item in items])
        return False
    return True


def _org_agent_attachment_error(error):
    code, message = error
    return _org_agent_error(
        code, message, 400, required_action="fix_attachments")


def _org_agent_attachment_store_failed(provider="", target_node=""):
    return _org_agent_error(
        "attachment_persistence_failed",
        (
            "The attached images could not be saved. No execution was "
            "authorized; try again."
        ),
        503,
        retryable=True,
        required_action="retry_queue",
        provider=provider,
        target_node=target_node,
        degraded=True,
    )


async def _org_agent_request_json(request):
    """The launch/steer body, allowed to be as large as its images need.
    Returns (data, error_response)."""
    try:
        return await bounded_json_request(
            request, ORG_AGENT_ATTACHMENT_BODY_LIMIT), None
    except RequestBodyTooLarge:
        return None, _org_agent_attachment_error((
            "attachment_too_large",
            "The attached images must total %d MB or less." % (
                ORG_AGENT_ATTACHMENT_MAX_TOTAL_BYTES // (1024 * 1024)),
        ))
    except Exception:
        return None, None


async def _org_agent_read_attachment(env, session_id, org_bi, attachment_id):
    rows = await d1_all(
        env,
        "SELECT chunk_index,data,created_at FROM org_agent_attachments "
        "WHERE session_id=? AND org_bi=? AND attachment_id=? "
        "ORDER BY chunk_index",
        session_id, org_bi, attachment_id,
    )
    chunks = []
    created_at = 0
    for expected, part in enumerate(rows or []):
        opened = await decrypt_row(env, part.get("data"))
        if (
            int(part.get("chunk_index") or 0) != expected
            or not isinstance(opened, dict)
        ):
            return b"", 0
        try:
            chunks.append(base64.b64decode(str(opened.get("d") or "")))
        except (ValueError, TypeError):
            return b"", 0
        created_at = int(part.get("created_at") or 0)
    return b"".join(chunks), created_at


async def repo_org_agent_job_attachment_handler(
        env, request, owner, repo, job_id, attachment_id):
    """One image of a leased job, for the node holding that lease only."""
    await ensure_schema(env)
    if method_name(request) != "GET":
        return _org_agent_error(
            "method_not_allowed",
            "Use GET to download an agent job attachment.",
            405,
        )
    if not await _authorized_agent_node_signing_key(env, request, owner):
        return _org_agent_error(
            "unauthorized",
            (
                "This node could not prove current ownership by its account. "
                "Re-link it before fetching the job."
            ),
            401,
            required_action="relink_desktop",
        )
    owner = clean_string(owner, MAX_NODE_NAME).strip().lower()
    repo = clean_string(repo, MAX_REPO_SEGMENT).strip().lower()
    attachment_id = clean_string(attachment_id, 64).strip().lower()
    try:
        lease_id = clean_string(
            (parse_qs(urlparse(request.url).query).get("leaseId") or [""])[0],
            64)
    except Exception:
        lease_id = ""
    row = None
    if lease_id and re.fullmatch(r"[0-9a-f]{32}", attachment_id):
        row = await d1_first(
            env,
            "SELECT id,session_id,org_bi,data FROM org_agent_jobs "
            "WHERE id=? AND target_node=? AND repo=? "
            "AND status IN ('leased','running') AND lease_id=?",
            int(job_id), owner, repo, lease_id,
        )
    if not row:
        return _org_agent_error(
            "lease_not_found",
            (
                "This job lease is no longer current. Refresh the agent "
                "queue before fetching its attachments."
            ),
            409,
            retryable=True,
            required_action="refresh_agent_queue",
            target_node=owner,
        )
    job = await decrypt_row(env, row.get("data"))
    meta = next((
        item for item in (
            job.get("attachments") if isinstance(job, dict)
            and isinstance(job.get("attachments"), list) else [])
        if isinstance(item, dict) and item.get("id") == attachment_id
    ), None)
    data = b""
    if meta:
        data, _created_at = await _org_agent_read_attachment(
            env, row["session_id"], row["org_bi"], attachment_id)
    if (
        not meta or not data
        or len(data) != int(meta.get("size") or 0)
        or hashlib.sha256(data).hexdigest() != meta.get("sha256")
    ):
        return _org_agent_error(
            "attachment_unavailable",
            "This job's attachment is no longer stored on the relay.",
            410,
            required_action="requeue_agent_prompt",
            target_node=owner,
        )
    return _org_agent_attachment_response(data)


def _org_agent_attachment_response(data):
    return JsResponse.new(
        # A JS-owned copy: the body streams after the GIL is released (see
        # git_bytes_response).
        Uint8Array.new(_to_js(bytes(data))),
        to_js({
            "status": 200,
            "headers": {
                "content-type": "application/octet-stream",
                "cache-control": "no-store",
                "x-content-type-options": "nosniff",
                "content-length": str(len(data)),
            },
        }),
    )


async def org_agent_session_attachment_handler(
        env, request, org, repo, session_id, attachment_id,
        owner_scope=False):
    await ensure_schema(env)
    if method_name(request) != "GET":
        return _org_agent_error(
            "method_not_allowed",
            "Use GET to open an image attached to an agent prompt.",
            405,
        )
    signed_account = None
    if _owner_agent_signed_triple(request):
        signed_account = await _owner_agent_signed_session(env, request)
    context, error = await _org_agent_member_context(
        env, request, org, repo, {}, owner_scope=owner_scope,
        signed_account=signed_account, own_session=True)
    if error:
        return error
    row, record = await _org_agent_session_row(
        env, context, clean_string(session_id, 64))
    if not row:
        return _org_agent_error(
            "not_found",
            "This agent session was not found in the selected repository.",
            404,
        )
    attachment_id = clean_string(attachment_id, 64).strip().lower()
    meta, author = None, ""
    history = record.get("history")
    for turn in history if isinstance(history, list) else []:
        items = turn.get("attachments") if isinstance(turn, dict) else None
        for item in items if isinstance(items, list) else []:
            if isinstance(item, dict) and item.get("id") == attachment_id:
                meta, author = item, str(turn.get("author") or "")
    if meta and not owner_scope and (
            author.lower() != str(context.get("actor") or "").lower()):
        return _org_agent_error(
            "attachment_private",
            "Only the member who attached this image can open it.",
            403,
        )
    data, created_at = b"", 0
    if meta:
        data, created_at = await _org_agent_read_attachment(
            env, row["session_id"], row["org_bi"], attachment_id)
    if (
        not data
        or len(data) != int(meta.get("size") or 0)
        or created_at < int(Date.now()) - ORG_AGENT_ATTACHMENT_KEEP_MS
    ):
        return _org_agent_error(
            "attachment_expired",
            (
                "Images attached to agent prompts are kept for a week. This "
                "one is no longer stored."
            ),
            410,
        )
    return _org_agent_attachment_response(data)


async def _owner_agent_cloud_node(env, context, provider, from_device):
    leaving = clean_string(from_device, MAX_NODE_NAME).strip().lower()
    for item in await _org_agent_owner_targets(env, context) or []:
        node = str(item.get("node") or "").strip().lower()
        if (
            item.get("runtimeMode") in ("headless", "runner-pool")
            and provider in (item.get("providers") or [])
            and node
            and node != leaving
            and valid_node_name(node)
        ):
            return node
    return ""


async def org_agent_bots_handler(
        env, request, org, repo, owner_scope=False):
    await ensure_schema(env)
    method = method_name(request)
    if method not in ("GET", "POST"):
        return _org_agent_error(
            "method_not_allowed",
            "Use GET to view agent sessions or POST to queue one.",
            405,
        )
    data = {}
    signed = _owner_agent_signed_triple(request)
    if method == "POST":
        if not signed and not _request_origin_allowed(env, request):
            return _org_agent_error(
                "origin_not_allowed",
                "Queue agent work from the signed-in ForkMesh site.",
                403,
                required_action="use_same_origin",
            )
        data, body_error = await _org_agent_request_json(request)
        if body_error is not None:
            return body_error
        if not isinstance(data, dict):
            return _org_agent_error(
                "invalid_json",
                "The agent request body is not valid JSON.",
                400,
                required_action="retry_request",
            )
    signed_account = None
    if signed:
        signed_account = await _owner_agent_signed_session(env, request, data)
    context, error = await _org_agent_member_context(
        env, request, org, repo, data, owner_scope=owner_scope,
        signed_account=signed_account)
    if error:
        return error
    if method == "GET":
        rows = await _account_agent_scope_rows(
            env, context["orgBi"], context["repo"], ORG_AGENT_MAX_SESSIONS)
        rows.sort(key=lambda row: int(row.get("updated_at") or 0),
                  reverse=True)
        sessions = []
        finished = 0
        last_finished_at = 0
        finished_before = 0
        for row in rows:
            is_active = (str(row.get("status") or "")
                         in ACCOUNT_AGENT_ACTIVE_STATUSES)
            if not is_active and finished >= ORG_AGENT_MAX_SESSIONS:
                if not finished_before:
                    finished_before = max(1, last_finished_at)
                continue
            if not is_active:
                finished += 1
                last_finished_at = int(row.get("updated_at") or 0)
            record = await decrypt_row(env, row.get("data"))
            if (
                isinstance(record, dict)
                and str(record.get("org") or "").lower() == context["org"]
                and str(record.get("repo") or "").lower() == context["repo"]
                and str(record.get("scope") or "organization")
                    == context.get("scope", "organization")
            ):
                sessions.append(_org_agent_hide_private_runner(
                    _org_agent_session_projection(row, record), row, record,
                    context["actor"]))
        targets = await _org_agent_session_targets(env, context)
        return json_response({
            "ok": True,
            "organization": context["org"],
            "repository": context["repo"],
            "scope": context.get("scope", "organization"),
            "memberRole": context["role"],
            "requiredTeam": "" if owner_scope else "engineering",
            "engineeringAccess": context["engineeringAccess"],
            "canQueueAgent": True,
            "canViewAgentSessions": True,
            "canDeleteAgentSessions": True,
            "accessBasis": context["accessBasis"],
            "accessReason": (
                "Owner access to agent-capable repository nodes."
                if owner_scope else
                "Engineering team access for eligible agent mirrors."
            ),
            "targets": targets,
            "providers": list(ORG_AGENT_PROVIDERS),
            "models": {
                provider: list(ORG_AGENT_MODEL_ALIASES[provider].keys())
                for provider in ORG_AGENT_PROVIDERS
            },
            "sessions": sessions,
            "finishedBefore": finished_before,
            "privacyBoundary": (
                "owner-command-encrypted-at-rest"
                if owner_scope else
                "engineering-team-encrypted-at-rest"
            ),
            "securityGate": "not-required",
        }, cache_control="no-store")

    provider = clean_string(data.get("provider"), 40).strip().lower()
    prompt = clean_string(data.get("prompt"), ORG_AGENT_MAX_PROMPT).strip()
    general_bot = provider in ORG_AGENT_GENERAL_PROVIDERS
    if general_bot:
        provider = ORG_AGENT_PROVIDERS[0]
    if provider not in ORG_AGENT_PROVIDERS:
        return _org_agent_error(
            "invalid_provider",
            "Choose Claude Code, Codex, Gemma, or the automatic agent option.",
            400,
            required_action="select_supported_provider",
        )
    model_alias = clean_string(data.get("model"), 30).strip().lower()
    model = _agent_handoff_model(provider, model_alias)
    if signed:
        model = _agent_handoff_model(provider, data.get("model"))
    elif model_alias and not model:
        return _org_agent_error(
            "invalid_model",
            "Choose a model offered for the selected agent provider.",
            400,
            required_action="select_supported_model",
            provider=provider,
        )
    if not prompt:
        return _org_agent_error(
            "prompt_required",
            "Enter the task you want the agent to perform.",
            400,
            required_action="enter_prompt",
        )
    attachments, attachment_error = _org_agent_parse_attachments(
        data.get("attachments"))
    if attachment_error:
        return _org_agent_attachment_error(attachment_error)
    preferred_node = clean_string(
        data.get("targetNode"), MAX_NODE_NAME).strip().lower()
    if signed:
        preferred_node = await _owner_agent_cloud_node(
            env, context, provider, data.get("fromDevice"))
        if not preferred_node:
            import runner_auth
            blocked = runner_auth.submit_block(
                await _org_agent_owner_targets(env, context), provider)
            if blocked:
                return _org_agent_error(
                    blocked["error"], blocked["message"], 409,
                    required_action=blocked["requiredAction"],
                    provider=provider, target_node=blocked["targetNode"])
            return _org_agent_error(
                "no_cloud_agent_node",
                (
                    "No ForkMesh agent node serves this repository with the "
                    "selected provider. Publish the repository to an agent "
                    "node, or untick the cloud to run it on this desktop."
                ),
                409,
                required_action="publish_owned_agent_node",
                provider=provider,
            )
    if preferred_node and not valid_node_name(preferred_node):
        return _org_agent_error(
            "invalid_target_node",
            "Select a valid linked desktop or repository mirror.",
            400,
            required_action="select_eligible_target",
        )
    target_decision = await _org_agent_target_mirror(
        env, context, provider, preferred_node)
    if not target_decision.get("ok") and general_bot:
        # A general bot is runtime-agnostic: take the first supported runtime
        # accepted by the linked desktop instead of failing on one vendor.
        for candidate in ORG_AGENT_PROVIDERS[1:]:
            candidate_decision = await _org_agent_target_mirror(
                env, context, candidate, preferred_node)
            target_decision = candidate_decision
            if candidate_decision.get("ok"):
                provider = candidate
                model = ORG_AGENT_MODEL_ALIASES.get(
                    candidate, {}).get(model_alias) or ""
                break
    if not target_decision.get("ok"):
        target_error = clean_string(
            target_decision.get("error"), 80) or "no_online_agent_mirror"
        target_status = {
            "target_not_owned": 403,
            "org_owner_required": 403,
            "repository_not_published": 409,
            "target_not_desktop": 409,
            "provider_not_advertised": 409,
            "preferred_target_ineligible": 409,
            "no_online_agent_mirror": 503,
        }.get(target_error, 409)
        return _org_agent_error(
            target_error,
            target_decision.get("message") or (
                "The selected agent target is not eligible."
            ),
            target_status,
            retryable=target_decision.get("retryable") is True,
            required_action=target_decision.get("requiredAction") or "",
            provider=target_decision.get("provider") or provider,
            target_node=target_decision.get("targetNode") or "",
            degraded=target_status == 503,
        )
    target_node = clean_string(
        target_decision.get("targetNode"), MAX_NODE_NAME).strip().lower()
    if not valid_node_name(target_node):
        return _org_agent_error(
            "preferred_target_ineligible",
            "The selected agent target is not eligible.",
            409,
            required_action="select_eligible_target",
            provider=provider,
        )
    runner_node = clean_string(
        target_decision.get("runnerNode"), MAX_NODE_NAME).strip().lower()
    shown_node = runner_node if valid_node_name(runner_node) else target_node
    now = int(Date.now())
    session_id = _ap_uuid()
    task_key = (
        "" if owner_scope
        else clean_string(data.get("taskKey"), 96).strip()
    )
    if task_key and not re.fullmatch(
        r"(?:task:[a-z0-9-]{1,48}|"
        r"issue:[a-z0-9-]{1,40}/[a-z0-9._-]{1,60}#[1-9][0-9]{0,8})",
        task_key,
    ):
        return _org_agent_error(
            "invalid_task_key",
            "The linked task reference is invalid.",
            400,
            required_action="select_valid_task",
        )
    try:
        issue_number = int(data.get("issueNumber") or 0)
    except (TypeError, ValueError):
        issue_number = 0
    if issue_number < 0 or issue_number > 10_000_000:
        return _org_agent_error(
            "invalid_issue_number",
            "The linked issue number is invalid.",
            400,
            required_action="select_valid_issue",
        )
    if issue_number and task_key != (
        "issue:%s/%s#%d" % (context["org"], context["repo"], issue_number)
    ):
        return _org_agent_error(
            "invalid_issue_task_key",
            "The issue reference does not match this organization repository.",
            400,
            required_action="select_matching_issue",
        )
    title = clean_string(
        data.get("title") or prompt.split("\n", 1)[0], 160).strip()
    create_pr = data.get("createPr") is True
    import agent_branches
    agent_branches._bind_runtime(globals())
    start_base = ""
    if data.get("baseBranch"):
        start_base = agent_branches.valid_base(
            clean_string(data.get("baseBranch"), 200))
        if not start_base:
            return _org_agent_error(
                "invalid_base_branch",
                "Choose an existing branch to start from.",
                400,
                required_action="select_base_branch",
            )
    start_branch = ""
    if data.get("branchName"):
        start_branch = await agent_branches.next_branch_name(
            env, context["orgBi"], context["repo"],
            clean_string(data.get("branchName"), 120))
    run_mode = _ORG_AGENT_RUN_MODES.get(
        clean_string(data.get("mode"), 20).strip().lower(), "")
    if not owner_scope:
        run_mode = ""
    run_effort = clean_string(data.get("effort"), 20).strip().lower()
    if run_effort not in _ORG_AGENT_RUN_EFFORTS:
        run_effort = ""
    security_record, security_check, queue_status = (
        _org_agent_security_envelope(context, owner_scope))
    record = {
        "org": context["org"],
        "scope": context.get("scope", "organization"),
        "repo": context["repo"],
        "nodeOwner": context["nodeOwner"],
        "targetNode": target_node,
        "provider": provider,
        "title": title,
        "createdBy": context["actor"],
        "localAgentId": 0,
        "taskKey": task_key,
        "issueNumber": issue_number,
        "requestedModel": model or "",
        "requestedModelAlias": model_alias,
        "createPr": create_pr,
        "runnerNode": shown_node,
        "route": clean_string(target_decision.get("route"), 32),
        "branch": start_branch,
        "baseBranch": start_base,
        "security": security_record,
        "history": [{
            "role": "user",
            "author": context["actor"],
            "text": prompt,
            "at": now,
            "state": queue_status,
        }],
    }
    if attachments:
        record["history"][0]["attachments"] = _org_agent_attachment_meta(
            attachments)
    job = {
        "kind": "start",
        "sessionId": session_id,
        "organization": context["org"],
        "repositoryOwner": target_node,
        "repositoryName": context["repo"],
        "provider": provider,
        "prompt": prompt,
        "taskKey": task_key,
        "issueNumber": issue_number,
        "model": model or "",
        "createPr": create_pr,
        "requestedBy": context["actor"],
        "securityCheck": security_check,
    }
    if start_branch:
        job["branch"] = start_branch
    if start_base:
        job["baseBranch"] = start_base
    if run_mode:
        job["mode"] = run_mode
    if run_effort:
        job["effort"] = run_effort
    if attachments:
        job["attachments"] = _org_agent_attachment_meta(attachments, True)
        if not await _org_agent_store_attachments(
                env, context["orgBi"], context["repo"], session_id,
                attachments):
            return _org_agent_attachment_store_failed(provider, target_node)
    runner_owner = clean_string(
        target_decision.get("runnerOwner"), MAX_NODE_NAME).strip().lower()
    if runner_owner:
        job["organization"] = runner_owner
        record["runnerOwner"] = runner_owner
    runner_pin = clean_string(
        target_decision.get("runnerPin"), MAX_NODE_NAME).strip().lower()
    if runner_owner and valid_node_name(runner_pin):
        job["runner"] = runner_pin
        record["runnerPrivate"] = True
    session_data = await encrypt_row(env, record)
    job_data = await encrypt_row(env, job)
    try:
        await _contribution_run_batch(env, [
            (
                "INSERT INTO org_agent_sessions "
                "(session_id,org_bi,repo,target_node,provider,status,"
                "created_by_bi,data,created_at,updated_at,completed_at) "
                "VALUES (?,?,?,?,?,?,?,?,?,?,0)",
                (
                    session_id, context["orgBi"], context["repo"],
                    target_node, provider, queue_status,
                    context["accountBi"], session_data, now, now,
                ),
            ),
            (
                "INSERT INTO org_agent_jobs "
                "(session_id,org_bi,target_node,repo,provider,status,lease_id,"
                "data,created_at,updated_at) "
                "VALUES (?,?,?,?,?,'queued','',?,?,?)",
                (
                    session_id, context["orgBi"], target_node,
                    context["repo"], provider, job_data, now, now,
                ),
            ),
        ])
    except Exception:
        if attachments:
            await _org_agent_delete_attachments(env, session_id)
        return _org_agent_error(
            "queue_persistence_failed",
            (
                "The task could not be saved to the agent queue. No execution "
                "was authorized; try again."
            ),
            503,
            retryable=True,
            required_action="retry_queue",
            provider=provider,
            target_node=target_node,
            degraded=True,
        )
    audit_scope = "owner" if owner_scope else "organization"
    await _audit_sensitive_action(
        env, context["actor"], audit_scope + ".agent_start",
        audit_scope + "_agent", context["org"] + "/" + context["repo"] +
        "/" + session_id, "success",
        {
            "provider": provider,
            "model": model or "provider-default",
            "issueNumber": issue_number,
            "targetNode": target_node,
            "createPr": create_pr,
        })
    await notify_repo_host(env, target_node, context["repo"], "org-agents")
    await _ring_cloud_agent_readers(env, record)
    queue_state = clean_string(
        target_decision.get("queueState"), 40) or "ready_for_claim"
    if queue_state == "waiting_for_desktop":
        queue_message = (
            f"Saved for {shown_node}. It will run as soon as ForkMesh "
            "reconnects."
            if owner_scope else
            f"Saved for {shown_node}. It will run after ForkMesh reconnects "
            "and the required security preflight approves it."
        )
    else:
        queue_message = (
            f"Sent to {shown_node}. It will start as soon as the node claims "
            "it."
            if owner_scope else
            f"Saved for {shown_node}. It will run after the required "
            "security preflight approves it."
        )
    return json_response({
        "ok": True,
        "queueState": queue_state,
        "targetOnline": target_decision.get("targetOnline") is True,
        "message": queue_message,
        "session": _org_agent_session_projection({
            "session_id": session_id,
            "repo": context["repo"],
            "target_node": target_node,
            "provider": provider,
            "status": queue_status,
            "created_at": now,
            "updated_at": now,
            "completed_at": 0,
        }, record),
    }, status=202, cache_control="no-store")


# How many of the account's cloud runs one read answers with, newest first.
ACCOUNT_AGENT_MAX_SESSIONS = 40
# And how much of each one's conversation rides along. A session's history is
# up to ORG_AGENT_MAX_HISTORY turns of up to MAX_AGENT_TRANSCRIPT characters,
# which is a fine answer for one repository and a megabytes-long one for a
# whole account: the list carries the tail, says it did, and a client that
# opens the run reads the rest from the session's own route.
ACCOUNT_AGENT_HISTORY_TURNS = 8
ACCOUNT_AGENT_MAX_ORGS = 20
ACCOUNT_AGENT_MAX_ACTIVE = 500
ACCOUNT_AGENT_ACTIVE_STATUSES = ("queued", "running", "security_pending")


async def _account_agent_scope_rows(
        env, org_bi, repo=None, finished_limit=None):
    placeholders = ",".join("?" for _ in ACCOUNT_AGENT_ACTIVE_STATUSES)
    columns = ("SELECT session_id,org_bi,repo,target_node,provider,status,data,"
               "created_at,updated_at,completed_at FROM org_agent_sessions ")
    scope = "WHERE org_bi=? "
    scope_args = [org_bi]
    if repo is not None:
        scope += "AND repo=? "
        scope_args.append(repo)
    if finished_limit is None:
        finished_limit = ACCOUNT_AGENT_MAX_SESSIONS
    active = await d1_all(
        env,
        columns + scope + "AND status IN (" + placeholders + ") "
        "ORDER BY updated_at DESC LIMIT ?",
        *scope_args, *ACCOUNT_AGENT_ACTIVE_STATUSES, ACCOUNT_AGENT_MAX_ACTIVE,
    )
    recent = await d1_all(
        env,
        columns + scope + "AND status NOT IN (" + placeholders + ") "
        "ORDER BY updated_at DESC LIMIT ?",
        *scope_args, *ACCOUNT_AGENT_ACTIVE_STATUSES, finished_limit + 1,
    )
    seen = set()
    rows = []
    for row in list(active or []) + list(recent or []):
        key = str(row.get("session_id") or "")
        if key in seen:
            continue
        seen.add(key)
        rows.append(row)
    return rows


async def _account_org_agent_rows(env, actor, account_bi):
    member_bi = str(await blind_index(env, actor))
    orgs = await d1_all(
        env,
        "SELECT o.org_bi AS org_bi,o.name AS name,m.role AS role "
        "FROM org_members m JOIN orgs o ON o.org_bi=m.org_bi "
        "WHERE m.member_bi=? ORDER BY o.name LIMIT ?",
        member_bi, ACCOUNT_AGENT_MAX_ORGS,
    )
    found = []
    for org in orgs or []:
        org_bi = str(org.get("org_bi") or "")
        name = clean_string(org.get("name"), MAX_NODE_NAME).strip().lower()
        if not org_bi or not name:
            continue
        if not _org_rbac.org_role_allows(
                str(org.get("role") or ""), "organization.agents.read"):
            continue
        if not await _org_floor_team_rows(
                env, org_bi, "engineering", member_bi=str(account_bi)):
            continue
        rows = await _account_agent_scope_rows(env, org_bi)
        found.extend((name, row) for row in rows or [])
    return found


async def account_agent_sessions_handler(env, request):
    """Every cloud agent session this account owns, across its repositories.

    The per-repository list answers "what is running in this repository"; a
    desktop and a phone also need "what is running anywhere", because the run
    a mirror claimed belongs to whichever repository the reader is not looking
    at. Fanning the per-repository route out over the catalog would cost one
    request per repository on a client that must not poll at all, so this is
    one indexed read of the account's own queue instead
    (idx_org_agent_sessions_scope leads with org_bi).

    An organization's sessions live under the organization's blind index; they
    join the list only for a member the organization agent route would let
    read them (docs/agents/README.md).
    """
    await ensure_schema(env)
    if method_name(request) != "GET":
        return _org_agent_error(
            "method_not_allowed",
            "Use GET to read this account's cloud agent sessions.",
            405,
        )
    # Same two credentials as the repository list: a desktop that launched
    # silently holds the account key and no session token and signs the URL;
    # a phone or a browser sends its session token. A triple on the URL is
    # the only credential considered once it is there.
    if _owner_agent_signed_triple(request):
        account_bi, account = await _owner_agent_signed_session(env, request)
    else:
        account_bi, account = await _account_session_record(env, request, None)
    actor = clean_string(
        (account or {}).get("name"), MAX_NODE_NAME).strip().lower()
    if not account_bi or not actor:
        return _org_agent_error(
            "invalid_session",
            "Sign in again to read this account's cloud agent sessions.",
            401,
            required_action="sign_in",
        )
    rows = await _account_agent_scope_rows(env, str(account_bi))
    candidates = [("", row) for row in rows or []]
    candidates.extend(await _account_org_agent_rows(env, actor, account_bi))
    candidates.sort(
        key=lambda item: int(item[1].get("updated_at") or 0), reverse=True)
    projections = []
    finished = 0
    last_finished_at = 0
    finished_before = 0
    for org, row in candidates:
        is_active = str(row.get("status") or "") in ACCOUNT_AGENT_ACTIVE_STATUSES
        if not is_active and finished >= ACCOUNT_AGENT_MAX_SESSIONS:
            if not finished_before:
                finished_before = max(1, last_finished_at)
            continue
        record = await decrypt_row(env, row.get("data"))
        if not isinstance(record, dict):
            continue
        scope = str(record.get("scope") or "organization")
        if org:
            if (
                scope != "organization"
                or str(record.get("org") or "").strip().lower() != org
            ):
                continue
        elif scope != "owner":
            continue
        projection = _org_agent_hide_private_runner(
            _org_agent_session_projection(row, record), row, record, actor)
        projection["scope"] = scope
        projections.append(projection)
        if not is_active:
            finished += 1
            last_finished_at = int(row.get("updated_at") or 0)
    sessions = []
    active = 0
    for projection in projections:
        if projection["status"] in ("running", "queued", "security_pending"):
            active += 1
        history = projection.get("history") or []
        if len(history) > ACCOUNT_AGENT_HISTORY_TURNS:
            projection["history"] = history[-ACCOUNT_AGENT_HISTORY_TURNS:]
            projection["historyTruncated"] = True
        sessions.append(projection)
    return json_response({
        "ok": True,
        "account": actor,
        "scope": "owner",
        "activeCount": active,
        "finishedBefore": finished_before,
        "sessions": sessions,
    }, cache_control="no-store")


ACCOUNT_DESKTOP_AGENT_MAX_REPOS = 500
ACCOUNT_DESKTOP_AGENT_MAX_ROWS = 200
ACCOUNT_DESKTOP_AGENT_CHUNK = 50


async def account_desktop_agents_handler(env, request):
    await ensure_schema(env)
    if method_name(request) != "GET":
        return _org_agent_error(
            "method_not_allowed",
            "Use GET to read this account's desktop agent sessions.",
            405,
        )
    account_bi, account = await _account_session_record(env, request, None)
    actor = clean_string(
        (account or {}).get("name"), MAX_NODE_NAME).strip().lower()
    if not account_bi or not actor:
        return _org_agent_error(
            "invalid_session",
            "Sign in again to read this account's desktop agent sessions.",
            401,
            required_action="sign_in",
        )
    repositories = {}
    for owner in await _profile_catalog_owner_names(env, actor):
        if len(repositories) >= ACCOUNT_DESKTOP_AGENT_MAX_REPOS:
            break
        owner_bi = await blind_index(env, owner)
        rows = await d1_all(
            env,
            "SELECT key_bi, data FROM repositories WHERE owner_bi=? LIMIT ?",
            owner_bi, ACCOUNT_DESKTOP_AGENT_MAX_REPOS,
        )
        for row in rows or []:
            key = str(row.get("key_bi") or "")
            if key and key not in repositories:
                repositories[key] = (owner, row.get("data"))
    keys = list(repositories)[:ACCOUNT_DESKTOP_AGENT_MAX_REPOS]
    found = []
    for start in range(0, len(keys), ACCOUNT_DESKTOP_AGENT_CHUNK):
        chunk = keys[start:start + ACCOUNT_DESKTOP_AGENT_CHUNK]
        rows = await d1_all(
            env,
            "SELECT repo_bi, agent_id, updated_at FROM repo_agents "
            "WHERE repo_bi IN (" + ",".join("?" for _ in chunk) + ") "
            "ORDER BY updated_at DESC LIMIT ?",
            *chunk, ACCOUNT_DESKTOP_AGENT_MAX_ROWS + 1,
        )
        found.extend(rows or [])
    found.sort(key=lambda row: int(row.get("updated_at") or 0), reverse=True)
    truncated = len(found) > ACCOUNT_DESKTOP_AGENT_MAX_ROWS
    found = found[:ACCOUNT_DESKTOP_AGENT_MAX_ROWS]
    names = {}
    agents = []
    for row in found:
        key = str(row.get("repo_bi") or "")
        if key not in names:
            owner, data = repositories.get(key, ("", None))
            record = await decrypt_row(env, data) if data else None
            names[key] = (owner, clean_string(
                (record or {}).get("name"), MAX_REPO_SEGMENT).strip())
        owner, repo = names[key]
        if not owner or not repo:
            continue
        agents.append({
            "id": clean_string(row.get("agent_id"), 32),
            "owner": owner,
            "repo": repo,
            "node": owner,
            "reportedAt": int(row.get("updated_at") or 0),
        })
    return json_response({
        "ok": True,
        "account": actor,
        "agents": agents,
        "truncated": truncated,
        "privacyBoundary": "owner-only-e2ee",
        "serverCanDecrypt": False,
    }, cache_control="no-store")


# ---- Desktop "Move to cloud" ------------------------------------------------
# The desktop's agent detail pane can hand a session it is running to the
# cloud.  Everything the work itself needs already travels in Git: the desktop
# commits and publishes the session's branch to the served mirror first, so the
# continuation only has to name the branch, the base it was cut from, the
# runtime, and the task to carry on with.  The transcript deliberately does NOT
# travel here -- it is owner-sealed on the /agents snapshot transport, which
# the receiving device decrypts with its own key, and copying it into a
# relay-readable job row would give the relay exactly the plaintext that
# transport exists to withhold.
#
# Authorization differs from the website's queue for one structural reason: a
# desktop holds no browser session (it authenticates with its owner signing
# key), so this route takes the same signed-owner proof the agent snapshot push
# and the job drain already take.  That proof names the account, and the job is
# queued into that same account's node namespace -- a node can only hand work
# to the account it signs for.
_AGENT_HANDOFF_MAX_TITLE = 160
_AGENT_HANDOFF_MAX_BRANCH = 200


def _agent_handoff_model(provider, requested):
    """Resolve the model a handoff asks for, or "" for the node's default.

    The website queue speaks in aliases ("opus"); a desktop session already
    holds the resolved id ("claude-opus-5").  Accept either spelling of a
    model this provider actually offers and fall back to the provider default
    rather than failing the move: the receiving node re-checks the id against
    its own allowlist before it launches anything.
    """
    aliases = ORG_AGENT_MODEL_ALIASES.get(provider, {})
    requested = clean_string(requested, 120).strip().lower()
    if not requested:
        return ""
    if requested in aliases:
        return aliases[requested]
    return requested if requested in set(aliases.values()) else ""


async def _agent_handoff_mirror_target(env, owner, repo, provider, from_device):
    """The owned mirror node the moved session should continue on, or "".

    "The cloud" is the account's headless mirror nodes: each drains the job
    queue of its own node name, never the account's, so a handoff left in the
    account namespace only ever reached another of the owner's desktops. Pick
    the freshest mirror that serves this repository and advertises the
    runtime; the device giving the session up is never its own destination.
    """
    owned = set(await _profile_catalog_owner_names(env, owner))
    owned.add(owner)
    targets = await _org_agent_owner_targets(env, {
        "scope": "owner",
        "nodeOwner": owner,
        "repo": repo,
        "ownedNodes": sorted(owned),
    })
    leaving = str(from_device or "").strip().lower()
    for item in targets or []:
        node = str(item.get("node") or "").strip().lower()
        if (
            item.get("runtimeMode") == "headless"
            and provider in (item.get("providers") or [])
            and node
            and node != leaving
            and valid_node_name(node)
        ):
            return node, item.get("online") is True
    return "", False


async def _agent_handoff_signer(env, request, owner, repo):
    key = await _authorized_owner_signing_key(env, request, owner)
    if key:
        return owner, key
    params = parse_qs(urlparse(request.url).query)
    signer = clean_string(
        params.get("owner", [""])[0], MAX_NODE_NAME).strip().lower()
    node = clean_string(owner, MAX_NODE_NAME).strip().lower()
    if not valid_node_name(signer) or signer == node:
        return "", ""
    if not (
        await _account_owns_node(env, signer, node)
        or await _org_owner_of_linked_repo(env, node, repo, signer)
    ):
        return "", ""
    key = await _authorized_owner_signing_key(env, request, signer)
    return (signer, key) if key else ("", "")


async def agent_handoff_handler(env, request, owner, repo, agent_id):
    await ensure_schema(env)
    if method_name(request) != "POST":
        return _org_agent_error(
            "method_not_allowed",
            "Use POST to move an agent session into the cloud.",
            405,
        )
    # The exact device key that signed, not just "somebody who may sign for
    # this account": it is what keeps this handoff from coming straight back
    # (see the drain's fromKey skip in repo_org_agent_jobs_handler).
    signer, from_key = await _agent_handoff_signer(env, request, owner, repo)
    if not from_key:
        return _org_agent_error(
            "unauthorized",
            (
                "This desktop could not prove it owns the account this "
                "session belongs to. Re-link it to the account and try again."
            ),
            401,
            required_action="relink_desktop",
        )
    owner = clean_string(owner, MAX_NODE_NAME).strip().lower()
    repo = clean_string(repo, MAX_REPO_SEGMENT).strip().lower()
    if not valid_node_name(owner) or not safe_segment(repo):
        return _org_agent_error(
            "not_found",
            "The requested repository was not found.",
            404,
        )
    try:
        data = await bounded_json_request(request)
    except Exception:
        return _org_agent_error(
            "invalid_json",
            "The handoff request body is not valid JSON.",
            400,
            required_action="retry_request",
        )
    provider = clean_string(data.get("provider"), 40).strip().lower()
    if provider not in ORG_AGENT_PROVIDERS:
        return _org_agent_error(
            "invalid_provider",
            "Only Claude Code, Codex and Gemma sessions can continue in the cloud.",
            400,
            required_action="select_supported_provider",
        )
    prompt = clean_string(data.get("prompt"), ORG_AGENT_MAX_PROMPT).strip()
    if not prompt:
        return _org_agent_error(
            "prompt_required",
            "A handoff must carry the task the cloud agent is to continue.",
            400,
            required_action="enter_prompt",
        )
    branch = clean_string(data.get("branch"), _AGENT_HANDOFF_MAX_BRANCH).strip()
    if not branch or not re.fullmatch(BRANCH_OP_NAME_PATTERN, branch):
        return _org_agent_error(
            "invalid_branch",
            (
                "A handoff must name the published branch that holds this "
                "session's work."
            ),
            400,
            required_action="publish_session_branch",
        )
    base_branch = clean_string(
        data.get("baseBranch"), _AGENT_HANDOFF_MAX_BRANCH).strip()
    if base_branch and not re.fullmatch(BRANCH_OP_NAME_PATTERN, base_branch):
        return _org_agent_error(
            "invalid_branch",
            "The base branch this session was cut from is invalid.",
            400,
            required_action="publish_session_branch",
        )
    base_ref = clean_string(data.get("baseRef"), 64).strip().lower()
    if base_ref and not re.fullmatch(r"[0-9a-f]{7,64}", base_ref):
        return _org_agent_error(
            "invalid_base_ref",
            "The base commit this session was cut from is invalid.",
            400,
            required_action="publish_session_branch",
        )
    try:
        issue_number = int(data.get("issueNumber") or 0)
    except (TypeError, ValueError):
        issue_number = 0
    if issue_number < 0 or issue_number > 10_000_000:
        return _org_agent_error(
            "invalid_issue_number",
            "The linked issue number is invalid.",
            400,
            required_action="select_valid_issue",
        )
    # The device that is giving the session up. It drains this very queue, so
    # without a name to skip on it would immediately claim its own handoff back
    # and the session would never leave the desk it was moved off.
    from_device = clean_string(data.get("fromDevice"), MAX_NODE_NAME).strip()
    model = _agent_handoff_model(provider, data.get("model"))
    title = clean_string(
        data.get("title") or prompt.split("\n", 1)[0],
        _AGENT_HANDOFF_MAX_TITLE).strip()
    create_pr = data.get("createPr") is True
    # Queue for an owned mirror node when there is one, so the session carries
    # on in the cloud; otherwise the account namespace, which the owner's
    # other desktops drain.
    mirror_node, mirror_online = await _agent_handoff_mirror_target(
        env, owner, repo, provider, from_device)
    target_node = mirror_node or owner
    account_bi = str(await blind_index(env, owner))
    signer = clean_string(signer, MAX_NODE_NAME).strip().lower() or owner
    signer_bi = (
        account_bi if signer == owner
        else str(await blind_index(env, signer)))
    now = int(Date.now())
    session_id = _ap_uuid()
    # The signer owns this node namespace, so it is the same person who could
    # open a terminal on the receiving node and resume the branch by hand: the
    # handoff queues like any website request (see
    # _org_agent_security_envelope).
    security_record, security_check, queue_status = (
        _org_agent_security_envelope({"actor": owner}, True))
    record = {
        "org": owner,
        "scope": "owner",
        "repo": repo,
        "nodeOwner": owner,
        "targetNode": target_node,
        "provider": provider,
        "title": title,
        "createdBy": signer,
        "localAgentId": 0,
        "taskKey": "",
        "issueNumber": issue_number,
        "requestedModel": model,
        "requestedModelAlias": "",
        "createPr": create_pr,
        "security": security_record,
        "handoff": {
            "branch": branch,
            "baseBranch": base_branch,
            "baseRef": base_ref,
            "fromDevice": from_device,
            "fromKey": from_key,
            "sourceAgentId": clean_string(agent_id, 40),
        },
        "history": [{
            "role": "user",
            "author": signer,
            "text": prompt,
            "at": now,
            "state": queue_status,
        }],
    }
    job = {
        "kind": "continue",
        "sessionId": session_id,
        "organization": owner,
        "repositoryOwner": target_node,
        "repositoryName": repo,
        "provider": provider,
        "prompt": prompt,
        "taskKey": "",
        "issueNumber": issue_number,
        "model": model,
        "createPr": create_pr,
        "requestedBy": owner,
        "securityCheck": security_check,
        "branch": branch,
        "baseBranch": base_branch,
        "baseRef": base_ref,
        "title": title,
        "fromDevice": from_device,
        "fromKey": from_key,
        "sourceAgentId": clean_string(agent_id, 40),
    }
    try:
        await _contribution_run_batch(env, [
            (
                "INSERT INTO org_agent_sessions "
                "(session_id,org_bi,repo,target_node,provider,status,"
                "created_by_bi,data,created_at,updated_at,completed_at) "
                "VALUES (?,?,?,?,?,?,?,?,?,?,0)",
                (
                    session_id, signer_bi, repo, target_node, provider,
                    queue_status, signer_bi,
                    await encrypt_row(env, record), now, now,
                ),
            ),
            (
                "INSERT INTO org_agent_jobs "
                "(session_id,org_bi,target_node,repo,provider,status,lease_id,"
                "data,created_at,updated_at) "
                "VALUES (?,?,?,?,?,'queued','',?,?,?)",
                (
                    session_id, signer_bi, target_node, repo, provider,
                    await encrypt_row(env, job), now, now,
                ),
            ),
        ])
    except Exception:
        return _org_agent_error(
            "queue_persistence_failed",
            (
                "The handoff could not be saved to the agent queue. The "
                "session is still on this device; try again."
            ),
            503,
            retryable=True,
            required_action="retry_queue",
            provider=provider,
            target_node=target_node,
            degraded=True,
        )
    await _audit_sensitive_action(
        env, owner, "owner.agent_handoff", "owner_agent",
        owner + "/" + repo + "/" + session_id, "success",
        {
            "provider": provider,
            "model": model or "provider-default",
            "branch": branch,
            "fromDevice": from_device,
            "targetNode": target_node,
        })
    await notify_repo_host(env, target_node, repo, "org-agents")
    await _ring_cloud_agent_readers(env, record)
    if mirror_node:
        return json_response({
            "ok": True,
            "sessionId": session_id,
            "queueState": (
                "ready_for_claim" if mirror_online else "waiting_for_node"),
            "branch": branch,
            "targetNode": mirror_node,
            "message": (
                "Moved to the cloud. " + mirror_node + " continues the "
                "session on " + branch + "."
                if mirror_online else
                "Queued for " + mirror_node + " on " + branch + ". The "
                "mirror has not checked in lately, so it continues the "
                "session when it next comes online."
            ),
        }, status=202, cache_control="no-store")
    # The device giving the session up is skipped on the drain, so whether
    # anything can actually claim this depends on the account having another
    # enrolled node. Say which of the two it is rather than leaving a stopped
    # session and an optimistic message.
    others = await d1_first(
        env,
        "SELECT COUNT(*) AS n FROM account_devices "
        "WHERE account_bi=? AND enabled=1 AND revoked_at=0 AND pubkey<>?",
        account_bi, from_key,
    )
    try:
        other_nodes = max(0, int((others or {}).get("n") or 0))
    except (TypeError, ValueError):
        other_nodes = 0
    return json_response({
        "ok": True,
        "sessionId": session_id,
        "queueState": "ready_for_claim" if other_nodes else "waiting_for_node",
        "branch": branch,
        "otherNodes": other_nodes,
        "message": (
            "Moved to the cloud. The next of your nodes to come online picks "
            "the session up on " + branch + "."
            if other_nodes else
            "Queued for the cloud on " + branch + ". No other node is "
            "enrolled on this account yet, so it waits until one is."
        ),
    }, status=202, cache_control="no-store")


async def org_agent_bot_handler(
        env, request, org, repo, session_id, owner_scope=False):
    await ensure_schema(env)
    method = method_name(request)
    if method not in ("GET", "POST", "DELETE"):
        return _org_agent_error(
            "method_not_allowed",
            (
                "Use GET to view this session, POST to send a follow-up (or "
                "{\"action\": \"stop\"} to stop it), or DELETE to remove it."
            ),
            405,
        )
    data = {}
    # A key-signed desktop request has no browser origin to check. It is safe
    # to waive only because the triple then becomes the sole credential (see
    # _org_agent_member_context): a junk signature authenticates as nobody.
    signed = _owner_agent_signed_triple(request)
    if method in ("POST", "DELETE") and not signed:
        if not _request_origin_allowed(env, request):
            return _org_agent_error(
                "origin_not_allowed",
                (
                    "Manage agent sessions from the signed-in ForkMesh site."
                ),
                403,
                required_action="use_same_origin",
            )
    if method == "POST":
        data, body_error = await _org_agent_request_json(request)
        if body_error is not None:
            return body_error
        if not isinstance(data, dict):
            return _org_agent_error(
                "invalid_json",
                "The agent follow-up body is not valid JSON.",
                400,
                required_action="retry_request",
            )
    signed_account = None
    if signed:
        signed_account = await _owner_agent_signed_session(env, request, data)
    stopping = (
        method == "POST"
        and clean_string(data.get("action"), 20).lower() == "stop")
    branch_action = (
        clean_string(data.get("action"), 20).lower()
        if method == "POST" else "")
    if branch_action not in ("merge", "cleanup", "link-pull", "update"):
        branch_action = ""
    context, error = await _org_agent_member_context(
        env, request, org, repo, data, owner_scope=owner_scope,
        signed_account=signed_account,
        own_session=method in ("GET", "DELETE") or stopping)
    if error:
        return error
    row, record = await _org_agent_session_row(
        env, context, clean_string(session_id, 64))
    if not row:
        return _org_agent_error(
            "not_found",
            "This agent session was not found in the selected repository.",
            404,
        )
    if method == "GET":
        session = _org_agent_session_projection(row, record)
        try:
            queue = await _org_agent_queue_projection(env, row, record)
        except Exception:
            queue = {}
        if queue:
            session["queue"] = queue
        _org_agent_hide_private_runner(session, row, record, context["actor"])
        return json_response({
            "ok": True,
            "session": session,
        }, cache_control="no-store")
    if method == "DELETE":
        try:
            await _contribution_run_batch(env, [
                (
                    "DELETE FROM org_agent_attachments WHERE session_id=? "
                    "AND org_bi=? AND repo=?",
                    (row["session_id"], context["orgBi"], context["repo"]),
                ),
                (
                    "DELETE FROM org_agent_jobs WHERE session_id=? "
                    "AND org_bi=? AND repo=?",
                    (row["session_id"], context["orgBi"], context["repo"]),
                ),
                (
                    "DELETE FROM org_agent_sessions WHERE session_id=? "
                    "AND org_bi=? AND repo=?",
                    (row["session_id"], context["orgBi"], context["repo"]),
                ),
            ])
        except Exception:
            return _org_agent_error(
                "session_delete_failed",
                "The agent session could not be deleted. Try again.",
                503,
                retryable=True,
                required_action="retry_delete",
                degraded=True,
            )
        try:
            streams = _agent_streams()
            await streams.forget(
                streams.WorkerStore(env), row["session_id"])
        except Exception:
            pass
        audit_scope = "owner" if owner_scope else "organization"
        await _audit_sensitive_action(
            env, context["actor"], audit_scope + ".agent_delete",
            audit_scope + "_agent", context["org"] + "/" +
            context["repo"] + "/" + row["session_id"], "success",
            {
                "provider": str(row.get("provider") or ""),
                "status": str(row.get("status") or ""),
                "targetNode": str(row.get("target_node") or ""),
            })
        return json_response({
            "ok": True,
            "deleted": True,
            "sessionId": row["session_id"],
            "message": "Agent session deleted.",
        }, cache_control="no-store")
    if stopping:
        return await _org_agent_stop_session(
            env, context, row, record, owner_scope)
    if branch_action:
        import agent_branches
        agent_branches._bind_runtime(globals())
        return await agent_branches.session_action(
            env, request, context, row, record, branch_action, data,
            owner_scope)
    # A finished run is still a conversation: the node keeps the session's
    # branch (and, for Claude Code, its provider conversation) and a steer job
    # resumes both, the way the desktop continues a Done session from its
    # composer. Only a session no node ever started (no localAgentId, checked
    # next) or one the preflight rejected has nothing to resume.
    if str(row.get("status") or "") not in (
            "running", "queued", "completed", "failed"):
        return _org_agent_error(
            "session_not_promptable",
            "This agent session is not running and cannot accept a follow-up.",
            409,
            required_action="start_new_session",
        )
    blocked = await _org_agent_private_runner_block(
        env, row, record, context["actor"])
    if blocked:
        return blocked
    local_agent_id = int(record.get("localAgentId") or 0)
    if local_agent_id <= 0:
        return _org_agent_error(
            "agent_not_ready",
            (
                "The desktop has not started this agent session yet. Wait for "
                "it to reconnect and finish the security preflight."
            ),
            409,
            retryable=True,
            required_action="wait_for_agent",
            provider=str(row.get("provider") or ""),
            target_node=str(row.get("target_node") or ""),
        )
    prompt = clean_string(data.get("prompt"), ORG_AGENT_MAX_PROMPT).strip()
    if not prompt:
        return _org_agent_error(
            "prompt_required",
            "Enter the follow-up you want to send to the agent.",
            400,
            required_action="enter_prompt",
        )
    attachments, attachment_error = _org_agent_parse_attachments(
        data.get("attachments"))
    if attachment_error:
        return _org_agent_attachment_error(attachment_error)
    now = int(Date.now())
    security_record, security_check, queue_status = (
        _org_agent_security_envelope(context, owner_scope))
    history = record.get("history")
    if not isinstance(history, list):
        history = []
    turn = {
        "role": "user",
        "author": context["actor"],
        "text": prompt,
        "at": now,
        "state": queue_status,
    }
    if attachments:
        turn["attachments"] = _org_agent_attachment_meta(attachments)
    history.append(turn)
    record["history"] = history[-ORG_AGENT_MAX_HISTORY:]
    record["security"] = security_record
    if isinstance(record.get("merge"), dict) and record["merge"]:
        record["lastMerge"] = record.pop("merge")
    job = {
        "kind": "steer",
        "sessionId": str(row.get("session_id") or ""),
        "organization": context["org"],
        "repositoryOwner": str(row.get("target_node") or ""),
        "repositoryName": context["repo"],
        "provider": str(row.get("provider") or ""),
        "prompt": prompt,
        "localAgentId": local_agent_id,
        "taskKey": clean_string(record.get("taskKey"), 96),
        "requestedBy": context["actor"],
        "securityCheck": security_check,
    }
    import agent_branches
    job.update(agent_branches.runner_job_owner(record))
    session_branch = agent_branches.projection_extras(row, record)
    if session_branch["branch"]:
        job["branch"] = session_branch["branch"]
        job["baseBranch"] = session_branch["baseBranch"]
        job["title"] = clean_string(record.get("title"), 160)
    if attachments:
        job["attachments"] = _org_agent_attachment_meta(attachments, True)
        if not await _org_agent_store_attachments(
                env, context["orgBi"], context["repo"],
                str(row.get("session_id") or ""), attachments):
            return _org_agent_attachment_store_failed(
                str(row.get("provider") or ""),
                str(row.get("target_node") or ""))
    session_data = await encrypt_row(env, record)
    job_data = await encrypt_row(env, job)
    try:
        await _contribution_run_batch(env, [
            (
                "UPDATE org_agent_sessions SET status=?,"
                "data=?,updated_at=? WHERE session_id=? AND org_bi=?",
                (
                    queue_status, session_data, now, row["session_id"],
                    context["orgBi"],
                ),
            ),
            (
                "INSERT INTO org_agent_jobs "
                "(session_id,org_bi,target_node,repo,provider,status,lease_id,"
                "data,created_at,updated_at) "
                "VALUES (?,?,?,?,?,'queued','',?,?,?)",
                (
                    row["session_id"], context["orgBi"], row["target_node"],
                    context["repo"], row["provider"], job_data, now, now,
                ),
            ),
        ])
    except Exception:
        if attachments:
            await _org_agent_delete_attachments(
                env, str(row.get("session_id") or ""),
                [item["id"] for item in attachments])
        return _org_agent_error(
            "queue_persistence_failed",
            (
                "The follow-up could not be saved to the agent queue. No "
                "execution was authorized; try again."
            ),
            503,
            retryable=True,
            required_action="retry_queue",
            provider=str(row.get("provider") or ""),
            target_node=str(row.get("target_node") or ""),
            degraded=True,
        )
    audit_scope = "owner" if owner_scope else "organization"
    await _audit_sensitive_action(
        env, context["actor"], audit_scope + ".agent_prompt",
        audit_scope + "_agent", context["org"] + "/" + context["repo"] +
        "/" + row["session_id"], "success")
    await notify_repo_host(
        env, row["target_node"], context["repo"], "org-agents")
    return json_response({
        "ok": True,
        "status": queue_status,
        "queueState": "ready_for_claim",
        "message": (
            "Follow-up sent. It will run as soon as the node picks it up."
            if owner_scope else
            "Follow-up saved. It will run after the required security "
            "preflight approves it."
        ),
    }, status=202, cache_control="no-store")


def _agent_streams():
    import agent_streams
    agent_streams._bind_runtime(globals())
    return agent_streams


async def org_agent_bot_stream_handler(
        env, request, org, repo, session_id, owner_scope=False):
    await ensure_schema(env)
    if method_name(request) != "GET":
        return _org_agent_error(
            "method_not_allowed",
            "Use GET to read this session's live output.",
            405,
        )
    signed_account = None
    if _owner_agent_signed_triple(request):
        signed_account = await _owner_agent_signed_session(env, request, {})
    context, error = await _org_agent_member_context(
        env, request, org, repo, {}, owner_scope=owner_scope,
        signed_account=signed_account)
    if error:
        return error
    row, record = await _org_agent_session_row(
        env, context, clean_string(session_id, 64))
    if not row:
        return _org_agent_error(
            "not_found",
            "This agent session was not found in the selected repository.",
            404,
        )
    if urlparse(request.url).path.endswith("/live"):
        return await _org_agent_live_stream(env, row, record)
    streams = _agent_streams()
    params = parse_qs(urlparse(request.url).query)
    body = await streams.read(
        streams.WorkerStore(env), str(row["session_id"]), context["actor"],
        params.get("after", ["0"])[0], str(row.get("status") or ""))
    body["status"] = str(row.get("status") or "")
    return json_response(body, cache_control="no-store")


def _org_agent_live_relay_object(env, record):
    owner = clean_string(
        (record or {}).get("createdBy"), MAX_NODE_NAME).strip().lower()
    if not owner:
        return None
    return env.FORKMESH_NODES.get(
        env.FORKMESH_NODES.idFromName(_node_events_do_name(owner)))


async def _org_agent_live_relay_call(env, record, path, payload):
    relay_object = _org_agent_live_relay_object(env, record)
    if relay_object is None:
        return None
    return await relay_object.fetch(JsRequest.new(
        "https://forkmesh.internal" + path,
        to_js({"method": "POST", "body": json.dumps(payload)})))


async def _publish_org_agent_live(env, record, session_id, payload):
    relay = _agent_live_relay
    try:
        response = await _org_agent_live_relay_call(
            env, record, relay.PUBLISH_PATH,
            dict(payload, session=str(session_id)))
        if response is None:
            return 0
        data = relay.parse_body(await response.text())
        return max(0, int(data.get("watchers") or 0))
    except Exception:
        return 0


async def _org_agent_live_stream(env, row, record):
    relay = _agent_live_relay
    status = str(row.get("status") or "")
    if status not in relay.LIVE_STATUSES:
        return Response(ReadableStream(relay.ended_stream(status)),
                        headers=dict(relay.SSE_HEADERS))
    live = _org_agent_live_projection(record or {}, status) or {}
    try:
        response = await _org_agent_live_relay_call(
            env, record, relay.WATCH_PATH, {
                "session": str(row["session_id"]),
                "at": int(live.get("at") or 0),
                "text": live.get("text") or "",
            })
    except Exception:
        response = None
    if response is None or int(response.status) != 200:
        return _org_agent_error(
            "live_unavailable",
            "Live output is unavailable right now; the session still "
            "updates on refresh.",
            503,
            retryable=True,
        )
    return response


async def repo_org_agent_job_stream_handler(
        env, request, owner, repo, job_id):
    await ensure_schema(env)
    if method_name(request) != "POST":
        return _org_agent_error(
            "method_not_allowed",
            "Use POST to relay a leased job's live output.",
            405,
        )
    if not await _authorized_agent_claimant_key(env, request, owner, repo):
        return _org_agent_error(
            "unauthorized",
            (
                "This node could not prove current ownership by its account. "
                "Re-link it before relaying output."
            ),
            401,
            required_action="relink_desktop",
        )
    try:
        data = await bounded_json_request(request)
    except Exception:
        data = None
    if not isinstance(data, dict):
        return _org_agent_error(
            "invalid_json",
            "The live output body is not valid JSON.",
            400,
        )
    owner = clean_string(owner, MAX_NODE_NAME).strip().lower()
    repo = clean_string(repo, MAX_REPO_SEGMENT).strip().lower()
    row = await d1_first(
        env,
        "SELECT id,session_id FROM org_agent_jobs WHERE id=? AND "
        "target_node=? AND repo=? AND status IN ('leased','running') "
        "AND lease_id=?",
        int(job_id), owner, repo, clean_string(data.get("leaseId"), 64),
    )
    if not row or not clean_string(data.get("leaseId"), 64):
        return _org_agent_error(
            "lease_not_found",
            "This job lease is no longer current.",
            409,
            target_node=owner,
        )
    streams = _agent_streams()
    status, body = await streams.accept(
        streams.WorkerStore(env), str(row["session_id"]), int(job_id), data)
    return json_response(body, status=status, cache_control="no-store")


async def repo_org_agent_job_result_handler(
        env, request, owner, repo, job_id):
    await ensure_schema(env)
    if method_name(request) != "POST":
        return _org_agent_error(
            "method_not_allowed",
            "Use POST to report an organization-agent job result.",
            405,
        )
    if not await _authorized_agent_claimant_key(env, request, owner, repo):
        return _org_agent_error(
            "unauthorized",
            (
                "This node could not prove current ownership by its account. "
                "Re-link it before reporting the result."
            ),
            401,
            required_action="relink_desktop",
        )
    try:
        data = await bounded_json_request(request)
    except Exception:
        return _org_agent_error(
            "invalid_json",
            "The agent result body is not valid JSON.",
            400,
            required_action="retry_result",
        )
    if isinstance(data, dict):
        data.pop("runnerName", None)
    return await _apply_org_agent_job_result(env, owner, repo, job_id, data)


async def _apply_org_agent_job_result(env, owner, repo, job_id, data):
    """Apply one leased job's reported result; the caller has authenticated.

    Shared by the node route above and the runner tier
    (runner_leases.py, POST /api/runners/report), which proves the runner's
    own lease before delegating here with the job's target node as owner.
    """
    if not isinstance(data, dict):
        data = {}
    owner = clean_string(owner, MAX_NODE_NAME).strip().lower()
    repo = clean_string(repo, MAX_REPO_SEGMENT).strip().lower()
    lease_id = clean_string(data.get("leaseId"), 64)
    row = await d1_first(
        env,
        "SELECT id,session_id,org_bi,provider,status,data FROM org_agent_jobs "
        "WHERE id=? AND target_node=? AND repo=? "
        "AND status IN ('leased','running') "
        "AND lease_id=?",
        int(job_id), owner, repo, lease_id,
    )
    if not row:
        return _org_agent_error(
            "lease_not_found",
            (
                "This job lease is no longer current. Refresh the desktop "
                "queue before reporting another result."
            ),
            409,
            retryable=True,
            required_action="refresh_agent_queue",
            target_node=owner,
        )
    job = await decrypt_row(env, row.get("data"))
    session_row = await d1_first(
        env,
        "SELECT session_id,status,created_by_bi,data,updated_at "
        "FROM org_agent_sessions "
        "WHERE session_id=? AND org_bi=? AND target_node=?",
        row["session_id"], row["org_bi"], owner,
    )
    session = (
        await decrypt_row(env, session_row.get("data"))
        if session_row else None)
    if data.get("live") is True and isinstance(session, dict):
        return await _apply_org_agent_live_transcript(
            env, row, session_row, session, data)
    if not isinstance(job, dict) or not isinstance(session, dict):
        return _org_agent_error(
            "invalid_job_state",
            (
                "The leased job is incomplete and was not authorized to run. "
                "Refresh the desktop queue."
            ),
            409,
            required_action="refresh_agent_queue",
            target_node=owner,
        )
    verdict = clean_string(data.get("securityVerdict"), 20).lower()
    run_status = clean_string(data.get("status"), 20).lower()
    if verdict not in ("approved", "rejected"):
        verdict = "rejected"
    if run_status not in ("running", "completed", "failed", "rejected"):
        run_status = "rejected"
    # Exact fail-closed coupling: a run cannot become active unless the node
    # reports that it accepted this exact leased job.
    if verdict != "approved" and run_status != "rejected":
        run_status = "rejected"
    now = int(Date.now())
    session["security"] = {
        "state": "not_required" if verdict == "approved" else verdict,
        "model": "",
        "reason": clean_string(data.get("securityReason"), 240),
        "at": now,
    }
    local_agent_id = int(data.get("localAgentId") or 0)
    if verdict == "approved" and local_agent_id > 0:
        session["localAgentId"] = local_agent_id
    if isinstance(data.get("agentInfo"), dict):
        session["agentInfo"] = _org_agent_info_projection(
            data.get("agentInfo"))
        reported_stats = data["agentInfo"].get("diffStats")
        if isinstance(reported_stats, dict):
            import agent_branches
            stats = agent_branches.diff_stats_projection(
                dict(reported_stats, known=True))
            stats["behind"] = 0
            stats["conflicted"] = False
            session["diffStats"] = stats
    runner_name = clean_string(data.get("runnerName"), MAX_NODE_NAME)
    if runner_name and valid_node_name(runner_name.strip().lower()):
        session["runnerNode"] = runner_name.strip().lower()
    if isinstance(data.get("availability"), dict):
        session["availability"] = _org_agent_availability_projection(
            data.get("availability"))
    session.pop("live", None)
    history = session.get("history")
    if not isinstance(history, list):
        history = []
    # A node that captured the run's conversation (the mirror node's
    # stream-json transcript, the desktop's log tail) sends it as
    # ``transcript``; ``result`` is the agent's closing message. The session
    # page shows the conversation when there is one.
    result_text = clean_string(
        data.get("transcript") or data.get("result"), MAX_AGENT_TRANSCRIPT)
    history.append({
        "role": "assistant" if verdict == "approved" else "security",
        "author": str(row.get("provider") or "agent"),
        # A run that failed before saying anything carries its error in
        # agentInfo.lastError (shown beside the history), not here.
        "text": result_text or (
            "The run stopped before the agent replied."
            if verdict == "approved" and run_status == "failed"
            else "Prompt dispatched."
            if verdict == "approved"
            else "The node refused to run this prompt."
        ),
        "at": now,
        "state": run_status,
    })
    session["history"] = history[-ORG_AGENT_MAX_HISTORY:]
    completed_at = now if run_status in ("completed", "failed", "rejected") else 0
    await d1_run(
        env,
        "UPDATE org_agent_sessions SET status=?,data=?,updated_at=?,"
        "completed_at=? WHERE session_id=? AND org_bi=?",
        run_status, await encrypt_row(env, session), now, completed_at,
        row["session_id"], row["org_bi"],
    )
    if completed_at:
        try:
            streams = _agent_streams()
            await streams.end(
                streams.WorkerStore(env), row["session_id"], now)
        except Exception:
            pass
    if run_status not in _agent_live_relay.LIVE_STATUSES:
        await _publish_org_agent_live(
            env, session, row["session_id"], {"status": run_status})
    # The node just changed what this run is doing — claimed it, started it,
    # or finished it. That is the moment the account's other devices want.
    await _ring_cloud_agent_readers(env, session)
    # Every job that starts a run keeps its lease open for the completion
    # report. A mirror node runs a follow-up prompt as its own run, so a steer
    # or continue job that closed on "running" could never report its result.
    keep_running = (
        str(job.get("kind") or "") in ("start", "continue", "steer")
        and verdict == "approved"
        and run_status == "running"
    )
    await d1_run(
        env,
        "UPDATE org_agent_jobs SET status=?,updated_at=? "
        "WHERE id=? AND lease_id=?",
        "running" if keep_running else "done", now, int(job_id), lease_id,
    )
    task_key = clean_string(session.get("taskKey"), 96)
    private_task_match = re.fullmatch(r"task:([a-f0-9]{32})", task_key)
    if private_task_match and run_status == "completed":
        private_task_id = private_task_match.group(1)
        private_task_row = await d1_first(
            env,
            "SELECT data,department,team,destination,assignee_kind,completed_at "
            "FROM organization_tasks "
            "WHERE org_bi=? AND task_id=? AND deleted_at=0 "
            "AND assignee_kind IN ('agent','bot','claude','codex')",
            row["org_bi"], private_task_id,
        )
        private_task_data = (
            await decrypt_row(env, private_task_row.get("data"))
            if private_task_row else None
        )
        if isinstance(private_task_data, dict):
            routing = private_task_data.get("routing")
            if not isinstance(routing, dict):
                routing = _org_task_routing_snapshot(
                    private_task_row, private_task_data)
            if str(private_task_row.get("destination") or "") != "qa":
                private_task_data["preQaRouting"] = routing
            private_task_data["routing"] = routing
            if not clean_string(
                    private_task_data.get("howToTest"), 720).strip():
                private_task_data["howToTest"] = (
                    "Open the completed feature and follow its normal user "
                    "flow. Confirm the requested behavior works, existing "
                    "behavior did not regress, and no console or network "
                    "error appears."
                )
            completion_note = clean_string(
                private_task_data.get("completionNote"),
                _ORG_TASK_COMPLETION_NOTE_LIMIT,
            )
            if not completion_note and result_text:
                completion_note = result_text[:_ORG_TASK_COMPLETION_NOTE_LIMIT]
            private_task_data["completionNote"] = completion_note
            history = _org_task_completion_history(
                private_task_data.get("completionHistory"))
            if int(private_task_row.get("completed_at") or 0) <= 0:
                history.append({
                    "completedAt": now,
                    "completedBy": "agent",
                    "note": completion_note,
                    "qaStatus": "unknown",
                    "qaReviewer": "",
                    "qaReviewedAt": 0,
                })
            private_task_data["completionHistory"] = history[
                -_ORG_TASK_COMPLETION_HISTORY_LIMIT:]
            private_task_data["qaReviewer"] = ""
            private_task_data["qaFailureReason"] = ""
            private_task_data["qaFailureScreenshot"] = ""
            await d1_run(
                env,
                "UPDATE organization_tasks SET status='idle',completed_at=?,"
                "destination='qa',qa_status='unknown',qa_reviewer_bi='',"
                "qa_reviewed_at=0,qa_requested_at=?,data=?,updated_at=? "
                "WHERE org_bi=? AND task_id=?",
                now, now, await encrypt_row(env, private_task_data), now,
                row["org_bi"], private_task_id,
            )
            if int(private_task_row.get("completed_at") or 0) <= 0:
                try:
                    await notify_organization_task_update(
                        env, row["org_bi"],
                        clean_string(
                            str(row.get("provider") or "agent"), 40
                        ).lower() + "-bot",
                        private_task_id,
                        {
                            "id": private_task_id,
                            "title": private_task_data.get("title"),
                            "createdBy": private_task_data.get("createdBy"),
                            "assignee": "agent",
                            "assigneeKind": "agent",
                            "department": private_task_row.get("department"),
                            "team": private_task_row.get("team"),
                            "status": "done",
                            "repository": private_task_data.get("repository"),
                        },
                        "completed",
                    )
                except Exception:
                    pass
    if (
        str(row.get("provider") or "") == "codex"
        and run_status == "completed"
        and private_task_match is None
        and re.fullmatch(
            r"(?:task:[a-z0-9-]{1,48}|"
            r"issue:[a-z0-9-]{1,40}/[a-z0-9._-]{1,60}#[1-9][0-9]{0,8})",
            task_key,
        )
    ):
        board_kind = "issue" if task_key.startswith("issue:") else "task"
        await d1_run(
            env,
            "INSERT INTO world_build_board_items "
            "(item_key,kind,owner,repo,issue_number,title,priority,"
            "updated_by_bi,updated_at,completed_at,completed_by_bi) "
            "VALUES (?,?, '', '',0,'',64,?,?,?,?) "
            "ON CONFLICT(item_key) DO UPDATE SET completed_at=excluded.completed_at,"
            "completed_by_bi=excluded.completed_by_bi,"
            "updated_by_bi=excluded.updated_by_bi,updated_at=excluded.updated_at",
            task_key, board_kind, session_row.get("created_by_bi") or "",
            now, now, session_row.get("created_by_bi") or "",
        )
        qa_title = clean_string(
            session.get("prompt") or task_key.replace("task:", ""),
            160,
        ).strip()
        await d1_run(
            env,
            "INSERT INTO world_qa_items("
            "item_key,title,how_to_test,source_key,added_at,active) "
            "VALUES(?,?,?,?,?,1) ON CONFLICT(item_key) DO UPDATE SET "
            "title=excluded.title,how_to_test=excluded.how_to_test,"
            "source_key=excluded.source_key,added_at=excluded.added_at,"
            "active=1",
            task_key,
            qa_title or "Completed agent task",
            (
                "Open the completed feature and follow its normal user flow. "
                "Confirm the requested behavior works, existing behavior did "
                "not regress, and no console or network error appears."
            ),
            task_key,
            now,
        )
    await _audit_sensitive_action(
        env, owner, "organization.agent_job_result", "organization_agent",
        str(row["session_id"]), "success",
        {"securityVerdict": verdict, "status": run_status})
    return json_response({"ok": True, "status": run_status},
                         cache_control="no-store")


async def _apply_org_agent_live_transcript(env, row, session_row, session,
                                           data):
    status = str(session_row.get("status") or "")
    if (
        str(row.get("status") or "") != "running"
        or status != "running"
        or clean_string(data.get("status"), 20).lower() != "running"
        or clean_string(data.get("securityVerdict"), 20).lower()
        != "approved"
    ):
        return json_response({"ok": True, "status": status, "live": False},
                             cache_control="no-store")
    text = clean_string(data.get("transcript"), MAX_AGENT_TRANSCRIPT)
    if not text:
        return json_response({"ok": True, "status": status, "live": False},
                             cache_control="no-store")
    now = max(int(Date.now()), int(session_row.get("updated_at") or 0) + 1)
    watchers = await _publish_org_agent_live(
        env, session, row["session_id"], {"at": now, "text": text})
    previous = session.get("live")
    previous_at = int(previous.get("at") or 0) if isinstance(
        previous, dict) else 0
    if watchers and now - previous_at < _agent_live_relay.PERSIST_MS:
        return json_response({"ok": True, "status": status, "live": True,
                              "watched": True}, cache_control="no-store")
    session["live"] = {"text": text, "at": now}
    await d1_run(
        env,
        "UPDATE org_agent_sessions SET data=?,updated_at=? "
        "WHERE session_id=? AND org_bi=? AND status='running' "
        "AND updated_at=?",
        await encrypt_row(env, session), now, row["session_id"],
        row["org_bi"], int(session_row.get("updated_at") or 0),
    )
    await _ring_cloud_agent_readers(env, session, "cloud-agents-live")
    return json_response({"ok": True, "status": status, "live": True,
                          "watched": watchers > 0}, cache_control="no-store")


def _org_agent_live_projection(record, status):
    live = record.get("live")
    if status != "running" or not isinstance(live, dict):
        return None
    text = clean_string(live.get("text"), MAX_AGENT_TRANSCRIPT)
    if not text:
        return None
    return {"text": text, "at": max(0, int(live.get("at") or 0))}


async def repository_security_scan_lease_handler(
        env, request, owner, repo):
    """Owner-authorized, short-lived daily lease for a native mirror scanner."""
    method = method_name(request)
    if method not in ("GET", "POST"):
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            cache_control="no-store",
            extra_headers={"allow": "GET, POST"})
    auth_mode = await _security_scan_owner_authorization(
        env, request, owner)
    if not auth_mode:
        return json_response(
            {"error": "unauthorized"}, status=401,
            cache_control="no-store")
    repo_bi, record = await _security_scan_catalog_record(
        env, owner, repo)
    if record is None:
        return json_response(
            {"error": "not_found"}, status=404, cache_control="no-store")
    catalog_commit = str(record.get("commit") or "").strip().lower()
    if not re.fullmatch(r"[0-9a-f]{40}", catalog_commit):
        return json_response(
            {"error": "commit_pin_required"}, status=409,
            cache_control="no-store")
    now = int(Date.now())
    row = await d1_first(
        env,
        "SELECT status,acquired_at,expires_at,next_attempt_at,"
        "attempt_count,completed_scan_id,updated_at,data "
        "FROM repo_security_scan_leases WHERE repo_bi=?",
        repo_bi,
    )
    saved = {}
    if row:
        try:
            saved = await decrypt_row(env, row.get("data")) or {}
        except Exception:
            saved = {}
    same_commit = hmac.compare_digest(
        str(saved.get("commit") or ""), catalog_commit)
    if method == "GET":
        status = str((row or {}).get("status") or "never")
        due = not (
            row
            and same_commit
            and status == "completed"
            and now - int(row.get("updated_at") or 0)
            < SECURITY_SCAN_DAILY_MS
        )
        return json_response({
            "ok": True,
            "status": status,
            "due": due,
            "commit": catalog_commit,
            "leaseExpiresAt": int((row or {}).get("expires_at") or 0),
            "nextAttemptAt": int((row or {}).get("next_attempt_at") or 0),
            "attemptCount": int((row or {}).get("attempt_count") or 0),
            "completedScanId": str(
                (row or {}).get("completed_scan_id") or ""),
            "artifactPolicy": "encrypted-public-safe-envelope-only",
            "privateDiagnosticsStored": False,
        }, cache_control="no-store")
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response(
            {"error": "invalid_json"}, status=400,
            cache_control="no-store")
    requested_commit = str(
        (data or {}).get("commit") or "").strip().lower()
    if (
        not re.fullmatch(r"[0-9a-f]{40}", requested_commit)
        or not hmac.compare_digest(requested_commit, catalog_commit)
    ):
        return json_response(
            {"error": "commit_mismatch"}, status=409,
            cache_control="no-store")
    if row and same_commit:
        status = str(row.get("status") or "")
        if (
            status == "completed"
            and now - int(row.get("updated_at") or 0)
            < SECURITY_SCAN_DAILY_MS
        ):
            return json_response({
                "ok": True,
                "leased": False,
                "reason": "not_due",
                "nextDueAt":
                    int(row.get("updated_at") or 0) + SECURITY_SCAN_DAILY_MS,
            }, cache_control="no-store")
        if status == "leased" and int(row.get("expires_at") or 0) > now:
            return json_response({
                "error": "lease_held",
                "retryAfterMs": int(row.get("expires_at") or 0) - now,
            }, status=409, cache_control="no-store")
        if int(row.get("next_attempt_at") or 0) > now:
            return json_response({
                "error": "retry_backoff",
                "retryAfterMs": int(row.get("next_attempt_at") or 0) - now,
            }, status=429, cache_control="no-store")
    token = _b64url_encode(_random_bytes(32))
    token_bi = await blind_index(env, "security-scan-lease:" + token)
    attempt = (
        int(row.get("attempt_count") or 0) + 1
        if row and same_commit else 1)
    expires = now + SECURITY_SCAN_LEASE_MS
    encrypted = await encrypt_row(env, {
        "commit": catalog_commit,
        "schema": "native-mirror-security-scan-lease-v1",
    })
    await d1_run(
        env,
        "INSERT INTO repo_security_scan_leases "
        "(repo_bi,lease_token_bi,status,acquired_at,expires_at,"
        "next_attempt_at,attempt_count,completed_scan_id,updated_at,data) "
        "VALUES (?,?, 'leased',?,?,0,?,'',?,?) "
        "ON CONFLICT(repo_bi) DO UPDATE SET "
        "lease_token_bi=excluded.lease_token_bi,status='leased',"
        "acquired_at=excluded.acquired_at,expires_at=excluded.expires_at,"
        "next_attempt_at=0,attempt_count=excluded.attempt_count,"
        "completed_scan_id='',updated_at=excluded.updated_at,data=excluded.data",
        repo_bi, token_bi, now, expires, attempt, now, encrypted,
    )
    await _audit_sensitive_action(
        env, owner, "repository.security_scan_lease",
        "repository", owner + "/" + repo, "success",
        {"attemptCount": attempt, "authMode": auth_mode})
    return json_response({
        "ok": True,
        "leased": True,
        "leaseToken": token,
        "expiresAt": expires,
        "commit": catalog_commit,
        "ingestUrl": (
            _public_base_url(env, request).rstrip("/")
            + "/api/repo/" + quote(owner) + "/" + quote(repo)
            + "/security-scans/ingest"
        ),
        "artifactPolicy": "public-safe-envelope-only",
    }, status=201, cache_control="no-store")


async def _pull_mutation_node(env, context, candidates, operation):
    """Pick the mirror that runs one owner pull mutation, or say why none can.

    Returns ``(endpoint, "")`` or ``(None, reason)`` with a reason from
    ``https_routing.merge_unavailable_reason``.
    """
    # Signed refs digests this request saw ahead of the pin set, by node.
    lagging_refs_quorum = {}
    rejected = set()
    for candidate in candidates:
        proof_details = {}
        # A fresh proof rather than a read's held failure: owner merges are
        # rare, and one live /health per candidate is cheap next to refusing.
        if await _https_mirror_repository_proof(
                env, candidate, context, operation,
                retry_failed=True, details=proof_details):
            return candidate, ""
        # The pin set trails every push until the health cron re-proves two
        # endpoints. Two mirrors signing the same digest here is the evidence
        # that cron would pin; the job still names exact commits.
        if https_routing.mismatch_is_servable(
                operation, candidate.get("node"), proof_details,
                lagging_refs_quorum):
            return candidate, ""
        rejected.add(proof_details.get("reason"))
    return None, https_routing.merge_unavailable_reason(
        len(candidates), rejected)


async def repository_pull_merge_handler(
        env, request, owner, repo, pull_number, mutation="merge"):
    """Authorize, pin, dispatch, and durably reconcile one pull mutation job.

    ``mutation`` selects between merging the pull request into the base branch
    and merging the base branch into the pull request's own head branch. Both
    are exact-OID, idempotent-by-request-id, owner-only, and audited; they share
    this handler so neither can drift away from those four properties.
    """
    profile = HTTPS_MIRROR_PULL_MUTATIONS[mutation]
    if method_name(request) != "POST":
        return json_response({"error": "method_not_allowed"}, status=405)
    try:
        context = await _https_mirror_public_context(env, owner, repo)
    except _HttpsMirrorContextUnavailable:
        # A snapshot fault is a degraded relay, not a missing pull request.
        # Answering not_found here would tell an owner mid-merge that their
        # repository is gone.
        return json_response(
            {"error": "mirror_unavailable"}, status=503,
            cache_control="no-store",
            extra_headers=EXPECTED_DEGRADED_HEADERS)
    if context is None:
        return json_response(
            {"error": "not_found"}, status=404, cache_control="no-store")
    try:
        announced = int(request.headers.get("content-length") or 0)
    except Exception:
        announced = 0
    if announced > HTTPS_MIRROR_MERGE_BODY_MAX_BYTES:
        return json_response({"error": "request_too_large"}, status=413)
    try:
        raw = str(await request.text())
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    data = _https_mirror_merge_body(raw, pull_number, mutation)
    if data is None:
        return json_response({"error": "invalid_request"}, status=400)
    actor_bi, actor_record = await _account_session_record(env, request, data)
    actor = clean_string(
        (actor_record or {}).get("name", ""), MAX_NODE_NAME).strip().lower()
    if not actor:
        return json_response({"error": "invalid_session"}, status=401)
    try:
        authorized = await _organization_owner_can_merge_repo(
            env, request, context["owner"], context["repo"], actor)
    except Exception:
        authorized = False
    target = context["owner"] + "/" + context["repo"]
    # An owner may merge without the independent-review gate. Only the owner
    # check above stands between a caller and that waiver, so every audit line
    # for this request says whether the waiver was asked for.
    override_review = bool(data.get("overrideReview"))
    if not authorized:
        await _audit_sensitive_action(
            env, actor, profile["action"], "repository", target,
            "denied", {"pullNumber": int(pull_number),
                       "overrideReview": override_review})
        return json_response({"error": "forbidden"}, status=403)

    node_request = {
        key: value for key, value in data.items()
        if key not in ("sessionToken", "deployApproval")
    }
    approved = None
    deploy_approvals = globals().get("_deploy_approvals")
    if data.get("deployApproval") is not None and deploy_approvals is not None:
        try:
            approved = await deploy_approvals.verify(
                env, actor, data["deployApproval"], "pull",
                str(owner or "").strip().lower(), context["repo"],
                str(int(pull_number)),
                node_request["expectedBaseOid"],
                node_request["expectedHeadOid"], node_request["requestId"],
                int(Date.now()))
        except Exception:
            approved = None
    digest_value = {
        **node_request,
        "actor": actor,
        "owner": context["owner"],
        "repository": context["repo"],
    }
    request_digest = hashlib.sha256(json.dumps(
        digest_value, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")).hexdigest()
    await ensure_schema(env)
    now = int(Date.now())
    # Delete this exact expired id first; otherwise its primary key could make
    # INSERT OR IGNORE look successful while no live job can be read back.
    await d1_run(
        env,
        "DELETE FROM repo_merge_jobs WHERE request_id=? AND expires_at<=?",
        node_request["requestId"], now,
    )
    row = await d1_first(
        env,
        """SELECT request_digest,repo_bi,actor_bi,pull_number,selected_node,
                  status,result,expires_at
             FROM repo_merge_jobs WHERE request_id=? AND expires_at>?""",
        node_request["requestId"], now,
    )
    job_created = False
    if row is None and mutation == "merge":
        blocked = await _pull_rules.merge_gate(
            env, context, int(pull_number),
            node_request.get("expectedBaseOid"),
            node_request.get("expectedHeadOid"))
        if blocked is not None:
            await _audit_sensitive_action(
                env, actor, profile["action"], "repository", target,
                "denied", {"pullNumber": int(pull_number),
                           "reason": "rules_required"})
            return json_response(
                blocked, status=409, cache_control="no-store")
        protection = await _branch_protection.merge_refusal(
            env, context["owner"], context["repo"], int(pull_number),
            override_review)
        if protection is not None:
            await _audit_sensitive_action(
                env, actor, profile["action"], "repository", target,
                "denied", {"pullNumber": int(pull_number),
                           "reason": protection[1].get("error", "")})
            return json_response(
                protection[1], status=protection[0], cache_control="no-store")
    if row is None:
        if not await _repo_merge_jobs_make_room(
                env, context["repoBi"], now):
            return json_response(
                {"error": "merge_queue_full"}, status=429,
                cache_control="no-store")
        candidates = await _https_mirror_candidates(
            env, context, world_request_country(request),
            operation=profile["operation"])
        selected, unavailable = await _pull_mutation_node(
            env, context, candidates, profile["operation"])
        if selected is None:
            # Every reason but no_merge_executor is refs-pin lag the browser
            # waits out by retrying; only the one waiting cannot fix is an
            # error worth a log row.
            return json_response(
                {"error": "merge_node_unavailable", "reason": unavailable},
                status=503, cache_control="no-store",
                extra_headers=(
                    None if unavailable == "no_merge_executor"
                    else EXPECTED_DEGRADED_HEADERS))
        expires_at = now + HTTPS_MIRROR_MERGE_JOB_RETENTION_MS
        try:
            await d1_run(
                env,
                """INSERT OR IGNORE INTO repo_merge_jobs
                     (request_id,request_digest,repo_bi,actor_bi,pull_number,
                      selected_node,status,result,created_at,updated_at,expires_at)
                     VALUES (?,?,?,?,?,?,'requested','',?,?,?)""",
                node_request["requestId"], request_digest, context["repoBi"],
                actor_bi, int(pull_number), selected["node"], now, now,
                expires_at,
            )
            job_created = True
        except Exception:
            # The schema triggers are the final concurrency-safe hard bounds.
            # Re-read first because an identical request may have won the race;
            # only a genuinely absent row is a full queue.
            job_created = False
        row = await d1_first(
            env,
            """SELECT request_digest,repo_bi,actor_bi,pull_number,selected_node,
                      status,result,expires_at
                 FROM repo_merge_jobs WHERE request_id=? AND expires_at>?""",
            node_request["requestId"], now,
        )
        if row is None:
            return json_response(
                {"error": "merge_queue_full"}, status=429,
                cache_control="no-store")
    if (
        not row
        or not hmac.compare_digest(
            str(row.get("request_digest") or ""), request_digest)
        or not hmac.compare_digest(
            str(row.get("repo_bi") or ""), context["repoBi"])
        or not hmac.compare_digest(
            str(row.get("actor_bi") or ""), actor_bi)
        or int(row.get("pull_number") or 0) != int(pull_number)
    ):
        return json_response(
            {"error": "request_id_reused"}, status=409,
            cache_control="no-store")
    if row.get("status") in ("succeeded", "failed"):
        try:
            result = json.loads(str(row.get("result") or ""))
        except Exception:
            result = None
        checked = _https_mirror_merge_result(
            result, node_request, mutation)
        if checked is None or checked.get("status") == "processing":
            return json_response(
                {"error": "merge_state_unavailable"}, status=503,
                cache_control="no-store")
        return _https_mirror_merge_response(checked, mutation)

    selected_node = clean_string(
        row.get("selected_node", ""), MAX_NODE_NAME).strip().lower()
    candidates = await _https_mirror_candidates(
        env, context, world_request_country(request), sticky=selected_node,
        operation=profile["operation"])
    endpoint = next(
        (candidate for candidate in candidates
         if candidate["node"] == selected_node),
        None,
    )
    if endpoint is None:
        # The node this job was dispatched to has dropped out of the healthy
        # set for now. Only it may answer for the job, so the browser waits.
        return json_response(
            {"error": "merge_node_unavailable", "reason": "job_node_offline"},
            status=503, cache_control="no-store",
            extra_headers=EXPECTED_DEGRADED_HEADERS)
    upstream_status, result = await _https_mirror_merge_proxy(
        env, endpoint, context, node_request, mutation)
    if result is None:
        # An uncertain transport result never fails over: the selected node may
        # already have committed the exact idempotent request.
        return json_response(
            {"error": "merge_status_pending"}, status=503,
            cache_control="no-store",
            extra_headers=EXPECTED_DEGRADED_HEADERS)
    if upstream_status == 202:
        if job_created:
            await _audit_sensitive_action(
                env, actor, profile["action"], "repository", target,
                "requested", {"pullNumber": int(pull_number),
                              "overrideReview": override_review})
        return _https_mirror_merge_response(result, mutation)
    terminal = (
        "succeeded" if result["status"] == profile["terminal"] else "failed")
    stored = dict(result)
    admitted_state = ""
    if terminal == "succeeded":
        # The owner's permission makes the mirror's post-mutation refs the
        # repository's accepted — and, while the source stays silent,
        # preferred — generation. Only a node-signature-verified attestation
        # is ever persisted with the job; an unverifiable one is dropped so
        # the routing override can never be driven by unsigned data.
        admitted_state = await _admit_owner_merge_state(
            env, context, endpoint, result)
        for field in ("state", "stateTs", "stateSig"):
            stored.pop(field, None)
        if admitted_state:
            stored["state"] = admitted_state
            source_state = str(context.get("sourceState") or "")
            if re.fullmatch(r"[0-9a-f]{64}", source_state):
                stored["sourceState"] = source_state
    encoded = json.dumps(
        stored, sort_keys=True, separators=(",", ":"))
    if len(encoded.encode("utf-8")) > HTTPS_MIRROR_MERGE_RESULT_MAX_BYTES:
        return json_response(
            {"error": "merge_state_unavailable"}, status=503,
            cache_control="no-store")
    await d1_run(
        env,
        """UPDATE repo_merge_jobs SET status=?,result=?,updated_at=?,expires_at=?
             WHERE request_id=? AND status='requested'""",
        terminal, encoded, int(Date.now()),
        int(Date.now()) + HTTPS_MIRROR_MERGE_JOB_RETENTION_MS,
        node_request["requestId"],
    )
    await _audit_sensitive_action(
        env, actor, profile["action"], "repository", target,
        "success" if terminal == "succeeded" else "failed",
        {"pullNumber": int(pull_number),
         "reason": str(result.get("error") or ""),
         "overrideReview": override_review,
         "statePinned": bool(admitted_state)})
    if terminal == "succeeded":
        try:
            await record_hosted_push_state(
                env, context["owner"], context["repo"], endpoint,
                pusher=actor)
        except Exception:
            pass
        queue_actions = globals().get("queue_runner_actions")
        if queue_actions is not None:
            await queue_actions(
                env, context["owner"], context["repo"],
                str(result.get("baseAfter") or ""),
                (endpoint or {}).get("node", ""), actor)
        if approved is not None:
            try:
                await deploy_approvals.record(
                    env, approved, result, int(Date.now()))
            except Exception:
                pass
        # Point the rest of the fleet at the generation the owner just made
        # current: the same payload-free "commits" event a source publish
        # sends, carrying the attested digest so every mirror keeps fetching
        # through the relay route until that exact state lands — whether or
        # not the source-of-truth desktop is online to republish.
        generation = int(Date.now()) if admitted_state else 0
        scheduled = await schedule_repo_mirror_wake(
            env, context["owner"], context["repo"], "commits",
            admitted_state, generation)
        if not scheduled:
            await notify_repo_mirrors(
                env, context["owner"], context["repo"], "commits",
                admitted_state, generation)
        # Wake the source node immediately. Its next signed /api/sync response
        # now includes the exact published receipt from repo_merge_jobs, and
        # its mesh converge fast-forwards its checkout onto the merge.
        await notify_repo_host(
            env, context["owner"], context["repo"], "mirror_merge")
        # Outbound webhooks (adhoc #207): only the merge mutation announces a
        # merged pull request, and only once the node reported the result
        # published — a queued build must never point at refs no mirror serves.
        if mutation == "merge":
            badge_fact = globals().get("_record_web_merge_badge_fact")
            if badge_fact is not None:
                try:
                    await badge_fact(env, context["repoBi"], int(pull_number),
                                     actor_bi, override_review)
                except Exception:
                    pass
            await queue_repo_webhook_event(
                env, context["owner"], context["repo"], "pull.merged",
                {
                    "pullNumber": int(pull_number),
                    "mergedBy": actor,
                    "base": str(result.get("baseAfter") or ""),
                    "head": str(result.get("head") or ""),
                    "baseBefore": str(result.get("baseBefore") or ""),
                    "overrideReview": bool(override_review),
                    "refsDigest": admitted_state,
                },
                str(result.get("baseAfter") or "") or admitted_state)
            # Activity ping (adhoc #295) on the same published merge. The
            # merger is known here, so they are named as the actor and
            # therefore do not ping themselves.
            try:
                await notify_repo_activity(
                    env, context["owner"], context["repo"], "pull.merged",
                    body=("Pull request #" + str(int(pull_number)) +
                          " was merged."),
                    actor=actor,
                    key=(str(result.get("baseAfter") or "")
                         or admitted_state),
                    meta={"number": int(pull_number)})
            except Exception:
                pass
    return _https_mirror_merge_response(result, mutation)


# --- Branch management requested from the website (adhoc #2098) -------------
# The desktop app manages branches with local Git. The website has no Git and
# the relay must never grow one, so a branch delete/update/create typed on the
# Branches page is validated and queued here for the repository's own node,
# which owns the checkout every mirror is published from. The node drains the
# queue from its next /api/sync, applies what it can, and propagates the
# result; the page reads the pending queue back so a request stays visible
# until the node has taken it.
BRANCH_OPS = {
    "delete": {
        "action": "repository.branch_delete",
        # The tip the requester saw. The node refuses the delete when the
        # branch has moved since, so nobody deletes commits they never saw.
        "oids": ("expectedOid",),
    },
    "update": {
        "action": "repository.branch_update",
        "oids": ("expectedOid",),
    },
    "create": {
        "action": "repository.branch_create",
        # The commit the new branch starts at.
        "oids": ("startOid",),
    },
    "merge": {
        "action": "repository.branch_merge",
        "oids": ("expectedOid", "baseOid"),
    },
}
MAX_PENDING_BRANCH_OPS = 1024
MAX_BRANCH_OP_RESULTS = 20
BRANCH_OP_RESULT_STATUSES = ("applied", "already", "refused")
# Patterns, not compiled objects: this module's ``re`` is bound by
# _bind_runtime after import, so module-level compilation would run too early.
BRANCH_OP_NAME_PATTERN = r"[A-Za-z0-9][A-Za-z0-9._/-]{0,199}"
BRANCH_OP_OID_PATTERN = r"(?:[0-9a-f]{40}|[0-9a-f]{64})"


def branch_op_name(value):
    """Return a Git-legal, non-internal branch name, or "" when unusable.

    Same refusals as the mirror node's own branch validation, so a name this
    relay accepts cannot be one the node will later reject; plus ForkMesh's
    internal collaboration refs, which are not user branches at all.
    """
    branch = clean_string(value, 200).strip()
    if (
        not re.fullmatch(BRANCH_OP_NAME_PATTERN, branch)
        or branch.startswith((".", "-", "/"))
        or branch.endswith((".", "/", ".lock"))
        or ".." in branch
        or "//" in branch
        or "@{" in branch
        or "\\" in branch
        or branch == "forkmesh/pulls"
        or branch.startswith("forkmesh/pull/")
    ):
        return ""
    return branch


def _branch_op_projection(row_id, record):
    """The bounded public shape of one queued operation."""
    return {
        "id": int(row_id or 0),
        "op": str(record.get("op") or ""),
        "branch": str(record.get("branch") or ""),
        "base": str(record.get("base") or ""),
        "expectedOid": str(record.get("expectedOid") or ""),
        "startOid": str(record.get("startOid") or ""),
        "baseOid": str(record.get("baseOid") or ""),
        "deleteBranch": record.get("deleteBranch") is True,
        "requestedBy": str(record.get("requestedBy") or ""),
        "queuedAt": int(record.get("queuedAt") or 0),
    }


def _branch_op_result_projection(row_id, record):
    return {
        "id": int(row_id or 0),
        "requestId": int(record.get("requestId") or 0),
        "op": str(record.get("op") or ""),
        "branch": str(record.get("branch") or ""),
        "base": str(record.get("base") or ""),
        "status": str(record.get("status") or ""),
        "message": str(record.get("message") or ""),
        "mergeOid": str(record.get("mergeOid") or ""),
        "branchDeleted": record.get("branchDeleted") is True,
        "reportedAt": int(record.get("reportedAt") or 0),
    }


async def _branch_op_results_recent(env, repo_bi):
    rows = await d1_all(
        env,
        "SELECT id, data FROM branch_op_results WHERE repo_bi=? "
        "ORDER BY id DESC LIMIT ?",
        repo_bi, MAX_BRANCH_OP_RESULTS,
    )
    results = []
    for row in rows or []:
        record = await decrypt_row(env, row.get("data", ""))
        if isinstance(record, dict):
            results.append(_branch_op_result_projection(row.get("id"), record))
    return results


def _branch_op_result_record(item, reported_at):
    if not isinstance(item, dict):
        return None
    operation = clean_string(item.get("op", ""), 20).strip().lower()
    if operation not in BRANCH_OPS:
        return None
    branch = branch_op_name(item.get("branch", ""))
    if not branch:
        return None
    status = clean_string(item.get("status", ""), 20).strip().lower()
    if status not in BRANCH_OP_RESULT_STATUSES:
        return None
    try:
        request_id = int(item.get("requestId") or 0)
    except (TypeError, ValueError, OverflowError):
        request_id = 0
    merge_oid = clean_string(item.get("mergeOid", ""), 64).strip().lower()
    if not re.fullmatch(BRANCH_OP_OID_PATTERN, merge_oid):
        merge_oid = ""
    return {
        "requestId": max(0, request_id),
        "op": operation,
        "branch": branch,
        "base": branch_op_name(item.get("base", "")),
        "status": status,
        "message": clean_string(item.get("message", ""), 300).strip(),
        "mergeOid": merge_oid,
        "branchDeleted": item.get("branchDeleted") is True,
        "reportedAt": reported_at,
    }


async def repository_branch_op_results_handler(env, request, owner, repo):
    await ensure_schema(env)
    method = method_name(request)
    if method not in ("POST", "DELETE"):
        return json_response({"error": "method_not_allowed"}, status=405)
    repo_bi = await blind_index(env, owner + "/" + repo)
    if method == "DELETE":
        authorized, denial = await _authorize_issue_manager(
            env, owner, {}, request)
        if not authorized:
            return denial
        await d1_run(
            env, "DELETE FROM branch_op_results WHERE repo_bi=?", repo_bi)
        return json_response(
            {"ok": True, "results": []}, cache_control="no-store")
    if not await _authorized_repo_inbox_signing_key(env, request, owner, repo):
        return json_response({"error": "unauthorized"}, status=401)
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    items = data.get("results") if isinstance(data, dict) else None
    if not isinstance(items, list):
        return json_response({"error": "results_required"}, status=400)
    reported_at = int(Date.now())
    recorded = 0
    for item in items[:MAX_PENDING_BRANCH_OPS]:
        record = _branch_op_result_record(item, reported_at)
        if record is None:
            continue
        await d1_run(
            env,
            "INSERT INTO branch_op_results (repo_bi, data, reported_at) "
            "VALUES (?,?,?)",
            repo_bi, await encrypt_row(env, record), reported_at,
        )
        recorded += 1
    if recorded:
        await d1_run(
            env,
            "DELETE FROM branch_op_results WHERE repo_bi=? AND id NOT IN "
            "(SELECT id FROM branch_op_results WHERE repo_bi=? "
            "ORDER BY id DESC LIMIT ?)",
            repo_bi, repo_bi, MAX_BRANCH_OP_RESULTS,
        )
    return json_response({"ok": True, "recorded": recorded})


async def _branch_ops_pending(env, repo_bi):
    rows = await d1_all(
        env,
        "SELECT id, data FROM branch_op_inbox WHERE repo_bi=? ORDER BY id ASC",
        repo_bi,
    )
    pending = []
    for row in rows or []:
        record = await decrypt_row(env, row.get("data", ""))
        if isinstance(record, dict):
            pending.append(_branch_op_projection(row.get("id"), record))
    return pending


def _branch_op_record(data, default_branch, actor):
    operation = clean_string(data.get("op", ""), 20).strip().lower()
    profile = BRANCH_OPS.get(operation)
    if profile is None:
        return None, json_response(
            {"error": "unsupported_operation"}, status=400)
    branch = branch_op_name(data.get("branch", ""))
    if not branch:
        return None, json_response({"error": "invalid_branch"}, status=400)
    if default_branch and branch == default_branch:
        return None, json_response({"error": "default_branch"}, status=409)
    record = {
        "op": operation,
        "branch": branch,
        "requestedBy": actor,
        "queuedAt": int(Date.now()),
        "source": "web",
    }
    for field in profile["oids"]:
        oid = clean_string(data.get(field, ""), 64).strip().lower()
        if not re.fullmatch(BRANCH_OP_OID_PATTERN, oid):
            return None, json_response({"error": "invalid_commit"}, status=400)
        record[field] = oid
    if operation == "update":
        base = branch_op_name(data.get("base", ""))
        if base == branch:
            return None, json_response({"error": "invalid_branch"}, status=400)
        record["base"] = base
    if operation == "merge":
        base = branch_op_name(data.get("base", "")) or default_branch
        if not base or base == branch:
            return None, json_response({"error": "invalid_branch"}, status=400)
        record["base"] = base
        record["deleteBranch"] = data.get("deleteBranch") is True
    return record, None


async def _branch_ops_node_drain(env, request, owner, repo):
    if not await _authorized_repo_inbox_signing_key(env, request, owner, repo):
        return json_response({"error": "unauthorized"}, status=401)
    repo_bi = await blind_index(env, owner + "/" + repo)
    ops = await _branch_ops_pending(env, repo_bi)
    ids = [op["id"] for op in ops]
    if ids:
        await d1_run(
            env,
            "DELETE FROM branch_op_inbox WHERE repo_bi=? AND id IN (%s)"
            % ",".join("?" for _ in ids),
            repo_bi, *ids,
        )
    return json_response({"ok": True, "ops": ops}, cache_control="no-store")


async def _branch_ops_wake(env, request, owner, repo):
    await notify_repo_host(env, owner, repo, "branches")
    try:
        match = REPO_API_PREFIX_RE.match(urlparse(str(request.url)).path)
    except Exception:
        return
    alias = (safe_segment(match.group(1)) or "").lower() if match else ""
    owner_l = str(owner or "").strip().lower()
    if not alias or alias == owner_l:
        return
    try:
        org_bi, org_row = await _org_row(env, alias)
        if not org_row:
            return
        members = await d1_all(
            env,
            "SELECT name FROM org_members "
            "WHERE org_bi=? AND role IN ('owner','admin') "
            "ORDER BY name LIMIT ?",
            org_bi, MAX_ORG_MEMBERS,
        )
    except Exception:
        return
    for member in members or []:
        account = clean_string(
            member.get("name") or "", MAX_NODE_NAME).strip().lower()
        if valid_node_name(account) and account != owner_l:
            await notify_repo_host(
                env, account, repo, "branches", event_owner=alias)


async def _branch_op_on_cores(env, request, owner, repo, repo_bi, target,
                              record, data, default_branch, actor):
    import agent_branches
    agent_branches._bind_runtime(globals())
    answer = await agent_branches.core_branch_op(
        env, request, owner, repo, record, data, default_branch, actor)
    if answer is None:
        return None
    status, payload = answer
    if status == 202:
        return json_response(payload, status=202, cache_control="no-store")
    outcome = payload.get("status")
    reported_at = int(Date.now())
    result = _branch_op_result_record({
        "op": record["op"], "branch": record["branch"],
        "base": payload.get("base", ""),
        "status": ("applied" if outcome in ("merged", "updated", "deleted")
                   else "already" if outcome == "already" else "refused"),
        "message": payload.get("message", ""),
        "mergeOid": payload.get("mergeOid", ""),
        "branchDeleted": payload.get("branchDeleted") is True,
    }, reported_at)
    if result is not None and payload.get("error") not in (
            "merge_status_pending", "merge_node_unavailable"):
        await d1_run(
            env,
            "INSERT INTO branch_op_results (repo_bi, data, reported_at) "
            "VALUES (?,?,?)",
            repo_bi, await encrypt_row(env, result), reported_at,
        )
        await d1_run(
            env,
            "DELETE FROM branch_op_results WHERE repo_bi=? AND id NOT IN "
            "(SELECT id FROM branch_op_results WHERE repo_bi=? "
            "ORDER BY id DESC LIMIT ?)",
            repo_bi, repo_bi, MAX_BRANCH_OP_RESULTS,
        )
    if payload.get("branchDeleted") is True:
        follow_up = {
            "op": "delete", "branch": record["branch"],
            "expectedOid": record["expectedOid"], "requestedBy": actor,
            "queuedAt": reported_at, "source": "cores",
        }
        try:
            await d1_run(
                env,
                "INSERT INTO branch_op_inbox (repo_bi, data, queued_at) "
                "VALUES (?,?,?)",
                repo_bi, await encrypt_row(env, follow_up), reported_at,
            )
            await _branch_ops_wake(env, request, owner, repo)
        except Exception:
            pass
    if outcome in ("merged", "updated", "deleted", "already") or (
            payload.get("error") in ("merge_conflict", "stale_base",
                                     "stale_head", "unsupported_branch")):
        await _audit_sensitive_action(
            env, actor, BRANCH_OPS[record["op"]]["action"], "repository",
            target, "success" if payload.get("ok") else "failed",
            {"branch": record["branch"], "op": record["op"],
             "base": payload.get("base", ""), "cores": True,
             "outcome": outcome, "error": payload.get("error", "")})
    payload["results"] = await _branch_op_results_recent(env, repo_bi)
    return json_response(
        payload, status=status, cache_control="no-store",
        extra_headers=(EXPECTED_DEGRADED_HEADERS
                       if status == 503 and payload.get("retryable") is True
                       else None))


async def repository_branch_ops_handler(env, request, owner, repo):
    """List, queue, or cancel branch operations for the repository's node."""
    await ensure_schema(env)
    method = method_name(request)
    if method not in ("GET", "POST", "DELETE"):
        return json_response({"error": "method_not_allowed"}, status=405)
    if method == "GET" and parse_qs(
            urlparse(str(request.url)).query).get("sig"):
        return await _branch_ops_node_drain(env, request, owner, repo)
    try:
        data = (
            await bounded_json_request(request) if method == "POST" else {})
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    if not isinstance(data, dict):
        return json_response({"error": "invalid_json"}, status=400)
    # Managing branches is the repository owner's (or an organization
    # owner/admin's) action — the same gate the issue and agent write paths
    # use, resolved from the caller's account session rather than a name in
    # the request.
    authorized, denial = await _authorize_issue_manager(
        env, owner, data, request)
    if not authorized:
        return denial
    _, actor_record = await _account_session_record(env, request, data)
    actor = clean_string(
        (actor_record or {}).get("name", ""), MAX_NODE_NAME).strip().lower()
    repo_bi = await blind_index(env, owner + "/" + repo)
    target = owner + "/" + repo
    if method == "GET":
        return json_response({
            "ok": True,
            "ops": await _branch_ops_pending(env, repo_bi),
            "results": await _branch_op_results_recent(env, repo_bi),
            "limit": MAX_PENDING_BRANCH_OPS,
        }, cache_control="no-store")
    if method == "DELETE":
        try:
            requested = int(
                parse_qs(urlparse(str(request.url)).query).get("id", [""])[0])
        except (TypeError, ValueError, OverflowError):
            requested = 0
        if requested <= 0:
            return json_response({"error": "invalid_request"}, status=400)
        await d1_run(
            env,
            "DELETE FROM branch_op_inbox WHERE id=? AND repo_bi=?",
            requested, repo_bi,
        )
        return json_response({
            "ok": True,
            "ops": await _branch_ops_pending(env, repo_bi),
            "limit": MAX_PENDING_BRANCH_OPS,
        }, cache_control="no-store")

    requests = data.get("ops") if "ops" in data else [data]
    if (not isinstance(requests, list) or not requests
            or len(requests) > MAX_PENDING_BRANCH_OPS):
        return json_response({"error": "invalid_request"}, status=400)
    default_branch = branch_op_name(data.get("defaultBranch", ""))
    touched = []
    for item in requests:
        if isinstance(item, dict):
            touched_record, _ = _branch_op_record(item, default_branch, actor)
            if touched_record:
                touched.append(touched_record["branch"])
                if touched_record["op"] == "merge":
                    touched.append(touched_record.get("base", ""))
    protection = await _branch_protection.branch_refusal(
        env, owner, repo, touched)
    if protection is not None:
        await _audit_sensitive_action(
            env, actor, "repository.branch_ops", "repository", target,
            "denied", {"reason": "branch_protected"})
        return protection
    if "ops" not in data and data.get("queue") is not True:
        record, error = _branch_op_record(data, default_branch, actor)
        if error is not None:
            return error
        executed = await _branch_op_on_cores(
            env, request, owner, repo, repo_bi, target, record, data,
            default_branch, actor)
        if executed is not None:
            return executed
    records = []
    for item in requests:
        if not isinstance(item, dict):
            return json_response({"error": "invalid_request"}, status=400)
        record, error = _branch_op_record(item, default_branch, actor)
        if error is not None:
            return error
        records.append(record)
    pending = await _branch_ops_pending(env, repo_bi)
    waiting = {(queued["op"], queued["branch"]) for queued in pending}
    fresh = []
    for record in records:
        key = (record["op"], record["branch"])
        if key not in waiting:
            waiting.add(key)
            fresh.append(record)
    if not fresh:
        return json_response({
            "ok": True,
            "queued": False,
            "queuedCount": 0,
            "ops": pending,
            "limit": MAX_PENDING_BRANCH_OPS,
        }, cache_control="no-store")
    room = MAX_PENDING_BRANCH_OPS - len(pending)
    if len(fresh) > room:
        return json_response(
            {"error": "queue_full", "room": max(room, 0),
             "limit": MAX_PENDING_BRANCH_OPS}, status=429)
    queued = 0
    for record in fresh:
        try:
            await d1_run(
                env,
                "INSERT INTO branch_op_inbox (repo_bi, data, queued_at) "
                "VALUES (?,?,?)",
                repo_bi, await encrypt_row(env, record), record["queuedAt"],
            )
        except Exception:
            break
        queued += 1
        await _audit_sensitive_action(
            env, actor, BRANCH_OPS[record["op"]]["action"], "repository",
            target, "requested",
            {"branch": record["branch"], "op": record["op"],
             "base": record.get("base", "")})
    if queued:
        await _branch_ops_wake(env, request, owner, repo)
    if queued < len(fresh):
        return json_response(
            {"error": "queue_full", "queuedCount": queued,
             "limit": MAX_PENDING_BRANCH_OPS}, status=429)
    return json_response({
        "ok": True,
        "queued": True,
        "queuedCount": queued,
        "ops": await _branch_ops_pending(env, repo_bi),
        "limit": MAX_PENDING_BRANCH_OPS,
    }, cache_control="no-store")


PROFILE_CONTRIBUTION_VIEW_FRESH_MS = 60 * 1000
PROFILE_CONTRIBUTION_VIEW_STALE_SECONDS = 10 * 60
PROFILE_CONTRIBUTION_VIEW_BUILT_HEADER = "x-forkmesh-built"


def _contribution_view_key(profile_bi, from_value, to_value):
    return _contribution_cache_key(
        profile_bi, from_value, to_value,
        "view-%d" % _D1_WATCH_GENERATIONS["contributions"])


async def _contribution_view_put(view_key, payload):
    await edge_cache_put(view_key, json_response(
        payload, cache_seconds=PROFILE_CONTRIBUTION_VIEW_STALE_SECONDS,
        extra_headers={
            PROFILE_CONTRIBUTION_VIEW_BUILT_HEADER: str(int(Date.now()))}))


async def _contribution_view_cached(env, request, view_key, profile_name,
                                    from_value, to_value):
    cached = await edge_cache_match(view_key)
    if cached is None:
        return None
    try:
        built = int(cached.headers.get(
            PROFILE_CONTRIBUTION_VIEW_BUILT_HEADER) or 0)
        payload = json.loads(await cached.text())
    except Exception:
        return None
    if (not isinstance(payload, dict) or payload.get("ok") is not True
            or payload.get("profile") != profile_name
            or payload.get("range") != {"from": from_value, "to": to_value}):
        return None
    age = int(Date.now()) - built
    if not 0 <= age < PROFILE_CONTRIBUTION_VIEW_FRESH_MS and not (
            _host_background(
                env, "contribution-view:" + view_key,
                lambda: _contribution_profile_api(
                    env, request, profile_name, view_refresh=True))):
        return None
    return json_response(
        payload, cache_control="no-store, max-age=0, must-revalidate")
