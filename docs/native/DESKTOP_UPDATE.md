# Updating the desktop from the phone

The desktop page of the phone app (iOS `DesktopScreen`, Android
`DesktopScreen`) has a **ForkMesh on this desktop** section. It shows the
version the desktop runs and a **Rebuild and restart** button. The button runs
the same quick rebuild and relaunch as the **Restart** button next to
**Resize** in the desktop's rail and debug bar (`quickRebuildRestart`): it
rebuilds the desktop's local source checkout and relaunches, without pulling
from a mirror, and the build log streams onto the page while it runs.

The protocol action is named `updateRestart`; on every desktop that has the
`update.` family it runs the quick rebuild, never a pull. A failed pull must
not block a restart the desktop's own Restart button could do.

On iOS the confirmation sheet hangs off `DesktopScreen`'s `List`, not off
`DesktopUpdateSection`: a modifier on a `Section` inside a `List` applies to
each row, and a rebuilt row would dismiss the sheet right after it opens.

## Code map

| Piece | Where |
| --- | --- |
| The `update.` family: names, confirmation summary, the line feed and its batching | `desktop/src/LocalControlUpdate.{h,cpp}`, `desktop/tests/test_local_control_update.cpp` |
| Requests, watchers, the flush timer, starting the update | `desktop/src/MainWindowLocalControlUpdate.cpp`; registered in `MainWindowLocalControlFamilies.cpp` |
| Taps on the update log | `appendUpdateLog`, `setUpdateStatus` and `showUpdateLog` (`MainWindowSetup.cpp`); `restartCountdownText` (`MainWindowRepoDetail.cpp`) |
| iOS store and view | `FMDesktopRemote/DesktopUpdateModels.swift` (tests: `DesktopUpdateStoreTests.swift`); `ForkMesh/Features/Desktop/DesktopUpdateSection.swift` |
| Android store and view | `core/.../hosts/DesktopUpdate.kt` (tests: `DesktopUpdateTest.kt`); `app/.../feature/hosts/DesktopUpdateCard.kt` |

## Protocol

The family is owner-only, like `diag.` and `fleet.`: a paired device whose
user is not the desktop's owner gets `not_owner`.

| Method | Answer |
| --- | --- |
| `update.status` | The run state (below), the last 300 log lines as `lines`, `seq`, `version` and `startedAtMs`. It also subscribes the connection to `update.progress`. |
| `update.unwatch` | Ends the subscription. |
| `update.prepare {action: "updateRestart"}` | A confirmation token, as for `diag.prepare`. `already_running` while a rebuild or update runs. |
| `update.run {action, token}` | `{started, action, subject, message}`, then the rebuild starts on the next event-loop turn so the answer leaves first. |

The run state is `{run, running, kind, percent, phase, activity, status,
error, relaunching}`: `percent`, `phase` and `activity` (the countdown text
the desktop shows beside its spinning button) come from the restart spinner,
`status` and `error` from the last `setUpdateStatus`, and `relaunching` is set
once the status reads "Relaunching...". `run` changes each time the update log
is cleared for a new run, so the phone drops the old run's lines.

`update.progress` carries the run state plus `{first, seq, skipped, lines}`,
sent to each watcher every 500 ms when something changed and at once on every
status change. A batch holds at most 150 lines and 7000 characters of lines,
so a burst of compiler output stays inside the relay's per-chunk budget
(`docs/native/DESKTOP_AGENTS.md`); `skipped` counts older lines left out, and
the phone shows it as one "N earlier lines not shown" row. Lines are split on
`\n` and `\r`, ANSI colour codes are removed and a line is cut at 400
characters.

## The restart

The desktop quits right after it relaunches, so the phone expects the link to
drop. The store tracks a run from the moment it sees `running`, whether the
phone or the desktop started it. When `relaunching` arrives, the relay link
closes (`__closed`) or a request fails while tracking, it shows
"Restarting" and asks `update.status` every 3 s for up to 20 minutes:

- an answer from the same process (`startedAtMs` unchanged) that is still
  running is ignored;
- the same process no longer running means the relaunch failed or was
  cancelled: the page shows the desktop's error;
- a new process that is not running means the desktop is back: the page shows
  "Restarted on <version> (was <old version>)".

After 20 minutes without an answer the page says the desktop has not come
back.

A desktop too old to have the family answers `unsupported`; the page then
says to update it once on the desktop instead of showing the button.
