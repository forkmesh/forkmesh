# Desktop: logs, stalls, crashes and health

Part of [the desktop docs](README.md).

## Where things are written

Paths come from `desktop/src/AppPaths.{h,cpp}`. For the default project on Linux
(`~/projects/forkmesh/`):

| File | Writer | Contents |
| --- | --- | --- |
| `data/network_log.txt` | `MainWindow::logSystem()` in `src/MainWindowSettings.cpp` | The app log shown on the **Log** page; append-only |
| rotated segments beside it | `src/NetworkLogArchive.*` | Rotated log segments and a 30-day digest used by the log chart; retention 30 days plus a byte budget |
| `logs/diagnostics/stalls.log` (+ `.1`) | `src/StallWatchdog.*` | UI freezes over the watchdog threshold with sampled stacks |
| `logs/diagnostics/actions.jsonl` | `src/StallWatchdog.cpp` (path beside `stalls.log`), fed from `src/MainWindowDiagnostics.cpp` | Background task lifecycle: `source`, `function`, `routine`, `cwd`, `queued_ms`, `execution` (`ui-blocking`, `worker`, `async`), `outcome` |
| crash records in `network_log.txt` | `src/CrashHandler.cpp` | Written from the dying process to the log file descriptor |

**Settings → Data** shows the resolved directories for the current OS. On
Windows and macOS, read paths from there rather than assuming Linux ones.

## Log page

- Every line goes through `MainWindow::logSystem()`; `networkLogStyleFor()`
  assigns a badge from the `kNetworkLogRules` substring table
  (`src/MainWindowSettings.cpp`); the legend lives in
  `src/MainWindowInternal.h`. Changing a message's wording can change its
  badge: classification is by substring.
- A `Push to owner/repo on branch → commit — subject` line always badges
  **GIT**, ahead of the error-word scan. The commit subject is somebody's
  message, not a diagnosis: the push of `Merge 423-fix-failed-load-extension
  into main` was painted red (ERROR) only because the branch name spells
  "failed", while its CI run passed. A push's CI result is reported on its own
  `Actions:` lines. The rule needs the ` on … → ` shape of that notice, so a
  line such as `Push to mirror7 failed: connection refused` still badges ERROR.
- Package-contributed lanes exist (for example the Sentry package adds a
  **Sentry** tab and lane).
- The Log page has a Live follow-tail button and batches live Activity lines.
- Rows are inserted with `insertLogRowsHtml` (`src/MainWindowSettings.cpp`).
  `QTextCursor::insertHtml` merges a fragment's first block into the block at
  the cursor, so each batch's first `<div>` landed on the end of the previous
  row (`...cpp:2961+ 20:54:35 WEB Node n2 ...`), and a history segment loaded
  at the top merged its last row into the first shown one. The helper starts
  a new block first when the edge block is not empty.
- Host logs: see [networking.md](networking.md#host-logs).
- Core node (Next n1-n3) journals and rqlite statements: see
  [networking.md](networking.md#fleet-node-logs). Every node journal line, every
  Caddy request and every deploy line (`Deploy <kind>: ...`) reaches the Log
  page since 2026-09-26 ([../NODE_LOGS.md](../NODE_LOGS.md)).

Audit of the badge set: [www/docs/engineering/network-log-badge-audit.md](../../www/docs/engineering/network-log-badge-audit.md).

### Repeated errors fold into one ping (2026-09-28)

Every ERROR line still lands in the Log with its badge, but repeats of one
failure now share one ping. During the 2026-09-28 core divergence the fleet
log feed turned each Caddy `err: GET app.forkmesh.com/.../info/refs -> 503`
line into its own error ping: 83 of the 100 journal slots in `pings.json`,
plus 8 "Errors are arriving faster than they can be shown" notices, with
`Mesh converge ... fetch from every mirror failed` and `Deploy publisher:
gateway cores: FAILED, N ref(s) updated, M rejected` repeating every cycle.
`alertOnLoggedError` only suppressed the *exact* text for 15 s, and every
access line differs by client address and flag.

- `ClientErrorReports::failureGroupKey` (`src/ClientErrorReports.cpp`) is the
  grouping key: the message with flag emoji removed and client addresses
  (IPv4, IPv6, `(Cloudflare)`-style labels), object ids and standalone numbers
  masked. A number right after `-> ` (the HTTP status) is kept, so one
  host/path/status is one group and a 502 is not a 503. The node name
  (`Node n2`) stays in the key.
- `ErrorPingGroups` holds key → ping id, count, first and last time. A group
  ends after 30 quiet minutes (`kQuietMs`) or when its ping is deleted.
- A repeat updates the group's ping in place
  (`MainWindow::foldRepeatedLoggedError`): title `N× <first title>`, body
  `Repeated N times since HH:mm:ss. Latest: <line>`, moved to the top. It
  raises no new border flash.
- Errors a caller reports itself through `flashMessage`
  (`m_noticeOwnsLoggedError`) keep the old behaviour.
- Tests: `test_crypto.cpp` (grouping key and groups) and the window test after
  "a live node 5xx is still an error" (12 access lines from 12 clients: 12
  ERROR log lines, one `12×` ping, no burst notice, a 502 on its own).

### Inline notices instead of toasts (issue 1867, 2026-10-09)

The user asked for no toasts anywhere in the app: information is shown inline
where it belongs. The desktop used to put every message in a floating card
stack anchored over the prompt (`showTopMessage`, a queue of up to 20 cards, a
5 s / 20 s countdown, slide-in and slide-out animations, a separate frameless
window beside a popped-out prompt, avatars, quick reactions and a gear menu).
All of that is gone, with its QSS (`#topMessage*` in `Theme.h`,
`12-alerts.qss`, `10-shell.qss`, `96-edge-indicators.qss`) and the
`kToast*` geometry constants.

- **`flashMessage(text, error, href, kind, runId)`** keeps its duties: it logs
  the line (typed INFO/ERROR), files a quiet row on the Pings page, reports an
  error to the relay (kind `notice`, see below) and plays the arrive/error cue.
  It then shows the text inline in the page notice strip
  (`MainWindow::showPageNotice`, `buildPageNoticeArea`): a column of
  `kit::InlineNotice` rows in the main content column, in layout, above the
  page stack (`#pageNoticeArea`). It takes no room when empty.
- **Keyed to the page it was raised on** (`currentNoticePage`): the section
  index, or for an open repository `0:<owner>/<name>:<tab>`. The strip is
  re-rendered on a section change, a repository tab change and when a
  repository opens or closes, so a notice shows again on its page and does not
  follow the person to an unrelated one.
- **No countdown, no slide.** Any newer message on the same page replaces
  an older confirmation; errors stay until their clear (x) button is pressed,
  at most three per page (`kPageNoticesPerPage`), 60 kept in all. Each row has
  Copy and clear, plus **View output** for a failed action run and an **Open**
  action named after the destination (View run, Open issue, Manage firewall,
  Log, Go online, Ping settings, View in Pings for an error without a link;
  `noticeDestination`). Long text is shortened to 600 characters with the full
  text in the tooltip and on Copy.
- **Local places first.** `showLocalNotice(QLabel *|kit::InlineNotice *, text,
  error, kind)` shows the text in a status label or notice that belongs to the
  panel or open dialog the action came from (issues, discussions, dialogs with a
  status label, settings panels), with the same logging, filing and reporting.
  If that label's panel is not on screen it falls back to the page strip.
  `fileMessage` does the bookkeeping only, for a call site that already shows
  the same sentence itself.
- **Unsolicited pings raise nothing on screen.** `flashPingMessage`,
  `flashNotification` (relay and local pings, chat messages, agent done,
  firewall blocks, usage resets) and the logged-error hook (`alertOnLoggedError`)
  file the row on the Pings page, where the rail badge counts it, and play the
  cue; warnings still pulse the red border and an agent finishing the green one.
  The logged-error card stream (`m_loggedErrorStream`) is gone.
- **Must-acknowledge pings** (`dismissOnly`, a usage reset) used to need the
  card's Dismiss button. They now keep the Pings rail badge counting, across a
  restart, until the Pings page is seen (`markLocalPingsSeen` acknowledges them).
- **Turning a kind off** moved from the card's gear to the gear on each Pings
  row and on a ping's detail (`showPingKindMenu`, `turnOffPingKind`). The
  switches are `pings::pingKindEnabled` / `setPingKindEnabled` (formerly
  `toastKind*`); the QSettings keys (`notifications/mutedDesktopKinds` and the
  synced kind list) are unchanged. A turned-off kind is still filed but plays no
  sound and is not forwarded to phones.
- **Settings.** "Show in-app notification cards above the prompt" and the
  card duration were removed (`notifications/inAppCards`,
  `notifications/inAppCardDurationSeconds` are no longer read). "Show a card for
  ForkMesh system alerts" is now "Alert me about ForkMesh system alerts".
  "Colored left edge on alerts and cards" puts the tone edge on the page
  notices.
- **Load status and sent prompts.** The repository/node loading line is an
  inline status row at the top of the same strip (`showLoadStatus`,
  `dismissLoadStatus`). The "Prompt sent" bubble was removed: a quick-add issue
  already says what happened in the Issues notice (`setIssueInlineNotice`, now a
  real inline label again), and an agent start flies its sparkle to Agents.
- **Error report kind.** The desktop reports a user-visible error as kind
  `notice` (`ClientErrorReports`); `app/src/entry.py` accepts `dialog`,
  `notice` and, from older desktops, `toast`, and refuses text carrying any of
  the three stamps.
- Tests: `checkInlinePageNotices`, `checkLoggedErrorAlert`,
  `checkDismissOnlyUsagePing`, `checkAgentDoneCelebration` and
  `checkChatPingRaisesNoCard` in `test_window_resize.cpp`; `workspace_snapshots`
  checks a burst of notices sits above the conversation header.

## Debug bar

- The opt-in bottom debug bar holds the Restart (fast rebuild and relaunch),
  Resize (snap to 1280x720) and Log tail buttons (`buildStatusBar`,
  `src/MainWindowChat.cpp`). Restart shows only when Settings has "Show a
  rebuild & restart button in the debug bar" on (`ui/showRebuildButton`).
- The move out of the top navigation landed in debc40941 (2026-08-05). The
  parallel attempt on `recover/agent-merge-sweep` (bbd354482) was left
  unmerged; its only surviving piece is the Settings label and tooltip, which
  still said "top bar" until 2026-09-27.
- The status dots in the debug bar (one per `/api/status` system plus this
  desktop's own probes of the website and the Status page, drawn by
  `LogActivityLights`) belong to the Status Page package (`status-page`,
  bundled and on by default) since 2026-10-02. `syncPackageRuntime` calls
  `syncStatusPagePackage`. With the package removed, the dots are cleared and
  `refreshFooterWebsiteStatus` and `probeDesktopWebsite` stop making requests.
  Installing it again reloads both at once.
- Since 2026-10-07 the package follows the account
  ([marketplace/status-page.md](../marketplace/status-page.md)). The desktop
  reads `/api/plugins/@account` about 2 s after launch, on sign-in, when the
  Market or a package settings page opens (at most every 5 s) and on window
  activation (at most once a minute), and applies it to the registry with
  `setInstalledState` (no dependency cascade, no echo back). A local install,
  removal or setting change of an account-synced package POSTs to the account
  first. `market/packages/<id>/state` is now a cache of the account's answer.
- Each dot needs an installed `status-check-*` package whose `status.monitor`
  contribution lists the row (`PackageRegistry::statusMonitorShows`); a row no
  check claims is hidden, and the desktop page probes of an uninstalled check
  do not run. `onlyProblems` (`market/packages/status-page/data/onlyProblems`)
  hides green dots, and `refreshMinutes` sets the healthy floor (2 to 30
  minutes; non-green keeps 2 minutes).
- Custom monitors (`.../data/customMonitors`, a cache of
  `/api/status-monitors`) are dots `custom:<id>`, probed through the window's
  network manager with a 10 s timeout: green below HTTP 400, red otherwise,
  grey while offline. A click rechecks one and opens its address. Status
  Page's settings page lists them with Remove and has an add row.
- `PackageRegistry` resolves `dependencies`: installing pulls in what is
  missing, removing takes every installed dependent with it
  (`installedDependents`), and the confirm dialogs name them.

### Diagnostic report (Report button, 2026-10-06)

The Report tool (copy icon) sits just left of Log in the debug bar's tool
cluster. A click copies a Markdown report meant to be pasted into a coding
agent: instructions for the agent first, then everything it needs to fix
errors and optimize the app.

- `src/DiagnosticsReport.{h,cpp}` (`forkmesh::diagreport`) builds the text from
  a plain `Snapshot`, so the format, grouping and thresholds are covered by
  `tests/test_diagnostics_report.cpp` (`forkmesh-diagnostics-report-tests`)
  without the widget stack. `MainWindow::diagnosticsReport` in
  `src/MainWindowDiagnostics.cpp` fills the snapshot from live state: the log
  buffer, stalls, crash blocks in the buffer, `NodeDiagnostics` findings, the
  Qt runtime snapshot and report, process and host resources, and the log
  file locations.
- Sections, in order: what to do, since the last report, findings (crossed
  thresholds), problems in the log, not seen since the last report, crashes,
  UI stalls, app, where to look, resources, self-diagnostics, log lines by
  category, recent log, Qt runtime. The bulky sections come last, because the
  report is cut at 48,000 characters.
- Problems are ERROR, STALL and BGBLOCK lines, grouped by a signature with
  numbers, hashes, UUIDs, paths, URLs and quoted text folded. Each group shows
  its count and its newest line with the `[file:line]` anchor.
- It evolves: every copy stores a baseline (sequence number, problem
  signatures with counts, stalls, crashes, resident memory, event-loop p95)
  in `diagnostics/selfFixReportBaseline`. The next report is numbered one
  higher, marks problem groups missing from the baseline as NEW, lists the
  groups that stopped as "Not seen since the last report", and shows the
  deltas. The intended loop: paste, let the agent fix, restart, copy again.
- The copy also writes the report to `<diagnosticsDir>/self-fix-report.md`
  through `diskwrite::writeLater`. Right-click offers "Send report to a new
  agent", which writes the file synchronously and starts an agent in this
  build's checkout with a short prompt that points at it (the composer and
  some command lines cap prompts near 16,000 characters, the report does
  not), and "Start comparisons over", which drops the baseline.
- The CPU figure is the last one-second sample from `updateFooterDiagnostics`
  (`m_debugLastCpuPercent`). Calling `SystemStats::cpuPercent()` again would
  reset its shared sampling window.

## Stall watchdog

`src/StallWatchdog.{h,cpp}`. `MainWindow` starts it with a 500 ms threshold
(`src/MainWindow.cpp`). A watcher thread notices the GUI thread has not returned
to the event loop, sends `SIGUSR2` to the main thread, and the handler samples
the stack. Qt, glibc and Mesa have no frame pointers, so since 2026-09-23 the
handler scans the stack for return addresses into known text ranges
(`safeStackScanTrace`); records before that often hold only a libc/Qt leaf.

Triage recipe: group `stalls.log` by its `likely origin:` line and sum
milliseconds per origin; symbolize unresolved `+0x` frames with
`addr2line -fCpe /proc/<pid>/exe` of the running binary (the on-disk
`build/forkmesh` is usually newer). `actions.jsonl` pairs of
`started`/`finished` give exact per-task time and are often a better guide
than stack samples.

`sonify::Player::stop()` used to wait on the UI thread for the pump thread,
which gives `pw-cat` 600 ms to exit after stdin closes and then kills it: every
Listen toggle-off was a ~600 ms stall (57 in one day). It now silences the
pump (it drops the mixer pointer under a mutex, so it never waits on the pump
thread) and lets the thread finish and delete itself; only the destructor
during application shutdown still waits.

The `likely origin:` line picks the first application frames in the scan,
and `MainWindow::eventFilter` and `handleFramelessResizeEvent` wrap every
event the window sees, so they head many groups without doing the work.
`QtEventDeliveryProbe::~QtEventDeliveryProbe` was in the origin of 57 records on
2026-09-27 and is skipped since then (see "Repo screen git reads" below). On 2026-09-27 those
groups (~85 s over five hours) held `QCss::StyleSelector::styleRulesForNode`,
`QTreeWidget::setItemWidget` and text layout underneath: stylesheet polish
and widget rebuilds, not the filter. Aggregate the demangled frames of a group
before blaming its origin.

### Stall reports after a log clear

`stalled()` is queued to the GUI thread and the watcher only emits it after it
sees the loop resume on its 100 ms poll, so a freeze can be reported after the
Log page's Clear (or `testResetNetworkLog()`) ran; a click queued behind the
freeze itself always is. The signal carries `startedAtMs`, the freeze's
estimated wall-clock start, and both clears stamp `m_networkLogClearedAtMs`.
`onUiStall` keeps a freeze that began before the stamp out of the app log (the
`UI stalled` line, the 5 s `UI froze` ERROR line and the STALL light) but still
counts it, writes it to the stall log and hands it to the stall teammates. The estimate can sit up to one 200 ms heartbeat early,
so a freeze inside the Clear slot itself (its archive flush) may count as
before the clear and show only in `stalls.log`. The first fix (pull/87, 2026-08) drained queued calls with
`sendPostedEvents(this, MetaCall)` inside the Clear slot; that ran every queued
call for the window re-entrantly and still missed reports not yet emitted, so
it was replaced by the stamp before landing.

### Background file writes

`src/BackgroundFileWriter.{h,cpp}` (`forkmesh::diskwrite`) moves the
`QSaveFile` commit, and its fsync, off the GUI thread:

- `writeLater(path, bytes)` queues a snapshot. One dedicated worker (not the
  global pool, which git and scan tasks can fill) writes paths in the order
  they were first queued; a newer snapshot for a path still waiting replaces
  the older one.
- `writeNow(path, bytes)` writes synchronously, first dropping any queued
  snapshot for that path and waiting out one being written, so an older
  snapshot never lands on top of it.
- `flush()` waits for the queue; `MainWindow::~MainWindow` calls it after the
  last ledger save.

The tool-efficiency and guardrail ledgers save through `saveLater()` from
their 5 s debounce timers and Clear buttons; `save()` stays synchronous for
tests. Test: `testBackgroundSave` in `tests/test_agent_tool_efficiency.cpp`.
The notification journal, the Vultr provision log and the network-log
digest still carry their own hand-written worker copies of this pattern.

### Settings writes that change nothing

Qt marks a key dirty on every `setValue()`, equal value or not, so a poll
that re-stores the same figures rewrote and fsynced the whole settings file
each time. `src/SettingsWrite.h` has `forkmesh::settings::put()` and `drop()`,
which skip the write when the stored value already matches (an ini reload
returns numbers and bools as strings, so those compare by their string form)
or the key is absent. On 2026-09-27 the `QSettings` syncs behind stalls were
led by `startSession` (12 s), `ActionStore::approve` (12 s), the Claude usage
and reset handlers (22 s), `rememberUserAccount` (5 s) and
`saveSolanaAddress` (4 s). The usage handlers (`MainWindowAgentUsage.cpp`,
Claude and Codex), `startSession`, `rememberUserAccount` and
`saveSolanaAddress` now use the helpers. `drop()` sees only leaf keys:
`contains()` is false for a group, so remove a group with `remove()`.
`ActionStore::approve` stores a new value on every approval and still writes.

### 2026-10-07 settings fsync and avatar records

Three startup and merge stalls from pid 1146139.

- **Private rooms on startup (578 ms).** `startSession` clears
  `m_privateChannels`, then `restorePrivateChannels()` rejoins each saved room
  through `ServerNode::createPrivateChannel`, which emits
  `privateChannelJoined` right away. Each join ran `persistPrivateChannels()`,
  so every startup rewrote and fsynced the settings file once per room, each
  time with a partial list. `m_restoringPrivateChannels` now holds those saves
  back while the rooms are rejoined, and `restorePrivateChannels` saves once
  afterwards through `forkmesh::settings::put`.
- **`put()` and string lists.** After the ini file is read back, a one-item
  `QStringList` comes back as a `QString`, an empty one as an invalid
  `QVariant` (`@Invalid()`), and a longer one as a `QVariantList`.
  `sameStoredValue` now treats all three as equal to the matching list, so an
  unchanged list is no longer rewritten on every start. Test:
  `tests/test_settings_write.cpp` (`forkmesh-settings-write-tests`).
- **Merge notes (779 ms).** `rememberMergedBranch` stored the merge credit
  notes (up to 200, as JSON) under `branches/mergeNotes` in `QSettings` on
  every merge that landed, and `applyWorktreeMergeStep` paid for the fsync.
  The value changes on every merge, so skipping unchanged writes could not
  help. The notes now live in `merge-notes.json` beside `ForkMesh.conf`, held
  in memory and saved with `diskwrite::writeLater`. The first load with no
  file imports the old setting, writes the file with `writeNow`, and only then
  removes the setting.
- **Avatar decode in `showAgentSession` (552 ms), left alone.** The one sample
  was in `QPixmap::loadFromData` under `roundedAvatar(effectiveUserAvatar(),
  26)`; the `topBarUserName` and `settingsAccountName` frames were stale
  addresses. `roundedAvatar(QByteArray, …)` already caches by bytes hash,
  size, side, ratio and DPR, and the 128x128 profile PNG decodes in about
  0.1 ms, so the sample shows only where the thread was at the 552 ms mark
  inside the queued `showAgentSession` call (in `getAccountSync`'s nested
  loop), not what took the time.

### Users page stall on first open

STALL-20261007T011001808 (1060 ms): clicking the Users rail button blocked the
GUI thread in `QTableView::sizeHintForColumn` under `QHeaderView` while
`MainWindow::showSection` raised the page. `buildUsersSection()` fills the All
users table from the cached account directory as soon as it is built, but
`makeColumnsResizable()` switches its ~56 `ResizeToContents` columns to
Interactive only on a zero-delay timer, so the page was shown while every
column still sized itself to its contents. At Qt's default resize-contents
precision of 1000 rows that laid out up to about 56,000 cells, and the timer's
own `resizeColumnToContents()` pass repeated the work. The Users and
Quarantine tables now set `QHeaderView::setResizeContentsPrecision(64)`, so
fitting a column measures the visible rows plus the nearest ones, up to 64
rows, however many accounts exist. A value wider than anything in that sample
is clipped until the column is dragged wider. The window test checks that both
Users tables keep a precision between 1 and 100. Any other table that is
filled before its page is first shown and keeps `ResizeToContents` columns
needs the same treatment.

### Agent diff refresh cadence

The Agents list measures each session's diff and worktree status on a worker
("refresh diff and worktree status"). Agent tool results ask for a pass
through one shared timer: the first result arms it 400 ms out, and later ones
never push that deadline back, so a stream of edits is still measured while it
goes on. On 2026-10-07 a fleet of six busy agents kept that pass running
back-to-back (the report's 92% CPU): 27 passes a minute on average and up to
42, at about 1.4 s and ~28 git processes each, busy 45% of the time, with 31%
of passes starting within 50 ms of the previous one. A tool result that landed
during a pass discarded that pass's results and queued another for the instant
it finished. Now no pass starts until a cooldown after the previous one ends:
three times that pass's duration, between 1 s and 15 s. Every request waits
for it, explicit rescans included. Tool results that land during a pass are
kept and served when the cooldown ends, not by discarding the running pass. A
pass triggered only by tool results re-measures only the running sessions that
reported one, plus sessions whose branch tip or status changed and the
selected session. Results from agents in other repositories do not trigger a
pass, and no pass runs when nothing was reported. Unchanged stats no longer
repaint their row (`sameAgentDiffStat`), and the end of a pass rebuilds the
table only when a stat changed or a row went away. The window test sends a 4 s
burst of tool results and checks the number of passes stays bounded, the
stream is still measured, and no pass runs once it stops.

### Branch range file list

`MainWindow::showRangeFilesInSourceControl` gives every changed file an
item widget of about nine children (icon, name, directory, status, actions),
and each one is polished against the application stylesheet. Built in one
pass, a wide branch froze the window: 17 stalls and ~50 s on 2026-09-27 with
`QCss::StyleSelector::styleRulesForNode` and `QTreeWidget::setItemWidget`
under `applyBranchDiffRender`. Rows are built in batches, one batch per
event-loop turn with the tree as the timer context. A batch ends after
`kScmRangeRowsPerBatch` (60) rows or once it has spent
`kScmRangeBatchBudgetMs` (12 ms), whichever comes first; the first batch
always builds at least `kScmRangeRowsFirstBatchMin` (8) rows, later ones at
least one. A fixed 60 rows was still 0.5 to 2 s per batch on 2026-09-28
(9 stalls). A batch stops if the tree was rebuilt since (the group row, its
rows key or its child count no longer match). After the last batch the tree
is refit, a previously selected file is reselected if the selection has not
moved off the first row, and the active outline is redrawn.

### Diff Viewed marks

The per-file Viewed marks (`MainWindow::loadDiffViewedStamps` /
`saveDiffViewedStamps`) live in `diff-viewed.json` beside `ForkMesh.conf`,
held in memory and written with `diskwrite::writeLater`. The first load
with no file imports the old `diffViewed/<owner>/<repo>/<context>` settings
group, writes the file, and only then removes the group. Empty mark lists are
dropped rather than stored.

Every review pane shows an "x of y files viewed" count: the branch diff's
summary strip, the Source Control header (working tree and branch range), the
pull request's file list, and the agent Code pane. The count turns into a green
"✓ y of y files viewed" (`diffViewedCountHtml`) only when every file is viewed
and each mark is current, that is its stamp matches the fingerprint of the patch
on screen (`MainWindow::diffViewedMarksCurrent`; the Code pane compares
`m_agentCodeViewed` against `m_agentCodeFingerprints`). A mark with no stamp yet
does not count as current. Added 2026-09-30 (task 799).

The pull request list shows the same count beside each row's files-changed
number as "x/y" (`kPullViewedFilesRole`): blue while some files are viewed,
green once x reaches y, nothing when no file is viewed. The list has no patch
in hand, so it counts the stored marks for `pull/<n>` (capped at y) without
checking their stamps; a stale mark drops out the next time that pull's diff
renders. `saveDiffViewedStamps` updates the row through `setPullViewedCount`,
so a tick in the diff shows in the list without a rebuild. Added 2026-10-06
(task 1513).

### Reading summed stall frames

The stack scan also picks up stale return addresses from calls that already
returned. Anything that pumps a nested event loop (`runGitCapture`, PullStore's
git helpers) is charged for every handler that runs inside it. Summed frame
totals therefore overstate a function's own cost: on 2026-09-27
`PullStore::loadAll` appeared in 643 s of stalls and
`relayscope::normalizedOrigin` in 301 s. Before optimizing, check the stall's
`system at detection` line for memory/io starvation and the worker's own
`duration_ms` in `actions.jsonl`. `pullRangeSnapshot`'s 5.6 s "waiting for
worker" was a 5 ms `git log`. To rank the UI-thread git waits, sum `finished`
records with `execution: ui-blocking` by `source`.

App-wide event filters are the worst case (2026-10-07). `MainWindow` filters
every event in the application, and the first thing `MainWindow::eventFilter`
does is call `handleFramelessResizeEvent`, so `eventFilter+0x36e` and
`handleFramelessResizeEvent+0x29` were left in dead stack slots and turned up
in 91 of 206 records, nearly always next to the frame that did the work
(`appendDiffStreamChunk`, `applyTheme`, a delegate's `sizeHint`). The Health
page summed them into 78.7 s on `MainWindowIssues.cpp` and ranked it the
least healthy file (score 52), for a frameless-resize check that compares a
few integers. Both readers now skip these frames: `isGlueFrame` in
`src/StallWatchdog.cpp` for the `likely origin:` line, and `isDispatchResidue`
in `src/CodeHealth.cpp` (`is_dispatch_residue` in `tools/code_health.py`) for
attribution. They skip any `::eventFilter` frame and the three frameless-resize
helpers it calls on every event (`handleFramelessResizeEvent`,
`resizeEdgesAtGlobalPos`, `updateFramelessResizeCursor`). A filter that does
real work calls a named helper, and that helper still takes the blame. A
record that names only those frames counts as unattributed. With the same log,
`MainWindowIssues.cpp` scores 81. The real stalls now rank first:
`applyTheme` from the view-mode menu (one 44 s freeze in the stylesheet
repolish) and `applyBranchDiffChunk` (16.7 s). Tests:
`checkEventFilterResidueIsNotOrigin` in `tests/test_stall_watchdog.cpp` and the
stall attribution block of `tests/test_code_health.cpp`.

### Performance pass (2026-09-27)

Driven by the 2026-09-27 `stalls.log` (926 stalls, 2106 s) and `actions.jsonl`
(263 s of UI-blocking git waits).

- **Mirror refs hash.** `MainWindow::mirrorStateHash` spawned `git for-each-ref
  refs/heads/ refs/tags/` on every refs-stamp miss, and agent pushes make misses
  common. GUI-thread callers (`drainNodeCommitEvent`, `syncMirrorsBehindRoster`,
  releases health) waited in `runGitCaptureBackgrounded`'s nested loop: 89.6 s
  over 107 calls, max 54.9 s, for a command that takes 6 ms on its own.
  `forEachRefHeadsAndTagsFromFiles` (`MainWindowInternal4.cpp`) now builds the
  same bytes in-process. It reads loose refs first, then `packed-refs` (loose
  wins; `#` and `^` lines skipped), sorts by refname and feeds the result to
  the unchanged `refsSha256FromForEachRef`, so peers still compare the same hash.
  Anything unusual falls back to git: reftable or ref-storage config,
  `commondir`, `.git`, `GIT_DIR`, foreign ownership, symlinks, symbolic or
  non-hex refs, invalid names, or malformed `packed-refs`. Unlike git, refs
  that point at missing objects are not filtered out. Test: the ref-files block
  in `tests/test_perf_budgets.cpp`.
- **Issue count and branch menu.** `mirrorOpenIssueCount` adds a memo keyed
  on the `.forkmesh/issues` tree id, so a merge that does not touch issues
  costs one `rev-parse` instead of three `ls-tree` and a `cat-file`.
  `listRepoBranches` is memoized on `gitRefsDirSignature` of the gitdir and
  common dir; reftable repositories skip it. The overview's own reads moved to
  a worker in "Repo screen git reads" below, so they are not memoized here.
- **Pull list.** `PullStore::loadAll` keeps a process-wide cache (8 entries)
  for writable stores. It is keyed on a stat-only stamp of `HEAD`,
  `packed-refs` and `refs/**` of the work tree's gitdir, its common dir and the
  mirror, plus the `.forkmesh/pulls` tree. Nothing is stored in these cases:
  the stamp moved in the last 3 s, a repository uses reftable, or any step of
  the load failed (a pull file that did not parse, a failed `for-each-ref` or
  `cat-file`, an isolated ref that did not yield its pull). Otherwise a git
  timeout under load would pin a partial list. Read-only stores
  (`loadFromMirror`) swallow git failures and are not cached.
- **Relay scope.** `relayscope::normalizedOriginText` is a per-thread memo
  (256 entries) in front of `normalizedOrigin(QUrl(...))`.
  `activeRelayOrigin()` is cached on the server field's text, its only input.
  `repoIndexFor` and `findNetworkRepoIndex` compare owner and name before
  running the relay check.
- **Avatars.** `roundedAvatar(const QByteArray &, …)` caches decoded, rounded
  pixmaps in a 16 MB `QCache` keyed by content hash, size, side, radius and
  device pixel ratio (a hit also compares the bytes).
- **Footer log and debug tail.** Both read-only `QTextEdit`s kept Qt's default
  undo history. Every append and trim grew a never-cleared undo stack, and
  19 of 41 stalls under `setFooterUpdateLine`/`appendDebugLogTailLine` were in
  `QTextUndoCommand` frames. Undo is now off. Lines are queued, and a 50 ms
  timer (`flushFooterLogLines`) appends each batch in one edit block, trims once
  and scrolls once. Only the lines that would survive the trim are queued, and
  the activity lights still count every line immediately. Test helpers that read
  these views flush the queue first.
- **Agent stream.** The JSONL framer searched the whole buffer for a newline on
  every chunk, which is quadratic on multi-megabyte tool results. It now
  searches only the new bytes, because the buffer never holds a newline between
  calls. Runtime frames remain bounded at 64 MiB: Codex can place a complete
  large build or tool result in one JSONL event, so the former 8 MiB limit
  discarded a usable turn before the adapter could handle it. Other changes on
  this path:
  - the raw-output buffer grows and trims in place;
  - `applyTranscriptEvent` passes the stream's session by reference instead of
    copying it twice per event;
  - `AgentStore::appendEvent`/`appendLog` build the session path once;
  - `refreshAgentTable` drops its session snapshot before selection signals
    call `findAgentSession`, which otherwise deep-copied every session;
  - a first launch resolves base commit and branch with one
    `git rev-parse HEAD --abbrev-ref HEAD`.

  Never keep a copy of `m_agentSessions` or `m_streamEvents` alive across a
  call that uses `findAgentSession` or appends events.
- **Unchanged style sheets.** `QWidget::setStyleSheet` re-polishes even when
  the sheet is unchanged. `updateNodeOnlineControls` and
  `updateHostsFleetSummary` now compare first, as the Vultr progress and
  milestone views already did. The application sheet itself is cheap to
  match: Qt indexes its 987 rules by ID and type, and only about 8 (`*`,
  `QWidget`, `#x > QWidget`, `#issuePageScroll .QWidget`) are checked against
  every widget.

Not changed, with the evidence:

- `HoverRowDelegate::paint` only forwards to `QStyledItemDelegate`. Its samples
  are Qt text shaping and icon scaling under starvation.
- The agent dot matrix's 45 ms scanner tick already skips unchanged rows
  (`AgentDotMatrix::setDots`).
- `alertOnLoggedError` opens several `QSettings` per ERROR line (7 stalls,
  22 s). Caching those values needs change notification.
- `startCliTranscript`'s `reloadAgents()` re-reads every `meta.json` on the
  GUI thread.

The UI-sound player start and the deploy-log cursor writes from the same
stall log are covered in "2026-09-28 stall pass" below.

The footer's CPU/RAM/stall indicator opens the stall details.
`src/SystemPressure.*` reads Linux PSI to tell a system-wide starvation from a
freeze in the app.

### Repo screen git reads

On 2026-09-27 the Code and Commits tabs were the most frequent `git wait`
entries with `"execution":"ui-blocking"` in `actions.jsonl`. `runGitCapture` on
the GUI thread hands the command to a worker and pumps the event loop until it
returns, so each read was a nested event loop, and under memory or IO pressure
the stall watchdog reported it as "synchronous UI wait for worker Git":

- `MainWindow::loadRepoOverview` ran `rev-parse`, `log -1`, `ls-tree`,
  `ls-tree -r -l`, the empty-tree `diff --numstat` and `show <README>` one
  after another (up to 2.9 s for one numstat).
- `MainWindow::loadCommits` ran `git log` (109 waits, up to 1.2 s), then
  `unpushedCommitHashes` (`rev-list <mirror tip>..<ref>`, 97 waits),
  `upstreamPublishesThroughMirror` (two reads, 180 waits) and the mirror tip.
- `commitsListIsCurrent`, which four callers used to choose between a
  source-control refresh and a reload, ran `rev-parse` and read the mirror tip.

Each of them is now a worker read plus a GUI apply:

- `loadRepoOverview` sets the path, shows the skeleton rows only when the
  list is not already showing this repo, path and ref, and starts
  `readRepoOverview` (a static member: no widget or `m_` state) through
  `runOffThread`. The worker returns the head, the commit-bar fields, the
  rows, the README and the `ls-tree -r`/numstat output. If the head matches
  the key on screen, it stops after `rev-parse` and the apply does nothing.
  `applyRepoOverview` drops a result whose generation is not the latest. If
  the repo, path, ref or git dir changed without a new load, it reloads. A
  second request for the same target while one is running only sets
  `m_overviewReloadPending`, so a burst of refreshes starts one worker, not
  one thread per call. The per-commit tree cache (`OverviewTreeCache`) is
  copied into the request and updated in the apply. The skeleton rows no
  longer call `processEvents`.
- `loadCommits` clears the table, then runs `readCommits` (log, mirror tip,
  unpushed and `@{upstream}` hashes) on a worker. `applyCommits` builds the
  rows. `unpushedCommitHashes` and `currentMirrorTip` are gone. "Load more"
  keeps its scroll position in `m_commitsLoadMoreScroll` and the apply
  restores it. The apply re-selects the open commit's row
  (`syncCurrentCommitRow`) because the history can now arrive after
  `showCommit`.
- `refreshCommitsUnlessCurrent` replaces `commitsListIsCurrent`. It reads the
  local and mirror tips on a worker (`readCommitTips`, shared with
  `refreshCommitMarkersIfStale`) and then refreshes source control or reloads.

`recoverMissingBrowsedRef` still resolves refs on the GUI thread. It runs only
when the worker reports that the browsed branch no longer resolves.

`forkmesh::ui::sourceChangedSince` walks the client source tree and
`../marketplace` (a `getdents` stall of 1.9 s on 2026-09-27). It ran on the
GUI thread after every rebuild pass, next to two `gitOutput` calls, which are
plain `QProcess::waitForFinished(3000)` waits that do not pump. Now the
`rev-parse --short HEAD` before a pass and the tree walk plus
`rev-parse` after it run through `runOffThread`. The pass logic itself
(`settle`) is unchanged.

`QtEventDeliveryProbe::~QtEventDeliveryProbe` does not block: it reads a
monotonic clock and adds to a few counters. It showed up as an origin because
`safeStackScanTrace` takes every word on the stack that follows a call.
`ForkMeshApplication::notify` creates a probe around each delivery, so return
addresses from earlier probe destructor calls stay behind in dead stack slots
and get picked up by the scan. `isGlueFrame` now skips `QtEventDeliveryProbe`,
`QtRuntimeDiagnostics::noteDelivery` and `recordEventDelivery`, so the origin
names the application frames under them. The same scan artefact can still
produce other odd pairs (`sonify::Player::shutdown` next to an unrelated
event). Read the whole sample before trusting a pair.

Tests: `app/tests/test_qt_repo_screen_git_off_gui.py` pins these paths: no
git, mirror, `gitOutput` or `processEvents` call outside a `runOffThread`
worker in the load and probe functions, and generation guards on the
applies. `checkInstrumentationFramesAreNotOrigins` in
`desktop/tests/test_stall_watchdog.cpp` feeds a sample from the 2026-09-27 log
through `likelyOrigin`. `forkmesh-window-tests` waits on
`testRepoOverviewLoading()` and retries `testOpenMostRecentCommit()` because
both loads now finish on a later event-loop turn.

The largest single GUI git wait that day was `mirrorStateHash`
(`MainWindowNavigation.cpp`, `for-each-ref` on the served mirror, up to 55 s
while the machine was starved). Its GUI callers moved to workers on
2026-09-28 (next section).

### Every GUI-thread git wait (BGBLOCK, 2026-09-28)

The footer log's `BGBLOCK Background ✕ git not backgrounded` lines on
2026-09-28 came from two separate problems.

**Mislabelled worker reads.** The footer tallies open tickets by word, and
`git wait` (the `UiBlocking` ticket `runGitCaptureBackgrounded` opens while
the GUI thread waits) shares the word `git` with every worker `git` ticket.
One GUI wait made the whole overlapping group ✕, and the line quoted the
newest ticket's command, caller and time. So `readAgentDiffStat`'s 82 s
`git status` timeout, the branches panel's `diff --numstat`, `branchMergeTree`,
`refreshSourceControl`, `refreshRepoChangeBadge` and `pruneNonStableMirrorRefs`
showed up as ✕ even though `actions.jsonl` records every one of them as
`"execution":"worker"`. `noteBackgroundActivity` now keeps a
`BackgroundBlockingSpan` per word for the `UiBlocking` tickets only. When the
group closes, the ✕ line carries the wait's own command
(`waiting for worker: git …`), caller, checkout, failures and longest wait. The
worker tickets still get their own ✓ line. `forkmesh-window-tests` checks this
with an overlapping worker read and wait (`probe650`).

**Real GUI-thread waits.** `actions.jsonl` rows with
`"kind":"git wait","execution":"ui-blocking"` list every call that made the
GUI thread wait for git, with the caller's source line. The running build
predated the repo screen change above, so `loadRepoOverview`/`loadCommits`
entries were already fixed. The rest are fixed as follows.

Branch and worktree actions in the desktop app no longer wait for git on the GUI thread. The journal showed "Update all agent branches" alone blocking the window for 351 s. "Merge & clean up", "Update from main", the merge editor, and the worktree panel's update, commit, resolve, remove and prune buttons all used to wait on git through a nested event loop, and each wait showed up as `BGBLOCK … git not backgrounded`. Each action now takes a snapshot on the GUI thread, runs its git steps in a static worker function through `runOffThread`, and does its notices, dialogs and reloads in an apply step. The apply step checks that the same repository is still open before touching the UI. Busy flags drop repeat clicks while an action is still running.

Flows that ask the user something mid-way (the stash prompt, the merge-conflict editor, the commit-message prompt, delete confirmations) split around the dialog: read on a worker, ask in apply, then carry on with another worker. The merge editor's own commit button now runs `git add`/`git commit` on a worker and keeps the dialog open until it finishes. `mergeWorktreeIntoMain`, `openBranchMergeEditor`, `createPullFromBranch` and `removeWorktree` report completion through callbacks instead of return values.

Looking up which worktree holds a branch, and which branch the checkout is on, now reads `.git/HEAD` and `.git/worktrees/*` directly when asked from the GUI thread. This matches `git worktree list --porcelain`. Workers still use the cached git listing, and the GUI thread only falls back to git when the files can't be read.

The repository screen no longer waits for git on the GUI thread. The branch list and default branch are read straight from the repository's ref files (loose refs and packed-refs), so opening a repo, activating the window or building a branch menu never starts git. Git's newest-first order is fetched by a background worker and merged in when it arrives. Repositories using the newer reftable ref storage, which these files can't read, fall back to the last list git gave.

When the branch being browsed has disappeared, the worker that finds out (overview, file tree or branch refresh) also works out which branch to fall back to, and the GUI only applies the result. Loading the file tree, opening a file or README, following a markdown link, file history, repository info, Insights, closing issues named in commit messages, commit file rows and the unsynced-files panel all read git on workers. Each result is checked against a generation number or the current repository and branch before it is shown. User actions (pull, create branch, delete or revert commit, reassign attribution, uncommit, file create/rename/delete, save and save-as-PR, patch download) keep their dialogs and messages but run their git steps on workers, with a guard against repeat clicks.

Diff image previews read only cached images on the GUI thread; on a cache miss they show "Loading preview…" and fetch in the background. Surfaces that load their patch on a worker also fill the image cache there, so they render with images straight away. The Calendar's repository commit scan and the markdown Preview button's before/after reads also run off the GUI thread.

The Mirror nodes panel no longer runs git on the GUI thread. It used to read the source mirror's route and refs hashes, its branch heads, ancestry between commits, backlog commit counts, commit subjects and, on first open, its own mirror's summary, all while the user waited. The journal showed `mirrorStateHash` as the most frequent wait, up to 2 s, alongside `branchHeadsFromGit`, `mirrorRoutingStateHash`, `mirrorHasCommit` and `merge-base --is-ancestor`. The panel now builds only from cached values, keyed by the mirror's refs signature, and collects whatever is missing into one request for a single background reader. When the answers arrive the panel redraws. The first time a repository is shown it displays a short "Reading this repository's mirror state" row instead of guessing at sync status. The panel also drops cached direct-probe rows whose node is no longer in Hosts each time it renders. The probe filters them when it writes the cache, but a cache written before a Host was removed would otherwise keep showing it until the next probe.

Mirror sync background passes follow the same pattern. The roster "behind" check, the commit-event hash check, the remote-route scan before reverse convergence, and the private-sync fingerprint (both the idle check and the value recorded after sealing) now read git on workers. When a result comes back, the desktop confirms the repository is still the same before it syncs, converges or saves anything, and overlapping triggers are merged instead of stacking workers.

Push to upstream keeps the same messages and dialogs, but reads the relay check, the upstream branch and the remote URL on a worker before asking about a read-only upstream. The "Mark safe & commit" step after a blocked secret scan also commits in the background. The repository shows as busy from the click onward, so repeat clicks are ignored.

The Releases and Artifacts panels (artifact list, release list, tag pruning, release detail, new release, release notes, delete tag, push to mirrors, replicate artifacts) read git through static readers on workers and apply the result on the GUI thread.

The mirror safety sweep and every sync used to check the repository-identity marker, the mirror's origin and the "is this a git repository" probe on the GUI thread before fetching. The desktop now does these reads and any pending identity stamps on a worker. The quarantine decision is applied on the GUI thread only if the record still names the same identity, source and mirror, and the sync then continues. Identity stamps written while adding, creating or adopting a repository are also written on a worker. Stamps still being written are applied by the next identity check first, so a check never sees a marker that is still being written.

A Windows source path with a drive letter (`c:/Users/…/task-cli` or `C:\Users\…\task-cli`) is a local folder, not a URL. Before issue #1937 the source check parsed it with `QUrl`, which reads the drive letter `c` as a URL scheme. The folder was then treated as a remote, its parent folder taken as the owner, and every repository imported with "Import folder…" on Windows was quarantined with "source c:/Users/…/Downloads/task-cli identifies downloads/task-cli, not <owner>/task-cli". It never synced, so it stayed under "Private & local" with 0 mirrors and "not on the relay", even when imported as Public. `mirrorSourceIdentity` and the source display now treat `X:/` and `X:\` paths as local. The folder name and the identity marker then decide the match, as for any other local source. A record already quarantined for this reason clears on the next sync once the updated desktop is installed.

The identity marker is only stamped on, or read from, a folder that is itself a repository (it holds `.git` or is a bare repository). `git config --local` run in any other folder walks up to the nearest enclosing repository. On 2026-10-06 the `laterepo` window test registered an empty temp folder, and agent scratch folders sit under `~/projects/forkmesh/src/.worktrees/.scratch`. Its stamp landed in the primary checkout's `.git/config` as `me/laterepo`, and every sync of forkmesh/forkmesh was then quarantined with "local source … is marked as me/laterepo, not forkmesh/forkmesh". The window suite now also sets `GIT_CEILING_DIRECTORIES` to the temp folder, so no git call from a test's temp folder can climb into the checkout that holds it. If that row still shows up, fix the primary checkout with `git -C ~/projects/forkmesh/src config --local forkmesh.repositoryIdentity forkmesh/forkmesh` and sync again.

The Issues tab badge counts the bare mirror's issues on a worker (`updateRepoIssueCount`, from main's 2026-09-28 stall pass). The push-URL check in ensurePushHook, the Repository settings remotes table and the import flow's origin reads also run on workers.

These were the `BGBLOCK ... git not backgrounded` waits the action journal attributed to `cachedGitConfigLocal`, `mirrorOpenIssueCount`, `ensurePushHook` and `stampGitRepositoryIdentity`. Each one held the GUI in a nested event loop while git ran, for up to 2.3 s on the issue count.

The Source Control panel's stage, unstage, discard, stage-all, discard-all, undo-commit, untrack and commit actions no longer run git on the GUI thread. Each change goes into a queue that runs one git job at a time on a worker, so actions on the same repository still happen in click order. A repeat click on the same action is dropped while the first is still queued or running. Warnings and panel refreshes happen once the worker answers. The index-lock retry sleep and the Ratchet Mode size check (its own blocking `git ls-files`) run there too. Commits and file reverts take a single in-flight flag. Any question that has to be asked mid-flow ("Nothing is staged…", "Undo the latest outgoing commit?", "Stop tracking it?") is asked between two worker steps, never while git is waiting.

On the Pulls page, the base..head commit walk behind the Commits and Checks tabs now reads only a cache on the GUI thread. A miss or a moved branch tip starts one background read per range, and the open pull re-renders when the answer changes. Until then a pull whose range isn't cached shows the same fallback as a range that can't be resolved. Branch deletion after deleting or merging a pull, the Build & preview head lookup, the New pull request reads and diff, applying a suggested fix, and the AI branch-merge commit or abort now all run on workers. Each apply step checks that the same repository and pull are still open before touching the UI.

For agent pulls, resolving the new pull's refs and pushing its runner ref to the served mirror moved to workers. The explicit-push marker is still set before the push, so the action watcher doesn't queue the checks twice. The size map's ignored-file listing is read once on a worker, and the scan starts when it arrives. Still blocking: PullStore's own git calls, because their callers need the result synchronously. The diff capture moved to a worker on 2026-09-28 (see "Git timeouts, index.lock and agent pull capture" below).

Still synchronous, and why:

- `pushToSshMirrorRemotes` / `removeSshMirrorRemotesForNode`
  (`MainWindowMirrorPush.cpp`) read `cachedRepoRemotesRaw`, which runs git
  only after the config file changes, and their callers use the result
  straight away.
- `PullStore`'s own git calls: the callers need the pull number
  synchronously.
- `detachForksBorrowingMirror`'s repack: the callers delete the mirror
  straight after it.
- `resolveCliTranscriptBase`: a deliberate, bounded wait, marked for code-health.

Tests: `app/tests/test_qt_branch_merge_git_off_gui.py`,
`test_qt_repo_detail_git_off_gui.py`, `test_qt_mirror_sync_git_off_gui.py`,
`test_qt_releases_git_off_gui.py`, `test_qt_mirror_primitives_git_off_gui.py`
and `test_qt_source_control_pulls_git_off_gui.py` check the changed GUI-side
bodies for git helpers outside workers, static readers, and staleness checks
in the apply steps.

### 2026-09-28 stall pass

Triage of `stalls.log` from 2026-09-27 23:13 to 2026-09-28 01:17 (617 records).
Until about 23:58 the machine was starved (memory PSI 30-60 %, load up to 140)
and most records carry the starvation verdict; after that, pressure was near
zero and the records point at the app's own work. Each fix below removes
blocking work from the GUI thread; none is a starvation workaround.

- **Agent pull landing (136 s, 14 stalls of 8 or 16 s).**
  `maybeCreatePullForStreamSession` ran `git diff --binary <baseRef>` and
  `git format-patch --stdout <baseRef>` as plain `QProcess::waitForFinished(8000)`
  calls. `baseRef` is the SHA recorded when the session started; agent
  worktrees get `main` merged into them, so for agent 575 it was 62 commits
  behind and the diff was 164 MB and took about 7 s. Both reads hit the 8 s cap,
  the patch came back empty, "Merge & clean up" refused for want of the
  requested PR, and every retry froze the window again. The diff base is now
  the merge-base of the worktree `HEAD` and the session's base branch whenever
  `baseRef` is an ancestor of it (22 KB and 0.08 s for the same session), the
  reads go through `runGitCapture` (worker plus pumped wait), and
  `m_agentPullCaptures` stops a second landing for the same session from
  starting while one is pumping.
- **Actions workspace quota (92 s, 6 stalls of 8-25 s).** After each clean step
  `ActionRunner` added up every file in the sandbox inside `onProcessFinished`.
  `checkWorkspaceQuotaAsync` (phase `QuotaCheck`) now runs the walk on a worker;
  the next step still waits for the verdict, which is dropped if the run was
  stopped, timed out or replaced meanwhile. `ActionStore::approve` (5 stalls,
  6 s) queues the approval and writes the settings on a worker one write at a
  time; `isApproved` and `lastApprovedContent` read the queued approvals until
  they are on disk. Step processes start with `UseVFork` (Qt 6.7+): a full
  `fork()` of the desktop under memory pressure took 1.2 s. The child-side
  modifier only calls `setsid`, `umask` and `setrlimit`, which are safe there.
- **Pings page (11.4 s and 2.8 s at zero pressure).** The two records name
  `eventFilter`/`handleFramelessResizeEvent`, but both samples sit in
  `refreshNotificationsTable`. It added rows one at a time while seven columns
  were still in content-fit mode (`makeColumnsResizable()` freezes them only on
  the next event-loop turn), so each `setItem()` re-measured those columns over
  every row. It now sets the row count once and fills rows by index: 3.5 s to
  about 50 ms for the first visible fill of 140 pings in an offscreen benchmark.
- **Chat members (14.6 s, 2 stalls).** Every relay roster update rebuilt the
  member cards in a popup that is normally closed, and hiding old cards made Qt
  create an accessibility object per child widget. `refreshChatMembers` now
  updates only the heading and count while the menu is closed
  (`m_chatMembersMenuOpen`) and rebuilds the cards when the members button
  opens it.
- **Deploy-log follower (24 s, 15 stalls, one of 15 s).** `DeployLogFollower`
  listed and read the autodeploy and ops log folders every 2 s and rewrote and
  fsynced its cursor file on the GUI thread. Polling and cursor writes now run on
  the follower's own one-thread pool, so they run in queue order and an older
  cursor can never overwrite a newer one; an unchanged cursor is not written.
  The destructor waits for the pool.
- **Private mirror idle check (15 s, 5 stalls).** The quiet pass of
  `syncPrivateRepository` ran `mirrorRefsDigest` (`git for-each-ref`) on the GUI
  thread for each private repository in the sweep. It now runs through
  `runOffThread` and re-checks that the repository record is still the same
  before acting; the post-seal fingerprint is computed inside the seal worker.
- **UI sounds (12.4 s, 2 stalls).** `sonify::Player::start()` posted
  `Pump::open()` with a blocking queued call, and `open()` waits up to 1.5 s for
  `pw-cat` and then tries the next player. UiSounds closes the player after
  15 s idle, so the first click after a pause paid for a process start. The
  pump now opens the player on its own thread and records the result in an
  atomic; `start()` returns at once. A player that cannot start is reported on
  the next `start()`, which is when UiSounds stops trying.
- **Repository list writes (18 s, 19 stalls).** `saveRepositories` rewrote
  every key of every repository, so each call re-read, rewrote and fsynced the
  320 KB settings file, mostly when a mirror sync finished. It now writes only
  keys whose stored value differs, and a change to an existing `lastSyncMs`
  alone waits for one 30 s timer (`flushRepositorySyncTimes`, also run from
  `~MainWindow`). A worker would not help: `QSettings` shares one in-memory file
  per process, and the next `QSettings` on the GUI thread would sync it.
- **Agent session list (7.5 s and 2.4 s).** `loadAllSessions` and
  `loadRecycledSessions` re-read and parsed every `meta.json` on each
  `reloadAgents`. Parsed sessions are now cached per path while size and
  modification time (and `deleted-at.json` in the bin) are unchanged;
  `saveSession` drops its path and bumps a generation counter so a concurrent
  load never stores a parse older than the save. `continueAgentSession` no
  longer calls `showAgentSession` right after `reloadAgents`, which already
  shows the selected session.

Left alone: records inside `QDialog::exec` of `killHighMemoryProcess` are
other work running in the dialog's nested loop, and `ActionStore::saveRun`
(one 2.9 s record under starvation) still writes on the GUI thread, because
a late background write could bring back the `meta.json` of a deleted run.

### 2026-09-28 stall pass, second session

Triage of the 56 records from pid 815281 (10:26 to 10:34). That binary was
built before the first pass above was merged, so its agent-landing, quota,
deploy-cursor and session-list records are already fixed. From 10:31 the
machine was starved (memory PSI up to 52 %, io full up to 51 %), and those
records carry the starvation verdict. The reported record,
`STALL-20260928T103315819-p815281-0044`, is one of them: the GUI thread
waited in `QWaylandShmBackingStore::recreateBackBufferIfNeeded` for the
compositor to release a buffer while repainting a background-task chip. The
fixes below remove GUI-thread work the records and the action timeline
point at.

- **Background task chips.** `tickBackgroundQueue` runs every 90 ms while
  work is queued. It reset every chip's tooltip on each tick (a
  `ToolTipChange` event each time) and repainted chips nobody could see. It
  now sets the tooltip only when the text changed and turns the spinner only
  while the chip and the window are visible and the window is not minimized.
  `startButtonSpin` frames skip hidden buttons the same way.
- **Node commit events (1.7 s).** `drainNodeCommitEvent` compared the event's
  target state with `mirrorStateHash` (`git for-each-ref` behind a pumped
  wait) on the GUI thread, reached from `updateFooterDiagnostics`. The hash
  now runs through `runOffThread`. `m_mirrorEventHashChecks` holds one check
  per repository, and a newer event waits in `m_pendingMirrorEventStates`
  until the check lands. On a mismatch the pending state becomes empty, which
  means "sync without comparing", and the drain runs again. A sync that
  started meanwhile goes through the existing one-second retry.
- **Issue badge (1 s).** `updateRepoIssueCount` read the primary branch tip
  and ran `ls-tree` three times plus `cat-file --batch` on the GUI thread for
  every issue list change. It now follows `updateRepoPullCount`: a worker
  counts, a generation counter drops stale answers, and a call while one is
  in flight queues exactly one rerun. The checkout count is shown at once
  when the repository (its mirror) changes and when the mirror count fails;
  otherwise the badge keeps its value until the mirror answers.
- **ForkMesh.conf writes.** The settings file is about 345 KB. A `QSettings`
  built while any instance has a pending change takes the lock, re-reads the
  file, rewrites all of it and fsyncs it.
  - `RoomSeal`'s constructor called `remove()` on the obsolete v1 phrase key
    whether or not it existed. Qt queues a removal either way, so each seal
    built (one per chat room in `OfficeChannelMirror::applyHistory`) rewrote
    the file. It now uses `forkmesh::settings::drop`.
  - `setOrganizationTaskBadge` wrote the count on every task refresh. It now
    remembers the stored count (`m_organizationTaskBadgeStored`) and writes
    only a change, through `put`.
  - `applyClaudeReset` reads the active account before opening its own
    `QSettings`. `activeAgentAccount` builds a second instance on a cache
    miss, and after the first `put` that instance flushed it.
    `scheduleUsageLimitReminder` writes its scheduled time through `put`.
  - `configuredRelayHost` built a `QSettings` per call from
    `refreshSourceControlOutgoing`, `loadCommits` and `relayPublishRepo`.
    The host is now cached process-wide.
    `forgetConfiguredRelayHost()` clears it wherever `server/url` is written
    (`saveServers`, the server field's `textEdited`, the setup save).
- **Chat room keys.** The first history reply for a room built its
  `RoomCrypto` on the GUI thread: 210k PBKDF2 rounds (0.2 s or more under
  load) per room at startup. `fetchHistory` now warms the key on a worker with
  `RoomCrypto::warmKey`, as the access path already did. The room stays
  marked `fetching` until the key is ready, then the history is applied.
- **Agent session saves (0.85 s and 6 s).** `AgentStore::saveSession`
  rewrote and fsynced `meta.json` on every turn result, permission prompt and
  status change, even when the JSON was byte-for-byte the same. It now
  remembers the bytes and the size and modification-time stamp of its last
  write per path. It skips the write while both match, so a file changed on
  disk since (bin restore, another writer) is still rewritten.
- **Teammate mentions.** `applyRepoMentions` checked every issue and pull
  event for a teammate mention before looking at `mentions/seen`. Each check
  re-listed the teammate directory and compiled three regular expressions per
  teammate. Seen events are now skipped first, the teammates load once per
  pass, `mentionHandle` uses static patterns, and `textMentionsTeammate`
  keeps the compiled pattern per handle (`thread_local`, at most 256).
- **Transcript clear.** `ClaudeTranscriptView::clear` removed rows with
  `takeAt(0)`, which shifts the whole item list each time. It now takes them
  from the end with updates off, and returns at once when there is nothing
  to remove.
- **Debug log tail.** The five-line tail under the debug bar is usually
  hidden, but it parsed every log line as HTML into its document. It now
  takes lines only while shown and is seeded from `m_networkLog` each time it
  opens (`seedDebugLogTail`), instead of once while the status area is built.

Left alone: `listRepoBranches` and `branchWorkDir` still wait for `git branch`
and `git rev-parse` in pumped waits (usually 30 ms, up to 0.5 s under load),
behind the 5 s branch cache. Rich-text layout records in the agent transcript
(`renderTranscriptSlice` is already sliced) and the frameless-resize records
with no app frame other than `eventFilter` showed no app work to move.

### Git timeouts, index.lock and agent pull capture (2026-09-28)

- **Timed-out git left `index.lock` behind.** `gitTimeoutError()`,
  `runWorkerProcess()` and PullStore's `killTimedOutProcess()` ended an
  overdue git with SIGKILL. Git removes its lock files from a SIGTERM
  handler, and SIGKILL skips that. `git merge --autostash --no-edit main` in
  an agent worktree hit the 8 s cap on 2026-09-27 at 18:54 and 18:55, and
  every later merge or update there failed with "fatal: Unable to write
  index." until the age-based `removeStaleGitLocks()` cleared the lock. All
  three now go through `forkmesh::stopProcessGracefully()` (`GracefulStop.h`):
  SIGTERM, up to 2 s for the process to exit, then SIGKILL. A lock that a live
  git holds is never deleted. The existing age-based sweep still clears locks
  left by crashes.
- **Index-writing git got the 8 s read cap.** `waitForGit()` capped every
  command at 8 s. `gitWaitTimeoutMs()` now gives `add`, `am`, `apply`,
  `checkout`, `cherry-pick`, `clean`, `commit`, `merge`, `mv`, `pull`,
  `read-tree`, `rebase`, `reset`, `restore`, `revert`, `rm`, `stash`,
  `switch`, `update-index` and `worktree` 60 s; reads keep 8 s. This fixes
  "Stage all: git timed out after 8s: git add -A". PullStore's
  `updateBranchFromBase()` merge gets 60 s too.
- **Merge failures with no reason.** Git writes some refusals only to stdout
  ("nothing to commit", "Automatic merge failed"), and the merge paths quoted
  only stderr. `GitCaptureResult::failureOutput` keeps stdout on failure, and
  `gitFailureDetail()` falls back to it, then to "git merge failed without
  saying why". `startBranchMergeEditor()` also used to ignore a failed
  `merge --no-commit` and then report the empty stderr of the `commit` that
  followed; it now reports the merge's own error. Callers covered:
  `prepareWorktreeMerge`, `mergeBranchIntoBase`,
  `mergeBaseIntoBranchCheckout`, `startBranchMergeEditor`,
  `prepareBranchConflictFix` and `PullStore::updateBranchFromBase`.
- **Upstream check spawned git on every refresh.**
  `upstreamPublishesThroughMirror()` ran `git rev-parse --abbrev-ref
  --symbolic-full-name @{upstream}` and `git remote get-url` every time the
  repo detail or a branch diff refreshed. On 2026-09-27 those cheap reads
  failed with 18-26 s "timed out" waits while other git work was running.
  The check now reads HEAD from the file, `branch.<name>.remote` and `.merge`
  through `cachedGitConfigLocal()`, and the URL from `cachedRepoRemotesRaw()`
  (`cachedRemoteFetchUrl()`). All three caches are keyed on the config file's
  mtime and size, so git runs only after the config changes.
- **Agent pull capture froze the window and reported "No code changes".**
  In the stall log for 2026-09-28 (01:12 to 12:42), 18 records named
  `maybeCreatePullForStreamSession`, each 8 to 16 s and 167 s in all. Each
  was one capture whose `git diff --binary` hit the 8 s cap. The empty result
  was logged as "==> No code changes; no pull request created.", and "Merge &
  clean up" then refused with "Couldn't create the requested PR for Agent #N":
  seven times for #635, three for #576 and four for #605 before the capture
  succeeded. The capture (`captureAgentPull()`) now runs on a
  `runOffThread` worker through `runWorkerProcess` with a 120 s cap
  (`kAgentPullGitTimeoutMs`). A read that fails or times out is logged as
  "!! Could not create pull request: …", not as "no code changes".
  `maybeCreatePullForStreamSession()` takes a continuation.
  `mergeWorktreeIntoMain()` waits for every requested PR, then runs itself
  again (keeping `m_worktreeMergeRunning` set until then, so other merges
  queue). If the repository changed meanwhile, or a PR is still missing, it
  refuses. The yolo finish path runs auto-merge, task completion and worktree
  cleanup after the capture, because cleanup deletes the worktree the capture
  reads. The Create PR button reports its result from the continuation.
  `PullStore::createPull` still runs its git in a pumped wait on the GUI
  thread.

Not changed: branches deleted mid-refresh. In the 2026-09-27 log,
`readAgentDiffStat` and the branch diff hit "ambiguous argument
'458-error-forkmesh-diagnostic'" and "unknown revision". Both already check
the ref before reading (`rev-parse --verify refs/heads/<b>` and the
`branchGone` fallback), so these were races with a cleanup running at the
same time. None appear after 2026-09-27 15:00.

### 2026-10-07 diagnostic report #2

The stalls are under "2026-10-07 settings fsync and avatar records", "Users
page stall on first open" and "Agent diff refresh cadence" above.

- **The `WindowFrameControl::watchGeometry` SIGSEGV (21:23)** came from a
  build older than e3173be7c6, which fixed it.
- **"Action failed before UI refresh" groups.** Most are not desktop faults:
  runs refused because the node had less free disk than a run's 2 GiB
  workspace quota, and real CI failures (a `test_prune_merged` fixture fixed
  in bffc9320da, a wrangler custom build, `forkmesh-mirror-fleet-window-tests`).
  The checkout failures were a race on the mirror's worktree list, now
  retried ([README.md](README.md), "A checkout that loses a race").

### 2026-10-07 diagnostic report #3

- **Market stall, 2.9 s.** The `likely origin` named
  `PackageRegistry::isInstalled`, but that frame was stale stack data; the
  samples were rendering SVG previews. Fixed by the preview cache in
  [../marketplace/README.md](../marketplace/README.md), "Desktop Market
  preview cache".
- **SIGSEGV in `WindowFrameControl::watchGeometry` (2026-10-06 21:23)** came
  from a build older than e3173be7c6, as in report #2.
- **SIGSEGV in `QWaylandXdgSurface::requestActivate` (2026-10-07 01:23)**:
  a transcript caption label shown before it had a parent
  ([agents-ui.md](agents-ui.md), "Wayland crash from a caption label").
- **84% CPU and typing lag.** The Qt runtime section showed about 4,300
  paint events a second with the app idle (1.84 M in six minutes, 270 µs
  average), and the main thread at about 70% CPU for its whole life. The
  report named only the slowest receiver class (`MainWindow`), not which
  widget kept repainting, and attaching a debugger to the 288 MB binary
  would have frozen the app. The detailed sampler now tallies paint events
  per widget, named by class and the nearest `objectName` up the parent
  chain ("QLabel in agentTable"), at most 256 names with the rest folded
  into "(other receivers)". The report lists the top eight under "Most
  painted widgets this session", so the next report says what to stop
  repainting. `test_qt_runtime_diagnostics` covers the tally and its reset.
- **"Action failed before UI refresh" groups** are CI and deploy failures
  of action runs, not desktop faults, as in report #2.

### Salvage of the 2026-09-27 fix branches (2026-09-28)

`fix/actions-failures`, and the uncommitted work left in the
`fix/desktop-ui-freezes` and `fix/desktop-git-errors` worktrees, were
compared with `main` after about 630 newer commits. Ported, re-applied to the
current code with tests:

- **Actions workspace clone survives a torn ref** (run 331, 2026-09-27: "git
  upload-pack: not our ref 0000..."). The clone is `--single-branch`; see the
  Actions traps in [README.md](README.md#traps).
- **Failure diagnostic masked the step command.** `crashContextHeader()`
  printed the command after `${{ vars.* }}` substitution; it now goes through
  `redact()` like the run log.
- **Old branches ran a retired push workflow.** Pushes to branches cut before
  `.forkmesh/deploy.yml` became manual-only still queued "Deploy Cloudflare
  Worker" (09-27 17:04, `3e740366`); see the README trap.
- **Team room members** (`refreshChatMembers`). For `#team` every history
  message was compared with every roster entry through
  `chatUserDisplayName()`, about 8 million calls for 20,000 messages and 400
  roster rows, and this runs whenever the member count refreshes, not only
  when the menu is open. It now builds id and display-name lookups once
  (the first roster match still wins) and looks at each distinct sender once.
  Window test "the Team room finds a speaker's account among 20,000 messages".
- **Cached branch diff repaint.** `renderBranchDiffPatchNow()` renders the
  cached patch (up to `kBranchDiffCacheBytes`, 8 MB) on the GUI thread for the
  instant repaint of adhoc #227. A cached patch over 256K characters now goes
  through the worker render (`renderBranchDiffPatch`); smaller ones still
  repaint on the next event-loop turn.
- **An agent branch with nothing to land** no longer blocks "Merge & clean
  up". When the PR capture finds no changes (the branch is already in its
  base and the worktree is clean), the session is recorded in
  `m_agentPullNotNeeded`; the merge then runs once more with that session in
  `m_agentPullSkipOnce`, so it merges and cleans up without the PR, and logs
  why. The skip is used by that one re-run only; a later merge captures again.
- **Worktree status could not be read** during "Merge & clean up": the
  worktree was treated as clean. It now refuses and quotes git's error.

Tests: `forkmesh-action-security-tests` (torn ref, masked diagnostic,
`defaultBranchRetiresPush`; the first two fail without the fix);
`forkmesh-window-tests --teammates-only` (the Team room scan: 22 ms, 1.3 s
before) and `--agent-landing-only` (the branch with nothing to land; fails
without the fix). The landing check lives in
`checkRequestedPullFailureReason`: run earlier in the full suite, its
background cleanup was still settling when the Pull main checks ran.

Already on `main`, so not ported: the Commits tab, code overview and rebuild
source check off the GUI thread (`550def3922`); approvals written off the GUI
thread (`028a1f0ca7`, queued approvals); range-file rows in batches
(`ea73005744`); the Artifacts tab and Mirror nodes reads on workers with
refs-signature caches; `repoBranches` from the ref files; the Desktop CI
agent-efficiency test (`71d13b52b4`); `GIT_OPTIONAL_LOCKS=0` for reads, lock
contention retries, 60 s for index-writing git, SIGTERM before SIGKILL and the
stdout fallback (`91ed2be814`); "Merge & clean up" around unrelated dirty
files (`ac061e89c3`). Landed on `main` by `35e3835efc` while this salvage was
in progress, so the ported copies were dropped at the merge in favour of
`main`'s: the held-back release candidate (`stillNamesAnotherRelease`),
merging a remote-only branch (`gitrefusal::mergeSourceFromRefs`), naming the
files behind a "would be overwritten by merge" refusal for both the worktree
update and a pull request merge, Delete merged branches skipping worktree
branches, and the merge editor commit reason.

Dropped:

- `GitWriteGuard` (a per-repository write lock, 10-minute write deadlines,
  deleting per-worktree locks after 2 s when no git process is seen, and
  aborting a half-done merge/am/rebase after a kill). `main` chose to keep
  git's locks and let a timed-out git drop its own lock on SIGTERM
  (`91ed2be814`); deleting a lock another process may own, or aborting a
  user's merge, is not safe. The per-repository serialization it wanted
  arrived with branch `572-…` as `GitIndexQueue` and its safe `index.lock`
  recovery ([git-locking.md](git-locking.md)).
- Settling diff end-cap resizes for 150 ms during a window resize. The
  resize stalls in the 2026-09-28 stall log sit in
  `QTextDocumentLayout::doLayout` under `handleFramelessResizeEvent`, the
  relayout Qt does for any width change, which that change does not remove.

## Crash handling and crash repair

- `src/CrashHandler.cpp` installs signal handlers and writes a crash block to
  the log file descriptor. A `SIGTERM` record carries a "shutdown signal"
  heading and is not treated as a crash.
- While an Action runs, the handler survives a stop signal so the run is not
  cut off mid-write. It also records that one arrived
  (`forkmesh::terminationSignalReceived()`); the Actions code reads that flag
  to tell a run that was killed by the stop from one that failed
  ([README.md](README.md#traps), "A stop signal reaches the Actions runner's
  children").
- On the next launch `src/CrashLogScan.*` scans `network_log.txt` and the newest
  rotated segments for crash blocks newer than 7 days.
  `MainWindow::dispatchCrashRepairs()` (`src/MainWindowSettings.cpp`) queues
  each unrepaired crash to every enabled teammate that owns error-log repair
  (`.forkmesh/teammates/`), ahead of the ten-error batch. The owner is the
  `error-fixer` special agent or one with an active `ERROR` monitor.
- Switch: setting `diagnostics/autoAgentOnErrorLog` (turn off on a node that
  should never start coding agents). Watermark:
  `diagnostics/crashRepairObservedEpoch`.
- A run that ends without Success holds its queue
  (`LogMonitorRuntime::deferRetry` in `src/TeammateStore.h`) instead of
  re-dispatching at once.
- Crash repairs are credited against the app's own repository.
- The handler writes each part of the record with one `write()` (banner
  through context, the compact `ForkMesh signal:` line, each backtrace frame,
  the footer). The log writer thread keeps running while the handler runs, and
  when the header went out a few bytes at a time its lines landed inside the
  record: "signal: 2026-09-24 17:38:14  Status: Disconnected..." became the
  signal name, and a status line became a backtrace frame. Whole lines from
  that writer can still fall between two parts, so the reader
  (`parseCrashRecords`) skips any `YYYY-MM-DD HH:MM:SS  ` app-log line inside a
  block, cuts one appended to a header or frame line, and takes a header value
  from the next line when the other writer split it off. Header fields are read
  only before `context:`/`backtrace:`, so action output cannot overwrite them.
- The handler's context buffer holds 4095 bytes
  (`forkmesh::kCrashContextCapacityBytes`) and used to keep the end of what it
  was given. An action run's context is its run header plus up to 8 KB of
  process output, so the header was cut off and the record showed build lines
  such as "dow-tests.dir/src/D1QuerySource.cpp.o". `ActionRunner` now sets the
  header plus as much of the output tail as fits, starting on a whole line
  (`forkmesh::fitCrashContext`).
- A crash batch is not re-dispatched forever:
  - A repair session that ran and ended without Success hands its batch back
    no more (`MainWindow::settleWorkedCrashRepair`). Only a run that a usage
    limit refused puts the batch back and waits for the reset. Before this, one
    batch of 42 crashes went back to the queue after every attempt (7 by
    2026-09-28), including crashes fixed by hand on 2026-09-18 and 2026-09-25.
  - Queued crashes recorded by an older version whose signature (signal plus
    innermost named frame outside libc, `crashlog::signature`) has not recurred
    in the running version are set aside
    (`crashlog::supersededByNewerBuild`). A recurrence in the running version
    keeps every record of that signature queued.
  - Both write a "Reliability monitor" line to the log. The crash blocks stay
    in the log file and in the Log view.

### Crashes 2026-09-18 to 2026-09-28

| When | Build | Signal / innermost frame | Status |
|---|---|---|---|
| 09-18 16:09 | 1.2.1 | SIGSEGV `QString::operator=` in `startCliTranscript` from `processAgentQueue` | Fixed 09-18 (`4dfb4b3d0`, `bc4361aa0e`): the queue pump passes a session copy. Guarded in `tests/validate_error_log_regressions.py`. |
| 09-22 20:57, 09-23 21:33 | 1.2.3 | SIGABRT `malloc(): unaligned tcache chunk` in a worker `QThread` (`QtRuntimeDiagnostics::noteDelivery`) | Heap corruption found on allocation. The frames do not name the writer, but the abort sits in a racy one: see **Delivery probe on worker threads** below. Not seen since 1.2.4. |
| 09-23 20:35 | 1.2.3 | SIGABRT `tcache_thread_shutdown(): unaligned tcache chunk` at thread exit, during action run 341 | Same corruption, found at thread exit (message from the user journal). Not seen since 1.2.4. |
| 09-24 11:57 to 09-25 10:18 (30: 20 and 10) | 1.2.4 | SIGSEGV `ServerNode::onSocketReadyRead` / `NodeEventSocket::drainReadBuffer` | Fixed by `297e41a5ed` (09-25 11:00), in every 1.2.5 build (first 1.2.5 session 09-25 14:49). No such crash since, across about 230 relay reconnects. |
| 09-25 17:46 to 17:47 (7) | 1.2.5 | SIGABRT `qFatal` in `QGuiApplicationPrivate::createEventDispatcher` | `forkmesh-source.service` restarted the app while GNOME Shell was shutting down. `wayland-0` still existed but refused connections, and the readiness check only tested that the file existed. See below. |
| 09-27 21:45 | 1.2.5 | SIGSEGV `QWaylandWindow::shellSurface` from `QWaylandXdgSurface::requestActivate` in `showHighMemoryProcessPanel` | See below. |
| 09-27 22:29 | 1.2.5 | SIGSEGV in the command-approval resolver under `ClaudeTranscriptView::addCommand` | See below. |
| 09-28 01:08 | 1.2.5 | SIGSEGV `QObjectPrivate::maybeSignalConnected` from `QIODevice::channelReadyRead` in a pumped wait (`PullStore::readPull`) | See below. |

- **Display readiness.** `DesktopDisplayReadiness` now connects to the
  Wayland or X11 socket (`desktopEndpointAcceptsConnections`). A refused
  connection means waiting, then exit code 3 with "cannot start its desktop
  UI". Qt no longer aborts in `QApplication`.
- **High-memory panel.** When memory reaches 85% the panel opens on its own
  (`QTimer::singleShot`), often from inside a pumped wait. On Wayland,
  showing it asks for activation. Qt 6.8.2's
  `QWaylandXdgSurface::requestActivate` reads
  `QGuiApplication::focusWindow()->handle()` without a null check, and the
  fault address was 0x58. The automatic open now shows the panel without
  activating it (`WA_ShowWithoutActivating` and the window's
  `_q_showWithoutActivating`), which skips that path in the xdg-shell plugin.
  A click on the chart still activates it. The focused
  `forkmesh-high-memory-panel-window-tests` regression verifies both the
  automatic non-activating window state and the interactive state, while the
  error-log contract pins the automatic caller to the former.
- **Command approval chip.** The resolver took `findAgentSession()` and read
  `session->mode` after `sessionWorkdir()`. That can list worktrees through
  `runGitCapture`'s nested event loop, which can reload `m_agentSessions`. The
  22:29 fault PC maps to `lock addl` on `session->mode`'s d-pointer (lambda
  `operator()+0x23c`; the source is unchanged since 09-21). The mode is now
  copied before anything that can pump events. Guarded in
  `tests/validate_error_log_regressions.py`. A second pass (2026-09-28) takes
  the worktree from `cachedSessionWorktreeForUi()` instead of
  `sessionWorkdir()`, so drawing a chip no longer waits on git at all. See
  [agents-ui.md](agents-ui.md).
- **Delivery probe on worker threads.** `ForkMeshApplication::notify` runs on
  every thread, and each delivery gets a `QtEventDeliveryProbe`. With detailed
  diagnostics on, a worker thread's probe wrote the GUI thread's event stats in
  `QtRuntimeDiagnostics::noteDelivery`. That includes replacing the
  `slowestReceiverClass` QStrings, with no lock, while the GUI thread wrote the
  same strings. Two threads assigning one QString can free its buffer twice.
  That is the heap corruption behind the 1.2.3 aborts, which were found inside
  `noteDelivery`. The probe now records only deliveries on the diagnostics
  object's own thread. The application event filter that counts events already
  saw only that thread. Test: `forkmesh-qt-runtime-diagnostics-tests` (a
  worker-thread delivery leaves the stats empty).
- **Socket deleted during its own `readyRead`.** A frame handler can run a
  pumped wait (`PullStore`'s `waitForFinishedKeepAlive`). If the link drops
  inside that pump, `discardCurrentSocket`/`closeCurrentSocket` called
  `deleteLater()` one scope level deeper than the pump, so the pump deleted
  the socket while its `readyRead` emission was still on the stack. Qt then
  touched the freed object in `channelReadyRead` (the fault address was 0x48,
  a garbage `ConnectionData`). `ServerNode` and `NodeEventSocket` now keep a
  socket they drop during one of its signals until that dispatch unwinds
  (`src/SocketRetirement.h`). Test:
  `testNestedPumpInFrameHandlerKeepsDeliveringSocket` in
  `tests/test_chat_transport.cpp`.

## Self-diagnostics: relay-flap (2026-09-29)

`src/NodeDiagnostics.*` reports `relay-flap` ("Relay link dropped N times in the
last 30m") from `Collector::noteRelayDrop`, which `ServerNode::handleLinkLost`
calls. A phone showed a Warning for 12 drops, but the count was inflated in two
ways:

- `handleLinkLost` also runs when a reconnect attempt fails (connect timeout,
  refused upgrade, socket error before the 101). Each retry during one outage
  counted as another drop, and the backoff starts at one second per endpoint.
  Losing the network (`setNetworkAvailable(false)`, e.g. sleep or Wi-Fi change)
  counted too. Now only the loss of an established link counts, and only
  while the network is up.
- Each office-channel mirror room runs its own `ServerNode`
  (`OfficeChannelMirror`), and all of them share the host collector. One relay
  restart dropped every room socket at once. `noteRelayDrop` now ignores a drop
  that comes within `kRelayDropCoalesceMs` (15 s) of the last one it counted, so
  links that fall together count once.

Test: the relay-flap checks in `tests/test_crypto.cpp`.

## Health page (component health)

Rail **Health** (section 24). `src/CodeHealth.{h,cpp}` scans the checkout off
the GUI thread and scores each file and function; `src/ComponentHealthBoard.*`
draws one small card per file; `src/MainWindowComponentHealth.cpp` is the page.
`tools/code_health.py` applies the same rules from a shell. Tests:
`forkmesh-code-health-tests`, `forkmesh-component-health-board-tests`.

### Binary size view

The page's **Binary size** toggle shows the running executable as a treemap:
every byte of the file, grouped into app code (component, then source file,
then symbol), shared template and inline code, libraries and runtime, and the
sections no symbol names (string literals, unwind tables, dynamic-linking
tables, debug info, the symbol table itself, headers). The top-level groups
always add up to the file size. Click a block to zoom in; right-click,
Backspace or the breadcrumb to zoom out.

`src/BinarySize.{h,cpp}` does the reading, on a worker thread (for the 200 MB
RelWithDebInfo build: about 4 s from a cold disk cache, 0.4 s warm). It is a
small ELF64 reader with no dependencies:

- Sizes come from the linked binary's own symbol table (`.symtab`, else
  `.dynsym`), so they are what was actually linked: after the linker dropped
  duplicate template copies and unused sections. Symbols that share an
  address, like constructor aliases, are counted once.
- To name the file a symbol came from, it reads the object files from the
  target's link line in `build.ninja` (else `CMakeFiles/<target>.dir/link.txt`,
  else every object in that folder). A global defined in exactly one object
  belongs to that object's source; one defined in several is a template or
  inline copy and goes under **Templates & inline**, grouped by class. Local
  symbols follow the symbol table's `STT_FILE` markers. An object listed on the
  link line but missing on disk still names its local symbols.
- An installed build has no build folder: local symbols are still named by
  file, globals are grouped by class under **Globals (no build folder)**, and
  the status line says so. A stripped build keeps only the exported symbols
  (`-rdynamic` makes that most of them); everything else shows as section
  bytes.
- Only 64-bit little-endian ELF is read. On macOS and Windows the view says the
  binary could not be read.
- Each group keeps its 150 largest symbols and folds the rest into one
  "N smaller symbols" block, which holds the tree to about 25k nodes.

`src/BinarySizeMap.*` draws the map with squarified layout, two levels at a
time, one fixed categorical hue per top-level group so a group keeps its colour
when you zoom. `binaryComponent()` in `src/MainWindowComponentHealth.cpp` maps
a source path to its component: resources, Qt meta-object code, marketplace
packages, and `codehealth::componentFor` for `src/`. Test:
`forkmesh-binary-size-tests`, which reads its own executable.

## Memory page

Rail **Memory** (section 25, the last deferred section):
`src/MainWindowMemory.cpp`, `src/ProcessMemory.*`, `src/MemoryUsageChart.*`.
A live breakdown of this process's mappings. Test:
`forkmesh-process-memory-tests`.

### Memory map

**Memory map**, beside **Agent governor** in the page heading, opens
`openMemoryMap()` (`src/MainWindowMemory.cpp`): the same 2D treemap and 3D view
as the repository code map (`CodeMapView`), fed with memory instead of files.
Each mapping group (heap, anonymous, thread stacks, shared libraries...) is a
folder and each merged mapping a cell inside it, coloured with the page's group
colours so the legend matches the chart and the tree. Up close a cell shows the
mapping's path and its resident, private, dirty, shared, proportional, swap and
virtual bytes, drawn the way the code map draws source.

- The size picker switches what a cell measures: resident (default),
  proportional, private, virtual, swapped out, or **Inside the app**, which maps
  the app's own probe counts from the "Inside the app" tab instead of kernel
  mappings. Cells of zero bytes are left out.
- It is a snapshot taken off the GUI thread when the window opens, not a live
  view: rebuilding the layout every second would move every cell under the
  pointer. **Refresh** samples again and keeps the focused group or mapping.
- `CodeMapModel` builds both maps: `buildTree` parses `git ls-tree` into
  `Entry` records and hands them to `buildTreeFromEntries`, which the memory
  map calls with group/mapping paths. `CodeMapView::setWording` replaces
  "repository/folder/file" in its tooltips and accessible text, and
  `setCategoryColors` pins a category's colour instead of the ranked hue slots.
- Added 2026-10-07 so the page's breakdown can be seen at a glance, by size,
  the way the code map shows a codebase.

## High memory usage window

`showHighMemoryProcessPanel()` in `src/MainWindowDiagnostics.cpp` pops up on its
own when host memory crosses 85% (re-armed below 80%), and opens on a click of
the memory quadrant of the resource chart. It lists the top 30 processes by
resident memory with Kill, Kill all and Stop agent.

The checkbox beside Refresh ("Kill the largest processes, one at a time, until
memory drops below 85% when this window pops up", setting
`diagnostics/highMemoryAutoKill`, off by default) makes the automatic pop-up
send SIGTERM to the largest listed process once the first scan finishes. It
walks down the list past PID 1, ForkMesh itself and anything it is not
permitted to signal. Ending one process is often not enough, so
`continueHighMemoryAutoKill()` then checks host memory every second: below 85%
ends the round; otherwise, once the process has exited or five seconds have
passed, it sets `m_highMemoryAutoKillPending` again and rescans, and the next
largest process not already signalled this round is ended. The round stops
when memory is below 85% or no listed process is left, and the status line
names every process it ended.

The pop-up sets `m_highMemoryAutoKillPending` only when the setting is on.
`endHighMemoryAutoKill()` clears the flag and the round's signalled PIDs when
the round finishes, when the box is unchecked, when the window closes and when
a new pop-up starts a fresh round, so opening the window by hand never kills
anything.

## Stats page

Rail **Stats**: `src/StatsDashboard.*`, `src/MainWindowStats.cpp`,
`src/RepoStatsStore.*`. Tile layout is persisted and has a Reset. Tests:
`forkmesh-stats-dashboard-tests`, `forkmesh-stats-window-tests`,
`forkmesh-repo-stats-tests`.

## Background timers

**Settings → Background timers** (`src/SystemdTimers.*` for reading and
commands, `src/SystemdTimersPanel.*` for the section) lists this computer's
systemd user timers named `forkmesh-*` and `fmnext-*`. Each row shows the
timer's description, whether it is on, its next and last run, the last result
of the service it starts, the command that service runs and its drop-in
overrides. **Turn off** runs `systemctl --user disable --now` and
`reset-failed`; **Turn on** runs `enable --now`; **Run now** runs
`start --no-block` on the service. The section only appears where
`systemctl` exists, so Linux only.

- Added 2026-10-02 after a leftover `forkmesh-release-android.timer` kept
  starting failing Codemagic iOS builds every hour, and nothing in the app
  showed which timers were running or what they ran
  ([../release/README.md](../release/README.md#recent-changes)).
- Most of these units are symlinks into `~/projects/forkmesh/config/systemd`,
  and `disable` removes the symlink, so a timer that was turned off is no
  longer a unit systemd knows about. The section also lists the `*.timer`
  files in that folder (`forkmesh::paths::configRoot()` + `/systemd`), and
  **Turn on** passes the file's path to `enable`, which links it again.
- `systemctl` runs on a `QtConcurrent` worker (15 s cap per call), never on
  the GUI thread. The list reloads when Settings is shown, at most once every
  30 s, and after each button press. Nothing polls.
- It is named "Background timers" because the Timers package already owns
  "Timers" for countdowns and alarms. Its namespace is
  `forkmesh::systemdtimers` because `forkmesh::timers::Timer` belongs to
  that package.
- Tests: `forkmesh-systemd-timers-tests` (`tests/test_systemd_timers.cpp`)
  uses a fake `systemctl`. It covers parsing, timers that are off and no
  longer linked, the button commands, and a failed `disable` showing its
  error.

## What is not an error group (2026-09-24 log triage)

The relay's error log (`/api/desktop-errors`, path `/desktop-error/app`) is for
faults a release can fix. These desktop events were filling it and are now held
back at the source:

- **Firewall blocks.** A favicon probe for an unlisted host is skipped without
  a prompt; it is no firewall decision, so `firewallRequestDecided` is not
  emitted and no "Firewall blocked" ping is filed. A block the operator chose
  in the prompt is shown as a plain (non-error) ping: it is their own answer.
- **Favicons for names QUrl cannot hold** (`host:443`, `a..b`, a name with a
  space). `QUrl::setHost` drops such a name silently and the request went to
  `https:/favicon.ico`. `hostCannotServePublicFavicon()` now rejects any name
  `QUrl` refuses, and `fetchFaviconFromUrl()` never requests a URL without a host.
- **UI freezes.** A freeze the operator sat through (5 s or more,
  `MainWindow::onUiStall()`) is logged, never pinged (2026-09-28). It used to
  raise an error card and a Pings entry through `alertOnLoggedError()`, and,
  on a healthy machine, a relay report that pinged the administrators; a
  freeze on a paging machine opened a fresh group per "likely origin". Now the
  freeze is one ERROR line in the app log carrying the whole watchdog report
  (stall id, origin, blocking call, timing, system state, verdict and every
  sampled stack, lines joined with ` | `). `m_muteLoggedErrorAlert` keeps the
  line out of Pings; it still counts as an ERROR, so it joins the
  Fixer teammate's error batch. The same report stays in `stalls.log`.
- **Git questions answered "no".** `git merge-base --is-ancestor`,
  `rev-parse --verify --quiet`, `merge-tree --write-tree` (conflicts),
  `diff --quiet`, `show-ref --verify`, `symbolic-ref -q`, `check-ignore` and
  `config --get` exit 1 as an answer. `gitProbeAnswerOutcome()` retires those
  tickets as `answered (exit 1)`, which `backgroundFailureText()` treats as
  success, so the outcome line no longer says "N failed" and the substring
  classifier no longer badges it ERROR. Exit 128 and other codes stay failures.
- **`refs/heads/HEAD` probes.** Claude Code records `gitBranch: "HEAD"` for a
  detached checkout; `ClaudeSessionScan` now reads that as no branch.
- **Questions (2026-09-28).** `recordMessageBoxNotification()` filed any
  message box without standard buttons other than OK/Close as informational,
  and `standardButtons()` does not see buttons added with `addButton(text,
  role)`. So "Task deleted — Task <id> was deleted, and its work on this
  desktop is not merged." with its Delete/Keep buttons was reported as error
  group 521. Every button now counts: OK or Close acknowledges; any other
  standard button, or a custom button with the Destructive, Yes or No role, is
  a decision; a custom Accept, Reject, Apply or Reset button is a choice; Action
  and Help buttons (Show Details) don't count.
  `ClientErrorReports::dialogAsksQuestion()` treats one decision or two choices
  as a question, which is neither filed nor reported. A warning with OK (plus
  Show Details) still reports.
- **Reports the relay would refuse (2026-09-28).** `desktop_error_handler`
  in `app/src/entry.py` answers 400 `invalid_desktop_error` when the kind is
  not `dialog`/`notice`/`toast`, the surface is not `app`/`headless`, or the message is
  empty or matches `desktop (?:dialog|notice|toast) \[(?:app|headless)\]` once control
  characters are spaces and whitespace is collapsed. The desktop checked the
  stamp only in `initialPingSync()` (which sets the ping's status) and used
  `\b`, and `reportUserVisibleError()` posted the dialog anyway. An "Agent
  network access" dialog quoting an earlier report went out every two minutes
  and got a 400 each time. `redact()` now turns control characters into spaces
  the way the relay does, and `ClientErrorReports::relayAccepts()` applies the
  relay's rules before a report is queued; a report that fails them stays a
  local ping with that reason. A 4xx other than 408, 425 or 429 ends a report's
  retries (`permanentRefusal()`); the ping reads Failed with the HTTP status.
- **Ending a process that already exited (2026-09-28).** Kill in the high
  memory window showed "Could not kill process — java (PID N): No such
  process". `SystemStats::requestTermination()` returns `AlreadyExited` for
  `ESRCH`; the status line says the process had already exited. Kill all
  already skipped `ESRCH`.

### Open desktop error groups, second pass (2026-09-28)

Every group the desktop had posted to `/api/desktop-errors` (status 521) was
checked against main. The running flagship was an old build, so a row that
recurs for something main already fixes counts as fixed on main. A notice or
dialog that answers the operator's own click is still shown; it is filed as
**Local only** (`kLocalOperationNoticeKind` for a notice,
`kLocalOperationDialogProperty` for a message box) and never reported.

**Real defects fixed in this pass**

- **"Could not mark the flagged lines as safe"** and **"N of M flagged lines
  could not be marked safe"**. The `Secret/token assignment` pattern allows
  whitespace, including a newline, between the key and the value. Dart's
  formatter wraps a long argument as `apiKey:` on one line and the quoted
  value on the next, so the whole-tree scan (the fallback when the push diff
  cannot be read) reported the finding on the `apiKey:` line. The ignore
  marker the operator had written on the value line was not seen, because only
  the first line of the match was checked. `markFindingSafe` then refused
  the finding because line 156 on its own is not a match. The mobile fixtures
  `agent_screen_test.dart:156` and `agent_session_service_test.dart:504` did
  this on every push from 09-27 until `mobile/` was removed. Now the marker
  counts on any line the match spans (`matchSpanHasIgnoreMarker`).
  `markFindingSafe` looks for a match that starts on the finding's line within
  the next 8 lines, and writes the marker at the end of the line where the
  match ends. Branch `572-…` holds the git index queue and does not touch
  this. Branch `575-…` hides these notices as local operations, which would
  have silenced a real scanner bug, so that part was not taken.
- **"Couldn't merge X into main — merge: X - not something we can merge"**.
  The merge flow ran `git merge X` without checking that `X` exists in the
  checkout. `prepareWorktreeMerge` now resolves the name first
  (`gitrefusal::mergeSourceFromRefs` over `git for-each-ref refs/heads/X
  refs/remotes/*/X`):
  - When only remote-tracking copies exist and they agree, it creates the
    local branch with `--track`.
  - When that copy is already in the base, it answers "X is already merged
    into main". This is the 446 case: a second click after the first merge
    had deleted the local branch.
  - When the remotes disagree, or the name exists nowhere, it says so as local
    guidance, not as git's raw refusal.
- **"Fetch failed — check the remotes."** and **"Pull failed — the branch has
  diverged or has nothing to pull from"**. Both ran through `runGitDetached`
  without `mergeStderr`, so git's reason was dropped and the notice guessed.
  The 09-27 pull came right after "Mesh converge … fetch from every mirror
  failed": the edge was down, and nothing had diverged. Now both quote
  `gitrefusal::firstFailureLine`. An unreachable remote says so. A real
  fast-forward refusal or local edits in the way is local guidance.
- **"The signed release manifest does not match the requested tag and source
  commit."** At startup the release resume replays `onReleaseMetadataLanded`
  for a pending publication. While v1.2.5 was held back for its assets,
  `latest/release.json` still named v1.2.4, and each launch reported that as
  a forged manifest. When the landed manifest names another release,
  `releasemanifest::stillNamesAnotherRelease` now logs "remains pending" and
  leaves the intent for the three-minute retry. A manifest for the same tag
  with the wrong commit or digest is still an error.
- **"Deleted N merged branch(es); M could not be deleted (…)"**. "0 behind,
  0 ahead" also matches a fresh agent branch checked out in a linked worktree,
  and `git branch -D` refuses those. The scan now skips branches listed by
  `git worktree list --porcelain` and branches of live agent sessions. A
  branch that still fails is named with git's reason.
- **"Merge editor — Could not commit the merge."** `commitResolvedMerge` kept
  only stderr, and `git commit` says why on stdout. It now quotes
  `gitFailureDetail`. A second call after the merge was already concluded
  succeeds instead of failing.
- **"Couldn't clear website pings (0)"** and **"Delete task — ForkMesh relay
  is rate-limited (HTTP 429); backing off"**. Both requests were refused
  locally by `BackoffSuppressedReply` while the relay host was cooling down.
  Both now wait out the cooldown and retry, up to 5 times.
  **"Delete task — task not found"** means the task is already gone, so it is
  treated as deleted.
- **"Merge pull request — Could not merge the pull request's branch cleanly:
  error: Your local changes … would be overwritten by merge"**.
  `PullStore::mergePull` merges in the operator's checkout. When git refuses
  over uncommitted edits, the message now says so and names the files
  (`pulldecision::localChangesBlockMessage`), and the dialog is local. A
  conflict still reports.

**Not errors, now held back**

- "Worktree X has uncommitted changes — commit or stash them first". This is
  the merge flow's worktree check, and it is also git's "would be overwritten
  by merge" refusal from the update step ("Couldn't update 434-… from main in
  its worktree").
- "Couldn't merge X into main cleanly — … resolve the conflicts". A conflict
  is an input to the operator, with the next step named.
- "Pick a repository before sending a prompt to the cloud", and the other two
  refusals of the same form.
- "Merge pull request — This pull request is already merged/closed"
  (`pulldecision::decidedPullError`, taken from branch `575-…`).
- "File X is not available in this repository" and "Branch X is not available
  in this repository". These answer a click on a transcript link to a name
  this checkout does not have.
- "Teammate already exists there" and "Choose a unique name". These are form
  validation.
- "Secret scanning: push blocked" and "Agent network access — … could not
  resolve". These are questions. Mark safe (`YesRole`) and Retry/Deny (two
  choices) already make `dialogAsksQuestion` true since 91ed2be814. The
  dialog keeps showing.
- The "could not resolve app.forkmesh.com … (DNS denied)" prompts for agents
  #403/#404 were also false: the agent had run `git log … | grep -i dns`,
  and the text it read quoted an earlier failure. `detectAccessFailure` now
  skips a Bash command made only of text readers
  (`agentnet::commandOnlyReadsText`, in `AgentReadOnlyCommand.h`) and lines
  quoted from a file (`path:12:`, `path-12-`, numbered `cat -n`/Read lines).
  It matches network patterns only for Bash and WebFetch.
- "Claude Code is not installed on this device" and the other refusals of
  the pop-out-to-terminal click (`agentTerminalHandoff` blockers). The CLI
  lookup itself was fixed by 645ca28d60.
- A transcript link to a directory (`.worktrees`) opens it in Files. A file
  that the agent's worktree lacks but the checkout has opens from the
  checkout. A slashed name that is not a branch is looked up as a path.

**Already fixed on main, or retired**

- "Firewall blocked GET …": 7e7f4e0b16 (09-24). The later rows came from the
  older running build.
- "Action failed — Build Android app", "Start Codemagic ad hoc iOS build" and
  "Upload iOS build to App Store Connect": the workflows were deleted with the
  Flutter app in f5ecb033ac. "Attach desktop build to release" was fixed by
  b9295c0e7d.
- "Claude Code is not installed on this device": 645ca28d60 finds agent CLIs
  outside the GUI's `PATH`.
- "Could not merge main into 640-…:" with nothing after the colon: 91ed2be814
  (`gitFailureDetail` falls back to stdout).
- "Mirror sync quarantined for record jett/private-repo": c6719b451b stamps
  the private replica's identity on a worker.
- "Could not delete 476-… on forkmesh-mesh": 264de76150.
- "This desktop did not get the run: …": d272ea6807 and 999cc56671, both on
  the relay side.
- Every "UI froze — Desktop UI froze over N s …" row. Since 01bf24d3bc a
  freeze is one ERROR line in the log, never a notice or a report. Of the named
  origins, the ones main still ran on the GUI thread were fixed here:
  - `QtRuntimeDiagnostics::refreshStructure`: the whole-object walk runs at
    most once a minute, without building a set.
  - `AgentStore::saveSession`: on Unix, `meta.json` is replaced by a temp
    file and `rename`, and the `fsync` runs on a one-thread pool.
    `QSaveFile::commit` fsynced on the GUI thread.
  - `Http3Get` connect: one process-wide `X509_STORE` of the system roots
    instead of parsing them for every connection.
  - `recordRailClick`: counts wait in memory and are written after 30 s or
    at quit.
  - `maybeAutoUpdate`: its `git rev-parse` / `for-each-ref` / `rev-list` /
    `merge-base` run in `runOffThread`.
  - `RoomSeal::persistPeers`: a new peer key is written 2 s later, together
    with any others, and `loadPeers` reads the pending copy.

  The other origins are gone from main (550def3922, 028a1f0ca7, 415a7464da,
  ea73005744, c6719b451b). Some stacks name no application work: "application
  frame unresolved", event-probe frames, a Qt leaf, or a machine under
  memory/io pressure. Those have nothing to change.
  `alertOnLoggedError`'s settings read was left as it is. The 09-24 frame was
  an unresolved leaf under pressure, and that function was rewritten since.

Where the pieces came from:

- Branch `575-…` (3f706607cf). Taken:
  - the decided-pull refusal (moved to the header-only `PullDecision.h`, so
    `PullStore.h`, which `MainWindow.h` includes, is unchanged);
  - the dirty-worktree notices;
  - the cloud-prompt form refusals.

  Superseded by 91ed2be814's question rule: the push-blocked and agent-access
  dialog properties. Superseded by c6719b451b's autostash: the
  update-from-main dirty notice. Dropped: marking the mark-safe notices local,
  because they came from the scanner bug above.
- Branch `572-…` (e64ffcf177, as merged up to 66447fa194): the per-repository
  git index queue and safe `index.lock` recovery, taken whole
  ([git-locking.md](git-locking.md)). It does not touch mark-safe.
- The uncommitted `fix/desktop-error-reporting` worktree (2026-09-27). Taken:
  - the read-only command filter for access failures, with its tests;
  - the terminal-handoff refusals;
  - the transcript directory handling;
  - the third teammate refusal.

  Superseded by this pass: its fetch/pull, website-ping, task-delete, pull
  and multi-line mark-safe changes. It marked every fetch or pull failure
  local, and treated rate limits as final. Dropped:
  - classifying `action` pings as workflow outcomes that never report. A
    failed run is a real failure, and the Android/iOS groups ended by
    retiring those workflows.
  - making "Release … served mirror could not be updated" local, because it
    is a real failure;
  - making a failed Vultr mirror deployment local, for the same reason;
  - the `warnLocalRefusal` helper in `MainWindowInternal.h`: a local helper
    in `MainWindowTeammates.cpp` does the same without recompiling every
    MainWindow unit.

Tests:

- `forkmesh-tests`: the wrapped assignment, `gitrefusal` and a real
  remote-only branch, decided and dirty-checkout pull refusals, the held-back
  manifest, owner-only `meta.json` with no temp left behind, and peer keys not
  written on the spot.
- `forkmesh-qt-runtime-diagnostics-tests`: the object walk throttle.
- `forkmesh-window-tests --error-log-triage-only`: a remote-only branch
  merges; an already-merged one, a missing one, a dirty worktree, a conflict
  and a missing transcript file are Local only; a real failure still reads
  Offline.
- `forkmesh-window-tests --error-alert-only`: the cloud-prompt form refusals
  ("Pick a repository…", a runtime no agent node runs) are Local only on a
  signed-out window, where a reportable failure would read Offline.
- `--stats-only`: a rail click does not write settings.
- `forkmesh-window-tests` (agent transcript links): a `forkmesh-file:` link to
  a folder and a `forkmesh-branch:` link naming a folder both open it in
  Files. The worktree's own copy of this check was recovered on
  `recovered/fix-desktop-error-reporting` and merged up to main on 2026-10-06.
- `forkmesh-agent-access-tests`: text readers and quoted file lines raise no
  access prompt; real `curl`/`node` resolve failures still do.
- `forkmesh-git-index-queue-tests`: branch `572-…`'s suite.

## UI freezes on a healthy machine (2026-09-27 log triage)

The relay's error log carried "Desktop UI froze over 5 s" groups whose
stall records showed no memory or IO pressure. The fixes, by path:

- **Commits tab** (`loadCommits`, `unpushedCommitHashes`, the four
  `commitsListIsCurrent()` callers). `git log --topo-order`, the two
  `rev-list` reads for the "not yet synced" markers and the upstream probe ran
  through `runGitCapture` on the GUI thread, which waits for the worker in a
  nested event loop. `startCommitsLoad()` now reads all of it in one
  `runOffThread` job and fills the table in the apply step. Two generations
  keep it honest: `m_commitsRequestGen` drops a read that a newer request or
  another repository superseded, and `m_commitsLoadGen` (bumped only when the
  table is really refilled) still guards `fillCommitStats`, so a "nothing
  moved" check no longer cancels the per-row stats. `refreshCommitsIfStale()`
  replaces the old "is the list current, else reload" pattern: the worker
  compares the tips and the apply step only refreshes source control when
  nothing moved. The rows stay on screen until the new ones arrive. The list
  still switches to the Changes page when the load is requested, not when it
  lands, so a later `showCommit()` is not overridden. "Load more" restores its
  scroll position from the load's completion callback.
- **Code overview** (`loadRepoOverview`). The HEAD `rev-parse`, `log -1`, both
  `ls-tree` passes, the empty-tree `diff --numstat` and the README `show` run on
  workers in two stages: the HEAD read first, so the "nothing changed" skip and
  the loading placeholders happen in the same order as before, then the
  listing. `m_overviewRequestGen` replaces the old re-entrancy flag: the newest
  request wins instead of the first. `testOpenRepoDirectory` waits until the
  request settles (`m_overviewSettledGen`).
- **Action approvals** (`ActionStore::approve`). Every approval rewrote the
  whole settings file (see Traps) on the GUI thread, once per workflow in
  `applyWorkflowScan`. Approvals now land in an in-process overlay that
  `isApproved()` and `lastApprovedContent()` read first, and a worker writes
  the settings file; the overlay entry is dropped once the file holds the
  same record. Off the GUI thread (tests, workers) the write stays
  synchronous.
- **Team room members** (`refreshChatMembers`). For `#team` it compared every
  history message with every roster entry (`chatUserDisplayName()` per pair),
  about 8 million calls for 20,000 messages and 400 roster rows. It now builds
  id and display-name lookups once, keeping the first roster match, and
  checks each distinct sender once.
- **Rebuild source check** (`sourceChangedSince`). The walk used
  `QDirIterator` and only skipped `build`/`tests`/hidden entries after
  visiting them, so it walked every file in the build trees. It now prunes
  those directories, and the check (plus the two `rev-parse --short HEAD`
  reads) runs on a worker between build passes.
- **Diff views during a window resize** (`scheduleDiffStreamBatch`,
  `DiffEndCapResizeFilter`). A width change makes Qt lay the whole diff
  document out again. The end-of-diff bar was resized on the next event-loop
  turn after every resize event, and its cursor moves (`setPosition`,
  `movePosition`) forced that layout synchronously. With a frameless drag that
  was one full layout per resize event, and one stall lasted 20 s. For a
  document over 32K characters, the end-cap resize, the window fill and the
  next streamed batch now wait until no resize has arrived for 150 ms. The
  end-cap edit runs in one edit block without `movePosition`. The streamed
  page's start position is read from `characterCount()` instead of a cursor
  moved to the end, which forced the same layout.
- **Cached branch diff repaint** (`renderBranchDiffPatchNow`). It rendered the
  cached patch (up to `kBranchDiffCacheBytes`, 8 MB) on the GUI thread to keep
  adhoc #227's instant repaint promise. That is where 30 s samples in
  `renderSplitDiffHtml`, `diffFileHeaderHtml` and `diffBinaryPatchImage` came
  from. A cached patch over 256K characters now goes through the worker
  render (`renderBranchDiffPatch`).
- **Branch range file list** (`showRangeFilesInSourceControl`). Each changed
  file gets a row of nine widgets. The first 300 rows are built at once, and
  the rest in batches of 300 on later event-loop turns. A batch checks the
  group row's key and child count before it adds rows. A previously selected
  file past the first batch is selected when its batch lands, unless
  something else has been selected by then.
- **Artifacts tab** (`loadArtifactsPanel`). The release-store scan, the
  `ls-tree <branch>:.forkmesh/releases` and one `show .../release.json` per
  channel now run on a worker.
- **Mirror nodes panel.** `branchHeadsFromGit` (`for-each-ref refs/heads`) is
  cached on the refs-directory signature. The sync-backlog `rev-list --count
  <tips> --not <heads>` is memoised on its sorted object names, because the
  answer for fixed object names never changes.
- **Branch list** (`repoBranches`). Its 5-second cache is also keyed on the
  refs signature, so a quiet repository no longer spawns
  `git branch --sort=-committerdate` every 5 s from the ~25 GUI callers.

Not changed:

- `recoverMissingBrowsedRef()` still probes refs synchronously. It runs only
  when the browsed branch has been deleted.
- `noteDiffFileBlocks` still splits the rendered HTML on the GUI thread.
- The other GUI-thread `renderDiffHtml` callers (commit detail, release
  detail, pull diff, source control) still render there.

## Traps

- Crashes reach the repair agent only through the next launch's log scan; a
  node that never restarts never dispatches them.
- Error groups on the relay group by exact message text; a message with a
  variable in it makes one group per value.
- Work started off the GUI thread must not touch widgets after the window is
  torn down; `forkmesh-off-thread-teardown-window-tests` guards this.
- `glib` `SIGTRAP` crashes have been caused by file-descriptor exhaustion, not
  by glib.
- Every `QSettings().setValue()` rewrites and fsyncs the whole settings file
  (`QSettings` commits through `QSaveFile`). It was 710 KB on the main node on
  2026-09-27, 433 KB of it the diff Viewed marks, and 14 of that day's 25
  fsync stalls were a `QSettings` destructor. A settings write on the GUI
  thread shows up as a stall in whatever called it (`applyClaudeUsage*`,
  `startSession`, `rememberUserAccount`, `persistEditsToActiveServer`), and
  more so under IO pressure. Keep large or frequently rewritten state out of
  `QSettings`; the Viewed marks moved out for this reason (below).
- `QSaveFile::commit()` always fsyncs. On the GUI thread under IO pressure
  that is a freeze of seconds: 27.5 s in `AgentToolEfficiencyLedger::save` on
  2026-09-27 at 93% io-full. Write periodic state through
  `forkmesh::diskwrite::writeLater` (below) instead.

## Related

- [www/docs/qt-client.md](../../www/docs/qt-client.md#logs-and-diagnostics)
- [build-and-test.md](build-and-test.md)

## Diff anchors and `QUrl::FullyDecoded`

`onPullDiffAnchorClicked` and `onBranchDiffAnchorClicked` read the clicked
href with `url.toString(QUrl::FullyDecoded)`. Qt does not allow that mode
for a whole URL: it logs "QUrl::FullyDecoded is not permitted when
reconstructing the full URL" and returns an empty string. So every
`thread:` link in the pull and branch diff views did nothing (8 warnings in
the 2026-09-26 log). Both handlers now use
`QUrl::fromPercentEncoding(url.toEncoded())`.

## Transcript image previews and pixmap teardown

An attached image in an agent transcript is drawn by `ThumbImage` in
`ClaudeTranscriptView.cpp`. It used to be a `QLabel` holding a `QPixmap`
made from the worker-decoded `QImage`. Moving the pixmap out of the
process-wide `QPixmapCache` was not enough: the pixmap-bearing label stayed
tied to Qt's GUI pixmap state, and `QApplication` teardown could free that
state a second time (crash fix, 2026-09-09). `ThumbImage` is now a plain
`QWidget` that keeps the decoded `QImage` per size cap, paints it itself
inside the rounded border, and reports its size through `sizeHint()`, so no
`QPixmap` is created for a preview at all. Until the first decode lands the
widget reserves a minimum size from the image header; `showScaled()`
clears that minimum. The window test waits on the cleared minimum and
checks the preview is not a `QLabel` and that nothing was left in
`QPixmapCache`.

## Pull list after a ledger change from elsewhere

The pull list reloaded after a served-ledger sync only when that sync itself
adopted, closed or rewarded a pull. A merge recorded from another machine or
a script moved `forkmesh/pulls` on the mirrors, the sync fetched it, and the
list kept showing the pulls as open; merging one then failed with "already
merged" (#168-#170, 2026-09-26). The sync now reloads the list whenever the
fetch succeeded, and a failed merge reloads it before showing the error.

Reloading was not enough (2026-09-27, #191-#194 and #304-#308). Each pull also
has its own metadata branch, `refs/heads/forkmesh/pull/<n>`, and
`PullStore::loadAll()` reads it in preference to the shared `forkmesh/pulls`
record whenever the two trees differ (it can carry comments the shared branch
lacks). A merge recorded only on `forkmesh/pulls` (by a script, the server, or
another machine) left the per-pull branch at its old `status: open`, so the
pull stayed in the list after every reload. Now, when the shared record says
`merged` and the per-pull snapshot does not, `loadAll()` keeps the snapshot but
takes `status`, `mergeBase` and `mergeHead` from the shared record
(`adoptLedgerMerge`). Only `merged` is adopted: nothing reopens a merged pull,
while `closed` can be reopened, so a stale `closed` on the shared branch must not
override a newer reopen on the per-pull branch. Test: the "merge recorded only on
forkmesh/pulls" check in `desktop/tests/test_crypto.cpp`. A script that marks a
pull merged should still update `refs/heads/forkmesh/pull/<n>` too, so older
desktops drop it.

Since 2026-10-07 a merged pull has no per-pull branch at all. The owner wanted
the merge on `main` to be the only history and wanted merging or deleting a
pull to delete its branch. `PullStore::materializePullMetadataRef()` checks
the record it is writing. When the record says `merged`, it writes only
`refs/pr/<n>/metadata` (parent: the old branch tip, or the previous
`refs/pr/<n>/metadata` once the branch is gone). In one `update-ref --stdin`
transaction it also stores the old tip as
`refs/forkmesh/trash/deleted/forkmesh/pull/<n>` and deletes the branch. That
covers `mergePull()`, the merged-pull reconcile pass and any later comment or
reward update on a merged pull. The tombstone is the one
`UpstreamCheckoutSync` already honours, so a mesh sync neither copies the
branch back nor keeps it on the mirrors. `removeMaterializedPullRefs()`
(delete and close) writes the same tombstone before it drops the branch,
except for a delete that rewrites history (`deletePull`/`deletePulls` with
`rewriteHistory`): that delete exists to scrub the record, and a tombstone
would keep the scrubbed content reachable on this machine, so it removes the
branch and any older tombstone for that number and keeps nothing. A mirror
copy of such a branch can still come back, but `loadAll()` already skips a
leftover older than the ledger commit that deleted the record
(`dropDeletedPullMetadataRefs`).
`loadAll()` lists `refs/pr/<n>/metadata` alongside the branches and uses it
for a pull that has no branch. A branch copied back from a mirror whose own
record and the shared record both say `merged` is retired the same way, once
per tip per run. The one-time migration no longer creates a branch for a
merged pull. So the list still shows merged pulls, and `nextNumber()` still
reserves their numbers. The cores do the same
(`docs/server/intake-and-attestation.md`, "Merged pulls keep no branch").
Test: `desktop/tests/test_pull_store_retire.cpp`
(`forkmesh-pull-store-retire-tests`).

- 2026-09-29: The agent background-work window test burned CPU with
  `(while : ; do : ; done) & p=$!; sleep 2; kill $p`, then killed the outer
  shell after about 0.4 s (`tree.kill()`). That left the loop orphaned at
  100% CPU on every run. Agents ran the window tests all day, and 17 orphans
  (up to 8 h old) held the machine at load 30+, starving builds and the World
  performance measurements. The test now uses
  `timeout 2 sh -c 'while :; do :; done' & wait`, which ends by itself
  whatever happens to its parent.
