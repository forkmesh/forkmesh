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
  an error. "no new failures" counts as a cleared tally too.
- A badge set by the caller (for example a deploy line's DEPLOY) lives only in
  the style cache. When the cache is cleared, or the buffer is reloaded from
  `network_log.txt` at start, the badge comes from the text again. The deploy
  log follower therefore also stores its severity in the line's source suffix
  (`|ERROR` or `|INFO`). The deploy agent's "tests: no new failures against
  the deployed …" was showing up as an ERROR in diagnostic report #9.
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
  In the debug bar the dots carry no caption and overlap like the phone's
  footer dots (`ShellStatusDots`): each later dot cuts a 2 px gap out of the
  one before it, so the row needs no background colour to draw the ring.
  Hover a dot for its name, verdict and when it is checked next.
- The debug bar's log icons show only categories that are being recorded:
  `syncDebugActivityLightCategories()` hides a muted category (Logs > Log
  types) and every category while logging is off. It runs from
  `rebuildLogFilterButtons()`, which every on/off switch already calls.
- Every chart is the size of one quadrant of the CPU/RAM/swap/disk chart: UP,
  NET, LOG and LOOP make one 2x2 block, EVT, PAINT, UI and POOL another
  (`kResourceSparklineCompactSide`). They are too small for captions, so
  hovering a chart opens `SparklineHoverCard`: its name, the current value, a
  larger chart of the last minute, now/avg/min/peak, and the tile's diagnostic
  text. The card re-reads its tile every 250 ms while the pointer stays on it,
  so it is live, and moving to the next chart swaps it without the tooltip
  delay. The resource quadrants open the same card.
- The footer CPU/RAM/stall indicator opens the stall details;
  `src/SystemPressure.*` reads Linux PSI to tell system-wide starvation from an
  app freeze.

### Qt runtime diagnostics cost

`src/QtRuntimeDiagnostics.*` runs a 10 Hz heartbeat and a 1 Hz sample for the
session; per-event accounting (`QtEventDeliveryProbe` in
`ForkMeshApplication::notify`) is on only while the debug strip is open. Since
2026-10-22 the per-event path allocates nothing:

- Paint tallies are keyed by the receiver's class, the nearest object name
  and whether it is the receiver's own (`PaintKey`), looked up once in the
  probe's constructor. The readable name is built only the first time a key
  is seen; it used to be built (and hashed as a string) for every paint.
- The slowest receiver of each event group is copied into a fixed buffer and
  turned into a string once per sample (`publishSlowestReceivers`).
- The widget, object and timer counts come from one walk of the object tree
  done in 4 ms slices on a zero-length timer (`advanceStructureWalk`), keeping
  `QPointer`s only for the path being walked so a subtree deleted between
  slices is skipped. The whole-tree `findChildren` passes it replaced froze
  the window for 2.5 s to 85 s on a paging machine. Enabling the strip and
  **Reset** still count synchronously.

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

Between the gauges and the share strips, two cards describe the processor and
the kernel's tuning (`desktopprocs::readMachine` / `refreshMachine`):

- **Cores**: the CPU model (trademarks and "N-Core Processor" removed by
  `shortCpuModel`), physical cores and threads, the P/E split, L3 size, base
  and peak clock, then one bar per online thread. Each bar's height is that
  thread's busy share since the last sample (per-CPU lines of `/proc/stat`,
  `applyCpuTicks`), and the strip under it is the core temperature
  (green / amber from 75 °C / red from 90 °C). Threads are grouped by kind:
  `/sys/devices/cpu_core/cpus` are P-cores and `/sys/devices/cpu_atom/cpus`
  E-cores on Intel hybrid chips; other machines show one group. The header
  gives the average clock (`scaling_cur_freq`) and the package temperature.
  Temperatures come from the `coretemp` hwmon (`Package id N`, `Core N`
  matched to each thread's `topology/core_id`) or `k10temp`/`zenpower`
  (`Tctl`/`Tdie`, package only).
- **CPU and memory settings**: governor (a mix is listed per governor) and
  scaling driver with its `intel_pstate`/`amd_pstate` status, energy
  performance preference, the allowed clock range and turbo (`no_turbo` or
  `cpufreq/boost`; turbo off shows amber), pressure stall (`/proc/pressure`
  `some avg10`; amber when memory or I/O reaches 10 % or CPU 50 %),
  transparent huge pages and defrag, swappiness, zswap and swap devices,
  overcommit mode, dirty ratios, `vfs_cache_pressure`, kernel release, uptime,
  SMT and NUMA nodes. Hovering a row explains the setting.

Topology, model, cache and temperature sensor paths are read once; clocks,
governors, temperatures and settings are re-read every sample (about 200 small
sysfs reads on a 32-thread machine). Every reader takes a root folder so the
tests feed it a fake `/sys` and `/proc` tree.

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

The debug bar's Diagnose tool builds a Markdown report meant for a coding
agent. A click starts an agent with it, using the provider and model picked in
the composer (`promptAreaAgentChoice`); right-click offers "Copy diagnostic
report" and "Start comparisons over". Until 2026-10-24 this was two tools,
Report (copy) and Send diagnostic (start an agent).

- `src/DiagnosticsReport.{h,cpp}` (`forkmesh::diagreport`) formats a plain
  `Snapshot`; `MainWindow::diagnosticsReport` fills it from the log buffer,
  stalls, crash blocks, `NodeDiagnostics` findings, the Qt runtime snapshot
  (including most painted widgets) and process/host resources.
- Problems are ERROR, STALL and BGBLOCK lines grouped by a signature with
  numbers, hashes, UUIDs, paths, URLs and quoted text folded. Bulky sections
  come last; the report is cut at 48,000 characters.
- A baseline in `diagnostics/selfFixReportBaseline` lets the next report mark
  NEW groups, list groups that stopped, and show deltas.
- The CPU figure reuses `m_debugLastCpuPercent`; calling
  `SystemStats::cpuPercent()` again would reset its sampling window.
- Fleet intrusion-detection incidents (`fmnext-ids` lines whose body starts
  `alert` or `lockdown`, not replayed history) are kept by
  `logSystemFrom` in `m_securityIncidentLines` and listed as a CRITICAL finding.

#### Red Diagnose tool

`MainWindow::updateDiagnosticsAlert` runs off the one-second diagnostics timer
at most every 5 s and tints the tool red (`ActivityRailButton::setDangerTint`)
when `diagreport::alert` finds an issue. Its tooltip lists them by kind:

- Performance: 2 GB resident, CPU at 50% or more (an average over about 5 s,
  so one busy second does not flash it), host memory at 85%, the home drive at
  90%, 70% of file descriptors open.
- Security: intrusion-detection incidents.
- UI: crash blocks in the log buffer, UI stalls, event-loop lag p95 of 100 ms.

The thresholds match the WARNING findings in the report. Crashes, stalls and
incidents are events, so starting an agent from the tool acknowledges the ones
seen so far (`acknowledgeDiagnosticsAlert`) and the tool only turns red again
for new ones; the resource checks stay red while the condition holds.

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

### Stalls to an agent: one session at a time (2026-10-22)

Settings > Agents "Let special agents create repair agents from UI stall
logs" (`diagnostics/autoAgentOnStall`) lets a special agent's `STALL` log
monitor (Quality ships one, threshold 50) hand UI stalls to an agent. Stalls
used to start a fresh agent each time a monitor or the **Send to agent**
button fired; now every route adds to the session already working on stalls.

- `src/StallRepairRouting.*` picks that session (`pickSession`): the one
  remembered in `diagnostics/stallRepairSession` while it is running, queued,
  waiting, or finished successfully and not merged; otherwise the newest
  running session whose title or prompt is stall work. Merged, stopped,
  failed, external and cloud sessions, and sessions in another repository,
  never take stalls.
- `MainWindow::maybeTriggerStallMonitoringTeammates` starts a run only when
  no such session exists. While one is working, new reports are batched into
  one follow-up (`deliverStallFollowUp`, sent with
  `sendPromptToAgentSession`) at most every 10 minutes, after a 1 minute
  settle. A finished session is resumed only when the reports not yet handed
  over cross the monitor threshold again, so a burst cannot restart it per
  stall. `m_stallHandedOffThrough` counts the reports already sent;
  **Clear** resets it.
- **Send to agent** (`sendStallLogToAgent`) and the phone's per-stall send
  (`diag.run` `sendStall`) add to the open session too and start a new one
  only when there is none.
- A background (`--headless`) or offscreen instance never routes stalls to
  an agent, whatever the setting says: its stalls are mirror work, and
  agent builds there would make the next stall worse.

### Page switch timing

`src/PageSwitchTrace.h` times each switch of a rail section (`showSection`), a
repository tab (the tab bar's `idClicked`) and an agent opened on the Agents
tab (`switchToAgentsTab`, which the Agents rail button also goes through).
Nested switches count towards the outer one. One line per switch goes to the
app log and, from a one-thread pool, to `diagnostics/page-switches.log`
(emptied past 1 MB):

```
[switch] section 11 | build 84ms | sync 97ms | first paint 112ms | last paint 940ms | paints 12 | nested 0 | settled
```

- `build`: building a deferred page or repository tab.
- `sync`: the whole switch call on the GUI thread, build included.
- `first paint` / `last paint`: the first and last paint of a widget inside
  the new page, from the click. The last paint is when late data stopped
  changing the page. Paints of the focused widget (a blinking caret) are
  ignored.
- `paints`: paint events inside the page until it settled; a high count
  means the page redrew as data arrived in pieces.
- `nested`: switches started inside this one, through a nested event loop.
- Outcome: `settled` (no paint for 800 ms), `still painting` (cut off at 6 s),
  `interrupted` (another switch began) or `no page`.

The paint watch is an application event filter installed only between a
switch and its outcome. It was added on 2026-10-23 to measure the Next
design's slow tab switches (late pop-in, flicker) before changing them.

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
- `likely origin:` names a system wait first when one sits at the top of the
  first sample, before any real application frame: `Qt window flush: GPU
  buffer swap waiting on the compositor` (`swapBuffers` / `QRhi::endFrame`),
  `QSettings write flushed to disk by fsync on the UI thread`, or `Qt
  style-sheet polish` (`QCss::StyleSelector`). The application frames follow
  after `, under`, and may be stale. In the 2026-10-22 logs most "Qt widget
  paint" and "application frame unresolved" stalls were these waits, and two
  thirds of them had `io full` above 10%: machine pressure and the compositor,
  not a code path. A window holding a `QWebEngineView` flushes through OpenGL,
  so every repaint is a buffer swap.
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
- **Agents, transcript and pull list (2026-10-22 stall log).**
  `forkMeshAvatarPng` keeps the last 512 generated avatars (a mutex-guarded
  `QCache`): the pull list painted and PNG-encoded one per row on every
  rebuild (29 stalls in `refreshPullList`). A landed worktree merge records
  its pull status (`reconcileMergedPulls`, which reads every pull) in the
  merge worker (`mergeBranchIntoBase`) instead of in `applyWorktreeMergeStep`,
  still before cleanup deletes the branch. The hourly and startup agent
  recycle-bin sweep runs on a worker
  (`purgeExpiredAgentRecycleBinInBackground`); the retention control and its
  local-control method still sweep at once. `saveRecentUserInstance` writes
  only when the account's instance or its place at the top of the recent
  list changes, and `refreshQuickAddTaskAssignees` leaves an unchanged picker
  alone (its re-selection rewrote `quickAdd/taskAssignee`). The transcript
  and Agents page fixes are in
  [agents-ui.md](agents-ui.md#transcript). Still open: the stalls under
  `scanExternalClaudeSessions` wait in `pthread_create` inside
  `runOffThread`, which starts a new `QThread` per call; under memory
  pressure the kernel took over 500 ms to create one. Fixing that means
  reusing threads in `runOffThread` itself, with the same teardown guarantees.
- **Logging, package pages and mirror push (2026-10-22 stall log).** Many of
  these reports came from a process with 400 MB swapped out, where touching
  20,000 scattered strings means 20,000 page faults.
  - The in-memory log (`m_networkLog`, 20,000 lines) was deep-copied on the
    GUI thread whenever a line arrived while another copy shared it: the
    `reallocateAndGrow` frames under `logSystemFrom` (and the stale
    `accentForBadge` and `drainCoreLogFeedLines` frames above them) are that
    copy, which bumps every line's reference count. The global search and the
    search page now hand their snapshot to the worker, which copies it there
    and drops the shared one (`networkLogShare`,
    `SearchPageInputs::LogSnapshot`); the saved-search check, which never
    searches the log, drops it first thing. `logSystemFrom` reserves room for
    twice the limit, so trimming from the front slides the array instead of
    reallocating. The popout's fill copy (`m_logPopoutFillHistory`) still
    shares the buffer while it fills.
  - `LogActivityLights` looks badges up in a table (`categoryLanes`) rebuilt
    when a category is registered, instead of comparing against 40 names per
    call. The footer tail renders only the last `kFooterLogSeedLines` of a
    batch that would scroll the rest out anyway.
  - `syncNativePackagePageSearch` calls a package's `setHostSearchQuery` only
    when its query changes, and never for a hidden page that has not been
    given one: every navigation re-sent `""` to every page and Computer
    Security re-rendered its tree each time. `preparePackagePages` and
    `preparePackageRails` prepare one package per event-loop turn
    (`drainPackagePreparation`) instead of all CRM, Marketing and other
    loads in one call.
  - `PackageRegistry::setPackageSetting` returns before `sync()` when the value
    is unchanged (`applyStatusMonitorsPayload` rewrote `ForkMesh.conf` on every
    status-monitor poll).
  - Ordinary managed-host pushes list the retained release CAS on a worker
    (`retainedManagedPushArtifacts`) and start artifact reconciliation when
    it returns; the ref push no longer waits for the directory walk or builds
    per-blob SSH commands it never uses. Release pushes still check their
    signed blobs first.
  - `deleteCancelledRunsAutomatically` reads its setting only when a run was
    cancelled; `cancelSupersededRuns` called it with an empty list on every
    run and waited on the `QSettings` lock.
  - The user activity log writes through `diskwrite::writeLater` once a
    minute and `writeNow` at quit, instead of a `QSaveFile` commit on the GUI
    thread from the input event filter.
  - The branch diff re-scores merge health on a worker when a patch over the
    synchronous render limit (`kBranchDiffSyncRenderChars`) gets a new Git
    verdict (`updateBranchDiffHealth`); the secret scan of a large patch was
    the `findSecretsInDiff` stalls.
  - The first network request waited for libproxy to load
    (`systemProxyForQuery` under `refreshRepoPinBanner`); the window now
    makes one proxy lookup on a worker at start.
  - No change: `ActivityTally::noteStatement` appears only in starved
    reports, and the `noteBackgroundActivity` frames are stale return
    addresses whose leaves were GL buffer swaps, text layout and
    `purgeSessionState`.
- **Theme switch, pull list and settings writes (report #9, 2026-10-24).**
  The app was still running a binary built on 2026-10-23 02:57, before the
  avatar cache above, so its `forkMeshAvatarPng` stalls were already fixed in
  source.
  - Choosing a theme or interface from the View menu froze the window for
    16 s. Qt 6.8's `QApplication::setStyleSheet` repolishes every widget that
    has cached style rules and then recurses into each one's children, so a
    widget is restyled once per styled ancestor. `MainWindow::applyTheme` now
    leaves an unchanged sheet alone. To change it, the app clears the sheet
    and sets the new one, which goes through `QApplication::setStyle` (the
    same path startup takes) and visits each widget once per pass. Offscreen
    with 5,600 widgets: 83,000 `StyleChange` events (up to 21 on one widget)
    and about 5.6 s per switch before; 11,700 events and 0.8 to 3.3 s after,
    depending on machine load
    (`forkmesh-window-tests --theme-switch-only`).
  - `refreshPullList` checked each row for running checks by scanning every
    action run (about 7,600); it now collects the running pull numbers once
    per rebuild (`pullsWithRunningChecks`).
  - The Genie spider saved its spot each time it finished a wander, every
    5 to 24 s, and the repository page saved the open tab on each switch,
    both through a static `QSettings`. See
    [Settings writes](#settings-writes) for what each does now.
  - No change: the `ActivityRailButton::paintEvent` and "Qt widget paint"
    stalls wait in the GPU driver (`amdgpu_bo_free`, `QRhi::endFrame`) right
    after the theme switch. The rich-text stalls run inside a layout pass
    with no app frame. The `ensureSectionBuilt` stall sizes a
    `ResizeToContents` table, but its action record had rotated out, so the
    page could not be named. `saveMutedLogCategories`, `saveRepositories`
    and `deleteCancelledRunsAutomatically` waited on the `QSettings` lock or
    on `fdatasync` while the disk was busy (io full 55 %).
  - Report #10, taken 14 minutes later from the same old binary while a build
    kept the disk 30 to 88 % busy, added writes that waited on the disk:
    - Action run output and the ping-fix ledger: see
      [Background file writes](#background-file-writes).
    - `savePromptOverlayPlacement` writes its five keys with `settings::put`,
      so an unchanged placement no longer rewrites `ForkMesh.conf`.
    - Already fixed in source by `19eee71df3`, which the running binary
      predates: the chat ratchet save (`ChatDevice::saveRatchets`, 14 stalls
      of up to 7.2 s) and `UserActivityLog::flush`.
    - No change: `maxRunningAgents` and `storeRepositoryRecords` read or
      write settings that are rarely changed, and they waited on the
      `QSettings` lock while the disk was 75 to 88 % busy.
  - Still open: four workers write `ForkMesh.conf` and take the process-wide
    `QSettings` lock while they sync. They are the served-mirror-head scan
    (`MainWindowActions.cpp`, `setValue` plus `sync()` under
    `QtConcurrent::run`), `scanExternalActionsSources`,
    `writeQueuedApprovals` (`ActionStore.cpp`) and RoomSeal's `storeValues`.
    A GUI-thread `QSettings()` waits on the lock for as long as their fsync
    takes.
- **Idle frame rate, teammate output and firewall defaults (report #12,
  2026-10-24).** The report showed 86 % CPU and about 2,500 paint events a
  second with nobody using the window. Sampling the GUI thread with gdb
  (40 stacks, `SIGWINCH` as the stop signal so a stray one is harmless) put
  23 of them in `QWaylandWindow::waitForFrameSync` under `swapBuffers`, and a
  breakpoint on `QWidgetRepaintManager::sync()` counted 59 frames a second.
  Breaking on the first `QWidget::update()` after each frame named what kept
  it going: the top-chrome Agents squares, the Actions strip and the Genie
  spider, each on its own timer.
  - `AgentDotMatrix` repainted from two unaligned timers: its own 60 ms sweep
    and the 45 ms agent scanner tick, whose decaying activity meter changes
    every beat, so `setDots` called `update()` 22 times a second. A change that
    only moves a running square's meter is now stored for the next sweep frame
    (`sameSquares`). The scanner tick calls `refreshAgentDots()`, which leaves
    out `scheduleSessionsPageRefresh()`: the Sessions page does not read the
    meter, and every beat re-queued its rebuild.
  - The Agents squares and the Actions strip share one 60 ms clock
    (`ChromeDotGrid::animationClock`, held through `holdAnimationClock` while
    something runs), so their repaints land in the same frame. The strip used
    40 ms while badges breathe and 90 ms for running cells; its phase steps
    were scaled to keep the same speeds.
  - `teammateLatestOutput` read and parsed the whole `events.jsonl` of a
    teammate's session on the GUI thread for each Team page row (1.3 s stall
    under `loadTeammatesPanel`; event files reach 15 MB). It and the
    teammate-creator result now read the last 512 KB
    (`AgentStore::loadRecentEvents`, `kLatestOutputTailBytes`), where the
    latest assistant text is.
  - `defaultApprovedFirewallRules()` parses the shipped rule list once; the
    firewall page asked for it on every rebuild.
  - No change: the `KebabHeaderView::sectionSizeFromContents` stalls under
    `renderUsersPage` sampled FreeType loading a font's tables for the first
    time. The `QSettings` fsync stalls under `syncMirrorActionsConfiguration`
    and `noteEventTiming` are the worker-held lock described above. The 33 s
    theme switch ran inside `QApplication::setStyleSheet` on about 8,000
    widgets while a CI build loaded the machine; its binary was rebuilt
    before the report, so the click could not be traced further.
- **Data tables, action output bursts and firewall defaults (report #13,
  2026-10-24).** The running binary was built from current source.
  - An 8.6 s paint (`STALL-20261024T214153582-p2840065-0006`) and two
    `KebabHeaderView::sectionSizeFromContents` stalls of about 650 ms were
    Qt laying out the whole text of one cell in a `dataTable`
    (`DataTableRowDelegate::paint`, then `viewItemSize` and
    `QTextLine::layout_helper`). Eliding still shapes every glyph of the
    full string, so one very long value cost seconds on every repaint and
    every column fit. The delegate now cuts the display text to
    `kit::kCellDisplayChars` (512) characters plus an ellipsis in
    `initStyleOption`, which `paint` and `sizeHint` both use. The item keeps
    its full text. The export and import value rows also cut their text and
    their tooltip (2,000 characters), because a tooltip lays out its whole
    string too. `forkmesh-ui-kit-tests` measures a 2,000,000-character cell.
    The stack also showed `QFontEngineMulti` fallback lookups. If a later
    report shows this stall with short cells, the cause is the first
    fallback-font load for a character the interface font lacks.
  - `ActionRunner::emitProcessOutput` rebuilt the crash context for every
    output chunk: it redacted the 12 KB output tail and the command, then
    formatted the header (684 ms during a CI build,
    `STALL-…-p2840065-0004`). Output now starts a 250 ms single-shot timer,
    so a burst of output costs one rebuild. A phase change, the end of the
    run and an interruption still update or clear the context straight
    away, and they stop the timer. A crash during a burst loses at most the
    last 250 ms of the output tail. `forkmesh-action-security-tests` feeds
    500 chunks and checks there is no rebuild until the timer fires.
  - `defaultApprovedFirewallRules` opened and parsed the bundled
    `:/firewall/default-approved.json` on every Firewall page refresh. The
    resource cannot change while the app runs, so it is parsed once.
  - No change: `refreshAgentDotMatrix` (591 ms, sampled at
    `agentStatusIconColor`) already skips unchanged rows (adhoc #2104). The
    sample's other frames were stale, so the time could not be pinned to
    the loop. The two compositor buffer-swap stalls wait on Wayland.

### Background file writes

`src/BackgroundFileWriter.{h,cpp}` (`forkmesh::diskwrite`) moves `QSaveFile`
commits and their fsync off the GUI thread.

- `writeLater(path, bytes)` queues on one dedicated worker; a newer snapshot
  for a waiting path replaces the older one.
- `writeNow(path, bytes)` drops any queued snapshot for the path, waits out one
  being written, then writes synchronously.
- `flush()` waits for the queue; `MainWindow::~MainWindow` calls it.
- Used by the agent ledgers, `merge-notes.json`, `diff-viewed.json`, the
  diagnostic report, the ping quick-fix ledger (`PingFixState::save`) and the
  saved searches after a search or alert run (`SavedSearchStore::saveLater`;
  `save()` goes through `writeNow`, so a later click never lands under an
  older queued snapshot). `AgentStore::saveSession` skips byte-identical writes.

The agent transcript writers (`AgentStore`) append through
`forkmesh::OrderedAppendWriter` (`src/OrderedAppendWriter.h`), one ordered
writer thread per lane with `flushPath()` for readers.

Action run files (report #10) have their own ordered writer in
`src/ActionStore.cpp`, because a log append is not a whole-file snapshot and a
deleted run must not come back:

- `appendLog` and `saveRun` queue on one worker thread and return at once; the
  GUI thread makes no file call. Appends to the same log are concatenated,
  a newer `meta.json` replaces a queued one. The worker resolves the run folder
  (canonical, else a matching legacy folder) when it writes.
- `readLog` returns the file plus the queued tail. It waits only while that
  same log is being written, and reads the file without the queue lock,
  retrying when a write of that log finished in between.
- `deleteRun` and `deleteRunsInBackground` drop the queued writes of the doomed
  folders and wait out one in flight, so the worker cannot recreate them.
- `ActionStore::flushWrites()` waits for the queue: the first run-cache scan,
  `MainWindow::~MainWindow` and a `qAddPostRoutine` hook call it.
- Why: under IO pressure (io full 30 to 38 %) the kernel throttles every
  writer that dirties pages, so `write()` in `appendLog` (`ActionRunner::emitLog`,
  9 stalls up to 1.3 s) and `close()` after the truncating rewrite of
  `meta.json` (`ActionRunner::setStepStatus`, `cancelSupersededRuns`) blocked
  the GUI thread. `forkmesh-action-security-tests` covers ordering, the queued
  tail and deletion.

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
- Report #10, measured on the running app (`strace -k -e fdatasync -p`, and a
  3-minute watch of which keys changed in `ForkMesh.conf`): 36 rewrites in 3
  minutes, each two `fdatasync` calls (`ForkMesh.conf.lock` from `QLockFile`,
  then the `QSaveFile` temp), 0.1 to 7 s apiece under load.
  - A long-lived `QSettings` (the `static` ones in `genieSettings()` and
    `MainWindow::recordNavLocation`) is not a buffer. Its `setValue` puts the key
    in the in-memory file every `QSettings` in the process shares; the sync
    runs from its own `QSettings::event` a moment later, or from the
    constructor of the next `QSettings` anywhere on the GUI thread
    (`initAccess` syncs). That is why stalls named readers such as
    `maxRunningAgents()`, `defaultAgentProvider()` and
    `disabledAgentToolFamilies()`.
  - A sync on a worker (approvals, served mirror heads) holds that shared
    file's mutex through the write and the fsync; a `QSettings()` constructed
    on the GUI thread meanwhile waits in `QBasicMutex::lockInternal`
    (`deleteCancelledRunsAutomatically`, sampled 560 ms).
  - The top writer was the Genie spider saving `genie/spot` on every wander
    ([genie.md](genie.md#the-spider)), 16 of the 36; it now saves only a spot
    the user chose. The rest: `actions/approved\v2-*` (3, 95 KB each),
    `repositories/items/*/lastSyncMs` (3, already batched),
    `repositories/lastOpenDetailTab` (2), one each of
    `stats/connectionTotalMs`, `actions/servedHeads/*` and
    `ui/railClickCounts`.
  - `repositories/lastOpenDetailTab` stays in memory
    (`m_lastRepoDetailTabPending`) and is written by
    `flushDeferredSettings()` from `closeEvent` and the destructor.
  - Not done yet: moving `actions/approved\v2-*` into its own file would take
    a quarter of `ForkMesh.conf` and its worker-held mutex off every read;
    it needs a migration of security-relevant approval records.
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
Hello and presence frames carry them as `dg` to everyone in a shared room, so
the wire form (`toJson`) holds only ids, severities and counted messages. The
newest error log line (last 80 characters) is kept in `Finding::detail`, which
`toJson` never sends; `detailText` shows it on this desktop's own diagnostics
and report. Before 2026-10-22 the `log-errors` message embedded that line, so
paths, hostnames or tokens in a log error reached every room member.
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
  `forkmesh-font-sizing-tests`, `forkmesh-ui-kit-tests`,
  `forkmesh-systemd-timers-tests` (fake `systemctl`),
  `tests/test_stall_watchdog.cpp`, `forkmesh-stall-repair-tests`
  (`tests/test_stall_repair_routing.cpp`), and grouping/relay-flap checks in
  `tests/test_crypto.cpp`.
- Window tests: `forkmesh-window-tests` (inline notices, error alerts,
  `--error-log-triage-only`; `--theme-switch-only` counts `StyleChange`
  events per theme switch; `--deploy-log-badges-only`; `--repo-lazy-only`
  also checks the deferred last-tab write),
  `forkmesh-high-memory-panel-window-tests`,
  `forkmesh-off-thread-teardown-window-tests`.
- Source-text checks: `app/tests/test_qt_*_git_off_gui.py`,
  `tests/validate_error_log_regressions.py`.
- Tests that burn CPU or spawn processes must end on their own (`timeout 2 sh
  -c '...'`), not rely on the parent killing them.

## Related

- [www/docs/qt-client.md](../../www/docs/qt-client.md#logs-and-diagnostics)
- [build-and-test.md](build-and-test.md)
- [git-locking.md](git-locking.md)
