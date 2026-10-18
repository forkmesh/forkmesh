import io
import os
import re
import shutil
import signal
import subprocess
import time
from pathlib import Path

import pytest

import mac_adhoc_deploy
from next_common import ToolError

ROOT = Path(__file__).resolve().parents[1]
SHORTCUT = ROOT / ".forkmesh" / "shortcuts" / "deploy-native-ios-ad-hoc-mac.sh"
DESKTOP_SHORTCUT = ROOT / ".forkmesh" / "shortcuts" / "build-desktop-macos-mac.sh"
STORE_SHORTCUT = ROOT / ".forkmesh" / "shortcuts" / "deploy-native-ios-app-store-mac.sh"


def git_runner(main, served_answers, ancestors=()):
    served = iter(served_answers)

    def run(args, **kwargs):
        if "rev-parse" in args:
            return subprocess.CompletedProcess(args, 0, main + "\n", "")
        if "ls-remote" in args:
            sha = next(served)
            return subprocess.CompletedProcess(args, 0, f"{sha}\trefs/heads/main\n" if sha else "", "")
        if "merge-base" in args:
            return subprocess.CompletedProcess(args, 0 if args[-1] in ancestors else 1, "", "")
        raise AssertionError(args)
    return run


def test_waits_until_the_cores_serve_main():
    slept = []
    run = git_runner("a" * 40, ["", "b" * 40, "a" * 40])
    served = mac_adhoc_deploy.wait_for_commit(ROOT, "https://example", run=run,
                                              sleep=slept.append, clock=lambda: 0)
    assert served == "a" * 40
    assert len(slept) == 2


def test_builds_a_newer_served_commit_that_contains_main():
    run = git_runner("a" * 40, ["c" * 40], ancestors={"c" * 40})
    assert mac_adhoc_deploy.wait_for_commit(ROOT, "u", run=run, sleep=lambda _: None) == "c" * 40


def test_gives_up_when_main_never_arrives():
    ticks = iter([0, 10, 100])
    run = git_runner("a" * 40, ["b" * 40] * 3)
    with pytest.raises(ToolError, match="still serves bbbbbbbbbbbb"):
        mac_adhoc_deploy.wait_for_commit(ROOT, "u", timeout=60, run=run, sleep=lambda _: None,
                                         clock=lambda: next(ticks))


class FakeMac:
    def command(self, remote):
        return ["ssh", remote]


class FakeProcess:
    def __init__(self, code, output=b""):
        self.code = code
        self.stdout = io.BytesIO(output)

    def wait(self):
        return self.code

    def poll(self):
        return self.code


def test_reconnects_while_the_mac_keeps_building(capsys):
    codes = iter([255, 255, 0])
    commands = []

    def popen(command, **kwargs):
        commands.append(command)
        return FakeProcess(next(codes))

    code = mac_adhoc_deploy.follow_build(FakeMac(), "ci/abc-1", "ci/abc-1", popen=popen,
                                         sleep=lambda _: None)
    assert code == 0
    assert len(commands) == 3
    assert "mac_adhoc_job.sh" in commands[0][1] and "ci/abc-1" in commands[0][1]
    assert "MAC_LOCK_NAME=ci/abc-1" in commands[0][1]
    assert capsys.readouterr().out.count("Reconnecting") == 2


def test_a_failed_step_is_not_retried():
    calls = []

    def popen(command, **kwargs):
        calls.append(command)
        return FakeProcess(1)

    assert mac_adhoc_deploy.follow_build(FakeMac(), "r", "r", popen=popen,
                                         sleep=lambda _: None) == 1
    assert len(calls) == 1


BUILD_OUTPUT = (
    b"==> Archive (signing per FORKMESH_NATIVE_SIGNING) (19:33:30)\n"
    b"<== Pick the ad hoc build number ok in 4 s\n"
    b"/Users/build/ci/tree/native/ios/ForkMesh/Features/Packages/PackageUninstall.swift:20:90: "
    b"error: call to main actor-isolated instance method 'dependents(of:)' in a synchronous "
    b"nonisolated context\n"
    b"ERROR: the archive failed. Every error:\n"
    b"/Users/build/ci/tree/native/ios/ForkMesh/Features/Packages/PackageUninstall.swift:20:90: "
    b"error: call to main actor-isolated instance method 'dependents(of:)' in a synchronous "
    b"nonisolated context\n"
    b"!! Archive (signing per FORKMESH_NATIVE_SIGNING) failed after 702 s\n"
)


def fresh_timeline(monkeypatch, ticks=None):
    times = iter(ticks or range(0, 10000, 30))
    timeline = mac_adhoc_deploy.Timeline(clock=lambda: next(times), out=io.StringIO())
    monkeypatch.setattr(mac_adhoc_deploy, "TIMELINE", timeline)
    return timeline


def test_the_build_output_is_relayed_and_its_errors_and_step_times_kept(monkeypatch):
    timeline = fresh_timeline(monkeypatch)
    timeline.begin("Build on the office Mac")
    out = io.StringIO()
    code = mac_adhoc_deploy.follow_build(
        FakeMac(), "r", "r", popen=lambda command, **kwargs: FakeProcess(1, BUILD_OUTPUT),
        sleep=lambda _: None, out=out)
    assert code == 1
    assert out.getvalue() == BUILD_OUTPUT.decode()
    steps = timeline.phases[-1]["steps"]
    assert steps["Pick the ad hoc build number"] == {"seconds": 4, "ok": True}
    assert steps["Archive (signing per FORKMESH_NATIVE_SIGNING)"] == {"seconds": 702, "ok": False}
    assert timeline.cause() == [
        "native/ios/ForkMesh/Features/Packages/PackageUninstall.swift:20:90: error: call to main "
        "actor-isolated instance method 'dependents(of:)' in a synchronous nonisolated context"]


def test_a_failed_fetch_names_the_git_error_and_a_fix(monkeypatch):
    timeline = fresh_timeline(monkeypatch)
    timeline.begin("Fetch the commit on the Mac")
    timeline.end(ok=False)
    error = ("the office Mac step failed (128): warning: redirecting to https://n3/x.git/\n"
             "fatal: early EOF\nfatal: index-pack failed\n")
    assert timeline.cause(error) == ["fatal: early EOF", "fatal: index-pack failed"]
    assert "fetch the commit" in mac_adhoc_deploy.failure_hint(timeline.cause(error))



def test_a_wedged_simulator_service_names_a_fix(monkeypatch):
    timeline = fresh_timeline(monkeypatch)
    timeline.begin("Build on the office Mac")
    timeline.note("/Users/build/ci/tree/native/ios/ForkMeshWatch/Assets.xcassets: error: Failed to "
                  "launch AssetCatalogSimulatorAgent via CoreSimulator spawn")
    timeline.end(ok=False)
    assert timeline.cause() == ["native/ios/ForkMeshWatch/Assets.xcassets: error: Failed to "
                                "launch AssetCatalogSimulatorAgent via CoreSimulator spawn"]
    assert "reboot the office Mac" in mac_adhoc_deploy.failure_hint(timeline.cause())


def test_a_compiler_crash_is_the_cause_and_names_a_fix(monkeypatch):
    timeline = fresh_timeline(monkeypatch)
    timeline.begin("Build on the office Mac")
    for line in ("ERROR: the archive failed. Every error:",
                 "ERROR: compiler crash: 1.\tApple Swift version 6.3.3",
                 "ERROR: compiler crash: 4.\tRunning pass \"lower-expect\" on function \"$s8F\"",
                 "!! Archive (signing per FORKMESH_NATIVE_SIGNING) failed after 1172 s"):
        timeline.note(line)
    timeline.end(ok=False)
    assert timeline.cause() == ["ERROR: compiler crash: 1.\tApple Swift version 6.3.3",
                                "ERROR: compiler crash: 4.\tRunning pass \"lower-expect\" on "
                                "function \"$s8F\""]
    assert "Compiler crashes" in mac_adhoc_deploy.failure_hint(timeline.cause())


def test_the_summary_times_every_phase_and_step(monkeypatch):
    timeline = fresh_timeline(monkeypatch, [0, 0, 65, 65, 765, 800])
    timeline.begin("Find the office Mac")
    timeline.begin("Build on the office Mac")
    timeline.note("<== Archive ok in 700 s")
    timeline.summary()
    text = timeline.out.getvalue()
    assert "✓ Find the office Mac done in 1:05" in text
    lines = text.splitlines()
    assert any("✓ Archive" in line and line.endswith("11:40") for line in lines)
    assert lines[-2].split() == ["Total", "13:20"]


def test_a_failure_pings_the_operator_with_the_specific_error(monkeypatch):
    timeline = fresh_timeline(monkeypatch)
    timeline.facts["Commit"] = "602f6859fe6f Merge x into main"
    timeline.facts["Mac run"] = "~/ci/602f6859fe6f-1"
    timeline.begin("Build on the office Mac")
    for line in BUILD_OUTPUT.decode().splitlines():
        timeline.note(line)
    timeline.end(ok=False)
    sent = []

    class Notifier:
        def send(self, title, body, key, email=False):
            sent.append((title, body, key))
            return True

    assert mac_adhoc_deploy.notify_failure("iOS ad hoc build", "the build failed (exit 1)", "u",
                                           notifier=Notifier())
    title, body, key = sent[0]
    assert title == "iOS ad hoc build failed: Build on the office Mac"
    assert key == mac_adhoc_deploy.PING_KEY
    assert "PackageUninstall.swift:20:90: error: call to main actor-isolated" in body
    assert "Fix: Swift does not compile" in body
    assert "Commit: 602f6859fe6f Merge x into main" in body
    assert "Mac run: ~/ci/602f6859fe6f-1" in body
    assert len(body) <= 2000 and len(title) <= 120


def test_deploy_pings_once_when_the_build_fails(monkeypatch, capsys):
    pings = []

    def failing(args, url, started):
        mac_adhoc_deploy.TIMELINE.begin("Build on the office Mac")
        raise ToolError("the build failed on the office Mac (exit 1)")

    monkeypatch.setattr(mac_adhoc_deploy, "ship_adhoc", failing)
    monkeypatch.setattr(mac_adhoc_deploy.signal, "signal", lambda *args: None)
    args = mac_adhoc_deploy.argparse.Namespace(desktop=False, app_store=False, execute=True,
                                               repo=ROOT, to=[])
    code = mac_adhoc_deploy.deploy(args, notify=lambda *given: pings.append(given))
    assert code == 1
    assert [ping[0] for ping in pings] == ["iOS ad hoc build"]
    out = capsys.readouterr().out
    assert "FAILED in \"Build on the office Mac\"" in out and "── Timing" in out


def test_steps_come_from_the_adhoc_workflow(tmp_path):
    config = (ROOT / "codemagic.yaml").read_text()
    helper = (ROOT / "native" / "ios" / "tools" / "codemagic_steps.py").read_text()
    names = mac_adhoc_deploy.write_steps(config, helper, tmp_path / "steps")
    assert "Export for ad hoc distribution" in names
    assert (tmp_path / "steps" / "index.tsv").is_file()
    assert (tmp_path / "steps" / "env.sh").is_file()


def test_watch_step_is_skipped_for_a_phone_only_build(tmp_path):
    config = ("workflows:\n  ios-native-adhoc:\n    environment:\n      vars:\n"
              "        FORKMESH_NATIVE_WATCH: \"0\"\n    scripts:\n"
              "      - name: watchOS platform\n        script: echo watch\n"
              "      - name: Archive\n        script: echo archive\n")
    helper = (ROOT / "native" / "ios" / "tools" / "codemagic_steps.py").read_text()
    assert mac_adhoc_deploy.write_steps(config, helper, tmp_path / "steps") == ["Archive"]


def test_build_record_matches_the_publisher():
    record = mac_adhoc_deploy.build_record("c" * 40, "subject", 100, 160, 1300)
    assert record["waitSeconds"] == 60 and record["buildSeconds"] == 1140
    assert record["buildStartedAt"] == 160


def test_shortcut_runs_the_tool_for_real():
    text = SHORTCUT.read_text()
    assert text.startswith("#!/usr/bin/env bash\n# name: ")
    assert "tools/mac_adhoc_deploy.py --execute" in text


def mac_shell(script, cwd, *args, timeout=60):
    return subprocess.run(["bash", "-c", script, "mac-step", *args], cwd=cwd, text=True,
                          capture_output=True, timeout=timeout)


def hold_lock(tmp_path, run):
    builder = subprocess.Popen(["bash", "-c", "sleep 300 & wait"], start_new_session=True)
    lock = tmp_path / "ci" / "lock"
    lock.mkdir(parents=True)
    (lock / "run").write_text(run + "\n")
    (lock / "pid").write_text(f"{builder.pid}\n")
    return builder


def test_a_new_run_cancels_the_older_ios_build(tmp_path):
    builder = hold_lock(tmp_path, "ci/aaaaaaaaaaaa-1")
    (tmp_path / "ci" / "ios-latest").write_text("ci/bbbbbbbbbbbb-2\n")
    try:
        result = mac_shell(mac_adhoc_deploy.LOCK_SCRIPT, tmp_path, "ci/bbbbbbbbbbbb-2", "60")
        assert builder.wait(timeout=10) is not None
    finally:
        if builder.poll() is None:
            builder.kill()
    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == ["cancelling the older iOS build ci/aaaaaaaaaaaa-1",
                                          "locked"]
    assert (tmp_path / "ci" / "lock" / "run").read_text() == "ci/bbbbbbbbbbbb-2\n"


def test_a_superseded_run_does_not_take_the_lock(tmp_path):
    builder = hold_lock(tmp_path, "ci/cccccccccccc-3")
    (tmp_path / "ci" / "ios-latest").write_text("ci/cccccccccccc-3\n")
    try:
        result = mac_shell(mac_adhoc_deploy.LOCK_SCRIPT, tmp_path, "ci/bbbbbbbbbbbb-2", "60")
        assert builder.poll() is None
    finally:
        builder.kill()
    assert result.stdout.splitlines() == ["superseded"]
    assert (tmp_path / "ci" / "lock" / "run").read_text() == "ci/cccccccccccc-3\n"


def test_a_release_build_is_waited_for_not_cancelled(tmp_path):
    builder = hold_lock(tmp_path, "desktop-release v1.2.5")
    (tmp_path / "ci" / "ios-latest").write_text("ci/bbbbbbbbbbbb-2\n")
    try:
        result = mac_shell(mac_adhoc_deploy.LOCK_SCRIPT, tmp_path, "ci/bbbbbbbbbbbb-2", "0")
        assert builder.poll() is None
    finally:
        builder.kill()
    assert result.returncode == 1
    assert "desktop-release v1.2.5" in result.stderr


def test_stopping_kills_the_build_and_releases_the_lock(tmp_path):
    builder = hold_lock(tmp_path, "ci/aaaaaaaaaaaa-1")
    try:
        result = mac_shell(mac_adhoc_deploy.STOP_SCRIPT, tmp_path, "ci/aaaaaaaaaaaa-1")
        assert builder.wait(timeout=10) is not None
    finally:
        if builder.poll() is None:
            builder.kill()
    assert result.returncode == 0, result.stderr
    assert not (tmp_path / "ci" / "lock").exists()


def test_stopping_leaves_another_runs_lock_alone(tmp_path):
    builder = hold_lock(tmp_path, "ci/cccccccccccc-3")
    try:
        mac_shell(mac_adhoc_deploy.STOP_SCRIPT, tmp_path, "ci/aaaaaaaaaaaa-1")
        assert builder.poll() is None
    finally:
        builder.kill()
    assert (tmp_path / "ci" / "lock" / "run").read_text() == "ci/cccccccccccc-3\n"


def test_the_stop_outlives_the_shortcut_being_killed():
    calls = []

    def run(command, **kwargs):
        calls.append((command, kwargs))

    mac_adhoc_deploy.stop_on_mac(FakeMac(), "ci/abc-1", run=run)
    command, kwargs = calls[0]
    assert "ci/abc-1" in command[1] and "killtree" in command[1]
    assert kwargs["start_new_session"] is True
    assert kwargs["stdout"] == kwargs["stderr"] == subprocess.DEVNULL


def test_the_build_runs_in_its_own_unit_with_a_clean_environment(tmp_path):
    environ = {"PATH": "/usr/bin", "FORKMESH_HOME": "/home/x/projects/forkmesh",
               "FORKMESH_AGENT_SCRATCH": "/scratch", "TMPDIR": "/scratch/tmp",
               "MAC_BUILD_GIT_URL": "https://example.test/r", "SECRET": "s"}
    command = mac_adhoc_deploy.detach_command("forkmesh-mac-adhoc-1", tmp_path / "b.log",
                                              ["--execute"], environ)
    assert command[:2] == ["systemd-run", "--user"]
    assert "--property=KillMode=mixed" in command
    assert "--property=IgnoreOnIsolate=yes" in command
    assert f"--setenv=FORKMESH_MAC_ADHOC_LOG={tmp_path / 'b.log'}" in command
    assert "--setenv=FORKMESH_HOME=/home/x/projects/forkmesh" in command
    assert "--setenv=MAC_BUILD_GIT_URL=https://example.test/r" in command
    assert not [arg for arg in command if "SCRATCH" in arg or "TMPDIR" in arg or "SECRET" in arg]
    assert command[-2:] == [str(Path(mac_adhoc_deploy.__file__).resolve()), "--execute"]


def test_the_shipped_main_comes_from_the_shared_git_directory():
    common = mac_adhoc_deploy.git_common_dir(ROOT)
    assert common.is_absolute() and (common / "HEAD").is_file()


def follow_with(tmp_path, signum=None, code="0"):
    log = tmp_path / "unit.log"
    log.write_text("==> Archive\n")
    (tmp_path / "unit.exit").write_text(code + "\n")
    polls, stops, out = [], [], io.StringIO()

    def active(unit):
        polls.append(unit)
        if signum is not None and len(polls) == 1:
            os.kill(os.getpid(), signum)
        return len(polls) < 3

    before = signal.getsignal(signal.SIGTERM)
    result = mac_adhoc_deploy.follow("unit", log, active=active, stop_build=stops.append,
                                     sleep=lambda _: None, out=out)
    assert signal.getsignal(signal.SIGTERM) is before
    return result, stops, out.getvalue()


def test_the_shortcut_shows_the_log_and_returns_the_build_status(tmp_path):
    result, stops, out = follow_with(tmp_path, code="1")
    assert (result, stops) == (1, [])
    assert "==> Archive" in out


def test_a_desktop_restart_leaves_the_build_running(tmp_path, capsys):
    for signum in (signal.SIGTERM, signal.SIGHUP):
        result, stops, _ = follow_with(tmp_path, signum)
        assert (result, stops) == (0, [])
    assert "keeps building" in capsys.readouterr().out


def test_ctrl_c_stops_the_build(tmp_path):
    result, stops, _ = follow_with(tmp_path, signal.SIGINT)
    assert stops == ["unit"]
    assert result == 0


def test_stop_ends_every_running_build(capsys):
    calls = []

    def run(args, **kwargs):
        calls.append(args)
        listing = "forkmesh-mac-adhoc-1-2.service loaded active running x\n"
        return subprocess.CompletedProcess(args, 0, listing if "list-units" in args else "", "")

    assert mac_adhoc_deploy.stop_units(run=run) == 0
    assert calls[-1] == ["systemctl", "--user", "stop", "forkmesh-mac-adhoc-1-2.service"]


def test_desktop_shortcut_builds_the_macos_app_for_real():
    text = DESKTOP_SHORTCUT.read_text()
    assert text.startswith("#!/usr/bin/env bash\n# name: ")
    assert "tools/mac_adhoc_deploy.py --desktop --execute" in text
    assert os.access(DESKTOP_SHORTCUT, os.X_OK)


class StreamingProcess:
    def __init__(self, lines, code):
        self.stdout = io.BytesIO(b"".join(lines))
        self.code = code

    def wait(self):
        return self.code

    def poll(self):
        return self.code


def test_the_desktop_build_streams_its_log_live(tmp_path):
    lines = [b"[macos-universal] build\n", b"a" * 64 + b"  /Users/build/x.dmg\n"]
    commands = []

    def popen(command, **kwargs):
        commands.append(command)
        return StreamingProcess(lines, 0)

    out = io.StringIO()
    log = tmp_path / "mac-build.log"
    code = mac_adhoc_deploy.stream(FakeMac(), "build", log, popen=popen, out=out)
    assert code == 0
    assert commands == [["ssh", "build"]]
    assert out.getvalue() == log.read_text()
    assert mac_adhoc_deploy.RELEASE.mac_dmg_result(log.read_text())[1] == "/Users/build/x.dmg"


def test_stopping_a_desktop_build_kills_it_and_releases_the_lock(tmp_path):
    run = mac_adhoc_deploy.DESKTOP_LOCK + "aaaaaaaaaaaa"
    builder = hold_lock(tmp_path, run)
    try:
        result = mac_shell(mac_adhoc_deploy.STOP_SCRIPT, tmp_path, run)
        assert builder.wait(timeout=10) is not None
    finally:
        if builder.poll() is None:
            builder.kill()
    assert result.returncode == 0, result.stderr
    assert not (tmp_path / "ci" / "lock").exists()


def test_app_store_shortcut_submits_for_review_for_real():
    text = STORE_SHORTCUT.read_text()
    assert text.startswith("#!/usr/bin/env bash\n# name: ")
    assert "tools/mac_adhoc_deploy.py --app-store --execute" in text
    assert os.access(STORE_SHORTCUT, os.X_OK)


def test_app_store_steps_come_from_the_release_workflow_with_submission_on(tmp_path):
    config = (ROOT / "codemagic.yaml").read_text()
    helper = (ROOT / "native" / "ios" / "tools" / "codemagic_steps.py").read_text()
    names = mac_adhoc_deploy.write_steps(
        config, helper, tmp_path / "steps", mac_adhoc_deploy.STORE_WORKFLOW,
        mac_adhoc_deploy.store_overrides("Fixes 'quotes' and $HOME."))
    assert mac_adhoc_deploy.STORE_SUBMIT_STEP in names
    assert "Upload to App Store Connect" in names
    assert "watchOS platform" in names
    shown = subprocess.run(
        ["bash", "-c", '. "$1"; printf "%s|%s" "$FORKMESH_IOS_APP_STORE" '
                       '"$FORKMESH_IOS_WHATS_NEW"', "env", str(tmp_path / "steps" / "env.sh")],
        capture_output=True, text=True, check=True).stdout
    assert shown == "1|Fixes 'quotes' and $HOME."


def test_an_ad_hoc_run_waits_for_an_app_store_submission(tmp_path):
    builder = hold_lock(tmp_path, mac_adhoc_deploy.STORE_LOCK + "aaaaaaaaaaaa-1")
    (tmp_path / "ci" / "ios-latest").write_text("ci/bbbbbbbbbbbb-2\n")
    try:
        result = mac_shell(mac_adhoc_deploy.LOCK_SCRIPT, tmp_path, "ci/bbbbbbbbbbbb-2", "0")
        assert builder.poll() is None
    finally:
        builder.kill()
    assert result.returncode == 1
    assert mac_adhoc_deploy.STORE_LOCK in result.stderr


class StoreMac:
    address = "192.0.2.7"

    def __init__(self, submitted="Submitted 1.4.2 (31) for App Review: abc; it goes live"):
        self.calls = []
        self.submitted = submitted

    def run(self, script, *args, stdin=None, timeout=600, lock="", **kwargs):
        self.calls.append((script, args, lock))
        if script == mac_adhoc_deploy.RELEASE.MAC_IOS_FETCH_SCRIPT:
            return "checked out abc: subject\n"
        if script.startswith("git -C ci/tree log"):
            return "Merge x into main\n"
        if script == "cat ci/tree/codemagic.yaml":
            return (ROOT / "codemagic.yaml").read_text()
        if script.startswith("cat ci/tree/native/ios/tools/codemagic_steps.py"):
            return (ROOT / "native" / "ios" / "tools" / "codemagic_steps.py").read_text()
        if script == mac_adhoc_deploy.RELEASE.MAC_IOS_STEPS_SCRIPT:
            stdin.read()
            return ""
        if "cm_env" in script:
            return "FORKMESH_IOS_VERSION=1.4.2\nFORKMESH_IOS_BUILD=31\n"
        if script == mac_adhoc_deploy.STORE_SUBMITTED_SCRIPT:
            return self.submitted
        return ""


def test_app_store_build_runs_under_its_own_lock_and_records_the_submission(tmp_path,
                                                                           monkeypatch):
    followed = []
    monkeypatch.setattr(mac_adhoc_deploy, "follow_build",
                        lambda mac, run_dir, lock: followed.append((run_dir, lock)) or 0)
    mac = StoreMac()
    record = mac_adhoc_deploy.submit_on_mac(mac, "u", "c" * 40, "Fixes.", tmp_path)
    assert (record["version"], record["build"], record["whatsNew"]) == ("1.4.2", "31", "Fixes.")
    run_dir, lock = followed[0]
    assert run_dir.startswith("release/ios-main-cccccccccccc-")
    assert lock.startswith(mac_adhoc_deploy.STORE_LOCK) and not lock.startswith("ci/")
    scripts = [call[0] for call in mac.calls]
    assert scripts[0] == mac_adhoc_deploy.RELEASE.MAC_LOCK_SCRIPT
    assert scripts[-1] == mac_adhoc_deploy.RELEASE.MAC_UNLOCK_SCRIPT
    steps = [call for call in mac.calls if call[0] == mac_adhoc_deploy.RELEASE.MAC_IOS_STEPS_SCRIPT]
    assert steps == [(mac_adhoc_deploy.RELEASE.MAC_IOS_STEPS_SCRIPT, (run_dir,), lock)]
    assert len(list((tmp_path / "app-store").glob("*-cccccccccccc.json"))) == 1


def test_app_store_build_without_a_submission_fails(tmp_path, monkeypatch):
    monkeypatch.setattr(mac_adhoc_deploy, "follow_build", lambda mac, run_dir, lock: 0)
    with pytest.raises(ToolError, match="without an App Store submission"):
        mac_adhoc_deploy.submit_on_mac(StoreMac(submitted=""), "u", "c" * 40, "x", tmp_path)


def sparse_patterns(script):
    line = script.split("git sparse-checkout set --no-cone ", 1)[1].split("\nattempt=", 1)[0]
    return [part.strip("'") for part in line.replace("\\\n", " ").split()]


def repo_paths_the_core_tests_read():
    core = ROOT / "native/ios/ForkMeshCore"
    constants = {}
    for source in (core / "Sources").rglob("*.swift"):
        text = source.read_text(encoding="utf-8")
        for match in re.finditer(r'static let (\w+)\s*(?::\s*String)?\s*=\s*"([^"\\]+)"', text):
            owner = re.findall(r"\b(?:enum|struct|class|extension)\s+(\w+)", text[:match.start()])
            if owner:
                constants[f"{owner[-1]}.{match.group(1)}"] = match.group(2)
    tests = {test: test.read_text(encoding="utf-8") for test in (core / "Tests").rglob("*.swift")}
    helpers = {name for text in tests.values() if "fileURLWithPath: #file" in text
               for name in re.findall(r"\b(?:func|var|let)\s+(\w+)", text)}
    paths = set()
    for text in tests.values():
        if "fileURLWithPath: #file" not in text and not any(re.search(rf"\b{name}\(", text) for name in helpers):
            continue
        for match in re.finditer(r'\w\(\s*(?:"([^"]*)"|([A-Z]\w*\.\w+)\))', text):
            path = (match.group(1) or constants.get(match.group(2), "")).split("\\(", 1)[0]
            if "/" in path.strip("/") and not path.startswith(("/", ".")) and (ROOT / path).exists():
                paths.add(path.rstrip("/"))
    return paths


def test_the_mac_checkout_holds_every_file_the_core_tests_read():
    patterns = sparse_patterns(mac_adhoc_deploy.RELEASE.MAC_IOS_FETCH_SCRIPT)
    found = repo_paths_the_core_tests_read()
    for path in ["app/src/apns.py", "app/src/app_runtime.py", "conformance/chat/v4/vectors.json",
                 "docs/native/PERFORMANCE_CHECKS.md", "docs/marketplace/video-mix-dj-studio-performance.md",
                 "marketplace/packages/secure-drive/tests/fixtures"]:
        assert path in found, path
    for path in sorted(found | {"native/ios/ForkMeshCore/Package.swift", "codemagic.yaml"}):
        assert any(pattern.rstrip("/") == f"/{path}" or (pattern.endswith("/") and f"/{path}".startswith(pattern))
                   for pattern in patterns), path


def fake_tool(tmp_path, name):
    tool = tmp_path / "bin" / name
    tool.parent.mkdir(exist_ok=True)
    tool.symlink_to(shutil.which("sleep"))
    return started(subprocess.Popen([str(tool), "300"], start_new_session=True), name)


def started(process, name):
    for _ in range(100):
        shown = subprocess.run(["ps", "-ww", "-o", "args=", "-p", str(process.pid)],
                               capture_output=True, text=True).stdout
        if name in shown:
            return process
        time.sleep(0.05)
    process.kill()
    raise AssertionError(f"{name} did not start")


def stray_follower(tmp_path):
    log = tmp_path / "ci" / "aaaaaaaaaaaa-1" / "jobs" / "all" / "log"
    log.parent.mkdir(parents=True, exist_ok=True)
    log.write_text("==> Archive (04:22:00)\nCompileSwift [ForkMesh]\n")
    return started(subprocess.Popen(["tail", "-F", str(log)], stdout=subprocess.DEVNULL,
                                    start_new_session=True), "tail")


def test_unstick_kills_every_build_process_and_releases_any_lock(tmp_path):
    builder = hold_lock(tmp_path, "ios-app-store aaaaaaaaaaaa-1")
    tool = fake_tool(tmp_path, "fm-test-xctest")
    follower = stray_follower(tmp_path)
    try:
        result = mac_shell(mac_adhoc_deploy.UNSTICK_SCRIPT, tmp_path, "fm-test-xctest")
        for process in (builder, tool, follower):
            assert process.wait(timeout=10) is not None
    finally:
        for process in (builder, tool, follower):
            if process.poll() is None:
                process.kill()
    assert result.returncode == 0, result.stderr
    assert f"stopping fm-test-xctest (pid {tool.pid}," in result.stdout
    assert f"stopping tail (pid {follower.pid}," in result.stdout
    assert "releasing the build lock held by ios-app-store aaaaaaaaaaaa-1" in result.stdout
    assert result.stdout.rstrip().endswith("the office Mac is idle")
    assert not (tmp_path / "ci" / "lock").exists()


def test_unstick_on_an_idle_mac_changes_nothing(tmp_path):
    result = mac_shell(mac_adhoc_deploy.UNSTICK_SCRIPT, tmp_path, "fm-test-xctest")
    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == ["the build lock was free", "the office Mac is idle"]


def test_status_shows_the_lock_the_newest_log_and_build_processes(tmp_path):
    builder = hold_lock(tmp_path, "ci/aaaaaaaaaaaa-1")
    tool = fake_tool(tmp_path, "fm-test-xctest")
    follower = stray_follower(tmp_path)
    try:
        result = mac_shell(mac_adhoc_deploy.STATUS_SCRIPT, tmp_path, "fm-test-xctest")
    finally:
        for process in (builder, tool, follower):
            process.kill()
    assert result.returncode == 0, result.stderr
    lines = result.stdout.splitlines()
    assert lines[0] == (f"lock: held by ci/aaaaaaaaaaaa-1 for 0 min, "
                        f"build process {builder.pid} running")
    assert lines[1] == ("newest build log: ~/ci/aaaaaaaaaaaa-1/jobs/all/log, "
                        "last written 0 min ago")
    assert "  | CompileSwift [ForkMesh]" in lines
    assert any(line.startswith(f"  {tool.pid} ") and line.endswith(" fm-test-xctest")
               for line in lines)
    assert any(line.startswith(f"  {follower.pid} ") and " tail" in line for line in lines)
    assert str(builder.pid) not in " ".join(lines[2:])


def test_status_on_an_idle_mac(tmp_path):
    result = mac_shell(mac_adhoc_deploy.STATUS_SCRIPT, tmp_path, "fm-test-xctest")
    assert result.stdout.splitlines() == ["lock: free", "build processes: none"]


def test_the_deadline_kills_what_the_command_started_in_its_own_session(tmp_path):
    marker = tmp_path / "grandchild"
    script = ROOT / "native" / "ios" / "tools" / "run_with_deadline.sh"
    result = subprocess.run(
        ["bash", str(script), "1", "bash", "-c",
         f"setsid sleep 300 & echo $! > {marker}; sleep 300"],
        capture_output=True, text=True, timeout=30)
    grandchild = int(marker.read_text())
    assert result.returncode != 0
    assert "ERROR: bash ran past 1 seconds" in result.stderr
    with pytest.raises(ProcessLookupError):
        os.kill(grandchild, 0)


def test_the_deadline_passes_the_commands_exit_code_through():
    script = ROOT / "native" / "ios" / "tools" / "run_with_deadline.sh"
    result = subprocess.run(["bash", str(script), "60", "bash", "-c", "echo hi; exit 3"],
                            capture_output=True, text=True, timeout=30)
    assert (result.returncode, result.stdout, result.stderr) == (3, "hi\n", "")


def test_core_tests_stop_every_process_at_the_deadline():
    config = (ROOT / "codemagic.yaml").read_text()
    assert "bash ../tools/run_with_deadline.sh 3600 swift test" in config
    assert "alarm 3600" not in config


def test_a_fresh_build_kills_leftover_build_tools_first():
    script = (ROOT / "native" / "ios" / "tools" / "mac_adhoc_build.sh").read_text()
    assert script.index("  sweep_orphans\n") < script.index("follow_logs 2>/dev/null &")
    assert '[ "$ppid" = 1 ] || continue' in script


@pytest.mark.parametrize("shortcut,flag", [
    ("office-mac-build-status.sh", "--status"),
    ("unstick-office-mac-build.sh", "--unstick"),
])
def test_status_and_unstick_shortcuts(shortcut, flag):
    text = (ROOT / ".forkmesh" / "shortcuts" / shortcut).read_text()
    assert text.startswith("#!/usr/bin/env bash\n# name: ")
    assert f"tools/mac_adhoc_deploy.py {flag}" in text


def test_app_store_status_shortcut_uses_the_store_status_helper():
    text = (ROOT / ".forkmesh" / "shortcuts" / "ios-app-store-status.sh").read_text()
    assert text.startswith("#!/usr/bin/env bash\n# name: ")
    assert 'tools/mac_adhoc_deploy.py --store-status "$@"' in text


def test_unstick_stops_the_units_before_cleaning_the_mac(monkeypatch, capsys):
    calls = []

    class Mac:
        address = "build.test"

        def run(self, script, *args, timeout):
            calls.append(("mac", script, args))
            return "the office Mac is idle\n"

    monkeypatch.setattr(mac_adhoc_deploy, "stop_units", lambda: calls.append(("units",)))
    monkeypatch.setattr(mac_adhoc_deploy, "find_mac", Mac)
    assert mac_adhoc_deploy.main(["--unstick"]) == 0
    assert calls == [("units",), ("mac", mac_adhoc_deploy.UNSTICK_SCRIPT,
                                  (mac_adhoc_deploy.BUILD_TOOLS,))]
    assert "the office Mac is idle" in capsys.readouterr().out


class IdsMac:
    address = "build.test"

    def __init__(self, answer="NAU34ZR27B\nissuer-uuid\n"):
        self.answer = answer

    def run(self, script, *args, timeout):
        assert script == mac_adhoc_deploy.ASC_IDS_SCRIPT
        return self.answer


def test_store_status_key_ids_come_from_the_environment_first():
    def no_mac():
        raise AssertionError("the Mac was asked")

    environ = {"ASC_KEY_ID": "K", "ASC_ISSUER_ID": "I"}
    assert mac_adhoc_deploy.asc_ids(environ, no_mac) == {"ASC_KEY_ID": "K", "ASC_ISSUER_ID": "I"}


def test_store_status_reads_missing_key_ids_from_the_office_mac():
    assert mac_adhoc_deploy.asc_ids({"ASC_KEY_ID": "K"}, IdsMac) == {
        "ASC_KEY_ID": "NAU34ZR27B", "ASC_ISSUER_ID": "issuer-uuid"}
    with pytest.raises(ToolError):
        mac_adhoc_deploy.asc_ids({}, lambda: IdsMac(""))


def test_store_status_reads_key_ids_from_desktop_actions_variables(tmp_path, monkeypatch):
    config = tmp_path / "ForkMesh"
    config.mkdir()
    (config / "ForkMesh.conf").write_text(
        '[actions]\nvariables="@ByteArray({\\\"ASC_KEY_ID\\\":\\\"K\\\",\\\"ASC_ISSUER_ID\\\":\\\"I\\\"})"\n'
    )
    monkeypatch.setenv("HOME", str(tmp_path))
    monkeypatch.setenv("FORKMESH_CONFIG_DIR", str(tmp_path))
    monkeypatch.setenv("XDG_CONFIG_HOME", str(tmp_path / "config"))
    assert mac_adhoc_deploy.asc_ids({}, lambda: (_ for _ in ()).throw(AssertionError())) == {
        "ASC_KEY_ID": "K", "ASC_ISSUER_ID": "I"}


def test_store_status_runs_the_report_with_the_key_ids(tmp_path, monkeypatch):
    keys = tmp_path / ".appstoreconnect" / "private_keys"
    keys.mkdir(parents=True)
    monkeypatch.setattr(mac_adhoc_deploy.Path, "home", lambda: tmp_path)
    monkeypatch.setenv("ASC_KEY_ID", "K")
    monkeypatch.setenv("ASC_ISSUER_ID", "I")
    runs = []

    def run(args, env):
        runs.append((args, env["ASC_KEY_ID"], env["ASC_ISSUER_ID"]))
        return subprocess.CompletedProcess(args, 0)

    assert mac_adhoc_deploy.store_status(run) == 1
    assert runs == []
    (keys / "AuthKey_K.p8").write_text("key")
    assert mac_adhoc_deploy.store_status(run) == 0
    assert runs[0][0][-2:] == [str(mac_adhoc_deploy.STORE_REPORT), "--publish"]
    assert runs[0][1:] == ("K", "I")
    assert mac_adhoc_deploy.STORE_REPORT.is_file()


def test_store_status_passes_the_requested_feedback_count(tmp_path, monkeypatch):
    keys = tmp_path / ".appstoreconnect" / "private_keys"
    keys.mkdir(parents=True)
    (keys / "AuthKey_K.p8").write_text("key")
    monkeypatch.setattr(mac_adhoc_deploy.Path, "home", lambda: tmp_path)
    monkeypatch.setenv("ASC_KEY_ID", "K")
    monkeypatch.setenv("ASC_ISSUER_ID", "I")
    commands = []

    def run(args, env):
        commands.append(args)
        return subprocess.CompletedProcess(args, 0)

    assert mac_adhoc_deploy.store_status(run, 2) == 0
    assert commands == [[mac_adhoc_deploy.sys.executable, "-u", str(mac_adhoc_deploy.STORE_REPORT),
                         "--publish", "--feedback", "2"]]
