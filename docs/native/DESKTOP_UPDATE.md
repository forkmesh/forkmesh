# Updating the desktop from the phone

Part of the phone apps ([README.md](README.md)). The desktop page (iOS and
Android `DesktopScreen`) has a **ForkMesh on this desktop** section: the
desktop's version, **Rebuild and restart** and **Restart computer**.

- **Rebuild and restart** runs the same quick rebuild and relaunch as the
  desktop's own **Restart** button next to **Resize** (`quickRebuildRestart`):
  it rebuilds the local source checkout and relaunches, never pulling from a
  mirror, and streams the build log onto the page. The protocol action is
  `updateRestart`. A failed pull must not block a restart the desktop's own
  button could do.
- **Restart computer** restarts the operating system after an owner-only
  confirmation, then shows the restart command in the same compact terminal
  view while the phone waits for the computer to return.

On iOS the confirmation sheet hangs off `DesktopScreen`'s `List`, not off
`DesktopUpdateSection`: a modifier on a `Section` inside a `List` applies to
each row, and a rebuilt row would dismiss the sheet as it opens.

## Restarting the computer

The desktop runs `systemctl --no-ask-password reboot` and keeps the process
until it has accepted or rejected the request, streaming the command and status
through `update.progress`. A launch, authorization or exit failure reaches the
phone as an error, never a false success.

- `systemctl reboot` asks logind over D-Bus, which polkit allows for the
  active local session. (`loginctl` has no `reboot` verb.)
- `--no-ask-password` makes a restart that needs a password fail at once with
  polkit's reason instead of opening a dialog on a screen nobody watches.
- `update.run` for `computerRestart` with a stale or wrong token reports
  `confirmation_expired` or `confirmation_mismatch`, not `unknown_action`.

### "Interactive authentication required"

polkit grants `org.freedesktop.login1.reboot` (and
`reboot-multiple-sessions`, needed while another user is logged in) only to the
*active* local session; for an inactive session or none it is
`auth_admin_keep`. So a desktop running as a systemd user service fails with
this error when another user holds the seat, or after a reboot before anyone
logs in, which is when a phone most wants to restart a headless desktop.

polkit is the security boundary, so only an administrator can lift it.
Settings → General → Startup → **Let my phones restart this computer** installs
`/usr/share/polkit-1/rules.d/49-forkmesh-restart-<uid>.rules` through `pkexec`
(`StartManager::allowRestart`, `desktop/src/BackgroundMode.cpp`), like the boot
sleep permission ([lifecycle.md](../desktop/lifecycle.md)), with one
administrator authentication.

- The rule returns `YES` for `login1.reboot` and
  `login1.reboot-multiple-sessions` when `subject.user` is this account, and
  nothing else: no power-off, no `reboot-ignore-inhibit`, so another user's
  block inhibitor still stops the restart.
- It is its own file, so turning it off (`pkexec rm -f` of that file) leaves
  the sleep rule alone.
- It applies to every process of the account, agents included, and lets the
  account end another user's session by restarting; the checkbox tooltip says
  so.
- Without it, a refusal containing "Interactive authentication required"
  reaches the phone as a message naming the setting
  (`computerRestartFailure`, `desktop/src/LocalControlUpdate.cpp`); other
  failures keep systemctl's words.

## Code map

| Piece | Where |
| --- | --- |
| The `update.` family: names, confirmation summary, the line feed and its batching | `desktop/src/LocalControlUpdate.{h,cpp}`, `desktop/tests/test_local_control_update.cpp` |
| Requests, watchers, the flush timer, starting the update | `desktop/src/MainWindowLocalControlUpdate.cpp`; registered in `MainWindowLocalControlFamilies.cpp` |
| Taps on the update log | `appendUpdateLog`, `setUpdateStatus` and `showUpdateLog` (`MainWindowSetup.cpp`); `restartCountdownText` (`MainWindowRepoDetail.cpp`) |
| iOS store and view | `FMDesktopRemote/DesktopUpdateModels.swift` (tests: `DesktopUpdateStoreTests.swift`); `ForkMesh/Features/Desktop/DesktopUpdateSection.swift` |
| Android store and view | `core/.../hosts/DesktopUpdate.kt` (tests: `DesktopUpdateTest.kt`); `app/.../feature/hosts/DesktopUpdateCard.kt` |

## Protocol

Owner-only, like `diag.` and `fleet.`: a paired device whose user is not the
desktop's owner gets `not_owner`.

| Method | Answer |
| --- | --- |
| `update.status` | The run state (below), the last 300 log lines as `lines`, `seq`, `version` and `startedAtMs`. Also subscribes the connection to `update.progress`. |
| `update.unwatch` | Ends the subscription. |
| `update.prepare {action: "updateRestart"}` | A confirmation token, as for `diag.prepare`. `already_running` while a rebuild or update runs. |
| `update.prepare {action: "computerRestart"}` | A separate confirmation token for restarting the computer. |
| `update.run {action, token}` | `{started, action, subject, message}`; the rebuild or restart starts on the next event-loop turn so the answer leaves first. |

The run state is `{run, running, kind, percent, phase, activity, status,
error, relaunching}`. `percent`, `phase` and `activity` (the countdown text
beside the desktop's spinning button) come from the restart spinner; `status`
and `error` from the last `setUpdateStatus`; `relaunching` is set once the
status reads "Relaunching...". `run` changes each time the log is cleared for a
new run, so the phone drops the old run's lines.

`update.progress` carries the run state plus `{first, seq, skipped, lines}`,
sent to each watcher every 500 ms when something changed and at once on every
status change. A batch holds at most 150 lines and 7000 characters, so a burst
of compiler output stays inside the relay's per-chunk budget
([DESKTOP_AGENTS.md](DESKTOP_AGENTS.md)); `skipped` counts older lines left
out and shows as one "N earlier lines not shown" row. Lines are split on `\n`
and `\r`, stripped of ANSI colour codes and cut at 400 characters.

## The restart

The desktop quits right after it relaunches, so the phone expects the link to
drop. The store tracks a run from the moment it sees `running`, whoever started
it. When `relaunching` arrives, the relay link closes (`__closed`) or a request
fails while tracking, it shows "Restarting" and asks `update.status` every 3 s
for up to 20 minutes:

- same process (`startedAtMs` unchanged), still running: ignored;
- same process, no longer running: the relaunch failed or was cancelled; the
  page shows the desktop's error;
- new process, not running: the desktop is back; the page shows "Restarted on
  <version> (was <old version>)".

After 20 minutes without an answer the page says the desktop has not come
back. A desktop without the family answers `unsupported`, and the page says to
update it once on the desktop.
