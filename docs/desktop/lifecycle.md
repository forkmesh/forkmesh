# Desktop lifecycle: quitting and the processes it owns

Part of [the desktop docs](README.md). What the desktop starts, what a quit
stops, and what it deliberately leaves running.

Code: `desktop/src/ProcessReaper.{h,cpp}`, called from the end of `main()`.
Test: `forkmesh-process-reaper-tests` (`desktop/tests/test_process_reaper.cpp`).

## Why a reaper is needed

Each owner object stops its own direct child on teardown, but work below or
beside that child can survive: processes left in agent and Actions systemd
scopes (MCP servers, tool shells, builds), children that called `setsid()` or
were started on a PTY, and anything reparented after `~QProcess` signals only
the direct child. Under a systemd user service the cgroup is killed anyway;
launched from a terminal or a `.desktop` file, nothing else would clean up.

## What happens on quit

1. `MainWindow::closeEvent` (window close, or the SIGTERM/SIGINT path in
   `CrashHandler.cpp`) saves agent session state, chat history, geometry and
   the log, and stops WebSocket services. A direct `QCoreApplication::quit()`
   (Rebuild & restart, Update, relaunch) skips this step.
2. `delete window` runs every owner's destructor. Agent CLIs get end of input
   and `SIGTERM`, then up to 1.5 s before `SIGKILL`
   (`ClaudeStreamSession::stopProcess(true)`). Runners write the agent store
   as they go, so sessions can resume.
3. `forkmesh::processreaper::reapOnQuit()` sweeps what is still running and
   writes one stderr line ("Quit: stopped N leftover process(es) ...") when it
   found something.

The sweep runs after the window is gone so no `finished()` handler can record
a reaped Actions step or agent as failed.

## The instance marker

`main()` sets `FORKMESH_DESKTOP_INSTANCE=<128 random bits in hex>` before any
thread or child exists (`initializeInstance()`). Every child inherits it, and
Linux exposes a process's initial environment in `/proc/<pid>/environ`, so the
marker identifies work that escaped the process tree. A new value per launch
means a quitting desktop never matches a newer instance.

## What the reaper selects

`selectTargets()` works on a process-table snapshot (`/proc` on Linux, `ps` on
macOS and other Unixes). It never picks pid 1, the desktop itself, its
ancestors, zombies, or processes of another uid. It picks:

- every process inside a **registered scope** this instance created
  (`registerOwnedUnit`, from `agentresources::contain()` and
  `ActionRunner::launchSandboxedStep`) named `forkmesh-(agent|action)-*.scope`;
- in the desktop's **own cgroup** (or a sub-cgroup): a process carrying this
  instance's marker, a descendant of the desktop, or a member of such a
  process's group (other than the desktop's own group).

A process that carries the marker but lives in another systemd unit was placed
there deliberately and is left alone. Without cgroups (macOS) only descendants
and their process groups are found.

## Escalation and the deadline

`reap()` is pure; hooks make it testable without real signals.

1. `SIGTERM` to every target; `systemctl --user stop --no-block` for live
   registered scopes.
2. Every 100 ms, re-read and re-select, so late forks are caught and reused
   pids are never signalled unless they match again.
3. After a 3 s grace period, `SIGKILL` the rest and
   `systemctl --user kill --signal=SIGKILL --kill-whom=all` remaining scopes.
4. At 5 s stop waiting and list survivors. Quit never hangs and stays inside
   the 15 s `CrashHandler` alarm.

## What a quit stops

| Process | How it is found |
| --- | --- |
| Agent CLIs, their MCP servers, tool shells, builds and tests | registered `forkmesh-agent-*.scope` |
| Sign-in PTYs, terminal shells and what runs in them | marker or descendant |
| Actions steps (`bwrap` in `forkmesh-action-*.scope`) and their checkout phases | registered scope; marker |
| Rebuild & restart `cmake`/`ninja`/`git` steps | marker |
| Git pumps, `QProcess` helpers, log followers, audio helpers | descendant or marker |
| Anything above that double-forked or `nohup`ed itself | marker |

## What a quit deliberately leaves running

| Process | Why |
| --- | --- |
| Fleet operations running as their own systemd units (core add/remove, fleet deploy) | Stopping them half way leaves a node half-provisioned or a fleet split across releases; the dialogs re-attach to the unit's log on the next start. |
| User services and timers (source publisher, mirror maintenance, release timers, backups) | Installed to run with or without the desktop; their prefixes are protected (`defaultProtectedUnitPrefixes()`). |
| Anything started with `systemd-run --user --unit=…` | An own unit means "outlive me". |
| Launches meant to outlive the desktop | Started with `processreaper::startReleased()` or `releasedEnvironment()`, which drop the marker and double-fork (relaunch after update, hand-back to the supervising service, opening other apps). |
| The shared VM for isolated agents | Slow to boot and shared; started without the marker. |
| Other desktop instances | Different marker. |
| ForkMesh without a window started at close, and its tray | Started released, in their own `forkmesh-headless-<project>` / `forkmesh-tray-<project>` scope or unit; both prefixes are protected. |

## Actions runs

A run active at quit stays Running on disk and is re-queued on the next start
(`requeueAfterDesktopInterruption`). The reaper only ensures the step's sandbox
and its scope are gone.

## Limits

- A second stop signal, `SIGKILL` or a crash skips the sweep; registered scopes
  then live until their own limits or until their processes exit.
- A process that clears its environment and leaves the process tree without
  entering a registered scope is not found.
- Windows: no sweep; per-object stops apply.

## Running without a window

Code: `desktop/src/BackgroundMode.{h,cpp}`, `BackgroundSettings.{h,cpp}`,
`TrayMode.cpp`, `SingleInstance.cpp`, `main.cpp`. Test:
`forkmesh-background-mode-tests` (`desktop/tests/test_background_mode.cpp`).

The desktop is a single Qt process. `forkmesh --headless` runs that same
process with the `offscreen` platform and no splash: it signs in, opens the
relay and the LAN room, runs agents and answers every phone request the window
answers. It does not save the window geometry, and it runs the deferred
startup (sign-in, relay, repository restore) at once instead of after the
5 s a window leaves for its first paint. Linux only, and only for a
project's own state folder (not a sandboxed `HOME` or test profile).

### Why there is no separate engine

From 2026-10-13 to 2026-10-16 a Go background engine, `forkmesh-core` in
`desktop/core/`, took over method families from the desktop and talked to it
over a local socket. It was removed on 2026-10-16. Every family it served
already had a C++ handler, so the engine duplicated the desktop rather than
replacing it: the IPC hop, the method fallbacks and two writers for the same
files were the cost, and the phone still needed the desktop process for
agents, teammates, wallet, Discord and pings. The parts worth keeping were the
lifecycle ones (start at boot, run with no window, a tray icon), and they are
now done by the desktop binary itself. Its service plug-ins and the iOS
"Background services" mini-apps went with it, and so did its encrypted secrets
vault, which was never finalized, so the plaintext settings it copied from
stayed current.

### Settings

Settings → General → STARTUP:

- **Keep ForkMesh running without a window after I close it**
  (`background/keepRunning`, off by default). Closing the window starts
  `forkmesh --headless --project <slug> --wait-for-lock` and the tray.
  Turning it on also keeps ForkMesh running after you log out (see
  [Logging out](#logging-out)).
- **Start ForkMesh without a window when the computer starts.** Turning it on
  also turns on the first box. The settings page runs `systemctl` and
  `loginctl` off the UI thread.

### Handing over at close

`MainWindow::closeEvent` calls `handOverAfterWindowClose`. It hands over
only when the setting is on, the window still holds the single-instance lock,
and the close was not a stop signal or a restart. Rebuild & restart, update,
switching project and deleting the account all release the lock before they
quit, so they never hand over. Whenever a user systemd is reachable (the
window runs as a service, or `$XDG_RUNTIME_DIR/systemd/private` exists), the
windowless instance is started in its own scope
(`systemd-run --user --scope --unit=forkmesh-headless-<project>`), so neither
stopping the window's unit (`forkmesh-source.service`) nor the end of the
launcher's `app-*.scope` at log-out stops it, and the service variables
(`INVOCATION_ID`, `NOTIFY_SOCKET`, …) are removed from its environment. When
the start-at-boot unit is installed, the window starts that unit instead
(`systemctl --user start --no-block`), so systemd restarts it after a crash.
`--wait-for-lock` makes the new instance wait up to 60 s for the closing window
to let go of `state/forkmesh.lock`.

### Opening a window again

A new `forkmesh` finds the lock held and sends `activate`. The windowless
instance answers `yield` and shuts down the way SIGTERM does (with a 25 s
`_Exit` backstop), and the new launch waits up to 45 s for the lock behind the
splash ("Taking over from ForkMesh running without a window"). A window that
took over (`tookOverFromBackgroundInstance()`) runs its deferred startup at
once, like a windowless instance, instead of after the 5 s first-paint delay:
the phones it serves were connected a moment ago, and the delay used to leave
them with no LAN listener and no relay for about 8 s. A windowless
instance that finds the lock held by a window exits without activating it. It
writes its pid to `state/forkmesh-background.pid`.

### Agents across a stop

No agent process outlives the desktop that started it; the next instance,
window or windowless, resumes every session left `running` (its deferred
startup turns them into `queued` and pumps the queue with
`--resume <session>`). What decides whether that works is the record the
stopping instance leaves behind:

- A window closing, a yield to a window and a stop signal all go through
  `closeEvent` (`m_closingDown`), and the destructor disconnects agent
  processes before it stops them, so their exits are never recorded.
- At a reboot or log-out, systemd stops the agents' own
  `forkmesh-agent-*.scope` units at the same time as the desktop, so an
  agent's exit can reach `finishCliTranscriptRun` before the window closes.
  It used to count as a crash: the session was relaunched into the dying
  system, the relaunches failed, and after three attempts it was marked
  failed, so the next start (often the headless unit at boot) had nothing to
  resume (2026-10-16, sessions killed at 16:07 were failed by 16:08 and had
  to be continued by hand). Now, while `desktopStopping()` (closing down or a
  stop signal received), an exit leaves a running or waiting session as it
  is (`keepCliRunForNextStart`), a turn that was only waiting for its
  background tasks included, and `processAgentQueue` starts nothing.
- An exit with 143 or 129 (SIGTERM, SIGHUP from outside; a user stop marks
  the session stopped first) is judged 3 s later, so a stop signal that
  reaches the desktop just after its agents is still seen. A runtime that was
  replaced in between is not judged again.
- `AgentRunner` (API runners) does the same through `interruptForShutdown`,
  as Actions do.

### No display

A window launch that waits 30 s for a display and gives up (exit 3, e.g.
`forkmesh-source.service` restarting before the desktop session is up, or
after a log-out) starts the windowless instance when **Keep ForkMesh running
without a window** is on (`handOverWithoutDisplay` in `main.cpp`), so phones
and agents are served until a window can open; that window then takes over as
usual. Starting it when it already runs does nothing.

### Logging out

Asked for on 2026-10-16: turn ForkMesh off, log out, and have it keep
running. Two things stood in the way. Lingering was only turned on with
**Start at boot**, so with just **Keep running** the user manager, and the
windowless instance in it, stopped at log-out. And logging out with the
window open never handed over: the compositor goes away, Qt `_exit`s on the
broken display connection, and no `closeEvent` runs.

With **Keep running** on, the window (5 s after it starts, on a pool thread,
`syncLogoutHandOver`) and the settings box install
`~/.config/systemd/user/forkmesh-logout-<project>.service` and turn lingering
on (the same `forkmesh-headless.linger` marker; the marker is released only
when no `forkmesh-headless-*` or `forkmesh-logout-*` unit is left). The unit
is `Type=oneshot`, `RemainAfterExit=yes`, `PartOf=` and
`After=graphical-session.target`, `WantedBy=graphical-session.target`, and
has only an `ExecStop`: `forkmesh --hand-over-at-logout --project <slug>`.
At log-out systemd stops it before the target, and so before GNOME Shell
(`graphical-session.target` is ordered after `gnome-session.target`, which is
after `org.gnome.Shell@wayland.service`), while the window can still close
normally. Turning **Keep running** off disables and removes it.

`--hand-over-at-logout` does nothing when **Keep running** is off or the
computer is shutting down (`systemctl is-system-running` or
`systemctl --user is-system-running` says `stopping`, or logind's
`PreparingForShutdown` is true); a reboot keeps the stop path described in
[Agents across a stop](#agents-across-a-stop). Otherwise it sends `hand-over`
on the single-instance socket. A window answers `yield`, records that the
session is ending, and closes: `closeEvent` hands over as it does for a
close by hand, without starting the tray (it would lose its display a moment
later) and without writing the source guard's close hold, so
`forkmesh-source.service` comes back at the next log-in. A windowless instance
does not answer. The helper then waits up to 45 s for the window's process to
exit (`TimeoutStopSec=60`).

An installed unit keeps its binary while that binary is still an executable
file, so a worktree build started against the real profile does not point the
unit at a build that is deleted with its worktree.

Limits: a desktop session without `graphical-session.target` (most window
managers started without systemd) never stops the unit at log-out; the window
dies with its display and the windowless instance starts only through
**Start at boot** or the next launch. Without a user systemd there is no
log-out hand-over at all.

### Start at boot

- `~/.config/systemd/user/forkmesh-headless-<project>.service`:
  `ExecStart` quoted for systemd, `Environment=QT_QPA_PLATFORM=offscreen`,
  `Restart=on-failure` (at most 5 starts in 10 minutes), `KillMode=mixed` so
  the desktop shuts down its own agents before systemd kills what is left,
  `WantedBy=default.target`, the ForkMesh path variables unset. Opening a
  window makes it exit 0, so the unit stays stopped until the next close or
  boot.
- A user unit only starts at boot when the user lingers. Enabling runs
  `loginctl --no-ask-password enable-linger <uid>` when lingering is off and
  leaves `forkmesh-headless.linger` beside the units; disabling the last
  `forkmesh-headless-*.service` turns lingering off only when that marker is
  there, so lingering the user set is left alone. If polkit refuses, the unit
  starts at log-in and Settings says how to turn lingering on.
- Without a user systemd it writes
  `~/.config/autostart/forkmesh-headless-<project>.desktop` instead.
- With **Keep running** on, lingering is on already (see
  [Logging out](#logging-out)), and turning start at boot off leaves it on.
- Either way it writes `~/.config/autostart/forkmesh-tray-<project>.desktop`,
  because a unit started at boot has no display for a tray.
- Unit names carry `headless` so a project called `source` cannot collide with
  `forkmesh-source.service`.

### The tray

`forkmesh --tray [--project <slug>]` is a small process with no window, one per
project (`state/tray.lock`). It shows the ForkMesh icon while the windowless
instance holds the lock, with "Open ForkMesh", "Stop ForkMesh running without
a window" (sends `quit` over the single-instance socket; a window ignores it)
and "Hide icon". It exits once a window takes over, 30 s after the windowless
instance is gone, after 90 s if none appeared, or after 2 minutes with no
system tray. The window keeps its own tray icon while it is open.
`forkmesh --stop-background [--project <slug>]` does what the menu item does.

### Several desktops at once

Every piece is keyed by project: the lock and socket in the project's state
folder, the pid file, the tray lock, the unit and autostart names. Two
projects can each run a window or a windowless instance side by side, and
start at boot independently.

### Retiring the Go engine

About 5 s after a window starts, `migrateLegacyEngine` (on a pool thread) looks for what
`forkmesh-core` left behind for this project:
`forkmesh-core-<project>.service` (disabled with `--now` and removed), its XDG
entry, its `forkmesh-core.linger` marker (renamed to `forkmesh-headless.linger`),
a running engine (the pid in `state/core/endpoint.json`, signalled only if
`/proc/<pid>/comm` is `forkmesh-core`), and `background: true` in
`config/core/daemon.json`. On macOS it removes
`com.forkmesh.core.<project>.plist` and on Windows the `forkmesh-core-<project>`
Run value. If the engine was set to start with the computer, the desktop
turns on both settings above and installs its own unit. The old binary and
`state/core/` are left on disk. `install.sh` no longer installs
`forkmesh-core`.

