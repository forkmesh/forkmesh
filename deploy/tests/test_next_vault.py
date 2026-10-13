"""tools/next_vault.py: dry-run default, guards, key on stdin only, and no
secret value (nor DATA_KEY) ever printed."""

import io
import json
import subprocess
import sys
from pathlib import Path

import pytest

pytest.importorskip("cryptography")

import next_deploy  # noqa: E402
import next_fleet  # noqa: E402
import next_vault  # noqa: E402
import render  # noqa: E402
from fakes import FakeSsh  # noqa: E402
from next_common import GuardError, Runner, ToolError  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
FLEET = REPO / "deploy" / "fleet.example.toml"
sys.path.insert(0, str(REPO / "app" / "host"))
from forkmesh_host import vault_crypto  # noqa: E402

DATA_KEY = "dk-SECRET-" + "k" * 40
IPS = {"n1": "203.0.113.11", "n2": "203.0.113.12", "n3": "203.0.113.13"}


def vault_fleet_file(tmp_path, vault=True):
    text = FLEET.read_text().replace("vault = false", "vault = %s" % (
        "true" if vault else "false"))
    path = tmp_path / "fleet.toml"
    path.write_text(text)
    return path


def fleet(tmp_path, vault=True):
    return render.load_fleet(vault_fleet_file(tmp_path, vault),
                             state_path=tmp_path / "none.json")


def secrets_env(sandbox, with_key=True):
    values = {"ADMIN_PASS": "ap-SECRET-2",
              "MIRROR_ROUTER_SIGNING_SEED": "A" * 43, "MIRROR_ROUTER_PUBLIC_KEY": "B" * 43,
              "FORKMESH_MESH_SECRET": "ms-SECRET-3", "WWW_EDGE_SECRET": "we-SECRET-4",
              "RQLITE_PASSWORD": "rq-SECRET-5",
              "FORKMESH_CERTSYNC_SECRET": "cs-SECRET-9",
              "FMNEXT_IDS_MASTER": "ids-SECRET-10"}
    if with_key:
        values["DATA_KEY"] = DATA_KEY
    (sandbox / "secrets.env").write_text("".join("%s=%s\n" % kv for kv in values.items()))
    dns = {"CLOUDFLARE_API_TOKEN": "cf-SECRET-6", "CLOUDFLARE_ZONE_ID": "zone-SECRET-7",
           "CLOUDFLARE_ACCOUNT_ID": "acct-8"}
    (sandbox / "cloudflare.env").write_text("".join("%s=%s\n" % kv for kv in dns.items()))
    values.update(dns)
    return values


def leader_is(name):
    def answer(host, script, stdin):
        state = "Leader" if host == IPS[name] else "Follower"
        return json.dumps({"store": {"raft": {"state": state}}}).encode()
    return (lambda host, script: "/status" in script and "fmnext-unseal" not in script,
            answer)


def readyz(states):
    """states: node -> 'sealed' | 'ready' | None (unreachable)."""
    def answer(host, script, stdin):
        node = next(n for n, ip in IPS.items() if ip == host)
        state = states.get(node)
        if state is None:
            return subprocess.CompletedProcess(["ssh"], 255, b"", b"no route")
        if state == "sealed":
            return json.dumps({"ok": False, "reasons": ["sealed", "not_hosting"]}).encode()
        return json.dumps({"ok": True, "reasons": []}).encode()
    return (lambda host, script: "8080/readyz" in script, answer)


class FakeRqlite:
    """urllib opener for the tunnelled leader rqlite."""

    def __init__(self, rows=None, table=True):
        self.rows = {r["name"]: dict(r) for r in rows or []}
        self.table = table
        self.requests = []

    def __call__(self, request, timeout=None):
        body = json.loads(request.data)
        self.requests.append((request.full_url, body, request.headers))
        if not self.table:
            results = [{"error": "no such table: host_secrets"}]
        elif "/db/query" in request.full_url:
            results = [{"rows": [{"name": n, "key_id": r["key_id"]}
                                 for n, r in sorted(self.rows.items())]}]
        else:
            results = []
            for statement in body:
                sql = statement[0]
                if sql.startswith("INSERT"):
                    name, nonce, ct, key_id, updated = statement[1:]
                    self.rows[name] = {"name": name, "nonce": nonce, "ciphertext": ct,
                                       "key_id": key_id, "updated_at": updated}
                elif sql.startswith("DELETE"):
                    keep = set(statement[1:])
                    self.rows = {n: r for n, r in self.rows.items() if n in keep}
                results.append({"rows_affected": 1})
        payload = json.dumps({"results": results}).encode()

        class Response(io.BytesIO):
            status = 200

            def __enter__(self):
                return self

            def __exit__(self, *exc):
                return False
        return Response(payload)


def everything_printed(capsys, runner_out=None):
    captured = capsys.readouterr()
    return captured.out + captured.err + (runner_out.getvalue() if runner_out else "")


def assert_no_values(text, values):
    for name, value in values.items():
        assert value not in text, name
    assert DATA_KEY not in text


# --------------------------------------------------------------------------
# key loading


def test_data_key_prefers_the_separate_file_and_checks_its_mode(sandbox):
    secrets_env(sandbox)
    assert next_vault.load_data_key() == DATA_KEY
    other = "file-key-" + "z" * 40
    path = sandbox / "DATA_KEY"
    path.write_text(other + "\n")
    path.chmod(0o644)
    with pytest.raises(GuardError, match="chmod 0400"):
        next_vault.load_data_key()
    path.chmod(0o400)
    assert next_vault.load_data_key() == other


def test_placeholder_or_short_keys_are_refused(sandbox):
    (sandbox / "secrets.env").write_text("DATA_KEY=forkmesh-dev-data-key\n")
    with pytest.raises(ToolError):
        next_vault.load_data_key()
    (sandbox / "secrets.env").write_text("DATA_KEY=short\n")
    with pytest.raises(ToolError):
        next_vault.load_data_key()


# --------------------------------------------------------------------------
# sync


def test_sync_plan_excludes_the_key_and_bootstrap_credentials(tmp_path):
    names = next_vault.sync_plan(fleet(tmp_path))
    assert "DATA_KEY" not in names
    assert "RQLITE_PASSWORD" not in names and "FORKMESH_MESH_SECRET" not in names
    assert {"ADMIN_PASS", "MIRROR_ROUTER_SIGNING_SEED", "WWW_EDGE_SECRET"} <= set(names)
    # Audit 2026-09-24 (commit 0b3ee3b): the only Cloudflare token edits the
    # whole forkmesh.com zone, so it is deliberately NOT synced to the nodes.
    for name in ("NEXT_DNS_API_TOKEN", "NEXT_DNS_ZONE_ID", "CLOUDFLARE_API_TOKEN",
                 "CLOUDFLARE_ZONE_ID", "CLOUDFLARE_ACCOUNT_ID"):
        assert name not in names
    assert next_vault.EXTRA_SOURCES == {}


def test_sync_never_reads_the_dns_token(tmp_path, sandbox):
    # A cloudflare.env without the token (or none at all) is not an error:
    # sync plans the same rows either way and never asks for DNS values.
    secrets_env(sandbox)
    (sandbox / "cloudflare.env").unlink(missing_ok=True)
    assert "NEXT_DNS_API_TOKEN" not in next_vault.sync_plan(fleet(tmp_path))
    out = io.StringIO()
    next_vault.sync(fleet(tmp_path), FakeSsh(), Runner(execute=False, out=out))
    assert "NEXT_DNS" not in out.getvalue()


def test_sync_is_refused_when_the_fleet_has_no_vault(tmp_path, sandbox):
    secrets_env(sandbox)
    with pytest.raises(GuardError, match="vault"):
        next_vault.sync(fleet(tmp_path, vault=False), FakeSsh(), Runner(execute=True))


def test_sync_dry_run_contacts_nothing(tmp_path, sandbox):
    values = secrets_env(sandbox)
    ssh = FakeSsh([leader_is("n2")])
    out = io.StringIO()
    next_vault.sync(fleet(tmp_path), ssh, Runner(execute=False, out=out))
    assert ssh.runs == [] and ssh.tunnels == []
    assert_no_values(out.getvalue(), values)


def test_sync_encrypts_locally_and_upserts_through_the_leader(tmp_path, sandbox, capsys):
    values = secrets_env(sandbox)
    ssh = FakeSsh([leader_is("n3"),
                   (lambda h, s: "fmnext-unseal --reload" in s,
                    json.dumps({"ok": True, "sealed": False, "secrets": ["ADMIN_PASS"]}).encode())])
    rqlite = FakeRqlite()
    out = io.StringIO()
    code = next_vault.sync(fleet(tmp_path), ssh, Runner(execute=True, out=out),
                           opener=rqlite)
    assert code == 0
    assert ssh.tunnels[0][0] == IPS["n3"]
    # One transaction with every row.
    executes = [r for r in rqlite.requests if "/db/execute?transaction" in r[0]]
    assert len(executes) == 1
    names = set(rqlite.rows)
    assert vault_crypto.CHECK_NAME in names
    assert names == {vault_crypto.CHECK_NAME, *next_vault.sync_plan(fleet(tmp_path))}
    # Only ciphertext went over the wire; it opens with DATA_KEY.
    wire = json.dumps([r[1] for r in rqlite.requests])
    assert_no_values(wire, values)
    opened = vault_crypto.open_rows(vault_crypto.VaultKey(DATA_KEY), rqlite.rows.values())
    assert opened["ADMIN_PASS"] == values["ADMIN_PASS"]
    assert opened["WWW_EDGE_SECRET"] == values["WWW_EDGE_SECRET"]
    # The zone-wide DNS token never reaches the vault (0b3ee3b).
    assert "NEXT_DNS_API_TOKEN" not in opened and "NEXT_DNS_ZONE_ID" not in opened
    assert values["CLOUDFLARE_API_TOKEN"] not in json.dumps(opened)
    # Every node was asked to reload.
    assert sum("--reload" in s for _, s, _ in ssh.runs) == 3
    for _, script, stdin in ssh.runs:
        assert stdin is None
    assert_no_values(everything_printed(capsys, out), values)


def test_sync_refuses_rows_sealed_under_another_key(tmp_path, sandbox):
    secrets_env(sandbox)
    other = vault_crypto.VaultKey("another-key-" + "q" * 40)
    rqlite = FakeRqlite([dict(other.check_row(), updated_at=1)])
    ssh = FakeSsh([leader_is("n1")])
    with pytest.raises(GuardError, match="another DATA_KEY"):
        next_vault.sync(fleet(tmp_path), ssh, Runner(execute=True, out=io.StringIO()),
                        opener=rqlite)
    assert not [r for r in rqlite.requests if "/db/execute" in r[0]]


def test_sync_prunes_only_with_prune(tmp_path, sandbox):
    secrets_env(sandbox)
    key = vault_crypto.VaultKey(DATA_KEY)
    old = dict(key.encrypt("OLD_THING", "x"), updated_at=1)
    ssh = FakeSsh([leader_is("n1")])
    rqlite = FakeRqlite([old])
    next_vault.sync(fleet(tmp_path), ssh, Runner(execute=True, out=io.StringIO()),
                    reload=False, opener=rqlite)
    assert "OLD_THING" in rqlite.rows
    next_vault.sync(fleet(tmp_path), ssh, Runner(execute=True, out=io.StringIO()),
                    prune=True, reload=False, opener=rqlite)
    assert "OLD_THING" not in rqlite.rows
    assert vault_crypto.CHECK_NAME in rqlite.rows


def test_sync_without_the_table_says_to_migrate(tmp_path, sandbox):
    secrets_env(sandbox)
    with pytest.raises(ToolError, match="migrate"):
        next_vault.sync(fleet(tmp_path), FakeSsh([leader_is("n1")]),
                        Runner(execute=True, out=io.StringIO()),
                        opener=FakeRqlite(table=False))


# --------------------------------------------------------------------------
# unseal / seal


def unseal_reply(host, script, stdin):
    return json.dumps({"ok": True, "sealed": False, "keyId": "abcd",
                       "secrets": ["ADMIN_PASS"]}).encode()


def test_unseal_pipes_the_key_on_stdin_only(tmp_path, sandbox, capsys):
    values = secrets_env(sandbox)
    ssh = FakeSsh([(lambda h, s: "fmnext-unseal --wait" in s, unseal_reply)])
    out = io.StringIO()
    assert next_vault.unseal(fleet(tmp_path), ssh, Runner(execute=True, out=out),
                             ["n2"]) == 0
    assert len(ssh.runs) == 1
    host, script, stdin = ssh.runs[0]
    assert host == IPS["n2"]
    assert stdin == (DATA_KEY + "\n").encode()
    assert DATA_KEY not in script
    assert_no_values(everything_printed(capsys, out), values)


def test_unseal_if_sealed_skips_unsealed_nodes(tmp_path, sandbox):
    secrets_env(sandbox)
    ssh = FakeSsh([readyz({"n1": "ready", "n2": "sealed", "n3": None}),
                   (lambda h, s: "fmnext-unseal --wait" in s, unseal_reply)])
    code = next_vault.unseal(fleet(tmp_path), ssh, Runner(execute=True, out=io.StringIO()),
                             if_sealed=True)
    keyed = [(h, stdin) for h, s, stdin in ssh.runs if stdin]
    assert keyed == [(IPS["n2"], (DATA_KEY + "\n").encode())]
    assert code == 1  # n3 unreachable


def test_unseal_reports_a_wrong_key_without_printing_it(tmp_path, sandbox, capsys):
    values = secrets_env(sandbox)
    ssh = FakeSsh([(lambda h, s: "fmnext-unseal" in s,
                    lambda h, s, i: subprocess.CompletedProcess(
                        ["ssh"], 1, json.dumps({"ok": False, "error": "wrong_key"}).encode(),
                        b""))])
    out = io.StringIO()
    assert next_vault.unseal(fleet(tmp_path), ssh, Runner(execute=True, out=out),
                             ["n1"]) == 1
    text = everything_printed(capsys, out)
    assert "wrong_key" in text
    assert_no_values(text, values)


def test_cli_defaults_to_dry_run_and_guards_names(tmp_path, sandbox, capsys):
    values = secrets_env(sandbox)
    path = str(vault_fleet_file(tmp_path))
    ssh = FakeSsh([leader_is("n1")])
    for argv in (["unseal"], ["unseal", "--if-sealed"], ["seal", "n1"], ["reload"],
                 ["sync", "--prune"]):
        assert next_vault.main(["--fleet", path] + argv, ssh=ssh) == 0, argv
    assert ssh.runs == [] and ssh.tunnels == []
    assert next_vault.main(["--fleet", path, "unseal", "n9", "--execute"], ssh=ssh) == 2
    assert next_vault.main(["--fleet", path, "seal", "mirror36", "--execute"], ssh=ssh) == 2
    assert next_vault.main(["--fleet", str(vault_fleet_file(tmp_path, False)),
                            "unseal", "--execute"], ssh=ssh) == 2
    assert ssh.runs == []
    assert_no_values(everything_printed(capsys), values)


def test_seal_sends_no_key(tmp_path, sandbox):
    secrets_env(sandbox)
    ssh = FakeSsh([(lambda h, s: "--seal" in s,
                    json.dumps({"ok": True, "sealed": True}).encode())])
    assert next_vault.main(["--fleet", str(vault_fleet_file(tmp_path)), "seal", "n3",
                            "--execute"], ssh=ssh) == 0
    assert [(h, i) for h, _, i in ssh.runs] == [(IPS["n3"], None)]


def test_status_shows_names_and_key_ids_only(tmp_path, sandbox, capsys):
    values = secrets_env(sandbox)
    key = vault_crypto.VaultKey(DATA_KEY)
    rows = [{"name": "ADMIN_PASS", "key_id": key.key_id, "updated_at": 1},
            {"name": "__vault_check__", "key_id": "0000000000000000", "updated_at": 1}]
    ssh = FakeSsh([readyz({"n1": "ready", "n2": "sealed", "n3": "ready"}),
                   (lambda h, s: "/db/query" in s,
                    json.dumps({"results": [{"rows": rows}]}).encode())])
    assert next_vault.main(["--fleet", str(vault_fleet_file(tmp_path)), "status"],
                           ssh=ssh) == 0
    text = everything_printed(capsys)
    assert "n2: SEALED" in text and "n1: unsealed" in text
    assert "(matches)" in text and "(OTHER KEY)" in text
    assert_no_values(text, values)


# --------------------------------------------------------------------------
# next_deploy / next_fleet with the vault on


def test_deploy_restart_unseals_with_the_key_on_stdin(tmp_path, sandbox):
    secrets_env(sandbox)
    seen = []

    def activate(host, script, stdin):
        seen.append((host, script, stdin))
        return subprocess.CompletedProcess(["ssh"], 0, b"activated", b"")
    expected = []

    def expect(host, script, stdin):
        expected.append((host, json.loads(stdin)))
        return subprocess.CompletedProcess(["ssh"], 0, b'{"ok": true}', b"")
    state = (lambda h, s: s == next_deploy.STATE_SCRIPT,
             b'{"current": "0123456789ab", "running": "0123456789ab", "app": "active"}')
    ssh = FakeSsh([leader_is("n1"), (lambda h, s: "switch_to" in s, activate),
                   (lambda h, s: "fmnext-ids expect" in s, expect), state])
    next_deploy.restart(fleet(tmp_path), "0123456789ab", ssh, Runner(execute=True,
                                                                      out=io.StringIO()))
    assert len(seen) == 3
    # Each node's IDS is told first (docs/IDS.md), with no secret on stdin.
    assert sorted(m["node"] for _, m in expected) == ["n1", "n2", "n3"]
    for _, manifest in expected:
        assert manifest["files"] == {
            "/opt/forkmesh-next/app/current": "link:/opt/forkmesh-next/app/releases/*"}
        assert DATA_KEY not in json.dumps(manifest)
    assert all(stdin is None for _, script, stdin in ssh.runs
               if script == next_deploy.STATE_SCRIPT)
    for _, script, stdin in seen:
        assert stdin == (DATA_KEY + "\n").encode()
        assert DATA_KEY not in script
        assert "fmnext-unseal --wait" in script
        assert script.index("read -r fmnext_key") < script.index("systemctl restart")
    # Vault off: no key leaves the desktop.
    seen.clear()
    next_deploy.restart(fleet(tmp_path, vault=False), "0123456789ab", ssh,
                        Runner(execute=True, out=io.StringIO()))
    assert all(stdin is None for _, _, stdin in seen)
    assert all("read -r fmnext_key" not in script for _, script, _ in seen)


def test_fleet_secrets_push_only_bootstrap_and_remove_vault_names(tmp_path, sandbox):
    values = secrets_env(sandbox)
    ssh = FakeSsh()
    next_fleet.push_secrets(fleet(tmp_path), "n1", ssh, Runner(execute=True,
                                                               out=io.StringIO()))
    pushed = set()
    removal = []
    for _, script, stdin in ssh.runs:
        for value in values.values():
            assert value not in script
        if "systemd-creds encrypt" in script:
            pushed.add(script.split("name=")[1].split()[0].strip("'"))
        if "rm -f --" in script:
            removal.append(script)
    assert pushed == {"RQLITE_PASSWORD", "FORKMESH_MESH_SECRET",
                      "FORKMESH_CERTSYNC_SECRET", "rqlite-auth.json",
                      "FMNEXT_IDS_KEY"}
    assert DATA_KEY.encode() not in b"".join(i or b"" for _, _, i in ssh.runs)
    assert len(removal) == 1
    script = removal[0]
    assert "Environment=FORKMESH_VAULT=1" in script  # refuses a non-vault unit
    assert "rm -rf" not in script and "-r " not in script
    for name in ("DATA_KEY", "ADMIN_PASS", "WWW_EDGE_SECRET"):
        assert name in script
    for name in ("RQLITE_PASSWORD", "FORKMESH_MESH_SECRET"):
        assert name not in script.split("for name in", 1)[1].split(";")[0]


def test_render_vault_unit(tmp_path):
    files, manifest = render.render_core(fleet(tmp_path), "n1")
    unit = files["/etc/systemd/system/forkmesh-app.service"][0]
    creds = [line for line in unit.splitlines() if line.startswith("LoadCredentialEncrypted=")]
    assert sorted(c.split("=", 1)[1].split(":")[0] for c in creds) == [
        "FORKMESH_MESH_SECRET", "RQLITE_PASSWORD"]
    assert "Environment=FORKMESH_VAULT=1" in unit.splitlines()
    assert "Environment=FORKMESH_VAULT_PEER_UNSEAL=1" in unit.splitlines()
    assert ("Environment=FORKMESH_MESH_PEERS=n1=http://10.77.0.1:8792,"
            "n2=http://10.77.0.2:8792,n3=http://10.77.0.3:8792") in unit.splitlines()
    assert manifest["vault_peer_unseal"] is True
    for line in ("MemorySwapMax=0", "LimitCORE=0", "ProtectProc=invisible",
                 "RuntimeDirectory=forkmesh-next", "RuntimeDirectoryMode=0700",
                 "NoNewPrivileges=true"):
        assert line in unit.splitlines(), line
    assert manifest["vault"] is True
    assert manifest["files"]["/usr/local/sbin/fmnext-unseal"] == "0o755"
    helper = files["/usr/local/sbin/fmnext-unseal"][0]
    assert "/run/forkmesh-next/unseal.sock" in helper
    assert "argv" in helper and "stdin" in helper
    ops = files["/usr/local/sbin/fmnext-app-ops"][0]
    assert 'if [ "1" = 1 ]' in ops
    # Vault off keeps every secret a credential, and ops runs as before.
    files, manifest = render.render_core(fleet(tmp_path, vault=False), "n1")
    unit = files["/etc/systemd/system/forkmesh-app.service"][0]
    assert "LoadCredentialEncrypted=DATA_KEY:" in unit
    assert "FORKMESH_VAULT" not in unit.replace("# Vault off", "")
    assert "MemorySwapMax=0" in unit
    assert manifest["vault"] is False


def test_bootstrap_names_match_the_host():
    assert tuple(render.VAULT_BOOTSTRAP_NAMES) == tuple(vault_crypto.BOOTSTRAP_NAMES)
    assert render.VAULT_KEY_NAME == vault_crypto.KEY_NAME


def test_fleet_toml_keeps_the_vault_switches():
    current = "[app]\nvault = true\nvault_peer_unseal = false\n"
    text = next_fleet.fleet_toml_text({}, {}, current=current)
    assert "\nvault = true\n" in text
    assert "\nvault_peer_unseal = false\n" in text


def test_peer_unseal_switch(tmp_path):
    data = fleet(tmp_path)
    data["app"]["vault_peer_unseal"] = False
    files, manifest = render.render_core(data, "n2")
    unit = files["/etc/systemd/system/forkmesh-app.service"][0]
    assert "Environment=FORKMESH_VAULT_PEER_UNSEAL=0" in unit.splitlines()
    assert manifest["vault_peer_unseal"] is False
