import json
import re
import subprocess
import tomllib
from pathlib import Path

import pytest

import next_aws
import next_fleet
import next_ids
import next_remove_core
import render
from fakes import FakeSsh
from next_common import Runner, ToolError

REPO = Path(__file__).resolve().parents[2]
FLEET = REPO / "deploy" / "fleet.example.toml"
SECRET = "C" * 40 + "_calls-SECRET-0123456789"
UNIT = "/etc/systemd/system/fmnext-calls.service"
CORES = ("n1", "n2", "n3")


def calls_fleet(tmp_path, turn=False, **calls):
    fleet = render.load_fleet(FLEET, state_path=tmp_path / "none.json")
    fleet["calls"] = dict({"enabled": True}, **calls)
    secrets = list(fleet["app"].get("optional_secrets") or [])
    secrets.append("FORKMESH_CALLS_SECRET")
    if turn:
        fleet["turn"] = {"enabled": True}
        secrets.append("OFFICE_TURN_SECRET")
    fleet["app"]["optional_secrets"] = secrets
    return fleet


def rendered(fleet, node="n1"):
    files, manifest = render.render_core(fleet, node)
    return {path: content for path, (content, _) in files.items()}, manifest


def per_node_block(caddyfile, host):
    start = caddyfile.index("\n%s {\n" % host)
    end = caddyfile.index("\n}\n", start)
    return caddyfile[start:end]


def test_every_core_runs_the_server_with_the_same_files(tmp_path):
    fleet = calls_fleet(tmp_path)
    shapes = {}
    for name in CORES:
        files, manifest = rendered(fleet, name)
        shapes[name] = sorted(files)
        assert UNIT in files
        assert "fmnext-calls.service" in manifest["units"]
        assert manifest["credentials"]["fmnext-calls.service"] == [
            "FORKMESH_CALLS_SECRET"]
        assert manifest["calls"] is True
    assert shapes["n1"] == shapes["n2"] == shapes["n3"]
    units = {name: rendered(fleet, name)[0][UNIT] for name in CORES}
    normalized = {re.sub(r"\bn[0-9]\b|203\.0\.113\.[0-9]+|2001:db8:[0-9a-f:]*",
                         "X", text) for text in units.values()}
    assert len(normalized) == 1


def test_unit_settings_and_public_addresses(tmp_path):
    fleet = calls_fleet(tmp_path)
    files, _ = rendered(fleet, "n2")
    unit = files[UNIT]
    node = render.node_by_name(fleet, "n2")
    addresses = [node["public_ipv4"]] + (
        [node["public_ipv6"]] if node.get("public_ipv6") else [])
    lines = set(unit.splitlines())
    for line in (
            "User=fmnext-calls", "Group=fmnext-calls",
            "ExecStart=/usr/local/bin/forkmesh-calls",
            "LoadCredentialEncrypted=FORKMESH_CALLS_SECRET:/etc/forkmesh-next/"
            "creds/FORKMESH_CALLS_SECRET.cred",
            "ConditionPathExists=/usr/local/bin/forkmesh-calls",
            "Environment=FORKMESH_CALLS_HTTP=127.0.0.1:7881",
            "Environment=FORKMESH_CALLS_UDP=:3479",
            "Environment=FORKMESH_CALLS_ADDRESSES=%s" % ",".join(addresses),
            "Environment=FORKMESH_CALLS_ORIGINS=https://%s"
            % render.served_app_host(fleet),
            "MemoryHigh=200M", "MemoryMax=256M", "MemorySwapMax=0",
            "Restart=on-failure", "NoNewPrivileges=true",
            "ProtectSystem=strict", "CapabilityBoundingSet=",
            "MemoryDenyWriteExecute=true"):
        assert line in lines, line
    assert "ExecStartPre" not in unit and "/bin/sh" not in unit
    deny = re.search(r"^IPAddressDeny=(.*)$", unit, re.M).group(1).split()
    assert "localhost" not in deny
    for item in ("link-local", "10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16",
                 "100.64.0.0/10", "fc00::/7"):
        assert item in deny


def test_origins_cover_the_served_and_next_app_names(tmp_path):
    fleet = calls_fleet(tmp_path, origins=["https://forkmesh.com"])
    fleet["production"] = {"app_host": "app.forkmesh.com",
                           "world_host": "world.forkmesh.com"}
    assert render.calls_origins(fleet) == [
        "https://app.forkmesh.com", "https://%s" % fleet["app_host"],
        "https://forkmesh.com"]


def test_secret_is_a_credential_and_never_rendered(tmp_path, sandbox):
    (sandbox / "secrets.env").write_text("FORKMESH_CALLS_SECRET=%s\n" % SECRET)
    blob = []
    for name in CORES:
        files, manifest = rendered(calls_fleet(tmp_path), name)
        blob += list(files.values()) + [json.dumps(manifest)]
    assert SECRET not in "\n".join(blob)


def test_firewall_ids_and_app_env_on_every_core(tmp_path):
    fleet = calls_fleet(tmp_path)
    for name in CORES:
        files, _ = rendered(fleet, name)
        assert "udp dport { 443, 51820, 3479 } accept" in files["/etc/nftables.conf"]
        ids = json.loads(files["/etc/forkmesh-next/ids.json"])
        assert 7881 in ids["listeners"]["tcp"]
        assert 3479 in ids["listeners"]["udp"]
        assert ids["processes"]["service_users"]["fmnext-calls"] == {
            "units": ["fmnext-calls.service"],
            "exe": ["/usr/local/bin/forkmesh-calls"]}
        assert "fmnext-calls.service" in ids["lockdown"]["stop_units"]
        assert UNIT + ".d/50-fmnext-ids.conf" in files
        env = files["/etc/forkmesh-next/app.env"]
        assert ("FORKMESH_VAR_FORKMESH_CALLS_NODES="
                "wss://n1.next.forkmesh.com/calls/ws,"
                "wss://n2.next.forkmesh.com/calls/ws,"
                "wss://n3.next.forkmesh.com/calls/ws") in env
        assert "FORKMESH_CALLS_SECRET" not in env


def test_turn_and_calls_ports_live_side_by_side(tmp_path):
    files, _ = rendered(calls_fleet(tmp_path, turn=True))
    assert "udp dport { 443, 51820, 3478, 49160-49999, 3479 } accept" in \
        files["/etc/nftables.conf"]


def test_node_list_follows_the_fleet(tmp_path):
    base = FLEET.read_text()
    grown = next_fleet.upsert_core_block(base, "n4", "\n".join([
        "[[nodes]]", 'name = "n4"', 'role = "core"',
        'public_ipv4 = "203.0.113.14"', 'wg_ip = "10.77.0.4"',
        'rqlite_id = "n4"']))
    shrunk = next_fleet.remove_core_block(grown, "n1")
    for text, expected in ((grown, ["n1", "n2", "n3", "n4"]),
                           (shrunk, ["n2", "n3", "n4"])):
        path = tmp_path / "fleet.toml"
        path.write_text(text)
        fleet = render.load_fleet(path, state_path=tmp_path / "none.json")
        fleet["calls"] = {"enabled": True}
        fleet["app"]["optional_secrets"] = ["FORKMESH_CALLS_SECRET"]
        assert render.calls_urls(fleet) == [
            "wss://%s.next.forkmesh.com/calls/ws" % n for n in expected]


def test_caddy_proxies_calls_on_every_per_node_name_only(tmp_path):
    fleet = calls_fleet(tmp_path)
    for name in CORES:
        caddyfile = rendered(fleet, name)[0]["/etc/caddy/Caddyfile"]
        block = per_node_block(caddyfile, "%s.%s" % (name, fleet["domain"]))
        handle = ("\thandle /calls/* {\n\t\treverse_proxy 127.0.0.1:7881 {\n"
                  "\t\t\tflush_interval -1\n\t\t}\n\t}")
        assert handle in block
        assert block.index(handle) < block.rindex("\thandle {")
        assert caddyfile.count("handle /calls/*") == 1
        for host in render.served_hosts(fleet):
            assert "/calls/" not in per_node_block(caddyfile, host)


def test_calls_off_renders_nothing(tmp_path):
    fleet = render.load_fleet(FLEET, state_path=tmp_path / "none.json")
    files, manifest = rendered(fleet)
    assert not any("calls" in path for path in files)
    assert "3479" not in files["/etc/nftables.conf"]
    assert "/calls/" not in files["/etc/caddy/Caddyfile"]
    assert "CALLS" not in files["/etc/forkmesh-next/app.env"]
    assert manifest["calls"] is False
    ids = json.loads(files["/etc/forkmesh-next/ids.json"])
    assert "fmnext-calls" not in ids["processes"]["service_users"]
    assert 7881 not in ids["listeners"]["tcp"]


@pytest.mark.parametrize("calls, message", [
    ({"udp_port": 51820}, "1025-10239"),
    ({"udp_port": 443}, "1025-10239"),
    ({"http_port": 8080}, "already used"),
    ({"http_port": 4001}, "already used"),
    ({"udp_port": "3479"}, "integer"),
    ({"node": "n2"}, "unknown"),
    ({"origins": ["http://app.forkmesh.com"]}, "origins"),
    ({"enabled": "yes"}, "true or false"),
])
def test_bad_calls_settings_are_refused(tmp_path, calls, message):
    with pytest.raises(render.RenderError, match=message):
        render.render_core(calls_fleet(tmp_path, **calls), "n1")


def test_calls_port_must_not_clash_with_turn(tmp_path):
    with pytest.raises(render.RenderError, match="already used"):
        render.render_core(calls_fleet(tmp_path, turn=True, udp_port=3478), "n1")


def test_calls_needs_the_app_to_hold_the_secret(tmp_path):
    fleet = calls_fleet(tmp_path)
    fleet["app"]["optional_secrets"].remove("FORKMESH_CALLS_SECRET")
    with pytest.raises(render.RenderError, match="FORKMESH_CALLS_SECRET"):
        render.render_core(fleet, "n1")


def test_node_list_is_not_a_production_var(tmp_path):
    fleet = calls_fleet(tmp_path)
    fleet["production"] = {"vars": {"FORKMESH_CALLS_NODES": "x"}}
    with pytest.raises(render.RenderError, match="production.vars"):
        render.calls_settings(fleet)


def test_live_fleet_runs_calls_on_every_core_with_the_app_secret():
    with open(REPO / "deploy" / "fleet.toml", "rb") as handle:
        live = tomllib.load(handle)
    assert live["calls"]["enabled"] is True
    assert "node" not in live["calls"]
    assert "FORKMESH_CALLS_SECRET" in live["app"]["optional_secrets"]
    assert "FORKMESH_CALLS_NODES" not in live["production"]["vars"]


def install_with(tmp_path, sandbox, monkeypatch, name="n1"):
    (sandbox / "secrets.env").write_text("FORKMESH_CALLS_SECRET=%s\n" % SECRET)
    built = []
    monkeypatch.setattr(next_fleet, "build_calls", built.append)
    ssh = FakeSsh()
    next_fleet.install_calls(calls_fleet(tmp_path), name, "198.51.100.1", ssh,
                             Runner(execute=True))
    return ssh, built


def test_install_builds_puts_and_sends_the_secret_on_stdin(tmp_path, sandbox,
                                                           monkeypatch):
    ssh, built = install_with(tmp_path, sandbox, monkeypatch)
    assert built == [next_fleet.calls_binary()]
    stage, install = ssh.runs
    assert "install -d -o root -g root -m 0700 /tmp/fmnext-calls" in stage[1]
    assert ssh.puts == [("198.51.100.1", str(next_fleet.calls_binary()),
                         "/tmp/fmnext-calls/forkmesh-calls")]
    script = install[1]
    assert install[2] == SECRET.encode()
    assert all(SECRET not in s for _, s, _ in ssh.runs)
    assert "useradd --system" in script and "fmnext-calls" in script
    assert "systemd-creds encrypt --with-key=host" in script
    assert "/run/credentials/$unit/$name" in script
    assert "rm -rf" not in script
    assert subprocess.run(["bash", "-n", "-c", script]).returncode == 0


def test_install_is_a_no_op_off_and_on_replicas(tmp_path, sandbox, monkeypatch):
    built = []
    monkeypatch.setattr(next_fleet, "build_calls", built.append)
    off = render.load_fleet(FLEET, state_path=tmp_path / "none.json")
    ssh = FakeSsh()
    next_fleet.install_calls(off, "n1", "198.51.100.1", ssh, Runner(execute=True))
    next_fleet.install_calls(calls_fleet(tmp_path), "a1", "198.51.100.2", ssh,
                             Runner(execute=True))
    assert ssh.runs == [] and ssh.puts == [] and built == []
    assert next_ids.expect_step(off, ssh, Runner(execute=True), "n1", "calls") is None
    with pytest.raises(ToolError, match="core nodes only"):
        next_fleet.install(calls_fleet(tmp_path), "r1", ssh,
                           Runner(execute=False), ("calls",))


def test_secret_is_kept_generated_and_required(tmp_path):
    fleet = calls_fleet(tmp_path)
    fleet["app"]["vault"] = True
    for name in ("n1", "n2", "n3"):
        assert "FORKMESH_CALLS_SECRET" in next_fleet.node_local_credential_names(
            fleet, name)
        assert "FORKMESH_CALLS_SECRET" not in next_fleet.stale_credential_names(
            fleet, name)
    assert "FORKMESH_CALLS_SECRET" in next_fleet.stale_credential_names(fleet, "a1")
    assert "FORKMESH_CALLS_SECRET" in next_fleet.required_secret_names(fleet)
    assert len(next_fleet.DEFAULT_SECRETS["FORKMESH_CALLS_SECRET"]()) >= 32


@pytest.mark.parametrize("value", ["", "short", "x" * 40 + " space"])
def test_secret_must_be_long_and_url_safe(tmp_path, value):
    with pytest.raises(ToolError, match="FORKMESH_CALLS_SECRET"):
        next_fleet.calls_payloads(calls_fleet(tmp_path),
                                  {"FORKMESH_CALLS_SECRET": value})


def test_secret_must_not_reuse_another_secret(tmp_path):
    with pytest.raises(ToolError, match="reuse"):
        next_fleet.calls_payloads(calls_fleet(tmp_path), {
            "FORKMESH_CALLS_SECRET": SECRET, "OFFICE_TURN_SECRET": SECRET})


def test_calls_step_runs_in_rolls_and_installs_before_files():
    for steps in (next_fleet.ROLL_STEPS, next_fleet.INSTALL_STEPS):
        assert steps.index("calls") < steps.index("files")
    assert "calls" in next_fleet.CORE_ONLY_STEPS
    assert "calls" not in next_fleet.RUNNER_STEPS
    assert "calls" not in next_fleet.APP_STEPS


def test_files_step_restarts_the_server_when_its_unit_changes(tmp_path):
    _, manifest = rendered(calls_fleet(tmp_path))
    script = next_fleet.files_script(manifest, "/tmp/x.tar.gz")
    assert "calls_unit_before=$(" in script
    assert "systemctl restart fmnext-calls.service" in script
    assert "fmnext-calls.service" in next_fleet.start_script(manifest)
    _, off = rendered(render.load_fleet(FLEET, state_path=tmp_path / "none.json"))
    assert "fmnext-calls" not in next_fleet.files_script(off, "/tmp/x.tar.gz")


def test_regenerated_fleet_toml_keeps_the_calls_section():
    current = "[calls]\nenabled = true\nudp_port = 3480\n\n[dns]\n"
    text = next_fleet.fleet_toml_text({}, {}, current=current)
    assert tomllib.loads(text)["calls"] == {"enabled": True, "udp_port": 3480}


def test_aws_ingress_mirrors_the_calls_port(tmp_path):
    fleet = calls_fleet(tmp_path)
    rules = next_aws.ingress_rules("core", fleet)
    assert ("udp", 3479, 3479, "0.0.0.0/0") in rules
    assert ("tcp", 7881, 7881, "0.0.0.0/0") not in rules
    off = render.load_fleet(FLEET, state_path=tmp_path / "none.json")
    assert ("udp", 3479, 3479, "0.0.0.0/0") not in next_aws.ingress_rules(
        "core", off)


def test_remove_core_stops_the_server():
    assert "fmnext-calls.service" in next_remove_core.STOP_UNITS
