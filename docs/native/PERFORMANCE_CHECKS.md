# Performance checks

"Send diagnostics" on the phone starts an agent with a scrubbed diagnostics
bundle (iOS: `DiagnosticsBundle` in `FMTools`, report from
`PerformanceReport`; Android: `DiagnosticsBundle` in
`core/.../tools/PerformanceDiagnostics.kt`). The prompt sends the agent here
first and asks it to add its check below before committing, so each check
starts from what the previous ones learned. Keep this page short: correct a
wrong reading in place rather than adding a contradicting note. The DJ
studio has its own button, bundle and page:
[../marketplace/video-mix-dj-studio-performance.md](../marketplace/video-mix-dj-studio-performance.md).

## Reading the bundle

iOS report, section by section (`PerformanceReport.text`; at most 4000
characters, lowest-ranked sections are trimmed first and say so):

- Header. `build ... commit <sha>`: check out that commit to read the code
  that ran. `tab`, `screen`, `recent screens` are the state when the report
  was built, not when the stalls happened. `stalls since launch` counts
  frame, event-loop and named sync stalls of 200 ms or more; `paused while
  visible N (longest X ms)` (since 2026-10-18) counts the times the whole
  process stopped while the stall probes were running, which are not stalls
  (see `FRAME`). `agents`
  (lines listed, running, list view, agent dots, aquarium) and `watch`
  (paired, app installed, reachable) since 2026-10-18: the watch bridge
  reads and parses agent work only while a watch with the app is paired.
- `FPS` comes from a 60 Hz probe (`TelemetryFrameProbe`), so about 60 is
  normal even on a 120 Hz phone; it is not the display rate.
- `FRAME` is the longest gap between two probe callbacks in each second,
  `LOOP` the worst heartbeat lag in each second (`PerformanceMonitor`).
  `avg` is the average of those per-second worsts. An average `LOOP` of
  10-20 ms means the main thread is rarely idle; a high `max` alone is one
  stall. Compare the 5 minute average with the hour's: a `FRAME` average of
  40-50 ms over an hour has been this phone's usual level (41 ms in the
  2026-10-18 bundle, 51 ms in the 2026-10-17 one), so only a rise above it
  points at the last few minutes. A `max` of tens of seconds with an
  `uptime` not much longer, in a build without `paused while visible`, is
  the process being suspended while the probes thought the app visible, not
  a frozen main thread: the per-second rows go missing for that stretch, no
  tracked operation lasts as long (one open across a suspension would), and
  the stall's in-flight requests and sections are the resume burst. Since
  2026-10-18 an off-main watch timer (`PerformanceMonitorService`, same
  rate as the heartbeat) sees such a gap too, and the time it missed is
  taken out of heartbeat lags, frame gaps and the telemetry `FRAME`.
- `RELAY` is relay frames received. Before 2026-10-17 the hook was never
  wired and it always read 0.
- `Stalls by name` (before 2026-10-17 merged into `Slow events by name`,
  where operations pushed the stalls out as `... N more`):
  `eventLoop ui-isolate blocked` is the heartbeat catching a blocked main
  thread, `frame` a late display callback, `sync <name>` a `timeSync`
  section that names its culprit. `last` is the clock time of the newest.
  A `frame` event's `vsyncMs=100` is the period of the 10 Hz stall probe,
  not the display, and `buildMs` the time past it; a frame stall and an
  event-loop stall a few milliseconds apart are one block seen twice.
- `Main-actor work during stalls` (since 2026-10-18): the named sections
  (`PerformanceMonitorService.section`, over 8 ms) that ran inside each
  stall, summed per name. Sections nest (`relay.event` holds
  `relay.control`, which holds the delta handling under it), so an outer
  name's time includes its inner ones. A stall with no sections ran
  unnamed work: SwiftUI's own updates, or code not wrapped yet.
- `Stalls by screen` (since 2026-10-17): the same stalls grouped by the
  tab and navigation stack showing when each was recorded
  (`AppRouter.screenTitle`, the `screen` detail, also in `stalls.log`
  lines). `not recorded` is a stall from an older build or one caught off
  the main thread. A missing section is not always trimming: build 276
  (commit b57c0d2) predates it, and a trimmed section says so. A screen
  that was left can still own the work: a load it started keeps running
  in `ApiService`'s caches after it is popped.
- `Slow operations by name`: `track`ed awaits over 500 ms, over every
  event kept (`events kept N/100`), while `HTTP endpoints` covers only
  the last 5 minutes, so compare the same window. An operation that sits
  near 8 s (`/api/repo/` reads) or 12 s (other GETs) is the request
  timeout of `getJson`, not the main thread; one retried after a 5xx can
  reach 24 s. Longer ones (31-32 s in the 2026-10-18 bundle) span the app
  being suspended: look for empty minutes in the per-minute rows. Waiting
  for the main actor between hops adds at most about the `LOOP` max per
  hop (once `LOOP` is not a suspension); it explains an operation a few hundred ms slower than its endpoint,
  not seconds.
- `In flight or finished during stalls`: requests open while a stall was
  recorded. Correlation, not cause: a request that is in flight in many
  stalls usually marks the burst the stalls happen in (resume, a ring, a
  screen's first load); look at what runs on the main actor when that
  burst lands. Weigh the count against how long the request was open: the
  Agents list read was in flight for about 13 s of the 5 minutes yet in 8
  of the stalls, which marks a burst; a request open most of the time is
  in every stall and marks nothing.
- `stalls.log tail`: the last lines of `diagnostics/stalls.log`. Release
  stacks are unsymbolicated addresses; `libswift_Concurrency` under
  `ForkMesh` frames means a main-actor task job, which is most app code.
- `App log` and `Busiest log lines` are optional (the user can leave logs
  out). `Connected to ... room` counts relay (re)connects; each one follows a
  resume or a dropped socket.

## Checks so far

Newest first. Each entry: date and branch; what the bundle showed; the
cause; the fix; what was ruled out; what the bundle lacked.

- 2026-10-18, `2284-slow-ios-app-performance` (1.4.1 (284), commit
  d11bec0, iPhone16,2, Agents tab root). Showed: "froze for 86.0 s (4
  freezes)" with `uptime 1m50s`: one frame and one event-loop stall of
  about 86 s (last 15:38:59, so starting about 10 s after the telemetry
  started) and three pairs of about 0.7 s; `FRAME` and `LOOP` averages of
  4 s are that one second averaged over about 22 sampled seconds (the
  rest of the 110 s has no samples); no slow operation near 86 s (the
  largest total is 9.6 s, and the list is sorted by total); chat
  channels, direct messages and reads in flight, the requests a return
  to the app starts; `relay.send.hello` (sent on each connect) inside two
  stalls. Cause: the process was stopped for about 86 s while both stall
  probes were armed, and the probes read the gap as a frozen main
  thread. The heartbeat and frame probe park only when the scene phase
  says the app went to the background. The monitor was installed as
  `.resumed` whatever UIKit's state was, and heard lifecycle changes only
  through `AppServices`, which subscribes after the identity load. So a
  launch in the background (Bluetooth or audio, both in
  `UIBackgroundModes`), a change during that load, or a suspension while
  `.inactive` (the probes run then on purpose) left them armed, and every
  frame and heartbeat gap after the suspension counted. Which of these
  happened on this phone the bundle cannot say. Fix: a watch timer on its
  own queue ticks with the heartbeat. A gap it saw too (or one still
  pending when a beat or frame arrives) is a pause: it is subtracted from
  the heartbeat lag, the frame gap and the telemetry frame, logged as
  `[pause] process paused N ms while visible`, and counted in the header.
  A real block of the main thread with the rest of the process running
  is still a stall, with `pausedMs` when a pause overlapped it.
  `AppLifecycle` starts from `UIApplication.applicationState`, and the
  monitor observes it from the moment it is installed. Ruled out: a real
  86 s hang (no operation spans it, and the burst in flight is the resume
  one); the pull ledger reads (`.forkmesh/pulls/<n>` trees, 24 in 5
  minutes at about 540 ms, plus blobs) finished after the stalls and are
  network time; MetricKit's 1.0 GB of disk writes is from the day before.
  Not checked: what the three pairs of about 0.7 s ran; they name no
  section over 8 ms besides `relay.send.hello` (73 ms), so it was
  SwiftUI's own updates or unwrapped code. Bundle gaps closed: pauses are
  told apart from stalls and counted (`paused while visible` in the
  header, `[pause]` lines in `stalls.log`). Still missing: symbolicated
  stacks, and what started the process (a background launch or a user
  launch).

- 2026-10-18, `2228-slow-ios-app-performance` (1.4.1 (276), commit
  b57c0d2, iPhone16,2, Agents tab root). Showed: "froze for 0.5 s": 23
  stalls in 5 minutes (39 in the hour), up to 540 ms, as loop and frame
  pairs a second apart; `FRAME` avg 72 ms and `LOOP` 44 ms over 5 minutes
  against 41 and 21 over the hour; the Agents list read (`POST
  .../agents/list`, 2.6 s average, up to 9.8 s) in flight in 8 stalls,
  direct-message `room-access` in 6; `MEM` up to 469 MB. No stack, no
  named section, no screen per stall. Measured (release build of
  `ForkMeshCore` on x86, about one A17 core) what runs on the main actor
  in an Agents ring and per streamed delta: the desktop's `agents.list`
  (200 rows) 7 ms; the tab's lines 6 ms a body; the Eagle view (the
  default) re-extracted every followed run's line (1.4 ms each) on each
  delta of any run; the watch bridge parsed up to four whole transcripts
  with `beatsFromTranscript` on the main actor per publish (58 ms per
  64 KB LAN tail, 15 ms per 16 KB relay tail), publishing on every ring and
  LAN list change after its own `agents/list` read. Cause (the largest
  measured, not proven: the bundle does not say whether a watch was
  paired): the watch bridge's transcript parse, up to about 230 ms a
  publish, landing in the same ring burst as the Eagle re-reads and the
  list rebuilds. Fix: `TranscriptBeatCache` parses in a detached task once
  per transcript length (injectable, so a test sees the thread); the Eagle
  feed re-reads only the run whose feed moved (`ControlWatchFeeds.revision`;
  10 deltas to one of six runs read 10 transcripts, not 60). Ruled out by
  measurement: opening the sealed list (7.7 MB for 300 sessions, 1.1 s of
  decrypting, but the main actor never waited over 2 ms); chat history
  replay (ML-KEM 0.18 ms a message; room seals load in a detached task);
  the follower's attachment scan (3-15 ms a delta on 200 KB, but the
  desktop's first follow reply lists attachments, so it never runs); the
  agent dots' beat parsing (its `@Sendable` load closures run off the main
  actor, checked); the Genie and aquarium scan (timed, never 200 ms; it can
  hold 100-190 ms unreported every 10 s or more). Not fixed: each ring
  re-downloads every sealed snapshot with its 16 KB transcript tail, twice
  with a watch (the bridge runs its own `AgentsListModel`); the list needs
  only the metadata, so the desktop should seal the transcript apart
  (desktop, relay and both phones). Each new chat message writes its
  ratchet and replay book to the Keychain on the main actor. Bundle gaps
  closed: stalls carry `sections`, the named main-actor work over 8 ms that
  ran inside them, summed in "Main-actor work during stalls"; nested
  sections record one `sync` stall; sections wrap relay frames
  (`relay.event`, `relay.control`), the desktop list (`agents.desktopList`,
  `agents.mergeRows`), the tab's lines (`agents.lines`), the Eagle feed
  (`agents.eagle`), chat receive (`chat.direct.receive`,
  `chat.office.receive`, `chat.office.history`) and the watch publish
  (`watch.fit`, `watch.context`); the header gains `agents` and `watch`.
  Still missing: SwiftUI's own update cost (the Eagle rows' 60 Hz ticker
  timelines, list diffing) shows only as unnamed time; symbolicated stacks.

- 2026-10-17, `2214-slow-ios-app-performance` (1.4.1 (275), iPhone16,2,
  Inbox tab, screen Shortcuts after forkmesh/forkmesh). Showed: "froze
  for 0.3 s": an event-loop stall of 324 ms and a 354 ms frame at
  22:58:36, 17 stalls in the hour; at 22:58:15 the repository overview
  listed `.forkmesh/issues/open` (746 ms), which starts the issue read;
  `FRAME` avg 51 ms and `LOOP` avg 24 ms; a DJ jam presence POST every
  4.5 s with a deck playing behind other screens; slow operations of 8 s
  and 9-15 s. Cause: `ApiService.publishedIssues` fetched the issue
  records in batches of 50 (parsed off the main actor, being over 16 KB)
  and then parsed every record's JSON (forkmesh/forkmesh has had 730
  issue numbers) on the main actor in one pass. That pass is the stall. It lands after
  the user has left the overview, because the read runs in the issues
  cache. Fix: the records are parsed and sorted in a detached task
  (`issueRecord`, injectable so a test sees the thread), and so are the
  release manifests the same overview reads. Ruled out: the
  8 s and 12 s operations are request timeouts (tree reads failed 2 of 5),
  not a busy main thread; the Shortcuts screen reads the phone's copy in a
  detached task; the presence reply and the 30 Hz now-playing chip are
  steady small work, not a 300 ms block (not measured; a deck playing off
  screen keeps the studio's poll, player ticks and AUTOMIX loop running,
  so the earlier "the DJ studio's animations stop" holds only when no deck
  plays). Bundle gaps closed: every stall records the screen showing
  (`PerformanceMonitorService.screenName`) and the report groups stalls
  by screen. Still missing: symbolicated stacks.
- 2026-10-17, `2194-slow-ios-app-performance` (1.4.1 (268), iPhone16,2,
  Agents tab). Showed: 21 stalls in an hour (up to 620 ms), none in the last
  5 minutes; `/api/orgs` in flight in 8 of them, with chat channels, direct
  messages, owner keys, desktops and channel members; operations of 3-5 s
  whose endpoints answered in about 300 ms; two relay reconnects in 5
  minutes. Cause: every return to the app (including closing Control
  Center or Notification Center, which goes inactive and back) refreshes the
  account's packages, the organization package state and Home's counts, and
  each of those rewrote observed state the shell reads (`state`,
  `monitors`, `installed`, `adReviewer`, `fetched`, the organization
  ledger) even when nothing had changed, so Home, its grid and the bottom
  bar were rebuilt several times inside the resume burst. Fix: those writes
  happen only on a real change, and the scene-phase refresh of account
  packages skips when the last one is under a minute old
  (`AccountPackagesStore.refreshIfStale`). Ruled out: reply parsing and
  owner-sealed unsealing (already off the main actor), the Agents list
  polling (it has none; rings are debounced), the DJ studio (its
  animations stop when no deck plays). Bundle gaps closed: stalls get their
  own section with the time of the newest, and `RELAY` is wired. (Its
  operations of 3-5 s were read as main-thread waits; see "Slow
  operations" above for why that reading overstated it.)
- 2026-10-16, `2158-slow-ios-app-performance`: Home's count refresh flooded
  a hot phone; counts now follow `NavigatorCountPlan` (fresh for 60 s, 300 s
  when warm, three reads at a time).
- 2026-10-16, `2134-slow-ios-app-performance`: a launch ring re-read every
  agent list; rings during a load now run one more load afterwards.
- 2026-10-16, `2124-slow-ios-app-performance`: the frame probe reported a
  whole skipped tick as the stall (inflated 100 ms holds to 200 ms);
  Keychain room keys, sleep detection and Core Motion history moved off the
  main thread on resume.
- 2026-10-14/15, `1994` and `2071`: the Genie spider's accessibility scan
  hitched the main thread every 2-3 s; scans back off. Chat history saves
  moved to a utility queue.
- 2026-10-13, `1996-ios-performance`: shell redraws per relay frame and per
  count change, hidden tabs animating, repeated picture decodes, quadratic
  Home layout ([README.md](README.md), Recent changes).

The standing rules these produced are in [README.md](README.md), "iOS
performance rules".

## Adding a check

1. Check out the bundle's commit and read the code that ran.
2. Find the burst or screen the stalls belong to before reading code
   paths; say what in the bundle points there.
3. Fix the cause with a test in `ForkMeshCore` (or Android `core`) that
   fails without the fix.
4. Add an entry under "Checks so far" in the format above, and correct
   "Reading the bundle" where it misled you.
5. If you had to guess because the bundle lacked something, make the app
   collect it (`PerformanceReport`, `PerformanceMonitorService`) in the
   same change and list it as a gap closed.
