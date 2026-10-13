# Desktop lifecycle: quitting and the processes it owns

Owner request (2026-10-01): "When we close ForkMesh, make sure it kills all
related processes." This page says what the desktop starts, what a quit stops,
what it deliberately leaves running, and why.

Code: `desktop/src/ProcessReaper.{h,cpp}`, called from the end of `main()`.
Test: `forkmesh-process-reaper-tests` (`desktop/tests/test_process_reaper.cpp`).

## Why a reaper was needed

Every owned object already stopped its own direct child on teardown
(`ClaudeStreamSession`, `CodexAppServerSession`, `AgentRunner::stop`,
`~ActionRunner`, `~QProcess`). What survived a quit was everything below or
beside that child:

- **Agent scopes.** Claude Code and Codex run in
  `forkmesh-agent-<label>-<desktop pid>-<n>.scope` units
  ([../agents/memory.md](../agents/memory.md)). Stopping the CLI left its MCP
  servers (`tools/forkmesh_mcp_server.py` and others), Bash-tool shells,
  `ninja`/`cmake` builds and test runs alive in the scope, because a scope
  lives as long as any process in it does.
- **Actions steps.** Each step's `bwrap` sandbox runs in
  `forkmesh-action-<desktop pid>-<run>-<step>.scope`. `~ActionRunner` only
  fired an asynchronous `systemctl --user kill`, which could lose the race with
  the process exit.
- **New sessions and process groups.** Rebuild & restart's `cmake`/`ninja`
  (`runUpdateStep`) and Actions phases call `setsid()`; PTYs
  (`TerminalWidget`, `ForkMeshTerm`, `AgentLoginPty`) are session leaders by
  `forkpty`. `~QProcess` signals only the direct child, so grandchildren were
  reparented to the user manager and kept running.
- **Runs launched outside a systemd user service.** When the desktop runs as a
  user service, `systemctl --user stop` kills the service cgroup, which already
  covered the in-cgroup leftovers. Launched from a terminal or a `.desktop`
  file, nothing did.

## What happens on quit

1. `MainWindow::closeEvent` (window close, or the SIGTERM/SIGINT path in
   `CrashHandler.cpp` that calls `window->close()` and `app.quit()`) saves
   agent session token counts (`flushPendingAgentSessionSaves`), chat history,
   geometry and the log, and stops the WebSocket services. A direct
   `QCoreApplication::quit()` (Rebuild & restart, Update, relaunch) skips it
   and goes straight to step 2. Unchanged.
2. `app.exec()` returns and `delete window` runs every owner's destructor. This
   is where agent sessions get their graceful stop: the CLI gets end of input
   and `SIGTERM`, then up to 1.5 s before `SIGKILL`
   (`ClaudeStreamSession::stopProcess(true)`), which is the stop Claude Code
   and Codex already handle by writing their own session files. The agent
   store (the session record and the CLI session id used for resume) is
   written by the runners as they go, so nothing extra is needed for resume.
   Unchanged.
3. New: `forkmesh::processreaper::reapOnQuit()` sweeps whatever is still
   running, and one line ("Quit: stopped N leftover process(es) ...") goes to
   stderr (the journal under systemd) when it found something.

The sweep runs after the window is gone on purpose: every owner has detached
its `finished()` handlers by then, so a reaped Actions step can no longer be
recorded as a failed run (see "Actions runs" below), and an agent's runner
cannot mark its session failed.

## The instance marker

`main()` sets `FORKMESH_DESKTOP_INSTANCE=<128 random bits in hex>` in its own
environment before any thread or child exists (`initializeInstance()`), so
every child inherits it: `QProcess` with the default or a copied
`systemEnvironment()`, `forkpty` + `execvp`, and `systemd-run --scope` (which
runs the command with the caller's environment). Linux shows a process's
initial environment in `/proc/<pid>/environ`, so the marker identifies work
that escaped the process tree with `setsid`, a double fork or `nohup`. A new
value per launch means a quitting desktop never matches a newer instance, even
one it started.

## What the reaper selects

`selectTargets()` works on a snapshot of the process table (`/proc` on Linux,
`ps -axo pid=,ppid=,pgid=,uid=,stat=` on macOS and other Unixes). It never
picks pid 1, the desktop itself, its ancestors (the shell or launcher that
started it), zombies, or any process of another uid. Of the rest it picks:

- every process inside a **registered scope**: a unit this instance created
  (`registerOwnedUnit`, called from `agentresources::contain()` and
  `ActionRunner::launchSandboxedStep`) whose name matches
  `forkmesh-(agent|action)-*.scope`, whatever its environment (`bwrap
  --clearenv` drops the marker);
- in the desktop's **own cgroup** (or a sub-cgroup of it): a process that
  carries this instance's marker, is a descendant of the desktop, or shares a
  process group with such a process (other than the desktop's own group, which
  a terminal launch shares with unrelated jobs).

The cgroup test is the line between "related" and "deliberately detached":
a process that carries the marker but lives in some other systemd unit was put
there on purpose (`systemd-run --user --unit=…` service units do not inherit
the caller's environment unless asked, and they are the documented way to make
work outlive a session), so it is left alone. Without cgroups (macOS) only
descendants and their process groups are found; escaped double forks there
are out of reach.

## Escalation and the deadline

`reap()` (pure; the hooks make it testable without real signals):

1. `SIGTERM` to every target, and `systemctl --user stop --no-block` for the
   live registered scopes (blocking would wait for the scope's 90 s
   `TimeoutStopSec`).
2. Every 100 ms: re-read the table and re-select, so a process forked during
   the teardown gets its own `SIGTERM` and a reused pid is never signalled
   unless it matches again.
3. After the 3 s grace period: one `SIGKILL` per remaining target and one
   `systemctl --user kill --signal=SIGKILL --kill-whom=all` for the remaining
   scopes.
4. At 5 s the reaper stops waiting and lists the survivors (a process in
   uninterruptible sleep cannot die sooner). Quit never hangs on it, and it
   stays inside the 15 s `CrashHandler` alarm armed by a stop signal.

With nothing left over the sweep is one table read and returns at once.

## What a quit stops

| Process | How it is found |
| --- | --- |
| Claude Code / Codex CLIs, their MCP servers, tool shells, builds and tests | registered `forkmesh-agent-*.scope` |
| Legacy `AgentRunner` agents and their git phases | scope, or marker + desktop cgroup |
| `AgentLoginPty` sign-in PTYs, `TerminalWidget` and `ForkMeshTerm` shells and what runs in them | marker or descendant |
| Actions steps (`bwrap` in `forkmesh-action-*.scope`) and the checkout/worktree phases | registered scope; marker |
| Rebuild & restart `cmake`/`ninja`/`git` steps (own session via `setsid`) | marker |
| Git pumps, `QProcess` helpers, log followers (`journalctl`, `ssh` tails), `pw-cat`/`piper` audio | descendant or marker |
| Anything above that double-forked or `nohup`ed itself | marker |

## What a quit deliberately leaves running

| Process | Why |
| --- | --- |
| Fleet operations: `fmnext-add-core-*`, `fmnext-remove-core-*` units, any `fmnext-*`/`fm-*` unit, `forkmesh-node-deploy.service` | Killing a core add, removal or fleet roll half-way leaves a node half-provisioned or a fleet split across releases. They run as their own systemd services so they finish without the desktop; the add-core and remove-core dialogs re-attach to the unit's log (`current.json`) on the next start. |
| User services and timers: source publisher, source guard, mirror renew/fanout/reconcile, release timers, backups, the DNS health loop | Installed to run with or without the desktop. They are separate units, and their prefixes are also on the protected list (`defaultProtectedUnitPrefixes()`), so even a registration bug cannot stop one. |
| Anything an agent or user started with `systemd-run --user --unit=…` | Own unit means "outlive me"; agent long builds and deploy runs use exactly this. |
| Launches that are meant to outlive the desktop | Started with `processreaper::startReleased()` or `releasedEnvironment()`, which drop the marker, and double-forked: the relaunch after Rebuild & restart and Update (including the `sudo -u` relaunch), the `systemctl --user stop/restart` hand-back to the supervising user service (killing it before it reaches systemd would let `Restart=always` reopen the window, or skip a restart), opening another project, VSCodium, the system terminal, a pull preview build, `notify-send`, `ollama rm`, `systemctl start forkmesh-mirror-renew.service`, and the post-uninstall file wipe. |
| The Lima VM for KVM-isolated agents | Shared and slow to boot; `limactl start` from Settings runs with the marker removed and `limactl shell` strips it through the guest environment policy, so the host agent is not related. The `limactl shell` processes themselves are descendants and stop. |
| Other ForkMesh desktops (other projects, test instances) | Different marker. |

The fleet units' environment is built from `releasedEnvironment()` too
(`AddCoreNodeDialog`, `RemoveCoreNodeDialog` pass `FORKMESH_*` variables with
`-E`), so they never carry the marker.

## Actions runs

Semantics are unchanged. A run that is active when the desktop quits stays
Running on disk: after a stop signal `ActionRunner::interruptForShutdown`
logs "INTERRUPTED" and `MainWindow::onRunInterrupted` re-queues it; on a
window close `~ActionRunner` disconnects before it stops the step, and
`restoreActionRuns` re-queues the Running run on the next start
(`requeueAfterDesktopInterruption`). The reaper only makes sure the step's
sandbox and everything in its scope are gone.

## Limits

- A second stop signal, `SIGKILL` or a crash exits without the sweep. Under a
  systemd user service, systemd still kills the service cgroup; registered
  scopes then outlive the desktop until their own `RuntimeMaxSec` (Actions) or
  until their processes exit (agents).
- The add-core/remove-core fallback that runs the script inside the desktop
  when `systemd-run` fails is a desktop child and stops with it, as it did
  before; only the systemd unit path survives a quit.
- A process that clears its environment and leaves the desktop's process tree
  without entering a registered scope is not found.
- Windows: no sweep; the existing per-object stops apply.
