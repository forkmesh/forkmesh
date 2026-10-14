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
    assert '"pulls", "actions", "discussions"' in DASHBOARD_JS
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
        "&& view.schemaVersion === 2;"
    ) in FRAGMENT
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
    assert "${escapeHtml(run.logTail)}" in FRAGMENT
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
