#!/usr/bin/env python3
"""Contracts for the World "Local controls" tabs: View, Work and Security.

The Security tab is the only place the World shows an account's own signed-in
sessions and the address each one came from; the Work tab and the owner-only
avatar back plate carry assigned tasks, timers, and recent issue assignments
instead of the retired session card.
"""

from pathlib import Path
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
WORLD = (ROOT.parent / "world" / "public" / "world" / "world.js").read_text(encoding="utf-8")
TASKS = (ROOT.parent / "world" / "public" / "world" / "world-office-tasks.js").read_text(
    encoding="utf-8")
SCENE = world_scene_source()
CSS = (ROOT.parent / "world" / "public" / "world" / "world.css").read_text(encoding="utf-8")


def test_local_controls_expose_three_labelled_tabs():
    for contract in (
        '<h2 id="world-settings-title">Settings</h2>',
        'role="tablist" aria-label="Local controls section"',
        'data-world-settings-tab="view"',
        'data-world-settings-tab="work"',
        'data-world-settings-tab="security"',
        'data-world-settings-pane="view"',
        'data-world-settings-pane="work"',
        'data-world-settings-pane="security"',
        "selectSettingsTab(",
    ):
        assert contract in WORLD
    assert ".world-settings-tabs" in CSS
    # Panes are switched by the hidden attribute, so only one is readable.
    assert "pane.hidden = pane.dataset.worldSettingsPane !== selected" in WORLD


def test_security_tab_lists_sessions_with_addresses_and_logout_controls():
    for contract in (
        'this.fetchJSON("/api/accounts/sessions"',
        "data-world-session-list",
        'data-world-session-revoke="others"',
        'data-world-session-revoke="all"',
        "session.ipAddress",
        "relativeTimeLabel(session.lastSeenAt)",
        "revokeWorldSession(",
        'method: "DELETE"',
        'role="status"',
        'aria-live="polite"',
    ):
        assert contract in WORLD
    # Session copy is account data rendered into innerHTML: every field is
    # escaped, and revoking this device tears the local session down too.
    assert "escapeHTML(session.deviceLabel)" in WORLD
    assert "escapeHTML(session.ipAddress" in WORLD
    assert "await this.logoutFromWorld();" in WORLD


def test_work_tab_shows_assigned_task_stats_with_start_stop():
    for contract in (
        "data-world-organization-task-list",
        "data-world-work-total",
        "data-world-work-active",
        "data-world-work-tracked",
        "data-world-work-issue-list",
        "Recent issue assignments",
        "syncRecentIssueAssignments()",
    ):
        assert contract in WORLD
    assert "this.officeTasks?.setPersonalView?.(true)" in WORLD
    assert "this.officeTasks?.setPersonalView?.(false)" in WORLD
    assert "this.ensureOfficeRuntime({ userInitiated: true }).then(" in WORLD
    assert 'this.settingsTab !== "work"' in WORLD
    for contract in (
        "function renderWorkPane()",
        "function normalizedRecentIssue(issue)",
        "function safeIssueHref(value)",
        "function prime()",
        "function setRecentIssues(items = [])",
        "setPersonalView,",
        "setRecentIssues,",
        "ownTasks()",
        'data-world-office-task-action="${activeTask ? "stop" : "start"}"',
        "function visibleWorkTasks()",
        "function taskBoardHTML(task)",
    ):
        assert contract in TASKS
    # Issue assignment data reuses the already-loaded private notification
    # stream; the task controller creates no second endpoint or poll.
    assert '"issue_assigned"' in WORLD
    assert "item.number" in WORLD
    assert "item.repo" in WORLD
    assert "this.notifications" in WORLD
    assert "/api/issues/assigned" not in WORLD
    assert "OFFICE_TASKS_POLL_MS = 60_000" in TASKS
    assert "OFFICE_TASKS_BACKGROUND_POLL_MS = 5 * 60_000" in TASKS


def test_avatar_back_plate_carries_work_instead_of_the_session_card():
    assert "world.setSelfWorkBoard?.(" in TASKS
    assert "function avatarWorkBadgeTexture" in SCENE
    assert 'badge.name = "forkmesh-self-work-back-badge"' in SCENE
    assert 'item?.kind === "issue" ? "issue" : "task"' in SCENE
    assert 'item.kind === "issue"' in SCENE
    assert "HIDDEN FROM PEERS + SCREENSHOTS" in SCENE
    # Built-in screenshots still hide the owner-only plate.
    assert "world.setSelfWorkBadgeVisibility?.(false)" in WORLD
    for retired in (
        "YOUR SESSION",
        "EDGE IP",
        "avatarSecurityBadgeTexture",
        "setSelfSecurityDetails",
    ):
        assert retired not in SCENE
        assert retired not in WORLD
