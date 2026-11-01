# Desktop: the Sessions page

Part of [Desktop](README.md). The Sessions page is the Claude-desktop way to
manage the sessions the [Agents page](agents-ui.md) lists: one list of every
session (Claude app sessions included), the open session's live transcript,
side panels with its progress and context, and a composer that talks to it or
relays between sessions.

## Code map

| Part | Files |
| --- | --- |
| Page, list, relays | `src/MainWindowSessions.cpp` |
| Side panels | `src/SessionSidePanels.*`, `src/SessionInsights.*` |
| Claude app sidebar | `src/ClaudeDesktopSidebar.*` |
| Styles | `resources/theme/next/63-sessions.qss` |
| Hooks | `applyTranscriptEvent` (`MainWindowAgentEvents.cpp`), `refreshAgentTable` and `refreshAgentDotMatrix` (`MainWindowAgentList.cpp`), `applyExternalClaudeScan` (`MainWindowAgentCliSession.cpp`), `refreshAgentTranscriptTheme` |

## Rail button

Rail **Sessions** (`sessionsNavButton`, rail key `sessions`, section
`kSessionsSectionIndex`) sits directly above **Agents**.

- The button is in `m_navGroup`, so `showSection` checks it like every
  section button, and the Agents button unchecks through
  `updateRepoActivityRail`.
- It sits in the Work group, follows the `agents` package
  (`syncPackageSurfaces`, desktop-offline dot), and the Code button is
  inserted above it.
- A saved rail order without the button places it just above Agents
  (`applyRailOrder`). It ranks a quarter step before Agents, not half a step:
  icons missing from a saved order rank half a step after their saved
  neighbour, so at half a step Sessions would tie with them and land between
  an icon and its default neighbour.

## Layout

- **Sidebar.** **New session** (the page's one green button), search, a
  **Between sessions** filter, then **Active** (Waiting first, then Running,
  then Queued) and **Recent** (newest first, at most 300). A delegate paints
  each row: status dot, title, repository · branch · model, age, unread dot
  and a `⇄ N` badge when the session sent or received messages from other
  sessions. Right-click: Open in Agents, Continue, Stop, Send message to
  another session…, Delete (confirms through `deleteAgentSessionFromRow`).
- **Grouping.** The filter menu's **Group by** offers **Like the Claude app**
  (the default), **Repository** and **Status**, kept in `sessions/groupBy`.
  Like the Claude app lays the list out as the Claude desktop app's Code tab
  does ([Claude app sessions](#claude-app-sessions)).
- **Centre.** Header (title, status chip, repository, branch, provider/model,
  mode, Stop, Continue, Open in Agents), the **Between sessions** strip (the
  last six messages to or from the open session; click one to open the other
  session), and the page's own `ClaudeTranscriptView`. It never borrows the
  Agents page's view.
- **Composer.** Enter sends, Shift+Enter adds a line. **To** defaults to the
  open session; **Also send to** sends the same text to a second session;
  **Forward last reply to…** sends the open session's latest reply to another
  session. In new-session mode the row shows Repository and Agent instead,
  and Send calls `startAdHocAgentForRepo` without leaving the page, then opens
  the new session.

## Claude app sessions

The list also shows every session in the Claude desktop app's Code tab. How
ForkMesh registers its own agents there is in
[claude-desktop-sessions.md](../agents/claude-desktop-sessions.md).

- `forkmesh::claudedesktop::readDesktopIndex` (Qt Core only, tested in
  `forkmesh-claude-desktop-sessions-tests`) reads the Code tab's records
  (`<Claude config>/claude-code-sessions/<account>/<org>/local_*.json`,
  archived ones left out; a `waiting-input/` marker means Waiting) and its
  sidebar layout from `claude_desktop_config.json`
  (`preferences.epitaxyPrefs`: `dframe-code-sections` for the section order,
  `dframe-group-scopes` for named groups and their members,
  `starred-local-code-sessions` for Pinned). It is read off the GUI thread
  every 5 s while the page is on screen (`requestSessionsDesktopIndex`), and
  the list redraws only when something changed.
- A record is matched to a ForkMesh session by `claudeAppSessionId`, an
  imported session's `claudeImportId`, an outside session's id, then by the
  agent worktree it ran in (`<repo>/.worktrees/<worktreeName>`). A matched
  agent keeps its row and shows the Claude app's title (with the progress
  emoji agents set). A record with no match becomes a Claude app row
  (`rebuildSessionsDesktopRows`, ids from -900000 down in the outside band).
  Those rows live only in the Sessions page (`SessionsPageUi::desktopRows`),
  not in `m_agentSessions`, so they never reach the Agents tab, the phone or
  the outside-session setting. Running means the transcript was written in
  the last 90 s.
- `arrangeSidebar` builds the groups: Pinned, then each named group in the
  Claude app's order (empty groups hidden), then one group per folder (the
  last folder of the session's working directory, so an agent worktree is
  its own group) in natural order (`15-…` before `30-…` before `2625-…`),
  newest session first inside each group. ForkMesh sessions the Claude app
  does not have go in their worktree's folder group, or the checkout's. Group
  headings put the fold chevron after the name, as the Claude app does
  (`kSessionTrailingChevronRole`).
- Opening a Claude app row reads its transcript like an outside session
  (`sessionsOutsideSession` gives the path), with the composer disabled.
  Right-click: **Open in Claude app** (the `claude://resume` link) and
  **Send its last reply to…**.

## Side panels

The right column of the new interface is `SessionSidePanels`, a stack of
cards. The same stack is the Agents page's **Details** tab in Focus and its
right column in Control Tower
([agents-ui.md](agents-ui.md#workspace-layout-in-the-new-interface)).

| Card | Shows |
| --- | --- |
| **Progress** | The to-do list from `TodoWrite`/`TaskCreate` and Codex plans, through `agenttasks::Tracker` |
| **Sub-sessions** | Live subagents, shells and workflows with Stop ([agents-ui.md](agents-ui.md#sub-sessions)) |
| **Outputs** | Files the agent wrote or edited, newest first, with line counts; a failed edit is taken back |
| **Sources** | Files read, pages fetched, web searches, skills and connectors |
| **Background** | Subagents and background shells with running/done/failed/stopped |
| **Changes**, **Context**, **Environment** | The session's diff, context use and environment (below) |
| **Tools** | Calls per tool and failures |

- Outputs, Sources and Background come from `sessioninsights::Collector`
  (no widgets, tested by `forkmesh-session-insights-tests`), which reads the
  events the transcript gets: `renderSessionsTranscriptSlice` hands every
  queued event to the panel, and the panel folds them in 8 ms slices only
  while it is visible.
- Every card is always shown, with a one-line hint while it is empty, so a
  new session shows the full set from the first prompt.
- Drag a card's header to move it; the others make room as it passes and the
  list scrolls at the edges. Click the header to fold it. Right-click for
  Move up, Move down, Fold, Hide, Show panel and Reset panels.
- Order, hidden and folded cards are kept in `sessions/panelOrder`,
  `sessions/panelHidden` and `sessions/panelCollapsed`, shared by every
  place the stack shows. `normalizedCardOrder` keeps a saved order valid when
  a card is added: a new card goes after its default neighbour.
- Clicking an output or a read file opens it; a page opens in the browser.
- The column opens whenever a session starts: **New session**, sending the
  first prompt, and every local launch (`startAgentRunner` calls
  `NextAccess::sessionStarted`) set `panelOpen` again even if it was hidden.
  The new-session screen shows the column too.
- **Context** reads like Claude Code's context panel: "N% used" and used /
  window above the bar, then rows with a coloured swatch, value and share:
  Used (blue), Free space, Input and Output tokens (share of all tokens in the
  session), Turns and Cost. Sessions that only report a total show one Tokens
  used row. The bar and the Used swatch turn amber at 80% and red at 95%
  (`sessionsLevel` on `sessionsContextBar`). The header's percentage shows
  only while the card is folded, since the open card already says it.
- **Environment** ends with a **Remote control** switch, the global
  phone-control switch (`localControl/remoteControl`, on by default;
  `MainWindow::setRemoteControlOn`). Under it, a connected phone shows its
  name and whether it reached the desktop on this network or through the
  relay; a paired phone that is offline shows when it was last seen; with no
  phone paired, a note explains how to connect one and links to Settings >
  Phone. Off makes `LocalControlServer` answer every paired request with
  `remote_off` and stop pushing events; pairing and `ping` still work so a
  phone can tell the desktop is there. Both phone apps word `remote_off`.

## Transcript sources and sending

- **Local sessions** render `m_streamEvents` (loaded with
  `ensureStreamEventsLoadedAsync`; a session with no events shows its prompt
  and last reply).
- **External Claude Code sessions** read their `.jsonl` off the GUI thread
  from the page's own offset (the last 400 KB first, then only what was
  appended), redacted like the Agents page.
- **Cloud runs** render `cloudAgentTranscriptEvents` and redraw when the
  relay history changes.
- **Sending.** Local and cloud sessions go through `sendPromptToAgentSession`.
  An external Claude Code session is messaged through `ClaudePeerMessaging`
  (the same peer check as the phone's outside-session prompt: refused or held
  messages are reported). One with no messaging socket shows the composer
  disabled with the reason.

## Messages between sessions

A forward or a context-menu message is sent as the source session, not as
the user: `hub::messageEnvelope`, with the same permission rule as
agent-to-agent messages (`hub::messageAllowed`: a session in a less
permissive mode may not direct a more permissive one; an outside session's
mode counts as unknown) and the same `m_agentMessageLimiter`. Both sessions
are logged as `MainWindowAgentHub.cpp` does: the sender gets a "Sent a message
to agent #N" notice, the receiver "==> Message from agent #M". Messages that
agents send each other through the hub show up too: `applyTranscriptEvent`
hands every event to `noteSessionsPageEvent`, which spots the envelope's lead
line and records it for the strip, the badge and the filter (kept in memory,
at most 200).

## No stalls

- The `applyTranscriptEvent` hook only notes the event: when the page is
  hidden it marks the transcript dirty and returns; when visible it queues
  one coalesced catch-up.
- Transcript rows are built from a queue in 8 ms slices on single-shot
  timers, at most the last 300 events at first (**Load earlier** adds 300),
  with bulk populate during a replay. Slices stop while the page is hidden
  and resume when it is shown.
- The list refreshes on a 400 ms coalescing timer only while visible, updates
  rows in place when the order is unchanged, and has no widgets per row.
  Nothing polls: refreshes come from the Agents list's own notifications and
  the external-session scan.

## Tests

`forkmesh-window-tests --sessions-only` (rail placement, list, live
transcript, composer, relays), `forkmesh-session-insights-tests`,
`forkmesh-claude-desktop-sessions-tests`.

## New interface

In the new interface ([ui-redesign.md](ui-redesign.md)) the page has its own
layout (`MainWindowSessions.cpp`, `63-sessions.qss`).

- **Sidebar** (264 px).
  - A search field and a filter button. Its menu holds the "Between sessions
    only" filter, which toggles the existing, now hidden,
    `sessionsBetweenFilter`, and a Group by Repository or Status choice kept in
    memory.
  - Sessions sit under foldable repository headings, live groups first. A
    heading's "+" opens new-session mode with that repository picked, when it
    has a local checkout.
  - `SessionsNextRowDelegate` paints 32 px rows: status icon, title, age, the
    relay count and unread dot. On hover a "…" opens the existing context
    menu. The repo · branch · model line is in the tooltip.
  - Headings use a third sentinel id, which `sessionsHeaderId()` filters.
- **Centre header** (one 48 px row): title with a chevron menu (the same
  context menu), a monospace branch badge, the status chip, the meta line,
  ghost Stop, Continue and Open in Agents buttons, and a toggle for a 288 px
  details panel.
- **Details panel.** Foldable cards built only from real data:
  - Environment: machine, worktree path, branch with ahead and behind from
    `m_agentDiffStats`, agent, mode.
  - Context: context-window use, tokens, cost.
  - Changes: +/− lines, files, uncommitted, pull request, conflicts.
- **Left out on purpose.** The mockup's remote-control switch, token breakdown,
  file list, Commit and Open PR buttons, task list, and terminal, files and
  browser buttons: nothing on this page backs them.
- **Transcript and composer.**
  - The transcript column is capped at 808 px and centred.
  - The composer is a centred rounded card with ghost pickers, the open
    session's mode and model, and a round send button.
- **New-session mode.** The header is hidden. The composer moves under a
  centred "Start a new session" heading with three starter cards, which fill
  the prompt but never send.
