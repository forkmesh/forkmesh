#!/usr/bin/env python3
"""Online mirrors merge collaboration submissions and drain the queue.

Web-submitted issues, pull requests, and discussions no longer wait as
"pending" for the source-of-truth node to come online: any approved online
mirror leases the rows, commits them onto the branch it serves, and its
?mirror=1 acknowledgement deletes them from the inbox outright. The ack also
carries the mirror's signed post-merge refs attestation so the newly served
state joins the clone integrity pin window while the source is offline.
"""

import ast
import asyncio
import re
from pathlib import Path
from urllib.parse import parse_qs, urlparse


# MainWindowRepos.cpp was split into feature files; read them in the original
# source order so section slices that span functions still line up.
MAIN_WINDOW_REPOS_SOURCES = (
    "MainWindowRepos.cpp",
    "MainWindowReposInternal.h",
    "MainWindowRepoSettings.cpp",
    "MainWindowRepoSync.cpp",
    "MainWindowMirrorPush.cpp",
    "MainWindowRepoFetch.cpp",
    "MainWindowAccounts.cpp",
)

ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
ENTRY_TEXT = (
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "node_events_runtime.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
)
QT_ROOT = ROOT.parent / "desktop" / "src"


def _function_source(name):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    node = next(
        item for item in tree.body
        if isinstance(item, (ast.FunctionDef, ast.AsyncFunctionDef))
        and item.name == name
    )
    return ast.get_source_segment(ENTRY_TEXT, node)


def _load_helpers(names, extra_globals=None):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in names
    ]
    assert {node.name for node in selected} == set(names)
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))
    namespace = {
        "parse_qs": parse_qs,
        "urlparse": urlparse,
        "re": re,
        "ISSUE_INBOX_CLAIM_TTL_MS": 300_000,
    }
    namespace.update(extra_globals or {})
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


class _Date:
    @staticmethod
    def now():
        return 1_000_000


class _Request:
    url = "https://forkmesh.test/api/repo/o/r/pulls?ids=7,9"


def test_handlers_offer_mirror_leases_and_drain_on_ack():
    for handler_name, table, topic in (
        ("pulls_handler", "pull_inbox", "pulls"),
        ("discussions_handler", "discussion_inbox", "discussions"),
    ):
        source = _function_source(handler_name)
        assert "_authorized_mirror_issue_signing_key(" in source
        assert "_claim_collaboration_inbox_on_mirror(" in source
        assert table in source
        # The mirror ack is a real drain now, and it records the mirror's
        # post-merge refs attestation so the served state stays pinned.
        assert "_drain_collaboration_inbox_on_mirror(" in source
        assert "_record_mirror_attested_state(" in source
        assert '"retainedForSource": False' in source
        assert '"retainedForSource": True' not in source
        assert f'notify_repo_mirrors(env, owner, repo, "{topic}")' in source
        if handler_name == "pulls_handler":
            # Pulls are ordinary mirror-first writes. A source health lease
            # must never prevent an approved online mirror from claiming one.
            assert "_deferred_mirror_intake(" not in source
            assert "return deferred" not in source
        else:
            # Discussions retain their existing source-preferred policy; this
            # change is intentionally scoped to issues and pull requests.
            assert "_deferred_mirror_intake(" in source
            assert "return deferred" in source

    issues = _function_source("issues_handler")
    assert "_drain_issue_inbox_on_mirror(" in issues
    assert "_record_mirror_attested_state(" in issues
    assert '"retainedForSource": False' in issues
    assert '"retainedForSource": True' not in issues
    assert "_deferred_mirror_intake(" not in issues
    assert "return deferred" not in issues
    # Standing down must read as an empty queue, not a refusal: the desktop
    # treats a rejected mirror drain as misconfiguration and backs off for hours.
    stand_down = _function_source("_deferred_mirror_intake")
    assert '"pending": []' in stand_down
    assert '"sourceOnline": True' in stand_down
    assert "status=" not in stand_down


def test_mirror_intake_defers_to_fresh_canonical_source_endpoint():
    calls = []

    async def public_context(_env, owner, repo):
        calls.append(("context", owner, repo))
        return {"sourceNode": "source-node"}

    async def d1_first(_env, sql, *args):
        calls.append(("query", sql, args))
        return {"online": 1}

    ns = _load_helpers(
        {"_collaboration_source_online"},
        extra_globals={
            "Date": _Date,
            "HTTPS_MIRROR_STATUS_FRESH_MS": 600_000,
            "MAX_NODE_NAME": 63,
            "_https_mirror_public_context": public_context,
            "clean_string": lambda value, limit: str(value or "")[:limit],
            "valid_node_name": lambda value: value == "source-node",
            "d1_first": d1_first,
        },
    )

    online = asyncio.run(ns["_collaboration_source_online"](
        object(), "owner", "repo"))

    assert online is True
    query = next(call for call in calls if call[0] == "query")
    assert "lower(node_name)=?" in query[1]
    assert "checked_at>=?" in query[1]
    assert "healthy=1" in query[1]
    assert "integrity='ok'" in query[1]
    assert query[2] == ("source-node", 400_000)


def test_unauthorized_mirror_is_not_stood_down_so_it_still_gets_401():
    # An empty signing key means the caller is not an approved mirror at all.
    # Returning a stand-down here would convert that 401 into a cheerful "no
    # submissions", hiding a misconfigured node instead of reporting it — and
    # the source-online lookup must not even be consulted to decide that.
    consulted = []

    async def source_online(_env, _owner, _repo):
        consulted.append(True)
        return True

    ns = _load_helpers(
        {"_deferred_mirror_intake"},
        extra_globals={
            "_collaboration_source_online": source_online,
            "json_response": lambda payload, **kw: ("json", payload, kw),
        },
    )

    stood_down = asyncio.run(ns["_deferred_mirror_intake"](
        object(), "owner", "repo", "", "mirror-node"))

    assert stood_down is None
    assert consulted == []


def test_authorized_mirror_stands_down_with_an_empty_queue_not_an_error():
    async def source_online(_env, _owner, _repo):
        return True

    ns = _load_helpers(
        {"_deferred_mirror_intake"},
        extra_globals={
            "_collaboration_source_online": source_online,
            "json_response": lambda payload, **kw: ("json", payload, kw),
        },
    )

    kind, payload, kwargs = asyncio.run(ns["_deferred_mirror_intake"](
        object(), "owner", "repo", "signing-key", "mirror-node"))

    assert kind == "json"
    # A 200 carrying nothing pending: the desktop reads a non-2xx mirror drain
    # as misconfiguration and slows retries to hours.
    assert kwargs == {}
    assert payload["ok"] is True
    assert payload["pending"] == []
    assert payload["sourceOnline"] is True
    assert payload["mirror"] == "mirror-node"


def test_mirror_intake_stays_available_when_source_is_offline_or_unknown():
    async def public_context(_env, _owner, _repo):
        return {"sourceNode": "source-node"}

    async def d1_first(_env, _sql, *_args):
        return None

    ns = _load_helpers(
        {"_collaboration_source_online"},
        extra_globals={
            "Date": _Date,
            "HTTPS_MIRROR_STATUS_FRESH_MS": 600_000,
            "MAX_NODE_NAME": 63,
            "_https_mirror_public_context": public_context,
            "clean_string": lambda value, limit: str(value or "")[:limit],
            "valid_node_name": lambda value: value == "source-node",
            "d1_first": d1_first,
        },
    )

    assert asyncio.run(ns["_collaboration_source_online"](
        object(), "owner", "repo")) is False


def test_mirror_claim_is_exact_and_ack_deletes_only_leased_rows():
    calls = []

    async def blind_index(_env, value):
        return "bi:" + value

    async def d1_run(_env, sql, *args):
        calls.append(("run", sql, args))

    async def d1_first(_env, sql, *args):
        calls.append(("first", sql, args))
        return {"c": 2}

    ns = _load_helpers(
        {
            "_drain_ids_from_request",
            "_claim_collaboration_inbox_on_mirror",
            "_drain_collaboration_inbox_on_mirror",
            "_log_inbox_drain",
        },
        extra_globals={
            "Date": _Date,
            "blind_index": blind_index,
            "d1_run": d1_run,
            "d1_first": d1_first,
        },
    )
    claimant = asyncio.run(ns["_claim_collaboration_inbox_on_mirror"](
        object(), "pull_inbox", "repo-bi", "signing-key"))
    assert claimant == "bi:collaboration-inbox-claim:pull_inbox:signing-key"
    assert "mirrored_at=0" in calls[0][1]

    count = asyncio.run(ns["_drain_collaboration_inbox_on_mirror"](
        object(), _Request(), "pull_inbox", "repo-bi", claimant,
        "mirror-node"))
    assert count == 2
    deletes = [c for c in calls if c[1].startswith("DELETE FROM pull_inbox")]
    assert len(deletes) == 1
    # The delete names the exact acknowledged rows and stays scoped to this
    # mirror's lease, so a submission that lands mid-drain survives and a
    # competing claimant can never sweep another mirror's rows.
    assert "id IN (?,?)" in deletes[0][1]
    assert "claimed_by_bi=?" in deletes[0][1]
    assert deletes[0][2] == ("repo-bi", 7, 9, claimant)
    logs = [c for c in calls if c[1].startswith("INSERT INTO inbox_drain_log")]
    assert len(logs) == 1 and logs[0][2][2] == "pulls"


def test_issue_mirror_ack_deletes_only_leased_rows():
    calls = []

    async def d1_run(_env, sql, *args):
        calls.append(("run", sql, args))

    async def d1_first(_env, sql, *args):
        return {"c": 1}

    ns = _load_helpers(
        {
            "_drain_ids_from_request",
            "_drain_issue_inbox_on_mirror",
            "_log_inbox_drain",
        },
        extra_globals={
            "Date": _Date,
            "d1_run": d1_run,
            "d1_first": d1_first,
        },
    )
    count = asyncio.run(ns["_drain_issue_inbox_on_mirror"](
        object(), _Request(), "repo-bi", "claimant-bi", "mirror-node"))
    assert count == 1
    deletes = [c for c in calls if c[1].startswith("DELETE FROM issue_inbox")]
    assert len(deletes) == 1
    assert "id IN (?,?)" in deletes[0][1]
    assert "claimed_by_bi=?" in deletes[0][1]
    logs = [c for c in calls if c[1].startswith("INSERT INTO inbox_drain_log")]
    assert len(logs) == 1 and logs[0][2][2] == "issues"


def _attestation_namespace(runs, verify_result=True, org_node=""):
    async def blind_index(_env, value):
        return "bi:" + value

    async def d1_run(_env, sql, *args):
        runs.append((sql, args))

    async def _owner_signing_pubkeys(_env, node):
        return ["mirror-public-key"] if node == "mirror-node" else []

    seen = {}

    async def ed25519_verify(pub, sig, canonical):
        seen["pub"] = pub
        seen["canonical"] = canonical
        return verify_result and sig == "good-sig"

    async def _org_repo_node(_env, _owner, _repo):
        return org_node

    def clean_string(value, limit):
        return str(value or "")[:limit]

    ns = _load_helpers(
        {"_record_mirror_attested_state"},
        extra_globals={
            "Date": _Date,
            "LOGIN_MAX_SKEW_MS": 120_000,
            "STATE_PIN_HISTORY": 100,
            "blind_index": blind_index,
            "d1_run": d1_run,
            "_owner_signing_pubkeys": _owner_signing_pubkeys,
            "_claimed_node_signing_pubkeys": _owner_signing_pubkeys,
            "ed25519_verify": ed25519_verify,
            "clean_string": clean_string,
            "MAX_NODE_NAME": 64,
            "_org_repo_node": _org_repo_node,
        },
    )
    return ns, seen


def test_mirror_state_attestation_joins_the_pin_history():
    digest = "ab" * 32

    class Request:
        url = ("https://forkmesh.test/api/repo/o/r/issues?mirror=1&ids=3"
               "&state=%s&stateTs=1000000&stateSig=good-sig" % digest)

    runs = []
    ns, seen = _attestation_namespace(runs)
    ok = asyncio.run(ns["_record_mirror_attested_state"](
        object(), Request(), "o", "r", "mirror-node"))
    assert ok is True
    # The canonical matches what the desktop signs on catalog publishes, bound
    # to the acknowledging mirror node and the exact repo segment.
    assert seen["canonical"] == (
        "forkmesh-repostate-v1\nmirror-node\nr\n%s\n1000000" % digest
    ).encode()
    inserts = [r for r in runs
               if r[0].startswith("INSERT INTO repo_state_history")]
    assert len(inserts) == 1
    assert inserts[0][1] == ("bi:o/r", digest, _Date.now())
    trims = [r for r in runs
             if r[0].startswith("DELETE FROM repo_state_history")]
    assert len(trims) == 1 and trims[0][1][-1] == 100


def test_mirror_state_attestation_lands_under_the_backing_node_key():
    # The drain arrives under the public organization alias, but every reader
    # of repo_state_history (clone_state_pins and the endpoint acceptance set)
    # resolves the backing node first. An attestation recorded under the alias
    # key is invisible to acceptance: the 2026-08-19 outage shape, where the
    # fleet survived on the two-endpoint quorum alone until one diverged
    # intake dropped every mirror out of freshness.
    digest = "cd" * 32

    class Request:
        url = ("https://forkmesh.test/api/repo/o/r/issues?mirror=1&ids=3"
               "&state=%s&stateTs=1000000&stateSig=good-sig" % digest)

    runs = []
    ns, _seen = _attestation_namespace(runs, org_node="backing")
    ok = asyncio.run(ns["_record_mirror_attested_state"](
        object(), Request(), "o", "r", "mirror-node"))
    assert ok is True
    inserts = [r for r in runs
               if r[0].startswith("INSERT INTO repo_state_history")]
    assert len(inserts) == 1
    assert inserts[0][1] == ("bi:backing/r", digest, _Date.now())


def test_mirror_state_attestation_rejects_bad_or_stale_signatures():
    digest = "cd" * 32

    class BadSig:
        url = ("https://forkmesh.test/api/repo/o/r/issues?state=%s"
               "&stateTs=1000000&stateSig=forged" % digest)

    class Stale:
        url = ("https://forkmesh.test/api/repo/o/r/issues?state=%s"
               "&stateTs=1&stateSig=good-sig" % digest)

    for request in (BadSig(), Stale()):
        runs = []
        ns, _ = _attestation_namespace(runs)
        ok = asyncio.run(ns["_record_mirror_attested_state"](
            object(), request, "o", "r", "mirror-node"))
        assert ok is False
        assert not runs


def test_managed_go_node_materializes_all_three_record_kinds_without_desktop():
    issues = (QT_ROOT / "MainWindowIssues.cpp").read_text(encoding="utf-8")
    server = (ROOT.parent / "server" / "intake.go").read_text(encoding="utf-8")
    daemon = (ROOT.parent / "server" / "daemon.go").read_text(encoding="utf-8")

    assert "pollMirrorIssueInboxes" not in issues
    assert "forceMirrorIntake" not in issues
    assert "mirrorIntake" not in issues
    for marker in (
        "materializeIssueInbox", "materializePullInbox",
        "materializeDiscussionInbox", "ackCollaboration",
    ):
        assert marker in server
    assert "drainCollaborationKind" in daemon
    assert "isCollaborationKind(event.Topic)" in daemon


def test_qt_legacy_inbound_pr_convergence_fingerprints_all_refs_not_only_head():
    repos = "".join(
        (QT_ROOT / name).read_text(encoding="utf-8")
        for name in MAIN_WINDOW_REPOS_SOURCES
    )
    backend = (QT_ROOT / "ChatBackend.h").read_text(encoding="utf-8")
    server = (QT_ROOT / "ServerNode.cpp").read_text(encoding="utf-8")
    roster = repos.split(
        "void MainWindow::syncMirrorsBehindRoster()", 1)[1].split(
            "void MainWindow::propagateRepoUpdate(int index)", 1)[0]
    peer = repos.split(
        "void MainWindow::onPeerMirrorUpdated(", 1)[1].split(
            "void MainWindow::onPeerMirrorSynced(", 1)[0]

    assert "QString refsFingerprint;" in backend
    # New desktops never publish catalogs/adverts. Keep decoding the compact
    # fingerprint only so an older online peer cannot regress convergence.
    assert 'head.insert("r", m.refsFingerprint);' not in server
    assert 'head.value("r").toString().left(64)' in server
    assert "m.refsFingerprint != localRefsFingerprint" in roster
    # Peers too old to advertise a fingerprint still fall back to the HEAD probe.
    assert "mirrorHasCommit(repo.mirrorPath, m.commit)" in roster
    # The local hash is only computed when a peer actually advertises one, so a
    # peer hello does not spawn a git process per repo.
    assert "if (!localRefsFingerprintDone) {" in roster
    # The converge worker owns the retry cadence. A failed read must not mark a
    # peer generation as handled forever; the same mismatch remains eligible
    # for the next safety pass.
    assert "m_mirrorRefsFingerprintActed" not in roster
    assert "behind = true;" in roster
    # A PR advances forkmesh/pulls without moving HEAD. The live update must
    # never treat possession of the primary commit as full convergence.
    assert "mirrorHasCommit(matched.mirrorPath, target)" not in peer


def test_mirror_intake_uses_group_membership_plus_fresh_endpoint_integrity():
    source = _function_source("_authorized_mirror_issue_signing_key")
    assert '(context or {}).get("groupNodes", set())' in source
    assert "checked_at>=? AND healthy=1" in source
    assert "integrity='ok'" in source
    assert "abuse_blocked=0" in source
    assert "forkmesh_active=1" not in source
    assert 'await _org_repo_node(env, "forkmesh", "forkmesh")' in source
    assert "(not flagship_intake and node not in allowed_nodes)" in source
    assert "_claimed_node_signing_pubkeys" in source
    context = _function_source("_https_mirror_public_context")
    assert '"sourceNode": clean_string(' in context
    assert 'canonical.get("machineName") or canonical.get("owner", "")' in context


def test_go_mirror_acks_attest_served_state_and_keeps_owner_only_rows():
    issues = (QT_ROOT / "MainWindowIssues.cpp").read_text(encoding="utf-8")
    server = (ROOT.parent / "server" / "intake.go").read_text(encoding="utf-8")

    assert "appendMirrorStateAttestation" not in issues
    assert '"forkmesh-repostate-v1"' in server
    assert "wantsAgent" in server
    assert "item.FediverseID" in server

    # Owner-only side effects survive the drain: rows carrying a fediverse
    # mention id or the owner's auto-agent request stay queued for the source
    # node instead of being acknowledged by a mirror.
    assert "if !wantsAgent && item.FediverseID == \"\"" in server
