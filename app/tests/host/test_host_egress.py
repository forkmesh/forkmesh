"""The host's outbound guard (egress.py; audit 2026-09-24 M1 and M6)."""

from __future__ import annotations

import asyncio
import socket

import pytest

from forkmesh_host import egress
from forkmesh_host import js


def run(coro):
    return asyncio.run(coro)


@pytest.fixture(autouse=True)
def _vps(monkeypatch):
    monkeypatch.delenv("FORKMESH_HOST_OFFLINE", raising=False)
    monkeypatch.delenv("FORKMESH_EGRESS_ALLOW", raising=False)
    monkeypatch.delenv("FORKMESH_EGRESS_GUARD", raising=False)
    monkeypatch.setenv("FORKMESH_PLATFORM", "vps")
    monkeypatch.setattr(egress, "_last_answers", {})


@pytest.mark.parametrize("address", [
    "127.0.0.1", "127.8.9.1", "10.0.0.1", "10.77.0.2", "172.16.5.4",
    "192.168.1.1", "169.254.169.254", "100.64.0.1", "100.127.255.254",
    "0.0.0.0", "224.0.0.1", "239.1.2.3", "240.0.0.1", "255.255.255.255",
    "198.18.0.1", "192.0.2.1",
    "::1", "::", "fc00::1", "fd12::1", "fe80::1", "ff02::1",
    "::ffff:127.0.0.1", "::ffff:169.254.169.254", "::ffff:10.77.0.1",
    "64:ff9b::a9fe:a9fe", "64:ff9b::7f00:1", "64:ff9b:1::1",
    "2002:7f00:1::1", "2002:a4d:1::", "2001:0:4136:e378::1",
    "2001:db8::1",
])
def test_blocked_addresses(address):
    assert egress.address_blocked(address)


@pytest.mark.parametrize("address", [
    "1.1.1.1", "8.8.8.8", "9.9.9.9", "2606:4700:4700::1111",
    "::ffff:8.8.8.8", "64:ff9b::808:808",
])
def test_public_addresses(address):
    assert not egress.address_blocked(address)


@pytest.mark.parametrize("host,production", [
    ("forkmesh.com", True), ("app.forkmesh.com", True),
    ("mirror35.forkmesh.com", True), ("APP.FORKMESH.COM.", True),
    ("next.forkmesh.com", False), ("app.next.forkmesh.com", False),
    ("n1.next.forkmesh.com", False), ("evilforkmesh.com", False),
    ("forkmesh.com.evil.example", False), ("xnext.forkmesh.com", True),
])
def test_production_hosts(host, production):
    assert egress.production_host(host) is production


def test_an_instance_may_name_the_production_hosts_it_needs():
    environ = {"FORKMESH_EGRESS_PRODUCTION_ALLOW": " APP.forkmesh.com. ,"}
    assert egress.production_host("app.forkmesh.com", environ) is False
    assert egress.production_host("forkmesh.com", environ) is True
    assert egress.production_host("world.forkmesh.com", environ) is True
    assert egress.production_host("app.forkmesh.com", {}) is True


def _fake_dns(monkeypatch, answers):
    async def getaddrinfo(host, port, *args, **kwargs):
        if host not in answers:
            raise socket.gaierror("no such host")
        return [(socket.AF_INET6 if ":" in a else socket.AF_INET,
                 socket.SOCK_STREAM, 6, "", (a, port)) for a in answers[host]]

    probe = asyncio.new_event_loop()
    loop_cls = probe.__class__
    probe.close()
    monkeypatch.setattr(loop_cls, "getaddrinfo",
                        lambda self, *a, **k: getaddrinfo(*a, **k))


def test_resolution_is_checked_and_pinned(monkeypatch):
    _fake_dns(monkeypatch, {
        "good.example": ["93.184.216.34"],
        "internal.example": ["10.77.0.2"],
        "mixed.example": ["93.184.216.34", "169.254.169.254"],
    })
    assert run(egress.resolve_checked("good.example", 443)) == "93.184.216.34"
    with pytest.raises(egress.EgressBlocked):
        run(egress.resolve_checked("internal.example", 443))
    # A single private answer blocks the name (no rebinding lottery).
    with pytest.raises(egress.EgressBlocked):
        run(egress.resolve_checked("mixed.example", 443))
    with pytest.raises(egress.EgressBlocked):
        run(egress.resolve_checked("app.forkmesh.com", 443))


def _flaky_dns(monkeypatch, outcomes):
    calls = []

    async def getaddrinfo(host, port, *args, **kwargs):
        calls.append(host)
        outcome = outcomes.pop(0)
        if isinstance(outcome, int):
            raise socket.gaierror(outcome, "resolver said %d" % outcome)
        return [(socket.AF_INET, socket.SOCK_STREAM, 6, "", (outcome, port))]

    probe = asyncio.new_event_loop()
    loop_cls = probe.__class__
    probe.close()
    monkeypatch.setattr(loop_cls, "getaddrinfo",
                        lambda self, *a, **k: getaddrinfo(*a, **k))
    monkeypatch.setattr(egress, "RESOLVE_RETRY_DELAYS", (0, 0))
    return calls


def test_transient_resolver_failure_is_retried(monkeypatch):
    calls = _flaky_dns(monkeypatch, [
        socket.EAI_AGAIN, socket.EAI_NODATA, "93.184.216.34"])
    assert run(egress.resolve_checked(
        "flaky.example", 443)) == "93.184.216.34"
    assert calls == ["flaky.example"] * 3


def test_resolver_retries_stop_after_the_budget(monkeypatch):
    import httpcore
    calls = _flaky_dns(monkeypatch, [socket.EAI_AGAIN] * 5)
    with pytest.raises(httpcore.ConnectError, match="cannot resolve"):
        run(egress.resolve_checked("flaky.example", 443))
    assert len(calls) == len(egress.RESOLVE_RETRY_DELAYS) + 1


def test_unknown_name_is_not_retried(monkeypatch):
    import httpcore
    calls = _flaky_dns(monkeypatch, [socket.EAI_NONAME, "93.184.216.34"])
    with pytest.raises(httpcore.ConnectError):
        run(egress.resolve_checked("missing.example", 443))
    assert calls == ["missing.example"]


def test_resolver_retry_stays_inside_the_timeout(monkeypatch):
    import httpcore
    calls = _flaky_dns(monkeypatch, [socket.EAI_AGAIN, "93.184.216.34"])
    monkeypatch.setattr(egress, "RESOLVE_RETRY_DELAYS", (5.0,))
    with pytest.raises(httpcore.ConnectError):
        run(egress.resolve_checked("flaky.example", 443, 1.0))
    assert calls == ["flaky.example"]


def test_transient_failure_reuses_the_last_answer(monkeypatch):
    calls = _flaky_dns(monkeypatch, [
        "93.184.216.34", socket.EAI_AGAIN, socket.EAI_AGAIN, socket.EAI_AGAIN])
    assert run(egress.resolve_checked("flaky.example", 443)) == "93.184.216.34"
    before = egress.stats["stale_answers"]
    assert run(egress.resolve_checked("flaky.example", 443)) == "93.184.216.34"
    assert egress.stats["stale_answers"] == before + 1
    assert len(calls) == len(egress.RESOLVE_RETRY_DELAYS) + 2


def test_unknown_name_does_not_reuse_the_last_answer(monkeypatch):
    import httpcore
    _flaky_dns(monkeypatch, ["93.184.216.34", socket.EAI_NONAME])
    assert run(egress.resolve_checked("gone.example", 443)) == "93.184.216.34"
    with pytest.raises(httpcore.ConnectError):
        run(egress.resolve_checked("gone.example", 443))


def test_last_answer_expires(monkeypatch):
    import httpcore
    _flaky_dns(monkeypatch, ["93.184.216.34"] + [socket.EAI_AGAIN] * 3)
    assert run(egress.resolve_checked("old.example", 443)) == "93.184.216.34"
    monkeypatch.setattr(egress, "RESOLVE_STALE_MAX_AGE", 0.0)
    with pytest.raises(httpcore.ConnectError, match="cannot resolve"):
        run(egress.resolve_checked("old.example", 443))


def _hanging_dns_after(monkeypatch, answer):
    calls = []

    async def getaddrinfo(host, port, *args, **kwargs):
        calls.append(host)
        if len(calls) > 1:
            await asyncio.sleep(3600)
        return [(socket.AF_INET, socket.SOCK_STREAM, 6, "", (answer, port))]

    probe = asyncio.new_event_loop()
    loop_cls = probe.__class__
    probe.close()
    monkeypatch.setattr(loop_cls, "getaddrinfo",
                        lambda self, *a, **k: getaddrinfo(*a, **k))
    return calls


def test_slow_lookup_reuses_the_last_answer_before_the_timeout(monkeypatch):
    _hanging_dns_after(monkeypatch, "93.184.216.34")
    monkeypatch.setattr(egress, "RESOLVE_STALE_AFTER", 0.05)
    assert run(egress.resolve_checked("slow.example", 443)) == "93.184.216.34"
    assert run(egress.resolve_checked(
        "slow.example", 443, 30.0)) == "93.184.216.34"


def test_stale_answer_is_checked_again(monkeypatch):
    _flaky_dns(monkeypatch, ["10.0.0.5", socket.EAI_AGAIN, socket.EAI_AGAIN,
                             socket.EAI_AGAIN])
    monkeypatch.setenv("FORKMESH_EGRESS_ALLOW", "inner.example:443")
    assert run(egress.resolve_checked("inner.example", 443)) == "10.0.0.5"
    monkeypatch.delenv("FORKMESH_EGRESS_ALLOW")
    with pytest.raises(egress.EgressBlocked):
        run(egress.resolve_checked("inner.example", 443))


def test_lookup_timeout_names_itself(monkeypatch):
    import httpcore

    async def getaddrinfo(host, port, *args, **kwargs):
        await asyncio.sleep(3600)

    probe = asyncio.new_event_loop()
    loop_cls = probe.__class__
    probe.close()
    monkeypatch.setattr(loop_cls, "getaddrinfo",
                        lambda self, *a, **k: getaddrinfo(*a, **k))
    with pytest.raises(httpcore.ConnectError,
                       match="cannot resolve slow.example: lookup timed out"):
        run(egress.resolve_checked("slow.example", 443, 0.05))


def test_backend_connects_to_the_checked_ip(monkeypatch):
    """The TCP connect uses the IP the check saw, not a second lookup."""
    _fake_dns(monkeypatch, {"good.example": ["93.184.216.34"]})
    connected = []

    class Inner:
        async def connect_tcp(self, host, port, timeout=None,
                              local_address=None, socket_options=None):
            connected.append((host, port))
            return "stream"

        async def sleep(self, seconds):
            return None

    backend = egress.GuardedBackend(Inner())
    assert run(backend.connect_tcp("good.example", 443))._stream == "stream"
    assert connected == [("93.184.216.34", 443)]
    with pytest.raises(egress.EgressBlocked):
        run(backend.connect_tcp("169.254.169.254", 80))
    with pytest.raises(egress.EgressBlocked):
        run(backend.connect_unix_socket("/run/caddy/admin.sock"))


def test_allowlist_and_dev_mode(monkeypatch):
    with pytest.raises(egress.EgressBlocked):
        run(egress.resolve_checked("127.0.0.1", 11434))
    monkeypatch.setenv("FORKMESH_EGRESS_ALLOW", "127.0.0.1:11434")
    assert run(egress.resolve_checked("127.0.0.1", 11434)) == "127.0.0.1"
    with pytest.raises(egress.EgressBlocked):  # port-exact
        run(egress.resolve_checked("127.0.0.1", 2019))
    monkeypatch.delenv("FORKMESH_EGRESS_ALLOW")
    monkeypatch.setenv("FORKMESH_PLATFORM", "dev")
    assert run(egress.resolve_checked("127.0.0.1", 2019)) == "127.0.0.1"
    # Production stays off limits even in development.
    with pytest.raises(egress.EgressBlocked):
        run(egress.resolve_checked("app.forkmesh.com", 443))


@pytest.mark.parametrize("url", [
    "http://127.0.0.1:2019/config/",       # Caddy admin
    "http://169.254.169.254/v1.json",      # Vultr metadata
    "http://10.77.0.2:8792/_rpc",          # mesh
    "http://[::1]:8080/",
    "http://[::ffff:127.0.0.1]:8080/",
    "https://app.forkmesh.com/api/webhooks",
    "https://mirror35.forkmesh.com/v1/catalog",
    "https://forkmesh.com/users/alice/inbox",
])
def test_fetch_refuses_internal_and_production(url):
    async def scenario():
        try:
            with pytest.raises(js.JsException) as caught:
                await js.fetch(url)
            assert caught.value.name == "TypeError"
            assert "blocked" in str(caught.value)
        finally:
            await js.close_http_client()
    run(scenario())


def test_redirect_hops_are_rechecked(monkeypatch):
    """An allowlisted server that redirects to loopback on another port, to
    metadata or to production: each hop is refused."""
    targets = {
        "/to-loopback": "http://localhost:2019/config/",
        "/to-metadata": "http://169.254.169.254/latest",
        "/to-production": "https://app.forkmesh.com/x",
    }

    async def handle(reader, writer):
        head = (await reader.readuntil(b"\r\n\r\n")).decode()
        path = head.split(" ")[1]
        writer.write(b"HTTP/1.1 302 Found\r\nlocation: %s\r\n"
                     b"content-length: 0\r\n\r\n" % targets[path].encode())
        await writer.drain()
        writer.close()

    async def scenario():
        server = await asyncio.start_server(handle, "127.0.0.1", 0)
        port = server.sockets[0].getsockname()[1]
        monkeypatch.setenv("FORKMESH_EGRESS_ALLOW", "127.0.0.1:%d" % port)
        base = "http://127.0.0.1:%d" % port
        try:
            for path in targets:
                manual = await js.fetch(base + path, {"redirect": "manual"})
                assert manual.status == 302
                with pytest.raises(js.JsException) as caught:
                    await js.fetch(base + path)
                assert "blocked" in str(caught.value)
        finally:
            server.close()
            await js.close_http_client()
    run(scenario())


def test_client_ignores_proxy_environment(monkeypatch):
    monkeypatch.setenv("HTTPS_PROXY", "http://127.0.0.1:1")

    async def scenario():
        try:
            client = js._http_client()
            assert client.trust_env is False
        finally:
            await js.close_http_client()
    run(scenario())


def test_client_offers_http2_for_apns_only():
    import h2  # noqa: F401
    import httpx

    async def scenario():
        try:
            client = js._http_client()
            for url in ("https://api.push.apple.com/3/device/x",
                        "https://api.sandbox.push.apple.com/3/device/x"):
                pool = client._transport_for_url(httpx.URL(url))._pool
                assert pool._http2 is True
                assert pool._http1 is True
            for url in ("https://n2.next.forkmesh.com/v1/repositories/a/b",
                        "https://example.com/"):
                pool = client._transport_for_url(httpx.URL(url))._pool
                assert pool._http2 is False
                assert pool._http1 is True
        finally:
            await js.close_http_client()
    run(scenario())


def _tls_pair(tmp_path):
    import datetime
    import ipaddress
    import ssl

    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.x509.oid import NameOID

    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "localhost")])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (x509.CertificateBuilder()
            .subject_name(name).issuer_name(name)
            .public_key(key.public_key())
            .serial_number(x509.random_serial_number())
            .not_valid_before(now - datetime.timedelta(minutes=5))
            .not_valid_after(now + datetime.timedelta(hours=1))
            .add_extension(x509.SubjectAlternativeName([
                x509.DNSName("localhost"),
                x509.IPAddress(ipaddress.ip_address("127.0.0.1"))]),
                critical=False)
            .add_extension(x509.BasicConstraints(ca=True, path_length=None),
                           critical=True)
            .sign(key, hashes.SHA256()))
    cert_path = tmp_path / "cert.pem"
    key_path = tmp_path / "key.pem"
    cert_path.write_bytes(cert.public_bytes(serialization.Encoding.PEM))
    key_path.write_bytes(key.private_bytes(
        serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption()))
    server = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    server.load_cert_chain(cert_path, key_path)
    server.set_alpn_protocols(["h2", "http/1.1"])

    def client_context(*args, **kwargs):
        context = ssl.create_default_context(cafile=str(cert_path))
        return context
    return server, client_context


async def _serve_h2(reader, writer, bodies):
    import h2.config
    import h2.connection
    import h2.events
    import h2.exceptions

    conn = h2.connection.H2Connection(config=h2.config.H2Configuration(
        client_side=False, header_encoding="utf-8"))
    conn.initiate_connection()
    pending = {}
    while True:
        for stream_id in list(pending):
            view = pending[stream_id]
            try:
                while view:
                    window = conn.local_flow_control_window(stream_id)
                    if window <= 0:
                        break
                    size = min(window, conn.max_outbound_frame_size, len(view))
                    conn.send_data(stream_id, view[:size].tobytes())
                    view = view[size:]
                if view:
                    pending[stream_id] = view
                else:
                    conn.end_stream(stream_id)
                    del pending[stream_id]
            except h2.exceptions.StreamClosedError:
                pending.pop(stream_id, None)
        writer.write(conn.data_to_send())
        await writer.drain()
        data = await reader.read(65536)
        if not data:
            return
        for event in conn.receive_data(data):
            if isinstance(event, h2.events.RequestReceived):
                body = bodies[dict(event.headers)[":path"]]
                conn.send_headers(event.stream_id, [
                    (":status", "200"), ("content-length", str(len(body)))])
                pending[event.stream_id] = memoryview(body)
            elif isinstance(event, h2.events.StreamReset):
                pending.pop(event.stream_id, None)
            elif isinstance(event, h2.events.ConnectionTerminated):
                return


async def _serve_http1(reader, writer, bodies):
    while True:
        head = await reader.readuntil(b"\r\n\r\n")
        body = bodies[head.split(b" ")[1].decode()]
        writer.write(b"HTTP/1.1 200 OK\r\ncontent-length: %d\r\n\r\n"
                     % len(body))
        for offset in range(0, len(body), 1 << 16):
            writer.write(body[offset:offset + (1 << 16)])
            await writer.drain()


def test_abandoned_body_does_not_stall_the_next_read_from_that_origin(
        monkeypatch, tmp_path):
    import httpx

    server_context, client_context = _tls_pair(tmp_path)
    monkeypatch.setattr(httpx, "create_ssl_context", client_context)
    monkeypatch.setenv("FORKMESH_PLATFORM", "dev")
    bodies = {"/big": b"b" * (20 * 1024 * 1024),
              "/small": b"s" * (300 * 1024)}
    protocols = []

    async def handle(reader, writer):
        protocol = writer.get_extra_info("ssl_object").selected_alpn_protocol()
        protocols.append(protocol)
        try:
            if protocol == "h2":
                await _serve_h2(reader, writer, bodies)
            else:
                await _serve_http1(reader, writer, bodies)
        except (ConnectionError, asyncio.IncompleteReadError):
            pass
        finally:
            writer.close()

    async def scenario():
        server = await asyncio.start_server(
            handle, "127.0.0.1", 0, ssl=server_context)
        base = "https://localhost:%d" % server.sockets[0].getsockname()[1]
        try:
            for _ in range(2):
                big = await asyncio.wait_for(js.fetch(base + "/big"), 5)
                assert big.status == 200
                await big.body.cancel()
                small = await asyncio.wait_for(js.fetch(base + "/small"), 5)
                assert small.status == 200
                text = await asyncio.wait_for(small.text(), 5)
                assert len(text) == len(bodies["/small"])
        finally:
            server.close()
            await js.close_http_client()
    run(scenario())
    assert protocols and "h2" not in protocols


class _Stream:
    def __init__(self, address, log, tls_ok):
        self.address = address
        self.log = log
        self.tls_ok = tls_ok
        self.closed = False

    async def start_tls(self, ssl_context, server_hostname=None, timeout=None):
        self.log.append(("tls", self.address, server_hostname))
        if not self.tls_ok:
            import httpcore
            raise httpcore.ConnectError("Hostname mismatch")
        return "tls:" + self.address

    async def aclose(self):
        self.closed = True


class _Inner:
    def __init__(self, bad_tls=(), refused=()):
        self.bad_tls = set(bad_tls)
        self.refused = set(refused)
        self.log = []
        self.streams = []

    async def connect_tcp(self, host, port, timeout=None,
                          local_address=None, socket_options=None):
        self.log.append(("tcp", host))
        if host in self.refused:
            import httpcore
            raise httpcore.ConnectError("refused")
        stream = _Stream(host, self.log, host not in self.bad_tls)
        self.streams.append(stream)
        return stream

    async def sleep(self, seconds):
        return None


def test_tls_failure_moves_to_the_next_checked_address(monkeypatch):
    _fake_dns(monkeypatch, {"api.push.apple.com": [
        "54.65.172.3", "2620:149:208:4303::a"]})
    inner = _Inner(bad_tls={"54.65.172.3"})

    async def scenario():
        stream = await egress.GuardedBackend(inner).connect_tcp(
            "api.push.apple.com", 443)
        return await stream.start_tls(None, server_hostname="api.push.apple.com")

    assert run(scenario()) == "tls:2620:149:208:4303::a"
    assert inner.log == [
        ("tcp", "54.65.172.3"),
        ("tls", "54.65.172.3", "api.push.apple.com"),
        ("tcp", "2620:149:208:4303::a"),
        ("tls", "2620:149:208:4303::a", "api.push.apple.com"),
    ]
    assert inner.streams[0].closed


def test_refused_connect_moves_to_the_next_checked_address(monkeypatch):
    _fake_dns(monkeypatch, {"good.example": ["93.184.216.34", "93.184.216.35"]})
    inner = _Inner(refused={"93.184.216.34"})
    stream = run(egress.GuardedBackend(inner).connect_tcp("good.example", 443))
    assert stream._stream.address == "93.184.216.35"


def test_failover_gives_up_after_the_last_address(monkeypatch):
    import httpcore
    _fake_dns(monkeypatch, {"bad.example": ["93.184.216.34", "93.184.216.35"]})
    inner = _Inner(bad_tls={"93.184.216.34", "93.184.216.35"})

    async def scenario():
        stream = await egress.GuardedBackend(inner).connect_tcp("bad.example", 443)
        return await stream.start_tls(None, server_hostname="bad.example")

    with pytest.raises(httpcore.ConnectError):
        run(scenario())
    assert [e for e in inner.log if e[0] == "tls"] == [
        ("tls", "93.184.216.34", "bad.example"),
        ("tls", "93.184.216.35", "bad.example"),
    ]


def test_failover_never_leaves_the_checked_addresses(monkeypatch):
    _fake_dns(monkeypatch, {"mixed.example": ["93.184.216.34", "10.77.0.2"]})
    inner = _Inner()
    with pytest.raises(egress.EgressBlocked):
        run(egress.GuardedBackend(inner).connect_tcp("mixed.example", 443))
    assert inner.log == []


def test_tls_failure_resolves_again_when_the_answer_was_a_single_bad_address(
        monkeypatch):
    answers = iter([["54.65.172.3"], ["54.65.172.3"],
                    ["17.188.169.157", "2620:149:208:4303::a"]])

    async def getaddrinfo(host, port, *args, **kwargs):
        return [(socket.AF_INET6 if ":" in a else socket.AF_INET,
                 socket.SOCK_STREAM, 6, "", (a, port)) for a in next(answers)]

    probe = asyncio.new_event_loop()
    loop_cls = probe.__class__
    probe.close()
    monkeypatch.setattr(loop_cls, "getaddrinfo",
                        lambda self, *a, **k: getaddrinfo(*a, **k))
    inner = _Inner(bad_tls={"54.65.172.3"})

    async def scenario():
        stream = await egress.GuardedBackend(inner).connect_tcp(
            "api.push.apple.com", 443)
        return await stream.start_tls(None, server_hostname="api.push.apple.com")

    assert run(scenario()) == "tls:17.188.169.157"
    assert [e[1] for e in inner.log if e[0] == "tcp"] == [
        "54.65.172.3", "17.188.169.157"]


def test_failover_stops_at_the_attempt_budget(monkeypatch):
    import httpcore
    counter = iter(range(100))

    async def getaddrinfo(host, port, *args, **kwargs):
        a = "93.184.216.%d" % next(counter)
        return [(socket.AF_INET, socket.SOCK_STREAM, 6, "", (a, port))]

    probe = asyncio.new_event_loop()
    loop_cls = probe.__class__
    probe.close()
    monkeypatch.setattr(loop_cls, "getaddrinfo",
                        lambda self, *a, **k: getaddrinfo(*a, **k))

    class AllBad(_Inner):
        async def connect_tcp(self, host, port, **kwargs):
            self.bad_tls.add(host)
            return await super().connect_tcp(host, port, **kwargs)

    inner = AllBad()

    async def scenario():
        stream = await egress.GuardedBackend(inner).connect_tcp("bad.example", 443)
        return await stream.start_tls(None, server_hostname="bad.example")

    with pytest.raises(httpcore.ConnectError):
        run(scenario())
    assert len([e for e in inner.log if e[0] == "tcp"]) == egress.MAX_ADDRESS_ATTEMPTS
