import argparse
import base64
import importlib.util
import json
import re
import shlex
import subprocess
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
SPEC = importlib.util.spec_from_file_location(
    "forkmesh_release_host", HERE / "forkmesh-release-host.py")
release_host = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(release_host)

X86 = "a" * 64
ARM = "b" * 64


def manifest(tag="v1.2.8", assets=None):
    if assets is None:
        assets = [{"name": "forkmesh-mirror-node-linux-x86_64", "blob_sha256": X86},
                  {"name": "forkmesh-linux-x86_64", "blob_sha256": "c" * 64}]
    return {"schema": "forkmesh-release-v2", "tag": tag, "assets": assets}


def test_rollout_release_picks_the_mirror_node_blobs():
    release = release_host.rollout_release(manifest(assets=[
        {"name": "forkmesh-mirror-node-linux-x86_64", "blob_sha256": X86.upper()},
        {"name": "forkmesh-mirror-node-linux-arm64", "blob_sha256": ARM},
        {"name": "forkmesh-linux-x86_64", "blob_sha256": "not-checked"}]))
    assert release == {"tag": "v1.2.8", "version": "1.2.8", "x86_64": X86, "arm64": ARM}


@pytest.mark.parametrize("bad", [
    None,
    {**manifest(), "schema": "forkmesh-release-v1"},
    manifest(tag="1.2.8"),
    manifest(tag="v1.2"),
    manifest(assets=[{"name": "forkmesh-linux-x86_64", "blob_sha256": X86}]),
    manifest(assets=[{"name": "forkmesh-mirror-node-linux-x86_64", "blob_sha256": "zz"}]),
    manifest(assets=[{"name": "forkmesh-mirror-node-linux-x86_64", "blob_sha256": X86},
                     {"name": "forkmesh-mirror-node-linux-x86_64", "blob_sha256": ARM}]),
    manifest(assets=["forkmesh-mirror-node-linux-x86_64"]),
])
def test_rollout_release_refuses_an_unusable_manifest(bad):
    with pytest.raises(release_host.CompletionError):
        release_host.rollout_release(bad)


def test_rollout_hosts_leave_next_cores_to_next_fleet():
    hosts = [("mirror40", "10.0.0.1"), ("fmnext-n1", "10.0.0.2"), ("fmnext-r1", "10.0.0.3")]
    assert release_host.rollout_hosts(hosts) == [("mirror40", "10.0.0.1")]


def decoded_remote(command):
    script = re.search(r"printf %s ([A-Za-z0-9+/=]+) \| base64 -d", command).group(1)
    return base64.b64decode(script).decode("utf-8")


def test_rollout_command_carries_the_release_and_a_parseable_script(tmp_path):
    release = release_host.rollout_release(manifest())
    command = release_host.rollout_remote_command(
        release, "https://example.test/blob/sha256/", "/srv/repo.git/forkmesh-releases", 30)
    env = dict(word.split("=", 1) for word in shlex.split(command.split(" bash -c ")[0]))
    assert env == {"X86": X86, "ARM": "", "VERSION": "1.2.8",
                   "BLOB_BASE": "https://example.test/blob/sha256/",
                   "STORE": "/srv/repo.git/forkmesh-releases", "READY_WAIT": "30"}
    script = decoded_remote(command)
    assert script == release_host.ROLLOUT_REMOTE
    (tmp_path / "remote.sh").write_text(script)
    assert subprocess.run(["bash", "-n", str(tmp_path / "remote.sh")]).returncode == 0


def test_remote_script_order_backs_up_before_swapping_and_checks_the_config_dir_first():
    script = release_host.ROLLOUT_REMOTE
    current = script.index('echo "rollout: current')
    writable = script.index("runuser -u forkmesh-node -- test -w")
    backup = script.index('cp -p -- "$bin" "$bin.bak-update"')
    swap = script.index('mv -f -- "$candidate" "$bin"')
    set_version = script.index('--set-version "$VERSION"')
    restart = script.index("systemctl restart forkmesh-mirror-node.service")
    assert current < writable < backup < swap < set_version < restart
    assert "next-managed" in script.split("\n[ -f \"$unit\" ] && [ ! -L")[0]


@pytest.mark.parametrize("code,out,err,expected", [
    (0, "noise\nrollout: deployed 1.2.8 sha256 %s from store\n" % X86, "",
     ("deployed", "deployed 1.2.8 sha256 %s from store" % X86)),
    (0, "rollout: current 1.2.8\n", "", ("current", "current 1.2.8")),
    (0, "rollout: next-managed\n", "", ("next-managed", "next-managed")),
    (0, "something else\n", "", ("failed", "exit 0: something else")),
    (73, "", "rollout: --set-version failed\n", ("failed", "exit 73: rollout: --set-version failed")),
    (255, "", "", ("failed", "exit 255: no output")),
])
def test_rollout_outcome(code, out, err, expected):
    assert release_host.rollout_outcome(code, out, err) == expected


def test_state_skips_settled_hosts_and_retries_failed_or_moved_ones(tmp_path):
    path = tmp_path / "rollout-v1.2.8.json"
    path.write_text(json.dumps({"tag": "v1.2.7", "hosts": {"m1": {}}}))
    state = release_host.load_rollout_state(path, "v1.2.8")
    assert state == {"tag": "v1.2.8", "attempts": 0, "last_attempt": 0, "hosts": {}}
    hosts = [("m1", "10.0.0.1"), ("m2", "10.0.0.2"), ("m3", "10.0.0.3")]
    state = release_host.record_rollout(state, {
        "m1": ("10.0.0.1", "deployed", "deployed 1.2.8"),
        "m2": ("10.0.0.2", "failed", "exit 70: download failed"),
        "m3": ("10.0.0.9", "current", "current 1.2.8")}, now=1000)
    assert state["attempts"] == 1 and state["last_attempt"] == 1000
    assert release_host.rollout_targets(state, hosts) == [("m2", "10.0.0.2"),
                                                          ("m3", "10.0.0.3")]
    state = release_host.record_rollout(state, {
        "m2": ("10.0.0.2", "current", "current 1.2.8"),
        "m3": ("10.0.0.3", "deployed", "deployed 1.2.8")}, now=2000)
    assert state["attempts"] == 0
    assert release_host.rollout_targets(state, hosts) == []


def test_rollout_due_backs_off_then_gives_up_unless_forced():
    backoff = release_host.FAILURE_BACKOFF_SECONDS
    assert release_host.rollout_due({"attempts": 0}, 0) == ""
    assert release_host.rollout_due({"attempts": 1, "last_attempt": 100}, 100 + backoff - 1) \
        == "failed recently"
    assert release_host.rollout_due({"attempts": 1, "last_attempt": 100}, 100 + backoff) == ""
    exhausted = {"attempts": release_host.ROLLOUT_MAX_ATTEMPTS, "last_attempt": 0}
    assert release_host.rollout_due(exhausted, 10 ** 9).startswith("gave up")
    assert release_host.rollout_due(exhausted, 10 ** 9, forced=True) == ""


def test_roll_out_hosts_runs_each_host_as_root_with_the_seeding_key(tmp_path):
    calls = []

    def runner(command, **kwargs):
        calls.append((command, kwargs))
        if command[-2] == "root@10.0.0.2":
            raise subprocess.TimeoutExpired(command, 1)
        return subprocess.CompletedProcess(command, 0, "rollout: current 1.2.8\n", "")

    targets = release_host.seed_targets([("m1", "10.0.0.1"), ("m2", "10.0.0.2")],
                                        tmp_path / "vultr")
    results = release_host.roll_out_hosts(targets, "REMOTE", runner=runner)
    assert results["m1"] == ("10.0.0.1", "current", "current 1.2.8")
    assert results["m2"][1] == "failed"
    command, kwargs = next(c for c in calls if c[0][-2] == "root@10.0.0.1")
    assert command[-1] == "REMOTE" and str(tmp_path / "vultr") in command
    assert "BatchMode=yes" in command and kwargs["stdin"] is subprocess.DEVNULL


def step_args(tmp_path, **overrides):
    hosts = tmp_path / "hosts"
    hosts.write_text("# managed mirrors\nm1 10.0.0.1\nfmnext-n1 10.0.0.2\n")
    values = dict(repo=str(tmp_path), work_dir=str(tmp_path / "work"), hosts_file=str(hosts),
                  ssh_key=str(tmp_path / "key"), tag=None, rollout_only=False, dry_run=False,
                  rollout_blob_base="https://example.test/b/",
                  remote_repository="/srv/repo.git")
    values.update(overrides)
    (tmp_path / "work").mkdir(exist_ok=True)
    return argparse.Namespace(**values)


def test_roll_out_release_records_success_and_does_not_ssh_again(tmp_path, monkeypatch):
    monkeypatch.setattr(release_host, "manifests_at", lambda repo, ref: {"latest": manifest()})
    calls = []

    def runner(command, **_kwargs):
        calls.append(command)
        return subprocess.CompletedProcess(command, 0, "rollout: deployed 1.2.8\n", "")

    args = step_args(tmp_path)
    assert release_host.roll_out_release(args, runner=runner, now=50) == 0
    assert [c[-2] for c in calls] == ["root@10.0.0.1"]
    state = json.loads((tmp_path / "work" / "rollout-v1.2.8.json").read_text())
    assert state["hosts"]["m1"]["status"] == "deployed" and state["settled"] == 50
    assert release_host.roll_out_release(args, runner=runner, now=60) == 0
    assert len(calls) == 1


def test_roll_out_release_failure_is_recorded_and_backed_off(tmp_path, monkeypatch):
    monkeypatch.setattr(release_host, "manifests_at", lambda repo, ref: {"latest": manifest()})
    calls = []

    def runner(command, **_kwargs):
        calls.append(command)
        return subprocess.CompletedProcess(command, 73, "", "rollout: --set-version failed")

    args = step_args(tmp_path)
    with pytest.raises(release_host.CompletionError, match="m1"):
        release_host.roll_out_release(args, runner=runner, now=100)
    assert release_host.roll_out_release(args, runner=runner, now=200) == 0
    assert len(calls) == 1
    forced = step_args(tmp_path, tag="v1.2.8", rollout_only=True)
    with pytest.raises(release_host.CompletionError):
        release_host.roll_out_release(forced, runner=runner, now=300)
    assert len(calls) == 2


def test_roll_out_release_with_no_managed_hosts_settles_quietly(tmp_path, monkeypatch):
    monkeypatch.setattr(release_host, "manifests_at", lambda repo, ref: {"latest": manifest()})
    args = step_args(tmp_path)
    Path(args.hosts_file).write_text("# retired\nfmnext-n1 10.0.0.2\n")

    def runner(command, **_kwargs):
        raise AssertionError("no host should be contacted")

    assert release_host.roll_out_release(args, runner=runner, now=5) == 0
    state = json.loads((tmp_path / "work" / "rollout-v1.2.8.json").read_text())
    assert state["settled"] == 5 and state["hosts"] == {}


def test_roll_out_release_ignores_a_tag_that_is_not_latest(tmp_path, monkeypatch):
    monkeypatch.setattr(release_host, "manifests_at", lambda repo, ref: {"latest": manifest()})

    def runner(command, **_kwargs):
        raise AssertionError("no host should be contacted")

    assert release_host.roll_out_release(step_args(tmp_path, tag="v1.2.9"), runner=runner) == 0
    with pytest.raises(release_host.CompletionError, match="not v1.2.9"):
        release_host.roll_out_release(step_args(tmp_path, tag="v1.2.9", rollout_only=True),
                                      runner=runner)
