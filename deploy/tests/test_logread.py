import hashlib
import io
import json
import subprocess
import sys
from pathlib import Path

import pytest

import logread
import next_fleet
import render
import secret_gate
from fakes import FakeSsh
from next_common import GuardError, Runner

REPO = Path(__file__).resolve().parents[2]
FLEET = REPO / "deploy" / "fleet.example.toml"


def fleet(tmp_path):
    return render.load_fleet(FLEET, state_path=tmp_path / "none.json")


def test_secret_rules_are_the_secret_gate_contract():
    ours = [(rule_id, pattern, group)
            for rule_id, pattern, group in logread.SECRET_RULES]
    theirs = [(r["id"], r["pattern"], r["valueGroup"])
              for r in secret_gate.RULES]
    assert ours == theirs


@pytest.mark.parametrize("text, leaked", [
    ("GET /api/x?token=abc123def&page=2", "abc123def"),
    ("Authorization: Bearer eyJhbGciOiJIUzI1NiJ9.payload.sig", "eyJhbGci"),
    ('{"Authorization":["Basic dXNlcjpwYXNzd29yZA=="],"x":1}', "dXNlcjpw"),
    ('{"cookie": "session=s3cr3tcookie"}', "s3cr3tcookie"),
    ("password=hunter2hunter2", "hunter2"),
    ("DATA_KEY=Zm9vYmFyYmF6cXV4MTIzNDU2Nzg5MEFCQ0RFRkdISUpLTE1OT", "Zm9vYmFy"),
    ("using key ghp_" + "a" * 36, "ghp_aaaa"),
    ("clone https://fmnext:pa55w0rd@n1.next.forkmesh.com/git/x", "pa55w0rd"),
    ("rotate sk-ant-" + "b" * 30, "sk-ant-bbb"),
    ("token Ab3dEf6hIj9kLm2nOp5qRs8tUv1wXy4zAb7dEf0hIj3", "Ab3dEf6h"),
])
def test_redact_removes_secrets(text, leaked):
    assert leaked not in logread.redact(text)


def test_redact_keeps_ordinary_lines():
    for text in ("rqlite: failed to retrieve stats for registered reporter network",
                 "Traceback (most recent call last):",
                 'File "/opt/app/entry.py", line 12, in handler',
                 "merged 9f2c4d1e8a7b6c5d4e3f2a1b0c9d8e7f6a5b4c3d into main"):
        assert logread.redact(text) == text


def test_private_key_material_drops_the_whole_message():
    text = "-----BEGIN OPENSSH PRIVATE KEY-----\nb3BlbnNzaC1rZXk=\n"  # forkmesh-secret-scan:ignore-line
    assert logread.redact(text) == "<redacted: private key material>"


def test_parse_request_accepts_only_the_protocol():
    assert logread.parse_request("") == {"mode": "follow", "lines": 200}
    assert logread.parse_request("hello") == {"mode": "hello"}
    assert logread.parse_request("follow lines 99999")["lines"] == logread.MAX_LINES
    cursor = "s=abc;i=1f;b=00;m=2a;t=5f;x=9"
    assert logread.parse_request("follow after " + cursor)["cursor"] == cursor
    for bad in ("bash", "follow after $(id)", "follow after 'x'",
                "follow lines -1", "follow after a b", "follow --merge",
                "follow after " + "a" * 300):
        with pytest.raises(ValueError):
            logread.parse_request(bad)


def test_journal_command_resumes_after_the_cursor():
    command = logread.journal_command({"mode": "follow", "cursor": "s=1;i=2"})
    assert command[0] == logread.JOURNALCTL
    assert "--after-cursor=s=1;i=2" in command and "--follow" in command
    assert not any(a.startswith("--lines") for a in command)
    assert "--lines=5" in logread.journal_command({"mode": "follow", "lines": 5})


def test_transform_redacts_and_keeps_the_fields():
    raw = json.dumps({"__CURSOR": "s=1;i=2", "__REALTIME_TIMESTAMP": "1790000000000000",
                      "PRIORITY": "3", "SYSLOG_IDENTIFIER": "caddy",
                      "_SYSTEMD_UNIT": "caddy.service", "_TRANSPORT": "stdout",
                      "_HOSTNAME": "n3", "MESSAGE": "boom token=abcdefghijk"})
    out = logread.transform(raw)
    assert out["__CURSOR"] == "s=1;i=2" and out["PRIORITY"] == "3"
    assert out["_SYSTEMD_UNIT"] == "caddy.service" and "_HOSTNAME" not in out
    assert "abcdefghijk" not in out["MESSAGE"]
    raw = json.dumps({"__CURSOR": "c", "MESSAGE": list(b"bytes \xff ok")})
    assert logread.transform(raw)["MESSAGE"].startswith("bytes ")
    assert logread.transform("not json") is None
    assert logread.transform(json.dumps({"MESSAGE": "no cursor"})) is None


def fake_journalctl(tmp_path, lines, code=0):
    script = tmp_path / "journalctl"
    data = tmp_path / "lines.jsonl"
    data.write_text("".join(json.dumps(line) + "\n" for line in lines))
    script.write_text("#!/bin/sh\necho \"$@\" > %s/args\ncat %s\nexit %d\n"
                      % (tmp_path, data, code))
    script.chmod(0o755)
    return str(script)


def test_serve_streams_header_then_redacted_entries(tmp_path):
    config = tmp_path / "ids.json"
    config.write_text(json.dumps({"node": "n3", "role": "core"}))
    journal = fake_journalctl(tmp_path, [
        {"__CURSOR": "s=1;i=1", "MESSAGE": "hello", "PRIORITY": "6"},
        {"__CURSOR": "s=1;i=2", "MESSAGE": "Bearer abcdefghijklmnop"}])
    out = io.BytesIO()
    code = logread.serve("follow after s=1;i=0", out, journalctl=journal,
                         config_path=str(config), drop=lambda: None)
    records = [json.loads(l) for l in out.getvalue().splitlines()]
    assert code == 0
    assert records[0] == {"logread": 1, "node": "n3", "role": "core",
                          "resumed": True}
    assert "logread_vitals" in records[1]
    records = [r for r in records if "logread_vitals" not in r]
    assert [r["__CURSOR"] for r in records[1:]] == ["s=1;i=1", "s=1;i=2"]
    assert "abcdefghijklmnop" not in records[2]["MESSAGE"]
    assert "--after-cursor=s=1;i=0" in (tmp_path / "args").read_text()


def fake_proc(tmp_path):
    proc = tmp_path / "proc"
    (proc / "sys" / "fs").mkdir(parents=True)
    (proc / "meminfo").write_text(
        "MemTotal:        1000000 kB\nMemFree:          100000 kB\n"
        "MemAvailable:     250000 kB\nSwapTotal:        512000 kB\n"
        "SwapFree:         256000 kB\n")
    (proc / "loadavg").write_text("0.52 0.40 0.31 3/187 4242\n")
    (proc / "sys" / "fs" / "file-nr").write_text("2304\t4\t9223372\n")
    (proc / "uptime").write_text("86461.25 170000.00\n")
    (proc / "stat").write_text("cpu  100 0 100 1100 0 0 0 0 0 0\ncpu0 1 1 1 1\n")
    return proc


def test_vitals_reads_memory_load_files_disk_and_cpu(tmp_path):
    proc = fake_proc(tmp_path)
    vitals = logread.Vitals(proc=str(proc), root=str(tmp_path),
                            clock=lambda: 1700000000.5)
    (proc / "stat").write_text("cpu  150 0 150 1200 0 0 0 0 0 0\n")
    record = vitals.sample()
    assert record["logread_vitals"] == 1700000000500
    assert record["mem_total"] == 1000000 * 1024
    assert record["mem_available"] == 250000 * 1024
    assert record["swap_total"] == 512000 * 1024
    assert record["swap_free"] == 256000 * 1024
    assert record["load"] == [0.52, 0.40, 0.31]
    assert (record["procs_running"], record["procs_total"]) == (3, 187)
    assert (record["files_open"], record["files_max"]) == (2300, 9223372)
    assert record["uptime"] == 86461
    assert record["disk_total"] > 0 and 0 <= record["disk_free"]
    assert record["inodes_total"] >= record["inodes_free"]
    assert record["cpu_busy"] == 50.0


def test_vitals_skips_what_the_node_does_not_have(tmp_path):
    vitals = logread.Vitals(proc=str(tmp_path / "missing"),
                            root=str(tmp_path / "missing"))
    record = vitals.sample()
    assert set(record) == {"logread_vitals", "cpus"}


def test_serve_sends_vitals_on_its_own_clock(tmp_path):
    lines = [{"__CURSOR": "s=1;i=%d" % i, "MESSAGE": "m%d" % i}
             for i in range(3)]
    journal = fake_journalctl(tmp_path, lines)

    class Counting:
        calls = 0

        def sample(self):
            Counting.calls += 1
            return {"logread_vitals": Counting.calls}

    out = io.BytesIO()
    code = logread.serve("follow", out, journalctl=journal,
                         drop=lambda: None, vitals=Counting(),
                         vitals_seconds=0)
    records = [json.loads(l) for l in out.getvalue().splitlines()]
    assert code == 0
    assert records[1] == {"logread_vitals": 1}
    assert [r["__CURSOR"] for r in records if "__CURSOR" in r] == [
        "s=1;i=0", "s=1;i=1", "s=1;i=2"]
    assert Counting.calls >= 2


def test_serve_refuses_other_commands_without_running_anything(tmp_path):
    out = io.BytesIO()
    ran = []
    code = logread.serve("cat /etc/shadow", out, journalctl="/bin/false",
                         drop=lambda: ran.append(1))
    assert code == 2 and not ran
    assert json.loads(out.getvalue())["error"] == "unsupported request"


def test_serve_drops_privileges_before_journalctl(tmp_path):
    order = []
    journal = fake_journalctl(tmp_path, [])
    logread.serve("follow", io.BytesIO(), journalctl=journal,
                  config_path=str(tmp_path / "missing.json"),
                  drop=lambda: order.append("drop"))
    assert order == ["drop"]


def test_serve_reports_a_failing_journalctl(tmp_path):
    out = io.BytesIO()
    journal = fake_journalctl(tmp_path, [], code=1)
    assert logread.serve("follow", out, journalctl=journal,
                         drop=lambda: None) == 1
    assert "logread_error" in out.getvalue().decode().splitlines()[-1]


def test_render_installs_reader_and_names_its_key_to_the_ids(tmp_path):
    data = fleet(tmp_path)
    key = render.logs_settings(data)["reader_public_key"]
    for files, manifest in (render.render_core(data, "n1"),
                            render.render_runner(data, "r1")):
        assert manifest["logread"]
        assert files[render.LOGREAD_PROGRAM][1] == 0o755
        line = files[render.LOGREAD_AUTHORIZED_KEY][0]
        assert line.startswith('restrict,command="/usr/bin/python3 -I '
                               '/opt/forkmesh-next/bin/fmnext-logread" ')
        assert line.rstrip().endswith(" fmnext-logread")
        ids = json.loads(files["/etc/forkmesh-next/ids.json"][0])
        assert render.logread_fingerprint(key) in ids["ssh"]["fleet_fingerprints"]
    files, manifest = render.render_core(render.without_logs(data), "n1")
    assert not manifest["logread"] and render.LOGREAD_PROGRAM not in files


def test_logread_fingerprint_matches_ssh_keygen():
    key = ("ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIMG2ajXAEwFNW/LoahHVcCrAoFfL4IO"
           "BhTdX/98mVagk fmnext-logread")
    assert render.logread_fingerprint(key) == \
        "SHA256:rkfw812nXpPIWmSrJs6glkzP+PwJvWuuqcc0Jbsviag"


def test_bad_reader_key_is_refused(tmp_path):
    data = fleet(tmp_path)
    data["logs"]["reader_public_key"] = "ssh-rsa AAAA root@laptop"
    with pytest.raises(render.RenderError):
        render.logs_settings(data)


def test_access_log_keeps_ip_and_user_agent_and_drops_headers(tmp_path):
    files, _ = render.render_core(fleet(tmp_path), "n1")
    caddy = files["/etc/caddy/Caddyfile"][0]
    snippet = caddy[caddy.index("(access_log) {"):caddy.index("(site_common) {")]
    for needle in ("request>headers delete", "resp_headers delete",
                   "log_append user_agent {http.request.header.User-Agent}",
                   "replace token REDACTED",
                   "log_skip @access_log_quiet", "output stderr"):
        assert needle in snippet
    assert "ip_mask" not in snippet
    common = caddy[caddy.index("(site_common) {"):]
    assert common.splitlines()[1] == "\timport access_log"
    node_block = caddy[caddy.index("n1.next.forkmesh.com {"):]
    assert "\timport access_log" in node_block
    bare, _ = render.render_core(render.without_logs(fleet(tmp_path)), "n1")
    assert "access_log" not in bare["/etc/caddy/Caddyfile"][0]


def test_start_script_authorizes_the_reader_only_after_the_ids_restart():
    script = next_fleet.start_script({"role": "core", "units": [], "ids": True,
                                      "logread": True})
    assert script.index("systemctl restart fmnext-ids.service") < \
        script.index(render.LOGREAD_AUTHORIZED_KEY)
    off = next_fleet.start_script({"role": "runner", "units": [], "ids": True})
    assert render.LOGREAD_AUTHORIZED_KEY not in off
    assert "grep -v ' fmnext-logread$'" in off


def run_authorized(tmp_path, existing, enabled=True):
    root = tmp_path / "root"
    (root / ".ssh").mkdir(parents=True)
    keys = root / ".ssh" / "authorized_keys"
    keys.write_text(existing)
    line = tmp_path / "logread.authorized_key"
    line.write_text('restrict,command="x" ssh-ed25519 AAAA fmnext-logread\n')
    script = next_fleet.logread_authorized_script(enabled)
    script = script.replace("/root/.ssh", str(root / ".ssh")).replace(
        render.LOGREAD_AUTHORIZED_KEY, str(line))
    done = subprocess.run(["bash", "-euo", "pipefail", "-c", script],
                          capture_output=True)
    return done, keys.read_text()


def test_authorized_keys_block_keeps_the_fleet_key(tmp_path):
    fleet_key = "ssh-ed25519 AAAAfleet fmnext-fleet\n"
    done, text = run_authorized(tmp_path, fleet_key)
    assert done.returncode == 0
    assert text == fleet_key + 'restrict,command="x" ssh-ed25519 AAAA fmnext-logread\n'
    done, again = run_authorized(tmp_path / "again", text)
    assert again == text
    done, removed = run_authorized(tmp_path / "off", text, enabled=False)
    assert removed == fleet_key


def test_logread_script_restarts_the_ids_before_authorizing(tmp_path):
    files, manifest = render.render_core(fleet(tmp_path), "n1")
    script = next_fleet.logread_script(manifest, "/tmp/x.tar.gz")
    order = [script.index(s) for s in (
        "/opt/forkmesh-next/bin/fmnext-logread", "systemctl restart fmnext-ids",
        "systemctl reload caddy", "cat " + render.LOGREAD_AUTHORIZED_KEY)]
    assert order == sorted(order)
    assert "Caddyfile.previous" in script
    _, runner_manifest = render.render_runner(fleet(tmp_path), "r1")
    assert "caddy" not in next_fleet.logread_script(runner_manifest, "/tmp/x")


def test_logread_guard_refuses_a_node_with_another_pending_change(tmp_path):
    data = fleet(tmp_path)
    bare, _ = render.render_core(render.without_logs(data), "n1")

    def digest(text):
        return hashlib.sha256(text.encode()).hexdigest()
    ok = FakeSsh([(lambda h, s: "sha256sum" in s, (
        "%s  /etc/caddy/Caddyfile\n" % digest(bare["/etc/caddy/Caddyfile"][0]))
        .encode())])
    next_fleet.logread_guard(data, "n1", ok, "1.2.3.4")
    stale = FakeSsh([(lambda h, s: "sha256sum" in s,
                      b"%s  /etc/caddy/Caddyfile\n" % digest("other").encode())])
    with pytest.raises(GuardError):
        next_fleet.logread_guard(data, "n1", stale, "1.2.3.4")


def test_logread_adds_only_its_fingerprint_to_the_live_ids_config(tmp_path):
    data = fleet(tmp_path)
    live = json.dumps({"node": "n3", "ssh": {"fleet_fingerprints": ["SHA256:a"]},
                       "pending": {"from": "another change"}})
    ssh = FakeSsh([(lambda h, s: "cat /etc/forkmesh-next/ids.json" in s,
                    live.encode())])
    subset, _ = next_fleet.logread_files(data, "n1", ssh, "1.2.3.4")
    merged = json.loads(subset["/etc/forkmesh-next/ids.json"][0])
    fingerprint = render.logread_fingerprint(
        render.logs_settings(data)["reader_public_key"])
    assert merged["ssh"]["fleet_fingerprints"] == ["SHA256:a", fingerprint]
    assert merged["pending"] == {"from": "another change"}
    again = next_fleet.logread_ids_config(json.dumps(merged), fingerprint)
    assert json.loads(again)["ssh"]["fleet_fingerprints"].count(fingerprint) == 1


def test_logread_step_is_known_and_dry_runs(tmp_path, capsys):
    assert "logread" in next_fleet.ALL_STEPS
    ssh = FakeSsh()
    next_fleet.install_logread(fleet(tmp_path), "n1", "1.2.3.4", ssh,
                               Runner(execute=False))
    assert not ssh.runs and not ssh.puts
    assert "[dry-run]" in capsys.readouterr().out
