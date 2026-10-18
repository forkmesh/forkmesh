# Video Mix DJ Studio performance checks

The small stethoscope button in the studio's bottom row (`DjDiagnosticsButton`,
iOS, since 2026-10-18) opens "Studio diagnostics": a live read-out of the
studio's frame rate and hitches, and a Send button that starts a coding agent
on `forkmesh/forkmesh` with a scrubbed bundle. The button turns red with a
warning triangle while `DjVitals.alert` reports a problem from the last 30 s.
The prompt (`DjVitals.lead`) sends the agent here first and asks it to add its
check below and to make the studio collect whatever evidence it lacked, so
each check starts further ahead than the last. Keep this page short: correct a
wrong reading in place rather than adding a contradicting note. The studio
itself is described in [video-mix-dj-studio.md](video-mix-dj-studio.md); the
app-wide checks are in
[../native/PERFORMANCE_CHECKS.md](../native/PERFORMANCE_CHECKS.md).

## Budgets

"Real time" for the studio means:

- every frame on time: at 60 Hz a frame has 16.7 ms, at 120 Hz 8.3 ms. A
  frame that takes more than 1.5 periods is a dropped frame;
- hitch time under 5 ms a second (Apple's hitch-time ratio; 5-10 is
  noticeable, over 10 is bad, and raises the alert);
- no frame over 50 ms (a visible jerk, listed under Long frames), none over
  250 ms (a freeze, raises the alert at once);
- any one piece of studio work on the main thread under 4 ms;
- a player tick from each playing deck every 0.25 s (a gap over 0.6 s is
  late), drift between the predicted and reported position under 0.3 s;
- web view scripts answered within 16 ms (round trip, sampled one call in 16);
- a continuous animation (a steady record, the waveforms, the reactions) asks
  for no more than 60 Hz, and the jam's people icons for 24; when iOS says
  the phone is hot (thermal state `serious` or `critical`) or Low Power Mode
  is on, those halve. Only a record being touched, scratched or spinning up
  asks for the screen's full rate (120 Hz on ProMotion). `DjPace`
  (`FMPackages/DjPace.swift`) holds these numbers and `DjStudioModel.pace`
  follows the thermal and power notifications.

## Reading the bundle

The bundle is the DJ section (`DjVitals.report`, collected by
`DjVitalsRecorder.shared` while the studio is open), then the app's own
performance report (see the app-wide page) if there is room.

- Header: how many seconds of frames were measured and the probe's rate. The
  probe (`DjFrameProbe`, `VideoMixDjStudioDiagnostics.swift`) is a
  `CADisplayLink` on the main run loop held to 60 Hz, so it does not force a
  ProMotion screen to 120 Hz by itself, but the studio's own animations can:
  long frames that are not multiples of 16.7 ms (42, 58, 62 ms) mean the
  screen was running faster than the probe. It runs only while the studio
  is on screen; the first frame after it starts and any gap over a second (the
  app was in the background) are not counted. The second line gives the span
  the counters cover: `Main-thread work`, `Player ticks` and `Web view
  scripts` count from the moment the app-wide studio model started (at launch
  when a set resumes) with the studio open or closed, so divide them by that
  span, not by the seconds of frames. Only `waveform draw` and `record step`
  run solely on screen, and so does the `App work` section.
- `## Frames`: frames per second, dropped frames, hitch ms a second and the
  worst frame in the last 10 s, the last 60 s and everything measured. The
  worst-frame series is one number per second, oldest first (`.` = not
  measured), so a burst can be matched to what the user did. `main thread
  busy ms/s` is how long the main run loop was awake each second (two run-loop
  observers in `DjFrameProbe`, since 2026-10-18): near 1000 with dropped
  frames means the main thread is the bottleneck, so find the work; well under
  that with dropped frames means the frames were lost elsewhere (the render
  server, the GPU, the web view's processes, or iOS throttling a hot phone).
  The average hides bursts: long frames whose busy time equals their length
  mean the main thread was blocked, even at 450 ms/s. `of it layout and
  drawing ms/s` (since the `2289` check) is the part from just before UIKit's
  pre-commit handler to the run loop's sleep (a third observer at order
  1,998,000; UIKit's handler is at 1,999,000, Core Animation's commit at
  2,000,000): view layout, SwiftUI's pending updates and Canvas drawing. The
  rest of the busy time is run-loop sources, timers, display-link callbacks
  and main-actor tasks. If SwiftUI updates outside that commit on some iOS
  version, they show as the rest. `main thread busy each second` is one number
  per second, like the worst-frame series.
- `## Long frames`: each frame of 50 ms or more, how much of it the main
  thread was busy and how much of that was layout and drawing, the work that
  finished inside it (`inside:`, items of 1 ms or more: studio kinds, and
  since the `2289` check the app's named main-actor sections as `app <name>`) and the
  moments recorded within half a second before it (`around:`). A busy frame
  that is mostly layout and drawing is SwiftUI or Core Animation work; a busy
  frame with neither layout nor an `inside:` item is unnamed main-actor work
  (a task, a notification, a network reply): wrap the suspect in
  `PerformanceMonitorService.section` and it shows here next time. A long
  frame with the main thread mostly idle was not lost on the main thread.
- `## Main-thread work`: count, average, maximum and how many went over 4 ms,
  per kind (`DjWork`): `player tick` and `player event` (`DjStudioModel.handle`),
  `waveform draw` (the Canvas in `DjWaveform`; divided by the seconds on
  screen it is the rate the moving waveforms were actually redrawn, against
  the `pace` asked for; it times recording the drawing, the pixels are drawn
  later in the commit), `record step` (one `DjSpinView.step`; before the `2289` check
  it committed its own Core Animation transaction, so its maximum could hold
  the whole window's layout and drawing), `crossfade step` (one frame of `fade`), `automix step`,
  `scratch move`, `jam reply` (`receive`) and `state save` (building the saved
  state; encoding runs on `DjStore`'s queue).
- `## App work on the main thread while the studio was on screen` (since
  check 3): the app's named sections (`PerformanceMonitorService.section`,
  forwarded through `watchSections` while the probe runs, 1 ms or more) by
  total time: count, average, maximum, total. Sections nest, so an outer
  name includes its inner ones.
- `## Player ticks`: per deck, ticks seen, the longest gap while playing, how
  many were late, the largest drift and how many corrections snapped (over
  0.3 s). Late ticks with smooth frames point at the web view or YouTube, not
  the app.
- `## Web view scripts`: `evaluateJavaScript` calls sent since launch and the
  round trip of one call in 16. A slow round trip during a fade or a scratch
  is the bridge working hard.
- `## Recent moments`: what the user and the studio did, newest first: start,
  pause, stop, load, scratch and release, fade and fade done, audio read and
  BPM, player errors, pace changes, and since the `2289` check each sheet opened or
  closed. Jam replies are not moments (see `jam reply`). A deck that was
  already playing when the studio opened has no `START`.
- `## Studio and device`: both decks (playing, rate, waveform bars, scratch
  audio), the `pace` the animations asked for (and whether it was eased),
  crossfader, AUTOMIX, jam, motion sharing, reactions, the audio
  session's sample rate, buffer and latency, the device, thermal state and Low
  Power Mode. A `serious` thermal state makes every number worse; the
  moments list `pace eased` when the studio slowed its animations for it.

## Checks so far

- 2026-10-18, `2233-improve-dj-plugin-performance`: no device bundle yet;
  this change built the instrument and fixed what the code showed. A crossfade
  rebuilt both records and the crossfader every display frame, scheduled a
  save task every frame and sent two web view scripts a frame: the fade now
  saves once at its end and both decks' volumes go to the web view in one
  script (`DjPlayerHost.setVolumes`). In a jam the people row rebuilt its whole
  scroll view 24 times a second while motion was shared: each icon now has
  its own timeline. Ruled out by reading: the scratch voices' render blocks
  (no locks held, no allocation), the save encoding (already off the main
  thread). The record spin was ruled out too as a render-server animation
  while steady, but that animation asked for 120 Hz: see the next check.
- 2026-10-18, `2257-fixing-video-mix-dj`: the first device bundle (1.4.1
  build 281, iPhone 15 Pro Max, iOS 27.2, portrait, thermal state `serious`,
  Deck A playing, AUTOMIX on, a jam of two sharing motion).
  Evidence: 39-41 fps against the 60 Hz probe for all 40 s the studio was
  open, 320-340 ms of hitches a second, a 50-67 ms worst frame in every one of
  the last 30 seconds and about two long frames a second, none with studio
  work inside or a moment around it; every measured kind of work averaged
  under 0.3 ms. The app report's per-minute FPS was 56-61 while the same set
  (AUTOMIX, jam, music) ran with the studio closed and fell to 31-36 once it
  opened, so the cost came with the studio's views on screen, not its model.
  Long frames of 42, 58, 62 and 64 ms showed the screen running faster than
  60 Hz, and the one moving waveform was redrawn 870 times in 40 s, 22 a
  second against the 60 its timeline could have drawn.
  Cause (inferred: the bundle had no main-thread busy time to tell the main
  thread from the render server): the studio held the ProMotion screen at
  120 Hz for the whole set on a phone iOS was already throttling. A steady
  record's `CABasicAnimation` asked for 120 Hz (`DjSpinView.fast`) and the
  waveform and reaction timelines allowed up to 80, so the render server recomposited the platters with
  their shadows and gradients 120 times a second and the main thread was
  woken for each frame, and nothing eased off as the phone got hot. Apart
  from the frame drops, the waveform stuttered on its own: `DjSpinView.step`
  advances the deck's shared `DjSpin` with `link.targetTimestamp`, up to 67 ms
  ahead at its 15 Hz idle pace, and `DjSpin.step` returned its last position
  for any earlier time, so the waveform (asking with the current time) only
  moved when the record stepped, in 15 Hz jumps whatever its own frame rate.
  Fix: `DjPace` (see Budgets): a steady record and the waveform and
  reactions at 60 Hz, people icons at 24, both halved when hot or in Low
  Power Mode, the full 120 Hz only for a touched record. `DjSpin.step` now
  answers a time before its last step by going back along its speed instead
  of freezing (`DjStudioTests.testWaveformKeepsMovingBetweenRecordStepsTimedForTheNextFrame`,
  `DjPaceTests`).
  Gaps closed: main-thread busy time, a second and per long frame, so the
  next check can tell a busy main thread from frames lost elsewhere, which
  this bundle could not; the span the counters cover; a `pace` context line
  and `pace eased` moments.
  Ruled out: studio work on the main thread (the few items over 4 ms were
  single outliers during loads); player ticks (6376 and 2544 over the 28
  minutes the model had run, about 4 and 1.5 a second as designed, not over
  the 40 s of frames); web view scripts (333 since launch, 7 ms average round
  trip); jam replies (0.09 ms); the app's own stalls (400-600 ms on the
  Agents screen during presence and repo-tree requests), which came before
  the studio opened, while its worst frame inside the studio was 182 ms.
  Not yet confirmed on a device: whether 60 Hz is enough on a hot phone
  (the next bundle, below, came from a cool one).
- 2026-10-18, `2289-fixing-video-mix-dj`: 1.4.1 build 284 (commit
  d11bec0, the studio code of the check above), iPhone 15 Pro Max, iOS 27.2,
  portrait, thermal state nominal, no Low Power Mode. Deck B was playing
  (already playing when the studio opened) and Deck A was stopped. AUTOMIX
  was off, and the user was in a jam of two sharing motion. Nobody touched
  anything: the only moments are Deck B's audio read and BPM in the first
  6 s.
  Evidence: 52.7 fps against the 60 Hz probe over 58 s, 383 dropped frames,
  110 ms of hitches a second, worst frame 376 ms. The worst-frame series
  comes in bursts of one to three bad seconds (150-376 ms) every 3-8 s, with
  17 ms seconds between them. The main thread was busy 452 ms a second, and
  every long frame was busy for all of its length (233 of 233 ms, 182 of
  182), so this time the main thread was the bottleneck, not the render
  server. The studio's measured work adds up to about 6 ms a second (record
  steps 242 ms, waveform draws 115 ms, ticks and jam replies 16 ms in 58 s),
  so almost all the busy time was unmeasured. The app report agrees: 8
  stalls of 215-352 ms on `Agents > Video Mix DJ Studio`, with no named
  section in them but a 16 ms `relay.send.presence`, and stalls of the same
  size on the Agents and Inbox screens in the minutes before the studio
  opened. Deck B's one late tick (a 0.62 s gap, 0.35 s drift, one snap) is
  a tick held behind one of those blocks.
  `record step` had a maximum of 129.9 ms, 6 steps over 4 ms, and one 20.7 ms
  step inside a 56 ms frame. `DjSpinView.step` is a few lines of arithmetic.
  It can only take that long because it wrapped its layer change in its own
  `CATransaction.begin`/`commit`. Called from a display link, that is the
  outermost transaction, so its commit flushed the whole window there and
  then: view layout, SwiftUI's pending update and drawing, then a second
  commit later in the same frame. Deck A, stopped, did this 15 times a
  second to set the angle it already had.
  Cause: the 100-376 ms blocks are main-thread work the studio does not do
  and the bundle could not name. It is unnamed main-actor work or SwiftUI
  updates (see Ruled out for what it is not). The studio's own share was
  the record step's forced commits, which pulled other work into the
  record's step and committed twice a frame.
  Candidates for the blocks, read in the code but not proven by the bundle:
  - `SurfacesCoordinator.publish`, 400 ms after every change to the agent
    rows (7 agents were running), on the main actor: a synchronous snapshot
    file read, encode and write, widget reloads, the Live Activity, and App
    Shortcuts parameters. It was not wrapped in a section.
  - SwiftUI's own updates.
  Fix: `DjRecordMotion` (`FMPackages`) decides each step: `none`, `turn` or
  `spin`. A record that has not moved is not touched, and a steady spin keeps
  its one animation. `DjSpinView` writes into the run loop's implicit
  transaction, with the disc's `transform` action turned off instead of an
  explicit transaction, so each frame commits once
  (`DjRecordMotionTests.testAStoppedRecordIsDrawnOnceNotOnEveryStep`: 150
  steps, 1 write, against 150 before). `surfaces.publish` is now a named
  section.
  Gaps closed: busy time split into layout and drawing and the rest, per
  second and per long frame; a busy series per second; the app's named
  sections inside long frames and summed while the studio is on screen
  (`PerformanceMonitorService.watchSections`); a moment for each sheet. Next
  check: if the long frames are mostly layout and drawing, look at SwiftUI
  (what invalidated the studio's views); if an `app` item fills them, that
  code; if neither, wrap the next suspect (the PR watcher's tree reads in
  `ApiService.publishedPulls`, `ShellCounts` writes) or add stack samples.
  Ruled out:
  - player ticks (277, 0.04 ms average) and web view scripts (8 sent);
  - waveform draws (0.04 ms, 50 a second for the one moving deck against the
    60 asked);
  - jam replies (0.16 ms average, 1.3 ms at most) and state saves;
  - heat (nominal);
  - the Agents screen itself: it stops its list, Eagle feed and watch
    500 ms after the studio is pushed over it;
  - the PR watcher (`ShellPullsWatch`, every 20 s, the 86 tree reads) as one
    block: by reading, its main-actor hops are each under 2 ms, an estimate;
  - the diagnostics sheet, opened at the end to send the bundle.
  Not explained: frames of 56, 58, 70 and 74 ms, not multiples of 16.7 ms,
  with no touch. Something kept the screen above 60 Hz for a while, perhaps
  the web view's video or the people icons' timelines.

## Adding a check

1. Check out the bundle's commit (in the app report header) and read the code
   that ran.
2. Find the burst the problem belongs to (the worst-frame series, the long
   frames and the moments around them) before reading code paths; say what in
   the bundle points there.
3. Fix the cause with a test in `ForkMeshCore` (`FMPackages`) that fails
   without the fix.
4. Add an entry under "Checks so far" (date, branch, evidence, cause, fix,
   ruled out), and correct "Budgets" or "Reading the bundle" where they misled
   you.
5. If you had to guess because the bundle lacked something, make the studio
   collect it in the same change (a new `DjWork` kind, a `mark`, a context
   line in `DjDiagnostics.context`, or a new section in `DjVitals.report`)
   and list it as a gap closed.
