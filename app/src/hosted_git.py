"""Empty repositories created from the web, pushable with a stock git.

Issue #676: every source in the create-repository modal needed either a
repository that already existed on another provider or a running ForkMesh
desktop node, so a new user whose code sat in a local folder had no way to
start. This module adds the missing path, on top of machinery that already
exists on the node side: the hosted write gateway creates
``<writableRepositoryRoot>/<owner>/<repo>.git`` on the first authorized push
and then serves and health-registers it like any mirror
(``tools/mirror_gateway.py``, mobile-node-contract.md section 7). What was
missing was a way for a browser — and then a plain ``git`` — to reach it.

Two deliberate boundaries:

* **The push credential is never stored.** It is a keyed MAC over (account,
  owner, repository, browser session id, expiry), so there is no new secret
  at rest and no new revocation surface: signing the session out, or letting
  it expire, invalidates every credential minted from it. Verification still
  applies the ordinary push permission checks, so the credential can only
  ever do what its account could already do.
* **A hosted record is Worker-authored, and public.** A web-only account
  holds no signing key at all (keys arrive with a desktop/mobile device), so
  the signature-gated catalog route cannot serve this flow. The record is
  therefore written here, marked ``source: "hosted-web"`` so nothing mistakes
  it for a node's own attested publication, and private visibility is refused
  rather than faked: a private mirror needs the owner-sealed encryption only
  a node holding the owner's key can produce.

A later signed publication from the user's own desktop node takes the record
over through the normal catalog path — nothing here claims source authority.

**Core hosting (ForkMesh Next, docs/HOSTED_REPOS.md).** With
``HOSTED_GIT_CORE_NODES`` set, the relay's own trusted core nodes hold every
hosted repository (``hosted_repos.py``, server/hosted.go), so the create form
also offers private visibility (served only to router capabilities, readable by
the owner and the organization's readers), a first commit (README, .gitignore,
LICENSE), an import of a public repository on an allowed forge, an
organization owner, and per-account quotas. Pushes go to the core that took the
last push, else the rendezvous-first live core; the others replicate from it.
"""

import hashlib
import hmac
import json
import re

import hosted_repos as _hosted_repos
import instance_features


HOSTED_SOURCE = "hosted-web"
# Domain-separated credential. The shared secret is the account-session HMAC
# key, so a session token digest can never be replayed as a push credential
# and vice versa.
HOSTED_PUSH_PREFIX = "forkmesh-hosted-push-v1"
HOSTED_PUSH_SCHEME = "fmp1"
# Long enough that a first push (and the retries a flaky uplink needs) works
# without a trip back to the browser, short enough that a credential pasted
# into a shell history or a .git/config does not outlive the work.
HOSTED_PUSH_TTL_MS = 7 * 24 * 60 * 60 * 1000
HOSTED_PUSH_PASSWORD_RE = re.compile(
    r"^fmp1\.([A-Za-z0-9_-]{24,64})\.([0-9]{10,16})\.([0-9a-f]{64})$")
# A credential the SSH gateway relays a key-authenticated session with
# (ssh_gateway_routes.py): the same Basic shape, bound to the SSH key instead
# of a browser session, to one operation, and to minutes instead of days. The
# relayed HTTPS request then runs the ordinary permission, branch-protection,
# contributor-namespace and bookkeeping path, exactly like a stock git push.
SSH_GIT_PREFIX = "forkmesh-ssh-git-v1"
SSH_GIT_SCHEME = "fms1"
SSH_GIT_TTL_MS = 30 * 60 * 1000
SSH_GIT_PASSWORD_RE = re.compile(
    r"^fms1\.(sk_[A-Za-z0-9_-]{16,80})\.([0-9]{10,16})\.([0-9a-f]{64})$")
SSH_GIT_ACCESS = ("read", "push")
# The same character set the create modal validates client-side.
HOSTED_NAME_RE = re.compile(r"^[A-Za-z0-9._-]{1,64}$")
HOSTED_BRANCH_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._/-]{0,199}$")
HOSTED_HEAD_MAX_BYTES = 2 * 1024 * 1024


def _bind_runtime(runtime):
    """Supply the initialized entrypoint primitives used by this route."""
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


async def _emit_first_repository(env, owner_bi, count_row, owner, name):
    attribution = globals().get("_attribution")
    if attribution is None or int((count_row or {}).get("c") or 0):
        return
    await attribution.emit_for_user(
        env, owner_bi, "first_mirror_created", {"repo": owner + "/" + name})


def _request_origin(request):
    """Origin the browser is talking to, used to build the push remote."""
    parsed = urlparse(str(getattr(request, "url", "") or ""))
    if parsed.scheme not in ("https", "http") or not parsed.netloc:
        return ""
    return parsed.scheme + "://" + parsed.netloc


def hosted_push_mac(env, account, owner, repo, session_id, expires_at):
    """Keyed MAC binding a credential to account, repo, session, expiry."""
    message = "\n".join((
        HOSTED_PUSH_PREFIX, account, owner, repo, session_id,
        str(expires_at),
    ))
    return hmac.new(
        _account_session_secret(env), message.encode(), "sha256",
    ).hexdigest()


def hosted_push_password(env, account, owner, repo, session_id, expires_at):
    """Assemble the Basic-auth password a stock git sends back."""
    return ".".join((
        HOSTED_PUSH_SCHEME, session_id, str(int(expires_at)),
        hosted_push_mac(env, account, owner, repo, session_id, expires_at),
    ))


def ssh_git_mac(env, account, owner, repo, key_id, access, expires_at):
    """Keyed MAC binding an SSH relay credential to key, repo and access."""
    message = "\n".join((
        SSH_GIT_PREFIX, str(account or "").lower(), str(owner or "").lower(),
        str(repo or "").lower(), key_id, access, str(int(expires_at)),
    ))
    return hmac.new(
        _account_session_secret(env), message.encode(), "sha256",
    ).hexdigest()


def ssh_git_password(env, account, owner, repo, key_id, access, expires_at):
    """The Basic password the SSH gateway relays one session with."""
    if access not in SSH_GIT_ACCESS:
        raise ValueError("invalid access")
    return ".".join((
        SSH_GIT_SCHEME, key_id, str(int(expires_at)),
        ssh_git_mac(env, account, owner, repo, key_id, access, expires_at),
    ))


async def _ssh_key_account_matches(env, key_id, account):
    """True while `key_id` is an active SSH key belonging to `account`.

    Read from the primary, like the session check: a revoked key stops a
    relayed session at its next request.
    """
    row = await d1_first_primary(
        env,
        "SELECT account_bi FROM account_ssh_keys "
        "WHERE key_id=? AND revoked_at=0",
        key_id,
    )
    if not row:
        return False
    return str(row.get("account_bi") or "") == await blind_index(env, account)


def _basic_credentials(header):
    """Split one HTTP Basic header into (username, password), or ("", "")."""
    value = clean_string(header or "", 600).strip()
    if not value.lower().startswith("basic "):
        return "", ""
    try:
        decoded = base64.b64decode(value[6:].strip() + "==").decode(
            "utf-8", "replace")
    except Exception:
        return "", ""
    username, separator, password = decoded.partition(":")
    if not separator:
        return "", ""
    return (
        clean_string(username, MAX_NODE_NAME).strip().lower(),
        clean_string(password, 200).strip(),
    )


def has_hosted_push_credential(header):
    """True when a request carries something this module can verify.

    The push route answers unsigned traffic before it consumes a request
    body; this lets it keep doing that without also turning away a plain
    ``git push`` that is about to authenticate.
    """
    return bool(_basic_credentials(header)[1])


async def _session_account_matches(env, session_id, account):
    """True while `session_id` is a live session belonging to `account`.

    Read from the primary: a replica that has not yet seen a sign-out must
    never keep answering pushes for a session the user just revoked.
    """
    row = await d1_first_primary(
        env,
        "SELECT account_bi,revoked_at,expires_at FROM account_sessions "
        "WHERE session_id=?",
        session_id,
    )
    if not row:
        return False
    now = int(Date.now())
    if int(row.get("revoked_at") or 0) or int(
            row.get("expires_at") or 0) <= now:
        return False
    return str(row.get("account_bi") or "") == await blind_index(env, account)


async def verify_hosted_push_credential(env, owner, repo, header, method="",
                                        access="push"):
    """Return the account authorized to push by a Basic credential, or "".

    ``access="read"`` checks read permission instead (a private hosted
    repository's clone); the credential itself is the same.

    Fails closed on every error. A credential proves only that the browser
    session minting it was live and belonged to the named account; the push
    permission itself is re-derived here, so revoking ownership or an
    organization role revokes the credential's reach immediately.

    Only full write access answers here; a contributor's namespaced push is
    authorized by ``verify_hosted_push_access`` alone.
    """
    account, _prefix = await _verify_hosted_credential(
        env, owner, repo, header, method, access)
    return account


async def verify_hosted_push_access(env, owner, repo, header, method="",
                                    alias=""):
    """Authorize a Basic-credential push: ``(account, ref_prefix)``.

    ``ref_prefix`` is "" for an owner or organization writer (the ordinary
    push rules apply) and ``refs/heads/pr/<account>/`` for a contributor on a
    repository the operator opened to contributor pushes; the gateway then
    refuses every ref outside it. ``("", "")`` is a refusal.
    """
    return await _verify_hosted_credential(
        env, owner, repo, header, method, "push", contributor=True,
        alias=alias)


async def _verify_hosted_credential(env, owner, repo, header, method, access,
                                    contributor=False, alias=""):
    owner = safe_segment(owner)
    repo = safe_segment(repo)
    account, password = _basic_credentials(header)
    audit_details = {
        "operation": (
            "git-upload-pack" if access == "read" else "git-receive-pack"),
        "method": clean_string(method, 8).upper(),
        "credential": "session-mac",
    }

    async def denied(audit=False):
        # Only a credential this relay actually issued is worth an audit row.
        # A forged or garbled one is unauthenticated probe traffic, and
        # writing a record for each would let anyone spend D1 writes.
        if audit and account and owner and repo:
            await _audit_sensitive_action(
                env, account,
                "git.read_authorize" if access == "read"
                else "git.push_authorize",
                "repository", owner + "/" + repo, "denied", audit_details)
        return "", ""

    if not owner or not repo or not account or not valid_node_name(account):
        return await denied()
    match = HOSTED_PUSH_PASSWORD_RE.match(password)
    ssh_match = None if match else SSH_GIT_PASSWORD_RE.match(password)
    if not match and not ssh_match:
        return await denied()
    session_id, expires_raw, mac = (match or ssh_match).groups()
    try:
        expires_at = int(expires_raw)
    except (TypeError, ValueError):
        return await denied()
    if ssh_match:
        # Minted by the SSH authorization endpoint for the route the gateway
        # relays to, which may be an organization alias of the node.
        audit_details["credential"] = "ssh-key"
        ssh_access = "read" if access == "read" else "push"
        owners = [owner] + (
            [alias] if alias and alias.lower() != owner.lower() else [])
        if not any(
                hmac.compare_digest(ssh_git_mac(
                    env, account, route_owner, repo, session_id, ssh_access,
                    expires_at), mac)
                for route_owner in owners):
            return await denied()
        if expires_at > int(Date.now()) + SSH_GIT_TTL_MS + 60 * 1000:
            return await denied()
    else:
        expected = hosted_push_mac(
            env, account, owner, repo, session_id, expires_at)
        if not hmac.compare_digest(expected, mac):
            return await denied()
    if expires_at <= int(Date.now()):
        return await denied(audit=True)
    prefix = ""
    try:
        await ensure_schema(env)
        live = (
            await _ssh_key_account_matches(env, session_id, account)
            if ssh_match
            else await _session_account_matches(env, session_id, account))
        if not live:
            return await denied(audit=True)
        if access == "read":
            allowed = await _hosted_read_allowed(env, owner, repo, account)
        else:
            allowed = (
                await _account_owns_node(env, account, owner)
                or await _org_write_allowed(env, owner, repo, account)
            )
            if not allowed and contributor and await contributor_push_allowed(
                    env, owner, repo, account, alias=alias):
                prefix = contributor_ref_prefix(account)
                allowed = bool(prefix)
    except Exception:
        return await denied(audit=True)
    if not allowed:
        return await denied(audit=True)
    if prefix:
        audit_details["refPrefix"] = prefix
    await _audit_sensitive_action(
        env, account,
        "git.read_authorize" if access == "read" else "git.push_authorize",
        "repository", owner + "/" + repo, "success", audit_details)
    return account, prefix


CONTRIBUTOR_PUSH_ENV = "HOSTED_GIT_CONTRIBUTOR_PUSH"
CONTRIBUTOR_REF_ROOT = "refs/heads/pr/"
CONTRIBUTOR_BRANCH_ROOT = "pr/"
_CONTRIBUTOR_ACCOUNT_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,62}$")
_CONTRIBUTOR_ENTRY_RE = re.compile(
    r"^(\*|[a-z0-9][a-z0-9._-]{0,62}/(\*|[a-z0-9._-]{1,100}))$")


def contributor_push_patterns(env):
    raw = str(getattr(env, CONTRIBUTOR_PUSH_ENV, "") or "")
    patterns = []
    for item in re.split(r"[\s,]+", raw.strip().lower()):
        if item and _CONTRIBUTOR_ENTRY_RE.fullmatch(item):
            patterns.append(item)
    return patterns


def contributor_push_enabled(env, owners, repo):
    repo = str(repo or "").strip().lower()
    patterns = contributor_push_patterns(env)
    if not repo or not patterns:
        return False
    for owner in owners:
        owner = str(owner or "").strip().lower()
        if not owner:
            continue
        for pattern in patterns:
            if pattern in ("*", owner + "/*", owner + "/" + repo):
                return True
    return False


def contributor_ref_prefix(account):
    account = str(account or "").strip().lower()
    if (
        not _CONTRIBUTOR_ACCOUNT_RE.fullmatch(account)
        or ".." in account
        or account.endswith((".", ".lock"))
    ):
        return ""
    return CONTRIBUTOR_REF_ROOT + account + "/"


async def contributor_push_allowed(env, owner, repo, account, alias=""):
    if not contributor_ref_prefix(account):
        return False
    if not contributor_push_enabled(env, (alias, owner), repo):
        return False
    try:
        if await _repo_is_private(env, owner, repo):
            return False
        _bi, rec = await _account_row(env, account)
    except Exception:
        return False
    if not isinstance(rec, dict):
        return False
    return (
        rec.get("status") == "active"
        and _account_kind(rec) == "user"
        and rec.get("email_verified") is True
    )


async def record_hosted_push_state(env, owner, repo, endpoint=None,
                                   pusher="", state="", notify=True):
    """Pin the refs digest the hosting node signs after a hosted push.

    A public read is routed only against an authenticated refs attestation
    (``clone_state_pins``), and a repository ForkMesh hosts has no node of
    its own to publish one — so without this a web-created repository would
    be pushable and then unreadable. The digest comes from the hosting node's
    own freshly signed repository proof, the same evidence routing compares
    every endpoint against; the pusher never supplies it.

    Only records this module authored (``source: "hosted-web"``) are touched,
    so a repository whose real node publishes attestations keeps its own.
    Returns the recorded digest, or "" when nothing was recorded.
    """
    owner = safe_segment(owner)
    repo = safe_segment(repo)
    if not owner or not repo:
        return ""
    key_bi = await blind_index(env, owner + "/" + repo)
    row = await d1_first(
        env, "SELECT owner_bi,data FROM repositories WHERE key_bi=?", key_bi)
    if not row:
        return ""
    record = await decrypt_row(env, row.get("data"))
    if not isinstance(record, dict) or record.get("source") != HOSTED_SOURCE:
        return ""
    if endpoint is None:
        endpoint = await _write_endpoint(env)
    if not endpoint:
        return ""
    if _hosted_repos.enabled(env):
        # Every accepted push moves pushed_at, even one that leaves the
        # routing digest (default branch + tags) alone: the other cores fetch
        # when their copy is behind it (hosted_repos.replicas).
        try:
            await _note_core_push(env, key_bi, endpoint.get("node", ""))
        except Exception:
            pass
    digest = clean_string(state, 64).strip().lower()
    if not re.fullmatch(r"[0-9a-f]{64}", digest):
        # A pin-less context asks for the proof itself: a signed, healthy,
        # available proof whose digest simply does not match a pin yet reports
        # ``refs_mismatch``, which is precisely the state a first push leaves.
        details = {}
        await _https_mirror_repository_proof(
            env,
            endpoint,
            {"owner": owner, "repo": repo, "routeOwner": owner,
             "pins": frozenset()},
            "git-upload-pack",
            force_refresh=True,
            details=details,
        )
        digest = clean_string(
            details.get("refsSha256", ""), 64).strip().lower()
        if (
            details.get("reason") not in ("verified", "refs_mismatch")
            or not re.fullmatch(r"[0-9a-f]{64}", digest)
        ):
            return ""
    if record.get("stateHash") == digest:
        return digest
    head = await _hosted_head(env, endpoint, owner, repo)
    if head:
        record["branch"], record["commit"] = head
    return await _pin_hosted_state(
        env, owner, repo, key_bi, row, record, digest, notify=notify,
        pusher=pusher)


async def _hosted_head(env, endpoint, owner, repo):
    try:
        target = https_routing.masked_target_url(
            endpoint["baseUrl"], owner, repo, "branches")
        router_public_key = _https_mirror_router_public_key(env)
        router_seed = _https_mirror_router_seed(env)
        if not target or not router_public_key or not router_seed:
            return None
        path = urlparse(target).path
        request_id = _b64url_encode(_random_bytes(18))
        issued_at = int(Date.now())
        body_digest = hashlib.sha256(b"").hexdigest()
        message = https_routing.request_message(
            endpoint["node"], "GET", path, body_digest, request_id,
            issued_at)
        signature = await ed25519_sign(
            router_public_key, router_seed, message.encode("utf-8"))
        if not message or not signature:
            return None
        upstream = await js_fetch_with_timeout(
            target,
            {"method": "GET", "redirect": "manual", "headers": {
                "X-ForkMesh-Node": endpoint["node"],
                "X-ForkMesh-Request-Id": request_id,
                "X-ForkMesh-Issued-At": str(issued_at),
                "X-ForkMesh-Body-Sha256": body_digest,
                "X-ForkMesh-Signature": signature,
            }},
            HTTPS_MIRROR_FETCH_TIMEOUT_SECONDS,
        )
        if int(getattr(upstream, "status", 0) or 0) != 200:
            return None
        raw = str(await upstream.text())
        if len(raw) > HOSTED_HEAD_MAX_BYTES:
            return None
        return hosted_head_from_branches(json.loads(raw))
    except Exception:
        return None


def hosted_head_from_branches(listing):
    if not isinstance(listing, dict) or listing.get("ok") is not True:
        return None
    branch = str(listing.get("defaultBranch") or "")
    if not HOSTED_BRANCH_RE.fullmatch(branch):
        return None
    for item in listing.get("branches") or []:
        if not isinstance(item, dict) or item.get("name") != branch:
            continue
        commit = str(item.get("commit") or "").lower()
        if re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", commit):
            return branch, commit
        return None
    return None


async def _pin_hosted_state(env, owner, repo, key_bi, row, record, digest,
                            notify=True, pusher=""):
    """Write one routing pin into a hosted catalog record (and its history)."""
    prior_digest = clean_string(record.get("stateHash", ""), 64).strip().lower()
    now_ms = str(int(Date.now()))
    record["stateHash"] = digest
    record["lastSync"] = now_ms
    record["updatedAt"] = now_ms
    _, owner_rec = await _account_row(env, owner)
    if not owner_rec:
        return ""
    encrypted = await encrypt_row(env, record)
    await _contribution_write_catalog_state(
        env, record, row.get("owner_bi"), owner_rec, key_bi, encrypted)
    # History is what lets a read keep working across the next push before
    # this pin is refreshed, exactly as it does for a node-published repo.
    await d1_run(
        env,
        "INSERT INTO repo_state_history (key_bi, state_hash, ts) "
        "VALUES (?,?,?) ON CONFLICT(key_bi, state_hash) "
        "DO UPDATE SET ts=excluded.ts",
        key_bi, digest, int(Date.now()),
    )
    await d1_run(
        env,
        "DELETE FROM repo_state_history WHERE key_bi=? AND state_hash NOT IN "
        "(SELECT state_hash FROM repo_state_history WHERE key_bi=? "
        "ORDER BY ts DESC LIMIT ?)",
        key_bi, key_bi, STATE_PIN_HISTORY,
    )
    _https_mirror_public_context_cache_clear()
    await edge_cache_delete(CATALOG_CACHE_KEY)
    await _bump_catalog_rev(env)
    await schedule_public_catalog_republish(env, owner, repo)
    if not notify:
        return digest
    # Outbound webhooks (adhoc #207). A web-created repository is pushed to
    # over plain git and its catalog state is pinned here rather than through
    # a node's signed publication, so this is the only place that push becomes
    # observable. There is no default-branch tip in this record, so only the
    # "any ref moved" event can be raised from it. Best-effort, like every
    # notification above: the pin is what makes the pushed refs readable, and
    # it must not be undone because a third-party endpoint is unreachable.
    try:
        await queue_repo_webhook_event(
            env, owner, repo, "repository.push",
            {"before": prior_digest, "refsDigest": digest, "hosted": True},
            digest)
    except Exception:
        pass
    # Code-push ping, beside the webhook and for the same reason: this pin is
    # the only evidence a hosted push happened, and the account that owns the
    # repository is who wants to hear about it. `pusher` is the account the
    # receive-pack authorized, so an organization member is not told about
    # their own push. The proof names only the refs digest — no branch, tip
    # or message — and that digest is the event key, so a re-pin of the same
    # state does not ping twice.
    try:
        await notify_code_push(
            env, owner, repo,
            pusher=pusher or owner, pusher_account=pusher,
            key=digest, refs_digest=digest,
            meta={"hosted": True})
    except Exception:
        pass
    return digest


HOSTED_STATS_FIELDS = ("commitCount", "activityWeeks", "mergeCount",
                       "mergeActivityWeeks")


async def record_hosted_stats(env, owner, repo, digest, stats):
    owner = safe_segment(owner)
    repo = safe_segment(repo)
    stats = _hosted_repos.clean_stats(stats)
    digest = clean_string(digest, 64).strip().lower()
    if not owner or not repo or not stats or not re.fullmatch(
            r"[0-9a-f]{64}", digest):
        return False
    key_bi = await blind_index(env, owner + "/" + repo)
    row = await d1_first(
        env, "SELECT owner_bi,data FROM repositories WHERE key_bi=?", key_bi)
    if not row:
        return False
    record = await decrypt_row(env, row.get("data"))
    if (not isinstance(record, dict) or record.get("source") != HOSTED_SOURCE
            or record.get("stateHash") != digest):
        return False
    commit = str(record.get("commit") or "").lower()
    if commit and commit != stats["commit"]:
        return False
    changed = {key: stats[key] for key in HOSTED_STATS_FIELDS
               if key in stats and record.get(key) != stats[key]}
    if not changed:
        return False
    record.update(changed)
    _, owner_rec = await _account_row(env, owner)
    if not owner_rec:
        return False
    encrypted = await encrypt_row(env, record)
    await _contribution_write_catalog_state(
        env, record, row.get("owner_bi"), owner_rec, key_bi, encrypted)
    await edge_cache_delete(CATALOG_CACHE_KEY)
    await _bump_catalog_rev(env)
    return True


async def record_hosted_branch_count(env, owner, repo, count):
    owner = safe_segment(owner)
    repo = safe_segment(repo)
    try:
        count = int(count)
    except (TypeError, ValueError, OverflowError):
        return False
    if not owner or not repo or count < 1:
        return False
    key_bi = await blind_index(env, owner + "/" + repo)
    row = await d1_first(
        env, "SELECT owner_bi,data FROM repositories WHERE key_bi=?", key_bi)
    if not row:
        return False
    record = await decrypt_row(env, row.get("data"))
    if (not isinstance(record, dict) or record.get("source") != HOSTED_SOURCE
            or str(record.get("branchCount") or "") == str(count)):
        return False
    record["branchCount"] = str(count)
    _, owner_rec = await _account_row(env, owner)
    if not owner_rec:
        return False
    encrypted = await encrypt_row(env, record)
    await _contribution_write_catalog_state(
        env, record, row.get("owner_bi"), owner_rec, key_bi, encrypted)
    await edge_cache_delete(CATALOG_CACHE_KEY)
    await _bump_catalog_rev(env)
    return True


async def _remote_url(env, origin, owner, name):
    clone_url = globals().get("_repo_clone_url")
    if callable(clone_url):
        return await clone_url(env, origin, owner, name)
    return origin + "/" + owner + "/" + name


def _push_instructions(remote, account, password, expires_at, node):
    """The copy-paste block the create modal shows after a hosted create."""
    return {
        "remoteUrl": remote,
        "username": account,
        "password": password,
        "expiresAt": int(expires_at),
        "hostedNode": node,
        "commands": [
            "git init -b main",
            "git add .",
            'git commit -m "first commit"',
            "git remote add forkmesh " + remote,
            "git push -u forkmesh main",
        ],
    }


async def _hosted_push_grant(env, request, data, owner, name, account,
                             mac_owner=""):
    """Mint one credential, bound to the calling browser session."""
    session_id = _request_account_session_id(request, data)
    if not session_id:
        return None
    endpoint = await _write_endpoint(env)
    if endpoint is None:
        return None
    origin = _request_origin(request)
    if not origin:
        return None
    expires_at = int(Date.now()) + HOSTED_PUSH_TTL_MS
    password = hosted_push_password(
        env, account, mac_owner or owner, name, session_id, expires_at)
    await _audit_sensitive_action(
        env, account, "repository.push_credential", "repository",
        (mac_owner or owner) + "/" + name, "success",
        {"expiresAt": expires_at, "hostedNode": endpoint.get("node", "")})
    return _push_instructions(
        await _remote_url(env, origin, owner, name), account, password,
        expires_at, endpoint.get("node", ""))


def _session_required_response():
    """The credential must name a revocable session, so require one.

    An account session that carries no opaque id (a compatibility or portable
    token shape) cannot be signed out of later, which is this credential's
    only revocation path.
    """
    return json_response(
        {
            "error": "session_required",
            "message": (
                "Sign in again in this browser to issue a push credential."
            ),
        },
        status=401,
        cache_control="no-store",
    )


def _unavailable_response():
    """No hosted write node is configured, so nothing could accept a push.

    Deliberate configuration state, not a fault: the create modal falls back
    to the desktop handoff on this exact code, and the expected-degraded
    header keeps it out of the error budget.
    """
    return json_response(
        {
            "error": "hosted_git_unavailable",
            "message": (
                "ForkMesh-hosted repositories are not available on this "
                "relay. Create the repository from a ForkMesh node instead."
            ),
        },
        status=503,
        cache_control="no-store",
        extra_headers=EXPECTED_DEGRADED_HEADERS,
    )


async def _create_empty_repository(env, request):
    """Create one empty, hosted, public repository in a session's namespace."""
    if method_name(request) != "POST":
        return json_response({"error": "method_not_allowed"}, status=405)
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    actor = await _authed_account_name(env, request, data)
    if not actor:
        return json_response(
            {"error": "unauthorized"}, status=401, cache_control="no-store")
    if _hosted_repos.enabled(env):
        return await _create_core_repository(env, request, data, actor)
    owner = clean_string(
        data.get("owner", "") or actor, MAX_NODE_NAME).strip().lower()
    name = clean_string(data.get("name", ""), MAX_REPO_SEGMENT).strip()
    if not valid_node_name(owner) or not safe_segment(name) or not (
            HOSTED_NAME_RE.match(name)):
        return json_response({"error": "invalid_name"}, status=400)
    if data.get("visibility") == "private":
        # Private mirrors are owner-sealed archives; the relay never holds the
        # key that would let it seal one. Say so instead of publishing a
        # repository whose contents would be readable by the host.
        return json_response({
            "error": "private_hosted_unsupported",
            "message": (
                "A private repository is an owner-sealed encrypted mirror, "
                "which only a ForkMesh node holding your key can publish. "
                "Create it as public, or publish it from a node."
            ),
        }, status=400)
    await ensure_schema(env)
    if not await _account_owns_node(env, actor, owner):
        return json_response(
            {"error": "forbidden"}, status=403, cache_control="no-store")
    if _is_blocked_catalog_identity(env, owner, name):
        return json_response({"error": "blocked"}, status=403)
    endpoint = await _hosted_git_write_endpoint(env)
    if endpoint is None:
        return _unavailable_response()
    maintainer = clean_string(endpoint.get("publicKey", ""), 120)
    if not maintainer:
        return _unavailable_response()
    _owner_bi_unused, owner_rec = await _account_row(env, owner)
    if not owner_rec:
        return json_response({"error": "account_required"}, status=403)
    owner_bi = await blind_index(env, owner)
    key_bi = await blind_index(env, owner + "/" + name)
    if await d1_first(
            env, "SELECT 1 AS one FROM repositories WHERE key_bi=?", key_bi):
        return json_response({"error": "repository_exists"}, status=409)
    # An owner recreating a name they deleted from the website is the one
    # caller allowed to lift its tombstone — the same "user intent" exception
    # the signed publish path makes, proven here by the session instead.
    if await _repo_delete_tombstone_active(env, key_bi):
        await _clear_repo_delete_tombstone(env, key_bi)
    count_row = await d1_first(
        env, "SELECT COUNT(*) AS c FROM repositories WHERE owner_bi=?",
        owner_bi)
    if count_row and (count_row.get("c", 0) or 0) >= (
            CATALOG_MAX_RECORDS_PER_OWNER):
        return json_response({"error": "too_many_repos"}, status=429)
    limited = await catalog_rate_check(env, owner_bi)
    if limited is not None:
        return limited
    now_ms = str(int(Date.now()))
    origin = _request_origin(request)
    if not origin:
        return _unavailable_response()
    record = safe_catalog_record({
        "owner": owner,
        "name": name,
        "visibility": "public",
        "description": clean_string(data.get("description", ""), 240),
        # The advertised address is the repository's public URL, never the
        # node currently holding the bytes; it is the same address the push
        # remote uses.
        "cloneUrl": origin + "/" + owner + "/" + name,
        "maintainer": maintainer,
        "source": HOSTED_SOURCE,
        "hostedSince": now_ms,
        "lastSync": now_ms,
        "updatedAt": now_ms,
    })
    if not record:
        return json_response({"error": "invalid_name"}, status=400)
    if not _request_account_session_id(request, data):
        return _session_required_response()
    grant = await _hosted_push_grant(env, request, data, owner, name, actor)
    if grant is None:
        return _unavailable_response()
    encrypted = await encrypt_row(env, record)
    await _contribution_write_catalog_state(
        env, record, owner_bi, owner_rec, key_bi, encrypted)
    await _emit_first_repository(env, owner_bi, count_row, owner, name)
    # This isolate may have answered "not publicly served" for this name a
    # moment ago; the row just written is the authority now.
    forget_closed = globals().get("_forget_repo_closed")
    if callable(forget_closed):
        forget_closed(owner, name)
    _https_mirror_public_context_cache_clear()
    await edge_cache_delete(CATALOG_CACHE_KEY)
    await _bump_catalog_rev(env)
    await schedule_public_catalog_republish(env, owner, name)
    await _audit_sensitive_action(
        env, actor, "repository.create_hosted", "repository",
        owner + "/" + name, "success",
        {"source": HOSTED_SOURCE, "hostedNode": endpoint.get("node", "")})
    return json_response({
        "ok": True,
        "repository": record,
        "push": grant,
    }, status=201, cache_control="no-store")


async def _mint_push_credential(env, request):
    """Re-mint a push credential for a repository the session may push."""
    if method_name(request) != "POST":
        return json_response({"error": "method_not_allowed"}, status=405)
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    actor = await _authed_account_name(env, request, data)
    if not actor:
        return json_response(
            {"error": "unauthorized"}, status=401, cache_control="no-store")
    owner = clean_string(
        data.get("owner", "") or actor, MAX_NODE_NAME).strip().lower()
    name = clean_string(data.get("name", ""), MAX_REPO_SEGMENT).strip()
    if not valid_node_name(owner) or not safe_segment(name):
        return json_response({"error": "invalid_name"}, status=400)
    await ensure_schema(env)
    node = await _org_repo_node(env, owner, name) or owner
    if node in _core_push_nodes(env):
        allowed = await _core_push_permitted(env, node, name, actor)
    else:
        allowed = (
            await _account_owns_node(env, actor, node)
            or await _org_write_allowed(env, node, name, actor)
        )
    prefix = ""
    if not allowed and await contributor_push_allowed(
            env, node, name, actor, alias=owner if owner != node else ""):
        prefix = contributor_ref_prefix(actor)
    if not allowed and not prefix:
        return json_response(
            {"error": "forbidden",
             "message": await _push_refusal_message(
                 env, node, owner, name, actor)},
            status=403, cache_control="no-store")
    if not _request_account_session_id(request, data):
        return _session_required_response()
    grant = await _hosted_push_grant(
        env, request, data, owner, name, actor, mac_owner=node)
    if grant is None:
        return _unavailable_response()
    if prefix:
        grant = _contributor_instructions(grant, prefix)
    else:
        grant["access"] = "write"
    return json_response(
        {"ok": True, "push": grant}, cache_control="no-store")


async def _push_refusal_message(env, node, owner, name, actor):
    if contributor_push_enabled(env, (owner, node), name):
        try:
            _bi, rec = await _account_row(env, actor)
        except Exception:
            rec = None
        if isinstance(rec, dict) and rec.get("email_verified") is not True:
            return ("Contributor pushes need a verified email address. "
                    "Verify it under Settings, Account, then try again.")
        return "Contributor pushes are not available to this account."
    return ("You cannot push to this repository. Ask its owner for write "
            "access, or for contributor pushes to be enabled.")


def _contributor_instructions(grant, prefix):
    branch_prefix = prefix[len("refs/heads/"):]
    example = branch_prefix + "my-change"
    grant = dict(grant)
    grant["access"] = "contributor"
    grant["refPrefix"] = prefix
    grant["branchPrefix"] = branch_prefix
    grant["commands"] = [
        "git clone " + grant["remoteUrl"],
        "git switch -c " + example,
        'git commit -am "Describe the change"',
        "git push origin " + example,
    ]
    return grant


async def hosted_repository_handler(env, request, path):
    """Route the hosted-repository endpoints."""
    route = str(path or "").rstrip("/")
    if route == "/api/hosted-repositories":
        if method_name(request) == "GET":
            return await _hosted_settings(env, request)
        return await _create_empty_repository(env, request)
    if route == "/api/hosted-repositories/push-credential":
        return await _mint_push_credential(env, request)
    if route == _hosted_repos.REPLICAS_PATH:
        if method_name(request) != "POST":
            return json_response({"error": "method_not_allowed"}, status=405)
        runtime = _ReplicaRuntime(env, request)
        raw = await runtime.body_bytes(_hosted_repos.MAX_BODY_BYTES)
        if raw is None:
            return json_response({"error": "request_too_large"}, status=413)
        return await _hosted_repos.replicas(runtime, raw)
    return json_response({"error": "not_found"}, status=404)


# ---------------------------------------------------------------------------
# Core hosting (HOSTED_GIT_CORE_NODES; docs/HOSTED_REPOS.md)
# ---------------------------------------------------------------------------

HOSTED_GIT_READ_OPERATIONS = frozenset({"git-info-refs", "git-upload-pack"})


def hosted_core_nodes(env):
    """Core node names hosting ForkMesh repositories ([] when off)."""
    return _hosted_repos.core_nodes(env)


def hosted_core_owners(env):
    return _hosted_repos.core_owners(env)
HOSTED_API_READ_OPERATIONS = frozenset({
    "tree", "blobs", "blob", "raw", "history", "commit", "compare",
    "merge-preview", "branches", "search", "stats", "sizes",
})


class _ReplicaRuntime:
    """Binds hosted_repos (and runner_leases._verify_signed) to this Worker."""

    def __init__(self, env, request=None):
        self.env = env
        self.request = request

    def method(self):
        return method_name(self.request)

    def header(self, name):
        try:
            return str(self.request.headers.get(name) or "")
        except Exception:
            return ""

    def now(self):
        return int(Date.now())

    def new_id(self):
        return bytes(_random_bytes(16)).hex()

    def response(self, data, status=200):
        return json_response(data, status=status, cache_control="no-store")

    async def body_bytes(self, limit):
        raw = str(await self.request.text()).encode("utf-8")
        return raw if len(raw) <= limit else None

    async def verify(self, public_key, signature, message):
        return bool(await ed25519_verify(public_key, signature, message))

    async def node_keys(self, name):
        return list(await _claimed_node_signing_pubkeys(self.env, name) or [])

    async def sign_router(self, message):
        public_key = _https_mirror_router_public_key(self.env)
        seed = _https_mirror_router_seed(self.env)
        if not public_key or not seed:
            return ""
        return str(await ed25519_sign(
            public_key, seed, message.encode("utf-8")) or "")

    def core_nodes(self):
        return _hosted_repos.core_nodes(self.env)

    async def live_core_nodes(self, cores):
        endpoints = await _core_endpoints(self.env)
        return [n for n in cores
                if n in endpoints and endpoints[n].get("healthy")]

    async def core_endpoints(self, cores):
        endpoints = await _core_endpoints(self.env)
        return {n: https_routing.normalize_base_url(endpoints[n]["baseUrl"])
                for n in cores if n in endpoints}

    async def open(self, value):
        return await decrypt_row(self.env, value)

    async def pin_initial(self, owner, name, digest):
        """Pin the first content a core reports (initial commit or import)."""
        key_bi = await blind_index(self.env, owner + "/" + name)
        row = await d1_first(
            self.env,
            "SELECT owner_bi,data FROM repositories WHERE key_bi=?", key_bi)
        if not row:
            return ""
        record = await decrypt_row(self.env, row.get("data"))
        if (not isinstance(record, dict)
                or record.get("source") != HOSTED_SOURCE
                or record.get("stateHash")):
            return ""
        pinned = await _pin_hosted_state(
            self.env, owner, name, key_bi, row, record, digest, notify=False)
        if pinned:
            await _notify_cores(self.env, owner, name)
        return pinned

    async def record_stats(self, owner, name, digest, stats):
        try:
            return await record_hosted_stats(
                self.env, owner, name, digest, stats)
        except Exception:
            return False

    async def push_mirror_keys(self):
        try:
            rows = await d1_all(
                self.env,
                "SELECT repo_bi FROM repo_provider_mirrors WHERE active=1")
        except Exception:
            return set()
        return {str(row.get("repo_bi") or "") for row in rows or []}

    async def push_mirror(self, owner, name, aliases, keys):
        for candidate in [owner] + list(aliases or []):
            key = await blind_index(self.env, candidate.lower() + "/" + name)
            if key in keys:
                return await provider_mirror_push_config(
                    self.env, candidate, name)
        return None

    async def record_push_mirror(self, owner, name, aliases, report):
        return await provider_mirror_record_push(
            self.env, owner, name, report, tuple(aliases or ()))

    async def d1_all(self, sql, *args):
        return await d1_all(self.env, sql, *args)

    async def d1_first(self, sql, *args):
        return await d1_first(self.env, sql, *args)

    async def d1_run(self, sql, *args):
        return await d1_run(self.env, sql, *args)

    @staticmethod
    def changes(result):
        try:
            return int(result.meta.changes)
        except Exception:
            try:
                converted = (
                    result.to_py() if hasattr(result, "to_py") else result)
                return int((converted.get("meta") or {}).get("changes") or 0)
            except Exception:
                return 0


async def _core_endpoints(env):
    """Registered, unblocked endpoints of the configured core nodes."""
    cores = _hosted_repos.core_nodes(env)
    if not cores:
        return {}
    try:
        rows = await d1_all(
            env,
            "SELECT node_name,base_url,mirror_url,public_key,registration_sig,"
            "issued_at,checked_at,latency_ms,region,healthy,integrity,"
            "abuse_blocked,forkmesh_refs_sha256 FROM mirror_https_endpoints "
            "WHERE node_name IN (%s)" % ",".join("?" * len(cores)),
            *cores)
    except Exception:
        return {}
    out = {}
    for row in rows or []:
        endpoint = _https_mirror_endpoint_projection(row)
        node = str(endpoint.get("node") or "").lower()
        if (node in cores and not endpoint.get("abuseBlocked")
                and https_routing.normalize_base_url(endpoint.get("baseUrl"))):
            out[node] = endpoint
    return out


async def _core_candidates(env, repo_id, pushed_by=""):
    """Core endpoints for one repository: last pusher, then rendezvous order.

    The node that accepted the last push certainly holds it; the others may
    still be fetching. Unhealthy (by the health cron) nodes come last.
    """
    cores = _hosted_repos.core_nodes(env)
    endpoints = await _core_endpoints(env)
    live = [n for n in cores if n in endpoints and endpoints[n].get("healthy")]
    rest = [n for n in cores if n in endpoints and n not in live]
    order = (_hosted_repos.rendezvous(live, repo_id)
             + _hosted_repos.rendezvous(rest, repo_id))
    if pushed_by in live:
        order = [pushed_by] + [n for n in order if n != pushed_by]
    return [endpoints[n] for n in order]


async def _write_endpoint(env, repo_id=""):
    if _hosted_repos.enabled(env):
        candidates = await _core_candidates(env, repo_id)
        return candidates[0] if candidates else None
    return await _hosted_git_write_endpoint(env)


async def _hosted_row(env, owner, repo):
    """The live hosted_repos row for owner/repo, or None (fails closed)."""
    if not _hosted_repos.enabled(env):
        return None
    owner = safe_segment(owner)
    repo = safe_segment(repo)
    if not owner or not repo:
        return None
    try:
        runtime = _ReplicaRuntime(env)
        await _hosted_repos.ensure_tables(runtime)
        key_bi = await blind_index(env, owner + "/" + repo)
        return await _hosted_repos.find_by_key(runtime, key_bi)
    except Exception:
        return None


async def _notify_cores(env, owner, name):
    """Payload-free "hosted" wake-up for every core daemon (best effort)."""
    for node in _hosted_repos.core_nodes(env):
        try:
            await notify_repo_host(env, node, name, "hosted", event_owner=owner)
        except Exception:
            pass


async def _note_core_push(env, key_bi, node):
    runtime = _ReplicaRuntime(env)
    await _hosted_repos.ensure_tables(runtime)
    row = await _hosted_repos.find_by_key(runtime, key_bi)
    if not row:
        return
    await _hosted_repos.note_push(runtime, row["id"], node, int(Date.now()))
    record = await decrypt_row(env, row.get("data"))
    if isinstance(record, dict):
        await _notify_cores(env, record.get("owner", ""), record.get("name", ""))


async def _hosted_read_allowed(env, owner, repo, account):
    """Read access to a hosted repository: its owner, or an org reader."""
    if not account:
        return False
    try:
        if await _account_owns_node(env, account, owner):
            return True
        rows = await d1_all(
            env, "SELECT org_bi FROM org_repos WHERE node_owner=? AND repo=?",
            owner.lower(), repo.lower())
        for row in rows or []:
            permission = await _org_permission(
                env, str(row.get("org_bi") or ""), account)
            if team_permission_rank(permission) >= team_permission_rank(
                    "read"):
                return True
    except Exception:
        return False
    return False


async def _creatable_organizations(env, actor):
    """Organizations where ``actor`` may create repositories (maintain+)."""
    try:
        rows = await d1_all(
            env,
            "SELECT o.name AS name, m.org_bi AS org_bi FROM org_members m "
            "JOIN orgs o ON o.org_bi=m.org_bi WHERE m.member_bi=?",
            await blind_index(env, actor))
    except Exception:
        return []
    out = []
    for row in rows or []:
        permission = await _org_permission(
            env, str(row.get("org_bi") or ""), actor)
        if team_permission_rank(permission) >= team_permission_rank(
                "maintain"):
            out.append(str(row.get("name") or "").lower())
    return sorted(n for n in out if valid_node_name(n))


async def _hosted_settings(env, request):
    """GET /api/hosted-repositories: what this relay offers the create form."""
    core = _hosted_repos.enabled(env)
    try:
        actor = await _authed_account_name(env, request, {})
    except Exception:
        actor = ""
    available = core or (await _hosted_git_write_endpoint(env)) is not None
    body = {
        "ok": True,
        "available": available,
        "mode": "core" if core else ("single-node" if available else "off"),
        "privateSupported": core,
        "initSupported": core,
        "importSupported": core,
        "importHosts": list(_hosted_repos.import_hosts(env)) if core else [],
        "gitignoreTemplates": sorted(_hosted_repos.GITIGNORE_TEMPLATES),
        "licenseTemplates": sorted(_hosted_repos.LICENSE_TEMPLATES),
    }
    if core:
        body["limits"] = {
            "repositoriesPerAccount": _hosted_repos.max_per_account(env),
            "repositoryBytes": _hosted_repos.max_bytes(env),
        }
    body["contributorPush"] = contributor_push_patterns(env)
    if actor and core:
        runtime = _ReplicaRuntime(env, request)
        await ensure_schema(env)
        await _hosted_repos.ensure_tables(runtime)
        body["used"] = await _hosted_repos.count_for_creator(
            runtime, await blind_index(env, actor))
        body["organizations"] = await _creatable_organizations(env, actor)
    return json_response(body, cache_control="no-store")


def _error(code, status, message="", **extra):
    body = {"error": code}
    if message:
        body["message"] = message
    body.update(extra)
    return json_response(body, status=status, cache_control="no-store")


async def core_repository_target(env, runtime, actor, owner, name,
                                 organization):
    await ensure_schema(env)
    await _hosted_repos.ensure_tables(runtime)
    if not await _account_owns_node(env, actor, owner):
        return ("forbidden", 403, ""), None
    if _is_blocked_catalog_identity(env, owner, name):
        return ("blocked", 403, ""), None
    org_bi = ""
    if organization:
        if not valid_node_name(organization):
            return ("invalid_organization", 400, ""), None
        org_bi, org_row = await _org_row(env, organization)
        if not org_row:
            return ("organization_not_found", 404, ""), None
        permission = await _org_permission(env, org_bi, actor)
        if team_permission_rank(permission) < team_permission_rank(
                "maintain"):
            return ("forbidden", 403,
                    "Creating an organization repository needs the "
                    "maintain permission there."), None
        if await d1_first(
                env, "SELECT 1 AS one FROM org_repos WHERE org_bi=? AND repo=?",
                org_bi, name.lower()):
            return ("repository_exists", 409, ""), None
        count_row = await d1_first(
            env, "SELECT COUNT(*) AS n FROM org_repos WHERE org_bi=?", org_bi)
        if count_row and int(count_row.get("n") or 0) >= MAX_ORG_REPOS:
            return ("too_many_repos", 429, ""), None
    _owner_bi_unused, owner_rec = await _account_row(env, owner)
    if not owner_rec:
        return ("account_required", 403, ""), None
    owner_bi = await blind_index(env, owner)
    key_bi = await blind_index(env, owner + "/" + name)
    if await d1_first(
            env, "SELECT 1 AS one FROM repositories WHERE key_bi=?", key_bi):
        return ("repository_exists", 409, ""), None
    if await _hosted_repos.find_by_key(runtime, key_bi):
        return ("repository_exists", 409, ""), None
    if await _repo_delete_tombstone_active(env, key_bi):
        await _clear_repo_delete_tombstone(env, key_bi)
    count_row = await d1_first(
        env, "SELECT COUNT(*) AS c FROM repositories WHERE owner_bi=?",
        owner_bi)
    if count_row and (count_row.get("c", 0) or 0) >= (
            CATALOG_MAX_RECORDS_PER_OWNER):
        return ("too_many_repos", 429, ""), None
    return None, {
        "owner": owner, "name": name, "organization": organization,
        "org_bi": org_bi, "owner_bi": owner_bi, "owner_rec": owner_rec,
        "key_bi": key_bi, "count_row": count_row,
    }


def core_catalog_record(target, visibility, description, origin, maintainer,
                        now, forked_from=""):
    public_owner = target["organization"] or target["owner"]
    extra = {"forkedFrom": forked_from} if forked_from else {}
    return safe_catalog_record({
        **extra,
        "owner": target["owner"],
        "name": target["name"],
        "visibility": visibility,
        "description": description,
        "cloneUrl": origin + "/" + public_owner + "/" + target["name"],
        "maintainer": maintainer,
        "source": HOSTED_SOURCE,
        "hostedSince": str(now),
        "lastSync": str(now),
        "updatedAt": str(now),
    })


async def store_core_repository(env, runtime, target, record, *, actor,
                                repo_id, visibility, init, endpoint, now,
                                imported=False, fork=None):
    owner, name = target["owner"], target["name"]
    organization = target["organization"]
    encrypted = await encrypt_row(env, record)
    await _contribution_write_catalog_state(
        env, record, target["owner_bi"], target["owner_rec"],
        target["key_bi"], encrypted)
    await _emit_first_repository(
        env, target["owner_bi"], target["count_row"], owner, name)
    hosted_data = await encrypt_row(env, {
        "owner": owner,
        "name": name,
        "organization": organization,
        "aliases": [organization] if organization else [],
        "init": init,
        "createdBy": actor,
        **({"fork": fork} if fork else {}),
    })
    await _hosted_repos.insert(
        runtime, repo_id, target["key_bi"], target["owner_bi"],
        await blind_index(env, actor), visibility, hosted_data, now)
    if organization:
        await _hosted_repos.link_organization(
            runtime, target["org_bi"], name, owner, now)
        memo = globals().get("_ORG_ALIAS_MEMO")
        if isinstance(memo, dict):
            memo.clear()
    forget_closed = globals().get("_forget_repo_closed")
    if callable(forget_closed):
        forget_closed(owner, name)
    _https_mirror_public_context_cache_clear()
    await edge_cache_delete(CATALOG_CACHE_KEY)
    await _bump_catalog_rev(env)
    await schedule_public_catalog_republish(env, owner, name)
    await _audit_sensitive_action(
        env, actor, "repository.create_hosted", "repository",
        owner + "/" + name, "success",
        {"source": HOSTED_SOURCE, "hostedNode": endpoint.get("node", ""),
         "visibility": visibility, "organization": organization,
         "import": bool(imported), "initialized": bool(init),
         **({"forkOf": fork.get("of", "")} if fork else {})})
    await _notify_cores(env, owner, name)


async def _create_core_repository(env, request, data, actor):
    """Create a repository the core nodes host (private allowed, optional
    first commit or import, optional organization owner, quotas)."""
    runtime = _ReplicaRuntime(env, request)
    owner = clean_string(
        data.get("owner", "") or actor, MAX_NODE_NAME).strip().lower()
    name = clean_string(data.get("name", ""), MAX_REPO_SEGMENT).strip()
    if (not valid_node_name(owner) or not safe_segment(name)
            or not HOSTED_NAME_RE.match(name)
            or not _hosted_repos.valid_name(name)):
        return _error("invalid_name", 400)
    visibility = "private" if data.get("visibility") == "private" else "public"
    description = clean_string(data.get("description", ""), 240).strip()
    organization = clean_string(
        data.get("organization", ""), MAX_NODE_NAME).strip().lower()
    source_url = ""
    if str(data.get("importUrl") or "").strip():
        source_url = _hosted_repos.import_url(
            data.get("importUrl"), _hosted_repos.import_hosts(env))
        if not source_url:
            return _error(
                "invalid_import_url", 400,
                "Use a public https URL on "
                + ", ".join(_hosted_repos.import_hosts(env)) + ".")
    problem, target = await core_repository_target(
        env, runtime, actor, owner, name, organization)
    if problem:
        return _error(*problem)
    creator_bi = await blind_index(env, actor)
    limit = _hosted_repos.max_per_account(env)
    used = await _hosted_repos.count_for_creator(runtime, creator_bi)
    if used >= limit:
        return _error(
            "hosted_quota_exceeded", 429,
            instance_features.rename(
                env, "This account already hosts %d repositories on ForkMesh, "
                "the limit. Delete one, or host more from your own node."
                % limit),
            limit=limit, used=used)
    limited = await catalog_rate_check(env, target["owner_bi"])
    if limited is not None:
        return limited
    if not _request_account_session_id(request, data):
        return _session_required_response()
    origin = _request_origin(request)
    if not origin:
        return _unavailable_response()
    repo_id = "hr" + bytes(_random_bytes(12)).hex()
    candidates = await _core_candidates(env, repo_id)
    if not candidates:
        return _unavailable_response()
    endpoint = candidates[0]
    maintainer = clean_string(endpoint.get("publicKey", ""), 120)
    if not maintainer:
        return _unavailable_response()
    now = int(Date.now())
    init, init_error = _hosted_repos.build_init(
        owner, name, description,
        readme=bool(data.get("readme", True)) and not source_url,
        gitignore=data.get("gitignore", ""),
        license_id=data.get("license", ""),
        author=actor,
        author_email=_hosted_repos.noreply_email(actor, origin),
        created_at_ms=now, source_url=source_url)
    if init_error:
        return _error(init_error, 400)
    public_owner = organization or owner
    record = core_catalog_record(
        target, visibility, description, origin, maintainer, now)
    if not record:
        return _error("invalid_name", 400)
    grant = await _hosted_push_grant(env, request, data, owner, name, actor)
    if grant is None:
        return _unavailable_response()
    old_remote = grant["remoteUrl"]
    grant["remoteUrl"] = await _remote_url(env, origin, public_owner, name)
    if init:
        # The remote gets a first commit (or an import): start from a clone.
        grant["commands"] = ["git clone " + grant["remoteUrl"], "cd " + name]
    else:
        grant["commands"] = [
            line.replace(old_remote, grant["remoteUrl"])
            for line in grant["commands"]]
    await store_core_repository(
        env, runtime, target, record, actor=actor, repo_id=repo_id,
        visibility=visibility, init=init, endpoint=endpoint, now=now,
        imported=bool(source_url))
    return json_response({
        "ok": True,
        "repository": record,
        "push": grant,
        "hosting": {
            "mode": "core",
            "id": repo_id,
            "primary": endpoint.get("node", ""),
            "nodes": _hosted_repos.core_nodes(env),
            "initialized": bool(init) and not source_url,
            "importing": bool(source_url),
            "organization": organization,
        },
        "quota": {"used": used + 1, "limit": limit,
                  "repositoryBytes": _hosted_repos.max_bytes(env)},
    }, status=201, cache_control="no-store")


def _fork_response(owner, name, parent_owner, parent_name, now, status=201,
                   existing=False):
    return json_response({
        "ok": True, "created": True, "queued": False, "hosted": True,
        "existing": existing,
        "fork": {
            "requestId": "",
            "sourceOwner": parent_owner.lower(),
            "sourceName": parent_name.lower(),
            "owner": owner,
            "name": name,
            "forkedFrom": (parent_owner + "/" + parent_name).lower(),
            "status": "completed",
            "url": "/" + owner + "/" + name,
            "createdAt": int(now),
        },
    }, status=status, cache_control="no-store")


async def hosted_fork_create(env, request, node, public_owner, repo):
    parent_row = await _hosted_row(env, node, repo)
    if not parent_row:
        return None
    try:
        data = await bounded_json_request(request)
    except Exception:
        return _error("invalid_json", 400)
    if not isinstance(data, dict):
        data = {}
    actor_bi, account = await _account_session_record(env, request, data)
    actor = clean_string(
        (account or {}).get("name", ""), MAX_NODE_NAME).strip().lower()
    if not actor_bi or not account or not valid_node_name(actor):
        return _error("unauthorized", 401)
    if (account.get("status") != "active"
            or _account_kind(account) != "user"):
        return _error("forbidden", 403)
    if account.get("email_verified") is not True:
        return _error(
            "email_verification_required", 403,
            "Verify your email address under Settings, Account, to fork.")
    runtime = _ReplicaRuntime(env, request)
    await _hosted_repos.ensure_tables(runtime)
    parent = await decrypt_row(env, parent_row.get("data"))
    parent_id = str(parent_row.get("id") or "")
    if (not isinstance(parent, dict)
            or not _hosted_repos.ID_RE.fullmatch(parent_id)
            or parent_row.get("visibility") != "public"
            or await _repo_is_private(env, node, repo)):
        return _error("fork_unavailable", 403,
                      "Only public repositories can be forked here.")
    parent_name = str(parent.get("name") or repo)
    parent_fork = _hosted_repos.fork_of(dict(parent, id=parent_id))
    network = parent_fork[1] if parent_fork else parent_id
    now = int(Date.now())
    creator_bi = await blind_index(env, actor)
    for row in await _hosted_repos.rows_for_creator(runtime, creator_bi):
        record = await decrypt_row(env, row.get("data"))
        fork = _hosted_repos.fork_of(record)
        if fork and fork[0] == parent_id:
            return _fork_response(
                str(record.get("owner") or actor),
                str(record.get("name") or parent_name),
                public_owner, parent_name, now, status=200, existing=True)
    if actor == node or actor == str(public_owner or "").lower():
        return _error("fork_of_own_repository", 409,
                      "This repository is already yours; push to it "
                      "directly.")
    name = clean_string(
        data.get("name", "") or parent_name, MAX_REPO_SEGMENT).strip()
    if (not safe_segment(name) or not HOSTED_NAME_RE.match(name)
            or not _hosted_repos.valid_name(name)):
        return _error("invalid_name", 400)
    limit = _hosted_repos.max_forks_per_account(env)
    used = await _hosted_repos.count_for_creator(
        runtime, creator_bi, forks=True)
    if used >= limit:
        return _error(
            "fork_quota_exceeded", 429,
            "This account already has %d forks here, the limit. Delete one "
            "to fork another repository." % limit, limit=limit, used=used)
    problem, target = await core_repository_target(
        env, runtime, actor, actor, name, "")
    if problem:
        if problem[0] == "repository_exists":
            return _error(
                "fork_name_taken", 409,
                "You already have a repository named %s. Fork under another "
                "name." % name)
        return _error(*problem)
    limited = await catalog_rate_check(env, target["owner_bi"])
    if limited is not None:
        return limited
    origin = _request_origin(request)
    repo_id = "hr" + bytes(_random_bytes(12)).hex()
    candidates = await _core_candidates(env, repo_id)
    if not origin or not candidates:
        return _unavailable_response()
    endpoint = candidates[0]
    maintainer = clean_string(endpoint.get("publicKey", ""), 120)
    if not maintainer:
        return _unavailable_response()
    record = core_catalog_record(
        target, "public", clean_string(parent.get("description", ""), 240),
        origin, maintainer, now,
        forked_from=str(public_owner or node) + "/" + parent_name)
    if not record:
        return _error("invalid_name", 400)
    await store_core_repository(
        env, runtime, target, record, actor=actor, repo_id=repo_id,
        visibility="public", init=None, endpoint=endpoint, now=now,
        fork={"of": parent_id, "network": network})
    index_edge = globals().get("_index_repo_fork_edge")
    if callable(index_edge):
        await index_edge(env, record, None, target["owner_bi"],
                         target["key_bi"])
    return _fork_response(actor, name, public_owner or node, parent_name, now)


async def hosted_push_candidates(env, owner, repo):
    """Push endpoints for a core-hosted repository, or None if it is not one."""
    row = await _hosted_row(env, owner, repo)
    if not row:
        return None
    return await _core_candidates(
        env, str(row.get("id") or ""), str(row.get("pushed_by") or ""))


async def hosted_push_limit(env, request, owner, repo, method):
    """413 when a push would take a core-hosted repository past its quota."""
    if method != "POST":
        return None
    row = await _hosted_row(env, owner, repo)
    if not row:
        return None
    limit = _hosted_repos.max_bytes(env)
    try:
        announced = int(request.headers.get("content-length") or 0)
    except Exception:
        announced = 0
    size = await _hosted_repos.size_of(_ReplicaRuntime(env), row["id"])
    if size >= limit or announced > limit:
        return json_response({
            "error": "repository_quota_exceeded",
            "message": instance_features.rename(
                env, "This repository has reached the %d MB ForkMesh hosting "
                "limit. Move it to your own node to grow further."
                % (limit // (1024 * 1024))),
            "limitBytes": limit,
            "sizeBytes": size,
        }, status=413, cache_control="no-store")
    return None


def _git_read_basic_challenge():
    return json_response(
        {"error": "authentication_required"}, status=401,
        cache_control="no-store",
        extra_headers={"WWW-Authenticate": 'Basic realm="forkmesh"'})


async def hosted_private_read(env, request, owner, repo, operation):
    """Serve a private core-hosted repository to a reader, else None.

    Called only for repositories the catalog marks private. A Basic hosted
    credential (git) or an account session (the website) must belong to the
    owner or an organization reader; the bytes then come from a core gateway
    through a router capability, the only route a "restricted" repository
    answers on.
    """
    if (operation not in HOSTED_GIT_READ_OPERATIONS
            and operation not in HOSTED_API_READ_OPERATIONS):
        return None
    row = await _hosted_row(env, owner, repo)
    if not row or row.get("visibility") != "private":
        return None
    git = operation in HOSTED_GIT_READ_OPERATIONS
    method = method_name(request)
    authorization = ""
    try:
        authorization = clean_string(
            request.headers.get("authorization") or "", 600).strip()
    except Exception:
        authorization = ""
    account = ""
    if has_hosted_push_credential(authorization):
        account = await verify_hosted_push_credential(
            env, owner, repo, authorization, method, access="read")
    else:
        try:
            account = await _authed_account_name(env, request, {})
        except Exception:
            account = ""
        if account and not await _hosted_read_allowed(
                env, owner, repo, account):
            account = ""
    if not account:
        if git:
            return _git_read_basic_challenge()
        return json_response(
            {"error": "not_found"}, status=404, cache_control="no-store")
    candidates = await _core_candidates(
        env, str(row.get("id") or ""), str(row.get("pushed_by") or ""))
    return await _core_proxy(env, request, owner, repo, operation, candidates)


async def _core_proxy(env, request, owner, repo, operation, candidates):
    """One capability-signed read against the first core that answers."""
    method = method_name(request)
    if operation == "git-upload-pack":
        if method != "POST":
            return json_response({"error": "method_not_allowed"}, status=405)
    elif method not in ("GET", "HEAD"):
        return json_response({"error": "method_not_allowed"}, status=405)
    query = _https_mirror_request_query(urlparse(request.url), operation)
    body = b""
    if method == "POST":
        try:
            announced = int(request.headers.get("content-length") or 0)
        except Exception:
            announced = 0
        if announced > HTTPS_MIRROR_BODY_MAX_BYTES:
            return json_response({"error": "request_too_large"}, status=413)
        try:
            body = bytes(await request.bytes())
        except Exception:
            return json_response({"error": "invalid_request"}, status=400)
        if len(body) > HTTPS_MIRROR_BODY_MAX_BYTES:
            return json_response({"error": "request_too_large"}, status=413)
    router_public_key = _https_mirror_router_public_key(env)
    router_seed = _https_mirror_router_seed(env)
    if not router_public_key or not router_seed or not candidates:
        return json_response(
            {"error": "mirror_unavailable"}, status=503,
            cache_control="no-store", extra_headers=EXPECTED_DEGRADED_HEADERS)
    body_digest = hashlib.sha256(body).hexdigest()
    git_protocol = clean_string(
        request.headers.get("git-protocol") or "", 32).strip().lower()
    if git_protocol not in ("", "version=1", "version=2"):
        git_protocol = ""
    for endpoint in candidates:
        target = https_routing.masked_target_url(
            endpoint["baseUrl"], owner, repo, operation, query)
        if not target:
            continue
        parsed = urlparse(target)
        signed_path = parsed.path + (("?" + parsed.query) if parsed.query else "")
        request_id = _b64url_encode(_random_bytes(18))
        issued_at = int(Date.now())
        message = https_routing.request_message(
            endpoint["node"], method, signed_path, body_digest, request_id,
            issued_at)
        signature = await ed25519_sign(
            router_public_key, router_seed, message.encode("utf-8"))
        if not message or not signature:
            continue
        headers = {
            "X-ForkMesh-Node": endpoint["node"],
            "X-ForkMesh-Request-Id": request_id,
            "X-ForkMesh-Issued-At": str(issued_at),
            "X-ForkMesh-Body-Sha256": body_digest,
            "X-ForkMesh-Signature": signature,
        }
        if git_protocol:
            headers["Git-Protocol"] = git_protocol
        if operation == "git-upload-pack":
            headers["Content-Type"] = "application/x-git-upload-pack-request"
            encoding = request.headers.get("content-encoding")
            if encoding:
                headers["Content-Encoding"] = encoding
        init = {"method": method, "headers": headers, "redirect": "manual"}
        if method == "POST":
            init["body"] = Uint8Array.new(_to_js(body))
        try:
            upstream = await js_fetch_until_response(
                target, init, HTTPS_MIRROR_GIT_INITIAL_RESPONSE_TIMEOUT_SECONDS)
            status = int(getattr(upstream, "status", 0) or 0)
        except Exception:
            upstream, status = None, 0
        # A 404 is a replica that has not materialized the repository yet.
        if upstream is None or status == 404 or status >= 500 or (
                status in HTTPS_MIRROR_RETRY_STATUSES):
            continue
        raw_headers = {}
        for header in ("content-type", "content-length", "etag"):
            try:
                value = upstream.headers.get(header)
            except Exception:
                value = None
            if value is not None:
                raw_headers[header] = str(value)
        response_headers = https_routing.response_headers(raw_headers)
        response_headers["cache-control"] = "private, no-store"
        return JsResponse.new(
            upstream.body,
            to_js({"status": status, "headers": response_headers}))
    return json_response(
        {"error": "mirror_unavailable"}, status=503, cache_control="no-store",
        extra_headers=EXPECTED_DEGRADED_HEADERS)
