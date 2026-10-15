#!/usr/bin/env python3
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CLIENT = REPO / "desktop"
BUILD = CLIENT / "build"
START = time.time()
POLL_SECONDS = 1.0
START_TIMEOUT_SECONDS = 90
LOST_ENGINE_SECONDS = 180


def stamp():
    return f"[{time.strftime('%H:%M:%S')} +{time.time() - START:7.1f}s]"


def say(text=""):
    print(f"{stamp()} {text}" if text else "", flush=True)


def git(*args):
    result = subprocess.run(["git", "-C", str(REPO), *args], capture_output=True, text=True)
    return result.stdout.strip() if result.returncode == 0 else ""


def core_binary():
    candidates = [os.environ.get("FORKMESH_CORE_BIN", ""), str(BUILD / "forkmesh-core"), shutil.which("forkmesh-core") or ""]
    for candidate in candidates:
        if candidate and os.access(candidate, os.X_OK):
            return candidate
    return ""


def core_call(binary, method, params=None):
    args = [binary, "call", method]
    if params is not None:
        args.append(json.dumps(params))
    try:
        result = subprocess.run(args, capture_output=True, text=True, timeout=30)
    except subprocess.TimeoutExpired:
        return None, "timed out"
    if result.returncode != 0:
        return None, (result.stderr or result.stdout).strip()
    try:
        return json.loads(result.stdout or "null"), ""
    except json.JSONDecodeError:
        return result.stdout.strip(), ""


def file_time(path):
    try:
        return path.stat().st_mtime
    except OSError:
        return 0.0


def describe_time(seconds):
    return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(seconds)) if seconds else "missing"


def cache_value(key):
    try:
        for line in (BUILD / "CMakeCache.txt").read_text(errors="replace").splitlines():
            if line.startswith(key + ":"):
                return line.split("=", 1)[1]
    except OSError:
        pass
    return ""


def ninja_entries():
    entries = {}
    try:
        lines = (BUILD / ".ninja_log").read_text(errors="replace").splitlines()
    except OSError:
        return entries
    for line in lines:
        parts = line.split("\t")
        if len(parts) < 5 or line.startswith("#"):
            continue
        try:
            entries[parts[3]] = (int(parts[0]), int(parts[1]), parts[2])
        except ValueError:
            continue
    return entries


def module_of(output):
    match = re.search(r"CMakeFiles/([^/]+)\.dir/", output)
    if match:
        return match.group(1)
    if "_autogen" in output:
        return output.split("_autogen", 1)[0].rsplit("/", 1)[-1] + " (moc/uic)"
    return output.rsplit("/", 1)[-1]


def report_compiled(before):
    after = ninja_entries()
    built = {path: entry for path, entry in after.items() if before.get(path, (0, 0, ""))[2] != entry[2]}
    if not built:
        say("compiled: nothing — every target was already up to date (or the build did not use Ninja)")
        return
    modules = {}
    for path, (begin, end, _) in built.items():
        seconds, count = modules.get(module_of(path), (0.0, 0))
        modules[module_of(path)] = (seconds + max(0, end - begin) / 1000.0, count + 1)
    say(f"compiled {len(built)} outputs across {len(modules)} modules (summed compile seconds, parallel jobs overlap):")
    for name, (seconds, count) in sorted(modules.items(), key=lambda item: -item[1][0]):
        print(f"    {seconds:8.1f}s  {count:5d} outputs  {name}", flush=True)
    say("slowest single outputs:")
    for path, (begin, end, _) in sorted(built.items(), key=lambda item: item[1][0] - item[1][1])[:25]:
        print(f"    {(end - begin) / 1000.0:8.1f}s  {path}", flush=True)


def snapshot_binaries():
    return {
        "desktop app": file_time(BUILD / "forkmesh"),
        "forkmesh-core (Go background engine)": file_time(BUILD / "forkmesh-core"),
        "forkmesh-mirror-node (Go companion)": file_time(BUILD / "forkmesh-mirror-node"),
    }


def main():
    binary = core_binary()
    say("=== Rebuild & restart ===")
    say(f"checkout {REPO}")
    say(f"branch {git('rev-parse', '--abbrev-ref', 'HEAD') or '?'}  HEAD {git('log', '-1', '--format=%h %s') or '?'}")
    dirty = [line for line in git("status", "--porcelain").splitlines() if line]
    say(f"uncommitted changes: {len(dirty)} files")
    say(f"build dir {BUILD}  type {cache_value('CMAKE_BUILD_TYPE') or 'not configured'}  generator {cache_value('CMAKE_GENERATOR') or '?'}")
    say(f"go toolchain: {shutil.which('go') or 'not found — forkmesh-core will not be rebuilt'}")
    binaries = snapshot_binaries()
    for name, mtime in binaries.items():
        say(f"before: {name} built {describe_time(mtime)}")
    if not binary:
        say("forkmesh-core is not available, so the rebuild cannot be handed to the background engine; use the desktop's rebuild button.")
        return 1
    say(f"engine {binary}")
    status, _ = core_call(binary, "core.status")
    if isinstance(status, dict):
        say(f"engine running: version {status.get('version', '?')} pid {status.get('pid', '?')}")
    ninja_before = ninja_entries()

    started, error = core_call(binary, "core.update.rebuild", {"requestedBy": "the Rebuild & restart shortcut"})
    previous = None
    if started is None:
        if "already_running" not in error:
            say(f"the engine refused the rebuild: {error}")
            return 1
        say("a rebuild is already running; following it")
    else:
        previous = started.get("previousRun") if isinstance(started, dict) else None
        say("rebuild requested; the desktop's debug area shows the same run")

    run = None
    outcome = "unknown"
    seq = 0
    phase = None
    phase_started = time.time()
    phases = []
    status_text = ""
    lost_since = None
    waiting_since = time.time()
    while True:
        state, error = core_call(binary, "core.update.state")
        if not isinstance(state, dict):
            lost_since = lost_since or time.time()
            if time.time() - lost_since > LOST_ENGINE_SECONDS:
                say(f"lost the background engine for {LOST_ENGINE_SECONDS}s ({error}); stopping the log here")
                break
            time.sleep(POLL_SECONDS * 2)
            continue
        lost_since = None
        current = state.get("run")
        if run is None:
            if current != previous and (state.get("running") or state.get("last")):
                run = current
                say(f"following run {run}")
            elif time.time() - waiting_since > START_TIMEOUT_SECONDS:
                say(f"no rebuild started within {START_TIMEOUT_SECONDS}s; the desktop may have refused it (see its debug area)")
                return 1
            else:
                time.sleep(POLL_SECONDS)
                continue
        if current != run:
            say(f"a newer run ({current}) replaced run {run}; stopping")
            break
        new_seq = int(state.get("seq") or 0)
        lines = state.get("lines") or []
        if new_seq > seq:
            for line in lines[-min(len(lines), new_seq - seq):]:
                print(f"{stamp()} | {line}", flush=True)
            seq = new_seq
        if state.get("phase") and state.get("phase") != phase:
            if phase:
                phases.append((phase, time.time() - phase_started))
            phase, phase_started = state.get("phase"), time.time()
            say(f">> phase: {phase}")
        if state.get("status") and state.get("status") != status_text:
            status_text = state.get("status")
            say(f">> status: {status_text}")
        last = state.get("last") or {}
        if not state.get("running") and last.get("run") == run:
            if phase:
                phases.append((phase, time.time() - phase_started))
            outcome = "ok" if last.get("ok") else ("cancelled" if last.get("cancelled") else "failed")
            say(f"=== finished: {outcome} {last.get('error') or ''}".rstrip())
            if state.get("relaunching"):
                say("the desktop is relaunching onto the new build")
            break
        time.sleep(POLL_SECONDS)

    say("")
    say("=== Timing ===")
    for name, seconds in phases:
        print(f"    {seconds:8.1f}s  {name}", flush=True)
    print(f"    {time.time() - START:8.1f}s  total", flush=True)
    say("=== Modules ===")
    report_compiled(ninja_before)
    say("=== Binaries ===")
    for name, mtime in snapshot_binaries().items():
        verdict = "rebuilt" if mtime > binaries[name] else "unchanged"
        say(f"{name}: {verdict} ({describe_time(mtime)})")
    say("the background engine restarts onto a newer forkmesh-core once its running work (this shortcut included) finishes")
    return 0 if outcome == "ok" else 1


if __name__ == "__main__":
    sys.exit(main())
