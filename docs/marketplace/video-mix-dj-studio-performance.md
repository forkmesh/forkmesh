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
- web view scripts answered within 16 ms (round trip, sampled one call in 16).

## Reading the bundle

The bundle is the DJ section (`DjVitals.report`, collected by
`DjVitalsRecorder.shared` while the studio is open), then the app's own
performance report (see the app-wide page) if there is room.

- Header: how many seconds of frames were measured and the probe's rate. The
  probe (`DjFrameProbe`, `VideoMixDjStudioDiagnostics.swift`) is a
  `CADisplayLink` on the main run loop held to 60 Hz, so it does not force a
  ProMotion screen to 120 Hz by itself; it sees what the main thread delays,
  not what the render server drops on its own. It runs only while the studio
  is on screen; the first frame after it starts and any gap over a second (the
  app was in the background) are not counted.
- `## Frames`: frames per second, dropped frames, hitch ms a second and the
  worst frame in the last 10 s, the last 60 s and everything measured. The
  worst-frame series is one number per second, oldest first (`.` = not
  measured), so a burst can be matched to what the user did.
- `## Long frames`: each frame of 50 ms or more, with the studio work that
  finished inside it (`inside:`, items of 1 ms or more) and the moments
  recorded within half a second before it (`around:`). An empty `inside:` with
  a long frame means the time went to something the studio does not measure
  yet (SwiftUI layout, the web view, another part of the app): measure it.
- `## Main-thread work`: count, average, maximum and how many went over 4 ms,
  per kind (`DjWork`): `player tick` and `player event` (`DjStudioModel.handle`),
  `waveform draw` (the Canvas in `DjWaveform`), `record step` (one
  `DjSpinView.step`), `crossfade step` (one frame of `fade`), `automix step`,
  `scratch move`, `jam reply` (`receive`) and `state save` (building the saved
  state; encoding runs on `DjStore`'s queue).
- `## Player ticks`: per deck, ticks seen, the longest gap while playing, how
  many were late, the largest drift and how many corrections snapped (over
  0.3 s). Late ticks with smooth frames point at the web view or YouTube, not
  the app.
- `## Web view scripts`: `evaluateJavaScript` calls sent since launch and the
  round trip of one call in 16. A slow round trip during a fade or a scratch
  is the bridge working hard.
- `## Recent moments`: what the user and the studio did (start, pause, stop,
  load, scratch, fade begin and end, sheet, jam, waveform read), newest first.
- `## Studio and device`: both decks (playing, rate, waveform bars, scratch
  audio), crossfader, AUTOMIX, jam, motion sharing, reactions, the audio
  session's sample rate, buffer and latency, the device, thermal state and Low
  Power Mode. A `serious` thermal state makes every number worse.

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
  thread), the record spin (render-server animation while steady).

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
