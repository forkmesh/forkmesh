"""repo sync routes, loaded on demand.

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


async def sync_handler(env, request):
    # GET /api/sync?owner={name}&ts=&sig= — one signed round-trip returning
    # everything the owner's desktop node needs across ALL of its repos:
    # pending issue/pull/discussion inbox items, queued agent prompts
    # (drained on read, same semantics as GET /agents), and the relay's pinned
    # repo state. This replaces the node's old fast polling (4 inbox GETs per
    # repo every 60s + agents every 30s + catalog list for pin checks): the
    # node now calls this on a bounded HTTPS polling schedule.
    # Auth reuses the forkmesh-issues-pull-v1 drain token (_authorize_owner).
    await ensure_schema(env)
    if method_name(request) != "GET":
        return json_response({"error": "method_not_allowed"}, status=405)
    params = parse_qs(urlparse(request.url).query)
    owner = safe_segment(params.get("owner", [""])[0])
    signing_key = await _authorized_owner_signing_key(
        env, request, owner)
    if not signing_key:
        return json_response({"error": "unauthorized"}, status=401)
    issue_claimant_bi = await blind_index(
        env, "issue-inbox-claim:" + signing_key)
    pull_claimant_bi = await blind_index(
        env, "collaboration-inbox-claim:pull_inbox:" + signing_key)
    # Discussions lease exactly like pulls now, under the same claimant index
    # the per-repo DELETE /discussions ack recomputes, so a node that reads a
    # discussion here can ack precisely the rows it merged.
    discussion_claimant_bi = await blind_index(
        env, "collaboration-inbox-claim:discussion_inbox:" + signing_key)
    owner_bi = await blind_index(env, owner)
    rows = await d1_all(
        env, "SELECT key_bi, data FROM repositories WHERE owner_bi=?", owner_bi)
    repo_bis = [row["key_bi"] for row in rows]
    # One query per table across ALL of the owner's repos (IN-list) instead of
    # six queries per repo: /api/sync fires on every pushed event frame AND
    # the fallback poll, and the old 6N sequential D1 roundtrips were a top
    # contributor to the 2026-07-11 free-plan overload. Table names are fixed
    # literals (same convention as _inbox_author_over_quota); inbox items are
    # NOT deleted here — the node acks a merged inbox with its per-repo DELETE.
    marks = ",".join("?" for _ in repo_bis)
    by_repo = {}      # repo_bi -> {topic: [decrypted rows...]}
    merge_sync_by_repo = {}  # repo_bi -> published exact-OID merge receipts
    drain_ids = {}    # repo_bi -> {table: [row ids read]}, for exact deletes
    claimed_elsewhere = {}         # repo_bi -> {topic: count}
    claimed_elsewhere_totals = {}  # topic -> count, account-wide
    if repo_bis:
        now = int(Date.now())
        await d1_run(
            env,
            "UPDATE issue_inbox SET claimed_by_bi=?, claim_expires_at=? "
            f"WHERE repo_bi IN ({marks}) AND "
            "(claimed_by_bi='' OR claim_expires_at<=? OR claimed_by_bi=?)",
            issue_claimant_bi, now + ISSUE_INBOX_CLAIM_TTL_MS,
            *repo_bis, now, issue_claimant_bi,
        )
        await d1_run(
            env,
            "UPDATE pull_inbox SET claimed_by_bi=?, claim_expires_at=? "
            f"WHERE repo_bi IN ({marks}) AND mirrored_at=0 AND "
            "(claimed_by_bi='' OR claim_expires_at<=? OR claimed_by_bi=?)",
            pull_claimant_bi, now + ISSUE_INBOX_CLAIM_TTL_MS,
            *repo_bis, now, pull_claimant_bi,
        )
        await d1_run(
            env,
            "UPDATE discussion_inbox SET claimed_by_bi=?, claim_expires_at=? "
            f"WHERE repo_bi IN ({marks}) AND mirrored_at=0 AND "
            "(claimed_by_bi='' OR claim_expires_at<=? OR claimed_by_bi=?)",
            discussion_claimant_bi, now + ISSUE_INBOX_CLAIM_TTL_MS,
            *repo_bis, now, discussion_claimant_bi,
        )
        for topic, table, ordered in (
                ("issues", "issue_inbox", True),
                ("pulls", "pull_inbox", True),
                ("discussions", "discussion_inbox", True),
                ("agentPrompts", "agent_prompts", True),
                ("branchOps", "branch_op_inbox", True),
                ("aboutUpdate", "about_inbox", False)):
            order = " ORDER BY id ASC" if ordered else ""
            id_col = "rowid AS drain_id" if table == "about_inbox" else "id AS drain_id"
            claim_bi = (
                issue_claimant_bi if table == "issue_inbox"
                else pull_claimant_bi if table == "pull_inbox"
                else discussion_claimant_bi
                if table == "discussion_inbox" else "")
            claim_filter = " AND claimed_by_bi=?" if claim_bi else ""
            table_rows = await d1_all(
                env,
                f"SELECT repo_bi, data, {id_col} FROM {table}"
                f" WHERE repo_bi IN ({marks})" + claim_filter + order,
                *repo_bis, *([claim_bi] if claim_bi else []))
            for r in table_rows or []:
                owner_crypto = globals().get("security_control")
                sealed = (
                    owner_crypto.decode_owner_envelope(r.get("data"))
                    if table == "agent_prompts" and owner_crypto is not None
                    else None
                )
                if sealed:
                    routing = sealed.get("routing") or {}
                    item = {
                        "queueId": int(r.get("drain_id") or 0),
                        "agentId": int(routing.get("agentId") or 0),
                        "queuedAt": int(routing.get("queuedAt") or 0),
                        "encrypted": True,
                        "envelope": sealed["envelope"],
                    }
                elif table == "agent_prompts" and owner_crypto is not None:
                    # Production always enforces owner-sealed agent payloads.
                    # A legacy row cannot be safely decrypted by the relay; it
                    # is retained for an owner-run offline migration instead.
                    item = None
                else:
                    item = await decrypt_row(env, r.get("data"))
                if item:
                    # Carry the inbox row id so a node draining from /api/sync
                    # can ack exactly what it merged (DELETE ...?ids=), leaving
                    # any submission that landed mid-drain for the next sync
                    # instead of blanket-deleting it undelivered (adhoc #97).
                    drain_id = r.get("drain_id")
                    if drain_id is not None and topic in (
                            "issues", "pulls", "discussions", "branchOps"):
                        item["id"] = drain_id
                    by_repo.setdefault(r.get("repo_bi"), {}) \
                        .setdefault(topic, []).append(item)
                # Track the exact rows READ (even undecryptable ones — they
                # can never be delivered, so draining them is correct) so the
                # drain deletes below can't sweep away rows inserted while
                # this request was still decrypting (drop-undelivered race).
                # Owner-sealed prompts are acknowledged only after the desktop
                # decrypts and accepts them. Legacy prompts keep drain-on-read
                # compatibility; About updates are unchanged.
                if (table in ("about_inbox", "branch_op_inbox")
                        or (table == "agent_prompts" and not sealed
                            and owner_crypto is None)):
                    drain_ids.setdefault(r.get("repo_bi"), {}) \
                        .setdefault(table, []).append(r.get("drain_id"))
        # Claim starvation, made observable. Two nodes of ONE account (a
        # desktop and a phone) both authorize from _owner_signing_pubkeys but
        # claim under their OWN blind index, so whichever polls first holds
        # every row for ISSUE_INBOX_CLAIM_TTL_MS and the loser reads an empty
        # inbox with nothing to explain it. Count — never name — the rows some
        # other key currently holds so the caller can say "another node is
        # draining this account" instead of "nothing pending". The claimant
        # index is an opaque blind index and never leaves D1; only integers do.
        # An approved mirror's lease counts here too, which is correct: those
        # rows really are being drained by someone else.
        elsewhere_rows = await d1_all(
            env,
            "SELECT 'issues' AS k, repo_bi, COUNT(*) AS c FROM issue_inbox "
            f"WHERE repo_bi IN ({marks}) AND claimed_by_bi NOT IN ('', ?) "
            "AND claim_expires_at>? GROUP BY repo_bi "
            "UNION ALL SELECT 'pulls', repo_bi, COUNT(*) FROM pull_inbox "
            f"WHERE repo_bi IN ({marks}) AND claimed_by_bi NOT IN ('', ?) "
            "AND claim_expires_at>? GROUP BY repo_bi "
            "UNION ALL SELECT 'discussions', repo_bi, COUNT(*) "
            "FROM discussion_inbox "
            f"WHERE repo_bi IN ({marks}) AND claimed_by_bi NOT IN ('', ?) "
            "AND claim_expires_at>? GROUP BY repo_bi",
            *repo_bis, issue_claimant_bi, now,
            *repo_bis, pull_claimant_bi, now,
            *repo_bis, discussion_claimant_bi, now,
        )
        for elsewhere_row in elsewhere_rows or []:
            kind = str(elsewhere_row.get("k") or "")
            count = int(elsewhere_row.get("c") or 0)
            if not kind or count <= 0:
                continue
            claimed_elsewhere.setdefault(
                elsewhere_row.get("repo_bi"), {})[kind] = count
            claimed_elsewhere_totals[kind] = (
                claimed_elsewhere_totals.get(kind, 0) + count)
        # A merge performed while the source-of-truth node was offline is a
        # durable relay job, not an ordinary PR event. Carry its exact,
        # already-validated public object IDs in the same signed sync response
        # so the owner node can reconcile the published mirror state later.
        # The selected mirror, actor, paths, and credentials never leave D1.
        merge_rows = await d1_all(
            env,
            "SELECT repo_bi,request_id,pull_number,result,updated_at "
            "FROM repo_merge_jobs "
            f"WHERE repo_bi IN ({marks}) AND status='succeeded' "
            "AND expires_at>? ORDER BY updated_at ASC",
            *repo_bis, now,
        )
        for merge_row in merge_rows or []:
            try:
                result = json.loads(str(merge_row.get("result") or ""))
            except Exception:
                continue
            request_id = clean_string(
                merge_row.get("request_id", ""), 80).strip()
            fields = {
                key: str(result.get(key) or "").strip().lower()
                for key in (
                    "baseBefore", "head", "pullsBefore",
                    "baseAfter", "pullsAfter",
                )
            }
            try:
                pull_number = int(merge_row.get("pull_number") or 0)
            except (TypeError, ValueError, OverflowError):
                continue
            if (
                result.get("ok") is not True
                or result.get("status") != "merged"
                or result.get("published") is not True
                or str(result.get("requestId") or "") != request_id
                or not HTTPS_MIRROR_MERGE_REQUEST_RE.fullmatch(request_id)
                or not 1 <= pull_number <= 999999999
                or any(
                    not re.fullmatch(
                        r"(?:[0-9a-f]{40}|[0-9a-f]{64})", oid)
                    for oid in fields.values()
                )
                or len({len(oid) for oid in fields.values()}) != 1
            ):
                continue
            merge_sync_by_repo.setdefault(
                merge_row.get("repo_bi"), []).append({
                    "requestId": request_id,
                    "pullNumber": pull_number,
                    **fields,
                })
    repos = []
    for row in rows:
        rec = await decrypt_row(env, row["data"])
        if not rec:
            continue
        repo_bi = row["key_bi"]
        pending = by_repo.get(repo_bi, {})
        entry = {
            "name": clean_string(rec.get("name", ""), 120),
            "owner": clean_string(rec.get("owner", owner), 120),
            # The relay's currently pinned state, so the node can detect pin
            # drift without fetching the whole catalog list.
            "stateHash": rec.get("stateHash", ""),
            "updatedAt": rec.get("updatedAt", ""),
            "source": rec.get("source", ""),
            "issues": pending.get("issues", []),
            "pulls": pending.get("pulls", []),
            "discussions": pending.get("discussions", []),
            "commits": pending.get("commits", []),
            "mirrorMerges": merge_sync_by_repo.get(repo_bi, []),
        }
        # Only present when non-zero, so an account with one node keeps the
        # exact response body it has today.
        elsewhere = claimed_elsewhere.get(repo_bi)
        if elsewhere:
            entry["claimedElsewhere"] = elsewhere
        # Drain-on-read deletes target the EXACT rows this request read (by
        # id), never the whole repo_bi: a prompt/About edit inserted while
        # this request was decrypting must survive for the next sync instead
        # of being swept away undelivered.
        prompts = pending.get("agentPrompts", [])
        prompt_ids = drain_ids.get(repo_bi, {}).get("agent_prompts", [])
        if prompt_ids:
            await d1_run(
                env,
                "DELETE FROM agent_prompts WHERE id IN (%s)"
                % ",".join("?" for _ in prompt_ids),
                *prompt_ids)
        entry["agentPrompts"] = prompts
        if any(bool(p.get("encrypted")) for p in prompts
               if isinstance(p, dict)):
            entry["agentPromptAckPath"] = (
                "/api/repo/%s/%s/agents/ack" % (
                    entry.get("owner", owner), entry.get("name", "")))
        # Web About edits, drained on read like agent prompts: the node writes
        # the change into the repo's committed .forkmesh/info.json.
        #
        # WHY THESE TWO STAY READ-DESTRUCTIVE (reviewed 2026-08-25 against the
        # mobile-node work, which asked for issue/pull-style acks here):
        #
        #  * There is no ack route for about_inbox at all, and the legacy
        #    (unsealed) agent_prompts branch below is a pre-E2EE compatibility
        #    shim that production never takes — `security_control` is always
        #    present there, so a real deployment already keeps prompts until
        #    /api/repo/{owner}/{repo}/agents/ack. Only the legacy branch and
        #    About edits are read-destructive.
        #  * Turning About edits ack-based means the relay keeps redelivering
        #    the same edit until someone acks. The desktop applies an About
        #    edit by COMMITTING .forkmesh/info.json, so an unacked redelivery
        #    is a fresh commit on every poll, and the desktop cannot be taught
        #    to ack from here (it ships independently). The failure mode of the
        #    "correct" model is therefore worse than the one it fixes, and it
        #    would break the existing desktop drain contract outright.
        #  * The loss window it leaves — a non-hosting node polling while the
        #    desktop is offline — is closed on the client side instead: a
        #    mobile node stays in standby whenever a desktop node is enrolled
        #    on the account, and persists whatever it does drain locally
        #    (native/ios/ForkMeshCore/Sources/FMChat/NodeSync/NodeSyncService.swift,
        #    `carried`).
        #
        # So that a client never has to infer this from behaviour, the response
        # names the kinds it must persist itself: `readDestructiveKinds`.
        abouts = pending.get("aboutUpdate", [])
        about_ids = drain_ids.get(repo_bi, {}).get("about_inbox", [])
        if abouts:
            entry["aboutUpdate"] = abouts[0]
        if about_ids:
            await d1_run(
                env,
                "DELETE FROM about_inbox WHERE rowid IN (%s)"
                % ",".join("?" for _ in about_ids),
                *about_ids)
        # Branch management requested on the website (adhoc #2098). Delivered
        # and drained the same way, and read-destructive for the same reason:
        # there is no ack route, and every operation is idempotent on the
        # node (a branch already deleted, created or current is a no-op), so
        # redelivery would buy nothing an owner can see.
        branch_ops = pending.get("branchOps", [])
        branch_op_ids = drain_ids.get(repo_bi, {}).get("branch_op_inbox", [])
        if branch_ops:
            entry["branchOps"] = branch_ops
        if branch_op_ids:
            await d1_run(
                env,
                "DELETE FROM branch_op_inbox WHERE id IN (%s)"
                % ",".join("?" for _ in branch_op_ids),
                *branch_op_ids)
        repos.append(entry)
    return json_response({
        "ok": True,
        "serverTime": int(Date.now()),
        "repos": repos,
        # Account-wide tally of inbox rows this caller was NOT shown because
        # another signing key holds an unexpired lease on them. Zeros mean
        # "nothing pending"; a non-zero count means "another node of this
        # account (or an approved mirror) is draining right now". No key,
        # device id or claimant index is exposed — counts only.
        "claimedElsewhere": {
            "issues": claimed_elsewhere_totals.get("issues", 0),
            "pulls": claimed_elsewhere_totals.get("pulls", 0),
            "discussions": claimed_elsewhere_totals.get("discussions", 0),
            "total": sum(claimed_elsewhere_totals.values()),
        },
        # Kinds this response DELETED as it handed them over. A caller that
        # cannot apply them must persist them locally; it cannot ask for them
        # again. See the block comment above for why they are not ack-based.
        "readDestructiveKinds": (
            ["aboutUpdate", "branchOps"]
            if globals().get("security_control") is not None
            else ["aboutUpdate", "branchOps", "agentPrompts"]),
    })
