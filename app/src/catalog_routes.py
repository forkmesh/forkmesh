"""Request-lazy repository catalog HTTP handler.

The catalog performs D1, crypto, and publication work only after the request
has entered ForkMesh's bounded App ingress Durable Object. Keeping its large
route implementation outside entry.py restores Python Worker startup headroom
without moving work back into the stateless Free-plan CPU path.
"""


def _bind_runtime(runtime):
    """Supply the initialized entrypoint primitives used by this route."""
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


async def rehome_organization_aliases_before_node_delete(
        env, target, account_owner):
    """Move durable org aliases off a disposable node before retirement."""
    linked_aliases = await d1_all(
        env,
        "SELECT org_bi,repo FROM org_repos WHERE node_owner=? ORDER BY repo",
        target,
    )
    if not linked_aliases:
        return [], ""
    now = int(Date.now())
    endpoint_rows = await d1_all(
        env,
        "SELECT node_name FROM mirror_https_endpoints "
        "WHERE checked_at>=? AND forkmesh_verified_at>=? AND healthy=1 "
        "AND forkmesh_active=1 AND integrity='ok' AND abuse_blocked=0 "
        "ORDER BY node_name",
        now - HTTPS_MIRROR_STATUS_FRESH_MS,
        now - HTTPS_MIRROR_STATUS_FRESH_MS,
    )
    fresh_nodes = {
        clean_string(row.get("node_name", ""), MAX_NODE_NAME).lower()
        for row in endpoint_rows or []
    }
    catalog_rows = await _decrypted_public_catalog(env, now)
    handoffs = []
    for linked in linked_aliases:
        linked_repo = clean_string(
            linked.get("repo", ""), MAX_REPO_SEGMENT).lower()
        target_record = next((
            (row or {}).get("data") or {}
            for row in catalog_rows or []
            if clean_string(
                ((row or {}).get("data") or {}).get("owner", ""),
                MAX_NODE_NAME,
            ).lower() == target
            and clean_string(
                ((row or {}).get("data") or {}).get("name", ""),
                MAX_REPO_SEGMENT,
            ).lower() == linked_repo
        ), None)
        candidates = []
        if target_record:
            for row in catalog_rows or []:
                candidate = (row or {}).get("data") or {}
                candidate_node = clean_string(
                    candidate.get("machineName")
                    or candidate.get("owner", ""),
                    MAX_NODE_NAME,
                ).lower()
                if (
                    candidate_node == target
                    or candidate_node not in fresh_nodes
                    or clean_string(
                        candidate.get("name", ""),
                        MAX_REPO_SEGMENT,
                    ).lower() != linked_repo
                    or not repo_mirror_same_group(target_record, candidate)
                ):
                    continue
                if account_owner and await _account_owns_node(
                        env, account_owner, candidate_node):
                    candidates.append(candidate_node)
        candidates = sorted(set(candidates))
        if not candidates:
            # Leave the old node and every alias untouched until a replacement
            # exists; callers return a fail-closed 409.
            return [], linked_repo
        handoffs.append({
            "orgBi": linked.get("org_bi"),
            "repo": linked_repo,
            "node": candidates[0],
        })
    for handoff in handoffs:
        await d1_run(
            env,
            "UPDATE org_repos SET node_owner=?,created_at=? "
            "WHERE org_bi=? AND repo=? AND node_owner=?",
            handoff["node"], now, handoff["orgBi"], handoff["repo"], target,
        )
    _ORG_ALIAS_MEMO.clear()
    await purge_catalog_related_caches()
    await schedule_public_catalog_republish(
        env,
        include_flagship=any(
            handoff["repo"] == "forkmesh" for handoff in handoffs),
    )
    return handoffs, ""


# Record fields a publisher legitimately refreshes on every heartbeat
# republish. Two records that differ only here describe the same repository
# state; treating such a POST as a change meant every mirror heartbeat
# (each node, every 30s-4min) re-encrypted the row, purged the shared edge
# caches, and re-challenged endpoint health, keeping /api/repositories
# permanently cache-cold.
CATALOG_VOLATILE_FIELDS = frozenset((
    "updatedAt", "lastSync", "signature", "stateSig",
    "cpuPercent", "memUsedBytes", "memTotalBytes",
    "diskUsedBytes", "diskTotalBytes", "sizeBytes",
    "clonesServed", "websiteServed", "cloneServedAt", "websiteServedAt",
    "cloneServedAgent", "websiteServedAgent", "activityWeeks",
    "mergeActivityWeeks",
))

# How long an unchanged republish may skip the D1 write entirely. Within the
# window the stored record keeps its prior lastSync lease; after it, one
# write renews the lease (and its volatile telemetry) without touching the
# shared caches. Kept just below the Go mirror daemon's 10-minute
# catalogRepublishInterval so every renewal it does send lands as a write.
CATALOG_UNCHANGED_LEASE_MS = 8 * 60 * 1000


def _catalog_ms(value):
    try:
        return int(str(value or "0"))
    except (TypeError, ValueError):
        return 0


def catalog_meaningful_change(prior, record):
    """True when two catalog records differ beyond volatile telemetry."""
    def significant(rec):
        return {
            key: value for key, value in (rec or {}).items()
            if key not in CATALOG_VOLATILE_FIELDS
        }
    return significant(prior) != significant(record)


def catalog_push_observed(record, exists, prior_commit, prior_state_hash):
    """Whether an accepted publication reports refs that actually moved.

    A node publishes the state it now advertises rather than receiving a
    push, so "the signed refs digest, or the default head, differs from the
    record we held" is the push event as this relay can observe it. Only a
    source publication counts: a mirror's "remote-clone" publication echoes
    the state after its own fetch and would otherwise report one push twice.

    Visibility is deliberately not part of this. A private repository is
    pushed to exactly like a public one, and its owner is who wants to hear
    about it (adhoc #295); it is the mirror fleet, below, that has no business
    with a private push.
    """
    if not exists or record.get("source") != "local-node":
        return False
    new_commit = clean_string(record.get("commit", ""), 64).strip().lower()
    current_state = clean_string(
        record.get("stateHash", ""), 64).strip().lower()
    head_moved = (
        bool(re.fullmatch(r"[0-9a-f]{40,64}", new_commit))
        and new_commit != str(prior_commit or "").strip().lower()
    )
    # stateHash covers every advertised head and tag. A feature-branch or tag
    # push can leave `commit` (the default branch tip) unchanged while the
    # mirror is nevertheless stale, so it must be an equal wake-up signal.
    refs_moved = (
        bool(re.fullmatch(r"[0-9a-f]{64}", current_state))
        and current_state != str(prior_state_hash or "").strip().lower()
    )
    return head_moved or refs_moved


def catalog_mirror_update_needed(
        record, exists, prior_commit, prior_state_hash):
    """Whether an accepted source publication must wake the mirror fleet."""
    if record.get("visibility") != "public":
        return False
    return catalog_push_observed(
        record, exists, prior_commit, prior_state_hash)


def catalog_push_commit_delta(record, prior):
    """How many commits a moved default head added, or 0 when unknown.

    Both publications carry the default branch's total commitCount; their
    difference is the push, as long as the branch is the same one and the
    count grew (a force push or a switched default branch says nothing
    reliable, so it is reported as "new commits" rather than a wrong number).
    """
    record = record if isinstance(record, dict) else {}
    prior = prior if isinstance(prior, dict) else {}
    if (clean_string(record.get("branch", ""), 120)
            != clean_string(prior.get("branch", ""), 120)):
        return 0
    try:
        before = int(str(prior.get("commitCount", "") or "").strip())
        after = int(str(record.get("commitCount", "") or "").strip())
    except (TypeError, ValueError):
        return 0
    delta = after - before
    return delta if before > 0 and delta > 0 else 0


async def _bump_catalog_rev(env):
    # Monotonic change counter behind the weak /api/repositories ETag; bumped
    # only when a write changed a record beyond volatile telemetry.
    # Best-effort: a missed bump only costs revalidating clients one full
    # refetch.
    try:
        await d1_run(
            env,
            "INSERT INTO catalog_rev (id, rev, ts) VALUES (1, 1, ?) "
            "ON CONFLICT(id) DO UPDATE SET rev=rev+1, ts=excluded.ts",
            int(Date.now()))
    except Exception:
        pass
    # This isolate's rev-keyed catalog/count memos were read under the old
    # counter; drop them so the mutation is visible here without waiting for
    # the probe TTL (other isolates learn it from the counter itself).
    mark_stale = globals().get("_catalog_memos_mark_stale")
    if callable(mark_stale):
        mark_stale()


async def _catalog_etag(env):
    try:
        row = await d1_first(env, "SELECT rev FROM catalog_rev WHERE id=1")
        rev = int((row or {}).get("rev") or 0)
    except Exception:
        return ""
    return 'W/"fmcat-%d"' % rev


async def catalog_handler(env, request):
    await ensure_schema(env)
    method = method_name(request)
    if method == "GET":
        # A logged-in owner may sign a short-lived forkmesh-catalog-view-v1 token to
        # additionally receive their OWN private repos (anonymous callers, and the
        # static website with no key, still get the public-only list). The result is
        # per-viewer, so an authenticated request must never read from or write to
        # the SHARED public edge cache — that would leak private repos to everyone.
        params = parse_qs(urlparse(
            str(getattr(request, "url", "") or "")).query)
        admin_query = _admin_query(params.get("admin", [""])[0])
        headers = getattr(request, "headers", {}) or {}
        cache_control = (headers.get("cache-control") or "").lower()
        bypass_cache = bool(
            params.get("fresh") or params.get("_") or params.get("fmv") or
            "no-cache" in cache_control or "no-store" in cache_control)
        viewer = safe_segment(params.get("viewer", [""])[0])
        view_ts = clean_string(params.get("ts", [""])[0], 20)
        view_sig = clean_string(params.get("sig", [""])[0], 200)
        authed_viewer = ""
        if viewer and view_sig:
            authed_viewer = await verify_catalog_view_token(
                env, viewer, view_ts, view_sig)
        catalog_etag = ""
        if authed_viewer:
            # The desktop revalidates its periodic catalog re-attest read with
            # the weak change-counter ETag; an unchanged catalog answers 304
            # for one indexed D1 read instead of a full-table decrypt. Only
            # this per-viewer no-store path carries the ETag — the public
            # cached response deliberately stays ETag-free so browsers keep
            # revalidating liveness fields on the normal 30-second TTL.
            catalog_etag = await _catalog_etag(env)
            if_none_match = clean_string(
                headers.get("if-none-match") or "", 80).strip()
            if (catalog_etag and not bypass_cache
                    and if_none_match == catalog_etag):
                return Response(None, status=304, headers={
                    "etag": catalog_etag,
                    "cache-control": "no-store, max-age=0, must-revalidate",
                })
        if not authed_viewer and not bypass_cache:
            cached = await edge_cache_match(CATALOG_CACHE_KEY)
            if cached is not None:
                return cached
        # Best-effort D1 housekeeping (drop blocked phantoms, prune stale nodes)
        # only on the cheap, edge-cached anonymous miss. The response is already
        # correct without it — blocked entries are skipped by
        # _is_blocked_catalog_identity — and the staggered cron sweeps D1 on its
        # own schedule. An
        # authenticated (per-viewer, never-cached) request must NOT run these:
        # replaying a burst of DELETEs plus host-offline notifications inline on
        # every catalog load held the single Worker event loop long enough for
        # the runtime to cancel the request as hung ("Cannot enter into task").
        # Housekeeping moved off the request path entirely (2026-07-11): the
        # staggered cron already runs purge_blocked_catalog (minute%15==9) and
        # purge_stale_registered_nodes (minute%15==4), and the response is
        # correct without them — blocked entries are skipped by
        # _is_blocked_catalog_identity, while stale signed catalog entries stay
        # visible with offline availability. Running purges on every 10s cache
        # miss helped melt the free plan during the 2026-07-11 overload.
        # Public repos are listed for everyone; an authenticated viewer additionally
        # gets the private repos they own (matched by blind index) AND any private
        # repo another owner has shared with them (issue #9, via the repo_shares
        # ACL). No other owner's un-shared private repos are ever returned.
        if authed_viewer:
            viewer_bi = await blind_index(env, authed_viewer)
            rows = await d1_all(
                env,
                "SELECT key_bi, owner_bi, data FROM repositories "
                "WHERE is_private = 0 OR owner_bi = ? OR key_bi IN "
                "(SELECT repo_bi FROM repo_shares WHERE grantee_bi = ?)",
                viewer_bi, viewer_bi)
        else:
            rows = await d1_all(
                env,
                "SELECT key_bi, owner_bi, data FROM repositories "
                "WHERE is_private = 0")
        # Repository sockets and their host_presence heartbeat were retired when
        # repository bytes moved to signed direct-HTTPS mirrors. Derive catalog
        # availability from that same trust boundary: a fresh, non-blocked
        # endpoint whose account-bound Ed25519 challenge proved the flagship
        # refs and required operations. node_bi and repositories.owner_bi use
        # the same blind-index domain, so no plaintext node join or client probe
        # is needed here.
        cutoff = int(Date.now()) - HTTPS_MIRROR_STATUS_FRESH_MS
        live_rows = await d1_all(
            env,
            """SELECT node_bi FROM mirror_https_endpoints
                WHERE checked_at>=? AND forkmesh_verified_at>=?
                  AND healthy=1 AND forkmesh_active=1
                  AND integrity='ok' AND abuse_blocked=0""",
            cutoff, cutoff,
        )
        live_endpoint_nodes = {
            str(row.get("node_bi") or "")
            for row in live_rows or []
            if str(row.get("node_bi") or "")
        }
        hosted_serving = {}
        hosted_cores = globals().get("hosted_core_nodes")
        if callable(hosted_cores) and hosted_cores(env):
            try:
                copy_rows = await d1_all(
                    env,
                    "SELECT h.key_bi AS key_bi, c.node AS node "
                    "FROM hosted_repos h JOIN hosted_repo_copies c "
                    "ON c.repo_id=h.id WHERE h.deleted_at=0 AND c.empty=0",
                )
            except Exception:
                copy_rows = []
            node_bis = {}
            for row in copy_rows or []:
                node = clean_string(row.get("node", ""), MAX_NODE_NAME).lower()
                key = str(row.get("key_bi") or "")
                if not node or not key:
                    continue
                if node not in node_bis:
                    node_bis[node] = str(await blind_index(env, node) or "")
                if node_bis[node] in live_endpoint_nodes:
                    hosted_serving.setdefault(key, set()).add(node)
        terms_rows = await d1_all(
            env,
            "SELECT repo_bi,category FROM repo_terms_flags WHERE active=1",
        )
        terms_flags = {
            str(row.get("repo_bi") or ""): clean_string(
                row.get("category"), 20).lower()
            for row in terms_rows or []
            if str(row.get("repo_bi") or "")
        }
        repos = []
        repository_owner_rows = []
        public_canonical_repo_bis = set()
        # Private authority records deliberately have blank plaintext identity
        # columns. Their canonical node id is already authenticated inside the
        # encrypted catalog row and is reconstructed below only after the
        # owner/share ACL succeeds.
        authority_rows = await d1_all(
            env, "SELECT repo_bi,node_id FROM repo_source_authorities "
                 "WHERE is_private=0")
        authority_by_repo = {
            str(value.get("repo_bi") or ""):
                clean_string(value.get("node_id", ""), 64)
            for value in authority_rows or []
        }
        ssh_gateway = _ssh_gateway_settings(env)
        for r in rows:
            rec = await decrypt_row(env, r["data"])
            if rec:
                owner_bi = r.get("owner_bi")
                visibility = rec.get("visibility")
                # Never trust the historical plaintext flag by itself:
                # migration 0011 defaulted pre-existing rows to public. Only a
                # signed record explicitly marked public is anonymous catalog
                # data. A private record selected through a stale flag still
                # requires an owner/share ACL recheck.
                if visibility not in ("public", "private"):
                    continue
                if not owner_bi:
                    rec_owner = clean_string(
                        rec.get("owner", ""), MAX_NODE_NAME).lower()
                    owner_bi = (
                        await blind_index(env, rec_owner)
                        if rec_owner else "")
                if visibility == "private":
                    if not authed_viewer:
                        continue
                    if str(owner_bi or "") != str(viewer_bi):
                        shared = await d1_first(
                            env,
                            "SELECT 1 AS one FROM repo_shares "
                            "WHERE repo_bi=? AND grantee_bi=?",
                            r.get("key_bi"), viewer_bi,
                        )
                        if not shared:
                            continue
                # Defense in depth: never surface a blocked identity even if a
                # row slipped in before the purge ran.
                if _is_blocked_catalog_identity(
                        env, rec.get("owner"), rec.get("name")):
                    continue
                if visibility == "public":
                    public_canonical_repo_bis.add(
                        str(r.get("key_bi") or ""))
                rec["liveHost"] = (
                    str(owner_bi or "") in live_endpoint_nodes
                    or (rec.get("source") == "hosted-web"
                        and bool(hosted_serving.get(
                            str(r.get("key_bi") or ""))))
                )
                authority_node_id = authority_by_repo.get(
                    str(r.get("key_bi") or ""), "")
                if visibility == "private" and not authority_node_id:
                    authority_node_id = clean_string(
                        rec.get("nodeId", ""), 64)
                if authority_node_id:
                    rec["canonicalNodeId"] = authority_node_id
                    rec["sourceAuthority"] = (
                        clean_string(rec.get("nodeId", ""), 64)
                        == authority_node_id)
                # Plaintext flag so clients can badge private repos without
                # re-deriving it from the visibility string.
                rec["isPrivate"] = rec.get("visibility") == "private"
                # A private repo surfaced to a viewer who is NOT its owner can only
                # be here because it was shared with them (issue #9). The client
                # badges it and clones it with the grantee view token, not the
                # owner one.
                rec["sharedWithMe"] = bool(
                    authed_viewer and rec["isPrivate"]
                    and rec.get("owner") != authed_viewer)
                terms_category = terms_flags.get(str(r.get("key_bi") or ""))
                rec["termsFlagged"] = bool(terms_category)
                if terms_category:
                    rec["termsCategory"] = terms_category
                # Publish an SSH clone/push URL only when a separate gateway
                # host and its Worker authorization token are both configured.
                # The URL is never a claim that this HTTP Worker terminates SSH.
                if ssh_gateway["configured"]:
                    ssh_url = _ssh_repository_url(
                        env, rec.get("owner", ""), rec.get("name", ""))
                    if ssh_url:
                        rec["sshUrl"] = ssh_url
                # Only this ACL-authorized, no-store catalog response may reveal
                # the random client locator for an encrypted private archive.
                # Anonymous/public catalog rows never reach this branch for a
                # private repository, and the locator itself contains no owner
                # or repository name. It is still not a bearer credential:
                # archive reads independently require the owner/share signature.
                if rec["isPrivate"]:
                    access_id = await _https_mirror_private_catalog_access_id(
                        env, r["key_bi"], int(rec.get("keyEpoch") or 0))
                    if access_id:
                        rec["privateAccessId"] = access_id
                        rec["privateAccessPath"] = (
                            "/api/private-replicas/" + access_id)
                repos.append(rec)
                repository_owner_rows.append((rec, owner_bi))
        # Same-account secondary devices have their own signed telemetry and
        # serving endpoint, but never replace the canonical catalog row. Return
        # them as explicit mirrors so node lists and failover tooling retain
        # their capacity. Private device mirrors stay owner-only; anonymous
        # responses include public records only.
        mirror_rows = await d1_all(
            env,
            "SELECT repo_bi,node_id,data FROM repo_device_mirrors "
            "ORDER BY updated_at DESC LIMIT ?",
            MAX_CATALOG_REPOS,
        )
        for mirror_row in mirror_rows or []:
            mirror = await decrypt_row(env, mirror_row.get("data"))
            if not mirror:
                continue
            if not _device_mirror_has_public_canonical(
                    mirror, mirror_row.get("repo_bi"),
                    public_canonical_repo_bis):
                # A device row is subordinate to its canonical repository.
                # Never let an older public device record resurrect a canonical
                # repo that is now private, deleted, or moderation-filtered.
                continue
            if mirror.get("visibility") != "public":
                if not authed_viewer or mirror.get("owner") != authed_viewer:
                    continue
            mirror["reportedSource"] = mirror.get(
                "reportedSource", mirror.get("source"))
            mirror["source"] = "remote-clone"
            mirror["sourceAuthority"] = False
            mirror["canonicalNodeId"] = authority_by_repo.get(
                str(mirror_row.get("repo_bi") or ""), "")
            mirror["isPrivate"] = mirror.get("visibility") == "private"
            mirror["liveHost"] = False
            repos.append(mirror)
            repository_owner_rows.append((mirror, ""))
        await _catalog_logical_owners(env, repository_owner_rows)
        # Second pass: mark each repo cloneable when its own host is offline but a
        # peer mirroring the same logical repo is online — the relay serves that
        # mirror in place through the repo's own URL (adhoc #61), so the website
        # shows the repo as available (and which nodes are live) instead of a
        # bare "host offline". served_mirror_groups sees the whole public list, so
        # this works even when the freshest live node is a different owner's mirror.
        served = served_mirror_groups(repos)
        for rec in repos:
            rec["cloneOnline"] = repo_clone_online(rec, served)
        repos.sort(key=lambda x: x.get("updatedAt", ""), reverse=True)
        payload = {
            "ok": True,
            "repositories": repos[:MAX_CATALOG_REPOS],
            "sshGatewayConfigured": ssh_gateway["configured"],
            "sshTransport": (
                "node-side-forced-command"
                if ssh_gateway["configured"] else "not-configured"),
            "payoutCustody": "external-self-custodial-public-addresses",
            "payoutNotice": (
                "Repository records may include an operator's optional public "
                "payout address only. ForkMesh holds no wallet key or user "
                "funds, and publication does not guarantee a reward."
            ),
        }
        # Per-viewer responses (with private repos) must never touch the shared
        # public cache — and must say no-store themselves so no downstream cache
        # holds them either; only the public-only list is cacheable.
        if authed_viewer or bypass_cache:
            return json_response(
                payload,
                cache_control="no-store, max-age=0, must-revalidate",
                extra_headers=(
                    {"etag": catalog_etag} if catalog_etag else None))
        resp = json_response(payload, cache_seconds=CATALOG_TTL)
        await edge_cache_put(CATALOG_CACHE_KEY, resp)
        return resp

    if method == "POST":
        try:
            data = await bounded_json_request(request)
        except Exception:
            return json_response({"error": "invalid_json"}, status=400)
        record = safe_catalog_record(data)
        if not record:
            return json_response(
                {"error": "owner_name_and_maintainer_required"}, status=400
            )
        owner = record["owner"]
        # Blocked phantom identities can never (re)enter the catalog.
        if _is_blocked_catalog_identity(env, owner, record["name"]):
            return json_response({"error": "blocked"}, status=403)
        # Repos are namespaced under a registered account, and only that account's
        # key holder may write its namespace. This ties repo identity to the
        # account (fixes duplicate forks) and prevents impersonation. A registered
        # account is REQUIRED — there is no self-asserted-maintainer fallback.
        # One account fetch serves both the signing-key check and the node touch
        # below (the old code fetched + decrypted the same record twice).
        # The account qualifies with its primary pubkey OR at least one
        # enabled owner_sign device key (_owner_signing_pubkeys), so a
        # phone-only account without a promoted primary key can publish
        # (mobile-node-contract.md section 7). An account with neither keeps
        # the historical account_required failure.
        _, publish_owner_rec = await _account_row(env, owner)
        if not publish_owner_rec:
            return json_response({"error": "account_required"}, status=403)
        if not await _owner_signing_pubkeys(env, owner):
            return json_response({"error": "account_required"}, status=403)
        owner_pub = await _catalog_publication_key(
            env, owner, record["maintainer"])
        if not owner_pub:
            return json_response({"error": "maintainer_mismatch"}, status=403)
        catalog_sig = clean_string(data.get("catalogSig", ""), 200)
        try:
            catalog_sig_version = int(data.get("catalogSigVersion", 1) or 1)
        except (TypeError, ValueError):
            catalog_sig_version = 0
        if catalog_sig_version == 2:
            # V2 binds every normalized record field, including visibility and
            # the served-ref fingerprint. A relay or replay holder cannot turn
            # a private publication into a public one while reusing a signature.
            signed_record = dict(record)
            signed_record.pop("signature", None)
            record_hash = hashlib.sha256(json.dumps(
                signed_record, sort_keys=True,
                separators=(",", ":")).encode()).hexdigest()
            canonical = ("forkmesh-catalog-v2\n" + record_hash).encode()
        else:
            # Legacy signatures did not bind visibility. They remain accepted
            # only for explicitly public repositories; private publication must
            # upgrade before the relay stores any confidential metadata.
            if record["visibility"] != "public":
                return json_response(
                    {
                        "error": "client_upgrade_required",
                        "requiredCatalogSigVersion": 2,
                    },
                    status=426,
                )
            canonical = (
                "forkmesh-catalog-v1\n" + owner + "\n" + record["name"] +
                "\n" + record["updatedAt"]
            ).encode()
        if not await ed25519_verify(owner_pub, catalog_sig, canonical):
            return json_response({"error": "bad_signature"}, status=401)
        if record["visibility"] == "private":
            opaque_repo_id = record.get("opaqueRepoId", "")
            manifest_hash = record.get("encryptedManifestHash", "")
            manifest_sig = record.get("encryptedManifestSig", "")
            key_epoch = int(record.get("keyEpoch") or 0)
            if (record.get("mirrorEncryption") != "owner-sealed-v1"
                    or not re.fullmatch(r"[a-f0-9]{32}", opaque_repo_id)
                    or key_epoch < 1
                    or not re.fullmatch(r"[a-f0-9]{64}", manifest_hash)
                    or not manifest_sig):
                return json_response({
                    "error": "private_mirror_encryption_required",
                    "required": (
                        "owner-sealed-v1 encrypted archive, opaque repository "
                        "id, key epoch, and owner-signed manifest hash"
                    ),
                }, status=426)
            manifest_canonical = (
                "forkmesh-private-manifest-v1\n" + owner + "\n"
                + record["name"] + "\n" + opaque_repo_id + "\n"
                + str(key_epoch) + "\n" + manifest_hash + "\n"
                + record["updatedAt"]
            ).encode()
            if not await ed25519_verify(
                    owner_pub, manifest_sig, manifest_canonical):
                return json_response(
                    {"error": "bad_private_manifest_signature"}, status=401)
        owner_bi = await blind_index(env, owner)
        try:
            await touch_registered_node(env, owner_bi, publish_owner_rec)
        except Exception:
            pass

        # Repo-state attestation: the same owner key signs the fingerprint of the
        # refs it serves, so the relay can later reject a tampered/stale mirror.
        # Reject the whole write if a present attestation doesn't verify (a bad
        # one must never be pinned); absent is allowed for backward compatibility.
        state_hash = record.get("stateHash", "")
        state_sig = record.get("stateSig", "")
        if state_hash or state_sig:
            state_canonical = (
                "forkmesh-repostate-v1\n" + owner + "\n" + record["name"] +
                "\n" + state_hash + "\n" + record["updatedAt"]
            ).encode()
            if not (state_hash and state_sig and await ed25519_verify(
                    owner_pub, state_sig, state_canonical)):
                return json_response({"error": "bad_state_signature"}, status=401)

        # Anti-spam: throttle writes per owner and cap how many repos one owner may
        # publish, so a single key can't flood the catalog.
        limited = await catalog_rate_check(env, owner_bi)
        if limited is not None:
            return limited

        key_bi = await blind_index(env, owner + "/" + record["name"])
        # The owner deleted this repository from the website, but the node that
        # hosts it still republishes on every heartbeat. Refuse those automatic
        # publishes so the delete sticks (adhoc #91). The signature above has
        # already proven this is the owner's key, so an explicit user-initiated
        # publish from the desktop is allowed to lift the tombstone.
        if await _repo_delete_tombstone_active(env, key_bi):
            if clean_string(data.get("publishIntent", ""), 16) != "user":
                return json_response(
                    {"error": "repository_deleted"}, status=410)
            await _clear_repo_delete_tombstone(env, key_bi)
        # Per-owner record cap (an update to an existing repo is always allowed).
        prior_row = await d1_first(
            env, "SELECT data FROM repositories WHERE key_bi=?", key_bi)
        exists = prior_row is not None
        prior_commit = ""
        prior_state_hash = ""
        prior = {}
        # Reject rollbacks: a replayed older record must not be able to repin an
        # earlier (validly-signed) repo state and downgrade the served refs.
        if exists:
            prior = await decrypt_row(env, prior_row["data"]) or {}
            prior_commit = clean_string(prior.get("commit", ""), 64)
            candidate_prior_state = clean_string(
                prior.get("stateHash", ""), 64).strip().lower()
            if (
                    (prior.get("source") or "local-node") == "local-node"
                    and re.fullmatch(
                        r"[0-9a-f]{64}", candidate_prior_state)):
                prior_state_hash = candidate_prior_state
            try:
                if int(record["updatedAt"]) < int(prior.get("updatedAt", 0) or 0):
                    return json_response({"error": "stale_update"}, status=409)
            except (TypeError, ValueError):
                pass
        # Device-scoped source authority. Account keys intentionally authorize
        # every device belonging to that account, but that must not mean a clone
        # on a second computer can silently replace the original working-copy
        # holder. Existing repositories acquire their authority lazily from the
        # stored record's nodeId; truly legacy rows without one let the first
        # modern, signed publisher claim it. There is deliberately no implicit
        # transfer path: losing a device cannot turn a random checkout into the
        # source of truth.
        incoming_node_id = clean_string(record.get("nodeId", ""), 64)
        incoming_machine = clean_string(
            record.get("machineName", ""), MAX_NODE_NAME)
        authority = await d1_first(
            env,
            "SELECT node_id,machine_name,is_private,data "
            "FROM repo_source_authorities WHERE repo_bi=?",
            key_bi,
        )
        stored_authority_node_id = clean_string(
            (authority or {}).get("node_id", ""), 64)
        if authority and authority.get("data"):
            protected_authority = await decrypt_row(
                env, authority.get("data")) or {}
            stored_authority_node_id = clean_string(
                protected_authority.get("nodeId", ""), 64)
        prior_node_id = (
            clean_string(prior.get("nodeId", ""), 64) if exists else "")
        authority_node_id, secondary_device = _repo_publication_authority(
            stored_authority_node_id, prior_node_id, incoming_node_id)
        if authority_node_id:
            now_ms = int(Date.now())
            authority_machine = (
                clean_string(prior.get("machineName", ""), MAX_NODE_NAME)
                if exists and authority_node_id != incoming_node_id
                else incoming_machine)
            private_authority = record.get("visibility") == "private"
            authority_data = (
                await encrypt_row(env, {
                    "nodeId": authority_node_id,
                    "machineName": authority_machine,
                }) if private_authority else "")
            await d1_run(
                env,
                "INSERT INTO repo_source_authorities "
                "(repo_bi,node_id,machine_name,created_at,updated_at,"
                "is_private,data) VALUES (?,?,?,?,?,?,?) "
                "ON CONFLICT(repo_bi) DO UPDATE SET "
                "node_id=excluded.node_id,machine_name=excluded.machine_name,"
                "updated_at=excluded.updated_at,is_private=excluded.is_private,"
                "data=excluded.data",
                key_bi, "" if private_authority else authority_node_id,
                "" if private_authority else authority_machine,
                now_ms, now_ms, 1 if private_authority else 0,
                authority_data,
            )
        if (
                exists
                and incoming_node_id
                and authority_node_id
                and secondary_device):
            mirror_record = dict(record)
            # `reportedSource` preserves what the signed client asserted. The
            # effective source is relay-derived from the durable authority row;
            # consumers must never grant this device source pin privileges.
            mirror_record["reportedSource"] = mirror_record.get("source")
            mirror_record["source"] = "remote-clone"
            mirror_record["sourceAuthority"] = False
            mirror_record["canonicalNodeId"] = authority_node_id
            device_storage_id = incoming_node_id
            if mirror_record.get("visibility") == "private":
                # Device identity is routing metadata for public mirrors, but a
                # private device/repository relationship is protected search
                # metadata. Preserve equality without exposing the device id.
                device_storage_id = await blind_index(
                    env, "private-repo-device:" + incoming_node_id)
            prior_device_row = await d1_first(
                env,
                "SELECT data FROM repo_device_mirrors "
                "WHERE repo_bi=? AND node_id=?",
                key_bi, device_storage_id,
            )
            prior_device = (
                await decrypt_row(env, prior_device_row.get("data", ""))
                if prior_device_row else None
            ) or {}
            device_meaningful = (
                not prior_device
                or catalog_meaningful_change(prior_device, mirror_record))
            if not device_meaningful:
                # Same heartbeat-lease rule as the canonical record below:
                # an unchanged device republish renews its lease at most once
                # per window and never evicts the shared catalog cache.
                device_lease_age = (
                    _catalog_ms(mirror_record.get("lastSync"))
                    - _catalog_ms(prior_device.get("lastSync")))
                if 0 <= device_lease_age < CATALOG_UNCHANGED_LEASE_MS:
                    canonical = dict(prior)
                    canonical["sourceAuthority"] = True
                    canonical["canonicalNodeId"] = authority_node_id
                    return json_response({
                        "ok": True,
                        "mirror": True,
                        "unchanged": True,
                        "canonicalNodeId": authority_node_id,
                        "repository": canonical,
                    }, status=202)
            if (
                    prior_device_row
                    and prior_device.get("visibility") != "private"
                    and mirror_record.get("visibility") == "private"):
                # This branch returns before the canonical visibility check;
                # close its independent public-device transition first.
                await invalidate_public_catalog_snapshots(
                    env, mirror_record.get("owner", ""),
                    mirror_record.get("name", ""))
            mirror_enc = await encrypt_row(env, mirror_record)
            await d1_run(
                env,
                "INSERT INTO repo_device_mirrors "
                "(repo_bi,node_id,data,updated_at) VALUES (?,?,?,?) "
                "ON CONFLICT(repo_bi,node_id) DO UPDATE SET "
                "data=excluded.data,updated_at=excluded.updated_at",
                key_bi, device_storage_id, mirror_enc, int(Date.now()),
            )
            # Endpoint health is maintained by the device's signed heartbeat;
            # unlike the canonical write below there is no authority state to
            # activate here. Evict the shared list so this mirror appears now
            # — but only when the record actually changed: a lease-renewal
            # write must not re-cool the shared catalog cache.
            if device_meaningful:
                _https_mirror_public_context_cache_clear()
                await edge_cache_delete(CATALOG_CACHE_KEY)
                await _bump_catalog_rev(env)
                await schedule_public_catalog_republish(
                    env, mirror_record.get("owner", ""),
                    mirror_record.get("name", ""))
            canonical = dict(prior)
            canonical["sourceAuthority"] = True
            canonical["canonicalNodeId"] = authority_node_id
            return json_response({
                "ok": True,
                "mirror": True,
                "canonicalNodeId": authority_node_id,
                "repository": canonical,
            }, status=202)
        first_repository = False
        if not exists:
            cnt = await d1_first(
                env, "SELECT COUNT(*) AS c FROM repositories WHERE owner_bi=?", owner_bi)
            if cnt and cnt.get("c", 0) >= CATALOG_MAX_RECORDS_PER_OWNER:
                return json_response({"error": "too_many_repos"}, status=429)
            first_repository = int((cnt or {}).get("c") or 0) == 0
        # Publishers re-post the identical record on a timer as a liveness
        # heartbeat. When nothing beyond volatile telemetry changed, renew the
        # lease without disturbing the shared caches — and skip even the D1
        # write while the stored lease is fresh. Snapshot ingestion, pin
        # history, the endpoint health challenge, cache purges, and change
        # notifications are reserved for records that actually changed.
        meaningful_change = (
            not exists or catalog_meaningful_change(prior, record))
        if not meaningful_change:
            lease_age = (
                _catalog_ms(record.get("lastSync"))
                - _catalog_ms(prior.get("lastSync")))
            if 0 <= lease_age < CATALOG_UNCHANGED_LEASE_MS:
                return json_response({
                    "ok": True,
                    "unchanged": True,
                    "repository": prior,
                })
            enc = await encrypt_row(env, record)
            await _contribution_write_catalog_state(
                env, record, owner_bi, publish_owner_rec, key_bi, enc)
            return json_response({
                "ok": True,
                "unchanged": True,
                "repository": record,
            })
        if (
            exists
            and prior.get("visibility") == "public"
            and record.get("visibility") == "private"
        ):
            # Persist the edge tombstone before the authoritative visibility
            # flip. If the main D1 snapshot table is unavailable, abort
            # rather than leave an old anonymous public row readable.
            await invalidate_public_catalog_snapshots(
                env, owner, record.get("name", ""))
        enc = await encrypt_row(env, record)
        await _contribution_write_catalog_state(
            env, record, owner_bi, publish_owner_rec, key_bi, enc)
        # Only the meaningful-change path indexes the fork edge: `forkedFrom`
        # is not a volatile field, so an unchanged heartbeat republish carries
        # the same parent the stored row was already indexed under.
        await _index_repo_fork_edge(env, record, prior, owner_bi, key_bi)
        attribution = globals().get("_attribution")
        if first_repository and attribution is not None:
            # Attribution funnel: this owner's first published repository,
            # joined through the session linked at signup. An account that
            # predates attribution simply records nothing (never raises);
            # the binding is optional like _forget_repo_closed above.
            await attribution.emit_for_user(
                env, owner_bi, "first_mirror_created",
                {"repo": str(record.get("owner", "")) + "/"
                 + str(record.get("name", ""))})
        # Hackathon builder points for the same event: publishing a mirror
        # is the thing the Preserve track exists for. One row per
        # participant per kind, so a republish cannot pay twice.
        hackathon = globals().get("_hackathon")
        if first_repository and hackathon is not None:
            try:
                await hackathon.on_first_mirror(
                    env, owner_bi,
                    str(record.get("owner", "")) + "/"
                    + str(record.get("name", "")))
            except Exception:
                pass
        # This isolate may have answered "not publicly served" for this name
        # moments ago (see _REPO_CLOSED_MEMO); the row it just wrote is the
        # authority now, so the publisher's own next read must not see it.
        forget_closed = globals().get("_forget_repo_closed")
        if callable(forget_closed):
            forget_closed(owner, record.get("name", ""))
        contribution_result = {"accepted": False, "warning": ""}
        if record["visibility"] == "public":
            try:
                contribution_result = await _contribution_ingest_snapshot(
                    env,
                    data,
                    record,
                    owner_bi,
                    publish_owner_rec,
                    key_bi,
                )
            except Exception:
                # Optional snapshot failures do not roll back an authorized
                # repository publication.
                contribution_result = {
                    "accepted": False,
                    "warning": "contribution_ingestion_failed",
                }
        # A working-copy holder's verified attestation also lands in the pin
        # history, which is what lets an honest mirror lag the source by a few
        # publishes without failing the clone integrity gate (clone_state_pins).
        # Mirrors' ("remote-clone") self-attestations are deliberately NOT
        # recorded — they must match a source pin. Best-effort: a failure here
        # must never fail the publish itself.
        if record.get("visibility") == "private":
            # A refs digest is useful for public mirror selection, but on a
            # private repository it is a stable search oracle over protected
            # commit state. Remove legacy pins on every private publication;
            # encrypted-manifest authentication is the private mirror rail.
            try:
                await d1_run(
                    env, "DELETE FROM repo_state_history WHERE key_bi=?", key_bi)
                if exists and prior.get("visibility") == "public":
                    # Device rows written while this repository was public
                    # carry a plaintext node id that cannot be blind-indexed
                    # retroactively, so the visibility flip discards them.
                    # Rows written as private are already encrypted under a
                    # blind device index; purging those on every publication
                    # would delete the owner-only failover capacity that the
                    # catalog and HTTPS routing still read.
                    await d1_run(
                        env,
                        "DELETE FROM repo_device_mirrors WHERE repo_bi=?",
                        key_bi)
            except Exception:
                pass
        elif state_hash and record.get("source") == "local-node":
            try:
                # Preserve the state being replaced explicitly. Older releases
                # wrote history best-effort after replacing the catalog row, so
                # an upgraded repository can otherwise lose its only
                # last-known-good handoff pin on the first new publication.
                # The previous record was already owner-signature-verified when
                # accepted; remote-clone self-pins never enter this path.
                history_states = [
                    digest for digest in (
                        prior_state_hash,
                        state_hash.strip().lower(),
                    )
                    if digest
                ]
                for digest in dict.fromkeys(history_states):
                    await d1_run(
                        env,
                        "INSERT INTO repo_state_history "
                        "(key_bi, state_hash, ts) VALUES (?,?,?) "
                        "ON CONFLICT(key_bi, state_hash) "
                        "DO UPDATE SET ts=excluded.ts",
                        key_bi, digest, int(Date.now()),
                    )
                await d1_run(
                    env,
                    "DELETE FROM repo_state_history WHERE key_bi=? "
                    "AND state_hash NOT IN (SELECT state_hash FROM "
                    "repo_state_history WHERE key_bi=? ORDER BY ts DESC LIMIT ?)",
                    key_bi, key_bi, STATE_PIN_HISTORY,
                )
            except Exception:
                pass
        # Cap: keep only the most-recent MAX_CATALOG_REPOS. COUNT first — the
        # old unconditional fetch-and-decrypt of EVERY catalog blob on every
        # publish was pure per-request overhead (the cap is almost never hit).
        republish_flagship = False
        # The rev-keyed count memo answers this without a table scan while
        # the catalog is unchanged; the row just written already bumped the
        # counter for it (2026-09-13: ~540 COUNT(*) scans a day from here).
        counter = globals().get("_repositories_count")
        if callable(counter):
            total_count = int(await counter(env) or 0)
        else:
            total_row = await d1_first(
                env, "SELECT COUNT(*) AS c FROM repositories")
            total_count = int((total_row or {}).get("c", 0) or 0)
        if total_count > MAX_CATALOG_REPOS:
            rows = await d1_all(env, "SELECT key_bi, data FROM repositories")
            decoded = []
            for r in rows:
                rec2 = await decrypt_row(env, r["data"])
                decoded.append((
                    r["key_bi"],
                    rec2.get("updatedAt", "") if rec2 else "",
                    rec2 or {},
                ))
            decoded.sort(key=lambda x: x[1], reverse=True)
            evicted = decoded[MAX_CATALOG_REPOS:]
            include_flagship = False
            for _, _, evicted_record in evicted:
                if await _is_flagship_snapshot_target(
                        env, evicted_record.get("owner", ""),
                        evicted_record.get("name", "")):
                    include_flagship = True
                    break
            republish_flagship = include_flagship
            await invalidate_public_catalog_snapshots(
                env, include_flagship=include_flagship)
            for stale_key, _, _ in evicted:
                await _delete_repo_scoped_state(env, stale_key)
                await d1_run(env, "DELETE FROM repositories WHERE key_bi=?", stale_key)
        # Endpoint registration deliberately resets health to pending. Once this
        # account-bound publication has durably pinned the public flagship state,
        # immediately challenge only the publishing node so clone availability
        # does not depend on a later cron tick. The helper is best-effort: an
        # unreachable or invalid proof remains fail-closed in endpoint state but
        # never rolls back an otherwise valid catalog publication.
        await _https_mirror_refresh_catalog_publisher_health(env, record)
        await purge_catalog_related_caches()
        await _bump_catalog_rev(env)
        # The anonymous snapshots this record feeds are rebuilt within seconds
        # by the republish doorbell instead of waiting for their Cron slot.
        await schedule_public_catalog_republish(
            env, owner, record.get("name", ""),
            include_flagship=republish_flagship)
        # Payload-free change advisory over the existing per-owner node event
        # socket, so the account's other connected devices refresh their
        # catalog views on push instead of a poll tick. Best-effort like every
        # notify: a relay hiccup never fails an authorized publication.
        try:
            await notify_repo_host(env, owner, record["name"], "catalog")
        except Exception:
            pass
        if (
                record["visibility"] == "public"
                and record.get("source") == "remote-clone"):
            try:
                await _promote_hosted_repository_import(env, record)
            except Exception:
                # Import metadata and its notification are useful secondary
                # effects. The signed native catalog is still authoritative
                # and must not be rolled back if this bounded linkage fails.
                pass
        # A moved default head is a visible activity event. Announce it to the
        # live World room after the cache purge above, so a viewer's immediate
        # refresh reads the new head. Best-effort and already bounded by
        # catalog_rate_check: a World relay hiccup must never fail an authorized
        # publication.
        new_commit = record.get("commit", "")
        if (record["visibility"] == "public" and exists
                and re.fullmatch(r"[0-9a-f]{40,64}", new_commit)
                and new_commit != prior_commit):
            try:
                await _world_broadcast_mirror_push(
                    env, owner, record["name"], new_commit,
                    record.get("changedFiles", []))
            except Exception:
                pass
        # Mirror convergence follows the complete signed refs state, not just
        # the default head displayed above. Push a payload-free "commits" event
        # for a changed branch or tag too, so every source push fetches now
        # instead of waiting on the dropped-event safety reconciliation. Only a
        # source publication notifies: a mirror's "remote-clone" publication
        # echoes the state after its fetch and must not re-wake the fleet.
        if catalog_mirror_update_needed(
                record, exists, prior_commit, prior_state_hash):
            try:
                generation = _catalog_ms(record.get("lastSync"))
                scheduled = await schedule_repo_mirror_wake(
                    env, owner, record["name"], "commits", state_hash,
                    generation)
                if not scheduled:
                    await notify_repo_mirrors(
                        env, owner, record["name"], "commits", state_hash,
                        generation)
            except Exception:
                pass
        # Outbound webhooks (adhoc #207) and activity pings (adhoc #295). An
        # owner-signed publication whose refs digest moved IS the push event
        # as the relay can observe it: there is no receive-pack here, the node
        # publishes the state it now advertises. Both rails read that one
        # observation; only the webhook rail is public-only, because a private
        # push is still a push its owner wants to hear about.
        if catalog_push_observed(
                record, exists, prior_commit, prior_state_hash):
            # `commit` is the tip of `branch` (the node's published default),
            # so the two move together and both rails below can name the
            # branch that actually advanced.
            branch = clean_string(record.get("branch", ""), 120) or "main"
            head_moved = (
                bool(re.fullmatch(r"[0-9a-f]{40,64}", new_commit))
                and new_commit != str(prior_commit or "").strip().lower())
            # "repository.push" covers any moved ref; "branch.push"
            # additionally fires when the default branch tip moved, which is
            # the trigger a CI provider actually builds on. Queued, never
            # delivered inline, so an unreachable endpoint cannot slow a push
            # down.
            if record["visibility"] == "public":
                await queue_repo_webhook_event(
                    env, owner, record["name"], "repository.push",
                    {
                        "defaultBranch": branch,
                        "before": str(prior_commit or ""),
                        "after": new_commit,
                        "refsDigest": state_hash,
                        "defaultBranchMoved": head_moved,
                    },
                    state_hash,
                    occurred_at=_catalog_ms(record.get("lastSync")))
                if head_moved:
                    await queue_repo_webhook_event(
                        env, owner, record["name"], "branch.push",
                        {
                            "ref": "refs/heads/" + branch,
                            "branch": branch,
                            "before": str(prior_commit or ""),
                            "after": new_commit,
                            "refsDigest": state_hash,
                        },
                        new_commit,
                        occurred_at=_catalog_ms(record.get("lastSync")))
            # The ping rail: the endpoints an owner registered are not the
            # only thing that wants to know a push landed. One ping per
            # publication — a moved default head is reported as the branch
            # push it is, anything else as the ref update it is — so a single
            # push never banners a fleet of devices twice. Best effort: an
            # authorized publication is never rolled back because a ping could
            # not be filed.
            try:
                await notify_code_push(
                    env, owner, record["name"],
                    branch=branch if head_moved else "",
                    commit=new_commit if head_moved else "",
                    commits=(catalog_push_commit_delta(record, prior)
                             if head_moved else 0),
                    subject=(record.get("commitSubject", "")
                             if head_moved else ""),
                    # The publishing machine, else the account: a node
                    # publishes the state it advertises and names no pusher.
                    pusher=(record.get("machineName", "") or owner),
                    key=new_commit if head_moved else state_hash,
                    refs_digest=state_hash)
            except Exception:
                pass
        payload = {
            "ok": True,
            "repository": record,
            "contributionsAccepted": bool(contribution_result["accepted"]),
        }
        if contribution_result["warning"]:
            payload["contributionWarning"] = contribution_result["warning"][:80]
        return json_response(payload, status=201)

    if method == "DELETE":
        params = parse_qs(urlparse(request.url).query)
        owner = safe_segment(params.get("owner", [""])[0])
        name = safe_segment(params.get("name", [""])[0])
        ts = clean_string(params.get("ts", [""])[0], 20)
        sig = clean_string(params.get("sig", [""])[0], 200)
        admin_user = clean_string(
            params.get("user", [""])[0], MAX_NODE_NAME).strip().lower()
        if not owner or not name:
            return json_response({"error": "owner_name_required"}, status=400)
        owner = await _org_repo_node(env, owner, name) or owner
        audit_target = owner + "/" + name

        # The desktop still signs the catalog-delete canonical with the
        # repository owner's Ed25519 key. The web Settings tab has no access to
        # that private key, so it instead proves the logged-in account session
        # and the same account->node ownership relationship used by About
        # editing. A valid session for some other account is never a substitute
        # for the signature.
        session_actor = await _authed_account_name(env, request)
        session_authorized = bool(
            session_actor
            and await _repo_admin_allowed(env, session_actor, owner, name)
        )
        # An administrator can purge any repository from any device, including
        # one whose files that device never had: the desktop's Delete button on
        # the Network Repos catalog signs this instead of graying itself out
        # (adhoc #196). The signature covers an admin-specific canonical that
        # names both the acting administrator and the target, so an owner's
        # delete signature can never be replayed as an administrator's.
        admin_authorized = False
        if not session_authorized and admin_user:
            if not ts or not sig:
                return json_response(
                    {"error": "owner_name_ts_sig_required"}, status=400)
            admin_canonical = (
                "forkmesh-admin-repo-delete-v1\n" + admin_user + "\n" + owner +
                "\n" + name + "\n" + ts
            ).encode()
            if not await _admin_authorized(
                    env, admin_user, ts, sig, admin_canonical):
                await _audit_sensitive_action(
                    env, admin_user, "repository.delete", "repository",
                    audit_target, "denied", {"reason": "not_admin"})
                return json_response({"error": "unauthorized"}, status=401)
            admin_authorized = True
        # Whoever the audit trail has to name for everything below.
        audit_actor = (
            admin_user if admin_authorized
            else session_actor if session_authorized
            else owner)
        if not session_authorized and not admin_authorized:
            if session_actor and (not ts or not sig):
                await _audit_sensitive_action(
                    env, session_actor, "repository.delete", "repository",
                    audit_target, "denied", {"reason": "not_authorized"})
                return json_response(
                    {"error": "not_authorized"}, status=403)
            if not ts or not sig:
                return json_response(
                    {"error": "owner_name_ts_sig_required"}, status=400)
            try:
                skew = abs(int(Date.now()) - int(ts))
            except (TypeError, ValueError):
                skew = LOGIN_MAX_SKEW_MS + 1
            if skew > LOGIN_MAX_SKEW_MS:
                return json_response({"error": "stale_request"}, status=401)

        key_bi = await blind_index(env, owner + "/" + name)
        try:
            row = await d1_first(
                env, "SELECT data FROM repositories WHERE key_bi=?", key_bi)
        except Exception:
            await _audit_sensitive_action(
                env, audit_actor, "repository.delete", "repository",
                audit_target, "failed", {"reason": "storage_error"})
            raise
        if not row:
            # No catalog row right now, but the owner node may still be about
            # to republish one (that race is exactly what makes a web delete
            # look like it did nothing). Tombstone it anyway.
            if session_authorized or admin_authorized:
                await invalidate_public_catalog_snapshots(env, owner, name)
                await _record_repo_delete_tombstone(
                    env, key_bi, await blind_index(env, owner))
                if admin_authorized:
                    # An administrator's purge means "leave nothing", and a
                    # missing catalog row is no evidence that the scoped
                    # tables are empty — an earlier partial delete, or a
                    # repository this relay only ever saw through an inbox,
                    # leaves rows behind with no row to find them from.
                    await _delete_repo_scoped_state(env, key_bi)
                    await _delete_repo_named_state(env, owner, name)
                    await _audit_sensitive_action(
                        env, audit_actor, "repository.delete", "repository",
                        audit_target, "success",
                        {"deleted": False, "admin": True})
                # Heal the barrier right after its deadline rather than at
                # the next Cron slot (the row is served fail-closed until a
                # normal generation replaces it).
                await schedule_public_catalog_republish(env, owner, name)
            return json_response({"ok": True, "deleted": False})
        try:
            existing = await decrypt_row(env, row["data"])
        except Exception:
            await _audit_sensitive_action(
                env, audit_actor, "repository.delete", "repository",
                audit_target, "failed", {"reason": "record_unreadable"})
            raise
        if not existing:
            await _audit_sensitive_action(
                env, audit_actor, "repository.delete", "repository",
                audit_target, "failed", {"reason": "record_unreadable"})
            return json_response({"error": "catalog_record_unreadable"}, status=500)
        if not session_authorized and not admin_authorized:
            try:
                owner_pub = await _owner_pubkey(env, owner)
            except Exception:
                await _audit_sensitive_action(
                    env, audit_actor, "repository.delete", "repository",
                    audit_target, "failed", {"reason": "identity_lookup_error"})
                raise
            if not owner_pub:
                await _audit_sensitive_action(
                    env, audit_actor, "repository.delete", "repository",
                    audit_target, "denied", {"reason": "account_required"})
                return json_response(
                    {"error": "account_required"}, status=403)
            canonical = (
                "forkmesh-catalog-delete-v1\n" + owner + "\n" + name +
                "\n" + ts
            ).encode()
            try:
                authorized = await _verify_owner_signature(
                    env, owner, sig, canonical)
            except Exception:
                await _audit_sensitive_action(
                    env, audit_actor, "repository.delete", "repository",
                    audit_target, "failed",
                    {"reason": "signature_check_error"})
                raise
            if not authorized:
                await _audit_sensitive_action(
                    env, audit_actor, "repository.delete", "repository",
                    audit_target, "denied", {"reason": "bad_signature"})
                return json_response({"error": "bad_signature"}, status=401)
        # Deleting a single repo must purge it as completely as the
        # whole-account teardown does, or leftover scoped state (inboxes,
        # host presence, agent state, bounties, chat history) keeps the repo
        # alive in practice even though its catalog row is gone.
        try:
            await invalidate_public_catalog_snapshots(env, owner, name)
            # A delete made from the website has to outlive the owner node's
            # next heartbeat: that node still holds the mirror and republishes
            # automatically, which used to re-create the row within a minute
            # (adhoc #91). The desktop's own owner-signed delete needs no
            # tombstone — it is the publisher, and it also uses this endpoint
            # to move a repo to a renamed owner. An administrator's purge does
            # need one: the owner's node is still running and would republish
            # the repository within the minute.
            if session_authorized or admin_authorized:
                await _record_repo_delete_tombstone(
                    env, key_bi, await blind_index(env, owner))
            await _delete_repo_scoped_state(env, key_bi)
            await _delete_repo_named_state(env, owner, name)
            await d1_run(env, "DELETE FROM repositories WHERE key_bi=?", key_bi)
            await _delete_bounties_namespace(env, owner, name)
            await d1_run(
                env, "DELETE FROM chat_history WHERE room_key LIKE ?",
                "repo:" + owner + "/" + name + ":room:%")
            await d1_run(
                env,
                "DELETE FROM funds_received WHERE scope='project' AND key=?",
                owner + "/" + name)
            await purge_catalog_related_caches()
            await _bump_catalog_rev(env)
            await schedule_public_catalog_republish(env, owner, name)
        except Exception:
            await _audit_sensitive_action(
                env, audit_actor, "repository.delete", "repository",
                audit_target, "failed", {"reason": "mutation_error"})
            raise
        await _audit_sensitive_action(
            env, audit_actor, "repository.delete", "repository",
            audit_target, "success",
            {"deleted": True, "admin": True} if admin_authorized
            else {"deleted": True})
        return json_response({"ok": True, "deleted": True})

    return json_response({"error": "method_not_allowed"}, status=405)


async def repository_index_search_handler(env, request):
    """GET /api/repositories/search: the searchable public index (#478).

    Reads the validated anonymous catalog snapshot (one indexed D1 row) and
    ranks it with the pure repository_index module. The response is public
    catalog data only, so it never accepts a viewer token and is cacheable.
    When no current snapshot exists it falls back to the anonymous catalog
    handler, whose own edge cache bounds that cost.
    """
    if method_name(request) not in ("GET", "HEAD"):
        return json_response({"error": "method_not_allowed"}, status=405)
    import repository_index

    url = urlparse(str(getattr(request, "url", "") or ""))
    params = parse_qs(url.query)
    now = int(Date.now())
    path = "/api/repositories"
    payload = None
    generated_at = 0
    source = "snapshot"
    try:
        row = await _public_snapshot_row(env, path)
        payload = _validated_public_snapshot_row(
            row, path, now,
            str(getattr(env, "DEPLOY_FINGERPRINT", "") or ""),
            allow_authoritative_fallback=True)
        if payload is not None:
            generated_at = int(row.get("generated_at") or 0)
    except Exception:
        payload = None
    if payload is None:
        source = "live-catalog"
        live = await catalog_handler(env, JsRequest.new(
            "https://app.forkmesh.internal" + path,
            to_js({"method": "GET",
                   "headers": {"accept": "application/json"}}),
        ))
        payload = await _response_json(live)
        if payload.get("ok") is not True:
            return json_response(
                {"error": "catalog_unavailable"}, status=503,
                cache_control="no-store, max-age=0, must-revalidate")
        # The anonymous catalog edge entry is at most CATALOG_TTL old.
        generated_at = now - CATALOG_TTL * 1000
    origin = ""
    if url.scheme and url.netloc:
        origin = url.scheme + "://" + url.netloc
    try:
        result = repository_index.search(
            payload, params, now,
            origin=repository_index.public_origin(origin),
            snapshot_generated_at=generated_at)
    except repository_index.IndexQueryError as error:
        return json_response({"error": error.code}, status=400)
    return json_response(
        result, cache_seconds=CATALOG_TTL,
        extra_headers={"x-forkmesh-index-source": source})
