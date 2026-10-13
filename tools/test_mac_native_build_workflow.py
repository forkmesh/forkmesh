import os
import re
import shutil
import signal
import subprocess
import time
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[1]
WORKFLOW = ROOT / ".forkmesh" / "mac-native-build.yml"
BUILD_STEPS = slice(3, -1)


def workflow():
    return yaml.safe_load(WORKFLOW.read_text())


def codemagic_step_names():
    config = yaml.safe_load((ROOT / "codemagic.yaml").read_text())
    scripts = config["workflows"]["ios-native-adhoc"]["scripts"]
    return [step.get("name", "") if isinstance(step, dict) else "" for step in scripts]


def qt_wildcard(pattern):
    return re.compile("".join("[^/]*" if c == "*" else re.escape(c) for c in pattern) + r"\Z")


def test_one_workflow_step_per_codemagic_step():
    steps = workflow()["jobs"]["mac"]["steps"]
    names = [step["name"] for step in steps[BUILD_STEPS]]
    assert names == codemagic_step_names()
    for step in steps[BUILD_STEPS]:
        assert 'mac_build "%s"' % step["name"] in step["run"]
    assert 'mac_build ""' in steps[-1]["run"]


def test_runs_without_a_local_checkout():
    assert workflow()["checkout"] is False


def test_push_paths_cover_every_native_ios_file():
    patterns = [qt_wildcard(p) for p in (workflow().get("on") or workflow()[True])["push"]["paths"]]
    files = subprocess.run(["git", "ls-files", "native/ios", "codemagic.yaml"], cwd=ROOT,
                           capture_output=True, text=True, check=True).stdout.split()
    assert files
    missed = [f for f in files if not any(p.match(f) for p in patterns)]
    assert missed == []


def test_step_scripts_parse():
    for step in workflow()["jobs"]["mac"]["steps"]:
        result = subprocess.run(["bash", "-n"], input=step["run"], text=True,
                                capture_output=True)
        assert result.returncode == 0, (step["name"], result.stderr)
        assert not any(line.lstrip().startswith("#") for line in step["run"].splitlines())


def build_number_script():
    config = yaml.safe_load((ROOT / "codemagic.yaml").read_text())
    for step in config["workflows"]["ios-native-adhoc"]["scripts"]:
        if isinstance(step, dict) and step.get("name") == "Pick the ad hoc build number":
            return step["script"]
    raise AssertionError("no build number step")


def pick(tmp_path, latest, pinned, last=None):
    work = tmp_path / ("run-%s-%s-%s" % (latest, pinned, last))
    (work / "tools").mkdir(parents=True)
    (work / "Config").mkdir()
    (work / "tools" / "asc_bundle_ids.py").write_text("print(%d)\n" % latest)
    (work / "Config" / "Version.xcconfig").write_text(
        "MARKETING_VERSION = 1.4.0\nCURRENT_PROJECT_VERSION = %d\n" % pinned)
    counter = work / "counter"
    if last is not None:
        counter.write_text("%d\n" % last)
    env = {"PATH": "/usr/bin:/bin", "FORKMESH_ADHOC_BUILD_FILE": str(counter)}
    subprocess.run(["bash", "-c", build_number_script()], cwd=work, env=env, check=True,
                   capture_output=True, text=True)
    version = (work / "Config" / "Version.xcconfig").read_text()
    number = int(re.search(r"^CURRENT_PROJECT_VERSION = (\d+)$", version, re.M).group(1))
    assert counter.read_text().strip() == str(number)
    return number


def test_every_ad_hoc_build_gets_a_new_build_number(tmp_path):
    assert pick(tmp_path, latest=17, pinned=17) == 18
    assert pick(tmp_path, latest=17, pinned=17, last=18) == 19
    assert pick(tmp_path, latest=25, pinned=17, last=18) == 26
    assert pick(tmp_path, latest=17, pinned=40, last=18) == 40


def archive_script():
    config = yaml.safe_load((ROOT / "codemagic.yaml").read_text())
    for step in config["workflows"]["ios-native-adhoc"]["scripts"]:
        if isinstance(step, dict) and step.get("name") == "Archive (signing per FORKMESH_NATIVE_SIGNING)":
            return step["script"]
    raise AssertionError("no archive step")


def archive_args(tmp_path, signing, cache):
    work = tmp_path / ("archive-%s-%s" % (signing, cache))
    bin_dir = work / "bin"
    bin_dir.mkdir(parents=True)
    xcodebuild = bin_dir / "xcodebuild"
    xcodebuild.write_text('#!/bin/bash\nprintf "%s\\n" "$@" > "$CM_BUILD_DIR/args"\n'
                          'mkdir -p "$CM_BUILD_DIR/forkmesh-native.xcarchive/Products/Applications/ForkMesh.app"\n')
    xcodebuild.chmod(0o755)
    env = {"PATH": "%s:/usr/bin:/bin" % bin_dir, "HOME": str(work), "CM_BUILD_DIR": str(work),
           "TMPDIR": str(work), "FORKMESH_NATIVE_SIGNING": signing,
           "ASC_API_KEY_P8": "key", "ASC_KEY_ID": "id", "ASC_ISSUER_ID": "issuer"}
    if cache is not None:
        env["FORKMESH_NATIVE_COMPILE_CACHE"] = cache
    subprocess.run(["bash", "-c", archive_script()], cwd=work, env=env, check=True,
                   capture_output=True, text=True)
    return (work / "args").read_text().splitlines()


def test_archive_uses_the_compilation_cache_only_when_asked(tmp_path):
    for signing in ("manual", "xcode"):
        assert "COMPILATION_CACHE_ENABLE_CACHING=YES" in archive_args(tmp_path, signing, "1")
        assert "COMPILATION_CACHE_ENABLE_CACHING=YES" not in archive_args(tmp_path, signing, None)
        assert "" not in archive_args(tmp_path, signing, "0")


def test_the_mac_turns_the_compilation_cache_on():
    script = (ROOT / "native" / "ios" / "tools" / "mac_adhoc_build.sh").read_text()
    assert 'export FORKMESH_NATIVE_COMPILE_CACHE="${FORKMESH_NATIVE_COMPILE_CACHE:-1}"' in script


def job_home(tmp_path):
    home = tmp_path / "home"
    tools = home / "tools"
    tools.mkdir(parents=True)
    shutil.copy(ROOT / "native" / "ios" / "tools" / "mac_adhoc_job.sh", tools)
    (tools / "mac_adhoc_build.sh").write_text(
        'echo "first $4"\n'
        'while [ ! -e "$HOME/go" ]; do sleep 0.1; done\n'
        'echo second\n'
        'exit 3\n')
    steps = home / "ci" / "abc-1" / "steps"
    steps.mkdir(parents=True)
    (steps / "index.tsv").write_text("00.sh\tPick\n01.sh\tArchive\n")
    (home / "ci" / "lock").mkdir()
    (home / "ci" / "lock" / "run").write_text("ci/abc-1\n")
    env = {"HOME": str(home), "PATH": "/usr/bin:/bin", "XDG_DATA_HOME": str(home),
           "XDG_CONFIG_HOME": str(home), "XDG_CACHE_HOME": str(home)}
    return home, ["bash", str(tools / "mac_adhoc_job.sh"), "ci/abc-1"], env


def wait_for(check, timeout=20):
    deadline = time.monotonic() + timeout
    while not check():
        assert time.monotonic() < deadline
        time.sleep(0.1)


def test_a_build_step_keeps_running_when_its_connection_drops(tmp_path):
    home, job, env = job_home(tmp_path)
    log = home / "ci" / "abc-1" / "jobs" / "2" / "log"
    follower = subprocess.Popen(job + ["Archive"], cwd=home, env=env, start_new_session=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    wait_for(lambda: log.exists() and "first Archive" in log.read_text())
    os.killpg(follower.pid, signal.SIGKILL)
    follower.wait()
    pid = int((home / "ci" / "abc-1" / "jobs" / "2" / "pid").read_text())
    assert (home / "ci" / "lock" / "pid").read_text().strip() == str(pid)
    os.kill(pid, 0)
    assert not (home / "ci" / "abc-1" / "jobs" / "2" / "status").exists()
    (home / "go").touch()
    result = subprocess.run(job + ["Archive"], cwd=home, env=env, capture_output=True,
                            text=True, timeout=30)
    assert result.returncode == 3, result.stdout + result.stderr
    assert "Reconnected" in result.stdout
    assert "first Archive\nsecond\n" in result.stdout
    again = subprocess.run(job + ["Archive"], cwd=home, env=env, capture_output=True,
                           text=True, timeout=30)
    assert again.returncode == 3 and "first Archive\nsecond\n" in again.stdout


def test_a_build_job_that_died_without_a_status_fails(tmp_path):
    home, job, env = job_home(tmp_path)
    dead = subprocess.Popen(["true"])
    dead.wait()
    stale = home / "ci" / "abc-1" / "jobs" / "1"
    stale.mkdir(parents=True)
    (stale / "log").write_text("half a build\n")
    (stale / "pid").write_text("%d\n" % dead.pid)
    result = subprocess.run(job + ["Pick"], cwd=home, env=env, capture_output=True, text=True,
                            timeout=30)
    assert result.returncode == 1
    assert "half a build" in result.stdout
    assert "stopped without an exit status" in result.stderr


def test_an_unknown_step_is_refused(tmp_path):
    home, job, env = job_home(tmp_path)
    result = subprocess.run(job + ["Nope"], cwd=home, env=env, capture_output=True, text=True,
                            timeout=30)
    assert result.returncode == 1 and "no step named" in result.stderr
    assert not (home / "ci" / "abc-1" / "jobs" / "all").exists()
