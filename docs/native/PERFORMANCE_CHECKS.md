# Performance checks

"Send diagnostics" on the phone starts an agent with a scrubbed diagnostics
bundle (iOS: `DiagnosticsBundle` in `FMTools`, report from
`PerformanceReport`; Android: `DiagnosticsBundle` in
`core/.../tools/PerformanceDiagnostics.kt`). The prompt sends the agent here
first and asks it to add its check below before committing, so each check
starts from what the previous ones learned. Keep this page short: correct a
wrong reading in place rather than adding a contradicting note.

## Reading the bundle

iOS report, section by section (`PerformanceReport.text`; at most 4000
characters, lowest-ranked sections are trimmed first and say so):

- Header. `build ... commit <sha>`: check out that commit to read the code
  that ran. `tab`, `screen`, `recent screens` are the state when the report
  was built, not when the stalls happened. `stalls since launch` counts
  frame, event-loop and named sync stalls of 200 ms or more.
- `FPS` comes from a 60 Hz probe (`TelemetryFrameProbe`), so about 60 is
  normal even on a 120 Hz phone; it is not the display rate.
- `FRAME` is the longest gap between two probe callbacks in each second,
  `LOOP` the worst heartbeat lag in each second (`PerformanceMonitor`).
  `avg` is the average of those per-second worsts. An average `LOOP` of
  10-20 ms means the main thread is rarely idle; a high `max` alone is one
  stall.
- `RELAY` is relay frames received. Before 2026-10-17 the hook was never
  wired and it always read 0.
- `Stalls by name` (before 2026-10-17 merged into `Slow events by name`,
  where operations pushed the stalls out as `... N more`):
  `eventLoop ui-isolate blocked` is the heartbeat catching a blocked main
  thread, `frame` a late display callback, `sync <name>` a `timeSync`
  section that names its culprit. `last` is the clock time of the newest.
- `Slow operations by name`: `track`ed awaits over 500 ms. Their time
  includes waiting for the main actor between hops, so an operation far
  slower than the same endpoint in `HTTP endpoints` points at a busy main
  thread, not at the server.
- `In flight or finished during stalls`: requests open while a stall was
  recorded. Correlation, not cause: a request that is in flight in many
  stalls usually marks the burst the stalls happen in (resume, a ring, a
  screen's first load); look at what runs on the main actor when that
  burst lands.
- `stalls.log tail`: the last lines of `diagnostics/stalls.log`. Release
  stacks are unsymbolicated addresses; `libswift_Concurrency` under
  `ForkMesh` frames means a main-actor task job, which is most app code.
- `App log` and `Busiest log lines` are optional (the user can leave logs
  out). `Connected to ... room` counts relay (re)connects; each one follows a
  resume or a dropped socket.

## Checks so far

Newest first. Each entry: date and branch; what the bundle showed; the
cause; the fix; what was ruled out; what the bundle lacked.

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
  own section with the time of the newest, and `RELAY` is wired. Still
  missing: which screen was showing at each stall, and symbolicated stacks.
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
