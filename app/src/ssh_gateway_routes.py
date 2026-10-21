"""ssh gateway routes routes, loaded on demand.

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


def _ssh_gateway_settings(env):
    """Return publishable gateway settings only for an operable deployment."""
    host = ssh_auth.configured_gateway_host(
        getattr(env, "SSH_GATEWAY_HOST", ""))
    token = ssh_auth.configured_gateway_token(
        getattr(env, "SSH_GATEWAY_TOKEN", ""))
    repositories = ssh_auth.parse_gateway_repository_allowlist(
        getattr(env, "SSH_GATEWAY_REPOSITORIES", ""))
    node_hosts = ssh_auth.parse_gateway_node_hosts(
        getattr(env, "SSH_GATEWAY_NODE_HOSTS", ""))
    try:
        port = int(getattr(env, "SSH_GATEWAY_PORT", 22) or 22)
    except (TypeError, ValueError):
        port = 0
    configured = bool(host and 1 <= port <= 65535 and token and repositories)
    return {
        "configured": configured,
        "host": host if configured else "",
        "port": port if configured else 0,
        "repositories": repositories if configured else {},
        "nodeHosts": node_hosts if configured else {},
    }
def _ssh_gateway_bridge(env):
    """True when the gateway relays every repository to the HTTPS routes.

    ``SSH_GATEWAY_REPOSITORIES=*``: no per-repository list to keep in step
    with hosted repositories and forks. Each authorization then also mints the
    short-lived credential the gateway relays the session with, so the push
    takes the HTTPS path's branch protection, contributor namespace, quota and
    bookkeeping instead of a second copy of them.
    """
    settings = _ssh_gateway_settings(env)
    return bool(
        settings["configured"]
        and ssh_auth.gateway_bridges_all(settings["repositories"]))


def _ssh_gateway_urls(env, owner, repo):
    """The ``git@host:owner/repo.git`` and ``ssh://`` forms, or ("", "")."""
    url = _ssh_repository_url(env, owner, repo)
    if not url:
        return "", ""
    settings = _ssh_gateway_settings(env)
    host = settings["nodeHosts"].get(str(owner or "").strip().lower())
    return ssh_auth.scp_repository_url(
        host or settings["host"], settings["port"], owner, repo), url


def _ssh_gateway_repository_access(env, owner, repo):
    settings = _ssh_gateway_settings(env)
    if not settings["configured"]:
        return ""
    return ssh_auth.gateway_repository_access(
        settings["repositories"], owner, repo)
def _ssh_repository_url(env, owner, repo):
    settings = _ssh_gateway_settings(env)
    if (
        not settings["configured"]
        or not ssh_auth.gateway_repository_access(
            settings["repositories"], owner, repo)
    ):
        return ""
    host = settings["nodeHosts"].get(str(owner or "").strip().lower())
    return ssh_auth.ssh_repository_url(
        host or settings["host"], settings["port"], owner, repo)
def _ssh_alias_repository_url(env, alias_owner, backing_owner, repo):
    """Return an org-facing SSH URL backed by an allowlisted node repo.

    The organization alias is an explicit read route on a mirror gateway. Its
    backing user/source identity is checked by the authorization endpoint but
    is never substituted into the public URL or the node-local repository
    path. This keeps users, nodes, and public organization repositories as
    separate identities.
    """
    settings = _ssh_gateway_settings(env)
    if (
        not settings["configured"]
        or not ssh_auth.gateway_repository_access(
            settings["repositories"], alias_owner, repo)
    ):
        return ""
    host = (
        settings["nodeHosts"].get(
            str(alias_owner or "").strip().lower())
        or settings["nodeHosts"].get(
            str(backing_owner or "").strip().lower())
    )
    return ssh_auth.ssh_repository_url(
        host or settings["host"], settings["port"], alias_owner, repo)
async def _ssh_json_body(request, max_bytes=32 * 1024):
    try:
        announced = int(request.headers.get("content-length") or 0)
    except (TypeError, ValueError):
        return None, "invalid_content_length"
    if announced < 0 or announced > max_bytes:
        return None, "payload_too_large"
    try:
        raw = await request.text()
    except Exception:
        return None, "invalid_json"
    if len(str(raw or "").encode("utf-8")) > max_bytes:
        return None, "payload_too_large"
    try:
        data = json.loads(raw)
    except Exception:
        return None, "invalid_json"
    if not isinstance(data, dict):
        return None, "invalid_json"
    return data, ""
def _ssh_key_public_projection(row, data=None):
    source = data if isinstance(data, dict) else {}
    return {
        "id": str(row.get("key_id") or ""),
        "label": clean_string(source.get("label", ""), ssh_auth.MAX_KEY_LABEL),
        "keyType": str(row.get("key_type") or ""),
        "fingerprint": str(row.get("fingerprint") or ""),
        "createdAt": int(row.get("created_at") or 0),
        "lastUsedAt": int(row.get("last_used_at") or 0),
    }
async def ssh_keys_handler(env, request, key_id=""):
    """Register, list, and revoke the signed-in user's SSH public keys."""
    await ensure_schema(env)
    method = method_name(request)
    data = {}
    if method == "POST":
        data, error = await _ssh_json_body(request)
        if error:
            return json_response({"error": error}, status=413 if (
                error == "payload_too_large") else 400)
    account_bi, account_rec = await _account_session_record(
        env, request, data)
    account = clean_string(
        (account_rec or {}).get("name", ""), MAX_NODE_NAME).lower()
    if (
        not account_bi
        or not account
        or _account_kind(account_rec) != "user"
    ):
        return json_response(
            {"error": "invalid_session"}, status=401,
            cache_control="no-store")

    if method == "GET" and not key_id:
        rows = await d1_all(
            env,
            "SELECT key_id,key_type,fingerprint,data,created_at,last_used_at "
            "FROM account_ssh_keys WHERE account_bi=? AND revoked_at=0 "
            "ORDER BY created_at DESC",
            account_bi,
        )
        keys = []
        for row in rows or []:
            try:
                key_data = await decrypt_row(env, row.get("data"))
            except Exception:
                key_data = None
            if not isinstance(key_data, dict):
                continue
            keys.append(_ssh_key_public_projection(row, key_data))
        gateway = _ssh_gateway_settings(env)
        bridge = bool(
            gateway["configured"]
            and ssh_auth.gateway_bridges_all(gateway["repositories"]))
        # A placeholder address for the settings page's instructions; the
        # repository page shows each repository's own.
        example_scp, example_url = "", ""
        if gateway["configured"]:
            example_url = ssh_auth.ssh_repository_url(
                gateway["host"], gateway["port"], "owner", "repository")
            example_scp = ssh_auth.scp_repository_url(
                gateway["host"], gateway["port"], "owner", "repository")
        return json_response({
            "ok": True,
            "keys": keys,
            "maximumKeys": ssh_auth.MAX_KEYS_PER_ACCOUNT,
            "gatewayConfigured": gateway["configured"],
            "gatewayHost": gateway["host"],
            "gatewayPort": gateway["port"],
            "gatewayAllRepositories": bridge,
            "gatewayExampleUrl": example_scp or example_url,
            "gatewayExampleSshUrl": example_url,
            "account": account,
            "privateKeysStored": False,
            "notice": (
                "ForkMesh stores encrypted public keys only. Keep every SSH "
                "private key and passphrase on your own device."
            ),
        }, cache_control="no-store")

    if method == "POST" and not key_id:
        try:
            parsed = ssh_auth.parse_public_key(data.get("publicKey", ""))
        except ssh_auth.SshPublicKeyError as error:
            return json_response({"error": str(error)}, status=400)
        label = ssh_auth.clean_key_label(
            data.get("label", ""), parsed.get("comment", ""))
        key_bi = await blind_index(
            env, "ssh-public-key:" + parsed["publicKey"])
        existing = await d1_first(
            env,
            "SELECT account_bi,revoked_at FROM account_ssh_keys "
            "WHERE key_bi=?",
            key_bi,
        )
        if existing:
            # Do not disclose whether another account owns the same key or
            # whether it was revoked; either case requires a fresh keypair.
            return json_response(
                {"error": "ssh_key_already_registered"}, status=409,
                cache_control="no-store")
        count = await d1_first(
            env,
            "SELECT COUNT(*) AS n FROM account_ssh_keys "
            "WHERE account_bi=? AND revoked_at=0",
            account_bi,
        )
        if int((count or {}).get("n") or 0) >= ssh_auth.MAX_KEYS_PER_ACCOUNT:
            return json_response(
                {"error": "ssh_key_limit_reached"}, status=429,
                cache_control="no-store")
        key_id = "sk_" + _b64url_encode(_random_bytes(18))
        now = int(Date.now())
        encrypted = await encrypt_row(env, {
            "keyId": key_id,
            "account": account,
            "publicKey": parsed["publicKey"],
            "label": label,
            "createdAt": now,
        })
        try:
            await d1_run(
                env,
                "INSERT INTO account_ssh_keys "
                "(key_id,account_bi,key_bi,key_type,fingerprint,data,"
                "created_at,last_used_at,revoked_at) "
                "VALUES (?,?,?,?,?,?,?,0,0)",
                key_id, account_bi, key_bi, parsed["keyType"],
                parsed["fingerprint"], encrypted, now,
            )
        except Exception:
            await _audit_sensitive_action(
                env, account, "ssh_key.register", "ssh_public_key",
                key_id, "failed", {"reason": "storage_error"})
            raise
        await _audit_sensitive_action(
            env, account, "ssh_key.register", "ssh_public_key",
            key_id, "success", {"keyType": parsed["keyType"]})
        return json_response({
            "ok": True,
            "key": {
                "id": key_id,
                "label": label,
                "keyType": parsed["keyType"],
                "fingerprint": parsed["fingerprint"],
                "createdAt": now,
                "lastUsedAt": 0,
            },
            "privateKeysStored": False,
        }, status=201, cache_control="no-store")

    if method == "DELETE" and key_id:
        key_id = clean_string(key_id, 96)
        if not re.fullmatch(r"sk_[A-Za-z0-9_-]{16,80}", key_id):
            return json_response({"error": "not_found"}, status=404)
        row = await d1_first(
            env,
            "SELECT key_id FROM account_ssh_keys "
            "WHERE key_id=? AND account_bi=? AND revoked_at=0",
            key_id, account_bi,
        )
        if not row:
            return json_response({"error": "not_found"}, status=404)
        now = int(Date.now())
        try:
            await d1_run(
                env,
                "UPDATE account_ssh_keys SET revoked_at=? "
                "WHERE key_id=? AND account_bi=? AND revoked_at=0",
                now, key_id, account_bi,
            )
        except Exception:
            await _audit_sensitive_action(
                env, account, "ssh_key.revoke", "ssh_public_key",
                key_id, "failed", {"reason": "storage_error"})
            raise
        await _audit_sensitive_action(
            env, account, "ssh_key.revoke", "ssh_public_key",
            key_id, "success")
        return json_response(
            {"ok": True, "revoked": True, "id": key_id},
            cache_control="no-store")

    return json_response({"error": "method_not_allowed"}, status=405)
def _ssh_gateway_tokens(env):
    """The gateway bearer tokens accepted now.

    ``SSH_GATEWAY_TOKEN`` plus, during a rotation, ``SSH_GATEWAY_TOKEN_NEXT``:
    set the new token as NEXT, move the gateway host to it, then promote it
    to SSH_GATEWAY_TOKEN and drop NEXT. Neither step interrupts a gateway.
    """
    tokens = []
    for name in ("SSH_GATEWAY_TOKEN", "SSH_GATEWAY_TOKEN_NEXT"):
        token = ssh_auth.configured_gateway_token(getattr(env, name, ""))
        if token:
            tokens.append(token)
    return tokens


def _ssh_gateway_address_allowed(env, request):
    """Optional pin of the gateway host's address(es).

    ``SSH_GATEWAY_ALLOWED_ADDRESSES`` is a comma-separated list of IP
    addresses or networks. When it is set, an authorize call must come from
    one of them as seen by the edge (``cf-connecting-ip``, set by the CDN or
    by the instance's own front end). Unset, the bearer token alone decides;
    a malformed list admits nobody rather than everybody.
    """
    import ipaddress
    raw = str(getattr(env, "SSH_GATEWAY_ALLOWED_ADDRESSES", "") or "").strip()
    if not raw:
        return True
    networks = []
    for item in raw.split(","):
        item = item.strip()
        if not item:
            continue
        try:
            networks.append(ipaddress.ip_network(item, strict=False))
        except ValueError:
            return False
    try:
        address = ipaddress.ip_address(str(
            request.headers.get("cf-connecting-ip") or "").strip().strip("[]"))
    except ValueError:
        return False
    return any(address in network for network in networks)


def _ssh_gateway_token_ok(env, request):
    expected = _ssh_gateway_tokens(env)
    if not expected:
        return False
    supplied = str(request.headers.get("authorization") or "")
    if not supplied.lower().startswith("bearer "):
        return False
    supplied = supplied[7:].strip()
    # Compare against every configured token so the time taken does not say
    # which one matched.
    matched = False
    for token in expected:
        matched = hmac.compare_digest(supplied, token) or matched
    return matched and _ssh_gateway_address_allowed(env, request)
async def _ssh_key_gateway_record(env, key_id="", public_key=""):
    row = None
    if key_id:
        row = await d1_first(
            env,
            "SELECT key_id,account_bi,key_type,fingerprint,data,created_at,"
            "last_used_at FROM account_ssh_keys "
            "WHERE key_id=? AND revoked_at=0",
            key_id,
        )
    elif public_key:
        try:
            parsed = ssh_auth.parse_public_key(public_key)
        except ssh_auth.SshPublicKeyError:
            return None, None
        key_bi = await blind_index(
            env, "ssh-public-key:" + parsed["publicKey"])
        row = await d1_first(
            env,
            "SELECT key_id,account_bi,key_type,fingerprint,data,created_at,"
            "last_used_at FROM account_ssh_keys "
            "WHERE key_bi=? AND revoked_at=0",
            key_bi,
        )
    if not row:
        return None, None
    try:
        data = await decrypt_row(env, row.get("data"))
        registered = ssh_auth.parse_public_key(data.get("publicKey", ""))
    except Exception:
        return None, None
    if public_key:
        try:
            supplied = ssh_auth.parse_public_key(public_key)
        except ssh_auth.SshPublicKeyError:
            return None, None
        if not hmac.compare_digest(
                registered["publicKey"], supplied["publicKey"]):
            return None, None
    # Resolve the current account name from the indexed account row. The
    # encrypted key envelope deliberately preserves its registration-time
    # metadata, while account rename updates this row's account_bi.
    account_rec = await _account_identity_rec_by_bi(
        env, row.get("account_bi"))
    account = clean_string(
        (account_rec or {}).get("name", ""), MAX_NODE_NAME).lower()
    if (
        not account
        or not account_rec
        or account_rec.get("status") != "active"
        or _account_kind(account_rec) != "user"
    ):
        return None, None
    # Return a copy with the current principal. Registration-time encrypted
    # metadata is useful evidence but must never control authorization after an
    # account rename.
    current = dict(data)
    current["account"] = account
    return row, current
async def _ssh_org_read_allowed(env, owner, repo, reader):
    try:
        rows = await d1_all(
            env, "SELECT org_bi FROM org_repos WHERE node_owner=? AND repo=?",
            owner.lower(), repo.lower())
        for row in rows or []:
            permission = await _org_permission(
                env, str(row.get("org_bi") or ""), reader)
            if team_permission_rank(permission) >= team_permission_rank("read"):
                return True
    except Exception:
        return False
    return False
async def ssh_gateway_authorize_handler(env, request):
    """Authorize a configured node-side SSH forced-command gateway.

    This is an HTTPS control-plane call. The raw SSH connection and Git process
    remain on the independently operated gateway host.
    """
    if method_name(request) != "POST":
        return json_response({"error": "method_not_allowed"}, status=405)
    if not _ssh_gateway_token_ok(env, request):
        return json_response(
            {"error": "unauthorized"}, status=401,
            cache_control="no-store")
    await ensure_schema(env)
    data, error = await _ssh_json_body(request)
    if error:
        return json_response(
            {"error": error},
            status=413 if error == "payload_too_large" else 400,
            cache_control="no-store")
    action = clean_string(data.get("action", ""), 20).lower()
    request_id = clean_string(data.get("requestId", ""), 40)
    if not re.fullmatch(r"[A-Za-z0-9_-]{24}", request_id):
        return json_response(
            {"authorized": False}, status=403,
            cache_control="no-store")
    if action == "lookup":
        row, _key_data = await _ssh_key_gateway_record(
            env, public_key=data.get("publicKey", ""))
        if not row:
            return json_response(
                {"authorized": False}, status=403,
                cache_control="no-store")
        return json_response({
            "ok": True,
            "authorized": True,
            "keyId": row.get("key_id"),
            "requestId": request_id,
        }, cache_control="no-store")
    if action != "authorize":
        return json_response({"error": "bad_action"}, status=400)

    key_id = clean_string(data.get("keyId", ""), 96)
    owner = safe_segment(data.get("owner", "")) if data.get("owner") else ""
    repo = safe_segment(data.get("repository", ""))
    operation = clean_string(data.get("operation", ""), 40)
    bridge = _ssh_gateway_bridge(env)
    if not owner and bridge and repo:
        # A short address (git@host:Repository.git) names a repository of the
        # instance's short-address owner, as /Repository does on the web.
        try:
            setting = await _repo_short_setting_row(env)
        except Exception:
            setting = None
        if isinstance(setting, dict) and setting.get("short"):
            owner = safe_segment(setting.get("owner") or "")
    if (
        not re.fullmatch(r"sk_[A-Za-z0-9_-]{16,80}", key_id)
        or not owner
        or not repo
        or operation not in ("git-upload-pack", "git-receive-pack")
    ):
        return json_response(
            {"authorized": False}, status=403,
            cache_control="no-store")
    row, key_data = await _ssh_key_gateway_record(env, key_id=key_id)
    if not row or not key_data:
        return json_response(
            {"authorized": False}, status=403,
            cache_control="no-store")
    account = clean_string(
        key_data.get("account", ""), MAX_NODE_NAME).lower()

    # An SSH URL may use an organization alias. Resolve it to the canonical
    # node namespace before repository lookup, permission enforcement, and the
    # path returned to the gateway.
    canonical_owner = await _org_repo_node(env, owner, repo) or owner
    is_org_alias = canonical_owner != owner
    route_owner = owner if is_org_alias else canonical_owner
    gateway_access = _ssh_gateway_repository_access(
        env, route_owner, repo)
    repo_bi = await blind_index(env, canonical_owner + "/" + repo)
    repo_row = await d1_first(
        env,
        "SELECT is_private,data FROM repositories WHERE key_bi=?",
        repo_bi,
    )
    record = None
    if repo_row:
        try:
            record = await decrypt_row(env, repo_row.get("data"))
        except Exception:
            record = None
    authoritative_private = bool(
        int((repo_row or {}).get("is_private") or 0))
    public_record = bool(
        isinstance(record, dict)
        and record.get("visibility") == "public")
    if (
        not gateway_access
        or (
            operation == "git-receive-pack"
            and gateway_access != "read-write"
        )
        or not _catalog_record_matches_identity(
            record, canonical_owner, repo)
    ):
        allowed = False
    else:
        owns_repo = await _account_owns_node(
            env, account, canonical_owner)
        if operation == "git-receive-pack" and bridge:
            # The relayed HTTPS push applies branch protection rule by rule
            # and confines a contributor to refs/heads/pr/<account>/, so
            # neither needs a blanket refusal here.
            allowed = bool(
                owns_repo
                or await _org_write_allowed(
                    env, canonical_owner, repo, account))
            if not allowed:
                contributor = await contributor_push_allowed(
                    env, canonical_owner, repo, account,
                    alias=owner if is_org_alias else "")
                allowed = bool(contributor)
        elif operation == "git-receive-pack":
            allowed = bool(
                owns_repo
                or await _org_write_allowed(
                    env, canonical_owner, repo, account)
            ) and not await _branch_protection.is_protected(
                env, canonical_owner, repo)
        else:
            allowed = bool(
                (public_record and not authoritative_private)
                or owns_repo
                or await _repo_shared_with(
                    env, canonical_owner, repo, account)
                or await _ssh_org_read_allowed(
                    env, canonical_owner, repo, account)
            )
    if not allowed:
        if operation == "git-receive-pack":
            await _audit_sensitive_action(
                env, account, "ssh.git_authorize", "repository",
                canonical_owner + "/" + repo, "denied",
                {"operation": operation})
        return json_response(
            {"authorized": False}, status=403,
            cache_control="no-store")

    now = int(Date.now())
    await d1_run(
        env, "UPDATE account_ssh_keys SET last_used_at=? WHERE key_id=?",
        now, key_id)
    if operation == "git-receive-pack":
        await _audit_sensitive_action(
            env, account, "ssh.git_authorize", "repository",
            canonical_owner + "/" + repo, "success",
            {"operation": operation})
    result = {
        "ok": True,
        "authorized": True,
        "keyId": key_id,
        "requestId": request_id,
        "operation": operation,
        "owner": route_owner,
        "repository": repo,
        "repositoryRelativePath": route_owner + "/" + repo + ".git",
    }
    if bridge:
        # hosted_git.SSH_GIT_TTL_MS: longer than any gateway deadline.
        expires_at = now + 30 * 60 * 1000
        result["httpCredential"] = {
            "username": account,
            "password": ssh_git_password(
                env, account, route_owner, repo, key_id,
                "push" if operation == "git-receive-pack" else "read",
                expires_at),
            "expiresAt": expires_at,
        }
    return json_response(result, cache_control="no-store")
