"""Outbound guard for the host's ``fetch`` (audit 2026-09-24, M1 and M6).

Every subrequest the Worker code makes (``js.fetch``) goes through one
httpx client whose network backend is ``GuardedBackend``:

* **Address check at connect time, pinned.** The backend resolves the name
  itself (``getaddrinfo``), refuses the connection when *any* answer is not a
  public address, and connects to the checked IP. TLS SNI and certificate
  verification still use the hostname (httpcore passes the origin host to
  ``start_tls``). Resolution and connection use the same answer, so DNS
  rebinding between a check and the connect is not possible. Pooled
  connections are keyed by hostname, so a reused connection was checked when
  it was opened.
* **Blocked:** loopback, RFC 1918, link-local (169.254.169.254 metadata),
  CGNAT 100.64/10, the core WireGuard 10.77.0.0/24, multicast, reserved and
  unspecified addresses; IPv6 loopback, ULA, link-local, multicast; and
  IPv4-mapped, NAT64 (64:ff9b::/96, 64:ff9b:1::/48), 6to4 and Teredo
  addresses that wrap a blocked (or any hidden) IPv4 address. Anything else
  that ``ipaddress`` does not call global is blocked too.
* **Every hop.** A request hook runs for each request of a redirect chain,
  so each hop is re-checked (hostname rules here, address at connect).
* **Production is off limits.** ``forkmesh.com`` and every
  ``*.forkmesh.com`` other than ``next.forkmesh.com`` / ``*.next.forkmesh.com``
  are refused by name, so Next never delivers webhooks, ActivityPub, mail
  callbacks or probes to production (AGENTS.md).
* **Allowlist.** ``FORKMESH_EGRESS_ALLOW=host:port,...`` lets the operator
  name an internal endpoint the App must reach (for example a local model
  server set as ``AI_API_URL``). The host's own internal traffic does not go
  through ``js.fetch``: rqlite has its own client (d1_rqlite.py), the mesh
  and peer unseal use cluster.py/vault.py, certsync is a separate service.
* **Local development:** with ``FORKMESH_PLATFORM`` set to something other
  than ``vps``, or ``FORKMESH_EGRESS_GUARD=0``, private addresses are allowed
  (the production-name rule still applies).

A refused request fails like a network error in Workers: ``js.fetch`` raises
a ``TypeError``.
"""

from __future__ import annotations

import asyncio
import ipaddress
import os
import socket
import time

import httpcore

WIREGUARD_NET = ipaddress.ip_network("10.77.0.0/24")
_BLOCKED_V4 = [ipaddress.ip_network(n) for n in (
    "0.0.0.0/8", "10.0.0.0/8", "100.64.0.0/10", "127.0.0.0/8",
    "169.254.0.0/16", "172.16.0.0/12", "192.0.0.0/24", "192.0.2.0/24",
    "192.88.99.0/24", "192.168.0.0/16", "198.18.0.0/15", "198.51.100.0/24",
    "203.0.113.0/24", "224.0.0.0/4", "240.0.0.0/4", "255.255.255.255/32")]
_BLOCKED_V6 = [ipaddress.ip_network(n) for n in (
    "::/128", "::1/128", "fc00::/7", "fe80::/10", "fec0::/10", "ff00::/8",
    "100::/64", "2001:db8::/32", "64:ff9b:1::/48", "2001::/32")]
_NAT64 = ipaddress.ip_network("64:ff9b::/96")
PRODUCTION_DOMAIN = "forkmesh.com"
NEXT_DOMAIN = "next.forkmesh.com"


class EgressBlocked(Exception):
    """A subrequest the guard refuses."""


def address_blocked(ip) -> bool:
    if isinstance(ip, str):
        ip = ipaddress.ip_address(ip.split("%", 1)[0])
    if ip.version == 6:
        if ip.ipv4_mapped is not None:
            return address_blocked(ip.ipv4_mapped)
        if ip in _NAT64:
            return address_blocked(ipaddress.IPv4Address(int(ip) & 0xFFFFFFFF))
        if ip.sixtofour is not None:
            return address_blocked(ip.sixtofour)
        if any(ip in net for net in _BLOCKED_V6):
            return True
    else:
        if ip in WIREGUARD_NET or any(ip in net for net in _BLOCKED_V4):
            return True
    return bool(ip.is_loopback or ip.is_private or ip.is_link_local
                or ip.is_multicast or ip.is_reserved or ip.is_unspecified
                or not ip.is_global)


def normalize_host(host) -> str:
    text = str(host or "").strip().lower().rstrip(".")
    if text.startswith("[") and text.endswith("]"):
        text = text[1:-1]
    return text


def serves_production(environ=None) -> bool:
    environ = os.environ if environ is None else environ
    return str(environ.get("FORKMESH_SERVES_PRODUCTION", "")) == "1"


def production_host(host, environ=None) -> bool:
    if serves_production(environ):
        return False
    name = normalize_host(host)
    if name == NEXT_DOMAIN or name.endswith("." + NEXT_DOMAIN):
        return False
    return name == PRODUCTION_DOMAIN or name.endswith("." + PRODUCTION_DOMAIN)


def _allowlist(environ=None):
    environ = os.environ if environ is None else environ
    out = set()
    for item in str(environ.get("FORKMESH_EGRESS_ALLOW", "")).split(","):
        item = item.strip()
        if not item:
            continue
        host, _, port = item.rpartition(":")
        if host and port.isdigit():
            out.add((normalize_host(host), int(port)))
    return out


def allowlisted(host, port, environ=None) -> bool:
    return (normalize_host(host), int(port)) in _allowlist(environ)


def private_allowed(environ=None) -> bool:
    environ = os.environ if environ is None else environ
    if environ.get("FORKMESH_EGRESS_GUARD", "") == "0":
        return True
    platform = str(environ.get("FORKMESH_PLATFORM", "") or "").lower()
    return bool(platform) and platform != "vps"


stats = {"blocked": 0, "stale_answers": 0}


def _refuse(reason):
    stats["blocked"] += 1
    raise EgressBlocked(reason)


def check_url_host(host, port, scheme):
    """The name-level rules for one hop (before any connection)."""
    if scheme not in ("http", "https"):
        _refuse("scheme %s not allowed" % scheme)
    if production_host(host):
        _refuse("production host %s is off limits for Next" % normalize_host(host))
    del port


RESOLVE_RETRY_DELAYS = (0.25, 0.5)
TRANSIENT_RESOLVE_ERRORS = frozenset(code for code in (
    socket.EAI_AGAIN, getattr(socket, "EAI_NODATA", None),
    getattr(socket, "EAI_FAIL", None)) if code is not None)


async def _getaddrinfo(name, port, timeout):
    loop = asyncio.get_running_loop()
    deadline = loop.time() + timeout
    delays = list(RESOLVE_RETRY_DELAYS)
    while True:
        try:
            return await asyncio.wait_for(
                loop.getaddrinfo(name, port, type=socket.SOCK_STREAM),
                max(0.0, deadline - loop.time()))
        except socket.gaierror as error:
            if (error.errno not in TRANSIENT_RESOLVE_ERRORS or not delays
                    or loop.time() + delays[0] >= deadline):
                raise
            await asyncio.sleep(delays.pop(0))


RESOLVE_STALE_AFTER = 2.0
RESOLVE_STALE_MAX_AGE = 3600.0
RESOLVE_STALE_NAMES = 512
_last_answers = {}


def _remember(name, addresses):
    _last_answers.pop(name, None)
    _last_answers[name] = (time.monotonic(), list(addresses))
    while len(_last_answers) > RESOLVE_STALE_NAMES:
        _last_answers.pop(next(iter(_last_answers)))


def _stale_answer(name):
    entry = _last_answers.get(name)
    if entry is None or time.monotonic() - entry[0] >= RESOLVE_STALE_MAX_AGE:
        return None
    return list(entry[1])


async def _lookup(name, port, timeout):
    stale = _stale_answer(name)
    budget = min(timeout, RESOLVE_STALE_AFTER) if stale else timeout
    try:
        infos = await _getaddrinfo(name, port, budget)
    except socket.gaierror as error:
        if stale is None or error.errno not in TRANSIENT_RESOLVE_ERRORS:
            raise
        stats["stale_answers"] += 1
        return stale
    except (OSError, asyncio.TimeoutError):
        if stale is None:
            raise
        stats["stale_answers"] += 1
        return stale
    addresses = []
    for info in infos:
        address = info[4][0]
        if address not in addresses:
            addresses.append(address)
    if addresses:
        _remember(name, addresses)
    return addresses


async def resolve_checked(host, port, timeout=None):
    """The one IP to connect to, or EgressBlocked."""
    return (await resolve_all_checked(host, port, timeout))[0]


async def resolve_all_checked(host, port, timeout=None):
    name = normalize_host(host)
    if production_host(name):
        _refuse("production host %s is off limits for Next" % name)
    exempt = allowlisted(name, port) or private_allowed()
    try:
        literal = ipaddress.ip_address(name.split("%", 1)[0])
    except ValueError:
        literal = None
    if literal is not None:
        if not exempt and address_blocked(literal):
            _refuse("address %s is not public" % literal)
        return [str(literal)]
    try:
        addresses = await _lookup(name, port, timeout or 10.0)
    except (OSError, asyncio.TimeoutError) as error:
        raise httpcore.ConnectError("cannot resolve %s: %s" % (
            name, str(error) or "lookup timed out"))
    if not addresses:
        raise httpcore.ConnectError("cannot resolve %s" % name)
    if not exempt:
        for address in addresses:
            if address_blocked(address):
                _refuse("%s resolves to a non-public address" % name)
    return addresses[:MAX_ADDRESS_ATTEMPTS]


def _default_backend():
    try:
        from httpcore._backends.auto import AutoBackend
        return AutoBackend()
    except ImportError:  # pragma: no cover - httpcore layout changed
        return httpcore.AnyIOBackend()


MAX_ADDRESS_ATTEMPTS = 4
MAX_LOOKUPS = 8
_RETRYABLE = (httpcore.ConnectError, httpcore.ConnectTimeout,
              httpcore.ReadError, httpcore.WriteError, OSError)


async def _connect_next(inner, pending, tried, port, options):
    while True:
        address = pending.pop(0)
        tried.append(address)
        try:
            return await inner.connect_tcp(address, port, **options)
        except _RETRYABLE:
            if not pending or len(tried) >= MAX_ADDRESS_ATTEMPTS:
                raise


class FailoverStream(httpcore.AsyncNetworkStream):
    def __init__(self, inner, host, port, options, stream, pending, tried):
        self._inner = inner
        self._host = host
        self._port = port
        self._options = options
        self._stream = stream
        self._pending = list(pending)
        self._tried = tried

    async def read(self, max_bytes, timeout=None):
        return await self._stream.read(max_bytes, timeout=timeout)

    async def write(self, buffer, timeout=None):
        await self._stream.write(buffer, timeout=timeout)

    async def aclose(self):
        await self._stream.aclose()

    def get_extra_info(self, info):
        return self._stream.get_extra_info(info)

    async def start_tls(self, ssl_context, server_hostname=None, timeout=None):
        while True:
            try:
                return await self._stream.start_tls(
                    ssl_context, server_hostname=server_hostname,
                    timeout=timeout)
            except _RETRYABLE:
                if len(self._tried) >= MAX_ADDRESS_ATTEMPTS:
                    raise
                lookups = 0
                while not self._pending and lookups < MAX_LOOKUPS:
                    lookups += 1
                    fresh = await resolve_all_checked(
                        self._host, self._port, self._options["timeout"])
                    self._pending = [a for a in fresh if a not in self._tried]
                if not self._pending:
                    raise
                try:
                    await self._stream.aclose()
                except Exception:
                    pass
                self._stream = await _connect_next(
                    self._inner, self._pending, self._tried, self._port,
                    self._options)


class GuardedBackend(httpcore.AsyncNetworkBackend):
    """httpcore network backend: resolve, check, connect to the pinned IP."""

    def __init__(self, inner=None):
        self._inner = inner or _default_backend()

    async def connect_tcp(self, host, port, timeout=None, local_address=None,
                          socket_options=None):
        pending = await resolve_all_checked(host, port, timeout)
        options = {"timeout": timeout, "local_address": local_address,
                   "socket_options": socket_options}
        tried = []
        stream = await _connect_next(self._inner, pending, tried, port,
                                     options)
        return FailoverStream(self._inner, host, port, options, stream,
                              pending, tried)

    async def connect_unix_socket(self, path, timeout=None,
                                  socket_options=None):
        _refuse("unix sockets are not reachable through fetch")

    async def sleep(self, seconds):
        await self._inner.sleep(seconds)


async def request_hook(request):
    """httpx event hook: runs for the first request and every redirect."""
    url = request.url
    check_url_host(url.host, url.port, url.scheme)


HTTP2_ONLY_HOSTS = ("api.push.apple.com", "api.sandbox.push.apple.com")


def guarded_transport(limits, http2=False):
    """An httpx transport whose connections go through ``GuardedBackend``
    (no proxy or certificate settings from the environment)."""
    import httpx
    transport = httpx.AsyncHTTPTransport(limits=limits, trust_env=False)
    transport._pool = httpcore.AsyncConnectionPool(
        ssl_context=httpx.create_ssl_context(trust_env=False),
        max_connections=limits.max_connections,
        max_keepalive_connections=limits.max_keepalive_connections,
        keepalive_expiry=limits.keepalive_expiry,
        http1=True,
        http2=bool(http2),
        network_backend=GuardedBackend())
    return transport


def http2_mounts(limits):
    transport = guarded_transport(limits, http2=True)
    return {"all://" + host: transport for host in HTTP2_ONLY_HOSTS}
