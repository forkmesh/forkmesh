# Desktop: Agents page, activity window and sounds

Part of [the desktop docs](README.md). How runs work underneath (runtimes,
sandboxing, website jobs, mirrors, errors): [../agents/README.md](../agents/README.md).

## Agents page

Rail **Agents**. Main sources:

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

### List views, sort and filters

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
  Code**. A row already in a folder, or dragged out, is left alone.

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

**Raw log jump.** Clicking a row's gutter switches to Raw at the line of the
event that drew it (`m_rowSource`, `transcripttools::rawLogLineStart()`).

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

### Composer

The prompt box (`src/PromptDraftStore.*`, `src/PromptSpellCheckHighlighter.*`,
`src/AgentPromptImages.*`) keeps drafts, spell-checks and takes images.

| Control | Setting | Effect |
| --- | --- | --- |
| Worktree checkbox (`promptWorktreeCheck`) | | Own worktree, or the checkout directly |
| **Cloud** (`promptCloudButton`) | `agents/composerRunInCloud` | **new** queues the run on an account agent node (`startQuickAddCloudAgent`, signed `POST /api/repo/<owner>/<repo>/agent-sessions`); Claude Code and Codex, published repositories, no attachments |
| **Vibe** (`promptVibeCheck`) | `agents/composerVibe` (off) | Stamps `AgentSession::vibe`; a successful worktree run is merged with clean-up (`maybeAutoMergeForSession`) and pushed (`pushCurrentRepoUpstream`). All merge gates apply |

- The repository comes from `resolveAgentRepositoryIndex` for local and cloud
  runs. Phones expose the same switches through `agents.start`.
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

## Workspace layout in the new interface

In the new interface (`app/design` = `next`) the Agents page has a **New**
(default) and a **Classic** layout (`agents/workspaceLayout`,
`src/WorkspaceLayout.h`, `workspaceLayoutNew()`). Code:
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
| Side panel (`workspacePanelStack`) | **Diff**, **Terminal**, **Browser** |

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

The browser tab (`src/WorkspaceBrowserPane.*`, library
`forkmesh-browser-pane`) opens pages in the system browser unless built with
`-DFORKMESH_BROWSER_PANE=ON` and Qt WebEngine. Only `http` and `https` load.

## Tool efficiency and Models board

**Runs view** (`src/AgentRunTable.*`, `src/AgentRunEfficiencyLog.*`). A run is
one prompt to its `result` event, recorded with provider, model, tool families
off at launch, outcome, time, tokens, tool calls and errors and estimated cost
(`AgentTokenPricing`). Efficiency is `1 − tokens at stake / tool tokens`. Runs
append to `agent-runs.jsonl` (last 2000) beside `tool-efficiency.json`; the
top table groups by runtime and tool configuration with a **vs all on**
column.

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

Rail **Special Agents**. A special agent is a named role definition owned by
the repository in `.forkmesh/teammates/*.json` (`src/TeammateStore.*`,
`src/MainWindowTeammates.cpp`). It runs on demand, on a cron cadence, on
events or continually with a decision gate (see
[www/docs/qt-client.md](../../www/docs/qt-client.md)); error-log and crash
repair are special agent tasks ([diagnostics.md](diagnostics.md)). Its
session is an ordinary agent session.

Only user-visible text says "special agent". Stored and wire names keep
"teammate": `.forkmesh/teammates/`, `Teammate*` classes, `teammates/*`
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
Edit. Missing permissions are refused at the external-call gate and, for
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

- Each special agent works in its own session: `ownTeammateSession` returns
  `lastSessionId` only for that agent's own run, and `teammateWithRuntime`
  keeps runtime fields across saves.
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
- **Rail.** `src/MainWindowTeammateRail.cpp` builds `#teammateRail`
  (`ui/showTeammateRail`), sorted by last run. Each 54 px item has an animated
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
- **Phones** show the same faces, moods and status in an adaptive tile grid
  ([../native/DESKTOP_AGENTS.md](../native/DESKTOP_AGENTS.md)).

## Testing

Build the test targets with `cmake -S desktop -B desktop/build
-DFORKMESH_BUILD_TESTS=ON` (`./run.sh` configures them off).

| Area | Tests |
| --- | --- |
| Window behaviour | `forkmesh-window-tests` with `--workspace-snapshots-only`, `--agent-list-view-only`, `--agent-folders-only`, `--agent-task-link-only`, `--command-approval-only`, `--agent-code-only`; `tests/test_window_resize.cpp` |
| Transcript tools | `forkmesh-claude-transcript-tools-tests` |
| List | `forkmesh-agent-folders-tests`, `forkmesh-agent-ticker-tests` |
| Cloud | `forkmesh-agent-cloud-stream-tests`, `tests/test_agent_cloud_sessions.cpp`, `tests/test_upstream_checkout_sync.cpp`, `tests/validate_cloud_agent_stream_contract.py`, `tests/validate_cloud_agent_watch_contract.py` |
| Accounts | `tests/test_claude_account_routing.cpp`, `tests/test_runner_login.cpp` |
| Models and activity | `forkmesh-agent-model-stats-tests`, `forkmesh-user-activity-tests` |
| Special agents | `forkmesh-teammate-workspace-tests`, `forkmesh-teammate-mood-tests`, `forkmesh-native-package-tests`, `app/tests/test_desktop_package_rail_startup.py` |
| Sound and Watch | `forkmesh-sonifier-tests`, `forkmesh-ui-sounds-tests`, `forkmesh-announcer-tests`, `forkmesh-machine-agent-scan-tests`, `forkmesh-agent-watch-tests` |

Window tests run offscreen; their `settle` step must also deliver deferred
deletes. `FORKMESH_AGENT_ROWS_SHOT` and `FORKMESH_AGENT_FOLDERS_SHOT` save row
images.
