#!/usr/bin/env python3
"""ForkMesh Next intrusion detection, operator side (docs/IDS.md).

    next_ids.py keygen      [--execute]        # operator release key (once)
    next_ids.py fingerprint                    # the fleet SSH key's SHA256 line
    next_ids.py status      [nN|rN ...]        # read-only
    next_ids.py baseline    nN|rN [--path P ...] [--allow K ...] [--execute]
    next_ids.py expect      nN|rN --path P ... [--allow K ...] [--minutes M] [--execute]
    next_ids.py test-fire   nN|rN [--execute]
    next_ids.py release     nN|rN [--note TEXT] [--execute]

**Dry-run is the default**; nothing is sent without ``--execute``.

Keys (all on this desktop, in ``$FMNEXT_CONFIG_DIR``):

* ``FMNEXT_IDS_MASTER`` in secrets.env (``next_fleet.py init-secrets``).
  Each node's expected-change key is ``HMAC-SHA256(master,
  "fmnext-ids-node-key-v1:<node>")``, pushed to that node only, as the
  systemd credential ``FMNEXT_IDS_KEY`` (``next_fleet.py secrets``): a key
  taken from one node cannot authorize changes on another.
* ``ids-operator.key`` (0400): the Ed25519 key that signs releases. Its
  public half is ``[ids] operator_public_key`` in deploy/fleet.toml. It never
  leaves this desktop; no node can sign a release.

``baseline`` / ``expect`` push an HMAC-signed *expected change* manifest to
the node's IDS (``fmnext-ids expect``, over SSH stdin): changes under the
listed paths are accepted into the baseline during the window (default 15
minutes). ``next_deploy.py`` and ``next_fleet.py`` push one before every step
that changes watched files (``expect_step``); ``baseline`` then asks the IDS
to rescan at once.

``release`` ends a lockdown after investigation: it signs a release row
(clearing every quarantine record of the node), stores it through a healthy
peer's rqlite, tells the node's IDS (which lifts the firewall and starts the
services), and unseals the node from this desktop (``next_vault.py unseal``).
Peers never hand a released node the key before that desktop unseal. It
refuses a node that is no longer an rqlite voter (rebuild it:
``next_fleet.py replace-core``).

``test-fire`` raises a harmless ``test`` incident on a node and waits until
an unsealed core has delivered it (Pings/APNs + email), printing the counts.
"""

from __future__ import annotations

import argparse
import hashlib
import hmac
import json
import os
import secrets as _secrets
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import next_common as common  # noqa: E402
from next_common import GuardError, ToolError  # noqa: E402

sys.path.insert(0, str(common.REPO / "app" / "host" / "forkmesh_host"))
import ids_records  # noqa: E402

MASTER_NAME = "FMNEXT_IDS_MASTER"
OPERATOR_KEY_FILE = "ids-operator.key"
IDS = "/opt/forkmesh-next/bin/fmnext-ids"
DEFAULT_MINUTES = 15
MAX_MINUTES = 60
ALLOW_KINDS = ("users", "modules", "listeners", "reboot", "ptrace",
               "processes")
EXPECT_SCRIPT = (
    "if [ -x %s ] && [ -f /etc/forkmesh-next/ids.json ] && "
    "systemctl is-active -q fmnext-ids.service; then %s expect; else "
    "echo '{\"ok\": false, \"error\": \"not_running\"}'; exit 3; fi" % (
        IDS, IDS))


# --------------------------------------------------------------------------
# keys


AUTOSCALED_NODE_RE = __import__("re").compile(r"^[ab][1-9]$")


def autoscale_submaster(master):
    return hmac.new(master.encode("utf-8"), b"fmnext-ids-autoscale-submaster-v1",
                    hashlib.sha256).hexdigest()


def node_key(master, node):
    """The expected-change HMAC key of one node (its FMNEXT_IDS_KEY)."""
    if AUTOSCALED_NODE_RE.match(str(node)):
        master = autoscale_submaster(master)
    return hmac.new(master.encode("utf-8"),
                    ("fmnext-ids-node-key-v1:%s" % node).encode("utf-8"),
                    hashlib.sha256).hexdigest()


def master_key(required=True):
    path = common.config_dir() / "secrets.env"
    values = common.read_env_file(path) if path.is_file() else {}
    value = values.get(MASTER_NAME, "")
    if not value and required:
        raise ToolError("%s is not in %s (run next_fleet.py init-secrets "
                        "--execute)" % (MASTER_NAME, path))
    return value


def operator_key_path():
    return common.config_dir() / OPERATOR_KEY_FILE


def load_operator_key():
    path = operator_key_path()
    if not path.is_file():
        raise ToolError("%s is missing (next_ids.py keygen --execute)" % path)
    if path.stat().st_mode & 0o077:
        raise GuardError("%s is readable by group/other; chmod 0400 it"
                         % path)
    return ids_records.load_private(path)


def keygen(fleet, runner):
    path = operator_key_path()
    if path.is_file():
        public = ids_records.public_key(load_operator_key())
        runner.say("%s exists; public key: %s" % (path, public))
        return public

    def write():
        raw = ids_records.generate_private()
        path.parent.mkdir(parents=True, exist_ok=True)
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o400)
        with os.fdopen(fd, "w") as handle:
            handle.write(ids_records.b64u(raw) + "\n")
        public = ids_records.public_key(raw)
        runner.say("operator release key written to %s (back it up "
                   "offline)" % path)
        runner.say('set in deploy/fleet.toml [ids]: operator_public_key = '
                   '"%s"' % public)
        return public
    return runner.step("generate the operator release key %s (0400)" % path,
                       write)


def fleet_fingerprint(run=subprocess.run):
    pub = Path(str(common.ssh_key_path()) + ".pub")
    done = run(["ssh-keygen", "-lf", str(pub)], capture_output=True,
               text=True, check=False)
    if done.returncode != 0:
        raise ToolError("ssh-keygen -lf %s failed" % pub)
    return done.stdout.split()[1]


# --------------------------------------------------------------------------
# targets


def target(fleet, name):
    import render
    if name[:1] in ("n", "a"):
        return render.node_by_name(fleet, name)
    if name[:1] in ("r", "b"):
        return render.runner_by_name(fleet, name)
    raise GuardError("%r is not a node (nN, aN) or runner (rN, bN)" % name)


def parse_reply(completed):
    text = (completed.stdout or b"").decode("utf-8", "replace").strip()
    for line in reversed(text.splitlines()):
        if line.strip().startswith("{"):
            try:
                return json.loads(line)
            except ValueError:
                return None
    return None


def ids_command(ssh, ip, command, stdin=None, timeout=180):
    completed = ssh.run(ip, "%s %s" % (IDS, command), input_bytes=stdin,
                        check=False, timeout=timeout)
    return parse_reply(completed) or {"ok": False, "error": "no reply (exit "
                                      "%d)" % completed.returncode}


# --------------------------------------------------------------------------
# expected changes


def build_manifest(node, key, files=None, rescan=(), allow=(), reason="",
                   minutes=DEFAULT_MINUTES, now_ms=None):
    minutes = int(minutes)
    if not 1 <= minutes <= MAX_MINUTES:
        raise GuardError("--minutes must be 1..%d" % MAX_MINUTES)
    for kind in allow:
        if kind not in ALLOW_KINDS:
            raise GuardError("unknown --allow %r (known: %s)" % (
                kind, ", ".join(ALLOW_KINDS)))
    for path in list(files or {}) + list(rescan):
        if not str(path).startswith("/") or ".." in str(path).split("/"):
            raise GuardError("paths must be absolute: %r" % path)
    issued = int(now_ms if now_ms is not None else time.time() * 1000)
    manifest = {"v": 1, "node": node, "nonce": _secrets.token_hex(16),
                "issued_ms": issued, "expires_ms": issued + minutes * 60_000,
                "reason": str(reason)[:200], "files": dict(files or {}),
                "rescan": sorted(set(rescan)), "allow": sorted(set(allow))}
    body = {k: v for k, v in manifest.items() if k != "mac"}
    manifest["mac"] = hmac.new(key.encode("utf-8"), json.dumps(
        body, sort_keys=True, separators=(",", ":")).encode("utf-8"),
        hashlib.sha256).hexdigest()
    return manifest


def push_expectation(fleet, ssh, runner, name, files=None, rescan=(),
                     allow=(), reason="", minutes=DEFAULT_MINUTES):
    """Sign and push one manifest; returns True (accepted), None (no IDS
    running there, or the fleet has no IDS key yet) or raises."""
    import render
    if not render.ids_enabled(fleet):
        return None
    master = master_key(required=False)
    node = target(fleet, name)
    if not master:
        # The fleet's IDS is not set up yet (no key to sign with); a running
        # IDS would then treat the change as unexpected (docs/IDS.md).
        runner.say("%s: no %s in secrets.env; the IDS is not told about this "
                   "change" % (name, MASTER_NAME))
        return None

    def action():
        manifest = build_manifest(name, node_key(master, name), files,
                                  rescan, allow, reason, minutes)
        completed = ssh.run(node["public_ipv4"], EXPECT_SCRIPT,
                            input_bytes=json.dumps(manifest).encode(),
                            check=False, timeout=120)
        reply = parse_reply(completed) or {}
        if reply.get("ok"):
            return True
        if reply.get("error") == "not_running":
            runner.say("%s: the IDS is not running there; nothing to tell it"
                       % name)
            return None
        raise ToolError("%s: the IDS refused the expected-change manifest "
                        "(%s); stopping, because the change would look like "
                        "an intrusion" % (name, reply.get("error")
                                          or "no reply"))
    what = []
    if files:
        what.append("%d exact files" % len(files))
    if rescan:
        what.append("rescan %s" % ", ".join(sorted(rescan)))
    if allow:
        what.append("allow %s" % ",".join(sorted(allow)))
    return runner.step("%s: tell the IDS to expect %s (%s, signed, %d min)"
                       % (name, reason or "a change", "; ".join(what) or
                          "nothing", minutes), action)


BASE_RESCAN = ("/etc/passwd", "/etc/group", "/etc/shadow", "/etc/gshadow",
               "/etc/ssh/sshd_config", "/etc/ssh/sshd_config.d",
               "/etc/sudoers", "/etc/sudoers.d", "/etc/pam.d", "/etc/forkmesh",
               "/etc/systemd/system/cloud-init-log-reader.service",
               "/etc/systemd/system/multi-user.target.wants")
AUTHORIZED_KEYS = ("/root/.ssh/authorized_keys",)
WANTS = ("/etc/systemd/system/multi-user.target.wants",
         "/etc/systemd/system/timers.target.wants",
         "/etc/systemd/system/sockets.target.wants")


SYSTEMD_UNIT_DIR = "/etc/systemd/system"


def files_expectation(fleet, name, replacement=False):
    """Exact sha256 of every rendered file (the files step writes exactly
    these), removals of retired units, and the two files the node edits."""
    import next_fleet
    files, manifest = next_fleet.render_node(fleet, name, replacement)
    expected = {path: hashlib.sha256(content.encode("utf-8")).hexdigest()
                for path, (content, _mode) in files.items()}
    for path in list(expected):
        parent = path.rsplit("/", 1)[0]
        if parent.startswith(SYSTEMD_UNIT_DIR + "/") and parent.endswith(".d"):
            expected.setdefault(parent, "dir")
    for path in ("/etc/wireguard/wg0.conf", "/etc/forkmesh-next/hosts.fragment"):
        expected.pop(path, None)
    routes = sorted(manifest.get("release_routes") or {})
    for path in routes:
        expected.pop(path, None)
    for unit in manifest.get("retired_units") or []:
        expected["/etc/systemd/system/" + unit] = None
    return expected, ["/etc/hosts", "/etc/wireguard/wg0.conf",
                      "/etc/forkmesh-next/hosts.fragment"] + routes


def caddy_expectation(fleet, required=True):
    import next_fleet
    import render
    settings, _, binary_sha = next_fleet.caddy_release(fleet, required=required)
    dropin = render.caddy_binary_dropin(fleet)
    if binary_sha is None:
        return settings, None, [render.CADDY_BIN, render.CADDY_BINARY_DROPIN]
    return settings, {
        render.CADDY_BIN: binary_sha,
        render.CADDY_BINARY_DROPIN: hashlib.sha256(
            dropin.encode("utf-8")).hexdigest()}, []


def expect_step(fleet, ssh, runner, name, step, replacement=False):
    """The expected-change manifest for one next_fleet install step."""
    if step == "base":
        return push_expectation(fleet, ssh, runner, name, rescan=BASE_RESCAN,
                                allow=("users", "modules", "listeners",
                                       "processes"),
                                reason="next_fleet base")
    if step == "wireguard":
        return push_expectation(fleet, ssh, runner, name,
                                allow=("modules", "listeners"),
                                reason="next_fleet wireguard")
    if step == "rqlite":
        return push_expectation(fleet, ssh, runner, name, rescan=(
            "/usr/local/bin/rqlited", "/usr/local/bin/rqlite"),
            reason="next_fleet rqlite")
    if step == "files":
        files, rescan = files_expectation(fleet, name, replacement)
        return push_expectation(fleet, ssh, runner, name, files=files,
                                rescan=rescan, allow=("modules", "listeners"),
                                reason="next_fleet files")
    if step == "turn":
        import render
        if not render.turn_enabled(fleet):
            return None
        return push_expectation(fleet, ssh, runner, name, rescan=BASE_RESCAN + (
            "/etc/systemd/system/coturn.service", "/etc/forkmesh-next/creds")
            + WANTS, allow=("users", "processes", "listeners"),
            reason="next_fleet turn")
    if step == "calls":
        import render
        if not render.calls_enabled(fleet):
            return None
        return push_expectation(fleet, ssh, runner, name, rescan=BASE_RESCAN + (
            render.CALLS_BIN, "/etc/forkmesh-next/creds") + WANTS,
            allow=("users", "processes", "listeners"),
            reason="next_fleet calls")
    if step == "secrets":
        return push_expectation(fleet, ssh, runner, name, rescan=(
            "/etc/forkmesh-next/creds", "/etc/forkmesh-next/rqlite.curlrc"),
            reason="next_fleet secrets")
    if step == "start":
        return push_expectation(fleet, ssh, runner, name,
                                rescan=WANTS + AUTHORIZED_KEYS,
                                allow=("modules", "listeners", "processes"),
                                reason="next_fleet start")
    if step == "caddy":
        settings, files, rescan = caddy_expectation(
            fleet, required=runner.execute)
        return push_expectation(fleet, ssh, runner, name, files=files,
                                rescan=rescan,
                                allow=("processes", "listeners"),
                                reason="next_fleet caddy v%s"
                                % settings["version"])
    if step == "caddy-debian":
        import render
        return push_expectation(fleet, ssh, runner, name, files={
            render.CADDY_BIN: None, render.CADDY_BINARY_DROPIN: None},
            allow=("processes", "listeners"), reason="next_fleet caddy-debian")
    if step == "daemon":
        return push_expectation(fleet, ssh, runner, name, rescan=(
            "/usr/local/bin/forkmesh-mirror-node", "/usr/local/share/forkmesh",
            "/etc/forkmesh") + WANTS, allow=("processes", "listeners"),
            reason="next_fleet daemon")
    return None


def expect_activate(fleet, ssh, runner, name, rev):
    """next_deploy restart/rollback: ``current`` may point at any release
    (the gate can roll back), and the Caddy route snippets change."""
    import render
    paths = render.fleet_paths(fleet)
    return push_expectation(
        fleet, ssh, runner, name,
        files={paths["current"]: "link:%s/*" % paths["releases"]},
        rescan=(paths["releases"],
                paths["etc"] + "/caddy/app-routes.caddy",
                paths["etc"] + "/caddy/world-routes.caddy"),
        allow=("listeners",), reason="next_deploy %s" % rev)


# --------------------------------------------------------------------------
# rqlite through a healthy peer (SSH, credentials from the node's curlrc)

RQ_POST = ("curl -fsS --max-time 20 -K /etc/forkmesh-next/rqlite.curlrc -H "
           "'Content-Type: application/json' --data-binary @- "
           "'http://127.0.0.1:4001%s'")


def healthy_peer(fleet, ssh, exclude=()):
    import next_deploy
    for node in fleet.get("nodes") or []:
        if node["name"] in exclude:
            continue
        try:
            if next_deploy.cluster_has_leader(next_deploy.rqlite_status(
                    ssh, node["public_ipv4"])):
                return node
        except (ToolError, ValueError, subprocess.TimeoutExpired):
            continue
    raise ToolError("no healthy core peer (rqlite with a leader) outside %s"
                    % ", ".join(exclude))


def peer_sql(ssh, peer, path, body):
    completed = ssh.run(peer["public_ipv4"], RQ_POST % path,
                        input_bytes=json.dumps(body).encode(), timeout=60)
    reply = json.loads(completed.stdout or b"{}")
    results = reply.get("results") or []
    for result in results:
        if result.get("error"):
            raise ToolError("rqlite on %s: %s" % (peer["name"],
                                                  result["error"][:200]))
    return results


def peer_nodes(ssh, peer):
    out = ssh.run(peer["public_ipv4"], common.rqlite_curl(
        "/nodes?nonvoters&ver=2&timeout=2s"), timeout=60).stdout
    import next_fleet
    return next_fleet.parse_nodes(json.loads(out or b"{}"))


# --------------------------------------------------------------------------
# commands


def status(fleet, ssh, runner, names):
    failures = 0
    targets = [n["name"] for n in (fleet.get("nodes") or [])] + [
        n["name"] for n in (fleet.get("app_nodes") or [])] + [
        r["name"] for r in (fleet.get("runners") or [])]
    for name in names or targets:
        node = target(fleet, name)
        reply = ids_command(ssh, node["public_ipv4"], "status", timeout=60)
        if not reply.get("ok"):
            failures += 1
            runner.say("%s: %s" % (name, reply.get("error")))
            continue
        runner.say("%s: %s, baseline %d paths, %d expectations, keys %s%s%s"
                   % (name, "LOCKDOWN %s" % json.dumps(reply.get(
                       "lockdownIncident")) if reply.get("lockdown")
                       else "normal", reply.get("baselinePaths", 0),
                      reply.get("expectations", 0), ",".join(
                          k for k, v in (reply.get("keys") or {}).items()
                          if v) or "NONE",
                      ", quarantined %s" % ",".join(reply["quarantined"])
                      if reply.get("quarantined") else "",
                      ", blocking %s" % ",".join(reply["blocked"])
                      if reply.get("blocked") else ""))
        for item in (reply.get("recent") or [])[-5:]:
            runner.say("    %s %-8s %s: %s" % (
                time.strftime("%m-%d %H:%M:%S", time.gmtime(
                    int(item.get("ts") or 0) / 1000)), item.get("severity"),
                item.get("detector"), item.get("summary")))
    return 1 if failures else 0


def baseline(fleet, ssh, runner, name, paths=(), allow=()):
    import render
    node = target(fleet, name)
    role = "core" if name[:1] in ("n", "a") else "runner"
    config = json.loads(render.ids_config(fleet, name, role,
                                          render.fleet_paths(fleet)))
    integrity = config["integrity"]
    rescan = list(paths) or (integrity["lockdown"] + integrity["alert"]
                             + integrity["trees"])
    if not paths and role == "core":
        rescan.append(render.fleet_paths(fleet)["releases"])
    if push_expectation(fleet, ssh, runner, name, rescan=rescan,
                        allow=allow, reason="next_ids baseline") is False:
        return 1

    def rescan_now():
        reply = ids_command(ssh, node["public_ipv4"], "rescan")
        runner.say("%s: rescan %s" % (name, "done" if reply.get("ok")
                                      else reply.get("error")))
        return reply.get("ok")
    return 0 if runner.step("%s: rescan now (the changes are accepted into "
                            "the baseline)" % name, rescan_now) is not False \
        else 1


def test_fire(fleet, ssh, runner, name, wait=120, sleep=time.sleep):
    node = target(fleet, name)

    def fire():
        reply = ids_command(ssh, node["public_ipv4"], "test-fire")
        if not reply.get("ok"):
            raise ToolError("%s: test-fire failed: %s" % (name,
                                                          reply.get("error")))
        incident = reply.get("incident")
        channels = {k: v[0] if isinstance(v, list) else v for k, v in
                    (reply.get("delivery") or {}).items()}
        runner.say("%s: incident %s raised; channels %s" % (
            name, incident, json.dumps(channels, sort_keys=True)))
        # Delivery happens on an unsealed core (forkmesh_host.ids_alerts).
        checker = node if name[:1] == "n" else healthy_peer(fleet, ssh)
        deadline = time.monotonic() + wait
        while time.monotonic() < deadline:
            status = ids_command(ssh, checker["public_ipv4"],
                                 "incident-status %s" % incident)
            row = status.get("incident") or {}
            if row.get("delivered_ms"):
                try:
                    delivery = json.loads(row.get("delivery") or "{}")
                except ValueError:
                    delivery = {}
                runner.say("%s: delivered by %s: %s pings, %s emails "
                           "(email %s)%s" % (
                               name, delivery.get("by"), delivery.get("pings"),
                               delivery.get("emails"),
                               delivery.get("email") or "-",
                               " (errors %s)" % delivery["errors"]
                               if delivery.get("errors") else ""))
                if not delivery.get("pings") and not delivery.get("emails"):
                    raise ToolError("delivered to nobody: are there admins "
                                    "with a verified email / a phone?")
                return True
            sleep(5)
        raise ToolError("%s: incident %s not delivered within %d s (is any "
                        "core unsealed? journalctl -u forkmesh-app | grep "
                        "ids:)" % (name, incident, wait))
    return 0 if runner.step("%s: raise a harmless test incident and wait for "
                            "its delivery" % name, fire) is not False else 1


def release(fleet, ssh, runner, name, note="", unseal=True, sleep=time.sleep):
    node = target(fleet, name)
    is_core = name.startswith("n")

    def action():
        private = load_operator_key()
        public = ids_records.public_key(private)
        import render
        configured = render.ids_settings(fleet).get("operator_public_key")
        if configured != public:
            raise GuardError("config/%s does not match [ids] "
                             "operator_public_key" % OPERATOR_KEY_FILE)
        status_reply = ids_command(ssh, node["public_ipv4"], "status",
                                   timeout=60)
        runner.say("%s: %s" % (name, "LOCKDOWN %s" % json.dumps(
            status_reply.get("lockdownIncident")) if status_reply.get(
                "lockdown") else status_reply.get("error") or "not locked "
            "down (releasing any quarantine record)"))
        clears = []
        peer = None
        if is_core:
            peer = healthy_peer(fleet, ssh, exclude=(name,))
            voters = {str(n.get("id")): n for n in peer_nodes(ssh, peer)}
            if name not in voters:
                raise GuardError(
                    "%s is no longer an rqlite member (the peers removed it "
                    "as a voter). Do not release it: rebuild it with "
                    "next_fleet.py replace-core %s --new nX" % (name, name))
            rows = peer_sql(ssh, peer, "/db/query?level=strong&associative",
                            [["SELECT id FROM cluster_quarantine WHERE "
                              "node_id=? AND kind != 'release'", name]])
            clears = [r["id"] for r in (rows[0].get("rows") or [])] \
                if rows else []
        body = ids_records.release_body(name, int(time.time() * 1000),
                                        clears, _secrets.token_hex(8), note)
        row = ids_records.make_row(
            "release", name, ids_records.OPERATOR, body,
            ids_records.sign(private, "release", body))
        if is_core:
            peer_sql(ssh, peer, "/db/execute",
                     [ids_records.insert_statement(row)])
            runner.say("%s: release %s stored through %s (clears %d "
                       "records)" % (name, row["id"], peer["name"],
                                     len(clears)))
        reply = ids_command(ssh, node["public_ipv4"], "release",
                            stdin=json.dumps(row).encode())
        if not reply.get("ok"):
            raise ToolError("%s: the IDS refused the release: %s" % (
                name, reply.get("note") or reply.get("error")))
        runner.say("%s: IDS %s" % (name, reply.get("note")))
        if not is_core:
            runner.say("%s: the runner was revoked at lockdown; register it "
                       "again from /dashboard/runners once you trust it"
                       % name)
            return True
        if unseal:
            import next_vault
            # The app restarts and must first read the release (its local
            # rqlite may be starting too): retry for about two minutes.
            for attempt in range(8):
                if next_vault.unseal(fleet, ssh, runner, [name]) == 0:
                    break
                if attempt == 7:
                    raise ToolError("%s: the desktop unseal failed; run "
                                    "next_vault.py unseal %s --execute" % (
                                        name, name))
                sleep(15)
        return True
    return 0 if runner.step(
        "%s: sign a release (operator key), store it through a healthy "
        "peer, tell the IDS (firewall off, services on)%s" % (
            name, ", unseal it from this desktop" if is_core and unseal
            else ""), action) is not False else 1


def main(argv=None, ssh=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--fleet")
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("keygen")
    common.add_execute_flags(p)
    sub.add_parser("fingerprint")
    p = sub.add_parser("status")
    p.add_argument("targets", nargs="*")
    p = sub.add_parser("baseline")
    p.add_argument("target")
    p.add_argument("--path", action="append", default=[])
    p.add_argument("--allow", action="append", default=[])
    common.add_execute_flags(p)
    p = sub.add_parser("expect")
    p.add_argument("target")
    p.add_argument("--path", action="append", default=[])
    p.add_argument("--allow", action="append", default=[])
    p.add_argument("--minutes", type=int, default=DEFAULT_MINUTES)
    p.add_argument("--reason", default="next_ids expect")
    common.add_execute_flags(p)
    p = sub.add_parser("test-fire")
    p.add_argument("target")
    common.add_execute_flags(p)
    p = sub.add_parser("release")
    p.add_argument("target")
    p.add_argument("--note", default="")
    p.add_argument("--no-unseal", action="store_true")
    common.add_execute_flags(p)
    args = parser.parse_args(argv)
    try:
        runner = common.runner_from_args(args)
        if args.command == "fingerprint":
            print("ssh_fleet_fingerprints = [\"%s\"]" % fleet_fingerprint())
            return 0
        fleet = common.load_fleet(args.fleet)
        ssh = ssh or common.Ssh()
        if args.command == "keygen":
            keygen(fleet, runner)
            return 0
        if args.command == "status":
            return status(fleet, ssh, runner, args.targets)
        target(fleet, args.target)
        if args.command == "baseline":
            code = baseline(fleet, ssh, runner, args.target, args.path,
                            args.allow)
        elif args.command == "expect":
            if not args.path and not args.allow:
                raise ToolError("expect needs --path and/or --allow")
            push_expectation(fleet, ssh, runner, args.target,
                             rescan=args.path, allow=args.allow,
                             reason=args.reason, minutes=args.minutes)
            code = 0
        elif args.command == "test-fire":
            code = test_fire(fleet, ssh, runner, args.target)
        else:
            code = release(fleet, ssh, runner, args.target, args.note,
                           unseal=not args.no_unseal)
        if not runner.execute:
            runner.say("(dry run: nothing was changed; pass --execute to "
                       "apply)")
        return code
    except (ToolError, OSError, KeyError, ValueError) as error:
        print("next_ids: %s" % error, file=sys.stderr)
        return 2 if isinstance(error, GuardError) else 1


if __name__ == "__main__":
    sys.exit(main())
