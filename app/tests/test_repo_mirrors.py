#!/usr/bin/env python3
"""Repo mirror-health payload contract tests."""

import ast
import asyncio
from pathlib import Path


ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
MIRRORS = ENTRY.parent / "mirrors.py"
APP_RUNTIME = ENTRY.parent / "app_runtime.py"
# Mirror grouping / clone-selection helpers were extracted from entry.py into
# mirrors.py; parse both sources so the AST loaders below still find them.
_WORKER_SRC = ENTRY.read_text(encoding="utf-8") + "\n" + MIRRORS.read_text(encoding="utf-8")


def _literal_assignment(path, name):
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    for node in tree.body:
        if not isinstance(node, ast.Assign):
            continue
        if any(
            isinstance(target, ast.Name) and target.id == name
            for target in node.targets
        ):
            return ast.literal_eval(node.value)
    raise AssertionError("missing %s" % name)


HISTORY_KEY_LIMIT = _literal_assignment(
    MIRRORS, "REPO_STATE_HISTORY_KEY_LIMIT")
HISTORY_ROW_LIMIT = _literal_assignment(ENTRY, "STATE_PIN_HISTORY")


def _load(*names, extra_globals=None):
    tree = ast.parse(_WORKER_SRC, filename=str(ENTRY))
    selected = []
    found = set()
    for node in tree.body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            if node.name in names:
                selected.append(node)
                found.add(node.name)
        elif isinstance(node, ast.Assign):
            # Policy constants a selected helper closes over (a default
            # argument is evaluated at def time, so it must come along).
            assigned = {
                target.id for target in node.targets
                if isinstance(target, ast.Name)
            } & set(names)
            if assigned:
                selected.append(node)
                found |= assigned
    missing = set(names) - found
    assert not missing, "missing definitions: %s" % sorted(missing)
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    namespace = {
        "REPO_STATE_HISTORY_KEY_LIMIT": HISTORY_KEY_LIMIT,
        "STATE_PIN_HISTORY": HISTORY_ROW_LIMIT,
    }
    namespace.update(extra_globals or {})
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return [namespace[name] for name in names]


(
    _mirror_ms,
    agent_provider_mirror_candidates,
    agent_provider_target_decision,
    build_repo_mirrors_payload,
    clone_state_pins,
    repo_mirror_group_key,
    repo_mirror_group_keys,
    repo_mirror_group_selection,
    repo_mirror_pin_history_keys,
    repo_mirror_same_group,
    peer_quorum_pins,
    _group_state_within,
    PEER_QUORUM_MIN_NODES,
) = _load(
    "_mirror_ms",
    "agent_provider_mirror_candidates",
    "agent_provider_target_decision",
    "build_repo_mirrors_payload",
    "clone_state_pins",
    "repo_mirror_group_key",
    "repo_mirror_group_keys",
    "repo_mirror_group_selection",
    "repo_mirror_pin_history_keys",
    "repo_mirror_same_group",
    "peer_quorum_pins",
    "_group_state_within",
    "PEER_QUORUM_MIN_NODES",
)


def test_agent_jobs_use_fresh_signed_provider_capability_not_https_presence():
    now = 1_000_000
    target = {
        "owner": "jett",
        "name": "forkmesh",
        "visibility": "public",
        "rootCommit": "abc",
    }
    records = [
        target,
        {
            "owner": "jett",
            "machineName": "mirror6",
            "name": "forkmesh",
            "visibility": "public",
            "rootCommit": "abc",
            "lastSync": str(now - 1_000),
            "agentProviders": ["claude-code", "codex"],
        },
        {
            "owner": "jett",
            "machineName": "mirror7",
            "name": "forkmesh",
            "visibility": "public",
            "rootCommit": "abc",
            "lastSync": str(now - 2_000),
            "agentProviders": ["codex"],
        },
        {
            "owner": "jett",
            "machineName": "mirror8",
            "name": "forkmesh",
            "visibility": "public",
            "rootCommit": "abc",
            "lastSync": str(now - 700_000),
            "agentProviders": ["claude-code"],
        },
    ]
    assert agent_provider_mirror_candidates(
        records, target, "jett", "claude-code", now, 600_000
    ) == ["mirror6"]
    assert agent_provider_mirror_candidates(
        records, target, "jett", "codex", now, 600_000
    ) == ["mirror6", "mirror7"]
    assert agent_provider_mirror_candidates(
        records, target, "jett", "unknown", now, 600_000
    ) == []


def test_org_owner_can_queue_to_owned_provider_capable_desktop_while_offline():
    now = 1_000_000
    target = {
        "owner": "desktop1",
        "name": "forkmesh",
        "runtimeMode": "desktop",
        "updatedAt": now - 700_000,
        "agentProviders": ["codex"],
        "rootCommit": "abc",
    }
    decision = agent_provider_target_decision(
        [target],
        target,
        "desktop1",
        "codex",
        now,
        600_000,
        org_owner=True,
        source_owned=True,
    )
    assert decision == {
        "ok": True,
        "targetNode": "desktop1",
        "targetOnline": False,
        "queueState": "waiting_for_desktop",
        "route": "owner_desktop",
    }


def test_offline_owner_target_rejections_are_exact_and_actionable():
    now = 1_000_000
    base = {
        "owner": "desktop1",
        "name": "forkmesh",
        "runtimeMode": "desktop",
        "updatedAt": now - 700_000,
        "agentProviders": ["claude-code"],
        "rootCommit": "abc",
    }
    cases = (
        (None, True, "repository_not_published", "publish_repository"),
        (base, False, "target_not_owned", "link_owned_desktop"),
        (
            {**base, "runtimeMode": "headless"},
            True,
            "target_not_desktop",
            "link_desktop_target",
        ),
        (
            base,
            True,
            "provider_not_advertised",
            "publish_provider_capability",
        ),
    )
    for target, source_owned, code, action in cases:
        decision = agent_provider_target_decision(
            [target] if target else [],
            target,
            "desktop1",
            "codex",
            now,
            600_000,
            org_owner=True,
            source_owned=source_owned,
        )
        assert decision["ok"] is False
        assert decision["error"] == code
        assert decision["requiredAction"] == action
        assert decision["message"]

    preferred = agent_provider_target_decision(
        [
            base,
            {
                "owner": "desktop1",
                "machineName": "mirror6",
                "name": "forkmesh",
                "lastSync": now - 1_000,
                "agentProviders": ["codex"],
                "rootCommit": "abc",
            },
        ],
        {**base, "agentProviders": ["codex"]},
        "desktop1",
        "codex",
        now,
        600_000,
        preferred_node="mirror6",
        org_owner=True,
        source_owned=True,
    )
    assert preferred["error"] == "preferred_target_ineligible"
    assert preferred["requiredAction"] == "select_linked_desktop"


def test_engineering_route_still_requires_a_fresh_capable_mirror():
    now = 1_000_000
    target = {
        "owner": "desktop1",
        "name": "forkmesh",
        "runtimeMode": "desktop",
        "updatedAt": now - 700_000,
        "agentProviders": ["codex"],
        "rootCommit": "abc",
    }
    mirror = {
        "owner": "desktop1",
        "machineName": "mirror6",
        "name": "forkmesh",
        "runtimeMode": "headless",
        "lastSync": now - 1_000,
        "agentProviders": ["codex"],
        "rootCommit": "abc",
    }
    accepted = agent_provider_target_decision(
        [target, mirror],
        target,
        "desktop1",
        "codex",
        now,
        600_000,
    )
    assert accepted["targetNode"] == "mirror6"
    assert accepted["queueState"] == "ready_for_claim"

    rejected = agent_provider_target_decision(
        [target, {**mirror, "lastSync": now - 700_000}],
        target,
        "desktop1",
        "codex",
        now,
        600_000,
    )
    assert rejected["error"] == "no_online_agent_mirror"
    assert rejected["retryable"] is True


def test_fresh_agent_only_headless_node_is_live_but_not_clone_ready():
    now = 1_000_000
    source = _row(
        "source", "jett", "forkmesh", root="abc", synced=str(now - 5_000)
    )
    mirror = _row(
        "mirror6",
        "jett",
        "forkmesh",
        root="abc",
        synced=str(now - 1_000),
        source="remote-clone",
    )
    mirror["data"]["machineName"] = "mirror6"
    mirror["data"]["agentProviders"] = ["claude-code", "codex"]
    payload = build_repo_mirrors_payload(
        "jett",
        "forkmesh",
        [source, mirror],
        {},
        {},
        now,
        30_000,
        30_000,
    )
    item = next(entry for entry in payload["mirrors"] if entry["node"] == "mirror6")
    assert item["status"] == "online"
    assert item["lastSeen"] == now - 1_000
    assert item["cloneAvailable"] is False
    assert item["agentProviders"] == ["claude-code", "codex"]


def test_handler_labels_the_public_repository_as_source_not_a_serving_replica():
    handler = ENTRY_TEXT[
        ENTRY_TEXT.index("async def repo_mirrors_handler"):
        ENTRY_TEXT.index("# --- Release download counts")
    ]
    assert 'source_of_truth = {' in handler
    assert 'payload["sourceOfTruth"] = source_of_truth' in handler
    assert '"owner": public_owner' in handler
    assert '"kind": "working-copy"' in handler
    assert '"isSourceOfTruth": True' in handler
    assert 'selection=(source_row, [source_row], catalog_rows)' in handler
    assert "not in STATUS_RETIRED_MIRRORS" in handler


def test_handler_returns_full_source_node_facts_outside_the_mirror_generation():
    now = _Clock.now()
    source = _row(
        "source", "jett", "forkmesh", root="old-root", synced=now - 1_000,
        commit="a" * 40, branch="main", version="0.8.0",
        cpu_percent=31, mem_used=400, mem_total=1000,
    )
    source["data"]["machineName"] = "forkmesh"
    mirror = _row(
        "mirror", "mirror29", "forkmesh", root="current-root",
        synced=now - 500, source="remote-clone",
    )
    mirror["data"]["cloneUrl"] = "https://forkmesh.com/forkmesh/forkmesh"
    handler, _calls = _load_handler(
        rows=[source, mirror], endpoint_nodes=["mirror29"])

    response = asyncio.run(
        handler(object(), _Request("GET"), "forkmesh", "forkmesh"))

    assert response["status"] == 200
    source_node = response["data"]["sourceOfTruth"]
    assert source_node["node"] == "forkmesh"
    assert source_node["commit"] == "a" * 40
    assert source_node["version"] == "0.8.0"
    assert source_node["cpuPercent"] == 31
    assert source_node["memUsedBytes"] == 400
    assert source_node["isSourceOfTruth"] is True
    assert source_node["referenceForMirrors"] is False
    assert {item["node"] for item in response["data"]["mirrors"]} == {
        "mirror29"
    }


def _row(key, owner, name, *, root="", visibility="public", hosted="", synced="",
         size=0, commit="", branch="", issue_count=None, platform="", version="",
         node_id="", clones_served=None, website_served=None, artifact_count=None,
         state_hash="", source="local-node", owner_user="", cpu_percent=None,
         mem_used=None, mem_total=None, disk_used=None, disk_total=None,
         actions_enabled=None, actions_state=None):
    data = {
        "owner": owner,
        "name": name,
        "visibility": visibility,
        "hostedSince": hosted,
        "lastSync": synced,
        "rootCommit": root,
        "sizeBytes": size,
        "source": source,
    }
    if state_hash:
        data["stateHash"] = state_hash
    # Node facts the publishing node mirrors into its catalog record (adhoc #56);
    # only set when provided so legacy records without them are also exercised.
    for field, value in (("commit", commit), ("branch", branch),
                         ("platform", platform), ("version", version),
                         ("nodeId", node_id)):
        if value:
            data[field] = value
    if issue_count is not None:
        data["issueCount"] = issue_count
    if clones_served is not None:
        data["clonesServed"] = clones_served
    if website_served is not None:
        data["websiteServed"] = website_served
    if artifact_count is not None:
        data["artifactCount"] = artifact_count
    if owner_user:
        data["ownerUser"] = owner_user
    if actions_enabled is not None:
        data["actionsEnabled"] = actions_enabled
    if actions_state is not None:
        data["actionsState"] = actions_state
    for field, value in (
        ("cpuPercent", cpu_percent),
        ("memUsedBytes", mem_used),
        ("memTotalBytes", mem_total),
        ("diskUsedBytes", disk_used),
        ("diskTotalBytes", disk_total),
    ):
        if value is not None:
            data[field] = value
    return {
        "key_bi": key,
        "is_private": 1 if visibility == "private" else 0,
        "data": data,
    }


def test_group_key_prefers_root_commit_and_falls_back_to_name():
    assert repo_mirror_group_key({"rootCommit": "ABC", "name": "forkmesh"}) == "root:abc"
    assert repo_mirror_group_key({"rootCommit": "", "name": "ForkMesh"}) == "name:forkmesh"


def test_same_group_matches_root_then_name():
    rooted = {"rootCommit": "ABC", "name": "forkmesh"}
    # Identical root -> same group; differing non-empty root -> separate (fork).
    assert repo_mirror_same_group(rooted, {"rootCommit": "abc", "name": "forkmesh"})
    assert not repo_mirror_same_group(rooted, {"rootCommit": "def", "name": "forkmesh"})
    # Missing root on either side falls back to a case-insensitive name match.
    assert repo_mirror_same_group(rooted, {"rootCommit": "", "name": "ForkMesh"})
    assert repo_mirror_same_group({"name": "forkmesh"}, {"rootCommit": "abc", "name": "forkmesh"})
    assert not repo_mirror_same_group(rooted, {"rootCommit": "", "name": "other"})


def test_same_group_accepts_clone_url_when_legacy_root_is_wrong():
    source = {"owner": "forkmesh", "name": "forkmesh", "rootCommit": "abc"}
    mirror = {
        "owner": "mirror2",
        "name": "forkmesh",
        "rootCommit": "def",
        "cloneUrl": "https://forkmesh.com/forkmesh/forkmesh",
    }
    assert repo_mirror_same_group(source, mirror)
    assert repo_mirror_same_group(mirror, source)
    fork = {
        "owner": "other",
        "name": "forkmesh",
        "rootCommit": "def",
        "cloneUrl": "https://github.com/other/forkmesh.git",
    }
    assert not repo_mirror_same_group(source, fork)


def test_payload_groups_public_root_commit_mirrors_and_sorts_online_first():
    now = 1_000_000
    rows = [
        _row("a", "mainnode", "forkmesh", root="abc", hosted="100000", synced="990000", size=10),
        _row("b", "kaif-node", "forkmesh", root="abc", hosted="", synced="940000", size=20),
        _row("c", "other", "forkmesh", root="def", hosted="90000", synced="999000", size=30),
    ]
    payload = build_repo_mirrors_payload(
        "mainnode",
        "forkmesh",
        rows,
        {"a": now - 1_000, "b": now - 30_000},
        {"b": 80_000},
        now,
        600_000,
        5_000,
    )

    assert payload["ok"] is True
    assert payload["groupKey"] == "root:abc"
    assert payload["summary"] == {
        "mirrors": 2,
        "online": 2,
        "cloneAvailable": 2,
        "dataHostedBytes": 30,
        "longestHostedSince": 80_000,
        "freshestSync": 990_000,
    }
    assert [m["node"] for m in payload["mirrors"]] == ["mainnode", "kaif-node"]
    assert payload["mirrors"][0]["status"] == "online"
    assert payload["mirrors"][0]["lastSeen"] == now - 1_000
    assert payload["mirrors"][0]["syncAgeMs"] == 10_000
    assert payload["mirrors"][0]["behind"] is False
    assert payload["mirrors"][1]["hostedSince"] == 80_000
    assert payload["mirrors"][1]["behind"] is True
    assert payload["mirrors"][1]["cloneAvailable"] is True


def test_payload_exposes_replaceable_direct_mirror_url():
    now = 1_000_000
    row = _row(
        "a", "mainnode", "forkmesh", root="abc", synced="990000")
    row["data"]["mirrorUrl"] = (
        "https://git.mainnode.example/mainnode/forkmesh.git")
    payload = build_repo_mirrors_payload(
        "mainnode", "forkmesh", [row], {"a": now - 1_000}, {},
        now, 600_000, 5_000)
    assert payload["mirrors"][0]["mirrorUrl"] == row["data"]["mirrorUrl"]
    assert repo_mirror_group_key(row["data"]) == repo_mirror_group_key({
        **row["data"],
        "mirrorUrl": "https://replacement.example/mainnode/forkmesh.git",
    })


def test_payload_keeps_machine_node_and_user_owner_as_distinct_identities():
    now = 1_000_000
    row = _row(
        "a", "jett", "forkmesh", root="abc", synced="990000",
        owner_user="jett",
    )
    row["data"]["machineName"] = "forkmesh"
    payload = build_repo_mirrors_payload(
        "jett",
        "forkmesh",
        [row],
        {"a": now - 1_000},
        {},
        now,
        600_000,
        5_000,
    )

    mirror = payload["mirrors"][0]
    assert mirror["node"] == "forkmesh"
    assert mirror["machineName"] == "forkmesh"
    assert mirror["owner"] == "jett"
    assert mirror["ownerUser"] == "jett"


def test_presence_does_not_claim_online_without_a_reachable_mirror_endpoint():
    now = 1_000_000
    rows = [
        _row("a", "mirror2", "forkmesh", root="abc", synced="990000"),
        _row("b", "mirror3", "forkmesh", root="abc", synced="990000"),
    ]
    payload = build_repo_mirrors_payload(
        "mirror2",
        "forkmesh",
        rows,
        {"a": now - 1_000, "b": now - 1_000},
        {},
        now,
        600_000,
        5_000,
        reachable_nodes={"mirror2"},
    )

    mirrors = {mirror["node"]: mirror for mirror in payload["mirrors"]}
    assert mirrors["mirror2"]["status"] == "online"
    assert mirrors["mirror2"]["cloneAvailable"] is True
    assert mirrors["mirror3"]["status"] == "offline"
    assert mirrors["mirror3"]["cloneAvailable"] is False
    assert payload["summary"]["online"] == 1


def test_fresh_signed_local_publication_is_source_node_liveness():
    now = 1_000_000
    row = _row(
        "a", "jett", "forkmesh", root="abc", synced="500000",
        source="local-node",
    )
    row["data"]["machineName"] = "forkmesh"
    payload = build_repo_mirrors_payload(
        "jett", "forkmesh", [row], {}, {}, now, 600_000, 5_000)

    source = payload["mirrors"][0]
    assert source["node"] == "forkmesh"
    assert source["status"] == "online"
    assert source["lastSeen"] == 500_000


def test_identical_signed_ref_states_are_not_behind_only_due_to_sync_time():
    now = 1_000_000
    exact_state = "a" * 64
    rows = [
        _row(
            "a", "mirror2", "forkmesh", synced="990000",
            state_hash=exact_state, source="remote-clone",
        ),
        _row(
            "b", "mirror3", "forkmesh", synced="940000",
            state_hash=exact_state, source="remote-clone",
        ),
    ]
    payload = build_repo_mirrors_payload(
        "mirror2",
        "forkmesh",
        rows,
        {"a": now - 1_000, "b": now - 2_000},
        {},
        now,
        600_000,
        5_000,
    )

    by_node = {mirror["node"]: mirror for mirror in payload["mirrors"]}
    assert by_node["mirror2"]["behind"] is False
    assert by_node["mirror3"]["behind"] is False

    rows[1]["data"]["stateHash"] = "b" * 64
    payload = build_repo_mirrors_payload(
        "mirror2",
        "forkmesh",
        rows,
        {"a": now - 1_000, "b": now - 2_000},
        {},
        now,
        600_000,
        5_000,
    )
    by_node = {mirror["node"]: mirror for mirror in payload["mirrors"]}
    assert by_node["mirror3"]["behind"] is True


def test_payload_keeps_clone_url_mirror_with_mismatched_legacy_root():
    now = 1_000_000
    rows = [
        _row("a", "forkmesh", "forkmesh", root="abc", synced="990000"),
        _row("b", "mirror2", "forkmesh", root="def", synced="995000",
             source="remote-clone"),
    ]
    rows[1]["data"]["cloneUrl"] = "https://forkmesh.com/forkmesh/forkmesh"
    payload = build_repo_mirrors_payload(
        "forkmesh",
        "forkmesh",
        rows,
        {"a": now - 1_000, "b": now - 2_000},
        {},
        now,
        600_000,
        5_000,
    )

    assert payload["summary"]["mirrors"] == 2
    assert [m["node"] for m in payload["mirrors"]] == ["mirror2", "forkmesh"]


def test_payload_uses_clone_url_canonical_target_when_source_row_is_absent():
    # The source-of-truth node can intentionally be offline and absent from the
    # catalog. A mirror-only group still belongs under the cloneUrl namespace, so
    # /api/repo/mainnode/forkmesh/mirrors must resolve even when only
    # remote-clone rows owned by mirror nodes exist.
    now = 1_000_000
    rows = [
        _row("b", "mirror3", "forkmesh", root="abc", synced="995000",
             source="remote-clone"),
        _row("c", "mirror4", "forkmesh", root="abc", synced="990000",
             source="remote-clone"),
        _row("d", "other", "forkmesh", root="abc", synced="999000",
             source="remote-clone"),
    ]
    rows[0]["data"]["cloneUrl"] = "https://forkmesh.com/mainnode/forkmesh"
    rows[1]["data"]["cloneUrl"] = "https://forkmesh.com/mainnode/forkmesh.git"
    rows[2]["data"]["cloneUrl"] = "https://forkmesh.com/other/forkmesh"

    payload = build_repo_mirrors_payload(
        "mainnode",
        "forkmesh",
        rows,
        {"b": now - 1_000, "c": now - 2_000, "d": now - 500},
        {},
        now,
        600_000,
        5_000,
    )

    assert payload["ok"] is True
    assert payload["owner"] == "mainnode"
    assert payload["repo"] == "forkmesh"
    assert payload["summary"]["mirrors"] == 2
    assert [m["node"] for m in payload["mirrors"]] == ["mirror3", "mirror4"]


def test_payload_carries_node_facts_for_offline_mirrors():
    # A node mirrors its latest commit / issue count / platform / version / id into
    # its catalog record, so the Mirror nodes view can show those columns even while
    # the node is offline (adhoc #56). A legacy record without them degrades to the
    # unknown sentinels rather than erroring.
    now = 1_000_000
    rows = [
        _row("a", "mainnode", "forkmesh", root="abc", synced="990000", size=10,
             commit="686d7ebd1ef0", branch="main", issue_count=302,
             platform="linux", version="0.5.22", node_id="7ZMh_2s_IOTPxYz",
             clones_served=42, website_served=118, artifact_count=3,
             owner_user="alice", cpu_percent=37, mem_used=300,
             mem_total=1000, disk_used=800, disk_total=2000,
             actions_enabled=True, actions_state="running"),
        _row("b", "legacy", "forkmesh", root="abc", synced="980000", size=20),
    ]
    payload = build_repo_mirrors_payload(
        "mainnode", "forkmesh", rows, {}, {}, now, 600_000, 5_000
    )
    rich = payload["mirrors"][0]
    assert rich["commit"] == "686d7ebd1ef0"
    assert rich["branch"] == "main"
    assert rich["issueCount"] == 302
    assert rich["platform"] == "linux"
    assert rich["version"] == "0.5.22"
    assert rich["id"] == "7ZMh_2s_IOTPxYz"
    assert rich["ownerUser"] == "alice"
    # Clone / website-serve tallies the node has provided (issue: mirror-node counts).
    assert rich["clonesServed"] == 42
    assert rich["websiteServed"] == 118
    # Release artifacts the node is hosting for download (adhoc #77).
    assert rich["artifactCount"] == 3
    assert rich["cpuPercent"] == 37
    assert (rich["memUsedBytes"], rich["memTotalBytes"]) == (300, 1000)
    assert (rich["diskUsedBytes"], rich["diskTotalBytes"]) == (800, 2000)
    assert rich["actionsEnabled"] is True
    assert rich["actionsState"] == "running"
    # Legacy record (no node facts): empty strings and the -1 "unknown" sentinels.
    legacy = payload["mirrors"][1]
    assert legacy["commit"] == ""
    assert legacy["platform"] == ""
    assert legacy["issueCount"] == -1
    assert legacy["id"] == ""
    assert legacy["ownerUser"] == ""
    assert legacy["clonesServed"] == -1
    assert legacy["websiteServed"] == -1
    assert legacy["artifactCount"] == -1
    assert legacy["cpuPercent"] is None
    assert legacy["memUsedBytes"] is None
    assert legacy["memTotalBytes"] is None
    assert legacy["diskUsedBytes"] is None
    assert legacy["diskTotalBytes"] is None
    assert legacy["actionsEnabled"] is False
    assert legacy["actionsState"] == "disabled"


def test_payload_names_the_latest_commit_of_each_mirror():
    # A node signs the subject/author/date of its head commit into its catalog
    # record, so the Mirror nodes table and the World cabinets can say what the
    # commit is instead of showing a bare hash (adhoc #337). A node that never
    # published them stays truthfully unknown (None), never a blank string.
    now = 1_000_000
    rows = [
        _row("a", "mainnode", "forkmesh", root="abc", synced="990000",
             commit="686d7ebd1ef0", branch="main"),
        _row("b", "legacy", "forkmesh", root="abc", synced="980000"),
    ]
    rows[0]["data"].update({
        "commitSubject": "Show the last commit on every node",
        "commitAuthorName": "Ada Lovelace",
        "commitAt": "1750000000000",
    })
    payload = build_repo_mirrors_payload(
        "mainnode", "forkmesh", rows, {}, {}, now, 600_000, 5_000
    )
    rich = payload["mirrors"][0]
    assert rich["lastCommitMessage"] == "Show the last commit on every node"
    assert rich["lastCommitAuthorName"] == "Ada Lovelace"
    assert rich["lastCommitAt"] == 1_750_000_000_000
    legacy = payload["mirrors"][1]
    assert legacy["lastCommitMessage"] is None
    assert legacy["lastCommitAuthorName"] is None
    assert legacy["lastCommitAt"] is None


def test_payload_carries_only_safe_changed_files_from_the_signed_head():
    now = 1_000_000
    rows = [
        _row("a", "mainnode", "forkmesh", root="abc", synced="990000"),
    ]
    rows[0]["data"]["changedFiles"] = [
        "src/world.js",
        "src/world.js",
        "../private-key",
        "/etc/passwd",
        "docs/onboarding.md",
        "bad\npath",
    ]
    payload = build_repo_mirrors_payload(
        "mainnode", "forkmesh", rows, {}, {}, now, 600_000, 5_000
    )
    assert payload["mirrors"][0]["changedFiles"] == [
        "src/world.js", "docs/onboarding.md"]


def test_payload_stamps_when_each_mirror_last_served_a_clone_or_web_read():
    # The serve counters on the Mirror node cards each carry "when, and to what
    # kind of client". A node that has served none of a kind, or predates the
    # stamps, stays None instead of borrowing its publication time.
    now = 1_000_000
    rows = [
        _row("a", "mainnode", "forkmesh", root="abc", synced="990000"),
        _row("b", "legacy", "forkmesh", root="abc", synced="980000"),
    ]
    rows[0]["data"].update({
        "cloneServedAt": "1750000000000",
        "cloneServedAgent": "git-client",
        "websiteServedAt": "1750000900000",
        "websiteServedAgent": "browser",
    })
    payload = build_repo_mirrors_payload(
        "mainnode", "forkmesh", rows, {}, {}, now, 600_000, 5_000
    )
    rich = payload["mirrors"][0]
    assert rich["cloneServedAt"] == 1_750_000_000_000
    assert rich["cloneServedAgent"] == "git-client"
    assert rich["websiteServedAt"] == 1_750_000_900_000
    assert rich["websiteServedAgent"] == "browser"
    legacy = payload["mirrors"][1]
    assert legacy["cloneServedAt"] is None
    assert legacy["cloneServedAgent"] is None
    assert legacy["websiteServedAt"] is None
    assert legacy["websiteServedAgent"] is None


def test_payload_defensively_bounds_or_hides_invalid_host_telemetry():
    now = 1_000_000
    rows = [
        _row(
            "a", "mainnode", "forkmesh", root="abc", synced="990000",
            cpu_percent=1000, mem_used=2000, mem_total=1000,
            disk_used=5, disk_total=0,
        ),
        _row(
            "b", "bad", "forkmesh", root="abc", synced="980000",
            cpu_percent=True, mem_used=-1, mem_total=1000,
        ),
    ]
    payload = build_repo_mirrors_payload(
        "mainnode", "forkmesh", rows, {}, {}, now, 600_000, 5_000
    )
    by_node = {mirror["node"]: mirror for mirror in payload["mirrors"]}
    assert by_node["mainnode"]["cpuPercent"] == 100
    assert (
        by_node["mainnode"]["memUsedBytes"],
        by_node["mainnode"]["memTotalBytes"],
    ) == (1000, 1000)
    assert by_node["mainnode"]["diskUsedBytes"] is None
    assert by_node["mainnode"]["diskTotalBytes"] is None
    assert by_node["bad"]["cpuPercent"] is None
    assert by_node["bad"]["memUsedBytes"] is None
    assert by_node["bad"]["memTotalBytes"] is None


def test_payload_fails_malformed_actions_state_closed_to_disabled():
    now = 1_000_000
    rows = [
        _row(
            "a", "enabled", "forkmesh", root="abc", synced="990000",
            actions_enabled=True, actions_state="enabled",
        ),
        _row(
            "b", "running", "forkmesh", root="abc", synced="980000",
            actions_enabled=True, actions_state="running",
        ),
        _row(
            "c", "tampered", "forkmesh", root="abc", synced="970000",
            actions_enabled=True, actions_state="queued",
        ),
        _row(
            "d", "legacy", "forkmesh", root="abc", synced="960000",
        ),
    ]
    payload = build_repo_mirrors_payload(
        "enabled", "forkmesh", rows, {}, {}, now, 600_000, 5_000
    )
    by_node = {mirror["node"]: mirror for mirror in payload["mirrors"]}
    assert (by_node["enabled"]["actionsEnabled"],
            by_node["enabled"]["actionsState"]) == (True, "enabled")
    assert (by_node["running"]["actionsEnabled"],
            by_node["running"]["actionsState"]) == (True, "running")
    assert (by_node["tampered"]["actionsEnabled"],
            by_node["tampered"]["actionsState"]) == (False, "disabled")
    assert (by_node["legacy"]["actionsEnabled"],
            by_node["legacy"]["actionsState"]) == (False, "disabled")


def test_payload_marks_mirrors_the_integrity_gate_rejects():
    # A mirror must advertise a refs fingerprint some working-copy holder in its
    # group attested (current pin or recent history) or the relay rejects every
    # clone it serves ("repository failed integrity check"); the payload carries
    # that verdict per node so the mirror list can show WHICH node is affected.
    now = 1_000_000
    rows = [
        _row("a", "mainnode", "forkmesh", root="abc", synced="990000",
             state_hash="AAA"),
        _row("b", "in-sync", "forkmesh", root="abc", synced="980000",
             state_hash="aaa", source="remote-clone"),
        _row("c", "lagging", "forkmesh", root="abc", synced="970000",
             state_hash="old", source="remote-clone"),
        _row("d", "tampered", "forkmesh", root="abc", synced="960000",
             state_hash="bbb", source="remote-clone"),
        _row("e", "legacy", "forkmesh", root="abc", synced="950000",
             source="remote-clone"),
    ]
    payload = build_repo_mirrors_payload(
        "mainnode", "forkmesh", rows, {}, {}, now, 600_000, 5_000,
        history={"a": ["old"]},
    )
    verdicts = {m["node"]: m["integrity"] for m in payload["mirrors"]}
    assert verdicts == {
        "mainnode": "ok",        # the source matches its own attestation
        "in-sync": "ok",         # matches the source's current pin (case-insensitive)
        "lagging": "ok",         # matches a recent pin from the history window
        "tampered": "rejected",  # matches nothing the source ever attested
        "legacy": "unknown",     # no fingerprint published; gate checks live refs
    }


def test_payload_downgrades_rejected_to_healing_when_source_online():
    # Same mirrors as above, but the source of truth (mainnode) has a fresh
    # healthy direct-HTTPS endpoint right now. The failing mirror can re-sync
    # from that source, so its verdict is "healing", not "rejected".
    now = 1_000_000
    rows = [
        _row("a", "mainnode", "forkmesh", root="abc", synced="990000",
             state_hash="AAA"),
        _row("b", "in-sync", "forkmesh", root="abc", synced="980000",
             state_hash="aaa", source="remote-clone"),
        _row("c", "lagging", "forkmesh", root="abc", synced="970000",
             state_hash="old", source="remote-clone"),
        _row("d", "tampered", "forkmesh", root="abc", synced="960000",
             state_hash="bbb", source="remote-clone"),
        _row("e", "legacy", "forkmesh", root="abc", synced="950000",
             source="remote-clone"),
    ]
    payload = build_repo_mirrors_payload(
        "mainnode", "forkmesh", rows, {"a": now - 1_000}, {}, now, 600_000, 5_000,
        history={"a": ["old"]},
    )
    verdicts = {m["node"]: m["integrity"] for m in payload["mirrors"]}
    assert verdicts == {
        "mainnode": "ok",       # the source matches its own attestation
        "in-sync": "ok",        # matches the source's current pin
        "lagging": "ok",        # matches a recent pin from the history window
        "tampered": "healing",  # would be rejected, but the source is online
        "legacy": "unknown",    # no fingerprint published
    }
    # With the source OFFLINE the tamper gate is back in force: hard "rejected".
    offline = build_repo_mirrors_payload(
        "mainnode", "forkmesh", rows, {}, {}, now, 600_000, 5_000,
        history={"a": ["old"]},
    )
    assert {m["node"]: m["integrity"] for m in offline["mirrors"]}["tampered"] \
        == "rejected"


def test_payload_uses_explicit_org_backing_node_as_integrity_anchor():
    # /forkmesh/forkmesh is an organization alias explicitly linked to
    # mirror2/forkmesh. The direct HTTPS router treats that account-owned,
    # signed backing record as the organization's canonical attestation. The
    # mirror-status payload must use the same pin instead of a stale same-name
    # local-node record, otherwise it contradicts the route it is describing.
    now = 1_000_000
    stale = "a" * 64
    current = "b" * 64
    rows = [
        _row("source", "jett", "forkmesh", root="abc", synced="910000",
             state_hash=stale),
        _row("canonical", "mirror2", "forkmesh", root="", synced="990000",
             state_hash=current, source="remote-clone"),
        _row("peer", "mirror3", "forkmesh", root="", synced="980000",
             state_hash=current, source="remote-clone"),
    ]
    # Unlinked, jett's local-node record is the anchor. Its pin is stale and no
    # mirror is inside it any more, so the two agreeing peers carry the repo
    # between them rather than the whole group reading "rejected" while jett is
    # away — the fleet is what serves this repository.
    ordinary = build_repo_mirrors_payload(
        "mirror2", "forkmesh", rows, {}, {}, now, 600_000, 5_000,
    )
    assert {
        m["node"]: m["integrity"] for m in ordinary["mirrors"]
    } == {
        "mirror2": "ok",
        "mirror3": "ok",
        "jett": "ok",
    }

    # ...and the anchor still is the anchor: one mirror alone is a claim, not
    # agreement, so it cannot admit its own state and stays rejected.
    lone = build_repo_mirrors_payload(
        "mirror2", "forkmesh", rows[:2], {}, {}, now, 600_000, 5_000,
    )
    assert {
        m["node"]: m["integrity"] for m in lone["mirrors"]
    } == {
        "mirror2": "rejected",
        "jett": "ok",
    }

    linked = build_repo_mirrors_payload(
        "mirror2", "forkmesh", rows, {}, {}, now, 600_000, 5_000,
        linked_canonical=True,
    )
    assert {
        m["node"]: m["integrity"] for m in linked["mirrors"]
    } == {
        "mirror2": "ok",
        "mirror3": "ok",
        "jett": "rejected",
    }


def test_linked_backing_node_presence_marks_mismatched_peer_healing():
    now = 1_000_000
    current = "a" * 64
    old = "b" * 64
    rows = [
        _row("canonical", "mirror2", "forkmesh", root="", synced="990000",
             state_hash=current, source="remote-clone"),
        _row("peer", "mirror3", "forkmesh", root="", synced="980000",
             state_hash=old, source="remote-clone"),
    ]
    payload = build_repo_mirrors_payload(
        "mirror2", "forkmesh", rows, {"canonical": now - 1_000}, {},
        now, 600_000, 5_000, linked_canonical=True,
    )
    assert {
        m["node"]: m["integrity"] for m in payload["mirrors"]
    } == {
        "mirror2": "ok",
        "mirror3": "healing",
    }


def test_linked_backing_node_without_attestation_fails_closed():
    now = 1_000_000
    rows = [
        _row("canonical", "mirror2", "forkmesh", root="", synced="990000",
             source="remote-clone"),
        _row("peer", "mirror3", "forkmesh", root="", synced="980000",
             state_hash="c" * 64, source="remote-clone"),
    ]
    payload = build_repo_mirrors_payload(
        "mirror2", "forkmesh", rows, {}, {}, now, 600_000, 5_000,
        linked_canonical=True,
    )
    assert {
        m["node"]: m["integrity"] for m in payload["mirrors"]
    } == {
        "mirror2": "unknown",
        "mirror3": "rejected",
    }


def test_payload_integrity_fails_open_without_source_attestation():
    # When no working-copy holder in the group ever attested a state, the gate
    # falls back to the mirror's own pins (legacy behaviour): nothing to compare
    # against, so nobody is flagged.
    now = 1_000_000
    rows = [
        _row("a", "mainnode", "forkmesh", root="abc", synced="990000"),
        _row("b", "mirror", "forkmesh", root="abc", synced="980000",
             state_hash="bbb", source="remote-clone"),
    ]
    payload = build_repo_mirrors_payload(
        "mainnode", "forkmesh", rows, {}, {}, now, 600_000, 5_000
    )
    assert all(m["integrity"] == "ok" for m in payload["mirrors"])


def test_payload_groups_mirror_with_missing_root_commit_by_name():
    # A mirror cloned from the relay can have an unset HEAD and publish an empty
    # rootCommit (issue #243). It must still group with the source of truth (which
    # does advertise a root) instead of dropping into its own name-keyed bucket and
    # vanishing from the owner's mirror-nodes list.
    now = 1_000_000
    rows = [
        _row("a", "mainnode", "forkmesh", root="abc", synced="990000", size=10),
        _row("b", "kaif-node", "forkmesh", root="", synced="980000", size=20),
        _row("c", "fork-owner", "forkmesh", root="def", synced="970000", size=30),
    ]
    payload = build_repo_mirrors_payload(
        "mainnode", "forkmesh", rows, {}, {}, now, 600_000, 5_000
    )

    # The empty-root mirror joins the source's root group by name; the genuine fork
    # (a different non-empty root) stays out.
    assert payload["groupKey"] == "root:abc"
    assert [m["node"] for m in payload["mirrors"]] == ["mainnode", "kaif-node"]


def test_payload_falls_back_to_repo_name_when_root_commit_is_absent():
    now = 1_000_000
    rows = [
        _row(
            "a", "mainnode", "forkmesh", synced="990000",
            source="remote-clone"),
        _row(
            "b", "backup", "ForkMesh", synced="980000",
            source="remote-clone"),
        _row(
            "c", "backup", "other", synced="999000",
            source="remote-clone"),
    ]
    payload = build_repo_mirrors_payload(
        "mainnode", "forkmesh", rows, {}, {}, now, 600_000, 5_000
    )

    assert payload["groupKey"] == "name:forkmesh"
    assert [m["node"] for m in payload["mirrors"]] == ["mainnode", "backup"]
    assert payload["summary"]["online"] == 0
    assert payload["summary"]["cloneAvailable"] == 0


def test_payload_returns_none_for_missing_or_private_target():
    now = 1_000_000
    private_rows = [_row("a", "mainnode", "forkmesh", visibility="private")]
    public_rows = [_row("a", "mainnode", "forkmesh")]

    assert build_repo_mirrors_payload(
        "mainnode", "missing", public_rows, {}, {}, now, 600_000, 5_000
    ) is None
    assert build_repo_mirrors_payload(
        "mainnode", "forkmesh", private_rows, {}, {}, now, 600_000, 5_000
    ) is None


class _Clock:
    @staticmethod
    def now():
        return 1_000_000


class _Request:
    def __init__(self, method):
        self.method = method


def _response(data, status=200, **_kwargs):
    return {"status": status, "data": data}


class _Calls(list):
    """Recorded SQL, plus the bindings each statement was issued with."""

    def __init__(self):
        super().__init__()
        self.bindings = []


def _load_sync_status():
    import importlib.util

    spec = importlib.util.spec_from_file_location(
        "sync_status", ENTRY.parent / "sync_status.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


_SYNC_STATUS = _load_sync_status()


def _load_availability_proof():
    import importlib.util

    spec = importlib.util.spec_from_file_location(
        "availability_proof", ENTRY.parent / "availability_proof.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


_AVAILABILITY_PROOF = _load_availability_proof()


def _load_handler(
    *, rows, first_hosted=None,
    linked_canonical=False, endpoint_nodes=None, endpoint_records=None,
    serve_counts=None, retired_nodes=(),
):
    calls = _Calls()

    async def ensure_schema(_env):
        calls.append("schema")

    async def d1_all(_env, sql, *args):
        calls.append(sql)
        calls.bindings.append((sql, args))
        if "FROM repo_first_hosted" in sql:
            keys = set(args)
            return [
                row for row in (first_hosted or [])
                if not keys or row.get("repo_bi") in keys
            ]
        if "FROM mirror_https_endpoints" in sql:
            if endpoint_records is not None:
                return endpoint_records
            if endpoint_nodes is not None:
                return [
                    {"node_name": node, "checked_at": _Clock.now()}
                    for node in endpoint_nodes
                ]
            return [
                {
                    "node_name": (
                        item.get("data", {}).get("machineName")
                        or item.get("data", {}).get("owner")
                    ),
                    "checked_at": _Clock.now(),
                }
                for item in rows
                if item.get("data", {}).get("visibility", "public") == "public"
            ]
        if "FROM mirror_serve_counters" in sql:
            return serve_counts or []
        return []

    async def d1_first(_env, sql, *args):
        calls.append(sql)
        if "FROM org_repos" in sql:
            return {"linked": 1} if linked_canonical else None
        raise AssertionError(sql)

    async def decrypt_row(_env, data):
        return data

    async def _decrypted_public_catalog(_env, _now):
        # The real helper is the shared per-isolate memo over
        # "SELECT ... FROM repositories" + decrypt_row; the handler must read the
        # catalog through it instead of running its own scan.
        calls.append("_decrypted_public_catalog FROM repositories")
        return rows

    async def _mirror_owner_user(_env, _node_name, _now):
        return ""

    async def _mirror_owner_users(_env, node_names, _now):
        return {str(node_name): "" for node_name in node_names}

    async def edge_cache_match(_key):
        return None  # always a miss in these unit tests

    async def edge_cache_put(_key, _response):
        calls.append("edge_cache_put")

    async def active_registered_node_bis(_env):
        # Mirror membership is durable even when the live-node set is empty.
        # Fresh endpoint/presence evidence below still controls online status.
        return set()

    namespace = {
        "Date": _Clock,
        "HOST_PRESENCE_STALE_MS": 600_000,
        "HTTPS_MIRROR_STATUS_FRESH_MS": 600_000,
        "MAX_NODE_NAME": 64,
        "STATUS_RETIRED_MIRRORS": frozenset(retired_nodes),
        "valid_node_name": lambda value: bool(value),
        "clean_string": lambda value, maximum: str(value or "")[:maximum],
        "mirrors_payload_status": _SYNC_STATUS.mirrors_payload_status,
        "mirrors_payload_availability": (
            _AVAILABILITY_PROOF.evaluate_mirrors_payload),
        "asyncio": asyncio,
        "ensure_schema": ensure_schema,
        "active_registered_node_bis": active_registered_node_bis,
        "d1_all": d1_all,
        "d1_first": d1_first,
        "decrypt_row": decrypt_row,
        "_decrypted_public_catalog": _decrypted_public_catalog,
        "_mirror_owner_user": _mirror_owner_user,
        "_mirror_owner_users": _mirror_owner_users,
        "_is_blocked_catalog_identity": lambda _env, _owner, _name: False,
        "json_response": _response,
        "edge_cache_match": edge_cache_match,
        "edge_cache_put": edge_cache_put,
        "quote": __import__("urllib.parse", fromlist=["quote"]).quote,
    }
    handler, *_ = _load(
        "repo_mirrors_handler",
        "_bounded_repo_state_history",
        "method_name",
        "_mirror_ms",
        "repo_mirror_group_key",
        "repo_mirror_group_keys",
        "repo_mirror_group_selection",
        "repo_mirror_pin_history_keys",
        "repo_mirror_same_group",
        "build_repo_mirrors_payload",
        "clone_state_pins",
        "peer_quorum_pins",
        "_group_state_within",
        "PEER_QUORUM_MIN_NODES",
        extra_globals=namespace,
    )
    return handler, calls


def test_repo_mirrors_handler_rejects_non_get_before_reading_tables():
    handler, calls = _load_handler(rows=[])

    response = asyncio.run(handler(object(), _Request("POST"), "mainnode", "forkmesh"))

    assert response == {"status": 405, "data": {"error": "method_not_allowed"}}
    assert calls == []


def test_repo_mirrors_handler_get_returns_public_mirrors_payload():
    handler, calls = _load_handler(
        rows=[
            {"key_bi": "a", "data": _row("a", "mainnode", "forkmesh", root="abc")["data"]},
            {"key_bi": "b", "data": _row("b", "kaif-node", "forkmesh", root="abc")["data"]},
        ],
        first_hosted=[{"repo_bi": "b", "ts": 500_000}],
    )

    response = asyncio.run(handler(object(), _Request("GET"), "mainnode", "forkmesh"))

    assert response["status"] == 200
    assert response["data"]["ok"] is True
    assert response["data"]["summary"]["mirrors"] == 2
    assert {
        mirror["node"] for mirror in response["data"]["mirrors"]
    } == {"mainnode", "kaif-node"}
    assert any("FROM repositories" in call for call in calls)
    assert not any("FROM host_presence" in call for call in calls)
    assert sum(
        "FROM mirror_https_endpoints" in call for call in calls
    ) == 1
    assert any("FROM repo_first_hosted" in call for call in calls)
    # The handler also gathers the attested-pin history so each mirror carries
    # its clone-integrity verdict.
    assert any("FROM repo_state_history" in call for call in calls)
    assert all("integrity" in mirror for mirror in response["data"]["mirrors"])
    # The availability proof is computed after the endpoint/probe overlay and
    # reproduces from the very payload it ships in.
    availability = response["data"]["availability"]
    assert availability["schema"] == "forkmesh-availability-proof-v1"
    assert availability == _AVAILABILITY_PROOF.evaluate_mirrors_payload(
        {k: v for k, v in response["data"].items() if k != "availability"})
    assert availability["counts"]["total"] == 2


def test_repo_mirrors_handler_never_resurrects_retired_catalog_nodes():
    handler, _ = _load_handler(
        rows=[
            {"key_bi": "source", "data": _row(
                "source", "mainnode", "forkmesh", root="abc")["data"]},
            {"key_bi": "retired", "data": _row(
                "retired", "mirror20", "forkmesh", root="abc",
                source="remote-clone")["data"]},
        ],
        retired_nodes={"mirror20"},
    )

    response = asyncio.run(
        handler(object(), _Request("GET"), "mainnode", "forkmesh")
    )

    assert response["data"]["summary"]["mirrors"] == 1
    assert [row["node"] for row in response["data"]["mirrors"]] == ["mainnode"]


def test_repo_mirrors_handler_reads_catalog_through_the_shared_memo():
    # adhoc #1630: this poll-heavy route ran its own "SELECT ... FROM
    # repositories" plus a sequential decrypt_row() over the whole public
    # catalog on every edge-cache miss. That unbounded per-request pass is what
    # wedges an isolate ("Cannot enter into task" / CpuLimitExceeded), so the
    # catalog must come from the shared 5s memo instead.
    handler, calls = _load_handler(
        rows=[
            {"key_bi": "a", "data": _row(
                "a", "mainnode", "forkmesh", root="abc")["data"]},
        ],
    )

    asyncio.run(handler(object(), _Request("GET"), "mainnode", "forkmesh"))

    assert "_decrypted_public_catalog FROM repositories" in calls
    assert not any(
        "FROM repositories" in sql for sql, _args in calls.bindings
    )
    body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def repo_mirrors_handler")
        : ENTRY_TEXT.index("\n\n\n", ENTRY_TEXT.index(
            "async def repo_mirrors_handler"))
    ]
    assert "await _decrypted_public_catalog(env, now)" in body
    assert not [
        line for line in body.splitlines()
        if "FROM repositories" in line and not line.lstrip().startswith("#")
    ]


def test_repo_mirrors_handler_scopes_per_repo_reads_to_the_mirror_group():
    # The pin-history and first-hosted reads were keyed by EVERY public repo,
    # so each poll pulled unrelated repositories' rows — and one sequential D1
    # round-trip per 80 catalog rows — to build a payload about one group.
    handler, calls = _load_handler(
        rows=[
            {"key_bi": "a", "data": _row(
                "a", "mainnode", "forkmesh", root="abc")["data"]},
            {"key_bi": "b", "data": _row(
                "b", "kaif-node", "forkmesh", root="abc",
                source="remote-clone")["data"]},
            {"key_bi": "c", "data": _row(
                "c", "othernode", "unrelated", root="zzz")["data"]},
            {"key_bi": "d", "data": _row(
                "d", "thirdnode", "alsounrelated", root="yyy")["data"]},
        ],
        first_hosted=[
            {"repo_bi": "b", "ts": 500_000},
            {"repo_bi": "c", "ts": 400_000},
        ],
    )

    response = asyncio.run(
        handler(object(), _Request("GET"), "mainnode", "forkmesh"))

    assert response["status"] == 200
    scoped = [
        (sql, args) for sql, args in calls.bindings
        if "FROM repo_state_history" in sql
        or "FROM repo_first_hosted" in sql
    ]
    assert scoped, "group reads must still happen"
    for sql, args in scoped:
        if "FROM repo_first_hosted" in sql:
            assert " IN (" in sql
            assert set(args) == {"a", "b"}
        else:
            assert sql.count("FROM repo_state_history") == 2
            assert args[::2] == ("a", "b")
            assert args[1::2] == (HISTORY_ROW_LIMIT,) * 2


def test_repo_mirrors_handler_resolves_the_group_once_per_request():
    # Group resolution is a pass over EVERY public catalog record. The handler
    # needs it up front to scope its D1 reads, so it must hand that selection to
    # build_repo_mirrors_payload instead of letting it repeat the same pass —
    # this route's failure mode is the CPU limit (adhoc #1630).
    handler, _calls = _load_handler(
        rows=[
            {"key_bi": "a", "data": _row(
                "a", "mainnode", "forkmesh", root="abc")["data"]},
            {"key_bi": "b", "data": _row(
                "b", "kaif-node", "forkmesh", root="abc",
                source="remote-clone")["data"]},
        ],
    )
    namespace = handler.__globals__
    resolved = []
    original = namespace["repo_mirror_group_selection"]

    def counting(*args, **kwargs):
        resolved.append(args[:2])
        return original(*args, **kwargs)

    namespace["repo_mirror_group_selection"] = counting

    response = asyncio.run(
        handler(object(), _Request("GET"), "mainnode", "forkmesh"))

    assert response["status"] == 200
    assert len(response["data"]["mirrors"]) == 2
    assert len(resolved) == 1


def test_pin_history_keys_keep_a_rootless_mirrors_named_source():
    # clone_state_pins validates a mirror against the local-node rows grouped
    # with IT, and a record published without a root commit groups by name. The
    # named source is therefore an attestation this payload depends on even
    # though it is not in the requested repo's own group — narrowing the pin
    # history to group members alone would silently drop its history and could
    # flip a lagging mirror to "rejected".
    members = [
        {"key_bi": "a", "data": _row(
            "a", "mainnode", "forkmesh", root="abc")["data"]},
        {"key_bi": "b", "data": _row(
            "b", "kaif-node", "forkmesh", source="remote-clone")["data"]},
    ]
    public_rows = members + [
        {"key_bi": "c", "data": _row(
            "c", "othernode", "forkmesh", root="zzz")["data"]},
        {"key_bi": "d", "data": _row(
            "d", "thirdnode", "unrelated", root="yyy")["data"]},
        {"key_bi": "e", "data": _row(
            "e", "fourthnode", "forkmesh", root="zzz",
            source="remote-clone")["data"]},
    ]

    keys = repo_mirror_pin_history_keys(members, public_rows, "a")

    # "c" groups with the rootless mirror "b" by name; "d" is a different repo
    # and "e" is not a working-copy holder, so neither can supply a pin. Source
    # histories precede the mirror's legacy fallback history.
    assert keys == ["a", "c", "b"]
    assert repo_mirror_group_keys(members) == ["a", "b"]


def test_pin_history_keys_are_target_first_authority_priority_and_hard_capped():
    members = [
        {"key_bi": "target", "data": _row(
            "target", "mirror", "forkmesh", root="",
            source="remote-clone")["data"]},
        {"key_bi": "fallback", "data": _row(
            "fallback", "backup", "forkmesh", root="",
            source="remote-clone")["data"]},
    ]
    authorities = [
        {"key_bi": "source-%02d" % index, "data": _row(
            "source-%02d" % index,
            "source-%02d" % index,
            "forkmesh",
            root="root-%02d" % index,
        )["data"]}
        for index in reversed(range(8))
    ]

    keys = repo_mirror_pin_history_keys(
        members, members + authorities, "target")

    assert HISTORY_KEY_LIMIT == 4
    assert keys == ["target", "source-00", "source-01", "source-02"]
    assert "fallback" not in keys


def test_repo_mirrors_handler_404s_unknown_repo_before_further_reads():
    handler, calls = _load_handler(
        rows=[
            {"key_bi": "c", "data": _row(
                "c", "othernode", "unrelated", root="zzz")["data"]},
        ],
    )

    response = asyncio.run(
        handler(object(), _Request("GET"), "mainnode", "forkmesh"))

    assert response == {"status": 404, "data": {"error": "not_found"}}
    assert not any("FROM repo_state_history" in call for call in calls)
    assert not any("FROM repo_first_hosted" in call for call in calls)
    assert not any("FROM org_repos" in call for call in calls)


def test_mirror_owner_user_memoizes_the_account_lookup_per_isolate():
    # Resolving the human owner behind each headless mirror is a blind_index
    # plus up to two D1 reads and a decrypt, run once per mirror in the group on
    # every uncached poll. A growing fleet turned that into dozens of sequential
    # round-trips, so the resolved link is memoized per isolate.
    lookups = []

    async def _account_row(_env, name):
        lookups.append(name)
        return "bi", {"kind": "node", "owner": "jett"}

    namespace = {
        "_account_row": _account_row,
        "_account_kind": lambda rec: rec.get("kind"),
        "_MIRROR_OWNER_USER_MEMO": {},
        "MIRROR_OWNER_USER_MEMO_TTL_MS": 300_000,
        "MIRROR_OWNER_USER_MEMO_MAX": 512,
    }
    resolve, *_ = _load("_mirror_owner_user", extra_globals=namespace)

    assert asyncio.run(resolve(object(), "mirror9", 1_000_000)) == "jett"
    assert asyncio.run(resolve(object(), "mirror9", 1_000_100)) == "jett"
    assert lookups == ["mirror9"]
    # The memo expires, so a re-linked node is picked up without a redeploy.
    assert asyncio.run(resolve(object(), "mirror9", 1_400_000)) == "jett"
    assert lookups == ["mirror9", "mirror9"]


def test_mirror_owner_user_memoizes_unlinked_nodes_and_bounds_the_memo():
    lookups = []

    async def _account_row(_env, name):
        lookups.append(name)
        return None, None

    memo = {}
    namespace = {
        "_account_row": _account_row,
        "_account_kind": lambda rec: (rec or {}).get("kind"),
        "_MIRROR_OWNER_USER_MEMO": memo,
        "MIRROR_OWNER_USER_MEMO_TTL_MS": 300_000,
        "MIRROR_OWNER_USER_MEMO_MAX": 2,
    }
    resolve, *_ = _load("_mirror_owner_user", extra_globals=namespace)

    assert asyncio.run(resolve(object(), "mirror9", 1_000_000)) == ""
    assert asyncio.run(resolve(object(), "mirror9", 1_000_100)) == ""
    assert lookups == ["mirror9"]  # an unlinked node is not re-resolved either
    asyncio.run(resolve(object(), "mirror14", 1_000_200))
    asyncio.run(resolve(object(), "mirror15", 1_000_300))
    assert len(memo) <= 2


def test_repo_mirrors_handler_aggregates_traffic_across_group_aliases():
    handler, calls = _load_handler(
        rows=[
            {"key_bi": "a", "data": _row(
                "a", "mirror2", "forkmesh", root="abc")["data"]},
            {"key_bi": "b", "data": _row(
                "b", "mirror10", "forkmesh", root="abc",
                source="remote-clone")["data"]},
        ],
        serve_counts=[
            {"node_name": "mirror2", "clones": 8, "website": 21},
            {"node_name": "mirror10", "clones": 5, "website": 34},
        ],
    )

    response = asyncio.run(
        handler(object(), _Request("GET"), "mirror2", "forkmesh")
    )

    mirrors = {
        mirror["node"]: mirror for mirror in response["data"]["mirrors"]
    }
    assert mirrors["mirror2"]["clonesServed"] == 8
    assert mirrors["mirror2"]["websiteServed"] == 21
    assert mirrors["mirror10"]["clonesServed"] == 5
    assert mirrors["mirror10"]["websiteServed"] == 34
    query = next(
        sql for sql in calls if "FROM mirror_serve_counters" in sql
    )
    assert "SUM(clones) AS clones" in query
    assert "GROUP BY node_name" in query


def test_repo_mirrors_handler_keeps_inactive_rows_visible_offline():
    handler, _calls = _load_handler(
        rows=[
            {
                "key_bi": "a",
                "owner_bi": "owner:mainnode",
                "data": _row(
                    "a", "mainnode", "forkmesh", root="abc"
                )["data"],
            },
            {
                "key_bi": "b",
                "owner_bi": "owner:backup",
                "data": _row(
                    "b", "backup", "forkmesh", root="abc",
                    source="remote-clone",
                )["data"],
            },
        ],
        endpoint_nodes=[],
    )

    response = asyncio.run(
        handler(object(), _Request("GET"), "mainnode", "forkmesh")
    )

    assert response["status"] == 200
    assert response["data"]["summary"]["mirrors"] == 2
    assert {
        mirror["node"]: mirror["status"]
        for mirror in response["data"]["mirrors"]
    } == {"backup": "offline", "mainnode": "offline"}
    assert all(
        mirror["cloneAvailable"] is False
        for mirror in response["data"]["mirrors"]
    )


def test_repo_mirrors_handler_applies_linked_org_integrity_anchor():
    handler, calls = _load_handler(
        rows=[
            {
                "key_bi": "source",
                "data": _row(
                    "source", "jett", "forkmesh", root="abc",
                    state_hash="a" * 64,
                )["data"],
            },
            {
                "key_bi": "canonical",
                "data": _row(
                    "canonical", "mirror2", "forkmesh", root="",
                    state_hash="c" * 64, source="remote-clone",
                )["data"],
            },
            {
                "key_bi": "peer",
                "data": _row(
                    "peer", "mirror3", "forkmesh", root="",
                    state_hash="c" * 64, source="remote-clone",
                )["data"],
            },
        ],
        linked_canonical=True,
        endpoint_nodes=["mirror2", "mirror3"],
    )

    response = asyncio.run(
        handler(object(), _Request("GET"), "mirror2", "forkmesh")
    )

    assert response["status"] == 200
    assert {
        item["node"]: item["integrity"]
        for item in response["data"]["mirrors"]
    } == {
        "mirror2": "ok",
        "mirror3": "ok",
        # The appointed backing endpoint is live, so an out-of-pin local
        # publisher is visibly auto-healing instead of permanently rejected.
        "jett": "healing",
    }
    assert any("FROM org_repos" in call for call in calls)


def test_repo_mirrors_handler_uses_one_signed_endpoint_snapshot_for_status():
    handler, _ = _load_handler(
        rows=[
            {"key_bi": "a", "data": _row("a", "mainnode", "forkmesh", root="abc")["data"]},
            {"key_bi": "b", "data": _row("b", "kaif-node", "forkmesh", root="abc")["data"]},
        ],
        endpoint_nodes=["kaif-node"],
    )

    response = asyncio.run(handler(object(), _Request("GET"), "mainnode", "forkmesh"))

    assert response["status"] == 200
    statuses = {m["node"]: m["status"] for m in response["data"]["mirrors"]}
    assert statuses == {"kaif-node": "online", "mainnode": "offline"}
    assert response["data"]["summary"]["online"] == 1
    live = next(
        mirror for mirror in response["data"]["mirrors"]
        if mirror["node"] == "kaif-node"
    )
    assert live["lastSeen"] == _Clock.now()


def test_repo_mirrors_handler_uses_routing_quorum_for_clone_availability():
    rows = [
        {"key_bi": "source", "data": _row(
            "source", "jett", "forkmesh", root="abc",
            state_hash="a" * 64,
        )["data"]},
        {"key_bi": "mirror", "data": _row(
            "mirror", "mirror9", "forkmesh", root="abc",
            state_hash="b" * 64, source="remote-clone",
        )["data"]},
    ]
    rows[1]["data"]["machineName"] = "mirror9"
    handler, _ = _load_handler(
        rows=rows,
        endpoint_records=[{
            "node_name": "mirror9",
            "checked_at": _Clock.now(),
            "forkmesh_verified_at": _Clock.now(),
            "healthy": 1,
            "forkmesh_active": 1,
            "integrity": "ok",
            "abuse_blocked": 0,
        }],
    )

    response = asyncio.run(
        handler(object(), _Request("GET"), "jett", "forkmesh")
    )

    mirror = next(
        item for item in response["data"]["mirrors"]
        if item["node"] == "mirror9"
    )
    assert mirror["integrity"] == "ok"
    assert mirror["cloneAvailable"] is True
    assert mirror["activity"] == "serving"


def test_fresh_healthy_ok_https_endpoint_hydrates_mirror_online():
    queries = []

    async def ensure_schema(_env):
        return None

    async def d1_first(_env, sql, owner, cutoff):
        queries.append((sql, owner, cutoff))
        return {"total": 1 if owner == "mirror2" else 0}

    namespace = {
        "Date": _Clock,
        "HOST_PRESENCE_STALE_MS": 600_000,
        "asyncio": asyncio,
        "_LIVE_HOST_PROBE_MEMO": {},
        "LIVE_HOST_PROBE_MEMO_TTL_MS": 30_000,
        "HYDRATE_PROBE_MAX": 8,
        "HYDRATE_PROBE_BUDGET_MS": 1_500,
        "safe_segment": lambda value: str(value or ""),
        "ensure_schema": ensure_schema,
        "d1_first": d1_first,
    }
    hydrate, *_ = _load(
        "hydrate_repo_group_live_hosts",
        "repo_live_host_count",
        "repo_mirror_same_group",
        extra_globals=namespace,
    )
    rows = [
        {
            "key_bi": "source",
            "data": _row(
                "source", "mainnode", "forkmesh", root="abc"
            )["data"],
        },
        {
            "key_bi": "mirror",
            "data": _row(
                "mirror",
                "mirror2",
                "forkmesh",
                root="abc",
                source="remote-clone",
            )["data"],
        },
    ]

    presence = asyncio.run(
        hydrate(object(), "mainnode", "forkmesh", rows, {}, _Clock.now())
    )

    assert "source" not in presence
    assert presence["mirror"] == _Clock.now()
    mirror_query = next(sql for sql, owner, _cutoff in queries if owner == "mirror2")
    assert "healthy=1" in mirror_query
    assert "abuse_blocked=0" in mirror_query
    assert "integrity='ok'" in mirror_query
    assert "integrity='verified'" not in mirror_query


def test_repo_mirrors_handler_returns_404_for_private_or_unpublished_target():
    private_handler, _ = _load_handler(
        rows=[
            {
                "key_bi": "a",
                "data": _row("a", "mainnode", "forkmesh", visibility="private")["data"],
                "is_private": 1,
            }
        ]
    )
    missing_handler, _ = _load_handler(
        rows=[
            {
                "key_bi": "a",
                "data": _row("a", "othernode", "forkmesh")["data"],
                "is_private": 0,
            }
        ]
    )

    private_response = asyncio.run(
        private_handler(object(), _Request("GET"), "mainnode", "forkmesh")
    )
    missing_response = asyncio.run(
        missing_handler(object(), _Request("GET"), "mainnode", "forkmesh")
    )

    assert private_response == {"status": 404, "data": {"error": "not_found"}}
    assert missing_response == {"status": 404, "data": {"error": "not_found"}}


ENTRY_TEXT = (
    ENTRY.read_text(encoding="utf-8") + "\n"
    + ENTRY.with_name("app_runtime.py").read_text(encoding="utf-8")
)
URLS_TEXT = (ENTRY.parent / "urls.py").read_text(encoding="utf-8")


def test_worker_exposes_repo_mirrors_route_and_uses_payload_builder():
    # The route pattern lives in urls.py and the lazy app runtime dispatches it.
    assert "REPO_MIRRORS_RE = re.compile" in URLS_TEXT
    assert "REPO_MIRRORS_RE" in ENTRY_TEXT
    assert "repo_mirrors_handler" in ENTRY_TEXT
    assert "build_repo_mirrors_payload(" in ENTRY_TEXT
    assert "host_presence" in ENTRY_TEXT
    assert "repo_first_hosted" in ENTRY_TEXT
    assert "mirrors_match = REPO_MIRRORS_RE.match" in APP_RUNTIME.read_text(
        encoding="utf-8")


def test_worker_exposes_exact_node_readme_reachability_probe():
    assert "REPO_MIRROR_REACHABILITY_RE = re.compile" in URLS_TEXT
    assert "repo_mirror_reachability_handler" in ENTRY_TEXT
    assert 'admitted = node in context.get("nodes", set())' in ENTRY_TEXT
    assert 'WHERE node_name=?' in ENTRY_TEXT
    assert '"blob", {"path": "README.md"}' in ENTRY_TEXT
    assert '"readmeLoaded": bool(reachable)' in ENTRY_TEXT
    assert "REPO_MIRROR_REACHABILITY_RE.match(original_url.path)" in ENTRY_TEXT
    assert 'context["routeOwner"] = route_owner' in ENTRY_TEXT
    assert "await _org_repo_node(env, route_owner, route_repo) == owner" in ENTRY_TEXT
    probe_start = ENTRY_TEXT.index("async def repo_mirror_reachability_handler")
    probe_end = ENTRY_TEXT.index("\n\ndef _https_mirror_merge_body", probe_start)
    probe = ENTRY_TEXT[probe_start:probe_end]
    assert "for endpoint in" not in probe
    assert "_https_mirror_route_advance" not in probe


def test_reachability_probe_asks_every_group_mirror_before_judging_it():
    # adhoc #1422: the probe used to reject a node unless the router already
    # considered it servable — inside the state-pin window AND holding a fresh
    # health lease AND passing a repository proof — so a single mirror reported
    # "README loaded" while every other one reported "Unavailable" without a
    # request ever having been made to it. Any mirror in the group gets a real
    # README.md request; the serving gates only classify a failure afterwards.
    probe_start = ENTRY_TEXT.index("async def repo_mirror_reachability_handler")
    probe_end = ENTRY_TEXT.index("\n\ndef _https_mirror_merge_body", probe_start)
    probe = ENTRY_TEXT[probe_start:probe_end]
    # A mirror of this repo that is merely behind the pin window is still probed.
    assert 'node not in context.get("groupNodes", set())' in probe
    assert '"groupNodes": group_nodes,' in ENTRY_TEXT
    # No pre-emptive bail on the cached lease/proof state.
    assert "endpoint_unavailable" not in probe
    assert "return result(False, \"repository_proof_failed\")" not in probe
    assert "https_routing.endpoint_eligible" not in probe
    # Only a node with nowhere to send the request short-circuits.
    assert 'return result(False, "endpoint_not_registered")' in probe
    assert '"endpointRegistered": endpoint_registered,' in probe
    # Failures are classified after the fact, most upstream cause first.
    classifier_start = ENTRY_TEXT.index(
        "async def _mirror_reachability_failure_reason")
    classifier = ENTRY_TEXT[classifier_start:ENTRY_TEXT.index(
        "\n\nasync def repo_mirror_reachability_handler", classifier_start)]
    for reason in ("state_pin_not_admitted", "repository_proof_failed",
                   "endpoint_stale"):
        assert reason in classifier
    assert classifier.index("state_pin_not_admitted") < classifier.index(
        "repository_proof_failed") < classifier.index("endpoint_stale")


def test_hydrate_live_host_probes_capped_budgeted_and_memoized():
    # adhoc #167: hydrate used to await one DO probe per stale group member on
    # every public info/refs, unbounded. A group full of dead mirrors (fleet
    # die-off) hung the request until the Workers runtime canceled it ("Cannot
    # enter into task"). Gathering them instead traded that for a worse
    # failure -- concurrent Pyodide tasks wedge the isolate outright -- so the
    # probes are sequential again, but now capped, wall-clock budgeted, and
    # memoized across calls so repeated
    # requests walk a large group instead of re-probing all of it every time.
    probed = []

    async def repo_live_host_count(_env, owner, repo):
        probed.append(f"{owner}/{repo}")
        return 1 if owner == "live-node" else 0

    namespace = {
        "asyncio": asyncio,
        "Date": _Clock,  # frozen, so the budget below never trips in this test
        "_LIVE_HOST_PROBE_MEMO": {},
        "LIVE_HOST_PROBE_MEMO_TTL_MS": 30_000,
        "HYDRATE_PROBE_MAX": 8,
        "HYDRATE_PROBE_BUDGET_MS": 1_500,
        "repo_live_host_count": repo_live_host_count,
    }
    hydrate, _ = _load(
        "hydrate_repo_group_live_hosts", "repo_mirror_same_group",
        extra_globals=namespace)

    rows = [
        {"key_bi": "src",
         "data": _row("src", "mainnode", "forkmesh", root="abc")["data"]},
        {"key_bi": "live",
         "data": _row("live", "live-node", "forkmesh", root="abc",
                      source="mainnode")["data"]},
    ] + [
        {"key_bi": f"dead{i}",
         "data": _row(f"dead{i}", f"dead-node-{i}", "forkmesh", root="abc",
                      source="mainnode")["data"]}
        for i in range(19)
    ]

    presence = asyncio.run(
        hydrate(object(), "mainnode", "forkmesh", rows, {}, 1_000_000))
    # Capped: 21 stale members, only the first 8 probed this request.
    assert len(probed) == 8
    # The next two calls (same 30s memo window) continue where the last
    # stopped instead of re-probing the memoized members.
    presence = asyncio.run(
        hydrate(object(), "mainnode", "forkmesh", rows, presence, 1_000_000))
    presence = asyncio.run(
        hydrate(object(), "mainnode", "forkmesh", rows, presence, 1_000_000))
    assert len(probed) == 21
    assert len(set(probed)) == 21  # no member probed twice within the TTL
    # A fourth call is fully memoized -- zero new subrequests.
    presence = asyncio.run(
        hydrate(object(), "mainnode", "forkmesh", rows, presence, 1_000_000))
    assert len(probed) == 21
    # Presence reflects the ground truth gathered across the calls.
    assert "live" in presence
    assert not any(key.startswith("dead") for key in presence)
    # The probes run one at a time. Gathering them put a fan-out of Durable
    # Object subrequests in flight as concurrent Pyodide tasks, which the
    # Workers Python runtime can re-enter -- and a re-entered task wedges the
    # isolate into answering 1101 for every later request (see
    # tests/test_worker_task_concurrency.py). Sequentially, the count cap is no
    # longer a latency bound on its own, so the pass also carries a wall-clock
    # budget and leaves the rest to the memo.
    src = _WORKER_SRC[_WORKER_SRC.index(
        "async def hydrate_repo_group_live_hosts"):]
    src = src[:src.index("\nasync def ", 10)]
    assert "asyncio.gather" not in src
    assert "hosts = await repo_live_host_count(" in src
    assert "HYDRATE_PROBE_BUDGET_MS" in src
