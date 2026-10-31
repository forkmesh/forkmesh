# Desktop: Agents page, activity window and sounds

Part of [the desktop docs](README.md). How runs work underneath (runtimes,
sandboxing, website jobs, mirrors, errors): [../agents/README.md](../agents/README.md).

## Agents page

Rail **Agents**. The rail button directly above it, **Sessions**, opens the
[Sessions page](#sessions-page), a Claude-desktop style view of the same
sessions. Main sources:

| Part | Files |
| --- | --- |
| Page | `src/MainWindowAgents.cpp` |
| List | `src/MainWindowAgentList.cpp`, `src/AgentFolders.*`, `src/ListViewMode.*`, `src/AgentTicker.*` |
| Detail | `src/MainWindowAgentDetail.cpp` |
| Transcript | `src/MainWindowAgentTranscript.cpp`, `src/ClaudeTranscriptView.*`, `src/ClaudeTranscriptTools.cpp` |
| Launch | `src/MainWindowAgentLaunch.cpp`, `src/MainWindowAgentCliSession.cpp` |
| Usage and accounts | `src/MainWindowAgentUsage.cpp`, `src/MainWindowAgentUsagePage.cpp`, `src/AgentUsageResetClock.*`, `src/CodexRateLimitsProbe.*`, `src/MainWindowChat.cpp`, `src/ClaudeAccountRouting.cpp` |
| Cloud rows | `src/AgentCloudSessions.*`, `src/AgentCloudStream.*`, `src/MainWindowAgentCloud.cpp` |
| External sessions | `src/ClaudeSessionScan.*`, `src/MachineAgentScan.*`, `src/ClaudePeerMessaging.*` |
| Task links | `src/MainWindowAgentTaskLink.cpp` |
| Intake socket | `src/AgentIntake.*`, `src/MainWindowAgentIntake.cpp` |

The toolbar has **Tool efficiency** (`src/AgentToolEfficiencyBoard.*`),
**Models** (page 3 of `agentPageStack`), **Usage**, **Listen** and
**Announcer**. Watch is a Market package page in the rail (see
[Watch](#watch-market-package-agent-watch)).

The page is tab 3 of an open repository, and its list shows only that
repository's agents. `openAgentsOverview` opens the selected or newest agent
whose repository is open or in the repository list; with no repository open
it opens the first repository on its Agents tab. Rail **Agents** with no
repository at all unchecks the button, shows Repos and a notice that agents
work inside a repository; the tour and the composer's Models link keep their
own handling. Before 2026-10-23 a click with no repository open did nothing,
which left a checked Agents button over an unchanged page in both designs.

### Usage page: install a missing CLI (2026-10-25)

When `forkmesh::agentcli::installed("claude")` or `("codex")` finds no CLI, that
provider's section on the Usage page says "Not installed on this desktop" and
shows **Install Claude Code** or **Install Codex**
(`agentUsagePageInstallClaude` / `agentUsagePageInstallCodex`); its terminal
buttons are disabled, since they would only fail. The button runs the same
install as Settings -> Apps (`installSuggestedApp`, npm with
`@anthropic-ai/claude-code` or `@openai/codex`, after a confirmation). Without
npm it is disabled and the page shows the reason; while an install runs it
reads "Installing…", and a failed install leaves its last output line
(`m_suggestedAppInstallProblems`) under the button. `refreshSuggestedApps`
redraws the Usage page, so the button goes away once the install finishes.

### Usage page: Refresh and reset times (2026-10-19)

The `usage` Market package's page is `MainWindow::buildAgentUsagePage`
(`src/MainWindowAgentUsagePage.cpp`). Its **Refresh** button calls
`refreshAgentUsageAccounts` (`src/MainWindowAgentUsage.cpp`), which walks every
Claude Code account one by one: it reads that account's own OAuth token from its
config folder (`claudeCodeOAuthToken(configDir)`), asks
`api.anthropic.com/api/oauth/usage`, and stores the answer with
`applyClaudeUsageResponse(response, accountId)`. The active account goes through
the full path (top-bar meter, limit reminders, credits); any other account only
gets its account-scoped settings written, so the meters on the page are a real
re-read of each account rather than a redraw. Each account keeps its own poll
backoff key (`claude-usage-<id>`), so a rate-limited account is skipped until
its backoff ends, and a failed read keeps the server's `Retry-After`, as the
regular poll does (`claudeUsageRetryAfterMs`). The button reads "Refreshing…"
until the last account answers.

Refreshes are coalesced: a call while one is running only joins its list of
waiters (`m_agentUsageRefreshWaiters`), and a call within 30 seconds of the
last finished one (`kAgentUsageRefreshMinMs`) answers from the stored readings
without asking Anthropic again. Without that, the web page and both phones
refreshing at once, or a paired client calling `agentops.usage {refresh: true}`
in a loop, would send one request per account per call with the user's tokens
and soon earn a 429 that also stalls the regular poll.

After the Claude Code accounts, Refresh re-reads every signed-in Codex account
(one whose config folder holds `auth.json`) the same way, one at a time. Codex
limits only come from `codex app-server`, so `CodexRateLimitsProbe`
(`src/CodexRateLimitsProbe.*`, tested by
`tests/test_codex_rate_limits_probe.cpp` with the test binary standing in for
the app-server) starts one with `CODEX_HOME` set to that account's folder,
sends `initialize`, `initialized` and `account/rateLimits/read`, takes
`rateLimits` and `rateLimitsByLimitId` (adding the general snapshot under its
own `limitId`, as `CodexAppServerSession` does), and stops the process. It
starts no thread and no conversation, so it spends no tokens. Each probe gets
10 seconds; a probe that times out, exits early or gets an error counts as a
failure for that account's backoff key (`codex-usage-<id>`) and the walk moves
on. Codex accounts are skipped when the `codex` CLI is not installed.

The probe is not a repository agent: it runs in the account's config folder,
touches no worktree, and so takes no repository guardrails or KVM isolation.
It does keep the rest of an agent launch: the widened `PATH`, the
`agentresources` environment and shell wrapper, and the systemd scope
(`agentresources::contain`) where that exists. `OPENAI_API_KEY` and
`CODEX_API_KEY` are removed from its environment so the read is the signed-in
account's, not whatever key ForkMesh was started with.

`applyCodexRateLimits(rateLimits, rateLimitsByLimitId, accountId)` stores the
answer. For the active account it is the usual path (top-bar meter, limit
reminders, the global keys). For any other account it calls
`applyInactiveCodexRateLimits`, which writes only that account's scoped
settings (usage windows, 5-hour and weekly percent, reset and window start), the
same split `applyClaudeUsageResponse(response, accountId)` makes for Claude.

Each meter shows the reset as a 12-hour clock time in the desktop's time zone,
next to the remaining time: "Resets 3:45 PM (in 5 h 40 min)", "Resets Tue
9:00 AM (in 22 h 55 min)" when it is not today, and "Resets Oct 26, 10:00 AM"
a week or more out. `forkmesh::usageclock` (`src/AgentUsageResetClock.*`,
tested by `tests/test_agent_usage_reset_clock.cpp`) formats it; the menus and
top bar keep their relative "resets in" text.

Remote surfaces get the same re-read through `agentops.usage {refresh: true}`
(`src/MainWindowLocalControlAgentOps.cpp`): the desktop runs
`refreshAgentUsageAccounts` and answers with the new summary once every Claude
Code and Codex account has answered, or after 20 seconds with whatever it has. Without `refresh` the
call answers the stored snapshot at once, as before; older desktops ignore the
flag and answer the snapshot. The web page (`/dashboard/usage`), iPhone and
Android send `refresh: true` from their Refresh button and pull-to-refresh, and
a plain read when the page first opens. Each formats the reset clock locally:
web `usagePageResetClock` (`marketplace/packages/usage/content/web/page.js`,
`app/tests/usage_page_web.test.mjs`), iOS `UsageFormat.resetClock`
(`FMAgents/Remote/AgentOpsModels.swift`, `UsageResetClockTests`), Android
`UsageFormat.resetClock` (`agents/ops/AgentOpsModels.kt`,
`UsageResetClockTest`). The web page previously passed the future reset time to
`relativeTimeLabel`, which only formats past times and so read "Resets 1 minute
ago"; the clock label replaces it. On Android the reset clock moves to the line
under the meter, beside the hour/day and pace text, so the bar keeps its width.

### Session sources and ids

The list is one list of every session. An icon in the title cell says where
it runs (`device-desktop` for this desktop, `cloud` for a mirror or runner);
searching "desktop", "cloud" or a mirror name narrows it.

| Source | Id band | Stored |
| --- | --- | --- |
| Local sessions | positive | `data/agents/sessions` |
| External Claude Code sessions and machine-wide agents | -1000 and down (`externalTempIdFor`) | never |
| Cloud runs from the account's mirrors and runners | -1000000 and down (`kCloudIdBase`) | never |

Invariants:

- Only positive ids are stored: `AgentStore::saveSession` refuses an id of 0
  or below and `loadAllSessions` skips one.
- Cloud runs that target this desktop are dropped; they exist locally.
- The cloud band lies inside the external band, so code must test
  `isCloudSession()` before `isExternalSession()`.
- Branch-wide deletes (`deleteWorktreeBranchAndAgentInBackground`) skip both
  bands.
- Durable per-row state (folders, unread) is keyed by `agentReadKey()`: the
  local id, `cloud:<relay id>` or `claude:<uuid>`.

### Importing Claude Code sessions (2026-10-22)

**Import** (pinned at the left of the list's action bar,
`showClaudeSessionImportDialog` in `MainWindowClaudeImport.cpp`) lists every
Claude Code and Claude Desktop session that ran in the open repository, of any
age, and turns the ones picked into ordinary stored agents.

- **Where sessions come from.** `ClaudeSessionScan::listAll` reads
  `<config>/projects/<key>/*.jsonl` under `~/.claude` and the desktop's own
  `CLAUDE_CONFIG_DIR`, where `<key>` is the cwd with every non-alphanumeric
  character turned into `-` (`projectKey`). Only folders whose key starts with
  the repository's are opened unless **Every folder** is ticked. A session
  copied into two project folders is listed once, from the folder matching its
  cwd. Sessions with no prompt and no title are left out. ForkMesh's own
  account folders (`data/agents/accounts`) are not read: those are its agents.
- **Names.** In order: Claude Desktop's title
  (`<Claude config>/claude-code-sessions/<account>/<org>/local_*.json`, keyed by
  `cliSessionId`; `~/.config/Claude` on Linux), the last `custom-title`
  (`/rename`), the last `ai-title`, then the first typed prompt cut to one line
  of 80 characters. External rows use the same names, so headless sessions no
  longer all read "Claude Code session".
- **Rows that are not offered.** Headless sessions (an `sdk-*` entrypoint:
  `claude -p`, the SDK, ForkMesh's own agents and special agents) and sessions
  in the agent worktree root (hundreds of ForkMesh runs on a busy machine) are
  left out unless **Headless runs** is ticked. One whose worktree belongs to
  an agent still in the list is never shown. A session already
  imported says so; importing it again refreshes its name.
- **The agent.** `createImportedClaudeAgent` stores a Claude Code session with
  status Success, the session's name as title, its first prompt, model, first
  and last timestamps, and `claudeImport` (`id`, `path`, `cwd`, `bytes`) in
  `meta.json`. A cwd inside the agent worktree root on a branch adopts that
  worktree (`worktreeName`, `branchName`); anything else is a no-worktree
  session that runs in the original cwd when it is inside the repository, else
  in the checkout.
- **Transcript.** `ClaudeSessionImport::convertLines` turns typed prompts into
  `_local_user`, keeps assistant turns and tool results with the session id
  stamped on them, drops sidechains, meta lines and command echoes, and clips a
  tool result to 16 KB. The readable log gets both sides, so a later switch to
  Codex hands over the conversation.
- **Mirroring.** While `claudeImport.bytes` is 0 or more, every 3 s tick
  (`syncImportedClaudeSessions`) appends whatever the `.jsonl` gained since
  that offset and refreshes the name, so turns taken in Claude Desktop keep
  arriving. An imported session is never also shown as an external row.
- **ForkMesh's own agents are never external rows (2026-10-25).** Since
  agents became Claude app sessions
  ([../agents/claude-desktop-sessions.md](../agents/claude-desktop-sessions.md)),
  each agent's account project folder is symlinked into `~/.claude/projects`,
  the folder the external scan reads. Merging an agent drops its stream (and
  with it the worktree from `ownStreamCwds`) while its transcript is still
  under 90 s old, so with "Exclude external Claude Code agents from the Agents
  tab" off a new
  row with the merged agent's title appeared. `ClaudeSessionScan::scan` now
  skips project folders that resolve into a Claude agent account's config
  dir, and the worktrees of streams ForkMesh dropped (`retireStreamCwd`, from
  `purgeSessionState`, `releaseTrackedWorktree` and `cleanupStreamWorktree`)
  stay excluded for the rest of the run, which covers agents that use
  `~/.claude` directly. A run without a worktree, which works in the
  repository checkout itself, is not remembered, so your own sessions there
  still list.
- **Continuing it.** The first follow-up resumes the Claude session id found
  in the transcript. `startCliTranscript` sets `bytes` to -1 (mirroring stops),
  `launchCliRuntime` copies the `.jsonl` to where the run's account looks for
  it (`<account config>/projects/<key of the workdir>/<id>.jsonl`,
  `ClaudeSessionImport::stage`; nothing is copied when that is the same file)
  and adds `--fork-session`, so the run gets its own session id and never
  writes into Claude's copy. If the resume still fails, the fallback prompt
  carries the last 48 KB of the imported log.
- **Typing into an External Claude Code row.** The composer used to refuse a
  row it did not start ("No agent open above"). Now
  `sendPromptToExternalClaudeSession` sends a live session the message
  through its cross-session inbox, the same path phones use, with
  `ClaudePeerMessaging::desktopSteerLead()` and sender name
  `ForkMesh (desktop)`; `steerText` strips either lead for display. A session
  that refuses or would hold the message without a terminal, or has no
  socket, offers to continue it as a ForkMesh agent instead. A row that is no
  longer live is imported and the message continues it. Attachments are
  refused for these rows.

### Claude Code session control (2026-10-23)

A Claude Code run on the desktop is a full stream-json host of its Claude Code
session, the way an IDE or Claude's desktop app hosts one.
`launchCliRuntime` sets the launch option `hostControl`, and
`ClaudeStreamSession` then:

- adds `--permission-prompt-tool stdio` and writes an `initialize` control
  request before the task. It declares the dialog kinds the desktop renders
  (`refusal_fallback_prompt`, `fable_overage_consent_prompt`,
  `auto_mode_server_fallback`), asks for sub-session progress summaries
  (`agentProgressSummaries`) and declares a per-task stop control
  (`perTaskStopAffordance`). An issue run, or a run you named yourself, passes
  its title as `title`, so Claude Code keeps that name;
- keeps every control request it has not answered under a card id
  (`claude-approval-<request>` for tool approvals, `claude-input-<request>` for
  dialogs and MCP input, the tool use id for `AskUserQuestion`) and answers it
  when the card is answered;
- refuses a request nobody renders (`hook_callback`, `mcp_message`, ...) with
  an error at once, so the CLI never waits on it, and never answers a dialog
  kind it did not declare (the CLI's rule: answering one would count as the
  user dismissing it). Neither marks the session Waiting;
- drops a pending prompt when the CLI withdraws it (`control_cancel_request`);
  the card then reads "Claude Code withdrew this question";
- routes the answers to its own requests (`generate_session_title`,
  `rename_session`, `stop_task`) to callbacks and keeps them out of the
  transcript.

The mapping from requests to cards and from answers to responses is pure code
in `ClaudeControlProtocol.{h,cpp}`; `MainWindowAgentClaudeControl.cpp` puts it
on screen. Shapes were checked against Claude Code 2.1.309.

**Names.** A run started from a prompt is usually named before launch by a
one-shot `claude -p`, Haiku first and then the run's own model (see
[../agents/README.md](../agents/README.md#local-runs-desktop)); that name
goes in as `title` and is sent again with `rename_session`, like a name you
gave it. Otherwise, on the first `system/init` of each process the desktop sends
`generate_session_title` with the prompt (`persist: true`), so the run is named
by its own Claude Code session and the name is saved in the session's
transcript; a resumed session gets its saved name back instead of a new one.
`system/session_title_changed` (a `/rename`, a rename on claude.ai, or the echo
of ours) renames the run at any time; a repeated name changes nothing.
`titledBy` records `Claude Code session` or `Claude Code rename`. The run is
renamed only when its title came from its prompt or from an earlier namer:
issue runs keep the issue title and a caller's name (a PR review, a conflict
fix) stays. The pre-launch namer finishes before the session starts and
`generate_session_title` is skipped for its name, so the two never race. A held
Claude run shows its prompt title until it starts. Settings > Agents > "Agent
titles" turns Claude's naming off too.

**Rename.** The agent list's context menu has **Rename…**. The new name is
saved with `titledBy` `You` (no namer overrides it) and sent to a running
session with `rename_session` (`source: host`, the CLI's session id); a session
renamed while stopped gets the name on its next start.

**Prompts.** Each prompt is a question card in the transcript
(`ClaudeTranscriptView::addAskUserQuestion`; its heading comes from the card's
`title`, and question and option text are plain text):

| Request | Card | Answers |
| --- | --- | --- |
| `can_use_tool` | "Claude Code asks to use *tool*": the command, file, URL or query, a path outside the working folders, and why it asks | **Allow once**; **Allow and remember** (only with `permission_suggestions`, described in words, sent back as `updatedPermissions`); **Deny**; typed text denies and is sent to Claude as what to do instead |
| `can_use_tool` for `ExitPlanMode` | "Claude Code finished planning" | **Approve and accept edits** / **Approve and ask before edits** (a `setMode` to `acceptEdits` / `default`; the session's mode label follows), **Keep planning**; typed text is plan feedback |
| `can_use_tool` for `AskUserQuestion` | the transcript's own card for the tool call | sent as `updatedInput.answers`, one per question, instead of a late tool result |
| `request_user_dialog` | "Claude Code needs a decision" with the dialog's own choices | `{behavior: completed, result}`; typed text on the refusal dialog edits the prompt and is sent as the next turn |

An MCP `elicitation` opens a small window (`ClaudeElicitationDialog`): a form
built from `requested_schema` (choices for `enum`/`oneOf`, a check box for
booleans, password echo for `format: password`, required fields marked `*`),
or for URL mode the link with **Open link** (http and https only) and
**Done**. **Send** checks required fields and number types; **Decline** and
**Cancel** answer `decline` and `cancel`, and so does closing the window.

In Ask, Edit and Plan modes the CLI now really asks. Before, a headless run
without a prompt tool refused every call that needed approval. The external-call
gate's PreToolUse hook still runs first and still only denies.

**Sub-sessions.** Claude Code's subagents, background shells and workflows
are the run's sub-sessions. `agentclitasks::Ledger` keeps each open task's
description, subagent type, nesting (`parent_task_id`, `spawn_depth`), last
progress summary (`task_progress.summary`, else its changed description), last
tool, tokens and tool calls. The **Sub-sessions** card of the side panels
(`SessionSidePanels::setSubSessions`, filled by `fillAgentSubSessions`) lists
them, nested under the sub-session that started them, one line each (name,
tokens or kind; the tooltip has the facts and the latest progress), with
**Stop** (`stop_task`) while the session is live. A finished sub-session leaves
the card; its Agent card in the transcript keeps the report.

Until 2026-10-28 they were a strip above the transcript
(`agentSubSessionStrip`). It deleted and rebuilt every row, and hid and showed
itself, on each task event, so it flashed and pushed the transcript up and
down while subagents ran. The card keeps its rows and only changes their text,
and it sits beside the transcript, so nothing below it moves.

### List views, sort and filters

- The list's toolbar (`agentsActionBar`) is an `OverflowActionRow` too
  (`agents/listActionUses`): up to six of Start all, Stop all, Update all,
  Delete selected, Usage, To desktop, Sort and the rest inline, the queue
  count always, everything else under **More**. A narrow list pane used to
  squeeze the fixed-size buttons on top of each other. The recycle-bin strip
  shows "N deleted" (the full text is its tooltip) and folds Show, Restore
  all and Delete all the same way.
- **View** (`setAgentListView`, `agents/listView`; shared with the Tasks page
  through `forkmesh::ui::ListViewMode`, see [tasks-page.md](tasks-page.md)):
  - **Compact**: lead column (avatar, status, age, branch button, churn) and a
    one-line title. A running session also scrolls its latest prose after the
    title (`agentTickerLine` reduces it to one line of at most 120
    characters; `AgentTickerTrain` scrolls it with a 33 ms timer that stops
    when nothing is painted).
  - **Zen**: titles only.
  - **Full**: title plus a line of icon + word chips (`agentRowChips()` into
    `kAgentRowChipsRole`, painted by `AgentRowActionsDelegate`), aligned in
    columns by slot (`AgentRowChipSlot`, `chipColumnWidths`).
- **Sort** includes **Conflicts**, ranked from the cached `AgentDiffStat`
  without running Git.
- The special-agent filter (`AgentTeammateFilter`) starts at All on every
  launch and is not saved. `switchToAgentsTab` (View agent on a notice or
  ping) must land on the exact run: it widens the filter, returns to the
  session list and clears a top search that would hide it.

### Folders

`AgentFolders` stores per-repository folders in `agents/folders` (one JSON
value keyed by `owner/name`: ordered folders with id, name and collapsed
state, plus a member map keyed by `agentReadKey()`). Folders are local; phones
receive them in `agents.list`, and a change rings `agents.changed`.

- Header rows span both columns (`paintAgentFolderRow`, `kAgentFolderRole`),
  are not selectable and hold `0` in `Qt::UserRole`; code that scans the
  table for a session id must skip them.
- Drag works in every sort (`AgentPriorityTable`): drop on a header to file,
  on a row to join its folder, below the last row to unfile; dragging a
  header (`application/x-forkmesh-agent-folder`) reorders folders. Drops
  rewrite folder state and the list is rebuilt from `AgentFolders::layout()`.
- Automatic filing (`AgentFolders::findOrCreate`): special agent sessions go
  in **Special Agents**, live external Claude Code rows in **External Claude
  Code**, imported Claude Code sessions in **Imported Claude Code**. A row
  already in a folder, or dragged out, is left alone.
- A header's right-click menu has **Start all in folder** and **Stop all in
  folder** (`startAgentFolderSessions`, `stopAgentFolderSessions`). They act on
  the folder's share of the toolbar's Start all / Stop all population
  (`startableAgentSessionIds`, `stoppableAgentSessionIds`), so merged and
  externally started rows are never touched, and each is disabled when the
  folder has nothing to start or stop. Stopping a folder takes only its own
  sessions off the queue.
- Filing external Claude Code rows saves once per scan, on the next event-loop
  turn, and `AgentFolders::save()` skips the write when the stored JSON is
  unchanged. Each newly surfaced session used to rewrite and fsync
  `ForkMesh.conf` from inside the scan result (five stalls of about a second,
  2026-10-22).

### Unread dot

A row is unread when a turn finished after the user last opened it.
`agents/seenFinishedAt` maps read keys to the last seen `finishedAtMs`;
`agents/readBaselineMs` treats older finishes as read. `showAgentSession`
marks read only when the list is on screen in the active window. Phones get
`unread` per row and can call `agents.markRead`.

### Paused and connector-created agents

The MCP `create_agent` tool ([../tools/mcp-create-agent.md](../tools/mcp-create-agent.md))
adds rows through the private intake socket. A paused agent is a `Queued`
session with `held` (and `heldBy`) in its `meta.json`. Held sessions are never
put on `m_agentQueue` and are skipped by `processAgentQueue` and
`AgentStore::queuedSessionIdsOldestFirst`. **Start now**, **Start** or any
prompt sent to it (`continueAgentSession` clears `held`) starts it under the
normal concurrency limit. A connector-created agent can carry an organization
task (`task`), or take over its creator's task (`handoff`), which unlinks the
creator from it. Phones can change the limit and queue order
(`agents.queue.limit`, `agents.queue.move`; see
[../native/DESKTOP_AGENTS.md](../native/DESKTOP_AGENTS.md)).

### Detail pane

- The header's actions live in one `OverflowActionRow` (`agentHeaderActions`,
  `src/OverflowActionRow.*`): the status pill on the left, then at most five
  actions, the branch sync label, and a **More** (⋯) button whose menu holds
  the rest (Listen, Raw, Worktree, Delete, issue and task links, cloud
  actions...). Which five show is decided by use: every click, from the row or
  the menu, is counted in `agents/headerActionUses/<name>`, and the most used
  visible actions win, ties going to a fixed rank (Continue, Stop, Continue
  here, Merge & clean up, Create PR, PR, Pull main, Code...). When the pane
  is too narrow even for those, the lowest-ranked ones move into the menu
  too, so the row never overlaps or runs off the right edge. Hidden actions
  stay real buttons, parked off-screen, so every existing visibility rule and
  click handler is unchanged.
- The title (`WrappedTitleLabel`) wraps up to `kAgentTitleMaxLines` lines.
  `showAgentSession()` reopens a detail pane collapsed to zero width.
- **Code** opens the live diff right of the transcript (`agentCodeSplit`),
  polling the worktree every 3 s while a run is live and visible.
- Branch sync controls (`refreshAgentSyncControls`, from `m_agentDiffStats`):
  a label with ahead/behind against the session's own base, and a button that
  is **Pull <base>** (`updateBranchFromBase()`) or **Fix conflicts**
  (`fixAgentConflictsWithAgent()`), disabled while a merge flow runs.
- **Add task**, **Create task from this agent** and **Attach to an existing
  task...** link an agent to an organization task ([tasks-page.md](tasks-page.md)).
- Cloud rows have their own actions (see
  [Cloud runs](#cloud-runs-in-the-agents-list)).

### Transcript

`ClaudeTranscriptView::handleEvent` renders Claude Code and Codex stream
events for local, external and cloud runs alike.

**Command rows.** A Bash call folds to one line (`CommandCard`): icon tiles
(up to ten, then `+N`), the call's description (or command), `N lines · Ns`,
the approval mark (✓ allowed, ? asking, ✕ denied; click opens
`src/MainWindowCommandPermissions.cpp`) and an expand caret. The terminal
panel opens only on click. Icons come from `transcripttools::commandIcons`,
which splits the command on pipes, `&&`, `;`, subshells and substitutions,
skips heredocs, redirects, assignments and wrappers (`sudo`, `env`,
`timeout`, …), and maps each program to an icon in `resources/icons/commands/`
(terminal icon as fallback). Hovers use `CommandHoverCard` via
`HoverCardHost`, not `QToolTip`, which blinks on re-layout.

**Invariant:** the approval resolver (`installAgentCommandApprovals`) runs
while drawing and must never wait on Git. It copies the session's mode,
repository and branch first and reads the worktree from
`cachedSessionWorktreeForUi()`, which never blocks; a nested event loop there
can free the session it holds.

**File edit rows.** Edit, MultiEdit, Write and Codex `FileChange` fold the
same way (`FileEditCard`): file link and `+N −M` (`transcripttools::editStats`,
line LCS); expanded, only changed lines show.

**Prose.** Rows have no bubble; the caption shows a portrait, name and full
date and time (streamed text is dated from its `message_start` `_ts`). Each
reply has a copy button that copies the stored Markdown (`proseMarkdown`).
Caption labels must be created as children of the caption row: a visible
parentless widget becomes a top-level window and crashes on Wayland.

**Subagent cards.** An Agent (Task) card ends with **Summary** (the report's
lead paragraph) and a folded **Full report**. `transcripttools::agentReport`
strips hand-back framing and usage blocks; a `task_notification` with `usage`
is matched to its card by `tool_use_id` (replays parse
`<task-notification>` text).

**References.** `transcriptReferenceRegex()` links `task:<id>` and similar
forms as `forkmesh-task:` before the commit alternative; a bare 32-hex id
links as a commit.

**Live task list (2026-10-25).** `AgentTaskListBar` sits below the rows, in
the bottom viewport margin `layoutPromptDeck()` reserves, and shows the
agent's current task list the way Claude Code desktop pins its list under the
latest message: updated in place instead of a new `TodoWrite` row scrolling
away with every change. Before this, the list only existed as those rows, and
Claude Code's newer `TaskCreate`/`TaskUpdate` tools showed as one-line rows
with no list at all.

- `agenttasks::Tracker` (`src/AgentTaskList.cpp`) folds the stream:
  `TodoWrite` and Codex plans replace a list; `TaskCreate` adds a task at once
  and takes its id from the `Task #N created` result (a failed create is
  dropped); `TaskUpdate` changes status, subject or active form, and
  `deleted` removes the task.
- Codex sends `turn/plan/updated` several times a turn, but
  `CodexAppServerSession::emitToolUse` passes only the first one per turn
  (the synthetic id is `plan-<turnId>`); later ones come only as
  `_codex_tool_delta`. The tracker reads those deltas, so a Codex plan
  updates live.
- Subagents keep their own lists. Events with a `parent_tool_use_id` go to a
  group headed by the Agent/Task call's `description` (`⎿ Explore the parser`),
  shown indented under the main list. The group goes away when that subagent's
  tool result arrives, as the subagent is done.
- Wording and marks follow Claude Code: the header is
  `5 tasks (2 done, 1 in progress, 2 open)`, rows are `◻` open, `◼` in
  progress (bold, showing the `activeForm`), `✔` done (struck through). At
  most `kMaxRows` (8) rows show, then `… +N more`.
- The bar emits `heightChanged` only when its text, fold or visibility
  changes. `clear()` resets it on every session switch, and an unconditional
  signal ran `layoutPromptDeck()` mid-replay, so a session with no tasks could
  lose the bottom-follow latch ("a deferred Agent transcript replay stays
  pinned to its latest turn").
- Click the header to fold the list to one line, which then names the
  current task. The bar folds itself when every task is done and opens
  again when new work arrives, unless the user folded it. A new prompt after
  an all-done list clears it.
- The Agents list shows the same progress on each row after the title,
  e.g. `◼ 2/5 · Running tests`, `◻ 0/3` (nothing started) or `✔ 5/5`.
  `MainWindow::m_agentTaskTrackers` keeps one tracker per session, fed by
  `applyTranscriptEvent` through `feedAgentTaskTracker`, and built from
  `m_streamEvents` the first time a row asks (`agentTaskProgress`); a session
  whose history is not loaded shows nothing rather than an empty tracker
  being cached. Running rows refresh it on the spin tick, which already
  repaints them, by recomposing the text from `kAgentTitleBaseRole` and
  `kAgentTaskProgressRole`, so a task change costs no table rebuild.
- Events skipped when a long session opens on its tail still pass through
  the tracker (`accumulateStatsOnly`), so the list is right after reopening.
  Earlier events loaded by scrolling up (`prependEarlierEvents`) are not fed,
  since they are older than what the tracker already holds.

**Pinned prompt.** `PromptDeckBar` sits above the rows, in the top viewport
margin `layoutPromptDeck()` reserves, and shows the last prompt in one line
(2026-10-22). It is the desktop version of the phone's prompt deck:

- Edges peek out below for earlier prompts. With several prompts it shows
  `2/5` and ‹ › arrows.
- To move between prompts, drag the card sideways, scroll sideways on a
  trackpad, or press ← →. Right goes to the previous prompt, left to the next.
- Click to read the whole prompt (up to eight lines). Right-click to copy it
  or jump back to the latest.
- Prompts come from `addUserTurn` and also from `accumulateStatsOnly`, so the
  turns a long replay skips still count.
- `prependEarlierEvents` does not add them again, and `clear()` empties the
  bar.

**Raw log jump.** Clicking a row's gutter switches to Raw at the line of the
event that drew it (`m_rowSource`, `transcripttools::rawLogLineStart()`).

**Transcript coverage (2026-10-25).** The transcript should show everything
the raw log holds. `forkmesh::transcriptcoverage` (`src/TranscriptCoverage.*`)
checks every event the view is given (`handleEvent`, and
`accumulateStatsOnly` for the turns a long replay skips). Any kind that draws
nothing is a gap: an unknown event type, `system` subtype, assistant or user
content block, stream event or delta.

Some raw log lines never become an event, so the sessions record them. Each
one becomes a `_local_raw_line` event that is stored with the session, so the
check survives a redraw or a restart. The kind is worked out again from the
stored line each time, so a fix also clears old sessions. Lines longer than
16,000 characters keep only the fields that name their kind.

- Claude Code (`ClaudeStreamSession::noteDroppedLine`): stdout that is not
  JSON, and control requests that `handleControlLine` consumes without a
  card. Those are requests ForkMesh refuses (`hook_callback`, …) and dialogs it
  cannot answer (`request_user_dialog` of an unsupported `dialog_kind`).
  Answers to ForkMesh's own control requests and stale cancels are
  bookkeeping.
- Codex (`CodexAppServerSession::noteUnshownMessage`): notifications that
  `handleNotification` falls through, items that are neither tool cards nor
  drawn items, and server requests it refuses. A red **● N
missing** pill (`transcriptCoverageGaps`) then shows at the top right of the
transcript. Its tooltip lists each kind and count. Clicking it starts an agent
in the ForkMesh checkout (`stallReportRepoIndex()`), with the default provider
and model from Settings, like the Tool efficiency page's **Send to agent**. The
prompt (`transcriptcoverage::agentPrompt`) names each kind with one clipped
example. The agent either draws the kind or adds it to the unshown lists below
with a reason here, then updates `TranscriptCoverage.cpp` and
`tests/test_transcript_coverage.cpp` so the pill stops flagging it. A kind
already handed to an agent that still exists is not sent again; a second click
opens that agent.

Claude Code `system/vcs_state_changed` draws a muted notice through
`transcripttools::systemNotice`, including its `kind` (for example, push).
An absent kind still draws a generic version control state change notice.

Claude Code `system/dev_intent` draws the same muted notice style, including
its development kind and trigger when present. `command_lifecycle` draws a
muted notice with the command state and `command_uuid`, so starts and later
updates can be matched. Missing fields still produce a generic notice, and
unfamiliar kinds, triggers and states are shown as received. These rows keep
their source event for the raw log jump and appear during live updates and
history replay in both layout modes.

Codex `remoteControl/status/changed`, `mcpServer/startupStatus/updated`, and
`thread/settings/updated` draw the existing notice rows. Remote control notices
name the server and status; MCP notices name the server and startup status,
with failures in the error style and any error or failure reason included.
Thread settings notices summarize the model, effort, approval policy, working
directory, sandbox policy, and collaboration mode, without repeating developer
instructions. These rows use the shared transcript in both layout modes.

These are deliberately not drawn, because another surface shows them or they
are bookkeeping:

- Usage page: `rate_limit_event`, `_codex_rate_limits`, Codex `account/*`.
- Codex `turn/moderationMetadata`: internal safety-check bookkeeping, including
  category flags and empty generation/tool metadata. It contains no transcript
  content; user-facing failures already arrive through the normal error and
  turn-result rows. The metadata remains available in Raw.
- Host control protocol: `control_request`, `control_response`,
  `control_cancel_request`, `keep_alive`. Prompts arrive as their own card
  events.
- Sub-sessions list: `system` `task_started`, `task_updated`,
  `task_progress`, `background_tasks_changed`.
- Agent name: `system/session_title_changed`.
- Only `status: compacting` has a row; other `system/status` events only
  update state.
- Claude Code `.jsonl` bookkeeping: `summary`, `file-history-snapshot`,
  `queue-operation`, `custom-title`, `ai-title`, `last-prompt`, and
  `isMeta` lines.
- Subagent work: any event with a `parent_tool_use_id`, and sidechain
  events. The subagent's card summarizes it.
- Prompt echoes: user `text` blocks and string content. The prompt row
  shows them.
- Streamed `input_json_delta` and `signature_delta`: the finished assistant
  message draws the tool call.
- Codex `thread/status/changed`, `thread/archived`, `thread/unarchived`,
  `thread/closed`, `serverRequest/resolved`, `skills/changed`,
  `app/list/updated`, `mcpServer/oauthLogin/completed`,
  `fuzzyFileSearch/*`, and the legacy `codex/event/*` duplicates.

Stderr is never judged. `CodexAppServerSession::isToolItem` reads the same Codex tool item
list (`transcriptcoverage::isCodexToolItem`), so the two cannot drift.

**Scrolling.**

- The follow-tail latch (`m_stickBottom`) changes only on
  `QScrollBar::actionTriggered`, never on programmatic clamps.
- `ScrollJumpButtons` shows a jump-to-latest arrow off the tail, with a dot
  when new content arrived.
- Rebuilding the transcript on screen holds a grab of the last frame
  (`setBatchHoldsFrame`) and restores the position in the same thaw.
  `CacheLabel` estimates heights only from its on-screen width.
- Sending a follow-up jumps to the tail; a resume keeps the rendered rows; a
  cloud follow-up is drawn at once until the relay history replaces it.
- Cloud runs and external Claude Code sessions redraw the whole transcript when
  the relay history or the `.jsonl` grows. Redrawing the session already on
  screen goes through the same held frame and keeps the reader's place. It
  used to end in `jumpToBottom()`, which pulled anyone reading earlier turns
  back down on every update (2026-10-21). Only opening a session, or a reader
  already at the tail, lands at the end.

**Keeping live updates cheap (2026-10-22 stall log).**

- A live event appends rows; it never rebuilds the view. When the view is
  behind by up to 32 events (a modal dialog was open, or a notice was added
  from inside another event's handling), `applyTranscriptEvent` draws the
  missing ones in place; a longer gap goes through `renderTranscriptTail`.
  Only a session that is not on screen yet takes `renderTranscriptForSession`.
  Rebuilding on every event froze the window for up to 17 s
  (`beginBatchBuild` grabbing and hiding the whole column per event).
- The time-sliced replay reads `m_streamEvents` by index instead of keeping
  its own copy. The copy shared the list, so every live event appended
  during a replay copied the whole history.
- `ClaudeTranscriptView::clear()` hides the old rows and deletes them a few
  at a time, 8 ms per event-loop turn (`retireRow`, `deleteRetiredRows`).
  `deleteLater()` on every row destroyed hundreds of command cards in one
  deferred-delete pass, up to 22 s (`~CommandLineView`, `~TranscriptPanel`).
- Deleting a session hands its event list and raw buffer to a pool thread to
  free (`releaseOffGuiThread`, `MainWindowAgentsInternal.h`); `QJsonObject` is
  implicitly shared, so whichever owner drops the last reference frees it.
  Erasing a long history from `m_streamEvents` took 1.7 s on the GUI thread.
- The raw buffer keeps its last 300,000 characters as before, but a line
  longer than that replaces the buffer with its own tail instead of being
  appended and then cut (`appendBoundedStreamRaw`).
- Rate-limit events from a transcript are applied at most once every 2 s,
  latest payload wins: each change rewrote a dozen settings and the nested
  `QSettings` in `refreshCodexUsageRemaining` synced the file mid-event.
- `disabledAgentToolFamilies()` is cached and updated by
  `setAgentToolFamilyEnabled`; it was read from `QSettings` for every event.

### Composer

The prompt box (`src/PromptDraftStore.*`, `src/PromptSpellCheckHighlighter.*`,
`src/AgentPromptImages.*`) keeps drafts, spell-checks and takes images.

| Control | Setting | Effect |
| --- | --- | --- |
| Branch toggle (`promptWorktreeCheck`, git-branch icon) | | Own worktree, or the checkout directly |
| **Cloud** (`promptCloudButton`) | `agents/composerRunInCloud` | **new** queues the run on an account agent node (`startQuickAddCloudAgent`, signed `POST /api/repo/<owner>/<repo>/agent-sessions`); Claude Code and Codex, published repositories, no attachments |
| **Vibe** (`promptVibeCheck`, sparkle icon) | `agents/composerVibe` (off) | Stamps `AgentSession::vibe`; a successful worktree run is merged with clean-up (`maybeAutoMergeForSession`) and pushed (`pushCurrentRepoUpstream`). All merge gates apply |

- The three toggles are icon buttons in the composer's floating button bar
  (`FloatingPromptBar`, `promptDragHandle`), after the channel picker and
  before the clear (×) and reset buttons. An "on" toggle gets a
  thin accent border, so the branch icon (on by default) reads as connected.
  They were a checkbox, a checkbox-plus-icon pair and a "vibe" checkbox on the
  left until 2026-10-22, which crowded the grip and hid what was on.
- The bar is not a row of its own: it floats over the composer's top-right
  corner, so the chat preview starts at the very top. Its left edge fades from
  transparent to the composer's surface, so a chat line running under the
  buttons fades out instead of colliding with them. With no chat to show, a
  band of the bar's height (`promptTopSpacer`) keeps it off the prompt text.
  The whole body drags the composer, not just the bar: `promptWrapper` and
  every widget in it that is not a control (`isPromptDragSurface`: not a
  button, picker, text field, slider or list) are watched, so a press on the
  chat lines, the bottom bar's empty space or the gaps around the send
  column that moves past the drag distance hands off to the bar's drag
  (`handlePromptPlacementEvent`). Each widget is watched directly because a
  rich-text label keeps its own presses instead of passing them up. The
  composer can be moved anywhere or dragged out of the window into its own.
  The prompt text, buttons and pickers keep their presses, and a plain click
  on a chat line still opens its channel. This works from the centred
  Agents-page layout too: the first drag lifts the composer out to float.
  Until 2026-10-22 the bar was a full-width
  strip with a centre drag pill and the grip on the left.
- **Channel picker** (`promptChannelButton`, `m_promptChannelButton`): the
  bar's first control names the chat channel the composer is in
  (`m_currentConversation`) with a ▾. Clicking it opens `promptChannelMenu`
  (`showPromptChannelMenu`): one row per channel (the sidebar's `m_channels`
  plus `unlistedChatConversations`), then a "Direct messages" section with
  one row per direct message (`m_openDms`, `m_officeDirectConversations` and
  unlisted ones), each with its user and chat counts after a tab, the current
  one checked. Picking a row calls `switchConversation`. The last row,
  "Message someone…" (`promptChannelMessagePerson`), opens the new-message
  people picker (`showNewMessageDialog`), so the composer can send straight to
  a person who has no conversation yet. Until 2026-10-24 direct messages were
  left out of this menu unless one was open. Users come from the
  office mirror's member count, else the open room's members button, else
  the distinct senders in the loaded history plus you; chats are the loaded
  top-level messages. Until 2026-10-24 this spot showed the characters left
  of the 16,000 the prompt holds; the limit still applies, uncounted.
- The reset button (`promptResetButton`, sync icon) at the bar's right end
  snaps the composer back to its default size and place
  (`resetPromptOverlayPlacement`), as double-clicking the bar does. There is
  no pop-out button since 2026-10-24: dragging the composer out of the window
  still tears it off into its own window, and a double-click docks it.
- **Resizing**: every edge and corner of the composer is a resize handle
  (`PromptResizeEdge`, `promptResizeEdge`, eight invisible bands 5 px thick
  with 10 px corners, children of `promptOverlayHost` laid over
  `promptWrapper`). The cursor shows the direction; dragging moves only the
  grabbed edges (`resizedPromptRect`), keeps the 280×120 minimum and stops at
  the window's edges. A docked composer lifts off the footer on the first
  resize, as it does on a drag; a popped-out composer asks the window system
  to resize from that edge (`startSystemResize`). The main window's own
  frameless-border handler (`handleFramelessResizeEvent`) only takes presses
  and cursor changes from widgets in the main window: Wayland reports every
  window at the origin, so until 2026-10-31 the popped-out composer's left and
  top bands fell inside the main window's 8 px border, and a press there
  resized the main window instead of the composer. The bands show whenever the
  composer does, the centred Agents-page layout included (its first resize
  lifts it out to float); only the collapsed avatar has none. Until
  2026-10-24 they hid with the button bar, so the centred composer could not
  be resized. The bar keeps a 10 px right margin so the top-right
  corner never covers its buttons. Until 2026-10-23 a small grip at the right
  end of the bar was the only handle and always resized from the top-right
  corner.
- **Chat preview** (`ChatPreviewLabel`, `promptChatPreview`): up to three
  one-line rows over the prompt with the three latest top-level chat messages
  across every loaded conversation (`m_history`), newest last. Each row is
  the sender's small round avatar (16 px, `chatActorAvatar`), then the
  channel (muted), the user (bold), a compact time (`now`, `5m`, `3h`, `2d`,
  then the date, muted) and the message, elided to the width. Clicking a row
  opens its channel on the Chat page. The rows never come from agent
  sessions: until 2026-10-24 they followed the agent open above when no chat
  room was open. With a chat room open on the Chat page (`promptChatRoom`),
  Enter (and the centred layout's send button) post the prompt to the room
  through **chat** (below). `refreshPromptChatPreview` runs (debounced
  250 ms) whenever a conversation's sidebar preview changes
  (`refreshChatListPreview`). The rows are painted at a fixed pitch (avatar
  height plus a 4 px gap) and the docked composer's height is recomputed
  whenever the row count changes (`baseAnchoredHeight`), so the third line is
  never clipped; before 2026-10-22 the height only followed showing or hiding
  the preview, which cut a third line in half.
- The follow-up button always reads **add**, chat preview or not (the centred
  workspace layout's send button reads **Add** when it is the Enter target).
  For part of 2026-10-22 it read **chat** over a preview; the label went back
  to **add** because "chat" now means a chat channel (below).
- The top of the send column is **chat**
  (`m_quickAddSendToChannelButton`, object name still `quickAddGenieButton`
  for the stylesheets): it sends to the channel the picker names
  (`m_currentConversation`, labelled by `chatConversationLabel`; the tooltip
  and accessible name carry it, the label read **send to: #channel** until
  2026-10-24) and posts the
  prompt there through `sendChatText`, the same path as the Chat page's own
  composer (`sendCurrentMessage`), refusing attachments and a room you cannot
  post in. `updateChatConversationHeading` and `updateQuickAddEnterTarget`
  refresh its label. While the Chat page is open (`promptChatRoom`) it takes
  Enter, and **add** and **new** step aside. It also carries the
  `enterTarget` highlight then (the accent fill in the new interface, a solid
  purple border in the classic one), so the button Enter will press is the one
  lit up; until 2026-10-24 only **add** and **new** were ever highlighted,
  which left nothing lit in a chat room. A package's own prompt takes the
  highlight back. For part of 2026-10-22 the
  follow-up button did this as **chat**, by pasting the text into the room's
  own message box (`sendPromptToChatRoom`). It replaced
  the **task** button, which filed the prompt in General: that lives in the
  agent/model dropdown as **Task · assign**, where leaving the assignee on
  Unassigned files the task in General.
- **new** (plus), **add** (paper airplane) and **chat** (comment) carry
  small icons in both interfaces (`tintComposerActionButton`; on the accent
  fill of the Enter target the icon uses the on-accent colour).

- The repository comes from `resolveAgentRepositoryIndex` for local and cloud
  runs. Phones expose the same switches through `agents.start`.
- Dropping local files on the prompt attaches images and videos and inserts
  `@` mentions for anything else; typing `/` into an empty prompt opens the
  "/" menu; Shift+Tab picks the next permission mode
  ([claude-code-parity.md](claude-code-parity.md#what-was-added-on-2026-10-25)).
- After a run's worktree is added, a `.worktreeinclude` in the checkout copies
  the ignored files it lists (`.env` and the like) into it before the agent
  starts (`copyWorktreeIncludes`, same page).
- A run's worktree is added by `agentwt::addScript`, retried three times. A
  `git worktree add -b` that fails after making the branch (for example with
  the disk full) leaves `<id>-<slug>` behind, so the script checks the branch
  out when it already exists and creates it only when it does not. Before
  2026-10-18 every retry then failed with "a branch named … already exists"
  (runs 2256 and 2257). The error shown keeps the first attempt's output
  when it differs from the last, so the real cause is not hidden.
- Dragging the strip moves the composer; dragging it out tears it off into a
  frameless window (`tearOffPromptOverlay`). Double-click docks it.
- Notices are inline in the page, never floating (see
  [diagnostics.md](diagnostics.md)).
- Sending plays a `PromptFlightOverlay` (sparkle to the Agents rail button,
  or a floating bubble) instead of a card; the overlay is click-through and
  deletes itself.
- `voice/inputEnabled` (on) shows the dictation mic.
- The agent/model dropdown (`quickAddAgentModelSelector`,
  `fillAgentModelSelector`) also offers destinations that start no agent
  (`quickAddProviderIsPromptAction`): **Manual · create issue**,
  **ActivityPub · post**, **Take a note** (`takeQuickAddNote`: a local note in
  `<data>/notes/notes.json`, first line as title, attachments as image links)
  and **Task · assign**. Task shows an assignee picker
  (`quickAddTaskAssignee`, last pick in `tasks/composerAssignee`) listing the
  organization's task members, you first; submitting posts `/api/tasks` with
  `assigneeKind: user` and that member, or unassigned to General. Members
  come from the task catalog (`organizationTaskMemberNames`); picking Task
  before the catalog was read starts the launch read.
- **Review with AI** on a pull request (`reviewCurrentPullWithAi`) runs with
  the agent and model the composer has selected (2026-10-20), so the review
  uses the same agent you are already working with. Claude API and OpenAI
  with a saved key send one request straight to the API; every other agent
  runs its own command from `agentConfigForProvider` once, read-only, in the
  repository's checkout (`aiReviewRunAgentCli`). The review only falls back
  to choosing for itself (Claude key, then OpenAI key, then Claude Code) when
  the composer is on Manual, ActivityPub, Note or Task. Some CLIs, such as
  Codex, print the prompt back before their answer, so
  `parseAiReviewFindings` tries again from each earlier `[` before the
  reply's last `]` until it finds a JSON array.
- The dropdown's last row, **Edit models…**, opens the Models board
  (`openComposerModelsPage`), whose **Show** column ticks which models the
  dropdown lists (`agents/composerHiddenModels`, same list as Settings →
  Composer models).

### Provider accounts and usage

- The bottom bar shows the usage meter of the composer's provider. Its menu
  (`showAgentAccountMenu`) has per-account **Edit**, **Log in**, **Log out**,
  **Delete** and a terminal button that opens the CLI as that account
  (`CLAUDE_CONFIG_DIR` / `CODEX_HOME`).
- **Delete** (`confirmDeleteAgentAccount`, `deleteAgentAccount`) asks first,
  switches new agents to the default account if the deleted one was selected,
  drops the profile, identity, label and cached usage settings, and removes the
  login file (`.credentials.json` / `auth.json`). The account folder stays: it
  holds the account's agent history, and running agents and peer messaging
  (`ClaudePeerMessaging.cpp`) still read it. A login still parked under
  `claude-login/` is moved to `agent-accounts/<provider>/<id>` so the next
  sign-in does not re-file it as a new account. The default account is the
  CLI's own home folder (`~/.claude`, `~/.codex`) and cannot be deleted; sign
  it out with **Log out**. On macOS a Claude login kept in the Keychain is not
  touched.
- Clicking a usage icon (the **Usage** button atop the agents list and the
  Claude Code and Codex meters at the bottom right of the prompt) opens the
  in-app **Usage** page (`openAgentUsagePage`, `MainWindowAgentUsagePage.cpp`).
  It lists Claude Code and Codex, the default provider first, and carries
  everything the right-click menus offer: **Open … in a terminal**
  (`openAgentProviderTerminal`), **Launch in system terminal**, **Add
  account…**, **Runner accounts**, **Use … for new agents**, and per account
  **Run new agents as this account**, **Edit account…**, **Log in…**, **Log
  out…**, **Delete…**, the terminal launch button and each usage window's bar
  with its reset countdown button. The page reads this desktop's settings, so
  it works without a ForkMesh host, and it rebuilds after usage responses and
  account changes (`scheduleAgentUsagePagesRefresh`). When the Usage package is
  installed its rail entry hosts the same page (`NativePackageHost::agentUsagePage`,
  permission `desktop.models.usage.read`) and a usage icon opens that entry;
  otherwise the page is a section of its own under **Agents**. Right-click
  still opens the account and terminal menus below. From 2026-10-18 until
  this change a click opened the web `/dashboard/usage` page; before that it
  opened the menu.
- **Hover card (2026-10-22).** Hovering the Claude Code or Codex usage chart
  in the bottom bar, or the agents toolbar's **Usage** button, shows a card
  (`agentUsageHoverCard`, `MainWindow::attachAgentUsageHoverCard` and
  `fillAgentUsageHoverCard` in `src/MainWindowAgentUsagePage.cpp`) instead of
  the plain text tooltip. It holds the active account's usage bars
  (`TokenUsageMenuRow`, with the 12-hour reset clock and the reset countdown
  button), the session stats line, and the Usage page's controls for that
  account: Refresh, Open the Usage page, Open in a terminal, Launch in system
  terminal, Edit account…, Log in…, Log out…, Add account…, a switch to each
  other account, Runner accounts… and **Use … for new agents**. The card is a
  `Qt::ToolTip` window that takes clicks; it opens 350 ms after the pointer
  enters, stays while the pointer is on the chart or the card, and closes
  about 360 ms after it leaves both, or on a click on the chart. It refills
  from `scheduleAgentUsagePagesRefresh` when usage changes, and skips the
  rebuild when nothing it shows changed, so a click is not lost to a refill.
  The chart's tooltip text is kept for accessibility and the toolbar button.
  `forkmesh-window-tests --usage-hover-only` checks both providers' cards
  (`checkAgentUsageHoverCard`).
- Both account popups start with **Open the Usage page**
  (`addAgentUsagePageAction`), which opens the in-app page.
- Usage rows (`TokenUsageMenuRow`) segment the bar per hour or day of the
  window, mark even pace and add a pace note (`usagePaceNote`).
- **Usage** (`agentUsageButton`) opens the in-app Usage page; its right-click menu
  (`showAgentUsageMenu`) shows the default
  provider's bars and offers terminals, **Run new agents as**, account
  management and the default provider switch. **Runner accounts**
  (`fillRunnerAccountsMenu`) switches a runner's account.
- **Claude account routing.** Each account is tied to one Claude login
  (`accountUuid`/`organizationUuid`, saved under
  `agents/accountIdentity/claude/<account id>`). Logins run in a fresh folder
  under `<data>/agent-accounts/claude-login/`. **Log in** on an account always
  files the login on that account, even as a different Claude identity: it
  replaces that account's login and rebinds it, and never adds an account.
  **Add account** reuses the account that already holds the identity and only
  adds one for an identity no account has. A login made outside ForkMesh in an
  account's folder (`/login` in its terminal) stays there:
  `reconcileClaudeAccountLogins` rebinds the account to it. Before 2026-10-16
  a login as another identity was moved into a new account labelled with the
  email. Files move by copy then remove, never by deleting folders.
- Accounts are shown by the Claude display name (`oauthAccount.displayName`
  in the account's `.claude.json`), never the email; a new account with no
  typed label is named after it.

### Usage-limit auto-continue

A run that fails on a usage limit is held under
`agents/usageLimitAgentHold/<id>` with the reset time parsed from the refusal
(`usageLimitResumeAtMs`), else the provider's window reset. A timer, the
reset ping or startup (`restoreUsageLimitAgentHolds`) continues it through
`continueAgentSession` if it is still failed on that limit. Only the refusal
is parsed, never the log tail. Setting: `agents/usageLimitAutoContinue` (on).

### When a run counts as done

The "Agent #N is done!" ping (`notifyAgentDone`) only goes out once a run
reaches Success. A clean `result` from the Claude CLI ends a turn, not the
task. An agent that started background work (a background subagent, a
`run_in_background` shell, a Monitor) gets a `result` while that work is still
running, and the CLI starts a new turn when each task reports back. Before
2026-10-15 every such `result` set the run to Success and sent a ping, so one
task could ping several times before it was finished (session #2066 pinged
four times).

Now `forkmesh::agentclitasks::Ledger` (`AgentCliTasks.h`) keeps the run's
`system` `task_started` / `task_updated` / `task_notification` events per
session (`m_agentCliTasks`). A task counts against the run while it is
backgrounded (`is_backgrounded`, or a `task_updated` patch that moves it to the
background), until a notification or a terminal status (`completed`, `failed`,
`killed`, `stopped`) closes it. A clean `result` that arrives while any
background task is open keeps the session Running, with no Success and no
ping. It posts a transcript notice naming the tasks and marks the session in
`m_agentResultAwaitingCliTasks`. The turn the CLI starts when the tasks report
back ends with its own `result`, and that one goes through the usual
subprocess drain to Success and the single done ping.

Fallbacks:

- If the last task closes and no new top-level turn starts within 30 s
  (`kAgentCliTaskSettleMs`), `settleAgentAfterCliTasks` finishes the run from
  the held `result`.
- If the CLI exits while a run is held, `finishCliTranscriptRun` treats the
  held `result` as the completion.
- A new CLI process for the session clears the ledger, so tasks from a stopped
  process cannot hold the next run.

A background task that never reports back keeps the run Running until it is
stopped. The unit test is `forkmesh-agent-cli-tasks-tests`
(`desktop/tests/test_agent_cli_tasks.cpp`).

## Sessions page

Rail **Sessions** (`sessionsNavButton`, rail key `sessions`, section
`kSessionsSectionIndex`), directly above **Agents** (2026-10-22). It is the
Claude-desktop way to manage the same sessions the Agents page lists: one list
of every session, the open session's live transcript, and a composer that can
talk to it or relay between sessions.

| Part | Files |
| --- | --- |
| Page, list, relays | `src/MainWindowSessions.cpp` |
| Styles | `resources/theme/next/63-sessions.qss` |
| Hooks | `applyTranscriptEvent` (`MainWindowAgentEvents.cpp`), `refreshAgentTable` and `refreshAgentDotMatrix` (`MainWindowAgentList.cpp`), `applyExternalClaudeScan` (`MainWindowAgentCliSession.cpp`), `refreshAgentTranscriptTheme` |

**Rail.** The button is in `m_navGroup`, so it is checked by `showSection` like
every section button, and the Agents button unchecks through
`updateRepoActivityRail`. It sits in the Work group, follows the `agents`
package (`syncPackageSurfaces`, desktop-offline dot), and the Code button is
inserted above it. A saved rail order from before the button existed places
it just above Agents (`applyRailOrder`). It ranks a quarter step before
Agents, not half a step: icons missing from the saved order rank half a step
after their saved neighbour, so at half a step Sessions tied with them and,
coming earlier in the default order, pushed itself between an icon and its
default neighbour (a saved order of Files then Agents put Sessions between
Files and Stats).

**Layout.**

- Sidebar: **New session** (the page's one green button), search, a
  **Between sessions** filter, then **Active** (Waiting first, then Running,
  then Queued) and **Recent** (newest first, at most 300). Each row is painted
  by a delegate: status dot, title, repository · branch · model, age, unread
  dot and a `⇄ N` badge when the session sent or received messages from other
  sessions. Right-click: Open in Agents, Continue, Stop, Send message to
  another session…, Delete (confirms through `deleteAgentSessionFromRow`).
- Centre: header (title, status chip, repository, branch, provider/model,
  mode, Stop, Continue, Open in Agents), the **Between sessions** strip (the
  last six messages to or from the open session; click one to open the other
  session), and the page's own `ClaudeTranscriptView`. It never borrows the
  Agents page's view.
- Composer: Enter sends, Shift+Enter adds a line. **To** defaults to the open
  session; **Also send to** sends the same text to a second session;
  **Forward last reply to…** sends the open session's latest reply to another
  session. In new-session mode the row shows Repository and Agent instead,
  and Send calls `startAdHocAgentForRepo` without leaving the page, then opens
  the new session.

**Side panels (2026-10-27).** The right column of the new interface is
`SessionSidePanels` (`src/SessionSidePanels.*`), a stack of cards:
**Progress** (the to-do list from `TodoWrite`/`TaskCreate` and Codex plans,
through `agenttasks::Tracker`), **Sub-sessions** (the live subagents, shells
and workflows with Stop, see "Sub-sessions" above), **Outputs** (files the agent wrote or edited,
newest first, with line counts; a failed edit is taken back), **Sources**
(files read, pages fetched, web searches, skills and connectors), **Background**
(subagents and background shells with running/done/failed/stopped),
**Changes**, **Context**, **Environment** and **Tools** (calls per tool and
failures). Outputs, Sources and Background come from
`sessioninsights::Collector` (`src/SessionInsights.*`, no widgets, tested by
`forkmesh-session-insights-tests`), which reads the same events the transcript
gets: `renderSessionsTranscriptSlice` hands every queued event to the panel,
and the panel folds them in 8 ms slices only while it is visible.

- Every card is always shown, with a one-line hint while it is empty, so a new
  session shows the full set from the first prompt.
- Drag a card's header to move it; the others make room as it passes and the
  list scrolls at the edges. Click the header to fold it. Right-click for Move
  up, Move down, Fold, Hide, Show panel and Reset panels.
- Order, hidden and folded cards are kept in `sessions/panelOrder`,
  `sessions/panelHidden` and `sessions/panelCollapsed`, shared by the Sessions
  page, the Agents page's **Details** tab in Focus and its right column in
  Control Tower. `normalizedCardOrder` keeps a
  saved order valid when a card is added: a new card goes after its default
  neighbour.
- Clicking an output or a read file opens it; a page opens in the browser.
- The column opens whenever a session starts: **New session**, sending the
  first prompt, and every local launch (`startAgentRunner` calls
  `NextAccess::sessionStarted`) set `panelOpen` again even if it was hidden.
  The new-session screen shows the column too.
- **Context (2026-10-31)** reads like Claude Code's context panel: "N% used"
  and used / window above the bar, then rows with a coloured swatch, value and
  share: Used (blue), Free space, Input and Output tokens (share of all tokens
  in the session), Turns and Cost. Sessions that only report a total show one
  Tokens used row. The bar and the Used swatch turn amber at 80% and red at 95%
  (`sessionsLevel` on `sessionsContextBar`). The header's percentage shows only
  while the card is folded, since the open card already says it.
- **Environment** ends with a **Remote control** switch (2026-10-31), the
  global phone-control switch (`localControl/remoteControl`, on by default;
  `MainWindow::setRemoteControlOn`). Under it, a connected phone shows its name
  and whether it reached the desktop on this network or through the relay; a
  paired phone that is offline shows when it was last seen; with no phone
  paired, a note explains how to connect one and links to Settings > Phone.
  Off makes `LocalControlServer` answer every paired request with
  `remote_off` and stop pushing events; pairing and `ping` still work so a
  phone can tell the desktop is there. Both phone apps word `remote_off`.

**Sources.** Local sessions render `m_streamEvents` (loaded with
`ensureStreamEventsLoadedAsync`; a session with no events shows its prompt and
last reply). External Claude Code sessions read their `.jsonl` off the GUI
thread from the page's own offset (the last 400 KB first, then only what was
appended), redacted like the Agents page. Cloud runs render
`cloudAgentTranscriptEvents` and redraw when the relay history changes.

**Sending.** Local and cloud sessions go through `sendPromptToAgentSession`.
An external Claude Code session is messaged through `ClaudePeerMessaging`
(the same peer check as the phone's outside-session prompt: refused or held
messages are reported). One with no messaging socket shows the composer
disabled with the reason.

**Messages between sessions.** A forward or a context-menu message is sent
as the source session, not as the user: `hub::messageEnvelope`, the same
permission rule as agent-to-agent messages (`hub::messageAllowed`: a session
in a less permissive mode may not direct a more permissive one; an outside
session's mode counts as unknown), and the same `m_agentMessageLimiter`.
Both sessions are logged like `MainWindowAgentHub.cpp` does: the sender gets
a "Sent a message to agent #N" notice, the receiver "==> Message from agent
#M". Messages that agents send each other through the hub show up too:
`applyTranscriptEvent` hands every event to `noteSessionsPageEvent`, which
spots the envelope's lead line and records it for the strip, the badge and
the filter (kept in memory, at most 200).

**No stalls.**

- The `applyTranscriptEvent` hook only notes the event: when the page is
  hidden it marks the transcript dirty and returns; when visible it queues
  one coalesced catch-up.
- Transcript rows are built from a queue in 8 ms slices on single-shot
  timers, at most the last 300 events at first (**Load earlier** adds 300),
  with bulk populate during a replay. Slices stop while the page is hidden
  and resume when it is shown.
- The list refreshes on a 400 ms coalescing timer only while visible, updates
  rows in place when the order is unchanged, and has no widgets per row.
  There is no polling: refreshes come from the Agents list's own
  notifications and the external-session scan.

## Workspace layout in the new interface

In the new interface (`app/design` = `next`) the Agents page follows the
app's layout mode ([ui-redesign.md](ui-redesign.md), "Two layout modes"):
**Focus** (default) is the layout below, **Control Tower** the earlier page
(`src/LayoutMode.h`, `src/WorkspaceLayout.h`, `workspaceLayoutNew()`). Code:
`src/MainWindowWorkspace.cpp`; styles: `resources/theme/next/61-workspace.qss`.
It is Qt Widgets, not QML (see [ui-redesign.md](ui-redesign.md)).

| Area | Contents |
| --- | --- |
| Session list header (`workspaceSidebarHeader`) | Repository, **New session**, search (mirrors the top bar), count, Sort, bulk actions |
| Session rows | Three-line cards: status badge, provider · status · age, title, location and line counts |
| Footer (`workspaceSidebarFooter`) | "N of M agents running", opens the limit menu |
| Conversation header (`workspaceHeader`) | Title, status chip, Stop/Continue, diff, Terminal and Browser toggles, action menu |
| Changed-files card (`workspaceChangesBar`) | Files, totals, branch, **Open diff** |
| Composer | Rounded box with one send button (`enterTarget`) and an options menu |
| Side panel (`workspacePanelStack`) | **Diff**, **Terminal**, **Browser**, **Details** |

Rules of construction:

- Nothing is re-parented or renamed; original buttons keep object names and
  order (rail shortcuts address them) and hidden rows move off-screen.
- New controls are proxies that read the original button's state and call
  `click()`, so no action is implemented twice.
- Cards are drawn by `forkmesh::ui::paintWorkspaceSessionRow` from the list
  delegate; the list stays in Full view underneath.
- The side panel is the third pane of `agentWorkspaceSplit`; the Code page
  moves into it. Below 700 px the diff is unified.
- Hooks: `NextAccess::installWorkspace`, `syncWorkspace`,
  `placeWorkspaceComposer`.
- `installWorkspace` finds the status row as the top row's layout that holds
  `m_agentStatusPill`, wherever it sits, and puts it back at that index for
  the classic layout. It used to read the first item; when the workflow link
  was put above it (2026-10-25) the lookup failed, the workspace returned
  early, and the new layout silently never installed.
- Keyboard shortcuts (Ctrl+Tab between sessions, Ctrl+Shift+D / Ctrl+\` /
  Ctrl+Shift+B for the panel tabs, Ctrl+\\ to close it, Ctrl+Shift+M / I / E
  for the composer pickers, Ctrl+/ for the sheet) come from
  `src/WorkspaceShortcuts.h` and are live only while this layout and the
  Agents page are shown. The session menu's **Open in** opens the worktree
  in an editor or the file manager. Both, and how they compare with Claude
  Code desktop, are in [claude-code-parity.md](claude-code-parity.md).

The **Details** tab (header toggle `workspaceToggleDetails`) holds the same
`SessionSidePanels` as the Sessions page: Progress, Outputs, Sources,
Background, Changes, Context, Environment and Tools, draggable in the same
order. `syncWorkspace` feeds it the selected session's `m_streamEvents` from
the last count it read (a cloud run's events are rebuilt when its history
changes) and fills the session facts through `NextAccess::fillSessionSidePanels`.
When a local agent starts and it is the selected session (or none is
selected), `NextAccess::workspaceSessionStarted` opens the panel on Details.
`NextAccess::feedSessionSidePanels` feeds a panel stack its session's facts and
the events it has not read yet (the read count lives in the panel,
`SessionSidePanels::feed`), so Focus and Control Tower share it.

In **Control Tower** the same cards are a right column of the detail pane
(`m_agentTowerPanels`, the third pane of `agentCodeSplit`, after the transcript
and the Code page; `buildAgentTowerPanels`). It shows while a session is
selected and the layout is not Focus, starts 220–320 px wide, can be dragged
narrower or shut at its splitter handle, and refreshes once a second while it
is on screen (`refreshAgentTowerPanels`), and at once on a sub-session event.
Focus hides it and uses the Details tab; switching modes (`syncWorkspace`)
re-checks it, and the Code page, put back on the way to Control Tower, goes
in before the column.
Switching into the Workspace view still opens on Terminal, the point of that
view.

The browser tab (`src/WorkspaceBrowserPane.*`, library
`forkmesh-browser-pane`) embeds an ungoogled Qt WebEngine view when the build
finds Qt WebEngine, and opens pages in the system browser otherwise; see
[browser.md](browser.md).

## Tool efficiency and Models board

The tool-efficiency page is built the first time it is opened
(`ensureAgentEfficiencyPage`); until then a placeholder holds its slot in the
page stack. Building and style-polishing it during deferred startup stalled
the window. The `test*` accessors for its widgets build it first.

**Runs view** (`src/AgentRunTable.*`, `src/AgentRunEfficiencyLog.*`). A run is
one prompt to its `result` event, recorded with provider, model, tool families
off at launch, outcome, time, tokens, tool calls and errors and estimated cost
(`AgentTokenPricing`). Efficiency is `1 − tokens at stake / tool tokens`. Runs
append to `agent-runs.jsonl` (last 2000) beside `tool-efficiency.json`; the
top table groups by runtime and tool configuration with a **vs all on**
column.

**Trends view** (`src/AgentPerformanceHistory.*`, `src/AgentPerformanceCharts.*`).
Twelve charts for the last hour, 24 hours, 7 days or 30 days, each with a
dashed line for the period before and the change against it, so a change to
the prompt, a tool family, the agent limit or the computer can be judged by
its result: agents running (average, peak in the tooltip), memory used, CPU
busy, tool calls, tool error rate, tool call time, context per call, tokens at
stake, tool efficiency, runs finished (with failures), run time and model
cost.

- **Storage.** `performance-history.json` beside `tool-efficiency.json` keeps
  two periods of buckets per range: 1 min for the hour, 15 min for 24 hours,
  1 h for 7 days and 6 h for 30 days. Older buckets are dropped on write.
- **Sources.** Tool calls come from the ledger's `callObserved`, tokens at
  stake from the improvement log's `occurred`, runs from
  `AgentRunEfficiencyLog::runFinished`. A one-minute timer samples running
  agents (`runningAgentCount`), memory (`SystemStats`) and host CPU.
- **Backfill.** A history created for the first time is seeded once from
  `agent-runs.jsonl`, so the 7 and 30 day charts start with past runs (their
  tool totals land at the run's end).
- **Markers.** The sampler compares total memory (whole GB), CPU cores, the
  agent limit and `FORKMESH_VERSION` with the last sample and draws a dotted
  line where one changed, so "more RAM, more agents" shows on the chart.
- **Copy summary** puts every metric for the shown period and the one before,
  plus the markers, on the clipboard as a markdown table.
- **Clear stats** leaves this history alone: it exists to compare across
  changes.

**Models board** (`src/AgentModelTable.*`, `src/MainWindowAgentModels.cpp`,
numbers from the widget-free `AgentModelStats::build`): one row per model in
`composerModelCatalog()` plus any seen elsewhere. Sources: `m_agentSessions`,
`model-outcomes.json` for deleted agents, reported or estimated cost (`~`),
`AgentToolEfficiencyLedger::modelUsage`, and a background `git log` per local
repository (`AgentModelStats::parseGitHistory` credits each merge to the most
frequent model trailer on its merged side; `ForkMesh-Agent:` wins over
`Co-Authored-By`). The git columns count history on disk and differ from
**Merged** on purpose.
The **Show** checkbox exists only for rows in the composer catalog; models
seen only in past runs cannot be offered there.

**Portraits.** Icons in `resources/agent-ui/icons/` are 256 px palette PNG
robot faces (WebP is avoided because Qt's decoder plugin is not always
shipped). New special agent portraits also need phone crops
(`teammate-face-<avatar>`, `teammate_faces/<avatar>.jpg`).

## Cloud runs in the Agents list

Rows come from `GET /api/account/agent-sessions`: every active session plus
the 40 newest finished ones, and `finishedBefore` when finished rows were cut.
`applyCloudAgentSessionSet` removes an unlisted row only if it is active or
newer than that cut-off.

- **Owner.** `fileCloudAgentSessionsUnderLocalRepos` files a run under the
  local repository of the same name, but `CloudAgentSession::relayOwner`
  keeps the relay's owner and every relay request uses `relayRepoOwner()`.
  Never build relay URLs from the display owner.
- **Task links.** `cloudAgentTaskId` reads `taskKey` or the prompt's
  `[task:<id>]` prefix.
- **Moving to this desktop.** With `agents/cloudRunsOnDesktop` on, runs for a
  repository with a working copy here become local agents and the relay copy
  is deleted. Moved ids are kept in `agents/cloudRunsMoved` so a run never
  starts twice. Runs marked `onRunner` are not moved
  (`movableCloudAgentSessionIds`); **Continue here** takes one over by hand.
- **Branches synced locally.** `syncCloudAgentBranches`
  (`forkmesh::upstream::syncServedAgentBranches`, on a worker) fetches
  unmerged `agent/` branches with `--no-tags --no-write-fetch-head` and writes
  only `refs/heads/agent/*` through `git update-ref` with the expected old
  value: create or fast-forward. A checked-out branch or one with local
  commits is left alone; nothing is deleted. The branch button opens the Git
  view, else the website's compare page.
- **Actions** (signed requests to the App, proof
  `forkmesh-owner-agent-branch-v1`):

| Action | Request |
| --- | --- |
| **Merge & clean up** | `{"action":"merge","cleanup":true,"requestId"}`, re-posted while `202 processing`; then the linked task is completed |
| **Clean up** | `{"action":"cleanup"}` |
| **Pull <base>** | `{"action":"update"}` |
| **Stop** | `{"action":"stop"}` |
| **Create PR** | Sync the branch, `createPullFromBranch`, then `{"action":"link-pull","number":N}` |
| **Continue**, **Fix conflicts** | A signed follow-up (`sendCloudAgentFollowUp`) |
| **Continue here**, **Retry in cloud**, **Sign in runner** | See [../agents/README.md](../agents/README.md) |

- **Merge & clean up** from the desktop calls
  `retireMergedCloudAgentSessions`: matching cloud rows leave the list, are
  remembered in `agents/cloudRunsRetired` (last 500) and get a silent relay
  delete.

### Live output of a cloud run

The selected cloud run streams its events into the transcript like a local
run; nothing is read for other runs. Relay and runner halves:
[../agents/README.md](../agents/README.md).

- **Who is followed.** `pumpCloudAgentStream()` follows only the selected
  session when it is a cloud row with status `running`, the page is visible
  and the window is not minimized (`CloudAgentStreamWatch::follow`). Anything
  else drops the cursor; the relay stops sending 30 s after the last read.
- **Reads.** `GET <session route>/stream?after=<seq>`, signed with the session
  read proof, on the `cloud-agent-stream` doorbell, again on `more`, otherwise
  every 3 s (10 s after a failure). One read at a time; stale answers are
  discarded.
- **Decoding.** `CloudAgentStreamDecoder` joins base64 chunks into JSON lines;
  `truncated` drops the partial line, `job`, `stream` or `reset` restarts.
- **Drawing.** Events go through `ClaudeTranscriptView::handleEvent` (max
  2000) and are shown only while status is `running`, so the final history
  turn replaces them without duplicates.
- While Watch is open with a run in flight, the account list is read every
  5 s (`kCloudAgentWatchRefreshMs`) instead of 2 min.

## Code > Branches and the Git page

The branch list is a sortable table: Agent / task links
(`refreshBranchLinks()`), Branch, Status, Behind / Ahead, Commit, Message,
Author, Changes, Updated, Worktree. Writers that change cells after load pause
sorting (`BranchSortPause`), because a sorted `QTableWidget` moves rows as
their sort cell changes.

- The diff header's **Conflict** button calls `fixAgentConflictsWithAgent()`
  when the branch belongs to an agent, otherwise **Fix with agent** starts
  one. **Pull <base>** is highlighted (`needsPull`) when the branch is behind.
- **Pull main** (`updateBranchFromBase`) and **Merge & clean up**
  (`requestMergeWorktreeIntoMain`) refuse to run while the other runs.
  Background reads use `GIT_OPTIONAL_LOCKS=0`. `Unable to write index` is not
  treated as lock contention, because `--autostash` may already have stashed
  edits.
- **Uncommitted work in the branch's worktree.** `mergeWorktreeIntoMain`
  records in `WorktreeMergeFlow` whether an agent session on the branch is
  still working (`agentSessionWorkInFlight` → `agentWorking`) or has stopped
  (`idleAgentId`). When `prepareWorktreeMerge` finds the worktree dirty:
  - **Merge & clean up** with a stopped agent: everything is committed on the
    branch as "Agent #N: commit the changes it left uncommitted", the files are
    logged, and the merge continues; the worktree is removed only after that
    commit. If the commit fails, nothing is merged and the notice gives git's
    reason.
  - The agent is still working: the merge is refused until it finishes;
    nothing is committed while an agent may still be writing.
  - No agent owns the branch, or a plain **Merge** (which keeps the worktree):
    refused as before, so a person's own edits are never committed for them.
    The notice lists up to five files (`describeDirtyPaths`) and says whether
    the edits would be lost with the worktree or left out of a plain merge.
  Tested in `checkErrorLogTriage` (`desktop/tests/test_window_resize.cpp`).
- After merging the shown branch, `applyWorktreeMergeStep` covers the Git
  workspace with a merged page (`m_gitMergedPage`) holding **Back to agents**,
  **Back to branches** and the merge stats. **Leaderboard**
  (`showGitLeaderboard`) shows the same page on demand; phones get it through
  `agentops.mergestats`.
- The merge page is built as sections joined by `diffSectionBreakHtml()` so
  each stays under `kDiffMaxUiApplyChars`. Rounded panels are 3×3 tables with
  corner images (`roundedPanelHtml`), since Qt rich text ignores
  `border-radius`. A pull request under 24 hours old shows a similar
  certificate (`pullCertificateHtml`).
- **Today's activity** (`UserActivityLog`, `UserActivityChart.cpp`) counts
  spontaneous presses, keys, touches and wheel notches delivered to a
  `QWindow` in 288 five-minute slots (`<state>/user-activity-today.json`, not
  in test mode), merged with other devices through
  `/api/accounts/activity-today` ([activity-today.md](../app/activity-today.md)).

## Watch (Market package `agent-watch`)

Watch is a bundled, removable Market package with its own rail page; the core
works without it. Package code:
[../../marketplace/packages/agent-watch/README.md](../../marketplace/packages/agent-watch/README.md).

The field draws each agent as a body ringed by activity icons, this desktop
at the centre, mirrors, hosts drawn by type, connections and the relay room.
It paints in software on the rail page; **Full screen** uses its own GPU
window, because a `QOpenGLWidget` in the main window would switch the whole
window to OpenGL composition. **Speak** reads replies aloud.

- **Core interface.** The neutral feed `src/AgentActivityFeed.*` (hosted by
  `src/MainWindowAgentFeed.cpp`) publishes agent events, sonifier notes, log
  lines, tasks and relay traffic, and answers `Host` calls. The core names no
  package; packages subscribe as `agentfeed::Listener`.
  `agentfeed::setWatching` turns on external transcript tailing and a faster
  cloud refresh. The package owns kind mutes (`agents/activityMutedKinds`);
  **Hide output** (`agents/activityLogsHideOutput`) hides forwarded node and
  mirror journal lines.
- **Machine-wide agents.** While Watch shows or local control has a client,
  `tickMachineAgents()` scans `/proc` every 2 s (`src/MachineAgentScan.*`)
  for agent CLIs. Ones not started by ForkMesh get external-band ids and are
  never stored; another login's agents are listed but not read. Phones see
  them as `outside` rows and can steer a live Claude Code session through its
  cross-session inbox (see [../native/DESKTOP_AGENTS.md](../native/DESKTOP_AGENTS.md)).
- **Connection traffic.** Clicking a host or line opens `AgentTrafficWindow`:
  per-connection rows keyed by session, pid, address and port, and a live
  feed from `Scene::traffic`.
- **Ids.** The desktop is -2 and seated mirrors use -100 to -899
  (`Scene::kMirrorFirst`). The view looks up mirrors before bodies, so the
  mirror band must never overlap an agent band. A body's title is
  `AgentSession::name` (it seeds colour and voice); the task goes in
  `Body::task`.

## Listen, announcer and interface sounds

- **Listen** (`src/AgentSonifier.*`): plays a run's stream events as music and
  rings a cadence at the end; voice parameters are hashed from session id and
  title. `agents/listenToNewRuns` listens to new runs automatically. Backends:
  Qt Multimedia (`QAudioSink`) when built with it, else raw PCM to `pw-cat
  --raw` or `aplay`. `FORKMESH_SONIFY_BACKEND=silent|pipe|none` forces one
  (tests use `silent`). Rendering runs on the player's thread.
  `Player::availablePipePrograms()` looks the pipe players up once per
  `PATH` value and keeps the answer, holding its lock across the lookup so
  a `start()` during a warm-up waits for it; `UiSounds` starts the warm-up
  (`Player::warmPipePrograms()`, on the global thread pool) when cues are
  audible. The lookup stats every `PATH` entry, and on WSL, whose `PATH`
  carries ~40 `/mnt/c` directories, it froze the first cue for ~1.5 s on
  the GUI thread.
- **Announcer** (`src/AgentAnnouncer.*`, `src/AgentAnnouncerVoice.*`):
  sportscaster commentary on running agents, spoken by Piper neural TTS,
  downloaded on demand with a pinned SHA-256 into `announcer/` under the data
  root. Commentary is pulled only when the speaker is idle, so stale lines are
  dropped rather than read late.
- **Interface sounds** (`src/UiSounds.*`, cue table `src/UiSoundCues.cpp`): an
  app-wide event filter installed while `ui/interfaceSounds` is on (default
  on, except on `offscreen`/`minimal` and in Qt test mode). Widgets with the
  `forkmeshQuietSounds` property on themselves or a parent are silent; the
  Chat section sets it. **Merge & clean up** plays `Cue::Merge`. The phones
  share the cue table (`docs/native/NAVIGATION.md`); change a shared cue here,
  in `InterfaceCues.swift` and in `SoundCues.kt` together.

## Voice Changer

A free Market package hosted in the desktop, not part of the core app; see
[../../marketplace/packages/voice-changer/README.md](../../marketplace/packages/voice-changer/README.md).

## Special agents (formerly Brains, Team and Teammates)

Rail **Special Agents**. A special agent is a workflow owned by the
repository in `.forkmesh/workflows/<id>.json` whose Special agent step holds
the role (`src/TeammateStore.*` reads it through `src/RepoWorkflow.*`;
`src/MainWindowTeammates.cpp`). Older `.forkmesh/teammates/*.json` files still
load and move on their next save. It runs on demand, on a cron cadence, on
events or continually with a decision gate (see
[www/docs/qt-client.md](../../www/docs/qt-client.md)); error-log and crash
repair are special agent tasks ([diagnostics.md](diagnostics.md)). Every run
is a workflow run with its own record and output file
([../app/workflows.md](../app/workflows.md#desktop-workflows-and-special-agents)).

Only user-visible text says "special agent". Stored and wire names keep
"teammate": legacy `.forkmesh/teammates/`, `Teammate*` classes, `teammates/*`
settings, the `teammates.` local-control family, the `#team` room and `team`
sender id, `teammate-*.png` portraits. Pausing all automatic runs sets
`schedule.active` to `false` and keeps the cadence.

### Agent choice and permissions

A definition has no provider, model or mode. Every start uses
`teammates::recentAgentChoice()`: the composer's agent and model, else the
newest session no special agent started, else `defaultAgentProvider()`.
`"lowCostModel": true` takes the chosen agent's cheapest model
(`lowCostAgentChoice`; used by the spider's agent, see [genie.md](genie.md)).
`"marketingChannels"` attaches an agent to Marketing channel pages
(`NativePackageHost::specialAgents`).

The run mode is the composer's, clamped by `agentrules::teammateRunMode`:
without `modify-code` it is Plan, and Auto without `run-unattended` becomes
Edit. A UI-stall repair ignores the composer and starts in Auto when the agent
holds `run-unattended`. Missing permissions are refused at the external-call gate and, for
Claude Code, by `--disallowedTools` (`deniedTeammatePermissions`; see
[../agents/access-rules.md](../agents/access-rules.md)):

| Permission missing | Refused |
| --- | --- |
| `external-tools` | External calls except approved destinations; WebFetch, WebSearch |
| `create-branches` | `git push`, new branches, `git worktree add`, `gh pr create`, MCP `open_pr_from_branch` |
| `manage-issues` | `gh issue` writes, MCP issue, milestone and project writes |
| `review-prs` | `gh pr review/comment/edit/close/ready` |
| `merge` | `gh pr merge` |

- Grants are recorded per session (`m_teammateRunGrants`).
- Special agents never open pull requests; prompts say to leave finished work
  for a person.
- `teammates/ownWorktrees` (off): while off, Claude Code and Codex runs work
  in the checkout and `create-branches` is dropped from the grant.

### Sessions

- A run is a workflow run: `runTeammate` starts the agent's workflow
  (`runTeammateWorkflow`) and its Special agent step starts a fresh session
  through `startTeammateSession`, filed in the **Workflows** agent folder.
  A mention or a direct prompt to an agent with a conversation continues that
  session instead, as chat, without a workflow run.
  `ownTeammateSession` still names the newest session the agent started, so
  steering, Stop and the face's status work as before, and
  `teammateWithRuntime` keeps runtime fields across saves.
- Mentions go through `deliverTeammateMessage` (steer while working, else
  continue). **Run team**, **Generate briefing** and `@all` use one session
  per member.
- **Run all in one session** (`runTeammatesInOneSessionNow`,
  `buildTeammateSharedPrompt`) holds only the intersection of the
  participants' permissions and splits the closing summary back by
  `## @Handle`.
- **Create needed teammate** runs a read-only analysis ending in one JSON
  definition, which the desktop saves (`teammateFromAgentOutput`) without
  `merge`, `process-payouts` or `run-unattended`, with schedules paused.
- A self-improvement cycle counts only when the summary adds a new
  `Improvement note:` lesson (`completeImprovementCycle`).

### Page and rail

- **Page.** Grid view shows video tiles (`buildTeammateVideoTile`) with the
  live face and the last run's caption (each run ends with `Caption:`,
  read by `runCaptionFromSummary`). Each agent has a home page
  (`src/MainWindowTeammateHome.cpp`) with a conversation, notes, widgets and
  about panel; notes and widgets stay on the machine
  (`<data>/teammate-workspaces/<repository digest>/<id>.json`), are added to
  every run's prompt and never widen permissions. Widget changes the agent
  proposes are applied only with the operator's approval.
- **Rail.** Until 2026-10-24 `src/MainWindowTeammateRail.cpp` built
  `#teammateRail` (`ui/showTeammateRail`); the right edge now holds the generic
  side rail, where a special agent appears when its workflow is dragged there
  or pinned right ([side-rail-and-terminals.md](side-rail-and-terminals.md)).
  The faces, moods and HUD buttons below still drive the Special Agents
  page's tiles and cards. Each 54 px item had an animated
  face (`facePoseAt`, one shared clock), a status border, a staleness tint
  (`teammates::runStaleness`), a play button, a mood emoji and a menu. The
  mood (`src/TeammateMood.*`) is scored from 100 weighted signals and sent to
  phones in `teammates.list`. `runWindowHours` sets a run reminder;
  `teammates/doneUnseenSessions` marks finished runs nobody has seen. The rail
  rebuilds only when its signature changes. Every face is driven by one
  `RailFaceTicker` (40 ms), so a tick is one repaint pass for the window; a
  face only redraws while its window is exposed and not minimized, and a calm
  face (still, floating, dozing or paused, without a spinner ring) redraws
  every 100 ms. While the window is not the active one every face redraws at
  most every 200 ms (`kRailFaceInactiveFrameMs`), and a face with no visible
  region (rail scrolled or covered) skips its frame: each face frame also
  repaints its item, name label and three HUD buttons, about 300 us a frame,
  and report #5 counted 267 such frames a second across the rail. The mood badge paints from a cached pixmap. In report #4
  (2026-10-19) one timer per face repainted each rail item about 540 times a
  second and the app sat at 106% CPU.
- **Channel status rail.** `src/MainWindowPackageRail.cpp` builds
  `#packageRail` (`ui/showPackageRail`) from package tiles
  (`NativePackageWidget::railTiles`, `PackageRailTileButton`); Marketing
  provides one square per channel with next steps. Pages are prepared after
  startup sign-in resolves and every 15 minutes. The channel reminder
  (`ui/packageRailReminder`) files one `channel_reminder` ping for the worst
  square after an hour without change.
- **Pins.** The special agent menu's **Open in Workflows** and **Pin to rail** put the
  agent's workflow on the Workflows page or on the top, left, right or bottom
  rail ([../app/workflows.md](../app/workflows.md#desktop-workflows-and-special-agents)).
- **Phones** show the same faces, moods and status in an adaptive tile grid
  ([../native/DESKTOP_AGENTS.md](../native/DESKTOP_AGENTS.md)).

## Testing

Build the test targets with `cmake -S desktop -B desktop/build
-DFORKMESH_BUILD_TESTS=ON` (`./run.sh` configures them off).

| Area | Tests |
| --- | --- |
| Window behaviour | `forkmesh-window-tests` with `--workspace-snapshots-only`, `--agent-list-view-only`, `--agent-folders-only`, `--agent-task-link-only`, `--command-approval-only`, `--agent-code-only`, `--sessions-only` (Sessions page: rail placement, list, live transcript, composer, relays); `tests/test_window_resize.cpp` |
| Transcript tools | `forkmesh-claude-transcript-tools-tests` |
| Claude Code session control | `forkmesh-claude-control-tests`, `forkmesh-agent-cli-tasks-tests`, `runHostControlProtocolTest` in `forkmesh-agent-transport-tests` (a scripted `claude` on stdio) |
| List | `forkmesh-agent-folders-tests`, `forkmesh-agent-ticker-tests` |
| Cloud | `forkmesh-agent-cloud-stream-tests`, `tests/test_agent_cloud_sessions.cpp`, `tests/test_upstream_checkout_sync.cpp`, `tests/validate_cloud_agent_stream_contract.py`, `tests/validate_cloud_agent_watch_contract.py` |
| Accounts | `tests/test_claude_account_routing.cpp`, `tests/test_runner_login.cpp` |
| Models and activity | `forkmesh-agent-model-stats-tests`, `forkmesh-user-activity-tests` |
| Special agents | `forkmesh-teammate-workspace-tests`, `forkmesh-teammate-mood-tests`, `forkmesh-native-package-tests`, `app/tests/test_desktop_package_rail_startup.py` |
| Sound and Watch | `forkmesh-sonifier-tests`, `forkmesh-ui-sounds-tests`, `forkmesh-announcer-tests`, `forkmesh-machine-agent-scan-tests`, `forkmesh-agent-watch-tests` |

Window tests run offscreen; their `settle` step must also deliver deferred
deletes. `FORKMESH_AGENT_ROWS_SHOT` and `FORKMESH_AGENT_FOLDERS_SHOT` save row
images.
