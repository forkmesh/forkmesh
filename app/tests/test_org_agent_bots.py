"""Organization-scoped Claude/Codex agent security and World contracts."""

import ast
import asyncio
from pathlib import Path
from world_scene_source import world_scene_source


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

# MainWindowAgents.cpp was split into feature files; read them in the original
# source order so section slices that span functions still line up.
MAIN_WINDOW_AGENT_SOURCES = (
    "MainWindowAgents.cpp",
    "MainWindowAgentsInternal.h",
    "MainWindowAgentSync.cpp",
    "MainWindowAgentUsage.cpp",
    "MainWindowAgentList.cpp",
    "MainWindowAgentDetail.cpp",
    "MainWindowAgentLaunch.cpp",
    "MainWindowAgentCliSession.cpp",
    "MainWindowAgentEvents.cpp",
    "MainWindowAgentTranscript.cpp",
    "MainWindowAgentLanding.cpp",
    "MainWindowAgentActions.cpp",
)

ROOT = Path(__file__).resolve().parents[1]
# The World and Office route bodies moved into sibling modules so Cloudflare's
# Python startup validation never parses them. These contracts still span the
# whole route family, so their source is read alongside entry.py.
WORLD_ROUTE_SOURCE = "".join(
    (Path(__file__).resolve().parents[2] / "world" / "src" / name).read_text(
        encoding="utf-8")
    for name in ("world_routes.py", "world_office_routes.py")
)
ENTRY = (
    (ROOT / "src/entry.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ROOT / "src/route_handlers.py").read_text(encoding="utf-8")
    + WORLD_ROUTE_SOURCE
)
MIRRORS = (ROOT / "src/mirrors.py").read_text(encoding="utf-8")
SCHEMA = (ROOT / "src/schema.py").read_text(encoding="utf-8")
URLS = (ROOT / "src/urls.py").read_text(encoding="utf-8")
DASHBOARD = (
    ROOT / "public/dashboard/js/06-repo-content.js"
).read_text(encoding="utf-8")
CHAT = (ROOT / "public/dashboard-chat.js").read_text(encoding="utf-8")
WORLD = (ROOT.parent / "world/public/world/world.js").read_text(encoding="utf-8")
SCENE = world_scene_source()
QT = "".join(
    (ROOT.parent / "desktop/src" / name).read_text(encoding="utf-8")
    for name in MAIN_WINDOW_AGENT_SOURCES
)
QT_REPOS = "".join(
    (ROOT.parent / "desktop/src" / name).read_text(encoding="utf-8")
    for name in MAIN_WINDOW_REPOS_SOURCES
)


def _load_route_function(name, extra):
    path = ROOT / "src" / "route_handlers.py"
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name == name
    ]
    assert len(selected) == 1
    namespace = dict(extra)
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[])), str(path), "exec"),
         namespace)
    return namespace[name]


def test_org_agents_are_separate_member_scoped_encrypted_records():
    assert "CREATE TABLE IF NOT EXISTS org_agent_sessions" in SCHEMA
    assert "CREATE TABLE IF NOT EXISTS org_agent_jobs" in SCHEMA
    assert "_org_agent_member_context" in ENTRY
    # Deny-by-default matrix: reads for insiders, queueing for writers.
    assert '"organization.agents.read" if method_name(request) == "GET"' in ENTRY
    assert 'else "organization.agents.queue"' in ENTRY
    assert "repository_not_linked" in ENTRY
    assert "_request_same_origin(request)" in ENTRY
    assert "await encrypt_row(env, record)" in ENTRY
    assert "await encrypt_row(env, job)" in ENTRY
    assert "privacyBoundary" in ENTRY
    assert '"engineering-team-encrypted-at-rest"' in ENTRY
    assert "engineering_team_required" in ENTRY
    # The gate resolves Engineering through the same alias table the elevator
    # uses, so it is still team-scoped without hardcoding one team slug.
    assert "_org_floor_team_rows(" in ENTRY
    assert '"engineering", member_bi=str(account_bi)' in ENTRY
    assert '"canQueueAgent": True' in ENTRY
    assert '"canViewAgentSessions": True' in ENTRY
    assert "if not is_engineering:" in ENTRY
    assert '"accessBasis": "engineering_team"' in ENTRY
    assert "await _contribution_run_batch(env, [" in ENTRY
    assert ENTRY.count("context, error = await _org_agent_member_context(") >= 1


def test_offline_owner_desktop_and_fresh_mirror_routes_remain_fail_closed():
    assert "agent_provider_target_decision" in ENTRY
    assert "agent_provider_mirror_candidates" in MIRRORS
    assert "10 * 60 * 1000" in ENTRY
    assert "context[\"nodeOwner\"]" in ENTRY
    assert 'org_owner=context["role"] == "owner"' in ENTRY
    assert "source_owned=await _account_owns_node(" in ENTRY
    # Provider availability stays private desktop/runtime state; the retired
    # desktop catalog publisher must not broadcast it.
    assert '"agentProviders"' not in QT_REPOS
    assert "agentProviders" in MIRRORS
    assert "waiting_for_desktop" in MIRRORS
    assert "provider_not_advertised" in MIRRORS
    assert "target_not_owned" in MIRRORS
    assert 'source_runtime != "desktop"' in MIRRORS
    assert "platform_admin=await _is_admin(env, context[\"actor\"])" in ENTRY
    assert "ORG_AGENT_MAX_PROMPT = 8000" in ENTRY
    assert "ORG_AGENT_JOB_LEASE_MS = 2 * 60 * 1000" in ENTRY
    assert "ORDER BY queued.id LIMIT 1" in ENTRY
    assert "status='leased'" in ENTRY
    assert "lease_not_found" in ENTRY
    assert "ORG_AGENT_BOTS_RE" in URLS
    assert "REPO_ORG_AGENT_JOB_RESULT_RE" in URLS


def test_agent_jobs_dispatch_without_a_haiku_preflight():
    # Website agent jobs no longer pass through a model safety preflight on the
    # desktop, the mirror node, or the relay.
    assert "haikuGate" not in QT
    assert "orgAgentJobWaivesSafetyCheck" not in QT
    assert "runOrgAgentSafetyCheck" not in QT
    assert "--model 'haiku' --max-turns 1" not in QT
    assert 'startApprovedOrgAgentJob(repo, job, QStringLiteral("Prompt dispatched."));' in QT
    assert '"securityVerdict"' in QT
    assert 'verdict not in ("approved", "rejected")' in ENTRY
    assert 'verdict != "approved" and run_status != "rejected"' in ENTRY
    assert "claude-haiku-tool-free-fail-closed" not in ENTRY
    assert 'Prompt rejected by the Haiku security preflight.' not in ENTRY


def test_every_agent_request_queues_straight_to_its_node():
    assert '{"state": "not_required", "model": "", "reason": ""}' in ENTRY
    assert 'return (record, {"required": False}, "queued")' in ENTRY
    assert "security_record, security_check, queue_status = (" in ENTRY
    # The definition, plus both queue paths: a new session and a follow-up.
    assert ENTRY.count("_org_agent_security_envelope(context, owner_scope)") == 3
    # Owner envelopes keep the waiver fields so nodes that predate the change
    # still accept owner-queued jobs while they update.
    assert '"waiver": "repository_owner"' in ENTRY
    assert '"waivedFor": context["actor"]' in ENTRY
    assert '"securityGate": "not-required"' in ENTRY
    assert '"state": "not_required" if verdict == "approved" else verdict' in ENTRY


def test_agents_ui_chat_and_world_expose_both_fixed_bot_identities():
    assert 'value: "claude-code", label: "Claude Code"' in DASHBOARD
    assert 'value: "codex", label: "Codex"' in DASHBOARD
    assert "data-org-agent-followup" in DASHBOARD
    assert "CLAUDE_MENTION_RE" in CHAT
    assert "CODEX_MENTION_RE" in CHAT
    assert "maybeAskOrgAgent" in CHAT
    assert '"claude"' in SCENE
    assert '"codex"' in SCENE
    assert "exciteAgentBot" in SCENE
    assert "onAgentBotChat" in SCENE
    assert "refreshOrgAgentBots" in WORLD
    assert "reconcileSessionAgentBots" in SCENE
    assert "createAgentBotAvatar" in SCENE
    assert "removeAgentBotAvatar" in SCENE
    assert "repositoryAgentSession = session" in SCENE
    assert "spawnedAt: performance.now()" in SCENE
    assert 'status === "running"' in SCENE
    assert "mirrorNodeAgentSessionsHTML" in WORLD
    assert "wireMirrorNodeAgentWorkspace" in WORLD
    assert "data-world-agent-followup" in WORLD
    assert "Organization owners can save" in WORLD


def test_agents_page_can_choose_model_and_node_and_delete_sessions():
    for marker in (
        '"canDeleteAgentSessions": True',
        'targets = await _org_agent_session_targets(env, context)',
        'if method not in ("GET", "POST", "DELETE")',
        '"DELETE FROM org_agent_jobs WHERE session_id=? "',
        '"DELETE FROM org_agent_sessions WHERE session_id=? "',
        'audit_scope + ".agent_delete"',
        '"session_delete_failed"',
    ):
        assert marker in ENTRY
    for marker in (
        "data-org-agent-provider",
        "data-org-agent-model",
        "data-org-agent-target",
        "data-org-agent-delete",
        "payload?.canDeleteAgentSessions === true",
        '{ provider, model, targetNode, prompt, taskKey, createPr: false }',
        '"DELETE",',
    ):
        assert marker in DASHBOARD


def test_an_agent_row_opens_a_session_detail_page_like_the_desktop():
    # adhoc #2117: the Agents tab used to print every session's metadata and
    # whole transcript inline. A row is now a summary that opens one session,
    # carrying the fields the desktop's own session table shows.
    for marker in (
        "function renderOrgAgentSessionDetail(session, repo)",
        "function openOrgAgentDetail(repo, sessionId)",
        "function closeOrgAgentDetail(repo)",
        'data-org-agent-open="${escapeHtml(facts.id)}"',
        'data-org-agent-detail="${escapeHtml(facts.id)}"',
        "data-org-agent-back",
        "data-org-agent-session-refresh",
        "data-org-agent-transcript",
        'orgAgentMetaRow("Mode", escapeHtml(facts.permissionLabel))',
        'orgAgentMetaRow("Tokens", orgAgentTokenSummary(info))',
        'orgAgentMetaRow("Context", orgAgentContextSummary(info))',
        "ORG_AGENT_TRANSCRIPT_ROLES",
        "renderOrgAgentTranscriptEntry",
    ):
        assert marker in DASHBOARD, marker
    # The open session only keeps the pane while its row is one the list would
    # draw, so a deleted session - or one the header search now excludes -
    # cannot strand the reader on it.
    assert ": visible.find((item) => String(item?.id || \"\") === String(selectedId)))" in DASHBOARD
    assert "const selectionDropped = selectedId != null && !selected;" in DASHBOARD
    assert "if (selectionDropped) state.agentsView.selectedAgentId = null;" in DASHBOARD
    assert "? renderOrgAgentSessionDetail(selected, repo)" in DASHBOARD
    # Refresh stays explicit: nothing in the session renderers or in the
    # open/close pair starts a timer behind the reader's back. Scoped to that
    # region so an unrelated timer elsewhere in the file cannot fail this.
    region = DASHBOARD[
        DASHBOARD.index("function orgAgentProviderLabel")
        : DASHBOARD.index("async function loadRepoAgents")
    ]
    assert "setInterval" not in region
    assert "setTimeout" not in region


def test_agents_page_is_the_desktop_two_pane_workspace():
    # adhoc #2120: the Agents tab was one column - a start form in the header,
    # then rows, then (once a row was clicked) a detail page *instead of* the
    # rows. It is now the desktop app's own shape: the session list on the
    # left, the open session on the right, and the prompt box along the bottom
    # of that pane.
    for marker in (
        "data-org-agent-sessions",
        "data-org-agent-detail-pane",
        "function renderOrgAgentDetailPane(repo = state.selectedRepo)",
        "function renderOrgAgentStartPane()",
        "function wireOrgAgentStartForm(repo, startForm)",
        # The two columns, and each one scrolling inside its own pane.
        'lg:grid-cols-[20rem_minmax(0,1fr)]',
        'data-org-agent-sessions class="max-h-72 min-h-0 min-w-0 overflow-y-auto',
        'data-org-agent-detail-pane class="flex min-h-0 min-w-0 flex-col"',
    ):
        assert marker in DASHBOARD, marker
    # The list is drawn whether or not a session is open, and the open row is
    # marked instead of replacing the rows.
    assert "list.innerHTML = desktopHtml + (visible.length" in DASHBOARD
    assert "data-org-agent-session-selected" in DASHBOARD
    # Both composers hang off the bottom edge of the right-hand pane: the
    # follow-up for an open session, the start form when none is open.
    detail = DASHBOARD[
        DASHBOARD.index("function renderOrgAgentSessionDetail")
        : DASHBOARD.index("function renderOrgAgentStartPane")
    ]
    assert detail.index('<div data-org-agent-detail-foot class="border-t border-border bg-secondary/20 px-4 py-3">') > \
        detail.index("data-org-agent-transcript")
    start_pane = DASHBOARD[
        DASHBOARD.index("function renderOrgAgentStartPane")
        : DASHBOARD.index("const ORG_AGENT_ERROR_MESSAGES")
    ]
    assert '<div class="border-t border-border bg-secondary/20 px-4 py-3">' in start_pane
    assert "data-org-agent-start " in start_pane
    # The start composer is redrawn every time the pane swaps back to it, so
    # its provider/model/node choices have to survive the panel read.
    assert "state.agentsView.startPayload = payload;" in DASHBOARD
    assert "wireOrgAgentStartForm(repo, pane.querySelector(\"[data-org-agent-start]\"));" in DASHBOARD


def test_finished_queue_statuses_are_not_toned_as_still_running():
    # "completed" and "rejected" are the relay's queue words for outcomes the
    # desktop calls "success" and "failed"; toning them as pending made a run
    # that had already finished read as in flight.
    assert 'if (value === "success" || value === "completed") return "text-success";' in DASHBOARD
    assert '["failed", "stopped", "rejected"].includes(value)' in DASHBOARD


def test_delete_agent_session_removes_scoped_job_and_session_then_audits():
    batches = []
    audits = []

    async def member_context(*_args, **_kwargs):
        return ({
            "org": "acme",
            "orgBi": "org-bi",
            "repo": "project",
            "actor": "engineer",
        }, None)

    async def session_row(*_args):
        return ({
            "session_id": "abc123",
            "provider": "codex",
            "status": "security_pending",
            "target_node": "mirror-one",
        }, {"scope": "organization"})

    async def batch(_env, statements):
        batches.append(statements)

    async def audit(*args):
        audits.append(args)

    handler = _load_route_function("org_agent_bot_handler", {
        "ensure_schema": lambda _env: asyncio.sleep(0),
        "method_name": lambda _request: "DELETE",
        "_request_origin_allowed": lambda _env, _request: True,
        "_owner_agent_signed_triple": lambda _request: False,
        "_org_agent_member_context": member_context,
        "_org_agent_session_row": session_row,
        "_contribution_run_batch": batch,
        "_audit_sensitive_action": audit,
        "clean_string": lambda value, maximum: str(value or "")[:maximum],
        "json_response": lambda payload, **_kwargs: payload,
        "_org_agent_error": lambda code, *_args, **_kwargs: {"error": code},
    })
    response = asyncio.run(handler(
        object(), object(), "acme", "project", "abc123"))
    assert response == {
        "ok": True,
        "deleted": True,
        "sessionId": "abc123",
        "message": "Agent session deleted.",
    }
    assert batches == [[
        (
            "DELETE FROM org_agent_attachments WHERE session_id=? "
            "AND org_bi=? AND repo=?",
            ("abc123", "org-bi", "project"),
        ),
        (
            "DELETE FROM org_agent_jobs WHERE session_id=? "
            "AND org_bi=? AND repo=?",
            ("abc123", "org-bi", "project"),
        ),
        (
            "DELETE FROM org_agent_sessions WHERE session_id=? "
            "AND org_bi=? AND repo=?",
            ("abc123", "org-bi", "project"),
        ),
    ]]
    assert audits[0][2] == "organization.agent_delete"
    assert audits[0][4] == "acme/project/abc123"


def test_agent_frontends_use_scoped_queue_capabilities_and_server_messages():
    assert "orgAgentEngineeringAccess = false" in CHAT
    assert "orgAgentQueueAccess = false" in CHAT
    assert "await loadOrgAgentChatAccess()" in CHAT
    assert "data?.canQueueAgent === true" in CHAT
    assert "payload?.canQueueAgent === true" in DASHBOARD
    assert "payload?.canQueueAgent === true" in WORLD
    assert "payload?.canViewAgentSessions === true" in DASHBOARD
    assert "payload?.canViewAgentSessions === true" in WORLD
    assert "String(details.message || \"\").trim()" in CHAT
    assert "String(details.message || \"\").trim()" in DASHBOARD
    assert "String(details.message || \"\").trim()" in WORLD
    assert "error.payload = details" in CHAT
    assert "error.payload = details" in DASHBOARD
    assert "error.payload = details" in WORLD
    assert "orgAgentAccessLoaded || !orgAgentEngineeringAccess" not in CHAT
    assert "no_eligible_headless_mirror" not in CHAT
    assert "no_eligible_headless_mirror" not in DASHBOARD
    assert "no_eligible_headless_mirror" not in WORLD
    assert "I could not queue that task." not in CHAT
    assert "Saved for ${target}" in CHAT
    assert "Saved for ${target}" in DASHBOARD
    assert "Saved for ${target}" in WORLD


def test_agent_chat_never_enters_the_shared_room_and_visibility_fails_closed():
    assert "orgAgentIdentity(entry.sender, entry.senderId)" in CHAT
    assert "if (!orgAgentIdentity(plain.sender, plain.senderId)) send(plain)" in CHAT
    assert "setAgentBotAccess" in SCENE
    assert "let agentBotAccessAllowed = false" in SCENE
    assert "avatar.visible = false" in SCENE
    assert "this.world?.setAgentBotAccess?.(false)" in WORLD
    assert 'this.orgAgentAccess?.state !== "allowed"' in WORLD
    assert 'if (!agentBotAccessAllowed) return false;' in SCENE
    assert "record.state === \"open\" && agentBotAccessAllowed" in SCENE


def test_qt_reports_full_session_and_runtime_availability_to_world():
    for marker in (
        "localCliAvailability",
        'providerAvailability\n                        .value(QStringLiteral("message")).toString()',
        'QStringLiteral("agentInfo")',
        'QStringLiteral("availability")',
        'QStringLiteral("contextWindow")',
        'QStringLiteral("costUsd")',
        'QStringLiteral("lastError")',
    ):
        assert marker in QT
    assert "_org_agent_info_projection" in ENTRY
    assert "_org_agent_availability_projection" in ENTRY
    assert '"agentInfo": agent_info' in ENTRY


def test_world_explains_stalled_agent_jobs_and_bot_clicks_open_full_status():
    assert '"code": "mirror_not_claiming_jobs"' in ENTRY
    assert '"diagnostic": diagnostic' in ENTRY
    assert "session?.diagnostic" in SCENE
    assert 'diagnostic.level || "").toLowerCase() === "attention"' in SCENE
    assert "openAgentBotDetail(name, {" in WORLD
    assert "ORGANIZATION AGENT / SESSION STATUS" in WORLD
    assert "provider," in WORLD
    assert "allNodes: true" in WORLD
    assert "data-world-agent-open-chat" in WORLD


def test_qt_accepts_supported_headless_credential_sources_without_relaying_secrets():
    for marker in (
        "ANTHROPIC_API_KEY",
        "ANTHROPIC_AUTH_TOKEN",
        "CLAUDE_CODE_OAUTH_TOKEN",
        "credentialSource",
    ):
        assert marker in QT
    assert '"credentialSource": clean_string' in ENTRY


def test_issue_agent_model_choice_is_allowlisted_and_bound_to_the_qt_issue():
    for marker in (
        '"haiku": "claude-haiku-4-5"',
        '"sonnet": "claude-sonnet-5"',
        '"opus": "claude-opus-5-5"',
        '"opus-5": "claude-opus-5"',
        '"fable": "claude-fable-5"',
        '"sol": "gpt-5.6-sol"',
        '"luna": "gpt-5.6-luna"',
        '"terra": "gpt-5.6-terra"',
        '"invalid_model"',
        '"invalid_issue_task_key"',
        '"issueNumber": issue_number',
        '"model": model or ""',
    ):
        assert marker in ENTRY
    assert "allowedWebsiteModels" in QT
    assert "startAgentForIssue(" in QT
    assert "requestedModel" in QT


def test_only_successful_codex_results_complete_a_valid_tracked_board_key():
    assert 'str(row.get("provider") or "") == "codex"' in ENTRY
    assert 'run_status == "completed"' in ENTRY
    assert "completed_by_bi=excluded.completed_by_bi" in ENTRY
    assert '"completedKeys"' in (
        ROOT.parent / "world" / "src" / "world_build_board.py"
    ).read_text(encoding="utf-8")
    assert "completeAgentTask" in WORLD
    assert "Completed ${taskLabel}. Moving it to Done." in SCENE


def test_a_general_bot_request_picks_whichever_runtime_has_a_mirror():
    for marker in (
        'ORG_AGENT_GENERAL_PROVIDERS = ("agent", "bot", "auto")',
        "general_bot = provider in ORG_AGENT_GENERAL_PROVIDERS",
        'if not target_decision.get("ok") and general_bot:',
        "for candidate in ORG_AGENT_PROVIDERS[1:]:",
        "async def cancel_agent_session(self, org_bi, session_id):",
        "UPDATE org_agent_jobs SET status='cancelled'",
        "UPDATE org_agent_sessions SET status='cancelled'",
        "AND assignee_kind IN ('agent','bot','claude','codex')",
    ):
        assert marker in ENTRY


def _session_projection():
    entry_path = ROOT / "src" / "entry.py"
    tree = ast.parse(entry_path.read_text(encoding="utf-8"))
    availability = [
        node for node in tree.body
        if isinstance(node, ast.FunctionDef)
        and node.name == "_org_agent_availability_projection"
    ]
    helpers = {
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "ORG_AGENT_MAX_HISTORY": 80,
        "Date": type("Date", (), {"now": staticmethod(lambda: 1_000_000)}),
        "_org_agent_info_projection": lambda value: dict(value or {}),
        "MAX_NODE_NAME": 64,
    }
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=availability, type_ignores=[])), str(entry_path),
        "exec"), helpers)
    helpers["_org_agent_live_projection"] = _load_route_function(
        "_org_agent_live_projection",
        {"clean_string": helpers["clean_string"],
         "MAX_AGENT_TRANSCRIPT": 16000})
    return _load_route_function("_org_agent_session_projection", helpers)


def test_unreported_or_working_runtime_is_not_flagged_missing():
    project = _session_projection()
    row = {"session_id": "s1", "status": "queued", "updated_at": 999_000}
    # A job no node has reported on yet has no availability to judge.
    fresh = project(row, {})
    assert fresh["availability"] == {}
    assert fresh["diagnostic"]["code"] == "queued"
    assert fresh["displayStatus"] == "queued"
    # A node that is running the session evidently has the provider CLI.
    missing = {"binaryFound": True, "loginState": "missing",
               "message": "Claude Code login was not found on this mirror."}
    running = project(dict(row, status="running"),
                      {"availability": missing, "localAgentId": 4})
    assert running["diagnostic"]["code"] == "running"
    assert running["displayStatus"] == "running"
    # A queued job the node refused still names the node's own reason.
    refused = project(row, {"availability": missing})
    assert refused["diagnostic"]["code"] == "provider_credential_missing"
    assert refused["diagnostic"]["message"] == missing["message"]


def test_a_session_sent_to_a_runner_is_marked_on_runner():
    project = _session_projection()
    row = {"session_id": "s1", "status": "queued", "target_node": "fmnext-n1"}
    pool = project(row, {"runnerNode": "fmnext-r1"})
    assert pool["targetNode"] == "fmnext-r1" and pool["onRunner"] is True
    unnamed = project(row, {"runnerNode": "fmnext-n1", "route": "runner_pool"})
    assert unnamed["onRunner"] is True
    desktop = project(dict(row, target_node="desk"), {"runnerNode": "desk"})
    assert desktop["onRunner"] is False
    assert project(row, {})["onRunner"] is False
    assert '"route": clean_string(target_decision.get("route"), 32),' in ENTRY


def test_a_failed_session_shows_its_error_once():
    # 2026-09-24: the relay builds a failed run's diagnostic from lastError,
    # and the detail drew both - the same text as an amber note and a red
    # error box. The diagnostic box is skipped when it only repeats lastError.
    detail = DASHBOARD[
        DASHBOARD.index("function renderOrgAgentSessionDetail")
        : DASHBOARD.index("function renderOrgAgentStartPane")
    ]
    assert "const showDiagnostic = Boolean(diagnosticText) && diagnosticText !== lastError;" in detail
    assert "${showDiagnostic ? `" in detail
    assert "${diagnostic.message ? `" not in detail


def test_a_session_that_failed_on_a_provider_login_links_to_runner_sign_in():
    # 2026-09-27: "Claude Code exited with status 1: Failed to authenticate:
    # OAuth session expired and could not be refreshed" left the reader with
    # no way to re-auth the runner from the Agents page.
    helpers = DASHBOARD[
        DASHBOARD.index("const ORG_AGENT_SIGN_IN_SIGNS")
        : DASHBOARD.index("function orgAgentSessionFacts")
    ]
    for sign in ('"oauth session expired"', '"failed to authenticate"',
                 '"please run /login"', '"login was not found"',
                 '"authentication_error"'):
        assert sign in helpers, sign
    assert 'diagnostic?.code === "provider_credential_missing"' in helpers
    assert 'if (session?.provider === "local-llama") return false;' in helpers
    assert '["failed", "rejected"].includes(String(session?.status || ""))' in helpers
    assert 'params.set("runner", runner)' in helpers
    assert 'session?.provider === "codex" ? "codex" : "claude-code"' in helpers
    assert "return `/dashboard/runners?${params}`;" in helpers
    assert 'href="${escapeHtml(orgAgentRunnerSignInUrl(session))}"' in helpers
    detail = DASHBOARD[
        DASHBOARD.index("function renderOrgAgentSessionDetail")
        : DASHBOARD.index("function renderOrgAgentStartPane")
    ]
    assert "orgAgentNeedsRunnerSignIn(session, lastError, diagnostic)" in detail
    assert "${signInLink}" in detail
    assert '${lastError ? "" : signInLink}' in detail


def test_an_open_session_hides_the_list_on_every_screen():
    assert "<div data-org-agent-layout class=" in DASHBOARD
    pane = DASHBOARD[DASHBOARD.index("function renderOrgAgentDetailPane("):]
    pane = pane[:pane.index("\n  }\n")]
    assert "syncOrgAgentLayout(pane);" in pane
    sync = DASHBOARD[DASHBOARD.index("function syncOrgAgentLayout(pane)"):]
    sync = sync[:sync.index("\n  }\n")]
    assert '"[data-org-agent-detail], [data-desktop-agent-detail]"' in sync
    assert 'classList.toggle("lg:grid-cols-[20rem_minmax(0,1fr)]", !open)' in sync
    assert '"[data-org-agent-sessions]")?.classList.toggle("hidden", open)' in sync
