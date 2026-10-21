#!/usr/bin/env python3
from __future__ import annotations

import argparse
import base64
import fnmatch
import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import forkmesh_paths  # noqa: E402
import next_common as common  # noqa: E402
from next_common import GuardError, ToolError  # noqa: E402

WORKFLOW_PATH = ".forkmesh/deploy-nodes.yml"
REPOSITORY = ("forkmesh", "forkmesh")
MAIN_REF = "refs/heads/main"
RESULT_MARK = "::node-deploy-result::"
SINK_NAME = "node-deploy.log"
RUN_DIR_RE = re.compile(r"^forkmesh-run-(\d+)-(\d+)$")
SHA_RE = re.compile(r"^[0-9a-f]{40}$")
REV_RE = re.compile(r"^[0-9a-f]{7,40}$")
DEBOUNCE_SECONDS = 3
SETTLE_SECONDS = 2
MAX_WAIT_SECONDS = 600
DEPLOY_COOLDOWN_SECONDS = 300
POLL_SECONDS = 3
LOCK_WAIT_SECONDS = 3600
VERIFY_SECONDS = 120
EMAIL_AFTER_FAILURES = 2
PATH_DEPTH = 12
HISTORY_LIMIT = 4000
UNIT_NAME = "forkmesh-node-deploy.service"
INSTALLED_FILES = ("next_autodeploy.py", "next_common.py", "forkmesh_paths.py")
INSTALLED_DEPLOY_FILES = ("render.py", "caddy_routes.py")
ALERT_PATH = "/api/accounts/admin-operator-alert"
ALERT_PROOF = "forkmesh-admin-operator-alert-v1"

RELEASE_PREFIXES = ("app/", "office/", "world/", "www/public/", "deploy/", "marketplace/packages/")
RELEASE_FILES = ("marketplace/catalog.json", "marketplace/manifest.schema.json",
                 "www/worker.js", "www/wrangler.production.toml",
                 "marketplace/tools/marketplace_lib.py", "tools/rqlite_migrate.py")
RELEASE_GLOBS = ("tools/next_*.py",)
ROLL_PREFIXES = ("server/", "deploy/core/")
ROLL_FILES = ("tools/mirror_gateway.py",)
RUNNER_ROLL_PREFIXES = ("server/",)
RUNNER_ROLL_RETRY_SECONDS = 600
RUNNERS_BUSY_EXIT = 3
TEST_DIRS = frozenset({"tests", "tests_rqlite", "tests_cluster", "browser_tests"})

MUST_PASS = (
    "app/tests/host/test_host_e2e.py",
    "deploy/tests",
    "app/tests/host/test_host_shim_units.py",
    "app/tests/host/test_host_request_guard.py",
    "app/tests/host/test_apps_e2e.py",
    "app/tests/test_timer_pings.py",
    "app/tests/test_desktop_pings.py",
    "app/tests/test_operator_alerts.py",
    "app/tests/test_users_nodes_claim_link.py",
    "app/tests/test_account_totp_gate.py",
    "app/tests/test_login_key_binding.py",
    "app/tests/test_no_unguarded_destructive_deletes.py",
    "app/tests/test_forkbot.py::test_forkbot_does_not_rekey_when_the_owner_is_a_plain_node",
    "app/tests/test_forkbot.py::test_forkbot_agent_requests_ignore_the_claimed_chat_sender",
    "app/tests/test_forkbot.py::test_forkbot_starts_agent_on_most_recent_issue_for_owner",
    "app/tests/test_forkbot.py::test_forkbot_starts_agent_on_named_issue_for_admin",
    "app/tests/test_forkbot.py::test_forkbot_resolves_only_allowlisted_model_picks",
    "app/tests/test_forkbot.py::test_ai_workspace_name_rejects_unsigned_and_falls_back_on_unusable_results",
    "app/tests/test_forkbot.py::test_ai_ask_refuses_unsigned_stale_and_unverified_callers",
    "app/tests/test_forkbot.py::test_ai_agent_caps_the_output_tokens_a_client_may_request",
    "app/tests/test_forkbot.py::test_forkbot_denies_agent_start_for_non_owner",
    "app/tests/test_forkbot.py::test_forkbot_rejects_agent_start_on_unknown_issue",
)
GO_TEST_MODULES = ("server",)
COMPARE_SUITE = ("app/tests/host",)
TEST_WORKERS = 12
TEST_TIMEOUT = 1800
BUILD_TIMEOUT = 3600
DEPLOY_TIMEOUT = 5400
ROLL_TIMEOUT = 3600


def is_test_path(path: str) -> bool:
    parts = path.split("/")
    return any(part in TEST_DIRS for part in parts[:-1]) or path.endswith("_test.go")


def release_path(path: str) -> bool:
    if is_test_path(path):
        return False
    return (path.startswith(RELEASE_PREFIXES) or path in RELEASE_FILES
            or any(fnmatch.fnmatchcase(path, glob) for glob in RELEASE_GLOBS))


def roll_path(path: str) -> bool:
    if is_test_path(path) or path.endswith(".md"):
        return False
    return path.startswith(ROLL_PREFIXES) or path in ROLL_FILES


def runner_roll_needed(paths) -> bool:
    return any(path.startswith(RUNNER_ROLL_PREFIXES) for path in paths)


WEBSITE_URL = "https://forkmesh.com/"
WEBSITE_HEADER = "x-forkmesh-deploy-fingerprint"
WEBSITE_TIMEOUT = 900


WEBSITE_TREES = ("www", "marketplace")


def website_fingerprint(git, target) -> str:
    trees = [git.call("rev-parse", "%s:%s" % (target, path), check=True).stdout.strip()
             for path in WEBSITE_TREES]
    return hashlib.sha256(("www-tree:" + ":".join(trees)).encode()).hexdigest()


def live_website_fingerprint(run=subprocess.run) -> str:
    completed = run(["curl", "-sS", "-I", "--max-time", "15", WEBSITE_URL],
                    capture_output=True, text=True, timeout=30)
    for line in (completed.stdout or "").splitlines():
        name, _, value = line.partition(":")
        if name.strip().lower() == WEBSITE_HEADER:
            return value.strip()
    return ""


def node22_bin(home=None):
    home = Path(home or os.path.expanduser("~"))
    candidates = sorted((home / ".nvm" / "versions" / "node").glob("v2[2-9]*/bin"),
                        reverse=True)
    for candidate in candidates:
        if (candidate / "npx").exists():
            return candidate
    return None


def classify(paths) -> dict:
    paths = sorted(set(p for p in paths if p))
    return {"release": [p for p in paths if release_path(p)],
            "roll": [p for p in paths if roll_path(p)]}


def workflow_path_patterns(depth=PATH_DEPTH) -> list:
    patterns = []
    for prefix in RELEASE_PREFIXES + ROLL_PREFIXES:
        base = prefix.rstrip("/")
        if any(prefix != other and prefix.startswith(other)
               for other in RELEASE_PREFIXES + ROLL_PREFIXES):
            continue
        patterns += [base + "/*" * level for level in range(1, depth + 1)]
    return patterns + list(RELEASE_FILES) + list(ROLL_FILES) + [
        glob[:-len(".py")] + "*" if glob.endswith(".py") else glob
        for glob in RELEASE_GLOBS]


def ready_to_start(now, first_request_at, last_request_at, main_changed_at,
                   debounce=DEBOUNCE_SECONDS, settle=SETTLE_SECONDS,
                   max_wait=MAX_WAIT_SECONDS) -> bool:
    if first_request_at is None:
        return False
    if now - first_request_at >= max_wait:
        return True
    return (now - main_changed_at >= debounce
            and now - (last_request_at or first_request_at) >= settle)


def switch_state(directory=None):
    directory = Path(directory or common.autodeploy_dir())
    paused = directory / "paused"
    if paused.exists():
        try:
            reason = paused.read_text("utf-8").strip()
        except OSError:
            reason = ""
        return "paused", reason or "paused"
    if not (directory / "armed").exists():
        return "disarmed", "not armed"
    return "armed", ""


def _write_private(path: Path, text: str):
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as handle:
        handle.write(text)


def pause(reason="", directory=None, clock=time.time):
    directory = Path(directory or common.autodeploy_dir())
    stamp = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(clock()))
    _write_private(directory / "paused", "%s (%s)\n" % (reason or "paused by hand", stamp))


def resume(directory=None, clock=time.time):
    directory = Path(directory or common.autodeploy_dir())
    try:
        (directory / "paused").unlink()
    except FileNotFoundError:
        pass
    stamp = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(clock()))
    _write_private(directory / "armed", "armed %s\n" % stamp)


def disarm(directory=None):
    directory = Path(directory or common.autodeploy_dir())
    for name in ("armed",):
        try:
            (directory / name).unlink()
        except FileNotFoundError:
            pass


def auto_roll_enabled(directory=None) -> bool:
    return not (Path(directory or common.autodeploy_dir()) / "manual-roll").exists()


def state_path(directory=None) -> Path:
    return Path(directory or common.autodeploy_dir()) / "state.json"


def load_state(directory=None) -> dict:
    data = common.load_json(state_path(directory), {}) or {}
    return data if isinstance(data, dict) else {}


def save_state(state, directory=None):
    common.save_json(state_path(directory), state)


class Git:
    def __init__(self, repo, run=subprocess.run):
        self.repo = Path(repo)
        self._run = run

    def call(self, *args, input_text=None, check=False, timeout=600):
        completed = self._run(["git", *args], cwd=self.repo, input=input_text,
                              capture_output=True, text=True, timeout=timeout,
                              check=False)
        if check and completed.returncode != 0:
            raise ToolError("git %s failed: %s" % (
                " ".join(args[:3]), (completed.stderr or "").strip()[-400:]))
        return completed

    def rev(self, ref) -> str:
        completed = self.call("rev-parse", "--verify", "--quiet", ref + "^{commit}")
        return completed.stdout.strip() if completed.returncode == 0 else ""

    def is_ancestor(self, older, newer) -> bool:
        if not older or not newer:
            return False
        return self.call("merge-base", "--is-ancestor", older, newer).returncode == 0

    def signature(self, commit) -> str:
        return common.served_signature(commit, self.repo, run=self._run)

    def changed_paths(self, older, newer) -> list:
        completed = self.call("diff", "--name-only", "--no-renames", older, newer, check=True)
        return [line for line in completed.stdout.splitlines() if line.strip()]

    def history(self, tip, limit=HISTORY_LIMIT) -> list:
        completed = self.call("rev-list", "-n", str(limit), tip, check=True)
        return completed.stdout.split()

    def find_signature(self, signature, tip, limit=HISTORY_LIMIT) -> str:
        commits = self.history(tip, limit)
        if not commits:
            return ""
        roots = common.SIGNATURE_ROOTS
        lines = "".join("%s:%s\n" % (c, root) for c in commits for root in roots)
        completed = self.call("cat-file", "--batch-check=%(objectname)",
                              input_text=lines, check=True)
        ids = completed.stdout.split("\n")
        for index, commit in enumerate(commits):
            trees = ids[index * len(roots):(index + 1) * len(roots)]
            if len(trees) != len(roots) or any(" missing" in t or not t for t in trees):
                continue
            text = "\n".join("%s %s" % pair for pair in zip(roots, trees)) + "\n"
            if hashlib.sha256(text.encode()).hexdigest() == signature:
                return commit
        return ""

    def show(self, commit, path) -> str:
        return self.call("show", "%s:%s" % (commit, path), check=True).stdout


def locate(rev, manifest, git, target, main):
    entry = (manifest.get("releases") or {}).get(rev) or {}
    commit = entry.get("commit") or ""
    if commit and not git.rev(commit):
        commit = ""
    if not commit:
        commit = git.rev(rev) if REV_RE.match(rev or "") else ""
    signature = entry.get("signature") or ""
    if not signature and commit:
        try:
            signature = git.signature(commit)
        except ToolError:
            signature = ""
    if commit and git.is_ancestor(commit, target):
        return "ancestor", commit
    if signature:
        match = git.find_signature(signature, target)
        if match:
            return "ancestor", match
    if commit and git.is_ancestor(target, commit):
        return "newer", commit
    if signature:
        match = git.find_signature(signature, main)
        if match and git.is_ancestor(target, match):
            return "newer", match
    if commit:
        return "diverged", commit
    return "unknown", ""


def fast_forward_gate(live, manifest, git, target, main, mode="auto"):
    if not git.is_ancestor(target, main):
        raise GuardError("%s is not on main" % target[:12])
    positions = {}
    notes = []
    for node, rev in sorted(live.items()):
        kind, commit = locate(rev, manifest, git, target, main)
        if kind == "ancestor":
            positions[node] = commit
            continue
        if kind == "unknown":
            if mode == "auto":
                raise GuardError(
                    "%s runs %s, which matches no commit on main by content; deploy "
                    "once by hand (next_autodeploy.py deploy --commit ...)" % (node, rev))
            notes.append("%s runs %s, unknown here; a manual deploy proceeds" % (node, rev))
            continue
        if mode != "rollback":
            raise GuardError(
                "%s runs %s (%s), which is %s %s; refusing to roll production back "
                "(use next_autodeploy.py rollback --rev ...)" % (
                    node, rev, commit[:12],
                    "newer than" if kind == "newer" else "not an ancestor of",
                    target[:12]))
        notes.append("%s runs %s (%s): manual rollback" % (node, rev, commit[:12]))
    base = ""
    for commit in positions.values():
        if not base or git.is_ancestor(commit, base):
            base = commit
    for commit in positions.values():
        if not git.is_ancestor(base, commit):
            raise GuardError("the nodes run revisions that are not on one line of history")
    return {"positions": positions, "base": base, "notes": notes}


def fleet_hosts(text: str, instances=None) -> dict:
    import tomllib
    import render
    data = tomllib.loads(text)
    production = data.get("production") or {}
    cores = [n for n in data.get("nodes") or [] if n.get("role", "core") == "core"]
    render.fill_public_addresses(cores, instances)
    nodes = [{"name": n["name"], "ip": n["public_ipv4"]} for n in cores]
    return {"domain": data.get("domain", ""),
            "app_host": production.get("app_host") or data.get("app_host", ""),
            "world_host": production.get("world_host") or data.get("world_host", ""),
            "nodes": nodes}


HOST_RE = re.compile(r"^[a-z0-9.-]{1,253}$")
IP_RE = re.compile(r"^[0-9.]{7,15}$")
CURL_ATTEMPTS = 5
CURL_RETRY_SECONDS = 2
OFFLINE_WAIT_SECONDS = 1800
OFFLINE_POLL_SECONDS = 30
UNREACHABLE_WAIT_SECONDS = 300


def curl(host, ip, path, run=subprocess.run, timeout=10, attempts=CURL_ATTEMPTS,
         sleep=time.sleep):
    if not HOST_RE.match(host) or not IP_RE.match(ip):
        raise GuardError("bad host %r or address %r" % (host, ip))
    address = common.route_address(ip) if run is subprocess.run else ip
    target = "[%s]" % address if ":" in address else address
    for attempt in range(attempts):
        if attempt:
            sleep(CURL_RETRY_SECONDS)
        try:
            completed = run(["curl", "-sS", "--max-time", str(timeout), "-o", "-",
                             "-w", "\n%{http_code}", "--resolve", "%s:443:%s" % (host, target),
                             "https://%s%s" % (host, path)],
                            capture_output=True, text=True, timeout=timeout + 5,
                            check=False)
        except subprocess.TimeoutExpired:
            continue
        body, _, code = (completed.stdout or "").rpartition("\n")
        if code.isdigit() and int(code):
            return int(code), body
    return 0, ""


def read_revs(hosts, run=subprocess.run, sleep=time.sleep) -> list:
    answers = []
    for node in hosts["nodes"]:
        code, body = curl(hosts["app_host"], node["ip"], "/api/version", run=run,
                          sleep=sleep)
        try:
            rev = json.loads(body).get("rev") if code == 200 else ""
        except ValueError:
            rev = ""
        answers.append((node["name"], code, rev or ""))
    return answers


def reask_silent(hosts, answers, run=subprocess.run, say=None, sleep=time.sleep,
                 clock=time.monotonic, wait=UNREACHABLE_WAIT_SECONDS) -> list:
    deadline = clock() + wait
    announced = False
    while True:
        silent = [name for name, code, _rev in answers if not code]
        if not silent or clock() >= deadline:
            return answers
        if not announced and say:
            say("gate 1: %s did not answer while other cores did; asking again for up "
                "to %d min" % (", ".join(silent), wait // 60))
            announced = True
        sleep(OFFLINE_POLL_SECONDS)
        nodes = [node for node in hosts["nodes"] if node["name"] in silent]
        fresh = {name: (name, code, rev) for name, code, rev in
                 read_revs(dict(hosts, nodes=nodes), run=run, sleep=sleep)}
        answers = [fresh.get(name, (name, code, rev)) for name, code, rev in answers]


def live_revs(hosts, run=subprocess.run, say=None, sleep=time.sleep,
              clock=time.monotonic, offline_wait=OFFLINE_WAIT_SECONDS,
              unreachable_wait=UNREACHABLE_WAIT_SECONDS) -> dict:
    deadline = clock() + offline_wait
    announced = False
    while True:
        answers = read_revs(hosts, run=run, sleep=sleep)
        if any(code for _name, code, _rev in answers) or len(answers) < 2:
            break
        if clock() >= deadline:
            raise GuardError(
                "no core node answered /api/version (HTTP 0 from %s) for %d s; "
                "this machine looks offline (check its own DNS and route), the "
                "fleet may be fine" % (", ".join(n for n, _c, _r in answers),
                                       offline_wait))
        if not announced and say:
            say("gate 1: no core node answers; this machine looks offline, "
                "waiting up to %d min for its network" % (offline_wait // 60))
            announced = True
        sleep(OFFLINE_POLL_SECONDS)
    if announced and say:
        say("gate 1: the network is back")
    answers = reask_silent(hosts, answers, run=run, say=say, sleep=sleep, clock=clock,
                           wait=unreachable_wait)
    revs = {}
    for name, code, rev in answers:
        if not rev:
            raise GuardError("%s did not answer /api/version (HTTP %s); the fleet "
                             "must be healthy before a deploy" % (name, code))
        revs[name] = rev
    return revs


def verify_once(hosts, rev, run=subprocess.run) -> list:
    problems = []
    for node in hosts["nodes"]:
        name, ip = node["name"], node["ip"]
        code, _ = curl(hosts["app_host"], ip, "/readyz", run=run)
        if code != 200:
            problems.append("%s: %s/readyz HTTP %s" % (name, hosts["app_host"], code))
        code, body = curl(hosts["app_host"], ip, "/api/version", run=run)
        try:
            running = json.loads(body).get("rev") if code == 200 else ""
        except ValueError:
            running = ""
        if running != rev:
            problems.append("%s: /api/version rev %r, want %s" % (name, running, rev))
        node_host = "%s.%s" % (name, hosts["domain"])
        code, _ = curl(node_host, ip, "/readyz", run=run)
        if code != 200:
            problems.append("%s: %s/readyz HTTP %s" % (name, node_host, code))
        for path in ("/dashboard", "/login"):
            code, _ = curl(hosts["app_host"], ip, path, run=run)
            if code != 200:
                problems.append("%s: %s%s HTTP %s" % (name, hosts["app_host"], path, code))
        code, body = curl(hosts["world_host"], ip, "/", run=run)
        if code != 200 or ('name="forkmesh-world-deploy" content="%s"' % rev) not in body:
            problems.append("%s: %s/ HTTP %s without deploy %s" % (
                name, hosts["world_host"], code, rev))
    return problems


def verify(hosts, rev, say, run=subprocess.run, sleep=time.sleep,
           clock=time.monotonic, timeout=VERIFY_SECONDS) -> bool:
    deadline = clock() + timeout
    while True:
        problems = verify_once(hosts, rev, run=run)
        if not problems:
            say("verify: %s on %s: readyz, /api/version, node readyz, /dashboard, "
                "/login and the World page all answer" % (
                    rev, ", ".join(n["name"] for n in hosts["nodes"])))
            return True
        if clock() >= deadline:
            for problem in problems:
                say("verify: %s" % problem)
            return False
        sleep(5)


class RunSink:
    def __init__(self, run_id, workspace: Path):
        self.run_id = run_id
        self.closed = False
        flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW
        directory = os.open(workspace, flags)
        try:
            info = os.fstat(directory)
            if info.st_uid != os.getuid():
                raise ToolError("run %s workspace is not ours" % run_id)
            self.fd = os.open(SINK_NAME, os.O_WRONLY | os.O_CREAT | os.O_EXCL
                              | os.O_APPEND | os.O_NOFOLLOW | os.O_NONBLOCK,
                              0o644, dir_fd=directory)
        finally:
            os.close(directory)
        info = os.fstat(self.fd)
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
            os.close(self.fd)
            raise ToolError("run %s: %s is not a plain file" % (run_id, SINK_NAME))

    def write(self, text: str):
        if self.closed:
            return
        try:
            os.write(self.fd, text.encode("utf-8", "replace"))
        except OSError:
            self.close()

    def finish(self, ok: bool, message: str):
        self.write("%s %s %s\n" % (RESULT_MARK, "ok" if ok else "fail",
                                   " ".join(message.split())))
        self.close()

    def close(self):
        if not self.closed:
            self.closed = True
            try:
                os.close(self.fd)
            except OSError:
                pass


class Log:
    def __init__(self, path=None, echo=True, clock=time.time):
        self.lock = threading.Lock()
        self.lines = []
        self.sinks = {}
        self.clock = clock
        self.echo = echo
        self.handle = None
        if path is not None:
            Path(path).parent.mkdir(parents=True, exist_ok=True)
            self.handle = open(path, "a", encoding="utf-8")
        self.path = path

    def say(self, message: str):
        stamp = time.strftime("%H:%M:%S", time.localtime(self.clock()))
        with self.lock:
            for raw in str(message).splitlines() or [""]:
                line = "%s %s\n" % (stamp, raw)
                self.lines.append(line)
                if self.handle:
                    self.handle.write(line)
                    self.handle.flush()
                if self.echo:
                    sys.stdout.write(line)
                    sys.stdout.flush()
                for sink in list(self.sinks.values()):
                    sink.write(line)

    def attach(self, sink):
        with self.lock:
            for line in self.lines:
                sink.write(line)
            self.sinks[sink.run_id] = sink

    def detach(self, run_id):
        with self.lock:
            return self.sinks.pop(run_id, None)

    def close(self):
        if self.handle:
            self.handle.close()
            self.handle = None


def actions_root(layout=None) -> Path:
    return (layout or forkmesh_paths.resolve()).data_path("actions")


def running_runs(root: Path) -> list:
    sandbox = root / "sandbox"
    runs = []
    try:
        entries = list(os.scandir(sandbox))
    except FileNotFoundError:
        return runs
    newest = {}
    for entry in entries:
        match = RUN_DIR_RE.match(entry.name)
        if not match or not entry.is_dir(follow_symlinks=False):
            continue
        run_id, started = int(match.group(1)), int(match.group(2))
        if run_id not in newest or started > newest[run_id][0]:
            newest[run_id] = (started, entry)
    for run_id, (_, entry) in newest.items():
        metas = list((root / "runs").glob("*/%d/meta.json" % run_id))
        if len(metas) != 1:
            continue
        try:
            meta = json.loads(metas[0].read_text("utf-8"))
        except (OSError, ValueError):
            continue
        if meta.get("workflowPath") != WORKFLOW_PATH or meta.get("status") != "running":
            continue
        if int(meta.get("id") or -1) != run_id:
            continue
        runs.append({"id": run_id, "meta": meta,
                     "workspace": Path(entry.path) / "workspace"})
    return sorted(runs, key=lambda r: r["id"])


def run_verdict(meta, on_main) -> tuple:
    if (meta.get("owner"), meta.get("name")) != REPOSITORY:
        return "refuse", "this run belongs to %s/%s, not %s/%s; only the canonical " \
            "repository deploys" % (meta.get("owner"), meta.get("name"), *REPOSITORY)
    if int(meta.get("pullNumber") or 0) > 0:
        return "skip", "pull request runs never deploy"
    if meta.get("ref") != MAIN_REF:
        return "skip", "%s is not main; only pushes to main deploy" % (meta.get("ref") or "?")
    commit = str(meta.get("commit") or "")
    if not SHA_RE.match(commit):
        return "refuse", "the run names no commit"
    if not on_main(commit):
        return "refuse", "%s is not on main" % commit[:12]
    return "request", commit


def sign_alert(key_path: Path, node: str, ts: str, fields: list) -> str:
    from cryptography.hazmat.primitives import serialization
    key = serialization.load_pem_private_key(Path(key_path).read_bytes(), None)
    digest = hashlib.sha256(json.dumps(fields, separators=(",", ":")).encode()).hexdigest()
    canonical = "\n".join([ALERT_PROOF, node, ts, digest]).encode()
    return base64.b64encode(key.sign(canonical)).decode()


def settings_node_name(layout=None) -> str:
    layout = layout or forkmesh_paths.resolve()
    try:
        text = layout.settings_file.read_text("utf-8", "replace")
    except OSError:
        return ""
    match = re.search(r"(?m)^nodeName=([a-z0-9_-]{1,40})\s*$", text)
    return match.group(1) if match else ""


class Notifier:
    def __init__(self, origin="", node="", key_path=None, opener=None, say=print,
                 clock=time.time):
        self.origin = origin
        self.node = node
        self.key_path = key_path
        self.opener = opener or urllib.request.urlopen
        self.say = say
        self.clock = clock

    def send(self, title, body, key, email=False) -> bool:
        if not (self.origin and self.node and self.key_path and Path(self.key_path).exists()):
            self.say("notify: no admin identity or origin; Ping not sent")
            return False
        severity = "email" if email else "ping"
        fields = [severity, key[:80], title[:120], body[:2000]]
        ts = str(int(self.clock() * 1000))
        sig = sign_alert(self.key_path, self.node, ts, fields)
        query = urllib.parse.urlencode({"node": self.node, "ts": ts, "sig": sig})
        payload = json.dumps({"severity": severity, "key": fields[1], "title": fields[2],
                              "body": fields[3]}).encode()
        request = urllib.request.Request(
            "https://%s%s?%s" % (self.origin, ALERT_PATH, query), data=payload,
            method="POST", headers={"Content-Type": "application/json",
                                    "User-Agent": "forkmesh-node-deploy/1"})
        try:
            with self.opener(request, timeout=20) as response:
                reply = json.loads(response.read() or b"{}")
        except urllib.error.HTTPError as error:
            self.say("notify: admin alert refused (HTTP %s)" % error.code)
            return False
        except (OSError, ValueError) as error:
            self.say("notify: admin alert failed (%s)" % type(error).__name__)
            return False
        self.say("notify: %s Ping(s)%s" % (reply.get("pings", 0), (
            ", %s email(s)" % reply.get("emails", 0)) if email else ""))
        return True



def stream(command, say, cwd=None, env=None, timeout=3600, pass_fds=(), popen=subprocess.Popen,
           clock=time.monotonic) -> int:
    process = popen(command, cwd=cwd, env=env, stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                    pass_fds=tuple(pass_fds), text=True, errors="replace")
    deadline = clock() + timeout
    timer = threading.Timer(timeout, process.kill)
    timer.daemon = True
    timer.start()
    try:
        for line in process.stdout:
            say(line.rstrip("\n"))
        code = process.wait()
    finally:
        timer.cancel()
    if clock() >= deadline:
        say("timed out after %d s" % timeout)
        return 124
    return code


def build_cache_dir() -> Path:
    configured = os.environ.get("FORKMESH_BUILD_CACHE")
    base = Path(configured) if configured else Path(
        os.environ.get("XDG_CACHE_HOME") or Path.home() / ".cache") / "forkmesh"
    return base / "worker-python"


GO_FALLBACKS = ("/usr/local/go/bin/go", "/usr/lib/go/bin/go", "~/go/bin/go", "~/.local/go/bin/go")


def go_root() -> Path:
    go = shutil.which("go") or next(
        (path for path in map(os.path.expanduser, GO_FALLBACKS) if os.path.isfile(path)), None)
    if not go:
        raise ToolError("go is required for the test gate (the %s Go tests); install it on "
                        "this machine or put it on the agent's PATH" % ", ".join(GO_TEST_MODULES))
    return Path(go).resolve().parent.parent


def go_test_env(work: Path, venv: Path, root: Path) -> tuple:
    return (("PATH", "%s/bin:%s/bin:/usr/bin:/bin" % (root, venv)),
            ("GOROOT", str(root)), ("GOCACHE", str(work / "gocache")),
            ("GOPATH", str(work / "gopath")), ("GOTOOLCHAIN", "local"),
            ("GOPROXY", "off"), ("GOFLAGS", "-mod=readonly"), ("CGO_ENABLED", "0"))


def sandbox_command(work: Path, venv: Path, data_bin: Path, argv: list, binds=(),
                    env=()) -> list:
    bwrap = shutil.which("bwrap")
    if not bwrap:
        raise ToolError("bwrap is required for the test gate")
    nice = shutil.which("nice")
    command = ([nice, "-n", "10"] if nice else []) + [bwrap, "--die-with-parent", "--new-session", "--unshare-all",
               "--proc", "/proc", "--dev", "/dev", "--tmpfs", "/tmp", "--tmpfs", "/run"]
    for path in ("/usr", "/bin", "/lib", "/lib64", "/etc", "/sbin"):
        if os.path.exists(path):
            command += ["--ro-bind", path, path]
    command += ["--bind", str(work), str(work), "--ro-bind", str(venv), str(venv)]
    cache = build_cache_dir()
    if cache.is_dir():
        command += ["--ro-bind", str(cache), str(cache)]
    if data_bin.is_dir():
        command += ["--ro-bind", str(data_bin.resolve()), str(data_bin.resolve())]
    for path in binds:
        command += ["--ro-bind", str(path), str(path)]
    command += ["--clearenv", "--setenv", "HOME", str(work / "home"),
                "--setenv", "PATH", "%s/bin:/usr/bin:/bin" % venv,
                "--setenv", "LANG", "C.UTF-8", "--setenv", "TMPDIR", "/tmp",
                "--setenv", "PYTHONDONTWRITEBYTECODE", "1",
                *(["--setenv", "FORKMESH_BUILD_CACHE", str(cache.parent)]
                  if cache.is_dir() else []),
                *[item for name, value in env for item in ("--setenv", name, value)],
                "--chdir", str(work / "src"), *argv]
    return command


FAIL_RE = re.compile(r"^(FAILED|ERROR) (\S+)")
GO_FAIL_RE = re.compile(r"^\s*--- FAIL: (\S+)")


def failing_ids(lines) -> set:
    return {match.group(2) for match in map(FAIL_RE.match, lines) if match}


def failing_go_ids(module, lines) -> set:
    return {"%s::%s" % (module, match.group(1)) for match in map(GO_FAIL_RE.match, lines) if match}


class Pipeline:
    def __init__(self, repo, log, mode="auto", run=subprocess.run, popen=subprocess.Popen,
                 notifier=None, layout=None, skip_tests=False, state_dir=None,
                 lock_wait=LOCK_WAIT_SECONDS, sleep=time.sleep):
        self.git = Git(repo, run=run)
        self.repo = Path(repo)
        self.log = log
        self.mode = mode
        self.run = run
        self.popen = popen
        self.notifier = notifier
        self.layout = layout or forkmesh_paths.resolve()
        self.skip_tests = skip_tests
        self.state_dir = state_dir
        self.lock_wait = lock_wait
        self.sleep = sleep

    def say(self, message):
        self.log.say(message)

    def hosts(self, commit):
        return fleet_hosts(self.git.show(commit, "deploy/fleet.toml"))

    def scratch(self) -> Path:
        path = self.layout.cache / "node-deploy"
        path.mkdir(parents=True, exist_ok=True)
        return path

    def venv(self, src: Path) -> Path:
        requirements = src / "app" / "host" / "requirements.txt"
        digest = hashlib.sha256(requirements.read_bytes()).hexdigest()[:12]
        venv = self.scratch() / ("venv-" + digest)
        if (venv / "bin" / "python").exists() and (venv / ".complete").exists():
            return venv
        if venv.exists():
            venv = self.scratch() / ("venv-%s-%d" % (digest, int(time.time())))
        self.say("tests: creating the test venv %s" % venv.name)
        if stream([sys.executable, "-m", "venv", str(venv)], self.say, popen=self.popen) != 0:
            raise ToolError("could not create %s" % venv)
        if stream([str(venv / "bin" / "pip"), "install", "--quiet", "--only-binary=:all:",
                   "-r", str(requirements)], self.say, timeout=900, popen=self.popen) != 0:
            raise ToolError("could not install the test requirements")
        (venv / ".complete").write_text(digest + "\n")
        return venv

    def archive(self, commit, work: Path):
        (work / "src").mkdir(parents=True)
        (work / "home").mkdir()
        (work / "data").mkdir()
        data_bin = common.data_dir() / "bin"
        if data_bin.is_dir():
            (work / "data" / "bin").symlink_to(data_bin.resolve())
        archive = self.run(["git", "archive", "--format=tar", commit], cwd=self.repo,
                           capture_output=True, check=False, timeout=600)
        if archive.returncode != 0:
            raise ToolError("git archive %s failed" % commit[:12])
        extract = self.run(["tar", "-x", "-C", str(work / "src")], input=archive.stdout,
                           capture_output=True, check=False, timeout=600)
        if extract.returncode != 0:
            raise ToolError("could not unpack the archive of %s" % commit[:12])

    def remember_baseline(self, commit, failures):
        state = load_state(self.state_dir)
        baselines = dict(list((state.get("baselines") or {}).items())[-4:])
        baselines[commit] = sorted(failures)
        state["baselines"] = baselines
        save_state(state, self.state_dir)

    def prestage(self, work: Path, venv: Path):
        code = stream(sandbox_command(work, venv, work / "data" / "bin", [
            "sh", "-c", "cd app && exec python tools/build_worker_python.py"]),
            lambda line: None, timeout=TEST_TIMEOUT, popen=self.popen)
        if code != 0:
            raise ToolError("could not stage app/src_build for the tests")

    def test_files(self, src: Path):
        files = []
        for suite in COMPARE_SUITE:
            files += sorted(p.relative_to(src).as_posix()
                            for p in (src / suite).glob("test_*.py"))
        for path in MUST_PASS:
            if (src / path).is_dir():
                extra = sorted(p.relative_to(src).as_posix()
                               for p in (src / path).glob("test_*.py"))
            else:
                extra = [path] if (src / path.split("::", 1)[0]).exists() else []
            files += [item for item in extra if item not in files]
        return files

    def pytest_quiet(self, work: Path, venv: Path, path: str):
        lines = []
        argv = ["python", "-m", "pytest", "-q", "-p", "no:cacheprovider", "-rfE",
                "--color=no", path]
        code = stream(sandbox_command(work, venv, work / "data" / "bin", argv),
                      lines.append, timeout=TEST_TIMEOUT, popen=self.popen)
        return path, code, failing_ids(lines), lines

    def go_tests(self, work: Path, venv: Path):
        root = go_root()
        results = {}
        for module in GO_TEST_MODULES:
            started = time.monotonic()
            lines = []
            argv = [str(root / "bin" / "go"), "test", "-C", module, "./..."]
            code = stream(sandbox_command(work, venv, work / "data" / "bin", argv,
                                          binds=(root,), env=go_test_env(work, venv, root)),
                          lines.append, timeout=TEST_TIMEOUT, popen=self.popen)
            failures = failing_go_ids(module, lines)
            results[module] = (code, failures, lines)
            self.say("tests: go test %s: exit %d in %.0f s" % (
                module, code, time.monotonic() - started))
        return results

    def pytest_many(self, work: Path, venv: Path, files: list, label: str):
        from concurrent.futures import ThreadPoolExecutor
        started = time.monotonic()
        results = {}
        with ThreadPoolExecutor(max_workers=TEST_WORKERS) as pool:
            for path, code, failures, lines in pool.map(
                    lambda path: self.pytest_quiet(work, venv, path), files):
                results[path] = (code, failures, lines)
                summary = next((line for line in reversed(lines)
                                if " in " in line and ("passed" in line or "failed" in line
                                                       or "error" in line)), "exit %d" % code)
                self.say("tests: %s: %s: %s" % (label, path, summary.strip("= ")))
        self.say("tests: %s: %d file(s) in %.0f s" % (
            label, len(files), time.monotonic() - started))
        return results

    def test_gate(self, target, base):
        if self.skip_tests:
            self.say("tests: skipped by request (manual)")
            return
        state = load_state(self.state_dir)
        with tempfile.TemporaryDirectory(prefix="tests-", dir=self.scratch()) as tmp:
            work = Path(tmp) / "target"
            self.archive(target, work)
            venv = self.venv(work / "src")
            self.prestage(work, venv)
            from concurrent.futures import ThreadPoolExecutor
            with ThreadPoolExecutor(max_workers=1) as pool:
                go_future = pool.submit(self.go_tests, work, venv)
                results = self.pytest_many(work, venv, self.test_files(work / "src"),
                                           "target")
                go_results = go_future.result()
            broken = []
            for module, (code, failures, lines) in sorted(go_results.items()):
                if code != 0:
                    broken += sorted(failures) or ["go test %s exit %d" % (module, code)]
                    for line in lines[-40:]:
                        self.say("  " + line)
            for path, (code, failures, lines) in sorted(results.items()):
                if not any(path == item or path.startswith(item.rstrip("/") + "/")
                           for item in MUST_PASS):
                    continue
                if code != 0:
                    broken += sorted(failures) or ["%s exit %d" % (path, code)]
                    for line in lines[-40:]:
                        self.say("  " + line)
            if broken:
                raise ToolError("must-pass tests failed: %s" % ", ".join(broken[:10]))
            failures = set()
            for path, (code, found, _lines) in results.items():
                if any(path.startswith(suite.rstrip("/") + "/") for suite in COMPARE_SUITE):
                    failures |= found
            self.remember_baseline(target, failures)
            if not base:
                self.say("tests: no deployed base to compare against; %d failure(s) "
                         "recorded" % len(failures))
                return
            baselines = state.get("baselines") or {}
            known = baselines.get(base)
            if known is None:
                base_work = Path(tmp) / "base"
                self.archive(base, base_work)
                self.prestage(base_work, venv)
                files = [f for f in self.test_files(base_work / "src")
                         if any(f.startswith(suite.rstrip("/") + "/")
                                for suite in COMPARE_SUITE)]
                known = set()
                for _code, found, _lines in self.pytest_many(
                        base_work, venv, files, "deployed %s" % base[:12]).values():
                    known |= found
                known = sorted(known)
                self.remember_baseline(base, known)
            new = sorted(failures - set(known))
            if new:
                for path, (_code, found, lines) in sorted(results.items()):
                    if found & set(new):
                        for line in lines[-40:]:
                            self.say("  " + line)
                raise ToolError("new test failures against the deployed %s: %s" % (
                    base[:12], ", ".join(new[:10])))
            self.say("tests: no new failures against the deployed %s (%d known)" % (
                base[:12], len(known)))

    def worktree(self, target) -> Path:
        path = self.layout.node_deploy_worktree
        self.git.call("worktree", "remove", "--force", str(path))
        self.git.call("worktree", "prune")
        if path.exists():
            raise ToolError("%s exists and is not a worktree git can remove; move it "
                            "aside by hand" % path)
        path.parent.mkdir(parents=True, exist_ok=True)
        self.git.call("worktree", "add", "--detach", str(path), target, check=True)
        return path

    def tool(self, worktree, lock, args, timeout):
        command = [sys.executable, "tools/next_deploy.py", *args]
        self.say("$ python3 tools/next_deploy.py %s" % " ".join(args))
        causes = []

        def say(line):
            if line.startswith("next_deploy: "):
                causes.append(line[len("next_deploy: "):].strip())
            self.say(line)
        code = stream(command, say, cwd=worktree, env=lock.child_env(),
                      timeout=timeout, pass_fds=(lock.fd,), popen=self.popen)
        if code != 0:
            raise ToolError("next_deploy %s exited %d%s" % (
                args[0], code, ": %s" % causes[-1] if causes else ""))

    def execute(self, target) -> dict:
        rev = target[:12]
        result = {"target": target, "rev": rev, "ok": False, "deployed": False,
                  "roll": [], "message": ""}
        lock = common.DeployLock(what="next_autodeploy %s %s" % (self.mode, rev))
        try:
            lock.acquire(wait=0)
        except common.LockBusy as busy:
            self.say("lock: %s; waiting up to %d min" % (
                common.describe_holder(busy.holder), self.lock_wait // 60))
            lock.acquire(wait=self.lock_wait, sleep=self.sleep)
        try:
            result = self._execute(target, rev, lock, result)
            result["nodesOk"] = bool(result.get("ok"))
            if result["nodesOk"] and result.get("roll"):
                if auto_roll_enabled(self.state_dir):
                    self.roll_nodes(target, lock, result)
                else:
                    self.say("roll: manual-roll is set; leaving the node roll to an operator")
            if result["nodesOk"]:
                self.website(target, result)
            return result
        finally:
            lock.release()

    def roll_nodes(self, target, lock, result):
        self.say("roll: %s changed daemon or gateway files; rolling the cores one at a "
                 "time" % target[:12])
        for path in result["roll"][:20]:
            self.say("  %s" % path)
        try:
            worktree = self.worktree(target)
            self.say("$ python3 tools/next_fleet.py roll --execute")
            code = stream([sys.executable, "tools/next_fleet.py", "roll", "--execute"],
                          self.say, cwd=worktree, env=lock.child_env(),
                          timeout=ROLL_TIMEOUT, pass_fds=(lock.fd,), popen=self.popen)
        except (ToolError, OSError, subprocess.SubprocessError) as error:
            code = str(error)
        if code == 0:
            result["rolled"] = True
            result["message"] += "; cores rolled"
            self.say("roll: every core rolled and back in DNS")
            if runner_roll_needed(result["roll"]):
                self.roll_runners(target, lock, result, worktree)
        else:
            result["rollError"] = "next_fleet roll exited %s" % code
            self.say("roll: FAILED: %s" % result["rollError"])

    def runner_worktree(self, target) -> Path:
        path = self.layout.node_deploy_worktree
        if path.exists() and self.git.call("-C", str(path), "rev-parse",
                                           "HEAD").stdout.strip() == target:
            return path
        return self.worktree(target)

    def roll_runners(self, target, lock, result, worktree=None):
        self.say("roll: %s changed the Go daemon; rolling the runners that run no "
                 "agent job" % target[:12])
        command = ["tools/next_fleet.py", "roll-runners", "--idle-wait", "0", "--execute"]
        try:
            worktree = worktree or self.runner_worktree(target)
            self.say("$ python3 %s" % " ".join(command))
            code = stream([sys.executable, *command], self.say, cwd=worktree,
                          env=lock.child_env(), timeout=ROLL_TIMEOUT,
                          pass_fds=(lock.fd,), popen=self.popen)
        except (ToolError, OSError, subprocess.SubprocessError) as error:
            code = str(error)
        if code == 0:
            result["runnersRolled"] = True
            result["message"] += "; runners rolled"
            self.say("roll: every runner runs the new daemon")
        elif code == RUNNERS_BUSY_EXIT:
            result["runnerRollPending"] = True
            result["message"] += "; runner roll waits for running agent jobs"
            self.say("roll: a runner is busy; the agent retries every %d s until it "
                     "is idle" % RUNNER_ROLL_RETRY_SECONDS)
        else:
            result["runnerRollError"] = "next_fleet roll-runners exited %s" % code
            self.say("roll: runners FAILED: %s" % result["runnerRollError"])

    def website(self, target, result):
        try:
            wanted = website_fingerprint(self.git, target)
            live = live_website_fingerprint(run=self.run)
            if live == wanted:
                self.say("website: forkmesh.com already serves www/ of %s" % target[:12])
                return
            self.say("website: forkmesh.com serves %s, www/ of %s is %s; deploying "
                     "forkmesh-www" % (live[:12] or "(unknown)", target[:12], wanted[:12]))
            bin_dir = node22_bin()
            if bin_dir is None:
                raise ToolError("Node 22 (~/.nvm/versions/node/v22*) is needed for wrangler")
            worktree = self.worktree(target)
            env = dict(os.environ)
            env["PATH"] = "%s:%s" % (bin_dir, env.get("PATH", ""))
            env["CLOUDFLARE_API_TOKEN"] = common.credential("cloudflare.env",
                                                            "CLOUDFLARE_API_TOKEN")
            env["CLOUDFLARE_ACCOUNT_ID"] = common.credential("cloudflare.env",
                                                             "CLOUDFLARE_ACCOUNT_ID")
            code = stream([str(bin_dir / "npx"), "--yes", "wrangler", "deploy",
                           "--config", "wrangler.production.toml",
                           "--var", "DEPLOY_FINGERPRINT:%s" % wanted],
                          self.say, cwd=str(Path(worktree) / "www"), env=env,
                          timeout=WEBSITE_TIMEOUT, popen=self.popen)
            if code != 0:
                raise ToolError("wrangler deploy exited %d" % code)
            for _ in range(12):
                if live_website_fingerprint(run=self.run) == wanted:
                    self.say("website: forkmesh.com now serves www/ of %s" % target[:12])
                    result["message"] += "; forkmesh.com updated"
                    return
                self.sleep(5)
            raise ToolError("forkmesh.com still does not serve %s" % wanted[:12])
        except (ToolError, OSError, subprocess.SubprocessError, KeyError) as error:
            self.say("website: FAILED: %s" % error)
            result.update(ok=False, message="%s; website deploy failed: %s" % (
                result.get("message", ""), error))

    def roll_paths(self, base, target, paths) -> list:
        state = load_state(self.state_dir)
        paths = set(paths) | set((state.get("roll") or {}).get("paths") or [])
        rolled = state.get("lastRoll") or {}
        ref = rolled.get("commit") or rolled.get("rev") or ""
        since = self.git.rev(ref) if REV_RE.match(ref) else ""
        if since and since != base and self.git.is_ancestor(since, base):
            paths.update(classify(self.git.changed_paths(since, target))["roll"])
        return sorted(paths)

    def _execute(self, target, rev, lock, result):
        main = self.git.rev(MAIN_REF)
        hosts = self.hosts(target)
        self.say("gate 1: %s must be on main and fast-forward what the cores run" % rev)
        live = live_revs(hosts, run=self.run, say=self.say, sleep=self.sleep)
        self.say("gate 1: live %s" % ", ".join("%s %s" % kv for kv in sorted(live.items())))
        manifest = common.load_manifest()
        gate = fast_forward_gate(live, manifest, self.git, target, main, mode=self.mode)
        for note in gate["notes"]:
            self.say("gate 1: %s" % note)
        base = gate["base"]
        if base:
            changed = classify(self.git.changed_paths(base, target))
            changed["roll"] = self.roll_paths(base, target, changed["roll"])
        else:
            changed = {"release": ["(unknown base)"], "roll": []}
        result["roll"] = changed["roll"]
        if all(r == rev for r in live.values()):
            result.update(ok=True, message="%s already runs on every node" % rev)
            self.say("gate 1: " + result["message"])
            return result
        if not changed["release"] and self.mode == "auto":
            result.update(ok=True, message="no served-stack change between the deployed "
                          "%s and %s" % (base[:12], rev))
            self.say("gate 1: " + result["message"])
            return result
        self.say("gate 1: passed (deployed position %s; %d served path(s) changed)" % (
            base[:12] if base else "unknown", len(changed["release"])))
        for path in changed["release"][:20]:
            self.say("  %s" % path)
        from concurrent.futures import ThreadPoolExecutor
        self.say("gate 2: tests from a clean git archive of %s, while gate 3 "
                 "builds and uploads (nothing is activated until they pass)" % rev)
        with ThreadPoolExecutor(max_workers=1) as pool:
            tests = pool.submit(self.test_gate, target, base)
            try:
                worktree = self.worktree(target)
                self.say("gate 3: build in %s (pinned to %s)" % (worktree, rev))
                self.tool(worktree, lock, ["build", "--execute"], BUILD_TIMEOUT)
                self.tool(worktree, lock, ["upload", "--rev", rev, "--execute"],
                          DEPLOY_TIMEOUT)
            finally:
                tests_error = tests.exception()
        if tests_error is not None:
            raise tests_error
        self.say("gate 2: passed")
        self.say("gate 4: next_deploy migrate, restart (one core at a time, the first chosen per release)")
        self.tool(worktree, lock, ["migrate", "--execute"], DEPLOY_TIMEOUT)
        result["deployed"] = True
        self.tool(worktree, lock, ["restart", "--rev", rev, "--execute"], DEPLOY_TIMEOUT)
        self.say("gate 5: post-deploy verification")
        if not verify(hosts, rev, self.say, run=self.run, sleep=self.sleep):
            raise ToolError("post-deploy verification failed for %s" % rev)
        result.update(ok=True, message="deployed %s to %s" % (
            rev, ", ".join(n["name"] for n in hosts["nodes"])))
        return result


def record_outcome(result, notifier, say, state_dir=None, mode="auto"):
    state = load_state(state_dir)
    rev = result.get("rev", "")
    if result.get("ok"):
        state["failures"] = 0
        state["emailed"] = False
        if result.get("deployed"):
            state["last"] = {"rev": rev, "commit": result.get("target"), "mode": mode,
                             "at": time.time()}
    else:
        state["failures"] = int(state.get("failures") or 0) + 1
        state["lastFailure"] = {"rev": rev, "message": result.get("message", ""),
                                "at": time.time()}
    roll = result.get("roll") or []
    note_runner_roll(state, result)
    if result.get("rolled"):
        state.pop("roll", None)
        state["lastRoll"] = {"rev": rev, "commit": result.get("target", ""),
                             "at": time.time()}
    elif roll and result.get("nodesOk", result.get("ok")):
        state["roll"] = {"since": rev, "paths": roll[:50], "at": time.time(),
                         "error": result.get("rollError", "")}
    save_state(state, state_dir)
    if notifier is None:
        return state
    if not result.get("ok"):
        email = state["failures"] >= EMAIL_AFTER_FAILURES and not state.get("emailed")
        notifier.send("Deploy of %s failed" % rev,
                      "%s\n\nConsecutive failures: %d. The full log is in the Deploy nodes "
                      "Actions run and in %s." % (result.get("message", ""),
                                                  state["failures"], common.logs_dir()),
                      "deploy-failed:%s" % rev, email=email)
        if email:
            state["emailed"] = True
            save_state(state, state_dir)
    elif result.get("runnerRollError"):
        notify_runner_roll(notifier, result)
    elif result.get("rolled"):
        notifier.send("Cores rolled after %s" % rev,
                      "%s changed daemon or gateway files; the cores were rolled one at a "
                      "time and are back in DNS:\n%s" % (rev, "\n".join(roll[:20])),
                      "node-rolled:%s" % rev)
    elif roll and result.get("rollError"):
        notifier.send("Node roll failed after %s" % rev,
                      "%s changed daemon or gateway files:\n%s\n\nThe automatic roll "
                      "stopped: %s. The core it stopped on is left drained out of DNS; "
                      "check it, then run python3 tools/next_fleet.py roll nN --execute "
                      "for the cores still to do and python3 tools/next_autodeploy.py "
                      "ack-roll (docs/deploy/auto-deploy.md)." % (
                          rev, "\n".join(roll[:20]), result["rollError"]),
                      "node-roll-failed:%s" % rev, email=True)
    elif roll:
        notifier.send("Node roll needed after %s" % rev,
                      "%s changed daemon or gateway files that only "
                      "next_fleet.py install applies:\n%s\n\nRoll one node at a time "
                      "(python3 tools/next_fleet.py roll, "
                      "docs/deploy/auto-deploy.md)." % (rev, "\n".join(roll[:20])),
                      "node-roll:%s" % rev)
    return state


def note_runner_roll(state, result):
    if result.get("runnersRolled"):
        state.pop("runnerRoll", None)
        state["lastRunnerRoll"] = {"rev": result.get("rev", ""), "at": time.time()}
    elif result.get("runnerRollPending") or result.get("runnerRollError"):
        state["runnerRoll"] = {"commit": result.get("target", ""),
                               "rev": result.get("rev", ""), "at": time.time(),
                               "error": result.get("runnerRollError", "")}


def notify_runner_roll(notifier, result):
    rev = result.get("rev", "")
    if result.get("runnersRolled"):
        notifier.send("Runners rolled after %s" % rev,
                      "Every runner was idle and now runs the Go daemon of %s." % rev,
                      "runners-rolled:%s" % rev)
    elif result.get("runnerRollError"):
        notifier.send("Runner roll failed after %s" % rev,
                      "%s changed the Go daemon; rolling the runners stopped: %s. "
                      "Check the runner, then run python3 tools/next_fleet.py "
                      "roll-runners --execute (docs/deploy/auto-deploy.md)." % (
                          rev, result["runnerRollError"]),
                      "runner-roll-failed:%s" % rev, email=True)


def retry_runner_roll(repo, log, state_dir=None, notifier=None, layout=None,
                      pipeline_class=None):
    state = load_state(state_dir)
    pending = state.get("runnerRoll")
    if not pending or pending.get("error") or not pending.get("commit"):
        return None
    if switch_state(state_dir)[0] != "armed" or not auto_roll_enabled(state_dir):
        return None
    lock = common.DeployLock(what="next_autodeploy runner roll %s" % pending.get("rev"))
    try:
        lock.acquire(wait=0)
    except common.LockBusy:
        return None
    result = {"target": pending["commit"], "rev": pending.get("rev", ""), "message": ""}
    try:
        pipeline = (pipeline_class or Pipeline)(repo, log, state_dir=state_dir,
                                                notifier=notifier, layout=layout)
        pipeline.roll_runners(pending["commit"], lock, result)
    finally:
        lock.release()
    state = load_state(state_dir)
    if state.get("runnerRoll", {}).get("commit") == pending["commit"]:
        if result.get("runnerRollPending"):
            state["runnerRoll"]["at"] = time.time()
        else:
            note_runner_roll(state, result)
        save_state(state, state_dir)
    if notifier is not None:
        notify_runner_roll(notifier, result)
    return result


def notifier_for(repo, say, layout=None):
    layout = layout or forkmesh_paths.resolve()
    try:
        hosts = fleet_hosts(Git(repo).show(MAIN_REF, "deploy/fleet.toml"))
        origin = hosts["app_host"]
    except (ToolError, KeyError, ValueError):
        origin = ""
    return Notifier(origin=origin, node=settings_node_name(layout),
                    key_path=layout.identity_key, say=say)


def run_pipeline(repo, target, mode, log, skip_tests=False, notifier=None,
                 state_dir=None, run=subprocess.run, popen=subprocess.Popen, layout=None):
    pipeline = Pipeline(repo, log, mode=mode, run=run, popen=popen, notifier=notifier,
                        layout=layout, skip_tests=skip_tests, state_dir=state_dir)
    try:
        result = pipeline.execute(target)
    except (ToolError, OSError, subprocess.SubprocessError, KeyError, ValueError) as error:
        result = {"target": target, "rev": target[:12], "ok": False, "deployed": False,
                  "roll": [], "message": str(error)}
        log.say("FAILED: %s" % error)
    record_outcome(result, notifier, log.say, state_dir=state_dir, mode=mode)
    log.say("result: %s: %s" % ("ok" if result["ok"] else "FAILED", result["message"]))
    return result


class Agent:
    def __init__(self, repo, root, layout=None, clock=time.time, notifier=None,
                 state_dir=None, pipeline=run_pipeline, logs=None):
        self.git = Git(repo)
        self.repo = Path(repo)
        self.root = Path(root)
        self.layout = layout
        self.clock = clock
        self.notifier = notifier
        self.state_dir = state_dir
        self.pipeline = pipeline
        self.logs = Path(logs or common.logs_dir() / "autodeploy")
        self.attached = {}
        self.workspaces = {}
        self.pending = {}
        self.first_request_at = None
        self.last_request_at = None
        self.main_tip = ""
        self.main_changed_at = clock()
        self.quiet_until = None
        self.worker = None
        self.current = None
        self.idle_log = Log(echo=True, clock=clock)
        self.runner_worker = None
        self.runner_retry_at = clock() + RUNNER_ROLL_RETRY_SECONDS

    def say(self, message):
        (self.current["log"] if self.current else self.idle_log).say(message)

    def attach(self, run):
        run_id = run["id"]
        self.workspaces[run_id] = run["workspace"]
        try:
            sink = RunSink(run_id, run["workspace"])
        except FileNotFoundError:
            return
        except (OSError, ToolError) as error:
            self.idle_log.say("run %s: not attached (%s)" % (run_id, error))
            self.attached[run_id] = None
            return
        self.attached[run_id] = sink
        meta = run["meta"]
        sink.write("node deploy agent: run %s, %s @ %s\n" % (
            run_id, meta.get("ref"), str(meta.get("commit"))[:12]))
        kind, detail = run_verdict(
            meta, lambda commit: self.git.is_ancestor(commit, self.main_tip or MAIN_REF))
        if kind != "request":
            sink.finish(kind == "skip", detail)
            self.idle_log.say("run %s: %s: %s" % (run_id, kind, detail))
            return
        now = self.clock()
        self.pending[run_id] = detail
        self.first_request_at = self.first_request_at or now
        self.last_request_at = now
        state, reason = switch_state(self.state_dir)
        if self.current:
            sink.write("a deploy of %s is in progress; following it\n" % self.current["rev"])
            self.current["log"].attach(sink)
        else:
            quiet = (self.quiet_until or 0) - now
            sink.write("queued: waiting for main to settle (%d s debounce)%s%s\n" % (
                DEBOUNCE_SECONDS,
                "; %d s left of the pause after the last deploy" % quiet
                if quiet > 0 else "",
                "" if state == "armed" else
                "; auto-deploy is %s (%s)" % (state, reason)))

    def finish_run(self, run_id, ok, message):
        sink = self.attached.get(run_id)
        if self.current:
            self.current["log"].detach(run_id)
        if sink is not None:
            sink.finish(ok, message)

    def tick(self):
        tip = self.git.rev(MAIN_REF)
        if tip and tip != self.main_tip:
            self.main_tip = tip
            self.main_changed_at = self.clock()
        live = {run["id"]: run for run in running_runs(self.root)}
        stale = [r for r in self.attached if r not in live or
                 self.workspaces.get(r) != live[r]["workspace"]]
        for run_id in stale:
            sink = self.attached.pop(run_id)
            self.workspaces.pop(run_id, None)
            if sink is not None:
                if self.current:
                    self.current["log"].detach(run_id)
                sink.close()
        for run_id, run in live.items():
            if run_id not in self.attached:
                self.attach(run)
        if self.worker is not None:
            if self.worker.is_alive():
                return
            self.complete()
        if not self.pending:
            self.maybe_retry_runner_roll()
            return
        state, reason = switch_state(self.state_dir)
        if state != "armed":
            for run_id in list(self.pending):
                self.finish_run(run_id, True, "auto-deploy is %s (%s); nothing was "
                                "deployed. Resume with next_autodeploy.py resume" % (
                                    state, reason))
            self.pending.clear()
            self.first_request_at = self.last_request_at = None
            return
        if self.quiet_until is not None and self.clock() < self.quiet_until:
            return
        if ready_to_start(self.clock(), self.first_request_at, self.last_request_at,
                          self.main_changed_at):
            self.start(self.main_tip)

    def start(self, target):
        rev = target[:12]
        stamp = time.strftime("%Y%m%d-%H%M%S", time.localtime(self.clock()))
        log = Log(self.logs / ("%s-%s.log" % (stamp, rev)), echo=True, clock=self.clock)
        self.current = {"target": target, "rev": rev, "log": log, "result": None}
        for run_id in self.pending:
            sink = self.attached.get(run_id)
            if sink is not None:
                log.attach(sink)
        log.say("deploy %s (main tip after %d pushes settled; log %s)" % (
            rev, len(self.pending), log.path))
        current = self.current

        def work():
            current["result"] = self.pipeline(
                self.repo, target, "auto", log, notifier=self.notifier,
                state_dir=self.state_dir)
        self.worker = threading.Thread(target=work, name="node-deploy", daemon=True)
        self.worker.start()

    def complete(self):
        current, self.current, self.worker = self.current, None, None
        result = current["result"] or {"ok": False, "message": "the deploy thread died"}
        target = current["target"]
        for run_id, commit in list(self.pending.items()):
            if commit == target or self.git.is_ancestor(commit, target):
                current["log"].detach(run_id)
                sink = self.attached.get(run_id)
                if sink is not None:
                    sink.finish(result["ok"], result["message"] if commit == target else
                                "%s (covers %s)" % (result["message"], commit[:12]))
                del self.pending[run_id]
        current["log"].close()
        if result.get("deployed"):
            self.quiet_until = self.clock() + DEPLOY_COOLDOWN_SECONDS
        if self.pending:
            self.first_request_at = self.last_request_at = self.clock()
        else:
            self.first_request_at = self.last_request_at = None

    def maybe_retry_runner_roll(self):
        if self.runner_worker is not None and self.runner_worker.is_alive():
            return
        if self.clock() < self.runner_retry_at:
            return
        self.runner_retry_at = self.clock() + RUNNER_ROLL_RETRY_SECONDS
        if not load_state(self.state_dir).get("runnerRoll"):
            return

        def work():
            try:
                retry_runner_roll(self.repo, self.idle_log, state_dir=self.state_dir,
                                  notifier=self.notifier, layout=self.layout)
            except (ToolError, OSError, subprocess.SubprocessError) as error:
                self.idle_log.say("runner roll retry failed: %s" % error)
        self.runner_worker = threading.Thread(target=work, name="runner-roll", daemon=True)
        self.runner_worker.start()

    def serve(self, sleep=time.sleep, once=False):
        self.idle_log.say("node deploy agent: watching %s for %s runs" % (
            self.root, WORKFLOW_PATH))
        while True:
            try:
                self.tick()
            except (ToolError, OSError, subprocess.SubprocessError) as error:
                self.idle_log.say("tick failed: %s" % error)
            if once:
                return
            sleep(POLL_SECONDS)


def render_unit(bin_dir: Path, python="/usr/bin/python3") -> str:
    return ("[Unit]\n"
            "Description=ForkMesh node deploy agent (Deploy nodes Actions runs)\n"
            "After=network-online.target\n"
            "Wants=network-online.target\n\n"
            "[Service]\n"
            "Type=simple\n"
            "Environment=PYTHONDONTWRITEBYTECODE=1\n"
            "Environment=PYTHONUNBUFFERED=1\n"
            "ExecStart=%s %s serve\n"
            "Restart=always\n"
            "RestartSec=10\n"
            "Nice=5\n\n"
            "[Install]\n"
            "WantedBy=default.target\n" % (python, bin_dir / "next_autodeploy.py"))


def install(layout=None, run=subprocess.run, user_units=None, enable=True, say=print) -> int:
    layout = layout or forkmesh_paths.resolve()
    here = Path(__file__).resolve().parent
    target = layout.node_deploy_bin
    target.mkdir(parents=True, exist_ok=True)
    sources = [(here / name, name) for name in INSTALLED_FILES]
    sources += [(here.parent / "deploy" / "core" / name, name) for name in INSTALLED_DEPLOY_FILES]
    for source, name in sources:
        temporary = target / (name + ".new")
        shutil.copyfile(source, temporary)
        os.chmod(temporary, 0o755 if name == "next_autodeploy.py" else 0o644)
        os.replace(temporary, target / name)
    units = layout.systemd_units
    units.mkdir(parents=True, exist_ok=True)
    unit = units / UNIT_NAME
    unit.write_text(render_unit(target), "utf-8")
    user_units = Path(user_units or Path(os.environ.get("XDG_CONFIG_HOME")
                                         or Path.home() / ".config") / "systemd" / "user")
    user_units.mkdir(parents=True, exist_ok=True)
    link = user_units / UNIT_NAME
    if link.is_symlink() and os.readlink(link) != str(unit):
        link.unlink()
    if not link.exists():
        link.symlink_to(unit)
    directory = common.autodeploy_dir()
    directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    if not enable:
        return 0
    for command in (["systemctl", "--user", "daemon-reload"],
                    ["systemctl", "--user", "enable", UNIT_NAME],
                    ["systemctl", "--user", "restart", UNIT_NAME]):
        completed = run(command, capture_output=True, text=True, timeout=60, check=False)
        if completed.returncode != 0:
            say("%s: %s" % (" ".join(command), completed.stderr.strip()))
            return 1
    say("installed %s and %s (auto-deploy is %s)" % (
        target, UNIT_NAME, switch_state()[0]))
    return 0


def resolve_commit(git, value) -> str:
    if not REV_RE.match(value or ""):
        raise GuardError("%r is not a commit id" % value)
    manifest = common.load_manifest()
    entry = (manifest.get("releases") or {}).get(value) or {}
    commit = git.rev(entry.get("commit") or "") or git.rev(value)
    if not commit:
        raise GuardError("%s is not a commit here and not in the deploy manifest" % value)
    return commit


def deploy_commit(git, value) -> str:
    if value == "main":
        value = git.rev(MAIN_REF)
        if not value:
            raise GuardError("this checkout has no main branch")
    return resolve_commit(git, value)


def status_report(repo, layout=None) -> dict:
    git = Git(repo)
    state, reason = switch_state()
    lock = common.DeployLock()
    report = {"switch": state, "reason": reason, "lock": lock.holder() or None,
              "state": load_state(), "main": git.rev(MAIN_REF)[:12]}
    try:
        hosts = fleet_hosts(git.show(MAIN_REF, "deploy/fleet.toml"))
        report["live"] = live_revs(hosts, offline_wait=0, unreachable_wait=0)
    except (ToolError, ValueError, KeyError) as error:
        report["live"] = {"error": str(error)}
    return report


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="Deploy main to the core nodes when served-stack paths change.")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("serve")
    sub.add_parser("status")
    p = sub.add_parser("pause")
    p.add_argument("reason", nargs="*")
    sub.add_parser("resume")
    sub.add_parser("disarm")
    p = sub.add_parser("deploy")
    p.add_argument("--commit", required=True)
    p.add_argument("--skip-tests", action="store_true")
    p = sub.add_parser("rollback")
    p.add_argument("--rev", required=True)
    p.add_argument("--run-tests", action="store_true")
    p = sub.add_parser("verify")
    p.add_argument("--rev", required=True)
    sub.add_parser("ack-roll")
    sub.add_parser("workflow-paths")
    p = sub.add_parser("install")
    p.add_argument("--no-enable", action="store_true")
    args = parser.parse_args(argv)
    layout = forkmesh_paths.resolve()
    repo = layout.source
    try:
        if args.command == "workflow-paths":
            for pattern in workflow_path_patterns():
                print('      - "%s"' % pattern)
            return 0
        if args.command == "pause":
            pause(" ".join(args.reason))
            print("auto-deploy paused")
            return 0
        if args.command == "resume":
            resume()
            print("auto-deploy armed")
            return 0
        if args.command == "disarm":
            disarm()
            print("auto-deploy disarmed")
            return 0
        if args.command == "ack-roll":
            state = load_state()
            pending = state.pop("roll", None) or {}
            state.pop("runnerRoll", None)
            if pending.get("since"):
                state["lastRoll"] = {"rev": pending["since"], "at": time.time()}
            save_state(state)
            print("node roll acknowledged")
            return 0
        if args.command == "status":
            print(json.dumps(status_report(repo, layout), indent=2, sort_keys=True))
            return 0
        if args.command == "install":
            return install(layout, enable=not args.no_enable)
        if args.command == "serve":
            agent = Agent(repo, actions_root(layout), layout=layout,
                          notifier=notifier_for(repo, print, layout))
            agent.notifier.say = agent.say
            agent.serve()
            return 0
        git = Git(repo)
        if args.command == "verify":
            hosts = fleet_hosts(git.show(MAIN_REF, "deploy/fleet.toml"))
            return 0 if verify(hosts, args.rev[:12], print, timeout=0) else 1
        commit = (deploy_commit(git, args.commit) if args.command == "deploy"
                  else resolve_commit(git, args.rev))
        mode = "manual" if args.command == "deploy" else "rollback"
        stamp = time.strftime("%Y%m%d-%H%M%S")
        log = Log(common.logs_dir() / "autodeploy" / ("%s-%s-%s.log" % (
            stamp, mode, commit[:12])))
        notifier = notifier_for(repo, log.say, layout)
        skip = args.skip_tests if mode == "manual" else not args.run_tests
        result = run_pipeline(repo, commit, mode, log, skip_tests=skip, notifier=notifier)
        log.close()
        return 0 if result["ok"] else 1
    except (ToolError, OSError) as error:
        print("next_autodeploy: %s" % error, file=sys.stderr)
        return 2 if isinstance(error, GuardError) else 1


if __name__ == "__main__":
    sys.exit(main())
