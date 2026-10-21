# Desktop: logs, stalls, crashes and health

Part of [the desktop docs](README.md).

## Where things are written

Paths come from `desktop/src/AppPaths.{h,cpp}`. For the default project on Linux
(`~/projects/forkmesh/`):

| File | Writer | Contents |
| --- | --- | --- |
| `data/network_log.txt` | `MainWindow::logSystem()` in `src/MainWindowSettings.cpp` | The app log shown on the **Log** page; append-only |
| rotated segments beside it | `src/NetworkLogArchive.*` | Rotated segments and a 30-day digest for the log chart; 30 days plus a byte budget |
| `logs/diagnostics/stalls.log` (+ `.1`) | `src/StallWatchdog.*` | UI freezes over the watchdog threshold with sampled stacks |
| `logs/diagnostics/actions.jsonl` | `src/StallWatchdog.cpp`, fed from `src/MainWindowDiagnostics.cpp` | Background task lifecycle: `source`, `function`, `routine`, `cwd`, `queued_ms`, `execution` (`ui-blocking`, `worker`, `async`), `outcome` |
| crash records in `network_log.txt` | `src/CrashHandler.cpp` | Written from the dying process to the log file descriptor |
| `<diagnosticsDir>/self-fix-report.md` | `src/DiagnosticsReport.*` | The last diagnostic report copied from the debug bar |

**Settings → Data** shows the resolved directories for the current OS; on
Windows and macOS read paths from there.

## Log page

- Every line goes through `MainWindow::logSystem()`. `networkLogStyleFor()`
  assigns a badge (category) from the `kNetworkLogRules` substring table in
  `src/MainWindowSettings.cpp`; the legend is in `src/MainWindowInternal.h`.
  Classification is by substring, so rewording a message can change its badge.
  A zeroed tally is not an error word: `withoutClearedTallies()` drops
  "0 failed", "no errors" and a count written after the word ("fails 0",
  "errors: 0"), so the DNS health loop's `ok (fails 0)` lines no longer badge
  ERROR. The crash handler's "termination signal survived" note avoids the
  word "fail" for the same reason: a stop deferred while an Action runs is not
  an error.
- Main categories: INFO, ERROR, GIT, WEB, STALL, BGBLOCK (a GUI-thread wait for
  background work), plus package lanes (for example **Sentry**). A
  `Push to owner/repo on branch → commit — subject` line always badges GIT,
  ahead of the error-word scan; CI results have their own `Actions:` lines.
- Live follow-tail and batched inserts (`insertLogRowsHtml`, which starts a new
  block so batches never merge into the previous row).
- Host and core node logs: [networking.md](networking.md#host-logs),
  [networking.md](networking.md#fleet-node-logs),
  [../NODE_LOGS.md](../NODE_LOGS.md).

### Repeated errors fold into one ping

- `ClientErrorReports::failureGroupKey` (`src/ClientErrorReports.cpp`) masks
  flag emoji, client addresses, object ids and standalone numbers; an HTTP
  status after `-> ` and the node name stay in the key.
- `ErrorPingGroups` maps key to ping id, count, first and last time; a group
  ends after 30 quiet minutes (`kQuietMs`) or when its ping is deleted.
- A repeat updates the ping in place (`MainWindow::foldRepeatedLoggedError`):
  `N× <title>`, `Repeated N times since HH:mm:ss. Latest: <line>`, no new flash.
  Errors a caller shows itself (`m_noticeOwnsLoggedError`) are not folded.

## Inline notices

The desktop has no toasts or floating cards; information is shown inline.

- `flashMessage(text, error, href, kind, runId)` logs the line, files a quiet
  row on the Pings page, reports an error to the relay (kind `notice`), plays
  the cue, and shows the text in the page notice strip
  (`MainWindow::showPageNotice`, `#pageNoticeArea`): `kit::InlineNotice` rows
  above the page stack, keyed to the page they were raised on
  (`currentNoticePage`: the section, or `0:<owner>/<name>:<tab>`).
- No countdown: a newer message replaces an older confirmation; errors stay
  until cleared, at most three per page (`kPageNoticesPerPage`), 60 in all.
  Rows have Copy, clear, **View output** for failed runs and an **Open**
  action (`noticeDestination`); text over 600 characters is shortened.
- One operation, one Pings row: notices raised inside a `PingThreadScope`
  share a key (`m_pingThreadKey`) and `refileThreadPing` rewrites that row in
  place instead of filing another. A branch merge (`mergePingThread(dir,
  branch)`) used to file four rows: "Merging…", "Merged… cleaning up in the
  background", "Agent session deleted…" (the cleanup's own notice, which also
  replaced the merge line in the strip) and the final outcome. Now its start
  opens a fresh thread, every merge outcome or refusal updates that one row,
  and the merge path calls `deleteWorktreeBranchAndAgentInBackground` with
  `announce=false`. Only the notice calls themselves sit inside the scope, so
  unrelated notices raised by work the merge triggers keep their own rows.
- `showLocalNotice(label, text, error, kind)` prefers the status label of the
  originating panel or dialog, falling back to the strip. `fileMessage` only
  does the bookkeeping.
- Unsolicited pings (`flashPingMessage`, `flashNotification`,
  `alertOnLoggedError`) only file a Pings row, play the cue and pulse the
  window border. Must-acknowledge pings (`dismissOnly`) keep the rail badge
  until the Pings page is seen (`markLocalPingsSeen`).
- Chat pings (`MainWindow::onMessage`, kind `chat`) are filed only for public
  channels such as `#general` and `#welcome`. Messages in private
  conversations (relay DMs, office DMs, relay private channels, private office
  rooms: `isPrivateChatConversation`) file no row and no `desktop` copy from
  their OS notification, because the ping journal (`pings.json`) is plain text
  and would keep the decrypted message after expiry, deletion and sign-out.
  `purgePrivateChatPings` drops such rows already in the journal when it loads
  and again when a private channel or office room becomes known.
- Per-kind switches live on each Pings row's gear (`pings::pingKindEnabled`,
  settings `notifications/mutedDesktopKinds`). A muted kind is filed silently
  and not forwarded to phones.
- Load status is an inline row at the top of the strip (`showLoadStatus`).

## Debug bar

- The opt-in bottom bar (`buildStatusBar`, `src/MainWindowChat.cpp`) holds
  Restart (rebuild and relaunch, with `ui/showRebuildButton`), Resize (1280x720),
  Report and Log tail.
- Status dots (`LogActivityLights`) belong to the Status Page package; each
  needs a `status-check-*` package listing it
  (`PackageRegistry::statusMonitorShows`). Custom monitors are dots
  `custom:<id>` probed with a 10 s timeout. See
  [marketplace/status-page.md](../marketplace/status-page.md).
- The footer CPU/RAM/stall indicator opens the stall details;
  `src/SystemPressure.*` reads Linux PSI to tell system-wide starvation from an
  app freeze.

### Desktop resource diagnostics

The CPU quadrant in the debug bar opens **Logs → Desktop**. The page samples
the machine every two seconds, groups CPU and resident memory by process name,
and lists the busiest processes with a short role description plus their
command, parent, owner, state, threads, age and ForkMesh ownership. Selecting
a row opens the full command line, executable, working directory, control
group, descriptors, I/O and direct children. PID 1 and the ForkMesh desktop
process are protected; stopping another process always asks first and uses the
platform's normal termination request. If the kernel denies that request, the
detail dialog requests administrator permission through PolicyKit or a masked
sudo password prompt and passes it to the privileged termination command.

#### From a phone (`diag.process`, `diag.processStop`)

The phone's **Memory** list (`diag.processes`) carries the same role
description, and tapping a row opens that process's details on the phone.
`answerLocalControlProcess()` (`src/MainWindowLocalControlDiagnostics.cpp`)
answers both methods on Linux; elsewhere they answer
`process_details_unavailable` (not `unsupported`, which the phone reads as
"update the desktop").

- `diag.process {pid}` reads `/proc/<pid>` (`desktopprocs::Sampler::readOne`)
  and replies `{pid, name, identity, description, details, stoppable, token}`.
  `identity` is the process start time in clock ticks, so a reused PID is
  told apart. `details` is `detailTextFor()` with the command redacted and the
  text capped at 64 KB. `stoppable` is false for PID 1 and the desktop itself;
  a stoppable process gets a one-use `stopProcess` confirmation bound to the
  device and to `<pid>:<identity>`.
- `diag.processStop {pid, identity, token, password?}` consumes that token and
  sends SIGTERM. When the kernel refuses (another user's process, such as
  `dockerd`), it replies `{passwordRequired, token, message}` and the phone
  asks for the computer's password; the retry runs
  `sudo -k -S -- kill -TERM <pid>` off the GUI thread with a 15 s limit. A
  wrong password asks again. Errors: `bad_pid`, `process_exited`,
  `process_changed`, `protected_process`, `process_stop_failed`,
  `bad_password`, `process_details_unavailable`.

These two methods were first answered by the Go background engine
(2026-10-15). When that engine was removed on 2026-10-16 (`0f25cb619c`) they
had no Qt handler, so the phone's process sheet showed "This desktop's
ForkMesh is too old for that" on an up-to-date desktop. They now live in the
Qt desktop.

### Diagnostic report

The Report tool copies a Markdown report meant for a coding agent.

- `src/DiagnosticsReport.{h,cpp}` (`forkmesh::diagreport`) formats a plain
  `Snapshot`; `MainWindow::diagnosticsReport` fills it from the log buffer,
  stalls, crash blocks, `NodeDiagnostics` findings, the Qt runtime snapshot
  (including most painted widgets) and process/host resources.
- Problems are ERROR, STALL and BGBLOCK lines grouped by a signature with
  numbers, hashes, UUIDs, paths, URLs and quoted text folded. Bulky sections
  come last; the report is cut at 48,000 characters.
- A baseline in `diagnostics/selfFixReportBaseline` lets the next report mark
  NEW groups, list groups that stopped, and show deltas. Right-click offers
  "Send report to a new agent" and "Start comparisons over".
- The CPU figure reuses `m_debugLastCpuPercent`; calling
  `SystemStats::cpuPercent()` again would reset its sampling window.

## Stall watchdog

`src/StallWatchdog.{h,cpp}`, started by `MainWindow` with a 500 ms threshold.

- A watcher thread notices that the GUI thread has not returned to the event
  loop, sends `SIGUSR2` to the main thread, and the handler samples the stack.
  Qt, glibc and Mesa lack frame pointers, so it scans the stack for return
  addresses into known text ranges (`safeStackScanTrace`).
- A record carries a stall id, a `likely origin:` line (first application
  frames, skipping glue via `isGlueFrame`), the blocking call, timing, a
  `system at detection` line (memory/io PSI) and a starvation verdict.
- `stalled()` is emitted after the loop resumes and carries `startedAtMs`; a
  freeze that began before a Log Clear (`m_networkLogClearedAtMs`) stays out of
  the app log but is still counted and written to `stalls.log`.
- A freeze of 5 s or more (`MainWindow::onUiStall()`) becomes one ERROR line
  with the whole report (lines joined with ` | `). It is never pinged or
  reported (`m_muteLoggedErrorAlert`) but joins the error-fixer's batch.
- `actions.jsonl` `started`/`finished` pairs give exact per-task time. A
  `BGBLOCK ... git not backgrounded` line names the GUI wait's own command,
  caller and checkout (`BackgroundBlockingSpan`).

Triage: check the starvation verdict first; group `stalls.log` by
`likely origin:` and sum milliseconds; symbolize `+0x` frames with
`addr2line -fCpe /proc/<pid>/exe` of the running binary; rank GUI-thread git
waits by summing `"execution":"ui-blocking"` `finished` records by `source`.

### Reading summed stall frames

The scan also picks up stale return addresses from calls that already
returned, so summed frame totals overstate a function's own cost.

- Anything that pumps a nested event loop (`runGitCapture`, PullStore's git
  helpers, `QDialog::exec`) is charged for every handler run inside it.
- `isGlueFrame` (origin line) and `isDispatchResidue` (`src/CodeHealth.cpp`,
  `is_dispatch_residue` in `tools/code_health.py`) skip `::eventFilter`
  frames, the frameless-resize helpers (`handleFramelessResizeEvent`,
  `resizeEdgesAtGlobalPos`, `updateFramelessResizeCursor`) and event-delivery
  instrumentation (`QtEventDeliveryProbe`, `QtRuntimeDiagnostics::noteDelivery`,
  `recordEventDelivery`). A record naming only those is unattributed.
- Before optimizing, check the worker's own `duration_ms` in `actions.jsonl`
  and aggregate a group's demangled frames before blaming its origin.

### Keeping the GUI thread free

- **No git on the GUI thread.** `runGitCapture` from the GUI thread pumps a
  nested loop. Take a snapshot, run git in a static worker through
  `runOffThread`, apply on the GUI thread after checking a generation counter
  or that the same repository is still open. Busy flags drop repeat clicks;
  mid-flow questions split the work around the dialog; Source Control actions
  run one at a time per repository. Pinned by
  `app/tests/test_qt_*_git_off_gui.py`.
- **Read ref files instead of spawning git** for branch lists, `HEAD`,
  worktrees and the mirror refs hash (`forEachRefHeadsAndTagsFromFiles`),
  falling back to git for reftable or anything unusual. Caches key on ref or
  config file signatures. `refTipFromFiles` parses `packed-refs` once per
  size and modification time instead of scanning it per lookup (the 200 KB
  file was rescanned for each branch in a loop).
- **Git timeouts.** Reads get 8 s, index-writing commands 60 s
  (`gitWaitTimeoutMs()`); agent pull capture (`captureAgentPull()`) 120 s on a
  worker. Overdue git is stopped with `forkmesh::stopProcessGracefully()`
  (SIGTERM, 2 s, SIGKILL) so it can drop its own `index.lock`; a live git's
  lock is never deleted ([git-locking.md](git-locking.md)).
  `gitFailureDetail()` falls back to stdout, where git writes some refusals.
- **Batch large widget builds** under a row and time budget
  (`kScmRangeRowsPerBatch`, `kScmRangeBatchBudgetMs`); set the row count once;
  give `ResizeToContents` tables a small `setResizeContentsPrecision`.
- **Do not show what is already shown.** `QWidget::setVisible(true)` on a
  child that was never explicitly shown or hidden runs the full show path,
  laying out its rich-text labels even inside a hidden tab. The Market filter
  (`setMarketCardShown`, `MainWindowMarket.cpp`) changes a card only when its
  explicit state differs: showing the Market after a rebuild stalled 3.7 s
  laying out every card of all three package tabs (report #5).
- **Render only the newest chat rows.** `renderConversationRows` builds rows
  from the newest message back until `forkmesh::chattranscript::Budget`
  (`ChatTranscriptWindow.h`) runs out: 120 messages or 24,000 characters of
  rendered body per batch, block messages weighed by their HTML. A
  "Show earlier messages" button above the rows adds a batch and keeps the
  reader's distance from the bottom (`m_transcriptKeepFromBottom`, dropped by
  any scroll-bar action). Switching conversation resets to one batch, and a
  live feed that grows past twice its batch while the reader is at the bottom
  is re-windowed. `#office/email-log` (277 mail-log messages, 600,000
  characters, about 9,600 colour emoji, mostly block sections) stalled 173 s
  opening and 11-34 s on each later layout pass (report #7, `STALL-…-p2003-0004`
  to `0008`). Qt shapes every colour emoji by decoding its PNG in
  `FT_Load_Glyph`, so text volume, not row count, is the cost.
- **Cache label heights per width.** The scroll area asks every row for
  `heightForWidth` and `minimumHeightForWidth` at several widths on every
  `LayoutRequest`, and `QLabel` relays its whole rich-text document each time.
  `MessageRow`'s sender and body labels (`MeasuredLabel`, `MessageRow.cpp`)
  keep the last four widths, keyed on text, font and word wrap and cleared
  on font, style, polish and contents-rect changes. On the real mail-log
  texts with block HTML (60 rows, offscreen benchmark) three layout passes
  went from 1,047 ms to 4 ms, building from 5.0 s to 2.2 s and two resizes
  from 1,454 ms to 427 ms. With both changes the whole mail-log channel went
  from 52 s to build and 3.7 s per layout pass to 374 ms and about 1 ms.
- **Paginate diff chunks on the worker.** The streamed branch diff
  (`MainWindow::renderBranchDiffPatch`) paginates each chunk with
  `paginateDiffStreamChunk` on the worker and hands the GUI thread pages
  (`appendDiffStreamPages`); fragmenting a chunk on the GUI thread stalled
  821 ms (report #5).
- **Skip no-op repaints**: compare before `setStyleSheet` or `setToolTip`; run
  spinners only while visible; read-only `QTextEdit` logs have undo off and
  append in batched edit blocks.
- **Rate-limit background passes**: the agent diff/worktree pass waits three
  times its last duration (1 s to 15 s) and re-measures only changed sessions.
- **Pull inbox and ledger writes run on a worker.** `applyPullsInboxPayload`
  queues the payload (`queuePullsInboxApply`); `applyPullsInboxEntries` runs
  every `applyRemotePull`/`applyRemoteEvent` on a `runOffThread` worker and
  `finishPullsInboxApply` acks, notifies and reloads the list in the
  background on the GUI thread. `reconcilePullLedgerFor` adopts and flips in
  its worker phase too. The two never write one repository at once: an
  inbox payload that arrives during a ledger sweep or another apply for the
  same repository waits in `m_pullsInboxApplyQueued` (merged by entry), and
  the sweep skips a repository whose inbox apply is running. Report #8
  traced most of its stalls (11 `PullStore::readPull`, the 12-34 s
  `loadAll`, `commit` and `materializePullRef` waits, and frames nested in
  their pumped git waits such as `logSystemFrom`) to `drainPullsInboxFor`
  running `PullStore::applyRemotePull` on the GUI thread, two
  `loadAllIncludingArchived` passes and a commit per pull.
- **Compile `on.push.paths` once per match.**
  `ActionWorkflow::matchesPushChangedPaths` used to build a
  `QRegularExpression` for every changed path and pattern pair; a 20,000-path
  push against 4 patterns took 1,979 ms, compiled once it takes 21.5 ms
  (report #8, `applyWorkflowScan`).

### Background file writes

`src/BackgroundFileWriter.{h,cpp}` (`forkmesh::diskwrite`) moves `QSaveFile`
commits and their fsync off the GUI thread.

- `writeLater(path, bytes)` queues on one dedicated worker; a newer snapshot
  for a waiting path replaces the older one.
- `writeNow(path, bytes)` drops any queued snapshot for the path, waits out one
  being written, then writes synchronously.
- `flush()` waits for the queue; `MainWindow::~MainWindow` calls it.
- Used by the agent ledgers, `merge-notes.json`, `diff-viewed.json` and the
  diagnostic report. `AgentStore::saveSession` skips byte-identical writes.

### Settings writes

Every `QSettings().setValue()` marks the file dirty, and the next sync rewrites
and fsyncs all of `ForkMesh.conf`; on the GUI thread under IO pressure that is
a freeze.

- `src/SettingsWrite.h`: `forkmesh::settings::put()` and `drop()` skip the
  write when the stored value already matches or the key is absent
  (`sameStoredValue` handles ini round-trips of numbers, bools and string
  lists). `drop()` sees only leaf keys; remove a group with `remove()`.
- Chat encryption state (trust records, held frames, ratchets, seen sets) has
  its own ini file in the core data folder
  ([chat.md](chat.md#keys-and-storage)); in report #5 it was two thirds of a
  1 MB `ForkMesh.conf` and doubled the cost of every unrelated write, so the
  stall origins named innocent readers such as `pingPauseUntilMs()`. The next
  largest keys are `actions/approved\v2-*` (about 95 KB, one entry per
  workflow file) and `shortcuts/runStats` (32 KB).
- Keep large or frequently rewritten state in a JSON file beside
  `ForkMesh.conf` written with `diskwrite::writeLater`; batch hot values on a
  timer flushed at quit. A worker does not help: `QSettings` shares one
  in-memory file per process.
- `QSettings::sync()` rereads and parses the whole file even when nothing is
  pending, so never call it for an unchanged value. `PackageRegistry::setPlaces`
  returns before writing or syncing when the stored places already match:
  applying the account's package list called it once per package and stalled
  the window for up to 7 s against a 1.5 MB `ForkMesh.conf` (report #4).

## Crash handling and crash repair

- `src/CrashHandler.cpp` installs signal handlers and writes a crash block to
  the log file descriptor, each part with one `write()`. A `SIGTERM` record
  has a "shutdown signal" heading and is not a crash.
- The context buffer holds 4095 bytes (`forkmesh::kCrashContextCapacityBytes`);
  `forkmesh::fitCrashContext` keeps an action run's header plus as much output
  tail as fits, from a whole line, with the command passed through `redact()`.
- While an Action runs, the handler survives a stop signal and records it
  (`forkmesh::terminationSignalReceived()`) so a stopped run is not reported as
  failed ([README.md](README.md#traps)).
- The reader (`parseCrashRecords`) tolerates app-log lines interleaved by other
  threads and reads header fields only before `context:`/`backtrace:`.
- On the next launch `src/CrashLogScan.*` scans `network_log.txt` and the
  newest rotated segments for crash blocks newer than 7 days.
  `MainWindow::dispatchCrashRepairs()` queues each unrepaired crash to every
  enabled teammate that owns error-log repair (`.forkmesh/teammates/`: the
  `error-fixer` special agent or one with an active `ERROR` monitor), ahead of
  the ten-error batch, credited against the app's own repository.
- Switch: `diagnostics/autoAgentOnErrorLog` (off on nodes that must never start
  coding agents). Watermark: `diagnostics/crashRepairObservedEpoch`.
- A run that ends without Success holds its queue
  (`LogMonitorRuntime::deferRetry`, `src/TeammateStore.h`) and does not hand
  its batch back (`MainWindow::settleWorkedCrashRepair`); only a usage-limit
  refusal requeues until the reset.
- Crashes from an older build whose signature (signal plus innermost named
  frame outside libc, `crashlog::signature`) has not recurred in the running
  build are set aside (`crashlog::supersededByNewerBuild`). Both cases log a
  "Reliability monitor" line; crash blocks stay in the log.
- Crashes reach repair only through the next launch's scan; a node that never
  restarts never dispatches them.

Crash-safety rules:

- Never hold a pointer into `m_agentSessions` or `m_streamEvents` across
  anything that can pump events; copy what you need first.
- A socket dropped during its own signal stays alive until the dispatch unwinds
  (`src/SocketRetirement.h`).
- Worker threads must not write GUI-thread diagnostics state;
  `QtEventDeliveryProbe` records only its own thread.
- Windows shown automatically use `WA_ShowWithoutActivating` (Wayland
  activation crash); widgets get a parent before they are shown.
- `DesktopDisplayReadiness` connects to the Wayland or X11 socket before Qt
  starts; a refused connection waits, then exits with code 3.
- Off-GUI work must not touch widgets after teardown
  (`forkmesh-off-thread-teardown-window-tests`).
- Transcript previews (`ThumbImage`) paint a `QImage` and create no `QPixmap`.
- Connect `QMessageBox::buttonClicked` only through
  `forkmesh::ui::onMessageBoxClicked` (`src/MessageBoxClicks.h`), which queues
  the handler. `QMessageBox` emits the signal and then reads its button box and
  calls `done()`; a handler that pumped events (a git wait, a file dialog) let
  the box's own deferred delete run first, and the click crashed in
  `QDialogButtonBox::standardButton` (SIGSEGV 2026-10-13, report #4). A queued
  handler runs after the box has finished and may delete it.
  `forkmesh-message-box-clicks-tests` reproduces the crash and fails on any
  direct connection under `desktop/src` or `marketplace/packages` (the Runners
  package's two confirmations were the last, found in report #5).
- `glib` `SIGTRAP` crashes have come from file-descriptor exhaustion.

## What is reported to the relay

The relay's error log (`/api/desktop-errors`) is for faults a release can fix.
`ClientErrorReports` holds back:

- replies to the user's own click, filed **Local only**
  (`kLocalOperationNoticeKind`, `kLocalOperationDialogProperty`);
- questions: `dialogAsksQuestion()` is true for one decision button or two
  choice buttons;
- UI freezes, user firewall decisions, git probes that answer "no"
  (`gitProbeAnswerOutcome()` records `answered (exit 1)`);
- reports the relay would refuse (`relayAccepts()`: kind `dialog`/`notice`/
  `toast`, surface `app`/`headless`, non-empty, no embedded stamp). A 4xx
  other than 408, 425 or 429 ends retries (`permanentRefusal()`).

Relay error groups key on exact text: a variable in a message makes one group
per value.

## Self-diagnostics

`src/NodeDiagnostics.*` produces findings for the report and phones.
`relay-flap` comes from `Collector::noteRelayDrop`: only loss of an established
link counts, only while the network is up, and drops within
`kRelayDropCoalesceMs` (15 s) coalesce.

## Health page (component health)

Rail **Health**. `src/CodeHealth.{h,cpp}` scans the checkout off the GUI thread
and scores each file and function, including attributed stall time;
`src/ComponentHealthBoard.*` draws a card per file;
`src/MainWindowComponentHealth.cpp` is the page; `tools/code_health.py` applies
the same rules from a shell.

**Binary size** shows the running executable as a treemap whose top-level
groups sum to the file size: app code by component, file and symbol; templates
and inline code; libraries; unnamed sections. `src/BinarySize.{h,cpp}` is a
dependency-free ELF64 reader run on a worker, using `.symtab` (else `.dynsym`)
and the link line in `build.ninja` to attribute symbols; each group keeps its
150 largest symbols. `src/BinarySizeMap.*` draws it. Non-ELF platforms show a
notice.

## Memory page

Rail **Memory**: `src/MainWindowMemory.cpp`, `src/ProcessMemory.*`,
`src/MemoryUsageChart.*`, a live breakdown of this process's mappings.
**Memory map** (`openMemoryMap()`) reuses the code map's treemap and 3D view
(`CodeMapView`, `CodeMapModel::buildTreeFromEntries`) on a snapshot taken off
the GUI thread, sized by resident, proportional, private, virtual, swapped or
in-app probe counts.

### High memory usage window

`showHighMemoryProcessPanel()` (`src/MainWindowDiagnostics.cpp`) opens without
activating when host memory crosses 85% (re-armed below 80%), or on a click of
the chart's memory quadrant. It lists the top 30 processes with Kill, Kill all
and Stop agent.

- `diagnostics/highMemoryAutoKill` (off by default) lets the automatic pop-up
  SIGTERM the largest process (never PID 1 or ForkMesh), then
  `continueHighMemoryAutoKill()` repeats each second until memory is below 85%.
- `m_highMemoryAutoKillPending` is set only by the automatic pop-up and cleared
  by `endHighMemoryAutoKill()`, so opening the window by hand never kills.

## Stats page

Rail **Stats**: `src/StatsDashboard.*`, `src/MainWindowStats.cpp`,
`src/RepoStatsStore.*`. Tile layout is persisted and has a Reset.

The phone shows every one of these tiles. It asks for them with `stats.tiles`
(`src/MainWindowLocalControlStats.cpp`), which sends each catalog tile through
`StatsDashboard::tileToJson`. A new tile in `statsTileCatalog()` also needs a
`DesktopTileEntry` in both phone apps, or
`app/tests/test_mobile_stats_desktop_parity.py` fails. See
[../native/STATS.md](../native/STATS.md).

## Background timers

**Settings → Background timers** (`src/SystemdTimers.*`,
`src/SystemdTimersPanel.*`) lists systemd user timers named `forkmesh-*` and
`fmnext-*`: description, state, next and last run, last result, command and
drop-ins. Linux only.

- **Turn off**: `systemctl --user disable --now` plus `reset-failed`;
  **Turn on**: `enable --now`; **Run now**: `start --no-block` on the service.
- Units are mostly symlinks into `~/projects/forkmesh/config/systemd`, so the
  section also lists `*.timer` files there (`forkmesh::paths::configRoot()` +
  `/systemd`) and **Turn on** re-links them by path.
- `systemctl` runs on a `QtConcurrent` worker (15 s cap); the list reloads when
  Settings is shown (at most every 30 s) and after each button. Nothing polls.
- Namespace `forkmesh::systemdtimers` (the Timers package owns
  `forkmesh::timers`).

## Testing

- Unit suites: `forkmesh-diagnostics-report-tests`, `forkmesh-settings-write-tests`,
  `forkmesh-qt-runtime-diagnostics-tests`, `forkmesh-code-health-tests`,
  `forkmesh-component-health-board-tests`, `forkmesh-binary-size-tests`,
  `forkmesh-process-memory-tests`, `forkmesh-stats-dashboard-tests`,
  `forkmesh-systemd-timers-tests` (fake `systemctl`),
  `tests/test_stall_watchdog.cpp`, and grouping/relay-flap checks in
  `tests/test_crypto.cpp`.
- Window tests: `forkmesh-window-tests` (inline notices, error alerts,
  `--error-log-triage-only`), `forkmesh-high-memory-panel-window-tests`,
  `forkmesh-off-thread-teardown-window-tests`.
- Source-text checks: `app/tests/test_qt_*_git_off_gui.py`,
  `tests/validate_error_log_regressions.py`.
- Tests that burn CPU or spawn processes must end on their own (`timeout 2 sh
  -c '...'`), not rely on the parent killing them.

## Related

- [www/docs/qt-client.md](../../www/docs/qt-client.md#logs-and-diagnostics)
- [build-and-test.md](build-and-test.md)
- [git-locking.md](git-locking.md)
