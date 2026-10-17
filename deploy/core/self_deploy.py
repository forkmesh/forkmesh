#!/usr/bin/env python3
import argparse
import base64
import fcntl
import fnmatch
import hashlib
import hmac
import json
import os
import re
import secrets
import shutil
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import forkmesh_safe_delete  # noqa: E402

PREFIX = "forkmesh-deploy-approval-v1"
CONFIG = "/etc/forkmesh-next/self-deploy.json"
CURLRC = "/etc/forkmesh-next/rqlite.curlrc"
RQLITE = "http://127.0.0.1:4001"
OPT = "/opt/forkmesh-next"
RELEASES = OPT + "/app/releases"
CURRENT = OPT + "/app/current"
IDS = OPT + "/bin/fmnext-ids"
STATE = "/var/lib/fmnext-self-deploy"
TICK_SECONDS = 15
MAX_APPROVAL_AGE_MS = 2 * 60 * 60 * 1000
STALL_MS = 45 * 60 * 1000
LIVE_MS = 2 * 60 * 1000
NOT_LIVE = ("leaving", "quarantined")
WAIT_FOR_COMMIT_MS = 15 * 60 * 1000
BUILD_TIMEOUT = 2400
STEP_TIMEOUT = 900
KEY_RE = re.compile(r"^[A-Za-z0-9_-]{43}$")
OID_RE = re.compile(r"^(?:[0-9a-f]{40}|[0-9a-f]{64})$")
NODE_RE = re.compile(r"^[a-z][a-z0-9-]{0,30}$")
ADMIN_KIND = "admin"
UPSTREAM_KEY = "core_upstream"
DONE = ("deployed", "failed", "skipped")
TERMINAL = ("deployed", "failed", "skipped", "superseded", "expired")

RELEASE_PREFIXES = ("app/", "office/", "world/", "www/public/", "deploy/",
                    "marketplace/packages/")
RELEASE_FILES = ("marketplace/catalog.json", "marketplace/manifest.schema.json",
                 "www/worker.js", "www/wrangler.production.toml",
                 "marketplace/tools/marketplace_lib.py", "tools/rqlite_migrate.py")
RELEASE_GLOBS = ("tools/next_*.py",)
ROLL_PREFIXES = ("server/", "deploy/core/")
ROLL_FILES = ("tools/mirror_gateway.py",)
DAEMON_TOOLS = (
    "tools/mirror_gateway.py", "app/src/mirror_replica.py",
    "app/src/public_snapshots.py", "app/src/portable_sessions.py",
    "tools/direct_https_bootstrap.py", "tools/node_operator_policy.py",
    "tools/secret_gate.py",
)
DAEMON_BIN = "/usr/local/bin/forkmesh-mirror-node"
DAEMON_TOOLS_DIR = "/usr/local/share/forkmesh/tools"
DAEMON_CONFIG = "/etc/forkmesh/mirror-node.json"
DAEMON_UNIT = "forkmesh-mirror-node.service"
ACTIVATION_LOCK = "/run/fmnext-activate.lock"
GO = "/usr/bin/go"
DAEMON_HEALTH_MS = 90 * 1000
DAEMON_STEADY_MS = 20 * 1000
DESKTOP_ROLL_NOTE = "deploy/core changes need a roll from the desktop"
NO_GO_NOTE = ("daemon or gateway changes need a roll from the desktop "
              "(no Go toolchain on this core)")
RUNNER_NOTE = "daemon rolled on the cores; runners wait for the desktop"
VERSION_RE = re.compile(r"project\s*\(\s*ForkMesh\s+VERSION\s+([0-9][0-9.]*)")
TEST_DIRS = frozenset({"tests", "tests_rqlite", "tests_cluster", "browser_tests"})


class Problem(Exception):
    pass


class Wait(Exception):
    pass


def b64url_decode(value):
    return base64.urlsafe_b64decode(value + "=" * (-len(value) % 4))


def approval_message(row):
    return "\n".join((
        PREFIX, str(row["kind"]), str(row["owner"]).lower(), str(row["repo"]),
        str(row["target"]), str(row.get("expected_base") or ""),
        str(row.get("expected_head") or ""), str(row["request_id"]),
        str(int(row["signed_at"])),
    )).encode("utf-8")


def signature_valid(row, keys):
    from cryptography.exceptions import InvalidSignature
    from cryptography.hazmat.primitives.asymmetric.ed25519 import (
        Ed25519PublicKey)
    key = str(row.get("public_key") or "")
    if key not in keys or not KEY_RE.fullmatch(key):
        return False
    try:
        Ed25519PublicKey.from_public_bytes(b64url_decode(key)).verify(
            b64url_decode(str(row.get("signature") or "")),
            approval_message(row))
        return True
    except (InvalidSignature, ValueError):
        return False


def is_test_path(path):
    parts = path.split("/")
    return any(part in TEST_DIRS for part in parts[:-1]) or path.endswith("_test.go")


def release_path(path):
    return not is_test_path(path) and (
        path.startswith(RELEASE_PREFIXES) or path in RELEASE_FILES
        or any(fnmatch.fnmatchcase(path, glob) for glob in RELEASE_GLOBS))


def roll_path(path):
    return (not is_test_path(path) and not path.endswith(".md")
            and (path.startswith(ROLL_PREFIXES) or path in ROLL_FILES))


def daemon_path(path):
    return (not is_test_path(path) and not path.endswith(".md")
            and (path.startswith("server/") or path in DAEMON_TOOLS))


def ids_manifest(node, key, rev, now_ms, minutes=30):
    return ids_window(
        node, key, "self-deploy %s" % rev, now_ms, minutes,
        files={CURRENT: "link:%s/*" % RELEASES},
        rescan=[RELEASES, "/etc/forkmesh-next/caddy/app-routes.caddy",
                "/etc/forkmesh-next/caddy/world-routes.caddy"],
        allow=["listeners"])


def daemon_ids_manifest(node, key, rev, now_ms, minutes=30):
    return ids_window(
        node, key, "self-deploy daemon %s" % rev, now_ms, minutes, files={},
        rescan=[DAEMON_BIN, "/usr/local/share/forkmesh", "/etc/forkmesh"],
        allow=["listeners", "processes"])


def ids_window(node, key, reason, now_ms, minutes, files, rescan, allow):
    manifest = {
        "v": 1, "node": node, "nonce": secrets.token_hex(16),
        "issued_ms": int(now_ms), "expires_ms": int(now_ms) + minutes * 60_000,
        "reason": reason, "files": files, "rescan": sorted(rescan),
        "allow": sorted(allow),
    }
    manifest["mac"] = hmac.new(key.encode("utf-8"), json.dumps(
        manifest, sort_keys=True, separators=(",", ":")).encode("utf-8"),
        hashlib.sha256).hexdigest()
    return manifest


class Rqlite:
    def __init__(self, url=RQLITE, curlrc=CURLRC):
        self.url = url
        text = Path(curlrc).read_text("utf-8")
        match = re.search(r'user\s*=\s*"([^"]*)"', text)
        if not match:
            raise Problem("no credentials in %s" % curlrc)
        self.auth = "Basic " + base64.b64encode(match.group(1).encode()).decode()

    def _post(self, path, statements):
        request = urllib.request.Request(
            self.url + path, data=json.dumps(statements).encode("utf-8"),
            headers={"Content-Type": "application/json",
                     "Authorization": self.auth}, method="POST")
        with urllib.request.urlopen(request, timeout=20) as response:
            body = json.loads(response.read().decode("utf-8"))
        results = body.get("results") or [{}]
        if results[0].get("error"):
            raise Problem("rqlite: %s" % results[0]["error"])
        return results[0]

    def query(self, sql, *args):
        return self._post("/db/query?level=strong&associative",
                          [[sql, *args]]).get("rows") or []

    def execute(self, sql, *args):
        return int(self._post("/db/execute", [[sql, *args]]).get(
            "rows_affected") or 0)


class Agent:
    def __init__(self, config, db, run=subprocess.run, now=None, log=print):
        self.config = config
        self.db = db
        self.run = run
        self.now = now or (lambda: int(time.time() * 1000))
        self.log = log
        self.node = config["node"]
        self.order = list(config["order"])
        self.owner, self.repo = config["repository"].split("/", 1)
        self.git_dir = config["gitDir"]
        self.keys = [k for k in config.get("approverKeys") or [] if KEY_RE.fullmatch(k)]
        self.state = Path(config.get("stateDir") or STATE)
        self.go = str(config.get("go") or GO)
        self.roll_daemon_enabled = bool(config.get("rollDaemon", True))
        self.admin_updates = bool(config.get("adminUpdates"))
        self.upstream = ""
        self.sleep = time.sleep

    def git(self, *args, check=True):
        completed = self.run(
            ["git", "-c", "safe.directory=*", "--git-dir=" + self.git_dir, *args],
            capture_output=True, text=True, timeout=120,
            env={"PATH": "/usr/bin:/bin", "GIT_CONFIG_NOSYSTEM": "1",
                 "GIT_CONFIG_GLOBAL": "/dev/null", "LANG": "C"})
        if check and completed.returncode != 0:
            raise Problem("git %s: %s" % (args[0], (completed.stderr or "").strip()[:200]))
        return completed

    def has_commit(self, oid):
        return self.git("cat-file", "-e", oid + "^{commit}", check=False).returncode == 0

    def is_ancestor(self, older, newer):
        return self.git("merge-base", "--is-ancestor", older, newer,
                        check=False).returncode == 0

    def current_rev(self):
        try:
            return (Path(CURRENT) / "BUILD_REV").read_text("utf-8").strip()
        except OSError:
            return ""

    def set_node(self, approval_id, status, message=""):
        self.db.execute(
            "INSERT INTO deploy_approval_nodes (approval_id,node,status,message,"
            "updated_at) VALUES (?,?,?,?,?) ON CONFLICT(approval_id,node) DO "
            "UPDATE SET status=excluded.status,message=excluded.message,"
            "updated_at=excluded.updated_at",
            approval_id, self.node, status, str(message)[:500], self.now())

    def set_approval(self, approval_id, status, message="", only_from=None):
        sql = ("UPDATE deploy_approvals SET status=?,message=?,updated_at=? "
               "WHERE id=?")
        args = [status, str(message)[:500], self.now(), approval_id]
        if only_from:
            sql += " AND status IN (%s)" % ",".join("?" * len(only_from))
            args += list(only_from)
        return self.db.execute(sql, *args)

    def check_admin(self, row):
        if not self.admin_updates:
            raise Problem("updates from the admin console are off on this core")
        if self.now() - int(row["created_at"]) > MAX_APPROVAL_AGE_MS and \
                row["status"] == "pending":
            raise Problem("the update request is older than two hours")
        if not row.get("base_after"):
            main = self.git("rev-parse", "refs/heads/main").stdout.strip()
            if not OID_RE.fullmatch(main):
                raise Problem("main is not readable on this core")
            self.db.execute(
                "UPDATE deploy_approvals SET base_before=?,head=?,base_after=?,"
                "updated_at=? WHERE id=? AND base_after=''",
                main, main, main, self.now(), row["id"])
            pinned = self.db.query(
                "SELECT base_before,head,base_after FROM deploy_approvals "
                "WHERE id=?", row["id"])
            if pinned:
                row.update(pinned[0])
        target = str(row.get("base_after") or "")
        if not OID_RE.fullmatch(target):
            raise Problem("the update target is malformed")
        if not self.has_commit(target):
            if self.now() - int(row["created_at"]) > WAIT_FOR_COMMIT_MS:
                raise Problem("the requested commit never reached this core")
            raise Wait()
        main = self.git("rev-parse", "refs/heads/main").stdout.strip()
        if not self.is_ancestor(target, main):
            raise Problem("the requested commit is not on main")

    def check(self, row):
        if row.get("kind") == ADMIN_KIND:
            return self.check_admin(row)
        if not signature_valid(row, self.keys):
            raise Problem("the approval is not signed by an enrolled phone key")
        if self.now() - int(row["created_at"]) > MAX_APPROVAL_AGE_MS and \
                row["status"] == "pending":
            raise Problem("the approval is older than two hours")
        for field in ("base_before", "head", "base_after"):
            if not OID_RE.fullmatch(str(row.get(field) or "")):
                raise Problem("the merge result is malformed")
        if row["kind"] == "pull" and (
                row["base_before"] != row["expected_base"]
                or row["head"] != row["expected_head"]):
            raise Problem("the merge does not match what the phone approved")
        target = row["base_after"]
        if not self.has_commit(target):
            if self.now() - int(row["created_at"]) > WAIT_FOR_COMMIT_MS:
                raise Problem("the merged commit never reached this core")
            raise Wait()
        if target != row["head"]:
            parent = self.git("rev-parse", target + "^1").stdout.strip()
            if parent != row["base_before"]:
                raise Problem("the deployed commit is not the approved merge")
        main = self.git("rev-parse", "refs/heads/main").stdout.strip()
        if not self.is_ancestor(target, main):
            raise Problem("the merged commit is not on main")

    def changed(self, target):
        current = self.current_rev()
        if not current:
            return None
        resolved = self.git("rev-parse", "--verify", "-q", current + "^{commit}",
                            check=False)
        base = resolved.stdout.strip()
        if resolved.returncode != 0 or not OID_RE.fullmatch(base):
            return None
        if not self.is_ancestor(base, target):
            raise Problem("this core runs %s, which is not behind the merge; "
                          "refusing to roll back" % current)
        names = self.git("diff", "--name-only", "--no-renames", base, target).stdout
        return [p for p in names.splitlines() if p]

    def pick(self):
        rows = self.db.query(
            "SELECT * FROM deploy_approvals WHERE owner=? AND repo=? AND status "
            "IN ('pending','deploying') ORDER BY id", self.owner, self.repo)
        deploying = [r for r in rows if r["status"] == "deploying"]
        if deploying:
            return deploying[0], []
        if not rows:
            return None, []
        return rows[-1], rows[:-1]

    def publish_upstream(self):
        resolved = self.git("rev-parse", "--verify", "-q", "refs/heads/main",
                            check=False)
        main = resolved.stdout.strip()
        if resolved.returncode != 0 or not OID_RE.fullmatch(main) or \
                main == self.upstream:
            return False
        fields = self.git("log", "-1", "--format=%ct%n%s", main).stdout
        stamp, _, subject = fields.partition("\n")
        cmake = self.git("show", main + ":desktop/CMakeLists.txt", check=False)
        match = VERSION_RE.search(cmake.stdout or "")
        doc = json.dumps({
            "commit": main, "time": int(stamp.strip() or 0),
            "subject": subject.strip()[:500],
            "version": match.group(1) if match else "",
            "node": self.node, "checkedAt": self.now(),
        }, sort_keys=True)
        self.db.execute(
            "INSERT INTO instance_settings (k, doc, updated_at) VALUES (?,?,?) "
            "ON CONFLICT(k) DO UPDATE SET doc=excluded.doc, "
            "updated_at=excluded.updated_at", UPSTREAM_KEY, doc, self.now())
        self.upstream = main
        return True

    def tick(self):
        try:
            self.publish_upstream()
        except Exception:
            pass
        row, older = self.pick()
        if row is None:
            return "idle"
        try:
            self.check(row)
        except Wait:
            return "waiting for the commit"
        except Problem as problem:
            if row["status"] == "pending":
                self.set_approval(row["id"], "skipped", str(problem), ("pending",))
            return "skipped: %s" % problem
        for stale in older:
            if self.is_ancestor(stale["base_after"], row["base_after"]):
                self.set_approval(stale["id"], "superseded",
                                  "covered by a newer approved merge",
                                  ("pending",))
        return self.advance(row)

    def nodes(self, approval_id):
        return {r["node"]: r for r in self.db.query(
            "SELECT * FROM deploy_approval_nodes WHERE approval_id=?", approval_id)}

    def sequence(self, row):
        key = str(row["id"])
        return sorted(self.order, key=lambda name: (hashlib.sha256(
            ("%s:%s" % (key, name)).encode("utf-8")).hexdigest(), name))

    def live_cores(self):
        try:
            rows = self.db.query(
                "SELECT node_id,status FROM cluster_members WHERE heartbeat_ms>=?",
                self.now() - LIVE_MS)
        except Exception:
            return set(self.order)
        if not rows:
            return set(self.order)
        live = {str(r.get("node_id")) for r in rows if r.get("status") not in NOT_LIVE}
        return live | {self.node}

    def required(self, row, nodes):
        live = self.live_cores()
        return [n for n in self.sequence(row) if n in live or n in nodes]

    def finalize(self, row, nodes):
        required = self.required(row, nodes)
        statuses = [nodes.get(n, {}).get("status") for n in required]
        if any(s == "failed" for s in statuses):
            failed = [n for n in required if nodes.get(n, {}).get("status") == "failed"]
            message = nodes[failed[0]].get("message") or "a core failed"
            self.set_approval(row["id"], "failed", "%s: %s" % (failed[0], message),
                              ("pending", "deploying"))
            return "failed"
        if all(s in ("deployed", "skipped") for s in statuses):
            notes = [nodes[n].get("message") or "" for n in required
                     if nodes[n]["status"] == "skipped"]
            notes += [nodes[n].get("message") or "" for n in required
                      if nodes[n]["status"] == "deployed"]
            absent = [n for n in self.order if n not in required]
            if absent:
                notes.insert(0, "not live, left for a roll: %s" % ", ".join(absent))
            note = next((n for n in notes if n), "")
            self.set_approval(row["id"], "deployed", note or "",
                              ("pending", "deploying"))
            return "deployed"
        now = self.now()
        for name in required:
            entry = nodes.get(name)
            if entry and entry["status"] not in DONE and \
                    now - int(entry["updated_at"]) > STALL_MS:
                self.set_approval(row["id"], "failed", "%s stalled at %s" % (
                    name, entry["status"]), ("pending", "deploying"))
                return "failed"
        return "in progress"

    def advance(self, row):
        self.set_approval(row["id"], "deploying", "", ("pending",))
        nodes = self.nodes(row["id"])
        mine = nodes.get(self.node)
        if mine and mine["status"] in DONE:
            return self.finalize(row, nodes)
        if any(n["status"] == "failed" for n in nodes.values()):
            self.set_node(row["id"], "skipped", "another core failed first")
            return self.finalize(row, self.nodes(row["id"]))
        target = row["base_after"]
        rev = target[:12]
        if mine is None or mine["status"] in ("queued", "building"):
            try:
                release, daemon, note = self.plan(target)
            except Problem as problem:
                self.set_node(row["id"], "failed", str(problem))
                return self.finalize(row, self.nodes(row["id"]))
            if not release and not daemon:
                if self.current_rev() == rev:
                    self.set_node(row["id"], "deployed",
                                  note or "already running %s" % rev)
                else:
                    self.set_node(row["id"], "skipped",
                                  note or "nothing served changed")
                return self.finalize(row, self.nodes(row["id"]))
            self.set_node(row["id"], "building")
            try:
                self.build_and_upload(target, rev, release=release, daemon=daemon)
            except Problem as problem:
                self.set_node(row["id"], "failed", "build: %s" % problem)
                return self.finalize(row, self.nodes(row["id"]))
            self.set_node(row["id"], "ready", note)
            nodes = self.nodes(row["id"])
            mine = nodes[self.node]
        if mine["status"] == "ready":
            sequence = self.sequence(row)
            before = sequence[:sequence.index(self.node)]
            live = self.live_cores()
            for name in before:
                if nodes.get(name, {}).get("status") not in ("deployed", "skipped") \
                        and (name in live or name in nodes):
                    return "waiting for %s" % name
            first = not any(nodes.get(name, {}).get("status") == "deployed"
                            for name in before)
            note = mine.get("message") or ""
            self.set_node(row["id"], "activating", note)
            try:
                release, daemon, _ = self.plan(target)
                if release:
                    if first:
                        self.migrate(rev)
                    self.activate(rev)
                if daemon:
                    self.roll_daemon(target, rev)
                    note = "; ".join(n for n in (note, RUNNER_NOTE) if n)
            except Problem as problem:
                self.set_node(row["id"], "failed", str(problem))
                return self.finalize(row, self.nodes(row["id"]))
            self.set_node(row["id"], "deployed", note)
        elif mine["status"] == "activating":
            try:
                release, daemon, _ = self.plan(target)
            except Problem:
                release, daemon = True, True
            if not release and not daemon:
                self.set_node(row["id"], "deployed", mine.get("message") or "")
            elif self.now() - int(mine["updated_at"]) > STALL_MS:
                self.set_node(row["id"], "failed", "activation did not finish")
        return self.finalize(row, self.nodes(row["id"]))

    def daemon_rollable(self):
        return self.roll_daemon_enabled and Path(self.go).is_file()

    def daemon_rev(self):
        try:
            value = (self.state / "daemon-rev").read_text("utf-8").strip()
        except OSError:
            return ""
        return value if OID_RE.fullmatch(value) else ""

    def set_daemon_rev(self, oid):
        self.state.mkdir(parents=True, exist_ok=True)
        path = self.state / "daemon-rev"
        path.with_suffix(".new").write_text(oid + "\n", "utf-8")
        os.replace(path.with_suffix(".new"), path)

    def daemon_base(self):
        base = self.daemon_rev()
        if base and self.has_commit(base):
            return base
        current = self.current_rev()
        if not current:
            return ""
        resolved = self.git("rev-parse", "--verify", "-q", current + "^{commit}",
                            check=False)
        base = resolved.stdout.strip()
        if resolved.returncode != 0 or not OID_RE.fullmatch(base):
            return ""
        self.set_daemon_rev(base)
        return base

    def daemon_paths(self, target):
        base = self.daemon_base()
        if not base or base == target or not self.is_ancestor(base, target):
            return []
        names = self.git("diff", "--name-only", "--no-renames", base, target).stdout
        return [p for p in names.splitlines() if daemon_path(p)]

    def plan(self, target):
        paths = self.changed(target)
        rev = target[:12]
        release = self.current_rev() != rev and (
            paths is None or any(release_path(p) for p in paths))
        daemon = False
        notes = []
        if any(roll_path(p) and not daemon_path(p) for p in paths or []):
            notes.append(DESKTOP_ROLL_NOTE)
        if self.daemon_rollable():
            daemon = bool(self.daemon_paths(target))
        elif any(daemon_path(p) and roll_path(p) for p in paths or []):
            notes.append(NO_GO_NOTE)
        return release, daemon, "; ".join(notes)

    def build_dir(self, rev):
        return self.state / "builds" / rev

    def build_and_upload(self, target, rev, release=True, daemon=False):
        work = self.checkout(target, rev)
        if daemon:
            self.build_daemon(work)
        if release:
            self.build_release(work, rev)

    def checkout(self, target, rev):
        builds = self.state / "builds"
        builds.mkdir(parents=True, exist_ok=True)
        os.chmod(builds, 0o755)
        work = self.build_dir(rev)
        for old in builds.iterdir():
            forkmesh_safe_delete.remove_tree(old, builds)
        work.mkdir(mode=0o755)
        clone = work / "src"
        out = work / "out"
        self.check_run(["git", "-c", "safe.directory=*", "clone", "--quiet",
                        "--no-hardlinks", "--no-checkout", self.git_dir, str(clone)],
                       "clone")
        self.check_run(["git", "-c", "safe.directory=*", "-C", str(clone),
                        "checkout", "--quiet", "--detach", target], "checkout")
        out.mkdir()
        self.prepare_config(work / "config")
        self.check_run(["chown", "-R", "%s:%s" % (self.build_user(), self.build_user()),
                        str(work)], "chown")
        return work

    def build_user(self):
        return self.config.get("buildUser") or "fmnext-build"

    def build_daemon(self, work):
        clone = work / "src"
        out = work / "daemon"
        user = self.build_user()
        command = [
            "systemd-run", "--scope", "--quiet", "--collect",
            "--uid=" + user, "--gid=" + user,
            "-p", "MemoryMax=" + str(self.config.get("daemonBuildMemoryMax") or "700M"),
            "-p", "MemorySwapMax=1G",
            "-p", "CPUQuota=" + str(self.config.get("buildCPUQuota") or "60%"),
            "--", "/usr/bin/env", "-i",
            "PATH=/usr/local/bin:/usr/bin:/bin", "HOME=" + str(work),
            "LANG=C.UTF-8", "GOCACHE=" + str(work / "gocache"),
            "GOPATH=" + str(work / "gopath"), "GOTOOLCHAIN=local",
            "GOPROXY=off", "GOFLAGS=-mod=readonly", "CGO_ENABLED=0",
            "GOOS=linux", "GOARCH=amd64", "GOMAXPROCS=1", "GOGC=25",
            self.go, "build", "-C", str(clone / "server"), "-trimpath",
            "-buildvcs=false", "-o", str(out / "forkmesh-mirror-node"),
            "./cmd/forkmesh-mirror-node",
        ]
        self.check_run(["install", "-d", "-o", user, "-g", user, "-m", "0755",
                        str(out)], "daemon dir")
        self.check_run(command, "daemon build", timeout=BUILD_TIMEOUT)
        tools = out / "tools"
        tools.mkdir(exist_ok=True)
        for tool in DAEMON_TOOLS:
            source = clone / tool
            if not source.is_file():
                raise Problem("daemon tool %s is missing from the merge" % tool)
            shutil.copyfile(source, tools / source.name)
        match = VERSION_RE.search((clone / "desktop" / "CMakeLists.txt").read_text("utf-8"))
        if not match:
            raise Problem("desktop/CMakeLists.txt has no release version")
        (out / "VERSION").write_text(match.group(1), "utf-8")

    def build_release(self, work, rev):
        clone = work / "src"
        out = work / "out"
        user = self.build_user()
        command = [
            "systemd-run", "--scope", "--quiet", "--collect",
            "--uid=" + user, "--gid=" + user,
            "-p", "MemoryMax=" + str(self.config.get("buildMemoryMax") or "450M"),
            "-p", "MemorySwapMax=512M",
            "-p", "CPUQuota=" + str(self.config.get("buildCPUQuota") or "60%"),
            "--", "/usr/bin/env", "-i",
            "PATH=/usr/local/bin:/usr/bin:/bin", "HOME=" + str(work),
            "LANG=C.UTF-8", "FMNEXT_CONFIG_DIR=" + str(work / "config"),
            "FMNEXT_DATA_DIR=" + str(work / "data"),
            "FMNEXT_DIST_DIR=" + str(out),
            "/usr/bin/python3", str(Path(__file__).resolve()), "build",
            "--clone", str(clone), "--rev", rev, "--out", str(out),
        ]
        self.check_run(command, "build", timeout=BUILD_TIMEOUT)
        result = json.loads((out / "result.json").read_text("utf-8"))
        tarball = out / ("fmnext-%s.tar.gz" % rev)
        wheels = Path(result["wheelhouse"])
        next_deploy = load_next_deploy(clone)
        key = wheels.name
        wheelhouses = Path(OPT) / "app" / "wheelhouses"
        wheelhouses.mkdir(mode=0o755, exist_ok=True)
        shutil.copy2(wheels.parent / (key + ".tar"),
                     wheelhouses / (".incoming-%s.tar" % key))
        self.check_run(["bash", "-c", next_deploy.wheelhouse_script(key)], "wheelhouse")
        shutil.copy2(tarball, Path(RELEASES) / (".incoming-%s.tar.gz" % rev))
        self.check_run(["bash", "-c", next_deploy.upload_script(rev, result["sha256"])],
                       "unpack", timeout=STEP_TIMEOUT)

    def prepare_config(self, config_dir):
        config_dir.mkdir()
        deploy = Path(CURRENT) / "deploy"
        for source, name in (("fleet-state.public.json", "fleet-state.json"),
                             ("fleet-instances.public.json", "fleet-instances.json")):
            path = deploy / source
            if path.is_file():
                shutil.copy2(path, config_dir / name)

    def migrate(self, rev):
        script = self.build_dir(rev) / "src" / "tools" / "rqlite_migrate.py"
        if not script.is_file():
            raise Problem("the build checkout with the migrations is gone")
        user, _, password = self.credentials()
        self.check_run(["/usr/bin/python3", str(script), "--url", RQLITE],
                       "migrate", timeout=STEP_TIMEOUT,
                       env={"PATH": "/usr/bin:/bin", "LANG": "C.UTF-8",
                            "RQLITE_USERNAME": user, "RQLITE_PASSWORD": password})

    def credentials(self):
        text = Path(CURLRC).read_text("utf-8")
        match = re.search(r'user\s*=\s*"([^"]*)"', text)
        if not match:
            raise Problem("no rqlite credentials")
        return match.group(1).partition(":")

    def ids_key(self):
        directory = os.environ.get("CREDENTIALS_DIRECTORY", "")
        path = Path(directory) / "FMNEXT_IDS_KEY" if directory else None
        if path and path.is_file():
            return path.read_text("utf-8").strip()
        return ""

    def activate(self, rev):
        key = self.ids_key()
        if key:
            self.open_ids_window(ids_manifest(self.node, key, rev, self.now()))
        next_deploy = load_next_deploy(Path(RELEASES) / rev)
        script = next_deploy.activate_script(rev, self.config["host"], vault=False)
        self.check_run(["bash", "-c", script], "activate", timeout=STEP_TIMEOUT)

    def open_ids_window(self, manifest):
        if not Path(IDS).exists():
            return
        completed = self.run([IDS, "expect"], input=json.dumps(manifest),
                             capture_output=True, text=True, timeout=60)
        try:
            reply = json.loads(completed.stdout or "{}")
        except ValueError:
            reply = {}
        if not reply.get("ok") and reply.get("error") != "not_running":
            raise Problem("the IDS refused the change window (%s)" % (
                reply.get("error") or "no reply"))

    def daemon_state(self):
        completed = self.run(["systemctl", "show", "-p", "ActiveState",
                              "-p", "NRestarts", DAEMON_UNIT],
                             capture_output=True, text=True, timeout=30)
        values = dict(line.split("=", 1) for line in
                      (completed.stdout or "").splitlines() if "=" in line)
        return values.get("ActiveState", ""), values.get("NRestarts", "")

    def daemon_healthy(self):
        deadline = self.now() + DAEMON_HEALTH_MS
        steady_since, restarts = None, None
        while True:
            state, count = self.daemon_state()
            if state == "active" and count == restarts and steady_since is not None:
                if self.now() - steady_since >= DAEMON_STEADY_MS:
                    return True, "active"
            elif state == "active":
                steady_since, restarts = self.now(), count
            else:
                steady_since, restarts = None, None
            if self.now() >= deadline:
                return False, "%s %s, %s restarts" % (DAEMON_UNIT, state or "unknown",
                                                     count or "?")
            self.sleep(3)

    def roll_daemon(self, target, rev):
        stage = self.build_dir(rev) / "daemon"
        binary = stage / "forkmesh-mirror-node"
        if not binary.is_file():
            raise Problem("daemon: the build for %s is gone" % rev)
        key = self.ids_key()
        if key:
            self.open_ids_window(daemon_ids_manifest(self.node, key, rev, self.now()))
        with open(ACTIVATION_LOCK, "a") as lock:
            deadline = time.monotonic() + 900
            while True:
                try:
                    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    break
                except BlockingIOError:
                    if time.monotonic() >= deadline:
                        raise Problem("daemon: another activation holds the lock")
                    self.sleep(2)
            self.swap_daemon(stage, binary)
        self.set_daemon_rev(target)

    def swap_daemon(self, stage, binary):
        bin_path = Path(DAEMON_BIN)
        config = Path(DAEMON_CONFIG)
        tools_dir = Path(DAEMON_TOOLS_DIR)
        backup = self.state / "daemon-prev"
        backup.mkdir(parents=True, exist_ok=True)
        os.chmod(backup, 0o700)
        incoming = bin_path.with_name(bin_path.name + ".next")
        shutil.copyfile(binary, incoming)
        os.chmod(incoming, 0o755)
        candidate = config.with_name(config.name + ".selfroll")
        info = config.stat()
        document = json.loads(config.read_text("utf-8"))
        document["version"] = (stage / "VERSION").read_text("utf-8").strip()
        candidate.write_text(json.dumps(document, indent=2) + "\n", "utf-8")
        os.chmod(candidate, info.st_mode & 0o7777)
        os.chown(candidate, info.st_uid, info.st_gid)
        checked = self.run(["runuser", "-u", "forkmesh-node", "--", str(incoming),
                            "--config", str(candidate), "--check"],
                           capture_output=True, text=True, timeout=120)
        if checked.returncode != 0:
            os.unlink(incoming)
            os.unlink(candidate)
            tail = ((checked.stderr or "") + (checked.stdout or "")).strip()
            raise Problem("daemon: the new binary refused this core's config: %s"
                          % tail[-300:])
        saved = []
        if bin_path.is_file():
            shutil.copy2(bin_path, backup / bin_path.name)
            saved.append((backup / bin_path.name, bin_path))
        shutil.copy2(config, backup / config.name)
        saved.append((backup / config.name, config))
        tools_dir.mkdir(parents=True, exist_ok=True)
        added = []
        for tool in sorted((stage / "tools").iterdir()):
            live = tools_dir / tool.name
            if live.is_file():
                shutil.copy2(live, backup / ("tool-" + tool.name))
                saved.append((backup / ("tool-" + tool.name), live))
            else:
                added.append(live)
        for tool in sorted((stage / "tools").iterdir()):
            shutil.copyfile(tool, tools_dir / (tool.name + ".next"))
            os.chmod(tools_dir / (tool.name + ".next"), 0o644)
            os.replace(tools_dir / (tool.name + ".next"), tools_dir / tool.name)
        os.replace(candidate, config)
        os.replace(incoming, bin_path)
        self.run(["systemctl", "restart", DAEMON_UNIT], capture_output=True,
                 text=True, timeout=120)
        ok, note = self.daemon_healthy()
        if ok:
            return
        for source, live in saved:
            shutil.copy2(source, live.with_name(live.name + ".back"))
            os.replace(live.with_name(live.name + ".back"), live)
        for live in added:
            if live.exists():
                os.unlink(live)
        self.run(["systemctl", "restart", DAEMON_UNIT], capture_output=True,
                 text=True, timeout=120)
        raise Problem("daemon: unhealthy after the roll (%s); rolled back" % note)

    def check_run(self, command, what, timeout=STEP_TIMEOUT, env=None):
        try:
            completed = self.run(command, capture_output=True, text=True,
                                 timeout=timeout, env=env)
        except subprocess.TimeoutExpired:
            raise Problem("%s timed out" % what)
        if completed.returncode != 0:
            tail = ((completed.stderr or "") + (completed.stdout or "")).strip()
            raise Problem("%s failed: %s" % (what, tail[-300:]))
        return completed


TOOL_MODULES = ("next_deploy", "next_common", "forkmesh_paths", "render",
                "caddy_routes", "next_ids", "forkmesh_safe_delete")


def load_next_deploy(root):
    import importlib
    root = Path(root)
    saved = list(sys.path)
    for name in TOOL_MODULES:
        if name != "forkmesh_safe_delete":
            sys.modules.pop(name, None)
    sys.path[:0] = [str(root / "tools"), str(root / "deploy" / "core")]
    try:
        return importlib.import_module("next_deploy")
    finally:
        sys.path[:] = saved


def build_command(clone, rev, out):
    clone = Path(clone).resolve()
    out = Path(out).resolve()
    sys.path[:0] = [str(clone / "tools"), str(clone / "deploy" / "core")]
    import tempfile
    import next_common as common
    import next_deploy
    if next_deploy.REPO.resolve() != clone:
        raise SystemExit("next_deploy did not load from the clone")
    fleet = common.load_fleet()
    with tempfile.TemporaryDirectory(prefix="stage-", dir=out) as tmp:
        stage = Path(tmp) / "release"
        next_deploy.stage_release(rev, stage, fleet)
        tarball = out / ("fmnext-%s.tar.gz" % rev)
        digest = next_deploy.pack(stage, tarball)
    wheels = next_deploy.cached_wheelhouse()
    next_deploy.wheelhouse_tarball(wheels)
    (out / "result.json").write_text(json.dumps(
        {"sha256": digest, "wheelhouse": str(wheels)}))
    return 0


def serve(config_path):
    config = json.loads(Path(config_path).read_text("utf-8"))
    if not config.get("enabled") or not (
            config.get("approverKeys") or config.get("adminUpdates")):
        print("self-deploy: disabled, or no enrolled phone key and no admin "
              "updates; idle", flush=True)
        while True:
            time.sleep(3600)
    agent = Agent(config, Rqlite())
    last = ""
    while True:
        try:
            outcome = agent.tick()
        except Exception as error:
            outcome = "error: %s" % error
        if outcome != last:
            print("self-deploy: %s" % outcome, flush=True)
            last = outcome
        time.sleep(TICK_SECONDS)


def main(argv=None):
    parser = argparse.ArgumentParser(description="ForkMesh core self-deploy agent")
    sub = parser.add_subparsers(dest="command", required=True)
    run = sub.add_parser("serve")
    run.add_argument("--config", default=CONFIG)
    build = sub.add_parser("build")
    build.add_argument("--clone", required=True)
    build.add_argument("--rev", required=True)
    build.add_argument("--out", required=True)
    args = parser.parse_args(argv)
    if args.command == "build":
        return build_command(args.clone, args.rev, args.out)
    return serve(args.config)


if __name__ == "__main__":
    sys.exit(main())
