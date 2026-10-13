#!/usr/bin/env python3
"""ForkMesh Next secrets vault, driven from the operator desktop only.

    next_vault.py status                           # read-only
    next_vault.py sync    [--prune] [--no-reload] [--execute]
    next_vault.py unseal  [nN ...] [--if-sealed]  [--execute]
    next_vault.py seal    nN [nN ...] [--restart]  [--execute]
    next_vault.py reload  [nN ...]                 [--execute]

The app variables live in the ``host_secrets`` table, AES-256-GCM encrypted
under a key derived from ``DATA_KEY`` (app/host/forkmesh_host/vault_crypto.py;
docs/VAULT.md). ``DATA_KEY`` exists only on this desktop:
``<config>/DATA_KEY`` (one line, mode 0400/0600; preferred when present) or
the ``DATA_KEY=`` line of ``<config>/secrets.env``.

* ``sync`` encrypts every app variable of ``[app] secrets`` +
  ``optional_secrets`` (except ``DATA_KEY`` and the bootstrap credentials
  RQLITE_PASSWORD / FORKMESH_MESH_SECRET, which stay systemd credentials;
  the zone-wide Cloudflare DNS token is deliberately NOT synced, audit
  2026-09-24), **here**, then upserts the ciphertexts plus the ``__vault_check__`` row
  through an SSH tunnel to any core node with a healthy rqlite (rqlite
  forwards the transaction to its leader). It refuses
  when the vault holds rows sealed under another key (rotation: TODO, see
  docs/VAULT.md). ``--prune`` deletes rows for names no longer listed. The
  unsealed nodes then re-read the vault (``reload``) unless ``--no-reload``.
* ``unseal`` pipes the key over SSH **stdin** into
  ``/usr/local/sbin/fmnext-unseal`` on each node, which hands it to the
  app's unseal socket: never an argument, environment variable or file.
  ``--if-sealed`` first asks the node's ``/readyz`` and only unseals nodes
  reporting ``sealed`` (the operator timer, deploy/operator/fmnext-unseal.*).
  With ``[app] vault_peer_unseal`` a sealed node normally gets the key from
  an unsealed peer by itself within seconds; this is the fallback for a
  restart of every node at once.
* ``seal`` makes a node drop the key (it answers 503 until unsealed again).
  ``--restart`` also ends the app process (``seal-restart``: the dropped
  values are overwritten and the memory goes back to the kernel); systemd
  restarts it sealed, and its peers unseal it again unless it is
  quarantined (docs/IDS.md).

**Dry-run is the default**: nothing is sent without ``--execute``. No value
(secret or key) is ever printed; ``status`` shows names and key ids only.
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import stat
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import next_common as common  # noqa: E402
from next_common import GuardError, ToolError  # noqa: E402

sys.path.insert(0, str(common.REPO / "app" / "host"))
from forkmesh_host import vault_crypto  # noqa: E402
from forkmesh_host.vault_crypto import VaultError, VaultKey  # noqa: E402

UNSEAL = "/usr/local/sbin/fmnext-unseal"
READYZ_SCRIPT = "curl -sS --max-time 5 http://127.0.0.1:8080/readyz || true"
UPSERT_SQL = (
    "INSERT INTO host_secrets(name, nonce, ciphertext, key_id, updated_at) "
    "VALUES(?, ?, ?, ?, ?) ON CONFLICT(name) DO UPDATE SET "
    "nonce=excluded.nonce, ciphertext=excluded.ciphertext, "
    "key_id=excluded.key_id, updated_at=excluded.updated_at")
LIST_SQL = "SELECT name, key_id, updated_at FROM host_secrets ORDER BY name"
# Vault variables that come from another operator file than secrets.env:
# vault name -> (file in the config dir, variable there). The Cloudflare DNS
# token/zone let the nodes run the DNS health loop themselves later.
# Security audit 2026-09-24: the Cloudflare token can edit the WHOLE
# forkmesh.com zone (production app, mail, CAA), so it must not live on the
# nodes. Re-add these only with a token scoped to a dedicated next zone.
EXTRA_SOURCES = {}


# --------------------------------------------------------------------------
# the key (desktop only)


def data_key_path() -> Path:
    return common.config_dir() / "DATA_KEY"


def load_data_key(out=None) -> str:
    """DATA_KEY from <config>/DATA_KEY (preferred) or secrets.env."""
    path = data_key_path()
    if path.exists():
        mode = path.stat().st_mode
        if not stat.S_ISREG(mode):
            raise GuardError("%s is not a regular file" % path)
        if mode & 0o077:
            raise GuardError("%s is readable by group/other; chmod 0400 it" % path)
        lines = path.read_text("utf-8").splitlines()
        text = lines[0] if lines else ""
        legacy = common.config_dir() / "secrets.env"
        if legacy.is_file() and common.read_env_file(legacy).get("DATA_KEY") \
                and out is not None:
            print("note: %s also holds DATA_KEY; remove that line once %s is "
                  "backed up" % (legacy, path.name), file=out)
    else:
        text = common.read_env_file(common.config_dir() / "secrets.env").get(
            "DATA_KEY", "")
    try:
        return vault_crypto.check_key_text(text)
    except VaultError as error:
        raise ToolError("DATA_KEY: %s" % error) from None


# --------------------------------------------------------------------------
# fleet


def require_vault(fleet):
    import render
    if not render.vault_enabled(fleet):
        raise GuardError("[app] vault is not true in the fleet file")


def select_nodes(fleet, names):
    nodes = common.core_nodes(fleet) + list(fleet.get("app_nodes") or [])
    known = {n["name"] for n in nodes}
    for name in names or []:
        if name not in known:
            raise GuardError("%r is not a core node in the fleet file" % name)
    return [n for n in nodes if not names or n["name"] in names]


def parse_reply(completed):
    """The last JSON line a node printed (fmnext-unseal, curl readyz)."""
    text = (completed.stdout or b"").decode("utf-8", "replace").strip()
    for line in reversed(text.splitlines()):
        line = line.strip()
        if line.startswith("{"):
            try:
                return json.loads(line)
            except ValueError:
                return None
    return None


def node_readiness(ssh, node):
    try:
        completed = ssh.run(node["public_ipv4"], READYZ_SCRIPT, check=False,
                            timeout=60)
    except (subprocess.SubprocessError, OSError):
        return None
    return parse_reply(completed)


def is_sealed(readiness):
    return bool(readiness) and "sealed" in (readiness.get("reasons") or [])


def describe(readiness):
    if readiness is None:
        return "unreachable"
    if readiness.get("ok"):
        return "unsealed, ready"
    reasons = readiness.get("reasons") or []
    if "sealed" in reasons:
        return "SEALED"
    return "not ready (%s)" % ", ".join(str(r) for r in reasons)


# --------------------------------------------------------------------------
# rqlite over the SSH tunnel


class RqliteHttp:
    def __init__(self, url, user, password, opener=None):
        self.url = url.rstrip("/")
        self._auth = "Basic " + base64.b64encode(
            ("%s:%s" % (user, password)).encode()).decode()
        self.opener = opener or urllib.request.urlopen

    def __repr__(self):
        return "RqliteHttp(%s)" % self.url

    def _post(self, path, body):
        request = urllib.request.Request(
            self.url + path, data=json.dumps(body).encode(), method="POST",
            headers={"Authorization": self._auth,
                     "Content-Type": "application/json"})
        try:
            with self.opener(request, timeout=30) as response:
                raw = response.read()
        except urllib.error.HTTPError as error:
            raise ToolError("rqlite %s -> HTTP %d" % (path.split("?")[0], error.code))
        results = (json.loads(raw or b"{}").get("results") or [])
        for result in results:
            if result.get("error"):
                raise ToolError("rqlite: %s" % result["error"])
        return results

    def query(self, sql, *params):
        results = self._post("/db/query?level=strong&associative",
                             [[sql, *params]])
        return (results[0] if results else {}).get("rows") or []

    def execute(self, statements):
        return self._post("/db/execute?transaction", statements)


def leader_client(fleet, ssh, opener=None):
    """(node, client, close) for a healthy node's rqlite through an SSH
    tunnel. Any node will do: writes and strong reads are forwarded to
    rqlite's own leader, so no host-level role is involved."""
    import next_deploy
    values = common.read_env_file(common.config_dir() / "secrets.env")
    user = (fleet.get("rqlite") or {}).get("user", "fmnext")
    leader = next_deploy.find_rqlite_node(fleet, ssh)
    url, close = next_deploy.open_rqlite_tunnel(ssh, leader, wait=opener is None)
    client = RqliteHttp(url, user, values.get("RQLITE_PASSWORD", ""), opener)
    return leader, client, close


# --------------------------------------------------------------------------
# commands


def status(fleet, ssh, runner):
    try:
        local = VaultKey(load_data_key()).key_id
    except ToolError as error:
        local = None
        runner.say("local DATA_KEY: unavailable (%s)" % error)
    else:
        runner.say("local DATA_KEY: key_id %s" % local)
    rows = None
    for node in select_nodes(fleet, None):
        readiness = node_readiness(ssh, node)
        runner.say("%s: %s" % (node["name"], describe(readiness)))
        if rows is None and readiness is not None:
            completed = ssh.run(node["public_ipv4"], common.rqlite_curl(
                "/db/query?level=strong&associative&q=" + urllib.parse.quote(LIST_SQL)),
                check=False, timeout=60)
            try:
                body = json.loads(completed.stdout or b"{}")
                result = (body.get("results") or [{}])[0]
                rows = None if result.get("error") else (result.get("rows") or [])
            except ValueError:
                rows = None
    if rows is None:
        runner.say("vault rows: unknown (no node answered, or no host_secrets table)")
        return 1
    runner.say("vault rows: %d" % len(rows))
    for row in rows:
        key_id = str(row.get("key_id") or "")
        mark = "" if local is None else (" (matches)" if key_id == local else " (OTHER KEY)")
        runner.say("  %-32s key_id %s%s" % (row.get("name"), key_id, mark))
    return 0


def sync_plan(fleet):
    import render
    names = render.vault_secret_names(fleet)
    names += [n for n in EXTRA_SOURCES if n not in names]
    for name in names:
        if not vault_crypto.NAME_RE.match(name):
            raise GuardError("%r is not a vault name" % name)
    return names


def sync_values(names):
    """{name: value} for the vault, from secrets.env and EXTRA_SOURCES."""
    values = common.read_env_file(common.config_dir() / "secrets.env")
    files = {}
    out = {}
    missing = []
    for name in names:
        if name in EXTRA_SOURCES:
            filename, variable = EXTRA_SOURCES[name]
            if filename not in files:
                path = common.config_dir() / filename
                files[filename] = common.read_env_file(path) if path.is_file() else {}
            value = files[filename].get(variable)
            where = "%s (%s)" % (filename, variable)
        else:
            value, where = values.get(name), "secrets.env"
        if not value:
            missing.append("%s from %s" % (name, where))
        else:
            out[name] = value
    if missing:
        raise ToolError("missing: %s" % ", ".join(missing))
    return out


def sync(fleet, ssh, runner, prune=False, reload=True, opener=None, clock=time.time):
    require_vault(fleet)
    names = sync_plan(fleet)
    values = sync_values(names)

    def action():
        key = VaultKey(load_data_key(runner.out))
        rows = [key.check_row()] + [key.encrypt(n, values[n]) for n in names]
        leader, client, close = leader_client(fleet, ssh, opener)
        try:
            try:
                existing = client.query("SELECT name, key_id FROM host_secrets")
            except ToolError as error:
                if "no such table" in str(error):
                    raise ToolError("host_secrets does not exist yet: run "
                                    "tools/next_deploy.py migrate --execute first")
                raise
            foreign = sorted(str(r.get("name")) for r in existing
                             if str(r.get("key_id") or "") != key.key_id)
            if foreign:
                raise GuardError(
                    "the vault holds rows sealed under another DATA_KEY (%s; "
                    "local key_id %s). Key rotation is not implemented "
                    "(docs/VAULT.md)" % (", ".join(foreign), key.key_id))
            now = int(clock())
            statements = [[UPSERT_SQL, r["name"], r["nonce"], r["ciphertext"],
                           r["key_id"], now] for r in rows]
            keep = [vault_crypto.CHECK_NAME] + names
            stale = sorted(str(r.get("name")) for r in existing
                           if str(r.get("name")) not in keep)
            if prune and stale:
                statements.append(["DELETE FROM host_secrets WHERE name NOT IN (%s)"
                                   % ", ".join("?" * len(keep)), *keep])
            client.execute(statements)
        finally:
            close()
        runner.say("sealed %d variables + %s on %s (key_id %s)%s" % (
            len(names), vault_crypto.CHECK_NAME, leader["name"], key.key_id,
            "; pruned %s" % ", ".join(stale) if prune and stale else
            ("; not in the list (use --prune): %s" % ", ".join(stale) if stale else "")))
    runner.step("encrypt %s locally under DATA_KEY, then upsert them + %s on the "
                "first healthy node's rqlite through an SSH tunnel (one transaction%s)" % (
                    ", ".join(names), vault_crypto.CHECK_NAME,
                    "; delete unlisted rows" if prune else ""), action)
    if reload:
        return node_command(fleet, ssh, runner, None, "--reload", "re-read the vault",
                            tolerate=("sealed",))
    return 0


def node_command(fleet, ssh, runner, names, flag, what, tolerate=()):
    failures = 0
    for node in select_nodes(fleet, names):
        def action(node=node):
            reply = parse_reply(ssh.run(node["public_ipv4"], "%s %s" % (UNSEAL, flag),
                                        check=False, timeout=120)) or {}
            if reply.get("ok"):
                runner.say("%s: ok (%s)" % (node["name"],
                                            "sealed" if reply.get("sealed") else
                                            "%d secrets" % len(reply.get("secrets") or [])))
                return True
            error = reply.get("error") or "no reply"
            runner.say("%s: %s" % (node["name"], error))
            return error in tolerate
        if runner.step("%s: %s (%s %s)" % (node["name"], what, UNSEAL, flag),
                       action) is False:
            failures += 1
    return 1 if failures else 0


def unseal(fleet, ssh, runner, names=None, if_sealed=False):
    require_vault(fleet)
    failures = 0
    key_holder = {}

    def key():
        if "key" not in key_holder:
            key_holder["key"] = load_data_key(runner.out)
        return key_holder["key"]

    for node in select_nodes(fleet, names):
        def action(node=node):
            if if_sealed:
                readiness = node_readiness(ssh, node)
                if readiness is None:
                    runner.say("%s: unreachable" % node["name"])
                    return False
                if not is_sealed(readiness):
                    runner.say("%s: not sealed (%s)" % (node["name"], describe(readiness)))
                    return True
            return unseal_node(ssh, node, key(), runner)
        if runner.step("%s: pipe DATA_KEY over SSH stdin into %s%s" % (
                node["name"], UNSEAL, " if /readyz says sealed" if if_sealed else ""),
                action) is False:
            failures += 1
    return 1 if failures else 0


def unseal_node(ssh, node, key, runner, wait=30):
    completed = ssh.run(node["public_ipv4"], "%s --wait %d" % (UNSEAL, int(wait)),
                        input_bytes=(key + "\n").encode(), check=False,
                        timeout=wait + 90)
    reply = parse_reply(completed) or {}
    if reply.get("ok"):
        runner.say("%s: unsealed (%d secrets, key_id %s)%s" % (
            node["name"], len(reply.get("secrets") or []), reply.get("keyId"),
            " (already)" if reply.get("already") else ""))
        return True
    runner.say("%s: unseal failed: %s%s" % (
        node["name"], reply.get("error") or "no reply (exit %d)" % completed.returncode,
        " (retry after %ss)" % reply["retryAfter"] if reply.get("retryAfter") else ""))
    return False


def main(argv=None, ssh=None, opener=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--fleet")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("status")
    p = sub.add_parser("sync")
    p.add_argument("--prune", action="store_true")
    p.add_argument("--no-reload", action="store_true")
    common.add_execute_flags(p)
    p = sub.add_parser("unseal")
    p.add_argument("nodes", nargs="*")
    p.add_argument("--if-sealed", action="store_true")
    common.add_execute_flags(p)
    p = sub.add_parser("seal")
    p.add_argument("nodes", nargs="+")
    p.add_argument("--restart", action="store_true",
                   help="also end the app process (seal-restart)")
    common.add_execute_flags(p)
    p = sub.add_parser("reload")
    p.add_argument("nodes", nargs="*")
    common.add_execute_flags(p)
    args = parser.parse_args(argv)
    try:
        runner = common.runner_from_args(args)
        fleet = common.load_fleet(args.fleet)
        ssh = ssh or common.Ssh()
        select_nodes(fleet, getattr(args, "nodes", None))
        if args.command == "status":
            return status(fleet, ssh, runner)
        if args.command == "sync":
            code = sync(fleet, ssh, runner, args.prune, not args.no_reload, opener)
        elif args.command == "unseal":
            code = unseal(fleet, ssh, runner, args.nodes, args.if_sealed)
        elif args.command == "seal":
            require_vault(fleet)
            code = node_command(
                fleet, ssh, runner, args.nodes,
                "--seal-restart" if args.restart else "--seal",
                "drop DATA_KEY and every vault value" + (
                    ", then restart the app process" if args.restart else ""))
        else:
            require_vault(fleet)
            code = node_command(fleet, ssh, runner, args.nodes, "--reload",
                                "re-read the vault", tolerate=("sealed",))
        if not runner.execute:
            runner.say("(dry run: nothing was changed; pass --execute to apply)")
        return code
    except (ToolError, OSError, KeyError, subprocess.SubprocessError) as error:
        print("next_vault: %s" % error, file=sys.stderr)
        return 2 if isinstance(error, GuardError) else 1


if __name__ == "__main__":
    sys.exit(main())
