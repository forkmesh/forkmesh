from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
JS = ROOT / "public" / "dashboard" / "js"
CONTENT = (JS / "06-repo-content.js").read_text(encoding="utf-8")
ACTIONS = (JS / "06-repo-actions.js").read_text(encoding="utf-8")
NETWORK = (JS / "08-repo-detail-network.js").read_text(encoding="utf-8")
SHELL = (ROOT / "public" / "dashboard" / "shell.html").read_text(encoding="utf-8")
BUNDLE = (ROOT / "public" / "dashboard.js").read_text(encoding="utf-8")


def function(source, name):
    start = source.index("function " + name + "(")
    return source[start:source.index("\n  }\n", start)]


def test_the_pull_page_reads_runner_checks_for_its_own_pull():
    loader = function(CONTENT, "loadRepoPullRunnerChecks")
    assert '`${repoApiBase(repo)}/pulls/${number}/checks`' in loader
    assert "canApprove: payload.canApprove === true" in loader
    assert "reportedHead === head" in loader
    assert "if (dashboardOnInstance())" in function(CONTENT, "loadRepoPullChecks")


def test_only_maintainers_get_approve_and_trust_buttons():
    approval = function(CONTENT, "renderRepoPullCheckApproval")
    assert approval.index("checks.canApprove") < approval.index("data-repo-pull-check-approve")
    assert 'approval.reason === "author" && approval.author' in approval
    assert "Always run for @" in approval
    text = function(CONTENT, "repoPullCheckApprovalText")
    assert "you do not need to do anything" in text
    assert "without secrets or write access" in text


def test_decisions_post_the_reviewed_head_or_the_author():
    handler = function(CONTENT, "handleRepoPullCheckDecision")
    assert 'action === "approve" ? { action, head: value } : { action, account: value }' in handler
    assert "window.confirm" in handler
    assert "data-repo-pull-check-approve], [data-repo-pull-check-trust], [data-repo-pull-check-untrust]" in NETWORK


def test_merge_box_lists_each_check_with_status_duration_and_log():
    assert "row.checkList ? renderRepoPullCheckList(repo, row.checkList)" in CONTENT
    item = function(CONTENT, "renderRepoPullCheckList")
    for marker in ("fm-check-chip", "repoPullCheckDuration(run)", "repoPullCheckRunUrl(repo, run)"):
        assert marker in item
    assert "/actions/runs/${run.id}" in function(CONTENT, "repoPullCheckRunUrl")
    for rule in (".fm-check-item", ".fm-check-chip[data-state=\"failed\"]", ".fm-check-approval", ".fm-runner-card"):
        assert rule in SHELL


def test_actions_tab_knows_pull_requests_and_forks():
    assert '"push", "pull_request"' in ACTIONS
    assert 'if (run.status === "awaiting-approval") {' in function(ACTIONS, "renderRepoActionsRunControls")
    fork = function(ACTIONS, "renderRepoActionsForkNote")
    assert "Workflows live in the parent repository" in fork
    assert "Checks for your pull request run there and appear on the pull request." in fork
    card = function(ACTIONS, "renderRepoActionsExecutor")
    assert "awaiting approval" in card and "data-repo-actions-runner-card" in card
    assert "loadRepoPullRunnerChecks" in BUNDLE and "renderRepoActionsForkNote" in BUNDLE
