#!/usr/bin/env python3
"""Self-assignment for public contributors (issue #570).

Assigning used to mean signing an "assignees" event, which carries the WHOLE
list and therefore replaces it. That is why the relay put it behind
``_authorize_issue_manager``: a visitor allowed to send one could quietly drop
every other name. So the website only offered assignment to maintainers, and a
contributor who wanted to say "I'm working on this" had no button at all.

A "claim" is the narrower event that makes the button safe. It names exactly
one account and a direction, and every projection folds it as a delta, so the
worst a visitor can do with it is move their own name. The relay therefore
authorizes it on identity alone — the account session must BE the account named
in the claim — and no repository role is involved.

Four implementations have to agree about that event: the browser that signs it,
the relay that verifies and authorizes it, the desktop host that folds it into
the issue file, and the Go mirror that projects it for everyone reading the web
UI. The canonical-content vector below is the same one pinned in
desktop/tests/test_crypto.cpp and exercised there against the C++ signer.
"""

import ast
import asyncio
import hashlib
import json
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[1]
ISSUE_IO = (ROOT / "public/dashboard/js/03-issues-profile-io.js").read_text()
REPO_UI = (ROOT / "public/dashboard/js/06-repo-content.js").read_text()
WIRING = (ROOT / "public/dashboard/js/08-repo-detail-network.js").read_text()
ENTRY = (ROOT / "src/entry.py").read_text()
EVENTS = (ROOT / "src/events.py").read_text()
ISSUE_STORE = (ROOT.parent / "desktop/src/IssueStore.cpp").read_text()
ISSUE_STORE_H = (ROOT.parent / "desktop/src/IssueStore.h").read_text()
QT_CRYPTO_TEST = (ROOT.parent / "desktop/tests/test_crypto.cpp").read_text()
GO_INTAKE = (ROOT.parent / "server/intake.go").read_text()

# sha256("alice\0assigned") — the content half of the signed canonical string.
CLAIM_CONTENT_HASH = "653b945284569a5ea948885cb71b6bb81043fa3bc446e0fc47871fb5ad798876"


def _slice(source, start, end):
    begin = source.index(start)
    finish = source.index(end, begin)
    return source[begin:finish]


def _function(source, name):
    """The source text of one named JS function, ready to run under node.

    The body brace is found by first stepping over the parameter list: a
    default like ``fields = {}`` puts a brace before the body, so counting
    braces from the declaration would stop one character in.
    """
    start = source.index(f"function {name}(")
    cursor = source.index("(", start)
    parens = 0
    while cursor < len(source):
        if source[cursor] == "(":
            parens += 1
        elif source[cursor] == ")":
            parens -= 1
            if parens == 0:
                break
        cursor += 1
    cursor = source.index("{", cursor)
    depth = 0
    while cursor < len(source):
        if source[cursor] == "{":
            depth += 1
        elif source[cursor] == "}":
            depth -= 1
            if depth == 0:
                return source[start:cursor + 1]
        cursor += 1
    raise AssertionError(f"unterminated JavaScript function: {name}")


def _entry_function(name, extra_globals):
    """One function lifted out of entry.py, runnable against stub globals."""
    tree = ast.parse(ENTRY, filename="entry.py")
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name == name
    ]
    assert len(selected) == 1, name
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    namespace = dict(extra_globals)
    exec(compile(module, "entry.py", "exec"), namespace)
    return namespace[name]


def _claim_gate(session_name):
    """_authorize_issue_self_claim wired to a session that resolves to a name."""
    async def account_session_record(_env, _request, _data):
        return ("name-bi", {"name": session_name}) if session_name else ("", None)

    return _entry_function("_authorize_issue_self_claim", {
        "_account_session_record": account_session_record,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "MAX_NODE_NAME": 64,
        "json_response": lambda body, status=200: (status, body),
    })


def _run_js(names, expression):
    script = "\n".join(_function(ISSUE_IO, name) for name in names)
    script += f"\nconsole.log(JSON.stringify({expression}));"
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True, text=True, capture_output=True,
    )
    return json.loads(result.stdout)


def test_browser_signs_the_same_claim_bytes_the_client_and_relay_do():
    # The browser's canonical content, executed rather than eyeballed, hashed
    # to the vector desktop/tests/test_crypto.cpp pins against the C++ signer.
    content = _run_js(
        ["webIssueEventContent"],
        'webIssueEventContent("claim", { assignee: "alice", claimState: "assigned" })',
    )
    assert content == "alice\x00assigned"
    assert hashlib.sha256(content.encode()).hexdigest() == CLAIM_CONTENT_HASH
    assert CLAIM_CONTENT_HASH in QT_CRYPTO_TEST

    # The relay canonicalizes the same two fields in the same order, and the
    # desktop signer joins them with the same NUL.
    relay = _slice(EVENTS, 'if t == "claim":', 'if t == "agent":')
    assert 'ev.get("assignee", "") or ""' in relay
    assert 'ev.get("claimState", "") or ""' in relay
    assert relay.index('"assignee"') < relay.index('"claimState"')
    assert 'return ev.assignee + nul + ev.claimState;' in ISSUE_STORE

    # The direction is inside the signed content, so an "assign" signature can
    # never be replayed as an "unassign".
    released = _run_js(
        ["webIssueEventContent"],
        'webIssueEventContent("claim", { assignee: "alice", claimState: "released" })',
    )
    assert released != content

    # ...and the signed field list the browser sends matches what it hashes.
    submit = _slice(
        ISSUE_IO,
        "async function submitWebIssueEvent(repo, number, type, fields = {})",
        "async function setWebIssueSubscription",
    )
    assert 'claim: ["assignee", "claimState"],' in submit


def test_relay_accepts_a_claim_only_from_the_account_it_names():
    types = _slice(EVENTS, "ISSUE_EVENT_TYPES = frozenset({", "})")
    assert '"claim"' in types
    # The whole shape check, anchored inside verify_issue_event rather than
    # sliced between its neighbours: which event types share the next branch is
    # not this feature's business, and pinning that made the test break when an
    # unrelated type was added beside it.
    verify = _slice(EVENTS, "async def verify_issue_event", "async def verify_discussion_event")
    assert (
        '    if event_type == "claim":\n'
        '        assignee = ev.get("assignee")\n'
        '        if not isinstance(assignee, str) or not assignee.strip():\n'
        '            return False\n'
        '        if ev.get("claimState") not in CLAIM_STATES:\n'
        '            return False\n'
    ) in verify
    assert 'CLAIM_STATES = frozenset({"assigned", "released"})' in EVENTS

    handler = _slice(
        ENTRY,
        "async def _accept_issue_submission(",
        "async def discussions_handler",
    )
    # A claim is deliberately NOT a structural event: it never goes through the
    # repository-authority gate, because it carries no authority to abuse.
    owner_set = _slice(handler, "owner_event_types = {", "if event_type in owner_event_types")
    assert '"claim"' not in owner_set
    assert 'if event_type == "claim":' in handler
    assert "await _authorize_issue_self_claim(env, data, request, event)" in handler

    # Identity comes from the account session, never from the self-asserted
    # authorName riding on the (per-browser, anonymous) signing key. What the
    # gate then does with that name is exercised below.
    gate = _slice(ENTRY, "async def _authorize_issue_self_claim", "async def _inbox_author_over_quota")
    assert "await _account_session_record(env, request, data)" in gate
    assert 'event.get("authorName"' not in gate


def test_the_relay_gate_refuses_every_claim_but_the_callers_own():
    def claim(assignee):
        return {"type": "claim", "assignee": assignee, "claimState": "assigned"}

    # The account session IS the authorization. Same person, any spelling.
    for session, named in (("alice", "alice"), ("alice", "Alice"), ("Alice", "alice")):
        ok, err = asyncio.run(_claim_gate(session)({}, {}, None, claim(named)))
        assert (ok, err) == (True, None), (session, named)

    # Somebody else's name, a blank name, a missing field, and no session at
    # all: all refused, and all with the same status the structural events use.
    for session, event in (
        ("alice", claim("bob")),
        ("alice", claim("  ")),
        ("alice", {"type": "claim", "claimState": "assigned"}),
        ("", claim("alice")),
    ):
        ok, err = asyncio.run(_claim_gate(session)({}, {}, None, event))
        assert ok is False
        assert err == (403, {"error": "not_authorized"})


def test_every_projection_folds_a_claim_as_a_delta():
    # Browser: executable, because this is the copy a reader sees immediately.
    assert _run_js(
        ["webIssueClaimApplied"],
        'webIssueClaimApplied(["maintainer"], { assignee: "alice", claimState: "assigned" })',
    ) == ["maintainer", "alice"]
    assert _run_js(
        ["webIssueClaimApplied"],
        'webIssueClaimApplied(["maintainer", "Alice"], { assignee: "alice", claimState: "assigned" })',
    ) == ["maintainer", "Alice"]
    assert _run_js(
        ["webIssueClaimApplied"],
        'webIssueClaimApplied(["maintainer", "Alice"], { assignee: "alice", claimState: "released" })',
    ) == ["maintainer"]
    assert _run_js(
        ["webIssueClaimApplied"],
        'webIssueClaimApplied(["maintainer"], { assignee: "", claimState: "assigned" })',
    ) == ["maintainer"]

    # Desktop host and Go mirror fold it the same way, so the issue file and
    # every mirror's projection converge on one list.
    assert 'else if (ev.type == "claim")\n            applyIssueClaim(issue.assignees, ev);' in ISSUE_STORE
    assert "void applyIssueClaim(QStringList &assignees, const IssueEvent &ev)" in ISSUE_STORE
    assert 'issue["assignees"] = applyIssueClaim(issue["assignees"], event)' in GO_INTAKE
    assert "func applyIssueClaim(current any, event map[string]any) []any {" in GO_INTAKE
    # Case-insensitive on all three, matching the relay's lower-cased check.
    assert "Qt::CaseInsensitive" in _slice(
        ISSUE_STORE, "void applyIssueClaim(", "QString newId()")
    assert "strings.EqualFold" in _slice(
        GO_INTAKE, "func applyIssueClaim(", "func writeIssueAttachments(")
    # The desktop event carries the two claim fields through JSON both ways.
    assert 'obj.insert("assignee", assignee);' in ISSUE_STORE
    assert 'ev.claimState = obj.value("claimState").toString();' in ISSUE_STORE
    assert "QString claimState;    // claim: assigned|released" in ISSUE_STORE_H


def test_a_claim_survives_a_mirror_that_cannot_project_it_yet():
    # The serving mirror may still be on a build that drops "claim" from its
    # projection, so the reader folds the claims recorded after the last full
    # "assignees" event over whatever list it was served. Idempotent against a
    # mirror that DID project them; stopping at the last "assignees" event keeps
    # a maintainer's later replacement authoritative.
    events = (
        '[{ type: "claim", assignee: "alice", claimState: "assigned" }]'
    )
    assert _run_js(
        ["webIssueAssigneesWithClaims", "webIssueClaimApplied"],
        f'webIssueAssigneesWithClaims([], {events})',
    ) == ["alice"]
    assert _run_js(
        ["webIssueAssigneesWithClaims", "webIssueClaimApplied"],
        f'webIssueAssigneesWithClaims(["alice"], {events})',
    ) == ["alice"]
    replaced = (
        '[{ type: "claim", assignee: "alice", claimState: "assigned" },'
        ' { type: "assignees", assignees: ["bob"] }]'
    )
    assert _run_js(
        ["webIssueAssigneesWithClaims", "webIssueClaimApplied"],
        f'webIssueAssigneesWithClaims(["bob"], {replaced})',
    ) == ["bob"]

    # The maintainer's editor is seeded from — and diffed against — that same
    # folded list, so opening and saving it does not revert a contributor.
    detail = _slice(
        REPO_UI,
        "function renderRepoRecordDetail(repo, kind, number, parsed)",
        "async function loadRepoRecordDetail",
    )
    assert "const issueAssignees = webIssueAssigneesWithClaims(" in detail
    assert 'data-repo-issue-assignees value="${escapeHtml(issueAssignees.join(", "))}"' in detail
    metadata = _slice(
        ISSUE_IO,
        "async function handleWebIssueMetadataSubmit(form)",
        "async function handleDiscussionReplySubmit",
    )
    assert "const shownAssignees = webIssueAssigneesWithClaims(" in metadata
    assert 'assignees.join(",") !== shownAssignees.join(",")' in metadata


def test_the_button_is_offered_to_contributors_not_only_maintainers():
    detail = _slice(
        REPO_UI,
        "function renderRepoIssueDetail(repo, number, parsed)",
        "function setRepoIssueRecordOpen(open)",
    )
    control = _slice(detail, "const selfAssignControl", "const assigneeValue")
    assert "data-repo-issue-claim" in control
    assert "Assign yourself" in control and "Unassign yourself" in control
    # Crucially it is NOT inside an issueCanManage branch — the whole point is
    # that a visitor with no repository role sees it.
    assert "canManage" not in control
    assert "viewerAssigned ? \"release\" : \"claim\"" in control
    # It hangs off the Assignees section, next to the names it changes.
    assert "selfAssignControl" in _slice(detail, "const assigneeValue", "const latestAgentEvent")

    # One click, through the same delegated handler as the other issue actions.
    assert '[data-repo-issue-claim]' in WIRING
    assert 'issueClaimButton.dataset.repoIssueClaim || "claim"' in WIRING
    action = _slice(
        ISSUE_IO,
        "async function handleWebIssueAction(action, target = \"\")",
        "async function handleWebIssueTitleSubmit",
    )
    assert 'if (action === "claim" || action === "release") {' in action
    assert 'assignee: String(state.session.nodeName || ""),' in action
    assert 'claimState: action === "claim" ? "assigned" : "released",' in action
    # A signed-out visitor is sent to sign in rather than shown a dead button:
    # the redirect guard runs before any action is chosen.
    assert action.index("location.href = \"/login?next=\"") < action.index('action === "claim"')
    # "not_authorized" on a claim means the session lapsed, not that the viewer
    # lacks a role, so it must not reuse the owner-only wording.
    failure = _slice(ISSUE_IO, "function webIssueActionError(error, action = \"\")", "function splitWebIssueList")
    assert 'action === "claim" || action === "release"' in failure
    assert "Sign in again" in failure
    assert 'showErrorNotice(webIssueActionError(error, action));' in ISSUE_IO


def test_the_timeline_reports_who_claimed_the_issue():
    assert 'claim: "user-check",' in REPO_UI
    description = _slice(REPO_UI, 'case "claim":', 'case "title":')
    assert '"unassigned themselves"' in description
    assert '"assigned themselves"' in description
    assert 'addActivity(ev, ev.claimState == QLatin1String("released")' in (
        ROOT.parent / "desktop/src/MainWindowIssues.cpp").read_text()
