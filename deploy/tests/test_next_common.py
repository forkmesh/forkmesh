import argparse
import io
import os
import stat
import subprocess
from pathlib import Path

import pytest

import next_common as common


def args(**values):
    return argparse.Namespace(**values)


def test_an_executing_runner_tees_every_line_to_an_ops_log(tmp_path):
    runner = common.runner_from_args(args(execute=True, dry_run=False,
                                          command="restart"))
    assert runner.log_path.parent == Path(os.environ["FMNEXT_LOGS_DIR"]) / "ops"
    assert runner.log_path.name.endswith("-restart.log")
    runner.out = io.StringIO()
    runner.say("n1: restart\nn1: ready")
    runner.step("n2: restart", lambda: None)
    lines = runner.log_path.read_text("utf-8").splitlines()
    assert [line.split(" ", 1)[1] for line in lines] == [
        "n1: restart", "n1: ready", "[execute] n2: restart"]
    assert stat.S_IMODE(runner.log_path.stat().st_mode) == 0o600
    assert stat.S_IMODE(runner.log_path.parent.stat().st_mode) == 0o700


def test_a_dry_run_writes_no_ops_log():
    runner = common.runner_from_args(args(execute=False, dry_run=False,
                                          command="restart"))
    assert runner.log_path is None
    runner.out = io.StringIO()
    runner.say("plan")
    assert not (Path(os.environ["FMNEXT_LOGS_DIR"]) / "ops").exists()


def test_the_ops_log_is_named_after_the_tool_and_command():
    path = common.ops_log_path(args(command="install"), argv0="/x/tools/next_fleet.py",
                               clock=lambda: 0)
    assert path.name.endswith("-next_fleet-install.log")
    assert common.ops_log_path(args(), argv0="next_dns.py").name.endswith("-next_dns.log")


def test_a_child_of_the_deploy_agent_leaves_logging_to_the_agent(monkeypatch):
    monkeypatch.setenv(common.LOCK_FD_ENV, "7")
    assert common.ops_log_path(args(command="restart")) is None


def test_an_unwritable_ops_log_never_breaks_the_tool(tmp_path):
    blocker = tmp_path / "file"
    blocker.write_text("x")
    out = io.StringIO()
    runner = common.Runner(execute=True, out=out, log_path=blocker / "ops" / "a.log")
    runner.say("still printed")
    runner.say("and again")
    assert out.getvalue() == "still printed\nand again\n"
    assert runner.log_path is None


def test_a_dropped_upload_resumes_from_the_bytes_already_on_the_node(tmp_path):
    local = tmp_path / "release.tar.gz"
    local.write_bytes(b"0123456789" * 100)
    remote = bytearray(b"0123456789" * 30)
    calls = []

    def run(command, **kwargs):
        calls.append(command[0])
        if command[0] == "scp":
            return subprocess.CompletedProcess(command, 1, b"", b"lost connection")
        if command[-1].startswith("stat -c"):
            return subprocess.CompletedProcess(command, 0, b"%d\n" % len(remote), b"")
        assert command[-1] == "cat >> /opt/r/.incoming-x.tar.gz"
        remote.extend(kwargs["stdin"].read())
        return subprocess.CompletedProcess(command, 0, b"", b"")
    ssh = common.Ssh(key=tmp_path / "key", known_hosts=tmp_path / "kh", run=run)
    ssh.put("198.51.100.10", local, "/opt/r/.incoming-x.tar.gz")
    assert bytes(remote) == local.read_bytes()
    assert calls == ["scp", "ssh", "ssh"]


def test_an_upload_that_never_resumes_still_fails(tmp_path):
    local = tmp_path / "release.tar.gz"
    local.write_bytes(b"x" * 10)

    def run(command, **kwargs):
        return subprocess.CompletedProcess(command, 255, b"", b"lost connection")
    ssh = common.Ssh(key=tmp_path / "key", known_hosts=tmp_path / "kh", run=run)
    with pytest.raises(common.ToolError, match="lost connection"):
        ssh.put("198.51.100.10", local, "/opt/r/.incoming-x.tar.gz")


V4 = "198.51.100.10"
V6 = "2001:db8::10"


def test_route_address_keeps_ipv4_while_it_answers_and_falls_back_to_ipv6():
    up = {V4}
    common._route_cache.clear()
    assert common.route_address(V4, "auto", {V4: V6}, probe=lambda a, port: a in up) == V4
    common._route_cache.clear()
    up = {V6}
    assert common.route_address(V4, "auto", {V4: V6}, probe=lambda a, port: a in up) == V6
    common._route_cache.clear()
    up = set()
    assert common.route_address(V4, "auto", {V4: V6}, probe=lambda a, port: a in up) == V4
    common._route_cache.clear()


def test_route_address_forced_families_and_unknown_nodes():
    never = lambda address, port: pytest.fail("forced families never probe")
    assert common.route_address(V4, "6", {V4: V6}, probe=never) == V6
    assert common.route_address(V4, "4", {V4: V6}, probe=never) == V4
    assert common.route_address("203.0.113.9", "6", {V4: V6}, probe=never) == "203.0.113.9"
    assert common.route_address(V6, "auto", {V4: V6}, probe=never) == V6
    with pytest.raises(common.GuardError):
        common.route_address(V4, "5", {V4: V6}, probe=never)


def test_ipv6_map_comes_from_fleet_instances(tmp_path):
    (tmp_path / "fleet-instances.json").write_text(
        '{"fmnext-core-2": {"ip": "%s", "ipv6": "%s"}, "fmnext-run-1": {"ip": "203.0.113.9"}}'
        % (V4, V6))
    assert common.ipv6_by_ipv4(tmp_path / "fleet-instances.json") == {V4: V6}


def test_ssh_over_ipv6_keeps_the_ipv4_host_key(tmp_path, monkeypatch):
    config = tmp_path / "config"
    config.mkdir()
    (config / "fleet-instances.json").write_text(
        '{"fmnext-core-2": {"ip": "%s", "ipv6": "%s"}}' % (V4, V6))
    monkeypatch.setenv("FMNEXT_CONFIG_DIR", str(config))
    monkeypatch.setenv("FMNEXT_ROUTE_FAMILY", "6")
    calls = []

    def run(command, **kwargs):
        calls.append(command)
        return subprocess.CompletedProcess(command, 0, b"", b"")
    ssh = common.Ssh(key=tmp_path / "key", known_hosts=tmp_path / "kh", run=run, route=True,
                     port=22)
    ssh.run(V4, "hostname")
    ssh.put(V4, tmp_path / "key", "/tmp/x")
    login, upload = calls
    assert "root@%s" % V6 in login and "HostKeyAlias=%s" % V4 in login
    assert upload[-1] == "root@[%s]:/tmp/x" % V6 and "HostKeyAlias=%s" % V4 in upload
    calls.clear()
    monkeypatch.setenv("FMNEXT_ROUTE_FAMILY", "4")
    ssh.run(V4, "hostname")
    assert "root@%s" % V4 in calls[0] and not any("HostKeyAlias" in part for part in calls[0])


def test_a_fake_runner_never_routes(tmp_path, monkeypatch):
    monkeypatch.setenv("FMNEXT_ROUTE_FAMILY", "6")
    calls = []
    ssh = common.Ssh(key=tmp_path / "key", known_hosts=tmp_path / "kh",
                     run=lambda command, **kwargs: calls.append(command) or
                     subprocess.CompletedProcess(command, 0, b"", b""))
    ssh.run(V4, "hostname")
    assert "root@%s" % V4 in calls[0]


def test_ssh_uses_the_fleet_port_and_pins_the_host_key_by_address(tmp_path):
    seen = []

    def run(command, **kwargs):
        seen.append(command)
        return subprocess.CompletedProcess(command, 0, b"", b"")
    ssh = common.Ssh(key=tmp_path / "key", known_hosts=tmp_path / "kh", run=run,
                     port=822)
    ssh.run("198.51.100.10", "true")
    assert "Port=822" in seen[-1] and "HostKeyAlias=198.51.100.10" in seen[-1]
    ssh = common.Ssh(key=tmp_path / "key", known_hosts=tmp_path / "kh", run=run,
                     port=22)
    ssh.run("198.51.100.10", "true")
    assert not any(part.startswith(("Port=", "HostKeyAlias=")) for part in seen[-1])


def test_ssh_falls_back_to_22_for_a_node_not_moved_yet(tmp_path):
    def probe(address, port):
        return port == 22
    ssh = common.Ssh(key=tmp_path / "key", known_hosts=tmp_path / "kh",
                     route=True, port=822, probe=probe)
    assert ssh.port("198.51.100.10") == 22
    ssh.use_port("198.51.100.10", 822)
    assert ssh.port("198.51.100.10") == 822
    assert ssh.dest("198.51.100.10")[-1] == "Port=822"


def test_the_fleet_port_comes_from_fleet_toml(tmp_path, monkeypatch):
    monkeypatch.delenv("FMNEXT_SSH_PORT", raising=False)
    path = tmp_path / "fleet.toml"
    path.write_text('ssh_key_name = "fmnext-fleet"\nssh_port = 822\n')
    assert common.fleet_ssh_port(path) == 822
    path.write_text('ssh_key_name = "fmnext-fleet"\n')
    assert common.fleet_ssh_port(path) == 22
    monkeypatch.setenv("FMNEXT_SSH_PORT", "2200")
    assert common.fleet_ssh_port(path) == 2200
