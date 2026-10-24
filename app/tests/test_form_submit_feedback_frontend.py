"""Issue #687: submit spinner and Ctrl/Cmd+Enter form submission."""

from pathlib import Path

from _dashboard_bundle import assembled_dashboard_js, built_dashboard_js


ROOT = Path(__file__).resolve().parents[1]
HELPERS = (ROOT / "public/dashboard/js/02-helpers.js").read_text()
COMPOSE = (ROOT / "public/dashboard/js/07-repo-compose-branch.js").read_text()
WIRING = (ROOT / "public/dashboard/js/08-repo-detail-network.js").read_text()


def _slice(source, start, end):
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


def test_submit_busy_helper_shows_spinner_and_restores_button():
    helper = _slice(HELPERS, "function setSubmitBusy(", "function formatCount(")
    assert "button.disabled = true" in helper
    assert "loadingHtml(escapeHtml(label))" in helper
    assert "button.innerHTML = button._idleHtml" in helper
    assert "button.disabled = false" in helper


def test_issue_and_pull_compose_show_spinner_while_submitting():
    issue = _slice(COMPOSE, "async function handleIssueComposeSubmit(",
                   "// --- CSV issue import")
    assert 'setSubmitBusy(submit, true, "Submitting issue…")' in issue
    # Sent, held for a moderator's review, or failed.
    assert issue.count("setSubmitBusy(submit, false)") == 3
    assert "submit.disabled" not in issue

    # The failure sentences themselves moved out to webPullSubmitErrorText
    # (web #49), so the slice ends at the declaration after the handler.
    pull = _slice(COMPOSE, "async function handlePullComposeSubmit(",
                  "const REPO_COMMITS_FETCH_TIMEOUT_MS")
    assert 'setSubmitBusy(submit, true, "Creating pull request…")' in pull
    assert pull.count("setSubmitBusy(submit, false)") == 2
    assert "submit.disabled" not in pull


def test_ctrl_or_cmd_enter_submits_through_primary_button():
    handler = _slice(WIRING, "// Ctrl/Cmd+Enter submits the form",
                     'if (!typingTarget && event.key === "/")')
    assert "event.metaKey || event.ctrlKey" in handler
    assert "!event.isComposing" in handler
    assert "!event.defaultPrevented" in handler
    assert 'button.matches(".bg-cta, .fm-btn-cta")' in handler
    assert "submitters.length === 1" in handler
    assert "if (!submitter.disabled) event.target.form.requestSubmit(submitter)" in handler


def test_built_dashboard_bundle_is_current():
    built = (ROOT / "public/dashboard.js").read_text()
    assert built == built_dashboard_js()
