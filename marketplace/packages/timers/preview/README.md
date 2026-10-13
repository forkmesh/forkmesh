# Timers preview

Timers puts a small clock on the ForkMesh desktop's top bar. Clicking it
starts one of three kinds (`TimerKind` in `content/desktop/TimersFeature.h`):

- a timer, which counts up from when it started;
- a countdown, which counts down a length typed as `25m standup`,
  `1h30m deploy freeze` or `coffee in 4m`;
- an alarm, which rings at a time of day: `7:30 standup`, `demo at 5pm`.

Each running entry gets its own chip. A chip's menu pauses, resets, extends or
removes it, and shows when a countdown will finish.

Deadlines are absolute, so a countdown keeps running while the app is closed
and comes back already finished rather than restarting. Entries are stored in
the package's own settings on one machine (`dataLifecycle.localData: true`),
are never published, and are deleted with the package; removing Timers from
the rail offers to save them to a file first, and reinstalling offers to
import that file.

`market.png` is the required Market listing screenshot. It is captured from the
running desktop by the `FORKMESH_TIMERS_SHOT` branch of
`desktop/tests/test_window_resize.cpp`, so the image shows the real contributed
strip rather than a mock-up.
