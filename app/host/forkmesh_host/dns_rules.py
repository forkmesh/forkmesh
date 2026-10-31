"""DNS rules shared by tools/next_dns.py (operator) and the node reconciler.

Pure standard library, so the operator tool can import it from the repo and
every release carries it inside app/host. The hard guards live here once:

* ``check_name``: a record name must be ``next.forkmesh.com`` or under it;
* ``check_reconciler_name``: the automatic reconciler may only ever touch
  ``app.next.forkmesh.com`` and ``world.next.forkmesh.com`` (the per-node
  ``nN.next`` records are managed by hand with ``next_dns.py ensure``);
* ``plan_set``: never empties a record set, A records only, TTL 60, DNS only
  (proxied false);
* ``MIN_CHANGE_INTERVAL``: at most one change per 5 minutes;
* ``VoterState``: a node votes a target down after 3 consecutive failed
  probes (90 s) and up again after 2 minutes of continuous health.
"""

from __future__ import annotations

import http.client
import ipaddress
import json
import socket
import ssl

ZONE_APEX = "next.forkmesh.com"
RECONCILER_NAMES = frozenset({"app.next.forkmesh.com",
                              "world.next.forkmesh.com"})
TTL = 60
PROXIED = False
FAILS_TO_REMOVE = 3
HEALTHY_TO_READD = 120.0
MIN_CHANGE_INTERVAL = 300.0
PROBE_INTERVAL = 30.0
READY = "ready"
READ_ONLY = "read_only"
DOWN = "down"


class DnsGuardError(ValueError):
    """A DNS change that the hard guards refuse."""


def check_name(name) -> str:
    """The normalized name, or DnsGuardError unless inside next.forkmesh.com."""
    value = str(name or "").strip().lower()
    if value.endswith("."):
        value = value[:-1]
    if value != ZONE_APEX and not value.endswith("." + ZONE_APEX):
        raise DnsGuardError("refusing DNS record %r: outside %s"
                            % (name, ZONE_APEX))
    labels = value.split(".")
    if any(not label or len(label) > 63 for label in labels) or "*" in value:
        raise DnsGuardError("refusing DNS record %r: malformed name" % name)
    return value


def check_reconciler_name(name) -> str:
    value = check_name(name)
    if value not in RECONCILER_NAMES:
        raise DnsGuardError("refusing DNS record %r: the reconciler only "
                            "manages %s" % (name, ", ".join(
                                sorted(RECONCILER_NAMES))))
    return value


def check_ipv4(value) -> str:
    try:
        return str(ipaddress.IPv4Address(str(value)))
    except ValueError:
        raise DnsGuardError("refusing A record content %r" % (value,))


def check_ipv6(value) -> str:
    try:
        address = ipaddress.IPv6Address(str(value))
    except ValueError:
        raise DnsGuardError("refusing AAAA record content %r" % (value,))
    if not address.is_global:
        raise DnsGuardError("refusing AAAA record content %s: not global"
                            % address)
    return str(address)


def check_address(record_type, value) -> str:
    if record_type == "A":
        return check_ipv4(value)
    if record_type == "AAAA":
        return check_ipv6(value)
    raise DnsGuardError("refusing record type %r" % (record_type,))


def plan_set(name, live_records, wanted_ips, record_type="A",
             name_check=check_name):
    """Changes that make the A set of ``name`` exactly ``wanted_ips``.

    ``live_records``: dicts with id/content/ttl/proxied. Returns a list of
    ``(action, ip, record)`` with action create/update/delete; refuses an
    empty wanted set (never empty a record set)."""
    name_check(name)
    wanted = sorted({check_address(record_type, ip) for ip in wanted_ips})
    if not wanted:
        raise DnsGuardError("refusing to empty %s" % name)
    by_ip = {}
    for record in live_records:
        content = record.get("content")
        if record_type == "AAAA":
            try:
                content = str(ipaddress.IPv6Address(str(content)))
            except ValueError:
                pass
        by_ip.setdefault(content, []).append(record)
    changes = []
    for ip in wanted:
        records = by_ip.get(ip) or []
        if not records:
            changes.append(("create", ip, None))
            continue
        record = records[0]
        if bool(record.get("proxied")) != PROXIED or \
                int(record.get("ttl") or 0) != TTL:
            changes.append(("update", ip, record))
        for extra in records[1:]:
            changes.append(("delete", ip, extra))
    for ip, records in by_ip.items():
        if ip not in wanted:
            for record in records:
                changes.append(("delete", ip, record))
    return changes


def record_body(name, ip):
    return {"type": "A", "name": check_name(name), "content": check_ipv4(ip),
            "ttl": TTL, "proxied": PROXIED}


def probe(ip, host, path="/readyz", timeout=5.0, context=None,
          user_agent="fmnext-dns-health/1"):
    """TLS-verified ``GET https://<host><path>`` connected to ``ip`` (like
    ``curl --resolve host:443:ip``): a node serving a stale or self-signed
    certificate, or answering 503 (sealed, no quorum), counts as down."""
    return probe_state(ip, host, path, timeout, context, user_agent) == READY


def probe_state(ip, host, path="/readyz", timeout=5.0, context=None,
                user_agent="fmnext-dns-health/1"):
    context = context or ssl.create_default_context()
    try:
        with socket.create_connection((ip, 443), timeout=timeout) as raw:
            with context.wrap_socket(raw, server_hostname=host) as tls:
                request = ("GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\n"
                           "Connection: close\r\n\r\n"
                           % (path, host, user_agent)).encode()
                tls.sendall(request)
                response = http.client.HTTPResponse(tls)
                response.begin()
                body = response.read(4096)
                if 200 <= response.status < 300:
                    return READY
                return READ_ONLY if response.status == 503 and \
                    read_only_body(body) else DOWN
    except (OSError, ssl.SSLError, http.client.HTTPException):
        return DOWN


def read_only_body(body):
    try:
        data = json.loads(body.decode("utf-8", "replace"))
    except ValueError:
        return False
    return isinstance(data, dict) and data.get("readOnly") is True


def state_of(value):
    if value is True:
        return READY
    if value in (READY, READ_ONLY):
        return value
    return DOWN


def serving(states):
    states = {name: state_of(value) for name, value in states.items()}
    floor = READY if READY in states.values() else READ_ONLY
    return {name: state == READY or state == floor
            for name, state in states.items()}


class VoterState:
    """One voter's hysteresis per target: ``vote`` is True (up), False
    (down) or None (no opinion yet)."""

    def __init__(self):
        self.targets = {}

    def observe(self, target, ok, now):
        entry = self.targets.setdefault(
            target, {"fails": 0, "healthy_since": None, "vote": None})
        if ok:
            entry["fails"] = 0
            if entry["healthy_since"] is None:
                entry["healthy_since"] = now
            if entry["vote"] is None or \
                    now - entry["healthy_since"] >= HEALTHY_TO_READD:
                # First impression counts as up (a fresh voter must not
                # remove anything); after a failure, up needs 2 minutes.
                entry["vote"] = True
        else:
            entry["fails"] += 1
            entry["healthy_since"] = None
            if entry["fails"] >= FAILS_TO_REMOVE:
                entry["vote"] = False
        return entry["vote"]


def majority(votes, live_voters, target):
    """(down, up) booleans: a strict majority of the live voters agree.

    ``votes``: {voter: {target: bool}} (fresh votes only)."""
    live = sorted(set(live_voters))
    need = len(live) // 2 + 1
    down = sum(1 for voter in live
               if (votes.get(voter) or {}).get(target) is False)
    up = sum(1 for voter in live
             if (votes.get(voter) or {}).get(target) is True)
    return bool(live) and down >= need, bool(live) and up >= need


def decide(votes, live_voters, targets, in_dns, now, last_change):
    """At most one ``(action, target)``: remove a target a majority sees
    down, else re-add one a majority sees up. ``in_dns``: {name: set(ip)}.
    Never empties a set; nothing within MIN_CHANGE_INTERVAL of the last
    change."""
    if last_change is not None and now - last_change < MIN_CHANGE_INTERVAL:
        return None
    for target in sorted(targets):
        ip = targets[target]
        down, _ = majority(votes, live_voters, target)
        present = [name for name, ips in in_dns.items() if ip in ips]
        if down and present and all(
                len(in_dns[name] - {ip}) >= 1 for name in present):
            return ("remove", target)
    for target in sorted(targets):
        ip = targets[target]
        _, up = majority(votes, live_voters, target)
        if up and any(ip not in ips for ips in in_dns.values()):
            return ("add", target)
    return None
