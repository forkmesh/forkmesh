#!/usr/bin/env python3
"""Leaderless sharing of the app/world TLS certificate between core nodes.

No object storage, no DNS token and no fixed issuer. Every core node runs the
same code and config shape; for each shared name (app.next, world.next) the
node that should issue or renew is chosen deterministically among the
**live** core nodes: rendezvous hashing on the hostname over the nodes whose
certsync server answers (self included). If that node dies, the next one in
rendezvous order takes over at the next sync.

``serve`` (user fmnext-certsync, always on)
    * on the WireGuard address: ``GET /v1/health`` and ``GET /v1/tls/<name>``
      from an allowed peer address, HMAC-authenticated under
      FORKMESH_CERTSYNC_SECRET (a systemd credential only certsync loads;
      the app, which holds the mesh secret, cannot fetch the private keys);
      ``/v1/tls`` returns the pair
      this node exported (whatever it holds, issued here or pulled), with the
      body MACed over the request nonce;
    * on 127.0.0.1 (``challenge_listen``): the HTTP-01 forwarder. Caddy on
      every node routes ``/.well-known/acme-challenge/*`` for the shared names
      here (unless Caddy is solving on this node itself); it asks each peer's
      Caddy on port 80 over WireGuard, in rendezvous order, and returns the
      first answer, so Let's Encrypt succeeds whichever A record it picks.
      Forwarded requests carry ``x-fmnext-acme-hop`` and are never forwarded
      again (no loops).
``sync`` (root oneshot, every 5 minutes and at boot)
    1. collect the local pairs (Caddy's storage, except its ``local``
       issuer, and ``tls_dir``), validate them (PEM shape, key matches,
       chains to a system CA for TLS server use, covers the name, valid now
       and for at most ``max_days`` more) and read their expiry;
    2. find the live peers (``/v1/health``) and fetch their pairs; keep the
       freshest valid pair (latest notAfter) from anywhere;
    3. install it in ``tls_dir`` (0600, owner caddy) when it is fresher than
       the one there, and export it for the peers (0640 root:fmnext-certsync);
    4. choose a mode per name and write the Caddy include files:
       ``files`` (serve the pair from ``tls_dir``); ``acme`` (this node is
       the rendezvous-first live node and the freshest pair is missing or
       within ``renew_days`` of expiry: Caddy obtains/renews it over
       HTTP-01, and the next sync exports it); ``internal`` (no pair anywhere
       yet and another node is issuing: a local self-signed certificate, so
       the node stays out of DNS until it has pulled the real one);
    5. reload Caddy only if a file changed.
``prepare`` (root, Caddy's ExecStartPre; no network)
    Steps 1, 3 and 4 with local pairs only, so Caddy can always start.

Request auth header::

    x-fmnext-certsync: v1:<unix seconds>:<nonce>:<hex hmac>
    hmac = HMAC-SHA256(secret, "fmnext-certsync-v1\\nGET\\n<path>\\n<ts>\\n<nonce>\\n<node>")
    x-fmnext-node: <requesting node name>

Response MAC header::

    x-fmnext-certsync-body: <hex HMAC-SHA256(secret,
        "fmnext-certsync-body-v1\\n<nonce>\\n<sha256 hex of body>")>
"""

from __future__ import annotations

import argparse
import calendar
import grp
import hashlib
import hmac
import http.server
import json
import os
import pwd
import re
import secrets
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

AUTH_HEADER = "x-fmnext-certsync"
NODE_HEADER = "x-fmnext-node"
BODY_MAC_HEADER = "x-fmnext-certsync-body"
SKEW_SECONDS = 60
_NAME = re.compile(r"^[a-z0-9](?:[a-z0-9.-]{0,251}[a-z0-9])?$")
_NONCE = re.compile(r"^[0-9a-f]{16,64}$")
_NODE = re.compile(r"^[a-z0-9][a-z0-9-]{0,31}$")


class CertSyncError(RuntimeError):
    pass


# --------------------------------------------------------------------------
# config and secret


def load_config(path):
    with open(path, "r", encoding="utf-8") as handle:
        config = json.load(handle)
    names = config.get("names") or []
    if not names or not all(isinstance(n, str) and _NAME.match(n) for n in names):
        raise CertSyncError("config names must be DNS names")
    return config


def read_secret(config, environ=None):
    environ = os.environ if environ is None else environ
    name = config.get("secret_credential", "FORKMESH_CERTSYNC_SECRET")
    directory = environ.get("CREDENTIALS_DIRECTORY", "")
    if not directory:
        raise CertSyncError("CREDENTIALS_DIRECTORY is not set (run under systemd)")
    value = Path(directory, name).read_bytes().strip()
    if len(value) < 32:
        raise CertSyncError("%s is shorter than 32 bytes" % name)
    return value


def read_replica_secret(config, environ=None):
    name = config.get("replica_secret_credential")
    if not name:
        return None
    environ = os.environ if environ is None else environ
    path = Path(environ.get("CREDENTIALS_DIRECTORY", ""), name)
    if not environ.get("CREDENTIALS_DIRECTORY") or not path.is_file():
        return None
    value = path.read_bytes().strip()
    return value if len(value) >= 32 else None


# --------------------------------------------------------------------------
# MACs


def request_mac(secret: bytes, path: str, ts: int, nonce: str, node: str) -> str:
    message = "fmnext-certsync-v1\nGET\n%s\n%d\n%s\n%s" % (path, ts, nonce, node)
    return hmac.new(secret, message.encode(), hashlib.sha256).hexdigest()


def body_mac(secret: bytes, nonce: str, body: bytes) -> str:
    message = "fmnext-certsync-body-v1\n%s\n%s" % (
        nonce, hashlib.sha256(body).hexdigest())
    return hmac.new(secret, message.encode(), hashlib.sha256).hexdigest()


def auth_headers(secret: bytes, path: str, node: str, now=None, nonce=None):
    ts = int(time.time() if now is None else now)
    nonce = nonce or secrets.token_hex(16)
    mac = request_mac(secret, path, ts, nonce, node)
    return {AUTH_HEADER: "v1:%d:%s:%s" % (ts, nonce, mac), NODE_HEADER: node}, nonce


class NonceCache:
    def __init__(self, ttl=2 * SKEW_SECONDS):
        self.ttl = ttl
        self.seen = {}
        self.lock = threading.Lock()

    def add(self, nonce, now):
        with self.lock:
            for key in [k for k, t in self.seen.items() if now - t > self.ttl]:
                del self.seen[key]
            if nonce in self.seen:
                return False
            self.seen[nonce] = now
            return True


def verify_request(secret, path, headers, nonces, now=None):
    """Return the nonce of a valid request, else raise CertSyncError."""
    now = time.time() if now is None else now
    value = headers.get(AUTH_HEADER) or ""
    node = headers.get(NODE_HEADER) or ""
    parts = value.split(":")
    if len(parts) != 4 or parts[0] != "v1" or not _NODE.match(node):
        raise CertSyncError("malformed auth")
    _, ts_text, nonce, mac = parts
    if not ts_text.isdigit() or not _NONCE.match(nonce):
        raise CertSyncError("malformed auth")
    ts = int(ts_text)
    if abs(now - ts) > SKEW_SECONDS:
        raise CertSyncError("stale request")
    expected = request_mac(secret, path, ts, nonce, node)
    if not hmac.compare_digest(expected, mac):
        raise CertSyncError("bad mac")
    if not nonces.add(nonce, now):
        raise CertSyncError("replayed nonce")
    return nonce


# --------------------------------------------------------------------------
# files


def write_if_changed(path: Path, data: bytes, mode: int, uid=-1, gid=-1) -> bool:
    try:
        if path.read_bytes() == data:
            if (path.stat().st_mode & 0o777) != mode:
                os.chmod(path, mode)
            return False
    except FileNotFoundError:
        pass
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".certsync-", dir=path.parent)
    try:
        os.fchmod(fd, mode)
        if uid != -1 or gid != -1:
            os.fchown(fd, uid, gid)
        with os.fdopen(fd, "wb") as handle:
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    return True


# Caddy's own internal CA (``tls internal``): never shared, never trusted.
LOCAL_ISSUER_DIRS = ("local",)


def storage_pairs(storage: Path, name: str):
    """Every <issuer dir>/<name>/<name>.{crt,key} in Caddy's storage, except
    the ``local`` (internal CA) issuer."""
    pairs = []
    if not storage.is_dir():
        return pairs
    for issuer_dir in sorted(storage.iterdir()):
        if issuer_dir.name in LOCAL_ISSUER_DIRS:
            continue
        crt = issuer_dir / name / (name + ".crt")
        key = issuer_dir / name / (name + ".key")
        if crt.is_file() and key.is_file():
            pairs.append((crt, key))
    return pairs


def newest_storage_pair(storage: Path, name: str):
    """Newest pair by file time (kept for tools that only need one)."""
    pairs = storage_pairs(storage, name)
    if not pairs:
        return None
    return max(pairs, key=lambda pair: pair[0].stat().st_mtime)


# --------------------------------------------------------------------------
# validation


def _openssl(args, data=None):
    completed = subprocess.run(["openssl", *args], input=data,
                               capture_output=True, check=False)
    return completed.returncode, completed.stdout


MAX_DAYS = 100
NOT_BEFORE_SKEW = 300
_PEM_CERT = re.compile(
    rb"-----BEGIN CERTIFICATE-----\r?\n[A-Za-z0-9+/=\r\n]+-----END CERTIFICATE-----")


def _pem_time(name, text, field):
    text = text.decode("ascii", "replace").strip()
    if not text.startswith(field + "="):
        raise CertSyncError("%s: certificate has no %s" % (name, field))
    try:
        return calendar.timegm(time.strptime(
            text[len(field) + 1:].replace("  ", " "), "%b %d %H:%M:%S %Y %Z"))
    except ValueError:
        raise CertSyncError("%s: unreadable %s %r" % (name, field, text))


def openssl_validate(name: str, cert: bytes, key: bytes, ca_file=None,
                     max_days=MAX_DAYS, now=None) -> int:
    """Validate the pair and return the certificate's notAfter (unix time).

    Raise CertSyncError unless the key matches the certificate, the leaf
    chains (through the intermediates in ``cert``) to a trusted CA for TLS
    server use (the system store, or ``ca_file``), covers ``name``, is
    already valid, has not expired and expires within ``max_days``. A
    self-signed or long-lived certificate from a compromised peer never
    wins ``freshest``."""
    now = time.time() if now is None else now
    blocks = _PEM_CERT.findall(cert)
    if not blocks:
        raise CertSyncError("%s: certificate does not parse" % name)
    with tempfile.TemporaryDirectory() as tmp:
        crt_path = Path(tmp, "c.pem")
        leaf_path = Path(tmp, "leaf.pem")
        chain_path = Path(tmp, "chain.pem")
        key_path = Path(tmp, "k.pem")
        crt_path.write_bytes(cert)
        leaf_path.write_bytes(blocks[0] + b"\n")
        chain_path.write_bytes(b"\n".join(blocks[1:]) + b"\n")
        key_path.write_bytes(key)
        os.chmod(key_path, 0o600)
        code, cert_pub = _openssl(["x509", "-in", str(crt_path), "-noout", "-pubkey"])
        if code != 0:
            raise CertSyncError("%s: certificate does not parse" % name)
        code, key_pub = _openssl(["pkey", "-in", str(key_path), "-pubout"])
        if code != 0 or key_pub.strip() != cert_pub.strip():
            raise CertSyncError("%s: key does not match certificate" % name)
        code, _ = _openssl(["x509", "-in", str(crt_path), "-noout", "-checkend", "0"])
        if code != 0:
            raise CertSyncError("%s: certificate has expired" % name)
        code, out = _openssl(["x509", "-in", str(crt_path), "-noout",
                              "-checkhost", name])
        if code != 0 or b"does match" not in out:
            raise CertSyncError("%s: certificate does not cover the name" % name)
        verify = ["verify", "-purpose", "sslserver"]
        if ca_file:
            verify += ["-CAfile", str(ca_file)]
        if len(blocks) > 1:
            verify += ["-untrusted", str(chain_path)]
        code, _ = _openssl(verify + [str(leaf_path)])
        if code != 0:
            raise CertSyncError("%s: certificate does not chain to a trusted CA"
                                % name)
        code, out = _openssl(["x509", "-in", str(crt_path), "-noout", "-enddate"])
        if code != 0:
            raise CertSyncError("%s: certificate has no notAfter" % name)
        not_after = _pem_time(name, out, "notAfter")
        code, out = _openssl(["x509", "-in", str(crt_path), "-noout", "-startdate"])
        if code != 0:
            raise CertSyncError("%s: certificate has no notBefore" % name)
        not_before = _pem_time(name, out, "notBefore")
        if not_before > now + NOT_BEFORE_SKEW:
            raise CertSyncError("%s: certificate is not valid yet" % name)
        if not_after > now + float(max_days) * 86400:
            raise CertSyncError("%s: certificate expires more than %s days out"
                                % (name, max_days))
        return not_after


def make_validator(config):
    """openssl_validate with this node's trust settings."""
    ca_file = config.get("ca_file")
    max_days = config.get("max_days", MAX_DAYS)

    def validator(name, cert, key):
        return openssl_validate(name, cert, key, ca_file=ca_file,
                                max_days=max_days)
    return validator


def check_pem_shape(name, cert: bytes, key: bytes):
    if not cert.startswith(b"-----BEGIN CERTIFICATE-----"):
        raise CertSyncError("%s: certificate is not PEM" % name)
    if b"PRIVATE KEY-----" not in key.split(b"\n", 1)[0]:
        raise CertSyncError("%s: key is not a PEM private key" % name)
    if len(cert) > 64 * 1024 or len(key) > 16 * 1024:
        raise CertSyncError("%s: oversized pair" % name)


class Pair:
    """A validated certificate + key and where it came from."""

    __slots__ = ("name", "cert", "key", "not_after", "source")

    def __init__(self, name, cert, key, not_after, source):
        self.name, self.cert, self.key = name, cert, key
        self.not_after, self.source = int(not_after), source

    def __repr__(self):
        return "Pair(%s, notAfter=%d, from %s)" % (self.name, self.not_after,
                                                    self.source)


def checked_pair(name, cert, key, source, validator, now=None):
    """A Pair, or None when the bytes fail any check (logged)."""
    try:
        check_pem_shape(name, cert, key)
        not_after = validator(name, cert, key)
    except CertSyncError as error:
        print("certsync: ignoring %s pair from %s: %s" % (name, source, error))
        return None
    if not_after is None:
        not_after = 0
    now = time.time() if now is None else now
    if not_after and not_after <= now:
        print("certsync: ignoring %s pair from %s: expired" % (name, source))
        return None
    return Pair(name, cert, key, not_after, source)


def freshest(pairs):
    pairs = [p for p in pairs if p is not None]
    return max(pairs, key=lambda p: p.not_after) if pairs else None


def local_pairs(config, name, validator, now=None):
    """Valid pairs this node holds: Caddy's storage and tls_dir."""
    found = []
    storage = Path(config.get(
        "caddy_storage", "/var/lib/caddy/.local/share/caddy/certificates"))
    for crt, key in storage_pairs(storage, name):
        found.append(checked_pair(name, crt.read_bytes(), key.read_bytes(),
                                  "caddy:" + crt.parent.parent.name,
                                  validator, now))
    tls_dir = Path(config["tls_dir"])
    crt, key = tls_dir / (name + ".crt"), tls_dir / (name + ".key")
    if crt.is_file() and key.is_file():
        found.append(checked_pair(name, crt.read_bytes(), key.read_bytes(),
                                  "tls_dir", validator, now))
    return [p for p in found if p is not None]


def installed_pair(config, name, validator, now=None):
    tls_dir = Path(config["tls_dir"])
    crt, key = tls_dir / (name + ".crt"), tls_dir / (name + ".key")
    if crt.is_file() and key.is_file():
        return checked_pair(name, crt.read_bytes(), key.read_bytes(),
                            "tls_dir", validator, now)
    return None


# --------------------------------------------------------------------------
# export (every node exports what it holds)


def export_pair(config, pair) -> list:
    target = Path(config["export_dir"])
    group = config.get("export_group")
    gid = grp.getgrnam(group).gr_gid if group else -1
    changed = []
    for data, suffix in ((pair.cert, ".crt"), (pair.key, ".key")):
        if write_if_changed(target / (pair.name + suffix), data, 0o640,
                            0 if gid != -1 else -1, gid):
            changed.append(pair.name + suffix)
    return changed


def export(config, validator=None, now=None) -> list:
    """Export the freshest valid local pair of every name."""
    validator = validator or make_validator(config)
    changed = []
    for name in config["names"]:
        best = freshest(local_pairs(config, name, validator, now))
        if best is None:
            print("certsync: no valid local certificate for %s yet" % name)
            continue
        changed += export_pair(config, best)
    return changed


# --------------------------------------------------------------------------
# serve (every node): pair server on WireGuard + HTTP-01 forwarder on loopback

HOP_HEADER = "x-fmnext-acme-hop"
_TOKEN = re.compile(r"^[A-Za-z0-9_-]{1,256}$")
CHALLENGE_PREFIX = "/.well-known/acme-challenge/"


REPLICA_IP = re.compile(r"^10\.77\.0\.2[1-9]$")


def replica_peers(config):
    path = config.get("replica_peers_file")
    if not path:
        return set()
    try:
        data = json.loads(Path(path).read_text("utf-8"))
    except (OSError, ValueError):
        return set()
    return {ip for ip in (data.get("ips") or []) if REPLICA_IP.match(str(ip))}


def make_handler(config, secret, nonces, replica_secret=None):
    names = set(config["names"])
    allowed = set(config.get("allowed_peers") or [])
    export_dir = Path(config["export_dir"])
    node = config.get("node", "")

    class Handler(http.server.BaseHTTPRequestHandler):
        server_version = "fmnext-certsync"
        sys_version = ""

        def log_message(self, fmt, *args):  # one line, no secrets
            sys.stderr.write("certsync: %s %s\n" % (
                self.client_address[0], fmt % args))

        def _reply(self, status, body=b"", nonce=None):
            self.send_response(status)
            self.send_header("content-type", "application/json")
            self.send_header("cache-control", "no-store")
            self.send_header("content-length", str(len(body)))
            if nonce:
                self.send_header(BODY_MAC_HEADER, body_mac(
                    getattr(self, "_key", secret), nonce, body))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            ip = self.client_address[0]
            self._key = secret
            if replica_secret and REPLICA_IP.match(ip) and ip in replica_peers(config):
                self._key = replica_secret
            elif allowed and ip not in allowed:
                return self._reply(403)
            try:
                nonce = verify_request(self._key, self.path, {
                    AUTH_HEADER: self.headers.get(AUTH_HEADER),
                    NODE_HEADER: self.headers.get(NODE_HEADER)}, nonces)
            except CertSyncError:
                return self._reply(401)
            if self.path == "/v1/health":
                body = json.dumps({"ok": True, "node": node}).encode()
                return self._reply(200, body, nonce)
            match = re.fullmatch(r"/v1/tls/([a-z0-9.-]+)", self.path)
            if not match or match.group(1) not in names:
                return self._reply(404, nonce=nonce)
            name = match.group(1)
            try:
                cert = (export_dir / (name + ".crt")).read_text("ascii")
                key = (export_dir / (name + ".key")).read_text("ascii")
            except FileNotFoundError:
                return self._reply(503, b'{"error":"no certificate here"}', nonce)
            body = json.dumps({"name": name, "cert": cert, "key": key}).encode()
            return self._reply(200, body, nonce)

    return Handler


def challenge_order(config, host):
    """Peers to ask for a challenge token, rendezvous order for ``host``
    (the node most likely to be issuing first); never this node."""
    peers = {n: a for n, a in (config.get("challenge_peers") or {}).items()
             if n != config.get("node")}
    return [peers[n] for n in rendezvous_order(host, peers)]


def forward_challenge(config, host, path, opener=None, timeout=3.0):
    """(status, body) for an HTTP-01 request: the first peer that has it."""
    if host not in set(config["names"]) or not path.startswith(CHALLENGE_PREFIX) \
            or not _TOKEN.match(path[len(CHALLENGE_PREFIX):]):
        return 404, b""
    opener = opener or urllib.request.urlopen
    for address in challenge_order(config, host):
        request = urllib.request.Request(
            "http://%s%s" % (address, path),
            headers={"Host": host, HOP_HEADER: config.get("node", "?")})
        try:
            with opener(request, timeout=timeout) as response:
                if response.status == 200:
                    return 200, response.read(4096)
        except (urllib.error.URLError, OSError, ValueError):
            continue
    return 404, b""


def make_challenge_handler(config, opener=None):
    class Handler(http.server.BaseHTTPRequestHandler):
        server_version = "fmnext-acme-forwarder"
        sys_version = ""

        def log_message(self, fmt, *args):
            sys.stderr.write("certsync-acme: %s\n" % (fmt % args))

        def do_GET(self):
            host = (self.headers.get("Host") or "").split(":")[0].lower()
            if self.headers.get(HOP_HEADER):
                status, body = 404, b""   # already forwarded once: no loops
            else:
                status, body = forward_challenge(config, host, self.path,
                                                 opener)
            self.send_response(status)
            self.send_header("content-type", "text/plain")
            self.send_header("cache-control", "no-store")
            self.send_header("content-length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    return Handler


def serve(config, secret, replica_secret=None):
    host, _, port = config["listen"].rpartition(":")
    server = http.server.ThreadingHTTPServer(
        (host, int(port)), make_handler(config, secret, NonceCache(),
                                        replica_secret))
    if config.get("challenge_listen"):
        chost, _, cport = config["challenge_listen"].rpartition(":")
        forwarder = http.server.ThreadingHTTPServer(
            (chost, int(cport)), make_challenge_handler(config))
        threading.Thread(target=forwarder.serve_forever, daemon=True).start()
    print("certsync: serving %s on %s" % (", ".join(config["names"]),
                                         config["listen"]), flush=True)
    server.serve_forever()


# --------------------------------------------------------------------------
# peers


def rendezvous_order(key, nodes):
    """Nodes best-first by sha256("<key>:<node>") (same on every node)."""
    return sorted(nodes, key=lambda node: hashlib.sha256(
        ("%s:%s" % (key, node)).encode()).digest(), reverse=True)


def _signed_get(config, secret, url, path, opener=None, now=None, timeout=10):
    headers, nonce = auth_headers(secret, path, config["node"], now=now)
    request = urllib.request.Request(url.rstrip("/") + path, headers=headers)
    opener = opener or urllib.request.urlopen
    with opener(request, timeout=timeout) as response:
        body = response.read(256 * 1024)
        mac = response.headers.get(BODY_MAC_HEADER) or ""
    if not hmac.compare_digest(body_mac(secret, nonce, body), mac):
        raise CertSyncError("%s: response MAC mismatch" % url)
    return json.loads(body)


def live_peers(config, secret, opener=None):
    """Peer names whose certsync server answers an authenticated health
    check, plus this node (always live to itself)."""
    live = [config["node"]]
    for name, url in sorted((config.get("peers") or {}).items()):
        if name == config["node"]:
            continue
        try:
            answer = _signed_get(config, secret, url, "/v1/health", opener,
                                 timeout=5)
            if answer.get("ok"):
                live.append(name)
        except (CertSyncError, urllib.error.URLError, OSError, ValueError):
            continue
    return sorted(live)


def fetch_pair(config, secret, name, opener=None, now=None, url=None):
    url = url or config["issuer_url"]
    payload = _signed_get(config, secret, url, "/v1/tls/%s" % name, opener, now)
    if payload.get("name") != name:
        raise CertSyncError("%s: peer answered for another name" % name)
    return payload["cert"].encode("ascii"), payload["key"].encode("ascii")


def peer_pairs(config, secret, name, peers, validator, opener=None, now=None):
    found = []
    urls = config.get("peers") or {}
    for peer in peers:
        if peer == config["node"] or peer not in urls:
            continue
        try:
            cert, key = fetch_pair(config, secret, name, opener, url=urls[peer])
        except (CertSyncError, urllib.error.URLError, OSError, ValueError,
                KeyError) as error:
            if not (isinstance(error, urllib.error.HTTPError) and error.code == 503):
                print("certsync: %s from %s: %s" % (name, peer, error))
            continue
        found.append(checked_pair(name, cert, key, "peer:" + peer, validator, now))
    return [p for p in found if p is not None]


# --------------------------------------------------------------------------
# modes and the Caddy include files


def issuer_for(name, live):
    order = rendezvous_order(name, live)
    return order[0] if order else None


def choose_mode(config, name, live, best, installed, now=None):
    """``files`` | ``acme`` | ``internal`` for one name on this node."""
    now = time.time() if now is None else now
    renew = float(config.get("renew_days", 30)) * 86400
    issuer = issuer_for(name, live)
    due = best is None or best.not_after - now < renew
    if issuer == config["node"] and due and config.get("issue", True):
        return "acme"
    if installed is not None:
        return "files"
    return "internal"


def tls_include(config, name, mode):
    tls_dir = config["tls_dir"]
    head = "# Managed by fmnext-certsync (%s): %s\n" % (config["node"], mode)
    if mode == "files":
        return head + "tls %s/%s.crt %s/%s.key\n" % (tls_dir, name, tls_dir, name)
    if mode == "acme":
        return head + "tls {\n\tissuer acme\n}\n"
    return head + "tls internal\n"


def http_include(config, modes):
    """Top-level site for port 80 of the shared names. Names Caddy is
    solving itself (acme) are left to Caddy's automatic HTTP handling; the
    rest forward challenges to the local forwarder."""
    forward = [n for n in config["names"] if modes.get(n) != "acme"]
    lines = ["# Managed by fmnext-certsync (%s)." % config["node"]]
    if forward:
        lines += [
            ", ".join("http://%s" % n for n in forward) + " {",
            "\theader X-Content-Type-Options nosniff",
            "\thandle /.well-known/acme-challenge/* {",
            "\t\treverse_proxy %s" % config.get("challenge_listen",
                                               "127.0.0.1:8794"),
            "\t}",
            "\thandle {",
            "\t\tredir https://{host}{uri} 308",
            "\t}",
            "}",
        ]
    return "\n".join(lines) + "\n"


def include_path(config, name):
    return Path(config["include_dir"]) / ("tls-%s.caddy" % name)


def write_includes(config, modes) -> list:
    changed = []
    for name, mode in modes.items():
        if write_if_changed(include_path(config, name),
                            tls_include(config, name, mode).encode(), 0o644):
            changed.append(include_path(config, name).name)
    http_path = Path(config["include_dir"]) / "acme-http.caddy"
    if write_if_changed(http_path, http_include(config, modes).encode(), 0o644):
        changed.append(http_path.name)
    return changed


def install_pair(config, pair, owner=None) -> list:
    tls_dir = Path(config["tls_dir"])
    owner = owner if owner is not None else config.get("tls_owner", "caddy")
    uid = gid = -1
    if owner:
        entry = pwd.getpwnam(owner)
        uid, gid = entry.pw_uid, entry.pw_gid
    changed = []
    for data, suffix in ((pair.cert, ".crt"), (pair.key, ".key")):
        if write_if_changed(tls_dir / (pair.name + suffix), data, 0o600, uid, gid):
            changed.append(pair.name + suffix)
    return changed


def _reload(config, reload):
    command = reload if reload is not None else config.get(
        "reload_command", ["systemctl", "reload", "caddy"])
    if command:
        subprocess.run(command, check=True)


def sync(config, secret, opener=None, validator=None, reload=None, owner=None,
         now=None, offline=False) -> dict:
    """One leaderless round (``offline``: the ``prepare`` variant)."""
    validator = validator or make_validator(config)
    now = time.time() if now is None else now
    if offline:
        live = sorted({config["node"], *(config.get("peers") or {})})
    else:
        live = live_peers(config, secret, opener)
    report = {"live": live, "modes": {}, "changed": [], "issuer": {}}
    for name in config["names"]:
        local = local_pairs(config, name, validator, now)
        remote = [] if offline else peer_pairs(config, secret, name, live,
                                               validator, opener, now)
        best = freshest(local + remote)
        installed = installed_pair(config, name, validator, now)
        if best is not None and (installed is None
                                 or best.not_after > installed.not_after):
            report["changed"] += install_pair(config, best, owner)
            installed = best
        local_best = freshest(local + ([installed] if installed else []))
        if local_best is not None and config.get("export_dir"):
            report["exported"] = report.get("exported", []) + export_pair(
                config, local_best)
        mode = choose_mode(config, name, live, best, installed, now)
        report["modes"][name] = mode
        report["issuer"][name] = issuer_for(name, live)
    report["changed"] += write_includes(config, report["modes"])
    if report["changed"] and not offline:
        _reload(config, reload)
    return report


def pull(config, secret, opener=None, validator=None, reload=None,
         owner=None) -> list:
    """Compatibility wrapper: one sync round; the files that changed."""
    return sync(config, secret, opener, validator, reload, owner)["changed"]


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("command", choices=("export", "serve", "sync", "pull",
                                            "prepare"))
    parser.add_argument("--config", required=True)
    args = parser.parse_args(argv)
    try:
        config = load_config(args.config)
        if args.command == "export":
            changed = export(config)
            print("certsync: exported %s" % (", ".join(changed) or "nothing new"))
        elif args.command == "serve":
            serve(config, read_secret(config), read_replica_secret(config))
        elif args.command == "prepare":
            report = sync(config, b"", offline=True)
            print("certsync: prepared %s" % json.dumps(report["modes"]))
        else:
            report = sync(config, read_secret(config))
            print("certsync: live %s, issuers %s, modes %s, %s" % (
                ",".join(report["live"]), json.dumps(report["issuer"]),
                json.dumps(report["modes"]),
                "updated " + ", ".join(report["changed"])
                if report["changed"] else "nothing changed"))
    except (CertSyncError, OSError, ValueError, urllib.error.URLError) as error:
        print("certsync: %s" % error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
