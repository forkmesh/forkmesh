# MCP connector: desktop windows, browser and agent tools

The Claude Code desktop app gives a session more than the CLI: it suggests
new sessions for side work, lists and messages other sessions and
environments, sends push notifications, drives a built-in browser and opens
panels and widgets beside the chat. ForkMesh runs `claude` itself
(stream-json, [agent tools](../agents/tools.md)), so those desktop-only tools
do not exist in its runs. The ForkMesh MCP server
(`tools/forkmesh_mcp_server.py`) provides the same abilities, plus small
windows like MCP Apps in chat clients, over the agent-intake socket to any
agent (Claude Code or another MCP client) the desktop launched. Part of
[the tools docs](README.md); the socket and session token are in
[mcp-create-agent.md](mcp-create-agent.md).

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

The MCP server is what the model sees; the intake socket is the
authenticated per-agent path back to the desktop. The window tools
(`WINDOW_TOOLS`: `show_window`, `read_window`, `close_window`,
`open_browser_window`, `read_browser_page`, `browser_action`, `show_widget`
and `suggest_session`) map to the intake methods `windows.show`,
`windows.read`, `windows.close`, `windows.browse`, `windows.capture`,
`windows.act`, `windows.widget` and `sessions.suggest`. The desktop draws
content windows as Qt tool windows with a `QTextBrowser`
(`desktop/src/AgentWindows.*`), owned by the main window; browser windows use
`QWebEngineView` when the build has WebEngine and the system browser
otherwise.

## Access and tokens

Every tool needs `FORKMESH_AGENT_INTAKE` and `FORKMESH_AGENT_INTAKE_TOKEN`
(set only for agents the desktop launched, never in the KVM workspace) and,
once the node publishes an MCP connector, a matching `FORKMESH_MCP_TOKEN`: the
window tools through `WINDOW_TOOLS`, the terminal tools through
`TERMINAL_TOOLS`, the others through `HUB_TOOLS` (`require_write_access`). `list_agents` and `list_runners` are marked
read-only in `tools/list` but still need the token, because they describe
the user's other work. `whoami` reports `agentTools` and `terminalTools`.
The terminal tools also work from Claude Code started in a ForkMesh terminal
tab, which gets its own channel token; the tabs, the launcher and the rules
(Auto mode to type, private tabs, limits) are in
[side rail and terminals](../desktop/side-rail-and-terminals.md).

- **The per-agent intake token** (`FORKMESH_AGENT_INTAKE_TOKEN`) is sent in
  every request; `agentintake::authorizedSession()` checks it for every
  method (`MainWindowAgentIntake.cpp`) and refuses with `unauthorized`. The
  token, not the socket, identifies the agent, and only that agent's windows
  are visible to it.
- **The connector token.** Once the node publishes an MCP connector
  (`mcp/connector.json`), window tools need a matching `FORKMESH_MCP_TOKEN`
  (`require_write_access`), the reads included, because they return what the
  user typed or a page the user may be signed in to. `tools/list` still marks
  `read_window` and `read_browser_page` read-only (`WINDOW_READ_TOOLS`).
  `whoami` reports `windowTools` and `canOpenWindows`.

## Windows

`show_window`:

| Argument | Meaning |
| --- | --- |
| `title` | Required, at most 120 characters |
| `body` | Required, at most 40000 characters |
| `format` | `markdown` (default), `html` or `text` |
| `buttons` | Up to 6 distinct labels, each at most 40 characters |
| `input_label`, `input_multiline` | Adds a one-line or multi-line text field |
| `wait_seconds` | 0-600. With buttons or an input, the call polls until the user answers, closes the window, or the time runs out |
| `window_id` | Replace the content of a window this agent opened |
| `width`, `height` | 240-1400 by 160-1000; default 520 by 380 |

Every call returns `window_id`, `title`, `open`, `answered`, `has_input`,
`buttons`, and once answered `button` and `input`. An HTML link
`action:<label>` acts as a button press; other `http(s)`/`mailto` links open
in the browser. `read_window` returns the same state (optionally waiting by
polling `windows.read` every 0.5 s); `close_window` closes it.

## Safety

- Only the session that opened a window can read, update or close it;
  window ids are 64 random bits.
- At most 8 open windows per agent and 30 new windows per 10 minutes per
  desktop; closed windows keep their answer, up to 32 per agent.
- `QTextBrowser` runs no scripts and loads no remote resources; images load
  only from local files (relative to the agent's worktree) or
  `data:image/...`, at most 16 MiB. In-window link navigation is off.
- Windows open with `WA_ShowWithoutActivating`, so they never take the
  keyboard focus. Escape closes a content window.

## Browser windows

`open_browser_window` takes `url` (http or https with a host, no user info,
at most 2048 characters), `title`, `window_id` and `width`/`height` (default
900 by 640), sent as `windows.browse`. Typical use: a local dev server, a
deployed preview or a pull request.

- **Where.** The page opens as a tab in the **Agent pages** panel
  (`#agentPagesPanel`, `MainWindow::dockAgentPage`) on the right of the main
  window, beside the side browser, so it sits next to the agent instead of
  floating over it ([side rail](../desktop/side-rail-and-terminals.md#agent-pages)).
  `Registry::setBrowserDock` hands the frame to the window; `width` and
  `height` then do not apply. When the side rail cannot show (window under
  1000 px or a reduced view mode) the page gets its own window as before, and
  raising a docked page the panel can no longer show turns it into one.
- WebEngine is optional (`forkmesh_link_webengine`, `FORKMESH_HAVE_WEBENGINE`);
  with it `main()` sets `Qt::AA_ShareOpenGLContexts` first. Without it the URL
  goes to the system browser (`QDesktopServices::openUrl`) and the reply says
  `external: true`, `open: false`. The opener is injectable
  (`Registry::setExternalOpener`) for tests.
- The window has back, forward, reload, an address bar (other schemes are
  refused) and **Open in browser**.
- All browser windows share one off-the-record `QWebEngineProfile`: no
  cookies or cache on disk, nothing shared with the user's browser. Downloads
  are cancelled, main-frame navigation is http/https only, subframes may not
  load `file:`, popups load in the same window, and camera, microphone and
  location are never granted.

## Reading the page

`read_browser_page` takes `window_id`, `max_chars` (default 20000, at most
100000), `offset`, `include_links` and `screenshot`, and returns:

1. one JSON line: `window_id`, `url`, `title`, `total_chars`, `offset`,
   `truncated`, `selection` (up to 5000 characters), scroll and size values,
   `ready_state`, `console` (the last 200 console messages), `links` when
   asked (up to 200), `has_screenshot`;
2. the page's visible text (`document.body.innerText`);
3. with `screenshot`, a PNG `image` block at most 1280 px wide.

The capture script (`pageCaptureScript`) runs read-only in WebEngine's
isolated `ApplicationWorld`. Captures are asynchronous
(`Server::setAsyncHandler` with a deferred `Reply`), time out after 8 s
(`capture_timeout`), and the MCP server reads up to 24 MiB for this method.

User controls: each browser window has **Share this page with the agent**
(on by default; unticked, reads fail with `not_shared`), and the footer shows
when the agent last read the page. `innerText` leaves out form field values;
a screenshot shows the screen as is, with password fields masked.

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

## In the transcript

`show_window`, `open_browser_window` and `read_browser_page` render as
"Window", "Browser" and "Read page" cards with a one-line result and a
**Show window** link (`forkmesh-window:<id>`, handled by
`MainWindow::openAgentTranscriptReference`). Page text stays out of the
transcript. Tool naming and summaries for the transcript view live in
`desktop/src/ClaudeTranscriptTools.*` (tests
`desktop/tests/test_claude_transcript_tools.cpp`).

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
- `desktop/tests/test_agent_intake.cpp` `testAuthorization`: the per-agent
  token check.

The live WebEngine paths (a loaded widget sending events, actions on a real
page) have no automated test yet; `testCaptureLive` in
`test_agent_windows.cpp`, which runs only in builds with WebEngine, is the
pattern for one.
