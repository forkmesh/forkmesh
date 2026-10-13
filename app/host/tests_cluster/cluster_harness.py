"""A real 3-node cluster: 3 rqlite nodes + 3 host servers (Phase 3).

Lives in its own module (conftest.py only re-exports the fixture) so test
modules can import it by a unique name even when another conftest.py (for
example deploy/tests) is collected in the same pytest run.

Everything lives under one ``tempfile.mkdtemp`` directory and only that
directory is removed. Host subprocesses get a sandboxed HOME/XDG_* and never
see FORKMESH_HOME / FORKMESH_PROJECTS_DIR (AGENTS.md).
"""

from __future__ import annotations

import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import httpx
import pytest

HERE = Path(__file__).resolve().parent
HOST = HERE.parent
SRC_ROOT = HERE.parents[2]
for path in (HOST / "tests_rqlite", HOST, SRC_ROOT / "tools"):
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))

for _leaked in ("FORKMESH_HOME", "FORKMESH_PROJECTS_DIR"):
    os.environ.pop(_leaked, None)
# rqlite must run in UTC (rqlited inherits this environment).
os.environ["TZ"] = "UTC"

import rqlite_cluster  # noqa: E402

from forkmesh_host import paths  # noqa: E402

MESH_SECRET = "test-mesh-secret-" + os.urandom(12).hex()
# One shared public origin for all nodes, like app.next behind DNS. The app
# only accepts plain http for localhost, so the origin is localhost on a port
# nobody listens on; clients connect to a node's socket and send this Host.
APP_ORIGIN = "http://localhost:%d" % rqlite_cluster.free_port()


def _stale_build():
    entry = paths.SRC_BUILD_DIR / "entry.py"
    if not entry.is_file():
        return True
    newest = max(
        p.stat().st_mtime
        for root in (paths.APP_DIR / "src", paths.REPO_DIR / "world" / "src")
        for p in root.glob("*.py"))
    return newest > entry.stat().st_mtime


def vm_rss_kib(pid):
    try:
        with open("/proc/%d/status" % pid, encoding="ascii") as handle:
            for line in handle:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
    except OSError:
        return 0
    return 0


def sandbox_env(root):
    env = {k: v for k, v in os.environ.items()
           if k not in ("FORKMESH_HOME", "FORKMESH_PROJECTS_DIR")
           and not k.startswith("FORKMESH_")}
    home = root / "home"
    for sub in ("", ".local/share", ".config", ".cache"):
        (home / sub).mkdir(parents=True, exist_ok=True)
    env.update({
        "HOME": str(home),
        "XDG_DATA_HOME": str(home / ".local/share"),
        "XDG_CONFIG_HOME": str(home / ".config"),
        "XDG_CACHE_HOME": str(home / ".cache"),
        "PYTHONPATH": str(HOST),
        "TZ": "UTC",
    })
    return env


class HostNode:
    def __init__(self, harness, node_id, rqlite_node, cluster=True,
                 extra_env=None, cron=False):
        self.harness = harness
        self.node_id = node_id
        self.rqlite = rqlite_node
        self.cluster = cluster
        self.port = rqlite_cluster.free_port()
        self.mesh_port = rqlite_cluster.free_port()
        self.root = harness.root / ("host-" + node_id)
        self.root.mkdir(parents=True, exist_ok=True)
        self.log_path = self.root / "server.log"
        self.proc = None
        self.extra_env = dict(extra_env or {})
        self.cron = cron

    @property
    def http(self):
        return "http://127.0.0.1:%d" % self.port

    @property
    def ws(self):
        return "ws://127.0.0.1:%d" % self.port

    @property
    def mesh(self):
        return "http://127.0.0.1:%d" % self.mesh_port

    def env(self):
        env = sandbox_env(self.root)
        env.update({
            "FORKMESH_HOST_DATA_DIR": str(self.root / "data"),
            "FORKMESH_HOST_ASSETS_DIR": str(self.harness.assets),
            "FORKMESH_HOST_OFFLINE": "1",
            "FORKMESH_HOST_DEBUG": "1",
            "FORKMESH_HOST_DB": "rqlite:" + self.rqlite.http,
            "FORKMESH_VAR_DATA_KEY": self.harness.data_key,
            "FORKMESH_VAR_ADMIN_PASS": self.harness.admin_pass,
            "FORKMESH_VAR_APP_ORIGIN": APP_ORIGIN,
            "FORKMESH_VAR_API_ORIGIN": APP_ORIGIN,
            "FORKMESH_VAR_WORLD_ORIGIN": APP_ORIGIN,
            "FORKMESH_VAR_PUBLIC_BASE_URL": APP_ORIGIN,
            "FORKMESH_VAR_CORS_ALLOWED_ORIGINS": APP_ORIGIN,
            # The public Host allowlist (request_guard.HostAllowlist): the
            # tests address every node as 127.0.0.1 or localhost.
            "FORKMESH_PUBLIC_HOSTS": "127.0.0.1,localhost",
        })
        if self.cluster:
            env.update({
                "FORKMESH_NODE_ID": self.node_id,
                "FORKMESH_MESH_LISTEN": "127.0.0.1:%d" % self.mesh_port,
                "FORKMESH_MESH_SECRET": MESH_SECRET,
                # The static fleet map (as deploy/core/render.py renders it):
                # the only source of member ids and mesh addresses.
                "FORKMESH_MESH_PEERS": ",".join(
                    "%s=%s" % (host.node_id, host.mesh)
                    for host in getattr(self.harness, "hosts", [])
                    if host.cluster) or "%s=%s" % (self.node_id, self.mesh),
            })
        env.update(self.extra_env)
        return env

    def start(self):
        log = open(self.log_path, "ab")
        self.proc = subprocess.Popen(
            [sys.executable, "-m", "forkmesh_host", "serve", "--port",
             str(self.port), "--no-build", "--log-level", "warning"]
            + ([] if self.cron else ["--no-cron"]),
            cwd=str(paths.HOST_DIR), env=self.env(), stdout=log,
            stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
        log.close()
        return self

    def wait_http(self, timeout=90.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError("%s exited:\n%s" % (self.node_id,
                                                       self.log_tail()))
            try:
                if httpx.get(self.http + "/health",
                             timeout=2).status_code == 200:
                    return
            except httpx.HTTPError:
                pass
            time.sleep(0.2)
        raise TimeoutError("%s did not start:\n%s" % (self.node_id,
                                                      self.log_tail()))

    def wait_ready(self, timeout=60.0):
        deadline = time.monotonic() + timeout
        last = None
        while time.monotonic() < deadline:
            last = self.readyz()
            if last[0] == 200:
                return
            time.sleep(0.25)
        raise TimeoutError("%s not ready: %r\n%s" % (
            self.node_id, last, self.log_tail()))

    def readyz(self):
        try:
            response = httpx.get(self.http + "/readyz", timeout=5)
            return response.status_code, response.json()
        except (httpx.HTTPError, ValueError) as error:
            return 0, repr(error)

    def cluster_state(self, ns=None, name=None):
        params = {"ns": ns, "name": name} if ns else None
        return httpx.get(self.http + "/_host/cluster", params=params,
                         timeout=10).json()

    def kill(self):
        if self.proc is not None and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGKILL)
            self.proc.wait(10)

    def stop(self):
        if self.proc is not None and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(15)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(5)

    @property
    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def log_tail(self, size=4000):
        try:
            return self.log_path.read_text(errors="replace")[-size:]
        except OSError:
            return ""


class Harness:
    def __init__(self, extra_env=None, cron=False):
        self.extra_env = dict(extra_env or {})
        self.cron = cron
        self.root = Path(tempfile.mkdtemp(prefix="fmnext-cluster-"))
        self.assets = self.root / "assets"
        self.data_key = "cluster-data-key-" + os.urandom(16).hex()
        self.admin_pass = "cluster-admin-" + os.urandom(8).hex()
        self.rqlite = None
        self.hosts = []

    def start(self):
        shutil.copytree(paths.APP_DIR / "public", self.assets)
        (self.assets / "index.html").write_text("<!doctype html><title>h")
        (self.assets / "404.html").write_text("<!doctype html><title>404")
        import urllib.request

        import rqlite_migrate
        # Migrating a 3-voter cluster file by file is slow (one Raft round
        # per file); migrate one throwaway node and load its image instead.
        image = None
        for attempt in range(3):
            with rqlite_cluster.Cluster(1) as single:
                try:
                    rqlite_migrate.migrate(
                        single.leader().http, app_schema=True,
                        log=lambda *_: None)
                except rqlite_migrate.MigrateError as error:
                    # Seen rarely under load: one request hangs. Retry on a
                    # fresh node.
                    print("migrate attempt %d failed: %s\n%s" % (
                        attempt, error, single.nodes[0].log_path.read_text(
                            errors="replace")[-2000:]))
                    continue
                with urllib.request.urlopen(
                        single.leader().http + "/db/backup",
                        timeout=120) as response:
                    image = response.read()
                break
        assert image, "could not migrate a fresh rqlite node"
        self.rqlite = rqlite_cluster.Cluster(3).start()
        request = urllib.request.Request(
            self.rqlite.leader().http + "/db/load", data=image,
            method="POST",
            headers={"content-type": "application/octet-stream"})
        with urllib.request.urlopen(request, timeout=120) as response:
            response.read()
        self.hosts = [HostNode(self, node.name, node,
                               extra_env=self.extra_env, cron=self.cron)
                      for node in self.rqlite.nodes]
        for host in self.hosts:
            host.start()
        for host in self.hosts:
            host.wait_http()
        for host in self.hosts:
            host.wait_ready()
        self.wait_converged()
        return self

    def wait_converged(self, timeout=30.0):
        ids = sorted(h.node_id for h in self.hosts if h.cluster)
        deadline = time.monotonic() + timeout
        views = None
        while time.monotonic() < deadline:
            views = {h.node_id: h.cluster_state()["active"]
                     for h in self.hosts if h.cluster}
            if all(sorted(view) == ids for view in views.values()):
                return
            time.sleep(0.25)
        raise TimeoutError("cluster views did not converge: %r" % (views,))

    def host(self, node_id):
        return next(h for h in self.hosts if h.node_id == node_id)

    def live_hosts(self):
        return [h for h in self.hosts if h.alive]

    def stop(self):
        for host in self.hosts:
            host.stop()
        if self.rqlite is not None:
            self.rqlite.stop()
        root = self.root.resolve()
        tmp = Path(tempfile.gettempdir()).resolve()
        if (root.parent == tmp and root.name.startswith("fmnext-cluster-")
                and root.is_dir()):
            shutil.rmtree(root, ignore_errors=True)


def start_harness(extra_env=None, cron=False):
    if rqlite_cluster.rqlited_binary() is None:
        pytest.skip("rqlited not found")
    if _stale_build():
        subprocess.run([sys.executable, "tools/build_worker_python.py"],
                       cwd=paths.APP_DIR, check=True)
    harness = Harness(extra_env, cron)
    try:
        harness.start()
    except BaseException:
        for host in harness.hosts:
            print("---- %s log ----\n%s" % (host.node_id, host.log_tail()))
        harness.stop()
        raise
    return harness


def finish_harness(harness):
    for host in harness.hosts:
        text = host.log_tail(200000)
        print("---- %s log (%d bytes) ----\n%s" % (
            host.node_id, len(text), text[-3000:]))
    harness.stop()


@pytest.fixture(scope="module")
def harness():
    harness = start_harness()
    yield harness
    finish_harness(harness)
