# Agent memory and CPU containment (desktop, Linux)

Many agents running at once each start their own C++ builds, Gradle/Java
builds and Python test runs. Until 2026-09-29 nothing tied that work to a
shared budget: on 2026-09-29 the box (31 GB, 36 cores) had 8.6 GB in 14
`cc1plus`, 6.8 GB in three Gradle daemons, 3.9 GB in 27 `python3`, 11 GB of
swap in use, and memory PSI "full" avg10 at 45 % (nothing made progress for
almost half of every ten seconds). Eight of the ten heaviest cgroups were
`systemd-run --user` units the agents had started themselves (`fm784-build.service`
and so on), so they sat in `app.slice` next to the desktop and GNOME, and the
kernel had no way to prefer the UI over them.

Code: `desktop/src/AgentResources.{h,cpp}` (launch side),
`desktop/src/AgentResourceGovernor.{h,cpp}` (freeze/thaw and CPU cap loop, started in
`main.cpp`). Test: `forkmesh-agent-resources-tests`
(`desktop/tests/test_agent_resources.cpp`).

## What it does

1. **One slice for all agent work.** At startup, off the GUI thread,
   `prepare()` runs `systemctl --user set-property --runtime forkmesh.slice`
   with `MemoryHigh`, `MemoryMax` and `CPUWeight=50`. The budget
   (`computeBudget()`) keeps a reserve for the desktop and everything else:
   20 % of RAM, at least 2 GB, at most half. `MemoryHigh` is RAM minus the
   reserve; `MemoryMax` is half-way into the reserve. Past `MemoryHigh` the
   kernel reclaims and slows down only the processes in the slice, so the
   desktop, the compositor and the browser stay responsive instead of the whole
   machine paging. `MemoryMax` makes the OOM killer pick a process inside the
   slice, never the desktop. `CPUWeight=50` against the default 100 gives the
   UI the CPU when both want it. On the 31 GB box: throttled above 24.8 GB,
   hard limit 27.9 GB, 6.2 GB kept back.
2. **One scope per agent.** Claude Code, Codex and legacy `AgentRunner`
   launches are wrapped in `systemd-run --user --scope --slice=forkmesh.slice
   --unit=forkmesh-agent-<label>-<pid>-<n>.scope` (`contain()`), outermost,
   around the Landlock helper. Each agent is throttled above 60 % of the
   slice and capped at 80 %, so one runaway agent cannot take the whole
   budget. `OOMPolicy=continue` keeps the agent CLI running when the kernel
   kills one of its builds; the build fails and the agent sees the failure
   rather than the session dying. JVMs size their default heap from the
   scope's limit. KVM runs are not wrapped (the VM has its own memory size).
   `systemctl --user status forkmesh-agent-claude-788-*` shows an agent's
   whole process tree and its memory.
3. **Agent-started systemd units join the slice.** `prepare()` writes a
   `systemd-run` shim to `$XDG_RUNTIME_DIR/forkmesh-agent-bin/`, and the
   launch shell command gets `export PATH=<that dir>:"$PATH";` in front
   (`wrapShellCommand()`; it has to be inside the command because `bash -lc`
   runs `/etc/profile`, which resets `PATH` on Debian). The shim adds
   `--slice=forkmesh.slice` to any `--user` call that names no slice
   (`--slice`, `--slice-inherit`, `-p Slice=`) and passes everything else
   through. Folder-locked agents get the shim directory as a read-only root.
   `FORKMESH_AGENT_SLICE` names the slice for tools that want it explicitly.
   A nested login shell (`bash -l`) inside the agent resets `PATH` again and
   bypasses the shim.
4. **Actions steps join the slice** (`ActionRunner::launchSandboxedStep`),
   keeping their own per-step limits.
5. **Build caps in the agent environment** (`environmentDefaults()`, only
   when the variable is not already set): `CMAKE_BUILD_PARALLEL_LEVEL`,
   `CARGO_BUILD_JOBS`, `PYTEST_XDIST_AUTO_NUM_WORKERS` at about one job per
   3 GB of the per-agent throttle (4 on the 31 GB box), and `GRADLE_OPTS`
   gains `-Dorg.gradle.workers.max=<jobs>` and
   `-Dorg.gradle.daemon.idletimeout=600000`, so Gradle daemons exit after 10
   idle minutes instead of 3 hours. `MAKEFLAGS` is left alone on purpose:
   plain `make` is serial, and a `-j` would raise its memory, not lower it.
   The desktop's own C++ builds are gated machine-wide by the compile gate
   ([../desktop/build-and-test.md](../desktop/build-and-test.md#machine-wide-compile-gate)).
6. **Freeze instead of thrash** (`Governor`, every 5 s). Pressure is the
   larger of the machine's and the slice's memory PSI "full" avg10. At 25 % or
   more, the newest unit in the slice that holds at least 256 MB is frozen
   with `systemctl --user freeze`, at most one every 20 s, and never the last
   running one. Below 5 %, with at least 3 GB available, the first frozen
   unit is thawed, at most one every 15 s. No unit stays frozen longer than 15
   minutes; after that forced thaw it counts as the oldest, so a different
   unit is picked next. Frozen processes stop touching memory, so the kernel
   can page them out once and let the rest finish, instead of every build
   paging against every other. Only agent-started units are frozen: agent CLI
   scopes (`forkmesh-agent-*`) and Actions scopes (`forkmesh-action-*`) are
   never frozen, because a frozen CLI would miss its API stream and its tool
   timeouts. The desktop thaws what it froze when it exits, and at startup
   thaws any unit left frozen in the slice by a desktop that crashed. Every
   freeze and thaw is a line in the app log (`Agent memory: ...`), and the
   startup line describes the budget.

7. **CPU cap** (added 2026-10-07). With memory under control the box sat
   near 100 % CPU: on 2026-10-07 load was 33 on 36 cores, 80 % user time,
   about 25 cores in `forkmesh.slice` (14 `cc1plus`, a headless Chrome at
   8 cores, two pytest runs, a KVM guest). `CPUWeight=50` only decides who
   waits once every core is taken, so the desktop and the browser still
   queued behind builds. The slice now also gets `CPUQuota`: the cores minus
   a reserve (`agents/cpuReservePercent`, default 20 %, at least one core,
   at most half), so 28.8 of 36 cores. That is the ceiling. Every governor
   tick reads the machine's busy time from `/proc/stat` and the slice's
   `usage_usec` from `cpu.stat`; what is busy outside the slice is "other
   programs" (the desktop, GNOME, the browser), and the cap becomes the
   ceiling minus that, never below a quarter of the cores (9 on 36). The
   other-programs figure rises at once and decays by half each tick, so a
   burst lowers the cap straight away and it comes back slowly. The cap is
   lowered at most every 5 s and raised at most every 15 s by at most an
   eighth of the cores, and only moves when the change is at least 5 % of
   the cores (one core minimum), so it does not chatter
   (`decideCpu()`). Changes go through `systemctl --user set-property
   --runtime forkmesh.slice CPUQuota=N%`, are logged as `Agent CPU: ...`
   and appear as "cpu cap" in the Activity list. The desktop puts the
   ceiling back when it exits; the next start writes it again anyway.
   Units inside the slice share the capped CPU by their equal default
   weights, so no per-agent CPU cap is set. Memory freezing is unchanged;
   CPU pressure never freezes anything, since a CPU-bound build paused does
   not help the others finish.

## Configuration

- Settings > Agents > "Keep agents from running the machine out of memory"
  (`agents/memoryGovernor`, on by default). Read at startup, so it applies
  after a restart; the tooltip shows the live budget or why containment is
  off.
- Settings > Agents > "Keep agents from running the machine out of memory
  and CPU" is the same switch; it covers the CPU cap too.
- `agents/memoryReservePercent` (QSettings only, default 20, clamped 5 to 60).
- `agents/cpuReservePercent` (QSettings only, default 20, clamped 5 to 60):
  the share of cores agents never get.

## Diagnostics page

Memory > "Agent governor", Settings > Agents > "Governor diagnostics…", or
"Agent governor" in Go to (`kGovernorSectionIndex`,
`desktop/src/MainWindowGovernor.cpp`). It samples every 2 s while it is on
screen and stops when it is left, like the Memory page.

- The header says whether the loop is running and since when, and whether the
  saved switch differs from the one the desktop started with (it applies only
  after a restart). Under it is the same `describe()` line as the startup log.
- Tiles: the memory stall the governor acts on (the larger of machine and
  slice PSI "full" avg10) against the freeze and thaw lines, each of the two
  inputs, available memory, the slice's `memory.current` against its
  `memory.high`/`memory.max`, units in the slice, freezes and thaws since
  start, how long ago the last check ran, and four CPU tiles: the machine's
  busy share, the cores agents used against other programs, the current
  cap against its floor and ceiling, and how often the slice hit the cap
  (`nr_throttled`, `throttled_usec`).
- Units: every service and scope in the slice as the governor last read it,
  with its memory, the cores it used between the last two checks, how long it has been seen and frozen, and why it would or
  would not be frozen. `nextFreezeCandidate()` is the same pick `decide()`
  makes, so the row marked "Next to freeze" is the one a stall would pause.
- Activity: the last 200 start, freeze and thaw events, including systemctl
  failures (`Governor::record()`), newest first.
- Settings: both QSettings keys, the computed budget, every policy threshold,
  the slice cgroup's live counters (`readSliceCounters()`: current, peak,
  swap, high, max, PSI and the `memory.events` high/max/oom/oom_kill counts,
  plus `cpu.max`, `cpu.stat` and CPU PSI), the CPU budget and cap policy,
  the systemd tools and shim folder, and the build caps agents get.
- "Copy report" puts all of it on the clipboard as plain text.

The page reads the governor through `governorSnapshot()`, which falls back to
`status()` when the loop never started (setting off or containment
unavailable). Cgroup and PSI files are read on a worker.

## Requirements and fallbacks

Linux, cgroup v2, `systemd-run` and `systemctl`, a reachable systemd user
manager, and the memory controller delegated to `user@<uid>.service` (the
default since systemd 252-ish; Debian 13 delegates `cpu memory pids`). The
`io` controller is not delegated there, so no `IOWeight` is set. `prepare()`
probes the whole path by starting a throwaway scope running `true`, first with
`OOMPolicy=continue` and again without it for older systemd. When any step
fails, nothing is wrapped and agents launch exactly as before; the environment
caps still apply wherever `prepare()` ran with the setting on (macOS and
Windows included).

## Traps

- The slice properties are `--runtime` drop-ins under
  `$XDG_RUNTIME_DIR/systemd/user.control/forkmesh.slice.d/`; they vanish at
  logout or reboot and the next desktop start writes them again.
  `systemctl --user revert forkmesh.slice` removes them by hand.
- If the desktop is killed while it has units frozen, those units stay frozen
  until the desktop starts again (`systemctl --user thaw <unit>` by hand).
- New agent launch paths should go through `applyEnvironment()`,
  `wrapShellCommand()` and `contain()` like the three existing ones. The short
  `claude -p` title and Auto-model triage calls are not wrapped.
