# Side rail, terminal tabs and the side browser

On 2026-10-24 the desktop's right-hand team rail became a generic **side rail**.
It no longer lists special agents by itself; it holds a terminal button, a
browser button and whatever the user drags onto it. Terminals open as tabs in
a **terminal pane** on the right of the window (since 2026-10-24, before that
on a full Terminals page), and agents (including Claude Code or Codex started
in one of those tabs) can read and type into them through the ForkMesh MCP
server, the way Claude Code's desktop app lets a session drive its own panes.

## Side rail

Code: `desktop/src/MainWindowSideRail.cpp` (widgets and menus) and
`desktop/src/SideRail.*` (the item model, tested by
`forkmesh-side-rail-tests`). Styles: `10-shell.qss` in Next, `Theme.h` in
Classic.

- **Where.** `#sideRail` sits at the right of the window in the slot the team
  rail used, after the channel status rail. It hides below 1000 px of window
  width, in reduced view modes, and when turned off with the top-bar panel
  button (`topSideRailButton`), the rail's own menu or **Settings > Appearance
  > Show the side rail on the right** (`ui/showSideRail`; the first read falls
  back to the old `ui/showTeammateRail`).
- **Fixed buttons.** At the top: **Terminal** (`sideRailTerminalButton`,
  shows or hides the terminal pane, opening a shell when none is open) and
  **Browser** (`sideRailBrowserButton`, toggles the side browser). Under a hairline come the items, then a dashed
  **+** tile (`sideRailAddButton`) whose menu adds a workflow from the current
  repository, a page from the left rail or a link, and offers **Open
  Workflows** and **Hide the side rail**.
- **Items** (`siderail::Item`, kinds `workflow`, `page`, `link`; at most 40):
  - *Workflow* (including special agents, which are workflows): its icon with
    the same blue running ring and red failure ring as other pins. Click runs
    it (`runPinnedWorkflow`, trigger "By hand · right rail"), right-click is
    the usual pin menu (stop, open in Workflows, agent home, last output,
    move to another rail, unpin).
  - *Page*: a left-rail entry by its `forkmeshRailKey`; click presses that
    rail button, so it opens the same page with the same side effects.
  - *Link*: an `http`/`https` address without credentials; click opens it in
    the side browser. Its menu also opens it in the system browser or copies
    it.
  - Page and link items have **Move up**, **Move down** and **Remove from the
    side rail**.
- **Drag and drop.** The item area accepts:
  - `application/x-forkmesh-workflow` (`{"repo","id"}` JSON) from a card on
    the Workflows page (`WorkflowCard` in the Workflows package, which also
    adds the pin format so a card drops on the strips); an account workflow
    uses the repository key `@account/workflows` and runs through the page
    ([../app/workflows.md](../app/workflows.md#one-list-on-the-desktop));
  - `application/x-forkmesh-workflow-pin` (`repo\nid`) from a pin dragged off
    another strip (`startWorkflowPinDrag`); during that drag the side rail
    shows the same dashed outline as the strips. A workflow dragged off the
    side rail carries this format too, so it can be dropped on the top, left
    or bottom strip;
  - `application/x-forkmesh-rail-item` from the left rail (the drag that also
    reorders and pops out pages);
  - web URLs or a dragged web address;
  - `application/x-forkmesh-side-rail-item` from the side rail itself, which
    reorders.
  An accent line shows where the item lands. Files, `file:` and `javascript:`
  links and plain text are refused.
- **Storage.** The order lives in `ui/sideRailItems` (JSON). Workflows are
  also pins: dropping one pins it to the right rail in `ui/workflowPins`
  (the same list the Workflows page edits), and `siderail::withWorkflows`
  drops stored workflow items that are no longer pinned right and appends
  right-rail pins that were made elsewhere. The separate right-hand pin strip
  is gone; right-rail pins show only in the side rail.
- **Refreshes.** Rebuilds are coalesced (150 ms) and wait while a popup is
  open or a side-rail item is being dragged, and pin strips do not rebuild
  during a pin drag (`m_workflowPinDragging`), so a drop never deletes the
  widget the drag started from. A pin that is already on a strip moves to the
  side rail even when its repository is not open here; a new workflow needs
  its checkout.

### What happened to the team rail

The special agent chips, their hover controls and the "create needed special
agent" tile left the rail. Special agents are still on the Special Agents page
(video tiles and cards keep the run, mood, menu and payout buttons), and any of
them can be put on the side rail by dragging its row from the Workflows page or
choosing **Pin to rail > Right**. **Create needed special agent** stays in
each special agent's menu (`showTeammateRailMenu`) on the Special Agents page
and its home pages. `restoreTeammateRunState` keeps
loading the finder sessions and done-unseen runs, and its 30 s clock still
refreshes the Special Agents home pages.

## Side browser

`#sideRailBrowserPanel` opens to the left of the rail with a
`WorkspaceBrowserPane` (the same pane as the Agents workspace's Browser tab,
library `forkmesh-browser-pane`). When the build finds Qt WebEngine pages
render inside it with Google's background services switched off
([browser.md](browser.md)); without it the pane shows the address and opens
pages in the system browser. Only `http` and `https` load. The panel's width is
dragged on its left edge (280 to 1200 px, `ui/sideRailBrowserWidth`, default
420); whether it is open and the last address are kept in
`ui/sideRailBrowserOpen` and `ui/sideRailBrowserUrl`. Agents keep using their
own browser windows (`open_browser_window`); the side browser is the user's.

## Terminal pane and launcher

Code: `desktop/src/MainWindowTerminals.cpp` (opening, launcher, agent
requests), `MainWindowSideRail.cpp` (the pane, `revealTerminals`,
`setSideRailTerminalsOpen`), `TerminalTabsPage.*` (tabs and header),
`TerminalBridge.*` (request parsing, key names, launch commands) and
`TerminalWidget` (the existing pty terminal, which gained `sendInput`,
`plainText` and an `outputArrived` signal).

- **One click.** The top bar's terminal button (in Next `topTerminalButton`,
  between the theme toggle and the side-rail toggle; in Classic a title-bar
  button with the `terminalLauncherButton` property, before the view
  switcher) and the side rail's Terminal button call `toggleTerminals`: with
  no tab open they open a shell straight away in the pane; with tabs open
  they show the pane, or hide it when it is showing. Hidden tabs keep
  running. The user asked for a terminal that opens at once beside their
  work, like an editor's terminal panel, rather than a dialog and a separate
  page.
- **Pane.** `#sideRailTerminalPanel` sits between the side browser and the
  rail's button column. Its width is dragged on its left edge
  (`#sideRailTerminalHandle`, 280 to 1200 px, `ui/sideRailTerminalWidth`,
  default 560). The header has **Shared with agents** / **Private** for the
  current tab, **+** (`terminalsNewButton`, a new shell in the current tab's
  folder), a menu (`terminalsNewKindButton`) with **Shell**, **Claude Code**,
  **Codex** and **Command or folder…** (the launcher), and **×** to hide the
  pane. A line under it shows the last thing an agent did in the tab. Tabs
  are closable and movable, at most 12; closing the last one hides the pane.
  A shell tab `exec`s the user's shell; the other kinds drop to an
  interactive shell when their program exits, so the tab stays usable.
- **Default folder.** A new tab from the top bar or rail opens in the current
  repository's checkout, else the launcher's last folder, else home; **+**
  and the menu use the current tab's folder.
- **Narrow windows.** The rail hides below 1000 px and in reduced view
  modes. Then `revealTerminals` moves the same `TerminalTabsPage` (running
  tabs and all) into section `kTerminalsSectionIndex` (30, no left-rail
  entry) and shows it; the next reveal with room moves it back, and the
  section then shows a note pointing at the pane.
- **Launcher.** `QDialog#terminalLauncher` (non-modal, from the pane's menu
  or the rail's **Open a terminal with options…**): **Run** shell, Claude
  Code, Codex or a command; **In** the selected agent's worktree, the current
  repository, any local checkout or home; **Share with agents**. The last
  choices are remembered under `terminals/`.
- **Environment.** Every tab gets `FORKMESH_AGENT_INTAKE` (the desktop's agent
  socket), its own `FORKMESH_AGENT_INTAKE_TOKEN` (`term.<random>`, kept in
  `m_terminalIntakeTokens` and dropped when the tab closes) and
  `FORKMESH_TERMINAL_ID`. When the socket is unavailable those variables are
  removed instead of inherited.
- **Claude Code tabs** run `claude --mcp-config <file>`, where the file
  (`<data>/terminals/mcp-<id>.json`, mode 0600, removed with the tab) is
  `forkmesh::mcp::configJson` plus the three variables above in the server's
  `env`, because MCP clients do not pass their whole environment to stdio
  servers. That Claude Code session therefore has the ForkMesh MCP tools,
  including the terminal tools, without editing `.mcp.json`.
- **Codex tabs** get the same file. Codex starts MCP servers with a scrubbed
  environment and has no `--mcp-config`, so the tab runs
  `codex -c 'mcp_servers.forkmesh.command="<python3>"' -c
  'mcp_servers.forkmesh.args=["<forkmesh_mcp_server.py>", "--env-file",
  "<data>/terminals/mcp-<id>.json"]'` (`launchCommand`, values written by
  `tomlString`), and the server's `--env-file` copies the `FORKMESH_*` values
  from that file's `mcpServers.forkmesh.env` into its environment
  (`load_env_file`), so no token appears on a command line. On Windows a
  Codex tab runs plain `codex`.
- **Agent runs** (the Agents page's Claude Code runs) get the same file with
  their own session token (`<data>/core/agent-mcp/session-<id>.json`), so an
  agent can open and drive a tab without the user approving `.mcp.json`
  first ([../agents/tools.md](../agents/tools.md#mcp-servers)).

## Terminal tools for agents

Intake methods (`terminals.*`, `TerminalBridge.h`), answered by
`MainWindow::handleTerminalRequest`; MCP tools in
`tools/forkmesh_mcp_server.py` (`TERMINAL_TOOLS`, tests in
`tools/test_forkmesh_mcp_terminals.py`):

| MCP tool | Method | What it does |
| --- | --- | --- |
| `list_terminals` | `terminals.list` | Shared tabs: `terminal`, `title`, `kind`, `cwd`, `running`, `exit_code`, `cols`, `rows`, `current`, `idle_ms`, `opened_by_agent`, `self`; `private_terminals` counts hidden ones |
| `open_terminal` | `terminals.open` | New tab: `kind`, `command` (kind `command` only), `repository`, `title`; default folder is the agent's worktree or the calling tab's folder |
| `read_terminal` | `terminals.read` | The last `lines` (1-2000, default 200) of scrollback and screen as text |
| `type_in_terminal` | `terminals.input` | `text` (8000 characters, newlines become Enter), then `keys` (enter, tab, escape, arrows, home, end, page keys, delete, backspace, space, `ctrl-<letter>`), then Enter if `enter` |
| `run_in_terminal` | read, input, list, read | Types a command and Enter (opening a shell tab when no `terminal_id` is given), waits until the tab is quiet for `quiet_ms` (default 1200) or `timeout_seconds` (default 30, at most 300) pass, and returns the new output and `settled` |
| `close_terminal` | `terminals.close` | Closes a tab an agent opened |

Rules:

- Callers are agent sessions the desktop launched (their session token) or a
  program running in a ForkMesh terminal tab (that tab's `term.` token, which
  only opens `terminals.*`). Anything else gets `unauthorized`.
- An agent below Auto mode (`agenthub::permissionRank` < 3) may list and read
  but not open, type or close (`mode_too_low`), because typing runs commands
  with the user's account.
- Private tabs are left out of the list and refuse reads and input
  (`not_shared`), except the caller's own tab.
- Tabs the user opened are not closed for an agent (`user_terminal`).
- At most 120 inputs a minute and 10 agent-opened tabs every 10 minutes per
  desktop; input to a tab whose program exited is refused (`not_running`).
- Like the window and hub tools, the terminal tools need the connector token
  once the node publishes an MCP connector.
- An agent-opened tab does not open the pane or switch the user's page; a
  notice names the agent.
