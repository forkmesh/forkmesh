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
  ProMotion screen to 120 Hz by itself, but the studio's own animations can.
  Long frames that are not multiples of 16.7 ms (42, 58, 62 ms) were read as
  the screen running faster than the probe; that is unproven: the `2330`
  check's 52-62 ms frames are not multiples of 8.3 ms either, and ProMotion
  varies its rate. It runs only while the studio is on screen; the first
  frame after it starts and any gap over a second (the app was in the
  background: since the `2330` check the moments say `app in background` and
  `app back`) are not counted. The second line gives the span
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
  mean the main thread was blocked, even at 450 ms/s. Busy is not the same as
  computing: `of it on the CPU ms/s` (since the `2330` check, the main
  thread's own CPU clock read at each probe frame) is how much of it the
  main thread actually ran. CPU near busy means it computed (find the work);
  CPU well under busy means it waited: a synchronous call (WebKit,
  `AVAudioSession`, `UserDefaults`, a file, a lock) or no free core, and
  `app CPU, all threads ms/s` (the whole process, so it can pass 1000 on
  several cores) says whether the app's other threads were taking them.
  `run-loop wakes a second` tells one long item from a storm of small ones
  (the probe alone wakes it 60 times a second). `of it layout and drawing
  ms/s` (since the `2289` check) is the part from just before UIKit's
  pre-commit handler to the run loop's sleep (a third observer at order
  1,998,000; UIKit's handler is at 1,999,000, Core Animation's commit at
  2,000,000). That is where UIKit lays out, updates SwiftUI and commits only
  up to iOS 17. From iOS 18 UIKit runs that update from a run-loop source
  (`_UIUpdateSequenceRun` under a source-0 callout), so the observers see
  nothing: iOS 27 read 0 ms over 56 s, a 156 ms busy frame included, and the
  report prints `not seen` instead of 0 once 50 ms of busy time has passed
  with under 2% of it in the commit (`DjVitals.commitShareSeen`; until the
  `2363` check any commit time at all counted as seen, so 8 minutes on
  iOS 27 printed `0 | 0 | 0` again). There, layout and drawing are inside
  the rest. A source can't be timed from observers either: after a turn
  that ran a source, CFRunLoop polls without calling its before-waiting and
  after-waiting observers, so nothing marks where the source ended. Use the
  studio view updates instead (below). `main thread busy each second` is one
  number per second, like the worst-frame series.
- `## Long frames`: each frame of 50 ms or more, how much of it the main
  thread was busy, on the CPU (since the `2330` check) and in layout and
  drawing (left out where it is `not seen`), how many times the run loop
  woke and how many studio view bodies SwiftUI evaluated in it (both since
  the `2330` check), the work that
  finished inside it (`inside:`, items of 1 ms or more: studio kinds, and
  since the `2289` check the app's named main-actor sections as `app <name>`) and the
  moments recorded within half a second before it (`around:`). A busy frame
  that is mostly layout and drawing, or one with hundreds of studio view
  updates, is SwiftUI or Core Animation work; a busy frame on the CPU with few
  view updates and no `inside:` item is unnamed main-actor work (a task, a
  notification, a network reply): wrap the suspect in
  `PerformanceMonitorService.section` and it shows here next time. A busy
  frame mostly off the CPU was a wait (see `of it on the CPU`). A long frame
  with the main thread mostly idle was not lost on the main thread.
- `## Main-thread work`: count, average, maximum and how many went over 4 ms,
  per kind (`DjWork`): `player tick` and `player event` (`DjStudioModel.handle`),
  `waveform draw` (the Canvas in `DjWaveform`; divided by the seconds on
  screen it is the rate the moving waveforms were actually redrawn, against
  the `pace` asked for; it times recording the drawing, the pixels are drawn
  later in the commit), `record step` (one `DjSpinView.step`; before the `2289` check
  it committed its own Core Animation transaction, so its maximum could hold
  the whole window's layout and drawing), `crossfade step` (one frame of `fade`), `automix step`,
  `scratch move`, `jam reply` (`receive`) and `state save` (building the saved
  state; encoding runs on `DjStore`'s queue). Since the `2363` check `deck
  load` (`DjStudioModel.load`) and `deck start` (`play`) are timed too; they
  run inside an `automix step` when AUTOMIX loads or starts a deck, so they
  tell which part of a slow step it was.
- `## App work on the main thread while the studio was on screen` (since
  check 3): the app's named sections (`PerformanceMonitorService.section`,
  forwarded through `watchSections` while the probe runs, 1 ms or more) by
  total time: count, average, maximum, total. Sections nest, so an outer
  name includes its inner ones.
- `## Studio view updates` (since the `2330` check): how many times SwiftUI
  evaluated each studio view's body (`DjDiagnostics.viewed`, one call at the
  top of each body; `waveform frame` and `clock time` are the timeline
  contents, `record view` the record's `updateUIView`), since the studio
  first opened, and per second measured. A view far above its own pace (the
  waveform frame at the `pace`, the clock time at 4, the rest only on a
  change) re-evaluates because something it reads changes: find that
  property. A new studio view gets the same call.
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
  closed, since the `2330` check the app going to the background and coming
  back. Since the `2363` check: `Deck B ended at 244.8 of 245.0 s` when a
  playing song ends, `Deck B end report ignored at 1.2 of 245.0 s` when the
  player says a song ended that the deck cannot have reached (`DjEnd`, see
  that check), `Deck B stopped at …, skipped` when such reports go on for
  3 s, and `audio read stopped` for a read cancelled because its song left
  the deck. Every moment of the last 30 s is listed (up to 40), not only the
  newest 16. Jam replies are not moments (see `jam reply`). A deck that was
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
  second and per long frame (it works only up to iOS 17: see the next check); a busy series per second; the app's named
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

- 2026-10-19, `2330-fixing-video-mix-dj`: 1.4.1 build 293 (commit
  09cff25, the studio code of the check above), iPhone 15 Pro Max, iOS 27.2,
  portrait, thermal state nominal, no Low Power Mode, 120 Hz screen. No jam,
  AUTOMIX off, empty queue. The load sheet for the queue was open from -72.6
  to -15.8 s. Deck A was loaded at -10.8 s and started at -8.3 s; its audio
  was read and its BPM found at -6.1 s. Deck B was loaded and stopped.
  Evidence: 51.9 fps against the 60 Hz probe over 56 s, 126 ms of hitches a
  second (230 in the last 10 s). The worst frame, 728 ms, came while the
  load sheet was open, at the app report's 845 ms stall on the studio screen
  (18:27:02, -72 s), before the alert's 30 s. The main thread was busy
  403 ms a second. It stayed at 333-423 even in seconds with nothing playing
  and 17 ms worst frames (-15 to -10 s). It peaked at 877 at -19 s, the first
  second measured after a 10 s gap in the series (most likely the app coming
  back from the background, which was not a moment then), and at 672-844
  from -6 to -4 s, Deck A's start and audio read. Right after those came 12
  frames of 52-62 ms in 1.1 s, each busy 42-60 ms, then a 160 ms frame
  (156 busy) at -2.0 s with nothing around it. Layout and drawing read 0 ms
  in every column and every long frame. No long frame had studio or app work
  inside it. The studio's work stayed small: record steps (1622, 29 a
  second, both decks at the 15 Hz idle pace, 0.01 ms average, 2.8 ms
  at most), waveform draws (419, about 52 a second over Deck A's 8 s of
  play), ticks and saves (0.01-0.03 ms).
  Cause: not found; the bundle could not tell what fills about 40% of
  the main thread. What it got wrong: the layout and drawing column. The
  observers that time it sit around UIKit's commit in the run loop's
  before-waiting step, where UIKit committed up to iOS 17. iOS 18 and later
  run layout, SwiftUI and the commit from a run-loop source, so the 0 ms
  meant "not measured". The `2289` check's advice for frames with neither
  layout nor an `app` item did not apply. These long frames could be the
  studio's SwiftUI updates, unnamed main-actor work, or a main thread
  waiting on a call or a core, and the bundle had nothing to tell those
  apart.
  Fix: no studio code changed, because nothing in the bundle pointed at
  studio code. The report says `not seen` instead of a misleading 0 when
  busy time passes with no commit time
  (`DjVitalsTests.testBusyTimeWithNoLayoutAndDrawingSaysItWasNotSeenInsteadOfZero`).
  Gaps closed (`DjVitalsTests.testLongFramesTellComputingFromWaitingByTheMainThreadsCpuTime`,
  `testStudioViewUpdatesAreCountedPerViewAndPerLongFrame`):
  - the main thread's CPU time, a second and per long frame, to tell
    computing from waiting;
  - the whole app's CPU time, to tell whether other threads took the cores;
  - run-loop wakes, a second and per long frame;
  - studio view updates per view and per long frame, which stand in for
    the layout split on iOS 18 and later;
  - `app in background`, `app back` and `app closing` moments.
  Next check: a long frame on the CPU with many studio view updates is the
  studio's SwiftUI work. Find what the busiest view reads that keeps
  changing. One on the CPU with few view updates and no `inside:` item is
  unnamed app work, so wrap the next suspect. One mostly off the CPU is a
  wait, so compare the app's CPU and look for synchronous calls.
  Ruled out:
  - studio work on the main thread (above);
  - player ticks (Deck A's longest gap 0.30 s, none late, one 0.32 s snap,
    the first correction after START) and web view scripts (12 sent);
  - the app's named sections (`relay.frame` 9 times, 6.3 ms average and
    20.3 ms at most; `surfaces.publish` 4 times, 6.2 ms; `relay.send.hello`
    and `relay.send.presence` 9-13 ms). Each one is over the 4 ms budget, but
    together they came to 113 ms, and none fell inside a long frame;
  - heat and Low Power Mode;
  - the diagnostics sheet, opened at the end.

- 2026-10-20, `2363-fixing-video-mix-dj`: 1.4.1 build 298 (commit
  7fdfc5d), iPhone 15 Pro Max, iOS 27.2, portrait, thermal state nominal, no
  Low Power Mode, 120 Hz screen. AUTOMIX and AUTO DJ on (7 choices), empty
  queue, no jam. Deck A stopped (129 s song), Deck B playing (410 s), 8 min
  measured.
  Evidence: 54.3 fps over the 8 minutes, 47 ms of hitches a second (129 in
  the last 10 s), worst frame 292 ms. The last 30 s had bad seconds at
  -19 s (139 ms), -16 and -14 s (85 ms), then every second from -10 to -4 s
  (47-66 ms). The 12 long frames listed (-7.2 to -4.1 s) were 50-54 ms with
  the main thread busy only 3-16 ms, 6-15 wakes, 2-3 view updates and no
  work inside, so they were not lost on the main thread. The moments around
  them show AUTOMIX loading Deck B at about -9.5, -7.9 and -5.9 s, each
  load followed 0.4-0.6 s later by `fade to B over 0.4 s`. A load with
  nothing playing is `DjAutomix.step`'s `.preload`, which it returns only
  when neither deck is cued, and a 0.4 s fade with nothing playing is its
  `.start`. So each song AUTO DJ put on Deck B was started, and 1.2-1.6 s
  later was no longer playing and counted as finished, with no player
  error. Only an end report does that: `apply` moves the position to the
  song's length on state 0. The third song kept playing. An audio read also
  finished at -1.2 s for a song that had already left the deck: the reads
  started at -9.3 and -7.7 s were never stopped. The app's CPU was
  336 ms a second in the last 10 s against 189 on the main thread. The only
  studio work over 4 ms was `automix step` (67.6 ms at most, 3 over) and
  `player event` (3.75 ms average, 23.5 at most, 16 of 67 over). Both run
  `refreshSmart`: `play` counts the play, and a `.meta` title change
  retitles the library. That call rebuilt every smart playlist
  (`DjSmart.lists`) on the main thread at every song start: 26.5 ms for
  500 songs in a release build on a desktop CPU, enough for the 23.5 ms
  event.
  Cause: AUTOMIX believed a player report that a song had ended about a
  second after it started, and loaded the next AUTO DJ pick onto the deck:
  three songs in four seconds. Each reload costs the web view's player and
  stream, a new audio download and decode, the artwork, a 0.4 s fade of 25
  volume scripts, and a song start on the main thread. That load landed off
  the main thread, in the web view's processes and the app's other threads,
  which is why the long frames had an idle main thread. Where the false end
  came from is not proven, because the bundle had no moment for a song's end.
  Two paths can deliver one. The host page's tick reads `getPlayerState`
  right after `cueVideoById`. A voice message the frame posted before it
  learned the new `gen` is still accepted by the host page and counts for
  1.5 s. A song YouTube really stops at once would look the same.
  Separately, the studio's main-thread outliers (the 67.6 ms step, the
  slow events) were the smart playlists rebuilt at every song start, which
  the reloads multiplied.
  Fix (`DjPartyTests.testAnEndReportedOneSecondIntoASongDoesNotMakeAutomixLoadAnotherSong`,
  `testARealEndIsBelievedNearTheEndOrWhenTheLengthIsUnknown`):
  - `DjEnd.judge` (`FMPackages/DjParty.swift`) believes an end report only
    within 5 s of the song's length, or when the length is unknown. Any
    other end report is ignored, so the deck keeps playing and AUTOMIX
    waits. If it goes on for 3 s while the deck should play, the song counts
    as failed: it is skipped for the session like a player error, and
    AUTOMIX loads the next one.
  - `refreshSmart` builds the lists on a background task, with only the
    newest result kept. It still runs at once when the studio is created,
    so a saved smart focus is not dropped.
  - An audio read is cancelled when its song leaves the deck (load, eject,
    Reset All), and the cancel reaches the detached download.
  Gaps closed (`DjVitalsTests.testACommitSliverInMinutesOfBusyTimeStillSaysNotSeenInsteadOfZero`,
  `testRecentMomentsCoverTheAlertsThirtySecondsNotJustTheLastSixteen`):
  - moments for a song's end, an ignored end report, a song skipped for
    one, and an audio read stopped;
  - `deck load` and `deck start` work kinds, to split a slow automix step;
  - every moment of the last 30 s, up to 40, where the newest 16 had cut
    the first reload out of this bundle;
  - `layout and drawing` prints `not seen` instead of `0` when a sliver of
    commit time was seen (see Reading the bundle).
  Ruled out:
  - heat and Low Power Mode;
  - the app's named sections (`relay.send.presence` 11 times at 18 ms,
    `relay.frame`, `surfaces.publish`: 309 ms in 8 minutes, none inside a
    long frame);
  - web view scripts (3.5 ms average round trip, 1 of 35 over 16 ms);
  - waveform draws at the pace (59 a second against 60) and record steps
    (0.05 ms);
  - the diagnostics sheet, opened at the end.
  Not explained: the steady 47 ms of hitches a second over all 8 minutes,
  outside the reloads too, with the main thread about 25% busy and view
  updates at their pace. Player ticks were often late (Deck B 267 of 1282
  over 0.6 s, longest gap 1.02 s; Deck A's longest 1.24 s, one 0.86 s snap)
  with an idle main thread, which points at the web view's processes.
  Next check: if the long frames still have an idle main thread with AUTO
  DJ quiet, measure the web view. `DjPlayerHost.dock` keeps it in the window
  behind the studio, decoding both decks' video.

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
   collect it in the same change (a new `DjWork` kind, a `mark`, a
   `DjDiagnostics.viewed` call, a context line in `DjDiagnostics.context`, or
   a new section in `DjVitals.report`)
   and list it as a gap closed.
