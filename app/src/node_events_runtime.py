"""node events runtime routes, loaded on demand.

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


async def schedule_repo_mirror_wake(
        env, owner, repo, topic, state="", generation=0):
    """Persist a coalesced mirror wake outside a repository POST path.

    A submitted collaboration item or accepted catalog generation is durable
    before this runs. The doorbell moves the potentially many-peer fan-out off
    that acceptance path and retries a failed handoff. Repeating a wake only
    updates the same bounded repository/topic key, so a commit burst delivers
    the latest refs target instead of creating one alarm per commit.
    """
    owner = (safe_segment(owner) or "").lower()
    repo = safe_segment(repo)
    topic = clean_string(topic, 40).strip().lower()
    state = clean_string(state, 64).strip().lower()
    try:
        generation = max(0, int(generation or 0))
    except (TypeError, ValueError, OverflowError):
        generation = 0
    if (
        not owner or not repo
        or topic not in ("issues", "pulls", "discussions", "commits")
        or (state and not re.fullmatch(r"[0-9a-f]{64}", state))
    ):
        return False
    try:
        binding = getattr(env, "FORKMESH_CRON_RUNNER", None)
        if binding is None or js_nullish(binding):
            return False
        query = "owner=%s&repo=%s&topic=%s" % (
            quote(owner, safe=""), quote(repo, safe=""), quote(topic, safe=""))
        if state:
            query += "&state=" + quote(state, safe="")
        if generation:
            query += "&generation=" + quote(str(generation), safe="")
        request = JsRequest.new(
            "https://forkmesh.internal" + MIRROR_WAKE_PATH + "?" + query,
            to_js({
                "method": "POST",
                "signal": JsAbortSignal.timeout(MIRROR_WAKE_DOORBELL_TIMEOUT_MS),
            }),
        )
        doorbell = binding.get(binding.idFromName(MIRROR_WAKE_NAME))
        response = await doorbell.fetch(request)
        return 200 <= int(getattr(response, "status", 0) or 0) < 300
    except BaseException:
        return False
def _node_events_do_name(owner):
    # One Durable Object per OWNER account (not per repo): a node syncs all of
    # its collaboration inboxes with one GET /api/sync and receives targeted
    # repository signals on the same socket. This keeps the DO count bounded by
    # accounts rather than repositories.
    return "nodes:" + str(owner or "").strip().lower()
async def notify_repo_host(
        env, owner, repo, topic, state="", event_owner="", generation=0):
    # Push a minimal "something changed" event frame to the owner's connected
    # desktop/headless node(s) through the ForkMeshNodes Durable Object. The
    # frame carries no payload — just a topic, repository identity, and optional
    # refs fingerprint — and the node performs the corresponding direct Git or
    # signed inbox sync. Commit generations are retained for reconnect replay;
    # other advisory topics retain their existing slow safety path. A failure
    # here must never fail the write that triggered it. (Unlike the
    # retired ForkMeshHost tunnel, this channel never carries repository bytes
    # or control commands — a compromised relay can at most make a node run
    # one extra signed sync.)
    owner = safe_segment(owner)
    repo = safe_segment(repo)
    if not owner or not repo:
        return False
    # The Durable Object is addressed by the account that owns the receiving
    # socket, but a mirror wake-up describes the *source* repository.  Those
    # identities are normally different (for example the `jett` account owns
    # the `mirror29` machine which follows `forkmesh/forkmesh`).  Reusing the
    # socket owner in the event's repo field made desktop mirrors look for
    # `jett/forkmesh`, fail to match their configured `forkmesh/forkmesh`, and
    # fall back to the periodic fetch.  Ordinary owner-inbox notifications omit
    # event_owner and retain their existing owner/repo identity.
    event_owner = safe_segment(event_owner) if event_owner else owner
    if not event_owner:
        return False
    try:
        node_id = env.FORKMESH_NODES.idFromName(_node_events_do_name(owner))
        node_object = env.FORKMESH_NODES.get(node_id)
        # Awaited directly, NOT under asyncio.wait_for: cancelling a JS-backed
        # await leaves the underlying Pyodide task pending forever, which is
        # how a single slow push wedges the isolate for every later request
        # (see the no-concurrent-tasks rule at the top of this module). The
        # frame is payload-free and the Durable Object answers immediately;
        # the platform's own subrequest limits bound the wait.
        _cron_subrequest_charge()
        target_state = clean_string(state or "", 64).strip().lower()
        if not re.fullmatch(r"[0-9a-f]{64}", target_state):
            target_state = ""
        target_query = (
            "&state=" + quote(target_state) if target_state else "")
        try:
            generation = max(0, int(generation or 0))
        except (TypeError, ValueError, OverflowError):
            generation = 0
        if generation:
            target_query += "&generation=" + quote(str(generation))
        response = await node_object.fetch(
            "https://forkmesh.internal/api/nodes/notify"
            "?topic=" + quote(topic or "") +
            "&repo=" + quote(event_owner + "/" + repo) + target_query
        )
        status = getattr(response, "status", None)
        if status is None:
            return not isinstance(response, dict) or bool(response.get("ok"))
        return 200 <= int(status or 0) < 300
    except Exception:
        return False
async def notify_repo_mirrors(
        env, owner, repo, topic, state="", generation=0):
    """Wake every known public mirror after its source changes."""
    try:
        context = await _https_mirror_public_context(env, owner, repo)
    except Exception:
        return False
    if not context:
        return True
    source_owner = str(owner or "").strip().lower()
    # `nodes` is the serving allowlist. A mirror that fell outside the source's
    # retained pin window is intentionally absent from it — but that is exactly
    # the mirror that most needs a wake-up. Fan the payload-free hint to every
    # catalog member in the verified group; routing continues to use `nodes`.
    group_nodes = context.get("groupNodes") or context.get("nodes", set())
    mirror_nodes = {
        str(value or "").strip().lower()
        for value in group_nodes
        if valid_node_name(str(value or "").strip().lower())
    }
    # A headless mirror opens its event socket under the node account, while a
    # desktop node linked to a person opens the same socket under that owner's
    # account. The HTTPS catalog is keyed by machine name, so notifying only
    # `mirror2` silently missed a live socket authenticated as (for example)
    # `jett`. Fan the payload-free hint to both identities. The node ownership
    # relation is server-written and the query is bounded by the already
    # bounded integrity-approved mirror set.
    core_nodes = set()
    core_nodes_of = globals().get("hosted_core_nodes")
    if callable(core_nodes_of):
        try:
            core_nodes = {
                str(value or "").strip().lower()
                for value in (core_nodes_of(env) or [])
                if valid_node_name(str(value or "").strip().lower())
            }
        except Exception:
            core_nodes = set()
    core_owners = set(core_nodes)
    core_owners_of = globals().get("hosted_core_owners")
    if core_nodes and callable(core_owners_of):
        try:
            core_owners |= {
                str(value or "").strip().lower()
                for value in (core_owners_of(env) or [])
            }
        except Exception:
            pass
    targets = set(mirror_nodes)
    if source_owner in core_owners:
        targets |= core_nodes
    ownership_complete = True
    if mirror_nodes:
        try:
            placeholders = ",".join("?" for _node in mirror_nodes)
            rows = await d1_all(
                env,
                "SELECT lower(n.name) AS node_name,lower(u.username) AS owner "
                "FROM nodes n LEFT JOIN users u ON u.user_bi=n.user_bi "
                "WHERE lower(n.name) IN (" + placeholders + ")",
                *sorted(mirror_nodes),
            )
            for row in rows or []:
                node = str(row.get("node_name") or "").strip().lower()
                linked_owner = str(row.get("owner") or "").strip().lower()
                if node in mirror_nodes and valid_node_name(linked_owner):
                    targets.add(linked_owner)
        except Exception:
            # The machine-name notification remains the fallback for headless
            # nodes and for a transient ownership lookup failure.
            ownership_complete = False
    delivered = ownership_complete
    for target in sorted(targets):
        if not valid_node_name(target) or (
                target == source_owner and target not in core_nodes):
            continue
        if state:
            accepted = await notify_repo_host(
                env, target, repo, topic, state, event_owner=source_owner,
                generation=generation)
        else:
            accepted = await notify_repo_host(
                env, target, repo, topic, event_owner=source_owner,
                generation=generation)
        if accepted is False:
            delivered = False
    return delivered


async def _load_node_commit_events(node):
    raw = await node.ctx.storage.get(NODE_COMMIT_REPLAY_STORAGE_KEY)
    if js_nullish(raw):
        return {}
    if hasattr(raw, "to_py"):
        raw = raw.to_py()
    if isinstance(raw, str):
        try:
            raw = json.loads(raw)
        except Exception:
            return {}
    return raw if isinstance(raw, dict) else {}


def _stored_commit_generation(value):
    if not isinstance(value, dict):
        return 0
    try:
        return max(0, int(value.get("generation") or 0))
    except (TypeError, ValueError, OverflowError):
        return 0


async def remember_node_commit_event(node, frame):
    """Retain only the newest commit convergence target for one repository."""
    pending = await _load_node_commit_events(node)
    repo = clean_string(frame.get("repo", ""), 200).strip().lower()
    state = clean_string(frame.get("state", ""), 64).strip().lower()
    generation = max(0, int(frame.get("generation") or 0))
    if not repo or not re.fullmatch(r"[0-9a-f]{64}", state):
        raise ValueError("invalid commit replay")
    prior = pending.get(repo)
    if isinstance(prior, dict):
        prior_generation = _stored_commit_generation(prior)
        if prior_generation > generation:
            return False
        if (
            prior_generation == generation
            and clean_string(prior.get("state", ""), 64).strip().lower()
                == state
        ):
            return False
    if repo not in pending and len(pending) >= NODE_COMMIT_REPLAY_MAX:
        oldest = min(
            pending,
            key=lambda key: _stored_commit_generation(pending.get(key)),
        )
        pending.pop(oldest, None)
    pending[repo] = {
        "repo": repo, "state": state, "generation": generation,
    }
    await node.ctx.storage.put(
        NODE_COMMIT_REPLAY_STORAGE_KEY,
        json.dumps(pending, separators=(",", ":")),
    )
    return True


async def replay_node_commit_events(node, ws):
    """Replay each repository's latest retained commit target on reconnect."""
    try:
        pending = await _load_node_commit_events(node)
    except Exception:
        return
    valid = [
        value for value in (
            pending.values() if isinstance(pending, dict) else [])
        if isinstance(value, dict)
    ]
    for item in sorted(valid, key=_stored_commit_generation):
        repo = clean_string(item.get("repo", ""), 200).strip().lower()
        state = clean_string(item.get("state", ""), 64).strip().lower()
        generation = _stored_commit_generation(item)
        if not repo or not generation or not re.fullmatch(
                r"[0-9a-f]{64}", state):
            continue
        frame = json.dumps({
            "type": "event", "topic": "commits", "repo": repo,
            "state": state, "generation": generation,
        }, separators=(",", ":"))
        try:
            ws.send(frame)
        except Exception:
            return
        durable_object_traffic_note(
            node, bytes_out=len(frame.encode("utf-8")), messages=1)
