#!/usr/bin/env python3
from __future__ import annotations

import argparse
import contextlib
import json
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tarfile
import tempfile
import time
from pathlib import Path
from urllib.parse import urlsplit

import mac_adhoc_publish
import mobile_changelog
import next_codemagic
from next_common import ToolError

ROOT = Path(__file__).resolve().parents[1]
WORKFLOW = "ios-native-adhoc"
RECONNECTS = 20
RECONNECT_SECONDS = 30
MIRROR_TIMEOUT_SECONDS = 1800
RELEASE = mac_adhoc_publish.release_host()
UNIT_ENV = "FORKMESH_MAC_ADHOC_LOG"
UNIT_PREFIX = "forkmesh-mac-adhoc-"
UNIT_LOGS = RELEASE.LAYOUT.logs / "mac-adhoc"
PUBLISHED = RELEASE.LAYOUT.state / "mac-adhoc" / "published.json"
PASSED_ENV = re.compile(r"PATH|LANG|LC_\w+|FORKMESH_\w+|MAC_BUILD_\w+|CODEMAGIC_\w+")
DROPPED_ENV = re.compile(r"FORKMESH_AGENT_\w*|" + UNIT_ENV)
FOLLOW_SECONDS = 0.5
DESKTOP_SOURCE = "main"
DESKTOP_LOCK = "desktop-build "
STORE_WORKFLOW = RELEASE.CODEMAGIC_IOS_WORKFLOW
STORE_LOCK = "ios-app-store "
STORE_RUNS = "release/ios-main-"
STORE_SUBMIT_STEP = "Submit to the App Store"
STORE_WHATS_NEW = "Bug fixes and improvements."
RULE_WIDTH = 72
MAC_TREE = re.compile(r"/Users/[^/\s]+/ci/tree/")
STEP_DONE = re.compile(r"^<== (?P<name>.+) ok in (?P<seconds>\d+) s$")
STEP_FAILED = re.compile(r"^!! (?P<name>.+) failed after (?P<seconds>\d+) s$")
ERROR_LINE = re.compile(r"(?:^|\s)(?:error|fatal):\s|^ERROR:|^!! ")
ERROR_LIMIT = 40
CAUSE_LINES = 8
PING_KEY = "mac-adhoc-failure"
FAILURE_HINTS = (
    (re.compile(r"\.swift:\d+:\d+: error:"),
     "Swift does not compile at this commit. Fix it on main (docs/native/MAC_BUILD.md, "
     "\"Swift rules the Mac archive enforces\") and run the shortcut again."),
    (re.compile(r"early EOF|index-pack failed|promisor remote|could not fetch"),
     "The Mac could not fetch the commit from the cores. Run the shortcut again; if it "
     "repeats, check the source publisher (docs/tools/source-publisher.md)."),
    (re.compile(r"still serves"),
     "The cores never served main. Check the source publisher "
     "(docs/tools/source-publisher.md)."),
    (re.compile(r"not on the local network|lost the office Mac"),
     "The office Mac is off or away. Wake it, check tools/office-mac.sh --find, then run again."),
    (re.compile(r"held ~/ci/lock"),
     "Another build holds the Mac. See tools/mac_adhoc_deploy.py --status, or --unstick."),
    (re.compile(r"via CoreSimulator spawn"),
     "CoreSimulator on the Mac stayed wedged after the archive restarted it once. Run the "
     "shortcut again; if it repeats, reboot the office Mac (docs/native/MAC_BUILD.md)."),
    (re.compile(r"without an IPA|^!! Export", re.M),
     "The archive built but no IPA was exported. Read the export log in the artifacts folder."),
)

TAKE_TURN_SCRIPT = r"""
mkdir -p ci
printf '%s\n' "$1" > ci/ios-latest
"""

LATEST_SCRIPT = "cat ci/ios-latest 2>/dev/null || true"

STEPS_SCRIPT = r"""
set -euo pipefail
mkdir "$1"
tar -x -C "$1"
printf 'started_at=%s\nlocked_at=%s\n' "$2" "$3" > "$1/times"
touch "$1/fetched"
"""

JOB_SCRIPT = 'exec bash ci/tree/native/ios/tools/mac_adhoc_job.sh "$1" ""'

KILLTREE = ('killtree() { local child; for child in $(pgrep -P "$1"); do killtree "$child"; '
            'done; kill "$1" 2>/dev/null || true; }')

UNLOCK = "rm -f ci/lock/run ci/lock/pid ci/lock/owner; rmdir ci/lock 2>/dev/null || true"

LOCK_SCRIPT = KILLTREE + r"""
run="$1"; limit="$2"; waited=0
mkdir -p ci
until mkdir ci/lock 2>/dev/null && printf '%s\n' "$run" > ci/lock/run; do
  if [ "$(ps -o ppid= -p $$ | tr -d ' ')" = 1 ]; then
    exit 1
  fi
  if [ "$(cat ci/ios-latest 2>/dev/null)" != "$run" ]; then
    echo superseded
    exit 0
  fi
  holder="$(cat ci/lock/run 2>/dev/null || true)"
  case "$holder" in
    ci/*)
      echo "cancelling the older iOS build $holder"
      pid="$(cat ci/lock/pid 2>/dev/null || true)"
      [ -z "$pid" ] || killtree "$pid"
      sleep 5
      if [ "$(cat ci/lock/run 2>/dev/null)" = "$holder" ]; then """ + UNLOCK + r"""; fi
      continue
      ;;
  esac
  if [ -n "$(find ci/lock -maxdepth 0 -mmin +240 2>/dev/null)" ]; then
    echo "removing a build lock older than four hours: $holder"
    """ + UNLOCK + r"""
    continue
  fi
  if [ "$waited" -ge "$limit" ]; then
    echo "another build held ~/ci/lock for $((limit / 60)) minutes: $holder" >&2
    exit 1
  fi
  [ "$waited" = 0 ] && echo "waiting for the build lock, held by $holder"
  sleep 5
  waited=$((waited + 5))
done
echo locked
"""

STOP_SCRIPT = KILLTREE + r"""
if [ "$(cat ci/lock/run 2>/dev/null)" = "$1" ]; then
  pid="$(cat ci/lock/pid 2>/dev/null || true)"
  [ -z "$pid" ] || killtree "$pid"
  """ + UNLOCK + r"""
fi
"""

BUILD_TOOLS = ("xctest xcodebuild swift-build swift-test swift-frontend swift-driver "
               "swift-plugin-server SWBBuildService XCBBuildService mac_adhoc_build.sh "
               "mac_adhoc_job.sh macos_universal_on_mac.sh run_with_deadline.sh")

BUILD_PROCESSES = r"""
build_processes() {
  local pid ppid elapsed first second rest name
  ps -ww -U "$(id -u)" -o pid=,ppid=,etime=,args= | while read -r pid ppid elapsed first second rest; do
    [ "$pid" = "$$" ] && continue
    name="${first##*/}"
    case " $1 " in
      *" $name "*) ;;
      *" ${second##*/} "*) name="${second##*/}" ;;
      *)
        case " $first $second $rest " in
          *"$PWD/ci/"*|*"$PWD/release/"*|*"$PWD/desktop-release/"*) ;;
          *) continue ;;
        esac
        ;;
    esac
    echo "$pid $ppid $elapsed $name"
  done
}
"""

STATUS_SCRIPT = BUILD_PROCESSES + r"""
shopt -s nullglob
now="$(date +%s)"
minutes() { echo $(( (now - $(stat -c %Y "$1" 2>/dev/null || stat -f %m "$1")) / 60 )); }
if [ -d ci/lock ]; then
  pid="$(cat ci/lock/pid 2>/dev/null || true)"
  state="no build process"
  if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then state="build process $pid running"; fi
  echo "lock: held by $(cat ci/lock/run 2>/dev/null || echo nobody) for $(minutes ci/lock) min, $state"
else
  echo "lock: free"
fi
logs=(ci/*/jobs/*/log release/*/jobs/*/log)
if [ "${#logs[@]}" -gt 0 ]; then
  newest="$(ls -t "${logs[@]}" | head -n 1)"
  echo "newest build log: ~/$newest, last written $(minutes "$newest") min ago"
  tail -n 3 "$newest" | cut -c1-160 | sed 's/^/  | /'
fi
found="$(build_processes "$1")"
if [ -z "$found" ]; then
  echo "build processes: none"
  exit 0
fi
echo "build processes (pid, running for, name):"
printf '%s\n' "$found" | awk '{ printf "  %s %s %s%s\n", $1, $3, $4,
  ($2 == 1 && $4 != "bash") ? "  <- orphaned: its build is gone" : "" }'
"""

UNSTICK_SCRIPT = KILLTREE + BUILD_PROCESSES + r"""
pid="$(cat ci/lock/pid 2>/dev/null || true)"
[ -z "$pid" ] || killtree "$pid"
found="$(build_processes "$1")"
if [ -n "$found" ]; then
  printf '%s\n' "$found" | awk '{ print "stopping " $4 " (pid " $1 ", running " $3 ")" }'
  pids="$(printf '%s\n' "$found" | awk '{ print $1 }')"
  kill $pids 2>/dev/null || true
  sleep 5
  kill -9 $pids 2>/dev/null || true
fi
if [ -d ci/lock ]; then
  echo "releasing the build lock held by $(cat ci/lock/run 2>/dev/null || echo nobody)"
  """ + UNLOCK + r"""
  if [ -d ci/lock ]; then
    echo "could not remove ~/ci/lock, which still holds: $(ls ci/lock | tr '\n' ' ')" >&2
    exit 1
  fi
else
  echo "the build lock was free"
fi
left="$(build_processes "$1")"
if [ -n "$left" ]; then
  echo "still running after kill -9:" >&2
  printf '%s\n' "$left" >&2
  exit 1
fi
echo "the office Mac is idle"
"""

STORE_SUBMITTED_SCRIPT = r"""
grep -F "for App Review:" "$1/jobs/all/log" 2>/dev/null | tail -n 1 || true
"""

MAC_FACTS_SCRIPT = r"""
printf 'macOS %s, %s, %s free, up %s\n' \
  "$(sw_vers -productVersion 2>/dev/null || echo '?')" \
  "$(xcodebuild -version 2>/dev/null | head -n 1 || echo 'no Xcode')" \
  "$(df -h . 2>/dev/null | awk 'NR == 2 { print $4 }')" \
  "$(uptime 2>/dev/null | sed -e 's/.* up *//' -e 's/, *[0-9]* users*.*//')"
"""

BRING_BACK_SCRIPT = r"""
set -euo pipefail
shopt -s nullglob
cd "$1/out"
tar -c ipa/*.ipa *.log
"""


class Stopped(Exception):
    pass


def say(message: str) -> None:
    RELEASE.log(message)


def clock_text(seconds: float) -> str:
    total = max(0, int(round(seconds)))
    hours, rest = divmod(total, 3600)
    minutes, secs = divmod(rest, 60)
    return f"{hours}:{minutes:02d}:{secs:02d}" if hours else f"{minutes}:{secs:02d}"


def rule(title: str = "", char: str = "─") -> str:
    if not title:
        return char * RULE_WIDTH
    head = f"{char * 2} {title} "
    return head + char * max(3, RULE_WIDTH - len(head))


def fit(text: str, width: int) -> str:
    return text if len(text) <= width else text[:width - 1] + "…"


class Timeline:
    def __init__(self, clock=time.monotonic, out=None):
        self.clock = clock
        self.out = out
        self.started = clock()
        self.phases: list[dict] = []
        self.errors: list[str] = []
        self.facts: dict[str, str] = {}

    def write(self, text: str) -> None:
        sink = self.out or sys.stdout
        sink.write(text + "\n")
        sink.flush()

    def banner(self, title: str, details: list[tuple[str, str]]) -> None:
        self.write(rule(char="━"))
        self.write(f"  {title}")
        for name, value in details:
            self.write(f"  {name:<10} {value}")
        self.write(rule(char="━"))

    @property
    def current(self) -> dict | None:
        return self.phases[-1] if self.phases and self.phases[-1]["ok"] is None else None

    def begin(self, title: str) -> None:
        self.end()
        self.phases.append({"title": title, "start": self.clock(), "seconds": 0.0, "ok": None,
                            "steps": {}})
        self.write("")
        self.write(rule(f"{len(self.phases)}. {title}"))

    def end(self, ok: bool = True) -> None:
        phase = self.current
        if phase is None:
            return
        phase["seconds"] = self.clock() - phase["start"]
        phase["ok"] = ok
        mark = "✓" if ok else "✗"
        state = "done" if ok else "FAILED"
        self.write(f"   {mark} {phase['title']} {state} in {clock_text(phase['seconds'])}")

    def fact(self, name: str, value: str) -> None:
        self.facts[name] = value
        say(f"{name}: {value}")

    def note(self, line: str) -> None:
        text = MAC_TREE.sub("", line.rstrip())
        timed = STEP_DONE.match(text) or STEP_FAILED.match(text)
        if timed and self.phases:
            self.phases[-1]["steps"][timed["name"]] = {"seconds": int(timed["seconds"]),
                                                       "ok": text.startswith("<== ")}
        if ERROR_LINE.search(text) and text not in self.errors and len(self.errors) < ERROR_LIMIT:
            self.errors.append(text[:400])

    def cause(self, fallback: str = "") -> list[str]:
        specific = [line for line in self.errors
                    if not line.startswith("!! ") and not line.endswith("Every error:")]
        lines = specific or self.errors
        if not lines:
            given = [line.strip() for line in fallback.splitlines() if line.strip()]
            lines = [line for line in given if ERROR_LINE.search(line)] or given
        return [MAC_TREE.sub("", line) for line in lines][:CAUSE_LINES]

    def failed_phase(self) -> dict | None:
        failed = [phase for phase in self.phases if phase["ok"] is False]
        return failed[-1] if failed else (self.phases[-1] if self.phases else None)

    def summary(self) -> None:
        self.end()
        rows = []
        for phase in self.phases:
            mark = {True: "✓", False: "✗", None: "·"}[phase["ok"]]
            rows.append((f"{mark} {phase['title']}", phase["seconds"]))
            for name, step in phase["steps"].items():
                rows.append((f"    {'✓' if step['ok'] else '✗'} {name}", step["seconds"]))
        width = RULE_WIDTH - 12
        self.write("")
        self.write(rule("Timing"))
        for label, seconds in rows:
            self.write(f"  {fit(label, width):<{width}} {clock_text(seconds):>8}")
        self.write(f"  {'Total':<{width}} {clock_text(self.clock() - self.started):>8}")
        self.write(rule())

    def failure(self, error: str) -> None:
        phase = self.failed_phase()
        where = f" in \"{phase['title']}\"" if phase else ""
        self.write("")
        self.write(rule("✗ FAILED" + where, "━"))
        for line in self.cause(error):
            self.write(f"  {fit(line, 400)}")
        hint = failure_hint(self.cause(error) + [error])
        if hint:
            self.write("")
            self.write(f"  Fix: {hint}")
        self.write(rule(char="━"))


TIMELINE = Timeline()


def failure_hint(lines: list[str]) -> str:
    text = "\n".join(lines)
    for pattern, hint in FAILURE_HINTS:
        if pattern.search(text):
            return hint
    return ""


def stop(signum, frame):
    for name in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
        signal.signal(name, signal.SIG_IGN)
    raise Stopped(signal.Signals(signum).name)


def wait_for_commit(repo: Path, url: str, timeout: float = MIRROR_TIMEOUT_SECONDS,
                    interval: float = 20, run=subprocess.run, sleep=time.sleep,
                    clock=time.monotonic) -> str:
    main = next_codemagic.local_main(repo, run)
    deadline = clock() + timeout
    last = None
    while True:
        served = next_codemagic.served_main(url, run)
        if next_codemagic.contains(repo, main, served, run):
            say(f"{url} serves main {served[:12]}")
            return served
        if served != last:
            say(f"{url} serves {served[:12] or 'nothing'}; waiting for main {main[:12]}")
            last = served
        if clock() > deadline:
            raise ToolError(f"{url} still serves {served[:12] or 'nothing'}, not main "
                            f"{main[:12]}, after {int(timeout) // 60} minutes "
                            "(docs/tools/source-publisher.md)")
        sleep(interval)


def describe_mac(mac) -> None:
    try:
        facts = mac.run(MAC_FACTS_SCRIPT, timeout=60).strip()
    except (RELEASE.CompletionError, OSError, subprocess.SubprocessError) as exc:
        facts = f"could not read its versions ({str(exc)[:200]})"
    TIMELINE.fact("Office Mac", f"{mac.address}, {facts}" if facts else mac.address)


def find_mac(key_dir: Path = RELEASE.MAC_BUILD_KEY_DIR):
    key, known_hosts = key_dir / "id_ed25519", key_dir / "known_hosts"
    if not key.is_file() or not known_hosts.is_file():
        raise ToolError(f"no office Mac key in {key_dir} (docs/native/MAC_BUILD.md)")
    hint = RELEASE.mac_setting("MAC_BUILD_HOST", RELEASE.MAC_BUILD_HOST)
    if hint:
        mac = RELEASE.MacHost(hint, key, known_hosts)
        if mac.reachable():
            return mac
        say(f"the office Mac is not at {hint}; scanning the LAN for its pinned host key")
    else:
        say("scanning the LAN for the office Mac's pinned host key")
    for address in RELEASE.lan_ssh_hosts({hint} if hint else set()):
        mac = RELEASE.MacHost(address, key, known_hosts)
        if mac.reachable():
            return mac
    raise ToolError("the office Mac is not on the local network (is it on?)")


def write_steps(config_text: str, helper_text: str, dest: Path, name: str = WORKFLOW,
                overrides: dict[str, str] | None = None) -> list[str]:
    import yaml

    workflow = (yaml.safe_load(config_text).get("workflows") or {}).get(name)
    if not workflow:
        raise ToolError(f"codemagic.yaml has no {name} workflow")
    watch = str(((workflow.get("environment") or {}).get("vars") or {})
                .get("FORKMESH_NATIVE_WATCH", "1"))
    with tempfile.TemporaryDirectory(prefix="mac-adhoc-") as scratch:
        config = Path(scratch) / "codemagic.yaml"
        config.write_text(config_text)
        helper = Path(scratch) / "codemagic_steps.py"
        helper.write_text(helper_text)
        skip = ["--skip", "watchOS platform"] if watch == "0" else []
        listed = subprocess.run([sys.executable, str(helper), name, str(dest),
                                 "--config", str(config), *skip],
                                capture_output=True, text=True, check=True).stdout
    with (dest / "env.sh").open("a") as env:
        for key, value in (overrides or {}).items():
            env.write(f"export {key}={shlex.quote(value)}\n")
    return [line.split("\t", 1)[1] for line in listed.splitlines() if "\t" in line]


def remote(script: str, *args: str, lock: str = "") -> str:
    if lock:
        script = f"MAC_LOCK_NAME={shlex.quote(lock)}\n{RELEASE.MAC_OWNED}\n{script}"
    return " ".join(["bash", "-c", shlex.quote(script), "mac-step",
                     *(shlex.quote(arg) for arg in args)])


def relay(lines, out=None, sink=None) -> None:
    out = out or sys.stdout
    for raw in lines:
        if sink is not None:
            sink.write(raw)
            sink.flush()
        text = raw.decode("utf-8", "replace")
        out.write(text)
        out.flush()
        TIMELINE.note(text)


def follow_build(mac, run_dir: str, lock: str, popen=subprocess.Popen,
                 sleep=time.sleep, out=None) -> int:
    command = mac.command(remote(JOB_SCRIPT, run_dir, lock=lock))
    code = RELEASE.MAC_UNREACHABLE
    for attempt in range(RECONNECTS + 1):
        process = popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT)
        try:
            relay(process.stdout, out)
            code = process.wait()
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait()
        if code != RELEASE.MAC_UNREACHABLE:
            return code
        if attempt < RECONNECTS:
            say(f"lost the connection to the Mac; the build keeps running there. "
                f"Reconnecting in {RECONNECT_SECONDS} s.")
            sleep(RECONNECT_SECONDS)
    return code


@contextlib.contextmanager
def ios_lock(mac, run_dir: str):
    answer = mac.run(LOCK_SCRIPT, run_dir, str(RELEASE.MAC_LOCK_WAIT_SECONDS),
                     timeout=RELEASE.MAC_LOCK_WAIT_SECONDS + 600).splitlines()
    for line in answer[:-1]:
        say(line)
    if answer[-1:] != ["locked"]:
        say(f"cancelled: a newer iOS build ({latest(mac) or 'unknown'}) replaced {run_dir}")
        yield None
        return
    say(f"holding the office Mac's build lock as {run_dir!r}")
    try:
        yield run_dir
    finally:
        try:
            mac.run(RELEASE.MAC_UNLOCK_SCRIPT, run_dir, timeout=120)
        except (RELEASE.CompletionError, subprocess.TimeoutExpired, OSError) as exc:
            say(f"could not release the Mac's build lock ({exc}); it expires after four hours")


def stop_on_mac(mac, run_dir: str, run=subprocess.run) -> None:
    say("stopping the build on the Mac")
    try:
        run(mac.command(remote(STOP_SCRIPT, run_dir)), stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True,
            timeout=120)
    except (subprocess.TimeoutExpired, OSError) as exc:
        say(f"could not reach the Mac to stop the build ({exc}); the next run cancels it")


def fetch_on_mac(mac, url: str, commit: str, lock: str) -> str:
    TIMELINE.begin("Fetch the commit on the Mac")
    say(f"fetching {commit[:12]} from {url} into ~/ci/tree")
    say("office Mac " + mac.run(RELEASE.MAC_IOS_FETCH_SCRIPT, url, commit, timeout=900,
                                lock=lock).strip())
    subject = mac.run("git -C ci/tree log -1 --format=%s", timeout=60).strip()
    TIMELINE.facts["Commit"] = f"{commit[:12]} {subject}"
    return subject


def write_changelog(dest: Path, commit: str, repo: Path = ROOT, published: Path = PUBLISHED) -> bool:
    try:
        draft = mobile_changelog.draft(repo, commit, mobile_changelog.read_json(published),
                                       mobile_changelog.read_json(mobile_changelog.NOTES), "ios")
    except (OSError, ValueError, subprocess.CalledProcessError, SystemExit) as exc:
        say(f"no changelog for this build ({exc}); the app shows its version with no notes")
        return False
    dest.write_text(json.dumps(draft, ensure_ascii=False) + "\n")
    head = draft["entries"][0]
    say(f"changelog: {sum(len(s['items']) for s in head['sections'])} generated notes since build "
        f"{head['after'] or 'none'}, {len(draft['entries']) - 1} earlier builds")
    return True


def send_steps(mac, script: str, *args: str, lock: str, name: str = WORKFLOW,
               overrides: dict[str, str] | None = None, commit: str = "") -> list[str]:
    TIMELINE.begin("Write the build steps")
    with tempfile.TemporaryDirectory(prefix="mac-adhoc-steps-") as scratch:
        names = write_steps(mac.run("cat ci/tree/codemagic.yaml", timeout=60),
                            mac.run("cat ci/tree/native/ios/tools/codemagic_steps.py",
                                    timeout=60),
                            Path(scratch) / "steps", name, overrides)
        say(f"{len(names)} steps from codemagic.yaml ({name}):")
        for number, step in enumerate(names, 1):
            say(f"  {number:>2}. {step}")
        if commit:
            write_changelog(Path(scratch) / "steps" / "changelog.json", commit)
        packed = subprocess.Popen(["tar", "-C", scratch, "-c", "steps"], stdout=subprocess.PIPE)
        try:
            mac.run(script, *args, stdin=packed.stdout, timeout=300, lock=lock)
        finally:
            packed.stdout.close()
            packed.wait()
    return names


def prepare_build(mac, url: str, commit: str, run_dir: str, lock: str, started: int,
                  locked: int) -> str:
    subject = fetch_on_mac(mac, url, commit, lock)
    send_steps(mac, STEPS_SCRIPT, run_dir, str(started), str(locked), lock=lock, commit=commit)
    return subject


def latest(mac) -> str:
    return mac.run(LATEST_SCRIPT, timeout=60).strip()


def superseded(mac, run_dir: str, commit: str) -> bool:
    newer = latest(mac)
    if newer == run_dir:
        return False
    say(f"cancelled: a newer iOS build ({newer or 'unknown'}) replaced {commit[:12]}; "
        "it emails its own link")
    return True


def build_record(commit: str, subject: str, started: int, locked: int, built: int) -> dict:
    return {"commit": commit, "subject": subject, "startedAt": started,
            "buildStartedAt": locked, "builtAt": built, "waitSeconds": locked - started,
            "buildSeconds": built - locked}


def build_on_mac(mac, url: str, commit: str, artifacts: Path, started: int) -> Path | None:
    stamp = time.strftime("%Y%m%d%H%M%S")
    run_dir = f"ci/{commit[:12]}-{stamp}"
    TIMELINE.begin("Take the office Mac's build lock")
    TIMELINE.fact("Mac run", f"~/{run_dir}")
    mac.run(TAKE_TURN_SCRIPT, run_dir, timeout=60)
    say(f"waiting for the office Mac's build lock as {run_dir}")
    with ios_lock(mac, run_dir) as lock:
        if lock is None or superseded(mac, run_dir, commit):
            return None
        locked = int(time.time())
        try:
            subject = prepare_build(mac, url, commit, run_dir, lock, started, locked)
            TIMELINE.begin("Build on the office Mac")
            say(f"building {commit[:12]} ({subject}) on the office Mac ({mac.address})")
            code = follow_build(mac, run_dir, lock)
        except (Stopped, KeyboardInterrupt):
            stop_on_mac(mac, run_dir)
            raise
        except RELEASE.CompletionError:
            if superseded(mac, run_dir, commit):
                return None
            raise
        if code != 0:
            if superseded(mac, run_dir, commit):
                return None
            raise ToolError(f"the build failed on the office Mac (exit {code}); "
                            f"its files are in ~/{run_dir} there")
        TIMELINE.begin("Bring the IPA and logs back")
        dest = artifacts / "shortcut" / stamp / "mac-adhoc" / commit[:12]
        dest.mkdir(parents=True, exist_ok=True)
        bundle = dest / "out.tar"
        mac.run(BRING_BACK_SCRIPT, run_dir, output=bundle, timeout=1800)
    with tarfile.open(bundle) as archive:
        archive.extractall(dest, filter="data")
    bundle.unlink()
    if not sorted((dest / "ipa").glob("*.ipa")):
        raise ToolError(f"the Mac finished without an IPA (logs in {dest})")
    record = build_record(commit, subject, started, locked, int(time.time()))
    (dest / "build.json").write_text(json.dumps(record, indent=2))
    ipa = sorted((dest / "ipa").glob("*.ipa"))[0]
    say(f"{ipa.name}: {ipa.stat().st_size / 1e6:.1f} MB, sha256 {RELEASE.sha256_file(ipa)[:12]}")
    say(f"built in {clock_text(record['buildSeconds'])} on the Mac after waiting "
        f"{clock_text(record['waitSeconds'])} for it; artifacts: {dest}")
    return dest


def stream(mac, command: str, log: Path, popen=subprocess.Popen, out=None) -> int:
    process = popen(mac.command(command), stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT)
    try:
        with log.open("wb") as sink:
            relay(process.stdout, out, sink)
        return process.wait()
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait()


def send_desktop_source(mac, repo: Path, commit: str, lock: str) -> None:
    archive = subprocess.Popen(["git", "-C", str(repo), "archive", "--format=tar", commit],
                               stdout=subprocess.PIPE)
    try:
        mac.run(RELEASE.MAC_DESKTOP_SOURCE_SCRIPT, DESKTOP_SOURCE, commit, stdin=archive.stdout,
                timeout=1800, lock=lock)
    finally:
        archive.stdout.close()
        archive.wait()
    if archive.returncode != 0:
        raise ToolError(f"git archive {commit[:12]} failed ({archive.returncode})")


def build_desktop_on_mac(mac, repo: Path, commit: str, artifacts: Path) -> Path:
    dest = artifacts / "desktop" / time.strftime("%Y%m%d%H%M%S")
    dest.mkdir(parents=True, exist_ok=True)
    build_log = dest / "mac-build.log"
    subject = subprocess.run(["git", "-C", str(repo), "log", "-1", "--format=%s", commit],
                             capture_output=True, text=True, check=False).stdout.strip()
    TIMELINE.facts["Commit"] = f"{commit[:12]} {subject}"
    TIMELINE.begin("Take the office Mac's build lock")
    say(f"waiting for the office Mac's build lock as {DESKTOP_LOCK}{commit[:12]}")
    with RELEASE.mac_lock(mac, DESKTOP_LOCK + commit[:12]) as lock:
        try:
            TIMELINE.begin("Send the desktop source to the Mac")
            send_desktop_source(mac, repo, commit, lock)
            TIMELINE.begin("Build the macOS desktop app")
            say(f"building the universal macOS desktop app from {commit[:12]} ({subject}) "
                f"on the office Mac ({mac.address}); log {build_log}")
            code = stream(mac, remote(RELEASE.MAC_DESKTOP_BUILD_SCRIPT, DESKTOP_SOURCE, commit,
                                      lock=lock), build_log)
        except (Stopped, KeyboardInterrupt):
            stop_on_mac(mac, lock)
            raise
        if code != 0:
            raise ToolError(f"the macOS desktop build failed on the office Mac (exit {code}); "
                            f"its log is {build_log}")
        TIMELINE.begin("Bring the .dmg back")
        digest, built = RELEASE.mac_dmg_result(build_log.read_text(errors="replace"))
        dmg = dest / Path(built).name
        mac.run('cat "$1"', built, output=dmg, timeout=1800)
    if RELEASE.sha256_file(dmg) != digest:
        raise ToolError(f"{dmg.name} changed on its way from the Mac")
    say(f"built {dmg} ({dmg.stat().st_size} bytes, sha256 {digest[:12]})")
    return dmg


def build_desktop(args) -> int:
    commit = next_codemagic.local_main(args.repo)
    if args.execute:
        TIMELINE.begin("Find the office Mac")
    mac = find_mac()
    if not args.execute:
        say(f"main is {commit[:12]}; office Mac at {mac.address}, building the macOS desktop app")
        say("(dry run: pass --execute to build it on the Mac)")
        return 0
    describe_mac(mac)
    build_desktop_on_mac(mac, args.repo, commit, args.artifacts)
    return 0


def store_overrides(whats_new: str) -> dict[str, str]:
    return {"FORKMESH_IOS_APP_STORE": "1", "FORKMESH_IOS_WHATS_NEW": whats_new}


def submit_on_mac(mac, url: str, commit: str, whats_new: str, artifacts: Path) -> dict:
    stamp = time.strftime("%Y%m%d%H%M%S")
    run_dir = f"{STORE_RUNS}{commit[:12]}-{stamp}"
    name = f"{STORE_LOCK}{commit[:12]}-{stamp}"
    started = int(time.time())
    TIMELINE.begin("Take the office Mac's build lock")
    TIMELINE.fact("Mac run", f"~/{run_dir}")
    say(f"waiting for the office Mac's build lock as {name!r}")
    with RELEASE.mac_lock(mac, name) as lock:
        locked = int(time.time())
        try:
            subject = fetch_on_mac(mac, url, commit, lock)
            names = send_steps(mac, RELEASE.MAC_IOS_STEPS_SCRIPT, run_dir, lock=lock,
                               name=STORE_WORKFLOW, overrides=store_overrides(whats_new))
            if STORE_SUBMIT_STEP not in names:
                raise ToolError(f"{STORE_WORKFLOW} at {commit[:12]} has no "
                                f"{STORE_SUBMIT_STEP!r} step")
            TIMELINE.begin("Build, upload and submit on the office Mac")
            say(f"building {commit[:12]} ({subject}) on the office Mac ({mac.address}), "
                "uploading it and submitting it for App Review")
            code = follow_build(mac, run_dir, lock)
        except (Stopped, KeyboardInterrupt):
            stop_on_mac(mac, lock)
            raise
        if code != 0:
            raise ToolError(f"the App Store build failed on the office Mac (exit {code}); "
                            f"its files are in ~/{run_dir} there")
        TIMELINE.begin("Check the App Review submission")
        values = RELEASE.cm_env_values(mac.run('cat "$1/steps/cm_env"', run_dir, timeout=60))
        submitted = mac.run(STORE_SUBMITTED_SCRIPT, run_dir, timeout=60).strip()
    if not submitted:
        raise ToolError(f"the build finished without an App Store submission "
                        f"(see ~/{run_dir}/jobs/all/log on the Mac)")
    record = {"commit": commit, "subject": subject,
              "version": values.get("FORKMESH_IOS_VERSION", ""),
              "build": values.get("FORKMESH_IOS_BUILD", ""), "whatsNew": whats_new,
              "startedAt": started, "buildStartedAt": locked, "submittedAt": int(time.time()),
              "macRun": run_dir}
    dest = artifacts / "app-store"
    dest.mkdir(parents=True, exist_ok=True)
    (dest / f"{stamp}-{commit[:12]}.json").write_text(json.dumps(record, indent=2) + "\n")
    say(f"{submitted}")
    say(f"iOS {record['version']} ({record['build']}) from {commit[:12]} is waiting for App "
        f"Review; record {dest / f'{stamp}-{commit[:12]}.json'}")
    return record


def submit_app_store(args, url: str) -> int:
    if not args.execute:
        served = next_codemagic.served_main(url)
        main_commit = next_codemagic.local_main(args.repo)
        mac = find_mac()
        say(f"main is {main_commit[:12]}; {url} serves {served[:12] or 'nothing'}")
        say(f"office Mac at {mac.address}, building {STORE_WORKFLOW} with "
            f"FORKMESH_IOS_APP_STORE=1; What's New: {args.whats_new}")
        say("(dry run: pass --execute to build, upload and submit it for App Review)")
        return 0
    TIMELINE.begin("Wait for the cores to serve main")
    commit = wait_for_commit(args.repo, url, args.mirror_timeout)
    TIMELINE.begin("Find the office Mac")
    mac = find_mac()
    describe_mac(mac)
    submit_on_mac(mac, url, commit, args.whats_new, args.artifacts)
    return 0


class Detached(Exception):
    pass


def leave(signum, frame):
    raise Detached(signal.Signals(signum).name)


def detach_command(unit: str, log: Path, argv: list[str], environ) -> list[str]:
    passed = [f"--setenv={name}={value}" for name, value in sorted(environ.items())
              if PASSED_ENV.fullmatch(name) and not DROPPED_ENV.fullmatch(name)
              and "\n" not in value]
    return ["systemd-run", "--user", "--quiet", "--collect", f"--unit={unit}",
            "--property=KillMode=mixed", "--property=IgnoreOnIsolate=yes",
            "--property=TimeoutStopSec=180",
            f"--property=StandardOutput=append:{log}", f"--property=StandardError=append:{log}",
            f"--working-directory={Path.home()}", f"--setenv={UNIT_ENV}={log}", *passed,
            sys.executable, "-u", str(Path(__file__).resolve()), *argv]


def git_common_dir(repo: Path, run=subprocess.run) -> Path:
    done = run(["git", "-C", str(repo), "rev-parse", "--path-format=absolute",
                "--git-common-dir"], capture_output=True, text=True, check=False)
    found = done.stdout.strip() if done.returncode == 0 else ""
    return Path(found) if found else repo


def unit_active(unit: str, run=subprocess.run) -> bool:
    return run(["systemctl", "--user", "is-active", "--quiet", unit], check=False).returncode == 0


def stop_unit(unit: str, run=subprocess.run) -> None:
    run(["systemctl", "--user", "stop", "--no-block", unit], check=False)


def running_units(run=subprocess.run) -> list[str]:
    done = run(["systemctl", "--user", "list-units", "--plain", "--no-legend",
                f"{UNIT_PREFIX}*.service"], capture_output=True, text=True, check=False)
    return [line.split()[0] for line in done.stdout.splitlines() if line.strip()]


def stop_units(run=subprocess.run) -> int:
    units = running_units(run)
    if not units:
        say("no Mac ad hoc build is running")
    for unit in units:
        say(f"stopping {unit}")
        run(["systemctl", "--user", "stop", unit], check=False)
    return 0


def last_line(log: Path) -> str:
    with contextlib.suppress(OSError):
        with log.open("rb") as handle:
            handle.seek(max(0, log.stat().st_size - 4096))
            lines = [line for line in handle.read().decode("utf-8", "replace").splitlines()
                     if line.strip()]
            return lines[-1][:160] if lines else ""
    return ""


def describe_units(run=subprocess.run, clock=time.time) -> None:
    units = running_units(run)
    if not units:
        say("this desktop runs no Mac build")
    for unit in units:
        log = UNIT_LOGS / f"{unit}.log"
        with contextlib.suppress(OSError):
            idle = int(clock() - log.stat().st_mtime) // 60
            say(f"{unit}: no output for {idle} min, log {log}")
            say(f"  | {last_line(log)}")
            continue
        say(f"{unit}: no log at {log}")


def status() -> int:
    describe_units()
    try:
        mac = find_mac()
        say(f"office Mac at {mac.address}")
        print(mac.run(STATUS_SCRIPT, BUILD_TOOLS, timeout=120).rstrip())
    except (ToolError, RELEASE.CompletionError, OSError, subprocess.SubprocessError) as error:
        print(f"mac_adhoc_deploy: {error}", file=sys.stderr)
        return 1
    return 0


ASC_IDS_SCRIPT = 'cat ~/.asc/key_id; echo; cat ~/.asc/issuer'
STORE_REPORT = ROOT / "native" / "ios" / "tools" / "asc_store_report.py"


def asc_ids(environ=os.environ, find=None) -> dict[str, str]:
    ids = {name: environ.get(name, "").strip() for name in ("ASC_KEY_ID", "ASC_ISSUER_ID")}
    if all(ids.values()):
        return ids
    tools_dir = ROOT / "native" / "ios" / "tools"
    if str(tools_dir) not in sys.path:
        sys.path.insert(0, str(tools_dir))
    from asc_bundle_ids import action_variables
    stored = action_variables()
    ids = {name: ids[name] or stored.get(name, "") for name in ids}
    if all(ids.values()):
        return ids
    mac = (find or find_mac)()
    say(f"reading the App Store Connect key ids from the office Mac at {mac.address}")
    answer = mac.run(ASC_IDS_SCRIPT, timeout=60).split()
    if len(answer) != 2:
        raise ToolError("the office Mac has no ~/.asc/key_id and ~/.asc/issuer "
                        "(docs/native/MAC_BUILD.md)")
    return {"ASC_KEY_ID": answer[0], "ASC_ISSUER_ID": answer[1]}


def store_status(run=subprocess.run, feedback: int | None = None) -> int:
    try:
        ids = asc_ids()
    except (ToolError, RELEASE.CompletionError, OSError, subprocess.SubprocessError) as error:
        print(f"mac_adhoc_deploy: {error}", file=sys.stderr)
        return 1
    key = Path.home() / ".appstoreconnect" / "private_keys" / f"AuthKey_{ids['ASC_KEY_ID']}.p8"
    if not key.is_file():
        print(f"mac_adhoc_deploy: no App Store Connect key at {key} "
              "(docs/native/IOS_RELEASE.md)", file=sys.stderr)
        return 1
    command = [sys.executable, "-u", str(STORE_REPORT), "--publish"]
    if feedback is not None:
        command.extend(["--feedback", str(feedback)])
    return run(command, env={**os.environ, **ids}).returncode


def unstick() -> int:
    stop_units()
    try:
        mac = find_mac()
        say(f"office Mac at {mac.address}")
        print(mac.run(UNSTICK_SCRIPT, BUILD_TOOLS, timeout=180).rstrip())
    except (ToolError, RELEASE.CompletionError, OSError, subprocess.SubprocessError) as error:
        print(f"mac_adhoc_deploy: {error}", file=sys.stderr)
        return 1
    return 0


def exit_status(log: Path) -> int:
    try:
        return int(log.with_suffix(".exit").read_text().strip())
    except (OSError, ValueError):
        say(f"the build ended without an exit status; its log is {log}")
        return 1


def follow(unit: str, log: Path, active=unit_active, stop_build=stop_unit, sleep=time.sleep,
           out=None) -> int:
    out = out or sys.stdout
    stopping = []

    def interrupt(signum, frame):
        if not stopping:
            stopping.append(signum)
            say(f"Ctrl-C: stopping {unit}")
            stop_build(unit)

    names = (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)
    previous = {name: signal.getsignal(name) for name in names}
    signal.signal(signal.SIGINT, interrupt)
    signal.signal(signal.SIGTERM, leave)
    signal.signal(signal.SIGHUP, leave)
    try:
        with log.open(errors="replace") as handle:
            while True:
                chunk = handle.read()
                if chunk:
                    out.write(chunk)
                    out.flush()
                elif active(unit):
                    sleep(FOLLOW_SECONDS)
                else:
                    out.write(handle.read())
                    out.flush()
                    break
    except (Detached, BrokenPipeError) as reason:
        cause = reason.args[0] if isinstance(reason, Detached) else "output closed"
        with contextlib.suppress(OSError):
            if stopping:
                say(f"{cause}: {unit} is stopping the build; its log is {log}")
            else:
                say(f"{cause}: {unit} keeps building; follow {log}, or stop it with "
                    "tools/mac_adhoc_deploy.py --stop")
        return 130 if stopping else 0
    finally:
        for name, handler in previous.items():
            signal.signal(name, handler)
    return exit_status(log)


def launch(argv: list[str], repo: Path, environ=os.environ, run=subprocess.run) -> int | None:
    if not shutil.which("systemd-run"):
        say("no systemd-run here, so the build runs in this process and stops with it")
        return None
    unit = f"{UNIT_PREFIX}{time.strftime('%Y%m%d%H%M%S')}-{os.getpid()}"
    UNIT_LOGS.mkdir(parents=True, exist_ok=True)
    log = UNIT_LOGS / f"{unit}.log"
    log.touch()
    started = run(detach_command(unit, log, [*argv, "--repo", str(git_common_dir(repo))],
                                 environ), capture_output=True, text=True, check=False)
    if started.returncode != 0:
        say(f"systemd-run could not start the build ({started.stderr.strip()}); "
            "it runs in this process and stops with it")
        return None
    say(f"building in {unit}.service, which a desktop restart leaves running; "
        f"Stop, Ctrl-C, --stop or a newer build ends it (log {log})")
    return follow(unit, log)


def mode_label(args) -> str:
    if args.desktop:
        return "macOS desktop build"
    if args.app_store:
        return "iOS App Store build"
    return "iOS ad hoc build"


def tool_commit() -> str:
    done = subprocess.run(["git", "-C", str(ROOT), "log", "-1", "--format=%h"],
                          capture_output=True, text=True, check=False)
    return done.stdout.strip() or "unknown"


def open_banner(args, url: str) -> None:
    log = os.environ.get(UNIT_ENV, "")
    TIMELINE.facts["Log"] = log or "this terminal only"
    TIMELINE.banner(f"ForkMesh {mode_label(args)} on the office Mac", [
        ("Source", url if not args.desktop else f"{args.repo} ({DESKTOP_SOURCE})"),
        ("Started", time.strftime("%Y-%m-%d %H:%M:%S %Z")),
        ("Tool", f"tools/mac_adhoc_deploy.py at {tool_commit()}, Python "
                 f"{sys.version.split()[0]}"),
        ("Log", log or "this terminal only"),
    ])


def failure_notifier(url: str):
    import next_autodeploy

    return next_autodeploy.Notifier(
        origin=urlsplit(url).hostname or "", node=next_autodeploy.settings_node_name(RELEASE.LAYOUT),
        key_path=RELEASE.LAYOUT.identity_key, say=say)


def failure_ping(label: str, error: str) -> tuple[str, str]:
    phase = TIMELINE.failed_phase()
    cause = TIMELINE.cause(error)
    total = clock_text(TIMELINE.clock() - TIMELINE.started)
    where = (f" in \"{phase['title']}\" after {clock_text(phase['seconds'])}" if phase else "")
    lines = [f"The {label} failed{where} (total {total}).", "Cause:"]
    lines += [f"  {fit(line, 300)}" for line in cause]
    hint = failure_hint(cause + [error])
    if hint:
        lines.append(f"Fix: {hint}")
    lines += [f"{name}: {value}" for name, value in TIMELINE.facts.items()]
    lines.append("Check: python3 tools/mac_adhoc_deploy.py --status")
    title = f"{label} failed" + (f": {phase['title']}" if phase else "")
    return fit(title, 120), "\n".join(lines)[:2000]


def notify_failure(label: str, error: str, url: str, notifier=None) -> bool:
    title, body = failure_ping(label, error)
    try:
        notifier = notifier or failure_notifier(url)
        return notifier.send(title, body, PING_KEY)
    except (OSError, ValueError, ImportError) as exc:
        say(f"notify: the failure Ping could not be sent ({exc})")
        return False


def ship_adhoc(args, url: str, started: int) -> int:
    TIMELINE.begin("Wait for the cores to serve main")
    commit = wait_for_commit(args.repo, url, args.mirror_timeout)
    TIMELINE.begin("Find the office Mac")
    mac = find_mac()
    describe_mac(mac)
    dest = build_on_mac(mac, url, commit, args.artifacts, started)
    if dest is None:
        return 0
    TIMELINE.begin("Publish and email the install link")
    publish = ["--execute", "--artifacts", str(args.artifacts),
               "--runs", str(args.artifacts / "runs")]
    for address in args.to:
        publish += ["--to", address]
    code = mac_adhoc_publish.main(publish)
    if code != 0:
        raise ToolError(f"the publisher exited {code} after the build succeeded "
                        f"(artifacts {dest}); see its output above")
    return 0


def dry_run(args, url: str) -> int:
    served = next_codemagic.served_main(url)
    main_commit = next_codemagic.local_main(args.repo)
    mac = find_mac()
    say(f"main is {main_commit[:12]}; {url} serves {served[:12] or 'nothing'}")
    say(f"office Mac at {mac.address}, building {WORKFLOW}")
    say("(dry run: pass --execute to build on the Mac and email the link)")
    return 0


def run_mode(args, url: str, started: int) -> int:
    if args.desktop:
        return build_desktop(args)
    if args.app_store:
        return submit_app_store(args, url)
    if not args.execute:
        return dry_run(args, url)
    return ship_adhoc(args, url, started)


def deploy(args, notify=notify_failure) -> int:
    global TIMELINE
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGHUP, stop)
    started = int(time.time())
    url = RELEASE.mac_setting("MAC_BUILD_GIT_URL", RELEASE.MAC_BUILD_GIT_URL)
    TIMELINE = Timeline()
    if args.execute:
        open_banner(args, url)
    try:
        code = run_mode(args, url, started)
    except Stopped as reason:
        TIMELINE.end(ok=False)
        say(f"stopped ({reason}); the office Mac's build lock is released")
        if args.execute:
            TIMELINE.summary()
        return 130
    except KeyboardInterrupt:
        return 130
    except (ToolError, RELEASE.CompletionError, OSError, subprocess.SubprocessError,
            tarfile.TarError) as error:
        TIMELINE.end(ok=False)
        print(f"mac_adhoc_deploy: {error}", file=sys.stderr, flush=True)
        if args.execute:
            TIMELINE.summary()
            TIMELINE.failure(str(error))
            notify(mode_label(args), str(error), url)
        return 1
    if args.execute:
        TIMELINE.summary()
        say(f"the {mode_label(args)} finished")
    return code


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="Build the native iOS app ad hoc on the office Mac from the cores' main, "
                    "then publish it and email its install link (docs/native/MAC_BUILD.md).")
    parser.add_argument("--execute", action="store_true", help="build and send (default: dry run)")
    parser.add_argument("--desktop", action="store_true",
                        help="build the universal macOS desktop .dmg from this checkout's main "
                             "instead, into <artifacts>/desktop/<time>/")
    parser.add_argument("--app-store", action="store_true",
                        help="build ios-native-release from the cores' main instead, upload it "
                             "to App Store Connect and submit it for App Review")
    parser.add_argument("--whats-new",
                        default=os.environ.get("FORKMESH_IOS_WHATS_NEW", "").strip()
                        or STORE_WHATS_NEW,
                        help="the App Store's What's New text for --app-store")
    parser.add_argument("--to", action="append", default=[],
                        help="email these addresses instead of the early iOS testers (repeatable)")
    parser.add_argument("--artifacts", type=Path, default=RELEASE.LAYOUT.data / "mac-adhoc")
    parser.add_argument("--mirror-timeout", type=int, default=MIRROR_TIMEOUT_SECONDS)
    parser.add_argument("--repo", type=Path, default=ROOT,
                        help="git directory whose main is shipped (default: this checkout)")
    parser.add_argument("--stop", action="store_true",
                        help="stop every running Mac ad hoc build started by this tool")
    parser.add_argument("--status", action="store_true",
                        help="show this desktop's Mac builds, the Mac's build lock, its newest "
                             "build log and every build process on it")
    parser.add_argument("--store-status", action="store_true",
                        help="show the iOS app in App Store Connect: App Store versions and "
                            "App Review submissions, TestFlight builds and groups, tester "
                            "feedback and customer reviews")
    parser.add_argument("--feedback", type=int,
                        help="show this many recent tester feedback reports and customer reviews")
    parser.add_argument("--unstick", action="store_true",
                        help="stop every Mac build this desktop runs, kill every build process "
                             "on the Mac, leftovers included, and release its build lock")
    raw = sys.argv[1:] if argv is None else list(argv)
    args = parser.parse_args(raw)
    if args.stop:
        return stop_units()
    if args.status:
        return status()
    if args.unstick:
        return unstick()
    if args.store_status:
        return store_status(feedback=args.feedback)
    unit_log = os.environ.get(UNIT_ENV, "")
    if args.execute and not unit_log:
        code = launch(raw, args.repo)
        if code is not None:
            return code
    code = 1
    try:
        code = deploy(args)
    finally:
        if unit_log:
            Path(unit_log).with_suffix(".exit").write_text(f"{code}\n")
    return code


if __name__ == "__main__":
    sys.exit(main())
