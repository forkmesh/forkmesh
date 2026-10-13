"""Shared plumbing for tools/next_{fleet,deploy,dns,backup}.py.

Safety model (all four tools):

* **Dry-run is the default.** A mutating step only runs with ``--execute``;
  otherwise it is printed as ``[dry-run] ...`` and skipped.
* Credentials are read from ``$FMNEXT_CONFIG_DIR`` (default
  ``~/projects/forkmesh/config/fleet``) env files and never printed, logged or
  put on a command line; SSH gets secrets on stdin only.
* HTTP and SSH go through small injectable classes so tests replace them
  with fakes (deploy/tests/conftest.py also blocks non-loopback sockets).
"""

from __future__ import annotations

import json
import os
import re
import shlex
import stat
import subprocess
import tempfile
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "deploy" / "core"))
sys.path.insert(0, str(REPO / "tools"))
sys.path.insert(0, str(REPO / "tools"))


class ToolError(RuntimeError):
    pass


class GuardError(ToolError):
    """A safety guard refused the operation."""


RELEASE_VERSION_RE = re.compile(
    r"^project\(ForkMesh VERSION ([0-9]+\.[0-9]+\.[0-9]+)\b", re.M)


def release_version(repo=None) -> str:
    text = (Path(repo or REPO) / "desktop" / "CMakeLists.txt").read_text()
    match = RELEASE_VERSION_RE.search(text)
    if not match:
        raise ToolError("desktop/CMakeLists.txt has no project(ForkMesh VERSION x.y.z)")
    return match.group(1)


# --------------------------------------------------------------------------
# configuration


def layout():
    import forkmesh_paths
    return forkmesh_paths.resolve()


def project_dir() -> Path:
    return layout().home_dir


def config_dir() -> Path:
    configured = os.environ.get("FMNEXT_CONFIG_DIR")
    if configured:
        return Path(configured)
    return layout().fleet_config


def data_dir() -> Path:
    configured = os.environ.get("FMNEXT_DATA_DIR")
    if configured:
        return Path(configured)
    return layout().fleet_data


def logs_dir() -> Path:
    configured = os.environ.get("FMNEXT_LOGS_DIR")
    if configured:
        return Path(configured)
    return layout().fleet_logs


def autodeploy_dir() -> Path:
    return config_dir() / "autodeploy"


def deploy_lock_path() -> Path:
    return config_dir() / "deploy.lock"


def deploy_manifest_path() -> Path:
    return config_dir() / "deploy-manifest.json"


LOCK_FD_ENV = "FMNEXT_DEPLOY_LOCK_FD"


class LockBusy(ToolError):
    def __init__(self, holder):
        self.holder = holder or {}
        super().__init__("another deploy holds %s: %s" % (
            deploy_lock_path(), describe_holder(self.holder)))


def describe_holder(holder):
    if not holder:
        return "holder unknown"
    return "%s (pid %s, since %s)" % (
        holder.get("what") or "?", holder.get("pid") or "?",
        time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(
            float(holder.get("since") or 0))))


class DeployLock:
    def __init__(self, path=None, what=""):
        self.path = Path(path or deploy_lock_path())
        self.what = what or " ".join(sys.argv[:3])
        self.fd = None
        self.inherited = False

    def _inherited_fd(self):
        raw = os.environ.get(LOCK_FD_ENV, "")
        if not raw.isdigit():
            return None
        fd = int(raw)
        try:
            held = os.fstat(fd)
            here = os.stat(self.path)
        except OSError:
            return None
        if (held.st_dev, held.st_ino) != (here.st_dev, here.st_ino):
            return None
        return fd

    def holder(self):
        try:
            return json.loads(self.path.read_text("utf-8") or "{}")
        except (OSError, ValueError):
            return {}

    def acquire(self, wait=0.0, sleep=time.sleep, clock=time.monotonic):
        import fcntl
        if self._inherited_fd() is not None:
            self.inherited = True
            return self
        self.path.parent.mkdir(parents=True, exist_ok=True)
        fd = os.open(self.path, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
        deadline = clock() + max(0.0, wait)
        while True:
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if clock() >= deadline:
                    os.close(fd)
                    raise LockBusy(self.holder()) from None
                sleep(1.0)
        record = json.dumps({"pid": os.getpid(), "what": self.what,
                             "since": time.time()}).encode()
        os.ftruncate(fd, 0)
        os.pwrite(fd, record, 0)
        self.fd = fd
        return self

    def release(self):
        import fcntl
        if self.fd is None:
            return
        try:
            os.ftruncate(self.fd, 0)
            fcntl.flock(self.fd, fcntl.LOCK_UN)
        finally:
            os.close(self.fd)
            self.fd = None

    def child_env(self, env=None):
        env = dict(os.environ if env is None else env)
        if self.fd is not None:
            env[LOCK_FD_ENV] = str(self.fd)
        return env

    def __enter__(self):
        return self.acquire()

    def __exit__(self, *exc):
        self.release()


SIGNATURE_ROOTS = ("app", "world", "www", "deploy")


def served_signature(commit, repo=None, run=subprocess.run) -> str:
    specs = ["%s:%s" % (commit, root) for root in SIGNATURE_ROOTS]
    completed = run(["git", "rev-parse", *specs], cwd=repo or REPO,
                    capture_output=True, text=True, check=False)
    if completed.returncode != 0:
        raise ToolError("cannot read the served trees of %s" % commit)
    import hashlib
    lines = ["%s %s" % pair for pair in zip(SIGNATURE_ROOTS,
                                             completed.stdout.split())]
    return hashlib.sha256(("\n".join(lines) + "\n").encode()).hexdigest()


def load_manifest(path=None) -> dict:
    data = load_json(Path(path or deploy_manifest_path()), {}) or {}
    return data if isinstance(data, dict) else {}


def record_deploy(rev, commit="", signature="", fingerprint="", by="",
                  path=None, clock=time.time) -> dict:
    path = Path(path or deploy_manifest_path())
    data = load_manifest(path)
    releases = data.get("releases") if isinstance(data.get("releases"), dict) else {}
    entry = {"rev": rev, "commit": commit, "signature": signature,
             "fingerprint": fingerprint, "by": by, "at": clock()}
    releases[rev] = entry
    data["releases"] = releases
    data["current"] = rev
    save_json(path, data)
    return entry


def read_env_file(path: Path) -> dict:
    values = {}
    try:
        text = Path(path).read_text("utf-8")
    except FileNotFoundError:
        raise ToolError("missing %s" % path) from None
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        if line.startswith("export "):
            line = line[len("export "):]
        name, _, value = line.partition("=")
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "'\"":
            value = value[1:-1]
        values[name.strip()] = value
    return values


def credential(env_file: str, name: str) -> str:
    """One value from <config_dir>/<env_file>; never echoed."""
    path = config_dir() / env_file
    values = read_env_file(path)
    value = values.get(name) or os.environ.get(name) or ""
    if not value:
        raise ToolError("%s is not set in %s" % (name, path))
    return value


def load_json(path: Path, default=None):
    try:
        return json.loads(Path(path).read_text("utf-8"))
    except FileNotFoundError:
        return default


def save_json(path: Path, data, mode=0o600):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, mode)
    with os.fdopen(fd, "w", encoding="utf-8") as handle:
        json.dump(data, handle, indent=2, sort_keys=True)
        handle.write("\n")
    os.replace(temporary, path)


# --------------------------------------------------------------------------
# dry-run


class Runner:
    """Decides whether a mutating step runs, and says what it does."""

    def __init__(self, execute=False, out=None, log_path=None):
        self.execute = bool(execute)
        self.out = out or sys.stdout
        self.skipped = []
        self.log_path = Path(log_path) if log_path else None
        self.log_handle = None

    def say(self, message):
        print(message, file=self.out, flush=True)
        self._tee(message)

    def _tee(self, message):
        if self.log_path is None:
            return
        try:
            if self.log_handle is None:
                self.log_path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
                os.chmod(self.log_path.parent, 0o700)
                fd = os.open(self.log_path, os.O_WRONLY | os.O_CREAT | os.O_APPEND,
                             0o600)
                self.log_handle = os.fdopen(fd, "a", encoding="utf-8")
            stamp = time.strftime("%H:%M:%S")
            for raw in str(message).splitlines() or [""]:
                self.log_handle.write("%s %s\n" % (stamp, raw))
            self.log_handle.flush()
        except (OSError, ValueError):
            self.log_path = None
            self.log_handle = None

    def step(self, description, action=None):
        """Run ``action()`` when executing; otherwise print and skip."""
        if not self.execute:
            self.skipped.append(description)
            self.say("[dry-run] %s" % description)
            return None
        self.say("[execute] %s" % description)
        return action() if action else None


def parallel_steps(runner, steps, workers=None):
    steps = list(steps)
    if not runner.execute or len(steps) < 2:
        return [runner.step(description, action) for description, action in steps]
    from concurrent.futures import ThreadPoolExecutor
    with ThreadPoolExecutor(max_workers=workers or len(steps)) as pool:
        futures = [pool.submit(runner.step, description, action)
                   for description, action in steps]
    errors = [future.exception() for future in futures if future.exception()]
    if errors:
        raise errors[0]
    return [future.result() for future in futures]


def add_execute_flags(parser):
    parser.add_argument("--execute", action="store_true",
                        help="actually do it (the default is a dry run)")
    parser.add_argument("--dry-run", action="store_true",
                        help="print the plan only (the default)")


def runner_from_args(args):
    if getattr(args, "execute", False) and getattr(args, "dry_run", False):
        raise ToolError("--execute and --dry-run are mutually exclusive")
    execute = getattr(args, "execute", False)
    return Runner(execute=execute, log_path=ops_log_path(args) if execute else None)


def ops_log_path(args, argv0=None, clock=time.time):
    if os.environ.get(LOCK_FD_ENV):
        return None
    tool = Path(argv0 or sys.argv[0] or "tool").name
    if tool.endswith(".py"):
        tool = tool[:-3]
    parts = [time.strftime("%Y%m%d-%H%M%S", time.localtime(clock())), tool]
    command = getattr(args, "command", None)
    if command:
        parts.append(str(command))
    name = re.sub(r"[^A-Za-z0-9._-]+", "_", "-".join(parts))
    return logs_dir() / "ops" / (name + ".log")


# --------------------------------------------------------------------------
# HTTP


class JsonHttp:
    """Minimal JSON client over urllib; ``opener`` is injectable for tests."""

    def __init__(self, base, token, opener=None, user_agent="fmnext-tools/1"):
        self.base = base.rstrip("/")
        self._token = token
        self.opener = opener or urllib.request.urlopen
        self.user_agent = user_agent

    def request(self, method, path, body=None, expect=(200, 201, 202, 204)):
        data = None if body is None else json.dumps(body).encode()
        request = urllib.request.Request(
            self.base + path, data=data, method=method,
            headers={"Authorization": "Bearer %s" % self._token,
                     "Content-Type": "application/json",
                     "Accept": "application/json",
                     "User-Agent": self.user_agent})
        try:
            with self.opener(request, timeout=30) as response:
                status = response.status
                raw = response.read()
        except urllib.error.HTTPError as error:
            status = error.code
            raw = error.read() or b""
        if status not in expect:
            detail = raw[:300].decode("utf-8", "replace")
            raise ToolError("%s %s -> HTTP %d: %s" % (method, path, status, detail))
        return json.loads(raw) if raw.strip() else {}

    def __repr__(self):  # never show the token
        return "JsonHttp(%s)" % self.base


# --------------------------------------------------------------------------
# SSH


def ssh_key_path() -> Path:
    configured = os.environ.get("FMNEXT_SSH_KEY")
    if configured:
        return Path(configured)
    return layout().fleet_ssh_key


SSH_PERSIST_SECONDS = 120
PUT_RESUME_ATTEMPTS = 6
ROUTE_FAMILIES = ("auto", "4", "6")
ROUTE_PROBE_SECONDS = 3
_route_cache: dict = {}


def ipv6_by_ipv4(path=None) -> dict:
    data = load_json(Path(path or config_dir() / "fleet-instances.json"), {}) or {}
    return {entry["ip"]: entry["ipv6"] for entry in data.values()
            if isinstance(entry, dict) and entry.get("ip") and entry.get("ipv6")}


def _tcp_open(address, port=22, timeout=ROUTE_PROBE_SECONDS) -> bool:
    import socket
    try:
        with socket.create_connection((address, port), timeout=timeout):
            return True
    except OSError:
        return False


def route_address(ip, family=None, mapping=None, probe=_tcp_open) -> str:
    family = family or os.environ.get("FMNEXT_ROUTE_FAMILY", "auto")
    if family not in ROUTE_FAMILIES:
        raise GuardError("FMNEXT_ROUTE_FAMILY must be one of %s" % ", ".join(ROUTE_FAMILIES))
    if family == "4" or ":" in ip:
        return ip
    ipv6 = (ipv6_by_ipv4() if mapping is None else mapping).get(ip)
    if not ipv6:
        return ip
    if family == "6":
        return ipv6
    key = (ip, ipv6)
    if key not in _route_cache:
        _route_cache[key] = ip if probe(ip) else (ipv6 if probe(ipv6) else ip)
    return _route_cache[key]


class Ssh:
    """ssh/scp to fleet nodes as root with the fleet key only."""

    def __init__(self, key=None, known_hosts=None, run=None, popen=None,
                 mux=None, route=None):
        self.key = Path(key or ssh_key_path())
        self.known_hosts = Path(known_hosts or config_dir() / "known_hosts")
        self._run = run or subprocess.run
        self._popen = popen or subprocess.Popen
        self.mux = (run is None) if mux is None else mux
        self.route = (run is None) if route is None else route
        self._masters = set()

    def mux_dir(self):
        path = Path(os.environ.get("FMNEXT_SSH_MUX_DIR") or
                    Path(tempfile.gettempdir()) / ("fmnext-ssh-%d" % os.getuid()))
        try:
            path.mkdir(mode=0o700, exist_ok=True)
            info = path.lstat()
        except OSError:
            return None
        if (not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid()
                or info.st_mode & 0o077):
            return None
        return path

    def options(self, mux=True, strict=False):
        options = ["-i", str(self.key), "-o", "IdentitiesOnly=yes",
                   "-o", "BatchMode=yes", "-o", "ConnectTimeout=10",
                   "-o", "StrictHostKeyChecking=%s" % ("yes" if strict else "accept-new"),
                   "-o", "UserKnownHostsFile=%s" % self.known_hosts,
                   "-o", "ServerAliveInterval=15"]
        directory = self.mux_dir() if mux else None
        if directory is None:
            return options + ["-o", "ControlMaster=no", "-o", "ControlPath=none"]
        return options + ["-o", "ControlMaster=no",
                          "-o", "ControlPath=%s/%%C" % directory]

    def address(self, host):
        return route_address(host) if self.route else host

    def dest(self, host):
        return ["-o", "HostKeyAlias=%s" % host] if self.address(host) != host else []

    def login(self, host):
        return "root@%s" % self.address(host)

    def master(self, host):
        if not self.mux or self.mux_dir() is None or host in self._masters:
            return
        self._masters.add(host)
        base = [*self.options(), *self.dest(host), self.login(host)]
        try:
            alive = self._run(["ssh", "-O", "check", *base],
                              stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL, timeout=10, check=False)
            if alive.returncode == 0:
                return
            start = [part if part != "ControlMaster=no" else "ControlMaster=yes"
                     for part in base]
            self._run(["ssh", "-f", "-N", "-o", "ControlPersist=%d" %
                       SSH_PERSIST_SECONDS, *start],
                      stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                      stderr=subprocess.DEVNULL, timeout=30, check=False)
        except (OSError, subprocess.TimeoutExpired):
            return

    def run(self, host, script, input_bytes=None, check=True, timeout=1800):
        self.master(host)
        command = ["ssh", *self.options(), *self.dest(host), self.login(host),
                   "bash -euo pipefail -c %s" % shlex.quote(script)]
        completed = self._run(command, input=input_bytes, capture_output=True,
                              timeout=timeout, check=False)
        if check and completed.returncode != 0:
            tail = (completed.stderr or b"")[-2000:].decode("utf-8", "replace")
            raise ToolError("ssh %s failed (%d): %s" % (host, completed.returncode, tail))
        return completed

    def pinned(self, host):
        completed = self._run(["ssh-keygen", "-F", host, "-f", str(self.known_hosts)],
                              capture_output=True, check=False, timeout=30)
        return completed.returncode == 0 and bool((completed.stdout or b"").strip())

    def run_strict(self, host, script, timeout=60):
        command = ["ssh", *self.options(mux=False, strict=True), *self.dest(host),
                   self.login(host),
                   "bash -euo pipefail -c %s" % shlex.quote(script)]
        return self._run(command, capture_output=True, timeout=timeout, check=False)

    def put(self, host, local, remote):
        self.master(host)
        sources = [str(item) for item in local] if isinstance(
            local, (list, tuple)) else [str(local)]
        address = self.address(host)
        command = ["scp", "-q", *self.options(), *self.dest(host), *sources,
                   "root@%s:%s" % ("[%s]" % address if ":" in address else address, remote)]
        completed = self._run(command, capture_output=True, check=False, timeout=3600)
        if completed.returncode == 0:
            return
        error = completed.stderr or b""
        if len(sources) == 1 and not remote.endswith("/"):
            for _ in range(PUT_RESUME_ATTEMPTS):
                self._masters.discard(host)
                self.master(host)
                done, error = self._resume(host, Path(sources[0]), remote)
                if done:
                    return
        raise ToolError("scp to %s failed: %s" % (
            host, error[-500:].decode("utf-8", "replace")))

    def _resume(self, host, local, remote):
        size = local.stat().st_size
        target = shlex.quote(remote)
        try:
            probe = self._run(
                ["ssh", *self.options(), *self.dest(host), self.login(host),
                 "stat -c %%s %s 2>/dev/null || echo 0" % target],
                capture_output=True, timeout=120, check=False)
        except subprocess.TimeoutExpired:
            return False, b"size probe timed out"
        text = (probe.stdout or b"").decode("ascii", "replace").strip()
        if probe.returncode != 0 or not text.isdigit():
            return False, probe.stderr or b"size probe failed"
        have = int(text)
        if have == size:
            return True, b""
        if have > size:
            have = 0
        append = ">>" if have else ">"
        with open(local, "rb") as stream:
            stream.seek(have)
            try:
                completed = self._run(
                    ["ssh", *self.options(), *self.dest(host), self.login(host),
                     "cat %s %s" % (append, target)],
                    stdin=stream, capture_output=True, timeout=3600, check=False)
            except subprocess.TimeoutExpired:
                return False, b"resume timed out"
        return completed.returncode == 0, completed.stderr or b""

    def tunnel(self, host, local_port, remote="127.0.0.1:4001", bind="127.0.0.1"):
        listen = "[%s]" % bind if ":" in bind else bind
        command = ["ssh", *self.options(mux=False), "-N", "-o", "ExitOnForwardFailure=yes",
                   "-L", "%s:%d:%s" % (listen, local_port, remote), *self.dest(host),
                   self.login(host)]
        return self._popen(command, stdin=subprocess.DEVNULL,
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)


def wait_for(predicate, timeout, interval=2.0, sleep=time.sleep, clock=time.monotonic):
    deadline = clock() + timeout
    while True:
        result = predicate()
        if result:
            return result
        if clock() >= deadline:
            return None
        sleep(interval)


# --------------------------------------------------------------------------
# fleet helpers


def fleet_file(path=None) -> Path:
    return Path(path) if path else REPO / "deploy" / "fleet.toml"


def load_fleet(path=None):
    import render  # deploy/core/render.py
    return render.load_fleet(fleet_file(path), config_dir() / "fleet-state.json")


def core_nodes(fleet):
    return list(fleet.get("nodes") or [])


def rqlite_curl(path, method="GET", body_file=None):
    """Shell snippet: curl the local rqlite with credentials from a root-only
    file written by next_fleet.py secrets (never on the command line)."""
    parts = ["curl", "-fsS", "--max-time", "10", "-K", "/etc/forkmesh-next/rqlite.curlrc"]
    if method != "GET":
        parts += ["-X", method]
    if body_file:
        parts += ["-H", "'Content-Type: application/json'", "--data-binary",
                  "@" + body_file]
    parts.append("'http://127.0.0.1:4001%s'" % path)
    return " ".join(parts)
