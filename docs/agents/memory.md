# Agent memory and CPU containment (desktop, Linux)

Many agents running at once each start their own builds and test runs. The
desktop ties that work to one shared memory and CPU budget so the desktop,
the compositor and the browser stay responsive.

Code: `desktop/src/AgentResources.{h,cpp}` (launch side),
`desktop/src/AgentResourceGovernor.{h,cpp}` (freeze/thaw and CPU cap loop,
started in `main.cpp`). Test: `forkmesh-agent-resources-tests`
(`desktop/tests/test_agent_resources.cpp`).

## What it does

1. **One slice for all agent work.** At startup, off the GUI thread,
   `prepare()` sets `MemoryHigh`, `MemoryMax` and `CPUWeight=50` on
   `forkmesh.slice` (`systemctl --user set-property --runtime`). The budget
   (`computeBudget()`) keeps a reserve of 20 % of RAM (at least 2 GB, at most
   half). `MemoryHigh` is RAM minus the reserve; `MemoryMax` is half-way into
   the reserve, so the OOM killer picks a process inside the slice, never the
   desktop.
2. **One scope per agent.** Claude Code, Codex and legacy `AgentRunner`
   launches are wrapped in `systemd-run --user --scope
   --slice=forkmesh.slice --unit=forkmesh-agent-<label>-<pid>-<n>.scope`
   (`contain()`), outermost, around the Landlock helper. Each agent is
   throttled above 60 % of the slice and capped at 80 %. `OOMPolicy=continue`
   keeps the CLI running when the kernel kills one of its builds. KVM runs
   are not wrapped.
3. **Agent-started systemd units join the slice.** `prepare()` writes a
   `systemd-run` shim to `$XDG_RUNTIME_DIR/forkmesh-agent-bin/`, and the
   launch command is prefixed with `export PATH=<that dir>:"$PATH";`
   (`wrapShellCommand()`). The shim adds `--slice=forkmesh.slice` to any
   `--user` call that names no slice. `FORKMESH_AGENT_SLICE` names the slice
   for tools that want it. A nested login shell resets `PATH` and bypasses the
   shim.
4. **Actions steps join the slice** (`ActionRunner::launchSandboxedStep`),
   keeping their own per-step limits.
5. **Build caps** (`environmentDefaults()`, only when unset):
   `CMAKE_BUILD_PARALLEL_LEVEL`, `CARGO_BUILD_JOBS`,
   `PYTEST_XDIST_AUTO_NUM_WORKERS` at about one job per 1.5 GB of the
   per-agent throttle (`kBuildJobBytes`), and `GRADLE_OPTS` gains `-Dorg.gradle.workers.max=<jobs>` and
   `-Dorg.gradle.daemon.idletimeout=600000`. `MAKEFLAGS` is left alone.
   Until 2026-10-25 a job was budgeted 3 GB: on a 36-core, 62 GB desktop that
   gave agent builds 9 jobs while the slice allowed 28 cores and nothing was
   throttled. A Qt translation unit peaks near 1 GB (the 47 MB resource file,
   the worst, took 1.3 GB), so 1.5 GB per job doubles agent build parallelism
   (18 jobs there) and still leaves the throttle and freezer below as the
   backstop.
6. **Freeze instead of thrash** (`Governor`, every 5 s). Pressure is the
   larger of the machine's and the slice's memory PSI "full" avg10. At 25 %
   or more, the newest unit in the slice holding at least 256 MB is frozen
   (`systemctl --user freeze`), at most one every 20 s and never the last
   running one. Below 5 % with at least 3 GB available, the first frozen unit
   is thawed, at most one every 15 s. No unit stays frozen longer than 15
   minutes. Agent CLI scopes (`forkmesh-agent-*`) and Actions scopes
   (`forkmesh-action-*`) are never frozen. The desktop thaws what it froze on
   exit and thaws leftovers at startup. Every freeze and thaw is logged
   (`Agent memory: ...`).
7. **CPU cap.** The slice gets `CPUQuota` of the cores minus a reserve
   (`agents/cpuReservePercent`, default 20 %, at least one core, at most
   half). Each tick reads machine busy time from `/proc/stat` and the slice's
   `usage_usec`; the cap becomes the ceiling minus what other programs use,
   never below a quarter of the cores. It is lowered at most every 5 s and
   raised at most every 15 s by at most an eighth of the cores, with a
   hysteresis of 5 % of the cores (`decideCpu()`). Changes are logged as
   `Agent CPU: ...`. CPU pressure never freezes anything.

## Configuration

- Settings > Agents > "Keep agents from running the machine out of memory
  and CPU" (`agents/memoryGovernor`, on by default). Read at startup.
- `agents/memoryReservePercent` (QSettings only, default 20, clamped 5 to 60).
- `agents/cpuReservePercent` (QSettings only, default 20, clamped 5 to 60).

## Diagnostics page

Memory > "Agent governor", Settings > Agents > "Governor diagnostics…", or
"Agent governor" in Go to (`desktop/src/MainWindowGovernor.cpp`). It samples
every 2 s while visible and shows: loop state, memory stall against the
freeze and thaw lines, available memory, slice `memory.current` against its
limits, freezes and thaws, CPU use and cap, every unit in the slice with why
it would or would not be frozen (`nextFreezeCandidate()`), the last 200
events, the settings, live slice counters (`readSliceCounters()`) and build
caps. "Copy report" copies it all as text. Cgroup and PSI files are read on a
worker.

## Requirements and fallbacks

Linux, cgroup v2, `systemd-run` and `systemctl`, a reachable systemd user
manager, and the memory controller delegated to `user@<uid>.service`. The
`io` controller is usually not delegated, so no `IOWeight` is set.
`prepare()` probes the path with a throwaway scope (with and without
`OOMPolicy=continue`). When any step fails, nothing is wrapped and agents
launch as before; the environment caps still apply on every platform.

## Traps

- The slice properties are `--runtime` drop-ins under
  `$XDG_RUNTIME_DIR/systemd/user.control/forkmesh.slice.d/`; they vanish at
  logout and are rewritten on the next start. `systemctl --user revert
  forkmesh.slice` removes them.
- If the desktop is killed with units frozen, they stay frozen until it starts
  again (`systemctl --user thaw <unit>` by hand).
- New agent launch paths should go through `applyEnvironment()`,
  `wrapShellCommand()` and `contain()`. The short `claude -p` title and
  Auto-model triage calls are not wrapped.
