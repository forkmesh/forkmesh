"""tools/next_ids.py and its wiring into next_fleet / next_deploy: signed
expected-change manifests (a forged one is refused by the IDS), the
operator release, test-fire, and no secret on a command line."""

import hashlib
import io
import json
import os
import subprocess
from pathlib import Path

import pytest

pytest.importorskip("cryptography")

import ids  # noqa: E402
import ids_records  # noqa: E402
import next_fleet  # noqa: E402
import next_ids  # noqa: E402
import render  # noqa: E402
from fakes import FakeSsh  # noqa: E402
from next_common import GuardError, Runner, ToolError  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
FLEET = REPO / "deploy" / "fleet.example.toml"
IPS = {"n1": "203.0.113.11", "n2": "203.0.113.12", "n3": "203.0.113.13",
       "r1": "203.0.113.21"}
MASTER = "ids-master-SECRET-" + "m" * 30


def fleet(tmp_path, operator_public=""):
    text = FLEET.read_text().replace('operator_public_key = ""',
                                     'operator_public_key = "%s"'
                                     % operator_public)
    path = tmp_path / "fleet.toml"
    path.write_text(text)
    return render.load_fleet(path, state_path=tmp_path / "none.json")


def secrets(sandbox, master=MASTER):
    (sandbox / "secrets.env").write_text(
        "RQLITE_PASSWORD=rq-SECRET\nFMNEXT_IDS_MASTER=%s\n" % master)


def quiet(execute=True):
    return Runner(execute=execute, out=io.StringIO())


def ok_expect(record):
    def answer(host, script, stdin):
        record.append((host, script, json.loads(stdin)))
        return subprocess.CompletedProcess(["ssh"], 0, b'{"ok": true}', b"")
    return (lambda h, s: "fmnext-ids expect" in s, answer)


def test_node_keys_are_per_node_and_manifests_verify(tmp_path):
    assert next_ids.node_key(MASTER, "n1") != next_ids.node_key(MASTER, "n2")
    now = ids.now_ms()
    manifest = next_ids.build_manifest(
        "n2", next_ids.node_key(MASTER, "n2"), {"/a": "00"}, ["/b"],
        ["users"], "x", now_ms=now)
    assert ids.verify_manifest(manifest, next_ids.node_key(MASTER, "n2"),
                               "n2", now, {}) is None
    # n1's key (a compromised n1) cannot authorize a change on n2.
    assert ids.verify_manifest(manifest, next_ids.node_key(MASTER, "n1"),
                               "n2", now, {}) == "bad_mac"
    with pytest.raises(GuardError):
        next_ids.build_manifest("n1", "k", allow=["everything"])
    with pytest.raises(GuardError):
        next_ids.build_manifest("n1", "k", rescan=["relative/path"])
    with pytest.raises(GuardError):
        next_ids.build_manifest("n1", "k", minutes=600)


def test_push_expectation_needs_the_master_and_stops_on_refusal(tmp_path,
                                                                sandbox):
    data = fleet(tmp_path)
    ssh = FakeSsh()
    assert next_ids.push_expectation(data, ssh, quiet(), "n1",
                                     rescan=["/x"]) is None
    assert ssh.runs == []             # no key: nothing is sent
    secrets(sandbox)
    sent = []
    ssh = FakeSsh([ok_expect(sent)])
    assert next_ids.push_expectation(data, ssh, quiet(), "n1",
                                     rescan=["/x"], reason="t") is True
    host, script, manifest = sent[0]
    assert host == IPS["n1"] and MASTER not in script
    assert MASTER not in json.dumps(manifest)
    assert ids.verify_manifest(manifest, next_ids.node_key(MASTER, "n1"),
                               "n1", ids.now_ms(), {}) is None
    refused = FakeSsh([(lambda h, s: "expect" in s, b'{"ok": false, '
                        b'"error": "bad_mac"}')])
    with pytest.raises(ToolError, match="bad_mac"):
        next_ids.push_expectation(data, refused, quiet(), "n1", rescan=["/x"])
    missing = FakeSsh([(lambda h, s: "expect" in s, subprocess.CompletedProcess(
        ["ssh"], 3, b'{"ok": false, "error": "not_running"}', b""))])
    assert next_ids.push_expectation(data, missing, quiet(), "n1",
                                     rescan=["/x"]) is None
    # Dry run: nothing goes out.
    dry = FakeSsh()
    next_ids.push_expectation(data, dry, quiet(False), "n1", rescan=["/x"])
    assert dry.runs == []


def test_files_step_expects_exactly_the_rendered_content(tmp_path, sandbox):
    data = fleet(tmp_path)
    files, _manifest = render.render_core(data, "n1")
    expected, rescan = next_ids.files_expectation(data, "n1")
    content = files["/opt/forkmesh-next/bin/fmnext-ids"][0]
    assert expected["/opt/forkmesh-next/bin/fmnext-ids"] == hashlib.sha256(
        content.encode()).hexdigest()
    assert expected["/etc/systemd/system/fmnext-cert-pull.timer"] is None
    assert "/etc/wireguard/wg0.conf" not in expected   # edited on the node
    assert "/etc/wireguard/wg0.conf" in rescan


def test_install_tells_the_ids_before_each_step(tmp_path, sandbox,
                                                 monkeypatch):
    monkeypatch.setattr(next_fleet, "verify_target_instance",
                        lambda *a: None)
    secrets(sandbox)
    data = fleet(tmp_path)
    sent = []
    ssh = FakeSsh([ok_expect(sent)])
    next_fleet.install(data, "n1", ssh, quiet(), ("start",), vultr=None)
    scripts = [s for _, s, _ in ssh.runs]
    assert "fmnext-ids expect" in scripts[0]
    assert "systemctl enable --now" in scripts[1]
    assert "systemctl try-restart fmnext-ids.service" in scripts[1]
    assert sent[0][2]["rescan"] == sorted(next_ids.WANTS + next_ids.AUTHORIZED_KEYS)
    assert sent[0][2]["allow"] == ["listeners", "modules", "processes"]


def test_ids_keygen_step_records_the_public_key(tmp_path, sandbox,
                                                monkeypatch):
    monkeypatch.setattr(next_fleet, "verify_target_instance",
                        lambda *a: None)
    data = fleet(tmp_path)
    public = ids_records.public_key(ids_records.generate_private())
    ssh = FakeSsh([(lambda h, s: "sign.key" in s, (public + "\n").encode())])
    state = tmp_path / "state.json"
    next_fleet.install(data, "n2", ssh, quiet(), ("ids",), vultr=None,
                       state_path=state)
    assert json.loads(state.read_text())["ids_pubkeys"] == {"n2": public}
    # The private key is generated on the node and never printed.
    assert "Ed25519PrivateKey.generate()" in ssh.runs[0][1]
    rerendered = render.load_fleet(tmp_path / "fleet.toml", state_path=state)
    env = render.render_core(rerendered, "n1")[0][
        "/etc/forkmesh-next/app.env"][0]
    assert "FORKMESH_IDS_PUBKEYS=n2=%s" % public in env


def test_sshd_logs_verbose_for_the_session_classifier():
    assert "LogLevel VERBOSE" in next_fleet.SSHD_SETTINGS
    assert "LogLevel VERBOSE" in next_fleet.base_script("core")


def test_keygen_writes_an_owner_only_operator_key(tmp_path, sandbox):
    data = fleet(tmp_path)
    public = next_ids.keygen(data, quiet())
    path = sandbox / "ids-operator.key"
    assert (path.stat().st_mode & 0o777) == 0o400
    assert ids_records.public_key(next_ids.load_operator_key()) == public
    assert next_ids.keygen(data, quiet()) == public    # idempotent


def release_ssh(node_rows, voters=("n1", "n2", "n3"), ids_ok=True):
    """Fake cluster: n2 is the healthy peer; captures the stored row."""
    stored = []
    released = []

    def status(host, script, stdin):
        return json.dumps({"store": {"raft": {"state": "Follower"},
                                     "leader": {"node_id": "n1"}}}).encode()

    def nodes(host, script, stdin):
        return json.dumps({"nodes": [{"id": v, "voter": True}
                                     for v in voters]}).encode()

    def sql(host, script, stdin):
        body = json.loads(stdin)
        if "/db/query" in script:
            return json.dumps({"results": [{"rows": [
                {"id": rid} for rid in node_rows]}]}).encode()
        stored.append(body[0])
        return b'{"results": [{}]}'

    def ids_cmd(host, script, stdin):
        if " release" in script:
            released.append(json.loads(stdin))
            reply = {"ok": ids_ok, "note": "released" if ids_ok else "bad"}
        else:
            reply = {"ok": True, "lockdown": True,
                     "lockdownIncident": {"incident": "i1"}}
        return json.dumps(reply).encode()
    ssh = FakeSsh([
        (lambda h, s: "/status" in s, status),
        (lambda h, s: "/nodes" in s, nodes),
        (lambda h, s: "/db/" in s, sql),
        (lambda h, s: "fmnext-ids" in s, ids_cmd)])
    return ssh, stored, released


def test_release_is_operator_signed_and_unseals_from_the_desktop(
        tmp_path, sandbox, monkeypatch):
    secrets(sandbox)
    next_ids.keygen(fleet(tmp_path), quiet())
    public = ids_records.public_key(next_ids.load_operator_key())
    data = fleet(tmp_path, public)
    ssh, stored, released = release_ssh(["q1abcdefgh", "q2abcdefgh"])
    unsealed = []
    import next_vault
    monkeypatch.setattr(next_vault, "unseal", lambda f, s, r, names:
                        unsealed.append(names) or 0)
    assert next_ids.release(data, ssh, quiet(), "n3", "checked") == 0
    row = released[0]
    assert stored[0][1:] == ids_records.insert_statement(row)[1:]
    checked = ids_records.verified(row, {}, public)
    assert checked and checked[0] == "release"
    assert checked[1]["clears"] == ["q1abcdefgh", "q2abcdefgh"]
    # Stored through a healthy peer, never through the released node.
    peer_hosts = {h for h, s, _ in ssh.runs if "/db/" in s}
    assert peer_hosts and IPS["n3"] not in peer_hosts
    assert unsealed == [["n3"]]
    # The operator key never travels.
    key_text = (sandbox / "ids-operator.key").read_text().strip()
    assert all(key_text not in s and key_text.encode() not in (i or b"")
               for _, s, i in ssh.runs)


def test_release_refuses_a_node_that_is_no_longer_a_voter(tmp_path, sandbox,
                                                          monkeypatch):
    secrets(sandbox)
    next_ids.keygen(fleet(tmp_path), quiet())
    public = ids_records.public_key(next_ids.load_operator_key())
    data = fleet(tmp_path, public)
    ssh, stored, released = release_ssh([], voters=("n1", "n2"))
    assert next_ids.main(["--fleet", str(tmp_path / "fleet.toml"), "release",
                          "n3", "--execute"], ssh=ssh) == 2
    assert stored == [] and released == []


def test_release_refuses_a_mismatched_operator_key(tmp_path, sandbox):
    secrets(sandbox)
    next_ids.keygen(fleet(tmp_path), quiet())
    other = ids_records.public_key(ids_records.generate_private())
    data = fleet(tmp_path, other)
    ssh, stored, released = release_ssh([])
    with pytest.raises(GuardError):
        next_ids.release(data, ssh, quiet(), "n2")
    assert stored == [] and released == []


def test_test_fire_waits_for_delivery(tmp_path, sandbox):
    data = fleet(tmp_path)
    polls = []

    def ids_cmd(host, script, stdin):
        if "test-fire" in script:
            return b'{"ok": true, "incident": "inc123", "delivery": {"journal": [true, "logged"], "rqlite": [true, "recorded"]}}'
        polls.append(host)
        delivered = len(polls) >= 2
        return json.dumps({"ok": True, "incident": {
            "id": "inc123", "delivered_ms": 5 if delivered else None,
            "delivery": json.dumps({"by": "n2", "pings": 1, "emails": 1})
            if delivered else None}}).encode()
    ssh = FakeSsh([(lambda h, s: "fmnext-ids" in s, ids_cmd)])
    out = io.StringIO()
    code = next_ids.test_fire(data, ssh, Runner(execute=True, out=out), "n1",
                              sleep=lambda s: None)
    assert code == 0 and polls == [IPS["n1"], IPS["n1"]]
    assert "1 pings, 1 emails" in out.getvalue()


def test_fleet_toml_keeps_its_ids_table(tmp_path):
    current = FLEET.read_text().replace('ssh_allow = []',
                                        'ssh_allow = ["192.0.2.7"]')
    text = next_fleet.fleet_toml_text(
        {"fmnext-core-1": {"id": "x", "ip": "198.51.100.1"}}, {},
        current=current)
    assert 'ssh_allow = ["192.0.2.7"]' in text
