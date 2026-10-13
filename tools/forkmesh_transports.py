#!/usr/bin/env python3
"""Leak-safe transport adapters and anonymity postures (issue #505).

Implements forkmesh-transport-profile-v1 (www/docs/transports.md). A profile
names the transport adapters a device has (direct clearnet, a SOCKS proxy onto
Tor, I2P, or another privacy network, an overlay network such as a WireGuard
mesh, and the local network) and one posture that decides which of them may
carry a repository operation and what may be disclosed on the way.

  forkmesh_transports.py validate PROFILE [--json]
  forkmesh_transports.py plan --profile PROFILE URL [--json]
  forkmesh_transports.py probe --profile PROFILE URL [--attempts N] [--timeout S] \\
      [--report FILE] [--telemetry FILE] [--json]
  forkmesh_transports.py git --profile PROFILE URL [--repo DIR] [--trace auto|required|off] \\
      [--report FILE] [--telemetry FILE] [--json] -- GIT_ARGS...
  forkmesh_transports.py status REPORT [--json]
  forkmesh_transports.py advertise --profile PROFILE --address IP... [--json]

GIT_ARGS name the remote as the literal token @URL, for example
`-- clone @URL checkout` or `-- -C repo fetch --atomic @URL main`.

`relay HOST PORT` is internal: Git and OpenSSH run it as their proxy command so
git:// and ssh:// traffic follows the same route rules as HTTP.

Exit codes: 0 done; 2 usage, invalid profile, or leak tracing required but
unavailable; 3 refused by policy before anything was sent; 4 no permitted route
was reachable; 5 exposed, a prohibited DNS query or connection was observed;
6 the Git command itself failed on a reachable route.
"""

from __future__ import annotations

import argparse
import hashlib
import ipaddress
import json
import os
import re
import secrets
import shlex
import shutil
import socket
import statistics
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
from pathlib import Path


PROFILE_SCHEMA = "forkmesh-transport-profile-v1"
PLAN_SCHEMA = "forkmesh-transport-plan-v1"
HEALTH_SCHEMA = "forkmesh-transport-health-v1"
ADVERTISEMENT_SCHEMA = "forkmesh-transport-advertisement-v1"
TELEMETRY_SCHEMA = "forkmesh-transport-telemetry-v1"

ADAPTER_KINDS = ("clearnet", "proxy", "overlay", "lan")
PROXY_NETWORKS = ("tor", "i2p", "socks")
POSTURES = ("standard", "private", "anonymous", "overlay-only", "lan-only")
SCHEMES = ("https", "http", "ssh", "git")

# What each posture permits and what it promises. `dns` is where names are
# resolved: `system` is the operating system resolver, `remote` is the proxy,
# `none` means only literal or pinned addresses are dialled.
POSTURE_RULES = {
    "standard": {
        "adapters": ("clearnet", "proxy", "overlay", "lan"), "dns": "system",
        "directFallback": True, "telemetry": "counts", "logs": "full",
        "advertise": "reachable", "streamIsolation": False,
        "proxyNetworks": PROXY_NETWORKS,
    },
    "private": {
        "adapters": ("proxy",), "dns": "remote", "directFallback": False,
        "telemetry": "counts", "logs": "redacted", "advertise": "none",
        "streamIsolation": False, "proxyNetworks": PROXY_NETWORKS,
    },
    "anonymous": {
        "adapters": ("proxy",), "dns": "remote", "directFallback": False,
        "telemetry": "off", "logs": "redacted", "advertise": "none",
        "streamIsolation": True, "proxyNetworks": ("tor", "i2p"),
    },
    "overlay-only": {
        "adapters": ("overlay",), "dns": "none", "directFallback": False,
        "telemetry": "counts", "logs": "redacted", "advertise": "overlay",
        "streamIsolation": False, "proxyNetworks": (),
    },
    "lan-only": {
        "adapters": ("lan",), "dns": "none", "directFallback": False,
        "telemetry": "counts", "logs": "redacted", "advertise": "lan",
        "streamIsolation": False, "proxyNetworks": (),
    },
}

# Profile validation codes, in the order they are checked; the first wins.
PROFILE_ERRORS = (
    "profile_schema", "unknown_posture", "adapters_missing", "adapter_id_invalid",
    "adapter_id_duplicate", "unknown_adapter_kind", "proxy_endpoint_not_literal",
    "proxy_network_unknown", "overlay_cidr_invalid", "overlay_cidr_global",
    "pinned_host_invalid", "budget_invalid", "posture_has_no_adapter",
    "anonymous_requires_anonymity_network",
)

# Per-adapter route refusals, in the order they are checked.
ROUTE_REFUSALS = (
    "adapter_not_permitted", "proxy_network_not_permitted",
    "destination_requires_anonymity_network", "onion_requires_tor",
    "i2p_requires_i2p", "local_destination_via_proxy", "destination_outside_overlay",
    "destination_outside_lan", "name_needs_dns", "plaintext_via_exit",
    "peer_unauthenticated",
)

# Plan-level refusals, reported when no adapter could carry the URL.
PLAN_REFUSALS = ("invalid_url", "scheme_unsupported", "no_permitted_route",
                 "repository_config_overrides_route")

ROUTE_STATES = ("reachable", "degraded", "unreachable")
STATES = ("exposed", "unreachable", "degraded", "anonymous", "protected", "direct")
STATE_LABELS = {
    "exposed": "Exposed",
    "unreachable": "Unreachable",
    "degraded": "Degraded",
    "anonymous": "Anonymous",
    "protected": "Protected",
    "direct": "Direct",
}
STATE_TONES = {
    "exposed": "danger", "unreachable": "danger", "degraded": "warning",
    "anonymous": "success", "protected": "success", "direct": "neutral",
}
STATE_EXPLANATIONS = {
    "exposed": "A DNS query or connection left this device outside the chosen "
               "route; your address or destination may have been disclosed.",
    "unreachable": "No permitted route answered. Nothing was sent another way.",
    "degraded": "Reachable, but slow, failing intermittently, or on a fallback route.",
    "anonymous": "Carried by an anonymity network with remote name resolution; "
                 "no leak was observed.",
    "protected": "Held to the chosen private, overlay, or local route; no leak was observed.",
    "direct": "Reachable directly. Peers and networks on the path can see your address.",
}
DEGRADED_REASONS = ("failed_over", "slow", "intermittent", "unauthenticated_peer")
INCIDENT_KINDS = ("dns_query", "direct_connection")
EXIT_OK, EXIT_USAGE, EXIT_REFUSED, EXIT_UNREACHABLE, EXIT_EXPOSED, EXIT_GIT = 0, 2, 3, 4, 5, 6

DEFAULT_LATENCY_BUDGET_MS = 2000
ADAPTER_ID = re.compile(r"^[a-z0-9][a-z0-9-]{0,39}$")
ONION_V3 = re.compile(r"^(?:[a-z0-9-]+\.)*[a-z2-7]{56}\.onion$")
I2P_NAME = re.compile(r"^(?:[a-z0-9-]+\.)+i2p$")
HOST_LABEL = re.compile(r"^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$")
SCP_LIKE = re.compile(r"^(?:(?P<user>[^@/]+)@)?(?P<host>\[[^\]]+\]|[^:/]+):(?P<path>.*)$")
DEFAULT_PORTS = {"https": 443, "http": 80, "ssh": 22, "git": 9418}

# The local-network rule shared with desktop/src/LocalMeshNode.cpp: loopback,
# RFC 1918, IPv4 link-local, IPv6 link-local and unique-local. Carrier-grade
# NAT space belongs to an ISP (or an overlay), never to a LAN.
LOCAL_NETWORKS = tuple(ipaddress.ip_network(n) for n in (
    "127.0.0.0/8", "10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16",
    "169.254.0.0/16", "::1/128", "fe80::/10", "fc00::/7"))
CGNAT = ipaddress.ip_network("100.64.0.0/10")
# Overlay ranges that Python calls global but are not reachable from clearnet.
KNOWN_OVERLAY_GLOBAL = (ipaddress.ip_network("200::/7"),)
MDNS_ENDPOINTS = {("224.0.0.251", 5353), ("ff02::fb", 5353)}
RESOLVER_SOCKETS = ("nscd", "resolve", "avahi", "dbus", "mdns")

# Environment kept for Git in every posture except standard. Anything that can
# redirect traffic (proxy variables, GIT_SSH*, GIT_PROXY_COMMAND) or print a
# destination (GIT_TRACE*, GIT_CURL_VERBOSE) is dropped.
ENV_KEEP = ("PATH", "HOME", "LANG", "LC_ALL", "LC_CTYPE", "TMPDIR", "TEMP", "TMP",
            "SYSTEMROOT", "COMSPEC", "USERPROFILE", "APPDATA", "LOCALAPPDATA",
            "GIT_AUTHOR_NAME", "GIT_AUTHOR_EMAIL", "GIT_COMMITTER_NAME",
            "GIT_COMMITTER_EMAIL", "GIT_EXEC_PATH", "SSH_AUTH_SOCK")
RISKY_CONFIG = re.compile(
    r"^(url\..*\.(insteadof|pushinsteadof)|http\..*proxy.*|https?\.proxy|"
    r"core\.gitproxy|core\.sshcommand|include\..*|includeif\..*|remote\..*\.proxy)$")
CONNECTION_FAILURE = re.compile(
    r"(Failed to connect|Could not connect|Connection refused|Couldn't connect|"
    r"unable to connect|Connection timed out|Connection reset|proxy|SOCKS|"
    r"Recv failure|early EOF|Could not resolve|the remote end hung up|RPC failed|"
    r"transfer closed|outstanding read data|unexpected disconnect|bad pack header|"
    r"HTTP2 framing layer|HTTP/2 stream|"
    r"index-pack failed|expected 'packfile'|expected flush after|timed out|Operation too slow)",
    re.I)


class UsageError(Exception):
    pass


class RouteRefused(Exception):
    def __init__(self, reason, detail=""):
        super().__init__(detail or reason)
        self.reason = reason


class LeakBlocked(OSError):
    """Raised inside a leak guard in place of a prohibited lookup or connection."""


class SocksError(OSError):
    REPLIES = {1: "proxy_general_failure", 2: "proxy_ruleset_refused",
               3: "network_unreachable", 4: "host_unreachable",
               5: "connection_refused", 6: "ttl_expired",
               7: "command_unsupported", 8: "address_type_unsupported"}

    def __init__(self, reason):
        super().__init__(reason)
        self.reason = reason


# --- Profiles ---------------------------------------------------------------

def _literal_ip(text):
    try:
        return ipaddress.ip_address(str(text).strip("[]").split("%", 1)[0])
    except ValueError:
        return None


def _parse_endpoint(text):
    match = re.match(r"^\[?([^\]]+?)\]?:(\d{1,5})$", str(text or ""))
    if not match or not 0 < int(match.group(2)) < 65536:
        return None
    host = match.group(1)
    if _literal_ip(host) is None:
        return None
    return host, int(match.group(2))


def validate_profile(profile):
    """Return a normalized profile or raise UsageError naming the first failing check."""
    def fail(code, detail):
        raise UsageError("%s: %s" % (code, detail))

    if not isinstance(profile, dict) or profile.get("schema") != PROFILE_SCHEMA:
        fail("profile_schema", "expected schema %s" % PROFILE_SCHEMA)
    posture = profile.get("posture")
    if posture not in POSTURES:
        fail("unknown_posture", "posture must be one of %s" % ", ".join(POSTURES))
    adapters = profile.get("adapters")
    if not isinstance(adapters, list) or not adapters:
        fail("adapters_missing", "a profile lists at least one adapter")
    seen = set()
    normalized = []
    for adapter in adapters:
        if not isinstance(adapter, dict) or not ADAPTER_ID.match(str(adapter.get("id", ""))):
            fail("adapter_id_invalid", "adapter ids are lowercase letters, digits, and hyphens")
        if adapter["id"] in seen:
            fail("adapter_id_duplicate", adapter["id"])
        seen.add(adapter["id"])
        kind = adapter.get("kind")
        if kind not in ADAPTER_KINDS:
            fail("unknown_adapter_kind", adapter["id"])
        entry = {"id": adapter["id"], "kind": kind,
                 "allowPlaintext": bool(adapter.get("allowPlaintext", False))}
        if kind == "proxy":
            endpoint = _parse_endpoint(adapter.get("socks"))
            if endpoint is None:
                fail("proxy_endpoint_not_literal",
                     "%s: socks must be a literal IP:port so reaching the proxy needs no DNS"
                     % adapter["id"])
            network = adapter.get("network", "socks")
            if network not in PROXY_NETWORKS:
                fail("proxy_network_unknown", adapter["id"])
            entry.update(socks=endpoint, network=network,
                         isolateStreams=bool(adapter.get("isolateStreams", network == "tor")),
                         serviceAddress=str(adapter.get("serviceAddress", "")))
        if kind == "overlay":
            networks = []
            for cidr in adapter.get("cidrs", ["100.64.0.0/10", "fd7a:115c:a1e0::/48"]):
                try:
                    network = ipaddress.ip_network(cidr, strict=False)
                except ValueError:
                    fail("overlay_cidr_invalid", "%s: %s" % (adapter["id"], cidr))
                if network.is_global and not any(
                        network.version == known.version and network.subnet_of(known)
                        for known in KNOWN_OVERLAY_GLOBAL):
                    fail("overlay_cidr_global",
                         "%s: %s is routable on clearnet, so it cannot mark an overlay"
                         % (adapter["id"], cidr))
                networks.append(network)
            entry.update(cidrs=networks,
                         authenticatedNetwork=bool(adapter.get("authenticatedNetwork", False)))
        if kind in ("overlay", "lan"):
            hosts = {}
            for name, address in (adapter.get("hosts") or {}).items():
                if not _valid_hostname(name) or _literal_ip(address) is None:
                    fail("pinned_host_invalid", "%s: %s" % (adapter["id"], name))
                hosts[name.lower()] = str(_literal_ip(address))
            entry["hosts"] = hosts
        normalized.append(entry)
    budgets = profile.get("budgets") or {}
    latency = budgets.get("latencyMs", DEFAULT_LATENCY_BUDGET_MS)
    if not isinstance(latency, int) or isinstance(latency, bool) or latency <= 0:
        fail("budget_invalid", "budgets.latencyMs must be a positive integer")
    rules = POSTURE_RULES[posture]
    permitted = [a for a in normalized if a["kind"] in rules["adapters"]
                 and (a["kind"] != "proxy" or a["network"] in rules["proxyNetworks"])]
    if not [a for a in normalized if a["kind"] in rules["adapters"]]:
        fail("posture_has_no_adapter",
             "posture %s needs a %s adapter" % (posture, " or ".join(rules["adapters"])))
    if posture == "anonymous" and not permitted:
        fail("anonymous_requires_anonymity_network",
             "the anonymous posture needs a tor or i2p proxy adapter")
    return {"schema": PROFILE_SCHEMA, "posture": posture, "adapters": normalized,
            "budgets": {"latencyMs": latency}}


def load_profile(path):
    try:
        raw = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise UsageError("cannot read profile %s: %s" % (path, exc)) from exc
    return validate_profile(raw)


# --- Destinations -------------------------------------------------------------

def _valid_hostname(name):
    name = str(name).lower().rstrip(".")
    return bool(name) and len(name) <= 253 and all(
        HOST_LABEL.match(label) for label in name.split("."))


def classify_host(host):
    """Name a destination host's class without resolving it."""
    host = host.lower().rstrip(".")
    address = _literal_ip(host)
    if address is not None:
        if getattr(address, "ipv4_mapped", None):
            address = address.ipv4_mapped
        if any(address.version == n.version and address in n for n in LOCAL_NETWORKS):
            return "local-address"
        if address.version == 4 and address in CGNAT:
            return "shared-address"
        return "public-address" if address.is_global else "reserved-address"
    if ONION_V3.match(host):
        return "onion"
    if I2P_NAME.match(host):
        return "i2p"
    if host == "localhost" or host.endswith(".localhost"):
        return "loopback-name"
    if host.endswith(".local"):
        return "mdns-name"
    return "dns-name"


def parse_destination(url):
    text = str(url or "")
    parsed = urllib.parse.urlsplit(text)
    if parsed.scheme:
        scheme = parsed.scheme.lower()
        if scheme not in SCHEMES:
            raise RouteRefused("scheme_unsupported", scheme)
        try:
            host, port = parsed.hostname, parsed.port
        except ValueError as exc:
            raise RouteRefused("invalid_url", str(exc)) from exc
    else:
        match = SCP_LIKE.match(text)
        if not match or "/" in match.group("host") or len(match.group("host")) < 2:
            raise RouteRefused("invalid_url", "not a URL")
        scheme, host, port = "ssh", match.group("host").strip("[]"), None
    if not host or not (_valid_hostname(host) or _literal_ip(host) is not None):
        raise RouteRefused("invalid_url", "missing or malformed host")
    host = host.lower()
    return {"scheme": scheme, "host": host, "port": port or DEFAULT_PORTS[scheme],
            "hostClass": classify_host(host)}


# --- Planning -----------------------------------------------------------------

def _route_for(adapter, destination, posture):
    rules = POSTURE_RULES[posture]
    kind, host_class, scheme = adapter["kind"], destination["hostClass"], destination["scheme"]
    if kind not in rules["adapters"]:
        raise RouteRefused("adapter_not_permitted")
    if kind == "proxy" and adapter["network"] not in rules["proxyNetworks"]:
        raise RouteRefused("proxy_network_not_permitted")
    if kind != "proxy" and host_class in ("onion", "i2p"):
        raise RouteRefused("destination_requires_anonymity_network")
    dial_address, dns = None, "none"
    if kind == "proxy":
        if host_class == "onion" and adapter["network"] != "tor":
            raise RouteRefused("onion_requires_tor")
        if host_class == "i2p" and adapter["network"] != "i2p":
            raise RouteRefused("i2p_requires_i2p")
        if host_class in ("local-address", "loopback-name", "mdns-name", "shared-address"):
            raise RouteRefused("local_destination_via_proxy")
        dns = "remote"
    elif kind == "overlay":
        pinned = adapter["hosts"].get(destination["host"])
        candidate = _literal_ip(pinned or destination["host"])
        if candidate is None or not any(
                candidate.version == n.version and candidate in n for n in adapter["cidrs"]):
            raise RouteRefused("destination_outside_overlay")
        dial_address, dns = str(candidate), ("pinned" if pinned else "none")
    elif kind == "lan":
        pinned = adapter["hosts"].get(destination["host"])
        if pinned:
            if classify_host(pinned) != "local-address":
                raise RouteRefused("destination_outside_lan")
            dial_address, dns = pinned, "pinned"
        elif host_class == "local-address":
            dial_address = destination["host"]
        elif host_class == "mdns-name":
            dns = "mdns"
        else:
            raise RouteRefused("destination_outside_lan")
    else:  # clearnet
        if host_class in ("dns-name", "mdns-name", "loopback-name"):
            if rules["dns"] != "system":
                raise RouteRefused("name_needs_dns")
            dns = "system"
        else:
            dial_address = destination["host"]
    if scheme == "https":
        authentication = "tls"
    elif scheme == "ssh":
        authentication = "ssh"
    elif host_class in ("onion", "i2p"):
        authentication = host_class
    elif kind == "overlay" and adapter["authenticatedNetwork"]:
        authentication = "network"
    elif kind == "proxy" and posture == "anonymous":
        raise RouteRefused("plaintext_via_exit")
    elif adapter["allowPlaintext"]:
        authentication = "none"
    else:
        raise RouteRefused("peer_unauthenticated")
    route = {"adapter": adapter["id"], "kind": kind, "dns": dns,
             "peerAuthentication": authentication}
    if kind == "proxy":
        route.update(network=adapter["network"],
                     streamIsolation=adapter["isolateStreams"] or rules["streamIsolation"])
    if dial_address:
        route["dialAddress"] = dial_address
    return route


def plan(profile, url):
    """Order the routes a posture permits for URL; refusals say why each adapter was skipped."""
    posture = profile["posture"]
    rules = POSTURE_RULES[posture]
    result = {"schema": PLAN_SCHEMA, "posture": posture,
              "policy": {key: (list(value) if isinstance(value, tuple) else value)
                         for key, value in rules.items()},
              "routes": [], "refused": [], "ok": False, "reason": None}
    try:
        destination = parse_destination(url)
    except RouteRefused as exc:
        result["reason"] = exc.reason
        return result
    result["destination"] = {k: destination[k] for k in ("scheme", "hostClass", "port")}
    adapters = profile["adapters"]
    if rules["directFallback"]:
        # Standard posture: prefer whatever keeps traffic most local, then clearnet.
        order = {"lan": 0, "overlay": 1, "clearnet": 2, "proxy": 3}
        adapters = sorted(adapters, key=lambda a: order[a["kind"]])
    for adapter in adapters:
        try:
            route = _route_for(adapter, destination, posture)
        except RouteRefused as exc:
            result["refused"].append({"adapter": adapter["id"], "reason": exc.reason})
            continue
        route["_adapter"] = adapter
        result["routes"].append(route)
    result["ok"] = bool(result["routes"])
    if not result["ok"]:
        result["reason"] = "no_permitted_route"
    result["_destination"] = destination
    return result


def public_plan(result):
    """A plan without the internal fields (adapter objects, the destination host)."""
    clean = {k: v for k, v in result.items() if not k.startswith("_")}
    clean["routes"] = [{k: v for k, v in route.items() if not k.startswith("_")}
                       for route in result["routes"]]
    return clean


# --- Leak guard (in-process) ---------------------------------------------------

_GUARD = threading.local()
_HOOK_LOCK = threading.Lock()
_HOOK_INSTALLED = False


def _audit(event, args):
    guard = getattr(_GUARD, "active", None)
    if guard is None or not event.startswith("socket."):
        return
    guard.inspect(event, args)


class LeakGuard:
    """Blocks name lookups and connections a route does not allow, on this thread.

    Python audit hooks cannot be removed, so one hook is installed for the life
    of the process and consults the guard active on the calling thread.
    """

    def __init__(self, allowed_endpoints=(), allow_mdns=False, allowed_names=()):
        self.allowed = {(str(_literal_ip(h) or h), int(p)) for h, p in allowed_endpoints}
        self.allow_mdns = allow_mdns
        self.allowed_names = {n.lower() for n in allowed_names}
        self.blocked = []

    def __enter__(self):
        global _HOOK_INSTALLED
        with _HOOK_LOCK:
            if not _HOOK_INSTALLED:
                sys.addaudithook(_audit)
                _HOOK_INSTALLED = True
        self._previous = getattr(_GUARD, "active", None)
        _GUARD.active = self
        return self

    def __exit__(self, *exc):
        _GUARD.active = self._previous
        return False

    def _block(self, kind):
        self.blocked.append(kind)
        raise LeakBlocked("blocked by transport leak guard: %s" % kind)

    def inspect(self, event, args):
        if event in ("socket.getaddrinfo", "socket.gethostbyname", "socket.gethostbyname_ex"):
            host = args[0]
            if isinstance(host, bytes):
                host = host.decode("ascii", "replace")
            if host is None or _literal_ip(host) is not None:
                return
            if host.lower() in self.allowed_names:
                return
            if self.allow_mdns and host.lower().rstrip(".").endswith(".local"):
                return
            self._block("dns_query")
        elif event in ("socket.gethostbyaddr", "socket.getnameinfo"):
            self._block("dns_query")
        elif event in ("socket.connect", "socket.sendto"):
            address = args[1] if event == "socket.connect" else args[-1]
            if not isinstance(address, tuple) or len(address) < 2:
                return  # AF_UNIX and friends do not leave the device
            endpoint = (str(_literal_ip(address[0]) or address[0]), int(address[1]))
            if endpoint in self.allowed or (self.allow_mdns and endpoint in MDNS_ENDPOINTS):
                return
            if self.allow_mdns and classify_host(endpoint[0]) == "local-address":
                return  # a .local name resolved on the LAN; the address is still local
            self._block("dns_query" if endpoint[1] == 53 else "direct_connection")


# --- Dialling -----------------------------------------------------------------

def _recv_exact(sock, size):
    data = b""
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise SocksError("proxy_closed")
        data += chunk
    return data


def socks5_connect(proxy, host, port, username=None, password=None, timeout=30.0):
    """RFC 1928 CONNECT that never resolves HOST locally: names go to the proxy."""
    sock = socket.create_connection(proxy, timeout=timeout)
    try:
        methods = b"\x02\x00\x02" if username is not None else b"\x01\x00"
        sock.sendall(b"\x05" + methods)
        version, method = _recv_exact(sock, 2)
        if version != 5 or method == 0xFF:
            raise SocksError("proxy_auth_refused")
        if method == 2:
            user, secret = (username or "").encode(), (password or "").encode()
            sock.sendall(b"\x01" + bytes([len(user)]) + user + bytes([len(secret)]) + secret)
            if _recv_exact(sock, 2)[1] != 0:
                raise SocksError("proxy_auth_refused")
        elif method != 0:
            raise SocksError("proxy_auth_refused")
        address = _literal_ip(host)
        if address is None:
            encoded = host.encode("ascii")
            target = b"\x03" + bytes([len(encoded)]) + encoded
        elif address.version == 4:
            target = b"\x01" + address.packed
        else:
            target = b"\x04" + address.packed
        sock.sendall(b"\x05\x01\x00" + target + int(port).to_bytes(2, "big"))
        reply = _recv_exact(sock, 4)
        if reply[1] != 0:
            raise SocksError(SocksError.REPLIES.get(reply[1], "proxy_general_failure"))
        if reply[3] == 1:
            _recv_exact(sock, 4 + 2)
        elif reply[3] == 4:
            _recv_exact(sock, 16 + 2)
        elif reply[3] == 3:
            _recv_exact(sock, _recv_exact(sock, 1)[0] + 2)
        else:
            raise SocksError("address_type_unsupported")
        sock.settimeout(None)
        return sock
    except BaseException:
        sock.close()
        raise


def isolation_credentials(route):
    """Fresh SOCKS credentials so Tor (IsolateSOCKSAuth) puts this run on its own circuit."""
    if not route.get("streamIsolation"):
        return None, None
    return "forkmesh-" + secrets.token_hex(8), secrets.token_hex(8)


def guard_for(route, destination):
    adapter = route["_adapter"]
    if route["kind"] == "proxy":
        return LeakGuard([adapter["socks"]])
    if route["dns"] in ("system",):
        return None  # standard posture makes no disclosure promise
    if route.get("dialAddress"):
        return LeakGuard([(route["dialAddress"], destination["port"])])
    return LeakGuard(allow_mdns=True, allowed_names=[destination["host"]])


def open_route(route, destination, timeout=30.0, credentials=(None, None)):
    """Open a stream to the destination over ROUTE inside that route's leak guard."""
    guard = guard_for(route, destination)
    adapter = route["_adapter"]

    def dial():
        if route["kind"] == "proxy":
            return socks5_connect(adapter["socks"], destination["host"], destination["port"],
                                  credentials[0], credentials[1], timeout=timeout)
        target = route.get("dialAddress") or destination["host"]
        sock = socket.create_connection((target, destination["port"]), timeout=timeout)
        sock.settimeout(None)
        return sock

    if guard is None:
        return dial(), None
    with guard:
        return dial(), guard


# --- Health, failover, and state -----------------------------------------------

def probe_route(route, destination, attempts=3, timeout=5.0, latency_budget_ms=None):
    samples, failures, reasons, blocked = [], 0, [], 0
    for _ in range(max(1, attempts)):
        started = time.monotonic()
        try:
            sock, guard = open_route(route, destination, timeout=timeout,
                                     credentials=isolation_credentials(route))
            sock.close()
            samples.append(int((time.monotonic() - started) * 1000))
        except LeakBlocked:
            failures += 1
            blocked += 1
            reasons.append("leak_blocked")
        except SocksError as exc:
            failures += 1
            reasons.append(exc.reason)
        except OSError:
            failures += 1
            reasons.append("connect_failed")
    budget = latency_budget_ms or DEFAULT_LATENCY_BUDGET_MS
    if not samples:
        state, reason = "unreachable", reasons[-1] if reasons else "connect_failed"
    elif failures:
        state, reason = "degraded", "intermittent"
    elif statistics.median(samples) > budget:
        state, reason = "degraded", "slow"
    elif route["peerAuthentication"] == "none":
        state, reason = "degraded", "unauthenticated_peer"
    else:
        state, reason = "reachable", None
    return {"adapter": route["adapter"], "kind": route["kind"], "state": state,
            "reason": reason, "attempts": max(1, attempts), "failures": failures,
            "blockedAttempts": blocked,
            "latencyMs": {"median": int(statistics.median(samples)) if samples else None,
                          "max": max(samples) if samples else None}}


def compose_state(posture, route_reports, active_adapter=None, exposures=0):
    """One UI state for a posture from its route reports and observed exposures."""
    if exposures:
        return "exposed", "leak_observed"
    usable = [r for r in route_reports if r["state"] != "unreachable"]
    if not usable:
        return "unreachable", (route_reports[0]["reason"] if route_reports else "no_permitted_route")
    active = next((r for r in route_reports if r["adapter"] == active_adapter), usable[0])
    if route_reports and active is not route_reports[0]:
        return "degraded", "failed_over"
    if active["state"] == "degraded":
        return "degraded", active["reason"]
    if posture == "anonymous":
        return "anonymous", None
    if posture == "standard":
        return "direct", None
    return "protected", None


def health_report(profile, route_reports, active_adapter=None, exposures=(),
                  leak_trace="not_run", extra=None):
    state, reason = compose_state(profile["posture"], route_reports, active_adapter,
                                  len(exposures))
    report = {
        "schema": HEALTH_SCHEMA, "posture": profile["posture"], "state": state,
        "reason": reason, "label": STATE_LABELS[state], "tone": STATE_TONES[state],
        "explanation": STATE_EXPLANATIONS[state], "activeRoute": active_adapter,
        "failovers": 0, "routes": route_reports,
        "exposures": list(exposures),
        "blockedAttempts": sum(r.get("blockedAttempts", 0) for r in route_reports),
        "leakTrace": leak_trace,
    }
    if active_adapter:
        names = [r["adapter"] for r in route_reports]
        report["failovers"] = names.index(active_adapter) if active_adapter in names else 0
    report.update(extra or {})
    return report


def probe(profile, url, attempts=3, timeout=5.0):
    planned = plan(profile, url)
    if not planned["ok"]:
        raise RouteRefused(planned["reason"])
    destination = planned["_destination"]
    reports = [probe_route(route, destination, attempts, timeout,
                           profile["budgets"]["latencyMs"]) for route in planned["routes"]]
    active = next((r["adapter"] for r in reports if r["state"] != "unreachable"), None)
    return health_report(profile, reports, active)


# --- Redaction ----------------------------------------------------------------

_IPV4 = re.compile(r"(?<![\w.])(?:\d{1,3}\.){3}\d{1,3}(?![\w.])")
_IPV6 = re.compile(r"(?<![\w:])\[?(?:[0-9a-fA-F]{0,4}:){2,7}[0-9a-fA-F]{0,4}\]?(?![\w:])")
_URL = re.compile(r"\b(?:https?|git|ssh)://[^\s'\"]+")
_ONION = re.compile(r"\b[a-z2-7]{56}\.onion\b|\b[\w.-]+\.i2p\b", re.I)


class Redactor:
    """Removes destinations, proxies, and addresses from text in redacting postures."""

    def __init__(self, posture, secrets_=()):
        self.active = POSTURE_RULES[posture]["logs"] == "redacted"
        self.secrets = sorted({s for s in secrets_ if s}, key=len, reverse=True)

    def __call__(self, text):
        if not self.active or not text:
            return text
        for secret in self.secrets:
            text = text.replace(secret, "<redacted>")
        for pattern in (_URL, _ONION, _IPV6, _IPV4):
            text = pattern.sub("<redacted>", text)
        return text


# --- Git over a route ----------------------------------------------------------

def audit_repository_config(repo):
    """Local Git configuration that could send traffic around the route."""
    if not repo:
        return []
    completed = subprocess.run(
        ["git", "config", "--local", "--includes", "--name-only", "--list"],
        cwd=repo, capture_output=True, text=True, check=False,
        env={**os.environ, "GIT_CONFIG_NOSYSTEM": "1", "GIT_CONFIG_GLOBAL": os.devnull})
    if completed.returncode != 0:
        return []
    return sorted({name for name in completed.stdout.split() if RISKY_CONFIG.match(name.lower())})


def _relay_wrapper(directory, route_spec):
    """A proxy command Git and OpenSSH can exec: this tool's `relay` with a fixed route."""
    spec = Path(directory) / "route.json"
    spec.write_text(json.dumps(route_spec), encoding="utf-8")
    spec.chmod(0o600)
    tool = Path(__file__).resolve()
    if os.name == "nt":
        wrapper = Path(directory) / "relay.cmd"
        wrapper.write_text('@"%s" "%s" relay --route "%s" %%*\r\n' % (sys.executable, tool, spec))
    else:
        wrapper = Path(directory) / "relay"
        wrapper.write_text("#!/bin/sh\nexec %s %s relay --route %s \"$@\"\n" % (
            shlex.quote(sys.executable), shlex.quote(str(tool)), shlex.quote(str(spec))))
        wrapper.chmod(0o700)
    return str(wrapper)


def git_invocation(profile, route, destination, workdir, credentials=(None, None)):
    """Config and environment that pin Git to ROUTE. Returns (config_args, env)."""
    posture = profile["posture"]
    adapter = route["_adapter"]
    # A route that stalls must fail (and fail over) rather than hang the run.
    config = ["-c", "protocol.ext.allow=never", "-c", "http.lowSpeedLimit=1",
              "-c", "http.lowSpeedTime=60"]
    if posture == "standard":
        env = dict(os.environ)
    else:
        env = {k: v for k, v in os.environ.items() if k in ENV_KEEP}
        env.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull,
                   GIT_TERMINAL_PROMPT="0", GIT_ASKPASS="", SSH_ASKPASS="")
        config += ["-c", "http.followRedirects=false", "-c", "credential.helper="]
    route_spec = {"kind": route["kind"], "hostClass": destination["hostClass"]}
    if route["kind"] == "proxy":
        host, port = adapter["socks"]
        route_spec.update(socks=[host, port], username=credentials[0], password=credentials[1])
        auth = ""
        if credentials[0] is not None:
            auth = "%s:%s@" % (credentials[0], credentials[1])
        literal = "[%s]" % host if ":" in host else host
        config += ["-c", "http.proxy=socks5h://%s%s:%d" % (auth, literal, port)]
    else:
        if posture != "standard":
            env["NO_PROXY"] = env["no_proxy"] = "*"
        route_spec.update(pins=adapter.get("hosts", {}),
                          cidrs=[str(c) for c in adapter.get("cidrs", [])])
        if route.get("dialAddress") and route["dns"] == "pinned":
            config += ["-c", "http.curloptResolve=%s:%d:%s" % (
                destination["host"], destination["port"], route["dialAddress"])]
    if posture != "standard" or route["kind"] == "proxy":
        wrapper = _relay_wrapper(workdir, route_spec)
        env["GIT_PROXY_COMMAND"] = wrapper
        ssh = ["ssh", "-o", "VerifyHostKeyDNS=no", "-o", "CanonicalizeHostname=no",
               "-o", "ProxyCommand=%s %%h %%p" % wrapper]
        if posture in ("private", "anonymous"):
            ssh[1:1] = ["-F", "none"]
        env["GIT_SSH_COMMAND"] = " ".join(shlex.quote(part) for part in ssh)
        env["GIT_SSH_VARIANT"] = "ssh"
    return config, env


_STRACE_LINE = re.compile(
    r"^\d+\s+(connect|sendto|sendmsg|sendmmsg)\((?P<body>.*)$")
_SA_INET = re.compile(r'sa_family=AF_INET, sin_port=htons\((\d+)\), sin_addr=inet_addr\("([^"]+)"\)')
_SA_INET6 = re.compile(
    r'sa_family=AF_INET6, sin6_port=htons\((\d+)\).*?inet_pton\(AF_INET6, "([^"]+)"')
_SA_UNIX = re.compile(r'sa_family=AF_UNIX, sun_path=@?"([^"]+)"')


def analyze_trace(text, allowed_endpoints, dns="none"):
    """Incidents in an strace log: DNS traffic or connections outside the allowed endpoints."""
    allowed = {(str(_literal_ip(h)), int(p)) for h, p in allowed_endpoints}
    incidents = []
    for line in text.splitlines():
        match = _STRACE_LINE.match(line)
        if not match:
            continue
        body = match.group("body")
        unix = _SA_UNIX.search(body)
        if unix:
            if match.group(1) == "connect" and re.search(r"\)\s*=\s*-1\b", body):
                continue  # a refused local socket carried nothing off the device
            path = unix.group(1).lower()
            if any(marker in path for marker in RESOLVER_SOCKETS) and "journal" not in path:
                if dns == "mdns" and "avahi" in path:
                    continue
                if dns != "system":
                    incidents.append({"kind": "dns_query", "via": "resolver_socket"})
            continue
        inet = _SA_INET.search(body) or _SA_INET6.search(body)
        if not inet:
            continue
        endpoint = (str(_literal_ip(inet.group(2))), int(inet.group(1)))
        if endpoint in allowed:
            continue
        if dns == "mdns" and endpoint in MDNS_ENDPOINTS:
            continue
        if endpoint[1] in (53, 853, 5353):
            if dns != "system":
                incidents.append({"kind": "dns_query", "via": "port_%d" % endpoint[1]})
            continue
        if dns == "system":
            continue
        incidents.append({"kind": "direct_connection",
                          "addressClass": classify_host(endpoint[0])})
    return incidents


def strace_available():
    return sys.platform.startswith("linux") and shutil.which("strace") is not None


def run_git(profile, url, git_args, repo=None, trace="auto", timeout=600, stream=None):
    """Run Git over the first reachable permitted route, failing over in preference order."""
    planned = plan(profile, url)
    posture = profile["posture"]
    if not planned["ok"]:
        raise RouteRefused(planned["reason"])
    if "@URL" not in git_args:
        raise UsageError("GIT_ARGS must name the remote as @URL")
    if posture != "standard":
        risky = audit_repository_config(repo)
        if risky:
            raise RouteRefused("repository_config_overrides_route", ", ".join(risky))
    tracing = trace != "off" and posture != "standard"
    if tracing and not strace_available():
        if trace == "required":
            raise UsageError("leak tracing is required but strace is not available")
        tracing = False
    destination = planned["_destination"]
    secrets_ = [url, destination["host"]]
    for route in planned["routes"]:
        if route["kind"] == "proxy":
            secrets_.append("%s:%d" % route["_adapter"]["socks"])
        if route.get("dialAddress"):
            secrets_.append(route["dialAddress"])
    redact = Redactor(posture, secrets_)
    reports, exposures, outcome = [], [], None
    for route in planned["routes"]:
        report = probe_route(route, destination, attempts=1, timeout=10.0,
                             latency_budget_ms=profile["budgets"]["latencyMs"])
        reports.append(report)
        if report["state"] == "unreachable":
            continue
        credentials = isolation_credentials(route)
        redact.secrets = sorted(set(redact.secrets) | {c for c in credentials if c},
                                key=len, reverse=True)
        with tempfile.TemporaryDirectory(prefix="forkmesh-transport-") as workdir:
            config, env = git_invocation(profile, route, destination, workdir, credentials)
            argv = ["git", *config, *[url if a == "@URL" else a for a in git_args]]
            trace_file = Path(workdir) / "trace.log"
            if tracing:
                argv = ["strace", "-f", "-qq", "-e", "trace=connect,sendto,sendmsg,sendmmsg",
                        "-o", str(trace_file), "--", *argv]
            started = time.monotonic()
            try:
                completed = subprocess.run(argv, cwd=repo, env=env, capture_output=True,
                                           timeout=timeout, check=False)
                code, out, err = completed.returncode, completed.stdout, completed.stderr
            except subprocess.TimeoutExpired as exc:
                code, out, err = 124, exc.stdout or b"", (exc.stderr or b"") + b"\ntimed out"
            elapsed = int((time.monotonic() - started) * 1000)
            if tracing:
                allowed = []
                if route["kind"] == "proxy":
                    allowed.append(route["_adapter"]["socks"])
                elif route.get("dialAddress"):
                    allowed.append((route["dialAddress"], destination["port"]))
                exposures = analyze_trace(
                    trace_file.read_text(errors="replace") if trace_file.exists() else "",
                    allowed, dns=route["dns"])
        outcome = {"adapter": route["adapter"], "exitCode": code, "elapsedMs": elapsed,
                   "stdout": redact(out.decode("utf-8", "replace")),
                   "stderr": redact(err.decode("utf-8", "replace"))}
        if stream is not None:
            stream(outcome)
        if exposures or code == 0:
            break
        if not CONNECTION_FAILURE.search(err.decode("utf-8", "replace")):
            break  # Git's own failure (missing repository, rejected push): do not retry elsewhere
        report["state"], report["reason"] = "unreachable", "transfer_failed"
    active = outcome["adapter"] if outcome and outcome["exitCode"] == 0 else None
    leak_trace = "traced" if tracing else ("not_required" if posture == "standard" else "unavailable")
    result = health_report(profile, reports, active, exposures, leak_trace)
    if outcome is not None and outcome["exitCode"] != 0 and not exposures:
        if CONNECTION_FAILURE.search(outcome["stderr"]) or all(
                r["state"] == "unreachable" for r in reports):
            result.update(state="unreachable", label=STATE_LABELS["unreachable"],
                          tone=STATE_TONES["unreachable"],
                          explanation=STATE_EXPLANATIONS["unreachable"], reason="transfer_failed")
        else:
            result["reason"] = "git_failed"
    result["git"] = outcome
    return result


def exit_code_for(report):
    if report["state"] == "exposed":
        return EXIT_EXPOSED
    if report["state"] == "unreachable":
        return EXIT_UNREACHABLE
    git = report.get("git")
    if git is not None and git["exitCode"] != 0:
        return EXIT_GIT
    return EXIT_OK


# --- Relay (proxy command for git:// and ssh://) ---------------------------------

def relay(route_spec, host, port):
    """Carry stdin/stdout to HOST:PORT under the route recorded in ROUTE_SPEC."""
    kind = route_spec["kind"]
    host = host.lower()
    if kind == "proxy":
        endpoint = tuple(route_spec["socks"])
        if classify_host(host) in ("local-address", "loopback-name", "mdns-name", "shared-address"):
            raise RouteRefused("local_destination_via_proxy")
        with LeakGuard([endpoint]):
            sock = socks5_connect(endpoint, host, int(port), route_spec.get("username"),
                                  route_spec.get("password"))
    else:
        target = route_spec.get("pins", {}).get(host, host)
        address = _literal_ip(target)
        if kind == "lan" and classify_host(target) != "local-address":
            raise RouteRefused("destination_outside_lan")
        if kind == "overlay" and (address is None or not any(
                address.version == ipaddress.ip_network(c).version
                and address in ipaddress.ip_network(c) for c in route_spec.get("cidrs", []))):
            raise RouteRefused("destination_outside_overlay")
        if kind == "clearnet" and address is None:
            raise RouteRefused("name_needs_dns")
        with LeakGuard([(str(address), int(port))]):
            sock = socket.create_connection((str(address), int(port)), timeout=30)
        sock.settimeout(None)

    def upstream():
        try:
            while True:
                data = os.read(0, 65536)
                if not data:
                    break
                sock.sendall(data)
        except OSError:
            pass
        try:
            sock.shutdown(socket.SHUT_WR)
        except OSError:
            pass

    threading.Thread(target=upstream, daemon=True).start()
    while True:
        data = sock.recv(65536)
        if not data:
            break
        os.write(1, data)
    sock.close()


# --- Address advertisement and NAT ------------------------------------------------

def advertise(profile, addresses):
    """Which of this device's addresses a posture lets peers learn, and inbound reachability."""
    posture = profile["posture"]
    policy = POSTURE_RULES[posture]["advertise"]
    parsed = [a for a in (_literal_ip(x) for x in addresses) if a is not None]
    overlay_cidrs = [c for a in profile["adapters"] if a["kind"] == "overlay" for c in a["cidrs"]]

    def in_overlay(address):
        return any(address.version == c.version and address in c for c in overlay_cidrs)

    public = [a for a in parsed if a.is_global and not in_overlay(a)]
    local = [a for a in parsed if classify_host(str(a)) == "local-address" and not a.is_loopback]
    overlay = [a for a in parsed if in_overlay(a)]
    shared = [a for a in parsed if a.version == 4 and a in CGNAT and not in_overlay(a)]
    if public:
        nat = "public"
    elif shared:
        nat = "carrier_nat"
    elif local:
        nat = "behind_nat"
    else:
        nat = "no_clearnet_address"
    if policy == "reachable":
        chosen = public + overlay + local
    elif policy == "overlay":
        chosen = overlay
    elif policy == "lan":
        chosen = local
    else:
        chosen = []
    services = [a["serviceAddress"] for a in profile["adapters"]
                if a["kind"] == "proxy" and a.get("serviceAddress")
                and a["network"] in POSTURE_RULES[posture]["proxyNetworks"]]
    withheld = len(parsed) - len(chosen)
    inbound = {"public": "direct", "carrier_nat": "needs_relay_or_overlay",
               "behind_nat": "needs_relay_or_overlay",
               "no_clearnet_address": "needs_relay_or_overlay"}[nat]
    if policy == "none":
        inbound = "onion_service" if services else "outbound_only"
    elif policy == "overlay":
        inbound = "overlay" if overlay else "outbound_only"
    elif policy == "lan":
        inbound = "local_network" if local else "outbound_only"
    return {"schema": ADVERTISEMENT_SCHEMA, "posture": posture,
            "addresses": [str(a) for a in chosen], "serviceAddresses": services,
            "withheld": withheld, "nat": nat, "inbound": inbound}


# --- Telemetry and CLI --------------------------------------------------------------

def append_telemetry(path, profile, event, report):
    """Count-only record; never written in the anonymous posture. Returns what happened."""
    if not path:
        return "not_requested"
    if POSTURE_RULES[profile["posture"]]["telemetry"] == "off":
        return "suppressed"
    record = {
        "schema": TELEMETRY_SCHEMA, "event": event, "posture": profile["posture"],
        "state": report["state"], "reason": report.get("reason"),
        "routesTried": len(report.get("routes", [])), "failovers": report.get("failovers", 0),
        "blockedAttempts": report.get("blockedAttempts", 0),
        "exposures": len(report.get("exposures", [])), "leakTrace": report.get("leakTrace"),
    }
    with open(path, "a", encoding="utf-8") as handle:
        handle.write(json.dumps(record, sort_keys=True) + "\n")
    return "written"


def describe(report):
    lines = ["%s (%s): %s" % (report["label"], report["posture"], report["explanation"])]
    if report.get("reason"):
        lines.append("reason: %s" % report["reason"])
    for route in report.get("routes", []):
        latency = route["latencyMs"]["median"]
        lines.append("  %-12s %-8s %-11s%s%s" % (
            route["adapter"], route["kind"], route["state"],
            " %dms" % latency if latency is not None else "",
            " (%s)" % route["reason"] if route.get("reason") else ""))
    if report.get("exposures"):
        lines.append("exposures: %s" % ", ".join(sorted({e["kind"] for e in report["exposures"]})))
    lines.append("leak trace: %s" % report.get("leakTrace", "not_run"))
    return lines


def _emit(value, as_json, lines):
    if as_json:
        print(json.dumps(value, indent=2, sort_keys=True))
    else:
        for line in lines:
            print(line)


def _write_report(path, report):
    if path:
        tmp = Path(str(path) + ".tmp")
        tmp.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        os.replace(tmp, path)


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)

    validate = commands.add_parser("validate", help="check a transport profile")
    validate.add_argument("profile")
    validate.add_argument("--json", action="store_true")

    planner = commands.add_parser("plan", help="list the routes a posture permits for a URL")
    planner.add_argument("url")
    planner.add_argument("--profile", required=True)
    planner.add_argument("--json", action="store_true")

    prober = commands.add_parser("probe", help="measure every permitted route")
    prober.add_argument("url")
    prober.add_argument("--profile", required=True)
    prober.add_argument("--attempts", type=int, default=3)
    prober.add_argument("--timeout", type=float, default=5.0)
    prober.add_argument("--report")
    prober.add_argument("--telemetry")
    prober.add_argument("--json", action="store_true")

    git = commands.add_parser("git", help="run Git over the posture's routes")
    git.add_argument("url")
    git.add_argument("--profile", required=True)
    git.add_argument("--repo")
    git.add_argument("--trace", choices=("auto", "required", "off"), default="auto")
    git.add_argument("--report")
    git.add_argument("--telemetry")
    git.add_argument("--json", action="store_true")

    status = commands.add_parser("status", help="show a saved health report")
    status.add_argument("report")
    status.add_argument("--json", action="store_true")

    advertiser = commands.add_parser("advertise", help="addresses a posture lets peers learn")
    advertiser.add_argument("--profile", required=True)
    advertiser.add_argument("--address", action="append", default=[])
    advertiser.add_argument("--json", action="store_true")

    relayer = commands.add_parser("relay", help=argparse.SUPPRESS)
    relayer.add_argument("--route", required=True)
    relayer.add_argument("host")
    relayer.add_argument("port", type=int)

    argv = list(sys.argv[1:] if argv is None else argv)
    git_args = []
    if argv[:1] == ["git"] and "--" in argv:
        split = argv.index("--")
        argv, git_args = argv[:split], argv[split + 1:]
    args = parser.parse_args(argv)
    try:
        if args.command == "relay":
            relay(json.loads(Path(args.route).read_text(encoding="utf-8")), args.host, args.port)
            return EXIT_OK
        if args.command == "status":
            try:
                report = json.loads(Path(args.report).read_text(encoding="utf-8"))
            except (OSError, ValueError) as exc:
                raise UsageError("cannot read report: %s" % exc) from exc
            if report.get("schema") != HEALTH_SCHEMA or report.get("state") not in STATES:
                raise UsageError("not a %s report" % HEALTH_SCHEMA)
            _emit(report, args.json, describe(report))
            return exit_code_for(report)
        if args.command == "validate":
            profile = load_profile(args.profile)
            summary = {"ok": True, "posture": profile["posture"],
                       "adapters": [a["id"] for a in profile["adapters"]]}
            _emit(summary, args.json, ["profile is valid: posture %s, adapters %s" % (
                profile["posture"], ", ".join(summary["adapters"]))])
            return EXIT_OK
        profile = load_profile(args.profile)
        if args.command == "plan":
            result = public_plan(plan(profile, args.url))
            lines = ["route %d: %s (%s, dns %s, peer %s)" % (
                i + 1, r["adapter"], r["kind"], r["dns"], r["peerAuthentication"])
                for i, r in enumerate(result["routes"])]
            lines += ["refused %s: %s" % (r["adapter"], r["reason"]) for r in result["refused"]]
            if not result["ok"]:
                lines.append("no route: %s" % result["reason"])
            _emit(result, args.json, lines)
            return EXIT_OK if result["ok"] else EXIT_REFUSED
        if args.command == "advertise":
            result = advertise(profile, args.address)
            _emit(result, args.json, [
                "advertise: %s" % (", ".join(result["addresses"] + result["serviceAddresses"])
                                   or "nothing"),
                "withheld: %d" % result["withheld"],
                "nat: %s; inbound: %s" % (result["nat"], result["inbound"])])
            return EXIT_OK
        if args.command == "probe":
            report = probe(profile, args.url, args.attempts, args.timeout)
        else:
            report = run_git(profile, args.url, git_args, repo=args.repo, trace=args.trace)
            if report.get("git") and not args.json:
                sys.stdout.write(report["git"]["stdout"])
                sys.stderr.write(report["git"]["stderr"])
        report["telemetry"] = append_telemetry(args.telemetry, profile, args.command, report)
        _write_report(args.report, report)
        if args.json or args.command == "probe":
            _emit(report, args.json, describe(report))
        else:
            print("\n".join(describe(report)), file=sys.stderr)
        return exit_code_for(report)
    except RouteRefused as exc:
        profile_posture = locals().get("profile", {}).get("posture", "standard")
        detail = Redactor(profile_posture)(str(exc))
        print("refused: %s%s" % (exc.reason, "" if detail == exc.reason else " (%s)" % detail),
              file=sys.stderr)
        return EXIT_REFUSED
    except UsageError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return EXIT_USAGE


if __name__ == "__main__":
    sys.exit(main())
