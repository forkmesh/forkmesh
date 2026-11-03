# Desktop: the Sessions page

Part of [Desktop](README.md). The Sessions page is the Claude-desktop way to
manage the sessions the [Agents page](agents-ui.md) lists: one list of every
session (Claude app sessions included), the open session's live transcript,
side panels with its progress and context, and the global prompt composer
docked under the transcript to talk to it.

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
  sessions. Right-click a row for its menu ([Row and header
  menus](#row-and-header-menus)).
- **Grouping.** The filter menu's **Group by** offers **Like the Claude app**
  (the default), **Repository** and **Status**, kept in `sessions/groupBy`.
  Like the Claude app lays the list out as the Claude desktop app's Code tab
  does ([Claude app sessions](#claude-app-sessions)).
- **Centre.** Header (title, status chip, repository, branch, provider/model,
  mode, Stop, Continue, Open in Agents), the **Between sessions** strip (the
  last six messages to or from the open session; click one to open the other
  session), and the page's own `ClaudeTranscriptView`. It never borrows the
  Agents page's view.
- **Composer.** The page has no composer of its own. The footer prompt
  (`#promptWrapper`) docks over the centre column, capped at 760 px and
  centred, the way it docks over the Agents page's detail pane
  (`positionGlobalFooterOverlays`, `sessionsComposerPane`). Its add button
  and Enter go to the open session (`sessionsComposerTarget`); with no session
  open, or in new-session mode, its new button starts one in the repository
  and agent its own pickers hold. A page-local second composer had drifted
  out of layout and duplicated the footer prompt, so it was removed;
  forwarding a reply moved to the row menu.

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
  every 2 s while the page is on screen (`requestSessionsDesktopIndex`), and
  the list redraws only when something changed (a title, `lastActivityAt`,
  Waiting, Running, a group or a pin). The same tick rewrites each row's age
  from `SessionsPageUi::listedAt`, so "1m" turns into "2m" without a redraw.
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
  Claude app's order (empty groups hidden), then the `sessions` section. Its
  `groupBy` decides the layout: `none` (the Claude app's default, and what a
  missing value means) puts every remaining session in one **Recents** group,
  as the Claude app's sidebar does; any other value gives one group per
  folder (the last folder of the session's working directory, so an agent
  worktree is its own group) in natural order (`15-…` before `30-…` before
  `2625-…`). Every group is sorted by recency, newest first unless the
  section's `ascending` is set.
- Recency is the Claude app's own `lastActivityAt` for any row that has a
  Claude app record, matched ForkMesh agents included (`desktopAt`), so the
  order and the age column match the Claude app's sidebar. A ForkMesh session
  with no record keeps its own activity time. A record with no title shows
  "General coding session", as the Claude app does. ForkMesh sessions the
  Claude app does not have go in their worktree's folder group, or the
  checkout's. Group
  headings put the fold chevron after the name, as the Claude app does
  (`kSessionTrailingChevronRole`).
- Opening a Claude app row reads its transcript like an outside session
  (`sessionsOutsideSession` gives the path); sending to it is refused with
  the reason.
  Its row menu opens it in the Claude app (the `claude://resume` link) and
  offers **Send its last reply to…**; Rename, Fork, Mark as unread and Delete
  are off, as those belong to the Claude app.

## Row and header menus

`buildSessionsContextMenu` and `buildSessionsHeaderMenu` follow the Claude
app's Code tab menus.

- **Row:** Open in (Agents, Claude app), Continue, Stop, Pin (P), Mark as
  unread (U), Rename… (R), Fork (F), Move to section, Send message to another
  session…, Send its last reply to…, Archive (A), Delete… (D). The letters work while the menu is open
  (`SessionsMenuKeys`, since Qt does not fire shortcuts of a stand-alone
  popup). A disabled entry's tooltip says why. The list allows
  Ctrl/Shift-click; when the clicked row is in a selection of several, Pin,
  Mark as unread, Move to section, Archive and Delete act on all of them.
- **Header:** New session (with the repository picked under Group by
  Repository), New section…, Rename section…, Move up, Move down, Select all,
  Collapse all, Expand all, Archive all (N), Delete all sessions (N) (one
  confirmation, `deleteAgentSessionsConfirmed`), Delete section. Only
  sections made in ForkMesh can be renamed, moved or deleted; the Claude
  app's own are read-only, because ForkMesh never writes the Claude app's
  config.
- **State.** Pins, section placement, ForkMesh sections and archived
  sessions are ForkMesh's own (`sessions/pins`, `sessions/sectionOf`,
  `sessions/sections`, `sessions/archived`), keyed by `agentReadKey` or, for
  a Claude app row, `claude-app:<record>`. They override the Claude app's
  pins and groups for that session, so unpinning a session the Claude app
  pinned works. All three groupings show Pinned and the sections first;
  Like the Claude app passes them to `arrangeSidebar`, and a ForkMesh section
  shows while empty (`DesktopSection::keepEmpty`). Archived sessions leave the
  list; the filter menu's **Archived sessions** shows them under an Archived
  heading at the bottom.
- **Mark as unread** adds the session to `agents/markedUnread`, which
  `agentSessionUnread` honours until the session is next opened, so the open
  session stays unread instead of being marked read by the next refresh.
- **Fork** (local Claude Code sessions only) makes a new session that resumes
  the source's conversation with `--fork-session`, through the import path
  (`claudeImportId`/`claudeImportPath`; `ClaudeSessionImport::stage` copies
  the transcript into the new run). The fork gets its own branch, created at
  the source branch's last commit, and its own worktree on the next run, so
  deleting it never touches the source's. Uncommitted changes in the source
  worktree are not carried over. The fork keeps a copy of the source's
  transcript and opens ready for the next message.

## Side panels

The right column of the new interface is `SessionSidePanels`, a stack of
cards. The same stack is the Agents page's **Details** tab in Focus and its
right column in Control Tower
([agents-ui.md](agents-ui.md#workspace-layout-in-the-new-interface)).

| Card | Shows |
| --- | --- |
| **Environment**, **Context** | Where the session runs and its context use (below) |
| **Progress** | The to-do list from `TodoWrite`/`TaskCreate` and Codex plans, through `agenttasks::Tracker` |
| **Sub-sessions** | Live subagents, shells and workflows with Stop ([agents-ui.md](agents-ui.md#sub-sessions)) |
| **Outputs** | Files the agent wrote or edited, newest first, with line counts; a failed edit is taken back |
| **Sources** | Files read, pages fetched, web searches, skills and connectors |
| **Background** | Subagents and background shells with running/done/failed/stopped |
| **Changes** | Files changed on the branch, uncommitted files, the pull request and conflicts |
| **Tools** | Calls per tool and failures |

- Outputs, Sources and Background come from `sessioninsights::Collector`
  (no widgets, tested by `forkmesh-session-insights-tests`), which reads the
  events the transcript gets: `renderSessionsTranscriptSlice` hands every
  queued event to the panel, and the panel folds them in 8 ms slices only
  while it is visible.
- A card shows only once it has something in it (`Card::empty`,
  `applyVisibility`): an empty Progress, Sub-sessions, Outputs, Sources,
  Background or Tools card is hidden, and so is Changes while the branch has
  no changed or uncommitted files. Environment always shows. Move up and Move
  down skip the hidden cards.
- Drag a card's header to move it; the others make room as it passes and the
  list scrolls at the edges. Click the header to fold it. Right-click for
  Move up, Move down, Fold, Hide, Show panel and Reset panels.
- Order, hidden and folded cards are kept in `sessions/panelOrder`,
  `sessions/panelHidden` and `sessions/panelCollapsed`, shared by every
  place the stack shows. `normalizedCardOrder` keeps a saved order valid when
  a card is added: a new card goes after its default neighbour.
- The default order starts with Environment, then Context. Only an order the
  user changed is saved (`customCardOrder`): `sessions/panelOrder` is removed
  while the order is the default, and a saved copy of an earlier default
  (folding a card used to save the whole order) counts as not customised, so
  it takes the new default while a real custom order stays.
- Clicking an output or a read file shows it in the file manager (its folder,
  or the folder itself), never through the file's own handler, so an agent
  cannot plant a script and have a click run it. A page opens in the browser
  only through `ChatLinkText::externalLinkUrl`, so a tool call's
  `smb://` or `file://` URL is dropped instead of handed to the system
  (`SessionSidePanels::openTargetUrl`; checked by
  `desktop/tests/validate_body_link_schemes.py`).
- The column opens whenever a session starts: **New session**, sending the
  first prompt, and every local launch (`startAgentRunner` calls
  `NextAccess::sessionStarted`) set `panelOpen` again even if it was hidden.
  The new-session screen shows the column too.
- **Flat sections.** The cards have no background or box: each
  is a titled section with a thin line under it (none under the last shown
  one, `sessionsLast`), so they sit closer together. The column has no border
  of its own; the splitter handle is the one line between the transcript and
  the panels, in Control Tower (`agentCodeSplit`) and on the Sessions page.
- **Context** reads like Claude Code's context panel: "N% used"
  and used / window above a bar split into coloured parts, then what fills the
  window, each with a swatch, tokens and share: **Messages** (blue),
  **Tool calls** (orange), **Memory files** (teal), **System prompt & tools**
  (pink) and **Free space**. Under a gap, grey rows give the session totals:
  Input and Output tokens, Turns, Cost, **Tools loaded** (tools and MCP
  servers), **Skills & agents** and how often it was **Compacted**. Last, a
  folding **Memory files N** row lists each file with its estimated tokens;
  clicking one opens it.
  - The numbers come from `sessioninsights::Collector::context()`: "used" is
    the last assistant message's input (with cache reads and writes) plus
    output; the first turn's input, less the prompt before it, is the fixed
    part (system prompt, tools, memory); the system `init` event gives the
    tool, MCP, skill and agent counts and the model; `compact_boundary`
    counts a compaction and restarts the conversation tally. Without usage
    events (Codex, cloud runs) "used" is the session's `contextTokens`.
  - `splitContext` takes the memory files' estimate out of the fixed part and
    splits the rest of "used" between Messages and Tool calls by the length of
    the text since the last compaction (4 characters a token). These are
    estimates; the totals are the API's.
  - `memoryFiles` finds what Claude Code loads: `~/.claude/CLAUDE.md`, then
    `CLAUDE.md`, `.claude/CLAUDE.md` and `CLAUDE.local.md` from the root down
    to the session folder, following `@path` imports, and the first 200 lines
    of the auto memory `MEMORY.md` (`~/.claude/projects/<slug>/memory`, the
    folder or its nearest parent that has one). With no CLAUDE.md anywhere it
    lists the `AGENTS.md` chain instead (Codex). The panel rescans every 30
    seconds or when the folder changes.
  - The window grows to 1M when the model name has `[1m]` or "used" passes
    the configured window. "N% used" turns amber at 80% and red at 95%
    (`sessionsLevel` on `sessionsContextUsed`). The header's percentage shows
    only while the card is folded, since the open card already says it.
- **Environment** shows Claude Code's own **Claude Remote Control** setting
  for Claude sessions on this desktop: On, Off or Default, from
  `remoteControlAtStartup` ("Enable Remote Control for all sessions" in
  Claude Code's `/config`), read from the active Claude account's
  `settings.json` and then its `.claude.json`
  (`agentaccounts::readClaudeRemoteControl`, cached by file time and size).
  It is shown, not changed: Claude Code rewrites `.claude.json` while it runs.
- **Environment** ends with a **Phone remote control** switch, the global
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
  messages are reported). Sending to one with no messaging socket is refused
  with the reason.

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
transcript, composer, relays, row and header menus), `forkmesh-session-insights-tests`,
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
  - The docked footer prompt sits under it, centred, at most 760 px wide.
- **New-session mode.** The header is hidden. A centred "Start a new session"
  heading shows three starter cards, which fill the footer prompt but never
  send.
