"""Start throwaway local rqlite nodes for tests.

Every data directory lives under a fresh ``tempfile.mkdtemp`` directory and
only that directory is ever removed. Ports are picked by the kernel on
127.0.0.1.
"""

from __future__ import annotations

import json
import os
import shutil
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

SRC_ROOT = Path(__file__).resolve().parents[3]


def _default_binary() -> Path:
    import sys
    tools = str(SRC_ROOT / "tools")
    if tools not in sys.path:
        sys.path.insert(0, tools)
    import forkmesh_paths
    return forkmesh_paths.resolve().fleet_data / "bin" / "rqlited"


DEFAULT_BINARY = _default_binary()


def rqlited_binary() -> Path | None:
    configured = os.environ.get("RQLITED")
    path = Path(configured) if configured else DEFAULT_BINARY
    return path if path.is_file() and os.access(path, os.X_OK) else None


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def http_json(url, body=None, timeout=10.0):
    data = None
    headers = {}
    if body is not None:
        data = json.dumps(body).encode()
        headers["content-type"] = "application/json"
    request = urllib.request.Request(url, data=data, headers=headers,
                                     method="POST" if data else "GET")
    with urllib.request.urlopen(request, timeout=timeout) as response:
        raw = response.read()
    try:
        return json.loads(raw)
    except ValueError:
        return raw.decode(errors="replace")


class Node:
    def __init__(self, cluster, name, extra_args=()):
        self.cluster = cluster
        self.name = name
        self.http_port = free_port()
        self.raft_port = free_port()
        self.data_dir = cluster.root / name
        self.extra_args = list(extra_args)
        self.proc = None
        self.log_path = cluster.root / (name + ".log")

    @property
    def http(self):
        return "http://127.0.0.1:%d" % self.http_port

    @property
    def raft_addr(self):
        return "127.0.0.1:%d" % self.raft_port

    @property
    def db_path(self) -> Path:
        # rqlite v8+ is always on-disk: <data dir>/db.sqlite, in WAL mode.
        return self.data_dir / "db.sqlite"

    def start(self, join=None):
        args = [
            str(self.cluster.binary),
            "-node-id", self.name,
            "-http-addr", "127.0.0.1:%d" % self.http_port,
            "-raft-addr", self.raft_addr,
            "-raft-election-timeout", "500ms",
            "-raft-heartbeat-timeout", "500ms",
        ]
        if join:
            args += ["-join", join]
        args += self.extra_args + [str(self.data_dir)]
        log = open(self.log_path, "ab")
        self.proc = subprocess.Popen(
            args, stdout=log, stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL)
        log.close()
        return self

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        self.proc = None

    def ready(self) -> bool:
        try:
            with urllib.request.urlopen(self.http + "/readyz", timeout=2) as r:
                return r.status == 200
        except (urllib.error.URLError, OSError):
            return False

    def wait_ready(self, timeout=60.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.proc is not None and self.proc.poll() is not None:
                raise RuntimeError("%s exited: %s" % (
                    self.name, self.log_path.read_text(errors="replace")[-2000:]))
            if self.ready():
                return
            time.sleep(0.1)
        raise TimeoutError("%s not ready: %s" % (
            self.name, self.log_path.read_text(errors="replace")[-2000:]))

    def status(self):
        return http_json(self.http + "/status")

    def is_leader(self) -> bool:
        try:
            return self.status()["store"]["raft"]["state"] == "Leader"
        except Exception:
            return False

    def request(self, statements, params="associative"):
        return http_json(self.http + "/db/request?" + params, statements)

    def query(self, statements, params="associative&level=none"):
        return http_json(self.http + "/db/query?" + params, statements)


class Cluster:
    def __init__(self, size=1, extra_args=()):
        binary = rqlited_binary()
        if binary is None:
            raise RuntimeError("rqlited binary not found")
        self.binary = binary
        self.root = Path(tempfile.mkdtemp(prefix="fmnext-rqlite-"))
        self.nodes = [Node(self, "n%d" % (i + 1), extra_args)
                      for i in range(size)]

    def start(self):
        first = self.nodes[0]
        first.start().wait_ready()
        for node in self.nodes[1:]:
            # One join at a time: concurrent joins occasionally stall.
            node.start(join=first.raft_addr).wait_ready()
        self.wait_voters(len(self.nodes))
        return self

    def wait_voters(self, count, timeout=30.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                # v10 shape: {"n1": {"voter": true, "reachable": true, ...}}
                nodes = http_json(self.leader().http + "/nodes?nonvoters")
                reachable = [n for n in nodes.values()
                             if n.get("voter") and n.get("reachable")]
                if len(reachable) >= count:
                    return
            except Exception:
                pass
            time.sleep(0.2)
        raise TimeoutError("cluster never reached %d voters" % count)

    def leader(self, timeout=15.0) -> Node:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for node in self.nodes:
                if node.proc is not None and node.is_leader():
                    return node
            time.sleep(0.1)
        raise TimeoutError("no leader")

    def followers(self):
        leader = self.leader()
        return [node for node in self.nodes if node is not leader]

    def stop(self):
        for node in reversed(self.nodes):
            node.stop()
        root = self.root.resolve()
        tmp = Path(tempfile.gettempdir()).resolve()
        # Only ever remove the directory mkdtemp created for this cluster.
        if (root.parent == tmp and root.name.startswith("fmnext-rqlite-")
                and root.is_dir()):
            shutil.rmtree(root, ignore_errors=True)

    def __enter__(self):
        try:
            return self.start()
        except BaseException:
            self.stop()
            raise

    def __exit__(self, *exc):
        self.stop()
