"""End to end: a ForkMesh-hosted repository across two real core nodes.

Two real daemons (server/, built here) each supervise a real gateway
(tools/mirror_gateway.py) with ``hosted`` on, exactly like the Next cores. A
small in-process app answers the one route they call,
``POST /api/hosted-repositories/replicas``, with the real
``hosted_repos.replicas`` over SQLite, verifying their node signatures. A
signing proxy plays the Worker's part for git: it forwards stock-git smart
HTTP to a chosen node's capability route, signing each request with the router
key (``_https_mirror_push_proxy`` / ``hosted_private_read`` in production).

create (first commit on the primary) -> the replica fetches it -> push to the
primary -> the replica converges -> clone from the replica -> make it private
-> the public route goes dark while a capability still clones it.
docs/HOSTED_REPOS.md.
"""

import asyncio
import base64
import hashlib
import http.server
import json
import os
import shutil
import socket
import sqlite3
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import edge_routing  # noqa: E402
import hosted_repos  # noqa: E402

crypto = pytest.importorskip("cryptography.hazmat.primitives.asymmetric.ed25519")
serialization = pytest.importorskip("cryptography.hazmat.primitives.serialization")

pytestmark = pytest.mark.skipif(
    not shutil.which("go") or not shutil.which("git"),
    reason="needs go and git")

NODES = ("fmnext-n1", "fmnext-n2")
REPO_ID = "hr" + "e2e0" * 6
ROUTER = crypto.Ed25519PrivateKey.from_private_bytes(b"R" * 32)


def b64(raw):
    return base64.urlsafe_b64encode(raw).decode().rstrip("=")


def b64d(text):
    return base64.urlsafe_b64decode(text + "=" * (-len(text) % 4))


ROUTER_PUBLIC = b64(ROUTER.public_key().public_bytes(
    serialization.Encoding.Raw, serialization.PublicFormat.Raw))


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def sandbox_env(tmp):
    """AGENTS.md: temp HOME and XDG dirs, no FORKMESH_HOME/PROJECTS_DIR."""
    env = {k: v for k, v in os.environ.items()
           if k not in ("FORKMESH_HOME", "FORKMESH_PROJECTS_DIR")}
    home = tmp / "home"
    for sub in ("", ".config", ".local/share", ".cache"):
        (home / sub).mkdir(parents=True, exist_ok=True)
    env.update({
        "HOME": str(home), "XDG_CONFIG_HOME": str(home / ".config"),
        "XDG_DATA_HOME": str(home / ".local/share"),
        "XDG_CACHE_HOME": str(home / ".cache"),
        "GIT_CONFIG_NOSYSTEM": "1", "GIT_TERMINAL_PROMPT": "0",
    })
    return env


def git(env, *args, cwd=None):
    return subprocess.run(
        ["git", *args], cwd=cwd, env=env, check=True, capture_output=True,
        text=True, timeout=120).stdout.strip()


class App:
    """The replica exchange over SQLite, as hosted_git's adapter binds it."""

    def __init__(self, node_keys, gateways):
        self.db = sqlite3.connect(":memory:", check_same_thread=False)
        self.db.row_factory = sqlite3.Row
        self.lock = threading.Lock()
        self.db.executescript(
            "CREATE TABLE repositories (key_bi TEXT PRIMARY KEY, data TEXT);"
            "CREATE TABLE runner_request_nonces (runner TEXT, request_id TEXT,"
            " expires_at INTEGER, PRIMARY KEY (runner, request_id));")
        self.db.executescript(
            (ROOT / "migrations" / "0199_hosted_repositories.sql").read_text())
        self.keys = node_keys
        self.gateways = gateways
        self.pins = []
        self.env = object()

    # runtime interface -----------------------------------------------------
    def header(self, name):
        return self.request_headers.get(name, "")

    def now(self):
        return int(time.time() * 1000)

    def new_id(self):
        return os.urandom(16).hex()

    def response(self, data, status=200):
        return status, data

    def core_nodes(self):
        return list(NODES)

    async def node_keys(self, name):
        return [self.keys[name]] if name in self.keys else []

    async def verify(self, public_key, signature, message):
        try:
            crypto.Ed25519PublicKey.from_public_bytes(b64d(public_key)).verify(
                b64d(signature), message)
            return True
        except Exception:
            return False

    async def sign_router(self, message):
        return b64(ROUTER.sign(message.encode()))

    async def live_core_nodes(self, cores):
        return list(cores)

    async def core_endpoints(self, cores):
        return {n: "http://127.0.0.1:%d" % self.gateways[n] for n in cores}

    async def open(self, value):
        return json.loads(value) if value else None

    async def pin_initial(self, owner, name, digest):
        self.pins.append((owner, name, digest))

    async def d1_all(self, sql, *args):
        return [dict(r) for r in self.db.execute(sql, args).fetchall()]

    async def d1_first(self, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row else None

    async def d1_run(self, sql, *args):
        return self.db.execute(sql, args).rowcount

    @staticmethod
    def changes(result):
        return int(result)

    def handle(self, headers, raw):
        with self.lock:
            self.request_headers = {k.lower(): v for k, v in headers.items()}
            self.request_headers.update({k: v for k, v in headers.items()})
            return asyncio.run(hosted_repos.replicas(self, raw))


def serve(handler_factory):
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler_factory)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server


def app_handler(app):
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def do_POST(self):  # noqa: N802
            raw = self.rfile.read(int(self.headers.get("content-length") or 0))
            if self.path != hosted_repos.REPLICAS_PATH:
                self.send_error(404)
                return
            status, data = app.handle(
                {k: v for k, v in self.headers.items()}, raw)
            body = json.dumps(data).encode()
            self.send_response(status)
            self.send_header("content-type", "application/json")
            self.send_header("content-length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):  # noqa: N802
            self.send_error(404)
    return Handler


def proxy_handler(gateways):
    """/<node>/<owner>/<repo>/(info/refs|git-*-pack) -> capability route."""

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *_args):
            pass

        def forward(self, method):
            path, _, query = self.path.partition("?")
            parts = path.strip("/").split("/")
            node, owner, name = parts[0], parts[1], parts[2]
            tail = "/".join(parts[3:])
            operation = {"info/refs": None, "git-upload-pack": "git-upload-pack",
                         "git-receive-pack": "git-receive-pack"}[tail]
            if operation is None:
                operation = ("git-info-refs" if "git-upload-pack" in query
                             else "git-receive-pack")
            target = "/v1/repositories/%s/%s/%s" % (owner, name, operation)
            if query:
                target += "?" + query
            length = int(self.headers.get("content-length") or 0)
            body = self.rfile.read(length) if length else b""
            if not length and self.headers.get("transfer-encoding") == "chunked":
                chunks = []
                while True:
                    size = int(self.rfile.readline().strip(), 16)
                    if not size:
                        self.rfile.readline()
                        break
                    chunks.append(self.rfile.read(size))
                    self.rfile.readline()
                body = b"".join(chunks)
            digest = hashlib.sha256(body).hexdigest()
            request_id = b64(os.urandom(18))
            issued = int(time.time() * 1000)
            message = edge_routing.request_message(
                node, method, target, digest, request_id, issued)
            headers = {
                "X-ForkMesh-Node": node, "X-ForkMesh-Request-Id": request_id,
                "X-ForkMesh-Issued-At": str(issued),
                "X-ForkMesh-Body-Sha256": digest,
                "X-ForkMesh-Signature": b64(ROUTER.sign(message.encode())),
            }
            for name_ in ("content-type", "content-encoding", "git-protocol"):
                if self.headers.get(name_):
                    headers[name_] = self.headers[name_]
            request = urllib.request.Request(
                "http://127.0.0.1:%d%s" % (gateways[node], target),
                data=body if method == "POST" else None, method=method,
                headers=headers)
            try:
                with urllib.request.urlopen(request, timeout=60) as upstream:
                    status, payload = upstream.status, upstream.read()
                    content_type = upstream.headers.get("content-type", "")
            except urllib.error.HTTPError as error:
                status, payload = error.code, error.read()
                content_type = error.headers.get("content-type", "")
            self.send_response(status)
            self.send_header("content-type", content_type)
            self.send_header("content-length", str(len(payload)))
            self.send_header("cache-control", "no-store")
            self.end_headers()
            self.wfile.write(payload)

        def do_GET(self):  # noqa: N802
            self.forward("GET")

        def do_POST(self):  # noqa: N802
            self.forward("POST")
    return Handler


def wait_for(predicate, timeout, what, poke=None):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if poke:
            poke()
        if predicate():
            return
        time.sleep(0.5)
    raise AssertionError("timed out waiting for " + what)


@pytest.fixture
def cluster(tmp_path):
    env = sandbox_env(tmp_path)
    binary = tmp_path / "forkmesh-mirror-node"
    subprocess.run(["go", "build", "-o", str(binary),
                    "./cmd/forkmesh-mirror-node"], cwd=REPO / "server",
                   env=env, check=True, timeout=600)
    seed = tmp_path / "seed.git"
    work = tmp_path / "seed-work"
    git(env, "init", "-q", "--bare", "-b", "main", str(seed))
    git(env, "init", "-q", "-b", "main", str(work))
    git(env, "-c", "user.email=s@example.test", "-c", "user.name=Seed",
        "commit", "-q", "--allow-empty", "-m", "seed", cwd=work)
    git(env, "push", "-q", str(seed), "main", cwd=work)

    ports = {n: (free_port(), free_port()) for n in NODES}
    gateways = {n: ports[n][0] for n in NODES}
    keys = {}
    configs = {}
    for node in NODES:
        base = tmp_path / node
        config = base / "mirror-node.json"
        subprocess.run([
            str(binary), "--init", "--config", str(config),
            "--state-dir", str(base / "state"), "--node", node,
            "--owner", "forkmesh", "--repository", "forkmesh",
            "--upstream", "file://" + str(seed),
            "--catalog-url", "http://127.0.0.1:1/api/repositories",
            "--disable-cloudflared", "--version", "0.0.1",
            "--router-public-key", ROUTER_PUBLIC,
            "--public-origin", "https://%s.example.test" % node,
            "--gateway-script", str(REPO / "tools" / "mirror_gateway.py"),
            "--python", sys.executable,
        ], env=env, check=True, capture_output=True, timeout=60)
        keys[node] = subprocess.run(
            [str(binary), "--config", str(config), "--public-key"],
            env=env, check=True, capture_output=True, text=True,
            timeout=30).stdout.strip()
        configs[node] = (config, base)

    app = App(keys, gateways)
    app_server = serve(app_handler(app))
    proxy = serve(proxy_handler(gateways))
    processes = []
    for node in NODES:
        config, base = configs[node]
        gateway_port, status_port = ports[node]
        data = json.loads(config.read_text())
        data.update({
            "role": "core",
            "catalogUrl": "http://127.0.0.1:%d/api/repositories"
                          % app_server.server_address[1],
            "disableCatalog": True,
            "listen": "127.0.0.1:%d" % status_port,
            "gatewayProbe": "127.0.0.1:%d" % gateway_port,
            "syncInterval": "5s",
            "hosted": {"enabled": True, "root": str(base / "state" / "hosted")},
        })
        data.pop("eventDrivenUpstreams", None)
        config.write_text(json.dumps(data, indent=2))
        gateway_config = Path(data["gatewayConfig"])
        gateway = json.loads(gateway_config.read_text())
        gateway["listen"]["port"] = gateway_port
        gateway["collaborationControlUrl"] = (
            "http://127.0.0.1:%d/v1/control/collaboration" % status_port)
        gateway_config.write_text(json.dumps(gateway, indent=2))
        log = open(base / "daemon.log", "wb")
        processes.append((subprocess.Popen(
            [str(binary), "--config", str(config)], env=env, stdout=log,
            stderr=subprocess.STDOUT), log))

    def status(node):
        try:
            with urllib.request.urlopen(
                    "http://127.0.0.1:%d/v1/status" % ports[node][1],
                    timeout=2) as response:
                return json.loads(response.read())
        except Exception:
            return {}

    def sync(node):
        try:
            urllib.request.urlopen(urllib.request.Request(
                "http://127.0.0.1:%d/v1/control/sync" % ports[node][1],
                data=b"", method="POST"), timeout=2).read()
        except Exception:
            pass

    try:
        for node in NODES:
            wait_for(lambda node=node: status(node).get("ready"), 60,
                     node + " ready")
        yield {
            "env": env, "app": app, "sync": sync, "tmp": tmp_path,
            "hosted_dir": {n: configs[n][1] / "state" / "hosted"
                           / (REPO_ID + ".git") for n in NODES},
            "gateways": gateways,
            "proxy": "http://127.0.0.1:%d" % proxy.server_address[1],
            "logs": {n: configs[n][1] / "daemon.log" for n in NODES},
        }
    finally:
        for process, log in processes:
            process.terminate()
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill()
            log.close()
        app_server.shutdown()
        proxy.shutdown()


def head(env, git_dir, ref="refs/heads/main"):
    try:
        return git(env, "--git-dir=" + str(git_dir), "rev-parse", "--verify",
                   "-q", ref)
    except subprocess.CalledProcessError:
        return ""


def test_create_push_replicate_clone_private(cluster):
    env, app, sync = cluster["env"], cluster["app"], cluster["sync"]
    primary = hosted_repos.rendezvous(list(NODES), REPO_ID)[0]
    replica = [n for n in NODES if n != primary][0]
    init, _ = hosted_repos.build_init(
        "alice", "app", "End to end", readme=True, gitignore="",
        license_id="mit", author="alice",
        author_email="alice@users.noreply.example.test",
        created_at_ms=1_790_000_000_000)
    record = {"owner": "alice", "name": "app", "aliases": [], "init": init}
    app.db.execute("INSERT INTO repositories VALUES (?,?)",
                   ("bi:alice/app", json.dumps({"source": "hosted-web"})))
    app.db.execute(
        "INSERT INTO hosted_repos (id,key_bi,owner_bi,creator_bi,visibility,"
        "data,created_at) VALUES (?,?,?,?,?,?,?)",
        (REPO_ID, "bi:alice/app", "bi:alice", "bi:alice", "public",
         json.dumps(record), 1_790_000_000_000))
    dirs = cluster["hosted_dir"]

    def poke():
        for node in NODES:
            sync(node)

    # 1. Create: the primary writes the first commit, the replica fetches it
    #    from the primary's gateway through a router-signed read grant.
    wait_for(lambda: head(env, dirs[primary]) != "", 90, "the first commit",
             poke)
    first = head(env, dirs[primary])
    wait_for(lambda: head(env, dirs[replica]) == first, 90,
             "the replica to fetch the first commit", poke)
    assert git(env, "--git-dir=" + str(dirs[replica]), "show",
               "main:LICENSE").startswith("MIT License")
    assert app.pins and app.pins[0][:2] == ("alice", "app")

    # 2. Push to the primary through the capability route.
    proxy = cluster["proxy"]
    work = cluster["tmp"] / "work"
    git(env, "clone", "-q", "%s/%s/alice/app" % (proxy, primary), str(work))
    (work / "main.py").write_text("print('hosted')\n")
    git(env, "add", "main.py", cwd=work)
    git(env, "-c", "user.email=a@example.test", "-c", "user.name=Alice",
        "commit", "-q", "-m", "code", cwd=work)
    git(env, "push", "-q", "origin", "main", cwd=work)
    pushed = git(env, "rev-parse", "HEAD", cwd=work)
    assert head(env, dirs[primary]) == pushed
    app.db.execute("UPDATE hosted_repos SET pushed_at=?, pushed_by=?",
                   (int(time.time() * 1000), primary))

    # 3. The replica converges; a clone from it has the pushed commit.
    wait_for(lambda: head(env, dirs[replica]) == pushed, 90,
             "the replica to fetch the push", poke)
    clone = cluster["tmp"] / "clone"
    git(env, "clone", "-q", "%s/%s/alice/app" % (proxy, replica), str(clone))
    assert (clone / "main.py").read_text() == "print('hosted')\n"
    copies = app.db.execute(
        "SELECT node,pushed_at,empty FROM hosted_repo_copies ORDER BY node"
    ).fetchall()
    assert {row["node"] for row in copies} == set(NODES)

    # 4. Private: the unauthenticated clone route goes dark, a capability
    #    still clones.
    public_url = "http://127.0.0.1:%d/alice/app.git/info/refs?service=" \
        "git-upload-pack" % cluster["gateways"][replica]
    with urllib.request.urlopen(public_url, timeout=10) as response:
        assert response.status == 200
    app.db.execute("UPDATE hosted_repos SET visibility='private'")

    def dark():
        try:
            urllib.request.urlopen(public_url, timeout=5).read()
            return False
        except urllib.error.HTTPError as error:
            return error.code == 404

    wait_for(dark, 90, "the public route to close", poke)
    private_clone = cluster["tmp"] / "private-clone"
    git(env, "clone", "-q", "%s/%s/alice/app" % (proxy, replica),
        str(private_clone))
    assert (private_clone / "main.py").is_file()
