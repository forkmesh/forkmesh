#!/usr/bin/env python3
"""Frontend contracts for the private Office marketing task board."""

import json
from pathlib import Path
import subprocess
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
PUBLIC = ROOT.parent / "world" / "public" / "world"
TASKS = PUBLIC / "world-office-tasks.js"
SCENE = PUBLIC / "world-scene.js"
WORLD = PUBLIC / "world.js"
OFFICE = PUBLIC / "world-office.js"
CSS = PUBLIC / "world.css"


def source(path):
    return path.read_text(encoding="utf-8") if path.exists() else ""


def test_marketing_task_controller_is_wired_to_the_office_and_scene():
    world = source(WORLD)
    office = source(OFFICE)
    for contract in (
        'import("./world-office-tasks.js")',
        "createWorldOfficeTasksController",
        "onOfficeTaskBoardSelect",
        "this.officeTasks",
    ):
        assert contract in world
    assert 'tasks?.setActive?.(floor.id === "marketing")' in office
    assert "tasks?.setActive?.(false)" in office


def test_marketing_board_has_accessible_create_assign_and_timer_controls():
    world = source(WORLD)
    for contract in (
        "data-world-office-task-panel",
        "data-world-office-task-form",
        "data-world-office-task-name",
        "data-world-office-task-assignee",
        "Select a Marketing team member",
        "data-world-office-task-list",
        "data-world-office-task-checkin",
        'role="status"',
        'aria-live="polite"',
    ):
        assert contract in world


def test_work_tab_has_a_compact_universal_task_creator_with_agent_routing():
    world = source(WORLD)
    tasks = source(TASKS)
    css = source(CSS)
    for contract in (
        "data-world-work-task-form",
        "data-world-work-task-title",
        "data-world-work-task-assignee",
        "data-world-work-task-department",
        "data-world-work-task-repository",
        "data-world-work-task-priority",
    ):
        assert contract in world
    assert 'label: "Bot"' in tasks
    assert 'assignment === "agent" ? "agent"' in tasks
    assert 'mutate(OFFICE_TASKS_PATH, body, "", { silent: true })' in tasks
    assert "repository before assigning agent work" in tasks
    assert ".world-work-quick-entry" in css


def test_agent_task_creator_reports_saved_state_without_claiming_dispatch():
    tasks = source(TASKS)
    assert "Agent task saved to the organization task list." in tasks
    assert "Agent task created and queued for a linked desktop." not in tasks
    assert "lastMutationError" in tasks
    assert 'String(error?.message || "").trim()' in tasks
    assert 'lastMutationError || "Task could not be created."' in tasks


def test_tasks_use_https_polling_and_server_clock_without_world_socket_data():
    tasks = source(TASKS)
    assert '"/api/world/office/marketing-tasks"' in tasks
    assert 'cache: "no-store"' in tasks
    assert "serverNowAtSync" in tasks
    assert "nextCheckinAt" in tasks
    assert "elapsedMs" in tasks
    assert "performance.now()" in tasks
    assert "OFFICE_TASKS_POLL_MS" in tasks
    for forbidden in (
        "WebSocket",
        "sendPresence",
        "sendMeeting",
        "localStorage",
        "sessionStorage",
    ):
        assert forbidden not in tasks


def test_task_polling_is_visibility_aware_and_adapts_after_leaving_the_office():
    tasks = source(TASKS)
    assert "const OFFICE_TASKS_POLL_MS = 60_000" in tasks
    assert "const OFFICE_TASKS_BACKGROUND_POLL_MS = 5 * 60_000" in tasks
    schedule = tasks[
        tasks.index("  function schedulePoll() {"):
        tasks.index("\n  async function refresh(", tasks.index("  function schedulePoll() {"))
    ]
    assert (
        "document.hidden || (!officeActive && !opened && !personalView)"
        in schedule
    )
    assert "if (!document.hidden) await refresh({ quiet: true })" in schedule
    assert "function onVisibilityChange()" in schedule
    assert "void refresh({ quiet: true }).finally(schedulePoll)" in schedule
    assert (
        'document.addEventListener("visibilitychange", onVisibilityChange)'
        in tasks
    )
    assert (
        'document.removeEventListener("visibilitychange", onVisibilityChange)'
        in tasks
    )


def test_random_checkin_window_stays_between_four_and_nine_minutes():
    script = f"""
      import {{ officeTaskCheckinDelay }} from {json.dumps(TASKS.as_uri())};
      if (officeTaskCheckinDelay(0) !== 240000) throw new Error("minimum");
      if (officeTaskCheckinDelay(1) !== 540000) throw new Error("maximum");
      const midpoint = officeTaskCheckinDelay(0.5);
      if (midpoint < 240000 || midpoint > 540000) throw new Error("bounds");
    """
    subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True,
        capture_output=True,
        text=True,
    )


def test_active_timer_uses_a_bounded_keepalive_stop_when_world_is_destroyed():
    tasks = source(TASKS)
    assert "function stopActiveForDeparture()" in tasks
    assert '`${OFFICE_TASKS_PATH}/stop-active`' in tasks
    assert 'keepalive: true' in tasks
    assert 'authorization: `Bearer ${sessionToken}`' in tasks
    destroy = tasks[
        tasks.index("  function destroy() {"):
        tasks.index("\n  root.addEventListener", tasks.index("  function destroy() {"))
    ]
    assert destroy.index("stopActiveForDeparture()") < destroy.index(
        "stopMonitoring()"
    )


def test_task_access_is_session_gated_and_wall_payload_is_explicitly_authorized():
    tasks = source(TASKS)
    assert "getSession()?.sessionToken" in tasks
    assert "authorized = false" in tasks
    assert "authorized = payload?.authorized !== false" in tasks
    assert "world.updateOfficeMarketingTasks" in tasks
    assert "authorized," in tasks
    assert "physicalState(\"locked\"" in tasks


def test_sortable_catalog_includes_departments_without_a_top_nav_count():
    tasks = source(TASKS)
    world = source(WORLD)
    assert "data-world-tasks-open" not in world
    assert "data-world-task-count" not in world
    assert "data-world-task-count" not in tasks
    assert "data-world-organization-task-heading" in world
    assert "function visibleWorkTasks()" in tasks
    assert "data-world-task-avatar" in tasks
    assert "data-tone=\"department\"" in tasks
    assert "data-world-task-search" in world
    assert "data-world-task-filter" in world
    assert "data-world-task-sort" in world
    assert "world-task-row-metadata" in tasks


def test_organization_catalog_can_filter_by_any_member():
    tasks = source(TASKS)
    assert 'workFilter.startsWith("user:")' in tasks
    assert 'task.assignee === workFilter.slice(5)' in tasks
    assert '<optgroup label="Assignee">' in tasks
    assert 'organizationMembers.includes(selectedFilter.slice(5))' in tasks


def test_organization_catalog_is_a_dense_sortable_table_with_row_bubbles():
    tasks = source(TASKS)
    world = source(WORLD)
    css = source(CSS)
    for contract in (
        'class="world-task-table-header" role="row"',
        "data-world-task-column-sort",
        'class="world-task-row-summary"',
        'class="world-task-chat-bubble"',
        'role="table"',
        '<option value="repository">Repository</option>',
        '<option value="tracked">Tracked time</option>',
        '<option value="qa">QA</option>',
    ):
        assert contract in tasks or contract in world
    for column in (
        "world-task-cell-title",
        "world-task-cell-status",
        "world-task-cell-priority",
        "world-task-cell-owner",
        "world-task-cell-agent",
        "world-task-cell-model",
        "world-task-cell-speed",
        "world-task-cell-route",
        "world-task-cell-updated",
        "world-task-cell-time",
        "world-task-cell-qa",
        "world-task-cell-actions",
    ):
        assert column in tasks
    for styled_column in (
        "world-task-cell-title",
        "world-task-cell-status",
        "world-task-cell-priority",
        "world-task-cell-owner",
        "world-task-cell-route",
        "world-task-cell-time",
        "world-task-cell-qa",
        "world-task-cell-actions",
    ):
        assert styled_column in css
    assert "grid-template-columns:" in css
    assert "min-height: 42px" in css


def test_task_table_has_quick_filters_bot_batches_thumbnails_and_words():
    tasks = source(TASKS)
    world = source(WORLD)
    css = source(CSS)
    for marker in (
        'data-world-task-quick-filter="active"',
        'data-world-task-quick-filter="queued"',
        'data-world-task-batch-size',
        'value="10"',
        "data-world-task-batch-send",
    ):
        assert marker in world
    for marker in (
        'task.agent?.provider',
        'task.agent?.model',
        'task.agent?.strength',
        "world-task-attachment-thumbnail",
        ">Follow-up</button>",
        '"Start"',
        '"Stop"',
        '"Done"',
        ">Delete</button>",
    ):
        assert marker in tasks
    assert "min-width: 1580px" in css
    assert ".world-task-attachment-thumbnail" in css


def test_work_tab_expands_and_active_tasks_have_readable_spinner_status():
    tasks = source(TASKS)
    world = source(WORLD)
    css = source(CSS)
    assert "panel.dataset.activeTab = selected" in world
    assert '.world-settings[data-active-tab="work"]' in css
    assert "920px" in css
    assert "world-office-task-progress-label" in tasks
    assert "In progress" in tasks
    assert "@keyframes world-office-task-spin" in css
    assert "animation: world-office-task-spin 720ms linear infinite" in css
    assert "white-space: normal" in css
    assert "-webkit-line-clamp: 2" not in css


def test_new_agent_tasks_show_one_bounded_world_bot_bubble():
    tasks = source(TASKS)
    scene = world_scene_source()
    assert "const announcedAgentTaskIds = new Set()" in tasks
    assert "function announceAgentTasks()" in tasks
    assert 'task.assigneeKind === "agent"' in tasks
    assert "world.showAgentTaskBubble?.(task.assigneeKind, task.title)" in tasks
    assert "function showAgentTaskBubble(botId, title)" in scene
    assert "if (!agentBotAccessAllowed) return false" in scene
    assert "New organization task: ${taskTitle}" in scene
    assert "showAgentTaskBubble," in scene


def test_only_assignees_receive_start_stop_and_checkin_controls():
    tasks = source(TASKS)
    assert "task.assignee === actor" in tasks
    assert 'task.status === "active"' in tasks
    assert 'data-world-office-task-action="${activeTask ? "stop" : "start"}"' in tasks
    assert "ownActiveTasks()" in tasks
    assert "showCheckin" in tasks


def test_manager_assignment_control_renders_only_returned_marketing_members():
    tasks = source(TASKS)
    world = source(WORLD)
    assert "<select" in world
    assert "data-world-office-task-assignees" in world
    assert "Select a Marketing team member" in tasks
    assert "marketingMembers" in tasks
    assert "<option value=" in tasks
    assert "<datalist" not in world


def test_physical_wall_has_direct_bounded_marketing_task_controls():
    tasks = source(TASKS)
    scene = world_scene_source()
    world = source(WORLD)
    for contract in (
        "async function physicalAction(payload = {})",
        "canStartStop: task.assignee === actor",
        "canComplete:",
        "canDelete: canManage",
        "members: canManage ? assignable : marketingMembers",
        "physicalAction,",
    ):
        assert contract in tasks
    for contract in (
        "function marketingTaskWallAction(uv)",
        '"+ ADD MARKETING TASK"',
        '"START"',
        '"DONE"',
        '"DELETE"',
        'action = task.status === "active" ? "stop" : "start"',
        "onOfficeTaskWallAction(action)",
    ):
        assert contract in scene
    assert "this.officeTasks?.physicalAction?.(action)" in world
    assert "new THREE.BoxGeometry(18.5, 11.4, 0.18)" in scene
    assert "new THREE.PlaneGeometry(18.12, 11.02)" in scene


def test_assignment_control_has_explicit_contrast_and_tasks_can_finish_or_delete():
    tasks = source(TASKS)
    css = source(CSS)
    assert 'data-world-office-task-action="complete"' in tasks
    assert 'data-world-office-task-action="delete"' in tasks
    assert '{ method: "DELETE", removeOnSuccess: true }' in tasks
    assert 'window.confirm("Delete this task' not in tasks
    assert ".world-office-task-manager select option {\n  background: #ffffff;\n  color: #111827;\n}" in css
    assert '.world-office-task[data-status="done"]' in css


def test_a_confirmed_delete_leaves_every_list_and_a_refusal_is_announced():
    """A deleted row may never be repainted by a slower or skipped read."""
    tasks = source(TASKS)
    world = source(WORLD)
    for contract in (
        "function dropTask(taskId)",
        "if (removeOnSuccess) dropTask(taskId);",
        "await refresh({ quiet: true, force: true });",
        # A read that started before the delete must not join it, and its
        # older payload must not overwrite the newer list.
        'if ((!monitoring && !force) || typeof fetchJSON !== "function")',
        "if (refreshPromise && !force) return refreshPromise;",
        "const sequence = ++refreshSequence;",
        "if (sequence !== refreshSequence) return false;",
        "dedupe: !force,",
        # A rejected change is reported inline: on the Office panel's status
        # line, or next to the Work tab control the visitor used.
        'if (errorMessage && !silent) report(errorMessage, "error");',
    ):
        assert contract in tasks
    # fetchJSON honors the dedupe opt-out the forced read relies on.
    assert "dedupe = true," in world
    assert "if (canDedupe && this.inflightRequests.has(requestKey))" in world


def test_completed_tasks_are_ready_for_qa_with_three_verdict_controls():
    tasks = source(TASKS)
    world = source(WORLD)
    css = source(CSS)
    for contract in (
        '"Ready for QA"',
        'data-world-office-task-action="qa-${verdict}"',
        '["pass", "fail", "unsure"]',
        "onQaVerdict({ task, verdict })",
        "Task marked done and ready for QA.",
    ):
        assert contract in tasks
    assert "this.recordTaskQaVerdict(task, verdict)" in world
    assert "async recordTaskQaVerdict(task, verdict)" in world
    assert "this.recordQaVerdict(verdict)" in world
    assert ".world-office-task-qa-actions" in css


def test_marketing_members_can_add_private_proof_from_their_own_desk():
    tasks = source(TASKS)
    scene = world_scene_source()
    for contract in (
        "function normalizedProof(proof)",
        "proofs = Array.isArray(marketingPayload?.proofs)",
        'if (action === "proof")',
        "!marketingMembers.includes(actor) || member !== actor",
        '`${MARKETING_TASKS_PATH}/proofs`',
        '"Paste the public HTTPS social post link:"',
    ):
        assert contract in tasks
    for contract in (
        "function officeMarketingDeskProofTexture(",
        "officeMarketingTaskSnapshot.proofs",
        'proofPanel.userData.interactive = "office-marketing-proof-desk"',
        'action: "proof"',
    ):
        assert contract in scene


def test_private_marketing_initiatives_are_rendered_on_a_clickable_world_panel():
    tasks = source(TASKS)
    scene = world_scene_source()
    for contract in (
        "function normalizedInitiative(initiative)",
        "initiatives = Array.isArray(marketingPayload?.initiatives)",
        'if (action === "initiative")',
        "initiatives.some((item) => item.href === href)",
        'window.open(href, "_blank", "noopener,noreferrer")',
    ):
        assert contract in tasks
    for contract in (
        "function officeMarketingInitiativesTexture(",
        '"forkmesh-office-marketing-initiatives-board"',
        '"office-marketing-initiatives-board"',
        "function marketingInitiativeWallAction(uv)",
        'return { action: "initiative", href: initiative.href }',
    ):
        assert contract in scene


def test_task_text_is_bounded_and_html_escaped_before_rendering():
    tasks = source(TASKS)
    assert "function escapeHTML" in tasks
    assert ".slice(0, limit)" in tasks
    assert "${escapeHTML(task.title)}" in tasks
    assert "task.assigneeKind === \"user\"" in tasks
    assert "`@${task.assignee}`" in tasks
    assert "escapeHTML(" in tasks
    assert "safeTaskId" in tasks


def test_task_rows_capture_and_retain_a_completion_work_note():
    tasks = source(TASKS)
    css = source(CSS)
    for contract in (
        "completionNote: text(task.completionNote, 4000)",
        "What I did to complete this task",
        "data-world-office-task-completion-note",
        'action === "complete" ? { completionNote } : {}',
        "task.completionNote || \"\"",
    ):
        assert contract in tasks
    for selector in (
        ".world-office-task-completion",
        ".world-office-task-completion-editor",
        ".world-office-task-completion-editor textarea",
    ):
        assert selector in css


def test_office_task_list_still_shows_bids_filed_from_the_website():
    world = source(WORLD)
    scene = world_scene_source()
    tasks = source(TASKS)
    css = source(CSS)
    for removed in (
        "openLobbyTaskBidKiosk",
        "data-world-task-bid-form",
        "bountyAmountSol",
    ):
        assert removed not in world, removed
    for removed in (
        "forkmesh-office-task-bid-kiosk",
        "TASK BOUNTY DESK",
        "office-task-bid-kiosk",
        "onLobbyTaskBidKioskSelect",
    ):
        assert removed not in scene, removed
    for contract in (
        'const kind = task.kind === "bid" ? "bid" : "task"',
        'data-kind="${bid ? "bid" : "task"}"',
        "SOL bounty requested · bidder @",
        "world-office-task-bid-badge",
        "function refreshNow()",
    ):
        assert contract in tasks
    assert '.world-office-task[data-kind="bid"]' in css
    assert ".world-office-task-bid-badge" in css


def test_task_panel_is_overlayed_responsive_and_reduced_motion_safe():
    css = source(CSS)
    for selector in (
        ".world-office-task-panel",
        ".world-office-task-manager",
        ".world-office-task-list",
        ".world-office-task-controls",
        ".world-office-task-checkin",
    ):
        assert selector in css
    assert '.world-office-task-panel[data-open="true"]' in css
    assert (
        '.fm-world:has(> .world-hud > .world-office-task-panel[data-open="true"])'
        in css
    )
    assert "@media (max-width: 720px)" in css
    assert "@media (prefers-reduced-motion: reduce)" in css


def test_physical_board_displays_bounded_authorized_summaries_only():
    scene = world_scene_source()
    assert "normalizeOfficeMarketingTasks(payload)" in scene
    assert "const authorized = source.authorized === true" in scene
    assert "const OFFICE_MARKETING_TASK_LIMIT = 250" in scene
    assert "task.elapsed" in scene
    assert "officeMarketingRosterGroup" in scene
    assert "officeMarketingAttendanceTexture" in scene
    assert "officeReclaimedWoodTexture" in scene


def test_one_general_bot_replaces_the_claude_and_codex_task_choice():
    tasks = source(TASKS)
    chat_view = (
        ROOT / "public" / "dashboard" / "partials" / "views" / "chat.html"
    ).read_text(encoding="utf-8")
    chat = (ROOT / "public" / "dashboard-chat.js").read_text(encoding="utf-8")
    assert '<option value="agent">Bot</option>' in chat_view
    assert '<option value="codex">Codex</option>' not in chat_view
    assert '<option value="claude">Claude</option>' not in chat_view
    assert 'const ORG_BOT_SENDER_ID = "agent"' in chat
    assert "MCP bots consume repository-linked agent tasks" in chat
    assert "MCP task created." in chat
    assert "no bot node accepted it yet" not in chat
    assert '"Bot",' in tasks
    assert '"Codex"' not in tasks
    assert '"Claude"' not in tasks


def test_task_images_are_embedded_on_tasks_and_lazy_loaded():
    tasks = source(TASKS)
    css = source(CSS)
    chat = (ROOT / "public" / "dashboard-chat.js").read_text(encoding="utf-8")
    assert "async function taskImageAttachments(control)" in chat
    assert "Each task image must be 256 KiB or smaller." in chat
    assert "file: bytesToB64(await file.arrayBuffer())" in chat
    assert "await sendDashboardDraft(attachmentControl)" not in chat[
        chat.index("async function runFullComposerAction"):
        chat.index("function sendFrom")
    ]
    assert 'loading="lazy"' in tasks
    assert "world-office-task-image" in tasks
    assert ".world-office-task-image img" in css


def test_queued_bot_tasks_can_be_returned_to_the_task_list():
    tasks = source(TASKS)
    css = source(CSS)
    for contract in (
        'const botTask = task.assigneeKind === "agent"',
        "canManage || task.createdBy === actor",
        'data-world-office-task-action="return"',
        '"start", "stop", "complete", "delete", "return",',
        "Task returned to the task list.",
        "queued on a node",
    ):
        assert contract in tasks
    assert ".world-office-task-return" in css


def test_task_feedback_is_reported_inline_where_the_visitor_acted():
    tasks = source(TASKS)
    world = source(WORLD)
    assert "toast" not in tasks
    report = tasks[tasks.index('  function report(message, tone = "") {'):]
    report = report[:report.index("\n  }\n")]
    assert "if (noticeInPanel && opened) {" in report
    assert "setStatus(message, tone);" in report
    assert 'if (!noticeOrigin) options.at = "[data-world-hud-notice]";' in report
    assert "notify(message, options);" in report
    for handler in (
        "  async function onSubmit(event) {\n    trackNoticeOrigin(event);",
        "  async function onClick(event) {\n    trackNoticeOrigin(event);",
        "  function onWorkControl(event) {\n    trackNoticeOrigin(event);",
        "  async function physicalAction(payload = {}) {\n    trackNoticeOrigin(null);",
    ):
        assert handler in tasks
    assert 'await mutate(OFFICE_TASKS_PATH, body, "", { silent: true });' in tasks
    assert "notify: (message, options) => this.notice(message, options)," in world
