"""Front-door checks the ASGI layer applies before the Worker sees a request.

* **Host allowlist.** On a VPS node the App answers only its configured
  public names: ``FORKMESH_PUBLIC_HOSTS`` (comma list rendered into app.env:
  app.next, world.next and every nN.next name) plus the hosts of
  ``APP_ORIGIN`` / ``WORLD_ORIGIN``. Anything else (``localhost``,
  ``127.0.0.1``, a raw IP, a foreign name) gets ``421``. Only ``/health``,
  ``/readyz`` and ``GET /api/version`` from a loopback peer are exempt: the
  deploy gate and the unseal timer curl ``127.0.0.1:8080`` directly. This
  closes the "local Host header" paths in the Worker (the demo bootstrap).
* **Body cap.** 16 MiB by default (``FORKMESH_MAX_BODY_BYTES``), 32 MiB for
  a pull submission ``POST /api/repo/<owner>/<repo>/pulls``
  (``FORKMESH_MAX_PULL_BODY_BYTES``), 256 MiB for a Git push
  ``POST /<owner>/<repo>/git-receive-pack`` (``FORKMESH_MAX_GIT_PUSH_BODY_BYTES``;
  the pack streams through to a core gateway, never buffered, and a
  ForkMesh-hosted repository's own size limit is checked before it is
  proxied, docs/HOSTED_REPOS.md). Caddy mirrors all three numbers
  (deploy/core/render.py). A ``Content-Length`` above the cap is refused
  before any byte is read; a chunked body is counted while it streams and
  stops at the cap, so it is never buffered past it.
* **Edge client.** ``x-forkmesh-edge-client`` from the homepage Worker is
  verified per deploy/core/edge_client.md and only then replaces
  ``cf-connecting-ip`` / ``cf-ipcountry``. The header never reaches the
  Worker code.
"""

from __future__ import annotations

import base64
import hashlib
import hmac
import ipaddress
import os
import re
import time
import urllib.parse

DEFAULT_MAX_BODY = 16 * 1024 * 1024
DEFAULT_MAX_PULL_BODY = 32 * 1024 * 1024
DEFAULT_MAX_GIT_PUSH_BODY = 256 * 1024 * 1024
# Keep in sync with deploy/core/render.py (Caddy request_body).
PULL_SUBMIT_RE = re.compile(r"^/api/repo/[^/]+/[^/]+/pulls$")
GIT_PUSH_RE = re.compile(r"^/[^/]+/[^/]+/git-receive-pack$")
LOOPBACK_EXEMPT = {"/health", "/readyz"}
LOOPBACK_EXEMPT_GET = {"/api/version"}
EDGE_HEADER = "x-forkmesh-edge-client"
EDGE_WINDOW_S = 300
_COUNTRY_RE = re.compile(r"^(?:[A-Z]{2})?$")


class BodyTooLarge(Exception):
    """The streamed request body passed the cap."""


class ClientDisconnected(ConnectionResetError):
    pass


TOO_MANY_FIELDS = "Max number of fields exceeded"


def is_too_many_fields(error):
    return isinstance(error, ValueError) and TOO_MANY_FIELDS in str(error)


def _int_env(name, default):
    try:
        value = int(os.environ.get(name, "") or default)
    except ValueError:
        return default
    return value if value > 0 else default


def body_limit(method, path):
    if method == "POST" and PULL_SUBMIT_RE.match(path or ""):
        return _int_env("FORKMESH_MAX_PULL_BODY_BYTES", DEFAULT_MAX_PULL_BODY)
    if method == "POST" and GIT_PUSH_RE.match(path or ""):
        return _int_env("FORKMESH_MAX_GIT_PUSH_BODY_BYTES",
                        DEFAULT_MAX_GIT_PUSH_BODY)
    return _int_env("FORKMESH_MAX_BODY_BYTES", DEFAULT_MAX_BODY)


def declared_length(scope):
    """``Content-Length`` as an int, None when absent, -1 when malformed."""
    for name, value in scope.get("headers") or []:
        if name == b"content-length":
            text = value.decode("latin-1").strip()
            if not text.isdigit():
                return -1
            return int(text)
    return None


def is_loopback(peer):
    if not peer:
        return False
    try:
        return ipaddress.ip_address(str(peer).split("%")[0]).is_loopback
    except ValueError:
        return False


def _host_of(value):
    """Hostname (lowercase, no port, no trailing dot) of a Host header or URL."""
    text = str(value or "").strip()
    if not text:
        return ""
    if "://" in text:
        text = urllib.parse.urlsplit(text).netloc
    text = text.rsplit("@", 1)[-1]
    if text.startswith("["):
        end = text.find("]")
        host = text[1:end] if end > 0 else text
    else:
        host = text.split(":", 1)[0] if text.count(":") <= 1 else text
    return host.strip().rstrip(".").lower()


class HostAllowlist:
    """Allowed Host names, recomputed only when the inputs change."""

    def __init__(self):
        self._key = None
        self._hosts = frozenset()

    @staticmethod
    def _value(env, name):
        value = getattr(env, name, None) if env is not None else None
        if value is None or not isinstance(value, str):
            value = os.environ.get(name, "")
        return value or ""

    def enforced(self, env):
        platform = self._value(env, "FORKMESH_PLATFORM") or "vps"
        return platform.lower() == "vps" or bool(
            os.environ.get("FORKMESH_PUBLIC_HOSTS"))

    def hosts(self, env):
        key = (os.environ.get("FORKMESH_PUBLIC_HOSTS", ""),
               self._value(env, "APP_ORIGIN"), self._value(env, "WORLD_ORIGIN"))
        if key != self._key:
            names = {_host_of(item) for item in key[0].split(",")}
            names.add(_host_of(key[1]))
            names.add(_host_of(key[2]))
            names.discard("")
            self._hosts = frozenset(names)
            self._key = key
        return self._hosts

    def allows(self, scope, env):
        if not self.enforced(env):
            return True
        path = scope.get("path", "/")
        peer = (scope.get("client") or (None,))[0]
        if is_loopback(peer) and (
                path in LOOPBACK_EXEMPT or (
                    path in LOOPBACK_EXEMPT_GET
                    and scope.get("method", "GET").upper() in ("GET", "HEAD"))):
            return True
        host = ""
        for name, value in scope.get("headers") or []:
            if name == b"host":
                host = _host_of(value.decode("latin-1"))
                break
        return bool(host) and host in self.hosts(env)


def _b64url(data):
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode("ascii")


def edge_message(ip, country, ts, method, path):
    return "\n".join(("v1", ip, country, str(ts), method.upper(), path))


def edge_sign(secret, ip, country, ts, method, path):
    mac = hmac.new(secret.encode("utf-8"),
                   edge_message(ip, country, ts, method, path).encode("utf-8"),
                   hashlib.sha256).digest()
    return ",".join(("v1", ip, country, str(ts), _b64url(mac)))


class EdgeClientVerifier:
    """deploy/core/edge_client.md, plus a single-use MAC cache: a captured
    header is accepted once per window (a replay falls back to the peer)."""

    def __init__(self, window=EDGE_WINDOW_S, max_seen=20000, clock=time.time):
        self.window = window
        self.max_seen = max_seen
        self.clock = clock
        self._seen: dict = {}
        self.accepted = 0
        self.rejected = 0

    def _prune(self, now):
        if len(self._seen) < self.max_seen:
            return
        horizon = now - self.window
        for mac, ts in list(self._seen.items()):
            if ts < horizon:
                del self._seen[mac]
        while len(self._seen) >= self.max_seen:
            self._seen.pop(next(iter(self._seen)))

    def verify(self, value, secret, method, path, consume=True):
        """``(ip, country)`` when the header is valid, else None."""
        if not value or not secret:
            return None
        parts = str(value).split(",")
        if len(parts) != 5 or parts[0] != "v1":
            self.rejected += 1
            return None
        _, ip, country, ts_text, mac = parts
        try:
            ts = int(ts_text)
            ipaddress.ip_address(ip)
        except ValueError:
            self.rejected += 1
            return None
        now = int(self.clock())
        if not _COUNTRY_RE.match(country) or abs(now - ts) > self.window:
            self.rejected += 1
            return None
        expected = edge_sign(secret, ip, country, ts, method, path)
        if not hmac.compare_digest(expected.encode(), str(value).encode()):
            self.rejected += 1
            return None
        if mac in self._seen:
            self.rejected += 1
            return None
        if not consume:
            return ip, country
        self._prune(now)
        self._seen[mac] = ts
        self.accepted += 1
        return ip, country
