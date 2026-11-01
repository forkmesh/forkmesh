# Desktop: the Agents page

Part of [Desktop](README.md). The Agents page lists every agent session of the
open repository (local, external Claude Code and cloud runs), shows the
selected one's live transcript and controls, and holds the composer that
starts and steers runs. This page covers the list, the detail pane, how the
desktop hosts a Claude Code session, the transcript, the composer, the
new-interface workspace, cloud rows, merging agent branches and the Models
board. How runs work underneath (runtimes, naming, sandboxing, website jobs,
cloud handoff, mirrors, errors) is in [agent runs](../agents/README.md).

Related pages:

| Page | Covers |
| --- | --- |
| [sessions-page.md](sessions-page.md) | The Sessions page and the side panels it shares with this page |
| [agent-usage.md](agent-usage.md) | Agent accounts, the Usage page, usage meters and menus |
| [special-agents.md](special-agents.md) | Special agents and the Special Agents page |
| [activity-and-sounds.md](activity-and-sounds.md) | Watch, Listen, the announcer and interface sounds |
| [claude-code-parity.md](claude-code-parity.md) | What Claude Code desktop has that this workspace has or lacks |
| [side-rail-and-terminals.md](side-rail-and-terminals.md) | Terminal tabs and the side browser agents can drive |

## Code map

| Part | Files |
| --- | --- |
| Page | `src/MainWindowAgents.cpp` |
| List | `src/MainWindowAgentList.cpp`, `src/AgentFolders.*`, `src/ListViewMode.*`, `src/AgentTicker.*` |
| Detail | `src/MainWindowAgentDetail.cpp` |
| Transcript | `src/MainWindowAgentTranscript.cpp`, `src/ClaudeTranscriptView.*`, `src/ClaudeTranscriptTools.cpp`, `src/TranscriptCoverage.*`, `src/AgentTaskList.cpp` |
| Claude Code host control | `src/ClaudeControlProtocol.*`, `src/MainWindowAgentClaudeControl.cpp`, `src/ClaudeElicitationDialog.*`, `src/AgentCliTasks.h` |
| Launch | `src/MainWindowAgentLaunch.cpp`, `src/MainWindowAgentCliSession.cpp` |
| Composer | `src/PromptDraftStore.*`, `src/PromptSpellCheckHighlighter.*`, `src/AgentPromptImages.*`, `src/PromptDrop.h`, `src/WorktreeInclude.h` |
| Workspace (new interface) | `src/MainWindowWorkspace.cpp`, `src/WorkspaceLayout.h`, `src/WorkspaceShortcuts.h`, `src/WorkspaceOpenIn.h`, `src/WorkspaceBrowserPane.*` |
| Import and external sessions | `src/MainWindowClaudeImport.cpp`, `src/ClaudeSessionScan.*`, `src/MachineAgentScan.*`, `src/ClaudePeerMessaging.*` |
| Cloud rows | `src/AgentCloudSessions.*`, `src/AgentCloudStream.*`, `src/MainWindowAgentCloud.cpp` |
| Usage and accounts | [agent-usage.md](agent-usage.md) |
| Task links | `src/MainWindowAgentTaskLink.cpp` |
| Intake socket | `src/AgentIntake.*`, `src/MainWindowAgentIntake.cpp` |
| Tool efficiency and Models | `src/AgentToolEfficiencyBoard.*`, `src/AgentRunTable.*`, `src/AgentRunEfficiencyLog.*`, `src/AgentPerformanceHistory.*`, `src/AgentPerformanceCharts.*`, `src/AgentModelTable.*`, `src/MainWindowAgentModels.cpp` |

## Page and toolbar

Rail **Agents**; the rail button directly above it, **Sessions**, opens the
[Sessions page](sessions-page.md). The page is tab 3 of an open repository,
and its list shows only that repository's agents.

- `openAgentsOverview` opens the selected or newest agent whose repository is
  open or in the repository list; with no repository open it opens the first
  repository on its Agents tab.
- Rail **Agents** with no repository at all unchecks the button, shows Repos
  and a notice that agents work inside a repository; the tour and the
  composer's Models link keep their own handling.
- The toolbar has **Tool efficiency**, **Models** (page 3 of
  `agentPageStack`), **Usage** ([agent-usage.md](agent-usage.md)), **Listen**
  and **Announcer** ([activity-and-sounds.md](activity-and-sounds.md)). Watch
  is a Market package page in the rail.

## Session list

### Session sources and ids

The list is one list of every session. An icon in the title cell says where
it runs (`device-desktop` for this desktop, `cloud` for a mirror or runner);
searching "desktop", "cloud" or a mirror name narrows it.

| Source | Id band | Stored |
| --- | --- | --- |
| Local sessions | positive | `data/agents/sessions` |
| External Claude Code sessions and machine-wide agents | -1000 and down (`externalTempIdFor`) | never |
| Cloud runs from the account's mirrors and runners | -1000000 and down (`kCloudIdBase`) | never |

- Only positive ids are stored: `AgentStore::saveSession` refuses an id of 0
  or below and `loadAllSessions` skips one.
- Cloud runs that target this desktop are dropped; they exist locally.
- The cloud band lies inside the external band, so code must test
  `isCloudSession()` before `isExternalSession()`.
- Branch-wide deletes (`deleteWorktreeBranchAndAgentInBackground`) skip both
  bands.
- Durable per-row state (folders, unread) is keyed by `agentReadKey()`: the
  local id, `cloud:<relay id>` or `claude:<uuid>`.

### Toolbar, views, sort and filters

- The list's toolbar (`agentsActionBar`) is an `OverflowActionRow`
  (`agents/listActionUses`): up to six of Start all, Stop all, Update all,
  Delete selected, Usage, To desktop, Sort and the rest inline, the queue
  count always, everything else under **More**, so a narrow list pane never
  squeezes the buttons. **Import** is pinned at the left
  ([Import](#importing-claude-code-sessions)). The recycle-bin strip shows "N
  deleted" (the full text is its tooltip) and folds Show, Restore all and
  Delete all the same way.
- **View** (`setAgentListView`, `agents/listView`; shared with the Tasks page
  through `forkmesh::ui::ListViewMode`, see [tasks-page.md](tasks-page.md)):
  - **Compact**: lead column (avatar, status, age, branch button, churn) and a
    one-line title. A running session also scrolls its latest prose after
    the title (`agentTickerLine` reduces it to one line of at most 120
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
- Each row shows the run's task progress after the title
  ([live task list](#live-task-list)).

### Folders

`AgentFolders` stores per-repository folders in `agents/folders` (one JSON
value keyed by `owner/name`: ordered folders with id, name and collapsed
state, plus a member map keyed by `agentReadKey()`). Folders are local;
phones receive them in `agents.list`, and a change rings `agents.changed`.

- Header rows span both columns (`paintAgentFolderRow`, `kAgentFolderRole`),
  are not selectable and hold `0` in `Qt::UserRole`; code that scans the
  table for a session id must skip them.
- Drag works in every sort (`AgentPriorityTable`): drop on a header to file,
  on a row to join its folder, below the last row to unfile; dragging a
  header (`application/x-forkmesh-agent-folder`) reorders folders. Drops
  rewrite folder state and the list is rebuilt from `AgentFolders::layout()`.
- Automatic filing (`AgentFolders::findOrCreate`): special agent sessions go
  in **Special Agents** (workflow runs in **Workflows**), live external Claude
  Code rows in **External Claude Code**, imported Claude Code sessions in
  **Imported Claude Code**. A row already in a folder, or dragged out, is left
  alone.
- A header's right-click menu has **Start all in folder** and **Stop all in
  folder** (`startAgentFolderSessions`, `stopAgentFolderSessions`). They act
  on the folder's share of the toolbar's Start all / Stop all population
  (`startableAgentSessionIds`, `stoppableAgentSessionIds`), so merged and
  externally started rows are never touched, and each is disabled when the
  folder has nothing to start or stop. Stopping a folder takes only its own
  sessions off the queue.
- Filing external Claude Code rows saves once per scan, on the next
  event-loop turn, and `AgentFolders::save()` skips the write when the stored
  JSON is unchanged, so a scan does not rewrite and fsync `ForkMesh.conf` per
  session.

### Unread dot

A row is unread when a turn finished after the user last opened it.
`agents/seenFinishedAt` maps read keys to the last seen `finishedAtMs`;
`agents/readBaselineMs` treats older finishes as read. `showAgentSession`
marks read only when the list is on screen in the active window. Phones get
`unread` per row and can call `agents.markRead`.

### Paused and connector-created agents

The MCP `create_agent` tool ([mcp-create-agent.md](../tools/mcp-create-agent.md))
adds rows through the private intake socket.

- A paused agent is a `Queued` session with `held` (and `heldBy`) in its
  `meta.json`. Held sessions are never put on `m_agentQueue` and are skipped
  by `processAgentQueue` and `AgentStore::queuedSessionIdsOldestFirst`.
- **Start now**, **Start** or any prompt sent to it (`continueAgentSession`
  clears `held`) starts it under the normal concurrency limit.
- A connector-created agent can carry an organization task (`task`), or take
  over its creator's task (`handoff`), which unlinks the creator from it.
- Phones can change the limit and queue order (`agents.queue.limit`,
  `agents.queue.move`; see [DESKTOP_AGENTS.md](../native/DESKTOP_AGENTS.md)).

A running agent can also link a branch it created to its own session with the
MCP `link_branch` tool (`agents.link_branch`,
[../tools/mcp-create-agent.md](../tools/mcp-create-agent.md#linking-a-branch-to-your-own-session-link_branch)).
This is for a session started directly in the checkout (`directCheckout`,
which has `branchName` empty) that later moved its work to a branch: after
the link the **Branch** button, **Code**, the merge actions and the task link
use that branch. `directCheckout` stays set, so the run and its follow-ups
keep working in the checkout; `cachedSessionWorktree` and
`cachedSessionWorktreeForUi` skip the live working directory
(`m_streamWorktree`, the checkout) for such a session
(`sessionLinkedFromCheckout`) and resolve the worktree that holds the branch
instead, or none when the branch is checked out in the checkout itself.
`m_streamWorktree` is left alone, so finishing the run never treats the linked
worktree as the run's own folder to clean up.

## Imported and external Claude Code sessions

### Importing Claude Code sessions

**Import** (`showClaudeSessionImportDialog`) lists every Claude Code and
Claude Desktop session that ran in the open repository, of any age, and turns
the ones picked into ordinary stored agents. The run side (resuming with
`--fork-session`) is summarised in [agent runs](../agents/README.md#local-runs-desktop).

- **Where sessions come from.** `ClaudeSessionScan::listAll` reads
  `<config>/projects/<key>/*.jsonl` under `~/.claude` and the desktop's own
  `CLAUDE_CONFIG_DIR`, where `<key>` is the cwd with every non-alphanumeric
  character turned into `-` (`projectKey`). Only folders whose key starts
  with the repository's are opened unless **Every folder** is ticked. A
  session copied into two project folders is listed once, from the folder
  matching its cwd. Sessions with no prompt and no title are left out.
  ForkMesh's own account folders (`data/agents/accounts`) are not read: those
  are its agents.
- **Names.** In order: Claude Desktop's title
  (`<Claude config>/claude-code-sessions/<account>/<org>/local_*.json`, keyed
  by `cliSessionId`; `~/.config/Claude` on Linux), the last `custom-title`
  (`/rename`), the last `ai-title`, then the first typed prompt cut to one
  line of 80 characters. External rows use the same names.
- **Rows not offered.** Headless sessions (an `sdk-*` entrypoint: `claude -p`,
  the SDK, ForkMesh's own agents and special agents) and sessions in the agent
  worktree root (hundreds of ForkMesh runs on a busy machine) are left out
  unless **Headless runs** is ticked. One whose worktree belongs to an agent
  still in the list is never shown. A session already imported says so;
  importing it again refreshes its name.
- **The agent.** `createImportedClaudeAgent` stores a Claude Code session with
  status Success, the session's name as title, its first prompt, model, first
  and last timestamps, and `claudeImport` (`id`, `path`, `cwd`, `bytes`) in
  `meta.json`. A cwd inside the agent worktree root on a branch adopts that
  worktree (`worktreeName`, `branchName`); anything else is a no-worktree
  session that runs in the original cwd when it is inside the repository,
  else in the checkout.
- **Transcript.** `ClaudeSessionImport::convertLines` turns typed prompts into
  `_local_user`, keeps assistant turns and tool results with the session id
  stamped on them, drops sidechains, meta lines and command echoes, and clips
  a tool result to 16 KB. The readable log gets both sides, so a later switch
  to Codex hands over the conversation.
- **Mirroring.** While `claudeImport.bytes` is 0 or more, every 3 s tick
  (`syncImportedClaudeSessions`) appends whatever the `.jsonl` gained since
  that offset and refreshes the name, so turns taken in Claude Desktop keep
  arriving. An imported session is never also shown as an external row.
- **Continuing it.** The first follow-up resumes the Claude session id found
  in the transcript. `startCliTranscript` sets `bytes` to -1 (mirroring
  stops), `launchCliRuntime` copies the `.jsonl` to where the run's account
  looks for it (`<account config>/projects/<key of the workdir>/<id>.jsonl`,
  `ClaudeSessionImport::stage`; nothing is copied when that is the same file)
  and adds `--fork-session`, so the run gets its own session id and never
  writes into Claude's copy. If the resume still fails, the fallback prompt
  carries the last 48 KB of the imported log.

### External Claude Code rows

- **ForkMesh's own agents are never external rows.** Each agent's account
  project folder is symlinked into `~/.claude/projects`
  ([claude-desktop-sessions.md](../agents/claude-desktop-sessions.md)), the
  folder the external scan reads, and a merged agent's transcript can still
  look live. So `ClaudeSessionScan::scan` skips project folders that resolve
  into a Claude agent account's config dir, and the worktrees of streams
  ForkMesh dropped (`retireStreamCwd`, from `purgeSessionState`,
  `releaseTrackedWorktree` and `cleanupStreamWorktree`) stay excluded for the
  rest of the run, which covers agents that use `~/.claude` directly. A run
  without a worktree, which works in the repository checkout itself, is not
  remembered, so your own sessions there still list.
- **Typing into an external row.** `sendPromptToExternalClaudeSession` sends
  a live session the message through its cross-session inbox, the same path
  phones use, with `ClaudePeerMessaging::desktopSteerLead()` and sender name
  `ForkMesh (desktop)`; `steerText` strips either lead for display. A session
  that refuses or would hold the message without a terminal, or has no
  socket, offers to continue it as a ForkMesh agent instead. A row that is no
  longer live is imported and the message continues it. Attachments are
  refused for these rows.

## Detail pane

- **Header actions** live in one `OverflowActionRow` (`agentHeaderActions`,
  `src/OverflowActionRow.*`): the status pill on the left, then at most five
  actions, the branch sync label, and a **More** (⋯) button whose menu holds
  the rest (Listen, Raw, Worktree, Delete, issue and task links, cloud
  actions...). Which five show is decided by use: every click, from the row
  or the menu, is counted in `agents/headerActionUses/<name>`, and the most
  used visible actions win, ties going to a fixed rank (Continue, Stop,
  Continue here, Merge & clean up, Create PR, PR, Pull main, Code...). When
  the pane is too narrow even for those, the lowest-ranked ones move into the
  menu too, so the row never overlaps or runs off the right edge. Hidden
  actions stay real buttons, parked off-screen, so every visibility rule and
  click handler is unchanged.
- The title (`WrappedTitleLabel`) wraps up to `kAgentTitleMaxLines` lines.
  `showAgentSession()` reopens a detail pane collapsed to zero width.
- **Code** opens the live diff right of the transcript (`agentCodeSplit`),
  polling the worktree every 3 s while a run is live and visible.
- **Branch sync** (`refreshAgentSyncControls`, from `m_agentDiffStats`): a
  label with ahead/behind against the session's own base, and a button that
  is **Pull <base>** (`updateBranchFromBase()`) or **Fix conflicts**
  (`fixAgentConflictsWithAgent()`), disabled while a merge flow runs.
- **Add task**, **Create task from this agent** and **Attach to an existing
  task...** link an agent to an organization task ([tasks-page.md](tasks-page.md)).
- Cloud rows have their own actions ([cloud runs](#cloud-runs-in-the-agents-list)).

## Claude Code session control

A Claude Code run on the desktop is a full stream-json host of its Claude
Code session, the way an IDE or Claude's desktop app hosts one.
`launchCliRuntime` sets the launch option `hostControl`, and
`ClaudeStreamSession` then:

- adds `--permission-prompt-tool stdio` and writes an `initialize` control
  request before the task. It declares the dialog kinds the desktop renders
  (`refusal_fallback_prompt`, `fable_overage_consent_prompt`,
  `auto_mode_server_fallback`), asks for sub-session progress summaries
  (`agentProgressSummaries`) and declares a per-task stop control
  (`perTaskStopAffordance`). An issue run, or a run you named yourself,
  passes its title as `title`, so Claude Code keeps that name;
- keeps every control request it has not answered under a card id
  (`claude-approval-<request>` for tool approvals, `claude-input-<request>`
  for dialogs and MCP input, the tool use id for `AskUserQuestion`) and
  answers it when the card is answered;
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
in `ClaudeControlProtocol.{h,cpp}`; `MainWindowAgentClaudeControl.cpp` puts
it on screen. Shapes were checked against Claude Code 2.1.309.

### Names and Rename

How a run gets its name (the pre-launch namer, `title`, `rename_session`,
`generate_session_title`) is in
[agent runs](../agents/README.md#local-runs-desktop). On the page:

- A run with no name before launch gets `generate_session_title` with its
  prompt (`persist: true`) on the first `system/init` of each process, so its
  own Claude Code session names it and the name is saved in the session's
  transcript.
- `titledBy` records who named the run: the namer, `Claude Code session`,
  `Claude Code rename` or `You`.
- `system/session_title_changed` (a `/rename`, a rename on claude.ai, or the
  echo of ours) renames the run at any time; a repeated name changes nothing.
  A resumed session gets its saved name back instead of a new one.
- The run is renamed only when its title came from its prompt or from an
  earlier namer: issue runs keep the issue title and a caller's name (a PR
  review, a conflict fix) stays.
- The pre-launch namer finishes before the session starts and
  `generate_session_title` is skipped for its name, so the two never race. A
  held Claude run shows its prompt title until it starts. Settings > Agents >
  "Agent titles" turns Claude's naming off too.
- **Rename…** in the list's context menu saves the new name with `titledBy`
  `You` (no namer overrides it) and sends it to a running session with
  `rename_session` (`source: host`, the CLI's session id); a session renamed
  while stopped gets the name on its next start.

### Prompts

Each prompt is a question card in the transcript
(`ClaudeTranscriptView::addAskUserQuestion`; its heading comes from the
card's `title`, and question and option text are plain text):

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

In Ask, Edit and Plan modes the CLI really asks (a headless run without a
prompt tool refuses every call that needs approval). The external-call gate's
PreToolUse hook still runs first and still only denies.

### Sub-sessions

Claude Code's subagents, background shells and workflows are the run's
sub-sessions. `agentclitasks::Ledger` keeps each open task's description,
subagent type, nesting (`parent_task_id`, `spawn_depth`), last progress
summary (`task_progress.summary`, else its changed description), last tool,
tokens and tool calls. The **Sub-sessions** card of the side panels
(`SessionSidePanels::setSubSessions`, filled by `fillAgentSubSessions`;
[sessions-page.md](sessions-page.md#side-panels)) lists them, nested under
the sub-session that started them, one line each (name, tokens or kind; the
tooltip has the facts and the latest progress), with **Stop** (`stop_task`)
while the session is live. A finished sub-session leaves the card; its Agent
card in the transcript keeps the report. The card keeps its rows and only
changes their text, and it sits beside the transcript, so nothing moves while
subagents run.

### When a run counts as done

The "Agent #N is done!" ping (`notifyAgentDone`) only goes out once a run
reaches Success. A clean `result` from the Claude CLI ends a turn, not the
task. An agent that started background work (a background subagent, a
`run_in_background` shell, a Monitor) gets a `result` while that work is
still running, and the CLI starts a new turn when each task reports back;
treating each `result` as Success would ping several times for one task.

The same ledger (`AgentCliTasks.h`, `m_agentCliTasks`) keeps the run's
`system` `task_started` / `task_updated` / `task_notification` events per
session. A task counts against the run while it is backgrounded
(`is_backgrounded`, or a `task_updated` patch that moves it to the
background), until a notification or a terminal status (`completed`,
`failed`, `killed`, `stopped`) closes it. A clean `result` that arrives while
any background task is open keeps the session Running, with no Success and no
ping; it posts a transcript notice naming the tasks and marks the session in
`m_agentResultAwaitingCliTasks`. The turn the CLI starts when the tasks
report back ends with its own `result`, and that one goes through the usual
subprocess drain to Success and the single done ping.

- If the last task closes and no new top-level turn starts within 30 s
  (`kAgentCliTaskSettleMs`), `settleAgentAfterCliTasks` finishes the run from
  the held `result`.
- If the CLI exits while a run is held, `finishCliTranscriptRun` treats the
  held `result` as the completion.
- A new CLI process for the session clears the ledger, so tasks from a
  stopped process cannot hold the next run.
- A background task that never reports back keeps the run Running until it is
  stopped.

### Usage-limit auto-continue

A run that fails on a usage limit is held under
`agents/usageLimitAgentHold/<id>` with the reset time parsed from the refusal
(`usageLimitResumeAtMs`), else the provider's window reset. A timer, the
reset ping or startup (`restoreUsageLimitAgentHolds`) continues it through
`continueAgentSession` if it is still failed on that limit. Only the refusal
is parsed, never the log tail. Setting: `agents/usageLimitAutoContinue` (on).

## Transcript

`ClaudeTranscriptView::handleEvent` renders Claude Code and Codex stream
events for local, external and cloud runs alike. The Next styling is in
[Transcript in the new interface](#transcript-in-the-new-interface).

### Rows

- **Command rows.** A Bash call folds to one line (`CommandCard`): icon tiles
  (up to ten, then `+N`), the call's description (or command),
  `N lines · Ns`, the approval mark (✓ allowed, ? asking, ✕ denied; click
  opens `src/MainWindowCommandPermissions.cpp`) and an expand caret. The
  terminal panel opens only on click. Icons come from
  `transcripttools::commandIcons`, which splits the command on pipes, `&&`,
  `;`, subshells and substitutions, skips heredocs, redirects, assignments and
  wrappers (`sudo`, `env`, `timeout`, …), and maps each program to an icon in
  `resources/icons/commands/` (terminal icon as fallback). Hovers use
  `CommandHoverCard` via `HoverCardHost`, not `QToolTip`, which blinks on
  re-layout.
- **The approval resolver never waits on Git.** `installAgentCommandApprovals`
  runs while drawing. It copies the session's mode, repository and branch
  first and reads the worktree from `cachedSessionWorktreeForUi()`, which
  never blocks; a nested event loop there can free the session it holds.
- **File edit rows.** Edit, MultiEdit, Write and Codex `FileChange` fold the
  same way (`FileEditCard`): file link and `+N −M` (`transcripttools::editStats`,
  line LCS); expanded, only changed lines show.
- **Prose.** Rows have no bubble; the caption shows a portrait, name and full
  date and time (streamed text is dated from its `message_start` `_ts`). Each
  reply has a copy button that copies the stored Markdown (`proseMarkdown`).
  Caption labels must be created as children of the caption row: a visible
  parentless widget becomes a top-level window and crashes on Wayland.
- **Subagent cards.** An Agent (Task) card ends with **Summary** (the
  report's lead paragraph) and a folded **Full report**.
  `transcripttools::agentReport` strips hand-back framing and usage blocks; a
  `task_notification` with `usage` is matched to its card by `tool_use_id`
  (replays parse `<task-notification>` text).
- **References.** `transcriptReferenceRegex()` links `task:<id>` and similar
  forms as `forkmesh-task:` before the commit alternative; a bare 32-hex id
  links as a commit.
- **Notices.** Claude Code `system/vcs_state_changed` draws a muted notice
  through `transcripttools::systemNotice`, including its `kind` (for example,
  push); an absent kind draws a generic version control state change notice.
  `system/dev_intent` draws the same style with its development kind and
  trigger when present. `command_lifecycle` draws the command state and
  `command_uuid`, so starts and later updates can be matched. Missing fields
  still produce a generic notice, and unfamiliar kinds, triggers and states
  are shown as received. Codex `remoteControl/status/changed`,
  `mcpServer/startupStatus/updated` and `thread/settings/updated` draw the
  same notice rows: remote control notices name the server and status; MCP
  notices name the server and startup status, with failures in the error
  style and any error or failure reason included; thread settings notices
  summarise the model, effort, approval policy, working directory, sandbox
  policy and collaboration mode, without repeating developer instructions.
  Notice rows keep their source event for the raw log jump and appear during
  live updates and history replay in both layout modes.
- **Raw log jump.** Clicking a row's gutter switches to Raw at the line of
  the event that drew it (`m_rowSource`, `transcripttools::rawLogLineStart()`).

### Live task list

`AgentTaskListBar` sits below the rows, in the bottom viewport margin
`layoutPromptDeck()` reserves, and shows the agent's current task list the
way Claude Code desktop pins its list under the latest message: updated in
place instead of a new `TodoWrite` row scrolling away with every change.

- `agenttasks::Tracker` folds the stream: `TodoWrite` and Codex plans replace
  a list; `TaskCreate` adds a task at once and takes its id from the
  `Task #N created` result (a failed create is dropped); `TaskUpdate` changes
  status, subject or active form, and `deleted` removes the task.
- Codex sends `turn/plan/updated` several times a turn, but
  `CodexAppServerSession::emitToolUse` passes only the first one per turn
  (the synthetic id is `plan-<turnId>`); later ones come only as
  `_codex_tool_delta`. The tracker reads those deltas, so a Codex plan
  updates live.
- Subagents keep their own lists. Events with a `parent_tool_use_id` go to a
  group headed by the Agent/Task call's `description` (`⎿ Explore the
  parser`), shown indented under the main list. The group goes away when that
  subagent's tool result arrives, as the subagent is done.
- Wording and marks follow Claude Code: the header is
  `5 tasks (2 done, 1 in progress, 2 open)`, rows are `◻` open, `◼` in
  progress (bold, showing the `activeForm`), `✔` done (struck through). At
  most `kMaxRows` (8) rows show, then `… +N more`.
- The bar emits `heightChanged` only when its text, fold or visibility
  changes. `clear()` resets it on every session switch, and an unconditional
  signal would run `layoutPromptDeck()` mid-replay and lose the bottom-follow
  latch ("a deferred Agent transcript replay stays pinned to its latest
  turn").
- Click the header to fold the list to one line, which then names the
  current task. The bar folds itself when every task is done and opens again
  when new work arrives, unless the user folded it. A new prompt after an
  all-done list clears it.
- The Agents list shows the same progress on each row after the title, e.g.
  `◼ 2/5 · Running tests`, `◻ 0/3` (nothing started) or `✔ 5/5`.
  `MainWindow::m_agentTaskTrackers` keeps one tracker per session, fed by
  `applyTranscriptEvent` through `feedAgentTaskTracker`, and built from
  `m_streamEvents` the first time a row asks (`agentTaskProgress`); a session
  whose history is not loaded shows nothing rather than caching an empty
  tracker. Running rows refresh it on the spin tick, which already repaints
  them, by recomposing the text from `kAgentTitleBaseRole` and
  `kAgentTaskProgressRole`, so a task change costs no table rebuild.
- Events skipped when a long session opens on its tail still pass through
  the tracker (`accumulateStatsOnly`), so the list is right after reopening.
  Earlier events loaded by scrolling up (`prependEarlierEvents`) are not fed,
  since they are older than what the tracker already holds.

### Pinned prompt

`PromptDeckBar` sits above the rows, in the top viewport margin
`layoutPromptDeck()` reserves, and shows the last prompt in one line. It is
the desktop version of the phone's prompt deck.

- Edges peek out below for earlier prompts. With several prompts it shows
  `2/5` and ‹ › arrows.
- To move between prompts, drag the card sideways, scroll sideways on a
  trackpad, or press ← →. Right goes to the previous prompt, left to the next.
- Click to read the whole prompt (up to eight lines). Right-click to copy it
  or jump back to the latest.
- Prompts come from `addUserTurn` and also from `accumulateStatsOnly`, so the
  turns a long replay skips still count. `prependEarlierEvents` does not add
  them again, and `clear()` empties the bar.

### Transcript in the new interface

In the new interface ([ui-redesign.md](ui-redesign.md)) the agent transcript
(`ClaudeTranscriptView.cpp`) follows the mockup on both the Sessions and
Agents pages.

- **User turns** are right-aligned `surfaceActive` bubbles: 16 px corners with
  a small bottom-right one, at most 80% of the column wide, with the time below
  in `fgSubtle`.
- **Commands.** Back-to-back shell commands fold into one "Ran N commands
  (M failed)" row. It opens to a bordered card listing each command with a
  status dot.
- **Assistant prose.** Finished prose is 15 px with 165% line height, and
  inline code shows as mono chips.
  - The prose is converted once per message through `QTextDocument` and
    cached, because a markdown `QLabel` cannot take line height or code
    backgrounds.
  - Streaming text is converted only when it completes, so the 60 ms flush
    stays cheap.

### Transcript coverage

The transcript should show everything the raw log holds.
`forkmesh::transcriptcoverage` (`src/TranscriptCoverage.*`) checks every
event the view is given (`handleEvent`, and `accumulateStatsOnly` for the
turns a long replay skips). Any kind that draws nothing is a gap: an unknown
event type, `system` subtype, assistant or user content block, stream event
or delta.

Some raw log lines never become an event, so the sessions record them. Each
one becomes a `_local_raw_line` event stored with the session, so the check
survives a redraw or a restart. The kind is worked out again from the stored
line each time, so a fix also clears old sessions. Lines longer than 16,000
characters keep only the fields that name their kind.

- Claude Code (`ClaudeStreamSession::noteDroppedLine`): stdout that is not
  JSON, and control requests that `handleControlLine` consumes without a
  card. Those are requests ForkMesh refuses (`hook_callback`, …) and dialogs
  it cannot answer (`request_user_dialog` of an unsupported `dialog_kind`).
  Answers to ForkMesh's own control requests and stale cancels are
  bookkeeping.
- Codex (`CodexAppServerSession::noteUnshownMessage`): notifications that
  `handleNotification` falls through, items that are neither tool cards nor
  drawn items, and server requests it refuses.

A red **● N missing** pill (`transcriptCoverageGaps`) then shows at the top
right of the transcript. Its tooltip lists each kind and count. Clicking it
starts an agent in the ForkMesh checkout (`stallReportRepoIndex()`), with the
default provider and model from Settings, like the Tool efficiency page's
**Send to agent**. The prompt (`transcriptcoverage::agentPrompt`) names each
kind with one clipped example. The agent either draws the kind or adds it to
the unshown list below with a reason, then updates `TranscriptCoverage.cpp`
and `tests/test_transcript_coverage.cpp` so the pill stops flagging it. A
kind already handed to an agent that still exists is not sent again; a second
click opens that agent.

These kinds are deliberately not drawn, because another surface shows them
or they are bookkeeping:

| Kind | Why |
| --- | --- |
| `rate_limit_event`, `_codex_rate_limits`, Codex `account/*` | The Usage page shows them |
| Codex `turn/moderationMetadata` | Internal safety-check bookkeeping (category flags, empty generation/tool metadata) with no transcript content; user-facing failures arrive through the normal error and turn-result rows. It stays in Raw |
| `control_request`, `control_response`, `control_cancel_request`, `keep_alive` | Host control protocol; prompts arrive as their own card events |
| `system` `task_started`, `task_updated`, `task_progress`, `background_tasks_changed` | The Sub-sessions list shows them |
| `system/session_title_changed` | It renames the agent |
| `system/commands_changed` | It refreshes the composer's `/` menu, which lists the commands and skills |
| `system/status` other than `status: compacting` | Only update state; compacting has a row |
| `summary`, `file-history-snapshot`, `queue-operation`, `custom-title`, `ai-title`, `last-prompt`, `isMeta` lines | Claude Code `.jsonl` bookkeeping |
| Any event with a `parent_tool_use_id`, and sidechain events | The subagent's card summarises them |
| User `text` blocks and string content | Prompt echoes; the prompt row shows them |
| Streamed `input_json_delta` and `signature_delta` | The finished assistant message draws the tool call |
| Codex `thread/status/changed`, `thread/archived`, `thread/unarchived`, `thread/closed`, `serverRequest/resolved`, `skills/changed`, `app/list/updated`, `mcpServer/oauthLogin/completed`, `fuzzyFileSearch/*`, legacy `codex/event/*` duplicates | Bookkeeping |

Stderr is never judged. `CodexAppServerSession::isToolItem` reads the same
Codex tool item list (`transcriptcoverage::isCodexToolItem`), so the two
cannot drift.

### Scrolling

- The follow-tail latch (`m_stickBottom`) changes only on
  `QScrollBar::actionTriggered`, never on programmatic clamps.
- `ScrollJumpButtons` shows a jump-to-latest arrow off the tail, with a dot
  when new content arrived.
- Rebuilding the transcript on screen holds a grab of the last frame
  (`setBatchHoldsFrame`) and restores the position in the same thaw.
  `CacheLabel` estimates heights only from its on-screen width.
- Sending a follow-up jumps to the tail; a resume keeps the rendered rows; a
  cloud follow-up is drawn at once until the relay history replaces it.
- Cloud runs and external Claude Code sessions redraw the whole transcript
  when the relay history or the `.jsonl` grows. Redrawing the session already
  on screen goes through the same held frame and keeps the reader's place, so
  an update never pulls someone reading earlier turns back down. Only opening
  a session, or a reader already at the tail, lands at the end.

### Keeping live updates cheap

- A live event appends rows; it never rebuilds the view. When the view is
  behind by up to 32 events (a modal dialog was open, or a notice was added
  from inside another event's handling), `applyTranscriptEvent` draws the
  missing ones in place; a longer gap goes through `renderTranscriptTail`.
  Only a session that is not on screen yet takes
  `renderTranscriptForSession`, because a rebuild per event, with
  `beginBatchBuild` grabbing the whole column, froze the window for seconds.
- The time-sliced replay reads `m_streamEvents` by index instead of keeping
  its own copy, which shared the list and made every live event appended
  during a replay copy the whole history.
- `ClaudeTranscriptView::clear()` hides the old rows and deletes them a few
  at a time, 8 ms per event-loop turn (`retireRow`, `deleteRetiredRows`),
  rather than `deleteLater()` on hundreds of command cards in one pass.
- Deleting a session hands its event list and raw buffer to a pool thread to
  free (`releaseOffGuiThread`, `MainWindowAgentsInternal.h`); `QJsonObject`
  is implicitly shared, so whichever owner drops the last reference frees it.
- The raw buffer keeps its last 300,000 characters; a line longer than that
  replaces the buffer with its own tail instead of being appended and then
  cut (`appendBoundedStreamRaw`).
- Rate-limit events from a transcript are applied at most once every 2 s,
  latest payload wins (each change rewrites a dozen settings).
- `disabledAgentToolFamilies()` is cached and updated by
  `setAgentToolFamilyEnabled`, not read from `QSettings` per event.

## Composer

The prompt box keeps drafts, spell-checks and takes images. It holds at most
16,000 characters (not shown). The system dictionary (Hunspell, or a plain word list)
loads on the global thread pool the first time any highlighter asks for it;
until it is ready a highlighter marks nothing, and every highlighter that
asked is rehighlighted when it arrives (loading it inline cost about a second
on the GUI thread at startup). Its usage meter and account menus are in
[agent-usage.md](agent-usage.md#composer-usage-meter-and-menus).

| Control | Setting | Effect |
| --- | --- | --- |
| Branch toggle (`promptWorktreeCheck`, git-branch icon) | | Own worktree, or the checkout directly |
| **Cloud** (`promptCloudButton`) | `agents/composerRunInCloud` | **new** queues the run on an account agent node (`startQuickAddCloudAgent`, signed `POST /api/repo/<owner>/<repo>/agent-sessions`); Claude Code and Codex, published repositories, no attachments |
| **Vibe** (`promptVibeCheck`, sparkle icon) | `agents/composerVibe` (off) | Stamps `AgentSession::vibe`; a successful worktree run is merged with clean-up (`maybeAutoMergeForSession`) and pushed (`pushCurrentRepoUpstream`). All merge gates apply |

The repository comes from `resolveAgentRepositoryIndex` for local and cloud
runs. Phones expose the same switches through `agents.start`.

### Floating bar, placement and resizing

- The three toggles are icon buttons in the composer's floating button bar
  (`FloatingPromptBar`, `promptDragHandle`), after the channel picker and
  before the clear (×) and reset buttons. An "on" toggle gets a thin accent
  border, so the branch icon (on by default) reads as connected.
- The bar is not a row of its own: it floats over the composer's top-right
  corner, so the chat preview starts at the very top. Its left edge fades
  from transparent to the composer's surface, so a chat line running under
  the buttons fades out instead of colliding with them. With no chat to
  show, a band of the bar's height (`promptTopSpacer`) keeps it off the
  prompt text.
- **Dragging.** The whole body drags the composer, not just the bar:
  `promptWrapper` and every widget in it that is not a control
  (`isPromptDragSurface`: not a button, picker, text field, slider or list)
  are watched, so a press on the chat lines, the bottom bar's empty space or
  the gaps around the send column that moves past the drag distance hands off
  to the bar's drag (`handlePromptPlacementEvent`). Each widget is watched
  directly because a rich-text label keeps its own presses instead of passing
  them up. The prompt text, buttons and pickers keep their presses, and a
  plain click on a chat line still opens its channel. The composer can be
  moved anywhere; dragging it out of the window tears it off into a
  frameless window of its own (`tearOffPromptOverlay`), and a double-click
  docks it. This works from the centred Agents-page layout too: the first
  drag lifts the composer out to float. There is no pop-out button.
- The reset button (`promptResetButton`, sync icon) at the bar's right end
  snaps the composer back to its default size and place
  (`resetPromptOverlayPlacement`), as double-clicking the bar does.
- **Resizing.** Every edge and corner is a resize handle (`PromptResizeEdge`,
  `promptResizeEdge`, eight invisible bands 5 px thick with 10 px corners,
  children of `promptOverlayHost` laid over `promptWrapper`). The cursor shows
  the direction; dragging moves only the grabbed edges (`resizedPromptRect`),
  keeps the 280×120 minimum and stops at the window's edges. A docked
  composer lifts off the footer on the first resize, as it does on a drag; a
  popped-out composer asks the window system to resize from that edge
  (`startSystemResize`). The main window's frameless-border handler
  (`handleFramelessResizeEvent`) only takes presses and cursor changes from
  widgets in the main window, because Wayland reports every window at the
  origin and a popped-out composer's bands would otherwise fall inside the
  main window's 8 px border. The bands show whenever the composer does, the
  centred Agents-page layout included (its first resize lifts it out to
  float); only the collapsed avatar has none. The bar keeps a 10 px right
  margin so the top-right corner never covers its buttons.

### Chat in the composer

- **Channel picker** (`promptChannelButton`, `m_promptChannelButton`): the
  bar's first control names the chat channel the composer is in
  (`m_currentConversation`) with a ▾. Clicking it opens `promptChannelMenu`
  (`showPromptChannelMenu`): one row per channel (the sidebar's `m_channels`
  plus `unlistedChatConversations`), then a "Direct messages" section with
  one row per direct message (`m_openDms`, `m_officeDirectConversations` and
  unlisted ones), each with its user and chat counts after a tab, the current
  one checked. Picking a row calls `switchConversation`. The last row,
  "Message someone…" (`promptChannelMessagePerson`), opens the new-message
  people picker (`showNewMessageDialog`), so the composer can send straight
  to a person who has no conversation yet. Users come from the office
  mirror's member count, else the open room's members button, else the
  distinct senders in the loaded history plus you; chats are the loaded
  top-level messages.
- **Chat preview** (`ChatPreviewLabel`, `promptChatPreview`): up to three
  one-line rows over the prompt with the three latest top-level chat messages
  across every loaded conversation (`m_history`), newest last. Each row is
  the sender's small round avatar (16 px, `chatActorAvatar`), then the
  channel (muted), the user (bold), a compact time (`now`, `5m`, `3h`, `2d`,
  then the date, muted) and the message, elided to the width. Clicking a row
  opens its channel on the Chat page. The rows never come from agent
  sessions. `refreshPromptChatPreview` runs (debounced 250 ms) whenever a
  conversation's sidebar preview changes (`refreshChatListPreview`). The rows
  are painted at a fixed pitch (avatar height plus a 4 px gap) and the docked
  composer's height is recomputed whenever the row count changes
  (`baseAnchoredHeight`), so the third line is never clipped.

### Send buttons

- **new** (plus) starts a run, **add** (paper airplane) sends a follow-up and
  **chat** (comment) posts to a chat channel. All three carry small icons in
  both interfaces (`tintComposerActionButton`; on the accent fill of the
  Enter target the icon uses the on-accent colour).
- The follow-up button always reads **add**, chat preview or not (the
  centred workspace layout's send button reads **Add** when it is the Enter
  target).
- **chat** (`m_quickAddSendToChannelButton`, object name `quickAddGenieButton`
  for the stylesheets) sends to the channel the picker names
  (`m_currentConversation`, labelled by `chatConversationLabel`; the tooltip
  and accessible name carry it) through `sendChatText`, the same path as the
  Chat page's own composer (`sendCurrentMessage`), refusing attachments and a
  room you cannot post in. `updateChatConversationHeading` and
  `updateQuickAddEnterTarget` refresh its label. While a chat room is open on
  the Chat page (`promptChatRoom`) it takes Enter (and the centred layout's
  send button), and **add** and **new** step aside. It carries the
  `enterTarget` highlight then (the accent fill in the new interface, a solid
  purple border in the classic one), so the button Enter will press is the
  one lit up. A package's own prompt takes the highlight back.
- Sending plays a `PromptFlightOverlay` (sparkle to the Agents rail button,
  or a floating bubble) instead of a card; the overlay is click-through and
  deletes itself. Notices are inline in the page, never floating
  ([diagnostics.md](diagnostics.md)).
- `voice/inputEnabled` (on) shows the dictation mic.

### Agent and model dropdown

- The dropdown (`quickAddAgentModelSelector`, `fillAgentModelSelector`) also
  offers destinations that start no agent (`quickAddProviderIsPromptAction`):
  **Manual · create issue**, **ActivityPub · post**, **Take a note**
  (`takeQuickAddNote`: a local note in `<data>/notes/notes.json`, first line
  as title, attachments as image links) and **Task · assign**.
- **Task · assign** shows an assignee picker (`quickAddTaskAssignee`, last
  pick in `tasks/composerAssignee`) listing the organization's task members,
  you first; submitting posts `/api/tasks` with `assigneeKind: user` and that
  member, or, left on Unassigned, files the task in General. Members come
  from the task catalog (`organizationTaskMemberNames`); picking Task before
  the catalog was read starts the launch read.
- The last row, **Edit models…**, opens the Models board
  (`openComposerModelsPage`), whose **Show** column ticks which models the
  dropdown lists (`agents/composerHiddenModels`, same list as Settings →
  Composer models).
- **Review with AI** on a pull request (`reviewCurrentPullWithAi`) runs with
  the agent and model the composer has selected. Claude API and OpenAI with a
  saved key send one request straight to the API; every other agent runs its
  own command from `agentConfigForProvider` once, read-only, in the
  repository's checkout (`aiReviewRunAgentCli`). The review only falls back
  to choosing for itself (Claude key, then OpenAI key, then Claude Code) when
  the composer is on Manual, ActivityPub, Note or Task. Some CLIs, such as
  Codex, print the prompt back before their answer, so
  `parseAiReviewFindings` tries again from each earlier `[` before the
  reply's last `]` until it finds a JSON array.

### Typing, drops and modes

- **Shift+Tab** moves `quickAddModeSelector` to its next mode, wrapping, the
  same as picking it. The key is not taken when the mode picker is hidden.
- **A picked mode sticks to the session.** Picking a mode in the dropdown, or
  with Shift+Tab, stores it on the session the composer is aimed at right away
  (`applyComposerModeToTargetSession`) and switches a live Claude Code process
  to it when the CLI allows, so a redraw of the session
  (`syncQuickAddControlsToAgentSession`) keeps it. Answering a permission card
  copies the CLI's mode back onto the session (`syncClaudePermissionMode`)
  only when that answer changed the CLI's mode (for example leaving plan
  mode); a process launched under Edit cannot become Auto mid-turn, so Auto
  stays stored and the next launch runs under it.
- **`/` opens the command menu.** When the prompt is empty, no chat room is
  open and the composer's **+** button is visible, typing `/` puts the `/` in the prompt
  and opens the "/" menu (`openQuickAddSlashActions`) with its filter
  focused, so the next letters filter the list. Picking a row removes that
  `/` first. Closing the menu without picking (Esc) puts what was typed in the
  filter back after the `/`, so a prompt that starts with a path such as
  `/etc/hosts` can still be typed.
- **Dropping files.** The prompt's viewport takes drags of local files
  (`src/PromptDrop.h`). Images (`png jpg jpeg gif webp bmp`) and videos are
  queued as attachments (`queueQuickAddImage`); any other file or folder is
  inserted at the drop point as an `@` mention, relative to the first of the
  selected session's worktree, the workspace's repository and the Issues
  repository that holds it (absolute otherwise), folders with a trailing
  `/`, quoted when the path has a space. Drops of text or web links keep the
  editor's default behaviour. `MainWindow::eventFilter` returns early for
  event types it does not list, so `DragEnter`, `DragMove` and `Drop` must
  stay in that list.

### Worktree creation

- A run's worktree is added by `agentwt::addScript`, retried three times. A
  `git worktree add -b` that fails after making the branch (for example with
  the disk full) leaves `<id>-<slug>` behind, so the script checks the branch
  out when it already exists and creates it only when it does not; otherwise
  every retry fails with "a branch named … already exists". The error shown
  keeps the first attempt's output when it differs from the last, so the real
  cause is not hidden.
- **`.worktreeinclude`.** When the checkout has a `.worktreeinclude` file,
  files that match its patterns (gitignore syntax) **and** are ignored by Git
  are copied from the checkout into the new worktree before the agent starts
  (`MainWindow::copyWorktreeIncludes`, `src/WorktreeInclude.h`). Git does the
  matching: `git ls-files --others --ignored --exclude-from=.worktreeinclude`
  lists the matches and `git ls-files --others --ignored --exclude-standard
  --directory` the ignored set; a match is copied when it is in that set or
  under an ignored folder. Files already in the worktree are never
  overwritten, symlinks are skipped, and the copy runs on a worker thread.
  The agent log gets one `== .worktreeinclude: copied N files` line. Tracked
  files are never copied, since the worktree already has them. Without the
  file nothing changes.

## Workspace layout in the new interface

In the new interface (`app/design` = `next`) the Agents page follows the
app's layout mode ([ui-redesign.md](ui-redesign.md), "Two layout modes"):
**Focus** (default) is the layout below, **Control Tower** the compact page
(`src/LayoutMode.h`, `src/WorkspaceLayout.h`, `workspaceLayoutNew()`). Code:
`src/MainWindowWorkspace.cpp`; styles: `resources/theme/next/61-workspace.qss`.
It is Qt Widgets, not QML.

| Area | Contents |
| --- | --- |
| Session list header (`workspaceSidebarHeader`) | Repository, **New session**, search (mirrors the top bar), count, Sort, bulk actions |
| Session rows | Three-line cards: status badge, provider · status · age, title, location and line counts |
| Footer (`workspaceSidebarFooter`) | "N of M agents running", opens the limit menu |
| Conversation header (`workspaceHeader`) | Title, status chip, Stop/Continue, diff, Terminal and Browser toggles, action menu |
| Changed-files card (`workspaceChangesBar`) | Files, totals, branch, **Open diff** |
| Composer | Rounded box with one send button (`enterTarget`) and an options menu |
| Side panel (`workspacePanelStack`) | **Diff**, **Terminal**, **Browser**, **Details** |

### Rules of construction

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
  the classic layout. Do not read the first item instead: when anything is
  put above the status row the lookup fails and the new layout silently never
  installs.

### Details tab and the Control Tower column

The **Details** tab (header toggle `workspaceToggleDetails`) holds the same
`SessionSidePanels` as the Sessions page
([side panels](sessions-page.md#side-panels)), draggable in the same order.

- `syncWorkspace` feeds it the selected session's `m_streamEvents` from the
  last count it read (a cloud run's events are rebuilt when its history
  changes) and fills the session facts through
  `NextAccess::fillSessionSidePanels`.
- When a local agent starts and it is the selected session (or none is
  selected), `NextAccess::workspaceSessionStarted` opens the panel on
  Details.
- `NextAccess::feedSessionSidePanels` feeds a panel stack its session's facts
  and the events it has not read yet (the read count lives in the panel,
  `SessionSidePanels::feed`), so Focus and Control Tower share it.
- In **Control Tower** the same cards are a right column of the detail pane
  (`m_agentTowerPanels`, the third pane of `agentCodeSplit`, after the
  transcript and the Code page; `buildAgentTowerPanels`). It shows while a
  session is selected and the layout is not Focus, starts 220–320 px wide,
  can be dragged narrower or shut at its splitter handle, and refreshes once
  a second while it is on screen (`refreshAgentTowerPanels`), and at once on a
  sub-session event. Focus hides it and uses the Details tab; switching modes
  (`syncWorkspace`) re-checks it, and the Code page, put back on the way to
  Control Tower, goes in before the column.
- Switching into the Workspace view still opens on Terminal, the point of
  that view.
- The browser tab (`src/WorkspaceBrowserPane.*`, library
  `forkmesh-browser-pane`) embeds an ungoogled Qt WebEngine view when the
  build finds Qt WebEngine, and opens pages in the system browser otherwise
  ([browser.md](browser.md)).

### Shortcuts and Open in

- **Workspace shortcuts.** `src/WorkspaceShortcuts.h` holds the table:
  Ctrl+Tab / Ctrl+Shift+Tab and Ctrl+Shift+] / [ between sessions,
  Ctrl+Shift+D / Ctrl+\` / Ctrl+Shift+B for the panel tabs, Ctrl+\\ to close
  the panel, Ctrl+Shift+M / I / E for the mode, model and effort pickers,
  Ctrl+/ for the sheet. `NextAccess::installWorkspace` makes a window-wide
  `QShortcut` for each bound row and turns them on only while the new layout
  is applied and the Agents page is visible (`Chrome::enableShortcuts`,
  driven by the page's Show/Hide), so Ctrl+Tab and the others never steal
  keys from other pages. macOS uses the Control key (`Qt::META`) for Ctrl+Tab
  and Ctrl+\`, as Claude does, since Cmd+\` switches windows there. Ctrl+N
  stays with the sidebar's New session button and Shift+Tab with the
  composer; both are listed on the sheet but bound where they were. The mode,
  model and effort menus only open when that picker is visible and enabled.
- **Shortcut sheet.** Ctrl+/ (or the session list's **…** menu, "Keyboard
  shortcuts…") opens `workspaceShortcutSheet`, a dialog listing every row of
  the table.
- **Open in.** The session **…** menu has **Open in** with VS Code, VSCodium,
  Cursor, Zed and Windsurf when their command (`code`, `codium`/`vscodium`,
  `cursor`, `zed`/`zeditor`, `windsurf`) is on PATH (`src/WorkspaceOpenIn.h`),
  then **File manager**, plus **Copy folder path** and **Copy branch name**.
  The folder is the session's worktree (`m_streamWorktree`, else
  `cachedSessionWorktreeForUi`, which never blocks), else the repository
  checkout. Editors installed only as macOS app bundles are not found.

## Cloud runs in the Agents list

Rows come from `GET /api/account/agent-sessions` (which runs it lists, and
the `finishedBefore` cut, are in
[agent runs](../agents/README.md#access-rules-on-the-relay)).
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
  unmerged `agent/` branches with `--no-tags --no-write-fetch-head` and
  writes only `refs/heads/agent/*` through `git update-ref` with the expected
  old value: create or fast-forward. A checked-out branch or one with local
  commits is left alone; nothing is deleted. The branch button opens the Git
  view, else the website's compare page.
- **Actions** are signed requests to the App with proof
  `forkmesh-owner-agent-branch-v1`:

| Action | Request |
| --- | --- |
| **Merge & clean up** | `{"action":"merge","cleanup":true,"requestId"}`, re-posted while `202 processing`; then the linked task is completed |
| **Clean up** | `{"action":"cleanup"}` |
| **Pull <base>** | `{"action":"update"}` |
| **Stop** | `{"action":"stop"}` |
| **Create PR** | Sync the branch, `createPullFromBranch`, then `{"action":"link-pull","number":N}` |
| **Continue**, **Fix conflicts** | A signed follow-up (`sendCloudAgentFollowUp`) |
| **Continue here**, **Retry in cloud**, **Sign in runner** | See [moving runs between places](../agents/README.md#moving-runs-between-places) |

- **Merge & clean up** from the desktop calls `retireMergedCloudAgentSessions`:
  matching cloud rows leave the list, are remembered in
  `agents/cloudRunsRetired` (last 500) and get a silent relay delete.

### Live output of a cloud run

The selected cloud run streams its events into the transcript like a local
run; nothing is read for other runs. The relay and runner halves are in
[agent runs](../agents/README.md#live-output-of-cloud-runs-on-the-desktop).

- **Who is followed.** `pumpCloudAgentStream()` follows only the selected
  session when it is a cloud row with status `running`, the page is visible
  and the window is not minimized (`CloudAgentStreamWatch::follow`). Anything
  else drops the cursor; the relay stops sending 30 s after the last read.
- **Reads.** `GET <session route>/stream?after=<seq>`, signed with the
  session read proof, on the `cloud-agent-stream` doorbell, again on `more`,
  otherwise every 3 s (10 s after a failure). One read at a time; stale
  answers are discarded.
- **Decoding.** `CloudAgentStreamDecoder` joins base64 chunks into JSON
  lines; `truncated` drops the partial line, `job`, `stream` or `reset`
  restarts.
- **Drawing.** Events go through `ClaudeTranscriptView::handleEvent` (max
  2000) and are shown only while status is `running`, so the final history
  turn replaces them without duplicates.
- While Watch is open with a run in flight, the account list is read every
  5 s ([activity-and-sounds.md](activity-and-sounds.md#watch-market-package-agent-watch)).

## Branches and merging agent work

Code > Branches lists branches in a sortable table: Agent / task links
(`refreshBranchLinks()`), Branch, Status, Behind / Ahead, Commit, Message,
Author, Changes, Updated, Worktree. Writers that change cells after load
pause sorting (`BranchSortPause`), because a sorted `QTableWidget` moves rows
as their sort cell changes. The Git page's diffs are in
[branch-diffs.md](branch-diffs.md).

- The diff header's **Conflict** button calls `fixAgentConflictsWithAgent()`
  when the branch belongs to an agent, otherwise **Fix with agent** starts
  one. **Pull <base>** is highlighted (`needsPull`) when the branch is
  behind.
- **Pull main** (`updateBranchFromBase`) and **Merge & clean up**
  (`requestMergeWorktreeIntoMain`) refuse to run while the other runs.
  Background reads use `GIT_OPTIONAL_LOCKS=0`. `Unable to write index` is not
  treated as lock contention, because `--autostash` may already have stashed
  edits ([git-locking.md](git-locking.md)).
- **Uncommitted work in the branch's worktree.** `mergeWorktreeIntoMain`
  records in `WorktreeMergeFlow` whether an agent session on the branch is
  still working (`agentSessionWorkInFlight` → `agentWorking`) or has stopped
  (`idleAgentId`). When `prepareWorktreeMerge` finds the worktree dirty:
  - **Merge & clean up** with a stopped agent: everything is committed on the
    branch as "Agent #N: commit the changes it left uncommitted", the files
    are logged, and the merge continues; the worktree is removed only after
    that commit. If the commit fails, nothing is merged and the notice gives
    git's reason.
  - The agent is still working: the merge is refused until it finishes;
    nothing is committed while an agent may still be writing.
  - No agent owns the branch, or a plain **Merge** (which keeps the
    worktree): refused, so a person's own edits are never committed for them.
    The notice lists up to five files (`describeDirtyPaths`) and says whether
    the edits would be lost with the worktree or left out of a plain merge.

  Tested in `checkErrorLogTriage` (`desktop/tests/test_window_resize.cpp`).
- After merging the shown branch, `applyWorktreeMergeStep` covers the Git
  workspace with a merged page (`m_gitMergedPage`) holding **Back to
  agents**, **Back to branches** and the merge stats. **Leaderboard**
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

## Tool efficiency and Models board

The tool-efficiency page is built the first time it is opened
(`ensureAgentEfficiencyPage`); until then a placeholder holds its slot in the
page stack, because building and style-polishing it during deferred startup
stalled the window. The `test*` accessors for its widgets build it first.

### Runs view

`src/AgentRunTable.*`, `src/AgentRunEfficiencyLog.*`. A run is one prompt to
its `result` event, recorded with provider, model, tool families off at
launch, outcome, time, tokens, tool calls and errors and estimated cost
(`AgentTokenPricing`). Efficiency is `1 − tokens at stake / tool tokens`.
Runs append to `agent-runs.jsonl` (last 2000) beside `tool-efficiency.json`;
the top table groups by runtime and tool configuration with a **vs all on**
column.

### Trends view

`src/AgentPerformanceHistory.*`, `src/AgentPerformanceCharts.*`. Twelve
charts for the last hour, 24 hours, 7 days or 30 days, each with a dashed
line for the period before and the change against it, so a change to the
prompt, a tool family, the agent limit or the computer can be judged by its
result: agents running (average, peak in the tooltip), memory used, CPU busy,
tool calls, tool error rate, tool call time, context per call, tokens at
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

### Models board

`src/AgentModelTable.*`, `src/MainWindowAgentModels.cpp`, numbers from the
widget-free `AgentModelStats::build`: one row per model in
`composerModelCatalog()` plus any seen elsewhere. Sources: `m_agentSessions`,
`model-outcomes.json` for deleted agents, reported or estimated cost (`~`),
`AgentToolEfficiencyLedger::modelUsage`, and a background `git log` per local
repository (`AgentModelStats::parseGitHistory` credits each merge to the most
frequent model trailer on its merged side; `ForkMesh-Agent:` wins over
`Co-Authored-By`). The git columns count history on disk and differ from
**Merged** on purpose. The **Show** checkbox exists only for rows in the
composer catalog; models seen only in past runs cannot be offered there.

### Portraits

Icons in `resources/agent-ui/icons/` are 256 px palette PNG robot faces (WebP
is avoided because Qt's decoder plugin is not always shipped). New special
agent portraits also need phone crops ([special-agents.md](special-agents.md)).

## Testing

Build the test targets with `cmake -S desktop -B desktop/build
-DFORKMESH_BUILD_TESTS=ON` (`./run.sh` configures them off;
[build-and-test.md](build-and-test.md)).

| Area | Tests |
| --- | --- |
| Window behaviour | `forkmesh-window-tests` with `--workspace-snapshots-only`, `--agent-list-view-only`, `--agent-folders-only`, `--agent-task-link-only`, `--command-approval-only`, `--agent-code-only`; `tests/test_window_resize.cpp` |
| Transcript | `forkmesh-claude-transcript-tools-tests`, `tests/test_transcript_coverage.cpp` |
| Claude Code session control | `forkmesh-claude-control-tests`, `forkmesh-agent-cli-tasks-tests` (`tests/test_agent_cli_tasks.cpp`), `runHostControlProtocolTest` in `forkmesh-agent-transport-tests` (a scripted `claude` on stdio) |
| List | `forkmesh-agent-folders-tests`, `forkmesh-agent-ticker-tests` |
| Workspace | `forkmesh-workspace-parity-tests` (`tests/test_workspace_parity.cpp`: no duplicate shortcut keys, editor detection, the drop plan, `.worktreeinclude` end to end in a scratch Git repository) |
| Cloud | `forkmesh-agent-cloud-stream-tests`, `tests/test_agent_cloud_sessions.cpp`, `tests/test_upstream_checkout_sync.cpp`, `tests/validate_cloud_agent_stream_contract.py`, `tests/validate_cloud_agent_watch_contract.py` |
| Models and activity | `forkmesh-agent-model-stats-tests`, `forkmesh-user-activity-tests` |

Sessions, usage, special agents and sounds list their tests on their own
pages. Window tests run offscreen; their `settle` step must also deliver
deferred deletes. `FORKMESH_AGENT_ROWS_SHOT` and `FORKMESH_AGENT_FOLDERS_SHOT`
save row images.
