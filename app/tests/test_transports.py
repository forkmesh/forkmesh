"""Leak-safe clearnet, privacy, overlay, and LAN transports (issue #505).

Drives tools/forkmesh_transports.py against a real SOCKS5 proxy, a Git smart
HTTP host, and a git daemon: common stream and repository conformance for
every adapter, planner vectors, leak tracing of the Git child process (DNS,
direct connections, fallback, environment and repository-config overrides),
log and telemetry disclosure, failover, recovery after a dropped transfer,
health states, address advertisement, and the documentation and gate wiring.
"""

import hashlib
import http.server
import importlib.util
import json
import os
import random
import re
import socket
import subprocess
import sys
import threading
import time
import urllib.parse
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
TOOL_PATH = ROOT / "tools" / "forkmesh_transports.py"
CASES = ROOT / "conformance" / "transports" / "v1" / "cases.json"
DOC = ROOT / "www" / "docs" / "transports.md"


def _load_tool():
    spec = importlib.util.spec_from_file_location("forkmesh_transports", TOOL_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


tool = _load_tool()

ONION = "abcdefghijklmnopqrstuvwxyz234567abcdefghijklmnopqrstuvwx.onion"
PRIVATE_NAME = "git.example.test"
GIT_ENV = {
    **os.environ,
    "GIT_AUTHOR_NAME": "Transport Test", "GIT_AUTHOR_EMAIL": "transport@example.invalid",
    "GIT_COMMITTER_NAME": "Transport Test", "GIT_COMMITTER_EMAIL": "transport@example.invalid",
    "GIT_CONFIG_NOSYSTEM": "1", "GIT_CONFIG_GLOBAL": os.devnull,
}
needs_strace = pytest.mark.skipif(not tool.strace_available(),
                                  reason="leak tracing needs strace on Linux")
posix_only = pytest.mark.skipif(os.name == "nt", reason="git daemon relay is POSIX-only")


def git(cwd, *args, check=True):
    completed = subprocess.run(
        ["git", "-c", "commit.gpgsign=false", "-c", "init.defaultBranch=main", *args],
        cwd=cwd, env=GIT_ENV, capture_output=True, check=False)
    if check:
        assert completed.returncode == 0, completed.stderr.decode()
    return completed.stdout.decode().strip()


def commit(repo, name, content, message):
    (repo / name).write_bytes(content)
    git(repo, "add", name)
    git(repo, "commit", "-q", "-m", message)
    return git(repo, "rev-parse", "HEAD")


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def recv_exact(sock, size):
    data = b""
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise ConnectionError("closed")
        data += chunk
    return data


class Socks5Proxy:
    """A SOCKS5 proxy that records what clients ask it and can misbehave on demand."""

    def __init__(self, mapping=None):
        self.mapping = dict(mapping or {})
        self.requests, self.auth = [], []
        self.connections = 0
        self.drop_after = None
        self.delay = 0.0
        self.refuse_every = 0
        self.lock = threading.Lock()
        self.listener = socket.create_server(("127.0.0.1", 0))
        self.port = self.listener.getsockname()[1]
        threading.Thread(target=self._accept, daemon=True).start()

    @property
    def endpoint(self):
        return "127.0.0.1:%d" % self.port

    def close(self):
        try:
            self.listener.shutdown(socket.SHUT_RDWR)  # wakes a blocked accept()
        except OSError:
            pass
        self.listener.close()

    def _accept(self):
        while True:
            try:
                conn, _ = self.listener.accept()
            except OSError:
                return
            threading.Thread(target=self._handle, args=(conn,), daemon=True).start()

    def _handle(self, conn):
        with self.lock:
            self.connections += 1
            number = self.connections
        try:
            if self.refuse_every and number % self.refuse_every == 0:
                return
            time.sleep(self.delay)
            _, count = recv_exact(conn, 2)
            methods = recv_exact(conn, count)
            if 2 in methods:
                conn.sendall(b"\x05\x02")
                recv_exact(conn, 1)
                user = recv_exact(conn, recv_exact(conn, 1)[0]).decode()
                secret = recv_exact(conn, recv_exact(conn, 1)[0]).decode()
                self.auth.append((user, secret))
                conn.sendall(b"\x01\x00")
            else:
                conn.sendall(b"\x05\x00")
                self.auth.append(None)
            header = recv_exact(conn, 4)
            atyp = header[3]
            if atyp == 3:
                host = recv_exact(conn, recv_exact(conn, 1)[0]).decode()
            elif atyp == 1:
                host = socket.inet_ntop(socket.AF_INET, recv_exact(conn, 4))
            else:
                host = socket.inet_ntop(socket.AF_INET6, recv_exact(conn, 16))
            port = int.from_bytes(recv_exact(conn, 2), "big")
            self.requests.append((atyp, host, port))
            target = self.mapping.get(host, None if atyp == 3 else host)
            if target is None:
                conn.sendall(b"\x05\x04\x00\x01" + bytes(6))
                return
            try:
                upstream = socket.create_connection((target, port), timeout=5)
            except OSError:
                conn.sendall(b"\x05\x05\x00\x01" + bytes(6))
                return
            conn.sendall(b"\x05\x00\x00\x01" + bytes(6))
            self._relay(conn, upstream)
        except (OSError, ConnectionError):
            pass
        finally:
            conn.close()

    def _relay(self, client, upstream):
        def pump(source, sink, limit):
            sent = 0
            try:
                while True:
                    data = source.recv(65536)
                    if not data:
                        break
                    if limit is not None and sent + len(data) > limit:
                        sink.sendall(data[:max(0, limit - sent)])
                        for side in (source, sink):  # shutdown, not close: wakes the other pump
                            try:
                                side.shutdown(socket.SHUT_RDWR)
                            except OSError:
                                pass
                        return
                    sink.sendall(data)
                    sent += len(data)
                sink.shutdown(socket.SHUT_WR)
            except OSError:
                pass

        up = threading.Thread(target=pump, args=(client, upstream, None), daemon=True)
        up.start()
        pump(upstream, client, self.drop_after)
        up.join(timeout=10)
        upstream.close()


class GitHttpHost:
    """Smart HTTP through `git http-backend`, counting every request it receives."""

    def __init__(self, root):
        self.requests = 0
        host = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.0"

            def log_message(self, *args):
                pass

            def do_GET(self):
                self._backend()

            def do_POST(self):
                self._backend()

            def _backend(self):
                host.requests += 1
                path, _, query = self.path.partition("?")
                if self.headers.get("Transfer-Encoding", "").lower() == "chunked":
                    body = b""
                    while True:
                        size = int(self.rfile.readline().strip() or b"0", 16)
                        if not size:
                            self.rfile.readline()
                            break
                        body += self.rfile.read(size)
                        self.rfile.readline()
                else:
                    body = self.rfile.read(int(self.headers.get("Content-Length") or 0))
                env = {**GIT_ENV, "GIT_PROJECT_ROOT": str(root), "GIT_HTTP_EXPORT_ALL": "1",
                       "REQUEST_METHOD": self.command,
                       "PATH_INFO": urllib.parse.unquote(path), "QUERY_STRING": query,
                       "CONTENT_TYPE": self.headers.get("Content-Type", ""),
                       "CONTENT_LENGTH": str(len(body)), "REMOTE_ADDR": "127.0.0.1",
                       "REMOTE_USER": "tester", "GATEWAY_INTERFACE": "CGI/1.1",
                       "HTTP_CONTENT_ENCODING": self.headers.get("Content-Encoding", ""),
                       "GIT_PROTOCOL": self.headers.get("Git-Protocol", "")}
                output = subprocess.run(["git", "http-backend"], input=body, env=env,
                                        capture_output=True, check=False).stdout
                separator = b"\r\n\r\n" if b"\r\n\r\n" in output else b"\n\n"
                head, _, payload = output.partition(separator)
                status, headers = 200, []
                for line in head.decode("latin-1").splitlines():
                    name, _, value = line.partition(":")
                    if name.lower() == "status":
                        status = int(value.strip().split()[0])
                    elif name:
                        headers.append((name, value.strip()))
                self.send_response(status)
                for name, value in headers:
                    self.send_header(name, value)
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.port = self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()


class GitDaemon:
    def __init__(self, root):
        self.port = free_port()
        self.process = subprocess.Popen(
            ["git", "daemon", "--reuseaddr", "--export-all", "--enable=receive-pack",
             "--base-path=%s" % root, "--listen=127.0.0.1", "--port=%d" % self.port, str(root)],
            env=GIT_ENV, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=0.2).close()
                return
            except OSError:
                time.sleep(0.05)
        raise RuntimeError("git daemon did not start")

    def close(self):
        self.process.terminate()
        self.process.wait(timeout=10)


class EchoServer:
    def __init__(self):
        self.listener = socket.create_server(("127.0.0.1", 0))
        self.port = self.listener.getsockname()[1]
        threading.Thread(target=self._accept, daemon=True).start()

    def _accept(self):
        while True:
            try:
                conn, _ = self.listener.accept()
            except OSError:
                return
            threading.Thread(target=self._echo, args=(conn,), daemon=True).start()

    @staticmethod
    def _echo(conn):
        with conn:
            while True:
                data = conn.recv(65536)
                if not data:
                    conn.shutdown(socket.SHUT_WR)
                    return
                conn.sendall(data)

    def close(self):
        try:
            self.listener.shutdown(socket.SHUT_RDWR)  # wakes a blocked accept()
        except OSError:
            pass
        self.listener.close()


@pytest.fixture
def origin(tmp_path):
    root = tmp_path / "served"
    root.mkdir()
    work = tmp_path / "work"
    work.mkdir()
    git(work, "init", "-q", "-b", "main")
    head = commit(work, "README.md", b"# transports\n", "initial")
    git(tmp_path, "clone", "-q", "--bare", str(work), str(root / "repo.git"))
    git(root / "repo.git", "config", "http.receivepack", "true")
    http_host = GitHttpHost(root)
    daemon = GitDaemon(root) if os.name != "nt" else None
    proxy = Socks5Proxy({ONION: "127.0.0.1", PRIVATE_NAME: "127.0.0.1"})
    yield {"root": root, "work": work, "head": head, "http": http_host, "daemon": daemon,
           "proxy": proxy, "tmp": tmp_path}
    proxy.close()
    http_host.close()
    if daemon:
        daemon.close()


def profile(posture, *adapters, latency_ms=None):
    raw = {"schema": tool.PROFILE_SCHEMA, "posture": posture, "adapters": list(adapters)}
    if latency_ms:
        raw["budgets"] = {"latencyMs": latency_ms}
    return tool.validate_profile(raw)


# Every supported adapter, the posture that confines traffic to it, the host a
# URL names, and the state a healthy run must report.
ADAPTERS = {
    "clearnet": (lambda o: profile("standard", {"id": "direct", "kind": "clearnet",
                                                "allowPlaintext": True}),
                 lambda o: "127.0.0.1", "degraded"),
    "proxy": (lambda o: profile("anonymous", {"id": "tor", "kind": "proxy", "network": "tor",
                                              "socks": o["proxy"].endpoint}),
              lambda o: ONION, "anonymous"),
    "overlay": (lambda o: profile("overlay-only", {
                    "id": "mesh", "kind": "overlay", "cidrs": ["127.0.0.0/8"],
                    "authenticatedNetwork": True, "hosts": {"builder.mesh": "127.0.0.1"}}),
                lambda o: "builder.mesh", "protected"),
    "lan": (lambda o: profile("lan-only", {"id": "lan", "kind": "lan", "allowPlaintext": True}),
            lambda o: "127.0.0.1", "degraded"),
}


def remote(origin, scheme, host):
    port = origin["http"].port if scheme == "http" else origin["daemon"].port
    return "%s://%s:%d/repo.git" % (scheme, host, port)


# --- Planner vectors and profile validation ------------------------------------

def _cases():
    return json.loads(CASES.read_text(encoding="utf-8"))


@pytest.mark.parametrize("case", _cases()["plans"], ids=lambda c: c["name"])
def test_planner_matches_conformance_vectors(case):
    raw = _cases()["profiles"][case["profile"]]
    result = tool.public_plan(tool.plan(tool.validate_profile(raw), case["url"]))
    for key, expected in case["expect"].items():
        assert result.get(key) == expected, key
    if "destination" not in case["expect"]:
        assert "destination" not in result


@pytest.mark.parametrize("case", _cases()["profileErrors"], ids=lambda c: c["name"])
def test_profile_validation_reports_the_first_failing_check(case):
    with pytest.raises(tool.UsageError) as raised:
        tool.validate_profile(case["profile"])
    assert str(raised.value).split(":", 1)[0] == case["error"]


def test_vectors_cover_every_code_and_posture():
    cases = _cases()
    reasons = {r["reason"] for c in cases["plans"] for r in c["expect"]["refused"]}
    reasons |= {c["expect"]["reason"] for c in cases["plans"] if c["expect"]["reason"]}
    assert set(tool.ROUTE_REFUSALS) - {"name_needs_dns"} <= reasons
    assert {"invalid_url", "scheme_unsupported", "no_permitted_route"} <= reasons
    assert {c["error"] for c in cases["profileErrors"]} == set(tool.PROFILE_ERRORS)
    assert {p["posture"] for p in cases["profiles"].values()} == set(tool.POSTURES)
    kinds = {a["kind"] for p in cases["profiles"].values() for a in p["adapters"]}
    assert kinds == set(tool.ADAPTER_KINDS)


def test_standard_posture_refuses_nothing_it_should_carry_but_other_postures_need_no_dns():
    lan = profile("lan-only", {"id": "lan", "kind": "lan"}, {"id": "direct", "kind": "clearnet"})
    assert tool.plan(lan, "https://example.org/r.git")["refused"][1]["reason"] == "adapter_not_permitted"
    local = tool.classify_host
    assert local("::ffff:192.168.0.3") == "local-address"
    assert local("100.64.1.1") == "shared-address"
    assert local("192.0.2.1") == "reserved-address"
    assert local("forge.localhost") == "loopback-name"


# --- Common transport conformance -----------------------------------------------

@pytest.mark.parametrize("adapter", sorted(ADAPTERS))
def test_every_adapter_carries_a_verified_stream(origin, adapter):
    make_profile, host_of, _ = ADAPTERS[adapter]
    echo = EchoServer()
    try:
        chosen = make_profile(origin)
        planned = tool.plan(chosen, "https://%s:%d/stream" % (host_of(origin), echo.port))
        assert planned["ok"], planned
        route = planned["routes"][0]
        payload = random.Random(505).randbytes(2 * 1024 * 1024)
        started = time.monotonic()
        sock, guard = tool.open_route(route, planned["_destination"],
                                      credentials=tool.isolation_credentials(route))
        with sock:
            sender = threading.Thread(target=lambda: (sock.sendall(payload),
                                                      sock.shutdown(socket.SHUT_WR)))
            sender.start()
            received = b""
            while True:
                chunk = sock.recv(65536)
                if not chunk:
                    break
                received += chunk
            sender.join()
        elapsed = max(time.monotonic() - started, 1e-6)
        assert hashlib.sha256(received).digest() == hashlib.sha256(payload).digest()
        assert len(payload) / elapsed > 256 * 1024, "throughput below 256 KiB/s on loopback"
        if adapter != "clearnet":
            assert guard is not None and guard.blocked == []
        if adapter == "proxy":
            assert origin["proxy"].requests[-1] == (3, ONION, echo.port)
        healthy = tool.probe_route(route, planned["_destination"], attempts=3)
        assert healthy["state"] == "reachable" and healthy["latencyMs"]["median"] is not None
    finally:
        echo.close()
    closed = tool.probe_route(route, planned["_destination"], attempts=2, timeout=2)
    assert closed["state"] == "unreachable" and closed["failures"] == 2


@pytest.mark.parametrize("scheme", ["http", pytest.param("git", marks=posix_only)])
@pytest.mark.parametrize("adapter", sorted(ADAPTERS))
def test_every_adapter_passes_repository_conformance(origin, adapter, scheme):
    make_profile, host_of, healthy_state = ADAPTERS[adapter]
    chosen = make_profile(origin)
    url = remote(origin, scheme, host_of(origin))
    tmp = origin["tmp"]

    listed = tool.run_git(chosen, url, ["ls-remote", "@URL"], repo=tmp)
    assert listed["git"]["exitCode"] == 0, listed["git"]["stderr"]
    assert origin["head"] in listed["git"]["stdout"]
    assert listed["state"] == healthy_state and listed["exposures"] == []
    if chosen["posture"] != "standard":
        assert listed["leakTrace"] == ("traced" if tool.strace_available() else "unavailable")

    cloned = tool.run_git(chosen, url, ["clone", "-q", "@URL", "checkout"], repo=tmp)
    assert cloned["git"]["exitCode"] == 0, cloned["git"]["stderr"]
    checkout = tmp / "checkout"
    assert git(checkout, "rev-parse", "HEAD") == origin["head"]

    newer = commit(origin["work"], "CHANGES.md", b"more\n", "second")
    git(origin["work"], "push", "-q", str(origin["root"] / "repo.git"), "main")
    fetched = tool.run_git(chosen, url, ["fetch", "-q", "--atomic", "@URL", "main"], repo=checkout)
    assert fetched["git"]["exitCode"] == 0, fetched["git"]["stderr"]
    assert git(checkout, "rev-parse", "FETCH_HEAD") == newer

    git(checkout, "switch", "-q", "-c", "topic")
    pushed_head = commit(checkout, "topic.txt", b"topic\n", "topic")
    pushed = tool.run_git(chosen, url, ["push", "-q", "@URL", "topic"], repo=checkout)
    assert pushed["git"]["exitCode"] == 0, pushed["git"]["stderr"]
    assert git(origin["root"] / "repo.git", "rev-parse", "refs/heads/topic") == pushed_head
    assert all(r["exposures"] == [] for r in (listed, cloned, fetched, pushed))
    if adapter == "proxy":
        assert {atyp for atyp, _, _ in origin["proxy"].requests} == {3}


# --- Leak tests -------------------------------------------------------------------

@needs_strace
def test_private_run_under_hostile_environment_sends_nothing_outside_the_proxy(origin, monkeypatch):
    for name, value in {"http_proxy": "http://127.0.0.1:9", "HTTPS_PROXY": "http://127.0.0.1:9",
                        "ALL_PROXY": "socks5://127.0.0.1:9", "NO_PROXY": "*", "no_proxy": "*",
                        "GIT_TRACE": "1", "GIT_CURL_VERBOSE": "1", "GIT_TRACE_PACKET": "1",
                        "GIT_SSH_COMMAND": "ssh -o ProxyCommand=none",
                        "GIT_PROXY_COMMAND": "/bin/false"}.items():
        monkeypatch.setenv(name, value)
    chosen = profile("private", {"id": "tor", "kind": "proxy", "network": "tor",
                                 "socks": origin["proxy"].endpoint, "allowPlaintext": True})
    url = "http://%s:%d/repo.git" % (PRIVATE_NAME, origin["http"].port)
    report = tool.run_git(chosen, url, ["ls-remote", "@URL"], repo=origin["tmp"], trace="required")
    assert report["git"]["exitCode"] == 0, report["git"]["stderr"]
    assert (report["state"], report["reason"]) == ("degraded", "unauthenticated_peer")
    assert report["exposures"] == [] and report["leakTrace"] == "traced"
    names = [host for atyp, host, _ in origin["proxy"].requests]
    assert names and set(names) == {PRIVATE_NAME}
    assert all(atyp == 3 for atyp, _, _ in origin["proxy"].requests)
    for text in (report["git"]["stderr"], json.dumps(report)):
        assert PRIVATE_NAME not in text and "127.0.0.1" not in text and "trace:" not in text


@needs_strace
def test_no_direct_fallback_when_every_permitted_proxy_is_down(origin):
    dead = free_port()
    chosen = profile("private",
                     {"id": "tor", "kind": "proxy", "network": "tor", "socks": "127.0.0.1:%d" % dead,
                      "allowPlaintext": True},
                     {"id": "direct", "kind": "clearnet", "allowPlaintext": True})
    checkout = origin["tmp"] / "kept"
    git(origin["tmp"], "clone", "-q", str(origin["root"] / "repo.git"), str(checkout))
    before = git(checkout, "for-each-ref")
    report = tool.run_git(chosen, "http://%s:%d/repo.git" % (PRIVATE_NAME, origin["http"].port),
                          ["fetch", "--atomic", "@URL", "main"], repo=checkout, trace="required")
    assert report["state"] == "unreachable" and tool.exit_code_for(report) == tool.EXIT_UNREACHABLE
    assert report["git"] is None, "Git must not run when no permitted route answers"
    assert origin["http"].requests == 0
    assert git(checkout, "for-each-ref") == before
    assert [r["adapter"] for r in report["routes"]] == ["tor"]


@needs_strace
def test_trace_catches_a_regression_that_drops_the_proxy_and_reports_exposed(origin, monkeypatch):
    chosen = profile("private", {"id": "tor", "kind": "proxy", "network": "tor",
                                 "socks": origin["proxy"].endpoint, "allowPlaintext": True})
    real = tool.git_invocation

    def leaky(*args, **kwargs):
        config, env = real(*args, **kwargs)
        index = next(i for i, part in enumerate(config) if part.startswith("http.proxy="))
        return config[:index - 1] + config[index + 1:], env

    monkeypatch.setattr(tool, "git_invocation", leaky)
    report = tool.run_git(chosen, "http://%s:%d/repo.git" % (PRIVATE_NAME, origin["http"].port),
                          ["ls-remote", "@URL"], repo=origin["tmp"], trace="required")
    assert report["state"] == "exposed" and report["tone"] == "danger"
    assert "dns_query" in {e["kind"] for e in report["exposures"]}
    assert tool.exit_code_for(report) == tool.EXIT_EXPOSED
    assert PRIVATE_NAME not in json.dumps(report)


def test_trace_analysis_classifies_resolver_traffic_and_direct_dials():
    trace = "\n".join([
        '10 connect(3, {sa_family=AF_UNIX, sun_path="/var/run/nscd/socket"}, 110) = -1 ENOENT (No such file)',
        '10 connect(3, {sa_family=AF_UNIX, sun_path="/run/systemd/resolve/io.systemd.Resolve"}, 42) = 0',
        '10 connect(3, {sa_family=AF_INET, sin_port=htons(53), sin_addr=inet_addr("127.0.0.53")}, 16) = 0',
        '11 connect(4, {sa_family=AF_INET, sin_port=htons(9050), sin_addr=inet_addr("127.0.0.1")}, 16) = 0',
        '12 connect(5, {sa_family=AF_INET6, sin6_port=htons(443), sin6_flowinfo=htonl(0), '
        'inet_pton(AF_INET6, "2606:4700::1", &sin6_addr), sin6_scope_id=0}, 28) = -1 EINPROGRESS',
        '13 sendto(6, "x", 1, 0, {sa_family=AF_INET, sin_port=htons(5353), sin_addr=inet_addr("224.0.0.251")}, 16) = 1',
        '14 connect(7, {sa_family=AF_UNIX, sun_path="/run/systemd/journal/socket"}, 30) = 0',
    ])
    incidents = tool.analyze_trace(trace, [("127.0.0.1", 9050)], dns="remote")
    assert incidents == [
        {"kind": "dns_query", "via": "resolver_socket"},
        {"kind": "dns_query", "via": "port_53"},
        {"kind": "direct_connection", "addressClass": "public-address"},
        {"kind": "dns_query", "via": "port_5353"},
    ]
    assert tool.analyze_trace(trace, [("127.0.0.1", 9050)], dns="system") == []
    mdns = tool.analyze_trace(trace, [("127.0.0.1", 9050)], dns="mdns")
    assert {"kind": "dns_query", "via": "port_5353"} not in mdns


@needs_strace
def test_trace_sees_a_real_resolver_lookup():
    trace_file = Path(os.environ.get("TMPDIR", "/tmp")) / ("forkmesh-505-%d.trace" % os.getpid())
    subprocess.run(["strace", "-f", "-qq", "-e", "trace=connect,sendto,sendmsg,sendmmsg",
                    "-o", str(trace_file), sys.executable, "-c",
                    "import socket\ntry: socket.getaddrinfo('leak.example.test', 443)\n"
                    "except OSError: pass"], check=False, timeout=60)
    try:
        incidents = tool.analyze_trace(trace_file.read_text(), [], dns="remote")
    finally:
        trace_file.unlink(missing_ok=True)
    if not incidents and not Path("/etc/resolv.conf").exists():
        pytest.skip("no resolver configured to observe")
    assert incidents and {i["kind"] for i in incidents} == {"dns_query"}


def test_in_process_guard_blocks_lookups_and_dials_outside_the_route():
    listener = socket.create_server(("127.0.0.1", 0))
    allowed = listener.getsockname()[1]
    try:
        with tool.LeakGuard([("127.0.0.1", allowed)]) as guard:
            socket.create_connection(("127.0.0.1", allowed), timeout=2).close()
            with pytest.raises(tool.LeakBlocked):
                socket.getaddrinfo("leak.example.test", 443)
            with pytest.raises(tool.LeakBlocked):
                socket.create_connection(("127.0.0.1", free_port()), timeout=2)
            with pytest.raises(tool.LeakBlocked):
                socket.gethostbyaddr("127.0.0.1")
        assert guard.blocked == ["dns_query", "direct_connection", "dns_query"]
        # Outside the guard, and on other threads, nothing is intercepted.
        socket.getaddrinfo("127.0.0.1", 80)
    finally:
        listener.close()


def test_repository_config_that_reroutes_traffic_is_refused_before_anything_is_sent(origin):
    chosen = profile("private", {"id": "tor", "kind": "proxy", "network": "tor",
                                 "socks": origin["proxy"].endpoint, "allowPlaintext": True})
    checkout = origin["tmp"] / "rerouted"
    git(origin["tmp"], "clone", "-q", str(origin["root"] / "repo.git"), str(checkout))
    for key, value in (("url.http://127.0.0.1:1/.insteadOf", "http://%s" % PRIVATE_NAME),
                       ("http.http://%s.proxy" % PRIVATE_NAME, "")):
        git(checkout, "config", key, value)
        with pytest.raises(tool.RouteRefused) as refused:
            tool.run_git(chosen, "http://%s/repo.git" % PRIVATE_NAME,
                         ["fetch", "@URL"], repo=checkout)
        assert refused.value.reason == "repository_config_overrides_route"
        git(checkout, "config", "--unset", key)
    assert origin["proxy"].connections == 0 and origin["http"].requests == 0


def test_stream_isolation_gives_every_anonymous_run_its_own_credentials(origin):
    chosen = ADAPTERS["proxy"][0](origin)
    url = remote(origin, "http", ONION)
    runs = []
    for _ in range(3):
        seen = len(origin["proxy"].auth)
        assert tool.run_git(chosen, url, ["ls-remote", "@URL"], repo=origin["tmp"],
                            trace="off")["git"]["exitCode"] == 0
        # The last connections of a run are Git's; the first is the health probe.
        runs.append(origin["proxy"].auth[seen:])
    assert all(auth is not None and auth[0].startswith("forkmesh-") for run in runs for auth in run)
    git_users = [{auth[0] for auth in run[1:]} for run in runs]
    assert all(len(users) == 1 for users in git_users), "one run shares one circuit"
    assert len(set().union(*git_users)) == len(runs), "every run gets its own circuit"
    assert len({run[0][0] for run in runs} | set().union(*git_users)) == 2 * len(runs)


def test_logs_and_telemetry_disclose_no_destination_proxy_or_credentials(origin, tmp_path):
    chosen = profile("private", {"id": "tor", "kind": "proxy", "network": "tor",
                                 "socks": origin["proxy"].endpoint, "isolateStreams": True,
                                 "allowPlaintext": True})
    url = "http://%s:%d/missing.git" % (PRIVATE_NAME, origin["http"].port)
    telemetry, report_file = tmp_path / "telemetry.jsonl", tmp_path / "report.json"
    completed = cli("git", "--profile", write_profile(tmp_path, chosen), url, "--trace", "off",
                    "--telemetry", telemetry, "--report", report_file, "--", "ls-remote", "@URL")
    assert completed.returncode == tool.EXIT_GIT, completed.stderr
    assert "<redacted>" in completed.stderr
    users = [auth for auth in origin["proxy"].auth if auth]
    leaked = [PRIVATE_NAME, "127.0.0.1", str(origin["proxy"].port), str(origin["http"].port),
              *[part for auth in users for part in auth]]
    for text in (completed.stdout, completed.stderr, telemetry.read_text(), report_file.read_text()):
        for private in leaked:
            assert private not in text
    record = json.loads(telemetry.read_text().splitlines()[0])
    assert record["schema"] == tool.TELEMETRY_SCHEMA and record["posture"] == "private"
    assert all(not isinstance(v, str) or v in _enums() for k, v in record.items()
               if k != "schema")

    anonymous = tmp_path / "anonymous.jsonl"
    completed = cli("git", "--profile", write_profile(tmp_path, ADAPTERS["proxy"][0](origin)),
                    remote(origin, "http", ONION), "--trace", "off", "--telemetry", anonymous,
                    "--json", "--", "ls-remote", "@URL")
    assert completed.returncode == 0, completed.stderr
    assert json.loads(completed.stdout)["telemetry"] == "suppressed"
    assert not anonymous.exists()
    assert ONION not in completed.stdout


def _enums():
    return (set(tool.POSTURES) | set(tool.STATES) | set(tool.DEGRADED_REASONS)
            | {"git_failed", "transfer_failed", "leak_observed", "not_run", "traced",
               "unavailable", "not_required", "git", "probe", "no_permitted_route",
               "connect_failed"})


# --- Failover, recovery, and health states ------------------------------------------

def test_failover_moves_to_the_next_permitted_proxy_and_reports_degraded(origin):
    dead = free_port()
    chosen = profile("private",
                     {"id": "tor-a", "kind": "proxy", "network": "tor", "socks": "127.0.0.1:%d" % dead},
                     {"id": "tor-b", "kind": "proxy", "network": "tor", "socks": origin["proxy"].endpoint})
    report = tool.run_git(chosen, remote(origin, "http", ONION), ["ls-remote", "@URL"],
                          repo=origin["tmp"], trace="off")
    assert report["git"]["exitCode"] == 0
    assert (report["state"], report["reason"]) == ("degraded", "failed_over")
    assert report["activeRoute"] == "tor-b" and report["failovers"] == 1
    assert [r["state"] for r in report["routes"]] == ["unreachable", "reachable"]


def test_dropped_transfer_leaves_refs_untouched_and_recovers_when_the_route_heals(origin):
    big = random.Random(5050).randbytes(600_000)
    newer = commit(origin["work"], "big.bin", big, "large")
    git(origin["work"], "push", "-q", str(origin["root"] / "repo.git"), "main")
    checkout = origin["tmp"] / "recovering"
    git(origin["tmp"], "init", "-q", str(checkout))
    chosen = ADAPTERS["proxy"][0](origin)
    url = remote(origin, "http", ONION)

    origin["proxy"].drop_after = 60_000
    failed = tool.run_git(chosen, url, ["fetch", "--atomic", "@URL", "main:refs/remotes/origin/main"],
                          repo=checkout, trace="off", timeout=120)
    assert failed["state"] == "unreachable" and tool.exit_code_for(failed) == tool.EXIT_UNREACHABLE
    assert git(checkout, "for-each-ref") == ""
    git(checkout, "fsck", "--no-dangling")

    origin["proxy"].drop_after = None
    recovered = tool.run_git(chosen, url, ["fetch", "--atomic", "@URL", "main:refs/remotes/origin/main"],
                             repo=checkout, trace="off")
    assert recovered["git"]["exitCode"] == 0, recovered["git"]["stderr"]
    assert recovered["state"] == "anonymous"
    assert git(checkout, "rev-parse", "refs/remotes/origin/main") == newer


@pytest.mark.parametrize("message", [
    "error: RPC failed; curl 18 transfer closed with outstanding read data remaining",
    "fetch-pack: unexpected disconnect while reading sideband packet",
    "fatal: early EOF",
    "fatal: index-pack failed",
    "fatal: protocol error: bad pack header",
    "fatal: expected 'packfile'",
    "fatal: unable to access '<redacted>': Failed to connect to <redacted> port 9050",
    "fatal: the remote end hung up unexpectedly",
    "fatal: unable to access '<redacted>': Error in the HTTP2 framing layer",
    "error: RPC failed; curl 92 HTTP/2 stream 5 was not closed cleanly: "
    "PROTOCOL_ERROR (err 1)",
    "fatal: unable to access '<redacted>': Operation too slow. Less than 1 bytes/sec "
    "transferred the last 60 seconds",
])
def test_broken_transfers_are_classified_as_route_failures(message):
    assert tool.CONNECTION_FAILURE.search(message)


@pytest.mark.parametrize("message", [
    "fatal: repository '<redacted>' not found",
    "! [rejected]        main -> main (non-fast-forward)",
    "fatal: couldn't find remote ref topic",
])
def test_git_own_failures_are_not_route_failures(message):
    assert not tool.CONNECTION_FAILURE.search(message)


def test_git_failures_that_are_not_the_route_do_not_fail_over(origin):
    chosen = profile("private",
                     {"id": "tor-a", "kind": "proxy", "network": "tor", "socks": origin["proxy"].endpoint,
                      "allowPlaintext": True},
                     {"id": "tor-b", "kind": "proxy", "network": "tor", "socks": origin["proxy"].endpoint,
                      "allowPlaintext": True})
    report = tool.run_git(chosen, "http://%s:%d/absent.git" % (PRIVATE_NAME, origin["http"].port),
                          ["ls-remote", "@URL"], repo=origin["tmp"], trace="off")
    assert report["reason"] == "git_failed" and tool.exit_code_for(report) == tool.EXIT_GIT
    assert len(report["routes"]) == 1


def test_probe_distinguishes_slow_intermittent_and_unreachable_routes(origin):
    echo = EchoServer()
    try:
        url = "https://%s:%d/" % (PRIVATE_NAME, echo.port)
        chosen = profile("private", {"id": "tor", "kind": "proxy", "network": "tor",
                                     "socks": origin["proxy"].endpoint}, latency_ms=20)
        assert tool.probe(chosen, url, attempts=2)["state"] == "protected"
        origin["proxy"].delay = 0.08
        slow = tool.probe(chosen, url, attempts=2)
        assert (slow["state"], slow["reason"]) == ("degraded", "slow")
        origin["proxy"].delay = 0.0
        origin["proxy"].refuse_every = 2
        flaky = tool.probe(chosen, url, attempts=4)
        assert (flaky["state"], flaky["reason"]) == ("degraded", "intermittent")
        origin["proxy"].refuse_every = 0
    finally:
        echo.close()
    gone = tool.probe(chosen, url, attempts=2, timeout=2)
    assert (gone["state"], gone["reason"]) == ("unreachable", "connection_refused")
    assert gone["label"] == "Unreachable" and gone["tone"] == "danger"


def test_states_are_distinct_and_exposure_outranks_everything():
    ok = {"adapter": "a", "state": "reachable", "reason": None}
    slow = {"adapter": "a", "state": "degraded", "reason": "slow"}
    down = {"adapter": "a", "state": "unreachable", "reason": "connect_failed"}
    backup = {"adapter": "b", "state": "reachable", "reason": None}
    assert tool.compose_state("anonymous", [ok]) == ("anonymous", None)
    assert tool.compose_state("private", [ok]) == ("protected", None)
    assert tool.compose_state("lan-only", [ok]) == ("protected", None)
    assert tool.compose_state("standard", [ok]) == ("direct", None)
    assert tool.compose_state("anonymous", [slow]) == ("degraded", "slow")
    assert tool.compose_state("anonymous", [down, backup], "b") == ("degraded", "failed_over")
    assert tool.compose_state("anonymous", [down]) == ("unreachable", "connect_failed")
    assert tool.compose_state("anonymous", [down], exposures=1) == ("exposed", "leak_observed")
    assert tool.compose_state("anonymous", [ok], exposures=1) == ("exposed", "leak_observed")
    assert set(tool.STATE_LABELS) == set(tool.STATE_TONES) == set(tool.STATE_EXPLANATIONS) == set(tool.STATES)
    assert len(set(tool.STATE_LABELS.values())) == len(tool.STATES)


def test_status_command_renders_and_exits_by_state(tmp_path):
    chosen = profile("anonymous", {"id": "tor", "kind": "proxy", "network": "tor",
                                   "socks": "127.0.0.1:9050"})
    for state_routes, exposures, label, code in (
            ([{"adapter": "tor", "kind": "proxy", "state": "reachable", "reason": None,
               "latencyMs": {"median": 40, "max": 50}}], [], "Anonymous", 0),
            ([{"adapter": "tor", "kind": "proxy", "state": "unreachable", "reason": "connect_failed",
               "latencyMs": {"median": None, "max": None}}], [], "Unreachable", 4),
            ([{"adapter": "tor", "kind": "proxy", "state": "reachable", "reason": None,
               "latencyMs": {"median": 40, "max": 50}}], [{"kind": "dns_query", "via": "port_53"}],
             "Exposed", 5)):
        path = tmp_path / "health.json"
        path.write_text(json.dumps(tool.health_report(chosen, state_routes, "tor", exposures)))
        completed = cli("status", path)
        assert completed.returncode == code
        assert completed.stdout.startswith(label + " (anonymous)")


# --- Address advertisement and NAT ----------------------------------------------------

def test_advertisement_withholds_addresses_the_posture_must_not_disclose():
    addresses = ["192.168.1.5", "93.184.216.34", "100.72.1.1", "fe80::1", "127.0.0.1"]
    overlay = {"id": "mesh", "kind": "overlay"}
    standard = tool.advertise(profile("standard", {"id": "direct", "kind": "clearnet"}, overlay),
                              addresses)
    assert standard["addresses"] == ["93.184.216.34", "100.72.1.1", "192.168.1.5", "fe80::1"]
    assert (standard["nat"], standard["inbound"]) == ("public", "direct")

    home = tool.advertise(profile("standard", {"id": "direct", "kind": "clearnet"}), ["192.168.1.5"])
    assert (home["nat"], home["inbound"]) == ("behind_nat", "needs_relay_or_overlay")
    carrier = tool.advertise(profile("standard", {"id": "direct", "kind": "clearnet"}),
                             ["100.72.1.1", "192.168.1.5"])
    assert carrier["nat"] == "carrier_nat"

    private = tool.advertise(profile("private", {"id": "tor", "kind": "proxy", "network": "tor",
                                                 "socks": "127.0.0.1:9050"}), addresses)
    assert private["addresses"] == [] and private["withheld"] == len(addresses)
    assert private["inbound"] == "outbound_only"
    onion = tool.advertise(profile("anonymous", {"id": "tor", "kind": "proxy", "network": "tor",
                                                 "socks": "127.0.0.1:9050", "serviceAddress": ONION}),
                           addresses)
    assert onion["addresses"] == [] and onion["serviceAddresses"] == [ONION]
    assert onion["inbound"] == "onion_service"

    lan = tool.advertise(profile("lan-only", {"id": "lan", "kind": "lan"}), addresses)
    assert lan["addresses"] == ["192.168.1.5", "fe80::1"] and lan["inbound"] == "local_network"
    mesh = tool.advertise(profile("overlay-only", overlay), addresses)
    assert mesh["addresses"] == ["100.72.1.1"] and mesh["inbound"] == "overlay"


# --- CLI, documentation, and gate wiring ------------------------------------------------

def write_profile(directory, normalized):
    raw = {"schema": tool.PROFILE_SCHEMA, "posture": normalized["posture"],
           "budgets": normalized["budgets"], "adapters": []}
    for adapter in normalized["adapters"]:
        entry = {"id": adapter["id"], "kind": adapter["kind"],
                 "allowPlaintext": adapter["allowPlaintext"]}
        if adapter["kind"] == "proxy":
            entry.update(socks="%s:%d" % adapter["socks"], network=adapter["network"],
                         isolateStreams=adapter["isolateStreams"])
        if adapter["kind"] == "overlay":
            entry.update(cidrs=[str(c) for c in adapter["cidrs"]],
                         authenticatedNetwork=adapter["authenticatedNetwork"])
        if adapter["kind"] in ("overlay", "lan"):
            entry["hosts"] = adapter["hosts"]
        raw["adapters"].append(entry)
    path = Path(directory) / ("profile-%s.json" % hashlib.sha256(
        json.dumps(raw, sort_keys=True).encode()).hexdigest()[:8])
    path.write_text(json.dumps(raw))
    return path


def cli(*args):
    return subprocess.run([sys.executable, str(TOOL_PATH), *map(str, args)],
                          capture_output=True, text=True, env=GIT_ENV, check=False, timeout=180)


def test_cli_plan_validate_and_git(origin, tmp_path):
    chosen = ADAPTERS["proxy"][0](origin)
    path = write_profile(tmp_path, chosen)
    assert cli("validate", path).returncode == 0
    refused = cli("plan", "--profile", path, "http://git.example.org/app.git", "--json")
    assert refused.returncode == tool.EXIT_REFUSED
    assert json.loads(refused.stdout)["refused"] == [{"adapter": "tor", "reason": "plaintext_via_exit"}]
    ran = cli("git", "--profile", path, remote(origin, "http", ONION), "--", "ls-remote", "@URL")
    assert ran.returncode == 0, ran.stderr
    assert origin["head"] in ran.stdout
    assert re.search(r"^(Anonymous|Degraded) \(anonymous\)", ran.stderr, re.M)
    bad = tmp_path / "bad.json"
    bad.write_text(json.dumps({"schema": tool.PROFILE_SCHEMA, "posture": "stealth", "adapters": []}))
    invalid = cli("validate", bad)
    assert invalid.returncode == tool.EXIT_USAGE and "unknown_posture" in invalid.stderr
    missing = cli("git", "--profile", path, remote(origin, "http", ONION), "--", "ls-remote")
    assert missing.returncode == tool.EXIT_USAGE and "@URL" in missing.stderr


def test_documentation_covers_the_contract():
    doc = DOC.read_text(encoding="utf-8")
    for command in ("validate", "plan", "probe", "git", "status", "advertise"):
        assert "forkmesh_transports.py %s" % command in doc, command
    for vocabulary in (tool.ADAPTER_KINDS + tool.PROXY_NETWORKS + tool.POSTURES + tool.STATES
                       + tool.PROFILE_ERRORS + tool.ROUTE_REFUSALS + tool.PLAN_REFUSALS
                       + tool.DEGRADED_REASONS + tool.INCIDENT_KINDS):
        assert "`%s`" % vocabulary in doc, vocabulary
    for label in tool.STATE_LABELS.values():
        assert label in doc
    for code in ("0", "2", "3", "4", "5", "6"):
        assert re.search(r"^\| `%s` \|" % code, doc, re.M), code
    assert "(transports.md)" in (ROOT / "www" / "docs" / "README.md").read_text()


def test_quality_gate_and_platform_matrix_run_this_suite():
    config = json.loads((ROOT / "www" / "docs" / "engineering" / "quality-gates.json").read_text())
    gate = next(g for g in config["gates"] if g["id"] == "transport-conformance")
    assert "app/tests/test_transports.py" in gate["command"]
    assert "transport-conformance" in config["profiles"]["core"]
    assert "transport-conformance" in config["profiles"]["release"]
    workflow = (ROOT / ".github" / "workflows" / "transport-conformance.yml").read_text()
    assert "app/tests/test_transports.py" in workflow
    assert "ubuntu-latest" in workflow and "macos-latest" in workflow
    assert "strace" in workflow
