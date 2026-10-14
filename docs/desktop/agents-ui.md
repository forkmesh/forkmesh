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
| Usage and accounts | `src/MainWindowAgentUsage.cpp`, `src/MainWindowChat.cpp`, `src/ClaudeAccountRouting.cpp` |
| Cloud rows | `src/AgentCloudSessions.*`, `src/AgentCloudStream.*`, `src/MainWindowAgentCloud.cpp` |
| External sessions | `src/ClaudeSessionScan.*`, `src/MachineAgentScan.*`, `src/ClaudePeerMessaging.*` |
| Task links | `src/MainWindowAgentTaskLink.cpp` |
| Intake socket | `src/AgentIntake.*`, `src/MainWindowAgentIntake.cpp` |

The toolbar has **Tool efficiency** (`src/AgentToolEfficiencyBoard.*`),
**Models** (page 3 of `agentPageStack`), **Usage**, **Listen** and
**Announcer**. Watch is a Market package page in the rail (see
[Watch](#watch-market-package-agent-watch)).

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
normal concurrency limit. Phones can change the limit and queue order
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
- Dragging the strip moves the composer; dragging it out tears it off into a
  frameless window (`tearOffPromptOverlay`). Double-click docks it.
- Notices are inline in the page, never floating (see
  [diagnostics.md](diagnostics.md)).
- Sending plays a `PromptFlightOverlay` (sparkle to the Agents rail button,
  or a floating bubble) instead of a card; the overlay is click-through and
  deletes itself.
- `voice/inputEnabled` (on) shows the dictation mic.

### Provider accounts and usage

- The bottom bar shows the usage meter of the composer's provider. Its menu
  (`showAgentAccountMenu`) has per-account **Edit**, **Log in**, **Log out**
  and a terminal button that opens the CLI as that account
  (`CLAUDE_CONFIG_DIR` / `CODEX_HOME`).
- Usage rows (`TokenUsageMenuRow`) segment the bar per hour or day of the
  window, mark even pace and add a pace note (`usagePaceNote`).
- **Usage** (`agentUsageButton`, `showAgentUsageMenu`) shows the default
  provider's bars and offers terminals, **Run new agents as**, account
  management and the default provider switch. **Runner accounts**
  (`fillRunnerAccountsMenu`) switches a runner's account.
- **Claude account routing.** Each account is tied to one Claude login
  (`accountUuid`/`organizationUuid`, saved under
  `agents/accountIdentity/claude/<account id>`). Logins run in a fresh folder
  under `<data>/agent-accounts/claude-login/` and are filed by identity;
  `reconcileClaudeAccountLogins` moves logins made outside ForkMesh. Files
  move by copy then remove, never by deleting folders.

### Usage-limit auto-continue

A run that fails on a usage limit is held under
`agents/usageLimitAgentHold/<id>` with the reset time parsed from the refusal
(`usageLimitResumeAtMs`), else the provider's window reset. A timer, the
reset ping or startup (`restoreUsageLimitAgentHolds`) continues it through
`continueAgentSession` if it is still failed on that limit. Only the refusal
is parsed, never the log tail. Setting: `agents/usageLimitAutoContinue` (on).

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
  rebuilds only when its signature changes.
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
