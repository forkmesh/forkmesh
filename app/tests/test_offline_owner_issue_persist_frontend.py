#!/usr/bin/env python3
"""Issue submission goes directly to an eligible mirror for materialization.

Issue #379 once kept a separate, numberless "pending issue" store that a mirror
issue retired by matching its *title*. That is gone: the relay reserves the
durable number and answers the POST with it, so a submission is a numbered
record from the start and reconciles by number (issue #609). What the browser
keeps is that numbered projection, not a placeholder — see
test_web_issue_visible_immediately.py for its behaviour.
"""

from pathlib import Path

from _dashboard_bundle import assembled_dashboard_js


DASHBOARD_JS = assembled_dashboard_js()


def _slice(text, start_marker, end_marker):
    start = text.index(start_marker)
    end = text.index(end_marker, start)
    return text[start:end]


def test_submit_uses_eligible_mirror_delivery_not_owner_persistence():
    body = _slice(
        DASHBOARD_JS,
        "async function handleIssueComposeSubmit(repo, form)",
        "// --- CSV issue import",
    )
    assert "submitWebIssue(" in body
    assert 'rememberOptimisticRepoRecord(repo, "issues", number, {' in body
    assert 'navigateHistory(repoRecordUrl(repo, "issues", number' in body
    assert "savePendingIssue" not in body
    assert "maintainer's inbox" not in body


def test_load_reconciles_by_number_against_the_mirrors_own_listing():
    load = _slice(
        DASHBOARD_JS,
        "async function loadRepoIssues(repo)",
        "async function loadRepoCollection(",
    )
    # The mirror's committed issues are still the list; this browser's own
    # accepted-but-uncommitted submissions ride on top until it carries them.
    assert (
        "state.issuesView.items = withOptimisticIssues(repo, items, servedNumbers);"
    ) in load
    # Retirement is keyed on the number the relay reserved, never on a title:
    # the served set is the mirror's own folder listing, open and closed.
    assert "reconcilePendingIssues" not in load
    assert (
        "const servedNumbers = new Set([\n"
        "        ...pathByNumber.keys(), ...closedPaths.keys(),\n"
        "      ]);"
    ) in load
