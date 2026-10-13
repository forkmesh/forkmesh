#!/usr/bin/env python3
"""Measure ForkMesh desktop startup end to end: exec() -> first window paint.

The desktop client already traces its own startup (``desktop/src/StartupTrace.h``
emits ``[startup +NNNNms] <message>``), but nothing ever *read* that trace, so a
startup that drifted from ~1.4s to ~3.0s was only ever noticed by feel. This
harness launches an already-built binary, reads its own trace back, and turns it
into a number with a budget attached.

What it measures
----------------
Wall time from process ``exec()`` to the ``first main-window paint`` trace line.
That interval covers everything a user waits through: QApplication construction,
Qt plugin and font-database init, single-instance acquisition, the temporary
mirror-directory sweep, settings parsing, MainWindow construction and the first
``show()``. The in-process budget in ``desktop/tests/test_perf_budgets.cpp``
historically measured only the MainWindow constructor, which is why it stayed
green while real startups took three seconds.

Where the trace comes from
--------------------------
Two sources, both on the same clock, merged here:

* **stderr** carries every trace line up to the point where ``main()`` installs
  the application log sink (``setAppLogSink(..., keepConsoleEcho=false)``).
* **the profile's ``network_log.txt``** carries everything from that point on.

Which side of that swap the marker falls on is a detail of ``main()`` that has
already moved once, so both are read and both are watched. Historically the
canonical marker was emitted *after* the sink swap and never reached the
console at all, so a stderr-only harness would have measured nothing.

Isolation (why this is safe to run on a box with a live node)
-------------------------------------------------------------
``acquireSingleInstance()`` locks ``$HOME/.forkmesh/forkmesh.lock`` and calls
``QLocalServer::removeServer("forkmesh-single-instance")``, and startup sweeps
``QDir::tempPath()`` for stale ForkMesh mirror directories -- so an
under-isolated test launch would hijack (or delete state belonging to) a running
production node. Isolation is therefore *built into* :func:`run_once`: the child
always gets a private ``HOME``/``XDG_*``/``TMPDIR`` under a throwaway scratch
directory, a curated environment (no ``DISPLAY``, no ``DBUS_SESSION_BUS_ADDRESS``,
no ``SSH_AUTH_SOCK``), ``QT_QPA_PLATFORM=offscreen``, a hard deadline, and a
guaranteed process-group kill in a ``finally`` block. There is deliberately no
flag that turns any of that off.

Profiles
--------
``empty``      a brand-new profile: the optimistic floor.
``realistic``  a copy of the real ``ForkMesh.conf`` with every secret-looking
               value replaced by a filler of exactly the same length, so
               settings *parse* cost is reproduced byte for byte while no live
               credential is ever handed to a throwaway instance. The "pat"
               pattern also catches ``localPath``/``mirrorPath``, which is a
               feature: it keeps the throwaway instance away from real
               repositories. The trade-off is that per-repository git work is
               under-represented in this profile, so treat it as "settings
               parse cost included, repository I/O excluded".

Usage::

    tools/measure_startup.py                       # both profiles, 3 runs each
    tools/measure_startup.py --runs 5 --profile realistic
    tools/measure_startup.py --binary /path/to/forkmesh --budget-ms 1500
    tools/measure_startup.py --json /tmp/startup.json
    tools/measure_startup.py --history             # launch nothing; read the
                                                   # startups this machine has
                                                   # already recorded

``--history`` is the cheapest useful call: the app has been writing its own
startup time to ``network_log.txt`` on every launch all along, and nothing had
ever read it back. It needs no binary and starts no process.

Exit status: 0 within budget, 1 over budget, 2 could not measure.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import signal
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BINARY = ROOT / "desktop" / "build" / "forkmesh"
sys.path.insert(0, str(Path(__file__).resolve().parent))
import forkmesh_paths  # noqa: E402  (sibling module; tools run as scripts)
import forkmesh_safe_delete  # noqa: E402

DEFAULT_SOURCE_CONF = forkmesh_paths.resolve().settings_file
DEFAULT_SCRATCH_ROOT = Path.home() / ".cache" / "forkmesh-startup-bench"
DEFAULT_BUDGET_MS = 500
BUDGET_ENV = "FORKMESH_STARTUP_BUDGET_MS"

# The line MainWindow emits from its first paint. The window and loading shell
# are visible by the time it is written.
EVENT_LOOP_MARKER = "first main-window paint"

# "[startup + 79ms] first main-window paint"
TRACE_RE = re.compile(r"\[startup \+\s*(\d+)ms\]\s?(.*)$")
# network_log.txt wraps each line: "2026-08-19 20:23:48  <message>  [file.cpp:26]"
LOG_PREFIX_RE = re.compile(r"^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\s+")
LOG_SUFFIX_RE = re.compile(r"\s+\[[^\]\s]+:\d+\]$")

# Keys whose values must never be handed to a throwaway instance. Substring
# match against the lower-cased QSettings key (which includes its "\"-joined
# sub-path). Deliberately blunt: over-redaction is free here because the filler
# preserves the value's length, and therefore its parse cost.
SECRET_KEY_PATTERNS = (
    "token",
    "secret",
    "key",
    "password",
    "pat",
    "apikey",
    "cookie",
    "session",
)

# Qt writes typed values as @Type(payload); keep the wrapper so the file still
# parses as the same type after redaction.
TYPED_VALUE_RE = re.compile(r"^@([A-Za-z]+)\((.*)\)$", re.DOTALL)


class MeasurementError(RuntimeError):
    """The harness could not produce a trustworthy number."""


# --------------------------------------------------------------------------
# Settings redaction
# --------------------------------------------------------------------------


def redact_value(value: str) -> str:
    """Return a same-length filler for *value*, preserving its ini shape."""
    if not value:
        return value
    typed = TYPED_VALUE_RE.match(value)
    if typed:
        return f"@{typed.group(1)}({'x' * len(typed.group(2))})"
    if len(value) >= 2 and value.startswith('"') and value.endswith('"'):
        return '"' + "x" * (len(value) - 2) + '"'
    return "x" * len(value)


def key_is_secret(key: str) -> bool:
    lowered = key.lower()
    return any(pattern in lowered for pattern in SECRET_KEY_PATTERNS)


def redact_conf(text: str) -> tuple[str, list[str]]:
    """Redact secret-looking values in a QSettings ini.

    Returns the redacted text and the list of redacted keys. The result has the
    same length as the input, line for line, so the cost of *parsing* it is
    unchanged -- that is the whole point of the "realistic" profile.
    """
    redacted: list[str] = []
    out: list[str] = []
    section = ""
    for line in text.splitlines(keepends=True):
        stripped = line.rstrip("\r\n")
        newline = line[len(stripped) :]
        if stripped.startswith("[") and stripped.endswith("]"):
            section = stripped[1:-1]
            out.append(line)
            continue
        head, sep, value = stripped.partition("=")
        if not sep:
            out.append(line)
            continue
        if key_is_secret(head):
            redacted.append(f"{section}/{head}" if section else head)
            out.append(head + "=" + redact_value(value) + newline)
        else:
            out.append(line)
    return "".join(out), redacted


# --------------------------------------------------------------------------
# Scratch directories and child environment
# --------------------------------------------------------------------------


def filesystem_type(path: Path) -> str:
    """Best-effort fstype for the mount that contains *path*."""
    try:
        mounts = Path("/proc/mounts").read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""
    best_point = ""
    best_type = ""
    target = str(path)
    for entry in mounts.splitlines():
        parts = entry.split()
        if len(parts) < 3:
            continue
        point, fstype = parts[1], parts[2]
        if target == point or target.startswith(point.rstrip("/") + "/"):
            if len(point) >= len(best_point):
                best_point, best_type = point, fstype
    return best_type


def resolve_scratch_root(requested: str | None) -> Path:
    """Pick and validate the scratch root.

    A ForkMesh profile plus a materialized mirror can be larger than a RAM-backed
    /tmp (this box's /tmp is a 16G shared tmpfs with a few gigabytes free and
    other agents building in it), so refuse to run there.
    """
    root = Path(requested).expanduser() if requested else DEFAULT_SCRATCH_ROOT
    root = root.resolve()
    existing = root
    while not existing.exists() and existing != existing.parent:
        existing = existing.parent
    fstype = filesystem_type(existing)
    if fstype in {"tmpfs", "ramfs"} or str(root) == "/tmp" or str(root).startswith(
        ("/tmp/", "/dev/shm", "/run/")
    ):
        raise MeasurementError(
            f"scratch root {root} is on a RAM-backed filesystem ({fstype or 'unknown'}); "
            "pass --scratch-root with a directory under /home"
        )
    real_home = Path.home().resolve()
    if root == real_home or root in real_home.parents:
        raise MeasurementError(
            f"scratch root {root} is or contains the real home directory; refusing to run"
        )
    root.mkdir(parents=True, exist_ok=True)
    usage = shutil.disk_usage(root)
    if usage.free < 512 * 1024 * 1024:
        raise MeasurementError(
            f"scratch root {root} has only {usage.free // (1024 * 1024)}MiB free"
        )
    return root


def make_scratch(root: Path, label: str, index: int) -> Path:
    scratch = root / f"{label}-{index:02d}-{os.getpid()}"
    if scratch.exists():
        forkmesh_safe_delete.remove_tree(scratch, root)
    for name in ("home", "data", "config", "cache", "run", "tmp"):
        (scratch / name).mkdir(parents=True)
    # XDG_RUNTIME_DIR must be private or Qt complains and falls back.
    (scratch / "run").chmod(0o700)
    return scratch


def child_env(scratch: Path) -> dict[str, str]:
    """The only environment a measured launch ever gets.

    Nothing here is caller-supplied. Every path that the app can write to, take
    a lock in, or sweep is inside *scratch*, and the session-bus / X11 / ssh-agent
    handles of the surrounding desktop session are deliberately absent so a
    throwaway instance cannot reach the real session at all.
    """
    return {
        "PATH": os.environ.get("PATH", "/usr/local/bin:/usr/bin:/bin"),
        "USER": os.environ.get("USER", "forkmesh"),
        "LOGNAME": os.environ.get("LOGNAME", os.environ.get("USER", "forkmesh")),
        # Real desktop sessions have a UTF-8 locale; without one Qt spends part
        # of startup switching, which would not reflect a real launch.
        "LANG": "C.UTF-8",
        "LC_ALL": "C.UTF-8",
        "HOME": str(scratch / "home"),
        "XDG_DATA_HOME": str(scratch / "data"),
        "XDG_CONFIG_HOME": str(scratch / "config"),
        "XDG_CACHE_HOME": str(scratch / "cache"),
        "XDG_RUNTIME_DIR": str(scratch / "run"),
        "TMPDIR": str(scratch / "tmp"),
        "QT_QPA_PLATFORM": "offscreen",
        # Never write into the developer's dconf database.
        "GSETTINGS_BACKEND": "memory",
    }


# --------------------------------------------------------------------------
# Trace parsing
# --------------------------------------------------------------------------


@dataclass
class TraceLine:
    ms: int
    message: str


@dataclass
class RunResult:
    profile: str
    index: int
    total_ms: int | None
    timeline: list[TraceLine] = field(default_factory=list)
    exit_code: int | None = None
    note: str = ""
    load_average: float = 0.0

    @property
    def ok(self) -> bool:
        return self.total_ms is not None


def load_average() -> float:
    """One-minute load average, or 0 where it is unavailable.

    Startup timing is dominated by CPU and page-cache behaviour, so a number
    measured on a box that is simultaneously running a parallel C++ build is not
    comparable with one measured on an idle box. Recording the load with every
    run keeps that visible instead of silently poisoning a baseline.
    """
    try:
        return os.getloadavg()[0]
    except (OSError, AttributeError):
        return 0.0


def strip_log_decoration(line: str) -> str:
    line = LOG_PREFIX_RE.sub("", line.rstrip("\n"))
    return LOG_SUFFIX_RE.sub("", line)


def parse_trace(raw_lines: list[str]) -> list[TraceLine]:
    """Turn raw output into one monotonic timeline.

    main() runs *two* elapsed clocks and prints both with the same
    ``[startup +NNNNms]`` prefix: the process-wide one from
    ``beginStartupTrace()`` and a short-lived one restarted just before the
    MainWindow constructor (which is where "MainWindow constructed" and "window
    shown; entering event loop" come from). Their numbers interleave, so keying
    on the prefix alone silently mixes the two. Only the process-wide clock is
    monotonic across the whole run, so any timestamp that goes backwards belongs
    to the secondary clock and is dropped here.

    The secondary clock starts later, so it always reports a *smaller* elapsed
    than the process clock does at the same instant. A secondary line can
    therefore never push the high-water mark past a real one, which means it can
    never cost us a later line or the marker; the worst it can do is leave one
    extra row in the interval breakdown when no process-clock line happened to
    be logged in between.
    """
    seen: set[tuple[int, str]] = set()
    timeline: list[TraceLine] = []
    high_water = -1
    for raw in raw_lines:
        match = TRACE_RE.search(strip_log_decoration(raw))
        if not match:
            continue
        ms = int(match.group(1))
        message = match.group(2).rstrip()
        if (ms, message) in seen:
            continue
        seen.add((ms, message))
        if ms < high_water:
            continue  # secondary (MainWindow-relative) clock
        high_water = ms
        timeline.append(TraceLine(ms, message))
    return timeline


def marker_ms(timeline: list[TraceLine]) -> int | None:
    """The last first-main-window-paint timestamp in the trace.

    Both stderr and the application log are merged and can contain the same
    event, so keeping the last de-duplicated marker is the stable rule.
    """
    found: int | None = None
    for line in timeline:
        if line.message.startswith(EVENT_LOOP_MARKER):
            found = line.ms
    if found is not None:
        return found
    for line in timeline:
        if EVENT_LOOP_MARKER in line.message:
            found = line.ms
    return found


def find_network_log(data_home: Path) -> Path | None:
    direct = data_home / "ForkMesh" / "ForkMesh" / "network_log.txt"
    if direct.exists():
        return direct
    for candidate in data_home.rglob("network_log.txt"):
        return candidate
    return None


# --------------------------------------------------------------------------
# One measured launch
# --------------------------------------------------------------------------


def kill_group(process: subprocess.Popen, sig: int) -> None:
    try:
        os.killpg(os.getpgid(process.pid), sig)
    except (ProcessLookupError, PermissionError):
        pass


def run_once(
    binary: Path,
    scratch: Path,
    profile: str,
    index: int,
    conf_text: str | None,
    timeout_s: float,
    settle_ms: int,
) -> RunResult:
    """Launch *binary* once, fully isolated, and read its startup trace back."""
    env = child_env(scratch)
    data_home = Path(env["XDG_DATA_HOME"])
    if conf_text is not None:
        conf_dir = Path(env["XDG_CONFIG_HOME"]) / "ForkMesh"
        conf_dir.mkdir(parents=True, exist_ok=True)
        (conf_dir / "ForkMesh.conf").write_text(conf_text, encoding="utf-8")

    stderr_path = scratch / "stderr.txt"
    stdout_path = scratch / "stdout.txt"
    result = RunResult(profile=profile, index=index, total_ms=None,
                       load_average=load_average())
    deadline = time.monotonic() + timeout_s

    with stdout_path.open("wb") as out, stderr_path.open("wb") as err:
        process = subprocess.Popen(  # noqa: S603 - fixed argv, curated env
            [str(binary)],
            cwd=str(scratch / "home"),
            env=env,
            stdin=subprocess.DEVNULL,
            stdout=out,
            stderr=err,
            start_new_session=True,  # own process group: the kill below is total
        )
        try:
            log_path: Path | None = None
            saw_marker = False
            while time.monotonic() < deadline:
                if log_path is None:
                    log_path = find_network_log(data_home)
                # Either source is enough to stop: the marker reaches the
                # console only while the default message handler is still
                # installed, and the profile log only once the app log sink
                # has taken over. Waiting on one specific source would hang for
                # the whole deadline whenever main() moves the sink swap.
                for candidate in (log_path, stderr_path):
                    if candidate is None:
                        continue
                    try:
                        if EVENT_LOOP_MARKER in candidate.read_text(
                            encoding="utf-8", errors="replace"
                        ):
                            saw_marker = True
                    except OSError:
                        pass
                if saw_marker:
                    break
                if process.poll() is not None:
                    result.note = f"process exited early (code {process.returncode})"
                    break
                time.sleep(0.025)
            if saw_marker:
                # Let the queued log write flush. The first-frame marker is
                # already fixed; post-paint hydration may begin during this
                # brief settle window and adds useful trace context without
                # changing the measured total.
                time.sleep(max(settle_ms, 0) / 1000.0)
            elif process.poll() is None:
                result.note = result.note or f"timed out after {timeout_s:.0f}s"
        finally:
            # A measured instance must never outlive the harness. Signal the
            # whole process group (start_new_session put the child and anything
            # it spawned in its own group) and only ever while the group leader
            # is still unreaped -- after reaping, its pid can be recycled and a
            # killpg would hit an unrelated process on this box.
            if process.poll() is None:
                kill_group(process, signal.SIGTERM)
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    kill_group(process, signal.SIGKILL)
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        pass
            result.exit_code = process.returncode

    raw: list[str] = []
    raw.extend(stderr_path.read_text(encoding="utf-8", errors="replace").splitlines())
    raw.extend(stdout_path.read_text(encoding="utf-8", errors="replace").splitlines())
    log_path = find_network_log(data_home)
    if log_path is not None:
        raw.extend(log_path.read_text(encoding="utf-8", errors="replace").splitlines())
    result.timeline = parse_trace(raw)
    result.total_ms = marker_ms(result.timeline)
    if result.total_ms is None and not result.note:
        result.note = "no 'first main-window paint' marker in the trace"
    return result


# --------------------------------------------------------------------------
# Reporting
# --------------------------------------------------------------------------


@dataclass
class ProfileSummary:
    profile: str
    runs: list[RunResult]
    note: str = ""

    @property
    def totals(self) -> list[int]:
        return sorted(r.total_ms for r in self.runs if r.total_ms is not None)

    @property
    def median_ms(self) -> int | None:
        totals = self.totals
        return int(round(statistics.median(totals))) if totals else None

    def representative(self) -> RunResult | None:
        median = self.median_ms
        if median is None:
            return None
        ok = [r for r in self.runs if r.total_ms is not None]
        return min(ok, key=lambda r: abs(r.total_ms - median))


def slowest_intervals(run: RunResult, limit: int) -> list[tuple[int, int, str]]:
    """(delta, end_ms, message) for the gaps between consecutive trace lines."""
    intervals: list[tuple[int, int, str]] = []
    previous = 0
    for line in run.timeline:
        intervals.append((line.ms - previous, line.ms, line.message))
        previous = line.ms
        if line.message.startswith(EVENT_LOOP_MARKER):
            break
    intervals.sort(key=lambda item: item[0], reverse=True)
    return intervals[:limit]


def print_summary_table(summaries: list[ProfileSummary], budget_ms: int) -> None:
    header = (f"{'profile':<12}{'runs':>5}{'min':>10}{'median':>10}"
              f"{'max':>10}{'budget':>10}  verdict")
    print()
    print(header)
    print("-" * len(header))
    for summary in summaries:
        totals = summary.totals
        if not totals:
            print(f"{summary.profile:<12}{len(summary.runs):>5}"
                  f"{'-':>10}{'-':>10}{'-':>10}{budget_ms:>10}  NO DATA")
            continue
        median = summary.median_ms or 0
        verdict = "PASS" if median <= budget_ms else "OVER BUDGET"
        print(f"{summary.profile:<12}{len(totals):>5}{totals[0]:>10}"
              f"{median:>10}{totals[-1]:>10}{budget_ms:>10}  {verdict}")
    print("-" * len(header))
    print("all figures are milliseconds from exec() to first main-window paint")


def print_breakdown(summary: ProfileSummary, limit: int) -> None:
    run = summary.representative()
    if run is None:
        return
    print()
    print(f"[{summary.profile}] slowest startup intervals "
          f"(representative run #{run.index}, total {run.total_ms}ms)")
    print(f"  {'delta':>7}  {'at':>7}  step")
    for delta, end_ms, message in slowest_intervals(run, limit):
        print(f"  {delta:>6}ms  {end_ms:>6}ms  {message[:96]}")


def print_run_details(summary: ProfileSummary) -> None:
    for run in summary.runs:
        total = f"{run.total_ms}ms" if run.total_ms is not None else "FAILED"
        note = f"  ({run.note})" if run.note else ""
        print(f"  {summary.profile} run #{run.index}: {total}"
              f"  [{len(run.timeline)} trace lines, load {run.load_average:.2f}]{note}")


# --------------------------------------------------------------------------
# Entry point
# --------------------------------------------------------------------------


# A log-file marker line: "2026-08-19 20:14:11  [startup + 3045ms] entering ..."
HISTORY_RE = re.compile(
    r"^(\d{4}-\d{2}-\d{2}) (\d{2}:\d{2}:\d{2})\s+\[startup \+\s*(\d+)ms\]\s*"
    + re.escape(EVENT_LOOP_MARKER)
)


def startup_history(data_home: Path) -> list[tuple[str, str, int]]:
    """Every startup this profile has already recorded, oldest file first.

    The app has been writing its own startup time to network_log.txt (and the
    rotated network_log.d/netlog-*.txt) all along -- nothing ever read it back.
    This is that read: real launches, on a real profile, on real hardware, which
    is evidence no synthetic harness run can replace.
    """
    files = sorted((data_home / "network_log.d").glob("netlog-*.txt"))
    files.append(data_home / "network_log.txt")
    found: list[tuple[str, str, int]] = []
    for path in files:
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for line in text.splitlines():
            match = HISTORY_RE.match(line)
            if not match:
                continue
            day, clock, ms = match.group(1), match.group(2), int(match.group(3))
            # main() can log two markers per launch (one before the app log sink
            # takes qInfo() over, one after). Keep the later of any pair that
            # lands in the same second.
            if found and found[-1][0] == day and found[-1][1] == clock:
                found[-1] = (day, clock, max(found[-1][2], ms))
                continue
            found.append((day, clock, ms))
    return found


def report_history(data_home: Path, budget_ms: int) -> int:
    history = startup_history(data_home)
    print(f"startup history from {data_home}")
    if not history:
        print("  no recorded startups found "
              "(no network_log.txt / network_log.d in this profile)")
        return 2
    values = sorted(entry[2] for entry in history)

    def pct(fraction: float) -> int:
        return values[min(len(values) - 1, int(len(values) * fraction))]

    over = sum(1 for v in values if v > budget_ms)
    print(f"  {len(values)} recorded launch(es), "
          f"{history[0][0]} .. {history[-1][0]}")
    print(f"  min {values[0]}ms  median {int(statistics.median(values))}ms  "
          f"p90 {pct(0.9)}ms  max {values[-1]}ms")
    print(f"  over the {budget_ms}ms budget: {over}/{len(values)} "
          f"({100 * over // len(values)}%)")
    print()
    header = f"  {'day':<12}{'n':>4}{'min':>8}{'median':>8}{'max':>8}"
    print(header)
    print("  " + "-" * (len(header) - 2))
    for day in sorted({entry[0] for entry in history}):
        per_day = sorted(entry[2] for entry in history if entry[0] == day)
        print(f"  {day:<12}{len(per_day):>4}{per_day[0]:>8}"
              f"{int(statistics.median(per_day)):>8}{per_day[-1]:>8}")
    median = int(statistics.median(values))
    if median > budget_ms:
        print(f"\nFAIL: real recorded startups have a median of {median}ms, "
              f"over the {budget_ms}ms budget", file=sys.stderr)
        return 1
    print(f"\nPASS: real recorded startups have a median of {median}ms, "
          f"within {budget_ms}ms")
    return 0


def load_profile_conf(profile: str, source_conf: Path) -> tuple[str | None, str]:
    """Return (conf text or None for an empty profile, human note)."""
    if profile == "empty":
        return None, "brand-new profile, no settings file"
    if not source_conf.exists():
        raise MeasurementError(
            f"realistic profile needs a settings file; {source_conf} does not exist "
            "(pass --source-conf, or use --profile empty)"
        )
    original = source_conf.read_text(encoding="utf-8", errors="replace")
    redacted, keys = redact_conf(original)
    if len(redacted) != len(original):
        raise MeasurementError(
            "redaction changed the settings file length; parse cost would no "
            "longer be representative"
        )
    return redacted, (
        f"copy of {source_conf} ({len(original)} bytes, {len(keys)} "
        f"secret-looking value(s) replaced by same-length filler)"
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Measure ForkMesh desktop startup (exec -> first paint).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Every launch is fully isolated; there is no flag to disable that.",
    )
    parser.add_argument(
        "--binary", default=str(DEFAULT_BINARY),
        help=f"already-built forkmesh binary (default: {DEFAULT_BINARY})")
    parser.add_argument(
        "--runs", type=int, default=3,
        help="launches per profile; single runs are noisy (default: 3)")
    parser.add_argument(
        "--profile", choices=("empty", "realistic", "both"), default="both",
        help="settings profile to measure (default: both)")
    parser.add_argument(
        "--budget-ms", type=int,
        default=int(os.environ.get(BUDGET_ENV) or DEFAULT_BUDGET_MS),
        help=f"median must stay at or under this (default: {DEFAULT_BUDGET_MS}, "
             f"env {BUDGET_ENV})")
    parser.add_argument(
        "--source-conf", default=str(DEFAULT_SOURCE_CONF),
        help=f"settings file the realistic profile copies (default: {DEFAULT_SOURCE_CONF})")
    parser.add_argument(
        "--scratch-root", default=None,
        help=f"where throwaway profiles live, never /tmp (default: {DEFAULT_SCRATCH_ROOT})")
    parser.add_argument(
        "--timeout-seconds", type=float, default=120.0,
        help="hard deadline per launch (default: 120)")
    parser.add_argument(
        "--settle-ms", type=int, default=150,
        help="pause after the marker so the queued log write lands (default: 150)")
    parser.add_argument(
        "--breakdown", type=int, default=12,
        help="how many slowest intervals to print per profile (default: 12)")
    parser.add_argument(
        "--json", default=None, help="also write the measurements to this file")
    parser.add_argument(
        "--history", action="store_true",
        help="do not launch anything; summarise the startups already recorded "
             "in this machine's own ForkMesh logs and check them against the "
             "budget")
    parser.add_argument(
        "--history-data-home",
        default=str(forkmesh_paths.resolve().data),
        help="profile directory --history reads (default: this user's)")
    parser.add_argument(
        "--keep-scratch", action="store_true",
        help="keep the throwaway profiles for inspection")
    args = parser.parse_args(argv)

    if args.history:
        # Read-only: launches nothing, so none of the isolation below applies.
        return report_history(Path(args.history_data_home).expanduser(),
                              args.budget_ms)

    if os.name == "posix" and os.geteuid() == 0:
        print("refusing to run as root: the app exits early when euid is 0",
              file=sys.stderr)
        return 2

    binary = Path(args.binary).expanduser().resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK):
        print(f"not an executable binary: {binary}", file=sys.stderr)
        return 2
    if args.runs < 1:
        print("--runs must be at least 1", file=sys.stderr)
        return 2

    try:
        scratch_root = resolve_scratch_root(args.scratch_root)
    except MeasurementError as exc:
        print(str(exc), file=sys.stderr)
        return 2

    version = "unknown"
    try:
        probe = subprocess.run(  # noqa: S603 - fixed argv
            [str(binary), "--version"], capture_output=True, text=True, timeout=60,
            env={"PATH": os.environ.get("PATH", "/usr/bin:/bin")}, check=False)
        if probe.returncode == 0 and probe.stdout.strip():
            version = probe.stdout.strip()
    except (OSError, subprocess.SubprocessError):
        pass

    source_conf = Path(args.source_conf).expanduser()
    profiles = ["empty", "realistic"] if args.profile == "both" else [args.profile]
    # A checkout that has never run the app has no settings to copy. Measure what
    # can be measured instead of refusing; an explicit --profile realistic still
    # fails loudly, because there the caller asked for the populated number.
    missing_conf = args.profile == "both" and not source_conf.exists()
    if missing_conf:
        profiles = ["empty"]

    print(f"binary   : {binary}")
    print(f"version  : {version}")
    print(f"scratch  : {scratch_root}")
    print(f"budget   : {args.budget_ms}ms (median, per profile)")
    print(f"isolation: private HOME/XDG_*/TMPDIR, offscreen QPA, "
          f"{args.timeout_seconds:.0f}s deadline, process-group kill")
    print(f"load     : {load_average():.2f} (1-min); a busy box inflates every "
          f"number below")
    if missing_conf:
        print(f"note     : no settings file at {source_conf}; measuring the "
              f"empty profile only")

    summaries: list[ProfileSummary] = []
    for profile in profiles:
        try:
            conf_text, note = load_profile_conf(profile, source_conf)
        except MeasurementError as exc:
            print(f"\n{profile}: {exc}", file=sys.stderr)
            return 2
        print(f"\n=== profile: {profile} -- {note}")
        runs: list[RunResult] = []
        for index in range(1, args.runs + 1):
            scratch = make_scratch(scratch_root, profile, index)
            try:
                run = run_once(binary, scratch, profile, index, conf_text,
                               args.timeout_seconds, args.settle_ms)
                runs.append(run)
                shown = f"{run.total_ms}ms" if run.ok else f"FAILED ({run.note})"
                print(f"  run {index}/{args.runs}: {shown}"
                      f"   (1-min load at launch: {run.load_average:.2f})")
            finally:
                if not args.keep_scratch:
                    forkmesh_safe_delete.remove_tree(scratch, scratch_root, ignore_errors=True)
        summaries.append(ProfileSummary(profile=profile, runs=runs, note=note))

    print_summary_table(summaries, args.budget_ms)
    for summary in summaries:
        print()
        print_run_details(summary)
        print_breakdown(summary, args.breakdown)

    if args.json:
        payload = {
            "binary": str(binary),
            "version": version,
            "budgetMs": args.budget_ms,
            "marker": EVENT_LOOP_MARKER,
            "profiles": [
                {
                    "profile": s.profile,
                    "note": s.note,
                    "totalsMs": s.totals,
                    "medianMs": s.median_ms,
                    "runs": [
                        {"index": r.index, "totalMs": r.total_ms,
                         "exitCode": r.exit_code, "note": r.note,
                         "loadAverage": round(r.load_average, 2)}
                        for r in s.runs
                    ],
                }
                for s in summaries
            ],
        }
        Path(args.json).write_text(json.dumps(payload, indent=2) + "\n",
                                   encoding="utf-8")
        print(f"\nwrote {args.json}")

    if any(not s.totals for s in summaries):
        print("\nFAIL: at least one profile produced no usable measurement",
              file=sys.stderr)
        return 2
    over = [s for s in summaries if (s.median_ms or 0) > args.budget_ms]
    if over:
        names = ", ".join(f"{s.profile} ({s.median_ms}ms)" for s in over)
        print(f"\nFAIL: median startup over the {args.budget_ms}ms budget: {names}",
              file=sys.stderr)
        return 1
    print(f"\nPASS: every profile's median startup is within {args.budget_ms}ms")
    return 0


if __name__ == "__main__":
    sys.exit(main())
