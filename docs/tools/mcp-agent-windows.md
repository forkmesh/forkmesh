# MCP connector: agent windows

Agents running as CLI processes can open small windows in the ForkMesh
desktop, similar to MCP Apps in chat clients:

- eight tools on the ForkMesh MCP server (`tools/forkmesh_mcp_server.py`,
  `WINDOW_TOOLS`): `show_window`, `read_window`, `close_window`,
  `open_browser_window`, `read_browser_page`, `browser_action`,
  `show_widget` and `suggest_session`;
- the agent-intake socket ([mcp-create-agent.md](mcp-create-agent.md))
  carries the matching methods `windows.show`, `windows.read`,
  `windows.close`, `windows.browse`, `windows.capture`, `windows.act`,
  `windows.widget` and `sessions.suggest`;
- the desktop draws content windows as Qt tool windows with a `QTextBrowser`
  (`desktop/src/AgentWindows.*`), owned by the main window. Browser windows
  use `QWebEngineView` when the build has WebEngine and the system browser
  otherwise.

The MCP server is the surface the model sees, and the intake socket is the
authenticated per-agent path back to the desktop. Browser actions, scripted
widgets and session suggestions, the ForkMesh side of Claude Code desktop's
built-in browser, widgets and "Start a new session?" chip, are described in
[mcp-claude-code-desktop.md](mcp-claude-code-desktop.md).

## The tools

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

## Tokens

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

Tests: `tools/test_forkmesh_mcp_windows.py`, `tools/test_forkmesh_mcp_server.py`,
`desktop/tests/test_agent_intake.cpp` `testAuthorization`.

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

## In the transcript

`show_window`, `open_browser_window` and `read_browser_page` render as
"Window", "Browser" and "Read page" cards with a one-line result and a
**Show window** link (`forkmesh-window:<id>`, handled by
`MainWindow::openAgentTranscriptReference`). Page text stays out of the
transcript. Tool naming and summaries for the transcript view live in
`desktop/src/ClaudeTranscriptTools.*` (tests
`desktop/tests/test_claude_transcript_tools.cpp`).
