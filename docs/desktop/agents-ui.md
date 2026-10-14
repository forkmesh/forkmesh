# Desktop: Agents page, activity window and sounds

Part of [the desktop docs](README.md). How runs work underneath (runtimes,
sandboxing, website jobs, mirrors, errors): [../agents/README.md](../agents/README.md).

## Agents page

Rail **Agents**. `src/MainWindowAgents.cpp` builds the page; the list is
`src/MainWindowAgentList.cpp`, the detail `src/MainWindowAgentDetail.cpp`, the
transcript `src/MainWindowAgentTranscript.cpp` and `src/ClaudeTranscriptView.*`,
launch `src/MainWindowAgentLaunch.cpp`, usage `src/MainWindowAgentUsage.cpp`.

- The list merges local sessions, external Claude Code sessions found on this
  machine (`src/ClaudeSessionScan.*`), and cloud sessions run by the account's
  mirrors (`src/AgentCloudSessions.*`, `src/MainWindowAgentCloud.cpp`). Cloud
  runs whose target is this desktop are dropped; they already exist locally.
- Only stored runs (positive ids) belong in `data/agents/sessions`.
  `AgentStore::saveSession` refuses an id of 0 or below, and `loadAllSessions`
  skips one. Until 2026-10-06, `updateAgentMergeCandidates` saved an external
  row too: `-1005`, agent 1432's own first Claude Code transcript seen from
  outside, was written to disk as a "Claude Code session" carrying branch
  `1432-quality-persistent-quality`. That record looked like a second session
  sharing the branch. The branch-wide sweep in
  `deleteWorktreeBranchAndAgentInBackground` already skipped the external
  band, so deleting the row never reached the branch or agent 1432.
- An agent with no organization task gets **Add task** in its detail rail and
  **Create task from this agent** / **Attach to an existing task...** in the
  list's right-click menu (`src/MainWindowAgentTaskLink.cpp`); see
  [tasks-page.md](tasks-page.md#creating-or-attaching-a-task-from-an-agent-2026-09-27).
- The toolbar has **Watch** (the activity window), **Tool efficiency** (the
  provider-neutral tool board, `src/AgentToolEfficiencyBoard.*`), **Listen**
  and **Announcer**.
- **Models** (since 2026-09-27) opens the model board, page 3 of
  `agentPageStack` (see [Models board](#models-board)).
- The detail header always has **Continue in cloud**; the list's right-click
  menu has **Delete agent**. Transcripts show an approval mark per command, a
  permissions dialog (`src/MainWindowCommandPermissions.cpp`) and a per-tool
  usage table (`src/AgentToolUsageTable.*`).
  Since 2026-10-04 (#1360) every Bash call folds to one summary line
  (`agentCommandSummary`): an arrow, a stack of overlapping icons, the
  call's description. Since #1417 the icons are separate rounded tiles (ten at
  most since #1473, then a `+N` tile for the rest; it was five, or four and
  `+N`) and the row's gutter dot is drawn at the summary line's
  centre (`RailItem` checks for a `CommandCard`), so it sits on the arrow's
  line instead of 5 px above it. Since #1382 the line shows only the description, not
  the command after it: the command is on hover and in the expanded panel, and
  a call with no description (Codex) shows its command instead. The terminal panel
  (prompt line, **Run**, output) opens only when the line is clicked, because
  fully drawn panels took up the transcript and their five-row output boxes
  caught the mouse wheel. The output box now passes the wheel on to the
  transcript until it has been clicked, and transcript search still opens a
  folded command that holds a match. The approval mark sits at the right end of
  the line: a green ✓ for an allowed or approved call, an amber ? while it asks,
  a red ✕ when it was blocked or denied. Clicking it opens the permissions
  dialog. A pulsing dot means the command is still running, and a failed call
  shows `failed` or `exit N`.
  Since 2026-10-06 (#1449) the line has no gutter dot and no leading arrow: the
  first icon tile sits where the dot was (`RailItem` narrows the gutter for a
  `CommandCard` and indents its expanded body back to the content column), and
  the expand caret (˅ folded, ˄ open) is the last thing on the line, after the
  approval mark. Muted markers before the mark say how many lines the command
  printed and how long it took (`12 lines · 3.3s`; times under a second are
  left out, and a replayed run uses the events' `_ts` stamps). They are also in
  the line's accessible description.
  Edit, MultiEdit and Write rows (#1449) name the file by its base name as a
  `forkmesh-file:` link, followed by green `+N` and red `−M` line counts
  (`forkmesh::transcripttools::editStats`, from a line LCS in `lineDiff`). The
  diff shows only the changed lines, with `⋯` between separate hunks; the
  context lines the tool quoted to make its match unique are left out. A
  successful result ("has been updated successfully") draws nothing, since the
  row already says what changed; a failed one still shows its error.
  Since 2026-10-06 these rows fold like a Bash call (`FileEditCard`): the
  line is the tool, file link and `+N −M`, with the expand caret at its end,
  and the diff or written file opens only when the line is clicked. A Write
  of a new 60-line script used to fill the transcript with the whole file.
  Clicking the file name still opens the file, and transcript search opens a
  folded edit that holds a match. A Codex `FileChange` folds the same way: its
  line links up to three of the changed files (then "and N more") and
  `editStats` counts the `+`/`-` lines of its hunks, or every line of a file
  it added or deleted whole.
  The icons come from
  `forkmesh::transcripttools::commandIcons` (`src/ClaudeTranscriptTools.cpp`).
  It splits the command on pipes, `&&`, `;`, subshells, `$(…)` and backticks.
  It skips heredoc bodies, redirect targets, assignments, `case` patterns and
  wrappers (`sudo`, `env`, `timeout`, `xargs`, `nice`, …), reads into
  `bash -c` and `systemd-run`, and maps each program to an icon key. Programs
  that share a key (`grep`/`rg`, `sed`/`awk`) share one icon, and its hover
  names it (`commandIconLabel`, e.g. "Search text") and lists them.
  Every hover on the line (command, icon, approval mark) is the same solid
  `CommandHoverCard` the prompt line uses, managed by `HoverCardHost`. The
  line used `QToolTip`, which a live transcript hides on every re-layout
  Leave, so it blinked. The card stays up until a timer sees the pointer
  leave that zone. The command card is highlighted bash: a one-liner over 60
  characters is split at top-level `&&`, `||`, `|` and `;` with `\`
  continuations, and `do`/`then` bodies are indented (`prettyShellLine`;
  lines with `case` are left alone). Since #1506 each line of that card starts
  with the icon of its first program (`commandLineIconHtml`, the same tinted
  art as the line's tiles, as a data-URL image), and the card is a two-column
  table so a wrapped line hangs past the icon. A line that does not start a
  command (heredoc body, a `\` continuation, the rest of a multi-line string,
  a bare assignment) gets a blank slot so the text stays aligned. `chmod` now shows a lock and AI CLIs a
  robot. The table covers the programs found in ~30,000 earlier
  transcript commands: search, head/tail/read, text transforms, file
  operations, git and gh, build tools, tests, language toolchains, network,
  ssh, services, logs, processes, archives, packages and cloud CLIs. Anything
  else, including scripts, falls back to a terminal icon, and test binaries
  (`*-tests`) get the beaker. Line icons drawn for this live in
  `resources/icons/commands/` and are tinted per family. Octicons fill the
  remaining keys, and language/tool logos come from the bundled file-type set
  (`:/icons/tree/file_type_*.svg`, with an octicon fallback where that set is
  not linked).
  The chip is decided while the transcript is drawn, so its resolver
  (`installAgentCommandApprovals`) must not wait on git. Until 2026-09-28 a
  `Bash` call without a `cwd` asked `sessionWorkdir()`, whose
  `git worktree list` pumps a nested event loop. A session reload in that loop
  freed the `AgentSession` the resolver still held, and copying its `mode`
  crashed (SIGSEGV in `ClaudeTranscriptView::addCommand`, 2026-09-27 22:29).
  The resolver now copies the session's mode, repository and branch first, then
  takes the worktree from `cachedSessionWorktreeForUi()`, which never blocks
  and falls back to the repository checkout until the background lookup lands.
  The window test "a command's approval chip is decided without waiting on
  Git" (`forkmesh-window-tests --command-approval-only`) fails if the
  resolver reaches the git wait again.
- The prompt box (`src/PromptDraftStore.*`, `src/PromptSpellCheckHighlighter.*`,
  `src/AgentPromptImages.*`) keeps drafts, spell-checks, and takes images.
- The worktree checkbox in the composer's top strip (`promptWorktreeCheck`)
  has a small `git-branch` octicon right after it (`promptWorktreeIcon`,
  2026-10-07), so the bare 16 px box says what it switches. The icon is a
  `QLabel` re-tinted with the theme like other label octicons; clicking it
  toggles the checkbox and it shares the checkbox's tooltip.
- The composer's top strip has a small **cloud** toggle (`promptCloudButton`,
  right of the worktree dot; remembered in `agents/composerRunInCloud`). Off,
  **new** starts the run on this desktop. On, **new** queues it on one of the
  account's ForkMesh agent nodes (a headless mirror or the runner pool)
  through `startQuickAddCloudAgent` in `src/MainWindowAgentCloud.cpp`, and the
  run shows up as a cloud row. The repository is resolved the same way a
  local run's is (`resolveAgentRepositoryIndex`: the Issues selector if set,
  else the last opened repository, the open detail, or the Agents selector),
  so the repository chosen at the top left counts even when the Issues
  selector is empty; before this the cloud path read only the Issues selector
  and refused with "Pick a repository before sending a prompt to the cloud."
  Only Claude Code and Codex can go to the cloud,
  the repository must be published, and attached files are refused because
  they live on this desktop. The request is a key-signed
  `POST /api/repo/<owner>/<repo>/agent-sessions` (see
  [../agents/README.md](../agents/README.md#cloud-launch-from-the-desktop-composer)).
- Right of the cloud toggle sits a tiny **vibe** checkbox (`promptVibeCheck`,
  remembered in `agents/composerVibe`, off by default; since 2026-09-29). A
  local worktree run started with **new** while it is ticked is stamped
  `AgentSession::vibe` (stored as `"vibe": true`), so unticking it later does
  not change runs already going. When such a run finishes successfully,
  `maybeAutoMergeForSession` merges its branch into the default branch with
  `cleanUp=true` (the **Merge & clean** path: the worktree, branch and Agent
  record are deleted), and once the merge has landed it pushes the default
  branch with `pushCurrentRepoUpstream`, the same push as the Push button
  (secret scan, upstream and read-only-fork checks included). All of
  `mergeWorktreeIntoMain`'s gates still apply: when it refuses (uncommitted
  changes in the worktree, a conflict, a checkout it cannot merge into), the
  branch and worktree stay, nothing is pushed, and the Log says why. For a
  Claude Code or Codex stream run the finish hook skips
  `cleanupStreamWorktree`, which would otherwise race the merge by
  force-removing the worktree while it is being merged; the merge's own
  cleanup removes it. Vibe does nothing for cloud runs or runs with the
  worktree box unticked (no branch to merge). It revives the automatic merge
  of the old YOLO toggle (adhoc #12, removed in adhoc #120), which merged
  without cleaning up or pushing; sessions stamped `yolo` by those builds
  still merge that way.
- The phones' main prompt has the same two switches as **YOLO** and **No
  worktree** when the run goes to this desktop: `agents.start` takes `vibe`
  and `worktree` (see
  [../native/DESKTOP_AGENTS.md](../native/DESKTOP_AGENTS.md#yolo-and-no-worktree-on-the-main-prompt-2026-10-01)).
- Dragging the composer's top strip moves it around the window; dragging it
  out of the window pops it into its own window (branch 783, 2026-09-29).
  `promptDragLeavesWindow` fires once the cursor leaves the main window, or
  once the centre of where the panel would land is past the workspace edge
  (so a maximised window on one screen can still tear it off).
  `tearOffPromptOverlay` then calls `setPromptOverlayDetached(true)` with the
  window placed under the cursor and keeps the same press dragging it:
  `QWindow::startSystemMove()` first, a manual move when the platform refuses.
  On Wayland the compositor chooses where a new window appears and may refuse
  the move, so the window can land away from the cursor; dragging its strip
  again moves it. Double-click the strip or use the reset button to put it
  back.
- Notifications no longer float over the composer (issue 1867, 2026-10-09):
  the user asked for no toasts, so every message is an inline notice in the
  page's own layout above the page (see `docs/desktop/diagnostics.md`,
  "Inline notices instead of toasts"). The card stack that followed the
  composer, its light-theme fixes and its separate window beside a popped-out
  prompt are gone.
- The popped-out prompt window is frameless and translucent with no margin,
  so it is just the prompt: no title bar and no band of window around it
  (branch 790, 2026-09-29). Its strip still moves it and its dock button
  brings it back. The grip at its top-left corner resizes it with
  `QWindow::startSystemResize`, or by hand where the platform refuses.
- While the prompt has its own window, notices stay inline in the main
  window's page, and the prompt window keeps its size and place.
- A cloud row's detail header has **Continue here**, and, for a run that has
  ended, **Retry in cloud**; a run that failed because the runner's CLI is not
  signed in also gets **Sign in runner**. The same entries are in the row's
  right-click menu. Continue here claims a run no node started, and continues
  an ended one as a local agent seeded with its prompt and conversation, on a
  new branch cut from the run's fetched `agent/` branch (or from the base
  branch, with a notice, when that branch is missing or merged). That notice
  is a plain notice, not a red error: the run still starts, only from the base
  branch (the missing-branch case was red until 2026-10-04). On a run a
  node has started and is still going it offers "Stop the cloud run and
  continue here" (since 2026-09-30). See
  [../agents/README.md](../agents/README.md#continuing-retrying-and-unblocking-ended-cloud-runs).
- The Agents list counts uncommitted agent edits and refreshes them live.
- **Code** in the detail's button row opens the run's live diff (busiest-file
  chips under an "x of y files viewed" count that turns into a green check
  once every file is viewed as it stands, file list, per-file diffs with a
  Viewed pill) as a pane on the right
  of the transcript, in a splitter (`agentCodeSplit`), rather than in place of
  it: the edits and the conversation that made them are read side by side.
  Raw swaps only the left pane and leaves Code open. The pane polls the
  worktree every 3 s while a run is live and on screen. Since 2026-09-25;
  before that Code was a page of the output stack that replaced the
  transcript.
- The prompt's bottom bar shows only the usage meter of the provider picked
  in the composer: the Codex meter for Codex, the Claude Code meter for CC,
  and neither for other providers (issue 1664: both meters side by side read
  as noise when only one applies to the next prompt). Since 2026-10-07;
  before that both meters were always shown, Codex left of Claude Code.
- **Settings > Voice > Show the microphone in the prompt box** (setting
  `voice/inputEnabled`, on by default) hides the dictation mic next to the
  prompt box and the comment boxes when turned off, for people who do not
  want a microphone in the prompt area. `updateVoiceInputButton` hides the
  buttons and stops a capture in progress, and `startVoiceCapture` and the
  comment mics refuse to record while it is off, so the setting does not
  open the voice set-up page either. The World speech bridge has its own
  pairing and is not affected. Since 2026-10-08; before that the prompt
  mic was always shown.
- The provider usage meter's click menu (`showAgentAccountMenu`,
  `src/MainWindowChat.cpp`) lists each Claude Code or Codex account with its
  usage. Each account has a row of buttons: **Edit account…**, **Log in…**,
  **Log out…** and a terminal icon that opens the CLI in the system terminal
  as that account (its `CLAUDE_CONFIG_DIR` / `CODEX_HOME`), whether or not it
  is the active one. A Claude Code terminal opened this way runs
  `claude --dangerously-skip-permissions`, like the agents the app starts, so
  a hand-off session does not stop on permission prompts. Since 2026-09-25;
  before that the menu had one **Log out active account…** entry and the
  terminal could only be opened for the active account, in default
  permission mode.
- Each usage row in that menu (`TokenUsageMenuRow`) with a reset ahead of it
  marks its bar for pacing: the bar is cut into one segment per hour of the
  5-hour window and per day of a weekly window (2 px gaps on whole pixels, so
  a week always reads as seven pieces), and a short line above and below the
  bar where even usage would put you now. A bar that reaches past the line is
  ahead of pace. Until 2026-10-13 (#1982) the days were faint 1 px lines that
  blurred between pixels or hid under the fill's end and the pace line, so a
  week looked like five days; `docs/native/DESKTOP_AGENTS.md` has the
  details and the phone's copy of these marks. The
  second line adds how far, from the used percent minus the elapsed share of
  the window: "27% over pace", "12% under pace" or "on pace"
  (`usagePaceNote`). Codex windows take their length from
  `windowDurationMins`. The row's minimum width went from 340 to 380 px so a
  Codex line ("58% available · resets in 5d 23h · 27% over pace") is not cut
  off. Since 2026-10-13 (task 1967: so we can tell how far over a steady
  rate we are and slow down); before that the bar showed only the used share.
- The Agents action bar has one **Usage** button (`agentUsageButton`,
  `src/MainWindowAgents.cpp`) where it had a "Claude 1" and a "Codex 1"
  terminal button, each with its own usage bars beside it. Its glyph is a
  terminal screen with the usage bars of the default agent provider's active
  account drawn on it (Codex when Settings' default provider is Codex, Claude
  Code otherwise); the bars are read from the nav rail's gauges, which call
  `refreshAgentToolbarAccountButtons` through `TokenUsageMiniChart::onChanged`
  whenever their numbers move (the old copy-the-chart mirror is gone). The
  tooltip names the account and lists its windows. Clicking it opens
  `showAgentUsageMenu` (`src/MainWindowChat.cpp`): for each provider, the shown
  one first, the active account's usage rows, **Open … in a terminal** (the
  in-app terminal the old buttons opened), a **Run new agents as** submenu,
  **Manage … accounts…** (the full account menu above) and, for the other
  provider, **Use … for new agents**, which changes the default provider and so
  the bars on the button. The workspace ⋯ menu has **Usage and terminals…** for
  the same menu. Rail buttons gained `setIconPainter` so a button can draw its
  own glyph in both the classic and the Next shell paint paths. Since
  2026-10-13.
- Each Claude Code account is tied to one Claude login: the
  `accountUuid`/`organizationUuid` pair in the account's `.claude.json`
  `oauthAccount` (`~/.claude.json` for the default account when ForkMesh has
  no `CLAUDE_CONFIG_DIR`). The pair is saved under
  `agents/accountIdentity/claude/<account id>` the first time the account is
  seen signed in. This keeps the sessions of two Claude logins out of one
  folder (issue 1446: logging in to another Claude account inside an existing
  account mixed their sessions). Since 2026-10-06:
  - **Log in…** and **Add account…** run `claude auth login` in a fresh folder
    under `<data>/agent-accounts/claude-login/` rather than in the account's
    own folder. When the login lands (a `QFileSystemWatcher` on that folder,
    or the next time the menu opens or a Claude agent starts), it is filed by
    who signed in. If an account is already tied to that login, its
    `.credentials.json` and `oauthAccount` move there and that account is
    selected. If the account you clicked has never been signed in, it takes the
    login. Otherwise the login becomes a new account, labelled with the email
    (or with the label typed in **Add account…**). Logging in to the same Claude
    account again therefore always returns to its own account and sessions.
    Folders left behind by logins that were never finished are filed or removed
    the next time a login starts.
  - `reconcileClaudeAccountLogins` runs when the menu opens and before each
    Claude agent starts. If a login made outside ForkMesh (`/login` in a
    terminal, a phone login terminal) left an account folder signed in as a
    different Claude login than the one it is tied to, the login is moved out
    to the account tied to it (a new account if there is none). The folder
    keeps its own sessions and shows as signed out, and if it was the selected
    account, the account the login moved to is selected instead.
  - Login and identity files move by copy then remove, never by deleting
    folders. The routing and file moves are in
    `src/ClaudeAccountRouting.cpp` and are tested by
    `tests/test_claude_account_routing.cpp`. On macOS the CLI may keep the
    token in the Keychain. Then only a new Claude login can be filed, by
    keeping its sign-in folder as the new account's folder. A login that
    matches an existing account just selects that account.
- The same menu ends with a **Runner accounts** submenu (2026-10-01) that
  switches the account a runner uses, the way the rows above switch this
  computer's. It lists each active runner that has the provider's CLI
  installed (from `GET /api/runners`, the Network → Hosts Runners list) with
  its saved accounts: the active one checked, each with its plan and a usage
  line, and **switching…** while a change is pending. Choosing another
  account posts `activate` through `requestRunnerAccountChange`, the Runners
  table's own call, and **Add … account on <runner>…** opens the runner
  sign-in dialog, which adds an account. Opening the menu re-reads the
  runner list; the submenu is rebuilt when it is shown and whenever that list
  arrives (`refreshAgentRunnerAccountMenus`), and the outcome of a switch is
  flashed when the Hosts status line is not on screen. Remove stays on the
  Runners table. The rows are built by `fillRunnerAccountsMenu`
  (`src/RunnerListView.cpp`), tested in `tests/test_runner_login.cpp`.

## Workspace layout in the new interface (2026-10-04, restyled 2026-10-05)

In the new interface ("New (preview)", `app/design` = `next`) the Agents page
is laid out like a coding-agent workspace: a session list on the left, the
conversation in the middle, and one side panel on the right for the live
diff, a shell and a browser. Classic is unchanged. The code is
`src/MainWindowWorkspace.cpp`; the styles are in
`resources/theme/next/61-workspace.qss`.

The look follows a reference the owner supplied on 2026-10-05, a screenshot
of the "ForkMesh · Qt Quick demo" window. It is rebuilt here in Qt Widgets;
the app still has no QML ([ui-redesign.md](ui-redesign.md), "Why not QML").

**Two layouts.** The new interface offers the Agents page in two layouts,
**New** (the default) and **Classic**, the page as it was before this work:
the action bar, the three list views, the detail buttons, the Code pane
beside the transcript, prose in the monospace font and the three-button
composer. The switch is under "Agents layout" in the View menu at the top
right of the window, which is there in both layouts, and under **Layout** in
the session list's menu. The choice is the setting `agents/workspaceLayout`
(`new` or `classic`), read through `src/WorkspaceLayout.h`;
`workspaceLayoutNew()` is true only in the new interface with the New
layout, and everything described below is keyed on it. The Classic interface
(`app/design` = `classic`) has only the earlier page and does not show the
switch. Switching redraws the open transcript, because its prose font
differs between the two.

What it replaces, and why:

- The list's action bar held fourteen icon-over-caption buttons at a fixed
  size. The list pane's minimum width (260 px) is narrower than the bar, so
  in Workspace view, where three panes share the window, the buttons were
  drawn on top of each other.
- The detail pane spent two rows on a status pill and up to fourteen more
  buttons, then two more lines on the prompt as a title.
- The live diff opened inside the detail pane and the shell in a third pane,
  so with both open the window had four columns.
- The composer had a strip of 16 px controls above the text and three
  stacked send buttons (task, add, new) beside it.

The areas:

| Area | What it shows |
| --- | --- |
| Session list header (`workspaceSidebarHeader`) | A blue wash with small star dots holding a letter badge and the repository name, **New session** (+), a search field, and "Sessions" with the count, Sort and a menu with every bulk action |
| Session rows | One card per session, three lines: a badge in the status colour with the provider's initial, "provider · status" and the age; the title; where it runs and its line counts. The selected card is filled and outlined |
| Session list footer (`workspaceSidebarFooter`) | "1 of 5 agents running", which opens the limit menu |
| Conversation header (`workspaceHeader`) | "repository / title", a status chip (opens the session details), **Stop** or **Continue** when they apply, **Show diff** / **Hide diff**, Terminal and Browser toggles, and a menu with every other session action |
| Changed-files card (`workspaceChangesBar`) | "2 changed files +6 −3" with the branch and **Open diff**, above the composer, for a session whose branch has edits; folded away while the diff is open |
| Composer | A rounded box 118 px tall with the placeholder "Ask for changes, send follow-ups", the attach, voice, command and model controls along the bottom, an options menu and one round send button |
| Side panel (`workspacePanelBar`, `workspacePanelStack`) | **Diff**, **Terminal**, **Browser** as pills, the branch and its totals on the Diff tab, a new-shell button on the Terminal tab, close |

The transcript is a centred column at most 780 px wide and the composer is
centred under it at the same width. With no session selected the
conversation shows "What should the agent work on?", the placeholder reads
"Describe a task to start a new session" and the composer starts a new run;
**New session** clears the selection to get there.

Details that are not obvious from the picture:

- **Search** in the list header is the top bar's search field under another
  name: text typed in either appears in both, so there is one filter.
- **The round send button** presses whichever of the original buttons Enter
  would (`enterTarget`): **add** when a session is on screen, **new**
  otherwise. Starting a new session while one is open, adding the prompt to
  the task list, the worktree, cloud and vibe switches, Clear and Pop out
  are in the options menu beside it.
- **The composer changes only while it is docked under a conversation in the
  New layout.** Floating, popped out, or on any other page it is the
  composer as before, drag strip and three buttons included.
- **The status chip is dropped** when the conversation header is narrower
  than 540 px, so the buttons never overlap; the row's badge still shows the
  status.
- **The blue header is a gradient**, which [ui-redesign.md](ui-redesign.md)
  rules out elsewhere ("No gradient fills"). It is here because the owner's
  reference has it, and only here. In the light scheme it is a pale accent
  wash with no stars.

Inside the transcript (`src/ClaudeTranscriptView.cpp`, new interface only)
prose is set in the interface font at 15 px, not the monospace one;
commands, diffs and tool rows keep the monospace font. Portraits and
captions stay. The transcript dropped its prose bubbles for both layouts on
2026-10-06, so this layout no longer changes them.

Every piece of agent prose has a copy button (`proseCopy`) at
the right end of its caption: each finished or streaming reply, and the
Summary of a subagent's report. It copies the agent's own Markdown, not the
rendered label, so file and task references come out as written rather than
as `forkmesh-file:` links. The Markdown sits on the label as the
`proseMarkdown` property, so a streaming reply copies everything received so
far, even before the 60 ms repaint has drawn it. A subagent Summary copies the
full report, not the shortened summary. The icon turns into a check for 1.5 s
after a copy. The user's own turns have no button.

The live diff in the side panel is drawn unified while the pane is narrower
than 700 px, whatever the split-view preference says; side by side does not
fit there (`renderAgentDiff`). Its file list is hidden in the panel; the
file chips above the diff and the file headers inside it do that job. The
diff is also rendered again when the colour scheme or the interface
changes. Before, a diff drawn in dark stayed dark after a switch to light.

How it is built:

- **Nothing is re-parented or renamed.** Rail shortcuts and remembered page
  buttons address a button by object name or by class and position among its
  siblings, so every existing button stays where it was. The list's action
  bar is hidden as a whole. The detail pane's action row and the composer's
  send column are layouts, not widgets, so each is taken out of its parent
  layout and its buttons are moved off-screen; they keep their parent, their
  order and their state.
- **The new controls are proxies.** A menu entry or header button reads the
  original button's caption, enabled state and checked state when it is
  shown, and calls `click()` on it. No action is implemented twice, and
  whatever shows or hides a button (`showAgentSession`,
  `updateAgentActionState`) decides what the menus offer.
- **Card rows are drawn by a hook in the list's own delegate.**
  `AgentRowActionsDelegate::paint` (`MainWindowAgentsInternal.h`) first calls
  `forkmesh::ui::paintWorkspaceSessionRow`, which draws the card when the
  table carries the `workspaceCards` property and otherwise returns false.
  The delegate then paints its hover controls (run, move, delete) on top,
  clipped to the card, so they work as before. The card reads the words the
  Full list view already puts on each row (`kAgentRowChipsRole`), so the
  list is kept in Full view underneath while the new interface is on, with
  74 px rows; `agents/listView` is put back so Classic keeps its own choice.
- **One side panel.** The third pane of `agentWorkspaceSplit`
  (`workspaceTerminalDock`) holds a stack: the shell, the Code page, the
  browser. In the new interface the Code page (`agentCodePage`) is moved from
  `agentCodeSplit` into the panel. Code mode and the Diff tab are kept in
  step in both directions, so the existing **Code** button, its shortcut and
  `setAgentCodeMode` still work. Switching back to Classic moves the page
  back.
- **The composer's bottom row.** The scrolling bar of attach, voice and
  model controls now sits in a row with the options and send buttons beside
  it, outside the scroll area, so a narrow composer clips the model
  selectors and never the send button. In Classic the two buttons are hidden
  and the row holds only the bar.
- **Plain `QWidget`s paint the page colour.** The base sheet's `QWidget`
  rule gives every plain `QWidget` a styled background, so the unnamed host
  around the prompt text covered the composer's rounded corners. The docked
  composer makes its direct children transparent
  (`#promptWrapper[workspaceDocked="true"] > QWidget`), and labels placed on
  the blue header or on a card are transparent for the same reason.
- **Workspace view** opens the panel on Terminal. In Standard view the panel
  is closed until a toggle opens it. Closing the panel no longer leaves
  Workspace view. The panel takes 42% of the width when it first opens.
- **Refresh.** An event filter on the proxied buttons (shown, hidden, enabled)
  and the list's selection schedule one coalesced refresh; a one-second timer
  covers the status chip while the header is on screen.
- **Hooks.** `NextAccess::installWorkspace` runs at the end of
  `buildAgentsTab`, `syncWorkspace` from `applyAppViewMode` and after a theme
  change, and `placeWorkspaceComposer` from `positionGlobalFooterOverlays`.
  `MainWindow.h` was not edited.

**Browser tab.** `src/WorkspaceBrowserPane.*` is its own static library,
`forkmesh-browser-pane`, so linking it changes no compile flags of the app.
By default it has an address bar and opens the page in the system browser. It
embeds Qt WebEngine only when the build is configured with
`-DFORKMESH_BROWSER_PANE=ON` and Qt WebEngine Widgets is installed, because
the three release pipelines do not carry WebEngine (the Linux build is a
static Qt, which cannot), and a web view switches the whole window to GPU
composition, the same trap documented for Watch. The view is created only
when the tab is first opened, and never on the `offscreen` or `minimal`
platforms. A bare port such as `3000` opens `http://localhost:3000`; only
`http` and `https` load.

Not done yet: tool rows, command cards and the diff's own file headers keep
their existing look, so the Diff tab is not yet the reference's plain
path-and-totals header with a coloured bar down each changed line; a file's
header row is wider than a narrow panel and scrolls sideways; the diff does
not switch between unified and split when the panel is dragged across
700 px until its next refresh; the file chips above the diff keep their
colours after a live switch of colour scheme until the next refresh.

Tests: `forkmesh-window-tests --workspace-snapshots-only`
(`tests/workspace_snapshots.cpp`) builds a fixture repository with six
sessions, a transcript and a branch with edits, checks the layout in Classic
and in the new interface, and the switch to the Classic layout and back (50
checks), and writes `workspace-*.png` and
`agents-standard.png` beside the other interface snapshots. Its `settle`
also delivers deferred deletes, which `processEvents` alone does not:
without that, transcript rows freed by a re-render stay on screen in the
test window and show through as ghosts. `./run.sh` configures the build
folder with `-DFORKMESH_BUILD_TESTS=OFF`, which removes the test targets;
`cmake -S desktop -B desktop/build -DFORKMESH_BUILD_TESTS=ON` brings them
back without a full rebuild.

Checked on 2026-10-05, on macOS with Qt 6.11.1 only:

- `forkmesh` and `forkmesh-window-tests` build.
- `--workspace-snapshots-only`: all 50 checks pass; images looked at in
  dark and light, at 1440 by 900 and 1100 by 720, and in both layouts.
- `--view-modes-only`, `--agent-efficiency-only`, `--agent-list-view-only`,
  `--agent-code-only`, `--agent-task-link-only`, `--command-approval-only`
  and `--inline-choices-only` pass apart from the Linux-only systemd check
  that fails in every suite on macOS.
- `--diff-stream-only` (9 failures), `--footer-overlay-only` (7) and
  `--agent-landing-only` (2) fail the same checks as they did on 2026-10-04
  before the restyle. `--footer-overlay-only` and `--agent-landing-only`
  were never compared with a build from before the workspace work.
- `--ui-snapshots-only` passes apart from the systemd check; the Agents
  page measures 665 px wide at its narrowest in the new interface.
- The Python tests that read the changed sources (34 files in `app/tests`
  and `world/tests`, installer tests left out): 638 pass and 18 fail, and
  the same 18 fail on an export of the commit before this work.
- `desktop/tests/validate_*.py`: the same four fail with and without this
  work.

Not checked: the real application. Everything visual comes from the
offscreen test window. A real shell in the Terminal tab, the embedded
browser, dragging the splitters, the menus, sending a prompt with the new
send button, and a session that is actually running (which shows **Stop**)
have not been tried. Linux, Windows and Qt 6.4 have not been built.

### Attaching an image to the prompt (2026-10-13)

Attaching an image or video (file picker, paste or drop) no longer posts the
"Image attached (n) — it'll go with your next send." notice. The new chip is
the feedback instead: `queueQuickAddImage` animates the last chip in
`quickAddAttachStrip` for 760 ms. The chip opens from zero width and fades in
over the first 40%, and its border and fill flare to the accent colour and
cool back to the resting chip colours. When the animation ends the chip's own
sheet, opacity effect and width cap are removed, so it matches the strip's
rules again. Removing an attachment still posts its notice.

## Tool efficiency: Runs view (2026-10-12)

The operator asked for a log of every agent run with its stats and how
efficient it was, to compare runs with and without tool families switched off.
The Tool efficiency page has a third view, **Runs**, beside Families and Tools
(`src/AgentRunTable.*`), fed by `AgentRunEfficiencyLog`
(`src/AgentRunEfficiencyLog.*`).

- A run is one prompt to its `result` event. It opens on the first event of
  the session after the previous result (`system` init, `_local_user`,
  `assistant`, `user` or `_codex_usage`); a run cut off without a result closes
  as `no result` when the next `system` init arrives.
- Each run records: session, title (the task title, else the prompt's first
  line), provider, model, the tool families switched off **when it launched**
  (`MainWindow::noteAgentRunToolConfig`, from the launch options, so the global
  allowlist and Codex's web-search rule are included; switching a family off
  only reaches a running agent at its next launch), outcome, wall time, model
  calls, input / cached input / cache-write / output tokens, tool calls, tool
  errors, tool argument and result tokens, saved tokens, improvement signals and
  the tokens they put at stake, per-family call counts and an estimated cost at
  `AgentTokenPricing` list prices (cache writes at 1.25×, cached input at 0.1×).
- Claude Code tokens are summed per assistant message id, keeping the largest
  count seen for each id (stream-json repeats usage per content block). Codex
  reports a running thread total, so a run counts from the total at the end of
  the previous run in this desktop session, or from the total before its first
  response (`total − last`).
- Efficiency is the share of tool tokens the improvement log did not flag:
  `1 − tokens at stake / (tool argument + result tokens)`.
- Every finished run is appended to `agent-runs.jsonl` beside
  `tool-efficiency.json` (the last 2000 are kept; the file is rewritten when it
  passes 4 MB). **Copy comparison** puts the comparison as a markdown table on
  the clipboard; **Open run log** opens the JSONL. **Clear stats** clears it
  with the ledger and the improvement log.
- The top table groups runs by runtime and tool configuration, all-tools-on
  first, with per-run averages and a **vs all on** column: context per run
  against the same runtime with every tool on. Runs differ in what they were
  asked, so a group needs several runs before the difference means anything.
  The lower table lists the newest 500 runs.

## Models board

**Models** in the Agents action bar opens a sortable table
(`src/AgentModelTable.*`) with one row per model: every entry of
`composerModelCatalog()` (the model picker's line-up, with the same icons),
plus any model that appears only in sessions, retired outcomes, the tool
ledger or git history. The page is built in `src/MainWindowAgentModels.cpp`;
the numbers come from `AgentModelStats::build` (`src/AgentModelStats.*`), which
has no widgets and is tested by `forkmesh-agent-model-stats-tests`.

## Icon file size (2026-10-13)

The icons stay PNG: Qt reads WebP only through the optional qtimageformats
plugin, which the Windows package does not ship (`tools/package/windows.sh`)
and many Linux desktops lack, and a missing decoder would leave a blank
avatar. To shrink them (2.9 MB to 0.69 MB) the teammate portraits were
palette-quantized to 256 colours with Floyd-Steinberg dithering at 256 px,
because portraits are drawn up to 84 px (the teammate home) and cropped to
62% for faces, so 256 px still covers a 2x display. The `mode-*` and
`effort-*` icons, drawn at 26 to 32 px, were resized to 128 px and quantized.
The `model-*-face.png` files were already palette PNGs and are unchanged.

## Model and teammate portraits (2026-09-28)

Every model and teammate icon in `resources/agent-ui/icons/` is a robot face:
a 256×256 square holding a gold-ringed circle with the name across the bottom,
the same framing as `model-fable-face.png`. The abstract marks (starburst,
feather, mountain blossom, book and quill, sun, crescent moon, globe and leaf)
were removed. The rows that used them (Claude API and other Claude ids without
a portrait, Codex and OpenAI API, mini/nano models, Cloudflare and anything
else) now take the face of the model the mark stood for: Sonnet, Sol, Luna and
Terra (`agentModelFaceIconIndex`). This reverses adhoc #1545, which kept marks
below the top models; `test_window_resize` now expects a portrait on every
model row.

The teammate portraits (`teammate-*.png`) were 512 and 1254 px square photos.
They are now the same badge, cropped to head and shoulders and labelled with
the teammate's name. There is one portrait per teammate: the second CFO and
first Quality pictures and the `cfo-classic` / `quality-classic` avatar keys
were dropped, `cfo` uses the office portrait and `quality` the violet one.

Teammates that had borrowed a model face (CEO, Sage, COO, CPA, Fixer, Tempo,
Reaper, Sales, YC News) now have their own avatar key and portrait, listed in
`teammateAvatarPixmap` and the teammate editor's avatar picker. No image model
was available when they were made, so they are recoloured and mirrored
versions of the existing robot art: CEO is the retired second CFO picture,
Reaper the retired first Quality picture in scarlet, and the rest are hue
shifts of the Steward, Mobile and Quality robots, one distinct colour each.
Generated artwork can replace any of them file for file.

Sentinel, the security teammate added on 2026-09-30
([../security/sentinel.md](../security/sentinel.md)), has the `sentinel`
avatar key and `teammate-sentinel.png` (cobalt). It is the first portrait made
from generated art: `codex exec` with image generation drew the robot from the
Steward, Reaper and Marketing portraits as style references, and the 256 px
badge reuses Reaper's gold ring with the name set in DejaVu Serif Bold in the
same gold.

Sources, per row:

- Runs, live/OK/fail/stop counts, pull requests, tokens, turns and working time
  come from `m_agentSessions`. OK counts `success` or merged; stop counts
  `stopped` and `cleared`; everything else not terminal is live. Rates are over
  finished runs, so live runs do not drag a model's success rate down.
- Runs and merges of deleted agents come from `model-outcomes.json`
  (`AgentStore::retiredModelOutcomes`), the file the model picker already reads
  for its "N merged" note. Those runs have no tokens or cost, so per-run
  averages ($/run, turns, tool calls per run) divide by stored runs only.
- Cost is `costUsd` when the runtime reported one, else prompt and completion
  tokens at `agentTokenPriceFor` list prices. A cell made only of estimates is
  prefixed `~`; the model cell's tooltip splits reported from estimated.
- Tool calls, tool error rate and tokens per tool call come from the tool
  ledger's new per-model counters (`AgentToolEfficiencyLedger::modelUsage`,
  persisted under `models` in the ledger JSON, no schema bump). The ledger only
  started keeping them on 2026-09-27, so older tool calls are not attributed.
- **Git** and **Commits** are backfilled from commit messages. The first time
  the page opens (and after ten minutes, or on **Rescan git**) a worker runs one
  `git log HEAD` per local repository (`localPath`, else `mirrorPath`, at most
  40) with the `Co-Authored-By` and `ForkMesh-Agent` trailers.
  `AgentModelStats::parseGitHistory` walks the first-parent chain from HEAD; for
  each merge it walks the merged side's first parents until it reaches the
  mainline or a commit an earlier merge already claimed, tallies the model
  trailers of those commits and credits the merge to the most frequent one.
  Merges met on such a walk are credited the same way in turn, so history
  that came in through one merge (the Next import of 2026-09-13, about 5,000
  first-parent commits) still has its own merges counted; before that was in
  place Opus 4.8 showed 16 merges for 1,564 commits. A merge of main into a
  branch walks nothing and credits nobody. A non-merge commit on the mainline
  that names a model counts as a merge too (agent work landed directly). A `ForkMesh-Agent: provider/model` trailer
  names the exact row; when a commit has one its `Co-Authored-By` lines are
  ignored so the commit is not counted twice. A `Co-Authored-By` trailer counts
  only when its address is at anthropic.com, openai.com or cursor.com; the name
  loses a leading "Claude " and any parenthetical ("Claude Opus 5 (1M context)"
  becomes "Opus 5") and is matched to a catalog label, else it gets its own
  row. Clones that share a root commit are read once (the longest log wins).

Traps: the git columns and the desktop's **Merged** column count different
things (history on disk versus runs this desktop merged) and are shown side by
side on purpose. The picker's "N merged" is still the desktop count.

## Watch (Market package `agent-watch`)

Since 2026-09-24 Watch is a bundled Market package, installed by default, and
no longer part of the core app: the "Watch" button left the Agents action bar
and the field is a page in the left rail (`nativePackageNav-agent-watch`).
Removing the package from the Market removes the page; the core keeps working
without it. Code and details:
[../../marketplace/packages/agent-watch/README.md](../../marketplace/packages/agent-watch/README.md).

The field draws each agent as a body circled by its project and task, the
desktop node at the centre, the mirrors it uses, network connections, and the
relay room (web visitors, relay clients, WebSocket frames, visitors' flags,
agents and threats). On the rail page it paints in software; **Full screen**
moves it into its own window that paints on the GPU, because a QOpenGLWidget
anywhere in the main window would switch the whole window to OpenGL
composition. There is no text on the field except on hover. Each agent's last
reply is shown on an LED display; **Speak** reads replies aloud.

Since 2026-09-27 the marks round each agent are the small icons the list of
sounds draws for each kind (a magnifier for search, a bolt for a command, a
branch for git), not coloured dots, so a mark can be matched to its row by
shape as well as colour. The same icons replace the dot in a log line's spark
and in a running subagent's or desktop task's moon. The ring holds as many
marks as fit side by side at its radius, at most 40, so a small body shows
fewer. Each icon is painted once per kind, size, pixel ratio and theme and
cached (`AgentActivityView::markIcon`), so a busy field costs a blit per mark.

The places on the field are icons too, not stars and diamonds: this desktop and
other desktops in the relay room are a screen, a mirror is a server, and each
host along the top is drawn for what it is (`AgentActivityView::hostPlace`,
matched on its domain): a sparkle for an AI API, a branch for a git host, a box
for a package registry, a cloud for a cloud or CDN provider, a mesh for
ForkMesh, a globe for anything else, and a screen for this machine. Its hover
card names the icon ("a cloud: a cloud or CDN provider").

The phone Watch (native Android and iOS) draws the same ring icons
and the same screen for this desktop; it has no hosts along the top.

The core side is the neutral feed `src/AgentActivityFeed.*`, hosted by
`src/MainWindowAgentFeed.cpp`. The core publishes (`publishAgentEvent` from
`applyTranscriptEvent` and `feedAgentActivity`, `publishNote` from the
sonifier's note observer, log lines, background tasks, relay roster and
frames, Worker tail events, the Speak switch) and answers `Host` calls
(agents, process roots, Listen, log styles, Cloudflare API). The core has no
package identifiers: a package subscribes as an `agentfeed::Listener`.
`agentfeed::setWatching` tells the core someone is looking, so external
Claude Code transcripts are tailed while Watch shows even if nobody listens.
The same flag keeps web-launched cloud runs moving (PR #292): the account event
socket only rings when a mirror claims, starts or finishes a run, so the turns
in between arrive only on a read. A one-second timer started by
`installAgentActivityHost()` (`MainWindowAgentFeed.cpp`) calls
`refreshCloudAgentSessions()` while `agentfeed::watched()` is true, and with a
run in flight the read floor drops from `kCloudAgentActiveRefreshMs` (2 min)
to `kCloudAgentWatchRefreshMs` (5 s). With Watch closed, or nothing running,
the cadence is unchanged. `tests/validate_cloud_agent_watch_contract.py` pins
this.

Since 2026-09-27 Watch shows the web agents (runs queued from the website or a
phone and worked on a mirror) even when no repository's Agents tab has been
opened. Before, the timer lived in `buildAgentsTab()` and
`refreshCloudAgentSessions()` returned while `m_agentTable` was null; the
Agents tab is built only when first opened, so Watch opened from the rail
never read the account's cloud runs and drew only this machine's agents. The
read now also runs with no table while Watch is watched; the rows land in
`m_agentSessions` as before and reach the field through `Host::agents`.

A web run looks like a local one in the field (its face comes from
`provider|model`), so `installAgentActivityHost()` says where it runs in the
task line under the body: `<title>, web run on <node>`, or
`<title>, web run queued for <node>` before a node starts it. This uses the
existing `Agent::task` field, the way machine agents say "started outside
ForkMesh", so the agent-watch package needs no change.

Watch shows every agent running on the machine, not only the ones ForkMesh
started or the external Claude Code rows the Agents tab surfaces (those
need the open repository and the "Exclude external Claude Code agents"
setting off, which is on by default). Since 2026-09-25, while Watch shows (or
one of these agents is being listened to), `tickMachineAgents()` walks `/proc`
every 2 s (`src/MachineAgentScan.*`) for agent CLIs: Claude Code, Codex,
Gemini CLI, opencode, Aider, Goose, Amp, Crush, Cursor Agent and
Copilot CLI, by process name or by the script a node/bun/deno/python process
runs. An agent of another login is listed as "Claude Code, running in b's
login" and nothing is read from it (`MachineAgentScan::processUid`,
`MachineAgent::otherUser`); see
[native/DESKTOP_AGENTS.md](../native/DESKTOP_AGENTS.md#agents-of-other-users-on-the-machine).
A process under the desktop or under a ForkMesh run's own process is
ForkMesh's; an agent started by another agent is folded into it; one that
matches a surfaced external row is left to that row. The rest are handed to
the package through `Host::agents`, `Host::agent` and `Host::processRoots`,
with ids from the external band (`externalTempIdFor("pid:<pid>")`), a title
from their folder and a face from `--model`, so their processes and network
flows show like any run. They are never written to `m_agentSessions` or the
store, so the Agents tab and its setting are unchanged. Since 2026-09-28 the scan also runs while a
phone or browser is connected to local control, and `agents.list` hands these
agents (and the live external Claude Code rows) to it as `outside`, so the
phone's Agents tab shows them too
([native/DESKTOP_AGENTS.md](../native/DESKTOP_AGENTS.md#what-is-running-on-the-desktop-on-the-agents-tab)). Since 2026-09-29 a phone can also follow and watch them:
`agents.transcript` answers their Claude Code or Codex transcript, and the
scan tick rings `agents.changed` when a followed one grows
([native/DESKTOP_AGENTS.md](../native/DESKTOP_AGENTS.md#following-and-watching-an-agent-started-outside-forkmesh)).
A Codex process's transcript is the rollout file it holds open; it is only
served to phones, and `externalActivityTranscript` still hands only Claude
Code transcripts to Watch and Listen here. Since 2026-09-30 a phone can also
steer an outside Claude Code: the same tick reads every live Claude Code
session record (`<config>/sessions/<pid>.json`, `src/ClaudePeerMessaging.*`)
into `m_claudePeerSessions`, the `outside` rows carry `steer`, and
`agents.prompt` on such a row is delivered through the session's own
cross-session inbox
([native/DESKTOP_AGENTS.md](../native/DESKTOP_AGENTS.md#steering-a-claude-code-started-outside-forkmesh)).
ForkMesh claims no permission mode, so a session that bypasses permission
prompts holds a steer for approval in its terminal unless its user set
`"crossSessionInbound": "accept"` in that session's `<config dir>/settings.json`;
the phone is told so (`held_for_approval`). Each message is written to the
desktop log with the phone that sent it. The
desktop's own Stop on an external row still ends the process after asking;
a phone cannot stop one.

A Claude Code process's transcript is found in its own config folder
(`CLAUDE_CONFIG_DIR` from `/proc/<pid>/environ`, else `$HOME/.claude`), under
`projects/<cwd with every non-alphanumeric character as ->`: the file named by
`--resume`/`--session-id`, else the newest file no other process claimed. It
is skipped for a folder a ForkMesh run is streaming in, where the newest file
could be the run's. The transcript is tailed from its current end by
`pumpExternalAgentActivity()`, which the scan tick now also calls, so surfaced
external rows animate while Watch shows too (before, only the Listen poll
tailed them). Other CLIs' transcripts are not read; they show as bodies with
their processes and traffic. The Agents tab's own scan still reads only
`~/.claude/projects`, so a terminal session using an account's
`CLAUDE_CONFIG_DIR` shows only in Watch.
**Hide output** (since 2026-09-27, `agents/activityLogsHideOutput`, off by
default, enabled only while **Logs** is on) keeps forwarded process output out
of the Log column: core and runner journal lines (`Node n2 core <unit> <word>:`,
`corelogs::nodeLogLine`) and mirror host lines (`Mirror <node> <source>:`,
`hosts::mainLogLine`). Those streams fill the column within seconds and push
the desktop's own lines out of sight. `Scene::outputLine` matches the two
prefixes and `LogLine::output` records it; the view skips the lines when it
paints the column, so they still spark, sound, light their mirror's seat and
stay on the Logs page, and turning the button off brings them back at once.
The desktop's own "Mirror: synced ..." and "Node connected: ..." lines do not
match and always show.

**Connection traffic** (since 2026-09-27): clicking a host along the top of
the field, or the line between an agent (or the desktop's node) and a host,
opens a traffic window (`AgentTrafficWindow.{h,cpp}`), in the style of Little
Snitch's network monitor. The top list has one row per connection, keyed by
session, process, address and port: the process holding the socket, the
agent, the remote name and address, the port and its usual service, the bytes
each way and the rate over the last `Scene::kConnectionRateSeconds`. Below it
is the live feed, newest first: one row for each direction of each sample
(every 400 ms), with the time, the process, the remote end and the bytes.
Selecting a connection narrows the feed to it; the filter box matches any
column in both lists; Pause holds both still and Resume catches up from the
scene's log; Clear empties the feed. A host click shows every agent talking
to the host (`Scene::kAnySession`); a line click shows that one agent.

- The sampler records which pid holds each socket inode (`Sample::holder`)
  and its name (`Sample::names`, from the process table, or
  `/proc/<pid>/comm` for the desktop's own pid). The ledger keys deltas by
  pid as well, so two processes talking to one address and port show as two
  connections.
- `Scene::network` also writes each delta to a log (`Scene::traffic`,
  `kMaxTraffic` records, trimmed in chunks) with a running serial, so each
  window asks only for records after the last one it has. `Scene::connections`
  sums the log into the rows of the top list.
- A reverse lookup or a mirror seat renames a host after its first bytes.
  `renameHost` rewrites the log and remembers the rename (`hostNamed`), so a
  window opened under an address follows the host to its name.
- An open traffic window keeps Watch sampling (`updateWatching`) even when
  the page is hidden. The windows are top-level, so they also show over the
  full-screen field, and the feature deletes them before its scene.
- `formatBytes` and `formatRate` moved from the view into the model so the
  window and the hover card format bytes the same way.

The package owns the kind mutes (`agents/activityMutedKinds`) and pushes them
to the sonifier through `Host::setMuted`, because the kind labels live in its
style table.

Traps: the body's title is `AgentSession::name` (the repository), which also
seeds colour and voice; the task goes in `Body::task`. External and cloud rows
never pass through `applyTranscriptEvent`: external ones are tailed, cloud ones
are fed from the cloud session reader.

Ids on the field: agents use their list id (positive for stored runs, -1000
and down for external Claude Code sessions and machine-wide agents, -1000000
and down for cloud rows),
the desktop is -2, and each seated mirror gets a source id from -100 down to
-899 (`Scene::kMirrorFirst`, `kMirrorSources`). The view looks up a mirror
before a body, so the mirror band must never overlap an agent band. Until
2026-09-24 it started at -1000, so with mirrors seated the first external
session's marks were drawn on a mirror's seat and its body never moved.

## Listen, Announcer and interface sounds

- **Listen** (`src/AgentSonifier.*`): plays a run's stream events as music
  and rings a cadence at the end. Voice parameters are hashed from session id
  and title. Setting `agents/listenToNewRuns` listens to new runs
  automatically. Backends: Qt Multimedia (`QAudioSink`) when built with it,
  otherwise a raw PCM pipe to `pw-cat --raw` or `aplay`.
  `FORKMESH_SONIFY_BACKEND=silent|pipe|none` forces one (tests use `silent`).
  Rendering runs on the player's own thread.
- **Announcer** (`src/AgentAnnouncer.*`, `src/AgentAnnouncerVoice.*`):
  sportscaster commentary on all running agents, spoken by the Piper neural TTS,
  downloaded on demand with pinned SHA-256 into the local data root
  (`announcer/`). Commentary is pulled only when the speaker is idle, so stale
  lines are dropped rather than read late.
- **Interface sounds** (`src/UiSounds.*`, cue table `src/UiSoundCues.cpp`):
  an app-wide event filter installed while setting `ui/interfaceSounds` is on.
  On by default since 2026-09-22 except on the `offscreen`/`minimal` platforms
  and in Qt test mode, so tests never reach the speakers. The prompt box has a
  typing sound when interface sounds are on. The phone apps play the same cue
  table (`docs/native/NAVIGATION.md` §6.5); change a cue here and in
  `InterfaceCues.swift` and `SoundCues.kt` together.
- **Chat is silent** (2026-10-02): the Chat section sets the
  `forkmeshQuietSounds` property (`kQuietSoundProperty`), and
  `UiSounds::cueForEvent` returns nothing for an event on a widget with that
  property on itself or any parent, so clicks, keys and menus opened from chat
  make no sound. The chat send paths no longer call `UiSounds::play(Cue::Send)`.
  Notices and pings raised while chat is open keep their cue.
- **Merge cue** (2026-10-01, desktop only): every **Merge & clean up** button
  (Branches header, Worktrees pane, Agents rail) calls
  `UiSounds::play(Cue::Merge)` as it is clicked, before the merge starts. The
  figure is a dry wood-and-ping snap at 0 s (the immediate response), two
  `Swell` pads breathing in under a run of ten `Chirp`s that climb the scale
  and get louder over about 0.4 s (the swish build-up), and a bell landing at
  0.46 s. The synth has no noise source, so the swish is the overlapping
  chirps' upward swoops. Weight 9, between send and warn, so the click and any
  dialog the merge opens do not replace it. The phones have no merge button,
  so their cue tables stay at fourteen.

Tests: `forkmesh-sonifier-tests`, `forkmesh-ui-sounds-tests`,
`forkmesh-announcer-tests`, `forkmesh-machine-agent-scan-tests`, and the
package's `forkmesh-agent-watch-tests`.

## Voice Changer

The Voice Changer is a free Market package hosted in the desktop, not part of
the core app. See [../../marketplace/packages/voice-changer/README.md](../../marketplace/packages/voice-changer/README.md).

## Special agents (formerly brains, Team and teammates)

Rail **Special Agents**. Repository-owned special agent definitions in
`.forkmesh/teammates/*.json` (`src/TeammateStore.*`,
`src/MainWindowTeammates.cpp`) run on demand, on a cron cadence, on events, or
continually with a decision gate; see
[www/docs/qt-client.md](../../www/docs/qt-client.md#team-and-teammate-run-modes).
Error-log and crash repair are teammate tasks
([diagnostics.md](diagnostics.md#crash-handling-and-crash-repair)).

Operators can pause every teammate's automatic runs at once, which sets
`schedule.active` to `false` and keeps the cadence. The marketing and Sage
(`code-explainer.json`) contracts in
`app/tests/test_teammate_documentation_cadence.py` therefore only check that
`active` is a boolean, and they compare `days`, `events`, `mode` and `time`
exactly. The teammates checked into main are paused, and the old exact
`"active": true` check failed on them.

Since 2026-10-02 a teammate's permissions are enforced, not only described in
its prompt: a run without `external-tools`, `create-branches`,
`manage-issues`, `review-prs` or `merge` has the matching commands and MCP
tools refused, and an empty mode means Edit (Auto with `run-unattended`)
instead of the composer's global mode. See
[../agents/access-rules.md](../agents/access-rules.md#teammates).

### Special agents, not brains (2026-10-01)

The operator renamed the brains to **special agents**, and the page and rail
from **Brains** to **Special Agents**, the day after the brains rename below.
As before, only what a person sees changed: the desktop's labels, tooltips,
messages and agent prompts, the native iOS and Android screens, and these
docs. Stored and wire names are unchanged, exactly as listed in the 2026-09-30
section. The plain word "agent" still means any agent session on the Agents
page; a special agent is one of the repository's named role definitions in
`.forkmesh/teammates/`, and the session it starts is still an agent session.

### Mobile box layout (2026-10-02)

The operator asked for the Special Agents page to use the same style and box
layout as the iPhone and Android Special Agents screens
(`TeammateCard` in `native/ios/ForkMesh/Features/Teammates/TeammatesScreen.swift`).
The **Grid** view is now that layout and the page opens on it: each special
agent is one compact box (`MainWindow::buildTeammateBoxCard`) with a 36 px
avatar beside the name and specialty (the primary model when there is no
specialty), a status capsule (`teammateBoxStatus`, tinted by status) with a
clock glyph for an automatic schedule and an eye glyph for active log
monitors, then the next run and a thin schedule progress bar
(`teammateBoxProgress`, `scheduleProgress`), or the last-active line for a
special agent with no automatic schedule. Boxes have no border until hover,
14 px corners and a soft fill. `TeammateCardGrid` lays them out like
SwiftUI's `GridItem(.adaptive(minimum: 160), spacing: 12)`: as many 160 px
columns as fit, all stretched to fill the row, so a lone box no longer
stretches across the page; the old 300 px target, 340 px cap and the packed
spacer column are gone. The empty state carries `fullRow` and keeps the whole
width. Clicking a box opens the profile panel, which holds everything the
large card used to show. The **List** view keeps the full table row with
models, duties, skills, output and the prompt box. The choice is stored as
`teammates/listView` (default off), replacing `teammates/gridView`, so
everyone lands on the boxes once.

### Named brains, not teammates (2026-09-30)

The operator renamed the AI agents with a role from "teammates" to "brains",
and the page from **Team** to **Brains**, so they read as the repository's
standing minds rather than as people on a staff list. Only the text a person
sees changed: the desktop's labels, tooltips, messages and agent prompts, and
the matching native iOS and Android screens. Everything that is stored or sent
keeps its old name, so existing repositories, settings and paired phones keep
working: the `.forkmesh/teammates/` folder, `TeammateStore` and the other
`Teammate*` code names, `teammates/*` settings keys, the `teammates.` local
control family, the `#team` private chat room and its `team` sender id, the
`teammate-*.png` portraits, and the `kTeamSectionIndex` section. Organization
teams (groups of people with a repository permission) are a different thing
and keep the word "team".

The same day two Sentinel definitions were checked in: `sentinel.json`
(758e75ce1e, wired to the `review-security.md` shortcut, the
`security-review.skill` skill and `app/tests/test_security_sentinel_teammate.py`)
and `security-sentinel.json` (ea0b5217d6, an app-saved copy with no shortcut or
skill), so the Brains page showed Sentinel twice. The copy was removed;
`sentinel.json` is the one security special agent.

Hearth, the Discord community special agent added on 2026-10-01
([../community/hearth.md](../community/hearth.md)), has the `hearth` avatar key
and `teammate-hearth.png` (ember and rose). It was drawn with `codex exec`
image generation from the Sentinel, Steward and Marketing portraits as style
references, and set in Sentinel's gold ring with the name in DejaVu Serif Bold.

The Reddit special agent added on 2026-10-02
([../community/reddit.md](../community/reddit.md)) has the `reddit` avatar key
and `teammate-reddit.png` (orange and ivory), drawn the same way from the
Hearth, Y Combinator News and Sentinel portraits and set in the same gold ring.

Blogger, the blog special agent split out of Marketing on 2026-10-04
([../marketing/README.md](../marketing/README.md)), has the `blogger` avatar
key and `teammate-blogger.png` (teal and ivory, a pen-nib emblem), drawn the
same way from the Marketing, Reddit and Hearth portraits and set in the same
gold ring.

Crawler, the website inspection special agent added on 2026-10-07
([../www/site-crawler.md](../www/site-crawler.md)), has the `crawler` avatar
key and `teammate-crawler.png` (lime and graphite, a lens visor over one eye
and a glowing link graph behind), generated from the Reddit, Sentinel and
Hearth portraits and set in the same gold ring. Its phone crops are
`teammate-face-crawler` and `teammate_faces/crawler.jpg`.

Herald, the X post special agent added on 2026-10-08
([../marketing/x-ready-post.md](../marketing/x-ready-post.md)), has the
`herald` avatar key and `teammate-herald.png` (midnight blue and silver, a
herald's horn on the forehead), drawn with `codex exec` image generation from
the Blogger, Reddit and Hearth portraits and set in the same gold ring with the
name in DejaVu Serif Bold. Its phone crops are `teammate-face-herald` and
`teammate_faces/herald.jpg`.

UX, the user-experience special agent added on 2026-10-11, runs a weekday
evidence-backed review of real desktop, web, and mobile journeys. It reproduces
the highest-impact confirmed source of friction, fixes it narrowly, and adds
coverage rather than concealing failures or weakening accessibility checks. The
operator chose its `run-unattended` permission, so it can carry out its focused
repair and verification without asking again. Its `ux` avatar key uses
`teammate-ux.png` (midnight blue, teal, and amber); its phone crops are
`teammate-face-ux` and `teammate_faces/ux.jpg`.

### Special agent pages (2026-10-04)

The operator asked for a click on a special agent to open a page of its own,
with its information, its history, a way to talk to it, notes, and widgets
built together that help the work, while keeping a play button to just run
it. `src/MainWindowTeammateHome.cpp` adds that page as the fourth page of the
Special Agents stack (`teammateHomePage`, after the roster, Stats and
Briefing). The rail avatar, a grid box, a list row, the top search's special
agent hits, a `teammate:` member card and the mention profile all open it
(`openTeammateHome` / `showTeammateHome`); **Open profile** in the ⋯ menu and
**Profile & settings** on the page still open the profile and editor column.
Back and forward record it as sub-page 3 with the special agent's id.

- **Header**: back to the roster (or the dashboard, whichever it came from),
  Run now or Stop, the current session, Briefing, Profile & settings and the
  ⋯ menu, then the avatar with its play button, name, handle, role, status,
  schedule and what happens next.
- **Talk with** *name*: one timeline of what you sent from the page and the
  special agent's last 12 runs, each with its title, status, closing summary
  (Markdown, cut at 4,000 characters) and a link to the session. The composer
  sends through `sendTeammatePrompt`: while the agent works the message steers
  the running session; otherwise it starts a run with the message as the
  assignment plus the last four messages and the last reply as context, since
  a new session remembers nothing. Finished outputs are cached per session and
  finish time; a running session shows its latest assistant text without
  reading the event log. Opening the page clears the done badge of the run it
  shows.
- **Notes**: free text saved 0.7 s after typing stops.
- **Widgets**: checklists, goal trackers (a number against a target), pinned
  notes, quick requests (a saved message sent with one click) and links
  (http and https only). Add one yourself, or **Build together**: that sends
  `widgetBuildAssignment`, and the special agent answers with a fenced
  `forkmesh-widget` JSON block. Proposals found in finished runs appear under
  "Proposed by" with Add widget or Apply update (same id) and Dismiss; nothing
  is added without the operator, and a handled proposal is never offered
  again. The ⋯ menu on a widget edits, renames, asks the agent to update it,
  or removes it.
- **About**: description, schedule, model, last run, recent run outcomes,
  average grade, reminder, skills and recent activity.

Notes, widgets and the conversation are the operator's and stay on the
machine: `teammates::WorkspaceStore` (`src/TeammateWorkspace.*`) keeps one
JSON file per special agent in
`<data>/teammate-workspaces/<repository digest>/<id>.json`, the digest being
the one the runtime settings use (`teammateRepoDigest`). They are not written
into the repository's `.forkmesh/teammates`, and removing a special agent
leaves its file. Every run's prompt carries them
(`workspacePromptSection` in `buildTeammatePrompt`): the notes, each widget's
current state with its id, and the `forkmesh-widget` contract, so the agent
works with the checklist and goals and can propose updates. They never widen
its permissions. `forkmesh-teammate-workspace-tests` covers the store, the
proposal parser and the prompt section.

**The play button.** `makeTeammatePlayButton` draws a round play button on a
corner of the avatar: always visible at the top left of a rail item (16 px,
replacing the hover-only Consult corner), at the bottom right of a grid box's
avatar (18 px) and of the page's avatar (28 px). It runs the special agent on
demand through `runTeammateFromRail`; while it works the button shows a
terminal prompt and opens the session instead, so a click never stops work.
Its colour carries the run reminder that used to be a 10 px dot on the rail
avatar: green, orange or red while a reminder is set, blue when none is set,
green while working and grey when paused.

### Live faces and moods on the rail (2026-10-06)

The operator asked for each special agent on the right rail to show how it
feels, for the robot's whole face to move as if it were on camera rather than
an animated emoji, for the round avatar to open up to the full square, and for
the buttons to become a HUD: three round buttons along the top and the name
tiny along the bottom. The mood is to weigh 100 different things.

- **The square.** Each rail item is a 54 px square box (`RailItem`, still
  tinted by run freshness, see below). The face (`RailFaceButton`,
  `teammateRailAvatar`) fills it with a 2 px inset and 7 px corners.
  `teammateFacePixmap` (`MainWindowInternal2.cpp`) crops the portrait art to
  the inside of its gold ring (62 % of the image, centred a little above the
  middle so the baked-in name stays out) and returns a square; model-icon
  avatars use the whole icon. The status ring became a border on the square:
  solid when scheduled, done or failed, dashed when paused, and dashes that
  march around the edge while the agent works. The green done check badge is
  gone; the solid green border and the Celebrating mood carry it.
- **On camera.** The face is drawn 10 % larger than the square and moved every
  40 ms (`facePoseAt`): a slow breathing zoom, a handheld drift and a slight
  roll always, a dark vignette and a faint scan line sweeping down, plus the
  mood's motion. Wave rocks the head side to side and leans in toward the
  camera in bursts, which is how an agent waves for attention; Jump hops,
  Bounce bobs, Nod dips, Pulse zooms, Float drifts, Wiggle tilts, Shake shakes
  the head, Spin wobbles dizzily and Doze droops and dims. A paused agent always
  dozes behind a dimmed face. Every face shares one clock (`railFaceClock`) and
  takes a phase from its id, so faces do not move in step and a rail rebuild
  does not make them jump. The timer runs only while the face is shown.
- **The HUD.** Along the top, round and always visible: the play button
  (14 px, left), the mood emoji (`teammateRailMood`, 16 px, centred, not
  animated, with an amber edge when the mood wants attention) and the ⋯ menu
  (14 px, right, `teammateRailHud`). The ⋯ button used to appear only on
  hover. The CFO's Process payments button (`teammateRailProcessPayouts`)
  still appears only on hover, now under the ⋯ button. The name is a 7 px
  label on the bottom edge of the face. Clicking the emoji opens the special
  agent's page, and its hover names the mood and the three signals that
  weighed most.
- **The mood.** `src/TeammateMood.*` (`assessMood`) scores 17 moods from
  exactly 100 named, weighted signals (`moodFactorCount`): the current run
  (waiting for a reply, held, queued, just started, turns taken, time running,
  background builds, spend, cloud, unattended), the finished run (unseen,
  grade, pull request, failed, stopped, error text, usage limit, credits,
  unavailable model), the last 12 runs (success and failure streaks, failure
  share, average grade and duration, recent spend, runs today and this week),
  the definition (paused, schedule and cadence, run reminder, staleness, next
  run, skills, permissions, log monitors, workflow, instructions, relay room,
  self-improvement, activity, age and last edit, last trigger), the clock
  (night, morning, weekend, Monday, Friday afternoon, lunch) and the rest of
  the team (how many work or fail, team size, a special agent being found).
  The mood with the highest total wins. Celebrating, Needs you, Ready to run
  (past its reminder or a missed scheduled run), Frustrated and Stuck want
  attention; Needs you and Ready to run wave. The mood key and reasons are
  part of the rail signature, so a change rebuilds the rail on the next
  refresh or 30 s tick. `forkmesh-teammate-mood-tests` covers the count and
  the main moods.

### Channel status rail (2026-10-06)

The operator asked for another rail next to the special agents showing the
status of each marketing channel, with counts on the squares and helpful next
steps for each one.

- **Where.** `src/MainWindowPackageRail.cpp` builds `#packageRail`, a 60 px
  rail that `buildChatPage` puts just left of the special agents rail, so the
  special agents keep the window edge. It shows only when a package has
  squares to show, hides with the rest of the shell chrome in the reduced view
  modes, and can be turned off from its right-click menu or Settings >
  Appearance ("Show marketing channel status in a rail on the right",
  `ui/showPackageRail`, on by default).
- **Always there while Marketing is installed (2026-10-07).** The operator
  asked that the channel rail always show while the Marketing package is
  installed. It used to vanish until Marketing's first background load
  finished, whenever that load failed or the organization had Marketing off,
  and below a 1100 px window width. The width gate is gone (the special agents
  rail still hides below 1000 px), and `MarketingFeature::railTiles` returns a
  placeholder **Marketing** square (id `marketing`, neutral, no failing
  checks, so it never wins the channel reminder) whenever it has no channel
  squares, labelled Loading channels, Could not load, No organization yet,
  Not installed for this organization or No channels turned on. Load failures
  and an empty organization list now call `railChanged` so the label follows.
  A click opens the Marketing page. Only the user's own Hide this rail and the
  reduced view modes hide it now.
- **A package socket, not a Marketing hook.** The host still names no package:
  `NativePackageWidget` gained `railTitle`, `railTiles`, `prepareRail` and
  `activateRailStep`, and `NativePackageHost` a `railChanged` callback. Each
  `NativePackageRailTile` carries a label, a count and what it counts, a tone
  (good, attention, warning, neutral, paused), a state label, detail lines,
  the number of failing checks and a list of next steps. Marketing is the only
  package that answers today; another package can add its own group, headed by
  its `railTitle`.
- **The square.** `PackageRailTileButton` (`src/PackageRailTile.*`) is a
  54 px rounded square tinted and bordered by its tone (dashed when paused),
  with the count in bold in the middle, the name in 8 px along the bottom and a
  round badge with the number of failing checks in the top right corner. The
  hover lists the state, the count, the details and the next steps.
- **Next steps.** A right-click opens a menu to the left of the square: the
  state, count and details as greyed lines, a **Next steps** section, then
  Open <channel>, Open Marketing and Hide this rail. Choosing a step opens
  the package page and hands the step to `activateRailStep`.
- **A click opens the channel (2026-10-06).** The operator asked that
  clicking a marketing channel bring up that channel's page. A tile with
  `opensPage` set makes a left click call the package's `openRailTile`
  and then show the package page; Marketing opens its channel page there
  ([Marketing README](../../marketplace/packages/marketing/README.md#desktop-channel-page)).
  A tile without it still opens the menu on a click, and the hover ends
  with "Click for details, right-click for next steps." when it is set.
- **Loading.** Package pages are built at startup but load on first open, so
  the host calls `prepareRail` once each package page exists and every 15
  minutes; Marketing then loads its organization in the background unless its
  page is open. Every load calls `railChanged`, and the rail rebuilds only when
  its signature (tiles, steps, theme) changes.
- **Loads after sign-in, in the background (2026-10-07).** The operator
  reported the channel rail not fully loading after a desktop restart, and
  asked that it, and everything else, load in the background. The first
  `prepareRail` ran on the first event-loop turn, but sign-in waits for
  `runDeferredStartup` (5 s after the first show), so every package request
  failed with "sign in" and the rail sat on Could not load for 15 minutes.
  Now `syncNativePackagePages` prepares a page only once there is an account
  session or startup sign-in has resolved; `activateAccountSession` calls
  `preparePackagePages` (each page's `prepareNavigationBadge`, then
  `prepareRail`) after every sign-in or account switch, and
  `runDeferredStartup` calls it when startup ends signed out so the square
  can say why. Marketing retries a failed load by itself after 30 s, doubling
  up to the 15-minute tick (`railLoadFailed`), whether or not its page is open.
- **A restored account prepares the pages too (2026-10-08).** The operator
  reported the Channels square still sitting on a dash after a restart. The
  2026-10-07 fix leaned on `activateAccountSession`, which only runs for an
  email/password or web login; a restart signs in through
  `authenticateSilently`, which sets the session without going near it. So
  `runDeferredStartup`'s `if (!hasActiveAccountSession())` guard skipped
  `preparePackagePages` for exactly the case it was meant to cover, and
  nothing else re-prepared: `syncNativePackagePages` returns early when the
  installed set is unchanged, and `applyAccountPackages` only re-syncs when
  an install changed, so the first `prepareRail` waited for the 15-minute
  `m_packageRailClock` tick. `runDeferredStartup` now calls
  `preparePackagePages` unconditionally once startup sign-in has resolved,
  signed in or not. Guarded by
  `app/tests/test_desktop_package_rail_startup.py`.
- **Google (Marketing 1.11.0).** Google is a default Marketing channel, so the
  rail always has its square: visits from Google in 30 days (else pages on
  Google), with steps to refresh the visits and to count the pages with the
  browser extension
  ([Marketing README](../../marketplace/packages/marketing/README.md)).
- **Email (Marketing 1.13.0).** Email is a default channel too: its square
  counts emails sent in 30 days (from CRM and its Zoho Mail sync) and lists
  closed deals; its steps refresh from CRM or point at CRM to connect Zoho
  Mail, since a package page cannot open another package from the rail
  ([Marketing README](../../marketplace/packages/marketing/README.md#email-outbound-and-closed-deals-from-crm-and-zoho-1130)).
- **Lined up with the special agents (2026-10-13).** The operator asked for a
  heading over the special agents that lines up with **Channels**, and for
  the channel squares to be the same size as the agents. The special agents
  rail now opens with a **Specials** label (`#teammateRailTitle`, styled with
  `#packageRailTitle`), the channel square grew from 52 to 54 px (the
  special agent item's size) and the channel rail's spacing went from 6 to
  8 px like the agents rail, so the two columns' squares sit level row by row.

### Channel reminder (2026-10-07)

The operator asked for a marketing reminder that checks the channels and,
when nothing has changed in an hour, keeps reminding them to fix the channel
that most needs attention.

- **What counts as a change.** `refreshPackageRail` keeps a state key per
  square (`packageRailStateKey` in `src/PackageRailTile.cpp`): its id, count,
  count label, tone, state label and number of failing checks. Details and
  steps are left out because they carry "3 days ago" text that moves on its
  own. A new key restarts `m_packageRailReminderTimer`, so the timer fires one
  hour after the last change and every hour after that. The 15-minute
  `prepareRail` tick is what keeps the channels checked meanwhile.
- **Which channel.** `packageRailNeedsMoreAttention` ranks the squares across
  every package: Warning (Marketing's stale numbers) over Attention (a failing
  check) over a neutral or good square with failing checks (not set up);
  paused and fully passing squares never qualify. More failing checks breaks a
  tie, and the first square in rail order wins a full tie.
- **The reminder.** `checkPackageRailReminder` files a dismiss-only ping, kind
  `channel_reminder` ("Channel reminder" on the Pings page), titled
  "ForkMesh — fix <channel>", with the state, the failing-check count and the
  first next step other than Open. It has no countdown and stays until
  Dismiss, and is raised again after a restart if it was still up, like the
  usage-limit ping. The OS card is resident too.
- **One at a time.** While a channel reminder is still waiting to be
  dismissed no new one is filed, so an idle evening leaves one card rather
  than a stack. After Dismiss, the next hour without a change files another.
- **Opening it.** The ping links `{kind: "package-rail", name: <package id>,
  ref: <square id>}`, and the OS card the href
  `fm:package-rail:<package>:<square>`; both open the square's page through
  `openPackageRailTile`, the same path as a left click on the square.
- **Turning it off.** Settings > Appearance, "Remind me to fix the channel
  that most needs attention when nothing changes for an hour"
  (`ui/packageRailReminder`, on by default). Hiding the channel rail stops it
  too.
- **Tests.** `forkmesh-native-package-tests` covers the ranking, the state
  key and the reminder text.

### Phone Special Agents grid (2026-10-06)

The operator asked for the iPhone and Android Special Agents pages to follow
the rail, with four or five special agents across in a tight grid instead of
two large cards per row.

- **The grid.** iOS uses `GridItem(.adaptive(minimum: 64, maximum: 120),
  spacing: 8)`, Android `GridCells.Adaptive(64.dp)` with 8 dp gaps, so a
  393 pt iPhone fits five across and a 375 pt one four. The header, filter
  chips, empty state and errors span the whole row
  (`LazyVerticalGrid` with `fullRow` on Android).
- **The tile.** Each special agent is a square (`TeammateTile` on both
  platforms) with 10 pt corners: the face fills it, with a vignette, the name
  in 9 pt along the bottom edge, and a HUD along the top: play (left), the
  mood emoji (centre, amber edge when it wants attention) and ⋯ (right). Play
  runs the special agent through the usual confirmation; while it works it
  shows a terminal glyph and a tap falls through to the page. ⋯ lists the
  specialty, the mood and its reasons, and the last run, then Run now or
  Stop and Generate briefing. Tapping the face opens the special agent's
  page. The border carries the status like the rail: marching dashes while
  working (green) or queued and waiting (amber), solid blue when scheduled,
  dashed grey on a dimmed face when paused.
- **On camera.** One clock per tile (`TimelineView` at 30 fps on iOS, paused
  under Reduce Motion; an infinite transition read only in the layer and draw
  phases on Android) drives the same breathing zoom, drift and roll as the
  desktop plus the mood's motion (`TilePose`), with a phase from the id so
  faces do not move in step.
- **The faces.** The phones had initials only. The portraits are now bundled
  pre-cropped the way `teammateFacePixmap` crops them (62 % of the art,
  centred at 46 % height, 192 px JPEG): `teammate-face-<avatar>` image sets in
  `Assets.xcassets` and `assets/teammate_faces/<avatar>.jpg` on Android
  (`TeammateFaces` and `rememberTeammateFace`). Model avatar keys (opus,
  sonnet, haiku, fable, sol, luna, terra) use the existing `agent-face-*`
  art; anything else keeps initials. The round `TeammateAvatar` on the
  special agent's page, briefing and stats uses the same faces. A new
  portrait on the desktop needs its crop added to both apps and its key to
  both lists.
- **The mood on the wire.** `teammates.list` rows carry `mood` (`key`,
  `emoji`, `label`, `motion`, `attention`, `intensity`, `reasons`;
  `moodJson`). `MainWindow::assessTeammateMoods` is the rail's mood pass
  lifted out of `refreshTeammateRail`, so the rail and the phones score the
  same signals. An older desktop sends no mood: the tile hides the emoji and
  floats.

### Video screens and run captions on the dashboard (2026-10-07)

The grid boxes on the Special Agents page had collapsed to a strip about
35 px tall: a `QPushButton` ignores its own layout's size hint, and once
`setMinimumWidth` gave the card an explicit minimum size the layout no longer
set its minimum height. The operator asked for the page to show each special
agent as a small video-call screen like the right rail, with every control
the rail square has, and to show each special agent's latest caption under
its face. Each run's caption should sum up the conversation in about 200
characters.

- **The screen.** Grid view (the dashboard and every filter) builds
  `buildTeammateVideoTile` (`MainWindowTeammateRail.cpp`) instead of the old
  box card. `RailVideoTile` is a `RailFaceButton` with 12 px corners, so it
  has the rail's live face, mood motion, vignette, scan line and status
  border. It is at least 240 px wide, and `TeammateCardGrid` uses the widest
  card minimum for its column count. `fitTileHeights` fixes each tile's height
  to `heightForWidth` of its column: half the width for the face plus a
  caption band of up to five lines. The face sits in the area above the band.
  A blurred, darkened copy of the face fills the rest of the screen, the way
  a call letterboxes a portrait camera. Faces repaint every 66 ms instead of
  40 ms because the screens are much larger than rail squares.
- **The controls.** `addTeammateRailHud` builds the rail's HUD for both
  places: play (left), the mood emoji (centre) and ⋯ (right), plus the CFO's
  Process payments under ⋯ on hover. The tile copies use 22/24 px buttons
  named `teammateTilePlay`, `teammateTileMood`, `teammateTileMenuButton` and
  `teammateTileProcessPayouts`. A click opens the special agent's page and a
  right-click opens the rail menu (`showTeammateRailMenu`). The tooltip, ring
  and accessible name come from `railFaceLook`, which the rail uses too, so
  the dashboard also gets a solid ring when a finished run has not been seen.
- **The caption.** The band shows the name, the status and how long ago it
  last ran, then the caption in white on a translucent black band like live
  captions. `buildTeammatePrompt` now asks every run to end with
  `Caption: <one plain sentence of at most 200 characters>`.
  `runCaptionFromSummary` (`TeammateStore.cpp`) uses the last labelled line.
  Older runs and runs that skip the line get a condensed caption: the closing
  summary with headings, fenced code, tables, hand-off lines and improvement
  notes dropped, markdown links and emphasis stripped, cut at a sentence or
  word boundary near 200 characters. The caption comes from each run's
  closing summary, so every run has one and nothing new is stored.
  `briefingMainPoints` skips the `Caption:` line so it does not repeat as a
  briefing point.

## Teammates rail, recent model and hidden teammate runs (2026-09-29)

`src/MainWindowTeammateRail.cpp` adds a second, narrow rail on the right edge
of the window (`#teammateRail`, `buildChatPage` puts it after the page shell).
It lists the teammates of the repository the Team section points at
(`activeTeammateRepoIndex`). Each teammate is just an avatar and a name. The
status is drawn around the avatar (`RailAvatarButton`) instead of as a dot: a
spinning arc while it works (green) or is queued or waiting (amber), a solid
blue ring when scheduled, a solid red ring when the last run failed, a dashed
grey ring on a faded avatar when paused, and no ring when idle. How long ago
it last ran (`lastRunAtMs`, refreshed every 30 s) moved into the avatar's
hover, next to the role, status, model and what a click does. Clicking the
avatar ran the teammate until 2026-10-04; it now opens the special agent's page
and the play button runs it ([Special agent pages](#special-agent-pages-2026-10-04)).
While it works, a click opened its agent session instead of stopping it. A click always starts an agent session, including for
a special agent with a native workflow (since 2026-10-01): clicking the CFO used to go
through `runTeammate` to the contributor payout run, which without armed
payouts only answered "CFO schedule recorded. Arm CFO daily payouts…" and
started nothing. `runTeammate` now takes the payout run only for
`kTaskTypeScheduled` work (the schedule and a continual loop), and arming
payouts in Control > Reward pool calls `runContributorPayoutTeammate` itself.
Every other start (the rail, the Team page Run buttons, a direct prompt, an
@mention, briefing follow-ups, a log monitor, a repository event and phone
control) runs the special agent's session. The Run button is gone, and the ⋯ menu button only
appears in the box's bottom-right corner while the pointer is on the teammate
(`RailItem`); right-clicking the avatar opens the same menu with every option:
run or stop, run with a prompt, open the agent session, profile, settings,
skills, briefing, schedule and availability, and remove. Dropping the age line
and the button row (2026-09-30) makes each teammate about half as tall. The
rail rebuilds only
when its signature changes; `refreshAgentTable`, `onAgentStatusChanged` and
`loadTeammatesPanel` ask for a debounced refresh, which waits while a popup
menu is open. It can be turned off from its right-click menu or with Settings >
Appearance > "Show teammates in a rail on the right" (`ui/showTeammateRail`, on
by default). It hides with the rest of the shell chrome in the reduced view
modes and while the window is narrower than 1000 px: at a few hundred pixels
its then 76 px pushed the end of each Agents row, where the delete button sits,
out of the table.

Since 2026-10-01 each box hugs the avatar: 3 px around the 44 px face, so the
rail is 60 px wide. The name is a 9 px pill laid over the bottom 8 px of the
avatar (`teammateRailName`, transparent to the mouse so a click still reaches
the avatar) instead of a line below it, and the item is placed by hand rather
than with a layout. The box's corners are hot corners: 16 px buttons that
appear with the ⋯ button on hover (`RailItem::addHoverButton`) and each run a
different exercise. Top left, Consult (`teammateRailConsult`), ran the special agent's
general session on demand, or opened its session while it worked; since
2026-10-04 the always-visible play button (`teammateRailPlay`) has that corner. Top right holds
the special agent's own workflow; for the CFO (`kWorkflowContributorPayouts`) it is
Process payments (`teammateRailProcessPayouts`), which calls
`processTeammatePayoutsFromRail`: it checks the special agent is available and holds
`process-payouts`, then starts `runContributorPayoutTeammate`, which asks to
arm the desktop-linked wallet if the session is not armed yet. Special agents without a
workflow leave that corner empty, and bottom right is the ⋯ menu.

Each special agent sits on a rounded rectangle (`RailItem::paintEvent`, since
2026-10-01) whose colour shows how recently it ran compared with how often it
should run. `teammates::expectedRunIntervalSecs` turns the schedule into a
cadence: an hour for hourly, a day for continual loops, the widest gap between
scheduled weekdays for daily, weekday, weekly and custom schedules (three days
for weekdays, so a Monday before the first run is not alarming), and a week for
on-demand, event-driven or paused schedules. `teammates::runStaleness` is the
time since `lastRunAtMs` over two cadences, clamped to 0..1, and the rail maps
it to a hue from green (130°) through amber to red (0°): green right after a
run, amber when the next run is due, red once a whole run was missed or when
the special agent never ran. A working special agent counts as fresh, and a paused special agent gets a
neutral grey box. The rounded staleness (twenty steps) is part of the rail
signature, so the 30 s clock repaints the fade without rebuilding on every
tick. The hover also names the schedule.

Since 2026-10-04 the rail and the Special Agents page (both the box grid and
the list) put the special agent that ran last on top: rows are stable-sorted by
`lastRunAtMs`, newest first (`teammates::ranMoreRecently`), and special agents
that never ran keep their definition order at the bottom. A run stamps
`lastRunAtMs` when it starts, so a special agent jumps to the top as soon as it
is run, and the new order is part of the rail signature, which rebuilds it.

A special agent can also carry a run reminder (2026-10-04): `runWindowHours`
in its definition, 0 (off) to 720, set from the editor's Schedule section
("Run reminder", `teammateRunWindowHours`). While it is set and the special
agent is available, the avatar got a 10 px dot on its top-left corner
(the top-right corner belongs to the done badge); since 2026-10-04 the play
button in that corner takes the colour instead. `teammates::runWindowState` reads the time since `lastRunAtMs`: green
(`Fresh`) after a recent run, orange (`DueSoon`) in the last quarter of the
window, capped at six hours, and red (`Overdue`) once the whole window passes
without a run or when the special agent never ran. A working special agent shows green;
a paused one shows no dot. The state is part of the rail signature, so the
30 s clock flips the colour without a timer of its own, and the hover adds a
"Reminder:" line from `teammates::humanRunWindow`. The marketing special
agent ships with a 24-hour window (`.forkmesh/teammates/marketing.json`);
every other special agent has it off until the operator sets one.

A teammate whose run finished successfully gets a solid green ring and a
green check badge on the avatar's top-right corner until someone looks at the
run (2026-09-30). `notifyAgentDone` calls `markTeammateDoneUnseen`, which
records the session in `teammates/doneUnseenSessions` so the badge survives a
restart. It skips a run the user is already watching (selected in a visible
Agents list in the active window). While the badge shows, clicking the avatar
opened the session instead of starting a new run, and the hover read "Done ·
click to see session #N"; since 2026-10-04 the click opens the special agent's
page, which shows the run and clears the badge. Opening the session any way clears it:
`switchToAgentsTab` (the rail, a Ping, a notification) and `showAgentSession`
while the Agents list is on screen both call `clearTeammateDoneUnseen`. The
badge is keyed by session, so a new run hides it, and ids of deleted sessions
are dropped the next time a run is marked. A failed run keeps the red ring and
gets no badge.

The button below the teammates, **Create needed teammate**, starts an agent run
on the teammate repository. Its prompt names the repository, the ForkMesh log
and diagnostics folders, the MCP tools for chats, issues, tasks and notes, the
existing teammates, the latest 40 agent runs and their errors, and asks the
agent to read commits, docs, errors and chats without changing anything and to
end with one JSON teammate definition. When the run succeeds
(`notifyAgentDone` > `finishTeammateCreatorRun`) the desktop, not the agent,
saves the definition: `teammates::teammateFromAgentOutput` clears any native
workflow, model routes, shortcut and history, drops the `merge` and
`process-payouts` permissions, adds `read-repositories`, and stores the
schedule and log monitors paused so the operator starts them. A clashing id
gets a numeric suffix. Run ids are kept in `teammates/creatorSessions` so a
run that finishes after a restart is still adopted.

A teammate run starts in the teammate's own `mode` (Auto, Edit, Ask) only
when it holds `modify-code` or `run-unattended` ("Run tools without
asking"); otherwise `startTeammateSession` forces Plan. `run-unattended` is
for special agents like Hearth whose work is a guarded external command rather than
code (`docs/community/hearth.md`). `teammateFromAgentOutput` drops it along
with `merge` and `process-payouts`, so a generated teammate never grants
itself unattended runs. Since 2026-10-01.

Since 2026-10-02 the mode and the remaining permissions are enforced in code
(`agentrules::teammateRunMode` and `deniedTeammatePermissions` in
`AgentRunRules.cpp`). An empty `mode` used to fall back to the global composer
mode, Auto by default, so Code Steward, Error Fixer and the other
`modify-code` teammates ran with every permission skipped. Now an empty mode
is Auto only with `run-unattended` and Edit otherwise, and Auto without
`run-unattended` drops to Edit. A permission the teammate lacks is refused at
the external-call gate and, for Claude Code, by `--disallowedTools`:

| Permission missing | Refused |
| --- | --- |
| `external-tools` | every external call except the project's approved destinations; WebFetch, WebSearch |
| `create-branches` | `git push`, `git branch <new>`, `git checkout -b`, `git switch -c`, `git worktree add`, `gh pr create`, MCP `open_pr_from_branch` |
| `manage-issues` | `gh issue create/comment/edit/close/...`, MCP issue, milestone and project writes |
| `review-prs` | `gh pr review/comment/edit/close/ready` |
| `merge` | `gh pr merge` |

The grant is recorded per session in `m_teammateRunGrants` when the run
starts (and for the session a continued run reuses); runs not started by a
teammate get no teammate restrictions.

Teammates never open pull requests. The `open-prs` permission ("Open pull
requests") is gone from `teammates::knownPermissions`, so a saved teammate that
still lists it drops it on load and on the next save, and every teammate prompt's
permission boundary says to leave finished work committed on its branch for a
person to open the pull request. Scheduled and log-triggered teammates (Fixer,
Quality, Sage and the others in `.forkmesh/teammates`) were opening pull
requests through the MCP `open_pr_from_branch` tool with nobody asking; see
[Pull requests are explicit](../agents/README.md#pull-requests-are-explicit).
Since 2026-09-30.

Teammates no longer run on a stored model (the stored fallback, per-task
routes and backup models were removed on 2026-10-04, see below). `teammates::recentAgentChoice` returns
the provider and model of the newest agent session that no teammate started
(`MainWindow::recentlyUsedAgentChoice`, registered with
`setRecentAgentChoiceSource`), so switching models for your own work switches
the team as well. The stored `provider`/`model` is only a fallback when there
is no such session. Explicit per-task routes and backup models still applied
until 2026-10-04.
The editor hides the primary model picker and shows the model in use, and the
local-control teammate payload reports the same model.

Since 2026-10-01 the source is the agent and model picked in the prompt area
(`MainWindow::promptAreaAgentChoice`, reading `m_quickAddAgentProvider` and
`m_quickAddClaudeModel`), so a special agent runs on whatever the composer shows even
before you start a run with it. The newest-session choice is only the fallback
when the composer holds a prompt action (manual, ActivityPub) or Cloudflare AI,
which cannot work in a checkout. Changing the composer's agent or model asks
the rail for a refresh so the hover names the new model.

### No branch or worktree by default (2026-10-07)

The operator asked for special agents to stop creating branches and worktrees
unless they are needed, with that turned off by default. Settings > Agents has
**Give special agents their own branch and worktree**
(`kTeammateWorktreesSetting`, `teammates/ownWorktrees`), off by default.

While it is off:

- `startTeammateSession` and `createNeededTeammate` pass
  `directCheckout=true` to `startAdHocAgentForRepo`, so a Claude Code or Codex
  run works in the repository's own checkout like a **No worktree** run. Other
  providers cannot run without a worktree and still get one.
- `agentrules::teammateWorkPermissions` drops `create-branches` from the run's
  grant, so the gate and `--disallowedTools` refuse `git branch <new>`,
  `git checkout -b`, `git switch -c`, `git worktree add`, `git push` and
  `open_pr_from_branch` even for a special agent whose definition holds it.
- `buildTeammatePrompt` lists Create branches under "Not allowed" and tells
  the agent to work in the checkout and leave finished work there for review,
  instead of committing it on its branch.

A session that already has a worktree keeps it when it is continued. Turning
the setting on gives each new special agent run its own branch and worktree,
and gives `create-branches` back to the special agents that hold it.

### Low-cost special agents (2026-10-07)

Issue 1739 asked for the Genie spider's special agent to run on a very low-cost
model. The one exception to "no prescribed agent settings" below is a boolean,
not a model: a definition with `"lowCostModel": true` keeps the prompt
settings' agent but takes its cheapest model (`lowCostAgentChoice` in
`TeammateStore.cpp`): Haiku 4.5 (`claude-haiku-4-5`) for any Claude provider,
GPT-5.6 Luna for Codex and GPT-4.1 mini for an OpenAI key; Cloudflare, local
and other providers keep their model. `agentChoiceFor(config)` applies it, and
`startTeammateSession`, the prompt's "Task type and agent" line, the play
button and rail tooltips, the roster card, the profile pill and Assigned agent
section (which says "the low-cost model of the prompt settings' agent") and
`teammates.list` on the phones all use it. `toJson` writes the key only when
it is true, so other definitions do not change on save; older desktops ignore
it and run the prompt settings' model. Only Genie (`genie.json`,
[genie.md](genie.md#replying-to-the-spider-the-genie-special-agent)) sets it.

### Special agents on Marketing channel pages (2026-10-07)

Issue 1764 asked for a marketing channel with a special agent attached to show
that agent on the channel's page in Marketing, so the operator can work with
it there. A definition attaches itself with `"marketingChannels"`, a list of
Marketing channel ids (`reddit`, `discord`, `hacker-news`, `blog`, the ids in
`CHANNEL_CATALOG` of `marketplace/packages/marketing/content/worker/marketing.py`).
`Config::fromJson` keeps lower-case ids of letters, digits and dashes, 16 at
most, and `toJson` writes the key only when the list is not empty, so other
definitions do not change on save. The checked-in attachments are Reddit →
`reddit`, Hearth → `discord`, Y Combinator News → `hacker-news`, Blogger →
`blog` and Herald → `x`. Marketing itself is not attached to any one channel: it directs them
all. Name matching was rejected because the Blogger agent would land on the
`blogger` (Blogger.com) channel instead of `blog`.

`teammates.list` rows (`teammateRow` in `LocalControlTeammates.cpp`) carry
`marketingChannels`, and native packages read the same rows through
`NativePackageHost::specialAgents` (see `docs/marketplace/README.md`, "Special
agents"). The Marketing package's channel page shows the attached agent with
Open, Run now and a message line
(`marketplace/packages/marketing/README.md`, "Special agent on a channel page").
`MainWindow::nativePackageSpecialAgents` adds `face` to each row it hands a
package: the rail's `teammateFacePixmap`, clipped round at 64 px and encoded as
a base64 PNG, cached per avatar, provider and model. Marketing draws it on the
channel chips along the top of its page and on the channel page's panel.

### No prescribed agent settings (2026-10-04)

The operator asked for special agents to start with whatever is selected in
the prompt settings and for their prescribed agent settings to go. A special
agent definition no longer has `provider`, `model`, `mode`, `backupModels` or
`taskAgents`: `Config`, `TaskAgentRoute`, `primaryAgentFor`, `agentForTask`
and `backupModelsFor` are gone (the teammate home page's play button and info
panel call `recentAgentChoice` like the rail), and the keys were stripped from every
`.forkmesh/teammates/*.json`. `Config::fromJson` ignores them in an older
definition and `toJson` no longer writes them.

- Every start (`runTeammate`) uses `teammates::recentAgentChoice()` (the
  composer's agent and model, see above) for every task type, including
  `@mention` replies, which used to drop to Haiku 4.5 or GPT-5.6 Luna, and the
  Quality and Fixer task routes. With no choice at all it falls back to
  `defaultAgentProvider()`.
- The run mode is the composer's mode selector (`m_quickAddModeSelector`).
  `agentrules::teammateRunMode` still clamps it to the special agent's
  permissions: without `modify-code` it is Plan, and Auto without
  `run-unattended` becomes Edit.
- Exhausted credits no longer move a run onto a backup model. A model the
  provider rejects still moves the run to the same provider's Settings model,
  then the built-in default (`nextTeammateBackupModel`).
- The editor's **Assigned agent** section only names the model in use; the
  hidden model picker, the run-mode picker, the backup list and the per-task
  agent pickers are gone. The card and profile show one `Model · …` pill.
- Phone control (`LocalControlTeammates`) no longer reports `mode`,
  `backupModels` or `taskAgents`, and lists none of the five as editable.
  `applyEdit` accepts and ignores them, so an older phone build that still
  sends them, or always sends `provider` when it creates a special agent, is
  not refused.
- Crash repair used to find its owner by the `error-logs` task route. It now
  goes to `teammates::kErrorFixerId` (`error-fixer`) or to any special agent
  with an active `ERROR` monitor (`errorRepairTaskType`).

The Agents list now hides teammate runs by default
(`AgentTeammateFilter::WithoutTeammates`); the Teammates filter cycles to
teammate runs only, then all runs. Opening a teammate's session explicitly
(`switchToAgentsTab`) switches the filter to all runs so the row is visible,
and the Agents landing page picks the newest run the filter shows.

`switchToAgentsTab` is also what **View agent** on an inline notice or a
Ping calls, and it must land on that exact run (adhoc #1359, 2026-10-04).
Before, it only relaxed the filter for a teammate run under "without
teammates". When the list still hid the requested run (the top search box
typed on the Agents tab, the "teammates only" filter, or an
Efficiency/Guardrails/Models board left open), `refreshAgentTable` fell back to
the first row and its deferred selection opened that agent instead. Now it
switches the filter to all runs whenever `agentTeammateFilterShows` rejects the
run, returns the Agents page to the session list, and clears the top search when
`agentSessionMatchesQuery` (the same match the list uses) leaves the run out.
The window test with sessions 135901/135902 covers the search case.

Since 2026-10-04 (#1385) the Agents list starts every launch with the filter
off (`AgentTeammateFilter::All`). The filter is not saved, so starting on
"without special agents" hid every special agent session after each restart
until the operator clicked the button, and the sessions seemed to take a while
to appear. The button still cycles all → without special agents → special
agents only → all.

## Each teammate in its own session (2026-09-30)

Team runs used to put every teammate into one shared agent session
(`runTeammateTeam`, since 2026-08-27): one model call played every teammate
under `# Team lane:` headings, and the run then became each participant's
`lastSessionId`. A teammate's next solo run, prompt, message or stop went to
that shared session, so members kept working in the team's session, or in
another teammate's, instead of their own.

Now **Run team**, **Generate briefing** and an `@all` mention in `#team` start
or continue one session per teammate. `runTeammateTeam` calls
`startTeammateSession` (the prompt-session half of `runTeammate`, which
briefings use so a native-workflow teammate such as the CFO reports without
running its workflow) for each member; `@all` calls `deliverTeammateMessage`
for every enabled prompt-based teammate, so each one replies in the thread as
itself and a busy teammate gets the mention as a steer. The shared-run prompt
and `markAgentSessionTeam` are gone.

`ownTeammateSession` returns the session `lastSessionId` points at only when
it is that teammate's own run (`teammateId` is the teammate, `teammateIds` is
empty). Continuing, steering, stopping, the busy check, the rail and the
briefing status all go through it, so a teammate whose `lastSessionId` still
names an old shared run starts a fresh session of its own; briefing follow-up
buttons fall back to that too. Old shared sessions keep their `teammateIds`
badge and their finish handling in `handleTeammateFinished`.

### Special agent runs go in a folder, and Run all in one session (2026-10-07)

The operator asked for special agent runs to always land in a folder so the
Agents list stays organised, and for a button that runs every special agent
under one agent session and updates them all as needed.

- **The Special Agents folder.** Every session a special agent starts is filed
  in its repository's **Special Agents** folder in the Agents list
  (`fileTeammateSessionInFolder`, called from `markAgentSessionTeammate` and
  `markAgentSessionTeam`). `AgentFolders::findOrCreate` reuses a folder with
  that name, matched without regard to case or spacing, and makes it when the
  repository has none, so deleting or renaming it just makes a new one on the
  next run. A session already in a folder stays where it is, and a session
  taken out of the folder is not put back. Existing special agent sessions
  are filed once, on the first schedule check after the update
  (`teammates/agentFolderBackfilled`).
- **Run all in one session.** A header button on the Special Agents page
  (`teammateRunAllOneSessionButton`, next to **Run Special Agents**, which
  still starts one session per special agent) and the rail's right-click
  menu call `runTeammatesInOneSessionNow`. It takes the same special agents
  as **Run Special Agents** (enabled, prompt-based, with Read repositories,
  not already working) and starts one session titled "Special Agents · All in
  one session". When a shared run for the repository is still working, it
  opens that session instead of starting another.
  - **Prompt.** `buildTeammateSharedPrompt` puts each special agent's own
    prompt (`buildTeammatePrompt`, without its closing instructions) in a
    `# Lane: @Handle · Name · .forkmesh/teammates/<id>.json` section, shortened
    in the middle when the lanes would pass 128 KB. The session works the
    lanes in order and reports each one under a `## @Handle` heading. When a
    lane's work shows that special agent's definition is out of date, it
    edits that file; it may not change ids, permissions, the enabled flag or
    schedules, nor add or remove special agents. A run that can only plan
    describes the change instead.
  - **Permissions.** The session holds only the permissions every
    participant has (the intersection of their work permissions, and the
    app-log grant only when every one of them watches the log), so running
    together never gives one special agent another's rights. The mode comes
    from that shared set through `teammateRunMode`.
  - **Bookkeeping.** The session carries every participant in `teammateIds`,
    as the pre-2026-09-30 shared runs did, so the row shows the team badge
    and each special agent's page lists it. Each participant's last run and
    activity are updated when it starts. `lastSessionId` is not touched, so
    every special agent keeps continuing, steering and stopping its own
    session (see "Each teammate in its own session" above). When the session
    finishes, `handleTeammateFinished` splits the closing summary by
    `## @Handle` heading (`teamChatRepliesFromSummary`) and records each
    special agent's own section in its activity.

### External Claude Code sessions go in a folder (2026-10-07)

The operator asked for external Claude Code sessions to be filed in their own
folder, as special agent runs already are (see above).

- `registerExternalSession` files a newly surfaced external row in its
  repository's **External Claude Code** folder
  (`fileExternalSessionInFolder`, `AgentFolders::findOrCreate`). The member key
  is the row's read key, `claude:<uuid>`, so the filing survives the
  per-run temp ids.
- A row already in any folder stays where it is. A row dragged out of the
  folder stays out for the rest of the run; the next desktop run files it
  again if the session is still live, since the scan only surfaces live ones.
- External rows are only surfaced while their session is live, so every
  filing first drops the folder's `claude:` members no longer surfaced for that
  repository (`AgentFolders::members`). The `agents/folders` setting does not
  grow with each session ever seen. Members in other folders are left alone.
- Machine-wide agents (`pid:` keys) and cloud rows are not filed.

### A mentioned brain reuses its own session (2026-10-01)

Mentioning a brain (`@Quality` in `#team` or any chat, an issue or pull
request, or an `@Name:` hand-off line in another brain's closing summary)
goes through `deliverTeammateMessage`, which steers the brain's session while
it works and otherwise continues it (`continueTeammateSession`). Both look the
session up from the brain's runtime `lastSessionId`, and that id kept getting
lost: when a self-improving brain finished a run with real changes,
`handleTeammateFinished` saved the improvement cycle and then wrote the brain
as loaded from its checked-in JSON, where `lastSessionId`, `lastRunAtMs` and
`lastTrigger` are always empty, over the runtime settings. Every later mention
or click then started a new session (CEO went from session 861 to 1161, and
Marketing, Mobile, CFO and others held `lastSessionId=0` while their earlier
sessions were still in the Agents list). The improvement step now reapplies
the runtime (`teammateWithRuntime`) before saving it.

`teammateWithRuntime(config, repoIndex)` also repairs ids that are already
lost: through `ownTeammateSessionId`, when `lastSessionId` does not name one of
the brain's own sessions, it takes the newest session in the Agents list whose
`teammateId` is the brain (not a shared team run) on the same repository. A
brain only starts a fresh session when it has none of its own left.

### An improvement cycle needs a new lesson (2026-10-04)

A self-improving brain's run with real repository changes used to bump
`improvementCycles` and `updatedAtMs` in its checked-in JSON even when the
closing summary had no `Improvement note:` line or repeated a lesson already
in the journal, so the teammate file changed with nothing learned (Quality went
from cycle 20 to 21 with the same journal). `completeImprovementCycle` now
counts the cycle only when it adds a lesson the journal does not already hold
(compared ignoring case; a repeat no longer moves the old lesson to the top)
and returns whether it did;
`handleTeammateFinished` saves the brain and logs "Improvement cycle N
recorded" only then.

## The agent queue from a phone (2026-09-30)

A paired phone can set the concurrent-agent limit and reorder the waiting
agents through local control (`agents.queue.limit`, `agents.queue.move`). The
limit goes through `setAgentConcurrencyLimit`, like the tool bar's queue menu.
A move changes only the moved session's priority, so its P chip in this list
changes too. Details in
[native/DESKTOP_AGENTS.md](../native/DESKTOP_AGENTS.md#adjusting-the-desktops-agent-queue-from-the-phone-2026-09-30).

## Paused agents, and agents created through the connector (2026-09-30)

The MCP connector's `create_agent` tool
([../tools/mcp-create-agent.md](../tools/mcp-create-agent.md)) adds a row to
this list through the desktop's private agent-intake socket
(`src/AgentIntake.*`, `src/MainWindowAgentIntake.cpp`). With `paused` the row
must show up without running, and the desktop had no such state: `Queued`
always meant "start as soon as a slot frees".

A paused agent is an ordinary `Queued` session with `held` set in its
`meta.json` (`AgentSession::held`, plus `heldBy`, the agent that created it).
Keeping the status `Queued` means every list, filter, diff and branch path
treats it as a not-yet-started run with no new cases. What `held` changes:

- It is never put on `m_agentQueue`: `startAdHocAgentForRepo` and
  `startAgentOnExistingBranch` take a `held` argument and stop after creating
  the session (the ad hoc path still names the workspace first).
  `processAgentQueue` and `agentQueueOrder` skip held sessions, and
  `AgentStore::queuedSessionIdsOldestFirst` leaves them out, so a restart does
  not start them either.
- The list shows "Paused" with a grey play icon; the badge tooltip says who
  created it and how to start it. Local control's `agents.list` marks the row
  `held: true`.
- To start it: right-click the row and choose **Start now**, or select it and
  press **Start** (the Continue button, re-captioned for a paused row). Both
  call `startHeldAgentSession`, which clears `held`, saves, logs "Started by
  you", appends it to the queue and runs `processAgentQueue`, so it starts now
  or waits for a slot like any queued agent.
- A prompt sent to it starts it too (2026-10-02). The composer, Continue, the
  conflict alert and the phone all end in `continueAgentSession`, which used to
  put the session on `m_agentQueue` with `held` still set: `processAgentQueue`
  and the queue rebuild skipped it, so the message sat in the transcript under
  a clock icon and nothing ran. `continueAgentSession` now clears `held` and
  `heldBy` before it saves the session, so typing to a paused agent is the same
  as pressing **Start** with the message parked for the launch.

`held` is written only when true and read with a false default, so older
session files load unchanged and ordinary sessions write the same JSON as
before. An older desktop build reading a held record ignores the key and treats
the session as queued, so it would start it.

## Cloud runs from the queue (2026-09-27)

Queueing 83 task runs onto the runner exposed four problems.

- **Missing runs.** `GET /api/account/agent-sessions` returned only the 40
  most recently updated sessions, so with 83 queued the desktop showed 40 and
  the rest looked lost. The server now always returns every active session
  (`queued`, `running`, `security_pending`, up to `ACCOUNT_AGENT_MAX_ACTIVE` =
  500) and bounds only finished ones at `ACCOUNT_AGENT_MAX_SESSIONS` (40)
  (`_account_agent_scope_rows` in `app/src/route_handlers.py`).
- **Cloud icon.** A cloud row without attached images now shows the `cloud`
  octicon in its title cell, so runs on a runner are told apart from this
  desktop's own at a glance.
- **Notice flood and titles.** `announceCloudAgentArrivals` merged arrivals only
  within one refresh, and the queue delivered them one refresh at a time, so
  each run got its own notice. Arrivals are now collected for 10 s and shown as
  one notice ("N new cloud agent runs (x queued, y running)"). A single arrival
  says "is queued for" or "is running on" to match its status; it used to say
  "running" for queued runs. Titles lose the leading `[task:<hex>]` tag that
  task prompts carry (`cloudAgentDisplayTitle`, applied when the list is
  parsed, so rows use it too) and are cut at a word boundary with an ellipsis
  (`cloudAgentShortTitle`).
- **Move to this desktop.** The Agents toolbar has **To desktop** while any
  cloud run is still waiting for a node. It claims those runs through the
  existing "Continue on this desktop" path, oldest first, at most
  `kCloudMoveConcurrency` (6) at a time. The rest wait in the relay queue and
  are claimed as local runs finish (`pumpCloudAgentMoves`, run after every
  list refresh). A moved run neither steals the selection nor raises its own
  notice. A run the runner has already started stays there. While runs wait in
  the move queue, the runner can still claim some of them.
- **Runs hidden by owner.** For an organization repository the relay files a
  run under the node that serves it (`fmnext-n1/forkmesh`, not
  `forkmesh/forkmesh`). The Agents list shows only the open repository's
  owner and name, so the Agents badge counted 51 runs while the list showed
  about 10. `applyCloudAgentSessionSet` now files such a run under the local
  repository when exactly one local repository has that name and none has
  that exact owner, and **To desktop** can then find its working copy.

## Live output of a cloud run

A cloud row used to change only when the account list was read again (at best
every 5 s while Watch is open, `kCloudAgentWatchRefreshMs`), and only by whole
history turns. The cloud run that is open now streams its events into the
transcript as a local run does. Nothing is read for any other run. The relay
and runner halves are in
[../agents/README.md](../agents/README.md#live-output-of-cloud-runs).

- **Who is followed.** `pumpCloudAgentStream()` (`MainWindowAgentCloud.cpp`)
  follows exactly `m_selectedAgentSessionId`, and only when it is a cloud row
  whose relay status is `running`, the Agents page is visible and the window
  is not minimized. The rule is `CloudAgentStreamWatch::follow`
  (`src/AgentCloudStream.*`). A different selection, a hidden page, a finished
  run or a deleted row unsubscribes at once: the cursor and the decoded events
  are dropped, and the relay stops counting the session as watched 30 s after
  the last read, so the runner stops sending.
- **When it reads.** `GET <session route>/stream?after=<seq>`, signed with the
  session read proof (`cloudAgentReadProof`), the same as the session detail.
  It reads when the `cloud-agent-stream` doorbell rings on the account event
  socket (`MainWindowRepoSync.cpp`), at once again when the answer says `more`,
  and otherwise every 3 s (`kFallbackMs`). After a failed read it waits 10 s.
  The pump runs from the existing 1 s cloud timer
  (`installAgentActivityHost`) and after every account-list refresh. There is
  one read at a time, and an answer that arrives after the selection moved is
  discarded (`generation`).
- **Decoding.** `CloudAgentStreamDecoder` joins the base64 chunks, splits
  them into lines, and keeps each line that parses to a JSON object with a
  `type`. A line split across chunks waits for its end. After `truncated` (the
  reader fell behind the relay's 256-chunk ring) the partial line is dropped.
  A new `job` (a follow-up run), a new `stream` or `reset` starts the live run
  over. A partial line over 1 MiB is dropped.
- **Drawing.** The events go through `ClaudeTranscriptView::handleEvent(event,
  false)`, the local runs' renderer, so assistant text, tool calls, tool
  results and the closing result look the same. Each event is stamped with
  the time it arrived (`_ts`). `renderCloudAgentTranscript` draws the relay
  history and then the live events, and `m_renderedCloudStreamEvents` tells it
  whether a redraw is needed. New events are appended without a redraw. At
  most 2000 live events are kept.
- **No duplicates at the end.** Live events are drawn only while the row's
  status is `running` (`cloudAgentStreamShownEvents`). When the run finishes,
  the account list brings its final history turn, which carries the same
  transcript. The status is no longer `running`, so the redraw shows the
  history alone and the live events are thrown away.

Tests: `tests/test_agent_cloud_stream.cpp` (target
`forkmesh-agent-cloud-stream-tests`) covers decoding, gaps, restarts and the
subscribe/unsubscribe rules. `tests/validate_cloud_agent_stream_contract.py`
pins the window wiring: selection, visibility, the doorbell topic shared with
`app/src/agent_streams.py`, the timer, and the dedupe.

## One list for desktop and cloud runs (2026-09-27)

The Agents list is a single list of every session. The three-step **Cloud**
filter (all / cloud only / desktop only) is gone: each row's title cell says
where the run is with one icon, `device-desktop` in grey for this desktop's
sessions and `cloud` in blue for runs on a mirror or the runner, and the
hover text says "Desktop agent" or "Cloud agent". An attached image still
takes the icon's place. Searching "desktop" or "cloud" (or a mirror's name)
narrows the list the way the filter used to.

## List view: Compact, Zen, Full (2026-09-29)

A **View** button in the list's toolbar, beside **Teammates** and **Sort**,
names the current view and steps to the next one on each click
(`setAgentListView` in `src/MainWindowAgentList.cpp`). The choice is kept in
the `agents/listView` setting.

- **Compact** is the list as it was: the lead column (avatar, status, age,
  branch button, churn bars) and a one-line title with its priority prefix.
- **Zen** hides the lead column and draws nothing but each agent's title: no
  priority, issue number, icon or transcript-hit count.
- **Full** hides the lead column and gives each row two lines: the title,
  left-aligned, and under it the row's details as icon + one word each
  (status, provider, desktop/cloud, teammate, priority, age, conflicts or
  behind, files changed, lines, dirty, branch or worktree). The words are
  built by `agentRowChips()` into `kAgentRowChipsRole` on the title cell and
  painted by `AgentRowActionsDelegate`; an empty role means a one-line row.
  Chips that do not fit the width are dropped from the end rather than
  squeezed.

The hover controls (run toggle, priority, delete) work in every view. The
branch button lives in the lead column, so it is only clickable in Compact.
The window test `forkmesh-agent-list-view-window-tests`
(`forkmesh-window-tests --agent-list-view-only`) steps the button through all
three views; set `FORKMESH_AGENT_ROWS_SHOT=/path/prefix` to save a row from
each.

### Full view in columns (2026-09-29)

Full's detail words used to follow one another, each as wide as its word, so
"Running" and "Success", "Behind" and "Conflicts", "8m" and "16h" pushed
every later word to a different place on each row. Now they line up in
columns, and each row has three fixes:

- **Columns.** Every chip carries its slot (`AgentRowChipSlot`, the fourth
  string in the `kAgentRowChipsRole` entry). The delegate sizes each slot to
  its widest word across all rows (`chipColumnWidths`). The cache clears when
  the chips role, the rows or the order change. A row without a slot's detail
  (no teammate, not behind, clean) leaves that column empty, so later columns
  stay in place. A slot no row uses takes no space. When the widths change,
  the whole list repaints so earlier rows move too.
- **Title flush left.** The title is drawn from the chip line's left edge,
  not after the item padding from the stylesheet, so title and status glyph
  share one left edge.
- **Running is the purple fireball.** A running session's status chip was
  the blue `sync` octicon. It is now `agentRunningFireballPixmap`, the
  orbiting purple flame the lead glyph and status pill use. It is drawn 2 px
  larger on each side so the orbit reads at chip size. The spin angle is
  stored on the lead cell (`kAgentSpinAngleRole`), and
  `animateRunningAgentIcons` repaints the title cell each tick in Full, so
  the chip turns with the rest.

The same three views are on the phones (`docs/native/DESKTOP_AGENTS.md`).

### Shared with the Tasks page (2026-09-30)

The view list, its order, keys, icons, captions, tooltip wording and
accessible name moved to `desktop/src/ListViewMode.{h,cpp}`
(`forkmesh::ui::ListViewMode`; `AgentListView` is now an alias for it) so the
Tasks page's View button ([tasks-page.md](tasks-page.md#list-view-compact-zen-full-2026-09-30))
behaves the same way. The Agents button looks and reads as before.
`agents/listView` is now written only when the view changes
(`storeListViewMode`, through `forkmesh::settings::put`); before, building the
Agents toolbar wrote back the value it had just read.

### Compact rows scroll what a running agent says (2026-10-07)

The user asked for the iPhone Eagle view's scrolling line
([DESKTOP_AGENTS.md](../native/DESKTOP_AGENTS.md#eagle-view-on-ios-2026-10-01))
on the desktop, in the Classic layout's Compact view. A running agent's row
now shows its latest prose after the title, on the same line, scrolling left.

- **Where the line comes from.** `m_lastAssistantText`, the last assistant
  prose the stream gave for the session (Claude and Codex). `agentTickerLine`
  (`src/AgentTicker.cpp`) keeps its last paragraph, heading or bullet, skips
  fenced and indented code, tables, JSON and HTML, strips Markdown marks and
  links, joins it into one line and cuts it to 120 characters, as the phone's
  `AgentLines.dialog` does. `agentTickerText` caches the result per session,
  so it is recomputed only when the prose changes.
- **When.** Only in Compact, only for a running, unmerged session with
  something to say. `applyAgentRowCells` sets `kAgentTickerRole` on the title
  cell; `animateRunningAgentIcons`, the 60 ms spin tick, updates it when the
  agent says something new, so no table refresh is needed. A session that stops
  loses the role on the next refresh and its lane closes. The New layout keeps
  its list in Full under the cards, so it never shows the ticker.
- **Layout.** The title keeps its natural width, but at most what leaves an
  80 px lane, and never less than 45% of the cell; the lane starts 10 px after
  it and runs to the row's end. A row narrower than that shows the title alone.
  The text is the Full view's chip font and grey. The hover controls fade in
  over the end of the lane as they do over a title.
- **Motion.** `AgentTickerTrain` works the same way as `AgentEagleStrip` on iOS:
  the line appears at once at the lane's left edge, waits 0.5 s, then scrolls at
  80 px/s; copies follow 5 px apart until the lane is full, so it loops with
  no gap. A new line replaces only the copies that have not reached the lane
  yet, so it comes in right behind what is showing. The position comes from
  the clock, and a train that has not been painted for a minute starts over.
- **Repaint.** The delegate (`AgentRowActionsDelegate`) has its own 33 ms
  timer, started when it paints a lane. Each tick repaints only the lanes it
  painted since the last tick; when none were painted (the list is hidden,
  scrolled away or nothing runs) the timer stops. The spin tick's 60 ms would
  move the text 5 px at a time.

Test: `forkmesh-agent-ticker-tests` (`tests/test_agent_ticker.cpp`) covers the
line cleanup and the train's hold, speed, seams, wrap and line swap.

## Agent folders (2026-10-04, #1357)

Agents can be grouped into folders in the Agents list. Each repository has
its own folders. They sit at the top of the list in the order you put them
in, each followed by its agents. Agents not in any folder come after the
folders. Inside a folder, agents follow the chosen **Sort**.

- **Header row.** A folder is one row across both columns: a chevron, the
  folder glyph, its name, and on the right "N agents · N running · N unread".
  The header is painted by `paintAgentFolderRow`, which
  `AgentBranchButtonDelegate` calls for any cell that has `kAgentFolderRole`.
  The row is a two-column span, so it also paints in Zen and Full, where the
  lead column is hidden. Header items are enabled but cannot be selected, so
  bulk actions and **Delete selected** never pick up a folder.
- **Collapse and expand.** Click a header, or press Space, Enter, Left or
  Right while it is current. A double-click toggles once, not twice. While
  you search, folders with matches open and folders without matches are
  hidden. When you collapse the folder holding the open agent, its transcript
  stays open; the list does not move to another agent.
- **Drag.** The table now handles drag in every sort, not only Priority
  (`AgentPriorityTable`). Its own drop indicator replaces Qt's: a box around
  a folder header means "file it here", and a line means "insert here".
  - Drop an agent, or a selection of agents, on a folder header to file it
    there.
  - Drop agents on an agent row to file them in that row's folder, or to take
    them out of folders if that row is loose. In Priority sort, a single
    agent is also reprioritised next to the row, as before. In other sorts, a
    drop that changes nothing explains that Priority sort is needed.
  - Drop below the last row to take agents out of folders.
  - Press a header and drag to reorder the folder. Dropping on another header
    puts it above or below that folder; dropping on a folder's agents puts it
    after that folder; dropping anywhere else moves it to the end. A folder
    drag carries `application/x-forkmesh-agent-folder`. Agent drags still use
    the model's own mime data. Neither one moves table items: the drop
    rewrites the folder state and the list is rebuilt from it.
- **Right-click.** On a folder: Expand/Collapse, Select agents in folder,
  Rename folder…, New folder…, Move up/down/to top/to bottom, Expand all,
  Collapse all, Delete folder. Deleting a folder sends its agents back to the
  main list; no agent is deleted. On an agent: **Move to folder** (or **Move
  N agents to folder** for a selection) lists the folders, with a check on
  the current one, plus New folder… and Remove from folder. On empty space:
  New folder…, Expand all, Collapse all.

State lives in `AgentFolders` (`desktop/src/AgentFolders.{h,cpp}`). It is
kept as one compact JSON value in the `agents/folders` setting, keyed by
`owner/name`, holding each repository's folders in order (id, name,
collapsed) and a map from member to folder id. A member is keyed by
`agentReadKey()`, the same durable key the unread dot uses: the local
session id, `cloud:<id>` or `claude:<uuid>`. That way cloud and external
Claude Code rows keep their folder across reloads. Folder names are
whitespace-collapsed and capped at 48 characters. Folders are not sent to
the relay. Since 2026-10-06 (#1436) the phones get them in the `agents.list`
answer (`localControlAgentFolders()`), and any folder change rings
`agents.changed`. See "Desktop folders on the iOS Agents tab" in
`docs/native/DESKTOP_AGENTS.md`.

`AgentFolders::layout()` turns the sorted visible sessions into rows. Each
row is a header or a member, identified as `f:<id>` or `s:<sessionId>`. When
the identities match the table's current rows, `refreshAgentTable` updates
the cells in place (`agentTableRowsMatch`). Otherwise it clears spans and
rebuilds the rows. A row that changes between header and session gets new
items, because the two kinds use different item classes and flags. Session
rows record their folder in `kAgentRowFolderRole` for the drop logic. Header
rows keep the compact row height in Full view. Code that scans the table for
a session id skips headers, which hold `0` in `Qt::UserRole`. The
deleted-agent neighbour pick (`MainWindowAgentLaunch.cpp`) and the
first-row fallback selection now pass over headers.

Tests: `forkmesh-agent-folders-tests` covers the model: create, assign,
layout, collapse, search, reorder, rename, save/load and delete.
`forkmesh-agent-folders-window-tests` (`forkmesh-window-tests
--agent-folders-only`) runs the list end to end: header click
collapse/expand, both context menus, folder and agent drops, Zen painting
and persistence. Set `FORKMESH_AGENT_FOLDERS_SHOT=/path/prefix` to save the
header and list images.

## Sort by conflicts (2026-10-04, #1379)

The Agents list's **Sort** menu has **Conflicts (conflicting first)**, after
**Status**. It ranks each row from the cached `AgentDiffStat` the list scan
already keeps, so choosing it runs no Git: branches whose merge into base
conflicts (`conflicted`, the amber "Conflicts" chip) come first, then branches
behind base (`behind > 0`, "Behind"), then branches up to date with base, then
rows with no ahead/behind count yet (no branch, or not scanned), and merged
agents last. Ties fall back to priority, then id, like every other sort. The
conflict probe only runs on idle sessions or when asked, so a running agent
that has fallen behind sorts as behind until its branch is probed.

The window test that walks the sort menu (`test_window_resize.cpp`) seeds the
cache through `testSetCachedAgentSyncState()` and checks the order.

## What a branch is attached to (Code > Branches, 2026-09-27)

Each row in the Code branch list carries small labels for what the
branch belongs to, in the **Agent / task** column left of the branch name.
Before this, the only sign was the agent status icon.

- `Agent #N`, or `Cloud run`: the session whose `branchName` is this branch.
  When several match, the pick is the same as before: prefer one with an
  issue, then the newest.
- `#N`: that session's issue.
- `PR #N`: a pull request whose `head` is this branch (an open one first,
  then the highest number), or else the session's `prNumber`.
- `Task` / `N tasks`: organization tasks with a work item on this repository
  and branch. These come from the task catalog, from the local work-item
  cache, or from the session's `orgTaskId`.

The column's tooltip lists each link in full: agent status and provider or
teammate, issue title, pull request title and state, and task title. When
the column is too narrow, labels drop from the right. `refreshBranchLinks()`
recomputes these labels and the status icon. It runs after every branch
load, pull list load and task catalog load.

## Branch list columns (Code > Branches, 2026-09-27)

The list used to be one painted cell per row: status icon, age, worktree
glyph, file count, churn bars, a behind/ahead bar chart, the branch name and
the link labels on the right. The bar charts gave no numbers and nothing
could be sorted. It is now a table with a visible header, one column per
fact:

| Column | Shows | Sorts by |
| --- | --- | --- |
| Agent / task | agent, issue, PR and task labels | agent number, then PR, then task title; unlinked rows last |
| Branch | agent status icon, behind/conflict glyph, name | name |
| Status | `default`, `merged` (no commits of its own), `up to date`, `diverged`, `conflicts`, `remote` | text |
| Behind / Ahead | commits the default branch has that this one lacks, and the reverse | count |
| Commit | short hash of the tip commit | text |
| Message | the tip commit's subject | text |
| Author | the tip commit's author | text |
| Changes | `+added −removed N files` against the default branch | added + removed lines |
| Updated | relative age; tooltip has the date | commit time |
| Worktree | folder name of the linked worktree; tooltip has the path | text |

Clicking a header sorts ascending, then descending, then clears the sort
(`setSortIndicatorClearable`). Clearing reloads the list in its natural
order: the default branch first, then newest first. Every writer that
changes cells after the load (`refreshBranchLinks()`, the change-stats and
conflict-probe callbacks, `renderBranchesPanel()`) pauses sorting while it
walks the rows (`BranchSortPause`), because a sorted `QTableWidget` moves a
row as soon as its sort cell changes and the loop would skip rows. The
header's column menu can hide columns. Name and Message share the spare
width; the rest fit their contents and grow when stats arrive.

`testReloadBranchesPanel()` now builds the Branches page before it loads.
Since 8b6d662b7 the page is only built when it is first shown, so the window
tests' branch-list checks (#172, #185, #251, badges and layout) were reading a
table that did not exist and failing.
- **Branch button opens the website.** A cloud run's branch is on a mirror,
  not in this desktop's clone, so the Git view had nothing to show for it. For
  a cloud row the branch button (in the list and in the detail rail) now opens
  `<site>/<owner>/<repo>/compare/<base>...<branch>` in the browser, the same
  compare page the website's Agents tab links to (`cloudAgentBranchWebUrl`).
  The base is the run's `baseBranch`, then `baseRef`, then `main`. Local rows
  still open the Git view. Superseded on 2026-09-30: the button now opens the
  Git view for cloud rows too (see "Cloud agent branches synced to the
  desktop" below); the website is the fallback.
- **Task links on cloud runs.** A cloud row now links back to its task like a
  local run: `cloudAgentTaskId` reads the relay's `taskKey` (`task:<id>`), or,
  for owner-queue runs whose `taskKey` the relay leaves empty on purpose, the
  `[task:<id>]` prefix of the prompt, and `cloudAgentListEntry` puts it in
  `orgTaskId`. The detail pane's task chip and "open task" work for them, and
  a run moved here with **To desktop** or "Continue on this desktop" keeps the
  link on its local session.
- **All cloud runs move to this desktop (2026-09-27, later the same day).**
  The runner is no longer used. With `agents/cloudRunsOnDesktop` on (the
  default), every cloud run the relay lists for a repository with a working
  copy here, whatever its state, is turned into a local agent through
  `startAdHocAgentForRepo`, so it waits in this desktop's own queue under the
  running-agent limit. A run that never started gets its original prompt; one
  that ran gets `cloudAgentContinuationPrompt` (its request and conversation so
  far). The task link is kept (`orgTaskId`), and the relay copy is deleted
  (`requestCloudAgentSessionDelete`), so the web queue empties. Moved session
  ids are remembered in `agents/cloudRunsMoved` so a failed delete never
  starts a run twice. **To desktop** does the same on demand after asking.
  This replaces the six-at-a-time claim pump.
- **Runs sent to a runner stay there (2026-09-30).** Runners came back after
  the move above: the phone's "Runs on" picker and the web composer can send a
  run to the pool runner (fmnext-r1). The desktop still moved every cloud run
  it could check out, so a prompt sent from a phone to fmnext-r1 was deleted
  from the relay and started here within seconds ("Moved 1 cloud run to this
  desktop."). The relay's session list now says `onRunner` for a run whose
  record names a runner other than the core it is queued on
  (`record.runnerNode`), or whose record was created on the `runner_pool`
  route (`record.route`, stored from that date). `movableCloudAgentSessionIds`
  skips those rows, so neither the automatic move nor **To desktop** takes
  them. "Continue on this desktop" on the row still takes one over by hand.
- **Taken over from the phone (2026-09-30).** A phone's **Continue on
  desktop** on a runner run sends local control `agents.takeover`. The desktop
  continues the runner's branch (or starts from the default branch when the
  branch was never pushed), links the new agent to the cloud session through
  `webSessionId`, and drops the relay row; the phone then deletes the runner's
  session from the relay (2026-10-01). See
  `docs/native/RUNNER_AGENTS.md`, "Continue on desktop".

## Conflict button in the branch header (Code > Branches, 2026-09-28)

To resolve a conflicted agent branch, you had to go back to the Agents list
and click the alert glyph on its row. The branch diff header now has a
**Conflict** button with the same alert icon, right after `Agent #N`. It
shows when the merge-tree probe finds a conflict (the red `conflicts` in the
header title) and the branch belongs to an agent session
(`branchDiffAgentSessionId()`). Clicking it calls
`fixAgentConflictsWithAgent()`, which is what the list glyph calls. That
sends the session's own agent the "merge the base in, resolve the conflicts,
keep the build and tests passing, commit" prompt, or queues it when no run
slot is free.

A conflicted branch with no agent keeps **Fix with agent** and its
provider and model pickers, which start a new agent. When the branch has an
agent, those three are hidden, so the header shows one conflict action, not
two. `refreshBranchConflictButton()` decides which set shows. It runs from
`refreshBranchAgentButton()`, so it also updates when the agent list reloads
and a session claims or releases the branch.

The button still never appeared at first, even for a branch whose agent row
showed the alert glyph. `updateBranchDetailActions()` built the header's
conflict-cache key from a single `git rev-parse --short <base> <branch>`, but
`--short` implies `--verify` and accepts only one revision. Git exited with
"Needed a single revision", the key stayed empty, the merge-tree probe never
ran, and the header always read the branch as clean. That also hid the red
`conflicts` in the title. The header now asks for each short SHA separately.
The resulting key is the same one the branch table builds from
`%(objectname:short)`, so the two views share cached verdicts.

## Pull button highlight (Code > Branches, 2026-09-28)

The **Pull main** (or **Pull <base>**) button in the branch diff header is a
ghost button, so a branch that was behind its base looked the same as one that
was up to date. When the button is enabled (the branch has a working tree, is
not the base, is behind it and has no restored-edit conflict), it now carries
the `needsPull` property and the theme gives it an amber border and tint in
both the dark and light sheets. The property is repolished only when it
changes.

## Pull main vs. Merge & clean up (2026-09-28)

"Couldn't update 689-… from main in its worktree: fatal: Unable to write
index.." came from two merges in one worktree at the same time. **Pull main**
(`updateBranchFromBase`, busy flag `m_branchMergeFlowBusy`) ran while **Merge
& clean up** (`requestMergeWorktreeIntoMain`, `m_worktreeMergeInProgress`)
was running its own `git merge main` in the same worktree. Each flag only
blocked its own button. Git prints only `Unable to write index.` when another
process holds `index.lock`.

- Each flow now refuses to start while the other runs, with a notice. The
  merge editor (`openBranchMergeEditor`) is covered too. Both buttons are
  disabled while either flow runs.
- `runGitCaptureDirect` runs git with `GIT_OPTIONAL_LOCKS=0`, as
  `WebBranchOps.cpp` already did, so the background `git status` refreshes no
  longer take `index.lock` to write back the stat cache.
- The message drops git's own trailing period (it read `index..`).
- `Unable to write index` is deliberately **not** added to
  `isGitLockContentionError`: git can stop there after `--autostash` has
  already stashed the edits, and re-running the merge on its own is not safe.

The refusal "The checkout has uncommitted changes in files this merge
touches" was logged as an ERROR, once for each press of the button. It is the
merge working as designed, so it is now an ordinary notice
(`WorktreeMergeStep::error = false`), and it names the branch: "Can't merge
<branch> yet: the checkout has uncommitted changes …".

## Merge & clean up with an agent's uncommitted work (2026-10-14)

On 2026-10-14 a phone **Merge & clean up** of `2001-customizable-bottom-navigation`
was refused twice with "Worktree … has uncommitted changes — commit or stash
them first (they'd be lost when it's deleted)". The agent had stopped without
committing its last edits. The notice gave no file names and left the user to
commit the edits somewhere else. They committed them six minutes later, and the
merge then went through.

`mergeWorktreeIntoMain` now records, in `WorktreeMergeFlow`, which agent
sessions share the branch: whether one is still working
(`agentSessionWorkInFlight`, which sets `agentWorking`) and which session has
stopped (`idleAgentId`). When `prepareWorktreeMerge` finds the branch's
worktree dirty:

- **Merge & clean up, and the branch's agent has stopped**: the desktop stages
  everything and commits it on the branch as "Agent #N: commit the changes it
  left uncommitted", logs which files it committed and goes on with the merge.
  The worktree is removed only after this commit, so nothing is lost. If the
  commit fails, nothing is merged and the notice gives git's reason.
- **The agent is still working**: "Worktree <branch> has uncommitted changes
  (files) and its agent is still working — merge it once the agent finishes."
  Nothing is committed while an agent is still writing.
- **No agent owns the branch** (the user's own worktree), or a plain
  **Merge**, which keeps the worktree: the merge is refused as before, so the
  user's own work is never committed for them. The notice now lists the files
  (`describeDirtyPaths`, up to five) and gives the right reason: the edits
  would be lost when the worktree is deleted, or a plain merge would leave
  them out.

Tested in `checkErrorLogTriage` (`desktop/tests/test_window_resize.cpp`).

## After a merge from the Git page (2026-09-29)

Merging the branch shown on the Git page (either **Merge to main** or
**Merge & clean up**) used to leave you on another branch. Cleanup set the
browsed branch back to main, and a plain merge moved the pane to the next
branch in the list. Now, when the merged branch is the one the range page is
showing, `applyWorktreeMergeStep` switches `m_gitWorkspaceStack` to a blank
page instead. The page covers the files list, the history and the diff, says
"Merged <branch> into <base>.", and has two buttons: **Back to agents**
(`openAgentsOverview`) and **Back to branches** (`showOverviewBranches`).
Below them are the merge stats: the same certificate, landings list and
per-agent tallies as the merged-branch celebration
(`renderMergedBranchCelebration`). Before this, the stats only showed in the
diff pane, and only when the branch's row was in the Branches list for the
flash to find. The cover hid that pane, so it now reads the stats itself
(`loadGitMergedStats`) on every merge. It uses its own generation counter,
because the branch-row celebration bumps `m_mergeCelebrationGen` and would
drop the cover's read. It also keeps a snapshot of the agent sessions taken
at merge time, since cleanup deletes the merged branch's agent record. The
range buttons (`fmmergerange:`) re-read the cover's stats while it is
showing.

## Today's activity heat chart after a merge (2026-10-02)

Below the merge certificate, every post-merge page (the branch-row flash and
the Git page cover, both built by `renderMergedBranchCelebration`) now shows
**Today's activity**. It is a GitHub-style heat grid with 24 columns (the hours
of the day, 00 to 23) and 12 rows (the 5-minute slices of each hour, :00 to
:55), so each cell holds the interactions in one 5-minute slot. Next to the
grid are the day's total, the active time (slots with any input × 5 min), the
busiest hour and the busiest 5 minutes.

- **What counts** (`UserActivityLog`, `desktop/src/UserActivityLog.cpp`): an
  application-wide event filter counts every mouse press, key press, touch
  start, tablet press and wheel notch (or the start of a touchpad scroll).
  Mouse moves, releases and the stream of touchpad scroll updates are left
  out, because they would drown the real gestures. An event only counts when
  it is spontaneous and delivered to a `QWindow`. Qt sends real input to the
  window first and then passes it on through the widget tree, so this counts
  each gesture once, however far it propagates. Synthetic events sent straight
  to widgets are not counted. A double click counts as two presses.
- **Storage**: 288 counters for the current local day go in
  `<state>/user-activity-today.json`. They are written at most once a minute
  while you work, and again on quit. A new day starts from zero. Test mode and
  the offscreen/minimal platforms (window tests, the headless node) write
  nothing.
- **Drawing** (`UserActivityChart.cpp`): each cell's level is
  `ceil(4 × count / busiest slot)`, mapped onto GitHub's five greens, with
  separate steps for light and dark themes. Lightness rises with each level, so
  the order reads correctly in both themes. The slot you are in has a ring
  round it, and empty slots later in the day are faded so that "not yet" looks
  different from "idle". The lightest greens are low-contrast against the
  card, so the legend and the numbers in the side column carry the values.
  `forkmesh-user-activity-tests` drives real window input offscreen and checks
  the counting, the rollover, the file round-trip, the levels and the pixel
  colours.
- **Synced across devices** (2026-10-06). The grid adds the counts from the
  member's phones, other desktops and browsers. `syncUserActivity` sends this
  desktop's counts to `/api/accounts/activity-today` every 2 minutes while
  there is new input, and asks again when a post-merge page or the Git
  leaderboard opens. The answer's other-device counts are kept in memory by
  `UserActivityLog::setOthers`, and `today()` returns local plus others. The
  file now also keeps a random `device` id. When other devices contributed, a
  line under the total reads "on this desktop, iOS device and browser".
  Signing out drops them. See
  [activity-today.md](../app/activity-today.md).
Cleanup still runs underneath. A plain merge no longer steps to the
neighbouring branch while the cover is up.

**Back to agents** opens the agent list of the repository the merge happened
in (`openRepoAgentsList`, 2026-10-02). It used to call `openAgentsOverview`,
the rail button's route. That route keeps the selected agent, but after
**Merge & clean up** the selected agent is the one just deleted. It then fell
back to the newest agent in *any* repository, so the button usually reopened
another repository (`openRepoDetail` and its blocking git reads) and listed
that repository's agents. The list is per repository (`refreshAgentTable`
filters on the open one). The button also reloaded every session from disk
(`reloadAgents`). Now it keeps the selected agent if it is still in this
repository's list, or else selects this repository's newest agent, honouring
the brains filter. It shows the Agents tab on its list page
(`setAgentPageTab(0)`) and repaints the list from the sessions in memory,
which cleanup has already pruned. It switches pages before clearing the
cover, so the Git workspace underneath is never shown and laid out on the
way out.

The cover clears when you go back into the Git workspace by any route
(`showOverviewCommits`), and when the page is hidden by leaving it
(a non-spontaneous `QEvent::Hide` on `m_gitMergedPage`). The hide check waits
one event-loop turn, so a relayout that briefly hides the page does not
clear it. Merges started from other pages (Worktrees, agent detail, a phone)
leave the Git page alone.

### The leaderboard replaced by "too large to render safely" (2026-10-02, #1326)

The merge stats page is drawn by the diff renderer (`setDiffHtml`), which
caps every single GUI apply at `kDiffMaxUiApplyChars` (16K characters of
layout, inline images credited separately). The page has no file anchors, so
`fragmentDiffHtml` could not slice it: it was one block. On a busy day the
velocity cards, 12 per-agent tallies, 12 landing rows, share links and the
new activity heat chart passed 16K, and the whole page became "This
unstructured diff content was too large to render safely".

The page is now built as top-level sections joined by
`diffSectionBreakHtml()` (an HTML comment the renderer ignores), and
`fragmentDiffHtml` splits a document at those breaks before looking for file
blocks. Each section is complete HTML on its own, wrapped in its own padded
div by `mergePaneHtml`: the certificate (header, velocity, share buttons), the
leaderboard (per-agent tallies, in a 760px column under the certificate
instead of inside its frame), the activity heat chart, and the landing rows.
`mergeCertificateHtml` therefore returns a list: the certificate and, when
anyone is tallied, the leaderboard. The cap still bounds each section; only a
page made of sections is no longer judged as one block. The window test
checks a full day (12 tallies, 12 rows, velocity and activity) plans every
fragment within the cap and reaches the pane whole.

## Relay owner vs. local filing (2026-09-27)

"Runs hidden by owner" above files a node-owned run (`fmnext-n1/forkmesh`)
under the local repository with the same name (`forkmesh/forkmesh`) by
rewriting `CloudAgentSession::owner`. Every relay request was built from that
same field, so from then on the desktop addressed the relay under the local
owner, not the relay's. For a run the relay keeps under an owner the
`forkmesh/forkmesh` alias does not resolve to, delete, follow-up, read and
claim then answered 404 "This agent session was not found in the selected
repository." The automatic move to this desktop deletes each moved run's
relay copy, so on 2026-09-27 this produced a burst of "The cloud agent session
was not deleted" notices. Those runs stayed queued on the relay.

- `CloudAgentSession::relayOwner` keeps the owner from the relay's payload.
  `owner` stays what the Agents list files the run under. The rule now lives
  in `fileCloudAgentSessionsUnderLocalRepos` (`AgentCloudSessions.cpp`) and
  always starts from the relay's owner, so a run goes back under that owner
  once its local copy is gone.
- These use `relayRepoOwner()`: `cloudAgentSessionUrl` (read, delete,
  follow-up), the claim URL, the retry queue and the branch compare link.
  `fetchCloudAgentDetail` carries it over when it replaces a row.
- A run recorded in `agents/cloudRunsMoved` that the relay still lists is a
  relay copy whose delete failed. `moveAllCloudAgentsToDesktop` retries that
  delete once per launch (`m_cloudLeftoverDeletes`), so runs stranded on
  2026-09-27 leave the relay queue and the runner cannot start them a second
  time.
- A claimed run still reports its result under the local repository
  (`reportOrgAgentJob` uses the `RepositoryRecord`). For `forkmesh/forkmesh`
  the alias resolves to the node the claim retargeted the job to. A local
  repository under an unrelated owner would not resolve that way.

## Finished cloud runs dropping out of the list (2026-09-27)

Cloud rows vanished from the Agents list, most visibly once the queue held
more than 40 runs. Two causes:

- `_account_agent_scope_rows` read the "recent" rows as the 40 newest of any
  status. With 40 or more runs queued or running, those 40 were all active, so
  the answer carried no finished run at all, and every finished run left the
  list until the queue drained. The finished rows now come from their own
  query (`status NOT IN` the active statuses), so active runs no longer crowd
  them out.
- The desktop treats a row missing from the account answer as deleted
  elsewhere (`applyCloudAgentSessionSet`). The answer keeps only the 40
  newest finished runs, so each newly finished run pushed the oldest one out
  of the answer, and the desktop then removed it. The route now returns
  `finishedBefore` when it cut finished rows, and a finished row updated at or
  before it stays in the list (`cloudAgentBeyondListedWindow`). Unlisted rows
  that are active, or newer than the cut-off, are still removed as deleted.
  A finished run deleted on another device while it is older than the cut-off
  stays until the next launch.

Right after a launch the list still shows only the 40 newest finished cloud
runs, because the desktop keeps only rows it has already seen.

## Agents continue when their usage limit resets (2026-09-28)

A Claude Code or Codex agent that stopped on a usage limit used to stay
Failed until someone pressed **Continue**. The reset reminder
(`scheduleUsageLimitReminder`) only posted a ping.

Now, when a run fails and its `lastError` is a usage-limit refusal
(`forkmesh::teammates::usageLimitReached`), `holdAgentForUsageLimit`
(`src/MainWindowAgentUsage.cpp`) saves the session under
`agents/usageLimitAgentHold/<id>` with the reset time. This happens on the
stream path (the errored `result` event) and in `onAgentFinished`, and only
when no special agent model fallback takes over. The reset time comes from the
refusal itself (`usageLimitResumeAtMs`: "resets 5pm (America/New_York)",
"try again at …", "try again in …", or the older
`usage limit reached|<epoch>`). If the refusal names no time, it falls back
to the latest reset of the provider's exhausted windows (Fable only for a
Fable model). A one-shot timer fires a minute after that time.
`continueUsageLimitHeldAgents` then runs the session through
`continueAgentSession`, the same route as the **Continue** button, so the
running-agent limit still applies. Only the refusal is read, never the log
tail, because an agent's own output can quote these phrases.

Other triggers for the same continue:

- `notifyUsageLimitReady` (the window-reset ping) continues the provider's
  held sessions once their time has passed or no window of that provider is
  still exhausted. That covers holds with no known time.
- At deferred startup, after the queue resumes, `restoreUsageLimitAgentHolds`
  re-arms the saved holds. It continues the ones whose reset passed while
  ForkMesh was closed, and drops the holds of deleted sessions.

A held session is continued only if it is still Failed on a usage limit. A
session the user already continued, or one that later failed for another
reason, is left alone. A run that hits the limit again is held again. The
Settings checkbox **Continue agents stopped by a usage limit when it resets**
(`agents/usageLimitAutoContinue`, on by default) turns this off.

## Task mentions in transcripts link to the task (2026-09-28)

The transcript makes references clickable with `transcriptReferenceRegex()`
in `src/ClaudeTranscriptView.cpp`. It had no pattern for organization tasks.
A task id is 32 lowercase hex characters, so the commit alternative took it:
the `[task:<id>]` tag that starts every task prompt, and an agent's "created
task <id>", became `forkmesh-commit:` links that opened a missing commit.

- A `task` alternative now sits before the commit one. It matches `task:<id>`,
  `task <id>`, `task #<id>` and `task id: <id>` (any case, spaces and tabs
  only), and links them as `forkmesh-task:<id>`, with the id lowercased. A
  sentence-ending period may follow the id. A longer hex run, or a word such
  as `subtask`, still falls through to the commit link.
- Inline code links as a task when it is exactly `task:<id>`, or a bare id
  right after the word "task" ("created task `<id>`").
- A bare id with no "task" before it still links as a commit, because the
  transcript cannot tell a task id from any other 32-character hash.
- `openBodyReference()` routes `forkmesh-task:` to `openOrganizationTask()`.
  Transcript clicks reach it through `openAgentTranscriptReference()`. Before
  this, a task href fell through to `QDesktopServices::openUrl`, which did
  nothing.

`checkAgentTaskLink()` in `tests/test_window_resize.cpp`
(`forkmesh-window-tests --agent-task-link-only`) checks these forms. It also
checks that clicking a task link opens Tasks with that task selected.

## Ahead/behind and Pull main in the agent header (2026-09-30)

How far an agent's branch had drifted from its base was only in the **Diff**
row of the status pill's popup, as `↑3 ↓12`, and pulling main in meant the
Code page's **Update** button or the Git view. The top row of the agent detail
page now carries both, just left of **Branch**:

- `m_agentSyncLabel` reads `↑3 ahead · ↓12 behind main`, `↑3 ahead of main`,
  `↓12 behind main` or `up to date with main`, with the behind count in amber
  and a red `· conflicts` when the merge-tree probe found one. Its tooltip
  spells out the counts.
- `m_agentSyncButton` is **Pull main** (or **Pull <base>**), with the behind
  count as its badge, while the branch is behind and merges cleanly. It calls
  `updateBranchFromBase()`, the Git view's Pull main, which merges into the
  agent's worktree transactionally (its local edits survive) or updates a
  branch with no worktree through the checkout. A session whose base is not
  the repository's default branch merges through `updateWorktreeFromMain()`
  instead, which only needs the base name.
- When the branch conflicts with its base the button turns into **Fix
  conflicts** with the alert tint. For a ForkMesh-run session it calls
  `fixAgentConflictsWithAgent()`, the same prompt the list row's alert glyph
  and the Git view's Conflict button send. An external Claude Code session
  can't be prompted, so it opens the merge editor through
  `updateWorktreeFromMain()`.

Both read `m_agentDiffStats`, which the Agents list's background scan fills
for the open repository. `readAgentDiffStat()` now also records the base it
counted against in `AgentDiffStat::base`, so the label names the session's
own base. Cloud, merged and other-repository sessions show neither control.
`refreshAgentSyncControls()` runs from `refreshAgentDetailMeta()` and when a
scan result for the selected session lands. While a pull started here is in
flight (`m_branchMergeFlowBusy`, `m_worktreeMergeInProgress` or the worktree
in `m_worktreeGitBusy`) the button is disabled and rechecked every 400 ms; when
the flow ends, a rescan updates the counts. `testAgentSyncControls()` in
`tests/test_window_resize.cpp` checks the label, caption and badge for each
state.

## Cloud agent branches synced to the desktop (2026-09-30)

A cloud run pushes `agent/<session>` (or its named `agent/<n>-<slug>`) to the
cores. The branch button on a cloud row used to open the website's compare
page, because the desktop's clone had no copy. Now those branches are kept in
the local checkout, so the button opens the same Git view (commits, files,
diff, prompt bar following the run) as for a local run.

- **Background sync.** Every time the cloud list is applied
  (`applyCloudAgentSessionSet`), `syncCloudAgentBranches` collects the listed
  runs with an unmerged `agent/` branch whose repository has a working copy
  here (`cloudAgentLocalRepoIndex`). A run is synced again only when its
  status, `updatedAt`, `finishedAt` or ahead count changes, at most one pass
  per repository in flight and one every 20 s; a pass held back by either
  limit runs again 20 s later. Window-test builds skip it.
- **What it fetches.** `forkmesh::upstream::syncServedAgentBranches`
  (`UpstreamCheckoutSync.cpp`) runs on a worker. It tries the active relay's
  clone URL for the run's relay owner, then the repository's `cloneUrl`, then
  its local bare mirror (private repositories that are not published skip the
  network). For each source it runs one `git ls-remote` for the wanted
  branches and fetches only tips it does not have, with `--no-tags
  --no-write-fetch-head`, so no remote-tracking refs or `FETCH_HEAD` change.
- **What it writes.** Only `refs/heads/agent/*`, through `git update-ref` with
  the expected old value: a missing branch is created, a branch behind the
  served tip is fast-forwarded. A branch checked out in any worktree, or with
  local commits the served tip lacks, is left alone. Nothing is ever deleted.
- **The click.** `openCloudAgentBranch` opens the Git view at once when the
  branch is already local, and syncs it in the background; the diff re-reads
  if the branch moved while it is on screen. When the branch is not local yet
  it says "Fetching … from the mirror" and opens the Git view when the fetch
  lands. With no working copy here, or when no source has the branch, it opens
  the website's compare page as before (`cloudAgentBranchWebUrl`).
- The compare base is the run's `baseBranch` when that branch exists locally,
  otherwise the repository's default branch (`openAgentBranchInGitView`,
  shared with local runs).

Tests: `tests/test_upstream_checkout_sync.cpp` (create, fast-forward, current,
missing, diverged and checked-out branches kept, dead source skipped,
`FETCH_HEAD` untouched).

## Sending a prompt: sparkle and float instead of a card (2026-09-30)

Sending a prompt from the desktop composer no longer raises the "↗ Prompt
sent" card. The card covered the transcript right where the reply was about to
appear, and the new run already shows up in the Agents list.

- **A new agent** (the send button, local or cloud): a sparkle leaves the send
  button, arcs across the window and lands on the rail's Agents button with a
  small burst (`flyPromptSparkleToAgents`). For a cloud run it flies once the
  relay has taken the prompt; a refusal is still an error notice.
- **Add to an open agent** (the "add" button, local or cloud follow-up): the
  prompt text lifts off the composer as a small bubble, floats up over the
  transcript and drifts back down to land at its bottom (`floatPromptIntoTranscript`). The
  landing spot is the first visible of the transcript, the agent output stack,
  the branch diff and the pull detail (`quickAddFollowUpLandingView`). The
  cloud follow-up's "Follow-up queued" notice now goes to the log only.
  Since 2026-10-01 there is no bubble while the agent detail is the landing
  spot (the transcript or the agent output stack): the added prompt already
  appears in the transcript, so the float only repeated it. It still flies
  when the follow-up goes out from the branch diff or the pull detail.
- Both are `PromptFlightOverlay` (`desktop/src/PromptFlightOverlay.*`): a
  click-through, focusless child covering the main window that deletes itself
  when its animation ends (1.1 s sparkle, 1.2 s float). When the Agents button
  or the transcript is not on screen the flight rises from the composer and
  fades out instead.
- The "Started a … agent on your prompt" line is still written to the log, and
  so is the board task a new run opens (`announceOrgTaskOpened`).
- Issues filed from the composer and ActivityPub posts keep their card: they
  do not start an agent, so there is no Agents button to fly to.

Tests: `checkPromptFlight` in `tests/test_window_resize.cpp` (the sparkle aims
at the Agents button, the float rises, neither raises the prompt card, and
both overlays clear themselves).

## Merge & clean up takes cloud runs off the list (2026-09-30)

**Merge & clean up** on a cloud agent's branch merged it and removed the
worktree and branch, but the cloud row stayed in the Agents list. The cleanup
only deleted this desktop's stored sessions (`deleteWorktreeBranchAndAgentInBackground`
skips every external id), and the relay kept listing the run, so the next
refresh drew it again. A run this desktop had claimed from the queue did the
same thing one step later: its local row went, which un-hid the relay's copy
(`localSessionForCloudSession` no longer matched).

The cleanup now calls `retireMergedCloudAgentSessions` right after the merge
lands, before the background teardown. It takes off the list at once every
cloud row for that repository and branch, plus the relay run behind any local
session on the branch (`AgentSession::webSessionId`), and repaints the list.
Their session ids are remembered in `m_cloudRetiredSessionIds` and the
`agents/cloudRunsRetired` setting (the last 500), so
`applyCloudAgentSessionSet` never draws them again, even if a refresh already
in flight or a failed relay delete still lists them. Each removed cloud row
also gets the signed relay delete the row's own **Delete** sends
(`sendCloudAgentSessionDelete`, shared with `requestCloudAgentSessionDelete`),
without a question or a notice; a refusal is written to the log.

## Transcript stays where you scrolled (2026-10-01)

Scrolling up in an agent transcript used to last only until the next message:
the view jumped back to the bottom. `ClaudeTranscriptView` decided whether to
keep following (`m_stickBottom`) on every `valueChanged`, so it could not tell
a reader's scroll from the view's own moves. When a streamed row was replaced
or a terminal panel shrank, Qt clamped the scroll value to the new maximum,
and the handler took that clamp as "the reader is back at the bottom". The
next growth then pulled the reader down.

The latch now changes only on `QScrollBar::actionTriggered`, which Qt emits
for the reader's own scrolling: the wheel and trackpad, dragging or clicking
the scroll bar, and the keyboard. Programmatic moves (clamps, rebuild
restores, the load-earlier compensation) leave it alone. A user action also
stops an in-flight smooth scroll. While a jump to the latest message is still
animating, a growing transcript moves the animation's end value instead of
fighting it.

`ScrollJumpButtons` draws the down arrow centred at the bottom of the
transcript, with ▲ in the bottom-right corner. The arrow shows whenever the
reader is off the tail and returns to the newest message. Clicking it, or
scrolling back to the bottom by hand, resumes following. Its visibility
updates on the next event-loop turn, after the view has pinned itself, so the
arrow no longer flashes on every streamed row while following. When new
content arrives below a reader who has scrolled up, the arrow gains an accent
dot and its tooltip reads "New activity below. Jump to latest".
`desktop/tests/test_window_resize.cpp` covers the clamp case, the arrow and
resuming.

Since 2026-10-06 (#1633) the dot compares the content height (scroll maximum
plus page step) with the height last seen while the reader was at the tail.
It used to record the maximum again on the update that left the tail, so new
rows only counted if the range grew a second time after that. When five
turns landed in a single layout pass, the arrow showed without its dot and the
Critical Qt client contracts step failed. Using the content height also means
that shrinking the window while scrolled up no longer counts as new activity.

## Cloud runs merge, stop and open pull requests like desktop agents (2026-10-04)

Asked: "improve the experience of the cloud runners to be more like desktop so
we can see transcripts, merge branches, view diffs and manage pull requests
from mobile, web and desktop seamlessly just like a desktop agent". The phones
and the website already merged a runner's branch through the App
([../native/RUNNER_AGENTS.md](../native/RUNNER_AGENTS.md)); the desktop could
only reach it through the Git view after the branch synced, could not stop a
run without moving it here, and hid Create PR on every cloud row.

A cloud row's detail header now has the desktop agent's actions, each a signed
request to the App (`MainWindowAgentCloud.cpp`):

- **Merge & clean up** (`agentCloudMergeButton`, shown while `canMerge`):
  `POST {"action":"merge","cleanup":true,"requestId":"dk-…"}`, repeated with
  the same body every 2 s while the App answers `202 processing` (at most 90
  times, `kCloudBranchActionPolls`). The notice reads like the merge
  celebration (`cloudAgentMergeOutcome`: "Merged agent/x into main and cleaned
  up. 3 commits · 5 files +40 −2.") or names the conflicting files. While the
  session's merge is `merging` the button reads **Finish merge**, and the App
  resumes the same merge.
- **Clean up** (`agentCloudCleanupButton`, shown while the merge left the
  branch on a core): `POST {"action":"cleanup","requestId":…}`.
- **Stop**: the rail's Stop button now stops a cloud run a node has started
  (`stopCloudAgentRun`, the signed `{"action":"stop"}`); before, the only stop
  was the one inside **Continue here**, and it needed a local agent CLI.
- **Create PR** (shown while the App says `canOpenPull`): fetches the branch
  from the mirror (`syncCloudAgentBranchesIn`), opens a branch-backed pull
  request from it the way a desktop agent does (`createPullFromBranch`), then
  links the number to the run (`{"action":"link-pull","number":N}`), so the
  website, the phones and this list show **PR #N** for it. Without a working
  copy of the repository it opens the run on the website, where **Create PR**
  signs with the browser key instead. **PR #N** opens the pull request.
- The meta table shows `branch → base`, the run's changes (files and +/−) and
  a merge in progress or in conflict.
- The transcript redraws when the newest turn grows in place, not only when a
  turn is added (`m_renderedCloudTailLength`).

The merge, clean up and link writes use the new proof
`forkmesh-owner-agent-branch-v1`; the canonical binds the SHA-256 of
`action\ncleanup\ndiscard\nrequestId\nnumber`, each empty when the body
does not carry it (`cloudAgentBranchActionText`, `_owner_agent_branch_text`).
An organization's runs are still merged from the website. The POST profile in
`resources/network/endpoint-catalog.json` lists the new callers.

Tests: `desktop/tests/test_agent_cloud_sessions.cpp` (top-level `prNumber`
outliving a runner report of 0, `canOpenPull`, `canCleanup`, the signed texts
and canonical, the merge wording).

## Cloud runs: Pull main, Actions and the task after a merge (2026-10-04)

Asked next: "fully manage branches and merge directly from them like we can
on desktop so task all the way to a merged and deployed code should work as
it does on desktop including actions". A cloud row's header adds:

- **Pull <base>** (`agentCloudUpdateButton`, shown with Merge & clean up):
  the signed `{"action":"update","requestId"}`, which merges the base into
  the run's branch on a core, the cloud counterpart of the agent header's
  **Pull main**. Conflicts are listed in the notice and leave the run's merge
  state `conflict`, so **Fix conflicts with agent** applies on the phones and
  a follow-up here.
- **Actions** (`agentCloudActionsButton`, shown once merged): opens the
  repository's Actions tab on the website, where the push workflows the
  merge started on the base run (the cores run them, not this desktop).
- After **Merge & clean up** lands, the linked task is completed the way a
  local merge completes it (`completeCloudAgentTask`, the same signed
  `/api/tasks/<id>/complete` as `completeOrgTaskForSession`), with the note
  "Merged agent/x into main (abc1234). …" (`cloudAgentMergedTaskNote`). The
  task id is the run's `taskKey` or its title's `[task:<id>]` prefix.

## Continue a finished cloud run on its runner (2026-10-06)

Asked: "allow for continuing an agent on the web runner". The rail's
**Continue** never showed for a cloud row (`updateAgentActionState` needed a
positive session id, and cloud ids are `<= kCloudIdBase`), and
`continueAgentSessionInteractive` returned before its cloud branch for the
same reason. Now a finished cloud run that a node started (`promptable()` and
`cloudAgentFinished`) shows **Continue**, with the tooltip "Resume this run on
<runner>, on its branch". It sends the signed follow-up
`Continue where you left off.` (`sendCloudAgentFollowUp`), the same as the
phones' Continue. The button is disabled while that write is in flight. A run
that is still queued or running takes a typed follow-up from the composer as
before. The App side of the fix, follow-ups that no runner would lease, is in
[../native/RUNNER_AGENTS.md](../native/RUNNER_AGENTS.md) ("Continue and steer").

## Fix conflicts on a cloud run (2026-10-04, #1393)

The conflict alert on a cloud row (the list glyph, the branch header's
**Conflict** button, **Pull main** on a conflicted branch) answered "This
session belongs to another process, so ForkMesh can only watch it — resolve
the conflicts there." and did nothing. A cloud row's id sits inside the
external-session band (`kCloudIdBase` is below `kExternalIdBase`), so the
`isExternalSession()` guard in `fixAgentConflictsWithAgent()` caught it
before the cloud path was ever tried. The function now checks
`isCloudSession()` first and sends the same "merge the base in, resolve the
conflicts" prompt to the run as a signed follow-up
(`sendCloudAgentFollowUp`), the route **Continue** already takes for a cloud
row. `pullBaseIntoAgentBranch()` hands a conflicted cloud row to it too.
Outside Claude Code sessions keep the watch-only message.

Test: `desktop/tests/test_window_resize.cpp` (a cloud run no node has
started answers "No node has started…", not the watch-only refusal).

## Activity log behind a button on the merge-health line (2026-10-02)

The range pane's activity log (every git read, render and layout step of a
load) used to open as a 150-pixel panel above the diff on every load, and its
open or closed state was saved in `branchDiff/activityLogCollapsed`. It now
starts contracted on every launch and is not saved. A button at the end of the
merge-health strip, after **Practices**, reads "▸ Activity 556ms": the time
the load took (`m_branchDiffLogTookMs`, set when the whole diff is laid out),
or the live elapsed time while a load is still running. Clicking it opens the
log panel under the strip, and clicking it again or **Hide log** contracts it.
The button's tooltip names the latest log line. `updateBranchDiffLogButton`
keeps the text, the checked state and the tooltip in step with each line.

## Unread dot on each agent (2026-10-02)

Each row of the Agents list shows whether the user has seen the agent's
latest reply. An agent is unread when a turn finished (`finishedAtMs`) after
the user last opened it: the row gets a 7 px blue dot (`#58a6ff` dark,
`#0969da` light) at its left edge and a semibold title, and the hover text
adds "Unread: finished since you last opened it". Running, waiting, queued
and merged agents are never unread on their own; a new finish makes a read
agent unread again.

- **State.** `agents/seenFinishedAt` maps a row key to the `finishedAtMs`
  the user last saw: the session id for this desktop's sessions,
  `cloud:<relay session id>` for cloud runs and `claude:<uuid>` for outside
  Claude Code sessions, whose list ids change on every launch. The map is
  pruned to live sessions once it passes 400 keys. `agents/readBaselineMs`
  is written the first time the desktop runs with this feature; a finish
  before it counts as read, so an upgrade does not mark every old agent
  unread.
- **Reading.** `showAgentSession` marks the agent read when the Agents list
  is on screen in the active window (the same rule as the teammate badge),
  window activation marks the open agent read, and a row rebuilt while its
  agent is open and watched never gets the dot. `markAgentSessionRead`
  clears the row's `kAgentUnreadRole` in place and rings the phones.
- **Drawing.** `AgentRowActionsDelegate` keeps a 12 px gutter at the left
  of the title cell on every row, read or not, so titles line up; the dot
  sits in it, centred on the title line in Full. The thumbnail hit test uses
  the same shifted rect.
- **Phones.** Every `agents.list` row carries `unread`, `true` when the
  desktop counts the run unread and `false` otherwise; a row without the key
  comes from an older desktop. `agents.markRead {id}`
  marks one read (`unknown_session` for an id that is not this desktop's
  own session). See `docs/native/DESKTOP_AGENTS.md`.

The window test `forkmesh-window-tests --agent-list-view-only` checks the dot,
the payload flag, clearing on open and the dot's return after a new finish;
`FORKMESH_AGENT_ROWS_SHOT=/path/prefix` also saves `.unread.png`.

## Leaderboard button on the Git page (2026-10-02)

The page a merge leaves on the Git page (the certificate, merge velocity, the
per-agent tallies, the window's landings and today's activity) used to appear
only after a merge. The Git history header now has a **Leaderboard** button
(`m_commitsLeaderboardButton`, next to Fetch and Pull) that opens it whenever
you like. `showGitLeaderboard` puts up the same cover (`m_gitMergedPage`)
with "Merge leaderboard for <base>", where the base is the repository's
default branch. It sets `m_gitMergedLookBack`, so `loadGitMergedStats` asks
`readMergeCelebration` for the newest landing in the window
(`newestIfNone`). `renderMergedBranchCelebration(..., lookBack)` then draws it
the way the branch "no changes" pane looks back at a landing: "<branch>
already landed in <base>", not "Merged!", with "MERGED PER AGENT" tallies
that count everyone with one merge or more (`mergeCelebrationHtml`'s
`lookBack`). A window with no merges shows the range summary
(`branchNoChangesHtml`) and its range picker. The range buttons re-read the
cover as before. The phones get the same numbers through
`agentops.mergestats` ([DESKTOP_AGENTS.md](../native/DESKTOP_AGENTS.md#merge-leaderboard-2026-10-02)).


## Steady transcript scrolling (2026-10-06, #1430)

Scrolling an agent transcript used to blank it out and jump. Three causes:

- **Load earlier blanked the view.** Scrolling within 200 px of the top asks
  for earlier events, and `loadEarlierTranscriptEvents` rebuilds the whole
  transcript through `renderTranscriptForSession`. That batch hid the column
  (`beginBatchBuild`), collapsed the scroll range to 0, and restored the
  reader's place one event-loop turn after the reveal. So the reader saw a
  blank canvas, then the top of the transcript, then a jump.
  `ClaudeTranscriptView::setBatchHoldsFrame(hold, rowsLandAbove)` now covers
  the view with a grab of its last frame (`HeldFrame`, which also swallows
  wheel events) for a batch that rebuilds the transcript already on screen.
  The scroll range is left alone, and `endBatchBuild` puts the reader back in
  the same synchronous thaw that reveals the rows. With `rowsLandAbove` it
  anchors to the bottom edge (old value plus the growth in maximum);
  otherwise it keeps the old value. Same-session rebuilds pass it from
  `renderTranscriptForSession` (`rowsLandAbove` when the load-earlier request
  set `m_agentTranscriptScrollRestoreSession`), `renderTranscriptTail` and
  `prependEarlierEvents`. Opening a different session passes `hold = false`
  and keeps the blank canvas. A tail batch used to drop a scrolled-up reader
  at the top, because the collapsed range clamped the value to 0.
- **A frozen batch still re-measured the column.** The container's
  `LayoutRequest`s were swallowed, but the scroll area's own were not, so
  every slice still recomputed the range from the hidden column's
  `heightForWidth`. `ClaudeTranscriptView::event` swallows those while
  frozen, and the thaw sends one to the view as well as to the container.
  `m_batchThawing` keeps the bottom-follow latch from being recomputed off
  the range changes that happen mid-thaw.
- **Rows were revealed at about twice their height, then shrank.**
  `QLayout::totalSizeHint` asks every word-wrapped label for its height at its
  size-hint width (about 170 px). `CacheLabel` then used that cached height as
  its "close temporary height" for the real width and fixed it a few turns
  later in `processLabelRelayouts`. Measured with 60 rows: range 0 at reveal,
  6276 one turn later, 2916 settled. The estimate now starts only from the
  width the label is on screen at (`m_shownWidth`, set in a visible
  `resizeEvent`), so a label that has not been laid out yet is measured
  exactly. `addRow` sizes each new row to the column's width, and the thaw
  activates the column before showing it, so no row lays itself out at its
  default width first. A window resize still defers relayouts as before; the
  `test_window_resize` check now shows the transcript before it resizes.

## Speaker rows without bubbles (2026-10-06, #1466)

The user's and the agent's prose rows no longer sit in a tinted, edged box with
an accent spine. `RailItem` paints only the gutter portrait, and the text runs
from the gutter to the right edge with no inner padding. `RowStyle::plate` is
the colour behind a transparent portrait (it used to borrow the bubble's
tint). The portrait is clipped to a circle instead of a rounded square.

The caption beside the speaker's name shows the full date and time
("Sunday, September 13, 2026 3:07:09 PM") instead of only "3:07 PM" with the
date in a tooltip. Live Claude prose arrives as `stream_event` deltas, which
were never stamped, so its row had no time at all. `MainWindowAgentEvents`
now stamps each `message_start` stream event with `_ts`, and the view dates
the text deltas that follow by that stamp (`m_streamMessageAt`). Streams saved
before this change get their time from the stamped `assistant` event that
completes them (`stampSpeakerCaption` in `completeStreamedText`).

## Speaker portrait above the prose (2026-10-06, #1477)

The speaker's portrait moved out of the gutter and into the caption line, left
of the name and time, so a speaker row reads portrait, name and time, then the
text on the lines below. `RailItem` gives a speaker row only the command rows'
lead (`kSpeakerRailW`), so the portrait sits where a command row's first icon
does and the prose starts at that same left edge instead of indented past a
gutter. The portrait is `SpeakerPortrait`, drawn at the command badge's 20 px
by `paintSpeakerPortrait`. An agent with no name still gets the caption line,
with the portrait alone.

### Wayland crash from a caption label (2026-10-07)

`speakerCaption` made the name label with no parent and called
`setVisible(true)` on it before the caption row existed. A visible widget with
no parent is a window, so every speaker line opened a top-level window for a
moment and closed it when the label moved into the row. On Wayland, activating
that short-lived window crashed in `QWaylandXdgSurface::requestActivate`
(`QWaylandWindow::shellSurface` on a surface already gone; SIGSEGV of
2026-10-07 01:23, via `renderTranscriptSlice` → `appendAgentText` →
`speakerCaption`). The caption row is now made first and both labels are its
children. `test_window_resize` fails if building a captioned user and agent
turn shows any top-level window.

## A summary on each subagent card (2026-10-06)

Asked: "for all of the agents in the list please update the output /
transcript to show a summary for each one", meaning the agents a run starts
with the Agent tool (Task in older Claude Code), whose results come back as
task notifications. Before this, a background agent's card said only "Async
agent launched successfully…". Its report came later as a muted notice,
`Background task completed: <report>`, and Claude Code's stream-json
`task_notification` puts the whole report (6–14k characters) in `summary`,
so that notice was a wall of text. A foreground agent's report showed as two
head lines, `⋯ N lines` and two tail lines of raw text, starting with the
`[Subagent hand-back]` frame line.

- **On the card.** Each Agent card now ends with **Summary**: the report's
  lead paragraph as markdown, under 420 characters and cut at a sentence end.
  Below it, a folded **Full report** holds the whole report as markdown, and
  transcript search opens it like any `Collapsible`. A background agent shows
  "Working in the background. Its summary shows here when it finishes." until
  its notification arrives. The summary then replaces that line, and a repeat
  notification (a resumed agent) replaces the summary. A failed agent's caption
  is red **Failed · summary**; a stopped or killed one says so in the caption
  (`showAgentReport` in `src/ClaudeTranscriptView.cpp`).
- **In the timeline.** Where the notification lands, there is now one line
  instead of the report: `Agent “<description>” finished: <first sentences>`
  (`addAgentTaskNotification`).
- **Matching.** A stream `task_notification` with a `usage` object is an
  agent's report; its `tool_use_id` finds the card. Background commands and
  short agent updates carry no `usage` and still go through `systemNotice`.
  Replayed sessions read `<tool-use-id>` and `<result>` from the
  `<task-notification>` text (`transcripttools::taskNotifications`). A
  notification whose card is not loaded (earlier events not fetched yet) falls
  back to a notice: the status line plus the summary, or a Monitor's `<event>`
  text.
- **Cleaning a report** (`transcripttools::agentReport`): drops the
  `[Subagent hand-back] … The report follows:` frame and its two-space
  indent, a leading `[harness: …]` note, trailing `agentId:` lines and
  `<usage>` blocks. `reportSummary` skips headings, tables, fences, lists,
  quotes and lone bold headings to reach the first prose paragraph. A report
  with no prose keeps its first block's opening lines. `plainSummary` drops
  `**` and backticks for the one-line notices. `systemNotice` cuts any
  multi-line or over-300-character `task_notification` summary the same way.
- The phone feed (`AgentLogText::taskBlock`, see
  [../native/DESKTOP_AGENTS.md](../native/DESKTOP_AGENTS.md#task-notifications-and-rich-transcripts-on-the-run-screen))
  is unchanged.
- Tests: `testAgentReports` in `tests/test_claude_transcript_tools.cpp`
  (`forkmesh-claude-transcript-tools-tests`).

## Changed-file counts up to 999 (2026-10-06, #1515)

The agent list's changed-files count stopped at `99+`, so a branch with 150
changed files looked the same as one with 900. The count now shows the real
number up to 999 and `999+` above that, matching the pull request list. The
count column reserves the width of `999+`. The phone agent lists
(`AgentLines.count` on iOS, `AgentLineChips.count` on Android) use the same
cap, which also covers their ahead and behind counts.

## A collapsed detail pane reopens on selection (2026-10-06)

The detail pane is the collapsible second pane of the Agents splitter, so
dragging the divider to the right edge (easy to do by accident) left it at
zero width. Picking an agent then showed nothing: the pane was "shown" with
no room. `showAgentSession()` now checks the splitter, and when the detail
pane has no width it splits the list and detail space in half again. The
splitter sizes are not saved, so a restart also restored it before.


## A transcript line opens its place in the raw log (2026-10-07, #1670)

Clicking a transcript row's gutter (its dot, chevron or portrait) switches the
output to Raw and selects the line of the event that drew the row, centred in
the view. A click on the row's padding does the same; text stays selectable,
links still open, and a command header still folds its output, because those
widgets take the click before the row sees it. A drag is not a click.

`ClaudeTranscriptView::handleEvent()` keeps the event it is drawing in
`m_rowSource`, and `addRow()` hands it to the row, so a row knows only the
event that created it: a tool result drawn into its tool card points at the
tool call. Rows the host adds itself (the user's own prompt, which never
appears in the CLI stream, notices, the activity ticker) carry no event and
are not clickable.

`forkmesh::transcripttools::rawLogLineStart()` finds the line. A reloaded run's
raw log is its stored events written back out as compact JSON, so the event's
own compact JSON matches exactly. A live run's raw log is the CLI's lines as
printed, in its own key order and spacing, so the fallback looks for the
event's `uuid`, then any `id`, `tool_use_id`, `call_id` or `item_id` it carries
(Codex events have no `uuid` but share item ids with the app-server lines).
When nothing matches, because the live buffer dropped its oldest 100 KB or a
credential was redacted from the line, Raw opens at the tail as before.

## Rounded merge certificate, and a certificate for new pull requests (2026-10-07, #1714)

The merge page (certificate, merge velocity, share buttons, per-agent tallies,
today's activity and the landings list) used square boxes and a gold double
border that matched nothing else in the app. It now uses the theme's roles
(`Theme::hex`/`Theme::color`) and the redesign's shapes: one 12 px card with a
hairline border for the certificate, a 12 px tinted panel for the "Merged!"
banner, 8 px tiles for the velocity cards, 6 px pills for the share and range
buttons, and 12 px cards for the leaderboard, the activity chart and the
landings, whose rows are divided by hairlines. The current landing is a
rounded green-tinted row. Text uses the interface font (Geist in Next, the
app font in Classic) instead of the diff view's monospace; branch names stay
monospace. Every section sits in the same centred 760 px column.

Qt's rich text ignores `border-radius`, so the rounding is built in markup.
`roundedPanelHtml` lays a panel out as a 3×3 table: the four corners are
small images of a quarter of a rounded rectangle (`roundedCornerMarkup`,
transparent outside, drawn at the screen's device pixel ratio and cached), the
edges are cells with the fill colour and a one-sided `border-*`, and the middle
cell holds the content. The corner and edge cells set `font-size:1px` so the
empty cells are no taller than the corner image; without it the top and bottom
rows grow to a text line and the corners come loose from the edges. The same
corner images repeat across the page, so the document decodes each one once.
A full day (12 tallies, 12 landings, velocity, activity) still keeps every
section under the renderer's 16K layout cap (largest about 11.6K).

The font family is emitted unquoted (`certificateFontStyle`): a `&quot;`
entity carries a `;`, which ends the CSS declaration early and drops the rest
of the cell's style, padding included.

**Pull requests.** When someone opens a pull request, its Conversation tab
now starts with the same certificate: "Pull request #N opened!", the title,
the author, `head → base`, when it was opened and its churn, and a
**Pull request velocity** row (24 hours, 7 days, 30 days) counted from the
open times of the repository's pull requests (`m_currentPulls`). It is built by
`pullCertificateHtml` and shown in a `QLabel` (`pullCertificate`) at the top of
the thread while the pull is open and less than 24 hours old, so it is there
right after **Create pull request** (which opens the new pull) and when anyone
opens a fresh pull from an agent, a cloud run or another node. The thread's test
helpers only read `issueTimelineRow` widgets, so the label does not change
them.

## The detail title wraps (2026-10-07, #1731)

The agent detail title ("Agent #N · provider · prompt") used to stay on one
line, clipped at the pane edge, with a dimmed `agentPromptSecondLine` label
meant to carry the rest (#1672). That second line did not always appear, so a
long prompt stopped mid-word at the edge of the pane. `WrappedTitleLabel` is now
a word-wrapped `QLabel`: the title wraps on to as many lines as it needs, up to
`kAgentTitleMaxLines` (3), and the last line is elided when the text runs
further. The tooltip still holds the whole title. The cap is recomputed on every
resize and font change; if it is ever stale the label still wraps, so text is
never cut off at the edge. The second-line label and its theme rules are gone.

## A sent follow-up shows at once (2026-10-09)

A follow-up sent from the composer could take a while to appear in the
transcript. Three causes, three fixes:

- **Resuming replayed the transcript.** A follow-up to an agent whose CLI had
  exited is recorded as a `_local_user` row and then resumes the session.
  The launch in `MainWindowAgentCliSession.cpp` reset
  `m_renderedTranscriptSession` on every start, including a resume, so the
  new CLI's first event saw the view as stale. It cleared the transcript and
  replayed up to 300 events behind a blank canvas, and the message came back
  only when that replay finished. A resume appends to the stream, so the rows
  on screen still stand. Only a fresh start, which empties the stream, marks
  the view stale now (the `!resuming` branch above it already did).
- **A reader scrolled up did not see it land.** Sending did not touch the
  bottom-follow latch, so a reader who had scrolled up got the new row below
  the fold. The composer's "add" and `sendPromptToSelectedAgent()` now jump
  the transcript to the tail before sending. Prompts sent by automation (pings,
  teammates, phones) do not move the reader.
- **A cloud follow-up waited for the relay.** The transcript of a cloud run is
  drawn from the relay's history, so the message showed only after the POST
  and the list refresh. `sendCloudAgentFollowUp()` now adds the turn as soon
  as the request goes out. The next render from history replaces it. If the
  relay refuses the follow-up, the transcript is redrawn without it.

`desktop/tests/test_window_resize.cpp` checks that a follow-up sent while
reading earlier turns brings the view down to it.
