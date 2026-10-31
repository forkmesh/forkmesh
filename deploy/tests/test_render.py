"""deploy/core/render.py: per-node output, no secrets, snapshots."""

import json
import os
import re
from pathlib import Path

import pytest

import render

REPO = Path(__file__).resolve().parents[2]
FLEET = REPO / "deploy" / "fleet.example.toml"
SNAPSHOTS = Path(__file__).resolve().parent / "snapshots"
# Files whose content depends only on the fleet file (the route snippets
# follow app/wrangler.toml and are covered by test_caddy_routes.py).
SNAPSHOT_FILES = (
    "/etc/systemd/system/rqlite.service",
    "/etc/systemd/system/forkmesh-app.service",
    "/etc/systemd/system/forkmesh-mirror-node.service",
    "/etc/forkmesh-next/app.env",
    "/etc/forkmesh-next/certsync.json",
    "/etc/forkmesh-next/mirror-node.overrides.json",
    "/etc/caddy/Caddyfile",
    "/etc/nftables.conf",
    "/etc/wireguard/wg0.conf",
)
KEYS = {"n1": "k" * 42 + "A=", "n2": "m" * 42 + "E=", "n3": "p" * 42 + "I="}


def fleet(tmp_path, keys=True):
    state = tmp_path / "state.json"
    state.write_text(json.dumps({"wg_pubkeys": KEYS if keys else {}}))
    return render.load_fleet(FLEET, state_path=state)


def rendered(tmp_path, node, **kwargs):
    files, manifest = render.render_core(fleet(tmp_path), node, **kwargs)
    return {path: content for path, (content, _) in files.items()}, manifest


def test_no_secret_values_are_rendered(tmp_path, sandbox):
    # Distinctive fake secrets in the (sandboxed) operator config dir: the
    # renderer must never read them, so none may appear in any output.
    values = {name: "SECRET-%s-%s" % (name, os.urandom(8).hex()) for name in (
        "DATA_KEY", "ADMIN_PASS", "MIRROR_ROUTER_SIGNING_SEED", "FORKMESH_MESH_SECRET",
        "WWW_EDGE_SECRET", "RQLITE_PASSWORD", "VULTR_API_KEY", "CLOUDFLARE_API_TOKEN")}
    (sandbox / "secrets.env").write_text("".join("%s=%s\n" % kv for kv in values.items()))
    (sandbox / "vultr.env").write_text("VULTR_API_KEY=%s\n" % values["VULTR_API_KEY"])
    everything = []
    for node in ("n1", "n2", "n3"):
        files, manifest = render.render_core(fleet(tmp_path), node)
        everything += [c for c, _ in files.values()] + [json.dumps(manifest)]
    files, manifest = render.render_runner(fleet(tmp_path), "r1")
    everything += [c for c, _ in files.values()] + [json.dumps(manifest)]
    blob = "\n".join(everything)
    for name, value in values.items():
        assert value not in blob, name
    assert "@@" not in blob  # every placeholder filled


def test_secrets_are_referenced_by_credential_name_only(tmp_path):
    files, manifest = rendered(tmp_path, "n2")
    unit = files["/etc/systemd/system/forkmesh-app.service"]
    names = re.findall(r"^LoadCredentialEncrypted=([A-Z_]+):/etc/forkmesh-next/creds/\1\.cred$",
                       unit, re.M)
    assert names == ["DATA_KEY", "ADMIN_PASS", "MIRROR_ROUTER_SIGNING_SEED",
                     "MIRROR_ROUTER_PUBLIC_KEY", "FORKMESH_MESH_SECRET",
                     "WWW_EDGE_SECRET", "RQLITE_PASSWORD"]
    assert manifest["credentials"]["forkmesh-app.service"] == names
    assert manifest["credentials"]["rqlite.service"] == ["rqlite-auth.json"]
    rqlite = files["/etc/systemd/system/rqlite.service"]
    assert "LoadCredentialEncrypted=rqlite-auth.json:/etc/forkmesh-next/creds/" \
           "rqlite-auth.json.cred" in rqlite
    assert "-auth %d/rqlite-auth.json" in rqlite


def test_per_node_values(tmp_path):
    for number, node in enumerate(("n1", "n2", "n3"), start=1):
        files, manifest = rendered(tmp_path, node)
        wg_ip = "10.77.0.%d" % number
        rqlite = files["/etc/systemd/system/rqlite.service"]
        assert "-node-id %s " % node in rqlite
        assert "-raft-addr %s:4002" % wg_ip in rqlite
        assert "-http-addr 127.0.0.1:4001" in rqlite
        assert "-bootstrap-expect 3" in rqlite
        assert "-join 10.77.0.1:4002,10.77.0.2:4002,10.77.0.3:4002" in rqlite
        assert "-join-as fmnext" in rqlite
        assert "-raft-snap-wal-size 33554432" in rqlite and "-raft-snap " not in rqlite
        assert "Environment=TZ=UTC" in rqlite and "GOMEMLIMIT=150MiB" in rqlite
        assert "MemoryHigh=260M" in rqlite and "MemoryMax=320M" in rqlite and "OOMScoreAdjust=-800" in rqlite
        assert "/var/lib/forkmesh-next/rqlite" in rqlite
        assert "auto-backup" not in rqlite  # no object storage
        app_env = files["/etc/forkmesh-next/app.env"]
        assert "FORKMESH_NODE_ID=%s\n" % node in app_env
        assert "FORKMESH_MESH_LISTEN=%s:8792\n" % wg_ip in app_env
        assert "FORKMESH_RQLITE_NODE_MAP=n1=n1,n2=n2,n3=n3" in app_env
        wg = files["/etc/wireguard/wg0.conf"]
        assert "Address = %s/24" % wg_ip in wg
        # The rendered file carries only a placeholder; the installer fills
        # in the node-local key, so no key ever leaves the node.
        keys = [l for l in wg.splitlines() if l.startswith("PrivateKey")]
        assert keys == ["PrivateKey = @FMNEXT_WG0_PRIVATE_KEY@"]
        assert "wg0.key" in wg
        assert KEYS[node] not in wg  # a node is not its own peer
        assert sum(1 for line in wg.splitlines() if line == "[Peer]") == 2
        assert manifest["wg_ip"] == wg_ip
        assert "127.0.0.1 app.next.forkmesh.com" in files["/etc/forkmesh-next/hosts.fragment"]
        caddy = files["/etc/caddy/Caddyfile"]
        assert "\n%s.next.forkmesh.com {" % node in caddy
        assert "reverse_proxy 127.0.0.1:8790" in caddy


def test_caddy_holds_requests_through_an_app_restart(tmp_path):
    files, _ = rendered(tmp_path, "n1")
    caddy = files["/etc/caddy/Caddyfile"]
    assert render.APP_UPSTREAM_TRY_SECONDS >= 30
    assert "\tlb_try_duration %ds\n" % render.APP_UPSTREAM_TRY_SECONDS in caddy
    assert "lb_try_duration 10s" not in caddy


def test_app_unit_limits(tmp_path):
    files, _ = rendered(tmp_path, "n1")
    unit = files["/etc/systemd/system/forkmesh-app.service"]
    for line in ("User=forkmesh-app", "Environment=MALLOC_ARENA_MAX=2", "MemoryHigh=280M",
                 "MemoryMax=340M", "OOMScoreAdjust=300", "Restart=always",
                 "Environment=TZ=UTC",
                 "WorkingDirectory=/opt/forkmesh-next/app/current/app/host"):
        assert line in unit.splitlines(), line
    assert "ExecStart=/opt/forkmesh-next/app/current/.venv/bin/python -OO -m " \
           "forkmesh_host serve" in unit
    assert "--db rqlite:http://127.0.0.1:4001" in unit and "--no-build" in unit
    assert "FORKMESH_HOST_DEBUG" not in unit


def test_certificates_are_leaderless_and_identical_on_every_core(tmp_path):
    outputs = {}
    for node in ("n1", "n2", "n3"):
        files, manifest = rendered(tmp_path, node)
        outputs[node] = files
        caddy = files["/etc/caddy/Caddyfile"]
        assert "is_issuer" not in manifest and "acme_issuer" not in manifest
        for host in ("app.next.forkmesh.com", "world.next.forkmesh.com"):
            # files | acme | internal: chosen at run time by fmnext-certsync.
            assert "\timport /etc/forkmesh-next/caddy/tls-%s.caddy" % host in caddy
        assert "\ttls " not in caddy
        assert "import /etc/forkmesh-next/caddy/acme-http.caddy" in caddy
        # No node address is hardcoded as a challenge target or issuer.
        for other in ("10.77.0.1", "10.77.0.2", "10.77.0.3"):
            assert "reverse_proxy %s" % other not in caddy
        assert "storage" not in caddy  # no shared certificate storage
        for unit in ("fmnext-certsync.service", "fmnext-cert-sync.service",
                     "fmnext-cert-sync.timer"):
            assert "/etc/systemd/system/" + unit in files
        assert "/etc/systemd/system/fmnext-cert-pull.service" not in files
        assert "/etc/systemd/system/fmnext-cert-export.service" not in files
        assert "fmnext-cert-pull.timer" in manifest["retired_units"]
        config = json.loads(files["/etc/forkmesh-next/certsync.json"])
        assert config["names"] == ["app.next.forkmesh.com", "world.next.forkmesh.com"]
        assert config["peers"] == {"n1": "http://10.77.0.1:8793",
                                   "n2": "http://10.77.0.2:8793",
                                   "n3": "http://10.77.0.3:8793"}
        assert config["challenge_listen"] == "127.0.0.1:8794"
        assert config["tls_owner"] == "caddy" and config["renew_days"] == 30
        dropin = files["/etc/systemd/system/caddy.service.d/forkmesh-next.conf"]
        assert "ExecStartPre=+/usr/bin/python3 -I /opt/forkmesh-next/bin/" \
               "fmnext-certsync prepare" in dropin
    config = json.loads(outputs["n2"]["/etc/forkmesh-next/certsync.json"])
    assert config["listen"] == "10.77.0.2:8793"
    assert config["allowed_peers"] == ["10.77.0.1", "10.77.0.3"]
    # Apart from the node's own name and address, the Caddyfiles match.
    one = outputs["n1"]["/etc/caddy/Caddyfile"].replace("n1", "nX")
    two = outputs["n2"]["/etc/caddy/Caddyfile"].replace("n2", "nX")
    assert one == two


def test_no_hardcoded_first_node_anywhere(tmp_path):
    """Nothing in a rendered core node treats n1 (or any node) specially."""
    for node in ("n2", "n3"):
        files, manifest = rendered(tmp_path, node)
        for path, content in files.items():
            if path in ("/etc/wireguard/wg0.conf", "/etc/nftables.conf",
                        "/etc/forkmesh-next/certsync.json",
                        "/etc/forkmesh-next/app.env",
                        "/etc/forkmesh-next/ids.json",
                        "/etc/forkmesh-next/mirror-node.overrides.json",
                        "/etc/systemd/system/rqlite.service",
                        "/etc/systemd/system/forkmesh-app.service",
                        "/opt/forkmesh-next/bin/fmnext-certsync",
                        "/etc/forkmesh-next/caddy/app-routes.caddy",
                        "/etc/forkmesh-next/caddy/world-routes.caddy"):
                continue  # peer lists name every node, by design
            assert "n1." not in content and "10.77.0.1" not in content, path


def test_dns_reconciler_settings(tmp_path):
    files, _ = rendered(tmp_path, "n3")
    env = files["/etc/forkmesh-next/app.env"]
    assert "FORKMESH_DNS_RECONCILER=dry" in env
    assert "FORKMESH_DNS_TARGETS=n1=203.0.113.11,n2=203.0.113.12,n3=203.0.113.13" in env
    assert "FORKMESH_DNS_NAMES=app.next.forkmesh.com,world.next.forkmesh.com" in env
    assert "TOKEN" not in env  # the Cloudflare token only ever comes from the vault


def test_firewall_mesh_ports_only_from_core_on_wg0(tmp_path):
    files, _ = rendered(tmp_path, "n2")
    nft = files["/etc/nftables.conf"]
    assert "policy drop;" in nft
    assert "tcp dport { 822, 80, 443 } accept" in nft
    assert "udp dport { 443, 51820 } accept" in nft
    assert ('iifname "wg0" ip saddr { 10.77.0.1, 10.77.0.2, 10.77.0.3 } '
            "tcp dport { 25, 80, 4001, 4002, 8792, 8793 } accept") in nft
    # Mesh ports are not open anywhere else.
    input_chain = nft.split("chain input", 1)[1].split("chain forward", 1)[0]
    for line in input_chain.splitlines():
        if "4002" in line or "8792" in line or " 25, " in line:
            assert 'iifname "wg0"' in line


def test_core_output_rules_restrict_internal_ports_by_uid(tmp_path):
    files, _ = rendered(tmp_path, "n2")
    output = files["/etc/nftables.conf"].split("chain output", 1)[1]
    rules = [line.strip() for line in output.splitlines() if "reject" in line]
    targets = "ip daddr { 10.77.0.0/24, 127.0.0.0/8 }"
    assert (targets + ' tcp dport 4002 meta skuid != "rqlite" '
            "counter reject with tcp reset") in rules
    assert (targets + ' tcp dport 4001 meta skuid != { "root", "forkmesh-app" } '
            "counter reject with tcp reset") in rules
    assert (targets + ' tcp dport 8792 meta skuid != "forkmesh-app" '
            "counter reject with tcp reset") in rules
    assert (targets + ' tcp dport 8793 meta skuid != "root" '
            "counter reject with tcp reset") in rules
    assert 'ip daddr 169.254.0.0/16 meta skuid != "root" counter reject' in rules
    assert "policy accept;" in output  # everything else leaves as before


def _nft_binary():
    import os
    import shutil
    found = os.environ.get("FMNEXT_NFT") or shutil.which("nft")
    return found if found and shutil.which("unshare") else None


@pytest.mark.skipif(_nft_binary() is None, reason="no nft (set FMNEXT_NFT)")
@pytest.mark.parametrize("node", ["n1", "r1"])
def test_nftables_ruleset_parses(tmp_path, node):
    """`nft -c` in a throwaway user+network namespace (no root needed). The
    node's service users do not exist here, so they map to local ones."""
    import subprocess
    if node == "r1":
        data = fleet(tmp_path)
        files, _ = render.render_runner(data, "r1")
        text = files["/etc/nftables.conf"][0]
    else:
        text = rendered(tmp_path, node)[0]["/etc/nftables.conf"]
    # The IDS egress log rule's user set (distinct users, as nft wants).
    text = text.replace('{ "forkmesh-app", "caddy", "rqlite", "fmnext-certsync" }',
                        '{ "nobody", "daemon" }')
    for user in ("rqlite", "forkmesh-app", "forkmesh-job", "forkmesh-node"):
        text = text.replace('"%s"' % user, '"nobody"')
    text = text.replace('{ "root", "nobody" }', '{ "root", "daemon" }')
    conf = tmp_path / "nft.conf"
    conf.write_text(text)
    result = subprocess.run(["unshare", "-rn", _nft_binary(), "-c", "-f", str(conf)],
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stderr

def test_runner_holds_no_core_secret_and_no_mesh(tmp_path):
    data = fleet(tmp_path)
    data["runners"][0]["wg_ip"] = "10.77.0.11"
    files, manifest = render.render_runner(data, "r1")
    # Only the runner's own IDS key (docs/IDS.md), never a core secret.
    assert manifest["credentials"] == {"fmnext-ids.service": ["FMNEXT_IDS_KEY"]}
    blob = "\n".join(content for path, (content, _) in files.items()
                     if path != "/etc/systemd/system/fmnext-ids.service")
    assert "LoadCredential" not in blob
    assert [l for l in files["/etc/systemd/system/fmnext-ids.service"][0]
            .splitlines() if "Credential" in l] == [
        "LoadCredentialEncrypted=FMNEXT_IDS_KEY:/etc/forkmesh-next/creds/"
        "FMNEXT_IDS_KEY.cred"]
    assert "/etc/wireguard/wg0.conf" not in files
    nft = files["/etc/nftables.conf"][0]
    assert "tcp dport { 822 } accept" in nft and "wg0" not in nft
    # Jobs reach only the public internet: no metadata, private, CGNAT,
    # loopback (except a local DNS stub) or WireGuard addresses.
    job_rules = [l.strip() for l in nft.splitlines() if 'skuid "forkmesh-job"' in l]
    v4 = next(l for l in job_rules if "ip daddr {" in l and "reject" in l)
    for net in ("169.254.0.0/16", "10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16",
                "100.64.0.0/10", "127.0.0.0/8", "10.77.0.0/24"):
        assert net in v4
    v6 = next(l for l in job_rules if "ip6 daddr" in l)
    for net in ("::1", "fc00::/7", "fe80::/10"):
        assert net in v6
    assert job_rules[0].startswith('meta skuid "forkmesh-job" ip daddr 127.0.0.53')
    assert 'meta skuid "forkmesh-node" ip daddr 169.254.0.0/16 counter reject' in nft
    overrides = json.loads(files["/etc/forkmesh-next/mirror-node.overrides.json"][0])
    assert overrides["role"] == "runner" and overrides["upstreams"] == {}
    assert overrides["runner"]["appUrl"] == "https://app.next.forkmesh.com"
    assert overrides["runner"]["maxConcurrentRuns"] == 1
    assert overrides["enableActions"] is True
    # Job dirs live outside the daemon's 0700 state dir.
    assert overrides["runner"]["workRoot"] == "/var/lib/forkmesh-runner/work"
    assert manifest["job_user"] == "forkmesh-job"
    unit = files["/etc/systemd/system/forkmesh-mirror-node.service"][0]
    # Root daemon: jobs start as forkmesh-job through the system manager.
    lines = unit.splitlines()
    assert "User=root" in lines and "Group=root" in lines
    assert "CapabilityBoundingSet=CAP_CHOWN CAP_DAC_OVERRIDE CAP_FOWNER CAP_KILL CAP_SETUID CAP_SETGID" in lines
    assert ("ReadWritePaths=/var/lib/forkmesh /var/lib/forkmesh-runner "
            "/var/lib/forkmesh-job") in lines
    assert "ProtectHome=true" in lines and "UMask=0077" in lines
    assert "IPAddressDeny=169.254.0.0/16" in lines
    assert "systemd-journal" not in unit
    assert "XDG_RUNTIME_DIR=/run/user/%U" not in unit and "EnvironmentFile=-/etc/forkmesh/node.env" in unit
    # And the runner is never a WireGuard peer of the core.
    core, _ = rendered(tmp_path, "n1")
    assert "10.77.0.11" not in core["/etc/wireguard/wg0.conf"]
    assert "10.77.0.11" not in core["/etc/nftables.conf"]


def test_core_daemon_has_agents_and_actions_off(tmp_path):
    files, _ = rendered(tmp_path, "n1")
    overrides = json.loads(files["/etc/forkmesh-next/mirror-node.overrides.json"])
    assert overrides == {
        "catalogUrl": "https://app.next.forkmesh.com/api/repositories",
        "disableAgentJobs": True, "disableAgentLogin": True,
        "disableCloudflared": True, "enableActions": False, "role": "core",
        # Every core keeps a replica of the ForkMesh-hosted repositories
        # (server/hosted.go, docs/HOSTED_REPOS.md).
        "hosted": {"enabled": True, "root": "/var/lib/forkmesh/hosted",
                   "maxRepositoryBytes": 256 * 1024 * 1024},
        "upstreams": {"forkmesh/forkmesh": [
            "https://app.next.forkmesh.com/forkmesh/forkmesh",
            "https://n2.next.forkmesh.com/forkmesh/forkmesh.git",
            "https://n3.next.forkmesh.com/forkmesh/forkmesh.git"]},
        "eventDrivenUpstreams": [
            "https://app.next.forkmesh.com/forkmesh/forkmesh"]}


def test_core_upstreams_fall_back_to_every_peer_core_but_itself(tmp_path):
    data = fleet(tmp_path)
    names = [n["name"] for n in data["nodes"]]
    for name in names:
        files, _ = render.render_core(data, name)
        overrides = json.loads(files[render.DAEMON_OVERRIDES_PATH][0])
        upstreams = overrides["upstreams"]["forkmesh/forkmesh"]
        assert upstreams[0] == "https://app.next.forkmesh.com/forkmesh/forkmesh"
        assert upstreams[1:] == [
            "https://%s.next.forkmesh.com/forkmesh/forkmesh.git" % peer
            for peer in names if peer != name]
        assert overrides["eventDrivenUpstreams"] == upstreams[:1]


def test_hosted_repositories_can_be_turned_off(tmp_path):
    import render
    fleet = {"app_host": "app.next.forkmesh.com",
             "hosted": {"enabled": False}}
    overrides = json.loads(render.daemon_overrides(fleet, "core"))
    assert "hosted" not in overrides
    runner = json.loads(render.daemon_overrides(
        {"app_host": "app.next.forkmesh.com"}, "runner"))
    assert "hosted" not in runner


def test_app_host_takes_git_pushes_up_to_the_hosted_limit(tmp_path):
    import render
    files, _ = rendered(tmp_path, "n1")
    caddyfile = files["/etc/caddy/Caddyfile"]
    assert "path_regexp git_push %s" % render.GIT_PUSH_PATH in caddyfile
    assert "max_size %s" % render.GIT_PUSH_BODY_MAX in caddyfile
    import sys
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]
                           / "app" / "host"))
    from forkmesh_host import request_guard
    assert request_guard.body_limit(
        "POST", "/alice/app/git-receive-pack") == 256 * 1024 * 1024
    assert request_guard.body_limit(
        "POST", "/alice/app/git-upload-pack") == 16 * 1024 * 1024
    assert render.GIT_PUSH_BODY_MAX == "256MB"


def test_replacement_joins_instead_of_bootstrapping(tmp_path):
    files, manifest = rendered(tmp_path, "n2", replacement=True)
    rqlite = files["/etc/systemd/system/rqlite.service"]
    assert "-bootstrap-expect" not in rqlite
    assert "-join 10.77.0.1:4002,10.77.0.3:4002" in rqlite
    assert manifest["replacement"] is True


def test_unknown_peer_keys_are_commented_out(tmp_path):
    files, _ = render.render_core(fleet(tmp_path, keys=False), "n1")
    wg = files["/etc/wireguard/wg0.conf"][0]
    assert "\n[Peer]" not in wg and "#[Peer]" in wg


@pytest.mark.parametrize("mutate, message", [
    (lambda f: f.update(domain="forkmesh.com"), "domain"),
    (lambda f: f.update(app_host="app.forkmesh.com"), "app_host"),
    (lambda f: f.update(acme_issuer="n1"), "acme_issuer is gone"),
    (lambda f: f.update(dns={"reconciler": "maybe"}), "dns.reconciler"),
    (lambda f: f.update(instance_label_prefix="mirror"), "fmnext-"),
    (lambda f: f["nodes"][1].update(wg_ip="10.77.0.1"), "duplicate"),
])
def test_fleet_validation(tmp_path, mutate, message):
    import tomllib
    data = tomllib.loads(FLEET.read_text())
    mutate(data)
    path = tmp_path / "fleet.toml"
    path.write_text(_toml(data))
    with pytest.raises(render.RenderError, match=message):
        render.load_fleet(path, state_path=tmp_path / "none.json")


def _toml(data):
    lines = []
    tables = []
    for key, value in data.items():
        if isinstance(value, dict):
            tables.append((key, value))
        elif isinstance(value, list) and value and isinstance(value[0], dict):
            tables.append((key, value))
        else:
            lines.append("%s = %s" % (key, json.dumps(value)))
    for key, value in tables:
        items = value if isinstance(value, list) else [value]
        for item in items:
            lines.append("[[%s]]" % key if isinstance(value, list) else "[%s]" % key)
            lines += ["%s = %s" % (k, json.dumps(v)) for k, v in item.items()]
    return "\n".join(lines) + "\n"


@pytest.mark.parametrize("node", ["n1", "n2", "r1"])
def test_snapshots(tmp_path, node):
    data = fleet(tmp_path)
    if node == "r1":
        files, _ = render.render_runner(data, node)
    else:
        files, _ = render.render_core(data, node)
    for path in SNAPSHOT_FILES:
        if path not in files:
            continue
        snapshot = SNAPSHOTS / node / path.lstrip("/")
        content = files[path][0]
        if os.environ.get("FMNEXT_UPDATE_SNAPSHOTS") == "1":
            snapshot.parent.mkdir(parents=True, exist_ok=True)
            snapshot.write_text(content)
        assert snapshot.is_file(), "missing snapshot %s (FMNEXT_UPDATE_SNAPSHOTS=1)" % snapshot
        assert content == snapshot.read_text(), "%s changed; review and refresh" % path


def test_core_daemon_unit_is_unprivileged(tmp_path):
    files, _ = rendered(tmp_path, "n1")
    lines = files["/etc/systemd/system/forkmesh-mirror-node.service"].splitlines()
    assert "User=forkmesh-node" in lines and "CapabilityBoundingSet=" in lines
    assert "UMask=0077" in lines
    assert not any("systemd-journal" in line for line in lines)


def test_app_env_has_public_hosts_and_static_mesh_peers(tmp_path):
    for vault in (True, False):
        data = fleet(tmp_path)
        data.setdefault("app", {})["vault"] = vault
        files, _ = render.render_core(data, "n1")
        env = dict(line.split("=", 1) for line in files[
            "/etc/forkmesh-next/app.env"][0].splitlines()
            if line and not line.startswith("#"))
        assert env["FORKMESH_PUBLIC_HOSTS"] == (
            "app.next.forkmesh.com,world.next.forkmesh.com,"
            "n1.next.forkmesh.com,n2.next.forkmesh.com,n3.next.forkmesh.com")
        assert env["FORKMESH_MESH_PEERS"] == (
            "n1=http://10.77.0.1:8792,n2=http://10.77.0.2:8792,"
            "n3=http://10.77.0.3:8792")


def test_app_env_logs_d1_statements_unless_switched_off(tmp_path):
    for setting, expected in ((None, "1"), (True, "1"), (False, "0")):
        data = fleet(tmp_path)
        if setting is not None:
            data.setdefault("app", {})["d1_log"] = setting
        files, _ = render.render_core(data, "n1")
        assert "FORKMESH_HOST_D1_LOG=%s\n" % expected in files[
            "/etc/forkmesh-next/app.env"][0]
    data = fleet(tmp_path)
    data.setdefault("app", {})["d1_log"] = "yes"
    with pytest.raises(render.RenderError):
        render.render_core(data, "n1")


def test_units_never_swap_secrets_and_sysctl_limits_ptrace(tmp_path):
    files, _ = rendered(tmp_path, "n1")
    for unit in ("rqlite.service", "forkmesh-app.service",
                 "fmnext-certsync.service", "fmnext-cert-sync.service",
                 "caddy.service.d/forkmesh-next.conf"):
        assert "MemorySwapMax=0" in files["/etc/systemd/system/" + unit].splitlines()
    assert "kernel.yama.ptrace_scope = 1" in files[
        "/etc/sysctl.d/90-forkmesh-next.conf"].splitlines()


def test_caddy_dropin_sandbox(tmp_path):
    files, _ = rendered(tmp_path, "n1")
    lines = files["/etc/systemd/system/caddy.service.d/forkmesh-next.conf"].splitlines()
    for setting in ("ProtectSystem=strict", "ReadWritePaths=/var/lib/caddy",
                    "NoNewPrivileges=true", "PrivateTmp=true", "ProtectHome=true",
                    "PrivateDevices=true", "SystemCallFilter=@system-service",
                    "RuntimeDirectory=caddy", "RuntimeDirectoryMode=0700",
                    "IPAddressDeny=169.254.0.0/16", "Restart=on-failure",
                    "RestartSec=2s"):
        assert setting in lines
    # Settings that add up across drop-ins are reset before being set.
    for key, value in (("CapabilityBoundingSet", "CAP_NET_BIND_SERVICE"),
                       ("AmbientCapabilities", "CAP_NET_BIND_SERVICE"),
                       ("SystemCallFilter", "@system-service")):
        index = lines.index("%s=%s" % (key, value))
        assert lines[index - 1] == key + "="
    # certsync's prepare (root, outside the sandbox) writes the includes.
    assert any(l.startswith("ExecStartPre=+") for l in lines)


def test_caddy_admin_is_a_unix_socket_and_limits_are_set(tmp_path):
    files, _ = rendered(tmp_path, "n1")
    caddy = files["/etc/caddy/Caddyfile"]
    assert "\tadmin unix//run/caddy/admin.sock" in caddy.splitlines()
    assert "localhost:2019" not in caddy and "admin off" not in caddy
    for line in ("read_header 10s", "read_body 5m", "idle 2m",
                 "max_header_size 64KB", "max_size 16MB", "max_size 32MB",
                 "max_size 1GB",
                 "Strict-Transport-Security \"max-age=31536000; includeSubDomains\"",
                 "X-Content-Type-Options nosniff"):
        assert line in caddy, line
    assert "path_regexp ^/api/repo/[^/]+/[^/]+/pulls$" in caddy
    upstream = caddy.split("(app_upstream) {", 1)[1].split("\n}\n", 1)[0]
    assert "keepalive 10s" in upstream
    assert "path_regexp ^/_app/api/repo/[^/]+/[^/]+/pulls$" in caddy
    # Every site imports the headers (the shared names via site_common).
    node_site = caddy.split("n1.next.forkmesh.com {", 1)[1]
    assert "import security_headers" in node_site
    site_common = caddy.split("(site_common) {", 1)[1].split("}", 1)[0]
    assert "import security_headers" in site_common
    assert "import app_body_limits" in site_common


def test_node_relay_never_reaches_host_internal_paths(tmp_path):
    files, _ = rendered(tmp_path, "n1")
    caddy = files["/etc/caddy/Caddyfile"]
    node_site = caddy.split("n1.next.forkmesh.com {", 1)[1]
    relay = node_site.split("\thandle_path /_app/* {\n", 1)[1].split(
        "\n\t}\n", 1)[0].splitlines()
    assert relay == ["\t\t@relay_internal path /_host/* /_do/* /_rpc/*",
                     "\t\thandle @relay_internal {", "\t\t\trespond 404",
                     "\t\t}", "\t\thandle {",
                     "\t\t\timport app_proxy_app_host", "\t\t}"]


def test_private_git_snippet_is_imported_first(tmp_path):
    # tools/next_git_host.py writes /etc/forkmesh-next/caddy/git-private.caddy;
    # the per-node and app.next sites import it by glob, before any handle,
    # and world.next never serves it.
    files, _ = rendered(tmp_path, "n1")
    caddy = files["/etc/caddy/Caddyfile"]
    line = "\timport /etc/forkmesh-next/caddy/git-private*.caddy"
    sites = {}
    for host in ("app.next.forkmesh.com", "world.next.forkmesh.com",
                 "n1.next.forkmesh.com"):
        sites[host] = caddy.split("\n%s {\n" % host, 1)[1].split("\n}\n", 1)[0]
    for host in ("app.next.forkmesh.com", "n1.next.forkmesh.com"):
        body = sites[host].splitlines()
        assert body.count(line) == 1, host
        first_handle = min(i for i, text in enumerate(body)
                           if text.lstrip().startswith(("handle", "import site_common",
                                                        "import client_headers")))
        assert body.index(line) < first_handle, host
    assert line not in sites["world.next.forkmesh.com"]
    assert caddy.count(line) == 2
    dropin = files["/etc/systemd/system/caddy.service.d/forkmesh-next.conf"]
    assert "ReadOnlyPaths=-/var/lib/forkmesh-next/git" in dropin
    assert "ProtectSystem=strict" in dropin


def test_public_ipv6_is_optional_and_validated(tmp_path):
    text = (REPO / "deploy" / "fleet.example.toml").read_text("utf-8")
    path = tmp_path / "fleet.toml"
    ok = text.replace('public_ipv4 = "203.0.113.11"\n',
                      'public_ipv4 = "203.0.113.11"\n'
                      'public_ipv6 = "2001:19f0:0005:0001::0011"\n', 1)
    path.write_text(ok)
    data = render.load_fleet(path, state_path=tmp_path / "none.json")
    assert data["nodes"][0]["public_ipv6"] == "2001:19f0:5:1::11"
    assert "public_ipv6" not in data["nodes"][1]
    _, manifest = render.render_core(data, "n1")
    assert manifest["public_ipv6"] == "2001:19f0:5:1::11"
    for bad in ("203.0.113.9", "fd00::1", "2001:db8::1", "fe80::1", "nope"):
        path.write_text(text.replace('public_ipv4 = "203.0.113.11"\n',
                                     'public_ipv4 = "203.0.113.11"\n'
                                     'public_ipv6 = "%s"\n' % bad, 1))
        with pytest.raises(render.RenderError):
            render.load_fleet(path, state_path=tmp_path / "none.json")


def labelled_fleet(tmp_path):
    text = (REPO / "deploy" / "fleet.example.toml").read_text("utf-8")
    for name, number in (("n1", "11"), ("n2", "12"), ("n3", "13"), ("r1", "21")):
        kind = "run" if name == "r1" else "core"
        text = text.replace('public_ipv4 = "203.0.113.%s"\n' % number,
                            'label = "fmnext-%s-%s"\n' % (kind, name[1:]), 1)
    path = tmp_path / "fleet.toml"
    path.write_text(text)
    assert "public_ipv4 =" not in text
    return path


INSTANCES = {"fmnext-core-1": {"ip": "203.0.113.11", "ipv6": "2001:19f0:ffff::11"},
             "fmnext-core-2": {"ip": "203.0.113.12"},
             "fmnext-core-3": {"ip": "203.0.113.13", "ipv6": "2001:19f0:ffff::13"},
             "fmnext-run-1": {"ip": "203.0.113.21"}}


def test_public_addresses_come_from_the_instances_file_by_label(tmp_path, monkeypatch):
    path = labelled_fleet(tmp_path)
    state = tmp_path / "none.json"
    inline = render.load_fleet(FLEET, state_path=state)
    data = render.load_fleet(path, state_path=state, instances=INSTANCES)
    assert [n["public_ipv4"] for n in data["nodes"]] == [
        n["public_ipv4"] for n in inline["nodes"]]
    assert data["nodes"][0]["public_ipv6"] == "2001:19f0:ffff::11"
    assert "public_ipv6" not in data["nodes"][1]
    assert data["runners"][0]["public_ipv4"] == "203.0.113.21"
    (tmp_path / "fleet-instances.json").write_text(json.dumps(INSTANCES))
    monkeypatch.setenv("FMNEXT_CONFIG_DIR", str(tmp_path))
    again = render.load_fleet(path, state_path=state)
    assert again["nodes"] == data["nodes"]
    explicit = tmp_path / "other.json"
    explicit.write_text(json.dumps(INSTANCES))
    assert render.load_fleet(path, state_path=state, instances_path=explicit)[
        "runners"] == data["runners"]


def test_a_node_without_an_address_fails_loudly(tmp_path):
    path = labelled_fleet(tmp_path)
    state = tmp_path / "none.json"
    with pytest.raises(render.RenderError, match="fleet-instances.json"):
        render.load_fleet(path, state_path=state)
    partial = {k: v for k, v in INSTANCES.items() if k != "fmnext-core-3"}
    with pytest.raises(render.RenderError, match="n3 \\(fmnext-core-3\\)"):
        render.load_fleet(path, state_path=state, instances=partial)
    empty = dict(INSTANCES, **{"fmnext-run-1": {"ip": ""}})
    with pytest.raises(render.RenderError, match="r1"):
        render.load_fleet(path, state_path=state, instances=empty)
    with pytest.raises(ValueError):
        render.load_fleet(path, state_path=state, instances=dict(
            INSTANCES, **{"fmnext-core-1": {"ip": "not-an-ip"}}))


def test_ids_egress_logging_covers_ipv6():
    rules = "\n".join(render.ids_egress_log_rules())
    assert rules.count("ip6 daddr != { ::1, fe80::/10 }") == 2
    assert rules.count("ip daddr != { 127.0.0.0/8, 10.77.0.0/24 }") == 2


def test_runner_daemon_tmp_is_on_disk_and_jobs_may_swap(tmp_path):
    files, manifest = render.render_runner(fleet(tmp_path), "r1")
    lines = files["/etc/systemd/system/forkmesh-mirror-node.service"][0].splitlines()
    assert "PrivateTmp=false" in lines and "PrivateTmp=true" not in lines
    assert ("BindPaths=/var/lib/forkmesh-runner/tmp:/tmp "
            "/var/lib/forkmesh-runner/tmp:/var/tmp") in lines
    assert "ReadWritePaths=/var/lib/forkmesh /var/lib/forkmesh-runner /var/lib/forkmesh-job" in lines
    assert manifest["job_tmp"] == "/var/lib/forkmesh-runner/tmp"
    overrides = json.loads(files[render.DAEMON_OVERRIDES_PATH][0])
    assert overrides["runner"]["jobMemorySwapMax"] == "1G"
    core, core_manifest = rendered(tmp_path, "n1")
    core_lines = core["/etc/systemd/system/forkmesh-mirror-node.service"].splitlines()
    assert "PrivateTmp=true" in core_lines
    assert not [l for l in core_lines if l.startswith("BindPaths=")]
    assert "job_tmp" not in core_manifest


def test_ids_egress_log_rules_only_count_connection_attempts():
    tcp_rules = [line for line in render.ids_egress_log_rules()
                 if "tcp dport" in line]
    assert len(tcp_rules) == 2
    for line in tcp_rules:
        assert "ct state new" in line
        assert "tcp flags & (syn | ack) == syn" in line


def test_ssh_helper_is_socket_activated_sandboxed_and_only_the_app_connects(tmp_path):
    files, manifest = render.render_core(fleet(tmp_path), "n1")
    socket_unit = files["/etc/systemd/system/fmnext-ssh-helper.socket"][0]
    service = files["/etc/systemd/system/fmnext-ssh-helper.service"][0]
    assert "ListenStream=/run/fmnext-ssh-helper.sock" in socket_unit
    assert "SocketUser=forkmesh-app" in socket_unit
    assert "SocketMode=0600" in socket_unit
    for line in ("DynamicUser=yes", "NoNewPrivileges=true", "ProtectSystem=strict",
                 "ProtectHome=true", "PrivateTmp=true", "CapabilityBoundingSet=",
                 "MemorySwapMax=0", "LimitCORE=0", "ProtectProc=invisible"):
        assert line in service.split("\n"), line
    deny = next(l for l in service.split("\n") if l.startswith("IPAddressDeny="))
    for net in ("localhost", "link-local", "10.0.0.0/8", "172.16.0.0/12",
                "192.168.0.0/16", "100.64.0.0/10", "fc00::/7"):
        assert net in deny
    assert "--systemd-socket" in service
    assert "LoadCredential" not in service and "EnvironmentFile" not in service
    env = files["/etc/forkmesh-next/app.env"][0]
    assert "FORKMESH_VAR_RUNNER_SSH_HELPER_SOCKET=/run/fmnext-ssh-helper.sock" in env
    assert "fmnext-ssh-helper.socket" in manifest["units"]


def test_access_log_redacts_room_tickets_and_signed_node_reads(tmp_path):
    files, _ = rendered(tmp_path, "n1")
    caddy = files["/etc/caddy/Caddyfile"]
    query = caddy[caddy.index("request>uri query {"):]
    query = query[:query.index("}")]
    for name in ("token", "sig", "ticket", "node"):
        assert "\treplace %s REDACTED\n" % name in query


def fleet_with(tmp_path, **relays):
    text = FLEET.read_text("utf-8")
    for name, relay in relays.items():
        anchor = 'name = "%s"\n' % name
        assert text.count(anchor) == 1
        text = text.replace(anchor, anchor + 'mail_relay = "%s"\n' % relay)
    path = tmp_path / "fleet.toml"
    path.write_text(text, "utf-8")
    state = tmp_path / "state.json"
    state.write_text(json.dumps({"wg_pubkeys": KEYS}))
    return render.load_fleet(path, state_path=state)


def test_app_env_lists_only_the_cores_that_send_mail_themselves(tmp_path):
    files, _ = render.render_core(fleet_with(tmp_path, n3="n1"), "n2")
    app_env = files["/etc/forkmesh-next/app.env"][0]
    assert ("FORKMESH_MAIL_SENDERS=n1.next.forkmesh.com=203.0.113.11,"
            "n2.next.forkmesh.com=203.0.113.12\n") in app_env


@pytest.mark.parametrize("relays", [{"n3": "n9"}, {"n3": "n3"},
                                    {"n3": "n1", "n1": "n2"}])
def test_a_mail_relay_must_be_another_core_that_sends_itself(tmp_path, relays):
    with pytest.raises(render.RenderError):
        fleet_with(tmp_path, **relays)
