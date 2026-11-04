# Video Mix DJ Studio performance checks

The small stethoscope button in the studio's bottom row (`DjDiagnosticsButton`,
iOS) opens "Studio diagnostics": a live read-out of the studio's frame rate and
hitches, and a Send button that starts a coding agent on `forkmesh/forkmesh`
with a scrubbed bundle. The button turns red with a warning triangle while
`DjVitals.alert` reports a problem from the last 30 s. The prompt
(`DjVitals.lead`) sends the agent here first and asks it to add its check below
and to make the studio collect whatever evidence it lacked, so each check
starts further ahead than the last. Keep this page short: correct a wrong
reading in place rather than adding a contradicting note. The studio itself is
described in [video-mix-dj-studio.md](video-mix-dj-studio.md); the app-wide
checks are in [../native/PERFORMANCE_CHECKS.md](../native/PERFORMANCE_CHECKS.md).
Part of [Marketplace](README.md).

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

- **Header**: seconds of frames measured and the probe's rate. The probe
  (`DjFrameProbe`, `VideoMixDjStudioDiagnostics.swift`) is a `CADisplayLink`
  on the main run loop held to 60 Hz, so it does not force a ProMotion screen
  to 120 Hz by itself, but the studio's own animations can. Long frames that
  are not multiples of 16.7 ms (42, 52-62 ms) are unexplained: they are not
  multiples of 8.3 ms either, and ProMotion varies its rate. The probe runs
  only while the studio is on screen; the first frame and any gap over a
  second (`app in background` / `app back` moments) are not counted. The
  second line gives the span the counters cover: `Main-thread work`,
  `Player ticks` and `Web view scripts` count from when the app-wide studio
  model started (at launch when a set resumes), studio open or closed, so
  divide them by that span, not by the seconds of frames. Only
  `waveform draw`, `record step` and the `App work` section run solely on
  screen.
- **`## Frames`**: frames per second, dropped frames, hitch ms a second and
  the worst frame in the last 10 s, the last 60 s and everything measured.
  The worst-frame series is one number per second, oldest first (`.` = not
  measured), so a burst can be matched to what the user did.
  - `main thread busy ms/s`: how long the main run loop was awake each
    second (two run-loop observers in `DjFrameProbe`). Near 1000 with dropped
    frames means the main thread is the bottleneck; well under that means the
    frames were lost elsewhere (render server, GPU, the web view's processes,
    a hot phone). The average hides bursts: long frames whose busy time
    equals their length mean the main thread was blocked, even at 450 ms/s.
    `main thread busy each second` is the per-second series.
  - `of it on the CPU ms/s`: the main thread's own CPU clock, read at each
    probe frame. CPU near busy means it computed (find the work); CPU well
    under busy means it waited: a synchronous call (WebKit,
    `AVAudioSession`, `UserDefaults`, a file, a lock) or no free core.
    `app CPU, all threads ms/s` (the whole process, can pass 1000) says
    whether other threads were taking the cores.
  - `run-loop wakes a second` tells one long item from a storm of small ones
    (the probe alone wakes it 60 times a second).
  - `of it layout and drawing ms/s`: from just before UIKit's pre-commit
    handler to the run loop's sleep (a third observer at order 1,998,000;
    UIKit's handler is at 1,999,000, Core Animation's commit at 2,000,000).
    This works only up to iOS 17. From iOS 18 UIKit runs layout, SwiftUI and
    the commit from a run-loop source (`_UIUpdateSequenceRun` under a
    source-0 callout), which observers cannot time, so the report prints
    `not seen` instead of 0 once 50 ms of busy time has passed with under 2%
    of it in the commit (`DjVitals.commitShareSeen`). There, layout and
    drawing are inside the rest; use the studio view updates instead.
- **`## Long frames`**: each frame of 50 ms or more, how much of it the main
  thread was busy, on the CPU and in layout and drawing (left out where it is
  `not seen`), run-loop wakes and studio view bodies SwiftUI evaluated in it,
  the work that finished inside it (`inside:`, items of 1 ms or more: studio
  kinds and the app's named main-actor sections as `app <name>`) and the
  moments within half a second before it (`around:`). How to read one:
  - mostly layout and drawing, or hundreds of studio view updates: SwiftUI
    or Core Animation work; find what the busiest view reads that keeps
    changing;
  - on the CPU with few view updates and no `inside:` item: unnamed
    main-actor work (a task, a notification, a network reply); wrap the
    suspect in `PerformanceMonitorService.section` and it shows next time;
  - mostly off the CPU: a wait (compare the app's CPU, look for synchronous
    calls);
  - main thread mostly idle: not lost on the main thread.
- **`## Main-thread work`**: count, average, maximum and how many went over
  4 ms, per kind (`DjWork`): `player tick` and `player event`
  (`DjStudioModel.handle`), `waveform draw` (the Canvas in `DjWaveform`;
  divided by the seconds on screen it is the real redraw rate against the
  `pace` asked for; it times recording the drawing, pixels come later in the
  commit), `record step` (one `DjSpinView.step`), `crossfade step` (one frame
  of `fade`), `automix step`, `scratch move`, `jam reply` (`receive`),
  `state save` (building the saved state; encoding runs on `DjStore`'s
  queue), `deck load` (`DjStudioModel.load`) and `deck start` (`play`); the
  last two run inside an `automix step` when AUTOMIX loads or starts a deck,
  so they split a slow step.
- **`## App work on the main thread while the studio was on screen`**: the
  app's named sections (`PerformanceMonitorService.section`, forwarded
  through `watchSections` while the probe runs, 1 ms or more) by total time.
  Sections nest, so an outer name includes its inner ones.
- **`## Studio view updates`**: how many times SwiftUI evaluated each studio
  view's body (`DjDiagnostics.viewed`, one call at the top of each body;
  `waveform frame` and `clock time` are the timeline contents, `record view`
  the record's `updateUIView`), since the studio first opened and per second.
  A view far above its own pace (the waveform frame at the `pace`, the clock
  time at 4, the rest only on a change) re-evaluates because something it
  reads changes. A new studio view gets the same call.
- **`## Player ticks`**: per deck, ticks seen, the longest gap while playing,
  how many were late, the largest drift and how many corrections snapped
  (over 0.3 s). Late ticks with smooth frames point at the web view or
  YouTube, not the app.
- **`## Web view scripts`**: `evaluateJavaScript` calls since launch and the
  round trip of one call in 16. A slow round trip during a fade or a scratch
  is the bridge working hard.
- **`## Recent moments`**: every moment of the last 30 s (up to 40), newest
  first: start, pause, stop, load, scratch and release, fade and fade done,
  audio read and BPM, player errors, pace changes, sheets opened and closed,
  app background and back, `Deck B ended at 244.8 of 245.0 s`,
  `Deck B end report ignored at 1.2 of 245.0 s` (`DjEnd`),
  `Deck B stopped at …, skipped` when such reports go on for 3 s, and
  `audio read stopped` when a read is cancelled because its song left the
  deck. Jam replies are not moments. A deck already playing when the studio
  opened has no `START`.
- **`## Studio and device`**: both decks (playing, rate, waveform bars,
  scratch audio), the `pace` (and whether it was eased), crossfader, AUTOMIX,
  jam, motion sharing, reactions, the audio session's sample rate, buffer and
  latency, the device, thermal state and Low Power Mode. A `serious` thermal
  state makes every number worse; `pace eased` moments mark the studio
  slowing its animations for it.

## Checks so far

All on an iPhone 15 Pro Max, iOS 27.2, portrait, build 1.4.1.

- **`2233-improve-dj-plugin-performance`** (no device bundle; built the
  instrument). Fixed by reading: a crossfade rebuilt both records and the
  crossfader every frame, scheduled a save every frame and sent two scripts a
  frame; it now saves once at its end and sends both volumes in one script
  (`DjPlayerHost.setVolumes`). The jam people row rebuilt its scroll view 24
  times a second; each icon now has its own timeline. Ruled out: the scratch
  voices' render blocks (no locks, no allocation), save encoding (off the
  main thread).
- **`2257-fixing-video-mix-dj`** (build 281, thermal `serious`, Deck A
  playing, AUTOMIX on, jam of two). 39-41 fps, 320-340 ms of hitches a
  second, about two long frames a second with no studio work inside; the app
  report's FPS fell from 56-61 to 31-36 when the studio opened, so the cost
  came with its views on screen. Cause (inferred): the studio held the
  ProMotion screen at 120 Hz on a throttled phone (a steady record's
  `CABasicAnimation` asked for 120 Hz, timelines up to 80), and the waveform
  moved only in 15 Hz jumps because `DjSpin.step` froze for times before its
  last step. Fix: `DjPace` (see Budgets); `DjSpin.step` goes back along its
  speed for earlier times
  (`DjStudioTests.testWaveformKeepsMovingBetweenRecordStepsTimedForTheNextFrame`,
  `DjPaceTests`). Gaps closed: main-thread busy time, counter span, `pace`
  line and `pace eased` moments. Ruled out: studio main-thread work, player
  ticks, web view scripts (7 ms average), jam replies. Open: whether 60 Hz is
  enough on a hot phone.
- **`2289-fixing-video-mix-dj`** (build 284, thermal nominal, Deck B
  playing untouched, jam of two). 52.7 fps, bursts of 150-376 ms frames every
  3-8 s, each busy for its whole length: the main thread was the bottleneck,
  but studio work was only ~6 ms a second. `record step` reached 129.9 ms
  because `DjSpinView.step` wrapped its change in its own
  `CATransaction.begin`/`commit`, which from a display link flushes the whole
  window (layout, SwiftUI, drawing) and commits twice a frame; a stopped deck
  did this 15 times a second. Fix: `DjRecordMotion` (`none`, `turn`, `spin`)
  leaves an unmoved record alone, and `DjSpinView` writes into the implicit
  transaction with the `transform` action off
  (`DjRecordMotionTests.testAStoppedRecordIsDrawnOnceNotOnEveryStep`: 1 write
  in 150 steps). `surfaces.publish` (`SurfacesCoordinator.publish`: a
  synchronous snapshot read/encode/write, widget reloads, Live Activity) is
  now a named section. Gaps closed: layout split (iOS 17 only), busy series,
  named app sections in long frames (`watchSections`), sheet moments. Ruled
  out: player ticks, waveform draws (50/s against 60), jam replies, heat, the
  Agents screen underneath (it stops its feeds 500 ms after the studio is
  pushed), the PR watcher (`ShellPullsWatch`, hops under 2 ms, estimated).
- **`2330-fixing-video-mix-dj`** (build 293, nominal, no jam, Deck A loaded
  and started near the end). 51.9 fps, main thread busy 333-423 ms/s even
  with nothing playing; worst frame 728 ms while the load sheet was open;
  12 frames of 52-62 ms after Deck A's start and audio read. Layout and
  drawing read 0 everywhere: on iOS 18+ that column measures nothing. Cause
  not found. No studio code changed; the report says `not seen` instead of 0
  (`DjVitalsTests.testBusyTimeWithNoLayoutAndDrawingSaysItWasNotSeenInsteadOfZero`).
  Gaps closed
  (`DjVitalsTests.testLongFramesTellComputingFromWaitingByTheMainThreadsCpuTime`,
  `testStudioViewUpdatesAreCountedPerViewAndPerLongFrame`): main-thread and
  app CPU time, run-loop wakes, studio view updates, app background moments.
  Ruled out: studio work (record steps 0.01 ms average), player ticks, the
  app's named sections (`relay.frame` up to 20.3 ms, `surfaces.publish`,
  `relay.send.*`: over 4 ms each but 113 ms in total and never inside a long
  frame), heat.
- **`2363-fixing-video-mix-dj`** (build 298, nominal, AUTOMIX and AUTO DJ
  on, 8 minutes). 54.3 fps, 47 ms of hitches a second; the last 30 s had
  50-54 ms frames with an idle main thread. Moments showed AUTOMIX loading
  three AUTO DJ songs onto Deck B in four seconds: each was believed to have
  ended 1.2-1.6 s after starting (an end report sets the position to the
  song's length). Each reload costs the web view's player and stream, an
  audio download and decode, artwork and a 0.4 s fade, mostly off the main
  thread. Where the false end comes from is unproven (the host tick reading
  `getPlayerState` right after `cueVideoById`, or a stale voice message
  accepted for 1.5 s before the frame learns the new `gen`). Separately,
  `refreshSmart` rebuilt every smart playlist on the main thread at each song
  start (26.5 ms for 500 songs). Fix
  (`DjPartyTests.testAnEndReportedOneSecondIntoASongDoesNotMakeAutomixLoadAnotherSong`,
  `testARealEndIsBelievedNearTheEndOrWhenTheLengthIsUnknown`): `DjEnd.judge`
  (`FMPackages/DjParty.swift`) believes an end report only within 5 s of the
  song's length or when the length is unknown; ignored reports that go on for
  3 s skip the song for the session. `refreshSmart` builds on a background
  task (newest result kept; still immediate when the studio is created). An
  audio read is cancelled when its song leaves the deck. Gaps closed
  (`DjVitalsTests.testACommitSliverInMinutesOfBusyTimeStillSaysNotSeenInsteadOfZero`,
  `testRecentMomentsCoverTheAlertsThirtySecondsNotJustTheLastSixteen`): end,
  ignored-end, skip and read-stopped moments; `deck load` / `deck start`;
  30 s of moments. Ruled out: heat, named app sections, web view scripts
  (3.5 ms average), waveform draws (59/s), record steps. Open: a steady 47 ms
  of hitches a second outside the reloads with the main thread ~25% busy, and
  late player ticks (Deck B 267 of 1282) with an idle main thread, which
  point at the web view. Next: with AUTO DJ quiet, measure the web view
  (`DjPlayerHost.dock` keeps it behind the studio, decoding both decks'
  video).

## Adding a check

1. Check out the bundle's commit (in the app report header) and read the code
   that ran.
2. Find the burst the problem belongs to (the worst-frame series, the long
   frames and the moments around them) before reading code paths; say what in
   the bundle points there.
3. Fix the cause with a test in `ForkMeshCore` (`FMPackages`) that fails
   without the fix.
4. Add a short entry under "Checks so far" (branch, build, setup, evidence,
   cause, fix with its tests, gaps closed, ruled out, open), and correct
   "Budgets" or "Reading the bundle" where they misled you.
5. If you had to guess because the bundle lacked something, make the studio
   collect it in the same change (a new `DjWork` kind, a `mark`, a
   `DjDiagnostics.viewed` call, a context line in `DjDiagnostics.context`, or
   a new section in `DjVitals.report`) and list it as a gap closed.
