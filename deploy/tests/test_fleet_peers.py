from __future__ import annotations

import sys
import types
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "deploy" / "core"))

import fleet_peers  # noqa: E402

CORE_KEY = "8cB/cHmWnp+prJ0MsLNmhFmsCexBYyod2MMPdxwh1nE="
KEY_A1 = "ju2Pcr0WX/MOpn7qdHmy1Af91HaBakjxx92ovt8GmWI="
KEY_A2 = "Ab8pL/fq7/2LiPVGedKusFdi7rr9aki9vFTT2zU+ax0="


def row(name="a1", key=KEY_A1, ip="45.76.10.1", wg=None, state="serving",
        created_by="autoscaler"):
    return {"name": name, "wg_pubkey": key, "public_ipv4": ip,
            "wg_ip": wg or "10.77.0.%d" % (20 + int(name[1:])), "state": state,
            "created_by": created_by}


def test_only_valid_autoscaled_replicas_are_wanted():
    rows = [row(), row("a2", KEY_A2, wg="10.77.0.1"), row("n1", "x"),
            row("a3", CORE_KEY, wg="10.77.0.23"), row("a4", "bad", wg="10.77.0.24"),
            row("a5", "5" * 43 + "=", ip="10.0.0.5"), row("a6", "6" * 43 + "=",
                                                            created_by="desktop"),
            row("a7", "7" * 43 + "=", state="booting"), row("b1", "8" * 43 + "=")]
    wanted = fleet_peers.wanted(rows, core_keys={CORE_KEY})
    assert list(wanted) == [KEY_A1]
    assert wanted[KEY_A1] == {"name": "a1", "wg_ip": "10.77.0.21",
                              "endpoint": "45.76.10.1"}


def test_plan_never_touches_core_peers():
    current = {CORE_KEY: "10.77.0.1/32", KEY_A2: "10.77.0.22/32"}
    desired = fleet_peers.wanted([row()], core_keys={CORE_KEY})
    commands = fleet_peers.plan(current, desired, 51820)
    assert ["wg", "set", "wg0", "peer", KEY_A2, "remove"] in commands
    assert any(c[4] == KEY_A1 and "allowed-ips" in c for c in commands)
    assert not any(CORE_KEY in c for c in commands)
    hijack = {CORE_KEY: {"name": "a1", "wg_ip": "10.77.0.21", "endpoint": "1.1.1.1"}}
    assert fleet_peers.plan(current, hijack, 51820) == [
        ["wg", "set", "wg0", "peer", KEY_A2, "remove"]]


def test_round_applies_peers_nft_set_and_certsync_allowlist(tmp_path):
    calls = []

    def runner(command, stdin=None, check=True):
        calls.append((command, stdin))
        out = "private\tpub\t51820\toff\n%s\t(none)\t1.2.3.4:51820\t10.77.0.1/32\t0\t0\t0\t25\n" % CORE_KEY
        return types.SimpleNamespace(stdout=out if command[:2] == ["wg", "show"] else "")
    curlrc = tmp_path / "rqlite.curlrc"
    curlrc.write_text('user = "fmnext:secretpw"\n')
    config = {"rqlite_url": "http://127.0.0.1:4001", "curlrc": str(curlrc),
              "replicas_file": str(tmp_path / "run" / "replicas.json")}
    report = fleet_peers.round_once(config, runner=runner,
                                    fetch=lambda url, creds: [row()])
    assert report == {"peers": ["a1"], "ips": ["10.77.0.21"]}
    nft = [s for c, s in calls if c[0] == "nft"][0]
    assert "flush set inet filter autoscaled" in nft
    assert "add element inet filter autoscaled { 10.77.0.21 }" in nft
    assert '"10.77.0.21"' in (tmp_path / "run" / "replicas.json").read_text()
    empty = fleet_peers.round_once(config, runner=runner, fetch=lambda url, creds: [])
    assert empty["ips"] == []


def autoscale_fleet(extra=None):
    import render
    state = {"wg_pubkeys": {"n1": "k" * 42 + "A=", "n2": "m" * 42 + "E=",
                            "n3": "p" * 42 + "I="}}
    state.update(extra or {})
    fleet = render.load_fleet(REPO / "deploy" / "fleet.example.toml", state=state)
    fleet["autoscale"] = {"enabled": True}
    return render, fleet


def test_core_render_with_autoscale_adds_the_agent_set_and_replica_secret():
    render, fleet = autoscale_fleet()
    files, manifest = render.render_core(fleet, "n1")
    text = {p: c for p, (c, _) in files.items()}
    assert "fmnext-fleet-peers.service" in manifest["units"]
    assert "/opt/forkmesh-next/bin/fmnext-fleet-peers" in text
    nft = text["/etc/nftables.conf"]
    assert "set autoscaled {" in nft and "ip saddr @autoscaled" in nft
    certsync = text["/etc/forkmesh-next/certsync.json"]
    assert "AUTOSCALE_CERTSYNC_SECRET" in certsync and "replicas.json" in certsync
    assert "AUTOSCALE_CERTSYNC_SECRET" in text["/etc/systemd/system/fmnext-certsync.service"]
    assert "AUTOSCALE_CERTSYNC_SECRET" not in text[
        "/etc/systemd/system/fmnext-cert-sync.service"]


def test_core_render_without_autoscale_is_unchanged():
    import render
    state = {"wg_pubkeys": {"n1": "k" * 42 + "A=", "n2": "m" * 42 + "E=",
                            "n3": "p" * 42 + "I="}}
    fleet = render.load_fleet(REPO / "deploy" / "fleet.example.toml", state=state)
    files, manifest = render.render_core(fleet, "n1")
    assert "fmnext-fleet-peers.service" not in manifest["units"]
    assert "@autoscaled" not in files["/etc/nftables.conf"][0]


def test_replica_render_is_a_non_voter_replica_follower():
    render, fleet = autoscale_fleet({"autoscaled": {"a1": {
        "kind": "app", "instance_id": "x", "public_ipv4": "45.76.10.1"}}})
    files, manifest = render.render_core(fleet, "a1")
    text = {p: c for p, (c, _) in files.items()}
    assert manifest.get("replica") is True
    assert "forkmesh-mirror-node.service" not in manifest["units"]
    assert "fmnext-fleet-peers.service" not in manifest["units"]
    assert "-raft-non-voter" in text["/etc/systemd/system/rqlite.service"]
    assert "-bootstrap-expect" not in text["/etc/systemd/system/rqlite.service"]
    assert "FORKMESH_CLUSTER_ROLE=replica" in text["/etc/forkmesh-next/app.env"]
    assert "FORKMESH_DNS_RECONCILER=off" in text["/etc/forkmesh-next/app.env"]
    assert '"issue": false' in text["/etc/forkmesh-next/certsync.json"]
    assert "a1.next.forkmesh.com" not in text["/etc/caddy/Caddyfile"]
    assert "set autoscaled" not in text["/etc/nftables.conf"]
    assert "10.77.0.21/24" in text["/etc/wireguard/wg0.conf"]


def test_burst_runner_render_serves_no_owner_and_is_marked_burst():
    render, fleet = autoscale_fleet({"autoscaled": {"b1": {
        "kind": "burst", "instance_id": "x", "public_ipv4": "45.76.10.2"}}})
    files, manifest = render.render_runner(fleet, "b1")
    assert manifest["burst"] is True
    overrides = files["/etc/forkmesh-next/mirror-node.overrides.json"][0]
    assert '"burst": true' in overrides and '"role": "runner"' in overrides
