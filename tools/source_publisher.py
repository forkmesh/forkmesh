#!/usr/bin/env python3
from __future__ import annotations

import argparse
import base64
import contextlib
import fcntl
import hashlib
import http.server
import json
import os
import re
import secrets
import shutil
import socket
import subprocess
import sys
import threading
import time
import tomllib
import urllib.error
import urllib.request
from pathlib import Path
from urllib.parse import urlsplit

sys.path.insert(0, str(Path(__file__).resolve().parent))
import forkmesh_paths  # noqa: E402

SOURCE_UNIT = "forkmesh-source.service"
FANOUT_UNIT = "forkmesh-mirror-fanout.service"
BRANCH_DELETE_UNIT = "forkmesh-mirror-branch-deletes.service"
SYNC_REFSPECS = ("refs/heads/*:refs/heads/*", "refs/tags/*:refs/tags/*")
GATEWAY_REFSPECS = ("refs/heads/*:refs/heads/*", "refs/tags/*:refs/tags/*")
PROTECTED_REFS = ("refs/heads/main", "refs/heads/forkmesh/pulls")
PUSH_SIGNATURE_PREFIX = "forkmesh-git-push-v1"
TAG_MOVE_PREFIX = "forkmesh-tag-move-v1"
TAG_MOVE_PATH = "/api/git/tag-moves"
MAX_TAG_MOVES = 8
BRANCH_DELETE_PREFIX = "forkmesh-branch-delete-v1"
BRANCH_DELETE_PATH = "/api/git/branch-deletes"
TOMBSTONE_PREFIX = "refs/forkmesh/trash/deleted/"
MAX_BRANCH_DELETES = 20
BRANCH_DELETE_POLL_ROUNDS = 15
BRANCH_DELETE_POLL_SECONDS = 2
BRANCH_DELETE_RETRYABLE = ("merge_status_pending", "merge_node_unavailable")
UNDELETABLE_BRANCHES = ("main", "master", "HEAD")
CORES_MAIN_REF = "refs/remotes/forkmesh-cores/main"
CORES_MERGE_MESSAGE = "Merge pushes to main from the cores"
FLAGSHIP_REPOSITORY = "forkmesh/forkmesh"
EMPTY_OBJECT = re.compile(r"object file \S*/objects/([0-9a-f]{2})/([0-9a-f]{38,62}) is empty")
MIN_PASS_SPACING_SECONDS = 2
MAX_PASSES = 3
FANOUT_TIMEOUT_SECONDS = 12 * 60
GATEWAY_TIMEOUT_SECONDS = 10 * 60
GATEWAY_RETRY_SECONDS = 10
TRANSIENT_PUSH_ERROR = re.compile(
    r"RPC failed|remote end hung up|returned error: 50[234]|HTTP 50[234]"
    r"|Connection refused|Failed to connect|Could not resolve host|timed out",
    re.IGNORECASE)
REF_LOCK_RACE = re.compile(r"failed to update ref|cannot lock ref", re.IGNORECASE)
UNPACK_RACE = re.compile(r"unpacker error", re.IGNORECASE)
LOOPBACK_PROBE_SECONDS = 3
LOOPBACK_BLOCKED = (
    "the signing proxy could not connect over loopback (127.0.0.1, ::1, "
    "127.0.0.2; last port %d, timeout %d s); check the local firewall such "
    "as OpenSnitch and allow loopback TCP")
LOOPBACK_CONNECT_FAILED = re.compile(r"Failed to connect to (?:127\.0\.0\.[12]|::1) port (\d+)")
LOCK_WAIT_SECONDS = 14 * 60
MAX_PUSH_BODY_BYTES = 512 * 1024 * 1024
UNIT_NAMES = (
    "forkmesh-mirror-fanout.service",
    "forkmesh-mirror-fanout.timer",
    "forkmesh-source-publish.service",
    "forkmesh-source-publish.path",
    "forkmesh-source-guard.service",
    "forkmesh-source-guard.timer",
    "forkmesh-source-cores.service",
    "forkmesh-source-cores.timer",
    "forkmesh-mirror-branch-deletes.service",
)
INACTIVE_STATES = ("inactive", "failed")
DURATION = re.compile(r"^(\d+)([smhd]?)$")
REPO_PATH = re.compile(
    r"^/(?P<token>[A-Za-z0-9_-]+)/(?P<owner>[A-Za-z0-9._-]+)/"
    r"(?P<repo>[A-Za-z0-9._-]+?)(?:\.git)?/"
    r"(?P<rest>info/refs|git-receive-pack)(?P<query>\?.*)?$")


class PublishError(RuntimeError):
    pass


def layout():
    return forkmesh_paths.resolve()


def now_ms() -> int:
    return int(time.time() * 1000)


def git(args, cwd=None, git_dir=None, env=None, timeout=600, check=False):
    command = ["git"]
    if git_dir is not None:
        command += ["--git-dir", str(git_dir)]
    command += list(args)
    completed = subprocess.run(
        command, cwd=cwd, env=env, capture_output=True, text=True,
        timeout=timeout, check=False)
    if check and completed.returncode != 0:
        raise PublishError("%s failed (%d): %s" % (
            " ".join(command[:4]), completed.returncode,
            completed.stderr.strip()[-600:]))
    return completed


def ref_digest(git_dir) -> str:
    listing = git(["for-each-ref", "--format=%(objectname) %(refname)",
                   "refs/heads", "refs/tags"], git_dir=git_dir, check=True).stdout
    lines = sorted(line for line in listing.splitlines() if line.strip())
    return hashlib.sha256(("\n".join(lines) + "\n").encode()).hexdigest()


def rev(git_dir, ref) -> str:
    completed = git(["rev-parse", "--verify", "--quiet", ref + "^{commit}"],
                    git_dir=git_dir)
    return completed.stdout.strip() if completed.returncode == 0 else ""


def sync(source: Path, bare: Path) -> dict:
    if not (source / ".git").exists() or not bare.is_dir():
        return {"ok": False, "reason": "missing source or bare mirror",
                "updated": [], "rejected": []}
    completed = git(["fetch", "--porcelain", str(source), *SYNC_REFSPECS],
                    git_dir=bare, timeout=900)
    updated, rejected = [], []
    for line in completed.stdout.splitlines():
        parts = line[2:].split()
        if len(line) < 2 or line[1] != " " or len(parts) < 3:
            continue
        flag, ref = line[0], parts[-1]
        if flag == "!":
            rejected.append(ref)
        elif flag in ("*", "+", " ", "-", "t"):
            updated.append(ref)
    if completed.returncode != 0 and not rejected:
        for line in completed.stderr.splitlines():
            if "[rejected]" in line or "non-fast-forward" in line:
                rejected.append(line.strip())
    return {"ok": completed.returncode == 0, "updated": updated,
            "rejected": rejected,
            "error": completed.stderr.strip()[-600:] if completed.returncode else ""}


def empty_loose_objects(bare: Path) -> list[str]:
    found = []
    objects = bare / "objects"
    for folder in sorted(objects.glob("[0-9a-f][0-9a-f]")):
        for path in sorted(folder.iterdir()):
            if path.is_file() and path.stat().st_size == 0 \
                    and re.fullmatch(r"[0-9a-f]{38,62}", path.name):
                found.append(folder.name + path.name)
    return found


def restore_object(source: Path, bare: Path, oid: str) -> bool:
    kind = git(["cat-file", "-t", oid], cwd=source)
    if kind.returncode != 0 or kind.stdout.strip() not in ("blob", "tree", "commit", "tag"):
        return False
    body = subprocess.run(["git", "cat-file", kind.stdout.strip(), oid], cwd=source,
                          capture_output=True, timeout=120, check=False)
    if body.returncode != 0:
        return False
    written = subprocess.run(
        ["git", "--git-dir", str(bare), "hash-object", "-t", kind.stdout.strip(),
         "-w", "--stdin"], input=body.stdout, capture_output=True, timeout=120,
        check=False)
    return written.returncode == 0 and written.stdout.decode().strip() == oid


def heal_empty_objects(source: Path, bare: Path, quarantine: Path) -> dict:
    moved, restored = [], []
    for oid in empty_loose_objects(bare):
        path = bare / "objects" / oid[:2] / oid[2:]
        target = quarantine / oid[:2] / oid[2:]
        target.parent.mkdir(parents=True, exist_ok=True)
        os.replace(path, target)
        moved.append(oid)
        if restore_object(source, bare, oid):
            restored.append(oid)
    return {"moved": moved, "restored": restored,
            "missing": [oid for oid in moved if oid not in restored]}


def sync_healing(source: Path, bare: Path, quarantine: Path, log=print) -> dict:
    synced = sync(source, bare)
    if synced["ok"] or not EMPTY_OBJECT.search(synced.get("error", "")):
        return synced
    healed = heal_empty_objects(source, bare, quarantine)
    if not healed["moved"]:
        return synced
    log("sync: moved %d empty object file(s) from the bare mirror to %s and restored "
        "%d from the checkout" % (len(healed["moved"]), quarantine, len(healed["restored"])))
    for oid in healed["missing"]:
        log("sync: %s is not in the checkout either; the bare mirror lacks it" % oid)
    synced = sync(source, bare)
    synced["healed"] = healed
    return synced


def read_json(path: Path, default=None):
    try:
        return json.loads(path.read_text("utf-8"))
    except (OSError, ValueError):
        return default


def write_json(path: Path, data) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".new")
    temporary.write_text(json.dumps(data, indent=1, sort_keys=True) + "\n", "utf-8")
    os.replace(temporary, path)


class StateLock:
    def __init__(self, path: Path, wait_seconds: float = LOCK_WAIT_SECONDS,
                 sleep=time.sleep, clock=time.monotonic):
        self.path = path
        self.wait_seconds = wait_seconds
        self.sleep = sleep
        self.clock = clock
        self.handle = None

    def __enter__(self):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.handle = open(self.path, "a+")
        deadline = self.clock() + self.wait_seconds
        while True:
            try:
                fcntl.flock(self.handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
                return self
            except BlockingIOError:
                if self.clock() >= deadline:
                    self.handle.close()
                    raise PublishError("another publish pass holds %s" % self.path)
                self.sleep(1.0)

    def __exit__(self, *exc):
        fcntl.flock(self.handle, fcntl.LOCK_UN)
        self.handle.close()
        return False


def read_gateways(path: Path, default_key: Path) -> list[dict]:
    gateways = []
    try:
        text = path.read_text("utf-8")
    except OSError:
        return gateways
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split()
        if len(fields) < 2:
            continue
        url = fields[1]
        parsed = urlsplit(url)
        if parsed.scheme != "https" and not (
                parsed.scheme == "http"
                and parsed.hostname in ("127.0.0.1", "::1", "127.0.0.2", "localhost")):
            continue
        key = Path(os.path.expanduser(fields[2])) if len(fields) > 2 else default_key
        gateways.append({"name": fields[0], "url": url, "key": key})
    return gateways


def b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).decode().rstrip("=")


def push_signature_message(owner, repo, method, ts, body_sha256) -> bytes:
    return "\n".join((PUSH_SIGNATURE_PREFIX, owner.strip().lower(), repo.strip(),
                      method.strip().upper(), str(ts), body_sha256)).encode()


class Signer:
    def __init__(self, key_path: Path):
        from cryptography.hazmat.primitives import serialization
        from cryptography.hazmat.primitives.asymmetric.ed25519 import (
            Ed25519PrivateKey)
        key = serialization.load_pem_private_key(key_path.read_bytes(), None)
        if not isinstance(key, Ed25519PrivateKey):
            raise PublishError("%s is not an Ed25519 key" % key_path)
        self._key = key
        self.public_key = b64url(key.public_key().public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw))

    def sign(self, message: bytes) -> str:
        return b64url(self._key.sign(message))


def _read_request_body(handler) -> bytes:
    if handler.headers.get("Transfer-Encoding", "").lower() == "chunked":
        chunks, total = [], 0
        while True:
            size_line = handler.rfile.readline(1024)
            size = int(size_line.split(b";", 1)[0].strip() or b"0", 16)
            if size == 0:
                while handler.rfile.readline(1024) not in (b"\r\n", b"\n", b""):
                    pass
                break
            total += size
            if total > MAX_PUSH_BODY_BYTES:
                raise PublishError("push body is too large")
            chunks.append(handler.rfile.read(size))
            handler.rfile.readline(1024)
        return b"".join(chunks)
    length = int(handler.headers.get("Content-Length") or 0)
    if length > MAX_PUSH_BODY_BYTES:
        raise PublishError("push body is too large")
    return handler.rfile.read(length) if length else b""


class SigningProxy:
    def __init__(self, upstream_origin: str, signer: Signer, owner: str, repo: str,
                 opener=None, host="127.0.0.1"):
        self.host = host
        self.upstream_origin = upstream_origin.rstrip("/")
        self.signer = signer
        self.owner = owner
        self.repo = repo
        self.token = secrets.token_urlsafe(24)
        self.opener = opener or urllib.request.build_opener(_NoRedirect())
        self.requests = []
        proxy = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *args):
                return

            def do_GET(self):
                proxy.forward(self, "GET")

            def do_POST(self):
                proxy.forward(self, "POST")

        server_type = type("LoopbackHTTPServer", (http.server.ThreadingHTTPServer,),
                           {"address_family": socket.AF_INET6 if ":" in host else socket.AF_INET})
        self.server = server_type((host, 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    @property
    def port(self) -> int:
        return self.server.server_address[1]

    @property
    def url(self) -> str:
        host = "[%s]" % self.host if ":" in self.host else self.host
        return "http://%s:%d/%s/%s/%s.git" % (
            host, self.port, self.token, self.owner, self.repo)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *exc):
        self.server.shutdown()
        self.server.server_close()
        return False

    def forward(self, handler, method):
        match = REPO_PATH.match(handler.path)
        if (not match or not secrets.compare_digest(match.group("token"), self.token)
                or match.group("owner").lower() != self.owner.lower()
                or match.group("repo") != self.repo
                or (method == "GET") != (match.group("rest") == "info/refs")):
            self._reply(handler, 404, b"not found", "text/plain")
            return
        if method == "GET" and match.group("query") != "?service=git-receive-pack":
            self._reply(handler, 404, b"not found", "text/plain")
            return
        try:
            body = _read_request_body(handler) if method == "POST" else b""
        except (PublishError, ValueError):
            self._reply(handler, 413, b"push body is too large", "text/plain")
            return
        digest = hashlib.sha256(body).hexdigest()
        ts = str(now_ms())
        message = push_signature_message(self.owner, self.repo, method, ts, digest)
        headers = {
            "X-ForkMesh-Pubkey": self.signer.public_key,
            "X-ForkMesh-Ts": ts,
            "X-ForkMesh-Sig": self.signer.sign(message),
            "User-Agent": handler.headers.get("User-Agent") or "git/forkmesh-publisher",
            "Accept": handler.headers.get("Accept") or "*/*",
        }
        for name in ("Content-Type", "Content-Encoding", "Git-Protocol"):
            if handler.headers.get(name):
                headers[name] = handler.headers[name]
        if method == "POST":
            headers["X-ForkMesh-Body-Sha256"] = digest
        target = "%s/%s/%s.git/%s%s" % (
            self.upstream_origin, self.owner, self.repo, match.group("rest"),
            match.group("query") or "")
        self.requests.append((method, match.group("rest"), len(body)))
        request = urllib.request.Request(
            target, data=body if method == "POST" else None, method=method,
            headers=headers)
        try:
            response = self.opener.open(request, timeout=GATEWAY_TIMEOUT_SECONDS)
            status, payload = response.status, response.read()
            content_type = response.headers.get("Content-Type", "")
        except urllib.error.HTTPError as error:
            status, payload = error.code, error.read()
            content_type = error.headers.get("Content-Type", "text/plain")
        except (urllib.error.URLError, OSError) as error:
            self._reply(handler, 502, str(error).encode(), "text/plain")
            return
        self._reply(handler, status, payload, content_type)

    @staticmethod
    def _reply(handler, status, payload, content_type):
        handler.send_response(status)
        handler.send_header("Content-Type", content_type or "application/octet-stream")
        handler.send_header("Content-Length", str(len(payload)))
        handler.send_header("Cache-Control", "no-store")
        handler.end_headers()
        handler.wfile.write(payload)


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


def parse_push_porcelain(output: str) -> dict:
    updated, rejected, failed = [], [], []
    for line in output.splitlines():
        if "\t" not in line or len(line) < 2 or line[1] != "\t":
            continue
        flag = line[0]
        fields = line[2:].split("\t")
        target = fields[0].split(":")[-1]
        if flag == "!":
            rejected.append({"ref": target,
                             "reason": fields[1] if len(fields) > 1 else ""})
        elif flag in ("*", "+", " ", "-"):
            updated.append(target)
        elif flag == "=":
            continue
        else:
            failed.append(line)
    diverged = [item["ref"] for item in rejected if item["ref"] in PROTECTED_REFS]
    return {"updated": updated, "rejected": rejected, "diverged": diverged,
            "other": failed}


def tag_move_message(owner, repo, ref, target, ts) -> bytes:
    return "\n".join((TAG_MOVE_PREFIX, owner.strip().lower(), repo.strip(),
                      ref, target, str(ts))).encode()


def authorize_tag_move(origin, signer, owner, repo, ref, target, opener=None) -> dict:
    ts = str(now_ms())
    body = json.dumps({"owner": owner, "repo": repo, "ref": ref,
                       "target": target}).encode()
    request = urllib.request.Request(
        origin.rstrip("/") + TAG_MOVE_PATH, data=body, method="POST", headers={
            "Content-Type": "application/json",
            "User-Agent": "forkmesh-publisher",
            "X-ForkMesh-Pubkey": signer.public_key,
            "X-ForkMesh-Ts": ts,
            "X-ForkMesh-Sig": signer.sign(
                tag_move_message(owner, repo, ref, target, ts)),
        })
    try:
        response = (opener or urllib.request.build_opener(_NoRedirect())).open(
            request, timeout=60)
        status = response.status
        response.read()
    except urllib.error.HTTPError as error:
        status = error.code
    except (urllib.error.URLError, OSError) as error:
        return {"ref": ref, "target": target, "ok": False, "status": 0,
                "error": str(error)[-200:]}
    return {"ref": ref, "target": target, "ok": status == 200, "status": status}


def branch_delete_message(owner, repo, branch, expect, ts) -> bytes:
    return "\n".join((BRANCH_DELETE_PREFIX, owner.strip().lower(), repo.strip(),
                      branch, expect, str(ts))).encode()


def _post_branch_delete(origin, signer, owner, repo, branch, expect,
                        resume=None, opener=None) -> dict:
    ts = str(now_ms())
    payload = {"owner": owner, "repo": repo, "branch": branch, "expect": expect}
    payload.update(resume or {})
    body = json.dumps(payload).encode()
    request = urllib.request.Request(
        origin.rstrip("/") + BRANCH_DELETE_PATH, data=body, method="POST",
        headers={
            "Content-Type": "application/json",
            "User-Agent": "forkmesh-publisher",
            "X-ForkMesh-Pubkey": signer.public_key,
            "X-ForkMesh-Ts": ts,
            "X-ForkMesh-Sig": signer.sign(
                branch_delete_message(owner, repo, branch, expect, ts)),
        })
    try:
        response = (opener or urllib.request.build_opener(_NoRedirect())).open(
            request, timeout=120)
        status = response.status
        raw = response.read(64 * 1024)
    except urllib.error.HTTPError as error:
        status = error.code
        raw = error.read(64 * 1024)
    except (urllib.error.URLError, OSError) as error:
        return {"branch": branch, "expect": expect, "ok": False, "status": 0,
                "outcome": "unreachable", "error": str(error)[-200:],
                "answer": {}}
    try:
        answer = json.loads(raw.decode("utf-8") or "{}")
    except (UnicodeError, ValueError):
        answer = {}
    if not isinstance(answer, dict):
        answer = {}
    outcome = str(answer.get("error") if status >= 400 else
                  answer.get("status") or answer.get("error") or "")
    return {"branch": branch, "expect": expect, "status": status,
            "outcome": outcome,
            "ok": status == 200 and outcome in ("deleted", "already"),
            "message": str(answer.get("message") or "")[:200],
            "answer": answer}


def request_branch_delete(origin, signer, owner, repo, branch, expect,
                          opener=None, sleep=time.sleep) -> dict:
    result = _post_branch_delete(origin, signer, owner, repo, branch, expect,
                                 opener=opener)
    resume = None
    for _round in range(BRANCH_DELETE_POLL_ROUNDS):
        answer = result.get("answer") or {}
        if result["status"] == 202:
            resume = {name: str(answer[name]) for name in
                      ("requestId", "node", "pinBase") if answer.get(name)}
        elif not (result["status"] == 503 and resume
                  and result["outcome"] in BRANCH_DELETE_RETRYABLE):
            break
        if not resume:
            break
        sleep(BRANCH_DELETE_POLL_SECONDS)
        result = _post_branch_delete(origin, signer, owner, repo, branch,
                                     expect, resume=resume, opener=opener)
    result.pop("answer", None)
    return result


def deletable_branch(branch: str) -> bool:
    return bool(branch) and branch not in UNDELETABLE_BRANCHES \
        and not branch.startswith("forkmesh/")


def branch_tombstones(source: Path) -> dict:
    listed = _checkout_git(source, [
        "for-each-ref", "--format=%(refname) %(objectname)", TOMBSTONE_PREFIX])
    tombstones = {}
    for line in listed.stdout.splitlines():
        ref, _, oid = line.partition(" ")
        branch = ref[len(TOMBSTONE_PREFIX):]
        if ref.startswith(TOMBSTONE_PREFIX) and deletable_branch(branch):
            tombstones[branch] = oid.strip()
    return tombstones


def cores_branches(url: str) -> dict | None:
    env = dict(os.environ, GIT_TERMINAL_PROMPT="0", GIT_ASKPASS="true")
    listed = git(["ls-remote", url, "refs/heads/*"], env=env, timeout=120)
    if listed.returncode != 0:
        return None
    heads = {}
    for line in listed.stdout.splitlines():
        oid, _, ref = line.partition("\t")
        ref = ref.strip()
        if ref.startswith("refs/heads/") and re.fullmatch(r"[0-9a-f]{40,64}", oid):
            heads[ref[len("refs/heads/"):]] = oid
    return heads


def every_core_branches(source: Path, url: str) -> dict | None:
    merged = {}
    try:
        cores = core_read_urls(source)
    except Exception:
        cores = []
    for core in cores:
        listed = cores_branches(core["url"])
        if listed is None:
            continue
        for branch, oid in listed.items():
            merged.setdefault(branch, set()).add(oid)
    edge = cores_branches(url)
    if edge is None and not merged:
        return None
    for branch, oid in (edge or {}).items():
        merged.setdefault(branch, set()).add(oid)
    return merged


def deleted_branches_on_cores(source: Path, heads: dict) -> list:
    local = set(_checkout_git(source, [
        "for-each-ref", "--format=%(refname:lstrip=2)", "refs/heads/"]
    ).stdout.split())
    pending = []
    for branch, tombstone in sorted(branch_tombstones(source).items()):
        found = heads.get(branch) or ()
        if branch in local:
            continue
        for oid in sorted({found} if isinstance(found, str) else set(found)):
            if oid == tombstone or _checkout_git(source, [
                    "merge-base", "--is-ancestor", oid, tombstone]).returncode == 0:
                pending.append((branch, oid))
    return pending


def propagate_branch_deletions(source: Path, gateway: dict, opener=None,
                               heads=None, log=print, sleep=time.sleep) -> dict:
    parsed = urlsplit(gateway["url"])
    parts = [part for part in parsed.path.split("/") if part]
    if len(parts) != 2:
        return {"ran": False, "reason": "bad gateway URL"}
    owner, repo = parts[0], re.sub(r"\.git$", "", parts[1])
    if not branch_tombstones(source):
        return {"ran": False, "reason": "no deleted branches"}
    if heads is None:
        heads = every_core_branches(source, gateway["url"])
    if heads is None:
        log("gateway %s: could not list the cores' branches; deletions wait"
            % gateway["name"])
        return {"ran": False, "reason": "ls-remote failed"}
    pending = deleted_branches_on_cores(source, heads)
    if not pending:
        return {"ran": True, "pending": 0, "results": []}
    try:
        signer = Signer(gateway["key"])
    except (OSError, ValueError, PublishError) as error:
        return {"ran": False, "reason": "key: %s" % error}
    origin = "%s://%s" % (parsed.scheme, parsed.netloc)
    results = [request_branch_delete(origin, signer, owner, repo, branch, oid,
                                     opener=opener, sleep=sleep)
               for branch, oid in pending[:MAX_BRANCH_DELETES]]
    done = [item["branch"] for item in results if item["ok"]]
    failed = [item for item in results if not item["ok"]]
    log("gateway %s: deleted %d branch(es) the source deleted (%d left)%s" % (
        gateway["name"], len(done), len(pending) - len(done),
        "" if not done else ": " + ", ".join(done[:5])))
    for item in failed[:5]:
        log("gateway %s: deleting %s %s (HTTP %s %s)" % (
            gateway["name"], item["branch"],
            "still processing" if item["status"] == 202 else "refused",
            item["status"], item["outcome"]))
    return {"ran": True, "pending": len(pending), "results": results}


def refusal(gateway: dict, ref: str) -> str:
    for item in gateway.get("rejected", []):
        if item.get("ref") == ref and item.get("reason"):
            return item["reason"]
    return (gateway.get("error") or "no reason given").splitlines()[-1]


def moved_tags(result: dict) -> list:
    return [item["ref"] for item in result.get("rejected", [])
            if item["ref"].startswith("refs/tags/")
            and "already exists" in item.get("reason", "")][:MAX_TAG_MOVES]


def stale_branch(item: dict) -> bool:
    return (item["ref"].startswith("refs/heads/")
            and item["ref"] not in PROTECTED_REFS)


def _gateway_push(bare, url, refspecs, run):
    env = dict(os.environ, GIT_TERMINAL_PROMPT="0", GIT_ASKPASS="true")
    env["NO_PROXY"] = env["no_proxy"] = "127.0.0.1,::1,127.0.0.2"
    command = ["git", "--git-dir", str(bare),
               "-c", "credential.helper=", "-c", "http.postBuffer=%d" % MAX_PUSH_BODY_BYTES,
               "push", "--porcelain", url, *refspecs]
    return run(command, capture_output=True, text=True, env=env,
               timeout=GATEWAY_TIMEOUT_SECONDS, check=False)


def loopback_accepts(port: int, timeout: float = LOOPBACK_PROBE_SECONDS,
                     host="127.0.0.1") -> bool:
    try:
        socket.create_connection((host, port), timeout=timeout).close()
    except OSError:
        return False
    return True


def loopback_blocked_result(gateway: dict, port: int, signer: Signer) -> dict:
    result = parse_push_porcelain("")
    result.update({
        "name": gateway["name"], "url": gateway["url"], "ok": False,
        "transient": True, "loopbackBlocked": True, "stale": [],
        "tagMoves": [], "tagsMoved": [], "signer": signer.public_key[:8],
        "error": LOOPBACK_BLOCKED % (port, LOOPBACK_PROBE_SECONDS),
    })
    return result


def transient_push_failure(completed, result: dict) -> bool:
    return (completed.returncode != 0 and not result["updated"]
            and not result["rejected"] and not result["other"]
            and bool(TRANSIENT_PUSH_ERROR.search(completed.stderr or "")))


def ref_lock_race(result: dict) -> bool:
    return any(item["ref"] in PROTECTED_REFS and REF_LOCK_RACE.search(item["reason"])
               for item in result["rejected"])


def unpack_race(result: dict) -> bool:
    return not result["updated"] and any(
        UNPACK_RACE.search(item["reason"]) for item in result["rejected"])


def push_gateway(bare: Path, gateway: dict, run=subprocess.run, opener=None,
                 sleep=time.sleep, probe=loopback_accepts) -> dict:
    parsed = urlsplit(gateway["url"])
    parts = [part for part in parsed.path.split("/") if part]
    if len(parts) != 2:
        return {"name": gateway["name"], "ok": False,
                "error": "gateway URL must be <origin>/<owner>/<repo>.git"}
    owner, repo = parts[0], re.sub(r"\.git$", "", parts[1])
    try:
        signer = Signer(gateway["key"])
    except (OSError, ValueError, PublishError) as error:
        return {"name": gateway["name"], "ok": False, "error": "key: %s" % error}
    origin = "%s://%s" % (parsed.scheme, parsed.netloc)
    authorizations, moved = [], []
    with contextlib.ExitStack() as stack:
        available = False
        port = 0
        for host in ("127.0.0.1", "::1", "127.0.0.2"):
            try:
                proxy = stack.enter_context(SigningProxy(origin, signer, owner, repo, host=host))
            except OSError:
                continue
            port = proxy.port
            available = (probe(proxy.port, host=host) if probe is loopback_accepts
                         else probe(proxy.port))
            if available:
                break
        if not available:
            return loopback_blocked_result(gateway, port, signer)
        completed = _gateway_push(bare, proxy.url, GATEWAY_REFSPECS, run)
        result = parse_push_porcelain(completed.stdout)
        blocked = LOOPBACK_CONNECT_FAILED.search(completed.stderr or "")
        if blocked and int(blocked.group(1)) == proxy.port and not result["updated"]:
            return loopback_blocked_result(gateway, proxy.port, signer)
        if (transient_push_failure(completed, result) or ref_lock_race(result)
                or unpack_race(result)):
            sleep(GATEWAY_RETRY_SECONDS)
            completed = _gateway_push(bare, proxy.url, GATEWAY_REFSPECS, run)
            result = parse_push_porcelain(completed.stdout)
        for ref in moved_tags(result):
            target = git(["rev-parse", "--verify", "--quiet", ref],
                         git_dir=bare).stdout.strip()
            if target:
                authorizations.append(authorize_tag_move(
                    origin, signer, owner, repo, ref, target, opener=opener))
        allowed = [item["ref"] for item in authorizations if item["ok"]]
        if allowed:
            forced = _gateway_push(
                bare, proxy.url, ["+%s:%s" % (ref, ref) for ref in allowed], run)
            second = parse_push_porcelain(forced.stdout)
            moved = [ref for ref in second["updated"] if ref in allowed]
    rejected = [item for item in result["rejected"] if item["ref"] not in moved]
    blocking = [item for item in rejected if not stale_branch(item)]
    transport_ok = completed.returncode == 0 or bool(result["rejected"])
    ok = transport_ok and not blocking and not result["other"]
    result.update({
        "name": gateway["name"], "url": gateway["url"],
        "ok": ok,
        "transient": not ok and transient_push_failure(completed, result),
        "updated": result["updated"] + moved,
        "rejected": rejected,
        "stale": [item["ref"] for item in rejected if stale_branch(item)],
        "tagMoves": authorizations,
        "tagsMoved": moved,
        "signer": signer.public_key[:8],
        "error": "" if ok else completed.stderr.strip()[-600:],
    })
    return result


def _checkout_git(source, args, timeout=900):
    env = dict(os.environ, GIT_TERMINAL_PROMPT="0", GIT_ASKPASS="true")
    return git(args, cwd=source, env=env, timeout=timeout)


def main_worktree(source: Path) -> str:
    listing = _checkout_git(source, ["worktree", "list", "--porcelain"]).stdout
    path = ""
    for line in listing.splitlines():
        if line.startswith("worktree "):
            path = line[len("worktree "):]
        elif line == "branch refs/heads/main":
            return path
    return ""


def cores_hold(source: Path, pushed: str, cores: str) -> bool:
    return not pushed or bool(cores) and _checkout_git(
        source, ["merge-base", "--is-ancestor", pushed, cores]).returncode == 0


def cores_ref_contained(source: Path, fetch_url: str, ref: str,
                        pushed: str = "") -> bool:
    local_ref = "refs/remotes/forkmesh-cores/" + ref[len("refs/heads/"):]
    fetched = _checkout_git(source, [
        "fetch", "--quiet", "--no-tags", fetch_url, "+%s:%s" % (ref, local_ref)])
    if fetched.returncode != 0:
        return False
    cores = _checkout_git(source, ["rev-parse", "--verify", "--quiet",
                                   local_ref + "^{commit}"]).stdout.strip()
    local = _checkout_git(source, ["rev-parse", "--verify", "--quiet",
                                   ref + "^{commit}"]).stdout.strip()
    return bool(cores and local) and _checkout_git(
        source, ["merge-base", "--is-ancestor", cores, local]).returncode == 0 \
        and cores_hold(source, pushed, cores)


def absorb_without_worktree(source: Path, local: str, cores: str) -> dict:
    if _checkout_git(source, ["merge-base", "--is-ancestor", local, cores]).returncode == 0:
        target, mode = cores, "fast-forward"
    else:
        trial = _checkout_git(source, ["merge-tree", "--write-tree", local, cores])
        if trial.returncode != 0:
            return {"absorbed": False, "cores": cores,
                    "reason": "main and the cores' main conflict; merge %s by hand" % cores[:12]}
        tree = trial.stdout.split("\n", 1)[0].strip()
        made = _checkout_git(source, ["commit-tree", tree, "-p", local, "-p", cores,
                                      "-m", CORES_MERGE_MESSAGE])
        target, mode = made.stdout.strip(), "merge"
        if made.returncode != 0 or not target:
            return {"absorbed": False, "cores": cores,
                    "reason": "merge failed: %s" % made.stderr.strip()[-300:]}
    moved = _checkout_git(source, ["update-ref", "-m", CORES_MERGE_MESSAGE,
                                   "refs/heads/main", target, local])
    if moved.returncode != 0:
        return {"absorbed": False, "cores": cores,
                "reason": "main moved meanwhile: %s" % moved.stderr.strip()[-300:]}
    _checkout_git(source, ["hook", "run", "--ignore-missing", "post-merge", "--", "0"])
    return {"absorbed": True, "mode": mode, "cores": cores}


def absorb_cores(source: Path, fetch_url: str) -> dict:
    fetched = _checkout_git(source, [
        "fetch", "--quiet", "--no-tags", fetch_url,
        "+refs/heads/main:" + CORES_MAIN_REF])
    if fetched.returncode != 0:
        return {"absorbed": False, "reason": "fetch failed: %s" % (
            fetched.stderr.strip()[-300:])}
    cores = _checkout_git(source, ["rev-parse", "--verify", "--quiet",
                                   CORES_MAIN_REF + "^{commit}"]).stdout.strip()
    local = _checkout_git(source, ["rev-parse", "--verify", "--quiet",
                                   "refs/heads/main^{commit}"]).stdout.strip()
    if not cores or not local:
        return {"absorbed": False, "reason": "main is missing"}
    if _checkout_git(source, ["merge-base", "--is-ancestor", cores, local]).returncode == 0:
        return {"absorbed": False, "reason": "already contained", "cores": cores}
    worktree = main_worktree(source)
    if not worktree:
        return absorb_without_worktree(source, local, cores)
    if _checkout_git(worktree, ["rev-parse", "--verify", "--quiet",
                                "MERGE_HEAD"]).returncode == 0:
        return {"absorbed": False, "reason": "a merge is in progress", "cores": cores}
    if _checkout_git(source, ["merge-base", "--is-ancestor", local, cores]).returncode == 0:
        merged = _checkout_git(worktree, ["merge", "--ff-only", "--quiet", cores])
        if merged.returncode != 0:
            return {"absorbed": False, "cores": cores,
                    "reason": "fast-forward failed: %s" % merged.stderr.strip()[-300:]}
        return {"absorbed": True, "mode": "fast-forward", "cores": cores}
    trial = _checkout_git(source, ["merge-tree", "--write-tree", "--name-only", local, cores])
    if trial.returncode != 0:
        return {"absorbed": False, "cores": cores,
                "reason": "main and the cores' main conflict; merge %s by hand" % cores[:12]}
    merged = _checkout_git(worktree, ["merge", "--no-ff", "--no-edit", "--quiet",
                                      "-m", CORES_MERGE_MESSAGE, cores])
    if merged.returncode != 0:
        _checkout_git(worktree, ["merge", "--abort"])
        return {"absorbed": False, "cores": cores,
                "reason": "merge failed: %s" % merged.stderr.strip()[-300:]}
    return {"absorbed": True, "mode": "merge", "cores": cores}


def cores_main(url: str) -> str:
    env = dict(os.environ, GIT_TERMINAL_PROMPT="0", GIT_ASKPASS="true")
    listed = git(["ls-remote", url, "refs/heads/main"], env=env, timeout=60)
    if listed.returncode != 0:
        return ""
    for line in listed.stdout.splitlines():
        oid, _, ref = line.partition("\t")
        if ref.strip() == "refs/heads/main" and re.fullmatch(r"[0-9a-f]{40,64}", oid):
            return oid
    return ""


def checkout_contains(source: Path, oid: str) -> bool:
    if git(["cat-file", "-e", oid + "^{commit}"], cwd=source).returncode != 0:
        return False
    return git(["merge-base", "--is-ancestor", oid, "refs/heads/main"],
               cwd=source).returncode == 0


def quarantine_dir(paths, clock=time.time) -> Path:
    return paths.publisher_state / "quarantine" / time.strftime(
        "%Y-%m-%d", time.gmtime(clock()))


def core_read_urls(source: Path) -> list[dict]:
    try:
        fleet = tomllib.loads((source / "deploy" / "fleet.toml").read_text("utf-8"))
    except (OSError, tomllib.TOMLDecodeError):
        return []
    domain = str(fleet.get("domain") or "").strip()
    if not re.fullmatch(r"[a-z0-9.-]+", domain):
        return []
    cores = []
    for node in fleet.get("nodes") or []:
        name = str(node.get("name") or "")
        if node.get("role", "core") != "core" or not re.fullmatch(r"[a-z][a-z0-9-]*", name):
            continue
        cores.append({"name": "core " + name,
                      "url": "https://%s.%s/%s.git" % (name, domain, FLAGSHIP_REPOSITORY)})
    return cores


def take_core_main(paths, core: dict, oid: str, log=print) -> bool:
    with StateLock(paths.publisher_state / "publish.lock"):
        outcome = absorb_cores(paths.source, core["url"])
    if outcome.get("absorbed"):
        log("%s: took main %s into the checkout (%s)" % (
            core["name"], oid[:12], outcome["mode"]))
        return True
    if outcome.get("reason") != "already contained":
        log("%s DIVERGED: main %s is not in the checkout; %s" % (
            core["name"], oid[:12], outcome.get("reason")))
    return False


def watch_cores(paths, run=subprocess.run, log=print) -> int:
    seen_path = paths.publisher_state / "cores-watch.json"
    seen = read_json(seen_path, {}) or {}
    for core in core_read_urls(paths.source):
        oid = cores_main(core["url"])
        if not oid or checkout_contains(paths.source, oid) or seen.get(core["name"]) == oid:
            continue
        log("%s: main moved to %s, which the checkout lacks; taking it" % (
            core["name"], oid[:12]))
        take_core_main(paths, core, oid, log=log)
        code = publish(paths, run=run, log=log)
        if not checkout_contains(paths.source, oid):
            seen[core["name"]] = oid
            write_json(seen_path, seen)
            log("%s: could not take %s; merge it into main by hand" % (
                core["name"], oid[:12]))
        return code
    for gateway in read_gateways(paths.fanout_gateways, paths.identity_key):
        cores = cores_main(gateway["url"])
        if not cores or checkout_contains(paths.source, cores):
            continue
        if seen.get(gateway["name"]) == cores:
            continue
        log("gateway %s: the cores' main moved to %s, which the checkout lacks; "
            "publishing now to take it" % (gateway["name"], cores[:12]))
        code = publish(paths, run=run, log=log)
        if not checkout_contains(paths.source, cores):
            seen[gateway["name"]] = cores
            write_json(seen_path, seen)
            log("gateway %s: could not take %s; the 2-minute pass keeps trying" % (
                gateway["name"], cores[:12]))
        return code
    return 0


def run_fanout(script: Path, run=subprocess.run) -> dict:
    if not script.is_file():
        return {"ran": False, "ok": True, "code": 0}
    completed = run([str(script)], timeout=FANOUT_TIMEOUT_SECONDS, check=False)
    return {"ran": True, "ok": completed.returncode == 0, "code": completed.returncode}


def fanout_script(paths) -> Path:
    configured = os.environ.get("FORKMESH_FANOUT_SCRIPT")
    if configured:
        return Path(configured)
    return paths.home_dir / "bin" / "forkmesh-mirror-fanout.sh"


def publish(paths, if_changed=False, run=subprocess.run, sleep=time.sleep,
            clock=time.time, log=print) -> int:
    state_dir = paths.publisher_state
    status_path = state_dir / "status.json"
    with StateLock(state_dir / "publish.lock"):
        status = read_json(status_path, {}) or {}
        exit_code = 0
        for attempt in range(MAX_PASSES):
            synced = sync_healing(paths.source, paths.bare_mirror,
                                  quarantine_dir(paths, clock), log=log)
            if not synced["ok"] and not synced["rejected"]:
                log("sync FAILED: the checkout's refs did not reach the bare mirror (%s)" % (
                    (synced.get("error") or "unknown error").splitlines()[-1]))
            if synced["updated"]:
                log("sync: %d ref(s) moved from %s" % (len(synced["updated"]), paths.source))
            for ref in synced["rejected"]:
                log("sync: %s not fast-forward in the checkout; left as is" % ref)
            if not paths.bare_mirror.is_dir():
                log("no bare mirror at %s" % paths.bare_mirror)
                return 1
            digest = ref_digest(paths.bare_mirror)
            if attempt > 0 and digest == status.get("digest"):
                break
            if attempt == 0 and if_changed and digest == status.get("digest") \
                    and status.get("ok"):
                log("up to date: %s" % digest[:12])
                break
            wait = MIN_PASS_SPACING_SECONDS - (clock() - float(status.get("startedAt", 0)))
            if 0 < wait <= MIN_PASS_SPACING_SECONDS:
                sleep(wait)
            started = clock()
            fanout = run_fanout(fanout_script(paths), run=run)
            gateways = [push_gateway(paths.bare_mirror, gateway, run=run)
                        for gateway in read_gateways(
                            paths.fanout_gateways, paths.identity_key)]
            for gateway in gateways:
                if "refs/heads/main" in gateway.get("diverged", []):
                    outcome = absorb_cores(paths.source, gateway["url"])
                    gateway["absorb"] = outcome
                    if outcome.get("reason") == "already contained" and not cores_hold(
                            paths.source, rev(paths.bare_mirror, "refs/heads/main"),
                            outcome.get("cores", "")):
                        outcome.update(behind=True, reason="the cores are behind and "
                                       "refused main: %s" % refusal(gateway, "refs/heads/main"))
                    if outcome.get("reason") == "already contained":
                        gateway["outrun"] = True
                        gateway["diverged"] = [
                            ref for ref in gateway["diverged"]
                            if ref != "refs/heads/main"]
                        gateway["rejected"] = [
                            item for item in gateway.get("rejected", [])
                            if item.get("ref") != "refs/heads/main"]
                for ref in [ref for ref in gateway.get("diverged", [])
                            if ref != "refs/heads/main"]:
                    if cores_ref_contained(paths.source, gateway["url"], ref,
                                           rev(paths.bare_mirror, ref)):
                        gateway["outrun"] = True
                        gateway.setdefault("outrunRefs", []).append(ref)
                        gateway["diverged"] = [
                            item for item in gateway["diverged"] if item != ref]
                        gateway["rejected"] = [
                            item for item in gateway.get("rejected", [])
                            if item.get("ref") != ref]
                if gateway.get("outrun") and not [
                        item for item in gateway.get("rejected", [])
                        if not stale_branch(item)] and not gateway.get("other"):
                    gateway["ok"] = True
                    gateway["error"] = ""
                log("gateway %s: %s, %d ref(s) updated, %d rejected%s" % (
                    gateway["name"],
                    "ok" if gateway["ok"] else
                    "loopback blocked" if gateway.get("loopbackBlocked") else
                    "unreachable after a retry" if gateway.get("transient") else "FAILED",
                    len(gateway.get("updated", [])), len(gateway.get("rejected", [])),
                    (" (" + gateway["error"].splitlines()[-1] + ")")
                    if gateway.get("error") else ""))
                for ref in gateway.get("tagsMoved", []):
                    log("gateway %s: moved %s with an audited tag move" % (
                        gateway["name"], ref))
                for item in gateway.get("tagMoves", []):
                    if not item.get("ok"):
                        log("gateway %s: tag move for %s refused (HTTP %s)" % (
                            gateway["name"], item["ref"], item.get("status")))
                if gateway.get("stale"):
                    log("gateway %s: %d branch(es) left at the cores' value: %s" % (
                        gateway["name"], len(gateway["stale"]),
                        ", ".join(gateway["stale"][:5])))
                if gateway.get("outrun"):
                    log("gateway %s: %s already past this pass (a newer pass "
                        "published it); nothing to do" % (
                            gateway["name"],
                            ", ".join(gateway.get("outrunRefs") or ["main was"])))
                if "refs/heads/main" in gateway.get("diverged", []):
                    outcome = gateway["absorb"]
                    if outcome.get("absorbed"):
                        log("gateway %s: took main %s from the cores (%s)" % (
                            gateway["name"], outcome["cores"][:12], outcome["mode"]))
                    elif outcome.get("behind"):
                        log("gateway %s: %s" % (gateway["name"], outcome["reason"]))
                    else:
                        log("gateway %s DIVERGED: the cores hold commits the source "
                            "lacks; %s" % (gateway["name"], outcome.get("reason")))
                elif gateway.get("diverged"):
                    log("gateway %s DIVERGED: %s rejected as non-fast-forward; the "
                        "cores hold commits the source lacks" % (
                            gateway["name"], ", ".join(gateway["diverged"])))
            ok = (fanout["ok"] and all(gateway["ok"] for gateway in gateways)
                  and (synced["ok"] or bool(synced["rejected"])))
            status = {
                "digest": digest, "ok": ok, "startedAt": started,
                "finishedAt": clock(), "sourceMain": rev(paths.bare_mirror, "refs/heads/main"),
                "sync": synced, "fanout": fanout, "gateways": gateways,
                "flagshipActive": unit_state(SOURCE_UNIT, run=run) not in INACTIVE_STATES,
            }
            write_json(status_path, status)
            exit_code = 0 if ok else (fanout["code"] or 1)
    notify_publish_failure(paths, status, log=log)
    start_branch_deletes(paths, run=run, log=log)
    return exit_code


def notify_publish_failure(paths, status, log=print, opener=None, clock=time.time):
    alert_path = paths.publisher_state / "publish-alert.json"
    if status.get("ok"):
        write_json(alert_path, {})
        return
    failed = [item for item in status.get("gateways", []) if not item.get("ok")]
    cause = "; ".join(item.get("error") or "gateway push failed" for item in failed)
    cause = cause or status.get("sync", {}).get("error") or "mirror fan-out failed"
    fix = ("Allow local TCP to 127.0.0.1, ::1 or 127.0.0.2 in OpenSnitch or "
           "the local firewall; reinstall with python3 tools/source_publisher.py install. "
           if any(item.get("loopbackBlocked") for item in failed) else
           "Check the publisher journal, network access and the reported rejection. ")
    body = ("The website repository may be behind the pushed code. Cause: %s\n"
            "Fix: %sRetry: python3 tools/source_publisher.py publish\n"
            "Check: python3 tools/source_publisher.py status; "
            "journalctl --user -u forkmesh-source-publish.service -n 50" % (cause, fix))
    log("ERROR: " + body)
    previous = read_json(alert_path, {}) or {}
    stable_body = re.sub(r"last port \d+", "last port <local>", body)
    digest = hashlib.sha256(stable_body.encode()).hexdigest()
    if previous.get("digest") == digest and clock() - previous.get("at", 0) < 3600:
        return
    try:
        match = re.search(r"(?m)^nodeName=([a-z0-9_-]{1,40})\s*$",
                          paths.settings_file.read_text("utf-8"))
        if not match:
            raise PublishError("no node identity name in settings")
        node, ts = match.group(1), str(now_ms())
        fields = ["ping", "source-publisher-failure", "Repository publishing failed", body[:2000]]
        digest_fields = hashlib.sha256(json.dumps(fields, separators=(",", ":")).encode()).hexdigest()
        signer = Signer(paths.identity_key)
        canonical = "\n".join(["forkmesh-admin-operator-alert-v1", node, ts, digest_fields])
        signature = base64.b64encode(signer._key.sign(canonical.encode())).decode()
        gateways = read_gateways(paths.fanout_gateways, paths.identity_key)
        origin = urlsplit(gateways[0]["url"])
        query = urllib.parse.urlencode({"node": node, "ts": ts, "sig": signature})
        request = urllib.request.Request(
            "%s://%s/api/accounts/admin-operator-alert?%s" % (origin.scheme, origin.netloc, query),
            data=json.dumps(dict(zip(("severity", "key", "title", "body"), fields))).encode(),
            headers={"Content-Type": "application/json"}, method="POST")
        with (opener or urllib.request.urlopen)(request, timeout=20) as response:
            reply = json.loads(response.read() or b"{}")
        write_json(alert_path, {"digest": digest, "at": clock()})
        log("notify: %s operator Ping(s)" % reply.get("pings", 0))
    except (OSError, ValueError, PublishError, IndexError) as error:
        log("notify: publishing failure Ping could not be sent: %s" % error)


def delete_branches(paths, log=print) -> int:
    try:
        with StateLock(paths.publisher_state / "branch-deletes.lock",
                       wait_seconds=0):
            for gateway in read_gateways(paths.fanout_gateways,
                                         paths.identity_key):
                try:
                    propagate_branch_deletions(paths.source, gateway, log=log)
                except Exception as error:
                    log("gateway %s: branch deletions failed: %s" % (
                        gateway["name"], str(error)[-200:]))
    except PublishError as error:
        log("branch deletions: %s" % error)
    return 0


def start_branch_deletes(paths, run=subprocess.run, log=print) -> None:
    try:
        started = run(["systemctl", "--user", "start", "--no-block",
                       BRANCH_DELETE_UNIT],
                      capture_output=True, text=True, timeout=60, check=False)
        if started.returncode == 0:
            return
    except (OSError, subprocess.TimeoutExpired):
        pass
    delete_branches(paths, log=log)


def unit_state(unit: str, run=subprocess.run) -> str:
    try:
        completed = run(["systemctl", "--user", "show", "-p", "ActiveState",
                         "-p", "LoadState", unit],
                        capture_output=True, text=True, timeout=30, check=False)
    except (OSError, subprocess.TimeoutExpired):
        return "unknown"
    values = dict(line.split("=", 1) for line in completed.stdout.splitlines()
                  if "=" in line)
    if completed.returncode != 0 or "ActiveState" not in values:
        return "unknown"
    if values.get("LoadState") != "loaded":
        return "not-found"
    return values["ActiveState"]


def parse_duration(text: str) -> int:
    match = DURATION.match(text.strip().lower())
    if not match:
        raise PublishError("duration must look like 90m, 2h or 1d")
    scale = {"": 60, "s": 1, "m": 60, "h": 3600, "d": 86400}[match.group(2)]
    return int(match.group(1)) * scale


def active_hold(path: Path, clock=time.time):
    hold = read_json(path)
    if not isinstance(hold, dict):
        return None
    try:
        if float(hold.get("until", 0)) > clock():
            return hold
    except (TypeError, ValueError):
        return None
    return None


def guard(paths, run=subprocess.run, clock=time.time, log=print) -> int:
    state = unit_state(SOURCE_UNIT, run=run)
    if state not in INACTIVE_STATES:
        return 0
    hold = active_hold(paths.publisher_state / "source-hold.json", clock=clock)
    if hold:
        log("%s is %s and held until %s (%s); leaving it stopped" % (
            SOURCE_UNIT, state,
            time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(float(hold["until"]))),
            hold.get("reason") or "no reason given"))
        return 0
    log("%s is %s with no hold; starting it (hold it first to keep it stopped: "
        "tools/source_publisher.py hold 2h --reason ...)" % (SOURCE_UNIT, state))
    run(["systemctl", "--user", "reset-failed", SOURCE_UNIT],
        capture_output=True, text=True, timeout=30, check=False)
    started = run(["systemctl", "--user", "start", "--no-block", SOURCE_UNIT],
                  capture_output=True, text=True, timeout=60, check=False)
    return 0 if started.returncode == 0 else 1


def hold(paths, duration: str, reason: str, clock=time.time) -> dict:
    seconds = parse_duration(duration)
    record = {"until": clock() + seconds, "reason": reason, "setAt": clock()}
    write_json(paths.publisher_state / "source-hold.json", record)
    return record


def unhold(paths) -> bool:
    try:
        (paths.publisher_state / "source-hold.json").unlink()
        return True
    except FileNotFoundError:
        return False


def status_report(paths, run=subprocess.run) -> dict:
    last = read_json(paths.publisher_state / "status.json", {}) or {}
    source_main = ""
    if (paths.source / ".git").exists():
        completed = git(["rev-parse", "--verify", "--quiet", "refs/heads/main"],
                        cwd=paths.source)
        source_main = completed.stdout.strip()
    return {
        "checkoutMain": source_main,
        "bareMain": rev(paths.bare_mirror, "refs/heads/main") if paths.bare_mirror.is_dir() else "",
        "lastPass": {key: last.get(key) for key in (
            "ok", "digest", "sourceMain", "startedAt", "finishedAt")},
        "gateways": [{"name": item.get("name"), "ok": item.get("ok"),
                      "diverged": item.get("diverged")}
                     for item in last.get("gateways") or []],
        "fanout": read_json(paths.home_root("state") / "fanout" / "status.json"),
        "flagship": unit_state(SOURCE_UNIT, run=run),
        "hold": active_hold(paths.publisher_state / "source-hold.json"),
    }


def render_units(paths, python="/usr/bin/python3") -> dict:
    tool = paths.publisher_bin / "source_publisher.py"
    source = paths.source
    return {
        "forkmesh-mirror-fanout.service": (
            "[Unit]\n"
            "Description=ForkMesh source publisher: checkout to bare mirror, fleet fan-out, gateway pushes\n"
            "StartLimitIntervalSec=0\n\n"
            "[Service]\n"
            "Type=oneshot\n"
            "Environment=PYTHONDONTWRITEBYTECODE=1\n"
            "ExecStart=%s %s publish\n"
            "TimeoutStartSec=15min\n"
            "Nice=5\n" % (python, tool)),
        "forkmesh-mirror-fanout.timer": (
            "[Unit]\n"
            "Description=ForkMesh source publisher full pass every 2 minutes\n\n"
            "[Timer]\n"
            "OnBootSec=2min\n"
            "OnUnitActiveSec=2min\n"
            "AccuracySec=30s\n\n"
            "[Install]\n"
            "WantedBy=timers.target\n"),
        "forkmesh-source-publish.service": (
            "[Unit]\n"
            "Description=ForkMesh source publisher pass after a source ref change\n"
            "StartLimitIntervalSec=0\n\n"
            "[Service]\n"
            "Type=oneshot\n"
            "Environment=PYTHONDONTWRITEBYTECODE=1\n"
            "ExecStart=%s %s publish --if-changed\n"
            "TimeoutStartSec=15min\n"
            "Nice=5\n" % (python, tool)),
        "forkmesh-source-publish.path": (
            "[Unit]\n"
            "Description=Publish ForkMesh source refs when they change\n\n"
            "[Path]\n"
            "PathChanged=%s\n"
            "PathChanged=%s\n"
            "PathChanged=%s\n"
            "PathChanged=%s\n"
            "Unit=forkmesh-source-publish.service\n\n"
            "[Install]\n"
            "WantedBy=default.target\n" % (
                source / ".git" / "refs" / "heads", source / ".git" / "refs" / "tags",
                source / ".git" / "packed-refs", paths.action_spool)),
        "forkmesh-source-guard.service": (
            "[Unit]\n"
            "Description=Restart the ForkMesh source node unless it is held stopped\n\n"
            "[Service]\n"
            "Type=oneshot\n"
            "Environment=PYTHONDONTWRITEBYTECODE=1\n"
            "ExecStart=%s %s guard\n" % (python, tool)),
        "forkmesh-source-guard.timer": (
            "[Unit]\n"
            "Description=Check the ForkMesh source node every minute\n\n"
            "[Timer]\n"
            "OnBootSec=3min\n"
            "OnUnitActiveSec=1min\n"
            "AccuracySec=15s\n\n"
            "[Install]\n"
            "WantedBy=timers.target\n"),
        "forkmesh-source-cores.service": (
            "[Unit]\n"
            "Description=ForkMesh source publisher pass when the cores' main moved\n"
            "StartLimitIntervalSec=0\n\n"
            "[Service]\n"
            "Type=oneshot\n"
            "Environment=PYTHONDONTWRITEBYTECODE=1\n"
            "ExecStart=%s %s watch-cores\n"
            "TimeoutStartSec=15min\n"
            "Nice=5\n" % (python, tool)),
        "forkmesh-mirror-branch-deletes.service": (
            "[Unit]\n"
            "Description=ForkMesh source publisher: delete on the cores the branches the source deleted\n"
            "StartLimitIntervalSec=0\n\n"
            "[Service]\n"
            "Type=oneshot\n"
            "Environment=PYTHONDONTWRITEBYTECODE=1\n"
            "ExecStart=%s %s delete-branches\n"
            "TimeoutStartSec=15min\n"
            "Nice=5\n" % (python, tool)),
        "forkmesh-source-cores.timer": (
            "[Unit]\n"
            "Description=Check the cores' main for merges made on the web every 20 seconds\n\n"
            "[Timer]\n"
            "OnBootSec=1min\n"
            "OnUnitActiveSec=20s\n"
            "AccuracySec=5s\n\n"
            "[Install]\n"
            "WantedBy=timers.target\n"),
    }


def install(paths, run=subprocess.run, user_units: Path | None = None,
            enable=True, log=print) -> int:
    here = Path(__file__).resolve().parent
    paths.publisher_bin.mkdir(parents=True, exist_ok=True)
    for name in ("source_publisher.py", "forkmesh_paths.py"):
        target = paths.publisher_bin / name
        temporary = target.with_name(name + ".new")
        shutil.copyfile(here / name, temporary)
        os.chmod(temporary, 0o755 if name == "source_publisher.py" else 0o644)
        os.replace(temporary, target)
    paths.publisher_state.mkdir(parents=True, exist_ok=True)
    paths.systemd_units.mkdir(parents=True, exist_ok=True)
    user_units = user_units or Path(
        os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config") / "systemd" / "user"
    user_units.mkdir(parents=True, exist_ok=True)
    for name, text in render_units(paths).items():
        target = paths.systemd_units / name
        temporary = target.with_name(name + ".new")
        temporary.write_text(text, "utf-8")
        os.replace(temporary, target)
        link = user_units / name
        if link.is_symlink() or not link.exists():
            if link.is_symlink() and os.readlink(link) == str(target):
                continue
            if link.is_symlink():
                link.unlink()
            link.symlink_to(target)
        else:
            log("left %s alone: it is a regular file, not a link into %s" % (
                link, paths.systemd_units))
    if not enable:
        return 0
    commands = [
        ["systemctl", "--user", "daemon-reload"],
        ["systemctl", "--user", "enable", "--now", "forkmesh-mirror-fanout.timer",
         "forkmesh-source-publish.path", "forkmesh-source-guard.timer",
         "forkmesh-source-cores.timer"],
    ]
    for command in commands:
        completed = run(command, capture_output=True, text=True, timeout=60, check=False)
        if completed.returncode != 0:
            log("%s: %s" % (" ".join(command), completed.stderr.strip()))
            return 1
    log("installed %s and %d units" % (paths.publisher_bin, len(UNIT_NAMES)))
    return 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        prog="source_publisher.py",
        description="Publish forkmesh/forkmesh from the source checkout without "
                    "the desktop app: sync the checkout into the bare mirror, run "
                    "the fleet fan-out, push to signed HTTPS gateways.")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("sync", help="fast-forward the bare mirror from the checkout")
    publish_parser = commands.add_parser("publish", help="sync, fan out, push gateways")
    publish_parser.add_argument("--if-changed", action="store_true",
                                help="skip when nothing moved since the last good pass")
    commands.add_parser("watch-cores",
                        help="publish when the cores' main holds commits the checkout lacks")
    commands.add_parser("delete-branches",
                        help="delete on the cores the branches the source deleted")
    commands.add_parser("status", help="print the publisher and flagship state as JSON")
    commands.add_parser("guard", help="start the flagship unit unless it is held stopped")
    hold_parser = commands.add_parser("hold", help="keep the flagship stopped for a while")
    hold_parser.add_argument("duration", help="for example 30m, 2h or 1d")
    hold_parser.add_argument("--reason", default="")
    commands.add_parser("unhold", help="drop the hold so the guard restarts the flagship")
    install_parser = commands.add_parser("install", help="install the tool and user units")
    install_parser.add_argument("--no-enable", action="store_true")
    args = parser.parse_args(argv)
    paths = layout()
    try:
        if args.command == "sync":
            result = sync(paths.source, paths.bare_mirror)
            print(json.dumps(result, indent=1))
            return 0 if result["ok"] else 1
        if args.command == "publish":
            return publish(paths, if_changed=args.if_changed)
        if args.command == "watch-cores":
            return watch_cores(paths)
        if args.command == "delete-branches":
            return delete_branches(paths)
        if args.command == "status":
            print(json.dumps(status_report(paths), indent=1))
            return 0
        if args.command == "guard":
            return guard(paths)
        if args.command == "hold":
            record = hold(paths, args.duration, args.reason)
            print("held until %s" % time.strftime(
                "%Y-%m-%d %H:%M:%S", time.localtime(record["until"])))
            return 0
        if args.command == "unhold":
            print("hold removed" if unhold(paths) else "no hold was set")
            return 0
        if args.command == "install":
            return install(paths, enable=not args.no_enable)
    except PublishError as error:
        print("source_publisher: %s" % error, file=sys.stderr)
        return 1
    return 2


if __name__ == "__main__":
    sys.exit(main())
