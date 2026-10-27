"""fediverse routes routes, loaded on demand.

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


def _ap_origin(env, request=None):
    # Absolute https origin for actor/object ids. PUBLIC_BASE_URL wins (set on
    # self-hosted relays); otherwise the request's own host; cron falls back to
    # the canonical production domain.
    base = (getattr(env, "PUBLIC_BASE_URL", "") or "").strip().rstrip("/")
    if base.startswith("https://") or base.startswith("http://"):
        return base
    if request is not None:
        try:
            host = urlparse(request.url).netloc
            if host:
                # The official App/WWW/World workers are access layers for one
                # public fediverse identity. Never mint actor, object, profile,
                # WebFinger, or rel=me URLs on a service subdomain.
                hostname = host.split(":", 1)[0].lower().rstrip(".")
                if hostname in (
                        "app.forkmesh.com", "forkmesh.com",
                        "world.forkmesh.com"):
                    return "https://forkmesh.com"
                return "https://" + host
        except Exception:
            pass
    return "https://forkmesh.com"
def _ap_domain_of(origin):
    return origin.split("://", 1)[-1]
async def _ap_settings(env, fresh=False):
    now = int(Date.now())
    if not fresh and now - _AP_SETTINGS_CACHE["ts"] < AP_SETTINGS_TTL_MS:
        return _AP_SETTINGS_CACHE
    row = await d1_first(env, "SELECT v FROM ap_settings WHERE k='enabled'")
    enabled = (row or {}).get("v", "1") != "0"
    rows = await d1_all(env, "SELECT domain FROM ap_blocked_domains")
    blocked = {r.get("domain", "") for r in (rows or []) if r.get("domain")}
    _AP_SETTINGS_CACHE.update({"ts": now, "enabled": enabled,
                               "blocked": blocked})
    return _AP_SETTINGS_CACHE
async def _ap_enabled(env):
    return (await _ap_settings(env))["enabled"]
def _ap_outbound_enabled(env):
    # Outbound federation (signed deliveries and signed actor fetches). Off by
    # default on a VPS host (ForkMesh Next): its imported actors and keys are
    # production's, so it must never sign or deliver as them. Var AP_OUTBOUND
    # (FORKMESH_VAR_AP_OUTBOUND=1) turns it on explicitly.
    raw = str(getattr(env, "AP_OUTBOUND", "") or "").strip().lower()
    if raw in ("1", "true", "on", "yes"):
        return True
    if raw in ("0", "false", "off", "no"):
        return False
    platform = str(getattr(env, "FORKMESH_PLATFORM", "") or "")
    return platform.strip().lower() != "vps"
async def _ap_domain_blocked(env, host):
    return ap.domain_blocked_by(host, (await _ap_settings(env))["blocked"])
def _ap_disabled_response():
    return json_response({"error": "not_found"}, status=404)
def _ap_actor_url(origin, kind, handle):
    if kind == AP_ACTOR_INSTANCE:
        return origin + ap.INSTANCE_ACTOR_PATH
    if kind == AP_ACTOR_USER:
        return origin + ap.user_actor_path(handle)
    owner, _, repo = handle.partition(".")
    return origin + ap.repo_actor_path(owner, repo)
def _ap_uuid():
    raw = js_crypto.getRandomValues(Uint8Array.new(16))
    return bytes(raw.to_py()).hex()
def _ap_js_bytes(value):
    # WebCrypto may retain BufferSource arguments after Python releases the
    # GIL. Uint8Array construction copies the WASM view into JS-owned memory.
    return Uint8Array.new(_to_js(bytes(value)))
async def _ap_generate_keypair():
    params = to_js({"name": "RSASSA-PKCS1-v1_5", "modulusLength": 2048,
                    "hash": "SHA-256"})
    params.publicExponent = _ap_js_bytes(bytes([1, 0, 1]))  # 65537
    pair = await js_crypto.subtle.generateKey(
        params, True, _to_js(["sign", "verify"]))
    spki = await js_crypto.subtle.exportKey("spki", pair.publicKey)
    pkcs8 = await js_crypto.subtle.exportKey("pkcs8", pair.privateKey)
    pub_pem = ap.pem_wrap(
        "PUBLIC KEY",
        base64.b64encode(bytes(Uint8Array.new(spki).to_py())).decode())
    priv_b64 = base64.b64encode(bytes(Uint8Array.new(pkcs8).to_py())).decode()
    return pub_pem, priv_b64
async def _ap_rsa_sign_b64(priv_b64, text):
    key = await js_crypto.subtle.importKey(
        "pkcs8", _ap_js_bytes(base64.b64decode(priv_b64)), to_js(_AP_RSA_ALG),
        False, _to_js(["sign"]))
    sig = await js_crypto.subtle.sign(
        to_js({"name": "RSASSA-PKCS1-v1_5"}), key,
        _ap_js_bytes(text.encode()))
    return base64.b64encode(bytes(Uint8Array.new(sig).to_py())).decode()
async def _ap_rsa_verify(pub_pem, sig_b64, text):
    try:
        der = base64.b64decode(ap.pem_body(pub_pem))
        signature = base64.b64decode(sig_b64)
    except Exception:
        return False
    try:
        key = await js_crypto.subtle.importKey(
            "spki", _ap_js_bytes(der), to_js(_AP_RSA_ALG), False,
            _to_js(["verify"]))
        ok = await js_crypto.subtle.verify(
            to_js({"name": "RSASSA-PKCS1-v1_5"}), key,
            _ap_js_bytes(signature), _ap_js_bytes(text.encode()))
        return bool(ok)
    except Exception:
        return False
async def _ap_actor_bi(env, kind, handle):
    return await blind_index(env, "ap-actor:%s:%s" % (kind, handle))
async def _ap_local_actor(env, kind, handle, create=False):
    # A local actor's keypair, minted lazily on the first fediverse lookup so
    # users/repos nobody follows never get a row.
    actor_bi = await _ap_actor_bi(env, kind, handle)
    row = await d1_first(
        env, "SELECT data FROM ap_actors WHERE actor_bi=?", actor_bi)
    if row:
        rec = await decrypt_row(env, row.get("data"))
        if rec and rec.get("privkey") and rec.get("pubkeyPem"):
            rec["actorBi"] = actor_bi
            return rec
    if not create:
        return None
    pub_pem, priv_b64 = await _ap_generate_keypair()
    rec = {"kind": kind, "handle": handle, "pubkeyPem": pub_pem,
           "privkey": priv_b64, "createdAt": int(Date.now())}
    await d1_run(
        env,
        "INSERT INTO ap_actors (actor_bi, kind, pubkey_pem, data, created_at)"
        " VALUES (?,?,?,?,?) ON CONFLICT(actor_bi) DO NOTHING",
        actor_bi, kind, pub_pem, await encrypt_row(env, rec),
        rec["createdAt"])
    # Re-read: a concurrent first-lookup may have won the insert race with a
    # different keypair, and the stored row is the canonical one (the pubkey
    # is already being served to the remote server that triggered it).
    row = await d1_first(
        env, "SELECT data FROM ap_actors WHERE actor_bi=?", actor_bi)
    stored = await decrypt_row(env, row.get("data")) if row else None
    rec = stored or rec
    rec["actorBi"] = actor_bi
    return rec
async def _ap_instance_actor(env, create=False):
    row = await d1_first(
        env, "SELECT data FROM ap_service_keys WHERE key_name='instance'")
    if row:
        rec = await decrypt_row(env, row.get("data"))
        if rec and rec.get("privkey") and rec.get("pubkeyPem"):
            return rec
    if not create:
        return None
    pub_pem, priv_b64 = await _ap_generate_keypair()
    rec = {
        "kind": "service",
        "handle": AP_INSTANCE_HANDLE,
        "pubkeyPem": pub_pem,
        "privkey": priv_b64,
        "createdAt": int(Date.now()),
    }
    await d1_run(
        env,
        "INSERT INTO ap_service_keys (key_name, pubkey_pem, data, created_at) "
        "VALUES ('instance',?,?,?) ON CONFLICT(key_name) DO NOTHING",
        pub_pem, await encrypt_row(env, rec), rec["createdAt"])
    row = await d1_first(
        env, "SELECT data FROM ap_service_keys WHERE key_name='instance'")
    return await decrypt_row(env, row.get("data")) if row else rec
async def _ap_user_federates(env, name):
    # Any active, non-private account federates as a Person — node-owner
    # accounts included, not just login ("user"-kind) accounts: the node name
    # is the public authoring identity on issues/PRs, so it is what fediverse
    # followers expect to find at @name@<domain>. The web follow API
    # (_account_follow) accepts the same targets.
    name = (name or "").strip().lower()
    if not valid_node_name(name):
        return False
    _, rec = await _account_row(env, name)
    return bool(rec and rec.get("status") == "active"
                and not rec.get("profile_private"))
async def _ap_org_alias_owner(env, owner, repo):
    # Organization repo aliases (issue #388): a fediverse actor is addressed by
    # the org handle (@org.repo@host), but the backing repo — its repositories
    # row, media, per-repo AP settings and issue inbox — lives under the linked
    # node, never under the org name. Resolve org->node for those data reads so
    # federation keeps working after a repo is fronted by an org, while the
    # public actor identity (handle, reply author, web URLs) stays the org. A
    # plain node URL (no alias) resolves to itself, so this is a safe no-op on
    # every non-org path.
    node = await _org_repo_node(env, owner, repo)
    return node or owner
async def _ap_repo_is_official_actor(env, owner, repo):
    # Mirror catalog rows are routing replicas, never their own public social
    # identities. Official repositories are locally published records or an
    # organization alias backed by one; remote/external clone records are not.
    # Do not call _ap_org_alias_owner/_org_repo_node here. Actor inventory
    # reconciliation runs from ensure_schema(), while _org_repo_node itself
    # calls ensure_schema(); that recursion makes alias resolution fail closed
    # during a cold start and used to delete valid org actors plus every
    # follower row. Read the already-created durable link directly instead.
    owner_l = str(owner or "").strip().lower()
    repo_l = str(repo or "").strip().lower()
    org_bi = await blind_index(env, "org:" + owner_l)
    alias_row = await d1_first(
        env,
        "SELECT node_owner FROM org_repos WHERE org_bi=? AND repo=?",
        org_bi, repo_l)
    alias_owner = str(
        (alias_row or {}).get("node_owner") or "").strip().lower()
    data_owner = (
        alias_owner
        if valid_node_name(alias_owner) and alias_owner != owner_l
        else owner_l
    )
    is_org_alias = (
        data_owner != owner_l
    )
    key_bi = await blind_index(env, data_owner + "/" + repo_l)
    row = await d1_first(
        env, "SELECT data FROM repositories WHERE key_bi=?", key_bi)
    if not row:
        return False
    rec = await decrypt_row(env, row.get("data"))
    # A durable org link promotes its backing repository to the org's one
    # official public identity even when the backing catalog row is tagged as
    # a remote clone. That tag describes the hosting node's copy; it must not
    # cause startup reconciliation to delete the org actor and its followers.
    if is_org_alias:
        return _catalog_record_matches_identity(rec, data_owner, repo_l)
    source = str((rec or {}).get("source") or "").strip().lower()
    return source not in ("remote-clone", "external")
async def _ap_prune_actor_inventory(env):
    # Actor visibility is enforced by the public resolution/federation gates,
    # not by erasing the durable social graph during schema startup. Catalog
    # records and org aliases can be temporarily unavailable or misclassified;
    # deleting here previously turned that transient state into permanent loss
    # of followers, actors, signing identities, and posts. Keep this hook for
    # callers from older deployments, but make reconciliation non-destructive.
    return
async def _ap_repo_federates(env, owner, repo):
    # Only published, public repos federate. A missing catalog row means the
    # repo was never published — unlike the git-clone path (which stays open
    # for ad-hoc hosts), an unpublished repo has no fediverse presence. The
    # owner can also switch federation off per repo (web/Qt repo settings).
    data_owner = await _ap_org_alias_owner(env, owner, repo)
    official_reader = globals().get("_ap_repo_is_official_actor")
    if callable(official_reader) and not await official_reader(
            env, owner, repo):
        return False
    privacy_reader = globals().get("_repo_is_private")
    if callable(privacy_reader):
        if await privacy_reader(env, data_owner, repo):
            return False
    else:
        # Isolated compatibility harnesses may omit _repo_is_private.
        # Production always uses its signed-record, fail-closed decision.
        key_bi = await blind_index(env, data_owner + "/" + repo)
        row = await d1_first(
            env, "SELECT is_private FROM repositories WHERE key_bi=?", key_bi)
        if not row or int(row.get("is_private") or 0):
            return False
    return (await _ap_repo_settings_get(env, data_owner, repo))["federate"]
async def _ap_repo_settings_bi(env, owner, repo):
    # Distinct namespace + full lowercasing on both halves: callers reach this
    # with URL-cased (About handler) and handle-cased (AP gates) names, and
    # they must all land on the same row. Org-alias handles resolve to the
    # backing node so a mention gate keys on the same row the owner's repo
    # settings wrote under the node name (issue #388).
    owner = await _ap_org_alias_owner(env, owner, repo)
    return await blind_index(
        env, "ap-repo-settings:%s/%s" % (str(owner or "").strip().lower(),
                                         str(repo or "").strip().lower()))
async def _ap_repo_settings_get(env, owner, repo):
    row = await d1_first(
        env, "SELECT data FROM ap_repo_settings WHERE repo_bi=?",
        await _ap_repo_settings_bi(env, owner, repo))
    settings = dict(AP_REPO_SETTING_DEFAULTS)
    if row:
        try:
            stored = json.loads(row.get("data") or "{}")
        except Exception:
            stored = {}
        if isinstance(stored, dict):
            for key in AP_REPO_SETTING_DEFAULTS:
                if key in stored:
                    settings[key] = bool(stored[key])
    return settings
async def _ap_org_digest_settings_get(env, org_bi):
    """Return the org-alias digest gate.

    Organization controls are deliberately a second, suppress-only gate. The
    linked repository owner's `federate` and `broadcastEvents` settings must
    also be enabled before any org-alias event can enter a queue.
    """
    row = await d1_first(
        env, "SELECT data FROM ap_org_digest_settings WHERE org_bi=?", org_bi)
    controls = fedi_digest.normalize_controls({})
    if row:
        try:
            stored = json.loads(row.get("data") or "{}")
        except Exception:
            stored = {}
        if isinstance(stored, dict):
            controls = fedi_digest.normalize_controls(stored)
    return controls
async def _ap_digest_scope_bi(env, scope):
    return await blind_index(
        env, "ap-digest:" + str(scope or "").strip().lower())
async def _ap_digest_queue_record(env, scope):
    scope_bi = await _ap_digest_scope_bi(env, scope)
    row = await d1_first(
        env,
        "SELECT scope_bi, next_ts, last_published_at, data "
        "FROM ap_digest_queues WHERE scope_bi=?",
        scope_bi)
    if not row:
        return scope_bi, None, None
    rec = await decrypt_row(env, row.get("data")) or {}
    if not isinstance(rec, dict) or rec.get("scope") != scope:
        # Fail closed if the encrypted payload does not match its blind key.
        return scope_bi, row, None
    return scope_bi, row, rec
async def _ap_enqueue_digest_scope(
        env, scope_owner, repo, source_owner, event):
    """Insert one already-reduced public event into an encrypted scope queue."""
    scope_owner = clean_string(scope_owner, MAX_NODE_NAME).strip().lower()
    source_owner = clean_string(source_owner, MAX_NODE_NAME).strip().lower()
    repo = clean_string(repo, MAX_REPO_SEGMENT).strip()
    scope = scope_owner + "/" + repo.lower()
    if (not valid_node_name(scope_owner) or not valid_node_name(source_owner)
            or not repo or not event):
        return False
    scope_bi, row, rec = await _ap_digest_queue_record(env, scope)
    now = int(Date.now())
    last_published = int((row or {}).get("last_published_at") or 0)
    events = fedi_digest.add_event(
        (rec or {}).get("events") if rec else [], event)
    if not events:
        return False
    # A new queue waits a full cadence so several meaningful events can be
    # combined. After a prior post, the next due time remains anchored to that
    # post rather than sliding forward with every incoming event.
    if row:
        next_ts = int(row.get("next_ts") or 0)
        if next_ts <= 0:
            next_ts = max(now, last_published + fedi_digest.DAY_MS)
    else:
        next_ts = now + fedi_digest.DAY_MS
    payload = {
        "scope": scope,
        "scopeOwner": scope_owner,
        "repo": repo,
        "sourceOwner": source_owner,
        "events": events,
        "lastPublishedAt": last_published,
    }
    await d1_run(
        env,
        "INSERT INTO ap_digest_queues "
        "(scope_bi, next_ts, last_published_at, data, updated_at) "
        "VALUES (?,?,?,?,?) ON CONFLICT(scope_bi) DO UPDATE SET "
        "next_ts=excluded.next_ts, data=excluded.data, "
        "updated_at=excluded.updated_at",
        scope_bi, next_ts, last_published,
        await encrypt_row(env, payload), now)
    return True
async def _ap_enqueue_repo_digest(
        env, request, owner, repo, kind, event_type, ref, title, body):
    """Queue a safe repository event for its repo and enabled org aliases."""
    if await _repo_is_private(env, owner, repo):
        return False
    settings = await _ap_repo_settings_get(env, owner, repo)
    if not (settings["federate"] and settings["broadcastEvents"]):
        return False
    origin = _ap_origin(env, request)
    timestamp = int(Date.now())
    canonical = fedi_digest.repository_event(
        origin, owner, repo, kind, event_type, ref, title, body, timestamp)
    queued = await _ap_enqueue_digest_scope(
        env, owner, repo, owner, canonical)

    # An organization alias has its own repo actor/followers and therefore its
    # own daily cadence. Org admins may suppress that alias, but cannot enable
    # it when the backing repository owner disabled federation.
    rows = await d1_all(
        env,
        "SELECT r.org_bi AS org_bi, o.name AS name, s.data AS settings "
        "FROM org_repos r JOIN orgs o ON o.org_bi=r.org_bi "
        "LEFT JOIN ap_org_digest_settings s ON s.org_bi=r.org_bi "
        "WHERE r.node_owner=? AND r.repo=? ORDER BY o.name LIMIT ?",
        str(owner).lower(), str(repo).lower(), MAX_ORGS_PER_ACCOUNT)
    for linked in rows or []:
        try:
            raw_controls = json.loads(linked.get("settings") or "{}")
        except Exception:
            raw_controls = {}
        controls = fedi_digest.normalize_controls(raw_controls)
        org = clean_string(linked.get("name"), MAX_NODE_NAME).strip().lower()
        if not controls["enabled"] or not valid_node_name(org):
            continue
        org_event = fedi_digest.repository_event(
            origin, org, repo, kind, event_type, ref, title, body, timestamp)
        queued = (await _ap_enqueue_digest_scope(
            env, org, repo, owner, org_event)) or queued
    return queued
async def _ap_purge_repo_digest_queues(env, owner, repo):
    """Drop queued automatic posts when the repository owner disables them."""
    scopes = [str(owner).lower() + "/" + str(repo).lower()]
    rows = await d1_all(
        env,
        "SELECT o.name AS name FROM org_repos r "
        "JOIN orgs o ON o.org_bi=r.org_bi "
        "WHERE r.node_owner=? AND r.repo=? LIMIT ?",
        str(owner).lower(), str(repo).lower(), MAX_ORGS_PER_ACCOUNT)
    scopes.extend(
        str(row.get("name") or "").lower() + "/" + str(repo).lower()
        for row in (rows or []) if row.get("name"))
    for scope in scopes:
        await d1_run(
            env, "DELETE FROM ap_digest_queues WHERE scope_bi=?",
            await _ap_digest_scope_bi(env, scope))
async def _ap_digest_preview(env, scope):
    _, row, rec = await _ap_digest_queue_record(env, scope)
    events = (rec or {}).get("events") if rec else []
    digest = instance_features.rename(env, fedi_digest.build_digest(events, scope))
    return {
        "scope": scope,
        "pending": len(events or []),
        "preview": digest,
        "lastPublishedAt": int((row or {}).get("last_published_at") or 0),
        "nextPublishAt": int((row or {}).get("next_ts") or 0),
    }
async def _ap_digest_scope_allowed(env, rec):
    """Revalidate privacy/ownership controls immediately before publishing."""
    scope_owner = clean_string(
        (rec or {}).get("scopeOwner"), MAX_NODE_NAME).strip().lower()
    source_owner = clean_string(
        (rec or {}).get("sourceOwner"), MAX_NODE_NAME).strip().lower()
    repo = clean_string((rec or {}).get("repo"), MAX_REPO_SEGMENT).strip()
    if not scope_owner or not source_owner or not repo:
        return False
    if await _repo_is_private(env, source_owner, repo):
        return False
    settings = await _ap_repo_settings_get(env, source_owner, repo)
    if not (settings["federate"] and settings["broadcastEvents"]):
        return False
    if scope_owner == source_owner:
        return True
    org_bi, org_row = await _org_row(env, scope_owner)
    if not org_row:
        return False
    linked = await d1_first(
        env,
        "SELECT 1 AS one FROM org_repos "
        "WHERE org_bi=? AND repo=? AND node_owner=?",
        org_bi, repo.lower(), source_owner)
    if not linked:
        return False
    return (await _ap_org_digest_settings_get(env, org_bi))["enabled"]
AP_DIGEST_FANOUT_BATCH = 10


async def _ap_publish_digest_row(env, row):
    """Durably fan out one digest in bounded, resumable follower slices."""
    scope_bi = str((row or {}).get("scope_bi") or "")
    rec = await decrypt_row(env, (row or {}).get("data")) or {}
    scope = str(rec.get("scope") or "")
    if not scope_bi or not scope or not await _ap_digest_scope_allowed(env, rec):
        if scope_bi:
            await d1_run(
                env, "DELETE FROM ap_digest_queues WHERE scope_bi=?", scope_bi)
        return False

    now = int(Date.now())
    events = rec.get("events") if isinstance(rec.get("events"), list) else []
    last_published = int(row.get("last_published_at") or 0)
    fanout = rec.get("fanout") if isinstance(rec.get("fanout"), dict) else None
    if fanout:
        event_ids = [
            str(value) for value in (fanout.get("eventIds") or [])
            if isinstance(value, str) and value
        ]
        digest = {
            "eventIds": event_ids,
            "text": str(fanout.get("text") or ""),
        }
        object_uuid = str(fanout.get("objectUuid") or "")
        published_at = int(fanout.get("publishedAt") or now)
        cursor = str(fanout.get("cursor") or "")
        if (
            not event_ids
            or not digest["text"]
            or not re.fullmatch(r"[0-9a-f]{32}", object_uuid)
        ):
            rec.pop("fanout", None)
            fanout = None
    if not fanout:
        if not fedi_digest.digest_due(events, last_published, now):
            return False
        digest = instance_features.rename(
            env, fedi_digest.build_digest(events, scope))
        if not digest:
            await d1_run(
                env, "DELETE FROM ap_digest_queues WHERE scope_bi=?", scope_bi)
            return False
        object_uuid = hashlib.sha256(
            ("forkmesh-ap-digest-v1\n" + scope + "\n"
             + "\n".join(digest["eventIds"])).encode()).hexdigest()[:32]
        published_at = now
        cursor = ""
        fanout = {
            "eventIds": list(digest["eventIds"]),
            "text": str(digest["text"]),
            "objectUuid": object_uuid,
            "publishedAt": published_at,
            "cursor": "",
        }
        rec["fanout"] = fanout

    origin = _ap_origin(env)
    scope_owner = str(rec.get("scopeOwner") or "").lower()
    repo = str(rec.get("repo") or "")
    handle = ap.repo_handle(scope_owner, repo.lower())
    local = await _ap_local_actor(env, AP_ACTOR_REPO, handle)
    if not local:
        await d1_run(
            env, "DELETE FROM ap_digest_queues WHERE scope_bi=?", scope_bi)
        return False
    followers = await d1_all(
        env,
        "SELECT follower_id,inbox,shared_inbox FROM ap_followers "
        "WHERE actor_bi=? AND follower_id>? ORDER BY follower_id "
        "LIMIT ?",
        local["actorBi"], cursor, AP_DIGEST_FANOUT_BATCH + 1)
    page = list(followers or [])
    has_more = len(page) > AP_DIGEST_FANOUT_BATCH
    page = page[:AP_DIGEST_FANOUT_BATCH]

    actor_url = _ap_actor_url(origin, AP_ACTOR_REPO, handle)
    object_url = origin + "/ap/o/" + object_uuid
    web_url = origin + repo_web_href(scope_owner, repo)
    note = ap.note_doc(
        object_url, actor_url, actor_url + "/followers",
        ap.note_html_from_text(digest["text"]), published_at, web_url=web_url)
    stored = {
        "note": note,
        "context": {
            "owner": scope_owner, "repo": repo, "kind": "daily_digest",
            "ref": object_uuid, "key": "digest:" + scope,
        },
        "media": [],
        "automated": True,
        "eventIds": list(digest["eventIds"]),
    }
    body_str = json.dumps(ap.create_activity(note))
    out_data = await encrypt_row(env, {
        "body": body_str, "actorKind": AP_ACTOR_REPO,
        "actorHandle": handle, "actorUrl": actor_url})

    statements = [(
        "INSERT OR IGNORE INTO ap_objects "
        "(object_uuid, actor_bi, context_bi, data, published) "
        "VALUES (?,?,NULL,?,?)",
        (object_uuid, local["actorBi"], await encrypt_row(env, stored),
         published_at),
    )]
    seen = set()
    for follower in page:
        inbox = ((follower.get("shared_inbox") or "").strip()
                 or (follower.get("inbox") or "").strip())
        if not inbox or inbox in seen:
            continue
        seen.add(inbox)
        delivery_bi = await blind_index(
            env, "ap-digest-delivery:" + object_uuid + ":" + inbox)
        statements.append((
            "INSERT OR IGNORE INTO ap_outbox "
            "(inbox, data, attempts, next_ts, created_at, dedupe_bi) "
            "VALUES (?,?,0,?,?,?)",
            (inbox, out_data, now, now, delivery_bi),
        ))

    if has_more:
        fanout["cursor"] = str(page[-1].get("follower_id") or cursor)
        rec["fanout"] = fanout
        next_ts = now
        published_marker = last_published
    else:
        rec["events"] = fedi_digest.remove_published(
            events, digest["eventIds"])
        rec["lastPublishedAt"] = published_at
        rec.pop("fanout", None)
        next_ts = published_at + fedi_digest.DAY_MS
        published_marker = published_at
    statements.append((
        "UPDATE ap_digest_queues SET next_ts=?, last_published_at=?, "
        "data=?, updated_at=? WHERE scope_bi=?",
        (next_ts, published_marker, await encrypt_row(env, rec), now, scope_bi),
    ))
    # Object/outbox/cursor advance are atomic. A failed slice retries without
    # losing event ids; the final slice alone consumes the published events.
    await _contribution_run_batch(env, statements)
    return True
async def _ap_negative_response(cache_key, error="not_found"):
    # Park a 404 at the edge briefly. Probes for handles that don't federate
    # (deleted accounts, crawlers, Mastodon re-resolving after a failed
    # follow) otherwise re-run the full D1 federation gate on every hit, and
    # they arrive in the same bursts the 200s do.
    resp = json_response({"error": error}, status=404, cache_seconds=120)
    await edge_cache_put(cache_key, resp)
    return resp
async def ap_hostmeta_handler(env, request):
    # Mastodon's resolver falls back to GET /.well-known/host-meta whenever a
    # webfinger lookup fails, so during any failure window each missed
    # resolution used to cost a SECOND request that walked the entire route
    # chain into the terminal JSON 404 — a self-amplifying loop. The document
    # is a constant pointer at our webfinger template: no schema, no D1, and
    # a day at the edge.
    if method_name(request) not in ("GET", "HEAD"):
        return json_response({"error": "method_not_allowed"}, status=405)
    origin = _ap_origin(env, request)
    cache_key = origin + "/.well-known/host-meta"
    cached = await edge_cache_match(cache_key)
    if cached is not None:
        return cached
    xrd = ('<?xml version="1.0" encoding="UTF-8"?>\n'
           '<XRD xmlns="http://docs.oasis-open.org/ns/xri/xrd-1.0">'
           '<Link rel="lrdd" '
           'template="%s/.well-known/webfinger?resource={uri}"/>'
           '</XRD>' % origin)
    resp = Response(xrd, status=200, headers={
        "content-type": "application/xrd+xml; charset=utf-8",
        "cache-control": "public, max-age=86400",
    })
    await edge_cache_put(cache_key, resp)
    return resp
async def ap_webfinger_handler(env, request):
    if method_name(request) != "GET":
        return json_response({"error": "method_not_allowed"}, status=405)
    params = parse_qs(urlparse(request.url).query)
    handle, domain = ap.parse_acct_resource(params.get("resource", [""])[0])
    if not handle:
        return json_response({"error": "invalid_resource"}, status=400)
    origin = _ap_origin(env, request)
    our_domain = _ap_domain_of(origin)
    if domain != our_domain:
        return json_response({"error": "not_found"}, status=404)
    # Key on the parsed acct (already lowercase-normalized), so the many
    # spellings of the same resource share one cache entry.
    cache_key = ("%s/.well-known/webfinger?resource=acct:%s@%s"
                 % (origin, handle, domain))
    cached = await edge_cache_match(cache_key)
    if cached is not None:
        try:
            if int(cached.status) >= 400:
                return cached
        except Exception:
            pass
    await ensure_schema(env)
    if not await _ap_enabled(env):
        return _ap_disabled_response()
    if handle == our_domain:
        kind, actor_handle, acct_name = (
            AP_ACTOR_INSTANCE, AP_INSTANCE_HANDLE, our_domain)
    else:
        parsed = ap.split_handle(handle)
        if not parsed:
            return await _ap_negative_response(cache_key)
        if parsed[0] == "user":
            return await _ap_negative_response(cache_key)
        else:
            if not await _ap_repo_federates(env, parsed[1], parsed[2]):
                return await _ap_negative_response(cache_key)
            kind = AP_ACTOR_REPO
            actor_handle = ap.repo_handle(parsed[1], parsed[2])
            acct_name = actor_handle
    # Resolve visibility before reading edge cache. Otherwise a public actor
    # document could remain discoverable after its account/repository is made
    # private, for the remainder of the old cache entry's TTL.
    if cached is not None:
        return cached
    actor_url = _ap_actor_url(origin, kind, actor_handle)
    doc = ap.webfinger_doc(acct_name + "@" + our_domain, actor_url)
    resp = json_response(doc, cache_seconds=3600, extra_headers={
        "content-type": ap.JRD_CONTENT_TYPE,
        "access-control-allow-origin": "*",
    })
    await edge_cache_put(cache_key, resp)
    return resp
async def ap_nodeinfo_handler(env, request, index):
    origin = _ap_origin(env, request)
    cache_key = origin + ("/.well-known/nodeinfo" if index else "/nodeinfo/2.1")
    cached = await edge_cache_match(cache_key)
    if cached is not None:
        return cached
    await ensure_schema(env)
    if not await _ap_enabled(env):
        return _ap_disabled_response()
    if index:
        resp = json_response(ap.nodeinfo_index(origin), cache_seconds=3600)
        await edge_cache_put(cache_key, resp)
        return resp
    now = int(Date.now())
    if now - _NODEINFO_CACHE["ts"] >= NODEINFO_CACHE_TTL_MS:
        users = await d1_first(env, "SELECT COUNT(*) AS c FROM users")
        posts = await d1_first(env, "SELECT COUNT(*) AS c FROM ap_objects")
        _NODEINFO_CACHE.update({
            "ts": now,
            "users": (users or {}).get("c", 0) or 0,
            "posts": (posts or {}).get("c", 0) or 0,
        })
    doc = ap.nodeinfo_doc(
        _build_rev(env)[:12], _NODEINFO_CACHE["users"],
        _NODEINFO_CACHE["posts"])
    if instance_features.branded(env):
        doc["metadata"]["nodeName"] = instance_features.display_name(env)
    resp = json_response(doc, cache_seconds=3600)
    await edge_cache_put(cache_key, resp)
    return resp
async def _ap_build_actor_doc(env, origin, kind, handle, rec):
    """The actor document, shared by the GET endpoint and the Update
    broadcast (profile/branding changes). Returns None when the actor does
    not federate (missing/private/blocked account or repo)."""
    # Brand defaults; repos with uploaded branding override below.
    icon_url = origin + AP_AVATAR_PATH
    image_url = origin + AP_BANNER_PATH
    if kind == AP_ACTOR_INSTANCE:
        doc = ap.instance_actor_doc(
            origin, _ap_domain_of(origin), rec["pubkeyPem"],
            icon_url=icon_url, image_url=image_url)
        if instance_features.branded(env) and isinstance(doc, dict):
            doc["name"] = instance_features.display_name(env)
        return doc
    if kind == AP_ACTOR_USER:
        if not await _ap_user_federates(env, handle):
            return None
        actor_type, display = "Person", handle
        profile_url = origin + "/@" + handle
        # Surface the account's own bio as the fediverse summary so the
        # Mastodon profile card mirrors the /@name page.
        _, account_rec = await _account_row(env, handle)
        bio = clean_string(
            (account_rec or {}).get("profile_bio", "") or "", 500).strip()
        summary = (ap.note_html_from_text(bio) if bio
                   else instance_features.rename(
                       env, "ForkMesh profile of @%s." % handle))
    else:
        # A repository's header is the repository's own artwork or nothing at
        # all: the brand banner above is this instance's identity, and shipping
        # it as every repo actor's `image` made unrelated projects look like
        # ForkMesh in every fediverse client.
        image_url = ""
        owner, _, repo = handle.partition(".")
        # Org handles read their repo data from the linked node (issue #388),
        # but display, profile URL and web links stay under the org name below.
        data_owner = await _ap_org_alias_owner(env, owner, repo)
        key_bi = await blind_index(env, data_owner + "/" + repo)
        repo_row = await d1_first(
            env, "SELECT is_private, data FROM repositories WHERE key_bi=?",
            key_bi)
        privacy_reader = globals().get("_repo_is_private")
        hidden = (
            await privacy_reader(env, data_owner, repo)
            if callable(privacy_reader)
            else bool(int((repo_row or {}).get("is_private") or 0))
        )
        if not repo_row or hidden:
            return None
        actor_type, display = "Group", owner + "/" + repo
        repo_web_url = origin + repo_web_href(owner, repo)
        # The actor's canonical `url` is the repo's fediverse profile page (a
        # feed of its federated posts), not the raw git-forge page: Mastodon
        # sends a user who clicks the repo handle — in a mention, or via the
        # profile's external-link — to this `url`, and dropping them onto a
        # code page straight from a social timeline is jarring (adhoc #50).
        # The git page stays one row away ("Repository", below) and is linked
        # prominently on the profile page itself.
        profile_url = origin + "/@" + handle
        # Owner-set About description becomes the fediverse bio.
        catalog_rec = await decrypt_row(env, repo_row.get("data"))
        description = clean_string(
            (catalog_rec or {}).get("description", "") or "", 240).strip()
        summary = (ap.note_html_from_text(description) if description
                   else instance_features.rename(
                       env, ("ForkMesh repository %s/%s. Follow for new issues, "
                             "pull requests, discussions and releases.")
                       % (owner, repo)))
        # Owner-uploaded branding (repo About tab): logo = avatar, banner =
        # header. updated_at rides along as a cache-buster so Mastodon
        # refetches when the image changes.
        media_rows = await d1_all(
            env, "SELECT kind, updated_at FROM repo_media WHERE repo_bi=?",
            key_bi)
        for media in media_rows or []:
            media_url = "%s/api/repo/%s/%s/media/%s.png?v=%d" % (
                origin, quote(owner), quote(repo), media.get("kind", ""),
                int(media.get("updated_at") or 0))
            if media.get("kind") == "logo":
                icon_url = media_url
            elif media.get("kind") == "banner":
                image_url = media_url
        # No uploaded banner: fall back to the repo's committed
        # `.forkmesh/banner.png` at its published commit (the ref doubles as
        # the cache-buster). Still nothing means the actor ships no `image`,
        # which every client renders as a plain header.
        if not image_url:
            banner_resolver = globals().get(
                "_committed_repository_banner_url")
            committed_banner = (
                banner_resolver(catalog_rec, owner, repo)
                if callable(banner_resolver) else ""
            )
            if committed_banner:
                image_url = origin + committed_banner
        # No owner-uploaded fediverse logo: point at the repository logo API,
        # which projects the project's own committed root logo (or approved
        # artwork) as an image and 302s to this same instance avatar when the
        # repository has neither.
        if icon_url == origin + AP_AVATAR_PATH:
            versioner = globals().get("_repository_social_version")
            logo_version = (
                versioner(catalog_rec) if callable(versioner) else "")
            icon_url = "%s/api/repo/%s/%s/logo?image=1" % (
                origin, quote(owner), quote(repo))
            if logo_version:
                icon_url += "&v=" + quote(logo_version, safe="")
    # Profile metadata rows: the canonical page link (VERIFIABLE — the served
    # page carries a reciprocal rel="me" back to this same URL, which is also
    # the actor's `url`, so Mastodon's link verification turns it green) and
    # the relay this actor lives on (origin-derived, so self-hosted relays
    # advertise their own domain).
    # The verified profile row links the human page ("Repository" = a repo's
    # git page, "Profile" = a user's /@name page). For repos that page is no
    # longer the actor's `url`, so the reciprocal rel="me" that earns the green
    # check lives on the git page (see _serve_repo_page), pointing at this actor.
    web_link = repo_web_url if kind == AP_ACTOR_REPO else profile_url
    attachments = [
        ap.property_value(
            "Repository" if kind == AP_ACTOR_REPO else "Profile", web_link),
        # Plain text, deliberately: an anchor here made every follower
        # server's link verifier fetch the worker-served homepage on each
        # verification round, for a row that can never earn the green check.
        ap.property_text("Relay", _ap_domain_of(origin)),
    ]
    return ap.actor_doc(
        _ap_actor_url(origin, kind, handle), actor_type, handle, display,
        summary, profile_url, rec["pubkeyPem"],
        shared_inbox=origin + "/ap/inbox",
        published_ms=rec.get("createdAt"),
        icon_url=icon_url, image_url=image_url,
        attachments=attachments)
async def _ap_actor_doc_response(env, request, kind, handle):
    if method_name(request) not in ("GET", "HEAD"):
        return json_response({"error": "method_not_allowed"}, status=405)
    origin = _ap_origin(env, request)
    # Keyed on the canonical actor URL so every route shape that serves this
    # document (/ap/users/x, /@x with an ActivityPub Accept header, /ap/actor)
    # shares one edge entry.
    cache_key = _ap_actor_url(origin, kind, handle)
    # Only official repositories are followable local actors. The relay still
    # has an internal signing identity for secure ActivityPub fetches, but it
    # does not occupy ap_actors or expose a user/instance actor inventory.
    if kind == AP_ACTOR_USER:
        return await _ap_negative_response(cache_key)
    cached = await edge_cache_match(cache_key)
    if cached is not None:
        try:
            if int(cached.status) >= 400:
                return cached
        except Exception:
            pass
    await ensure_schema(env)
    if not await _ap_enabled(env):
        return _ap_disabled_response()
    if kind != AP_ACTOR_INSTANCE:
        # Cheap federation gate before minting keys for a 404.
        parsed = await _ap_resolve_local_target(
            env, origin, _ap_actor_url(origin, kind, handle))
        if not parsed:
            return await _ap_negative_response(cache_key)
    if cached is not None:
        return cached
    rec = (
        await _ap_instance_actor(env, create=True)
        if kind == AP_ACTOR_INSTANCE
        else await _ap_local_actor(env, kind, handle, create=True)
    )
    if not rec:
        return json_response({"error": "unavailable"}, status=503)
    doc = await _ap_build_actor_doc(env, origin, kind, handle, rec)
    if not doc:
        return await _ap_negative_response(cache_key)
    resp = json_response(doc, cache_seconds=300, extra_headers={
        "content-type": ap.ACTIVITY_CONTENT_TYPE})
    await edge_cache_put(cache_key, resp)
    return resp
async def _ap_broadcast_actor_update(env, request, kind, handle):
    """Push an Update(actor) to every follower so remote servers refetch the
    profile (avatar/banner/bio) immediately instead of waiting out their
    cache — this is how a repo's uploaded logo actually appears on Mastodon
    next to existing followers' timelines."""
    if not await _ap_enabled(env):
        return
    actor_bi = await _ap_actor_bi(env, kind, handle)
    rec = await _ap_local_actor(env, kind, handle)
    if not rec:
        return  # never followed / fetched — nothing to update
    origin = _ap_origin(env, request)
    actor_url = _ap_actor_url(origin, kind, handle)
    # Drop the edge-cached actor doc so refetches see the new profile — even
    # with no followers to notify, a remote server may hold the URL and poll
    # it (best-effort: other colos age out within the TTL). The rel=me page
    # the actor points at is edge-cached too (_serve_profile_page /
    # _serve_repo_page), so drop the canonical page keys alongside it.
    await edge_cache_delete(actor_url)
    if kind == AP_ACTOR_USER:
        await edge_cache_delete(origin + "/@" + quote(handle))
        await edge_cache_delete(origin + "/@" + quote(handle) + "/repositories")
        await edge_cache_delete(ACCOUNT_LOOKUP_CACHE_PREFIX + quote(handle))
        # Toggling "make my followers/following public" (Settings) changes
        # what ap_collection_handler returns for the same cached URL, so drop
        # it too — otherwise the old bare-collection response (or stale
        # items) lingers for the rest of its 300s TTL.
        await edge_cache_delete(actor_url + "/followers")
        await edge_cache_delete(actor_url + "/following")
    elif kind == AP_ACTOR_REPO:
        page_owner, _, page_repo = handle.partition(".")
        # The git page carries the rel="me" the green check verifies; the
        # /@owner.repo profile page is the actor's `url` and its post feed.
        await edge_cache_delete(origin + repo_web_href(page_owner, page_repo))
        await edge_cache_delete(origin + "/@" + handle)
    followers = await d1_all(
        env, "SELECT inbox, shared_inbox FROM ap_followers WHERE actor_bi=?",
        actor_bi)
    if not followers:
        return
    doc = await _ap_build_actor_doc(env, origin, kind, handle, rec)
    if not doc:
        return
    now = int(Date.now())
    body_str = json.dumps(ap.update_activity(actor_url, doc, now))
    out_data = await encrypt_row(env, {
        "body": body_str, "actorKind": kind, "actorHandle": handle,
        "actorUrl": actor_url})
    seen = set()
    for follower in followers:
        inbox = ((follower.get("shared_inbox") or "").strip()
                 or (follower.get("inbox") or "").strip())
        if not inbox or inbox in seen:
            continue
        seen.add(inbox)
        await d1_run(
            env,
            "INSERT INTO ap_outbox (inbox, data, attempts, next_ts,"
            " created_at) VALUES (?,?,0,?,?)",
            inbox, out_data, now, now)
    await _ap_drain_outbox(env, AP_IMMEDIATE_DELIVERIES)
async def ap_collection_handler(env, request, kind, handle, which):
    if method_name(request) != "GET":
        return json_response({"error": "method_not_allowed"}, status=405)
    origin = _ap_origin(env, request)
    collection_url = _ap_actor_url(origin, kind, handle) + "/" + which
    cached = await edge_cache_match(collection_url)
    if cached is not None:
        try:
            if int(cached.status) >= 400:
                return cached
        except Exception:
            pass
    await ensure_schema(env)
    if not await _ap_enabled(env):
        return _ap_disabled_response()
    if kind == AP_ACTOR_USER and not await _ap_user_federates(env, handle):
        return await _ap_negative_response(collection_url)
    if kind == AP_ACTOR_REPO:
        owner, _, repo = handle.partition(".")
        if not await _ap_repo_federates(env, owner, repo):
            return await _ap_negative_response(collection_url)
    if cached is not None:
        return cached
    actor_bi = await _ap_actor_bi(env, kind, handle)
    total = 0
    items = None
    if which == "followers":
        row = await d1_first(
            env, "SELECT COUNT(*) AS c FROM ap_followers WHERE actor_bi=?",
            actor_bi)
        total = (row or {}).get("c", 0) or 0
        # Repo watchers are already public (the About/Watch popover lists
        # them), so repo actors always enumerate. User actors only enumerate
        # when the account owner opted in via Settings — otherwise remote
        # servers correctly read the bare collection as "not made visible".
        show_items = True
        if kind == AP_ACTOR_USER:
            _, account_rec = await _account_row(env, handle)
            show_items = bool((account_rec or {}).get(
                "profile_followers_public"))
        if show_items and total:
            rows = await d1_all(
                env,
                "SELECT follower_id FROM ap_followers WHERE actor_bi=?"
                " ORDER BY created_at DESC LIMIT ?",
                actor_bi, AP_COLLECTION_PAGE_SIZE)
            items = [r.get("follower_id", "") for r in (rows or [])
                     if r.get("follower_id")]
        elif show_items:
            items = []
    elif which == "outbox":
        row = await d1_first(
            env, "SELECT COUNT(*) AS c FROM ap_objects WHERE actor_bi=?",
            actor_bi)
        total = (row or {}).get("c", 0) or 0
    resp = json_response(
        ap.collection_doc(collection_url, total, items=items),
        cache_seconds=300,
        extra_headers={"content-type": ap.ACTIVITY_CONTENT_TYPE})
    await edge_cache_put(collection_url, resp)
    return resp
async def ap_object_handler(env, request, object_uuid):
    if method_name(request) not in ("GET", "HEAD"):
        return json_response({"error": "method_not_allowed"}, status=405)
    cache_key = _ap_origin(env, request) + "/ap/o/" + object_uuid
    await ensure_schema(env)
    if not await _ap_enabled(env):
        return _ap_disabled_response()
    row = await d1_first(
        env, "SELECT data FROM ap_objects WHERE object_uuid=?", object_uuid)
    rec = await decrypt_row(env, row.get("data")) if row else None
    if not rec or not isinstance(rec.get("note"), dict):
        return json_response({"error": "not_found"}, status=404)
    context = rec.get("context") if isinstance(rec.get("context"), dict) else {}
    segmenter = globals().get("safe_segment")
    context_owner = (
        segmenter(context.get("owner", ""))
        if callable(segmenter) else str(context.get("owner", "") or ""))
    context_repo = (
        segmenter(context.get("repo", ""))
        if callable(segmenter) else str(context.get("repo", "") or ""))
    privacy_reader = globals().get("_repo_is_private")
    if context_owner and context_repo and callable(privacy_reader):
        data_owner = await _ap_org_alias_owner(
            env, context_owner, context_repo)
        if await privacy_reader(env, data_owner, context_repo):
            return json_response(
                {"error": "not_found"}, status=404, cache_control="no-store")
    cached = await edge_cache_match(cache_key)
    if cached is not None:
        return cached
    doc = dict(rec["note"])
    doc["@context"] = ap.AS_CONTEXT
    resp = json_response(doc, cache_seconds=300, extra_headers={
        "content-type": ap.ACTIVITY_CONTENT_TYPE})
    await edge_cache_put(cache_key, resp)
    return resp
async def ap_object_media_handler(env, request, object_uuid, index):
    # Re-serves one image that was embedded as a "data:" URL in the source
    # issue/comment body, at a real URL — remote servers fetch a Note's
    # attachment URLs directly and can't resolve a data: URI.
    if method_name(request) not in ("GET", "HEAD"):
        return json_response({"error": "method_not_allowed"}, status=405)
    # Every remote server that receives the Note fetches its attachment URLs,
    # and the bytes are immutable — serve the burst from the colo cache.
    idx = int(index)
    cache_key = "%s/ap/o/%s/media/%d" % (
        _ap_origin(env, request), object_uuid, idx)
    await ensure_schema(env)
    if not await _ap_enabled(env):
        return _ap_disabled_response()
    row = await d1_first(
        env, "SELECT data FROM ap_objects WHERE object_uuid=?", object_uuid)
    rec = await decrypt_row(env, row.get("data")) if row else None
    context = rec.get("context") if isinstance((rec or {}).get("context"), dict) \
        else {}
    segmenter = globals().get("safe_segment")
    context_owner = (
        segmenter(context.get("owner", ""))
        if callable(segmenter) else str(context.get("owner", "") or ""))
    context_repo = (
        segmenter(context.get("repo", ""))
        if callable(segmenter) else str(context.get("repo", "") or ""))
    privacy_reader = globals().get("_repo_is_private")
    if context_owner and context_repo and callable(privacy_reader):
        data_owner = await _ap_org_alias_owner(
            env, context_owner, context_repo)
        if await privacy_reader(env, data_owner, context_repo):
            return json_response(
                {"error": "not_found"}, status=404, cache_control="no-store")
    media = (rec or {}).get("media") or []
    if idx < 0 or idx >= len(media):
        return json_response({"error": "not_found"}, status=404)
    cached = await edge_cache_match_media(cache_key, "image/png")
    if cached is not None:
        return cached
    item = media[idx]
    try:
        raw = base64.b64decode(item.get("data", ""), validate=True)
    except Exception:
        return json_response({"error": "not_found"}, status=404)
    # Uint8Array.new copies into a JS-owned buffer; a bare _to_js(bytes) view
    # into WASM memory would be read off the GIL (edge-cache put / client
    # stream, after this handler returns) and crash the isolate.
    resp = JsResponse.new(Uint8Array.new(_to_js(bytes(raw))), to_js({
        "status": 200,
        "headers": {
            "content-type": item.get("mediaType", "image/png"),
            "cache-control": "public, max-age=31536000, immutable",
        },
    }))
    await edge_cache_put(cache_key, resp)
    return resp
async def _ap_signed_request(key_id, priv_b64, method, url, body_str=None,
                             accept="application/activity+json"):
    # One signed outbound HTTP request (draft-cavage). GETs sign
    # (request-target) host date accept; POSTs add digest + content-type.
    url = ap.public_federation_url(url)
    if not url:
        return None
    parts = urlparse(url)
    path = (parts.path or "/") + (("?" + parts.query) if parts.query else "")
    headers = {"host": parts.netloc, "date": ap.http_date(int(Date.now())),
               "accept": accept}
    names = list(ap.SIGNED_HEADERS_GET)
    if body_str is not None:
        headers["digest"] = ap.digest_header_value(body_str.encode())
        headers["content-type"] = "application/activity+json"
        names = list(ap.SIGNED_HEADERS_POST)
    base = ap.signing_string(method, path, headers, names)
    if base is None:
        return None
    signature = await _ap_rsa_sign_b64(priv_b64, base)
    send_headers = {
        "date": headers["date"],
        "accept": accept,
        "user-agent": instance_features.user_agent(
            "forkmesh-app/1.0 (+https://forkmesh.com)", "app"),
        "signature": ap.build_signature_header(key_id, names, signature),
    }
    init = {"method": method, "headers": send_headers,
            "redirect": "manual"}
    if body_str is not None:
        send_headers["digest"] = headers["digest"]
        send_headers["content-type"] = headers["content-type"]
        init["body"] = body_str
    try:
        return await js_fetch_with_timeout(
            url, init, CRON_OUTBOUND_FETCH_TIMEOUT_SECONDS)
    except Exception:
        return None
async def _ap_instance_key(env):
    origin = _ap_origin(env)
    rec = await _ap_instance_actor(env, create=True)
    if not rec:
        return None, None
    key_id = _ap_actor_url(origin, AP_ACTOR_INSTANCE,
                           AP_INSTANCE_HANDLE) + "#main-key"
    return key_id, rec.get("privkey")
async def _ap_remote_actor(env, actor_id, force_refresh=False):
    # Cached remote actor document (inbox + public key). Fetches are signed
    # with the instance actor so "secure mode" servers answer.
    actor_id = ap.public_federation_url(
        (actor_id or "").split("#")[0].rstrip("/"))
    if not actor_id:
        return None
    if await _ap_domain_blocked(env, urlparse(actor_id).netloc):
        return None
    row = await d1_first(
        env, "SELECT * FROM ap_remote_actors WHERE actor_id=?", actor_id)
    now = int(Date.now())
    if (row and not force_refresh and row.get("pubkey_pem")
            and now - int(row.get("updated_at") or 0) < AP_REMOTE_ACTOR_TTL_MS):
        return row
    if not _ap_outbound_enabled(env):
        return row or None
    key_id, priv = await _ap_instance_key(env)
    if not priv:
        return row or None
    resp = await _ap_signed_request(key_id, priv, "GET", actor_id)
    if resp is None or int(getattr(resp, "status", 0)) >= 400:
        return row or None
    try:
        raw = await bounded_request_text(resp, ap.MAX_REMOTE_ACTOR_BYTES)
        doc = json.loads(raw)
    except RequestBodyTooLarge:
        return row or None
    except Exception:
        return row or None
    ess = ap.actor_essentials(doc)
    # The document must describe the URL we fetched (same host at minimum) so
    # a malicious document can't impersonate another server's actor.
    actor_url = ap.public_federation_url((ess or {}).get("id"))
    inbox = ap.public_federation_url((ess or {}).get("inbox"))
    if (not ess or not actor_url or not inbox
            or urlparse(actor_url).netloc != urlparse(actor_id).netloc):
        return row or None
    shared_inbox = ap.public_federation_url(ess.get("sharedInbox"))
    handle = ess["preferredUsername"]
    if handle:
        handle = handle + "@" + urlparse(ess["id"]).netloc
    # Public presentation, cached alongside the routing fields: the avatar URL
    # must be an https media URL (never a data: payload or a private host), and
    # the bio is untrusted remote HTML flattened to bounded plain text.
    avatar_url = ap.public_media_url(ess.get("icon"))
    summary = ap_threads.sanitize_remote_content(ess.get("summary"), 500)
    await d1_run(
        env,
        "INSERT INTO ap_remote_actors (actor_id, inbox, shared_inbox,"
        " pubkey_pem, handle, display_name, url, updated_at, avatar_url,"
        " summary) VALUES (?,?,?,?,?,?,?,?,?,?)"
        " ON CONFLICT(actor_id) DO UPDATE SET inbox=excluded.inbox,"
        " shared_inbox=excluded.shared_inbox, pubkey_pem=excluded.pubkey_pem,"
        " handle=excluded.handle, display_name=excluded.display_name,"
        " url=excluded.url, updated_at=excluded.updated_at,"
        " avatar_url=excluded.avatar_url, summary=excluded.summary",
        actor_id, inbox, shared_inbox, ess["pubkeyPem"],
        handle, clean_string(ess["name"], 200), ess["url"], now,
        avatar_url, summary)
    return {
        "actor_id": actor_id, "inbox": inbox,
        "shared_inbox": shared_inbox, "pubkey_pem": ess["pubkeyPem"],
        "handle": handle, "display_name": clean_string(ess["name"], 200),
        "url": ess["url"], "updated_at": now, "avatar_url": avatar_url,
        "summary": summary,
    }
async def _ap_refresh_follower_profiles(env, limit, window=0):
    """Backfill the cached avatar/bio of accounts that follow a local actor.

    A follower row only carries routing fields; the presentation a "who follows
    this repository" surface shows comes from the cached actor document, which
    is otherwise only refreshed when that actor next delivers something (or
    never, for a quiet follower — and never for any row written before
    migration 0078). This sweeps the oldest cache entries that are still missing
    those columns, bounded to one signed GET each.

    The window rotates the selection so a permanently unreachable server — whose
    row keeps its old updated_at and therefore keeps sorting first — cannot
    starve every other follower out of the batch."""
    limit = max(1, int(limit))
    candidates = (
        "SELECT actor_id FROM ap_remote_actors"
        " WHERE actor_id IN (SELECT follower_id FROM ap_followers)"
        "   AND (avatar_url IS NULL OR summary IS NULL)"
        " ORDER BY updated_at LIMIT ? OFFSET ?")
    offset = max(0, int(window)) * limit
    rows = await d1_all(env, candidates, limit, offset)
    if not rows and offset:
        # Backlog smaller than this window: sweep from the front instead of
        # idling the tick.
        rows = await d1_all(env, candidates, limit, 0)
    refreshed = 0
    for row in rows or []:
        actor_id = str(row.get("actor_id") or "")
        if not actor_id:
            continue
        if await _ap_remote_actor(env, actor_id, force_refresh=True):
            refreshed += 1
    return refreshed
async def _ap_deliver_body(env, actor_url, priv_b64, inbox_url, body_str):
    """Returns (status, retry_after_ms). retry_after_ms is 0 unless the
    remote answered 429/503 with a parseable Retry-After — the drain uses it
    as a floor under the normal backoff so a throttling server isn't re-hit
    sooner than it asked."""
    if not _ap_outbound_enabled(env):
        print("ap: outbound delivery disabled (AP_OUTBOUND off); not sent")
        return 0, 0
    resp = await _ap_signed_request(
        actor_url + "#main-key", priv_b64, "POST", inbox_url,
        body_str=body_str)
    if resp is None:
        return 0, 0
    status = int(getattr(resp, "status", 0))
    retry_after_ms = 0
    if status in (429, 503):
        try:
            raw = (resp.headers.get("retry-after") or "").strip()
        except Exception:
            raw = ""
        if raw.isdigit():
            retry_after_ms = int(raw) * 1000
        elif raw:
            when = ap.parse_http_date_ms(raw)
            if when:
                retry_after_ms = when - int(Date.now())
        retry_after_ms = max(0, min(retry_after_ms, 24 * 60 * 60 * 1000))
    return status, retry_after_ms
async def _ap_drain_outbox(env, limit):
    # Deliver due ap_outbox rows. Success (or a permanently-gone inbox, or the
    # attempt cap) deletes the row; anything else reschedules with backoff.
    if not await _ap_enabled(env) or not _ap_outbound_enabled(env):
        return
    now = int(Date.now())
    rows = await d1_all(
        env,
        "SELECT id, inbox, data, attempts FROM ap_outbox WHERE next_ts<=?"
        " ORDER BY next_ts ASC LIMIT ?",
        now, int(limit))
    for row in rows or []:
        if await _ap_domain_blocked(
                env, urlparse(row.get("inbox", "")).netloc):
            await d1_run(env, "DELETE FROM ap_outbox WHERE id=?", row.get("id"))
            continue
        rec = await decrypt_row(env, row.get("data"))
        attempts = int(row.get("attempts") or 0) + 1
        status, retry_after_ms = 0, 0
        if rec and rec.get("body") and rec.get("actorKind"):
            local = await _ap_local_actor(
                env, rec["actorKind"], rec["actorHandle"])
            if local:
                status, retry_after_ms = await _ap_deliver_body(
                    env, rec.get("actorUrl", ""), local["privkey"],
                    row.get("inbox", ""), rec["body"])
        done = (200 <= status < 400) or status in (404, 410)
        if done or not rec or attempts >= ap.MAX_DELIVERY_ATTEMPTS:
            await d1_run(env, "DELETE FROM ap_outbox WHERE id=?", row.get("id"))
        else:
            await d1_run(
                env, "UPDATE ap_outbox SET attempts=?, next_ts=? WHERE id=?",
                attempts,
                now + max(ap.retry_backoff_ms(attempts), retry_after_ms),
                row.get("id"))
async def _ap_publish_repo_event(env, request, owner, repo, kind, event_type,
                                 ref, title, body, author_name,
                                 extra_images=None, manual=False,
                                 include_repo_actor=True, public_owner=None,
                                 link_url=None):
    """Queue an automatic repo event, or immediately publish an explicit one.

    Normal issue/PR/discussion/commit/release hooks use the encrypted,
    deterministic, at-most-daily digest queue. An authenticated caller that
    explicitly passes ``manual=True`` retains the immediate Note path. The
    caller decides whether its proof also permits the repository actor.

    Actor rows remain lazy. Queued digests are published only after a
    fediverse lookup has created the corresponding repository actor."""
    owner = (owner or "").strip().lower()
    repo = (repo or "").strip()
    announcement = kind == AP_ANNOUNCEMENT_KIND
    if (not owner or not repo
            or (kind not in AP_FEDI_KINDS and not announcement)
            or (announcement and not manual)):
        return
    if not await _ap_enabled(env):
        return
    privacy_reader = globals().get("_repo_is_private")
    if callable(privacy_reader) and await privacy_reader(env, owner, repo):
        return
    if not manual:
        await _ap_enqueue_repo_digest(
            env, request, owner, repo, kind, event_type, ref, title, body)
        return
    origin = _ap_origin(env, request)
    public = (public_owner or owner).strip().lower()
    web_url = link_url or (origin + repo_web_href(public, repo))
    author = clean_string(author_name or "", MAX_NODE_NAME).lower()
    # Handles (and thus actor ids) are lowercase-normalized so the same repo
    # always federates as one stable actor URL regardless of link casing.
    # The repo actor obeys both the caller's authorization scope and per-repo
    # switches, while the author's own actor is governed by that user's
    # federation presence.
    settings = await _ap_repo_settings_get(env, owner, repo)
    candidates = []
    if (include_repo_actor and settings["federate"]
            and settings["broadcastEvents"]):
        candidates.append((AP_ACTOR_REPO, ap.repo_handle(public, repo.lower())))
    if author and valid_node_name(author):
        candidates.append((AP_ACTOR_USER, author))
    # Attached issue/comment images travel as inline "![name](data:...)"
    # markdown (there's no separate upload channel); pull them out so they
    # federate as real Image attachments instead of unrenderable base64 text.
    clean_body, images = ap.extract_body_images(body or "")
    # Caller-supplied media (the generated pull-request badge PNG, adhoc #44)
    # leads the attachment list so it becomes the visible preview.
    images = [img for img in (extra_images or []) if img] + images
    text = ap.event_note_text(
        kind, event_type, public, repo, ref, clean_string(title, 240),
        clean_body, author, web_url)
    content_html = ap.note_html_from_text(text)
    ckey = ap.context_key(public, repo, kind, ref)
    context_bi = await blind_index(env, "ap-context:" + ckey)
    now = int(Date.now())
    queued = False
    for actor_kind, handle in candidates:
        actor_bi = await _ap_actor_bi(env, actor_kind, handle)
        exists = await d1_first(
            env, "SELECT kind FROM ap_actors WHERE actor_bi=?", actor_bi)
        if not exists:
            continue
        followers = await d1_all(
            env,
            "SELECT inbox, shared_inbox FROM ap_followers WHERE actor_bi=?",
            actor_bi)
        if not followers:
            continue
        actor_url = _ap_actor_url(origin, actor_kind, handle)
        object_uuid = _ap_uuid()
        attachments = [
            ap.image_object(
                "%s/ap/o/%s/media/%d" % (origin, object_uuid, i),
                media_type=img["mediaType"])
            for i, img in enumerate(images)]
        note = ap.note_doc(
            origin + "/ap/o/" + object_uuid, actor_url,
            actor_url + "/followers", content_html, now, web_url=web_url,
            attachments=attachments)
        rec = {"note": note,
               "context": {"owner": public, "repo": repo, "kind": kind,
                           "ref": str(ref), "key": ckey},
               "media": images}
        await d1_run(
            env,
            "INSERT INTO ap_objects (object_uuid, actor_bi, context_bi, data,"
            " published) VALUES (?,?,?,?,?)",
            object_uuid, actor_bi, context_bi,
            await encrypt_row(env, rec), now)
        body_str = json.dumps(ap.create_activity(note))
        out_data = await encrypt_row(env, {
            "body": body_str, "actorKind": actor_kind, "actorHandle": handle,
            "actorUrl": actor_url})
        seen = set()
        for follower in followers:
            inbox = ((follower.get("shared_inbox") or "").strip()
                     or (follower.get("inbox") or "").strip())
            if not inbox or inbox in seen:
                continue
            seen.add(inbox)
            await d1_run(
                env,
                "INSERT INTO ap_outbox (inbox, data, attempts, next_ts,"
                " created_at) VALUES (?,?,0,?,?)",
                inbox, out_data, now, now)
            queued = True
    if queued:
        await _ap_drain_outbox(env, AP_IMMEDIATE_DELIVERIES)
async def _ap_forget_remote(env, actor_id):
    await d1_run(env, "DELETE FROM ap_followers WHERE follower_id=?", actor_id)
    await d1_run(env, "DELETE FROM ap_remote_actors WHERE actor_id=?", actor_id)
async def _ap_resolve_local_target(env, origin, url):
    """Map an activity's object URL to a local actor: returns (kind, handle,
    notify_recipient, display) or None."""
    target = ap.parse_local_actor_url(origin, url)
    if not target or target[0] == "instance":
        return None
    if target[0] == "user":
        if not await _ap_user_federates(env, target[1]):
            return None
        return (AP_ACTOR_USER, target[1], target[1], "@" + target[1])
    owner, repo = target[1], target[2]
    if not await _ap_repo_federates(env, owner, repo):
        return None
    return (AP_ACTOR_REPO, ap.repo_handle(owner, repo), owner,
            owner + "/" + repo)
async def _ap_handle_follow(env, request, activity, remote):
    origin = _ap_origin(env, request)
    resolved = await _ap_resolve_local_target(
        env, origin, ap.activity_object_id(activity.get("object")))
    if not resolved:
        return json_response({"error": "not_found"}, status=404)
    kind, handle, recipient, display = resolved
    local = await _ap_local_actor(env, kind, handle, create=True)
    if not local:
        return json_response({"error": "unavailable"}, status=503)
    count = await d1_first(
        env, "SELECT COUNT(*) AS c FROM ap_followers WHERE actor_bi=?",
        local["actorBi"])
    if ((count or {}).get("c", 0) or 0) >= AP_MAX_FOLLOWERS_PER_ACTOR:
        return json_response({"error": "followers_limit"}, status=429)
    now = int(Date.now())
    await d1_run(
        env,
        "INSERT INTO ap_followers (actor_bi, follower_id, inbox, shared_inbox,"
        " follower_handle, created_at) VALUES (?,?,?,?,?,?)"
        " ON CONFLICT(actor_bi, follower_id) DO UPDATE SET"
        " inbox=excluded.inbox, shared_inbox=excluded.shared_inbox,"
        " follower_handle=excluded.follower_handle",
        local["actorBi"], remote["actor_id"], remote.get("inbox", ""),
        remote.get("shared_inbox", "") or "", remote.get("handle", "") or "",
        now)
    # Auto-accept (no manual follow approval), delivered synchronously — one
    # subrequest, and Mastodon marks the follow pending until it arrives.
    actor_url = _ap_actor_url(origin, kind, handle)
    follow_ref = {
        "id": str(activity.get("id", "") or ""),
        "type": "Follow",
        "actor": remote["actor_id"],
        "object": actor_url,
    }
    accept_body = json.dumps(ap.accept_activity(actor_url, follow_ref))
    inbox_url = (remote.get("inbox") or "").strip()
    status, _ = await _ap_deliver_body(
        env, actor_url, local["privkey"], inbox_url, accept_body)
    if inbox_url and not (200 <= status < 400) and status not in (404, 410):
        # The remote server keeps the follow "pending" (spinner) until the
        # Accept lands, and it does NOT retry a Follow we already 202'd — so
        # a lost Accept used to leave the follow stuck forever. Queue it for
        # the cron drain, which retries with the normal backoff.
        out_data = await encrypt_row(env, {
            "body": accept_body, "actorKind": kind, "actorHandle": handle,
            "actorUrl": actor_url})
        await d1_run(
            env,
            "INSERT INTO ap_outbox (inbox, data, attempts, next_ts,"
            " created_at) VALUES (?,?,1,?,?)",
            inbox_url, out_data, now, now)
    follower_label = remote.get("handle") or remote.get("actor_id", "")
    await _best_effort_inbox_side_effect(enqueue_notification(
        env, recipient, "subscribed", "New fediverse follower",
        body="%s is now following %s on the fediverse." % (follower_label,
                                                           display),
        source="fediverse",
        dedupe="ap-follow:%s:%s" % (remote["actor_id"], handle)))
    return json_response({"ok": True}, status=202)
async def _ap_handle_undo(env, request, activity):
    inner = activity.get("object")
    if isinstance(inner, dict) and str(inner.get("type", "")) == "Follow":
        origin = _ap_origin(env, request)
        target = ap.parse_local_actor_url(
            origin, ap.activity_object_id(inner.get("object")))
        if target and target[0] != "instance":
            if target[0] == "user":
                kind, handle = AP_ACTOR_USER, target[1]
            else:
                kind, handle = AP_ACTOR_REPO, ap.repo_handle(
                    target[1], target[2])
            actor_bi = await _ap_actor_bi(env, kind, handle)
            await d1_run(
                env,
                "DELETE FROM ap_followers WHERE actor_bi=? AND follower_id=?",
                actor_bi, ap.activity_object_id(activity.get("actor")))
    return json_response({"ok": True}, status=202)
async def _ap_comment_by_remote_id(env, remote_id):
    """Return (row, decrypted record, blind index) for a remote reply."""
    if not str(remote_id or "").startswith("https://"):
        return None, None, ""
    remote_id_bi = await blind_index(env, "ap-comment:" + str(remote_id))
    row = await d1_first(
        env,
        "SELECT context_bi, remote_id_bi, parent_remote_id_bi, lifecycle,"
        " data, ts FROM ap_comments WHERE remote_id_bi=?",
        remote_id_bi)
    rec = await decrypt_row(env, row.get("data")) if row else None
    return row, rec, remote_id_bi
async def _ap_reply_parent(env, origin, parent_url):
    """Resolve a direct local-object or nested remote-reply parent.

    Remote activities cannot choose a ForkMesh context themselves. Nested
    Lemmy comments inherit the verified parent's encrypted local context.
    """
    parent_uuid = ap.parse_local_object_url(origin, parent_url)
    if parent_uuid:
        row = await d1_first(
            env, "SELECT data, context_bi FROM ap_objects WHERE object_uuid=?",
            parent_uuid)
        parent = await decrypt_row(env, row.get("data")) if row else None
        if parent and isinstance(parent.get("context"), dict):
            return row.get("context_bi"), parent["context"], ""
        return None, None, ""
    row, rec, parent_bi = await _ap_comment_by_remote_id(env, parent_url)
    if row and rec and isinstance(rec.get("context"), dict):
        return row.get("context_bi"), rec["context"], parent_bi
    return None, None, ""
def _ap_activity_is_public(activity):
    """True only when Create/Note addressing explicitly names AS Public."""
    if not isinstance(activity, dict):
        return False
    candidates = [activity]
    embedded = activity.get("object")
    if isinstance(embedded, dict):
        candidates.append(embedded)
    for candidate in candidates:
        for key in ("to", "cc", "audience"):
            values = candidate.get(key, [])
            if not isinstance(values, list):
                values = [values]
            for value in values:
                target = ap.activity_object_id(value)
                if target == ap.AS_PUBLIC:
                    return True
    return False
async def _ap_handle_create(env, request, activity, remote):
    # A remote reply to one of our published Notes becomes a federated comment
    # on the underlying thread; a post that @-mentions one of our repo actors
    # becomes verified public activity awaiting optional, explicit owner
    # review. Nothing in this inbound path files an issue automatically.
    # Anything else is acknowledged and dropped (we host repos, not timelines).
    note = ap.note_essentials(activity.get("object"))
    if not note:
        return json_response({"ok": True}, status=202)
    # HTTP signatures authenticate the sender, not the intended audience.
    # Direct/private Notes must never enter a public thread or World feed.
    if not _ap_activity_is_public(activity):
        return json_response({"ok": True}, status=202)
    # The note's author must be the signing actor (same host is not enough:
    # one compromised account must not be able to speak for a whole server).
    if note["attributedTo"].split("#")[0].rstrip("/") != remote["actor_id"]:
        return json_response({"error": "author_mismatch"}, status=401)
    origin = _ap_origin(env, request)
    context_bi, context, parent_remote_id_bi = await _ap_reply_parent(
        env, origin, note["inReplyTo"])
    if not context:
        mention = _ap_note_repo_mention(origin, note)
        if mention:
            return await _ap_handle_repo_mention(
                env, request, note, remote, mention[0], mention[1])
        return json_response({"ok": True}, status=202)
    # Owner switched federated comments (or the whole actor) off: acknowledge
    # and drop, same as any non-reply — redelivery storms must not 4xx.
    reply_settings = await _ap_repo_settings_get(
        env, context.get("owner", ""), context.get("repo", ""))
    if not reply_settings["federate"] or not reply_settings["acceptComments"]:
        return json_response({"ok": True}, status=202)
    existing_row, existing_rec, remote_id_bi = \
        await _ap_comment_by_remote_id(env, note["id"])
    if existing_row and existing_rec:
        return json_response({"ok": True, "duplicate": True}, status=202)
    count = await d1_first(
        env, "SELECT COUNT(*) AS c FROM ap_comments WHERE context_bi=?",
        context_bi)
    if ((count or {}).get("c", 0) or 0) >= AP_MAX_COMMENTS_PER_THREAD:
        return json_response({"error": "thread_full"}, status=429)
    now = int(Date.now())
    normalized = ap_threads.apply_activity(
        {}, activity, remote_actor=remote, context=context, received_ms=now)
    if not normalized.get("ok"):
        return json_response({"ok": True}, status=202)
    rec = normalized["record"]
    # The HTTP delivery signature was verified above, but that is explicitly
    # not a ForkMesh Ed25519 native-event signature.
    rec["provenance"]["deliverySignatureVerified"] = True
    rec["provenance"]["nativeSignatureVerified"] = False
    await d1_run(
        env,
        "INSERT OR IGNORE INTO ap_comments (context_bi, remote_id_bi,"
        " parent_remote_id_bi, lifecycle, data, ts) VALUES (?,?,?,?,?,?)",
        context_bi, remote_id_bi, parent_remote_id_bi or None,
        rec["lifecycle"], await encrypt_row(env, rec), now)
    if context.get("kind") == "issue":
        reply_owner = context.get("owner", "")
        reply_repo = context.get("repo", "")
        data_owner = await _ap_org_alias_owner(
            env, reply_owner, reply_repo)
        if not await _repo_is_private(env, data_owner, reply_repo):
            public_note = dict(note)
            public_note["content"] = rec.get("body", "")
            await fediverse_mentions_api.record_verified(
                _WorldCommunityRuntime(env, request),
                note=public_note,
                remote=remote,
                actor_owner=reply_owner,
                data_owner=data_owner,
                repo=reply_repo,
                kind="reply",
                context={
                    "ref": context.get("ref", ""),
                    "issueUrl": (
                        origin
                        + repo_web_href(reply_owner, reply_repo)
                        + "/issues/"
                        + str(context.get("ref", ""))
                    ),
                },
                signature_verified=True,
                public_activity=True,
                public_repository=True,
            )
    title = "Fediverse reply on %s/%s %s %s" % (
        context.get("owner", ""), context.get("repo", ""),
        context.get("kind", ""), context.get("ref", ""))
    await _best_effort_inbox_side_effect(enqueue_notification(
        env, context.get("owner", ""), "subscribed", title,
        body="%s: %s" % (rec["author"], rec["body"][:280]),
        repo="%s/%s" % (context.get("owner", ""), context.get("repo", "")),
        href=repo_web_href(context.get("owner", ""), context.get("repo", "")),
        source="fediverse", dedupe="ap-comment:" + note["id"]))
    return json_response({"ok": True}, status=202)
async def _ap_handle_update(env, activity, remote):
    note = ap.note_essentials(activity.get("object"))
    if not note:
        return json_response({"ok": True}, status=202)
    row, existing, _ = await _ap_comment_by_remote_id(env, note["id"])
    if not row or not existing:
        return json_response({"ok": True}, status=202)
    result = ap_threads.apply_activity(
        {note["id"]: existing}, activity, remote_actor=remote,
        context=existing.get("context"), received_ms=int(Date.now()))
    if not result.get("ok"):
        status = 401 if result.get("reason") == "author_mismatch" else 202
        return json_response({"error": result.get("reason")}, status=status)
    if result.get("changed"):
        rec = result["record"]
        rec["provenance"]["deliverySignatureVerified"] = True
        rec["provenance"]["nativeSignatureVerified"] = False
        await d1_run(
            env,
            "UPDATE ap_comments SET lifecycle=?, data=?"
            " WHERE remote_id_bi=?",
            rec["lifecycle"], await encrypt_row(env, rec),
            row.get("remote_id_bi"))
    return json_response(
        {"ok": True, "duplicate": not result.get("changed")}, status=202)
async def _ap_handle_remove(env, activity, remote):
    target_id = ap.activity_object_id(activity.get("object"))
    row, existing, _ = await _ap_comment_by_remote_id(env, target_id)
    if not row or not existing:
        return json_response({"ok": True}, status=202)
    # Lemmy community moderators use a different actor from the comment
    # author. Accept moderation only from the same already-verified instance.
    actor_host = urlparse(remote.get("actor_id", "")).hostname or ""
    if actor_host.lower() != existing.get("sourceInstance", "").lower():
        return json_response({"error": "moderator_instance_mismatch"},
                             status=401)
    result = ap_threads.apply_activity(
        {target_id: existing}, activity, remote_actor=remote,
        context=existing.get("context"), received_ms=int(Date.now()))
    if result.get("changed"):
        rec = result["record"]
        rec["provenance"]["deliverySignatureVerified"] = True
        rec["provenance"]["nativeSignatureVerified"] = False
        await d1_run(
            env,
            "UPDATE ap_comments SET lifecycle=?, data=?"
            " WHERE remote_id_bi=?",
            rec["lifecycle"], await encrypt_row(env, rec),
            row.get("remote_id_bi"))
    return json_response({"ok": True}, status=202)
def _ap_note_repo_mention(origin, note):
    """First Mention tag pointing at one of our repo actors -> (owner, repo)
    (lowercased), else None. Pure string parsing so the inbox can gate on it
    before the signature dance."""
    for href in note.get("mentions", []):
        target = ap.parse_local_actor_url(origin, href)
        if target and target[0] == "repo":
            return target[1], target[2]
    return None
async def _ap_send_mention_reply(env, request, owner, repo, remote, note,
                                 text, issue_number, dedupe_id="",
                                 object_uuid=""):
    """Reply to the mentioning post as the repo actor: a public Note in the
    author's thread with a Mention tag so their server notifies them. Queued
    through ap_outbox so a lost delivery retries on the cron drain. The stored
    object carries the new issue's context, so fediverse replies to OUR reply
    land as federated comments on that issue."""
    origin = _ap_origin(env, request)
    handle = ap.repo_handle(owner, repo)
    local = await _ap_local_actor(env, AP_ACTOR_REPO, handle, create=True)
    if not local:
        return False
    actor_url = _ap_actor_url(origin, AP_ACTOR_REPO, handle)
    now = int(Date.now())
    if not re.fullmatch(r"[a-f0-9]{32}", str(object_uuid or "")):
        object_uuid = _ap_uuid()
    reply = ap.note_doc(
        origin + "/ap/o/" + object_uuid, actor_url, actor_url + "/followers",
        ap.note_html_from_text(text), now,
        web_url=origin + repo_web_href(owner, repo) + "/issues",
        in_reply_to=note["id"])
    # Address the author directly (still public, so the exchange is visible
    # in the thread); the Mention tag is what triggers their notification.
    reply["to"] = [remote["actor_id"]]
    reply["cc"] = [ap.AS_PUBLIC, actor_url + "/followers"]
    tag = {"type": "Mention", "href": remote["actor_id"]}
    if remote.get("handle"):
        tag["name"] = "@" + remote["handle"]
    reply["tag"] = [tag]
    ckey = ap.context_key(owner, repo, "issue", issue_number)
    rec = {"note": reply,
           "context": {"owner": owner, "repo": repo, "kind": "issue",
                       "ref": str(issue_number), "key": ckey},
           "media": []}
    await d1_run(
        env,
        "INSERT OR IGNORE INTO ap_objects "
        "(object_uuid, actor_bi, context_bi, data,"
        " published) VALUES (?,?,?,?,?)",
        object_uuid, await _ap_actor_bi(env, AP_ACTOR_REPO, handle),
        await blind_index(env, "ap-context:" + ckey),
        await encrypt_row(env, rec), now)
    inbox = ((remote.get("inbox") or "").strip()
             or (remote.get("shared_inbox") or "").strip())
    if not inbox:
        return False
    out_data = await encrypt_row(env, {
        "body": json.dumps(ap.create_activity(reply)),
        "actorKind": AP_ACTOR_REPO, "actorHandle": handle,
        "actorUrl": actor_url})
    if dedupe_id:
        dedupe_bi = await blind_index(
            env,
            "ap-mention-followup:" + str(dedupe_id) + ":" + inbox,
        )
        await d1_run(
            env,
            "INSERT OR IGNORE INTO ap_outbox "
            "(inbox, data, attempts, next_ts, created_at, dedupe_bi)"
            " VALUES (?,?,0,?,?,?)",
            inbox, out_data, now, now, dedupe_bi)
    else:
        await d1_run(
            env,
            "INSERT INTO ap_outbox "
            "(inbox, data, attempts, next_ts, created_at)"
            " VALUES (?,?,0,?,?)",
            inbox, out_data, now, now)
    await _ap_drain_outbox(env, 1)
    return True
async def _ap_handle_repo_mention(env, request, note, remote, owner, repo):
    """A verified fediverse post that @-mentions one of our repo actors
    ("@owner.repo@host this button is broken ..."). It becomes sanitized
    public activity in World's manual review feed. It never runs AI, enters an
    issue inbox, or publishes a reply here. Only a later owner-authorized
    preview + explicit create action may queue it, and even then it stays
    pending until the owner node confirms the materialized issue number."""
    dropped = json_response({"ok": True}, status=202)
    data_owner = await _ap_org_alias_owner(env, owner, repo)
    if await _repo_is_private(env, data_owner, repo):
        return dropped
    settings = await _ap_repo_settings_get(env, data_owner, repo)
    if not settings["federate"] or not settings["acceptComments"]:
        return dropped
    # A legacy deployment may already have auto-filed this note. Preserve that
    # historical dedupe marker so a redelivery cannot reintroduce it as a new
    # manual-review candidate.
    mention_bi = await blind_index(env, "ap-mention:" + note["id"])
    seen = await d1_first(
        env, "SELECT ts FROM ap_mentions WHERE remote_id_bi=?", mention_bi)
    if seen:
        return dropped
    text = ap.sanitize_remote_html(note["content"])
    text = re.sub(
        r"(?i)@" + re.escape(ap.repo_handle(owner, repo))
        + r"(?:@[A-Za-z0-9.-]+)?", " ", text)
    text = "\n".join(
        re.sub(r"[ \t]+", " ", line).strip() for line in text.split("\n"))
    text = text.strip()
    if not text:
        return dropped
    public_note = dict(note)
    public_note["content"] = text
    await fediverse_mentions_api.record_verified(
        _WorldCommunityRuntime(env, request),
        note=public_note,
        remote=remote,
        actor_owner=owner,
        data_owner=data_owner,
        repo=repo,
        kind="mention",
        signature_verified=True,
        public_activity=True,
        public_repository=True,
    )
    return dropped
async def _ap_handle_delete(env, activity, remote=None):
    object_id = ap.activity_object_id(activity.get("object"))
    if object_id:
        row, existing, _ = await _ap_comment_by_remote_id(env, object_id)
        if not row or not existing:
            return json_response({"ok": True}, status=202)
        remote_actor_id = (remote or {}).get("actor_id", "")
        if remote_actor_id.rstrip("/") != \
                str(existing.get("authorId", "")).rstrip("/"):
            return json_response({"error": "author_mismatch"}, status=401)
        result = ap_threads.apply_activity(
            {object_id: existing}, activity, remote_actor=remote,
            context=existing.get("context"), received_ms=int(Date.now()))
        if not result.get("ok"):
            return json_response({"error": result.get("reason")},
                                 status=401)
        rec = result["record"]
        rec["provenance"]["deliverySignatureVerified"] = True
        rec["provenance"]["nativeSignatureVerified"] = False
        await d1_run(
            env,
            "UPDATE ap_comments SET lifecycle=?, data=?"
            " WHERE remote_id_bi=?",
            rec["lifecycle"], await encrypt_row(env, rec),
            row.get("remote_id_bi"))
    return json_response({"ok": True}, status=202)
async def ap_inbox_handler(env, request):
    # Shared + per-actor ActivityPub inbox: verify the draft-cavage HTTP
    # signature (keyId fetched from the remote server) and the body digest,
    # then dispatch on activity type. Processing is synchronous — no queues on
    # the free plan — and every path is a handful of D1 statements.
    if method_name(request) != "POST":
        return json_response({"error": "method_not_allowed"}, status=405)
    try:
        body = await bounded_request_text(request, AP_MAX_INBOX_BYTES)
    except RequestBodyTooLarge:
        return json_response({"error": "too_large"}, status=413)
    except Exception:
        return json_response({"error": "invalid_body"}, status=400)
    body_bytes = body.encode()
    try:
        activity = json.loads(body)
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    if not isinstance(activity, dict):
        return json_response({"error": "invalid_activity"}, status=400)
    activity_type = str(activity.get("type", "") or "")
    actor_id = ap.public_federation_url(
        ap.activity_object_id(activity.get("actor")).rstrip("/"))
    origin = _ap_origin(env, request)
    if not actor_id or actor_id.startswith(origin + "/"):
        return json_response({"error": "invalid_actor"}, status=401)
    object_id = ap.activity_object_id(activity.get("object")).rstrip("/")

    # Classify the large, intentionally ignored part of the federation stream
    # before initializing D1.  ensure_schema() is cheap after one request in a
    # warm isolate, but its cold path imports schema.py and constructs the DDL
    # fingerprint. Combining that CPU with the request-lazy app runtime,
    # fediverse module and ActivityStreams parser can exhaust an ingress
    # Durable Object's CPU slice. Mastodon broadcasts Update(Person) profile
    # changes to every server it has reached; those cannot affect ForkMesh and
    # are the common cold-inbox trigger. The same is true of Like/Announce/
    # Accept traffic. Validate the public actor URL above, then acknowledge
    # these no-state activities without D1, signature crypto or remote fetches.
    if activity_type not in (
            "Follow", "Undo", "Create", "Update", "Delete", "Remove"):
        return json_response({"ok": True}, status=202)
    if activity_type == "Update":
        note = ap.note_essentials(activity.get("object"))
        if not note:
            return json_response({"ok": True}, status=202)
    elif activity_type == "Create":
        # Most Create traffic is neither a reply to one of our objects nor a
        # mention of a repository actor. Both tests are pure; only a possible
        # nested remote reply needs storage and stays on the initialized path.
        note = ap.note_essentials(activity.get("object"))
        if not note:
            return json_response({"ok": True}, status=202)
        if (not ap.parse_local_object_url(origin, note["inReplyTo"])
                and not _ap_note_repo_mention(origin, note)
                and not note["inReplyTo"].startswith("https://")):
            return json_response({"ok": True}, status=202)

    # Mastodon broadcasts actor self-deletes to every instance the account has
    # ever reached. Almost all are unknown here. Keep that cold-ingress path to
    # one indexed query and avoid schema/settings initialization entirely; the
    # full path remains mandatory for a known follower or cached actor because
    # only a verified signature may remove that durable state.
    known_self_delete = False
    if activity_type == "Delete" and object_id == actor_id:
        try:
            known = await d1_first(
                env,
                "SELECT 1 AS known WHERE EXISTS ("
                " SELECT 1 FROM ap_remote_actors WHERE actor_id=?"
                ") OR EXISTS ("
                " SELECT 1 FROM ap_followers WHERE follower_id=?"
                ") LIMIT 1",
                actor_id, actor_id)
            if not known:
                return json_response({"ok": True}, status=202)
            known_self_delete = True
        except Exception as error:
            # A genuinely fresh self-hosted database still needs the normal
            # compatibility schema path. Transient D1 errors must propagate;
            # acknowledging them would silently retain a known follower.
            if "no such table" not in str(error).lower():
                raise

    # From here onward the activity can read or mutate federation state.
    await ensure_schema(env)
    if not await _ap_enabled(env):
        return _ap_disabled_response()
    if await _ap_domain_blocked(env, urlparse(actor_id).netloc):
        return json_response({"error": "domain_blocked"}, status=403)

    # Unknown actor self-deletes are broadcast widely and can be dropped
    # without a remote fetch.  Known actors continue through signature
    # verification: accepting an unsigned Delete would let anyone remove a
    # legitimate follower edge by naming that actor in a forged body.
    # Mastodon broadcasts every account deletion to every instance it has ever
    # seen, so nearly all of these are for actors we don't know: check first
    # (two indexed reads) instead of always issuing two D1 writes.
    if (activity_type == "Delete" and object_id == actor_id
            and not known_self_delete):
        known = await d1_first(
            env,
            "SELECT 1 AS known WHERE EXISTS ("
            " SELECT 1 FROM ap_remote_actors WHERE actor_id=?"
            ") OR EXISTS ("
            " SELECT 1 FROM ap_followers WHERE follower_id=?"
            ") LIMIT 1",
            actor_id, actor_id)
        if not known:
            return json_response({"ok": True}, status=202)

    # Anything we'd acknowledge-and-drop after verifying (Like/Announce/
    # Accept/...) is dropped before the signature dance instead: the
    # verify path costs a signed GET back to the sender's server (plus D1
    # reads/writes to cache its actor), so fediverse-wide Like/boost floods
    # both melted our origin AND hammered Mastodon with actor refetches —
    # the "too many requests" loop of issue #416.
    if activity_type == "Create":
        # Two Note shapes matter: replies to our own /ap/o/<uuid> objects
        # (federated comments) and posts whose Mention tags point at one of
        # our repo actors (candidate issue requests). Everything else is
        # dropped here, before the remote-actor fetch (the handler re-checks
        # after verification). A nested Lemmy reply also qualifies when its
        # parent is an already-stored remote reply; that costs one indexed D1
        # read but avoids verifying unrelated fediverse traffic.
        qualifies = bool(note and (
            ap.parse_local_object_url(origin, note["inReplyTo"])
            or _ap_note_repo_mention(origin, note)))
        if note and not qualifies and note["inReplyTo"].startswith("https://"):
            parent_row, _, _ = await _ap_comment_by_remote_id(
                env, note["inReplyTo"])
            qualifies = bool(parent_row)
        if not qualifies:
            return json_response({"ok": True}, status=202)
    elif activity_type == "Update":
        target_row, _, _ = await _ap_comment_by_remote_id(env, note["id"])
        if not target_row:
            return json_response({"ok": True}, status=202)
    elif activity_type == "Remove":
        target_id = ap.activity_object_id(activity.get("object"))
        target_row, _, _ = await _ap_comment_by_remote_id(env, target_id)
        if not target_row:
            return json_response({"ok": True}, status=202)

    parsed_sig = ap.parse_signature_header(
        request.headers.get("signature") or "")
    if not parsed_sig:
        return json_response({"error": "signature_required"}, status=401)
    # The signature must cover the request target and the body digest, or it
    # proves nothing about this delivery.
    signed_headers = set(parsed_sig["headers"])
    if ("(request-target)" not in signed_headers
            or "host" not in signed_headers
            or "digest" not in signed_headers):
        return json_response({"error": "weak_signature"}, status=401)
    key_url = ap.public_federation_url(
        parsed_sig["keyId"].split("#")[0].rstrip("/"))
    if (not key_url
            or urlparse(key_url).netloc != urlparse(actor_id).netloc):
        return json_response({"error": "key_actor_mismatch"}, status=401)
    if not ap.digest_matches(request.headers.get("digest") or "", body_bytes):
        return json_response({"error": "digest_mismatch"}, status=401)
    now = int(Date.now())
    freshness_verified = False
    for date_name in ("date", "x-date"):
        if date_name not in signed_headers:
            continue
        sent = ap.parse_http_date_ms(request.headers.get(date_name) or "")
        if not sent or abs(now - sent) > AP_DATE_SKEW_MS:
            return json_response({"error": "date_skew"}, status=401)
        freshness_verified = True
    if "(created)" in signed_headers:
        created = parsed_sig.get("created")
        if (created is None
                or abs(now - created * 1000) > AP_DATE_SKEW_MS):
            return json_response({"error": "created_skew"}, status=401)
        freshness_verified = True
    if not freshness_verified:
        return json_response({"error": "date_required"}, status=401)
    if "(expires)" in signed_headers:
        expires = parsed_sig.get("expires")
        if expires is None or expires * 1000 < now:
            return json_response({"error": "signature_expired"}, status=401)

    url = urlparse(request.url)
    path = url.path + (("?" + url.query) if url.query else "")
    headers_map = {}
    for name in parsed_sig["headers"]:
        if name.startswith("("):
            continue
        value = request.headers.get(name)
        if value is None:
            return json_response({"error": "missing_header"}, status=401)
        headers_map[name] = value
    base = ap.signing_string(
        "POST", path, headers_map, parsed_sig["headers"],
        created=parsed_sig.get("created"), expires=parsed_sig.get("expires"))
    if base is None:
        return json_response({"error": "bad_signature_params"}, status=401)
    remote = await _ap_remote_actor(env, key_url)
    verified = bool(remote and remote.get("pubkey_pem")
                    and await _ap_rsa_verify(
                        remote["pubkey_pem"], parsed_sig["signature"], base))
    if not verified:
        # The remote key may have rotated since we cached it — refresh once.
        remote = await _ap_remote_actor(env, key_url, force_refresh=True)
        verified = bool(remote and remote.get("pubkey_pem")
                        and await _ap_rsa_verify(
                            remote["pubkey_pem"], parsed_sig["signature"],
                            base))
    if not verified:
        return json_response({"error": "bad_signature"}, status=401)
    key_owner = str(remote.get("actor_id") or "").rstrip("/")
    if key_owner != actor_id and not key_owner.startswith(actor_id + "/"):
        return json_response({"error": "actor_mismatch"}, status=401)

    if activity_type == "Delete" and object_id == actor_id:
        await _ap_forget_remote(env, actor_id)
        return json_response({"ok": True}, status=202)

    if activity_type == "Follow":
        return await _ap_handle_follow(env, request, activity, remote)
    if activity_type == "Undo":
        return await _ap_handle_undo(env, request, activity)
    if activity_type == "Create":
        return await _ap_handle_create(env, request, activity, remote)
    if activity_type == "Update":
        return await _ap_handle_update(env, activity, remote)
    if activity_type == "Delete":
        return await _ap_handle_delete(env, activity, remote)
    if activity_type == "Remove":
        return await _ap_handle_remove(env, activity, remote)
    # Like/Announce/Accept/... are acknowledged and dropped above.
    return json_response({"ok": True}, status=202)
async def _ap_publish_authorized(env, request, author):
    """Authenticate a manual post's user actor without trusting its payload.

    The query names the account whose device produced the signature. A linked
    node may speak as its owning user; every other mismatch is rejected.
    """
    params = parse_qs(urlparse(request.url).query)
    signer = clean_string(params.get("owner", [""])[0], MAX_NODE_NAME).lower()
    author = clean_string(author or "", MAX_NODE_NAME).lower()
    if (not valid_node_name(signer) or not valid_node_name(author)
            or not await _authorized_owner_signing_key(
                env, request, signer)):
        return False
    return signer == author or await _account_owns_node(env, author, signer)


async def _ap_public_repo_owner(env, request, owner, repo):
    try:
        match = REPO_API_PREFIX_RE.match(
            urlparse(str(getattr(request, "url", "") or "")).path)
    except Exception:
        match = None
    owner_l = str(owner or "").strip().lower()
    if not match:
        return owner_l
    alias = (safe_segment(match.group(1)) or "").lower()
    if (not alias or alias == owner_l
            or (safe_segment(match.group(2)) or "").lower()
            != str(repo or "").strip().lower()):
        return owner_l
    node = await _org_repo_node(env, alias, repo)
    if str(node or "").strip().lower() != owner_l:
        return owner_l
    return alias


async def ap_publish_handler(env, request, owner, repo):
    # Canonical repository announcements still require the repository-aware
    # owner proof. An explicit manual prompt may instead authenticate its own
    # user actor; that narrower path cannot publish as the repository actor or
    # enqueue an automatic repository digest.
    if method_name(request) != "POST":
        return json_response({"error": "method_not_allowed"}, status=405)
    await ensure_schema(env)
    privacy_reader = globals().get("_repo_is_private")
    if callable(privacy_reader) and await privacy_reader(env, owner, repo):
        return json_response({"error": "not_found"}, status=404)
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    kind = clean_string(data.get("kind", ""), 20).lower()
    manual = data.get("manual") is True
    announcement = kind == AP_ANNOUNCEMENT_KIND
    if kind not in AP_FEDI_KINDS and not (announcement and manual):
        return json_response({"error": "invalid_kind"}, status=400)
    default_type = "publish" if kind == "release" else "open"
    event_type = clean_string(
        data.get("eventType", "") or default_type, 20).lower()
    ref = clean_string(
        str(data.get("ref", "") or data.get("number", "")
            or data.get("tag", "")), 80)
    author = clean_string(
        data.get("author", "") or owner, MAX_NODE_NAME).lower()
    link_url = None
    if announcement:
        link_url = clean_string(data.get("url", ""), 512).strip()
        if not link_url.startswith(_ap_origin(env, request) + "/"):
            return json_response({"error": "invalid_url"}, status=400)
    repo_authorized = await _authorize_repo_inbox_owner(
        env, request, owner, repo)
    author_authorized = (
        manual and await _ap_publish_authorized(env, request, author))
    if not repo_authorized and not author_authorized:
        return json_response({"error": "unauthorized"}, status=401)
    public_owner = await _ap_public_repo_owner(env, request, owner, repo)
    await _ap_publish_repo_event(
        env, request, owner, repo, kind, event_type, ref,
        clean_string(data.get("title", ""), 240),
        clean_string(data.get("body", ""), 4000),
        author, manual=manual, include_repo_actor=repo_authorized,
        public_owner=public_owner, link_url=link_url)
    # Outbound webhooks (adhoc #207). This route is the one place the relay
    # learns that a release was cut: the manifest is signed and tagged on the
    # owner's node and the desktop announces it here right after `git tag`
    # succeeds (MainWindowReleases.cpp: announceReleaseOnFediverse). The
    # announcement only proves a release when the repository-aware owner proof
    # carried it, so a manual user-actor post cannot trigger somebody's CI.
    if kind == "release" and event_type == "publish" and repo_authorized:
        await queue_repo_webhook_event(
            env, owner, repo, "release.published",
            {
                "tag": ref,
                "name": clean_string(data.get("title", ""), 240),
                "notes": clean_string(data.get("body", ""), 4000),
                "author": author,
            },
            ref)
        await provider_mirror_note_event(
            env, owner, repo, "release.published",
            {
                "tag": ref,
                "name": clean_string(data.get("title", ""), 240),
                "notes": clean_string(data.get("body", ""), 4000),
                "author": author,
            })
        # Activity ping (adhoc #295). notify_release_published has existed
        # since the release_published kind did, but nothing ever called it —
        # so a cut release reached a registered webhook endpoint and no
        # inbox. Best effort: the announcement is already published.
        try:
            await notify_release_published(
                env, owner, repo,
                {"tag": ref,
                 "name": clean_string(data.get("title", ""), 240),
                 "created_by": author})
        except Exception:
            pass
    return json_response({
        "ok": True,
        "mode": "manual_immediate" if manual else "daily_digest",
    }, status=202)
async def ap_digest_handler(env, request, owner, repo):
    """Owner-only preview of the encrypted automatic-update queue."""
    await ensure_schema(env)
    method = method_name(request)
    if method not in ("GET", "POST"):
        return json_response({"error": "method_not_allowed"}, status=405)
    if await _repo_is_private(env, owner, repo):
        return json_response({"error": "not_found"}, status=404)
    data = {}
    if method == "POST":
        try:
            data = await bounded_json_request(request)
        except Exception:
            return json_response({"error": "invalid_json"}, status=400)
    if not await _authorize_repo_owner_web(env, request, owner, repo, data):
        return json_response({"error": "not_authorized"}, status=403)
    settings = await _ap_repo_settings_get(env, owner, repo)
    preview = await _ap_digest_preview(
        env, str(owner).lower() + "/" + str(repo).lower())
    preview.update({
        "ok": True,
        "enabled": bool(
            settings["federate"] and settings["broadcastEvents"]),
        "cadence": "daily",
        "minimumIntervalMs": fedi_digest.DAY_MS,
        "automatedLabel": True,
    })
    return json_response(preview, cache_control="no-store")
async def ap_posts_handler(env, request, owner, repo):
    # Repo-owner fediverse-post management (dashboard "manage posts" dropdown,
    # issue #426): list the repo actor's federated posts, or delete one. A
    # delete broadcasts a Delete(Tombstone) to every follower inbox so the post
    # disappears from Mastodon timelines, then drops the local object. POST-only
    # so the session token rides in the body, never a query string / access log.
    if method_name(request) != "POST":
        return json_response({"error": "method_not_allowed"}, status=405)
    await ensure_schema(env)
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    if not await _authorize_repo_owner_web(env, request, owner, repo, data):
        return json_response({"error": "not_authorized"}, status=403)
    action = clean_string(data.get("action", "list"), 12).lower()
    handle = ap.repo_handle(
        await _ap_public_repo_owner(env, request, owner, repo), repo.lower())
    actor_bi = await _ap_actor_bi(env, AP_ACTOR_REPO, handle)
    if action == "list":
        rows = await d1_all(
            env,
            "SELECT object_uuid, data, published FROM ap_objects"
            " WHERE actor_bi=? ORDER BY published DESC LIMIT 50", actor_bi)
        posts = []
        for row in rows or []:
            rec = await decrypt_row(env, row.get("data"))
            note = (rec or {}).get("note") or {}
            posts.append({
                "id": row.get("object_uuid"),
                "content": note.get("content") or "",  # our own safe HTML
                "url": note.get("url") or "",
                "published": int(row.get("published") or 0),
            })
        return json_response({"ok": True, "posts": posts})
    if action != "delete":
        return json_response({"error": "bad_action"}, status=400)
    object_uuid = clean_string(data.get("id", ""), 64).strip()
    if not object_uuid:
        return json_response({"error": "missing_id"}, status=400)
    # Only delete a post the repo actor actually authored: the actor_bi filter
    # stops one owner's token from reaching another actor's objects.
    row = await d1_first(
        env,
        "SELECT object_uuid FROM ap_objects WHERE object_uuid=? AND actor_bi=?",
        object_uuid, actor_bi)
    if not row:
        return json_response({"error": "not_found"}, status=404)
    # Drop the local object first: even with federation disabled or zero
    # followers the owner's intent (remove the post) is honored — /ap/o/<uuid>
    # starts 404ing and it vanishes from the repo's fediverse profile feed.
    await d1_run(env, "DELETE FROM ap_objects WHERE object_uuid=?", object_uuid)
    origin = _ap_origin(env, request)
    object_url = origin + "/ap/o/" + object_uuid
    # Drop the edge-parked Note (max-age 300) so the object 404s immediately
    # rather than lingering a cache TTL after the owner deleted it.
    await edge_cache_delete(object_url)
    actor_url = _ap_actor_url(origin, AP_ACTOR_REPO, handle)
    now = int(Date.now())
    queued = False
    if await _ap_enabled(env):
        followers = await d1_all(
            env,
            "SELECT inbox, shared_inbox FROM ap_followers WHERE actor_bi=?",
            actor_bi)
        body_str = json.dumps(ap.delete_activity(
            actor_url, object_url, actor_url + "/followers", now))
        out_data = await encrypt_row(env, {
            "body": body_str, "actorKind": AP_ACTOR_REPO,
            "actorHandle": handle, "actorUrl": actor_url})
        seen = set()
        for follower in followers or []:
            inbox = ((follower.get("shared_inbox") or "").strip()
                     or (follower.get("inbox") or "").strip())
            if not inbox or inbox in seen:
                continue
            seen.add(inbox)
            await d1_run(
                env,
                "INSERT INTO ap_outbox (inbox, data, attempts, next_ts,"
                " created_at) VALUES (?,?,0,?,?)",
                inbox, out_data, now, now)
            queued = True
        if queued:
            await _ap_drain_outbox(env, AP_IMMEDIATE_DELIVERIES)
    return json_response({"ok": True, "deleted": object_uuid,
                          "federated": queued})
