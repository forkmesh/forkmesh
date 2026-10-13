"""Mirror grouping, clone-fallback selection, and state-pin helpers.

Pure (builtin-only, no ``js``/``workers`` runtime, no ``env``/DB access) functions
pulled out of the ~11k-line ``entry.py`` so the logic that decides how repo
mirrors group together, which healthy mirror should serve a clone when the named
source-of-truth host is offline, and which attested repo states a mirror may
serve, all live in one small, independently-testable module. Imported back into
``entry.py`` like ``urls.py``/``solana.py``/``git_http.py`` (adhoc #215); the AST
tests load these helpers straight from this file.
"""


def _mirror_ms(value):
    try:
        n = int(value)
    except (TypeError, ValueError):
        return None
    return n if n > 0 else None


def repo_mirror_group_key(record):
    root = str((record or {}).get("rootCommit") or "").strip().lower()
    if root:
        return "root:" + root
    name = str((record or {}).get("name") or "").strip().lower()
    return "name:" + name


def repo_mirror_same_group(target, record):
    # Whether `record` mirrors the same logical repo as `target`. They match when
    # they share a root (first) commit. A mirror cloned from the relay can have an
    # unset HEAD and so publish an empty rootCommit (issue #243); when either side
    # lacks a root, fall back to matching the repo name so such a mirror still
    # groups with its source of truth instead of vanishing from the owner's
    # mirror-nodes list. If both roots are present but differ, trust a ForkMesh
    # clone URL that points at the other row's owner/name: older mirror records
    # could publish the wrong root while still clearly being clones of the source
    # repo, and those must still show up and serve as live mirrors. Otherwise two
    # differing non-empty roots mean a genuine fork with rewritten history, which
    # stays in its own group.
    def owner_name(rec):
        return (
            str((rec or {}).get("owner") or "").strip().lower(),
            str((rec or {}).get("name") or "").strip().lower(),
        )

    def clone_target(rec):
        raw = str((rec or {}).get("cloneUrl") or "").strip()
        if not raw:
            return "", ""
        path = raw
        if "://" in path:
            path = path.split("://", 1)[1]
            path = path.split("/", 1)[1] if "/" in path else ""
        elif path.startswith("git@") and ":" in path:
            path = path.split(":", 1)[1]
        path = path.split("?", 1)[0].split("#", 1)[0].strip("/")
        parts = [p for p in path.split("/") if p]
        if len(parts) < 2:
            return "", ""
        owner = parts[-2].strip().lower()
        name = parts[-1].strip().lower()
        if name.endswith(".git"):
            name = name[:-4]
        return owner, name

    def clone_points_to(src, dst):
        target_owner, target_name = clone_target(src)
        dst_owner, dst_name = owner_name(dst)
        return bool(
            target_owner and target_name and dst_owner and dst_name and
            target_owner == dst_owner and target_name == dst_name
        )

    troot = str((target or {}).get("rootCommit") or "").strip().lower()
    rroot = str((record or {}).get("rootCommit") or "").strip().lower()
    if troot and rroot:
        return (
            troot == rroot or
            clone_points_to(record, target) or
            clone_points_to(target, record)
        )
    tname = str((target or {}).get("name") or "").strip().lower()
    rname = str((record or {}).get("name") or "").strip().lower()
    return bool(tname) and tname == rname


def agent_provider_mirror_candidates(
    records, target, source_node, provider, now, fresh_ms,
):
    """Return fresh same-repo headless nodes that signed the required runtime.

    Agent jobs are claimed over the polling API, so a direct HTTPS clone
    endpoint is neither required nor sufficient. The catalog-v2 record is the
    account-signed capability lease; the node still performs a local binary
    and login check before it executes a claimed job.
    """
    provider = str(provider or "").strip().lower()
    if provider not in {"claude-code", "codex", "local-llama"}:
        return []
    source = str(source_node or "").strip().lower()
    try:
        now_ms = int(now)
        lease_ms = max(1, int(fresh_ms))
    except (TypeError, ValueError):
        return []
    eligible = []
    seen_nodes = set()
    for rec in records or []:
        rec = rec if isinstance(rec, dict) else {}
        if rec.get("visibility") == "private":
            continue
        if not repo_mirror_same_group(target, rec):
            continue
        node = (
            str(rec.get("machineName") or "").strip()
            or str(rec.get("owner") or "").strip()
        ).lower()
        if not node or node == source or node in seen_nodes:
            continue
        providers = rec.get("agentProviders")
        if not isinstance(providers, list) or provider not in {
            str(value or "").strip().lower() for value in providers
        }:
            continue
        last_sync = _mirror_ms(rec.get("lastSync"))
        if not last_sync or now_ms - last_sync > lease_ms:
            continue
        seen_nodes.add(node)
        eligible.append((last_sync, node))
    eligible.sort(key=lambda item: (-item[0], item[1]))
    return [node for _last_sync, node in eligible]


def agent_provider_target_decision(
    records,
    target,
    source_node,
    provider,
    now,
    fresh_ms,
    preferred_node="",
    org_owner=False,
    source_owned=False,
    platform_admin=False,
):
    """Choose a durable owner desktop or a fresh capable mirror.

    The repository's retained source catalog record is a durable, signed
    capability statement. An organization owner may therefore queue to their
    own linked desktop while it is offline. Claiming and execution remain
    separately authorized by the node-owner signature and the local fail-closed
    safety preflight. Other routes keep the short publication lease required
    for unattended mirrors.
    """

    source = str(source_node or "").strip().lower()
    provider = str(provider or "").strip().lower()
    preferred = str(preferred_node or "").strip().lower()
    provider_label = {
        "codex": "Codex", "local-llama": "Gemma (local)",
    }.get(provider, "Claude Code")

    def rejected(code, message, action, retryable=False, target_node=""):
        return {
            "ok": False,
            "error": code,
            "message": message,
            "requiredAction": action,
            "retryable": bool(retryable),
            "provider": provider,
            "targetNode": target_node or source,
        }

    if not isinstance(target, dict):
        return rejected(
            "repository_not_published",
            (
                "The linked repository has not been published by its source "
                "desktop. Open ForkMesh on that desktop and publish the "
                "repository once."
            ),
            "publish_repository",
        )

    source_runtime = str(target.get("runtimeMode") or "").strip().lower()
    source_providers = {
        str(value or "").strip().lower()
        for value in (
            target.get("agentProviders")
            if isinstance(target.get("agentProviders"), list)
            else []
        )
    }
    try:
        now_ms = int(now)
        source_seen_at = int(
            target.get("updatedAt") or target.get("lastSync") or 0)
        lease_ms = max(1, int(fresh_ms))
    except (TypeError, ValueError):
        now_ms = 0
        source_seen_at = 0
        lease_ms = 1
    source_online = bool(
        source_seen_at and now_ms - source_seen_at <= lease_ms)

    def source_desktop_decision():
        if source_runtime != "desktop":
            return rejected(
                "target_not_desktop",
                (
                    "The linked source node is not an attended desktop. Link "
                    "this repository to the desktop that should run agent "
                    "tasks."
                ),
                "link_desktop_target",
            )
        if not source_owned:
            return rejected(
                "target_not_owned",
                (
                    "The linked desktop is not owned by your account. Link a "
                    "desktop you own before queueing an organization agent."
                ),
                "link_owned_desktop",
            )
        if provider not in source_providers:
            return rejected(
                "provider_not_advertised",
                (
                    f"The linked desktop has not advertised {provider_label}. "
                    f"Open ForkMesh there, install or sign in to "
                    f"{provider_label}, and enable website agent publishing."
                ),
                "publish_provider_capability",
            )
        if org_owner or (platform_admin and source_online):
            return {
                "ok": True,
                "targetNode": source,
                "targetOnline": source_online,
                "queueState": (
                    "ready_for_claim"
                    if source_online else "waiting_for_desktop"
                ),
                "route": (
                    "owner_desktop"
                    if org_owner else "platform_admin_desktop"
                ),
            }
        return rejected(
            "org_owner_required",
            (
                "Only an organization owner may save work for an offline "
                "desktop. Ask an owner to queue it, or bring an eligible "
                "agent mirror online."
            ),
            "ask_organization_owner",
        )

    eligible = agent_provider_mirror_candidates(
        records, target, source, provider, now_ms, lease_ms)
    if preferred:
        if org_owner and preferred != source:
            return rejected(
                "preferred_target_ineligible",
                (
                    "Organization-owner offline queues must target the linked "
                    "desktop they own. Select that desktop or update the "
                    "repository link."
                ),
                "select_linked_desktop",
                target_node=preferred,
            )
        if preferred == source:
            return source_desktop_decision()
        if preferred in eligible:
            return {
                "ok": True,
                "targetNode": preferred,
                "targetOnline": True,
                "queueState": "ready_for_claim",
                "route": "online_mirror",
            }
        return rejected(
            "preferred_target_ineligible",
            (
                "The requested target is not a fresh repository mirror that "
                f"advertises {provider_label}. Select the linked desktop or "
                "bring that mirror online with agent publishing enabled."
            ),
            "select_eligible_target",
            retryable=True,
            target_node=preferred,
        )

    # An organization owner's linked desktop is authoritative for this route.
    # Return its exact configuration problem instead of silently dispatching
    # the owner's prompt to a different machine.
    if org_owner:
        return source_desktop_decision()
    if (
        platform_admin
        and source_runtime == "desktop"
        and source_owned
        and provider in source_providers
        and source_online
    ):
        return {
            "ok": True,
            "targetNode": source,
            "targetOnline": True,
            "queueState": "ready_for_claim",
            "route": "platform_admin_desktop",
        }
    if eligible:
        return {
            "ok": True,
            "targetNode": eligible[0],
            "targetOnline": True,
            "queueState": "ready_for_claim",
            "route": "online_mirror",
        }
    return rejected(
        "no_online_agent_mirror",
        (
            f"No online repository mirror currently advertises "
            f"{provider_label}. Bring a capable mirror online or ask an "
            "organization owner to queue the task for their linked desktop."
        ),
        "bring_agent_mirror_online",
        retryable=True,
    )


def mirroring_owner_set(records):
    # Owners (node names) that host at least one repo ALSO hosted by a DIFFERENT
    # owner — i.e. a repo genuinely mirrored across nodes. Repos that only one
    # owner hosts (a single repo with no mirror elsewhere) don't qualify their
    # owner. Grouping reuses repo_mirror_group_key (root commit, name fallback)
    # so two nodes serving the same logical repo land in one group. Used to keep
    # nodes that mirror nothing for anyone out of the donation split (issue #94).
    groups = {}
    for rec in records or []:
        owner = str((rec or {}).get("owner") or "").strip()
        if not owner:
            continue
        groups.setdefault(repo_mirror_group_key(rec), set()).add(owner.lower())
    owners = set()
    for members in groups.values():
        if len(members) > 1:
            owners |= members
    return owners


def served_mirror_groups(records):
    # Mirror groups (root-commit / name keyed) that have at least one online,
    # PUBLIC host right now. `records` are catalog records already annotated with
    # rec["liveHost"] (the named node's own host presence). A group lands here as
    # soon as ANY node mirroring that logical repo is live, which is what lets a
    # repo stay cloneable/browsable in place through its own URL while its named
    # source of truth is down (adhoc #61). Private rows never serve a public
    # group, matching the clone fallback in select_clone_fallback.
    groups = set()
    for rec in records or []:
        if (rec or {}).get("liveHost") and (rec or {}).get("visibility") != "private":
            groups.add(repo_mirror_group_key(rec))
    return groups


def repo_clone_online(rec, served_groups):
    # Whether a repo is actually reachable for clone/browse right now: its own
    # named host is live, OR — for a public repo — a peer mirroring the same
    # logical repo is online and the relay will serve it in place (adhoc #61).
    # Private repos get no mirror fallback, so they depend on their own host.
    if (rec or {}).get("liveHost"):
        return True
    if (rec or {}).get("visibility") == "private":
        return False
    return repo_mirror_group_key(rec) in (served_groups or set())


def repo_mirror_group_selection(owner, repo, rows):
    """Resolve <owner>/<repo> to (target, members, public_rows), or None.

    The mirror group of a repo is a small subset of the public catalog, but the
    only way to find it is to look at every catalog record. Callers that need
    just the group's key set — to scope a per-repo D1 read instead of reading
    the whole table — share this selection with build_repo_mirrors_payload so
    the two can never disagree about who is in the group.
    """

    def clone_target(rec):
        raw = str((rec or {}).get("cloneUrl") or "").strip()
        if not raw:
            return "", ""
        path = raw
        if "://" in path:
            path = path.split("://", 1)[1]
            path = path.split("/", 1)[1] if "/" in path else ""
        elif path.startswith("git@") and ":" in path:
            path = path.split(":", 1)[1]
        path = path.split("?", 1)[0].split("#", 1)[0].strip("/")
        parts = [p for p in path.split("/") if p]
        if len(parts) < 2:
            return "", ""
        clone_owner = parts[-2].strip().lower()
        clone_name = parts[-1].strip().lower()
        if clone_name.endswith(".git"):
            clone_name = clone_name[:-4]
        return clone_owner, clone_name

    def canonical_owner_name(rec):
        source = str((rec or {}).get("source") or "").strip()
        if source == "remote-clone":
            clone_owner, clone_name = clone_target(rec)
            if clone_owner and clone_name:
                return clone_owner, clone_name
        return (
            str((rec or {}).get("owner") or "").strip().lower(),
            str((rec or {}).get("name") or "").strip().lower(),
        )

    owner_l = (owner or "").strip().lower()
    repo_l = (repo or "").strip().lower()
    public_rows = []
    target = None
    canonical_target = None
    inferred_target = False
    for row in rows or []:
        rec = row.get("data") or {}
        if row.get("is_private") or rec.get("visibility") == "private":
            continue
        rec_owner = str(rec.get("owner") or "").strip()
        rec_name = str(rec.get("name") or "").strip()
        if not rec_owner or not rec_name:
            continue
        item = {"key_bi": row.get("key_bi"), "data": rec}
        public_rows.append(item)
        if rec_owner.lower() == owner_l and rec_name.lower() == repo_l:
            target = item
        if canonical_target is None and canonical_owner_name(rec) == (owner_l, repo_l):
            canonical_target = item
    if not target:
        if not canonical_target:
            return None
        inferred_target = True
        target = {
            "key_bi": canonical_target.get("key_bi"),
            "data": {
                **canonical_target["data"],
                "owner": owner,
                "name": repo,
                "liveHost": False,
            },
        }

    if inferred_target:
        members = [
            r for r in public_rows
            if canonical_owner_name(r["data"]) == (owner_l, repo_l)
        ]
    else:
        members = [
            r for r in public_rows if repo_mirror_same_group(target["data"], r["data"])
        ]
    return target, members, public_rows


def repo_mirror_group_keys(members):
    """Deduplicated catalog key_bi values for a resolved mirror group.

    Lets a route read only this group's rows out of a per-repo table
    (repo_first_hosted) instead of scanning it for every public repository on
    every request.
    """
    keys = []
    for row in members or []:
        key = str((row or {}).get("key_bi") or "")
        if key and key not in keys:
            keys.append(key)
    return keys


REPO_STATE_HISTORY_KEY_LIMIT = 4


def repo_mirror_pin_history_keys(members, public_rows, primary_key=""):
    """key_bi values whose attested pin history can affect this group's verdicts.

    The requested row stays first, followed by working-copy authorities, then
    other group members only as the legacy no-authority fallback. An authority
    is not always a member of the requested group: clone_state_pins validates
    each member against the local-node rows grouped with IT, and grouping is not
    strictly transitive — a record published without a root commit groups by
    name (see repo_mirror_same_group), so a source can supply pins to a mirror
    without landing in the requested repo's own group.

    Current stateHash attestations remain present on every catalog row. History
    only widens the lag-tolerance window, so deterministically limiting it to
    the primary plus three authority-priority rows preserves the integrity
    boundary while putting a hard ceiling on the D1 history scan.
    """
    member_keys = set(repo_mirror_group_keys(members))
    authority_keys = set()
    for row in public_rows or []:
        key = str((row or {}).get("key_bi") or "")
        if not key:
            continue
        rec = (row or {}).get("data") or {}
        if str(rec.get("source") or "local-node").strip().lower() != "local-node":
            continue
        if not any(
            repo_mirror_same_group((member or {}).get("data") or {}, rec)
            for member in members or []
        ):
            continue
        authority_keys.add(key)

    keys = []
    primary_key = str(primary_key or "")
    if primary_key:
        keys.append(primary_key)
    for key in sorted(authority_keys):
        if key not in keys:
            keys.append(key)
    for key in sorted(member_keys):
        if key not in keys:
            keys.append(key)
    return keys[:REPO_STATE_HISTORY_KEY_LIMIT]


def build_repo_mirrors_payload(
    owner, repo, rows, presence, first_hosted, now, stale_ms, sync_tolerance_ms,
    history=None, linked_canonical=False, reachable_nodes=None,
    routing_verified_nodes=None, selection=None,
):
    # Resolving the group means a pass over every public catalog record. A
    # caller that already ran repo_mirror_group_selection — to scope its D1
    # reads to the group — passes it back in rather than paying for a second
    # identical pass on a route whose failure mode is the CPU limit.
    if selection is None:
        selection = repo_mirror_group_selection(owner, repo, rows)
    if selection is None:
        return None
    target, members, public_rows = selection
    group_key = repo_mirror_group_key(target["data"])

    def exact_state_hash(rec):
        value = str((rec or {}).get("stateHash") or "").strip().lower()
        if (
            len(value) == 64
            and all(ch in "0123456789abcdef" for ch in value)
        ):
            return value
        return ""

    freshest_sync = 0
    for row in members:
        freshest_sync = max(freshest_sync, _mirror_ms(row["data"].get("lastSync")) or 0)
    freshest_states = {
        exact_state_hash(row["data"])
        for row in members
        if (
            (_mirror_ms(row["data"].get("lastSync")) or 0) == freshest_sync
            and exact_state_hash(row["data"])
        )
    }

    # Is the logical repo's source of truth (a working-copy holder —
    # "local-node") reachable through a fresh, healthy direct-HTTPS endpoint?
    # While it is, a mirror with stale refs can re-sync from the source, so it is
    # auto-healing rather than blocked. The hard reject still applies when no
    # healthy source endpoint is available.
    source_online = False
    for row in members:
        rec = row["data"]
        if str(rec.get("source") or "local-node") != "local-node":
            continue
        seen = _mirror_ms((presence or {}).get(row.get("key_bi")))
        if seen and now - seen <= stale_ms:
            source_online = True
            break

    # Organization aliases explicitly appoint one account-owned node as the
    # canonical backing node for /<org>/<repo>. The HTTPS router therefore
    # treats a linked remote-clone target's signed state as the organization's
    # attestation instead of letting an unrelated same-name local-node record
    # override it. Mirror status must use that same trust anchor or it can label
    # the node actively serving a verified org route "rejected".
    #
    # `linked_canonical` is supplied only after the caller verifies the
    # org_repos link in D1. It is deliberately ignored for a local-node target,
    # whose normal source-pin policy is already the stronger/correct one.
    target_record = target["data"]
    canonical_link_mode = bool(
        linked_canonical
        and str(target_record.get("source") or "").strip().lower()
        == "remote-clone"
    )
    canonical_pins = set()
    canonical_online = False
    if canonical_link_mode:
        target_state = str(target_record.get("stateHash") or "").strip().lower()
        if (
            len(target_state) == 64
            and all(ch in "0123456789abcdef" for ch in target_state)
        ):
            canonical_pins.add(target_state)
        for state in (history or {}).get(str(target.get("key_bi") or ""), []) or []:
            state = str(state or "").strip().lower()
            if (
                len(state) == 64
                and all(ch in "0123456789abcdef" for ch in state)
            ):
                canonical_pins.add(state)
        target_seen = _mirror_ms((presence or {}).get(target.get("key_bi")))
        canonical_online = bool(target_seen and now - target_seen <= stale_ms)

    def _int_field(rec, name):
        try:
            return int(rec.get(name))
        except (TypeError, ValueError):
            return -1

    def _optional_metric(rec, name, maximum):
        value = rec.get(name)
        if isinstance(value, bool) or not isinstance(value, int) or value < 0:
            return None
        return min(value, maximum)

    def _optional_usage(rec, used_name, total_name):
        used = _optional_metric(rec, used_name, 1 << 50)
        total = _optional_metric(rec, total_name, 1 << 50)
        if used is None or total is None or total <= 0:
            return None, None
        return min(used, total), total

    def _changed_files(rec):
        files = []
        values = rec.get("changedFiles")
        if not isinstance(values, list):
            return files
        for item in values[:16]:
            path = str(item or "").strip().replace("\\", "/")
            if (
                    not path or len(path) > 160 or path.startswith("/")
                    or path == ".." or path.startswith("../")
                    or path in files
                    or any(ord(char) < 0x20 or ord(char) == 0x7f
                           for char in path)):
                continue
            files.append(path)
            if len(files) >= 8:
                break
        return files

    mirrors = []
    for row in members:
        rec = row["data"]
        node_name = (
            str(rec.get("machineName") or "").strip()
            or str(rec.get("owner") or "").strip()
        )
        routing_verified = bool(
            routing_verified_nodes is not None
            and node_name.lower() in routing_verified_nodes
        )
        key = row.get("key_bi")
        seen = _mirror_ms((presence or {}).get(key))
        hosted = _mirror_ms(rec.get("hostedSince")) or _mirror_ms((first_hosted or {}).get(key))
        last_sync = _mirror_ms(rec.get("lastSync"))
        agent_providers = [
            provider
            for provider in ("claude-code", "codex", "local-llama")
            if provider in {
                str(value or "").strip().lower()
                for value in (
                    rec.get("agentProviders")
                    if isinstance(rec.get("agentProviders"), list)
                    else []
                )
            }
        ]
        # Public byte serving no longer keeps the legacy host WebSocket open.
        # A source-of-truth desktop still signs and publishes its local-node
        # catalog while it is alive, so that fresh publication is its bounded
        # liveness lease. Remote clones keep using independently challenged
        # endpoint presence and cannot self-declare online this way.
        local_publication_seen = (
            last_sync
            if str(rec.get("source") or "local-node") == "local-node"
            else None
        )
        effective_seen = seen or local_publication_seen
        effective_stale_ms = (
            max(stale_ms, 10 * 60 * 1000)
            if local_publication_seen and not seen
            else stale_ms
        )
        online = bool(
            effective_seen and now - effective_seen <= effective_stale_ms)
        if reachable_nodes is not None:
            online = online and node_name.lower() in reachable_nodes
        # A polling headless node may intentionally expose no direct HTTPS
        # clone endpoint. Its fresh, signed agent capability publication still
        # proves the machine is active, while cloneAvailable remains false.
        # This lets World render the cabinet as live/yellow instead of treating
        # an eligible worker as a dead machine.
        agent_runtime_online = bool(
            agent_providers
            and last_sync
            and now - last_sync <= 10 * 60 * 1000
        )
        node_online = online or agent_runtime_online
        try:
            size_bytes = max(0, int(rec.get("sizeBytes") or 0))
        except (TypeError, ValueError):
            size_bytes = 0
        exact_current_state = exact_state_hash(rec)
        state_hash = str(rec.get("stateHash") or "").strip().lower()
        sync_timestamp_behind = bool(
            last_sync and freshest_sync and freshest_sync - last_sync > sync_tolerance_ms
        )
        # `lastSync` is a publication timestamp, not repository content. Two
        # independently renewed mirrors can publish the same signed all-refs
        # state minutes apart; the older timestamp must not label exact,
        # integrity-equivalent content as behind. Legacy records without a
        # valid state hash retain the conservative timestamp fallback.
        behind = bool(
            sync_timestamp_behind
            and not (
                exact_current_state
                and exact_current_state in freshest_states
            )
        )
        issue_count = _int_field(rec, "issueCount")
        # Clones / website serves this node has provided; -1 == not advertised
        # (older peer or a record predating the counters), shown as an em-dash.
        clones_served = _int_field(rec, "clonesServed")
        website_served = _int_field(rec, "websiteServed")
        cpu_percent = _optional_metric(rec, "cpuPercent", 100)
        mem_used, mem_total = _optional_usage(
            rec, "memUsedBytes", "memTotalBytes")
        disk_used, disk_total = _optional_usage(
            rec, "diskUsedBytes", "diskTotalBytes")
        actions_enabled = rec.get("actionsEnabled") is True
        actions_state = str(rec.get("actionsState") or "").strip()
        if (
            (not actions_enabled and actions_state != "disabled")
            or (actions_enabled and actions_state not in {"enabled", "running"})
        ):
            actions_enabled = False
            actions_state = "disabled"
        # Would the clone integrity gate serve this node right now? Its published
        # refs fingerprint (stateHash, the same one it signs on publish) must be
        # a state the group's trust anchor attested: normally a working-copy
        # holder's current/recent pin, or the explicitly appointed backing
        # node's pin for an organization alias. Otherwise every clone it serves
        # is rejected with "repository failed integrity check" (see
        # clone_state_pins). Verdicts: "ok" (matches a pin, or the legacy
        # unlinked gate is unpinned), "rejected" (matches no attested state and
        # no quorum of peers vouches for it), "healing" (matches nothing yet,
        # but the anchor is online, so it can re-sync), "unknown" (no
        # fingerprint published). Once no mirror is left inside the anchor's
        # window the pin set already includes what two peers independently
        # agree on, so "rejected" means the fleet itself does not recognize
        # this node's state — not merely that its source went away.
        pins = (
            canonical_pins
            if canonical_link_mode
            else clone_state_pins(rec, key, public_rows, history)
        )
        integrity_anchor_online = (
            canonical_online if canonical_link_mode else source_online
        )
        # A modern HTTPS verifier proves the exact state the router authorizes:
        # the default branch and release tags, signed by the endpoint and
        # accepted by the repository quorum. That is stronger and more current
        # serving evidence than the catalog's all-refs hash, which also changes
        # for harmless agent/PR branch churn. Keep the catalog-pin path for old
        # endpoints that have no verifier verdict.
        if routing_verified:
            integrity = "ok"
        elif canonical_link_mode and not pins:
            # Unlike the legacy unlinked path, an explicit organization route
            # is fail-closed until its appointed backing node publishes an
            # authenticated refs digest.
            integrity = "unknown" if not state_hash else "rejected"
        elif not pins or state_hash in pins:
            integrity = "ok"
        elif not state_hash:
            integrity = "unknown"
        elif integrity_anchor_online:
            integrity = "healing"
        else:
            integrity = "rejected"
        if not node_online:
            activity = "offline"
        elif actions_state == "running":
            activity = "running-actions"
        elif behind:
            # The node is live but its last signed publication is not the
            # freshest exact refs state. This is a truthful, Worker-observable
            # "sync pending/in progress" signal without guessing which local
            # process is currently consuming CPU.
            activity = "syncing"
        elif integrity == "healing":
            activity = "verifying"
        elif integrity == "rejected":
            activity = "integrity-blocked"
        elif integrity == "unknown":
            activity = "awaiting-verification"
        else:
            activity = "serving"
        serving = online and integrity == "ok"
        mirrors.append({
            # A node is a machine, not the user/account that owns its catalog
            # row. Older publishers did not advertise machineName, so retain
            # owner only as a compatibility fallback.
            "node": node_name,
            "owner": str(rec.get("owner") or "").strip(),
            "ownerUser": str(rec.get("ownerUser") or "").strip(),
            # The publishing machine's advertised node name (may differ from
            # the owning account); display-only, never an identity key.
            "machineName": str(rec.get("machineName") or "").strip(),
            "repo": str(rec.get("name") or "").strip(),
            "status": "online" if node_online else "offline",
            "lastSeen": (
                effective_seen
                if online
                else last_sync if agent_runtime_online else None
            ),
            "hostedSince": hosted,
            "syncAgeMs": max(0, now - last_sync) if last_sync else None,
            "lastSync": last_sync,
            "behind": behind,
            "cloneAvailable": serving,
            **({
                "mirrorUrl": str(rec.get("mirrorUrl") or "").strip(),
            } if rec.get("mirrorUrl") else {}),
            "sizeBytes": size_bytes,
            "source": str(rec.get("source") or "").strip(),
            # Node facts the publishing node mirrored into its catalog record, so the
            # Mirror nodes view fills these columns even for an offline node (adhoc #56).
            "commit": str(rec.get("commit") or "").strip(),
            "branch": str(rec.get("branch") or "").strip(),
            # What that commit says and who wrote it, mirrored from the node's
            # signed record, so a hash alone isn't all a viewer gets. Optional
            # catalog-v2 extensions: None when the node never published them.
            "lastCommitMessage": (
                str(rec.get("commitSubject") or "").strip() or None),
            "lastCommitAuthorName": (
                str(rec.get("commitAuthorName") or "").strip() or None),
            "lastCommitAt": _mirror_ms(rec.get("commitAt")),
            # The paths come from the same signed head record as the commit
            # subject. Expose them here so World activity can describe the
            # verified commit rather than echoing a socket doorbell.
            "changedFiles": _changed_files(rec),
            "issueCount": issue_count,
            "commitCount": _int_field(rec, "commitCount"),
            "branchCount": _int_field(rec, "branchCount"),
            "pullCount": _int_field(rec, "pullCount"),
            "discussionCount": _int_field(rec, "discussionCount"),
            "worktreeCount": _int_field(rec, "worktreeCount"),
            "artifactCount": _int_field(rec, "artifactCount"),
            # A filtered or metadata-only replica (www/docs/offline-sync.md)
            # declares its scope; the availability proof never counts it as a
            # verified full copy. Absent on full mirrors and legacy records.
            **({
                "replicaScope": str(rec.get("replicaScope") or "").strip().lower()[:32],
            } if str(rec.get("replicaScope") or "").strip() else {}),
            "platform": str(rec.get("platform") or "").strip(),
            "runtimeMode": (
                str(rec.get("runtimeMode") or "").strip().lower()
                if str(rec.get("runtimeMode") or "").strip().lower()
                in {"desktop", "headless"}
                else ""
            ),
            "version": str(rec.get("version") or "").strip(),
            "agentProviders": agent_providers,
            # Missing and malformed legacy records fail closed to disabled.
            # Only the bounded status pair signed into catalog-v2 is exposed.
            "actionsEnabled": actions_enabled,
            "actionsState": actions_state,
            "id": str(rec.get("nodeId") or "").strip(),
            "clonesServed": clones_served,
            "websiteServed": website_served,
            # When those two counters last moved, and the bounded class of
            # client served ("git-client", "browser", ...). Optional catalog-v2
            # extensions: None when the node has served none yet, or runs a
            # build that predates the stamps -- never a fabricated "just now".
            "cloneServedAt": _mirror_ms(rec.get("cloneServedAt")),
            "cloneServedAgent": (
                str(rec.get("cloneServedAgent") or "").strip() or None),
            "websiteServedAt": _mirror_ms(rec.get("websiteServedAt")),
            "websiteServedAgent": (
                str(rec.get("websiteServedAgent") or "").strip() or None),
            # Optional, operator-approved public host telemetry. These values
            # were bounded and signed as part of catalog-v2; preserve None as
            # "not shared" rather than inventing a zero for an older/opted-out
            # node.
            "cpuPercent": cpu_percent,
            "memUsedBytes": mem_used,
            "memTotalBytes": mem_total,
            "diskUsedBytes": disk_used,
            "diskTotalBytes": disk_total,
            "integrity": integrity,
            "activity": activity,
            "activityUpdatedAt": (
                effective_seen if online else last_sync
            ),
        })

    mirrors.sort(
        key=lambda m: (
            0 if m["status"] == "online" else 1,
            -(m["lastSync"] or 0),
            m["node"].lower(),
        )
    )
    hosted_values = [m["hostedSince"] for m in mirrors if m["hostedSince"]]
    return {
        "ok": True,
        "owner": target["data"].get("owner"),
        "repo": target["data"].get("name"),
        "groupKey": group_key,
        "generatedAt": now,
        "summary": {
            "mirrors": len(mirrors),
            "online": sum(1 for m in mirrors if m["status"] == "online"),
            "cloneAvailable": sum(1 for m in mirrors if m["cloneAvailable"]),
            "dataHostedBytes": sum(m["sizeBytes"] for m in mirrors),
            "longestHostedSince": min(hosted_values) if hosted_values else None,
            "freshestSync": freshest_sync or None,
        },
        "mirrors": mirrors,
    }


def select_clone_fallback(owner, repo, rows, presence, now, stale_ms, source_online,
                          rotate=0):
    # Pick a healthy, online mirror to serve a clone of owner/repo from when the
    # named owner's own host is offline. This is what keeps a repo cloneable when
    # the source of truth goes down: a clone of /owner/repo is redirected to a peer
    # that mirrors the SAME logical repo (grouped by root commit, name fallback) and
    # is online right now. Returns the fallback owner's name, or None to fall
    # through to the normal named-owner host route.
    #
    # When several mirrors qualify, `rotate` (a per-repo counter the caller bumps
    # on every fallback) spreads clone traffic across them round-robin instead of
    # always hammering the single freshest mirror. rotate=0 keeps the
    # freshest-first pick.
    #
    # Pure (no I/O) so it is unit-testable like build_repo_mirrors_payload; the
    # caller gathers the catalog rows + host_presence map and whether the named
    # owner is currently online. We never redirect away from an online source.
    owner_l = (owner or "").strip().lower()
    repo_l = (repo or "").strip().lower()
    if not owner_l or not repo_l or source_online:
        return None
    public = []
    target = None
    for row in rows or []:
        rec = row.get("data") or {}
        if row.get("is_private") or rec.get("visibility") == "private":
            continue
        rec_owner = str(rec.get("owner") or "").strip()
        rec_name = str(rec.get("name") or "").strip()
        if not rec_owner or not rec_name:
            continue
        item = {"key_bi": row.get("key_bi"), "data": rec}
        public.append(item)
        if rec_owner.lower() == owner_l and rec_name.lower() == repo_l:
            target = item
    # When the source itself never published a record we can still group by name so
    # an offline-but-unpublished source can fall back to a name-matching mirror.
    target_data = target["data"] if target else {
        "owner": owner, "name": repo, "rootCommit": ""}
    candidates = []
    for item in public:
        rec = item["data"]
        rec_owner = str(rec.get("owner") or "").strip()
        if not rec_owner or rec_owner.lower() == owner_l:
            continue  # never redirect to the (offline) source owner itself
        if not repo_mirror_same_group(target_data, rec):
            continue
        seen = _mirror_ms(presence.get(item.get("key_bi")))
        if not (seen and now - seen <= stale_ms):
            continue  # only redirect to a mirror that is actually online
        sync = _mirror_ms(rec.get("lastSync")) or 0
        candidates.append((sync, seen, rec_owner))
    if not candidates:
        return None
    # Freshest-synced first, then most-recently-seen, then name for a stable order.
    candidates.sort(key=lambda c: (-c[0], -c[1], c[2].lower()))
    # Round-robin across the eligible online mirrors so the load of serving a
    # downed repo is spread over all of them rather than landing on one mirror.
    return candidates[rotate % len(candidates)][2]


def browse_mirror_candidates(owner, repo, rows, presence, now, stale_ms):
    # Ordered list of node owners that can serve a website browse of owner/repo
    # right now: every ONLINE mirror of the same logical repo, INCLUDING the named
    # source itself. Freshest-synced first, then most-recently-seen, then name, so
    # the round-robin order is stable. Unlike select_clone_fallback (which only
    # diverts when the named host is offline and never lists the source), this
    # spreads normal page loads across all live mirrors so the website can rotate
    # over them and show which node served each request. Returns [] when nothing is
    # online. Pure (no I/O) so it is unit-testable like build_repo_mirrors_payload.
    owner_l = (owner or "").strip().lower()
    repo_l = (repo or "").strip().lower()
    if not owner_l or not repo_l:
        return []
    public = []
    target = None
    for row in rows or []:
        rec = row.get("data") or {}
        if row.get("is_private") or rec.get("visibility") == "private":
            continue
        rec_owner = str(rec.get("owner") or "").strip()
        rec_name = str(rec.get("name") or "").strip()
        if not rec_owner or not rec_name:
            continue
        item = {"key_bi": row.get("key_bi"), "data": rec}
        public.append(item)
        if rec_owner.lower() == owner_l and rec_name.lower() == repo_l:
            target = item
    # When the source itself never published a record we can still group by name.
    target_data = target["data"] if target else {
        "owner": owner, "name": repo, "rootCommit": ""}
    candidates = []
    for item in public:
        rec = item["data"]
        rec_owner = str(rec.get("owner") or "").strip()
        if not rec_owner:
            continue
        if not repo_mirror_same_group(target_data, rec):
            continue
        seen = _mirror_ms(presence.get(item.get("key_bi")))
        if not (seen and now - seen <= stale_ms):
            continue  # only serve from a mirror that is actually online
        sync = _mirror_ms(rec.get("lastSync")) or 0
        candidates.append((sync, seen, rec_owner))
    candidates.sort(key=lambda c: (-c[0], -c[1], c[2].lower()))
    ordered = []
    seen_owners = set()
    for _sync, _seen, name in candidates:
        low = name.lower()
        if low in seen_owners:
            continue  # one live node serves a logical repo once
        seen_owners.add(low)
        ordered.append(name)
    return ordered


def release_blob_mirror_candidates(owner, repo, rows, presence, now, stale_ms):
    # Ordered list of online nodes likely to serve a release CAS blob for
    # owner/repo. This is intentionally close to browse_mirror_candidates, but
    # release bytes live outside git, so prefer nodes that have published a
    # positive artifactCount before falling back to legacy/unknown records. The
    # caller still verifies by hash and retries on 404; artifactCount is a
    # priority signal, not proof that this exact sha256 is present.
    owner_l = (owner or "").strip().lower()
    repo_l = (repo or "").strip().lower()
    if not owner_l or not repo_l:
        return []
    public = []
    target = None
    for row in rows or []:
        rec = row.get("data") or {}
        if row.get("is_private") or rec.get("visibility") == "private":
            continue
        rec_owner = str(rec.get("owner") or "").strip()
        rec_name = str(rec.get("name") or "").strip()
        if not rec_owner or not rec_name:
            continue
        item = {"key_bi": row.get("key_bi"), "data": rec}
        public.append(item)
        if rec_owner.lower() == owner_l and rec_name.lower() == repo_l:
            target = item
    target_data = target["data"] if target else {
        "owner": owner, "name": repo, "rootCommit": ""}
    candidates = []
    for item in public:
        rec = item["data"]
        rec_owner = str(rec.get("owner") or "").strip()
        if not rec_owner:
            continue
        if not repo_mirror_same_group(target_data, rec):
            continue
        seen = _mirror_ms(presence.get(item.get("key_bi")))
        if not (seen and now - seen <= stale_ms):
            continue
        sync = _mirror_ms(rec.get("lastSync")) or 0
        try:
            artifacts = int(rec.get("artifactCount") or 0)
        except (TypeError, ValueError):
            artifacts = 0
        candidates.append((artifacts > 0, sync, seen, rec_owner))
    candidates.sort(key=lambda c: (0 if c[0] else 1, -c[1], -c[2], c[3].lower()))
    ordered = []
    seen_owners = set()
    for _has_artifacts, _sync, _seen, name in candidates:
        low = name.lower()
        if low in seen_owners:
            continue
        seen_owners.add(low)
        ordered.append(name)
    return ordered


# --- Peer mirror requests (issue #385) --------------------------------------
# A repo owner can ask another node to mirror their repo ("ask node to mirror
# your repo"): the target gets a notification and, if its holder accepts, its
# node starts mirroring. The request lifecycle (pending -> accepted/rejected)
# is parked as a small bounded list on the TARGET account record (the same
# heartbeat-delivered-marker pattern as claim_pending / ownership_transfer),
# and accepted requests ride back on that node's signed heartbeat so the
# desktop node clones the repo. These helpers are pure list/dict manipulation
# (no I/O), unit-testable like the mirror-grouping functions above; entry.py
# imports them and persists the list inside the encrypted account blob.
MAX_MIRROR_REQUESTS = 50
MIRROR_REQUEST_STATUSES = ("pending", "accepted", "rejected")


def mirror_request_id(owner, repo, target):
    # Stable id so re-asking the same node to mirror the same repo updates the
    # one pending request instead of piling up duplicates.
    owner_l = str(owner or "").strip().lower()
    repo_l = str(repo or "").strip().lower()
    target_l = str(target or "").strip().lower()
    return owner_l + "/" + repo_l + "@" + target_l


def add_mirror_request(requests, entry):
    # Insert (or replace, by id) a request into the bounded, newest-first list.
    # Returns a new list; the caller assigns it back onto the account record.
    entry_id = str((entry or {}).get("id") or "").strip()
    if not entry_id:
        return list(requests or [])
    kept = [r for r in (requests or []) if str((r or {}).get("id") or "") != entry_id]
    kept.insert(0, dict(entry))
    return kept[:MAX_MIRROR_REQUESTS]


def find_mirror_request(requests, entry_id):
    entry_id = str(entry_id or "").strip()
    for r in requests or []:
        if str((r or {}).get("id") or "") == entry_id:
            return r
    return None


def set_mirror_request_status(requests, entry_id, status, ts=0):
    # Mutate the matching request's status in place (pending -> accepted/rejected).
    # Returns (new_list, updated_entry_or_None).
    entry_id = str(entry_id or "").strip()
    status = status if status in MIRROR_REQUEST_STATUSES else "pending"
    out = []
    updated = None
    for r in requests or []:
        rec = dict(r or {})
        if str(rec.get("id") or "") == entry_id:
            rec["status"] = status
            if ts:
                rec["resolvedAt"] = int(ts)
            updated = rec
        out.append(rec)
    return out, updated


def accepted_mirror_requests(requests):
    # The accepted-and-not-yet-acknowledged repos this node should mirror, as the
    # minimal {id, owner, repo} the heartbeat reply carries to the desktop node.
    out = []
    for r in requests or []:
        rec = r or {}
        if str(rec.get("status") or "") != "accepted":
            continue
        owner = str(rec.get("owner") or "").strip()
        repo = str(rec.get("repo") or "").strip()
        entry_id = str(rec.get("id") or "").strip()
        if not owner or not repo or not entry_id:
            continue
        out.append({"id": entry_id, "owner": owner, "repo": repo})
    return out


def ack_mirror_requests(requests, ids):
    # Drop the accepted requests the node has confirmed it acted on (it reports
    # their ids in its next signed heartbeat), so they stop being redelivered.
    ack = {str(i or "").strip() for i in (ids or []) if str(i or "").strip()}
    if not ack:
        return list(requests or [])
    return [
        r for r in (requests or [])
        if not (str((r or {}).get("id") or "") in ack
                and str((r or {}).get("status") or "") == "accepted")
    ]


# How many recent owner-attested state pins are kept (and accepted) per repo.
# The window is the availability/rollback trade: a mirror may lag the source by
# up to this many publishes and still serve, while a rollback older than the
# window is rejected.
#
# Every issue/PR/discussion action commits to a served branch and therefore
# republishes the catalog with a fresh state hash, so a repo with active
# collaboration churns pins fast. A 10-deep window let a mirror fall out of the
# accepted set after only a handful of issue edits between its ~5-minute
# re-syncs, at which point the relay refused to route ANY read to it and the
# web UI reported "Issues are unavailable until a live desktop host serves the
# .forkmesh/issues/ folder" even though healthy mirrors held the folder. A
# deeper window keeps those mirrors serving issues (and every other read) across
# far more source publishes. Only states the owner genuinely attested are ever
# admitted, so widening the window does not weaken the anti-tamper guarantee —
# it only accepts older-but-real rollbacks over a longer span.
STATE_PIN_HISTORY = 100


# How many DISTINCT group nodes must independently advertise the same repo
# state before it is served without a reachable source of truth. Two is the bar
# the direct-HTTPS router already sets for advancing a route past its catalog
# host (_https_mirror_flagship_quorum in entry.py): one compromised or merely
# divergent node can never move the fleet by itself, while any two agreeing
# peers can carry the repo while its source is away.
PEER_QUORUM_MIN_NODES = 2


def peer_quorum_pins(target, rows, minimum=PEER_QUORUM_MIN_NODES):
    """Repo states that `minimum` distinct group nodes independently advertise.

    Agreement is the evidence, not possession: a state one node claims alone
    proves nothing, because a tampered node can always publish a hash matching
    its own forged refs. A state two separately registered nodes both publish
    cannot be forged without compromising both of them.

    Counted per node name, so one machine holding several catalog rows (a repo
    record plus its secondary-device rows) is still one voice.
    """
    nodes_by_state = {}
    for row in rows or []:
        rec = (row or {}).get("data") or {}
        if not repo_mirror_same_group(target, rec):
            continue
        state = str(rec.get("stateHash") or "").strip().lower()
        if len(state) != 64 or any(c not in "0123456789abcdef" for c in state):
            continue
        node = str(
            rec.get("machineName") or rec.get("owner") or ""
        ).strip().lower()
        if not node:
            continue
        nodes_by_state.setdefault(state, set()).add(node)
    return {
        state for state, nodes in nodes_by_state.items()
        if len(nodes) >= minimum
    }


def _group_state_within(pins, target, rows):
    """Is any group member still advertising a state inside `pins`?

    The liveness question this file can answer without a database: while even
    one mirror sits inside the source's attested window, the source is still
    governing the group and stays its sole authority. When none does, the
    window has run out — the source stopped publishing and the fleet moved past
    it — and peer agreement takes over.

    Only mirrors count. A source-of-truth record keeps advertising the state it
    last published long after the machine behind it is gone, so letting it
    answer this would mean the window never closes and the group could never
    recover from losing its source — the exact failure this guards against.
    """
    for row in rows or []:
        rec = (row or {}).get("data") or {}
        if str(rec.get("source") or "local-node") == "local-node":
            continue
        if not repo_mirror_same_group(target, rec):
            continue
        if str(rec.get("stateHash") or "").strip().lower() in pins:
            return True
    return False


def clone_state_pins(target, target_key, rows, history=None):
    # The set of repo-state hashes (sha256 over the canonical heads+tags
    # advertisement) the relay accepts from the node serving a clone of the
    # `target` catalog record, or None when the repo is unpinned (fail-open, e.g.
    # a never-attested legacy repo). `rows` are decrypted catalog records
    # [{"key_bi", "data"}]; `history` maps key_bi -> recent attested hashes from
    # repo_state_history.
    #
    # A working-copy holder ("local-node" — the source of truth for its
    # namespace) is validated against ITS OWN attestations: current pin plus
    # recent history. Self-attestation is fine there — the node holds the signing
    # key either way, so the pin is a consistency check, and the history absorbs
    # the push-to-republish lag.
    #
    # A MIRROR ("remote-clone") must serve a state some working-copy holder in
    # its logical-repo group (root commit, name fallback — see
    # repo_mirror_same_group) actually attested. Its own self-signed pin proves
    # nothing: a tampered mirror can always republish a hash matching its forged
    # refs. Mirrors sync with `fetch --prune refs/heads/* refs/tags/*`, so a
    # faithful mirror's full ref set — and therefore its hash — equals the
    # source's. Only when NO group source has ever attested (legacy clients)
    # does the mirror's own pin still apply, preserving the old behaviour.
    #
    # When the source's window has RUN OUT — no mirror in the group is still
    # advertising a state inside it — the group's own agreement admits the
    # state it converged on instead. A mesh whose whole purpose is to outlive
    # its source cannot treat one departed node's frozen pin as the last word:
    # mirrors keep accepting collaboration writes while the source is away,
    # every one of those moves them past the frozen state, and the fleet then
    # refuses itself, permanently, with no path back. See PEER_QUORUM_MIN_NODES
    # for why agreement — not mere possession — is what admits a state. While
    # any mirror is still inside the window the source stays the sole
    # authority, so a live source is never overridden and this costs no extra
    # lookup on the healthy path.
    #
    # Residual limits, by design: a registered account that publishes a
    # local-node record grouped with the repo can inject acceptable pins (raising
    # the bar from "compromise one mirror" to "register a visible fake source"),
    # and a mirror-run agent's local branches make its ref set diverge from the
    # source until the next pruning sync (~5 min) — clones of that mirror fail
    # the check for that window.
    if not isinstance(target, dict) or not target:
        return None

    def _clean(value):
        return str(value or "").strip().lower()

    hist = history or {}

    def _pins_for(key, rec):
        pins = set()
        if cur := _clean(rec.get("stateHash")):
            pins.add(cur)
        for h in hist.get(str(key or ""), []) or []:
            if h := _clean(h):
                pins.add(h)
        return pins

    # Records published before the field existed default to "local-node" (the
    # same default safe_catalog_record applies on write).
    if _clean(target.get("source") or "local-node") == "local-node":
        return _pins_for(target_key, target) or None

    source_pins = set()
    for row in rows or []:
        rec = (row or {}).get("data") or {}
        if _clean(rec.get("source") or "local-node") != "local-node":
            continue
        if not repo_mirror_same_group(target, rec):
            continue
        source_pins |= _pins_for(row.get("key_bi"), rec)
    if source_pins:
        if not _group_state_within(source_pins, target, rows):
            source_pins |= peer_quorum_pins(target, rows)
        return source_pins
    return _pins_for(target_key, target) or None


def admission_override_pins(members):
    """Every valid state hash currently advertised by a group member.

    Used ONLY under an authenticated, short-lived source-of-truth admission
    override (see _admission_override_active in entry.py). A mirror can fall out
    of the signed pin window and then be unable to re-sync — its own catch-up
    fetch is the very same gated clone route — so the whole repo goes dark
    ("mirror unavailable") with no path to recover on its own. While the repo's
    operator holds an override open, the accepted-pin set is widened to the
    states mirrors are actually serving, so they serve (and therefore converge)
    again until the override's TTL lapses.

    Deliberately bounded: only states that already appear in a published
    catalog record (a registered node) are returned, never arbitrary input, and
    callers apply it strictly on TOP of a real source pin set — never when a
    repo has no source-of-truth attestation at all.
    """
    pins = set()
    for row in members or []:
        rec = (row or {}).get("data") or {}
        state = str(rec.get("stateHash") or "").strip().lower()
        if len(state) == 64 and all(c in "0123456789abcdef" for c in state):
            pins.add(state)
    return pins
