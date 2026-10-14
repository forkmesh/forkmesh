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
