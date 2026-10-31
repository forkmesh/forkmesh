# Updating the desktop from the phone

The desktop page of the phone app (iOS `DesktopScreen`, Android
`DesktopScreen`) has a **ForkMesh on this desktop** section. It shows the
version the desktop runs, a **Rebuild and restart** button, and a **Restart
computer** button. Rebuild and restart runs the same quick rebuild and
relaunch as the **Restart** button next to **Resize** in the desktop's rail
and debug bar (`quickRebuildRestart`): it rebuilds the desktop's local source
checkout and relaunches, without pulling from a mirror, and the build log
streams onto the page while it runs. Restart computer asks the desktop to
restart the whole operating system after an owner-only confirmation. Its
acknowledgement immediately opens the same compact terminal view with the
restart command while the phone waits for the computer to return.

The desktop retains the `systemctl --no-ask-password reboot` process until it
has accepted or rejected the request. It streams the command and status through
`update.progress`; a launch, authorization, or exit failure reaches the phone as
an error instead of a false success message.

The command was once `loginctl reboot`, which failed with "Unknown command verb
'reboot', did you mean 'help'?": `loginctl` has no `reboot` verb (systemd 257
included). `systemctl reboot` asks logind over D-Bus, which polkit allows for
the active local session. `--no-ask-password` makes a restart that needs a
password fail at once with polkit's reason, instead of opening a password
dialog on a screen nobody is watching. A `update.run` for `computerRestart`
with a stale or wrong token now reports the token error
(`confirmation_expired`, `confirmation_mismatch`) rather than `unknown_action`.

### "Interactive authentication required"

On 2026-10-28 an iPhone showed "Could not request a computer restart: Call to
Reboot failed: Interactive authentication required." The desktop's account
(`f`) was logged in on seat0, but a second user (`um`) had switched in and held
the seat's active session, and the desktop ran as a systemd user service.
polkit grants `org.freedesktop.login1.reboot` (and
`reboot-multiple-sessions`, needed while another user is logged in) only to
the *active* local session; for an inactive session or no session at all it
is `auth_admin_keep`. logind's `CanReboot` answered `challenge`, and
`--no-ask-password` turned that into the error. The same happens after a
reboot before anyone logs in, which is exactly when a phone wants to restart a
headless desktop. Nothing held a shutdown block inhibitor, so polkit was the
only refusal.

polkit is the security boundary, so only an administrator can lift it. Settings
→ General → Startup has **Let my phones restart this computer**. Turning it on
installs `/usr/share/polkit-1/rules.d/49-forkmesh-restart-<uid>.rules` through
`pkexec` (`StartManager::allowRestart`, `desktop/src/BackgroundMode.cpp`), the
same way and in the same readable directory as the boot sleep permission
(`docs/desktop/lifecycle.md`), with one administrator authentication. The rule
returns `YES` for `org.freedesktop.login1.reboot` and
`org.freedesktop.login1.reboot-multiple-sessions` when `subject.user` is this
account, and grants nothing else: no power-off, no `reboot-ignore-inhibit`, so
another user's block inhibitor still stops the restart. It is a separate file
from the sleep rule so turning it off (`pkexec rm -f` of that one file) leaves
sleep alone. It applies to every process of the account, agents included, and
lets the account restart the computer while another user is logged in, which
ends their session; the checkbox tooltip says so. Without it, a refusal that
contains polkit's "Interactive authentication required" reaches the phone as a
message naming that setting (`computerRestartFailure`,
`desktop/src/LocalControlUpdate.cpp`); other failures keep systemctl's words.

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
| `update.prepare {action: "computerRestart"}` | A separate confirmation token for restarting the entire computer. |
| `update.run {action, token}` | `{started, action, subject, message}`, then the requested rebuild or computer restart starts on the next event-loop turn so the answer leaves first. |

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
