import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORLD_DIR = ROOT / "world" / "public" / "world"
APP = (WORLD_DIR / "world.js").read_text(encoding="utf-8")
CSS = (WORLD_DIR / "world.css").read_text(encoding="utf-8")
TASKS = (WORLD_DIR / "world-office-tasks.js").read_text(encoding="utf-8")
CHAT = (ROOT / "app" / "public" / "dashboard-chat.js").read_text(encoding="utf-8")


def _between(source, start, end):
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


ISLAND = _between(APP, '<section\n          class="world-hud-island"', "</section>")
STATUS = _between(APP, '<nav class="world-status-actions"', "</nav>")
HELPERS = _between(
    APP, "const WORLD_AGENT_COMPOSER_KEY", "function orgAgentSavedMessage(",
)


def _run_helpers(body):
    script = HELPERS + "\n" + body
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True,
        text=True,
        capture_output=True,
    )
    return json.loads(result.stdout)


def test_island_holds_actions_feed_input_model_target_and_one_send_button():
    order = [
        "data-world-avatar-actions",
        "data-world-island-toggle",
        'id="fullChatMessages"',
        "data-world-composer-modes",
        'id="fullChatInput"',
        'id="fullChatSend"',
        "data-world-agent-controls",
        "data-world-agent-model",
        "data-world-agent-targets",
        "data-world-agent-run",
    ]
    positions = [ISLAND.index(marker) for marker in order]
    assert positions == sorted(positions)
    assert ISLAND.count("<button id=") == 1
    assert 'data-world-agent-target="desktop"' in ISLAND
    assert 'data-world-agent-target="runner"' in ISLAND
    assert 'data-world-composer-mode="chat"' in ISLAND
    assert 'data-world-composer-mode="agent"' in ISLAND
    assert 'data-world-agent-controls hidden' in ISLAND
    assert 'aria-haspopup="listbox"' in ISLAND
    assert 'role="radiogroup" aria-label="Send as"' in ISLAND
    assert 'role="radiogroup" aria-label="Run on"' in ISLAND


def test_old_chat_bubble_panel_and_actions_toggle_are_gone():
    for removed in (
        "data-world-chat-terminal",
        "world-chat-terminal",
        "data-world-quick-channel",
        "data-world-avatar-actions-toggle",
        'id="fullChatTaskSend"',
        "data-dashboard-task-routing",
        "setChatTerminalLastMessage",
        "scheduleQuickComposerIdle",
    ):
        assert removed not in APP
    for removed in (
        "world-chat-terminal",
        "world-quick-chat",
        "world-quick-channel",
        "world-avatar-action-menu",
    ):
        assert removed not in CSS


def test_action_buttons_are_small_but_keep_a_tall_touch_target():
    buttons = _between(
        CSS,
        ".fm-world .world-hud-island :is(.world-avatar-actions button, .world-hud-island-toggle, .world-hud-island-open-chat) {",
        "}",
    )
    assert "width: 30px;" in buttons
    assert "height: 30px;" in buttons
    hit = _between(
        CSS,
        ".fm-world .world-hud-island :is(.world-avatar-actions button, .world-hud-island-toggle, .world-hud-island-open-chat)::after {",
        "}",
    )
    assert "inset: -7px -1px;" in hit
    icon = _between(CSS, ".fm-world .world-hud-island .world-ui-icon {", "}")
    assert "width: 16px;" in icon


def test_island_is_centered_compact_and_respects_safe_areas_and_keyboard():
    island = _between(CSS, ".fm-world .world-hud-island {", "\n}")
    assert "left: 50%;" in island
    assert "transform: translate(calc(-50% + var(--world-hud-island-x, 0px)), var(--world-hud-island-y, 0px));" in island
    assert "width: min(540px, calc(100vw - 24px - env(safe-area-inset-left) - env(safe-area-inset-right)));" in island
    assert "bottom: calc(var(--world-hud-island-gap) + var(--world-chat-covered-bottom, 0px));" in island
    assert "--world-hud-island-gap: max(12px, env(safe-area-inset-bottom));" in CSS
    phone = _between(CSS, "@media (max-width: 600px) {\n  .fm-world {\n    --world-hud-island-gap", "\n}\n")
    assert "width: calc(100vw - 16px - env(safe-area-inset-left) - env(safe-area-inset-right));" in phone
    assert ".fm-world .world-hud-mode button span {\n    display: none;" in phone
    assert "#fullChatInput {\n    font-size: 16px;" in CSS
    assert "bottom: var(--world-hud-island-clearance);" in CSS
    assert 'this.style.setProperty("--world-hud-island-height", `${height}px`);' in APP
    assert 'forkmesh-world[data-world-island-expanded="true"] .fm-world .world-touch-controls' in CSS


def test_feed_collapses_to_two_lines_and_expands_on_demand():
    collapsed = _between(CSS, ".fm-world .world-hud-island .world-hud-island-feed #fullChatMessages {", "\n}")
    assert "max-height: 48px;" in collapsed
    assert "overflow: hidden;" in collapsed
    expanded = _between(
        CSS,
        '.fm-world .world-hud-island[data-expanded="true"] .world-hud-island-feed #fullChatMessages {',
        "\n}",
    )
    assert "overflow-y: auto;" in expanded
    assert "340px" in expanded
    expand = _between(APP, "  setHudIslandExpanded(expanded) {", "\n  pinHudIslandFeed()")
    assert 'toggle?.setAttribute("aria-expanded", String(active));' in expand
    assert "this.dataset.worldIslandExpanded = String(active);" in expand
    assert "handleQuickChatEscape" in APP
    assert '!event.target.closest("[data-world-hud-island]")' in APP



def test_feed_rows_beat_the_roomier_native_chat_rules():
    row = _between(CSS, ".fm-world .world-hud-island .world-hud-island-feed .chat-message-row {", "\n}")
    assert "grid-template-columns: 20px minmax(0, 1fr);" in row
    assert "padding: 2px 4px;" in row
    avatar = _between(CSS, ".fm-world .world-hud-island .world-hud-island-feed .chat-message-avatar {", "\n}")
    assert "width: 20px;" in avatar and "height: 20px;" in avatar
    text = _between(CSS, ".fm-world .world-hud-island .world-hud-island-feed .chat-message-bubble p {", "\n}")
    assert "font: 400 13px/20px var(--font-sans);" in text
    assert ".fm-world .world-hud-island-feed .chat-message-row {" not in CSS
    assert CSS.index(".fm-world .world-native-chat .chat-message-row {") > CSS.index(
        ".fm-world .world-hud-island .world-hud-island-feed .chat-message-row {"
    )


def test_island_can_be_dragged_nudged_and_put_back():
    assert ISLAND.index("data-world-island-grip") < ISLAND.index("data-world-avatar-actions")
    assert 'const WORLD_HUD_ISLAND_OFFSET_KEY = "forkmesh.world.hudIslandOffset.v1";' in APP
    drag = _between(APP, "  bindHudIslandDrag(island) {", "\n  setHudIslandExpanded(")
    assert "island.setPointerCapture?.(pointerId);" in drag
    assert "ArrowUp: [0, -WORLD_HUD_ISLAND_STEP]," in drag
    assert 'event.key === "Home"' in drag
    assert 'island.addEventListener("dblclick"' in drag
    assert "this.hudIslandBoundsObserver.observe(bounds);" in drag
    apply = _between(APP, "  applyHudIslandOffset(wanted = this.hudIslandOffset) {", "\n  setHudIslandOffset(")
    assert 'island.style.setProperty("--world-hud-island-x", `${x}px`);' in apply
    assert 'island.dataset.placement = placement;' in apply
    assert "this.hudIslandBoundsObserver?.disconnect();" in APP
    assert '.fm-world .world-hud-island[data-placement="top"] :is(.world-hud-channel-menu, .world-hud-model-menu, .world-hud-runners) {' in CSS
    phone = _between(CSS, "@media (max-width: 600px) {\n  .fm-world {\n    --world-hud-island-gap", "\n}\n")
    assert ".fm-world .world-hud-island-grip {\n    display: none;" in phone

def test_status_pill_keeps_only_the_online_count():
    assert 'aria-label="Who is online"' in STATUS
    assert "data-world-online-count" in STATUS
    for removed in (
        "data-world-mcp-prompt",
        "data-world-notifications-open",
        "data-world-admin-errors",
        "data-world-tasks-open",
        "world-prompt-button",
    ):
        assert removed not in STATUS
        assert removed not in APP.replace("data-world-admin-errors-open", "").replace(
            "data-world-admin-errors-panel", ""
        ).replace("data-world-admin-errors-refresh", "")


def test_polling_that_only_fed_the_removed_counters_is_gone():
    for removed in (
        "startAdminErrorPolling",
        "refreshAdminErrors()",
        "ADMIN_ERROR_POLL_MS",
        "adminErrorTimer",
        "copyMcpTaskPrompt",
        "/api/world/admin/errors?after=${",
    ):
        assert removed not in APP
    assert "taskCounts" not in TASKS
    assert "if (!taskAssignee || !taskTeam) return;" in CHAT


def test_agent_targets_split_desktop_from_runner_pool_and_prefer_online():
    result = _run_helpers(
        """
        const targets = [
          { node: "old-desktop", providers: ["claude-code"], online: false, runtimeMode: "desktop" },
          { node: "jett-desktop", providers: ["claude-code", "codex"], online: true, runtimeMode: "desktop" },
          { node: "fmnext-r1", providers: ["claude-code"], online: true, runtimeMode: "runner-pool" },
          { node: "idle", providers: [], online: true, runtimeMode: "headless" },
        ];
        process.stdout.write(JSON.stringify({
          desktop: worldAgentTargetFor(targets, "desktop")?.node,
          runner: worldAgentTargetFor(targets, "runner")?.node,
          none: worldAgentTargetFor([], "runner"),
          headless: worldAgentTargetKind({ runtimeMode: "headless" }),
        }));
        """
    )
    assert result == {
        "desktop": "jett-desktop",
        "runner": "fmnext-r1",
        "none": None,
        "headless": "runner",
    }


def test_model_list_comes_from_the_target_and_remembers_the_last_choice():
    result = _run_helpers(
        """
        const models = { "claude-code": ["opus", "sonnet"], codex: ["sol"], "local-llama": ["gemma4:e4b"] };
        const runner = { providers: ["claude-code"] };
        const desktop = { providers: ["claude-code", "codex"] };
        const runnerOptions = worldAgentModelOptions(models, runner);
        const desktopOptions = worldAgentModelOptions(models, desktop);
        process.stdout.write(JSON.stringify({
          runner: runnerOptions.map((option) => option.value),
          desktop: desktopOptions.map((option) => [option.provider, option.model, option.label]),
          fallback: worldAgentModelOptions(null, null).length,
          defaultPick: worldAgentPickModel(desktopOptions, ""),
          remembered: worldAgentPickModel(desktopOptions, "codex:sol"),
          unsupported: worldAgentPickModel(runnerOptions, "codex:sol"),
        }));
        """
    )
    assert result["runner"] == ["claude-code:opus", "claude-code:sonnet"]
    assert result["desktop"] == [
        ["claude-code", "opus", "Opus 5.5"],
        ["claude-code", "sonnet", "Sonnet 5"],
        ["codex", "sol", "GPT-5.6 Sol"],
    ]
    assert result["fallback"] == 9
    assert result["defaultPick"] == "claude-code:opus"
    assert result["remembered"] == "codex:sol"
    assert result["unsupported"] == "claude-code:opus"


def test_repository_choices_and_run_states_are_bounded_and_clear():
    result = _run_helpers(
        """
        process.stdout.write(JSON.stringify({
          repos: worldAgentRepositoryChoices(["Jett/app", "jett/app", "bad", "forkmesh/forkmesh", ""]).map((r) => r.display),
          path: worldAgentSessionsPath({ owner: "jett", name: "my.app" }),
          states: [
            worldAgentRunState("queued", "ready_for_claim"),
            worldAgentRunState("queued", "waiting_for_desktop"),
            worldAgentRunState("security_pending"),
            worldAgentRunState("running"),
            worldAgentRunState("completed"),
            worldAgentRunState("rejected"),
          ],
          copy: worldAgentRunCopy({ node: "fmnext-r1", state: "waiting" }),
        }));
        """
    )
    assert result["repos"] == ["jett/app", "forkmesh/forkmesh"]
    assert result["path"] == "/api/repo/jett/my.app/agent-sessions"
    assert result["states"] == ["queued", "waiting", "queued", "running", "done", "failed"]
    assert result["copy"] == "Queued · starts when fmnext-r1 reconnects"


def test_agent_mode_intercepts_send_and_posts_to_the_owner_queue():
    bind = _between(APP, "  bindHudIsland(island) {", "\n  setWorldRightRailExpanded(")
    assert 'island.dataset.composerMode === "agent"' in bind
    assert bind.count("event.stopImmediatePropagation();") == 3
    assert bind.count("\n      true,\n    );") == 3
    assert 'island.addEventListener(\n      "paste",' in bind
    assert "Agent prompts from the World are text only." in bind
    send = _between(APP, "  async sendAgentPrompt() {", "\n  showAgentRun(")
    assert "worldAgentSessionsPath(repo)" in send
    assert "provider: choice.provider," in send
    assert "model: choice.model," in send
    assert "targetNode: target.node," in send
    assert "this.rememberAgentComposer({ repo: repo.display, model: choice.value });" in send
    assert "WORLD_AGENT_MAX_PROMPT" in send
    run = _between(APP, "  showAgentRun(payload, { repo, target, choice, path }) {", "\n  renderAgentRun()")
    assert "href: `/${repo.display}/agents`," in run
    assert "this.notifyChatArea(" in run
    watch = _between(APP, "  watchAgentRun() {", "\n  stopAgentRunWatch()")
    assert "WORLD_AGENT_RUN_POLLS" in watch
    assert "WORLD_AGENT_RUN_POLL_MS" in watch


def test_agent_targets_load_lazily_once_per_repository_and_need_sign_in():
    mode = _between(APP, "  setComposerMode(mode) {", "\n  syncComposerCopy()")
    assert "this.sessionAuthenticated && validWorldSession()" in mode
    assert 'this.toggleWorldAccount(true, "login");' in mode
    assert "void this.loadAgentTargets();" in mode
    load = _between(APP, "  async loadAgentTargets({ force = false } = {}) {", "\n  renderAgentControls()")
    assert "WORLD_AGENT_TARGETS_TTL_MS" in load
    assert "this.agentTargetCache.set(key, { pending, loadedAt: 0 });" in load
    assert APP.count("this.loadAgentTargets(") == 6
    assert 'const WORLD_AGENT_COMPOSER_KEY = "forkmesh.world.agentComposer.v1";' in APP
    assert "localStorage.setItem(WORLD_AGENT_COMPOSER_KEY" in APP


def test_runner_notice_names_the_runner_that_needs_a_sign_in():
    result = _run_helpers(
        """
        const now = 10_000_000;
        const expired = [{
          node: "fmnext-n1", queueNode: "fmnext-n1", runner: "fmnext-r1",
          providers: [], needsLogin: { "claude-code": ["fmnext-r1"] },
          online: true, lastSeenAt: now - 60_000, runtimeMode: "runner-pool",
          runners: [{ name: "fmnext-r1", online: true, lastSeenAt: now - 60_000,
            ready: [], needsLogin: ["claude-code"], signIn: [] }],
        }];
        const stale = JSON.parse(JSON.stringify(expired));
        stale[0].runners[0].online = false;
        stale[0].runners[0].lastSeenAt = now - 3 * 3600_000;
        const unsigned = [{ node: "fmnext-n1", providers: [], runtimeMode: "runner-pool",
          runners: [{ name: "new-box", online: true, lastSeenAt: now, ready: [], needsLogin: [], signIn: ["codex"] }] }];
        const bare = [{ node: "fmnext-n1", providers: [], runtimeMode: "runner-pool",
          runners: [{ name: "bare-box", online: true, lastSeenAt: now, ready: [], needsLogin: [], signIn: [] }] }];
        const offline = [{ node: "fmnext-r1", runner: "fmnext-r1", providers: ["claude-code"],
          online: false, lastSeenAt: now - 20 * 60_000, runtimeMode: "runner-pool" }];
        const ready = [{ node: "fmnext-r1", providers: ["claude-code"], online: true, runtimeMode: "runner-pool" }];
        const desktopOnly = [{ node: "laptop", providers: ["claude-code"], online: true, runtimeMode: "desktop" }];
        process.stdout.write(JSON.stringify({
          expired: worldAgentRunnerNotice(expired, "forkmesh/forkmesh", now),
          stale: worldAgentRunnerNotice(stale, "forkmesh/forkmesh", now).text,
          unsigned: worldAgentRunnerNotice(unsigned, "x/y", now),
          bare: worldAgentRunnerNotice(bare, "x/y", now).action,
          offline: worldAgentRunnerNotice(offline, "x/y", now),
          ready: worldAgentRunnerNotice(ready, "x/y", now),
          none: worldAgentRunnerNotice(desktopOnly, "forkmesh/forkmesh", now),
          age: [worldRelativeAge(0, now), worldRelativeAge(now - 5000, now),
                worldRelativeAge(now - 30 * 60_000, now), worldRelativeAge(now - 5 * 3600_000, now),
                worldRelativeAge(now - 4 * 86400_000, now)],
        }));
        """
    )
    assert result["expired"] == {
        "text": "fmnext-r1 needs Claude reconnected.",
        "tone": "bad",
        "action": {
            "label": "Connect Claude",
            "href": "/dashboard/runners?runner=fmnext-r1&provider=claude-code&connect=1",
        },
    }
    assert result["stale"] == (
        "fmnext-r1 needs Claude reconnected. It is offline, last seen 3 h ago."
    )
    assert result["unsigned"]["text"] == "new-box needs Codex signed in."
    assert result["unsigned"]["action"]["href"].endswith(
        "runner=new-box&provider=codex&connect=1")
    assert result["bare"] == {"label": "Manage", "href": "/dashboard/runners"}
    assert result["offline"] == {
        "text": "fmnext-r1 is offline, last seen 20 min ago. The prompt waits until it reconnects.",
        "tone": "muted",
        "action": {"label": "Runners", "panel": True},
    }
    assert result["ready"] is None
    assert result["none"] == {
        "text": "No runner yet for forkmesh/forkmesh —",
        "tone": "bad",
        "action": {"label": "Set one up", "href": "/dashboard/runners?setup=1"},
    }
    assert result["age"] == ["never", "just now", "30 min ago", "5 h ago", "4 days ago"]


def test_runner_panel_rows_show_login_state_jobs_and_shared_runners():
    result = _run_helpers(
        """
        const now = 10_000_000;
        const runners = [
          { name: "fmnext-r1", scope: "pool", status: "active", lastSeenAt: now - 30_000, activeLeases: 1,
            availability: {
              "claude-code": { binaryFound: true, loginState: "expired", auth: { state: "expired" } },
              codex: { binaryFound: false, loginState: "missing" },
            } },
          { name: "jett-box", scope: "personal", status: "active", lastSeenAt: now - 3600_000, activeLeases: 0,
            availability: {
              "claude-code": { binaryFound: true, loginState: "available", auth: { state: "expiring", expiresAt: now + 5 * 3600_000 } },
              codex: { binaryFound: true, loginState: "available" },
            } },
          { name: "old-box", scope: "personal", status: "revoked", lastSeenAt: 0, availability: {} },
        ];
        const targets = [{ node: "core1", providers: [], runtimeMode: "runner-pool",
          runners: [{ name: "shared-r2", online: true, lastSeenAt: now, ready: ["claude-code"], needsLogin: ["codex"], signIn: [] }] }];
        const rows = worldRunnerRows(runners, targets, now);
        process.stdout.write(JSON.stringify({
          names: rows.map((row) => [row.name, row.online, row.manage, row.jobs]),
          r1: rows.find((row) => row.name === "fmnext-r1").providers.map((p) => [p.label, p.state, p.text, p.connect]),
          box: rows.find((row) => row.name === "jett-box").providers.map((p) => [p.state, p.text]),
          shared: rows.find((row) => row.name === "shared-r2").providers.map((p) => [p.state, p.connect]),
          unknown: worldRunnerProviderState(undefined),
        }));
        """
    )
    assert result["names"] == [
        ["fmnext-r1", True, True, 1],
        ["shared-r2", True, False, 0],
        ["jett-box", False, True, 0],
    ]
    assert result["r1"] == [
        ["Claude", "expired", "Needs sign-in", True],
        ["Codex", "missing", "Not installed", False],
    ]
    assert result["box"] == [["expiring", "Signed in · expires in 5 h"], ["ok", "Signed in"]]
    assert result["shared"] == [["ok", False], ["expired", False]]
    assert result["unknown"] == {"state": "unknown", "text": "Not reported", "connect": False}


def test_runner_attention_chip_summarises_the_endpoint():
    result = _run_helpers(
        """
        process.stdout.write(JSON.stringify({
          none: worldRunnerAttentionSummary({ items: [] }),
          bad: worldRunnerAttentionSummary({ items: [{ runner: "../x", provider: "claude-code" }] }),
          one: worldRunnerAttentionSummary({ items: [{
            runner: "fmnext-r1", provider: "claude-code", state: "expired",
            connectUrl: "/dashboard/runners?runner=fmnext-r1&provider=claude-code&connect=1" }] }),
          foreign: worldRunnerAttentionSummary({ items: [{
            runner: "fmnext-r1", provider: "codex", state: "expiring", connectUrl: "https://evil.test/" }] }),
          two: worldRunnerAttentionSummary({ items: [
            { runner: "a-box", provider: "codex", state: "expiring" },
            { runner: "b-box", provider: "claude-code", state: "expired" }] }),
        }));
        """
    )
    assert result["none"] is None and result["bad"] is None
    assert result["one"] == {
        "text": "fmnext-r1 needs Claude sign-in",
        "state": "expired",
        "href": "/dashboard/runners?runner=fmnext-r1&provider=claude-code&connect=1",
        "count": 1,
    }
    assert result["foreign"]["href"] == (
        "/dashboard/runners?runner=fmnext-r1&provider=codex&connect=1")
    assert result["foreign"]["text"] == "fmnext-r1: Codex sign-in expires soon"
    assert result["two"]["text"] == "a-box: Codex sign-in expires soon · +1"
    assert result["two"]["state"] == "expired"


def test_runners_entry_panel_and_attention_chip_are_in_the_hud():
    agent = _between(ISLAND, "data-world-agent-controls hidden", "data-world-agent-run ")
    assert agent.index("data-world-agent-targets") < agent.index("data-world-runners-toggle")
    assert 'aria-controls="world-runners-panel"' in agent
    assert 'role="dialog" aria-label="Runners" data-world-runners-panel hidden' in agent
    assert "data-world-runners-setup" in agent and "WORLD_RUNNERS_SETUP_PAGE" in agent
    assert "data-world-runners-manage" in agent
    assert "data-world-agent-status-action" in ISLAND
    assert "data-world-runner-attention" in APP
    assert "data-world-runner-attention" not in STATUS
    bind = _between(APP, "  bindHudIsland(island) {", "\n  setWorldRightRailExpanded(")
    assert "this.setRunnersPanelOpen(" in bind
    hydrate = _between(APP, "  applyWorldTicketIdentity(ticket) {", "\n  clearWorldTicketIdentity()")
    assert "this.startRunnerAttention();" in hydrate
    clear = _between(APP, "  clearWorldTicketIdentity() {", "\n  applyWorldActivityTicket(")
    assert "this.stopRunnerAttention();" in clear
    start = _between(APP, "  startRunnerAttention() {", "\n  scheduleRunnerAttention()")
    assert "WORLD_RUNNER_ATTENTION_FIRST_MS" in start
    assert 'const WORLD_RUNNER_ATTENTION_PATH = "/api/runners/attention";' in APP
    assert ".world-hud-runners {" in CSS and ".world-runner-attention {" in CSS
    render = _between(APP, "  renderAgentControls() {", "\n  async sendAgentPrompt()")
    assert "worldAgentRunnerNotice(loaded.targets, repo.display)" in render
    assert "No ForkMesh runner can run agents" not in APP
