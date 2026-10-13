#!/usr/bin/env python3
"""Organization-alias authorization for pull-inbox drains."""

import ast
import asyncio
from pathlib import Path
from urllib.parse import parse_qs, urlparse


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
ENTRY_TEXT = ENTRY.read_text(encoding="utf-8")
QT_PULLS = (
    ROOT.parent / "desktop" / "src" / "MainWindowPulls.cpp"
).read_text(encoding="utf-8")
QT_ISSUES = (
    ROOT.parent / "desktop" / "src" / "MainWindowIssues.cpp"
).read_text(encoding="utf-8")
QT_DISCUSSIONS = (
    ROOT.parent / "desktop" / "src" / "MainWindowDiscussions.cpp"
).read_text(encoding="utf-8")


def _load(name, extra_globals):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    dependencies = {
        "_authorize_repo_inbox_owner": {
            "_authorize_repo_inbox_owner",
            "_authorized_repo_inbox_signing_key",
        },
    }.get(name, {name})
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in dependencies
    ]
    assert {node.name for node in selected} == dependencies
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))
    namespace = dict(extra_globals)
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace[name]


def _run(coro):
    return asyncio.run(coro)


def _function_source(name):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    node = next(
        item for item in tree.body
        if isinstance(item, (ast.FunctionDef, ast.AsyncFunctionDef))
        and item.name == name
    )
    return ast.get_source_segment(ENTRY_TEXT, node)


class _Date:
    @staticmethod
    def now():
        return 1_000_000


class _Request:
    url = (
        "https://forkmesh.test/api/repo/forkmesh/forkmesh/pulls"
        "?owner=forkmesh&ts=1000000&sig=org-admin-proof"
    )


def _runtime(*, direct=False, private=False, linked=True, members=None):
    calls = {"verified": [], "links": 0, "members": 0}

    async def authorized_owner_key(_env, _request, _owner):
        return "direct-key" if direct else ""

    async def repo_is_private(_env, owner, repo):
        assert (owner, repo) == ("mirror2", "forkmesh")
        return private

    async def org_row(_env, org):
        assert org == "forkmesh"
        return "org-bi", {"name": "forkmesh"}

    async def d1_first(_env, sql, *args):
        assert "FROM org_repos" in sql
        assert args == ("org-bi", "forkmesh", "mirror2")
        calls["links"] += 1
        return {"ok": 1} if linked else None

    async def d1_all(_env, sql, *args):
        assert "FROM org_members" in sql
        assert "role IN ('owner','admin')" in sql
        assert args == ("org-bi", 200)
        calls["members"] += 1
        return list(members or [])

    async def owner_signing_pubkeys(_env, account):
        return ["key:" + account]

    async def ed25519_verify(public_key, sig, canonical):
        account = public_key.removeprefix("key:")
        calls["verified"].append(
            (account, sig, canonical.decode("utf-8")))
        return account == "jett"

    def safe_segment(value):
        value = str(value or "")
        return value if value and "/" not in value else None

    return {
        "_authorized_owner_signing_key": authorized_owner_key,
        "_repo_is_private": repo_is_private,
        "_org_row": org_row,
        "d1_first": d1_first,
        "d1_all": d1_all,
        "_owner_signing_pubkeys": owner_signing_pubkeys,
        "ed25519_verify": ed25519_verify,
        "REPO_API_PREFIX_RE": __import__(
            "re").compile(r"^/api/repo/([^/]+)/([^/]+)(?:/|$)"),
        "safe_segment": safe_segment,
        "parse_qs": parse_qs,
        "urlparse": urlparse,
        "Date": _Date,
        "LOGIN_MAX_SKEW_MS": 300_000,
        "MAX_ORG_MEMBERS": 200,
        "MAX_NODE_NAME": 63,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "valid_node_name": lambda value: bool(value),
    }, calls


def test_current_org_admin_device_can_drain_exact_linked_public_repo():
    runtime, calls = _runtime(
        members=[
            {"name": "plain-member", "role": "member"},
            {"name": "jett", "role": "admin"},
        ])
    authorize = _load("_authorize_repo_inbox_owner", runtime)

    assert _run(authorize(
        object(), _Request(), "mirror2", "forkmesh")) is True
    assert calls["links"] == 1
    assert calls["members"] == 1
    assert calls["verified"] == [(
        "jett",
        "org-admin-proof",
        "forkmesh-issues-pull-v1\nforkmesh\n1000000",
    )]


def test_plain_org_members_and_unlinked_aliases_are_denied():
    runtime, calls = _runtime(
        members=[{"name": "reader", "role": "member"}])
    authorize = _load("_authorize_repo_inbox_owner", runtime)
    assert _run(authorize(
        object(), _Request(), "mirror2", "forkmesh")) is False
    assert calls["verified"] == []

    runtime, calls = _runtime(
        linked=False, members=[{"name": "jett", "role": "owner"}])
    authorize = _load("_authorize_repo_inbox_owner", runtime)
    assert _run(authorize(
        object(), _Request(), "mirror2", "forkmesh")) is False
    assert calls["members"] == 0
    assert calls["verified"] == []


def test_org_role_never_grants_private_inbox_access():
    runtime, calls = _runtime(
        private=True, members=[{"name": "jett", "role": "owner"}])
    authorize = _load("_authorize_repo_inbox_owner", runtime)

    assert _run(authorize(
        object(), _Request(), "mirror2", "forkmesh")) is False
    assert calls["links"] == 0
    assert calls["members"] == 0
    assert calls["verified"] == []


def test_direct_owner_proof_still_authorizes_private_repositories():
    runtime, calls = _runtime(direct=True, private=True)
    authorize = _load("_authorize_repo_inbox_owner", runtime)

    assert _run(authorize(
        object(), _Request(), "mirror2", "forkmesh")) is True
    assert calls["links"] == 0
    assert calls["members"] == 0


def test_pull_handler_uses_repo_aware_leased_gate_for_read_and_ack():
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    handler = next(
        node for node in tree.body
        if isinstance(node, ast.AsyncFunctionDef)
        and node.name == "pulls_handler"
    )
    source = ast.get_source_segment(ENTRY_TEXT, handler)
    assert source.count("_authorized_repo_inbox_signing_key(") == 2
    assert "_claim_collaboration_inbox_on_mirror(" in source
    assert "_leased_inbox_page(" in source
    assert "_drain_collaboration_inbox_on_mirror(" in source
    assert source.count("_authorize_owner(env, request, owner)") == 0


def test_issue_handler_uses_repo_aware_leased_gate_for_read_and_ack():
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    handler = next(
        node for node in tree.body
        if isinstance(node, ast.AsyncFunctionDef)
        and node.name == "issues_handler"
    )
    source = ast.get_source_segment(ENTRY_TEXT, handler)
    assert source.count("_authorized_repo_inbox_signing_key(") == 2
    assert "_claim_issue_inbox(" in source
    assert "_leased_inbox_page(" in source

    drain = QT_ISSUES.split(
        "void MainWindow::drainIssuesInboxFor", 1)[1].split(
            "void MainWindow::applyIssuesInboxPayload", 1)[0]
    apply = QT_ISSUES.split(
        "void MainWindow::applyIssuesInboxPayload", 1)[1].split(
            "void MainWindow::updateIssueVoteState", 1)[0]
    assert "hasOwnerSigningCapability()" in drain
    assert "hasOwnerSigningCapability(repo.owner)" not in drain
    assert "hasOwnerSigningCapability()" in apply
    assert "hasOwnerSigningCapability(repo.owner)" not in apply


def test_qt_manual_activitypub_post_signs_as_the_visible_user():
    publish = QT_ISSUES.split(
        "void MainWindow::publishQuickAddToActivityPub", 1)[1].split(
            "void MainWindow::quickAddIssue", 1)[0]
    assert "const QString signer = accountOwner().trimmed().toLower();" in publish
    assert "const QString author = topBarUserName().trimmed().toLower();" in publish
    assert "hasOwnerSigningCapability(signer)" in publish
    assert "signedInboxQuery(signer)" in publish
    assert "hasOwnerSigningCapability(owner)" not in publish
    assert "signedInboxQuery(owner)" not in publish


def test_leased_reads_stay_confined_to_the_claiming_device():
    # The handlers read through the bounded page reader now, so this is where
    # the lease scope has to hold: without it a drain would hand one device
    # rows another device had already claimed.
    page = _function_source("_leased_inbox_page")
    assert "claimed_by_bi=?" in page
    assert "mirrored_at=0" in page


def test_qt_pull_drain_signs_org_alias_but_keeps_server_authoritative():
    drain = QT_PULLS.split(
        "void MainWindow::drainPullsInboxFor", 1)[1].split(
            "void MainWindow::applyPullsInboxPayload", 1)[0]
    apply = QT_PULLS.split(
        "void MainWindow::applyPullsInboxPayload", 1)[1].split(
            "void MainWindow::pollOwnedInboxes", 1)[0]
    query = QT_PULLS.split(
        "QUrlQuery MainWindow::signedInboxQuery", 1)[1].split(
            "void MainWindow::scheduleRelaySync", 1)[0]
    assert "hasOwnerSigningCapability()" in drain
    assert "hasOwnerSigningCapability(repo.owner)" not in drain
    assert "hasOwnerSigningCapability()" in apply
    assert "hasOwnerSigningCapability(repo.owner)" not in apply
    assert "hasOwnerSigningCapability()" in query
    assert "hasOwnerSigningCapability(owner)" not in query


def test_owner_intake_never_gates_the_drain_on_the_session_account_name():
    # The relay authorizes a PUBLIC owner (org alias, or a namespace this key
    # owns that is no longer the account name); a local name-equality test can
    # only produce false negatives. The asserts above check for the literal
    # "repo.owner", which a drift past them defeated simply by hoisting the same
    # value into a `signer` variable -- and that silently killed every drain for
    # forkmesh/forkmesh while the site showed a pending badge indefinitely. Pin
    # the owner-side shape. Mirror intake is native to the managed Go service and
    # no longer exists in the desktop.
    for label, text, fn in (
        ("issues", QT_ISSUES, "void MainWindow::drainIssuesInboxFor"),
        ("pulls", QT_PULLS, "void MainWindow::drainPullsInboxFor"),
        ("discussions", QT_DISCUSSIONS,
         "void MainWindow::drainDiscussionsInboxFor"),
    ):
        drain = text.split(fn, 1)[1][:4000]
        assert "!hasOwnerSigningCapability(signer))" not in drain, label
        assert "hasOwnerSigningCapability()" in drain, label
        assert "mirrorIntake" not in drain, label
        assert "forceMirrorIntake" not in drain, label
