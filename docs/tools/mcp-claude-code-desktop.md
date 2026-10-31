# MCP connector: Claude Code desktop features in ForkMesh

The Claude Code desktop app gives a session a few things beyond the CLI: it
offers to start a new session for side work, lists the other sessions and
environments it can reach and messages them, sends push notifications, drives
a built-in browser, and opens small panels and widgets next to the chat.
ForkMesh runs `claude` itself (stream-json, [../agents/tools.md](../agents/tools.md)),
so those desktop-only tools do not exist in its runs. The ForkMesh MCP server
(`tools/forkmesh_mcp_server.py`) provides the same abilities, backed by the
ForkMesh desktop over the agent-intake socket, so a ForkMesh agent (Claude
Code or any MCP client the desktop launched) gets them inside ForkMesh.

| Claude Code desktop | ForkMesh MCP tool | Intake method | Desktop code |
| --- | --- | --- | --- |
| "Start a new session?" chip for side work | `suggest_session` | `sessions.suggest` | `MainWindowAgentHub.cpp`, `AgentHub.*` |
| `ListAgents` (sessions on this machine, other machines, cloud) | `list_agents` | `agents.list` | `MainWindowAgentHub.cpp` |
| `SendMessage` to another session | `send_agent_message` | `agents.message` | `MainWindowAgentHub.cpp` |
| Environments: local, cloud, remote | `list_runners` | `agents.runners` | `MainWindowAgentHub.cpp`, `RunnerLogin.*` |
| `PushNotification` | `notify_user` | `desktop.notify` | `MainWindowAgentHub.cpp` |
| Built-in browser pane: navigate, click, type, read | `open_browser_window`, `browser_action`, `read_browser_page` | `windows.browse`, `windows.act`, `windows.capture` | `AgentWindows.*` |
| Terminal panes the session can read and drive | `list_terminals`, `open_terminal`, `read_terminal`, `type_in_terminal`, `run_in_terminal`, `close_terminal` | `terminals.list`, `terminals.open`, `terminals.read`, `terminals.input`, `terminals.close` | `MainWindowTerminals.cpp`, `TerminalBridge.*` |
| Panels, widgets and mods beside the chat | `show_window` (static), `show_widget` (scripted) | `windows.show`, `windows.widget` | `AgentWindows.*` |

All of them need `FORKMESH_AGENT_INTAKE` and `FORKMESH_AGENT_INTAKE_TOKEN`
(set only for agents the desktop launched, never in the KVM workspace) and,
once the node publishes an MCP connector, a matching `FORKMESH_MCP_TOKEN`: the
window tools through `WINDOW_TOOLS`, the terminal tools through
`TERMINAL_TOOLS`, the others through `HUB_TOOLS` (`require_write_access`). `list_agents` and `list_runners` are marked
read-only in `tools/list` but still need the token, because they describe
the user's other work. `whoami` reports `agentTools` and `terminalTools`. Window basics, safety
limits and the browser profile are in [mcp-agent-windows.md](mcp-agent-windows.md);
the session token and socket in [mcp-create-agent.md](mcp-create-agent.md).
The terminal tools also work from Claude Code started in a ForkMesh terminal
tab, which gets its own channel token; the tabs, the launcher and the rules
(Auto mode to type, private tabs, limits) are in
[../desktop/side-rail-and-terminals.md](../desktop/side-rail-and-terminals.md).

## Suggesting a new session

`suggest_session` takes `prompt` (required, 8000 characters), `title`,
`reason` (300), `repository` (default: the calling agent's), `provider`,
`model` and `wait_seconds`. The desktop checks the repository has a checkout,
then opens a window titled "Start a new session?" with the title, reason,
quoted prompt (first 1200 characters), repository and the calling agent, and
the buttons **Start session** and **Not now**.

- Nothing runs until the user presses **Start session**. The click goes
  through `createAgentFromIntake` (the `create_agent` path, so the same
  checkout, provider and rate-limit rules apply: 12 agents per 10 minutes per
  desktop, counted when the agent is created, not when it is suggested). The
  calling agent's transcript gets a notice naming the new agent.
- The window is a `suggestion` window (`WindowState::kind`): the agent can
  read and close it but not rewrite it (`locked`), so the text the user
  approves is the prompt that runs. The prompt is kept in
  `MainWindow::m_agentSuggestions`, not in the window.
- The reply and `read_window` carry `suggestion`: `offered`, `started` (with
  `session_id` and `branch`), `dismissed` or `failed` (with `error` and
  `message`). After an answer the window closes.
- `Registry::setAnswerHandler` reports every button press to the main
  window on the next event-loop turn; only suggestion windows act on it.
  Suggestions whose window the user closed without answering are dropped
  the next time the desktop records one.

The tool description tells the model to prefer it to `create_agent` when the
user did not ask for the work, which is how Claude Code's desktop offers side
tasks.

## Listing and messaging agents

`list_agents` returns the desktop's sessions (not outside CLI sessions,
deleted ones or association rows): `id`, `title`, `repository`, `branch`,
`state` (`agentIntakeSessionState`), `provider`, `model`, `mode`, `where`
(`desktop` or `cloud`), `self`, `running`, `can_message` and, when false,
`cannot_message` (`self`, `cloud`, `paused`, `merged`, `more_permissive`). By default
it lists running, queued and paused sessions plus the caller;
`include_finished` adds the rest. Active sessions sort first, then newest;
`limit` is 1-100 (default 30). Provider credentials are redacted from every
row (`redactProviderCredentials`).

`send_agent_message` takes `agent_id` and `message` (8000 characters) and
calls `sendPromptToAgentSession`, the same path as a follow-up typed in the
composer: a running agent gets it folded into its current turn, an idle one
resumes. Rules:

- Only sessions on this desktop; cloud runs (`cloud`), paused sessions
  (`paused`) and sessions whose work was merged (`merged`) are refused, and
  an agent cannot message itself.
- **Mode check.** The receiver's permission mode must not allow more than
  the sender's: Plan < Ask < Edit < Auto (`agenthub::permissionRank`). An
  unknown receiver mode counts as Auto and an unknown sender mode as Plan, so
  an agent in Plan or Ask mode cannot use an Auto agent to do what it may not
  do itself (`more_permissive`).
- The text arrives wrapped by `agenthub::messageEnvelope`: "Message from
  ForkMesh agent #N “title” (another agent, not the user):", the message,
  and how to reply. Both sessions' logs record the message.
- 30 messages per 10 minutes per desktop (`m_agentMessageLimiter`).

## Runners

`list_runners` refreshes the account's runner list (`refreshRunners`, the
same `/api/runners` call as Settings > Runners) and returns `name`, `scope`,
`status`, `last_seen_seconds_ago`, `active_jobs` and per provider
`installed`, `signed_in`, `state` and the active account's display name. It
answers `runners_unavailable` when the desktop is not signed in. The call is
asynchronous (`Server::setAsyncHandler`).

## Notifications

`notify_user` takes `title` (120) and `body` (1000). The desktop files a
ping (`flashPingMessage`) and, when ForkMesh is not the active window, a
system notification (`notifyIfInactive`); both open the agent
(`fm:agent:<id>`). 10 per 10 minutes per desktop.

## Browser actions

`browser_action` works on a browser window from `open_browser_window`:

| `action` | Arguments | What happens |
| --- | --- | --- |
| `navigate` | `url` (http/https, `browserUrlFromInput`) | Loads it; replies on `loadFinished` or after `wait_ms` |
| `back`, `forward`, `reload` | | History navigation; `no_history` when there is none |
| `click` | `selector` or `target` | Scrolls it into view, sends pointer and mouse down/up, focuses, `click()` |
| `type` | `selector` or `target`, `value`, `clear` (default true), `submit` | Sets the value and fires `input` and `change`; content-editable elements use `insertText`; a `<select>` picks the option whose value or text matches; `submit` sends Enter and submits the form |
| `press` | `value` (a key such as `Enter`, `Escape`) and optionally an element | Dispatches keydown/keypress/keyup; Enter in a form input submits it |
| `scroll` | `delta` pixels, or an element to scroll to | |
| `wait_for` | `selector` or `target`, `wait_ms` | Polls every 250 ms until the element exists; replies `found` |

`target` matches an element's visible text, labels, `aria-label`,
`placeholder`, `title`, `name` or `alt`, exact match first, then the first
partial match among visible candidates (form fields for `type`, clickable
elements otherwise). The script (`pageActionScript`) runs in WebEngine's
isolated `ApplicationWorld`, takes its arguments as one JSON literal, and is
the only code the agent can run in the page: there is no free-form
JavaScript evaluation.

- The **Share this page with the agent** box gates actions like reads
  (`not_shared`), and the footer shows the last thing the agent did there.
- Typing into `input[type=password]` is refused (`password_field`): the user
  signs in themselves.
- Every action replies within `wait_ms` (default and maximum 8000) plus a
  short margin (`action_timeout`); other actions wait 300 ms after the script
  so a click's navigation can start, then report `url` and `title`.
- Synthetic key events do not move focus the way a real Tab does; `click`
  the next field instead.

## Widgets

`show_window` stays the safe static window (`QTextBrowser`, no scripts).
`show_widget` is the scripted one: `title`, `html` (50000 characters),
`data` (any JSON value, 16000 characters encoded), `window_id`, `width`,
`height` (default 420 by 320) and `wait_seconds`.

- It needs WebEngine; without it the desktop answers `no_widget_engine` and
  the description points the model to `show_window`.
- The page is loaded with `setHtml` into its own off-the-record profile
  (`Registry::widgetProfile`) whose request interceptor (`WidgetInterceptor`)
  blocks everything except `data:`, `blob:` and `about:`: no network, no
  files, nothing shared with the browser windows' profile. Local storage,
  plugins and window opening are off. Clicked http(s) and mailto links open
  in the system browser; other navigations are refused.
- A script injected at document creation (`widgetBridgeScript`) defines a
  frozen `window.forkmesh` with `data`, `send(value)`, `onData(callback)`;
  updates also fire a `forkmesh-data` window event.
- `send` writes one console line prefixed `forkmesh-widget-event:` with the
  JSON value (at most 4000 characters); `WidgetPage` turns it into an event
  (`widgetEventFromConsole`). Up to 50 events wait per widget, the oldest
  dropped first (`dropped_events`). `read_window` returns `events` and clears
  them; `answered` is true while events wait, so `wait_seconds` returns on
  the first event.
- `show_widget` with `window_id` and only `data` updates a live widget
  (`widgetDataScript` calls the bridge) without reloading it; with `html` it
  replaces the page.

A widget only runs what its agent wrote and can reach nothing but the
desktop window it is in, so its events are as trustworthy as the agent's own
output, not as the user's answer to a `show_window` question.

## In the transcript

`ClaudeTranscriptTools` names the cards "Suggest session", "Widget",
"Browser action", "List agents", "Message agent", "List runners" and
"Notify". Suggestion results read "Asked the user to start a new session",
"The user started agent #N", "The user chose not to start it" or "Could not
start it: …"; actions read "Did click · <page title>"; widgets count their
events. Window cards keep the **Show window** link.

## Tests

- `desktop/tests/test_agent_hub.cpp` (`forkmesh-agent-hub-tests`): request
  parsing, the suggestion text, permission ranks, the message envelope.
- `desktop/tests/test_agent_windows.cpp`: widget and action parsing, the
  bridge and data scripts, the widget request filter, locked suggestion
  windows, the answer handler, bounded and drained widget events, the
  no-WebEngine refusals.
- `desktop/tests/test_claude_transcript_tools.cpp`: names, subtitles and
  summaries.
- `tools/test_forkmesh_mcp_agent_hub.py`, `tools/test_forkmesh_mcp_windows.py`
  and `tools/test_forkmesh_mcp_server.py`: parameters, waits, connector
  gating and the pinned tool list.

The live WebEngine paths (a loaded widget sending events, actions on a real
page) have no automated test yet; `testCaptureLive` in
`test_agent_windows.cpp`, which runs only in builds with WebEngine, is the
pattern for one.
