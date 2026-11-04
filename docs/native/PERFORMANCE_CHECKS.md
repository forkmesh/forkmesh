# Performance checks

Part of the phone apps ([README.md](README.md)). "Send diagnostics" on the
phone starts an agent with a scrubbed diagnostics bundle (iOS:
`DiagnosticsBundle` in `FMTools`, report from `PerformanceReport`; Android:
`DiagnosticsBundle` in `core/.../tools/PerformanceDiagnostics.kt`). The prompt
sends the agent here first and asks it to add its check below before
committing, so each check starts from what the previous ones learned. Keep this
page short: correct a wrong reading in place rather than adding a
contradicting note. The DJ studio has its own button, bundle and page:
[video-mix-dj-studio-performance.md](../marketplace/video-mix-dj-studio-performance.md).
The standing rules are in [iOS performance rules](#ios-performance-rules).

## Reading the bundle

iOS report, section by section (`PerformanceReport.text`; at most 4000
characters; the lowest-ranked sections are trimmed first and say so). A
missing section is not always trimming: an older build may predate it (dates
below say when each field arrived).

- **Header.** `build ... commit <sha>`: check out that commit to read the code
  that ran. `tab`, `screen`, `recent screens` are the state when the report was
  built, not when the stalls happened; `screen` is the page covering the tab
  (`Home`, `Report a bug`). Before 2026-10-29 it gave only the tab, so a report
  from Home on an iPhone read `tab: agents, screen: tab root`.
  - `stalls since launch`: frame, event-loop and named sync stalls of 200 ms
    or more.
  - `paused while visible N (longest X ms)` (2026-10-18): times the whole
    process stopped while the stall probes ran. These are not stalls (see
    `FRAME`).
  - `agents` (lines listed, running, list view, agent dots, aquarium) and
    `watch` (paired, app installed, reachable): the watch bridge parses agent
    work only while a watch with the app is paired.
  - `chat history` (2026-10-29): size of the stored chat history, which the
    app reads at every launch.
- **`FPS`** comes from a 60 Hz probe (`TelemetryFrameProbe`), so about 60 is
  normal even on a 120 Hz phone.
- **`FRAME`** is the longest gap between two probe callbacks in each second,
  **`LOOP`** the worst heartbeat lag in each second (`PerformanceMonitor`);
  `avg` averages those per-second worsts.
  - An average `LOOP` of 10-20 ms means the main thread is rarely idle; a high
    `max` alone is one stall.
  - An `uptime` of a few seconds makes every average a launch figure.
  - Take the stalls out before reading an average: (sum of the per-second
    worsts, `avg` x seconds sampled, minus the stalls' total) / the other
    seconds. What is left above about 20 ms is work on every second, not this
    phone's normal level: on 2026-11-02 a `LOOP` of 43 ms over 57 s left about
    30 ms a second after 779 ms of stalls (the token tanks, below). Earlier
    checks (from 2026-10-18) read 40-50 ms as normal, but from 2026-10-15
    until ef737b7 the header avatar also redrew with the motion feed 30 times
    a second, so the main thread was not idle then either.
  - An off-main watch timer (`PerformanceMonitorService`, same rate as the
    heartbeat) sees a process suspension too, and the missed time is taken out
    of heartbeat lags, frame gaps and the telemetry `FRAME`. In a build without
    `paused while visible`, a `max` of tens of seconds with an `uptime` not
    much longer is a suspension, not a frozen main thread: the per-second rows
    go missing, no tracked operation lasts as long, and the stall's in-flight
    requests are the resume burst.
- **`RELAY`** is relay frames received (always 0 before 2026-10-17). Its `max`
  is the busiest second: dozens in one second is the relay (re)joining a busy
  room (`world-general` answers a hello from every peer), and every frame is
  handled on the main actor.
- **`Stalls by name`**: `eventLoop ui-isolate blocked` is the heartbeat
  catching a blocked main thread, `frame` a late display callback,
  `sync <name>` a `timeSync` section naming its culprit; `last` is the newest's
  clock time. A `frame` event's `vsyncMs=100` is the 10 Hz stall probe's
  period, `buildMs` the time past it. A frame stall and an event-loop stall a
  few milliseconds apart are one block seen twice.
- **`Main-actor work during stalls`**: named sections
  (`PerformanceMonitorService.section`) that ran inside each stall, summed per
  name. Main-thread sections of 1 ms or more are kept (the last 256).
  - Sections nest (`relay.event` holds `relay.control`), so an outer name
    includes its inner ones.
  - A stall with no sections ran unnamed work: SwiftUI's own updates, unwrapped
    code, or many short main-actor jobs queued back to back (65 relay frames of
    a few ms read as one stall; check `RELAY` max). Each chat relay frame is a
    `relay.frame` section.
  - A `(no named section)` row (since 2026-11-01) sums each stall's
    `unnamedMs`, the part of the block no section covered. When it is most of
    the stall, read `Main thread during stalls`.
- **`Main thread during stalls (leaf images < app frames)`** (since
  2026-11-01): up to four samples of the main thread's stack per event-loop
  stall, taken from the watch queue while the heartbeat was late
  (`MainThreadSampler`), counted by signature: the leaf images (up to three,
  e.g. `AttributeGraph < SwiftUICore`), then the two app frames nearest the
  leaf (`ForkMesh +0x<offset>`, named with `atos`).
  - System images with `no app frame`, or only the app's entry point after
    them, mean SwiftUI, UIKit or Core Animation working on their own: layout,
    a first build, a long list diff.
  - `libsystem_kernel` at the leaf means the thread waited: a lock, a file
    read, a Keychain or XPC round trip.
  - A sample is taken 100 ms into a block and then once per heartbeat
    interval, so a 350 ms block gives about three. Before 2026-11-02 trimming
    cut every section to three lines before cutting any further, so this one
    lost all but two signatures (`... 3 lines trimmed`); it now keeps its count
    line and six rows until the last pass (`Part.keep`).
  - Launch: `startup.services` holds everything `AppServices.start` does on the
    main thread after the identity load: `startup.init` (with
    `startup.auth`), `startup.projects`, `startup.features` (one
    `startup.feature.<area>` each), `startup.wire` and one `startup.<area>` per
    installer. Over 200 ms it is its own `sync startup.services` stall.
    `shell.start` is the shell's first-appearance work, `chat.history.restore`
    the stored history going back into the chat services, `shell.tokenTanks`
    one frame of the token tanks under the island.
  - `surfaces.publish` is the widget, Live Activity and Shortcuts snapshot
    after agent row changes. `watchSections` hands every section of 1 ms or
    more to one watcher; the DJ studio uses it.
- **`Stalls by time since shown`** (2026-10-19): each stall's `shown`
  (`launch` or `resume`) and `shownMs`, grouped into the first 10 s and
  `later`. A stall in the first seconds of a resume belongs to the resume
  burst whatever screen it lands on.
- **`Stalls by screen`**: stalls grouped by the tab and stack showing when
  each was recorded (`AppRouter.screenTitle`, also in `stalls.log`). `not
  recorded` is an older build or a stall caught off the main thread. The
  screen reads `Launch` until the shell starts and `Home` while Home covers
  the tab; before 2026-10-29 both read `Agents`, the router's default tab, so
  an `Agents` launch stall in an older bundle was not the Agents tab. A screen
  that was left can still own the work: a load it started keeps running in
  `ApiService`'s caches.
- **`Slow operations by name`**: `track`ed awaits over 500 ms, over every
  event kept (`events kept N/100`); `HTTP endpoints` covers only the last
  5 minutes, so compare the same window.
  - Near 8 s (`/api/repo/` reads) or 12 s (other GETs) is `getJson`'s request
    timeout, not the main thread; one retried after a 5xx can reach 24 s.
  - Time hidden or paused is taken out and shown as `awayMs` (2026-10-19);
    in older bundles, operations of minutes were open while the app was in the
    background (look for empty per-minute rows).
  - Main-actor hops add at most about the `LOOP` max each: that explains a
    few hundred ms over the endpoint, not seconds.
- **`In flight or finished during stalls`**: correlation, not cause. A request
  in flight in many stalls usually marks the burst they happen in (resume, a
  ring, a screen's first load); look at what runs on the main actor when it
  lands. Weigh the count against how long it was open: a request open most of
  the time is in every stall and marks nothing.
- **`stalls.log tail`**: release stacks are unsymbolicated;
  `libswift_Concurrency` under `ForkMesh` frames is a main-actor task job.
  Frame stalls carry no stack (the probe runs after the block); an event-loop
  stall's stack is samples of the main thread taken while it was blocked
  (`main thread at <lag>ms:`, four leaf frames and up to eight app frames);
  the other stacks are where slow operations and `sync` sections started. Each frame
  gives `+0x<offset>` from its image's load address: `atos -o
  ForkMesh.app.dSYM/Contents/Resources/DWARF/ForkMesh -l 0x100000000
  <0x100000000 + offset>` with the build's dSYM names it. Without a Mac:
  ad hoc builds since 2026-11-02 keep `ForkMesh-native-<version>-<build>-symbols.txt.gz`
  (`nm -n` of the dSYM) next to the IPA (Codemagic artifacts; the Mac
  pipeline's `data/actions/artifacts/<repo>/<run>/mac-adhoc/<commit>/ipa/`),
  and `native/ios/tools/symbolize_offsets.py <symbols> < report.txt` names
  every `ForkMesh +0x...` frame, demangled with the local Swift toolchain.
- **`App log`** and **`Busiest log lines`** are optional. `Connected to ...
  room` counts relay (re)connects; each follows a resume or a dropped socket.

## iOS performance rules

The shell's account avatar does not follow phone motion; the Genie view does.
Agent Watch feel defaults on unless the user turns it off; Reduce Motion stops
its movement but does not change that saved preference.

A tab hidden under Home or behind another tab starts only the work the shell
needs from it (Agents: the list, for the counts); streams, follows and
watchers wait until it is first shown (`AgentsTabLiveGate`). SwiftUI evaluates
every `@State` default each time the parent builds the view and keeps only
the first, so a default does no reads or parsing in `init`
(`AgentReadTracker` reads its store on first use).

- No Keychain write per received frame on the main actor (`SecItemDelete` plus
  `SecItemAdd` is a round trip to `securityd`). The chat replay book is written
  at most every 250 ms off the main thread (`ChatKeyring.persistReplay`).
- Parse API replies of 16 KB or more off the main actor
  (`decodeResponseAsync`, `ApiParse.parseAsync`, `ApiParse.modelAsync`),
  including documents read in batches (`publishedIssues`) and replies that
  bypass `ApiService` (`ApiPackagesTransport.parse`; `PackagesClient.crm`
  builds `CrmOrganizationData` in a detached task).
- Write `@Observable` properties only when the value changes; every write
  notifies, equal or not, including refreshes on every return to the app
  (`AccountPackagesStore`, `HubPackageState`, `OrganizationPackagesLedger`,
  `ShellNavigatorCounts`).
- A scene-phase refresh runs on every inactive-to-active change (Control
  Center, Notification Center, a system alert); gate it on age
  (`AccountPackagesStore.refreshIfStale`) or a real return (`AppReturnGate`).
- Read and decode stored files off the main actor at launch and restore a list
  in one write (`ChatHub.attachHistory(inBackground:)`, batched `restore`s):
  everything in `AppServices.start` after the identity load runs on the main
  thread in one block. Name every launch step (`startup.<step>`).
- Parse message text once per text, not per redraw (`ChatTextCache`).
- Compute derived lists once per body and pass them down.
- Do not let a ring cancel a load in flight; mark it and run one more load
  afterwards, or none when the load's reads had not gone out yet. Join
  in-flight loads (`loadIfStale`, shared reads).
- Fan out independent reads (`PackagesClient.organizationPackages([String])`)
  instead of asking each organization in turn.
- Read only what a screen needs (`publishedPullHeads`, front matter only) and
  keep ledgers between rings unless something relevant changed.
- Build the selected tab first; other tabs later or on first visit. Hidden
  tabs set `shellTabHidden` and pause frame-rate animations.
- Fan out live values to small observed objects (one row redraws, not the
  list); throttle shell counts (`ThrottledSnapshot`, at most 4 reads a
  second); bump roster revisions only on real changes.
- Decode images once through `FmImageCache` (48 MB cap), never in `body`.
- Build Home and bottom-bar groups once per redraw and hand the installed set
  down (`HubGroups.current(guest:installed:)`); `installedGatedPackages` reads
  project settings once and indexes account state and catalog by id, so a
  redraw is not quadratic in the package count.
- Home tiles take their lookups from the layout built once per body;
  `ShellHomeIcon` keeps only the shown fields (`ShellHomeIconFace`), not the
  action closure, so SwiftUI skips unchanged icons.
- The performance monitor ignores frame gaps that reach back into the
  background.
- Walking the view or accessibility tree (the Genie spider, the aquarium,
  `GenieScreenScan`) runs on the main thread and builds accessibility
  elements; scan on page and tab changes, then rarely, and cache each walk.
- Parse agent transcripts off the main actor (`beatsFromTranscript` costs about
  58 ms per 64 KB, `AgentTranscriptFormat.blocks` about 16 ms).
  `TranscriptBeatCache` parses in a detached task once per transcript length;
  live feeds re-read only the transcript that moved
  (`ControlWatchFeeds.revision`, `AgentEagleFeed`).
- A frame-rate simulation (the token tanks) draws at most 30 times a second,
  keeps its sensor on only while something moves, and keeps its hot loops in
  a struct: element access to a class's stored arrays pays a dynamic
  exclusivity check each time (`AgentTokenFluid`, about 4x).
- Wrap main-actor work that grows with the account's data in
  `PerformanceMonitorService.section(name)`. A section over 8 ms is named in
  the `sections` detail of any stall it falls inside; one over 200 ms is its
  own `sync` stall.

## Checks so far

Newest first. Each entry: date and branch; what the bundle showed; the cause;
the fix; what was ruled out; what the bundle lacked.

- **2026-11-02, `106-ios-home-freeze-diagnosis`** (1.5.0 (22), 266bf12,
  iPhone16,2, `uptime 57s`; 10 agent lines, 8 running, Eagle, no watch app).
  Showed: event-loop stalls of 508 and 271 ms and a 322 ms frame, all
  `launch, first 10s` on `Home` at 09:49:24, 756 ms of them outside any
  section (`shell.start` 23 ms); two of five main-thread samples survived
  trimming: SwiftUI's graph under the app's entry and an `@Observable` write
  (`libswiftObservation`) from app code. Also `LOOP avg 43ms`, `FRAME avg
  45ms`, `FPS 54.5` over the whole minute: about 30 ms of lag every second
  after the launch stalls. Cause of the constant load (measured): this build
  was the first with the liquid token tanks under the island (053aa2a). Two
  60 x 12 particle fluids ran 60 steps a second on the main thread inside a
  `TimelineView(.animation)` Canvas redrawn at up to 120 Hz, rebuilding up to
  1,440 rects a frame, with an accelerometer feed on the main queue at 30 Hz
  even while empty. With 8 agents running they were rarely empty. A full
  pair cost 127 ms of a desktop core per simulated second, about 70 % of it
  Swift's dynamic exclusivity checks on the class's arrays. Fix: the fluid
  is a struct (`AgentTokenFluid`, bit-identical results, 33 ms); the tanks
  draw at 30 fps; the accelerometer runs only while a tank holds or owes
  grains (`AgentTokenTanks.wantsMotion`); each frame is a `shell.tokenTanks`
  section. Android gets the same pacing and sensor rule. Not fixed, not
  proven: the launch stalls themselves. The tanks were empty and paused
  then, so they did not cause them; what is left is SwiftUI's first build of
  the shell with the Agents tab under Home and an observed write during it,
  as in the previous check, which the trimmed, unsymbolicated samples cannot
  name. The header avatar's 30 Hz motion redraw (since 06715ef) was removed
  on main after this build (ef737b7). Tests:
  `testThirtyFramesASecondRunTheSameSimulationAsTheDisplayRate` (iOS and
  Android), `testThePairAsksForTheAccelerometerOnlyWhileGrainsAreInATank`,
  `testTrimmingKeepsEveryMainThreadSampleRowUntilTheLastPass`,
  `tools/test_symbolize_offsets.py`. Ruled out: the repository tree 403s and
  slow reads (after the stalls), chat history (262 KB), the watch bridge, the
  1.4.1 MetricKit CPU exceptions. Bundle gaps closed: the samples section is
  no longer trimmed to two rows; ad hoc builds keep a symbol list and
  `symbolize_offsets.py` names offsets on Linux (the previous check's "still
  missing"). Still missing: build 22's own symbols, so this bundle's offsets
  stay unnamed; the next launch bundle from a new build can name them.

- **2026-11-01, `46-app-detected-performance-issue`** (1.5.0 (327), 421a5f2,
  iPhone16,2, `uptime 49s`; 22 agent lines, 15 running, Eagle view, no watch
  app). Showed: event-loop stalls of 352 and 260 ms and a 248 ms frame at
  launch on `Home`; the only named work was `startup.services` 38 ms (holding
  `startup.features` 29 ms) and `shell.start` 24 ms, so most of the 612 ms ran
  outside any section. Cause, not proven: an iPhone opens on Home but builds
  the selected tab (Agents) under it, and `AgentsScreen.onAppear` ran in full
  while hidden. Besides the list Home's counts need, it started the Eagle feed
  (every running desktop row's transcript from `since: 0`, each chunk handled
  on the main actor by `TranscriptFollower.apply`) and the agent dot watcher
  (`AgentDotsWatch`, up to 12 relay watch targets). `HomeShell` also re-created
  `AgentsScreen()` on each body pass, so the `AgentReadTracker` `@State`
  default re-parsed up to 400 read marks each time. Fix: the hidden Agents tab
  starts the Eagle follows and the dot watcher only when first shown
  (`AgentsTabLiveGate`, read in `AgentsScreen.followDialogs` and `syncDots`);
  `AgentReadTracker` reads its store on first use. Tests:
  `testATrackerReadsItsStoredMarksOnlyWhenFirstUsed`,
  `testTheHiddenAgentsTabStartsItsLiveWorkOnlyOnceShownAndKeepsIt`,
  `testAnEventLoopStallCarriesTheMainThreadSamplesTakenWhileItWasBlocked`,
  `testASignatureNamesTheLeafImagesThenTheNearestAppFrames`,
  `testStallsSayHowMuchRanOutsideNamedSectionsAndWhereTheMainThreadWas`, and
  on Darwin `testTheMainThreadCanBeSampledFromAnotherThread`. Ruled out: the
  launch block, chat history (137 KB), slow repository reads and
  `agents/list` 403s (finished after the stalls), the watch bridge, the 1.4.1
  MetricKit CPU exceptions. Not measured: shell redraws in the launch burst,
  the Eagle and dot streams' cost at launch, SwiftUI's first build. Bundle gaps
  closed: main-thread samples and `unnamedMs` (above). Still missing: samples
  are unsymbolicated, so app frames need `atos` and the build's dSYM.
- **2026-10-29, `2672-agents-tab-freeze-fix`** (1.5.0 (324), cb5b44d).
  Showed: two event-loop stalls of about 344 ms and a 372 ms frame at
  `uptime 13s`, all `launch, first 10s`, filed under `Agents`; only
  `startup.auth` named. Cause: the synchronous rest of `AppServices.start`,
  the largest measured part being `installChat` reading and decoding the whole
  chat history (`relay-<scope>.json`, up to 200 messages per conversation) and
  restoring it message by message: O(n²) array copies, a save rescheduled per
  direct message, `isDirect` validating hex through `Character` ranges, a deep
  `snapshot != loaded` compare. Measured about 170 ms for 2.8 MB and 420 ms for
  6.2 MB of history. `Agents` was the router's default title; the launch
  screen was up. Fix: `ChatHub.loadHistoryInBackground` (read off the main
  thread; nothing saved until loaded; arrivals merged); restores group by
  conversation and merge once (`RelayService.merging`); `validChannelId`
  checks UTF-8 bytes; the purge check compares counts. Now 28-70 ms on the
  main thread. Android had the same copies (`RelayRoom`,
  `DirectMessageService`, `OfficeChannelService`, now `mergeSorted`); its
  history read in `ChatDepth.install` is still synchronous. Tests include
  `testRestoringDirectHistoryReportsOneChangeAndKeepsTheNewestInOrder` and
  Android `testRestoresMergeOnceInOrderAndKeepTheNewest`. Ruled out: the
  Agents tab, network operations, the watch bridge, Keychain reads. Not
  measured, still on the main thread at launch: WebRTC setup
  (`installCalls`), the local-model folder scan (`LocalAIFeature.register`),
  MetricKit replay, TipKit. Gaps closed: every launch step is a section; the
  screen reads `Launch`/`Home`; the header gives `chat history`. Still
  missing: the main thread's stack during an event-loop stall.
- **2026-10-21, `2424-improve-mobile-chat-room`** (no bundle; "very laggy"
  around chat). Found: direct messages closed and reopened every watched room
  (up to 16) on every `.resumed`, including closing Control Center; each
  `.resumed` refreshed rooms, direct messages, read marks, members and
  retention, writing state even when unchanged; the conversation built its
  rows two or three times per redraw; typing redrew the transcript every
  2.5 s; the online light ran a `repeatForever` shadow; bubbles re-parsed text
  on every redraw; the channel list rebuilt `directRows` about seven times.
  Fix: `AppReturnGate` (refresh only after `.paused`, `.hidden` or
  `.detached`); rooms reopen only when `foreground` changes;
  `ChatHub.applyReadMarks` joins a fetch in flight and skips one within 5 s;
  only changed values are written; items built once per body;
  `MessagesTypingRow`; the light pulses from a `TimelineView`; `ChatTextCache`
  (1024 entries); `directRows` computed once. Not fixed: replayed frames are
  opened in full before dedupe; the Message Info swipe redraws per drag frame;
  `trustChanged` bumps on every connect; relay channel history has no per-room
  cap.
- **2026-10-19, `2321-slow-ios-app-performance`** (1.4.1 (291), 63b65a9;
  "laggy when we switch to it"). Showed: 11 unnamed stalls in 24 minutes, the
  return-burst requests in flight, `RELAY` up to 65 frames in one second.
  Cause: every fresh sealed relay frame made `ChatKeyring.recordSeen`
  re-encode the room's replay book (up to 256 senders by 1024 counters) and
  rewrite it in the Keychain on the main actor; 65 frames cost about 125 ms
  before Keychain time. The shell also archived navigation into
  `@SceneStorage` on every inactive step, and `savedAt` always changed, so the
  root rebuilt on return. Fix: the book is written at most once per 250 ms per
  room on a utility queue and at once on leaving the foreground
  (`ChatKeyring.persistPendingReplay`); navigation is archived only on
  `.background`. Ruled out: the 534 s repository reads (open in the
  background), the relay's resume, the direct-message replay. `nextCounter`
  still writes the Keychain on every send (it must be durable first). Gaps
  closed: 1 ms sections, `relay.frame`, `shown`/`shownMs`, `awayMs`, `atos`
  offsets.
- **2026-10-18, `2284-slow-ios-app-performance`** (1.4.1 (284), d11bec0).
  Showed: "froze for 86.0 s" at `uptime 1m50s`, with no operation spanning
  it. Cause: the process was stopped while both stall probes were armed. The
  probes park only on a background scene phase, and the monitor started as
  `.resumed` and heard lifecycle changes only after the identity load, so a
  background launch (Bluetooth or audio), a change during that load, or a
  suspension while `.inactive` left them armed. Fix: an off-main watch timer
  detects pauses, subtracts them, logs `[pause] process paused N ms while
  visible` and counts them in the header; `AppLifecycle` starts from
  `UIApplication.applicationState`. A real block with the process running is
  still a stall, with `pausedMs` when a pause overlapped.
- **2026-10-18, `2228-slow-ios-app-performance`** (1.4.1 (276), b57c0d2).
  Showed: 23 stalls in 5 minutes up to 540 ms, the Agents list read in flight
  in 8. Cause (largest measured, not proven): the watch bridge parsed up to
  four transcripts with `beatsFromTranscript` on the main actor per publish
  (up to about 230 ms), and the Eagle view re-read every followed run on each
  delta. Fix: `TranscriptBeatCache` parses in a detached task once per
  transcript length; the Eagle feed re-reads only the run that moved
  (`ControlWatchFeeds.revision`). Ruled out by measurement: opening the sealed
  list, chat history replay, the attachment scan, agent dots, the Genie scan.
  Not fixed: each ring re-downloads every sealed snapshot with its 16 KB
  transcript tail (twice with a watch); the desktop should seal the transcript
  apart. Gaps closed: stalls carry `sections`; sections wrap relay frames,
  agent list and lines, Eagle, chat receive and watch publish; the header
  gains `agents` and `watch`.
- **2026-10-17, `2214-slow-ios-app-performance`** (1.4.1 (275)). Cause:
  `ApiService.publishedIssues` parsed every issue record's JSON on the main
  actor in one pass (730 issues), after the user had left the overview. Fix:
  records and the overview's release manifests are parsed in a detached task
  (`issueRecord`). Ruled out: 8 s and 12 s operations (request timeouts). Gap
  closed: stalls record their screen.
- **2026-10-17, `2194-slow-ios-app-performance`** (1.4.1 (268)). Cause: every
  return (including closing Control Center) refreshed packages, organization
  package state and Home's counts, each rewriting observed state even when
  unchanged, so Home rebuilt several times in the resume burst. Fix: writes
  only on change; `AccountPackagesStore.refreshIfStale` skips a refresh under a
  minute old. Gap closed: stalls have their own section; `RELAY` is wired.
- **2026-10-16**: Home's count refresh now follows `NavigatorCountPlan` (fresh
  for 60 s, 300 s when warm, three reads at a time) (`2158`); rings during a
  load run one more load afterwards (`2134`); the frame probe no longer
  reports a whole skipped tick as the stall, and Keychain room keys, sleep
  detection and Core Motion history moved off the main thread on resume
  (`2124`).
- **2026-10-14/15** (`1994`, `2071`): the Genie spider's accessibility scan
  backs off; chat history saves moved to a utility queue.
- **2026-10-13** (`1996-ios-performance`): shell redraws per relay frame and
  count change, hidden tabs animating, repeated picture decodes, quadratic
  Home layout ([README.md](README.md), Recent changes).

## Adding a check

1. Check out the bundle's commit and read the code that ran.
2. Find the burst or screen the stalls belong to before reading code paths;
   say what in the bundle points there.
3. Fix the cause with a test in `ForkMeshCore` (or Android `core`) that fails
   without the fix.
4. Add an entry under "Checks so far" in the format above, and correct
   "Reading the bundle" where it misled you.
5. If you had to guess because the bundle lacked something, make the app
   collect it (`PerformanceReport`, `PerformanceMonitorService`) in the same
   change and list it as a gap closed.
