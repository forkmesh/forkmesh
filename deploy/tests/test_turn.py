import ipaddress
import json
import re
import tomllib
from pathlib import Path

import pytest

import next_fleet
import next_ids
import render
from fakes import FakeSsh
from next_common import Runner, ToolError

REPO = Path(__file__).resolve().parents[2]
FLEET = REPO / "deploy" / "fleet.example.toml"
SECRET = "T" * 40 + "_turn-SECRET-0123456789"


def turn_fleet(tmp_path, **turn):
    fleet = render.load_fleet(FLEET, state_path=tmp_path / "none.json")
    fleet["turn"] = dict({"enabled": True}, **turn)
    fleet["app"]["optional_secrets"] = list(
        fleet["app"].get("optional_secrets") or []) + ["OFFICE_TURN_SECRET"]
    return fleet


def rendered(fleet, node="n1"):
    files, manifest = render.render_core(fleet, node)
    return {path: content for path, (content, _) in files.items()}, manifest


def conf_values(text, key):
    return [line.split("=", 1)[1] for line in text.splitlines()
            if line.startswith(key + "=")]


def denied_networks(text):
    out = []
    for value in conf_values(text, "denied-peer-ip"):
        low, _, high = value.partition("-")
        low = ipaddress.ip_address(low)
        high = ipaddress.ip_address(high or str(low))
        out.append((low, high))
    return out


def is_denied(text, address):
    address = ipaddress.ip_address(address)
    return any(low.version == address.version and low <= address <= high
               for low, high in denied_networks(text))


def test_turnserver_conf_denies_private_loopback_link_local_and_metadata(tmp_path):
    files, _ = rendered(turn_fleet(tmp_path))
    conf = files["/etc/forkmesh-next/turnserver.conf"]
    for address in ("10.77.0.2", "10.0.0.1", "127.0.0.1", "169.254.169.254",
                    "172.16.5.4", "192.168.1.1", "100.64.0.1", "0.0.0.1",
                    "224.0.0.251", "255.255.255.255", "198.18.0.1",
                    "::1", "fd00:ec2::254", "fe80::1", "ff02::1",
                    "::ffff:10.0.0.1", "::ffff:169.254.169.254",
                    "64:ff9b::a00:1", "2002:a00:1::1", "2001:0:a00:1::1"):
        assert is_denied(conf, address), address
    for address in ("93.184.215.14", "1.1.1.1", "2606:4700::1111",
                    "9.9.9.9"):
        assert not is_denied(conf, address), address


def test_no_deny_range_starts_at_the_unspecified_address(tmp_path):
    files, _ = rendered(turn_fleet(tmp_path))
    conf = files["/etc/forkmesh-next/turnserver.conf"]
    for low, _ in denied_networks(conf):
        assert low != ipaddress.ip_address("::"), \
            "coturn 4.6 treats :: as matching every IPv4 peer"


def test_turnserver_conf_settings(tmp_path):
    fleet = turn_fleet(tmp_path)
    files, _ = rendered(fleet, "n2")
    conf = files["/etc/forkmesh-next/turnserver.conf"]
    lines = set(conf.splitlines())
    for flag in ("use-auth-secret", "no-cli", "no-multicast-peers",
                 "no-tcp-relay", "fingerprint", "no-dtls", "no-rfc5780",
                 "no-software-attribute", "realm=forkmesh.com",
                 "min-port=49160", "max-port=49999", "user-quota=12",
                 "total-quota=1200", "listening-port=3478",
                 "tls-listening-port=5349"):
        assert flag in lines, flag
    node = render.node_by_name(fleet, "n2")
    assert conf_values(conf, "listening-ip")[0] == node["public_ipv4"]
    assert conf_values(conf, "relay-ip")[0] == node["public_ipv4"]
    host = render.served_app_host(fleet)
    assert conf_values(conf, "cert") == [
        "/var/lib/forkmesh-next/certsync/export/%s.crt" % host]
    assert conf_values(conf, "pkey") == [
        "/var/lib/forkmesh-next/certsync/export/%s.key" % host]
    assert "static-auth-secret" not in conf
    assert not any(line.startswith(("listening-ip=10.", "relay-ip=10."))
                   for line in lines)


def test_secret_is_a_credential_and_never_rendered(tmp_path, sandbox):
    (sandbox / "secrets.env").write_text("OFFICE_TURN_SECRET=%s\n" % SECRET)
    files, manifest = rendered(turn_fleet(tmp_path))
    blob = "\n".join(files.values()) + json.dumps(manifest)
    assert SECRET not in blob
    unit = files["/etc/systemd/system/fmnext-turn.service"]
    assert ("LoadCredentialEncrypted=OFFICE_TURN_SECRET:/etc/forkmesh-next/"
            "creds/OFFICE_TURN_SECRET.cred") in unit
    assert "ExecStartPre=+/opt/forkmesh-next/bin/fmnext-turn-config" in unit
    assert "User=turnserver" in unit and "CapabilityBoundingSet=\n" in unit
    deny = re.search(r"^IPAddressDeny=(.*)$", unit, re.M).group(1).split()
    for item in ("localhost", "link-local", "10.0.0.0/8", "172.16.0.0/12",
                 "192.168.0.0/16", "100.64.0.0/10", "fc00::/7"):
        assert item in deny
    assert manifest["credentials"]["fmnext-turn.service"] == ["OFFICE_TURN_SECRET"]
    assert manifest["turn"] is True
    assert "fmnext-turn.service" in manifest["units"]
    assert "fmnext-turn-cert.path" in manifest["units"]
    script = files["/opt/forkmesh-next/bin/fmnext-turn-config"]
    assert "$CREDENTIALS_DIRECTORY/OFFICE_TURN_SECRET" in script
    assert "static-auth-secret=%s" in script
    assert "turnserver -c" not in script


def test_firewall_ids_and_app_env_follow_the_turn_ports(tmp_path):
    fleet = turn_fleet(tmp_path)
    files, _ = rendered(fleet)
    nft = files["/etc/nftables.conf"]
    assert "tcp dport { 22, 80, 443, 3478, 5349 } accept" in nft
    assert "udp dport { 443, 51820, 3478, 49160-49999 } accept" in nft
    ids = json.loads(files["/etc/forkmesh-next/ids.json"])
    assert {3478, 5349} <= set(ids["listeners"]["tcp"])
    assert 3478 in ids["listeners"]["udp"]
    assert ids["processes"]["service_users"]["turnserver"] == {
        "units": ["fmnext-turn.service"], "exe": ["/usr/bin/turnserver"]}
    assert "fmnext-turn.service" in ids["lockdown"]["stop_units"]
    assert "/etc/systemd/system/fmnext-turn.service.d/50-fmnext-ids.conf" in files
    env = files["/etc/forkmesh-next/app.env"]
    host = render.served_app_host(fleet)
    assert ("FORKMESH_VAR_OFFICE_TURN_URLS=turn:%s:3478?transport=udp,"
            "turn:%s:3478?transport=tcp,turns:%s:5349?transport=tcp"
            % (host, host, host)) in env
    assert ("FORKMESH_VAR_OFFICE_STUN_URLS=stun:%s:3478,"
            "stun:stun.cloudflare.com:3478" % host) in env


def test_turn_off_renders_nothing(tmp_path):
    fleet = render.load_fleet(FLEET, state_path=tmp_path / "none.json")
    files, manifest = rendered(fleet)
    assert not any("turn" in path for path in files)
    assert "3478" not in files["/etc/nftables.conf"]
    assert "OFFICE_TURN" not in files["/etc/forkmesh-next/app.env"]
    assert manifest["turn"] is False
    assert "fmnext-turn.service" not in json.loads(
        files["/etc/forkmesh-next/ids.json"])["lockdown"]["stop_units"]


@pytest.mark.parametrize("turn, message", [
    ({"relay_min_port": 40000, "relay_max_port": 50000}, "relay ports"),
    ({"relay_min_port": 1000, "relay_max_port": 2000}, "relay ports"),
    ({"user_quota": 0}, "positive integer"),
    ({"user_quota": 50, "total_quota": 10}, "user_quota"),
    ({"bogus": 1}, "unknown"),
    ({"host": "evil host"}, "DNS name"),
])
def test_bad_turn_settings_are_refused(tmp_path, turn, message):
    with pytest.raises(render.RenderError, match=message):
        render.render_core(turn_fleet(tmp_path, **turn), "n1")


def test_turn_needs_the_app_to_hold_the_secret(tmp_path):
    fleet = turn_fleet(tmp_path)
    fleet["app"]["optional_secrets"].remove("OFFICE_TURN_SECRET")
    with pytest.raises(render.RenderError, match="OFFICE_TURN_SECRET"):
        render.render_core(fleet, "n1")


def test_live_fleet_turns_the_relay_on_with_the_app_secret():
    with open(REPO / "deploy" / "fleet.toml", "rb") as handle:
        live = tomllib.load(handle)
    assert live["turn"]["enabled"] is True
    assert "OFFICE_TURN_SECRET" in live["app"]["optional_secrets"]


def test_turn_secret_is_pushed_kept_and_generated(tmp_path, sandbox):
    fleet = turn_fleet(tmp_path)
    (sandbox / "secrets.env").write_text("OFFICE_TURN_SECRET=%s\n" % SECRET)
    ssh = FakeSsh()
    next_fleet.install_turn(fleet, "n1", "198.51.100.1", ssh, Runner(execute=True))
    package, encrypt = ssh.runs
    assert "systemctl mask coturn.service" in package[1]
    assert package[1].index("mask") < package[1].index("apt-get")
    assert "name=OFFICE_TURN_SECRET" in encrypt[1]
    assert encrypt[2] == SECRET.encode()
    assert all(SECRET not in script for _, script, _ in ssh.runs)
    assert "OFFICE_TURN_SECRET" in next_fleet.node_local_credential_names(fleet, "n1")
    assert "OFFICE_TURN_SECRET" not in next_fleet.stale_credential_names(fleet, "n1")
    assert "OFFICE_TURN_SECRET" in next_fleet.required_secret_names(fleet)
    assert len(next_fleet.DEFAULT_SECRETS["OFFICE_TURN_SECRET"]()) >= 32


@pytest.mark.parametrize("value", ["", "short", "x" * 40 + " space"])
def test_turn_secret_must_be_long_and_url_safe(tmp_path, sandbox, value):
    fleet = turn_fleet(tmp_path)
    with pytest.raises(ToolError, match="OFFICE_TURN_SECRET"):
        next_fleet.turn_payloads(fleet, {"OFFICE_TURN_SECRET": value})


def test_turn_secret_must_not_reuse_another_secret(tmp_path):
    with pytest.raises(ToolError, match="reuse"):
        next_fleet.turn_payloads(turn_fleet(tmp_path), {
            "OFFICE_TURN_SECRET": SECRET, "FORKMESH_MESH_SECRET": SECRET})


def test_turn_step_runs_in_rolls_and_installs_before_files():
    for steps in (next_fleet.ROLL_STEPS, next_fleet.INSTALL_STEPS,
                  next_fleet.APP_STEPS):
        assert steps.index("turn") < steps.index("files")
    assert "turn" in next_fleet.CORE_ONLY_STEPS
    assert "turn" not in next_fleet.RUNNER_STEPS


def test_turn_step_is_a_no_op_when_off_and_refused_on_runners(tmp_path, sandbox):
    off = render.load_fleet(FLEET, state_path=tmp_path / "none.json")
    ssh = FakeSsh()
    next_fleet.install_turn(off, "n1", "198.51.100.1", ssh, Runner(execute=True))
    assert ssh.runs == []
    assert next_ids.expect_step(off, ssh, Runner(execute=True), "n1", "turn") is None
    with pytest.raises(ToolError, match="core nodes only"):
        next_fleet.install(turn_fleet(tmp_path), "r1", ssh,
                           Runner(execute=False), ("turn",))


def test_start_restarts_the_relay_to_load_new_config(tmp_path):
    _, manifest = rendered(turn_fleet(tmp_path))
    assert "systemctl try-restart fmnext-turn.service" in \
        next_fleet.start_script(manifest)


def test_regenerated_fleet_toml_keeps_the_turn_section():
    current = "[turn]\nenabled = true\nuser_quota = 6\n\n[dns]\n"
    text = next_fleet.fleet_toml_text({}, {}, current=current)
    assert tomllib.loads(text)["turn"] == {"enabled": True, "user_quota": 6}
