#!/usr/bin/env python3
"""ForkMesh fleet autoscaler, operator side (docs/fleet/AUTOSCALING.md).

The scaler runs on the cores. This tool is the desktop's signed control plane:

    next_autoscale.py status [--json]
    next_autoscale.py settings show [--json]
    next_autoscale.py settings set               (settings JSON on stdin)
    next_autoscale.py settings kill|unkill       (flip the kill switch)
    next_autoscale.py scale app|runner up|down [--node aN|bN] [--execute]
    next_autoscale.py init-secrets [--execute]   (vault names in secrets.env)
    next_autoscale.py snapshot build [--execute] (build and publish the boot snapshot)

Dry run is the default for everything that changes something.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import secrets as _secrets
import ssl
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import next_common as common  # noqa: E402
from next_common import GuardError, ToolError  # noqa: E402

sys.path.insert(0, str(common.REPO / "app" / "host"))
from forkmesh_host import autoscale_policy as policy  # noqa: E402

API_HOST = os.environ.get("FMNEXT_AUTOSCALE_API", "app.forkmesh.com")
VAULT_NAMES = ("VULTR_API_KEY", "AUTOSCALE_IDS_SUBMASTER", "AUTOSCALE_CERTSYNC_SECRET")
SNAPSHOT_DIR = "/opt/forkmesh-next/snapshot"


def operator_key():
    import next_ids
    return next_ids.load_operator_key()


def nonce():
    return _secrets.token_hex(16)


def now_ms():
    return int(time.time() * 1000)


def post(path, body, opener=None, timeout=60):
    request = urllib.request.Request("https://%s%s" % (API_HOST, path),
                                     data=json.dumps(body).encode(), method="POST")
    request.add_header("Content-Type", "application/json")
    request.add_header("User-Agent", "fmnext-autoscale-cli/1")
    opener = opener or (lambda r, timeout: urllib.request.urlopen(
        r, timeout=timeout, context=ssl.create_default_context()))
    try:
        with opener(request, timeout=timeout) as response:
            return json.loads(response.read() or b"{}")
    except urllib.error.HTTPError as error:
        try:
            reply = json.loads(error.read(4096) or b"{}")
        except ValueError:
            reply = {}
        raise ToolError("%s: HTTP %d %s" % (path, error.code,
                                            reply.get("error") or ""))
    except urllib.error.URLError as error:
        raise ToolError("%s: %s" % (path, error.reason))


def signed(kind, body, private=None):
    return policy.signed_document(kind, body, private or operator_key())


def fetch_status(opener=None, private=None):
    body = signed(policy.STATUS_KIND, {"ts": now_ms(), "nonce": nonce()}, private)
    return post("/api/fleet/autoscale/status", body, opener)


def push_settings(settings, opener=None, private=None):
    settings = policy.validate(settings)
    document = policy.settings_document(settings, private or operator_key(), now_ms())
    return post("/api/fleet/autoscale/settings", document, opener)


def queue_command(pool, direction, node=None, reason="", opener=None, private=None):
    body = {"ts": now_ms(), "nonce": nonce(), "pool": pool, "direction": direction,
            "node": node, "reason": reason[:200]}
    return post("/api/fleet/autoscale/command",
                signed(policy.COMMAND_KIND, body, private), opener)


def print_status(document):
    service = document.get("service") or {}
    print("settings: %s%s" % ("signed" if document.get("verified") else "NOT VERIFIED",
                              " (%s)" % document["settingsError"]
                              if document.get("settingsError") else ""))
    settings = document.get("settings") or {}
    print("kill switch: %s" % ("ON" if settings.get("killSwitch") else "off"))
    print("scaler: %s on the cores (owner %s, last tick %s)" % (
        "running" if service.get("active") else "NOT SEEN",
        service.get("owner") or "-",
        time.strftime("%H:%M:%S", time.localtime(service["lastTickMs"] / 1000))
        if service.get("lastTickMs") else "-"))
    for pool, info in (document.get("pools") or {}).items():
        conf = settings.get(pool) or {}
        print("%s pool: %s%s, %s-%s nodes, spent %.2f of %s cents in %s" % (
            pool, "on" if conf.get("enabled") else "off",
            " (dry run)" if conf.get("dryRun") else "", conf.get("min"),
            conf.get("max"), float(info.get("spentCents") or 0),
            conf.get("monthlyCapCents"), info.get("month") or "-"))
        decision = info.get("decision") or {}
        print("  decision: %s (%s)" % (decision.get("action", "-"),
                                      decision.get("reason", "")))
        print("  signals: %s" % json.dumps(info.get("signals") or {}, sort_keys=True))
        for node in info.get("nodes") or []:
            print("  %s %s %s %s" % (node["name"], node["label"], node.get("ip", ""),
                                     node["state"]))
    for item in (document.get("recent") or [])[:10]:
        print("  %s %s %s %s%s" % (
            time.strftime("%m-%d %H:%M:%S", time.localtime(item["ts"] / 1000)),
            item["pool"], item["event"], item.get("detail", "")[:120],
            "" if item.get("ok", True) else " (FAILED)"))


def init_secrets(runner):
    import next_ids
    path = common.config_dir() / "secrets.env"
    values = common.read_env_file(path)
    missing = [n for n in VAULT_NAMES if not values.get(n)]
    if not missing:
        runner.say("secrets.env already has %s" % ", ".join(VAULT_NAMES))
        return []

    def write():
        current = common.read_env_file(path)
        for name in missing:
            if name == "VULTR_API_KEY":
                current[name] = common.credential("vultr.env", "VULTR_API_KEY")
            elif name == "AUTOSCALE_IDS_SUBMASTER":
                current[name] = next_ids.autoscale_submaster(next_ids.master_key())
            else:
                current[name] = _secrets.token_urlsafe(48)
        text = "".join("%s=%s\n" % (k, v) for k, v in sorted(current.items()))
        fd = os.open(path.with_name(path.name + ".tmp"),
                     os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w") as handle:
            handle.write(text)
        os.replace(path.with_name(path.name + ".tmp"), path)
    runner.step("add %s to %s (values are never printed); then add the names to "
                "[app] optional_secrets and run next_vault.py sync --execute"
                % (", ".join(missing), path), write)
    return missing


SNAPSHOT_CLEAN = """
set -e
export DEBIAN_FRONTEND=noninteractive
apt-get clean
rm -rf /var/lib/fmnext-ids /etc/wireguard/wg0.key /etc/forkmesh /var/lib/forkmesh/identity \\
  /etc/forkmesh-next/creds/* /etc/forkmesh-next/rqlite.curlrc /tmp/* /var/tmp/* \\
  /root/.bash_history /var/lib/fmnext-join.done
find /var/log -type f -exec truncate -s 0 {} +
rm -f /etc/ssh/ssh_host_*
cloud-init clean --logs --seed
truncate -s 0 /etc/machine-id
rm -f /var/lib/dbus/machine-id
sync
"""


def snapshot_build(runner, sleep=time.sleep):
    import next_fleet
    import next_deploy
    import render
    fleet = common.load_fleet()
    vultr = next_fleet.Vultr.from_config()
    ssh = common.Ssh()
    label = next_fleet.label_for("snap", 1)
    settings = fetch_status().get("settings") or policy.validate({})
    plan = settings["app"]["plan"]
    region = settings["app"]["region"]
    runner.say("snapshot build: %s (%s, %s) -> base install for app replicas and "
               "burst runners, binaries, the join agent; no keys; snapshot; destroy "
               "the builder; publish the id" % (label, plan, region))
    if not runner.execute:
        runner.say("[dry-run] nothing created")
        return None
    builder = next_fleet.create("snap", 1, vultr, runner, plan_id=plan, region=region)
    try:
        live = common.wait_for(lambda: (lambda i: i if i.get("status") == "active"
                                        and i.get("main_ip") not in (None, "", "0.0.0.0")
                                        else None)(vultr.instance(builder["id"])),
                               900, interval=10, sleep=sleep)
        if not live:
            raise ToolError("the builder did not become active")
        ip = live["main_ip"]
        if not common.wait_for(lambda: ssh.run(ip, "true", check=False,
                                               timeout=30).returncode == 0,
                               600, interval=10, sleep=sleep):
            raise ToolError("the builder never answered SSH")
        runner.say("builder %s at %s" % (label, ip))
        ssh.run(ip, next_fleet.base_script("core"), timeout=1800)
        ssh.run(ip, next_fleet.base_script("runner"), timeout=1800)
        version, sha256, tarball = next_fleet.rqlite_tarball(fleet)
        if hashlib.sha256(tarball.read_bytes()).hexdigest() != sha256:
            raise GuardError("local rqlite tarball does not match fleet.toml")
        ssh.put(ip, tarball, "/tmp/%s" % tarball.name)
        ssh.run(ip, next_fleet.rqlite_script(version, sha256))
        caddy, caddy_tarball, binary_sha = next_fleet.caddy_release(fleet)
        ssh.put(ip, caddy_tarball, "/tmp/%s" % caddy_tarball.name)
        dropin = render.caddy_binary_dropin(fleet)
        ssh.run(ip, "\n".join([
            "set -e", "stage=$(mktemp -d)",
            "echo %s'  '/tmp/%s | sha256sum -c -" % (caddy["sha256"], caddy_tarball.name),
            "tar -xzf /tmp/%s -C \"$stage\" caddy" % caddy_tarball.name,
            "echo %s'  '\"$stage/caddy\" | sha256sum -c -" % binary_sha,
            "install -m 0755 -o root -g root \"$stage/caddy\" %s" % render.CADDY_BIN,
            "install -d -m 0755 %s" % render.CADDY_BINARY_DROPIN.rsplit("/", 1)[0],
            "cat >%s" % render.CADDY_BINARY_DROPIN,
            "rm -rf \"$stage\" /tmp/%s" % caddy_tarball.name]),
            input_bytes=dropin.encode())
        dist = common.data_dir() / "dist"
        dist.mkdir(parents=True, exist_ok=True)
        binary = dist / "forkmesh-mirror-node"
        subprocess.run(["go", "build", "-trimpath", "-o", str(binary),
                        "./cmd/forkmesh-mirror-node"], cwd=common.REPO / "server",
                       check=True, env=dict(os.environ, CGO_ENABLED="0",
                                            GOOS="linux", GOARCH="amd64"))
        ssh.run(ip, "install -d -m 0755 %s %s/tools /opt/forkmesh-next/bin"
                % (SNAPSHOT_DIR, SNAPSHOT_DIR))
        ssh.put(ip, binary, "%s/forkmesh-mirror-node" % SNAPSHOT_DIR)
        for tool in next_fleet.DAEMON_TOOLS:
            ssh.put(ip, common.REPO / tool, "%s/tools/" % SNAPSHOT_DIR)
        ssh.put(ip, common.REPO / "deploy" / "core" / "join_agent.py",
                "/opt/forkmesh-next/bin/fmnext-join")
        ssh.run(ip, "chmod 0755 %s/forkmesh-mirror-node /opt/forkmesh-next/bin/fmnext-join "
                "&& chmod 0644 %s/tools/*" % (SNAPSHOT_DIR, SNAPSHOT_DIR))
        public = Path(str(common.ssh_key_path()) + ".pub").read_text().split()
        ssh.run(ip, "umask 077; install -d -m 0700 /root/.ssh; cat >/root/.ssh/authorized_keys",
                input_bytes=(" ".join(public[:2]) + " fmnext-fleet\n").encode())
        ssh.run(ip, SNAPSHOT_CLEAN, check=False)
        description = "fmnext-autoscale %s rev %s" % (
            time.strftime("%Y-%m-%d %H:%M", time.gmtime()), next_deploy.git_rev(
                allow_dirty=True))
        snapshot = vultr.create_snapshot(builder["id"], description)
        snapshot_id = snapshot["id"]
        runner.say("snapshot %s requested; waiting for it to complete" % snapshot_id)
        done = common.wait_for(lambda: vultr.snapshot(snapshot_id).get("status")
                               == "complete", 3600, interval=30, sleep=sleep)
        if not done:
            raise ToolError("snapshot %s did not complete" % snapshot_id)
    finally:
        runner.say("destroying the builder %s" % label)
        vultr.delete_instance(builder["id"])
        data = common.load_json(next_fleet.instances_path(), {}) or {}
        if data.pop(label, None) is not None:
            common.save_json(next_fleet.instances_path(), data)
        if "ip" in locals():
            subprocess.run(["ssh-keygen", "-R", ip, "-f",
                            str(common.config_dir() / "known_hosts")],
                           capture_output=True, check=False)
    current = fetch_status().get("settings") or policy.validate({})
    current["app"]["snapshotId"] = snapshot_id
    current["runner"]["snapshotId"] = snapshot_id
    push_settings(current)
    runner.say("published snapshot %s for both pools" % snapshot_id)
    return snapshot_id


def readyz(ip, host="app.forkmesh.com", timeout=6):
    import http.client
    import socket
    context = ssl.create_default_context()
    try:
        with socket.create_connection((ip, 443), timeout=timeout) as raw:
            with context.wrap_socket(raw, server_hostname=host) as tls:
                tls.sendall(("GET /readyz HTTP/1.1\r\nHost: %s\r\nConnection: "
                             "close\r\n\r\n" % host).encode())
                response = http.client.HTTPResponse(tls)
                response.begin()
                return response.status, json.loads(response.read(4096) or b"{}")
    except (OSError, ssl.SSLError, ValueError) as error:
        return None, {"error": str(error)[:120]}


def check(name, out=print):
    import next_fleet
    import next_ids
    document = fetch_status()
    node = None
    for info in (document.get("pools") or {}).values():
        for item in info.get("nodes") or []:
            if item["name"] == name:
                node = item
    if node is None:
        raise ToolError("%s is not an autoscaled node right now" % name)
    fleet = common.load_fleet()
    ssh = common.Ssh()
    report = {"node": name, "label": node["label"], "ip": node["ip"],
              "state": node["state"]}
    if name.startswith("a"):
        members = next_fleet.cluster_nodes(fleet, ssh)
        entry = next((m for m in members if m.get("id") == name), None)
        report["rqlite"] = None if entry is None else {
            "voter": entry.get("voter"), "reachable": entry.get("reachable")}
        report["voters"] = sorted(m.get("id") for m in members if m.get("voter", True))
        report["readyz"] = readyz(node["ip"])
        import next_dns
        cf = next_dns.Cloudflare.from_config(allowed=set())
        report["dns"] = {host: sorted(r.get("content") for r in cf.records(host, "A"))
                         for host in ("app.forkmesh.com", "world.forkmesh.com")}
        report["inDns"] = node["ip"] in report["dns"]["app.forkmesh.com"]
    else:
        import urllib.parse
        peer = next_ids.healthy_peer(fleet, ssh)
        out_text = ssh.run(peer["public_ipv4"], common.rqlite_curl(
            "/db/query?level=strong&associative&q=" + urllib.parse.quote(
                "SELECT name, scope, status, allowed_owners, last_seen_at FROM "
                "runner_nodes WHERE name='fmnext-%s'" % name)), timeout=60).stdout
        rows = ((json.loads(out_text or b"{}").get("results") or [{}])[0]
                .get("rows") or [])
        report["runner"] = rows[0] if rows else None
    ids = next_ids.ids_command(ssh, node["ip"], "status", timeout=60)
    report["ids"] = {"ok": ids.get("ok"), "lockdown": ids.get("lockdown"),
                     "baselinePaths": ids.get("baselinePaths"),
                     "error": ids.get("error")}
    out(json.dumps(report, indent=2, sort_keys=True, default=str))
    return report


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("status")
    p.add_argument("--json", action="store_true")
    p = sub.add_parser("settings")
    p.add_argument("action", choices=("show", "set", "kill", "unkill"))
    p.add_argument("--json", action="store_true")
    p = sub.add_parser("scale")
    p.add_argument("pool", choices=policy.POOLS)
    p.add_argument("direction", choices=("up", "down"))
    p.add_argument("--node")
    p.add_argument("--reason", default="manual (operator)")
    common.add_execute_flags(p)
    p = sub.add_parser("check")
    p.add_argument("node")
    p = sub.add_parser("init-secrets")
    common.add_execute_flags(p)
    p = sub.add_parser("snapshot")
    p.add_argument("action", choices=("build",))
    common.add_execute_flags(p)
    args = parser.parse_args(argv)
    try:
        if args.command == "status":
            document = fetch_status()
            if args.json:
                print(json.dumps(document, sort_keys=True))
            else:
                print_status(document)
            return 0
        if args.command == "check":
            check(args.node)
            return 0
        if args.command == "settings":
            if args.action == "show":
                document = fetch_status()
                print(json.dumps({"ok": True, "settings": document.get("settings"),
                                  "verified": document.get("verified"),
                                  "settingsError": document.get("settingsError", ""),
                                  "defaults": policy.DEFAULTS}, sort_keys=True,
                                 indent=None if args.json else 2))
                return 0
            if args.action in ("kill", "unkill"):
                current = fetch_status().get("settings") or policy.validate({})
                current["killSwitch"] = args.action == "kill"
                print(json.dumps(push_settings(current)))
                return 0
            try:
                incoming = json.loads(sys.stdin.read() or "{}")
                reply = push_settings(incoming)
            except (ValueError, policy.PolicyError, ToolError) as error:
                print(json.dumps({"ok": False, "error": str(error)}))
                return 1
            print(json.dumps(reply))
            return 0 if reply.get("ok") else 1
        runner = common.runner_from_args(args)
        if args.command == "scale":
            if args.node and not (len(args.node) == 2 and args.node[0] ==
                                  ("a" if args.pool == "app" else "b")):
                raise GuardError("--node must be aN for app or bN for runner")
            if not runner.execute:
                runner.say("[dry-run] would queue %s %s%s on the cores" % (
                    args.pool, args.direction, " " + args.node if args.node else ""))
                print(json.dumps({"ok": True, "dryRun": True}))
                return 0
            print(json.dumps(queue_command(args.pool, args.direction, args.node,
                                           args.reason)))
            return 0
        if args.command == "init-secrets":
            init_secrets(runner)
            return 0
        if args.command == "snapshot":
            snapshot_build(runner)
            return 0
    except (ToolError, OSError, KeyError, subprocess.CalledProcessError) as error:
        print("next_autoscale: %s" % error, file=sys.stderr)
        return 2 if isinstance(error, GuardError) else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
