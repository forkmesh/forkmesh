"""Contracts for the compact World diagnostics and unified activity HUD."""

from pathlib import Path
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
WORLD = (ROOT.parent / "world/public/world/world.js").read_text(encoding="utf-8")
CSS = (ROOT.parent / "world/public/world/world.css").read_text(encoding="utf-8")
CHAT = (ROOT / "public/dashboard-chat.js").read_text(encoding="utf-8")
SCENE = world_scene_source()
BUILD_BOARD = (ROOT.parent / "world" / "src" / "world_build_board.py").read_text(encoding="utf-8")


DIAGNOSTIC_DOTS = (
    "fps", "frame", "draw", "input", "network", "traffic", "queue",
    "build", "world",
)

# Renderer readings no health dot owns, charted under the nine dot traces.
DIAGNOSTIC_DETAIL_CHARTS = (
    "triangles", "cpu", "p95", "jank", "anim", "avatars",
)


def test_debug_bar_is_a_live_metric_pill_with_graded_health_dots():
    assert 'class="world-diagnostics-orb"' in WORLD
    for metric in DIAGNOSTIC_DOTS:
        assert f'data-diagnostic-dot="{metric}"' in WORLD
    assert "  width: 56px;\n" in CSS
    assert "function diagnosticsChartPoints(" in WORLD
    assert "this.renderDiagnosticsChart(snapshot)" in WORLD
    assert 'trace("triangles", {\n          zeroBased: false,' in WORLD
    assert "verticalInset = 1.5" in WORLD
    assert "const drawableHeight = Math.max(0, height - inset * 2);" in WORLD
    assert "memoryMB: snapshot.memory.chartUsedMB" in WORLD
    assert 'chartSource: Number.isFinite(heapMB) ? "JS heap" : "estimated renderer"' in WORLD
    assert '.world-diagnostics-orb > span[data-level="good"]' in CSS
    assert '.world-diagnostics-orb > span[data-level="caution"]' in CSS
    assert '.world-diagnostics-orb > span[data-level="high"]' in CSS
    assert "function diagnosticDotLevels(snapshot) {" in WORLD
    assert "world-diagnostics-chart" not in CSS.replace(
        ":not(.world-diagnostics-chart)", ""
    )


def _tile_order():
    table = WORLD[
        WORLD.index("const WORLD_PERF_TILES = ["):
        WORLD.index("function worldPerfTileMarkup(tile)")
    ]
    return table, [
        line.split('metric: "', 1)[1].split('"', 1)[0]
        for line in table.splitlines()
        if 'metric: "' in line
    ]


def test_every_health_dot_has_its_own_square_tile_in_the_bottom_row():
    table, order = _tile_order()
    charts = [*DIAGNOSTIC_DOTS, *DIAGNOSTIC_DETAIL_CHARTS, "memory"]
    assert order[: len(charts)] == charts
    for metric in DIAGNOSTIC_DETAIL_CHARTS:
        assert f'{{ metric: "{metric}", label:' in table
    detail_lines = [line for line in table.splitlines() if "detail: true" in line]
    assert len(detail_lines) == len(DIAGNOSTIC_DETAIL_CHARTS)
    markup = WORLD[
        WORLD.index("function worldPerfTileMarkup(tile)"):
        WORLD.index("function worldPerfBarMarkup()")
    ]
    for attribute in (
        'data-world-diagnostics-chart-metric="${tile.metric}"',
        'data-world-diagnostics-chart-line="${tile.metric}"',
        'data-world-diagnostics-chart-area="${tile.metric}"',
        'data-world-diagnostics-chart-value="${tile.metric}"',
        "data-world-diagnostics-chart-detail",
        'viewBox="0 0 ${WORLD_PERF_TILE_SIZE} ${WORLD_PERF_TILE_SIZE}"',
    ):
        assert attribute in markup
    assert "const WORLD_PERF_TILE_SIZE = 40;" in WORLD
    assert "${worldPerfBarMarkup()}`;" in WORLD
    pill = WORLD[
        WORLD.index('<details class="world-diagnostics" data-world-diagnostics'):
        WORLD.index("<strong>WORLD DEBUG</strong>")
    ]
    assert "data-world-diagnostics-chart" not in pill
    assert (
        'forkmesh-world[data-world-perf-layout="on"] > .fm-world {\n'
        "  height: calc(var(--world-viewport-height, 100dvh) - var(--world-perf-bar-height));"
    ) in CSS
    assert ".world-perf-tile {" in CSS
    assert "  width: 44px;\n  height: 44px;\n" in CSS


def test_draw_charts_calls_and_triangles_get_their_own_live_trace():
    table, _ = _tile_order()
    assert '{ metric: "draw", label: "DRAW", title: "Draw calls per frame" }' in table
    assert '{ metric: "triangles", label: "TRIS", title: "Triangles drawn per frame", detail: true }' in table
    assert 'trace("drawCalls", {' in WORLD
    assert "value: renderer ? compactCount(renderer.calls) : \"—\"," in WORLD
    assert "value: renderer ? compactCount(renderer.triangles) : \"—\"," in WORLD


def test_renderer_detail_traces_sample_once_a_second_and_grade_themselves():
    table, _ = _tile_order()
    for label in ('label: "CPU"', 'label: "P95"', 'label: "JANK"', 'label: "ANIM"', 'label: "AVTR"'):
        assert label in table
    for key in (
        "drawCalls: liveRenderer ? liveRenderer.calls : NaN,",
        "frameTimeP95Ms: liveRenderer ? liveRenderer.frameTimeP95Ms : NaN,",
        "cpuFrameMs: liveRenderer ? liveRenderer.cpuFrameMs : NaN,",
        "animations: liveRenderer ? liveRenderer.animations : NaN,",
        "remoteAvatars: liveRenderer ? liveRenderer.remoteAvatars : NaN,",
    ):
        assert key in WORLD
    for metric in ("triangles", "cpuFrameMs", "frameTimeP95Ms", "longFrames"):
        assert f'diagnosticLevel("{metric}", ' in WORLD
    assert WORLD.count(': "unavailable",') >= 6
    assert '.world-perf-tile[data-level="unavailable"]' in CSS


def test_square_tiles_draw_right_aligned_area_traces_like_the_desktop():
    assert "    slots = 0,\n" in WORLD
    assert "width - (retainedCount - 1 - index) * slotWidth" in WORLD
    assert "function diagnosticsChartArea(points, height) {" in WORLD
    assert "slots: 60," in WORLD
    assert "diagnosticsChartArea(reading.points, WORLD_PERF_TILE_SIZE)" in WORLD
    assert ".world-perf-tile polygon {" in CSS


def test_state_dots_chart_their_own_verdict_on_a_fixed_scale():
    assert "const WORLD_DIAGNOSTIC_HEALTH_SCORES = { good: 1, caution: 0.5, high: 0 };" in WORLD
    assert "function diagnosticHealthScore(level) {" in WORLD
    assert "if (Number.isFinite(ceiling)) high = Math.max(high, ceiling);" in WORLD
    assert "const healthChart = (key) => trace(key, { ceiling: 1 });" in WORLD
    for key in ("networkHealth", "buildHealth", "worldHealth"):
        assert f"{key}: diagnosticHealthScore(dotLevels." in WORLD
        assert f'healthChart("{key}")' in WORLD


def test_history_keeps_sampling_while_the_scene_is_paused():
    # The socket, queue and build traces stay continuous across a paused scene;
    # renderer readings go in as NaN so their traces gap instead of dipping.
    assert "this.diagnosticsFrameHistory.push({" in WORLD
    assert "snapshot.renderer && !snapshot.renderer.paused ? snapshot.renderer : null" in WORLD
    assert "fps: liveRenderer ? liveRenderer.fps : NaN," in WORLD
    assert "longestFrameMs: liveRenderer ? liveRenderer.longestFrameMs : NaN," in WORLD
    assert "inputResponseMs: liveRenderer ? liveRenderer.inputResponseMs : NaN," in WORLD
    # Frame health only speaks for seconds that were actually rendered.
    assert (
        "const samples = (Array.isArray(history) ? history : []).filter((sample) =>\n"
        "      Number.isFinite(sample?.frameTimeMs),\n"
        "    );"
    ) in WORLD
    # Cumulative queue counters are charted as the per-second rise, not the sum.
    assert "queueEvents: Math.max(0, queueTotal - previousQueueTotal)," in WORLD
    assert "this.diagnosticsQueueTotal = NaN;" in WORLD


def test_island_composer_drops_channel_tabs_and_keeps_the_chat_action():
    assert "data-world-quick-channel" not in WORLD
    assert "forkmesh.worldComposer.selectedChannel" not in WORLD
    assert "data-world-quick-attachment" in WORLD
    assert 'class="world-hud-input-shell"' in WORLD
    assert 'fullAction.value = "chat";' in CHAT
    keydown = CHAT.split('inputEl.addEventListener("keydown"', 1)[1].split(
        'inputEl.addEventListener("input"', 1
    )[0]
    assert 'fullAction.value = "chat"' not in keydown
    assert "pulseWorldQuickComposer(fullAction?.value || \"chat\")" in CHAT
    assert "fullComposerStatus.classList.remove(" in CHAT
    assert "fullComposerStatus.className =" not in CHAT


def test_hud_popouts_keep_close_controls_sticky_and_dismiss_outside():
    for heading in (
        ".world-detail-header",
        ".world-settings-heading",
        ".world-account-heading",
        ".world-chat-heading",
        ".world-office-panel-heading",
    ):
        assert heading in CSS
    assert "position: sticky;" in CSS
    assert "top: 12px;" in CSS
    for selector in (
        '!event.target.closest("[data-world-settings]")',
        '!event.target.closest("[data-world-account]")',
        '!event.target.closest("[data-world-chat]")',
    ):
        assert selector in WORLD
    assert '"[data-world-detail], [data-world-detail-resize]"' in WORLD


def test_corner_launchers_stay_clear_and_debug_stays_open_until_outside_click():
    assert "  width: 56px;\n" in CSS
    assert ".fm-world .world-shirt-badge" in CSS
    assert 'diagnostics.addEventListener("pointerenter"' in WORLD
    assert 'diagnostics?.addEventListener("focusin"' in WORLD
    assert 'diagnostics.removeAttribute("open")' in WORLD
    assert 'details.addEventListener("pointerleave"' not in WORLD


def test_todo_actions_are_bridged_into_the_chat_transcript():
    assert "notifyChatArea(message, kind" in WORLD
    assert 'type: "forkmesh:chat-notification"' in WORLD
    assert 'data.type === "forkmesh:chat-ready"' in WORLD
    assert "this.chatTaskNotifications" in WORLD
    assert "this.notifyChatArea(copy, tone);" in WORLD
    assert "notify: (message, options) => this.notice(message, options)," in WORLD


def test_world_chat_has_separate_chat_and_task_paths_and_routing_step():
    assert 'id="fullChatSend"' in WORLD
    assert "wireTaskSend()" in CHAT
    assert 'fullAction.value = "task"' in CHAT
    assert "Choose a team, then assign this task to a person or an agent." in CHAT
    assert "wireInput(fullInput, fullSend)" in CHAT
    assert "forceChat" not in CHAT
    assert 'fullSendLabel.textContent = presentation.label || "Send"' in CHAT
    assert 'fullAction.value !== "chat"' in CHAT
    assert "void runFullComposerAction(inputEl, attachmentControl)" in CHAT


def test_unsolicited_notices_land_in_the_transcript_without_a_bubble_stack():
    # No toasts: the floating bubble stack above the chat icon is gone, and
    # nothing pops up over the World. Pings, announcements and presence events
    # are written into the native chat transcript, which already lists them.
    assert "data-world-activity-stream" not in WORLD
    assert "world-activity-stream" not in CSS
    assert "toast" not in WORLD.lower()
    assert "world-toast" not in CSS
    assert 'type: "forkmesh:chat-notification"' in WORLD
    assert "appendSystem(text);" in CHAT
    assert "forkmesh:world-activity" not in CHAT
    announce = WORLD[
        WORLD.index("  announceWorldNotifications() {"):
        WORLD.index("\n  }", WORLD.index("  announceWorldNotifications() {"))
    ]
    assert 'this.notifyChatArea(`New ping: ${item.title}`, "status");' in announce
    assert "this.notice(" not in announce


def test_action_feedback_is_written_inline_where_the_action_happened():
    notice = WORLD[
        WORLD.index('  notice(message, { kind = "", at = null } = {}) {'):
        WORLD.index("\n  watchInlineNotices() {")
    ]
    assert "origin.closest(WORLD_NOTICE_HOSTS)" in notice
    assert "const line = worldInlineStatusLine(host);" in notice
    assert "if (block) block.after(line);" in notice
    assert "this.hudNotice(copy, tone);" in notice
    assert "setTimeout" not in notice
    assert 'line.dataset.worldInlineStatus = "";' in WORLD
    assert 'line.setAttribute("role", "status");' in WORLD
    assert 'document.addEventListener(type, this.recordNoticeOrigin, true);' in WORLD
    assert 'document.removeEventListener(type, this.recordNoticeOrigin, true);' in WORLD
    rule = CSS[CSS.index(".world-inline-status {"):]
    rule = rule[:rule.index("}")]
    assert "position" not in rule
    assert ".world-inline-status[data-tone=\"error\"]" in CSS
    island = WORLD[
        WORLD.index('class="world-hud-island"'):
        WORLD.index("</section>", WORLD.index('class="world-hud-island"'))
    ]
    assert island.index("world-hud-island-bar") < island.index(
        "data-world-hud-notice",
    ) < island.index('id="fullChatMessages"')
    hud = WORLD[
        WORLD.index('  hudNotice(message, kind = "", { sticky = false } = {}) {'):
        WORLD.index("\n  dismissHudNotice() {")
    ]
    assert 'if (tone !== "error" && !sticky) {' in hud
    assert ".fm-world .world-hud-notice {" in CSS


def test_island_starts_collapsed_with_actions_feed_and_composer_in_order():
    island = WORLD[
        WORLD.index('class="world-hud-island"'):
        WORLD.index("</section>", WORLD.index('class="world-hud-island"'))
    ]
    assert 'data-expanded="false"' in island
    assert island.index("data-world-avatar-actions") < island.index(
        'id="fullChatMessages"',
    )
    assert island.index('id="fullChatMessages"') < island.index(
        "data-dashboard-chat-composer",
    )
    assert "data-dashboard-chat-composer-toolbar hidden inert" in island
    assert 'messageActionButton("☺"' in CHAT
    assert "content?.append(buildMessageActions(record));" in CHAT


def test_island_follows_the_visible_viewport_and_keyboard():
    assert "var(--world-chat-viewport-height, 100dvh)" in CSS
    assert "var(--world-chat-covered-bottom, 0px)" in CSS
    assert '"--world-chat-viewport-height"' in WORLD
    assert '"--world-chat-covered-bottom"' in WORLD
    assert "env(safe-area-inset-bottom)" in CSS
    assert "@media (prefers-reduced-motion: reduce)" in CSS


def test_new_hud_work_is_recorded_in_the_build_board_api():
    assert '"task:world-orb-hud"' in BUILD_BOARD


def test_new_hud_has_an_explicit_shared_qa_card():
    # The QA deck moved out of entry.py so the isolate only pays for it on the
    # QA route; the cards themselves still have to spell out the new HUD work.
    qa = (ROOT.parent / "world" / "src" / "world_qa.py").read_text(encoding="utf-8")
    assert "WORLD_QA_DECK_REVISION = " in qa
    assert '"world-compact-debug-chat-orbs"' in qa
    assert "logo-sized circles while closed" in qa
    assert "fades after ten " in qa
    assert '"world-debug-live-telemetry-strip"' in qa
    assert "DRAW must read draw calls per frame and TRIS " in qa
