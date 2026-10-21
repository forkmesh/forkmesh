#!/usr/bin/env python3
"""The repository Actions tab on the web (adhoc #266).

Workflows run inside a desktop; the website shows what a mirror's attested
Actions executor publishes and sends control requests back to that same node.
Wire contract: www/docs/operations/mirror-actions-control.md.

These pin the parts a refactor could silently drop: the tab is addressable on a
hard refresh, the fragment ships in the bundle, the page never claims controls
an executor did not advertise, and executor-supplied text is never markup.
"""

import re
import sys
from pathlib import Path

from _dashboard_bundle import assembled_dashboard_js

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import dashboard_bundle  # noqa: E402
import static_routes  # noqa: E402

DASHBOARD_JS = assembled_dashboard_js()
FRAGMENT = (
    ROOT / "public" / "dashboard" / "js" / "06-repo-actions.js"
).read_text(encoding="utf-8")


def test_actions_is_an_addressable_repository_tab():
    assert "actions" in static_routes.REPO_TAB_ROUTES
    assert static_routes.looks_like_repo_route("/owner/repo/actions")
    # The API stays off the document route.
    assert not static_routes.looks_like_repo_route("/api/repo/o/r/actions/runs")
    routes = re.search(r"const REPO_TAB_ROUTES = \[([^\]]*)\]", DASHBOARD_JS)
    assert routes and '"actions"' in routes.group(1)


def test_fragment_ships_in_the_bundle_in_lexical_order():
    fragments = list(dashboard_bundle.FRAGMENTS)
    assert "06-repo-actions.js" in fragments
    assert fragments == sorted(fragments)
    assert FRAGMENT in DASHBOARD_JS


def test_tab_strip_panel_and_loaders_are_wired():
    assert 'actions: { label: "Actions", icon: "workflow", count: "" }' in DASHBOARD_JS
    assert '"pulls", "discussions", "insights", "projects", "actions"' in DASHBOARD_JS
    assert 'data-dashboard-repo-tab-panel="actions"' in DASHBOARD_JS
    assert "<div data-repo-actions></div>" in DASHBOARD_JS
    # Opened by click, restored by deep link, and silenced when left.
    assert 'if (tab === "actions") void loadRepoActions(state.selectedRepo);' in DASHBOARD_JS
    assert '} else if (active === "actions") {' in DASHBOARD_JS
    assert 'if (tab !== "actions") stopRepoActionsRefresh();' in DASHBOARD_JS
    assert "if (handleRepoActionsClick(event)) return;" in DASHBOARD_JS
    assert "if (handleRepoActionsSubmit(event)) return;" in DASHBOARD_JS


def test_reads_and_controls_use_the_session_routes():
    assert '"GET", `${repoApiBase(repo)}/actions/runs`' in FRAGMENT
    assert '"POST", `${repoApiBase(repo)}/actions/control`' in FRAGMENT
    # Run ids are per executor: a control names the node whose runs are shown.
    assert "requestId, action, node: view.node, ...fields," in FRAGMENT


def test_controls_require_an_advertising_v2_executor():
    assert (
        "view.controlAvailable = payload.controlAvailable === true "
        "&& (view.schemaVersion === 2 || runner);"
    ) in FRAGMENT
    assert 'const runner = view.source === "runner";' in FRAGMENT
    assert "if (!view.controlAvailable) return \"\";" in FRAGMENT
    assert "if (!repo || !view.controlAvailable || !view.node) return;" in FRAGMENT
    # Every control the contract names has a sender.
    for action in (
        '"dispatch"', '"set-workflow-enabled"', '"set-repository-enabled"',
        '"approve"', '"reject"', '"cancel"', '"rerun"',
    ):
        assert action in FRAGMENT, action
    assert "window.confirm(" in FRAGMENT  # approval is a deliberate second step


def test_quiet_unavailable_body_is_not_reported_as_an_error():
    # orgAgentRequest throws on {"ok": false}; the relay's quiet 200 for "no
    # executor answered" must land on the explanatory empty state.
    assert 'failure?.payload?.state === "unavailable"' in FRAGMENT
    assert "No runs reported for this repository yet" in FRAGMENT


def test_executor_text_is_escaped_and_bounded():
    assert "const logText = fullLog?.full && fullLog.text ? fullLog.text : run.logTail;" in FRAGMENT
    assert "${escapeHtml(logText)}" in FRAGMENT
    assert "${escapeHtml(step.name)}" in FRAGMENT
    assert "${escapeHtml(run.workflow)}" in FRAGMENT
    assert "${escapeHtml(view.node)}" in FRAGMENT
    # No executor-controlled value is interpolated raw into markup. (Plain
    # notice/label strings are escaped where they are rendered.)
    markup = "\n".join(
        line for line in FRAGMENT.splitlines() if "class=" in line)
    raw = re.findall(r"\$\{(run|step|workflow|view)\.(\w+)\}", markup)
    assert set(raw) <= {("run", "id"), ("run", "pullNumber")}, raw
    assert "<span class=\"fm-flash-body\">${escapeHtml(view.notice.text)}</span>" in FRAGMENT
    assert "escapeHtml(view.pending.map((entry) => entry.label).join(\", \"))" in FRAGMENT
    assert "REPO_ACTIONS_WORKFLOW_PATH.test(path)" in FRAGMENT


def test_polling_is_bounded_to_the_visible_tab():
    assert "if (!repoActionsIsCurrent(repo)) return;" in FRAGMENT
    assert "if (!active && document.hidden) return;" in FRAGMENT
    assert "(document.hidden ? 30000 : 6000)" in FRAGMENT
    assert ": 60000;" in FRAGMENT


def function(name):
    start = FRAGMENT.index("function %s(" % name)
    end = FRAGMENT.index("\n  }\n", start)
    return FRAGMENT[start:end]


def test_runner_snapshots_dispatch_with_inputs_cancel_and_rerun_locally():
    sender = function("sendRepoActionsRunnerControl")
    assert '"POST", `${repoApiBase(repo)}/actions/control`, { action, ...fields }' in sender
    assert "requestId" not in sender.split("orgAgentRequest", 1)[1].split(");", 1)[0]
    assert 'if (repo && view.controlAvailable && view.source === "runner") {' in FRAGMENT
    submit = function("handleRepoActionsSubmit")
    assert '{ workflow, ref: branch, inputs }' in submit
    assert 'inputs[name] = Boolean(field.checked);' in submit
    assert 'data-repo-actions-input-required' in submit
    controls = function("renderRepoActionsRunControls")
    assert 'button("cancel"' in controls and 'button("rerun"' in controls
    assert 'run.status === "awaiting-approval" && view.source !== "runner"' in controls


def test_dispatch_inputs_render_by_type():
    field = function("renderRepoActionsInputField")
    assert 'input.type === "boolean"' in field and 'type="checkbox"' in field
    assert 'input.type === "choice"' in field and "<select" in field
    assert 'type="text"' in field and '${input.required ? " required" : ""}' in field
    assert "escapeHtml(input.description)" in field and "escapeHtml(option)" in field
    assert "escapeHtml(input.defaultValue)" in field
    normalize = function("normalizeRepoActionsInputs")
    assert '["string", "choice", "boolean"].includes(details.type)' in normalize
    assert "REPO_ACTIONS_INPUT_NAME.test(name)" in normalize
    assert 'events.includes("workflow_dispatch")' in function("normalizeRepoActionsWorkflow")
    bar = function("renderRepoActionsWorkflowBar")
    assert 'view.source === "runner" ? workflow.inputs || [] : []' in bar
    assert 'view.summaryWorkflows && view.source !== "runner"' in bar


def test_runs_show_their_event_and_the_full_log():
    for event in ("push", "schedule", "workflow_dispatch", "issues", "issue_comment"):
        assert '"%s"' % event in FRAGMENT.split("REPO_ACTIONS_EVENTS = [", 1)[1].split("]", 1)[0]
    assert "event: repoActionsRunEvent(run?.event)," in FRAGMENT
    assert 'data-repo-actions-run-event="${escapeHtml(run.event)}"' in FRAGMENT
    log = function("loadRepoActionsRunLog")
    assert '"GET", `${repoApiBase(repo)}/actions/runs/${runId}/log`' in log
    assert 'typeof payload?.log === "string"' in log


def test_actions_secrets_on_repository_and_organization_settings():
    assert 'data-actions-secrets-base="${escapeHtml(`${repoApiBase(repo)}/actions/secrets`)}"' in DASHBOARD_JS
    assert '"/api/org/" + encodeURIComponent(name) + "/actions/secrets"' in DASHBOARD_JS
    assert 'mountActionsSecrets($("[data-actions-secrets]"));' in DASHBOARD_JS
    assert "Actions secrets" in DASHBOARD_JS
    save = function("saveActionsSecret")
    assert '"PUT", `${base}/${encodeURIComponent(name)}`, { value }' in save
    assert 'valueField.value = ""' in save
    assert "ACTIONS_SECRET_MAX_BYTES" in save
    assert '"DELETE", `${base}/${encodeURIComponent(name)}`' in function("deleteActionsSecret")
    assert 'orgAgentRequest("GET", base)' in function("loadActionsSecrets")
    assert "const ACTIONS_SECRET_NAME = /^[A-Z_][A-Z0-9_]{0,63}$/;" in FRAGMENT
    rendered = function("renderActionsSecrets")
    assert "escapeHtml(secret.name)" in rendered
    assert "secret.value" not in FRAGMENT
    assert "bg-cta" not in rendered and "fm-btn-cta" not in rendered


def test_signed_out_visitors_on_an_instance_read_the_public_summary():
    loader = function("loadRepoActions")
    assert "if (!state.session?.sessionToken && !dashboardOnInstance()) {" in loader
    assert (
        'view.access = view.source === "runner" && payload.access === "public" '
        '? "public" : "member";'
    ) in loader
    assert 'view.access !== "public" && view.selectedRunId' in loader
    assert 'view.access === "public") return;' in function("loadRepoActionsRunLog")
    toggle = function("handleRepoActionsClick")
    assert 'view.source === "runner" && view.access !== "public"' in toggle


def test_public_runs_hint_at_signing_in_for_logs():
    detail = function("renderRepoActionsRunDetail")
    assert 'if (view.access === "public") return renderRepoActionsPublicDetail(run);' in detail
    public = function("renderRepoActionsPublicDetail")
    assert "Sign in to see logs" in public
    assert "escapeHtml(repoActionsSignInHref())" in public
    assert "issuePersonNameHtml(run.actor," in public
    assert "logTail" not in public and "loadRepoActionsRunLog" not in public
    assert "/login?next=${encodeURIComponent(location.pathname + location.search)}" in function(
        "repoActionsSignInHref")
    assert "actor: repoActionsCleanLine(run?.actor, 64)," in FRAGMENT
    assert '${issuePersonNameHtml(run.actor, "")}' in function("renderRepoActionsRuns")
    assert "Sign in to see runs" in FRAGMENT


def test_the_instance_runner_reads_as_this_server():
    source = (ROOT / "public" / "dashboard" / "js" / "06-repo-actions.js").read_text(encoding="utf-8")
    assert 'view.local = view.source === "runner" && payload.local === true;' in source
    assert "Runs on this server" in source
    assert "escapeHtml(view.host)" in source
    assert "Runs on this server" in DASHBOARD_JS


def test_a_run_link_opens_that_run():
    assert static_routes.looks_like_repo_route("/owner/repo/actions/runs/41")
    assert 'routeKind === "actions" && routeParts[3] === "runs"' in DASHBOARD_JS
    assert "`${repoLinkUrl(repo)}/actions/runs/${actionsRunId}`" in DASHBOARD_JS
    loader = FRAGMENT.split("async function loadRepoActions(", 1)[1]
    loader = loader.split("\n  async function ", 1)[0]
    assert "view.selectedRunId = initialRun.runId;" in loader
    assert loader.index("Object.assign(view, {") < loader.index(
        "view.selectedRunId = initialRun.runId;")
