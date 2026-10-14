#!/usr/bin/env python3
from __future__ import annotations

import argparse
import base64
import json
import os
import secrets
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import next_common as common
from next_common import GuardError, ToolError

APP_URL = "https://app.forkmesh.com"
IDENTITY_PEM = "/var/lib/forkmesh/identity/ed25519.pem"
NODE_PREFIX = "fmnext-"
RQ_POST = ("curl -fsS --max-time 20 -K /etc/forkmesh-next/rqlite.curlrc -H "
           "'Content-Type: application/json' --data-binary @- "
           "'http://127.0.0.1:4001%s'")

SIGNER = r'''
import base64, sys
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
with open(sys.argv[1], "rb") as handle:
    key = serialization.load_pem_private_key(handle.read(), None)
if not isinstance(key, Ed25519PrivateKey):
    raise SystemExit("identity key is not Ed25519")
enc = lambda v: base64.urlsafe_b64encode(v).decode().rstrip("=")
raw = key.public_key().public_bytes(serialization.Encoding.Raw,
                                    serialization.PublicFormat.Raw)
message = "\n".join(["forkmesh-link-grant-v2"] + sys.argv[2:6]).encode()
print(enc(raw) + " " + enc(key.sign(message)))
'''

ACCOUNT_SIGNER = r'''
import base64, sys
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
with open(sys.argv[1], "rb") as handle:
    key = serialization.load_pem_private_key(handle.read(), None)
if not isinstance(key, Ed25519PrivateKey):
    raise SystemExit("identity key is not Ed25519")
enc = lambda v: base64.urlsafe_b64encode(v).decode().rstrip("=")
raw = key.public_key().public_bytes(serialization.Encoding.Raw,
                                    serialization.PublicFormat.Raw)
node, ts = sys.argv[2], sys.argv[3]
reserve = ("forkmesh-reserve-v1\n" + node + "\n" + ts).encode()
finalize = ("forkmesh-finalize-v1\n" + node + "\n\n" + ts).encode()
print(enc(raw) + " " + enc(key.sign(reserve)) + " " + enc(key.sign(finalize)))
'''


def keyset():
    import next_rekey
    return next_rekey.read_key(common.config_dir() / "DATA_KEY", "production")


class Db:
    def __init__(self, fleet, ssh, keys):
        self.fleet = fleet
        self.ssh = ssh
        self.keys = keys
        self._peer = None

    def peer(self):
        if self._peer is None:
            import next_ids
            self._peer = next_ids.healthy_peer(self.fleet, self.ssh)
        return self._peer

    def post(self, path, body):
        completed = self.ssh.run(self.peer()["public_ipv4"], RQ_POST % path,
                                 input_bytes=json.dumps(body).encode(),
                                 timeout=60)
        reply = json.loads(completed.stdout or b"{}")
        results = reply.get("results") or []
        for result in results:
            if result.get("error"):
                raise ToolError("rqlite: %s" % result["error"][:200])
        return results

    def rows(self, sql, *args):
        results = self.post("/db/query?level=strong&associative",
                            [[sql, *args]])
        return (results[0].get("rows") or []) if results else []

    def execute(self, statements):
        return self.post("/db/execute?transaction", statements)

    def record(self, table, key, name):
        rows = self.rows("SELECT data FROM %s WHERE %s=?" % (table, key),
                         self.keys.bi(name))
        if not rows:
            return None
        plain = self.keys.open(rows[0].get("data"))
        return json.loads(plain) if plain else None

    def account(self, name):
        return self.record("users", "user_bi", name) or self.record(
            "nodes", "node_bi", name)

    def node_row(self, name):
        rows = self.rows("SELECT n.pubkey, n.user_bi, u.username FROM nodes n "
                         "LEFT JOIN users u ON u.user_bi=n.user_bi "
                         "WHERE n.node_bi=?", self.keys.bi(name))
        return rows[0] if rows else None

    def runner_row(self, name):
        rows = self.rows("SELECT name, public_key, registered_by, scope, "
                         "allowed_owners, status, last_seen_at FROM "
                         "runner_nodes WHERE name=?", name)
        return rows[0] if rows else None

    def is_admin(self, name):
        rows = self.rows("SELECT is_admin FROM users WHERE user_bi=?",
                         self.keys.bi(name))
        return bool(rows and int(rows[0].get("is_admin") or 0))

    def audit_statement(self, actor, action, target_type, target, details):
        return ["INSERT INTO sensitive_audit_log (ts, actor_bi, actor_label, "
                "action, target_type, target_token, outcome, details) "
                "VALUES (?,?,?,?,?,?,?,?)", int(time.time() * 1000),
                self.keys.bi(actor), actor, action, target_type,
                self.keys.bi("audit:%s:%s" % (target_type, target)), "success",
                json.dumps(details, sort_keys=True, separators=(",", ":"))]


def core_accounts(fleet):
    return [(node, NODE_PREFIX + node["name"]) for node in
            fleet.get("nodes") or []]


def runner_accounts(fleet):
    return [(node, NODE_PREFIX + node["name"]) for node in
            fleet.get("runners") or []]


def owned_nodes(record):
    nodes = (record or {}).get("nodes")
    return [str(n) for n in nodes] if isinstance(nodes, list) else []


def status(db, runner, fleet):
    for _node, account in core_accounts(fleet):
        record = db.account(account) or {}
        row = db.node_row(account) or {}
        runner.say("%s: kind=%s owner=%s nodes.user=%s status=%s" % (
            account, record.get("kind"), record.get("owner") or "-",
            row.get("username") or "-", record.get("status")))
    for _node, account in runner_accounts(fleet):
        row = db.runner_row(account)
        if not row:
            runner.say("%s: no runner_nodes row" % account)
            continue
        runner.say("%s: registered_by=%s scope=%s allowed_owners=%s status=%s"
                   % (account, row["registered_by"], row["scope"],
                      row["allowed_owners"], row["status"]))
    return 0


def grant_canonical(account, target, ts, nonce):
    return "\n".join(["forkmesh-link-grant-v2", account, target, ts,
                      nonce]).encode()


def accept_canonical(account, target, grant_sig, accept_ts):
    return "\n".join(["forkmesh-link-accept-v1", account, target, grant_sig,
                      accept_ts]).encode()


def b64url(raw):
    return base64.urlsafe_b64encode(raw).decode().rstrip("=")


class Acceptor:
    def __init__(self, key_path="", session_env=""):
        self.key_path = key_path
        self.session_env = session_env

    def ready(self):
        return bool(self.key_path or self.session_env)

    def describe(self):
        if self.key_path:
            return "the target's own key"
        if self.session_env:
            return "the target's session ($%s)" % self.session_env
        return "nobody (pass --accept-key or --session-env)"

    def session(self):
        if not self.session_env:
            return ""
        token = os.environ.get(self.session_env, "").strip()
        if not token:
            raise GuardError("$%s is empty" % self.session_env)
        return token

    def sign(self, message):
        from cryptography.hazmat.primitives import serialization
        from cryptography.hazmat.primitives.asymmetric.ed25519 import (
            Ed25519PrivateKey)
        with open(os.path.expanduser(self.key_path), "rb") as handle:
            key = serialization.load_pem_private_key(handle.read(), None)
        if not isinstance(key, Ed25519PrivateKey):
            raise GuardError("%s is not an Ed25519 key" % self.key_path)
        return b64url(key.sign(message))

    def fields(self, account, target, grant_sig):
        if not self.key_path:
            return {}
        accept_ts = str(int(time.time() * 1000))
        return {"acceptTs": accept_ts, "acceptSig": self.sign(
            accept_canonical(account, target, grant_sig, accept_ts))}


def sign_grant(ssh, node, account, target, ts, nonce):
    command = "python3 -I - %s %s %s %s %s" % (IDENTITY_PEM, account, target,
                                               ts, nonce)
    completed = ssh.run(node["public_ipv4"], command,
                        input_bytes=SIGNER.encode(), timeout=60)
    parts = (completed.stdout or b"").decode().split()
    if len(parts) != 2:
        raise ToolError("%s: the node signer returned no signature" % account)
    return parts[0], parts[1]


def sign_account(ssh, node, account, ts):
    command = "python3 -I - %s %s %s" % (IDENTITY_PEM, account, ts)
    completed = ssh.run(node["public_ipv4"], command,
                        input_bytes=ACCOUNT_SIGNER.encode(), timeout=60)
    parts = (completed.stdout or b"").decode().split()
    if len(parts) != 3:
        raise ToolError("%s: the node signer returned no signatures" % account)
    return parts[0], parts[1], parts[2]


def register_core(db, ssh, runner, node, account, app_url):
    record = db.account(account)
    row = db.node_row(account)
    if record and record.get("kind") == "node" and record.get(
            "status") == "active" and row:
        if runner.execute:
            pubkey = sign_account(ssh, node, account, "0")[0]
            if pubkey != row.get("pubkey"):
                raise ToolError(
                    "%s: the account is bound to another key than the node's "
                    "(a rebuilt core reusing the name); its daemon gets HTTP "
                    "401 until the owner reclaims the name "
                    "(docs/NODE_OWNERSHIP.md)" % account)
        runner.say("%s: node account already active" % account)
        return False
    if record and record.get("kind") not in (None, "node"):
        raise GuardError("%s is a %s account; refusing" % (
            account, record.get("kind")))
    if not runner.execute:
        runner.say("[dry-run] %s: node key signs a reserve and a finalize; "
                   "%s creates the node account" % (account, app_url))
        return False
    ts = str(int(time.time() * 1000))
    pubkey, reserve_sig, finalize_sig = sign_account(ssh, node, account, ts)
    code, reply = post_account(app_url, "reserve", {
        "nodeName": account, "pubkey": pubkey, "ts": ts, "sig": reserve_sig})
    if code not in (201, 409):
        raise ToolError("%s: reserve answered HTTP %d %s" % (
            account, code, reply.get("error") or ""))
    code, reply = post_account(app_url, "finalize", {
        "nodeName": account, "email": "", "password": "", "pubkey": pubkey,
        "ts": ts, "sig": finalize_sig, "linkCode": ""})
    if code != 201:
        raise ToolError("%s: finalize answered HTTP %d %s" % (
            account, code, reply.get("error") or ""))
    after = db.node_row(account) or {}
    if after.get("pubkey") != pubkey:
        raise ToolError("%s: nodes.pubkey reads back as %r" % (
            account, after.get("pubkey")))
    runner.say("%s: node account registered with its identity key" % account)
    return True


def register(db, ssh, runner, fleet, app_url, names=(), owner="",
             acceptor=None):
    for node, _account in core_accounts(fleet):
        if names and node["name"] not in names:
            continue
        provision_core(db, ssh, runner, fleet, node, app_url, owner, acceptor)
    return 0


def post_account(app_url, path, body):
    return post_json(app_url.rstrip("/") + "/api/accounts/" + path, body)


def post_grant(app_url, body, session=""):
    return post_json(app_url.rstrip("/") + "/api/accounts/link-grant", body,
                     session)


def post_json(url, body, session=""):
    headers = {"Content-Type": "application/json",
               "User-Agent": "fmnext-tools/1"}
    if session:
        headers["Authorization"] = "Bearer " + session
    request = urllib.request.Request(
        url, data=json.dumps(body).encode(), method="POST", headers=headers)
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            return response.status, json.loads(response.read() or b"{}")
    except urllib.error.HTTPError as error:
        return error.code, json.loads(error.read() or b"{}")


def link_core(db, ssh, runner, node, account, source, target, app_url,
              acceptor):
    record = db.account(account)
    row = db.node_row(account)
    if not record or record.get("kind") != "node" or not row:
        raise GuardError("%s: no active node account" % account)
    owner = record.get("owner") or ""
    if owner == target:
        runner.say("%s: already owned by %s" % (account, target))
        return False
    if owner != source:
        raise GuardError("%s: owner is %r, not %r; refusing" % (
            account, owner, source))
    if not runner.execute:
        runner.say("[dry-run] %s: node key signs a v2 link grant naming "
                   "%s; %s accepts it (owner %s -> %s)"
                   % (account, target, acceptor.describe(), source,
                      target))
        return False
    if not acceptor.ready():
        raise GuardError("the new owner must accept: pass --accept-key "
                         "or --session-env")
    ts = str(int(time.time() * 1000))
    nonce = secrets.token_urlsafe(24)
    pubkey, sig = sign_grant(ssh, node, account, target, ts, nonce)
    if pubkey != row.get("pubkey"):
        raise GuardError("%s: the node's identity key does not match the "
                         "registered public key; refusing" % account)
    body = {"nodeName": account, "user": target, "ts": ts,
            "nonce": nonce, "sig": sig}
    body.update(acceptor.fields(account, target, sig))
    code, reply = post_grant(app_url, body, acceptor.session())
    if code != 200 or not reply.get("linked") or reply.get("user") != target:
        raise ToolError("%s: link-grant answered HTTP %d %s" % (
            account, code, reply.get("error") or ""))
    db.execute([db.audit_statement(
        target, "node.owner_transfer", "node", account,
        {"from": source, "to": target, "via": "link-grant"})])
    after = db.account(account) or {}
    if after.get("owner") != target:
        raise ToolError("%s: owner reads back as %r" % (
            account, after.get("owner")))
    runner.say("%s: owner %s -> %s (signed link grant)" % (
        account, source or "none", target))
    return True


def transfer_cores(db, ssh, runner, fleet, source, target, app_url,
                   acceptor):
    moved = 0
    for node, account in core_accounts(fleet):
        moved += link_core(db, ssh, runner, node, account, source, target,
                           app_url, acceptor)
    return moved


def fleet_owner(db, fleet, exclude=()):
    owners = set()
    for node, account in core_accounts(fleet):
        if node["name"] in exclude:
            continue
        owner = (db.account(account) or {}).get("owner") or ""
        if owner:
            owners.add(owner)
    return owners.pop() if len(owners) == 1 else ""


def key_pubkey(path):
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import (
        Ed25519PrivateKey)
    try:
        key = serialization.load_pem_private_key(
            Path(path).expanduser().read_bytes(), None)
    except (OSError, ValueError, TypeError):
        return ""
    if not isinstance(key, Ed25519PrivateKey):
        return ""
    return b64url(key.public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw))


def owner_acceptor(db, owner, key_path=None):
    path = Path(key_path) if key_path else common.layout().identity_key
    record = db.record("users", "user_bi", owner) or {}
    pubkey = record.get("pubkey") or ""
    if pubkey and key_pubkey(path) == pubkey:
        return Acceptor(key_path=str(path))
    return Acceptor()


def provision_core(db, ssh, runner, fleet, node, app_url, owner="",
                   acceptor=None):
    account = NODE_PREFIX + node["name"]
    register_core(db, ssh, runner, node, account, app_url)
    owner = owner or fleet_owner(db, fleet, exclude=(node["name"],))
    if not owner:
        runner.say("%s: the other cores share no single owner; link it with "
                   "transfer --from '' --to <owner>" % account)
        return False
    current = (db.account(account) or {}).get("owner") or ""
    if current == owner:
        runner.say("%s: already owned by %s" % (account, owner))
        return False
    if current:
        runner.say("%s: owned by %s, not %s; left as it is" % (
            account, current, owner))
        return False
    acceptor = acceptor or owner_acceptor(db, owner)
    if not acceptor.ready():
        runner.say("%s: no owner yet, and no key of %s on this desktop. Link "
                   "it: next_node_owner.py transfer --from '' --to %s "
                   "--accept-key <PEM> --execute" % (account, owner, owner))
        return False
    if not runner.execute:
        runner.say("[dry-run] %s: node key signs a v2 link grant naming %s; "
                   "%s accepts it" % (account, owner, acceptor.describe()))
        return False
    return link_core(db, ssh, runner, node, account, "", owner, app_url,
                     acceptor)


def transfer_runners(db, runner, fleet, source, target):
    moved = 0
    for _node, account in runner_accounts(fleet):
        row = db.runner_row(account)
        if not row:
            runner.say("%s: no runner_nodes row; nothing to transfer" % account)
            continue
        try:
            owners = json.loads(row.get("allowed_owners") or "[]")
        except ValueError:
            owners = []
        if target not in owners:
            raise GuardError("%s: %s is not in allowed_owners %s; the runner "
                             "would not take its jobs" % (account, target,
                                                          owners))
        if row["registered_by"] == target:
            runner.say("%s: already registered by %s" % (account, target))
            continue
        if row["registered_by"] != source:
            raise GuardError("%s: registered_by is %r, not %r; refusing" % (
                account, row["registered_by"], source))
        if row["scope"] == "pool" and not db.is_admin(target):
            raise GuardError("%s: a pool runner's registrant must be an "
                             "administrator; %s is not" % (account, target))
        if row["scope"] == "personal" and owners != [target]:
            raise GuardError("%s: a personal runner serves only its "
                             "registrant" % account)
        if not runner.execute:
            runner.say("[dry-run] %s: registered_by %s -> %s" % (
                account, source, target))
            continue
        now = int(time.time() * 1000)
        results = db.execute([
            ["UPDATE runner_nodes SET registered_by=?, updated_at=? WHERE "
             "name=? AND registered_by=?", target, now, account, source],
            db.audit_statement(target, "runner.owner_transfer", "runner",
                               account, {"from": source, "to": target,
                                         "via": "next_node_owner"})])
        if int((results[0] or {}).get("rows_affected") or 0) != 1:
            raise ToolError("%s: the registrant changed underneath; nothing "
                            "written" % account)
        runner.say("%s: registered_by %s -> %s" % (account, source, target))
        moved += 1
    return moved


def transfer(db, ssh, runner, fleet, source, target, app_url,
             acceptor=None):
    acceptor = acceptor or Acceptor()
    user = db.record("users", "user_bi", target)
    if not user or user.get("kind", "user") != "user" or user.get(
            "status") != "active":
        raise GuardError("%s is not an active user account" % target)
    if source == target:
        raise GuardError("source and target are the same account")
    cores = transfer_cores(db, ssh, runner, fleet, source, target, app_url,
                           acceptor)
    runners = transfer_runners(db, runner, fleet, source, target)
    if runner.execute:
        left = [n for n in owned_nodes(db.account(source))
                if n.startswith(NODE_PREFIX)]
        if left:
            raise ToolError("%s still lists %s" % (source, ", ".join(left)))
    runner.say("cores moved: %d, runners moved: %d" % (cores, runners))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Fleet node ownership (docs/NODE_OWNERSHIP.md)")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("status")
    reg = sub.add_parser("register")
    reg.add_argument("names", nargs="*", help="core names (default: every core)")
    reg.add_argument("--app-url", default=APP_URL)
    reg.add_argument("--owner", default="",
                     help="link to this account (default: the other cores' owner)")
    reg.add_argument("--accept-key", default="",
                     help="the owner's Ed25519 PEM (default: this desktop's "
                          "identity key when it is the owner's)")
    common.add_execute_flags(reg)
    p = sub.add_parser("transfer")
    p.add_argument("--from", dest="source", required=True)
    p.add_argument("--to", dest="target", required=True)
    p.add_argument("--app-url", default=APP_URL)
    accept = p.add_mutually_exclusive_group()
    accept.add_argument("--accept-key", default="",
                        help="the target account's Ed25519 PEM")
    accept.add_argument("--session-env", default="",
                        help="env var holding the target account's session")
    common.add_execute_flags(p)
    args = parser.parse_args(argv)
    runner = common.runner_from_args(args) if hasattr(args, "execute") else \
        common.Runner(execute=False)
    fleet = common.load_fleet()
    ssh = common.Ssh()
    db = Db(fleet, ssh, keyset())
    try:
        if args.command == "status":
            return status(db, runner, fleet)
        if args.command == "register":
            return register(db, ssh, runner, fleet, args.app_url,
                            [n.strip().lower() for n in args.names],
                            args.owner.strip().lower(),
                            Acceptor(args.accept_key) if args.accept_key
                            else None)
        return transfer(db, ssh, runner, fleet, args.source.strip().lower(),
                        args.target.strip().lower(), args.app_url,
                        Acceptor(args.accept_key, args.session_env))
    except (ToolError, subprocess.TimeoutExpired) as error:
        print("error: %s" % error, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
