# MCP connector: agent windows (2026-10-01)

The owner asked for ForkMesh to be "integrated closely with Claude Code", with
the ability for an agent "to launch little windows in our app to do things like
it does in Claude Desktop". Claude Desktop renders MCP Apps (HTML UIs from an
MCP server) inside the conversation. Agents run as `claude` CLI processes and
content windows must work in every desktop build, including the ones without
Qt WebEngine (it is optional, see [Browser windows](#browser-windows-open_browser_window)),
so the same idea is built on what both sides already have:

- five tools on the ForkMesh MCP server (`tools/forkmesh_mcp_server.py`):
  `show_window`, `read_window`, `close_window`, and for browser windows
  `open_browser_window` and `read_browser_page` (`WINDOW_TOOLS`);
- the existing agent-intake socket ([mcp-create-agent.md](mcp-create-agent.md))
  carries five more methods, `windows.show`, `windows.read`, `windows.close`,
  `windows.browse` and `windows.capture`;
- the desktop draws content windows as small Qt tool windows with a
  `QTextBrowser` (`desktop/src/AgentWindows.*`), owned by the main window so
  they stay above it but never above other applications. Browser windows use
  `QWebEngineView` when the build has WebEngine and the system browser when it
  does not.

Claude Code only exposes `getDiagnostics`/`executeCode` from an IDE bridge to
the model, so a window tool on `ClaudeIdeBridge` would be invisible to it. The
MCP server is the surface the model sees, and the intake socket is already the
authenticated per-agent path from that server back to the desktop.

## The tools

`show_window`:

| Argument | Meaning |
| --- | --- |
| `title` | Required, at most 120 characters. The window title also names the agent. |
| `body` | Required, at most 40000 characters. |
| `format` | `markdown` (default), `html` or `text`. |
| `buttons` | Up to 6 distinct labels, each at most 40 characters. |
| `input_label`, `input_multiline` | Adds a one-line or multi-line text field. |
| `wait_seconds` | 0-600. With buttons or an input, the call polls until the user answers, closes the window, or the time runs out. Without controls it returns at once. |
| `window_id` | Replace the content of a window this agent opened (progress updates). The answer is cleared. |
| `width`, `height` | 240-1400 by 160-1000; default 520 by 380. |

Every call returns `window_id`, `title`, `open`, `answered`, `has_input`,
`buttons`, and once answered `button` and `input`. A field with no buttons gets
a Send button. An HTML link `action:<label>` acts as a button press. Other
`http(s)`/`mailto` links open in the browser when the user clicks them.

`read_window` returns the same state (optionally waiting); `close_window`
closes it. Waiting is done by the server polling `windows.read` every 0.5 s,
because the intake socket answers one request per connection within 10 s and
cannot hold a reply open.

## Tokens

Window tools need two tokens, checked on both sides of the socket:

- **The per-agent intake token.** The desktop mints `FORKMESH_AGENT_INTAKE_TOKEN`
  for each agent it launches (`MainWindow::agentIntakeEnv`) and the MCP server
  sends it in every request. Without both `FORKMESH_AGENT_INTAKE` and the token
  the server refuses before connecting. On the desktop,
  `agentintake::authorizedSession()` checks the token first, for every method,
  in the socket's handler (`MainWindowAgentIntake.cpp`), and a request without
  the right token gets `unauthorized` ("Only agents this ForkMesh desktop
  launched can open or read its windows."). A raw client of the socket cannot
  bypass the MCP server: the token, not the socket, identifies the agent, and
  only that agent's windows are visible to it.
- **The connector token.** Once the node publishes an MCP connector
  (`mcp/connector.json`), window tools need a matching `FORKMESH_MCP_TOKEN`,
  like the write and CRM tools; without it `require_write_access` refuses them
  before any request reaches the desktop. This covers the two reads as well,
  because `read_window` returns what the user typed and `read_browser_page`
  returns a page the user may be signed in to. `tools/list` still marks
  `read_window` and `read_browser_page` read-only (`WINDOW_READ_TOOLS`).

Until 2026-10-02 the window tools skipped the connector token on the grounds
that they change nothing in a repository and sign nothing, and the desktop's
refusal message for windows said "can create agents". `whoami` reports
`windowTools` and `canOpenWindows`, which is true only with the intake channel
and, when a connector exists, the connector token.

Tests: `tools/test_forkmesh_mcp_windows.py` (connector gating of all five
tools, the intake token carried to a fake desktop socket, refusal without it),
`tools/test_forkmesh_mcp_server.py` (refusal in read-only mode),
`desktop/tests/test_agent_intake.cpp` `testAuthorization` (raw `windows.*`
requests without the token or with a wrong one never reach the handler).

## Safety

- Every window request carries the agent's intake token and, with a published
  connector, the connector token (see [Tokens](#tokens)).
- Only the session that opened a window can read, update or close it; another
  agent's `window_id` gets `not_found`. Window ids are 64 random bits.
- At most 8 open windows per agent and 30 new windows per 10 minutes on one
  desktop. Closed windows keep their answer for reads; the oldest closed ones
  are dropped past 32 per agent.
- Rendering is `QTextBrowser`: no scripts. Remote resources never load
  (`localResourceAllowed`); images load only from local files (relative paths
  resolve against the agent's worktree) or `data:image/...`, at most 16 MiB.
  Link navigation inside the window is off.
- Windows open with `WA_ShowWithoutActivating`, so a window never takes the
  keyboard away from someone typing in the composer. Escape closes one.

## Browser windows (`open_browser_window`)

Asked for next: "let Claude open a browser window in a small window like this
too". `open_browser_window` takes `url` (required, http or https with a host,
no user name or password, at most 2048 characters), `title`, `window_id` and
`width`/`height` (default 900 by 640), and goes over the socket as
`windows.browse`. The typical use is showing the user a local dev server
(`http://localhost:3000`), a deployed preview or a pull request next to the
agent.

The page is drawn by Qt WebEngine (`QWebEngineView`), which the build links
only when it is installed (`forkmesh_link_webengine`, `FORKMESH_HAVE_WEBENGINE`;
`qt6-webengine-dev` on Debian/Ubuntu). It stays optional because it is a
large Chromium-based module that many Qt installs leave out, the same reason
Multimedia is optional. With it, `main()` sets `Qt::AA_ShareOpenGLContexts`
before the `QApplication` is built, as WebEngine requires.

- The window has back, forward, reload, an editable address bar (a typed host
  gets `https://`, `localhost`/`127.*` gets `http://`, anything with another
  scheme is refused) and **Open in browser**.
- All browser windows share one off-the-record `QWebEngineProfile` owned by the
  registry: no cookies or cache on disk, nothing shared with the user's own
  browser. Downloads are cancelled. Main-frame navigation is limited to
  http/https (`BrowserPage::acceptNavigationRequest`); subframes may not load
  `file:`. Pages that open a new window load in the same one
  (`createWindow` returns the page itself). Camera, microphone and location
  requests are never granted.
- `read_window` returns `kind: "browser"`, `url` (the page the user is on now)
  and `page_title` only. The content comes from `read_browser_page` below.
- `show_window` with a browser window's `window_id` turns it into a content
  window and the other way round. Escape closes content windows only, because
  web pages use Escape themselves.

Without WebEngine the desktop hands the URL to the system browser
(`QDesktopServices::openUrl`) and replies `external: true`, `open: false` with
a message, so the agent knows there is no window to read. The opener is
injectable (`Registry::setExternalOpener`) so tests never start a browser.

Verified both ways on 2026-10-01: the normal build (no WebEngine on the deploy
box) runs the external path; a second build dir configured against WebEngine
6.8.2 extracted from the Debian packages (`apt-get download` + `dpkg-deb -x`,
`-DQt6WebEngineWidgets_DIR=...`, see
`docs/desktop/build-and-test.md`) builds the app and runs
`forkmesh-agent-windows-tests` with a real page load. The extracted tree also
needs `qt6-declarative-dev-tools`, `libqt6qmlcompiler6` (for
`qmlimportscanner`) and `libqt6webenginecore6-bin` (the `QtWebEngineProcess`
helper), with `LD_LIBRARY_PATH`, `QTWEBENGINEPROCESS_PATH` and
`QTWEBENGINE_RESOURCES_PATH` pointing into it.

## Reading the page (`read_browser_page`)

Asked next: "we want it to be able to see what is in the page as well".
`read_browser_page` takes `window_id`, `max_chars` (default 20000, at most
100000), `offset` (to page through long pages), `include_links` and
`screenshot`. It returns MCP content blocks:

1. one line of JSON: `window_id`, `url`, `title`, `total_chars`, `offset`,
   `truncated`, `selection` (what the user has selected, up to 5000
   characters), `scroll_y`, `page_height`, `viewport_height`, `ready_state`,
   `console` (the page's last 200 console messages with level, message,
   source and line, so a dev server's errors reach the agent), `links` when
   asked (up to 200, http/https only), `has_screenshot`;
2. the page's visible text (`document.body.innerText`);
3. with `screenshot`, an `image` block (PNG of the page as shown, at most
   1280 px wide) that the model looks at directly.

How it runs: the desktop runs a read-only script
(`pageCaptureScript`) in WebEngine's `ApplicationWorld`, an isolated
JavaScript world, so the page's own scripts can neither see nor tamper with
it. Console messages are collected by `BrowserPage::javaScriptConsoleMessage`
from the moment the window opens. The screenshot is `QWidget::grab()` of the
web view. The capture is asynchronous, so the intake socket gained deferred
replies: `Server::setAsyncHandler` hands the handler a `Reply` callback, and
`MainWindow` routes `windows.capture` through it while every other method
still answers synchronously. A capture that has not answered in 8 s replies
`capture_timeout`. Replies can be large, so the MCP server reads up to 24 MiB
for this method instead of the usual 64 KiB, and drops a screenshot that is
not a valid PNG.

What the user controls and sees:

- Each browser window has a **Share this page with the agent** checkbox,
  on by default. Unticked, reads fail with `not_shared`.
- After a read the footer says "The agent read this page at 14:02".
- The text is `innerText`, which leaves out what is typed into form fields.
  A screenshot shows the screen as it is, including text typed into ordinary
  fields; password fields stay masked. The checkbox tooltip says so.
- Only the agent that opened the window can read it.

Verified 2026-10-01 in the WebEngine build: `forkmesh-agent-windows-tests`
serves a page from an in-process HTTP server and checks the text, title,
links, a `console.error`, the PNG and the `not_shared` refusal.

## In the transcript

`mcp__forkmesh__read_browser_page` renders as "Read page" with one line such
as "Read 1,234 characters of Login · 1 console error · screenshot" (parsed
from the first JSON line; the page text itself stays out of the transcript).
`mcp__forkmesh__open_browser_window` renders as a "Browser" card with the URL
and a result line "Browser open · <page title>" (or "Opened in the user's
browser") with the same **Show window** link.
`mcp__forkmesh__show_window` renders as a "Window" card with the title as its
subtitle and a markdown body. Its result line says "Window open", "Window
closed" or "The user chose “Approve”: …" with a **Show window** link
(`forkmesh-window:<id>`, handled by `MainWindow::openAgentTranscriptReference`)
that raises the window again.

# Transcript update for current Claude Code (2026-10-01)

The same request asked for the transcript to use "the latest and all
improvements made". The view (`desktop/src/ClaudeTranscriptView.cpp`) was
written for an older stream: it knew `Task` but not `Agent`, dumped every
newer tool's input as JSON, and drew subagent traffic as if the main agent had
made those calls. Tool naming and summaries moved to a pure helper,
`desktop/src/ClaudeTranscriptTools.*`, with unit tests
(`desktop/tests/test_claude_transcript_tools.cpp`).

- `Agent` (the current name of `Task`) gets the subagent header, accent and
  prompt body.
- Subagent events (`parent_tool_use_id` set) no longer render flat. Each
  subagent tool call updates one line under its Agent card:
  "↳ 12 tool calls · Read src/x.cpp". Subagent thinking deltas no longer feed
  the main thinking card.
- Claude text streams live: `--include-partial-messages` already sent
  `text_delta`; the view now keys each text block by message id and index,
  grows it as it arrives, and settles it in place when the full `assistant`
  block lands. A block with inline choices is redrawn by `addAssistantText` so
  the choice buttons appear.
- Names and subtitles for current tools: Glob (pattern and path),
  NotebookEdit (`notebook_path`), WebFetch, Skill, ToolSearch, Monitor,
  BashOutput/TaskOutput, KillShell/TaskStop, ScheduleWakeup, SendMessage,
  Workflow, Task*/Cron* tools, and `mcp__server__tool` shown as
  "server · tool". ExitPlanMode shows the plan as markdown. Tools whose input
  is noise (ToolSearch, Workflow scripts, ...) no longer dump JSON. Bash and
  Agent say "in background" when run that way.
- `redacted_thinking`, `server_tool_use`/`mcp_tool_use` and their
  `*_tool_result` blocks (web search titles and URLs) are drawn instead of
  dropped. Image blocks in tool results show as `[image]`; ToolSearch's
  `tool_reference` blocks as "Loaded ...".
- System events: `compact_boundary`, `status: compacting`, `api_retry`,
  `local_command_output` and `task_notification` show as muted notices.
  Background-task notifications that arrive as user text are shown the same
  way.
- Output tokens are counted once per message id
  (`OutputTokenCounter`). Claude Code repeats a message's `usage` on every
  content-block event, so the old sum counted a message once per block. This
  applies to the view, the Agents list token cell
  (`MainWindowAgentEvents.cpp`) and external session reads.
- The result line shows "N in · M out tokens" from the Claude `result.usage`
  (it only knew Codex's `total_tokens` before).
- External session replay skips `isSidechain`/`isMeta` entries and injected
  harness text (`<system-reminder>`, `<command-...>`, task notifications)
  instead of showing them as "You" bubbles (`AgentLogText::isInjectedPrompt`).
