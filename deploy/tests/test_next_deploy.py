"""tools/next_deploy.py: dry-run default, rollout order, gate, rollback."""

import gzip
import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

import next_common
import next_deploy
from fakes import FakeSsh
from next_common import GuardError, Runner, ToolError

REPO = Path(__file__).resolve().parents[2]
FLEET = REPO / "deploy" / "fleet.example.toml"
REV = "0123456789ab"


def fleet(tmp_path):
    import render
    return render.load_fleet(FLEET, state_path=tmp_path / "none.json")


def leader_is(name):
    ips = {"n1": "203.0.113.11", "n2": "203.0.113.12", "n3": "203.0.113.13"}

    def answer(host, script, stdin):
        state = "Leader" if host == ips[name] else "Follower"
        return json.dumps({"store": {"raft": {"state": state}}}).encode()
    return (lambda host, script: "/status" in script, answer)


def test_rollout_is_fleet_order_without_any_leader_lookup(tmp_path):
    # Only the app restarts (rqlite keeps running), so no node is special.
    order = next_deploy.rollout_order(fleet(tmp_path))
    assert [n["name"] for n in order] == ["n1", "n2", "n3"]


def test_the_first_node_of_a_rollout_rotates_with_the_release(tmp_path):
    data = fleet(tmp_path)
    firsts = {next_deploy.rollout_order(data, rev="%012x" % i)[0]["name"]
              for i in range(40)}
    assert firsts == {"n1", "n2", "n3"}
    order = next_deploy.rollout_order(data, rev=REV)
    assert sorted(n["name"] for n in order) == ["n1", "n2", "n3"]
    assert order == next_deploy.rollout_order(data, rev=REV)


def ips_in_order(data):
    return [n["public_ipv4"] for n in next_deploy.rollout_order(data, rev=REV)]


def test_restart_dry_run_runs_nothing(tmp_path):
    ssh = FakeSsh([leader_is("n2")])
    runner = Runner(execute=False)
    next_deploy.restart(fleet(tmp_path), REV, ssh, runner)
    assert ssh.runs == []
    assert len(runner.skipped) == 3


def state_answer(states):
    def answer(host, script, stdin):
        current, running = states[host]()
        return json.dumps({"current": current, "running": running,
                           "app": "active"}).encode()
    return (lambda host, script: script == next_deploy.STATE_SCRIPT, answer)


@pytest.fixture(autouse=True)
def _no_settle(monkeypatch):
    monkeypatch.setattr(next_deploy, "SETTLE_SECONDS", 0)


@pytest.mark.parametrize("parallel", [False, True])
def test_restart_lets_the_cluster_see_a_node_before_the_next_drains(
        tmp_path, monkeypatch, parallel):
    monkeypatch.setattr(next_deploy, "SETTLE_SECONDS", 3)
    events = []

    def activate(host, script, stdin):
        events.append(host)
        return subprocess.CompletedProcess(["ssh"], 0, b"activated", b"")
    states = {ip: (lambda: (REV, REV)) for ip in
              ("203.0.113.11", "203.0.113.12", "203.0.113.13")}
    ssh = FakeSsh([(lambda host, script: "switch_to" in script, activate),
                   state_answer(states)])
    data = fleet(tmp_path)
    first, second, third = ips_in_order(data)
    next_deploy.restart(data, REV, ssh, Runner(execute=True),
                        parallel=parallel,
                        settle=lambda seconds: events.append(seconds))
    assert events[:2] == [first, 3]
    if parallel:
        assert sorted(events[2:]) == sorted([second, third])
    else:
        assert events[2:] == [second, 3, third]


def test_restart_executes_in_order_and_stops_at_first_failed_gate(tmp_path):
    activations = []

    def activate(host, script, stdin):
        activations.append(host)
        code = 4 if host == "203.0.113.13" else 0
        return subprocess.CompletedProcess(["ssh"], code, b"", b"gate failed")
    old = "aaaaaaaaaaaa"
    data = fleet(tmp_path)
    states = {"203.0.113.11": lambda: (REV, REV), "203.0.113.12": lambda: (REV, REV),
              "203.0.113.13": lambda: (old, old)}
    ssh = FakeSsh([(lambda host, script: "switch_to" in script, activate),
                   state_answer(states)])
    with pytest.raises(ToolError, match="n3 failed.*current aaaaaaaaaaaa"):
        next_deploy.restart(data, REV, ssh, Runner(execute=True))
    expected = ips_in_order(data)
    assert activations == expected[:expected.index("203.0.113.13") + 1]
    assert not any("/status" in script for _, script, _ in ssh.runs)


def test_gate_success_is_not_enough_without_the_running_rev(tmp_path):
    ok = subprocess.CompletedProcess(["ssh"], 0, b"activated", b"")
    ssh = FakeSsh([(lambda host, script: "switch_to" in script, ok),
                   state_answer({ip: (lambda: (REV, "aaaaaaaaaaaa")) for ip in
                                 ("203.0.113.11", "203.0.113.12", "203.0.113.13")})])
    with pytest.raises(ToolError, match="n1 failed.*running aaaaaaaaaaaa"):
        next_deploy.restart(fleet(tmp_path), REV, ssh, Runner(execute=True),
                            names=["n1"])
    assert sum("switch_to" in script for _, script, _ in ssh.runs) == 2


def test_half_activated_node_is_activated_again(tmp_path):
    runs = []

    def activate(host, script, stdin):
        runs.append(host)
        if len(runs) == 1:
            return subprocess.CompletedProcess(["ssh"], 1, b"", b"reload failed")
        return subprocess.CompletedProcess(["ssh"], 0, b"activated", b"")
    ssh = FakeSsh([(lambda host, script: "switch_to" in script, activate),
                   state_answer({"203.0.113.11": lambda: (
                       REV, "aaaaaaaaaaaa" if len(runs) == 1 else REV)})])
    assert next_deploy.restart(fleet(tmp_path), REV, ssh, Runner(execute=True),
                               names=["n1"]) == ["n1"]
    assert runs == ["203.0.113.11", "203.0.113.11"]


def test_lost_session_is_judged_by_the_node_state(tmp_path):
    def lost(host, script, stdin):
        raise subprocess.TimeoutExpired(["ssh"], 1200)
    ssh = FakeSsh([(lambda host, script: "switch_to" in script, lost),
                   state_answer({"203.0.113.11": lambda: (REV, REV)})])
    assert next_deploy.restart(fleet(tmp_path), REV, ssh, Runner(execute=True),
                               names=["n1"]) == ["n1"]
    assert sum("switch_to" in script for _, script, _ in ssh.runs) == 1


@pytest.mark.parametrize("script", [next_deploy.activate_script(REV, "n1.next.forkmesh.com"),
                                    next_deploy.rollback_script("n1.next.forkmesh.com"),
                                    next_deploy.STATE_SCRIPT])
def test_activation_survives_a_dropped_session_and_is_serialized(script):
    guard = script.index("set +e +o pipefail")
    assert guard < script.index("trap '' HUP PIPE") < script.index("flock -w 900 9")
    assert "set -e\n" not in script[guard:]


def test_failed_reload_under_bash_e_still_restarts_the_app(tmp_path):
    script = next_deploy.activate_script(REV, "n1.next.forkmesh.com")
    body = script[script.index("switch_to() {"):script.index("\ngate() {")]
    body = body.replace(next_deploy.CURRENT, str(tmp_path / "current"))
    body = body.replace("/etc/forkmesh-next/caddy", str(tmp_path / "etc"))
    release = tmp_path / "release"
    (release / "caddy").mkdir(parents=True)
    (release / "caddy" / "app-routes.caddy").write_text("")
    (release / "caddy" / "world-routes.caddy").write_text("")
    (tmp_path / "etc").mkdir()
    functions = next_deploy.caddy_functions("n1.next.forkmesh.com")
    program = ("set -euo pipefail\n" + next_deploy.ACTIVATION_GUARD.replace(
        next_deploy.ACTIVATION_LOCK, str(tmp_path / "lock")) + functions
        + "unseal_app() { :; }\n" + body + "\n" + 'switch_to "%s"\n' % release)
    completed, calls = run_with_fakes(tmp_path, program, after_reload="forever",
                                      after_restart="forever", RELOAD_EXIT=1)
    assert completed.returncode == 0, completed.stderr
    assert "systemctl restart forkmesh-app.service" in calls
    assert (tmp_path / "current").resolve() == release


HOST = "n2.next.forkmesh.com"


def test_activate_script_gates_and_rolls_back():
    script = next_deploy.activate_script(REV, HOST)
    assert "curl -fsS --max-time 3 http://127.0.0.1:8080/readyz" in script
    assert "http://127.0.0.1:8080/api/version" in script
    assert '.get("rev")==sys.argv[1]' in script
    assert 'switch_to "$prev"' in script
    assert "/etc/forkmesh-next/caddy/app-routes.caddy" in script
    assert "systemctl restart forkmesh-app.service" in script
    assert "/opt/forkmesh-next/app/releases/%s" % REV in script
    assert "--resolve %s:443:127.0.0.1 https://%s/readyz" % (HOST, HOST) in script
    assert "timeout 60 systemctl reload caddy" in script


@pytest.mark.parametrize("host", ["", "n1.next.forkmesh.com; reboot",
                                  "n1 next", "-k.example.com", "N1.example.com"])
def test_bad_node_hosts_are_refused(host):
    with pytest.raises(GuardError):
        next_deploy.activate_script(REV, host)
    with pytest.raises(GuardError):
        next_deploy.rollback_script(host)


def test_node_host_is_the_per_node_name(tmp_path):
    data = fleet(tmp_path)
    assert next_deploy.node_host(data, "n2") == "n2." + data["domain"]


FAKE_SYSTEMCTL = """#!/bin/sh
echo "systemctl $*" >>"$FAKE/calls"
case "$1" in
  is-active)
    down=$(cat "$FAKE/caddy_down")
    [ "$down" = 0 ] && exit 0
    [ "$down" = forever ] || echo $((down - 1)) >"$FAKE/caddy_down"
    exit 3;;
  restart) [ "$2" = caddy ] && cat "$FAKE/after_restart" >"$FAKE/caddy_down"; exit 0;;
  reload) cat "$FAKE/after_reload" >"$FAKE/caddy_down"; exit "${RELOAD_EXIT:-0}";;
esac
exit 0
"""
FAKE_CURL = """#!/bin/sh
for last in "$@"; do :; done
case "$last" in
  http://127.0.0.1:8080/readyz) exit "${APP_READY_EXIT:-0}";;
  http://127.0.0.1:8080/api/version) printf '{"rev": "%s"}' "${APP_REV}"; exit 0;;
  https://*/readyz)
    [ "$(cat "$FAKE/caddy_down")" = 0 ] || exit 7
    exit "${CADDY_READYZ_EXIT:-0}";;
esac
exit 99
"""
FAKE_CADDY = """#!/bin/sh
echo "caddy $*" >>"$FAKE/calls"
exit "${VALIDATE_EXIT:-0}"
"""


def gate_function(script):
    start = script.index("\ngate() {") + 1
    end = script.index("\n}\n", start) + 3
    return script[start:end]


def run_with_fakes(tmp_path, body, down=0, after_restart=0, after_reload=0,
                   **env):
    bindir = tmp_path / "bin"
    bindir.mkdir(exist_ok=True)
    for name, text in (("systemctl", FAKE_SYSTEMCTL), ("curl", FAKE_CURL),
                       ("caddy", FAKE_CADDY), ("sleep", "#!/bin/sh\nexit 0\n")):
        (bindir / name).write_text(text)
        (bindir / name).chmod(0o755)
    (tmp_path / "caddy_down").write_text(str(down))
    (tmp_path / "after_restart").write_text(str(after_restart))
    (tmp_path / "after_reload").write_text(str(after_reload))
    (tmp_path / "calls").write_text("")
    completed = subprocess.run(
        ["bash", "-c", body],
        env={"PATH": "%s:/usr/bin:/bin" % bindir, "FAKE": str(tmp_path),
             "APP_REV": REV, **{k: str(v) for k, v in env.items()}},
        capture_output=True, text=True, timeout=60)
    calls = (tmp_path / "calls").read_text().splitlines()
    return completed, calls


def gate_body(tmp_path):
    release = tmp_path / "release"
    release.mkdir(exist_ok=True)
    (release / "BUILD_REV").write_text(REV + "\n")
    script = next_deploy.activate_script(REV, HOST)
    functions = next_deploy.caddy_functions(HOST)
    return functions + gate_function(script) + 'gate "%s"\n' % release


def test_gate_waits_for_a_caddy_that_is_restarting(tmp_path):
    completed, calls = run_with_fakes(tmp_path, gate_body(tmp_path), down=20)
    assert completed.returncode == 0, completed.stderr
    assert "systemctl restart caddy" not in calls
    assert calls.count("systemctl is-active --quiet caddy") == 21


def test_gate_restarts_a_caddy_that_stays_down_once(tmp_path):
    completed, calls = run_with_fakes(tmp_path, gate_body(tmp_path),
                                      down="forever", after_restart=3)
    assert completed.returncode == 0, completed.stderr
    assert calls.count("systemctl restart caddy") == 1
    assert "did not answer within 30 s" in completed.stderr


def test_gate_fails_when_caddy_never_serves(tmp_path):
    completed, calls = run_with_fakes(tmp_path, gate_body(tmp_path),
                                      down="forever", after_restart="forever")
    assert completed.returncode != 0
    assert calls.count("systemctl restart caddy") == 1
    assert calls.count("systemctl is-active --quiet caddy") == 60


def test_gate_needs_readyz_through_caddy_not_only_a_response(tmp_path):
    completed, calls = run_with_fakes(tmp_path, gate_body(tmp_path),
                                      CADDY_READYZ_EXIT=22)
    assert completed.returncode != 0
    assert calls.count("systemctl restart caddy") == 1


def test_gate_still_needs_the_new_rev(tmp_path):
    completed, calls = run_with_fakes(tmp_path, gate_body(tmp_path),
                                      APP_REV="ffffffffffff")
    assert completed.returncode != 0
    assert not any("caddy" in call for call in calls)


def reload_body():
    return next_deploy.caddy_functions(HOST) + "reload_caddy\n"


def test_reload_that_keeps_caddy_serving_restarts_nothing(tmp_path):
    completed, calls = run_with_fakes(tmp_path, reload_body())
    assert completed.returncode == 0
    assert calls[0].startswith("caddy validate --config /etc/caddy/Caddyfile")
    assert "systemctl reload caddy" in calls
    assert "systemctl restart caddy" not in calls


def test_reload_waits_out_a_short_caddy_restart(tmp_path):
    completed, calls = run_with_fakes(tmp_path, reload_body(), after_reload=10)
    assert completed.returncode == 0
    assert "systemctl restart caddy" not in calls


def test_caddy_that_dies_on_reload_is_restarted(tmp_path):
    completed, calls = run_with_fakes(tmp_path, reload_body(),
                                      after_reload="forever", RELOAD_EXIT=1)
    assert completed.returncode == 0
    assert calls.count("systemctl restart caddy") == 1
    assert "restarting it" in completed.stderr


def test_routes_that_do_not_validate_are_not_reloaded(tmp_path):
    completed, calls = run_with_fakes(tmp_path, reload_body(), VALIDATE_EXIT=1)
    assert completed.returncode == 0
    assert not any(call.startswith("systemctl") for call in calls)
    assert "do not validate" in completed.stderr


@pytest.mark.parametrize("script", [next_deploy.activate_script(REV, HOST),
                                    next_deploy.rollback_script(HOST)])
def test_every_caddy_reload_goes_through_the_guarded_helper(script):
    body = script.split("reload_caddy() {", 1)[1].split("\n}\n", 1)[1]
    assert "systemctl reload caddy" not in body
    assert "reload_caddy" in body


def test_upload_script_verifies_and_installs_offline():
    script = next_deploy.upload_script(REV, "f" * 64)
    assert "sha256sum -c -" in script
    assert '"$venv/bin/pip" install --quiet --no-index --find-links' in script
    assert 'ln -sfn "$venv" .tmp-%s/.venv' % REV in script
    assert "venv $req reused" in script
    assert "compileall -q -o 2" in script
    assert "mv -T .tmp-%s %s" % (REV, REV) in script


@pytest.mark.parametrize("rev", ["", "../etc", "0123456789ab; rm -rf /", "ABCDEF123456"])
def test_bad_release_ids_are_refused(rev):
    with pytest.raises(GuardError):
        next_deploy.activate_script(rev, HOST)
    with pytest.raises(GuardError):
        next_deploy.upload_script(rev, "f" * 64)


def test_migrate_dry_run_opens_no_tunnel(tmp_path):
    ssh = FakeSsh([leader_is("n2")])
    next_deploy.migrate(fleet(tmp_path), ssh, Runner(execute=False))
    assert ssh.tunnels == [] and ssh.runs == []


def healthy_from(first_healthy):
    """n1.. before ``first_healthy`` have no rqlite leader; the rest are
    followers (or the leader) of a healthy cluster."""
    ips = {"n1": "203.0.113.11", "n2": "203.0.113.12", "n3": "203.0.113.13"}
    order = ["n1", "n2", "n3"]

    def answer(host, script, stdin):
        node = next(n for n, ip in ips.items() if ip == host)
        if order.index(node) < order.index(first_healthy):
            store = {"raft": {"state": "Candidate"}, "leader": {}}
        else:
            store = {"raft": {"state": "Follower"},
                     "leader": {"node_id": "n9", "addr": "10.77.0.9:4002"}}
        return json.dumps({"store": store}).encode()
    return (lambda host, script: "/status" in script, answer)


def test_migrate_uses_the_first_healthy_node_not_the_leader(tmp_path, sandbox,
                                                             monkeypatch):
    (sandbox / "secrets.env").write_text("RQLITE_PASSWORD=pw-SECRET\n")
    ssh = FakeSsh([healthy_from("n2")])
    monkeypatch.setattr(next_deploy, "_port_open", lambda *a: True)
    monkeypatch.setattr(next_deploy, "loopback_accepts", lambda host: True)
    seen = {}

    def run(command, env=None, check=None, **kwargs):
        seen["command"] = command
        return subprocess.CompletedProcess(command, 0)
    next_deploy.migrate(fleet(tmp_path), ssh, Runner(execute=True), run=run)
    assert ssh.tunnels[0][0] == "203.0.113.12"   # a follower: rqlite forwards
    assert "--force-follower" not in seen["command"]


def test_migrate_targets_the_leader_with_env_credentials(tmp_path, sandbox, monkeypatch):
    (sandbox / "secrets.env").write_text("RQLITE_PASSWORD=pw-SECRET\n")
    ssh = FakeSsh([leader_is("n3")])
    monkeypatch.setattr(next_deploy, "_port_open", lambda *a: True)
    monkeypatch.setattr(next_deploy, "loopback_accepts", lambda host: True)
    seen = {}

    def run(command, env=None, check=None, **kwargs):
        seen["command"], seen["env"] = command, env
        return subprocess.CompletedProcess(command, 0)
    next_deploy.migrate(fleet(tmp_path), ssh, Runner(execute=True), run=run)
    assert ssh.tunnels[0][0] == "203.0.113.13"
    assert seen["command"][1].endswith("tools/rqlite_migrate.py")
    assert "pw-SECRET" not in " ".join(seen["command"])
    assert seen["env"]["RQLITE_PASSWORD"] == "pw-SECRET"
    assert seen["env"]["RQLITE_USERNAME"] == "fmnext"


def test_tunnel_error_names_a_live_tunnel_the_machine_cannot_reach():
    tunnel = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"],
                              stderr=subprocess.PIPE)
    try:
        error = next_deploy.tunnel_error("n1", tunnel, 40123)
    finally:
        tunnel.kill()
        tunnel.communicate()
    assert "is up" in str(error) and "127.0.0.1:40123" in str(error)
    assert "Fix: allow loopback TCP" in str(error)


def test_tunnel_error_reports_why_ssh_exited():
    tunnel = subprocess.Popen(
        [sys.executable, "-c",
         "import sys; sys.stderr.write('Permission denied (publickey).'); sys.exit(255)"],
        stderr=subprocess.PIPE)
    tunnel.wait(timeout=10)
    error = next_deploy.tunnel_error("n2", tunnel, 40124)
    assert str(error).startswith(
        "SSH tunnel to n2 exited 255: Permission denied (publickey). Fix: the node "
        "refused the fleet SSH key")


def test_tunnel_error_without_a_known_cause_has_no_fix():
    tunnel = subprocess.Popen(
        [sys.executable, "-c", "import sys; sys.stderr.write('odd.'); sys.exit(3)"],
        stderr=subprocess.PIPE)
    tunnel.wait(timeout=10)
    assert str(next_deploy.tunnel_error("n2", tunnel, 1)) == "SSH tunnel to n2 exited 3: odd."


def test_migrate_tunnels_through_another_loopback_when_127_0_0_1_is_dropped(
        tmp_path, sandbox, monkeypatch):
    (sandbox / "secrets.env").write_text("RQLITE_PASSWORD=pw\n")
    ssh = FakeSsh([leader_is("n3")])
    monkeypatch.setattr(next_deploy, "loopback_accepts", lambda host: host != "127.0.0.1")
    probed = []
    monkeypatch.setattr(next_deploy, "_port_open",
                        lambda port, host: probed.append(host) or True)
    seen = {}

    def run(command, env=None, check=None, **kwargs):
        seen["command"] = command
        return subprocess.CompletedProcess(command, 0)
    next_deploy.migrate(fleet(tmp_path), ssh, Runner(execute=True), run=run)
    host, port, remote, bind = ssh.tunnels[0]
    assert (host, remote, bind) == ("203.0.113.13", "127.0.0.1:4001", "::1")
    assert probed and set(probed) == {"::1"}
    assert seen["command"][-1] == "http://[::1]:%d" % port


def test_loopback_host_explains_a_machine_that_drops_all_loopback(monkeypatch):
    monkeypatch.setattr(next_deploy, "loopback_accepts", lambda host: False)
    with pytest.raises(ToolError) as caught:
        next_deploy.loopback_host()
    message = str(caught.value)
    assert "127.0.0.1, ::1, 127.0.0.2" in message
    assert "not the fleet" in message and "Fix: allow loopback TCP" in message


def test_open_rqlite_tunnel_closes_ssh_when_the_port_never_opens(monkeypatch):
    monkeypatch.setattr(next_deploy, "loopback_accepts", lambda host: True)
    monkeypatch.setattr(next_deploy, "_port_open", lambda port, host: False)
    monkeypatch.setattr(next_common, "wait_for", lambda predicate, *a, **k: predicate())
    events = []

    class Tunnel:
        stderr = None

        def poll(self):
            return None

        def terminate(self):
            events.append("terminate")

        def wait(self, timeout=None):
            events.append("wait")

    class Ssh:
        def tunnel(self, host, port, bind="127.0.0.1"):
            return Tunnel()
    with pytest.raises(ToolError, match="is up"):
        next_deploy.open_rqlite_tunnel(Ssh(), {"name": "n1", "public_ipv4": "203.0.113.11"})
    assert events == ["terminate", "wait"]


def test_cli_defaults_to_dry_run(tmp_path, sandbox, monkeypatch):
    ssh = FakeSsh([leader_is("n1")])
    calls = []
    monkeypatch.setattr(next_deploy, "git_rev", lambda **k: REV)
    assert next_deploy.main(["--fleet", str(FLEET), "all"], ssh=ssh,
                            run=lambda *a, **k: calls.append(a)) == 0
    assert calls == [] and ssh.runs == [] and ssh.puts == []
    assert next_deploy.main(["--fleet", str(FLEET), "restart", "n9"], ssh=ssh) == 2
    assert next_deploy.main(["--fleet", str(FLEET), "rollback", "n2"], ssh=ssh) == 0
    assert ssh.runs == []


def test_world_meta(tmp_path):
    index = tmp_path / "world" / "index.html"
    index.parent.mkdir(parents=True)
    index.write_text("<!doctype html><html><head><title>w</title></head></html>" + "x" * 3000)
    next_deploy.inject_world_meta(index, "https://app.next.forkmesh.com", REV,
                                  "0.9.3", 1790000000)
    html = index.read_text()
    assert '<meta name="forkmesh-world-version" content="0.9.3">' in html
    assert '<meta name="forkmesh-world-built" content="1790000000">' in html
    assert html.index('<meta name="forkmesh-api-origin" content="https://app.next.forkmesh.com">') \
        < html.index("<title>")
    assert '<meta name="forkmesh-world-deploy" content="%s">' % REV in html
    assert html.index('<link rel="preconnect" href="https://app.next.forkmesh.com" '
                      'crossorigin>') < html.index("<title>")
    next_deploy.inject_world_meta(index, "https://other", REV)  # idempotent
    assert index.read_text() == html
    # /health/mobile is live now (app/host/forkmesh_host/world_health.py).
    assert not hasattr(next_deploy, "mobile_health")


def test_precompress_writes_gzip_sidecars(tmp_path):
    (tmp_path / "big.js").write_text("console.log(1);\n" * 200)
    (tmp_path / "small.js").write_text("x")
    (tmp_path / "image.png").write_bytes(b"\x89PNG" * 500)
    assert next_deploy.precompress(tmp_path) == 1
    assert gzip.decompress((tmp_path / "big.js.gz").read_bytes()) == \
        (tmp_path / "big.js").read_bytes()
    assert not (tmp_path / "small.js.gz").exists()
    assert not (tmp_path / "image.png.gz").exists()


def test_precompress_writes_brotli_sidecars_without_the_python_module(tmp_path):
    if next_deploy.node_binary() is None:
        pytest.skip("node is not installed")
    (tmp_path / "big.js").write_text("console.log(1);\n" * 200)
    (tmp_path / "small.js").write_text("x")
    next_deploy.node_brotli([tmp_path / "big.js"])
    packed = (tmp_path / "big.js.br").read_bytes()
    assert 0 < len(packed) < (tmp_path / "big.js").stat().st_size
    completed = subprocess.run(
        [next_deploy.node_binary(), "-e",
         "process.stdout.write(require('zlib').brotliDecompressSync("
         "require('fs').readFileSync(process.argv[1])).toString())",
         str(tmp_path / "big.js.br")],
        capture_output=True, text=True, check=True)
    assert completed.stdout == (tmp_path / "big.js").read_text()
    assert not (tmp_path / "small.js.br").exists()


def world_rows(ssh):
    return [json.loads(stdin)[0] for host, script, stdin in ssh.runs
            if "/db/execute" in script]


def test_world_deploy_body_marks_start_and_finish():
    sql, state, rev, started, finished = json.loads(
        next_deploy.world_deploy_body("deploying", REV, 1000))[0]
    assert "world_deploy_status" in sql and "?" in sql
    assert (state, rev, started, finished) == ("deploying", REV, 1000, 0)
    assert json.loads(next_deploy.world_deploy_body("ready", REV, 2000))[0][1:] == [
        "ready", REV, 0, 2000]


def test_world_deploy_signal_writes_through_a_healthy_node(tmp_path):
    ssh = FakeSsh([healthy_from("n2"),
                   (lambda host, script: "/db/execute" in script,
                    json.dumps({"results": [{"rows_affected": 1}]}).encode())])
    runner = Runner(execute=True)
    next_deploy.signal_world_deploy(fleet(tmp_path), ssh, runner, "deploying", REV,
                                    clock=lambda: 5.0)
    writes = [(h, s) for h, s, _ in ssh.runs if "/db/execute" in s]
    assert writes and writes[0][0] == "203.0.113.12"
    assert "-X POST" in writes[0][1] and "@-" in writes[0][1]
    assert world_rows(ssh)[0][1:] == ["deploying", REV, 5000, 0]


def test_world_deploy_signal_never_fails_the_deploy(tmp_path, capsys):
    ssh = FakeSsh([healthy_from("n1"),
                   (lambda host, script: "/db/execute" in script,
                    json.dumps({"results": [{"error": "no such table"}]}).encode())])
    runner = Runner(execute=True)
    next_deploy.signal_world_deploy(fleet(tmp_path), ssh, runner, "ready", REV)
    assert "World deploy notice not written (no such table)" in capsys.readouterr().out


def test_world_deploy_dry_run_writes_nothing(tmp_path):
    ssh = FakeSsh([healthy_from("n1")])
    next_deploy.signal_world_deploy(fleet(tmp_path), ssh, Runner(execute=False),
                                    "deploying", REV)
    assert ssh.runs == []


def test_upload_announces_and_failed_restart_reports_it(tmp_path, sandbox, monkeypatch):
    ssh = FakeSsh([healthy_from("n1"),
                   (lambda host, script: "/db/execute" in script,
                    json.dumps({"results": [{}]}).encode())])
    tarball = tmp_path / ("fmnext-%s.tar.gz" % REV)
    tarball.write_bytes(b"x")
    monkeypatch.setattr(next_deploy, "dist_dir", lambda: tmp_path)
    monkeypatch.setattr(next_deploy, "upload", lambda *a, **k: None)
    assert next_deploy.main(["--fleet", str(FLEET), "upload", "--rev", REV,
                             "--execute"], ssh=ssh) == 0
    assert world_rows(ssh) == []
    monkeypatch.setattr(next_deploy, "migrate", lambda *a, **k: None)
    assert next_deploy.main(["--fleet", str(FLEET), "migrate", "--rev", REV,
                             "--execute"], ssh=ssh) == 0
    assert [row[1] for row in world_rows(ssh)] == ["deploying"]

    def broken(*a, **k):
        raise ToolError("n2 failed its gate")
    monkeypatch.setattr(next_deploy, "restart", broken)
    assert next_deploy.main(["--fleet", str(FLEET), "restart", "--rev", REV,
                             "--execute"], ssh=ssh) == 1
    assert [row[1] for row in world_rows(ssh)] == [
        "deploying", "deploying", "failed"]


def test_single_node_restart_leaves_the_world_notice_alone(tmp_path, sandbox,
                                                          monkeypatch):
    ssh = FakeSsh([healthy_from("n1")])
    monkeypatch.setattr(next_deploy, "restart", lambda *a, **k: ["n2"])
    assert next_deploy.main(["--fleet", str(FLEET), "restart", "n2", "--rev", REV,
                             "--execute"], ssh=ssh) == 0
    assert world_rows(ssh) == []


def test_upload_script_prunes_old_releases_and_reuses_the_venv(tmp_path):
    root = tmp_path / "app"
    releases = root / "releases"
    releases.mkdir(parents=True)
    import time as _time
    names = ["%012x" % i for i in range(1, 10)]
    for index, name in enumerate(names):
        (releases / name).mkdir()
        stamp = _time.time() - 1000 + index
        os.utime(releases / name, (stamp, stamp))
    (releases / "notarelease").mkdir()
    (root / "current").symlink_to(releases / names[0])
    (root / "previous").write_text(str(releases / names[1]))
    script = next_deploy.upload_script(REV, "f" * 64)
    tail = script[script.index("keep="):]
    tail = (tail.replace(next_deploy.CURRENT, str(root / "current"))
                .replace(next_deploy.PREVIOUS, str(root / "previous")))
    subprocess.run(["bash", "-euo", "pipefail", "-c", tail], cwd=releases,
                   check=True)
    left = sorted(p.name for p in releases.iterdir())
    assert names[0] in left and names[1] in left
    assert "notarelease" in left
    assert len([n for n in left if n in names]) == next_deploy.KEEP_RELEASES + 2


def fake_wheels(tmp_path, monkeypatch):
    wheels = tmp_path / "wheelhouse" / "0123456789abcdef"
    wheels.mkdir(parents=True)
    (wheels / "demo-1.0-py3-none-any.whl").write_bytes(b"wheel")
    monkeypatch.setattr(next_deploy, "cached_wheelhouse", lambda run=None: wheels)
    return wheels


def test_upload_reaches_every_node_at_once(tmp_path, monkeypatch):
    import threading
    fake_wheels(tmp_path, monkeypatch)
    tarball = tmp_path / ("fmnext-%s.tar.gz" % REV)
    tarball.write_bytes(b"release")
    nodes = next_deploy.serving_nodes(fleet(tmp_path))
    barrier = threading.Barrier(len(nodes), timeout=5)

    class ConcurrentSsh(FakeSsh):
        def put(self, host, local, remote):
            barrier.wait()
            super().put(host, local, remote)
    ssh = ConcurrentSsh()
    next_deploy.upload(fleet(tmp_path), REV, tarball, ssh, Runner(execute=True))
    assert sorted(h for h, _, _ in ssh.puts) == sorted(n["public_ipv4"] for n in nodes)
    assert len(ssh.runs) == 2 * len(nodes)


def test_one_failed_upload_fails_the_step_after_the_others_finish(tmp_path, monkeypatch):
    fake_wheels(tmp_path, monkeypatch)
    tarball = tmp_path / ("fmnext-%s.tar.gz" % REV)
    tarball.write_bytes(b"release")

    class OneBadNode(FakeSsh):
        def put(self, host, local, remote):
            if host == "203.0.113.12":
                raise ToolError("scp to %s failed" % host)
            super().put(host, local, remote)
    ssh = OneBadNode()
    with pytest.raises(ToolError, match="203.0.113.12"):
        next_deploy.upload(fleet(tmp_path), REV, tarball, ssh, Runner(execute=True))
    assert sorted(h for h, _, _ in ssh.puts) == ["203.0.113.11", "203.0.113.13"]






def test_wheelhouse_is_downloaded_once_per_requirements(tmp_path, monkeypatch):
    monkeypatch.setenv("FMNEXT_DIST_DIR", str(tmp_path / "dist"))
    calls = []

    def run(command, check=None, **kwargs):
        calls.append(command)
        dest = Path(command[command.index("--dest") + 1])
        dest.mkdir(parents=True)
        (dest / "x-1.0-py3-none-any.whl").write_bytes(b"wheel")
        return subprocess.CompletedProcess(command, 0)
    first = next_deploy.cached_wheelhouse(run)
    again = next_deploy.cached_wheelhouse(run)
    assert first == again and len(calls) == 1
    assert first.parent == tmp_path / "dist" / "wheelhouse"
    assert (first / "x-1.0-py3-none-any.whl").is_file()


def test_parallel_node_uploads_build_the_wheelhouse_tarball_once(tmp_path, monkeypatch):
    import tarfile
    import threading
    wheels = tmp_path / "wheelhouse" / "12baeef54c13003e"
    wheels.mkdir(parents=True)
    (wheels / "x-1.0-py3-none-any.whl").write_bytes(b"wheel")
    barrier = threading.Barrier(3, timeout=1)
    real_open = tarfile.open
    opened = []

    def crowded_open(*args, **kwargs):
        opened.append(args[0])
        try:
            barrier.wait()
        except threading.BrokenBarrierError:
            pass
        return real_open(*args, **kwargs)
    monkeypatch.setattr(next_deploy.tarfile, "open", crowded_open)
    results, errors = [], []

    def upload():
        try:
            results.append(next_deploy.wheelhouse_tarball(wheels))
        except OSError as error:
            errors.append(error)
    threads = [threading.Thread(target=upload) for _ in range(3)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(10)
    monkeypatch.setattr(next_deploy.tarfile, "open", real_open)
    assert errors == []
    assert len(opened) == 1
    assert results == [wheels.parent / "12baeef54c13003e.tar"] * 3
    with tarfile.open(results[0]) as archive:
        assert archive.getnames() == ["x-1.0-py3-none-any.whl"]
    assert sorted(p.name for p in wheels.parent.iterdir()) == [
        "12baeef54c13003e", "12baeef54c13003e.tar"]




def test_app_schema_replay_is_batched(monkeypatch):
    import rqlite_migrate
    ddl = ["CREATE TABLE IF NOT EXISTS t%d (a)" % i for i in range(250)]
    alters = ["ALTER TABLE t0 ADD COLUMN b", "ALTER TABLE t0 ADD COLUMN b"]
    monkeypatch.setattr(rqlite_migrate, "app_schema_groups",
                        lambda: (([], True), (ddl, False), (alters, True)))
    requests = []

    class Db:
        def results(self, statements, **kwargs):
            requests.append(len(statements))
            seen = set()
            out = []
            for (sql,) in statements:
                out.append({"error": "duplicate column name: b"} if sql in seen
                           else {})
                seen.add(sql)
            return out
    report = rqlite_migrate.apply_app_schema(Db(), log=lambda *a: None)
    assert requests == [100, 100, 50, 2]
    assert report == {"applied": 251, "tolerated": 1}


def test_app_schema_replay_still_fails_on_a_strict_statement(monkeypatch):
    import rqlite_migrate
    monkeypatch.setattr(rqlite_migrate, "app_schema_groups",
                        lambda: ((["CREATE TABLE broken ("], False),))

    class Db:
        def results(self, statements, **kwargs):
            return [{"error": "incomplete input"}]
    with pytest.raises(rqlite_migrate.MigrateError, match="incomplete input"):
        rqlite_migrate.apply_app_schema(Db(), log=lambda *a: None)


def test_ssh_starts_one_detached_master_per_node_and_tunnels_stay_separate(
        tmp_path, monkeypatch):
    monkeypatch.setenv("FMNEXT_SSH_MUX_DIR", str(tmp_path / "mux"))
    calls = []

    def run(command, **kwargs):
        calls.append((command, kwargs))
        code = 255 if "-O" in command else 0
        return subprocess.CompletedProcess(command, code, b"", b"")
    ssh = next_common.Ssh(key=tmp_path / "k", known_hosts=tmp_path / "kh",
                          run=run, mux=True)
    ssh.run("203.0.113.11", "true")
    ssh.run("203.0.113.11", "true")
    masters = [c for c, k in calls if "-f" in c]
    assert len(masters) == 1 and "ControlMaster=yes" in masters[0]
    assert all(k.get("stdout") == subprocess.DEVNULL for c, k in calls if "-f" in c)
    commands = [c for c, k in calls if "bash -euo pipefail -c true" in c]
    assert len(commands) == 2
    assert all("ControlMaster=no" in c and any(
        part.startswith("ControlPath=%s/" % (tmp_path / "mux")) for part in c)
        for c in commands)
    assert oct((tmp_path / "mux").stat().st_mode & 0o777) == "0o700"
    popened = []
    ssh._popen = lambda command, **kwargs: popened.append(command)
    ssh.tunnel("203.0.113.11", 4001)
    assert "ControlPath=none" in popened[0]
    assert "127.0.0.1:4001:127.0.0.1:4001" in popened[0]
    ssh.tunnel("203.0.113.11", 4002, bind="::1")
    assert "[::1]:4002:127.0.0.1:4001" in popened[1]


def test_ssh_mux_dir_falls_back_when_the_temp_path_is_too_long(
        tmp_path, monkeypatch):
    monkeypatch.delenv("FMNEXT_SSH_MUX_DIR", raising=False)
    deep = tmp_path / ("x" * 80)
    deep.mkdir()
    runtime = tmp_path / "run"
    runtime.mkdir()
    name = "fmnext-ssh-%d" % os.getuid()
    monkeypatch.setattr(next_common.tempfile, "tempdir", str(deep))
    monkeypatch.setattr(next_common, "SSH_MUX_DIR_MAX",
                        len(str(runtime / name)))
    monkeypatch.setenv("XDG_RUNTIME_DIR", str(runtime))
    ssh = next_common.Ssh(key=tmp_path / "k", known_hosts=tmp_path / "kh",
                          run=lambda *a, **k: None, mux=True)
    assert ssh.mux_dir() == runtime / name
    monkeypatch.setenv("XDG_RUNTIME_DIR", str(deep))
    assert ssh.mux_dir() is None
    assert "ControlPath=none" in ssh.options()


def test_wheels_go_to_a_node_only_when_it_lacks_them(tmp_path, monkeypatch):
    wheels = fake_wheels(tmp_path, monkeypatch)
    tarball = tmp_path / ("fmnext-%s.tar.gz" % REV)
    tarball.write_bytes(b"release")
    state = {"203.0.113.11": b"need\n"}
    ssh = FakeSsh([(lambda host, script: "wheelhouse" in script and "cd /opt/forkmesh-next/app/wheelhouses" in script,
                    lambda host, script, stdin: state.pop(host, b"wheelhouse present\n"))])
    next_deploy.upload(fleet(tmp_path), REV, tarball, ssh, Runner(execute=True))
    wheel_puts = [(h, r) for h, _, r in ssh.puts if r.endswith(".tar")]
    assert wheel_puts == [("203.0.113.11", "/opt/forkmesh-next/app/wheelhouses/"
                           ".incoming-%s.tar" % wheels.name)]
    import tarfile
    with tarfile.open(next_deploy.wheelhouse_tarball(wheels)) as archive:
        assert archive.getnames() == ["demo-1.0-py3-none-any.whl"]
    script = next_deploy.upload_script(REV, "f" * 64)
    assert "--find-links \"$wheels\"" in script and "cat .tmp-%s/WHEELHOUSE" % REV in script
    with pytest.raises(GuardError):
        next_deploy.wheelhouse_script("../x")



def test_join_tree_carries_only_the_public_addresses(sandbox, tmp_path):
    from fakes import repo_fleet_instances
    instances = repo_fleet_instances()
    private = {label: dict(info, id="id-" + label, plan="p", region="ewr")
               for label, info in instances.items()}
    next_common.save_json(sandbox / "fleet-instances.json", private)
    stage = tmp_path / "stage"
    next_deploy.stage_join_tree(stage, state={})
    written = json.loads((stage / "deploy" / "fleet-instances.public.json").read_text())
    assert written == instances
    import render
    fleet = render.load_fleet(
        stage / "deploy" / "fleet.toml", state={},
        instances_path=stage / "deploy" / "fleet-instances.public.json")
    assert [n["public_ipv4"] for n in fleet["nodes"]] == [
        instances[n["label"]]["ip"] for n in fleet["nodes"]]

def test_prune_dist_keeps_newest_releases_and_the_named_one(tmp_path):
    names = ["%012x" % n for n in range(9)]
    for age, rev in enumerate(names):
        tarball = tmp_path / ("fmnext-%s.tar.gz" % rev)
        tarball.write_bytes(b"x")
        next_deploy.sha_path(tarball).write_text("d\n")
        os.utime(tarball, (1000 + age, 1000 + age))
    (tmp_path / "forkmesh-mirror-node").write_bytes(b"bin")
    (tmp_path / "wheelhouse").mkdir()
    removed = next_deploy.prune_dist(tmp_path, keep={"fmnext-%s.tar.gz" % names[0]}, keep_n=3)
    left = sorted(p.name for p in tmp_path.glob("fmnext-*.tar.gz"))
    assert left == sorted("fmnext-%s.tar.gz" % rev for rev in (names[0], *names[-3:]))
    assert len(removed) == 5
    assert not (tmp_path / ("fmnext-%s.sha256" % names[1])).exists()
    assert (tmp_path / ("fmnext-%s.sha256" % names[0])).exists()
    assert (tmp_path / "forkmesh-mirror-node").exists() and (tmp_path / "wheelhouse").is_dir()
