"""The real fmnext-ids daemon process, end to end, with stub journalctl /
nft / systemctl on PATH: a journal line of an SSH login with a foreign key
locks the node down (firewall, stopped units, marker), the state survives a
restart, and the control socket answers status and test-fire."""

import json
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

import pytest

import ids

REPO = Path(__file__).resolve().parents[2]
IDS = REPO / "deploy" / "core" / "ids.py"
FLEET_FP = "SHA256:" + "A" * 43
FOREIGN = "SHA256:" + "Z" * 43


def stub(directory, name, body):
    path = directory / name
    path.write_text("#!/bin/sh\n" + body)
    path.chmod(0o755)


def setup(tmp_path, journal_lines):
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir(parents=True)
    log = tmp_path / "calls.log"
    lines = tmp_path / "journal.json"
    lines.write_text("".join(json.dumps(l) + "\n" for l in journal_lines))
    stub(bin_dir, "journalctl", "cat %s\nexec sleep 60\n" % lines)
    stub(bin_dir, "nft", "echo \"nft $*\" >>%s; cat >>%s; exit 0\n" % (log, log))
    stub(bin_dir, "systemctl", "echo \"systemctl $*\" >>%s\n" % log)
    stub(bin_dir, "pkill", "echo \"pkill $*\" >>%s\n" % log)
    watched = tmp_path / "watched"
    watched.mkdir()
    (watched / "tool").write_text("v1")
    config = {
        "node": "n1", "role": "core",
        "state_dir": str(tmp_path / "state"), "run_dir": str(tmp_path / "run"),
        "nodes": ["n1", "n2", "n3"],
        "ssh": {"fleet_fingerprints": [FLEET_FP]},
        "integrity": {"lockdown": [str(watched)], "authorized_keys": False},
        "processes": {}, "listeners": {"tcp": list(range(1, 65536)),
                                       "udp": list(range(1, 65536))},
        "rqlite": {"url": "http://127.0.0.1:9", "user": ""},
        "unseal_socket": str(tmp_path / "no-app.sock"),
        "lockdown": {"stop_units": ["forkmesh-app.service", "caddy.service"],
                     "core_wg_ips": ["10.77.0.1"],
                     "core_public_ips": ["198.51.100.1"]},
        "heartbeat": {"interval": 2},
    }
    path = tmp_path / "ids.json"
    path.write_text(json.dumps(config))
    env = dict(os.environ, PATH=str(bin_dir) + os.pathsep + os.environ["PATH"])
    env.pop("CREDENTIALS_DIRECTORY", None)
    return config, path, env, log


def start(path, env):
    return subprocess.Popen([sys.executable, str(IDS), "--config", str(path),
                             "run"], env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)


def wait_status(config, predicate, timeout=30):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        last = ids.control(config, {"command": "status"}, timeout=5)
        if last.get("ok") and predicate(last):
            return last
        time.sleep(0.2)
    raise AssertionError("status never matched: %r" % (last,))


def stop(process):
    process.terminate()
    try:
        out, _ = process.communicate(timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()
        out, _ = process.communicate()
    return out.decode("utf-8", "replace")


@pytest.mark.skipif(not sys.platform.startswith("linux"), reason="linux only")
def test_foreign_ssh_key_locks_the_node_down(tmp_path):
    login = {"SYSLOG_IDENTIFIER": "sshd-session", "__CURSOR": "c1",
             "MESSAGE": "Accepted publickey for root from 203.0.113.9 port "
                        "40000 ssh2: ED25519 %s" % FOREIGN}
    config, path, env, log = setup(tmp_path, [login])
    process = start(path, env)
    try:
        status = wait_status(config, lambda s: s["lockdown"])
        assert status["lockdownIncident"]["incident"]
        assert any(r["detector"] == "ssh_unknown_key" for r in
                   status["recent"])
        calls = log.read_text()
        assert "meta skuid != 0 counter drop" in calls
        assert "systemctl stop --no-block forkmesh-app.service " \
               "caddy.service" in calls
        assert (tmp_path / "state" / "LOCKDOWN").exists()
        reply = ids.control(config, {"command": "test-fire"})
        assert reply["ok"] and reply["incident"]
        assert reply["delivery"]["journal"][0] is True
        # Without its key the node refuses every expected-change manifest.
        reply = ids.control(config, {"command": "expect", "manifest": {
            "v": 1, "node": "n1"}})
        assert reply == {"ok": False, "error": "no_key"}
    finally:
        out = stop(process)
    assert "LOCKDOWN" in out
    # A restart (or a reboot) comes back locked down and re-applies it.
    log.write_text("")
    config, path, env, _ = setup(tmp_path / "again", [])
    config["state_dir"] = str(tmp_path / "state")
    path.write_text(json.dumps(config))
    process = start(path, env)
    try:
        wait_status(config, lambda s: s["lockdown"])
        deadline = time.monotonic() + 10
        again_log = tmp_path / "again" / "calls.log"
        while time.monotonic() < deadline and not (
                again_log.exists() and "counter drop" in again_log.read_text()):
            time.sleep(0.2)
        assert "meta skuid != 0 counter drop" in again_log.read_text()
    finally:
        stop(process)


@pytest.mark.skipif(not sys.platform.startswith("linux"), reason="linux only")
def test_integrity_change_alerts_without_lockdown_for_alert_paths(tmp_path):
    config, path, env, log = setup(tmp_path, [])
    config["integrity"] = {"alert": config["integrity"]["lockdown"],
                           "authorized_keys": False, "interval": 1}
    path.write_text(json.dumps(config))
    process = start(path, env)
    try:
        wait_status(config, lambda s: s["baselinePaths"] >= 2)
        Path(config["integrity"]["alert"][0], "tool").write_text("v2 longer")
        status = wait_status(config, lambda s: any(
            r["detector"] == "integrity_modified" for r in s["recent"]))
        assert not status["lockdown"]
    finally:
        stop(process)


def test_unit_command_line_parses(monkeypatch):
    """The fmnext-ids unit's ExecStart must be accepted by the real parser:
    --config belongs to the top-level parser, before the subcommand (the
    first rollout crash-looped on `run --config ...`)."""
    unit = (REPO / "deploy" / "core" / "templates" / "fmnext-ids.service").read_text()
    exec_line = next(l for l in unit.splitlines() if l.startswith("ExecStart="))
    argv = [a.replace("@@ETC@@", "/etc-test")
            for a in exec_line.split("fmnext-ids", 1)[1].split()]

    class Parsed(Exception):
        pass

    def fake_load_config(path):
        raise Parsed(path)

    monkeypatch.setattr(ids, "load_config", fake_load_config)
    with pytest.raises(Parsed) as got:
        ids.main(argv)
    assert str(got.value) == "/etc-test/ids.json"
