"""tools/next_fleet.py guards: labels/tags, dry-run default, quorum, secrets."""

import tomllib
import os
import json
import subprocess
from pathlib import Path

import pytest

import next_fleet
from fakes import FakeHttp, FakeSsh, instance
from next_common import GuardError, Runner, ToolError

REPO = Path(__file__).resolve().parents[2]
FLEET = REPO / "deploy" / "fleet.example.toml"
# Unrelated production instances on the same Vultr account.
PRODUCTION_LABELS = ["tradeskie-production", "Blt", "mirror35", "mirror36",
                     "mirror37", "mirror38"]
IDS = {
    "fmnext-core-1": "aaaaaaaa-0000-0000-0000-000000000001",
    "fmnext-core-2": "aaaaaaaa-0000-0000-0000-000000000002",
    "fmnext-core-3": "aaaaaaaa-0000-0000-0000-000000000003",
    "fmnext-run-1": "aaaaaaaa-0000-0000-0000-000000000011",
}


def fleet(tmp_path):
    import render
    return render.load_fleet(FLEET, state_path=tmp_path / "none.json")


def vultr_with(instances):
    by_id = {i["id"]: i for i in instances}
    routes = {
        ("GET", "/instances?"): {"instances": instances, "meta": {"links": {"next": ""}}},
        ("DELETE", "/instances/"): {},
        ("POST", "/instances"): lambda body: {"instance": dict(
            body, id="bbbbbbbb-0000-0000-0000-000000000009", main_ip="0.0.0.0")},
        ("GET", "/ssh-keys"): {"ssh_keys": [{"id": "key-1", "name": "fmnext-fleet"}]},
        ("GET", "/os"): {"os": [{"id": 2625, "name": "Debian 13 x64 (trixie)"}]},
        ("GET", "/plans"): {"plans": [
            {"id": "vc2-1c-0.5gb", "ram": 512, "monthly_cost": 2.5, "locations": ["ewr"]},
            {"id": "vc2-1c-1gb", "ram": 1024, "monthly_cost": 5, "locations": ["ewr"]},
            {"id": "vc2-1c-1gb-v6", "ram": 1024, "monthly_cost": 4, "locations": ["ewr"]},
            {"id": "vhf-1c-1gb", "ram": 1024, "monthly_cost": 6, "locations": ["ewr"]},
            {"id": "vc2-1c-1gb-sjc", "ram": 1024, "monthly_cost": 3, "locations": ["sjc"]},
        ]},
    }
    for instance_id, value in by_id.items():
        routes[("GET", "/instances/" + instance_id)] = {"instance": value}
    http = FakeHttp(routes)
    return next_fleet.Vultr(http), http


def fleet_instances():
    return [instance(label, ip="198.51.100.%d" % n, instance_id=IDS[label])
            for n, label in enumerate(sorted(IDS), start=1)]


# --------------------------------------------------------------------------
# label / tag guard


@pytest.mark.parametrize("label", PRODUCTION_LABELS)
def test_production_labels_are_refused_even_when_tagged(label):
    with pytest.raises(GuardError):
        next_fleet.guard_instance({"label": label, "tags": ["forkmesh-next"]})
    with pytest.raises(GuardError):
        next_fleet.parse_target(label)


@pytest.mark.parametrize("inst", [
    {"label": "fmnext-core-1", "tags": []},
    {"label": "fmnext-core-1", "tags": ["forkmesh"]},
    {"label": "fmnext-core-1"},
    {"label": "core-1", "tags": ["forkmesh-next"]},
    {"label": "fmnext-core-1x", "tags": ["forkmesh-next"]},
    {"label": "fmnextcore-1", "tags": ["forkmesh-next"]},
    {"label": "", "tags": ["forkmesh-next"]},
    {},
])
def test_instances_need_both_prefix_and_tag(inst):
    with pytest.raises(GuardError):
        next_fleet.guard_instance(inst)


def test_guard_accepts_fleet_instances():
    for label in IDS:
        next_fleet.guard_instance({"label": label, "tags": ["x", "forkmesh-next"]})


def test_list_hides_everything_that_is_not_ours():
    mixed = fleet_instances() + [
        instance(label, instance_id="cccccccc-0000-0000-0000-00000000000%d" % n)
        for n, label in enumerate(PRODUCTION_LABELS)] + [
        instance("fmnext-core-7", tags=("other",),
                 instance_id="dddddddd-0000-0000-0000-000000000001")]
    vultr, _ = vultr_with(mixed)
    assert sorted(i["label"] for i in vultr.fleet_instances()) == sorted(IDS)


@pytest.mark.parametrize("label", PRODUCTION_LABELS)
def test_delete_refuses_production_instances_by_live_lookup(label):
    victim = instance(label, instance_id="eeeeeeee-0000-0000-0000-000000000001")
    vultr, http = vultr_with([victim])
    with pytest.raises(GuardError):
        vultr.delete_instance(victim["id"])
    assert http.mutations() == []


def test_delete_rechecks_live_tags(tmp_path):
    # The instance lost its tag since it was listed: the delete must refuse.
    stale = instance("fmnext-run-1", instance_id=IDS["fmnext-run-1"])
    vultr, http = vultr_with([stale])
    http.routes[("GET", "/instances/" + IDS["fmnext-run-1"])] = {
        "instance": dict(stale, tags=[])}
    with pytest.raises(GuardError):
        vultr.delete_instance(IDS["fmnext-run-1"])
    assert http.mutations() == []


def test_malformed_ids_are_refused():
    vultr, http = vultr_with([])
    with pytest.raises(GuardError):
        vultr.delete_instance("../../account")
    assert http.calls == []


# --------------------------------------------------------------------------
# destroy


def test_destroy_is_dry_run_by_default_and_needs_yes(tmp_path):
    vultr, http = vultr_with(fleet_instances())
    runner = Runner(execute=False)
    with pytest.raises(GuardError, match="--yes"):
        next_fleet.destroy(fleet(tmp_path), "r1", vultr, FakeSsh(), runner, yes=False)
    next_fleet.destroy(fleet(tmp_path), "r1", vultr, FakeSsh(), runner, yes=True)
    assert http.mutations() == []
    assert any("DESTROY" in s for s in runner.skipped)


def test_destroy_runner_executes_only_with_execute(tmp_path):
    vultr, http = vultr_with(fleet_instances())
    next_fleet.destroy(fleet(tmp_path), "fmnext-run-1", vultr, FakeSsh(),
                       Runner(execute=True), yes=True)
    assert http.mutations() == [("DELETE", "/instances/" + IDS["fmnext-run-1"], None)]


@pytest.mark.parametrize("target", PRODUCTION_LABELS + ["mirror35.forkmesh.com", "n", "x1"])
def test_destroy_refuses_foreign_targets(tmp_path, target):
    vultr, http = vultr_with(fleet_instances())
    with pytest.raises(GuardError):
        next_fleet.destroy(fleet(tmp_path), target, vultr, FakeSsh(),
                           Runner(execute=True), yes=True, force=True)
    assert http.mutations() == []


def nodes_answer(voters):
    payload = {name: {"id": name, "voter": True, "reachable": reachable,
                      "leader": name == "n2"} for name, reachable in voters.items()}
    return [(lambda host, script: "/nodes" in script, json.dumps(payload).encode())]


def test_core_destroy_refused_below_three_voters(tmp_path):
    vultr, http = vultr_with(fleet_instances())
    ssh = FakeSsh(nodes_answer({"n1": True, "n2": True, "n3": True}))
    with pytest.raises(GuardError, match="2 healthy rqlite voter"):
        next_fleet.destroy(fleet(tmp_path), "n1", vultr, ssh, Runner(execute=True), yes=True)
    assert http.mutations() == []
    # The quorum query never goes to the node being destroyed.
    assert ssh.runs and all(host != "203.0.113.11" for host, _, _ in ssh.runs)


def test_core_destroy_allowed_with_a_joined_replacement(tmp_path):
    vultr, http = vultr_with(fleet_instances())
    ssh = FakeSsh(nodes_answer({"n1": True, "n2": True, "n3": True, "n4": True}))
    next_fleet.destroy(fleet(tmp_path), "n1", vultr, ssh, Runner(execute=True), yes=True)
    assert http.mutations() == [("DELETE", "/instances/" + IDS["fmnext-core-1"], None)]


def test_core_destroy_counts_only_reachable_voters(tmp_path):
    vultr, http = vultr_with(fleet_instances())
    ssh = FakeSsh(nodes_answer({"n1": True, "n2": True, "n3": True, "n4": False}))
    with pytest.raises(GuardError):
        next_fleet.destroy(fleet(tmp_path), "n1", vultr, ssh, Runner(execute=True), yes=True)
    assert http.mutations() == []


def test_force_skips_quorum_but_never_the_label_guard(tmp_path):
    vultr, http = vultr_with(fleet_instances())
    ssh = FakeSsh(nodes_answer({"n1": True, "n2": True, "n3": True}))
    next_fleet.destroy(fleet(tmp_path), "n1", vultr, ssh, Runner(execute=True), yes=True,
                       force=True)
    assert len(http.mutations()) == 1
    unreachable = FakeSsh([(lambda h, s: True, lambda *a: (_ for _ in ()).throw(
        ToolError("ssh down")))])
    vultr, http = vultr_with(fleet_instances())
    with pytest.raises(GuardError, match="cannot verify"):
        next_fleet.destroy(fleet(tmp_path), "n2", vultr, unreachable, Runner(execute=True),
                           yes=True)
    assert http.mutations() == []


# --------------------------------------------------------------------------
# create / plans / fleet.toml


def test_cheapest_plan_mirrors_the_desktop_rules():
    vultr, _ = vultr_with([])
    assert next_fleet.cheapest_plan(vultr.plans())["id"] == "vc2-1c-1gb"
    tie = [{"id": "b", "ram": 1024, "monthly_cost": 5, "locations": ["ewr"]},
           {"id": "a", "ram": 2048, "monthly_cost": 5, "locations": ["ewr"]}]
    assert next_fleet.cheapest_plan(tie)["id"] == "a"


def test_create_is_idempotent_by_label_and_adopts(tmp_path, sandbox):
    vultr, http = vultr_with(fleet_instances())
    adopted = next_fleet.create("core", 2, vultr, Runner(execute=True))
    assert adopted["id"] == IDS["fmnext-core-2"]
    assert http.mutations() == []
    recorded = json.loads((sandbox / "fleet-instances.json").read_text())
    assert recorded["fmnext-core-2"]["id"] == IDS["fmnext-core-2"]


def test_create_dry_run_posts_nothing(tmp_path, sandbox, monkeypatch):
    key = tmp_path / "key"
    Path(str(key) + ".pub").write_text("ssh-ed25519 AAAA test")
    monkeypatch.setenv("FMNEXT_SSH_KEY", str(key))
    vultr, http = vultr_with([])
    runner = Runner(execute=False)
    next_fleet.create("core", 4, vultr, runner)
    assert http.mutations() == []
    assert any("fmnext-core-4" in s and "forkmesh-next" in s for s in runner.skipped)


def test_create_executes_with_label_and_tag(tmp_path, sandbox, monkeypatch):
    key = tmp_path / "key"
    Path(str(key) + ".pub").write_text("ssh-ed25519 AAAA test")
    monkeypatch.setenv("FMNEXT_SSH_KEY", str(key))
    vultr, http = vultr_with([])
    next_fleet.create("run", 2, vultr, Runner(execute=True))
    [(method, path, body)] = http.mutations()
    assert (method, path) == ("POST", "/instances")
    assert body["label"] == "fmnext-run-2" and body["tags"] == ["forkmesh-next"]
    assert body["region"] == "ewr" and body["plan"] == "vc2-1c-1gb" and body["os_id"] == 2625
    assert body["sshkey_id"] == ["key-1"]


def test_create_refuses_to_post_an_unguarded_body():
    vultr, http = vultr_with([])
    with pytest.raises(GuardError):
        vultr.create_instance({"label": "mirror39", "tags": ["forkmesh-next"]})
    with pytest.raises(GuardError):
        vultr.create_instance({"label": "fmnext-core-5", "tags": []})
    assert http.mutations() == []


def test_fleet_toml_from_instances(tmp_path):
    instances = {
        "fmnext-core-1": {"id": IDS["fmnext-core-1"], "ip": "207.0.113.1"},
        "fmnext-core-2": {"id": IDS["fmnext-core-2"], "ip": "207.0.113.2"},
        "fmnext-core-3": {"id": IDS["fmnext-core-3"], "ip": "207.0.113.3"},
        "fmnext-run-1": {"id": IDS["fmnext-run-1"], "ip": "207.0.113.4"},
    }
    text = next_fleet.fleet_toml_text(instances, {"wg_pubkeys": {"n2": "K" * 42 + "A="}})
    assert "public_ipv4" not in text and "207.0.113" not in text
    path = tmp_path / "fleet.toml"
    path.write_text(text)
    import render
    data = render.load_fleet(path, state_path=tmp_path / "none.json",
                             instances=instances)
    assert [(n["name"], n["public_ipv4"], n["wg_ip"]) for n in data["nodes"]] == [
        ("n1", "207.0.113.1", "10.77.0.1"), ("n2", "207.0.113.2", "10.77.0.2"),
        ("n3", "207.0.113.3", "10.77.0.3")]
    assert data["nodes"][1]["wg_pubkey"] == "K" * 42 + "A="
    assert data["runners"][0]["wg_ip"] == "10.77.0.11"


def test_fleet_toml_command_refuses_foreign_labels(sandbox):
    (sandbox / "fleet-instances.json").write_text(json.dumps({
        "fmnext-core-1": {"id": IDS["fmnext-core-1"], "ip": "207.0.113.1"},
        "mirror35": {"id": "x", "ip": "207.0.113.9"}}))
    assert next_fleet.main(["fleet-toml"]) == 2


# --------------------------------------------------------------------------
# install / secrets


def test_install_dry_run_touches_nothing(tmp_path):
    ssh = FakeSsh()
    runner = Runner(execute=False)
    next_fleet.install(fleet(tmp_path), "n2", ssh, runner, next_fleet.INSTALL_STEPS)
    assert ssh.runs == [] and ssh.puts == []
    assert len(runner.skipped) == len(next_fleet.INSTALL_STEPS)


def test_install_execute_requires_a_guarded_instance_at_that_ip(tmp_path):
    vultr, _ = vultr_with(fleet_instances())  # IPs differ from fleet.example.toml
    with pytest.raises(GuardError, match="has IP"):
        next_fleet.install(fleet(tmp_path), "n1", FakeSsh(), Runner(execute=True),
                           ("base",), vultr=vultr)
    vultr, _ = vultr_with([])
    with pytest.raises(GuardError):
        next_fleet.install(fleet(tmp_path), "n1", FakeSsh(), Runner(execute=True),
                           ("base",), vultr=vultr)


def test_files_step_renders_and_installs_manifest_paths(tmp_path):
    data = fleet(tmp_path)
    vultr, _ = vultr_with([instance("fmnext-core-2", ip="203.0.113.12",
                                    instance_id=IDS["fmnext-core-2"])])
    ssh = FakeSsh()
    next_fleet.install(data, "n2", ssh, Runner(execute=True), ("files",), vultr=vultr)
    assert ssh.puts[0][2] == "/tmp/fmnext-files.tar.gz"
    script = ssh.runs[-1][1]
    assert "install -D -m 0644 \"$stage\"/etc/systemd/system/rqlite.service " \
           "/etc/systemd/system/rqlite.service" in script
    assert "install -D -m 0600 \"$stage\"/etc/wireguard/wg0.conf" in script
    assert "/^# BEGIN forkmesh-next/,/^# END forkmesh-next/d" in script
    # Leaderless certificates: no fixed issuer, no throwaway self-signed
    # pair; the old issuer/puller units are retired by name and certsync
    # writes Caddy's tls includes from the local certificates.
    assert "openssl req -x509" not in script
    assert "fmnext-certsync prepare --config /etc/forkmesh-next/certsync.json" in script
    assert "rm -f /etc/systemd/system/fmnext-cert-pull.timer" in script
    # Every old pull/export unit, by exact name, disabled then removed.
    for unit in ("fmnext-cert-export.service", "fmnext-cert-export.timer",
                 "fmnext-cert-pull.service", "fmnext-cert-pull.timer",
                 "cert-export.timer", "cert-pull.timer"):
        assert "rm -f /etc/systemd/system/%s" % unit in script
        assert unit in script.split("systemctl disable --now", 1)[1].split("\n")[0]
    assert "rm -rf" not in script.replace('rm -rf "$stage"', "")
    assert "*" not in "".join(l for l in script.splitlines() if l.startswith("rm -f"))


def test_files_step_never_restarts_live_zram_and_masks_ssh_generator(tmp_path):
    data = fleet(tmp_path)
    vultr, _ = vultr_with([instance("fmnext-core-2", ip="203.0.113.12",
                                    instance_id=IDS["fmnext-core-2"])])
    ssh = FakeSsh()
    next_fleet.install(data, "n2", ssh, Runner(execute=True), ("files",), vultr=vultr)
    lines = ssh.runs[-1][1].splitlines()
    assert not [l for l in lines if "restart systemd-zram-setup" in l]
    zram = [l for l in lines if "zram0" in l]
    assert zram == [next_fleet.ZRAM_START]
    assert zram[0].startswith("if ! grep -q '^/dev/zram0 ' /proc/swaps; then ")
    assert "systemctl start dev-zram0.swap" in zram[0]
    mask = "ln -sfn /dev/null /etc/systemd/system-generators/systemd-ssh-generator"
    assert lines.index(mask) < lines.index("systemctl daemon-reload")
    stop = next(l for l in lines if "sshd-unix-local.socket" in l)
    assert lines.index("systemctl daemon-reload") < lines.index(stop)
    assert "LoadState --value" in stop and "= not-found ]" in stop
    assert "systemctl stop \"$unit\"" in stop
    assert not [l for l in lines if "forkmesh-runner/tmp" in l]


def test_runner_files_step_makes_the_job_tmp_before_the_reload(tmp_path):
    _, manifest = next_fleet.render_node(fleet(tmp_path), "r1")
    lines = next_fleet.files_script(manifest, "/tmp/f.tgz").splitlines()
    make = "install -d -o root -g root -m 1777 /var/lib/forkmesh-runner/tmp"
    assert lines.index(make) < lines.index("systemctl daemon-reload")


def secrets_env(sandbox):
    values = {"DATA_KEY": "dk-SECRET-1", "ADMIN_PASS": "ap-SECRET-2",
              "MIRROR_ROUTER_SIGNING_SEED": "A" * 43, "MIRROR_ROUTER_PUBLIC_KEY": "B" * 43,
              "FORKMESH_MESH_SECRET": "ms-SECRET-3", "WWW_EDGE_SECRET": "we-SECRET-4",
              "RQLITE_PASSWORD": "rq-SECRET-5",
              "FORKMESH_CERTSYNC_SECRET": "cs-SECRET-6",
              "FMNEXT_IDS_MASTER": "ids-SECRET-7"}
    (sandbox / "secrets.env").write_text("".join("%s=%s\n" % kv for kv in values.items()))
    return values


def test_secrets_travel_on_stdin_only(tmp_path, sandbox):
    values = secrets_env(sandbox)
    ssh = FakeSsh()
    next_fleet.push_secrets(fleet(tmp_path), "n1", ssh, Runner(execute=True))
    sent = {}
    for host, script, stdin in ssh.runs:
        for value in values.values():
            assert value not in script  # never in the command line
        if "systemd-creds encrypt" in script:
            name = script.split("name=")[1].split()[0].strip("'")
            sent[name] = stdin
    assert sent["DATA_KEY"] == b"dk-SECRET-1"
    assert sent["WWW_EDGE_SECRET"] == b"we-SECRET-4"
    # certsync's own credential, separate from the mesh secret.
    assert sent["FORKMESH_CERTSYNC_SECRET"] == b"cs-SECRET-6"
    assert sent["FORKMESH_MESH_SECRET"] == b"ms-SECRET-3"
    # The IDS gets its own node's derived key, never the master.
    import next_ids
    assert sent["FMNEXT_IDS_KEY"] == next_ids.node_key(
        "ids-SECRET-7", "n1").encode()
    assert all(b"ids-SECRET-7" not in (stdin or b"") for _, _, stdin in ssh.runs)
    auth = json.loads(sent["rqlite-auth.json"])
    assert auth[0] == {"username": "fmnext", "password": "rq-SECRET-5", "perms": ["all"]}
    assert auth[1] == {"username": "*", "perms": ["notify"]}
    assert all("--with-key=host" in s for _, s, _ in ssh.runs if "encrypt" in s)


def test_secrets_dry_run_and_runner(tmp_path, sandbox):
    secrets_env(sandbox)
    ssh = FakeSsh()
    runner = Runner(execute=False)
    next_fleet.push_secrets(fleet(tmp_path), "n1", ssh, runner)
    assert ssh.runs == []
    assert "rq-SECRET-5" not in " ".join(runner.skipped)
    next_fleet.push_secrets(fleet(tmp_path), "r1", ssh, Runner(execute=True))
    # A runner receives no core secret: only its own IDS key.
    import next_ids
    assert [(s.split("name=")[1].split()[0], stdin) for _, s, stdin in ssh.runs] \
        == [("FMNEXT_IDS_KEY", next_ids.node_key("ids-SECRET-7", "r1").encode())]


def test_init_secrets_generates_without_printing(tmp_path, sandbox, capsys):
    pytest.importorskip("cryptography")
    next_fleet.init_secrets(fleet(tmp_path), Runner(execute=False))
    assert not (sandbox / "secrets.env").exists()
    next_fleet.init_secrets(fleet(tmp_path), Runner(execute=True))
    path = sandbox / "secrets.env"
    assert (path.stat().st_mode & 0o777) == 0o600
    values = dict(line.split("=", 1) for line in path.read_text().splitlines())
    out = capsys.readouterr().out
    for value in values.values():
        assert value not in out
    import re
    assert re.fullmatch(r"[A-Za-z0-9_-]{43}", values["MIRROR_ROUTER_SIGNING_SEED"])
    assert re.fullmatch(r"[A-Za-z0-9_-]{43}", values["MIRROR_ROUTER_PUBLIC_KEY"])


def test_cli_mutations_default_to_dry_run(tmp_path, sandbox, monkeypatch):
    vultr, http = vultr_with(fleet_instances())
    ssh = FakeSsh(nodes_answer({"n1": True, "n2": True, "n3": True, "n4": True}))
    fleet_path = str(FLEET)
    assert next_fleet.main(["--fleet", fleet_path, "destroy", "r1", "--yes"],
                           vultr=vultr, ssh=ssh) == 0
    assert next_fleet.main(["--fleet", fleet_path, "destroy", "n1", "--yes"],
                           vultr=vultr, ssh=ssh) == 0
    assert next_fleet.main(["--fleet", fleet_path, "destroy", "mirror36", "--yes",
                            "--execute", "--force"], vultr=vultr, ssh=ssh) == 2
    assert next_fleet.main(["--fleet", fleet_path, "install", "n2"], ssh=ssh) == 0
    assert http.mutations() == []


def test_secrets_execute_refuses_unguarded_target(sandbox):
    secrets_env(sandbox)
    vultr, _ = vultr_with([instance("fmnext-core-1", tags=(), ip="203.0.113.11",
                                    instance_id=IDS["fmnext-core-1"])])
    ssh = FakeSsh()
    assert next_fleet.main(["--fleet", str(FLEET), "secrets", "n1", "--execute"],
                           vultr=vultr, ssh=ssh) == 2
    assert ssh.runs == []


def test_fleet_toml_keeps_the_fleets_optional_secrets(tmp_path):
    """Regenerating fleet.toml (new WireGuard keys, a replaced node) must not
    silently turn integrations back off."""
    current = ('[app]\noptional_secrets = [\n  "MAILTRAP_API_TOKEN",\n'
               '  "SENTRY_DSN",\n]\n')
    instances = {label: {"id": IDS[label], "ip": "207.0.113.%d" % n}
                 for n, label in enumerate(sorted(IDS), start=1)}
    text = next_fleet.fleet_toml_text(instances, {}, current=current)
    fleet = tomllib.loads(text)
    assert fleet["app"]["optional_secrets"] == ["MAILTRAP_API_TOKEN", "SENTRY_DSN"]
    # Without a current file the example's empty list stands.
    fresh = tomllib.loads(next_fleet.fleet_toml_text(instances, {}))
    assert fresh["app"]["optional_secrets"] == []


def test_base_script_hardens_sshd_and_vendor_accounts():
    for role in ("core", "runner"):
        script = next_fleet.base_script(role)
        for setting in ("AllowAgentForwarding no", "AllowTcpForwarding local",
                        "X11Forwarding no", "PasswordAuthentication no",
                        "PermitRootLogin prohibit-password"):
            assert setting in script
        # The SSH tunnels of next_backup/next_deploy need local forwarding.
        assert "AllowTcpForwarding no" not in script
        assert "rm -f /etc/ssh/sshd_config.d/50-cloud-init.conf" in script
        # sshd -t gates the reload; a broken drop-in is removed.
        assert "if sshd -t; then systemctl reload ssh" in script
        # Root password locked without breaking key logins.
        assert "passwd -l root" in script and "usepam yes" in script
        assert "systemctl mask --now cloud-init-log-reader.service" in script
        assert "userdel -r linuxuser" in script and "pgrep -u linuxuser" in script
    core = next_fleet.base_script("core")
    assert "forkmesh-job" not in core


def test_runner_base_creates_the_job_user_and_dirs():
    script = next_fleet.base_script("runner")
    assert ("useradd --system --home-dir /var/lib/forkmesh-job --shell "
            "/usr/sbin/nologin forkmesh-job --create-home") in script
    assert ("install -d -o forkmesh-job -g forkmesh-job -m 0700 /var/lib/forkmesh-job "
            "/var/lib/forkmesh-job/.claude /var/lib/forkmesh-job/.codex") in script
    assert "-m 0711 /var/lib/forkmesh-runner /var/lib/forkmesh-runner/work" in script
    assert "install -d -o root -g root -m 1777 /var/lib/forkmesh-runner/tmp" in script
    assert "enable-linger" not in script
    # The job user is in no extra group.
    assert "usermod -aG" not in script and "-G " not in script


def test_daemon_script_sets_job_uid_and_owner_only_modes(tmp_path):
    runner_script = next_fleet.daemon_script(fleet(tmp_path), "r1", "next")
    assert "pwd.getpwnam('forkmesh-job').pw_uid" in runner_script
    assert "cfg.pop('mirrorUrl', None)" in runner_script
    # A runner's --check runs as root (jobUid needs a root daemon).
    assert ("if /usr/local/bin/forkmesh-mirror-node --config "
            "/etc/forkmesh/mirror-node.json.new --check") in runner_script
    assert "XDG_RUNTIME_DIR=/run/user" not in runner_script
    for script in (runner_script, next_fleet.daemon_script(fleet(tmp_path), "n1", "next")):
        assert "chmod 0700 /etc/forkmesh /var/lib/forkmesh" in script
        assert "chmod -R go-rwx /etc/forkmesh /var/lib/forkmesh" in script
    core_script = next_fleet.daemon_script(fleet(tmp_path), "n1", "next")
    assert "runuser -u forkmesh-node -- /usr/local/bin/forkmesh-mirror-node --config " \
           "/etc/forkmesh/mirror-node.json.new --check" in core_script


def _gateway_pin_snippet(script, router_key, node_config):
    body = script.split("python3 - <<'PY'\n", 1)[1].split("\nPY\n", 1)[0]
    return (body.replace("/tmp/fmnext-router-key", str(router_key))
            .replace("/etc/forkmesh/mirror-node.json", str(node_config)))


def test_daemon_script_repins_the_dashboard_replica_source(tmp_path):
    import render
    import subprocess
    import sys
    current = fleet(tmp_path)
    origin = "https://" + render.served_app_host(current)
    gateway = tmp_path / "gateway.json"
    gateway.write_text(json.dumps({
        "routerPublicKey": "old",
        "dashboard": {"enabled": True, "assetsRepository": "forkmesh/forkmesh",
                      "canonicalOrigin": "https://app.old.example",
                      "replicaPath": str(tmp_path / "dashboard-replica.json")},
    }))
    node_config = tmp_path / "mirror-node.json"
    node_config.write_text(json.dumps({"gatewayConfig": str(gateway)}))
    router_key = tmp_path / "router-key"
    router_key.write_text("new\n")
    snippet = _gateway_pin_snippet(
        next_fleet.daemon_script(current, "n1", "next"), router_key, node_config)
    out = subprocess.run([sys.executable, "-c", snippet], capture_output=True,
                         text=True, check=True).stdout
    doc = json.loads(gateway.read_text())
    assert doc["routerPublicKey"] == "new"
    assert doc["dashboard"]["canonicalOrigin"] == origin
    assert "dashboard replica source set to " + origin in out
    assert doc["dashboard"]["assetsRoot"] == (
        render.fleet_paths(current)["current"] + "/app/dist")
    router_key.write_text("")
    again = subprocess.run([sys.executable, "-c", snippet], capture_output=True,
                           text=True, check=True).stdout
    assert again == ""
    doc["dashboard"].pop("replicaPath")
    doc["dashboard"]["canonicalOrigin"] = "https://elsewhere.example"
    gateway.write_text(json.dumps(doc))
    subprocess.run([sys.executable, "-c", snippet], check=True)
    assert json.loads(gateway.read_text())["dashboard"]["canonicalOrigin"] == \
        "https://elsewhere.example"


def test_certsync_secret_is_required_and_distinct(tmp_path, sandbox):
    values = secrets_env(sandbox)
    del values["FORKMESH_CERTSYNC_SECRET"]
    (sandbox / "secrets.env").write_text("".join("%s=%s\n" % kv for kv in values.items()))
    with pytest.raises(ToolError, match="FORKMESH_CERTSYNC_SECRET"):
        next_fleet.push_secrets(fleet(tmp_path), "n1", FakeSsh(), Runner(execute=True))
    values["FORKMESH_CERTSYNC_SECRET"] = values["FORKMESH_MESH_SECRET"]
    (sandbox / "secrets.env").write_text("".join("%s=%s\n" % kv for kv in values.items()))
    with pytest.raises(ToolError, match="must differ"):
        next_fleet.push_secrets(fleet(tmp_path), "n1", FakeSsh(), Runner(execute=True))
    assert "FORKMESH_CERTSYNC_SECRET" in next_fleet.required_secret_names(fleet(tmp_path))


def test_init_secrets_never_generates_a_data_key_next_to_the_key_file(sandbox):
    """With the vault on, DATA_KEY lives in config/DATA_KEY; init-secrets must
    not add a second, random DATA_KEY to secrets.env."""
    cfg = next_fleet.common.config_dir()
    (cfg / "DATA_KEY").write_text("k" * 64)
    (cfg / "DATA_KEY").chmod(0o400)
    fleet = next_fleet.common.load_fleet(FLEET)
    next_fleet.init_secrets(fleet, Runner(True))
    text = (cfg / "secrets.env").read_text()
    assert "DATA_KEY=" not in [line.split("=")[0] + "=" for line in text.splitlines()]


def test_fleet_toml_keeps_production_and_leaves_addresses_to_instances(tmp_path):
    current = ('[production]\napp_host = "app.forkmesh.com"\n'
               'world_host = "world.forkmesh.com"\n\n[production.vars]\n'
               'API_ORIGIN = "https://app.forkmesh.com"\n\n[dns]\nreconciler = "dry"\n')
    instances = {label: {"id": IDS[label], "ip": "207.0.113.%d" % n,
                         "ipv6": "2001:19f0:0:%d::1" % n}
                 for n, label in enumerate(sorted(IDS), start=1)}
    text = next_fleet.fleet_toml_text(instances, {}, current=current)
    data = tomllib.loads(text)
    assert data["production"]["app_host"] == "app.forkmesh.com"
    assert data["production"]["vars"]["API_ORIGIN"] == "https://app.forkmesh.com"
    assert "public_ipv" not in text
    assert text == next_fleet.fleet_toml_text(instances, {}, current=text)
    path = tmp_path / "fleet.toml"
    path.write_text(text)
    import render
    loaded = render.load_fleet(path, state_path=tmp_path / "none.json",
                               instances=instances)
    assert loaded["nodes"][0]["public_ipv4"] == "207.0.113.1"
    assert loaded["nodes"][0]["public_ipv6"] == "2001:19f0:0:1::1"
    assert loaded["runners"][0]["public_ipv6"] == "2001:19f0:0:4::1"
    assert render.served_app_host(loaded) == "app.forkmesh.com"


def test_start_script_restarts_fail2ban_after_the_firewall():
    for role in ("core", "runner"):
        script = next_fleet.start_script({"role": role, "units": ["nftables.service"]})
        assert script.index("reload-or-restart nftables.service") < \
            script.index("systemctl try-restart fail2ban.service")


def ipv6_vultr(v6_after_patch="2001:19f0:5:1:5400:5ff:fe00:1"):
    inst = instance("fmnext-core-1", ip="203.0.113.11",
                    instance_id=IDS["fmnext-core-1"])
    inst["v6_main_ip"] = ""
    vultr, http = vultr_with([inst] + [instance(label, tags=()) for label in
                                       PRODUCTION_LABELS])

    def patch(body):
        assert body == {"enable_ipv6": True}
        inst["v6_main_ip"] = v6_after_patch
        return {}
    http.routes[("PATCH", "/instances/")] = patch
    http.routes[("POST", "/instances/%s/reboot" % IDS["fmnext-core-1"])] = {}
    return vultr, http, inst


def test_enable_ipv6_dry_run_changes_nothing(tmp_path):
    vultr, http, _ = ipv6_vultr()
    ssh = FakeSsh()
    assert next_fleet.enable_ipv6(fleet(tmp_path), "n1", vultr, ssh,
                                  Runner(execute=False)) is None
    assert http.mutations() == [] and ssh.runs == []


def test_enable_ipv6_patches_records_and_waits_for_the_node(tmp_path):
    address = "2001:19f0:5:1:5400:5ff:fe00:1"
    vultr, http, inst = ipv6_vultr(address)

    def addresses(host, script, stdin):
        return (address + "\n").encode() if inst["v6_main_ip"] else b""
    ssh = FakeSsh([(lambda h, s: s == next_fleet.IPV6_CHECK_SCRIPT, addresses)])
    result = next_fleet.enable_ipv6(fleet(tmp_path), "n1", vultr, ssh,
                                    Runner(execute=True), reboot=True,
                                    sleep=lambda s: None)
    assert result == address
    methods = [(m, p) for m, p, _ in http.mutations()]
    assert methods == [("PATCH", "/instances/%s" % IDS["fmnext-core-1"])]
    saved = json.loads((next_fleet.instances_path()).read_text())
    assert saved["fmnext-core-1"]["ipv6"] == address


def test_enable_ipv6_reboots_when_on_but_not_configured(tmp_path):
    address = "2001:19f0:5:1:5400:5ff:fe00:1"
    vultr, http, inst = ipv6_vultr(address)
    inst["v6_main_ip"] = address
    rebooted = []
    http.routes[("POST", "/instances/%s/reboot" % IDS["fmnext-core-1"])] = \
        lambda body: rebooted.append(True) or {}

    def addresses(host, script, stdin):
        return (address + "\n").encode() if rebooted else b""
    ssh = FakeSsh([(lambda h, s: s == next_fleet.IPV6_CHECK_SCRIPT, addresses)])
    assert next_fleet.enable_ipv6(fleet(tmp_path), "n1", vultr, ssh,
                                  Runner(execute=True), reboot=True,
                                  sleep=lambda s: None) == address
    assert [m for m, _, _ in http.mutations()] == ["POST"] and rebooted


def test_enable_ipv6_without_reboot_flag_changes_nothing(tmp_path):
    vultr, http, _ = ipv6_vultr()
    next_fleet.enable_ipv6(fleet(tmp_path), "n1", vultr, FakeSsh(),
                           Runner(execute=True), sleep=lambda s: None)
    assert http.mutations() == []


def test_enable_ipv6_refuses_foreign_instances():
    vultr, http = vultr_with([instance("mirror35", tags=("forkmesh-next",),
                                       instance_id=IDS["fmnext-core-1"])])
    with pytest.raises(GuardError):
        vultr.enable_ipv6(IDS["fmnext-core-1"])
    with pytest.raises(GuardError):
        vultr.reboot(IDS["fmnext-core-1"])
    assert http.mutations() == []


def test_daemon_script_toggles_signed_pushes_on_the_flagship_entry(tmp_path):
    import subprocess
    import sys
    for enabled in (True, False):
        current = fleet(tmp_path)
        current.setdefault("app", {})["flagship_receive_pack"] = enabled
        gateway = tmp_path / "gateway.json"
        gateway.write_text(json.dumps({"repositories": [
            {"owner": "forkmesh", "name": "forkmesh",
             "operations": ["git-upload-pack"] + (
                 [] if enabled else ["git-receive-pack"])},
            {"owner": "someone", "name": "else",
             "operations": ["git-upload-pack"]},
        ]}))
        node_config = tmp_path / "mirror-node.json"
        node_config.write_text(json.dumps({"gatewayConfig": str(gateway)}))
        router_key = tmp_path / "router-key"
        router_key.write_text("")
        snippet = _gateway_pin_snippet(
            next_fleet.daemon_script(current, "n1", "next"), router_key,
            node_config)
        subprocess.run([sys.executable, "-c", snippet], check=True,
                       capture_output=True)
        repos = json.loads(gateway.read_text())["repositories"]
        assert ("git-receive-pack" in repos[0]["operations"]) is enabled
        assert repos[1]["operations"] == ["git-upload-pack"]
    runner = next_fleet.daemon_script(current, "r1", "next")
    assert "receive = False" in runner


def test_daemon_script_stamps_the_release_version(tmp_path):
    for name in ("n1", "r1"):
        script = next_fleet.daemon_script(fleet(tmp_path), name, "1.2.4")
        assert "cfg['version'] = '1.2.4'" in script
        merge = script.index("cfg.update(json.load(open('/etc/forkmesh-next/"
                             "mirror-node.overrides.json')))")
        assert merge < script.index("cfg['version'] = '1.2.4'") < script.index(
            "/etc/forkmesh/mirror-node.json.new', 'w')")


def test_release_version_reads_the_desktop_project_version(tmp_path):
    import next_common
    (tmp_path / "desktop").mkdir()
    (tmp_path / "desktop" / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.21)\n"
        "project(ForkMesh VERSION 7.8.9 LANGUAGES CXX)\n")
    assert next_common.release_version(tmp_path) == "7.8.9"
    assert next_common.release_version() == next_common.RELEASE_VERSION_RE.search(
        (next_common.REPO / "desktop" / "CMakeLists.txt").read_text()).group(1)
    (tmp_path / "desktop" / "CMakeLists.txt").write_text("project(ForkMesh)\n")
    with pytest.raises(ToolError):
        next_common.release_version(tmp_path)


def test_delete_ref_refuses_protected_refs_and_checks_containment():
    import next_fleet as nf
    for ref in ("refs/heads/main", "refs/tags/v1", "refs/heads/forkmesh/pulls",
                "refs/heads/a/../b", "HEAD"):
        with pytest.raises(nf.GuardError):
            nf.delete_ref({}, "n1", None, None, "forkmesh/forkmesh", ref, "a" * 40)
    script = nf.delete_ref_script("forkmesh", "forkmesh",
                                  "refs/heads/test/x", "b" * 40)
    assert "merge-base --is-ancestor %s refs/heads/main" % ("b" * 40) in script
    assert "update-ref -d refs/heads/test/x %s" % ("b" * 40) in script
    assert "runuser -u forkmesh-node" in script


def test_operator_deletes_keep_the_tip_in_the_executor_trash():
    import re
    import next_fleet as nf
    trash = nf.delete_ref_trash("refs/heads/test/x", "b" * 40)
    assert re.fullmatch(r"refs/forkmesh/trash/branch/test/x/[0-9a-f]{16}", trash)
    assert nf.delete_ref_trash("refs/heads/agent/7-fix", "b" * 40).startswith(
        "refs/forkmesh/trash/agent-branch/agent/7-fix/")
    assert nf.delete_ref_trash("refs/heads/agent/", "b" * 40).startswith(
        "refs/forkmesh/trash/branch/")
    script = nf.delete_ref_script("forkmesh", "forkmesh",
                                  "refs/heads/test/x", "b" * 40)
    assert script.index("update-ref %s %s" % (trash, "b" * 40)) < script.index(
        "update-ref -d refs/heads/test/x")
    batch = nf.delete_refs_script("forkmesh", "forkmesh")
    assert batch.index("refs/forkmesh/trash/$kind/$branch/$token") < batch.index(
        'update-ref -d "$ref"')
    token_line = next(line for line in batch.splitlines() if "token=" in line)
    kind_line = next(line for line in batch.splitlines() if "kind=" in line)
    for ref in ("refs/heads/test/x", "refs/heads/agent/7-fix"):
        shell = subprocess.run(
            ["bash", "-c", "ref=%s; expect=%s; branch=${ref#refs/heads/}\n%s\n%s\n"
             'echo "refs/forkmesh/trash/$kind/$branch/$token"' % (
                 ref, "b" * 40, kind_line.strip(), token_line.strip())],
            check=True, capture_output=True, text=True).stdout.strip()
        assert shell == nf.delete_ref_trash(ref, "b" * 40)


def test_parse_ref_list_qualifies_branches_and_refuses_protected():
    import next_fleet as nf
    pairs = nf.parse_ref_list("# merged\nfix/a %s\n\nrefs/heads/b %s\n" % (
        "a" * 40, "b" * 40))
    assert pairs == [("refs/heads/fix/a", "a" * 40), ("refs/heads/b", "b" * 40)]
    for bad in ("main %s" % ("a" * 40), "forkmesh/pulls %s" % ("a" * 40),
                "refs/tags/v1 %s" % ("a" * 40)):
        with pytest.raises(nf.GuardError):
            nf.parse_ref_list(bad)
    for bad in ("x", "x abc", "x %s extra" % ("a" * 40), "# nothing"):
        with pytest.raises(ToolError):
            nf.parse_ref_list(bad)


def test_delete_refs_runs_one_batch_per_core(tmp_path):
    import next_fleet as nf
    fl = fleet(tmp_path)
    cores = [n["name"] for n in fl["nodes"]]
    pairs = [("refs/heads/fix/a", "a" * 40), ("refs/heads/b", "b" * 40)]
    ssh = FakeSsh()
    nf.delete_refs(fl, [], ssh, Runner(execute=True), "forkmesh/forkmesh", pairs)
    batches = [run for run in ssh.runs if "update-ref -d" in run[1]]
    assert len(batches) == len(cores)
    for _, script, payload in batches:
        assert payload == ("refs/heads/fix/a %s\nrefs/heads/b %s\n" % (
            "a" * 40, "b" * 40)).encode()
        assert 'merge-base --is-ancestor "$expect" refs/heads/main' in script
        assert "</dev/null" in script


def test_delete_refs_refuses_part_of_the_cores_and_foreign_nodes(tmp_path):
    import next_fleet as nf
    fl = fleet(tmp_path)
    pairs = [("refs/heads/b", "b" * 40)]
    with pytest.raises(nf.GuardError):
        nf.delete_refs(fl, ["n1"], FakeSsh(), Runner(execute=True),
                       "forkmesh/forkmesh", pairs)
    with pytest.raises(nf.GuardError):
        nf.delete_refs(fl, ["r1"], FakeSsh(), Runner(execute=True),
                       "forkmesh/forkmesh", pairs)
    ssh = FakeSsh()
    nf.delete_refs(fl, ["n1"], ssh, Runner(execute=False), "forkmesh/forkmesh", pairs)
    assert ssh.runs == []


def test_delete_refs_repeats_until_nothing_comes_back(tmp_path):
    import next_fleet as nf
    fl = fleet(tmp_path)
    cores = len(fl["nodes"])
    answers = iter([b"deleted 2, already absent 0, kept 0\n"] * cores +
                   [b"deleted 1, already absent 1, kept 0\n"] +
                   [b"deleted 0, already absent 2, kept 0\n"] * (3 * cores))
    ssh = FakeSsh([(lambda host, script: "update-ref -d" in script,
                    lambda host, script, payload: next(answers))])
    nf.delete_refs(fl, [], ssh, Runner(execute=True), "forkmesh/forkmesh",
                   [("refs/heads/a", "a" * 40), ("refs/heads/b", "b" * 40)])
    batches = [run for run in ssh.runs if "update-ref -d" in run[1]]
    assert len(batches) == 3 * cores


def test_delete_refs_gives_up_when_branches_keep_coming_back(tmp_path):
    import next_fleet as nf
    ssh = FakeSsh([(lambda host, script: "update-ref -d" in script,
                    b"deleted 1, already absent 0, kept 0\n")])
    with pytest.raises(ToolError, match="kept coming back"):
        nf.delete_refs(fleet(tmp_path), [], ssh, Runner(execute=True),
                       "forkmesh/forkmesh", [("refs/heads/a", "a" * 40)])


def test_delete_refs_reports_kept_branches(tmp_path):
    import next_fleet as nf
    fl = fleet(tmp_path)
    ssh = FakeSsh([(lambda host, script: "update-ref -d" in script,
                    subprocess.CompletedProcess(["ssh"], 1, b"kept refs/heads/b: it is at x\n",
                                                b""))])
    with pytest.raises(ToolError, match="left branches in place"):
        nf.delete_refs(fl, [], ssh, Runner(execute=True), "forkmesh/forkmesh",
                       [("refs/heads/b", "b" * 40)])


def test_operator_paths_default_to_the_home_project(monkeypatch, tmp_path):
    home = Path(os.environ["HOME"])
    for name in ("FMNEXT_CONFIG_DIR", "FMNEXT_SSH_KEY", "FMNEXT_DATA_DIR",
                 "FMNEXT_LOGS_DIR", "FMNEXT_DIST_DIR", "FMNEXT_BACKUPS_DIR"):
        monkeypatch.delenv(name, raising=False)
    project = home / "projects" / "forkmesh"
    import next_backup
    import next_deploy
    assert next_fleet.common.project_dir() == project
    assert next_fleet.common.config_dir() == project / "config" / "fleet"
    assert next_fleet.common.ssh_key_path() == project / "ssh" / "fleet" / "next_fleet_ed25519"
    assert next_backup.backups_dir() == project / "data" / "fleet" / "backups"
    assert next_deploy.dist_dir() == project / "data" / "fleet" / "dist"
    assert next_fleet.rqlite_tarball({})[2].parent == project / "data" / "fleet" / "dl"
    legacy = home / "projects" / "forkmesh-next"
    (legacy / "config").mkdir(parents=True)
    assert next_fleet.common.config_dir() == legacy / "config"
    (project / "config" / "fleet").mkdir(parents=True)
    assert next_fleet.common.config_dir() == project / "config" / "fleet"


def test_roll_waits_for_the_deploy_lock_and_releases_it(tmp_path, sandbox, monkeypatch):
    import next_common
    vultr, _http = vultr_with(fleet_instances())
    ssh = FakeSsh(nodes_answer({"n1": True, "n2": True, "n3": True, "n4": True}))
    rolled = []
    monkeypatch.setattr(next_fleet, "roll", lambda *a, **k: rolled.append(a[1]))
    monkeypatch.setattr(next_fleet, "ROLL_LOCK_WAIT_SECONDS", 0)
    deploy = next_common.DeployLock(what="next_deploy restart").acquire()
    try:
        code = next_fleet.main(["--fleet", str(FLEET), "roll", "n1", "--execute"],
                               vultr=vultr, ssh=ssh)
        assert code != 0 and rolled == []
    finally:
        deploy.release()
    assert next_fleet.main(["--fleet", str(FLEET), "roll", "n1", "--execute"],
                           vultr=vultr, ssh=ssh) == 0
    assert rolled == [["n1"]]
    after = next_common.DeployLock(what="probe").acquire()
    after.release()


def _runner_roll(tmp_path, monkeypatch, job_counts, install_error=None, healthy=b"active"):
    counts = list(job_counts)
    ssh = FakeSsh([
        (lambda h, s: s == next_fleet.RUNNER_JOBS_SCRIPT,
         lambda *a: str(counts.pop(0)).encode()),
        (lambda h, s: s.startswith("systemctl is-active"), healthy),
    ])
    installs = []

    def fake_install(fleet_doc, name, ssh, runner, steps, **kwargs):
        installs.append((name, steps, kwargs.get("idle_guard")))
        if install_error:
            raise ToolError(install_error)
    monkeypatch.setattr(next_fleet, "install", fake_install)
    monkeypatch.setattr(next_fleet, "verify_target", lambda *a: None)
    clock = iter(range(0, 10000, 10))
    return ssh, installs, dict(sleep=lambda s: None, clock=lambda: next(clock))


def test_roll_runners_waits_for_running_jobs_then_installs_with_the_guard(
        tmp_path, monkeypatch):
    ssh, installs, timing = _runner_roll(tmp_path, monkeypatch, [2, 1, 0])
    done = next_fleet.roll_runners(fleet(tmp_path), ["r1"], ssh, Runner(execute=True),
                                   idle_wait=600, **timing)
    assert done == ["r1"] and installs == [("r1", ("daemon",), True)]


def test_roll_runners_never_restarts_a_runner_with_a_job(tmp_path, monkeypatch):
    ssh, installs, timing = _runner_roll(tmp_path, monkeypatch, [1])
    with pytest.raises(next_fleet.RunnersBusy, match="r1"):
        next_fleet.roll_runners(fleet(tmp_path), ["r1"], ssh, Runner(execute=True),
                                idle_wait=0, **timing)
    assert installs == []


def test_roll_runners_treats_a_job_started_during_the_upload_as_busy(
        tmp_path, monkeypatch):
    ssh, installs, timing = _runner_roll(
        tmp_path, monkeypatch, [0],
        install_error="ssh x failed (75): %s" % next_fleet.RUNNER_BUSY_MARK)
    with pytest.raises(next_fleet.RunnersBusy):
        next_fleet.roll_runners(fleet(tmp_path), ["r1"], ssh, Runner(execute=True),
                                idle_wait=0, **timing)


def test_roll_runners_fails_when_the_daemon_does_not_come_back(tmp_path, monkeypatch):
    ssh, _, timing = _runner_roll(tmp_path, monkeypatch, [0], healthy=b"failed")
    with pytest.raises(ToolError, match="unhealthy") as raised:
        next_fleet.roll_runners(fleet(tmp_path), ["r1"], ssh, Runner(execute=True),
                                idle_wait=0, **timing)
    assert not isinstance(raised.value, next_fleet.RunnersBusy)


def test_the_runner_daemon_script_refuses_while_a_job_runs(tmp_path, monkeypatch):
    monkeypatch.setattr(next_fleet.common, "release_version", lambda: "test")
    doc = fleet(tmp_path)
    guarded = next_fleet.daemon_script(doc, "r1", "test", idle_guard=True)
    assert guarded.startswith(next_fleet.RUNNER_BUSY_SCRIPT + "\n")
    assert guarded.index(next_fleet.RUNNER_BUSY_SCRIPT) < guarded.index(
        "systemctl restart forkmesh-mirror-node")
    assert next_fleet.RUNNER_BUSY_SCRIPT not in next_fleet.daemon_script(doc, "r1", "test")
    assert next_fleet.RUNNER_BUSY_SCRIPT not in next_fleet.daemon_script(
        doc, "n1", "test", idle_guard=True)


def test_roll_runners_command_refuses_cores_and_reports_busy(tmp_path, sandbox, monkeypatch):
    vultr, _http = vultr_with(fleet_instances())
    assert next_fleet.main(["--fleet", str(FLEET), "roll-runners", "n1", "--execute"],
                           vultr=vultr, ssh=FakeSsh()) == 2

    def busy(*a, **k):
        raise next_fleet.RunnersBusy("busy with agent jobs: r1")
    monkeypatch.setattr(next_fleet, "roll_runners", busy)
    assert next_fleet.main(["--fleet", str(FLEET), "roll-runners", "--execute"],
                           vultr=vultr, ssh=FakeSsh()) == next_fleet.RUNNERS_BUSY_EXIT


def test_core_files_step_restarts_rqlite_only_when_its_unit_changed():
    core = next_fleet.files_script(
        {"role": "core", "files": {"/etc/systemd/system/rqlite.service": "0o644"}}, "/tmp/a.tgz")
    lines = core.splitlines()
    before = next(i for i, line in enumerate(lines) if line.startswith("rqlite_unit_before="))
    install = next(i for i, line in enumerate(lines) if line.startswith("install -D") and "rqlite.service" in line)
    restart = next(i for i, line in enumerate(lines) if "systemctl restart rqlite.service" in line)
    assert before < install < restart
    assert '"$rqlite_unit_before" != ' in lines[restart]
    assert "systemctl is-active -q rqlite.service" in lines[restart]
    runner = next_fleet.files_script({"role": "runner", "files": {}}, "/tmp/a.tgz")
    assert "rqlite" not in runner


def test_files_step_restarts_the_daemon_when_its_unit_changed():
    for role in ("core", "runner"):
        lines = next_fleet.files_script({"role": role, "files": {}}, "/tmp/a.tgz").splitlines()
        before = next(i for i, line in enumerate(lines) if line.startswith("daemon_unit_before="))
        restart = next(i for i, line in enumerate(lines)
                       if "systemctl restart forkmesh-mirror-node.service" in line)
        reload = lines.index("systemctl daemon-reload")
        assert before < reload < restart
        assert '"$daemon_unit_before" != ' in lines[restart]


def test_rqlite_unit_digest_survives_pipefail_without_dropins(tmp_path):
    script = next_fleet.RQLITE_UNIT_DIGEST.replace("/etc/systemd/system", str(tmp_path))
    done = subprocess.run(["bash", "-euo", "pipefail", "-c", "x=$(%s); echo \"$x\"" % script],
                          capture_output=True, text=True)
    assert done.returncode == 0 and len(done.stdout.split()[0]) == 64



def test_files_step_loads_the_fleet_firewall_while_ufw_is_active(tmp_path):
    data = fleet(tmp_path)
    vultr, _ = vultr_with([instance("fmnext-core-2", ip="203.0.113.12",
                                    instance_id=IDS["fmnext-core-2"])])
    ssh = FakeSsh()
    next_fleet.install(data, "n2", ssh, Runner(execute=True), ("files",), vultr=vultr)
    script = ssh.runs[-1][1]
    firewall = next_fleet.FIREWALL_BEFORE_CADDY
    assert firewall in script
    assert script.index("nft -c -f /etc/nftables.conf") < script.index(firewall)
    assert firewall.index("nft -c -f /etc/nftables.conf") < firewall.index("ufw --force disable")
    assert "Status: active" in firewall


def test_caddy_install_waits_long_enough_for_a_first_certificate():
    settings = {"tarball": "caddy.tar.gz", "sha256": "0" * 64, "version": "2.11.4"}
    script = next_fleet.caddy_script(settings, "1" * 64, "[Service]\n", "n4.next.example.test")
    assert "seq 1 %d" % next_fleet.CADDY_INSTALL_WAIT in script
    assert next_fleet.CADDY_INSTALL_WAIT >= 120
    assert "seq 1 %d" % next_fleet.CADDY_INSTALL_WAIT in next_fleet.caddy_debian_script(
        "n4.next.example.test")
